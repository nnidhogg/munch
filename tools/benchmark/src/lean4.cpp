#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "munch/core/mode.hpp"
#include "munch/core/mode_builder.hpp"
#include "munch/core/mode_lexer.hpp"
#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/read_antlr.hpp"
#include "munch/tools/benchmark/harness.hpp"

namespace
{
using namespace munch::tools::benchmark;

/**
 * @brief Reads a file whole, byte for byte.
 * @param path The file to read.
 * @return The file's bytes, std::nullopt when it cannot be opened.
 */
std::optional<std::string> read_file(const std::string_view path)
{
    std::ifstream stream{std::string{path}, std::ios::binary};

    if (!stream)
    {
        return std::nullopt;
    }

    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

/**
 * @brief Returns the modes of a scanner by index, INITIAL first and the others in the order the grammar declares them.
 * @param spec The scanner.
 * @return The mode names.
 */
std::vector<std::string> modes_of(const munch::tools::audit::Lexer_spec& spec)
{
    std::vector<std::string> modes{std::string{munch::tools::audit::initial_condition}};

    for (const auto& [name, exclusive] : spec.conditions)
    {
        modes.push_back(name);
    }

    return modes;
}

/**
 * @brief Returns what a rule's ANTLR commands do to the mode stack, read from the action's text, where the reader keeps
 *        `pushMode`, `popMode` and `mode`.
 * @param action The rule's action, `-> skip, pushMode(M)` or the like.
 * @param modes The modes by index, INITIAL first.
 * @return The action; stay where no command changes the mode.
 */
munch::core::Mode_action mode_action(const std::string_view action, const std::vector<std::string>& modes)
{
    const auto target_of{[&modes, action](const std::size_t open) {
        const auto close{action.find(')', open)};

        const auto name{action.substr(open, close - open)};

        const auto mode{name == "DEFAULT_MODE" ? munch::tools::audit::initial_condition : name};

        const auto found{std::ranges::find(modes, mode)};

        const auto index{std::ranges::distance(modes.begin(), found)};

        return static_cast<std::size_t>(index);
    }};

    if (action.contains("popMode"))
    {
        return {.kind = munch::core::Mode_action_kind::pop};
    }

    constexpr std::string_view push{"pushMode("};

    if (const auto at{action.find(push)}; at != std::string_view::npos)
    {
        return {.kind = munch::core::Mode_action_kind::push, .target = target_of(at + push.size())};
    }

    constexpr std::string_view go_to{"mode("};

    if (const auto at{action.find(go_to)}; at != std::string_view::npos)
    {
        return {.kind = munch::core::Mode_action_kind::go_to, .target = target_of(at + go_to.size())};
    }

    return {};
}

/**
 * @brief Compiles a scanner's modes into one modal lexer, every mode's token set as the audit reads it and each rule's
 *        mode commands as its action, so that the scan pushes and pops as ANTLR's does.
 * @param spec The scanner.
 * @return The lexer, mode zero INITIAL.
 */
munch::core::Mode_lexer modal(const munch::tools::audit::Lexer_spec& spec)
{
    const auto modes{modes_of(spec)};

    munch::core::Mode_builder builder{};

    for (std::size_t mode{0}; mode < modes.size(); ++mode)
    {
        const auto set{munch::tools::audit::token_set(spec, modes[mode])};

        for (const auto& [regex, id, priority, discarded] : set.rules)
        {
            const auto action{mode_action(spec.rules[id].action, modes)};

            builder.add_token(mode, regex, id, priority, action);
        }
    }

    return builder.build();
}

} // namespace

/**
 * @brief Measures the bundled Lean 4 grammar's modal scan over a corpus of Lean source.
 *
 * The grammar is read from its ANTLR file as the audit reads it, every mode compiled and every mode command made the
 * token's action, so the rows time the scan whose cuts the audit certifies. The corpus is one file, the concatenation a
 * pinned snapshot yields, and every pass must consume it whole with the mode stack empty at its end; a scan that stops
 * short is reported with the offset and the mode it stopped in. Usage: munch_benchmark_lean4 GRAMMAR CORPUS [passes]
 * @param argc The argument count.
 * @param argv The grammar, the corpus and the passes, the last optional.
 * @return EXIT_SUCCESS when every pass consumed the corpus whole and reproduced the warmup result, EXIT_FAILURE
 *         otherwise.
 */
int main(const int argc, char** argv)
{
    const int passes{argc > 3 ? std::atoi(argv[3]) : default_passes};

    if (argc < 3 || passes <= 0)
    {
        std::printf("usage: munch_benchmark_lean4 GRAMMAR CORPUS [passes > 0]\n");

        return EXIT_FAILURE;
    }

    const auto grammar{read_file(argv[1])};

    const auto corpus{read_file(argv[2])};

    if (!grammar || !corpus)
    {
        std::printf("munch_benchmark_lean4: cannot read %s\n", grammar ? argv[2] : argv[1]);

        return EXIT_FAILURE;
    }

    const auto scanners{munch::tools::audit::read_antlr(*grammar)};

    const auto& spec{scanners.front()};

    const auto lexer{modal(spec)};

    const auto modes{modes_of(spec)};

    munch::core::Mode_stack stack{};

    const auto consumed{lexer.tokenize_all<std::size_t>(*corpus, [](std::size_t, std::size_t, std::size_t) {}, stack)};

    if (consumed != corpus->size() || stack != munch::core::Mode_stack{})
    {
        std::printf(
                "munch_benchmark_lean4: the scan stopped at offset %zu of %zu in mode %s\n", consumed, corpus->size(),
                modes.at(stack.current).c_str());

        return EXIT_FAILURE;
    }

    const auto pass{[&lexer, &corpus] {
        std::size_t tokens{0};

        const auto whole{lexer.tokenize_all<std::size_t>(
                *corpus, [&tokens](std::size_t, std::size_t, std::size_t) { ++tokens; })};

        return whole == corpus->size() ? tokens : 0;
    }};

    print_provenance("Lean 4 grammar over a corpus", passes, std::nullopt);

    std::printf("lean4, %zu bytes in %zu modes, %d passes\n", corpus->size(), modes.size(), passes);

    const auto ok{measure("lexer_all/lean4", corpus->size(), passes, pass)};

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
