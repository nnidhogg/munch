# The technical reports in LaTeX

Two sibling reports live here, sharing `figures/` and `data/`.

`split-points/split-points.tex` is the formal version of [docs/split_points.md](../docs/split_points.md), published as
[arXiv:2608.03473](https://arxiv.org/abs/2608.03473): the argument stated over the live subautomaton, with numbered
definitions, the split-invariance theorem and its malformed-input corollary, the characterization theorem that makes
the condition necessary as well as sufficient, the weaker certificate modulo the tokens a caller discards with its own
soundness and strict-extension results, and a bibliography.

`split-windows/split-windows.tex` is its companion, published as
[arXiv:2608.09761](https://arxiv.org/abs/2608.09761) and mirrored by [docs/split_windows.md](../docs/split_windows.md):
the generalization from certified bytes to certified windows, with the conservative cloud model and its soundness
proof, the finite quotient that makes the search a decision procedure, the specialization theorem tying length one to
the shipped predicate, and the strictness witnesses. Its evaluation runs as asserted probes in the test suite
(`tools/probes/window_gate.cpp`), and `split-windows/campaign.md` names the archived collections behind its figures.

The markdown reports are the accessible form, linked from the README and kept next to the implementation they
describe; they carry the same claims but not every formal qualification, so where the two differ this directory is
authoritative.

## Building

```
sudo apt-get install -y texlive-latex-recommended texlive-fonts-recommended texlive-latex-extra latexmk
make
```

The recursive Makefile builds both reports: `split-points/split-points.pdf` and `split-windows/split-windows.pdf`.
Building needs no Graphviz and no compiler: the figure PDFs under `figures/` are checked in.

## Figures

`figures/certificates.cpp` draws the three automata of the re-entrancy figure by asking munch for the minimized table it
compiles for each token set, so the figure is the library's own output rather than a hand drawing. It also prints what
`Lexer::is_split_point()` answers for each candidate byte, which is what the caption reports. Regenerate the figures
only when the exported DOT or the automata change:

```
c++ -std=c++23 -I libs/common/include -I libs/core/include -I libs/dfa/include -I libs/dfa/tools/include \
    -I libs/nfa/include -I libs/regex/include paper/figures/certificates.cpp -o /tmp/certificates \
    -L build/libs/core -L build/libs/dfa -L build/libs/nfa -L build/libs/regex \
    -lmunch_core -lmunch_dfa_tools -lmunch_dfa -lmunch_nfa -lmunch_regex
/tmp/certificates paper/figures
for f in paper/figures/*.dot; do dot -Tpdf "$f" -o "${f%.dot}.pdf"; done
for f in paper/figures/*.dot; do dot -Tsvg "$f" -o "docs/$(basename "${f%.dot}").svg"; done
```

Run those from the repository root against an existing build tree. The PDFs stay here for the paper; the SVGs go to
`docs/`, where the markdown report embeds them.

## Applicability table

`figures/applicability.cpp` produces the applicability table rather than documenting it. It builds each studied token
set, reads `Lexer::is_split_point()` for all 256 byte values, and asserts the answer against the published row, so a
change in the certificate that would silently invalidate the table fails the program instead. It prints one line per row
and exits non-zero on any disagreement:

```
c++ -std=c++23 -I libs/common/include -I libs/core/include -I libs/dfa/include -I libs/nfa/include \
    -I libs/regex/include -I tools/benchmark/include -I build/generated \
    paper/figures/applicability.cpp tools/benchmark/src/harness.cpp -o /tmp/applicability \
    -L build/libs/core -L build/libs/dfa -L build/libs/nfa -L build/libs/regex \
    -lmunch_core -lmunch_dfa -lmunch_nfa -lmunch_regex -lpthread
/tmp/applicability
```

## Evaluation data

The evaluation section cites five measurements, described in full by `data/README.md`. Its primary source is
`data/bare-metal-pinned-run2/`, a run on an AMD Ryzen 9 9950X3D confined to one L3 domain, with
`data/bare-metal-unpinned-run2/` the same measurement free to use every logical processor; both carry the machine, the
topology, the summary, and every pass of the scaling scenarios individually. The `-run1` pair is an earlier collection
of the same two placements at an earlier commit, kept because the paper reports what changed between them. The
bare-metal trees are preserved by the `benchmark/split-points-2026-08-run1` and `benchmark/split-points-2026-08` tags;
`benchmark/split-points-2026-07` preserves the Intel run's code-identical archival tree, as described below.

`data/benchmark.txt` is the earlier virtualized run, retained because the paper draws a result from the contrast between
the two environments. It holds the best, median, and worst of each scenario rather than the individual observations,
with the measured checkout, machine, compiler, and command recorded in its header. The header's `tree` is the checkout
whose benchmark produced these rows: it has the one-chunk baseline the evaluation reports and predates the interleaved
harness. The `benchmark/split-points-2026-07` tag points at its direct child `c0e2fb6`, which changes only paper and
documentation files, so the tagged tree builds the identical program. The paper quotes that file rather than the
README's table, so ordinary benchmark refreshes cannot silently change what the paper claims. Reproduce it with:

```
git worktree add /tmp/munch-paper 1aca0cd13837cd40ab90a32222012eabdfc6018b
cd /tmp/munch-paper
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMUNCH_BUILD_BENCHMARK=ON
cmake --build build
./build/tools/benchmark/munch_benchmark 16 15
```

That commit is not on `master`: the history was squashed after it, and it is kept reachable by the tag
`benchmark/split-points-2026-07` so these steps keep working. One caveat about it: at that commit `mdspan` was fetched
from a moving branch rather than a pinned revision, so a reproduction today may compile against different dependency
sources than the archived numbers did. The builds this repository was verified against resolved it to
`884f17a24301955d47cbb22318f06b8d8bee7ca3`, which is what `master` now pins; add `-DFETCHCONTENT_SOURCE_DIR_MDSPAN`
pointing at that revision to remove the doubt entirely. The tip's benchmark is a later, interleaved harness that would
not reproduce these numbers even on the same machine.

The checkout matters: `master` moves, and the benchmark's definitions move with it, so building the tip reproduces a
different program. Throughput naturally varies by machine; the point of the worktree is that the *program* is the one
the numbers came from.

To take a *new* measurement rather than reproduce the archived one, use
[`tools/benchmark/collect.sh`](../tools/benchmark/collect.sh) from the repository root:

```
./tools/benchmark/collect.sh ~/munch-run 1,16,128,512 15
```

It builds the current benchmark, records the machine it ran on, and writes every timed pass of the scaling scenarios to
`observations.csv` rather than only the best, median, and worst. The current harness runs the scaling scenarios in
interleaved rounds and sweeps input sizes, neither of which the archived run did, so its output is not comparable to
`data/benchmark.txt` scenario by scenario and should be archived as its own artifact.
