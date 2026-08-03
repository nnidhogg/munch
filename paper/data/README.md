# Evaluation data

Five measurements back the evaluation section. They are kept apart rather than merged because they were taken on
different machines, at different commits, under different conditions, and the differences between them are part of what
the section reports. In particular the two pinned runs disagree about one scenario, and that disagreement is a published
result, so both are archived rather than the better-looking one.

## `benchmark.txt`

Summaries only, from an Intel Core i9-12900K running Ubuntu 24.04 under WSL 2. Taken at checkout
`1aca0cd13837cd40ab90a32222012eabdfc6018b` with the fixed-order harness, before the interleaved rounds and the size
sweep existed, so it cannot be reproduced scenario by scenario on current `master`. Its rows were first committed one
commit later at `c0e2fb62b17d4e4553fd02cc44a4059351fd1ff1`, a documentation-only child whose program sources are
identical; the `benchmark/split-points-2026-07` tag preserves that archival tree, and the measured checkout is its
parent, reachable from the same tag. `paper/README.md` gives the worktree recipe for the measured checkout.

## The four bare-metal directories

A selected archive of `tools/benchmark/collect.sh` output on an AMD Ryzen 9 9950X3D running Ubuntu 26.04, `performance`
governor, over 1, 16, 128 and 512 MiB corpora, each with a clean tree. The collector's `summary.txt` and
`observations.csv` are kept as written and `environment.txt` as written apart from the two redactions described below;
its `configure.log` and `build.log` were not archived, and `topology.txt` is not a collector output but a separate
`lscpu -e` capture from the same session.

| Directory | Placement | Commit |
|----------------------------|---------------------------------|-----------|
| `bare-metal-pinned-run1` | `taskset -c 0-7`, one L3 domain | `7d067c2` |
| `bare-metal-unpinned-run1` | all 32 logical processors | `7d067c2` |
| `bare-metal-pinned-run2` | `taskset -c 0-7`, one L3 domain | `141b12e` |
| `bare-metal-unpinned-run2` | all 32 logical processors | `141b12e` |

The placement column is author-recorded rather than artifact-certified: each `environment.txt` proves only `nproc: 8`
from inside the mask, `topology.txt` describes the whole machine, and neither the affinity mask nor the launch command
was captured.

The paper's tables report run 2, which was taken at the later commit, records observations at round-trip precision, and
carries the plan-and-execute scenario that run 1 predates. Run 1 is retained as a measurement of the same machine and
placement at an earlier benchmark revision. It is not a replication: the executable differs, so a difference between the
two cannot be attributed to the machine.

Each measured tree is preserved by an annotated tag, since `master` is rewritten freely:
`benchmark/split-points-2026-07` for the Intel run (it points at the archival commit `c0e2fb6`, whose parent `1aca0cd`
is the measured checkout, with identical program sources), `benchmark/split-points-2026-08-run1` for `7d067c2`, and
`benchmark/split-points-2026-08` for `141b12e`.

**At 512 MiB the two pinned runs differ most in the single-threaded rows.** The dense two-, four- and eight-chunk rows
moved by at most 0.8% and the source rows by at most 1.4%, while the plain scan moved 14.3%, from 715.9 to 818.0 MiB/s,
the one-chunk row moved 3.7% the other way, and the direction of the comparison between them inverted. That stability is
specific to 512 MiB; across the whole sweep multi-chunk rows move by as much as 5.9% pinned and 6.3% unpinned, both at
16 MiB. The paper therefore quotes both headline figures as ranges, but parallel efficiency, measured against the
one-chunk baseline, as a narrow one and the end-to-end ratio, which divides by the plain scan, as a wide one.

Two fields in each `environment.txt` were redacted after collection, marked `<redacted>` in place rather than
removed: the hostname printed by `uname -a`, and the count of logged-in users on the `uptime` lines. The machine
belongs to a volunteer and neither field bears on any published figure. The load averages beside the user count, which
do bear on the non-quiescence discussion, are untouched. `collect.sh` no longer records either field.

The redaction is on `master` only: the tagged trees this file cites by SHA predate it and still carry both fields. The
tags are left where they are so every SHA cited here keeps resolving: for the bare-metal runs to the measured tree
itself, and for the Intel run to the code-identical archival child of the measured checkout.

Each directory holds:

| File | Contents |
|------------------|--------------------------------------------------------------------------------|
| `environment.txt` | machine, toolchain, governor, load, and the commit the run was taken at |
| `topology.txt` | `lscpu -e`, which is what identifies the two L3 domains |
| `summary.txt` | the human-readable table, best/median/worst per scenario |
| `observations.csv` | every timed pass of the scaling scenarios: `run,scenario,input_mib,round,seconds,mib_per_s` |

The CSV records every pass **of the ten scaling scenarios** and only those: `lexer_all` and `chunked1` through
`chunked8`, over each corpus and size, fifteen passes each, which is 600 rows. Everything else in `summary.txt`
(`lexer/`, `tokenizer/`, `build/`, `register/`, `total/`, `plan/`, `threads/`) is reported as best, median and worst
over 15 passes with no per-pass record, so any statistic quoted from those scenarios rests on the summary alone.

In run 1 the CSV stored each pass at the default six significant digits, so a statistic recomputed from it can land 0.1
MiB/s away from the summary, which computes from the timings in memory. Recomputing best, median and worst per scenario
and size and rounding half away from zero to the summary's one decimal, that affects three of the 40 pinned groups and
seven of the 40 unpinned ones; a different rounding convention counts differently. Run 2 writes `max_digits10`, and
every statistic in both its archives recomputes from the CSV exactly.

**Filter by the `run` column before computing anything.** The benchmark appends, so a CSV can in principle hold more
than one run. Each of these files holds exactly one, and `collect.sh` now refuses to write into a directory that already
contains observations, but the column is the check that does not depend on remembering that.

## Licence

These archives are released under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/). They are
measurements, not authorship, and requiring attribution to quote a throughput number would help nobody. The report
that reads them is CC BY 4.0 and the code that produced them is MIT; see the repository README.

## `modes-2026-08`

Two runs of `munch_benchmark_compare 16 15` on an Intel machine under WSL 2, taken at `7fe4a60` on a clean tree, both
appended to one `observations.csv` and separated by its `run` column. Backs the two mode tables in the top-level
README. The benchmark records the commit and whether the tree was dirty on every CSV row, so a row detached from this
directory still says which tree produced it.

Kept because the README quotes its ratios and its per-token comparison. That comparison is reported as a property of
this archive and not of the library: here the mode grammar costs about 6.5 percent less per token, while an earlier
archive on the same machine gave about 5 percent more, the flat row having moved by 11 percent between them against
1 percent for the modal rows. The two were also taken at different commits.

`observations.csv` holds every timed pass of the five scenarios behind the two mode tables, which are the rows the
README quotes. The other engine rows in `summary.txt` are summaries only.
