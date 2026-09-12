# **Munch**

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-23-blue.svg" alt="C++23">
  <img src="https://github.com/nnidhogg/munch/actions/workflows/ci.yml/badge.svg" alt="CI">
  <img src="https://github.com/nnidhogg/munch/actions/workflows/codeql.yml/badge.svg" alt="CodeQL">
  <img src="https://codecov.io/gh/nnidhogg/munch/branch/master/graph/badge.svg" alt="Coverage">
  <img src="https://img.shields.io/github/license/nnidhogg/munch" alt="License">
  <img src="https://img.shields.io/github/v/release/nnidhogg/munch?include_prereleases&sort=semver" alt="Release">
  <a href="https://doi.org/10.5281/zenodo.21752996">
    <img src="https://zenodo.org/badge/915800531.svg" alt="DOI">
  </a>
</p>

`munch` is a **modern C++23 library** for building lexical analyzers. It scans serially, and splits a large input
across threads wherever the token set proves that splitting cannot change the token stream, so the parallel scan is
certified rather than speculated and needs no fixup pass. Not every token set proves it, and the library says which
do. Tokens are defined with a small regex-like combinator DSL, compiled through Thompson construction, subset
construction, and DFA minimization by Moore partition refinement, then executed by a cache-optimized table
simulator. There are no predefined tokens or grammars. You describe the language, and the library builds the
automaton.

The same tables answer a few questions about the token set, worked out once and then queried in constant time: which
bytes are safe to split an input at, which short byte windows are safe when no single byte is, where a scan can resume
after an error on a proved boundary rather than a guessed one, and whether two token sets ever cut the same input
differently. The first two make parallel chunking safe without speculation; the last makes a tokenizer change
checkable against every input instead of against a test corpus.

It is also fast. On the [engine comparison](docs/benchmarks.md#comparison-with-other-engines) it measures as the
quickest of the run-time-built lexers there, C++ and Rust alike, on both benchmark corpora, and is the only one of them
with a threaded row at all; compile-time code generation measures ahead of it, and the one generator threaded there
leans on a hand analysis of the single token set it was given. Splitting at certified boundaries scales strongly and
reproduces the serial token stream exactly on completely tokenizable input, with a serial-prefix guarantee on malformed
input. Where a token set certifies nothing, which several conventional grammars do, the library says so and scans
serially rather than speculating.

The name is pronounced /mʌntʃ/, like the English *munch*, after the maximal munch rule every lexer lives by.

## **Features**

- **Fully User-Defined Tokens**

  No built-in keyword or literal set. Every token is a pattern plus a priority you register yourself, so the library
  adapts to any language or custom syntax without forking it.

- **Regex-like Combinators, Not a Regex String**

  Token patterns are built from composable, typed combinator functions (`concat`, `choice`, `plus`, `kleene`,
  `optional`, `exact`, `at_least`, `range`, `any_of`, `text`) instead of a parsed regex string. Patterns are checked by
  the compiler and can be built, stored, and reused as ordinary C++ values. `utf8::range` matches Unicode code point
  ranges by expanding them into byte sequences, so the engine itself stays byte-oriented.

- **A Real Automata Pipeline, Not a Backtracking Matcher**

  Each pattern becomes an NFA (Thompson construction), is determinized (subset construction) and minimized on its own,
  then all patterns are recombined and the whole lexer is determinized and minimized once more. Matching an input is
  always a single deterministic pass with no backtracking.

- **A Simulator Built for Throughput**

  The minimized DFA is compiled into flat transition tables with input symbols grouped into equivalence classes, so
  advancing the automaton on a character is one table read rather than a hash lookup or a graph walk. See
  [docs/benchmarks.md](docs/benchmarks.md).

- **Two Tokenization Layers**

  A low-level `core::Lexer` for single-shot, longest-match tokenization over an iterator range or container, and a
  `tools::tokenizer::Tokenizer` on top of it that streams a whole input into a sequence of tokens with position tracking
  and structured errors, plus a `Mode_tokenizer` for context-dependent languages that holds several lexers as modes over
  one input. Both carry a seek escape hatch for hand-scanned tokens and a scanner for C++ raw string literals; only the
  flat one reaches the parallel path, through `lexer()`.

- **An Auditor for Other Generators' Scanners**

  `munch-audit` reads a flex, re2c, ANTLR 4 or logos file and reports what the library decides about its token set,
  per start condition: the certified bytes and windows, the anchor-free span, which rule blocks each candidate byte, and
  what it would cost to certify one. See [docs/audit.md](docs/audit.md).

- **Graphviz Export for Debugging**

  Any NFA or DFA the library builds can be dumped to Graphviz DOT and rendered to SVG, which is how the diagrams in
  [docs/debugging.md](docs/debugging.md) were produced.

- **Lightweight to Integrate**

  Builds as static libraries by default (`BUILD_SHARED_LIBS` is honored) with `FetchContent`-managed dependencies. Add
  it with `add_subdirectory`, or install it and `find_package(munch)`; either way, link `munch::munch`. The installed
  package is self-contained, with no third-party dependencies in its public headers.

## **Getting Started**

### **Requirements**

- A C++23 compiler; GCC and Clang on Linux are the toolchains built and tested (GCC 13.3 and Clang 19 in CI). Clang 18
  and older cannot compile the tokenizer: libstdc++'s `<expected>` requires `__cpp_concepts >= 202002L`, which Clang
  first reports in 19. GCC 13 has no native `<mdspan>`, which `external/mdspan` (the Kokkos reference implementation)
  supplies via `FetchContent`.
- CMake 3.20.6+.
- python3 at test time only: two probe tests under `tools/probes` run the recovery cross-check scripts with it, and
  `ctest` reports those two as failed without it; the library and every other target build and test without python.
- Everything else (`boost.config`/`describe`/`mp11`/`container_hash`, `mdspan`, `googletest`) is fetched by CMake at
  configure time; there is nothing to install manually. Pass `-DUSE_SYSTEM_BOOST=ON` / `-DUSE_SYSTEM_GTEST=ON` to use
  system packages instead.

### **Building the Project**

```bash
cmake -S . -B build
cmake --build build -j 8
```

Build in `Release` for anything performance-sensitive. The default `CMAKE_BUILD_TYPE` is `Release` when unset, but an
existing `build/` directory keeps whatever type it was first configured with.

## **A First Lexer**

A complete program, from token kinds to a match. The identifier pattern shares its prefix with the two keywords, and the
lower priority value wins where several patterns accept the same input:

```cpp
#include <cstdint>
#include <iostream>
#include <string>

#include <munch/core/builder.hpp>
#include <munch/regex/regex.hpp>

using namespace munch;
using namespace munch::core;
using namespace munch::regex;

int main()
{
    enum class Token_kind : uint8_t
    {
        Boolean,
        Char,
        Identifier,
        Integer_literal,
        Whitespace,
    };

    Builder builder;

    // Define tokens
    builder.add_token(text("boolean"), Token_kind::Boolean, 1);
    builder.add_token(text("char"), Token_kind::Char, 1);

    const auto identifier{concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_')))};
    builder.add_token(identifier, Token_kind::Identifier, 4);

    builder.add_token(plus(any_of(Set::digits())), Token_kind::Integer_literal, 2);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);

    const auto lexer{builder.build()};

    // Tokenize input
    const std::string input = "boolean";
    const auto [token, consumed] = lexer.tokenize<Token_kind>(input);

    std::cout << "Token: " << (token ? std::to_string(static_cast<int>(*token)) : "None") << ", Consumed: "
              << consumed << '\n';

    return 0;
}
```

It prints `Token: 0, Consumed: 7`. Both `text("boolean")` and the identifier pattern match all seven bytes, and the
keyword wins the tie by its lower priority value; the same program with the input `booleans` reports the identifier.

## **Documentation**

The library is documented in `docs/`, one page per subject; the README is the entry.

- [docs/usage.md](docs/usage.md): consuming munch from CMake; token kinds, the combinators and `Set`, the `Builder`; the
  one-shot, batch and parallel entry points of `core::Lexer` with the certificates their planners rest on; the resumable
  `Tokenizer`; error recovery; the mode layer for context-dependent languages.
- [docs/limits.md](docs/limits.md): the library's scope, what every call guarantees, and the escape hatches.
- [docs/how_it_works.md](docs/how_it_works.md): the pipeline from combinators to a flat table, the modules that carry
  it, and where each lives in the tree.
- [docs/benchmarks.md](docs/benchmarks.md): the measured numbers, how the runs are collected and archived, the parallel
  scan at certified bytes and at certified windows, and the comparison with other engines;
  [docs/performance.md](docs/performance.md) explains why a library this small outruns engines orders of magnitude
  larger, and [docs/design.md](docs/design.md) collects the architectural decisions behind it.
- [docs/audit.md](docs/audit.md): `munch-audit`, which reads a flex, re2c, ANTLR 4 or logos file and reports what the
  library decides about its token set, per start condition, with the price of certifying a byte it does not certify.
- [docs/debugging.md](docs/debugging.md): dumping any NFA or DFA to Graphviz DOT, with the rendered automata of a
  keyword alternation, a floating-point literal and a minimization.
- [docs/research.md](docs/research.md): munch as a laboratory bench for studying tokenization, and the probe methodology
  that ships with the code.
- [docs/split_points.md](docs/split_points.md), [docs/split_windows.md](docs/split_windows.md) and
  [docs/panic_mode.md](docs/panic_mode.md): the three technical reports, mirrored word for word from
  [arXiv:2608.03473](https://arxiv.org/abs/2608.03473), [arXiv:2608.09761](https://arxiv.org/abs/2608.09761) and
  [arXiv:2609.10600](https://arxiv.org/abs/2609.10600), whose theorems the certificates, the window planner and the
  recovery layer implement.

## **Testing**

Each automaton and scanner library under `libs/` has a GoogleTest suite in a `tests/` subdirectory, and the probes
under `tools/probes/` are self-checking executables; everything is registered with CTest, while the fuzz harnesses run
as their own CI job instead. To run the registered tests:

```bash
cd build
ctest --output-on-failure
```

Tests, the benchmark tool, and warnings-as-errors are enabled by default only when munch is the top-level project; a
build consuming munch through `add_subdirectory` gets none of them unless it opts in with `-DMUNCH_BUILD_TESTS=ON`,
`-DMUNCH_BUILD_BENCHMARK=ON`, or `-DMUNCH_WERROR=ON`.

Beyond the per-layer unit tests, the `dfa` and `core` suites include fixed-seed property tests: random DFAs check the
compiled simulator against the definition maps and minimization against the original language, and random pattern sets
check the whole pipeline against direct NFA simulation.

## **Versioning and Stability**

munch follows semantic versioning. The stable surface is what this README and [docs/usage.md](docs/usage.md) document:
the regex combinators with `Set`, `utf8::range`, `utf8::ranges`, and the `unicode` XID classes, `core::Builder` with
`add_token()`, `build()`, `diagnose()`, `set_state_limit()`, and `set_ignored_tokens()`, `core::determinize()`,
`core::Lexer` with `Match`, `tokenize()`, `tokenize_all()`, `is_split_point()`, `is_split_point_ignoring()`,
`chunk_boundaries()`, and `tokenize_all_parallel()`, and the `tools::tokenizer` layer. Breaking any of it bumps the
major version; additions arrive in minor versions. The window layer, `is_split_window()` and
`chunk_boundaries_with_windows()`, joined that surface in 1.4.0; the release the split-windows report evaluates, v1.3.3,
deliberately ships no window-planning API.

The recovery layer joined that surface in 1.6.0: `next_certified_start()`, `next_certified_evidence()`,
`next_anchored_start()`, `minimal_repair()`, `lag()`, and `rescue_free()`, each under the contract its own documentation
states, evidence-order answers under preserved evidence and complete-repair invariance, the guarantee described under
[Error Recovery](docs/usage.md#error-recovery) that every completely tokenizable repair of the text before the evidence
places a token boundary at the answer.

The mode layer joined that surface in 1.3.0: `core::Mode_builder`, `core::Mode_lexer`, `core::Mode_stack`,
`Mode_action` with its four kinds, the `Tokenizer` constructors taking a `Mode_lexer`, and `depth()`. So did
`Builder::set_token_payload()` and the three-argument `tokenize_all()` sink that delivers what it attaches. A sink
accepting both arities is called with two.

The supported platform is 64-bit Linux with GCC 13 or Clang 19 and newer, which is exactly what CI builds, tests,
sanitizes, and fuzzes, on x86-64 and ARM64 so both signednesses of plain `char` are exercised. Other platforms, 32-bit
ones included, may work but carry no promise; macOS specifically is known not to build, because Apple's libc++ ships
no `std::jthread` on any Xcode through 26 and the parallel scan keeps `jthread`, so that door opens when Apple ships
P0660. Semantic versioning covers source compatibility only. munch builds as static libraries by default and honours
`BUILD_SHARED_LIBS`; either way the result is meant to be compiled by the consumer, so no ABI stability is promised
between any two versions.

The automata layers underneath (`munch::nfa`, `munch::dfa`) remain public for inspection, debugging, property testing,
and Graphviz export, but they exist to serve the pipeline and may evolve in minor releases: depend on them for tooling,
not for stability. The regex node types are likewise inspectable, but constructing them directly rather than through the
combinators is outside the stable surface. The benchmark tools and the prose in `docs/` carry no compatibility promise,
and performance numbers are measurements, not contracts. Additions like the Unicode XID identifier classes arrive as new
combinators without changing what exists (see [docs/limits.md](docs/limits.md)).

## **License**

The source code is licensed under the terms of the MIT License. See the [LICENSE](LICENSE) file for details. The
generated Unicode identifier tables derive from the Unicode Character Database and are used under the Unicode License
v3; the complete notice is in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), installed alongside the package.

The technical reports and their figures, `paper/split-points/split-points.tex`, `paper/split-windows/split-windows.tex`,
`paper/panic-mode/panic-mode.tex` and `paper/figures/*.pdf`, are licensed under [Creative Commons Attribution 4.0
International](https://creativecommons.org/licenses/by/4.0/) (CC BY 4.0), matching the licence they carry on arXiv as
[arXiv:2608.03473](https://arxiv.org/abs/2608.03473), [arXiv:2608.09761](https://arxiv.org/abs/2608.09761) and
[arXiv:2609.10600](https://arxiv.org/abs/2609.10600). The benchmark archives under `paper/data/` are released under [CC0
1.0](https://creativecommons.org/publicdomain/zero/1.0/): they are measurements rather than authorship, and attribution
on a throughput table serves no one. The programs that generate the figures are source code and are MIT like the rest of
the tree.

## **Author**

Developed and maintained by **Nicklas Nidhögg**, [nnidhogg](https://github.com/nnidhogg) on GitHub.
