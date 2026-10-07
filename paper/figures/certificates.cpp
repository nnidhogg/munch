/*
 * Draws the automata behind the re-entrancy condition, using munch's own Graphviz export.
 *
 * Built as munch_certificates by this directory's CMakeLists.txt, and run under CTest into the build tree, where the
 * emitted DOT files are compared byte for byte against the committed ones, so the committed figures cannot drift from
 * the automata. To refresh the committed figures after a deliberate change, run it with this directory as the argument
 * and re-render:
 *   ./build/paper/figures/munch_certificates paper/figures
 *   for f in paper/figures/certificate_*.dot; do dot -Tpdf "$f" -o "${f%.dot}.pdf"; done
 */

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <iostream>
#include <ranges>
#include <string>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/dfa/tools/graphviz.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace
{
using munch::regex::any_of;
using munch::regex::concat;
using munch::regex::kleene;
using munch::regex::plus;
using munch::regex::Set;
using munch::regex::text;

using figures::Builder_dbg;

/**
 * @brief The figures' token kinds.
 */
enum class Token : std::size_t
{
    /**
     * @brief The first token of a figure.
     */
    first,

    /**
     * @brief The second token of a figure.
     */
    second
};

/**
 * @brief Asserts what the shipped predicate says about each candidate byte, so the figure's caption cannot drift from
 *        the automaton it describes, and prints each byte's verdict.
 * @param builder The figure's grammar.
 * @param name The figure's name, which opens the printed line.
 * @param bytes The candidate bytes.
 * @param expected One verdict per byte, true where the caption says certified.
 * @return True when every verdict matches the caption.
 */
bool check_captions(
        const Builder_dbg& builder, const std::string& name, const std::string& bytes,
        const std::initializer_list<bool> expected)
{
    const auto lexer{builder.build()};

    auto agrees{true};

    std::cout << std::format("{}: ", name);

    for (const auto& [byte, wanted] : std::views::zip(bytes, expected))
    {
        const auto certified{lexer.is_split_point(byte)};

        const auto matches{certified == wanted};

        std::cout << std::format(
                "'{}' {}{}  ", byte, certified ? "certified" : "rejected", matches ? "" : " <- caption says otherwise");

        agrees = agrees && matches;
    }

    std::cout << '\n';

    return agrees;
}

/**
 * @brief Draws a+ and ';'.
 *
 * The only state consuming ';' is the initial one, which nothing re-enters, so ';' is certified.
 * @param dir The directory the DOT file is written to.
 * @return True when the verdicts match the caption.
 */
bool write_sound(const std::filesystem::path& dir)
{
    Builder_dbg builder{};

    builder.add_token(plus(any_of(Set{'a'})), Token::first, 1);

    builder.add_token(text(";"), Token::second, 1);

    munch::dfa::tools::Graphviz::to_file(builder.dfa(), dir / "certificate_sound.dot");

    return check_captions(builder, "sound    (a+ and ';')", "a;", {false, true});
}

/**
 * @brief Draws a* alone.
 *
 * The initial state accepts and carries a self-loop, so it is the only state consuming 'a' and the re-entrancy
 * condition is the only thing that keeps 'a' out of the certificate.
 * @param dir The directory the DOT file is written to.
 * @return True when the verdicts match the caption.
 */
bool write_nullable(const std::filesystem::path& dir)
{
    Builder_dbg builder{};

    builder.add_token(kleene(any_of(Set{'a'})), Token::first, 1);

    munch::dfa::tools::Graphviz::to_file(builder.dfa(), dir / "certificate_nullable.dot");

    return check_captions(builder, "nullable (a*)        ", "a", {false});
}

/**
 * @brief Draws (ab)*c.
 *
 * The initial state is re-entered through a cycle rather than a self-loop, which is why the condition is stated as an
 * incoming transition and not as a self-loop.
 * @param dir The directory the DOT file is written to.
 * @return True when the verdicts match the caption.
 */
bool write_cyclic(const std::filesystem::path& dir)
{
    Builder_dbg builder{};

    builder.add_token(concat(kleene(concat(text("a"), text("b"))), text("c")), Token::first, 1);

    munch::dfa::tools::Graphviz::to_file(builder.dfa(), dir / "certificate_cyclic.dot");

    return check_captions(builder, "cyclic   ((ab)*c)    ", "abc", {false, false, false});
}

} // namespace

/**
 * @brief Writes the three certificate figures and checks their captions.
 * @param argc The argument count.
 * @param argv The directory the DOT files are written to, the current one by default.
 * @return EXIT_SUCCESS when every verdict matches its caption, EXIT_FAILURE otherwise.
 */
int main(const int argc, char** argv)
{
    const std::filesystem::path dir{argc > 1 ? argv[1] : "."};

    const auto sound{write_sound(dir)};

    const auto nullable{write_nullable(dir)};

    const auto cyclic{write_cyclic(dir)};

    std::cout << "wrote three dot files to " << dir << '\n';

    if (!(sound && nullable && cyclic))
    {
        std::cout << "a predicate verdict disagrees with the caption of Figure 1\n";

        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
