# Certified Splitting: Decidability and Anchor Supply for Maximal-Munch Tokenization

**Nicklas Nidhögg**, October 2026. Mirrors `paper/certified-splitting/certified-splitting.tex`, published as
[arXiv:2610.08854](https://arxiv.org/abs/2610.08854), which evaluates munch at the v2.1.0 release, archived at
doi:10.5281/zenodo.23026012 under the concept identifier doi:10.5281/zenodo.21752996. Its instruments in the tree are
the threaded scanner probe `tools/probes/src/parallel_scan.cpp`, which cuts a corpus at certified anchors, scans the
chunks in parallel and asserts that the boundary stream equals the sequential one; the lexer's decisions
`window_occurrence()`, `window_counterexample()` and `segmentation_difference()`, the span `anchor_free_span()` and the
audit tool's supply, each held by the library's tests to the exploration program that models it; and the probe
`tools/probes/src/paper4_recompute.cpp`, which rederives the paper's emissions from those decisions in the emissions'
own line format. The paper's runner `run-artifact.sh` pins the two probes by repository, commit and source digest. The
emissions, caches and campaign corpora, the exploration programs that derive them, the release gate and the runner are
deposited as a standalone record at doi:10.5281/zenodo.22994324. The four tables the paper inputs from its `data/`
fragments are read here from those same files, and every other table from the paper's own tabular source, never
transcribed by hand.

*A technical report on the forward reading of the certificates behind `Lexer::is_split_point()` and
`Lexer::is_split_window()`: certification decided, the anchors read into cuts, edits and audits, and the supply
measured. The implementation, tests, and probes live in this repository; this document states the results precisely,
relates them to prior work, and reports what the measurements found.*

## Abstract

A certificate is a window of bytes with an origin inside it: wherever the window occurs in a completely tokenizable
input, the token covering the window's final byte begins at that origin. Decided from the token set alone, a certified
position marks where work may soundly begin. This paper decides certification and reads its anchors forward, into cuts,
edits and audits. Certification is decided two ways: online with a completeness cutoff for literal vocabularies, and
offline through an armed-run verifier for arbitrary regular token sets. Either route, under a declared window budget,
yields the anchor inventory, with a witness on each refusal. The consequences are theorems. Cutting an input at
certified anchors and scanning the chunks independently reproduces the sequential segmentation, with no speculation and
no fixup pass. A byte substitution that keeps the input tokenizable moves boundaries only strictly between the certified
anchors witnessed in the unchanged bytes on either side of it. For tokens of length at most `L`, an edit at `p` moves no
boundary at or below `p - L + 1`. A delimiter permits a sound cut before it exactly when it sits only token-initial, and
one after it exactly when only token-final, so the embedded-delimiter failure is refused. Two instruments follow: the
anchor-free span of a finite certified inventory, decided bounded or not with its exact supremum, three for the UTF-8
shape; and a differential auditor deciding whether two token sets place different boundaries on an input both tokenize,
with witnesses. Every theorem carries an executable check. Every measurement is a named program's output or arithmetic
on such outputs, each program shipped with the paper or pinned by commit in the library it studies; that library's
threaded scanner splits the archived campaign corpus at certified anchors, its boundary stream byte-identical
throughout.

## 1 Introduction

A certificate is a window of bytes with an origin inside it: wherever the window occurs in a completely tokenizable
input, the token covering the window's final byte begins at that origin. A companion paper on recovery
([arXiv:2609.10600](https://arxiv.org/abs/2609.10600)), called the recovery companion below, proved such certificates as
instruments for resuming after damage. There a certified answer is a boundary committed by the scan of every prefix
repair that commits through the certificate's returned evidence, so a reader who has lost its place may trust the
position the certificate names. The certificate beneath that result quantifies over every completely tokenizable context
containing its evidence, and that quantifier does more work than recovery asks of it. A position that every context
agrees is a boundary is not merely a safe place to resume after something went wrong. It is a safe place to begin,
before anything has gone wrong at all.

Two tokens show why recognizing such a position needs a theorem at all. Over `T = {ab, b}`, a splitter that cuts at
every occurrence of `b`, on the ground that `b` is a token and so surely starts one, cuts the input `ab` at position 1.
The sequential scan commits the single token `ab` and has no boundary there, and the chunk before the cut, the lone byte
`a`, scans to nothing at all. The split scan fails on an input the sequential scan consumes. The cut is not suboptimal;
it is unsound, and no amount of testing on inputs where `b` happens to stand alone will say so. The certificate repairs
the reasoning by putting the quantifier where the intuition was. `b` occurs at offset 1 of `ab`, so some completely
tokenizable input places it inside a token rather than at its start, while `a` occurs in the token list only at offset
zero and therefore begins a token in every such input. Cutting at occurrences of `a` is sound, and Lemma 3 turns that
observation into a decision over the token list rather than over inputs.

Read forward, the subject is one property of a token set and four questions about it. Does a proposed window force the
token covering its final byte to begin at its named origin wherever the window occurs, whatever the surrounding input,
and what is the exact inventory of such windows through a declared budget `H`? That property is the recovery companion's
certificate. How is such a position recognized, when its definition quantifies over inputs no program can enumerate?
What does recognition cost, once that quantifier is discharged? And how often do such positions occur in the material a
scanner will actually meet? The four are not independent; existence without recognition is a definition, recognition
without a price is a promise, and either without supply is a guarantee that never fires. Wherever a scanner starts
mid-input, splits work, joins a stream late, or absorbs an edit, the question underneath is one of these four. This
paper answers all four for maximal-munch scanning, the first by deciding each proposed window and each exact inventory
through a declared budget, the second and third by theorem, and the fourth by measurement on named vocabularies and
grammars.

Table 1 lists what the paper proves and measures, and where. The main results are the decisions; the applications are
their consequences. None of them requires density. Where a grammar supplies few anchors, the theorems say so exactly,
and the measured supply of a real grammar on a real corpus is an honest fact about that input.

The two predecessors introduce the first question and open the second. All three papers share one proof pattern, local
evidence that the surrounding context cannot invalidate, with a different notion of agreement in each, namely a
boundary, a resumption and a partition. Certified positions were introduced as single bytes, with a controlled study in
which several conventional token sets certify no byte at all ([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)).
They were generalized to bounded windows that recover a token origin where no single byte certifies
([arXiv:2608.09761](https://arxiv.org/abs/2608.09761)). The first gave an exact criterion for single-byte windows and
the second a sufficient condition for wider ones; neither decided an arbitrary proposed window, and the window paper
planned cuts without an inventory or a price. This paper stands on the same certificate, arrives at it from the recovery
companion's repair-invariance machinery, and adds breadth, price, and supply. The additions are certification decided
for arbitrary regular token sets, exact anchor inventories through a declared window budget with a synthesized witness
on refusal, the anchor-free span decided with its exact supremum where one exists, the differential auditor over pairs
of token sets, and the measured supply of BPE-derived maximal-munch vocabularies. None of these appear in the
predecessors.

Three points of scope hold throughout. First, every certification decision takes a proposed window as input, and every
inventory computation a declared finite budget `H` besides. Existence at unrestricted width is a separate question, and
Corollary 2 decides it, for windows that occur and for windows that need not. Second, every supply and edit measurement
is a fact about named material under a named policy, inventory budget and seed. The scanner timings are facts about a
named machine as well, and the density verdicts say when no grammar-global gap guarantee exists. Third, the two subword
vocabularies are BPE-derived token sets read under maximal munch, which is the scan the theorems speak about and not the
ranked merge procedure that produced them, and the manuscript says so wherever the distinction bears on a number.

**Table 1.** The principal contributions and where each is stated, proved or measured. The proofs establish the
universal claims; each row's section pairs its construction with a source comment naming the executable check that
exercises it on finite instances, and the manuscript is held to the emissions the release gate names.

| Result                | Statement                                                                                                                                                                                                                                                                                         | Where                                      |
|-----------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------|
| Certification decided | Online with a completeness cutoff for literal token sets; offline through an armed-run verifier for arbitrary regular token sets, each window and origin reduced to one union-emptiness problem with a witness on refusal, so inventories through a declared budget are computable                | Theorems 1 and 2, Corollary 1              |
| Splitting             | Cutting a completely tokenizable input at certified anchors and scanning the chunks independently reproduces the sequential segmentation exactly, for literal and for regular token sets; run whole on the recovery campaign's four archived corpora                                              | Lemma 5, Theorem 4, Corollary 4, Section 6 |
| Edit damage           | A tokenizability-preserving byte substitution moves no boundary outside the open interval between the certified anchors witnessed on either side of it, nor, when token length is bounded, at or before the first position of the edit's lookahead shadow                                         | Theorem 5                                  |
| Delimiters            | A delimiter admits sound splitting at it exactly when every occurrence of it inside every token is token-initial, and after it exactly when every such occurrence is token-final, refusing the embedded-delimiter failure before any splitter is built                                            | Theorem 6                                  |
| Anchor-free span      | Whether a grammar's longest anchor-free stretch under a given inventory is bounded, and its exact supremum when it is; three for the UTF-8 shape                                                                                                                                                  | Theorem 3                                  |
| Differential auditor  | Whether two regular token sets differ in boundaries or domain: a boundary placed differently on an input both tokenize, or an input exactly one of them tokenizes, with a witness in either case                                                                                                  | Theorem 7                                  |
| Supply                | Anchors per kibibyte, gap tails and edit hulls for two BPE-derived maximal-munch vocabularies on held-out slices of the simdjson fixture, and the campaign's four C-like grammars whole, every reported quantity emitted by a shipped program or derived by the release gate from named emissions | Section 6                                  |

## 2 Setting

This section fixes the scan the paper reasons about, the boundaries it commits, and the certificate the later sections
decide.

A *token set* `T` is a finite nonempty set of nonempty byte strings; `L` denotes the maximum token length. The
*maximal-munch scan* of an input `x` proceeds from position `0`, and at position `c` it commits the longest member of
`T` that is a prefix of `x[c..]`, advances past it, and fails if no member matches. For distinct literal tokens the
longest match is unique, so the scan is deterministic. An input is *completely tokenizable* when the scan consumes it
whole; its *segmentation* is the resulting sequence of committed tokens, and its *boundaries* are the committed tokens'
start positions. A *regular token set* is a finite nonempty ordered family of empty-string-free regular languages over
bytes, scanned the same way. At each position the scan commits a longest word of any of the languages that prefixes the
remaining input, breaking equal-length ties by list priority, so the committed token is single-valued; a literal token
set is the case whose languages are singletons. Two facts about the scan are used throughout and are immediate from the
definition.

**Lemma 1 (memorylessness).** If `b` is a boundary of a completely tokenizable `x`, then the scan of the suffix `x[b..]`
taken as an input of its own commits exactly the tokens the scan of `x` commits from `b` on.

*Proof.* The scan's choice at any position depends only on the input from that position onward, and the scan of `x`
reaches `b` with no other state than the position itself. ∎

**Lemma 2 (boundaries confine matches).** If `b` is a boundary of a completely tokenizable `x` and `c < b` is a
boundary, then the token committed at `c` ends at or before `b`.

*Proof.* Committed tokens partition `x`, so the token at `c` ends at the next boundary after `c`, which is at most `b`.
∎

A byte `β` is *certified* for `T` when in every completely tokenizable input every occurrence of `β` begins a committed
token. A window `(W, o)`, with `0 <= o < |W|`, is a *certified window* when, at every occurrence of `W` in every
completely tokenizable input, the token covering the occurrence's final byte begins exactly `o` bytes into the
occurrence. The quantifier is per occurrence, so one conforming occurrence excuses no other. The certificate is
sufficient for a sound cut and stronger than a boundary at the origin. Over `T = {a, b}` every occurrence of `ab` may be
cut at offset `0`, since every byte is a token, yet `(ab, 0)` is not certified, because the token covering its final
byte begins one byte in, and the decider of Section 3 says so with `ab` as its witness. Which occurrence-based cut rules
beyond this certificate are always sound is not decided here. A *certified anchor* of a particular input is a position
that some certificate makes a boundary, taken together with the occurrence that witnesses it: for a certified byte, the
position of an occurrence of that byte, and for a certified window, an occurrence of the window together with its
origin. The witnessing occurrence is part of the anchor, and the edit theorem below conditions on where it lies. The
recovery companion proves these certificates sound for recovery, reading them after damage; this paper reads the same
definitions forward. Certification itself is decidable, which Section 3 establishes next, and Section 4 draws the
consequences of a certificate once decided.

## 3 Deciding certification

Certificates would be an idle notion if establishing one required the very quantification over all inputs it promises;
this section makes certification a decision, and its proofs are compact. Every construction behind the fixed-pair
decisions, the finite inventories, the anchor-free span and the auditor of Section 5 is implemented, except the
unrestricted-width corollary, whose factors-and-complement construction is proved and not built. Every lemma sits beside
an exhaustive small-universe sweep in the repository's exploration programs. Those sweeps, and every finite exhaustive
sweep this paper reports, test the implementations and the worked instances over the universe each one declares; what
establishes the general theorems is the written proofs, and no count in this paper is offered as evidence for a
statement quantifying over all token sets and all inputs.

**Lemma 3 (interior-byte characterization).** For a regular token set, a byte is certified exactly when every occurrence
of it in a word of any token language is initial.

*Proof.* If some token `t` carries the byte at a noninitial offset, the input `t` alone is completely tokenizable as the
single token `t`, whatever the priority, since no longer token is a prefix of `t`. That occurrence does not begin a
token. Conversely, every occurrence in a tokenizable input lies inside some committed token at the offset it has in that
token; if every such offset is zero, every occurrence begins a token. ∎

**Convention: non-nullable token languages.** Every regular token set in this paper is empty-string-free: no token
language contains the empty string, so every token language is non-nullable and every match consumes at least one byte.
What a nullable token breaks is not the choice. Finitely many token languages still have a longest matching prefix at
every position, and the empty match is one candidate among them, so the greedy rule remains well defined pointwise. What
breaks is progress. A committed zero-length match neither advances the scan nor closes a segment, so the process need
not terminate, and the marking representation loses its correspondence with positions because a boundary can repeat
without any byte between. This is a standing convention of the paper, fixed in the Setting and in force wherever a
regular token set appears, rather than a hypothesis each statement carries privately; the literal token sets of Section
2 are sets of nonempty byte strings by definition and satisfy it already. Ties between distinct token languages matching
the same longest prefix resolve by list priority, exactly as for literal sets, and the reference scans implement that
rule.

The window case is not syntactic, and its decision runs through an automaton view of the scan. Feed the input byte by
byte and keep the *pending tail*, the suffix since the last committed boundary. The commit of a token is final exactly
when the tail cannot be extended into a strictly longer token.

**Lemma 4 (pending-state invariant).** An unresolved pending tail is a proper prefix of some token. Hence the online
scan is a deterministic automaton whose states are the token prefixes, at most the sum of token lengths many, plus a
dead state; end of input resolves the tail by a plain scan of it, so acceptance is the predicate that the tail scans
clean.

*Proof.* If the tail is not a proper prefix of any token, no longer token can overtake the longest match at its front,
so that match is final, and either it commits and shortens the tail, or nothing matches and no extension ever can, the
dead state. The commit loop repeats until the tail is unresolved or empty. ∎

**Theorem 1 (window certification decides, with a completeness cutoff).** Let `T` be a token set with maximum token
length `L`, and let `(W, o)` be a window with `|W| = h`. A violating completely tokenizable input exists if and only if
one of length at most `N = h + 2L - 2` does. Certification of `(W, o)` is therefore decidable, and a product of the
pending automaton with an occurrence tracker decides it without enumerating inputs.

*Proof.* Two truncations, one at each end of a violation, and nothing between them needs pumping.

*The head.* Let `x` violate, with the occurrence at `p`, and let `b` be the greatest boundary of `x` at or below `p`. By
Lemma 1 the suffix `x[b..]` is completely tokenizable and its segmentation is the segmentation of `x` from `b` on, so
the occurrence sits at `p - b` with the same covering tokens and the violation survives. The token committed at `b`
covers position `p`, so its length is at least `p - b + 1`, giving `p - b <= L - 1`; at most `L - 1` bytes precede the
occurrence.

*The tail.* Work in that suffix, let `t` be the token covering the occurrence's final byte, and let `e` be `t`'s last
position. Truncate after `e`. The origin's own position needs no separate accounting, since a token covering it either
ends before the occurrence's final byte or covers that byte as well, and then it is `t`. Every token of the segmentation
beginning at or before `e` ends at or before `e`, so the truncated input is a whole prefix of that segmentation, and
maximal munch reproduces it. At each position the token the full input's scan takes lies entirely within the truncated
remainder, and deleting a suffix makes no longer token match. The truncated input is therefore completely tokenizable,
still contains the occurrence, and carries the same boundaries across it, so it still violates. It ends at a token end,
so end of input yields a complete scan, even when that token remains ambiguous until end of input.

*The length.* The occurrence begins at index at most `L - 1` and is `h` bytes long, so its final byte has index at most
`L + h - 2`. The token covering that byte begins at or before it and has length at most `L`, so `e <= 2L + h - 3` and
the truncated input has length at most `N = h + 2L - 2`.

Enumeration to `N` therefore decides certification, and the enumeration helper takes `N` itself. Tokens that resolve
only at end of input need no separate case. Over `T = {ba, bac}` with `W = a` and `o = 0`, the commit of `ba` is final
only when the input ends, since one further byte would let `bac` overtake it, and the truncation cuts after that token's
last position all the same. The product procedure decides certification without enumerating inputs by searching the same
composite space, the pending state beside the occurrence-match progress and the violation phase, and accepting a
violating configuration exactly when its pending state can still complete. The occurrence tracker it carries records the
designated occurrence's final-byte offset within the pending tail, initially at most `L - 1`, decreasing only as bytes
commit, and resolved at a streaming commit or at end of input, so the product has at most `|P(T)| (h + 1) (L + 2)`
configurations, `O(|P(T)| · h · L)`, where `P(T)` is the set of proper token prefixes, the empty one included. ∎

The cutoff is attained, so at these parameters it is tight rather than merely sufficient. Over `T = {aab}` with `W = ba`
and `o = 0`, the completely tokenizable inputs are exactly the repetitions of `aab`, and the shortest one containing
`ba` at all is `x = aabaab`. The occurrence begins at position 2, while the token covering its final byte begins at
position 3, one byte into the occurrence rather than at the origin, so `x` violates. Here `L = 3` and `h = 2`, so `N = h
+ 2L - 2 = 6`, which is exactly `|x|`, and no shorter tokenizable input contains the window at all.

The online route has a limit of its own. Over `T = {a⁺b, a}`, after reading `a^n` the end of the first token is still
undecided, falling after the first `a` if the input ends there and after the `b` if one follows, so no fixed lookahead
settles it, and a subsequential finite-state transducer, which emits its output irrevocably as it reads, cannot produce
the boundary stream at all. Certification still decides there, by verifying completed segmentations rather than
computing them.

**Definition 1 (armed-run automaton).** Fix a regular token set. The armed-run automaton reads an input together with a
boundary marking, one bit per position, from a distinct unconsumed initial state that no consumed byte revisits. At each
mark it starts a fresh run of every token automaton. A run is unarmed until the next mark, where the segment it has read
must be accepted by some token, and armed from then on; an armed accept at any later position rejects the marking, since
it exhibits a longer match the greedy scan would have taken. The automaton accepts when the final segment closes at end
of input and no run has rejected, and it accepts the empty marking in its initial state. Every nonempty marking it
accepts marks position zero, a mark is read before the byte at its position, and end of input closes the final segment.

**Theorem 2 (offline verifiability).** A boundary marking of an input, carrying boundary positions and no token
identities, is the boundary projection of the greedy segmentation of a completely tokenizable input exactly when the
armed-run automaton accepts it. The automaton's state space is finite. Consequently window certification is decidable
for arbitrary regular token sets.

*Proof.* Let the marking cut `x` into segments `s_1, ..., s_k` at boundaries `b_0 = 0 < b_1 < ... < b_k = |x|`. The
empty input is the case `k = 0`, where no token matches it, the languages being empty-string-free, so the greedy scan
commits nothing and ends complete, and the automaton accepts the empty marking in its initial state. That state is
distinct because the ordinary start configuration can recur, reading `ab` without a mark under the token language
`(ab)*a` restoring it, and accepting there would confuse the empty input with a nonempty one; the finiteness count below
includes it.

Soundness: suppose the automaton accepts. Each closure at `b_i` required some unarmed run started at `b_(i-1)` to
accept, so each `s_i` is a token. We show by induction that the greedy scan of `x` commits exactly `s_i` at `b_(i-1)`.
It suffices that no token strictly longer than `s_i` is a prefix of `x[b_(i-1)..]`. Were `t` such a token, the run of
`t`'s automaton started at the mark `b_(i-1)` would still be alive at `b_i`, be armed from that mark on, and accept at
`b_(i-1) + |t| > b_i`, an armed accept, contradicting acceptance. So `s_i` is the longest token prefix, the greedy scan
commits it, and since the final segment closes at `|x|`, the input is completely tokenizable with the accepted marking
as its greedy segmentation.

Completeness: the greedy segmentation of a completely tokenizable input passes both tests. Every segment is the
committed token, so every closure succeeds; and an armed accept at position `j > b_i` by a run started at `b_(i-1)`
would exhibit a token prefix of `x[b_(i-1)..]` of length `j - b_(i-1) > |s_i|`, contradicting the longest-match choice
at `b_(i-1)`.

Finiteness: an ordinary state is a set of configurations, one configuration being a token automaton's derivative paired
with an armed flag, and beside the ordinary states there is the single unconsumed initial state, which no ordinary state
equals. The derivatives are finitely many up to the standard associativity, commutativity, and idempotence quotient of
alternation, so the configurations are finitely many, the ordinary states are finitely many, and the accepted
marked-string language is regular. Certification of a pair `(w, o)` is then the emptiness of the intersection of this
regular language with the regular language of markings carrying a badly covered occurrence of `w`, decidable, with
violations pumping along the product's states. ∎

**Corollary 1 (computable anchor inventories).** For a regular token set and a declared window budget `H`, the certified
pairs `(w, o)` with `|w| <= H` are computable by one union-emptiness problem per pair over the armed-run product, with
no string enumeration and no cutoff.

*Proof.* This is immediate from Theorem 2, since a search over the product walks exactly the greedy segmentations of
completely tokenizable inputs containing the occurrence, one bit decides whether the covering token starts at the origin
when the window's last byte lands, and the product state space is finite, so breadth-first search terminates and its
emptiness is the certification. ∎

In the implementation the search is two branch searches, the occurrence-at-the-start branch and the later-occurrence
one. Each walks valid markings byte by byte, marks pruned to greedy by the armed simulation, with a nondeterministically
placed occurrence tracked through the window and one bit recording whether the latest mark sits at the origin. A
violation is exactly a reachable post-window state, bit clear, from which the input can end validly.

**Corollary 2 (existence at unrestricted width is decidable).** For a fixed regular token set the certified pairs form
an effectively regular language over the byte alphabet with one added origin marker, and so do the certified pairs whose
window occurs in some completely tokenizable input. Both languages have decidable emptiness, and a shortest member of
either is computable when one exists. Moreover certification is closed under extension on the left: if `(W, o)` is
certified then so is `(UW, |U| + o)` for every byte string `U`.

*Proof.* Encode a candidate as `u # v` with `W = uv`, `o = |u|` and `v` nonempty, over the byte alphabet extended by one
marker `#` that occurs exactly once. Theorem 2 gives the regular language `G` of valid boundary-marked inputs. The
factors of `G` are regular. Take the same automaton with every reachable state admitted as a start and every
co-accessible state as an accept; it accepts exactly the marked stretches that occur inside some completely tokenizable
input. Place the marker in such a factor and record, with a finite flag, whether the token covering the factor's last
byte begins at the marker, which holds when the mark on the first byte after the marker is set and every later mark in
the factor is clear. Keep the factors where the flag fails, project away the mark track, and the result is the regular
language `B` of encoded pairs having at least one badly covered occurrence in some completely tokenizable context.
Complement `B` within the regular set of well formed encodings `Σ* # Σ⁺`, writing `Σ` for the byte alphabet, and the
complement is the language of certified pairs, regular and effectively constructed. Emptiness, infinitude and a shortest
member are then the standard decisions on a finite automaton.

For the left extension, fix an occurrence of `UW` in a completely tokenizable input. It contains an occurrence of `W`
ending at the same byte, and the token covering that byte is the same token in both readings. Since `(W, o)` is
certified, that token begins at offset `o` within the occurrence of `W`, which is offset `|U| + o` within the occurrence
of `UW`. Every occurrence of `UW` arises this way, so `(UW, |U| + o)` is certified. Prefixing arbitrary strings then
produces certified pairs of every width above the first, so no upper bound on width carries information once a single
certificate is known and the question worth asking is existence. For the occurring language the restriction is by
intersection with the marked factor language of `G`'s byte projection, regular by the same operations; note that a
prefixed `UW` need not occur even when `W` does, so the left-extension closure is a statement about the unrestricted
reading. ∎

The two readings differ, and the difference is not a technicality. A window that occurs in no completely tokenizable
input is certified vacuously, so for any token set leaving a byte unused the unrestricted language is already nonempty
at width one. Existence is therefore worth deciding only in the occurring form. The measurements decide no existence
question; which reading each of their inventories reports, and which campaign certificates occur, is stated beside the
campaign rows in Section 6.

**Theorem 3 (the anchor-free span is decidable).** For a regular token set and a finite certified inventory, it is
decidable whether the anchor-free stretch, the longest run of positions carrying no inventory anchor over all completely
tokenizable inputs, is bounded, and the exact supremum is computable when it is.

*Proof.* First the empty inventory, which the statement admits and the construction below cannot start from, having no
longest window to buffer. With no certificate every position is anchor-free, so the question is only whether completely
tokenizable inputs are arbitrarily long, and the union of the token languages settles it in three cases. If that union
is infinite it contains words of unbounded length, each a completely tokenizable one-token input, so the stretch is
unbounded. If it is finite and nonempty, take a word `t` of maximum length; no token is longer, so the greedy scan of
`t^n` commits `t` each time, and those inputs are completely tokenizable of unbounded length. If it is empty, only the
empty input is completely tokenizable and the supremum is zero. An infinite regular language need not have a longest
word, which is why the first case cannot be folded into the second. Assume from here that the inventory is nonempty.
Write `M` for the longest window of the inventory, which is a different quantity from the maximum token length `L` and
is not bounded by it, and build the *credit product*: the armed-run verifier of Theorem 2, the last `M - 1` bytes read,
and one flag per buffered position recording whether some completed window has already credited it as an anchor. The
product is finite, being a verifier state, a bounded byte buffer, and a bounded flag vector.

*Flags settle before they leave.* A window `w` with origin `o` credits the position `o` bytes into its occurrence, and
it completes when its final byte is read, which is `|w| - 1 - o` positions later. That age is at most `M - 1`, so a
position is credited, if at all, no later than the step at which it reaches age `M - 1`. Taking that step's credits
before the position leaves the buffer, it leaves with its flag final, and no later byte can change it.

*Live states only.* Restrict the product to states reachable from the start and from which acceptance is still
reachable. Anchor-free stretches are measured on completely tokenizable inputs, and only live states occur in the runs
of such inputs, so nothing is lost and nothing spurious is admitted.

*The unbounded case.* Suppose the live subgraph, restricted to transitions whose exiting position leaves unanchored,
contains a cycle. Every live state lies on some accepting run, and pumping the cycle inside such a run yields accepted
runs with arbitrarily long anchor-free stretches, so the stretch is unbounded. Conversely, if the stretch is unbounded
then some accepting run carries an anchor-free stretch of exits longer than the number of live states, the buffered tail
past the last exit contributing at most `M - 1` more positions, and the stretch's exits repeat a state, closing such a
cycle. Bounded and acyclic therefore coincide, and cycle detection decides which holds.

*The bounded case.* With that subgraph acyclic, let `r(q)` be the longest anchor-free run of exits ending at the live
state `q`. The recurrence runs over incoming labeled transitions `p → q`, each contributing a candidate: `r(p) + 1`
where the transition's exiting position leaves unanchored, `0` where it leaves anchored, and `r(p)` during warm-up,
where the transition exits no position at all; `r(q)` is the largest candidate over all incoming transitions, so a state
carrying both anchored and unanchored incoming edges is not forced to either. Only the unanchored-exit subgraph is
acyclic; the whole live graph is not, since anchored exits may close cycles. Relaxation still settles. A warm-up
transition strictly increases the buffer's occupancy and no transition decreases it, so none lies on a cycle. Every
cycle therefore consists of exits and contains an anchored one, which resets the run to zero, since a cycle of
unanchored exits would lie in the subgraph the bounded case has just made acyclic. Run lengths are thus bounded by the
number of live states, and the start state's value is initialized to zero. A stretch of an accepted input either ends
inside the input, where it is some `r(q)`, or runs into the end, where the buffered flags supply the correction. At an
accepting state the stretch continues through the oldest contiguously unanchored buffered positions, and a stretch lying
wholly inside the buffer is the longest unanchored run among the flags. The supremum is the maximum of the three,
attained by the accepting run that realizes it. ∎

Over the small universes every bounded verdict equals the longest anchor-free stretch found by enumerating all
tokenizable strings to length twelve, which checks that the computed supremum is exact rather than merely valid, and
every unbounded verdict is matched by a measured stretch that reaches the enumeration's own limit, which checks that
verdict for consistency.

The density verdict is the design lesson made formal. The UTF-8 shape's inventory yields the bound three, computed
rather than recalled, while a miniature C-like grammar of identifiers, numbers, spaces, and an operator is refused a
bound outright, its unboundedly long identifiers being exactly the anchor-free payload no inventory can cover. A grammar
buys a synchronization guarantee with designed delimiters or bounded tokens, and this theorem prices that purchase from
the grammar alone.

The instance a format designer reads as a resynchronization distance is the longest run of a valid stream that carries
no certified byte, and it needs no buffer at all.

**Corollary 3 (synchronization distance is computable).** For a token set `T`, the supremum over completely tokenizable
inputs of the longest run carrying no certified byte is computable: it is unbounded exactly when the subgraph of the
pending automaton's live states using only non-certified bytes has a cycle, and otherwise the longest path through that
subgraph.

*Proof.* This is Theorem 3 with the certified bytes as the inventory: `M = 1`, the buffer empty and an exit credited
exactly when its byte is certified, or its empty-inventory case when no byte is certified. The form of the statement
over the pending automaton of Lemma 4 is the direct argument. A run avoiding certified bytes can begin after any prefix,
so every completable state starts one; each such run traces quiet transitions between completable states, and conversely
every such path is realized by some tokenizable input, extending the path's string by any clean completion. Longest path
in a finite graph is computable, and unboundedness is exactly a reachable quiet cycle. ∎

The worked example is the UTF-8 shape over byte classes: one ASCII class, three lead classes, one continuation class,
with tokens the four sequence shapes. Lemma 3 derives the code's celebrated self-synchronization in two lines, the
certified bytes being exactly the ASCII and lead classes, and Corollary 3 computes the synchronization distance three,
the resynchronize-within-three-bytes property, as an output of the instrument rather than folklore. The name is borrowed
from code synchronization, where the delay of a code is counted in codewords and assumes unique decipherability
(Restivo, Theoretical Computer Science 1975); the distance here is in bytes and is computed for a token set that need
not be a code. Run forward, this is a design-time metric. A proposed format's certificate inventory and gap bound give a
guaranteed resynchronization distance after a mid-stream join of a valid stream, and under corruption exactly where the
damaged stream remains completely tokenizable; a stream damaged past validity is the recovery companion's subject, whose
repair model this instrument feeds. Inventory and bound are both computed before the format ships.

## 4 Splitting theorems

This section draws the consequences of a certificate, and Section 6 measures them: a cut at boundaries is exact, so a
certified position is a sound cut; an edit's damage stays strictly between the certified anchors witnessed in the
unchanged bytes on either side of it; and splitting at a delimiter is sound exactly when the delimiter is token-initial,
splitting after it exactly when it is token-final.

**Lemma 5 (cuts at boundaries are exact).** Let `T` be a token set, `x` completely tokenizable under `T`, and `B` a set
of boundaries of `x`. Scanning each segment of `x` between consecutive members of `{0} ∪ B ∪ {|x|}` independently yields
segmentations whose concatenation equals the sequential segmentation of `x`.

*Proof.* Fix consecutive cut points `u < v` from `{0} ∪ B ∪ {|x|}` and consider the chunk `y = x[u..v)`. We show by
induction along the chunk that the chunk-local scan of `y` commits, position for position, the tokens the sequential
scan commits inside `[u, v)`.

Both scans stand at `u`, a boundary. Suppose both stand at a common position `c` with `u <= c < v`, a boundary of the
sequential scan. The sequential scan commits the longest member of `T` prefixing `x[c..]`; call it `t`. The token `t`
ends at or before `v`, trivially when `v = |x|`, and by Lemma 2 applied to the boundary `v` when `v ∈ B`. So `t` is also
a prefix of `y[c-u..]` and is available to the chunk-local scan. Conversely every candidate available to the chunk-local
scan is a prefix of `x[c..]` and hence available to the sequential scan, so no chunk-local candidate is longer than `t`.
For distinct literal tokens the longest match is unique, so the two scans commit the same token and advance to the same
position. The induction reaches `v` exactly, since the sequential boundaries inside `[u, v)` partition the chunk, so the
chunk-local scan consumes `y` completely and its boundaries are the sequential ones shifted by `u`. Concatenating over
all chunks yields the claim. ∎

**Theorem 4 (certified positions split soundly).** Let `T` be a token set, `x` completely tokenizable under `T`, and `A`
a set of certified anchor positions of `x`. Scanning each segment of `x` between consecutive members of `{0} ∪ A ∪
{|x|}` independently yields segmentations whose concatenation equals the sequential segmentation of `x`.

*Proof.* Every anchor `a ∈ A` is a boundary of the sequential segmentation, by the defining guarantee of its certificate
applied to the tokenizable input `x` itself, so Lemma 5 applies. ∎

What the certificate adds to the lemma is that its hypothesis is established before any input exists, so a cut set drawn
from a certified inventory is exact on every completely tokenizable input.

The preceding results are stated for literal tokens, and the next corollary extends them to the regular token sets
defined in Section 2.

**Corollary 4 (splitting at the regular generality).** Lemmas 1, 2 and 5 and Theorem 4 hold verbatim for regular token
sets, so a cut at boundaries is exact there too, and a certified position splits soundly.

*Proof.* The proofs of the three lemmas use three properties of the scan and nothing else: its choice at a position
depends only on the suffix from that position, its committed tokens partition the input, and that choice is the optimum,
under a fixed preference, of the token members prefixing the remaining input. Shortening that remainder can remove a
candidate but never promote one over a winner that still fits, which is what the chunk-local step turns on. The third is
not implied by the first two, and it is what the chunk step needs. A rule that commits the shortest match when exactly
three bytes remain satisfies the first two, yet its chunk-local scan disagrees with its sequential one at a cut that is
a boundary of the input, over `{a, aa}` on `aaaaa` cut at `3`. All three hold for a regular token set as the Setting
defines it, the priority order making the optimum single-valued, so the arguments transfer unchanged, and Theorem 4
follows from Lemma 5 as before. Uniqueness of the committed token is where the transfer wants saying out loud, since
several token languages may match the same longest prefix. The two scans see the identical family of candidates ending
at or before the cut, longest ones included, and resolve a tie among equal-length candidates by the same list-priority
rule, so they commit the same token and not merely the same length. ∎

Under a common length bound `L` on the token languages, the edit's lookahead shadow is `(p - L, p]`, the positions whose
commits may have read the edited byte. No commit made at or below `p - L` can have read it, which gives an edit's damage
a low edge that no anchor has to supply. The first boundary past `p - L` is `0` or ends a token committed at or below `p
- L`, so both inputs share it and the damage lies above `p - L + 1`. Regularity plays no part in the edge, since a
common length bound on the token languages is all the argument uses, and for a regular token set whose languages hold
words of unbounded length the argument supplies no such edge. The edge that survives on both sides is a certified anchor
witnessed in bytes the edit left untouched, which its certificate makes a boundary of both inputs.

**Theorem 5 (edit damage is confined between witnessed anchors).** Let `T` be a regular token set, `x` completely
tokenizable under `T`, and `x'` a single-byte substitution at position `p` that is completely tokenizable under `T`. Let
`a` be a certified anchor position of `x` whose witnessing occurrence lies wholly in `[0, p)`, or `a = 0`, and `q` a
certified anchor position of `x` whose witnessing occurrence lies wholly in `[p+1, |x|)`, or `q = |x|`. Every boundary
present in exactly one of the two segmentations lies strictly between `a` and `q`. When an integer `L` bounds the length
of every word of every token language of `T`, and in particular when `T` is a literal token set with maximum token
length `L`, every such boundary lies in `(max(a, p - L + 1), q)`.

*Proof.* Low side, from the anchor. When `a = 0` it is a boundary of both segmentations by definition and nothing lies
below it. Otherwise the occurrence behind `a` lies in bytes the two inputs share, so its certificate makes `a` a
boundary of both segmentations. By Corollary 4 applied to each input with the single cut `a`, each segmentation
restricted to `[0, a)` is the chunk-local scan of its own first `a` bytes, and those byte ranges are identical, so the
two segmentations agree on every boundary below `a`, and both carry `a` itself.

Low side, from the shadow. Let `L` bound the length of every word of every token language of `T`. The two inputs agree
on `[0, p)`. We show the two scans commit identical tokens at every common boundary `c <= p - L`. The choice at `c` is
decided by the words of length at most `L` that prefix the remaining input, all inside `[c, c + L) ⊆ [0, p)`, where the
inputs agree, so the same words match, the same one wins under the same priority, and the committed token and the next
boundary coincide. By induction from `0`, the boundary sequences of `x` and `x'` are identical up to and including the
first boundary `d` exceeding `p - L`. Both scans therefore carry `d`, so their boundaries in `[0, d]` coincide, and `d
>= p - L + 1`. Hence no boundary at or below `p - L + 1` belongs to exactly one segmentation, and with the previous
paragraph none at or below `max(a, p - L + 1)` does.

High side. If `q = |x|`, no boundary lies at or beyond `q` and nothing is claimed there. Otherwise the occurrence behind
`q` lies in the unchanged suffix `[p+1, |x|)`, so it is present in both inputs, and both are completely tokenizable. The
certificate therefore makes `q` a boundary of both segmentations. By Lemma 1, which Corollary 4 carries to regular sets,
each segmentation from `q` on is the segmentation of the suffix from `q`. The two suffixes are byte-identical, so the
segmentations agree on every boundary at or beyond `q`. What remains is `(a, q)`, and under the length bound `(max(a, p
- L + 1), q)`. ∎

The greatest such `a` and the least such `q` give the tightest window; any qualifying pair gives a valid one, which is
what the budget-limited inventories of Section 6 report.

**Theorem 6 (the delimiter-splitting criteria, a dual pair).** Let `T` be a regular token set and `δ` a byte of its
alphabet. Splitting every completely tokenizable input *at* each occurrence of `δ` and scanning the pieces independently
preserves the segmentation if and only if `δ` occurs in tokens only at position zero. Splitting *after* each occurrence
preserves it if and only if `δ` occurs in tokens only at final positions.

*Proof.* At-splitting. If `δ` occurs in tokens only at position zero, then `δ` is a certified byte by Lemma 3, since
every occurrence in a tokenizable input lies inside some committed token at the offset it has in that token, and every
such offset is zero. Theorem 4, carried to regular sets by Corollary 4, then gives preservation. Conversely, if some
token `t` carries `δ` at offset `j > 0`, the input `t` alone is completely tokenizable as the single token `t`, whatever
the priority, since no longer token can be a prefix of `t`; cutting at every occurrence of `δ` in `t` produces chunks
whose cut points include `j`, and there may be more than two of them when `t` carries `δ` more than once. If any chunk
fails to scan, the split scan fails on an input the sequential scan consumes; if every chunk scans, the concatenated
chunk segmentation contains a boundary at `j`, a token start of its own chunk, that the sequential segmentation of the
single token `t` lacks. Preservation fails either way, and the dichotomy is over all the chunks the split produces
rather than over a pair.

After-splitting. If `δ` occurs in tokens only at final positions, every occurrence in a tokenizable input is the last
byte of its committed token, so the position after it starts the next token and is a boundary, except when the
occurrence is the input's last byte, where that position is `|x|` itself. That is not a token start and is not claimed
to be one; it is the endpoint the cut set of Lemma 5 already carries, and it opens no chunk. The lemma, at the regular
generality of Corollary 4, then gives preservation. Conversely, if some token `t` carries `δ` at a non-final offset `j`,
the single-token input `t` is cut at `j + 1`, mid-token, and fails preservation as before. ∎

## 5 The differential auditor

This section compares two token sets with the verifier of Theorem 2. Two such verifiers over a common input decide
whether the sets ever place a boundary differently, and a determinized projection of each decides whether they ever
disagree on whether an input tokenizes at all.

**Theorem 7 (segmentation differentials are decidable for regular token sets).** Given regular token sets `T_1` and
`T_2`, it is decidable whether some input completely tokenizable under both receives different boundary sets, and
likewise whether some input is completely tokenizable under exactly one of the two; in each case a witness input is
computable when one exists.

*Proof.* Run both of Theorem 2's armed-run verifiers in product over a common input, each side nondeterministically
guessing its own marking, with a flag set when a position is marked by exactly one side. The armed pruning confines each
side's guesses to its greedy segmentation. Any other guess is rejected by an armed accept or an unclosable segment, so a
run that survives feeds the two true boundary sets position by position. Accept when the input can end, both sides
closing a final segment, with the flag set; such a run exhibits an input both sets tokenize whose boundary sets differ,
and conversely any such input yields an accepting run by guessing both true markings, the flag set at the first boundary
they dispute. The product state, two derivative-set pairs and one flag, is finite, so the search terminates, and
reconstructing the fed bytes from an accepting run yields the witness.

The second predicate needs a different construction, because it asserts non-membership on one side and a
nondeterministic pair product cannot. A run failing to accept is not a proof that no run accepts. Project each side's
verifier to an automaton over bytes alone, existentially in the mark bits, so it accepts exactly the inputs that side
tokenizes completely; that projection is a nondeterministic byte automaton, and determinizing it by the subset
construction gives a deterministic one whose state is the set of marking configurations still alive after the bytes
read. Non-membership is then decidable position by position, since the input read so far is outside the side's domain
exactly when no configuration in the current subset is accepting at the current end of input. Search the product of the
two subset automata for a state accepting on exactly one side. The state space is finite, being pairs of subsets, so the
search terminates, and parent pointers reconstruct the witness. ∎

The security reading is direct. The auditor decides two predicates, each with a witness: boundary disagreement on inputs
both sets tokenize completely, from the product of the two armed-run verifiers, and domain difference, an input that
exactly one side tokenizes at all, from the product of their determinized byte projections. Request smuggling, polyglot
inputs, and log mangling are one stream read differently by two implementations; at the lexical layer the auditor either
proves a pair of token sets free of both differentials or produces the diverging input. What neither predicate observes
is token identity where the boundaries coincide, or priority ties among them; that comparison is outside this
instrument.

The auditor also decides the recovery campaign's own design change. The conventional and split-friendly grammars of
Section 6 differ exactly in their whitespace treatment, and the auditor synthesizes the minimal disagreement: the
witness `SN`, a space then a newline, one whitespace token under the conventional row with boundary set `{0}` and two
tokens under the split-friendly row with boundary set `{0, 1}`. The conventional row against itself is proved
differential-free over every input, not merely over a corpus, and that is the shape of guarantee no finite test suite
provides.

What each of these decisions costs is a property of its construction, and the four constructions are worth reading
beside one another. Table 2 collects the space each one searches; every entry is read off the proof that introduces it,
not measured, and each row is a reachability question over the space named, not a running time. The literal product of
Theorem 1 carries a pending state, the occurrence match's progress through the window, and the violation phase, and its
tracker offset ranges to `L`. Over `T = {a^L}` with the window `a` at origin zero the search visits `1 + L(L + 1)/2`
product states before refusing, 37 at `L = 8` and 8,257 at `L = 128`, which no bound independent of `L` admits. The
literal route also admits plain enumeration to length `N = h + 2L - 2`, which the witness of Section 3 shows is
attained. The armed-run product of Theorem 2 and Corollary 1 carries a verifier state and the same occurrence progress,
once per pair. The inventory rows separate three quantities: the exact number of certification decisions, one per
(window, origin) pair through the budget; the peak space of one decision, a window of width at most `H`; and the
aggregate space of all of them, a width-`j` decision costing its row's bound at `h = j`. The credit product of Theorem 3
adds the byte buffer and its flag vector. Its row is written as a sum so that it stays one for an empty alphabet, where
the product keeps its start state. The quantity `M` is defined only for a nonempty inventory, which is why the empty
inventory has its own row, decided before any product is built by whether the token-language union is empty. If it is,
only the empty input tokenizes and the stretch is `0`; if it is nonempty, tokenizable inputs grow without bound and so
does the stretch. The two differentials of Theorem 7 are the only place a subset construction appears, since deciding
that an input lies outside one side's domain is a non-membership question a nondeterministic product cannot answer.

**Table 2.** The space searched by the decisions of Sections 3 and 5, read off their constructions rather than measured.
Here `P(T)` is the set of proper token prefixes of a literal token set, the empty prefix included, so that `|P(T)|`
counts the pending automaton's live states, the proper prefixes of tokens; `L` is the maximum token length, `h` the
window length, `Σ` the alphabet, `V` the armed-run verifier's state count for a regular family, `M` the longest
inventory window, and `H` the declared window budget.

| Decision                                     | Construction                                                    | Search-space bound                                                                 |
|----------------------------------------------|-----------------------------------------------------------------|------------------------------------------------------------------------------------|
| Window certification, literal                | pending automaton, occurrence, phase, tracker                   | `O(\|P(T)\| · h · L)`                                                              |
| Window certification, regular                | armed-run product, per `(w, o)`                                 | `O(V · h)`                                                                         |
| Anchor inventory through budget `H`, literal | `sum_{j=1}^{H} j \|Σ\|^j` certification decisions, one per pair | peak `O(\|P(T)\| · H · L)`, aggregate `O(\|P(T)\| · L · sum_{j=1}^{H} j² \|Σ\|^j)` |
| Anchor inventory through budget `H`, regular | the same count of decisions, each the regular row               | peak `O(V · H)`, aggregate `O(V · sum_{j=1}^{H} j² \|Σ\|^j)`                       |
| Anchor-free span, nonempty inventory         | credit product                                                  | `O(V · sum_{k=0}^{M-1} (2\|Σ\|)^k)`                                                |
| Anchor-free span, empty inventory            | emptiness of the token-language union                           | the union's reachable states                                                       |
| Boundary differential                        | two verifiers in product, one flag                              | `O(V_1 · V_2)`                                                                     |
| Domain differential                          | subset determinization of each projection                       | `O(2^{V_1} · 2^{V_2})`                                                             |

## 6 Measurements

This section measures, on named material, how many anchors certificates supply, how a frozen inventory transfers, how
far an edit's damage reaches, and what the four campaign grammars offer. It applies the decisions of Section 3, the
splitting and edit theorems of Section 4, and the auditor of Section 5.

Two byte-pair vocabularies, named below, are here because the supply study needs a token set of their shape. Anchor
supply is defined for every token set and is measured on the grammars of the recovery companion's measurement campaign
too; what the supply study needs besides is a token set whose literal tokens overlap in bulk, hundreds to thousands of
strings sharing prefixes, with a corpus they were drawn from and a held-out slice beside it. No lexer grammar in this
study or in the campaign has that, and the two byte-pair vocabularies studied here have it, while the campaign's four
grammars, measured at the end of this section, are the other shape the decisions meet in practice.

The first study reads two vocabularies over a held-out byte slice of the simdjson benchmark fixture. Every quantity is a
byte quantity, and the fixture is read through the byte-to-character bijection, so slices, windows, and substitutions
never touch code points. That fixture is `twitter.json`, taken verbatim at 631,515 bytes and byte-identical to
`jsonexamples/twitter.json` at that project's release v3.10.1 (the simdjson repository). It is a published benchmark
fixture rather than a collection made for this work, fetched by the artifact from that release and held to its digest
rather than redistributed, and it is used here only as a byte stream. The JSON was not parsed or semantically inspected.
The vocabularies are local-384, a byte-pair vocabulary of 384 merges trained here on the fixture's first 64 KiB over the
full 256-symbol fallback base, and GPT2-prefix-4k, the first 4,096 merges of the GPT-2 byte-level vocabulary over its
full byte alphabet, nothing pruned to the sample. Both are read as maximal-munch token sets, not with merge-rank
tokenization, which is what makes them objects this theory speaks about. The distinction is between provenance and rule:
the token strings come from BPE merge tables, and native BPE applies those merges by rank, while this experiment
discards rank at scan time and reads the strings as a maximal-munch vocabulary. Every figure below is therefore a figure
of that policy over those strings and not a measurement of BPE's own segmentation. The local training merges the most
frequent adjacent pair at each step, resolving ties by the shorter concatenation and then by the lexicographically
greatest pair. This is not the first-occurrence rule, and the difference identifies the vocabulary rather than
decorating it, since 251 of the 384 steps had a most-frequent tie and 185 of those would have merged a different pair
under first occurrence. The evaluation slice is the byte range `[65536, 81920)`, exactly 16 KiB, adjacent to the
training bytes. The inventory per vocabulary is the exact certified-byte set, every alphabet byte decided by the
interior-byte lemma and cross-checked against the window decider, together with every window of width two, three, or
four that occurs in that slice, each decided at every origin. Because the candidates are drawn from the slice the supply
is then measured on, what these rows report is a ceiling measured after the fact, exact through the budget on this
slice, and not the supply an inventory frozen before the input would deliver; the two are separated below. Table 3 gives
the supply, counting distinct internal anchor positions rather than witnessing certificates, together with the gaps
between consecutive anchors and the snap, the distance an ideal eight-way cut moves to reach its nearest anchor.

**Table 3.** Certified-anchor supply of the two vocabularies on the evaluation slice, emitted by
`splitting_measurements.py`. Gap rows here are distances between consecutive anchor positions, so the run before the
first anchor and the run after the last are excluded; those runs are counted by the anchor-free stretch reported in the
text.

| Vocabulary     | anchors/KiB | gap p50 | gap p90 | gap max | snap max (8) |
|----------------|------------:|--------:|--------:|--------:|-------------:|
| local-384      |       242.1 |       1 |      14 |      61 |            9 |
| GPT2-prefix-4k |       651.2 |       1 |       2 |      11 |            1 |

Both vocabularies supply anchors densely, and the difference between them is a factor rather than a gulf. The trained
vocabulary yields 242.1 anchors per kibibyte and the external one 651.2, a ratio of about 2.7 on this slice. The trained
vocabulary is behind on the tail statistics too, its longest anchor-free stretch running to 60 bytes against the
external one's 10 and its eight-way cuts snapping within 9 bytes against 1, while the two agree at the median gap of one
byte. Position by position, 2,937 positions are anchors under both vocabularies, 937 under the trained one alone, 7,482
under the external one alone, and 5,027 under neither, of the slice's 16,383 interior positions, a Jaccard index of
0.259 between the two anchor sets. This is a paired comparison of two named objects on one named slice and it is
reported as such. It is consistent with frequent evaluation substrings having become token interiors under training,
which is the mechanism one would expect, but it cannot identify that mechanism, since the two policies differ in
provenance, merge depth, training corpus, and construction at once, and they are read on the same evaluation block.

One of those four differences, merge depth, can be varied alone, and is. The series holds the training corpus, the
evaluation slice, the fallback base, the scan convention, and the window budget fixed, and moves only the number of
merges. Supply falls monotonically: 508.6 anchors per kibibyte at 48 merges, 420.6 at 96, 329.4 at 192, and 242.1 at
384, a factor of 2.10 across that range, while the longest anchor-free stretch grows from 21 bytes to 60. Along this
local byte-pair trajectory the four measured checkpoints have lower anchor supply at greater depth. What that licenses
is narrower than the shape of the curve suggests. The four points span 48 to 384 merges; GPT2-prefix-4k lies an order of
magnitude outside that range at 4,096 merges and differs from every one of the four checkpoints in provenance, training
data, and construction as well, so this series neither estimates nor rules out a depth contribution to the
cross-vocabulary gap. What it does is cut against a simple monotone depth-only account of that gap, since the deeper
vocabulary supplies 2.7 times more rather than less; it does not exclude nonlinear or family-specific depth effects, and
a depth effect that saturates or reverses somewhere between 384 and 4,096 merges is consistent with everything measured
here. Which of the varied axes, depth, provenance, training domain, construction, or their interactions, produces the
gap is not identified here; that would need matched depths across several training domains, which this section does not
run.

One slice-wide figure characterizes neither vocabulary on its own. Across the sixteen kibibyte blocks the slice divides
into, the trained vocabulary supplies between 131 and 448 anchors per block with a median of 242, and the external one
between 537 and 756 with a median of 688. Each anchor is detected once on the full slice and binned by its position;
percentiles here and throughout this section are the order statistic at `⌊qn⌋`, an upper median for even counts. The
ranges do not meet, which is what makes the comparison above worth reporting, and each is wide enough that a single
aggregate would hide it.

The inventory is exact through a declared window budget, and stating it that way is what makes the numbers trustworthy.
Every window of widths two through four occurring anywhere in the slice is decided, 20,690 decisions over 6,307 windows
for each vocabulary, so the supply is the slice's own, not a sample of it. The budget curve shows what each width buys:
through `H = 1`, the certified bytes alone, the trained vocabulary supplies 52.2 anchors per kibibyte and the external
one 379.8; widths two, three, and four carry the trained one to 208.9, 230.7, and 242.1, and the external one to 608.8,
620.9, and 651.2. What each width bought and what it cost are worth reading together, since the second is what a wider
budget would charge. Width two is where almost everything is, and it gains 2,507 anchor positions on the trained
vocabulary and 3,665 on the external one for 2,316 decisions. Width three gains 349 and 193 for 6,666 decisions, and
width four gains 183 and 485 for 11,708. The curve has not flattened at the budget, and on the external GPT-2 vocabulary
width four gains more than width three did, so what lies past `H = 4` is not estimated here.

What the rows above are, then, is a ceiling: what an inventory chosen with the input already in hand could certify
through the budget. A deployment fixes its inventory first and meets the input afterwards, and the difference is
measurable rather than a matter of framing. The frozen inventory is a hybrid: every certified byte, which the
interior-byte lemma settles from the token set alone, together with every certified window of width two to four that
occurs in the adjacent 16 KiB, the byte range `[81920, 98304)`, which is disjoint from both the training bytes and the
evaluation slice. That inventory is applied out of sample, on the evaluation slice. There the trained vocabulary
supplies 209.4 anchors per kibibyte against its ceiling of 242.1, which is 86.5 per cent of it, and the external one
608.6 anchors per kibibyte against 651.2, or 93.5 per cent. Aggregate supply, on this one adjacent-slice transfer, costs
13.5 and 6.5 per cent. The tail is where the external vocabulary pays. Its longest anchor-free stretch grows from 10
bytes to 35, while the trained vocabulary's is unchanged at 60 bytes although it loses the larger share of aggregate
supply, so the vocabulary that loses least on average loses most where it matters to a worker waiting for a cut. One
slice-wide share hides its own spread the same way one supply figure does, so the transfer is also reported as a
distribution, and Table 4 gives, over the sixteen kibibyte blocks the evaluation slice divides into, the range and upper
median of the share each block keeps under the frozen inventory. These are held-out figures from a single calibration
split, not guarantees; a deployment that needs a worst-case bound should read the grammar-global anchor-free span where
one exists, and a deployment estimate would want several disjoint, preferably cross-document calibration folds, which
this section does not run.

**Table 4.** Share of each evaluation block's ceiling supply retained by the frozen hybrid inventory, in per cent. Its
width-two-to-four component was calibrated on the disjoint slice; figures are emitted by `frozen_inventory.py`.
Percentiles here are the order statistic at `⌊qn⌋`, an upper median for the even count, as throughout this section.

| Vocabulary     | lowest block | upper median | highest block |
|----------------|-------------:|-------------:|--------------:|
| local-384      |         71.0 |         88.5 |         100.0 |
| GPT2-prefix-4k |         85.0 |         94.0 |         100.0 |

The exactness matters because a sampled inventory of the same slice misses many anchors, especially for local-384. The
candidate set is sixty frequent slice n-grams, the twenty most frequent of each width. Every occurrence of every n-gram
of that width is counted, the final one included; distinct n-grams are ranked by that count; and frequency ties are
broken by first occurrence in the slice. The rule has to be stated because the rank-20 selection boundary is tied. At
each of the three widths the twentieth frequency is shared by five candidates, so "the twenty most frequent" names a
family of candidate sets and not one. The trained vocabulary, over all 250 selections consistent with those ties,
certifies exactly one pair whichever is chosen while the external one certifies between 35 and 38. Its 38 is therefore a
figure of the stated tie rule as much as of the candidate count, and the trained vocabulary's 1 is not. What the tie
moves is the external count and not the supply behind it. Across all 250 selections the anchors per kibibyte the
selected pairs yield are invariant, in the wider component alone and again counted as distinct positions together with
the certified bytes, so the two per-kibibyte figures below are properties of the candidate count and the widths rather
than of the tie break; the external pair count is not. Those sixty candidates yield a single certified pair for the
trained vocabulary and 5.6 anchors per kibibyte of their own, against the 189.9 the exhaustive inventory's wider
component adds beyond the certified bytes on the same slice. The same sixty candidates cost the external GPT-2
vocabulary far less, yielding 38 certified pairs and 370.9 anchors per kibibyte. Counting distinct positions over the
certified bytes and the sampled pairs together, the certified bytes being exact under either candidate set, the trained
vocabulary's sampled inventory comes to 57.8 anchors per kibibyte against the exhaustive 242.1, 23.9 per cent of it, and
almost all of that is the 52.2 the certified bytes supply on their own. The external vocabulary's comes to 427.4 against
651.2, 65.6 per cent of it; its certified bytes supply 379.8 alone and its sampled pairs 370.9, the two sets of
positions overlapping heavily. The emitted stats lines call this count the sampled half added to the certified bytes;
the figure they carry is the count of distinct positions. That pattern is consistent with the reading that the material
a corpus repeats is material a vocabulary trained on that corpus has absorbed into its token interiors, and that a
vocabulary trained elsewhere has not, but it does not establish it. A failed certificate for an n-gram is a statement
that some completely tokenizable context places a boundary other than where the window demands; it does not exhibit that
n-gram inside a token of the trained vocabulary. Nor do the two vocabularies differ in alphabet coverage, since both
carry all 256 fallback bytes, so every byte string is tokenizable under either. What differs is the token inventory
above that base, the provenance of the merges that built it, and which byte sequences occur inside tokens rather than at
their starts. Any of these moves a certification verdict on its own.

Incremental damage is measured over 200 random byte substitutions per vocabulary. The draws come from seed 20260826,
reset per vocabulary, with positions uniform over the slice and replacement bytes uniform over the 256-byte alphabet. A
draw that repeats the original byte or leaves the input untokenizable is redrawn. Of 201 attempted draws 200 were
accepted; the one rejection repeated the original byte, and none was rejected for failing to tokenize, since the
fallback base makes every byte string tokenizable. The bounding anchor in every trial is the least eligible anchor the
declared `H <= 4` inventory supplies, its witnessing occurrence lying wholly in the unchanged suffix, eligibility
decided per witness before the minimum is taken. It is not the least such anchor over all widths, because a certificate
wider than the budget can witness an earlier eligible anchor, and a probe one width past the budget finds one in 2 of
the 200 trials on the trained vocabulary and 1 on the external one. That one-width probe is incomplete and does not
determine the global least, though every qualifying anchor it finds is a valid, possibly nonminimal, confinement bound;
for a fixed trial the global least is decidable, by testing every substring of the unchanged suffix at every origin,
since every witnessing occurrence lies wholly in that suffix, and this paper does not run that search. What the budget
makes inventory-relative is the slack, which is therefore an upper bound on the global slack rather than the global
figure.

An accepted edit has two outcomes, and reporting them as one distribution hides the larger of the two. Table 5 reports
both symmetrically across the vocabularies: how often an accepted substitution moves nothing at all, the damage
conditional on it moving something, the unconditional mixture that carries the trials which moved nothing as an atom at
zero, and the count of boundaries actually moved. Every hull there, the inclusive span from the first moved boundary to
the last, is inside Theorem 5's window. The two vocabularies are paired within each of the 200 trials rather than
compared as two aggregates, and the paired difference, trained minus external, has median 3 bytes and ninetieth
percentile 13 on the hull, and 11 and 25 on the admissible width. That is a statement about the paired distribution and
not about individual trials. The hull difference is positive in 144 of the 200 trials, zero in 38, and negative in 18,
so the trained vocabulary's hull is the larger one usually rather than always. Whether an edit moves anything at all is
paired the same way. The same substitution moves a boundary under both vocabularies in 106 of the 200 trials, under the
trained one alone in 63, under the external one alone in 12, and under neither in 19. Means in the table are printed to
three decimals; the unconditional means, whose denominator is 200, are exact at that precision.

**Table 5.** Incremental damage from one edit seed, every figure emitted by `splitting_measurements.py` into
`data/splitting-stats.txt`. The hull is the inclusive span from the first moved boundary to the last. A trial that moves
nothing has no first and no last moved boundary and is recorded with hull zero, which is a convention and not a
measurement, so the unconditional rows are percentiles of a mixture carrying that atom.

| 200 accepted substitutions per vocabulary      | local-384 | GPT2-prefix-4k |
|------------------------------------------------|----------:|---------------:|
| trials moving no boundary                      |        31 |             82 |
| trials moving a boundary                       |       169 |            118 |
| conditional hull median                        |         6 |              1 |
| conditional hull ninetieth percentile          |        13 |              2 |
| conditional hull maximum                       |        20 |              7 |
| conditional hull mean                          |     6.852 |          1.508 |
| unconditional hull median                      |         4 |              1 |
| unconditional hull ninetieth percentile        |        13 |              2 |
| unconditional hull maximum                     |        20 |              7 |
| unconditional hull mean                        |     5.790 |          0.890 |
| boundaries moved, mean conditional on a change |     3.882 |          1.415 |
| boundaries moved, mean per accepted trial      |     3.280 |          0.835 |

How much room the window leaves beyond the disturbed span is two statistics, not one, and they are reported as two
because they answer different questions and coincide only when the moved boundaries fill their own hull. Both count
admissible boundary positions in the window `(p - L + 1, q)`, the theorem's length-bounded window with `a = 0`, clamped
to positions that exist, since position zero cannot move and the window reaches it when `p < L - 1`. The *hull slack*
subtracts the hull's inclusive width and measures how much wider the theorem's containment interval is than the span the
edit actually disturbed; the *unoccupied count* subtracts instead the number of boundaries that moved, and counts
admissible positions no moved boundary sits on. Both are inventory-relative, so both are reported twice: once against
the evaluation-tailored ceiling inventory of the rows above, and once against the deployable frozen inventory of the
transfer above, which supplies fewer anchors and therefore leaves more room. Table 6 gives the four columns; where the
two statistics agree there, it is a coincidence of these trials and not a fact about the definitions.

**Table 6.** How much of Theorem 5's window the edit left unused, under both inventories, emitted by
`splitting_measurements.py`. The ceiling inventory is the exhaustive one decided on the evaluation slice itself; the
frozen hybrid inventory is the one whose width-two-to-four component is calibrated on the disjoint adjacent slice and
applied out of sample.

| Vocabulary     | Statistic        | ceiling p50 | ceiling p90 | frozen p50 | frozen p90 |
|----------------|------------------|------------:|------------:|-----------:|-----------:|
| local-384      | hull slack       |          40 |          50 |         40 |         50 |
| local-384      | unoccupied count |          40 |          54 |         41 |         54 |
| GPT2-prefix-4k | hull slack       |          31 |          34 |         31 |         36 |
| GPT2-prefix-4k | unoccupied count |          31 |          35 |         31 |         36 |

The synchronization distance of Corollary 3 is unbounded for both subword vocabularies, since each admits a cycle of
non-certified bytes in its pending automaton, while the designed UTF-8 shape has distance three. The delimiter criteria
of Theorem 6 refuse the newline byte for both vocabularies in both directions, since both carry newline inside
multi-byte tokens, so a stream of either vocabulary's material is not soundly newline-splittable, and the criteria say
so before any splitter is built. Two segmentations of one corpus disagree readily across merge depths. The 48-merge and
384-merge local vocabularies agreed on none of 64 held-out slices, the two scans of each disagreeing slice exhibiting
the divergence directly.

The instruments also reach the four C-like grammars of the recovery companion's measurement campaign whole. Each grammar
enters through a class abstraction stated beside the program that measures it. That abstraction is checked by execution
to commute with the byte-level greedy scan on every token start of the grammar's archived half-mebibyte corpus, all
149,842 of the conventional row's among them. Every row's density verdict is unbounded, which rules out a grammar-global
gap guarantee, so the supply figures below are corpus measurements, and on every row the split theorem runs whole, the
archived corpus cut at every certified anchor and the chunked scans byte-identical to the sequential stream. Table 7
gives the four rows.

**Table 7.** Certified-anchor supply of the recovery campaign's four C-like grammars on their archived corpora, emitted
by `campaign_inventory.py`. Gap rows here include the endpoints, the run from position zero to the first anchor and the
run from the last anchor to the end of the corpus, so they are not comparable row for row with Table 3, whose gaps
exclude both.

| Campaign row   | anchors/KiB | gap p50 | gap p90 | gap max | snap max (8) |
|----------------|------------:|--------:|--------:|--------:|-------------:|
| conventional   |        25.7 |      43 |      69 |     104 |           15 |
| split-friendly |        41.0 |      21 |      64 |     104 |           15 |
| block          |         6.7 |     121 |     304 |     965 |           96 |
| bare           |       324.4 |       2 |       6 |      10 |            3 |

`campaign_inventory.py` asserts Theorem 5 on the archive as well. On each of the conventional and split-friendly
corpora, sixty cross-class single-byte substitutions are applied to a slice ending at a line boundary. They are retained
from eighty-eight draws per row, the twenty-eight whose edited slice no longer tokenized being redrawn; on each
attempted draw the position is uniform on the slice and the target class uniform among the other classes, with one fixed
representative byte per class. Every disturbed boundary moves strictly between the flanking shared-context anchors, with
a boundary-difference hull median of one byte and an observed maximum of two under the declared seed. Twelve same-class
substitutions per row, retained from fourteen position draws with the two whose byte was already its class's sole
representative redrawn, move no boundary at all, which is the class abstraction's invariance read as a measurement.

The four rows show what grammar design costs in anchors, and one pair of them is a controlled comparison. The
conventional and split-friendly rows differ in the grammar alone over the same bytes, while the block-comment and bare
rows are case studies of their own grammars and corpora. The class letters are the program's: `L` a letter or
underscore, `D` a digit, `S` a space or tab, `N` the newline, `P` a punctuator, `Q` the quote, `C` the slash, `A` the
star, `O` any other operator byte, and `X` every byte in none of those sets, each row distinguishing only the classes
its tokens tell apart and folding the rest into `O` or `X`. The conventional row has identifiers, numbers, operators,
punctuation, whitespace, string literals, and line comments, and holds 15 certified class pairs at windows up to length
two, none of them a single class at origin zero. It supplies 25.7 anchors per kibibyte with gap percentiles 43 and 69
and maximum 104, and with 13,171 anchors cutting the archive its eight-way splits snap within 15 bytes and
sixty-four-way within 38. The split-friendly row runs on the very same corpus bytes with the newline its own token, and
the design pays immediately. Here the newline class itself certifies at origin zero, the supply rises to 41.0 anchors
per kibibyte and the median gap falls from 43 to 21, exactly the certified-anchor reading of that row's purpose. The
block-comment row is the starved extreme. No window of two classes certifies at all, and the six-byte witness the
decision procedure synthesizes for the deepest of those refusals is a whole block comment. A close pins a boundary only
with one class of left context, since the opener's star followed by an interior slash mimics a close. Of the 81
close-shaped four-class windows, 65 certify at origin three, 57 of them with a window that occurs in some completely
tokenizable class string and 8 vacuously, a distinction the next paragraph takes up, and the supply is 6.7 anchors per
kibibyte with a maximum gap of 965 bytes. The bare row is the opposite extreme, all operators folded into one certified
class and all punctuators into another, 324.4 anchors per kibibyte with a maximum gap of 10 bytes.

**Occurring and vacuous certificates.** Corollary 2 separates two readings of a certificate, and the two inventories of
this section report different ones. The vocabulary candidate inventories enumerate windows occurring in the slice, so
every certificate of Table 3 occurs. The campaign inventories enumerate the whole declared class alphabet, so their
counts are unrestricted: a pair whose window occurs in no completely tokenizable class string is certified vacuously and
anchors nothing. `occurring_windows.py` decides, for every certified pair, whether its window occurs in some completely
tokenizable class string, and `campaign_inventory.py` reports the split. Of the 156 certified pairs over the four rows,
117 occur and 39 are vacuous; the block row's 65 are the selected close-shaped width-four extension alone, its inventory
through width two being empty, and the other three rows' are their inventories through width two. On the conventional
row 13 of the 15 occur and on the split-friendly row 17 of the 19, the two vacuous pairs of each row being the window
`NX` at both origins, since `X` occurs only inside a string literal or a line comment, neither of which contains a
newline, and no token begins with `X`. On the block row 57 of the 65 occur, the eight vacuous ones being `xACX` at
origin three for every class `x` but the slash, since `X` occurs only inside a block comment, a star followed by a slash
past the opener closes one, and no token begins with `X`; with the slash the window occurs, as an opener followed by an
interior slash, and is refused. On the bare row 30 of the 57 occur, the 27 vacuous ones being every pair whose window
uses `X`, that class being in the row's alphabet and in no token language of it. Nothing downstream moves, because a
window that occurs in no tokenizable input generates no corpus anchor, and the program checks that none of the 39 occurs
in its row's corpus, so anchor density, gaps, snapping, edit bounds and split equality are untouched.

**Scope of the measurements.** One corpus for the vocabulary studies; one 16 KiB slice of it, with an adjacent slice for
the frozen transfer; two primary vocabularies read under maximal munch rather than under their native merge ranks,
supplemented by a four-depth local series over the same corpus and slice; a window budget of `H <= 4`; one fixed edit
seed per study, 20260826 for the vocabulary trials and 20260827 for the two campaign edit rows; generated corpora for
the campaign rows; and one machine's illustrative timings. The theorems hold for every token set and input satisfying
their stated hypotheses; the figures in this section are what those theorems produced on that material, and nothing here
generalizes them to another corpus, vocabulary, or budget.

## 7 The artifact: the theorem as a scanner

The artifact is a threaded scanner probe, `parallel_scan.cpp` in the `munch` library, that cuts a corpus at certified
anchors, scans the chunks in parallel, joins the results, and asserts that the boundary stream equals the sequential
one. The inventory those anchors come from is the artifact's, derived from the campaign grammar alone and computed
before the input exists. The probe is pinned rather than described, by repository, commit and source digest, as Appendix
A records. Timing lines are machine-dependent and recorded rather than enforced; every other line is stable and compared
on regeneration.

On the archived half-mebibyte corpus, 149,842 token starts, the snap maxima are 15, 26, 28, and 38 bytes at 8, 16, 32,
and 64 workers, and the chunked stream is byte-identical in every configuration. On sixteen concatenated copies,
8,388,608 bytes and 2,397,472 token starts with 210,751 anchors, the snaps collapse to 0, 0, and 6 bytes and the
equality holds again. That larger input is the same corpus sixteen times over, so it stresses scale and not variety, and
it is read here as a scaling case rather than a second workload. The two zero snaps are a fact about its layout and say
nothing about snapping at scale, since the fifteen copy joins are anchors, `210,751 = 16 × 13,171 + 15`, and the eight-
and sixteen-worker grids fall exactly on them. In the recorded timings on the recorded machine, the sequential scan
takes 35.06 milliseconds and the 32-worker split 5.70, a speedup of 6.16, while the half-mebibyte configuration tops out
at 3.06 at 8 workers over a two-millisecond scan. That is consistent with a fixed parallel overhead mattering more as
the input shrinks, though nothing here isolates that overhead. The timings are best-of-five on one machine, with five
whole-probe repetitions of the half-mebibyte configuration recorded beside them in the sidecar; the sixteen-copy figures
are the probe's own best of five and nothing more. The timings cover scanning and, for parallel runs, thread creation
and joining; they exclude anchor discovery, cut selection and output concatenation. Sequential runs build a fresh
boundary vector each repetition, while parallel runs reuse each chunk's vector capacity across repetitions. All these
timings are illustrative, and they are not part of the theorem's guarantee. The stable fact, equality, is what the
theorem promises; the snap distances are measured, and depend on the input, the inventory, and the worker grid; and the
timing is what a machine adds. How a reader regenerates every emission and checks the scanner's provenance is recorded
in Appendix A.

## 8 Related work

**Code synchronization and resolving blocks.** The certificate has a classical special case, and the window paper places
it in that lineage ([arXiv:2608.09761](https://arxiv.org/abs/2608.09761)). A synchronizing pair of a code `X` is a pair
`(x, y)` of words of `X*` such that `uxyv ∈ X*` forces `ux ∈ X*` and `yv ∈ X*` for all `u` and `v` (Berstel, Perrin and
Reutenauer, Cambridge University Press 2010), so an occurrence of `xy` in a word of `X*` fixes the cut between its
halves in every context. A code has finite synchronization delay when some number of codewords on each side makes every
such pair synchronizing (Restivo, Theoretical Computer Science 1975). A related bounded-window recovery notion for the
circular factors of morphic images is the morphism synchronizing with finite delay of Fici, Romana, Sciortino and Urbina
(MFCS 2025). For prefix codes the synchronizing words are themselves an algorithmic subject: shortest ones for Huffman
codes (Biskup and Plandowski, Theoretical Computer Science 2009) and for prefix codes and their decoders (Ryzhikov and
Szykuła, MFCS 2018). Over a prefix code read as a token set, the completely tokenizable inputs are the words of `X*` and
the scan computes their one factorization, so the definitions give the relation exactly. A window occurring in a word of
`X*` is certified exactly when two conditions hold: every occurrence of it in every word of `X*` has a factorization
boundary at the origin, and its part from the origin on is a prefix of a codeword. The first condition fixes the
factorization cut, whose two sides need not themselves lie in `X*` as a synchronizing pair's do. The second makes the
codeword starting at the cut cover the window's tail, since a codeword beginning at the cut cannot end inside that tail
without being a proper prefix of the codeword the tail begins. In symbolic dynamics a resolving block is a block of a
factor subshift all of whose admissible preimages agree at a selected coordinate (Adler, Coppersmith and Hassner, IEEE
Transactions on Information Theory 1983; Marcus, IEEE Transactions on Information Theory 1985). The certificate has that
shape by analogy, the window as the block and the boundary bits from its origin to its end as the values every preimage
agrees on, though resolving blocks are stated for factor maps of subshifts, without token priorities or a finite input.
What separates the C-like grammars and the byte-pair vocabularies measured here from the code case is that they are not
prefix codes, and need not be codes at all. Each C-like token set measured here contains the extensible identifier
language, in which an identifier is a proper prefix of a longer one, so none is prefix-free, while `ab` has two
factorizations over `{a, ab, b}`, so that set is no code, and it is maximal munch, not unique decipherability, that
picks one. For such sets the certificate is defined through the scan and decided by the constructions of Section 3, the
literal cutoff and the armed run, rather than on a code's automaton. The sentence to hold this paper to is the window
paper's: beyond the code-factorization special case, no prior work was found that decides, for prioritized token
languages under maximal munch, whether a window's every occurrence fixes the origin of the covering token.

**Computing the scan.** Maximal munch is the standard convention, and its algorithmics have an established literature.
Reps (TOPLAS 1998) shows that a scanner can perform it in time linear in the input. He contrasts it with the textbook
backtracking scanner, which is quadratic on token sets such as `{a, a*b}`, whose scan may look arbitrarily far past a
final state before committing. Li and Mamouras (OOPSLA 2025) give algorithms for the uniform version of the problem, the
grammar an input beside the text, that run in time linear in the text for a fixed grammar. For WordPiece's finite
vocabulary, Song, Salcianu, Song, Dopson and Zhou (EMNLP 2021) scan in linear time by keeping the uncommitted suffix as
a node of the vocabulary trie and, where the trie cannot continue, emitting the node's failure pops and following its
failure link to the node of the suffix still uncommitted. Read against Lemma 4, that node is the pending tail, run there
as a scanner rather than searched as a decision. Reps states two conventions this paper inherits: a tie over the longest
match is broken by the earlier token definition, and no token language admits the empty string. That literature answers
how fast the scan runs; the question here is which of its positions are safe to start from, which is orthogonal and does
not appear in it.

**Longest match as a finite-state relation.** Karttunen (ACL 1996) compiles left-to-right longest-match replacement into
finite-state transducers. He builds tokenizers with them by inserting an end-of-token mark after each match, an encoding
of segmentation as a marked string, which the markings of Theorem 2 take up in bit form. Kaplan (US Patent 5,721,939,
1998; CSLI Publications 2005) tokenizes with a finite-state relation and emits output at pinch points, positions of the
text at hand where all tokenization paths reaching them can be extended in exactly the same ways. That is convergence on
one input's configurations, where a certificate asks that a fixed window fix the covering token's origin in every
context. Two of Karttunen's observations bear directly on Section 3. He notes that the tokenizing transducer needs the
left-to-right longest-match constraint to have a unique output at all, since a mark can be placed after a letter
sequence only once it is known the word is not part of a longer one. And he gives the separation this paper's frontier
rests on: the transducer for `a⁺b` is unambiguous yet cannot be sequentialized, because "it is obviously impossible for
any finite-state device to accumulate an unbounded amount of delayed output". The same shape recurs across the
literature. It is Reps's quadratic example, it is Karttunen's unsequentializable one, and it is the set on which
certification here decides offline while no finite-state scanner emitting boundaries irrevocably, left to right, can
exist. Two consequences of his construction are credited rather than restated. That the correctly marked greedy streams
of a regular token set form a regular language is already there, since a transducer marking maximal instances has a
regular output range. And once that language is effectively regular, deciding whether a marking proposed from outside
belongs to it is membership testing, so the existence of an offline verifier is an inference from his work and is not
claimed here either. What Theorem 2 supplies is the particular automaton: an explicit armed run over marked bytes,
carrying no token identities, whose size is bounded in the token set and which is built so that it can be intersected
with a window's badly covered occurrences. That intersection is what turns a verifier into a certificate test, and it is
the reason the construction is stated rather than the fact that some verifier exists. What Karttunen constructs is the
transducer; what this paper decides are properties of the segmentations it produces.

**Equivalence of transducers, and what it decides.** The two predicates of Theorem 7 mirror the decomposition Veanes,
Hooimeijer, Livshits, Molnar and Bjørner (POPL 2012) state for symbolic finite transducers, where deciding `A ≡ B`
"reduces to two independent tasks", domain equivalence and equality of outputs on the common domain. That is the general
reason a comparison of this shape is decidable, and it is stated of single-valued transducers over a decidable label
theory. What it does not settle is the object compared. Two maximal-munch policies are not given as transducers, though
one whose output is the segmentation exists by Karttunen's construction above, and what is used here instead is Theorem
2's armed run, an acceptor of correctly marked scans that validates a guess of the marks and confines it to the policy's
own scan. The distinction is the construction that produces the objects to compare, and the product searches that return
the witnesses; Veanes et al.'s own equivalence decision is described as nonconstructive, with counterexample generation
supplied by a separate procedure.

**Incremental and checkpointed lexing.** Theorem 5 bounds an edit's damage by a position the token set fixes before any
input exists, which is a different currency from the one incremental lexers spend. Incremental lexing in the style of
Wagner and Graham (manuscript 1997) restarts from per-token scanner-state snapshots, exact for the text they were cached
against, in a versioned token stream with dynamically maintained lookahead dependencies. The confinement is then a
property of a recorded run and of the state carried across edits, and it is available wherever that state was kept. The
bound here keeps nothing. The next certified anchor whose witnessing occurrence lies in the unchanged suffix is a
boundary of both versions by the certificate alone, so the damage interval follows from the token set and the edit
position. It holds where no snapshot exists, at the price of existing only where the token set admits an anchor at all.
The two compose rather than compete, and Section 6 measures how much of the theorem's window a real edit leaves unused,
which is what decides whether the certificate-only bound is worth acting on. The author's parser library hopper (release
v1.0.0, archived at doi:10.5281/zenodo.22998778) implements the restart for range edits, beyond the single-byte
substitution Theorem 5 is stated for. Its `Document` type searches backward for a certified anchor whose evidence the
edit left untouched and rescans from it to the first shared boundary past the edit, relexing whole when that bounded
search finds no certified anchor; hopper's figures are recorded in that repository and are not bound by this paper's
gate.

**Cutting at a boundary, and deciding where one is.** Chassot and Kunčak (CAV 2026) verify a linear-time maximal-munch
lexer under the same two conventions this paper uses, non-nullable rules and priority by rule order. One of its lemmas
states that "removing a suffix of the input does not affect the lexing of the prefix, provided that the cut occurs at
token boundaries". Their lemma, iterated, implies Lemma 5, and Theorem 4 is what the certificate adds to it. Their
closest result is a conservative predicate `sep(t1, t2)` on already known adjacent tokens, and pairwise satisfaction
makes the printed token sequence re-lex unchanged. That predicate starts from concrete adjacent token texts, while a
certificate is decided from a raw window over every tokenizable context. Lester, Ong and Schäfer (Journal of Computer
Security 2016) check the same hypothesis at a known join when concatenating analysed code strings. They require every
lexer state reachable after the first string either to have emitted its token or to emit the same token on every
character that may begin the second, so that lexing the concatenation is concatenating the lexings. That check is made
per concatenation site, over the strings a string analysis admits for the two operands. The ZipLex lemma assumes a
boundary of a concrete input, while the join check establishes a sufficient boundary condition at one analysed site.
What this paper supplies is a certificate stronger than a boundary at the origin, computed from the token set before any
input exists, which makes the window's origin a boundary at every occurrence in every completely tokenizable context.
Neither `sep` nor the join check is the certificate. Whether a window's origin is a boundary at every occurrence in
every completely tokenizable input, without fixing the token that covers its final byte, is the weaker predicate, and
this paper does not decide it, the wider class of occurrence-based sound-cut rules being out of scope, as Section 2
says. The introduction and Section 2 separate the two predicates, with `(b, 0)` over `{ab, b}` refused because a context
overturns the window, and `(ab, 0)` over `{a, b}` refused although its origin is a boundary in every context.

**Parallel scanning.** Mytkowicz, Musuvathi and Schulte (ASPLOS 2014) parallelize finite-state machines, tokenization
among their applications, by enumerating transitions from every possible start state on each chunk and afterwards
selecting the run that began in the correct one. That is exact rather than approximate, and it buys its exactness by
paying for the enumeration, naively an overhead proportional to the state count, which their convergence optimization
reduces to the states still active. The artifact of Section 7 buys exactness differently, cutting only where a theorem
already fixes the start state, so there is no enumeration to pay for and no reconciliation pass, at the price of cutting
where the grammar permits rather than wherever one likes. The two are complementary, and the anchor supply of Section 6
is what decides whether the second is available for a given grammar at a given place. The other parallel-lexing families
pay in their own coin: speculation predicts an entry state, validates, and re-executes on a miss (Prabhu, Ramalingam and
Vaswani, PLDI 2010). In the patent literature a partition scanner cuts a stream at likely token boundaries, scans the
chunks in parallel, and squashes the later chunks and restarts at the actual boundary when a guess is refuted (von
Praun, US Patent 8,001,329, 2011). An XML preparser carries the runs of a chunk from every possible entry state in one
meta-DFA state, and keeps the run the preceding chunk's end state selects (Pan, Zhang, Chiu and Lu, e-Science 2007;
Chiu, US Patent 8,782,514, 2014). Bounded alternative-state scans carry every reading a chunk's entry admits and
reconcile at the join, with the syntax restricted where the alternatives would not stay bounded (Barenghi, Crespi
Reghizzi, Mandrioli, Panella and Pradella, Science of Computer Programming 2015). Prescanning pays a pass over the input
(Li, Sato, Liu and Taura, IPDPS 2021), and Yang (Computer Languages 1996) treats longest-match lexing with Mealy
machines, with a data-parallel algorithm among the results. Shao, Zheng, Wang, Zheng, Li and Fan (ACL 2026) claim exact
equality with sequential tokenization for BPE and WordPiece tokenizers by another route, overlapping the chunks and
merging adjacent ones at overlap tokens that sit at the same character positions in both. Their Theorem 3.1 states a
sufficient condition, a position-matched span strictly longer than the vocabulary's longest token, and the published
algorithm restarts with a doubled chunk only when it finds no overlap token at all, a check that does not enforce that
length condition. That pays after the fact where the enumeration pays during, and where a certificate pays neither; the
price here is again that a cut must land where the token set permits. Zhang and Cao (arXiv:2607.29678, 2026) use a
related matched-overlap idea for text appended to a session, under a different acceptance rule. They match fresh tokens
against cached token records and accept a splice only through a long enough equal run that contains a family-specific
stable boundary. For their byte-level BPE families that boundary is a character-class transition from a per-family
synchronizing set, which they report as resetting the pre-tokenizer under every left context. A failed check widens the
window and finally re-tokenizes the request. Their appendix names the junction condition that check discharges a splice
certificate, proves the splice lossless under it, and summarizes the per-family discharges, whose full proofs it places
in a separate document shipped with the artifact. Among the serving systems surveyed, those per-family acceptance
predicates are the closest analogues of a certificate, established for a fixed pipeline where the inventories here are
decided from the token set.

**The neighbouring quantity.** Li, Yang and Mamouras (ASPLOS 2026) statically compute, for a tokenizer, how far past a
token's end a scanner must read before its maximality is settled. That is finite-state analysis of the same shape as
Section 3's. Among the sources surveyed it is the nearest tokenizer-specific quantity to the ones here, but it is not
either of them. It measures confirmation from a start already known. Theorem 1 instead asks whether an origin is forced
at every occurrence in every context, and Theorem 3 asks how long an input can run with no anchor from a given finite
inventory. One token set separates them outright, `T = {aa}`. Its completely tokenizable inputs are the even-length runs
of `a`, and no longer token competes with `aa`, so nothing has to be read past a token's end and their distance is zero.
Here no occurring window is certified, because an occurrence of a window of `a`s beginning at position `p` has its last
byte covered by the token beginning at the greatest even position at or below it, so the origin is a function of the
parity of `p`, and both parities occur once the input is long enough. No occurring window of any width has a universal
origin, the occurring inventory is empty, and the anchor-free span is unbounded. A bounded confirmation distance
therefore does not imply a single certified position, and their supremum cannot stand in for either of these.

**Expected density under coverage, worst-case gaps without it.** Two lines outside compiler construction select
positions in a byte stream by a bounded window and then ask how often the selection fires, which is this paper's density
question asked of a designed rule. Bjørner, Blass and Gurevich (Journal of Computer and System Sciences 2010) call a
chunking method local when the cutpoints of every file are exactly the positions whose `h`-vicinity, the `2h + 1`
entries around the position, lies in a chosen criterion set. A finite certified inventory induces a cut rule of that
bounded-context form on every position whose `h`-vicinity lies inside the file, but the converse fails, since a local
rule need not respect token boundaries. Over `T = {aa}` the rule selecting every position is local and cuts every token
in half, where a certificate demands the covering-origin condition at every occurrence in every context. Over files
whose entries are drawn independently and uniformly, Bjørner et al. define the slack of two files that agree from a
point onward as the distance from that point to their first common cutpoint, analyze its expectation normalized by the
expected chunk length, and separately bound the probability that a long interval carries no cutpoint, for four chunking
methods. Schleimer, Wilkerson and Aiken (SIGMOD 2003) define the density of a fingerprinting scheme as the expected
fraction of positions selected under a given input distribution. Assuming independent, uniformly distributed hashes,
they prove winnowing's density asymptotically `2/(w+1)` for window size `w`, and a lower bound of `1.5/(w+1)` for every
local algorithm, one that selects a position from each window of `w` hashes by that window's contents alone. Neither
result transfers here. A certified inventory is under no obligation to select in every window, so the coverage the lower
bound assumes need not hold, and this paper asks for worst-case and corpus gaps rather than expected density under an
independence model. Two other lines ask the worst case of a finite inventory with no semantics attached. A finite set of
words is unavoidable when all but finitely many words contain one of them as a factor, which is decided by building the
automaton of the words that avoid the set and looking for a cycle among its live states (Rosaz, RAIRO Informatique
théorique et applications 1995). For a fixed alphabet the longest avoiding word is bounded as a function of the set's
longest word, a bound Rosaz traces to Schützenberger (Information and Control 1964) and one Crochemore, Le Rest and
Wender (Information Processing Letters 1983) showed best possible. A universal hitting set in the sense of Orenstein,
Pellow, Marçais, Shamir and Kingsford (PLOS Computational Biology 2017) is a set of `k`-mers one of which every `L`-long
sequence contains, for `L > k`, which is a bounded anchor-free span for a chosen inventory over all strings. Theorem 3
is that question with the semantics put back: the inventory is chosen subject to certification, an occurrence credits a
position only inside a completely tokenizable input, and what it credits is the origin the certificate names rather than
the occurrence itself, so the loop is looked for in the credit product of the armed-run verifier rather than in the
automaton of the avoiding words. The token set constrains which certificates are admissible; the window budget and the
chosen inventory decide which are used; and Section 6 measures what that supply comes to on named material rather than
tuning it.

**Self-synchronization.** UTF-8's self-synchronization is listed among its characteristics in Yergeau (RFC 3629, 2003)
as "character boundaries are easily found from anywhere in an octet stream", resting on the disjointness of the lead and
continuation ranges. The distance is not new either, since the same document bounds a character at four octets, one lead
and at most three continuations. What Corollary 3 adds is that the number is computed from the token set by a procedure
that knows nothing about UTF-8, returning three for that shape and returning unbounded, a reachable quiet cycle standing
behind the verdict, for token sets that have no such guarantee.

**Subword vocabularies.** The vocabularies measured in Section 6 are byte-pair encodings in the sense of Sennrich,
Haddow and Birch (ACL 2016), learned as an ordered list of merges and applied in that order. Berglund and van der Merwe
(NCMA 2023) give that process a formal semantics, a dictionary of ordered rules rewriting a whole string, and bound how
far appending on the right can move the resulting tokenization. Further work gives byte-pair encoding finite-state and
incremental accounts. Berglund, Martens and van der Merwe (Theoretical Computer Science 2026) construct automata
operating on BPE tokenizations together with an input-deterministic transducer relating strings to their tokenizations,
Jiang and Gong (ICML 2026) maintain a BPE tokenization incrementally over every prefix, and Mamouras, Li and Yang (PLDI
2026) bound its streaming delay from a known beginning. Cognetta and Okazaki (Computational Linguistics 2025) show both
BPE and MaxMatch representable by finite-state transducers, noting that BPE "does not tokenize strings from left to
right and requires a notion of priority". That priority is why Section 6 reads its BPE-derived vocabularies as
maximal-munch token sets and measures that policy, not BPE's own segmentation. The boundary comparison emitted here is
between depths of the locally trained family under that one reading; a study of agreement between native merge-rank BPE
and MaxMatch is not run, and its outcome is not assumed.

## 9 Conclusion

One definition carried the paper: a window and origin such that at every occurrence in every completely tokenizable
input, the token covering the window's last byte begins at that origin, a boundary no context can later dispute. The
guarantee runs one way, every certified position a boundary and not every boundary certified, and everything else is
that agreement spent four ways: splitting, editing, delimiting, and designing. The quantifier that made recovery
certificates trustworthy after damage is exactly what safe work division needs before it, and once certification became
decidable, at inventory scale and with witnesses, the applications stopped being policies and became theorems with
programs attached. The honest boundary of the results is stated where each holds. The theory lives at the lexical layer,
over greedy maximal-munch scans of literal and regular token sets, and the campaign grammars enter through class
abstractions checked by execution against byte-level scans. Density is a property the grammars studied here mostly
refuse, so a real grammar's supply is a corpus fact the instruments measure rather than a constant they promise. The
artifact scans with precomputed cuts, no speculation and no reconciliation pass, and asserts the equality of boundary
streams on every run. The forward reading has a natural continuation past this layer, where segmentation feeds parsing
and caching, and the same discipline, decide first, then measure, then assert, is what that continuation will be held
to.

## A Reproduction and provenance

**Artifact and data availability.** The script `sh scripts/check-paper4.sh` regenerates every in-bundle emission this
paper quotes and compares it byte for byte, regenerating the scanner statistics when the pinned build is supplied and
otherwise checking the prose against the committed statistics. The same gate rebuilds the manuscript and holds every
measurement it names to the emission that produced it, printing that a measurement it does not name is not checked. It
holds the timing sidecar to its schema's fields and to the row count its declared repetitions imply; it runs in minutes
on an ordinary machine, the wide cutoff sweep being the slowest stage. The vocabulary measurements read two GPT-2
reference files that are not redistributed here. The gate names them, holds them to their published digests, and refuses
rather than skipping when they are absent. The evaluation fixture is not redistributed either, and the shipped bundle's
`verify-all.sh` fetches `twitter.json` from the pinned simdjson release and holds it to its digest, while the campaign
corpora ship, so the campaign emissions rerun with no network at all. Two stages are optional and named as such. Setting
`CHECK_PAPER4_REDERIVE=1` rederives all three inventories with every decision remade, reading and writing no committed
cache, which costs hours rather than minutes and writes its own log outside the repository; and passing a built `munch`
checkout to the gate reruns the threaded scanner of Section 7, whose stable lines are compared and whose timing lines
are recorded rather than enforced. That same stage also reruns the library's own recomputation, described below, and
holds every line it recomputes to the committed record. The emissions, caches and campaign corpora, the programs that
derive them, the gate and the runner are archived together as a standalone record at doi:10.5281/zenodo.22994324.

The provenance a standalone reader needs is the following. The threaded scanner is the `munch` library (release v2.1.0),
a public C++ repository at https://github.com/nnidhogg/munch under the MIT licence. Its archived record across versions
carries one concept identifier, doi:10.5281/zenodo.21752996; the record of release v2.1.0 itself is
doi:10.5281/zenodo.23026012. The runner `run-artifact.sh` names the repository, the commit it pins, the commit its
release v2.1.0 tags, and the sha256 of each probe source, and refuses to run against anything else, so the scanner
behind these numbers is that commit rather than whatever happens to be checked out. The build directory must sit inside
a checkout at that commit, and the probe sources, `parallel_scan.cpp` among them, must carry their pinned sha256. The
commit is what binds the scanner, not the one file; the scanner probe includes the grammar header and links the library,
so a source digest alone would leave most of what runs unaccounted for. The two GPT-2 reference files are the byte-level
GPT-2 tokenizer's vocabulary and merge table, published with that model as `vocab.json` and `merges.txt` and mirrored in
the `openai-community/gpt2` model repository; place them beside the bundle under the names below, and the digests, not
the source, are what decide whether they are the bytes this paper read:

- `gpt2-vocab.json` `196139668be63f3b5d6574427317ae82f612a97c5d1cdaf36ed2256dbf636783`
- `gpt2-merges.txt` `1ce1664773c50f3e0cc8842619a93edc4624525b728b188a9e0be33b7726adc5`

The gate holds both to those digests before any measurement runs and refuses rather than skipping when they are absent.
The measurements read only the merge table; the vocabulary file is digested and reported, which is what names the GPT-2
release that merge table belongs to.

**The decisions in the library.** The library's own tests in `munch` release v2.1.0 check four of the facilities the
exploration programs model against those programs: three decisions declared beside the byte and window certificates and
forwarded by its lexer, and one measurement. `window_occurrence()` decides whether a window occurs in some completely
tokenizable input, with the shortest such input. `window_counterexample()` decides whether some completely tokenizable
input violates a window certificate, with the shortest such input, so that an exhausted search proves the certificate
exact. `segmentation_difference()` decides whether two token sets define the same domain-to-boundary map, with a
shortest differing input and whether the difference lies in their domains or their boundaries. The fourth is the audit
tool's supply, a measurement over the audit report rather than a lexer function. The supply measures rather than
decides. It gives the anchors the certificates place in a given input, with their density and gaps. At the commit
`run-artifact.sh` pins, those tests check that each of the three decisions gives the same verdict and the same shortest
witness length as the exploration that models it: the window decisions on the exploration's own enumerated universes,
and the segmentation difference on the 225 literal pairs of its census and the 64 pairs of the companion sweep's eight
sets, the 25 regex pairs and the cross-representation pair staying the exploration's alone. The supply is checked
against the exploration's anchors, density and gap figures on the vocabulary and corpus the two share. The library also
carries a probe, `paper4_recompute.cpp`, pinned by `run-artifact.sh` beside the scanner, that rederives this paper's
emissions from those decisions in the emissions' own line format. In it the token sets are rebuilt as the programs build
them and digested, the byte certificate is the lexer's split-point test, the window certificate is
`window_counterexample()` exhausted with no witness, the supply is the audit report's, the gap corollary is the
anchor-free span, the campaign differential is the boundary difference, and the edit trials replay the programs' seeded
draws through a port of their generator. Two routes differ from the library's, one by convention and one by inventory.
The campaign's gap percentiles count the runs to the input's ends, which the report's supply leaves out, so the emission
carries the supply's figures on a line of their own. The campaign's density verdict is decided by the emission over the
exact class-window inventory, and by the probe by `anchor_free_span()` over the byte expansions the conservative window
model admits, a subset of that inventory. The probe's verdict therefore corroborates the emission's without establishing
it, an unbounded span over fewer anchors saying nothing of the span over more; on the four campaign rows both are
unbounded. Of the lines it recomputes across the eleven emissions, 228 are the committed lines byte for byte. Of the
rest, seven agree up to a remainder the probe does not write, an inventory key that digests the Python sources or the
prose after a verdict, and two agree in their figure under the library's name for the decision, the wide sweep's refuted
and tight counts, which the probe writes with *counterexample* where the emission says *violation*; none differs. The
lines the probe writes that no committed line answers, its own diagnostics on the campaign and vocabulary runs, are
listed apart by the comparison and stand outside these counts. The exact window decider reaches the offline verifier's
certified counts and the anchors they supply on the evaluation, calibration, depth-series and campaign universes, at
every width of the budget on the evaluation slice, and on the conventional campaign row its certified pairs one by one.
The eighteen lines it does not recompute are prose, the attribution comments, the line naming the trial records with
their digest, and seven constructive-walk lines of the wide sweep, carrying five counts, whose enumeration runs to a
declared margin above each cutoff and which the library does not carry. The explorations remain the record the committed
emissions were written by and the independent oracle the library is checked against, and each of the two now checks the
other. One difference between the two stays and is stated: the report's window row counts the windows the library's
conservative window model certifies. That model refuses some windows the offline verifier certifies wherever a token has
an accepted proper prefix. On a byte-fallback vocabulary that row is a lower bound on the exact inventory of Table 3,
while the byte rows agree with it exactly.

## References

- N. Nidhögg. *Certified Panic Mode: Repair-Invariant Error Recovery for Maximal-Munch Lexing.* Preprint,
  arXiv:2609.10600, 2026.
- N. Nidhögg. *Certified Split Points for Parallel Lexing: Exact and Modulo Discarded Tokens.* Preprint,
  arXiv:2608.03473, 2026.
- N. Nidhögg. *Certified Split Windows for Parallel Lexing: Recovering Boundaries Where No Byte Certifies.* Preprint,
  arXiv:2608.09761, 2026.
- N. Nidhögg. *munch: a lexical analysis library based on automata theory.* Public repository,
  https://github.com/nnidhogg/munch, 2026. The threaded scanner of Section 7 is this library at the commit
  `run-artifact.sh` pins, the commit its release v2.1.0 tags, whose archived record is doi:10.5281/zenodo.23026012 under
  the concept identifier doi:10.5281/zenodo.21752996.
- A. Restivo. *A Combinatorial Property of Codes Having Finite Synchronization Delay.* Theoretical Computer Science
  1(2):95-101, 1975.
- *simdjson: parsing gigabytes of JSON per second (source repository).* GitHub repository, release v3.10.1, file
  `jsonexamples/twitter.json`, 2024. https://github.com/simdjson/simdjson/blob/v3.10.1/jsonexamples/twitter.json.
  Dataset provenance for the evaluation fixture; not redistributed with this work, fetched from this release by the
  bundle's verification script and held to its SHA-256.
- J. Berstel, D. Perrin, C. Reutenauer. *Codes and Automata.* Encyclopedia of Mathematics and its Applications 129,
  Cambridge University Press, 2010.
- G. Fici, G. Romana, M. Sciortino, C. Urbina. *Morphisms and BWT-Run Sensitivity.* MFCS 2025, LIPIcs 345, 49:1-49:18.
- M. T. Biskup, W. Plandowski. *Shortest Synchronizing Strings for Huffman Codes.* Theoretical Computer Science
  410(38-40):3925-3941, 2009.
- A. Ryzhikov, M. Szykuła. *Finding Short Synchronizing Words for Prefix Codes.* MFCS 2018, LIPIcs 117, 21:1-21:14.
- R. L. Adler, D. Coppersmith, M. Hassner. *Algorithms for Sliding Block Codes: An Application of Symbolic Dynamics to
  Information Theory.* IEEE Transactions on Information Theory 29(1):5-22, 1983.
- B. H. Marcus. *Sofic Systems and Encoding Data.* IEEE Transactions on Information Theory 31(3):366-377, 1985.
- T. Reps. *"Maximal-Munch" Tokenization in Linear Time.* ACM Transactions on Programming Languages and Systems
  20(2):259-273, 1998.
- A. W. Li, K. Mamouras. *Efficient Algorithms for the Uniform Tokenization Problem.* PACMPL 9(OOPSLA1), article 133,
  1492-1518, 2025.
- X. Song, A. Salcianu, Y. Song, D. Dopson, D. Zhou. *Fast WordPiece Tokenization.* EMNLP 2021, 2089-2103.
- L. Karttunen. *Directed Replacement.* ACL 1996, 108-115.
- R. M. Kaplan. *Method and Apparatus for Tokenizing Text.* US Patent 5,721,939, filed 1995, assigned to Xerox
  Corporation, 1998.
- R. M. Kaplan. *A Method for Tokenizing Text.* In Inquiries into Words, Constraints and Contexts, CSLI Publications,
  55-64, 2005.
- M. Veanes, P. Hooimeijer, B. Livshits, D. Molnar, N. Bjørner. *Symbolic Finite State Transducers: Algorithms and
  Applications.* POPL 2012, 137-150.
- T. A. Wagner, S. L. Graham. *General Incremental Lexical Analysis.* Manuscript, University of California, Berkeley,
  1997; the circulating build is the December 1999 revision. https://harmonia.cs.berkeley.edu/papers/twagner-lexing.pdf
- N. Nidhögg. *hopper: a C++23 library for building recursive-descent parsers on top of munch lexers.* Public
  repository, https://github.com/nnidhogg/hopper, 2026. `hopper::json::Document` applies the restart principle of
  Theorem 5, read at its release v1.0.0, whose archived record is doi:10.5281/zenodo.22998778 under the concept
  identifier doi:10.5281/zenodo.22998777; the repository's `docs/design.md` records the probe's figures.
- S. Chassot, V. Kunčak. *Formally Verified Linear-Time Invertible Lexing.* CAV 2026, LNCS 16683, 141-164.
- M. M. Lester, L. Ong, M. Schäfer. *Information Flow Analysis for a Dynamically Typed Language with Staged
  Metaprogramming.* Journal of Computer Security 24(5):541-582, 2016.
- T. Mytkowicz, M. Musuvathi, W. Schulte. *Data-Parallel Finite-State Machines.* ASPLOS 2014, 529-542.
- P. Prabhu, G. Ramalingam, K. Vaswani. *Safe Programmable Speculative Parallelism.* PLDI 2010, 50-61.
- C. von Praun. *Speculative Stream Scanning.* US Patent 8,001,329, filed 2008, assigned to International Business
  Machines Corporation, 2011.
- Y. Pan, Y. Zhang, K. Chiu, W. Lu. *Parallel XML Parsing Using Meta-DFAs.* Third IEEE International Conference on
  e-Science and Grid Computing, 237-244, 2007.
- K. Chiu. *Parallel XML Parsing Using Meta-DFAs.* US Patent 8,782,514, filed 2009, assigned to The Research Foundation
  for The State University of New York, 2014.
- A. Barenghi, S. Crespi Reghizzi, D. Mandrioli, F. Panella, M. Pradella. *Parallel parsing made practical.* Science of
  Computer Programming 112:195-226, 2015.
- L. Li, S. Sato, Q. Liu, K. Taura. *Plex: Scaling Parallel Lexing with Backtrack-Free Prescanning.* IPDPS 2021,
  693-702.
- W. Yang. *Mealy Machines Are a Better Model of Lexical Analyzers.* Computer Languages 22(1):27-38, 1996.
- W. Shao, L. Zheng, P. Wang, P. Zheng, J. Li, Y. Fan. *LoPT: Lossless Parallel Tokenization Acceleration for Long
  Context Inference of Large Language Model.* ACL 2026, 33107-33122.
- Z. Zhang, Z. Cao. *TokTier: Exact Stateful CPU+GPU Tokenization for Agentic LLM Serving.* Preprint, arXiv:2607.29678,
  version 3 of 6 August 2026.
- A. W. Li, Y. Yang, K. Mamouras. *Static Analysis for Efficient Streaming Tokenization.* ASPLOS 2026, 1880-1896.
- N. Bjørner, A. Blass, Y. Gurevich. *Content-dependent chunking for differential compression, the local maximum
  approach.* Journal of Computer and System Sciences 76(3-4):154-203, 2010.
- S. Schleimer, D. S. Wilkerson, A. Aiken. *Winnowing: Local Algorithms for Document Fingerprinting.* SIGMOD 2003,
  76-85.
- L. Rosaz. *Unavoidable Languages, Cuts and Innocent Sets of Words.* RAIRO Informatique théorique et applications
  29(5):339-382, 1995.
- M.-P. Schützenberger. *On the Synchronizing Properties of Certain Prefix Codes.* Information and Control 7(1):23-36,
  1964.
- M. Crochemore, M. Le Rest, P. Wender. *An Optimal Test on Finite Unavoidable Sets of Words.* Information Processing
  Letters 16(4):179-180, 1983.
- Y. Orenstein, D. Pellow, G. Marçais, R. Shamir, C. Kingsford. *Designing Small Universal k-mer Hitting Sets for
  Improved Analysis of High-Throughput Sequencing.* PLOS Computational Biology 13(10):e1005777, 2017.
- F. Yergeau. *UTF-8, a transformation format of ISO 10646.* RFC 3629, Internet Engineering Task Force, 2003.
- R. Sennrich, B. Haddow, A. Birch. *Neural Machine Translation of Rare Words with Subword Units.* ACL 2016.
- M. Berglund, B. van der Merwe. *Formalizing BPE Tokenization.* NCMA 2023, EPTCS 388, 16-27. Preprint:
  arXiv:2309.08715.
- M. Berglund, W. Martens, B. van der Merwe. *Constructing a BPE Tokenization DFA.* Theoretical Computer Science
  1083:116131, 2026.
- S. Jiang, R. Gong. *Incremental BPE Tokenization.* Accepted to ICML 2026 (Spotlight). Preprint: arXiv:2605.30813.
- K. Mamouras, A. W. Li, Y. Yang. *An Efficient Algorithm for Streaming BPE Tokenization.* PACMPL 10(PLDI), article 252,
  2085-2108, 2026.
- M. Cognetta, N. Okazaki. *Tokenization as Finite-State Transduction.* Computational Linguistics 51(4):1119-1149, 2025.
