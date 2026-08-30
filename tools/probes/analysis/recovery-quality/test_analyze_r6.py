#!/usr/bin/env python3
#
# Everything in this file is negative-path software testing of this research artifact's own contents: each staged case
# corrupts a scratch copy of data the bundle carries, or alters only the interpreter configuration, and proves a validation check declines it. Nothing here probes,
# monitors, or touches any live system, network, or third party.
# Proves that analyze_r6.py is fail-closed: every invariant it asserts is exercised by a mutation of the
# archived r6 campaign that the analyzer must refuse. The suite first establishes the baseline, running the
# analyzer on the decompressed gold archive and requiring exit 0 together with three emissions byte-identical
# to the archived copies, so a later rejection can be attributed to the mutation rather than to drift. It
# then reproduces both archived mechanism emissions with analyze_r6_mechanism.py from the same staged CSV,
# the printed figures and the overhang data file the manuscript's plot reads coordinate by coordinate, so
# the second checked-in program that feeds the manuscript is pinned to its archived output as well. Each
# case that follows stages one corrupted archive and requires a nonzero exit from the program it is aimed
# at, the auditing analyzer for most cases and the mechanism companion for its eleven, so the
# companion's own refusal of a corrupted archive is executable rather than assumed; a mutation the program
# accepts is a hole in the audit and fails the suite.
#
# Two properties beyond exit codes are checked, because an exit code alone is weak evidence. First, a
# rejection must come from the guard the mutation aims at: the analyzer's stderr is captured and searched
# for a marker chosen from analyze_r6.py itself, either the failing assertion's source text or the content
# of the tuple it carries, or a small set of such fragments where two cases share a guard and only
# the tuple tells them apart; a rejection whose stderr lacks any of them is reported as
# REJECTED-WRONG-GUARD and fails the suite just as an acceptance does. Second, one case stages the pristine
# archive through the same edit and streaming machinery every mutant passes through and requires exit 0 with
# all three emissions byte-identical, so the suite is shown to distinguish acceptance from rejection rather
# than refusing everything it is handed. That case is counted and printed as a control, not as a mutation.
#
# The third property, that the suite would really report an acceptance rather than pass over it, is an
# executable mode rather than a claim about the code. Under --prove-detection one mutation is disarmed
# before the run begins: its rows are staged unchanged, so the analyzer is handed the pristine archive and
# accepts it. Everything downstream is the ordinary machinery, which must record that case as ACCEPTED,
# name it as the run's only failure, and exit nonzero. The mode's own exit code is therefore inverted, 0
# exactly when the suite failed for that reason and 1 otherwise, and the run prints a line saying so.
#
# Usage: test_analyze_r6.py [--prove-detection] [--list-cases] [--case NAME] [data-dir]
#
# The suite is deterministic and stdlib-only. It never writes inside the data directory: the gold archive is
# decompressed once into a temporary working directory, held in memory as lines, and each mutant is written
# by streaming those lines with a small set of line edits applied. The working directory is removed on exit.

import gzip
import os
import shutil
import subprocess
import sys
import tempfile

# Set before the sibling import: an interpreter caching bytecode for it would drop an unlisted file
# into the very tree the exact-set gates refuse.
sys.dont_write_bytecode = True

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import commit_r6

ANALYZER = "analyze_r6.py"
MECHANISM = "analyze_r6_mechanism.py"
GOLD_DIR = "r6"
CAMPAIGN = "recovery-quality-six-rows-512k-500-r6.csv"
SIDECAR_SUFFIX = ".moves.csv"
EMISSIONS = ("r6-stats.txt", "r6-pooled-table.tex", "r6-landing-figure.csv")

# Both files analyze_r6_mechanism.py writes: the printed mechanism figures, and the overhang data file the
# manuscript's plot reads directly, which is pinned here for the same reason the tables are.
MECHANISM_EMISSIONS = ("r6-mechanism.txt", "r6-overhang.dat")

# The eleven arms the harness runs against every damaging incident, and the six of them that recover by
# scanning for a delimiter rather than by deciding. Only the arm count is needed here, to check that a whole
# incident really was collected before a case deletes it, and only the delimiter names are needed to aim the
# zero-attempt cases at a row that refuses without proposing.
ARMS_PER_INCIDENT = 11
DELIMITER_ARMS = ("newline", "newline-at", "semicolon", "semicolon-at", "token-newline", "token-semicolon")

# The trials the schedule draws in every cell, and the last of them by index. The analyzer requires the
# exact count rather than merely a uniform one, so the case that removes the last trial from every cell at
# once, leaving the cells uniform and their numbering contiguous from zero, needs the index to aim at.
TRIALS_PER_CELL = 500
LAST_TRIAL = str(TRIALS_PER_CELL - 1)

# The move sidecar's columns, in the order the analyzer asserts them.
SIDECAR_COLUMNS = [
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
]

# The mutation --prove-detection disarms. It is a sidecar-only case with a single edited line, so
# disarming it is exactly the substitution of one gold line for one corrupted one, and nothing else about
# the run changes.
NEUTERED_CASE = "sidecar-answer-equals-evidence-end"

# The operation name the cell-renaming case writes, chosen so it is not one the schedule ever ran.
UNKNOWN_OPERATION = "scramble"

# The grammar name the unknown-row case writes, chosen so it is not one of the six rows the schedule ran and
# so the analyzer's per-grammar source lengths cannot carry it.
UNKNOWN_GRAMMAR = "c-like row the schedule never ran"

# The outer bound the analyzer holds every archived byte offset under, mirrored here so the case that breaks
# it can name the value the guard reports. No input the harness reads approaches sixteen mebibytes.
POSITION_BOUND = 1 << 24

# The inner bound's source length for a generated row, mirrored here for the same reason: the cases that
# reach past a row's own corpus name the coordinates they write. The five generated rows run on half a
# mebibyte each and the real-world row runs on a longer document, so those cases are staged on generated
# rows, and the rows they use are asserted not to be the real-world one.
GENERATED_SOURCE_BYTES = 512 * 1024
REAL_DOCUMENT_GRAMMAR = "json rfc 8259 lexical forms on a real-world document"

# What each damage operation does to the input's length, mirrored here for the same reason as the lengths
# themselves: a substitution replaces k bytes in place, a deletion removes them, an insertion adds them.
DAMAGED_LENGTH_DELTA = {"substitute": 0, "delete": -1, "insert": 1}

# The real document's source length, mirrored like the generated one because every capped row in this
# archive lives on that grammar and the capped case must name that row's own input end.
REAL_DOCUMENT_SOURCE_BYTES = 631515

# Every column a row may carry only when it answered, mirrored here so the case that rewrites a row into a
# refusal empties exactly the columns a genuine refusal leaves empty and no others.
ANSWER_DEPENDENT_COLUMNS = (
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

# The two coordinates those cases write. Nine hundred million lies past the outer bound as well, so the outer
# bound is what refuses it and the case pins that. Six hundred thousand is the coordinate that matters: it
# sits deep inside the outer bound and past every generated corpus, so only the source length derived for the
# row's own grammar can see it.
WILD_COORDINATE = 900000000
PAST_CORPUS_COORDINATE = 600000

# The driver's attempt budget, and the advance a row claiming the whole of it must archive between its
# first and terminal answers, since every attempt past the first moves the answer at least one byte. The
# two cases that hand a row the whole budget are staged on rows whose answers already cover it, so the
# feasibility relation is satisfied and the contract each case aims at is the only one left to object.
ATTEMPT_BUDGET = 100
BUDGET_ADVANCE = ATTEMPT_BUDGET - 1


def load_gzipped_lines(path):
    # The archive is written without quoting or carriage returns, so plain comma splitting is exact; the
    # property is asserted rather than assumed, because a quoted field would make the line edits wrong.
    with gzip.open(path, "rt", newline="") as handle:
        lines = handle.readlines()
    assert lines, path
    assert not any('"' in line or "\r" in line for line in lines), path
    assert all(line.endswith("\n") for line in lines), path
    return lines


def split_fields(line):
    return line.rstrip("\n").split(",")


def join_fields(fields):
    return ",".join(fields) + "\n"


class Archive:
    # The gold archive as line lists plus the line indices every mutation aims at, all discovered in one
    # pass so no case rescans half a million rows.
    def __init__(self, campaign_lines, sidecar_lines):
        self.campaign = campaign_lines
        self.sidecar = sidecar_lines
        self.columns = split_fields(campaign_lines[0])
        self.index = {name: position for position, name in enumerate(self.columns)}
        self.targets = {}

        # Whole blocks of lines, as opposed to single rows: the eleven arm rows of one incident, needed by
        # the cases that delete an incident outright or rewrite a shared field on all of its arms at once.
        self.blocks = {"incident": [], "repairable_incident": [], "delete_incident": []}

        # Every line of one (grammar, op, k, seed) cell, absorbed draws included: the block the case that
        # deletes a whole schedule cell removes, which is far larger than an incident and so is kept apart
        # from the incident-sized blocks and their arm-count assertion.
        self.cell_block = []

        # Every line of the last trial of every cell, the block whose removal leaves each cell uniform in
        # size and contiguous from zero and so slips past every grid check but the exact trial count.
        self.trial_block = []

        # Every schedule cell the archive carries, used only to check that the block above really covers
        # all of them before a case is built on it.
        self.cells = set()

        # The sidecar line holding each certified arm's first move, so a case that moves a campaign row's
        # evidence interval can move the sidecar's copy of it coherently and leave the reconciliation
        # between the two files intact, the line holding its second move, which is the earliest move the
        # advance guard speaks about, and the line holding its last move, which is the one the archived
        # terminal answer and its landing flag are reconciled against.
        self.sidecar_first = {}
        self.sidecar_second = {}
        self.sidecar_last = {}

        # Certified rows that answered more than once and archive a landed terminal: candidates for the
        # covered-terminal case, which cannot be settled until the sidecar's last move is known.
        self.terminal_candidates = []

        # Certified rows that answered at least three times: candidates for the advance case, which pulls
        # a middle move's evidence back onto its predecessor's answer and so needs a move after it, and
        # which cannot be settled until the sidecar's first two moves are known.
        self.advance_candidates = []
        self._scan()
        self._scan_sidecar()
        self._pair()

    def _scan(self):
        column = self.index
        incident_key = None
        repairable_key = None
        delete_key = None
        cell_key = None
        for position in range(1, len(self.campaign)):
            fields = split_fields(self.campaign[position])

            # The first five columns are the trial key the analyzer builds, so a block is every row that
            # repeats it. The arm rows of one incident are written consecutively, but the membership test
            # is by key rather than by adjacency, so the block stays right if the writer ever reorders.
            key = tuple(fields[:5])
            strategy = fields[column["strategy"]]

            # The first four columns are the schedule cell, and the cell block is collected before the
            # strategy is consulted, because an absorbed draw belongs to its cell exactly as an incident
            # does and the grid the analyzer audits counts both.
            cell = tuple(fields[:4])
            self.cells.add(cell)
            if cell_key is None:
                cell_key = cell
            if cell == cell_key:
                self.cell_block.append(position)
            if fields[column["trial"]] == LAST_TRIAL:
                self.trial_block.append(position)
            if strategy == "absorbed":
                self._note("absorbed_first", position)
                if self.targets.get("absorbed_first") != position:
                    self._note("absorbed_second", position)

                # An absorbed draw whose damage is a span rather than a seam, so its corruption end is the
                # damage start plus the damage size: the geometry the analyzer now checks before an
                # absorbed row leaves the loop, and the only fact a one-byte shift of that end disturbs.
                if fields[column["op"]] == "substitute":
                    self._note("absorbed_substitute", position)

                # An absorbed insertion, whose damage start is a seam rather than a span consumed from the
                # source: the draw the insertion's own source bound is tested through, since insertion is
                # the one operation whose start may sit at the source's very end.
                if fields[column["op"]] == "insert":
                    self._note("absorbed_insert", position)
                continue
            self._note("ordinary", position)
            if incident_key is None:
                incident_key = key
            if key == incident_key:
                self.blocks["incident"].append(position)
            if repairable_key is None and fields[column["repairable"]] == "1":
                repairable_key = key
            if key == repairable_key:
                self.blocks["repairable_incident"].append(position)

            # A deletion incident, whose damage geometry is the seam rather than a span: the case that
            # shifts the corruption end off the deletion point needs all eleven of its arms at once.
            if delete_key is None and fields[column["op"]] == "delete":
                delete_key = key
            if key == delete_key:
                self.blocks["delete_incident"].append(position)
            if fields[column["outcome"]] == "capped":
                self._note("capped_row", position)
                if (
                    fields[column["strategy"]] not in ("certified", "certified-clean")
                    and int(fields[column["first"]]) + int(fields[column["attempts"]]) - 1
                    <= (REAL_DOCUMENT_SOURCE_BYTES
                        if fields[column["grammar"]] == REAL_DOCUMENT_GRAMMAR
                        else GENERATED_SOURCE_BYTES)
                    + DAMAGED_LENGTH_DELTA[fields[column["op"]]] * int(fields[column["k"]])
                ):
                    self._note("capped_room_to_eof", position)

            # A completed row of a repairable incident whose first answer is not the stream's own origin:
            # the convergence point can be put one byte below that answer and stay a canonical nonnegative
            # integer, so the spelling guard cannot be what objects to the convergence-order case.
            if (
                fields[column["outcome"]] == "completed"
                and fields[column["exact_at_anchor"]]
                and fields[column["first"]]
                and int(fields[column["first"]]) >= 1
            ):
                self._note("completed_at_anchor", position)

            # A completed row of a repairable incident whose convergence point does not reach past the
            # corruption end: the divergence region is empty there, so neither a lost nor a spurious
            # boundary can be counted in it. Two rows are kept, because the guard is one assertion and the
            # pair of cases built on it are told apart by the row each reports.
            if (
                fields[column["outcome"]] == "completed"
                and fields[column["exact_at_anchor"]]
                and fields[column["converged"]]
                and int(fields[column["converged"]]) <= int(fields[column["corruption_end"]])
            ):
                self._note("empty_region", position)
                if self.targets.get("empty_region") != position:
                    self._note("empty_region_second", position)

            # Completed rows with room in their divergence region, one repairable and one beyond
            # repair: the region's width bounds the disjoint lost and spurious positions inside it, and
            # a case per side of the label proves the bound is read from the row rather than reached
            # through the anchor column. Each keeps the other count at zero so the staged sum is exact.
            if (
                fields[column["outcome"]] == "completed"
                and fields[column["converged"]]
                and int(fields[column["converged"]]) > int(fields[column["corruption_end"]])
            ):
                if fields[column["exact_at_anchor"]] and fields[column["spurious"]] == "0":
                    self._note("span_room", position)
                if not fields[column["exact_at_anchor"]] and fields[column["lost"]] == "0":
                    self._note("unrepairable_span_room", position)

            # The same two facts on an incident the routine labels beyond repair, where the decider's
            # anchor query returned nothing at all. Completion and an empty divergence region are
            # properties of the row rather than of the label, so the guards must reach these rows too,
            # and a case staged on a repairable row cannot show that they do. The convergence target
            # keeps a byte of room below its first answer above the corruption end, so lowering the
            # convergence point breaks the order alone and leaves the divergence region inhabited.
            if fields[column["outcome"]] == "completed" and not fields[column["exact_at_anchor"]]:
                if fields[column["first"]] and int(fields[column["first"]]) - 1 > int(fields[column["corruption_end"]]):
                    self._note("unrepairable_completed", position)
                if (
                    fields[column["converged"]]
                    and int(fields[column["converged"]]) <= int(fields[column["corruption_end"]])
                    and fields[column["lost"]] == "0"
                ):
                    self._note("unrepairable_empty_region", position)
            if fields[column["trial"]] not in ("", "0"):
                # A trial whose value is not zero is the one padding can genuinely disturb: prefixing a
                # zero to it produces a different string naming the same trial.
                self._note("trial_nonzero", position)
            if strategy == "certified":
                self._note("certified", position)
                if fields[column["outcome"]] == "completed":
                    self._note("completed_certified", position)
                if fields[column["moves_covered"]] not in ("", "0"):
                    self._note("certified_covered", position)

                # A blind row that completed after answering more than once, with at least two bytes
                # between its first answer and its terminal one: the row the two reconciliation cases
                # against the sidecar need, since a single-attempt row's terminal is pinned to its first
                # and moving either of them alone would break that pinning before the sidecar is reached.
                if (
                    fields[column["outcome"]] == "completed"
                    and int(fields[column["attempts"]]) >= 2
                    and int(fields[column["terminal"]]) >= int(fields[column["first"]]) + 2
                ):
                    self._note("certified_multi_attempt", position)

                # A completed blind row whose two answers already cover the whole attempt budget:
                # relabeling a completed row capped means handing it that budget, and a budget of one
                # hundred attempts is feasible only where the answers advance ninety-nine bytes or more.
                if (
                    fields[column["outcome"]] == "completed"
                    and int(fields[column["terminal"]]) - int(fields[column["first"]]) >= BUDGET_ADVANCE
                ):
                    self._note("certified_wide_advance", position)

                # A blind row that answered at least three times: the advance case pulls the second move's
                # evidence back onto the first move's answer, and a third move behind it keeps that move
                # out of the terminal reconciliation, which reads the last move alone.
                if int(fields[column["attempts"]]) >= 3 and len(self.advance_candidates) < 500:
                    self.advance_candidates.append(position)
                if fields[column["first"]]:
                    begin = int(fields[column["evidence_begin"]])
                    end = int(fields[column["evidence_end"]])
                    floor = int(fields[column["failure_offset"]]) + 1
                    corruption_end = int(fields[column["corruption_end"]])
                    if fields[column["evidence_kind"]] == "window":
                        self._note("certified_window", position)
                    else:
                        # Byte-shaped evidence, the shape through which the mechanism companion can see an
                        # unknown certificate kind at all: that program keeps no kind domain of its own and
                        # reads the column only against the width of the interval it describes.
                        self._note("certified_byte", position)

                    # A row whose evidence begins exactly on the blind floor, and whose interval can be
                    # slid one byte down without disturbing anything else the analyzer reconciles: the
                    # answer stays strictly inside the slid interval, the width and certificate shape are
                    # unchanged, and the interval's coverage of the corruption end does not flip, so the
                    # archived covered tally still holds. Only the floor itself is broken.
                    if begin == floor and int(fields[column["first"]]) <= end - 2 and begin != corruption_end:
                        self._note("certified_floor_tight", position)

                    # A blind row whose first move is covered and archived as landed: the row the
                    # covered-move landing reconciliation speaks about, so flipping its flag is the
                    # only corruption the case introduces.
                    if begin >= corruption_end and fields[column["first_landed"]] == "1":
                        self._note("certified_covered_first", position)
                    if (
                        int(fields[column["attempts"]]) >= 2
                        and fields[column["terminal_landed"]] == "1"
                        and len(self.terminal_candidates) < 500
                    ):
                        self.terminal_candidates.append(position)
            if strategy == "certified-clean" and fields[column["first"]]:
                self._note("clean_arm", position)
                begin = int(fields[column["evidence_begin"]])
                end = int(fields[column["evidence_end"]])
                corruption_end = int(fields[column["corruption_end"]])

                # A clean row whose evidence begins exactly on the corruption end, and whose interval can
                # be slid one byte down without disturbing anything else: the answer stays strictly inside
                # the slid interval, the width and certificate shape are unchanged, and the blind floor
                # one past the failure offset is still respected. Only the clean floor is broken, and the
                # covered tally is adjusted with it so the reconciliation cannot be what objects.
                if (
                    begin == corruption_end
                    and int(fields[column["first"]]) <= end - 2
                    and begin - 1 >= int(fields[column["failure_offset"]]) + 1
                    and fields[column["moves_covered"]] not in ("", "0")
                ):
                    self._note("clean_floor_tight", position)
            if strategy == "exact":
                self._note("exact_arm", position)

                # The anchored arm on an incident the routine labels beyond repair, where the anchor
                # query archived nothing: the row that shows the totality guard reads the arm rather
                # than the anchor column.
                if not fields[column["exact_at_anchor"]]:
                    self._note("unrepairable_exact_row", position)

                # The exact arm answering on a repairable incident, far enough above its blind floor that
                # its answer can be lowered a byte and still clear it: the row the direct-agreement case
                # needs, since the archived decider answer and this arm's answer are the same query and
                # moving one of them away from the other is all the case does.
                if (
                    fields[column["exact_at_anchor"]]
                    and fields[column["first"]]
                    and int(fields[column["first"]]) - 1 >= int(fields[column["failure_offset"]]) + 1
                ):
                    self._note("exact_direct_answer", position)

                # The exact arm completing in one move on a repairable incident with both landings
                # archived: the row the two cross-arm reconciliation cases are staged through, since
                # the certified arm answers the same coordinate on these incidents and the
                # incident-level guards read the two rows against each other. That sharing is asserted
                # where the cases are built rather than assumed here, since this scan sees one row at
                # a time.
                if (
                    fields[column["exact_at_anchor"]]
                    and fields[column["outcome"]] == "completed"
                    and fields[column["attempts"]] == "1"
                    and fields[column["first_landed"]] == "1"
                    and fields[column["terminal_landed"]] == "1"
                ):
                    self._note("exact_shared_coordinate", position)
            if strategy == "exact-clean" and fields[column["first"]]:
                # The other arm the harness floors at the corruption end, and the one that carries no
                # evidence at all: its answer alone is what the floor can be tested through.
                self._note("exact_clean_answered", position)

                # The same arm answering on a repairable incident with both landings archived: the arm
                # searches ground the damage never touched, so this is the row whose landing flags the
                # landing contract forbids to be unlanded.
                if (
                    fields[column["exact_at_anchor"]]
                    and fields[column["first_landed"]] == "1"
                    and fields[column["terminal_landed"]] == "1"
                ):
                    self._note("exact_clean_landed", position)

                # The same arm answering on an incident labeled beyond repair. The arm is floored at the
                # corruption end whatever the label says, so it searches the same untouched ground and
                # its answers land the same way; the row is here because the label is not what the
                # landing rests on, and a case staged on a repairable row cannot show that.
                if (
                    not fields[column["exact_at_anchor"]]
                    and fields[column["first_landed"]] == "1"
                    and fields[column["terminal_landed"]] == "1"
                ):
                    self._note("unrepairable_exact_clean_landed", position)
            if strategy == "newline":
                self._note("newline_arm", position)

            # The at-placement rows of the two delimiter pairs. The copied-answer case needs room for
            # the raised first to stay strictly below the terminal; the refusal case needs any answered
            # at-row, rewritten into a refusal while its past-placement partner keeps its answer.
            if strategy == "newline-at" and fields[column["first"]]:
                if (
                    int(fields[column["attempts"]]) >= 2
                    and int(fields[column["terminal"]]) >= int(fields[column["first"]]) + 2
                ):
                    self._note("pair_at_with_room", position)
                self._note("pair_at_answered", position)

                # Room to lower this row's terminal two bytes below its partner's, which leaves the
                # pair's two-value terminal law as the only thing broken: the lowered terminal stays
                # strictly past the first with the attempts still feasible, off the mapped boundary,
                # and unlanded, so no other guard can be what objects.
                if (
                    int(fields[column["attempts"]]) >= 2
                    and fields[column["terminal_landed"]] == "0"
                    and int(fields[column["terminal"]]) - int(fields[column["first"]])
                    >= int(fields[column["attempts"]]) + 1
                    and int(fields[column["terminal"]]) - 1 != int(fields[column["first_true"]])
                    and int(fields[column["terminal"]]) - 2 != int(fields[column["first_true"]])
                ):
                    self._note("pair_terminal_room", position)

                # Attempt headroom for the pair-attempts case: one more attempt stays feasible against
                # the advance and under the budget, and the outcome is not capped, so raising the count
                # by one breaks only the reconciliation with the partner row.
                if (
                    fields[column["outcome"]] != "capped"
                    and int(fields[column["attempts"]]) + 1 < ATTEMPT_BUDGET
                    and int(fields[column["terminal"]]) - int(fields[column["first"]])
                    >= int(fields[column["attempts"]]) + 1
                ):
                    self._note("pair_attempts_room", position)

            # The other delimiter family's at-placement, so every pair law is staged on both families
            # rather than proved for one and assumed for the other: co-presence, the copied first, the
            # terminal's two-value set, and the attempts reconciliation each get a semicolon row too,
            # under the same room conditions their newline twins require.
            if strategy == "semicolon-at" and fields[column["first"]]:
                self._note("semicolon_at_answered", position)
                if (
                    int(fields[column["attempts"]]) >= 2
                    and int(fields[column["terminal"]]) >= int(fields[column["first"]]) + 2
                ):
                    self._note("semicolon_at_with_room", position)
                if (
                    int(fields[column["attempts"]]) >= 2
                    and fields[column["terminal_landed"]] == "0"
                    and int(fields[column["terminal"]]) - int(fields[column["first"]])
                    >= int(fields[column["attempts"]]) + 1
                    and int(fields[column["terminal"]]) - 1 != int(fields[column["first_true"]])
                    and int(fields[column["terminal"]]) - 2 != int(fields[column["first_true"]])
                ):
                    self._note("semicolon_terminal_room", position)
                if (
                    fields[column["outcome"]] != "capped"
                    and int(fields[column["attempts"]]) + 1 < ATTEMPT_BUDGET
                    and int(fields[column["terminal"]]) - int(fields[column["first"]])
                    >= int(fields[column["attempts"]]) + 1
                ):
                    self._note("semicolon_attempts_room", position)
            if fields[column["attempts"]] == "0":
                self._note("zero_attempt", position)
                if strategy in DELIMITER_ARMS:
                    self._note("zero_attempt_delimiter", position)
            elif fields[column["attempts"]] == "1" and fields[column["first"]]:
                self._note("attempts_one", position)
            if fields[column["outcome"]] == "refused" and fields[column["attempts"]] not in ("", "0"):
                # A refusal that did propose: the divergence fields are empty here because the incident did
                # not complete, not because the arm never answered.
                self._note("refused_with_attempts", position)

                # The same row on a generated grammar, for the case that slides its terminal onto the
                # input's very end: the harness closes that state as completed before any other outcome
                # can be written, so a refusal ending there is the contradiction the case stages. The
                # noncertified arm keeps the sidecar out of it.
                if (
                    fields[column["grammar"]] != REAL_DOCUMENT_GRAMMAR
                    and fields[column["strategy"]] not in ("certified", "certified-clean")
                    and int(fields[column["attempts"]]) >= 2
                ):
                    self._note("refused_room_to_eof", position)
            if strategy != "certified" and fields[column["first"]] and fields[column["first_landed"]]:
                self._note("landed_answer", position)
            if strategy not in ("certified", "certified-clean") and fields[column["first"]]:
                self._note("noncertified_answered", position)

                # Answered rows on arms the sidecar knows nothing about, so a terminal answer can be moved
                # on them without any reconciliation standing in the way. The multi-attempt one answered
                # far enough above its floor that its terminal can be put below its first answer and still
                # above that floor, leaving the order between the two as the only broken fact; the
                # one-attempt one carries both landing flags, which a single attempt forces to agree.
                floor = int(fields[column["failure_offset"]]) + 1
                if strategy == "exact-clean":
                    floor = max(floor, int(fields[column["corruption_end"]]))
                attempts = int(fields[column["attempts"]])
                if attempts >= 2 and int(fields[column["first"]]) - 1 >= floor:
                    self._note("noncertified_multi_attempt", position)

                # A row that answered more than once, outside the capped outcome whose count is pinned to
                # the whole budget, with room between the advance its two answers cover and that budget:
                # raising the count past the advance breaks the feasibility relation and nothing else,
                # since the arm owns no sidecar moves to reconcile the count against.
                advance = int(fields[column["terminal"]]) - int(fields[column["first"]])
                if attempts >= 2 and fields[column["outcome"]] != "capped" and advance + 2 < ATTEMPT_BUDGET:
                    self._note("attempts_advance", position)
                # An unlanded answer inside the damaged window of a span operation: the oracle maps no
                # boundary in there, so the flag can only be corrupted upward, and the row chosen keeps
                # its flag at zero so the flip is the only change. More than one attempt, so the
                # single-attempt flag tie is not what objects.
                if (
                    attempts >= 2
                    and fields[column["op"]] in ("substitute", "insert")
                    and int(fields[column["p"]]) <= int(fields[column["first"]]) < int(fields[column["corruption_end"]])
                    and fields[column["first_landed"]] == "0"
                ):
                    self._note("window_interior_answer", position)

                # Answered skip rows on each side of the repairability label, chosen so raising the
                # first answer one byte breaks the arm's definition and nothing else: the advance still
                # covers the attempts, the raised answer stays below the terminal, off the mapped
                # boundary, and unlanded, so no feasibility, boundary, or landing guard can be what
                # objects. The arm's whole definition is answering one past the failure, and a case per
                # label proves the guard reads the row rather than the anchor column.
                if (
                    strategy == "skip-one"
                    and int(fields[column["terminal"]]) - int(fields[column["first"]]) >= attempts + 1
                    and fields[column["first_landed"]] == "0"
                    and int(fields[column["first"]]) + 1 != int(fields[column["first_true"]])
                ):
                    if fields[column["exact_at_anchor"]]:
                        self._note("skip_answered", position)
                    else:
                        self._note("skip_answered_beyond", position)

                # A completed skip row away from the attempt budget, for the outcome relabeling case,
                # and a multi-attempt skip row with room to slide its terminal to the input's end.
                if strategy == "skip-one":
                    if fields[column["outcome"]] == "completed" and fields[column["attempts"]] != "100":
                        self._note("skip_completed", position)
                    if (
                        attempts >= 2
                        and fields[column["outcome"]] != "capped"
                        and fields[column["grammar"]] != REAL_DOCUMENT_GRAMMAR
                    ):
                        self._note("skip_multi", position)

                # A landed covered terminal with the boundary strictly below it and room to lower the
                # terminal beneath the boundary without touching the first answer or the attempt
                # feasibility: the case that proves the terminal half of the mapped-boundary bound.
                if (
                    strategy != "exact-clean"
                    and attempts >= 2
                    and fields[column["terminal_landed"]] == "1"
                    and int(fields[column["first_true"]]) - 1 >= int(fields[column["corruption_end"]])
                    and int(fields[column["first_true"]]) >= int(fields[column["first"]]) + attempts
                ):
                    self._note("terminal_below_boundary", position)

                # A single-attempt answer sitting exactly on the oracle's first mapped boundary with
                # both flags landed: flipping both keeps the single-attempt tie and leaves the
                # boundary-answer landing rule as the only thing broken, on its first-answer side.
                if (
                    attempts == 1
                    and fields[column["first"]] == fields[column["first_true"]]
                    and fields[column["first_landed"]] == "1"
                    and fields[column["terminal_landed"]] == "1"
                ):
                    self._note("boundary_first_landed", position)

                # A row whose terminal answer sits exactly on the oracle's first mapped boundary and is
                # flagged as landed: an answer standing on that boundary is on a boundary of the pristine
                # mapping by that very fact, so the flag cannot be cleared. More than one attempt, so the
                # single-attempt rule that ties the two flags together is not what objects.
                if attempts >= 2 and fields[column["terminal"]] == fields[column["first_true"]]:
                    if fields[column["terminal_landed"]] == "1":
                        self._note("terminal_on_first_true", position)
                if attempts >= 2 and fields[column["outcome"]] == "refused" and advance >= BUDGET_ADVANCE:
                    # A refusal that proposed more than once, its two answers already covering the whole
                    # budget: relabeling its attempt count as that budget leaves every other relation on
                    # the row intact, the feasibility relation included, and the budget contract is what
                    # is left to object.
                    self._note("refused_multi_attempt", position)
                if attempts == 1 and fields[column["first_landed"]] and fields[column["terminal_landed"]]:
                    self._note("attempts_one_flagged", position)
        required = (
            "absorbed_first",
            "absorbed_second",
            "ordinary",
            "certified",
            "certified_covered",
            "completed_certified",
            "certified_window",
            "certified_floor_tight",
            "exact_arm",
            "newline_arm",
            "zero_attempt",
            "zero_attempt_delimiter",
            "attempts_one",
            "refused_with_attempts",
            "landed_answer",
            "noncertified_answered",
            "trial_nonzero",
            "capped_row",
            "certified_covered_first",
            "clean_arm",
            "clean_floor_tight",
            "exact_clean_answered",
            "absorbed_substitute",
            "absorbed_insert",
            "certified_multi_attempt",
            "noncertified_multi_attempt",
            "refused_multi_attempt",
            "attempts_one_flagged",
            "completed_at_anchor",
            "exact_direct_answer",
            "exact_clean_landed",
            "empty_region",
            "empty_region_second",
            "certified_byte",
            "attempts_advance",
            "certified_wide_advance",
            "unrepairable_completed",
            "unrepairable_empty_region",
            "unrepairable_exact_clean_landed",
            "terminal_on_first_true",
            "span_room",
            "unrepairable_span_room",
            "window_interior_answer",
            "skip_completed",
            "skip_multi",
            "terminal_below_boundary",
            "boundary_first_landed",
            "pair_at_with_room",
            "pair_at_answered",
            "skip_answered",
            "skip_answered_beyond",
            "pair_terminal_room",
            "semicolon_at_with_room",
            "unrepairable_exact_row",
            "pair_attempts_room",
            "refused_room_to_eof",
            "capped_room_to_eof",
            "semicolon_at_answered",
            "semicolon_terminal_room",
            "semicolon_attempts_room",
            "exact_shared_coordinate",
        )
        missing = [name for name in required if name not in self.targets]
        assert not missing, missing
        for name, block in self.blocks.items():
            assert len(block) == ARMS_PER_INCIDENT, (name, len(block))
        assert len(self.cell_block) > ARMS_PER_INCIDENT, len(self.cell_block)

        # The last trial of every cell, and no cell missing from the block: the case built on it removes
        # one trial from each cell at once, which is the corruption only the exact count can see.
        trial_cells = {tuple(split_fields(self.campaign[position])[:4]) for position in self.trial_block}
        assert trial_cells == self.cells, sorted(self.cells - trial_cells)[:3]
        assert self.terminal_candidates, "no multi-attempt certified row archives a landed terminal"
        assert self.advance_candidates, "no certified row archives three moves"

    def _scan_sidecar(self):
        for position in range(1, len(self.sidecar)):
            fields = split_fields(self.sidecar[position])
            key = (tuple(fields[:5]), fields[SIDECAR_COLUMNS.index("strategy")])
            if fields[SIDECAR_COLUMNS.index("move")] == "0":
                assert key not in self.sidecar_first, key
                self.sidecar_first[key] = position
            if fields[SIDECAR_COLUMNS.index("move")] == "1":
                assert key not in self.sidecar_second, key
                self.sidecar_second[key] = position

            # The moves of one incident are written in order, so the last line carrying a key is that
            # incident's terminal move, the move the archived terminal answer is reconciled against.
            self.sidecar_last[key] = position

    def _pair(self):
        # The one target that lives in neither file alone: a covered terminal move is a sidecar fact,
        # while the landing flag it forces is a campaign column, so the two scans are joined here.
        for position in self.terminal_candidates:
            fields = split_fields(self.campaign[position])
            key = (tuple(fields[:5]), fields[self.index["strategy"]])
            last = self.sidecar_last.get(key)
            if last is None:
                continue
            if int(self.sidecar_field(last, "evidence_begin")) >= int(fields[self.index["corruption_end"]]):
                self._note("certified_covered_terminal", position)

                # The same row on a generated grammar, whose corpus is the shorter one: the case that
                # slides a terminal move to the input's far end writes the length that grammar's own
                # source implies, and on the real-world row that length would be an ordinary interior
                # coordinate with nothing to object to it. One target per span operation besides, since
                # the damaged length is derived per operation and a case on one operation cannot prove
                # the derivation for another.
                if fields[self.index["grammar"]] != REAL_DOCUMENT_GRAMMAR:
                    self._note("certified_generated_terminal", position)
                    self._note("sidecar_op_" + fields[self.index["op"]], position)
        assert "certified_covered_terminal" in self.targets, len(self.terminal_candidates)
        assert "certified_generated_terminal" in self.targets, len(self.terminal_candidates)
        for op in ("substitute", "insert", "delete"):
            assert "sidecar_op_" + op in self.targets, op

        # The other joined target: the advance case pulls the second move's evidence back to begin exactly
        # on the first move's answer, which is the strongest position the advance guard forbids, and stops
        # there. Two conditions make that the only broken fact. The interval must still fit inside the
        # searched widths once its far end is set one past the answer it carries, which bounds the gap
        # between the two answers by three; and it must not change sides of the corruption end, or the
        # recounted covered tally would stop matching the archived one and object first.
        for position in self.advance_candidates:
            fields = split_fields(self.campaign[position])
            key = (tuple(fields[:5]), fields[self.index["strategy"]])
            first = self.sidecar_first.get(key)
            second = self.sidecar_second.get(key)
            if first is None or second is None:
                continue
            answer = int(self.sidecar_field(first, "answer"))
            next_answer = int(self.sidecar_field(second, "answer"))
            next_begin = int(self.sidecar_field(second, "evidence_begin"))
            corruption_end = int(fields[self.index["corruption_end"]])
            if next_answer - answer > 3:
                continue
            if (next_begin >= corruption_end) != (answer >= corruption_end):
                continue
            self._note("certified_advance", position)
            break
        assert "certified_advance" in self.targets, len(self.advance_candidates)

    def _note(self, name, position):
        self.targets.setdefault(name, position)

    def field(self, position, name):
        return split_fields(self.campaign[position])[self.index[name]]

    def edited(self, position, **changes):
        fields = split_fields(self.campaign[position])
        for name, value in changes.items():
            fields[self.index[name]] = value
        return [join_fields(fields)]

    def tuple_of(self, position):
        # The (key, strategy) pair the analyzer's row guards carry, spelled as the traceback prints it, so
        # two cases aimed at one guard can still be told apart by the row that tripped it.
        fields = split_fields(self.campaign[position])
        return repr((tuple(fields[:5]), fields[self.index["strategy"]]))

    def arm_row_of(self, position, strategy):
        """The row of `strategy` belonging to the same incident as `position`.

        The eleven arm rows of one incident are written consecutively, so the partner sits within a
        block's reach of the given row; membership is still decided by the key, not by adjacency.
        """
        key = tuple(split_fields(self.campaign[position])[:5])
        for candidate in range(max(1, position - 15), min(len(self.campaign), position + 16)):
            fields = split_fields(self.campaign[candidate])
            if tuple(fields[:5]) == key and fields[self.index["strategy"]] == strategy:
                return candidate
        raise AssertionError(("no partner row found", position, strategy))

    def duplicated(self, position):
        return [self.campaign[position], self.campaign[position]]

    def duplicated_edited(self, position, **changes):
        # The gold row followed by a copy of it carrying the edit: the pair is a semantic duplicate whose
        # two lines are not byte-identical, which is exactly what a padded key field produces.
        return [self.campaign[position]] + self.edited(position, **changes)

    def sidecar_edited(self, position, **changes):
        fields = split_fields(self.sidecar[position])
        for name, value in changes.items():
            fields[SIDECAR_COLUMNS.index(name)] = value
        return [join_fields(fields)]

    def sidecar_field(self, position, name):
        return split_fields(self.sidecar[position])[SIDECAR_COLUMNS.index(name)]

    def first_move_of(self, position):
        # The sidecar line carrying move zero of the campaign row at this position, which is the line
        # holding the same evidence interval the campaign row archives as its first answer's.
        fields = split_fields(self.campaign[position])
        return self.sidecar_first[(tuple(fields[:5]), fields[self.index["strategy"]])]

    def second_move_of(self, position):
        # The sidecar line carrying move one of the campaign row at this position, the earliest move whose
        # evidence the analyzer floors against the move before it.
        fields = split_fields(self.campaign[position])
        return self.sidecar_second[(tuple(fields[:5]), fields[self.index["strategy"]])]

    def last_move_of(self, position):
        # The sidecar line carrying the final move of the campaign row at this position, the move the
        # archived terminal answer and its landing flag are reconciled against.
        fields = split_fields(self.campaign[position])
        return self.sidecar_last[(tuple(fields[:5]), fields[self.index["strategy"]])]


def stream(lines, edits, destination):
    # A mutant is the gold line list with a handful of positions replaced by zero, one, or two lines.
    with open(destination, "w", newline="") as handle:
        for position, line in enumerate(lines):
            if position in edits:
                for replacement in edits[position]:
                    handle.write(replacement)
            else:
                handle.write(line)


def link_or_copy(source, destination):
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)


def run_analyzer(analyzer, csv_path, out_dir, extra_env=None):
    # The analyzer's stderr is returned beside its status, because a nonzero exit is only half the evidence:
    # the traceback names the guard that fired, and the suite insists on the guard the case aims at.
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)
    env = dict(os.environ)
    env.pop("PYTHONOPTIMIZE", None)
    if extra_env:
        env.update(extra_env)
    completed = subprocess.run(
        [sys.executable, analyzer, csv_path, out_dir], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env
    )
    return completed.returncode, completed.stderr.decode("utf-8", "replace")


def compare_emissions(out_dir, gold_dir, names):
    mismatched = []
    for name in names:
        produced = os.path.join(out_dir, name)
        archived = os.path.join(gold_dir, name)
        if not os.path.exists(produced):
            mismatched.append(name + " (not emitted)")
        elif open(produced, "rb").read() != open(archived, "rb").read():
            mismatched.append(name)
    return mismatched


# The one declaration of every guard-bearing case's law and stratum, complete over the population:
# every rejecting case that names an intended-guard marker must appear here, and the name sets are
# asserted equal in both directions, so an untagged guard-bearing case, a renamed case, a removed
# entry, a retagged one, or a name that is not staged is a build failure. The case objects, the
# coverage queries, and the --list-cases output all derive from this mapping; the critical entries
# are additionally bound to their cases' own markers below, so a tag moved to a different case or
# two valid tags swapped between cases also fails, and --prove-metadata proves each breakage fatal.
CASE_SCHEMA = {
    "sidecar-answer-equals-evidence-end": ("sidecar", "answer-at-end"),
    "sidecar-answer-outside-evidence-interval": ("sidecar", "answer-outside"),
    "sidecar-row-deleted": ("sidecar", "row-deleted"),
    "sidecar-row-duplicated": ("sidecar", "row-duplicated"),
    "sidecar-move-index-broken": ("sidecar", "index-broken"),
    "sidecar-file-missing": ("sidecar", "file-missing"),
    "sidecar-move-index-padded": ("sidecar", "index-padded"),
    "sidecar-answer-negated": ("sidecar", "answer-negated"),
    "sidecar-move-evidence-back-on-the-previous-answer": ("sidecar", "move-regressed"),
    "sidecar-move-evidence-past-the-damaged-input-length": ("sidecar", "move-past-end"),
    "sidecar-move-past-the-deletion-shortened-input": ("sidecar", "deletion-length"),
    "sidecar-move-past-the-insertion-lengthened-input": ("sidecar", "insertion-length"),
    "campaign-first-answer-changed": ("record-join", "first-answer"),
    "campaign-terminal-answer-changed": ("record-join", "terminal-answer"),
    "campaign-covered-count-corrupted": ("record-join", "covered-count"),
    "campaign-covered-landed-differs-from-covered": ("record-join", "covered-landed"),
    "certified-answer-loses-covered-tally": ("record-join", "covered-presence"),
    "campaign-ordinary-row-duplicated": ("containment", "ordinary-duplicate"),
    "campaign-absorbed-row-duplicated": ("containment", "absorbed-duplicate"),
    "absorbed-row-reuses-damaging-key": ("containment", "key-exclusive"),
    "absorbed-row-carries-answer-field": ("containment", "absorbed-answer-field"),
    "arm-row-dropped-from-incident": ("containment", "arm-missing"),
    "unknown-strategy-name": ("containment", "strategy-domain"),
    "unknown-outcome-value": ("containment", "outcome-domain"),
    "header-schema-altered": ("containment", "column-header"),
    "row-names-a-grammar-with-no-source-length": ("containment", "grammar-domain"),
    "zero-attempt-row-fabricates-first-answer": ("zero-attempt", "fabricated-first"),
    "zero-attempt-row-retains-terminal-and-evidence-kind": ("zero-attempt", "retained-terminal"),
    "positive-attempts-row-loses-first-answer": ("zero-attempt", "answer-presence"),
    "optimized-interpreter-refused": ("interpreter", "analyzer"),
    "mechanism-refuses-a-window-kind-on-byte-shaped-evidence": ("mechanism", "byte-kind"),
    "mechanism-refuses-an-unknown-kind-on-window-shaped-evidence": ("mechanism", "unknown-kind"),
    "mechanism-refuses-a-five-byte-window": ("mechanism", "wide-window"),
    "mechanism-refuses-an-empty-window": ("mechanism", "empty-window"),
    "mechanism-refuses-a-deleted-noncertified-arm-row": ("mechanism", "arm-set"),
    "mechanism-refuses-a-nonnumeric-noncertified-field": ("mechanism", "numeric-canonical"),
    "mechanism-refuses-a-blank-certified-landing-flag": ("mechanism", "certified-flag"),
    "mechanism-refuses-a-deleted-absorbed-incident": ("mechanism", "absorbed-incident-count"),
    "mechanism-refuses-a-trial-rekeyed-off-the-schedule": ("mechanism", "trial-rekeyed"),
    "mechanism-refuses-a-negative-zero-trial": ("mechanism", "negative-zero-trial"),
    "mechanism-refuses-a-negative-zero-ignored-field": ("mechanism", "negative-zero-ignored"),
    "padded-zero-attempts-hides-fabricated-answer": ("padding", "attempts-zero"),
    "padded-trial-key-hides-duplicate-arm-row": ("padding", "trial-key"),
    "padded-attempts-on-answered-row": ("padding", "attempts-answered"),
    "absorbed-row-nonnumeric-damage-coordinate": ("padding", "nonnumeric-coordinate"),
    "completed-row-relabeled-capped-keeping-divergence": ("outcome-fields", "capped-divergence"),
    "refused-row-given-convergence-point": ("outcome-fields", "refused-convergence"),
    "capped-row-short-of-the-attempt-budget": ("outcome-fields", "capped-budget"),
    "attempts-past-the-budget-bound": ("outcome-fields", "budget-bound"),
    "refused-row-claiming-the-whole-attempt-budget": ("outcome-fields", "refused-budget"),
    "skip-row-relabeled-as-a-refusal": ("outcome-fields", "skip-refusal"),
    "completed-row-negative-convergence-point": ("outcome-fields", "negative-convergence"),
    "completed-row-negative-lost-count": ("outcome-fields", "negative-lost"),
    "refused-row-ending-at-the-damaged-input-end": ("outcome-fields", "refused-at-end"),
    "capped-row-ending-at-the-damaged-input-end": ("outcome-fields", "capped-at-end"),
    "certified-evidence-kind-unknown": ("evidence", "kind-domain"),
    "certified-evidence-kind-contradicts-interval-width": ("evidence", "kind-width"),
    "noncertified-arm-carries-fabricated-evidence": ("evidence", "noncertified-fabricated"),
    "noncertified-arm-carries-covered-tally": ("evidence", "noncertified-covered"),
    "certified-evidence-begins-below-blind-anchor": ("evidence", "anchor-floor"),
    "certified-window-stretched-past-four-bytes": ("evidence", "window-width"),
    "absorbed-row-deleted-breaks-grid": ("schedule-grid", "absorbed-deleted"),
    "whole-incident-deleted-breaks-grid": ("schedule-grid", "incident-deleted"),
    "whole-schedule-cell-deleted": ("schedule-grid", "cell-deleted"),
    "schedule-cell-operation-renamed": ("schedule-grid", "operation-renamed"),
    "last-trial-deleted-from-every-cell": ("schedule-grid", "trial-deleted"),
    "arm-damage-coordinates-shifted": ("damage-geometry", "coordinates-shifted"),
    "delete-incident-corruption-end-off-the-seam": ("damage-geometry", "delete-seam"),
    "absorbed-substitution-corruption-end-off-the-span": ("damage-geometry", "absorbed-span"),
    "absorbed-row-position-past-the-sixteen-mebibyte-bound": ("damage-geometry", "position-bound"),
    "absorbed-substitution-moved-to-a-wild-coordinate": ("damage-geometry", "wild-coordinate"),
    "absorbed-substitution-span-past-its-grammar-source": ("damage-geometry", "span-source"),
    "absorbed-insertion-seam-past-its-grammar-source": ("damage-geometry", "seam-source"),
    "terminal-answer-past-the-damaged-input-length": ("damage-geometry", "answer-length"),
    "delete-incident-boundary-at-the-undeleted-source-length":
        ("damage-geometry", "boundary-length"),
    "arm-repairable-flag-flipped": ("repairability", "flag-repair"),
    "minimal-repair-blanked-on-one-arm": ("repairability", "blank-one"),
    "minimal-repair-blanked-on-every-arm-of-one-incident": ("repairability", "blank-incident"),
    "certified-minimal-answer-past-the-answer-taken": ("repairability", "minimal-past-answer"),
    "blind-minimal-position-below-the-search-floor": ("repairability", "blind-floor"),
    "clean-minimal-position-below-the-corruption-end": ("repairability", "clean-floor"),
    "landing-flag-without-answer": ("landing", "flag-without-answer"),
    "covered-first-move-flag-flipped-to-unlanded": ("landing", "covered-first"),
    "covered-terminal-move-flag-flipped-to-unlanded": ("landing", "covered-terminal"),
    "terminal-answer-on-the-mapped-boundary-flagged-unlanded": ("landing", "mapped-terminal"),
    "answer-inside-the-damaged-window-flagged-landed": ("landing", "damaged-window"),
    "terminal-answer-at-the-damaged-end-carrying-a-flag": ("landing", "damaged-end"),
    "landed-terminal-below-the-mapped-boundary": ("landing", "mapped-order"),
    "first-answer-on-the-mapped-boundary-flagged-unlanded": ("landing", "mapped-first"),
    "clean-answer-evidence-below-the-corruption-end": ("clean-scope", "evidence-floor"),
    "clean-sidecar-move-below-the-corruption-end": ("clean-scope", "move-floor"),
    "exact-clean-answer-below-the-corruption-end": ("clean-scope", "answer-floor"),
    "exact-clean-terminal-below-the-corruption-end": ("clean-scope", "terminal-floor"),
    "exact-clean-answer-unlanded-on-both-flags": ("clean-scope", "unlanded-flags"),
    "beyond-repair-exact-clean-answer-unlanded-on-both-flags":
        ("clean-scope", "beyond-repair-flags"),
    "first-true-boundary-blanked-on-every-arm-of-one-incident": ("mapped-boundary", "blanked"),
    "first-true-boundary-below-the-corruption-end-on-every-arm": ("mapped-boundary", "below-end"),
    "first-true-boundary-past-a-landed-covered-answer-on-every-arm":
        ("mapped-boundary", "past-answer"),
    "multi-attempt-terminal-below-the-first-answer": ("attempt-order", "below-first"),
    "multi-attempt-terminal-equal-to-the-first-answer": ("attempt-order", "equal-first"),
    "single-attempt-terminal-past-the-first-answer": ("attempt-order", "single-past"),
    "single-attempt-terminal-landing-differs-from-the-first": ("attempt-order", "single-landing"),
    "attempts-past-the-advance-the-answers-allow": ("attempt-order", "advance-bound"),
    "skip-row-answering-past-its-own-definition": ("attempt-order", "skip-definition"),
    "beyond-repair-skip-row-answering-past-its-own-definition":
        ("attempt-order", "skip-beyond-repair"),
    "completed-row-converges-below-its-first-answer": ("divergence-region", "converged-below"),
    "beyond-repair-row-converges-below-its-first-answer":
        ("divergence-region", "beyond-converged"),
    "completed-row-counts-a-lost-boundary-in-an-empty-divergence-region":
        ("divergence-region", "empty-lost"),
    "completed-row-counts-a-spurious-boundary-in-an-empty-divergence-region":
        ("divergence-region", "empty-spurious"),
    "beyond-repair-row-counts-a-lost-boundary-in-an-empty-divergence-region":
        ("divergence-region", "beyond-empty-lost"),
    "completed-row-counting-more-lost-boundaries-than-its-region-holds":
        ("divergence-region", "region-width"),
    "beyond-repair-row-counting-more-spurious-boundaries-than-its-region-holds":
        ("divergence-region", "beyond-region-width"),
    "negative-decider-answer-on-every-arm-of-one-incident": ("decider", "negative"),
    "decider-answer-at-zero-on-every-arm-of-one-incident": ("decider", "floor"),
    "exact-clean-arm-rewritten-into-a-refusal": ("totality", "clean-refusal-plain"),
    "at-placement-answer-copied-from-its-past-partner": ("pair-first", "newline"),
    "semicolon-at-answer-copied-from-its-past-partner": ("pair-first", "semicolon"),
    "at-placement-refuses-while-its-past-partner-answers": ("pair-presence", "newline"),
    "semicolon-at-refuses-while-its-past-partner-answers": ("pair-presence", "semicolon"),
    "at-placement-terminal-outside-the-pair-law": ("pair-terminal", "newline"),
    "semicolon-at-terminal-outside-the-pair-law": ("pair-terminal", "semicolon"),
    "at-placement-spending-an-attempt-its-partner-does-not": ("pair-attempts", "newline"),
    "semicolon-at-spending-an-attempt-its-partner-does-not": ("pair-attempts", "semicolon"),
    "semicolon-pair-attempts-broken-on-the-equal-terminal-branch": ("pair-attempts", "equal-terminal"),
    "archived-decider-answer-beside-an-exact-arm-that-refused": ("totality", "exact-repairable"),
    "beyond-repair-exact-arm-rewritten-into-a-refusal": ("totality", "exact-beyond-repair"),
    "exact-clean-arm-rewritten-into-a-refusal-on-a-repairable-incident": ("totality", "clean-repairable"),
    "beyond-repair-exact-clean-arm-rewritten-into-a-refusal": ("totality", "clean-beyond-repair"),
    "exact-arm-answer-disagrees-with-the-archived-decider-answer": ("direct-query", "present"),
    "exact-arm-answering-its-blind-floor-after-a-direct-refusal": ("direct-query", "absent"),
    "exact-arm-landing-flags-flipped-against-a-sharing-neighbour": ("landing", "flipped-flags"),
    "newline-pair-equal-terminals-landing-apart": ("landing", "equal-terminals"),
    "semicolon-pair-shared-boundary-terminal-claimed-unlanded": ("landing", "owned-boundary"),
    "single-attempt-arm-diverging-from-its-coordinate-sharing-neighbour": ("divergence", "triple"),
    "sharing-exact-clean-arm-opting-out-of-its-completed-group": ("divergence", "membership-plain"),
    "sharing-certified-clean-arm-opting-out-on-a-collapsed-incident": ("divergence", "membership-collapsed"),
    "collapsed-floor-exact-pair-parting-on-the-first-answer": ("identity", "run-field"),
    "collapsed-floor-certified-pair-parting-on-its-evidence": ("identity", "evidence-field"),
    "collapsed-floor-certified-move-owned-by-its-record-join": ("identity", "sidecar-owned"),
    "token-arm-erasing-its-own-membership": ("erasure", "token-newline"),
    "token-semicolon-arm-erasing-its-own-membership": ("erasure", "token-semicolon"),
    "semicolon-pair-erasing-both-memberships-together": ("erasure", "paired-semicolon"),
    "all-three-excluded-family-arms-erased-together": ("erasure", "all-three"),
    "multi-attempt-semicolon-relabeled-refused-with-its-answer-standing":
        ("erasure", "outcome-relabel"),
    "cell-balanced-membership-transfer-preserving-every-count": ("membership", "emission-neutral"),
    "completed-row-divergence-rebalanced-within-its-region": ("membership", "value-moving"),
    "completed-row-relabeled-capped": ("summary", "capped-count"),
    "summary-header-column-renamed": ("summary", "header-schema"),
    "summary-header-deleted-from-one-section": ("summary", "header-deleted"),
    "summary-header-duplicated-inside-another-section": ("summary", "header-duplicated"),
    "summary-carrying-an-unknown-cell": ("summary", "cell-domain"),
    "summary-cell-with-operation-size-and-arm-all-unknown": ("summary", "cell-all-unknown"),
    "summary-cell-with-an-unknown-operation-alone": ("summary", "cell-operation"),
    "summary-cell-with-an-unknown-arm-alone": ("summary", "cell-arm"),
    "summary-cell-carrying-an-extra-field": ("summary", "cell-extra-field"),
    "summary-cell-missing-its-last-field": ("summary", "cell-missing-field"),
    "summary-rewritten-with-a-non-breaking-space": ("summary", "cell-non-ascii"),
    "summary-count-in-arabic-indic-digits": ("summary", "cell-unicode-digits"),
    "summary-count-padded-with-a-leading-zero": ("summary", "cell-leading-zero"),
    "summary-percentage-past-a-hundred": ("summary", "cell-percentage-range"),
    "summary-count-longer-than-any-campaign-writes": ("summary", "cell-length-bound"),
    "summary-cell-field-off-its-grammar": ("summary", "cell-field-grammar"),
    "summary-signed-overshoot-padded-with-a-leading-zero":
        ("summary", "cell-signed-leading-zero"),
    "summary-signed-overshoot-spelling-a-negative-zero":
        ("summary", "cell-signed-negative-zero"),
    "summary-tail-displacement-padded-with-a-leading-zero":
        ("summary", "tail-leading-zero"),
    "summary-tail-displacement-spelling-a-negative-zero":
        ("summary", "tail-negative-zero"),
    "summary-preamble-seeds-padded-with-a-leading-zero": ("summary", "preamble-leading-zero"),
    "summary-oracle-rows-padded-with-a-leading-zero": ("summary", "oracle-leading-zero"),
    "summary-pooled-answers-padded-with-a-leading-zero": ("summary", "pooled-leading-zero"),
    "summary-per-seed-rate-padded-with-a-leading-zero": ("summary", "seed-leading-zero"),
}

# Ownership, not merely population: each critical stratum names a fragment that must appear inside
# its own case's declared marker and inside no other case's, read back from the case objects after
# staging. A tag moved to a different case, or two valid tags swapped between cases, leaves the
# multiset whole and fails here, because the marker is the ground truth of which guard a case
# exercises and the fragment rides the marker, not the schema. The critical laws are declared once
# and the signature table's key set is asserted equal to their full strata in both directions, so
# a deleted or stray signature is a build failure, never a quietly narrower ownership check.
# Every tag's own guard, declared once for the whole population rather than for the critical laws
# alone. The value is the exact marker tuple the case holding that tag must carry, so a tag moved
# between two cases whose guards differ fails here even though the population, the grid, and the
# critical signatures all survive: the declaration says which guard each tag names, and a case
# whose marker is not that guard cannot hold it. Where two cases genuinely exercise one guard with
# one detail their tuples coincide, and the binding then fixes the group rather than the member;
# the run prints how many such groups there are rather than leaving the reader to assume none.
TAG_GUARDS = {
    ("attempt-order", "advance-bound"): ('assert int(record["terminal"]) - int(record["first"]) >= int(record["attempts"]) - 1', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '13'), 'skip-one')"),
    ("attempt-order", "below-first"): ('assert int(record["terminal"]) >= int(record["first"])', ", 'newline')"),
    ("attempt-order", "equal-first"): ('assert int(record["terminal"]) > int(record["first"])', ", 'newline')"),
    ("attempt-order", "single-landing"): ('assert record["terminal_landed"] == record["first_landed"]', ", 'exact')"),
    ("attempt-order", "single-past"): ('assert record["terminal"] == record["first"]', ", 'certified')"),
    ("attempt-order", "skip-beyond-repair"): ('assert int(record["first"]) == int(record["failure_offset"]) + 1', "(('c-like conventional with strings and line comments', 'substitute', '4', '0', '42'), 'skip-one'"),
    ("attempt-order", "skip-definition"): ('assert int(record["first"]) == int(record["failure_offset"]) + 1', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '13'), 'skip-one'"),
    ("clean-scope", "answer-floor"): ('assert int(record["first"]) >= floor', ", 'exact-clean')"),
    ("clean-scope", "beyond-repair-flags"): ('assert record["first_landed"] == "1" and record["terminal_landed"] == "1"', "(('c-like conventional with strings and line comments', 'substitute', '4', '0', '42'), 'exact-clean')"),
    ("clean-scope", "evidence-floor"): ('assert int(record["evidence_begin"]) >= int(record["corruption_end"])',),
    ("clean-scope", "move-floor"): ('assert begin >= int(record["corruption_end"])',),
    ("clean-scope", "terminal-floor"): ('assert int(record["terminal"]) >= floor', ", 'exact-clean')"),
    ("clean-scope", "unlanded-flags"): ('assert record["first_landed"] == "1" and record["terminal_landed"] == "1"', ", 'exact-clean')"),
    ("containment", "absorbed-answer-field"): ('assert not record[field], (key, field)',),
    ("containment", "absorbed-duplicate"): ('assert key not in absorbed_keys',),
    ("containment", "arm-missing"): ('assert len(arms) == len(ARMS)',),
    ("containment", "column-header"): ('assert head == COLUMNS',),
    ("containment", "grammar-domain"): ('assert record["grammar"] in GRAMMAR_SOURCE_BYTES', 'c-like row the schedule never ran'),
    ("containment", "key-exclusive"): ('assert absorbed_keys.isdisjoint(incidents.keys())',),
    ("containment", "ordinary-duplicate"): ('assert record["strategy"] not in incidents[key]',),
    ("containment", "outcome-domain"): ('assert record["outcome"] in OUTCOMES',),
    ("containment", "strategy-domain"): ('assert record["strategy"] in ARMS',),
    ("damage-geometry", "absorbed-span"): ('assert int(record["corruption_end"]) == int(record["p"]) + int(record["k"])', "'absorbed')"),
    ("damage-geometry", "answer-length"): ('assert int(record[field]) <= damaged_size', ", 'exact', 'terminal', '600000')"),
    ("damage-geometry", "boundary-length"): ('assert int(record[field]) <= damaged_size', ", 'first_true', '524288')"),
    ("damage-geometry", "coordinates-shifted"): ("'p')",),
    ("damage-geometry", "delete-seam"): ('assert record["corruption_end"] == record["p"]',),
    ("damage-geometry", "position-bound"): ("('p', '16777216')",),
    ("damage-geometry", "seam-source"): ('assert int(record["p"]) <= source_size', ", 'absorbed', '524289')"),
    ("damage-geometry", "span-source"): ('assert int(record["p"]) + int(record["k"]) <= source_size', ", 'absorbed', '600000')"),
    ("damage-geometry", "wild-coordinate"): ('assert int(value) < POSITION_BOUND', "('p', '900000000')"),
    ("decider", "floor"): ('assert int(record["exact_at_anchor"]) >= int(record["failure_offset"]) + 1',),
    ("decider", "negative"): ("('exact_at_anchor', '-1')",),
    ("direct-query", "absent"): ('assert int(exact_first) > int', "'299352'"),
    ("direct-query", "present"): ('assert direct == exact_first', ", '327130', '327129')"),
    ("divergence", "membership-collapsed"): ('assert r["outcome"] == "completed" and r["attempts"] == "1"', "'certified-clean'"),
    ("divergence", "membership-plain"): ('assert r["outcome"] == "completed" and r["attempts"] == "1"', "'exact-clean'"),
    ("divergence", "triple"): ('assert len(triples) == 1', "'327131'"),
    ("divergence-region", "beyond-converged"): ('assert int(record["converged"]) >= int(record["first"])', "(('c-like conventional with strings and line comments', 'substitute', '4', '0', '42'), 'certified-clean')"),
    ("divergence-region", "beyond-empty-lost"): ('assert record["lost"] == "0" and record["spurious"] == "0"', "(('c-like conventional with strings and line comments', 'substitute', '4', '0', '42'), 'skip-one')"),
    ("divergence-region", "beyond-region-width"): ('assert int(record["lost"]) + int(record["spurious"]) <= region', "(('c-like conventional with strings and line comments', 'substitute', '16', '0', '0'), 'skip-one'"),
    ("divergence-region", "converged-below"): ('assert int(record["converged"]) >= int(record["first"])', ", 'certified')"),
    ("divergence-region", "empty-lost"): ('assert record["lost"] == "0" and record["spurious"] == "0"', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '12'), 'skip-one')"),
    ("divergence-region", "empty-spurious"): ('assert record["lost"] == "0" and record["spurious"] == "0"', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '15'), 'skip-one')"),
    ("divergence-region", "region-width"): ('assert int(record["lost"]) + int(record["spurious"]) <= region', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '0'), 'certified'"),
    ("erasure", "all-three"): ('cell_first_present.get(summary_cell, 0) == answers', "'4', 'semicolon')"),
    ("erasure", "outcome-relabel"): ('cell_refused.get(summary_cell, 0) == terminal_refusals',),
    ("erasure", "paired-semicolon"): ('cell_first_present.get(summary_cell, 0) == answers', "'1', 'semicolon')"),
    ("erasure", "token-newline"): ('cell_first_present.get(summary_cell, 0) == answers', "'token-newline'"),
    ("erasure", "token-semicolon"): ('cell_first_present.get(summary_cell, 0) == answers', "'token-semicolon'"),
    ("evidence", "anchor-floor"): ('assert int(record["evidence_begin"]) >= int(record["failure_offset"]) + 1',),
    ("evidence", "kind-domain"): ('assert record["evidence_kind"] in ("byte", "window")',),
    ("evidence", "kind-width"): ('assert (record["evidence_kind"] == "byte") == (width == 1)',),
    ("evidence", "noncertified-covered"): ("'exact', 'moves_covered')",),
    ("evidence", "noncertified-fabricated"): ("'exact', 'evidence_begin')",),
    ("evidence", "window-width"): ('assert 1 <= width <= 4',),
    ("identity", "evidence-field"): ('assert arms[one][field] == arms[twin][field]', "'evidence_begin'"),
    ("identity", "run-field"): ('assert arms[one][field] == arms[twin][field]', "'first')"),
    ("identity", "sidecar-owned"): ('assert int(record["evidence_begin"]) == first_begin', "'certified-clean'"),
    ("interpreter", "analyzer"): ('refusing to run with assertions disabled',),
    ("landing", "covered-first"): ('assert record["first_landed"] == "1"',),
    ("landing", "covered-terminal"): ('assert record["terminal_landed"] == "1"',),
    ("landing", "damaged-end"): ('assert record[flag] == ""', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '13'), 'skip-one'", "'terminal_landed')"),
    ("landing", "damaged-window"): ('assert record[flag] == "0"', "(('c-like conventional with strings and line comments', 'substitute', '4', '0', '0'), 'skip-one'", "'first')"),
    ("landing", "equal-terminals"): ('assert seen_flag == flag', "'83377'"),
    ("landing", "flag-without-answer"): ('assert record[flag] == ""', "'first_landed')"),
    ("landing", "flipped-flags"): ('assert seen_flag == flag', "'327130'"),
    ("landing", "mapped-first"): ('assert record[flag] == "1"', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '12'), 'skip-one'", "'first')"),
    ("landing", "mapped-order"): ('assert int(record["first_true"]) <= int(record["terminal"])', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '13'), 'skip-one')"),
    ("landing", "mapped-terminal"): ('assert record[flag] == "1"', ", 'skip-one', 'terminal')"),
    ("landing", "owned-boundary"): ('assert record[flag] == "1"', "'semicolon-at', 'terminal')"),
    ("mapped-boundary", "below-end"): ('assert int(record["first_true"]) >= int(record["corruption_end"])',),
    ("mapped-boundary", "blanked"): ('assert record["first_true"]',),
    ("mapped-boundary", "past-answer"): ('assert int(record["first_true"]) <= int(record["first"])', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '0'), 'certified')"),
    ("mechanism", "byte-kind"): ('analyze_r6_mechanism.py', 'assert (kind == "byte") == (width == 1), row'),
    ("mechanism", "empty-window"): ('analyze_r6_mechanism.py', 'assert 1 <= width <= 4, row'),
    ("mechanism", "unknown-kind"): ('analyze_r6_mechanism.py', 'assert kind in ("byte", "window"), row'),
    ("mechanism", "wide-window"): ('analyze_r6_mechanism.py', 'assert 1 <= width <= 4, row'),
    ("mechanism", "arm-set"): ('analyze_r6_mechanism.py',
                               "arm set is neither the eleven recovery arms nor one absorbed draw"),
    ("mechanism", "numeric-canonical"): ('analyze_r6_mechanism.py',
                                         'attempts is neither blank nor a canonical integer'),
    ("mechanism", "certified-flag"): ('analyze_r6_mechanism.py',
                                      'a certified row leaves first_landed blank'),
    ("mechanism", "absorbed-incident-count"): ('analyze_r6_mechanism.py',
                                               "trial identifiers are not exactly the declared zero through four"),
    ("mechanism", "trial-rekeyed"): ('analyze_r6_mechanism.py',
                                     "trial identifiers are not exactly the declared zero through four"),
    ("mechanism", "negative-zero-trial"): ('analyze_r6_mechanism.py',
                                           'trial is neither blank nor a canonical integer'),
    ("mechanism", "negative-zero-ignored"): ('analyze_r6_mechanism.py',
                                             'attempts is neither blank nor a canonical integer'),
    ("membership", "emission-neutral"): ('membership commitment broken', 'plus block comments alone|substitute|1|0'),
    ("membership", "value-moving"): ('membership commitment broken', 'with strings and line comments|substitute|16|0'),
    ("outcome-fields", "budget-bound"): ('assert int(record["attempts"]) <= 100',),
    ("outcome-fields", "capped-at-end"): ('assert record["outcome"] == "completed"', "(('json rfc 8259 lexical forms on a real-world document', 'substitute', '1', '0', '56'), 'skip-one'"),
    ("outcome-fields", "capped-budget"): ('assert record["outcome"] != "capped" or record["attempts"] == "100"',),
    ("outcome-fields", "capped-divergence"): ("'certified', 'converged')",),
    ("outcome-fields", "negative-convergence"): ("('converged', '-1')",),
    ("outcome-fields", "negative-lost"): ("('lost', '-3')",),
    ("outcome-fields", "refused-at-end"): ('assert record["outcome"] == "completed"', "(('json rfc 8259 lexical forms', 'substitute', '16', '0', '411'), 'semicolon'"),
    ("outcome-fields", "refused-budget"): ('assert not (record["outcome"] == "refused" and record["attempts"] == "100")', ", 'semicolon')"),
    ("outcome-fields", "refused-convergence"): ("'semicolon-at', 'converged')",),
    ("outcome-fields", "skip-refusal"): ('assert record["outcome"] != "refused"', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '0'), 'skip-one')"),
    ("padding", "attempts-answered"): ("('attempts', '01')",),
    ("padding", "attempts-zero"): ("('attempts', '00')",),
    ("padding", "nonnumeric-coordinate"): ("('p', '12x')",),
    ("padding", "trial-key"): ("('trial', '01')",),
    ("pair-attempts", "equal-terminal"): ('assert int(at["attempts"]) == int(past["attempts"]) + extra', "'45'"),
    ("pair-attempts", "newline"): ('assert int(at["attempts"]) == int(past["attempts"]) + extra', "'newline'"),
    ("pair-attempts", "semicolon"): ('assert int(at["attempts"]) == int(past["attempts"]) + extra', "'semicolon'"),
    ("pair-first", "newline"): ('assert int(at["first"]) == int(past["first"]) - 1', "'newline'"),
    ("pair-first", "semicolon"): ('assert int(at["first"]) == int(past["first"]) - 1', "'semicolon'"),
    ("pair-presence", "newline"): ('assert bool(past["first"]) == bool(at["first"])', "'newline'"),
    ("pair-presence", "semicolon"): ('assert bool(past["first"]) == bool(at["first"])', "'semicolon'"),
    ("pair-terminal", "newline"): ('assert int(at["terminal"]) in (int(past["terminal"]) - 1, int(past["terminal"]))', "'newline'"),
    ("pair-terminal", "semicolon"): ('assert int(at["terminal"]) in (int(past["terminal"]) - 1, int(past["terminal"]))', "'semicolon'"),
    ("record-join", "covered-count"): ('assert move_covered.get((key, arm), 0) == expected',),
    ("record-join", "covered-landed"): ('assert record["moves_covered_landed"] == record["moves_covered"]',),
    ("record-join", "covered-presence"): ('assert bool(record[field]) == answered',),
    ("record-join", "first-answer"): ('assert record["first"] and int(record["first"]) == first_answer',),
    ("record-join", "terminal-answer"): ('assert record["terminal"] and int(record["terminal"]) == move_last[(key, arm)]',),
    ("repairability", "blank-incident"): ('assert (record["repairable"] == "1") == bool(record["minimal_repair"])',),
    ("repairability", "blank-one"): ('assert (record["repairable"] == "1") == bool(record["minimal_repair"])',),
    ("repairability", "blind-floor"): ('assert int(record["minimal"]) >= floor', ", 'certified')"),
    ("repairability", "clean-floor"): ('assert int(record["minimal"]) >= floor', ", 'certified-clean')"),
    ("repairability", "flag-repair"): ('assert (record["repairable"] == "1") == bool(record["minimal_repair"])',),
    ("repairability", "minimal-past-answer"): ('assert int(record["minimal"]) <= int(record["first"])',),
    ("schedule-grid", "absorbed-deleted"): ('assert cell_sizes == {500}', 'AssertionError: [499, 500]'),
    ("schedule-grid", "cell-deleted"): ('assert set(cells) == expected_cells', "AssertionError: [('c-like conventional with strings and line comments', 'substitute', '1', '0')]"),
    ("schedule-grid", "incident-deleted"): ('assert cell_sizes == {500}', 'AssertionError: [499, 500]'),
    ("schedule-grid", "operation-renamed"): ('assert set(cells) == expected_cells', "'scramble'"),
    ("schedule-grid", "trial-deleted"): ('assert cell_sizes == {500}', 'AssertionError: [499]'),
    ("sidecar", "answer-at-end"): ('assert begin <= answer < end',),
    ("sidecar", "answer-negated"): ("['c-like conventional with strings and line comments', 'substitute', '1', '0', '0', 'certified'], '-327130')",),
    ("sidecar", "answer-outside"): ('assert begin <= answer < end',),
    ("sidecar", "deletion-length"): ('assert end <= move_damaged_size', ', 524284)'),
    ("sidecar", "file-missing"): ('assert os.path.exists(sidecar)',),
    ("sidecar", "index-broken"): ('assert index == move_prev.get((key, arm), -1) + 1',),
    ("sidecar", "index-padded"): ("['c-like conventional with strings and line comments', 'substitute', '1', '0', '0', 'certified'], '00')",),
    ("sidecar", "insertion-length"): ('assert end <= move_damaged_size', ', 524292)'),
    ("sidecar", "move-past-end"): ('assert end <= move_damaged_size', ', 524288)'),
    ("sidecar", "move-regressed"): ('assert begin > move_last[(key, arm)]', ", 'certified', 1)"),
    ("sidecar", "row-deleted"): ('assert move_prev.get((key, arm), -1) + 1 == expected_moves',),
    ("sidecar", "row-duplicated"): ('assert index == move_prev.get((key, arm), -1) + 1',),
    ("summary", "capped-count"): ('cell_capped.get(summary_cell, 0) == capped', "'skip-one')"),
    ("summary", "cell-all-unknown"): ('summary cell row off the declared grid', 'alien 999 ghost'),
    ("summary", "cell-arm"): ('summary cell row off the declared grid', 'mystery'),
    ("summary", "cell-domain"): ('summary cell row off the declared grid', 'scramble    999'),
    ("summary", "cell-extra-field"): ('summary cell row off the declared grid', '26.1 0'),
    ("summary", "cell-field-grammar"): ('summary cell field off its declared grammar', 'bogus%'),
    ("summary", "cell-signed-leading-zero"):
        ('summary cell field off its declared grammar', '-02.1'),
    ("summary", "cell-signed-negative-zero"):
        ('summary cell field spells a negative zero', '-0.0'),
    ("summary", "tail-leading-zero"):
        ('summary number carries a leading zero', '-014029'),
    ("summary", "tail-negative-zero"):
        ('summary number spells a negative zero', 'displacement -0 bytes'),
    ("summary", "preamble-leading-zero"):
        ('summary number carries a leading zero', '03 independent seeds'),
    ("summary", "oracle-leading-zero"):
        ('summary number carries a leading zero', '06 rows'),
    ("summary", "pooled-leading-zero"):
        ('summary number carries a leading zero', '07356'),
    ("summary", "seed-leading-zero"):
        ('summary number carries a leading zero', '096.9%'),
    ("summary", "cell-leading-zero"): ('summary cell field carries a leading zero', '0841'),
    ("summary", "cell-length-bound"): ('summary cell field longer than any count this campaign writes',),
    ("summary", "cell-missing-field"): ('summary cell row off the declared grid', "0.00', ('substitute'"),
    ("summary", "cell-non-ascii"): ('byte outside ASCII at offset', '0xc2'),
    ("summary", "cell-operation"): ('summary cell row off the declared grid', 'scramble     1'),
    ("summary", "cell-percentage-range"): ('summary percentage outside nought to a hundred', '999.9%'),
    ("summary", "cell-unicode-digits"): ('byte outside ASCII at offset', '0xd9'),
    ("summary", "header-deleted"): ('summary header not verbatim', "'c-like conventional plus block comments alone'"),
    ("summary", "header-duplicated"): ('summary cell row off the declared grid', 'overshoot'),
    ("summary", "header-schema"): ('summary header not verbatim', 'answcnt'),
    ("totality", "clean-beyond-repair"): ('assert answered, (key', "(('c-like conventional with strings and line comments', 'substitute', '4', '0', '42'), 'exact-clean')"),
    ("totality", "clean-refusal-plain"): ('assert answered, (key', ", 'exact-clean')"),
    ("totality", "clean-repairable"): ('assert answered, (key', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '0'), 'exact-clean')"),
    ("totality", "exact-beyond-repair"): ('assert answered, (key', "(('c-like conventional with strings and line comments', 'substitute', '4', '0', '42'), 'exact')"),
    ("totality", "exact-repairable"): ('assert answered, (key', "(('c-like conventional with strings and line comments', 'substitute', '1', '0', '0'), 'exact')"),
    ("zero-attempt", "answer-presence"): ('assert answered == bool(record["first"]) == bool(record["terminal"])',),
    ("zero-attempt", "fabricated-first"): ("'semicolon', 'first')",),
    ("zero-attempt", "retained-terminal"): ("'semicolon', 'evidence_kind')",),
}

CRITICAL_LAWS = ("erasure", "membership", "summary")

CRITICAL_SIGNATURES = {
    ("erasure", "token-newline"): "'token-newline'",
    ("erasure", "token-semicolon"): "'token-semicolon'",
    ("erasure", "paired-semicolon"): "'1', 'semicolon')",
    ("erasure", "all-three"): "'4', 'semicolon')",
    ("erasure", "outcome-relabel"): "cell_refused.get(summary_cell, 0) == terminal_refusals",
    ("membership", "emission-neutral"): "plus block comments alone|substitute|1|0",
    ("membership", "value-moving"): "with strings and line comments|substitute|16|0",
    ("summary", "capped-count"): "cell_capped.get(summary_cell, 0) == capped",
    ("summary", "header-schema"): "answcnt",
    ("summary", "header-deleted"): "'c-like conventional plus block comments alone'",
    ("summary", "header-duplicated"): "overshoot",
    ("summary", "cell-domain"): "scramble    999",
    ("summary", "cell-all-unknown"): "alien 999 ghost",
    ("summary", "cell-operation"): "scramble     1",
    ("summary", "cell-arm"): "mystery",
    ("summary", "cell-extra-field"): "26.1 0",
    ("summary", "cell-missing-field"): "0.00', ('substitute'",
    ("summary", "cell-non-ascii"): "0xc2",
    ("summary", "cell-unicode-digits"): "0xd9",
    ("summary", "cell-leading-zero"): "0841",
    ("summary", "cell-percentage-range"): "999.9%",
    ("summary", "cell-length-bound"): "longer than any count this campaign writes",
    ("summary", "cell-field-grammar"): "bogus%",
    ("summary", "cell-signed-leading-zero"): "-02.1",
    ("summary", "preamble-leading-zero"): "03 independent seeds",
    ("summary", "oracle-leading-zero"): "06 rows",
    ("summary", "pooled-leading-zero"): "07356",
    ("summary", "seed-leading-zero"): "096.9%",
    ("summary", "cell-signed-negative-zero"): "-0.0",
    ("summary", "tail-leading-zero"): "-014029",
    ("summary", "tail-negative-zero"): "displacement -0 bytes",
}

# The complete expected grid, laws to strata, every erasure stratum included: the schema's value
# multiset must equal this grid exactly, so a silent thinning of any law, the erasure family that
# once slipped a query included, is a build failure and never a shorter list that reads like the claim.
LAW_STRATA = {
    "sidecar": ("answer-at-end", "answer-outside", "row-deleted", "row-duplicated", "index-broken",
                "file-missing", "index-padded", "answer-negated", "move-regressed", "move-past-end",
                "deletion-length", "insertion-length"),
    "record-join": ("first-answer", "terminal-answer", "covered-count", "covered-landed",
                    "covered-presence"),
    "containment": ("ordinary-duplicate", "absorbed-duplicate", "key-exclusive",
                    "absorbed-answer-field", "arm-missing", "strategy-domain", "outcome-domain",
                    "column-header", "grammar-domain"),
    "zero-attempt": ("fabricated-first", "retained-terminal", "answer-presence"),
    "interpreter": ("analyzer",),
    "mechanism": ("byte-kind", "unknown-kind", "wide-window", "empty-window", "arm-set",
                  "numeric-canonical", "certified-flag", "absorbed-incident-count",
                  "negative-zero-trial", "negative-zero-ignored", "trial-rekeyed"),
    "padding": ("attempts-zero", "trial-key", "attempts-answered", "nonnumeric-coordinate"),
    "outcome-fields": ("capped-divergence", "refused-convergence", "capped-budget", "budget-bound",
                       "refused-budget", "skip-refusal", "negative-convergence", "negative-lost",
                       "refused-at-end", "capped-at-end"),
    "evidence": ("kind-domain", "kind-width", "noncertified-fabricated", "noncertified-covered",
                 "anchor-floor", "window-width"),
    "schedule-grid": ("absorbed-deleted", "incident-deleted", "cell-deleted", "operation-renamed",
                      "trial-deleted"),
    "damage-geometry": ("coordinates-shifted", "delete-seam", "absorbed-span", "position-bound",
                        "wild-coordinate", "span-source", "seam-source", "answer-length",
                        "boundary-length"),
    "repairability": ("flag-repair", "blank-one", "blank-incident", "minimal-past-answer",
                      "blind-floor", "clean-floor"),
    "clean-scope": ("evidence-floor", "move-floor", "answer-floor", "terminal-floor",
                    "unlanded-flags", "beyond-repair-flags"),
    "mapped-boundary": ("blanked", "below-end", "past-answer"),
    "attempt-order": ("below-first", "equal-first", "single-past", "single-landing",
                      "advance-bound", "skip-definition", "skip-beyond-repair"),
    "divergence-region": ("converged-below", "beyond-converged", "empty-lost", "empty-spurious",
                          "beyond-empty-lost", "region-width", "beyond-region-width"),
    "decider": ("negative", "floor"),
    "pair-first": ("newline", "semicolon"),
    "pair-presence": ("newline", "semicolon"),
    "pair-terminal": ("newline", "semicolon"),
    "pair-attempts": ("newline", "semicolon", "equal-terminal"),
    "totality": ("exact-repairable", "exact-beyond-repair", "clean-repairable",
                 "clean-beyond-repair", "clean-refusal-plain"),
    "direct-query": ("present", "absent"),
    "landing": ("flipped-flags", "equal-terminals", "owned-boundary", "flag-without-answer",
                "covered-first", "covered-terminal", "mapped-terminal", "damaged-window",
                "damaged-end", "mapped-order", "mapped-first"),
    "divergence": ("triple", "membership-plain", "membership-collapsed"),
    "identity": ("run-field", "evidence-field", "sidecar-owned"),
    "erasure": ("token-newline", "token-semicolon", "paired-semicolon", "all-three",
                "outcome-relabel"),
    "membership": ("emission-neutral", "value-moving"),
    "summary": ("capped-count", "header-schema", "header-deleted", "header-duplicated",
                "cell-domain", "cell-all-unknown", "cell-operation", "cell-arm",
                "cell-extra-field", "cell-missing-field", "cell-field-grammar",
                "cell-non-ascii", "cell-unicode-digits", "cell-leading-zero",
                "cell-percentage-range", "cell-length-bound", "cell-signed-leading-zero",
                "cell-signed-negative-zero", "tail-leading-zero", "tail-negative-zero",
                "preamble-leading-zero", "pooled-leading-zero", "seed-leading-zero",
                "oracle-leading-zero"),
}


def assert_case_metadata(cases, schema, signatures=None):
    """Inject each tagged case's law and stratum and hold the declaration fail-closed five ways.

    Population: the schema's name set must equal, in both directions, the set of rejecting cases
    that declare an intended-guard marker, so an untagged guard-bearing case, a renamed case, and a
    schema name that is not staged all fail. Grid: the schema's value multiset must equal the law
    grid in both directions, so a removed, retagged, or extra entry fails. Ownership: every critical
    stratum's signature fragment must appear inside its own case's declared marker and inside no
    other case's, so a tag moved between cases or two valid tags swapped fails even though the
    multiset survives. The signature table's key set: it must equal the critical laws' strata in
    both directions, so a deleted or stray signature is a build failure, never a quietly narrower
    ownership check. Injection: the tags land in the case objects themselves, which is what
    --list-cases prints.
    """
    by_name = {entry["name"]: entry for entry in cases}
    missing = [name for name in schema if name not in by_name]
    assert not missing, ("schema names cases that are not staged", missing)
    guard_bearing = {entry["name"] for entry in cases
                     if entry["expect"] == "reject" and entry["marker"] is not None}
    untagged = sorted(guard_bearing - set(schema))
    assert not untagged, ("guard-bearing cases outside the declaration", untagged[:3])
    tagged_beyond = sorted(set(schema) - guard_bearing)
    assert not tagged_beyond, ("declared names outside the guard-bearing population",
                               tagged_beyond[:3])
    expected = sorted((law, stratum) for law, strata in LAW_STRATA.items() for stratum in strata)
    declared = sorted(schema.values())
    assert declared == expected, (
        "the schema's law and stratum grid is not the expected one",
        [pair for pair in expected if pair not in declared][:3],
        [pair for pair in declared if pair not in expected][:3],
    )
    # The ownership table is itself held fail-closed: its key set must equal, in both directions,
    # the full strata of the critical laws as LAW_STRATA declares them, so deleting one signature,
    # or signing a stratum no critical law carries, is a build failure and never a silently
    # narrower ownership check. The required set is derived from the grid, not declared twice.
    required = {(law, stratum) for law in CRITICAL_LAWS for stratum in LAW_STRATA[law]}
    if signatures is None:
        signatures = CRITICAL_SIGNATURES
    missing_signatures = sorted(required - set(signatures))
    assert not missing_signatures, (
        "critical strata without an ownership signature", missing_signatures[:3])
    stray_signatures = sorted(set(signatures) - required)
    assert not stray_signatures, (
        "ownership signatures outside the critical laws", stray_signatures[:3])
    # The whole-population binding: every declared tag must name the guard the declaration says it
    # names, checked against the case that holds it.
    owners = {tag: name for name, tag in schema.items()}
    declared_tags = set(schema.values())
    assert set(TAG_GUARDS) == declared_tags, (
        "the tag-to-guard declaration is not the tag set",
        sorted(declared_tags - set(TAG_GUARDS))[:3], sorted(set(TAG_GUARDS) - declared_tags)[:3])
    for tag, expected_marker in TAG_GUARDS.items():
        holder = owners[tag]
        marker = by_name[holder]["marker"]
        actual = (marker,) if isinstance(marker, str) else tuple(marker)
        assert actual == expected_marker, (
            "the case holding this tag does not carry the guard the declaration names",
            tag, holder, actual, expected_marker)
    for tag, fragment in signatures.items():
        owner = owners.get(tag)
        assert owner is not None, ("a critical stratum has no owning case", tag)
        marker = by_name[owner]["marker"]
        joined = "\x1f".join(marker) if isinstance(marker, tuple) else (marker or "")
        assert fragment in joined, (
            "the critical stratum's signature is absent from its own case's marker",
            tag, owner, fragment)
        for entry in cases:
            if entry["name"] == owner or entry["marker"] is None:
                continue
            other = entry["marker"]
            other_joined = "\x1f".join(other) if isinstance(other, tuple) else other
            assert fragment not in other_joined, (
                "a critical signature also matches a case that does not own the tag",
                tag, owner, entry["name"], fragment)
    for name, (law, stratum) in schema.items():
        by_name[name]["law"] = law
        by_name[name]["stratum"] = stratum


def build_cases(archive):
    column_targets = archive.targets
    ordinary = column_targets["ordinary"]
    certified = column_targets["certified"]
    certified_covered = column_targets["certified_covered"]
    completed_certified = column_targets["completed_certified"]
    absorbed_first = column_targets["absorbed_first"]
    absorbed_second = column_targets["absorbed_second"]
    exact_arm = column_targets["exact_arm"]
    newline_arm = column_targets["newline_arm"]
    zero_attempt = column_targets["zero_attempt"]
    zero_attempt_delimiter = column_targets["zero_attempt_delimiter"]
    refused_with_attempts = column_targets["refused_with_attempts"]
    landed_answer = column_targets["landed_answer"]
    noncertified_answered = column_targets["noncertified_answered"]
    trial_nonzero = column_targets["trial_nonzero"]
    certified_window = column_targets["certified_window"]
    certified_floor_tight = column_targets["certified_floor_tight"]
    attempts_one = column_targets["attempts_one"]
    capped_row = column_targets["capped_row"]
    clean_arm = column_targets["clean_arm"]
    clean_floor_tight = column_targets["clean_floor_tight"]
    certified_covered_first = column_targets["certified_covered_first"]
    certified_covered_terminal = column_targets["certified_covered_terminal"]
    exact_clean_answered = column_targets["exact_clean_answered"]
    absorbed_substitute = column_targets["absorbed_substitute"]
    absorbed_insert = column_targets["absorbed_insert"]
    certified_multi_attempt = column_targets["certified_multi_attempt"]
    certified_advance = column_targets["certified_advance"]
    noncertified_multi_attempt = column_targets["noncertified_multi_attempt"]
    refused_multi_attempt = column_targets["refused_multi_attempt"]
    attempts_one_flagged = column_targets["attempts_one_flagged"]
    completed_at_anchor = column_targets["completed_at_anchor"]
    exact_direct_answer = column_targets["exact_direct_answer"]
    exact_clean_landed = column_targets["exact_clean_landed"]
    empty_region = column_targets["empty_region"]
    empty_region_second = column_targets["empty_region_second"]
    certified_byte = column_targets["certified_byte"]
    attempts_advance = column_targets["attempts_advance"]
    certified_wide_advance = column_targets["certified_wide_advance"]
    unrepairable_completed = column_targets["unrepairable_completed"]
    unrepairable_empty_region = column_targets["unrepairable_empty_region"]
    unrepairable_exact_clean_landed = column_targets["unrepairable_exact_clean_landed"]
    terminal_on_first_true = column_targets["terminal_on_first_true"]
    certified_generated_terminal = column_targets["certified_generated_terminal"]
    span_room = column_targets["span_room"]
    unrepairable_span_room = column_targets["unrepairable_span_room"]
    window_interior_answer = column_targets["window_interior_answer"]
    skip_completed = column_targets["skip_completed"]
    skip_multi = column_targets["skip_multi"]
    terminal_below_boundary = column_targets["terminal_below_boundary"]
    boundary_first_landed = column_targets["boundary_first_landed"]
    pair_at_with_room = column_targets["pair_at_with_room"]
    pair_at_answered = column_targets["pair_at_answered"]
    sidecar_op_delete = column_targets["sidecar_op_delete"]
    sidecar_op_insert = column_targets["sidecar_op_insert"]
    skip_answered = column_targets["skip_answered"]
    skip_answered_beyond = column_targets["skip_answered_beyond"]
    pair_terminal_room = column_targets["pair_terminal_room"]
    semicolon_at_with_room = column_targets["semicolon_at_with_room"]
    semicolon_at_answered = column_targets["semicolon_at_answered"]
    semicolon_terminal_room = column_targets["semicolon_terminal_room"]
    semicolon_attempts_room = column_targets["semicolon_attempts_room"]
    exact_shared_coordinate = column_targets["exact_shared_coordinate"]
    unrepairable_exact_row = column_targets["unrepairable_exact_row"]
    pair_attempts_room = column_targets["pair_attempts_room"]
    refused_room_to_eof = column_targets["refused_room_to_eof"]
    capped_room_to_eof = column_targets["capped_room_to_eof"]
    incident_block = archive.blocks["incident"]
    repairable_block = archive.blocks["repairable_incident"]
    delete_block = archive.blocks["delete_incident"]
    cell_block = archive.cell_block
    trial_block = archive.trial_block

    first_move = 1
    last_move = len(archive.sidecar) - 1
    move_begin = int(archive.sidecar_field(first_move, "evidence_begin"))
    move_end = int(archive.sidecar_field(first_move, "evidence_end"))
    move_index = archive.sidecar_field(first_move, "move")

    key_fields = split_fields(archive.campaign[ordinary])[:5]
    absorbed_fields = split_fields(archive.campaign[absorbed_first])
    absorbed_fields[:5] = key_fields
    collided_absorbed = [join_fields(absorbed_fields)]

    header_fields = list(archive.columns)
    header_fields[archive.index["spurious"]] = "spurious_count"

    certified_covered_value = archive.field(certified_covered, "moves_covered")
    certified_landed_value = archive.field(certified, "moves_covered_landed")
    shifted_p = str(int(archive.field(exact_arm, "p")) + 1)
    shifted_end = str(int(archive.field(exact_arm, "corruption_end")) + 1)
    flipped = "0" if archive.field(exact_arm, "repairable") == "1" else "1"

    # Values the markers are built from, so a marker states what the mutated row actually says rather than
    # a name written down twice.
    padded_trial = "0" + archive.field(trial_nonzero, "trial")
    zero_delimiter_arm = archive.field(zero_attempt_delimiter, "strategy")
    zero_attempt_arm = archive.field(zero_attempt, "strategy")
    noncertified_arm = archive.field(noncertified_answered, "strategy")
    exact_arm_name = archive.field(exact_arm, "strategy")
    certified_arm_name = archive.field(certified, "strategy")
    refused_arm = archive.field(refused_with_attempts, "strategy")
    noncertified_multi_arm = archive.field(noncertified_multi_attempt, "strategy")
    refused_multi_arm = archive.field(refused_multi_attempt, "strategy")
    attempts_one_arm = archive.field(attempts_one, "strategy")
    attempts_one_flagged_arm = archive.field(attempts_one_flagged, "strategy")
    kind_flipped = "window" if archive.field(certified, "evidence_kind") == "byte" else "byte"

    # The other legal kind for the byte-shaped row, so the case that relabels it names a kind the domain
    # admits and leaves the width law as the only guard that can object.
    byte_row_kind_flipped = "window" if archive.field(certified_byte, "evidence_kind") == "byte" else "byte"
    fabricated_begin = archive.field(noncertified_answered, "corruption_end")
    fabricated_end = str(int(fabricated_begin) + 1)

    # Coordinates for the two cases that move a certified row's evidence interval and move the sidecar's
    # copy of that interval with it. The floor case slides the whole interval one byte below the blind
    # anchor, keeping its width, its certificate shape and its coverage; the width case stretches the far
    # end of a window by one byte, taking the width past four.
    floor_move = archive.first_move_of(certified_floor_tight)
    slid_begin = str(int(archive.field(certified_floor_tight, "evidence_begin")) - 1)
    slid_end = str(int(archive.field(certified_floor_tight, "evidence_end")) - 1)
    width_move = archive.first_move_of(certified_window)
    stretched_end = str(int(archive.field(certified_window, "evidence_end")) + 1)

    # Coordinates for the clean-floor pair. The clean walk searches from the corruption end, so a clean
    # row whose evidence begins exactly there is the one row a single byte can push below the floor. The
    # campaign case slides the interval in both files at once; the sidecar case slides the move alone.
    # Both lower the covered tally by the one move that stops being covered, so the reconciliation between
    # the archived count and the recounted moves stays satisfied either way.
    clean_move = archive.first_move_of(clean_floor_tight)
    clean_below_begin = str(int(archive.field(clean_floor_tight, "evidence_begin")) - 1)
    clean_below_end = str(int(archive.field(clean_floor_tight, "evidence_end")) - 1)
    clean_lowered_covered = str(int(archive.field(clean_floor_tight, "moves_covered")) - 1)
    clean_below_minimal = str(int(archive.field(clean_arm, "corruption_end")) - 1)

    # The six key columns of a sidecar line, spelled as the analyzer's canonical-integer guard reports
    # them, so the two sidecar spelling cases name the exact line and value they corrupt.
    sidecar_key = repr(split_fields(archive.sidecar[first_move])[:6])
    padded_move_index = "0" + move_index
    negated_answer = "-" + archive.sidecar_field(first_move, "answer")

    # A deletion incident's geometry: the corruption end is the deletion seam itself, so moving it one
    # byte past the deletion point is a geometry violation and nothing else. Written into all eleven arms
    # at once, the cross-arm equality check sees nothing wrong and only the geometry can object.
    delete_seam_shifted = str(int(archive.field(delete_block[0], "p")) + 1)

    # The oracle's mapped boundary lies at or past the corruption end by construction, so lowering it one
    # byte below that end is a contradiction the incident carries by itself, on every arm at once.
    lowered_first_true = str(int(archive.field(incident_block[0], "corruption_end")) - 1)

    # An answer one byte below the corruption end on the arm the harness floors there.
    exact_clean_below = str(int(archive.field(exact_clean_answered, "corruption_end")) - 1)

    # The same two corruptions the repairable cases stage, aimed at rows the routine labels beyond
    # repair: a convergence point a byte below the first answer, and a lost boundary counted in a
    # divergence region that is empty. Both values stay canonical and both rows stay completed.
    unrepairable_converged_below = str(int(archive.field(unrepairable_completed, "first")) - 1)
    unrepairable_completed_arm = archive.field(unrepairable_completed, "strategy")
    terminal_on_first_true_arm = archive.field(terminal_on_first_true, "strategy")

    # The last move of a covered-terminal certified row on a generated grammar, slid to the far end of
    # the damaged input. The row's terminal answer moves with it, because the last move's answer and the
    # archived terminal are reconciled against each other; the answer stops exactly at the damaged
    # input's length, which the campaign bound admits, so the one coordinate left outside the input is
    # the move's own evidence end. The length is derived here the way the operation implies rather than
    # read from the analyzer, which is the program on trial.
    terminal_move = archive.last_move_of(certified_generated_terminal)
    terminal_damaged_size = GENERATED_SOURCE_BYTES + DAMAGED_LENGTH_DELTA[
        archive.field(certified_generated_terminal, "op")
    ] * int(archive.field(certified_generated_terminal, "k"))
    # The answer stays one byte inside the input, since an answer at the very end carries no landing
    # flag and the flag the row already has would trip that rule instead; the interval around it still
    # ends one byte outside, which is the one fact left for the length bound to refuse.
    move_answer_inside = str(terminal_damaged_size - 1)
    move_past_input_end = str(terminal_damaged_size + 1)

    # The divergence region's width on the two span-room rows, and the count one past it that the
    # bound must refuse; the other count is zero on each row by the scan, so the staged sum is exact.
    def region_of(position):
        return int(archive.field(position, "converged")) - int(archive.field(position, "corruption_end"))

    lost_past_region = str(region_of(span_room) + 1)
    spurious_past_region = str(region_of(unrepairable_span_room) + 1)

    # The skip row's own damaged input length, derived the way the operation implies, for the case that
    # slides its terminal onto the very end, where the harness computes no landing flag.
    skip_damaged_size = str(
        GENERATED_SOURCE_BYTES
        + DAMAGED_LENGTH_DELTA[archive.field(skip_multi, "op")] * int(archive.field(skip_multi, "k"))
    )

    # The mapped boundary a byte below itself, for the terminal half of the boundary bound.
    boundary_below_terminal = str(int(archive.field(terminal_below_boundary, "first_true")) - 1)

    # The at-placement terminal lowered out of the pair's two-value set, and the semicolon family's
    # copied first answer, so both laws are staged on both families.
    pair_terminal_partner = archive.arm_row_of(pair_terminal_room, "newline")
    pair_terminal_outside = str(int(archive.field(pair_terminal_partner, "terminal")) - 2)
    semicolon_partner = archive.arm_row_of(semicolon_at_with_room, "semicolon")
    semicolon_copied_first = archive.field(semicolon_partner, "first")

    # The past-placement partner rows of the two pair cases, and the value the copy case writes.
    pair_past_partner = archive.arm_row_of(pair_at_with_room, "newline")
    copied_past_first = archive.field(pair_past_partner, "first")
    pair_refusal_fields = {field: "" for field in ANSWER_DEPENDENT_COLUMNS}
    pair_refusal_fields["attempts"] = "0"
    pair_refusal_fields["outcome"] = "refused"

    # The semicolon family's terminal case mirrors the newline one: the at-placement terminal lowered
    # two bytes below its partner's, out of the pair law's two-value set.
    semicolon_terminal_partner = archive.arm_row_of(semicolon_terminal_room, "semicolon")
    semicolon_terminal_outside = str(int(archive.field(semicolon_terminal_partner, "terminal")) - 2)

    # The cross-arm reconciliation cases edit the exact arm of an incident whose certified arm answered
    # the same coordinate the same way. That sharing is the staged precondition, so it is asserted from
    # the archive here rather than assumed: if the target row's neighbour ever stops sharing, this
    # staging fails loudly instead of building a case that proves nothing.
    shared_coordinate = archive.field(exact_shared_coordinate, "first")
    shared_partner = archive.arm_row_of(exact_shared_coordinate, "certified")
    assert archive.field(shared_partner, "first") == shared_coordinate, shared_coordinate
    assert archive.field(shared_partner, "first_landed") == "1", shared_coordinate
    assert archive.field(shared_partner, "outcome") == "completed", shared_coordinate
    assert archive.field(shared_partner, "attempts") == "1", shared_coordinate
    assert archive.field(shared_partner, "converged") == archive.field(exact_shared_coordinate, "converged")
    shared_converged_moved = str(int(archive.field(exact_shared_coordinate, "converged")) + 1)

    # The floor-answer case: the exact arm on a direct-absent incident rewritten to claim the blind
    # floor itself, one past the failure, which is the position the absent direct answer says refused.
    floor_answer = str(int(archive.field(unrepairable_exact_row, "failure_offset")) + 1)

    # The cross-row pair targets below need both placements of an incident at once, which the one-row
    # scan above cannot see: this walk reads each at-placement row beside its past partner and returns
    # the first pair satisfying the staged conditions, failing loudly if the archive holds none.
    def find_at_row(past_name, at_name, want):
        for position in range(1, len(archive.campaign)):
            fields = split_fields(archive.campaign[position])
            if fields[archive.index["strategy"]] != at_name or not fields[archive.index["first"]]:
                continue
            partner = split_fields(archive.campaign[archive.arm_row_of(position, past_name)])
            if want(fields, partner):
                return position, partner
        raise AssertionError((past_name, at_name, "no archive row satisfies the staging conditions"))

    # A pair one byte apart on terminals whose flags disagree about the shared position-to-be: raising
    # the at placement's terminal onto its partner's, with the extra attempt the pair law then expects,
    # leaves every pair law satisfied while the two rows declare one coordinate landed and unlanded.
    def equal_terminal_want(at_fields, past_fields):
        # Raising the terminal and the attempts together keeps the advance law satisfied on its own:
        # the archived row already has its terminal past its first by at least the attempts less one,
        # and both sides of that inequality grow by one. Only the cap and the budget need checking.
        return (
            at_fields[archive.index["terminal_landed"]] == "0"
            and past_fields[archive.index["terminal_landed"]] == "1"
            and int(past_fields[archive.index["terminal"]]) == int(at_fields[archive.index["terminal"]]) + 1
            and at_fields[archive.index["outcome"]] != "capped"
            and int(at_fields[archive.index["attempts"]]) + 1 < ATTEMPT_BUDGET
            # The raised terminal must not sit on the oracle's first mapped boundary, where a clear
            # flag is refused by a row guard before any pair or cross-arm law is read.
            and int(past_fields[archive.index["terminal"]]) != int(at_fields[archive.index["first_true"]])
        )

    newline_landing_row, newline_landing_partner = find_at_row("newline", "newline-at", equal_terminal_want)

    # The semicolon family admits no off-boundary flag disagreement: in every archived semicolon pair
    # whose flags part ways across a one-byte terminal gap, the shared coordinate is the oracle's first
    # mapped boundary, where the row-level boundary rule refuses the clear flag before any cross-arm
    # law is read. The family's staging is therefore that fact itself: the terminal raised onto the
    # shared boundary coordinate must be refused by the row guard that owns the shape.
    def boundary_terminal_want(at_fields, past_fields):
        return (
            at_fields[archive.index["terminal_landed"]] == "0"
            and past_fields[archive.index["terminal_landed"]] == "1"
            and int(past_fields[archive.index["terminal"]]) == int(at_fields[archive.index["terminal"]]) + 1
            and at_fields[archive.index["outcome"]] != "capped"
            and int(at_fields[archive.index["attempts"]]) + 1 < ATTEMPT_BUDGET
            and int(past_fields[archive.index["terminal"]]) == int(at_fields[archive.index["first_true"]])
        )

    semicolon_boundary_row, semicolon_boundary_partner = find_at_row(
        "semicolon", "semicolon-at", boundary_terminal_want)

    # The equal-terminal attempts branch: on the archive's own rows of that branch every field is
    # pinned by the row-level geometry, one attempt over an advance of one, so no coherent single edit
    # reaches the pair law there. The branch is staged from the other side instead: a pair one byte
    # apart is pushed onto the shared terminal without the extra attempt the branch requires, and the
    # attempts reconciliation is the first law that can object.
    def rare_branch_want(at_fields, past_fields):
        # The two placements' terminal flags must agree, which also keeps the raised coordinate off
        # the end of the input and inside every flag-domain rule the partner row already satisfies
        # there, so the attempts reconciliation is the first law that can object.
        return (
            int(past_fields[archive.index["terminal"]]) == int(at_fields[archive.index["terminal"]]) + 1
            and int(at_fields[archive.index["attempts"]]) == int(past_fields[archive.index["attempts"]])
            and int(at_fields[archive.index["attempts"]]) >= 2
            and at_fields[archive.index["outcome"]] != "capped"
            and at_fields[archive.index["terminal_landed"]] != ""
            and at_fields[archive.index["terminal_landed"]] == past_fields[archive.index["terminal_landed"]]
            and int(past_fields[archive.index["terminal"]]) != int(at_fields[archive.index["first_true"]])
        )

    def find_incident(strategy, want):
        """The first row of `strategy` whose whole incident satisfies `want(fields, arms)`."""
        for position in range(1, len(archive.campaign)):
            fields = split_fields(archive.campaign[position])
            if fields[archive.index["strategy"]] != strategy:
                continue
            key = tuple(fields[:5])
            arms = {}
            for candidate in range(max(1, position - 15), min(len(archive.campaign), position + 16)):
                sibling = split_fields(archive.campaign[candidate])
                if tuple(sibling[:5]) == key:
                    arms[sibling[archive.index["strategy"]]] = sibling
            if len(arms) == ARMS_PER_INCIDENT and want(fields, arms):
                return position
        raise AssertionError((strategy, "no incident satisfies the staging conditions"))

    F = archive.index

    def is_collapsed(fields):
        return int(fields[F["failure_offset"]]) + 1 >= int(fields[F["corruption_end"]])

    def completed_one(fields):
        return fields[F["outcome"]] == "completed" and fields[F["attempts"]] == "1"

    # The membership cases relabel an arm out of a shared completed single-attempt group; the group
    # guard must be what objects, so the arm must genuinely share its first answer with another
    # completed single-attempt member.
    def shares_completed_group(name):
        def want(fields, arms):
            return completed_one(fields) and any(
                other != name and r[F["first"]] == fields[F["first"]] and completed_one(r)
                for other, r in arms.items())
        return want

    membership_exact_clean = find_incident(
        "exact-clean",
        lambda fields, arms: not is_collapsed(fields) and shares_completed_group("exact-clean")(fields, arms))
    membership_token_newline = find_incident(
        "token-newline",
        lambda fields, arms: not is_collapsed(fields) and shares_completed_group("token-newline")(fields, arms))
    membership_token_semicolon = find_incident(
        "token-semicolon", lambda fields, arms: completed_one(fields))
    membership_semicolon_pair = find_incident(
        "semicolon",
        lambda fields, arms: completed_one(fields) and fields[F["k"]] == "1"
        and completed_one(arms["semicolon-at"]))
    membership_all_three = find_incident(
        "semicolon",
        lambda fields, arms: completed_one(fields) and fields[F["k"]] == "4"
        and completed_one(arms["semicolon-at"]) and completed_one(arms["token-semicolon"]))
    semicolon_multi_attempt = find_incident(
        "semicolon",
        lambda fields, arms: fields[F["outcome"]] == "completed" and fields[F["attempts"]] == "2")
    membership_certified_clean = find_incident(
        "certified-clean",
        lambda fields, arms: is_collapsed(fields) and shares_completed_group("certified-clean")(fields, arms))

    # The collapsed identity case moves the exact-clean pair's one answer a byte down, since the
    # convergence point sits at the answer on these rows and an upward move would trip its floor. The
    # moved coordinate must stay at or above both search floors and the first mapped boundary, carry
    # no contradicting landing flag, and start no completed group of its own, so the identity
    # comparison is the first law that can object.
    def first_movable(fields, arms):
        if not (is_collapsed(fields) and completed_one(fields)):
            return False
        moved = str(int(fields[F["first"]]) - 1)
        if int(moved) < int(fields[F["failure_offset"]]) + 1:
            return False
        if int(moved) < int(fields[F["corruption_end"]]):
            return False
        if int(fields[F["first_true"]]) > int(moved):
            return False
        for r in arms.values():
            if r[F["first"]] == moved:
                return False
            for coordinate, flag in ((r[F["first"]], r[F["first_landed"]]),
                                     (r[F["terminal"]], r[F["terminal_landed"]])):
                if coordinate == moved and flag == "0":
                    return False
        return True

    identity_first = find_incident("exact-clean", first_movable)
    identity_first_moved = str(int(archive.field(identity_first, "first")) - 1)

    # Every collapsed certified-clean run in this archive is single-attempt, and a single move is
    # fully joined to its row by the sidecar reconciliation, so the ordered-sidecar half of the
    # identity law is implied here by run-field identity plus the join guards; it stays asserted for
    # any archive where multi-move collapsed runs appear. The stageable shapes are therefore the
    # identity on an evidence field, row and move shifted coherently together, and the join guard's
    # ownership of a sidecar-only divergence.
    def evidence_movable(fields, arms):
        if not (is_collapsed(fields) and completed_one(fields)):
            return False
        begin, end = int(fields[F["evidence_begin"]]), int(fields[F["evidence_end"]])
        return end - begin >= 2 and begin + 1 <= int(fields[F["first"]])

    identity_evidence = find_incident("certified-clean", evidence_movable)
    identity_evidence_moved = str(int(archive.field(identity_evidence, "evidence_begin")) + 1)
    identity_evidence_key = tuple(split_fields(archive.campaign[identity_evidence])[:5])
    identity_evidence_move = None
    for position in range(1, len(archive.sidecar)):
        move_fields = split_fields(archive.sidecar[position])
        if (tuple(move_fields[:5]) == identity_evidence_key and move_fields[5] == "certified-clean"
                and move_fields[6] == "0"):
            identity_evidence_move = position
            break
    assert identity_evidence_move is not None, identity_evidence_key

    rare_semicolon_row, rare_semicolon_partner = find_at_row("semicolon", "semicolon-at", rare_branch_want)
    rare_shared_terminal = rare_semicolon_partner[archive.index["terminal"]]
    # The guard's message carries the incident key, whose most distinctive member is the trial.
    rare_incident_trial = archive.field(rare_semicolon_row, "trial")

    # The certified window's interval stretched to five bytes and collapsed to zero, for the
    # companion's width range; the campaign auditor refuses both by its own bound, so these run the
    # companion alone.
    window_end_stretched = str(int(archive.field(certified_window, "evidence_end")) + 1)
    window_end_collapsed = archive.field(certified_window, "evidence_begin")

    # The per-operation damaged lengths for the two remaining sidecar span cases.
    def op_damaged_size(position):
        return GENERATED_SOURCE_BYTES + DAMAGED_LENGTH_DELTA[archive.field(position, "op")] * int(
            archive.field(position, "k")
        )

    delete_move = archive.last_move_of(sidecar_op_delete)
    delete_size = op_damaged_size(sidecar_op_delete)
    insert_move = archive.last_move_of(sidecar_op_insert)
    insert_size = op_damaged_size(sidecar_op_insert)

    # The exact arm turned into a refusal that never proposed, coherently: no attempts, the refused
    # outcome, and every answer-dependent column emptied, exactly as a genuine refusal is archived. The
    # archived decider answer stays where it is, which is the one fact left contradicting the row, and a
    # coherent refusal is the shape that matters, since an incoherent one would be refused by the
    # dependency guards long before the agreement between the two columns is read.
    refused_exact_fields = {field: "" for field in ANSWER_DEPENDENT_COLUMNS}
    refused_exact_fields["attempts"] = "0"
    refused_exact_fields["outcome"] = "refused"
    direct_without_answer = archive.field(exact_direct_answer, "exact_at_anchor")

    # The cell the deletion case removes, spelled as the grid guard reports the cells it finds missing.
    deleted_cell = repr(tuple(split_fields(archive.campaign[cell_block[0]])[:4]))

    # An absorbed substitution's corruption end, moved one byte past the span the damage size fixes. The
    # row stays an absorbed row in every other respect, so the geometry is the only fact it breaks, and
    # the geometry is now read before an absorbed row is allowed to leave the loop.
    absorbed_end_shifted = str(
        int(archive.field(absorbed_substitute, "p")) + int(archive.field(absorbed_substitute, "k")) + 1
    )

    # The blind row the two sidecar reconciliation cases run on: it answered more than once, so moving its
    # first answer up a byte or its terminal answer up a byte leaves the row's own order intact, and the
    # gap between the two absorbs the first of those moves.
    multi_first_raised = str(int(archive.field(certified_multi_attempt, "first")) + 1)
    multi_terminal_raised = str(int(archive.field(certified_multi_attempt, "terminal")) + 1)

    # Terminal positions the terminal contract forbids, each staged on a row where nothing else objects:
    # below the oracle-floored arm's own floor, below the first answer on a row that answered twice, above
    # the first answer on a row that answered once, and equal to it on a row that answered twice.
    noncertified_terminal_below_first = str(int(archive.field(noncertified_multi_attempt, "first")) - 1)
    noncertified_terminal_at_first = archive.field(noncertified_multi_attempt, "first")
    attempts_one_terminal_raised = str(int(archive.field(attempts_one, "terminal")) + 1)
    attempts_one_flag_flipped = "0" if archive.field(attempts_one_flagged, "terminal_landed") == "1" else "1"

    # The advance case's coordinates: the second move's evidence is pulled back to begin exactly on the
    # first move's answer, which the advance guard forbids by one byte, and its far end is set one past
    # the answer it carries, so the interval still contains that answer and still fits the searched
    # widths. Nothing else moves, in either file.
    advance_move = archive.second_move_of(certified_advance)
    advance_begin = archive.sidecar_field(archive.first_move_of(certified_advance), "answer")
    advance_end = str(int(archive.sidecar_field(advance_move, "answer")) + 1)

    # A convergence point one byte below the answer the incident started from, and the arm that archives
    # it, which the marker needs to say which row of the incident tripped the order guard.
    completed_at_anchor_arm = archive.field(completed_at_anchor, "strategy")
    converged_below_first = str(int(archive.field(completed_at_anchor, "first")) - 1)

    # The exact arm's answer lowered a byte, and the archived decider answer it is thereby pulled away
    # from. The arm answers once in this campaign without exception, so its terminal answer is lowered with
    # its first: the one-attempt rule pins the two together and would otherwise be what objects.
    direct_answer = archive.field(exact_direct_answer, "exact_at_anchor")
    exact_answer_lowered = str(int(archive.field(exact_direct_answer, "first")) - 1)

    # An absorbed substitution moved bodily onto the position bound, its damage start and its span end
    # together, so the operation's geometry still holds and only the bound is broken. The start is the
    # first position column the spelling and bound pass reads, so it is the column the guard reports.
    absorbed_beyond_bound = str(POSITION_BOUND)
    absorbed_beyond_end = str(POSITION_BOUND + int(archive.field(absorbed_substitute, "k")))

    # Coordinates for the three cases that move an absorbed draw's damage outside the source its own grammar
    # runs on. Each moves the damage start and the span end together, so the operation's geometry still holds
    # and the row stays an absorbed row in every other respect, and the start is the first coordinate the
    # source bounds are read from, so it is the column the guard reports. The wild coordinate lies past the
    # outer bound as well, which is therefore what refuses it. The other two are what the outer bound cannot
    # see: a substitution's span, which must lie whole inside the source, and an insertion's seam, which may
    # sit at the source's end and so is broken by putting it one byte past.
    wild_start = str(WILD_COORDINATE)
    wild_end = str(WILD_COORDINATE + int(archive.field(absorbed_substitute, "k")))
    past_corpus_start = str(PAST_CORPUS_COORDINATE)
    past_corpus_end = str(PAST_CORPUS_COORDINATE + int(archive.field(absorbed_substitute, "k")))
    insert_past_corpus_start = str(GENERATED_SOURCE_BYTES + 1)
    insert_past_corpus_end = str(GENERATED_SOURCE_BYTES + 1 + int(archive.field(absorbed_insert, "k")))

    # A coordinate at exactly the generated source's length: inside the input a substitution damages, and one
    # byte past the shorter input a deletion of k bytes leaves behind.
    undeleted_source_length = str(GENERATED_SOURCE_BYTES)

    # An attempt count one past what the row's two answers can cover: each attempt after the first
    # advances the answer by at least a byte, so a count of advance plus two is one more than the advance
    # allows, and it stays inside the budget the driver enforces.
    attempts_past_advance = str(
        int(archive.field(attempts_advance, "terminal")) - int(archive.field(attempts_advance, "first")) + 2
    )

    # The mapped boundary raised past every landed covered answer of one incident. A landed answer at or
    # past the corruption end sits on a mapped boundary, so the region's first mapped boundary cannot lie
    # beyond it; raising the boundary past all of them makes the first such answer in the block the one the
    # guard reports, and writing it into all eleven arms leaves the arms agreeing with each other.
    landed_covered_arms = [
        position
        for position in incident_block
        if archive.field(position, "first_landed") == "1"
        and archive.field(position, "first")
        and int(archive.field(position, "first")) >= int(archive.field(position, "corruption_end"))
    ]
    raised_first_true = str(max(int(archive.field(position, "first")) for position in landed_covered_arms) + 1)

    cases = []

    def case(
        name,
        campaign=None,
        sidecar=None,
        omit_sidecar=False,
        extra_env=None,
        program=ANALYZER,
        marker=None,
        expect="reject",
        emissions=None,
        summary_edit=None,
        commitments=None,
    ):
        cases.append(
            {
                "name": name,
                "campaign": campaign or {},
                "sidecar": sidecar or {},
                "omit_sidecar": omit_sidecar,
                "env": extra_env,
                "program": program,
                "marker": marker,
                "expect": expect,
                "emissions": emissions,
                "summary_edit": summary_edit,
                "commitments": commitments,
                # law and stratum are injected from the one schema declaration after every case is
                # built, so the case object carries its own metadata and the two can never drift.
                "law": None,
                "stratum": None,
            }
        )

    # The control, run first so a later failure cannot be blamed on the staging machinery. The archive is
    # streamed through the same edit path every mutant uses, with edits that replace both headers by
    # themselves, so the bytes reaching the analyzer are the gold bytes and the analyzer must accept them
    # and reproduce all three archived emissions. Counted and printed as a control, never as a mutation.
    case(
        "pristine-archive-restaged-unchanged",
        campaign={0: [archive.campaign[0]]},
        sidecar={0: [archive.sidecar[0]]},
        expect="accept",
        emissions=EMISSIONS,
    )

    # Sidecar geometry: half-open evidence intervals, containment, contiguous numbering, presence. The two
    # containment cases push the answer out of its interval from opposite ends, so they share one marker:
    # the guard is the same containment assertion either way.
    case(
        "sidecar-answer-equals-evidence-end",
        sidecar={first_move: archive.sidecar_edited(first_move, answer=str(move_end))},
        marker="assert begin <= answer < end",
    )
    case(
        "sidecar-answer-outside-evidence-interval",
        sidecar={first_move: archive.sidecar_edited(first_move, answer=str(move_begin - 1))},
        marker="assert begin <= answer < end",
    )
    case(
        "sidecar-row-deleted",
        sidecar={last_move: []},
        marker="assert move_prev.get((key, arm), -1) + 1 == expected_moves",
    )
    case(
        "sidecar-row-duplicated",
        sidecar={first_move: [archive.sidecar[first_move], archive.sidecar[first_move]]},
        marker="assert index == move_prev.get((key, arm), -1) + 1",
    )
    case(
        "sidecar-move-index-broken",
        sidecar={first_move: archive.sidecar_edited(first_move, move=str(int(move_index) + 7))},
        marker="assert index == move_prev.get((key, arm), -1) + 1",
    )
    case("sidecar-file-missing", omit_sidecar=True, marker="assert os.path.exists(sidecar)")

    # Reconciliation between the sidecar and the archived per-incident aggregates. Both cases run on a row
    # that answered more than once with room to spare between its two answers, because the row's own
    # terminal contract pins a single-attempt row's terminal to its first answer and would otherwise be
    # what objects, leaving the reconciliation against the sidecar unproven.
    case(
        "campaign-first-answer-changed",
        campaign={certified_multi_attempt: archive.edited(certified_multi_attempt, first=multi_first_raised)},
        marker='assert record["first"] and int(record["first"]) == first_answer',
    )
    case(
        "campaign-terminal-answer-changed",
        campaign={certified_multi_attempt: archive.edited(certified_multi_attempt, terminal=multi_terminal_raised)},
        marker='assert record["terminal"] and int(record["terminal"]) == move_last[(key, arm)]',
    )
    case(
        "campaign-covered-count-corrupted",
        campaign={certified_covered: archive.edited(certified_covered, moves_covered="0", moves_covered_landed="0")},
        marker="assert move_covered.get((key, arm), 0) == expected",
    )
    case(
        "campaign-covered-landed-differs-from-covered",
        campaign={certified: archive.edited(certified, moves_covered_landed=str(int(certified_landed_value) + 1))},
        marker='assert record["moves_covered_landed"] == record["moves_covered"]',
    )

    # One row per (incident, arm), one row per absorbed draw, and the two outcomes kept exclusive.
    case(
        "campaign-ordinary-row-duplicated",
        campaign={ordinary: archive.duplicated(ordinary)},
        marker='assert record["strategy"] not in incidents[key]',
    )
    case(
        "campaign-absorbed-row-duplicated",
        campaign={absorbed_second: archive.duplicated(absorbed_second)},
        marker="assert key not in absorbed_keys",
    )
    case(
        "absorbed-row-reuses-damaging-key",
        campaign={absorbed_first: collided_absorbed},
        marker="assert absorbed_keys.isdisjoint(incidents.keys())",
    )
    case(
        "absorbed-row-carries-answer-field",
        campaign={absorbed_second: archive.edited(absorbed_second, first="12345")},
        marker="assert not record[field], (key, field)",
    )
    case("arm-row-dropped-from-incident", campaign={newline_arm: []}, marker="assert len(arms) == len(ARMS)")

    # Zero-attempt rows answer nothing, whichever arm they belong to. The markers are the tail of the tuple
    # the emptiness guard carries, because its source text is shared with the guard that keeps evidence off
    # the arms that do not own it; the field name in the tuple is what separates them.
    case(
        "zero-attempt-row-fabricates-first-answer",
        campaign={
            zero_attempt: archive.edited(
                zero_attempt, first=archive.field(zero_attempt, "corruption_end"), first_landed="0"
            )
        },
        marker=f"'{zero_attempt_arm}', 'first')",
    )
    case(
        "zero-attempt-row-retains-terminal-and-evidence-kind",
        campaign={
            zero_attempt: archive.edited(
                zero_attempt,
                terminal=archive.field(zero_attempt, "corruption_end"),
                terminal_landed="0",
                evidence_kind="byte",
            )
        },
        marker=f"'{zero_attempt_arm}', 'evidence_kind')",
    )

    # Incident-level facts written identically into every arm's row. The damage span moves whole, its
    # start and its end together, so the operation's geometry still holds on the moved row and the arm
    # simply disagrees with its ten siblings about where the damage was; the shared-field guard reports
    # the first column it finds them differing on, which is the position the span starts at.
    case(
        "arm-damage-coordinates-shifted",
        campaign={exact_arm: archive.edited(exact_arm, p=shifted_p, corruption_end=shifted_end)},
        marker="'p')",
    )
    case(
        "arm-repairable-flag-flipped",
        campaign={exact_arm: archive.edited(exact_arm, repairable=flipped)},
        marker='assert (record["repairable"] == "1") == bool(record["minimal_repair"])',
    )

    # Landing flags exist exactly with their answers, and covered tallies belong to the certified arms.
    case(
        "landing-flag-without-answer",
        campaign={landed_answer: archive.edited(landed_answer, first="")},
        marker=('assert record[flag] == ""', "'first_landed')"),
    )
    case(
        "noncertified-arm-carries-covered-tally",
        campaign={exact_arm: archive.edited(exact_arm, moves_covered="1", moves_covered_landed="1")},
        marker=f"'{exact_arm_name}', 'moves_covered')",
    )

    # Schema and domain guards.
    case(
        "unknown-strategy-name",
        campaign={landed_answer: archive.edited(landed_answer, strategy="certified-extra")},
        marker='assert record["strategy"] in ARMS',
    )
    case(
        "unknown-outcome-value",
        campaign={newline_arm: archive.edited(newline_arm, outcome="aborted")},
        marker='assert record["outcome"] in OUTCOMES',
    )
    case("header-schema-altered", campaign={0: [join_fields(header_fields)]}, marker="assert head == COLUMNS")

    # The audit is the assertions, so an interpreter that strips them is refused on the pristine archive.
    # The marker here is the refusal message rather than a traceback, because there is no traceback: the
    # program declines to start.
    case(
        "optimized-interpreter-refused",
        extra_env={"PYTHONOPTIMIZE": "1"},
        marker="refusing to run with assertions disabled",
    )

    # Canonical integers. A padded number is not a number the analyzer will accept, whether the padding
    # hides a zero behind a nonzero-looking string or hides a semantic duplicate behind a distinct key.
    # Both cases carry a second corruption the padding would otherwise conceal: a fabricated answer on a
    # row that never proposed, and a repeated arm row inside one incident.
    case(
        "padded-zero-attempts-hides-fabricated-answer",
        campaign={
            zero_attempt_delimiter: archive.edited(
                zero_attempt_delimiter,
                attempts="00",
                first=archive.field(zero_attempt_delimiter, "corruption_end"),
                first_landed="0",
            )
        },
        marker="('attempts', '00')",
    )
    case(
        "padded-trial-key-hides-duplicate-arm-row",
        campaign={trial_nonzero: archive.duplicated_edited(trial_nonzero, trial=padded_trial)},
        marker=f"('trial', '{padded_trial}')",
    )

    # Outcome states own their fields. The divergence triple exists exactly on a completed incident, so
    # relabeling a completed row and populating a refused one both break the same dependency from opposite
    # directions; the arm name in the tuple keeps the two markers apart. The relabeled row is given the
    # full attempt budget along with its new label, because a capped outcome means the budget was spent
    # and the budget contract would otherwise be what objects, leaving the dependency unproven. It is a
    # row that answered more than once for the same reason: the budget it is given is not one, and a row
    # claiming more than one attempt must archive a terminal answer past its first, far enough past it to
    # cover the advance that budget implies, which is why the row chosen archives ninety-nine bytes or more
    # between its two answers.
    case(
        "completed-row-relabeled-capped-keeping-divergence",
        campaign={certified_wide_advance: archive.edited(certified_wide_advance, outcome="capped", attempts="100")},
        marker=f"'{certified_arm_name}', 'converged')",
    )
    case(
        "refused-row-given-convergence-point",
        campaign={
            refused_with_attempts: archive.edited(
                refused_with_attempts, converged=archive.field(refused_with_attempts, "corruption_end")
            )
        },
        marker=f"'{refused_arm}', 'converged')",
    )

    # Evidence ownership and shape. The certificate kind is drawn from a closed set, it agrees with the
    # width of the interval it describes, and it exists on the certified arms alone.
    case(
        "certified-evidence-kind-unknown",
        campaign={certified: archive.edited(certified, evidence_kind="bogus")},
        marker='assert record["evidence_kind"] in ("byte", "window")',
    )
    case(
        "certified-evidence-kind-contradicts-interval-width",
        campaign={certified: archive.edited(certified, evidence_kind=kind_flipped)},
        marker='assert (record["evidence_kind"] == "byte") == (width == 1)',
    )
    case(
        "noncertified-arm-carries-fabricated-evidence",
        campaign={
            noncertified_answered: archive.edited(
                noncertified_answered, evidence_begin=fabricated_begin, evidence_end=fabricated_end
            )
        },
        marker=f"'{noncertified_arm}', 'evidence_begin')",
    )
    case(
        "certified-answer-loses-covered-tally",
        campaign={certified: archive.edited(certified, moves_covered="")},
        marker="assert bool(record[field]) == answered",
    )

    # The blind floor and the window's width, the two facts the overhang law rests on. Both cases move the
    # sidecar's copy of the interval with the campaign's, so the reconciliation between the two files stays
    # satisfied and the guard named in the marker is the only one that can object. The floor case slides an
    # interval that began exactly on the anchor one byte below it; the width case stretches a window to
    # five bytes, which is a shape no certificate has.
    case(
        "certified-evidence-begins-below-blind-anchor",
        campaign={
            certified_floor_tight: archive.edited(
                certified_floor_tight, evidence_begin=slid_begin, evidence_end=slid_end
            )
        },
        sidecar={floor_move: archive.sidecar_edited(floor_move, evidence_begin=slid_begin, evidence_end=slid_end)},
        marker='assert int(record["evidence_begin"]) >= int(record["failure_offset"]) + 1',
    )
    case(
        "certified-window-stretched-past-four-bytes",
        campaign={certified_window: archive.edited(certified_window, evidence_end=stretched_end)},
        sidecar={width_move: archive.sidecar_edited(width_move, evidence_end=stretched_end)},
        marker="assert 1 <= width <= 4",
    )

    # The canonical-integer guard's positive case: a padded one is refused exactly as a padded zero is, so
    # the guard is not merely a test for a falsely nonempty attempts field.
    case(
        "padded-attempts-on-answered-row",
        campaign={attempts_one: archive.edited(attempts_one, attempts="01")},
        marker="('attempts', '01')",
    )

    # Grid completeness. The schedule is a fixed block of trials per cell, so a silently deleted absorbed
    # draw and a silently deleted incident are the same corruption seen twice: one trial vanishes from one
    # cell, and the cell that lost it is the only one short of the count. The two share a marker because
    # they share both the guard and the pair of sizes it reports.
    case(
        "absorbed-row-deleted-breaks-grid",
        campaign={absorbed_first: []},
        marker=("assert cell_sizes == {500}", "AssertionError: [499, 500]"),
    )
    case(
        "whole-incident-deleted-breaks-grid",
        campaign={position: [] for position in incident_block},
        marker=("assert cell_sizes == {500}", "AssertionError: [499, 500]"),
    )

    # Answers exist exactly with attempts: blanking both the answer and its landing flag leaves the flag
    # pairing intact and lands squarely on the attempts dependency.
    case(
        "positive-attempts-row-loses-first-answer",
        campaign={noncertified_answered: archive.edited(noncertified_answered, first="", first_landed="")},
        marker='assert answered == bool(record["first"]) == bool(record["terminal"])',
    )

    # A nonnumeric damage coordinate on an absorbed row: absorbed rows carry only the coordinates, and the
    # canonical-integer check runs on every row before the strategy is even consulted, so the corruption is
    # caught before anything asks what kind of row it is.
    case(
        "absorbed-row-nonnumeric-damage-coordinate",
        campaign={absorbed_first: archive.edited(absorbed_first, p="12x")},
        marker="('p', '12x')",
    )

    # The sign domain: every archived count and position is nonnegative, with no column excepted, the
    # convergence point included, since the signed distance the manuscript quotes is derived from that
    # point rather than stored. A negative loss on a completed row breaks the domain directly, and the
    # convergence case below breaks it on the one column an exception would most plausibly have been
    # written for. A negative decider answer written identically
    # into all eleven arms of one incident is the case the cross-arm equality check cannot see, because the
    # arms agree perfectly with each other; only the domain rejects it.
    case(
        "completed-row-negative-lost-count",
        campaign={completed_certified: archive.edited(completed_certified, lost="-3")},
        marker="('lost', '-3')",
    )
    case(
        "negative-decider-answer-on-every-arm-of-one-incident",
        campaign={position: archive.edited(position, exact_at_anchor="-1") for position in repairable_block},
        marker="('exact_at_anchor', '-1')",
    )

    # Evidence order: the walk answers with the first certificate it sees, so the minimal answerable
    # position never lies past the answer taken, and the nonminimality figure is exactly their gap.
    case(
        "certified-minimal-answer-past-the-answer-taken",
        campaign={certified: archive.edited(certified, minimal=str(int(archive.field(certified, "first")) + 1))},
        marker='assert int(record["minimal"]) <= int(record["first"])',
    )

    # The repairability dependency: repairable, minimal_repair and exact_at_anchor rise and fall together.
    # Blanking the minimal repair on a single arm of a repairable incident makes that row inconsistent with
    # itself, so the row-level dependency fires before the cross-arm comparison ever runs; blanking it on
    # all eleven arms removes the cross-arm disagreement entirely and leaves the dependency as the only
    # guard that can catch it. Both therefore carry the dependency's marker, which is the point of the
    # second case: the audit does not rely on arms disagreeing.
    case(
        "minimal-repair-blanked-on-one-arm",
        campaign={repairable_block[0]: archive.edited(repairable_block[0], minimal_repair="")},
        marker='assert (record["repairable"] == "1") == bool(record["minimal_repair"])',
    )
    case(
        "minimal-repair-blanked-on-every-arm-of-one-incident",
        campaign={position: archive.edited(position, minimal_repair="") for position in repairable_block},
        marker='assert (record["repairable"] == "1") == bool(record["minimal_repair"])',
    )

    # A whole schedule cell removed. Every row of every trial of one (grammar, op, k, seed) cell goes,
    # absorbed draws and damaging incidents alike, so nothing survives to make the cell sizes disagree and
    # the grammar keeps its other twenty-six cells: the trial-count guards see a perfectly regular
    # schedule. Only the exact product of grammars, operations, damage sizes and seeds notices that one
    # cell of the schedule was never run.
    case(
        "whole-schedule-cell-deleted",
        campaign={position: [] for position in cell_block},
        marker=("assert set(cells) == expected_cells", f"AssertionError: [{deleted_cell}]"),
    )

    # The same grid, broken from the other side. The cell is left in place and its operation is renamed to
    # one the schedule never ran, on every row of it at once. The trial counts stay right, the arms stay
    # eleven, the shared columns still agree, and the damage geometry of the renamed rows is the geometry
    # a span operation has, which is what the analyzer checks for anything that is not a deletion. What
    # the product notices is that one expected cell is gone and one unexpected cell has appeared.
    case(
        "schedule-cell-operation-renamed",
        campaign={position: archive.edited(position, op=UNKNOWN_OPERATION) for position in cell_block},
        marker=("assert set(cells) == expected_cells", f"'{UNKNOWN_OPERATION}'"),
    )

    # The dodge the uniform-size and contiguity checks cannot see: the last trial of every cell removed at
    # once. Every cell still holds the same number of trials as every other, numbered contiguously from
    # zero, so only the schedule's exact trial count per cell is left to object.
    case(
        "last-trial-deleted-from-every-cell",
        campaign={position: [] for position in trial_block},
        marker=("assert cell_sizes == {500}", "AssertionError: [499]"),
    )

    # Canonical integers in the sidecar, the same discipline the campaign columns keep. A padded move
    # index still parses as the index the contiguity check wants, and a negated answer still parses as a
    # number, so without the spelling guard both would reach guards that cannot see the corruption at all.
    case(
        "sidecar-move-index-padded",
        sidecar={first_move: archive.sidecar_edited(first_move, move=padded_move_index)},
        marker=f"{sidecar_key}, '{padded_move_index}')",
    )
    case(
        "sidecar-answer-negated",
        sidecar={first_move: archive.sidecar_edited(first_move, answer=negated_answer)},
        marker=f"{sidecar_key}, '{negated_answer}')",
    )

    # Covered moves land, the harness's own runtime assertion, and the archive records that landing twice:
    # once as the per-incident flag and once as the covered tally. The two cases unland a flag while
    # leaving the tallies untouched and equal to each other, so the tally reconciliation is satisfied and
    # only the per-move landing reconciliation can object, at the first move and at the terminal one
    # respectively. The first-move case unlands both flags of its row at once: no covered first move in
    # this archive belongs to a row that answered twice, and on a row that answered once the two flags are
    # required to agree, so unlanding one alone would be caught by that agreement instead.
    case(
        "covered-first-move-flag-flipped-to-unlanded",
        campaign={
            certified_covered_first: archive.edited(certified_covered_first, first_landed="0", terminal_landed="0")
        },
        marker='assert record["first_landed"] == "1"',
    )
    case(
        "covered-terminal-move-flag-flipped-to-unlanded",
        campaign={certified_covered_terminal: archive.edited(certified_covered_terminal, terminal_landed="0")},
        marker='assert record["terminal_landed"] == "1"',
    )

    # The clean walk's own floor, which is the corruption end rather than the blind anchor. The campaign
    # case slides the archived interval and the sidecar's copy of it one byte below the corruption end
    # together, lowering the covered tally by the move that stops being covered, so the reconciliation
    # between the two files still holds and the clean floor is the only broken fact. The sidecar case
    # leaves the campaign row's interval alone and moves the move, which the analyzer floors per move.
    case(
        "clean-answer-evidence-below-the-corruption-end",
        campaign={
            clean_floor_tight: archive.edited(
                clean_floor_tight,
                evidence_begin=clean_below_begin,
                evidence_end=clean_below_end,
                moves_covered=clean_lowered_covered,
                moves_covered_landed=clean_lowered_covered,
            )
        },
        sidecar={
            clean_move: archive.sidecar_edited(
                clean_move, evidence_begin=clean_below_begin, evidence_end=clean_below_end
            )
        },
        marker='assert int(record["evidence_begin"]) >= int(record["corruption_end"])',
    )
    case(
        "clean-sidecar-move-below-the-corruption-end",
        campaign={
            clean_floor_tight: archive.edited(
                clean_floor_tight, moves_covered=clean_lowered_covered, moves_covered_landed=clean_lowered_covered
            )
        },
        sidecar={
            clean_move: archive.sidecar_edited(
                clean_move, evidence_begin=clean_below_begin, evidence_end=clean_below_end
            )
        },
        marker='assert begin >= int(record["corruption_end"])',
    )

    # The driver's attempt budget. A capped outcome means the budget was spent to the last attempt, so a
    # capped row that stopped short is corruption; and no row of any outcome may claim more attempts than
    # the budget allows. The row chosen is a delimiter arm, which owns no sidecar moves, so no
    # reconciliation stands between the mutation and the contract it aims at.
    case(
        "capped-row-short-of-the-attempt-budget",
        campaign={capped_row: archive.edited(capped_row, attempts="99")},
        marker='assert record["outcome"] != "capped" or record["attempts"] == "100"',
    )
    case(
        "attempts-past-the-budget-bound",
        campaign={capped_row: archive.edited(capped_row, attempts="101")},
        marker='assert int(record["attempts"]) <= 100',
    )

    # The minimal answerable position obeys the floor of the search that found it, and the two certified
    # arms search under different floors: the blind walk from one past the failure, the clean walk from
    # the corruption end besides. Both cases put the minimal position below the arm's own floor while
    # leaving it at or before the answer taken, so the order guard is satisfied and the floor is the only
    # broken fact; the arm name in the tuple is what tells the two apart.
    case(
        "blind-minimal-position-below-the-search-floor",
        campaign={certified: archive.edited(certified, minimal="0")},
        marker=('assert int(record["minimal"]) >= floor', f", '{certified_arm_name}')"),
    )
    case(
        "clean-minimal-position-below-the-corruption-end",
        campaign={clean_arm: archive.edited(clean_arm, minimal=clean_below_minimal)},
        marker=('assert int(record["minimal"]) >= floor', ", 'certified-clean')"),
    )

    # Damage geometry and the oracle's mapped boundary, both written into all eleven arms of one incident
    # so the arms agree perfectly with each other and the cross-arm comparison sees nothing. A deletion
    # leaves a seam, so its corruption end is the deletion point itself and a shifted end is a geometry
    # violation; and every damaging incident carries the boundary the overshoot figures are measured
    # against, so a blank one is corruption rather than an absent measurement.
    case(
        "delete-incident-corruption-end-off-the-seam",
        campaign={position: archive.edited(position, corruption_end=delete_seam_shifted) for position in delete_block},
        marker='assert record["corruption_end"] == record["p"]',
    )
    case(
        "first-true-boundary-blanked-on-every-arm-of-one-incident",
        campaign={position: archive.edited(position, first_true="") for position in incident_block},
        marker='assert record["first_true"]',
    )
    case(
        "first-true-boundary-below-the-corruption-end-on-every-arm",
        campaign={position: archive.edited(position, first_true=lowered_first_true) for position in incident_block},
        marker='assert int(record["first_true"]) >= int(record["corruption_end"])',
    )

    # The convergence distance is the one archived figure the manuscript reads as signed, and it is the
    # column a sign check is most tempting to except. It is not excepted: the archived point is a position
    # in the repaired stream, the signed distance is derived from it, and a negative position is corruption
    # like any other.
    case(
        "completed-row-negative-convergence-point",
        campaign={completed_certified: archive.edited(completed_certified, converged="-1")},
        marker="('converged', '-1')",
    )

    # The answer floor, on the arm that carries no evidence to floor instead. The oracle-floored arms are
    # told where the damage ended, so their answers cannot lie below it, and the exact-clean arm is where
    # that fact stands alone: it archives an answer and nothing else the floor could be read off.
    case(
        "exact-clean-answer-below-the-corruption-end",
        campaign={exact_clean_answered: archive.edited(exact_clean_answered, first=exact_clean_below)},
        marker=('assert int(record["first"]) >= floor', ", 'exact-clean')"),
    )

    # Damage geometry on an absorbed draw. An absorbed row carries the damage coordinates and nothing
    # else, so before the geometry was read ahead of the absorbed rows' early exit this shift was the one
    # corruption an absorbed row could carry undetected: the emptiness check has nothing to say about a
    # populated coordinate, and the row leaves the loop before any later guard sees it. The strategy in
    # the reported tuple is what shows the rejection came from an absorbed row rather than an arm row.
    case(
        "absorbed-substitution-corruption-end-off-the-span",
        campaign={absorbed_substitute: archive.edited(absorbed_substitute, corruption_end=absorbed_end_shifted)},
        marker=('assert int(record["corruption_end"]) == int(record["p"]) + int(record["k"])', "'absorbed')"),
    )

    # The decider's own answer is a position in the stream, found by a search that starts at the blind
    # anchor, so it cannot lie at or below the failure offset. Zero is written into all eleven arms of a
    # repairable incident at once, so the arms agree perfectly and the cross-arm equality guard has
    # nothing to report; the repairability dependency is satisfied too, since a zero is a value and the
    # dependency reads presence rather than magnitude. Only the anchor floor is left to object.
    case(
        "decider-answer-at-zero-on-every-arm-of-one-incident",
        campaign={position: archive.edited(position, exact_at_anchor="0") for position in repairable_block},
        marker='assert int(record["exact_at_anchor"]) >= int(record["failure_offset"]) + 1',
    )

    # The terminal answer is an answer, so it obeys the same floor its arm searched under. The exact-clean
    # arm is where that stands alone again: floored at the corruption end, carrying no evidence, and its
    # first answer left where the archive put it, so the first answer's own floor check passes and the
    # terminal's is the only one that can fire.
    case(
        "exact-clean-terminal-below-the-corruption-end",
        campaign={exact_clean_answered: archive.edited(exact_clean_answered, terminal=exact_clean_below)},
        marker=('assert int(record["terminal"]) >= floor', ", 'exact-clean')"),
    )

    # The order between the two answers, and the two ways the attempt count constrains it. A row that
    # answered more than once must end past where it began, so a terminal below the first answer is
    # corruption even when it clears the floor, and a terminal equal to the first is corruption because
    # nothing advanced; a row that answered once must end exactly where it began, position and landing
    # flag alike. All four run on arms the sidecar knows nothing about, or on the campaign row alone, so
    # no reconciliation between the two files stands between the mutation and the contract it aims at.
    case(
        "multi-attempt-terminal-below-the-first-answer",
        campaign={
            noncertified_multi_attempt: archive.edited(
                noncertified_multi_attempt, terminal=noncertified_terminal_below_first
            )
        },
        marker=('assert int(record["terminal"]) >= int(record["first"])', f", '{noncertified_multi_arm}')"),
    )
    case(
        "multi-attempt-terminal-equal-to-the-first-answer",
        campaign={
            noncertified_multi_attempt: archive.edited(
                noncertified_multi_attempt, terminal=noncertified_terminal_at_first
            )
        },
        marker=('assert int(record["terminal"]) > int(record["first"])', f", '{noncertified_multi_arm}')"),
    )
    case(
        "single-attempt-terminal-past-the-first-answer",
        campaign={attempts_one: archive.edited(attempts_one, terminal=attempts_one_terminal_raised)},
        marker=('assert record["terminal"] == record["first"]', f", '{attempts_one_arm}')"),
    )
    case(
        "single-attempt-terminal-landing-differs-from-the-first",
        campaign={
            attempts_one_flagged: archive.edited(attempts_one_flagged, terminal_landed=attempts_one_flag_flipped)
        },
        marker=('assert record["terminal_landed"] == record["first_landed"]', f", '{attempts_one_flagged_arm}')"),
    )

    # Spending the whole budget is what capped means, so a refusal that claims the whole budget is naming
    # the wrong outcome for what it did. The row chosen proposed more than once, so its terminal answer
    # already lies past its first and the advance the new count implies is already archived; the budget
    # contract is then the only thing the relabeled count breaks.
    case(
        "refused-row-claiming-the-whole-attempt-budget",
        campaign={refused_multi_attempt: archive.edited(refused_multi_attempt, attempts="100")},
        marker=(
            'assert not (record["outcome"] == "refused" and record["attempts"] == "100")',
            f", '{refused_multi_arm}')",
        ),
    )

    # The sidecar's own advance. A walk that resumes one past its predecessor cannot produce a move whose
    # evidence begins where the previous move answered, and the case puts it exactly there, one byte short
    # of the floor the guard reads. The far end moves with it, so the interval still holds the move's
    # answer and still fits the searched widths; the move is an interior one, so the terminal
    # reconciliation reads a different line; and the interval stays on the side of the corruption end it
    # was already on, so the recounted covered tally still matches the archived one.
    case(
        "sidecar-move-evidence-back-on-the-previous-answer",
        sidecar={
            advance_move: archive.sidecar_edited(advance_move, evidence_begin=advance_begin, evidence_end=advance_end)
        },
        marker=("assert begin > move_last[(key, arm)]", ", 'certified', 1)"),
    )

    # The oracle-floored decider searches ground the damage never touched, so its answers land, the first
    # and the last alike. Both flags are unlanded at once: the row answered once, and a single attempt
    # forces the two flags to agree, so unlanding one alone would be caught by that agreement rather than
    # by the landing contract the case aims at. Nothing else on the row moves, and the arm carries no
    # evidence and no sidecar moves, so no reconciliation stands in the way either.
    case(
        "exact-clean-answer-unlanded-on-both-flags",
        campaign={exact_clean_landed: archive.edited(exact_clean_landed, first_landed="0", terminal_landed="0")},
        marker=('assert record["first_landed"] == "1" and record["terminal_landed"] == "1"', ", 'exact-clean')"),
    )

    # A completed incident converges at or past the answer it started from, so a convergence point one byte
    # below the first answer is corruption. The value stays a canonical nonnegative integer, so the
    # spelling guard has nothing to say, and it stays populated on a completed row, so the outcome
    # dependency is satisfied too; only the order between the two positions is broken.
    case(
        "completed-row-converges-below-its-first-answer",
        campaign={completed_at_anchor: archive.edited(completed_at_anchor, converged=converged_below_first)},
        marker=('assert int(record["converged"]) >= int(record["first"])', f", '{completed_at_anchor_arm}')"),
    )

    # The archived decider answer and the exact arm's first answer are the same query asked once, so where
    # both exist they agree. The arm's answer is lowered a byte and the archived answer is left alone. The
    # exact arm archives a single attempt on every row of this campaign, so its terminal answer is lowered
    # with its first, as the one-attempt rule requires; the lowered answer still clears the arm's blind
    # floor, the row's convergence point still lies at or past it, and the sidecar knows nothing about this
    # arm. Only the agreement between the two columns is left to object.
    case(
        "exact-arm-answer-disagrees-with-the-archived-decider-answer",
        campaign={
            exact_direct_answer: archive.edited(
                exact_direct_answer, first=exact_answer_lowered, terminal=exact_answer_lowered
            )
        },
        marker=("assert direct == exact_first", f", '{direct_answer}', '{exact_answer_lowered}')"),
    )

    # A convergence point that does not reach past the corruption end leaves the divergence region empty,
    # and an empty region holds no boundaries at all, so neither a lost nor a spurious one can be counted
    # in it. The two cases put a count on each side of that guard in turn, on two different rows, since the
    # guard is a single assertion whose source text they share and the row it reports is what tells them
    # apart. Both counts stay canonical nonnegative integers and both rows stay completed, so neither the
    # spelling guard nor the outcome dependency can be what objects.
    case(
        "completed-row-counts-a-lost-boundary-in-an-empty-divergence-region",
        campaign={empty_region: archive.edited(empty_region, lost="1")},
        marker=(
            'assert record["lost"] == "0" and record["spurious"] == "0"',
            archive.tuple_of(empty_region),
        ),
    )
    case(
        "completed-row-counts-a-spurious-boundary-in-an-empty-divergence-region",
        campaign={empty_region_second: archive.edited(empty_region_second, spurious="1")},
        marker=(
            'assert record["lost"] == "0" and record["spurious"] == "0"',
            archive.tuple_of(empty_region_second),
        ),
    )

    # The mechanism companion reads the same archive, so several cases are aimed at that program rather
    # than at the auditing analyzer: a second program deriving manuscript figures from a corrupted archive
    # would be a hole in the audit the first program's guards say nothing about. This one relabels byte-shaped
    # evidence as a window, a kind the domain admits, so the width law is the only thing that can see the
    # contradiction between the name and the one-byte interval it stands for. The marker names the
    # companion's own file besides its guard, since the two programs are what these cases tell apart.
    case(
        "mechanism-refuses-a-window-kind-on-byte-shaped-evidence",
        campaign={certified_byte: archive.edited(certified_byte, evidence_kind=byte_row_kind_flipped)},
        program=MECHANISM,
        marker=(MECHANISM, 'assert (kind == "byte") == (width == 1), row'),
    )

    # No input this campaign reads approaches sixteen mebibytes, so a coordinate past that bound is a
    # corrupted field rather than a large run. The absorbed row carries the damage coordinates and nothing
    # else, and both of them move together onto the bound, so the operation's geometry still holds and the
    # row is still an absorbed row in every other respect: only the bound is left to object, and it objects
    # at the damage start, the first position column the spelling and bound pass reads.
    case(
        "absorbed-row-position-past-the-sixteen-mebibyte-bound",
        campaign={
            absorbed_substitute: archive.edited(
                absorbed_substitute, p=absorbed_beyond_bound, corruption_end=absorbed_beyond_end
            )
        },
        marker=f"('p', '{absorbed_beyond_bound}')",
    )

    # Each attempt past the first advances the answer by at least one byte, so the distance between the
    # first and terminal answers bounds the attempt count from below. The count is raised one past what
    # that distance allows, on an arm the sidecar knows nothing about, outside the capped outcome whose
    # count is pinned to the whole budget and well inside the budget itself, so neither reconciliation nor
    # the budget contract can be what objects.
    case(
        "attempts-past-the-advance-the-answers-allow",
        campaign={attempts_advance: archive.edited(attempts_advance, attempts=attempts_past_advance)},
        marker=(
            'assert int(record["terminal"]) - int(record["first"]) >= int(record["attempts"]) - 1',
            archive.tuple_of(attempts_advance),
        ),
    )

    # A landed answer at or past the corruption end sits on a mapped boundary of the repaired region, so
    # the first mapped boundary of that region cannot lie past it. The boundary is raised one byte past the
    # furthest such answer the incident archives, on all eleven arms at once, so the arms agree perfectly
    # and the cross-arm comparison sees nothing; the boundary still lies at or past the corruption end, so
    # the guard that floors it there is satisfied too, and the row the guard reports is the incident's
    # first landed covered answer.
    case(
        "first-true-boundary-past-a-landed-covered-answer-on-every-arm",
        campaign={position: archive.edited(position, first_true=raised_first_true) for position in incident_block},
        marker=(
            'assert int(record["first_true"]) <= int(record["first"])',
            archive.tuple_of(landed_covered_arms[0]),
        ),
    )

    # The generic bound is wide enough to admit coordinates no corpus in this campaign can carry, so every
    # coordinate is held to the source its own grammar runs on besides. The three cases below move an
    # absorbed draw's damage bodily, keeping the span equation the geometry guard reads, so the row is
    # corrupt in exactly one respect: it names a place its own source does not have. The first is the wild
    # coordinate, past the outer bound as well, and the outer bound is what refuses it, which is the point of
    # keeping the case: it fixes what that bound does and does not settle. The second stays deep inside the
    # outer bound and lands past a half-mebibyte corpus, where the outer bound has nothing to say and the
    # source length derived for the row's grammar is the only guard left. The third breaks the insertion
    # bound, which is one byte wider than the span operations' because an insertion consumes nothing and may
    # sit at the source's end.
    case(
        "absorbed-substitution-moved-to-a-wild-coordinate",
        campaign={absorbed_substitute: archive.edited(absorbed_substitute, p=wild_start, corruption_end=wild_end)},
        marker=("assert int(value) < POSITION_BOUND", f"('p', '{wild_start}')"),
    )
    case(
        "absorbed-substitution-span-past-its-grammar-source",
        campaign={
            absorbed_substitute: archive.edited(
                absorbed_substitute, p=past_corpus_start, corruption_end=past_corpus_end
            )
        },
        marker=(
            'assert int(record["p"]) + int(record["k"]) <= source_size',
            f", 'absorbed', '{past_corpus_start}')",
        ),
    )
    case(
        "absorbed-insertion-seam-past-its-grammar-source",
        campaign={
            absorbed_insert: archive.edited(
                absorbed_insert, p=insert_past_corpus_start, corruption_end=insert_past_corpus_end
            )
        },
        marker=('assert int(record["p"]) <= source_size', f", 'absorbed', '{insert_past_corpus_start}')"),
    )

    # Every coordinate but the damage start indexes the damaged input rather than the source, so the length
    # they are held to is the one the operation leaves behind. The terminal answer is moved past a
    # half-mebibyte row's damaged input, deep inside the outer bound again, on an arm the sidecar knows
    # nothing about; and a deletion incident's mapped boundary is put at exactly the undamaged source length,
    # which a substitution could carry and a deletion of k bytes cannot, so the case passes only if the
    # length was derived per operation rather than taken from the source. That one is written into all eleven
    # arms of the incident, the shared column's own convention, so the arms agree perfectly and the guard
    # reports the first of them.
    case(
        "terminal-answer-past-the-damaged-input-length",
        campaign={noncertified_answered: archive.edited(noncertified_answered, terminal=past_corpus_start)},
        marker=(
            "assert int(record[field]) <= damaged_size",
            f", '{noncertified_arm}', 'terminal', '{past_corpus_start}')",
        ),
    )
    case(
        "delete-incident-boundary-at-the-undeleted-source-length",
        campaign={
            position: archive.edited(position, first_true=undeleted_source_length) for position in delete_block
        },
        marker=(
            "assert int(record[field]) <= damaged_size",
            f", 'first_true', '{undeleted_source_length}')",
        ),
    )

    # The three row-level facts above hold whatever the routine's label says, so each is staged a second
    # time on an incident labeled beyond repair, where the decider's anchor query returned nothing. These
    # are the cases the earlier suite could not have failed: it chose every one of its targets by that
    # same anchor query, so it exercised only the rows where the guards were reached at all.
    case(
        "beyond-repair-row-converges-below-its-first-answer",
        campaign={
            unrepairable_completed: archive.edited(unrepairable_completed, converged=unrepairable_converged_below)
        },
        marker=(
            'assert int(record["converged"]) >= int(record["first"])',
            archive.tuple_of(unrepairable_completed),
        ),
    )
    case(
        "beyond-repair-row-counts-a-lost-boundary-in-an-empty-divergence-region",
        campaign={unrepairable_empty_region: archive.edited(unrepairable_empty_region, lost="1")},
        marker=(
            'assert record["lost"] == "0" and record["spurious"] == "0"',
            archive.tuple_of(unrepairable_empty_region),
        ),
    )
    case(
        "beyond-repair-exact-clean-answer-unlanded-on-both-flags",
        campaign={
            unrepairable_exact_clean_landed: archive.edited(
                unrepairable_exact_clean_landed, first_landed="0", terminal_landed="0"
            )
        },
        marker=(
            'assert record["first_landed"] == "1" and record["terminal_landed"] == "1"',
            archive.tuple_of(unrepairable_exact_clean_landed),
        ),
    )

    # The decider arms answer on every incident, and the guard must be shown to read the row rather
    # than the anchor column or the arm name: the oracle-floored arm rewritten into a refusal, and the
    # anchored arm on an incident the routine labels beyond repair, each staged as a coherent refusal.
    case(
        "exact-clean-arm-rewritten-into-a-refusal",
        campaign={exact_clean_answered: archive.edited(exact_clean_answered, **pair_refusal_fields)},
        marker=("assert answered, (key", ", 'exact-clean')"),
    )
    case(
        "beyond-repair-exact-arm-rewritten-into-a-refusal",
        campaign={
            unrepairable_exact_row: archive.edited(unrepairable_exact_row, **pair_refusal_fields)
        },
        marker=("assert answered, (key", archive.tuple_of(unrepairable_exact_row)),
    )

    # The decider's anchor query and the exact arm's first answer are one query asked once, so an archived
    # anchor answer beside an arm that never proposed is a contradiction. The arm is rewritten into a
    # refusal that is coherent in every other respect, so the dependency guards have nothing to say and the
    # co-presence of the two columns is what is left to object.
    case(
        "archived-decider-answer-beside-an-exact-arm-that-refused",
        campaign={exact_direct_answer: archive.edited(exact_direct_answer, **refused_exact_fields)},
        marker=("assert answered, (key", archive.tuple_of(exact_direct_answer)),
    )

    # The membership-erasure shapes reports drove through: an arm relabeled refused with every
    # answer field blanked leaves the divergence groups entirely, and coherently. What objects is
    # the harness reconciliation, three exact per-cell identities over all eleven arms: answers
    # count the rows carrying a first answer and equally the rows that attempted, initial refusals
    # the refusals that never attempted, terminal refusals the refused rows outright. Each staged
    # shape below was demonstrated by a report and is pinned at its own identity: the single token
    # arms of both families, the semicolon pair erased together so the pair laws stay silent, all
    # three excluded-family arms at once, and a multi-attempt relabel that keeps its first answer
    # and attempts so only the outcome moves.
    case(
        "token-arm-erasing-its-own-membership",
        campaign={membership_token_newline: archive.edited(
            membership_token_newline, **refused_exact_fields)},
        marker=("cell_first_present.get(summary_cell, 0) == answers", "'token-newline'"),
    )
    case(
        "token-semicolon-arm-erasing-its-own-membership",
        campaign={membership_token_semicolon: archive.edited(
            membership_token_semicolon, **refused_exact_fields)},
        marker=("cell_first_present.get(summary_cell, 0) == answers", "'token-semicolon'"),
    )
    case(
        "semicolon-pair-erasing-both-memberships-together",
        campaign={
            membership_semicolon_pair: archive.edited(
                membership_semicolon_pair, **refused_exact_fields),
            archive.arm_row_of(membership_semicolon_pair, "semicolon-at"): archive.edited(
                archive.arm_row_of(membership_semicolon_pair, "semicolon-at"),
                **refused_exact_fields),
        },
        marker=("cell_first_present.get(summary_cell, 0) == answers", "'1', 'semicolon')"),
    )
    case(
        "all-three-excluded-family-arms-erased-together",
        campaign={
            membership_all_three: archive.edited(membership_all_three, **refused_exact_fields),
            archive.arm_row_of(membership_all_three, "semicolon-at"): archive.edited(
                archive.arm_row_of(membership_all_three, "semicolon-at"), **refused_exact_fields),
            archive.arm_row_of(membership_all_three, "token-semicolon"): archive.edited(
                archive.arm_row_of(membership_all_three, "token-semicolon"), **refused_exact_fields),
        },
        marker=("cell_first_present.get(summary_cell, 0) == answers", "'4', 'semicolon')"),
    )
    case(
        "multi-attempt-semicolon-relabeled-refused-with-its-answer-standing",
        campaign={semicolon_multi_attempt: archive.edited(
            semicolon_multi_attempt, outcome="refused", converged="", lost="", spurious="")},
        marker=("cell_refused.get(summary_cell, 0) == terminal_refusals",),
    )

    # An answer standing exactly on the oracle's first mapped boundary is on a boundary of the pristine
    # mapping by that very fact, so its landing flag cannot be clear. The row answered more than once, so
    # the single-attempt rule that ties the two flags together is not what objects, and the arm carries no
    # evidence and no sidecar moves, so no reconciliation does either.
    case(
        "terminal-answer-on-the-mapped-boundary-flagged-unlanded",
        campaign={terminal_on_first_true: archive.edited(terminal_on_first_true, terminal_landed="0")},
        marker=('assert record[flag] == "1"', f", '{terminal_on_first_true_arm}', 'terminal')"),
    )

    # The mechanism companion reads the certificate kind against the width of the interval it describes,
    # and that equivalence has both sides false for an unknown kind on any multi-byte evidence, so the
    # kind's own domain is asserted before it. The byte-shaped case above is kept because it fixes what the
    # width law settles by itself; this one is aimed at the half of the domain the law cannot see.
    case(
        "mechanism-refuses-an-unknown-kind-on-window-shaped-evidence",
        campaign={certified_window: archive.edited(certified_window, evidence_kind="bogus")},
        program=MECHANISM,
        marker=(MECHANISM, 'assert kind in ("byte", "window"), row'),
    )

    # The sidecar's coordinates index the damaged input exactly as the campaign's do, so they are held to
    # the same length. The terminal move is slid to the input's far end, and the archived terminal answer
    # is slid with it, since the two are reconciled against each other. The answer stops exactly at the
    # damaged input's length, which the campaign bound admits and which leaves the move's evidence end one
    # byte outside the input as the only coordinate left to object; the move stays covered, so the
    # recounted covered tally still matches the archived one.
    case(
        "sidecar-move-evidence-past-the-damaged-input-length",
        campaign={
            certified_generated_terminal: archive.edited(certified_generated_terminal, terminal=move_answer_inside)
        },
        sidecar={
            terminal_move: archive.sidecar_edited(
                terminal_move,
                answer=move_answer_inside,
                evidence_begin=move_answer_inside,
                evidence_end=move_past_input_end,
            )
        },
        marker=("assert end <= move_damaged_size", f", {terminal_damaged_size})"),
    )

    # Lost and spurious boundaries are disjoint positions inside the divergence region, so their sum
    # is bounded by the region's width. One case per count and per side of the repairability label,
    # since a bound reached through the anchor column would be the round-old mistake repeated.
    case(
        "completed-row-counting-more-lost-boundaries-than-its-region-holds",
        campaign={span_room: archive.edited(span_room, lost=lost_past_region)},
        marker=(
            'assert int(record["lost"]) + int(record["spurious"]) <= region',
            archive.tuple_of(span_room)[:-1],
        ),
    )
    case(
        "beyond-repair-row-counting-more-spurious-boundaries-than-its-region-holds",
        campaign={unrepairable_span_room: archive.edited(unrepairable_span_room, spurious=spurious_past_region)},
        marker=(
            'assert int(record["lost"]) + int(record["spurious"]) <= region',
            archive.tuple_of(unrepairable_span_room)[:-1],
        ),
    )

    # The oracle maps no boundary into a span operation's damaged window, so an answer inside it
    # cannot land, and the only corruption a flag there admits is the upward flip staged here.
    case(
        "answer-inside-the-damaged-window-flagged-landed",
        campaign={window_interior_answer: archive.edited(window_interior_answer, first_landed="1")},
        marker=('assert record[flag] == "0"', archive.tuple_of(window_interior_answer)[:-1], "'first')"),
    )

    # The skip arm's resume is always its own start, so it can never refuse; and no landing flag
    # exists for an answer at the damaged input's very end, where the harness computes none.
    case(
        "skip-row-answering-past-its-own-definition",
        campaign={
            skip_answered: archive.edited(skip_answered, first=str(int(archive.field(skip_answered, "first")) + 1))
        },
        marker=(
            'assert int(record["first"]) == int(record["failure_offset"]) + 1',
            archive.tuple_of(skip_answered)[:-1],
        ),
    )
    case(
        "beyond-repair-skip-row-answering-past-its-own-definition",
        campaign={
            skip_answered_beyond: archive.edited(
                skip_answered_beyond, first=str(int(archive.field(skip_answered_beyond, "first")) + 1)
            )
        },
        marker=(
            'assert int(record["first"]) == int(record["failure_offset"]) + 1',
            archive.tuple_of(skip_answered_beyond)[:-1],
        ),
    )
    case(
        "skip-row-relabeled-as-a-refusal",
        campaign={
            skip_completed: archive.edited(skip_completed, outcome="refused", converged="", lost="", spurious="")
        },
        marker=('assert record["outcome"] != "refused"', archive.tuple_of(skip_completed)),
    )
    case(
        "terminal-answer-at-the-damaged-end-carrying-a-flag",
        campaign={skip_multi: archive.edited(skip_multi, terminal=skip_damaged_size)},
        marker=('assert record[flag] == ""', archive.tuple_of(skip_multi)[:-1], "'terminal_landed')"),
    )

    # The terminal half of the mapped-boundary bound: a landed covered terminal lowered beneath the
    # incident's first mapped boundary, everything else untouched.
    case(
        "landed-terminal-below-the-mapped-boundary",
        campaign={
            terminal_below_boundary: archive.edited(terminal_below_boundary, terminal=boundary_below_terminal)
        },
        marker=(
            'assert int(record["first_true"]) <= int(record["terminal"])',
            archive.tuple_of(terminal_below_boundary),
        ),
    )

    # The first-answer half of the boundary landing rule: an answer standing exactly on the mapped
    # boundary with both flags cleared at once, the single-attempt tie kept.
    case(
        "first-answer-on-the-mapped-boundary-flagged-unlanded",
        campaign={
            boundary_first_landed: archive.edited(boundary_first_landed, first_landed="0", terminal_landed="0")
        },
        marker=('assert record[flag] == "1"', archive.tuple_of(boundary_first_landed)[:-1], "'first')"),
    )

    # The two placements of one delimiter convention are one search reported twice, so their rows
    # answer together and their first answers differ by exactly one byte. One case copies the past
    # placement's answer onto the at placement; the other rewrites the at placement into a refusal
    # while its partner keeps answering.
    case(
        "at-placement-answer-copied-from-its-past-partner",
        campaign={pair_at_with_room: archive.edited(pair_at_with_room, first=copied_past_first)},
        marker=('assert int(at["first"]) == int(past["first"]) - 1', "'newline'"),
    )
    case(
        "at-placement-spending-an-attempt-its-partner-does-not",
        campaign={
            pair_attempts_room: archive.edited(
                pair_attempts_room, attempts=str(int(archive.field(pair_attempts_room, "attempts")) + 1)
            )
        },
        marker=('assert int(at["attempts"]) == int(past["attempts"]) + extra', "'newline'"),
    )
    case(
        "refused-row-ending-at-the-damaged-input-end",
        campaign={
            refused_room_to_eof: archive.edited(
                refused_room_to_eof,
                terminal=str(GENERATED_SOURCE_BYTES
                             + DAMAGED_LENGTH_DELTA[archive.field(refused_room_to_eof, "op")]
                             * int(archive.field(refused_room_to_eof, "k"))),
                terminal_landed="",
            )
        },
        marker=('assert record["outcome"] == "completed"', archive.tuple_of(refused_room_to_eof)[:-1]),
    )
    case(
        "capped-row-ending-at-the-damaged-input-end",
        campaign={
            capped_room_to_eof: archive.edited(
                capped_room_to_eof,
                terminal=str((REAL_DOCUMENT_SOURCE_BYTES
                              if archive.field(capped_room_to_eof, "grammar") == REAL_DOCUMENT_GRAMMAR
                              else GENERATED_SOURCE_BYTES)
                             + DAMAGED_LENGTH_DELTA[archive.field(capped_room_to_eof, "op")]
                             * int(archive.field(capped_room_to_eof, "k"))),
                terminal_landed="",
            )
        },
        marker=('assert record["outcome"] == "completed"', archive.tuple_of(capped_room_to_eof)[:-1]),
    )
    case(
        "at-placement-terminal-outside-the-pair-law",
        campaign={pair_terminal_room: archive.edited(pair_terminal_room, terminal=pair_terminal_outside)},
        marker=("assert int(at[\"terminal\"]) in (int(past[\"terminal\"]) - 1, int(past[\"terminal\"]))",
                "'newline'"),
    )
    case(
        "semicolon-at-answer-copied-from-its-past-partner",
        campaign={
            semicolon_at_with_room: archive.edited(semicolon_at_with_room, first=semicolon_copied_first)
        },
        marker=('assert int(at["first"]) == int(past["first"]) - 1', "'semicolon'"),
    )
    case(
        "at-placement-refuses-while-its-past-partner-answers",
        campaign={pair_at_answered: archive.edited(pair_at_answered, **pair_refusal_fields)},
        marker=('assert bool(past["first"]) == bool(at["first"])', "'newline'"),
    )

    # The semicolon family staged under every pair law its newline twin already carries, so the staging
    # claim can name both families: the terminal lowered out of the two-value set, the attempts count
    # raised past its reconciliation, and the at placement rewritten into a refusal beside an answering
    # partner. The equal-terminal attempts branch, which the archive holds only rarely, gets its own
    # case through a pair already on that branch.
    case(
        "semicolon-at-terminal-outside-the-pair-law",
        campaign={
            semicolon_terminal_room: archive.edited(
                semicolon_terminal_room, terminal=semicolon_terminal_outside)
        },
        marker=("assert int(at[\"terminal\"]) in (int(past[\"terminal\"]) - 1, int(past[\"terminal\"]))",
                "'semicolon'"),
    )
    case(
        "semicolon-at-spending-an-attempt-its-partner-does-not",
        campaign={
            semicolon_attempts_room: archive.edited(
                semicolon_attempts_room,
                attempts=str(int(archive.field(semicolon_attempts_room, "attempts")) + 1))
        },
        marker=('assert int(at["attempts"]) == int(past["attempts"]) + extra', "'semicolon'"),
    )
    case(
        "semicolon-at-refuses-while-its-past-partner-answers",
        campaign={semicolon_at_answered: archive.edited(semicolon_at_answered, **pair_refusal_fields)},
        marker=('assert bool(past["first"]) == bool(at["first"])', "'semicolon'"),
    )
    case(
        "semicolon-pair-attempts-broken-on-the-equal-terminal-branch",
        campaign={
            rare_semicolon_row: archive.edited(rare_semicolon_row, terminal=rare_shared_terminal)
        },
        marker=('assert int(at["attempts"]) == int(past["attempts"]) + extra',
                f"'{rare_incident_trial}'"),
    )

    # Decider totality staged on both arms across both repairability labels rather than proved for
    # three cells of that grid and assumed for the fourth: the exact-clean arm rewritten into an
    # otherwise coherent refusal on a repairable incident and on one labeled beyond repair. The exact
    # arm's two labels are staged above, by the archived-decider co-presence case and the beyond-repair
    # refusal case.
    case(
        "exact-clean-arm-rewritten-into-a-refusal-on-a-repairable-incident",
        campaign={exact_clean_landed: archive.edited(exact_clean_landed, **pair_refusal_fields)},
        marker=("assert answered, (key", archive.tuple_of(exact_clean_landed)),
    )
    case(
        "beyond-repair-exact-clean-arm-rewritten-into-a-refusal",
        campaign={
            unrepairable_exact_clean_landed: archive.edited(
                unrepairable_exact_clean_landed, **pair_refusal_fields)
        },
        marker=("assert answered, (key", archive.tuple_of(unrepairable_exact_clean_landed)),
    )

    # The direct query and the exact arm cover both of the law's branches: the tie is staged above by
    # moving an archived answer off its direct partner, and this case stages the complementary branch,
    # an arm claiming the blind floor its absent direct answer says refused.
    case(
        "exact-arm-answering-its-blind-floor-after-a-direct-refusal",
        campaign={
            unrepairable_exact_row: archive.edited(
                unrepairable_exact_row, first=floor_answer, terminal=floor_answer)
        },
        marker=("assert int(exact_first) > int", f"'{floor_answer}'"),
    )

    # The cross-arm reconciliations, staged in the shapes that once rode through: one arm's landing
    # flags flipped while a neighbour keeps answering the same coordinate landed; a pair pushed onto
    # equal terminals whose flags then disagree about one position, in both delimiter families; and a
    # single-attempt completed arm whose divergence triple parts from the neighbour it shares its
    # answer with.
    case(
        "exact-arm-landing-flags-flipped-against-a-sharing-neighbour",
        campaign={
            exact_shared_coordinate: archive.edited(
                exact_shared_coordinate, first_landed="0", terminal_landed="0")
        },
        marker=("assert seen_flag == flag", f"'{shared_coordinate}'"),
    )
    case(
        "newline-pair-equal-terminals-landing-apart",
        campaign={
            newline_landing_row: archive.edited(
                newline_landing_row,
                terminal=newline_landing_partner[archive.index["terminal"]],
                attempts=str(int(archive.field(newline_landing_row, "attempts")) + 1))
        },
        marker=("assert seen_flag == flag",
                f"'{newline_landing_partner[archive.index['terminal']]}'"),
    )
    case(
        "semicolon-pair-shared-boundary-terminal-claimed-unlanded",
        campaign={
            semicolon_boundary_row: archive.edited(
                semicolon_boundary_row,
                terminal=semicolon_boundary_partner[archive.index["terminal"]],
                attempts=str(int(archive.field(semicolon_boundary_row, "attempts")) + 1))
        },
        marker=('assert record[flag] == "1"',
                f"'{archive.field(semicolon_boundary_row, 'strategy')}', 'terminal')"),
    )
    case(
        "single-attempt-arm-diverging-from-its-coordinate-sharing-neighbour",
        campaign={
            exact_shared_coordinate: archive.edited(
                exact_shared_coordinate, converged=shared_converged_moved)
        },
        marker=("assert len(triples) == 1", f"'{shared_converged_moved}'"),
    )

    # Membership is validated before outcome filtering, staged in the exact shape one round accepted:
    # an arm sharing a completed single-attempt answer relabels itself refused, clears its divergence
    # fields coherently, and must be caught by the group guard rather than slipping out of the very
    # comparison meant to validate it. Staged on both oracle-floored arms, one on a plain incident and
    # one on a collapsed-floor incident, so neither label owns the law alone.
    case(
        "sharing-exact-clean-arm-opting-out-of-its-completed-group",
        campaign={
            membership_exact_clean: archive.edited(
                membership_exact_clean, outcome="refused", converged="", lost="", spurious="")
        },
        marker=('assert r["outcome"] == "completed" and r["attempts"] == "1"', "'exact-clean'"),
    )
    case(
        "sharing-certified-clean-arm-opting-out-on-a-collapsed-incident",
        campaign={
            membership_certified_clean: archive.edited(
                membership_certified_clean, outcome="refused", converged="", lost="", spurious="")
        },
        marker=('assert r["outcome"] == "completed" and r["attempts"] == "1"', "'certified-clean'"),
    )

    # The collapsed-floor identity, staged on a run field and on the ordered sidecar: where one past
    # the failure reaches the corruption end the two floors coincide, so the clean twin cannot part
    # from its arm on any field, and the certified pair cannot part on a single move.
    case(
        "collapsed-floor-exact-pair-parting-on-the-first-answer",
        campaign={
            identity_first: archive.edited(
                identity_first, first=identity_first_moved, terminal=identity_first_moved)
        },
        marker=("assert arms[one][field] == arms[twin][field]", "'first')"),
    )
    case(
        "collapsed-floor-certified-pair-parting-on-its-evidence",
        campaign={
            identity_evidence: archive.edited(
                identity_evidence, evidence_begin=identity_evidence_moved)
        },
        sidecar={
            identity_evidence_move: archive.sidecar_edited(
                identity_evidence_move, evidence_begin=identity_evidence_moved)
        },
        marker=("assert arms[one][field] == arms[twin][field]", "'evidence_begin'"),
    )
    case(
        "collapsed-floor-certified-move-owned-by-its-record-join",
        sidecar={
            identity_evidence_move: archive.sidecar_edited(
                identity_evidence_move, evidence_begin=identity_evidence_moved)
        },
        marker=('assert int(record["evidence_begin"]) == first_begin', "'certified-clean'"),
    )

    # The companion's searched width range, probed from both ends on window evidence: a five-byte
    # interval the search never produces, and an empty one. The width law alone sees neither, both of
    # its sides false, and the campaign auditor's own range does not travel to a second program.
    case(
        "mechanism-refuses-a-five-byte-window",
        campaign={certified_window: archive.edited(certified_window, evidence_end=window_end_stretched)},
        program=MECHANISM,
        marker=(MECHANISM, "assert 1 <= width <= 4, row"),
    )
    case(
        "mechanism-refuses-an-empty-window",
        campaign={certified_window: archive.edited(certified_window, evidence_end=window_end_collapsed)},
        program=MECHANISM,
        marker=(MECHANISM, "assert 1 <= width <= 4, row"),
    )

    # The companion's population closure, staged as the accepted shapes that were caughted before it
    # existed. Uniqueness alone let a deleted arm row pass, because a missing row repeats nothing;
    # the aggregate filter let a corrupted field ride in an arm no aggregate reads; and the certified
    # arm's landing flags were held to the whole file's domain, which admits blank, rather than to
    # the answers that arm always gives. Each is aimed at the companion, whose own refusal is the
    # thing on trial; the archive auditor catching the same corruption proves nothing about it.
    case(
        "mechanism-refuses-a-deleted-noncertified-arm-row",
        campaign={newline_arm: []},
        program=MECHANISM,
        marker=(MECHANISM, "arm set is neither the eleven recovery arms nor one absorbed draw"),
    )
    case(
        "mechanism-refuses-a-nonnumeric-noncertified-field",
        campaign={noncertified_answered: archive.edited(noncertified_answered, attempts="notanumber")},
        program=MECHANISM,
        marker=(MECHANISM, "attempts is neither blank nor a canonical integer"),
    )
    case(
        "mechanism-refuses-a-blank-certified-landing-flag",
        campaign={certified: archive.edited(certified, first_landed="")},
        program=MECHANISM,
        marker=(MECHANISM, "a certified row leaves first_landed blank"),
    )
    # The arm-set closure misses a whole incident deleted with every row it had, because a missing
    # key repeats nothing; the declared five hundred draws per cell close that. And negative zero
    # satisfies a naive signed integer class while being a number no arithmetic here emits, staged
    # once in a key column and once in a field no aggregate reads.
    case(
        "mechanism-refuses-a-deleted-absorbed-incident",
        campaign={absorbed_first: []},
        program=MECHANISM,
        marker=(MECHANISM, "trial identifiers are not exactly the declared zero through four"),
    )
    # Rekeying one absorbed draw onto an unused trial keeps the cell's count at five hundred while
    # its identifiers leave the schedule, so the wall is the exact identifier set, never the count.
    case(
        "mechanism-refuses-a-trial-rekeyed-off-the-schedule",
        campaign={absorbed_second: archive.edited(absorbed_second, trial="500")},
        program=MECHANISM,
        marker=(MECHANISM, "trial identifiers are not exactly the declared zero through four"),
    )
    case(
        "mechanism-refuses-a-negative-zero-trial",
        campaign={certified: archive.edited(certified, trial="-0")},
        program=MECHANISM,
        marker=(MECHANISM, "trial is neither blank nor a canonical integer"),
    )
    case(
        "mechanism-refuses-a-negative-zero-ignored-field",
        campaign={clean_arm: archive.edited(clean_arm, attempts="-0")},
        program=MECHANISM,
        marker=(MECHANISM, "attempts is neither blank nor a canonical integer"),
    )

    # The damaged length is derived per operation, so the substitution-staged case above proves
    # nothing about deletion or insertion; one case per remaining operation slides that operation's
    # own terminal move to its own input length.
    case(
        "sidecar-move-past-the-deletion-shortened-input",
        campaign={
            sidecar_op_delete: archive.edited(sidecar_op_delete, terminal=str(delete_size - 1))
        },
        sidecar={
            delete_move: archive.sidecar_edited(
                delete_move,
                answer=str(delete_size - 1),
                evidence_begin=str(delete_size - 1),
                evidence_end=str(delete_size + 1),
            )
        },
        marker=("assert end <= move_damaged_size", ", %d)" % delete_size),
    )
    case(
        "sidecar-move-past-the-insertion-lengthened-input",
        campaign={
            sidecar_op_insert: archive.edited(sidecar_op_insert, terminal=str(insert_size - 1))
        },
        sidecar={
            insert_move: archive.sidecar_edited(
                insert_move,
                answer=str(insert_size - 1),
                evidence_begin=str(insert_size - 1),
                evidence_end=str(insert_size + 1),
            )
        },
        marker=("assert end <= move_damaged_size", ", %d)" % insert_size),
    )

    # The source lengths are pinned per grammar, so a row naming a grammar the mapping does not carry has no
    # length to be bounded by at all. The audit refuses it there rather than reading on with no bound, which
    # is where the schedule grid would otherwise catch it, several hundred thousand rows later.
    case(
        "row-names-a-grammar-with-no-source-length",
        campaign={ordinary: archive.edited(ordinary, grammar=UNKNOWN_GRAMMAR)},
        marker=('assert record["grammar"] in GRAMMAR_SOURCE_BYTES', UNKNOWN_GRAMMAR),
    )

    assert certified_covered_value not in ("", "0"), certified_covered_value
    assert archive.field(absorbed_substitute, "op") == "substitute", absorbed_substitute
    assert archive.field(certified_covered_first, "attempts") == "1", certified_covered_first
    assert archive.field(attempts_one, "attempts") == "1", attempts_one
    assert archive.field(attempts_one_flagged, "attempts") == "1", attempts_one_flagged
    assert archive.field(refused_multi_attempt, "outcome") == "refused", refused_multi_attempt
    assert archive.field(refused_multi_attempt, "attempts") != "100", refused_multi_attempt
    assert archive.field(repairable_block[0], "repairable") == "1", repairable_block[0]
    assert archive.field(delete_block[0], "op") == "delete", delete_block[0]
    assert archive.field(capped_row, "outcome") == "capped", capped_row
    assert archive.field(exact_clean_landed, "strategy") == "exact-clean", exact_clean_landed
    assert archive.field(exact_direct_answer, "strategy") == "exact", exact_direct_answer
    assert archive.field(exact_direct_answer, "attempts") == "1", exact_direct_answer
    assert direct_answer == archive.field(exact_direct_answer, "first"), exact_direct_answer
    assert archive.field(exact_direct_answer, "terminal") == direct_answer, exact_direct_answer
    assert archive.field(certified_byte, "evidence_kind") == "byte", certified_byte
    assert archive.field(certified_window, "evidence_kind") == "window", certified_window
    assert not archive.field(unrepairable_completed, "exact_at_anchor"), unrepairable_completed
    assert not archive.field(unrepairable_empty_region, "exact_at_anchor"), unrepairable_empty_region
    assert not archive.field(unrepairable_exact_clean_landed, "exact_at_anchor"), unrepairable_exact_clean_landed
    assert archive.field(unrepairable_exact_clean_landed, "strategy") == "exact-clean", unrepairable_exact_clean_landed
    assert archive.field(unrepairable_completed, "outcome") == "completed", unrepairable_completed
    assert archive.field(unrepairable_empty_region, "lost") == "0", unrepairable_empty_region
    assert direct_without_answer, exact_direct_answer
    assert archive.field(terminal_on_first_true, "terminal") == archive.field(
        terminal_on_first_true, "first_true"
    ), terminal_on_first_true
    assert int(archive.field(terminal_on_first_true, "attempts")) >= 2, terminal_on_first_true
    assert archive.field(certified_generated_terminal, "grammar") != REAL_DOCUMENT_GRAMMAR, certified_generated_terminal
    assert int(archive.field(certified_generated_terminal, "terminal")) < terminal_damaged_size - 1, terminal_damaged_size
    assert archive.field(empty_region, "lost") == "0", empty_region
    assert archive.field(empty_region_second, "spurious") == "0", empty_region_second
    assert empty_region != empty_region_second, empty_region
    assert int(archive.field(absorbed_substitute, "p")) < POSITION_BOUND, absorbed_substitute
    assert archive.field(absorbed_substitute, "k") == "1", absorbed_substitute
    assert archive.field(absorbed_insert, "op") == "insert", absorbed_insert
    assert archive.field(absorbed_insert, "strategy") == "absorbed", absorbed_insert
    assert UNKNOWN_GRAMMAR != archive.field(ordinary, "grammar"), UNKNOWN_GRAMMAR
    assert GENERATED_SOURCE_BYTES < PAST_CORPUS_COORDINATE < POSITION_BOUND < WILD_COORDINATE, POSITION_BOUND
    # The three cases that reach past a corpus are staged on generated rows, which are the shorter ones: on
    # the real-world row the coordinates they write would be inside the source and nothing would object.
    for position in (absorbed_substitute, absorbed_insert, noncertified_answered, delete_block[0]):
        assert archive.field(position, "grammar") != REAL_DOCUMENT_GRAMMAR, position
    assert archive.field(attempts_advance, "outcome") != "capped", attempts_advance
    assert int(archive.field(attempts_advance, "attempts")) >= 2, attempts_advance
    assert landed_covered_arms, incident_block[0]
    assert archive.field(certified_wide_advance, "outcome") == "completed", certified_wide_advance
    assert archive.field(certified_wide_advance, "strategy") == certified_arm_name, certified_wide_advance
    # The renamed cell must not be a deletion cell: a deletion's corruption end is the deletion point,
    # and a row renamed out of that operation would be caught by the geometry rather than by the grid.
    assert archive.field(cell_block[0], "op") != "delete", cell_block[0]
    assert UNKNOWN_OPERATION not in ("substitute", "insert", "delete"), UNKNOWN_OPERATION

    # The five certification cases the sixth round demanded: the two membership transfers only the
    # cell commitments can see, the capped relabel only the summary's exact capped column can see,
    # and the two summary corruptions the exact schema and cell domain refuse. The transfer rows are
    # located by their keys and the completion's fields are taken from the incident's own exact arm,
    # the reported construction reproduced rather than approximated.
    def row_position(grammar, op, k, seed, trial, strategy):
        prefix = f"{grammar},{op},{k},{seed},{trial},"
        for position in range(1, len(archive.campaign)):
            line = archive.campaign[position]
            if line.startswith(prefix) and split_fields(line)[archive.index["strategy"]] == strategy:
                return position
        raise AssertionError((grammar, op, k, seed, trial, strategy))

    transfer_refusal = {field: "" for field in ANSWER_DEPENDENT_COLUMNS}
    transfer_refusal["attempts"] = "0"
    transfer_refusal["outcome"] = "refused"
    balanced_grammar = "c-like conventional plus block comments alone"
    balanced_out = row_position(balanced_grammar, "substitute", "1", "0", "0", "token-semicolon")
    balanced_in = row_position(balanced_grammar, "substitute", "1", "0", "382", "token-semicolon")
    balanced_exact = archive.arm_row_of(balanced_in, "exact")
    balanced_completion = {
        field: archive.field(balanced_exact, field)
        for field in ("first", "first_landed", "terminal", "terminal_landed", "converged",
                      "lost", "spurious", "attempts")
    }
    balanced_completion["outcome"] = "completed"
    case(
        "cell-balanced-membership-transfer-preserving-every-count",
        campaign={
            balanced_out: archive.edited(balanced_out, **transfer_refusal),
            balanced_in: archive.edited(balanced_in, **balanced_completion),
        },
        marker=("membership commitment broken", "plus block comments alone|substitute|1|0"),
    )

    # The value-moving twin of the balanced transfer: a completed multi-attempt row's lost and
    # spurious counts are rebalanced with their sum preserved, so the divergence bound, every
    # aggregate identity, and every incident law hold while the pooled lost and spurious means move.
    # Multi-attempt, so the single-attempt divergence sharing law is not what objects; only the
    # cell's membership commitment sees the bytes change.
    rebalance_grammar = "c-like conventional with strings and line comments"
    rebalance_row = None
    for trial in range(TRIALS_PER_CELL):
        try:
            candidate = row_position(rebalance_grammar, "substitute", "16", "0", str(trial),
                                     "token-semicolon")
        except AssertionError:
            continue
        if archive.field(candidate, "outcome") == "completed" \
                and int(archive.field(candidate, "attempts")) >= 2 \
                and int(archive.field(candidate, "lost")) >= 1:
            rebalance_row = candidate
            break
    assert rebalance_row is not None
    case(
        "completed-row-divergence-rebalanced-within-its-region",
        campaign={
            rebalance_row: archive.edited(
                rebalance_row,
                lost=str(int(archive.field(rebalance_row, "lost")) - 1),
                spurious=str(int(archive.field(rebalance_row, "spurious")) + 1),
            )
        },
        marker=("membership commitment broken", "with strings and line comments|substitute|16|0"),
    )

    # The commitment's disclosed boundary, staged rather than left to prose: the same coherent
    # rebalance with its commitment recomputed from the mutant campaign is accepted, and the
    # statistics emission drifts, which is exactly what the commitment cannot see on its own. The
    # authority binding the commitment file itself is the manifest, the root ledger, and the pinned
    # reproduction run; this case proves the boundary sits
    # where the prose says it sits, not nearer and not farther.
    # The row is derived from the guards themselves: a completed single-attempt skip-one row whose
    # answer sits on no mapped boundary, inside no damaged window, at no input end, and is shared by
    # no other arm of its incident, so both landing flags can flip together with every structural
    # law still holding. The landing emission then moves, which is the demonstration.
    def coherent_flag_row():
        for position in range(1, len(archive.campaign)):
            try:
                if archive.field(position, "strategy") != "skip-one":
                    continue
                if archive.field(position, "outcome") != "completed":
                    continue
                if archive.field(position, "attempts") != "1":
                    continue
                first = archive.field(position, "first")
                if not first or archive.field(position, "first_landed") != "1":
                    continue
                if first == archive.field(position, "first_true"):
                    continue
                p = int(archive.field(position, "p"))
                k = int(archive.field(position, "k"))
                if p <= int(first) < p + k:
                    continue
                shared = False
                for arm in ("certified", "certified-clean", "exact", "exact-clean", "skip-one",
                            "newline", "newline-at", "semicolon", "semicolon-at",
                            "token-newline", "token-semicolon"):
                    partner = archive.arm_row_of(position, arm)
                    if partner is not None and partner != position \
                            and archive.field(partner, "first") == first:
                        shared = True
                        break
                if not shared:
                    return position
            except (IndexError, KeyError, ValueError):
                continue
        raise AssertionError("no coherent flag row found for the commitment boundary case")

    flag_row = coherent_flag_row()
    case(
        "coherent-cell-rewrite-with-recomputed-commitment",
        campaign={
            flag_row: archive.edited(flag_row, first_landed="0", terminal_landed="0")
        },
        commitments="recompute",
        expect="accept-drift",
        emissions=EMISSIONS,
    )

    capped_row = row_position("json rfc 8259 lexical forms on a real-world document",
                              "substitute", "4", "0", "437", "skip-one")
    assert archive.field(capped_row, "outcome") == "completed"
    assert archive.field(capped_row, "attempts") == "100"
    case(
        "completed-row-relabeled-capped",
        campaign={
            capped_row: archive.edited(capped_row, outcome="capped", converged="", lost="",
                                       spurious="")
        },
        marker=("cell_capped.get(summary_cell, 0) == capped", "'skip-one')"),
    )

    # The summary's closed positional parser, exercised shape by shape: a renamed header column, a
    # header deleted from one section and one duplicated inside another, and five cell-row shapes,
    # the fully unknown 595th cell the reports staged themselves among them. Each dies at the exact
    # position the walk expects something else, because the parser never infers what a line is from
    # field values whose corruption it exists to catch.
    summary_header_line = ("  op           k  strategy        answers  refuse   t-ref  f-land"
                           "   t-land complete capped attempts     conv   lost   spur overshoot")
    case(
        "summary-header-column-renamed",
        summary_edit=("strategy        answers", "strategy        answcnt", 1),
        marker=("summary header not verbatim", "answcnt"),
    )
    case(
        "summary-header-deleted-from-one-section",
        summary_edit=(
            "c-like conventional plus block comments alone\n" + summary_header_line,
            "c-like conventional plus block comments alone",
            1,
        ),
        marker=("summary header not verbatim", "'c-like conventional plus block comments alone'"),
    )
    case(
        "summary-header-duplicated-inside-another-section",
        summary_edit=(
            "json rfc 8259 lexical forms\n" + summary_header_line,
            "json rfc 8259 lexical forms\n" + summary_header_line + "\n" + summary_header_line,
            1,
        ),
        marker=("summary cell row off the declared grid", "overshoot"),
    )
    case(
        "summary-carrying-an-unknown-cell",
        summary_edit=(
            "  substitute   1  certified ",
            "  scramble    999  certified             0       0       0    0.0%     0.0%"
            "     0.0%      0     0.00        0   0.00   0.00       0.0\n  substitute   1  certified ",
            1,
        ),
        marker=("summary cell row off the declared grid", "scramble    999"),
    )
    case(
        "summary-cell-with-operation-size-and-arm-all-unknown",
        summary_edit=(
            "  substitute   1  certified ",
            "  alien 999 ghost 0 0 0 0 0 0 0 0 0 0 0 0\n  substitute   1  certified ",
            1,
        ),
        marker=("summary cell row off the declared grid", "alien 999 ghost"),
    )
    case(
        "summary-cell-with-an-unknown-operation-alone",
        summary_edit=("  substitute   1  certified           841",
                      "  scramble     1  certified           841", 1),
        marker=("summary cell row off the declared grid", "scramble     1"),
    )
    case(
        "summary-cell-with-an-unknown-arm-alone",
        summary_edit=("  substitute   1  skip-one ", "  substitute   1  mystery  ", 1),
        marker=("summary cell row off the declared grid", "mystery"),
    )
    case(
        "summary-cell-carrying-an-extra-field",
        summary_edit=("   7.53   0.00      26.1", "   7.53   0.00      26.1 0", 1),
        marker=("summary cell row off the declared grid", "26.1 0"),
    )
    case(
        "summary-rewritten-with-a-non-breaking-space",
        summary_edit=("certified           841", "certified\u00a0          841", 1),
        marker=("byte outside ASCII at offset", "0xc2"),
    )
    case(
        "summary-count-in-arabic-indic-digits",
        summary_edit=("certified           841", "certified           \u0668\u0664\u0661", 1),
        marker=("byte outside ASCII at offset", "0xd9"),
    )
    case(
        "summary-count-padded-with-a-leading-zero",
        summary_edit=("certified           841", "certified           0841", 1),
        marker=("summary cell field carries a leading zero", "0841"),
    )
    case(
        "summary-percentage-past-a-hundred",
        summary_edit=("  100.0%   100.0%   100.0%      0     1.00       28   7.53   0.00      26.1",
                      "  999.9%   100.0%   100.0%      0     1.00       28   7.53   0.00      26.1", 1),
        marker=("summary percentage outside nought to a hundred", "999.9%"),
    )
    case(
        "summary-count-longer-than-any-campaign-writes",
        summary_edit=("certified           841", "certified           " + "9" * 40, 1),
        marker=("summary cell field longer than any count this campaign writes",),
    )
    case(
        "summary-cell-field-off-its-grammar",
        summary_edit=("certified           841       0       0  100.0%",
                      "certified           841       0       0  bogus%", 1),
        marker=("summary cell field off its declared grammar", "bogus%"),
    )
    case(
        "summary-signed-overshoot-padded-with-a-leading-zero",
        summary_edit=("   0.05   0.73      -2.1", "   0.05   0.73      -02.1", 1),
        marker=("summary cell field off its declared grammar", "-02.1"),
    )
    # Negative zero walks past the leading-zero wall, which is anchored at the first character, and
    # satisfies the signed field's own grammar. It is not a spelling the harness's integer-sum mean
    # can emit for zero, so it is staged as its own case at its own wall.
    case(
        "summary-signed-overshoot-spelling-a-negative-zero",
        summary_edit=("   0.05   0.73      -2.1", "   0.05   0.73      -0.0", 1),
        marker=("summary cell field spells a negative zero", "-0.0"),
    )
    # The tail lines are matched as whole lines, so their digit classes admit spellings the cell
    # loop's field walls would refuse. These two stage that boundary from both directions.
    case(
        "summary-tail-displacement-padded-with-a-leading-zero",
        summary_edit=("net displacement -14029 bytes", "net displacement -014029 bytes", 1),
        marker=("summary number carries a leading zero", "-014029"),
    )
    case(
        "summary-tail-displacement-spelling-a-negative-zero",
        summary_edit=("net displacement -14029 bytes", "net displacement -0 bytes", 1),
        marker=("summary number spells a negative zero", "displacement -0 bytes"),
    )
    # The canonical scanner covers every summary line kind, so each remaining kind is staged once:
    # a padded seed count in the determinism preamble, a padded pooled answer count, and a padded
    # per-seed rate, each a spelling the harness's own arithmetic cannot emit.
    case(
        "summary-oracle-rows-padded-with-a-leading-zero",
        summary_edit=("violations over 6 rows", "violations over 06 rows", 1),
        marker=("summary number carries a leading zero", "06 rows"),
    )
    case(
        "summary-preamble-seeds-padded-with-a-leading-zero",
        summary_edit=("3 independent seeds", "03 independent seeds", 1),
        marker=("summary number carries a leading zero", "03 independent seeds"),
    )
    case(
        "summary-pooled-answers-padded-with-a-leading-zero",
        summary_edit=("  certified       answers   7356 initial",
                      "  certified       answers   07356 initial", 1),
        marker=("summary number carries a leading zero", "07356"),
    )
    case(
        "summary-per-seed-rate-padded-with-a-leading-zero",
        summary_edit=("seed 0 first-landing: certified 96.9%",
                      "seed 0 first-landing: certified 096.9%", 1),
        marker=("summary number carries a leading zero", "096.9%"),
    )
    case(
        "summary-cell-missing-its-last-field",
        summary_edit=("   7.53   0.00      26.1", "   7.53   0.00", 1),
        marker=("summary cell row off the declared grid", "0.00', ('substitute'"),
    )

    # One declaration carries every case's law and stratum, and everything else derives from it: the
    # tags are injected into the case objects themselves, the full law-and-stratum grid is asserted
    # equal to the declaration in both directions, and every declared name must be a staged case.
    # There is no second structure to drift from the first, removing or retagging any entry breaks
    # the grid equality, and --prove-metadata demonstrates that the breakage is fatal.
    staged = {entry["name"] for entry in cases}
    if len(staged) != len(cases):
        raise AssertionError("duplicate case names in the suite")
    assert_case_markers(cases)
    assert_case_metadata(cases, CASE_SCHEMA)

    return cases


def missing_markers(case, stderr):
    # A case names either one marker or a small set of them, all of which must appear. A set is what
    # separates two cases aimed at one guard whose source text they share: the guard's line pins the
    # guard, and a fragment of the tuple it carries pins which of the two rows tripped it.
    marker = case["marker"]
    if marker is None:
        return []
    wanted = (marker,) if isinstance(marker, str) else tuple(marker)
    # An empty fragment is a substring of every refusal, so a case carrying one would credit any
    # rejection at all to the guard it names, which is the opposite of what a marker is for. The
    # emptiness is refused here rather than left to a reader's eye, and the whole population is
    # held to the same rule by assert_case_markers before any case runs.
    assert all(text for text in wanted), ("a case carries an empty marker fragment", case["name"])
    return [text for text in wanted if text not in stderr]


def assert_case_markers(cases):
    """Every rejecting case names at least one nonempty fragment, asserted over the population.

    A marker that is absent, empty, or a tuple carrying an empty fragment would make the suite
    report REJECTED for a refusal from any guard whatever, so the population is checked once,
    before the first case is staged, rather than case by case as they happen to run.
    """
    for entry in cases:
        if entry["expect"] != "reject":
            continue
        marker = entry["marker"]
        assert marker is not None, ("a rejecting case names no guard", entry["name"])
        fragments = (marker,) if isinstance(marker, str) else tuple(marker)
        assert fragments, ("a rejecting case names an empty marker set", entry["name"])
        assert all(isinstance(text, str) and text for text in fragments), \
            ("a rejecting case carries an empty marker fragment", entry["name"])


def neuter_case(archive, cases, name):
    # Disarm one mutation by staging its rows unchanged: the case still streams through the same edit
    # path, at the same line positions, but every replacement line is the gold line it replaces, so the
    # archive reaching the analyzer is the archive the baseline ran on. The analyzer must then accept it,
    # and a suite that is really checking acceptance against expectation must call that a failure.
    for case in cases:
        if case["name"] != name:
            continue
        assert case["expect"] == "reject", name
        assert not case["omit_sidecar"] and not case["env"], name
        assert case["campaign"] or case["sidecar"], name
        case["campaign"] = {position: [archive.campaign[position]] for position in case["campaign"]}
        case["sidecar"] = {position: [archive.sidecar[position]] for position in case["sidecar"]}
        return case
    raise AssertionError(f"no such case to neuter: {name}")


def run_suite(data_dir, neutered=None, only=None):
    analyzer = os.path.join(data_dir, ANALYZER)
    mechanism = os.path.join(data_dir, MECHANISM)
    gold_dir = os.path.join(data_dir, GOLD_DIR)
    campaign_gz = os.path.join(gold_dir, CAMPAIGN + ".gz")
    sidecar_gz = os.path.join(gold_dir, CAMPAIGN + SIDECAR_SUFFIX + ".gz")
    for path in (analyzer, mechanism, campaign_gz, sidecar_gz):
        if not os.path.exists(path):
            sys.exit(f"test_analyze_r6.py: missing {path}")

    work = tempfile.mkdtemp(prefix="analyze-r6-mutation-")
    failures = []
    verdicts = {}
    rejected = 0
    mechanism_rejected = 0
    controls = 0
    boundaries = 0
    try:
        archive = Archive(load_gzipped_lines(campaign_gz), load_gzipped_lines(sidecar_gz))
        base_dir = os.path.join(work, "base")
        mut_dir = os.path.join(work, "mut")
        os.makedirs(base_dir)
        os.makedirs(mut_dir)
        base_csv = os.path.join(base_dir, CAMPAIGN)
        base_sidecar = base_csv + SIDECAR_SUFFIX
        mut_csv = os.path.join(mut_dir, CAMPAIGN)
        mut_sidecar = mut_csv + SIDECAR_SUFFIX
        stream(archive.campaign, {}, base_csv)
        stream(archive.sidecar, {}, base_sidecar)
        # The harness summary and the membership commitments travel beside every staged CSV under
        # the twin names the analyzer derives, because both reconciliations read them as required
        # inputs, never optional ones. The summary is rewritten per case, pristine or carrying the
        # case's own summary edit, so a summary corruption is staged the same way a row corruption is.
        summary_name = CAMPAIGN[: -len(".csv")] + ".txt"
        gold_summary = os.path.join(gold_dir, summary_name)
        with open(gold_summary, encoding="utf-8") as handle:
            gold_summary_text = handle.read()
        shutil.copyfile(gold_summary, os.path.join(base_dir, summary_name))
        shutil.copyfile(gold_summary, os.path.join(mut_dir, summary_name))
        commitments_name = CAMPAIGN[: -len(".csv")] + ".commitments.txt"
        gold_commitments = os.path.join(gold_dir, commitments_name)
        shutil.copyfile(gold_commitments, os.path.join(base_dir, commitments_name))
        shutil.copyfile(gold_commitments, os.path.join(mut_dir, commitments_name))

        out_base = os.path.join(work, "out-base")
        status, _ = run_analyzer(analyzer, base_csv, out_base)
        mismatched = compare_emissions(out_base, gold_dir, EMISSIONS) if status == 0 else []
        if status != 0 or mismatched:
            detail = f"exit {status}" if status != 0 else "emissions differ: " + ", ".join(mismatched)
            print(f"baseline-reproduces-archived-emissions FAILED ({detail})")
            failures.append("baseline")
        else:
            print("baseline-reproduces-archived-emissions PASSED")

        # The mechanism companion is pinned the same way, from the same staged CSV: it reads the campaign
        # alone, needs no sidecar, and writes its emissions into a directory of the suite's choosing, so
        # nothing is written beside the archive. Both are compared, the printed figures and the overhang
        # data file, because the plot the manuscript prints is drawn from the second one alone.
        out_mech = os.path.join(work, "out-mech")
        status, _ = run_analyzer(mechanism, base_csv, out_mech)
        mismatched = compare_emissions(out_mech, gold_dir, MECHANISM_EMISSIONS) if status == 0 else []
        if status != 0 or mismatched:
            detail = f"exit {status}" if status != 0 else "emissions differ: " + ", ".join(mismatched)
            print(f"mechanism-baseline-reproduces-archived-emissions FAILED ({detail})")
            failures.append("mechanism-baseline")
        else:
            print("mechanism-baseline-reproduces-archived-emissions PASSED")

        out_mut = os.path.join(work, "out-mut")
        cases = build_cases(archive)
        if neutered is not None:
            neuter_case(archive, cases, neutered)
        if only is not None:
            named = [case for case in cases if case["name"] == only]
            if not named:
                sys.exit(f"test_analyze_r6.py: no case named {only}; --list-cases prints them")
            # The control still runs first so a failure of the named case cannot be blamed on the
            # staging machinery, except when the control itself is what was named: then it runs once,
            # since --case NAME promises one named run and a doubled control would count itself twice.
            passing = [case for case in cases if case["expect"] == "accept" and case["name"] != only]
            cases = passing + named
        for case in cases:
            for path in (mut_csv, mut_sidecar):
                if os.path.exists(path):
                    os.remove(path)
            summary_text = gold_summary_text
            if case["summary_edit"] is not None:
                before, after, count = case["summary_edit"]
                assert summary_text.count(before) >= count, (case["name"], before[:40])
                summary_text = summary_text.replace(before, after, count)
            with open(os.path.join(mut_dir, summary_name), "w", encoding="utf-8") as handle:
                handle.write(summary_text)
            if case["campaign"]:
                stream(archive.campaign, case["campaign"], mut_csv)
            else:
                link_or_copy(base_csv, mut_csv)
            # Every case starts from the archived commitment file; the one boundary case recomputes
            # it from the mutant campaign instead, which is exactly the coherent-rewrite shape the
            # commitment is documented not to see on its own.
            shutil.copyfile(gold_commitments, os.path.join(mut_dir, commitments_name))
            if case["commitments"] == "recompute":
                digests = commit_r6.cell_commitments(mut_csv)
                with open(os.path.join(mut_dir, commitments_name), "w",
                          encoding="utf-8") as handle:
                    handle.write("\n".join(f"{digests[key]}  {key}"
                                           for key in sorted(digests)) + "\n")
            if not case["omit_sidecar"]:
                if case["sidecar"]:
                    stream(archive.sidecar, case["sidecar"], mut_sidecar)
                else:
                    link_or_copy(base_sidecar, mut_sidecar)
            status, stderr = run_analyzer(os.path.join(data_dir, case["program"]), mut_csv, out_mut, case["env"])
            if case["expect"] == "accept-drift":
                mismatched = compare_emissions(out_mut, gold_dir, case["emissions"]) if status == 0 else []
                if status != 0:
                    verdicts[case["name"]] = "REJECTED"
                    print(f"{case['name']} REJECTED (boundary, expected acceptance, exit {status})")
                    failures.append(case["name"])
                elif sorted(mismatched) != ["r6-landing-figure.csv", "r6-stats.txt"]:
                    verdicts[case["name"]] = "ACCEPTED-WITHOUT-DRIFT"
                    print(f"{case['name']} ACCEPTED-WITHOUT-DRIFT (boundary, expected exactly the "
                          f"statistics and landing emissions to move, got: "
                          f"{', '.join(sorted(mismatched)) or 'none'})")
                    failures.append(case["name"])
                else:
                    verdicts[case["name"]] = "ACCEPTED-WITH-DRIFT"
                    print(f"{case['name']} ACCEPTED-WITH-DRIFT (boundary: the coherent rewrite "
                          f"passes with its recomputed commitment and moves: {', '.join(mismatched)})")
                    boundaries += 1
                continue
            if case["expect"] == "accept":
                mismatched = compare_emissions(out_mut, gold_dir, case["emissions"]) if status == 0 else []
                if status != 0:
                    verdicts[case["name"]] = "REJECTED"
                    print(f"{case['name']} REJECTED (control, expected acceptance, exit {status})")
                    failures.append(case["name"])
                elif mismatched:
                    verdicts[case["name"]] = "ACCEPTED-WRONG-EMISSIONS"
                    print(f"{case['name']} ACCEPTED-WRONG-EMISSIONS (control, differ: " f"{', '.join(mismatched)})")
                    failures.append(case["name"])
                else:
                    verdicts[case["name"]] = "ACCEPTED"
                    print(f"{case['name']} ACCEPTED (control)")
                    controls += 1
                continue
            absent = missing_markers(case, stderr)
            if status == 0:
                verdicts[case["name"]] = "ACCEPTED"
                print(f"{case['name']} ACCEPTED")
                failures.append(case["name"])
            elif absent:
                # The mutation was refused, but not by the guard it aims at, so the guard it aims at is
                # still unproven and the case is a failure.
                verdicts[case["name"]] = "REJECTED-WRONG-GUARD"
                print(f"{case['name']} REJECTED-WRONG-GUARD (expected marker: {' + '.join(absent)})")
                failures.append(case["name"])
            else:
                verdicts[case["name"]] = "REJECTED"
                print(f"{case['name']} REJECTED")
                rejected += 1
                if case["program"] == MECHANISM:
                    mechanism_rejected += 1
    finally:
        shutil.rmtree(work, ignore_errors=True)

    # The two programs under test are named apart in the summary, since one count over both would
    # read as coverage of either alone: the archive auditor and the mechanism companion each reject
    # their own staged corruptions.
    print(f"{rejected} rejected, {rejected - mechanism_rejected} against the archive auditor and "
          f"{mechanism_rejected} against the mechanism companion, {controls} control accepted, "
          f"{boundaries} boundary accepted with drifted emissions, {len(failures)} failed")
    if failures:
        print("failing: " + ", ".join(failures))
        return 1, failures, verdicts
    return 0, failures, verdicts


def prove_detection(data_dir):
    # The advertised self-check, run as a program rather than asserted in prose. One mutation is disarmed
    # before the suite starts, so the archive it stages is the pristine archive and the analyzer has
    # nothing to refuse. Everything downstream is the ordinary machinery, unchanged and unaware. The run
    # is a success exactly when the suite notices: the disarmed case must be recorded as ACCEPTED, must be
    # named as a failure, and must be the only failure, and the suite must exit nonzero because of it.
    status, failures, verdicts = run_suite(data_dir, neutered=NEUTERED_CASE)
    detected = status != 0 and failures == [NEUTERED_CASE] and verdicts.get(NEUTERED_CASE) == "ACCEPTED"
    held = "held" if detected else "did not hold"
    verdict = verdicts.get(NEUTERED_CASE)
    seen = "accepted it" if verdict == "ACCEPTED" else f"did not accept it, recorded {verdict}"
    print(
        f"--prove-detection: {NEUTERED_CASE} was staged unchanged on purpose, so the analyzer was "
        f"handed the pristine archive and {seen}, and the suite ran on unaware and exited {status}. "
        f"The exit code is inverted for this mode, 0 exactly when that acceptance was caught as a "
        f"failure, so this run exits {0 if detected else 1} because the detection {held}."
    )
    if not detected:
        print(
            f"--prove-detection: expected exactly [{NEUTERED_CASE}] recorded ACCEPTED and a nonzero "
            f"suite exit, got exit {status}, verdict {verdicts.get(NEUTERED_CASE)}, failures "
            f"{failures}"
        )
    return 0 if detected else 1


def main():
    # The audit is the assertions: under -O or PYTHONOPTIMIZE they are stripped and the coverage
    # matrix, strata, and staging checks silently vanish, so an optimized interpreter is refused
    # outright, exactly as the analyzer refuses one.
    if sys.flags.optimize:
        sys.exit("test_analyze_r6.py: refusing to run with assertions disabled (-O/PYTHONOPTIMIZE)")
    arguments = sys.argv[1:]
    prove = "--prove-detection" in arguments
    prove_metadata = "--prove-metadata" in arguments
    listing = "--list-cases" in arguments
    only = None
    positional = []
    skip_next = False
    for index, argument in enumerate(arguments):
        if skip_next:
            skip_next = False
            continue
        if argument in ("--prove-detection", "--prove-metadata", "--list-cases"):
            continue
        if argument == "--case":
            if index + 1 >= len(arguments):
                sys.exit("test_analyze_r6.py: --case needs a case name; --list-cases prints them")
            only = arguments[index + 1]
            skip_next = True
            continue
        positional.append(argument)
    data_dir = os.path.abspath(positional[0] if positional else os.path.dirname(os.path.abspath(__file__)))
    if prove_metadata:
        # The metadata check must be fatal for nine doctored declarations, each staged to reach
        # a specific wall, population, grid, ownership, or the ownership table's own key set, and
        # each must be refused at that wall with its own named detail, matched against the
        # refusal's message. The exit is inverted the way --prove-detection's is: zero exactly
        # when all nine were caught as staged.
        gold_dir = os.path.join(data_dir, GOLD_DIR)
        archive = Archive(
            load_gzipped_lines(os.path.join(gold_dir, CAMPAIGN + ".gz")),
            load_gzipped_lines(os.path.join(gold_dir, CAMPAIGN + SIDECAR_SUFFIX + ".gz")),
        )
        cases = build_cases(archive)
        # Nine doctored declarations, each a shape a report staged or demanded: a critical
        # entry removed, a stratum retagged to a name the grid does not hold, two valid erasure
        # tags swapped between the token cases so the multiset survives, a critical tag
        # retargeted to an unrelated staged case, a non-critical entry removed so a guard-bearing
        # case goes untagged, an ownership signature deleted so the key-set wall must catch the
        # narrowing, an ownership signature altered so it no longer lives in its own case's
        # marker, a stray ownership key, and a noncritical tag swap. Every one must fail its own
        # assertion for this mode to pass.
        thinned = dict(CASE_SCHEMA)
        del thinned["token-semicolon-arm-erasing-its-own-membership"]
        retagged = dict(CASE_SCHEMA)
        retagged["token-arm-erasing-its-own-membership"] = ("erasure", "renamed-away")
        swapped = dict(CASE_SCHEMA)
        swapped["token-arm-erasing-its-own-membership"] = ("erasure", "token-semicolon")
        swapped["token-semicolon-arm-erasing-its-own-membership"] = ("erasure", "token-newline")
        # The retarget preserves the population and the grid deliberately, tags exchanged between a
        # critical case and an unrelated staged one, so only the ownership signature can catch it;
        # a retarget that also thins the population is the thinned shape wearing another name.
        retargeted = dict(CASE_SCHEMA)
        retargeted["row-names-a-grammar-with-no-source-length"] = ("erasure", "token-semicolon")
        retargeted["token-semicolon-arm-erasing-its-own-membership"] = \
            CASE_SCHEMA["row-names-a-grammar-with-no-source-length"]
        untagged = dict(CASE_SCHEMA)
        del untagged["sidecar-answer-equals-evidence-end"]
        # Two noncritical tags exchanged between cases whose guards differ: population, grid, and
        # every critical signature survive, and only the whole-population tag-to-guard binding can
        # see it. This is the shape a report demonstrated passing.
        noncritical_swap = dict(CASE_SCHEMA)
        noncritical_swap["sidecar-row-deleted"] = CASE_SCHEMA["unknown-strategy-name"]
        noncritical_swap["unknown-strategy-name"] = CASE_SCHEMA["sidecar-row-deleted"]
        unsigned = dict(CRITICAL_SIGNATURES)
        del unsigned[("summary", "cell-all-unknown")]
        resigned = dict(CRITICAL_SIGNATURES)
        resigned[("summary", "cell-arm")] = "gremlin"
        oversigned = dict(CRITICAL_SIGNATURES)
        oversigned[("summary", "stray-ownership-key")] = "unused"
        # Each doctored declaration must fail at its own named assertion, distinguished by the
        # refusal's message, so a shape caught by an unrelated guard fails this mode rather than
        # padding the count.
        staged_shapes = (
            (thinned, None, "guard-bearing cases outside the declaration",
             "token-semicolon-arm-erasing-its-own-membership"),
            (retagged, None, "law and stratum grid is not the expected one", "renamed-away"),
            (swapped, None, "does not carry the guard the declaration names",
             "token-semicolon-arm-erasing-its-own-membership"),
            (retargeted, None, "does not carry the guard the declaration names",
             "token-semicolon-arm-erasing-its-own-membership"),
            (untagged, None, "guard-bearing cases outside the declaration",
             "sidecar-answer-equals-evidence-end"),
            (noncritical_swap, None,
             "does not carry the guard the declaration names", "sidecar-row-deleted"),
            (dict(CASE_SCHEMA), unsigned, "critical strata without an ownership signature",
             "cell-all-unknown"),
            (dict(CASE_SCHEMA), resigned, "signature is absent from its own case's marker",
             "summary-cell-with-an-unknown-arm-alone"),
            (dict(CASE_SCHEMA), oversigned, "ownership signatures outside the critical laws",
             "stray-ownership-key"),
        )
        caught = 0
        for doctored, doctored_signatures, wall, detail in staged_shapes:
            try:
                assert_case_metadata(cases, doctored, doctored_signatures)
            except AssertionError as refusal:
                text = str(refusal)
                if wall in text and detail in text:
                    caught += 1
                else:
                    print("--prove-metadata: a doctored declaration was refused at the wrong "
                          "wall: expected %r naming %r, got %s" % (wall, detail, text[:200]),
                          file=sys.stderr)
        if caught != 9:
            print("--prove-metadata: a doctored declaration passed the metadata assertions or "
                  "failed off its own wall, %d of 9 caught as staged" % caught, file=sys.stderr)
            return 1
        print("--prove-metadata: a thinned critical entry, a stratum retagged off the grid, two "
              "valid tags swapped between their cases, a critical tag retargeted onto an unrelated "
              "case with the population preserved, an untagged guard-bearing case, a deleted "
              "ownership signature, an altered one, a stray ownership signature, and two "
              "noncritical tags exchanged between "
              "cases whose guards differ, each fail at their own named assertion, "
              "distinguished by its message, so the declaration is fail-closed over population, "
              "grid, and ownership, and the ownership table itself is fail-closed over its key set")
        return 0

    if listing:
        # The names come from the staged archive's own case list, so the listing can never drift from
        # what a run would stage.
        gold_dir = os.path.join(data_dir, GOLD_DIR)
        archive = Archive(
            load_gzipped_lines(os.path.join(gold_dir, CAMPAIGN + ".gz")),
            load_gzipped_lines(os.path.join(gold_dir, CAMPAIGN + SIDECAR_SUFFIX + ".gz")),
        )
        cases = build_cases(archive)
        for case in cases:
            if case["expect"] == "accept":
                kind = "control"
            elif case["expect"] == "accept-drift":
                kind = "boundary"
            else:
                kind = "mutation"
            if case["law"] is not None:
                print("%s  [%s, %s/%s]" % (case["name"], kind, case["law"], case["stratum"]))
            else:
                print("%s  [%s]" % (case["name"], kind))
        controls = sum(1 for case in cases if case["expect"] == "accept")
        boundaries = sum(1 for case in cases if case["expect"] == "accept-drift")
        mutations = sum(1 for case in cases if case["expect"] == "reject")
        tagged = sum(1 for case in cases if case["law"] is not None)
        # The split is asserted from the cases' own expectations, fail-closed: a listing that says
        # anything other than the documented population exits nonzero rather than printing a shorter
        # list that still reads like the claim. Every rejecting case carries a tag now, the
        # population equality lives in assert_case_metadata, and the counts here are the outer wall.
        if mutations != 170 or controls != 1 or boundaries != 1 or tagged != len(CASE_SCHEMA) \
                or tagged != mutations:
            print("the case population is %d mutations, %d controls, %d boundary, %d tagged, not "
                  "the documented 170, 1, 1, and %d" % (mutations, controls, boundaries, tagged,
                                                        len(CASE_SCHEMA)),
                  file=sys.stderr)
            return 1
        print("170 mutations, 1 control, and 1 commitment-boundary case, every mutation carrying "
              "its law and stratum, asserted from the cases' own objects")
        return 0
    if prove:
        return prove_detection(data_dir)
    return run_suite(data_dir, only=only)[0]


if __name__ == "__main__":
    sys.exit(main())
