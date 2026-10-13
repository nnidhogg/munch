# **Performance**

The measured numbers: what `tools/benchmark` reports, how the runs are collected and archived, how the
parallel scan scales at certified bytes and at certified windows, and how munch compares with other
engines. [performance.md](performance.md) explains why the numbers come out as they do and
[design.md](design.md) the decisions behind that.

`core::Lexer::tokenize` advances the DFA through `dfa::Simulator::run`, which turns matching a character into one table
read on the hot path:

- **Symbol equivalence classes.** Two input bytes that the automaton never tells apart (e.g. two digits, in a lexer with
  no per-digit tokens) share one row of the transition table. The table therefore needs one row per class the automaton
  actually distinguishes rather than one per possible `char` value.
- **A flat `(class, state)` table**, viewed as a 2D `mdspan`, replaces the `unordered_map<(state, Label), state>` the
DFA itself is built and inspected through. The class of the next symbol is known before the current state is, so the
row offset is computed off the state-to-state dependency chain that would otherwise limit how fast `run()` can advance.
- **Narrow table entries** (`uint32_t` state indices, `uint8_t` class indices) keep more of the table resident in cache
  than the `size_t`-keyed hash map would.

The design rationale, i.e. why a library this small outruns engines orders of magnitude larger, is
written up in [docs/performance.md](performance.md); the architectural decisions behind it are
collected in [docs/design.md](design.md).

Measured with `tools/benchmark` (Release build, GCC 15.2 on an AMD Ryzen 9 9950X3D, Ubuntu 26.04) over generated
pseudo-code. Every scenario reports its best, median, and worst pass: the best estimates the least-interrupted cost of
the work, and the spread to the worst is the run-to-run variability, system interference plus, for the threaded
scenarios, thread creation and scheduling.

The scaling scenarios, the whole-input scans and the chunked rows, run in interleaved rounds rather than one scenario at
a time, so drift in clock or host load spreads across them instead of biasing the ratios between them. The size argument
accepts a comma-separated list to sweep input sizes (`16,128` crosses a typical last-level cache), and an optional third
argument writes every observation of the scaling scenarios to a CSV instead of keeping only the three summary figures.
The transcript below is abridged from the run archived as `paper/data/bare-metal-pinned-run2/`, which confined the
process to one L3 domain; `paper/data/README.md` describes all five measurements and what each can support.

To reproduce a run somewhere else, `tools/benchmark/collect.sh` builds the benchmark and records the result together
with the machine it ran on, which is what makes a number quotable later:

```bash
$ ./tools/benchmark/collect.sh ~/munch-run 1,16,128,512 15
```

It writes `environment.txt` (CPU, visible topology, governor, memory, kernel, toolchain, and whether a hypervisor is
present), `summary.txt`, and `observations.csv` with every timed pass of the scaling scenarios; the construction,
planning and thread-launch rows appear in the summary only. It checks its own prerequisites first: CMake 3.20.6+, a
C++23 compiler (GCC 13+ or Clang 19+), and git with network access on the first configure, since four header-only Boost
libraries and mdspan are cloned at pinned revisions. `-DUSE_SYSTEM_BOOST=ON` skips the Boost clones but not mdspan, so a
fully offline configure is not supported.

```
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
$ cmake --build build -j 8 --target munch_benchmark
$ ./build/tools/benchmark/munch_benchmark 1,16,128,512 15
lexer/ascii      1.0 MiB, 571472 tokens, 15 passes: best 661.5, median 652.4, worst 581.5 MiB/s
tokenizer/ascii  1.0 MiB, 571472 tokens, 15 passes: best 623.3, median 620.3, worst 611.1 MiB/s
build/keywords   143 patterns, 251 states, 15 passes: best 16.0, median 16.1, worst 16.1 ms
plan/frequent   8 of 8 chunks, 15 passes: best 0.1, median 0.1, worst 0.1 us
plan/rare       8 of 8 chunks, 15 passes: best 1020.6, median 1021.0, worst 1024.0 us
plan/absent     1 of 8 chunks, 15 passes: best 16352.0, median 16365.5, worst 17104.6 us
threads/8        spawn and join, 15 passes: best 36.4, median 39.7, worst 48.4 us
lexer_all/ascii  1.0 MiB, 571472 tokens, 15 passes: best 796.7, median 767.5, worst 753.9 MiB/s
...
scaling at 512 MiB, 15 interleaved rounds
lexer_all/ascii  512.0 MiB, 292637972 tokens, 15 passes: best 820.6, median 818.0, worst 751.2 MiB/s
lexer_all/source 512.0 MiB, 152160096 tokens, 15 passes: best 759.3, median 757.5, worst 736.0 MiB/s
chunked1/ascii   512.0 MiB, 292637972 tokens, 15 passes: best 754.1, median 730.1, worst 688.6 MiB/s
chunked1/source  512.0 MiB, 152160096 tokens, 15 passes: best 714.5, median 711.1, worst 693.0 MiB/s
chunked2/ascii   512.0 MiB, 292637972 tokens, 15 passes: best 1474.0, median 1434.2, worst 1341.8 MiB/s
chunked2/source  512.0 MiB, 152160096 tokens, 15 passes: best 1428.9, median 1415.0, worst 1368.7 MiB/s
chunked4/ascii   512.0 MiB, 292637972 tokens, 15 passes: best 2872.8, median 2828.0, worst 2645.4 MiB/s
chunked4/source  512.0 MiB, 152160096 tokens, 15 passes: best 2805.6, median 2763.0, worst 2666.3 MiB/s
chunked8/ascii   512.0 MiB, 292637972 tokens, 15 passes: best 5723.9, median 5568.3, worst 5233.0 MiB/s
chunked8/source  512.0 MiB, 152160096 tokens, 15 passes: best 5591.5, median 5500.5, worst 4791.7 MiB/s
```

The scenarios measure the core lexer called once per token on C-like source, the same input through the batch
`tokenize_all()` entry point, which stays in one call across token boundaries, the `Tokenizer` driver, identifiers
containing UTF-8 code points matched through byte expansion, the same input through the full Unicode XID identifier
classes (`lexer_all/xid` tracks `lexer_all/utf8` within noise: the property adds no Unicode-specific per-byte work, and
its runtime impact is limited to the resulting DFA and table size), the keyword-scale build cost, the XID construction
cost split into registration, finalization, and their total, the cost of planning chunk boundaries as certified bytes
grow scarce, and the parallel chunked scans at certified split points on two, four, and eight threads. Inputs are
fixed-seed and deterministic, so the bytes measured are identical across changes; that does not make timings comparable
across a changed executable. Numbers depend on the machine, the token set, and the compiler: Clang 19 measures within
about ten percent of GCC since the accept path was pinned to a branch (see [docs/performance.md](performance.md)).
Two collections on the same bare-metal machine, at two benchmark revisions, disagreed by 14% on the plain serial scan
and inverted its comparison with the one-chunk path, for reasons the report does not isolate. Rerun the benchmark on
your own hardware and language before citing these numbers.

## **Window-Planned Scaling**

The conventional token set of the windows study, identifiers, integers, strings, line comments, whitespace runs that
cross newlines, and single-byte operators, certifies no byte at all, so its parallel plan exists only through
`chunk_boundaries_with_windows()`: the two-byte window of a newline before an operator certifies at the operator. The
benchmark drives that planner on source-like text whose operator-initial lines put the certified window near every
equal-division target, plans inside every timed pass exactly as the byte rows replan on every call, and validates
before timing that the plan is complete (nine boundaries for eight chunks) and that the spliced chunk streams equal
the serial scan. The `window_plan` row prices planning alone, and the `windowed1` row runs the full path on a single
chunk, separating the plan-and-thread overhead from the parallelism the other rows add.

```
$ ./build/tools/benchmark/munch_benchmark 1,16,128,512 15
  commit      802fdf0
  system      Linux 6.18.33.2-microsoft-standard-WSL2 x86_64
window_plan/conv 1.0 MiB, 9 tokens, 15 passes: best 10754.8, median 10361.2, worst 9021.1 MiB/s
...
scaling at 512 MiB, 15 interleaved rounds
lexer_all/conv   512.0 MiB, 200780997 tokens, 15 passes: best 870.8, median 858.2, worst 847.7 MiB/s
windowed1/conv   512.0 MiB, 200780997 tokens, 15 passes: best 749.5, median 744.6, worst 740.7 MiB/s
windowed2/conv   512.0 MiB, 200780997 tokens, 15 passes: best 1501.6, median 1484.9, worst 1433.3 MiB/s
windowed4/conv   512.0 MiB, 200780997 tokens, 15 passes: best 2986.9, median 2951.8, worst 2858.9 MiB/s
windowed8/conv   512.0 MiB, 200780997 tokens, 15 passes: best 5488.9, median 5124.2, worst 4732.6 MiB/s
```

The full output and every observation are archived as `paper/data/windows-wsl-2026-08/`. This collection ran on the
development machine under WSL2, a different setup from the bare-metal archive the transcript above quotes, so ratios
compare within one collection and never across two: here the window plan turns an 858 MiB/s serial scan into 5124 MiB/s
at eight chunks. The plan row prices the eight-chunk plan of the one-mebibyte corpus at about a tenth of a millisecond;
the search is local to each equal-division target rather than a pass over the input, so the plan's share of the total
only falls as inputs grow. Two disclosures carry the numbers. The plan is a property of the token set together with the
input: this corpus is built so the certified window occurs near every target, and text without such shapes degrades
toward fewer chunks, never toward an unsafe cut. And the window guarantee is conditional on completely tokenizable
input, so byte planning remains the default everywhere and this section prices the explicit opt-in; see the planner
documentation for the exact contract.

## **Byte-Pair Chunk Plans**

`munch_planner_bench` measures what `verifier::chunk_plan()` is worth as a worker contract for byte-pair encoding, and
is written to answer either way. Per table and corpus it plans one chunk per byte at context budgets of 1, 2, 3, 4, 6
and 8 bytes and prints the cuts found as a share of the sequential encoding's boundaries, the median, 95th percentile
and longest gap between cuts, the windows decided against those asked, which is the cache's reuse, and the planning
time; then for two, four and eight chunks at a budget of four it prints the planning time, the time to decide the plan's
evidence windows again alone, the time of the workers, one thread each, with the stitching, the sequential time, the
end-to-end speedup, the imbalance and the fallback volume, the bytes scanned past their floors for want of a cut. Before
any figure is printed it holds its encoder, one pass per merge in rank order, to the leftmost-lowest encoder on a slice
of each corpus, every cut of every plan to the sequential boundaries, and on every timed run the stitched tokens to the
sequential encoding token for token, and any failure exits 1. The tables are the first 64 merges of GPT-2, read from the
token automaton fixture, whose origin radius is infinite; their prefix of 41 merges, the longest of finite radius,
origin radius 3 and right start radius 2, so its uniform window is six bytes wide; and the merges (a, b), (a, ab) over a
generated text of runs of a, where no window of a's is certified at its last byte. The merge tower of the 64 merges
plans the first 2 KiB of each corpus for the price of the verifier route. The corpora are `twitter.json`, simdjson's
benchmark document at release v3.10.1, 631,515 bytes, as the recovery and splitting campaigns read it, and munch's own
C++ sources under `libs`. These are raw tables over the whole text, without GPT-2's pre-tokenizer; that pipeline, its
token ids and its comparison with the production tokenizers belong to the production tokenizers' pre-tokenized
pipelines, outside this probe.

```
$ ./build/tools/probes/munch_planner_bench 100000000 5 - twitter.json libs
...
corpus twitter.json under gpt2-64: 631515 bytes, 576818 tokens, sequential 28.878 ms
  windows   budget 4:   576624 cuts, 100.0% of boundaries,   935.0 per KiB, gap median 1 p95 2 longest 4, 1681 decided of 938352 asked (reuse 99.8%), 70.1 ms
  windows   x8: 8 chunks, plan 0.012 ms, decided 11, evidence alone 0.005 ms, workers and stitching 4.457 ms, sequential 28.878 ms, speedup 6.46x, imbalance 1.00, fallback 1 bytes (0.0%)
  tower     x8: 8 chunks, plan 159.847 ms, decided 8, evidence alone 176.949 ms, workers and stitching 0.136 ms, sequential 0.089 ms, speedup 0.000554x, imbalance 1.00, fallback 0 bytes (0.0%)
corpus twitter.json under gpt2-41: 631515 bytes, 592205 tokens, sequential 18.651 ms
  windows   budget 4:   592184 cuts, 100.0% of boundaries,   960.2 per KiB, gap median 1 p95 2 longest 4, 1358 decided of 905565 asked (reuse 99.9%), 66.0 ms
  uniform   budget 4:   592204 cuts, 100.0% of boundaries,   960.3 per KiB, gap median 1 p95 2 longest 4, 1358 decided of 903986 asked (reuse 99.8%), 72.5 ms
corpus libs under gpt2-64: 1675954 bytes, 1306957 tokens, sequential 93.525 ms
  windows   budget 4:  1298882 cuts,  99.4% of boundaries,   793.6 per KiB, gap median 1 p95 2 longest 4, 4712 decided of 3133831 asked (reuse 99.8%), 256.6 ms
  windows   x8: 8 chunks, plan 0.020 ms, decided 16, evidence alone 0.006 ms, workers and stitching 14.256 ms, sequential 93.525 ms, speedup 6.55x, imbalance 1.00, fallback 4 bytes (0.0%)
  tower     x8: 8 chunks, plan 299.502 ms, decided 17, evidence alone 181.848 ms, workers and stitching 0.190 ms, sequential 0.097 ms, speedup 0.000323x, imbalance 1.00, fallback 2 bytes (0.1%)
corpus generated runs of a under ab-aab: 65536 bytes, 60452 tokens, sequential 0.151 ms
  windows   budget 4:     5041 cuts,   8.3% of boundaries,    78.8 per KiB, gap median 3 p95 41 longest 46, 12 decided of 271763 asked (reuse 100.0%), 3.9 ms
  uniform   budget 4:    60450 cuts, 100.0% of boundaries,   944.5 per KiB, gap median 1 p95 1 longest 3, 12 decided of 254442 asked (reuse 100.0%), 6.6 ms
  windows   x8: 8 chunks, plan 0.015 ms, decided 9, evidence alone 0.009 ms, workers and stitching 0.259 ms, sequential 0.151 ms, speedup 0.549x, imbalance 1.00, fallback 129 bytes (0.2%)
```

This run was made on the development machine under WSL2 with GCC 13, so its figures compare within it, and its excerpt
keeps each corpus's rows at a budget of four bytes and at eight chunks; the timings vary from run to run on that
machine, the counts do not. They show a speedup, and its cause. On raw GPT-2 merges the certified windows are dense on
actual text although the origin radius of the 64 merges is infinite: at a budget of four bytes they give 576,624 cuts in
the 576,818 tokens of `twitter.json` and 99.4% of munch's sources, no gap longer than four bytes, from fewer than 5,000
distinct windows, every other ask answered by the cache. So the supply never limits a plan, an eight-chunk plan costs
about a hundredth of a millisecond, the imbalance is 1.00 and the fallback a few bytes, and eight workers encode the
text 6.5 times faster than one; the plan being negligible, the shortfall from eight lies in the workers and the
stitching themselves, which the probe does not break down. The prefix of 41 merges, whose uniform window cuts at every
byte, adds the last few boundaries the windows miss and nothing a plan of eight chunks needs. The verifier route does
not pay: on the merge tower each window costs tens of milliseconds, two searches over the product with its 257 states,
so the eight-chunk plan of a 2 KiB slice takes longer than encoding all of `twitter.json`; the token automaton decides
its windows in microseconds. Where windows are scarce the uniform window is what bounds the gap: over the runs of a,
windows of four bytes cut 8.3% of the boundaries with gaps as long as a run, the uniform window gives 60,450 cuts in the
60,452 tokens with no gap longer than three bytes; that text is too small for its timing to mean anything.

## **Lean 4 on Mathlib**

`munch_benchmark_lean4` scans real Lean source with the bundled Lean 4 grammar, `tools/audit/grammars/lean4.g4`, read
from its ANTLR file as `munch-audit` reads it, every mode compiled and every `pushMode` and `popMode` made the action of
its token, so the row times the scan whose cuts [A Modal Grammar: Lean 4](audit.md#a-modal-grammar-lean-4) audits. The
corpus is Mathlib's `Mathlib/Lean` and `Mathlib/Logic` directories at leanprover-community/mathlib4 commit
`2da3ab0a20dc0c66f56a557e37df39f7d36cb953`, 81 `.lean` files concatenated in sorted path order into one file of 652,998
bytes, whose SHA-256 is `2f05b11dba835ad0d3f8db31869b034e9a0f75a2c33796289643da97c71ec708`. It is not redistributed
here, and is fetched, joined and checked with

```
git clone --depth 1 --filter=blob:none --sparse https://github.com/leanprover-community/mathlib4 /tmp/mathlib-corpus
git -C /tmp/mathlib-corpus sparse-checkout set Mathlib/Lean Mathlib/Logic
git -C /tmp/mathlib-corpus checkout 2da3ab0a20dc0c66f56a557e37df39f7d36cb953
(cd /tmp/mathlib-corpus && find Mathlib/Lean Mathlib/Logic -name '*.lean' | LC_ALL=C sort | xargs cat) \
    > /tmp/mathlib.lean
sha256sum /tmp/mathlib.lean
```

A depth-one clone fetches the branch tip, so if the tip has moved past the pinned commit, deepen the fetch with `git -C
/tmp/mathlib-corpus fetch --depth 100 origin` before the checkout, or drop `--depth` entirely. Before any clock starts,
the benchmark scans the file once and requires that the scan consume it whole and end with the mode stack empty, naming
the offset and the mode where a scan stops short, and every timed pass must consume it whole and reproduce the first
pass's token count.

```
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
$ cmake --build build -j 8 --target munch_benchmark_lean4
$ ./build/tools/benchmark/munch_benchmark_lean4 tools/audit/grammars/lean4.g4 /tmp/mathlib.lean 15
Lean 4 grammar over a corpus
...
lean4, 652998 bytes in 5 modes, 15 passes
lexer_all/lean4  0.6 MiB, 202003 tokens, 15 passes: best 380.3, median 376.8, worst 355.5 MiB/s
```

This run was made on the development machine under WSL2 with GCC 13, a different setup from the bare-metal archive the
first transcript quotes, so its figures compare within it and never with that archive. The 202,003 tokens count the
discarded ones too, whitespace and comment text, about 3.2 bytes per token. The row times the serial modal scan and
plans no chunks, since a cut needs the mode known and the default mode certifies one window alone.

## **Comparison with Other Engines**

`munch_benchmark_modes` measures the modal driver across the axes that separate its paths: an action that never fires
against a grammar with no actions at all, the batch entry point against the per-token one, how often an action token
occurs, how long the free-content runs between actions are, `go_to` against `push`/`pop`, and mode-stack depth. Run it
when the action lookup changes; a single corpus hides which path an edit helped, and one edit measured +11% on one row
and -13% on another.

```console
$ ./build/tools/benchmark/munch_benchmark_modes 4 15
```

Configuring with `-DMUNCH_BENCHMARK_COMPARE=ON` additionally builds `munch_benchmark_compare`, which runs the same
tokenization job through six engines: lexertl17 (the C++17 line of lexertl), munch's nearest relative, a lexer likewise
built at run time from rules and compiled to a DFA, and five widely used regex engines, used the way one uses a regex
engine to write a lexer: one pattern with an alternation per token kind, matched anchored at the current offset. Each
engine's full tokenization, every token's kind and length, is validated to agree with munch's before anything is timed.
That agreement is a property of these corpora, not of the semantics: first-match alternation can stop before the longest
match in general, as a token pair like `if`/`ifx` shows, so the validation is what licenses the comparison; the timed
passes then run a lightweight recognition tally of token count and kinds. The option is off by default because it
fetches the engines as additional dependencies.

Two corpus shapes keep the conclusions honest: `dense`, averaging under two bytes per token, magnifies per-token
overhead, and `source`, shaped like real code with long identifiers and indentation, amortizes it.

Unlike the transcript above, these comparison figures were not re-measured on the bare-metal machine: they are the older
run on an Intel i9-12900K under WSL2 with GCC 13.3, and only the ratios between engines on one machine are meaningful.
Rebuild and rerun before quoting any absolute number here. The Rust dependencies are pinned by the committed
`Cargo.lock`, but the Rust compiler version behind this transcript was not recorded and no shared environment transcript
covers the C++ and Rust binaries together, so the cross-language comparison is looser evidence than the split-point
measurements, whose raw runs are archived under `paper/data/`.

```
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMUNCH_BENCHMARK_COMPARE=ON
$ cmake --build build -j 8 --target munch_benchmark_compare
$ ./build/tools/benchmark/munch_benchmark_compare 16 15
corpus dense: 1.83 bytes per token
munch            16.0 MiB, 9144476 tokens, 15 passes: best 577.0, median 563.6, worst 555.1 MiB/s
munch-mt4        16.0 MiB, 9144476 tokens, 15 passes: best 2035.0, median 1973.4, worst 1474.1 MiB/s
munch-mt8        16.0 MiB, 9144476 tokens, 15 passes: best 3755.4, median 3410.3, worst 2445.8 MiB/s
lexertl          16.0 MiB, 9144476 tokens, 15 passes: best 175.7, median 173.2, worst 168.6 MiB/s
ctre             16.0 MiB, 9144476 tokens, 15 passes: best 421.4, median 414.6, worst 374.9 MiB/s
pcre2-jit        16.0 MiB, 9144476 tokens, 15 passes: best 79.9, median 78.2, worst 77.3 MiB/s
re2              16.0 MiB, 9144476 tokens, 15 passes: best 11.2, median 11.0, worst 10.8 MiB/s
boost-regex      16.0 MiB, 9144476 tokens, 15 passes: best 15.5, median 15.3, worst 15.0 MiB/s
std-regex        16.0 MiB, 9144476 tokens, 15 passes: best 10.9, median 10.9, worst 10.8 MiB/s
corpus source: 3.53 bytes per token
munch            16.0 MiB, 4755600 tokens, 15 passes: best 563.8, median 547.6, worst 529.0 MiB/s
munch-mt4        16.0 MiB, 4755600 tokens, 15 passes: best 2075.6, median 2018.1, worst 1868.8 MiB/s
munch-mt8        16.0 MiB, 4755600 tokens, 15 passes: best 3873.3, median 3444.5, worst 2997.5 MiB/s
lexertl          16.0 MiB, 4755600 tokens, 15 passes: best 223.6, median 220.5, worst 218.4 MiB/s
ctre             16.0 MiB, 4755600 tokens, 15 passes: best 575.3, median 564.8, worst 544.6 MiB/s
pcre2-jit        16.0 MiB, 4755600 tokens, 15 passes: best 139.8, median 137.1, worst 133.6 MiB/s
re2              16.0 MiB, 4755600 tokens, 15 passes: best 19.1, median 18.9, worst 18.7 MiB/s
boost-regex      16.0 MiB, 4755600 tokens, 15 passes: best 28.4, median 28.1, worst 27.8 MiB/s
std-regex        16.0 MiB, 4755600 tokens, 15 passes: best 16.4, median 16.2, worst 16.1 MiB/s
```

The same job runs through Rust's lexer class in `tools/benchmark/rust`, whose corpus generators are byte-identical ports
and whose tally matches the one above, so the numbers are comparable across the two binaries. It measures logos, the
code-generating lexer, and the dense DFAs of regex-automata, the nearest Rust relative of munch's runtime-construction
class, both through its search API and steelmanned through its low-level automaton walk:

```
$ cd tools/benchmark/rust && cargo run --release -- 16 15
corpus dense: 1.83 bytes per token
logos            16.0 MiB, 9144476 tokens, 15 passes: best 728.0, median 691.5, worst 665.3 MiB/s
logos-mt4        16.0 MiB, 9144476 tokens, 15 passes: best 2708.0, median 2640.2, worst 2307.8 MiB/s
logos-mt8        16.0 MiB, 9144476 tokens, 15 passes: best 5148.9, median 4742.7, worst 3283.3 MiB/s
regex-automata   16.0 MiB, 9144476 tokens, 15 passes: best 168.2, median 166.5, worst 155.2 MiB/s
regex-automata-raw 16.0 MiB, 9144476 tokens, 15 passes: best 349.1, median 346.9, worst 340.9 MiB/s
corpus source: 3.53 bytes per token
logos            16.0 MiB, 4755600 tokens, 15 passes: best 891.1, median 858.8, worst 833.5 MiB/s
logos-mt4        16.0 MiB, 4755600 tokens, 15 passes: best 3462.0, median 3290.0, worst 2854.9 MiB/s
logos-mt8        16.0 MiB, 4755600 tokens, 15 passes: best 6141.5, median 5487.2, worst 4131.6 MiB/s
regex-automata   16.0 MiB, 4755600 tokens, 15 passes: best 222.3, median 219.7, worst 215.8 MiB/s
regex-automata-raw 16.0 MiB, 4755600 tokens, 15 passes: best 399.1, median 392.8, worst 352.4 MiB/s
```

The engines fall into three groups, and each group is in the comparison to answer a different question.

**The same class: lexers constructed at run time.** These share munch's contract, a token set supplied as data while the
program runs, so they are the alternatives munch directly competes with, and the group the claim of fastest in its class
is measured against. regex-automata appears twice deliberately: once through its search API as a user would call it, and
once steelmanned through its low-level automaton walk, so the claim holds against the best configuration measured here
rather than its friendliest entry point.

Every cell in the four tables below is the **best** of 15 passes, the statistic the transcripts above report first; the
raw blocks carry the medians and the spread beside it. The paper's scaling tables quote **medians** instead, so a figure
from here and a figure from there are not the same statistic and should not be set side by side.

| Engine                 | Version   | Matching approach                         | dense | source |
|------------------------|-----------|-------------------------------------------|------:|-------:|
| `munch`                | this repo | table-compiled minimized DFA, single pass |   577 |    564 |
| `regex-automata` (raw) | 0.4       | dense DFA walked at the automaton level   |   349 |    399 |
| lexertl17              | 652435f   | rules compiled to a DFA                   |   176 |    224 |
| `regex-automata`       | 0.4       | dense DFA through its search API          |   168 |    222 |

**The class above: compile-time code generation.** These freeze the token set at build time and emit code shaped like
it, the one advantage a runtime-built table cannot take, so this group does not measure a competition munch can enter;
it measures the price of munch's flexibility. Across measurement sessions on the reference machine, that price has
ranged from under ten percent to roughly a quarter against logos on dense input, and from forty to sixty percent on
source-shaped input: gaps between separately built binaries move with machine state, which the best-to-worst spread in
the raw output makes visible. Membership in this class is not sufficient to win, though: munch outruns CTRE outright on
the dense corpus, because CTRE compiles the regex structure into code and still resolves the alternation between token
kinds per token, while munch's determinized table erased the alternation before the first byte arrived. Only when longer
tokens let generated code consume multi-byte runs does CTRE pull ahead.

| Engine  | Version   | Matching approach                         | dense | source |
|---------|-----------|-------------------------------------------|------:|-------:|
| logos   | 0.15.1    | matcher generated by a derive macro       |   728 |    891 |
| `munch` | this repo | table-compiled minimized DFA, single pass |   577 |    564 |
| CTRE    | 3.9.0     | matcher generated from the regex          |   421 |    575 |

**The industry defaults: general-purpose regex engines.** This is what a codebase typically reaches for when it needs a
tokenizer without adopting a lexer library, so the group measures what that convenience costs. The gap is not a defect
in these engines: they solve a far broader problem, searching, captures, backreferences, and they pay for that
generality on a workload of anchored matches every couple of bytes.

| Engine       | Version        | Matching approach                         | dense | source |
|--------------|----------------|-------------------------------------------|------:|-------:|
| `munch`      | this repo      | table-compiled minimized DFA, single pass |   577 |    564 |
| PCRE2        | 10.44          | backtracking, JIT-compiled                |    80 |    140 |
| Boost.Regex  | 1.86.0         | backtracking                              |    16 |     28 |
| RE2          | 2024-07-02     | leaves the DFA to extract captures        |    11 |     19 |
| `std::regex` | libstdc++ 13.3 | backtracking                              |    11 |     16 |

With the input chunked at safe split points and scanned with one thread per chunk, for the two lexer classes fast enough
for threading to matter:

| Engine  | Threads | dense | source |
|---------|--------:|------:|-------:|
| logos   |       8 |  5149 |   6142 |
| `munch` |       8 |  3755 |   3873 |
| logos   |       4 |  2708 |   3462 |
| `munch` |       4 |  2035 |   2076 |

Throughputs in MiB/s. The medians tell the same story throughout: no ranking above changes if the median is substituted
for the best pass.

Read the numbers for what they measure. The corpus averages under two bytes per token, so per-token overhead dominates:
munch tokenizes the whole input through `tokenize_all()`, CTRE compiles the token set into a matcher at C++ compile
time, and the general-purpose engines re-enter a full match API for every token (PCRE2 through its dedicated JIT entry
point, `pcre2_jit_match`). RE2 does not use its DFA to report capture positions at all: it picks among three submatch
engines, `SearchOnePass` when the program is one-pass and the match is anchored, `SearchBitState` for small enough
subtexts, and `SearchNFA` otherwise. These matches are anchored, so the cost here is leaving the DFA path rather than
falling all the way to the NFA. Both RE2 and PCRE2 are also designed for searching long texts, not for anchored matches
every couple of bytes. The comparison pattern orders keywords before identifiers and multi-character operators before
their prefixes, and the full-stream validation confirms that on these corpora the engines with first-match alternation
semantics produce exactly munch's longest-match, priority-resolved tokenization. Ordering alone does not guarantee that
in general: with `if` ordered ahead of identifiers, first match splits `ifx` where longest match does not.

Read as classes, the two corpora say one thing together. munch is the fastest among the run-time-built lexers measured
here, on both, roughly 1.4 to 3.3 times ahead of its nearest relatives even with regex-automata steelmanned through its
low-level walk, the narrow end being that steelman on the source-shaped corpus. The compile-time code generators own the
overall lead as tokens grow longer: logos on both corpora and CTRE on source, because generated code consumes multi-byte
runs where a table walk pays one dependent load per byte, which is also why munch's own throughput is nearly identical
on both corpus shapes. [docs/performance.md](performance.md) explains why that boundary is where it is.

Threading multiplies the class verdict rather than reordering it: both lexer classes scale strongly, close to linear at
four threads and sublinearly at eight, so the serial ranking carries over at every width. What the threaded rows
actually compare is how each side knows its chunk boundaries are safe. munch certifies them from the compiled transition
table, for whatever token set was built, through `is_split_point()`; the logos rows rest on a hand-written analysis of
this one token set, documented in the Rust driver, which logos itself can neither produce nor check. On a token set
where no byte is safe, such as one with string literals, munch reports that no split points exist, while a hand analysis
has to notice by itself that the trick is no longer sound.
