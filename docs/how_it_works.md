# **How It Works**

The pipeline from combinators to a flat table, the modules that carry it, and where each lives in the tree.
[design.md](design.md) explains why the pipeline is shaped this way and [usage.md](usage.md) how it is driven.

## **The Pipeline**

Building a lexer is a pipeline from combinators down to a flat table, run once by `Builder::build()`:

```
regex combinators ──▶ NFA (Thompson construction)
                          │  per pattern
                          ▼
                    subset construction ──▶ per-pattern DFA ──▶ minimize
                          │
                          ▼  convert back to NFA, union all patterns (ε-transitions)
                    merged NFA
                          │
                          ▼
                    subset construction ──▶ DFA ──▶ minimize ──▶ final DFA
                          │
                          ▼
                    Simulator (flat tables, symbol equivalence classes)
```

1. **Regex → NFA.** Each combinator (`concat`, `choice`, `kleene`, ...) knows how to lower itself to an `nfa::Builder`
   fragment; composing combinators composes NFA fragments.
2. **Per-pattern determinization.** Every registered pattern's NFA is independently turned into a DFA by subset
   construction and minimized. This resolves the non-determinism a single pattern's own combinators introduce (e.g. the
   branching in `choice` or the loop in `kleene`) before patterns ever interact.
3. **Recombination.** Each minimized per-pattern DFA is converted back into an NFA fragment carrying its token, and the
   fragments are united into one NFA under a single fresh start state that ε-links to every fragment. This union is what
   lets multiple tokens share a lexer.
4. **Final determinization.** The merged NFA is determinized and minimized once more. This is the step that resolves
   *cross-pattern* ambiguity, such as shared prefixes between an identifier and a keyword, using each token's priority
   (lower value wins) to pick a winner when several patterns accept the same input.
5. **Compilation to tables.** `core::Lexer` wraps the final DFA in a `dfa::Simulator`, which compiles it into flat
   `(class, state) → state` and `state → token` tables (see [Performance](benchmarks.md#performance)) instead of running
   the DFA against the maps it was built from.

Determinization and minimization run twice, once per pattern and once for the whole lexer, so redundant states are
collapsed at both levels. The refinement keeps states apart when they differ in accepted token or in having a missing
transition, which a lexer needs, so the result can retain more states than a language-minimal DFA would. The first pass
depends only on its own pattern, so adding a token changes nothing in what the other patterns lower to; `build()` runs
both passes each time it is called.

## **Architecture Overview**

| Module                                    | Responsibility                                                                                        |
|-------------------------------------------|-------------------------------------------------------------------------------------------------------|
| `munch::regex`                            | The combinator DSL (`concat`, `choice`, `kleene`, `any_of`, `text`, ...), `parse()`, NFA lowering.    |
| `munch::nfa`                              | `Nfa` / `nfa::Builder`: NFA representation, epsilon closures, Thompson-style append/merge.            |
| `munch::dfa`                              | `Dfa` / `dfa::Builder`; `minimize()`; `Simulator`, and the decisions over it in their own headers.    |
| `munch::core`                             | `Builder`: runs the full pipeline described above; `Lexer`: the public, one-shot matching API.        |
| `munch::tools::tokenizer`                 | `Tokenizer`: resumable cursor over `core::Lexer`. `Mode_tokenizer`: the same, with modes.             |
| `munch::nfa::tools` / `munch::dfa::tools` | `Graphviz`: DOT export for NFAs and DFAs, renders debugging.md's diagrams.                            |
| `munch::common`                           | Shared concepts (`Byte_iterable`, `Random_access_byte_iterable`, `Token_id`, `Token_sink`).           |

## **Directory Structure**

```
docs/                     The documentation, one page per subject as the README's index lists them, the three
                          mirrored technical reports, and the SVG diagrams debugging.md shows.
libs/
  common/                 Shared concepts (Byte_iterable, Random_access_byte_iterable, Token_id, Token_sink).
  regex/                  The combinator DSL: Regex nodes, parse() for flex-style patterns, and their lowering to
                          munch::nfa::Builder.
  nfa/                    NFA representation and builder (Thompson construction, epsilon closure, merge/append).
    tools/                Graphviz DOT export for NFAs.
  dfa/                    DFA representation, minimize() (Moore partition refinement), the table Simulator.
    tools/                Graphviz DOT export for DFAs.
  core/                   Builder (drives the full pipeline) and Lexer (the public matching API).
tools/
  tokenizer/              Tokenizer and Mode_tokenizer: resumable cursors, seek, recovery, raw strings.
  audit/                  munch-audit: flex, re2c, ANTLR and logos files read into token sets, the report, prices.
  benchmark/              Throughput benchmarks: core lexer, tokenizer driver, UTF-8, other engines.
```
