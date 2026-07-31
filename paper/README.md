# Certified Split Points: the technical report in LaTeX

`split-points.tex` is the formal version of [docs/split_points.md](../docs/split_points.md): the same argument with
numbered definitions, a lemma, the theorem and its corollary, and a bibliography. The markdown report stays the
living form, linked from the README and kept next to the implementation it describes; this directory holds the form
meant for a preprint server.

## Building

```
sudo apt-get install -y texlive-latex-recommended texlive-fonts-recommended texlive-latex-extra latexmk
make
```

The result is `split-points.pdf`.

## Evaluation data

`data/benchmark.txt` is the raw benchmark output the evaluation section cites, captured at a named commit with the
machine, compiler, and command recorded in its header. The paper quotes that file rather than the README's table,
so ordinary benchmark refreshes cannot silently change what the paper claims. Reproduce it with:

```
git worktree add /tmp/munch-paper $(git log -1 --format=%H -- paper/data/benchmark.txt)
cd /tmp/munch-paper
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMUNCH_BUILD_BENCHMARK=ON
cmake --build build
./build/tools/benchmark/munch_benchmark 16 15
```

The checkout matters: `master` moves, and the benchmark's definitions move with it, so building the tip reproduces
a different program. Throughput naturally varies by machine; the point of the worktree is that the *program* is the
one the numbers came from.
