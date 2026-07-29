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

Per input byte, the work is one 256-entry offset read, one transition read, one accept-table probe, and two compares. No
allocation, no dispatch, no interpretation, and no data-dependent branching beyond the dead-state exit and the accept
check.

## **Data Beats Code**

CTRE, the closest competitor in the README's benchmark, also compiles the token set ahead of time, but it compiles the
*regex structure* into code: the alternation over token kinds still exists at runtime as branches to try, and which
alternative matched must be discovered per token. Determinization erases the alternation entirely. A data-driven table
walk has no alternation branches to mispredict, and the equivalence classes keep the table small enough that memory
latency does not take back what branch elimination won.

## **The Theory Is Old; the Discipline Is the Feature**

None of this is novel. Determinization is Rabin and Scott (1959), the NFA construction is Thompson (1968), and "a
minimal DFA is the optimal single-pass recognizer for a regular language" is textbook. What this library does is
implement the sixty-year-old right answer without compromise, which is only possible by refusing every feature that
would break the model: no captures, no backreferences, no unanchored search. Where a real language genuinely exceeds
regular power, such as C++ raw string literals, the answer is a hand-written scanner beside the automaton (the
tokenizer's `seek()` escape hatch and `scan_raw_string()`), never a compromise inside the engine. That refusal is the
performance. The full inventory of what the library refuses, guarantees, and provides escape hatches for is in
[limits.md](limits.md).
