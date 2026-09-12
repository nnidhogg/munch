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
find_package(munch 1.0 CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE munch::munch)
```

The installed package is self-contained: the build-time header-only dependencies (Boost utilities, mdspan) never appear
in munch's public headers, so nothing else needs to be installed or found, and the exported targets carry the C++23
requirement themselves. Do not install from a `MUNCH_BENCHMARK_COMPARE` build, whose comparison engines insist on
installing alongside. Install rules are generated only when munch is the top-level project, or when `MUNCH_INSTALL=ON`
is passed explicitly.

The `munch::munch` target is an interface umbrella over `munch_core` and `munch_tokenizer`, which pull in `munch_regex`,
`munch_nfa`, `munch_dfa`, and `munch_common` transitively. The Graphviz debugging helpers of
[docs/debugging.md](debugging.md) are not part of the umbrella target; link `munch_nfa_tools` and/or
`munch_dfa_tools` (installed as `munch::munch_nfa_tools` / `munch::munch_dfa_tools`) directly to use them.

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

The same nodes can be read from a pattern written the way a lexer generator takes it. `parse()` reads flex's
dialect of POSIX extended regular expressions over bytes: alternation, grouping, `*`, `+`, `?` and counted `{n,m}`,
the dot for any byte but the newline, bracket expressions with ranges, negation and the POSIX classes, escapes,
double-quoted literals, and `{name}` expanding to a definition given alongside. One escape flex has not got serves the
readers of character-level generators: `\u{X...}` names a code point, the UTF-8 encoding of that scalar in a literal,
and inside a bracket it turns the bracket to scalars, so `[\u{0}-\u{10FFFF}]` is any scalar and `[^\n\u{e9}]` every
scalar but two. What a token language cannot say is refused with the offset and the reason rather than approximated:
the anchors, trailing context and start conditions. A parsed pattern and its hand-built equivalent compile to the
same automaton.

```cpp
using namespace munch::regex;

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

const std::vector<std::pair<std::string_view, std::size_t>> inventory{{"aab", 0}, {"ba", 1}};

lexer.anchor_free_span(inventory);          // 2: the interior again, now reached through windows
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

The technical report *Certified Split Points for Parallel Lexing: Exact and Modulo Discarded Tokens* states both
certificates formally, relates them to the parallel-automata literature, and studies which grammars certify usable split
symbols under each: read it as [arXiv:2608.03473](https://arxiv.org/abs/2608.03473), as
[docs/split_points.md](split_points.md), or build the formal version from [paper/](../paper/). Its companion report
*Certified Split Windows for Parallel Lexing: Recovering Boundaries Where No Byte Certifies* generalizes the certificate
from single bytes to short byte windows, the mechanism behind `chunk_boundaries_with_windows()`: read it as
[arXiv:2608.09761](https://arxiv.org/abs/2608.09761), as [docs/split_windows.md](split_windows.md), or build it
from the same directory. The third report *Certified Panic Mode: Repair-Invariant Error Recovery for Maximal-Munch
Lexing* turns the same certificates into resynchronization points for input that does not tokenize, the contract behind
the Tokenizer's `recover()` family: read it as [arXiv:2609.10600](https://arxiv.org/abs/2609.10600), as
[docs/panic_mode.md](panic_mode.md), or build it from the same directory.

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
between them with `set_mode()`, as a driver does for header-names after `#include`. For tokens no practical automaton
covers, such as C++ raw string literals, whose bounded delimiter makes them regular in principle but not worth a table,
the driver reads a prefix token, scans by hand using `input()` and `scan_raw_string()`, and continues past the literal
with `seek()`.

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

When the remainder in hand is the whole rest of the input, a truncated or damaged file tail, the anchored queries answer
exactly rather than conservatively, because they may use what the certificates cannot: that the input ends where the
tail ends. A token set in which some token matches the empty string is decided, here and by every other query, through
its positive-width equivalent, the same automaton entered through a start state that does not accept, which changes no
scan.

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
a rollback could occur, with `std::nullopt` as a certificate that the excursion is unbounded. `rescue_free()` is a
one-sided gate: true guarantees that a scheme restarting at every accept agrees with serial maximal munch on every
input, and false is inconclusive, since the gate is sufficient and not necessary. On `{a, abc, bc}` it returns false
though no rescue exists there, a rescue being a rollback after a failed lookahead that lets the scan continue where the
restarting scheme would have declared the input malformed. Like `is_split_point()`, all of these are properties
certified from the compiled automaton, not heuristics.

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

`Mode_tokenizer` accepts a `Mode_lexer` too, so a driver gets the same grammar-carried transitions: `mode()`
follows the stack, `depth()` reports the nesting, and `set_mode()` still forces a mode as an error-recovery hatch.
Underneath, one built from plain lexers is the same thing with a `Mode_lexer` whose tokens carry no actions, so the mode
is scan state either way: `load()` and `reset()` return it to mode 0 with the position, and a driver that wants another
mode after a reset sets it again. The flat `Tokenizer` has none of this surface, and that is the point of the split: a
mode lexer has no parallel entry point, so `lexer()` lives only on the side where planning a chunk is sound.

Among the other measured engines, only lexertl17 carries mode transitions in the grammar itself. It is munch's nearest
relative, a lexer built at run time from rules, and it has had start states with a next-state per rule for years, with a
stack behind them: `enums.hpp` carries `push_dfa` and `pop_dfa` bits and `lookup.hpp` pushes and pops start states,
underflow included. **Its mode support is therefore the same expressive class as munch's, not a weaker one, and nesting
is as available there as here.** The difference the table below reports is throughput, not reach. logos reaches the same
end from the caller's side rather than the grammar's: `Lexer::morph` turns a lexer for one token type into a lexer for
another over the same input, which is context-dependent lexing driven by user code rather than by a per-rule transition.
The general-purpose regex engines have no mode concept at all, so a caller would switch patterns by hand, which measures
their per-match cost rather than their mode support and is what the tables of [benchmarks.md](benchmarks.md) already
report.

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
scenarios behind these tables, and the machine are archived in [paper/data/modes-2026-08](../paper/data/modes-2026-08). The
harness validates that the engines agree on every token before timing either, so the 5,121,241 tokens of the first
corpus and the 4,859,619 of the second are the same tokens in both.

**Ratios, as ranges rather than a point: 3.02 to 3.05 where modes are optional, 2.62 to 2.70 where the tested grammar
uses a stack to count nesting.** Those are the spreads within one session. Across three archive generations measured at
different commits, the optional-mode ratio spans about 2 percent and the nested ratio about 7 percent; those movements
conflate executable changes with environmental variation. Read them as observations, not as a trend: the two corpora
differ in token density, grammar and mechanism at once, so the difference between them is not attributable to any one of
those.

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
