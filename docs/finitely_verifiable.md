# Finitely Verifiable Segmentation: Decidability for Every Tokenizer with a Finite-State Verifier

**Nicklas Nidhögg**, October 2026. Mirrors `paper/finitely-verifiable/finitely-verifiable.tex`, published as
[arXiv:2610.30168](https://arxiv.org/abs/2610.30168), which evaluates munch at the v2.3.0 release, archived at
doi:10.5281/zenodo.23287096 under the concept identifier doi:10.5281/zenodo.21752996. Its instruments in the tree are
the verifier `verifier::Verifier`, with the armed run `armed_run()` over a byte automaton and the verifiers of the two
tokenizer instances, the merge tower `merge_tower()` of canonical byte-pair encoding and the trie chain `trie_chain()`
of two-mode WordPiece; the decisions `miscovering()`, `boundary_gap()`, `realizable()` and `divergence()` and the cut
and chunk independence decisions `dependence()` and `chunk_dependence()`, each held by the library's tests to the five
reference fixtures the paper's exploration writes, `verifier_reference.txt`, `cut_reference.txt`, `chunk_reference.txt`,
`bpe_reference.txt` and `wordpiece_reference.txt` under `libs/verifier/tests/data/`; and the probe
`tools/probes/src/verifier_supply.cpp`, which holds the verifier's window certificates against the lexer's window route
on `twitter.json` and measures the certified positions they supply. At v2.3.0 the verifier and its constructions lived
under `munch/dfa/` in namespace `munch::dfa`, as the paper names them, and since 3.0.0 they live under `munch/verifier/`
in namespace `munch::verifier`, as the README's Versioning section records. The exploration modules that implement the
decisions and print the counts the paper quotes, the generators of the five fixtures, the records of the library,
transfer and WordPiece instances and the gate are deposited as a standalone record at doi:10.5281/zenodo.23708161. The
two tables are read here from the paper's own tabular source, never transcribed by hand.

*A technical report on the finite verifier behind `verifier::Verifier` and the decisions it carries, with the class
placed between scanning and regular domains, closed under the pipeline operations, its certificates carried across
stages and its masses solved exactly. The implementation, tests, and probes live in this repository; this document
states the results precisely, relates them to prior work, and reports what the instances measured.*

## Abstract

A segmentation policy assigns to each byte string at most one segmentation into tokens. Call it *finitely verifiable*
when a finite automaton over marked strings, bytes paired with boundary bits, accepts exactly the policy's own
segmentations. The automaton judges a proposed segmentation and need not compute one. From a supplied verifier the paper
gives constructive decisions for four questions, whether a window certifies a token boundary, whether the boundary gap
is bounded, whether a mask can keep generation from dead-ending, and whether two policies are equivalent. The last three
specialize limitedness, co-accessibility and the equivalence of single-valued transducers. The first refines the
synchronizing cut of code theory, and two more ask whether a certified cut, or the cuts of an inventory, let each piece
scan alone. The class sits strictly between the online-scannable policies and those with regular domains, and its inner
frontier is Choffrut's and decidable in polynomial time. Markings are length-preserving, so the class is exactly the
rational segmentation policies, a normal form rather than a restriction. The class is closed under four pipeline
operations, composition over a labeled stage, restriction to a regular schema, splitting at character-class changes, and
byte-fallback union of literal policies. Each composite inherits the six decisions, and certificates transfer between
stages under four laws. Under rational, unifilar, almost surely stopping finite-state sources, each mass of the weighted
section is an exact rational, and so is each rate where expected length is positive. Maximal-munch lexing, canonical
byte-pair encoding and two-mode WordPiece instantiate the interface, and the munch library implements all six decisions.
Every theorem carries an executable check, and every reported statistic is a program's own output.

## 1 Introduction

A production system may need to know where its byte stream resynchronizes after a lost fragment, how far that stream can
run before a token boundary is guaranteed, whether a grammar-masked generator can be left with no admissible next token,
and what a tokenizer upgrade does to cached segmentations. Exact procedures answer some of these in particular settings,
such as masks in constrained decoding (Koo, Liu and He, COLM 2024) and the equivalence of byte-pair dictionaries
(Berglund, Martens and van der Merwe, CIAA 2024). Each question quantifies over all inputs, and a finite sample of
inputs cannot establish such a universal guarantee. From a supplied verifier, this paper gives constructive decisions
for all four, certification, the boundary gap, realizability and equivalence.

The series began with certified bytes ([arXiv:2608.03473](https://arxiv.org/abs/2608.03473)) and certified windows
([arXiv:2608.09761](https://arxiv.org/abs/2608.09761)) for maximal-munch scanning, and its two companion studies
answered special cases of those questions. The recovery companion ([arXiv:2609.10600](https://arxiv.org/abs/2609.10600))
proved that certificates make resuming after damage sound. The splitting companion
([arXiv:2610.08854](https://arxiv.org/abs/2610.08854)) read the same certificates forward into speculation-free work
division, and decided certification, the anchor-free span, and segmentation differentials for regular token sets. The
present paper states what those decisions were always instances of. It rests on one hypothesis.

**Definition 1 (segmentation policy; finite verifiability).** A *segmentation policy* assigns to each string at most one
segmentation, none when the string is not tokenizable. A segmentation here is an unlabeled partition of the string into
consecutive nonempty pieces, equivalently its boundary bit pattern, the boundary projection of what the splitting
companion calls a segmentation. The names a stage gives those pieces are separate labeler data, which is why two
policies naming identical spans differently are the same policy. A *marked string* pairs each byte with one bit, whether
a token boundary follows that byte's position, and a position `i` of it is a *token start* exactly when `i = 0` or the
bit at `i-1` is set. A policy is *finitely verifiable* when some deterministic finite automaton over marked strings
accepts exactly the markings that are the policy's own segmentations of its tokenizable strings.

The verifier decides a *proposed* segmentation by reading it. It need not compute the segmentation online, and that
distinction is the crux of the paper. Section 4 exhibits a policy with no finite-state bounded-delay scanner that is
nevertheless finitely verifiable. Verification is the strictly weaker demand, and everything below asks only for it.

The class is not new, and the paper does not present it as new. A finitely verifiable policy is a length-preserving
partial *automatic function* in the sense of Case, Jain, Seah and Stephan (Logical Methods in Computer Science 2013),
its graph a functional synchronous relation, regular under convolution. The text keeps the verifier-first name because
the verifier is the interface every construction below programs against. Section 9 surveys the transducer literature.
The first three contributions below were not found in it and are what the paper claims as its own; the last two place
the class and carry it to real tokenizers.

The contributions are:

- The four decisions, and the two that ask whether certified cuts let the pieces scan alone, run on supplied verifier
  presentations with constructive outputs and survive the four pipeline operations of Theorem 4, composition over a
  labeled stage, restriction to a regular schema, splitting at character-class changes and byte-fallback union of
  literal policies (Sections 3 and 5).
- Certificates transfer between stages under proved laws, with an exact stage-local lift (Section 6).
- Zero differential mass is identified with the absence of a boundary differential in the source's support, and the
  exact masses behind it are solved from the supplied verifiers through the same interface (Section 7).
- The class sits strictly between the online-scannable policies and those with regular domains, its inner frontier is
  decidable in polynomial time, and over the marking encoding it is exactly the rational segmentation policies (Section
  4).
- Literal and regular maximal munch, canonical byte-pair encoding and two-mode WordPiece carry the interface, each
  validated against an independent reference scanner or encoder, and the munch library implements all six decisions
  (Sections 2 and 8).

**Table 1.** What the hypothesis of Definition 1 buys, and where each result is stated and proved. Each computation
behind a row has an executable check, named in a source comment in its section or held by the claims gate; the outside
lemma and the collapse proposition are proofs alone.

| Result                     | Statement                                                                                                                                                                                                                                                                                                                                                                            | Where                                                      |
|----------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------|
| Four decisions             | Certification, the boundary gap and realizability are decidable for every supplied verifier, and full equivalence for every pair of them; certification returns a counterexample when negative, equivalence a differing string, the boundary gap a pumpable lasso, and realizability the mask                                                                                        | Theorem 1                                                  |
| Cut and chunk independence | Whether a certified cut lets both its sides scan alone, and whether all of an inventory's cuts do at once, are decidable, each with a shortest counterexample; the first for every pair implies the second, not conversely; every maximal-munch cut and inventory is independent, WordPiece and threshold-splice ones need not be                                                    | Theorems 2 and 3                                           |
| Where the class sits       | A regular token set with unbounded commit delay has no finite-state scanner yet a finite verifier; non-regular domains are outside; a regular domain is not sufficient                                                                                                                                                                                                               | Section 4, Lemma 1, Proposition 1                          |
| A normal form              | Markings are length-preserving, so finite verifiability coincides with rationality of the marking relation; undecidability sits in presentations that do not convert effectively to a verifier                                                                                                                                                                                       | Proposition 2, Section 4                                   |
| Closure                    | Pipelines over a labeled stage one, restrictions, splitter pre-tokenizers and byte-fallback unions of literal policies stay in the class and inherit all six decisions                                                                                                                                                                                                               | Theorem 4                                                  |
| Transfer                   | Certificates are monotone under restriction, forced at splitter boundaries, subset-bounded through pipelines, and lifted through pipelines exactly when a product of two stage-local automata is empty, with a per-label side condition as the sufficient corollary; a tower's certificate factors through its levels on the tower's own domain                                      | Propositions 5 to 7, Theorem 5, Corollary 1, Proposition 8 |
| Weighted theory            | Under rational, unifilar, almost surely stopping sources the tokenizability and differential masses of Section 7 are exact rationals, as are the unnormalized expected length and expected token, mismatch and boundary counts, and every rate whose expected length is positive; and differential mass is nonzero exactly when a boundary differential lies in the source's support | Theorem 6, Proposition 9                                   |
| Instances                  | Literal maximal munch, regular-token maximal munch, canonical byte-pair encoding and two-mode WordPiece carry the whole interface, each validated against an independent reference scanner or encoder; under maximal munch a certified window makes a cache keyed on its cut sound for continuations that keep the window and that the policy tokenizes completely                   | Section 2, Proposition 11                                  |

Table 1 summarizes the results and names the statements that carry them. Three points of scope hold throughout and are
stated here once. First, the theorems assume a *supplied* verifier, presented as its states, transitions, acceptance and
alphabet. A mere promise that some program computes a policy in the class is not enough, since the reductions of Section
4 leave the four questions without a uniform procedure. Second, membership in the class is determined by the marking
relation, whatever its presentation. A policy whose domain is not regular lies outside the class, but a regular domain
alone does not put it inside. Third, every validation sweep is bounded, in that it checks every case within its declared
universe and says nothing beyond it, so a sweep supports a theorem but never replaces its proof.

## 2 The definition and the interface

The whole paper programs against four methods, `start`, `step(state, byte, mark)` returning a successor state or
refusal, `accepts(state)`, and `alphabet`. An object with those four methods is a policy the theorems cover when it
presents a *finite, effective* verifier, which means finitely many reachable states and a finite alphabet, both
enumerated once per decision. The executable modules take the alphabet's symbols as one-character strings, and windows
and the strings a decision returns are spelled over them. The verifier's accepted language must be *functional*, at most
one accepted marking per byte string, and *well formed*, every nonempty accepted marking ending in a zero bit, the
final-zero convention, so that it encodes a segmentation. A byte outside an object's own alphabet leaves its domain by
definition, and every decision treats that side as dead without stepping its verifier, so the interface requires no
behavior of a verifier on a foreign byte. Functionality can also be checked at the interface. The self-product that
guesses two markings of one byte stream and latches their first disagreement is empty exactly when the object is
functional. The other half of being a policy is that every nonempty accepted marking closes with a zero bit, which is
read off the transitions into accepting states. Nothing below inspects a policy's internals.

A marked string is best read as a scan's transcript. Take the token set `{ab, b}` under maximal munch and the input
`bab`. Its unique segmentation is `b · ab`, and its marked string pairs the bytes `b`, `a`, `b` with the boundary bits
1, 0, 0, one bit per byte saying whether a token boundary follows that byte, so a set bit closes the current token and
the next one begins at the following byte. The bit after the final byte is always zero, since the string's end closes
the last token and a boundary there would open an empty segment. The verifier for this policy accepts exactly the marked
strings whose bits close segments that are tokens and never close one while a strictly longer token could still
complete, and it accepts nothing at all for an input like `ba`, which no segmentation covers. The splitting companion's
armed-run verifier carries the same information on the other side of each byte, a mark on every token start, position
zero included, so its mark at position `i+1` is the bit here at position `i`. A one-byte buffer translates either
encoding into the other, so finite verifiability does not depend on the choice. Everything the paper does is a question
about the regular language of such transcripts.

Two conventions about the empty input are fixed here because later constructions lean on them. First, the empty input
carries no bits and admits one candidate segmentation, the empty sequence, but whether that segmentation is in a
policy's domain varies. The three maximal-munch tokenizers below consume the empty input trivially and accept it. A
policy accepting nothing does not, and neither does a restriction to a schema that excludes the empty input. The
presentation-level reductions of Section 4 need policies of exactly that kind. The verifier answers that question from
its own start state, and it must, since Lemma 1 reads the policy's domain off the projection of the accepted language,
and that reading is right only if the automaton, and no fact recorded beside it, decides the empty input. Second, the
start state is kept distinct from every configuration reachable after a byte, which any start state no step re-enters
provides. The point is that whether nothing has been consumed is a fact about the run, which a configuration alone does
not record. A derivative automaton for `a(aa)*` returns to its initial configuration after `aa`, and reading the return
as an unconsumed state accepts two markings of that string. Under the normalization the start state's acceptance is
exactly the empty input's membership, and every product and enumeration below uses it.

Three policies whose scanners share no code carry the interface, as a test of its generality. For literal maximal munch,
a verifier run pairs a candidate token with the length consumed so far. At the next proposed mark the segment the run
spans must be accepted by some token, and from then on the run is armed. A run that completes a token while armed has
found a longer match past a commit, which greedy scanning forbids. For regular-token maximal munch, the runs are
Brzozowski derivatives of the token expressions, armed the same way, and the tested sets include `{a⁺b, a}`, which no
online scanner decides. For two-mode WordPiece, the verifier carries the mode bit, and a closed segment flips it to
continuation, which is the split of subword tokenizers between word-initial and mid-word vocabularies. Each verifier is
validated against an independent reference scanner, namely the direct greedy scan, a longest-language-match reference
over regular expressions, and the two-mode scan respectively, on every marking of every string of bounded length. In
each case the verifier accepts exactly the reference segmentation of each tokenizable string and nothing for the
untokenizable ones, over 1,530, 381, and 765 strings.

## 3 The four decisions

Four questions are decidable, three for every supplied verifier and equivalence for every pair of them, with the
counterexamples and constructive outputs stated below. The theorem states them and the proof gives one finite
construction per question. As the introduction states, the theorem's hypothesis is a given presentation, which mere
membership in the class does not supply. One phrase is fixed first, since the first decision and every transfer law of
Section 6 use it.

**Definition 2 (correct covering; certification).** For a window `W` of length `h` and an origin `0 <= o < h`, a policy
`P` *covers* an occurrence of `W` at position `p` of a string in its domain *correctly* when `p+o` is a token start of
`P`'s segmentation and no token start lies in `(p+o, p+h-1]`. Equivalently, the token covering the occurrence's final
byte begins exactly `o` bytes into it. `P` *certifies* `(W,o)` when it covers every occurrence of `W` in every string of
its domain correctly.

**Theorem 1 (the four decisions).** Let a well-formed functional finite verifier presenting a segmentation policy be
given as its states, its transition and acceptance functions, and its finite alphabet. The following are decidable,
uniformly in the verifiers given, and each answer comes with the object named.

1. **Certification.** Given a nonempty window `W` and an origin `0 <= o < |W|`, whether the policy certifies `(W,o)`;
   when it does not, a string of the domain with an occurrence it covers wrongly.
2. **Boundary gap.** Whether the length of a boundary-free stretch is bounded over the policy's segmentations; when it
   is, the exact supremum, and when it is not, a pumpable lasso of the verifier.
3. **Realizability.** Whether the domain is nonempty; when it is, the mask that admits at each *completable* state, one
   from which an accepted completion exists, exactly the steps after which an accepted completion remains, under which
   generation from any completable state never dead-ends.
4. **Equivalence.** Given two such verifiers, whether the two policies have the same domain and the same segmentation on
   it, and separately each half of that: whether they segment every string of their common domain alike, and whether
   their domains are equal. Each failure comes with a differing string, one in one domain and not the other or one in
   both domains segmented apart.

Three remarks fix the statement's terms. Throughout, *boundary gap* names the stretch of the second item, between
consecutive token boundaries, and its supremum. The leading stretch, from position zero to the first boundary, and the
trailing stretch, from the last boundary to the end of the input, count the same way, end of input being an implicit
close, and a string with no marked boundary is one stretch entire. A domain that is empty or holds the empty string
alone has no stretch, and its supremum is zero by convention, a value the decision returns although no stretch attains
it. Realizability guarantees that generation never blocks, and it says nothing about whether a run stops. An empty
domain answers no, and on a nonempty one the computed mask is the positive answer's output. The supplied verifier is
needed even for the boundary gap's yes-or-no, which Proposition 3 shows undecidable from a program promised to compute a
policy of the class.

Two worked decisions fix the ideas at their smallest. Over `{ab, b}`, the window `(b, 0)` is *refuted*, since the input
`ab` is one token whose `b` sits mid-token, and the decision's counterexample is that very string. The window `(ab, 0)`
is *certified*, since `ab` occurs in tokens only token-initially, so every occurrence in every tokenizable input begins
a token. The two halves of the equivalence decision separate cleanly on `{ab, b}` against `{ab, b, a}`. Adding the
one-byte token creates a domain difference, the differing string `a`, tokenizable only on one side, while the boundary
half finds no *boundary differential*, no commonly tokenizable string the two segment apart, because on every commonly
tokenizable string the added token never wins a longest match. The two policies are one segmenter on their common domain
and differ only in reach. Domain difference and boundary differential are independent findings, and the deciders
separate them with differing strings.

*Proof.* One shape proves all four. The verifier is finite, and each property is decided by a finite computation over
the verifier or a product with it, namely reachability or emptiness for certification and equivalence, a cycle search
and a longest path for the boundary gap, and a co-accessibility mask for realizability. Each returned string is read
back from its search path. Throughout, functionality is used freely. A policy assigns at most one segmentation per
string, and the verifier accepts exactly the graph, so an accepted marking *is* the policy's segmentation of its byte
projection.

*Certification.* Fix the window `W` with `h = |W|` and origin `o`. In a marked string the token covering position `j`
begins at the last token start at most `j`, which is why the two readings of Definition 2 agree. Correct covering of the
occurrence at `p` comes down to two facts about the `h` token-start indicators of the occurrence's positions. Build the
product of the verifier with a matcher that nondeterministically designates one occurrence of `W` in the byte projection
and records those bits as it passes. A state is violating when its designated occurrence has completed with the covering
condition false. The pair is refuted exactly when a violating state can complete to acceptance, which is reachability in
a finite graph, and the reachability path projects to a violating input with its own segmentation as the exhibit.

*Boundary gap.* Restrict the verifier to its live states, those reachable from the start and from which acceptance is
reachable, since only live states occur in accepted runs. Consider the subgraph of transitions whose mark bit is zero.
If it contains a cycle through a live state, pumping the cycle inside any accepting run through that state produces
accepted markings with arbitrarily long boundary-free stretches, so the boundary gap is unbounded. The output is the
lasso, a pumpable family, no single string exhibiting unboundedness. If it is acyclic, every boundary-free stretch is
scored off its zero-edge path with its closing byte counted where one exists. An internal stretch of `k` zero edges ends
in a mark-one byte and spans `k+1` bytes; a final stretch ends at accepting end of input and spans its `k` zero edges
alone, the terminal byte carrying the non-closing bit. The supremum is the larger of the two maxima over the finite
acyclic graph, computable and, wherever a stretch exists at all, attained. The empty and empty-string-only domains take
the conventional zero, which no stretch attains. Scoring the zero path alone would undercount every internal stretch by
its closing byte. The policy `{aab ↦ 010}` segments as `aa | b` with boundary gap two against a longest zero path of
one.

*Realizability.* Call a state completable, or co-accessible, when some marked suffix leads from it to an accepting
state. Completability is a reverse reachability fixpoint, computed once. The mask admits, at state `q`, exactly the
pairs `(σ, m)` with `step(q, σ, m)` completable. Suppose generation stands at a completable state `q`. If `q` accepts,
generation may stop; if not, completability of `q` names a marked suffix to acceptance, whose first step the mask
admits, so at least one admitted step exists and preserves completability. A masked generator therefore never dead-ends,
and the mask is computable from the fixpoint. The hypothesis that generation begins at a completable state is needed,
since a verifier accepting nothing is a policy of this class whose start state completes to nothing. Its mask offers
neither a stop nor a step, and the instrument refuses it, since a guarantee there would hold vacuously. Of the four this
is the shallow one, an implementation recipe once co-accessibility is computed, kept among the decisions because the
interface makes the guarantee available wherever the other three are.

*Equivalence.* Three terms are fixed first and used in these senses throughout. Boundary equivalence is agreement of the
two segmentations on the common domain. A domain difference is a string tokenizable under exactly one policy. Full
equivalence is boundary equivalence together with the absence of any domain difference, and the item asks about it
first, the reductions of Section 4 separating pairs on it with their differing strings on the domain half.

Full equivalence is language equality, and that is the first decision. Functionality gives a policy exactly one accepted
marking per string of its domain, so a verifier's accepted language is the policy's graph, the marked strings it accepts
being the pairs of a domain string and that string's own segmentation. Two policies are therefore fully equivalent
exactly when their accepted marked-string languages are equal. In one direction, equal languages have equal byte
projections, and those projections are the two domains. On a string both domains contain, the marking the first side
accepts lies in the second side's language too, and the second accepts exactly one marking of that string, so the two
segmentations agree there. In the other, unequal languages carry a marked string one side accepts and the other does
not, whose bytes either leave one domain or are segmented apart, and either way the policies are not fully equivalent.
Both verifiers are deterministic over the alphabet of (byte, bit) pairs, so this is equality of two deterministic
automata. Build the product over that alphabet, a byte outside one policy's own alphabet sending that side to a dead
state without its verifier being stepped, and search the product for a reachable pair in which exactly one side accepts.
Breadth-first search returns the shortest such marked string, or reports that no such pair is reachable. The search
visits each product state once, so the decision costs the product's size. Its states are the pairs of the two verifiers'
states with a dead state adjoined on each side, and out of each pair it has one edge per bit for every byte of the union
alphabet.

The two halves are decided separately, each by a product of its own, and the same products read a differing string for
the full decision. Which half a separation falls in is a further fact about it, the reductions of Section 4 separating
on the domain half and the pipeline applications asking which half they have. Boundary equivalence is decided by the
first product. Run both verifiers over one byte stream with independently guessed marks `(m_1, m_2)` and a flag that
latches when `m_1 ≠ m_2`. A byte outside one policy's own alphabet is outside its domain, so no jointly accepted stream
contains one, and this product walks the intersection of the two alphabets. If a run reaches joint acceptance with the
flag set, functionality turns the two accepted markings into the two policies' own segmentations of the same input, and
they differ at the latched position, which gives a boundary differential with the input synthesized from the path.
Conversely a differential input yields such a run by guessing both true markings. Emptiness of flagged joint acceptance
is boundary equivalence.

Domain difference is the other half, and it needs a product of its own, since the first accepts only strings both
policies tokenize. That product walks the union of the two alphabets, the side whose alphabet lacks the byte recorded as
dead while the other runs on. Each policy's domain is the byte projection of its verifier's language, regular by
projection, so the difference of the two domains is regular and its emptiness decidable. The construction that yields a
differing string is the subset product. Track, for each side, the set of verifier states its surviving marking guesses
can occupy after the bytes read so far, which is that side's subset construction and finite. The string read so far is
in one side's domain exactly when some state in its set accepts, so a reachable pair of sets in which one side accepts
and the other does not marks a differing string, and the bytes labeling the path to it reconstruct the string.
Breadth-first search over pairs of subsets terminates and returns the shortest such string, or reports that the domains
coincide. ∎

The generic deciders are validated against the purpose-built ones they generalize. Certification is checked against the
pending-automaton product on 36 triples, and equivalence against the armed-run product on 9 regex pairs with every
differing string re-verified. The boundary gap is checked against the known literal boundary gaps, that is, each set's
longest token, and against the unbounded verdict on the unbounded-delay set, and realizability is fixpoint-clean on one
policy of each kind. One validation only the interface can express is that a literal policy and the regular policy
denoting the same token languages are proved equivalent across representations. For maximal munch the splitting
companion's two instruments, the product of two pending automata for literal policies and the armed-run product for
regular ones, are validated against brute force on 225 and 25 ordered pairs. The language-equality decision is held to
the two-half decision on all 251 pairs, the two pools together with the cross-representation pair. One product over
marked strings returns a differing string exactly where the two-half decision returns one, and each of the 228 shortest
differing strings is replayed against the reference scans, 212 of them separating the domains and 16 the boundaries.

The boundary gap is not the anchor-free span of the splitting companion, which counts the longest run of positions
carrying no anchor from a fixed inventory and can be unbounded where the boundary gap is bounded. Over the greedy token
set `{aa, a}` the longest boundary-free stretch is two, the longest token, while every unary window `a^k` occurs in
`a^(k+1)` at starts of both parities, so by parity no window fixes the offset at which its covering token starts, and
the anchor-free span is unbounded. The execution refutes every one of the 10 window-and-origin pairs over that set's
one-letter alphabet through length four.

**Theorem 2 (cut independence).** Let a well-formed functional finite verifier presenting a policy `P` be given as in
Theorem 1, and let `(W, o)` be a pair it certifies. It is decidable whether every certified cut is independent: whether
for every `x` in the domain and every occurrence of `W` in `x` at `p`, with `c = p + o`, `u = x[0..c)` and `v = x[c..)`,
the prefix `u` lies in the domain with `P(u)` equal to `P(x)` restricted to `u`, and the suffix `v` lies in the domain
with `P(v)` equal to `P(x)` restricted to `v` and shifted by `c`. When some certified cut is not independent, a shortest
such `x` is returned with the occurrence and the failing side or sides.

A certificate makes `c` a boundary of the whole string's segmentation, and that is all it says. Whether each side scans
alone is a further question, and the decision separates the two. The construction is a product over the verifier in two
phases. Before the cut it carries the verifier's state and a matcher for `W[0..o)`, so that the cut, placed after a
marked byte, is where an occurrence may begin `o` bytes earlier. At a nonzero cut the product holds two states for the
byte immediately before it. The whole-string run reads that byte with bit one and continues, while the prefix test reads
the same byte from the same state with bit zero, the last bit of the marked `u` under the final-zero convention, and
asks whether the state reached accepts, which by functionality holds exactly when `P(u)` is `P(x)` restricted to `u`. A
failure is latched and the search goes on, because a policy whose cut state does not accept can still have `x` in its
domain. A cut at `0`, from an occurrence at `0` of a window certified at origin `0`, has no byte before it, so both
copies then begin at the start state, and the empty prefix fails exactly when the start state does not accept. After the
cut the same marked suffix is read by two copies of the verifier, one continuing and one restarted from the start state,
while a matcher confirms that `W[o..)` follows at once. The restarted copy accepts exactly when `P(v)` is `P(x)`
restricted to `v`. A path on which the occurrence has completed and the continuing copy accepts, with the flag set or
the restarted copy rejecting, projects to a counterexample with its own segmentation, and breadth-first search returns
the shortest. With `h = |W|`, the product has at most `|Q|(o+1) + 2|Q|(|Q|+1)(h-o+1)` states, each with at most `3|Σ|`
out-edges, the cut edges beside the two bits. Over 494 small universes the decision is held to brute force on 5,627
certified pairs, every verdict equal through length 8 and all 62 counterexamples replayed by the reference scans. On
every certified pair of every literal, regular and byte-pair policy, 5,256 pairs of 469 literal sets, 56 of the five
regular sets and 48 of the five byte-pair towers, one literal maximal-munch stage per merge, every cut is independent,
which is the splitting companion's theorem for maximal munch executed, a tower of maximal-munch stages inheriting it
boundary by boundary. It fails where a scan carries state across a boundary that the suffix alone cannot recover, as it
does on 21 of the 59 certified WordPiece pairs, the restarted scan beginning in the initial mode, and on 41 of the 68
certified pairs of the six threshold splices of Section 4, a side shorter than the threshold segmented under the other
policy. Over `{a, ab}` with continuation `{b}` the pair `(bb, 1)` is certified and `abb`, segmented `ab · b`, is cut at
`2` into a prefix the policy segments the same way and a suffix `b` that is not in its domain at all. For a policy that
is not maximal munch, splitting at every cut of an inventory, a set of certified pairs, needs more than the
certificates, and the property it needs is exactly the one the next theorem decides.

**Theorem 3 (chunk independence).** Let a well-formed functional finite verifier presenting a policy `P` be given as in
Theorem 1, and let `I` be a nonempty finite set of pairs it certifies, an *inventory*. It is decidable whether `I` is
chunk-independent: whether for every `x` in the domain, with anchors `c_1 < ... < c_n` the positions `p + o` for every
pair `(W, o)` of `I` and every occurrence of `W` in `x` at `p`, each of the chunks `x[0..c_1)`, `x[c_1..c_2)`, ...,
`x[c_n..)` lies in the domain with `P` of it equal to `P(x)` restricted to it and shifted by its start, an anchor at `0`
leaving an empty first chunk, in the domain exactly when the verifier accepts its start, and a string with no anchors,
`n = 0`, its own sole chunk. When `I` is not chunk-independent, a shortest such `x` is returned with its anchors and the
chunks that fail. If every pair of `I` is cut-independent in the sense of Theorem 2 then `I` is chunk-independent; the
converse does not hold, even for an inventory of one pair.

Parallel scanning cuts at every anchor of an inventory and scans each chunk alone, and a middle chunk has a cut at both
ends, so what it needs is not the property of Theorem 2 taken pair by pair. The implication is nonetheless short. A
string with no anchors is its own sole chunk, which lies in the domain with its own segmentation. Otherwise the first
and the last chunk are the sides of the first and the last cut. For a middle chunk `x[c_i..c_(i+1))`, the prefix
`x[0..c_(i+1))` lies in the domain segmented as `x` is, by the pair anchoring `c_(i+1)`. The occurrence anchoring `c_i`
lies within that prefix, since its certificate makes the token covering the window's final byte begin at `c_i` and that
token ends at or before the boundary `c_(i+1)`. The pair anchoring `c_i`, applied to the prefix, therefore puts the
chunk in the domain segmented as the prefix is. The converse fails because the other anchors, another pair's or the
window's own overlapping occurrences, can cut a string so finely that no chunk shows the misreading a whole side would.
Over the initial tokens `{a, ab, b}` with continuation `{a, b}` the pair `(aa, 1)` is certified and not cut-independent,
the suffix `ab` of `aab` reading as one token alone, while the inventory `{(aa, 1), (aab, 2)}` is chunk-independent, the
second pair anchoring the `b` of every `aab` so that every chunk a restarted scan could read differently is the single
byte `a`. Over `{a, aa, ba}` with continuation `{a}` the pair `(aaa, 2)` is certified and not cut-independent, the
suffix `aa` of `aaaa` reading as one token alone, while the inventory of that one pair is chunk-independent, its
overlapping occurrences anchoring every position of a run of `a`'s from the third on, so that every chunk after the
first is a single byte. The decision is a product over the verifier with two copies, one reading `x` and one reading the
current chunk, the second fed with a lag of `h` bytes, `h` the length of the longest window, so that when a byte reaches
it every occurrence that could anchor the position after that byte has completed. At an anchor the chunk copy reads the
byte with bit zero, is accepted or refused, the refusal latched, and is restarted. Every state is a candidate end of
`x`, the held bytes committed with no more to come, and the state is violating when the copy reading `x` accepts and a
chunk was refused. A state is the two copies, the up to `h - 2` bytes kept ahead of the lagged ones so that every
occurrence can be matched, the up to `h` marked bytes the lag holds, a bit for the first commit and the flag, so the
product has at most `4|Q|(|Q|+1) sum_{k=0}^{max(h-2,0)} |Σ|^k sum_{j=0}^{h} (2|Σ|)^j` states with `2|Σ|` out-edges each,
a count every product of the sweep is checked against. Over 498 small universes the decision is held to brute force on
29,532 inventories of one to three certified pairs, and 29,530 verdicts agree with brute force through length 8, while
the remaining two are refutations whose counterexamples have length 9, past the brute-force bound. All 1,588
counterexamples replay on the reference scans, the 1,586 within the bound, at the brute-force shortest length. The
27,896 inventories with every pair cut-independent are all chunk-independent, and 48 inventories are chunk-independent
with a pair that is not, 3 of them of one pair. Every inventory of every literal, regular, threshold-restriction and
byte-pair policy is chunk-independent, 23,807 inventories over the 469 literal sets, 784 over the five regular sets,
1,541 over the six restrictions and 916 over the five byte-pair towers, which is the splitting companion's theorem for
maximal munch extended to inventories; 442 of the 485 WordPiece inventories and 822 of the 1,577 threshold-splice
inventories are not, for the reasons the single cuts fail. An inventory is fit for parallel scanning exactly when this
decision says so. For a policy that is not maximal munch this decision is the question to ask, cut independence of its
pairs being only the sufficient condition that Theorem 2 checks one pair at a time.

## 4 Where the class sits

Finite verifiability would add nothing if it coincided with online scannability. It does not, since the class contains
every online-scannable policy and strictly more, and it is contained in the policies with a regular domain and strictly
fewer. Over the marking encoding it is exactly the rational segmentation policies. The undecidability of general
transducers concerns relations that are not functions, and what remains undecidable here sits in how a policy is
presented.

**Online scanners are inside.** An *online scanner* here is a deterministic finite-state device reading bytes left to
right and emitting each boundary bit within a bounded delay. A device with unbounded buffering is left outside the term,
since with unbounded memory the question dissolves. Every online scanner's policy is finitely verifiable. Run the
scanner beside the proposed marks, hold the last `d` bits, and compare each emitted bit when its delay expires. The
buffer is finite, so the inner region of Figure 1 sits inside the class by construction. Some policies have no online
scanner yet are finitely verifiable. Over the token set `{a⁺b, a}`, the scan of `a^n` commits nothing until the input
ends, and the scan of `a^n b` commits everything as one token. The first commit's delay grows with `n`, so no online
scanner in the sense above decides it. The armed-run verifier covers the set regardless, validated on 1,020 strings, 255
for each of four regex sets, with every candidate marking tried, and certification violations pump along verifier states
exactly where no online state exists. The separation is not new, and neither is this kind of example. Karttunen (ACL
1996) observes the same obstruction for the replacement of `a⁺b` by `x`, whose transducer is unambiguous and yet cannot
be sequentialized, because no finite-state device can accumulate an unbounded amount of delayed output. The four
decisions here hold on both sides of that frontier.

**The frontier is decided.** Beyond the one example, the frontier is decided, and by classical means. A scanner has
delay `k` when it emits the bit of each position after reading at most `k` further bytes, the bits still pending at end
of input being emitted there. Replace every verifier edge `(σ, m)` by the transducer edge `σ | m` and trim the result.
This transducer `T_V` is letter-to-letter by construction and, since the verifier is functional, realizes the policy's
partial boundary function, which is length-preserving. By Choffrut's theorem (Choffrut, Theoretical Computer Science
1977), in the form Béal and Carton (Theoretical Computer Science 2002) state it, that function is subsequential exactly
when `T_V` has the twinning property, which is decidable in polynomial time in the size of `T_V` (Weber and Klemm,
Information and Computation 1995; Béal, Carton, Prieur and Sakarovitch, Theoretical Computer Science 2003). When it
fails, the failure is shown by a pair of same-input paths from the initial state reaching two states together with
same-input loops at them along which the relative output delay changes. When the test succeeds, take a trim
subsequential realization. Every cycle on one of its successful runs reads as many bytes as it writes bits, since
pumping a cycle that did not would produce an accepted pair of unequal lengths, so the lag between input and output is
bounded. Frougny and Sakarovitch (Theoretical Computer Science 1998) turn a subsequential transducer of bounded delay
into an on-line automaton, one that is letter-to-letter after an initial period, which is a scanner emitting each bit
within a bounded delay. Conversely, a bounded-delay scanner with its buffer of pending bits is a subsequential
transducer of bounded delay. Whether a supplied verifier's policy lies in the inner region of Figure 1 is thus decidable
in polynomial time from the verifier. Inside that region the delay is decided level by level. Projecting `T_V` onto its
bytes, each edge keeping its bit, gives a nondeterministic automaton whose accepting runs are unambiguous by
functionality. After trimming, two edges with the same byte, source and target cannot carry different bits, since a
common path into the source and out of the target would otherwise give one byte string two accepted markings, so the
projected automaton's choice of successor determines the bit. In the other direction, a scanner with delay `k` fixes the
bit from the state and the lookahead alone. Let `q` be a state of the trimmed automaton, `u` a byte string reaching it,
and `v` and `v'` continuations from `q` to acceptance that agree on their first `k + 1` bytes. The words `uv` and `uv'`
lie in the domain, each with one marking, and they agree through the byte at which a delay-`k` scanner emits the bit of
the byte after `u`, so the scanner emits the same bit for both, and that bit is the one each marking carries there. The
bit at `q` is therefore a function of `q` and the next `k + 1` bytes, and by determinism of `V` so is the successor.
When fewer than `k + 1` bytes remain, the lookahead is the whole remaining suffix, and acceptance is tested on that
suffix alone and on no longer continuation it begins. If the input ending there is accepted, its one marking fixes the
bit and the successor, and if it is not, no accepted run passes this way and the choice is free, as it is for any
lookahead that begins no accepted continuation. A missing transition goes to a rejecting sink, which makes the automaton
total as the definition reads it. A scanner with delay `k` is exactly a `k`-lookahead delegator for that automaton,
choosing the successor forced on every accepted continuation that shares the lookahead, which Löding and Repke (FSTTCS
2013, Corollary 8) decide in polynomial time for each fixed `k` over the fixed byte alphabet. A later erratum finds
Lemma 3 of that paper faulty, confirms Lemma 7 and Corollary 8, and proves Corollary 8 without that lemma (Löding, note
of 22 February 2023). The levels are distinct, since for `k >= 1`, maximal munch over `{a, a^k b}` has delay exactly
`k`. A token starting at a byte is `a^k b` exactly when the next `k` bytes are `a^(k-1) b`, so `k` further bytes settle
every bit, and fewer do not, since `a^(k+1)` and `a^k b` share their first `k` bytes and differ in their first boundary
bit. This paper uses none of that, since verification needs no scanner, and states it so that the hypothesis is placed
where it belongs, one ring outside a class the transducer literature already decides.

**Non-regular domains are outside.** The hypothesis has a hard outer boundary.

**Lemma 1 (non-regular domains are outside).** If a policy's set of tokenizable strings is not a regular language, the
policy is not finitely verifiable.

*Proof.* The unmarked projection of a regular marked-string language is regular, and the projection of the verifier's
accepted language is exactly the policy's domain. ∎

By the lemma, balanced-bracket segmentation sits outside the hypothesis, and so does any policy whose domain is not
regular. The lemma draws that line and no more. In particular it says nothing about how a policy is presented.
Membership is determined by the marking relation, so a stack-defined device is neither enough to exclude a policy, if
the relation it computes is regular after all, nor enough to include one. We claim no parser-adjacent policy on either
side here. The line is not tight from the other direction, because a regular domain is the first thing a reader will
reach for as a sufficient condition.

**Proposition 1 (a regular domain does not suffice).** There is a segmentation policy whose domain is the regular
language `{a, b}*` and which is not finitely verifiable.

*Proof.* Take the tokens `{a, b, ab}` and the policy that reads a leading `ab` as a single token exactly when the rest
of the input has the form `a^n b^n`, and otherwise cuts every byte into a token of its own. Every string is tokenizable,
since `a` and `b` are tokens, so the domain is all of `{a, b}*`. The bit following the first byte is zero on the strings
`ab a^n b^n` and, by the final-zero convention, on the two one-byte strings, and on nothing else. If the policy's marked
strings formed a regular language, intersecting it with the regular set of markings whose first bit is zero and
projecting the bits away would make the set of those strings regular. But a regular language stays regular when two
strings are removed from it, and what remains is `ab a^n b^n`, which is not regular. ∎

The domain obstruction is necessary but not sufficient. The domain is the marking relation's projection, so a
non-regular domain forces a non-regular relation and puts a policy outside by Lemma 1, while a regular domain, as the
example shows, does not bring a policy inside. A supplied verifier does, and it is the hypothesis the theorems use.

**The class is a normal form.** It is tempting to place this class below a frontier of rational transductions, where
Filiot, Jecker, Löding and Winter (ICALP 2016) record inclusion, equivalence, and sequential uniformization as
undecidable in general, and to say that finite verifiability is the restriction buying the decisions back. That reading
does not survive its own encoding. A boundary marking pairs each byte with exactly one bit, so the relation carrying an
input to its marking is length-preserving, and by Eilenberg and Schützenberger's theorem (Eilenberg, Academic Press
1974) a length-preserving rational relation over `A* × B*` is a rational subset of `(A × B)*`, equivalently realizable
by a letter-to-letter transducer. A rational subset of `(A × B)*` is a regular language of marked strings, which is a
finite verifier. The converse is immediate, since a finite verifier over marked strings is already a letter-to-letter
transducer, hence rational. Over this encoding the two classes coincide, and there is no region between them for a
frontier to occupy.

**Proposition 2 (collapse).** For policies presented by boundary markings, one bit per byte, a segmentation policy's
marking relation is rational if and only if the policy is finitely verifiable.

*Proof.* A finite verifier is a deterministic automaton over the alphabet of (byte, bit) pairs, so its language is a
rational subset of `(A × B)*` and the relation it defines is rational. For the converse, the marking relation is
length-preserving by construction, so by Eilenberg (Academic Press 1974) it is a rational subset of `(A × B)*`. The same
step can be checked in Elgot and Mezei (IBM Journal of Research and Development 1965). By their Theorem 6.1 (p. 58), a
rational relation being a transduction in their terms, every bounded transduction, a length-preserving one among them,
is finite-automaton definable, and by their Proposition 4.4 (p. 55) a length-preserving finite-automaton definable
relation is defined by an elementary automaton, every edge of which carries one letter per tape (their Definition 4.1,
p. 55). The rational subset is a regular language of marked strings, and determinizing an automaton for it yields a
verifier. ∎

The finite-verifier discipline is not a restriction chosen to make the questions answerable but a normal form for
rational segmentation policies, and the four decisions apply to every one of them. The undecidability the general
transducer results describe belongs to relations that are not functions, and a policy's marking relation is a function
by definition, which is why its equivalence was decidable by Blattner and Head (Journal of Computer and System Sciences
1977) before any encoding. Length preservation adds the normal form, one verifier interface on which all four decisions
and the calculus run.

**Undecidability sits in the presentation.** That is the reason for the interface of Section 2, and the undecidability
reaches all four decisions. Every decision here is uniform in its *given* verifiers, whose four methods are the input
and from which the products are built. No decision is uniform in a policy presented as an arbitrary program promised to
compute one. Effective finite presentations, regular expressions, automata, or letter-to-letter transducers among them,
convert uniformly into verifiers and are not the obstruction. Fix a machine `M` and an input `w`, and build threshold
policies that simulate `M` on the fixed `w` for `|x|` steps, so that which branch governs `x` depends on `|x|` alone.
For each fixed `M` the result is one branch everywhere or a regular length-threshold splice of two, a *threshold
splice*, hence finitely verifiable either way. Three splices then carry the four decisions. Splice singleton splitting
over `{a, b}` into greedy scanning over `{ab, a, b}` after the halt. The splice flips certification of `(ab, 1)`, true
on the first branch and refuted on the second. It also moves the exact supremum the boundary gap decision returns from
one to two, both branches bounded, so what the splice reaches is the supremum the decision returns and not boundedness
alone. Deciding either from the program would decide halting. The simulated machine is normalized to halt only after at
least one step, so the first branch always governs some input. Splicing refusal into singleton splitting, so that only
inputs past the halting step are tokenized, makes the domain empty exactly when `M` never halts, so realizability
decides halting. The third splice compares singleton splitting with its splice into refusal after the halt, which is its
restriction to inputs shorter than the halting step. The two are fully equivalent exactly when `M` never halts, so
equivalence decides halting too. The simulation must run on the fixed `w`, since simulating `M` on `x` would make the
halting set a language that need not be regular, and the constructed policy would not be finitely verifiable at all. The
shipped instruments separate each pair of branches, so each reduction is also checked by execution. Existential
membership in the class is not a usable hypothesis; a supplied verifier is, and every theorem here is stated of the
verifier presentations it names. Figure 1 draws the resulting landscape.

The first splice keeps both branches bounded, so it shows that no procedure computes the supremum from a program and
leaves open whether boundedness alone could be decided that way. Another branch after the halt closes that question, and
the certificate `(ab, 1)` still flips.

**Proposition 3 (boundedness from a program).** For a machine `M` and an input `w`, let `P_(M,w)` be the policy over
`{a, b}` that splits a string `x` into single bytes when `M` does not halt on `w` within `|x|` steps, and otherwise
takes a nonempty `x` as one token, as regular maximal munch over the token language `{a, b}⁺` does. Each `P_(M,w)` is
finitely verifiable. If `M` never halts on `w`, then `P_(M,w)` certifies `(ab, 1)` and its boundary gap is one; if `M`
halts on `w`, then `P_(M,w)` refutes `(ab, 1)` and its boundary gap is unbounded. Hence no procedure that receives a
program promised to compute a finitely verifiable policy decides whether the policy's boundary gap is bounded.

*Proof.* Both branches tokenize every string over `{a, b}`, so the domain is all of them. If `M` never halts on `w`,
every string is split into single bytes, the accepted markings are the empty string and the markings `1^(n-1)0` for
`n >= 1`, a regular language of marked strings, and every boundary-free stretch is one byte, so the boundary gap is one.
Every position is then a token start, so an occurrence of `ab` at `p` has a token start at `p+1` and none after it
inside the occurrence, and `(ab, 1)` is certified. If `M` halts on `w` after `t >= 1` steps, the strings shorter than
`t` are split into single bytes and every other string is one token. The accepted markings are the empty string, the
markings `1^(n-1)0` of the strings of length `n` with `1 <= n < t`, and the all-zero markings of the strings of length
at least `t`. That language of marked strings is regular, a finite set beside the all-zero markings of length at least
`t`, it gives each string one marking, and every nonempty marking in it ends in a zero bit, so a deterministic automaton
for it, built from `t` alone, is a verifier presenting `P_(M,w)`. Every string of length `n >= max(t, 2)` is one
boundary-free stretch of `n` bytes, so the boundary gap is unbounded, and such a string beginning with `ab` has its
position `1` inside the token that begins at `0`, so `(ab, 1)` is refuted. A program for `P_(M,w)`, which simulates `M`
on `w` for `|x|` steps before choosing the branch, is computed from `M` and `w`, so a procedure deciding boundedness of
the boundary gap from such programs would decide whether `M` halts on `w`. ∎

**Figure 1.** The landscape the hypothesis draws. Verification is strictly weaker than scanning, and the frontier
between them is decidable. Every policy with a non-regular domain, the exhibited context-free segmentation among them,
is provably outside. The theorems hold on the whole middle region, which is exactly the rational segmentation policies.

```
segmentation policies
  finitely verifiable, equivalently rational by Proposition 2: the four decisions hold, closed under the calculus
    online scannable: bounded delay, decidable from the verifier
      literal maximal munch
    {a⁺b, a}: verifiable, not scannable
  balanced brackets: outside by Lemma 1
```

## 5 The calculus

Real systems rarely run one policy. A pre-tokenizer splits at class changes, a subword stage re-segments each run, a
schema restricts the inputs a stage can see, and a byte fallback totalizes a partial vocabulary. The class is closed
under all of it.

**Definition 3 (labeler).** A *labeler* for a policy is a deterministic finite automaton over bytes, run from its
initial state on each segment's bytes, that names the segment when it closes by a map from its states to a finite name
alphabet. A literal vocabulary supplies a labeler directly, by spelling, and a regular one by least accepting index. A
finite token alphabet alone does not, so the labeler is data that a stage's presentation must supply.

**Theorem 4 (closure).** Finitely verifiable policies are closed under four operations. A *pipeline* over a stage one
presented with a labeler feeds each closed stage-one token to a second policy as its name. A *restriction* limits the
inputs to a regular schema. A *splitter pre-tokenizer* forces boundaries at character-class changes and runs the inner
policy on each run in isolation. A *fallback union* totalizes a literal vocabulary with single-byte tokens. Each
composite verifier is constructed explicitly, and every composite inherits the four decisions of Theorem 1 unchanged,
together with the cut and chunk independence decisions of Theorems 2 and 3 for the pairs it certifies and the
inventories of them.

*Proof sketch.* Pipelines are the construction with content. Pipeline composition requires a supplied finite-state
labeler with a finite name alphabet, and stage two reads that alphabet. A literal vocabulary can use its token spellings
as names, so a spelling labeler's alphabet is stage one's finite token alphabet. A regular vocabulary may have
infinitely many spellings, the closed lexemes `a^n b` of a regular stage among them, which no finite composite state
could carry, so its presentation must supply an effective finite-state labeler with finite range. The construction takes
the labeler as a parameter, so the closure holds for every labeler a stage is presented with.

Under the hypothesis a composite marked string carries only the composite boundaries, so the composite verifier must
judge stage one's segmentation without seeing its marks. It runs the subset construction on guessed marks, tracking the
finite set of triples of stage-one verifier state, labeler state, and stage-two verifier state consistent with the bytes
read and the composite marks seen, beside one bit recording whether any byte has been read at all. Whenever a tracked
guess closes a stage-one segment, the labeler's name for it is fed to stage two and the labeler resets. At end of input
a tracked triple that has read something feeds its final segment's label to stage two with the non-closing bit before
asking both stages' acceptance; one that has read nothing asks both stages about the empty input and feeds no label. The
consumed-input bit is the construction's own memory, never an inspection of the inner policy's start representation,
which no interface-faithful account may perform. The tracked set is an element of the power set of a finite cube, so the
composite verifier is finite, and finiteness is the closure proof. Each composite verifier accepts exactly the composite
policy's markings, so it is functional and well formed, and Theorems 1 to 3, which take any such verifier, apply to it
unchanged. Acceptance requires a tracked triple in which both stages accept.

The other three constructions are short. Restrictions are the product with the schema's automaton, marks invisible to
the schema side. Splitters force a boundary at every character-class change and run the inner policy on each run in
isolation, and their composite verifier carries a one-byte lookahead buffer, enough to see the class change that forces
a boundary before committing the bit. At a forced boundary the composite bit is one, and the buffered byte reaches the
inner policy with bit zero, since its run ends there, so the inner policy must accept before it restarts on the next
run. Fallback unions are set union of a literal vocabulary with the byte alphabet, a plain policy again, total by
construction. ∎

Each construction is validated against a composite reference scan on every marking of every small string. The regular
pipeline, its labeler naming each closed segment by its least accepting language index, the same priority the reference
scan uses, is exact on 5,080 strings, the literal pipelines on 1,020 strings across four pipeline pairs, and the
splitters on 3,094 across three splitter configurations. Given the labeler that spells a segment back, the composite is
decided boundary-equivalent to the literal pipeline on every staged pair, and each of the two matches the same two-stage
reference scan over 508 strings. Given a labeler that names a segment by its length class alone, forgetting which bytes
it read, stage two receives an alphabet coarser than the inner token set, which the literal form cannot be handed. That
difference is one of alphabets only. The composite built over the length-class labeler is decided equal to the two-gram
atom over the alphabet, the single-stage policy the composite flattens to, and the literal form reaches that same atom
by another route, so a coarser labeler shrinks the stage-two alphabet without leaving the literal form's reach.

Definition 3 says the labeler is part of stage one's presentation and not a consequence of it. The following proposition
makes that precise, and the theorem's proof does not use it.

**Proposition 4 (a boundary verifier does not determine the names).** A finite verifier for a policy's boundaries,
together with the fact that its token names are finitely many, does not determine which name a closed segment carries.
Two ordered families can present one policy, so that a single verifier accepts the markings of both, and still induce
labelers that disagree on every segment. A second stage then tells the two pipelines apart.

*Proof.* Take the ordered family, a finite list of token languages read in priority order, whose first token language is
`{a}` under the name `X` and whose second is `{a}` under the name `Y`, over the alphabet `{a}`. Nothing in the
definition of an ordered family forbids repeating a language under two names, and read as a literal vocabulary it would
carry one name twice, so it is necessarily a named regular family. Maximal munch with list priority closes a segment at
every byte and names it `X`. Reversing the order closes a segment at every byte and names it `Y`. The two presentations
have the same domain and the same boundary function, so they are one policy in the sense of Definition 1 and one
verifier accepts the markings of both, while the names differ at every segment. A stage two that accepts `X⁺` and
refuses any `Y` then has domain `a⁺` under the first labeler and the empty domain under the second, so the composite
differs. Names are not scarce here, since there are two of them, and the obstruction is that the boundary graph records
where segments close and not which language closed them, so priority among equally matching languages is the information
it drops. ∎

The calculus has two immediate uses. First, a two-stage pipeline is proved to agree with the single-stage atom it
flattens to, by the generic boundary decider of Theorem 1 on the common domain, which for these total policies is
equivalence. With one pair removed the pipeline is proved to differ from that atom, the differential `bb` synthesized by
the decider. Second, certification becomes *schema-relative*. The window `(b, 0)` is refuted globally over `{ab, b}` and
certified on the `b`-only schema, by the same decider on both sides of the restriction, so a certificate's meaning under
a data contract is computed by the same decider.

## 6 Stage-local certificates

Certificates computed for one stage survive composition under four laws. Each is proved below, and each is cross-checked
by execution on finite cases against the deciders or, for the boundary-subset law, against an independent reference
scan, the checks named where their counts are quoted. Covering an occurrence correctly, and certifying a window, are the
terms of Definition 2, under which the position `p+o` the origin names is a token start and no token start follows it
inside the occurrence.

**Proposition 5 (monotonicity).** If `P` certifies `(W,o)`, so does its restriction to any regular schema `K`.

*Proof.* The restriction's domain is `dom(P) ∩ K` and its segmentation of each string is `P`'s, so every occurrence it
must cover is one `P` already covers correctly. ∎

Restriction only adds certificates. The global certified set is a subset of the restricted one on all 12 staged cases, 7
strictly, so a global certificate is reusable under every schema.

**Proposition 6 (splitter forcing).** Let `S` be the splitter over a class function `κ` and any inner policy, and let
`W` have length `h >= 2` with `κ(W_(h-2)) ≠ κ(W_(h-1))`. Then `S` certifies `(W, h-1)`.

*Proof.* The splitter cuts its input into maximal same-class runs and segments each run in isolation, so every token
lies inside one run and every position at which the class changes is a token start. At an occurrence of `W` at `p` the
bytes at `p+h-2` and `p+h-1` differ in class, so `p+h-1` is a token start, and since it is the occurrence's final
position no token start lies beyond it inside the occurrence. ∎

A window whose final byte follows a class change is thus certified at that final position whatever the inner policy
runs, on all 504 staged final-position cases. That is the splitting companion's anchor supply turned into a closure
operation. The stronger any-offset statement is false, and the paper's own decider refutes it, while the final-position
statement holds.

**Proposition 7 (boundary subset).** For a pipeline `P = P_2 ∘_λ P_1` over a labeler `λ`, `dom(P) ⊆ dom(P_1)`, and for
every `x ∈ dom(P)` the token starts of `P(x)` are a subset of those of `P_1(x)`.

*Proof.* By the construction of Theorem 4, `P` is defined where `P_1` is and `P_2` accepts the label stream
`λ*(P_1(x))`, and the pipeline segments `x` by grouping consecutive stage-one segments as `P_2` groups their labels, so
every composite token starts where some stage-one segment starts. ∎

A pipeline only removes boundaries, on all 363 composite-tokenizable strings of the staged pairs, so a composite
certificate never lands off an inner boundary. The fourth law is a decision rather than an inequality. Write `Γ` for the
labeler's finite name alphabet. A *flagged stream* is a word over `Γ × {0,1}` with exactly one flag set. When a flagged
stream is the label stream of `P_1(x)` flagged on segment `k`, it records that `x`'s `k`-th stage-one segment is the one
in question.

**Theorem 5 (the lift).** Let `P = P_2 ∘_λ P_1` and fix a window `(W,o)`. Let `R ⊆ (Γ × {0,1})*` be the set of flagged
streams obtained by taking `x ∈ dom(P_1)` and an occurrence at `p` of `W` in `x` that `P_1` covers correctly, and
flagging `λ*(P_1(x))` on the segment that begins at `p+o`. Let `L ⊆ (Γ × {0,1})*` be the set of flagged streams in
`dom(P_2)` whose flagged index is not a token start of `P_2`'s segmentation. Both are regular, `R` effectively from
`P_1`'s verifier, the labeler and the window, and `L` from `P_2`'s verifier alone. A stream in `R ∩ L` is the flagged
stream of some string on which `P` refutes `(W,o)`. If `P_1` covers every occurrence of `W` in every string of `dom(P)`
correctly, in particular if `P_1` certifies `(W,o)`, then `P` certifies `(W,o)` if and only if `R ∩ L = ∅`.

*Proof.* *`R` is regular.* Run `P_1`'s verifier on bytes with guessed marks, the labeler beside it, reset at every
closed segment, and beside both the matcher of the certification proof, which tracks the window's match progress and the
token-start indicators of the last `h` positions. Designate one occurrence nondeterministically at its final byte,
provided the indicators show a start at the origin's position and none after. The segment open at that byte is then the
one beginning at `p+o`, and it is flagged when it closes, at that byte's own mark, a later one, or end of input. Emit
each closed segment's label with its flag. The device is finite, so its accepted runs form a regular language, and `R`
is the image of that language under the erasing morphism that keeps the emissions, regular again. *`L` is regular.* Run
`P_2`'s verifier on the stream with guessed marks, carrying whether the next index is a token start, true at index zero
and thereafter the previous mark. On the flagged symbol, latch whether that bit was false. Accept when `P_2` accepts
with the latch set, and project the marks away.

*Refutation.* Let `(γ, k) ∈ R ∩ L`, and take `x ∈ dom(P_1)` and an occurrence at `p` whose cut `p+o` begins segment `k`
with `(γ, k)` as the stream flagged there, which exist since `(γ, k) ∈ R`. The labeler need not be injective, so `x` is
one such string and not necessarily the only one. Since `γ ∈ dom(P_2)`, `x ∈ dom(P)`; since `k` is no token start of
`P_2(γ)`, `p+o` is no token start of `P(x)`, and `P` covers that occurrence wrongly.

*Certification.* Suppose `R ∩ L = ∅`, and take `x ∈ dom(P)` with an occurrence at `p`. By hypothesis `P_1` covers it
correctly, so `p+o` begins some segment `k` and the stream flagged there lies in `R`. That stream lies in `dom(P_2)` and
not in `L`, so `k` is a token start of `P_2`'s segmentation and `p+o` one of `P(x)`. No token start of `P(x)` lies in
`(p+o, p+h-1]`, every one being a start of `P_1(x)` by Proposition 7 and none of those lying there. Hence `P` covers the
occurrence correctly. ∎

Emptiness of `R ∩ L` is reachability in the product of the two automata. The product runs over the `R`-device's own byte
moves, the `L` side stepping only at the labels those moves emit, so a reaching path projects to a refuting byte string
and not only to its label stream, and that projection is the counterexample. `R` depends only on the labeled inner stage
and `(W,o)`, and `L` only on the outer policy, so the two automata are stage-local. Thus `R` can be built once per
window and origin and reused across outer policies. The instrument builds both languages for regular inner stages, under
the labeler that names a segment by its least accepting language index, the priority the reference scan uses, and for
literal outer stages. It holds the product's verdict to the composite decider, that is, the composite verifier of
Theorem 4 under the generic certification decision, on all 190 (inner stage, outer policy, certified window) cases, and
its 70 refutations read back to byte strings the two-stage reference scan tokenizes and covers wrongly. Its reach is
shown by execution. Over inner `a` and `b` with outer tokens `0`, `1` and `00000001`, the window `(ab, 1)` is refuted by
the eight-byte counterexample `aaaaaaab`, the composite decider's own, which no enumeration of streams to length seven
contains.

**Corollary 1 (the per-label condition).** In the setting of Theorem 5, with `P_1` covering every occurrence of `W` in
`dom(P)` correctly and `P_2` literal over `Γ`, call a label *realized left of the boundary* when some stream of `R`
carries it immediately before its flagged segment. If no such label occurs as a non-final symbol of a token of `P_2`,
then `P` certifies `(W,o)`. When `P_1` is literal, every label realized left of the boundary is the image under `λ` of a
*compatible left token*, an inner token `t` whose suffix of length `min(|t|, o)` equals the suffix of `W[0..o)` of that
length, so testing those images instead is a stronger sufficient test.

*Proof.* Suppose `(γ, k) ∈ R ∩ L`. Index zero is always a token start, so `k >= 1` and `γ_(k-1)` is realized left of the
boundary. Since `k` is no token start of `P_2(γ)`, the token of `P_2` covering index `k-1` covers `k` as well, so
`γ_(k-1)` is a non-final symbol of it, against the hypothesis. Hence `R ∩ L = ∅` and Theorem 5 applies. For literal
`P_1` the segment ending at `p+o` is a token `t` occupying `[p+o-|t|, p+o)`, which meets the window's first `o` bytes on
`[max(p, p+o-|t|), p+o)`, an interval of length `min(|t|, o)`, and agrees with them there, so `t` is compatible and
`γ_(k-1) = λ(t)`. ∎

Neither literalness is used beyond a finite vocabulary. The first part needs only that every segment of `P_2` is a word
of a known finite set `V` over `Γ`, the hypothesis then reading that no label realized left of the boundary occurs as a
non-final symbol of a word of `V`. The second part needs only that every segment of `P_1` is a word of a known finite
set `U`, every realized label then being `λ(t)` for a compatible `t ∈ U`. The proof above uses nothing more, and in that
form the corollary covers towers, whose stages have finite vocabularies.

The condition is about labels and not spellings, and the point is executed. Take inner `{a, b}` with every segment given
one label, call it `Z`, and outer tokens `ZZ` and `Z`. The inner window `(ab, 1)` is certified, and no compatible left
token's spelling is carried non-finally, so the spelling test grants the lift. The exact decision refutes it with `ab`
as the counterexample, since the outer stream is `ZZ` and maximal munch takes it whole. The compatible tokens reach
outside the window for a reason of the same kind. Over inner `{aa, a, b}` the occurrence in `aab` ends the token `aa` at
a boundary whose window shows only `a`. Nor is the condition necessary. Over inner `{a, b}` with outer tokens `(a,a)`,
`(a)`, `(b)`, the window `(ab, 1)` lifts although `a` is carried non-finally, because the window fixes the following
symbol and the merge the condition fears is unreachable in its context. In general the condition falls short because it
reads what can stand left of the boundary and not what the window and the inner stage allow to follow it, so a token
carrying the left label non-finally can be unrealizable at the boundary. Evaluated over the labels the product realizes,
the condition grants no lift the composite refuses and is wrong as a decision on 41 of the 190 staged cases.

The lift also answers a dictionary update. Take the byte-pair table with the one merge `b+c` over the alphabet
`{a, b, c}` and extend it by the merge `a+b` at the later rank, so that the extended table tokenizes `abc` as `a` then
`bc`, the earlier rank winning. The window `(b, 0)` is certified under the one-merge table and refuted under the
extended one by the counterexample `ab`, one token, whose `b` is covered by a token beginning before it, while the
window `(abc, 1)` is certified under both. The update is a pipeline in which the one-merge tower, whose single stage is
the literal set `{a, b, c, bc}`, sits below a literal stage over those token names that carries the new merge as the
token `(a, b)`, each segment named by its spelling. Its reference segmentation equals the extended table's on all 3,280
strings through length 7.

On the surviving certificate the per-label condition refuses the lift, since the label `a` is realized left of the
boundary and carried non-finally by `(a, b)`, while the exact lift proves it, the product of the two stage-local
automata being empty and the composite decider agreeing. At an occurrence of `abc` the inner stage supplies `a` followed
by `bc`, so the outer token `(a, b)` cannot cross the marked boundary. The product built for `(b, 0)` refutes that
window by the counterexample `ab`.

The two greedy disciplines separate on the same window. Literal maximal munch over the extended table's final vocabulary
`{a, b, c, ab, bc}` tokenizes `abc` as `ab` then `c`, so the generic decision certifies `(abc, 1)` on the tower and
refutes it on the literal policy with `abc` as the counterexample. A certificate is a statement about one policy, and a
table and the longest match over its vocabulary are two.

**Proposition 8 (stack transitivity).** Let `C = T ∘ G` be a tower of pipelines, `T` reading `G`'s segments through a
labeler as in Theorem 5, whose innermost stage certifies `(W,o)`. Then `C` certifies `(W,o)` if and only if `G` covers
every occurrence of `W` in every string of `dom(C)` correctly and `R_G ∩ L_T = ∅`, with `R_G` and `L_T` the languages of
Theorem 5 for `G` and `T`.

*Proof.* By Proposition 7 at each level, the token starts of `C(x)` lie among those of `G(x)`, which lie among the
innermost stage's, and the innermost certificate leaves no start in `(p+o, p+h-1]` for any occurrence at `p` in any
`x ∈ dom(C)`, so `G` covers such an occurrence correctly exactly when `p+o` starts a token of `G(x)`, and `C` exactly
when `p+o` starts one of `C(x)`. If `G` covers every occurrence in `dom(C)` correctly, Theorem 5 with `P_1 = G` and
`P_2 = T` gives the equivalence. Otherwise some `p+o` starts no token of `G(x)`, hence none of `C(x)`, so `C` refutes
the window and both sides fail. ∎

Each level of a tower is thus asked its own question, the quantifier running over the tower's own domain, and both
premises are necessary. A top stage that rejects the stream carrying a violation of `G` hides it from `C`, so the
quantification must run over `dom(C)` and not `dom(G)`. A top stage that merges the window's bytes into one token
repairs a violation of `G`, so without the innermost certificate the equivalence fails. Both are staged, and the
equivalence is checked by enumeration on 60 three-stage cases.

## 7 The weighted theory

The decisions say whether; deployment asks how often. The hypothesis of this section covers every well-formed functional
verifier presenting a policy, in the sense of Theorem 1, under a rational, unifilar, almost surely stopping finite-state
source, unifilar meaning that the current state and the byte emitted determine the next state. A memoryless byte source
with rational byte probabilities and a positive rational stopping probability is the one-state case, and Proposition 10
shows that the theorem holds without unifilarity. Token counts below follow the final-zero convention of Section 2. For
a nonempty string, the token count is the number of set boundary bits plus one; the empty string has zero tokens.
Differential mass between two policies is the mass of the strings of their common domain that they segment apart, and
the per-byte rates are ratios of expectations, differing positions in expectation over expected length, which keeps them
rational where an expectation of per-string ratios would not be.

**Theorem 6 (exact weighted shadow).** Let `V` be a well-formed functional finite verifier presenting a policy, and let
`S` be a finite-state byte source with rational transition and stopping probabilities, unifilar, and almost surely
stopping. The following are rational: the tokenizability mass of `V` under `S`; the unnormalized expected length and
expected token and boundary counts contributed by accepted strings; and, for a second such verifier `V'`, the
differential mass and the unnormalized expected length and number of mismatched boundary bits on the common domain. Each
is zero when the product has no live state. Otherwise each is the start-state coordinate of the unique solution of an
affine system `x = Ax + b` over the live states of the product of `V`, or of `V` and `V'`, with `S`, where the spectral
radius of `A` is strictly below one. Every rate that is a quotient of two of these expectations is rational wherever its
denominator, an expected length, is positive.

*Proof.* Each mass and each unnormalized expectation is a vector indexed by the live states of the product of the
verifier with the source, the states reachable from the start from which joint acceptance is reachable, where the
verifier accepts and the source can stop. For the pair quantities the product runs both verifiers over one byte stream
with their marks guessed in pairs and their first disagreement latched. Each such vector satisfies an affine fixpoint
`x = Ax + b` with coefficients drawn from the byte probabilities and stopping mass, and each rate is the ratio of two
start-state coordinates of such solutions. When no state is live the system is empty, and the mass, the expected length
and the expected counts are zero. Those systems are absorbing chains only in the deterministic presentation, since a raw
row can exceed one.

The bound on the spectrum is where functionality earns its place. On a raw marked verifier both marks of one byte can
remain useful, `{a, ab}` after reading `a` already does it, and rows of `A` can exceed one, so the bound on `A^n` has to
come from multiplicity, since row sums give none. Two markings of one byte prefix that reach the same live state extend,
by liveness, along one completion to two accepted markings of one string, which functionality forbids. Hence the alive
markings of a prefix occupy distinct states, at most `N` of them for `N` live states, and the mass of the paths of
length `n` is at most `N` times the probability that the source emits at least `n` bytes. For the quantities solved over
the product of two verifiers a marking is a pair of markings, and the same argument runs on whichever component the two
differ in. From every state it enters with positive probability, a finite-state source that stops almost surely has a
positive probability of stopping within `m` steps, `m` its state count. Those states are finitely many, so the least of
these probabilities is a `δ > 0`, and the probability of emitting at least `n` bytes decays geometrically over blocks of
`m` steps, by a factor of `1 - δ` per block, so its sum over `n` is finite. The Neumann series `sum_n A^n` therefore
converges from the start state, hence from every live state, each reachable from the start with positive mass, and the
spectral radius sits strictly below one, so the solution is unique. Where a deterministic online presentation exists and
every source state stops with positive probability, one successor per byte keeps every row at most one minus the
stopping probability and the same bound follows in a line. Rational coefficients keep the unique solution rational, and
a quotient of two rationals with positive denominator is rational. ∎

**Proposition 9 (positivity).** Under the hypotheses of Theorem 6, the differential mass of `V` and `V'` under `S` is
positive exactly when the boundary half of the equivalence decision of Theorem 1, run on the restrictions of `V` and
`V'` to the support of `S`, returns a differing string.

*Proof.* The support of `S`, the strings it emits with positive probability, is a regular language, recognized by the
source's states with its positive-probability transitions, accepting where the stopping probability is positive.
Differential mass is a sum of nonnegative terms, one per string of the common domain that the two policies segment
apart, each that string's probability under `S`, so it is positive exactly when some such string lies in the support. A
differing string through a zero-probability byte carries no mass. The restrictions of `V` and `V'` to the support are
verifiers by Theorem 4, with the common domain cut down to the support and the segmentations unchanged, so the boundary
half run on them returns a differing string exactly when such a string exists. ∎

The measure and the logic thus agree about zero, on the boundary half alone, since the measured event is disagreement on
the common domain. Two policies whose domains differ while their shared segmentations agree carry zero differential mass
and are still separated by the domain half, which the law does not speak about. Under a full-support source, one in
which every byte and the stop carry positive probability, the restriction is the identity and the law reads plainly.

The theorem itself holds for sources in which one state and one byte lead to several next states, provided each
transition emits exactly one byte.

**Proposition 10 (sources need not be unifilar).** Theorem 6 and Proposition 9 hold for every finite-state byte source
`S` with rational probabilities on its byte-labeled transitions and on stopping that emits exactly one byte per
transition and stops almost surely, whether or not `S` is unifilar.

*Proof.* Of the proofs of Theorem 6 and Proposition 9, the product's definition and the bound on the spectrum use
unifilarity, where a byte prefix determines the source's run, so both are rebuilt here. Build the product over pairs of
a source state and a verifier state, with one edge for every source transition of positive probability and every mark
the verifier admits on its byte, weighted by the transition's probability, and trim it to its live states. A string's
probability is the sum of the weights of the source runs that emit it and stop, and functionality gives its bytes at
most one accepted marking, so the masses and expectations are again start-state coordinates of affine systems
`x = Ax + b` over the live states. Fix a run of `S` through `n` transitions. Two distinct marking paths along it that
reach one verifier state at a live product state extend, along one source continuation and one marked completion to
joint acceptance, to two accepted markings of one string, which functionality forbids. The live marking paths along the
run therefore occupy distinct verifier states, at most `|Q|` of them for the verifier's state set `Q`, and summed over
the runs the mass of the product's paths of length `n` is at most `|Q|` times the probability that `S` emits at least
`n` bytes. For the pair quantities a path carries a pair of markings, and two distinct pairs reaching one pair of
verifier states at a live product state differ in a component that would receive two accepted markings of one string, so
at most `|Q| |Q'|` pairs live along each run, `Q'` the second verifier's state set. The block bound in the proof of
Theorem 6 uses only that `S` is a finite Markov chain on its states that stops almost surely, so the probability of
emitting at least `n` bytes decays geometrically over blocks, the Neumann series converges from the start state and from
every live state, the spectral radius of `A` is below one, and each solution is unique and rational. For positivity, the
source's positive-probability transitions, accepting where the stopping probability is positive, now form a
nondeterministic automaton, whose language, the support of `S`, is still regular, so the proof of Proposition 9 runs
unchanged on the restrictions to it. ∎

The executable interface of this paper remains unifilar. Its source objects present one successor per state and byte,
and the solver and its checks below run on those, so Proposition 10 is proved here and not claimed as implemented.

The solver solves each system exactly over the rationals and asserts the residual `x - (Ax + b)` zero coordinate by
coordinate, and the dynamic program is held equal, to the digit, to per-string enumeration on 30 staged triples. The
deterministic engine serves the literal instances, while the regular-token example of Section 4, which has no online
scanner, and the two-mode instance are decided by the trimmed product alone, cut down to its live states. Raw-product
validation is split by family, with the memoryless literal cases and the literal pairs compared with their exact
deterministic engines, the two-state literal cases checked by exact inverse and residual identities and by finite
multiplicity and path-mass tests, and the regular and two-mode single-policy cases and the pair cases beyond the literal
path bracketed by finite enumeration with exact tails. Unifilar Markov sources carry every quantity over by the same
methods. The block-survival bound above replaces the memoryless one-step tail, the solver refuses a source that does not
terminate and gives its reason, and on a one-state source it returns exactly the rationals of the memoryless solver it
generalizes. The positivity law is checked in its plain form on 100 ordered pairs under a full-support source, and in
its support-refined form, with the right-hand side computed by the paper's own restriction calculus, on 60 pairs of
policy pair and source, including two erasures where a globally differential pair has mass exactly zero because the
support forbids every differing string.

Two closed forms show the instrument's reach. Both are outputs of the same solved systems, reported without separate
enclosures of their own. The one-merge extension `{a, b, ab}` to `{a, b, ab, ba}` under a memoryless source has
differential probability exactly 4/11 and per-byte mismatch rate 1/5 under `P(a) = P(b) = 2/5` and `P(stop) = 1/5`,
where the earlier experiments sampled. The analogous extension `{s, q, o, qo}` to `{s, q, o, qo, os}` over the three
classes structural, quote and other, under corpus digram statistics, has differential mass about 0.063, exactly
3396201545233/54186662216209, against 833/2561, about 0.325, under the matched uniform source. The measure moves with
the world it models, in closed form. The comparison is matched by design. The digram conditionals carry no smoothing
weight, both sources stop with probability exactly 1/8 at every state, the corpus start state is the first byte's class,
and the uniform comparator's expected length, exactly seven, equals the corpus source's, so one comparator is matched on
stopping and on length at once. Corpus structure, on this slice, concentrates mass on strings where the two vocabularies
agree.

## 8 Instances and applications

The empirical weight of this series lives in its two companions and stays there. The recovery companion measures
certified recovery against the classical conventions on archived campaigns, and the splitting companion measures anchor
supply, splits the recovery campaign's archived corpus at certified anchors with a threaded scanner, and decides its own
design change with a synthesized differing string. This paper keeps the instances that belong to the abstraction, and
one measurement its own lift invites. A vocabulary upgrade from 320 to 352 tokens produces a boundary differential, a
differing string of length two. The tokens are byte-pair merges over the full byte alphabet, trained on the JSON sample
the programs read, simdjson's `twitter.json`. That file is fetched from its pinned release, held to its digest and read
byte for byte. The added merges silently reframe two bytes, the cached-segmentation hazard exhibited by construction.
Byte-pair encoding is an exact instance of the class. Canonical BPE has the finite-state constructions of Cognetta and
Okazaki (Computational Linguistics 2025) and Berglund, Martens and van der Merwe (CIAA 2024). For a proper dictionary,
one whose every multi-symbol merge part is an earlier merge's product, the construction of Berglund, Martens and van der
Merwe (CIAA 2024) is a subsequential transducer from strings to tokenizations by Theorem 4 of that paper's extended
version. Writing each emitted token `u` as the mask `0^(|u|-1)1` and closing the output with a zero bit in place of the
last one keeps that transducer subsequential and length-preserving, so the bridge of Section 4 places such a policy
inside the online-scannable region, and the four decisions apply to it directly. Canonical BPE is nonetheless not in
general a maximal-munch policy, for the reason Section 9 records.

The lift of Theorem 5 answers a question a growing dictionary asks of every certificate it holds, whether the
certificate survives the next merges. Appending merges is one way pretrained tokenizers are adapted. Purason, Chizhov,
Yamshchikov and Fishel (Findings of EACL 2026) continue a tokenizer's BPE training on new data, Smith, Dakhran, Cabrera,
Lee, Pak, Tadimeti, Seyde, Labonne, Amini and Lechner (arXiv:2607.15232, 2026) continue its merges in place, and Herold,
Kozielski, Santavas, Versley and Khadivi (NeurIPS 2025 CCFM Workshop) add domain tokens under a guarantee that no input
tokenizes longer, but none of them asks which certificates survive. The exploration decides it on byte-pair dictionaries
of 64 merges extended by the next `k` merges of their own training, for `k` from 1 to 16. It runs two dictionaries side
by side, one trained on the corpus and the other the first merges of the GPT-2 table, applied to raw bytes without
GPT-2's pre-tokenizer. In the GPT-2 table the appended merges with the space byte as left part number 1 among the first
8 appended and 3 among the first 16.

The verifier is a deterministic tower, one single-lookahead transducer per merge composed into one automaton, since the
subset-construction tower behind the byte-pair policies of Section 3 passes 5,000 states within two merges. The
deterministic tower has 4,063 states for the corpus dictionary and 561 for the GPT-2 prefix over the bytes some merge
names and one inert stand-in for the rest, a reduction that is sound because a byte no merge names is only ever a
single-byte token and no merge joins across it, and that is checked against the full byte base on 48 sampled pairs. The
tower requires that every merge produce a token distinct from the alphabet and from every earlier product, as both
trained tables do. The verifier is held to the reference encoder on exhaustive sweeps over the five byte-pair universes
of Section 3 and 12 random tables, 2,555 and 22,746 strings, to the subset tower on those universes, and to the
reference markings of 171 held-out pieces, which the longest extension's tower accepts while refusing every one of their
8,037 single-mark perturbations.

The inventory is frozen before any extension is decided and holds every window of width one to three, with every origin,
in a 16 KiB slice held out from training, 3,740 pairs for the corpus dictionary and 2,684 for the GPT-2 prefix, of which
the base towers certify 1,174 and 902, the 2,566 and 1,782 refutations replayed by the encoder. The anchor supply is
measured on the next 16 KiB. Each certificate is then decided three ways under every extension: by the composite
decision on the extended tower; by the exact lift, the inner stage the base tower and the outer stage the tower of the
appended merges over base token names, a pipeline whose marked language equals the extended tower's, checked at
`k = 16`, with `R` built once per certificate from the base tower and `L` from the outer tower's verifier; and by the
compatible-token test of Corollary 1, its stronger second part, in the finite-vocabulary form stated after it, with the
base-token spellings of all outer tokens as the outer vocabulary, each base token, the single bytes among them, spelled
as itself and each appended product as the base tokens it concatenates. The test is coarser than the per-label condition
it bounds, which reads the labels realized left of the boundary. Over `{a, b}` with the base merge `a+a` and the
appended merges `a+b` and `a+aa`, the window `(baaa, 3)` is certified by both towers and by the exact lift, its only
realized label is `aa` and the per-label condition grants it, while the compatible tokens include `a`, which the new
token `(a, b)` carries non-finally, and the test refuses it. Table 2 has the counts. The exact lift equals the composite
decision on every certificate of every extension, and the test grants nothing the lift refuses. Of what the test
discards, the lift keeps 474 certificates of the corpus dictionary at `k = 16` and 236 of the GPT-2 prefix.

**Table 2.** Base certificates of a dictionary of 64 merges that survive its next `k` merges, decided three ways, by the
composite decision, the exact lift and the compatible-token test; the first two agree on every certificate and share the
column exact, and test is the compatible-token test. Kept counts the surviving certificates of the frozen inventory and
recovered those the lift keeps that the test discards; anchors per KiB, the 99th-percentile anchor gap, between
consecutive anchors, and the longest anchor-free stretch, both in bytes, are measured on the held-out evaluation slice,
exact against the test where a cell holds two figures, and the base row has no extension to compare. The JSON dictionary
is trained on the corpus; the GPT-2 dictionary is the first merges of the GPT-2 table, whose space-led merges begin at
`k = 8`.

| Dictionary | `k`  | Kept exact | Kept test | Recovered | Anchors per KiB exact | Anchors per KiB test | Anchor gap p99 | Longest stretch |
|------------|------|-----------:|----------:|----------:|----------------------:|---------------------:|---------------:|----------------:|
| JSON       | base |      1,174 |           |           |                 482.4 |                      |                |                 |
|            | 1    |      1,168 |     1,132 |        36 |                 480.1 |                476.4 |        17 / 17 |         22 / 22 |
|            | 2    |      1,162 |     1,068 |        94 |                 478.0 |                462.8 |        17 / 17 |         22 / 24 |
|            | 4    |      1,152 |       969 |       183 |                 472.2 |                428.2 |        17 / 17 |         22 / 24 |
|            | 8    |      1,136 |       799 |       337 |                 461.9 |                365.9 |        20 / 21 |         30 / 32 |
|            | 16   |      1,111 |       637 |       474 |                 446.6 |                313.8 |        20 / 23 |         30 / 42 |
| GPT-2      | base |        902 |           |           |                 927.9 |                      |                |                 |
|            | 1    |        898 |       846 |        52 |                 923.7 |                914.2 |          2 / 3 |           5 / 7 |
|            | 2    |        895 |       811 |        84 |                 922.2 |                902.3 |          2 / 3 |           5 / 7 |
|            | 4    |        889 |       784 |       105 |                 920.6 |                887.8 |          2 / 3 |           5 / 7 |
|            | 8    |        878 |       710 |       168 |                 914.2 |                615.7 |         2 / 11 |          5 / 20 |
|            | 16   |        867 |       631 |       236 |                 909.0 |                574.9 |         2 / 11 |          5 / 20 |

The supply tells where the loss falls. On the corpus dictionary the test's anchors thin steadily, 313.8 per KiB against
the exact lift's 446.6 at `k = 16`, the longest anchor-free stretch 42 bytes against 30, 2,126 of 7,146 anchor positions
of the evaluation slice recovered by the lift. On the GPT-2 prefix the test's supply falls gently, to 887.8 anchors per
KiB at `k = 4`, until the first space-led merge arrives at `k = 8`, and then it falls to 615.7 while the exact lift goes
from 920.6 to 914.2. From there on the test refuses all 11 base certificates whose boundary follows a space, the space
being carried non-finally by the new token, and the lift, which asks instead whether the merge is ever realized across
the boundary, keeps every one of them. At `k = 16` the exact lift holds 909.0 anchors per KiB against the test's 574.9,
the p99 anchor gap 2 against 11, the longest stretch 5 against 20, and 5,346 of 14,544 anchor positions are recovered.
Across all five extensions, the exact lift costs less even after including its one-time construction, 2,863.3 against
16,112 seconds of summed task time on the corpus dictionary and 652.6 against 1,502 on the GPT-2 prefix. For GPT-2 the
first extension alone, 425.1 against 249, does not repay the build. The stage-local automata of the base tower are built
once, 1,819 and 391 seconds of summed task time outside the factor, and a lift product over them is cheaper than
deciding the extended composite from scratch by a factor of 13.7 to 16.3 on the corpus dictionary and 4.7 to 7.3 on the
GPT-2 prefix, in summed task elapsed time. Over a sample of 24 certificates and the five extensions, reuse took 108.4s
against 658.3s on the corpus dictionary and 26.4s against 66.3s on the GPT-2 prefix. The compatible-token test is a
sound first filter that needs no product search, but after sixteen appended merges it discards 29.8 and 36.8 percent of
the anchor positions the exact lift keeps on the two dictionaries, against at most 9.3 percent through four.

WordPiece, the subword tokenization method of Schuster and Nakajima (ICASSP 2012), has a real-vocabulary instance too,
at two depths. The policy is the partial two-mode longest-match lexical policy over the BERT-Base, Uncased vocabulary of
Devlin, Chang, Lee and Toutanova (NAACL 2019), read on lowercased raw text, its plain entries the initial tokens and its
`##` entries the continuation tokens. It is not the end-to-end tokenizer of BERT's reference implementation (Google
Research, GitHub repository 2019), because it omits the basic tokenizer's cleaning, its accent stripping by Unicode
decomposition, its splitting on whitespace and punctuation and its handling of CJK characters, and the `[UNK]` fallback
with its limit of 200 characters on word length, and it fails where no token matches. A vocabulary does not determine
its inference method, Uzan, Schmidt, Tanner and Pinter (ACL 2024) comparing several over the same vocabularies, so the
policy is named by its scan and not by its vocabulary alone. Song, Salcianu, Song, Dopson and Zhou (EMNLP 2021) compute
that scan in linear time from the vocabulary's trie with failure links in the manner of Aho and Corasick, while here its
marked scans are recognized and properties quantified over every string decided on them. The vocabulary file opens with
999 reserved entries, its special tokens and unused slots, and 997 single characters, and the first depth takes the next
4,096 entries in the file's token-ID order over those 997 characters. Reduced jointly with the policies it is compared
with below, its alphabet is a quotient of 41 symbols, namely 39 named characters, 38 of them named by its own tokens,
and two stand-ins, one for each joint vector of initial and continuation membership that the remaining characters carry
across the compared policies. At this depth `<1>` stands for the 957 characters that every compared policy, the prefix's
own among them, holds as single initial tokens and none as continuation tokens, and `<2>` for the one character
subscript two, which the deepest comparison also holds as a continuation token.

The reduction is sound. A character that no multi-character token of any compared policy contains can be consumed only
as a single-character token, in the modes its membership vector allows, and no token begun before it reaches across it,
so replacing every such character by another with the same vector leaves every scan's success or failure and every
boundary unchanged. A refuting text over the full base maps to one over the quotient with the same boundaries and the
same occurrence, and a refuting quotient text lifts to a raw one by restoring the raw window's characters at the
distinguished occurrence and writing any representative of its class elsewhere, so a canonical window's verdict is the
verdict of every raw window it stands for. A check of 51 pairs decided both ways, the raw window over all 997 characters
and the canonical one over the quotient, confirms it.

Over the quotient the prefix has a verifier of 29,786 states, on which the four decisions take seconds to about a
minute: 4,747 window and origin pairs from a held-out slice of `twitter.json`, 2,065 certified and 2,682 refuted with
every refutation replayed; a boundary gap of 15, the length of `representatives`, with no lasso; a realizability mask of
26,211 states that strands nothing; and the vocabulary one entry deeper separated by the appended word, `electoral`,
which the shorter prefix spells `el|e|c|tor|al`. The mode is visible in both differing strings against maximal munch
over the same initial tokens, `<1>00` segmented apart and `<1><1>` tokenized only without modes, and in the cut
decision. Of the 476 occurring certificates, those whose window occurs in some domain string, 466 are cuts that are not
independent, each failing on the suffix side only, a shortest counterexample `<1>ca`.

At full lexical depth, all 28,526 entries after the head over the same 997-character base, the 999 reserved entries
excluded, the policy's verifier is built from its two tries, without exploring the scan's configurations, the two
constructions corresponding at the prefix with no transition in disagreement. It has 138,510 states over a quotient of
71 symbols, 70 named characters and `<1>`, which at this depth stands for the other 927 characters, single tokens in
both modes here and initial ones only at the 4,096 prefix. Each quotient symbol is a single-character token in both
modes, so every string over the quotient alphabet is in the policy's domain. On the same 4,747 pairs it certifies 1,230
and refutes 3,517, every refutation replayed. The boundary gap is 18, the length of `telecommunications`, with no lasso,
the search for one taking 708.1 seconds, and the mask keeps 136,766 of 138,510 states and strands nothing. The
full-depth policy is separated from the 4,096 prefix by `02` on the boundaries and `<1><1>` on the domain, and from
maximal munch over its own initial tokens by `<1>0s` on the boundaries alone, the two domains coinciding. Of the 590
occurring certificates, 196 are cuts that are independent, every one of them with a stand-in in its window, and 394 are
not, again each on the suffix side only.

Within maximal named-character runs the anchors are few. On the 16,384-character evaluation slice, counting only in the
runs the policy tokenizes whole, the prefix policy certifies 597 anchor positions and the full-depth policy 333, or 37.3
and 20.8 per 1,024 characters of the full slice, none of them inside a run the scan reads as one token. The runs start
at 1,434 positions. Across the 1,049 maximal alphabet runs of the raw slice, within which every quotient symbol is a
single-character token in both modes at full depth, the full policy certifies 4,853 positions, or 303.3 per 1,024
characters of the full slice, the other 5,003 characters of which lie outside the alphabet.

The munch lexer library (this repository) implements the interface at its release 2.3.0 with a deterministic trim
automaton over marked symbols built from any transition table, the armed run of Section 2 over a byte automaton, and the
verifiers the two tokenizer instances above are decided on, the merge tower of canonical byte-pair encoding as
`merge_tower()` and the trie chain of two-mode WordPiece as `trie_chain()`. The library also implements the four
decisions of Section 3 and the cut and chunk independence decisions of Theorems 2 and 3 as `dependence()` and
`chunk_dependence()`. Its test suite holds each of them to reference answers this paper's exploration writes for it.
Every counterexample of a refuted window or a failing cut or chunk, every differing string of a failed equivalence and
every lasso, the library's and the fixture's, is replayed through a reference scan, a reference encoder or the verifier,
except the refuting texts the cut fixture records for its uncertified pairs, which the tests do not read, since an
uncertified pair need only be refused. Verdicts that return no string and boundary-gap suprema are compared, and a
realizability mask is checked against co-accessibility state by state in the fixture of the four decisions but only for
its presence and size in the merge-tower and trie-chain fixtures.

The fixture of the four decisions has 474 universes, every set of one to three distinct words of length one to three
over two symbols and the five regular sets of the edit sweep, and carries 16,116 certification decisions, 474
boundary-gap decisions of which 2 are lassos, 474 realizability verdicts and 1,818 equivalence pairs. The cut fixture
decides every window of length one to three at every origin over 489 universes, 5,517 certified cuts independent and 62
not. The chunk fixture decides 29,085 inventories over 494 universes, 1,759 of them not chunk-independent. Both fixtures
are the independence sweeps above less their five byte-pair universes, whose tabulated towers run to thousands of
states. The chunk fixture adds one universe of its own, every nonempty string over `{a, b}` segmented byte by byte,
whose start accepts nothing. The merge-tower and trie-chain fixtures hold the two constructions to the reference
encoder's markings of 10,177 strings over 14 byte-pair tables and the reference scanner's markings or refusals of 13,713
strings over 16 two-mode vocabularies, and pose the four decisions on each.

Given a build of the library at that release, the gate's library stage regenerates the five fixtures and holds each byte
for byte to the library's copy, runs the library's verifier, merge-tower and trie-chain tests and its supply probe, and
writes the record this instance is held to. Without such a build it reports the archived record and names the
regeneration it did not run. On `twitter.json` under the JSON grammar the library's verifier examines 19,434 windows of
one to three bytes at 53,074 window origin pairs and certifies 284 of them. It agrees with the library's online window
route on every pair except 17 the route refuses conservatively, each confirmed by exact search. The certified positions
are 43,043, or 69.79 per KiB, and the longest anchor-free stretch is 464 bytes.

The seam between text and tokens in language-model serving is the same mathematics under other names. The finite-state
tokenizer of Kaplan (US Patent 5,721,939, 1998) emits its output at a pinch point, a position where all tokenization
paths reaching it can be extended in exactly the same ways, which is a commitment about that input's configurations,
whereas a certificate here asks whether one fixed window and origin determine a boundary at every occurrence across the
whole domain. Scanners and serving systems mostly validate that seam per input. IBM's speculative stream scanning
partitions a stream at likely token boundaries, scans the chunks in parallel and confirms or refutes each guessed
boundary (von Praun, US Patent 8,001,329, 2011). Patented systems do the same in other settings. A natural-language
segmenter proposes segmentations and submits each to a linguistic knowledge component for validation, proposing further
ones until the input is validly segmented or no further segmentation is possible (Pentheroudakis, Bradlee and Knoll, US
Patent 7,092,871, 2006). A context-aware tokenizer combines a record that left and right context automata build with its
token automaton, segmenting left to right by longest match (Walther, US Patent Application Publication 2003/0149562,
2003). An XML preparser runs a copy of its automaton from every possible state at a chunk's start and lets the preceding
chunk's actual end state select the run (Chiu, US Patent 8,782,514, 2014). An editor re-lexes the text an edit affects
with a batch lexer, comparing the new token stream with the old until the two agree (Van De Vanter, US Patent 5,737,608,
1998). Incremental lexing restarts from recorded scanner states and reproduces the batch token stream exactly (Wagner
and Graham, manuscript 1997), and parallel lexing for parsing lexes chunks under alternative readings and reconciles
them at the joins (Barenghi, Crespi Reghizzi, Mandrioli, Panella and Pradella, Science of Computer Programming 2015).
Parallel tokenization compares the tokenizations of overlapping chunks and restarts with a doubled chunk when it finds
no overlap token at the same position in both (Shao, Zheng, Wang, Zheng, Li and Fan, ACL 2026). Session continuation
matches fresh against cached token records and accepts a splice only through a long enough equal run containing a
family-specific stable boundary (Zhang and Cao, arXiv:2607.29678, 2026). In generative steganography the sender
re-tokenizes the generated text after each token to obtain the receiver's view and resets only where that view diverges
from its prediction, suspending the check while it emits pre-segmentation anchor tokens until the next token outside
that family has fixed the boundary (Wang, Wang, Pang, Han, Qi, Hu and Chen, arXiv:2604.25486, 2026).

For byte-pair encoding, the reach of an appended suffix is bounded under a proper dictionary, the kind standard training
produces (Berglund and van der Merwe, NCMA 2023), the tokenization is maintained incrementally (Jiang and Gong, ICML
2026), and its streaming delay is bounded from a known beginning (Mamouras, Li and Yang, PLDI 2026). Three
implementations precompute sufficient cut rules. Gigatoken uses absent vocabulary byte pairs before selected
punctuation, arguing that every merge result is a vocabulary piece (Rød, Gigatoken, 2026). SentencePiece Lite states the
analogous character-bigram rule for normalized input (Kudo, SentencePiece Lite, 2026). Xu (blog 2025) checks the merge
table for cuts across which no merge can join. GPU implementations of byte-pair encoding run its merges in parallel
within thread blocks, dropping regex pre-tokenization at a small loss in generation quality (You, arXiv:2507.11941,
2025), follow GPT-2's merge rules and produce the same tokens as a CPU version on the sequences tested (Kadamba and
Jaisankar, arXiv:2603.02597, 2026), or hold the token sequence as a linked list and trade minor discrepancies in the
tokenized output for throughput (Kovilur, Shekar and Azad, arXiv:2610.04951, 2026). Each of these validates, checks in
context, reconciles or resynchronizes the seam of the input at hand, or, like the three static cut rules, states a
sufficient condition for one tokenizer family. Certification asks, for a policy presented by a supplied finite verifier
and a proposed window and origin, whether every occurrence throughout the domain pins that boundary. This predicate is
decidable before any input is known. The static cut rules guarantee boundaries across contexts for fixed patterns; the
contribution here is an exact decision for arbitrary candidate windows and for policies given by finite verifiers.

A prompt cache is sound at a position exactly when every continuation retokenizes the prefix identically, and a
certificate delivers the guarded form of that.

**Proposition 11 (guarded cache soundness).** Let `P` be maximal munch over a literal or regular token set and let
`(W,o)` be certified by `P`. For every string `x` of `dom(P)` with an occurrence of `W` at `p`, the prefix `x[0, p+o)`
is in `dom(P)` and `P` segments it as `P(x)` does, so a cache keyed on the cut `p+o` is sound under every continuation
that keeps the window in place and that the policy tokenizes completely.

*Proof.* Certification makes `p + o` a boundary of the complete scan of `x`. Every token the scan of `x` commits before
`p + o` ends at or before it, and every candidate the scan of the prefix sees was already a candidate of the scan of
`x`, so the prefix's scan commits the same tokens and completes, which is the splitting companion's split theorem
([arXiv:2610.08854](https://arxiv.org/abs/2610.08854)) for one cut. ∎

The guarantee is stated for maximal munch, for continuations that keep the window, and for caches keyed on cuts, and the
examples below show why. A continuation that replaces the window's bytes is outside the guarantee. Maximal munch over
`a`, `ab`, `b`, `c` certifies `c` at origin zero, and the prefix `a` scans as one token before `c` and as part of `ab`
before `b`. Certification alone does not reach even the guarded form for arbitrary policies, since a certified window
pins the covering token's start and not the prefix's segmentation. A two-string domain suffices for a counterexample,
both strings certifying their shared separator while segmenting the shared prefix apart. Nor does a policy's agreeing
with its own restart at every boundary reach it. Over the alphabet `a`, `b`, `c`, take the policy that splits into
single bytes unless the input ends in `c`, and scans by maximal munch over `a`, `aa`, `b`, `c` when it does. It is
finitely verifiable, restarts consistently at every boundary, and certifies `b` at origin zero everywhere, yet the
prefix `aa` before that certified cut scans as two tokens in `aab` and as one in `aabc`.

The realizability decision governs a masked generator that must emit only completable token sequences, and its mask
provably never dead-ends. A tokenizer upgrade is an equivalence question whose differing strings are the inputs whose
cached segmentations it silently breaks, and it is a question about boundaries. By Proposition 4 an upgrade can preserve
every boundary and still rename every segment, so a cache keyed on token identifiers needs the labeled comparison, while
this one serves a cache keyed on cuts. The decisions of Theorem 1 deliver those guarantees at that scope; they are not
analogies for them.

## 9 Related work

This section names the prior result behind each part of the paper and says what remains this paper's own.

**The class is classical.** Definition 1 names a known object. Writing `conv(x,y)` for the convolution that pairs two
strings position by position, a function `f` is *automatic* when `{conv(x, f(x)) : x ∈ dom(f)}` is regular (Case, Jain,
Seah and Stephan, Logical Methods in Computer Science 2013, Definitions 2.1 and 2.2). The relations so recognized are
the synchronized rational relations of Frougny and Sakarovitch (Theoretical Computer Science 1993), recognized on
convolutions in which the shorter component is padded with blanks, equivalently realized by letter-to-letter automata
with a terminal function whose values are rational residuals `(S × {ε}) ∪ ({ε} × T)`, `S` and `T` rational, so that
residual input remains on at most one tape (Frougny and Sakarovitch, Theoretical Computer Science 1993, Definition 4.1).
A policy's boundary function maps a byte string to an equal-length bit string, so the convolution needs no padding and a
marked string *is* that convolution. A policy is therefore finitely verifiable exactly when its boundary function is
automatic. The standard consequences come with the class, namely that automata recognizable relations are closed under
the Boolean operations and both quantifiers, with the automata constructed (Khoussainov and Nerode, Logic and
Computational Complexity 1995, Theorem 4.4), so anything first-order definable from finitely many automatic functions
and relations is automatic again, and that automatic structures have a decidable first-order theory (Khoussainov and
Nerode, Logic and Computational Complexity 1995, Corollary 4.2), which goes back to Hodgson (Theoretical Computer
Science 1982). This paper claims none of that. The theory of automatic structures does not supply the content of
Sections 3 through 7. Its generic decisions answer sentences, and an accepted string can be searched out of any
constructed automaton there too. But the umbrella does not compute the supremum the boundary gap decision returns, it
does not construct the realizability mask, and it says nothing about how certificates behave under the operations that
build real tokenizers, which is this paper's center.

**What each decision inherits.** Each of the four has a predecessor, three resting on results that predate this paper
and the fourth refining a classical special case, and naming them is part of stating what is here.

*Equivalence.* Single-valued transducers have decidable equivalence, proved by Blattner and Head (Journal of Computer
and System Sciences 1977) from their Corollaries 2 and 3 (pp. 314-315). A verifier is a letter-to-letter transducer
emitting one bit per byte, single-valued because the policy is a function, so the decision already follows from that
line of work. Veanes, Hooimeijer, Livshits, Molnar and Bjørner (POPL 2012) state the shape the proof takes as well, that
deciding `A ≡ B` "reduces to two independent tasks", domain equivalence and equality on the common domain. Their
decision is non-constructive, and a separate semi-decision procedure of theirs generates a shortest input on which the
two outputs differ, so a differing string of that kind is prior too. Neither reading here is an addition to that, since
their definition of single-valuedness admits partial functions, and representing outputs as boundary sets is a choice it
leaves free. The route the proof above takes first is not an addition either. A marking language is regular and a
verifier is deterministic over marked symbols, so full equivalence is equality of two deterministic automata, which the
product over the symmetric difference decides by the decision problems of Rabin and Scott (IBM Journal of Research and
Development 1959, Theorem 10 and Corollary 10.1). Past that, the two-task shape supplies the name of the half a
differing string falls in, and the reductions of Section 4 separate on the domain half.

*Realizability.* The mask is co-accessibility, which supervisory control calls nonblocking (Ramadge and Wonham, SIAM
Journal on Control and Optimization 1987, Section 6). Constrained decoding compiles a target language against a language
model's vocabulary and masks the next token by the edges of the automaton that results (Koo, Liu and He, COLM 2024), and
with the automaton trimmed to its co-accessible states that mask admits exactly the tokens after which an accepted
completion remains. The proof above says so where it is used, and the decision only makes that restriction available
through the interface beside the other three.

*Boundary gap.* The yes-or-no is limitedness. Read a boundary as a counter reset and a byte as an increment, and a
bounded boundary gap is an instance of limitedness for R-automata, whose counters are incremented, reset or left
unchanged. Limitedness asks whether some bound makes the language accepted within it the whole accepted language, and
Abdulla, Krčál and Yi (CONCUR 2008, Theorem 4) decide it beside the universality problem. Past that verdict the decision
here returns the exact supremum under the conventions of Section 3, and a pumpable lasso when there is none. A
deterministic finite graph makes both elementary. Exact bounds for counter automata over infinite words are computed as
well as decided (Colange, Racordon and Buchs, AVoCS 2015), and the value returned here is that quantity for a
deterministic marked graph, with this paper's counting conventions and a lasso. The nearest tokenizer-specific quantity
among the sources surveyed is the token-neighbor distance of Li, Yang and Mamouras (ASPLOS 2026), which measures how far
past a token's end a maximal-munch scanner reads before that token's maximality is settled, from a known start. The
boundary gap starts from no boundary and returns the longest span carrying none, and the splitting companion separates
the token-neighbor distance from certification and the anchor-free span on one token set
([arXiv:2610.08854](https://arxiv.org/abs/2610.08854)).

*Certification.* The certificate refines code synchronization. In the general convention of Berstel, Perrin and
Reutenauer (Cambridge University Press 2010), a pair `(x, y)` of words of `A*` is synchronizing for `X*` when
`uxyv ∈ X*` forces `ux ∈ X*` and `yv ∈ X*` for all `u` and `v` in `A*`, so an occurrence of `xy` in a word of `X*` fixes
a factorization cut between its halves in every context. Over `X = {ab, ba}` the pair `(b, b)` is synchronizing, though
neither half lies in `X*`. Carpi and D'Alessandro (Theoretical Computer Science 2017, Definition 2) restrict
synchronizing pairs to `X* × X*`. A code has finite synchronization delay when some number of codewords on each side
makes every such pair synchronizing (Restivo, Theoretical Computer Science 1975, Definition 1). For a regular code,
whether that delay is bounded is decidable through the syntactic monoid of `X*` (Lassez, Groupe d'étude d'algèbre 1976,
Théorème 2). For a regular synchronizing code the pair test is finite-state too. Take a transitive, synchronizing,
unambiguous automaton for `X*` whose base state is its only initial and final state. Under those hypotheses a pair
`(x, y)` of `X* × X*` is synchronizing exactly when two sets meet only in that base state, the states reachable by an
`x`-labeled path from some state and the states from which a `y`-labeled path exists (Carpi and D'Alessandro,
Theoretical Computer Science 2017, Lemma 2(4)). The bounded-window form is synchronization with finite delay under a
morphism, and Fici, Romana, Sciortino and Urbina (MFCS 2025, Example 21) separate its two readings. Under the Thue-Morse
morphism the doubled letters fix the cut between them wherever they occur, yet on the images of `a^n b` and `b^n a` the
windows free of such a pair grow without bound, while every circular factor of at least five letters of the image of a
word in `{ab, ba}*` holds one. Prefix-synchronized codes design the property in, every codeword beginning with a marker
that occurs at no other offset of any concatenation of codewords (Morita, van Wijngaarden and Han Vinck, IEEE
Transactions on Information Theory 1996), a window certified at origin zero by construction of the code. The window
paper of this series places the certificate in this lineage ([arXiv:2608.09761](https://arxiv.org/abs/2608.09761)), and
the definitions give the exact relation over a prefix code read as a token set. A window occurring in a word of `X*` is
certified exactly when every occurrence of it in every word of `X*` has a factorization boundary at the origin and its
part from the origin on is a prefix of a codeword. The first condition is the synchronizing-pair condition, in the
general convention, for the window's two parts `W[0..o)` and `W[o..)`; the second, which certification adds, pins the
covering codeword, since a codeword beginning at the cut cannot end inside the window's tail without being a proper
prefix of the codeword that tail begins.

Neither condition is about the pieces a worker scans. Over the prefix code `{ab, cd}` the window `(bc, 1)` is certified,
yet its halves `b` and `c` are not words of `X*`. Cut independence of Theorem 2 concerns the whole input's prefix and
suffix instead, `ub` and `cv` for an occurrence in `ubcv`, each of which must scan alone with the segmentation it
inherits, and chunk independence asks the same of every chunk cut at an inventory's anchors. Where data-parallel
finite-state machines enumerate a chunk's possible entry states (Mytkowicz, Musuvathi and Schulte, ASPLOS 2014) or
speculate on them (Zhao, Wu and Shen, ASPLOS 2014), a chunk-independent inventory makes the restart from the start state
correct, so no entry state is enumerated or guessed. Programmable speculation lexes a chunk from an entry state
predicted by lexing a few characters before it, validates the prediction against the predecessor's actual end state at
run time and re-executes the chunk on a miss (Prabhu, Ramalingam and Vaswani, PLDI 2010). The boxing check of Lester
(PLAS 2013, Section 3.4) validates one known join instead, the concatenation of two analyzed string abstractions,
requiring that every state the lexer may be in at the end of the left one emit a token and restart on every character
that may begin the right one, so that concatenation moves no token boundary. Lester, Ong and Schäfer (Journal of
Computer Security 2016, Section 5.2.3) give the condition in full, requiring of every lexer state possible after the
left abstraction that either it already emits a token or every possible first character of the right abstraction
immediately makes it emit the same token, so that the join is a lexeme boundary. The speculation and the boxing check
both validate a seam at a narrower scope, a speculated state at run time or a known join of analyzed fragments, where a
certificate pins the boundary at every occurrence of its window throughout the domain.

A classical shape outside code theory with the same outline is the resolving block, as Adler, Coppersmith and Hassner
(IEEE Transactions on Information Theory 1983, Definition 3.3) define it, a block of a factor subshift all of whose
admissible preimages agree at a selected coordinate, stated for factor maps of subshifts, where a certificate concerns
finite accepted markings with a covering origin. Constrained decoding meets the question from the other side. For a
fixed suffix under an unknown left context, Melcer, Fulton, Gouda and Qian (arXiv:2402.17988v1, 2024, Section 5.3.1)
compute every position of the suffix at which a lexical token begun before it may end, where a certificate asks that one
origin be forced at every occurrence. This paper decides that predicate for an arbitrary supplied verifier, over token
sets that need not be codes and domains that are whatever the verifier accepts, with the covering token's start fixed at
the proposed offset within the window in every occurrence. The decision is an emptiness question once the badly covered
occurrences are recognized, and its proof builds that recognizer for a supplied window and origin.

**What the calculus inherits.** The closure clauses stand on the same footing and are named here for the same reason.
Rational relations compose (Elgot and Mezei, IBM Journal of Research and Development 1965, Theorem 4.13), which is the
pipeline clause once a supplied finite-state labeler makes the intermediate alphabet finite. The labeler is the clause's
hypothesis, since a boundary verifier and a finite set of names do not by themselves recover which name a segment
carries, which is Proposition 4. Restriction to a regular schema is intersection with a lifted schema automaton (Rabin
and Scott, IBM Journal of Research and Development 1959, Theorem 6). Byte fallback totalizes a literal vocabulary the
way Cognetta and Okazaki (Computational Linguistics 2025) assume the atomic alphabet sits inside the vocabulary, and the
enlarged vocabulary is again finite and literal. Lemma 1 is the contrapositive of the classical fact that a rational
relation has a regular domain. The lift's factoring, an inner stage's realized label streams computed once and then
intersected with an outer stage's condition, is the one Park, Zhou and D'Antoni (ICML 2025) use at another interface.
They precompute from a maximal-munch lexer composed with a language model's vocabulary the terminal sequences each
vocabulary token can realize from each lexer state. Against a context-free parser's states they settle ahead of time
many sequences every stack accepts and many that no stack accepts, the rejected ones by a finite-state overapproximation
that erases the stack. Only the unresolved residue is checked against the stack while decoding. Dong, Ruan, Cai, Lai,
Xu, Zhao and Chen (MLSys 2025) settle a grammar's context-independent accepted and rejected tokens offline and check the
rest against the live stack, the same split between the settled and the live, and Li, Dong, Li and Li (ISSTA 2026)
compile the acceptance condition of every vocabulary token into one classifier of the parser stack, so that one
examination of the stack yields the whole mask. The lift's inner language is instead the flagged label streams of whole
stage-one segmentations, its outer language a finite-state stage's refusal to start a token at the flag, and the
emptiness of their product decides whether a certificate transfers, where those systems decide which token a mask
admits. The closure operations themselves are all known, and the section's contribution is to carry each one out on the
verifier, so that the composite is a verifier again and all six decisions run on it unchanged, and to show the
label-recovery hypothesis necessary.

**What the weighted theory inherits.** The product of a verifier with a rational finite-state source is a weighted
automaton over the nonnegative rationals, so each mass of Section 7 is the total weight of a rational series, the series
giving each string the weight of its accepted runs. Rational series are the notion Schützenberger (Information and
Control 1961) introduced for integer coefficients through matrix representations (his Definition 3), taken here over the
nonnegative rationals. The linear systems that sum such a series are the standard ones of weighted-automata theory
(Droste, Kuich and Vogler, Springer 2009). Where a deterministic online presentation exists the product is an absorbing
Markov chain with rewards, and the masses and expectations are the fundamental-matrix quantities of Kemeny and Snell
(Van Nostrand 1960, Theorems 3.2.4, 3.3.5 and 3.3.7). Where none exists, the expected token, boundary and mismatch
counts are expectation-semiring totals in the sense of Eisner (ACL 2002, Section 4), additive rewards summed over the
paths of a weighted automaton. None of the linear algebra is new here, and neither is the raw product. Benedikt,
Lenhardt and Worrell (arXiv:1405.4560, 2014) read an unambiguous finite automaton against a Markov chain and solve an
affine system of the same form over the trimmed product. The event there is a trajectory with an accepted prefix, here
the whole string a stopping source emits. Their solution is unique because every retained state reaches acceptance with
positive probability, which is the argument Section 7 runs, with functionality supplying the unambiguity and almost sure
stopping the tail. The proof of that note's infinite-word theorem was withdrawn, and Baier, Kiefer, Klein, Müller and
Worrell (Journal of Computer and System Sciences 2023) record that its finite-word lemma is unaffected. The memoryless
case, letters and a stop drawn with fixed probabilities and read by an unambiguous automaton, opens Kiefer and
Widdershoven (MFCS 2019) as a known example. A row sum above one is the ordinary condition of that product, and the
multiplicity bound is the run bound any trim unambiguous automaton supplies per prefix. The positivity law is the
support principle for series over the nonnegative rationals, a positive semiring, whose supports are again recognizable
(Kirsten, Acta Cybernetica 2011, Theorem 3.1(1)). That a total is zero exactly when every coefficient is zero is the
elementary half of the same positivity. This paper's own part is the identification of that support with the boundary
decision's differing strings inside the source's support, so that one construction makes the measure and the logic agree
about zero. This paper's too are the systems themselves, built from the supplied verifiers through the same interface
the decisions use.

**The hypothesis is a normal form.** Proposition 2 is Eilenberg and Schützenberger's theorem (Eilenberg, Academic Press
1974) read over the marked alphabet. The general-transducer undecidability of Filiot, Jecker, Löding and Winter (ICALP
2016) concerns relations that are not functions, and the presentation-level reductions of Section 4 are the standard
uniformity obstruction.

**The frontier, and whose example it is.** The separation of Section 4 is Karttunen's example and Choffrut's theorem,
cited there. The same paper of Karttunen (ACL 1996) builds tokenizers by inserting an end-of-token mark after each
match, a marked string in another encoding, length-changing where the bit encoding used here is length-preserving. The
four decisions ask only for verification, which the example still admits, so this paper's contribution begins outside
the inner ring of Figure 1.

**The instances have finite-state accounts.** Cognetta and Okazaki (Computational Linguistics 2025) constructively
represent both BPE and MaxMatch, the algorithm behind WordPiece, by finite-state transducers, and the extended version
of Berglund, Martens and van der Merwe (CIAA 2024) gives an input-deterministic transducer relating strings to their BPE
tokenizations along with equivalence checking of tokenization dictionaries. Their constructions place canonical BPE
inside this paper's class, which is why Section 8 treats it as an exact instance. They also explain why it is not in
general a maximal-munch policy, since BPE, in Cognetta and Okazaki's words, "does not tokenize strings from left to
right and requires a notion of priority", its vocabulary being an ordered merge list in the sense of Sennrich, Haddow
and Birch (ACL 2016). That distance from maximal munch is a per-vocabulary quantity, measurable as boundary agreement
between the two readings, and it is not assumed away. Neither Cognetta and Okazaki nor Berglund and colleagues decide
certification or the boundary gap, and Cognetta and Okazaki mask token choices to preserve an accepted completion, which
instantiates the co-accessibility condition of this paper's realizability inside their own constructions. Both do treat
composition, Cognetta and Okazaki defining transducer composition explicitly and Berglund and colleagues naming
composition of tokenized languages as an application, so this paper's part is composition of segmentation stages with
certificate transfer, composition as such being theirs. The dictionary equivalence of Berglund and colleagues asks
whether two merge dictionaries induce the same canonical tokenizations, a question about one tokenizer family's outputs,
with a distinguishing string available from its automata by symmetric difference, where the decision here is the full
equivalence of arbitrary policies in the class. Vieira, Liu, Pasti, Emara, DuSell, LeBrun, Giulianelli, Gastaldi,
O'Donnell and Cotterell (ICML 2025) characterize canonical BPE token strings locally, a token string being canonical
exactly when each adjacent pair is, where every vocabulary token is canonical and no pre-tokenizer intervenes. That
tests a token string's own neighbors, where a certificate here is a fixed byte window whose covering token starts at the
same offset in every occurrence.

**Computing the scan.** The scan has its own literature, of which Reps (TOPLAS 1998) is the algorithmic reference,
giving maximal munch in linear time, with the conventions this paper also adopts, earliest definition on a longest-match
tie and no token language containing the empty word.

## 10 Limitations

The hypothesis is the boundary. Policies with non-regular domains are outside by Lemma 1, the exhibited context-free
segmentation among them. Canonical BPE is an instance by the constructions cited in Section 8, and its distance from
maximal munch is a per-vocabulary measurement this paper does not report. The exact weighted solving runs at toy
state-space sizes, with the certified enclosures reaching beyond, and scaling the exact solver is open. The lift of
Theorem 5 is implemented for regular inner stages under the least-index labeler and for literal outer stages, together
with the constant-labeler example, and it is run once more in Section 8 with byte-pair towers as both stages. The
literal pipelines are run through the per-label corollary, which is sufficient only. The stack proposition is proved
under the full stack's own domain and an innermost certificate, both shown necessary, and checked by enumeration alone.

The verifier, not the decisions, is where the tokenizer instances stop. The merge tower grows from 4,063 states at 64
merges to 69,773 at 128 and 841,141 at 256, so the dictionaries of Section 8, at 64 to 80 merges, are nearly three
orders of magnitude below a production table such as GPT-2's 50,000 merges. For WordPiece the tabulated verifier carries
the campaign at 4,096 entries and reaches 8,192 in a scale probe, 56,636 states and 2,965,165 transitions built in 175
seconds, and a tabulation attempted at 16,384 entries did not complete, while the construction from the vocabulary's
tries reaches 16,384 entries and the full lexical vocabulary. On the verifiers of these instances the recorded decisions
take seconds to minutes, and the lasso search over the full lexical WordPiece vocabulary takes 708.1 seconds. The
records specify 12 worker processes but do not identify the processor, operating system, or Python version. The token
automaton of Berglund, Martens and van der Merwe (CIAA 2024) has linearly many states in the dictionary, but the
marked-symbol verifier over bytes derived from it, which these decisions consume, pairs a state of the token automaton,
from its state set `Q_token`, with a node of `Trie(V)`, the trie of the byte spellings of the vocabulary `V`, so its
size is bounded by the product `O(|Q_token| |Trie(V)|)` and depends on those explicit spellings. The trie has up to one
node per byte of each token's spelling, and no polynomial in the merge count bounds it, a product's spelling being as
long as its two parts together. A decision posed on the token automaton itself is the step between the instances and a
tool, and it is not taken here.

## 11 Conclusion

The paper rests on one hypothesis, that a policy's segmentations are recognizable by a finite verifier over marked
strings. From a supplied verifier it gives constructive decisions for certification, the boundary gap, realizability and
equivalence, three of them specializing classical decisions, and with them two more, whether one certified cut and
whether all the anchors of an inventory let the resulting pieces scan alone. It survives four operations real pipelines
apply, carries certificates across stages under proved laws, and admits an exact probabilistic shadow that agrees with
the decisions about what is zero. The hypothesis is weaker than finite-state online scannability, and in this encoding
it is exactly the rational policies, so the dividing line the series has drawn case by case now has a definition.

The instruments came first and the theorems answer to them. Every executable claim and reported statistic above is one
command away from its check, except the incomplete 16,384-entry tabulation, which the module documentation records, and
the general results rest on the proofs. Regenerating the library instance's record needs a build of the library, and
regenerating the stored records of the tokenizer instances needs the two published vocabularies. The modules, the gate
and the library instance's record are archived together as a standalone record at doi:10.5281/zenodo.23708161.

## References

- N. Nidhögg. *Certified Split Points for Parallel Lexing: Exact and Modulo Discarded Tokens.* Preprint,
  arXiv:2608.03473, 2026.
- N. Nidhögg. *Certified Split Windows for Parallel Lexing: Recovering Boundaries Where No Byte Certifies.* Preprint,
  arXiv:2608.09761, 2026.
- N. Nidhögg. *Certified Panic Mode: Repair-Invariant Error Recovery for Maximal-Munch Lexing.* Preprint,
  arXiv:2609.10600, 2026.
- N. Nidhögg. *Certified Splitting: Decidability and Anchor Supply for Maximal-Munch Tokenization.* Preprint,
  arXiv:2610.08854, 2026.
- N. Nidhögg. *munch: a lexical analysis library based on automata theory.* Public repository,
  https://github.com/nnidhogg/munch, 2026. The library instance of Section 8 is this library at its release 2.3.0,
  commit `fa3f20d`, whose archived record is doi:10.5281/zenodo.23287096 under the concept identifier
  doi:10.5281/zenodo.21752996.
- T. Koo, F. Liu, L. He. *Automata-Based Constraints for Language Model Decoding.* COLM 2024. Preprint:
  arXiv:2407.08103.
- M. Berglund, W. Martens, B. van der Merwe. *Constructing a BPE Tokenization DFA.* CIAA 2024, LNCS 15015, 66-78.
  Extended version: arXiv:2405.07671v2, 2025, whose Theorem 4 gives the subsequential transducer for proper
  dictionaries; journal version: Theoretical Computer Science 1083 (2026), article 116131,
  doi:10.1016/j.tcs.2026.116131.
- J. Case, S. Jain, S. Seah, F. Stephan. *Automatic Functions, Linear Time and Learning.* Logical Methods in Computer
  Science 9(3:19):1-26, 2013.
- L. Karttunen. *Directed Replacement.* ACL 1996, 108-115.
- C. Choffrut. *Une caractérisation des fonctions séquentielles et des fonctions sous-séquentielles en tant que
  relations rationnelles.* Theoretical Computer Science 5(3):325-337, 1977.
- M.-P. Béal, O. Carton. *Determinization of Transducers over Finite and Infinite Words.* Theoretical Computer Science
  289(1):225-251, 2002.
- A. Weber, R. Klemm. *Economy of Description for Single-Valued Transducers.* Information and Computation
  118(2):327-340, 1995.
- M.-P. Béal, O. Carton, C. Prieur, J. Sakarovitch. *Squaring Transducers: An Efficient Procedure for Deciding
  Functionality and Sequentiality.* Theoretical Computer Science 292(1):45-63, 2003.
- C. Frougny, J. Sakarovitch. *Synchronisation déterministe des automates à délai borné.* Theoretical Computer Science
  191(1-2):61-77, 1998.
- C. Löding, S. Repke. *Decidability Results on the Existence of Lookahead Delegators for NFA.* FSTTCS 2013, LIPIcs 24,
  327-338.
- C. Löding. *Errata for: Decidability Results on the Existence of Lookahead Delegators for NFA.* Note of 22 February
  2023, https://www.lics.rwth-aachen.de/global/show_document.asp?id=aaaaaaaabwfmhbl, read 28 October 2026.
- E. Filiot, I. Jecker, C. Löding, S. Winter. *On Equivalence and Uniformisation Problems for Finite Transducers.* ICALP
  2016, LIPIcs 55, 125:1-125:14.
- S. Eilenberg. *Automata, Languages, and Machines, Volume A.* Academic Press, 1974.
- C. C. Elgot, J. E. Mezei. *On Relations Defined by Generalized Finite Automata.* IBM Journal of Research and
  Development 9(1):47-68, 1965.
- M. Blattner, T. Head. *Single-Valued a-Transducers.* Journal of Computer and System Sciences 15(3):310-327, 1977.
- M. Cognetta, N. Okazaki. *Tokenization as Finite-State Transduction.* Computational Linguistics 51(4):1119-1149, 2025.
- T. Purason, P. Chizhov, I. P. Yamshchikov, M. Fishel. *Teaching Old Tokenizers New Words: Efficient Tokenizer
  Adaptation for Pretrained Models.* Findings of EACL 2026, 6492-6516.
- J. T. H. Smith, T. Dakhran, A. Cabrera, S. S. Lee, P. Pak, A. Tadimeti, T. Seyde, M. Labonne, A. Amini, M. Lechner.
  *In-Place Tokenizer Expansion for Pre-trained LLMs.* Preprint, arXiv:2607.15232, 2026.
- C. Herold, M. Kozielski, N. Santavas, Y. Versley, S. Khadivi. *Vocabulary Customization for Efficient Domain-Specific
  LLM Deployment.* NeurIPS 2025 CCFM Workshop. Preprint: arXiv:2509.26124.
- M. Schuster, K. Nakajima. *Japanese and Korean Voice Search.* ICASSP 2012, 5149-5152.
- J. Devlin, M.-W. Chang, K. Lee, K. Toutanova. *BERT: Pre-training of Deep Bidirectional Transformers for Language
  Understanding.* NAACL 2019, 4171-4186.
- Google Research. *BERT (source repository).* GitHub repository, commit d66a146,
  https://github.com/google-research/bert/blob/d66a146/tokenization.py, 2019.
- O. Uzan, C. W. Schmidt, C. Tanner, Y. Pinter. *Greed is All You Need: An Evaluation of Tokenizer Inference Methods.*
  ACL 2024, 813-822.
- X. Song, A. Salcianu, Y. Song, D. Dopson, D. Zhou. *Fast WordPiece Tokenization.* EMNLP 2021, 2089-2103.
- R. M. Kaplan. *Method and Apparatus for Tokenizing Text.* US Patent 5,721,939, filed 1995, assigned to Xerox
  Corporation, 1998.
- C. von Praun. *Speculative Stream Scanning.* US Patent 8,001,329, filed 2008, assigned to International Business
  Machines Corporation, 2011.
- J. E. Pentheroudakis, D. G. Bradlee, S. S. Knoll. *Tokenizer for a Natural Language Processing System.* US Patent
  7,092,871, filed 2001, assigned to Microsoft Corporation, 2006.
- M. Walther. *Context-Aware Linear Time Tokenizer.* US Patent Application Publication 2003/0149562, filed 2002, 2003.
- K. Chiu. *Parallel XML Parsing Using Meta-DFAs.* US Patent 8,782,514, filed 2009, assigned to the Research Foundation
  of the State University of New York, 2014.
- M. L. Van De Vanter. *Per-Keystroke Incremental Lexing Using a Conventional Batch Lexer.* US Patent 5,737,608, filed
  1996, assigned to Sun Microsystems, Inc., 1998.
- T. A. Wagner, S. L. Graham. *General Incremental Lexical Analysis.* Manuscript, University of California, Berkeley,
  1997; the circulating build is the December 1999 revision. https://harmonia.cs.berkeley.edu/papers/twagner-lexing.pdf
- A. Barenghi, S. Crespi Reghizzi, D. Mandrioli, F. Panella, M. Pradella. *Parallel parsing made practical.* Science of
  Computer Programming 112:195-226, 2015.
- W. Shao, L. Zheng, P. Wang, P. Zheng, J. Li, Y. Fan. *LoPT: Lossless Parallel Tokenization Acceleration for Long
  Context Inference of Large Language Model.* ACL 2026, 33107-33122.
- Z. Zhang, Z. Cao. *TokTier: Exact Stateful CPU+GPU Tokenization for Agentic LLM Serving.* Preprint, arXiv:2607.29678,
  version 3 of 6 August 2026.
- Y. Wang, R. Wang, W. Pang, J. Han, Y. Qi, D. Hu, K. Chen. *ReTokSync: Self-Synchronizing Tokenization Disambiguation
  for Generative Linguistic Steganography.* Preprint, arXiv:2604.25486, 2026.
- M. Berglund, B. van der Merwe. *Formalizing BPE Tokenization.* NCMA 2023, EPTCS 388, 16-27. Preprint:
  arXiv:2309.08715.
- S. Jiang, R. Gong. *Incremental BPE Tokenization.* ICML 2026, PMLR 306, 52312-52342.
  proceedings.mlr.press/v306/jiang26l.html
- K. Mamouras, A. W. Li, Y. Yang. *An Efficient Algorithm for Streaming BPE Tokenization.* PACMPL 10(PLDI), article 252,
  2085-2108, 2026.
- M. Rød. *Gigatoken.* https://github.com/marcelroed/gigatoken, revision 542367a of 21 July 2026, released on PyPI as
  version 0.9.0. The vocabulary pair rule is the comment in `finalize_speed_paths` of `src/bpe/sentencepiece.rs`.
- T. Kudo. *SentencePiece Lite Runtime.* https://github.com/google/sentencepiece, file `lite/README.md` at its initial
  revision a2cb4d8 of 15 September 2026, Section 6.3, "Safe Boundary Pre-tokenization", its bullet "Character Bigram
  Co-occurrence Principle".
- C. Xu. *5.5x Faster HuggingFace Tokenizers for a Single Sequence.* https://charlesxu.io/parallel-tokenizer/, blog
  dated 25 November 2025; the merge-table check, `compute_never_merges_across`, is present in commit 9eecc71 of 9
  October 2026 on the branch `parallel` of https://github.com/cxuu/tokenizers.
- A. You. *BlockBPE: Parallel BPE Tokenization.* Preprint, arXiv:2507.11941, 2025.
- V. G. Kadamba, K. Jaisankar. *GPUTOK: GPU Accelerated Byte Level BPE Tokenization.* Preprint, arXiv:2603.02597, 2026.
- A. Kovilur, V. C. Shekar, A. Azad. *LLBPE: Linked-List Based GPU-Parallel BPE Tokenizer.* Preprint, arXiv:2610.04951,
  2026.
- C. Frougny, J. Sakarovitch. *Synchronized Rational Relations of Finite and Infinite Words.* Theoretical Computer
  Science 108(1):45-82, 1993.
- B. Khoussainov, A. Nerode. *Automatic Presentations of Structures.* In D. Leivant (editor), Logic and Computational
  Complexity, International Workshop LCC'94, Selected Papers, LNCS 960, 367-392, Springer, 1995.
- B. R. Hodgson. *On Direct Products of Automaton Decidable Theories.* Theoretical Computer Science 19(3):331-335, 1982.
- M. Veanes, P. Hooimeijer, B. Livshits, D. Molnar, N. Bjørner. *Symbolic Finite State Transducers: Algorithms and
  Applications.* POPL 2012, 137-150.
- M. O. Rabin, D. Scott. *Finite Automata and Their Decision Problems.* IBM Journal of Research and Development
  3(2):114-125, 1959.
- P. J. Ramadge, W. M. Wonham. *Supervisory Control of a Class of Discrete Event Processes.* SIAM Journal on Control and
  Optimization 25(1):206-230, 1987.
- P. A. Abdulla, P. Krčál, W. Yi. *R-Automata.* CONCUR 2008, LNCS 5201, 67-81.
- M. Colange, D. Racordon, D. Buchs. *Computing Bounds for Counter Automata.* AVoCS 2015, ECEASST 72.
- A. W. Li, Y. Yang, K. Mamouras. *Static Analysis for Efficient Streaming Tokenization.* ASPLOS 2026, 1880-1896.
- J. Berstel, D. Perrin, C. Reutenauer. *Codes and Automata.* Encyclopedia of Mathematics and its Applications 129,
  Cambridge University Press, 2010.
- A. Carpi, F. D'Alessandro. *On Incomplete and Synchronizing Finite Sets.* Theoretical Computer Science 664:67-77,
  2017.
- A. Restivo. *A Combinatorial Property of Codes Having Finite Synchronization Delay.* Theoretical Computer Science
  1(2):95-101, 1975.
- J.-L. Lassez. *Messages parasites et codes synchronisants.* Groupe d'étude d'algèbre, tome 1 (1975-1976), exposé no.
  17 of 28 May 1976, 17-01 to 17-04. https://numdam.org/item/GEA_1975-1976__1__A17_0/
- G. Fici, G. Romana, M. Sciortino, C. Urbina. *Morphisms and BWT-Run Sensitivity.* MFCS 2025, LIPIcs 345, 49:1-49:18.
- H. Morita, A. J. van Wijngaarden, A. J. Han Vinck. *On the Construction of Maximal Prefix-Synchronized Codes.* IEEE
  Transactions on Information Theory 42(6):2158-2166, 1996.
- T. Mytkowicz, M. Musuvathi, W. Schulte. *Data-Parallel Finite-State Machines.* ASPLOS 2014, 529-542.
- Z. Zhao, B. Wu, X. Shen. *Challenging the "Embarrassingly Sequential": Parallelizing Finite State Machine-Based
  Computations through Principled Speculation.* ASPLOS 2014, 543-558.
- P. Prabhu, G. Ramalingam, K. Vaswani. *Safe Programmable Speculative Parallelism.* PLDI 2010, 50-61.
- M. M. Lester. *Position Paper: The Science of Boxing.* PLAS 2013, 83-88.
- M. Lester, L. Ong, M. Schäfer. *Information Flow Analysis for a Dynamically Typed Language with Staged
  Metaprogramming.* Journal of Computer Security 24(5):541-582, 2016.
- R. L. Adler, D. Coppersmith, M. Hassner. *Algorithms for Sliding Block Codes: An Application of Symbolic Dynamics to
  Information Theory.* IEEE Transactions on Information Theory 29(1):5-22, 1983.
- D. Melcer, N. Fulton, S. K. Gouda, H. Qian. *Constrained Decoding for Code Language Models via Efficient Left and
  Right Quotienting of Context-Sensitive Grammars.* Preprint, arXiv:2402.17988v1, 28 February 2024.
- K. Park, T. Zhou, L. D'Antoni. *Flexible and Efficient Grammar-Constrained Decoding.* ICML 2025, PMLR 267,
  48262-48275. Preprint: arXiv:2502.05111.
- Y. Dong, C. F. Ruan, Y. Cai, R. Lai, Z. Xu, Y. Zhao, T. Chen. *XGrammar: Flexible and Efficient Structured Generation
  Engine for Large Language Models.* MLSys 2025. Preprint: arXiv:2411.15100.
- Y. Li, Y. Dong, J. Li, G. Li. *Efficient Grammar-Constrained Decoding via Parser Stack Classification.* PACMSE
  3(ISSTA):1856-1880, 2026. Preprint: arXiv:2608.03065.
- M. P. Schützenberger. *On the Definition of a Family of Automata.* Information and Control 4(2-3):245-270, 1961.
- M. Droste, W. Kuich, H. Vogler (editors). *Handbook of Weighted Automata.* Monographs in Theoretical Computer Science,
  An EATCS Series, Springer, 2009.
- J. G. Kemeny, J. L. Snell. *Finite Markov Chains.* Van Nostrand, Princeton, 1960.
- J. Eisner. *Parameter Estimation for Probabilistic Finite-State Transducers.* ACL 2002, 1-8.
- M. Benedikt, R. Lenhardt, J. Worrell. *Model Checking Markov Chains Against Unambiguous Büchi Automata.*
  arXiv:1405.4560, 2014; version 2 of 26 March 2016 withdraws the proof of its infinite-word theorem (Theorem 2).
- C. Baier, S. Kiefer, J. Klein, D. Müller, J. Worrell. *Markov Chains and Unambiguous Automata.* Journal of Computer
  and System Sciences 136:113-134, 2023.
- S. Kiefer, C. Widdershoven. *Efficient Analysis of Unambiguous Automata Using Matrix Semigroup Techniques.* MFCS 2019,
  LIPIcs 138, 82:1-82:13.
- D. Kirsten. *The Support of a Recognizable Series over a Zero-sum Free, Commutative Semiring is Recognizable.* Acta
  Cybernetica 20(2):211-221, 2011.
- R. Sennrich, B. Haddow, A. Birch. *Neural Machine Translation of Rare Words with Subword Units.* ACL 2016, 1715-1725.
- T. Vieira, T. Liu, C. Pasti, Y. Emara, B. DuSell, B. LeBrun, M. Giulianelli, J. L. Gastaldi, T. J. O'Donnell, R.
  Cotterell. *Language Models over Canonical Byte-Pair Encodings.* ICML 2025, PMLR 267, 61413-61443. Preprint:
  arXiv:2506.07956.
- T. Reps. *"Maximal-Munch" Tokenization in Linear Time.* ACM Transactions on Programming Languages and Systems
  20(2):259-273, 1998.
