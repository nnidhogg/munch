# The archived benchmark campaign: protocol

The instruments exist and run in CI; this file is the checklist that turns them into archivable figures. Nothing
in the draft's evaluation may quote a number that did not come through this protocol.

## Requirements

- A quiet machine: no browser, no other users, performance governor, and ideally bare metal. The dev box (the
  README's i9-12900K under WSL2) is acceptable for README rows; the paper prefers the bare-metal ritual used for
  arXiv:2608.03473.
- A tagged release containing `munch_window_bench`, so the version DOI the paper pins can name a tree that holds
  the instrument. This gates the campaign on the release after v1.3.0.
- A clean checkout of that tag: the CSV carries commit and dirty on every row, and dirty rows are unusable.

## The run

1. `tools/benchmark/collect.sh` now runs the window probe with the modal size and pass count and archives
   `windows.txt` and `windows.csv` beside the other observations. Use 512 MiB and at least 15 passes, matching the
   published discipline: the corpus must exceed the last-level cache, and medians are reported over the passes
   with the spread stated.
2. Real-corpus occurrence statistics: run `munch_window_bench <size> <passes> - <files...>` over the chosen
   corpora (own sources, a large vendored codebase, prose) and record the per-file occurrence gaps printed. The
   probe does not require the files to be tokenizable; the statistic is byte-level.
3. Archive with the existing tar ritual (owner and group zeroed); place the archive under `paper/data/` BESIDE the
   split-points archives, never replacing anything, named `windows-<yyyy-mm>`.

## What the paper then takes

- Median serial and window-cut throughput with pass spread, no-byte grammar, and the speedup range.
- Median byte-cut versus window-cut ratio on the both-certificates grammar.
- Construction time for the window table, and the occurrence-gap table over the real corpora.
- Every figure re-pinned in the abstract and evaluation from `windows.csv`, and asserted in the artifact where the
  gate pattern applies.

## Standing cautions

- Dev-grade previews (16 MiB, five passes, busy box) exist in the research notes and are quotable nowhere else.
- The 16 MiB CI default fits inside a 30 MB L3: preview throughputs are cache-flattered by construction.
- If the release the campaign measures differs from the tree the draft was written against, rerun the gate first;
  the asserted figures catch drift, but only when the gate runs.
