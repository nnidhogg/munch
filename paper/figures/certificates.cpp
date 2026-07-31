// Draws the automata behind the re-entrancy condition, using munch's own Graphviz export.
//
// Build and run from the repository root against an existing build tree, then render:
//   c++ -std=c++23 -I libs/core/include -I libs/dfa/include -I libs/dfa/tools/include -I libs/nfa/include \
//       -I libs/regex/include paper/figures/certificates.cpp -o certificates \
//       -L cmake-build-debug/libs/core -L cmake-build-debug/libs/dfa -L cmake-build-debug/libs/nfa \
//       -L cmake-build-debug/libs/regex \
//       -lmunch_core -lmunch_dfa_tools -lmunch_dfa -lmunch_nfa -lmunch_regex
//   ./certificates paper/figures
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

// Reports what the shipped predicate says, so the figure's captions are checked rather than asserted.
void report(const Builder_dbg& builder, const std::string& name, const std::string& bytes)
{
    const auto lexer{builder.build()};

    std::cout << name << ": ";

    for (const auto byte : bytes)
    {
        std::cout << '\'' << byte << "' " << (lexer.is_split_point(byte) ? "certified" : "rejected") << "  ";
    }

    std::cout << '\n';
}

using munch::regex::any_of;
using munch::regex::concat;
using munch::regex::kleene;
using munch::regex::plus;
using munch::regex::Set;
using munch::regex::text;

// a+ and ';'. The only state consuming ';' is the initial one, which nothing re-enters, so ';' is certified.
void write_sound(const std::filesystem::path& dir)
{
    Builder_dbg builder;

    builder.add_token(plus(any_of(Set{'a'})), Token::First, 1);
    builder.add_token(text(";"), Token::Second, 1);

    munch::dfa::tools::Graphviz::to_file(builder.dfa(), dir / "certificate_sound.dot");

    report(builder, "sound    (a+ and ';')", "a;");
}

// a* alone. The initial state accepts and carries a self-loop, so it is the only state consuming 'a' and the
// re-entrancy condition is the only thing that keeps 'a' out of the certificate.
void write_nullable(const std::filesystem::path& dir)
{
    Builder_dbg builder;

    builder.add_token(kleene(any_of(Set{'a'})), Token::First, 1);

    munch::dfa::tools::Graphviz::to_file(builder.dfa(), dir / "certificate_nullable.dot");

    report(builder, "nullable (a*)        ", "a");
}

// (ab)*c. The initial state is re-entered through a cycle rather than a self-loop, which is why the condition is
// stated as an incoming transition and not as a self-loop.
void write_cyclic(const std::filesystem::path& dir)
{
    Builder_dbg builder;

    builder.add_token(concat(kleene(concat(text("a"), text("b"))), text("c")), Token::First, 1);

    munch::dfa::tools::Graphviz::to_file(builder.dfa(), dir / "certificate_cyclic.dot");

    report(builder, "cyclic   ((ab)*c)    ", "abc");
}
} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path dir{argc > 1 ? argv[1] : "."};

    write_sound(dir);
    write_nullable(dir);
    write_cyclic(dir);

    std::cout << "wrote three dot files to " << dir << '\n';

    return 0;
}
