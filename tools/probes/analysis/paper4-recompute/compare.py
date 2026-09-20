#!/usr/bin/env python3
# Holds the certified-splitting paper's committed emissions to what munch_paper4_recompute recomputed from the
# library's own decisions, line by line.
#
# The probe writes one file per emission, named as the paper's data directory names it, and this script reads
# each committed file and gives a verdict on every one of its lines:
#
#   agree            the probe wrote the same line, byte for byte, each line the probe wrote answering for one
#                    committed line, so a line committed twice needs the probe to write it twice
#   agree (prefix)   the probe wrote a proper prefix of the line ending at a field boundary, the comma and blank
#                    the emissions part their fields with, the remainder being something it does not recompute,
#                    the inventory key that digests the Python sources or the prose after a verdict; the
#                    remainder is shown
#   differ           the probe wrote a line that names the same figure, the two agreeing once every digit run
#                    is masked, with a different value; both are shown
#   not recomputed   the probe wrote nothing for the line: prose, an attribution, or a figure that would need
#                    a decision the library does not carry
#
# Lines the probe wrote that match no committed line are listed after each file as additions: the conservative
# model's counts beside the exact ones, and the library's own gap convention beside the campaign's. A committed
# emission the probe wrote no file for is a run that did not reach it, listed as missing, and not a file of lines
# not recomputed.
#
# Usage: compare.py <committed data directory> <probe output directory>
#
# The exit status is one when any line differs or any emission is missing, so the script can gate;
# not-recomputed lines do not fail it, since they say what was not checked rather than that a check failed.

from __future__ import annotations

import re
import sys

sys.dont_write_bytecode = True

from pathlib import Path

EMISSIONS = (
    "splitting-stats.txt",
    "splitting-supply-table.tex",
    "edit-slack-table.tex",
    "campaign-splitting-stats.txt",
    "campaign-anchor-table.txt",
    "campaign-supply-table.tex",
    "frozen-inventory-stats.txt",
    "frozen-transfer-table.tex",
    "depth-series-stats.txt",
    "budget-coverage-stats.txt",
    "wide-cutoff-sweep.txt",
)

DIGITS = re.compile(r"\d+")


def masked(line: str) -> str:
    """The line with every digit run replaced, the shape two lines share when they name the same figure."""
    return DIGITS.sub("#", line)


def read_lines(path: Path) -> list[str]:
    """The file's lines, none when there is no file."""
    return path.read_text(encoding="utf-8").splitlines() if path.exists() else []


def compare(committed_dir: Path, probe_dir: Path) -> int:
    """Prints the verdict table and returns the number of differing lines and missing emissions, a committed
    emission that is not there, that this run of the probe did not write, or that it recomputed no line of,
    counted missing."""
    totals = {"agree": 0, "agree (prefix)": 0, "differ": 0, "not recomputed": 0}
    missing = 0
    # What this run of the probe wrote, which is what the comparison reads: a file an earlier run left in the
    # directory is named by no manifest and is no evidence of anything.
    written = set(read_lines(probe_dir / "manifest.txt"))
    # The emissions this run wrote and agreed on no line of.
    barren = set()
    for name in EMISSIONS:
        committed = read_lines(committed_dir / name)
        if not committed:
            print(f"== {name}: missing, no committed emission")
            missing += 1
            continue
        if name not in written:
            print(f"== {name}: missing, this run of the probe did not write it")
            missing += 1
            continue
        if not read_lines(probe_dir / name):
            print(f"== {name}: missing, the probe wrote no line for it")
            missing += 1
            continue
        # Each probe line answers for one committed line: the pool it is taken from once matched.
        pool = list(read_lines(probe_dir / name))
        agreed = 0
        print(f"== {name}")
        for line in committed:
            detail = ""
            if line in pool:
                verdict = "agree"
                pool.remove(line)
            else:
                prefix = next((p for p in pool if line.startswith(p + ", ")), None)
                shaped = [p for p in pool if masked(p) == masked(line)]
                if prefix is not None:
                    verdict = "agree (prefix)"
                    detail = f"remainder not recomputed: {line[len(prefix) + 2:]!r}"
                    pool.remove(prefix)
                elif shaped:
                    verdict = "differ"
                    detail = f"probe: {shaped[0]}"
                    pool.remove(shaped[0])
                else:
                    verdict = "not recomputed"
            totals[verdict] += 1
            agreed += verdict.startswith("agree")
            print(f"  [{verdict:15}] {line}")
            if detail:
                print(f"  {'':17} {detail}")
        for line in pool:
            print(f"  [addition       ] {line}")
        if not agreed:
            barren.add(name)
    print()
    print("totals: " + ", ".join(f"{k} {v}" for k, v in totals.items()) + (f", missing {missing}" if missing else ""))
    # An emission the probe wrote but agreed on no line of recomputed nothing of it, whatever it printed, which is
    # the shape a probe writing unrelated output leaves; the count of those is a failure of its own, so one agreeing
    # line elsewhere cannot carry a run.
    for name in sorted(barren):
        print(f"== {name}: the probe wrote it and agreed on no line of the committed emission")
    return totals["differ"] + missing + len(barren)


def main() -> int:
    if len(sys.argv) != 3:
        sys.exit("usage: compare.py <committed data directory> <probe output directory>")
    return 1 if compare(Path(sys.argv[1]), Path(sys.argv[2])) else 0


if __name__ == "__main__":
    sys.exit(main())
