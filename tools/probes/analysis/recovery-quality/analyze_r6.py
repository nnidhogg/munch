#!/usr/bin/env python3
# Derives the campaign figures the paper quotes from the r6 archive alone, so the manuscript's numbers are
# the output of a checked-in program rather than a transcription. Reads the campaign CSV, writes three
# files beside it: r6-stats.txt (every named figure, one per line), r6-pooled-table.tex (the pooled table
# body in exactly the manuscript's columns, taken up by input), and r6-landing-figure.csv (per-grammar
# per-arm landing coordinates for the results figure). Figures the harness alone can attest (the pristine
# oracle pass, within-cell repeat counts, per-seed stability) live in the archived summary text, and the
# manuscript cites them from there; this program derives everything that comes from the CSV.
#
# Usage: analyze_r6.py <campaign csv> [output directory]
#
# The program is deterministic, stdlib-only, and asserts the schema it reads: 28 columns, known strategies,
# known outcomes, and every coordinate inside the input its own row's grammar and operation describe. Wilson
# intervals are descriptive conditional-on-draw summaries; the rows draw disjoint schedule and payload streams
# by construction in this revision, and per-seed figures are printed in the archived summary beside the pooled
# ones.

import csv
import math
import os
import sys

sys.dont_write_bytecode = True

from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import commit_r6

COLUMNS = [
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
]

ARMS = [
    "certified",
    "certified-clean",
    "exact",
    "exact-clean",
    "skip-one",
    "newline",
    "newline-at",
    "semicolon",
    "semicolon-at",
    "token-newline",
    "token-semicolon",
]

OUTCOMES = {"completed", "refused", "capped"}

# No input this harness reads approaches sixteen million bytes: the generated corpora are half a mebibyte and
# the real document is under a mebibyte. A coordinate past this bound is not a large campaign, it is a
# corrupted field. This is the outer bound, wide enough to admit coordinates no corpus in the campaign can
# carry; the inner one, derived per row from the source its own grammar runs on, is what refuses those.
POSITION_BOUND = 1 << 24

# Every column holding a byte offset into an input, as opposed to a count or a flag.
POSITION_COLUMNS = (
    "p", "failure_offset", "corruption_end", "first_true", "minimal_repair", "exact_at_anchor", "first",
    "evidence_begin", "evidence_end", "minimal", "terminal", "converged",
)

# The position columns the damaged input's length bounds, which is every one of them but `p`, the damage
# start in the pristine source. Ten are offsets into the damaged input the arms search; `minimal_repair` is
# the returned repair's byte length rather than an offset, and a repair longer than the whole input it
# repairs is a corrupted field like any other.
DAMAGED_LENGTH_COLUMNS = (
    "failure_offset", "corruption_end", "first_true", "minimal_repair", "exact_at_anchor", "first",
    "evidence_begin", "evidence_end", "minimal", "terminal", "converged",
)

# The campaign's six grammar rows, fixed by the schedule, each beside the byte length of the source it runs
# on: the grid audit requires exactly these rows, and every coordinate a row archives is bounded by the input
# that length implies. No column carries the length, so the mapping is pinned here as it was derived. The
# five generated rows run on the 512 KiB corpora archived with the campaign (`corpus-*.bin`, 524,288 bytes
# each); the real-world row runs on the archived `twitter.json`, 631,515 bytes. The row population
# corroborates every entry: all 518,360 archived rows fit inside the lengths below, the closest any
# coordinate comes to the length bounding it is 38 bytes, and the widest damage the schedule drew stops 66
# bytes short of its corpus end.
GRAMMAR_SOURCE_BYTES = {
    "c-like bare: identifiers numbers operators punctuation": 524288,
    "c-like conventional plus block comments alone": 524288,
    "c-like conventional with strings and line comments": 524288,
    "c-like split-friendly with strings and line comments": 524288,
    "json rfc 8259 lexical forms": 524288,
    "json rfc 8259 lexical forms on a real-world document": 631515,
}

GRAMMARS = tuple(GRAMMAR_SOURCE_BYTES)

# Fields an arm may populate only by answering: any zero-attempt row must leave every one of them empty.
ANSWER_FIELDS = (
    "first",
    "first_landed",
    "evidence_begin",
    "evidence_end",
    "evidence_kind",
    "minimal",
    "terminal",
    "terminal_landed",
    "moves_covered",
    "moves_covered_landed",
    "converged",
    "lost",
    "spurious",
)

# Incident-level facts the harness writes identically into every arm's row: damage coordinates and the
# oracle verdicts. A mismatch between arms is corruption, never data.
SHARED_FIELDS = (
    "p",
    "failure_offset",
    "corruption_end",
    "first_true",
    "repairable",
    "minimal_repair",
    "exact_at_anchor",
)

# Every column that ever carries a number. Values must be canonical ASCII integers, str(int(v)) == v, so
# a padded 00 cannot slip past an equality test with 0 and a padded key cannot hide a semantic
# duplicate; every value, the convergence distance included, is nonnegative in this archive.
NUMERIC_COLUMNS = (
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
    "first",
    "first_landed",
    "evidence_begin",
    "evidence_end",
    "minimal",
    "terminal",
    "terminal_landed",
    "attempts",
    "moves_covered",
    "moves_covered_landed",
    "converged",
    "lost",
    "spurious",
)

# The evidence-bearing fields only the certified arms own; any value on another arm is corruption.
CERTIFIED_ONLY = (
    "evidence_begin",
    "evidence_end",
    "evidence_kind",
    "minimal",
    "moves_covered",
    "moves_covered_landed",
)


def wilson(successes, n):
    if n == 0:
        return 0.0, 0.0
    z = 1.959963984540054
    p = successes / n
    denominator = 1.0 + z * z / n
    center = p + z * z / (2.0 * n)
    margin = z * math.sqrt(p * (1.0 - p) / n + z * z / (4.0 * n * n))
    return 100.0 * (center - margin) / denominator, 100.0 * (center + margin) / denominator


def damaged_length(source_size, op, k):
    # The length of the input a row's arms search, which the damage its operation applied to the source
    # decides: a substitution rewrites k bytes in place and leaves the length alone, a deletion removes k of
    # them, and an insertion adds k.
    if op == "delete":
        return source_size - k
    if op == "insert":
        return source_size + k
    return source_size


def quantile(values, q):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = q * (len(ordered) - 1)
    low = int(math.floor(index))
    high = int(math.ceil(index))
    if low == high:
        return float(ordered[low])
    return ordered[low] + (ordered[high] - ordered[low]) * (index - low)


def main():
    # The audit is the assertions: under -O or PYTHONOPTIMIZE they are stripped and every check
    # silently vanishes, so an optimized interpreter is refused outright.
    if sys.flags.optimize:
        sys.exit("analyze_r6.py: refusing to run with assertions disabled (-O/PYTHONOPTIMIZE)")
    if len(sys.argv) < 2:
        sys.exit("usage: analyze_r6.py <campaign csv> [output directory]")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "."

    trials = {}  # (grammar, op, k, seed, trial) -> shared trial fields
    incidents = defaultdict(dict)  # same key -> arm -> row
    absorbed = 0
    absorbed_keys = set()
    region_rows = 0
    region_tight = 0
    landing_cross = 0
    landing_within = 0
    divergence_rechecks = 0
    cell_rows = defaultdict(int)
    cell_first_present = defaultdict(int)
    cell_attempts_positive = defaultdict(int)
    cell_refused_zero_attempts = defaultdict(int)
    cell_refused = defaultdict(int)
    cell_capped = defaultdict(int)
    collapsed_incidents = 0
    collapsed_moves = defaultdict(list)

    with open(sys.argv[1], newline="") as handle:
        reader = csv.reader(handle)
        head = next(reader)
        assert head == COLUMNS, head
        for row in reader:
            assert len(row) == len(COLUMNS), row
            record = dict(zip(COLUMNS, row))

            # Canonical integers everywhere a number can appear, before any key is built or any value
            # compared: padded digits would otherwise defeat equality tests and duplicate detection, and
            # every count and position is nonnegative, the signed convergence distance the one exception.
            for field in NUMERIC_COLUMNS:
                value = record[field]
                if value:
                    assert value.isdigit() and value == str(int(value)), (field, value)
                    if field in POSITION_COLUMNS:
                        assert int(value) < POSITION_BOUND, (field, value)
            key = (record["grammar"], record["op"], record["k"], record["seed"], record["trial"])

            # Per-cell tallies for the harness reconciliation below: what this CSV says each
            # (grammar, op, k, arm) cell holds, counted before any row can be filtered, grouped, or
            # absorbed out of a later comparison. Three facts per row feed the three summary columns:
            # whether it carries a first answer, whether it spent attempts, and whether it refused,
            # split by whether the refusal ever attempted.
            cell = (record["grammar"], record["op"], record["k"], record["strategy"])
            cell_rows[cell] += 1
            if record["first"]:
                cell_first_present[cell] += 1
            if record["attempts"] and int(record["attempts"]) > 0:
                cell_attempts_positive[cell] += 1
            if record["outcome"] == "refused":
                cell_refused[cell] += 1
                if not record["attempts"] or int(record["attempts"]) == 0:
                    cell_refused_zero_attempts[cell] += 1
            if record["outcome"] == "capped":
                cell_capped[cell] += 1
            # The damage geometry belongs to the draw, so it is checked before the absorbed rows leave:
            # deletion leaves a seam, insertion and substitution a k-byte span.
            if record["op"] == "delete":
                assert record["corruption_end"] == record["p"], (key, record["strategy"])
            else:
                assert int(record["corruption_end"]) == int(record["p"]) + int(record["k"]), (key, record["strategy"])

            # Every coordinate is bounded by the input it indexes, and that input is the source this row's own
            # grammar runs on rather than the generic bound above: a corpus of half a mebibyte carries no
            # coordinate at six hundred thousand, though that bound admits one. The damage start is the one
            # column measured against the pristine source; the rest are measured against the damaged input,
            # whose length the operation fixes. Like the geometry above, this belongs to the draw, so it is
            # read before the absorbed rows leave.
            assert record["grammar"] in GRAMMAR_SOURCE_BYTES, record["grammar"]
            source_size = GRAMMAR_SOURCE_BYTES[record["grammar"]]
            damaged_size = damaged_length(source_size, record["op"], int(record["k"]))
            if record["op"] == "insert":
                # An insertion consumes nothing, so its start is a seam and may be the source's end itself.
                assert int(record["p"]) <= source_size, (key, record["strategy"], record["p"])
            else:
                # A substitution and a deletion consume k bytes at the start, so the whole span lies inside.
                assert int(record["p"]) + int(record["k"]) <= source_size, (key, record["strategy"], record["p"])
            for field in DAMAGED_LENGTH_COLUMNS:
                if record[field]:
                    assert int(record[field]) <= damaged_size, (key, record["strategy"], field, record[field])
            if record["strategy"] == "absorbed":
                # One absorbed row per absorbed draw, before any duplicate can shadow the count, with
                # only the damage coordinates populated: everything else on an absorbed row is empty.
                assert key not in absorbed_keys, key
                absorbed_keys.add(key)
                assert record["p"] and record["corruption_end"], key
                for field in COLUMNS:
                    if field not in ("grammar", "op", "k", "seed", "trial", "strategy", "p", "corruption_end"):
                        assert not record[field], (key, field)
                absorbed += 1
                continue
            assert record["strategy"] in ARMS, record["strategy"]
            assert record["outcome"] in OUTCOMES, record["outcome"]
            assert record["attempts"].isdigit(), (key, record["strategy"])

            # The driver's attempt budget: no incident exceeds one hundred attempts, and a capped
            # outcome means exactly the budget was spent.
            assert int(record["attempts"]) <= 100, (key, record["strategy"])
            assert record["outcome"] != "capped" or record["attempts"] == "100", (key, record["strategy"])
            if record["attempts"] == "0":
                # A zero-attempt row is a refusal that never proposed, whichever arm it belongs to:
                # no answer, evidence, terminal, coverage, or divergence field may carry a value.
                assert record["outcome"] == "refused", (key, record["strategy"])
                for field in ANSWER_FIELDS:
                    assert not record[field], (key, record["strategy"], field)

            # Domains and dependent fields: landing flags are binary and exist exactly with their answers,
            # answers exist exactly with attempts, the divergence fields belong to completed incidents
            # alone, evidence belongs to the certified arms alone, the oracle's verdict fields rise and
            # fall together, and the certified covered tallies obey landed == covered <= attempts, the
            # harness's own runtime assertion. Every relation here was derived from the archived rows
            # before it was asserted.
            # Damaging incidents always carry the oracle's mapped boundary.
            assert record["first_true"], (key, record["strategy"])
            assert int(record["first_true"]) >= int(record["corruption_end"]), (key, record["strategy"])
            assert record["repairable"] in ("0", "1"), (key, record["strategy"])
            assert (record["repairable"] == "1") == bool(record["minimal_repair"]) == bool(record["exact_at_anchor"]), (
                key,
                record["strategy"],
            )
            # A landing flag exists exactly where the harness computes one: for an answer strictly
            # inside the damaged input. At the input's very end there is nothing to land on and the
            # harness writes no flag, so an answer there carrying a flag is corruption. No archived
            # answer sits at the end; the domain is stated in full so a row claiming one is refused at
            # the flag it cannot have rather than accepted through the shorter answers-iff-flags rule.
            for flag, anchor_field in (("first_landed", "first"), ("terminal_landed", "terminal")):
                assert record[flag] in ("", "0", "1"), (key, record["strategy"], flag)
                if not record[anchor_field] or int(record[anchor_field]) == damaged_size:
                    assert record[flag] == "", (key, record["strategy"], flag)
                else:
                    assert record[flag] in ("0", "1"), (key, record["strategy"], flag)
            answered = record["attempts"] != "0"
            assert answered == bool(record["first"]) == bool(record["terminal"]), (key, record["strategy"])

            # The decider arms answer on every incident of this campaign, the anchored one by searching
            # past a refusing anchor and the oracle-floored one from covered ground; the manuscript
            # reports both at the full incident count, so a decider row without an answer is corruption.
            if record["strategy"] in ("exact", "exact-clean"):
                assert answered, (key, record["strategy"])

            # The skip arm's resume is always its own start, so it can complete or exhaust the budget
            # but never lacks a position to propose: a refused skip row describes a run that cannot
            # have happened. Its first answer is that start, one past the failure, by the same fact;
            # skipping one byte is the arm's whole definition, so an answered skip row starting
            # anywhere else is corruption.
            if record["strategy"] == "skip-one":
                assert record["outcome"] != "refused", (key, record["strategy"])
                if answered:
                    assert int(record["first"]) == int(record["failure_offset"]) + 1, \
                        (key, record["strategy"], record["first"])

            # A terminal answer at the damaged input's very end means the search ran out of input, and
            # the harness closes exactly that state as completed before any other outcome can be
            # written: a refused or capped row ending there describes a run that cannot have happened.
            if answered and int(record["terminal"]) == damaged_size:
                assert record["outcome"] == "completed", (key, record["strategy"], record["outcome"])
            if answered:
                # Every answer respects its arm's search floor: one past the failure for the blind
                # arms, the corruption end besides for the oracle-floored ones. The terminal answer is
                # an answer too, so it obeys the floor and never precedes the first; a single attempt
                # means the two coincide, position and landing flag alike, and more than one advances.
                floor = int(record["failure_offset"]) + 1
                if record["strategy"] in ("certified-clean", "exact-clean"):
                    floor = max(floor, int(record["corruption_end"]))
                assert int(record["first"]) >= floor, (key, record["strategy"])
                assert int(record["terminal"]) >= floor, (key, record["strategy"])
                assert int(record["terminal"]) >= int(record["first"]), (key, record["strategy"])
                if record["attempts"] == "1":
                    assert record["terminal"] == record["first"], (key, record["strategy"])
                    assert record["terminal_landed"] == record["first_landed"], (key, record["strategy"])
                else:
                    assert int(record["terminal"]) > int(record["first"]), (key, record["strategy"])
                    # Each attempt past the first advances the answer by at least one byte, so the
                    # distance between the first and terminal answers bounds the attempts from below.
                    assert int(record["terminal"]) - int(record["first"]) >= int(record["attempts"]) - 1, (
                        key,
                        record["strategy"],
                    )
                # The oracle maps no pristine boundary into the damaged window: a substitution's images
                # are the identity outside it and boundaries inside it are dropped, and an insertion's
                # images jump the inserted span entirely. An answer inside the window therefore cannot
                # land, and a landed flag there is corruption. Deletion leaves no window, its end being
                # the seam itself.
                if record["op"] in ("substitute", "insert"):
                    for field, flag in (("first", "first_landed"), ("terminal", "terminal_landed")):
                        if int(record["p"]) <= int(record[field]) < int(record["corruption_end"]):
                            assert record[flag] == "0", (key, record["strategy"], field)

                # A landed answer at or past the corruption end sits on a mapped boundary, so the first
                # mapped boundary of that region cannot lie beyond it; the terminal answer is an answer
                # too, so a landed covered terminal bounds that boundary the same way.
                if record["first_landed"] == "1" and int(record["first"]) >= int(record["corruption_end"]):
                    assert int(record["first_true"]) <= int(record["first"]), (key, record["strategy"])
                if record["terminal_landed"] == "1" and int(record["terminal"]) >= int(record["corruption_end"]):
                    assert int(record["first_true"]) <= int(record["terminal"]), (key, record["strategy"])
                # An answer sitting exactly on the oracle's first mapped boundary is on a boundary of the
                # pristine mapping by that very fact, so its landing flag cannot be clear.
                for field, flag in (("first", "first_landed"), ("terminal", "terminal_landed")):
                    if int(record[field]) == int(record["first_true"]):
                        assert record[flag] == "1", (key, record["strategy"], field)
            # The decider's direct answer is a position too, and it searches from the blind anchor.
            if record["exact_at_anchor"]:
                assert int(record["exact_at_anchor"]) >= int(record["failure_offset"]) + 1, (key, record["strategy"])
            # The oracle-floored decider searches covered ground, so every answer it gives lands, first
            # and last, on every incident it answers, the beyond-repair ones included.
            if record["strategy"] == "exact-clean" and answered:
                assert record["first_landed"] == "1" and record["terminal_landed"] == "1", \
                    (key, record["strategy"])
            # A refusal never spends the whole budget: exhausting it is what capped means.
            assert not (record["outcome"] == "refused" and record["attempts"] == "100"), (key, record["strategy"])
            completed = record["outcome"] == "completed"
            for field in ("converged", "lost", "spurious"):
                assert bool(record[field]) == completed, (key, record["strategy"], field)

            # A completed incident's convergence point lies at or past the answer it started from, and
            # where it does not reach past the corruption end the divergence region is empty, so neither
            # a lost nor a spurious boundary can be counted in it. Completion is a property of the row
            # alone, so this binds every completed row, the beyond-repair incidents included; it is read
            # after the dependency above, so a row carrying a convergence point it has no right to is
            # refused as that contradiction rather than as a broken order.
            if completed:
                assert int(record["converged"]) >= int(record["first"]), (key, record["strategy"])
                if int(record["converged"]) <= int(record["corruption_end"]):
                    assert record["lost"] == "0" and record["spurious"] == "0", \
                        (key, record["strategy"])

                # Lost and spurious boundaries are disjoint byte positions inside the divergence
                # region, so their sum is bounded by the region's width; the archive realizes the
                # bound exactly on some rows, so it is tight, and a count past it names positions the
                # region does not have.
                region = max(0, int(record["converged"]) - int(record["corruption_end"]))
                assert int(record["lost"]) + int(record["spurious"]) <= region, \
                    (key, record["strategy"], region)
                if region > 0:
                    region_rows += 1
                    if int(record["lost"]) + int(record["spurious"]) == region:
                        region_tight += 1
            if record["strategy"] in ("certified", "certified-clean"):
                for field in ("evidence_begin", "evidence_end", "evidence_kind", "minimal", "moves_covered"):
                    assert bool(record[field]) == answered, (key, record["strategy"], field)
                if answered:
                    assert record["evidence_kind"] in ("byte", "window"), (key, record["strategy"])
                    width = int(record["evidence_end"]) - int(record["evidence_begin"])
                    assert (record["evidence_kind"] == "byte") == (width == 1), (key, record["strategy"])
                    assert 1 <= width <= 4, (key, record["strategy"], width)

                    # The walk's documented floor: no evidence begins before the blind anchor, the fact
                    # the overhang law rests on. The clean walk's floor is at least the blind one.
                    assert int(record["evidence_begin"]) >= int(record["failure_offset"]) + 1, (key, record["strategy"])

                    # The clean walk floors its search at the corruption end besides, so its evidence
                    # is covered by construction; an uncovered clean answer is corruption.
                    if record["strategy"] == "certified-clean":
                        assert int(record["evidence_begin"]) >= int(record["corruption_end"]), key

                    # The minimal answerable position obeys the same floor as the search that found it.
                    floor = int(record["failure_offset"]) + 1
                    if record["strategy"] == "certified-clean":
                        floor = max(floor, int(record["corruption_end"]))
                    assert int(record["minimal"]) >= floor, (key, record["strategy"])

                    # The walk answers with the first certificate in evidence order, so the minimal
                    # answerable position never lies past the answer taken; the nonminimality figure
                    # counts exactly the gap between them.
                    assert int(record["minimal"]) <= int(record["first"]), (key, record["strategy"])
                assert record["moves_covered_landed"] == record["moves_covered"], (key, record["strategy"])
                if record["moves_covered"]:
                    assert int(record["moves_covered"]) <= int(record["attempts"]), (key, record["strategy"])
            else:
                for field in CERTIFIED_ONLY:
                    assert not record[field], (key, record["strategy"], field)

            # One row per (incident, arm): a duplicate would silently shadow its predecessor in a plain
            # dictionary write, so it is rejected instead.
            assert record["strategy"] not in incidents[key], (key, record["strategy"])
            trials[key] = record
            incidents[key][record["strategy"]] = record

    grammars = sorted({key[0] for key in trials})
    damaging = len(trials)

    # An absorbed draw and a damaging incident are exclusive outcomes of one (row, op, k, seed, trial)
    # cell: a key carrying both is corruption.
    assert absorbed_keys.isdisjoint(incidents.keys()), sorted(absorbed_keys & incidents.keys())[:3]

    # The schedule is a full grid: every (grammar, op, k, seed) cell carries the same contiguous block of
    # trials, each trial absorbed or damaging, so a silently deleted draw or incident breaks the grid.
    cells = defaultdict(set)
    for grammar, op, k, seed, trial in list(absorbed_keys) + list(incidents.keys()):
        cells[(grammar, op, k, seed)].add(int(trial))

    # The schedule is the exact Cartesian product: every observed grammar crosses the three
    # operations, the three damage sizes, and the three seeds, so a silently deleted cell, draw, or
    # incident breaks the grid; trials are contiguous from zero and equal across cells.
    assert sorted({cell[0] for cell in cells}) == sorted(GRAMMARS), sorted({cell[0] for cell in cells})
    expected_cells = {
        (g, op, k, s)
        for g in GRAMMARS
        for op in ("substitute", "insert", "delete")
        for k in ("1", "4", "16")
        for s in ("0", "1", "2")
    }
    assert set(cells) == expected_cells, sorted(set(cells) ^ expected_cells)[:3]
    cell_sizes = {len(cell_trials) for cell_trials in cells.values()}
    assert cell_sizes == {500}, sorted(cell_sizes)
    per_cell = 500
    for cell, cell_trials in cells.items():
        assert cell_trials == set(range(per_cell)), (cell, per_cell, len(cell_trials))
    for key, arms in incidents.items():
        assert len(arms) == len(ARMS), (key, sorted(arms))
        # The archived direct call is the same query the exact arm makes at its first move, so where the
        # anchor query answered they agree. That the arm answered at all is the row-level decider rule
        # above, so existence needs no second assertion here; what is left to hold is the equality.
        direct, exact_first = arms["exact"]["exact_at_anchor"], arms["exact"]["first"]
        if direct:
            assert direct == exact_first, (key, direct, exact_first)
        else:
            # The complementary branch, so the law covers both of the query's outcomes: the arm starts
            # at the blind floor, one past the failure, and the archived direct query is that floor's
            # own anchored question, so an absent direct answer means the floor refused and the arm's
            # first answer must sit strictly past it. Derived before it was asserted: every
            # direct-absent exact row in the archive advances, by one byte up to four hundred eighty.
            assert int(exact_first) > int(arms["exact"]["failure_offset"]) + 1, (key, exact_first)
        base = arms[ARMS[0]]
        for record in arms.values():
            for field in SHARED_FIELDS:
                assert record[field] == base[field], (key, field)

        # Each delimiter convention runs in two placements over the same input, paired delimiter
        # searches over one incident rather than one search reported twice, since the placements can
        # part ways late: pairs exist whose two placements end in different outcomes. What the pairing
        # does bind, derived before it was asserted, is the start: they answer together or refuse
        # together, and an at-placement answer sits exactly one byte below its past-placement partner.
        # The terminal is looser by exactly one case: the at-placement can fit one further attempt in
        # before a shared refusal point, and then the two terminals coincide instead of differing by one.
        for past_name, at_name in (("newline", "newline-at"), ("semicolon", "semicolon-at")):
            past, at = arms[past_name], arms[at_name]
            assert bool(past["first"]) == bool(at["first"]), (key, past_name)
            if past["first"]:
                assert int(at["first"]) == int(past["first"]) - 1, (key, past_name, at["first"])
                assert int(at["terminal"]) in (int(past["terminal"]) - 1, int(past["terminal"])), \
                    (key, past_name, at["terminal"])

                # The paired searches spend the same attempts, plus exactly the one further attempt
                # the at placement fits in whenever the two terminals coincide; anything else is a
                # pair whose rows describe different searches.
                extra = 1 if at["terminal"] == past["terminal"] else 0
                assert int(at["attempts"]) == int(past["attempts"]) + extra, \
                    (key, past_name, at["attempts"])

    # The move sidecar, when archived beside the CSV, must agree with the per-incident aggregates: every
    # certified move row is re-tallied against the incident's moves_covered count, fail-closed.
    # The sidecar is part of the archive's contract, not an optional extra: its absence fails the
    # analysis rather than silently narrowing the audit.
    sidecar = sys.argv[1] + ".moves.csv"
    assert os.path.exists(sidecar), sidecar
    if True:
        move_covered = defaultdict(int)
        move_last = {}
        move_first = {}
        move_last_begin = {}
        move_prev = {}
        move_rows = 0
        with open(sidecar, newline="") as handle:
            reader = csv.reader(handle)
            head = next(reader)
            assert head == [
                "grammar",
                "op",
                "k",
                "seed",
                "trial",
                "strategy",
                "move",
                "answer",
                "evidence_begin",
                "evidence_end",
            ], head
            for row in reader:
                assert len(row) == 10, row
                move_rows += 1
                key = (row[0], row[1], row[2], row[3], row[4])
                arm = row[5]
                assert arm in ("certified", "certified-clean"), row
                record = incidents[key][arm]

                # Canonical nonnegative integers in every numeric sidecar field, the same discipline
                # as the campaign columns: a padded or signed spelling is corruption.
                for field in row[6:10]:
                    assert field.isdigit() and field == str(int(field)), (row[:6], field)
                index = int(row[6])
                answer = int(row[7])
                begin = int(row[8])
                end = int(row[9])

                # Contiguous unique numbering, evidence widths within the searched lengths, the interval
                # ending at or after its answer's certificate shape, and strictly advancing moves.
                assert index == move_prev.get((key, arm), -1) + 1, (key, arm, index)
                move_prev[(key, arm)] = index
                assert 1 <= end - begin <= 4, row

                # A move's coordinates index the damaged input exactly as the campaign columns do, so
                # they are held to the same length: an interval ending past the input is a place the
                # walk cannot have read. The bound lives here as well as on the campaign row because
                # this file is a separate archive with its own coordinates.
                move_damaged_size = damaged_length(
                    GRAMMAR_SOURCE_BYTES[record["grammar"]], record["op"], int(record["k"])
                )
                assert end <= move_damaged_size, (row, move_damaged_size)

                # A certified answer lies inside its evidence interval: at the byte itself, or at the
                # occurrence plus an origin strictly inside the window.
                assert begin <= answer < end, row
                if index > 0:
                    # A later move's evidence begins past the position the previous move resumed at,
                    # the reconstructible floor for a search restarting one past its predecessor.
                    assert begin > move_last[(key, arm)], (key, arm, index)
                if index == 0:
                    move_first[(key, arm)] = (answer, begin, end)

                    # A covered first move lands, the harness's runtime assertion: its archived flag
                    # cannot disagree.
                    if begin >= int(record["corruption_end"]):
                        assert record["first_landed"] == "1", (key, arm)
                else:
                    assert answer > move_last[(key, arm)], (key, arm)
                move_last[(key, arm)] = answer
                move_last_begin[(key, arm)] = begin

                # Every clean-walk move's evidence sits at or past the corruption end, the floor the
                # arm searches under; a single uncovered clean move is corruption.
                if arm == "certified-clean":
                    assert begin >= int(record["corruption_end"]), (key, index)
                if begin >= int(record["corruption_end"]):
                    move_covered[(key, arm)] += 1

                # Where the two search floors collapse, the certified pair's ordered moves are compared
                # whole below; only those incidents are collected, so the memory stays bounded.
                if int(record["failure_offset"]) + 1 >= int(record["corruption_end"]):
                    collapsed_moves[(key, arm)].append((index, answer, begin, end))
        for key, arms in incidents.items():
            for arm in ("certified", "certified-clean"):
                record = arms[arm]

                # Every incident's sidecar rows reconcile with the archived aggregates: one row per
                # attempt, the first row joining the incident's first answer and evidence interval, the
                # last joining its terminal answer, and the covered tally equal to the archived count. The
                # covered-landed aggregate is asserted at runtime by the harness and equals the covered
                # count in this archive; landing itself needs the mapped oracle and is not recomputable
                # from the sidecar alone.
                expected_moves = int(record["attempts"]) if record["attempts"] else 0
                assert move_prev.get((key, arm), -1) + 1 == expected_moves, (key, arm)
                if expected_moves > 0:
                    first_answer, first_begin, first_end = move_first[(key, arm)]
                    assert record["first"] and int(record["first"]) == first_answer, (key, arm)
                    assert int(record["evidence_begin"]) == first_begin, (key, arm)
                    assert int(record["evidence_end"]) == first_end, (key, arm)
                    assert record["terminal"] and int(record["terminal"]) == move_last[(key, arm)], (key, arm)
                    if move_last_begin[(key, arm)] >= int(record["corruption_end"]):
                        assert record["terminal_landed"] == "1", (key, arm)
                else:
                    # Zero archived attempts must mean zero answers: an incident with no sidecar rows
                    # cannot carry a first answer or evidence interval.
                    assert not record["first"], (key, arm)
                    assert not record["evidence_begin"] and not record["evidence_end"], (key, arm)
                expected = int(record["moves_covered"]) if record["moves_covered"] else 0
                assert move_covered.get((key, arm), 0) == expected, (key, arm)

    # The consistency reconciliations run in their own pass, after every row-level and sidecar-level
    # guard above, so a corruption those guards already pin keeps its own refusal and these object
    # only to what nothing narrower caught. What each establishes is agreement among duplicated
    # archive fields, not an independent recomputation of the fact they duplicate.
    for key, arms in incidents.items():
        # Landing is a fact about a coordinate, not about the arm or placement that reached it:
        # within one incident every answered placement at a given coordinate carries the same landing
        # flag, first and terminal placements alike. Every arm that has placed at a coordinate is
        # kept, so a recheck is within-arm exactly when this very arm has already placed there and
        # cross-arm when only other arms have: the split is a fact about the recheck itself and does
        # not depend on the order the arms are read in.
        landings = {}
        for name, record in arms.items():
            for coordinate, flag in ((record["first"], record["first_landed"]),
                                     (record["terminal"], record["terminal_landed"])):
                if coordinate:
                    if coordinate in landings:
                        seen_flag, seen_arms = landings[coordinate]
                        if name in seen_arms:
                            landing_within += 1
                        else:
                            landing_cross += 1
                        assert seen_flag == flag, (key, coordinate, flag)
                        seen_arms.add(name)
                    else:
                        landings[coordinate] = (flag, {name})

        # Sharing is decided before outcomes are read: the arms are grouped by their first answer,
        # and a group containing a completed single-attempt member must be completed single-attempt
        # throughout, with one divergence verdict, because every member resumes the identical
        # deterministic scan from the same coordinate. Filtering on outcome first would let a
        # corrupted row opt out of the very comparison meant to validate it, which is exactly the
        # accepted relabeling this guard exists against. Multi-attempt groups stay exempt by
        # derivation: their later moves are their own.
        by_first = defaultdict(list)
        for name, record in arms.items():
            if record["first"]:
                by_first[record["first"]].append((name, record))
        for coordinate, group in by_first.items():
            if not any(r["outcome"] == "completed" and r["attempts"] == "1" for _, r in group):
                continue
            triples = set()
            for name, r in group:
                assert r["outcome"] == "completed" and r["attempts"] == "1", (key, name, coordinate)
                triples.add((r["converged"], r["lost"], r["spurious"]))
            divergence_rechecks += len(group) - 1
            assert len(triples) == 1, (key, coordinate, sorted(triples))

        # Where the damage collapses the two search floors, one past the failure at or past the
        # corruption end, an oracle-floored arm and its blind twin search identical ground, so the
        # exact pair and the certified pair must agree on their entire run records, and the certified
        # pair on the ordered move sidecar as well. Derived exhaustively before assertion; the
        # incident count is emitted below.
        base = arms["certified"]
        if int(base["failure_offset"]) + 1 >= int(base["corruption_end"]):
            collapsed_incidents += 1
            for one, twin in (("exact", "exact-clean"), ("certified", "certified-clean")):
                for field in ANSWER_FIELDS + ("outcome", "attempts"):
                    assert arms[one][field] == arms[twin][field], (key, one, field)
            # In this archive every collapsed run is single-attempt, so this ordered comparison is
            # implied by the field identities above plus the sidecar join guards; it is asserted all
            # the same, because the law is about runs, not about this archive's shape of them.
            assert collapsed_moves.get((key, "certified"), []) == \
                collapsed_moves.get((key, "certified-clean"), []), (key, "certified sidecar order")

    # Per (grammar, arm) pooled aggregates.
    pooled = defaultdict(lambda: defaultdict(list))
    counts = defaultdict(lambda: defaultdict(int))
    for key, arms in incidents.items():
        grammar = key[0]
        for arm, record in arms.items():
            cell = counts[(grammar, arm)]
            cell["trials"] += 1
            if record["first"]:
                cell["answers"] += 1
                if record["first_landed"] == "1":
                    cell["first_landings"] += 1
                if record["first_true"]:
                    pooled[(grammar, arm)]["overshoot"].append(int(record["first"]) - int(record["first_true"]))
            else:
                cell["refusals"] += 1
            if record["terminal_landed"]:
                cell["terminal_interior"] += 1
                if record["terminal_landed"] == "1":
                    cell["terminal_landings"] += 1
            if record["outcome"] == "completed":
                cell["completions"] += 1
                distance = int(record["converged"]) - int(record["corruption_end"])
                pooled[(grammar, arm)]["conv"].append(distance)
                pooled[(grammar, arm)]["lost"].append(int(record["lost"]))
                pooled[(grammar, arm)]["spurious"].append(int(record["spurious"]))
            elif record["outcome"] == "capped":
                cell["capped"] += 1
            elif record["outcome"] == "refused":
                cell["terminal_refused"] += 1
            pooled[(grammar, arm)]["attempts"].append(int(record["attempts"]))

    # Cross-arm trial-level facts.
    repairable = sum(1 for r in trials.values() if r["repairable"] == "1")
    unrepairable = damaging - repairable
    vacuous_walk = 0
    collapse_total = 0
    collapse_covered = 0
    collapse_uncovered_landed = 0
    undetermined = 0
    vacuous_walk_covered = 0
    vacuous_walk_covered_landed = 0
    walk_answers_total = 0
    moves_total = 0
    moves_covered = 0
    moves_covered_landed = 0
    direct_answers = 0
    direct_saved_repairable = 0
    exact_saved_repairable = 0
    exact_net = 0
    exact_pairs = 0
    clean_blind_differ = 0
    clean_blind_pairs = 0
    for key, arms in incidents.items():
        walk = arms["certified"]
        exact = arms["exact"]
        clean = arms["certified-clean"]
        if walk["first"]:
            walk_answers_total += 1
            moves_total += int(walk["attempts"])
            if walk["moves_covered"]:
                moves_covered += int(walk["moves_covered"])
                moves_covered_landed += int(walk["moves_covered_landed"])
        if walk["exact_at_anchor"]:
            direct_answers += 1
            if walk["repairable"] == "1" and walk["first"]:
                delta = int(walk["first"]) - int(walk["exact_at_anchor"])
                assert delta >= 0, key
                direct_saved_repairable += delta
        if walk["first"] and walk["repairable"] == "0":
            vacuous_walk += 1
            one_attempt = walk["outcome"] == "completed" and walk["attempts"] == "1"
            covered_here = int(walk["evidence_begin"]) >= int(walk["corruption_end"])

            # The proposition's anchor premise, asserted per applied trial: the answer's evidence begins
            # at or after the blind search anchor, one past the failure offset.
            assert int(walk["evidence_begin"]) >= int(walk["failure_offset"]) + 1, key
            if one_attempt:
                collapse_total += 1
                if covered_here:
                    collapse_covered += 1
                elif walk["first_landed"] == "1":
                    collapse_uncovered_landed += 1
            else:
                undetermined += 1

                # The collapse premise fails closed: a covered unrepairable answer whose incident took
                # more than one attempt would contradict the manuscript's applied stratification.
                assert not covered_here, key
            if covered_here:
                vacuous_walk_covered += 1
                if walk["first_landed"] == "1":
                    vacuous_walk_covered_landed += 1
        if walk["first"] and exact["first"]:
            exact_pairs += 1
            delta = int(walk["first"]) - int(exact["first"])
            exact_net += delta
            if walk["repairable"] == "1":
                assert delta >= 0, key
                exact_saved_repairable += delta
        if walk["first"] and clean["first"]:
            clean_blind_pairs += 1
            if walk["first"] != clean["first"]:
                clean_blind_differ += 1

    evidence_covered = 0
    evidence_uncovered = 0
    evidence_uncovered_landed = 0
    nonminimal = 0
    nonminimal_bytes = 0
    coverage = defaultdict(lambda: defaultdict(int))
    lags = []
    failure_before_end = 0
    for record in (arms["certified"] for arms in incidents.values()):
        lags.append(int(record["failure_offset"]) - int(record["p"]))
        if int(record["failure_offset"]) < int(record["corruption_end"]):
            failure_before_end += 1
        if not record["first"]:
            continue
        row_cover = coverage[record["grammar"]]
        row_cover["answers"] += 1
        if record["evidence_kind"] == "byte":
            row_cover["byte_evidence"] += 1
        if int(record["evidence_begin"]) >= int(record["corruption_end"]):
            evidence_covered += 1
            row_cover["covered"] += 1
        else:
            evidence_uncovered += 1
            row_cover["uncovered"] += 1
            if record["first_landed"] == "1":
                evidence_uncovered_landed += 1
                row_cover["uncovered_landed"] += 1
        if record["minimal"] and int(record["minimal"]) < int(record["first"]):
            nonminimal += 1
            nonminimal_bytes += int(record["first"]) - int(record["minimal"])

    lines = []

    def emit(name, value):
        lines.append(f"{name} = {value}")

    emit("damaging-trials", damaging)
    emit("absorbed-trials", absorbed)
    emit("grammar-rows", len(grammars))
    emit("repairable-at-blind-anchor", repairable)
    emit("unrepairable-at-blind-anchor", unrepairable)
    emit("walk-answers-total", walk_answers_total)
    emit("walk-answers-on-unrepairable", vacuous_walk)
    if unrepairable:
        emit("walk-answered-share-of-unrepairable-percent", f"{100.0 * vacuous_walk / unrepairable:.1f}")
    if walk_answers_total:
        emit("walk-unrepairable-share-of-answers-percent", f"{100.0 * vacuous_walk / walk_answers_total:.1f}")
    emit("walk-on-unrepairable-evidence-covered", vacuous_walk_covered)
    emit("walk-on-unrepairable-covered-landed", vacuous_walk_covered_landed)
    emit("collapse-applies-one-attempt-completed", collapse_total)
    emit("collapse-covered-clean-anchor-transfers", collapse_covered)
    emit("collapse-uncovered", collapse_total - collapse_covered)
    emit("collapse-uncovered-landed", collapse_uncovered_landed)
    emit("blind-anchor-undetermined-multi-attempt", undetermined)
    emit("sidecar-move-rows-audited", move_rows)
    emit("certified-moves-total", moves_total)
    emit("certified-moves-covered", moves_covered)
    emit("certified-moves-covered-landed", moves_covered_landed)
    emit("decider-direct-answers-at-blind-anchor", direct_answers)
    emit("decider-direct-saved-bytes-repairable", direct_saved_repairable)
    emit("evidence-covered-answers", evidence_covered)
    emit("evidence-uncovered-answers", evidence_uncovered)
    emit("evidence-uncovered-landed", evidence_uncovered_landed)
    emit("nonminimal-answers", nonminimal)
    emit("nonminimal-extra-bytes", nonminimal_bytes)
    emit("exact-paired-answers", exact_pairs)
    emit("exact-saved-bytes-repairable", exact_saved_repairable)
    emit("exact-net-displacement-bytes", exact_net)
    emit("clean-blind-paired-answers", clean_blind_pairs)
    emit("clean-blind-differing-first-positions", clean_blind_differ)
    emit("lag-median", f"{quantile(lags, 0.5):.0f}")
    emit("lag-p99", f"{quantile(lags, 0.99):.0f}")
    emit("lag-min", min(lags) if lags else 0)
    emit("lag-max", max(lags) if lags else 0)
    emit("failure-before-corruption-end", failure_before_end)
    for arm in ARMS:
        answers = sum(counts[(g, arm)]["answers"] for g in grammars)
        landings = sum(counts[(g, arm)]["first_landings"] for g in grammars)
        completions = sum(counts[(g, arm)]["completions"] for g in grammars)
        trial_count = sum(counts[(g, arm)]["trials"] for g in grammars)
        capped = sum(counts[(g, arm)]["capped"] for g in grammars)
        attempts = [v for g in grammars for v in pooled[(g, arm)]["attempts"]]
        conv = [v for g in grammars for v in pooled[(g, arm)]["conv"]]
        low, high = wilson(landings, answers)
        emit(f"pooled | {arm} | answers", answers)
        emit(f"pooled | {arm} | refusals", trial_count - answers)
        if answers:
            emit(f"pooled | {arm} | first-landing-percent", f"{100.0 * landings / answers:.1f}")
            emit(f"pooled | {arm} | first-landing-wilson", f"[{low:.1f}, {high:.1f}]")
        terminal_refused = sum(counts[(g, arm)]["terminal_refused"] for g in grammars)
        emit(f"pooled | {arm} | terminal-refused", terminal_refused)
        emit(f"pooled | {arm} | completion-percent", f"{100.0 * completions / trial_count:.1f}")
        emit(f"pooled | {arm} | capped", capped)
        if attempts:
            emit(f"pooled | {arm} | attempts-mean", f"{sum(attempts) / len(attempts):.2f}")
        if conv:
            emit(f"pooled | {arm} | convergence-mean", f"{sum(conv) / len(conv):.1f}")
            emit(f"pooled | {arm} | convergence-median", f"{quantile(conv, 0.5):.0f}")
            emit(f"pooled | {arm} | convergence-p90", f"{quantile(conv, 0.9):.0f}")
        lost = [v for g in grammars for v in pooled[(g, arm)]["lost"]]
        spurious = [v for g in grammars for v in pooled[(g, arm)]["spurious"]]
        if lost:
            emit(f"pooled | {arm} | lost-mean", f"{sum(lost) / len(lost):.2f}")
            emit(f"pooled | {arm} | spurious-mean", f"{sum(spurious) / len(spurious):.2f}")
    for grammar in grammars:
        row_cover = coverage[grammar]
        emit(
            f"{grammar} | coverage",
            f"answers {row_cover['answers']} covered {row_cover['covered']} uncovered "
            f"{row_cover['uncovered']} uncovered-landed {row_cover['uncovered_landed']} "
            f"byte-evidence {row_cover['byte_evidence']}",
        )

    for grammar in grammars:
        for arm in ARMS:
            cell = counts[(grammar, arm)]
            values = pooled[(grammar, arm)]
            answers = cell["answers"]
            low, high = wilson(cell["first_landings"], answers)
            tag = f"{grammar} | {arm}"
            emit(f"{tag} | answers", answers)
            emit(f"{tag} | refusals", cell["refusals"])
            if answers:
                emit(f"{tag} | first-landing-percent", f"{100.0 * cell['first_landings'] / answers:.1f}")
                emit(f"{tag} | first-landing-wilson", f"[{low:.1f}, {high:.1f}]")
            if cell["terminal_interior"]:
                emit(
                    f"{tag} | terminal-landing-percent",
                    f"{100.0 * cell['terminal_landings'] / cell['terminal_interior']:.1f}",
                )
            emit(f"{tag} | terminal-refused", cell["terminal_refused"])
            emit(f"{tag} | completion-percent", f"{100.0 * cell['completions'] / cell['trials']:.1f}")
            emit(f"{tag} | capped", cell["capped"])
            emit(
                f"{tag} | attempts-mean",
                f"{sum(values['attempts']) / len(values['attempts']):.2f}" if values["attempts"] else "0",
            )
            if values["overshoot"]:
                emit(f"{tag} | overshoot-mean", f"{sum(values['overshoot']) / len(values['overshoot']):.1f}")
                absolutes = [abs(v) for v in values["overshoot"]]
                emit(f"{tag} | overshoot-abs-mean", f"{sum(absolutes) / len(absolutes):.1f}")
                emit(f"{tag} | overshoot-abs-median", f"{quantile(absolutes, 0.5):.0f}")
            if values["conv"]:
                emit(f"{tag} | convergence-mean", f"{sum(values['conv']) / len(values['conv']):.1f}")
                emit(f"{tag} | convergence-median", f"{quantile(values['conv'], 0.5):.0f}")
                emit(f"{tag} | convergence-p90", f"{quantile(values['conv'], 0.9):.0f}")
                emit(f"{tag} | lost-mean", f"{sum(values['lost']) / len(values['lost']):.2f}")
                emit(f"{tag} | spurious-mean", f"{sum(values['spurious']) / len(values['spurious']):.2f}")

    # The divergence bound's tightness is printed as a derived count rather than stated: rows whose
    # lost and spurious positions fill their region exactly are what make the bound the strongest one
    # assertable, and a claim of tightness with this count at zero would be a recollection.
    assert region_tight > 0, (region_tight, region_rows)
    lines.append(
        f"divergence bound tightness: {region_tight} of {region_rows} positive-region completed rows "
        f"attain lost + spurious = region"
    )

    # The incident-level reconciliations above are printed as derived counts for the same reason: a
    # guard that fired zero times guards nothing, and the count of agreements it enforced should be
    # read off this run rather than recalled. The landing count is split by ownership, because a
    # single arm's terminal rechecking its own first placement is a placement-consistency fact, not a
    # cross-arm one, and the two must not be summed under one label.
    # The harness's own summary is the one campaign artifact these rows cannot rewrite about
    # themselves: the harness printed per-(grammar, op, k, arm) answered and refusal counts into the
    # summary text as it ran, so a row that relabels its outcome or erases its own membership fields
    # still moves a count the summary already fixed. The reconciliation covers every one of the
    # eleven arms with three exact per-cell identities, derived against this archive and each
    # holding on all 594 cells: the summary's answers column counts exactly the rows carrying a
    # first answer, and exactly the rows that spent attempts, so the two are also identical to each
    # other; its initial-refusals column counts exactly the refused rows that never attempted; and
    # its terminal-refusals column counts exactly the refused rows outright. An all-zero summary
    # cell must hold no rows at all, and every CSV cell of a recovery arm must have its summary row,
    # so erasure is refused in both directions. The reconciliation runs after the intra-archive laws
    # above deliberately, as the outermost cross-check, so a corruption a narrower guard can name is
    # refused at that guard and only the shapes no grouping can see reach this one.
    summary_path = sys.argv[1][:-len(".csv")] + ".txt"
    assert sys.argv[1].endswith(".csv"), sys.argv[1]
    assert os.path.exists(summary_path), summary_path

    # The summary is parsed against an exact schema rather than by shape: every header line must be
    # exactly the header the harness prints, one per grammar section; every cell row must name a
    # known operation, damage size, and arm; and the parsed cell set must equal the full expected
    # grid, all six grammars by three operations by three damage sizes by eleven arms, zero-valued
    # cells included. A renamed column, an unknown cell, a missing section, or a section for a
    # grammar the campaign never ran is refused here, not skipped by a shape filter.
    SUMMARY_HEADER = ("op", "k", "strategy", "answers", "refuse", "t-ref", "f-land", "t-land",
                      "complete", "capped", "attempts", "conv", "lost", "spur", "overshoot")
    SUMMARY_OPS = ("substitute", "delete", "insert")
    SUMMARY_KS = ("1", "4", "16")
    harness_cells = {}
    summary_grammar = None
    headers_seen = 0
    with open(summary_path) as handle:
        for line in handle:
            if line.strip() and not line.startswith(" "):
                stripped = line.strip()
                summary_grammar = stripped if stripped in GRAMMARS else None
                continue
            parts = line.split()
            if parts and parts[0] == "op":
                assert tuple(parts) == SUMMARY_HEADER, (summary_path, parts)
                assert summary_grammar is not None, line
                headers_seen += 1
                continue
            # A cell row is op, k, strategy, then the counted columns; pooled and per-seed lines
            # carry neither a known operation at the front nor a known arm third, and are not cells.
            # A line that is cell-shaped on either side, a known operation or a known arm in place,
            # must be a fully well-formed known cell: an unknown operation, damage size, or arm in a
            # cell position is summary corruption and is refused, never skipped by shape.
            if len(parts) >= 11 and (parts[0] in SUMMARY_OPS or parts[2] in ARMS):
                assert parts[0] in SUMMARY_OPS and parts[1] in SUMMARY_KS and parts[2] in ARMS, \
                    (summary_path, parts[:3])
                assert summary_grammar is not None, line
                assert parts[3].isdigit() and parts[4].isdigit() and parts[5].isdigit() \
                    and parts[9].isdigit(), (summary_path, parts)
                summary_cell = (summary_grammar, parts[0], parts[1], parts[2])
                assert summary_cell not in harness_cells, summary_cell
                harness_cells[summary_cell] = (int(parts[3]), int(parts[4]), int(parts[5]),
                                               int(parts[9]))
    assert headers_seen == len(GRAMMARS), (headers_seen, len(GRAMMARS))
    expected_grid = {(grammar, op, k, arm) for grammar in GRAMMARS for op in SUMMARY_OPS
                     for k in SUMMARY_KS for arm in ARMS}
    assert set(harness_cells) == expected_grid, (
        sorted(set(harness_cells) - expected_grid)[:3],
        sorted(expected_grid - set(harness_cells))[:3])
    reconciled_cells = 0
    reconciled_answered = 0
    reconciled_initial = 0
    reconciled_terminal = 0
    reconciled_capped = 0
    for summary_cell, (answers, initial_refusals, terminal_refusals, capped) in \
            harness_cells.items():
        reconciled_cells += 1
        reconciled_answered += answers
        reconciled_initial += initial_refusals
        reconciled_terminal += terminal_refusals
        reconciled_capped += capped
        assert cell_first_present.get(summary_cell, 0) == answers, \
            (summary_cell, "first answers", answers, cell_first_present.get(summary_cell, 0))
        assert cell_attempts_positive.get(summary_cell, 0) == answers, \
            (summary_cell, "attempted", answers, cell_attempts_positive.get(summary_cell, 0))
        assert cell_refused_zero_attempts.get(summary_cell, 0) == initial_refusals, \
            (summary_cell, "initial refusals", initial_refusals,
             cell_refused_zero_attempts.get(summary_cell, 0))
        assert cell_refused.get(summary_cell, 0) == terminal_refusals, \
            (summary_cell, "terminal refusals", terminal_refusals,
             cell_refused.get(summary_cell, 0))
        # The capped count is exact in the summary and exact here: with the row count, the terminal
        # refusals, and the capped rows each fixed, the completed count is determined too, so an
        # outcome relabel between completed and capped moves a count the summary already holds.
        assert cell_capped.get(summary_cell, 0) == capped, \
            (summary_cell, "capped", capped, cell_capped.get(summary_cell, 0))
        if answers == 0 and initial_refusals == 0 and terminal_refusals == 0:
            assert cell_rows.get(summary_cell, 0) == 0, (summary_cell, cell_rows.get(summary_cell, 0))
    for cell in cell_rows:
        if cell[3] != "absorbed":
            assert cell in harness_cells, (cell, "cell missing from the harness summary")

    # The membership commitments, the outermost reconciliation of all: the aggregate identities above
    # bind counts, and a balanced pair of edits inside one cell can preserve every count while moving
    # an incident's membership, so the archived commitment file binds the bytes of each
    # (grammar, op, k, seed) cell's rows themselves. The digests are recomputed here from the raw
    # lines by the same shipped tool that generated the archive's, the reproduction script
    # regenerates them from the fresh harness rerun it byte-compares first, and the comparison is
    # exact in both directions over the full 162-cell grid, so a transfer that survives every count
    # still changes a digest.
    commitments_path = sys.argv[1][:-len(".csv")] + ".commitments.txt"
    assert os.path.exists(commitments_path), commitments_path
    archived_commitments = {}
    with open(commitments_path, encoding="utf-8") as handle:
        for number, line in enumerate(handle.read().split("\n")[:-1], start=1):
            assert len(line) > 66 and line[64:66] == "  ", (commitments_path, number, line[:70])
            digest, key = line[:64], line[66:]
            assert all(c in "0123456789abcdef" for c in digest), (number, digest)
            assert key not in archived_commitments, (number, key)
            archived_commitments[key] = digest
    recomputed = commit_r6.cell_commitments(sys.argv[1])
    expected_commit_cells = {
        "|".join((grammar, op, k, seed))
        for grammar in GRAMMARS for op in SUMMARY_OPS for k in SUMMARY_KS
        for seed in ("0", "1", "2")
    }
    assert set(recomputed) == expected_commit_cells, (
        sorted(set(recomputed) ^ expected_commit_cells)[:3])
    assert set(archived_commitments) == expected_commit_cells, (
        sorted(set(archived_commitments) ^ expected_commit_cells)[:3])
    commitment_mismatches = [key for key in sorted(expected_commit_cells)
                             if archived_commitments[key] != recomputed[key]]
    assert not commitment_mismatches, (commitment_mismatches[:3], "membership commitment broken")


    assert landing_cross > 0 and landing_within > 0 and divergence_rechecks > 0, (
        landing_cross, landing_within, divergence_rechecks)
    assert collapsed_incidents > 0, collapsed_incidents
    lines.append(
        f"landing agreement: {landing_cross + landing_within} repeated placement rechecks, "
        f"{landing_cross} against a different arm and {landing_within} within one arm's own "
        f"placements, every flag a function of its coordinate"
    )
    lines.append(
        f"cross-arm divergence agreement: {divergence_rechecks} rechecks of arms sharing a completed "
        f"single-attempt answer, membership and the converged, lost, spurious triple both enforced"
    )
    lines.append(
        f"harness reconciliation: {reconciled_cells} cells of all eleven arms under the exact "
        f"header and cell grid, {reconciled_answered} answered, {reconciled_initial} initial and "
        f"{reconciled_terminal} terminal refusals and {reconciled_capped} capped equal to the "
        f"harness summary's own counts in both directions, the answered rows exactly the rows "
        f"carrying a first answer"
    )
    lines.append(
        f"membership commitments: {len(expected_commit_cells)} per-seed cells, every recomputed "
        f"cell digest equal to the archived commitment, so membership transfers that balance every "
        f"count are refused at the bytes"
    )
    lines.append(
        f"collapsed-floor identity: {collapsed_incidents} incidents where one past the failure "
        f"reaches the corruption end, the exact and certified pairs identical on every run field and "
        f"the certified pair on its ordered move sidecar"
    )

    with open(f"{out_dir}/r6-stats.txt", "w") as handle:
        handle.write("\n".join(lines) + "\n")

    display = {
        "c-like conventional with strings and line comments": "C-like, strings and line comments",
        "c-like conventional plus block comments alone": "C-like, block comments",
        "json rfc 8259 lexical forms": "JSON, RFC 8259 lexical forms",
        "c-like split-friendly with strings and line comments": "C-like, split-friendly",
        "c-like bare: identifiers numbers operators punctuation": "C-like, bare",
        "json rfc 8259 lexical forms on a real-world document": "JSON, real-world document",
    }
    arm_display = {
        "newline": "newline-past",
        "semicolon": "semicolon-past",
    }
    table_rows = [
        g
        for g in (
            "c-like conventional with strings and line comments",
            "c-like conventional plus block comments alone",
            "json rfc 8259 lexical forms",
            "c-like split-friendly with strings and line comments",
            "c-like bare: identifiers numbers operators punctuation",
            "json rfc 8259 lexical forms on a real-world document",
        )
        if g in display and g in {k[0] for k in counts}
    ]
    table_arms = [
        "certified",
        "exact",
        "skip-one",
        "newline",
        "newline-at",
        "semicolon",
        "semicolon-at",
        "token-newline",
        "token-semicolon",
    ]
    with open(f"{out_dir}/r6-pooled-table.tex", "w") as handle:
        for grammar in table_rows:
            for index, arm in enumerate(table_arms):
                cell = counts[(grammar, arm)]
                values = pooled[(grammar, arm)]
                answers = cell["answers"]
                landing = f"{100.0 * cell['first_landings'] / answers:.1f}\\%" if answers else "--"
                absolutes = [abs(v) for v in values["overshoot"]]
                overshoot = f"{sum(absolutes) / len(absolutes):.1f}" if absolutes else "--"
                conv = (
                    f"{sum(values['conv']) / len(values['conv']):,.0f}".replace(",", "{,}") if values["conv"] else "--"
                )
                first = display[grammar] if index == 0 else ""
                completed = f"{100.0 * cell['completions'] / cell['trials']:.1f}\\%"
                refused_terminal = cell["terminal_refused"]
                answers_tex = f"{answers:,}".replace(",", "{,}")
                refused_tex = f"{refused_terminal:,}".replace(",", "{,}")
                handle.write(
                    f"{first} & {arm_display.get(arm, arm)} & ${answers_tex}$ & ${landing}$ & "
                    f"${overshoot}$ & ${conv}$ & ${completed}$ & ${refused_tex}$ \\\\\n"
                )
            if grammar != table_rows[-1]:
                handle.write("\\midrule\n")

    with open(f"{out_dir}/r6-landing-figure.csv", "w") as handle:
        handle.write("grammar,arm,landing_percent,wilson_low,wilson_high\n")
        for grammar in grammars:
            for arm in ARMS:
                cell = counts[(grammar, arm)]
                answers = cell["answers"]
                rate = 100.0 * cell["first_landings"] / answers if answers else 0.0
                low, high = wilson(cell["first_landings"], answers)
                handle.write(f"{grammar},{arm},{rate:.2f},{low:.2f},{high:.2f}\n")

    print(f"analyzed {damaging} damaging trials over {len(grammars)} rows into {out_dir}")


if __name__ == "__main__":
    main()
