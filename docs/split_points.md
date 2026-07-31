# Certified Split Points: Parallel Lexing Without Speculation

**Nicklas Nidhögg**, July 2026. Describes munch v1.0.0.

*A technical report on the mechanism behind `Lexer::is_split_point()`, `chunk_boundaries()`, and
`tokenize_all_parallel()`. The implementation, tests, and benchmarks live in this repository; this document states the
idea precisely, relates it to prior work, and reports where it applies.*

## Abstract

Scanning with a DFA is serial by construction: each transition depends on the state the previous byte produced, so one
input admits no instruction-level shortcut past roughly one load latency per byte. The standard escape is to split the
input into chunks and scan them concurrently, but a chunk's first byte arrives with the automaton state unknown, so
existing approaches either simulate from every state and merge (simultaneous automata), guess a state and patch up
mispredictions (speculation), or overlap chunks and verify convergence. This report describes a fourth approach
implemented in munch: derive, from the compiled automaton of the token set itself, a set of *certified split symbols*,
bytes at which, on a completely tokenizable input, every occurrence begins a token. Splitting immediately before such a
byte preserves the token stream exactly, by construction, with no speculation, no overlap, no merge phase, and no
speculative or duplicate tokenization when the property does not hold: the plan degenerates to a serial scan. We state
the certificate and its one subtlety (a re-entrant initial state invalidates the exemption that makes it usable), show
that deriving it is a linear-time analysis of the compiled table, measure near-linear scaling to four threads on a 16
MiB corpus, and survey which grammars certify usable symbols. The survey grounds a piece of folklore: for conventional
tokenizations, line-based splitting of source text is sound when no token can span a line, and one token kind that can,
the block comment, is alone sufficient to destroy every useful certificate in the surveyed C-like grammar.

## 1 The problem

A table-compiled scanner executes, per input byte, one transition: `state = table[row(byte) + state]`. The load that
produces the next state cannot begin before the previous state is known, so the scan is a serial dependency chain and
its throughput is bounded by the load-to-use latency of the cache holding the table. munch's serial loop sits at that
floor (see [performance.md](performance.md)), which means further speedup on one input must come from scanning several
regions of it at once.

Splitting is the obstacle. A scanner dropped at an arbitrary offset does not know the automaton state there: the offset
may fall inside a string, halfway through an identifier, or in the middle of a multi-byte operator. Any tokenization
computed from a wrong entry state is garbage until the scanner happens to resynchronize, and whether and when it
resynchronizes depends on the automaton and the input.

## 2 Prior approaches

Published solutions accept the unknown-state problem and manage it:

- **Simulate from all states and compose.** Simultaneous finite automata (Sin'ya et al.) run each chunk from every
  possible state, producing a state-to-state function per chunk; the functions compose associatively, so chunks
  combine in a parallel-prefix step. General, but the per-chunk work multiplies with the state count and a merge
  phase remains.
- **Speculate and patch.** Data-parallel FSMs (Mytkowicz et al., ASPLOS 2014) and enumerative speculation (Qiu et
  al., PPoPP 2017; SimdFSM) start each chunk from one or a few guessed states, exploiting the empirical observation
  that real automata converge quickly. Correctness is recovered through enumeration, validation, or re-execution;
  what remains uncertain is how much redundant work the recovery costs, and with it the speedup. Reduced-interface
  DFAs (Borsotti et al.) shrink the set of entry states worth retaining.
- **Restrict the automaton.** Holub and Štekr's parallel DFA run is exact and efficient for *k-local* automata,
  where any k consecutive symbols force a unique state regardless of the start. The property is uniform over the
  whole automaton and most lexical grammars do not have it.
- **Overlap and verify.** Lossless parallel tokenization for LLM vocabularies (LoPT, 2025) splits at fixed offsets
  with overlap regions and verifies that adjacent chunks agree inside the overlap.
- **Folklore delimiters.** Data systems split logs and CSV at newlines because "records do not contain newlines",
  adjusting the cut to the next delimiter (the widow/orphan pattern). The assumption is per-format, informal, and
  famously unsound for CSV with quoted newlines, which is why speculative CSV parsing exists as a research topic
  (Ge et al., SIGMOD 2019). Format-specific structural scans (simdjson, Mison, Parabix) hand-derive comparable
  facts per format.

What none of these do is ask the token set's own automaton *which bytes are safe*.

## 3 The certificate

The setting is a lexer in the usual longest-match loop: the scanner starts in the initial state, consumes bytes
recording the last accepting position, emits the token found there, and restarts in the initial state at the next byte.
The composite system is therefore not just a DFA run; it is a DFA run that *resets at every token boundary*. The
certificate exploits exactly that reset.

**Definition.** A byte b is a *certified split symbol* of a compiled token set when no state consumes it except,
possibly, a non-re-entrant initial state. "Consumes" means the state has an outgoing transition on b; "re-entrant"
means some nonempty input returns the automaton to the initial state; since every compiled state is reachable,
that is equivalent to the initial state having an incoming transition in the table.

**Theorem.** Let an input be completely tokenizable by the serial longest-match scan. Splitting it immediately before
any occurrences of certified split symbols and scanning the chunks independently, each from the initial state,
produces chunk streams whose ordered concatenation is exactly the serial token stream.

*Proof.* Consider an occurrence of a certified symbol b at offset i. Suppose, for contradiction, that i is not a token
boundary of the serial scan. Then b is consumed after a nonempty prefix of some token; let q be the automaton state
immediately before consuming b. Since q consumes b and b is certified, q can only be the initial state, and that
exemption is available only while the initial state is non-re-entrant. But q was reached after a nonempty prefix, so the
initial state would be re-entrant, a contradiction; and a non-initial q consuming b contradicts certification directly.
So every occurrence of b begins a token, the serial scan is in the initial state exactly at i, and that is precisely the
entry condition of the chunk that starts there. Induction over the chunks gives stream equality. ∎

**Corollary (malformed input).** If the serial scan fails at some offset, every chunk before the failing one is fully
consumed with an identical stream, and the failing chunk stops at the same relative offset; the concatenated parallel
stream therefore has the serial stream as a prefix, but later chunks may independently emit tokens beyond the
failure. A caller must check every chunk's consumed length, which the parallel API returns per chunk, before treating
the concatenated output as a successful tokenization.

**The subtlety.** The initial state is exempted from "no state consumes b" because a token may legitimately begin with
b. That exemption is only sound while no input can *return* the automaton to the initial state mid-scan. A nullable
pattern breaks it: `kleene(text("a"))` minimizes to an accepting initial state with a self-loop on
`a`, the initial state is re-entrant, and the naive certificate wrongly certifies `a`; splitting `aa` then changes the
token stream. munch shipped exactly this bug and an external review found it: the fix conditions the exemption on the
initial state having no incoming table entry, and the regression suite carries the counterexample and a cyclic `(ab)*c`
re-entry case. The condition is not a refinement for completeness; without it the certificate is unsound.

**Vacuous certificates.** A byte no state consumes at all is certified vacuously: no completely tokenizable input can
contain it, so the theorem's implication holds trivially. On malformed input, both the serial scan and the chunk
beginning at such a byte fail there, while later chunks may still emit tokens exactly as the malformed-input corollary
describes. Vacuous symbols are counted by the implementation but useless for planning; the useful certified set is the
intersection with bytes that can actually appear in valid input.

## 4 Deriving and using it

The certificate is derived by a linear-time analysis of the compiled transition table at construction time: one scan
detects initial-state re-entry, then a 256-by-state sweep certifies each byte value. The cost is `O(states × symbols)`
once, amortized into a build that already did strictly more work to exist. The scanner's inner loop is untouched.

The public surface is three functions. `is_split_point(byte)` reports the certificate. `chunk_boundaries(input, chunks)`
is the pure planner: it aims for equal divisions and slides each interior boundary forward to the next certified byte,
returning offsets; when nothing certifies, it returns one chunk. `tokenize_all_parallel(input, chunks, sink)` executes
the plan, one thread per chunk, and for completely tokenizable input the theorem guarantees that concatenating the
per-chunk streams gives the serial stream. There is no speculation to retry, no overlap to verify, and no merge beyond
concatenation.

The planner's cost deserves stating: for k requested chunks it performs at most k - 1 forward searches for a certified
byte, so its worst case is `O(kN)` over an input of N bytes when certified occurrences are rare, though k is normally a
small hardware-thread count and the searches start at equally spaced offsets. When the certificate holds no byte at all,
the planner returns the single whole-input chunk without scanning. A one-pass planner would reduce the worst case to
`O(N + k)`.

## 5 What certifies in practice

The certificate asks the grammar for cooperation, and the interesting question is how much real grammars give. The
following table was produced by compiling each token set with munch and reading `is_split_point` for all 256 bytes (the
probe procedure and token grammars are described in the appendix). "Useful" excludes vacuously certified bytes.

| Token set                                               | Useful certified bytes             |
|---------------------------------------------------------|------------------------------------|
| C-like: identifiers, numbers, ws runs, operators, punct | all operator and punctuation bytes |
| the same plus strings (no raw newline inside)           | none                               |
| the same plus `//` line comments                        | none                               |
| the same plus `/* */` block comments                    | none                               |
| JSON (ASCII strings, ws runs)                           | none                               |
| log lines (`[^\n]+` and `\n`)                           | `\n`                               |
| C-like, split-friendly tokenization (see below)         | `\n`                               |
| the same plus block comments                            | none                               |

Two mechanisms explain the collapses:

- **Free-content tokens absorb the alphabet.** A string literal that may contain `(` makes `(` a mid-token byte,
  de-certifying it everywhere. One token kind with a near-total interior alphabet removes almost every candidate.
- **Run tokens de-certify their own bytes.** With whitespace tokenized as `[ \t\n]+`, the second newline of a blank line
  is consumed by the run's continuation state, so `\n` itself is mid-token-consumable and uncertified. This is easy to
  miss: the byte's *own* token kills it.

Both mechanisms suggest the same design lever: certification is a property of the *tokenization*, not the recognized
language, and the tokenization can be refactored without changing what is recognized. The "split-friendly" row keeps the
identical C-like language but tokenizes newline as its own single-byte token, leaves spaces and tabs as whitespace runs,
and keeps strings and comments line-bounded. Then `\n` certifies, and the folklore rule follows as a practical
corollary:
**for conventional tokenizations in which newline is its own token, line-based splitting is sound when no other token
can contain a newline.** The automaton-level statement remains the exact one: "no token spans a line" is sufficient here
but not necessary in general, since a token that merely begins with newline leaves the certificate intact (newline is
then consumed only from the initial state). Adding block comments, the one C token that spans lines, destroys the
certificate again, which is the formal shape of both "you cannot chunk C by lines" and the quoted-newline problem that
pushed CSV parsing into speculation.

## 6 Evaluation

The following figures come from `paper/data/benchmark.txt`, an archived run whose header records the machine, compiler,
and command, and whose commit is discoverable from the file's own history: GCC 13.3 at -O2 on Ubuntu 24.04 under WSL2,
Intel i9-12900K, over 16 MiB fixed-seed corpora, reporting best, median, and worst of 15 passes. Citing the archive
rather than the README's table keeps an ordinary benchmark refresh from silently changing what this report claims. The
serial batch scan reaches 599.8 MiB/s median on the dense corpus; the certified chunked scan reaches 1084.0 MiB/s on two
threads, 2164.9 on four, and 3496.0 on eight, with the source-shaped corpus scaling comparably (1060.6, 2067.2, and
3650.0). That is 90% per-core efficiency at four threads for a plan whose certificate is derived by a linear-time table
analysis at build time, followed by one boundary search per requested interior boundary at scan time. The planner's own
cost is measured across certificate densities in the same archive: 0.2 microseconds when certified bytes are common, 1.9
ms when they are a megabyte apart, and 30.6 ms in the worst case of a certified byte absent from a 16 MiB input, the two
scanning cases agreeing on 1.8 GiB/s, which is consistent with the O(kN) bound.

Correctness is enforced at three levels. The benchmark proves the chunked token stream identical to the serial one by
exact (kind, length) comparison before any timing. The unit suite carries the certification counterexamples, including
the re-entrant-initial-state case. And the fuzzer generates arbitrary grammars and inputs, checks every planned boundary
against `is_split_point`, and compares the concatenated parallel stream with the serial stream on every execution;
roughly half a million generated cases have run without a violation.

## 7 Limitations and future work

The approach trades generality for certainty: when the grammar does not cooperate, it offers no usable split points, by
design, and the speculation and composition families remain the only options. Several extensions look natural. A
*conditional* certificate over symbol pairs or short windows ("`)` followed by `\n`") would recover splitting for
grammars where no single byte certifies. A hybrid plan could split at certified bytes where they exist and fall back to
speculative entry elsewhere, keeping the guarantee where it is free and paying for it only where it is not. Finally, the
grammar-refactoring lever of Section 5 could be automated: given a token set, propose the minimal trivia re-tokenization
that makes a chosen byte certify.

## 8 Related-work summary

A certified split symbol is not a synchronizing word in the classical sense (Volkov's survey covers that theory): a
synchronizing word maps every state to one common state, whereas a certified symbol is one that no reachable mid-token
state may consume at all, so the surrounding longest-match loop must already have finished its token and returned to the
initial state before the symbol arrives. The reset comes from the token loop, not from the automaton's transition
structure.

Certified split points differ from simultaneous automata (no per-state simulation, no composition), from speculation (no
guess, no re-run, exact by construction), from k-locality (the property is per-symbol and derived from the token loop's
reset, not uniform over the automaton), from overlap verification (no overlap), and from delimiter folklore (the safe
set is derived from the compiled token set rather than assumed per format, and the derivation correctly refuses grammars
where the folklore is unsound). To our knowledge the specific formulation, a per-symbol certificate extracted from the
compiled automaton of a longest-match token set, with the re-entrant-initial-state condition, has not been published,
though the components (synchronization, delimiter splitting, chunked scanning) are all classical.

## References

- R. Sin'ya, K. Matsuzaki, M. Sassa. *Simultaneous Finite Automata: An Efficient Data-Parallel Model for Regular
  Expression Matching.* ICPP 2013. arXiv:1405.0562.
- T. Mytkowicz, M. Musuvathi, W. Schulte. *Data-Parallel Finite-State Machines.* ASPLOS 2014.
- J. Qiu et al. *Combining SIMD and Many/Multi-core Parallelism for Finite State Machines with Enumerative Speculation.*
  PPoPP 2017.
- J. Holub, Š. Štekr. *On Parallel Implementations of Deterministic Finite Automata.* CIAA 2009.
- C. Ge, Y. Li, et al. *Speculative Distributed CSV Data Parsing for Big Data Analytics.* SIGMOD 2019.
- E. Stehle, H.-A. Jacobsen. *ParPaRaw: Massively Parallel Parsing of Delimiter-Separated Raw Data.* VLDB 2020.
- G. Langdale, D. Lemire. *Parsing Gigabytes of JSON per Second.* VLDB Journal 2019 (simdjson).
- Y. Li et al. *Mison: A Fast JSON Parser for Data Analytics.* VLDB 2017.
- R. D. Cameron, K. S. Herdy, D. Lin. *High Performance XML Parsing Using Parallel Bit Stream Technology.*
  CASCON 2008 (Parabix).
- W. Shao, L. Zheng, P. Wang, P. Zheng, J. Li, Y. Fan. *LoPT: Lossless Parallel Tokenization Acceleration for Long
  Context Inference of Large Language Model.* 2025. arXiv:2511.04952.
- M. V. Volkov. *Synchronizing Automata and the Černý Conjecture.* LATA 2008.
- A. Borsotti, L. Breveglieri, S. Crespi Reghizzi, A. Morzenti. *Minimizing Speculation Overhead in a Parallel
  Recognizer for Regular Texts.* 2024.
  arXiv:2412.14975.
- R. Voetter. *Parallel Lexing, Parsing and Semantic Analysis on the GPU.* MSc thesis, Leiden University, 2021.

## Appendix: the applicability probe

The table in Section 5 is reproducible with a short program against the public API: build each token set with
`core::Builder`, then read `Lexer::is_split_point()` for every byte value. The grammars are those listed, with
`Set::all()` interiors for strings and comments and the block comment written as
`/* ( [^*] | *+ [^*/] )* *+ /`.
