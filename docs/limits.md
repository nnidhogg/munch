# **Scope, Guarantees, and Limits**

[performance.md](performance.md) argues that munch is fast because of what it refuses to do. This note is the inventory:
the model the library commits to, the guarantees that commitment buys, the boundaries you can hit, and the escape hatch
for each. Read it when deciding whether munch fits a language, not after.

## **The Model: Regular Languages, Longest Match, Anchored**

munch recognizes exactly the regular languages its combinators can express, always by longest match from the current
position, with token priorities (lower value wins) breaking ties between patterns that accept the same lexeme. That
model excludes, deliberately and permanently:

- **Capture groups and submatch extraction.** A token is a kind and a lexeme. Structure inside the lexeme (the digits of
  a float, the body of a string) is the consumer's job, and the lexeme is right there to slice.
- **Backreferences and lookaround.** Both exceed regular power; supporting either would force a fundamentally slower
  matching engine on every token.
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

- There are no Unicode properties, no case folding, and no normalization. A case-insensitive keyword is spelled out
  (`choice(text("if"), text("IF"))` or a `Set` per position), and input is matched as the bytes it is.
- Standards-accurate Unicode identifiers (the XID_Start and XID_Continue properties) are a planned addition: helpers
  generated from the Unicode tables, expanding to `utf8::range` unions the way everything Unicode already does here.
  They extend the combinator vocabulary without changing anything that exists, so they are deliberately not gating 1.0;
  until they land, identifier classes beyond ASCII are spelled with `utf8::range` directly.
- Ill-formed UTF-8 in the input is not an error the engine detects; it is simply bytes no pattern matches, which the
  `Tokenizer` reports as an unrecognized character at that position.

## **Hard Bounds**

- The compiled `Simulator` indexes states with `uint32_t`, so a lexer is limited to just under 2^32 DFA states. The
  constructor throws rather than truncate. No realistic token set approaches this; the bound exists so the transition
  table stays cache-resident, which is where the throughput comes from.
- Determinization has exponential worst cases, and a caller accepting untrusted token sets should cap it:
  `Builder::set_state_limit()` makes `build()` and `diagnose()` throw once subset construction discovers more states
  than allowed. It is specifically a DFA state cap, not a complete construction sandbox: regex tree size, NFA expansion
  (a huge `exact()` count builds its NFA before any limit is consulted), and the number of registered patterns are not
  bounded by it, and the transition table it indirectly bounds is the dominant but not the only allocation. The default
  is unlimited. Matching itself needs no such guard: a compiled table cannot be made to backtrack, so no input, however
  crafted, can slow scanning beyond its linear pace.
- A pattern matching the empty string is legal to build but useless to run: the `Tokenizer` reports a zero-width match
  as an error rather than looping forever at one offset.
- A `Tokenizer` holds the entire input in memory as one string; there is no chunked or incremental feeding, so inputs
  are bounded by memory. Where that matters, the way out is the layer below: `core::Lexer` matches over any iterator
  range without owning it, so a driver can do its own buffering and drive the lexer directly, provided each buffer ends
  on a token boundary.

## **Concurrency and Lifetimes**

- A `core::Lexer` is immutable after `Builder::build()` and safe to share across threads; `tokenize()` is `const`
  and touches no shared mutable state.
- `tokenize_all_parallel()` spawns one thread per chunk and joins them all before returning. The sink is invoked in
  input order within a chunk but concurrently across chunks, so it must tolerate concurrent calls for different chunk
  indices; per-chunk state indexed by the chunk achieves that without locking, and hot per-chunk accumulators belong on
  their own cache lines. There is no early-stop form, and a token set that certifies no split points yields a single
  chunk, i.e. the serial scan on the calling thread.
- A `Tokenizer` is a stateful cursor and is not thread-safe. Use one per thread, or one per input.
- `Token::lexeme()` is a `string_view` into the owning `Tokenizer`'s buffer, invalidated by `load()` or by the
  `Tokenizer`'s destruction. Copy it to a `std::string` if the token outlives either.
- The NFA and DFA builders offer both `build() const&`, which leaves the builder intact, and `build() &&`, which moves
  the accumulated automaton into the result; giving a builder up is an explicit choice at the call site.

## **Construction Cost**

`Builder::build()` runs the whole pipeline, so it is milliseconds where matching is nanoseconds: the benchmark's
keyword-scale token set, 143 patterns covering 100 keywords with identifier, literal, operator, and punctuation forms,
builds in about 34 ms into a 251-state minimal DFA on the README's reference machine
(`./build/tools/benchmark/munch_benchmark` reports it as `build/keywords`), roughly doubling when identifiers admit
every non-ASCII code point through `utf8::range`. Compiling the tables out of the finished DFA contributes less than a
tenth of a millisecond of that; the cost is determinization, not the Simulator. It is a one-time cost. Build the lexer
once and reuse it across inputs and threads; building per input or per request is a design mistake the library does not
try to make cheap.

## **The Escape Hatches**

Real languages exceed the model in places. The `Tokenizer` carries the primitives to handle them beside the automaton
instead of corrupting the engine:

- **Modes.** A `Tokenizer` can hold several lexers over one input and switch with `set_mode()`, the way a driver
  switches to a header-name lexer after `#include`. An unknown mode throws `std::out_of_range`.
- **Hand scanning.** `input()` exposes the buffer and `seek()` moves the cursor, so a driver can recognize a construct
  by hand and drop back into the automaton after it.
- **Raw string literals.** `scan_raw_string()` implements the one C++ token no finite automaton can express, matching
  its delimiter by hand and returning the length for `seek()` to consume.

If a language needs more than these three hatches cover, that is the signal munch is the wrong tool for it, and the
honest answer is a hand-written lexer rather than a fight with the model.
