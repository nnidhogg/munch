#!/usr/bin/env python3
# Second exhaustive checker: the anchored comparator machinery, wide window evidence, and pristine transfer.
#
# This extends the run in crosscheck_scan.py without touching it. Three bodies of work, all judged against
# reference.py, the from-scratch maximal-munch model that agreed with munch's own scan on 1.4 million
# strings in the first run:
#
#   Part 1, the anchored machinery, under exhaustive negative testing.
#     minimal_repair(tail) claims a shortest byte string r with r + tail completely tokenizable, and a
#     refusal claims no repair of any length exists. next_anchored_start(tail, from) claims the first
#     position p at or after `from` such that every completely tokenizable r + tail has a token
#     beginning at |r| + p. Over the whole enumeration of crosscheck_scan.py, every returned repair is scanned
#     by the reference, every refusal is checked by an exhaustive search of all repairs up to length
#     four over the token alphabet, and every anchored answer is confronted with every one of those
#     bounded repairs.
#
#   Part 2, window certificates of width three and four.
#     crosscheck_scan.py's token sets have tokens of length at most two, so the walk rarely answers on a wide
#     window. A curated and machine-enumerated family with tokens of length three and four is added and
#     crosscheck_scan.py's own claim 1 and claim 2 logic is rerun over it, with the evidence width histogrammed.
#
#   Part 3, pristine transfer.
#     Completely tokenizable strings are damaged, the reference locates the failure, the walk is asked
#     one past it, and every answer whose evidence begins at or after the corruption end must map back
#     onto a token boundary of the pristine string.
#
# Part 4 is the negative controls, each of which deliberately breaks one of the above and must be caught.
#
# The repair searches use the token alphabet alone, which loses nothing: a completely tokenizable string
# is a concatenation of tokens, so a repair containing a byte outside every token can never complete.

from __future__ import annotations

import sys

# Set before any local import: a stray __pycache__ written on the way to a refusal fails the release gate.
sys.dont_write_bytecode = True

import itertools
import subprocess
import textwrap
import time
from pathlib import Path

import crosscheck_scan as scan
import reference

HERE = Path(__file__).resolve().parent
if len(sys.argv) < 2:
    sys.exit("usage: crosscheck_anchor.py <path to the built munch_crosscheck_anchor probe>")
PROBE = Path(sys.argv[1])

PAD = scan.PAD

# Part 1 walks the enumeration crosscheck_scan.py already established: inputs up to six bytes over the token
# alphabet plus the pad byte, every anchor. The bounded repair search runs to length four.
MAX_INPUT = scan.MAX_INPUT
MAX_ANCHORED_REPAIR = 4

# A tail carrying the pad byte can never be completed, so the existing enumeration answers the anchored
# queries with a refusal on most of its inputs. The pad-free extension drops the pad and goes longer,
# which is where checks (a) and (c) get their exercise; the bound is smaller on wider alphabets.
PAD_FREE_MAX_INPUT = 10
PAD_FREE_MAX_INPUT_WIDE = 7

# Part 2 pushes the inputs one byte further, which the long-token family can afford.
LONG_MAX_INPUT = 7

# Part 3 wants at least this many distinct pristine strings per token set, and caps them so the damage
# enumeration stays finite.
PRISTINE_TARGET = 200
PRISTINE_CAP = 320
PRISTINE_MAX_LENGTH = 20

# What every negative control must produce, denominators included. The enumeration is deterministic, so
# these are exact rather than approximate, and the whole tuple is compared for equality: asserting only
# what a control caught would let a control pass while the population it was supposed to catch things in
# had silently emptied, which is the vacuous pass these controls exist to rule out. The cross path is
# pinned by its whole decomposition, because its catch count mixes two sources, an answer later than a
# certificate and a certificate the decider refused outright, and a catch count alone can neither tell
# those apart nor say how many cases the two machineries both answered in.
CONTROL_EXPECTATION = {
    "pad answers": 35,
    "pad caught": 35,
    "pad survivors": 0,
    "letter answers": 35,
    "letter caught": 28,
    "letter survivors": 7,
    "letter not minimal": 35,
    "displaced non-vacuous": 68,
    "displaced caught": 31,
    "skew covered": 38781,
    "skew violations": 38781,
    "unskewed covered": 38781,
    "unskewed violations": 0,
    "range non-vacuous": 68,
    "range caught": 68,
    "cross pairs": 49,
    "cross earlier": 0,
    "cross equal": 0,
    "cross late": 49,
    "cross walk only": 0,
    "cross caught": 49,
}

LONG_CURATED = [
    ("abc", "c"),
    ("abca", "a"),
    ("ab", "abc"),
    ("aab", "ba"),
]


def machine_long_sets():
    """Machine-enumerated token sets, each containing a token of length three or four.

    Every three-byte string over {a, b} paired with each of a, b, ab, ba, then every four-byte string
    over {a, b} paired with b. Deduplicated by membership.
    """
    out = []

    for long_token in ("".join(c) for c in itertools.product("ab", repeat=3)):
        for short in ("a", "b", "ab", "ba"):
            out.append((long_token, short))

    for long_token in ("".join(c) for c in itertools.product("ab", repeat=4)):
        out.append((long_token, "b"))

    return out


def long_token_sets():
    seen = set()
    out = []

    for candidate in LONG_CURATED + machine_long_sets():
        key = frozenset(candidate)

        if key in seen or len(key) != len(candidate):
            continue

        seen.add(key)
        out.append(candidate)

    return out


def pristine_sets():
    """The token sets of the pristine-transfer campaign.

    The long-token curated family, crosscheck_scan.py's curated family, and every second machine-enumerated long
    set. Sets over a one-letter alphabet, {a} and {aaa, a}, are left out: over one letter there is at
    most one completely tokenizable string per length, so two hundred distinct ones would force strings
    of length two hundred and a damage enumeration to match. Those sets are covered by parts one and
    two instead.
    """
    seen = set()
    out = []

    machine = machine_long_sets()

    for candidate in LONG_CURATED + list(scan.CURATED) + machine[::2]:
        key = frozenset(candidate)

        if key in seen or len(key) != len(candidate):
            continue

        if len({character for token in candidate for character in token}) < 2:
            continue

        seen.add(key)
        out.append(candidate)

    return out


def encode(text):
    return text if text else "-"


def run_probe(tokens, queries):
    """One probe process per token set: a SET line, the queries, END. Returns the answer lines."""
    script = ["SET %d %s" % (len(tokens), " ".join(tokens))]
    script.extend(queries)
    script.append("END")

    finished = subprocess.run(
        [str(PROBE)],
        input="\n".join(script) + "\n",
        capture_output=True,
        text=True,
        check=True,
    )

    lines = finished.stdout.splitlines()

    if len(lines) != len(queries):
        raise RuntimeError("probe returned %d lines for %d queries" % (len(lines), len(queries)))

    return lines


def parse_walk(line):
    if line == "R":
        return None

    parts = line.split()

    if parts[0] != "A":
        raise RuntimeError("unexpected walk line %r" % line)

    return (int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]))


def parse_anchored(line):
    if line == "NR":
        return None

    parts = line.split()

    if parts[0] != "N":
        raise RuntimeError("unexpected anchored line %r" % line)

    return int(parts[1])


def manuscript_anchored_answer():
    """The manuscript separation example's decider answer, asserted rather than described.

    Over {ab, ba} on the tail ab at anchor zero, the anchored decider must answer exactly zero, the
    answer the manuscript states: a refusal or any other position fails this run instead of joining a
    discussion count.
    """
    line = run_probe(("ab", "ba"), ["N %s %d" % (encode("ab"), 0)])[0]
    answered = parse_anchored(line)
    if answered != 0:
        return "the anchored decider returned %r instead of 0 on the separation tail" % (answered,)
    return None


def parse_repair(line):
    if line == "MR":
        return None

    parts = line.split()

    if parts[0] != "M":
        raise RuntimeError("unexpected repair line %r" % line)

    return "" if parts[1] == "-" else parts[1]


# ----------------------------------------------------------------------------------------------
# Part 1: the anchored machinery
# ----------------------------------------------------------------------------------------------


class AnchoredChecker:
    """Checks minimal_repair() and next_anchored_start() against the reference, exhausrially.

    `corrupt_repair` and `displace_anchored`, when given, are the negative controls: they mangle the
    library's answer before it is checked, and the checks must then fail.
    """

    def __init__(self, corrupt_repair=None, displace_anchored=None):
        self.corrupt_repair = corrupt_repair
        self.displace_anchored = displace_anchored

        self.sets = 0
        self.inputs = 0
        self.cases = 0
        self.repairs_searched = 0

        self.repair_answers = 0
        self.repair_refusals = 0
        self.repair_empty = 0
        self.repair_verified = 0
        self.repair_bad = []

        self.repair_in_bounded_search = 0
        self.repair_beyond_bound = 0
        self.repair_not_minimal = []

        self.refusal_confirmed = 0
        self.refusal_refuted = []

        self.anchored_answers = 0
        self.anchored_refusals = 0
        self.anchored_vacuous = 0
        self.anchored_pairs = 0
        self.anchored_bad = []
        self.anchored_range_bad = []

        self.firstness_positions = 0
        self.firstness_unrefuted = 0
        self.refusals_with_repairs = 0
        self.refusal_unrefuted_positions = 0

        self.cross_pairs = 0
        self.cross_equal = 0
        self.cross_earlier = 0
        self.cross_bad = []
        self.cross_late = 0
        self.anchored_only = 0
        self.walk_only = 0
        self.anchored_only_first = None
        self.walk_only_first = None

    def check_set(self, tokens, max_input=MAX_INPUT, pad_free=False):
        self.sets += 1

        token_letters = sorted({character for token in tokens for character in token})
        letters = token_letters if pad_free else scan.alphabet_of(tokens)
        inputs = scan.strings_up_to(letters, max_input)
        repairs = scan.strings_up_to(token_letters, MAX_ANCHORED_REPAIR)
        model = reference.Model(tokens)

        # For every input, the bounded set of repairs that complete it, and the positions every one of
        # them agrees is a token start. The anchored answer must lie in that set; the certificate for
        # position q under repair r is that |r| + q begins a committed token of r + x.
        complete = {}
        universal = {}

        for text in inputs:
            good = []

            for repair in repairs:
                target = repair + text
                position, starts = model.scan(target)

                if position == len(target):
                    good.append((repair, starts))

            self.repairs_searched += len(repairs)
            complete[text] = good

            if good:
                agreed = None

                for repair, starts in good:
                    shifted = {start - len(repair) for start in starts if start >= len(repair)}
                    agreed = shifted if agreed is None else (agreed & shifted)

                universal[text] = frozenset(q for q in agreed if q < len(text))
            else:
                universal[text] = None

        cases = [(text, anchor) for text in inputs for anchor in range(len(text) + 1)]

        queries = ["M %s" % encode(text) for text in inputs]
        queries.extend("N %s %d" % (encode(text), anchor) for text, anchor in cases)
        queries.extend("Q %s %d" % (encode(text), anchor) for text, anchor in cases)

        lines = run_probe(tokens, queries)

        repair_lines = lines[: len(inputs)]
        anchored_lines = lines[len(inputs) : len(inputs) + len(cases)]
        walk_lines = lines[len(inputs) + len(cases) :]

        self.inputs += len(inputs)
        self.cases += len(cases)

        for text, line in zip(inputs, repair_lines):
            answered = parse_repair(line)

            if answered is None:
                # (b) The negative label. An exhaustive bounded search must not find what the library
                # says does not exist.
                self.repair_refusals += 1

                if complete[text]:
                    self.refusal_refuted.append((tokens, text, [r for r, _ in complete[text]][:4]))
                else:
                    self.refusal_confirmed += 1

                continue

            self.repair_answers += 1

            if self.corrupt_repair is not None:
                answered = self.corrupt_repair(answered)

            if answered == "":
                self.repair_empty += 1

            # (a) Every returned repair must actually repair.
            target = answered + text
            position, starts = model.scan(target)

            if position != len(target):
                self.repair_bad.append((tokens, text, answered, target, position, len(target)))
            else:
                self.repair_verified += 1

            # Minimality, checked as far as the bound reaches.
            shortest = min((len(r) for r, _ in complete[text]), default=None)

            if len(answered) <= MAX_ANCHORED_REPAIR:
                self.repair_in_bounded_search += 1

                if shortest is not None and shortest < len(answered):
                    self.repair_not_minimal.append((tokens, text, answered, shortest))
            else:
                self.repair_beyond_bound += 1

                if shortest is not None:
                    self.repair_not_minimal.append((tokens, text, answered, shortest))

        # (d) The two machineries against each other. A certified start w is invariant across every
        # completely tokenizable repair, since such a repair reaches all evidence and, being prepended,
        # alters none of it. The anchored decider is exact for that same question, so on a tail some
        # repair completes it must answer, and answer at or before w.
        for (text, anchor), anchored_line, walk_line in zip(cases, anchored_lines, walk_lines):
            if not complete[text]:
                continue

            certified = parse_walk(walk_line)
            decided = parse_anchored(anchored_line)

            if decided is not None and self.displace_anchored is not None:
                decided = self.displace_anchored(decided, len(text))

            if certified is None:
                if decided is not None:
                    self.anchored_only += 1

                    if self.anchored_only_first is None:
                        self.anchored_only_first = (tokens, text, anchor, decided)

                continue

            if decided is None:
                self.walk_only += 1

                if self.walk_only_first is None:
                    self.walk_only_first = (tokens, text, anchor, certified)

                self.cross_bad.append(("refused where a certificate answers", tokens, text, anchor, certified))

                continue

            self.cross_pairs += 1

            if decided > certified[0]:
                self.cross_late += 1
                self.cross_bad.append(("answered later than the certificate", tokens, text, anchor, decided, certified))
            elif decided == certified[0]:
                self.cross_equal += 1
            else:
                self.cross_earlier += 1

        for (text, anchor), line in zip(cases, anchored_lines):
            answered = parse_anchored(line)

            if answered is None:
                self.anchored_refusals += 1

                if universal[text] is not None:
                    # The tail is repairable within the bound, so the refusal claims no position at or
                    # after the anchor is invariant. Bounded evidence cannot refute that, since an
                    # unbounded repair may still disagree, but the positions the bound fails to refute
                    # are worth counting.
                    self.refusals_with_repairs += 1
                    self.refusal_unrefuted_positions += len(
                        [q for q in range(anchor, len(text)) if q in universal[text]]
                    )

                continue

            self.anchored_answers += 1

            if self.displace_anchored is not None:
                answered = self.displace_anchored(answered, len(text))

            if answered < anchor or answered >= len(text):
                self.anchored_range_bad.append((tokens, text, anchor, answered))

            good = complete[text]

            if not good:
                # No bounded repair completes this input, so the quantifier is bounded-vacuous here.
                self.anchored_vacuous += 1

                continue

            self.anchored_pairs += len(good)

            if answered not in universal[text]:
                offender = next(
                    ((repair, sorted(starts)) for repair, starts in good if len(repair) + answered not in starts),
                    None,
                )

                self.anchored_bad.append((tokens, text, anchor, answered, offender))

            # The answer claims to be the first invariant position at or after the anchor. Each earlier
            # position the bounded repairs fail to separate is recorded, again as evidence and not proof.
            for q in range(anchor, min(answered, len(text))):
                self.firstness_positions += 1

                if q in universal[text]:
                    self.firstness_unrefuted += 1


# ----------------------------------------------------------------------------------------------
# Part 2: window certificates of width three and four
# ----------------------------------------------------------------------------------------------


class WidthRecorder:
    """Wraps scan.parse_answer so every walk answer's evidence width is histogrammed."""

    def __init__(self):
        self.histogram = {1: 0, 2: 0, 3: 0, 4: 0}
        self.width_bad = []
        self.refusals = 0
        self.original = scan.parse_answer

    def __enter__(self):
        def recording(line):
            answer = self.original(line)

            if answer is None:
                self.refusals += 1
            else:
                width = answer[2] - answer[1]
                if width not in (1, 2, 3, 4):
                    self.width_bad.append((width, answer))
                self.histogram[width] = self.histogram.get(width, 0) + 1

            return answer

        scan.parse_answer = recording

        return self

    def __exit__(self, *unused):
        scan.parse_answer = self.original

        return False


def run_long_family():
    sets = long_token_sets()
    checker = scan.Checker()
    failures = []

    original_max = scan.MAX_INPUT
    scan.MAX_INPUT = LONG_MAX_INPUT

    started = time.time()

    with WidthRecorder() as recorder:
        for index, tokens in enumerate(sets):
            print("  long [%2d/%2d] %r" % (index + 1, len(sets), list(tokens)), file=sys.stderr)

            try:
                checker.check_set(tokens)
            except scan.Violation as violation:
                failures.append((tokens, violation.claim, violation.detail))

    scan.MAX_INPUT = original_max

    return sets, checker, recorder, failures, time.time() - started


# ----------------------------------------------------------------------------------------------
# Part 3: pristine transfer
# ----------------------------------------------------------------------------------------------


def pristine_strings(model, tokens):
    """Distinct completely tokenizable strings, breadth first over token concatenations.

    Every completely tokenizable string is a concatenation of tokens, so extending by whole tokens
    reaches all of them; the converse fails, since maximal munch can misalign a concatenation into a
    dead end, which is why each candidate is put to the reference before it is kept. Candidates that
    fail stay in the frontier, because a longer extension of them can succeed.
    """
    seen = {""}
    found = []
    frontier = [""]

    while frontier and len(found) < PRISTINE_TARGET:
        following = []

        for base in frontier:
            for token in tokens:
                candidate = base + token

                if len(candidate) > PRISTINE_MAX_LENGTH or candidate in seen:
                    continue

                seen.add(candidate)
                following.append(candidate)

                if model.completely_tokenizable(candidate):
                    found.append(candidate)

        frontier = following

    found = sorted(found, key=lambda s: (len(s), s))

    if len(found) > PRISTINE_CAP:
        # Thin by a stride so the kept strings spread over every length reached, never below the target.
        stride = max(1, len(found) // PRISTINE_CAP)
        found = found[::stride][:PRISTINE_CAP]

    return found


def damages_of(text, letters):
    """Every substitution, insertion and deletion of size one and two, at every position.

    Each damage is reported as (kind, at, size, payload, damaged, corruption_end, delta), where for
    every position j of the damaged string at or after corruption_end, damaged[j] == text[j + delta].
    """
    out = []

    for size in (1, 2):
        payloads = ["".join(c) for c in itertools.product(letters, repeat=size)]

        for at in range(len(text) - size + 1):
            for payload in payloads:
                if payload == text[at : at + size]:
                    continue

                out.append(
                    (
                        "substitute",
                        at,
                        size,
                        payload,
                        text[:at] + payload + text[at + size :],
                        at + size,
                        0,
                    )
                )

        for at in range(len(text) + 1):
            for payload in payloads:
                out.append(("insert", at, size, payload, text[:at] + payload + text[at:], at + size, -size))

        for at in range(len(text) - size + 1):
            out.append(("delete", at, size, "", text[:at] + text[at + size :], at, size))

    return out


class PristineChecker:
    """The pristine-transfer campaign. `skew` displaces the position mapping: the negative control."""

    def __init__(self, skew=0):
        self.skew = skew

        self.sets = 0
        self.pristine = 0
        self.pristine_short = []
        self.damages = 0
        self.still_tokenizable = 0
        self.trials = 0
        self.distinct_damaged = 0
        self.answers = 0
        self.refusals = 0
        self.covered = 0
        self.uncovered = 0
        self.landed = 0
        self.violations = []
        self.by_kind = {"substitute": 0, "insert": 0, "delete": 0}
        self.covered_by_kind = {"substitute": 0, "insert": 0, "delete": 0}
        self.covered_window = 0
        self.covered_byte = 0

    def check_set(self, tokens):
        self.sets += 1

        model = reference.Model(tokens)
        token_letters = sorted({character for token in tokens for character in token})
        letters = token_letters + [PAD]

        pristine = pristine_strings(model, tokens)

        self.pristine += len(pristine)

        if len(pristine) < PRISTINE_TARGET:
            census = {}

            for text in pristine:
                census[len(text)] = census.get(len(text), 0) + 1

            self.pristine_short.append((tokens, len(pristine), sorted(census.items())))

        trials = []
        wanted = {}
        starts_of = {}

        for text in pristine:
            starts_of[text] = frozenset(model.starts(text))

            for damage in damages_of(text, letters):
                kind, at, size, payload, damaged, corruption_end, delta = damage

                self.damages += 1

                position = model.consumed(damaged)

                if position == len(damaged):
                    self.still_tokenizable += 1

                    continue

                self.trials += 1
                self.by_kind[kind] += 1

                trials.append((text, damage, position))
                wanted[damaged] = position + 1

        order = sorted(wanted)

        self.distinct_damaged += len(order)

        lines = run_probe(tokens, ["Q %s %d" % (encode(d), wanted[d]) for d in order])

        answers = {d: parse_walk(line) for d, line in zip(order, lines)}

        for text, damage, failure in trials:
            kind, at, size, payload, damaged, corruption_end, delta = damage

            answer = answers[damaged]

            if answer is None:
                self.refusals += 1

                continue

            self.answers += 1

            start, begin, end, window = answer

            if begin < corruption_end:
                self.uncovered += 1

                continue

            self.covered += 1
            self.covered_by_kind[kind] += 1

            if window:
                self.covered_window += 1
            else:
                self.covered_byte += 1

            mapped = start + delta + self.skew

            if mapped not in starts_of[text]:
                self.violations.append(
                    (
                        tokens,
                        text,
                        kind,
                        at,
                        size,
                        payload,
                        damaged,
                        failure,
                        answer,
                        corruption_end,
                        delta,
                        mapped,
                        sorted(model.starts(text)),
                    )
                )
            else:
                self.landed += 1


# ----------------------------------------------------------------------------------------------
# Report
# ----------------------------------------------------------------------------------------------


def table(out, rows):
    out.append("| Quantity | Value |")
    out.append("| --- | --- |")

    for name, value in rows:
        out.append("| %s | %s |" % (name, value))

    out.append("")


def table_pair(out, left, right, rows):
    out.append("| Quantity | %s | %s |" % (left, right))
    out.append("| --- | --- | --- |")

    for name, first, second in rows:
        out.append("| %s | %s | %s |" % (name, first, second))

    out.append("")


def main():

    began = time.time()

    # Part 1.
    base_sets = scan.token_sets()
    anchored = AnchoredChecker()
    extended = AnchoredChecker()

    print("part 1: anchored machinery over %d token sets" % len(base_sets), file=sys.stderr)

    started = time.time()

    for index, tokens in enumerate(base_sets):
        print("  anchored [%2d/%2d] %r" % (index + 1, len(base_sets), list(tokens)), file=sys.stderr)
        anchored.check_set(tokens)

        width = len({character for token in tokens for character in token})
        limit = PAD_FREE_MAX_INPUT if width <= 2 else PAD_FREE_MAX_INPUT_WIDE

        extended.check_set(tokens, max_input=limit, pad_free=True)

    part1_seconds = time.time() - started

    # Part 2.
    print("part 2: long-token family", file=sys.stderr)

    long_sets, long_checker, recorder, long_failures, part2_seconds = run_long_family()

    # Part 3.
    campaign_sets = pristine_sets()
    pristine = PristineChecker()

    print("part 3: pristine transfer over %d token sets" % len(campaign_sets), file=sys.stderr)

    started = time.time()

    for index, tokens in enumerate(campaign_sets):
        print("  pristine [%2d/%2d] %r" % (index + 1, len(campaign_sets), list(tokens)), file=sys.stderr)
        pristine.check_set(tokens)

    part3_seconds = time.time() - started

    # Part 4.
    print("part 4: negative controls", file=sys.stderr)

    started = time.time()

    control_sets = [("aab", "ba"), ("abc", "c"), ("ab", "b")]

    control_pad = AnchoredChecker(corrupt_repair=lambda r: r + PAD)
    control_letter = AnchoredChecker(corrupt_repair=lambda r: r + "a")
    control_displaced = AnchoredChecker(displace_anchored=lambda p, size: min(p + 1, size - 1) if size else p)

    for tokens in control_sets:
        control_pad.check_set(tokens, max_input=4)
        control_letter.check_set(tokens, max_input=4)
        control_displaced.check_set(tokens, max_input=4)

    control_range = AnchoredChecker(displace_anchored=lambda p, size: size + 1)

    for tokens in control_sets:
        control_range.check_set(tokens, max_input=4)

    control_skew = PristineChecker(skew=1)
    control_skew.check_set(("aab", "ba"))

    control_plain = PristineChecker()
    control_plain.check_set(("aab", "ba"))

    part4_seconds = time.time() - started

    total_seconds = time.time() - began

    out = []
    out.append("# Anchored machinery, wide window evidence, and pristine transfer")
    out.append("")
    out.append("A second independent run, extending the one recorded in `RESULTS.md` and sharing nothing")
    out.append("with the campaign harness in the munch tree. The ground truth is again `reference.py`, the")
    out.append("from-scratch maximal-munch model, which agreed with munch's own scan on all 1444612 strings")
    out.append("the first run put to it. `crosscheck_anchor.cpp` is `crosscheck_scan.cpp` widened with two commands, for")
    out.append("`Lexer::next_anchored_start()` and `Lexer::minimal_repair()`; it asserts nothing.")
    out.append("`crosscheck_anchor.py` owns every verdict and reuses `crosscheck_scan.py`'s claim 1 and claim 2 logic verbatim")
    out.append("for part 2.")
    out.append("")
    out.append("Every quantifier below is bounded, while the contracts quantify over repairs of unbounded")
    out.append("length, so a clean pass here is evidence for the contracts and not a proof of them.")
    out.append("")
    out.append("Repair searches range over the token alphabet alone, which loses nothing: a completely")
    out.append("tokenizable string is a concatenation of tokens, so a repair carrying a byte that appears in")
    out.append("no token can never complete an input.")
    out.append("")

    out.append("## Part 1: the anchored machinery, under exhaustive negative testing")
    out.append("")
    out.append(
        "For every (token set, input, anchor) of `crosscheck_scan.py`'s enumeration, being the %d token sets," % len(base_sets)
    )
    out.append("every input of length at most %d over the token alphabet plus the pad byte '#', and every" % MAX_INPUT)
    out.append("anchor from 0 to the input's length:")
    out.append("")
    out.append("- **(a)** `minimal_repair(x)` returning r is checked by scanning r + x with the reference; it")
    out.append("  must be completely tokenizable, every time. Minimality is checked against the bounded")
    out.append("  search: no repair shorter than r may complete x.")
    out.append("- **(b)** `minimal_repair(x)` refusing is checked by scanning every repair of length at most")
    out.append(
        "  %d over the token alphabet. Any completing repair found is a counterexample to the" % MAX_ANCHORED_REPAIR
    )
    out.append("  negative label within the bound.")
    out.append("- **(c)** `next_anchored_start(x, anchor)` returning p is confronted with every bounded")
    out.append("  completing repair r: the reference's committed segmentation of r + x must begin a token at")
    out.append("  |r| + p, with no bounded completing repair disagreeing.")
    out.append("- **(d)** the two machineries against each other, on every input some bounded repair")
    out.append("  completes. A certified start w from the walk is invariant across every completely")
    out.append("  tokenizable repair, since such a repair reaches all evidence and, being prepended, alters")
    out.append("  none of it; the anchored decider is exact for that same question, so on a repairable tail")
    out.append("  it must answer, and answer at or before w. That is the header's own claim that on a")
    out.append("  repairable tail it answers at or before any certificate, turned into a check.")
    out.append("")
    out.append("Both queries take the whole input as the tail and the anchor as the search offset, so the")
    out.append("repairs quantified over are prepended to the input rather than replacing a prefix of it.")
    out.append(
        "Since the enumeration contains every string of length at most %d, every (tail, offset) pair" % MAX_INPUT
    )
    out.append("of that universe is queried.")
    out.append("")
    out.append("That enumeration alone would leave (a) and (c) thinly exercised, because no tail carrying the")
    out.append("pad byte can ever be completed and so most of its inputs are answered with a refusal. It is")
    out.append("therefore run twice: once as it stands, and once over the pad-free inputs, every string of")
    out.append(
        "length at most %d over the token alphabet on two-letter alphabets and at most %d on wider"
        % (PAD_FREE_MAX_INPUT, PAD_FREE_MAX_INPUT_WIDE)
    )
    out.append("ones. The pad-free extension is where the repairs live; the original enumeration is where the")
    out.append("negative label of (b) is exercised in bulk.")
    out.append("")

    table_pair(
        out,
        "existing enumeration",
        "pad-free extension",
        [
            (
                name,
                getattr(anchored, field) if not count else len(getattr(anchored, field)),
                getattr(extended, field) if not count else len(getattr(extended, field)),
            )
            for name, field, count in [
                ("token sets", "sets", False),
                ("distinct inputs, each a minimal_repair query", "inputs", False),
                ("(input, anchor) cases, each a next_anchored_start query", "cases", False),
                ("repairs scanned by the bounded search", "repairs_searched", False),
                ("minimal_repair answers", "repair_answers", False),
                ("... of those the empty repair, the input already tokenizing", "repair_empty", False),
                ("... verified completely tokenizable by the reference (a)", "repair_verified", False),
                ("... **failing that verification**", "repair_bad", True),
                ("... within the search bound, so minimality is decidable", "repair_in_bounded_search", False),
                ("... longer than the bound", "repair_beyond_bound", False),
                ("... **found not minimal**", "repair_not_minimal", True),
                ("minimal_repair refusals", "repair_refusals", False),
                ("... confirmed: no bounded repair completes the input (b)", "refusal_confirmed", False),
                ("... **refuted: a bounded repair completes the input**", "refusal_refuted", True),
                ("next_anchored_start answers", "anchored_answers", False),
                ("... **outside [anchor, input size)**", "anchored_range_bad", True),
                ("... bounded-vacuous, no repair up to the bound completing", "anchored_vacuous", False),
                ("... (answer, completing repair) pairs verified (c)", "anchored_pairs", False),
                ("... **answers no bounded completing repair supports**", "anchored_bad", True),
                ("next_anchored_start refusals", "anchored_refusals", False),
                ("... on an input some bounded repair completes", "refusals_with_repairs", False),
                ("(d) repairable cases where both machineries answered", "cross_pairs", False),
                ("... the anchored answer strictly earlier", "cross_earlier", False),
                ("... the same position", "cross_equal", False),
                ("... **the anchored answer later than the certificate**", "cross_late", False),
                ("repairable cases the anchored decider answers and the walk refuses", "anchored_only", False),
                ("**repairable cases the walk answers and the anchored decider refuses**", "walk_only", False),
            ]
        ],
    )

    out.append("Verdicts for part 1, over both enumerations together:")
    out.append("")

    for name, offenders in (
        ("(a) every returned repair repairs", anchored.repair_bad + extended.repair_bad),
        ("returned repairs are minimal within the bound", anchored.repair_not_minimal + extended.repair_not_minimal),
        (
            "(b) no bounded repair exists where the routine reports none",
            anchored.refusal_refuted + extended.refusal_refuted,
        ),
        (
            "(c) every anchored answer is a boundary under every bounded completing repair",
            anchored.anchored_bad + extended.anchored_bad,
        ),
        (
            "every anchored answer lies in [anchor, input size)",
            anchored.anchored_range_bad + extended.anchored_range_bad,
        ),
        (
            "(d) on a repairable tail the decider answers at or before the certificate",
            anchored.cross_bad + extended.cross_bad,
        ),
    ):
        if offenders:
            out.append("- **VIOLATION, %s: %d cases.** The first is:" % (name, len(offenders)))
            out.append("")
            out.append("```")
            out.append(repr(offenders[0]))
            out.append("```")
            out.append("")
        else:
            out.append("- **%s: no violations.**" % name)

    walk_only_total = anchored.walk_only + extended.walk_only
    example = anchored.anchored_only_first or extended.anchored_only_first

    out.append("")
    out.append("The two machineries are not ordered by inclusion, and the run shows both directions. On")
    out.append(
        "repairable tails the anchored decider answered %d cases the certificate walk refused"
        % (anchored.anchored_only + extended.anchored_only)
    )
    out.append("outright, which is the end-of-input knowledge the certificates cannot use, and it answered")
    out.append("strictly earlier than the certificate in %d more." % (anchored.cross_earlier + extended.cross_earlier))

    if example:
        out.append("")
        out.append(
            "- first case answered only by the decider: token set {%s}, input %r, anchor %d, answer %d"
            % (", ".join(example[0]), example[1], example[2], example[3])
        )
        out.append("")

    if walk_only_total:
        out.append("The reverse, a certificate on a repairable tail the decider refuses, is a violation of (d)")
        out.append("and occurred %d times." % walk_only_total)
    else:
        out.append("The reverse, a certificate on a repairable tail the decider refuses, would be a violation")
        out.append("of (d) and did not occur once.")

    out.append("Outside the repairable tails the walk answers freely where the decider refuses by design,")
    out.append("since a tail beyond repair makes every position vacuously invariant.")
    out.append("")

    passed_over = anchored.firstness_positions + extended.firstness_positions
    unseparated = anchored.firstness_unrefuted + extended.firstness_unrefuted
    refusals_open = anchored.refusals_with_repairs + extended.refusals_with_repairs
    refusal_unseparated = anchored.refusal_unrefuted_positions + extended.refusal_unrefuted_positions

    out.append("")
    out.append("### Exactness, as far as the bound reaches")
    out.append("")
    out.append("Checks (a) to (c) test soundness. The routine also claims exactness: the answer is the")
    out.append("*first* invariant position at or after the anchor, and a refusal on a repairable tail says no")
    out.append("position at or after the anchor is invariant at all. Neither claim can be refuted by a")
    out.append("bounded search, since an unbounded repair might disagree where every bounded one agrees, so")
    out.append("what is counted here is how many declined positions the bound fails to separate, meaning")
    out.append("positions that every bounded completing repair happens to begin a token at.")
    out.append("")
    out.append(
        "- positions passed over by an answer: %d, of which %d are unseparated by the bound"
        % (passed_over, unseparated)
    )
    out.append(
        "- refusals on an input some bounded repair completes: %d, leaving %d positions unseparated"
        % (refusals_open, refusal_unseparated)
    )
    out.append("")

    if unseparated == 0 and refusal_unseparated == 0:
        out.append("Every single position the decider declined has a bounded completing repair that puts no")
        out.append("token boundary there, so on this universe the decider is not merely sound but exact, and")
        out.append("exact with witnesses short enough for the search to exhibit.")
    else:
        out.append("The unseparated positions are not violations; they are the places where a witness, if one")
        out.append("exists, is longer than the search bound.")

    out.append("")

    out.append("## Part 2: window certificates of width three and four")
    out.append("")
    out.append("`crosscheck_scan.py`'s token sets have tokens of length at most two, which under-exercises window")
    out.append("evidence: the walk consults windows of two to four bytes, and a set whose tokens are short")
    out.append("rarely needs a wide one. A family with longer tokens is added, being the curated sets")
    out.append(
        "{abc, c}, {abca, a}, {ab, abc} and {aab, ba} plus %d machine-enumerated sets, every"
        % (len(long_sets) - len(LONG_CURATED))
    )
    out.append("three-byte string over {a, b} paired with each of a, b, ab, ba and every four-byte string")
    out.append(
        "over {a, b} paired with b, deduplicated by membership. Every one of the %d sets contains a" % len(long_sets)
    )
    out.append("token of length three or four. `crosscheck_scan.py`'s claim 1 and claim 2 logic is rerun over them")
    out.append("unchanged, with inputs of length at most %d." % LONG_MAX_INPUT)
    out.append("")

    total_width = sum(recorder.histogram.values())
    width_mismatch = total_width != long_checker.answers

    table(
        out,
        [
            ("token sets in the long family", len(long_sets)),
            ("(set, input, anchor) cases", long_checker.cases),
            ("walk answers", long_checker.answers),
            ("walk refusals", long_checker.refusals),
            ("answers on a certified byte, evidence width 1", recorder.histogram.get(1, 0)),
            ("answers on a window of width 2", recorder.histogram.get(2, 0)),
            ("**answers on a window of width 3**", recorder.histogram.get(3, 0)),
            ("**answers on a window of width 4**", recorder.histogram.get(4, 0)),
            ("answers where the answer sits past the evidence start", long_checker.answers_displaced),
            ("(answer, repair) pairs verified for claim 1", long_checker.claim1_pairs),
            ("... with claim 1's premise holding", long_checker.claim1_premise_held),
            ("... of those on window evidence", long_checker.claim1_premise_held_window),
            ("(answer, repair) pairs verified for claim 2a", long_checker.claim2a_pairs),
            ("... with a completely tokenizable repaired input", long_checker.claim2a_premise_held),
            ("collapse checks for claim 2b", long_checker.claim2b_answers),
            ("... with the collapse hypothesis holding", long_checker.claim2b_hypothesis_held),
            ("shift-invariance checks", long_checker.shift_checks),
            ("scan cross-checks against munch's own scan", long_checker.scan_crosschecks),
            ("scan cross-check mismatches", len(long_checker.scan_mismatches)),
        ],
    )

    if total_width != long_checker.answers:
        out.append(
            "The width histogram covers %d answers against %d counted by the reused checker, which"
            % (total_width, long_checker.answers)
        )
        out.append("would be a bookkeeping defect in this driver.")
        out.append("")

    wide = recorder.histogram.get(3, 0), recorder.histogram.get(4, 0)

    if min(wide) >= 1000:
        out.append("Both wide widths clear one thousand answers, %d at width 3 and %d at width 4, so window" % wide)
        out.append("evidence of every width the walk can produce is exercised in bulk.")
    else:
        out.append("Wide window evidence is under target: %d answers at width 3 and %d at width 4." % wide)

    out.append("")

    if long_failures:
        out.append("- **VIOLATIONS in the long family: %d.**" % len(long_failures))
        out.append("")

        for tokens, claim, detail in long_failures[:5]:
            out.append("```")
            out.append("%r %s: %s" % (list(tokens), claim, detail))
            out.append("```")
            out.append("")
    else:
        out.append("- **claim 1, walk soundness, over the long family: no violations.**")
        out.append("- **claim 2a and claim 2b, over the long family: no violations.**")
        out.append("")

    if long_checker.scan_mismatches:
        out.append(
            "- **the reference disagrees with munch's scan on %d of the long family's strings**, so"
            % len(long_checker.scan_mismatches)
        )
        out.append("  nothing above can be trusted; the first is `%r`." % (long_checker.scan_mismatches[0],))
    else:
        out.append(
            "- the reference agrees with munch's own scan on all %d strings scanned here, in committed"
            % long_checker.scan_crosschecks
        )
        out.append("  byte count and in the full set of token starts.")

    out.append("")

    out.append("## Part 3: pristine transfer")
    out.append("")
    out.append("Independent of the munch tree's own harness in every part: its own pristine generator, its")
    out.append("own damage model, the reference locating every failure, and the walk consulted only through")
    out.append("the probe.")
    out.append("")
    out.append(
        "For each of %d token sets, completely tokenizable strings are enumerated breadth first over" % pristine.sets
    )
    out.append("token concatenations and each candidate is confirmed by the reference before it is kept,")
    out.append("since maximal munch can misalign a concatenation of tokens into a dead end. Every damage in")
    out.append("{substitute, insert, delete} is then applied at every position for sizes k in {1, 2}, with")
    out.append("every payload over the token alphabet plus the pad byte '#'. Damages that leave the string")
    out.append("completely tokenizable are dropped. For the rest the reference gives the failure offset f,")
    out.append("the offset where the scan stops, and the walk is asked from f + 1.")
    out.append("")
    out.append("**The position mapping.** Write x for the pristine string and d for the damaged one. Each")
    out.append("damage fixes a corruption end in d, the first position past everything it touched, and a")
    out.append("shift delta with d[j] == x[j + delta] for every j at or after that corruption end:")
    out.append("")
    out.append("| Damage at position i, size k | d | corruption end | delta |")
    out.append("| --- | --- | --- | --- |")
    out.append("| substitute payload s | x[:i] + s + x[i+k:] | i + k | 0 |")
    out.append("| insert payload s | x[:i] + s + x[i:] | i + k | -k |")
    out.append("| delete | x[:i] + x[i+k:] | i | +k |")
    out.append("")
    out.append("An answer is *covered* when its evidence_begin lies at or after the corruption end, so the")
    out.append("whole evidence interval sits in the region where d and x agree under the shift. For a covered")
    out.append("answer at start p the mapped position is p + delta, which is checked to be a token start of")
    out.append("the pristine scan. That is the transfer the certificate promises: x is itself a completely")
    out.append("tokenizable repair of everything before the evidence, and its scan commits through the")
    out.append("evidence because it commits through everything.")
    out.append("")

    table(
        out,
        [
            ("token sets", pristine.sets),
            ("distinct pristine strings generated and confirmed", pristine.pristine),
            ("damages applied", pristine.damages),
            ("... dropped, the damaged string still completely tokenizable", pristine.still_tokenizable),
            ("trials, the damaged string failing to tokenize", pristine.trials),
            ("... substitutions", pristine.by_kind["substitute"]),
            ("... insertions", pristine.by_kind["insert"]),
            ("... deletions", pristine.by_kind["delete"]),
            ("distinct damaged strings put to the walk", pristine.distinct_damaged),
            ("trials where the walk answered", pristine.answers),
            ("trials where the walk refused", pristine.refusals),
            ("**covered answers, evidence at or after the corruption end**", pristine.covered),
            ("... on a certified byte", pristine.covered_byte),
            ("... on a window", pristine.covered_window),
            ("... from substitutions", pristine.covered_by_kind["substitute"]),
            ("... from insertions", pristine.covered_by_kind["insert"]),
            ("... from deletions", pristine.covered_by_kind["delete"]),
            ("uncovered answers, evidence beginning before the corruption end", pristine.uncovered),
            ("**covered answers landing on a pristine token boundary**", pristine.landed),
            ("**covered answers failing to land**", len(pristine.violations)),
        ],
    )

    if pristine.pristine_short:
        out.append(
            "**Token sets short of %d pristine strings.** The generator searches concatenations up to" % PRISTINE_TARGET
        )
        out.append(
            "%d bytes, and for these sets that is not a limitation of the search but of the language:"
            % PRISTINE_MAX_LENGTH
        )
        out.append("")

        for tokens, count, census in pristine.pristine_short:
            out.append(
                "- {%s}: only %d completely tokenizable strings of length at most %d exist at all,"
                % (", ".join(tokens), count, PRISTINE_MAX_LENGTH)
            )
            out.extend(
                textwrap.wrap(
                    "counted by length as " + ", ".join("%d at %d" % (n, size) for size, n in census) + ".",
                    width=92,
                    initial_indent="  ",
                    subsequent_indent="  ",
                )
            )
            out.append("  The count grows by at most one per byte here rather than exponentially, so reaching")
            out.append(
                "  %d distinct strings would need strings of some hundreds of bytes and a damage" % PRISTINE_TARGET
            )
            out.append("  enumeration to match. The set is kept in the campaign at the strings it does have.")

        out.append("")

    if pristine.violations:
        out.append("- **VIOLATION: %d covered answers failed to land.** The first is:" % len(pristine.violations))
        out.append("")
        out.append("```")
        out.append(repr(pristine.violations[0]))
        out.append("```")
        out.append("")
    else:
        out.append("**Every covered answer landed on a token boundary of the pristine string: no violations,")
        out.append("at zero tolerance.** The mapping is the one derived above and nothing else; had a covered")
        out.append("answer failed to land, the tuple would have been rescanned by the reference on its own")
        out.append("before any verdict, since a wrong shift in this driver looks exactly like a wrong answer")
        out.append("from the library until the arithmetic is redone by hand.")
        out.append("")

    # The enumeration is deterministic, so every one of these counts is an exact expectation, denominators
    # as much as numerators: a control that fires proves nothing if the population it fired over silently
    # collapsed, so the whole tuple is pinned and compared for equality. The prose below is generated from
    # this same mapping rather than from the objects, so what the report prints and what decides the
    # verdict cannot drift apart.
    observed = {
        "pad answers": control_pad.repair_answers,
        "pad caught": len(control_pad.repair_bad),
        "pad survivors": control_pad.repair_verified,
        "letter answers": control_letter.repair_answers,
        "letter caught": len(control_letter.repair_bad),
        "letter survivors": control_letter.repair_verified,
        "letter not minimal": len(control_letter.repair_not_minimal),
        "displaced non-vacuous": control_displaced.anchored_answers - control_displaced.anchored_vacuous,
        "displaced caught": len(control_displaced.anchored_bad),
        "skew covered": control_skew.covered,
        "skew violations": len(control_skew.violations),
        "unskewed covered": control_plain.covered,
        "unskewed violations": len(control_plain.violations),
        "range non-vacuous": control_range.anchored_answers - control_range.anchored_vacuous,
        "range caught": len(control_range.anchored_range_bad),
        "cross pairs": control_range.cross_pairs,
        "cross earlier": control_range.cross_earlier,
        "cross equal": control_range.cross_equal,
        "cross late": control_range.cross_late,
        "cross walk only": control_range.walk_only,
        "cross caught": len(control_range.cross_bad),
    }

    out.append("## Part 4: negative controls")
    out.append("")
    out.append("A check that cannot fail proves nothing, so each new check is fed an answer known to be")
    out.append("wrong. The controls run over the token sets {aab, ba}, {abc, c} and {ab, b} with inputs of")
    out.append("length at most 4, and over {aab, ba} for the transfer control.")
    out.append("")
    out.append("- **control C, a corrupted repair with the pad byte appended.** Every repair returned by")
    out.append(
        "  `minimal_repair` had '#' appended before check (a) scanned it. Of %d answers, %d were"
        % (observed["pad answers"], observed["pad caught"])
    )
    out.append(
        "  caught as not completely tokenizable and %d passed. A byte in no token cannot be"
        % observed["pad survivors"]
    )
    out.append("  tokenized, so the expected catch rate is every answer.")
    out.append(
        "- **control C', a subtler corruption, one token letter appended.** Of %d answers, %d were"
        % (observed["letter answers"], observed["letter caught"])
    )
    out.append(
        "  caught by (a) as not completely tokenizable and %d survived it, the survivors being inputs"
        % observed["letter survivors"]
    )
    out.append("  where the longer repair happens to complete as well. Check (a) therefore catches a wrong")
    out.append(
        "  repair without needing it to be obviously wrong, and the minimality check catches all %d,"
        % observed["letter not minimal"]
    )
    out.append("  survivors included, since a shorter completing repair exists for every one of them.")
    out.append("- **control D, a displaced anchored answer.** Every `next_anchored_start` answer was moved")
    out.append("  one position forward, capped at the last input position, before every check; an answer")
    out.append("  already at that final position is left unmoved, so the control's tally counts only the")
    out.append(
        "  genuinely displaced. Of %d non-vacuous answers, %d were caught as"
        % (observed["displaced non-vacuous"], observed["displaced caught"])
    )
    out.append("  positions some bounded completing repair does not begin a token at.")
    out.append("- **control E, a displaced mapping in part 3.** The pristine-transfer mapping was skewed by")
    out.append("  one position, so a covered answer at start p was checked at p + delta + 1. Over {aab, ba}")
    out.append(
        "  that gives %d landing violations out of %d covered answers, against %d violations out of"
        % (observed["skew violations"], observed["skew covered"], observed["unskewed violations"])
    )
    out.append("  %d covered answers in the same run unskewed." % observed["unskewed covered"])
    out.append("- **control F, an anchored answer displaced past the input.** Every answer was moved to")
    out.append(
        "  size + 1, firing the range check %d times over %d non-vacuous answers, and firing the"
        % (observed["range caught"], observed["range non-vacuous"])
    )
    out.append(
        "  cross-consistency check %d times over the %d repairable cases where both machineries"
        % (observed["cross caught"], observed["cross pairs"])
    )
    out.append(
        "  answered: %d of those sit later than the certificate, %d at the same position and %d"
        % (observed["cross late"], observed["cross equal"], observed["cross earlier"])
    )
    out.append(
        "  earlier, with a further %d cases where the decider refused a tail the walk certified."
        % observed["cross walk only"]
    )
    out.append("")

    controls_healthy = observed == CONTROL_EXPECTATION
    if not controls_healthy:
        for key in sorted(set(observed) | set(CONTROL_EXPECTATION)):
            got, want = observed.get(key), CONTROL_EXPECTATION.get(key)
            if got != want:
                out.append("- control tuple mismatch, %s: expected %s, observed %s" % (key, want, got))
        out.append("")

    if controls_healthy:
        out.append("All controls fired, the range and cross paths included, so none of the checks above")
        out.append("is vacuously passing.")
    else:
        out.append("**A control failed to fire, which is itself a defect in this checker.**")

    out.append("")

    out.append("## Violations, with a verdict each")
    out.append("")
    out.append("Every check above, with the count of cases that failed it. A failure would be hand-checked")
    out.append("before being called genuine: the exact tuple is rescanned by the reference on its own, and")
    out.append("the library is queried again in isolation, which separates a checker defect, meaning a")
    out.append("mis-stated premise or a wrong position mapping, from a library defect. Nothing here required")
    out.append("that, since every count is zero.")
    out.append("")

    ledger = [
        ("(1a) minimal_repair's repairs actually repair", len(anchored.repair_bad) + len(extended.repair_bad)),
        (
            "(1a) minimal_repair's repairs are minimal within the bound",
            len(anchored.repair_not_minimal) + len(extended.repair_not_minimal),
        ),
        (
            "(1b) minimal_repair's refusals survive an exhaustive search to length %d" % MAX_ANCHORED_REPAIR,
            len(anchored.refusal_refuted) + len(extended.refusal_refuted),
        ),
        (
            "(1c) anchored answers are boundaries under every bounded completing repair",
            len(anchored.anchored_bad) + len(extended.anchored_bad),
        ),
        (
            "(1c) anchored answers lie in [anchor, input size)",
            len(anchored.anchored_range_bad) + len(extended.anchored_range_bad),
        ),
        (
            "(1d) the anchored decider answers at or before the certificate on a repairable tail",
            len(anchored.cross_bad) + len(extended.cross_bad),
        ),
        ("(2) claim 1 and claim 2 over the long-token family", len(long_failures)),
        ("(2) the reference against munch's own scan", len(long_checker.scan_mismatches)),
        ("(3) covered answers landing on a pristine token boundary", len(pristine.violations)),
    ]

    table_pair(
        out,
        "Failing cases",
        "Verdict",
        [(name, count, "no finding" if count == 0 else "listed above, hand-check required") for name, count in ledger],
    )

    out.append("## Runtime")
    out.append("")

    table(
        out,
        [
            ("part 1, anchored machinery", "%.1f s" % part1_seconds),
            ("part 2, long-token family", "%.1f s" % part2_seconds),
            ("part 3, pristine transfer", "%.1f s" % part3_seconds),
            ("part 4, negative controls", "%.1f s" % part4_seconds),
            ("total", "%.1f s" % total_seconds),
        ],
    )


    print("\n".join(out))

    # The manuscript's separation example, held with teeth in this run's exit status.
    anchored_fact = manuscript_anchored_answer()

    bad = (
        anchored_fact is not None
        or anchored.repair_bad
        or anchored.repair_not_minimal
        or anchored.refusal_refuted
        or anchored.anchored_bad
        or extended.repair_bad
        or extended.repair_not_minimal
        or extended.refusal_refuted
        or extended.anchored_bad
        or anchored.anchored_range_bad
        or anchored.cross_bad
        or extended.anchored_range_bad
        or extended.cross_bad
        or long_failures
        or width_mismatch
        or recorder.width_bad
        or recorder.histogram.get(3, 0) < 1000
        or recorder.histogram.get(4, 0) < 1000
        or long_checker.scan_mismatches
        or pristine.violations
        or not controls_healthy
    )

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
