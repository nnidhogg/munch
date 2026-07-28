# CLAUDE.md

Guidance for AI agents working in this repository.

## Project

A C++23 lexing library built as a layered pipeline: `libs/regex` (combinator AST as a variant of plain structs) →
`libs/nfa` (Thompson construction) → `libs/dfa` (subset construction, minimization, compiled Simulator) →
`libs/core` (Builder/Lexer orchestration) → `tools/tokenizer` (driver layer: token stream, modes, hand scanners).
The engine below `tools/` is language-agnostic and byte-oriented; anything language-specific (raw string scanning,
mode switching policy) belongs in the driver layer, never in the engine.

## Build and test

```bash
cmake -S . -B build                  # configure (fetches boost, googletest, mdspan)
cmake --build build -j 8             # build
cd build && ctest --output-on-failure # run all test suites
./build/tools/benchmark/lexer_benchmark [MiB] [passes]  # throughput benchmark, three scenarios
```

Run the full test suite after every change. For performance-relevant changes, run the benchmark before and after and
cite the numbers; report best-of-N, not averages.

GCC 13 has no native `<mdspan>`; `external/mdspan` fetches the Kokkos implementation, which injects `mdspan` into
namespace `std` via `<experimental/mdspan>`.

## Line endings

LF everywhere, enforced by .gitattributes (`* text=auto eol=lf`). No manual normalization is needed; never introduce
CRLF.

## Code conventions

- Clean interfaces and abstractions: one responsibility per class, few member variables. Prefer splitting a class
  (e.g. Dfa = definition, Simulator = execution) over accumulating members or friends.
- Value semantics throughout: no reference or raw-pointer members referencing other objects. Borrow in the
  constructor, own the result. Views (string_view, mdspan) are transient locals or return values, never members —
  the one documented exception is Token::lexeme_. Every class follows the rule of zero.
- Implementation code goes in .cpp files; only templates stay in headers.
- Designated initializers for aggregate construction: `{.kind = ..., .regex = ...}`.
- Class layout: private aliases, constants, and nested types go in the implicit private section at the TOP of the
  class, before `public:`. The explicit `private:` section at the bottom holds functions first, then data members.
  Within a group, order declarations by increasing line length unless dependencies force otherwise; note such
  exceptions in a comment.
- Definitions in a .cpp appear in header declaration order, except private constructors, which are defined with the
  other constructors directly after the public ones.
- Doxygen comments (`@brief`, `@param`, `@return`, `@throws`) on every entity, including private members.
- Naming: `Upper_snake` types, `_t`-suffixed type aliases, `lower_snake` functions, trailing-underscore members.
- Style: Allman braces, 4-space indent, 120 columns, brace initialization. A .clang-format exists (CLion-derived) but
  the binary is not installed — match the surrounding style manually.
- Errors: throw standard exceptions for programming errors; return sum types (Result, std::expected) for expected
  failures. Model exactly the states an operation has — three sibling states beat two nested two-state types.

## Commits

One line only: an imperative subject stating what was done. No body, no numbers, no Co-Authored-By footer.
Example: `Compress transition table columns into symbol equivalence classes`.

## Testing conventions

Each library has its own test executable (`lexer_<lib>_tests`). Regex-layer tests verify through nfa::Simulator;
dfa-layer tests build automata by hand via dfa::Builder and verify through the dfa Simulator; driver tests exercise
the Tokenizer end to end. New behavior needs tests at the layer that owns it, including negative and boundary cases.
