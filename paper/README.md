# Certified Split Points: the technical report in LaTeX

`split-points.tex` is the formal version of [docs/split_points.md](../docs/split_points.md): the same argument stated
over the live subautomaton, with numbered definitions, the split-invariance theorem and its malformed-input corollary,
the characterization theorem that makes the condition necessary as well as sufficient, and a bibliography. The markdown
report is the accessible form, linked from the README and kept next to the implementation it describes; it carries the
same claims but not every formal qualification, so where the two differ this directory is authoritative.

## Building

```
sudo apt-get install -y texlive-latex-recommended texlive-fonts-recommended texlive-latex-extra latexmk
make
```

The result is `split-points.pdf`. Building needs no Graphviz and no compiler: the figure PDFs under `figures/` are
checked in.

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

`figures/applicability.cpp` produces the applicability table rather than documenting it. It builds each surveyed token
set, reads `Lexer::is_split_point()` for all 256 byte values, and asserts the answer against the published row, so a
change in the certificate that would silently invalidate the table fails the program instead. It prints one line per row
and exits non-zero on any disagreement:

```
c++ -std=c++23 -I libs/common/include -I libs/core/include -I libs/dfa/include -I libs/nfa/include \
    -I libs/regex/include paper/figures/applicability.cpp -o /tmp/applicability \
    -L build/libs/core -L build/libs/dfa -L build/libs/nfa -L build/libs/regex \
    -lmunch_core -lmunch_dfa -lmunch_nfa -lmunch_regex -lpthread
/tmp/applicability
```

## Evaluation data

`data/benchmark.txt` is the summarized benchmark output the evaluation section cites, holding the best, median, and
worst of each scenario rather than the individual observations, with the measured commit, machine, compiler, and command
recorded in its header. That commit is the tree whose benchmark produced these rows: it has the one-chunk baseline the
evaluation reports and predates the interleaved harness. The paper quotes that file rather than the README's table, so
ordinary benchmark refreshes cannot silently change what the paper claims. Reproduce it with:

```
git worktree add /tmp/munch-paper c0e2fb62b17d4e4553fd02cc44a4059351fd1ff1
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
./tools/benchmark/collect.sh ~/munch-run 1,16,128 15
```

It builds the current benchmark, records the machine it ran on, and writes every individual timed pass to
`observations.csv` rather than only the best, median, and worst. The current harness runs the scaling scenarios in
interleaved rounds and sweeps input sizes, neither of which the archived run did, so its output is not comparable to
`data/benchmark.txt` scenario by scenario and should be archived as its own artifact.
