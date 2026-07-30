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
compares; the matched token itself is resolved once after the scan. No allocation, no dispatch, no interpretation, and
no data-dependent branching beyond the dead-state exit and the accept check.

## **Data Beats Code**

CTRE, the closest competitor in the README's benchmark, also compiles the token set ahead of time, but it compiles the
*regex structure* into code: the alternation over token kinds still exists at runtime as branches to try, and which
alternative matched must be discovered per token. Determinization erases the alternation entirely. A data-driven table
walk has no alternation branches to mispredict, and the equivalence classes keep the table small enough that memory
latency does not take back what branch elimination won.

## **What Beats It, and What That Buys**

Lexer-specialized code generators beat this design, and the margin grows with token length. logos, the Rust lexer
generator, is measured in tools/benchmark/rust on byte-identical ports of the README's two benchmark corpora, validated
to produce the same token stream to the token: it runs ten to thirty percent ahead of the whole-input tokenize_all()
entry point, and CTRE overtakes on the source-shaped corpus as well; re2c occupies the same class for C. munch's own
throughput is nearly identical on both corpus shapes, which is the table model's signature: it pays per byte, so token
length neither helps nor hurts it, while generated code consumes multi-byte runs. Within munch's own class the lead
survives falsification: even with regex-automata steelmanned through its low-level automaton walk rather than its search
API, munch measures one and a half to nearly two times ahead, and further ahead of lexertl. They win by escaping the one
cost the table model cannot shed: a table walk performs one dependent load per input byte, a serial chain of L1
latencies, while generated code fuses multi-byte consumption into the matcher, comparing whole keywords at once and
eating identifier runs without a per-byte state step.

That chain is untouchable from inside the loop, and this was measured rather than assumed: packing the accept flag into
the transition entry put one extra mask on the chain and lost about ten percent, and consuming self-loop runs against a
per-state bitmask lost far more, because realistic runs are a few bytes long and every run ended in a mispredicted exit
branch.

The SIMD form of that run consumer was then built, measured, and reverted as well. The build certified at construction
time every state whose self-loop bytes form at most four contiguous ranges, probed ahead of the cursor to filter out
short runs, and classified sixteen bytes per vector compare to find each run's end, with the token stream proven
identical by the test suite. It lost on both corpora however the attempt was triggered: fired on every byte of an
accelerated state it measured 202.4 MiB/s dense and 366.2 source against 651.7 and 629.0 serial, and fired once per run
entry it recovered only to 241.6 and 398.2. The arithmetic behind the failure is structural rather than a tuning miss:
on input averaging under two bytes per token, roughly seventy percent of tokens enter an accelerable state, so the
attempt runs about four times per ten bytes, and any trigger that reads data before deciding, whether the acceleration
entry or a probe byte, hangs a late-resolving data-dependent branch on nearly every token. The mispredictions cost more
than the skipped table steps return. Hyperscan and regex-automata ship exactly this acceleration, but certify it only
for states whose loop sets span nearly the whole byte alphabet, string literal and comment bodies, where every run is
structurally long and the short-run gamble cannot arise; a lexical token set of identifiers, numbers, and whitespace
contains no such state. The conclusion is the same one the interleaving experiment reached from the other side: the
table model wins by making no data-dependent bets, and every acceleration is a bet.

What could still close the gap is a code-generating backend, and that trades away the property this design exists for:
the code generators freeze the token set at compile time, while munch builds lexers at run time from ordinary values,
which is what lets a driver hold several lexers as modes, and lets a language be defined by data rather than by a build
step.

## **The Remaining Headroom Is Parallel**

The two-corpus comparison measured the bound precisely: munch's throughput is the same at under two bytes per token and
at three and a half, because the cost is one dependent table load per byte, a single serial chain of L1 latencies that
nothing inside the loop may touch. The chain cannot be shortened; what remains open is running more than one. Several
cursors stepping independent chunks of one large input in a single interleaved loop would overlap their latency chains
on one core, and threads would scale the same chunking across cores for batch work; the input is read far below memory
bandwidth today, so the ceiling is genuinely unclaimed.

Chunks must begin at real token boundaries, and in this design that is not a heuristic but a property the automaton can
certify at build time: a byte that no state except the start state consumes can only ever begin a token, so every
occurrence is a safe split point, computable by one pass over the transition table. The start state's exemption holds
only while no transition re-enters it, which a nullable pattern's self-looping start state does; the exemption then
certifies nothing, and only bytes no state consumes remain vacuously safe. A token set whose strings or comments can
contain any byte certifies no safe points, and the right behavior is to refuse and scan sequentially rather than
speculate, in keeping with [limits.md](limits.md). The certification is built:
Lexer::is_split_point() reports the certified bytes of a compiled token set, computed by that one pass in the
simulator's constructor.

The threaded chunking is measured, and the prediction registered here before the experiment held. The benchmark splits
the input at the certified points nearest the equal-division offsets, runs one whole-input scan per chunk on its own
thread, and first proves the chunked token stream identical to the serial one by an exact (kind, length) stream comparison before timing, keeping only a light tally as a consistency signal in the timed passes. On
the benchmark token set, chunking scales the serial 651.7 MiB/s to 1161.8 MiB/s on two threads, 2292.9 MiB/s on four,
and 3834.7 MiB/s on eight, with the source-shaped corpus within a few percent of the dense one at every width
(`cmake -B build-perf -DCMAKE_BUILD_TYPE=Release -DMUNCH_BUILD_BENCHMARK=ON && cmake --build build-perf && ./build-perf/tools/benchmark/munch_benchmark 16 15`).
That is 88% per-core efficiency at four threads against the 70% bar this section committed to in advance, and the shape
confirms the diagnosis: the chains overlap almost perfectly because each thread's working set is a cache-resident table
plus a streamed slice of input. The chunking has since been promoted into the library: chunk_boundaries() computes the
certified plan and tokenize_all_parallel() runs it, one thread per chunk with the last on the calling thread, and the
benchmark now routes through that entry point. The plan computation stays pure, so a caller owning its own thread pool
can take the boundaries and leave the library's threads unused.

The single-core variant was then built, measured, and reverted, and its failure taught more than the threaded success.
The experiment flattened the scan into a per-byte state machine and stepped several lanes per loop iteration, each lane
a chunk of the same input at the same certified boundaries, predicting that the lanes' per-byte load chains would
overlap in one core's pipeline. Measured on the source corpus against a 623.1 MiB/s serial scan, one lane ran at 373.7
MiB/s, two lanes at 372.8, four at 193.5, and the dense corpus fared worse still, so the code was removed under the
two-times-or-revert criterion this section registered in advance. The control at one lane is the first lesson: the
serial loop is fast because token-end handling sits outside the byte loop, and flattening it into per-byte checks costs
forty percent before any interleaving begins. The flat lane count curve is the second and larger one: the prediction
modeled the scan as one long dependency chain, but the scan resets its state to a constant at every token start, which
cuts the chain, so on inputs averaging a few bytes per token the out-of-order window already overlaps several tokens'
chains in the plain serial loop. The instruction-level parallelism the lanes were meant to create was already being
harvested, and paying structure, register pressure, and branch-history interference to recreate it can only lose.
Threads scale where lanes could not because each core brings its own registers and its own branch predictor, not just
its own execution ports.

The engine comparison confirms the threaded picture across implementations rather than just within munch. With both
lexer classes chunked the same way and validated token for token against their serial scans (`munch_benchmark_compare`
and the Rust driver, 16 MiB, best of fifteen), munch goes from 624.2 MiB/s serial to 2302.0 on four threads and 3284.5
on eight on the dense corpus, and from 604.5 to 2342.3 and 3792.9 on the source corpus, while logos goes from 680.2 to
2576.9 and 4594.0, and from 844.4 to 3253.8 and 5628.6. Both classes scale strongly, close to linear at four threads and
sublinearly at eight, so threading multiplies the serial verdict instead of reordering it, and the meaningful difference
between the rows is epistemic: munch's chunk boundaries are certified by is_split_point() from the compiled table for
whatever token set was built, while the logos rows rest on a hand-written safety analysis of this one token set that the
generated lexer can neither produce nor check.

Measuring this added one mechanism worth recording. The first version of the threaded comparison scenario let each
worker update its slot in a shared array of sixteen-byte tallies once per token, which put four workers' write targets
on one cache line; the false sharing cost about a third of the four-thread scaling, 1505.9 MiB/s against the 2302.0
measured after each worker accumulated locally and stored its tally once. A parallel scan is only as scalable as its
least private write, and a sink that writes shared memory per token quietly rejoins the threads the chunking was meant
to separate.

## **The Theory Is Old; the Discipline Is the Feature**

None of this is novel. Determinization is Rabin and Scott (1959), the NFA construction is Thompson (1968), and "a
minimal DFA is the optimal single-pass recognizer for a regular language" is textbook. What this library does is
implement the sixty-year-old right answer without compromise, which is only possible by refusing every feature that
would break the model: no captures, no backreferences, no unanchored search. Where a real language genuinely exceeds
regular power, such as C++ raw string literals, the answer is a hand-written scanner beside the automaton (the
tokenizer's `seek()` escape hatch and `scan_raw_string()`), never a compromise inside the engine. That refusal is the
performance. The full inventory of what the library refuses, guarantees, and provides escape hatches for is in
[limits.md](limits.md).
