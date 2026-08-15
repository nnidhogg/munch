# Certified Split Windows for Parallel Lexing: Recovering Boundaries Where No Byte Certifies

**Nicklas Nidhögg**, August 2026. Mirrors `paper/split-windows/split-windows.tex`, published as
[arXiv:2608.09761](https://arxiv.org/abs/2608.09761), which evaluates munch at the v1.3.3 release, where the window
machinery is a probe: the paper's principle is that the certificate's formulation should freeze there before becoming a
public contract. The supported API, `Lexer::is_split_window()` and `chunk_boundaries_with_windows()`, arrived in release
1.4.0, after the paper's submission, and lies outside its evaluation. The mandatory core section is this document's
addition alone: release 1.5.0 derives that machinery on top of the same certificates, and no version of the paper
contains it. Every empirical aggregate below is printed and asserted by the probes, so a drifted number fails the test
suite.

*A technical report on the certificate behind the window layer. The implementation, tests, and probes live in this
repository; this document states the idea precisely, relates it to prior work, and reports what it recovers.*

## Abstract

A certified split point lets a parallel lexer cut unlexed input at a single byte with the serial token stream provably
preserved, but several conventional token sets in the predecessor's controlled study certify no byte once string,
comment, or whitespace-run forms are included ([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)). This report
generalizes from a byte to a bounded window: a byte string after which the position where the current token began is
known, regardless of surrounding context. The certified form is the directly usable one: the token covering the
window's final byte begins at the reported origin. The certificate is conditional on occurrence and may be vacuous;
every applicability figure counts only windows carrying an asserted completely tokenizable occurrence witness. A
conservative model of a maximal-munch scanner's possible histories across a window is given and proved sound, and
reachability in that model is decided exactly by exhausting a finite quotient of its reachable configurations, so
every answer of the unbudgeted procedure is either a certified window with its origin or a proof that the model admits
none. Within the stated flat, non-nullable, completely-tokenizable scope, model-positive answers are semantic
certificates; negatives are relative to the conservative model, which deliberately refuses some windows a greedy
scanner would allow. In a sample of 400 random token sets, 91 of the 95 non-nullable sets certifying no byte gain a
witnessed window, with zero inconclusive searches, and every exact-empty row of the predecessor's study gains a
witnessed window of two to four bytes. Rewind-stress rows exercised 1,079,392 executions that scanned through the
window and contained at least one rewind, with zero disagreements against the shipped scanner. The analysis runs once
after automaton construction, using only the compiled tables and no input.

## 1 The problem

The predecessor report derives, from a compiled token set, the complete set of bytes at which unlexed input can be cut
with the serial token stream preserved, and proves the condition necessary as well as sufficient. Its sharpest
limitation is its own applicability table: several of the studied conventional token sets certify *no* byte, because a
single string form, comment form, or whitespace run gives some non-initial live state a transition on every candidate,
and a fresh 400-grammar random sweep generated for this study reproduces the pattern, 95 of its 134 non-nullable sets
certifying none. This report is about the object that refusal leaves standing: a byte *string* after which the start
of the token covering the window's final byte is pinned, regardless of surrounding context.

Concretely: for a token set compiled to a DFA and scanned by maximal munch, is there a window `W = w0 ... w(k-1)` and
an offset `o` with `0 <= o < k` such that in *every* completely tokenizable input containing `W`, the token covering
the occurrence's final byte begins exactly `o` bytes into it? A worker that finds `W` in a completely tokenizable
input may then begin scanning at the recovered boundary with no speculation, no state enumeration, and no
DFA-state-recovery pass over the input, exactly as at a certified byte, which is the `k = 1` case.

The analysis is offline: certificates and model-search results are decided from the compiled tables after automaton
construction, before any input exists. The occurrence witnesses and scanner checks are generated executions against
the shipped scanner.

## 2 Certified split windows

The scanner model is inherited from the predecessor. A token set compiles to a DFA with initial state q0; scanning is
by maximal munch: from the current position the scanner runs the automaton as far as a transition exists, emits the
token of the last accepting configuration passed, resumes immediately after it, and restarts in q0. An input is
*completely tokenizable* when this process consumes it exactly. The setting is *flat*: one fixed token set compiled to
one DFA scanned from one fixed initial state, with no lexical modes, no mode stack, and no semantic scanner state. The
live subautomaton retains the states both reachable from q0 and co-accessible to acceptance, with only transitions
between live states. No token matches the empty string; this exclusion is load-bearing and revisited in the
limitations.

**Definition (certified split window).** Let `W = w0 ... w(k-1)` with `k >= 1` and let `o` be an offset with
`0 <= o < k`. The pair `(W, o)` is a *certified split window* for a token set when, for every completely tokenizable
input containing `W` at offset `t`, the token of the maximal-munch tokenization that contains the byte at offset
`t + k - 1` begins at `t + o`.

**Definition (witnessed window).** A certified split window is *witnessed* when some completely tokenizable input
contains `W`.

The first definition quantifies over the inputs containing `W` and is therefore vacuously true when no completely
tokenizable input contains it, and the vacuity is not hypothetical: over `{0, 00, 01}` the model below certifies the
window `1001` at origin 2, yet no completely tokenizable input contains `1001`, since every `1` is the tail of a
greedily chosen `01`, a token boundary therefore follows the window's first byte, maximal munch must then consume
`00`, and the final `1` sits at a boundary no token starts. That argument proves non-occurrence; the artifact asserts
the model prediction and the empty result of its bounded targeted witness search. Every applicability figure in this
report counts only witnessed certificates: the named rows pin their witness inputs in the artifact, and for each
random grammar the probe constructs and verifies a concrete completely tokenizable input; a certificate the bounded
witness search cannot witness is reported as unresolved, never as a rescue.

For `k = 1` the definition is the boundary guarantee of a certified split point, since the token containing the single
byte begins at it. Three guarantees should be kept apart, and this report certifies the strongest: model unanimity
implies that the covering token's origin is fixed, which implies that some fixed safe boundary exists inside the
window, and neither converse holds. Over `{a, ab, b}` at `ab` the covering origin is fixed yet the model refuses
(Section 7); over `{a, abx, b, x}` at `ab` the byte `a` always begins a token, so offset 0 is a fixed safe boundary,
yet the covering token's origin is not fixed, since the final `b` belongs to `b` in the input `ab` and to `abx` in
`abx`. The weakest guarantee is already usable, since a worker can cut at the known boundary and rescan the window's
suffix; the covering-token form is the sufficient, deliberately stronger property this forward origin-recovery model
certifies, and it hands the worker its resumption point directly. The model therefore has two distinct sources of
false negatives: covering-origin certification is stricter than locating some safe boundary, and the cloud below
conservatively represents extra segmentations and may refuse even a true covering-origin certificate. The definition
also says nothing about the tokens overlapping the window's earlier bytes, and it does not require the boundary to be
the only one inside the window.

## 3 The cloud model

A worker cutting blind knows only that in the final segmentation, the window's first byte is consumed from *some* live
token-prefix state, either inside a token that began at some unknown earlier offset or exactly at a boundary; the
acceptance-gated seed below carries the boundary case, including a window at the start of the input. The model tracks
a *cloud* of hypotheses `(q, origin)`: a live state paired with an origin, either *before* for a token that began
before the window or an in-window offset. The initial cloud `C0` is every live state paired with *before*.

Reading window byte `w_j` maps `C_j` to `C_(j+1)` by two rules:

- **Direct step.** A pair whose state consumes `w_j` into a live state moves there with its origin unchanged. A pair
  whose state cannot consume `w_j` into a live state is an impossible history and is dropped, never restarted.
- **Acceptance-gated seed.** If some pair in `C_j` is accepting, one fresh pair `(delta(q0, w_j), j)` is seeded,
  provided that target is live: a token can begin at offset `j` only where the previous token could have ended or at
  the input start, the case the initially open gate represents, and tokens end only where the automaton accepts.

One rename accompanies the direct step: where q0 is not re-entrant in the live subautomaton, a pair stepping *from* q0
is beginning a token, so its origin becomes the current offset; where a non-empty live path returns to q0, the rename
is disabled, since reaching q0 then no longer identifies a boundary. This is the same re-entrancy condition the
length-one certificate carries.

Clouds are *sets* of pairs; the set semantics is load-bearing below, where two rules inserting the same pair yield one
element. The window is *certified at origin o* when `C_k` is non-empty and every pair of `C_k` carries the same
in-window origin `o`; an empty cloud certifies nothing, matching the implementation, which refuses it, and the empty
cloud is absorbing. Non-emptiness guarantees nothing in the other direction: a certified window may still be vacuous,
which is the separation the witnessed definition exists to make. Agreement on the state alone is not enough: learning
that the scan is inside a string literal is knowledge, but not a boundary.

**The discarded predecessor.** A natural variant restarts a trajectory that cannot consume the byte, treating the
failure point as a token boundary, in place of the acceptance-gated seed. That variant is not merely unproved but
refuted. Over the token set `{a, abc, bx, x}` and window `abx`, its cloud after `ab` is the single pair carrying
origin 0 toward `abc`; `x` kills that trajectory, the restart replaces it with a token beginning at offset 2, nothing
else survives, and the variant certifies `(abx, 2)`. Yet the input `abx` itself is completely tokenizable as `a`
followed by `bx`, so the token covering the final byte begins at offset 1: the certificate is false at a witnessed
occurrence. A failing trajectory is an impossible history, not a boundary, and restarting it manufactures support for
origins no execution justifies. The repaired model certifies `abx` at origin 1, where the scanner does cut, and the
case is carried as an asserted row of the artifact.

## 4 Soundness

Fix a completely tokenizable input containing `W` at offset `t`. For each window position `j`, take the start of the
token that the final maximal-munch segmentation assigns to the byte at `t + j - 1`, the automaton state of that
token's prefix after consuming through that byte, and the corresponding origin, in-window or *before*.

**Lemma (representation).** For every `j`, that actual state-and-origin pair is in `C_j`.

*Proof sketch.* Every such state is live: reachable through the actual token prefix, and co-accessible because its
token ends at some later offset along an accepting path. In the base case, a token begun before the window enters
`C_1` by the direct step from its pre-window prefix state, which sits in `C0` paired with *before*, and the rename
cannot interfere: that state has consumed at least one byte, so it equals q0 only if a non-empty live path returns to
q0, which disables the rename. A token begun at the window's first byte enters by the seed, whose gate is open because
`C0` contains a live accepting state. In the step case, a byte that continues its token is carried by the direct step
with origin unchanged, and a byte that begins a token has a predecessor token ending exactly there, so the pair
representing that predecessor is accepting, the gate opens, and the seed emits exactly the new token's pair. ∎

The lemma is containment, not equality: the cloud may carry hypotheses no execution realizes. That is harmless in one
direction and load-bearing in the other: at a realized occurrence in a completely tokenizable input, surplus
hypotheses can only preserve the actual origin's unanimity or destroy unanimity; they cannot manufacture unanimity at
a false origin.

**Theorem (soundness).** If `C_k` is non-empty and every pair of `C_k` carries the same in-window origin `o`, then
`(W, o)` is a certified split window.

*Proof.* By the representation lemma the actual pair is in `C_k`, so its origin is `o`, so the covering token begins
at `t + o`, a token start of the final segmentation; the input was an arbitrary completely tokenizable one containing
`W`. ∎

**Backup never appears.** Maximal-munch lookahead that a later rewind discards occupies states the lemma says nothing
about; the invariant tracks where the *final* segmentation's tokens begin, not where the read head wanders. A boundary
that a rewind later exposes was seeded by the model step reading the first byte after the accepting position
justifying it. This holds when lookahead crosses the window's left edge, when several accepting positions are passed,
and across chains of consecutive rewinds; each is an instance of the step case.

## 5 The finite quotient and the decision procedure

Acceptance gating alone does not terminate: over `a+` the automaton accepts after every byte and origins accumulate
without bound. The search therefore deduplicates on a quotient of the cloud: the set `B` of states carrying *before*,
and for each state the number of distinct in-window origins it carries, saturated at two.

**Invariant (single occupancy).** In every reachable cloud, each surviving in-window origin occupies at most one
state.

An origin enters the cloud at most once per rule application, into a single state, and the deterministic step moves it
to at most one successor, so it continues in one state or dies. At offset `j` the acceptance seed and the
non-re-entrant-q0 rename can both propose the fresh origin `j`; both target exactly `delta(q0, w_j)`, and set
semantics coalesces the identical pair, so single occupancy survives the collision. Origins may die, which is why the
invariant says at most one rather than exactly one. The pre-window origin is deliberately different: it may occupy
many states and is held apart as the Boolean support set `B`. An arbitrary cloud could violate the invariant, placing
one origin in two states, and there the saturated counts could not decide unanimity; no such cloud is reachable, and
the restriction is load-bearing for everything below. The key of a cloud is the pair of `B` and the per-state
saturated origin count.

**Lemma (congruence, depth-aligned).** Two search clouds with equal keys have, on every byte, successors that are
either both empty or both non-empty with equal keys and agreeing certification verdicts. For successor-key equality
alone it is enough that the inserted offsets be *fresh*, exceeding every in-window origin of their respective clouds;
the certification verdict additionally needs legal depths, and the search supplies both, since it steps with the
actual word lengths and every origin in a length-l cloud lies below l. Freshness is not decorative: over `{a+, b}` the
cloud after `a` carries origin 0, and stepping on `a` with offset 0 merges the seed into that origin while offset 1
creates a second one, so the same key would have two different successors under arbitrary offsets.

The proof reads everything the step needs from the key: the acceptance gate from the occupied states, the new
*before*-support as the live deterministic image of `B` with q0's contribution excluded exactly when the rename fires,
and the successor counts as sums of arriving origins, which come from distinct predecessors by single occupancy, plus
at most one fresh origin, with the seed and the rename coalescing because they denote the same pair. Saturation
commutes with the update in the only direction needed, no rule ever reads an origin's value, only equality between
origins, and certification of the full window is the key predicate that `B` is empty with total count exactly one.

One tempting simplification is false and worth flagging: it is not true that a state holding two origins can never
contribute to unanimity later. Over `{a+, b}`, after `aa` the `a`-state carries two in-window origins as well as
*before*; on `b` all of them die while their pre-step acceptance opens the gate for one fresh `b`-origin, and the
cloud is unanimous. What is true, and what the lemma uses, is that co-located origins either follow the same live
successor together or all die, and that origins which die leave no trace the key must distinguish.

**Theorem (decision procedure).** Breadth-first search over windows, deduplicated on keys, decides whether the model
certifies any window for a given token set, and returns the minimum certified length; it does not enumerate every
certified word. Each state contributes a factor of 2 × 3 to the key space, so at most `6^n` keys are ever retained
over `n` live states; each key expands into at most 256 successors, each computable from the key in `O(n)` time, for a
worst case of `O(256 · n · 6^n)` time and `O(n · 6^n)` space.

By the congruence lemma the walk cannot miss the existence of a certifying continuation, nor change the minimum
certified length: after one matched step both successor clouds sit at fresh depths again, so the lemma inducts over
every common suffix; breadth-first order retains a shallowest representative of each key; and a certifying suffix from
a discarded deeper occurrence therefore yields an equal or shorter certificate from the retained representative.
Deduplication may still skip individual certified words; exhaustion proves the model admits none at any length. What
exhaustion does not give is semantic non-existence, because the model itself is conservative (Section 7). The probe
realizes the procedure with one concrete representative cloud and word per key, using the quotient for deduplication,
so its cost additionally depends on representative size. It is budgeted, with three outcomes: certified, exhausted,
and inconclusive once the retained key count exceeds a fixed threshold of 200,000 keys; the evaluation below reports
zero inconclusive searches, and the retained key counts it reports are one indicator of the search footprint rather
than a complete cost model.

## 6 At length one: exactly the shipped predicate

At length one the model collapses to the published certificate, and the correspondence is exact at the level of the
*shipped predicate*, the one that withholds vacuously certified bytes, rather than of the bare condition. Throughout
this section the token set is non-empty and non-nullable and q0 is live. Call a byte *useful* when `delta(q0, b)` is
defined and live; `is_split_point(b)` holds exactly when `b` is useful, no live state other than q0 has a
`b`-transition into a live state, and q0 is not re-entrant in the live subautomaton.

**Theorem (specialization).** For every byte `b`, the model certifies `(b, 0)`, the only origin a length-one window
admits, if and only if `is_split_point(b)` holds.

The proof computes `C_1` explicitly: the seed contributes `(delta(q0, b), 0)` exactly when `b` is useful; the direct
step on q0 contributes the same pair when the rename fires and a *before*-pair when q0 is re-entrant; and the direct
step on every other live state with a live-target `b`-transition contributes a *before*-pair. If the predicate holds,
only the first two contribute and they coalesce, so `C_1` is the unanimous singleton. Conversely, a missing usefulness
empties the cloud or leaves only *before*-pairs, a second consuming state plants a *before*-pair, and a re-entrant q0
plants one beside the seed; each breaks certification, and these are exactly the predicate's three clauses. ∎

The vacuous case lands on the predicate's side of the published condition-versus-predicate distinction by
construction: a byte no live state consumes empties the cloud, which the model refuses, exactly as the shipped
predicate withholds a byte the bare condition certifies vacuously. At length one the model is therefore exactly the
shipped predicate; the multi-byte construction is its conservative continuation. The artifact additionally asserts the
agreement byte for byte on every grammar of the evaluation before each search; after the theorem that check verifies
the implementation rather than the claim.

## 7 What the model refuses

The model refuses windows a greedy scanner would allow. The conservatism is deliberate: the seed rule uses acceptance
as the only license a token needs to begin, which over-approximates greedy behaviour by design. Both witnesses below
are asserted artifact rows: the assertion checks that the model refuses the window *and* that an exhaustive oracle
over every completely tokenizable input up to a length bound finds, at every occurrence, the token covering the
window's final byte beginning at the claimed origin, with the occurrence count pinned exactly. The covering-token
check is the defined property; checking merely that some token begins at the origin passes false covering-origin
witnesses, such as `{a, abx, b, x}` at `ab`, where the input `ab` tokenizes as `a | b` while the fixed boundary at the
occurrence remains a safe cut.

*Witness one.* Over `{a, ab, b}` the window `ab` is semantically certified at origin 0, and universally so, not merely
to the oracle's bound: the byte `a` occurs only as a token's first byte, so a token begins at every occurrence offset,
and maximal munch there prefers `ab` over `a`. The oracle confirms the argument over all inputs to length 14, with
98,305 occurrences and zero violations. The model refuses it: after `a`, the cloud is the single pair carrying origin
0, but `a` is a token, so reading `b` seeds a competing trajectory at origin 1, the segmentation `a | b` that greedy
scanning never chooses, and unanimity is lost.

*Witness two.* Over `{ab, abc, c}` the window `abc` is semantically certified at origin 0, again universally: `a`
occurs only token-initially and maximal munch prefers `abc` over `ab`. The oracle confirms it over all inputs to
length 12 with 932 occurrences and zero violations. Both witnesses instantiate the same `(u, uv, v)` shape; they
differ in prefix depth rather than mechanism, the competing origin arriving one byte in for the first and two bytes in
for the second, where the accepting proper prefix `ab` seeds `c`, the segmentation `ab | c` that maximal munch
forgoes.

Both witnesses have length at least two, and that is not an accident of the examples.

**Corollary (non-vacuous strictness begins at length two).** Let `b` be a byte occurring in some completely
tokenizable input. If the model refuses `(b, 0)`, then `(b, 0)` is not a certified split window. The occurrence
hypothesis is necessary: a byte no completely tokenizable input contains satisfies the definition vacuously while the
model refuses its emptied cloud, and such vacuous disagreements are not strictness.

*Proof.* Since `b` occurs, the final segmentation's covering token consumes it at that occurrence, a transition from a
live state into a live state, so if only q0 has a live-target `b`-transition and q0 is not re-entrant, the predicate
reports `b` and, by the specialization theorem, the model certifies `(b, 0)`, contrary to assumption. So the
predecessor's exact condition fails for `b`, and its necessity theorem constructs a completely tokenizable input
placing an occurrence of `b` strictly inside a token; at that occurrence the token containing `b` begins before it. ∎

Non-vacuous conservatism is therefore a strictly multi-byte phenomenon: at length one the model is exact for occurring
bytes, and the shortest strict refusals have length two, a bound witness one attains. The `{a, abx, b, x}` family
plays the opposite role in the artifact, a negative control for the covering-origin property itself: cutting at its
fixed boundary is safe, since `a` occurs only token-initially, the covering origin genuinely varies, and the artifact
pins those violations exactly; an oracle that misses them has lost its teeth. Negatives in every table of this report
are claims about the model, never about the language.

## 8 From a boundary to a parallel cut

At every occurrence at offset `t` in a completely tokenizable input, a certified window `(W, o)` yields a true
boundary `t + o` of the final segmentation. Turning a boundary into a parallel cut is the predecessor's territory: its
prefix-stability result is what licenses a worker to scan from a known boundary and agree with the serial stream
around the cut. The division of labour is exact: this report establishes that `t + o` is a boundary; the predecessor
establishes what a scan starting at a boundary preserves. The v1.3.3 artifact the paper evaluates plans with
single-byte certificates only; release 1.4.0, published after submission, added the explicit window-planning sibling,
which lies outside the paper's evaluation and is not a claim of the paper.

### The mandatory core

For token sets whose certified windows all share structure, the simulator proves it at construction: a byte string that
occurs, with at least one byte after it, inside every certified split window. Candidates come from the shortest words
that force a scan to die, and each is proved or refuted against every death path the live tables allow, so the accessor
reports only what holds for all of them. Block comments prove their closer; token sets whose windows share no such
string report nothing and lose nothing.

The window planner runs on this licence when it exists: candidate windows are generated only around occurrences of the
core, visited in the exhaustive walk's own order and certified by the same memoized decision, so the plan is byte for
byte the walk's, refusals included. A core too long to fit the longest window with a byte to spare concludes the walk's
refusal without scanning, and a tail with no occurrence refuses later targets without another scan. The core is an
accelerator's licence, never a certificate: every cut is still established by the certified window decision, and
grammars without a proved core keep the exhaustive walk unchanged. Construction cost is discussed in limits.md.

## 9 Evaluation

The artifact runs the evaluation in the default test target and CI; the figures are asserted rather than merely
printed, so a drifted number fails the test suite.

Over 400 random token sets on a three-symbol alphabet, generated by the probe itself with a pinned seed and draw
order, 266 are nullable and excluded, since the soundness proof does not cover them; of the 134 remaining, 39 certify
at least one byte exactly. Of the 95 that certify no byte, **91 gain a certified window, and every one of the 91 is
witnessed**: for each, the bounded search finds a completely tokenizable input containing a certified window, with the
covering token beginning at the reported origin, verified as each input is constructed, with the all-91 aggregate
asserted; 4 exhaust the quotient with no window under the model, and none are inconclusive or unresolved. Occurrence
is a property of the concrete word rather than its quotient key, so the witness search continues past the shortest
certified length instead of stopping at the first certifying word. Separate rewind-stress rows exercised 1,079,392
generated executions that scanned through the window and contained at least one rewind, with zero disagreements
against the shipped scanner; 418,466 of those executions tokenize their whole input completely and the remaining
660,926 have malformed suffixes past the window, both counts asserted. This is an implementation stress check: the
generated inputs were required to scan through the window, not to tokenize completely. The random sweep additionally
checked every certified two-byte window over the probe's generated contexts. The length-one case reproduces the
published certificate on all 134 grammars.

Named token sets: all six exact-empty rows of the predecessor's applicability table, five C-like variants and JSON,
gain witnessed windows, and one new cumulative C-like variant joins them as a seventh positive row. The example
windows show the recovery anchors: the string and line-comment rows resynchronize at a newline followed by a byte that
must begin a token, and the block-comment rows at the byte pair `*/` followed by whitespace, in comment context the
closer, with the origin immediately after the pair. The certificate is occurrence-universal, so it also covers
occurrences where `*/` reads as two operator tokens; the pinned witnesses exercise exactly that reading. The
conventional row certifies at length two: its whitespace runs include the newline, so `!` must begin a token there as
well. The JSON row uses the RFC 8259 lexical forms over bytes and assumes UTF-8 validity; it is not a conforming JSON
processor. How often such windows occur in real corpora is an empirical question for the measurement campaign, and no
frequency claim is made. The non-nullable `a+` is included as the negative row: every byte continues a run as readily
as it begins one, the model certifies no window, since absent-byte windows certify only vacuously, and the search
exhausts its quotient, which is precisely the shape of a model-negative. Retained keys count the quotient keys the
search kept before shortest-window stopping, one indicator of the search footprint in place of the `6^n` bound.

| Token set                      | Shortest model-certified k | Example window at origin | Retained keys |
|--------------------------------|----------------------------|--------------------------|---------------|
| C-like, string literals        | 2                          | `\n!` at 1               | 24            |
| C-like, line comments          | 2                          | `\n!` at 1               | 18            |
| C-like, block comments         | 4                          | `\t*/\t` at 3            | 53            |
| C-like, conventional           | 2                          | `\n!` at 1               | 27            |
| split-friendly, block comments | 4                          | `\n*/\t` at 3            | 188           |
| C-like, cumulative (new here)  | 4                          | `\n*/\t` at 3            | 189           |
| JSON, RFC 8259                 | 2                          | `\t"` at 1               | 69            |
| `a+` (negative row)            | none                       | search exhausted         | 3             |

## 10 Limitations

All deliberate. The soundness proof excludes token sets where a token matches the empty string; two thirds of random
grammars are nullable and are excluded rather than counted, and nothing here says anything about them. It also speaks
only of completely tokenizable inputs: malformed input is outside the proof, and no consumed-prefix analogue is
claimed. Negatives are model-relative: the model refuses windows a greedy scanner would allow, so an exhausted search
means no window *under this model*, never that none exists. The worst case is exponential and the probe is budgeted,
though the retained keys stayed below 200 on every named row, at most 32 with mean 9.2 among the 95 no-byte grammars.
The evaluated v1.3.3 artifact ships no window-planning API: its certificate machinery is a probe, on the principle
that the certificate's formulation should freeze in the paper before becoming a public contract. Release 1.4.0,
published after submission, made the formulation a public contract; that implementation lies outside the paper's
evaluation. And no representative real-corpus evidence exists yet; window occurrence frequency is the measurement
campaign's question.

## 11 Related-work summary

The window generalizes the certified split point and inherits its relation to the parallel lexing families:
composition carries every state and pays for it (Mytkowicz et al., ASPLOS 2014); speculation predicts an entry state,
validates, and re-executes on a miss (Prabhu et al., PLDI 2010); bounded alternative-state scans disambiguate without
single-state speculation (Barenghi et al. 2015); prescanning pays a pass over the input (Plex, IPDPS 2021); the
window, like the byte, is derived from the grammar before any input exists.

In the maximal-munch setting, the streaming analysis of Li, Yang, and Mamouras (ASPLOS 2026) statically computes a
grammar's maximum token-neighbor distance and uses the resulting bounded lookahead to emit a sequential maximal-munch
stream without backtracking; their scan advances from a known token boundary, so the window decides maximality rather
than recovering the origin of the token covering an arbitrary occurrence, and parallelization is left as future work.
ZipLex (CAV 2026) formally verifies linear-time invertible maximal-munch lexing, and Li and Mamouras (OOPSLA 2025)
formalize the uniform tokenization problem with algorithms linear in text length for a fixed grammar, precomputing
what each suffix admits in a right-to-left pass; neither derives an occurrence-universal raw-window certificate or
recovers the covering token's origin without scanning from a known boundary. Lester's boxing check anticipates the
flavor at a known join between two analyzed fragments; it is per-join rather than a certificate over every occurrence
of a grammar-derived word, and it does not recover a covering origin.

Recent LLM-tokenizer work addresses input-specific seams instead: LoPT validates position-aligned tokenizations of
overlapping chunks; for BPE, recent work bounds the streaming delay of ordered merge rules from a known beginning
(Mamouras, Li, Yang, PLDI 2026) or maintains the tokenization incrementally over every prefix; Hayase et al. enumerate
the tokenizations whose last token straddles a byte-prefix end, a valid covering tree over one concrete input. The
certificate here is grammar-derived and occurrence-universal rather than input-relative and enumerative, and it fixes
one origin for every occurrence. TokTier certifies input-relative splice junctions for selected frozen pre-tokenizer
and BPE pipelines; ReTokSync monitors the receiver-view tokenization of one concrete generated stream and triggers a
corrective reset when ambiguity occurs. None decides occurrence-universal bounded words for competing prioritized
token languages under maximal munch.

A classical antecedent of window-determines-state is the definite automaton (Perles, Rabin, Shamir 1963); its
operational form for a fixed `k` is `k`-locality: any `k` consecutive symbols force a unique state (Holub and Štekr,
CIAA 2009). Both quantify uniformly over all windows and speak of states; the certificate here is per-window, speaks
of token boundaries under maximal munch, and recovers an origin: raw-state synchronization alone does not identify the
start of the covering maximal-munch token. The classical special case of boundary recovery is code synchronization,
directly so for prefix codes: a synchronizing pair fixes the boundary between its halves regardless of context, and
finite synchronization delay (Restivo 1975) bounds how many codewords are required; the modern bounded-window form is
the synchronizing morphism (Fici et al., MFCS 2025), in a morphic code-factorization setting. Uniquely decipherable
codes may share prefixes; what they guarantee is a unique factorization, with no maximal-munch priority resolving
overlaps between competing token languages, and that difference is where the origin machinery here earns its
existence. The reset-word relationship is one-way and stops at length one: a useful certified byte induces a
reset-like action on the partial live automaton with domain `{q0}` (with the re-entrancy qualification; the
correspondence fails under the classical complete-DFA reading), but a certified window need not be a reset word of the
token DFA at all, since over `{a, b}` the window `ab` certifies at origin 1 while its action on the partial automaton
is empty, and a rank-one letter need not certify. Careful synchronization of partial automata is PSPACE-complete
already over two-letter alphabets (Martyugin 2010); that is context, not a bound, and no hardness result is claimed
for the window problem, whose per-grammar retained-key counts stayed far below the worst case throughout.

Symbolic dynamics comes closest in shape: a resolving block (Adler, Coppersmith, Hassner 1983; Marcus 1985) is a block
all of whose admissible preimages agree at a selected coordinate, an occurrence-universal lift of a local observation
to hidden state, introduced for resetting encoding automata in sliding-block code construction. A certified window
shares that shape, reading the window as the observable block and the covering token's start as the hidden coordinate;
the classical theory is stated for factor maps of subshifts, without token priorities, maximal munch, backup, or any
tokenization semantics, and it neither defines nor decides the lexical instantiation.

The practical counterparts differ in kind: compiler panic-mode recovery discards input to a recovery set after an
error, a post-error parser strategy against a pre-input lexical boundary guarantee; Wagner-Graham incremental lexing
restarts from per-token scanner-state snapshots exact for the text they were cached against, where a certified window
is grammar-universal instead, valid in every completely tokenizable context and known before any input exists, at the
price of existing only where the token set admits one.

Across these areas the paper reports finding no prior work that defines or decides in general, for competing
prioritized token languages under maximal munch, a grammar-derived bounded word whose every occurrence identifies the
origin of the covering token at an arbitrary cut; the closest results decide maximality from a known boundary, bound
retained memory, assume the boundaries they schedule, recover a state without an origin, or validate a concrete
overlap or splice after local retokenization.

## References

- N. Nidhögg. *Certified Split Points for Parallel Lexing: Exact and Modulo Discarded Tokens.* Preprint,
  arXiv:2608.03473, 2026.
- N. Nidhögg. *munch.* Release v1.3.3, archived at doi:10.5281/zenodo.21842344, 2026.
- T. Mytkowicz, M. Musuvathi, W. Schulte. *Data-Parallel Finite-State Machines.* ASPLOS 2014, 529-542.
- P. Prabhu, G. Ramalingam, K. Vaswani. *Safe Programmable Speculative Parallelism.* PLDI 2010, 50-61.
- A. Barenghi, S. Crespi Reghizzi, D. Mandrioli, F. Panella, M. Pradella. *Parallel parsing made practical.* Science
  of Computer Programming 112:195-226, 2015.
- L. Li, S. Sato, Q. Liu, K. Taura. *Plex: Scaling Parallel Lexing with Backtrack-Free Prescanning.* IPDPS 2021,
  693-702.
- A. W. Li, Y. Yang, K. Mamouras. *Static Analysis for Efficient Streaming Tokenization.* ASPLOS 2026, 1880-1896.
- S. Chassot, V. Kunčak. *Formally Verified Linear-Time Invertible Lexing.* CAV 2026, LNCS 16683, 141-164.
- A. W. Li, K. Mamouras. *Efficient Algorithms for the Uniform Tokenization Problem.* PACMPL 9(OOPSLA1), 2025.
- M. M. Lester. *Position Paper: The Science of Boxing.* PLAS 2013, 83-88.
- M. M. Lester, L. Ong, M. Schäfer. *Information Flow Analysis for a Dynamically Typed Language with Staged
  Metaprogramming.* Journal of Computer Security 24(5):541-582, 2016.
- W. Shao, L. Zheng, P. Wang, P. Zheng, J. Li, Y. Fan. *LoPT: Lossless Parallel Tokenization Acceleration for Long
  Context Inference of Large Language Model.* ACL 2026, 33107-33122. Preprint: arXiv:2511.04952.
- K. Mamouras, A. W. Li, Y. Yang. *An Efficient Algorithm for Streaming BPE Tokenization.* PACMPL 10(PLDI), 2026.
- S. Jiang, R. Gong. *Incremental BPE Tokenization.* ICML 2026 (Spotlight). Preprint: arXiv:2605.30813.
- J. Hayase, A. Liu, N. A. Smith, S. Oh. *Sampling from Your Language Model One Byte at a Time.* ICML 2026.
  Preprint: arXiv:2506.14123.
- Z. Zhang, Z. Cao. *TokTier: Exact Stateful CPU+GPU Tokenization for Agentic LLM Serving.* Preprint,
  arXiv:2607.29678, 2026.
- Y. Wang, R. Wang, W. Pang, J. Han, Y. Qi, D. Hu, K. Chen. *ReTokSync: Self-Synchronizing Tokenization
  Disambiguation for Generative Linguistic Steganography.* Preprint, arXiv:2604.25486, 2026.
- A. Borsotti, S. Crespi Reghizzi, M. Pradella. *Attribute-Based Precedence Relations for Context-Free Grammars.*
  Preprint, SSRN, doi:10.2139/ssrn.6890975, 2026.
- M. Chiari, D. Mandrioli, M. Pradella. *Cyclic operator precedence grammars for parallel parsing.* Information and
  Computation 307:105363, 2025.
- M. A. Perles, M. O. Rabin, E. Shamir. *The Theory of Definite Automata.* IEEE Transactions on Electronic Computers
  EC-12(3):233-243, 1963.
- J. Holub, Š. Štekr. *On Parallel Implementations of Deterministic Finite Automata.* CIAA 2009, LNCS 5642, 54-64.
- A. Restivo. *A Combinatorial Property of Codes Having Finite Synchronization Delay.* Theoretical Computer Science
  1(2):95-101, 1975.
- M. V. Berlinkov, R. Ferens, A. Ryzhikov, M. Szykuła. *Synchronization of Strongly Connected Partial DFAs and Prefix
  Codes.* DMTCS 28:2, 2026. Extended version of the STACS 2021 paper.
- G. Fici, G. Romana, M. Sciortino, C. Urbina. *Morphisms and BWT-Run Sensitivity.* MFCS 2025, LIPIcs 345,
  49:1-49:18.
- M. V. Volkov. *Synchronizing Automata and the Černý Conjecture.* LATA 2008, LNCS 5196, 11-27.
- P. V. Martyugin. *Complexity of Problems Concerning Carefully Synchronizing Words for PFA and Directing Words for
  NFA.* CSR 2010, LNCS 6072, 288-302.
- A. V. Aho, M. S. Lam, R. Sethi, J. D. Ullman. *Compilers: Principles, Techniques, and Tools.* 2nd edition,
  Addison-Wesley, 2006.
- T. A. Wagner, S. L. Graham. *General Incremental Lexical Analysis.* Manuscript, UC Berkeley, 1997.
  https://harmonia.cs.berkeley.edu/papers/twagner-lexing.pdf
- R. M. Kaplan. *A Method for Tokenizing Text.* In Inquiries into Words, Constraints and Contexts: Festschrift for
  Kimmo Koskenniemi on his 60th Birthday, CSLI Publications, 55-64, 2005.
- M. Cognetta, N. Okazaki. *Tokenization as Finite-State Transduction.* Computational Linguistics 51(4):1119-1149,
  2025.
- R. L. Adler, D. Coppersmith, M. Hassner. *Algorithms for Sliding Block Codes: An Application of Symbolic Dynamics
  to Information Theory.* IEEE Transactions on Information Theory 29(1):5-22, 1983.
- B. H. Marcus. *Sofic Systems and Encoding Data.* IEEE Transactions on Information Theory 31(3):366-377, 1985.
- T. Bray (Ed.). *The JavaScript Object Notation (JSON) Data Interchange Format.* RFC 8259, STD 90, December 2017.
