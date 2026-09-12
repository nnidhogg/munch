# Certified Split Windows for Parallel Lexing: Recovering Boundaries Where No Byte Certifies

**Nicklas Nidhögg**, August 2026. Mirrors `paper/split-windows/split-windows.tex`, published as
[arXiv:2608.09761](https://arxiv.org/abs/2608.09761), which evaluates munch at the v1.3.3 release, where the window
machinery is a probe: the paper's principle is that the certificate's formulation should freeze there before becoming a
public contract. The supported API, `Lexer::is_split_window()` and `chunk_boundaries_with_windows()`, arrived in release
1.4.0, after the paper's submission, and lies outside its evaluation. A token set in which some token matches the empty
string is inside the paper's scope: Lemma 1 decides it through its positive-width equivalent, the same automaton entered
through a start state that does not accept, which changes no scan, and the random sweep decides its 266 nullable sets
that way rather than setting them aside. The mandatory core section is this document's addition alone: release 1.5.0
derives that machinery on top of the same certificates, and no version of the paper contains it. Every empirical
aggregate below is printed and asserted by the probes, so a drifted number fails the test suite; the table below is
produced mechanically from the paper's own tabular source, never transcribed by hand.

*A technical report on the certificate behind the window layer. The implementation, tests, and probes live in this
repository; this document states the idea precisely, relates it to prior work, and reports what it recovers.*

## Abstract

A certified split point lets a parallel lexer cut unlexed input at a single byte with the serial token stream provably
preserved, but several conventional token sets in the predecessor's controlled study certify no byte once string,
comment, or whitespace-run forms are included ([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)). We generalize from
a byte to a bounded window: a byte string after which the position where the current token began is known, regardless of
surrounding context. We certify the directly usable form of that recovery: the token covering the window's final byte
begins at the reported origin. The certificate is conditional on occurrence and may be vacuous; every applicability
figure counts only windows carrying an asserted completely tokenizable occurrence witness. We give a conservative model
of a maximal-munch scanner's possible histories across a window, prove it sound, and decide reachability in that model
exactly by exhausting a finite quotient of its reachable configurations, so every answer of the unbudgeted procedure is
either a certified window with its origin or a proof that the model admits none. Within the stated flat,
completely-tokenizable scope, model-positive answers are semantic certificates; negatives are relative to the
conservative model, which deliberately refuses some windows a greedy scanner would allow. A token set in which some
token matches the empty string is decided through its positive-width equivalent, the same automaton entered through a
start state that does not accept, which changes no scan. In a sample of 400 random token sets, 322 of the 337 sets
certifying no byte gain a witnessed window, with zero inconclusive searches, and every exact-empty row of the
predecessor's study gains a witnessed window of two to four bytes. Rewind-stress rows exercised 1,079,392 executions
that scanned through the window and contained at least one rewind, with zero disagreements against the shipped scanner.
The analysis runs once after automaton construction, using only the compiled tables and no input.

## 1 Introduction

The predecessor of this paper derives, from a compiled token set, the complete set of bytes at which unlexed input can
be cut with the serial token stream preserved, and proves the condition necessary as well as sufficient
([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)). Its sharpest limitation is its own applicability table: several
of the studied conventional token sets certify *no* byte, because a single string form, comment form, or whitespace run
gives some non-initial live state a transition on every candidate, and a fresh 400-grammar random sweep generated for
this study reproduces the pattern, 337 of its 400 sets certifying none. This paper is about the object that refusal
leaves standing: a byte *string* after which the start of the token covering the window's final byte is pinned,
regardless of surrounding context.

Concretely, we ask: for a token set compiled to a DFA and scanned by maximal munch, is there a window
`W = w0 ... w(k-1)` and an offset `o` with `0 <= o < k` such that in *every* completely tokenizable input containing
`W`, the token covering the occurrence's final byte begins exactly `o` bytes into it? A worker that finds `W` in a
completely tokenizable input may then begin scanning at the recovered boundary with no speculation, no state
enumeration, and no DFA-state-recovery pass over the input, exactly as at a certified byte, which is the `k = 1` case.

Our contributions:

- A conservative model of the scanner's possible token-prefix histories across a window, replacing a refuted natural
  predecessor, and a soundness proof by a representation invariant (Section 3, Section 4).
- A finite quotient of the model's reachable configurations, exact for them by a stated single-occupancy invariant,
  turning breadth-first search over windows into a terminating decision procedure for the model (Section 5).
- Two specializations: at length one the model certifies exactly the bytes the published predicate reports, so the
  multi-byte construction is that predicate's conservative continuation; and over a prefix code the certified windows
  are exactly the synchronizing splits of code theory whose right half sits inside one codeword, which places the
  classical case as the special case it is (Section 6).
- An evaluation over all six exact-empty rows of the prior study's applicability table, one new cumulative variant, and
  400 random token sets generated for this paper: the witnessed rescue rate where no byte certifies, the window lengths
  that suffice for the studied C-like and JSON tokenizations, and rewind-stress checks of 1,079,392 generated executions
  that scanned through the window and contained at least one rewind, 418,466 of them completely tokenizable, with zero
  disagreements against the shipped scanner (Section 9).

The probe decides certificates and model-search results offline from the compiled tables after automaton construction;
the occurrence witnesses and scanner checks are generated executions against the shipped scanner. Every empirical
aggregate and table entry reported here is printed and asserted by the probe in the munch repository (release v1.3.3,
archived at doi:10.5281/zenodo.21842344): a drifted number fails the test suite.

## 2 Preliminaries

We inherit the scanner model of the predecessor. A token set compiles to a DFA `A` with initial state `q0`; scanning is
by maximal munch: from the current position the scanner runs `A` as far as a transition exists, emits the token of the
last accepting configuration passed, resumes immediately after it, and restarts in `q0`. An input is *completely
tokenizable* when this process consumes it exactly. The setting is *flat*: one fixed token set compiled to one DFA
scanned from one fixed `q0`, with no lexical modes, no mode stack, and no semantic scanner state. `A⁺` denotes the live
subautomaton: states both reachable from `q0` and co-accessible to acceptance, with only transitions between live states
retained. A token may match the empty string; the scan never emits it, and the next lemma is how every statement below
still assumes a start state that neither accepts nor is re-entered.

**Lemma 1 (positive-width equivalent).** Let `A` be the DFA of a token set whose start state `q0` accepts, and let `A'`
be `A` with a fresh start state `q0'` that carries `q0`'s outgoing transitions and does not accept, `q0` itself
retained. Then the maximal-munch scans of `A` and `A'` agree on every input, `A'` accepts exactly the nonempty words `A`
accepts, and no transition of `A'` enters `q0'`.

*Proof.* A token has positive width, so a scan records an accepting configuration only after consuming a byte. From `q0`
and from `q0'` the first byte leads to the same state, since `q0'` carries `q0`'s transitions, and from there the two
scans traverse the same states and record the same accepting positions; the emitted token and the restart offset
therefore agree, and both scans restart in their own start state. The accepted words of `A'` are the words of length at
least one accepted by `A`, the empty word being lost with `q0'`'s acceptance. No transition of `A` targets `q0'`, which
is fresh, and the copied transitions target `q0`'s successors, so nothing enters `q0'`. ∎

Throughout, a token set whose start state accepts is read through `A'`, and the artifact compiles every token set this
way before deciding anything about it: the unrolled automaton has one state more and the same scan, so nothing below
loses generality by assuming that `q0` does not accept, and the re-entrancy condition of Section 6 is then trivially met
at `q0'` while `q0`, now an ordinary state, is handled as any other.

**Definition 1 (certified split window).** Let `W = w0 ... w(k-1)` with `k >= 1` and let `o ∈ {0, ..., k-1}`. The pair
`(W, o)` is a *certified split window* for a token set when, for every completely tokenizable input `x` containing `W`
at offset `t`, the token of the maximal-munch tokenization of `x` that contains the byte at offset `t + k - 1` begins at
`t + o`.

**Definition 2 (witnessed window).** A certified split window `(W, o)` is *witnessed* when some completely tokenizable
input contains `W`.

Definition 1 quantifies over the inputs containing `W` and is therefore vacuously true when no completely tokenizable
input contains it, and the vacuity is not hypothetical: over `{0, 00, 01}` the model below certifies the window `1001`
at origin 2, yet no completely tokenizable input contains `1001`, since every `1` is the tail of a greedily chosen `01`,
a token boundary therefore follows the window's first byte, maximal munch must then consume `00`, and the final `1` sits
at a boundary no token starts. The preceding argument proves non-occurrence; the artifact asserts the model prediction
and the empty result of its bounded targeted witness search. Every applicability figure in this paper counts only
witnessed certificates: the named rows pin their witness inputs in the artifact, and for each random grammar the probe
constructs and verifies a concrete completely tokenizable input, with the 322 aggregate asserted; a certificate the
bounded witness search cannot witness is reported as unresolved, never as a rescue.

For `k = 1` Definition 1 is the boundary guarantee of a certified split point, since the token containing the single
byte begins at it. Three guarantees should be kept apart, and this paper certifies the strongest: model unanimity
implies that the covering token's origin is fixed, which implies that some fixed safe boundary exists inside the window,
and neither converse holds. Over `{a, ab, b}` at `ab` the covering origin is fixed yet the model refuses (Section 7);
over `{a, abx, b, x}` at `ab` the byte `a` always begins a token, so offset 0 is a fixed safe boundary, yet the covering
token's origin is not fixed, since the final `b` belongs to `b` in the input `ab` and to `abx` in `abx`. The weakest
guarantee is already usable, since a worker can cut at the known boundary and rescan the window's suffix; the
covering-token form is the sufficient, deliberately stronger property this forward origin-recovery model certifies, and
it hands the worker its resumption point directly. The model therefore has two distinct sources of false negatives:
covering-origin certification is stricter than locating some safe boundary, and the cloud conservatively represents
extra segmentations and may refuse even a true covering-origin certificate. Note also what the definition does not
require: it says nothing about the tokens overlapping the window's earlier bytes, and it does not require the boundary
to be the only one inside the window.

## 3 The model

A worker cutting blind knows only that in the final segmentation, the window's first byte is consumed from *some* live
token-prefix state, either inside a token that began at some unknown earlier offset or exactly at a boundary; the
acceptance-gated seed below carries the boundary case, including a window at the start of the input. The model tracks a
*cloud* of hypotheses `(q, ω)`: a live state `q` paired with an origin `ω`, either `before` for a token that began
before the window or an in-window offset. The initial cloud `C0` is every live state paired with `before`.

Reading window byte `w_j` maps `C_j` to `C_(j+1)` by two rules:

- **Direct step.** A pair `(q, ω)` whose state consumes `w_j` into a live state moves there with its origin unchanged. A
  pair whose state cannot consume `w_j` into a live state is an impossible history and is dropped, never restarted.
- **Acceptance-gated seed.** If some pair in `C_j` is accepting, one fresh pair `(δ(q0, w_j), j)` is seeded, provided
  that target is live: a token can begin at offset `j` only where the previous token could have ended or at the input
  start, the case the initially open gate represents, and tokens end only where the automaton accepts.

One rename accompanies the direct step: where `q0` is not re-entrant in `A⁺`, a pair stepping *from* `q0` is beginning a
token, so its origin becomes the current offset; where a non-empty live path returns to `q0`, the rename is disabled,
since reaching `q0` then no longer identifies a boundary. This is the same re-entrancy condition the length-one
certificate carries ([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)).

Clouds are *sets* of pairs; the set semantics is load-bearing below, where two rules inserting the same pair yield one
element. The window is *certified at origin `o`* when `C_k ≠ ∅` and there is an `o ∈ {0, ..., k-1}` with `ω = o` for
every `(q, ω) ∈ C_k`; an empty cloud certifies nothing, matching the implementation, which refuses it, and the empty
cloud is absorbing: every step of it is empty. Non-emptiness guarantees nothing in the other direction: a certified
window may be vacuous under Definition 1, the separation Definition 2 exists to make. Agreement on the state alone is
not enough: learning that the scan is inside a string literal is knowledge, but not a boundary. Throughout, `q0` is
*re-entrant* when some live transition of `A⁺` targets it; non-re-entrancy means no live edge enters `q0` at all.

**The discarded predecessor.** A natural variant restarts a trajectory that cannot consume the byte, treating the
failure point as a token boundary, in place of the acceptance-gated seed. That variant is not merely unproved but
refuted. Over the token set `{a, abc, bx, x}` and window `abx`, its cloud after `ab` is the single pair carrying origin
0 toward `abc`; `x` kills that trajectory, the restart replaces it with a token beginning at offset 2, nothing else
survives, and the variant certifies `(abx, 2)`. Yet the input `abx` itself is completely tokenizable as `a` followed by
`bx`, so the token covering the final byte begins at offset 1: the certificate is false at a witnessed occurrence. A
failing trajectory is an impossible history, not a boundary, and restarting it manufactures support for origins no
execution justifies. The repaired model certifies `abx` at origin 1, where the scanner does cut, and the case is carried
as an asserted row of the artifact.

## 4 Soundness

Fix a completely tokenizable input `x` containing `W` at offset `t`. For `j ∈ 1..k` let `σ_j` be the start of the token
that the final maximal-munch segmentation of `x` assigns to the byte at `t+j-1`, let `ρ_j = δ*(q0, x[σ_j .. t+j))` be
that token's prefix state after consuming through byte `t+j-1`, and let `ω_j = σ_j - t`, or `before` when `σ_j < t`.

**Lemma 2 (representation).** For every `j ∈ 1..k`, the pair `(ρ_j, ω_j)` is in `C_j`.

*Proof.* Every `ρ_j` is live: reachable through the actual token prefix, and co-accessible because its token ends at
some `e >= t+j` with `δ*(ρ_j, x[t+j .. e)) = δ*(q0, x[σ_j .. e))`, and the right side is accepting.

*Base, `j = 1`.* If `σ_1 < t`, the state `p = δ*(q0, x[σ_1 .. t))` is live and lies in `C0` paired with `before`; the
direct step carries it to `(ρ_1, before)`. The rename cannot interfere: `p` has consumed at least one byte, so `p = q0`
only if a non-empty live path returns to `q0`, which disables the rename. If `σ_1 = t`, then `ρ_1 = δ(q0, w0)` and the
seed fires, because `C0`, which is all of the live states, contains a live accepting state: the fixed input is
completely tokenizable and non-empty, so its first token traces an accepting path from `q0`, making `q0` live with an
accepting state reachable from it, and a reachable accepting state is trivially co-accessible.

*Step, `j` to `j+1`.* If the byte at `t+j` continues its token, `σ_(j+1) = σ_j`, and the direct step carries
`(ρ_j, ω_j)` to `(δ(ρ_j, w_j), ω_j)`; the rename does not overwrite the origin by the same re-entrancy argument as in
the base. If the byte at `t+j` begins a token, the previous token ended at `t+j`, so `δ*(q0, x[σ_j .. t+j))` accepts;
that state is `ρ_j`, in `C_j` by hypothesis, so the seed fires and emits exactly
`(δ(q0, w_j), j) = (ρ_(j+1), ω_(j+1))`. ∎

The lemma is containment, not equality: the cloud may carry hypotheses no execution realizes. That is harmless in one
direction and load-bearing in the other: at a realized occurrence in a completely tokenizable input, surplus hypotheses
can only preserve the actual origin's unanimity or destroy unanimity; they cannot manufacture unanimity at a false
origin.

**Theorem 1 (soundness).** If `C_k ≠ ∅` and every pair of `C_k` carries the same origin `o ≠ before`, then `(W, o)` is a
certified split window.

*Proof.* By Lemma 2, `(ρ_k, ω_k) ∈ C_k`, so `ω_k = o`, so `σ_k = t + o`, a token start of the final segmentation. The
input was an arbitrary completely tokenizable one containing `W` at `t`. ∎

**Backup never appears.** Maximal-munch lookahead that a later rewind discards occupies states the lemma says nothing
about; the invariant tracks where the *final* segmentation's tokens begin, not where the read head wanders. A boundary
that a rewind later exposes was seeded by the model step reading the first byte after the accepting position justifying
it. This holds when lookahead crosses the window's left edge, when several accepting positions are passed, and across
chains of consecutive rewinds; each is an instance of the step case.

## 5 The finite quotient and the decision procedure

Acceptance gating alone does not terminate: over `a⁺` the automaton accepts after every byte and origins accumulate
without bound. The search therefore deduplicates on a quotient of the cloud: the set `B` of states carrying `before`,
and for each state the number of distinct in-window origins it carries, saturated at two.

**Invariant 1 (single occupancy).** In every reachable cloud, each surviving in-window origin occupies at most one
state.

An origin enters the cloud at most once per rule application, into a single state, and the deterministic step moves it
to at most one successor, so it continues in one state or dies. At offset `j` the acceptance seed and the
non-re-entrant-`q0` rename can both propose the fresh origin `j`; both target exactly `δ(q0, w_j)`, and set semantics
coalesces the identical pair, so single occupancy survives the collision. Origins may die, which is why the invariant
says at most one rather than exactly one. The pre-window origin is deliberately different: it may occupy many states and
is held apart as the Boolean support set `B`. An arbitrary cloud could violate the invariant, placing one origin in two
states, and there the saturated counts could not decide unanimity; no such cloud is reachable, and the restriction is
load-bearing for everything below. The key of a cloud is the pair `κ(C) = (B, m)` with
`m(q) = min(2, number of in-window origins at q)`.

**Lemma 3 (congruence, depth-aligned).** Let `C_u` and `C_v` be clouds reached by the search after words `u` and `v`. If
`κ(C_u) = κ(C_v)`, then for every byte `b` the successors `step(C_u, b, |u|)` and `step(C_v, b, |v|)` are either both
empty or both non-empty, and when non-empty, `κ(step(C_u, b, |u|)) = κ(step(C_v, b, |v|))`, with the certification
verdict agreeing on both sides. For successor-key equality alone, it is enough that the inserted offsets be *fresh*,
exceeding every in-window origin of their respective clouds; the certification verdict additionally needs legal depths,
and the search supplies both: it steps with the word lengths `|u|` and `|v|`, and every origin in a length-`ℓ` cloud
lies below `ℓ`. Freshness is not decorative: over `{a⁺, b}` the cloud after `a` carries origin 0, and stepping on `a`
with offset 0 merges the seed into that origin while offset 1 creates a second one, so the same key would have two
different successors under arbitrary offsets.

*Proof.* The acceptance gate reads only which occupied states accept, and the occupied states are `B ∪ {q : m(q) > 0}`,
a function of the key. The new `before`-support is the live deterministic image of `B`, excluding the contribution from
`q0` when `q0` is non-re-entrant; that excluded contribution joins the fresh-origin rename handled below. Over the
single-token set `{a}`, stepping `C0` on `a` renames `q0`'s `before` origin to 0, so `B' = ∅`, where the unexcluded
image would wrongly retain `δ(q0, a)`. For the in-window counts at a successor state `q'`: the direct step carries each
origin from its unique state (Invariant 1), so origins arriving at `q'` from distinct predecessors are distinct, and the
exact update at each target is: sum the old-origin contributions arriving there, add one fresh origin if the seed or the
non-re-entrant-`q0` rename fires toward it, combining those two by logical or, since they denote the same fresh origin
and the cloud is a set, and then saturate at two. When `q0` is non-re-entrant, no live edge enters `q0`, so after the
first step no reachable cloud contains `q0` with an in-window origin; the rename decision is therefore readable from
`q0 ∈ B`. Whether the rename fires is determined by `q0 ∈ B`, which is key-readable, and by re-entrancy, which is fixed
automaton data rather than part of the key; whether the seed fires is determined by the key's accepting support; that
the fresh origin collides with no surviving origin is exactly the freshness hypothesis. Note that saturation commutes
with this update in the only direction needed: a saturated state maps its surviving origins together, so a successor's
saturated sum computed from saturated counts equals the saturation of the exact sum; a state's count can also drop to
zero outright when its transition is missing, which the update handles as an empty contribution. Fresh origins are
equivariant under renaming of origin values, and no rule ever reads an origin's value, only equality between origins, so
keys computed on either side agree. Emptiness is key-readable, and certification of the full window is the key predicate
`B = ∅` with total count exactly one, using Invariant 1 to identify one total count with one origin in one state. ∎

One tempting simplification is false and worth flagging: it is not true that a state holding two origins can never
contribute to unanimity later. Over `{a⁺, b}`, after `aa` the `a`-state carries two in-window origins, as well as
`before`; on `b` all of them die while their pre-step acceptance opens the gate for one fresh `b`-origin, and the cloud
is unanimous. What is true, and what the lemma uses, is that co-located origins either follow the same live successor
together or all die, and that origins which die leave no trace the key must distinguish.

**Theorem 2 (decision procedure).** Breadth-first search over windows, deduplicated on keys, decides whether the model
certifies any window for a given token set, and returns the minimum certified length; it does not enumerate every
certified word. Each state contributes a factor of `2 × 3` to the key space, so at most `6^|Q⁺|` keys are ever retained;
each key expands into at most 256 successors, each computable from the key in `O(|Q⁺|)` time, for a worst case of
`O(256 · |Q⁺| · 6^|Q⁺|)` time and `O(|Q⁺| · 6^|Q⁺|)` space.

By Lemma 3 the walk cannot miss the existence of a certifying continuation, nor change the minimum certified length:
after one matched step both successor clouds sit at fresh depths again, so the lemma inducts over every common suffix;
breadth-first order retains a shallowest representative of each key; and a certifying suffix from a discarded deeper
occurrence therefore yields an equal or shorter certificate from the retained representative. Deduplication may still
skip individual certified words; exhaustion proves the model admits none at any length. What exhaustion does not give is
semantic non-existence, because the model itself is conservative (Section 7). The displayed bounds describe an abstract
key-to-key search; the probe realizes the procedure with one concrete representative cloud and word per key, using the
quotient for deduplication, so its cost additionally depends on representative size. It is budgeted, with three
outcomes: certified, exhausted, and inconclusive once the retained key count exceeds a fixed threshold of 200,000 keys;
the evaluation below reports zero inconclusive searches, and the retained key counts it reports are one indicator of the
search footprint rather than a complete cost model.

## 6 Specialization to length one

At length one the model collapses to the published certificate, and the correspondence is exact at the level of the
*shipped predicate*, the one that withholds vacuously certified bytes, rather than of the bare condition. Throughout
this section the token set is non-empty, read through its positive-width equivalent where a token matches the empty
string, and `q0` is live.

Call a byte `b` *useful* when `δ(q0, b)` is defined and live. The predicate `is_split_point(b)` of the predecessor holds
exactly when `b` is useful, no live state other than `q0` has a `b`-transition into a live state, and `q0` is not
re-entrant in `A⁺`. Two quantifier readings coincide here without loss: a transition target that is co-accessible makes
its source co-accessible, so "reachable state with a live-target `b`-transition" and "live state with a live-target
`b`-transition" name the same states.

**Theorem 3 (specialization).** For every byte `b`, the model certifies `(b, 0)`, the only origin a length-one window
admits, if and only if `is_split_point(b)` holds.

*Proof.* Compute `C_1` explicitly. `C0` is every live state paired with `before`, and it contains a live accepting
state, since `q0` is live and an accepting state reachable from it is trivially co-accessible, so the seed's acceptance
gate is open at offset 0. Reading `b` therefore yields exactly:

- from the seed, the pair `(δ(q0, b), 0)`, present if and only if `b` is useful;
- from the direct step on `(q0, before)`, present if and only if `b` is useful: the pair `(δ(q0, b), 0)` when `q0` is
  not re-entrant, by the rename, and `(δ(q0, b), before)` when it is;
- from the direct step on every other live state `q` that has a live-target `b`-transition, the pair
  `(δ(q, b), before)`.

If `is_split_point(b)` holds, only the first two items contribute, the rename fires, and both surviving contributions
are the single pair `(δ(q0, b), 0)`, so `C_1` is the singleton and the model certifies `(b, 0)`. Conversely, suppose the
model certifies `(b, 0)`, so `C_1` is non-empty and unanimous at origin 0. If `b` were not useful, the first two items
would be absent and every pair of `C_1` would carry `before` from the third, or `C_1` would be empty; either way
certification fails, so `b` is useful. If some live `q ≠ q0` had a live-target `b`-transition, the third item would
place a `before`-pair in `C_1`, breaking unanimity; so none does. If `q0` were re-entrant, the second item would place
`(δ(q0, b), before)` in `C_1` beside the seed's origin-0 pair, breaking unanimity; so `q0` is not re-entrant. These are
exactly the predicate's three clauses. ∎

The vacuous case lands on the predicate's side of the published condition-versus-predicate distinction by construction:
a byte no live state consumes empties the cloud, which the model refuses, exactly as the shipped predicate withholds a
byte the bare condition certifies vacuously. At length one the model is therefore exactly the shipped predicate; the
multi-byte construction is its conservative continuation. The artifact additionally asserts the agreement byte for byte
on every grammar of the evaluation before each search; after Theorem 3 that check verifies the implementation rather
than the claim.

### The prefix-code case

The other specialization is by token set rather than by length. A *prefix code* `X ⊆ Σ⁺` has no word that is a proper
prefix of another, and read as a token set it is the setting where the certificate needs neither maximal-munch priority
nor the cloud's origin bookkeeping: at any boundary at most one codeword begins, so the completely tokenizable inputs
are exactly `X*`, each with the one factorization the scan computes. Code theory has two notions of a boundary forced by
bounded context. A *synchronizing pair* of `X` (Berstel, Perrin and Reutenauer, Cambridge University Press 2010) is a
pair `(x, y) ∈ X* × X*` such that `u x y v ∈ X*` implies `u x ∈ X*` and `y v ∈ X*` for all `u, v ∈ Σ*`; and, in the
factor formulation of Fici, Romana, Sciortino and Urbina (MFCS 2025), a split `w = w1 w2` of a factor `w` of `X*` is
*synchronizing* when every occurrence of `w` in a word of `X*` has a factorization boundary between `w1` and `w2`.
Neither is a certified window as it stands, since neither says which codeword covers the window's final byte, and the
certificate says nothing about the halves being in `X*`; the exact relation is the following.

**Proposition 1 (prefix codes).** Let `X` be a prefix code read as a token set, and let `W` be a window with origin `o`,
split as `W = W_<o W_≥o` at the origin.

1. `(W, o)` is certified if and only if the split is synchronizing and `W_≥o` is a prefix of some codeword.
2. If `(x, y)` is a synchronizing pair with `y` nonempty and `y = c1 ... cm` is its factorization into codewords, then
   `(xy, |x| + |c1 ... c(m-1)|)` is certified. Conversely, if `(W, o)` is certified with `W_<o ∈ X*` and `W_≥o ∈ X`,
   then `(W_<o, W_≥o)` is a synchronizing pair.

*Proof.* Throughout, a completely tokenizable input is a word of `X*` and its token boundaries are the boundaries of its
one factorization, since at a boundary the codeword the text spells is the only codeword that begins there.

(1) If `(W, o)` is certified, every occurrence has a boundary at offset `o`, so the split is synchronizing, and the
codeword covering the final byte begins at `o`, so `W_≥o` lies within that codeword from its start and is a prefix of
it. Conversely, take an occurrence of `W` at offset `t` in a word of `X*`; the split being synchronizing puts a boundary
at `t + o`, where some codeword `c'` begins. Let `c` be a codeword with prefix `W_≥o`. Were `c'` to end inside `W_≥o`,
it would be a proper prefix of `c`, which a prefix code forbids; so `c'` extends through `W_≥o`, and the token covering
the final byte begins at `t + o`.

(2) Let `(x, y)` be a synchronizing pair and take an occurrence `u x y v ∈ X*`. Then `u x ∈ X*` places a boundary before
`y`, and `y v ∈ X*` with `y ∈ X*` factors from that boundary through `y`'s own codewords, since at each of their starts
the codeword the text spells is the one `y` contains; so `cm` begins at `|u x| + |c1 ... c(m-1)|` and covers the final
byte of `xy`. Conversely, if `(W, o)` is certified with `W_<o ∈ X*` and `W_≥o ∈ X`, then every `u W v ∈ X*` has a
boundary at `|u| + o`, so `u W_<o` is a concatenation of codewords and so is `W_≥o v`, which is the synchronizing-pair
condition. ∎

The proposition places the classical case: over a prefix code the certified windows are the synchronizing splits whose
right half sits inside one codeword, and Berstel, Perrin and Reutenauer's pairs are the certified windows whose halves
are themselves in the code, the origin resting on the last codeword. Everything in the studied token sets falls outside
it. An identifier is a proper prefix of a longer identifier and `<` of `<<`, so none of the C-like or JSON token sets is
a prefix code; over `{a, ab, b}` the word `ab` has two factorizations and it is maximal munch, not the code, that picks
one. There the certificate is defined through the scan, the origin bookkeeping of Section 3 is what decides it, and the
reach beyond the code case is exactly the reach the evaluation measures.

## 7 Strictness of the model

The model refuses windows a greedy scanner would allow. The conservatism is deliberate: the seed rule uses acceptance as
the only license a token needs to begin, which over-approximates greedy behaviour by design, and this section exhibits
one source of conservatism and shows that nonvacuous strictness begins at length two. Both witnesses below are asserted
artifact rows: the assertion checks that the model refuses the window *and* that an exhaustive oracle over every
completely tokenizable input up to a length bound finds, at every occurrence, the token covering the window's final byte
beginning at the claimed origin, with the occurrence count pinned exactly. The covering-token check is the property of
Definition 1; checking merely that some token begins at the origin passes false covering-origin witnesses, such as
`{a, abx, b, x}` at `ab`, where the input `ab` tokenizes as `a|b` while the fixed boundary at the occurrence remains a
safe cut.

*Witness one.* Over `{a, ab, b}` the window `ab` is semantically certified at origin 0, and universally so, not merely
to the oracle's bound: the byte `a` occurs only as a token's first byte, so a token begins at every occurrence offset
`t`, and maximal munch there prefers `ab` over `a`, so the covering token of the final byte begins at `t`. The oracle
confirms the argument over all inputs to length 14, with 98,305 occurrences and zero violations. The model refuses it:
after `a`, the cloud is the single pair carrying origin 0, but `a` is a token, so reading `b` seeds a competing
trajectory at origin 1, the segmentation `a|b` that greedy scanning never chooses, and unanimity is lost.

*Witness two.* Over `{ab, abc, c}` the window `abc` is semantically certified at origin 0, again universally: `a` occurs
only token-initially, so a token begins at `t`, and maximal munch prefers `abc` over `ab` there. The oracle confirms it
over all inputs to length 12 with 932 occurrences and zero violations. Both witnesses instantiate the same `(u, uv, v)`
shape; they differ in prefix depth rather than mechanism, the competing origin arriving one byte in for the first and
two bytes in for the second, where the accepting proper prefix `ab` seeds `c`, the segmentation `ab|c` that maximal
munch forgoes.

Both witnesses have length at least two, and that is not an accident of the examples.

**Corollary 1 (non-vacuous strictness begins at length two).** Let `b` be a byte occurring in some completely
tokenizable input. If the model refuses `(b, 0)`, then `(b, 0)` is not a certified split window. The occurrence
hypothesis is necessary: a byte no completely tokenizable input contains satisfies Definition 1 vacuously while the
model refuses its emptied cloud, and such vacuous disagreements are not strictness.

*Proof.* Since `b` occurs, the final segmentation's covering token consumes it at that occurrence, a transition from a
live state into a live state, so if only `q0` has a live-target `b`-transition and `q0` is not re-entrant, the predicate
reports `b` and, by Theorem 3, the model certifies `(b, 0)`, contrary to assumption. So the exact condition of the
predecessor fails for `b`, and its necessity theorem constructs a completely tokenizable input placing an occurrence of
`b` strictly inside a token; at that occurrence the token containing `b` begins before it, so `(b, 0)` is not
certified. ∎

Non-vacuous conservatism is therefore a strictly multi-byte phenomenon: at length one the model is exact for occurring
bytes, and the shortest strict refusals have length two, a bound witness one attains. The `{a, abx, b, x}` family of
Section 2 plays the opposite role in the artifact, a negative control for the covering-origin property itself: cutting
at its fixed boundary is safe, since `a` occurs only token-initially, the covering origin genuinely varies, and the
artifact pins those violations exactly; an oracle that misses them has lost its teeth. Negatives in every table of this
paper are claims about the model, never about the language.

## 8 From a boundary to a parallel cut

At every occurrence at offset `t` in a completely tokenizable input, a certified window `(W, o)` yields a true boundary
`t + o` of the final segmentation. Turning a boundary into a parallel cut is the predecessor's territory: its
prefix-stability result is what licenses a worker to scan from a known boundary and agree with the serial stream around
the cut ([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)). The division of labour is exact: this paper establishes
that `t + o` is a boundary; the predecessor establishes what a scan starting at a boundary preserves. The v1.3.3
artifact evaluated here plans with single-byte certificates only; release 1.4.0, published after submission, added an
explicit window-planning sibling, which lies outside this paper's evaluation and is not a claim of this paper.

### The mandatory core

For token sets whose certified windows all share structure, the simulator proves it at construction: a byte string that
occurs, with at least one byte after it, inside every certified split window. Candidates come from the shortest words
that force a scan to die, and each is proved or refuted against every death path the live tables allow from its
proposing state, so the accessor reports only what holds for all of them. Block comments prove their closer; token sets
whose windows share no such string report nothing and lose nothing.

The window planner runs on this licence when it exists: candidate windows are generated only around occurrences of the
core, visited in the exhaustive walk's own order and certified by the same memoized decision, so the plan is byte for
byte the walk's, refusals included. A core too long to fit the longest window with a byte to spare concludes the walk's
refusal without scanning, and a tail with no occurrence refuses later targets without another scan. The core is an
accelerator's licence, never a certificate: every cut is still established by the certified window decision, and
grammars without a proved core keep the exhaustive walk unchanged. Construction cost is discussed in limits.md.

## 9 Evaluation

The artifact runs the evaluation in the default test target and CI; the figures below are asserted rather than merely
printed, so a drifted number fails the test suite.

Over 400 random token sets on a three-symbol alphabet, generated by the probe itself with a pinned seed and draw order,
266 are nullable and are decided through their positive-width equivalent, exactly as the artifact compiles them; 63
certify at least one byte exactly. Of the 337 that certify no byte, **326 gain a certified window under the model, and
322 of those are witnessed**: for each, the bounded search finds a completely tokenizable input containing a certified
window, with the covering token beginning at the reported origin, verified as each input is constructed, with the 322
aggregate asserted; 4 model-positive grammars have no occurrence within the bounded witness search and are reported as
unresolved, never as rescues; 11 exhaust the quotient with no window under the model, and none are inconclusive.
Occurrence is a property of the concrete word rather than its quotient key, so the witness search continues past the
shortest certified length instead of stopping at the first certifying word. Separate rewind-stress rows exercised
1,079,392 generated executions that scanned through the window and contained at least one rewind, with zero
disagreements against the shipped scanner; 418,466 of those executions tokenize their whole input completely and the
remaining 660,926 have malformed suffixes past the window, both counts asserted. This is an implementation stress check:
the generated inputs were required to scan through the window, not to tokenize completely. The random sweep additionally
checked every certified two-byte window over the probe's generated contexts. The length-one case reproduces the
published certificate on all 400 grammars, the 266 nullable ones included.

Named token sets: all six exact-empty rows of the predecessor's applicability table, five C-like variants and JSON, gain
witnessed windows, and one new cumulative C-like variant joins them as a seventh positive row. Table 1 shows the
concrete windows, with the retained key counts, one indicator of the search footprint, in place of the `6^|Q⁺|` bound.
The example windows show the recovery anchors: the string and line-comment rows resynchronize at a newline followed by a
byte that must begin a token, and the block-comment rows at the byte pair `*/` followed by whitespace, in comment
context the closer, with the origin immediately after the pair. The certificate is occurrence-universal, so it also
covers occurrences where `*/` reads as two operator tokens; the pinned witnesses exercise exactly that reading. The
conventional row certifies at length two: its whitespace runs include the newline, so `!` must begin a token there as
well. The JSON row uses the RFC 8259 lexical forms (Bray, RFC Editor 2017) over bytes and assumes UTF-8 validity; it is
not a conforming JSON processor. How often such windows occur in real corpora is an empirical question for the
measurement campaign, and no frequency claim is made here. The run `a⁺` is included as the negative row, and `a*` would
be decided as the same automaton: every byte continues a run as readily as it begins one, the model certifies no window,
since absent-byte windows certify only vacuously under Definition 1, and the search exhausts its quotient, which is
precisely the shape of a model-negative.

**Table 1.** Certified windows for the named token sets, from the gate's asserted rows: the shortest model-certified
length, one example window with its origin, and the quotient keys the search retained before shortest-window stopping.
Every positive row's displayed window carries an asserted occurrence witness; among the positive rows the cumulative one
is new to this study, and every other positive row is an exact-empty row of the predecessor's table.

| Token set                      | Shortest model-certified `k` | Example window at origin | Retained keys |
|--------------------------------|-----------------------------:|--------------------------|--------------:|
| C-like, string literals        |                            2 | `\n!` at 1               |            24 |
| C-like, line comments          |                            2 | `\n!` at 1               |            18 |
| C-like, block comments         |                            4 | `\t*/\t` at 3            |            53 |
| C-like, conventional           |                            2 | `\n!` at 1               |            27 |
| split-friendly, block comments |                            4 | `\n*/\t` at 3            |           188 |
| C-like, cumulative (new here)  |                            4 | `\n*/\t` at 3            |           189 |
| JSON, RFC 8259                 |                            2 | `\t"` at 1               |            69 |
| `a⁺` (negative row)            |                         none | search exhausted         |             3 |

## 10 Related work

The window generalizes the certified split point of the predecessor
([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)), and inherits its relation to the parallel lexing families:
composition carries every state and pays for it (Mytkowicz, Musuvathi and Schulte, ASPLOS 2014); speculation predicts an
entry state, validates, and re-executes on a miss (Prabhu, Ramalingam and Vaswani, PLDI 2010); bounded alternative-state
scans disambiguate without single-state speculation (Barenghi, Crespi Reghizzi, Mandrioli, Panella and Pradella, Science
of Computer Programming 2015); prescanning pays a pass over the input (Li, Sato, Liu and Taura, IPDPS 2021); the window,
like the byte, is derived from the grammar before any input exists. A close compiler-style neighbor in the maximal-munch
setting is the streaming analysis of Li, Yang, and Mamouras (ASPLOS 2026), which statically computes a grammar's maximum
token-neighbor distance and uses the resulting bounded lookahead to emit a sequential maximal-munch stream without
backtracking; their scan advances from a known token boundary, so the window decides maximality rather than recovering
the origin of the token covering an arbitrary occurrence, and parallelization is left there as future work. On the
formal side of the same setting, ZipLex formally verifies linear-time invertible maximal-munch lexing, with an
abstraction capturing the separability of tokens in a sequence (Chassot and Kunčak, CAV 2026), and Li and Mamouras
formalize the uniform tokenization problem and give `O(mn)`-time algorithms, linear in text length `n` for a fixed
grammar of size `m`, precomputing what each suffix admits in a right-to-left pass before tokenizing from the input's
start (OOPSLA 2025); neither line of work derives an occurrence-universal raw-window certificate or recovers the
covering token's origin from an arbitrary occurrence without scanning from a known boundary. Lester's boxing check
anticipates the flavor at a known join: every lexer state possible after the first analyzed string must either already
emit a token, making the following character irrelevant to that string's lexing, or emit the same token immediately on
every character that may begin the second, so the join is a lexeme boundary and concatenation preserves token
boundaries, stated compactly in the position paper and in full in the journal treatment (Lester, PLAS 2013; Lester, Ong
and Schäfer, Journal of Computer Security 2016); the check is per-join over two analyzed fragments rather than a
certificate over every occurrence of a grammar-derived word, and it does not recover a covering origin. Recent
LLM-tokenizer work addresses input-specific seams instead: LoPT validates position-aligned tokenizations of overlapping
chunks and adjusts chunk length where needed (Shao, Zheng, Wang, Zheng, Li and Fan, ACL 2026), and for BPE, recent work
bounds the streaming delay of ordered merge rules from a known beginning (Mamouras, Li and Yang, PLDI 2026) or maintains
the tokenization incrementally over every prefix (Jiang and Gong, ICML 2026); Hayase, Liu, Smith, and Oh enumerate, for
a byte prefix of BPE-tokenized text, the tokenizations whose last token straddles the prefix end, a valid covering tree
over one concrete input (ICML 2026). The certificate here is grammar-derived and occurrence-universal rather than
input-relative and enumerative, and it fixes one origin for every occurrence. The parsing side of that pipeline is
active again: cyclic operator precedence grammars (Chiari, Mandrioli and Pradella, Information and Computation 2025)
admit parallel-suitable chunking of flat unbounded terminal-string substructures, with precedence relations defined
between adjacent terminals; in a conventional pipeline those terminals are lexer output, so that application presupposes
tokenization, though the formalism itself is not restricted to pre-tokenized input. A classical antecedent of
window-determines-state is the definite automaton, studied in depth by Perles, Rabin, and Shamir (IEEE Transactions on
Electronic Computers 1963); its operational form for a fixed `k` is `k`-locality: any `k` consecutive symbols force a
unique state, every word of length `k` being synchronizing (Holub and Štekr, CIAA 2009). Both quantify uniformly over
all windows and speak of states; the certificate here is per-window, speaks of token boundaries under maximal munch, and
recovers an origin: raw-state synchronization alone does not identify the start of the covering maximal-munch token. The
classical special case of boundary recovery is code synchronization, and Proposition 1 states the relation exactly for
prefix codes, where left-to-right tokenization realizes the code factorization: the certified windows are the
synchronizing splits whose right half sits inside one codeword, and a synchronizing pair (Berstel, Perrin and
Reutenauer, Cambridge University Press 2010) is a certified window whose halves are in the code. The oldest member of
that family is the comma-free code of Golomb, Gordon and Welch (Canadian Journal of Mathematics 1958), a block code no
codeword of which occurs across the boundary of two adjacent codewords, so that every codeword is a certified window
with origin 0 for the code's own factorization. Finite synchronization delay (Restivo, Theoretical Computer Science
1975) bounds how many codewords a synchronizing pair needs, hence a byte bound for a finite code; the `ww`-style
resumption argument survives into the partial-DFA treatment (Berlinkov, Ferens, Ryzhikov and Szykuła, DMTCS 2026). The
explicit modern bounded-window form of that special case is the synchronizing morphism: a window of bounded length
suffices to detect boundaries between codewords (Fici, Romana, Sciortino and Urbina, MFCS 2025), in a morphic
code-factorization setting rather than among competing prioritized token languages under maximal munch. Uniquely
decipherable codes may share prefixes; what they guarantee is a unique factorization, with no maximal-munch priority
resolving overlaps between competing token languages, and that difference is where the origin machinery here earns its
existence. The relationship to reset words is one-way and stops at length one: a useful certified byte induces a
reset-like action on the partial live automaton with domain `{q0}`, under the stated re-entrancy qualification, a
correspondence that fails under the classical complete-DFA reading (Volkov, LATA 2008). It does not extend: a certified
window need not be a reset word of the token DFA at all, since over `{a, b}` the window `ab` certifies at origin 1 while
the action of `ab` on the partial automaton is empty, and a rank-one letter need not certify, since over the token
language `b*a` the letter `b` can act with rank one while `q0` is re-entrant and `b` occurs inside tokens. Certified
windows synchronize token-origin information in an enriched cloud model; they need not synchronize the raw token DFA.
The complexity of the neighborhood is known: checking careful synchronizability of a partial automaton, and finding a
shortest carefully synchronizing word, are PSPACE-complete already over two-letter alphabets (Martyugin, CSR 2010),
careful meaning the word stays defined from every state and maps all states to one; that is context, not a bound, and no
hardness result is claimed for the window problem here, whose per-grammar retained-key counts, one footprint indicator
rather than a cost model, stayed far below the worst case throughout.

Two practices are the practical counterparts. Compiler panic-mode recovery discards input to a recovery set (Aho, Lam,
Sethi and Ullman, Addison-Wesley 2006), which may itself be grammar-derived; the distinction is post-error parser
recovery against a pre-input lexical boundary guarantee. Incremental lexing in the style of Wagner and Graham restarts
from per-token scanner-state snapshots in a versioned token stream, with dynamically maintained lookahead dependencies,
exact for the text they were cached against (Wagner and Graham, manuscript 1997); a certified window is
grammar-universal instead, valid in every completely tokenizable context and known before any input exists, at the price
of existing only where the token set admits one.

Very recent concurrent work establishes nearby but different local guarantees. TokTier certifies input-relative splice
junctions for selected frozen pre-tokenizer and BPE pipelines: it matches cached against fresh runs, uses
family-specific synchronizing character-class transitions proved to reset the pre-tokenizer under every left context,
and proves exact recombination of its overlapping windows (Zhang and Cao, arXiv:2607.29678, 2026). ReTokSync monitors
the receiver-view tokenization of one concrete generated stream and triggers a corrective reset when ambiguity occurs
(Wang, Wang, Pang, Han, Qi, Hu and Chen, arXiv:2604.25486, 2026). Borsotti, Crespi Reghizzi, and Pradella define
precedence relations over synthesized attributes for a deterministic parser using one-symbol left and right
neighborhoods (SSRN 2026). None decides occurrence-universal bounded words for competing prioritized token languages
under maximal munch, nor recovers the origin of the token covering an arbitrary cut.

Two formal treatments of tokenization itself address a different question. Kaplan models a language's tokenizing
conventions as a finite-state transduction and emits output at input-relative pinch-points, positions where all live
analysis paths converge to a single transducer state, so settled prefixes stream out as a particular text is processed
(Kaplan, CSLI Publications 2005). Cognetta and Okazaki encode the tokenizations of a regular language as finite-state
transduction and represent canonical MaxMatch and BPE subword tokenizers as transducers (Computational Linguistics
2025). Neither derives occurrence-universal windows from the grammar alone, and neither recovers the origin of the
covering maximal-munch token at an arbitrary occurrence.

Symbolic dynamics comes closest in shape. A resolving block is a block of a factor subshift all of whose admissible
preimages agree at a selected coordinate (Adler, Coppersmith and Hassner, IEEE Transactions on Information Theory 1983;
Marcus, IEEE Transactions on Information Theory 1985): an occurrence-universal lift of a local observation to hidden
state, introduced as a means of resetting an encoding automaton in sliding-block code construction. A certified window
shares that shape, reading the window as the observable block and the covering token's start as the hidden coordinate;
the classical theory, however, is stated for factor maps of subshifts, without token priorities, maximal munch, backup,
or any tokenization semantics, and it neither defines nor decides the lexical instantiation.

Across these areas, definite and local automata, synchronizing words and codes under both the complete and the partial
reading, resolving blocks, careful synchronization, streaming, verified, and uniform maximal-munch tokenization,
incremental relexing, the parallel lexing families, and the parallel parsing line above, and beyond the prefix-code and
code-factorization special case, where synchronizing pairs and bounded windows already recover codeword boundaries
(Restivo, Theoretical Computer Science 1975; Fici, Romana, Sciortino and Urbina, MFCS 2025), we have not found prior
work that defines or decides in general, for competing prioritized token languages under maximal munch, a
grammar-derived bounded word whose every occurrence identifies the origin of the covering token at an arbitrary cut, nor
prior work instantiating resolving-block or local-decoding machinery for prioritized maximal-munch token origins, nor a
derivation of this conservative compiled-table cloud model; the closest results decide maximality from a known boundary,
bound retained memory, assume the boundaries they schedule, recover a state without an origin, or validate a concrete
overlap or splice after local retokenization. We are likewise not aware of an implementation that derives certified
windows from a compiled token set and checks them against a running scanner, other than the instruments this paper
reports and their supported successor in release 1.4.0.

## 11 Limitations

All deliberate. The soundness proof speaks only of completely tokenizable inputs: malformed input is outside the proof,
and no consumed-prefix analogue is claimed. Negatives are model-relative: the model refuses windows a greedy scanner
would allow, so an exhausted search means no window *under this model*, never that none exists. The worst case is
exponential and the probe is budgeted, though the retained keys stayed below 200 on every named row, at most 32 with
mean 9.9 among the 337 no-byte grammars. The evaluated v1.3.3 artifact ships no window-planning API: its certificate
machinery is a probe, on the principle that the certificate's formulation should freeze in this paper before becoming a
public contract. Release 1.4.0, published after submission, made the formulation a public contract; that implementation
lies outside this paper's evaluation. And no representative real-corpus evidence exists yet; window occurrence frequency
is the measurement campaign's question.

## 12 Conclusion

A certified split window `(W, o)` of a compiled token set is a byte string such that in every completely tokenizable
input containing `W`, the token covering the occurrence's final byte begins exactly `o` bytes into it. Model
certification is decided from the compiled tables alone by running a conservative cloud of token-prefix hypotheses
across the window, seeding new trajectories only where the automaton accepts, and demanding unanimity on an in-window
origin. The soundness argument is a representation invariant: the final segmentation's actual token-prefix history is
always among the hypotheses, so unanimity can only land on the truth, and maximal-munch backup never needs simulating,
because the model tracks where tokens begin rather than where the read head wanders.

The search is a decision procedure for the model, not a heuristic: a quotient of the reachable clouds, exact under the
stated single-occupancy invariant, makes breadth-first search terminate by exhaustion, so every answer is a certified
window with its origin or a proof that the model admits none at any length. The three-layer honesty is deliberate and
permanent. Model-positive answers are semantically certified within the stated flat, completely-tokenizable scope.
Negatives are model-relative: the model refuses windows a greedy scanner would allow, the two asserted witnesses exhibit
the over-approximation, and the strictness corollary locates its minimum nonvacuous length at two, since at length one
the model provably coincides with the published predicate, made exact for occurring bytes by the predecessor's necessity
theorem. Over a prefix code the certificate is code synchronization under another name, the synchronizing splits whose
right half sits inside one codeword, and the studied token sets are exactly the ones that are not codes. A token set in
which some token matches the empty string is decided through its positive-width equivalent, which unrolls the accepting
start state and changes no scan, so nothing is set aside on that account.

The generalization does what it was built for: it recovers every exact-empty row of the predecessor's table, all
witnessed. Of the 337 random token sets certifying no byte, 322 gain a witnessed certified window with zero inconclusive
searches; the predecessor's six exact-empty rows gain witnessed windows of two to four bytes, with the concrete windows
in Table 1. Separate rewind-stress rows exercised 1,079,392 generated executions that scanned through the window and
contained at least one rewind, with zero disagreements against the shipped scanner, and the figures are printed and
asserted in the artifact, so they fail the test suite if they move. What this paper deliberately does not deliver is the
cut itself: the evaluated v1.3.3 planner uses single-byte certificates only, and the window-planning sibling that
release 1.4.0 added after submission is outside this evaluation; turning a window's boundary into a parallel cut is an
explicit composition with the predecessor's prefix-stability result, and whether windows occur often enough in real
corpora to plan balanced chunks is an empirical question on which this paper reports and relies on no controlled
evaluation; the artifact archives exploratory preview measurements only. That evaluation, on representative corpora with
planning times, occurrence frequencies, and end-to-end comparisons against byte certificates, is the natural next step
and is not claimed here.

The contribution is therefore narrow and exactly bounded, in the same sense as its predecessor: not another way to
recover context, but the certification step, extended from single bytes to bounded windows, with the origin recovered
and the conservatism exhibited. Certified windows synchronize token-origin information in an enriched cloud model, and
need not synchronize the token DFA itself; the predecessor's one-way reset-word connection applies at length one, and no
general implication extends to longer windows.

## References

- N. Nidhögg. *Certified Split Points for Parallel Lexing: Exact
  and Modulo Discarded Tokens.* Preprint, arXiv:2608.03473, 2026.
- N. Nidhögg. *munch.* Release tag v1.3.3, archived at doi:10.5281/zenodo.21842344, 2026; a lexical
  analysis library based on automata theory, and the probes reported here ship in this release.
- T. Mytkowicz, M. Musuvathi, W. Schulte. *Data-Parallel Finite-State Machines.* ASPLOS 2014, 529-542.
- P. Prabhu, G. Ramalingam, K. Vaswani. *Safe Programmable Speculative Parallelism.* PLDI 2010, 50-61.
- A. Barenghi, S. Crespi Reghizzi, D. Mandrioli, F. Panella, M. Pradella. *Parallel
  parsing made practical.* Science of Computer Programming 112:195-226, 2015.
- L. Li, S. Sato, Q. Liu, K. Taura. *Plex: Scaling Parallel
  Lexing with Backtrack-Free Prescanning.* IPDPS 2021, 693-702.
- A. W. Li, Y. Yang, K. Mamouras. *Static Analysis for Efficient Streaming Tokenization.* ASPLOS 2026, 1880-1896.
- S. Chassot, V. Kunčak. *Formally Verified Linear-Time Invertible Lexing.* CAV 2026, LNCS 16683, 141-164.
- A. W. Li, K. Mamouras. *Efficient Algorithms for the Uniform
  Tokenization Problem.* PACMPL 9(OOPSLA1), article 133, 1492-1518, 2025.
- M. M. Lester. *Position Paper: The Science of Boxing.* PLAS 2013, 83-88.
- M. M. Lester, L. Ong, M. Schäfer. *Information Flow Analysis for a Dynamically Typed
  Language with Staged Metaprogramming.* Journal of Computer Security 24(5):541-582, 2016.
- W. Shao, L. Zheng, P. Wang, P. Zheng, J. Li, Y. Fan. *LoPT: Lossless Parallel Tokenization Acceleration
  for Long Context Inference of Large Language Model.* ACL 2026, 33107-33122. Preprint: arXiv:2511.04952.
- K. Mamouras, A. W. Li, Y. Yang. *An Efficient Algorithm for Streaming
  BPE Tokenization.* PACMPL 10(PLDI), article 252, 2085-2108, 2026.
- S. Jiang, R. Gong. *Incremental BPE Tokenization.* Accepted to ICML 2026 (Spotlight). Preprint: arXiv:2605.30813.
- J. Hayase, A. Liu, N. A. Smith, S. Oh. *Sampling from Your Language Model One
  Byte at a Time.* ICML 2026. Preprint: arXiv:2506.14123, version 3 of 7 May 2026.
- Z. Zhang, Z. Cao. *TokTier: Exact Stateful CPU+GPU Tokenization for Agentic
  LLM Serving.* Preprint, arXiv:2607.29678, version 2 of 3 August 2026.
- Y. Wang, R. Wang, W. Pang, J. Han, Y. Qi, D. Hu, K. Chen. *ReTokSync: Self-Synchronizing Tokenization
  Disambiguation for Generative Linguistic Steganography.* Preprint, arXiv:2604.25486, 2026.
- A. Borsotti, S. Crespi Reghizzi, M. Pradella. *Attribute-Based Precedence Relations
  for Context-Free Grammars.* Preprint, SSRN, doi:10.2139/ssrn.6890975, 2026.
- M. Chiari, D. Mandrioli, M. Pradella. *Cyclic operator precedence grammars
  for parallel parsing.* Information and Computation 307:105363, 2025.
- M. A. Perles, M. O. Rabin, E. Shamir. *The Theory of Definite Automata.*
  IEEE Transactions on Electronic Computers EC-12(3):233-243, 1963.
- J. Holub, S. Štekr. *On Parallel Implementations of Deterministic Finite Automata.* CIAA 2009, LNCS 5642, 54-64.
- J. Berstel, D. Perrin, C. Reutenauer. *Codes and Automata.* Encyclopedia
  of Mathematics and its Applications 129, Cambridge University Press, 2010.
- S. W. Golomb, B. Gordon, L. R. Welch. *Comma-Free Codes.* Canadian Journal of Mathematics 10:202-209, 1958.
- A. Restivo. *A Combinatorial Property of Codes Having Finite
  Synchronization Delay.* Theoretical Computer Science 1(2):95-101, 1975.
- M. V. Berlinkov, R. Ferens, A. Ryzhikov, M. Szykuła. *Synchronization of Strongly Connected
  Partial DFAs and Prefix Codes.* DMTCS 28:2, 2026. Extended version of the STACS 2021 paper.
- G. Fici, G. Romana, M. Sciortino, C. Urbina. *Morphisms and BWT-Run Sensitivity.* MFCS 2025, LIPIcs 345, 49:1-49:18.
- M. V. Volkov. *Synchronizing Automata and the Černý Conjecture.* LATA 2008, LNCS 5196, 11-27.
- P. V. Martyugin. *Complexity of Problems Concerning Carefully Synchronizing
  Words for PFA and Directing Words for NFA.* CSR 2010, LNCS 6072, 288-302.
- A. V. Aho, M. S. Lam, R. Sethi, J. D. Ullman. *Compilers: Principles,
  Techniques, and Tools.* 2nd edition, Addison-Wesley, 2006.
- T. A. Wagner, S. L. Graham. *General Incremental Lexical Analysis.* Manuscript, University of California, Berkeley,
  1997; the circulating build is the December 1999 revision. https://harmonia.cs.berkeley.edu/papers/twagner-lexing.pdf
- R. M. Kaplan. *A Method for Tokenizing Text.* In Inquiries into Words, Constraints and Contexts:
  Festschrift for Kimmo Koskenniemi on his 60th Birthday, CSLI Publications, 55-64, 2005.
- M. Cognetta, N. Okazaki. *Tokenization as Finite-State Transduction.* Computational Linguistics 51(4):1119-1149, 2025.
- R. L. Adler, D. Coppersmith, M. Hassner. *Algorithms for Sliding Block Codes: An Application of
  Symbolic Dynamics to Information Theory.* IEEE Transactions on Information Theory 29(1):5-22, 1983.
- B. H. Marcus. *Sofic Systems and Encoding Data.* IEEE Transactions on Information Theory 31(3):366-377, 1985.
- T. Bray (Ed.). *The JavaScript Object Notation (JSON) Data
  Interchange Format.* RFC 8259, STD 90, RFC Editor, December 2017.
