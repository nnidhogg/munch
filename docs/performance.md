# **Why Munch Is Fast**

The [README's Performance section](../README.md#performance) documents the mechanisms and the measured numbers. This
note explains why a library this small can outrun engines orders of magnitude larger: it gets to cheat in a way
general-purpose engines cannot. It knows the entire matching problem before the first byte of input arrives, and it
spends all of its complexity at build time so that almost nothing is left to do per byte. The size is not a paradox; it
is the mechanism.

## **The Problem Is Radically Narrower**

General regex engines solve a huge problem: capture groups, backreferences, lookarounds, Unicode properties, unanchored
searching, submatch extraction. Those features force runtime interpretation of pattern structure, and every one of them
costs something at every match call. munch solves exactly one problem: longest-match, anchored, byte-oriented
tokenization with a token set fixed before matching begins. The code paths that make general engines slow at lexing
simply do not exist here. In the README's comparison, where the corpus averages under two bytes per token, most of the
gap to the general-purpose engines is *them paying for generality*, re-entering a full match API at every token
boundary.

## **Every Decision Is Made Once, in build()**

The pipeline (Thompson construction, subset construction, minimization, run per pattern and then once more for the
combined lexer) is where alternation, ambiguity, and priority get dissolved rather than deferred. After the final
determinization there is no "try the keyword branch, then the identifier branch": all registered token patterns are
matched simultaneously by one automaton, so adding tokens costs nothing at match time. Priorities are baked into the
accept table; longest-match is just "remember the last accepting state and keep reading." Construction is a one-time
cost, paid when the lexer is built and never again.

## **The Hot Loop Is Engineered Down to Its Serial Dependency Chain**

A DFA's speed is fundamentally limited by one thing: the next state depends on the current state, a serial chain of
dependent table reads. Everything else in the simulator is arranged to stay off that chain:

- **Symbol equivalence classes** collapse the 256 possible byte values into the handful of columns the automaton
  actually distinguishes, so the whole transition table stays cache-resident.
- **The row-per-class layout** makes the row offset depend only on the input byte, which is known before the current
  state is. The CPU computes the offset in parallel with the previous transition instead of after it.
- **Narrow table entries** (`uint32_t` states, `uint8_t` classes) double what fits in each cache level.

Per input byte, the work is one 256-entry offset read, one transition read, a one-byte accept-flag probe, and two
compares; the matched token itself is resolved once after the scan. No
allocation, no dispatch, no interpretation, and no data-dependent branching beyond the dead-state exit and the accept
check.

## **Data Beats Code**

CTRE, the closest competitor in the README's benchmark, also compiles the token set ahead of time, but it compiles the
*regex structure* into code: the alternation over token kinds still exists at runtime as branches to try, and which
alternative matched must be discovered per token. Determinization erases the alternation entirely. A data-driven table
walk has no alternation branches to mispredict, and the equivalence classes keep the table small enough that memory
latency does not take back what branch elimination won.

## **What Beats It, and What That Buys**

Lexer-specialized code generators beat this design, and the margin grows with token length. logos, the Rust lexer
generator, is measured in tools/benchmark/rust on byte-identical ports of the README's two benchmark corpora,
validated to produce the same token stream to the token: it runs ten to thirty percent ahead of the whole-input
tokenize_all() entry point, and CTRE overtakes on the source-shaped corpus as well; re2c occupies the same class
for C. munch's own throughput is nearly identical on both corpus shapes, which is the table model's signature: it
pays per byte, so token length neither helps nor hurts it, while generated code consumes multi-byte runs. Within
munch's own class the lead survives falsification: even with regex-automata steelmanned through its low-level
automaton walk rather than its search API, munch measures one and a half to nearly two times ahead, and further
ahead of lexertl. They win by escaping the one cost the
table model cannot shed: a table walk performs one dependent load per input byte, a serial chain of L1 latencies,
while generated code fuses multi-byte consumption into the matcher, comparing whole keywords at once and eating
identifier runs without a per-byte state step.

That chain is untouchable from inside the loop, and this was measured rather than assumed: packing the accept flag
into the transition entry put one extra mask on the chain and lost about ten percent, and consuming self-loop runs
against a per-state bitmask lost far more, because realistic runs are a few bytes long and every run ended in a
mispredicted exit branch. Closing the gap for real would take SIMD classification inside the run consumer or a
code-generating backend, and both trade away the property this design exists for: the code generators freeze the
token set at compile time, while munch builds lexers at run time from ordinary values, which is what lets a driver
hold several lexers as modes, and lets a language be defined by data rather than by a build step.

## **The Remaining Headroom Is Parallel**

The two-corpus comparison measured the bound precisely: munch's throughput is the same at under two bytes per token
and at three and a half, because the cost is one dependent table load per byte, a single serial chain of L1
latencies that nothing inside the loop may touch. The chain cannot be shortened; what remains open is running more
than one. Several cursors stepping independent chunks of one large input in a single interleaved loop would overlap
their latency chains on one core, and threads would scale the same chunking across cores for batch work; the input
is read far below memory bandwidth today, so the ceiling is genuinely unclaimed.

Chunks must begin at real token boundaries, and in this design that is not a heuristic but a property the automaton
can certify at build time: a byte that no state except the start state consumes can only ever begin a token, so
every occurrence is a safe split point, computable by one pass over the transition table. A token set whose strings
or comments can contain any byte certifies no safe points, and the right behavior is to refuse and scan
sequentially rather than speculate, in keeping with [limits.md](limits.md). None of this is built; it is recorded
here because it is the one identified improvement that does not fight the serial chain, and because the decision it
depends on belongs, like every other decision in this library, at build time.

## **The Theory Is Old; the Discipline Is the Feature**

None of this is novel. Determinization is Rabin and Scott (1959), the NFA construction is Thompson (1968), and "a
minimal DFA is the optimal single-pass recognizer for a regular language" is textbook. What this library does is
implement the sixty-year-old right answer without compromise, which is only possible by refusing every feature that
would break the model: no captures, no backreferences, no unanchored search. Where a real language genuinely exceeds
regular power, such as C++ raw string literals, the answer is a hand-written scanner beside the automaton (the
tokenizer's `seek()` escape hatch and `scan_raw_string()`), never a compromise inside the engine. That refusal is the
performance. The full inventory of what the library refuses, guarantees, and provides escape hatches for is in
[limits.md](limits.md).
