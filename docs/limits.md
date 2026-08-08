# **Scope, Guarantees, and Limits**

[performance.md](performance.md) argues that munch is fast because of what it refuses to do. This note is the inventory:
the model the library commits to, the guarantees that commitment buys, the boundaries you can hit, and the escape hatch
for each. Read it when deciding whether munch fits a language, not after.

## **Modes: What a Flat Token Set Cannot Say**

`Lexer` matches one flat token set at every position. Real front ends need more than that: inside a string literal a
quote terminates rather than opens, and a comment that nests needs to know how deep it is. An ordinary escaped string
literal is regular, so a flat grammar matches it whole; what modes buy there is its interior as separate tokens.
Arbitrary nesting is the case no flat token set reaches at all. `Mode_lexer`, built by
`Mode_builder`, supplies it. Each mode is its own token set compiled through the ordinary `Builder`, and each token
carries an action on a mode stack the caller owns: `stay`, `go_to`, `push`, `pop`. Nesting comes from the stack, so
nested comments need no counter in user code.

**A `Mode_lexer` has no parallel entry point.** `Lexer` can be split because a certified byte recovers the whole scan
state: a worker starting there knows it is between tokens. With modes it would also have to recover the mode and every
saved frame, and no byte carries that. The same byte is a quote in code and a terminator in a string, and the depth a
nested comment reached is unbounded, so no finite per-byte certificate can name it. What is proved unavailable is
narrower than "never". What is proved is that a *single byte* cannot identify which mode the scan is in when two or more
modes admit every byte. Identifying the mode is sufficient for a safe cut but not necessary, since two modes might emit
the same tokens from that point on, so this is an obstruction rather than an impossibility. A multi-byte window, a
checkpoint left by an earlier pass, an independently recorded stack, or a mode set restricted to stackless `go_to` are
all outside that result. Restricting to `go_to` alone bounds the state space and makes the question askable again, which
is a genuine open direction rather than a shipped feature. Where a grammar admits a flat token set, use `Lexer` and get
the parallel path; where it does not, `Mode_lexer` scans serially and honestly.

**Modes are an expressiveness feature, not a performance one.** On the one corpus measured both ways the mode grammar
took 14.3 to 15.5 percent longer and emitted 11.1 percent more tokens, scanning string interiors and so emitting more
of them for the same bytes. Whether that holds for other grammars is not measured. The `munch_benchmark_compare` mode
section measures both on one corpus with the scenarios interleaved. In the archive at `paper/data/modes-2026-08` the
mode grammar emits 11.1% more tokens for the same bytes and takes about 15% longer, so its per-token cost is 3 to 4
percent higher there. That describes one session and the sign does not hold: across the three archives this history
reaches, the figure runs from 6.6 percent lower to 6.4 percent higher, tracking a flat row that varies by about 15
percent against about 3 percent for the modal rows. No per-token figure is carried as a property of the library.
Whether the gap widens as the input carries fewer string literals is untested, since no flat grammar is measured
beside the modal one at each density. The intuition that several small automata beat one large one does not hold
either, because only the states a scan actually visits are hot and splitting them across modes does not shrink that
working set. Use `Lexer` where the grammar allows it, both for the throughput and for the parallel path; use
`Mode_lexer` where the language genuinely needs a mode stack.

Within `Mode_lexer`, scanning stays inside one mode's batch pass until a token carries any non-stay action, rather
than re-entering the scanner once per token. Any such action ends the pass, including a push whose target is the
mode already being scanned, which is what a comment nesting inside itself does. The `per-token` and `batched` rows of
`munch_benchmark_modes` measure what it is worth; no run of that benchmark is archived here, so no figure for it is
quoted. A test pins only that the two drivers agree on the
token stream, since a wall-clock bound in a unit test varies with the machine, the compiler and the sanitizers.

The rest of this note describes the model each individual mode obeys.

## **The Model: Regular Languages, Longest Match, Anchored**

munch recognizes exactly the regular languages its combinators can express, always by longest match from the current
position, with token priorities (lower value wins) breaking ties between patterns that accept the same lexeme. That
model excludes, deliberately and permanently:

- **Capture groups and submatch extraction.** A token is a kind and a lexeme. Structure inside the lexeme (the digits of
  a float, the body of a string) is the consumer's job, and the lexeme is right there to slice.
- **Backreferences and lookaround.** Backreferences exceed regular power outright. Lookaround over regular
  subexpressions does not: it stays within the regular languages, but it is a zero-width, context-dependent operator
  that a single forward table scan cannot express, so supporting it would mean a different construction and a slower
  matcher on every token. Both are omitted, for different reasons.
- **Unanchored searching.** A lexer always matches at the current offset. There is no scanning ahead for the next
  occurrence of a pattern.
- **Shortest or first match.** Longest match is not configurable. `--` lexes as one token when a `--` pattern exists,
  never as two `-` tokens, which is exactly the maximal-munch rule real languages specify.

The batch entry point `tokenize_all()` additionally requires random access to the input, because longest match may read
past the last accepting position and must resume from it; single-pass input works through the per-token API.

## **Byte-Oriented, With UTF-8 by Expansion**

The engine reads bytes, never code points. `regex::Set` holds byte values, and `utf8::range` admits Unicode by expanding
a code point range into its UTF-8 byte sequences at pattern-build time (surrogates excluded, ill-formed sequences
rejected by construction). The consequences:

- There is no general Unicode property API, no case folding, and no normalization; XID_Start and XID_Continue below are
  the two properties currently supplied. A case-insensitive keyword is spelled out (`choice(text("if"), text("IF"))` or
  a `Set` per position), and input is matched as the bytes it is.
- Standards-accurate Unicode identifier classes exist: `unicode::xid_start()` and `unicode::xid_continue()` match the
  XID_Start and XID_Continue properties, generated from the Unicode Character Database pinned at the version
  `unicode::version()` reports and expanding through `utf8::ranges` the way everything Unicode does here. They are
  lexical property matching, nothing more: no normalization, no case folding, no mixed-script or confusable detection,
  and no language profile. Underscore, for instance, is a profile choice spelled `choice(text('_'),
  unicode::xid_start())` by the caller. Those concerns belong above the lexer, per UAX #31.
- `utf8::range` never accepts ill-formed UTF-8. The engine itself does not validate input as UTF-8, though: a grammar's
  other byte-oriented patterns may accept arbitrary bytes, and ill-formed sequences no pattern accepts simply fail to
  match, which the `Tokenizer` reports as an unrecognized character at that position.

## **Hard Bounds**

- The compiled `Simulator` indexes states with `uint32_t`, so a lexer is limited to just under 2^32 DFA states. The
  constructor throws rather than truncate. On a 32-bit `std::size_t`, outside the supported 64-bit contract, the
  bound is smaller still: the transition table is states times symbol classes entries, and the constructor
  refuses a product that would not fit rather than allocate a wrapped size, a guard kept as defense in depth
  beyond the platforms CI tests. No realistic token set approaches this. A narrow index keeps entries small,
  which keeps realistic tables small enough to stay resident in cache, where the throughput comes from; it is not a
  guarantee, and a table with billions of states would not be resident at any entry width.
- Determinization has exponential worst cases, and a caller accepting untrusted token sets should cap it:
  `Builder::set_state_limit()` makes `build()` and `diagnose()` throw once subset construction discovers more states
  than allowed. It is specifically a DFA state cap, not a complete construction sandbox: regex tree size, NFA expansion
  (a huge `exact()` count builds its NFA before any limit is consulted), and the number of registered patterns are not
  bounded by it, and the transition table it indirectly bounds is the dominant but not the only allocation. The default
  is unlimited. Matching needs no such guard against the automaton itself: a compiled table cannot be made to backtrack
  the way a regex engine can, so no input triggers exponential matching.
- Longest match re-reads, so scanning is linear in the input only when tokens are short relative to the lookahead they
  force. After a failed longer match the scan resumes at the last accepting position, and a token set where a long
  pattern repeatedly fails back to a short one makes that quadratic: with the rules `a*b` and `a`, an input of 65536 `a`
  bytes takes 2.7 seconds, sixteen times the cost of 16384 bytes. Real token sets do not behave this way, because their
  long patterns (strings, comments, identifiers) either succeed or fail within a few bytes, but a caller accepting
  untrusted token sets should know the shape exists. Eliminating it needs a different algorithm, not a bigger table:
  Reps (TOPLAS 1998) gave a linear-time maximal-munch tokenizer by memoizing failures per position, and Li and Mamouras
  (PACMPL 2025) precompute per-position suffix information to the same end.
- A subexpression denoting the *empty language*, such as `any_of()` over an empty set, is legal to build and matches
  nothing. Determinization keeps the states leading into it, and minimization does not merge them, because the
  refinement runs over a partial transition function and a missing transition differs from one into a state that cannot
  accept. The pattern as a whole need not be empty for this: `choice(text("a"), concat(text("b"), any_of(Set{})))`
  matches only `"a"` and still keeps a reachable dead `b` branch. The result is therefore not state-minimal for such a
  token set, though it recognizes the same language; `is_split_point()` accounts for it by ignoring transitions that can
  never reach acceptance.
- A pattern matching the empty string is legal to build but needs care to run, and the three entry points differ.
  `tokenize_all()` and `tokenize_all_parallel()` treat a zero-length acceptance as no match and stop there, so the
  returned consumed count falls short of the input. `Lexer::tokenize()` returns the match as it is, with length zero, so
  a caller driving it in a loop must reject that or spin at one offset. The `Tokenizer` does exactly that, reporting a
  zero-width match as an error. `Mode_lexer` inherits the same split, and its two drivers therefore report a nullable
  token differently: the per-token entry point returns it with length zero, while the batch one stops without
  reporting it at all. The mode is the same either way, because `Mode_builder::build()` rejects a nullable token
  carrying an action outright: an action only one entry point would apply is not something a caller can act on. A
  nullable token without an action still builds, and still reports differently between the two.
- A `Tokenizer` holds the entire input in memory as one string; there is no chunked or incremental feeding, so inputs
  are bounded by memory. Where that matters, the way out is the layer below: `core::Lexer` matches over any iterator
  range without owning it, so a driver can do its own buffering and drive the lexer directly, provided each buffer ends
  on a token boundary.

## **Concurrency and Lifetimes**

- A `core::Lexer` is immutable after `Builder::build()` and safe to share across threads; `tokenize()` is `const` and
  touches no shared mutable state.
- `tokenize_all_parallel()` uses one execution thread per chunk, spawning N-1 workers and scanning the last chunk on the
  calling thread, and joins the workers before returning. The sink is invoked in input order within a chunk but
  concurrently across chunks, so it must tolerate concurrent calls for different chunk indices; per-chunk state indexed
  by the chunk achieves that without locking, and hot per-chunk accumulators belong on their own cache lines. There is
  no early-stop form, and a token set that certifies no split points yields a single chunk, i.e. the serial scan on the
  calling thread.
- A `Tokenizer` is a stateful cursor and is not thread-safe. Use one per thread, or one per input.
- `Token::lexeme()` is a `string_view` into the owning `Tokenizer`'s buffer, invalidated by `load()` or by the
  `Tokenizer`'s destruction. Copy it to a `std::string` if the token outlives either.
- The NFA and DFA builders offer both `build() const&`, which leaves the builder intact, and `build() &&`, which moves
  the accumulated automaton into the result; giving a builder up is an explicit choice at the call site.

## **Construction Cost**

`Builder::build()` runs the whole pipeline, so it is milliseconds where matching is nanoseconds: the benchmark's
keyword-scale token set, 143 patterns covering 100 keywords with identifier, literal, operator, and punctuation forms,
builds in about 21 ms into a 251-state DFA on the README's reference machine (`./build/tools/benchmark/munch_benchmark`
reports it as `build/keywords`), roughly doubling when identifiers admit every non-ASCII code point through
`utf8::range`. Compiling the tables out of the finished DFA contributes less than a tenth of a millisecond of that; the
cost is determinization, not the Simulator. It is a one-time cost. Build the lexer once and reuse it across inputs and
threads; building per input or per request is a design mistake the library does not try to make cheap.

## **The Escape Hatches**

Real languages exceed the model in places. The `Tokenizer` carries the primitives to handle them beside the automaton
instead of corrupting the engine:

- **Modes.** A `Tokenizer` can hold several lexers over one input and switch with `set_mode()`, the way a driver
  switches to a header-name lexer after `#include`. An unknown mode throws `std::out_of_range`.
- **Hand scanning.** `input()` exposes the buffer and `seek()` moves the cursor, so a driver can recognize a construct
  by hand and drop back into the automaton after it.
- **Raw string literals.** C++ bounds the raw-string delimiter to at most 16 characters, so the construct is regular in
  principle; a DFA covering every possible delimiter is simply impractical to build. `scan_raw_string()` therefore
  matches the delimiter by hand and returns the length for `seek()` to consume. The limit is engineering, not
  computability.

If a language needs more than these three hatches cover, that is the signal munch is the wrong tool for it, and the
honest answer is a hand-written lexer rather than a fight with the model.
