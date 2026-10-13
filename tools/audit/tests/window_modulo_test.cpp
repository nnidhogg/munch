#include "munch/tools/audit/window_modulo.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/pattern/parse.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/file_kind.hpp"
#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/read_antlr.hpp"
#include "munch/tools/audit/read_flex.hpp"
#include "munch/tools/audit/read_logos.hpp"
#include "munch/tools/audit/read_re2c.hpp"
#include "munch/tools/audit/report.hpp"
#include "munch/tools/audit/token_set.hpp"
#include "munch/verifier/verifier.hpp"
#include "munch/verifier/verifier_decisions.hpp"

using namespace munch::tools::audit;
using munch::pattern::parse;

namespace
{
/**
 * @brief The pieces a drawn pattern is built from: single bytes, sets of two and four, and a set of every byte but two,
 *        so that the drawn sets hold byte classes of one, two and many bytes.
 */
constexpr std::array atoms{"a", "b", "c", "[ab]", "[cd]", "[a-d]", "[^ab]", R"(\n)"};

/**
 * @brief The operators a drawn piece may carry, none among them.
 */
constexpr std::array suffixes{"", "", "*", "+", "?"};

/**
 * @brief Draws a number below a bound from a sequence.
 * @param sequence The sequence, advanced by the draw.
 * @param below The bound.
 * @return The number.
 */
std::size_t draw(std::mt19937& sequence, const std::size_t below)
{
    std::uniform_int_distribution<std::size_t> distribution{0, below - 1};

    return distribution(sequence);
}

/**
 * @brief Draws a pattern: one to three pieces, each an atom with or without an operator, or a choice of two atoms.
 * @param sequence The sequence the pattern is drawn from.
 * @return The pattern, as pattern::parse() reads it.
 */
std::string random_pattern(std::mt19937& sequence)
{
    std::string pattern{};

    for (auto pieces{draw(sequence, 3) + 1}; pieces > 0; --pieces)
    {
        const std::string_view atom{atoms[draw(sequence, atoms.size())]};

        const std::string_view suffix{suffixes[draw(sequence, suffixes.size())]};

        if (draw(sequence, 4) == 0)
        {
            const std::string_view other{atoms[draw(sequence, atoms.size())]};

            pattern += std::format("({}|{}){}", atom, other, suffix);

            continue;
        }

        pattern += std::format("{}{}", atom, suffix);
    }

    return pattern;
}

/**
 * @brief Draws a token set: two to four rules, each a drawn pattern under its index as id and priority, the first
 *        discarded and each other discarded or kept.
 * @param sequence The sequence the set is drawn from.
 * @return The set.
 */
Token_set random_set(std::mt19937& sequence)
{
    Token_set set{};

    for (std::size_t id{0}, rules{draw(sequence, 3) + 2}; id < rules; ++id)
    {
        const auto pattern{random_pattern(sequence)};

        const auto discarded{id == 0 || draw(sequence, 2) == 0};

        set.rules.push_back({.regex = parse(pattern), .id = id, .priority = id, .discarded = discarded});
    }

    return set;
}

/**
 * @brief Returns the whole of a file.
 * @param path The file's path.
 * @return Its text.
 */
std::string contents(const std::filesystem::path& path)
{
    std::ifstream stream{path, std::ios::binary};

    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

/**
 * @brief Reads the scanners of a grammar file by its kind, as munch-audit does with no option given.
 * @param source The file's text.
 * @return The scanners.
 */
std::vector<Lexer_spec> scanners_of(const std::string_view source)
{
    switch (kind_of(source))
    {
    case Kind::flex:
        return read_flex(source);
    case Kind::antlr:
        return read_antlr(source);
    case Kind::logos:
        return read_logos(source);
    case Kind::re2c:
        break;
    }

    return read_re2c(source);
}

/**
 * @brief Returns the start conditions of a scanner that have rules, INITIAL first and then the declared ones.
 * @param spec The scanner.
 * @return The names.
 */
std::vector<std::string> conditions_of(const Lexer_spec& spec)
{
    std::vector<std::string> names{std::string{initial_condition}};

    for (const auto& [name, exclusive] : spec.conditions)
    {
        if (!std::ranges::contains(names, name))
        {
            names.push_back(name);
        }
    }

    const auto inactive{[&spec](const std::string& name) { return active_rules(spec, name).empty(); }};

    std::erase_if(names, inactive);

    return names;
}

/**
 * @brief Returns the byte a class is shown by, as the windows are: its first graphic member when it has one, else its
 *        lowest.
 * @param members The class, ascending.
 * @return The representative.
 */
unsigned char representative(const std::vector<unsigned char>& members)
{
    const auto graphic{std::ranges::find_if(members, is_graphic)};

    return graphic == members.end() ? members.front() : *graphic;
}

/**
 * @brief Returns the kinded armed run of a compiled set read over one byte of every class: the DFA of its live states
 *        with their tokens and their steps on each class's representative, and the armed run of that DFA.
 * @param lexer The compiled set.
 * @param classes The byte classes.
 * @return The verifier.
 */
munch::verifier::Verifier represented_verifier(
        const munch::core::Lexer& lexer, const std::vector<std::vector<unsigned char>>& classes)
{
    const auto& simulator{lexer.simulator()};

    munch::dfa::Dfa::Transitions_t transitions{};

    munch::dfa::Dfa::Accept_states_t accepting{};

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (!simulator.is_live(state))
        {
            continue;
        }

        if (const auto token{simulator.accepted(state)})
        {
            accepting.emplace(state, *token);
        }

        for (const auto& members : classes)
        {
            const auto byte{representative(members)};

            const auto to{simulator.step(state, byte)};

            if (!to || !simulator.is_live(*to))
            {
                continue;
            }

            const munch::dfa::Dfa::Key_t key{state, munch::dfa::Label{static_cast<char>(byte)}};

            transitions.emplace(key, *to);
        }
    }

    const munch::dfa::Dfa dfa{simulator.init_state(), std::move(transitions), std::move(accepting)};

    return munch::verifier::armed_run(dfa);
}

/**
 * @brief Returns the windows of width two up to the report's limit over class representatives that occur, that the
 *        exact certificate refuses and that modulo_dependence() certifies at some cut, each with its least cut, widths
 *        ascending and each width in the order of its classes.
 * @param report The set's report.
 * @param lexer The compiled set.
 * @param verifier The set's kinded armed run read over the representatives.
 * @return The windows and their cuts.
 */
std::vector<std::pair<std::string, std::size_t>> decided_windows(
        const Report& report, const munch::core::Lexer& lexer, const munch::verifier::Verifier& verifier)
{
    std::vector<std::pair<std::string, std::size_t>> windows{};

    std::vector<std::string> width{std::string{}};

    for (std::size_t length{1}; length <= report.window_limit; ++length)
    {
        std::vector<std::string> longer{};

        for (const auto& prefix : width)
        {
            for (const auto& members : report.classes)
            {
                longer.push_back(prefix + static_cast<char>(representative(members)));
            }
        }

        width = std::move(longer);

        for (const auto& window : width)
        {
            const auto occurrence{munch::verifier::pinned_cut(verifier, window, 0)};

            if (length < 2 || std::holds_alternative<munch::verifier::Absent_window>(occurrence))
            {
                continue;
            }

            const auto holds{[&](const std::size_t cut) {
                return !munch::verifier::modulo_dependence(verifier, window, cut, report.discarded);
            }};

            const auto cuts{std::views::iota(0UZ, window.size())};

            const auto least{std::ranges::find_if(cuts, holds)};

            if (least != cuts.end() && !lexer.is_split_window(window))
            {
                windows.emplace_back(window, *least);
            }
        }
    }

    return windows;
}

/**
 * @brief Returns every cut modulo_dependence() certifies of every window over class representatives up to the report's
 *        limit that occurs, widths ascending, each width in the order of its classes and each window's cuts ascending.
 * @param report The set's report.
 * @param verifier The set's kinded armed run read over the representatives.
 * @return The windows and their cuts.
 */
std::vector<std::pair<std::string, std::size_t>> decided_cuts(
        const Report& report, const munch::verifier::Verifier& verifier)
{
    std::vector<std::pair<std::string, std::size_t>> cuts{};

    std::vector<std::string> width{std::string{}};

    for (std::size_t length{1}; length <= report.window_limit; ++length)
    {
        std::vector<std::string> longer{};

        for (const auto& prefix : width)
        {
            for (const auto& members : report.classes)
            {
                longer.push_back(prefix + static_cast<char>(representative(members)));
            }
        }

        width = std::move(longer);

        for (const auto& window : width)
        {
            const auto occurrence{munch::verifier::pinned_cut(verifier, window, 0)};

            if (std::holds_alternative<munch::verifier::Absent_window>(occurrence))
            {
                continue;
            }

            for (std::size_t cut{0}; cut < window.size(); ++cut)
            {
                if (!munch::verifier::modulo_dependence(verifier, window, cut, report.discarded))
                {
                    cuts.emplace_back(window, cut);
                }
            }
        }
    }

    return cuts;
}

} // namespace

TEST(Window_modulo_test, The_windows_and_the_cuts_of_modulo_cuts_are_the_ones_modulo_dependence_certifies_at_random)
{
    // Sets of two to four drawn rules, the first discarded. The section's windows are the windows over the classes'
    // representatives that occur, that the exact certificate refuses and that modulo_dependence() certifies at some
    // cut, each at its least cut, in the order the section lists them; and modulo_cuts() lists exactly the cuts
    // modulo_dependence() certifies of the windows that occur.
    std::mt19937 sequence{0x5EEDU};

    constexpr std::size_t drawn{50};

    constexpr std::size_t limit{3};

    std::size_t with_windows{0};

    for (std::size_t index{0}; index < drawn; ++index)
    {
        const auto set{random_set(sequence)};

        const auto lexer{compile(set)};

        const auto report{audit(set, limit)};

        const auto found{windows_modulo(report, lexer)};

        const auto verifier{represented_verifier(lexer, report.classes)};

        std::vector<std::pair<std::string, std::size_t>> listed{};

        for (const auto& [window, cut] : found.windows)
        {
            listed.emplace_back(window, cut);
        }

        with_windows += listed.empty() ? 0 : 1;

        std::vector<unsigned char> symbols{};

        std::ranges::transform(report.classes, std::back_inserter(symbols), representative);

        std::vector<std::pair<std::string, std::size_t>> amortized{};

        for (const auto& [window, offset] : munch::verifier::modulo_cuts(verifier, symbols, limit, report.discarded))
        {
            amortized.emplace_back(window, offset);
        }

        EXPECT_TRUE(found.discards) << "token set " << index;
        EXPECT_EQ(listed, decided_windows(report, lexer, verifier)) << "token set " << index;
        EXPECT_EQ(amortized, decided_cuts(report, verifier)) << "token set " << index;
    }

    EXPECT_GT(with_windows, 0U);
}

TEST(Window_modulo_test, Every_byte_of_a_bundled_grammar_certified_modulo_discarded_tokens_holds_its_cut_exactly)
{
    // At width one the window is a byte and its cut the gap before it: wherever is_split_point_ignoring() certifies the
    // byte, at every occurrence at once, modulo_dependence() certifies the one cut, over every condition of every
    // bundled grammar that discards some token.
    const std::filesystem::path root{SOURCE_DIR};

    std::size_t conditions{0};

    std::size_t certified{0};

    for (const auto& entry : std::filesystem::directory_iterator{root / "tools/audit/grammars"})
    {
        if (entry.path().filename() == "README.md")
        {
            continue;
        }

        for (const auto& spec : scanners_of(contents(entry.path())))
        {
            for (const auto& condition : conditions_of(spec))
            {
                const auto set{token_set(spec, condition)};

                const auto lexer{compile(set)};

                const auto report{audit(set, 1)};

                if (report.discarded.empty())
                {
                    continue;
                }

                ++conditions;

                const auto verifier{represented_verifier(lexer, report.classes)};

                for (const auto& members : report.classes)
                {
                    const auto byte{representative(members)};

                    if (!lexer.is_split_point_ignoring(static_cast<char>(byte)))
                    {
                        continue;
                    }

                    ++certified;

                    const std::string window(1, static_cast<char>(byte));

                    const auto found{munch::verifier::modulo_dependence(verifier, window, 0, report.discarded)};

                    EXPECT_FALSE(found) << entry.path().filename() << " " << condition << " " << window;
                }
            }
        }
    }

    EXPECT_EQ(conditions, 18U);
    EXPECT_EQ(certified, 20U);
}
