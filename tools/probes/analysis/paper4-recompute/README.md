# Paper four recomputed

A differential stage for the certified-splitting paper: its emitted figures, written by the Python programs under
`explorations/certification` in the research repository, recomputed from munch's own decisions and held against the
committed emissions line by line. The paper's decider is its own exact verifier over a pending-state automaton; the
library decides the same questions from its compiled tables, and this stage is where the two are shown to agree, or
where they part and why.

## What decides what

The paper decides certified bytes by the interior-byte lemma, cross-checked against its decider; munch answers
`Lexer::is_split_point()`. The paper decides certified windows by `decide_window()`, its product search; munch
answers `window_counterexample()`, a certificate holding exactly when the search is exhaustive with no witness, and
`is_split_window()`, the conservative model, is reported beside it on a line of its own. The occurring reading is
the paper's `occurrence()` against `window_occurrence()`; anchors, density and gaps are occurrences of the inventory
in the slice against tools/audit's `supply()` over a report holding the decided pairs, the count held to the probe's
own occurrence scan; the sync distance and the density verdict are the gap corollary over the pending automaton
against `anchor_free_span()`, over the bytes and over the inventory; the campaign differential is the regex
differential auditor against `boundary_difference()`; and the split theorem, the abstraction check and the edit
trials scan with `tokenize_all()` where the paper scans with its own greedy scanner.

The token sets are rebuilt as the Python builds them: the trained byte-pair vocabulary by the same merge procedure
with the same tie rule, the GPT-2 subset from the same merge table, and each list is digested so the configuration
line's digest is what proves the token set is the paper's. The edit trials replay the paper's seeded draws through a
port of Python's generator, so the draws are the paper's and the retokenization is munch's.

## Layout

- `compare.py`, the comparison, a classification of lines rather than a bytewise diff: one verdict per committed
  line, `agree`, each probe line answering for one committed line, `agree (prefix)` where the probe's line ends at
  a field boundary and the remainder is something the probe does not recompute, `differ` with both values, or `not
  recomputed`, and the probe's additions after each file; an emission this run of the probe did not write, which
  its manifest says, or wrote no line for, is listed as missing, and one it wrote and agreed on no line of is
  listed as recomputing nothing of it. Exit status one on any differing line, any missing emission, and any
  emission nothing of which was recomputed, so a file an earlier run left in the directory satisfies nothing.
- The C++ probe lives in `../../src/paper4_recompute.cpp`, built as `munch_paper4_recompute`; it writes one file
  per emission under an output directory, named as the paper's `data/` directory names them, and a `manifest.txt`
  naming the files that run wrote, which the comparison reads so that a file an earlier run left is no part of this
  one.

## Running

The corpora are not redistributed: the corpus is `jsonexamples/twitter.json` of simdjson v3.10.1, the merge table is
the GPT-2 release's `gpt2-merges.txt`, and the campaign corpora are the recovery paper's archived r6 corpora, each
where the research repository's gate, `scripts/check-paper4.sh`, says it is. With them in hand:

```
munch_paper4_recompute <out> <twitter.json> <gpt2-merges.txt> <r6 corpus directory>
python3 compare.py <research repository>/paper-drafts/certified-splitting/data <out>
```

A section name after the paths restricts the run to it: `vocabularies`, `budget`, `depth`, `frozen`, `campaign`,
`sweep`, any other name refused. The whole run takes about eleven minutes on twenty-four threads, the GPT-2 subset's
20,690 exact window decisions more than half of it. With the output directory alone the probe runs the sections that
need no external file, the UTF-8 shape's sync distance and the wide cutoff sweep's decisions, which is what the test
entry pins.
