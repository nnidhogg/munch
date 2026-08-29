#!/usr/bin/env python3
# Proves that analyze_r6.py is fail-closed: every invariant it asserts is exercised by a mutation of the
# archived r6 campaign that the analyzer must refuse. The suite first establishes the baseline, running the
# analyzer on the decompressed gold archive and requiring exit 0 together with three emissions byte-identical
# to the archived copies, so a later rejection can be attributed to the mutation rather than to drift. It
# then reproduces the archived mechanism emission with analyze_r6_mechanism.py from the same staged CSV, so
# the second checked-in program that feeds the manuscript is pinned to its archived output as well. Each
# case that follows stages one corrupted archive and requires a nonzero analyzer exit; a mutation the
# analyzer accepts is a hole in the audit and fails the suite.
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
# Usage: test_analyze_r6.py [--prove-detection] [data-dir]
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

ANALYZER = "analyze_r6.py"
MECHANISM = "analyze_r6_mechanism.py"
GOLD_DIR = "r6"
CAMPAIGN = "recovery-quality-six-rows-512k-500-r6.csv"
SIDECAR_SUFFIX = ".moves.csv"
EMISSIONS = ("r6-stats.txt", "r6-pooled-table.tex", "r6-landing-figure.csv")
MECHANISM_EMISSION = "r6-mechanism.txt"

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
        # between the two files intact, and the line holding its last move, which is the one the archived
        # terminal answer and its landing flag are reconciled against.
        self.sidecar_first = {}
        self.sidecar_last = {}

        # Certified rows that answered more than once and archive a landed terminal: candidates for the
        # covered-terminal case, which cannot be settled until the sidecar's last move is known.
        self.terminal_candidates = []
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
                if fields[column["first"]]:
                    begin = int(fields[column["evidence_begin"]])
                    end = int(fields[column["evidence_end"]])
                    floor = int(fields[column["failure_offset"]]) + 1
                    corruption_end = int(fields[column["corruption_end"]])
                    if fields[column["evidence_kind"]] == "window":
                        self._note("certified_window", position)

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
            if strategy == "exact-clean" and fields[column["first"]]:
                # The other arm the harness floors at the corruption end, and the one that carries no
                # evidence at all: its answer alone is what the floor can be tested through.
                self._note("exact_clean_answered", position)
            if strategy == "newline":
                self._note("newline_arm", position)
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
            if strategy != "certified" and fields[column["first"]] and fields[column["first_landed"]]:
                self._note("landed_answer", position)
            if strategy not in ("certified", "certified-clean") and fields[column["first"]]:
                self._note("noncertified_answered", position)
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

    def _scan_sidecar(self):
        for position in range(1, len(self.sidecar)):
            fields = split_fields(self.sidecar[position])
            key = (tuple(fields[:5]), fields[SIDECAR_COLUMNS.index("strategy")])
            if fields[SIDECAR_COLUMNS.index("move")] == "0":
                assert key not in self.sidecar_first, key
                self.sidecar_first[key] = position

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
                break
        assert "certified_covered_terminal" in self.targets, len(self.terminal_candidates)

    def _note(self, name, position):
        self.targets.setdefault(name, position)

    def field(self, position, name):
        return split_fields(self.campaign[position])[self.index[name]]

    def edited(self, position, **changes):
        fields = split_fields(self.campaign[position])
        for name, value in changes.items():
            fields[self.index[name]] = value
        return [join_fields(fields)]

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
    kind_flipped = "window" if archive.field(certified, "evidence_kind") == "byte" else "byte"
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

    # The cell the deletion case removes, spelled as the grid guard reports the cells it finds missing.
    deleted_cell = repr(tuple(split_fields(archive.campaign[cell_block[0]])[:4]))

    cases = []

    def case(
        name,
        campaign=None,
        sidecar=None,
        omit_sidecar=False,
        extra_env=None,
        marker=None,
        expect="reject",
        emissions=None,
    ):
        cases.append(
            {
                "name": name,
                "campaign": campaign or {},
                "sidecar": sidecar or {},
                "omit_sidecar": omit_sidecar,
                "env": extra_env,
                "marker": marker,
                "expect": expect,
                "emissions": emissions,
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

    # Reconciliation between the sidecar and the archived per-incident aggregates.
    case(
        "campaign-first-answer-changed",
        campaign={certified: archive.edited(certified, first=str(int(archive.field(certified, "first")) + 1))},
        marker='assert record["first"] and int(record["first"]) == first_answer',
    )
    case(
        "campaign-terminal-answer-changed",
        campaign={certified: archive.edited(certified, terminal=str(int(archive.field(certified, "terminal")) + 1))},
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
        marker='assert (record[flag] == "") == (record[anchor_field] == "")',
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
    # and the budget contract would otherwise be what objects, leaving the dependency unproven.
    case(
        "completed-row-relabeled-capped-keeping-divergence",
        campaign={completed_certified: archive.edited(completed_certified, outcome="capped", attempts="100")},
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
    # once as the per-incident flag and once as the covered tally. The two cases flip a flag while leaving
    # the tallies untouched and equal to each other, so the tally reconciliation is satisfied and only the
    # per-move landing reconciliation can object, at the first move and at the terminal one respectively.
    case(
        "covered-first-move-flag-flipped-to-unlanded",
        campaign={certified_covered_first: archive.edited(certified_covered_first, first_landed="0")},
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

    assert certified_covered_value not in ("", "0"), certified_covered_value
    assert archive.field(repairable_block[0], "repairable") == "1", repairable_block[0]
    assert archive.field(delete_block[0], "op") == "delete", delete_block[0]
    assert archive.field(capped_row, "outcome") == "capped", capped_row
    # The renamed cell must not be a deletion cell: a deletion's corruption end is the deletion point,
    # and a row renamed out of that operation would be caught by the geometry rather than by the grid.
    assert archive.field(cell_block[0], "op") != "delete", cell_block[0]
    assert UNKNOWN_OPERATION not in ("substitute", "insert", "delete"), UNKNOWN_OPERATION
    return cases


def missing_markers(case, stderr):
    # A case names either one marker or a small set of them, all of which must appear. A set is what
    # separates two cases aimed at one guard whose source text they share: the guard's line pins the
    # guard, and a fragment of the tuple it carries pins which of the two rows tripped it.
    marker = case["marker"]
    if marker is None:
        return []
    wanted = (marker,) if isinstance(marker, str) else tuple(marker)
    return [text for text in wanted if text not in stderr]


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


def run_suite(data_dir, neutered=None):
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
    controls = 0
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
        # alone, needs no sidecar, and writes its single emission into a directory of the suite's choosing,
        # so nothing is written beside the archive.
        out_mech = os.path.join(work, "out-mech")
        status, _ = run_analyzer(mechanism, base_csv, out_mech)
        mismatched = compare_emissions(out_mech, gold_dir, (MECHANISM_EMISSION,)) if status == 0 else []
        if status != 0 or mismatched:
            detail = f"exit {status}" if status != 0 else "emission differs: " + ", ".join(mismatched)
            print(f"mechanism-baseline-reproduces-archived-emission FAILED ({detail})")
            failures.append("mechanism-baseline")
        else:
            print("mechanism-baseline-reproduces-archived-emission PASSED")

        out_mut = os.path.join(work, "out-mut")
        cases = build_cases(archive)
        if neutered is not None:
            neuter_case(archive, cases, neutered)
        for case in cases:
            for path in (mut_csv, mut_sidecar):
                if os.path.exists(path):
                    os.remove(path)
            if case["campaign"]:
                stream(archive.campaign, case["campaign"], mut_csv)
            else:
                link_or_copy(base_csv, mut_csv)
            if not case["omit_sidecar"]:
                if case["sidecar"]:
                    stream(archive.sidecar, case["sidecar"], mut_sidecar)
                else:
                    link_or_copy(base_sidecar, mut_sidecar)
            status, stderr = run_analyzer(analyzer, mut_csv, out_mut, case["env"])
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
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f"{rejected} rejected, {controls} control accepted, {len(failures)} failed")
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
    arguments = sys.argv[1:]
    prove = "--prove-detection" in arguments
    positional = [argument for argument in arguments if argument != "--prove-detection"]
    data_dir = os.path.abspath(positional[0] if positional else os.path.dirname(os.path.abspath(__file__)))
    if prove:
        return prove_detection(data_dir)
    return run_suite(data_dir)[0]


if __name__ == "__main__":
    sys.exit(main())
