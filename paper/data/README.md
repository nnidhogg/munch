# Evaluation data

Three measurements back the evaluation section. They are kept apart rather than merged because they were taken on
different machines, at different commits, under different conditions, and the differences between them are part of what
the section reports.

## `benchmark.txt`

Summaries only, from an Intel Core i9-12900K running Ubuntu 24.04 under WSL 2. Taken at commit
`c0e2fb62b17d4e4553fd02cc44a4059351fd1ff1` with the fixed-order harness, before the interleaved rounds and the size
sweep existed, so it cannot be reproduced scenario by scenario on current `master`. The `benchmark/split-points-2026-07`
tag preserves the tree it came from; `paper/README.md` gives the worktree recipe.

## `bare-metal-unpinned/` and `bare-metal-pinned/`

Full output of `tools/benchmark/collect.sh` on an AMD Ryzen 9 9950X3D running Ubuntu 26.04, at commit
`7d067c2354fc4cfaf135fbcac58c4a1eed26654c` with a clean tree, `performance` governor, over 1, 16, 128 and 512 MiB
corpora. The pinned run confines the process to `taskset -c 0-7`, the eight physical cores of one L3 domain; the
unpinned run is free to use all 32 logical processors.

Each directory holds:

| File | Contents |
|------------------|--------------------------------------------------------------------------------|
| `environment.txt` | machine, toolchain, governor, load, and the commit the run was taken at |
| `topology.txt` | `lscpu -e`, which is what identifies the two L3 domains |
| `summary.txt` | the human-readable table, best/median/worst per scenario |
| `observations.csv` | every timed pass of the scaling scenarios: `run,scenario,input_mib,round,seconds,mib_per_s` |

The CSV records every pass **of the ten scaling scenarios** and only those: `lexer_all` and `chunked1` through
`chunked8`, over each corpus and size, fifteen passes each, which is 600 rows. That is what makes the spread in those
figures visible rather than asserted.

Where the two disagree, `summary.txt` is the figure to quote. The benchmark computes its statistics from the timings in
memory, while the CSV stores each pass rounded, so any statistic recomputed from the CSV can land 0.1 MiB/s away when
the value it selects sits on a rounding boundary. Counting all three printed statistics, this affects three of the 40
pinned groups, all of them medians, and seven of the 40 unpinned groups: one median, five best values and one worst. The
published tables follow `summary.txt`.

Everything else in `summary.txt` (`lexer/`, `tokenizer/`, `build/`, `register/`, `total/`, `plan/`, `threads/`) is
reported as best, median and worst over 15 passes with no per-pass record. Any statistic quoted from those scenarios
rests on the summary alone and cannot be recomputed from the CSV.

**Filter by the `run` column before computing anything.** The benchmark appends, so a CSV can in principle hold more
than one run. These two files each hold exactly one, and `collect.sh` now refuses to write into a directory that already
contains observations, but the column is the check that does not depend on remembering that.
