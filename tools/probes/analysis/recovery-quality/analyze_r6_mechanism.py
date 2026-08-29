#!/usr/bin/env python3
# Derives the coverage-mechanism figures the manuscript quotes from the r6 archive alone: the overhang
# identity and its binned rates, the certificate-shape split, the per-operation and per-size coverage,
# the uncovered geometry against the corruption end, the bin composition behind the final bin's rebound,
# and the bare row's within-row decomposition. Companion to analyze_r6.py, which audits the archive and
# emits the main tables; this program emits only the mechanism numbers, so every one of them is the
# output of a checked-in program rather than a transcription. Reads the campaign CSV, decompressed or
# gzip-compressed, prints one labeled figure per line, and, given an output directory, writes the same
# lines to r6-mechanism.txt there, the archived copy of which is compared byte for byte by the test
# suite.
#
# Usage: analyze_r6_mechanism.py <campaign csv or csv.gz> [output directory]
#
# The overhang is the corruption end past the failure offset, h = e - f, and the identity is exact: with
# the search travel l = q - (f + 1), an answer is covered precisely when l >= h - 1. The search never
# looks before the blind anchor, l >= 0, so an overhang of at most one forces coverage by construction.

import csv
import gzip
import os
import statistics
import sys

COLUMN_COUNT = 28

BINS = [
    ("<=0", None, 0),
    ("1", 1, 1),
    ("2", 2, 2),
    ("3", 3, 3),
    ("4-7", 4, 7),
    ("8-15", 8, 15),
    ("16-31", 16, 31),
    ("32+", 32, None),
]


def main():
    if sys.flags.optimize:
        sys.exit("analyze_r6_mechanism.py: refusing to run with assertions disabled (-O/PYTHONOPTIMIZE)")
    if len(sys.argv) not in (2, 3):
        sys.exit("usage: analyze_r6_mechanism.py <campaign csv or csv.gz> [output directory]")

    opener = gzip.open if sys.argv[1].endswith(".gz") else open
    with opener(sys.argv[1], "rt", newline="") as handle:
        reader = csv.reader(handle)
        head = next(reader)
        assert head == [
            "grammar",
            "op",
            "k",
            "seed",
            "trial",
            "p",
            "failure_offset",
            "corruption_end",
            "first_true",
            "repairable",
            "minimal_repair",
            "exact_at_anchor",
            "strategy",
            "first",
            "first_landed",
            "evidence_begin",
            "evidence_end",
            "evidence_kind",
            "minimal",
            "terminal",
            "terminal_landed",
            "outcome",
            "attempts",
            "moves_covered",
            "moves_covered_landed",
            "converged",
            "lost",
            "spurious",
        ], head
        col = {name: index for index, name in enumerate(head)}
        rows = []
        for row in reader:
            assert len(row) == COLUMN_COUNT, row
            if row[col["strategy"]] != "certified":
                continue
            assert row[col["first"]], row
            failure = int(row[col["failure_offset"]])
            end = int(row[col["corruption_end"]])
            begin = int(row[col["evidence_begin"]])
            width = int(row[col["evidence_end"]]) - begin
            kind = row[col["evidence_kind"]]
            # The kind's domain first, then the searched width range, then the width law. The
            # equivalence alone would wave through an unknown kind on any multi-byte evidence, both of
            # its sides false, and it says nothing at all about a width of zero or five, which the
            # search never produces: a row outside the range would otherwise be retained in the totals
            # and silently dropped from the fixed shape buckets below.
            assert kind in ("byte", "window"), row
            assert 1 <= width <= 4, row
            assert (kind == "byte") == (width == 1), row

            # The coverage identity, asserted per row: covered is exactly travel >= overhang - 1.
            assert (begin >= end) == (begin - (failure + 1) >= (end - failure) - 1), row
            rows.append(
                (
                    end - failure,  # overhang
                    begin >= end,  # covered
                    row[col["first_landed"]] == "1",  # landed
                    kind,
                    width,
                    row[col["op"]],
                    row[col["k"]],
                    row[col["grammar"]].startswith("c-like bare"),
                    begin,
                    begin + width,  # evidence interval
                    end,  # the corruption end
                    int(row[col["p"]]),  # the damage start
                    begin - (failure + 1),  # the evidence travel
                    row[col["grammar"]],
                    abs(int(row[col["first"]]) - int(row[col["first_true"]])) if row[col["first_true"]] else None,
                )
            )

    lines = []
    emit = lines.append

    total = len(rows)
    covered = sum(1 for r in rows if r[1])
    emit(f"certified first answers {total}, covered {covered}, uncovered {total - covered}")

    sheltered = [r for r in rows if r[0] <= 1]
    exposed = [r for r in rows if r[0] >= 2]
    assert len(sheltered) + len(exposed) == total
    assert all(r[1] for r in sheltered), "an uncovered answer inside the sheltered regime"
    emit(f"sheltered (overhang <= 1): {len(sheltered)} answers, all covered")
    emit(
        f"exposed (overhang >= 2): {len(exposed)} answers, "
        f"covered {sum(1 for r in exposed if r[1])} ({100 * sum(1 for r in exposed if r[1]) / len(exposed):.1f}%)"
    )

    emit("overhang bins (bin, answers, covered%, landed%):")
    for label, low, high in BINS:
        members = [r for r in rows if (low is None or r[0] >= low) and (high is None or r[0] <= high)]
        assert members, label
        emit(
            f"  {label}: {len(members)}, "
            f"{100 * sum(1 for r in members if r[1]) / len(members):.1f}%, "
            f"{100 * sum(1 for r in members if r[2]) / len(members):.1f}%"
        )

    emit("overhang bin composition and within-kind coverage " "(bin, window share%, byte covered%, window covered%):")
    for label, low, high in BINS:
        members = [r for r in rows if (low is None or r[0] >= low) and (high is None or r[0] <= high)]
        bytes_ = [r for r in members if r[3] == "byte"]
        windows = [r for r in members if r[3] == "window"]
        byte_rate = f"{100 * sum(1 for r in bytes_ if r[1]) / len(bytes_):.1f}%" if bytes_ else "none"
        window_rate = f"{100 * sum(1 for r in windows if r[1]) / len(windows):.1f}%" if windows else "none"
        emit(f"  {label}: {100 * len(windows) / len(members):.1f}%, {byte_rate}, {window_rate}")

    emit("certificate shape (kind/width, answers, covered%):")
    shape_total = 0
    for kind, width in (("byte", 1), ("window", 2), ("window", 3), ("window", 4)):
        members = [r for r in rows if r[3] == kind and r[4] == width]
        shape_total += len(members)
        emit(f"  {kind}/{width}: {len(members)}, {100 * sum(1 for r in members if r[1]) / len(members):.1f}%")

    # The four buckets partition the certified answers: a row outside them would be counted in the
    # totals above and missing here, which is exactly how a corrupted shape would hide.
    assert shape_total == len(rows), (shape_total, len(rows))

    emit("pooled coverage by operation and by damage size:")
    for field, values in ((5, ("substitute", "insert", "delete")), (6, ("1", "4", "16"))):
        for value in values:
            members = [r for r in rows if r[field] == value]
            emit(f"  {value}: {len(members)}, {100 * sum(1 for r in members if r[1]) / len(members):.1f}%")

    emit("coverage per operation and size cell (op/k, answers, covered%):")
    for op in ("substitute", "insert", "delete"):
        for k in ("1", "4", "16"):
            members = [r for r in rows if r[5] == op and r[6] == k]
            emit(f"  {op}/{k}: {len(members)}, {100 * sum(1 for r in members if r[1]) / len(members):.1f}%")

    uncovered_rows = [r for r in rows if not r[1]]
    emit("uncovered geometry against the corruption end (count):")
    emit(f"  begins before the damage start: {sum(1 for r in uncovered_rows if r[8] < r[11])}")
    emit(f"  ends at or before the corruption end: {sum(1 for r in uncovered_rows if r[9] <= r[10])}")
    emit(f"  straddles the corruption end: {sum(1 for r in uncovered_rows if r[9] > r[10])}")

    # The travel is what the search spends to certify, and by the overhang law it is also what coverage
    # requires: the same quantity read in opposite directions, per row.
    emit(
        "search travel and coverage per row (row, answers, median travel, median overhang, mean travel, "
        "covered%, mean overshoot):"
    )
    labels = sorted({r[13] for r in rows}, key=lambda g: statistics.median([r[12] for r in rows if r[13] == g]))
    for label in labels:
        members = [r for r in rows if r[13] == label]
        overshoots = [r[14] for r in members if r[14] is not None]
        emit(
            f"  {label}: {len(members)}, {statistics.median([r[12] for r in members]):.1f}, "
            f"{statistics.median([r[0] for r in members]):.1f}, "
            f"{statistics.mean([r[12] for r in members]):.1f}, "
            f"{100 * sum(1 for r in members if r[1]) / len(members):.1f}%, "
            f"{statistics.mean(overshoots):.1f}"
        )

    emit("bare row within-row split (kind, answers, uncovered, uncovered%, uncovered landed):")
    for kind in ("byte", "window"):
        members = [r for r in rows if r[7] and r[3] == kind]
        uncovered = [r for r in members if not r[1]]
        emit(
            f"  {kind}: {len(members)}, {len(uncovered)}, "
            f"{100 * len(uncovered) / len(members):.1f}%, {sum(1 for r in uncovered if r[2])}"
        )

    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if len(sys.argv) == 3:
        with open(os.path.join(sys.argv[2], "r6-mechanism.txt"), "w", newline="\n") as handle:
            handle.write(text)

        # The overhang figure's data file, consumed directly by the manuscript's plot so no
        # coordinate is ever transcribed by hand: bin index, covered rate, landed rate.
        with open(os.path.join(sys.argv[2], "r6-overhang.dat"), "w", newline="\n") as handle:
            handle.write("bin covered landed\n")
            for position, (label, low, high) in enumerate(BINS, start=1):
                members = [r for r in rows if (low is None or r[0] >= low) and (high is None or r[0] <= high)]
                handle.write(
                    f"{position} "
                    f"{100 * sum(1 for r in members if r[1]) / len(members):.1f} "
                    f"{100 * sum(1 for r in members if r[2]) / len(members):.1f}\n"
                )


if __name__ == "__main__":
    main()
