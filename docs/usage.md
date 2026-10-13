# **Usage Overview**

How a lexer is defined and driven: token kinds, the combinators and `Set`, the `Builder`, the one-shot, batch and
parallel entry points of `core::Lexer` with the certificates their planners rest on, the resumable `Tokenizer`, error
recovery, and the mode layer for context-dependent languages. The [README](../README.md) shows one complete program;
[limits.md](limits.md) states what every call guarantees.

## **Integrating with CMake**

munch can be consumed two ways. As a subdirectory, vendored or fetched:

```cmake
cmake_minimum_required(VERSION 3.20.6)
project(MyProject VERSION 1.0 LANGUAGES CXX)

# Add the munch library
add_subdirectory(munch)

# Link the munch library to your target
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE munch::munch)
```

Or as an installed package:

```bash
cmake -S munch -B munch/build -DCMAKE_BUILD_TYPE=Release -DMUNCH_BUILD_TESTS=OFF -DMUNCH_BUILD_BENCHMARK=OFF
cmake --build munch/build -j 8
cmake --install munch/build --prefix /your/prefix
```

```cmake
find_package(munch 3.0 CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE munch::munch)
```

The installed package is self-contained: the build-time header-only dependencies (Boost utilities, mdspan) never appear
in munch's public headers, so nothing else needs to be installed or found, and the exported targets carry the C++23
requirement themselves. Do not install from a `MUNCH_BENCHMARK_COMPARE` build, whose comparison engines insist on
installing alongside. Install rules are generated only when munch is the top-level project, or when `MUNCH_INSTALL=ON`
is passed explicitly.

The `munch::munch` target is an interface umbrella over `munch_core`, `munch_pattern`, `munch_tokenizer` and
`munch_verifier`, which pull in `munch_regex`, `munch_nfa`, `munch_dfa`, and `munch_common` transitively. The Graphviz
debugging helpers of [docs/debugging.md](debugging.md) are not part of the umbrella target; link `munch_nfa_tools`
and/or `munch_dfa_tools` (installed as `munch::munch_nfa_tools` / `munch::munch_dfa_tools`) directly to use them.

## **Defining Token Kinds**

Token kinds are defined as an `enum` or as an integer type directly. Each token kind corresponds to a specific token:

```cpp
enum class Token_kind : uint8_t
{
    // Keywords
    Boolean,
    Char,
    String,
    Int8,
    Uint8,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Int64,
    Uint64,

    // Identifier
    Identifier,

    // Literals
    Integer_literal,
    String_literal,
    Wide_string_literal,
    Character_literal,
    Wide_character_literal,
    Fixed_point_literal,
    Floating_point_literal,

    // Comments
    Single_line_comment,
    Multi_line_comment,
};
```

## **Defining Tokens**

### **1. Combinator Functions**

Token patterns are built using a small, composable DSL inspired by regular expressions. Each combinator produces a
*pattern object* that can be freely combined with other patterns and later registered with the `Builder`.

Patterns are ordinary value objects and can be reused across multiple token definitions; copies are deep, so prefer
moving large generated patterns such as the XID classes.

#### **Primitive Combinators**

- `text("abc")`: Matches the exact character sequence `"abc"`.
- `any_of(set)`: Matches any single character contained in the provided character set.

#### **Structural Combinators**

- `concat(p1, p2, ...)`: Matches patterns sequentially from left to right.
- `choice(p1, p2, ...)`: Matches any of the provided alternatives. The alternatives form one automaton, so there is no
  ordering or preference among them; the lexer always takes the longest match overall.

#### **Repetition Combinators**

- `plus(p)`: Matches one or more repetitions of `p`.
- `kleene(p)`: Matches zero or more repetitions of `p`.
- `optional(p)`: Matches zero or one occurrence of `p`.
- `exact(p, count)`: Matches exactly `count` repetitions of `p`.
- `at_least(p, min)`: Matches `min` or more repetitions of `p`.
- `range(p, min, max)`: Matches between `min` and `max` repetitions of `p`.

#### **Character Sets**

`any_of` takes a `regex::Set`, built from an initializer list, an explicit range, or one of the predefined character
classes: `Set::digits()`, `Set::alpha()`, `Set::alphanum()`, `Set::printable()`, `Set::escape()`, `Set::newline()`,
`Set::whitespace()`, `Set::all()`, or `Set::range(start, end)`. Sets combine with `+`/`+=` (union) and `-`/`-=`
(difference), including against single characters.

For Unicode input, `utf8::range(first, last)` from `munch/regex/utf8.hpp` matches one code point from an inclusive
range, expanded into its UTF-8 byte sequences. Surrogates are excluded, and ill-formed input such as overlong encodings
is rejected by construction. `utf8::ranges()` matches one code point from a sorted list of disjoint ranges, and
`munch/regex/unicode.hpp` provides `unicode::xid_start()` and `unicode::xid_continue()`, the UAX #31 identifier
properties generated from the Unicode Character Database (`unicode::version()` names the pinned version). A C-style
identifier is then `concat(choice(text('_'), unicode::xid_start()), kleene(unicode::xid_continue()))`; underscore
already holds XID_Continue, so only the head needs the profile choice. The properties themselves are matched exactly.

#### **Example**

```cpp
using namespace munch::regex;

// Identifier: [A-Za-z_][A-Za-z0-9_]*
const auto identifier = concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_')));
```

#### **Pattern Syntax**

The same nodes can be read from a pattern written the way a lexer generator takes it. `parse()` reads flex's dialect
of POSIX extended regular expressions over bytes: alternation, grouping, `*`, `+`, `?` and counted `{n,m}`, the dot
for any byte but the newline, bracket expressions with ranges, negation, the POSIX classes and their `[:^digit:]`
negations, flex's class operators `[a-z]{-}[aeiou]` and `[a-z]{+}[0-9]` between two brackets, escapes, double-quoted
literals, and `{name}` expanding to a definition given alongside, parenthesised as flex parenthesises it. One escape
flex has not got serves the readers of character-level generators: `\u{X...}` names a code point, the UTF-8 encoding
of that scalar in a literal, and inside a bracket it turns the bracket to scalars, so `[\u{0}-\u{10FFFF}]` is any
scalar and `[^\n\u{e9}]` every scalar but two. What a token language cannot say is refused with the offset and the
reason rather than approximated: the anchors, trailing context and a start-condition prefix, which only a `<`
opening the pattern is. A parsed pattern and its hand-built equivalent compile to the same automaton.

```cpp
#include <munch/pattern/parse.hpp>

using namespace munch::pattern;

const Definitions_t definitions{{"DIGIT", "[0-9]"}};

const auto identifier{parse("[a-zA-Z_][a-zA-Z0-9_]*")};
const auto number{parse("{DIGIT}+(\\.{DIGIT}+)?", definitions)};
const auto latin{parse("[a-z\\u{c0}-\\u{ff}]")};    // one scalar, encoded: q, or é as its two bytes

parse("^abc");    // throws Syntax_error at offset 0: an anchor conditions the context, not the match
```

### **2. Builder Methods**

The `munch::core::Builder` is responsible for collecting token definitions and producing a deterministic lexer. It
represents the *construction phase* of the lexer pipeline described in [How It Works](how_it_works.md).

Once `build()` is called, the resulting `Lexer` is immutable and safe to reuse across multiple inputs.

#### **add_token (pattern, kind, priority)**

Registers a token definition composed of:

- `pattern`: a regex-like combinator expression,
- `kind`: a user-defined token kind (typically an enum),
- `priority`: an integer used to resolve ambiguities.

```cpp
builder.add_token(pattern, Token_kind::Identifier, 4);
```

#### **Priority Semantics**

- Matching is always longest-match first; priorities never shorten or reorder a match.
- When several token patterns accept the same longest match, the token with the *lowest* priority value is selected.
- Priority resolution is deterministic and performed during the final DFA construction, once all patterns share a single
  automaton.

This mechanism allows keyword tokens to override more general patterns such as identifiers.

#### **`build()`**

```cpp
const auto lexer{builder.build()};
```

Finalizes the builder and constructs a `munch::core::Lexer`, running the pipeline in [How It Works](how_it_works.md):
per-pattern NFA construction and determinization, recombination via Thompson union, and a final subset construction and
minimization producing the DFA the returned `Lexer` simulates.

After calling `build()`:

- the builder should be treated as immutable,
- the returned lexer can be reused safely and efficiently,
- lexers already built are unaffected by later changes to the builder.

#### **`diagnose()`**

```cpp
const auto diagnostics{builder.diagnose()};
```

Certifies the health of the registered grammar from the merged automaton, without building a lexer. Two findings are
reported, both as registered token values:

- `dead_tokens`: tokens that never win any input. The classic cause is a keyword registered at a worse priority than the
  identifier pattern, which then owns the keyword's own spelling; every input still tokenizes, so nothing else ever
  reveals the mistake.
- `equal_priority_ties`: pairs of distinct tokens that accept the same input at the same priority. The build resolves
  such ties deterministically but arbitrarily, by the lower registered value, so a tie usually marks a priority the
  grammar author never actually decided.

Like `is_split_point()`, these are properties certified from the automaton rather than heuristics: a token reported dead
is provably dead for every input there is.

#### **Example**

```cpp
using namespace munch;
using namespace munch::core;
using namespace munch::regex;

Builder builder;

// Register keyword tokens
builder.add_token(text("boolean"), Token_kind::Boolean, 1);
builder.add_token(text("char"), Token_kind::Char, 1);

// Create and register identifier and literal tokens
const auto identifier{concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_')))};
const auto integer_literal{plus(any_of(Set::digits()))};

builder.add_token(identifier, Token_kind::Identifier, 4);
builder.add_token(integer_literal, Token_kind::Integer_literal, 2);

// Build the lexer
const auto lexer{builder.build()};
```

- **Token Patterns**: Patterns can represent fixed strings (e.g., keywords) or complex regex-like expressions (e.g.
  identifiers, literals).
- **Priority**: When several patterns accept the same longest match, the lowest priority number wins, ensuring the
  correct token is selected for overlapping patterns such as keywords and identifiers.

## **Tokenization**

The library provides two complementary ways to tokenize. The difference is not convenience: `core::Lexer` already
scans a whole input, in one thread or in parallel. The difference is who controls the reading position.

1. **Stateless matching** via `munch::core::Lexer`, one-shot, batch, or parallel
2. **A resumable cursor** via `munch::tools::tokenizer::Tokenizer`, for a driver that stops, looks, moves, and resumes

### **1. Core API (`munch::core::Lexer`)**

The core lexer performs direct tokenization on containers or iterators. It returns a `Match` holding the recognized
token kind and the number of characters consumed.

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

You can pass a standard container such as `std::array` or `std::string`, or any common range whose elements read as
bytes through const iteration, integral or `std::byte`.

```cpp
std::array<char, 5> input = {'1', '2', '3', '4', '\0'};
const auto [token, consumed] = lexer.tokenize<Token_kind>(input);

// token  -> Token_kind::Integer_literal
// consumed -> 4
```

Alternatively, you can tokenize a string using iterators:

```cpp
std::string input = "boolean";
const auto [token, consumed] = lexer.tokenize<Token_kind>(input.begin(), input.end());

// token  -> Token_kind::Boolean
// consumed -> 7
```

In both cases, the lexer returns:

- the **token kind** (`std::optional<Token_kind>`), which is empty if no valid token was matched, and
- the **length**, the number of characters consumed during the match.

This API is efficient and lightweight, suitable for use in parsers or compiler front ends.

To tokenize a whole input at once, `tokenize_all` scans in a single call and invokes a sink per consumed token, paying
the per-call overhead once; it is the fastest way to tokenize a complete input. It requires random access to input of
byte elements, integral or `std::byte`, so a `std::string` or a `std::vector` of bytes qualifies, and returns the number
of characters tokenized, so a result short of the input's size names the offset where the scan stopped: no token matched
there, a zero-width token did, or the sink returned false:

```cpp
std::vector<std::pair<Token_kind, std::size_t>> tokens;

const auto consumed = lexer.tokenize_all<Token_kind>(input, [&tokens](const Token_kind kind, const std::size_t length) {
    tokens.emplace_back(kind, length);
});

// consumed == input.size() exactly when the whole input tokenized.
```

`is_split_point(symbol)` reports whether a symbol is a certified safe chunk boundary: for input that tokenizes
completely, splitting immediately before it produces the identical token stream. The property is computed from the
compiled transition table, so it reflects the actual token set rather than a heuristic; a newline-run token, for
example, correctly disqualifies newline, where a split-at-newline rule would silently corrupt the token stream.
`chunk_boundaries(input, chunks)` turns the certified points into a chunk plan, and `tokenize_all_parallel<T>(input,
chunks, sink)` scans the chunks concurrently, one thread per chunk, reaching 92.6-95.3% parallel efficiency at eight
threads on a restricted CPU set, over a 512 MiB dense corpus that does not fit in cache, and 3.46-3.94× the serial
throughput on four, both across two benchmark revisions on one machine; see [docs/performance.md](performance.md);
for input that tokenizes completely, the token stream is guaranteed identical to the serial scan's; the result counts as
a successful tokenization only when every returned per-chunk consumed length equals its chunk's size, and a short entry
names the offset within its chunk where that scan stopped. A token set that certifies no split points degenerates to one
chunk and the serial scan. The sink receives `(chunk, token, length)` and runs concurrently across chunks; see
[docs/limits.md](limits.md) for the contract and [docs/performance.md](performance.md) for the measurements.

Before planning, `anchor_free_span()` says how bad the plan can get: the longest run of positions a tokenizable
input can carry with no certified byte among them, exactly, or `std::nullopt` when such runs are unbounded. It is the
gap `chunk_boundaries()` may be asked to span, so it is what decides whether a chunk count is achievable on every
input or only on the inputs a corpus happened to hold. Unbounded is the common answer for a conventional grammar,
since no certificate anchors a position inside a token and an identifier or a run of blanks has no longest form, and
it is not a failure, only the statement that no fixed chunk count is guaranteed:

```cpp
builder.add_token(concat(text("a"), text("b"), text("c")), Token::Abc, 1);

const auto lexer{builder.build()};

lexer.is_split_point('a');   // true: only the initial state consumes it
lexer.is_split_point('b');   // false: consumed mid-token, after an a
lexer.anchor_free_span();    // 2: the interior of one token, and never longer
```

The same question can be asked of a window inventory, for a token set that certifies windows where it certifies no
byte. Each entry pairs a window `is_split_window()` certifies with the origin it reports, and an anchor is then any
position such a window's origin lands on. A window is only known to have landed once the bytes after its origin have
been read, so the decision holds positions back until every window over them is settled:

```cpp
builder.add_token(concat(text("aa"), text("b")), Token::Aab, 1);

const auto lexer{builder.build()};

lexer.is_split_point('a');                  // false: consumed mid-token as well as first
lexer.is_split_point('b');                  // false: consumed only after aa
lexer.anchor_free_span();                   // std::nullopt: no byte anchors anything
lexer.is_split_window("aab");               // 0: at its own start
lexer.is_split_window("ba");                // 1: a b only ever ends a token

const std::vector<dfa::Certified_pair> inventory{{.window = "aab", .origin = 0}, {.window = "ba", .origin = 1}};

lexer.anchor_free_span(inventory);          // 2: the interior again, now reached through windows
```

A window certificate is conditional on occurrence: it promises where the covering token begins in every completely
tokenizable input containing the window, and a window no such input contains satisfies it vacuously, so
`is_split_window()` may certify a window that anchors nothing. `window_occurrence(window)` splits the two readings. It
decides exactly whether some nonempty completely tokenizable input contains the window, by the same boundary-guessing
search as `rescue()` and `boundary_difference()` with a window matcher beside the scan, and a positive answer comes
with a shortest such input; a certified window with a witness is an occurring certificate, one an exhaustive search
finds no witness for is a vacuous one, and the cap is a ceiling as for the other two searches, so an answer under it is
always one the cap paid for and a search it stops settles nothing. The empty input, which contains the empty window
alone, is no input a cut could fall in and is never a witness, so the empty window has a shortest token as its
witness, or none under a token set with no positive-width token:

```cpp
builder.add_token(text("0"), Token::Zero, 1);
builder.add_token(text("00"), Token::Pair, 1);
builder.add_token(text("01"), Token::One, 1);

const auto lexer{builder.build()};

lexer.is_split_window("1001");              // 2: certified, and vacuously so
lexer.window_occurrence("1001");            // {"", true}: no completely tokenizable input contains it
lexer.window_occurrence("001");             // {"0001", true}: 00 then 01
```

`shortest_split_window()` turns the question around: rather than deciding a window it is given, it finds a shortest
window `is_split_window()` certifies, with its origin, or proves that the model certifies none of any length. It follows
one origin from where a token may begin, beside the states every other hypothesis occupies, so it never tries
candidates, and a shortest window can be far longer than the state count: on a family of 69 states it is 2,531 bytes,
and over that family it grows faster than any polynomial in the states. The search holds at most (n + 2) 2^(n - 1) nodes
over n live states, a shortest window is shorter than that, and whether the question is hard is open, so the search
takes a budget of nodes and reports running out as such. A grammar whose shortest window is longer than
`chunk_boundaries_with_windows()` tries, four bytes, gains nothing from that planner, and this is how to tell.

`window_counterexample(window, origin)` holds the certificate itself against every completely tokenizable input. A
counterexample is such an input holding an occurrence of the window whose covering token begins elsewhere than the
origin, and the decision is exact: it searches for one by the same boundary-guessing search as `window_occurrence()`,
with one bit beside the matcher for whether the latest token start sits at the origin, and returns the shortest
counterexample or, from an exhaustive search, the proof that none exists. A window `is_split_window()` certifies has no
counterexample at the origin it reports, and a window it refuses is refused relative to its conservative model, which
this call settles either way; a window `window_occurrence()` places in no input has no counterexample at any origin, its
certificate being vacuous. The cap is the same ceiling, and the empty window and an origin outside the window are
refused as no certificate at all:

```cpp
builder.add_token(text("a"), Token::Identifier, 1);
builder.add_token(text("ab"), Token::Keyword, 1);
builder.add_token(text("b"), Token::Operator, 1);

const auto lexer{builder.build()};

lexer.is_split_window("ab");                     // std::nullopt: the model refuses it
lexer.window_counterexample("ab", 0);            // {"", true}: no input fails it, the certificate is exact
lexer.window_counterexample("ab", 1);            // {"ab", true}: ab itself, one token beginning at offset 0
```

`boundary_counterexample(window, gap)` asks for the weaker guarantee. Gap `g` of a window sits before its byte `g`, and
gap `|W|` right after its final byte; the gap is a boundary at an occurrence when a token begins there or the input ends
there. The decision asks whether the gap is a boundary at every occurrence of the window in every completely tokenizable
input, whatever covers the final byte, and a worker can cut at such a gap and rescan the window's suffix. A certificate
places a token start, so a window `window_counterexample()` proves exact at an origin has a boundary at that gap of
every occurrence, while such a boundary can exist where no covering origin is fixed. The decision is the same search
with the bit recording whether the gap is a boundary, and it returns the shortest input holding an occurrence a token
crosses at the gap or, from an exhaustive search, the proof that none exists. `crossing_counterexample(window, gap)`
decides the other side, whether a token crosses the gap at every occurrence, and returns the shortest input holding an
occurrence cut there. The two sides together are the certificate: `(W, o)` holds exactly when gap `o` is a boundary at
every occurrence and every later gap inside the window is crossed at every occurrence. The gap after the window adds
what no offset inside it can: over `{ab}` the window `b` is crossed at gap 0 and cut at gap 1 at every occurrence.
`boundary_profile(window)` decides every gap both ways. Absence is a verdict on the window, proved by
`window_occurrence()` under the cap, which runs first, or by any gap whose two searches both exhaust without a witness,
since then no input holds an occurrence crossed at the gap and none holds one cut there, and it is given at every gap or
at none. The two proofs can come at different caps, since the occurrence search keeps every branch that has read the
window through until its token closes while a gap search drops the through branches that keep its claim, so a gap can
prove absence under a cap that stops the occurrence search. `must`, `never` and `may` are given only for a window some
search has shown to occur: two searches per gap, each under the cap on its own, give each gap `must`, `never` or `may`,
each with a witness showing the window occurring, or `undetermined` where the cap stopped a search without such proof.
Must and never are monotone, a gap keeping its verdict in every extension of the window at the shifted gap unless the
extension occurs nowhere, while may carries nothing. The cap, the empty window and a gap past the window's end are
handled as for `window_counterexample()`:

```cpp
builder.add_token(text("0"), Token::Zero, 1);
builder.add_token(text("1"), Token::One, 1);
builder.add_token(text("x"), Token::Mark, 1);
builder.add_token(text("001x"), Token::Low, 1);
builder.add_token(text("011x"), Token::High, 1);

const auto lexer{builder.build()};

lexer.window_counterexample("0011", 1);     // {"0011", true}: 0|0|1|1 covers the final byte from offset 3
lexer.window_counterexample("0011", 3);     // {"0011x", true}: 0|011x covers it from offset 1
lexer.boundary_counterexample("0011", 1);   // {"", true}: a token begins there at every occurrence
lexer.boundary_counterexample("0011", 2);   // {"0011x", true}: gap 2 falls inside 011x
lexer.crossing_counterexample("0011", 4);   // {"0011", true}: the input ends right after the window
lexer.boundary_profile("0011");             // must at gaps 0 and 1, may at 2, 3 and 4
lexer.boundary_profile("0011")[4].verdict;  // Gap::may: 0011x crosses gap 4 and 0011 ends there
```

`boundary_difference(other)` asks the question a tokenizer change asks: is there any input both token sets tokenize
that they cut into different tokens? It answers from the two compiled tables rather than from a corpus, so a negative
covers every input, and a positive comes with one that shows it:

```cpp
const auto difference{old_lexer.boundary_difference(new_lexer)};

if (!difference.exhaustive) { /* the search hit its cap: undetermined, not identical */ }
else if (difference.witness.empty()) { /* proved: the two cut every shared input alike */ }
else { /* difference.witness is an input they cut differently */ }
```

`segmentation_difference(other)` asks the whole question: are the two token sets the same segmentation function, the
same inputs tokenized completely and every one of them cut alike? A token set's segmentation function is the set of
marked runs its scan accepts, an input's bytes each with or without a boundary after it, one marking per input of the
domain, so two sets are the same function exactly when those languages are equal, and the decision is that language
equality: the same boundary-guessing search as `boundary_difference()` with one guessed marking fed to both scans at
once, the witness the bytes of the shortest marked run only one side accepts. The half the witness falls in is a fact
about it, read off the two scans: a domain witness one set tokenizes completely and the other does not, a boundary
witness both do and cut apart, which makes it a `boundary_difference()` witness too, while an exhaustive negative here
is one there as well. The two routes need not agree on the witness: over `{a}` against `{aa}` the shortest marked run
only one side accepts is `a`, the domain witness, where `boundary_difference()` returns `aa`, one token against two.
The cap is the same ceiling:

```cpp
const auto [witness, half, exhaustive]{old_lexer.segmentation_difference(new_lexer)};

if (!exhaustive) { /* the search hit its cap: undetermined, not the same function */ }
else if (witness.empty()) { /* proved: the same inputs, cut the same way, over every input */ }
else if (half == dfa::Separation_half::domain) { /* one set tokenizes the witness completely, the other does not */ }
else { /* both tokenize the witness and cut it differently */ }
```

These six decisions and `rescue()` under Error Recovery are declared together in `munch/dfa/boundary_search.hpp`, seven
instances of one search over guessed token boundaries, with `boundary_profile()` beside the two it asks at every gap,
and each is forwarded by `core::Lexer`.

The searches above decide questions about one token set's scan. `munch/verifier/verifier.hpp` states the object they are
questions about, so that the same decisions can be made about any segmentation a finite automaton recognizes.
`verifier::Verifier` is a deterministic, trim automaton over marked symbols, a byte together with the bit saying whether
a token boundary follows it, and it accepts exactly the marked strings of a policy: an input with its boundaries, one
marking per input the policy segments. Every position, length, origin, cut and context the decisions take or return
counts marked symbols, so it counts bytes over a verifier of bytes such as the armed run, codepoints over a table whose
symbols each stand for a class of codepoints, one symbol per codepoint, and UTF-8 bytes over the character overloads of
`merge_tower()`, `token_automaton()` and `ripple_radius()`. `verifier::armed_run(dfa)` builds the verifier of
maximal-munch scanning over a token set's DFA, `verifier::merge_tower(merges)` of `munch/verifier/merge_tower.hpp` the
verifier of canonical byte-pair encoding over a merge table, `verifier::trie_chain(initial, continuation)` of
`munch/verifier/trie_chain.hpp` the verifier of WordPiece segmentation over a two-mode vocabulary, and the constructor
builds one from any transition table, trimming it to the states reachable from the start that can reach acceptance. The
twelve decisions of `munch/verifier/verifier_decisions.hpp` then run on the product of the verifier with a search or on
its minimal automaton, each with a witness replayable through `Verifier::step()` but `fanout()`, which returns states.
`miscovering(verifier, window, origin)` decides the window certificate `(window, origin)`, that the token containing an
occurrence's final byte begins at the origin at every occurrence in every accepted marked string, and returns the
shortest accepted marked string that miscovers it; on the armed run this is the exact side of `is_split_window()`, and
the online decision is conservative against it, refusing 17 of 53,074 window origin pairs on `twitter.json` under the
JSON grammar that the verifier certifies, each confirmed by exact search. `boundary_gap(verifier)` decides whether the
distance between consecutive boundaries is bounded, and returns the supremum or, when it is not, a stem, a loop and a
suffix whose pumping grows the gap. `realizable(verifier)` decides whether generation restricted to
completion-preserving steps never strands, and returns each state's admitted steps. `divergence(a, b)` decides whether
two functional verifiers accept the same marked strings, and returns the shortest input they diverge on in the first
half that has one, the boundaries searched before the domain, with the half it falls in; over two armed runs it is
`segmentation_difference()` decided on the verifiers. The verifier is a library-level interface: `core::Lexer` forwards
none of it, and a caller reaches a token set's DFA through `dfa::Builder` or by deriving from `core::Builder`, whose
`dfa()` is protected.

`pinned_cut(verifier, window, byte)` asks the window certificate of any byte of the window rather than of its last:
whether the token covering the byte begins at one offset of the window at every occurrence in every accepted marked
string. It returns that offset, at which a token boundary then sits at every occurrence, or, as a `Miscovering`, the
shortest accepted marked string with an occurrence where the token begins elsewhere. The offset is read at the window's
first occurrence in the shortest accepted marked string that holds one, which is the window itself under its own
segmentation when the window alone is accepted, and a window that occurs in no accepted marked string is answered
`Absent_window`, since no token covers its byte. `miscovering()` is the case of the last byte with the offset given, and
both searches run over its product. A pinned byte places a cut, though not in general the window certificate of the
window at that offset, and a cut at offset a pins byte a, so the windows that pin some byte and the windows with a
boundary at one offset at every occurrence are one window language. The byte asked need not be the last: over the merges
(a, b) and (a, ab) no origin certifies the last byte of a run of a's, which ends in a token a alone and in aab before a
b, while every window of four bytes pins its byte 2, as the pinned-byte theorem of the report *The Left Ripple of
Byte-Pair Encoding* says of every window of width R + Q_s + 1 under a table of finite origin radius R and right start
radius Q_s.

`dependence(verifier, window, origin)` asks a second question of a pair the verifier certifies, and refuses one it does
not: the certificate makes the cut at the origin a boundary of the whole string's segmentation, and independence says
that each side of the cut is segmented alone as the whole string segments it, the prefix before the cut and the suffix
from it each in the domain under the whole string's marking restricted to them, so that a scan may stop at the cut or
begin there. The decision returns the shortest accepted marked string with an occurrence whose cut is not independent,
with the occurrence, the cut and the side or sides that fail, by a search over a product that carries the verifier's
state and the window's progress before the cut and a continuing and a restarted copy of the verifier after it. On an
armed run every certified cut is independent, which is the splitting theorem of maximal munch; the decision separates
the certificate from the split for a verifier built from a caller's table, where a scan that carries state across a
boundary, the mode of a two-mode scan or the length of the input, can certify a cut whose suffix alone leaves the
domain.

`modulo_dependence(verifier, window, offset, discarded)` asks what a cut needs once the tokens a parser never sees are
deleted, at any offset of any window, certified or not: at every occurrence of the window in every accepted marked
string, the prefix before the cut at the offset and the suffix from it each lie in the domain, and the kept segments of
their segmentations, those whose kind is not discarded, are the string's kept segments with the same kinds at the same
positions. The cut need not be a boundary, since where it falls inside a discarded segment the chunks may segment the
bytes around it otherwise. The kinds are what a verifier records beside its marking: `Verifier::close_kind(state, byte)`
is the kind of the segment a marked step closes and `Verifier::final_kind(state)` the kind of the last segment at an
accepting state, which `armed_run()` reads off the token the unarmed run's DFA state accepts, so that a kind is a token
ID, and the constructor takes them as a `Verifier::Kinds` beside a caller's table, which otherwise records none. The
decision returns the shortest accepted marked string with an occurrence whose cut is not independent, with the
occurrence, the cut, the side or sides outside the domain and, where both lie in it, the kept tokens of the string and
of the chunks as `Kept_token`s, by two searches: a product of the verifier with the set of states it can be in under any
marks, which says whether each side lies in the domain, and a product of two copies of the verifier, one on the string's
marking and one on the chunks', compared as their kept segments close. Positions are compared, as Theorem 3 of
[docs/split_points.md](split_points.md) delivers them for a certified byte, because comparing kinds and lengths alone
certifies a cut at which a kept token changes its text: with ab and b discarded and aab|bba kept, the window aab cut at
0 occurs in bbaab, which reads bba then ab, while its chunks read b, b and aab. With nothing discarded the cut must be a
boundary at every occurrence, and a boundary cut of an armed run is independent, so on an armed run with nothing
discarded the decision certifies a window that occurs exactly where `pinned_cut()` places the byte after the cut at the
offset, and a window that occurs nowhere vacuously, where `pinned_cut()` returns `Absent_window`. A boundary cut of
another verifier need not be independent: over the domain of the empty string and ab, segmented a|b, `pinned_cut()`
places the window b at 0 and the decision refutes the cut before it, since neither a nor b lies in the domain. For one
byte at offset zero the decision certifies wherever `is_split_point_ignoring()` does.
`modulo_cuts(verifier, alphabet, width, discarded)` is the same decision explored once per verifier rather than once per
pair: it lists every cut that holds of every window up to the width that occurs in some accepted marked string, the
windows written over the alphabet, which holds every byte the verifier reads, by carrying each window's clouds of the
two products' nodes, and leaves the witness of a refused pair to `modulo_dependence()`; munch-audit's `--windows-modulo`
calls it.

`chunk_dependence(verifier, inventory)` asks the question parallel scanning needs of an inventory of certified pairs,
and refuses an inventory with a pair the verifier does not certify: cutting every accepted marked string at every
anchor, the position a window's origin lands on at every occurrence of every window of the inventory, each chunk between
consecutive anchors, the first from position zero and the last to the end, is accepted under the whole string's marking
restricted to it, so that each chunk may be scanned alone. A middle chunk has an anchor at both ends, so this is not
`dependence()` taken over the pairs. Cut independence of every pair implies chunk independence, a middle chunk being the
suffix side of a certified cut within the prefix side of the next, and the converse fails, since an inventory is
chunk-independent when its strings are cut finely enough that no chunk shows what a whole side would, by another pair's
anchors or by a window's overlapping occurrences. Over the two-mode scan with initial tokens a, ab and b and
continuation tokens a and b the pair (aa, 1) is certified and not cut-independent, the suffix ab of aab reading as one
token alone, while the inventory {(aa, 1), (aab, 2)} is chunk-independent, the second pair anchoring the b of every aab.
Over initial tokens a, aa and ba with continuation a the inventory of the one pair (aaa, 2) is chunk-independent while
the pair is not cut-independent, its occurrences anchoring every later position of a run of a's. The decision returns
the shortest accepted marked string with a chunk that fails, with its anchors and the failing chunks as half-open
ranges, by a search over a product of a continuing copy of the verifier reading the string and a restarted copy reading
the current chunk, fed with a lag of the longest window's length so that every occurrence anchoring a position has
completed when its byte reaches the copy. On an armed run every inventory is chunk-independent, since every certified
cut is independent.

`right_context(verifier, window, origin)` asks how far past a certified cut a worker must read, and refuses a pair the
verifier does not certify. A worker scanning the chunk that ends at the cut reads a right context of bytes past it, its
input ending there or at the string's end, and keeps the tokens before the cut; it reproduces the whole string's
segmentation of the chunk when its input is accepted under the whole string's marks below the cut, with a boundary at
the cut when it reads past it, and none after its last byte, and a right context fails when at some occurrence of the
window whose cut lies after position zero it does not. The decision returns the right context, the least from which
every larger one suffices, with every context that fails and the least that suffices alone, and an inventory's right
context is the largest of its pairs'. This is the prefix side of `dependence()` with the input read further,
`dependence()` asking it with no byte past the cut, together with the suffix side. A context that suffices need not make
a larger one suffice: under maximal munch over aa and b the cut of (ba, 1) is independent, so a worker reading up to it
alone segments the bytes below it, while one whose input ends inside a token aa fails, as it does one byte past the cut
and at every context from three, and the right context is unbounded. Over the threshold splice of a and b below length
three and ab, a and b from it, the pair (a, 0) needs one byte: aba is segmented ab|a, while its prefix ab, shorter than
three bytes, is segmented a|b alone. A finite right context comes as a `Finite_context`, the right context and the least
context that suffices alone with a truncation at every context that fails, the shortest marked string with its
occurrence, its cut and the context. An unbounded one comes as an `Unbounded_context`, since the failing contexts repeat
with a period from a threshold on: the threshold, the least period, the least context that suffices alone when one does,
a truncation at every context below the threshold that fails, and for every residue modulo the period that fails from
the threshold a truncation lasso, a stem, a loop and a suffix whose cut fails at a context that grows by a multiple of
the period with every repetition of the loop. It searches a product that carries the verifier's state and the window's
progress before the cut and, after it, the continuing state, the window's remaining bytes and the set of states a
worker's copy can be in under any marks, and the failing contexts are the lengths of its paths from the cut to a
failure, bounded exactly when no cycle lies on one and read off layer by layer until a layer repeats. The set makes the
product exponential in the verifier's states in the worst case, and the period can be superpolynomial in the product's
size.

`fanout(verifier, window, origin)` asks what a worker beginning at the cut of any pair, certified or not, must know: the
configurations the cut at the origin can carry over every occurrence of the window in every accepted marked string whose
cut lies after position zero, a configuration being the state of the verifier's minimal automaton reached on the
string's marked prefix, which is all a scan resumed at the cut needs. A worker that begins at the cut runs one copy of
the scan per configuration, and the configuration the scan before it ends in selects the copy, so the rank, the number
of configurations, is the fan-out the cut costs, known before any input is read. The decision is two subset runs over
the byte projection of the minimal automaton, forward over the window's first origin bytes from every state and backward
over the rest, the forward set at origin zero being the states a byte enters, and it returns the rank with each
configuration as the least verifier state of its class. Rank one is not certification: under maximal munch over a, b and
aa the certified pair (b, 0) has rank 2, the cut before a b following either a lone a, with aa still armed, or a token
with nothing armed. Over aa and a no window is certified, a run of a's being cut by its parity, and every cut before an
a has rank 2, inside a token aa or between tokens, so two copies split the input at every byte.

`smallest_fanout(verifier)` returns the smallest fan-out of a pumpable window, one that occurs any number of times in
some accepted marked strings and so places cuts that recur: the degree, the fewest fixed points of a nonzero idempotent
among the relations nonempty byte strings induce on the states of the minimal automaton, with the window (ww, |w|) of
that rank for w the shortest such string, and a stem, a loop reading w and a suffix whose pumping repeats the window.
Every pumpable window has rank at least the degree, so no cut that recurs costs fewer copies, while a window that occurs
a bounded number of times can cost fewer, as (ab, 1) does under the initial token a and the continuation token bb, rank
one after the initial token where the degree is two. Over aa and a the degree is 2, attained by aaaa at origin 2. The
search runs over the monoid of those relations and is exponential in the states of the minimal automaton in the worst
case, and a verifier whose domain is finite is refused with `std::invalid_argument`, since no window recurs.

```cpp
#include "munch/verifier/verifier.hpp"
#include "munch/verifier/verifier_decisions.hpp"

// over the tokens a, ab and b of the example above, with dfa their DFA
const auto verifier{verifier::armed_run(dfa)};               // the maximal-munch scan over the token set's DFA

verifier::miscovering(verifier, "ab", 0);                    // std::nullopt: exact, where is_split_window() refuses
verifier::miscovering(verifier, "ab", 1);                    // the marked ab itself, its token beginning at offset 0
verifier::boundary_gap(verifier);                            // 2, the longest token, since no token grows forever
verifier::divergence(verifier, verifier::armed_run(other));  // std::nullopt when the two scans segment alike
verifier::dependence(verifier, "ab", 0);                     // std::nullopt: every certified cut of an armed run splits
verifier::modulo_dependence(verifier, "ab", 1, {});          // the marked ab itself, the cut inside its token

const std::vector<verifier::Certified_pair> pairs{{.window = "ab", .origin = 0}, {.window = "a", .origin = 0}};

verifier::chunk_dependence(verifier, pairs);    // std::nullopt: cut at every anchor, every chunk scans alone
verifier::right_context(verifier, "ab", 0);     // finite, 0: a worker reads past the cut as far as it likes
verifier::fanout(verifier, "ab", 0);            // rank 2: a lone a before the cut leaves ab armed, a b does not
verifier::smallest_fanout(verifier);            // degree 1, the window aa at origin 1
```

`verifier::k_copy_scan(verifier, input, request)` of `munch/verifier/k_copy_scan.hpp` turns the fan-out into a chunked
scanner. The input is cut at anchors of the request's inventory, the positions after position zero that a pair's origin
lands on at an occurrence of its window, sought as `chunk_boundaries()` seeks its cuts, and each chunk is scanned on its
own thread, the last on the calling thread, by a worker that runs one copy of the scan per configuration its cut can
carry, those `fanout()` gives every pair anchoring the cut. Each copy reads its chunk under every marking the minimal
automaton admits and offers one marking per configuration it can end in at the next cut; the stitch begins at the start,
lets each chunk's exit select the next chunk's copy, keeps every exit a selected copy offers with a back-pointer, and at
the input's end follows the back-pointers from the one accepting exit. For every inventory, certified or not, and with
no independence condition, the stitched segmentation is guaranteed identical to the sequential one, by the k-copy chunk
theorem the header states with its proof, and an input outside the domain gives `std::nullopt`; each chunk reports the
copies its worker ran and the cut at which the stitch fixed its marking. Selection is pure, every chunk fixed at its own
end, at a cut that carries one configuration, and under a policy of bounded delay once each worker reads that many bytes
past its chunk, `Scan_request::lookahead`: maximal munch over a token set whose longest token has l bytes, and two-mode
WordPiece likewise, need l - 1, and canonical byte-pair encoding over a table of byte merges, `merge_tower(merges)`'s,
at most Q_s + 1, which is at most max(1, S) for S the lengths of its merges' left parts summed, whatever its origin
radius. Over characters, `merge_tower(merges, alphabet)`'s, the lookahead counts bytes and the bound does not hold:
under the one merge (a, U+1F600) over a, U+1F600 and U+1F601, S is 1 and the boundary after a waits on the fourth byte
past it. Without that lookahead a chunk may resolve only at a later cut or at the input's end, which costs latency and
not correctness; under backward maximal munch over a and aa, with no lookahead, a run of a's resolves only at its end. A
window of the inventory that is empty or whose origin lies outside it is refused with `std::invalid_argument`.

Two decisions of the same header help choose the inventory. `shortest_fanout_windows(verifier)` returns, for every rank
bound from the least rank of a window that occurs to the largest, the shortest pair of rank at most the bound, the least
by length, then bytes, then origin, decided over two finite families of state sets with no bound on the window's length;
the least rank can lie below the degree, by a window that occurs a bounded number of times. `relative_fanout(verifier,
window, origin)` returns the classes of a pair's configurations that the window's remaining bytes do not tell apart,
configurations of one class giving equal copies on every chunk that holds those bytes, so the relative rank is the
fewest copies such a chunk needs; under maximal munch over a, b and aa the certified pair (b, 0) has rank 2 and relative
rank 1.

```cpp
#include "munch/verifier/k_copy_scan.hpp"

// over the tokens aa and a, with dfa their DFA
const auto verifier{verifier::armed_run(dfa)};
const std::array inventory{verifier::Fanout_pair{.window = "a", .origin = 0}};

verifier::shortest_fanout_windows(verifier);  // one window, a at origin 0 of rank 2, at bound 2
verifier::relative_fanout(verifier, "a", 0);   // relative rank 2: the next a ends a token aa or begins one

const verifier::Scan_request request{.chunks = 5, .inventory = inventory, .lookahead = 0};
const auto scan{verifier::k_copy_scan(verifier, "aaaaa", request)};

scan->segmentation;  // aa|aa|a, as the sequential scan segments it
scan->chunks[1];     // [1, 2), two copies, resolved at its own end
```

`verifier::merge_tower(merges)` takes a byte-pair merge table in rank order, each merge two parts, a byte or the product
of an earlier merge, whose concatenation is its product, and builds the verifier of canonical byte-pair encoding over
it, the encoder that applies the lowest-ranked merge present anywhere at its leftmost occurrence until none applies; the
verifier accepts every byte string under exactly the encoder's segmentation of it. Each merge is one stage over the
stage below, a one-symbol-lookahead transducer that holds a symbol while it equals the merge's left part, joins it with
an arriving right part into the product and otherwise passes what it holds and what arrives upward, so a state of the
verifier is the symbol each stage holds with the boundary claimed after it, together with whether the top stage has
emitted nothing, a token with a boundary claimed after it, or a token claimed to be the last. A step refuses a boundary
claimed inside a product or after a token claimed to be the last, and a held symbol whose merge no possible next arrival
completes is passed upward at once, which keeps the states few. The products must be distinct from the bytes and from
one another, since the encoder merges a recreated token again where a stage above cannot: the table (c,c), (c,cc),
(ccc,a), (cc,c) encodes `ccca` as one token, and `merge_tower()` refuses it with `std::invalid_argument` naming the
merge. The decisions then run on the tower as on any verifier, `miscovering()` deciding a window certificate under the
table, `pinned_cut()` the cut a byte of a window pins, `boundary_gap()` returning the length of the longest token the
encoder emits, `realizable()` the mask of every state, and `divergence()` over two towers deciding whether two tables
segment every byte string alike, with the shortest string they segment apart:

```cpp
#include "munch/verifier/merge_tower.hpp"
#include "munch/verifier/verifier_decisions.hpp"

const std::vector<verifier::Merge> first{{.left = "a", .right = "b"}};
const std::vector<verifier::Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "a"}};

const auto tower{verifier::merge_tower(merges)};            // ab, then ab followed by a

verifier::miscovering(tower, "ab", 0);                      // std::nullopt: every ab lies in a token beginning at its a
verifier::miscovering(tower, "ab", 1);                      // the marked ab itself, its token beginning at offset 0
verifier::pinned_cut(tower, "ab", 1);                       // 0: the token covering the b of every ab begins at its a
verifier::boundary_gap(tower);                              // 3, the longest token aba
verifier::divergence(tower, verifier::merge_tower(first));  // aba, one token here and ab|a under the first merge alone
```

`verifier::merge_tower(merges, alphabet)` builds the same tower over an alphabet of characters in place of the bytes,
for a character-level table such as SentencePiece's, whose merges join Unicode characters. The alphabet is UTF-8 text
whose characters are the tokens before any merge, each part of a merge is one of them or the product of an earlier
merge, and every spelling is UTF-8. The verifier reads marked bytes, as the byte tower does, and a state also holds the
bytes of the character under way, the character reaching the lowest stage with the boundary bit of its last byte, and a
step is refused when it claims a boundary inside a character or reads a byte that continues no character of the
alphabet, so the verifier accepts exactly the strings over the alphabet, each under the encoder's segmentation of its
characters. No character's encoding is a prefix of another's and each begins with a byte that no other place of an
encoding holds, so a window of characters occurs only at character boundaries, and every decision on the tower is exact
over those strings, with positions and lengths in bytes as everywhere else. Over the characters U+0000 to U+007F the
tower is the byte tower of the same table restricted to those bytes. A character the alphabet holds twice is refused
with `std::invalid_argument` naming it, as is a merge that names a character outside the alphabet:

```cpp
const std::vector<verifier::Merge> merges{
        {.left = "▁", .right = "t"}, {.left = "h", .right = "e"}, {.left = "▁t", .right = "he"}};

const auto tower{verifier::merge_tower(merges, "▁theé")};  // the characters ▁, t, h, e and é, ▁ three bytes long

verifier::miscovering(tower, "▁t", 0);  // std::nullopt: every ▁t lies in a token beginning at its ▁
verifier::miscovering(tower, "he", 0);  // the marked ▁the, one token, its he four bytes in
verifier::miscovering(tower, "é", 1);   // the marked é, whose token never begins inside the character
verifier::boundary_gap(tower);          // 6, the bytes of the longest token ▁the
```

`verifier::token_automaton(merges)` of `munch/verifier/token_automaton.hpp` builds the token automaton of canonical
byte-pair encoding over a merge table, and `verifier::miscovering(automaton, window, origin)` decides on it the same
window certificate under the same encoder as `miscovering()` on `merge_tower(merges)`, without building the tower. It
rests on the canonical adjacency theorem, Theorem 2 of the report *The Left Ripple of Byte-Pair Encoding*: a list of
tokens is the encoder's segmentation of its spelling exactly when every token is the segmentation of its own spelling
and so is every adjacent pair. A run of consecutive tokens of a segmentation is then the segmentation of its own
spelling, so a miscovering, when there is one, is spelled by the tokens over one occurrence of the window with nothing
on either side, and it is at most 2L + |window| - 2 bytes long, L the longest token. The decision searches those runs: a
token holding the whole window anywhere but at its start when the origin is zero, or a first token whose tail spells the
start of the window, then tokens inside the window, then a last token that contains the window's final byte and begins
anywhere but at the origin. Which token may follow which is read off the token automaton of Berglund, Martens and van
der Merwe (Theoretical Computer Science 2026, by Theorem 1, Corollary 2 and Algorithm 2 of its preprint
arXiv:2405.07671v2), in which a token enters the same state wherever it is read, so a node of the search is that state
with the number of the window's bytes spelled so far, each node is expanded once, and the search stops at the first
miscovering. The automaton has one base state plus one per merge whose product can be read, and on a large table
building it costs far more than a decision on it, so a caller deciding many windows builds it once; the search then
reads every token against the window and explores at most |window| - 1 nodes per state. `Token_automaton::state_count()`
counts the states, and `allowed_tokens(state)` lists the tokens a state may read, ascending, those
`is_allowed(state, token)` admits. The miscovering returned is the encoder's segmentation of the first string the search
finds, not necessarily the shortest, and a table `merge_tower()` refuses is refused the same way:

```cpp
#include "munch/verifier/token_automaton.hpp"

const std::vector<verifier::Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "a"}};

const auto automaton{verifier::token_automaton(merges)};        // built once for every window decided on it

verifier::miscovering(automaton, "ab", 0);                      // std::nullopt, as miscovering() on the tower answers
verifier::miscovering(automaton, "ab", 1);                      // the marked ab itself, its token beginning at offset 0
verifier::miscovering(verifier::token_automaton({}), "ab", 0);  // a|b, the empty table cutting between every two bytes
```

`verifier::token_automaton(merges, alphabet)` builds the token automaton over an alphabet of characters, as
`merge_tower(merges, alphabet)` builds the tower. The alphabet is UTF-8 text whose characters are the tokens before any
merge, numbered in its order with the products after them at their ranks, and every spelling is UTF-8. `miscovering()`
decides on it by the same search, which compares spellings as bytes, a token's spelling neither beginning nor ending
inside a character, and its verdict is `miscovering()`'s on `merge_tower(merges, alphabet)`; over the characters U+0000
to U+007F it decides an ASCII window as on `token_automaton(merges)`, to the byte. The automaton has one base state plus
one per merge whose product can be read, however many characters the alphabet holds, so it is the route for a whole
character-level table, whose alphabet is every character of its vocabulary. A tokenizer's normalizer, such as
SentencePiece's replacement of the space by U+2581, and its byte fallback for characters outside the vocabulary are not
part of the table, and its loader applies them:

```cpp
const std::vector<verifier::Merge> merges{
        {.left = "▁", .right = "t"}, {.left = "h", .right = "e"}, {.left = "▁t", .right = "he"}};

const auto automaton{verifier::token_automaton(merges, "▁theé")};  // the five characters are the tokens 0 to 4

verifier::miscovering(automaton, "▁t", 0);  // std::nullopt, as on the tower
verifier::miscovering(automaton, "he", 0);  // the marked ▁the, one token, its he four bytes in
automaton.token_of("▁the");                 // 7, the third product past the five characters
```

The table a SentencePiece model needs is its self-split table, not the one the Hugging Face converter derives.
SentencePiece merges any two adjacent symbols whose concatenation is a piece, the highest score first and the leftmost
on a tie, and on any input it forms a piece only by the pair its own loop joins last on the piece's spelling alone, so
the self-split table takes that pair for every piece, ranked by score, the highest first and products of equal score the
shortest first. On input over the model's characters, before its user-defined symbols and byte fallback, the encoder
over that table is SentencePiece's when no two products share a score, and the constructions take the table when every
part ranks before its product, as on Mistral 7B v0.1 and Yi 6B. The converter takes every split of every piece into two
pieces, and a piece can score above a piece it contains, as ▁the scores above ▁th in Mistral 7B v0.1, so its table names
parts before any merge makes them, here in the merge (▁th, e), and the constructions refuse it there. Where products
share a score SentencePiece breaks the tie by position, which a rank order emulates only for some ties, so a loader
finds a rank order of such a group that reproduces the model or refuses the model; for the fifteen pieces of Mistral 7B
v0.1 made of U+2581 alone, which share one score, an exhaustive search finds such an order.

`verifier::token_automaton(tokens, pairs)` builds the token automaton from an encoder's canonical adjacency relation in
place of a table: the canonical tokens V, each a string the encoder segments as one token, and the canonical pairs E,
each two tokens of V as a `verifier::Adjacent` whose concatenation the encoder segments into the two. It has the base
state and one state per token, the state of a token reading the tokens a pair lets follow it, and `miscovering()`
decides a window certificate on it by the same search with the same bound, since the canonical adjacency theorem the
search rests on holds for every projective encoder. A merge process joins one adjacent pair of tokens at a time and
never splits one, and it is projective when the merges of a run inside any interval of the input, up to the first merge
across an end of it, begin the run on the interval alone. Every key rule is projective, a rule that joins, among the
pairs it may join, one of least key under any function of the pair, the leftmost or the rightmost of a tie: canonical
byte-pair encoding over a table, and SentencePiece's own loop with its position ties, which no rank order reproduces for
the pieces ab and ba at one score. So is the pass order, which applies one merge left to right until it no longer
applies before choosing the next. The rule that joins the pair occurring most often is not, since over the pieces aa and
ab at one score it segments aab as aa|b and aabab as a|ab|ab. The relation must be the encoder's own, which the caller
vouches for, read off the encoder run on each token and on each pair of tokens, so a model whose ties no table
reproduces is certified through its own encoder. A token held twice and a pair naming a token the list does not hold are
refused with `std::invalid_argument`. The state of a token stores the tokens that may not follow it, |V|^2 - |E| in all,
so the relation of a whole model is too large to build: Mistral 7B v0.1 has 31,741 canonical tokens and 152,174,452
canonical pairs of the 1,007,491,081 it could have, and Yi 6B 63,436 tokens and 409,579,072 pairs of 4,024,126,096. A
loader builds the relation of a tie group or of the pieces over a few characters, as the tests do for the whitespace
pieces of Mistral 7B v0.1 and for the pieces of both models over t, h, e and n.

`verifier::encoder_divergence(first, second)` decides whether the encoders of two token automata segment every string
alike, comparing their canonical tokens and pairs, and returns `std::nullopt` or a string they segment apart. Two
projective encoders over one alphabet are equal exactly when their V and their E agree, and a shortest string they
segment apart is a token of one V that the other lacks or the spelling of a canonical pair of each, split at two
different places, so it has at most 2L - 1 bytes, L the longest token of either. The decision returns the first token of
the first automaton the second lacks, else the first of the second the first lacks, else the shortest spelling of a pair
canonical under exactly one, which is a shortest string the two segment apart, in at most 2 |V|^2 adjacency tests. Two
encoders that agree on every string of at most 2L - 1 bytes therefore agree on every string: the longest of the fifteen
whitespace pieces of Mistral 7B v0.1 is a run of 16, so a table reproduces SentencePiece on every run of U+2581 exactly
when it does on every run of at most 31. It is the acceptance test of a loader that compiles a tie group into a rank
table, which takes the table exactly when its automaton and the automaton of the model's own relation do not diverge:

```cpp
const std::vector<std::string> tokens{"a", "b", "ab", "ba"};  // the pieces ab and ba at one score, as SentencePiece
const std::vector<verifier::Adjacent> pairs{
        {.left = "a", .right = "a"},   {.left = "a", .right = "ab"},  {.left = "b", .right = "b"},
        {.left = "b", .right = "ba"},  {.left = "ab", .right = "a"},  {.left = "ab", .right = "b"},
        {.left = "ab", .right = "ab"}, {.left = "ab", .right = "ba"}, {.left = "ba", .right = "a"},
        {.left = "ba", .right = "b"},  {.left = "ba", .right = "ab"}, {.left = "ba", .right = "ba"}};
const std::vector<verifier::Merge> merges{{.left = "a", .right = "b"}, {.left = "b", .right = "a"}};

const auto relation{verifier::token_automaton(tokens, pairs)};  // the base state and one state per token

verifier::miscovering(relation, "aa", 1);                                         // std::nullopt: every aa is a|a
verifier::encoder_divergence(relation, verifier::token_automaton(merges, "ab"));  // bab, ba|b here, b|ab by the table
```

`verifier::trie_chain(initial, continuation)` takes a two-mode vocabulary, the initial tokens and the continuation
tokens, and builds the verifier of WordPiece segmentation over it, the scanner that takes the longest initial token at
the start of the input and then, at each boundary, the longest continuation token, failing where no token of the mode
matches; the verifier accepts a byte string under exactly the scanner's segmentation of it and under no marking when the
scanner does not consume it whole, so a byte no token names is in no domain string. A run of the scanner is a node of
its mode's trie begun at a boundary, and a state of the verifier is the node of the oldest run still armed, which spells
the unresolved tail, with the depth inside the tail at which the unarmed run began: the armed runs between are not
stored, since every boundary inside the tail is forced by longest match, the scanner's own reading of the tail, which is
done once per trie node at construction as the node's chain. A step is refused when an armed run reaches a token or the
unarmed run drops, and a boundary requires the unarmed run to be at a token. With N the nodes of both tries, P the nodes
other than the root with a child and c the longest chain, at most the longest token, the states are at most
1 + (N - 1) + P * c. A token may be in both sets and a set may be empty; an empty token is refused with
`std::invalid_argument` naming its set and position. The four decisions then run on the chain as on any verifier,
`miscovering()` deciding a window certificate under the vocabulary, `boundary_gap()` returning the length of the longest
token the scanner emits, `realizable()` the mask of every state, and `divergence()` over two chains deciding whether two
vocabularies segment every byte string alike, with the shortest string they segment apart or only one of them scans
whole:

```cpp
#include "munch/verifier/trie_chain.hpp"
#include "munch/verifier/verifier_decisions.hpp"

const std::vector<std::string> initial{"a", "ab"};
const std::vector<std::string> continuation{"b"};
const std::vector<std::string> with_b{"a", "ab", "b"};

const auto chain{verifier::trie_chain(initial, continuation)};    // a alone, or ab followed by any number of b

verifier::miscovering(chain, "ab", 0);    // std::nullopt: every ab is a token beginning at its a
verifier::miscovering(chain, "b", 0);     // the marked ab itself, its b in a token beginning at 0
verifier::boundary_gap(chain);            // 2, the longest token ab

verifier::divergence(chain, verifier::trie_chain(with_b, continuation));  // b, scanned whole only where b is initial
```

`verifier::ripple_radius(merges)` of `munch/verifier/ripple_radius.hpp` decides the lookbehind of canonical byte-pair
encoding over a merge table: how far a byte prepended to an input can change the input's tokens. The origin ripple of a
byte and an input is one past the last byte of the input whose covering token, as an interval, differs once the byte is
prepended, a token that reaches into the prepended byte differing wherever it reaches; the origin radius is its supremum
over every byte and input, and the mirrored constant the supremum of one past the start of the last changed token, the
least length beyond which every boundary survives every prepended string. A table `merge_tower()` takes, each part a
byte or an earlier product and no product spelled twice, satisfies the ordering condition, every merge producing a token
ranked before every merge consuming it, and under it the radius is infinite exactly when an effective self-pair exists:
a merge (t,t) whose two copies of t the merges before it leave as two tokens, so that prepending t to an even power of t
moves the covering token of every byte. The decision takes the merges in rank order and returns the first such merge as
`verifier::Effective_self_pair`. Without one it explores the overlap graph, two copies of the automaton that spells the
canonical tokens reading one input, the second having read the prepended byte first, and returns
`verifier::Finite_ripple`, the radius and the mirrored constant read off the longest path to a step on which the two
copies' covering tokens differ, each with a prepended input attaining it when it is not zero. The overlap graph decides
the radius for any table and a polynomial computation decides it in the number of merges under the ordering condition;
on the tables munch takes the two answer alike, so one function serves both. It refuses what `merge_tower()` refuses,
with `std::invalid_argument` naming the merge and its rank:

```cpp
#include "munch/verifier/ripple_radius.hpp"

const std::vector<verifier::Merge> merges{{.left = "b", .right = "c"}, {.left = "a", .right = "bc"}};
const std::vector<verifier::Merge> doubled{{.left = "a", .right = "b"}, {.left = "ab", .right = "ab"}};

const auto ripple{verifier::ripple_radius(merges)};  // finite: a prepended to bc makes the one token abc
const auto& finite{std::get<verifier::Finite_ripple>(ripple)};

finite.radius;                                       // 2, both bytes of bc covered anew
finite.mirrored;                                     // 1, the changed token bc beginning at 0
verifier::ripple_radius(doubled);                    // the effective self-pair at rank 1: (a,b) leaves abab as ab|ab
```

`verifier::ripple_radius(merges, alphabet)` decides the radius over an alphabet of characters, as `merge_tower(merges,
alphabet)` builds the tower. The alphabet is UTF-8 text whose characters are the tokens before any merge, a prepended
symbol is one of its characters, an input is a string over them, and the decision is the same, with its distances in
bytes as every position over the character overloads: the radius is one past the last byte of the input whose covering
token changes, the mirrored constant one past the first byte of the last changed token, and `Prepended_input` spells the
prepended character by its UTF-8 bytes. The radius counted in characters is finite exactly when this one is, but it is
not this one divided by a length, since this one weighs each character by its bytes and the inputs attaining the two can
differ: with every character the table names p to q bytes long, the radius in characters r and the radius in bytes R
satisfy p r <= R <= q r, and over characters of one length k, R is k times r. The report *The Left Ripple of Byte-Pair
Encoding* counts symbols, which are characters here, so a figure it gives for a table of characters longer than a byte
counts characters, not bytes. Over the characters U+0000 to U+007F the decision is `ripple_radius(merges)`'s, to the
byte, and a character the alphabet holds twice is refused with `std::invalid_argument` naming it, as is a merge that
names a character outside the alphabet:

```cpp
const std::vector<verifier::Merge> merges{{.left = "é", .right = "t"}, {.left = "▁", .right = "ét"}};

const auto ripple{verifier::ripple_radius(merges, "▁ét")};  // finite: ▁ prepended to ét makes the one token ▁ét
const auto& finite{std::get<verifier::Finite_ripple>(ripple)};

finite.radius;                                             // 3, the three bytes of ét, two characters, covered anew
finite.mirrored;                                           // 1, the changed token ét beginning at 0
finite.radius_witness->symbol;                             // "▁", the prepended character, three bytes long
```

`verifier::rule_ceilings(merges)` of `munch/verifier/rule_ceilings.hpp` reads the ceilings on a table's radii off its
merges, with no pair graph, and computes its two right radii from the merges. The right span ripple of an input and an
appended byte is the furthest from the input's end that a token of its encoding changes once the byte is appended, and
the right start ripple the furthest that a byte's covering token changes its start; their suprema are the right span
radius and the right start radius Q_s, finite for every table. With the origin radius R, every window of width
R + Q_s + 1 pins its byte R at the start that byte has in the window's own encoding, the pinned-byte theorem of the
report *The Left Ripple of Byte-Pair Encoding*, so a planner cutting at those windows needs R and Q_s, or ceilings on
them, a wider window pinning its byte by the same theorem. The ceilings are the report's Proposition 'Bounds in the
rules': Q_s is at most max(0, S - 1) for S the lengths of the merges' left parts summed, which the chain of merges
(c, d), (b, c), (a, b) attains; a finite R is at most the lengths of the useful merges' right parts summed, under the
ordering condition, which every table munch takes satisfies; and without that condition at most n_0^2, n_0 one more than
the bytes the table names plus the lengths of the products summed. Both radius ceilings are `std::nullopt` exactly when
an effective self-pair makes R infinite, as `ripple_radius()` decides it. The radii are the report's Theorem 'Chain
search', exact under the ordering condition: a state is a string and a byte whose encodings with and without the byte
differ in their first token, it is built from a chain of useful merges, each prepending its left part, and the search
extends every state by every merge passing one look-up and one encoding of two tokens, visiting each state once. It
enumerates every state, which is exponential in the span radius in the worst case. Each radius comes with an
`Appended_input` attaining it, and the function refuses what `merge_tower()` refuses:

```cpp
#include "munch/verifier/rule_ceilings.hpp"

const std::vector<verifier::Merge> merges{{.left = "a", .right = "b"}, {.left = "a", .right = "ab"}};

const auto ceilings{verifier::rule_ceilings(merges)};

ceilings.start_ceiling;   // 1, the left parts a and a summed less one
ceilings.radius_ceiling;  // 3, the right parts b and ab summed, at least the radius 2 ripple_radius() returns
ceilings.span_radius;     // 2: appending b to aa makes the tokens a|a the one token aab
ceilings.start_radius;    // 1, the second a's token beginning at 0 once b is appended
```

`verifier::piece_set(pieces)` of `munch/verifier/piece_set.hpp` takes the vocabulary of a segmenter that cuts every
string it reads into pieces of a finite set, such as the pieces of a SentencePiece Unigram model, and builds the
Aho-Corasick automaton of the pieces once. The cut theorem it rests on reads no scores: a position of a string that no
occurrence of a piece straddles, beginning before it and ending after it, is a boundary of every segmentation of the
string into pieces, since the token holding the byte before the position cannot reach past it. So it holds for every
segmenter whose tokens are pieces of the set, whatever chooses among the segmentations: Unigram's Viterbi pass, its
sampled and n-best segmentations, maximal munch over the set, or any other rule. `Piece_set::straddle_free(input)`
returns every such position of an input in one pass of the automaton. `verifier::piece_cut(set, window, offset)` decides
the cut offset bytes into a window for every string the window occurs in: it is certified, `std::nullopt`, when no piece
placed across the cut agrees with the window wherever the two overlap, and otherwise the `Straddling` returned is the
refutation, the window extended by the bytes of the first such piece that hang past either end, segmented with the piece
as one token and every other character alone, and the window's start in it. The decision is sound for every set and
exact when every byte of the window is a piece by itself, or every character of a window over pieces of whole
characters, and the refutation is then a segmentation into pieces whenever each character it writes alone is a piece, as
for a window and pieces of whole characters or a window of ASCII. With L - 1 bytes on each side of the cut, 2L - 2 in
all for L the longest piece, the decision is whether a piece occurs in the window across the cut; at width two it is the
pair table `Piece_set::has_pair()`, and a pair at the cut that no piece holds certifies at any width. Gigatoken, Marcel
Rød's Rust tokenizer ([github.com/marcelroed/gigatoken](https://github.com/marcelroed/gigatoken), version 0.9.0 on PyPI,
in `src/bpe/sentencepiece.rs`), uses the same pair test to certify cuts before eight punctuation bytes, `.`, `,`, `"`,
`)`, `;`, `:`, `!` and `?`, in SentencePiece models when word splitting is enabled, a sufficient condition, and
SentencePiece Lite states it as its Character Bigram Co-occurrence Principle (section 6.3 of `lite/README.md` in
[github.com/google/sentencepiece](https://github.com/google/sentencepiece), first committed on 15 September 2026). The
set is read in bytes, and what lies outside it is the caller's to map: the cuts are positions of the normalized string
under a normalizer such as SentencePiece's, a wrapper that also refuses every cut inside a character and every cut
between two characters without a piece of one character keeps the theorem for the runs of unknown characters
SentencePiece joins into one token, and byte fallback and user-defined symbols are not read. An empty piece or a piece
held twice is refused with `std::invalid_argument`, as are an empty window and an offset that does not lie between two
of its bytes:

```cpp
#include "munch/verifier/piece_set.hpp"

const std::vector<std::string> pieces{"a", "b", "abba"};

const auto set{verifier::piece_set(pieces)};

set.straddle_free("aabbab");         // {1, 5}: abba straddles 2, 3 and 4
set.has_pair('b', 'a');              // true, inside abba
verifier::piece_cut(set, "bb", 1);   // abba, the window at 1 in it: bb may be the middle of abba
verifier::piece_cut(set, "bbb", 1);  // std::nullopt: abba agrees with bbb at no placement across the cut
```

`verifier::chunk_plan(verifier, input, request)`, `verifier::chunk_plan(automaton, input, request, radii)` and
`verifier::chunk_plan(set, input, request)` of `munch/verifier/chunk_plan.hpp` turn these decisions into a worker
contract. A plan is a list of chunks, each with the byte it begins at, the cut that ends it, below which its worker
keeps the tokens, the byte the worker reads to, and the evidence that licenses the cut, so that workers that each
segment their chunk alone reproduce the whole input's segmentation. The automaton is a byte table's, as
`token_automaton(merges)` builds it, and one over the characters of an alphabet is refused with `std::invalid_argument`.
The cuts are sought as `chunk_boundaries()` seeks them, the first at or after the later of each equal-division target
and one past the previous cut, and a target with no cut after it is dropped, its chunk scanned sequentially to the next
cut or the end: that is the fallback over a stretch with no cut.

From the floor the search walks the input byte by byte and takes, at each byte, the first route that gives a cut at or
after the floor. A cut the caller supplies in `Plan_request::supplied`, such as a boundary of an exact pre-tokenizer,
comes first, with the bytes the caller says a worker reads past it. Then the windows ending at the byte are certified on
the input, from one byte wide to the request's budget: on a verifier `pinned_cut()` at the window's last byte gives the
origin and `dependence()` must find every certified cut of the pair independent, the per-cut guarantee a sparse subset
of an inventory's anchors needs, and the window must begin inside the chunk so that its worker reads it; on a token
automaton the origin is the start the last byte has in the window's own encoding, `miscovering()` decides it, and the
canonical adjacency theorem makes every boundary a cut whose two sides are encoded alone, anywhere in the input. Charles
Xu's parallel tokenizer ([charlesxu.io/parallel-tokenizer](https://charlesxu.io/parallel-tokenizer/), dated 25 November
2025, its merge-table check since 9 October 2026) cuts where no merge pairs a symbol on the left of the cut with one on
the right, a static check on the merge table of the same kind. Last, on a token automaton given the origin radius and
the right start radius, or ceilings on them, the uniform window of the pinned-byte theorem, of width R + Q_s + 1 with
the byte walked as its byte R, is encoded and its byte's start read off, which always places a cut within the longest
token's length before the byte, so that, away from the input's ends, each cut lies within that length past its floor.
The search is monotone: a wider window ending at the same byte occurs wherever a narrower one does and so certifies the
same cut, so only failing windows are widened, and each distinct window is decided once per plan, the plan counting the
windows asked for and the ones decided. On a verifier the prefix side of `dependence()` is the right context at no byte
past the cut, so a worker reads to its cut there as on a token automaton, and only a supplied cut makes it read further.
`munch_planner_bench` measures the plans against the sequential encoding, with the results in
[docs/benchmarks.md](benchmarks.md).

Over a piece set the cuts are the positions of the input that no occurrence of a piece straddles, sought from the same
targets with the same fallback, each a boundary of every segmentation of the input into pieces, so a worker reads to its
cut and no window is decided; the request's budget is not read. Every segmentation of the input is then a concatenation
of segmentations of its chunks, so under a score summed over the tokens the input's optimal segmentations are the
concatenations of its chunks' optimal ones, and a Viterbi pass in exact arithmetic that replaces the best path at a
position only on a strictly greater score returns the same tokens on the chunks as on the whole input, ties included. A
pass whose scores round, as SentencePiece's float32 lattice does on long inputs, may choose between paths of nearly
equal score otherwise on the whole input, so the contract is the exact optimum. A supplied cut comes first at its
position, as on the other routes, with its context read past it, since a boundary of an exact pre-tokenizer is a cut
that every segmenter over the set keeps when it segments the pre-tokenized text. A stretch whose every position a piece
straddles, such as a run of one byte under a piece of two of it, is scanned whole by one worker.

```cpp
#include "munch/verifier/chunk_plan.hpp"

// over the merges (a, b) and (a, ab) of the example above, origin radius 2 and right start radius 1
const auto automaton{verifier::token_automaton(merges)};
const verifier::Uniform_radii radii{.radius = 2, .start_radius = 1};

const std::string text(12, 'a');
const verifier::Plan_request request{.chunks = 3, .budget = 4, .supplied = {}};

verifier::chunk_plan(automaton, text, request, std::nullopt);  // one chunk: no window of a's is certified
const auto plan{verifier::chunk_plan(automaton, text, request, radii)};

plan.chunks[0];  // [0, 4), read to 4, cut by the uniform window from byte 2 to 6, its byte 2 beginning a token
plan.chunks[1];  // [4, 8), the same way at byte 8
```

`verifier::window_density(merges)` of `munch/verifier/window_density.hpp` decides whether a table's windows certified at
their last byte, `miscovering()`'s certificate, are dense, that is whether only finitely many strings contain none of
them, and when they are, the density bound N, one more than the longest string that contains none; otherwise they are
sparse. The decision is the report's Proposition 'The last byte'. An infinite origin radius leaves them sparse at every
width, and the result is the effective self-pair of `ripple_radius()`. With R finite, a certified window wider than
R + 1 has its suffix of width R + 1 certified, so the question is decided on the windows of width at most R + 1, and the
strings avoiding those are the paths of a graph on the R-byte strings that avoid them, finitely many exactly when the
graph has no cycle. Each window is decided by `miscovering()` on the table's token automaton at the start its last byte
has in its own encoding, the one origin that can hold. On a sparse table a cycle's label comes back as the loop, every
power of which avoids every certified window, with the first pumping double when one shows it: a token t that the
encoding of tt keeps as t|t and a byte b with which the encoding of ttb begins with another token. Beside the verdict is
the pinned-byte guarantee with the right start radius Q_s of `rule_ceilings()`: every window of width R + Q_s + 1 has a
cut, so the cuts are dense whatever the verdict, with a density bound between R + 1 and R + Q_s + 1 by the report's
Theorem 'Density'. The graph has up to |B|^R states for B the bytes the table names, so the decision takes a budget of
avoiding strings, `window_density_budget` by default, and returns `Density_budget` when it runs out; GPT-2's first 41
merges, of radius 3, need at most a hundred. `munch_merge_density` in tools/probes prints the decision for the report's
small tables and GPT-2's prefixes:

```cpp
#include "munch/verifier/window_density.hpp"

// over the merges (a, b) and (a, ab) of the examples above
const auto sparse{std::get<verifier::Sparse_windows>(verifier::window_density(merges))};

sparse.loop;                   // "a": a run of a's followed by b ends in the token aab, so no factor of it is certified
sparse.pumping_double->token;  // "a", with the byte "b": aab begins with aab, not with a
sparse.pinned.width;           // 4, R = 2 and Q_s = 1: every window of four bytes has a cut

const std::vector<verifier::Merge> pair{{.left = "a", .right = "b"}};

const auto dense{std::get<verifier::Dense_windows>(verifier::window_density(pair))};

dense.bound;  // 2: with Q_s = 0 every window of two bytes is certified
```

That certificate is exact and, for the same reason, fragile: one string literal, comment, or whitespace run whose
interior admits the candidate byte disqualifies it, which is enough to leave a conventional token set certifying
nothing. Since the tokens responsible are usually the ones a parser throws away, `set_ignored_tokens()` lets a builder
declare them, and `is_split_point_ignoring(symbol)` then answers a weaker question: is splitting here safe once those
tokens are deleted from both streams? It is sound and never admits less than `is_split_point()`, but it is conservative
rather than exact, and the guarantee it carries is correspondingly weaker on two counts: it holds only for input the
serial scan tokenizes completely, with no malformed-input prefix analogue, and a caller that keeps those tokens must use
`is_split_point()` instead. It recovers newline for a conventional C-like token set and newline, tab and carriage return
for a JSON lexer, in both cases without changing the token definitions:

```cpp
builder.set_ignored_tokens({Token::Whitespace, Token::LineComment});

const auto lexer{builder.build()};

lexer.is_split_point('\n');           // false: a whitespace run can contain it
lexer.is_split_point_ignoring('\n');  // true: both halves of the split run are discarded
```

Since 2.2 the weaker test asks less than the report's: the restart after the cut need not reach the very state the
interrupted scan is in, only one with the same future once the discarded kinds are not told apart. So it certifies every
symbol the report's condition does and some more, still in one table lookup per symbol. The technical report *Certified
Split Points for Parallel Lexing: Exact and Modulo Discarded Tokens* states both certificates formally as of v1.2.0,
relates them to the parallel-automata literature, and studies which grammars certify usable split symbols under each:
read it as [arXiv:2608.03473](https://arxiv.org/abs/2608.03473), as [docs/split_points.md](split_points.md), or build
the formal version from [paper/](../paper/). Its companion report *Certified Split Windows for Parallel Lexing:
Recovering Boundaries Where No Byte Certifies* generalizes the certificate from single bytes to short byte windows, the
mechanism behind `chunk_boundaries_with_windows()`: read it as [arXiv:2608.09761](https://arxiv.org/abs/2608.09761), as
[docs/split_windows.md](split_windows.md), or build it from the same directory. The third report *Certified Panic Mode:
Repair-Invariant Error Recovery for Maximal-Munch Lexing* turns the same certificates into resynchronization points for
input that does not tokenize, the contract behind the Tokenizer's `recover()` family: read it as
[arXiv:2609.10600](https://arxiv.org/abs/2609.10600), as [docs/panic_mode.md](panic_mode.md), or build it from the same
directory.

### **2. Tokenizer API (`munch::tools::tokenizer::Tokenizer`)**

The Tokenizer owns the input and a reading position, and reports each read as a `Result`: a token, the end of the
input, or an error carrying its offset. That much is a convenience, since `Lexer::tokenize_all()` also walks a whole
input and does it faster.

What the Tokenizer adds, and the reason it exists, is that the position is yours to move. A parser can stop on an
error, ask where it stopped, move to a position the automaton certifies as a token start, and resume; or read a
prefix token, scan a construct no automaton covers by hand through `input()`, and continue past it with `seek()`.
None of that is reachable through `Lexer`, whose `tokenize()` is stateless and whose `tokenize_all()` pushes through
a sink that cannot be stopped, questioned, or repositioned. If a driver never needs to move the position, it does
not need this class.

```cpp
using namespace munch;
using namespace munch::core;
using namespace munch::regex;
using namespace munch::tools::tokenizer;

const std::string input = "boolean x 1234";

Tokenizer tokenizer{lexer, input};

for (;;)
{
    const auto result{tokenizer.next<Token_kind>()};

    if (result.end_of_input())
    {
        break;
    }

    if (result.has_error())
    {
        // Invalid input or unrecognized symbol
        std::cerr << result.error().message() << '\n';
        break;
    }

    const auto& token{result.token()};
    std::cout << "Token kind=" << static_cast<int>(token.kind()) << " lexeme=\"" << token.lexeme() << "\"\n";
}
```

`next()` returns a flat, three-state `Result<Token_kind>`:

- **Token**: `has_token()` is true and `token()` returns the matched token with its kind and lexeme.
- **End of input**: `end_of_input()` is true once the input is exhausted.
- **Error**: `has_error()` is true and `error()` carries the position and a textual description.

The three states are alternatives of one sum type, so they can also be handled exhaustively with `visit()`.

For context-dependent languages, a `Mode_tokenizer` holds several lexers as modes over the same input and switches
between them with `set_mode()`, as a driver does for header-names after `#include`. For tokens no practical
automaton covers, such as C++ raw string literals, whose bounded delimiter makes them regular in principle but not
worth a table, the driver reads a prefix token, scans by hand using `input()` and `scan_raw_string()`, and continues
past the literal with `seek()`.

Together the two layers split by control rather than by convenience: `core::Lexer` scans, including in parallel,
and `tools::tokenizer::Tokenizer` lets a driver decide where scanning resumes. `Mode_tokenizer` is the same cursor
over a `core::Mode_lexer`, and has no parallel entry point because a mode lexer has none.

> **Note:** `Tokenizer` is not thread-safe, and `Token::lexeme()` is a `string_view` into the `Tokenizer`'s internal
> input buffer. The view is invalidated by `load()` or by the `Tokenizer` being destroyed, so copy the lexeme to a
> `std::string` if a token needs to outlive either.

## **Error Recovery**

On malformed input, `next()` reports the error and deliberately does not advance: guessing a skip would invent
tokens. The driver chooses what happens next, and `recover()` is the certified choice: it moves to the first
position past the failure that a certificate proves begins a token, the first met in evidence order rather
than the provably smallest, and returns how many bytes were skipped.

```cpp
for (;;)
{
    const auto result{tokenizer.next<Token_kind>()};

    if (result.end_of_input())
    {
        break;
    }

    if (result.has_error())
    {
        const auto skipped{tokenizer.recover()};

        if (!skipped)
        {
            break;  // no certified byte or window of two to four bytes ahead: an explicit refusal, not a guess
        }

        std::cerr << "recovered, skipped " << *skipped << " bytes\n";
        continue;
    }

    process(result.token());
}
```

The position `recover()` lands on carries a contract rather than a convention: every completely tokenizable
repair of the input before the answer's supporting evidence places a token boundary there, so however the
damage before that evidence might be fixed, the resume point is a real token start; a repair that alters the
evidence itself, the certified byte or the whole window occurrence, forfeits the guarantee. When no certified
byte and no certified window of two to four bytes, the widths the search consults, lies ahead, the position
does not move and the refusal is explicit. Under modes, the answer is relative to the active mode's automaton.
The position-only form is `Lexer::next_certified_start(input, from)`, for drivers that plan without moving.

The evidence itself is returned on request: `recover_from_failure()` answers with the certified start and the evidence
interval and kind behind it, so a caller can reject an answer whose evidence overlaps text it distrusts, and
`recover_from_clean(clean_from)` floors the search at a caller's known-clean offset (an editor knows its edit span), so
the returned evidence is covered by construction. `recover()` stays the skip-count form above. All three leave the
position unchanged when their own search refuses; `recover()` and `recover_from_failure()` search identically, while the
clean floor can shrink the search domain, so the clean form may refuse where the failure-anchored forms answer. The
lexer-level form is `Lexer::next_certified_evidence(input, from)`.

When the remainder in hand is the whole rest of the input, a truncated or damaged file tail, the anchored queries
answer exactly rather than conservatively, because they may use what the certificates cannot: that the input ends
where the tail ends. A token set in which some token matches the empty string is decided, here and by every other
query, through its positive-width equivalent, the same automaton entered through a start state that does not
accept, which changes no scan.

```cpp
// Over the token set {ab, ba}: position 0 of the tail "ab" is provably a token start in every
// completely tokenizable repair, yet no certificate can see it, because refuting continuations
// would need bytes past the end of the input. The quantifier matters: a repair whose scan fails
// partway (prefix "b" gives "bab", which commits "ba" and dies) places no boundary there.
lexer.next_certified_start("ab", 0);  // nullopt: sound, but blind to the end of input
lexer.next_anchored_start("ab", 0);   // 0: exact at the tail

lexer.minimal_repair("b");            // "a": the shortest prefix that makes the tail tokenize
lexer.minimal_repair("z");            // nullopt: a certificate that no repair of any length exists
```

A tail beyond repair makes `next_anchored_start()` refuse rather than answer vacuously, and `minimal_repair()`
returning a value guarantees the repaired whole tokenizes, with the empty string meaning the tail already does.

Two related queries describe the token set itself. `lag()` reports how far a scan can run past an accepted token before
a rollback could occur, with `std::nullopt` as a certificate that the excursion is unbounded. `rescue()` decides exactly
whether some completely tokenizable input makes the scan roll back, a rescue being a rollback after a failed lookahead
that lets the scan continue where a scheme restarting at every accept would have declared the input malformed, and
returns the shortest such input with whether its search was exhaustive: on `{a, abb, b, c}` the witness is `ab`, where
the scan of `a` reads the `b` before rolling back, and `{a, abc, bc}` is rescue-free though a stretch opens after `a`,
since every completely tokenizable continuation closes `abc` instead. `rescue_free()` is that decision as one bit: true
exactly when the exhaustive search found no witness, so that the restarting scheme emits the tokens of serial maximal
munch on every completely tokenizable input, and false either when a witness exists or when the search stopped at its
cap, which `rescue()` tells apart. Like `is_split_point()`, all of these are properties certified from the compiled
automaton, not heuristics.

## **Context-Dependent Tokenization**

`Lexer` matches one flat token set everywhere. Inside a string literal a quote terminates rather than opens, and a
comment that nests needs to know how deep it is. A flat token set does reach an ordinary escaped literal, matching it
whole; what it cannot do is report the interior as separate tokens, and it cannot count nesting to an unbounded depth at
all. There are two ways to get context-dependence, and they differ in who decides when the context changes.

`Mode_tokenizer` holds several lexers as modes and the driver switches between them with `set_mode()`. Constructed that
way the tokenizer never switches on its own, which suits cases where the surrounding parser knows what is coming, such
as a header-name after `#include`.

`Mode_lexer`, built by `Mode_builder`, declares the switches in the grammar instead. Each mode is its own token set
compiled through the ordinary `Builder`, and each token carries an action on a mode stack: `stay`, `go_to`, `push` or
`pop`. Nesting comes from the stack, so nested comments need no counter in user code.

```cpp
enum class Mode : std::size_t { code, string };
enum class Token : std::size_t { identifier, quote, text, escape };

munch::core::Mode_builder builder;

builder.add_token(Mode::code, plus(any_of(Set::alpha())), Token::identifier, 2);
builder.add_token(Mode::code, text("\""), Token::quote, 1,
                  {.kind = munch::core::Mode_action_kind::push, .target = std::size_t{1}});

builder.add_token(Mode::string, text("\""), Token::quote, 1, {.kind = munch::core::Mode_action_kind::pop});
builder.add_token(Mode::string, concat(text("\\"), any_of(Set::all())), Token::escape, 1);
builder.add_token(Mode::string, plus(any_of(Set::all() - '"' - '\\')), Token::text, 2);

const auto lexer{builder.build()};

munch::core::Mode_stack stack;

const auto consumed{lexer.tokenize_all<Token>(
        input, [](Token token, std::size_t length, std::size_t mode) { /* ... */ }, stack)};
```

`Mode_tokenizer` accepts a `Mode_lexer` too, so a driver gets the same grammar-carried transitions: `mode()` follows the
stack, `depth()` reports the nesting, and `set_mode()` still forces a mode as an error-recovery hatch. Underneath, one
built from plain lexers is the same thing with a `Mode_lexer` whose tokens carry no actions, and the two differ in one
place: under a `Mode_lexer` the mode is scan state, so `load()` and `reset()` return it to mode 0 with the position,
while under plain lexers it is the caller's choice and survives them. The flat `Tokenizer` has none of this surface, and
that is the point of the split: a mode lexer has no parallel entry point, so `lexer()` lives only on the side where
planning a chunk is sound.

Among the other measured engines, only lexertl17 carries mode transitions in the grammar itself. It is munch's nearest
relative, a lexer built at run time from rules, and it has had start states with a next-state per rule for years, with
a stack behind them: `enums.hpp` carries `push_dfa` and `pop_dfa` bits and `lookup.hpp` pushes and pops start states,
underflow included. **Its mode support is therefore the same expressive class as munch's, not a weaker one, and nesting
is as available there as here.** The difference the table below reports is throughput, not reach. logos reaches the
same end from the caller's side rather than the grammar's: `Lexer::morph` turns a lexer for one token type into a lexer
for another over the same input, which is context-dependent lexing driven by user code rather than by a per-rule
transition. The general-purpose regex engines have no mode concept at all, so a caller would switch patterns by hand,
which measures their per-match cost rather than their mode support and is what the tables of
[benchmarks.md](benchmarks.md) already report.

Mode support is an extension the grammar opts into, so the comparison comes in layers: input where modes are optional,
and input where the tested grammar uses a stack to count nesting.

**Where modes are optional.** A string literal can be one token, so a flat grammar tokenizes this corpus too.

| Engine       | Mode mechanism                                 |        MiB/s |
|--------------|------------------------------------------------|-------------:|
| `munch`      | grammar-carried actions on a mode stack        | 748.9, 750.1 |
| lexertl17    | start states with a next-state per rule        | 245.5, 248.6 |
| `munch` flat | no modes at all, a string literal as one token | 865.0, 857.4 |

Neither engine pushes on this corpus: the string mode is entered and left by a plain next-state transition, so this
table prices carrying a mode at all rather than the stack.

**Where the tested grammar uses a stack to count nesting.** Block comments nest here, and both engines push and pop to
track the depth.

| Engine    | Mode mechanism                          |        MiB/s |
|-----------|-----------------------------------------|-------------:|
| `munch`   | grammar-carried actions on a mode stack | 627.9, 621.6 |
| lexertl17 | start states pushed and popped          | 232.9, 236.8 |

Be precise about the theory this does not demonstrate. The language of *arbitrarily* nested comments is not regular,
since counting to an unbounded depth is what one finite automaton cannot do. This corpus nests only to depth four, and a
bounded depth is regular, so a flat grammar could unroll four levels and tokenize it. There is no flat row because that
would be a different grammar answering a different question, not because none could exist.

Both figures in each cell are medians of 15 passes from two runs of `munch_benchmark_compare 16 15`, with each corpus's
scenarios interleaved, measured at commit `00aa889` on a clean tree. The full transcript, every timed pass of the five
scenarios behind these tables, and the machine are archived in [paper/data/modes-2026-08](../paper/data/modes-2026-08).
The harness validates that the engines agree on every token before timing either, so the 5,121,241 tokens of the first
corpus and the 4,859,619 of the second are the same tokens in both.

**Ratios, as ranges rather than a point: 3.02 to 3.05 where modes are optional, 2.62 to 2.70 where the tested grammar
uses a stack to count nesting.** Those are the spreads within one session. Across three archive generations measured
at different commits, the optional-mode ratio spans about 2 percent and the nested ratio about 7 percent; those
movements conflate executable changes with environmental variation. Read them as observations, not as a trend: the
two corpora differ in token density, grammar and mechanism at once, so the difference between them is not
attributable to any one of those.

**What modes cost against a flat grammar, in this archive.** The mode grammar emits 11.1% more tokens for the same bytes
and takes 15.5% and 14.3% longer, so its cost per token is 3 to 4 percent *higher*. Read that as a fact about this
archive and nothing wider, because the sign does not remain stable across archive generations: across the three archives
this history reaches, the same figure runs from 6.6 percent lower to 6.4 percent higher. It is dominated by the flat
row, which ranged from 754.5 to 865.0 MiB/s, about 15 percent, while the modal rows stayed between 725.8 and 750.1,
about 3 percent. That is why no per-token figure is carried forward as a property of the library.

These are separate tables rather than rows in the ones above because a mode grammar emits more tokens than a flat one
for the same bytes, so the two are not doing the same work and are not treated as equal-work engine rows.

The `Mode_stack` overload reports where a scan stopped and what it was doing there: `stack.current` names the mode and
`stack.saved.size()` the nesting depth, which is what distinguishes an unterminated string from an unrecognized byte in
code. `Mode_builder::diagnose()` reports each mode's dead tokens and priority ties, plus the two faults only a modal
grammar has: modes nothing can enter, and modes nothing can leave. Unreachable modes are computed from mode 0 with an
initially empty stack. An inescapable mode has neither a live non-self `push`/`go_to` nor a live `pop` for which that
default-start grammar can establish a frame naming another mode; a caller-supplied frame may provide the missing return
context when such a live `pop` exists.

**Two things to know before reaching for it.** A `Mode_lexer` has no parallel entry point, because a worker cutting
blind cannot recover the mode and saved stack from the bytes at the cut. That is an obstruction rather than an
impossibility: what is proved is that a single byte cannot identify the mode when two or more admit every byte, which is
sufficient for a safe cut but not necessary. No scheme is ruled out: a multi-byte window, a checkpoint recorded by an
earlier pass, or a mode set restricted to stackless `go_to` are all outside what has been ruled out. And modes are an
expressiveness feature rather than a performance one. On the one corpus measured both ways the mode grammar took 14.3 to
15.5 percent longer while emitting 11.1 percent more tokens, most of the difference being those extra tokens. Whether
that holds for other grammars is not measured, and neither is how it varies with the share of input sitting inside
context-dependent constructs, since the scenario matrix varies the modal workload without a flat grammar beside it at
each density. [docs/limits.md](limits.md) gives the reasoning for both.

[docs/limits.md](limits.md) collects the full contract in one place: the matching model and what it excludes,
byte-orientation and UTF-8 handling, hard bounds, concurrency and lifetime guarantees, construction cost, and the escape
hatches for constructs beyond regular languages.
