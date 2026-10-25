# Certified Split Windows for Parallel Lexing: Recovering Boundaries Where No Byte Certifies

**Nicklas Nidhögg**, August 10, 2026. Mirrors `paper/split-windows/split-windows.tex` as it now stands, revised beyond
the source published as [arXiv:2608.09761](https://arxiv.org/abs/2608.09761). The paper evaluates munch at the v1.3.3
release, where the window machinery is a probe: the paper's principle is that the certificate's formulation should
freeze there before becoming a public contract. The supported API arrived in release 1.4.0, two days before the paper's
submission: `Lexer::is_split_window()`, which the revised sweep cross-checks against its model, and
`chunk_boundaries_with_windows()`, which lies outside the paper's evaluation. The paper makes three later version
statements. Release 2.1.0 decides the boundary profile of Section 11 exactly within its search cap, as
`boundary_profile()` and the two searches it asks at every gap. Release 2.2.0 ships the chosen-origin search of Section
6.2 as `shortest_split_window()`, under a node budget. And the boundary-profile campaign of Section 11 ran at munch
commit `df818b6a224221cf964095dd8e2c20735f506701`; its record lies outside this repository and no test asserts it. A
token set in which some token matches the empty string is inside the paper's scope: Lemma 1 decides it through its
positive-width equivalent, the same automaton entered through a start state that does not accept, which changes no scan,
and the random sweep decides its 266 nullable sets that way rather than setting them aside.

Two parts are this document's addition alone. The mandatory core section derives machinery release 1.5.0 adds on top of
the same certificates, and no version of the paper contains it. Release 2.2.0 also decides `anchor_free_span()` over an
inventory of windows of any length, which the paper does not name: the walk guesses the anchor-free stretch rather than
buffering the bytes a window may still reach back over. [docs/usage.md](usage.md) and the [README](../README.md)
document both of release 2.2.0's additions. Every empirical aggregate of the evaluation is printed and asserted by the
window gate, `tools/probes/src/window_gate.cpp`, and every figure of Section 6 by the figure program
`paper/figures/window_search.cpp`, so a drifted number fails the test suite.
`ctest --test-dir build -R "munch_window_gate|munch_window_search" --timeout 300` runs both from a configured build. The
tables below are produced mechanically from the paper's own tabular sources, never transcribed by hand.

*A technical note on the certificate behind the window layer. The implementation, tests, and probes live in this
repository; this document states the idea precisely, relates it to prior work, and shows what it recovers.*

## Abstract

A certified split point lets a parallel lexer cut unlexed input at a single byte with the serial token stream provably
preserved, but several conventional token sets in the predecessor's controlled study certify no byte once string,
comment, or whitespace-run forms are included ([arXiv:2608.03473v2](https://arxiv.org/abs/2608.03473v2)). We generalize
from a byte to a bounded window, a byte string after which the position where the current token began is known,
regardless of surrounding context. We certify the directly usable form of that recovery, namely that the token covering
the window's final byte begins at the returned origin. The certificate is conditional on occurrence and may be vacuous;
every applicability figure counts only windows carrying an asserted completely tokenizable occurrence witness. We give a
conservative model of a maximal-munch scanner's possible histories across a window, prove it sound, and decide
reachability in that model exactly by exhausting a finite quotient of its reachable configurations. Within the flat,
completely tokenizable scope, model-positive answers are semantic certificates; negatives are relative to the model,
which deliberately refuses some windows a greedy scanner would allow. Whether the model certifies any window, plain or
witnessed, is PSPACE-complete, and a shortest window can need `2^Ω(√n)` bytes over `n` live states. Two smaller searches
decide plain model existence, the second shipped in release 2.2.0 of munch. In a sample of 400 random token sets, 322 of
the 337 sets certifying no byte gain a witnessed window, with zero inconclusive searches, and every exact-empty row of
the predecessor's study gains a witnessed window of two to four bytes. The analysis runs once after automaton
construction, using only the compiled tables and no input. A boundary profile marks each gap of a window forced, crossed
or open, and a window-local cut is sound exactly at forced gaps.

## 1 Introduction

The predecessor of this paper derives, from a compiled token set, the complete set of bytes at which unlexed input can
be cut with the serial token stream preserved, and proves its condition necessary as well as sufficient
([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)). Its sharpest limitation is its own applicability table, in which
several of the studied conventional token sets certify *no* byte because a single string form, comment form, or
whitespace run gives some non-initial live state a transition on every candidate. A fresh 400-grammar random sweep
generated for this study reproduces the pattern, 337 of its 400 sets certifying none. This paper is about the object
that refusal leaves standing, a byte *string* after which the start of the token covering the string's final byte is
pinned, regardless of surrounding context.

Concretely, we ask: for a token set compiled to a DFA and scanned by maximal munch, is there a window
`W = w0 ... w(k-1)` and an offset `o` with `0 <= o < k` such that in *every* completely tokenizable input containing
`W`, the token covering the occurrence's final byte begins exactly `o` bytes into it? A worker that finds `W` in a
completely tokenizable input may then begin scanning at the recovered boundary with no speculation, no state
enumeration, and no DFA-state-recovery pass over the input, as at a certified byte, which is the `k = 1` case.

The contributions are:

- A conservative model of the scanner's possible token-prefix histories across a window, replacing a refuted natural
  predecessor, and a soundness proof by a representation invariant (Section 3, Section 4).
- A finite quotient of the model's reachable configurations, exact for them by a stated single-occupancy invariant,
  turning breadth-first search over windows into a terminating decision procedure for the model (Section 5).
- Two specializations: at length one the model certifies exactly the bytes the published predicate certifies, so the
  multi-byte construction is that predicate's conservative continuation; and over a prefix code the certified windows
  that occur are exactly the synchronizing splits of code theory whose right half sits inside one codeword, which places
  the classical case as the special case it is (Section 7).
- Two smaller exact searches for the model (Section 6): a quotient of at most `3^n` keys over `n` live states, and an
  unambiguous automaton of at most `(n + 2) · 2^(n-1)` states that follows one chosen origin. The second returns a
  shortest certified window, bounds its length, and is the search release 2.2.0 of munch ships as
  `shortest_split_window()`.
- A proof that existence of a model certificate, plain or witnessed, is PSPACE-complete (Section 6.3), by a reduction
  from DFA intersection non-emptiness with one token kind and five symbols that fixes the minimum window length at
  `m + 3` plus the length of the shortest word that all `m` source DFAs accept. A family of such token sets has shortest
  windows of length `2^Ω(√n)`, with exact minima 4, 6, 11, 18, 67, 68, 429, 850 and 2,531 for its first nine members,
  searched by a figure program.
- A characterization of the cloud (Section 9), whose hypotheses are exactly those of arbitrary token factorizations of
  concatenations of tokens, greedy or not. From this the vacuous window follows, token kinds may be erased before any
  search, and the model is exact wherever every factorization is the scan's, a class decided in polynomial time that
  holds the prefix codes strictly. A shortest witnessed model certificate is found exactly with its occurrence, by a
  product with the verifier of the companion paper on certified splitting
  ([arXiv:2610.08854](https://arxiv.org/abs/2610.08854)), called the splitting companion below, and whether the model
  misses any witnessed semantic certificate at all, decidable already from the regularity Section 13 notes, is decided
  by an explicit construction that returns a shortest missed window.
- An evaluation over all six exact-empty rows of the prior study's applicability table, those certifying no useful byte
  under the exact condition, one new cumulative variant, and 400 random token sets generated for this paper: the
  witnessed rescue rate where no byte certifies, the window lengths that suffice for the studied C-like and JSON
  tokenizations, and rewind-stress checks of 1,079,392 generated executions that scanned through the window and
  contained at least one rewind, 418,466 of them completely tokenizable, with zero disagreements against the shipped
  scanner (Section 12).
- A boundary profile of a window, the verdict forced, crossed or open at each of its gaps over every occurrence, with
  the certificate as one of its patterns, and a proof that a window-local cut rule that reconciles nothing is sound
  exactly at forced gaps (Section 11). Over a token set with a longest token, a window containing a forced gap forces
  every boundary the scan from that gap commits with its lookahead inside the window, while no single width decides
  every gap of the JSON tokenization, since no bounded window sees whether an unmatched quote lies to its left. Release
  2.1.0 of munch decides the profile exactly within its search cap, and an exploratory campaign over JSON, C-like and
  byte-pair token sets measures what it adds to the certificate.

The probe decides certificates and model-search results offline from the compiled tables after automaton construction;
the occurrence witnesses and scanner checks are generated executions against the shipped scanner. Executable checks in
the munch repository (doi:10.5281/zenodo.21842344) print and assert every figure of the evaluation (Section 12) and of
the long-window family (Table 2), so a drifted number fails the test suite. Section 12 names the programs and the
evaluation versions. The boundary-profile measurements of Section 11 are an exploratory campaign that no test asserts.

## 2 Preliminaries

We use the scanner model of the predecessor. A token set compiles to a DFA `A` with initial state `q0` and is scanned by
maximal munch, in which the scanner runs `A` from the current position as far as a transition exists, emits the token of
the last accepting configuration passed, resumes immediately after it, and restarts in `q0`. An input is *completely
tokenizable* when this process consumes it exactly. The setting is *flat*, meaning one fixed token set compiled to one
DFA scanned from one fixed `q0`, with no lexical modes, no mode stack, and no semantic scanner state. `A⁺` denotes the
live subautomaton, whose states are those both reachable from `q0` and co-accessible to acceptance and whose transitions
are those between live states. Write `Q⁺` for its state set. A token may match the empty string. The scan never emits
it, and the next lemma lets every statement below assume a start state that does not accept.

**Lemma 1 (positive-width equivalent).** Let `A` be the DFA of a token set whose start state `q0` accepts, and let `A'`
be `A` with a fresh start state `q0'` that carries `q0`'s outgoing transitions and does not accept, `q0` itself
retained. Then the maximal-munch scans of `A` and `A'` agree on every input, `A'` accepts exactly the nonempty words `A`
accepts, and no transition of `A'` enters `q0'`.

*Proof.* Under the scan's convention only an accepting configuration reached after consuming a byte determines an
emitted token, so every emitted token has positive width and a scan records an acceptance only after consuming a byte.
From `q0` and from `q0'` the first byte leads to the same state, since `q0'` carries `q0`'s transitions, and from there
the two scans traverse the same states and record the same accepting positions. The emitted token and the restart offset
agree, and both scans restart in their own start state. The accepted words of `A'` are the words of length at least one
accepted by `A`, the empty word being lost because `q0'` does not inherit `q0`'s acceptance. No transition of `A`
targets `q0'`, which is fresh, and the copied transitions target `q0`'s successors, so nothing enters `q0'`. ∎

Throughout, a token set whose start state accepts is read through `A'`, and the artifact compiles every such set this
way before deciding anything about it, while a set whose start does not accept passes through unchanged. The unrolled
automaton has one state more and the same scan, so nothing below loses generality by assuming that `q0` does not accept.
From here on `A` and `q0` name the automaton so read and its start, which is `q0'` for a nullable set. For a nullable
set the fresh start is never re-entered, and for any other set the re-entrancy of `q0` remains the separate condition
that Section 7 treats.

**Definition 1 (certified split window).** Let `W = w0 ... w(k-1)` with `k >= 1` and let `o ∈ {0, ..., k-1}`. The pair
`(W, o)` is a *certified split window* for a token set when, for every completely tokenizable input `x` containing `W`
at offset `t`, the token of the maximal-munch tokenization of `x` that contains the byte at offset `t + k - 1` begins at
`t + o`.

**Definition 2 (witnessed window).** A certified split window `(W, o)` is *witnessed* when some completely tokenizable
input contains `W`.

Witnessed windows are what the splitting companion calls *occurring*. Definition 1 quantifies over the inputs containing
`W`, so it holds vacuously when no completely tokenizable input contains `W`, and the vacuity is not hypothetical. Over
`{0, 00, 01}` the model below certifies the window `1001` at origin 2, yet no completely tokenizable input contains
`1001`. Every `1` is the tail of a greedily chosen `01`, so a token boundary follows the window's first byte, maximal
munch must then consume `00`, and the final `1` sits at a boundary where no token starts. That argument proves
non-occurrence, and the certificate holds because `1001` occurs in a factorization of `01001` into tokens that the scan
does not take, as Section 9 explains. The artifact asserts the model prediction and the empty result of its bounded
targeted witness search. Every applicability figure in this paper counts only witnessed certificates. The named rows pin
their witness inputs in the artifact, and for each of the 322 witnessed random grammars the probe constructs and
verifies a concrete completely tokenizable input, with the 322 aggregate asserted; a certificate the bounded witness
search cannot witness is counted as unresolved, never as a rescue.

For `k = 1` Definition 1 is the boundary guarantee of a certified split point, since the token containing the single
byte begins at it. Three guarantees should be kept apart, and this paper certifies the strongest. Model unanimity
implies that the covering token's origin is fixed, which in turn implies that some fixed safe boundary exists inside the
window, and neither converse holds. Over `{a, ab, b}` at `ab` the covering origin is fixed yet the model refuses
(Section 8); over `{a, abx, b, x}` at `ab` the byte `a` always begins a token, so offset 0 is a fixed safe boundary, yet
the covering token's origin is not fixed, since the final `b` belongs to `b` in the input `ab` and to `abx` in `abx`.
The weakest guarantee is already usable, since a worker can cut at the known boundary and rescan the window's suffix,
and release 2.1.0 of munch decides it exactly within its search cap (Section 11). Its `boundary_profile()` classifies
every gap of an occurring window as `must`, `never` or `may`, a boundary at every occurrence, at none, or at some. The
covering-token form is the stronger, sufficient property this forward origin-recovery model certifies, and it hands the
worker its resumption point directly. The model therefore has two distinct sources of false negatives. Covering-origin
certification is stricter than locating some safe boundary, and the cloud conservatively represents extra segmentations
and refuses some true covering-origin certificates, `{a, ab, b}` at `ab` among them. Note also that the definition says
nothing about the tokens overlapping the window's earlier bytes, and that it does not require the boundary to be the
only one inside the window.

## 3 The model

A worker cutting blind knows only that in the final segmentation, the window's first byte is consumed from *some* live
token-prefix state, either inside a token that began at some unknown earlier offset or exactly at a boundary. The
acceptance-gated seed below carries the boundary case, including a window at the start of the input. The model tracks a
*cloud* of hypotheses `(q, ω)`, each a live state `q` paired with an origin `ω`, which is either `before` for a token
that began before the window or an in-window offset. The initial cloud `C0` is every live state paired with `before`.

Reading window byte `w_j` maps `C_j` to `C_(j+1)` by two rules:

- **Direct step.** A pair `(q, ω)` whose state consumes `w_j` into a live state moves there with its origin unchanged. A
  pair whose state cannot consume `w_j` into a live state is an impossible history and is dropped, never restarted.
- **Acceptance-gated seed.** If some pair in `C_j` is accepting, one fresh pair `(δ(q0, w_j), j)` is seeded, provided
  that target is live. The gate is sound because tokens end only where the automaton accepts and a token can begin at
  offset `j` only where the previous token could have ended or at the input start, the case the initially open gate
  represents.

One rename accompanies the direct step. Where `q0` is not re-entrant in `A⁺`, a pair stepping *from* `q0` is beginning a
token, so its origin becomes the current offset. Where a non-empty live path returns to `q0`, the rename is disabled,
since reaching `q0` then no longer identifies a boundary. This is the same re-entrancy condition the length-one
certificate carries ([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)).

Clouds are *sets* of pairs; the set semantics is load-bearing below, where two rules inserting the same pair yield one
element. The window is *certified at origin `o`* when `C_k ≠ ∅` and there is an `o ∈ {0, ..., k-1}` with `ω = o` for
every `(q, ω) ∈ C_k`. An empty cloud certifies nothing, matching the implementation, which refuses it, and the empty
cloud is absorbing, since every step of it is empty. Non-emptiness does not guarantee that the window occurs in any
completely tokenizable input, since a certified window may be vacuous under Definition 1, and Definition 2 sets the
witnessed windows apart. Agreement on the state alone is not enough, since learning that the scan is inside a string
literal is knowledge, but not a boundary. Throughout, `q0` is *re-entrant* when some live transition of `A⁺` targets it;
non-re-entrancy means no live edge enters `q0` at all.

**The discarded predecessor.** A natural variant restarts a trajectory that cannot consume the byte, treating the
failure point as a token boundary, in place of the acceptance-gated seed. That variant is refuted. Over the token set
`{a, abc, bx, x}` and window `abx`, its cloud after `ab` is the single pair carrying origin 0 toward `abc`. The byte `x`
kills that trajectory, the restart replaces it with a token beginning at offset 2, nothing else survives, and the
variant certifies `(abx, 2)`. Yet the input `abx` itself is completely tokenizable as `a` followed by `bx`, so the token
covering the final byte begins at offset 1, and the certificate is false at a witnessed occurrence. A failing trajectory
is an impossible history, not a boundary, and restarting it manufactures support for origins no execution justifies. The
repaired model certifies `abx` at origin 1, where the scanner does cut, and the case is carried as an asserted row of
the artifact.

## 4 Soundness

Fix a completely tokenizable input `x` containing `W` at offset `t`. For `j ∈ 1..k` let `σ_j` be the start of the token
that the final maximal-munch segmentation of `x` assigns to the byte at `t+j-1`, let `ρ_j = δ*(q0, x[σ_j .. t+j))` be
that token's prefix state after consuming through byte `t+j-1`, and let `ω_j = σ_j - t`, or `before` when `σ_j < t`.

**Lemma 2 (representation).** For every `j ∈ 1..k`, the pair `(ρ_j, ω_j)` is in `C_j`.

*Proof.* Every `ρ_j` is live. It is reachable through the actual token prefix, and co-accessible because its token ends
at some `e >= t+j` with `δ*(ρ_j, x[t+j .. e)) = δ*(q0, x[σ_j .. e))`, and the right side is accepting.

*Base, `j = 1`.* If `σ_1 < t`, the state `p = δ*(q0, x[σ_1 .. t))` is live and lies in `C0` paired with `before`; the
direct step carries `p` to `(ρ_1, before)`. The rename cannot interfere, since `p` has consumed at least one byte, which
makes `p = q0` possible only if a non-empty live path returns to `q0`, and that disables the rename. If `σ_1 = t`, then
`ρ_1 = δ(q0, w0)` and the seed fires, because `C0`, which is all of the live states, contains a live accepting state.
The fixed input is completely tokenizable and non-empty, so its first token traces an accepting path from `q0`, making
`q0` live with an accepting state reachable from it, and a reachable accepting state is trivially co-accessible.

*Step, `j` to `j+1`.* If the byte at `t+j` continues its token, `σ_(j+1) = σ_j`, and the direct step carries
`(ρ_j, ω_j)` to `(δ(ρ_j, w_j), ω_j)`; the rename does not overwrite the origin, by the same re-entrancy argument as in
the base. If the byte at `t+j` begins a token, the previous token ended at `t+j`, so `δ*(q0, x[σ_j .. t+j))` accepts;
that state is `ρ_j`, in `C_j` by hypothesis, so the seed fires and emits exactly `(δ(q0, w_j), j) = (ρ_(j+1), ω_(j+1))`.
∎

The lemma states containment, not equality, so the cloud may carry hypotheses no execution realizes. The surplus is
harmless in one direction and load-bearing in the other. At a realized occurrence in a completely tokenizable input,
surplus hypotheses can only preserve the actual origin's unanimity or destroy unanimity; they cannot manufacture
unanimity at a false origin.

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

Acceptance gating alone does not terminate, since over `a⁺` the automaton accepts after every byte and origins
accumulate without bound. The search deduplicates on a quotient of the cloud, which records the set `B` of states
carrying `before` and, for each state, the number of distinct in-window origins it carries, saturated at two.

**Invariant 1 (single occupancy).** In every reachable cloud, each surviving in-window origin occupies at most one
state.

An origin enters the cloud at most once per rule application, into a single state, and the deterministic step moves it
to at most one successor, so it continues in one state or dies. At offset `j` the acceptance seed and the
non-re-entrant-`q0` rename can both propose the fresh origin `j`; both target `δ(q0, w_j)`, and set semantics coalesces
the identical pair, so single occupancy survives the collision. Origins may die, which is why the invariant says at most
one. The pre-window origin differs by design, since it may occupy many states, and it is held apart as the Boolean
support set `B`. An arbitrary cloud could violate the invariant, placing one origin in two states, and there the
saturated counts could not decide unanimity; no such cloud is reachable, and the restriction is load-bearing for
everything below. The key of a cloud is the pair `κ(C) = (B, m)` with `m(q) = min(2, number of in-window origins at q)`.

**Lemma 3 (congruence, depth-aligned).** Let `C_u` and `C_v` be clouds reached by the search after words `u` and `v`. If
`κ(C_u) = κ(C_v)`, then for every byte `b` the successors `step(C_u, b, |u|)` and `step(C_v, b, |v|)` are either both
empty or both non-empty, and when non-empty, `κ(step(C_u, b, |u|)) = κ(step(C_v, b, |v|))`, with the certification
verdict agreeing on both sides. For successor-key equality alone, it is enough that the inserted offsets be *fresh*,
exceeding every in-window origin of their respective clouds; the certification verdict additionally needs legal depths,
and the search supplies both, since it steps with the word lengths `|u|` and `|v|` and every origin in a length-`ℓ`
cloud lies below `ℓ`. Freshness is not decorative. Over `{a⁺, b}` the cloud after `a` carries origin 0, and stepping on
`a` with offset 0 merges the seed into that origin while offset 1 creates a second one, so the same key would have two
different successors under arbitrary offsets.

*Proof.* The acceptance gate reads only which occupied states accept, and the occupied states are `B ∪ {q : m(q) > 0}`,
a function of the key. The new `before`-support is the live deterministic image of `B`, excluding the contribution from
`q0` when `q0` is non-re-entrant; that excluded contribution joins the fresh-origin rename handled below. Over the
single-token set `{a}`, stepping `C0` on `a` renames `q0`'s `before` origin to 0, so `B' = ∅`, where the unexcluded
image would wrongly retain `δ(q0, a)`. Consider the in-window counts at a successor state `q'`. The direct step carries
each origin from its unique state (Invariant 1), so origins arriving at `q'` from distinct predecessors are distinct.
The exact update at each target sums the old-origin contributions arriving there and adds one fresh origin if the seed
or the non-re-entrant-`q0` rename fires toward it. The seed and the rename are combined by logical or, since they denote
the same fresh origin and the cloud is a set. The update then saturates at two. When `q0` is non-re-entrant, no live
edge enters `q0`, so after the first step no reachable cloud contains `q0` with an in-window origin; for a
non-re-entrant start state the rename decision is readable from `q0 ∈ B`. Whether the rename fires is determined by
`q0 ∈ B`, which is key-readable, and by re-entrancy, which is fixed automaton data outside the key; whether the seed
fires is determined by the key's accepting support; that the fresh origin collides with no surviving origin is the
freshness hypothesis. Note that saturation commutes with this update in the only direction needed, since a saturated
state maps its surviving origins together, and a successor's saturated sum computed from saturated counts equals the
saturation of the exact sum. A state's count can also drop to zero when its transition is missing, which the update
handles as an empty contribution. Fresh origins are equivariant under renaming of origin values, and no rule ever reads
an origin's value, only equality between origins, so keys computed on either side agree. Emptiness is key-readable, and
certification of the full window is the key predicate `B = ∅` with total count exactly one, using Invariant 1 to
identify one total count with one origin in one state. ∎

One tempting simplification is false and worth flagging, namely that a state holding two origins can never contribute to
unanimity later. Over `{a⁺, b}`, after `aa` the `a`-state carries two in-window origins, as well as `before`; on `b` all
of them die while their pre-step acceptance opens the gate for one fresh `b`-origin, and the cloud is unanimous. The
true statement, and the one the lemma uses, is that co-located origins either follow the same live successor together or
all die, and that origins which die leave no trace the key must distinguish.

**Theorem 2 (decision procedure).** Breadth-first search over windows, deduplicated on keys, decides whether the model
certifies any window for a given token set, and returns the minimum certified length; it does not enumerate every
certified word. Each state contributes a factor of `2 × 3` to the key space, so at most `6^|Q⁺|` keys are ever retained;
each key expands into at most 256 successors, each computable from the key in `O(|Q⁺|)` time, for a worst case of
`O(256 · |Q⁺| · 6^|Q⁺|)` time and `O(|Q⁺| · 6^|Q⁺|)` space.

By Lemma 3 the walk cannot miss the existence of a certifying continuation, nor change the minimum certified length.
After one matched step both successor clouds sit at fresh depths again, so the lemma inducts over every common suffix;
breadth-first order retains a shallowest representative of each key, and a certifying suffix from a discarded deeper
occurrence yields an equal or shorter certificate from the retained representative. Deduplication may still skip
individual certified words; exhaustion proves the model admits none at any length. Exhaustion does not prove semantic
non-existence, because the model is conservative (Section 8).

The displayed bounds describe an abstract key-to-key search; the probe realizes the procedure with one concrete
representative cloud and word per key, using the quotient for deduplication, so its cost additionally depends on
representative size. It is budgeted, with three outcomes: certified, exhausted, and inconclusive once the retained key
count exceeds a fixed threshold of 200,000 keys. The evaluation below finds zero inconclusive searches, and the retained
key counts it gives are one indicator of the search footprint rather than a complete cost model. Section 6 replaces the
`6^|Q⁺|` bound by `3^|Q⁺|` and by `(|Q⁺| + 2) · 2^(|Q⁺| - 1)`, and shows that no polynomial algorithm decides the
question unless P = PSPACE.

## 6 Smaller searches and the complexity of the model

Write `n = |Q⁺|` for the number of live states. The quotient of Section 5 is exact and finite, and the evaluation's
retained-key counts stand against its `6^n` bound. Two smaller presentations decide the same question. The first
coarsens the key to three statuses per state; the second guesses the one origin that survives and is an unambiguous
automaton over the certified windows. The second is the search release 2.2.0 of munch ships, and it is also the route to
the model's exact complexity. Existence of a model certificate is PSPACE-complete, and a shortest window can need
`2^Ω(√n)` bytes.

### 6.1 A three-status quotient

Fix a reachable cloud. A live state is *empty* when no pair occupies it, *single* when exactly one pair occupies it and
that pair carries an in-window origin, and *blocked* otherwise, when it carries `before` or at least two in-window
origins. The three-status key of a cloud is the pair `κ_3(C) = (S, B)` of its single and its blocked states. A state
carrying `before` beside an in-window origin is blocked, so `κ_3` identifies keys that the six-status `κ` keeps apart.

**Theorem 3 (three-status quotient).** Let `C_u` and `C_v` be clouds reached by the search after words `u` and `v` with
`κ_3(C_u) = κ_3(C_v)`. For every byte `b` the successors `step(C_u, b, |u|)` and `step(C_v, b, |v|)` are both empty or
both non-empty, and when non-empty their three-status keys agree. A reachable cloud is certified exactly when `B = ∅`
and `|S| = 1`. Breadth-first search over three-status keys therefore decides whether the model certifies any window and
returns the minimum certified length. It retains at most `3^n` keys, in `O(256 · n · 3^n)` time and `O(n · 3^n)` space.

*Proof.* The occupied states are `S ∪ B`, so the key says whether the acceptance gate is open. When `q0` is not
re-entrant, no live edge enters it, so `q0` is occupied only in `C0`, where it carries `before` and is blocked; the key
says whether the rename fires, through `q0 ∈ B`. When it fires, the contribution of `q0` to the direct step is set aside
and counted as the fresh origin, as in Lemma 3. Now fix a successor state `q'` and collect what arrives there. If a
blocked predecessor `p` has `δ(p, b) = q'`, it brings `before` or two distinct in-window origins, all of which move to
`q'` together, so `q'` is blocked whatever else arrives. Otherwise every predecessor of `q'` is single and brings one
in-window origin. These origins are pairwise distinct by Invariant 1, since one origin occupies one state, and the fresh
origin, when the seed or the rename targets `q'`, is distinct from all of them by freshness. Hence `q'` is blocked,
single or empty as the number of arriving origins is at least two, one or zero. Every quantity used is a function of
`(S, B)`, of `b` and of fixed automaton data, hence the successor keys agree, and the successor is empty exactly when
every state is empty, which the key says too.

For the verdict, a non-empty cloud is certified when no pair carries `before` and all pairs share one in-window origin.
No `before` and no two origins at one state means `B = ∅`, and then every occupied state is single; by single occupancy
the number of distinct origins is the number of occupied states, so one origin means `|S| = 1`. The congruence inducts
over every continuation as in Theorem 2, with the same depth alignment, and the bounds follow from the three statuses
per state. ∎

The coarsening loses nothing, since the key forgets only distinctions that the transition and certification rules never
read. At a blocked state the six-status key additionally separates whether it carries `before` and how many in-window
origins it holds up to two, and the proof above reads neither. The figure program of Section 12 runs this search beside
the gate's and finds the same shortest length on every named row, and Table 4 gives the retained keys of both.

### 6.2 The chosen-origin search

A certified window has one surviving origin. Guessing that origin at its birth, and carrying its state beside the
support of every other hypothesis, turns the search into a nondeterministic automaton over windows. Its states have two
forms: `U(S)`, for `S ⊆ Q⁺`, means that no origin has been chosen yet and that `S` is the set of states the cloud
occupies, and `V(S, q)`, with `q ∈ Q⁺` and `S ⊆ Q⁺ \ {q}`, means that the chosen origin sits at `q` and that the pairs
carrying any other origin occupy exactly the states of `S`. The initial state is `U(Q⁺)`, and the accepting states are
the `V(∅, q)`. The state `U(∅)` accepts nothing and has no successor, and no transition enters it, so it is reached only
as the initial state of a token set with no live state.

Take a byte `b` at `U(S)`; the rename fires when `q0 ∈ S` and `q0` is not re-entrant. Let `T` be the live image of `S`
under `b`, with the contribution of `q0` set aside when the rename fires. A fresh origin is born at `r = δ(q0, b)` when
`r` is live and either `S` contains an accepting state or the rename fired. Without a birth the successor is `U(T)`,
when `T` is non-empty. With a birth there are two successors. Declining the birth moves to `U(T ∪ {r})`, and choosing it
moves to `V(T, r)`, which is allowed only when `r ∉ T`. Now take a byte `b` at `V(S, q)`. Let `q' = δ(q, b)`; there is
no successor when `q'` is not live. Let `T` be the live image of `S`, together with `δ(q0, b)` when that state is live
and `S ∪ {q}` contains an accepting state. There is no successor when `q' ∈ T`, and otherwise the successor is
`V(T, q')`. No rename arises in a `V` state, since a non-re-entrant `q0` is unoccupied after the first byte and a
re-entrant one disables the rename.

**Theorem 4 (chosen-origin automaton).** The automaton accepts exactly the windows the model certifies. It is
unambiguous: a certified window has exactly one accepting run, which chooses the window's origin at that origin's birth.
It has at most `K = 2^n + n · 2^(n-1) = (n + 2) · 2^(n-1)` states, and at most two successors per state and byte.

*Proof.* Two invariants hold along every run, by induction on the bytes read. In `U(S)` after a word `u`, the set `S` is
the support of `C_u`, since the gate reads only which occupied states accept, the direct step maps the support to its
live image, the rename is read from `q0 ∈ S` as in Theorem 3, and a fresh origin occupies `δ(q0, b)`. In `V(S, q)` after
`u`, the cloud `C_u` contains a pair `(q, o)`, where `o` is the origin chosen when the run left `U`, and `S` is the set
of states occupied by pairs with an origin other than `o`. The pair `(q, o)` moves to `(δ(q, b), o)` when that state is
live; otherwise `o` dies, and no later cloud contains it, since every origin born later carries a larger offset. The
other pairs move to the live image of `S`, and the fresh origin, when born, is another origin and lands at `δ(q0, b)`,
while the gate reads the support `S ∪ {q}` of `C_u`. The successor `V(T, q')` thus records exactly the state of `o` and
the support of the others, as long as `q' ∉ T`. When `q' ∈ T` the run stops, and nothing is lost. The state `q'` then
carries `o` and a second origin, and by determinism the two move together or die together at every later step, so no
later cloud has `o` as its only origin.

A run in `V(∅, q)` after `w` thus says `C_w = {(q, o)}` with `o` in the window, which is certification. Conversely let
`w` be certified with origin `o`. At the byte `w_o` the origin `o` is born at `δ(q0, w_o)`, by the seed or by the
rename, and that state carries no other origin, for otherwise `o` could never become the only one. Choose `o` there and
decline every other birth. The chosen pair survives to the end, since a pair carrying `o` is in `C_w`, and it never
shares a state with another origin, since both would then reach `C_w`; its final `S` is empty, because every pair of
`C_w` carries `o`. For unambiguity, an accepting run chooses the origin that is alone at the end, which is the unique
origin of `C_w` and has one birth, and the run is deterministic before the choice, its transitions deterministic after
it. For the count, there are `2^n` sets `S` for the `U` states, counting the empty support, which no transition enters,
and for the `V` states there are `n` choices of `q`, with `S` a subset of the remaining `n - 1` states. Only a birth
before the choice branches. ∎

**Corollary 1 (shortest window).** Breadth-first search over the automaton decides whether the model certifies any
window and returns a shortest one, in `O(256 · n · K)` time and `O(n · K)` space. The origin is the depth at which the
returned path leaves `U`. If a certified window exists, one of length at most `K - 1` exists. Existence of a model
certificate is in PSPACE in the size of the compiled automaton.

*Proof.* A path from `U(Q⁺)` to an accepting state spells a certified window, and conversely. Breadth-first order finds
a shortest one, and the origin is read off the one `U`-to-`V` edge of the path. A shortest accepting path repeats no
state, so its length is below `K`. A state is written in `O(n)` bits and a successor is computed in polynomial time, so
a path of length below `K` is guessed holding one state and a counter of `O(n)` bits, which places existence in
nondeterministic polynomial space, and so in PSPACE. ∎

**Remark 1.** Determinizing the chosen-origin automaton gives back the three-status quotient once equivalent dead states
are identified. After a word whose cloud is non-empty with three-status key `(S, B)`, the set of automaton states
reached is `U(S ∪ B)`, where the run that chose nothing survives, together with `V((S ∪ B) \ {q}, q)` for every `q ∈ S`.
A `V` run survives exactly while its origin is alone at its state, which is the single status, and its other support is
then the rest of the occupied states. A non-empty word that empties the cloud leaves no run alive, so an emptied cloud
corresponds to the empty subset. The subset construction stays within these special subsets, which is why no second
exponential appears.

Release 2.2.0 of munch ships the search of Corollary 1 as `shortest_split_window()`, beside the window decision
`is_split_window()` of release 1.4.0. The search holds one node per automaton state reached, packed as a chosen state
and a bit per live state, and reads the window and its origin back along the trail of parents. A node budget, `2^20` by
default, bounds the nodes held, and running out is returned as such, never as an answer. The library's test reproduces
the first six minima of the family of Section 6.4, and the figure program of Section 12 runs the same search over the
named token sets. Table 4 gives the nodes the search holds when its shortest window appears.

### 6.3 PSPACE-completeness

The upper bound of Corollary 1 is matched by a lower bound that holds with one token kind and five symbols. The source
problem is intersection non-emptiness: given complete DFAs `A_1, ..., A_m` over a common alphabet, is some word accepted
by all of them? Kozen proved it PSPACE-complete (Kozen, FOCS 1977). We take the source DFAs over the alphabet `{a, b}`,
which loses nothing, since a fixed-length binary code of the letters, decoded by a polynomial number of added states
with a rejecting sink for malformed codes, carries any alphabet to this one. We also keep in each `A_i` only the states
reachable from its start, which changes no language.

**Theorem 5 (PSPACE-completeness).** Deciding whether a compiled token set admits a model certificate of any length is
PSPACE-complete. So is deciding whether it admits a witnessed one. Both remain PSPACE-hard for token sets with one token
kind over five symbols, whose start state neither accepts nor is re-entered and whose every other state accepts.

Membership is Corollary 1 for the plain problem and Theorem 10 below for the witnessed one, and the rest of this section
is the reduction. Let `A_i = (Q_i, {a, b}, δ_i, s_i, F_i)` for `i = 1, ..., m`, with `m >= 1`. The token DFA has the
symbols `a`, `b`, `r`, `t`, `#`, and its states are the root `q0`, an error state `E`, a guard `G`, a result state `Y`,
timers `T_0, ..., T_m`, headers `H_0, ..., H_m`, and a disjoint copy `(i, q)` of every state `q` of every `A_i`. The
root is the only state that does not accept, and nothing enters it; every other state accepts the one token kind. The
root has the two transitions `q0 -r-> H_0` and `q0 -#-> Y`, and the other transitions are in Table 1, where an entry
marked undefined is absent; on the remaining bytes of the alphabet no state has a transition.

**Table 1.** The transitions of the reduction's token DFA at every state but the root, which has `q0 -r-> H_0` and
`q0 -#-> Y` only. The letter `x` ranges over the source alphabet `{a, b}`.

| State               | `r`       | `t`        | `#`                              | `x`              |
|---------------------|-----------|------------|----------------------------------|------------------|
| `E`                 | `T_m`     | `E`        | `E`                              | `E`              |
| `G`                 | `T_m`     | `E`        | undefined                        | `G`              |
| `Y`                 | `T_m`     | `E`        | `E`                              | `E`              |
| `T_j`, `j > 0`      | `T_(j-1)` | `E`        | `E`                              | `E`              |
| `T_0`               | `T_0`     | `G`        | `E`                              | `E`              |
| `H_(i-1)`, `i <= m` | `H_i`     | `(i, s_i)` | `E`                              | `E`              |
| `H_m`               | `H_m`     | `G`        | `E`                              | `E`              |
| `(i, q)`            | `T_m`     | `E`        | undefined if `q ∈ F_i`, else `E` | `(i, δ_i(q, x))` |

The construction has `Σ_i |Q_i| + 2m + 6` states and takes polynomial time. Every state is live. The root reaches `H_j`
on `r^(j+1)`, `Y` on `#`, `E` on `##`, `T_m` on `#r`, each lower timer on further `r`'s, `G` on `r^(m+1) t`, the start
`(i, s_i)` on `r^i t`, and every other `(i, q)` on the word that reaches `q` in `A_i` from there. Every state but the
root accepts, and the root has an accepting successor, so all are co-accessible. The root is not re-entrant, since no
transition targets it, and the rename is in force. Two facts about the table carry the proof. First, on `r`, `t`, `a`,
`b` every state but the root has a transition, so on these letters no pair at a non-root state ever dies. Second, every
occupied state after the first byte is a non-root state and accepts, so the gate is open at every step after the first,
and it is open at the first because `C0` holds accepting states.

**Lemma 4 (common word to window).** If every `A_i` accepts `w`, then `W = r^(m+1) t w #` is certified at origin
`|W| - 1`. Moreover `W` is itself completely tokenizable, with its final byte a token of its own.

*Proof.* Follow the cloud. During the `m + 1` bytes `r`, every pair at `E`, `G`, `Y`, a timer or a source copy moves
into the timers and down to `T_0`, where it stays, every pair at a header moves up to `H_m` and stays, and the root's
pair is renamed to origin 0 at `H_0`. The gate is open at every byte, so a fresh origin `j` is born at `H_0` at each
byte `j = 0, ..., m`; the rename and the seed coincide at `j = 0`. After the block, origin `j` sits at `H_(m-j)`, one
origin per header, with `(T_0, before)` and `(H_m, before)` beside them. The byte `t` sends `T_0` and `H_m` to `G`, and
`H_(i-1)` to `(i, s_i)` for `i = 1, ..., m`, and since the root has no `t` edge no origin is born. The letters of `w`
keep `G` fixed and drive each `(i, s_i)` to `(i, δ_i(s_i, w))`, with no birth. On the final `#` every pair at `G` dies,
and every source pair dies too, because its state is in `F_i`, while the open gate seeds `(Y, |W| - 1)`; the final cloud
is that singleton. For the scan, the run of `W` from the root passes `H_0, ..., H_m`, enters `G` on `t` and stays there
through `w`, accepting at every byte. Since `G` has no `#` edge, the first token is `W` without its last byte, and the
scan restarts at `#`, which the root consumes into `Y`. ∎

**Lemma 5 (window to common word).** Suppose the model certifies some window. Then it certifies a prefix of it of the
form `P r^L t w #` at its last byte, with `L >= m + 1` and `w ∈ {a, b}*` accepted by every `A_i`.

*Proof.* Take a certified window and cut it at the first byte after which the cloud is certified. Call the result
`W' = w0 ... w(k-1)`, so `C_k` is certified and `C_(k-1)` is not. A byte outside the five symbols empties the cloud,
which is absorbing and never certified, so every byte of `W'` is one of the five, and `C_(k-1)` is non-empty. The last
byte is `#`. Were it one of `r`, `t`, `a`, `b`, every pair of `C_(k-1)` at a non-root state would survive with its
origin intact, since the rename touches only the root. Those pairs carry `before` for `k = 1`, and for `k > 1` whatever
made `C_(k-1)` uncertified, `before` or two distinct origins, and a fresh origin only adds a pair, which cannot certify
an uncertified cloud; so `C_k` would not be certified. On `#`, every pair at a non-root state moves to `E` or dies, and
the open gate seeds `(Y, k - 1)`. The root's pair, present only in `C_0`, becomes that same seeded pair by the rename.
Certification at origin `k - 1` needs every pair of `C_(k-1)` at a non-root state to die, so those pairs occupy only `G`
and accepting source copies.

It remains to show that `W'` ends in `r^L t w #` with `L >= m + 1` and `w` a binary word, and that every `A_i` accepts
`w`. For the suffix, follow one pair, the sentinel, through the window. If some byte of `w0 ... w(k-2)` is `#`, let `j`
be the position of the last one. The cloud `C_j` is non-empty, since the empty cloud is absorbing and `C_(k-1)` is not
empty, and the gate is open, so `(Y, j) ∈ C_(j+1)`. Otherwise take `(E, before) ∈ C0` and set `j = -1`. The bytes
`w_(j+1), ..., w_(k-2)` are not `#`, so they are among `r`, `t`, `a`, `b`, on which no pair at a non-root state dies,
and the sentinel survives to `C_(k-1)` with its origin, following the table from `Y` or `E`. From `Y`, `E`, the timers
and `G`, these four letters lead only to `Y`, `E`, the timers and `G`, so the sentinel never reaches a header or a
source copy, and to die on the final `#` it must sit at `G` in `C_(k-1)`.

Call `z = w_(j+1) ... w_(k-2)` the word the sentinel reads. Among these states, `G` is entered only from `T_0` on `t`;
it is left on `r` and `t`, and kept on `a` and `b`. Hence the last `t` of `z` is read from `T_0`, and only the letters
`a` and `b` follow it; that is, `z = z' t w` with `w ∈ {a, b}*`, and the sentinel is at `T_0` after `z'`.

The timer `T_0` is entered only from `T_1` or from itself, on `r`, and every other letter from a timer leads to `E` or
`G`. Let `r^L` be the longest run of `r`'s ending `z'`. The state before that run is reached by a letter other than `r`,
or is the sentinel's initial state, so it is among `Y`, `E`, `G`. If `L = 0` the sentinel is there after `z'`, not at
`T_0`. Otherwise the run enters `T_m` on its first byte and needs `m` more to reach `T_0`, so `L >= m + 1` and
`W' = P r^L t w #`.

During the last `m + 1` bytes of the run `r^L` the gate is open and the root's `r` edge is live, so an origin is born at
`H_0` at each of them. The one born `j` bytes before the run's end sits at `H_j` when the run ends, for `j = 0, ..., m`,
so these origins are distinct, one per header. The `t` sends `H_(i-1)` to `(i, s_i)` for `i = 1, ..., m`, and `w` drives
each to `(i, δ_i(s_i, w))`. Every pair of `C_(k-1)` dies on the final `#`, and a source copy dies there exactly when its
state is in `F_i`, so `δ_i(s_i, w) ∈ F_i` for every `i`. ∎

*Proof of Theorem 5.* Lemma 4 maps a common word to a certified window, which is witnessed, since `W` is its own
completely tokenizable occurrence, and Lemma 5 maps any certified window, witnessed or not, to a common word. The
construction is polynomial, so both problems are PSPACE-hard, and they are in PSPACE by Corollary 1 and Theorem 10. ∎

The reduction fixes the minimum length exactly, at the source's minimum plus `m + 3`.

**Corollary 2 (minimum length).** Let `ℓ` be the length of a shortest word every `A_i` accepts. Then a shortest
model-certified window of the constructed token set has length `ℓ + m + 3`, and so does a shortest witnessed one.

*Proof.* Lemma 4 gives a witnessed window of length `ℓ + m + 3`. By Lemma 5, any certified window has a certified prefix
ending in `r^L t w #` with `L >= m + 1` and `|w| >= ℓ`, so its length is at least `ℓ + m + 3`; a witnessed window is
certified, so the same bound holds for it. ∎

The theorem concerns the model and says nothing about the complexity of deciding Definition 1 itself, which the model
only approximates from below. It also concerns the compiled automaton, and since a token set given as regular
expressions may compile to exponentially many states, no bound in the expression size is claimed. Unless P = PSPACE it
rules out a polynomial-time algorithm for the model's existence question, so replacing the quotient by another search
cannot give one. Theorem 6 below shows separately that returning a certified window can require superpolynomially many
bytes.

### 6.4 Shortest windows can be long

Corollary 1 bounds a shortest certified window by `K - 1`, which is exponential in `n`. The reduction instantiated on
cycles gives shortest windows of length `2^Ω(√n)`, so no polynomial bound holds, and the figure program searches their
exact minima.

For `m >= 1` let the source `A_i`, for `i = 1, ..., m`, be a cycle of `i` states numbered `0, ..., i - 1`, placed behind
the header `H_(i-1)`. It starts at 0, both letters advance by one modulo `i`, and it accepts at state `i - 1` alone, so
it accepts exactly the words whose length is -1 modulo `i`. Let `L_m = lcm(1, ..., m)`. A word every source accepts has
length -1 modulo `L_m`, and `a^(L_m - 1)` is a shortest one. The reduction gives a token set with
`n_m = Σ_(i=1..m) i + 2m + 6 = (m² + 5m + 12) / 2` states, and by Corollary 2 its shortest certified window, plain or
witnessed, has length `R_m = L_m + m + 2`. One such window is `r^(m+1) t a^(L_m - 1) #`, certified at its last byte and
completely tokenizable as two tokens.

**Theorem 6 (long windows).** For every `m >= 1` the token set above has `n_m` live states and a shortest certified
window of length `R_m >= 2^m / (m + 1) + m + 2`. Hence `R_m = 2^Ω(√n_m)`. No polynomial `p` bounds the length of a
shortest model-certified window by `p(n)` over all compiled token sets, nor of a shortest witnessed one.

*Proof.* It remains to show `L_m >= 2^m / (m + 1)`. Fix a prime `p` and write `v_p` for the exponent of `p` in an
integer. By the formula for the exponent of `p` in a factorial,
`v_p(binom(m, k)) = Σ_(j >= 1) (⌊m / p^j⌋ - ⌊k / p^j⌋ - ⌊(m - k) / p^j⌋)`. Each term is 0 or 1, since `m = k + (m - k)`
and the floor of a sum exceeds the sum of the floors by at most one. Terms with `p^j > m` vanish, so
`v_p(binom(m, k)) <= ⌊log_p m⌋ = v_p(L_m)`. This holds for every prime, so every `binom(m, k)` divides `L_m`. In
particular `L_m >= max_k binom(m, k) >= 2^m / (m + 1)`, since the `m + 1` binomial coefficients sum to `2^m`. Since
`n_m` is quadratic in `m`, `m = Θ(√n_m)` and the growth follows. For the last claim, `R_m / n_m^d` is unbounded for
every fixed degree `d`. ∎

The family does not reach every size `n`, but its sizes are dense enough. Since `n_(m+1) - n_m = m + 3 <= n_m`, for
every `n >= 9` the member with the largest `n_m <= n` has `n < 2 n_m`, so some token set with at most `n` live states
has a shortest window of length `2^Ω(√n)`. Table 2 gives the exact minima for `m = 1, ..., 9`. The figure program finds
them by the chosen-origin search, replays each window through the plain cloud at the claimed origin, scans it, and
asserts every entry of the table. The result is explicit, a shortest window that is long and not merely hard to find,
though it does not exclude a compressed description of long windows. It concerns the model, and the semantic question
meets the constraint only through the model.

**Table 2.** The long-window family: live states, `lcm(1, ..., m)`, the shortest certified window's length, equal to
`L_m + m + 2` on every row, and the nodes the chosen-origin search holds when it appears. Every window is certified at
its last byte and scans as two tokens.

| `m` | States `n_m` | `L_m` | Shortest window | Nodes held |
|----:|-------------:|------:|----------------:|-----------:|
|   1 |            9 |     1 |               4 |         18 |
|   2 |           13 |     2 |               6 |         41 |
|   3 |           18 |     6 |              11 |        110 |
|   4 |           24 |    12 |              18 |        253 |
|   5 |           31 |    60 |              67 |      1,226 |
|   6 |           39 |    60 |              68 |      1,781 |
|   7 |           48 |   420 |             429 |     11,038 |
|   8 |           58 |   840 |             850 |     27,017 |
|   9 |           69 | 2,520 |           2,531 |     87,518 |

## 7 Specializations

### 7.1 Length one

At length one the model collapses to the published certificate, and the correspondence is exact at the level of the
*shipped predicate*, the one that withholds vacuously certified bytes, rather than of the bare condition. Throughout
this section the token set is non-empty, read through its positive-width equivalent where a token matches the empty
string, and `q0` is live.

Call a byte `b` *useful* when `δ(q0, b)` is defined and live. The predicate `is_split_point(b)` of the predecessor holds
exactly when `b` is useful, no live state other than `q0` has a `b`-transition into a live state, and `q0` is not
re-entrant in `A⁺`. Two quantifier readings coincide here without loss. A transition target that is co-accessible makes
its source co-accessible, so "reachable state with a live-target `b`-transition" and "live state with a live-target
`b`-transition" name the same states.

**Theorem 7 (specialization).** For every byte `b`, the model certifies `(b, 0)`, the only origin a length-one window
admits, if and only if `is_split_point(b)` holds.

*Proof.* Compute `C_1` explicitly. `C0` is every live state paired with `before`, and it contains a live accepting
state, since `q0` is live and an accepting state reachable from it is trivially co-accessible, so the seed's acceptance
gate is open at offset 0. Reading `b` yields exactly:

- from the seed, the pair `(δ(q0, b), 0)`, present if and only if `b` is useful;
- from the direct step on `(q0, before)`, present if and only if `b` is useful, namely the pair `(δ(q0, b), 0)` when
  `q0` is not re-entrant, by the rename, and `(δ(q0, b), before)` when it is;
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

The vacuous case lands on the predicate's side of the published condition-versus-predicate distinction by construction,
since a byte no live state consumes empties the cloud, which the model refuses, exactly as the shipped predicate
withholds a byte the bare condition certifies vacuously. At length one the model is therefore exactly the shipped
predicate; the multi-byte construction is its conservative continuation. The artifact additionally asserts the agreement
byte for byte on every grammar of the evaluation before each search; after Theorem 7 that check verifies only the
implementation.

### 7.2 The prefix-code case

The other specialization is by token set. A *prefix code* `X ⊆ Σ⁺` has no word that is a proper prefix of another, and
read as a token set it is the setting where the certificate needs neither maximal-munch priority nor the cloud's origin
bookkeeping. At any boundary at most one codeword begins, so the completely tokenizable inputs are exactly `X*`, each
with the one factorization the scan computes. Code theory has two notions of a boundary forced by bounded context. A
*synchronizing pair* of `X` (Berstel, Perrin and Reutenauer, Cambridge University Press 2010) is a pair
`(x, y) ∈ X* × X*` such that `u x y v ∈ X*` implies `u x ∈ X*` and `y v ∈ X*` for all `u, v ∈ Σ*`. The second comes from
the synchronization pairs of Fici, Romana, Sciortino and Urbina (MFCS 2025, Section 3.3), stated there for circular
factors of a morphic image. Specializing them to a morphism `μ` sending the letters of an alphabet `Γ` to the codewords,
so that `X = μ(Γ)`, and to `L = Γ*`, we call a split `w = w1 w2` of a factor `w` of `X*` *synchronizing* when every
occurrence of `w` in a word of `X*` has a factorization boundary between `w1` and `w2`. Neither is a certified window as
it stands, since neither says which codeword covers the window's final byte, and the certificate says nothing about the
halves being in `X*`; the exact relation is the following.

**Proposition 1 (prefix codes).** Let `X` be a prefix code read as a token set, and let `W` be a window occurring in
some word of `X*`, with origin `o`, split as `W = W_<o W_≥o` at the origin.

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

The proposition places the classical case. Over a prefix code the certified windows that occur are the synchronizing
splits whose right half sits inside one codeword. A synchronizing pair of Berstel, Perrin and Reutenauer with a nonempty
right component is a certified window on its concatenation, with the origin at the start of the right component's last
codeword, and a certified window whose left half is in `X*` and whose right half is a codeword is a synchronizing pair.
The occurrence hypothesis sets the vacuous case of Definition 1 aside, since a window no word of `X*` contains is
certified whatever its right half spells. The named token sets of Section 12 fall outside the code case. An identifier
is a proper prefix of a longer identifier, so no C-like token set is a prefix code, and the JSON number `1` is a proper
prefix of `11`, so JSON is not one either; over `{a, ab, b}` the word `ab` has two factorizations and maximal munch
picks one. The random sample of Section 12 is drawn without that restriction and contains prefix codes, which the
proposition covers. Beyond the code case the certificate is defined through the scan, the origin bookkeeping of Section
3 decides it, and the evaluation measures that reach.

## 8 Strictness of the model

The model refuses windows a greedy scanner would allow. The conservatism is deliberate. The seed rule uses acceptance as
the only license a token needs to begin, which over-approximates greedy behavior. This section exhibits one source of
conservatism and shows that nonvacuous strictness begins at length two. Both examples below are asserted artifact rows,
whose assertion checks that the model refuses the window and that an exhaustive oracle over every completely tokenizable
input up to a length bound finds, at every occurrence, the token covering the window's final byte beginning at the
claimed origin, with the occurrence count pinned exactly. The covering-token check is the property of Definition 1;
checking merely that some token begins at the origin passes false covering-origin claims, such as `{a, abx, b, x}` at
`ab`, where the input `ab` tokenizes as `a|b` while the fixed boundary at the occurrence remains a safe cut.

*Example one.* Over `{a, ab, b}` the window `ab` is semantically certified at origin 0, and universally so. The byte `a`
occurs only as a token's first byte, so a token begins at every occurrence offset `t`, and maximal munch there prefers
`ab` over `a`, so the covering token of the final byte begins at `t`. The oracle confirms the argument over all inputs
to length 14, with 98,305 occurrences and zero violations. The model refuses the window. After `a`, the cloud is the
single pair carrying origin 0, but `a` is a token, so reading `b` seeds a competing trajectory at origin 1, the
segmentation `a|b` that greedy scanning never chooses, and unanimity is lost. Section 9 shows that such segmentations
are the whole of the surplus.

*Example two.* Over `{ab, abc, c}` the window `abc` is semantically certified at origin 0, again universally, since `a`
occurs only token-initially, so a token begins at `t`, and maximal munch prefers `abc` over `ab` there. The oracle
confirms it over all inputs to length 12 with 932 occurrences and zero violations. Both examples instantiate the same
`(u, uv, v)` shape; they share the mechanism and differ in prefix depth, the competing origin arriving one byte in for
the first and two bytes in for the second, where the accepting proper prefix `ab` seeds `c`, the segmentation `ab|c`
that maximal munch forgoes.

Each has length at least two, and that is not an accident of the examples.

**Corollary 3 (non-vacuous strictness begins at length two).** Let `b` be a byte occurring in some completely
tokenizable input. If the model refuses `(b, 0)`, then `(b, 0)` is not a certified split window. The occurrence
hypothesis is necessary, since a byte no completely tokenizable input contains satisfies Definition 1 vacuously while
the model refuses its emptied cloud, and such vacuous disagreements are not strictness.

*Proof.* Since `b` occurs, the final segmentation's covering token consumes `b` at that occurrence, a transition from a
live state into a live state, so if only `q0` has a live-target `b`-transition and `q0` is not re-entrant, the predicate
certifies `b` and, by Theorem 7, the model certifies `(b, 0)`, contrary to assumption. The exact condition of the
predecessor thus fails for `b`, and its necessity theorem constructs a completely tokenizable input placing an
occurrence of `b` strictly inside a token; at that occurrence the token containing `b` begins before `b`, so `(b, 0)` is
not certified. ∎

Non-vacuous conservatism is therefore a strictly multi-byte phenomenon. At length one the model is exact for occurring
bytes, and the shortest strict refusals have length two, a bound example one attains. The `{a, abx, b, x}` family of
Section 2 plays the opposite role in the artifact, a negative control for the covering-origin property. Cutting at the
family's fixed boundary is safe, since `a` occurs only token-initially, yet the covering origin genuinely varies, and
the artifact pins those violations exactly; an oracle that misses them would accept false origins. Negatives in every
table of this paper are claims about the model, never about the language.

## 9 What the cloud represents

The cloud was built as a conservative over-approximation, and Section 8 exhibited the surplus; this section says exactly
what the surplus is. Let `X` be the token language, the set of non-empty words `A` accepts from `q0`, and let `𝒢` be the
set of completely tokenizable inputs, so `𝒢 ⊆ X*`. A *factorization* of a word `x ∈ X*` is a sequence of words of `X`
whose concatenation is `x`, with no greedy choice imposed. The cloud is exactly the set of histories of all
factorizations of all words of `X*` around the window. This characterization explains why the vacuous window of Section
2 is certified, locates a class where the model is exact, finds a shortest witnessed window exactly, and decides whether
the model misses any witnessed certificate at all.

### 9.1 Factorization semantics

Fix a window `W = w0 ... w(k-1)`, and take a word `x = u W v ∈ X*` and a factorization of `x`. Let `s` be the start of
the factor covering the byte at `|u| + k - 1`, let `q = δ*(q0, x[s .. |u| + k))` be that factor's state after the
window's last byte, and let `ω` be `before` when `s < |u|`, and `s - |u|` otherwise. The set `F(W)` collects all pairs
`(q, ω)` so obtained, over every `u`, `v` and factorization.

**Theorem 8 (factorization semantics).** For every non-empty window, `C_k = F(W)`. Every pair of `C_k` is realized by
some `x = u W v` with `|u| <= n` and `|v| <= n - 1`. In particular `C_k` is non-empty exactly when `W` is a factor of a
word of `X*`.

*Proof.* First `F(W) ⊆ C_k`. Fix `x = u W v` and a factorization, and for `j ∈ 1..k` let `s_j` be the start of the
factor covering the byte at `|u| + j - 1`, with `ρ_j` and `ω_j` defined from it as in Section 4. The argument of Lemma 2
applies with "factor" in place of "token of the final segmentation'', since it used only that tokens are accepted words
placed end to end. Each `ρ_j` is live, reached by a prefix of an accepted factor and completed by its rest. If
`s_1 < |u|`, the state `p = δ*(q0, x[s_1 .. |u|))` is live and `(p, before) ∈ C0`, and the direct step gives
`(ρ_1, before)`; the rename cannot fire, since `p` has consumed a byte and `p = q0` only when `q0` is re-entrant. If
`s_1 = |u|`, the seed fires, because `C0` holds an accepting state, `X` being non-empty, and it gives `(ρ_1, 0)`. At a
continuing byte the direct step carries the pair with its origin, and again the rename cannot fire. At a byte beginning
a factor the previous factor `x[s_j .. |u| + j)` is in `X`, so `ρ_j` accepts and the seed gives `(ρ_(j+1), j)`.

Now `C_k ⊆ F(W)`, by induction on `j`. The claim is that every pair of `C_j` is the pair of some `x = u W[0 .. j) v`
with a factorization, where `|u| <= n` and `v` is a shortest accepting completion of the covering factor's state, so
that `|v| <= n - 1`. For `j = 1` the pairs of `C_1` arise three ways. A direct step from `(p, before)` with `p ≠ q0`
gives `(δ(p, w0), before)`. Let `u` be a shortest word with `δ*(q0, u) = p`, non-empty and of length at most `n - 1`,
and let `v` be a shortest accepting completion from `δ(p, w0)`, which exists since that state is live; then `u w0 v ∈ X`
is one factor, starting before the window. A direct step from `(q0, before)` that keeps `before` happens only when `q0`
is re-entrant, and then a shortest non-empty `u` with `δ*(q0, u) = q0` has length at most `n`, and the same completion
gives one factor `u w0 v`. The fresh origin, from the seed or from the rename of `(q0, before)`, gives `(δ(q0, w0), 0)`,
realized with `u` empty by the factor `w0 v` beginning at the window.

For the step, a pair of `C_(j+1)` is either a direct successor `(δ(q, b), ω)` of some `(q, ω) ∈ C_j`, or the fresh
origin. In the first case take the realization `x = u W[0 .. j) v` of `(q, ω)`, whose covering factor reaches `q` at the
window's end, and replace its remainder `v` by `b v'`, with `v'` a shortest completion from `δ(q, b)`, which keeps every
earlier factor and the origin. In the second case the seed fired, so some `(q', ω') ∈ C_j` has `q'` accepting. Take its
realization, end its covering factor at the window's end, which is legitimate since `q'` accepts, and begin a new factor
`b v'`, with `v'` a shortest completion from `δ(q0, b)`, whose origin is `j`. The rename does not arise at `j >= 1`,
since when `q0` is not re-entrant nothing enters it, and when it is, the rename is off; the left context `u` chosen at
`j = 1` is never changed. If `X` is empty, no state is live and `C_k` is empty, and no word of `X*` contains a non-empty
window, so both sides are empty. ∎

**Corollary 4 (what certification decides).** Let `W` be a factor of a word of `X*`. The model certifies `(W, o)`
exactly when, in every factorization of every word of `X*` containing `W`, the factor covering the occurrence's last
byte begins `o` bytes into it.

The corollary explains the vacuous window of Section 2. Over `{0, 00, 01}` the word `01001` factors as `01|0|01`, a
factorization containing `1001` at offset 1, with the last factor beginning 2 bytes into the window. Every factorization
of a word containing `1001` agrees, since a `1` is only ever the second byte of `01`, so the window's two `1`'s end
factors, and the factor covering the last one is the `01` at offset 2. The model therefore certifies `(1001, 2)`, and
rightly so for `X*`. The scan of `01001`, however, takes `01` and `00` and fails on the last `1`, so the word is in `X*`
and not in `𝒢`. Non-emptiness of the cloud is an occurrence theorem for `X*`, not for `𝒢`, and that is the whole gap
between the model's certificates and the witnessed ones.

**Corollary 5 (token kinds are immaterial).** Two automata with the same token language `X` give, on every window,
clouds with the same set of origins. Hence they give the same certificates, the same origins and the same shortest
certified length. Token kinds may be erased, the positive-width equivalent taken, and the Boolean recognizer of `X`
minimized before any search of this paper. Re-entrancy is recomputed on the result.

*Proof.* The set of origins of `F(W)` depends only on `X` and `W`, and certification, the origin and the shortest length
are read from that set. ∎

The reduction can be large. If every binary word of length `k` is given its own token kind, the kind-preserving minimal
automaton keeps every prefix apart and has `2^(k+1) - 1` live states, while the Boolean recognizer of `{a, b}^k` has
`k + 1`, and both give the same certificates. The evaluation runs on the lexer's own automata, as shipped, so the
corollary offers a reduction these searches do not use.

### 9.2 Where the model is exact

By Corollary 4 the model quantifies over factorizations, where Definition 1 quantifies over scans, and the two coincide
whenever every factorization of a word of `X*` is the successful scan of that word.

**Corollary 6 (exactness).** Suppose every word of `X*` has exactly one factorization and `𝒢 = X*`. Then for every
window the origins of the cloud are exactly the covering origins over the completely tokenizable occurrences of `W`,
with `before` standing for every origin outside the window. The model certifies every witnessed certificate, and refuses
every occurring window that is not one. Every prefix code satisfies the hypotheses, and so do `{a, ab}` and `ab*`, which
are not prefix codes.

*Proof.* Every word of `X*` is completely tokenizable, and its scan is a factorization, hence its only one. The
factorizations of words of `X*` containing `W` are exactly the scans of the completely tokenizable inputs containing
`W`, and the origins of Theorem 8 are the covering origins. An occurring window with the same in-window covering origin
at every occurrence is certified by the model, and any other occurring window is refused, since its covering origins,
`before` or two distinct ones, appear in `F(W)`. A prefix code admits at most one codeword at any position, so a word of
`X*` has one factorization, and the scan computes it by taking the one codeword that begins. Over `{a, ab}` every word
of `X*` is a sequence of `a`'s, each followed by at most one `b`; its factors begin at the `a`'s, so the factorization
is unique, and the scan computes it, taking `ab` where a `b` follows and `a` otherwise. Over `ab*` the same holds with
runs of `b`. ∎

Unique factorization alone is not enough; the scan must also succeed on all of `X*`. Over `{a, ab, bb}` every word of
`X*` has one factorization, since the reversed set `{a, ba, bb}` is a prefix code, so a word is factored uniquely from
its right end. The window `ab` is semantically certified at origin 0 and witnessed by the input `ab`, because an `a` is
only ever a token's first byte and the scan prefers `ab` to `a` where a `b` follows. Yet `abb` factors as `a|bb`,
covering the window's last byte with a factor beginning at offset 1, so the model refuses the window. The scan of `abb`
takes `ab` and fails on the final `b`, so `abb ∈ X* \ 𝒢`, and that is the hypothesis that fails. The paper's example
one, `{a, ab, b}`, fails the other hypothesis, since `ab` has two factorizations.

The hypotheses of Corollary 6 are decidable in polynomial time. Write `Pref₊(X*)` for the non-empty prefixes of words of
`X*`.

**Theorem 9 (the exact class).** The following are equivalent for a token language `X`.

1. Every factorization of every word of `X*` is the scan of that word, which succeeds.
2. Every word of `X*` has exactly one factorization and `𝒢 = X*`.
3. `X ∩ X Pref₊(X*) = ∅`: no word of `X` is a word of `X` followed by a non-empty prefix of a word of `X*`.

Condition (3) is decided on the live automaton in `O(256 · n²)` time. When it fails, the decision returns `u ∈ X`, a
non-empty `r` with `u r ∈ X`, and `z` with `r z ∈ X*`, of total length at most `n² + 2n - 2`. The word `u r z` is then
in `X*`, and its factorization beginning with `u` is not its scan.

*Proof.* First, (1) implies (2). A word of `X*` has a factorization, so its scan succeeds, and every factorization is
that scan, so there is one. Next, (2) implies (1). The scan of a word of `X*` succeeds and is a factorization, hence the
only one. Suppose (3) fails, so that `u ∈ X`, `r` is non-empty, `u r ∈ X` and `r z ∈ X*`. Then `u r z ∈ X*` has a
factorization beginning with `u`, while its scan, if it succeeds, begins with a token of length at least `|u r| > |u|`;
either the scan fails or it is a different factorization, so (1) fails. Suppose (3) holds and take any factorization
`x = x1 ... xm`. If `m = 0` the scan of the empty word succeeds. Otherwise `x1 ∈ X` is a candidate first token, and a
longer accepted prefix `x1 r` of `x` would have `r` a non-empty prefix of `x2 ... xm ∈ X*`, against (3). The scan thus
commits `x1` and restarts, and by induction on `m` it commits `x2, ..., xm` and succeeds.

For the decision, `Pref₊(X*)` is recognized by a nondeterministic automaton `P` over the live states, which starts at
`q0`, follows `δ`, optionally resets to `q0` before any byte read from an accepting state, and accepts at every state
reached after at least one byte. A path of `P` spells a sequence of accepted factors followed by a prefix of a live run,
which completes to a factor, so the word is in `Pref₊(X*)`; conversely a non-empty prefix of `x1 ... xm` is spelled by
resetting at each factor's end. Now run one breadth-first search over pairs `(p, p')`, started at every pair `(q, q0)`
with `q` a reachable accepting state, where `p` continues `δ` from `q` and `p'` runs `P` from `q0`, both over the same
bytes, and a pair reached after at least one byte is marked as started. A started pair with `p` accepting is an
obstruction. Let `u` be a shortest word reaching the `q` its search path began at, of length at most `n - 1` since `q0`
does not accept, let `r` be the bytes read, and let `z` be a shortest accepting completion from `p'`, of length at most
`n - 1`; then `u ∈ X`, `u r ∈ X` and `r z ∈ X*`. Conversely an obstruction `u`, `r` drives the search from `δ*(q0, u)`
along `r` to such a pair. There are at most `n²` started pairs, and a shortest path to one repeats none, so `|r| <= n²`,
and the search visits each pair's successors, at most two per byte, once. ∎

The figure program runs the decision on the examples above. It places `{a, ab}` and `ab*` in the class, and
`{a, ab, bb}` outside it with the obstruction `abb`, `{a, ab, b}` with `ab`, and `{0, 00, 01}` with `00`, whose
factorization `0|0` the scan does not take. The conventional C-like row is out with `//` followed by a tab, a comment
and a prefix of a whitespace run, which the scan reads as one comment. The class is a sufficient condition for the model
to be exact, not a necessary one, since exactness compares unanimous origins, a weaker observable than equality of every
history.

### 9.3 A shortest witnessed window

The evaluation separates model certificates from witnessed ones by a bounded search for an occurrence, and a certificate
that search cannot witness is counted as unresolved. The separation can be decided exactly. A shortest witnessed window
is found, with its occurrence, by running the chosen-origin automaton of Section 6.2 in step with a recognizer of the
factors of `𝒢`. That recognizer uses a recognizer of `𝒢` as a black box, the byte projection of the splitting
companion's armed-run automaton ([arXiv:2610.08854](https://arxiv.org/abs/2610.08854)) over one compiled token
automaton. An input is completely tokenizable exactly when it segments into accepted words such that no committed word
extends, within the input, to a longer accepted word from the same start. The automaton guesses the boundaries, runs
each committed word's state onward as an armed run, and rejects if an armed run accepts again. The automaton's states
are a current token state or a fresh boundary, together with the set of armed states, at most `(n + 1) · 2^n` of them,
and it recognizes exactly `𝒢`.

Let `Z` be that automaton, trimmed to the states reachable from its start and co-accessible to its accepting states, and
let `Fact(Z)` be `Z` with every retained state made initial and accepting.

**Lemma 6 (factors).** `Fact(Z)` accepts exactly the factors of completely tokenizable inputs.

*Proof.* Take a path of `Z` between retained states. The trim gives a stem from the start to its first state and a
suffix from its last state to an accepting state, and the three together are an accepting run of `Z`, so the path's
label is a factor of a word of `𝒢`. Conversely a factor of a word of `𝒢` is the label of the middle of an accepting run
of `Z`, whose states are all retained. ∎

**Theorem 10 (witnessed synthesis).** The product of the chosen-origin automaton with `Fact(Z)` accepts exactly the
witnessed model certificates. Breadth-first search over it decides whether one exists, returning a shortest one with its
origin and a completely tokenizable input containing it, and its exhaustion proves that no witnessed model certificate
exists at any length. The product has at most `K · (n + 1) · 2^n = O(n² · 4^n)` states. Existence of a witnessed model
certificate is in PSPACE.

*Proof.* The product accepts the intersection of the two languages, the model-certified windows by Theorem 4 and the
factors of `𝒢` by Lemma 6. That intersection is the set of witnessed model certificates, by Definition 2. Every edge
reads one byte, so breadth-first search from the initial pairs finds a shortest accepted window, and the chosen-origin
component gives its origin. For the occurrence, take the `Z` states at the ends of the accepting path and attach the
stem `u` and the suffix `v` the trim guarantees; then `u W v` is accepted by `Z`, so it is completely tokenizable, and
it contains `W` at offset `|u|`. By Theorem 1 the token covering that occurrence's last byte begins at the certified
origin. For the space bound, a product state is written in `O(n)` bits, a path of length below the product's size is
guessed with a counter of `O(n)` bits, and the initial and final `Z` states are checked reachable and co-accessible by
implicit searches in `Z` within the same space, so the problem is in nondeterministic polynomial space. ∎

Exhaustion here means no witnessed *model* certificate, and it still says nothing about a semantic certificate the model
refuses. The returned input is a witness of occurrence, not a shortest such input, since the search minimizes the
window. The four model-positive grammars of the random sweep that the bounded witness search leaves unresolved are a
question this search settles either way, though this paper does not rerun the sweep under it. With Lemma 4, whose window
is witnessed, the witnessed half of Theorem 5 follows.

### 9.4 Whether the model misses a certificate

Section 8 shows the model refusing a witnessed certificate over two small token sets, and whether a given token set has
any such refusal is decidable from the regularity noted in Section 13. This section decides it explicitly, with a
shortest missed window and a bound. Call the model *globally complete* for a token set when it certifies every witnessed
certificate of Definition 2, which holds vacuously when there is none.

Two marked graphs carry the decision. In both, an edge is labeled by a byte and marked when a token begins just before
that byte, so the last marked edge of a path locates the start of the token covering the last byte it spells. The first
is the trimmed `Z`, whose paths are the greedy histories. The second, `Φ`, has the live states and a fresh boundary `⊥`.
It has an unmarked edge `p -b-> δ(p, b)` for every live transition and, for every byte `b` with `δ(q0, b)` live, a
marked edge `p -b-> δ(q0, b)` from every accepting `p` and from `⊥`. Its initial state is `⊥`, its final states are the
accepting live states, and it is trimmed likewise. Its accepting paths are exactly the factorizations of non-empty words
of `X*`, and by the argument of Lemma 6 its paths between retained states are exactly the factors of words of `X*`, with
the factor starts they contain marked. Write `N` and `f` for the two graphs' state counts, with `N <= (n + 1) · 2^n` and
`f <= n + 1`. For a graph `J` and a candidate `(W, o)`, a path spelling `W` *has origin `o`* when its edge at byte `o`
is marked and no later edge is, so a path with no marked edge has none. Let `Occ_J` be the set of candidates some path
of `J` spells, and `Bad_J` the set of candidates `(W, o)` that some path of `J` spells without having origin `o`.

**Theorem 11 (global completeness).** The model misses `(W, o)`, a witnessed certificate it refuses, exactly when
`(W, o) ∈ Occ_Z ∩ Bad_Φ \ Bad_Z`. Whether the model is globally complete is decidable. When it is not, a shortest missed
window is returned with its origin, a completely tokenizable input containing it, and a word of `X*` with a
factorization that covers the occurrence's last byte from another origin. The decision runs over an observer of at most
`2^(N + f) + 4^(N + f)` states. So it is in EXPSPACE in `n`, and takes doubly exponential time.

*Proof.* A witnessed certificate `(W, o)` is a candidate in `Occ_Z`, since `W` occurs in some completely tokenizable
input, and it is outside `Bad_Z`, since every greedy occurrence has origin `o`. The model refuses it exactly when `C_k`
carries an origin other than `o`, because the pair of the greedy occurrence is in `C_k` by Lemma 2 and carries `o`, so
the cloud is non-empty and certification fails only by a second origin. By Theorem 8 a second origin is a factorization
of a word of `X*` covering the occurrence's last byte from elsewhere, which is membership in `Bad_Φ`, so the three
conditions are exactly the missed certificates.

The observer reads `W` with its byte `o` marked, and for each graph `J` it holds the set of ends of the paths spelling
the prefix read so far. Before the mark it holds one set per graph, initially every retained state; after the mark that
set is split into the *right* set `R_J` of ends whose path has origin `o` so far and the *wrong* set `B_J` of the rest.
At the marked byte, an edge from the set goes to `R_J` when marked and to `B_J` otherwise. After it, `R_J'` is the set
of ends of unmarked edges from `R_J`, and `B_J'` the set of ends of all edges from `B_J`, together with the ends of
marked edges from `R_J`. The two sets may overlap, since different paths reaching one state may differ in origin, and
they are held apart. At the window's end, `(W, o)` is missed exactly when `R_Z ≠ ∅`, `B_Z = ∅` and `B_Φ ≠ ∅`, which
restates the three conditions. Given the position of the mark the observer is deterministic, and the mark is one extra
choice per window, so breadth-first search over observer states, marking at each depth or not, reaches a missed window
of minimum length or exhausts the finitely many states. From a missed window, a path through `Z` into `R_Z` and a path
through `Φ` into `B_Φ` are read back, and stems and suffixes complete them into the two inputs claimed. A state of the
observer is one or two subsets of each graph, hence the bound, and it is written in `O(N + f)` bits, which is
`O(n · 2^n)`; a nondeterministic search with a counter runs in that space, and the explicit search runs in time
polynomial in the observer's size. ∎

Over `{a, ab, b}` the shortest missed window is `ab` at origin 0, with the completely tokenizable input `ab` and the
factorization `a|b` of the same word. Over `{a, ab, bb}` it is the same window and input, with the factorization `a|bb`
of `abb`. The model is globally complete over `{a}` and over `{a, ab}`, the latter by Corollary 6. The bound is an upper
bound only, and whether a polynomial criterion for global completeness exists is open; Theorem 9 decides a sufficient
condition in polynomial time. Completeness is a property of the token set, settled once from its tables, and where it
holds every negative of the model is a negative of Definition 2, so the three-layer reading of the results collapses to
two.

## 10 From a boundary to a parallel cut

At every occurrence at offset `t` in a completely tokenizable input, a certified window `(W, o)` yields a true boundary
`t + o` of the final segmentation. Turning a boundary into a parallel cut is the predecessor's territory, since its
prefix-stability result licenses a worker to scan from a known boundary and agree with the serial stream around the cut
([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)). The division of labor is exact, in that this paper establishes
that `t + o` is a boundary and the predecessor establishes what a scan starting at a boundary preserves. The v1.3.3
artifact evaluated here plans with single-byte certificates only. Release 1.4.0 added the window decision as a public
contract, which the sweep at the later commit cross-checks against its model, and a window-planning sibling, which lies
outside this paper's evaluation and is not a claim of this paper.

### The mandatory core

For token sets whose certified windows all share structure, the simulator proves it at construction: a byte string that
occurs, with at least one byte after it, inside every certified split window. Candidates come from the shortest words
that force a scan to die, and each is proved or refuted against every death path the live tables allow from its
proposing state, so the accessor returns only what holds for all of them. Block comments prove their closer; token sets
whose windows share no such string give nothing and lose nothing.

The window planner runs on this licence when it exists: candidate windows are generated only around occurrences of the
core, visited in the exhaustive walk's own order and certified by the same memoized decision, so the plan is byte for
byte the walk's, refusals included. A core too long to fit the longest window with a byte to spare concludes the walk's
refusal without scanning, and a tail with no occurrence refuses later targets without another scan. The core is an
accelerator's licence, never a certificate: every cut is still established by the certified window decision, and
grammars without a proved core keep the exhaustive walk unchanged. Construction cost is discussed in
[limits.md](limits.md).

## 11 Boundary profiles

Section 2 kept three guarantees apart, and this paper certifies the strongest. This section asks, of every gap of a
window at once, whether the weakest guarantee, a fixed safe boundary, holds there. Release 2.1.0 of munch decides that
property exactly within its search cap, and the answer places the certificate among the anchors a window can soundly
supply, the positions it makes a boundary at every occurrence.

**Definition 3 (boundary profile).** Let `W = w0 ... w(k-1)` with `k >= 1`. A *gap* of `W` is an index
`g ∈ {0, ..., k}`: the gap before byte `w_g` when `g < k`, and the gap after the final byte when `g = k`. Take an
occurrence of `W` at offset `t` in a completely tokenizable input `x`. The gap `g` *is a boundary* there when a token of
the maximal-munch tokenization of `x` begins at `t + g` or `t + g = |x|`, and a token *crosses* it otherwise. Over every
occurrence of `W` in every completely tokenizable input, the gap is *forced* when it is a boundary at each of them,
*crossed* when it is a boundary at none, and *open* when both happen. The *boundary profile* of `W` assigns every gap
its verdict.

A gap here is a position between adjacent bytes or at either end of the window, not a distance between anchors. A window
no completely tokenizable input contains is forced and crossed at every gap, both vacuously, exactly as Definition 1
holds vacuously there, while for an occurring window each gap has one verdict. The library names the three verdicts
`must`, `never` and `may`, and a window occurring nowhere `absent`.

**Remark 2.** Three earlier notions are profile patterns by definition. The pair `(W, o)` is a certified split window
exactly when gap `o` is forced and gaps `o + 1, ..., k - 1` are crossed. At an occurrence, the token covering byte
`t + k - 1` begins at `t + o` exactly when a token begins at `t + o` and none begins at `t + o + 1, ..., t + k - 1`, and
both conditions are quantified over the same occurrences, so the conjunction splits gap by gap. A certified split point
of the predecessor ([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)) is the case `k = 1` with gap 0 forced, and the
fixed safe boundary of Section 2 is a forced gap.

The profile names anchors the certificate cannot. Over `{0, 1, x, 001x, 011x}` the window `0011` has gaps 0 and 1
forced, since `0` lies inside a token only as the second byte of `001x`, and neither of the window's first two bytes is
followed by `1x`. Gaps 2 to 4 are open, crossed in the input `0011x`, which tokenizes as `0` and `011x`, and boundaries
in `0011`, which tokenizes byte by byte. The covering token's origin is therefore 1 in one input and 3 in the other,
while the profile gives the window two anchors. No substring of the window is a certified split window at any origin,
since the last byte of each begins a token in `0011` and lies inside an earlier token in `0011x`, or in `001x` when it
is a `0`.

**Local maximality.** The weakest guarantee is the one a cut consumes. Fix a width `H >= 1`. A *cut rule of width `H`*
is a set `R` of pairs `(W, g)` with `1 <= |W| <= H` and `g` a gap of `W`, and on an input `x` the rule cuts at every
position `t + g` such that `(W, g) ∈ R` and `W` occurs at `t`. Each piece between consecutive cuts is scanned from `q0`
on its own, and the token streams are concatenated with nothing reconciled. The rule is *sound* when on every completely
tokenizable input the concatenation is the input's maximal-munch tokenization. It reads the window and nothing else. No
state crosses a cut, and no piece is rescanned.

**Proposition 2 (local maximality).** A cut rule is sound if and only if every gap it cuts at is forced.

*Proof.* Suppose every `(W, g) ∈ R` has gap `g` forced, and fix a completely tokenizable `x`. Every cut falls at a
forced gap of an occurrence, so at a boundary of the tokenization of `x` or at `|x|`, and a cut at a boundary is exact,
the pieces between boundaries, scanned from `q0` and concatenated, reproducing the tokenization. That is the splitting
companion's lemma that cuts at boundaries are exact ([arXiv:2610.08854](https://arxiv.org/abs/2610.08854)), stated there
for literal token sets and carried to regular ones by its corollary on splitting at the regular generality, and Section
10 composes with the same step. Conversely, suppose some `(W, g) ∈ R` has gap `g` not forced. Some completely
tokenizable `x` then contains `W` at an offset `t` where a token of the tokenization crosses `t + g`, beginning before
it and ending after it, and the rule cuts `x` at `t + g`. The piece ending there emits no token reaching past `t + g`,
and the piece beginning there emits none beginning before it, so the crossing token is emitted by neither and the
concatenation differs from the tokenization. ∎

The certificate is the case of one forced gap followed by crossed gaps to the window's end, and it supplies the anchor
`t + o`. The proposition says what else a window supplies. A sound rule of width `H` cuts at no position that the forced
gaps of windows of width at most `H` fail to name, and it may cut at every position they name. The profile at width `H`
is the whole anchor supply of that width, of which the certificate names one member per window. The gap `k` after the
window is the member no origin can name, since a forced gap `k` of `W` is the certificate `(Wc, k)` for every byte `c`
that can follow, one byte later.

**Decidability.** Gap `g` is not forced exactly when some completely tokenizable input has an occurrence of `W` that a
token crosses at `g`, and not crossed exactly when some has an occurrence cut there. Each is a question of the form the
splitting companion's offline verifiability theorem decides ([arXiv:2610.08854](https://arxiv.org/abs/2610.08854)),
since the boundary markings of completely tokenizable inputs form a regular language, recognized by its armed-run
automaton, and its intersection with the markings holding an occurrence of `W` with, or without, a mark at gap `g`, the
end-of-input closure counting as one, has decidable emptiness with a shortest counterexample. The profile is therefore
decidable for every regular token set, and nothing is proved fresh here. Release 2.1.0 of munch decides both questions
per gap, as `boundary_counterexample()` and `crossing_counterexample()`, which both run the one search of its exact
window decision `window_counterexample()`, building the input byte by byte, breadth first, with its token boundaries
guessed and every guess held to maximal munch. Each returns the shortest refuting input or, once it has exhausted its
states under a cap on the states held at once, the proof that none exists; a search the cap stops settles nothing, and
the gap is undetermined. The function `boundary_profile()` asks both questions at every gap and reads the verdicts off,
giving `must` when only the first search exhausts without a counterexample, `never` when only the second does, and `may`
when both find one. Whether the window occurs at all is decided once per window by `window_occurrence()` under the same
cap, and `absent` is given at every gap or at none.

**Anchor propagation.** Section 2 noted that the weakest guarantee is already usable, a worker cutting at the known
boundary and rescanning the window's suffix. Read as a window, that rescan forces further gaps. The statement needs a
longest token. Write `L` for the maximum token length of a literal token set, a finite set of byte strings, or of a
regular token set whose token languages are all finite; a regular token set with an infinite token language has no `L`,
and the lemma is not stated for it. Two facts precede it. First, extension is monotone. A forced or crossed gap of `W`
stays forced or crossed, shifted, in every window that contains `W`, since every occurrence of the larger window
contains an occurrence of `W` at the shifted offset. Second, the scan restarts in `q0` at every boundary, which is the
splitting companion's memorylessness lemma ([arXiv:2610.08854](https://arxiv.org/abs/2610.08854)), so from a boundary
`p` of a completely tokenizable `x` it commits the longest token that is a prefix of `x[p ..)`.

**Lemma 7 (anchor propagation).** Let the token set have maximum token length `L`, let `W` occur in some completely
tokenizable input, and let gap `g` of `W` be forced. Define offsets `g = c_0 < c_1 < ... < c_m` by
`c_(i+1) = c_i + |τ_i|` for as long as `c_i + L <= k`, where `τ_i` is the longest token that is a prefix of
`W[c_i .. k)`. Then every `c_i` is a forced gap of `W`, and every gap strictly between `c_i` and `c_(i+1)` is crossed.

*Proof.* Take an occurrence of `W` at offset `t` in a completely tokenizable `x`, and suppose `t + c_i` is a boundary of
the tokenization of `x`, as it is for `i = 0` by hypothesis. The scan commits at `t + c_i` the longest token prefixing
`x[t + c_i ..)`, and since every token has length at most `L`, that token is determined by `x[t + c_i .. t + c_i + L)`,
which is `W[c_i .. c_i + L)` when `c_i + L <= k`. The committed token is `τ_i`, which exists because the occurrence is
tokenized, and it is the same at every occurrence. Its end `t + c_(i+1)` is a boundary and no boundary lies strictly
inside it, so induction on `i` gives both claims. ∎

**Corollary 7 (resolving width).** Let the token set have maximum token length `L`, let `x` be completely tokenizable,
and let `b` be a boundary of its tokenization with `b + L <= |x|`. Let `a < b` be a boundary that some window `W_a` of
width `h_a` forces: `W_a` occurs at `q`, and `a = q + g_a` for a forced gap `g_a`. Then the window `x[q .. e)` with
`e = max(b + L, q + h_a)` forces `b` at its gap `b - q`. A window of width at most `(b - a) + h_a + L` therefore forces
`b`.

*Proof.* The window contains `W_a` at offset 0, so its gap `g_a` is forced by monotonicity, and it occurs in `x`. Let
`a = p_0 < p_1 < ... < p_m = b` be the boundaries of `x` from `a` to `b`. Each is the end of the token the scan commits
at the one before, and each `p_i` with `i < m` satisfies `p_i + L < b + L <= e`. In the window's offsets these are the
`c_i` of Lemma 7, each with `c_i + L <= e - q`, so `c_m = b - q` is forced. The width is
`e - q = max((b - a) + g_a + L, h_a) <= (b - a) + h_a + L`. ∎

In words, a boundary that lies `d` bytes past an anchor forced by a window of width `h`, and at least `L` bytes before
the end of the input, is itself forced by a window of width at most `d + h + L` that reaches back to the anchor. This
gives the guarantee of Section 2 in window form, where a worker can cut at the known boundary and rescan the suffix. The
window carries the anchor, and the rescan is the lemma's induction. The bound is not the only route to a forced gap, and
the measurements below show windows forcing a boundary with no anchor in reach, from local structure the lemma does not
describe.

**Reach and its limits.** Fix a non-empty completely tokenizable input `x` and a width `H`. The *anchors of `x` at width
`H`* are the positions `t + g` with `0 < t + g < |x|` such that some window `x[t .. t + h)` with `h <= H` has gap `g`
forced. The *reach* `R_H(x)` is the length of the longest interval of `x` between consecutive anchors, with 0 and `|x|`
counting as anchors, which we call its longest *anchor spacing*. The *all-width reach* `R_∞(x)` admits every finite
width, and the *floor* `F(x)` is the length of the longest token of the tokenization of `x`. The reach is a distance on
one input, while the splitting companion's anchor-free stretch ([arXiv:2610.08854](https://arxiv.org/abs/2610.08854))
counts the positions of the longest run carrying no anchor from a fixed inventory of certificates, a supremum over all
completely tokenizable inputs. With the same anchors on one input, an interval `[a, b)` holds `b - a` positions,
unanchored but for `a`, so a distance `d` is a run of `d - 1`, or of `d` from 0 when position 0 carries none, and over
`{a}` the input `aa` has reach 1 and no unanchored position. Then `R_H(x) >= R_∞(x) >= F(x)`. The first inequality holds
because a wider budget adds anchors and removes none, and the second because every anchor is a boundary, so none lies
strictly inside the longest token, and some interval contains that token whole. The floor is shared by every method that
cuts only at boundaries; below it only a cut inside a token, resumed from scanner state, can go. Two facts locate the
middle term.

(a) With a maximum token length `L`, Corollary 7 makes every boundary `b` past an anchor `a`, with `b + L <= |x|`, an
anchor at some finite width, so past its first anchor and up to `L` bytes before its end, the all-width intervals of
such an input are single tokens. The interval before the first anchor need not be one. Over `{a, aa, bbb}` the input
`aaaaabbba` has boundaries 0, 2, 4, 5, 8, 9 and the all-width anchors 5 and 8 alone, since a longer run of `a` may
precede any occurrence, so the all-width reach is 5 against a floor of 3. The corollary gives the bound
`R_∞(x) <= max(a_1, F(x) + L - 1)`, with `a_1` the first anchor of `x`, taken as `|x|` when there is none. The head
interval has length `a_1`, and every other interval begins at an anchor, where the scan commits a token ending at a
boundary `b`; either `b` is an anchor and the interval is that token, or `b + L > |x|` by the corollary and the interval
ends within `L - 1` bytes past `b`. A brute-force check over every literal set of one to three tokens of length at most
3 over `{a, b}`, and every completely tokenizable input of length at most 10, found no violation in 82,336 inputs,
12,526 of them above the floor. Past the head, the middle term measures how far apart the anchors of the input lie, not
an ambiguity wider windows cannot resolve.

(b) No uniform radius exists for the JSON row. For `H >= 1` let `v_H` be the prefix of length `H` of the periodic byte
string `,1,1,1...`. The input `v_H` tokenizes into one-byte tokens, a comma or the number `1` each, so every gap of that
occurrence is a boundary. The input `"v_H"` is one string token, since neither byte is a quote, a backslash or a control
byte, so a token crosses every gap of that occurrence. Both inputs are completely tokenizable, so every gap of `v_H` is
open, at every width `H`; the two occurrences differ in whether an unmatched quote lies to their left, which no bounded
window sees. The conventional and split-friendly rows hold the same family between the quotes of a string literal, since
outside a string a comma is a punctuation token and `1` a number, and the string form excludes only a quote and a
newline. The block-comment rows hold it between `/*` and `*/`, which the family never closes, having no `*`, and the
bare row has neither strings nor comments, so the family says nothing there.

In the terms of constrained coding, read the token set as a tagged graph, the splitting companion's armed-run automaton
with each edge labeled by its byte and tagged by its boundary bit. A uniform radius `r` is the tagged analogue of the
`(m, a)`-sliding-block decodability of Marcus, Roth and Siegel (lecture notes 2001, Section 4.3), defined there for
tagged encoders, which the armed-run automaton is not. In that definition any two paths `e_(-m) ... e_0 ... e_a` that
generate the same word agree on the tag of `e_0`, and the radius maps to `(m, a) = (r, r - 1)` when `e_0` is the byte
after the gap, with `r` bytes preceding it and `r - 1` following it. The family above is its failure for every
look-behind and look-ahead, since the quoted and the bare reading are two paths over the same bytes with opposite tags
throughout. Locality in the sense of Béal, Berstel, Eilers and Perrin (Handbook of Automata Theory 2021, Section 6.1)
pins the state that a long enough label passes at a fixed position, where a uniform radius pins only the boundary tag,
and for a local automaton with `N` states the synchronization delay, the least window length that pins that state, is
quadratic in `N` in the worst case (Béal and Senellart, Theoretical Computer Science 1998). Deciding that ceiling for a
token set is an analysis of the cycles of the pair graph of the splitting companion's verifier, two copies run over
equal bytes, which this paper does not carry out.

**Measurements.** The campaign examined every distinct substring of width 1 to `H` of each corpus, with nothing sampled.
It decided the certificate exactly using `window_counterexample()` at every origin and the profile exactly using
`boundary_profile()`. Every search ran under the library's default cap of `2^20` states, and none reached it. An anchor
is a position strictly inside the corpus that a certified origin, or a forced gap, of an occurring window names, counted
once. Every anchor was checked against the sequential scan to be a token boundary, and every crossed gap was checked at
every occurrence. The record is the campaign note `notes/cut-contexts-campaign-2026-09-28.md` and its outputs
`explorations/cut-contexts/` in the author's research repository, which is private. The figures are exploratory until an
archived record exists, and the probe of Section 12 asserts none of them.

The library is munch, commit `df818b6a224221cf964095dd8e2c20735f506701`, one commit before the absence verdict moved to
one decision per window, a move that changed nothing here, since no run gave an absent gap. The JSON row runs over
`twitter.json` of simdjson v3.10.1 under the JSON lexer of hopper, the author's parser library,
`libs/json/src/lexer.cpp` at commit `caff92b`, unchanged since release v1.0.0 and compiled into the probe as it stands.
The shortest input on which this lexer differs from the recovery companion's JSON lexer is a string literal containing
an ill-formed UTF-8 byte, found by the library's segmentation decision. The C-like rows run over the 512 KiB generated
corpora of the recovery companion's campaign ([arXiv:2609.10600](https://arxiv.org/abs/2609.10600)), each under a token
set the same decision proves equivalent to the corpus's generator. Two literal vocabularies of the splitting companion's
supply study run over a 16 KiB slice of `twitter.json`: a byte-pair vocabulary of 384 merges trained on that corpus, 640
tokens, and the first 4,096 merges of the GPT-2 vocabulary, 4,352 tokens. The width `H` is 4 for JSON and C-like and 3
for the vocabularies, and Table 3 gives the anchor density and the longest anchor spacing of both detectors.

**Table 3.** Anchors from the certificate and from forced gaps at the campaign width `H`: distinct positions per KiB of
corpus, the gain of the profile over the certificate, and the longest anchor spacing in bytes under each, the corpus's
ends counting as anchors. The block-comment row is the campaign's token set proved equivalent to its corpus's generator,
and munch's own block-comment grammar gives the same figures on that corpus.

| Token set              | `H` | Anchors per KiB, certificate | Anchors per KiB, forced gaps |  Gain | Longest anchor spacing, certificate | Longest anchor spacing, forced gaps |
|------------------------|----:|-----------------------------:|-----------------------------:|------:|------------------------------------:|------------------------------------:|
| JSON, `twitter.json`   |   4 |                        94.64 |                       123.73 | 30.7% |                                 465 |                                 465 |
| C-like, conventional   |   4 |                        32.01 |                        32.01 |     0 |                                 104 |                                 104 |
| C-like, split-friendly |   4 |                        47.33 |                        47.33 |     0 |                                 104 |                                 104 |
| C-like, block comments |   4 |                         6.71 |                         6.71 |     0 |                                 965 |                                 965 |
| C-like, bare           |   4 |                       324.42 |                       324.42 |     0 |                                  10 |                                  10 |
| Byte-pair, 384 merges  |   3 |                       230.69 |                       240.31 |  4.2% |                                  61 |                                  61 |
| GPT-2, 4,096 merges    |   3 |                       620.88 |                       630.75 |  1.6% |                                  13 |                                  13 |

The gain has two sources. A forced gap `k` is the certificate of the window's one-byte extensions, so at the next width
the certificate can name that gap. The C-like rows gain only below the campaign width, and only this way. The
conventional row gains 2,140 positions at width 2, the split-friendly row 10,506 at width 1 and 2,140 at width 2, the
bare row 26,499 at width 1, and the block-comment row none, every one from the gap after the window. The gain is gone
from width 3 on the first two rows and from width 2 on bare, and the profile shortens no C-like interval at the campaign
width. The other source is several boundaries per window, which no certificate names, and 16,517 of JSON's 17,940
profile-only positions come from a gap inside the window. One such window is two spaces, a closing bracket and a
newline. No string holds a raw newline, so the occurrence lies outside every string, the bracket is a token and the
newline begins a whitespace run; gaps 2 and 3 are forced, while the certificate names 3 alone.

The 465-byte interval on JSON is one string literal, the floor itself, which nothing at any width can shorten. On the
vocabularies the gain is 4.2 and 1.6 percent, and at width 4 the 384-merge vocabulary's longest anchor spacing under
forced gaps falls from 61 to 40 bytes, the floor, while under the certificate it stays at 61. The model of Section 3
equals the exact certificate on every JSON and C-like row and falls short only on the vocabularies, at 224.88 against
230.69 anchors per KiB on the 384-merge vocabulary and 615.38 against 620.88 on the GPT-2 prefix.

A second run asked how wide a window must be to force the boundaries inside each corpus's largest intervals between
consecutive anchors. Up to 40 boundaries per row, evenly spaced, were asked of `boundary_counterexample()` over a ladder
of widths 4 to 128 with three splits each, and of a counterexample-guided growth beside it, every answer checked against
the scan. JSON and bare are at the floor, with no interval longer than the longest token, 465 bytes of one string on
JSON and 10 on bare, so nothing was searched there, but the other C-like rows are not. The conventional row's longest
interval is 104 bytes holding 17 token starts, where the longest token is 15 bytes, and the block-comment row's is 965
bytes holding 283, where the longest token is 135 bytes. The conventional and split-friendly corpora are byte-identical,
and both rows have a longest anchor spacing of 104 bytes. Every searched boundary resolves, all 40 on the conventional
and split-friendly rows by width 128, and on block comments 7 of 40 by 128 and all 40 by 1,024. Every resolving window
the search found on a C-like row reaches back to within a byte of the interval's left anchor, or past it, 40 of 40 on
the conventional row over distances of 5 to 95 bytes, 40 of 40 on split-friendly over 5 to 96, and 40 of 40 on block
comments over 1 to 912. A boundary 95 bytes past its anchor resolves with a guided window of 106 bytes, and one 912
bytes past with 933. These resolving windows resemble propagation from an anchor in Corollary 7, which proves it only
for a token set with a maximum token length, a hypothesis the C-like rows do not meet. Inside the conventional row's
searched intervals the longest goes 104, 101, 91, 82, 44 and 16 bytes at widths 4 to 128, and inside the block-comment
row's it goes from 965 to 857 at 128 and to 127 at 1,024. In these C-like samples, the windows that brought the longest
anchor spacing near the floor were about as wide as the original intervals.

The vocabularies differ from each other. The 384-merge vocabulary's 12 searched boundaries resolve by width 16, 11 of
them by windows that do not reach back to within a byte of the anchor, while the GPT-2 prefix's 6 resolve by width 16,
every one by a window reaching back to within a byte of its anchor, over 3 to 12 bytes.

Three neighbors frame the profile. Kaplan's pinch-points (Kaplan, CSLI Publications 2005), the concatenation check of
Lester, Ong and Schäfer (Journal of Computer Security 2016) and ZipLex's separability predicate (Chassot and Kunčak, CAV
2026) each certify one join, of a text at a position or of two code strings, where a forced gap quantifies over every
occurrence in every completely tokenizable input. Resolved context provides reach past the floor where boundaries
cannot, as in the data-parallel simulation of Mytkowicz, Musuvathi and Schulte (ASPLOS 2014) and the bounded alternative
states of Barenghi et al. (Science of Computer Programming 2015), which enter a chunk in every live state and pay a scan
per state kept.

## 12 Evaluation

The artifact runs the evaluation in the default test target and CI; the figures of this section are asserted, so a
drifted number fails the test suite.

The random sweep is the output of `tools/probes/src/window_gate.cpp` at munch commit
707a79941472885a260c0bc96e615dd2fb5e99d2, the commit at which the positive-width equivalent entered the library. The
v1.3.3 release the rest of this evaluation describes excludes the nullable sets and asserts the earlier counts. Over 400
random token sets on a three-symbol alphabet, generated by the probe with a pinned seed and draw order, 266 are nullable
and are decided through their positive-width equivalent, as the artifact compiles them, and 63 certify at least one byte
exactly. Of the 337 that certify no byte, **326 gain a certified window under the model, and 322 of those are
witnessed**. For each of the 322, the bounded search finds a completely tokenizable input containing a certified window,
with the covering token beginning at the returned origin, and each input is verified as it is constructed, with the 322
aggregate asserted. Another 4 model-positive grammars have no occurrence within the bounded witness search and are
counted as unresolved, never as rescues, and the remaining 11 exhaust the quotient with no window under the model; none
are inconclusive. Occurrence is a property of the concrete word and is not determined by its quotient key, so the
witness search continues past the shortest certified length instead of stopping at the first certifying word.

Separate rewind-stress rows exercised 1,079,392 generated executions that scanned through the window and contained at
least one rewind, with zero disagreements against the shipped scanner; 418,466 of those executions tokenize their whole
input completely and the remaining 660,926 have malformed suffixes past the window, both counts asserted. This is an
implementation stress check, since the generated inputs were required to scan through the window, with no requirement of
complete tokenization. The random sweep additionally checked every certified two-byte window over the probe's generated
contexts. The length-one case reproduces the published certificate on all 400 grammars, the 266 nullable ones included.

Among the named token sets, all six exact-empty rows of the predecessor's applicability table, five C-like variants and
JSON, gain witnessed windows, and one new cumulative C-like variant joins them as a seventh positive row. Table 4 shows
the concrete windows, with the retained key counts, one indicator of the search footprint, in place of the `6^|Q⁺|`
bound, and beside the gate's six-status keys it gives the keys of the three-status quotient and the nodes of the
chosen-origin search of Section 6. Those come from the figure program `paper/figures/window_search.cpp`, added after
release v1.3.3, which runs both over the same token sets, asserts that their shortest lengths equal the gate's on every
row, and pins the counts.

The example windows show the recovery anchors. The string and line-comment rows resynchronize at a newline followed by a
byte that must begin a token, and the block-comment rows at the byte pair `*/` followed by whitespace, in comment
context the closer, with the origin immediately after the pair. The certificate is occurrence-universal, so it also
covers occurrences where `*/` reads as two operator tokens; the pinned witnesses exercise that reading. The conventional
row certifies at length two because its whitespace runs include the newline, so `!` must begin a token there as well.
The JSON row uses the RFC 8259 lexical forms (Bray, RFC Editor 2017) over bytes and assumes UTF-8 validity; it is not a
conforming JSON processor. Section 11 measures the anchor density supplied by certified windows, in an exploratory
campaign the artifact does not assert, and under hopper's JSON lexer those of width at most 4 name 94.64 anchors per KiB
of `twitter.json`. The run `a⁺` is included as the negative row, and `a*` would be decided as the same automaton, in
which every byte continues a run as readily as it begins one. The model certifies no window, since absent-byte windows
certify only vacuously under Definition 1, and the search exhausts its quotient, which is the shape of a model-negative.

**Table 4.** Certified windows for the named token sets, from the gate's asserted rows and the figure program's: the
shortest model-certified length, one example window with its origin, and the quotient keys the search retained before
shortest-window stopping, under the six-status key of the gate and the three-status key of Section 6.1. The last column
is the nodes the chosen-origin search of Section 6.2 holds when its shortest window appears, or when the search
exhausts. Every positive row's displayed window carries an asserted occurrence witness. Among the positive rows the
cumulative one is new to this study, and every other positive row is an exact-empty row of the predecessor's table.

| Token set                      | Shortest model-certified `k` | Example window at origin | Retained keys, six-status | Retained keys, three-status | Chosen-origin nodes |
|--------------------------------|-----------------------------:|--------------------------|--------------------------:|----------------------------:|--------------------:|
| C-like, string literals        |                            2 | `\n!` at 1               |                        24 |                          20 |                  17 |
| C-like, line comments          |                            2 | `\n!` at 1               |                        18 |                          13 |                  14 |
| C-like, block comments         |                            4 | `\t*/\t` at 3            |                        53 |                          30 |                  30 |
| C-like, conventional           |                            2 | `\n!` at 1               |                        27 |                          22 |                  16 |
| split-friendly, block comments |                            4 | `\n*/\t` at 3            |                       188 |                         103 |                  81 |
| C-like, cumulative (new here)  |                            4 | `\n*/\t` at 3            |                       189 |                         102 |                  79 |
| JSON, RFC 8259                 |                            2 | `\t"` at 1               |                        69 |                          52 |                  29 |
| `a⁺` (negative row)            |                         none | search exhausted         |                         3 |                           2 |                   2 |

## 13 Related work

The window generalizes the certified split point of the predecessor
([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)), and inherits its relation to the parallel lexing families:
composition carries every state and pays for it (Mytkowicz, Musuvathi and Schulte, ASPLOS 2014); speculation predicts an
entry state, validates, and re-executes on a miss (Prabhu, Ramalingam and Vaswani, PLDI 2010); bounded alternative-state
scans disambiguate without single-state speculation (Barenghi, Crespi Reghizzi, Mandrioli, Panella and Pradella, Science
of Computer Programming 2015); prescanning pays a pass over the input (Li, Sato, Liu and Taura, IPDPS 2021). The window,
like the byte, is derived from the grammar before any input exists. A close compiler-style neighbor in the maximal-munch
setting is the streaming analysis of Li, Yang, and Mamouras (ASPLOS 2026), which statically computes a grammar's maximum
token-neighbor distance and uses the resulting bounded lookahead to emit a sequential maximal-munch stream without
backtracking. Their scan advances from a known token boundary, so the window decides maximality, and parallelization is
left there as future work. On the formal side of the same setting, ZipLex formally verifies linear-time invertible
maximal-munch lexing, with an abstraction capturing the separability of tokens in a sequence (Chassot and Kunčak, CAV
2026), and Li and Mamouras formalize the uniform tokenization problem and give `O(mn)`-time algorithms, linear in text
length `n` for a fixed grammar of size `m`, precomputing what each suffix admits in a right-to-left pass before
tokenizing from the input's start (OOPSLA 2025). Neither line of work derives an occurrence-universal raw-window
certificate or recovers the covering token's origin from an arbitrary occurrence without scanning from a known boundary.
Lester's boxing check anticipates the flavor at a known join. Every lexer state possible after the first analyzed string
must either already emit a token, making the following character irrelevant to that string's lexing, or emit the same
token immediately on every character that may begin the second, so the join is a lexeme boundary and concatenation
preserves token boundaries. The check is stated compactly in the position paper and in full in the journal treatment
(Lester, PLAS 2013; Lester, Ong and Schäfer, Journal of Computer Security 2016); it judges one join of two analyzed
fragments at a time, and it neither certifies every occurrence of a grammar-derived word nor recovers a covering origin.

Recent LLM-tokenizer work addresses input-specific seams instead. LoPT validates position-aligned tokenizations of
overlapping chunks and adjusts chunk length where needed (Shao, Zheng, Wang, Zheng, Li and Fan, ACL 2026), and for BPE,
recent work bounds the streaming delay of ordered merge rules from a known beginning (Mamouras, Li and Yang, PLDI 2026)
or maintains the tokenization incrementally over every prefix (Jiang and Gong, ICML 2026); Hayase, Liu, Smith, and Oh
enumerate, for a byte prefix of BPE-tokenized text, the tokenizations whose last token straddles the prefix end, a valid
covering tree over one concrete input (ICML 2026). The certificate here is grammar-derived and occurrence-universal,
where that work is input-relative and enumerative, and the certificate fixes one origin for every occurrence. The
parsing side of that pipeline is active again. Cyclic operator precedence grammars (Chiari, Mandrioli and Pradella,
Information and Computation 2025) admit parallel-suitable chunking of flat unbounded terminal-string substructures, with
precedence relations defined between adjacent terminals; in a conventional pipeline those terminals are lexer output, so
that application presupposes tokenization, though the formalism is not restricted to pre-tokenized input.

A classical antecedent of window-determines-state is the definite automaton, studied in depth by Perles, Rabin, and
Shamir (IEEE Transactions on Electronic Computers 1963); its operational form for a fixed `k` is `k`-locality, in which
any `k` consecutive symbols force a unique state, every word of length `k` being synchronizing (Holub and Štekr, CIAA
2009). Both quantify uniformly over all windows and speak of states; the certificate here is per-window, speaks of token
boundaries under maximal munch, and recovers an origin. Raw-state synchronization alone does not identify the start of
the covering maximal-munch token.

The classical special case of boundary recovery is code synchronization, and Proposition 1 states the relation exactly
for prefix codes, where left-to-right tokenization realizes the code factorization. Over a prefix code the certified
windows that occur are the synchronizing splits whose right half sits inside one codeword, and a synchronizing pair
(Berstel, Perrin and Reutenauer, Cambridge University Press 2010) with a nonempty right component yields, on its
concatenation, a certified window whose origin is the start of the right component's last codeword. The pair notion is
older than the book. Lassez defines a pair of words of `X*` as synchronizing when every occurrence of it in a word of
`X*` forces a factorization cut between its halves, and proves that whether a regular code has a bounded synchronization
delay is decidable, through the syntactic monoid of `X*` (Lassez, Groupe d'étude d'algèbre 1976, Théorème 2). Carpi and
D'Alessandro define synchronizing pairs for finite sets that need not be codes and, for a regular synchronizing code,
characterize them on an unambiguous automaton for `X*` (Carpi and D'Alessandro, Theoretical Computer Science 2017,
Definition 2 and Lemma 2(4)). An early member of that family is the comma-free code of Golomb, Gordon and Welch
(Canadian Journal of Mathematics 1958), a block code no codeword of which occurs across the boundary of two adjacent
codewords, so that every codeword is a certified window with origin 0 for the code's own factorization. Finite
synchronization delay (Restivo, Theoretical Computer Science 1975) bounds how many codewords a synchronizing pair needs,
hence a byte bound for a finite code; the resumption argument, in which an occurrence of `ww` in a coded message, for a
synchronizing word `w`, lets decoding resume independently from the position after the first `w`, survives into the
partial-DFA treatment (Berlinkov, Ferens, Ryzhikov and Szykuła, DMTCS 2026). The explicit modern bounded-window form of
that special case is the synchronizing morphism, for which a window of bounded length suffices to detect boundaries
between codewords (Fici, Romana, Sciortino and Urbina, MFCS 2025), in a morphic code-factorization setting. Uniquely
decipherable codes may share prefixes; what they guarantee is a unique factorization, with no maximal-munch priority
resolving overlaps between competing token languages, and the origin machinery here addresses that difference.

The relationship to reset words is one-way and stops at length one. A useful certified byte induces a reset-like action
on the partial live automaton with domain `{q0}`, under the stated re-entrancy qualification, a correspondence that
fails under the classical complete-DFA reading (Volkov, LATA 2008). The correspondence does not extend. A certified
window need not be a reset word of the token DFA at all, since over `{a, b}` the window `ab` certifies at origin 1 while
the action of `ab` on the partial automaton is empty, and a rank-one letter need not certify either, since over the
token language `b*a` the letter `b` can act with rank one while `q0` is re-entrant and `b` occurs inside tokens.
Certified windows synchronize token-origin information in an enriched cloud model; they need not synchronize the raw
token DFA.

The complexity of the neighborhood is known. Checking careful synchronizability of a partial automaton, and finding a
shortest carefully synchronizing word, are PSPACE-complete already over two-letter alphabets (Martyugin, CSR 2010,
Theorem 1), careful meaning the word stays defined from every state and maps all states to one. Section 6.3 proves the
model's existence problem PSPACE-complete from the same source, the intersection non-emptiness of DFAs (Kozen, FOCS
1977), by a reduction of the shape Martyugin uses for careful synchronization of a partial automaton with a zero
(Martyugin, CSR 2010, Proposition 2). In Martyugin's construction, a first letter sends every state to a source's start
or to the zero, the common word runs the sources, and a final letter is defined exactly where the sources accept and at
the zero. Here the prefix `r^(m+1) t` gives each source's start one fresh origin and parks the pairs from before the
window at the guard `G`, and the final `#` is undefined exactly at the accepting copies and at `G`, since the model
needs competing histories to die rather than to merge. A careful reset word maps every state of the automaton to one,
where a certified window need not synchronize the token DFA's states, only make the cloud's survivors agree on an
origin. The per-grammar retained-key counts, one footprint indicator, stayed far below the worst case throughout.

Two practices are the practical counterparts. Compiler panic-mode recovery discards input to a recovery set (Aho, Sethi
and Ullman, Addison-Wesley 1986, Section 4.1), which may itself be grammar-derived; the distinction is post-error parser
recovery against a pre-input lexical boundary guarantee. Incremental lexing in the style of Wagner and Graham restarts
from per-token scanner-state snapshots in a versioned token stream, with dynamically maintained lookahead dependencies,
exact for the text they were cached against (Wagner and Graham, manuscript 1997); a certified window is
grammar-universal instead, valid in every completely tokenizable context and known before any input exists, at the price
of existing only where the token set admits one.

Very recent concurrent work establishes nearby but different local guarantees. TokTier certifies input-relative splice
junctions for selected frozen pre-tokenizer and BPE pipelines. It matches cached against fresh runs, uses
family-specific synchronizing character-class transitions proved to reset the pre-tokenizer under every left context,
and proves exact recombination of its overlapping windows (Zhang and Cao, arXiv:2607.29678, 2026). ReTokSync monitors
the receiver-view tokenization of one concrete generated stream and triggers a corrective reset when ambiguity occurs
(Wang, Wang, Pang, Han, Qi, Hu and Chen, arXiv:2604.25486, 2026). Borsotti, Crespi Reghizzi, and Pradella define
precedence relations over synthesized attributes for a deterministic parser using one-symbol left and right
neighborhoods (SSRN 2026). Gigatoken certifies cuts before selected punctuation in SentencePiece input where no
vocabulary piece contains the two symbols at the cut in that order, a sufficient condition argued in a source comment
and checked by tests (Rød, Gigatoken, 2026), which is a certified window of width two in subword form. None decides
occurrence-universal bounded words for competing prioritized token languages under maximal munch, nor recovers the
origin of the token covering an arbitrary cut.

Two formal treatments of tokenization address a different question. Kaplan models a language's tokenizing conventions as
a finite-state transduction and emits output at input-relative pinch-points, positions where all live analysis paths
converge to a single transducer state, so settled prefixes stream out as a particular text is processed (Kaplan, CSLI
Publications 2005). Cognetta and Okazaki encode the tokenizations of a regular language as a finite-state transduction
and represent canonical MaxMatch and BPE subword tokenizers as transducers (Computational Linguistics 2025). Neither
derives occurrence-universal windows from the grammar alone, and neither recovers the origin of the covering
maximal-munch token at an arbitrary occurrence.

Symbolic dynamics comes closest in shape. A resolving block is a block of a factor subshift all of whose admissible
preimages agree at a selected coordinate (Adler, Coppersmith and Hassner, IEEE Transactions on Information Theory 1983;
Marcus, IEEE Transactions on Information Theory 1985), an occurrence-universal lift of a local observation to hidden
state, which Adler, Coppersmith and Hassner define in their Definition 3.3 and use as a means of resetting an encoding
automaton in sliding-block code construction (Adler, Coppersmith and Hassner, IEEE Transactions on Information Theory
1983, Definition 3.3). A certified window shares that shape, reading the window as the observable block and the covering
token's start as the hidden coordinate; the classical theory, however, is stated for factor maps of subshifts, without
token priorities, maximal munch, backup, or any tokenization semantics, and it neither defines nor decides the lexical
instantiation.

The areas surveyed above are definite and local automata, synchronizing words and codes under both the complete and the
partial reading, resolving blocks, careful synchronization, streaming, verified, and uniform maximal-munch tokenization,
incremental relexing, the parallel lexing families, and the parallel parsing line. Two special cases lie outside the
claim that follows. In codes, synchronizing pairs and bounded windows already recover codeword boundaries, and bounded
delay is decidable for a regular code (Lassez, Groupe d'étude d'algèbre 1976; Restivo, Theoretical Computer Science
1975; Fici, Romana, Sciortino and Urbina, MFCS 2025); in single formats, a proved two-character pattern fixes the state
of a CSV automaton (Ge, Li, Eilebrecht, Chandramouli and Kossmann, SIGMOD 2019). Beyond those cases we have not found
prior work in these areas that defines in general, for competing prioritized token languages under maximal munch, a
grammar-derived bounded word whose every occurrence identifies the origin of the covering token at an arbitrary cut. Nor
have we found, in these areas, prior work instantiating resolving-block or local-decoding machinery for prioritized
maximal-munch token origins, nor a derivation of this conservative compiled-table cloud model; the closest results
decide maximality from a known boundary, bound retained memory, assume the boundaries they schedule, recover a state
without an origin, or validate a concrete overlap or splice after local retokenization.

We claim no novelty for the decidability of the exact certificate of a proposed window. Longest-match tokenization is a
finite-state transduction (Karttunen, ACL 1996), so the property is regular and a proposed window is decided by standard
closure, and what this paper supplies is the conservative model, its soundness, the finite quotient that makes the
search terminate and the specializations, together with the smaller searches, the complexity of the model's existence
problem, the factorization semantics of the cloud, explicit constructions for the witnessed and global-completeness
questions, and the boundary profile's cut results. We are likewise not aware of an implementation that derives certified
windows from a compiled maximal-munch token set and checks them against a running scanner, other than the instruments
this paper describes and their supported successor in release 1.4.0. For a subword vocabulary, Gigatoken derives
two-symbol cuts before selected punctuation from a SentencePiece vocabulary and checks the chunked output against its
own serial encoding in its tests (Rød, Gigatoken, 2026).

## 14 Limitations

All deliberate. The soundness proof speaks only of completely tokenizable inputs, so malformed input is outside the
proof, and no consumed-prefix analogue is claimed. Negatives are model-relative. The model refuses windows a greedy
scanner would allow, so an exhausted search means no window *under this model*, never that none exists. The worst case
is exponential, and Section 6.3 shows that no polynomial algorithm for the model's existence question exists unless P =
PSPACE, with shortest windows themselves of length `2^Ω(√n)` on a family of token sets. The probe and the shipped search
are budgeted, though the retained keys stayed below 200 on every named row, at most 32 with mean 9.9 among the 337
no-byte grammars. The complexity results concern the model and the compiled automaton; the complexity of Definition 1
itself, and bounds in the size of a regular expression, are not claimed. The decision of global completeness has an
EXPSPACE upper bound and no matching lower bound, and the exact witnessed synthesis was not run over the random sweep.

The evaluated v1.3.3 artifact ships no window-planning API, and its probe excludes the nullable sets, so the sweep of
Section 12 is reproduced at the later commit named there; its certificate machinery is a probe, on the principle that
the certificate's formulation should freeze in this paper before becoming a public contract. Release 1.4.0 made the
formulation a public contract, `is_split_window()`, which the sweep at the later commit asks beside its own model at the
coverage the sweep's source states: every one-byte answer of the named rows and of the 400 random token sets, every
model-positive random two-byte answer, and the named certificates and refusals, with the count of those checks pinned.
The window planner that release added beside the decision lies outside this paper's evaluation. The real-corpus evidence
is the exploratory campaign of Section 11, recorded outside the artifact and asserted by no test, and window occurrence
frequency on representative corpora remains a question for a controlled campaign.

## 15 Conclusion

A certified split window `(W, o)` of a compiled token set is a byte string such that in every completely tokenizable
input containing `W`, the token covering the occurrence's final byte begins exactly `o` bytes into it. Model
certification is decided from the compiled tables alone by running a conservative cloud of token-prefix hypotheses
across the window, seeding new trajectories only where the automaton accepts, and demanding unanimity on an in-window
origin. The soundness argument is a representation invariant. The final segmentation's actual token-prefix history is
always among the hypotheses, so a unanimous origin is always the actual one, and maximal-munch backup never needs
simulating, because the model tracks where tokens begin.

The search is a decision procedure for the model. A quotient of the reachable clouds, exact under the stated
single-occupancy invariant, makes breadth-first search terminate by exhaustion, so every answer is a certified window
with its origin or a proof that the model admits none at any length. Two smaller searches decide the same question, a
three-status quotient and an unambiguous automaton that follows one chosen origin, the latter shipped as
`shortest_split_window()` by release 2.2.0 of munch. Existence of a model certificate is PSPACE-complete, plain or
witnessed, so no polynomial-time algorithm decides it unless P = PSPACE, and separately a shortest window can be
`2^Ω(√n)` bytes long over `n` states. The cloud's hypotheses are exactly the histories of arbitrary token factorizations
of words of `X*`, the concatenations of tokens, which is why the vacuous window is certified, why token kinds do not
matter to the search, why the model is exact on the polynomially decidable class where every factorization is the
scan's, and how a shortest witnessed window and the model's global completeness are decided.

The three-layer reading of the results is intentional and permanent. Model-positive answers are semantically certified
within the stated flat, completely-tokenizable scope. Negatives are model-relative. The model refuses windows a greedy
scanner would allow, the two asserted examples exhibit the over-approximation, and the strictness corollary locates its
minimum nonvacuous length at two, since at length one the model provably coincides with the published predicate, made
exact for occurring bytes by the predecessor's necessity theorem. Over a prefix code the certificate is code
synchronization under another name, the occurring certified windows being the synchronizing splits whose right half sits
inside one codeword, and the named token sets studied here are not codes. A token set in which some token matches the
empty string is decided through its positive-width equivalent, which unrolls the accepting start state and changes no
scan, so nothing is set aside on that account.

Of the 337 random token sets certifying no byte, 322 gain a witnessed certified window with zero inconclusive searches,
and the predecessor's six exact-empty rows all gain witnessed windows of two to four bytes, with the concrete windows in
Table 4. Separate rewind-stress rows exercised 1,079,392 generated executions that scanned through the window and
contained at least one rewind, with zero disagreements against the shipped scanner, and the figures are printed and
asserted in the artifact, so they fail the test suite if they move. By choice, this paper stops short of the cut itself,
whose construction requires explicit composition with the predecessor's prefix-stability result. A controlled evaluation
on representative corpora, with planning times, occurrence frequencies and end-to-end comparisons against byte
certificates, is the natural next step.

Read gap by gap, the certificate is one pattern of a window's boundary profile, one forced gap followed by crossed gaps
to the window's end. A rule that cuts at a window's gap and reconciles nothing is sound exactly at forced gaps, so the
profile is the whole anchor supply a window of its width can give such a rule. Over a token set with a maximum token
length `L` a forced gap propagates, the scan from it forcing every boundary it commits with its lookahead inside the
window, so a boundary lying after an anchor and at least `L` bytes before the end of the input resolves at a width of at
most its distance from the last preceding anchor plus that anchor's window width and `L`. No single width decides every
gap of the JSON row, or of the C-like rows with strings or comments, since no bounded window sees whether an unmatched
quote or an open comment lies to its left. At the campaign width the profile adds anchors on JSON and the byte-pair
vocabularies, none on the C-like rows, and shortens no longest anchor spacing, whose floor is the longest token.

Like its predecessor, this paper contributes the certification step and no new way to recover context. It extends that
step from single bytes to bounded windows, with the origin recovered and the conservatism exhibited. Certified windows
synchronize token-origin information in an enriched cloud model, and need not synchronize the token DFA itself; the
predecessor's one-way reset-word connection applies at length one, and no general implication extends to longer windows.

## References

- N. Nidhögg. *Certified Split Points for Parallel Lexing: Exact and Modulo Discarded Tokens.* Preprint,
  arXiv:2608.03473v2, 2026; the applicability table cited is that of versions 1 and 2, whose six exact-empty rows
  version 3 extends to eight.
- N. Nidhögg. *Certified Splitting: Decidability and Anchor Supply for Maximal-Munch Tokenization.* Preprint,
  arXiv:2610.08854, 2026.
- N. Nidhögg. *Certified Panic Mode: Repair-Invariant Error Recovery for Maximal-Munch Lexing.* Preprint,
  arXiv:2609.10600, 2026.
- N. Nidhögg. *munch.* Release tag v1.3.3, archived at doi:10.5281/zenodo.21842344, 2026; a lexical analysis library
  based on automata theory. The artifact evaluated here is that release, with its single-byte decision and a single-byte
  planner this paper does not evaluate; release v1.4.0 added the window decision is_split_window(), which the sweep at
  the later commit cross-checks, and the window planner, which is not evaluated here; the 400-set sweep here is the
  output of tools/probes/src/window_gate.cpp at commit 707a79941472885a260c0bc96e615dd2fb5e99d2 of the public
  repository, after release v1.6.0.
- N. Nidhögg. *hopper: a C++23 library for building recursive-descent parsers on top of munch lexers.* Public
  repository, https://github.com/nnidhogg/hopper, 2026. The JSON lexer libs/json/src/lexer.cpp, read at commit caff92b
  and unchanged since release v1.0.0, archived at doi:10.5281/zenodo.22998778, is the token set of the JSON row of
  Section 11.
- T. Mytkowicz, M. Musuvathi, W. Schulte. *Data-Parallel Finite-State Machines.* ASPLOS 2014, 529-542.
- P. Prabhu, G. Ramalingam, K. Vaswani. *Safe Programmable Speculative Parallelism.* PLDI 2010, 50-61.
- A. Barenghi, S. Crespi Reghizzi, D. Mandrioli, F. Panella, M. Pradella. *Parallel parsing made practical.* Science of
  Computer Programming 112:195-226, 2015.
- L. Li, S. Sato, Q. Liu, K. Taura. *Plex: Scaling Parallel Lexing with Backtrack-Free Prescanning.* IPDPS 2021,
  693-702.
- A. W. Li, Y. Yang, K. Mamouras. *Static Analysis for Efficient Streaming Tokenization.* ASPLOS 2026, 1880-1896.
- S. Chassot, V. Kunčak. *Formally Verified Linear-Time Invertible Lexing.* CAV 2026, LNCS 16683, 141-164.
- A. W. Li, K. Mamouras. *Efficient Algorithms for the Uniform Tokenization Problem.* PACMPL 9(OOPSLA1), article 133,
  1492-1518, 2025.
- M. M. Lester. *Position Paper: The Science of Boxing: Analysing Eval Using Staged Metaprogramming.* PLAS 2013, 83-88.
- M. Rød. *Gigatoken.* https://github.com/marcelroed/gigatoken, revision 542367a of 21 July 2026, released on PyPI as
  version 0.9.0. The vocabulary pair rule is the comment in `finalize_speed_paths` of `src/bpe/sentencepiece.rs`, and
  the chunked output is held to the serial encoding by the test `sp_parallel_fragmented_matches_serial` of
  `src/batch.rs`.
- J.-L. Lassez. *Messages parasites et codes synchronisants.* Groupe d'étude d'algèbre, tome 1 (1975-1976), exposé no.
  17 of 28 May 1976, 17-01 to 17-04. https://numdam.org/item/GEA_1975-1976__1__A17_0/
- A. Carpi, F. D'Alessandro. *On Incomplete and Synchronizing Finite Sets.* Theoretical Computer Science 664:67-77,
  2017.
- M. M. Lester, L. Ong, M. Schäfer. *Information Flow Analysis for a Dynamically Typed Language with Staged
  Metaprogramming.* Journal of Computer Security 24(5):541-582, 2016.
- W. Shao, L. Zheng, P. Wang, P. Zheng, J. Li, Y. Fan. *LoPT: Lossless Parallel Tokenization Acceleration for Long
  Context Inference of Large Language Model.* ACL 2026, 33107-33122. Preprint: arXiv:2511.04952.
- K. Mamouras, A. W. Li, Y. Yang. *An Efficient Algorithm for Streaming BPE Tokenization.* PACMPL 10(PLDI), article 252,
  2085-2108, 2026.
- S. Jiang, R. Gong. *Incremental BPE Tokenization.* Accepted to ICML 2026 (Spotlight). Preprint: arXiv:2605.30813.
- J. Hayase, A. Liu, N. A. Smith, S. Oh. *Sampling from Your Language Model One Byte at a Time.* ICML 2026. Preprint:
  arXiv:2506.14123, version 3 of 7 May 2026.
- Z. Zhang, Z. Cao. *TokTier: Exact Stateful CPU+GPU Tokenization for Agentic LLM Serving.* Preprint, arXiv:2607.29678,
  version 2 of 3 August 2026.
- Y. Wang, R. Wang, W. Pang, J. Han, Y. Qi, D. Hu, K. Chen. *ReTokSync: Self-Synchronizing Tokenization Disambiguation
  for Generative Linguistic Steganography.* Preprint, arXiv:2604.25486, 2026.
- A. Borsotti, S. Crespi Reghizzi, M. Pradella. *Attribute-Based Precedence Relations for Context-Free Grammars.*
  Preprint, SSRN, doi:10.2139/ssrn.6890975, 2026.
- M. Chiari, D. Mandrioli, M. Pradella. *Cyclic operator precedence grammars for parallel parsing.* Information and
  Computation 307:105363, 2025.
- M. A. Perles, M. O. Rabin, E. Shamir. *The Theory of Definite Automata.* IEEE Transactions on Electronic Computers
  EC-12(3):233-243, 1963.
- J. Holub, S. Štekr. *On Parallel Implementations of Deterministic Finite Automata.* CIAA 2009, LNCS 5642, 54-64.
- J. Berstel, D. Perrin, C. Reutenauer. *Codes and Automata.* Encyclopedia of Mathematics and its Applications 129,
  Cambridge University Press, 2010.
- S. W. Golomb, B. Gordon, L. R. Welch. *Comma-Free Codes.* Canadian Journal of Mathematics 10:202-209, 1958.
- A. Restivo. *A Combinatorial Property of Codes Having Finite Synchronization Delay.* Theoretical Computer Science
  1(2):95-101, 1975.
- M. V. Berlinkov, R. Ferens, A. Ryzhikov, M. Szykuła. *Synchronization of Strongly Connected Partial DFAs and Prefix
  Codes.* DMTCS 28:2(34), 2026. Extended version of the STACS 2021 paper.
- G. Fici, G. Romana, M. Sciortino, C. Urbina. *Morphisms and BWT-Run Sensitivity.* MFCS 2025, LIPIcs 345, 49:1-49:18.
- M. V. Volkov. *Synchronizing Automata and the Černý Conjecture.* LATA 2008, LNCS 5196, 11-27.
- P. V. Martyugin. *Complexity of Problems Concerning Carefully Synchronizing Words for PFA and Directing Words for
  NFA.* CSR 2010, LNCS 6072, 288-302.
- D. Kozen. *Lower Bounds for Natural Proof Systems.* 18th Annual Symposium on Foundations of Computer Science (FOCS),
  254-266, 1977. doi:10.1109/SFCS.1977.16
- A. V. Aho, R. Sethi, J. D. Ullman. *Compilers: Principles, Techniques, and Tools.* Addison-Wesley, 1986; reprinted
  with corrections, March 1988.
- C. Ge, Y. Li, E. Eilebrecht, B. Chandramouli, D. Kossmann. *Speculative Distributed CSV Data Parsing for Big Data
  Analytics.* SIGMOD 2019, 883-899.
- L. Karttunen. *Directed Replacement.* Proceedings of the 34th Annual Meeting of the Association for Computational
  Linguistics (ACL 1996), 108-115.
- T. A. Wagner, S. L. Graham. *General Incremental Lexical Analysis.* Manuscript, University of California, Berkeley,
  1997; the circulating build is the December 1999 revision. https://harmonia.cs.berkeley.edu/papers/twagner-lexing.pdf
- R. M. Kaplan. *A Method for Tokenizing Text.* In Inquiries into Words, Constraints and Contexts: Festschrift for Kimmo
  Koskenniemi on his 60th Birthday, CSLI Publications, 55-64, 2005.
- M. Cognetta, N. Okazaki. *Tokenization as Finite-State Transduction.* Computational Linguistics 51(4):1119-1149, 2025.
- R. L. Adler, D. Coppersmith, M. Hassner. *Algorithms for Sliding Block Codes: An Application of Symbolic Dynamics to
  Information Theory.* IEEE Transactions on Information Theory 29(1):5-22, 1983.
- B. H. Marcus. *Sofic Systems and Encoding Data.* IEEE Transactions on Information Theory 31(3):366-377, 1985.
- B. H. Marcus, R. M. Roth, P. H. Siegel. *An Introduction to Coding for Constrained Systems.* Lecture notes, fifth
  edition, October 2001. https://ronny.cswp.cs.technion.ac.il/wp-content/uploads/sites/54/2016/05/chapters1-9.pdf
- T. Bray (Ed.). *The JavaScript Object Notation (JSON) Data Interchange Format.* RFC 8259, STD 90, RFC Editor, December
  2017.
- M.-P. Béal, J. Berstel, S. Eilers, D. Perrin. *Symbolic Dynamics.* In Handbook of Automata Theory, Volume II, EMS
  Press, 987-1030, 2021; cited from arXiv:1006.1265v3.
- M.-P. Béal, J. Senellart. *On the Bound of the Synchronization Delay of a Local Automaton.* Theoretical Computer
  Science 205(1-2):297-306, 1998.
