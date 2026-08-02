// Draws the automata behind the re-entrancy condition, using munch's own Graphviz export.
//
// Built as munch_certificates by this directory's CMakeLists.txt, and run under CTest into the build tree, where
// the emitted DOT files are compared byte for byte against the committed ones, so the committed figures cannot
// drift from the automata. To refresh the committed figures after a deliberate change, run it with this directory
// as the argument and re-render:
//   ./build/paper/figures/munch_certificates paper/figures
//   for f in paper/figures/*.dot; do dot -Tpdf "$f" -o "${f%.dot}.pdf"; done

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>

#include "munch/core/builder.hpp"
#include "munch/dfa/tools/graphviz.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace
{
enum class Token : std::size_t
{
    First,
    Second,
};

// The compiled automaton is protected on the builder, exposed here the way the unit tests expose it.
class Builder_dbg : public munch::core::Builder
{
public:
    using Builder::dfa;
};

// Asserts what the shipped predicate says about each candidate byte, so the figure's caption cannot drift from the
// automaton it describes. `expected` carries one character per byte: 'c' for certified, 'r' for rejected.
bool report(const Builder_dbg& builder, const std::string& name, const std::string& bytes, const std::string& expected)
{
    const auto lexer{builder.build()};

    auto agrees{true};

    std::cout << name << ": ";

    for (std::size_t i{0}; i < bytes.size(); ++i)
    {
        const auto certified{lexer.is_split_point(bytes[i])};

        const auto wanted{expected[i] == 'c'};

        std::cout << '\'' << bytes[i] << "' " << (certified ? "certified" : "rejected")
                  << (certified == wanted ? "" : " <- CAPTION SAYS OTHERWISE") << "  ";

        agrees = agrees && certified == wanted;
    }

    std::cout << '\n';

    return agrees;
}

using munch::regex::any_of;
using munch::regex::concat;
using munch::regex::kleene;
using munch::regex::plus;
using munch::regex::Set;
using munch::regex::text;

// a+ and ';'. The only state consuming ';' is the initial one, which nothing re-enters, so ';' is certified.
bool write_sound(const std::filesystem::path& dir)
{
    Builder_dbg builder;

    builder.add_token(plus(any_of(Set{'a'})), Token::First, 1);
    builder.add_token(text(";"), Token::Second, 1);

    munch::dfa::tools::Graphviz::to_file(builder.dfa(), dir / "certificate_sound.dot");

    return report(builder, "sound    (a+ and ';')", "a;", "rc");
}

// a* alone. The initial state accepts and carries a self-loop, so it is the only state consuming 'a' and the
// re-entrancy condition is the only thing that keeps 'a' out of the certificate.
bool write_nullable(const std::filesystem::path& dir)
{
    Builder_dbg builder;

    builder.add_token(kleene(any_of(Set{'a'})), Token::First, 1);

    munch::dfa::tools::Graphviz::to_file(builder.dfa(), dir / "certificate_nullable.dot");

    return report(builder, "nullable (a*)        ", "a", "r");
}

// (ab)*c. The initial state is re-entered through a cycle rather than a self-loop, which is why the condition is
// stated as an incoming transition and not as a self-loop.
bool write_cyclic(const std::filesystem::path& dir)
{
    Builder_dbg builder;

    builder.add_token(concat(kleene(concat(text("a"), text("b"))), text("c")), Token::First, 1);

    munch::dfa::tools::Graphviz::to_file(builder.dfa(), dir / "certificate_cyclic.dot");

    return report(builder, "cyclic   ((ab)*c)    ", "abc", "rrr");
}
} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path dir{argc > 1 ? argv[1] : "."};

    const auto sound{write_sound(dir)};

    const auto nullable{write_nullable(dir)};

    const auto cyclic{write_cyclic(dir)};

    std::cout << "wrote three dot files to " << dir << '\n';

    if (!(sound && nullable && cyclic))
    {
        std::cout << "a predicate verdict disagrees with the caption of Figure 1\n";

        return 1;
    }

    return 0;
}
