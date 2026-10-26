# Certified Panic Mode: Repair-Invariant Error Recovery for Maximal-Munch Lexing

**Nicklas Nidhögg**, September 6, 2026. Mirrors `paper/panic-mode/panic-mode.tex`, published as
[arXiv:2609.10600](https://arxiv.org/abs/2609.10600), which evaluates munch at the v1.6.0 release: the recovery
primitive `Lexer::next_certified_start()`, its evidence-returning sibling `next_certified_evidence()`, and the
tokenizer's `recover()` family, measured for recovery quality rather than throughput. The campaign instrument is
`tools/probes/src/recovery_quality.cpp` of that release, invoked as
`munch_recovery_quality 512 500 out.csv twitter.json 3` with every seed fixed, so the whole experiment is deterministic;
the pooled per-row table its analysis program generates is checked in under `paper/panic-mode/data/r6/` and read into
the paper verbatim, and the full CSV, the generated corpora, the analysis programs, and a pinned reproduction script are
deposited at doi:10.5281/zenodo.22178507, from which every campaign statistic recomputes. The tables below are produced
mechanically from those files and from the paper's own tabular sources, never transcribed by hand; the tables and
numbers of Section 4 come from `paper/panic-mode/data/exact/`, written by `paper/panic-mode/checks/repair_profile.py`
and held to it by the test suite.

*A technical report on the recovery contract behind `Lexer::next_certified_start()`, `next_certified_evidence()`, and
`Tokenizer::recover()`. The implementation, tests, and probes live in this repository; this document states the contract
precisely, relates it to prior work, and reports what the corruption study measured.*

## Abstract

Classical panic-mode recovery skips a failed scan forward by convention, promising progress but no repair-universal
boundary guarantee for where it lands. We choose the resume position by theorem, via a committed-prefix lemma over
certificates posted for parallel lexing ([arXiv:2608.03473](https://arxiv.org/abs/2608.03473),
[arXiv:2608.09761](https://arxiv.org/abs/2608.09761)): a position is a sound recovery point when every prefix repair
whose scan commits through the certificate's returned evidence places a token boundary there. This quantifier is
strictly stronger than its complete-repair special case, and at a sound recovery point the inclusion is strict precisely
when no repair completes while some scan commits through the evidence. The evidence-returning interface pairs each
answer with the interval it rests on, so a caller who knows what the scanner cannot check decides whether the boundary
transfers to the intended input. The search provably terminates; the guarantee is relative to the active mode's
automaton. For an automaton of `n` states the quantifier is decided exactly from the preserved tail by at most `n + 1`
representative repairs, and a shortest reaching repair has at most `n - 1` symbols and a shortest counterrepair at most
`n`, both tight. Every anchor of an `m`-byte input is profiled in `O(nm + n|Σ|)` time. The shipped repair routines are
the complete-repair case, which proves their contracts. In a deterministic corruption study of munch's procedure beside
skip-one and delimiter recovery, every certified answer whose evidence survives the damage passes its landing check on
every move, 40,885 of 40,885, while the 5,595 first answers resting on evidence the damage reached land 1,589 times,
reported, not asserted. Of the 16,808 trials labeled beyond repair, 13,522 resume on a suffix that tokenizes whole,
where both quantifiers are provably empty, and 3,286 stay undetermined, their damaged inputs not archived.

## 1 Introduction

Two companion papers certify positions in completely tokenizable input. A certified byte begins a token at every
occurrence in every completely tokenizable input, and a certified split window places a token origin at a fixed offset
into every occurrence ([arXiv:2608.03473](https://arxiv.org/abs/2608.03473),
[arXiv:2608.09761](https://arxiv.org/abs/2608.09761)). Error recovery lives where those hypotheses fail, in input that
is not completely tokenizable. The classical answer is panic mode, which works at two levels. The lexical form deletes
characters until a well-formed token appears, and the parser form discards input up to a designated synchronizing
delimiter such as a newline or a semicolon. Both are old and widespread. They guarantee progress and, where a
synchronizer is found, membership in the designated set, but neither supplies a repair-universal boundary guarantee for
the position it lands on. This study moves the delimiter convention down a level and evaluates it, in both placements,
beside the lexical form. This paper supplies the theorem the convention never had. It shows that the same static
certificates that plan parallel chunk boundaries in valid input certify resynchronization points in broken input, with a
guarantee quantified over every prefix repair whose scan commits through the certificate's preserved evidence.

Three naive statements fail, and their failures shape the definition. First, demanding that the resumed suffix tokenize
on its own is reasonable but insufficient. Over `{ab, b}` the suffix `b` tokenizes alone, yet the repair `a` produces
the whole `ab`, one token with no boundary at the cut, so standalone suffix tokenizability filters positions without
aligning any of them with a repaired whole. Second, a certified window with positive origin spans bytes before the
position it certifies, so the resumed suffix alone does not contain the occurrence the certificate speaks about; a
statement quantifying only over the suffix cannot use the window theorem. Third, the prefix guarantee of the
split-points paper speaks about the tokens committed before a failure, never about how to continue past it. The
definition that survives all three is repair invariance: a position is a sound recovery point when every repair of the
broken prefix whose scan commits through the certificate's evidence places a committed token boundary there, with
complete tokenizability as the corollary case. It quantifies over repaired inputs, which defeats the first failure; the
theorems' conditions on the repair anchor, the left edge of the text a repair keeps, pin the certificate's evidence
inside the unmodified region, which defeats the second; and it is a statement about continuation past the failure, not
about the tokens committed before it, as the third failure requires. The resynchronization results are one-line
consequences of the posted certificates, and intentionally so. Nothing new needs to be proved about the automaton to
state them, only the right question asked of theorems that already exist. The contribution is the recovery contract, its
operational validation, and an exact decision procedure for the quantifier the contract introduces. Certified bytes are
repair-invariant at their own position. Certified windows are repair-invariant at their occurrence plus the certified
origin under one extra constraint on the repair, that it leaves the occurrence intact. The searching algorithm satisfies
this constraint by construction because it only reports evidence lying wholly at or after its own search anchor. Whether
the text past that anchor is truly unmodified is the caller's trust decision, and the evaluation prices it. A progress
lemma makes the scan-and-recover loop terminate, and mode-driven scanning gets a per-automaton certificate, relative to
the active mode, never a theorem over repairs that change modes. The procedure ships in the munch lexing library
(release v1.6.0), and the evaluation measures what the theorem is worth against the conventions it replaces, in quality.

The contributions are:

- Repair-invariant resynchronization, a definition of recovery-point soundness that quantifies over every prefix repair
  whose scan commits through the certificate's evidence, complete repairs the special case, with the three naive
  alternatives shown to fail (Section 3).
- Two theorems, one per certificate kind, each a short consequence of the companions' posted results, plus the progress
  lemma and the mode-scope remark that state a driver's obligations (Section 3).
- An exact decision procedure for the evidence-reaching quantifier, at most `n + 1` representative repairs deciding
  inhabitation and invariance at every evidence endpoint. A shortest reaching repair has at most `n - 1` symbols, and a
  shortest counterrepair at most `n`, or `n - 1` for a position past the anchor, every bound tight. A single threshold
  governs nonvacuous invariance, two repairs settle an endpoint and three settle every endpoint, and every anchor of an
  input is profiled in `O(nm + n|Σ|)` time and `O(n + m)` space. The shipped repair routines are the complete-repair
  case, which proves their contracts (Section 4).
- The searching primitive and its driver as shipped, with the one divergence from the planners' certificate policy
  explained, and the fail-closed fixtures that pin each layer (Sections 5 and 6).
- A deterministic corruption study whose oracle is exact for the theorem-predicted boundary consequence on the exhibited
  pristine repair. The pristine corpus supplies the repair the theorems quantify over, so the landing check runs on
  every returned answer whose supporting evidence the damage spares. The library's anchored complete-repair decider,
  whose contract Section 4 proves, is evaluated under that contract, in blind and oracle-anchored forms, beside the walk
  as a comparator for a different property. Repairability stratifies every trial, and the classical skip-one and
  skip-to-delimiter conventions are measured in the same campaign (Section 7).

The paper separates three questions a damaged scan raises: whether any repair completes the input, whether a returned
position is a boundary under every repair whose scan commits through its evidence, and how often, under measured damage,
that evidence survives. Certificates from the companion papers identify positions, and the resynchronization theorems
transfer their guarantee to every evidence-reaching repair of a failed scan's prefix. The collapse proposition and its
dichotomy corollary locate exactly where the strengthened quantifier adds content, and Section 4 decides from the
preserved tail which side of that line any anchor falls on. The overhang lemma tells a caller when the evidence is
forced by arithmetic alone to be *covered*, beginning at or past the end of the damage, and the campaign measures
availability, coverage, and displacement where the theorems' premises hold and where they fail.

## 2 Preliminaries

The model is the companions', restated and not reproved. A token set compiles to a deterministic finite automaton
scanned by the usual longest-match loop. From an offset the scanner consumes bytes while transitions are defined,
remembers the last accepting position, emits the token found there, and restarts at that position in the initial state.
Emitted tokens have positive length. The committed position strictly advances. An accepting initial state (a nullable
pattern's minimization) never emits at zero width, and a scan that cannot advance fails. Write `con(x)` for the
scanner's final committed offset on input `x`, and call `x` completely tokenizable when `con(x) = |x|`. The *failure
offset* of a failed scan is its final committed offset `f = con(x)`. It is the start of the token attempt that failed,
not the read head where the transition died, which lies at or past `f` and equals it exactly when the very first byte of
the attempt has no transition. For a scan resumed at offset `s`, the failure offset is the absolute
`f_s = s + con(x[s..])`, the driver's current committed offset. The shipped driver always searches from one past the
failure offset. The evaluation's oracle-aided arm and the shipped clean-anchored interface name another anchor, and they
do so by calling the primitive directly, never through the failure-anchored driver.

A byte is *formally certified* when the split-points paper's static condition holds of it. That paper's characterization
theorem makes the condition exact, in that a byte is certified if and only if every occurrence of it, in every
completely tokenizable input, begins a token. The characterization carries that paper's standing assumption that the
initial state is itself live. The condition is vacuously true of a byte no completely tokenizable input contains at all,
and the library reports only the *useful* certified bytes, those with a live initial-state transition, which is the
class the split-points paper reports. Throughout this paper *certified byte* means the reported, useful kind, narrower
than the companions' usage, in which a certified byte may be vacuous. The walk, the worked example, the refusal claims,
and the evaluation all speak of what the search reports, and the vacuous case is named where it matters.

A *certified window* `(W, o)` is a nonempty byte string with a certified origin `o`, `0 <= o < |W|`: in every completely
tokenizable input containing an occurrence of `W`, the token covering the occurrence's final byte begins exactly `o`
bytes into the occurrence. Unlike a certified byte here, a certified window may be vacuous, as in the window paper. A
certificate's *evidence* is the occurrence it rests on, the byte itself for a certified byte and the whole window
occurrence for a certified window. The certificate is defined for every window length, while the shipped recovery walk
searches lengths two to four, a cap whose consequences Section 5 takes up. The window results inherit the scope of the
cited version of the window paper, flat token sets with non-nullable patterns, which its later versions extend to
nullable sets through a positive-width equivalent; the byte results carry no such restriction. Everything in this paper
is relative to one fixed automaton, and mode-driven scanning returns in Section 3. Both certificates are static,
computed from the automaton alone before any input exists, which lets a recovery built on them give guarantees about
inputs it has never scanned.

## 3 Repair-invariant resynchronization

Let `Σ` be the byte alphabet throughout.

**Definition 1 (local repair).** For input `x` and anchor `c` with `0 <= c <= |x|`, a *repair before `c`* is any string
`r` in `Σ*`, giving the repaired input `y = r · x[c..]`. A repair is *tokenizable* when `y` is completely tokenizable.

Nothing at or after the anchor changes; everything before it may, by replacement as well as insertion or deletion, so
the results below quantify over every prefix-local fix at once. The anchor is only the left edge of the text a repair
keeps, chosen by the caller or the driver, and it need not be certified, unlike a certified anchor elsewhere in this
series, which is a position some certificate makes a boundary.

Complete tokenizability to the end of input is the natural quantification for a whole file in hand, but it is stronger
than the certificates need, and the stronger form buys vacuity, since a tail whose far end no repair can save would
escape every claim below even where the near end is perfectly ordinary. The scan supplies the weaker, sharper currency.
For any input `w`, write `con(w)` for the scan's *committed length*: the sum of the emitted token lengths when maximal
munch halts, equal to `|w|` exactly when `w` is completely tokenizable.

**Lemma 1 (committed prefix).** For any input `w`, the committed prefix `z = w[0..con(w))` is completely tokenizable,
and its scan emits exactly the tokens the scan of `w` commits.

*Proof.* By induction over the committed tokens. Each commit is the last accepting position on the viable run from its
token's start, and the scan of `w` read past it only through nonaccepting states before rolling back, so truncating `w`
at the final commit removes no accepting candidate. At every committed start, the scan of `z` sees the same last accept
and commits the same token, and the final commit lands exactly at `z`'s end. ∎

The lemma is the bridge the certificates cross, because whatever a scan commits is itself a completely tokenizable input
with the identical segmentation, so any theorem quantifying over completely tokenizable inputs applies to the committed
prefix of every scan, finished or failed.

**Definition 2 (evidence-reaching repair).** For evidence occupying `x[q..q+w)` with `c <= q`, `0 < w`, and
`q + w <= |x|`, a repair `r` before `c` is *evidence-reaching* when the scan of `y = r · x[c..]` commits through the
evidence's image: `con(y) >= |r| + (q + w - c)`.

Every tokenizable repair is evidence-reaching for every choice of evidence, since `con(y) = |y|`; the reverse fails, and
that gap is the point. A repair whose scan dies far past the evidence still counts, so the quantification below binds
real, failing inputs as well as the ideal ones that tokenize to the end.

**Definition 3 (repair-invariant resynchronization point).** Position `p` with `c <= p < |x|` is a *repair-invariant
resynchronization point* for `x` anchored at `c`, relative to evidence occupying `x[q..q+w)`, if in every
evidence-reaching repair `y = r · x[c..]`, a token of the scan's committed segmentation begins at `|r| + (p - c)`.

**Theorem 1 (certified bytes resynchronize).** If `x[p]` is a certified byte and `c <= p`, then `p` is a
repair-invariant resynchronization point for `x` anchored at `c`, relative to the evidence `x[p..p+1)`, which means that
in every repair whose scan commits through the byte's image, a committed token begins at `|r| + (p - c)`.

*Proof.* Every repair anchored at or before `p` preserves the occurrence of the certified byte at `p`'s image. When the
scan of `y` commits through that image, the committed prefix `z` contains it, `z` is completely tokenizable with the
scan's own segmentation by Lemma 1, and the byte certificate places a token origin at every occurrence in every
completely tokenizable input, the characterization theorem of the split-points paper (Theorem 2 of
[arXiv:2608.03473](https://arxiv.org/abs/2608.03473), arXiv v2), so a token of `z`'s segmentation, which is the
committed segmentation of `y`, begins at the image. ∎

**Theorem 2 (certified windows resynchronize at their origin).** Let the token set be flat with non-nullable patterns,
the scope of the cited version of the window paper. If `x` carries an occurrence of a certified window `(W, o)` at
position `q`, then `p = q + o` is a repair-invariant resynchronization point for `x` anchored at any `c <= q`, relative
to the evidence `x[q..q+|W|)`, which means that in every repair whose scan commits through the occurrence's image, a
committed token begins at `|r| + (p - c)`.

*Proof.* Every repair anchored at or before `q` preserves the occurrence whole at `q`'s image. When the scan of `y`
commits through the occurrence's image, the committed prefix `z` contains it whole and is completely tokenizable with
the scan's own segmentation by Lemma 1, and the window certificate places the origin of the token covering the
occurrence's final byte at its image plus `o` in every completely tokenizable input carrying the occurrence, the
certified-window contract and its soundness theorem (Definition 1 and Theorem 1 of
[arXiv:2608.09761](https://arxiv.org/abs/2608.09761), arXiv v1). ∎

**Corollary 1 (complete repairs).** In every tokenizable repair `y = r · x[c..]`, a token of `y`'s segmentation begins
at `|r| + (p - c)`, for `p` and `c` as in either theorem.

*Proof.* A tokenizable repair commits its whole input, `con(y) = |y|`, so it is evidence-reaching for any evidence, and
its committed segmentation is its segmentation. ∎

The corollary is the weaker claim. For an anchor `c` and evidence `x[q..q+w)`, write
`R_complete(x, c) = { r in Σ* : con(r · x[c..]) = |r| + |x| - c }` and
`R_reach(x, c; q, w) = { r in Σ* : con(r · x[c..]) >= |r| + q + w - c }`, where the parameters are fixed per instance
and dropped hereafter and `w` is the evidence width throughout. `R_reach` is the set of evidence-reaching repairs at an
anchor and `R_complete` the set of completely tokenizable ones. The inclusion `R_complete ⊆ R_reach` holds always, the
corollary binds only `R_complete`, and the theorems bind all of `R_reach`. The inclusion can be strict, and the example
is small. Over `{ab, ;}` on the damaged text `ab;#`, anchored at zero on the certified semicolon's evidence `[2, 3)`, no
repair completes the tail, the byte `#` being tokenizable in no context, so `R_complete` is empty and the corollary
holds vacuously. Yet the identity repair commits `ab` and `;` through the evidence, so `R_reach` is inhabited and the
theorem guarantees a committed interior boundary at the image of position two in every reaching repair.

The anchor constraint is `c <= q`, not `c <= p`, since a repair may not touch the window's own bytes, which lie partly
before the resynchronization point whenever the origin is positive; at origin zero the two constraints coincide. The
searching algorithm satisfies the constraint by construction, since it only reports occurrences lying wholly in the
unmodified suffix.

**Lemma 2 (progress).** A driver that alternates scanning with recovery, stopping at the input's end or when the
recovery search refuses, terminates after at most `|x|` advancing iterations.

*Proof.* Every emitted token has positive length, and the recovery search begins one past the failure offset, so every
answer lies strictly past the position the scan held; each iteration that does not stop strictly increases the driver's
offset, which is bounded by `|x|`. Refusal and the input's end stop the driver by the loop's own condition. ∎

One-past is not pedantry, because a certified byte can sit at the failure offset. Over `{ab, ;}` the byte `a` is
certified, occurring only token-initially. On the input `";a;ab"` the scanner commits the first semicolon, reads `a`,
dies on the following semicolon with no accepting position, and halts with failure offset 1, exactly the certified
occurrence; the failed transition itself happens one byte later. A search that began at the failure offset would answer
that same offset forever; beginning one past it, the search answers the second semicolon and the remainder tokenizes.

**Remark 1 (mode scope).** Under mode-driven scanning, an answer is a certificate relative to the active mode's
automaton only, since the certificates quantify over one automaton's completely tokenizable inputs, and a repair, or the
resumed text itself between the failure and the answer, may reach the resume position in a different mode, whose
automaton certifies different positions. The library consults the active mode and documents the answer as mode-relative;
nothing here upgrades the flat theorems to a guarantee over modal repairs.

One more consequence connects invariance to the vacuity question. Call `x` *repairable at `c`* when some tokenizable
repair anchored at `c` exists.

**Corollary 2 (recovery succeeds where repair exists).** If `x` is repairable at `c` and `p` is a repair-invariant
resynchronization point for `x` anchored at `c` relative to some evidence `x[q..q+w)` with `c <= q`, then `x[p..]` is
completely tokenizable.

*Proof.* Take any tokenizable repair `y = r · x[c..]`; it commits its whole input, so it is evidence-reaching, a token
of `y` begins at `|r| + (p - c)`, and the tokens of `y` from that boundary on are a complete tokenization of `y`'s
suffix there, which is `x[p..]` itself. ∎

So where a complete repair exists, a repair-invariant answer begins a resumed scan that tokenizes everything that
remains. Nonvacuity alone does not buy this, because an evidence-reaching repair can commit through the evidence and
still fail later, so the complete-tokenizability conclusion needs the complete-repair premise, as stated. The converse
direction closes the loop the evaluation needs, because it gives a necessary condition for the strengthened quantifier
to outrun the corollary's at a given anchor, namely that the answer's own resumed suffix must fail to tokenize
completely.

**Proposition 1 (reaching collapses to completing at a surviving answer).** Let `p` be a repair-invariant
resynchronization point for `y` anchored at `c`, relative to evidence `y[q..q+w)` with `c <= q`, and suppose `y[p..]` is
completely tokenizable. Then every evidence-reaching repair anchored at `c` is a tokenizable repair, and since every
tokenizable repair is evidence-reaching, the two quantifiers coincide at `c`.

*Proof.* Both inclusions. Take an evidence-reaching repair `r`. By the invariance premise, a token of the committed
segmentation of `r · y[c..]` begins at `|r| + (p - c)`, and past a committed boundary the scan restarts in the initial
state and reads exactly the image of `y[p..]`, which is completely tokenizable by hypothesis, so the repaired scan
commits everything and `r` is tokenizable. Conversely a tokenizable repair commits its whole input, so it commits
through the evidence's image and is evidence-reaching, the observation already made after Definition 2. ∎

The proposition is a fence against over-reading the strengthening. At an anchor whose answer's resumed scan completes,
the new semantics binds exactly the repairs the old corollary already bound, and any vacuity of the complete-repair form
at that anchor is vacuity of the evidence-reaching form too. The proposition's premises are the walk's own
postcondition, that a certified answer is repair-invariant relative to its reported evidence, and that evidence lies at
or after the search anchor by construction. The proposition therefore applies to every certified answer whose resumed
suffix tokenizes completely in one attempt. The strengthening's strictly additional content can live only where resumed
scans fail downstream, and Section 7 reports how many of the campaign's answers remain candidates for it, while
one-attempt complete resumption decides the rest.

**Corollary 3 (inhabited complete repairs force the collapse).** Let `p` be repair-invariant for `x` anchored at `c`
relative to evidence `x[q..q+w)` with `c <= q`. If `R_complete(x, c) ≠ ∅`, then `x[p..]` is completely tokenizable and
`R_complete = R_reach`. Equivalently, `R_complete ⊊ R_reach` if and only if `R_complete = ∅` and `R_reach ≠ ∅`.

*Proof.* Take a complete repair. It is evidence-reaching, so the invariance premise on `p` places a committed boundary
at `p`'s image; the repair tokenizes its whole input, so the suffix from that boundary, which is `x[p..]`, tokenizes
completely, and Proposition 1 turns every evidence-reaching repair into a complete one. To prove the equivalence,
suppose the inclusion is strict. The two sets differ, so the first part forces `R_complete = ∅`, and `R_reach` properly
contains it and is inhabited. Conversely, an empty `R_complete` inside an inhabited `R_reach` is strict by definition. ∎

So the strengthening can add content in exactly one regime. Whether a given candidate answer lies in that regime turns
on `R_reach`'s inhabitation, which Section 4 decides from the preserved tail and which the campaign's archive, carrying
no tails, does not.

### Non-claims

Four fences, each deliberate. A repair that modifies the suffix at or after the anchor falls outside the repair relation
proved here, since the definition quantifies over prefix repairs only and, for the window theorem, the anchor must
additionally sit at or before the occurrence. The theorems say nothing about such an edit either way, though one that
happens to preserve the evidence still faces the occurrence-universal certificate. The certificate alone does not decide
whether an evidence-reaching repair exists, and Section 4 decides it from the preserved tail. Where no scan of any
repair commits through the evidence, the guarantee holds vacuously. That boundary is strictly tighter than one drawn at
complete tokenizability, and the residual asymmetry between the certificate kinds narrows with it. A byte answer's
evidence is the answer itself, so whenever the damaged suffix admits a first commit at all, that commit witnesses
content for the anchor placed at the answer. A window with positive origin instead rests on evidence beginning before
the resume position, which no scan anchored at the answer reads, so its guarantee can bind repairs the resumed scan
never exhibits. The answer is the first certificate in evidence order, not the smallest answerable position, since a
window met earlier can answer a byte or two past one met later. Finally, on a suffix no repair can save to the end of
input, the theorems still bind every repair whose scan commits through the evidence, but the driver guarantees
termination and nothing else, each resume strictly advancing until the input ends or the search refuses.

## 4 Deciding the quantifier

The theorems above bind every evidence-reaching repair and say nothing about whether one exists. This section decides
that question, and with it invariance at every endpoint, from the preserved tail alone, at the cost of a finite family
of repairs read off the automaton, at most one more than its state count. The family also yields a shortest reaching
repair, a shortest repair refuting any proposed position, the single threshold that governs nonvacuous invariance, and
the exact relation to complete repairs. The shipped repair routines turn out to be the family restricted to complete
repairs, which proves their documented contracts and with them the negative labels the evaluation consumes.

### The tail, the repairs, and what a repair shows

Translate the anchor to zero. The preserved tail is `t = x[c..]`, of length `m > 0`, and a repair `r` gives the input
`rt`, whose *seam* is the offset `|r|` where the tail begins. The automaton has `n >= 1` states, initial state `q_0`, a
partial transition function `δ` over `Σ`, and accepting states carrying their token kinds. The count `n` is of the
automaton as given, an accepting initial state allowed, at which the scan emits no empty token. For a repair `r` write
`e(r) = con(rt) - |r|` and `B(r) = { p in [0, m) : a committed token of rt begins at |r| + p }`, the relative terminal
and the in-tail committed starts. The start of a failed token attempt is not a committed start, and neither is the end
of input, so `e(r)` is not in `B(r)`. Evidence `x[q..q+w)` at anchor `c` has the endpoint `h = q + w - c`, with
`1 <= h <= m`, and the two sets of Section 3 read `R_reach = { r : e(r) >= h }` and `R_complete = { r : e(r) = m }`.
Only the endpoint enters, since where the evidence begins does not change which repairs reach it. A position `p` in
`[0, m)` is *invariant at `h`* when `p` is in `B(r)` for every `r` in `R_reach`, vacuously when `R_reach` is empty. The
definition admits `p >= h` as well as positions inside the evidence.

### Finitely many repairs decide everything

Call a state *positively reachable* when some nonempty word drives `q_0` to it. For each such state `q` fix a shortest
nonempty word `u_q`, which a breadth-first search seeded with `q_0`'s outgoing transitions, `q_0` itself unvisited,
finds for every state at once. The initial state is a candidate like any other. When a nonempty word returns to it,
`u_{q_0}` is a shortest positive return, and the empty word does not stand in for it. Continue the run from `q` over the
whole of `t`, stopping where a transition is missing, and let `d_q` in `[0, m]` be its last accepting offset, offset
zero counting when `q` itself accepts. A state whose run never accepts contributes nothing.

**Definition 4 (representative repairs).** The representatives of `t` are the empty repair and, for every positively
reachable `q` whose run over `t` accepts, the repair `u_q`, whose first committed token ends at tail offset `d_q`. There
are at most `n + 1` of them.

The name is justified by what each representative does. The first token of `u_q t` ends at `|u_q| + d_q`, because the
run's acceptance at `d_q` comes after every acceptance inside `u_q` and by definition none comes later; the scan then
restarts in `q_0` at tail offset `d_q`. When `d_q = 0`, the state `q` accepts and nothing later on its run does, so the
first token ends exactly at the seam and the rest is the fresh scan of `t`, the empty repair's own behavior. Such a
representative is an actual repair that adds nothing, kept for uniformity, and may be dropped. For any repair whose scan
restarts at tail offset `d`, the rest is the fresh scan of `t[d..]`, so `e` and `B` are read off that scan.

**Theorem 3 (finite exactness).** Every repair `r` with `e(r) >= 1` has the same relative terminal `e` and the same
in-tail committed starts `B` as some representative of length at most `|r|`. Hence for every endpoint `h` the
representatives decide whether `R_reach` is inhabited and which positions are invariant. They also contain a shortest
reaching repair and a shortest repair refuting any position that is not invariant.

*Proof.* The scan of `rt` commits through the first tail byte, so one committed token covers it. Let it begin at `s` and
write `a = |r|` for the seam. If `s = a`, the scan stands in `q_0` at the seam and everything it commits from there is
the fresh scan of `t`, so the empty repair has the same `e` and `B` and length zero. Otherwise `s < a`. Put
`u = r[s..a)`, nonempty, and `q = δ*(q_0, u)`, defined because `u` is a prefix of a committed token. The token ends at
`a + d` with `d > 0`, since it covers the first tail byte. By longest match, `d` is the last accepting offset of the run
from `q` over `t`, because a later acceptance before the run died would have ended the token there. Hence `q` is
positively reachable, its run accepts, and `d_q = d`. Now scan `u_q t`. The run from `q` over `t` is the same, its
acceptance at `d > 0` comes after every acceptance inside `u_q`, and the first token ends at `|u_q| + d` with the same
kind. Both scans then restart in `q_0` on `t[d..]` and commit the same tokens to the same terminal, while the tokens of
`rt` before the crossing token all begin before the seam and are not in `B(r)`. Hence `e(u_q) = e(r)` and
`B(u_q) = B(r)`, with `|u_q| <= |u| <= |r|`. The decisions follow. A reaching repair exists exactly when a
representative reaches, and a position is invariant exactly when every reaching representative commits a token there. A
shortest witness of either kind can be taken among the representatives, since every witness maps to one that is no
longer and every representative is itself a repair. ∎

The theorem preserves the kinds and lengths of the tokens that begin in the tail, but not the crossing token's length or
where it began, which change with the repaired prefix. Leaving out a state whose run over `t` never accepts does not say
that `u_q t` fails to reach, since its scan may commit inside `u_q` and cross the seam in another state, which then
represents it.

**Theorem 4 (sharp witness bounds).** If `R_reach` is inhabited, it contains a repair of length at most `n - 1`. If a
position is not invariant, some reaching repair of length at most `n` refutes it, and one of length at most `n - 1` when
the position is not `0`. All three bounds are attained for every `n >= 1`, already over two symbols.

*Proof.* A shortest nonempty word to a state other than `q_0` repeats no state, so `|u_q| <= n - 1`. A shortest positive
return to `q_0` is a simple cycle, so `|u_{q_0}| <= n`. With Theorem 3 this gives the counterrepair bound. For reaching
alone, the return to `q_0` is not needed. If `d_{q_0} > 0`, then `d_{q_0}` is the last accepting offset of the run from
`q_0` over `t`, which is where the empty repair's first token ends, so the empty repair restarts at the same offset and
has the same terminal. If `d_{q_0} = 0`, the representative's scan is the fresh scan of `t`, as noted above. An
accepting `q_0` changes nothing here, since its acceptance at offset zero is where the token of `u_{q_0}` ends, while
the empty repair emits no empty token there, and the terminals still agree. A shortest reaching repair is the empty one
or some `u_q` with `q != q_0`, of length at most `n - 1`. The same comparison serves a position `p > 0`, where by
Theorem 3 a shortest refuter is again a representative and only `u_{q_0}` can have length `n`. The empty repair and
`u_{q_0}` restart at the same offset and commit the same starts from there on, and the only start they can differ in is
`0`, where the empty repair's first token begins when `d_{q_0} > 0`. A reaching `u_{q_0}` that refutes `p > 0` is
matched by the empty repair, which reaches and refutes `p` as well, so a shortest refuter of such a position has length
at most `n - 1`.

Tightness. For `n = 1`, the reaching bound is zero and the empty repair attains it wherever anything reaches. For the
counterrepair bound at `n = 1`, take one accepting state with a loop on `a`. It recognizes `a*`, and since the scan
emits no empty token its tokens are `a+`. Take the tail `a` with `p = 0`. The empty repair emits the tail as a token
beginning at zero, while the repair `a` makes `aa` one token, so no token begins at the seam, and no shorter nonempty
repair exists. For `n >= 2`, take the chain `q_0 -a-> q_1 -a-> ... -a-> q_{n-1}` with a loop on `b` at `q_{n-1}`, which
alone accepts, and the tail `b`. The repair `a^(n-1)` reaches, and any token covering the tail's `b` reads it from
`q_{n-1}`, the only state with a `b` transition, so the part of the repair inside that token has at least `n - 1`
symbols. For the counterrepair bound, take the cycle `q_i -a-> q_((i+1) mod n)`, the one extra transition
`q_0 -b-> q_1`, and `q_1` alone accepting, with the tail `b` and `p = 0`. The empty repair emits `b` at zero. A refuting
repair needs a crossing token that reads the tail's `b`, which only `q_0` can, so the part of the repair inside that
token is a positive return to `q_0`, of length at least `n`; the repair `a^n` makes `a^n b` one token and attains it.
For the bound at a position `p > 0`, take the chain family, at `n = 1` the single accepting state with a loop on `b`,
with the tail `bb` and `p = 1`. At `n = 1` the empty repair emits `bb` as one token and refutes `1` at length zero. For
`n >= 2`, as for the tail `b`, a reaching repair has a crossing token that reads the tail from `q_{n-1}`, so the part of
the repair inside it has at least `n - 1` symbols, and that token absorbs both `b`s, so `a^(n-1)` refutes `1` and no
shorter repair does. Making `q_0` accepting in either family changes nothing, since no empty token is emitted, so the
bounds are attained with an accepting initial state as well, and `n` counts the automaton as given. ∎

### One threshold

For `i < m` let `J(i) > i` be the end of the first token a fresh scan of `t[i..]` commits, undefined when that attempt
fails, and let `m` be a node too. The edges `i -> J(i)` point toward larger offsets and form a forest whose roots are
the failed attempts and the end of input; let `ρ(i)` be the root reached from `i`. A repair restarting at `d` has
`e = ρ(d)`, and its `B` is the path from `d` to `ρ(d)` with the root removed.

**Lemma 3 (common starts).** If two repairs share a committed start `p` in the tail, they have the same terminal and the
same committed starts from `p` on.

*Proof.* At a committed start the scanner is in `q_0` and reads `t[p..]`, the same fixed suffix for both, so it commits
the same tokens to the same terminal. In the forest, both continue along the one path from `p` to its root. ∎

Hence repairs with different terminals have disjoint start sets. Let `E` be the largest terminal of a representative,
and let `F` be the largest terminal below `E`, or `0` when no positive one exists. The representatives with terminal `E`
lie in one tree; let `g` be the lowest common ancestor of their entries, and let `C` be the path from `g` to `E` with
`E` removed, empty when `g = E`.

**Theorem 5 (the evidence profile).** Let `1 <= h <= m`. If `h > E`, no repair reaches and every position is vacuously
invariant. If `h <= F`, repairs reach and no position is invariant. If `F < h <= E`, repairs reach and the invariant
positions are exactly `C`.

*Proof.* The first case is Theorem 3 and the maximality of `E`. In the second, a representative with terminal `E` and
one with terminal `F` both reach `h`, and their start sets are disjoint by Lemma 3. In the third, the reaching
representatives are exactly those with terminal `E`, since no terminal lies strictly between `F` and `E`. By Theorem 3
the invariant positions are the common starts of these representatives, the intersection of their paths to `E`, which is
the path from their lowest common ancestor `g` to `E`; removing `E` removes the one node on it that is not a committed
start. ∎

Among the endpoints some repair reaches, the invariant set changes at most once, at `F + 1`. The reaching family may
change several times below `F`, but its common starts stay empty there, and every position that is ever nonvacuously
invariant becomes so at `F + 1`, while positions outside `C` never do. If the evidence must also contain a position `p`
of `C`, as a byte's or a window's evidence does, its least nonvacuous endpoint is `max(F + 1, p + 1)`.

**Corollary 4 (reaching against completing).** If `E = m`, then `R_reach = R_complete` exactly when `h > F`. If `E < m`,
then `R_complete = ∅`, and `R_reach = R_complete` exactly when `h > E`. Proposition 1 and Corollary 3 follow.

*Proof.* A repair completes exactly when its terminal is `m`. If `E = m`, some representative completes, and `R_reach`
exceeds `R_complete` exactly when some reaching repair stops short of `m`. By Theorem 3 that is exactly when a
representative has terminal in `[h, m)`, that is, `h <= F`. If `E < m`, no repair completes, so the two sets are equal
exactly when `R_reach` is empty, which is `h > E`. For the proposition, let `p` be invariant and let `t[p..]` tokenize
completely. If `h > E`, both sets are empty. Otherwise invariance is not vacuous, so `F < h <= E` and `p` is in `C`,
whence `ρ(p) = E`. The complete fresh scan from `p` gives `ρ(p) = m`, so `E = m`, and `h > F` gives
`R_reach = R_complete`. For the dichotomy, let `p` be invariant. If `R_complete` is inhabited, a complete repair
reaches, so `E = m >= h` and invariance is not vacuous. Then `F < h <= E` and `p` is in `C`, whence `ρ(p) = m`, so the
suffix `t[p..]` tokenizes completely, and `R_reach = R_complete` by the first part. For the equivalence, if `h > E`,
both sets are empty, so they are equal and `R_reach` is not inhabited. If `h <= E`, then `F < h` as before, and the
inclusion is strict exactly when `E < m`, which is exactly `R_complete = ∅` with `R_reach ≠ ∅`. ∎

Equal terminals alone force no agreement. Over `{a, ba}` on the tail `a`, both the empty repair and `b` complete, the
first emitting `a` at zero and the second absorbing the tail into `ba`. Here `E = m`, `F = 0`, and `C` is empty, as
Table 2 records. The same table profiles two other examples from the paper. On `ab;#` over `{ab, ;}` the only reaching
behavior is the empty repair's, with `E = 3 < m`, so positions 0 and 2 are invariant at every endpoint up to 3, the
strict inclusion of Section 3 at all three. On `ab` over `{ab, ba}` the repair `b` ends at `F = 1`, so the one-byte
evidence has no invariant position while the whole tail has position 0, the separation Section 7 draws between the
decider and the certificates.

### Two repairs, then three

**Theorem 6 (a finite consensus basis).** At every endpoint with `R_reach` inhabited, one or two reaching repairs have
exactly the common starts of all of `R_reach`. One fixed set of at most three repairs reproduces inhabitation and the
invariant set at every endpoint at once. Every repair chosen has length at most `n`, and both counts are tight.

*Proof.* If `h <= F`, a representative with terminal `E` and one with terminal `F` have disjoint starts, settling every
position. If `F < h <= E` and some representative enters at `g`, its path alone is `C`. Otherwise every entry with
terminal `E` lies strictly below `g`, and not all in one child subtree of `g`, since a child containing them all would
be a common ancestor below `g`. Two entries from two different children have lowest common ancestor `g`, so their paths
meet in exactly `C`. For the uniform set, keep those one or two representatives and add one with terminal `F` when
`F > 0`. Above `E` nothing reaches, between `F` and `E` only the kept maximal ones reach and give `C`, and at or below
`F` the added one empties the intersection while at least one repair reaches. Tightness is Table 1, where over
`{a, bb, bbb}` on the tail `bbba` there are exactly three reaching behaviors. A token crossing the seam reads the tail's
first `b` at a noninitial position, so by longest match it is `bbb` with one or two of its `b`s before the seam. It ends
at tail offset `2`, where the scan of `ba` fails, or at offset `1`, after which the scan emits `bb` and `a`. A token
ending or beginning at the seam gives the empty repair's behavior. At `h = 3` the invariant set is `{3}`, and neither
terminal-4 behavior has it alone, so two repairs are needed at that endpoint. At `h = 1` those two still meet in `{3}`
while the third empties the intersection, so no pair serves every endpoint. ∎

**Table 1.** The basis example, `{a, bb, bbb}` on the tail `bbba`. Above, the three reaching behaviors, one actual
repair each, with the committed tokens of the repaired input, the in-tail starts, and the terminal. The empty repair is
ε. Below, which behaviors reach each endpoint and the exact invariant set there. Two behaviors are needed at `h = 3` and
all three across the endpoints. Both bodies are written by the program of Section 4 and held to it by the test suite.

| Repair | Committed tokens of `rt` | `B(r)`  | `e(r)` |
| ------ | ------------------------ | ------- | -----: |
| ε      | `bbb a`                  | `{0,3}` |      4 |
| `bb`   | `bbb bb a`               | `{1,3}` |      4 |
| `b`    | `bbb`                    | ∅       |      2 |

| Endpoint      | Reaching behaviors | Invariant positions |
| ------------- | ------------------- | ------------------- |
| `1 <= h <= 2` | ε, bb, b            | ∅                   |
| `3 <= h <= 4` | ε, bb               | {3}                 |

**Table 2.** Evidence profiles of the examples in this paper: the tail length, the largest terminal `E`, the next one
`F`, the invariant positions `C` at every endpoint in `(F, E]`, and the repair basis of Theorem 6. The first row is the
strict-inclusion example of Section 3, the second the separation example of Section 7, the third the equal-terminals
example, and the last the basis example. Written by the program of Section 4.

| Tokens         | Tail   | `m` | `E` | `F` | `C`     | Basis    |
| -------------- | ------ | --: | --: | --: | ------- | -------- |
| `{ab, ;}`      | `ab;#` |   4 |   3 |   0 | `{0,2}` | ε        |
| `{ab, ba}`     | `ab`   |   2 |   2 |   1 | `{0}`   | ε, b     |
| `{a, ba}`      | `a`    |   1 |   1 |   0 | ∅       | b        |
| `{a, bb, bbb}` | `bbba` |   4 |   4 |   2 | `{3}`   | ε, bb, b |

### Cost

**Theorem 7 (one anchor).** For one tail, the evidence profile `(E, F, g, C)` with its basis of at most three repairs is
computed in `O(nm + n|Σ|)` time and `O(n + m)` words beyond the automaton and the tail. Afterwards, inhabitation and
invariance at any endpoint and position are `O(1)`. A shortest reaching repair, or a shortest counterrepair to a given
position, is selected in `O(n)` time and written out in time proportional to its length.

*Proof.* For `0 <= i <= m` and state `q` let `L_i(q)` be the last accepting offset of the run from `q` over `t[i..]`,
offset `i` allowed, undefined when the run never accepts. Then `L_m(q) = m` for accepting `q` and is undefined
otherwise. For `i < m`, `L_i(q)` is `L_{i+1}(δ(q, t[i]))` when that transition and value exist, else `i` when `q`
accepts, else undefined. A later acceptance exists exactly when the value after the next byte does, and then it
dominates. Two rows of `n` entries compute every row right to left in `nm` steps. Keep `J(i) = L_i(q_0)` where it
exceeds `i`, which excludes an empty token at an accepting `q_0`, and keep the row `L_0`, whose entries are the `d_q`.
Then `ρ` follows right to left in `O(m)`. The breadth-first search stores one parent transition per state, `O(n|Σ|)`
time and `O(n)` words, and no representative is written out until asked for. For `g`, put one unit at each entry with
terminal `E`, duplicates counted, and push the counts along `J` in increasing offset order. Every node below a node is
processed before it, so the first node carrying the full count is `g`, and the chain from `g` gives `C`. Carrying one
entry index with the counts names an entry at `g`, or one from each of two children of `g` with positive counts. That is
the basis, and when `F > 0` an entry with terminal `F` is added. Reversing `J` and walking each tree assigns every node
a preorder interval, so whether `p` lies on a representative's path is `p < ρ(d)` together with an interval test,
`O(1)`. The witness selections scan at most `n + 1` representatives with that test, and the parent transitions write out
the chosen word. Every array has `O(n + m)` entries. ∎

The jump table of `recovery.cpp` is built differently, by running the scanner afresh from every tail offset and reading
each run to its end, which is quadratic in the tail on a long token. On the two-state automaton of `a+` over a tail of
2,048 bytes the shipped construction takes 2,098,176 transitions where the recurrence updates 4,096 cells. The counts
cover the tabulation alone and give no timing for either routine.

**Theorem 8 (every anchor).** For one input of length `m`, the evidence profiles and bases of all its anchors are
computed in `O(nm + n|Σ|)` time and `O(n + m)` words. Afterwards, at every anchor, inhabitation, invariance, the
earliest nonvacuous invariant start, the least nonvacuous endpoint of a position, and equality of the two quantifiers
are `O(1)`.

*Proof.* Every anchor's tail ends at the same end of input, so the recurrence, `J` and `ρ` are shared. One right-to-left
pass builds them, and the breadth-first search is shared too. At anchor `c` the entries are `c` for the empty repair and
`L_c(q)` for each positively reachable `q` where defined, so a second right-to-left pass, recomputing the rows while
retaining only two at a time, has every anchor's entries in turn. Offsets are absolute from here on, with `E_c`, `F_c`
and `g_c` offsets into the input and `F_c` defaulting to `c`. The relative endpoint `h` corresponds to the absolute
endpoint `c + h`, where the evidence ends. Among the entries with terminal `E_c` keep only the earliest and the latest
in preorder. Their lowest common ancestor is that of the whole set. A subtree is a contiguous preorder interval, so the
subtree of their common ancestor contains every entry between them, and any common ancestor of all entries is an
ancestor of the two. One constant-time lowest-common-ancestor query then gives `g_c`. The two kept entries are the
basis, with an entry of terminal `F_c` added when `F_c > c`. The query comes from the classical static index, which
attaches the roots to one virtual root, records an Euler tour with its depths, and answers a query by the minimum depth
between two first visits. Adjacent depths differ by one, and the range-minimum index of Bender and Farach-Colton (LATIN
2000) takes `O(m)` preprocessing and answers in `O(1)`. The second pass does `O(n)` work per anchor and one query. The
pass keeps two rolling rows, the forest with its index, the parent transitions, and a constant-size evidence profile per
anchor. The queries read the profile: inhabitation is `c + h <= E_c`; invariance is `c + h > E_c`, the vacuous case, or
`F_c < c + h <= E_c` with `p` on the chain from `g_c`, which is `p < E_c` and an ancestor test; the earliest start is
`g_c` unless `g_c = E_c`; the least nonvacuous endpoint of a position of `C_c` is the absolute `F_c + 1`, or
`max(F_c + 1, p + 1)` when the evidence must contain `p`, and a position outside `C_c` has none; and equality of the
quantifiers is Corollary 4. ∎

The bound is for recovery decisions on a fixed input and automaton, in the word model with the transition table given,
and does not bound the scan itself; it says nothing about edits to the input, a change of automaton, or a change of
mode.

### The shipped routines are the complete case

For complete repairs, at `h = m`, the routines the evaluation runs, `minimal_repair` and `next_anchored_start`, use the
representatives of Definition 4. Their crossing entries are the positively reachable states with shortest words, found
by the same seeded breadth-first search, and each entry's boundary is `d_q`. Their jump table carries `J`, exact because
the start these routines use is nonaccepting, together with whether the fresh scan from each offset reaches `m`, which
is `ρ(i) = m`. `minimal_repair` returns the empty repair when `ρ(0) = m`, otherwise a shortest `u_q` with `ρ(d_q) = m`,
and refuses when there is none. `next_anchored_start` intersects the start sets of the representatives with terminal `m`
and returns the first member at or after the given offset, refusing when no representative completes. Both are members
of the simulator in `libs/dfa/src/simulator.cpp` at the campaign's release v1.6.0 and free functions over it in
`libs/dfa/src/recovery.cpp` since release v2.0.0, the same algorithm in both.

**Theorem 9 (negative-label exactness).** For every non-nullable token set, every tail `t`, and every anchor,
`minimal_repair(t) = nullopt` implies `R_complete = ∅`, and a returned repair is a shortest complete one. When some
complete repair exists, `next_anchored_start(t, s)` returns the least position at or after `s` at which every complete
repair commits a token, and refuses when none lies at or after `s`. It refuses when no complete repair exists. Since
release v2.0.0 the same holds for every token set.

*Proof.* On an empty tail `minimal_repair` returns the empty repair and `next_anchored_start` refuses, both correctly,
because the empty repair completes and there is no position. Let `m > 0` and take `h = m`, so that
`R_reach = R_complete`. By Theorem 3, a complete repair exists exactly when a representative has terminal `m`, and a
shortest complete repair is a shortest such representative. The positions every complete repair commits a token at are
the common starts of those representatives, and the routines compute exactly these. A refusal of `minimal_repair`
therefore certifies that no repair of any length completes, and the decider's answer is the least invariant position at
or after `s`. The campaign's release refused every nullable token set before computing anything, which is why the
statement is restricted to non-nullable sets, every campaign automaton among them. Since release v2.0.0 the library
compiles a nullable set with a fresh nonaccepting start, one state more. The compiled automaton's positively reachable
states, shortest words and `d_q` are those of the given automaton, so the representatives, and with them the theorem,
are the same for every token set. ∎

**Held to a program.** Every table and number of this section is written by `repair_profile.py` under
`paper/panic-mode/checks`, a program with no dependencies that runs in the test suite, where its output is compared byte
for byte against the committed files. The program builds the representatives, the forest, the evidence profile and the
basis, and checks every decision against brute force over 977 automaton and tail instances, 17,358 endpoint and position
queries and 256,088 scans. The brute-force check scans every repair up to a length bound with the program's own
maximal-munch scanner. The instances are random partial automata and random literal vocabularies, the two tight
families, the basis example, and the worked examples of Table 2. The families are checked at every state count up to 12
by brute force and up to 64 by the construction, each with and without an accepting initial state. The basis example is
checked to repair length 8, where the three behaviors of Table 1 are the only reaching ones. The shipped routines,
transcribed from `recovery.cpp`, are held to the `h = m` case on every instance, with an accepting start unrolled first,
as the library does. The test checks that transcription, and the C++ itself is checked by `munch_crosscheck_anchor`, the
recovery cross-check of Section 7. The bounded runs are evidence for the program, while the proofs carry the
quantifiers.

## 5 The search

The primitive is one forward walk (the listing below) that consults both certificate kinds at every position. A
certified byte answers at its own position, and a searched window answers at its occurrence plus the certified origin.
The walk returns the first certificate met in evidence order, which is the position order of the supporting evidence,
hence the first-in-evidence-order fence above. At one position the byte certificate is consulted first, then windows by
increasing length, two through four. One design choice departs from the companions' planners. The planners disable the
window search whenever byte certificates exist, because a planner prices whole-input cuts and byte certificates are
strictly cheaper there. A recovery wants an early sound point of either kind after a specific offset, so the walk keeps
both kinds live throughout. In the campaign's release a nullable token set contributes no windows, since the window
proof of the cited version excludes nullability, while its byte certificates, when any exist, stand and answer alone.
The library now compiles a nullable set with the fresh nonaccepting start of Section 4, so its windows are decided too,
and the walk consults them like any other. Window membership is memoized per distinct byte string as in the planner.
When no searched certificate exists at or after the offset there is no answer, and the evaluation counts that refusal.
Refusal is relative both to the searched lengths and to the window paper's conservative model. A shipped fixture shows
the first, carrying a five-byte certificate exactly one past the cap, where the walk refuses by design. The second
arises because the model can refuse a semantically certified window. Over `{a, ab, b}` every occurrence of `ab` begins a
token at origin zero, yet the shipped decider refuses it, so a refusal asserts only that no certificate of the searched
kinds is reported, never that no sound position exists.

**Corollary 5 (walk soundness).** If the search starting at offset `s` returns `p` on evidence occupying `x[q..q+w)`
with `q >= s`, then `p` is a repair-invariant resynchronization point for `x` anchored at any `c <= q`, hence at any
`c <= s`, relative to that evidence, which means that in every repair whose scan commits through the evidence's image, a
committed token begins at the answer's image.

*Proof.* By cases on the evidence. A certified byte answers at its own position, `q = p` with `w = 1`, and `c <= q = p`
is the byte theorem's anchor condition. A certified window answers at its occurrence plus the certified origin with the
occurrence at `q` and `w = |W|`, and `c <= q` is the window theorem's. The walk reports only evidence lying wholly at or
after `s`, so `c <= s` always suffices. ∎

The following worked answer is taken verbatim from the library's own pinned fixture. Over identifiers and whitespace
runs no byte usefully certifies (the at-sign, which no token contains, is formally certified only in the vacuous sense
and reported by nothing), so recovery rests on windows alone. On the input `"abc@@@def ghi"` the scan fails inside the
junk. In the forward search, the first certificate met is the four-byte window `"def "` at offset 6 with certified
origin 3, so the answer is 9, the whitespace byte that begins a token in every completely tokenizable input containing
the occurrence. The window's bytes at offsets 6 through 9 sit wholly in the unmodified suffix, which is the anchor
constraint of the window theorem, so a repair of everything before offset 6 is bound by the guarantee, and a repair that
rewrote `def` would not be. A recovery that returned the occurrence instead of occurrence plus origin would resume at 6,
a position the window does not certify and one that can lie mid-identifier in a tokenizable repair; the shipped fixture
kills that mutant.

**Figure 1.** The search and the canonical driver loop. The walk returns the first certificate in evidence order, the
byte at its own position before windows by increasing length at their occurrence plus origin, and refusal is explicit.
The search starts one past the failure offset, the progress lemma's premise, and resumption is at the returned position.
`next_certified_start()` implements the walk and `recover()` performs one recovery move, while the loop and its stop
policy belong to the caller, shown here as pseudocode.

```
search(x, s):                                  ▷ the forward walk, evidence order
  for p := s to |x| - 1:
    if certified_byte(x[p]):                   ▷ a byte answers at its own position
      return p
    for len := 2 to 4, while p + len <= |x|:   ▷ the shipped window lengths
      o := certified_window(x[p .. p + len))
      if o != none:
        return p + o                           ▷ occurrence plus certified origin
  return none                                  ▷ explicit refusal

driver(x):
  pos := 0
  while pos < |x|:
    scan from pos, emitting tokens
    if the scan committed everything: stop
    f := the failed scan's committed offset
    p := search(x, f + 1)                      ▷ the search starts one past f
    if p = none: stop                          ▷ refusal, never a guess
    pos := p                                   ▷ resume at the returned position
```

## 6 Implementation

The munch library (release v1.6.0) ships the procedure in two layers, and the split is load-bearing. The cited release
states the contract in its API documentation and in the repository's top-level readme. The primitive,
`Lexer::next_certified_start(input, from)`, is a position-only query on one automaton, documented with the
complete-repair corollary's contract, and it knows nothing about errors or drivers. Beside it, the evidence-returning
`next_certified_evidence(input, from)` carries the interval and kind behind each answer. The behavior,
`Tokenizer::recover()`, belongs to the driver. A failed `next()` does not advance the reading position, since guessing a
skip would invent tokens, and the driver then chooses among stopping, seeking by its own rule, or `recover()`, which
asks the active mode's lexer for the first certified start at or after the position one past the current one and moves
there, returning the skip count. When no certificate among the searched kinds and lengths lies ahead, the position does
not move and the refusal is explicit. Two sibling forms return the evidence itself: `recover_from_failure()` answers
with the certified start and its evidence interval, and `recover_from_clean(clean_from)` floors the search at a caller's
known-clean offset, the returned evidence covered by construction, the interface the trust discussion of Section 7
measures.

The fail-closed tests pin each half at its own layer. At the lexer, fixtures pin both certificate kinds answered in
evidence order, the nullable set answering through its certified byte with no window consulted, and the unbounded run
refusing. At the driver, a mutant that ignored the window origin would move to the uncertified occurrence, and a named
fixture kills it. The driver's refusal path and its exact skip counts are pinned, and a driver fixture holds recovery to
the active mode's automaton alone. The evaluation's harness adds the strongest check. On every trial, an answer whose
supporting evidence lies wholly in the preserved suffix is checked by an executable landing assertion against the repair
the pristine corpus supplies, and every such check in the campaign passed.

## 7 Evaluation

The campaign is `tools/probes/src/recovery_quality.cpp` of munch release `v1.6.0`, invoked as
`munch_recovery_quality 512 500 out.csv twitter.json 3`, with 512 KiB of corpus per generated row, 500 trials per cell
and three independent seeds. Every seed is fixed, so the experiment is deterministic. The full CSV, the probe's own
summary, the five generated corpora, two checked-in analysis programs that derive this section's CSV-borne figures from
the archive, and a pinned reproduction script with the artifact hashes are deposited as a standalone record at
doi:10.5281/zenodo.22178507. The CSV has twenty-eight columns: the trial's failure offset, corruption end, first mapped
boundary, repairability and minimal-repair length, the decider's direct answer at the blind anchor, then per arm the
first and terminal positions with landing flags, the evidence interval and kind, the terminal outcome, attempts, the
per-move covered and landed counts, and the convergence triple. The pristine-oracle pass and the within-cell repeat
count are the harness summary's own attestations, archived beside the CSV and its per-move file. Every campaign
statistic and mapped-oracle count this section reports recomputes from that deposit, the pinned munch source tree the
script checks out and verifies against the bundled harness snapshot, and the pinned simdjson corpus the script fetches
and holds to its recorded digest. The real-document row reads the simdjson benchmark corpus `twitter.json` verbatim
(631,515 bytes, byte-identical to `jsonexamples/twitter.json` at the simdjson repository's release v3.10.1, SHA-256
30721e49..., the full digest in the data notes and verified by the reproduction script), held to the same
complete-tokenizability assertion, the same damage protocol, and the same oracle as the generated rows.

Quality, not throughput. A pristine corpus `x`, completely tokenizable by its row's grammar and asserted so, is damaged
at a position by one of three operations at `k ∈ {1, 4, 16}`: substituting `k` bytes with pseudo-random bytes, deleting
`k` existing bytes, or inserting `k` pseudo-random bytes. Every operation leaves a suffix intact, in that the damaged
input `y` satisfies `y[e..] = x[c..]` for a corruption end `e` and pristine anchor `c` the operation designates. For the
damage start `d`, substitution sets `(e, c) = (d + k, d + k)`, deletion `(e, c) = (d, d + k)`, and insertion
`(e, c) = (d + k, d)`, so images shift by zero, `-k`, and `+k` respectively; `e` is the designated start of the suffix
the operation guarantees untouched, and a replacement byte that happens to equal the original never moves it earlier.
The oracle is the pristine boundary set mapped through that shift, exact for the theorem-predicted consequence on the
exhibited pristine repair, boundaries inside the damaged window having no image.

Six rows are measured: the conventional C-like grammar with strings and line comments, the same with block comments
alone, byte-level RFC 8259 JSON, the bare C-like row, whose operator and punctuation bytes all certify exactly, the
split-friendly variant, which adds a certified newline byte to live windows, and the JSON grammar over the real-world
document above. The JSON grammar assumes UTF-8 well-formedness without checking it, so a corruption-injected raw byte is
absorbed inside strings that valid interchange would exclude. Trials the grammar absorbs without a scan failure are
counted and set aside, 37,264 of the campaign's 81,000 damage draws, leaving 43,736 broken scans. Damage positions are
drawn by unbiased rejection sampling from the full span, clear of both corpus edges by at least sixty-four bytes,
independently per seed and per row, no two rows sharing a schedule or a payload stream; 43 draws repeated an earlier
position within their own cell and were included in the counts.

Eleven arms run under one completed-incident driver. After every failure the arm proposes a resume, the scan continues
from it, and the incident ends at the end of input, at a refusal, or at a budget of one hundred attempts, the terminal
outcome recorded per trial. All blind arms search from one past the failure offset, the same progress contract the
driver keeps; the two oracle arms floor their search at the corruption end, modeling a caller told the
operation-designated start of the guaranteed-untouched suffix, and are taken up with the trust discussion. Returned
offsets are byte-exact. The arms are certified recovery, the walk's answer with its evidence interval archived per
trial; certified-clean, the same walk under the oracle floor; exact, the library's anchored decider
(`next_anchored_start`, exact for complete-repair invariance on non-nullable sets by Theorem 9) run as a procedure, the
anchor advancing past a tail the decider refuses until a certificate holds; exact-clean, the decider under the oracle
floor; and skip-one, the search start itself, the lexical panic baseline. The rest are delimiter conventions. The raw
ones run in both placements, newline-past and semicolon-past answering one past the delimiter's offset (the archive
records them under the names newline and semicolon) and newline-at and semicolon-at at the delimiter's own offset, with
an in-harness regression failing if the two are interchanged. One past a final delimiter is the end-of-input offset,
which counts as a completed resume, so the placements answer together everywhere and the paired regression asserts they
differ by exactly the delimiter on every trial. The fresh-restart token-filtered pair, token-newline and
token-semicolon, makes the classical two-phase reading concrete at the lexical layer, skipping until a fresh scan makes
progress, discarding its emitted tokens through the first designated synchronizer, the delimiter's own punctuation token
or an all-whitespace token carrying the newline, and resuming one past it. The filter is relative to the restarted scan,
not the pristine one, and a restart inside an original string or comment can reclassify interior text, so context
blindness is reduced, never abolished.

Positions throughout are byte offsets, named apart: `d` the damage start (the archive's `p` column), `e` the corruption
end (the operation-designated start of the guaranteed-untouched suffix, as defined above), `f = con(y)` the failure
offset, the answer the resumed slice's first byte. An answer lands when it equals the shift-mapped image of a pristine
boundary outside the damaged window. Detection lag is real and measured. The signed lag `f - d` has median 0 and 99th
percentile 17 over the range `[-436, 78]`, negative when a token opened before the damage dies at it, and `f` lies
before `e` on 40,401 of the 43,736 broken-scan trials. The metrics per arm and trial are the first answer's position and
landing, the terminal position and its landing where it lies inside the input, the terminal outcome (completed, refused,
or capped), attempts per incident, and mean signed overshoot from the first mapped boundary at or past the corruption
end. The tables' overshoot column reports mean absolute overshoot. For completed incidents, convergence is the signed
distance from the corruption end to where the resumed token-boundary stream and the mapped pristine boundary stream
agree forever after, token starts compared and never token identities, with the mapped boundaries from the corruption
end to that point counted lost and the non-landing emitted starts in the divergence region counted spurious. Landing and
overshoot score each arm's first answer per incident, skip-one's first one-byte step included; repeated skipping
surfaces in the attempts column.

Every trial is additionally stratified by repairability at the blind anchor, the routine-reported verdict of the shipped
`minimal_repair` with every returned repair witness-verified by scanning, so an answer on a tail no repair can complete
is labeled as such while remaining in the pooled averages. Positive labels carry their witnesses; negative labels rest
on Theorem 9, by which, over a non-nullable token set, for every input `y` and every anchor `0 <= c <= |y|`,
`minimal_repair(y[c..]) = nullopt` implies `R_complete(y, c) = ∅`. The campaign's release refused nullable sets, whether
or not a repair exists, but every campaign automaton is non-nullable, so the theorem covers every labeled trial. Pooled
rates below carry Wilson 95 percent intervals as descriptive conditional-on-draw summaries, never as inferential bands.

**Proposition 2 (the pristine corpus is a repair).** Let `y[e..] = x[c..]` with `x` completely tokenizable, and let
`p >= e` be a repair-invariant resynchronization point for `y` anchored at `e`, relative to evidence `y[q..q+w)` with
`e <= q`. Then `c + (p - e)` is a token boundary of `x`.

*Proof.* `x = x[0..c) · x[c..]` exhibits `x` itself as a tokenizable repair of `y`'s preserved suffix, with
`r = x[0..c)`; a tokenizable repair is evidence-reaching for the stated evidence, so the definition places a token
boundary at `|r| + (p - e) = c + (p - e)`. ∎

The harness's landing check rests on this proposition, and Figure 2 draws the two cases it separates on one byte axis. A
certified answer whose supporting evidence, the byte itself or the whole window occurrence, lies at or after `e` is
repair-invariant for `y` anchored at `e`, so its image must be a boundary of the pristine corpus, and the harness
asserts that on every such trial, failing the run on any violation. It asserted the landing on every one of the 38,141
evidence-covered first answers in this campaign, and on every covered answer of every later recovery move besides,
40,885 covered moves of the 48,217 the certified incidents made in total, all landing. Answers whose evidence begins
before `e` fall outside Proposition 2's applicability at anchor `e`, its `e <= q` premise unmet, so they are reported
measurements, never counterexamples. The campaign measured 5,595 such first answers of which 1,589 landed, reported and
not asserted.

Near the seam the damaged input's true segmentation can genuinely diverge from the mapped pristine one. The same
conservative oracle is applied uniformly to every arm, though their near-seam exposure differs. Before any corruption, a
pristine-corpus oracle pass asserted that every certified answer from 512 rejection-sampled offsets per row is a
boundary, with zero violations over the six rows, the same discipline the split-points paper uses. The sampler draws
without bias from every offset but the corpus's final byte.

**Figure 2.** The paper draws the coverage split on one byte axis: failure offset `f`, blind anchor `f + 1`, corruption
end `e`, evidence `[q, q + w)` with the answer `p` inside it. Above, covered evidence begins at or past `e` and
Proposition 2 applies at anchor `e`. Below, uncovered evidence begins before the corruption end, the anchor's `e <= q`
premise is unmet, and only the walk's guarantee at the blind anchor remains. The drawing is schematic and not to scale,
and its overlap with the damage span is only one uncovered geometry, since uncovered means `q < e` alone.

Table 3 draws the guarantee boundary once, so no later figure has to carry its own caveat. The table lists the results
the campaign consumes, with what each needs, what it gives, and what it leaves open.

**Table 3.** The guarantee boundary, read top to bottom as a ladder. Each row names one result, what it needs before it
applies, what it gives once it does, and what it leaves open, so a reader can locate any campaign figure on the exact
rung that backs it and see in the last column where that rung's guarantee stops. The campaign's figures cite their rows.
The 40,885 covered move-level landings are Proposition 2 instances, the 13,522 collapses are Proposition 1 instances
whose emptiness Theorem 9 supplies, and the 1,589 uncovered first-answer landings remain resynchronization-theorem
instances at their blind anchors whose pristine transfer no row supplies, so there they are measurements.

| Result                     | Needs                                                                                                                                                               | Gives                                                                                   | Leaves open                                                           |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| Resynchronization theorems | a certified byte or window occurrence at or after the anchor (for windows, flat non-nullable sets), its evidence preserved, the repair's scan committing through it | a committed boundary at the image, in every such repair                                 | whether any such repair exists, which Theorem 3 decides from the tail |
| Corollary 1                | the same certificate, with the repair under consideration tokenizing whole                                                                                          | the same boundary                                                                       | existence again, decided the same way                                 |
| Proposition 1              | `p` repair-invariant relative to its evidence, `c <= q`, resumed suffix completely tokenizable                                                                      | `R_complete = R_reach` at that anchor                                                   | whether either set is inhabited, read off `E` and `F` in Theorem 5    |
| Proposition 2              | `p` repair-invariant at anchor `e` with `y[e..] = x[c..]`, `x` completely tokenizable, evidence at or after `e`                                                     | the image is a boundary of `x`                                                          | pristine transfer when `q < e`                                        |
| Corollary 3                | `p` repair-invariant relative to its evidence, `c <= q`                                                                                                             | `R_complete ⊊ R_reach` exactly when `R_complete = ∅` and `R_reach ≠ ∅`                  | which of the two holds at a given anchor, which Corollary 4 decides   |
| Lemma 4                    | the walk's floor at the blind anchor                                                                                                                                | covered exactly when the evidence travel reaches the overhang less one                  | how far the evidence lies from the anchor                             |
| Theorem 9                  | the routines as shipped, on non-nullable sets in the campaign's release and on any token set since v2.0.0                                                           | `R_complete = ∅` on negative-labeled tails, a shortest complete repair on positive ones | nothing, the proof being Section 4's                                  |
| Theorem 9, the decider     | the same                                                                                                                                                            | exact complete-repair decisions at the anchor, the least invariant position             | evidence-reaching exactness, which Theorem 3 supplies instead         |

**Table 4.** Evidence coverage per row, certified first answers only. Covered answers, evidence at or past the
corruption end, pass the asserted landing check, every one. Uncovered answers sit outside the transfer premise, and
their landings are reported, never asserted.

| Row                    | Answers | Covered | Uncovered | Uncovered landed |
|------------------------|--------:|--------:|----------:|-----------------:|
| C-like, conventional   |   7,356 |   6,995 |       361 |              119 |
| C-like, block comments |   4,754 |   4,753 |         1 |                1 |
| JSON, generated        |   8,888 |   8,051 |       837 |              554 |
| C-like, split-friendly |   7,306 |   6,817 |       489 |              291 |
| C-like, bare           |   7,877 |   4,387 |     3,490 |              376 |
| JSON, real document    |   7,555 |   7,138 |       417 |              248 |
| Total                  |  43,736 |  38,141 |     5,595 |            1,589 |

**Table 5.** Certified recovery pooled per row over the campaign: answers and refusals, first-answer landing rate, mean
absolute overshoot in bytes, mean recovery attempts per incident, and mean signed convergence distance in bytes from the
corruption end to where the resumed boundary stream agrees with the mapped pristine one forever after. The bare row's
60.5% is its pooled landing rate. The block-comment row's 164.9 bytes accompanies the certificate sparsity the text
discusses, and its convergence decomposes into the same distance once, with no accumulation over attempts. Every figure
recomputes from the archived CSV through the checked-in analysis programs.

| Row                    | Answers | Refusals | Landing | Overshoot | Attempts | Conv. |
|------------------------|--------:|---------:|--------:|----------:|---------:|------:|
| C-like, conventional   |   7,356 |        0 |   96.7% |      25.3 |     1.02 |    28 |
| C-like, block comments |   4,754 |        0 |  100.0% |     164.9 |     1.00 |   169 |
| JSON, generated        |   8,888 |        0 |   96.8% |       4.2 |     1.03 |     7 |
| C-like, split-friendly |   7,306 |        0 |   97.3% |      24.3 |     1.02 |    27 |
| C-like, bare           |   7,877 |        0 |   60.5% |       4.3 |     1.47 |     3 |
| JSON, real document    |   7,555 |        0 |   97.8% |       3.3 |     1.02 |    26 |

**Three patterns in the recovery results.** First, the pristine-repair transfer is assertable exactly where its
evidence-preservation condition holds. The harness checked every evidence-covered answer against its mapped pristine
boundary, first answers and every later move alike, and all 40,885 move-level checks passed, the 38,141 first-answer
checks among them (Table 4 splits the coverage per row). Overall, certified recovery landed 90.8% of its 43,736 first
answers, Wilson interval [90.6, 91.1], refusing nothing in this campaign, and the pooled figure's entire distance from
the window-dense rows' 96.7 to 100% (Table 5) is one row's trusted-anchor price, taken up below. The classical
conventions carry no assertion clause anywhere. Skip-one lands 10.0% pooled at 10.9 recovery attempts per incident
against the other answering arms' 1.0 to 1.1 (the semicolon variants average below one, their frequent first-move
refusals contributing zero attempts), and it alone exhausts the attempt budget, 776 incidents.

Second, where certificates are dense the guarantee is also near. On the generated JSON row certified recovery's mean
absolute overshoot is 4.2 bytes against newline-past's 29.5 (Table 6 pools every non-oracle arm per row), and its
resumed stream agrees with the mapped pristine one within a median of 6 bytes past the corruption.

Third, on the rows where certificates are scarce the guarantee is paid for in distance. On the block-comment row the
certified answers land 100% but 165 bytes out on mean absolute overshoot, while newline-past lands 92.5% at 22.5 bytes,
under a seventh of the distance (Figure 3). The certificate guarantees soundness at its own position, not proximity, and
on these rows the price tracks certificate sparsity.

One pooled number, the first-answer landing rate, favors a classical convention over certified recovery. The
fresh-restart token-filtered newline convention lands 97.1% pooled, above certified recovery's 90.8%. Most of the pooled
gap is in the bare row. The coverage split above stratifies certified recovery's landing rate there. The convention
supplies no repair-universal boundary guarantee behind any of those landings and no answer at all where its delimiter
never occurs as a designated token, as the semicolon variants' refusal counts show at scale. Its median convergence
distance is 26 bytes, more than twice certified recovery's 10.

**Short evidence is exposed: the byte path under damage.** The bare C-like row, where every operator and punctuation
byte certifies exactly, exercises the byte path at scale. It also lands worst, 60.5% overall, Wilson interval [59.4,
61.5]. The covered split decomposes that rate within the one row and campaign schedule, grouped by the returned
certificate's kind, an observed, unrandomized split. Of the row's 2,408 byte-evidence answers, 2,213 rest on evidence
not wholly in the preserved suffix, 91.9 percent, and none of them land, while of its 5,469 window answers 1,277 are
uncovered, 23.3 percent, and 376 of those uncovered answers land. All 4,387 evidence-covered answers of the row pass the
asserted landing check every time. The contrast does not lean on the cross-row comparison, where evidence length is
confounded with grammar and corpus. One explanation consistent with this split is that a single byte is far easier for
damage to produce than a two-to-four-byte occurrence with the right context.

**The mechanism behind coverage, from the archive.** Coverage has a proved part and an observed part. The identity is
proved algebraically, and the archive recomputes the observed bins (Figure 4) while checking every archived row against
it.

**Lemma 4 (the overhang law).** Call `η = e - f` the damage's *overhang* and `l = q - (f + 1)` the walk's *evidence
travel* for that answer, the distance from the blind anchor to where the answer's evidence begins. The answer is
covered, `q >= e`, exactly when `l >= η - 1`; the search starts at the blind anchor, so `l >= 0`, and an overhang of at
most one forces coverage.

*Proof.* `q >= e` rearranges to `q - (f + 1) >= (e - f) - 1`, and the walk's documented floor puts `q` at or past
`f + 1`. ∎

The threshold is sharp. For every `η >= 2` choose `n` with `2n >= η + 2`, and in `x = (ab)^n` replace `x[2..2+η)` by the
length-`η` prefix of `(ba)^ω`. The corruption end is the one the operation designates, `e = 2 + η`, as everywhere else
in this paper. This block is chosen because it also differs from `x` at every replaced position, so the damage is actual
across the whole designated span. The instance stands under either reading of where the damage ends, the designated end
or the byte-difference end one past the last differing byte. Every `a` of a completely tokenizable string over `{ab}`
begins a token, so `a` is a certified byte. The scan commits the leading `ab` and fails at `f = 2`; the blind search
from three returns the certified `a` at `q = p = 3`, and `q < e` for every `η >= 2`. At every overhang at least two
there is thus an uncovered certified instance, and the lemma's threshold cannot be weakened.

**Figure 3.** Pooled first-answer landing rate per row and arm, recomputed from the archived campaign, in percent; the
paper draws these as grouped bars, and the two near-zero bars, bare newline-at and real JSON newline-past, carry their
printed values 0 and 0.01 so a missing series cannot be read where a collapse is the finding. The raw placements still
flip between near-perfect and near-zero across rows. Token filtering removed that failure mode in this campaign, landing
97.0% on the real document where its raw counterpart fails. Certified recovery needs no placement convention, its one
low row the trusted-anchor precondition. Blind exact wins that row and loses the conventional one, the complementarity
the text takes up. The semicolon arms are in Table 6.

| Row            | certified |  exact | skip-one | newline-past | newline-at | token-newline |
|----------------|----------:|-------:|---------:|-------------:|-----------:|--------------:|
| conventional   |     96.71 |  54.28 |    12.18 |        97.96 |      26.59 |         98.00 |
| block          |    100.00 | 100.00 |     8.46 |        92.51 |       1.28 |         93.67 |
| JSON           |     96.82 |  99.16 |    12.32 |        97.57 |      97.54 |         97.57 |
| split-friendly |     97.29 |  99.60 |    12.55 |        97.43 |      97.29 |         97.43 |
| bare           |     60.47 |  89.12 |     8.04 |        97.73 |       0.00 |         97.74 |
| real JSON      |     97.76 |  98.32 |     5.56 |         0.01 |      96.89 |         96.96 |

**Figure 4.** Coverage and first-answer landing for certified recovery against the damage's overhang past the failure
offset, recomputed from the archived CSV, with the bin populations from left to right in the paper's plot. At overhang
at most one the anchor shelters the evidence and coverage is forced, and every uncovered answer lives to the right.

| Overhang e - f | Answers | Covered (%) | Landed (%) |
|----------------|--------:|------------:|-----------:|
| <= 0           |   3,335 |       100.0 |      100.0 |
| 1              |   8,980 |       100.0 |      100.0 |
| 2              |   1,133 |        92.3 |       96.4 |
| 3              |   2,733 |        88.8 |       93.5 |
| 4 to 7         |   9,708 |        87.9 |       92.2 |
| 8 to 15        |   5,103 |        75.9 |       80.9 |
| 16 to 31       |  11,558 |        76.4 |       82.4 |
| 32+            |   1,186 |        94.4 |       98.2 |

**The known-clean arms.** The effect of supplying a known-clean search floor is measured. An oracle-aided campaign arm
runs the same walk given the operation-designated start of the guaranteed-untouched suffix, starting at the corruption
end or one past the failure, whichever is later, and a second oracle arm runs the exact decider from the same floor.
Their soundness arguments differ, and each arm's landing is asserted per trial. The clean walk's evidence is covered by
construction and every answer lands under the pristine transfer, while the clean decider returns no evidence and its
landings rest on complete-repair invariance with the pristine corpus the complete repair. Each arm gives 43,736 first
answers, the same count as the blind walk's, every one landing, with no refusals. The clean and blind walks answer the
same trials but not at the same places. Their first positions differ on 4,592 of the 43,736 paired answers, 10.5
percent, so what the truth about the damage changes is both which theorems cover the answers and where a tenth of them
land, which makes the deployment precondition's content a measured column.

**The real document.** The real-document row runs the same JSON grammar over twitter.json, real Twitter API output.
Certified recovery lands as well here as on generated JSON, 97.8% of its 7,555 first answers against 96.8%, at a mean
absolute overshoot of 3.3 bytes against 4.2, though its mean signed convergence distance grows from 7 bytes to 26. The
newline baselines expose the placement sensitivity the campaign measures as outcome-critical, which is why both
placements are named. Newline-past collapses to one landing in 7,555 first answers, 0.01%, because the document is
pretty-printed, almost every newline is followed by indentation, and skipping past the newline lands inside the run.
Retained, newline-at lands 96.9% here, by hitting the whitespace run the newline begins. Neither number transfers, since
on the generated rows the same one-byte change runs the other way, pooled per row, 98.0% falling to 26.6% on the
conventional row, 92.5% to 1.3% on block comments, 97.7% to 0.0% on bare, because the generated layout makes the newline
mid-run and the byte after it a token start. The token-filtered form settles the placement question by resuming at the
end of a synchronizer token emitted by its restarted scan. Token-newline lands 97.0% on this document and 93.7 to 98.0%
on every generated row, because it never resumes inside a token its own scan just emitted, so the token-filtered
baseline both repairs the classical convention and removes its one-byte trap. The landing swing between the two
placements was a byte-level artifact, and what no delimiter reading escapes is availability and the missing
repair-universal boundary guarantee. The certificate's soundness is placement-free and, for a fixed automaton with
preserved evidence, layout-independent, while availability, coverage, refusal, and proximity remain corpus and layout
properties, measured above.

**Refusal is explicit and scoped.** Certified recovery refused nowhere in this campaign, 43,736 first answers on 43,736
broken scans. Its refusal mode is real, pinned by fixture, and unexercised on these corpora under the rejection sampler.
On the window-dense rows every answer is a window answer, those grammars carrying no useful byte certificates. The bare
and split-friendly rows supply the byte path above, 2,408 and 51 byte-evidence answers. The delimiter arms show what
refusal looks like without a theorem. The semicolon variants each refuse over fifteen thousand of the campaign's 43,736
trials at the first move, over sixteen thousand terminally once incidents that answered and then starved are counted,
raw and token-filtered alike and nearly all on the two JSON rows. The token-filtered form returns nothing at all on the
real document, whose single pristine semicolon lies inside a string token that every restarted scan in this campaign
emitted whole, while the raw forms' few answers lie at or immediately after that in-string byte, and semicolon-past's
answers land 0.7 percent. Skip-one never refuses and lands rarely, 10.0% pooled, the opposite failure.

**The vacuity fence, stratified and then decided.** Every trial carries the verdict of the shipped `minimal_repair`, the
routine's answer to whether any repair completes the blind tail. It labels 26,928 trials repairable against 16,808
beyond repair, and the walk answered every one of the latter, 38.4 percent of its first answers. By Theorem 9 a negative
label is exact, so at those 16,808 blind anchors `R_complete` is empty and the complete-repair reading of Corollary 1
claims nothing; for 13,522 of them the strengthened semantics is proved to fare no better. Every certified answer's
evidence lies at or after its blind search anchor, so Proposition 1 needs only one further premise per trial, a resumed
scan that completes in one attempt. This premise holds for 13,522 of the 16,808 trials. These comprise the 12,023 whose
evidence the damage spared and 1,499 more on evidence not wholly in the preserved suffix. At all 13,522 blind anchors
the two quantifiers therefore coincide, and both are empty. Coincidence holds more widely still, because on the 26,928
trials the routine labels repairable, every returned repair was witness-verified by scanning, so `R_complete` is
inhabited by exhibition and Corollary 3 forces the two sets equal there. Equality therefore holds at 40,450 of the
campaign's 43,736 answers, 92.5 percent, and only the 3,286 remain candidates for strict inclusion at all.

The remaining 3,286, whose first resumed scans failed downstream, are the only answers this campaign leaves undetermined
in content at the blind anchor. Section 4 can decide each of them from its preserved tail. A trial's negative label
gives `E < m`, so by Corollary 4 the inclusion is strict exactly when `h <= E`, where `h = q + w - c` is the evidence
endpoint. Applying this test requires rerunning the campaign, because the archive keeps each trial's offsets and labels
but not its damaged input, so the count stands at 3,286. These answers' soundness stands either way. Coverage did not
enter that argument; what coverage decides is the transfer. The 12,023 covered answers all landed, guaranteed at the
corruption-end anchor where the pristine corpus is a complete repair, the same guarantee the known-clean arms exercise
by construction, while the 1,499 uncovered collapses carry no such transfer and land 1,066 times, which no theorem
guarantees. For uncovered evidence the corruption-end anchor is no anchor at all, the evidence beginning before it,
outside the definition's domain, though each such answer stays governed by the walk's soundness theorem at its own blind
anchor. For deployment, then, every returned answer is sound at its blind anchor, and coverage decides which answers
additionally carry Proposition 2's pristine transfer, the 12,023 against the 1,499. The failure offset cannot draw that
line; the returned evidence interval can, which is why the evidence-returning form of the interface returns it.

**The anchored decider, a different-property comparator.** The second machine in the campaign is the library's anchored
decider, exact for complete-repair invariance by Theorem 9, the campaign's release holding it on non-nullable sets. With
the tail's end known to be the end of input, it answers the positions every completely tokenizable repair agrees on and
refuses tails no repair completes. That is a different predicate from the certificates' evidence-reaching soundness, and
the two separate on a two-token example. Over `{ab, ba}` the decider answers position zero of the tail `ab`, yet
relative to the one-byte evidence `y[0..1)` the repair `b` commits `ba` and dies having reached that evidence's image,
with no committed boundary at the answer's image, so the decider's answer is not evidence-reaching-invariant for that
interval. With the whole tail as evidence, reaching and completing already coincide, which is the decider's own reading.
The comparison below is therefore between two different guarantees. The campaign binds them where their properties
overlap and tests both directions on the decider's direct call at the blind anchor. The direct answer is archived as a
column. The direct call answered every one of the 26,928 routine-labeled-repairable trials at or before the walk, never
later, which is not a coincidence of these rows:

**Proposition 3 (the decider never answers later than the walk).** Fix a non-nullable token set and an anchor `c`.
Suppose the walk, searching from `c`, answers `p` on evidence `x[q..q+w)` with `c <= q`, and the anchored decider,
consulted at the same anchor, which by Theorem 9 reports the earliest position at or after `c` meeting its certificate
predicate, answers `d`. Then `d <= p`.

*Proof.* Every completely tokenizable repair is evidence-reaching, so by the resynchronization theorems a committed
token begins at `p`'s image in every complete repair anchored at `c`, and the position `p` is therefore one the complete
repairs agree on. By Theorem 9 the decider reports the earliest position at or after `c` meeting that predicate, so
where it answers at all its answer `d` is at or before every such position; `p` is such a position, hence `d <= p`.
Exactness alone would not suffice, because an invariant position that is not the earliest could exceed the walk, so it
is the minimality Theorem 9 states, matched by the shipped `next_anchored_start`, that the bound rests on. ∎

The measured containment is that proposition instantiated, worth 10,738 bytes across those trials. On every trial the
routine labeled unrepairable, the decider's direct call refused. That agreement is only a consistency regression between
the two routines, since both walk the same scenario machinery and are exact by the same theorem, Theorem 9. Every repair
the routine did return was witness-verified by scanning it against its tail.

Run as a procedure, the decider answers everything and lands 89.8 percent pooled against the walk's 90.8, and the
per-row split is the finding. The decider wins the bare row, 89.1 against 60.5 percent, and loses the conventional row,
54.3 against 96.7. On that row an advanced anchor still sits inside a broken string and anchored reasoning binds repairs
of a damaged prefix, while the walk's occurrence-universal certificates land wherever their evidence survived. Under the
oracle floor both land on every trial, 100 percent asserted, and each rests on its own result. The clean walk rests on
the pristine transfer, its evidence covered by construction, and the clean decider on complete-repair invariance, with
the pristine corpus as the complete repair, the contract Theorem 9 proves. The two machines are complementary because
their properties are. Anchored completeness paid off at this campaign's trustworthy anchors and evidence-order search at
its poisoned ones, two row-level contrasts and no law about classes of anchors. At 13,522 of the poisoned anchors the
surviving answers carry no evidence-reaching content either, only the clean-anchor transfer where their evidence is
covered.

**Convergence: what each convention costs the stream.** First landings decide where an arm resumes, and convergence
measures what that choice costs downstream. It is the signed distance from the corruption end to the point where the
resumed boundary stream agrees with the mapped pristine one forever after, taken over completed incidents, and the
boundaries from the corruption end to that point, the initial jump included, are counted lost. Skip-one, the worst arm
by pooled first-answer landing rate, converges at median 2 bytes and loses only 0.90 boundaries per error pooled, at the
price of 10.9 attempts per incident, 0.9 spurious starts per error inside the divergence region, and 776 budget
exhaustions.

Certified recovery converges at a pooled median of 10 bytes, 2 on the bare row and 6 on generated JSON. These
convergence figures are medians, whereas the convergence column of Table 6 reports means. Certified recovery loses 6.85
boundaries per error pooled, from 0.22 on the bare row to 34.6 on block comments. On the block-comment row, where every
completed certified incident converges at its first answer, the lost boundaries are the initial jump the overshoot
already stated. They are the same quantity counted in tokens instead of bytes, and they appear once without
accumulating. On the other rows a later move can lose boundaries the first answer did not skip.

The newline conventions lose slightly more than certified recovery, 8.01 past and 7.42 retained, at a median of 24 to 26
bytes. The semicolon forms lose a statement's worth regardless of placement, 24.4, 23.4, and 24.5 for past, retained,
and token-filtered, at a median of about 60. The flip that swings first landings between 0.01 and 96.9 percent on the
real document moves convergence by roughly a token, so the classical convention's real cost is the discarded remainder
of the line. The token-filtered semicolon form shows the availability cliff in stream terms. It refuses every incident
on the real document, because it declines to treat the document's single candidate delimiter, which lies inside a string
token, as a synchronizer.

**The price of evidence order, measured.** The walk answers with the first certificate in evidence order, and the
campaign priced that fence. Of the campaign's 43,736 certified first answers, 90 were nonminimal, by 90 bytes in total,
one byte each. The fence is real and its cost on these rows is negligible, which is worth knowing before anyone
complicates the walk to remove it.

**Independent bounded verification.** A from-scratch maximal-munch reference model, written in Python against only the
public API and kept in the munch test suite as the recovery cross-check, is checked against the shipped library's own
scan on 7.6 million strings with zero disagreements. It also drives bounded exhaustive sweeps over 89 small non-nullable
literal-token sets, comparing boundaries and committed lengths, and each obligation carries its own denominator. For
walk soundness the sweeps cover 46,035,590 (answer, repair) pairs, the evidence-reaching premise holding on 1,308,265 of
them, and find no violations. The collapse proposition's sweeps cover 836,387 walk answers, its premise holding on
148,920, again with no violations. For the anchored decider they cover 4,712,940 (answer, completing repair) pairs with
no violations, and the decider is never later than the walk on the 174,368 tails where the bounded search exhibits a
completing repair and both answered. None of the routine's 95,836 negative labels is refuted by any repair up to length
four, a consequence of Theorem 9 kept as a check on the implementation. For pristine transfer, an independently mapped
damage sweep of 2.36 million trials has all 1,068,953 covered answers landing, which replicates Proposition 2 without
the harness. Every quantifier is bounded where the contracts are not, and literal tokens leave regex structure and
priority ties outside the tested universe, so these runs are evidence, never proof. The drivers' executed negative
controls make a silent check an exit-failing defect of the cross-check itself, which runs in the munch test suite, where
the library it checks is built.

**Table 6.** Every non-oracle arm pooled per row, three seeds of five hundred trials per cell, the body generated by the
archived analysis program and taken up verbatim. The columns are first-answer count and landing rate, mean absolute
overshoot, mean signed convergence distance, completed share, and terminal refusals, incidents that never answered or
answered and then starved, defined with the metrics above. The two oracle arms are omitted as constant, 100 percent
landing on every row, asserted per trial. The per-cell grid over three operations, three damage sizes, and three seeds
is in the archived CSV, where the past placements appear under the names newline and semicolon. Elsewhere the rows
shorten, in this order, to conventional (strings and line comments), block, generated JSON, split-friendly, bare, and
real JSON. One rounding is load-bearing. The real document's newline-past cell prints 0.0 from exactly one landing in
7,555, while the bare newline-at cell is a true zero. A blank cell is one the generated body prints with no value.

| Row                               | Arm             | Answers | Landing | Overshoot | Conv. | Completed | Refused |
|-----------------------------------|-----------------|--------:|--------:|----------:|------:|----------:|--------:|
| C-like, strings and line comments | certified       |   7,356 |   96.7% |      25.3 |    28 |    100.0% |       0 |
|                                   | exact           |   7,356 |   54.3% |      25.2 |    28 |    100.0% |       0 |
|                                   | skip-one        |   7,356 |   12.2% |       9.2 |     3 |    100.0% |       0 |
|                                   | newline-past    |   7,356 |   98.0% |      25.6 |    28 |    100.0% |       0 |
|                                   | newline-at      |   7,356 |   26.6% |      24.7 |    28 |    100.0% |       0 |
|                                   | semicolon-past  |   7,356 |   98.1% |      82.7 |    86 |    100.0% |       0 |
|                                   | semicolon-at    |   7,356 |   98.0% |      81.7 |    85 |    100.0% |       0 |
|                                   | token-newline   |   7,356 |   98.0% |      25.6 |    28 |    100.0% |       0 |
|                                   | token-semicolon |   7,356 |   98.1% |      82.8 |    86 |    100.0% |       0 |
| C-like, block comments            | certified       |   4,754 |  100.0% |     164.9 |   169 |    100.0% |       0 |
|                                   | exact           |   4,754 |  100.0% |     164.9 |   169 |    100.0% |       0 |
|                                   | skip-one        |   4,754 |    8.5% |      10.8 |     4 |    100.0% |       0 |
|                                   | newline-past    |   4,754 |   92.5% |      22.5 |    26 |    100.0% |       0 |
|                                   | newline-at      |   4,754 |    1.3% |      21.7 |    26 |    100.0% |       0 |
|                                   | semicolon-past  |   4,752 |   97.7% |     125.7 |   133 |    100.0% |       2 |
|                                   | semicolon-at    |   4,752 |   97.6% |     124.8 |   132 |    100.0% |       2 |
|                                   | token-newline   |   4,754 |   93.7% |      23.1 |    26 |    100.0% |       0 |
|                                   | token-semicolon |   4,752 |   97.7% |     125.7 |   133 |    100.0% |       2 |
| JSON, RFC 8259 lexical forms      | certified       |   8,888 |   96.8% |       4.2 |     7 |    100.0% |       0 |
|                                   | exact           |   8,888 |   99.2% |       1.9 |     4 |    100.0% |       0 |
|                                   | skip-one        |   8,888 |   12.3% |       9.9 |    12 |    100.0% |       0 |
|                                   | newline-past    |   8,888 |   97.6% |      29.5 |    32 |    100.0% |       0 |
|                                   | newline-at      |   8,888 |   97.5% |      28.5 |    31 |    100.0% |       0 |
|                                   | semicolon-past  |     208 |    6.2% |       8.9 |     0 |      0.2% |   8,874 |
|                                   | semicolon-at    |     208 |    0.0% |       9.9 |       |      0.0% |   8,888 |
|                                   | token-newline   |   8,888 |   97.6% |      29.5 |    32 |    100.0% |       0 |
|                                   | token-semicolon |       0 |         |           |       |      0.0% |   8,888 |
| C-like, split-friendly            | certified       |   7,306 |   97.3% |      24.3 |    27 |    100.0% |       0 |
|                                   | exact           |   7,306 |   99.6% |      24.7 |    27 |    100.0% |       0 |
|                                   | skip-one        |   7,306 |   12.6% |       9.2 |     3 |    100.0% |       0 |
|                                   | newline-past    |   7,306 |   97.4% |      25.3 |    28 |    100.0% |       0 |
|                                   | newline-at      |   7,306 |   97.3% |      24.3 |    27 |    100.0% |       0 |
|                                   | semicolon-past  |   7,304 |   97.7% |      81.8 |    86 |    100.0% |       2 |
|                                   | semicolon-at    |   7,304 |   97.6% |      80.9 |    85 |    100.0% |       2 |
|                                   | token-newline   |   7,306 |   97.4% |      25.3 |    28 |    100.0% |       0 |
|                                   | token-semicolon |   7,304 |   97.8% |      82.1 |    86 |    100.0% |       2 |
| C-like, bare                      | certified       |   7,877 |   60.5% |       4.3 |     3 |    100.0% |       0 |
|                                   | exact           |   7,877 |   89.1% |       1.2 |     3 |    100.0% |       0 |
|                                   | skip-one        |   7,877 |    8.0% |       8.4 |     2 |    100.0% |       0 |
|                                   | newline-past    |   7,877 |   97.7% |      22.1 |    24 |    100.0% |       0 |
|                                   | newline-at      |   7,877 |    0.0% |      21.2 |    24 |    100.0% |       0 |
|                                   | semicolon-past  |   7,877 |   97.7% |      74.9 |    78 |    100.0% |       0 |
|                                   | semicolon-at    |   7,877 |   97.6% |      73.9 |    77 |    100.0% |       0 |
|                                   | token-newline   |   7,877 |   97.7% |      22.1 |    24 |    100.0% |       0 |
|                                   | token-semicolon |   7,877 |   97.7% |      74.9 |    78 |    100.0% |       0 |
| JSON, real-world document         | certified       |   7,555 |   97.8% |       3.3 |    26 |    100.0% |       0 |
|                                   | exact           |   7,555 |   98.3% |       2.2 |    25 |    100.0% |       0 |
|                                   | skip-one        |   7,555 |    5.6% |      48.3 |     9 |     89.7% |       0 |
|                                   | newline-past    |   7,555 |    0.0% |      18.4 |    49 |    100.0% |       0 |
|                                   | newline-at      |   7,555 |   96.9% |      17.5 |    40 |    100.0% |       0 |
|                                   | semicolon-past  |     708 |    0.7% |   16830.4 |     3 |      0.2% |   7,543 |
|                                   | semicolon-at    |     708 |    0.0% |   16829.9 |       |      0.0% |   7,555 |
|                                   | token-newline   |   7,555 |   97.0% |      26.8 |    49 |    100.0% |       0 |
|                                   | token-semicolon |       0 |         |           |       |      0.0% |   7,555 |

**Limitations and external validity.** The corpora are generated except for one real-world document, the damage model is
synthetic throughout, and real edit traces were not studied. Cell sizes are broken-scan counts and vary with absorption,
and while pooled rates carry Wilson intervals, individual cells remain small in the absorbed strata. Damage positions
come from unbiased rejection sampling over the full span under three independent seeds, with every row's schedule and
payload streams seeded apart. Per-seed figures are printed beside the pooled ones, and the 43 within-cell repeated draws
are counted. Every arm runs to a terminal outcome under one driver, so the completed-incident view is a reported column,
and only skip-one ever exhausts the budget of one hundred attempts. The anchored procedure's per-move cost was not
priced against the walk's single forward scan, since the comparison here is of quality alone. That cost is one scenario
play per automaton state over the remaining tail, with a jump-table construction that can reach quadratic in the tail
and an advancing procedure that is cubic in the worst case. Every campaign automaton is non-nullable, so the campaign
release's nullable refusal is never exercised and the comparison never leaves its domain.

Six rows over five grammars were measured, and none of the quality figures generalize beyond them, since proximity,
refusal, and exposure rates varied with certificate density and evidence length, an association this campaign observed
but did not isolate. The repairability labels and the decider's refusals rest on the anchored routines' contracts,
proved in Section 4, and the two routines share their scenario machinery, so their agreement in the campaign shows
consistency only. The 3,286 undetermined answers are decidable per trial by the same section and were not decided,
because the archive carries no damaged inputs. The byte path is exercised at scale by one grammar row, with 51 further
byte answers on the split-friendly row. The search is the shipped form, whose window component searches lengths two
through four (the byte search is not length-capped), and the cap's sensitivity was not varied. The seam oracle is
conservative and applied uniformly. No claim is made that every sound position is discoverable by the searched
certificates, and throughput was not measured. The convergence metric compares whole boundary streams, dropped tokens
included, so it credits agreement whether or not it is trivial, and the lost and spurious columns carry the divergence
region's content.

One grammar tried for the byte path produced no broken-scan trials. The grammar, a log-line lexer whose tokens are a run
of non-newline bytes and the newline, tokenizes every byte string, so no corruption can break its scan and recovery
never fires.

## 8 Related work

Panic mode is the textbook's simplest strategy at both levels. At the lexical level it deletes successive characters
until a well-formed token appears, and at the parsing level it discards symbols until one of a designated set of
synchronizing tokens is found, usually delimiters such as semicolon, selected by the compiler designer (Aho, Sethi and
Ullman 1986, pp. 88 and 164 to 165). The guarantee attached is termination and, where a synchronizer is found,
membership in the designated set. No repair-universal boundary guarantee is attached for the position landed on, and the
text notes that lexical panic-mode recovery may confuse the parser. Our progress lemma reproduces the termination half
of that classical guarantee, membership following from the search rule where a delimiter convention is in force, and the
certificates add what that guarantee lacked, a theorem about the position landed on that ranges over repairs.

A close ancestor at the lexical level is Boullier and Jourdan's two-level scheme, which repairs table-driven scanners
and LR parsers alike. Correction models specify insertions, deletions, and replacements around the detection point,
validated by the analyzer accepting the model's trailing symbols, and when local repair fails the parser skips to a
grammar-writer key terminal while the scanner deletes the offending character (Boullier and Jourdan, Science of Computer
Programming 1987). The classical comparator ideas have close antecedents here: the scanner's deletion at the offending
character is the nearest one-character antecedent of the skip-one convention, which advances from the committed failure
offset instead; the key-terminal skip anteceded the token-aware designated-token discipline without supplying a
repair-universal boundary guarantee; and the bounded correction models carry a per-correction success test, continued
acceptance. The scheme is the classical position at its most parameterized. It chooses repairs and fallback recoveries
judged excellent, mean, or poor against what a human reader would have corrected, and it states operational acceptance
conditions but no repair-universal boundary property of the resume position. Repair invariance adds the missing
quantifier for a scoped form of this design. Once an anchor and its evidence are fixed, every prefix-local correction
that preserves the evidence and whose scan commits through it agrees at the certified position.

The repair school selects one concrete fix and validates it forward. Burke and Fisher (TOPLAS 1987) defer parse actions
so repairs can be tried left of the error token, generate single-token candidates at each trial point, and judge every
candidate by the distance it lets the parse advance before blocking. A candidate must advance at least one token in
their implementation to remain in consideration, the farthest-checking candidates are kept, and the pruning criteria
were arrived at largely through experimentation with erroneous Pascal and Ada programs. When simple recovery fails,
scope closers are tried at the same trial points under their own acceptance rules. Success means the correction a
knowledgeable human reader would choose, a notion the paper calls impossible to define precisely. The validation
criterion concerns the proposed repair's parse-ahead, never the position scanning resumes at, and nothing quantifies
over the repairs not chosen, which is the quantification repair invariance makes.

Nearest in spirit is Richter's noncorrecting recovery, which attempts no correction and analyzes the remainder of the
text against the subword language, buying provable properties about the error messages, no spurious reports and no
skipped text, with no assumptions about the nature of the errors (Richter, TOPLAS 1985). Two of its 1985 observations
anticipate this paper's measurements: that panic mode trusts a single fiducial symbol "to tell the parser where it is"
even when "a string of more than one symbol were less ambiguous", which is the byte-versus-window distinction the
evaluation prices, and that the properties worth having are the provable ones. The deliverable still differs the same
way, because subword membership says the damaged remainder can extend to a sentence, which is repairability at the
language level, while repair invariance says where every such extension places a boundary.

The minimum-distance school fixes a metric and searches for minimum-cost repairs. Aho and Peterson (SIAM Journal on
Computing 1972) parse any input to completion with the fewest possible errors, through a covering grammar whose error
productions mark the positions and kinds of the corrections, in time cubic in the input; they offer the algorithm as the
yardstick for evaluating recovery methods, and the textbook echoes that framing while pricing it out of practice. The
quantification is the dual of ours, in that minimum distance searches over all repairs to return a minimum-error repair
and its parse, while repair invariance asks what every repair reaching the certificate's evidence agrees on, trading the
richer deliverable for the stronger quantifier. The shortest reaching repair and shortest counterrepair of Section 4 are
shortest replacement prefixes at the lexical layer, found among `n + 1` candidates, but they minimize the prefix's
length, not an edit distance, so the parallel ends at the quantifier. The school's modern member, Diekmann and Tratt's
CPCT+, returns the whole optimum. It (ECOOP 2020) reports the complete set of minimum-cost repair sequences at an error
location, repairs 98.4% of 200,000 real syntactically invalid Java programs within a half-second timeout each, and uses
that completeness to report fewer than half the error locations panic mode does on the same corpus. The deliverable is
richer still, every best repair at once; the quantification is unchanged, over repairs to select among, never over
repairs to agree.

The incremental lexers cited here maintain consistency across edits without certifying recovery points across
hypothetical repairs. Wagner and Graham's general incremental lexer (1997) restarts the batch machine from saved states
at startable token boundaries and reproduces a batch scan of the incorporated text; errors are handled
representationally, by unmatched-text tokens and by error patterns recognizing near misses, and their history-based
alternative leaves invalid modifications unincorporated and retries them later. Hugo and Hansson's divide-and-conquer
generator (Chalmers MSc thesis 2015) stores lexical errors in its intermediate results, and the thesis also describes a
local-error variant that lexes the following text from the starting state. These works target batch or sequential
agreement for the text they incorporate; none states that a chosen position is a committed boundary in every repair of
an unrepaired prefix whose scan commits through the stated evidence.

Coding theory offers a longstanding form of the resynchronization question. A synchronizing word for a prefix code
drives the decoder into a known state from every state on which it is defined, so a complete decoder realigns at each
intact occurrence in the stream an error leaves behind, and a partial one, under the cited definition, exactly where the
word is defined from the state the error left. Almost all complete, equivalently maximal, finite binary prefix codes
admit such words as the number of codewords tends to infinity (Freiling, Jungreis, Théberge and Zeger, IEEE Transactions
on Information Theory 2003), and Ryzhikov and Szykuła (MFCS 2018) study the complexity of finding shortest ones, proving
strong inapproximability for decoder-defined codes beside algorithms for literal ones. Synchronizing words are the
closest prefix-code analogue of repair-independent reanchoring, and the certified byte is their lexical cousin. The
general setting does not transfer directly, since maximal-munch token sets need not be prefix codes, one token may
prefix another, and commitment waits on lookahead and rollback, a decoder model the prefix-code results do not address.
The certificates quantify instead over repairs of the broken prefix, evidence-reaching failed scans included, are
decided statically per automaton, and refuse where their bounded search certifies nothing. The coding-theory question is
the existence and length of a universal resynchronizer for a code; ours is the soundness of one position in one damaged
input, under a scanning discipline whose rollback desynchronizes the decoder picture.

Three axes separate the lines above: what a position is keyed to, what its guarantee ranges over, and what happens where
nothing is found. Table 7 places the classical convention, the lexical-level ancestor, the prefix-code analogue, and the
certificates on those axes, each row argued against its sources in the paragraphs above.

**Table 7.** Three axes across the lines that re-anchor a damaged stream: what each line's search looks for in the
input, whom the resulting guarantee binds, and what happens when the search finds nothing. Each cell states only what
its paragraph argues from the sources. Panic mode attaches termination and membership but no repair-universal boundary
guarantee, the two-level scheme's acceptance tests are per-correction, and synchronizing-word guarantees quantify over
decoder states, without quantifying over repairs of a broken prefix. Panic mode, synchronizing words, and the
certificates all search the damaged input forward, while the two-level scheme instead edits at the point of detection.
The last row is the only one whose guarantee ranges over repairs, complete repairs the special case, and the only
forward search of the damaged input here that can end in a refusal.

| Line                                 | Synchronizes on                                                               | Guarantee quantifies over                                                                                           | Failure or refusal                                                                              |
|--------------------------------------|-------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------|
| Parser-level panic mode              | designated synchronizing tokens, chosen by the designer, delimiters typically | nothing beyond termination, and membership when a synchronizer is found, but no repair-universal boundary guarantee | no refusal, discarding to a synchronizer or the input's end                                     |
| Boullier and Jourdan                 | ordered correction models at the detection point, else key terminals          | the chosen correction only, by continued acceptance                                                                 | when local repair fails, the parser skips to a key terminal and the scanner deletes a character |
| Synchronizing words for prefix codes | an intact occurrence of a synchronizing word                                  | every decoder state the word is defined on                                                                          | for partial decoders, possibly undefined from the error's state                                 |
| This paper's certificates            | a certified byte or certified window occurrence                               | every prefix repair whose scan commits through the returned evidence                                                | explicit refusal when no searched certificate lies ahead                                        |

In deployment, tree-sitter aims, in its documentation's words, to be fast enough to parse on every keystroke and robust
enough to provide useful results in the presence of syntax errors, representing unrecognized text as error nodes and
inserted recovery tokens as zero-width missing nodes (tree-sitter, version 0.26.12). The property claimed is usefulness,
stated operationally; no soundness property of the re-anchoring position is asserted, which is a practical target the
theorem here is meant to sharpen.

Where these lines single out a resume position at all, it is chosen by convention, validated by a score, or inherited
from history; to our knowledge, none states a property of a fixed broken-input position quantified over every prefix
repair whose scan commits through its supporting evidence, complete repairs the special case. Repair invariance is that
scoped statement, and the certificates satisfying it were already posted for a different purpose. Certified recovery
does not prove that damaged input has a correct continuation. It proves the narrower, operationally useful claim that
whenever the search returns preserved evidence, every repair whose scan commits through that evidence agrees on the
returned boundary.

## 9 Conclusion

Parser-level panic mode chooses a delimiter by convention; the certificates choose a resume position by theorem. A
position is a sound recovery point when every repair whose scan commits through the certificate's evidence places a
token boundary there, complete repairs the corollary, and certified bytes and window origins supply such positions under
the stated anchor conditions. The guarantee costs the committed-prefix lemma, a definition of reaching, and short proofs
on top of the companions' posted theorems.

The procedure ships with its contract documented and its behavior pinned by fail-closed tests, and the measurements say
what the theorem buys. Every evidence-covered answer passed its executable landing check on every recovery move, 40,885
of 40,885, and 90.8% of the procedure's first answers landed. The library's anchored complete-repair decider, measured
beside the procedure as a different-property comparator, won the bare row and lost the conventional one, both
known-clean arms holding their contractual 100 percent. The two repair readings are proved equal at 40,450 of the 43,736
answers, 92.5 percent. At 26,928 of them a completing repair, exhibited and scanned by the harness, forces the equality,
and at 13,522 a resumed suffix that tokenizes whole forces it, the negative labels, proved exact, making both sets empty
there. Only 3,286 remain candidates for strict inclusion.

Section 4 decides inhabitation and invariance using at most `n + 1` representative repairs. It gives sharp witness
bounds, one nonvacuous threshold, a consensus basis of at most three repairs, and an evidence profile of every anchor in
`O(nm + n|Σ|)` time, linear in the input's length for a fixed automaton. The remaining 3,286 campaign answers need only
their preserved tails.

On the generated JSON row, where certificates are dense, newline-past's mean absolute overshoot is seven times certified
recovery's, 29.5 bytes against 4.2. On the campaign's rows where certificates are sparse, certified recovery pays for
its guarantee in distance, and where the searched certificates end it returns an explicit refusal.

## References

- N. Nidhögg. *Certified Split Points for Parallel Lexing: Exact and Modulo Discarded Tokens.* Preprint,
  arXiv:2608.03473, 2026.
- N. Nidhögg. *Certified Split Windows for Parallel Lexing: Recovering Boundaries Where No Byte Certifies.* Preprint,
  arXiv:2608.09761, 2026.
- N. Nidhögg. *munch: a lexical analysis library based on automata theory.* Public repository, release v1.6.0, archived
  at doi:10.5281/zenodo.22544477, 2026. https://github.com/nnidhogg/munch
- *simdjson: parsing gigabytes of JSON per second (source repository).* GitHub repository, release v3.10.1, file
  `jsonexamples/twitter.json`, 2024. https://github.com/simdjson/simdjson/blob/v3.10.1/jsonexamples/twitter.json
- T. Bray. *The JavaScript Object Notation (JSON) Data Interchange Format.* RFC 8259, December 2017.
- A. V. Aho, R. Sethi, J. D. Ullman. *Compilers: Principles, Techniques, and Tools.* Addison-Wesley, 1986. Reprinted
  with corrections, March 1988.
- P. Boullier, M. Jourdan. *A New Error Repair and Recovery Scheme for Lexical and Syntactic Analysis.* Science of
  Computer Programming 9(3):271-286, 1987.
- M. G. Burke, G. A. Fisher. *A Practical Method for LR and LL Syntactic Error Diagnosis and Recovery.* ACM Transactions
  on Programming Languages and Systems 9(2):164-197, 1987.
- H. Richter. *Noncorrecting Syntax Error Recovery.* ACM Transactions on Programming Languages and Systems 7(3):478-489,
  1985.
- A. V. Aho, T. G. Peterson. *A Minimum Distance Error-Correcting Parser for Context-Free Languages.* SIAM Journal on
  Computing 1(4):305-312, 1972.
- L. Diekmann, L. Tratt. *Don't Panic! Better, Fewer, Syntax Errors for LR Parsers.* ECOOP 2020, LIPIcs 166, 6:1-6:32.
- T. A. Wagner, S. L. Graham. *General Incremental Lexical Analysis.* Harmonia project manuscript, University of
  California, Berkeley, dated 1997; the December 6, 1999 revision of that manuscript.
  https://harmonia.cs.berkeley.edu/papers/twagner-lexing.pdf
- J. Hugo, K. Hansson. *A Generator of Incremental Divide-and-Conquer Lexers: A Tool to Generate an Incremental Lexer
  from a Lexical Specification.* MSc thesis, Chalmers University of Technology, 2015.
- C. F. Freiling, D. S. Jungreis, F. Théberge, K. Zeger. *Almost All Complete Binary Prefix Codes Have a
  Self-Synchronizing String.* IEEE Transactions on Information Theory 49(9):2219-2225, 2003.
- A. Ryzhikov, M. Szykuła. *Finding Short Synchronizing Words for Prefix Codes.* MFCS 2018, LIPIcs 117, 21:1-21:14.
  Preprint: arXiv:1806.06299.
- M. A. Bender, M. Farach-Colton. *The LCA Problem Revisited.* LATIN 2000, Lecture Notes in Computer Science 1776, pages
  88-94, Springer, 2000. doi:10.1007/10719839_9
- M. Brunsfeld et al. *Tree-sitter.* Version 0.26.12, released 2026-08-08, doi:10.5281/zenodo.21851305.
  https://tree-sitter.github.io/
