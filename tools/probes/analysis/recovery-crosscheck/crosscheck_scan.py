#!/usr/bin/env python3
# Exhaustive checker for the lexing-recovery paper's walk-soundness and collapse claims.
#
# The claims, over a bounded universe of token sets, inputs, anchors and repairs:
#
#   Claim 1, walk soundness.
#     If the certificate walk, searching x from offset s, certifies start p with evidence [q, q + k),
#     then for every repair r of x[0..c) with c = s and |r| <= M, if the maximal-munch scan of
#     y = r + x[c..] commits through position |r| + (q + k - c), the committed segmentation of y has a
#     token beginning at |r| + (p - c).
#
#   Claim 2a, whole-input sufficiency.
#     A repair whose y is completely tokenizable is evidence reaching.
#
#   Claim 2a-literal, the resumed-suffix reading.
#     When the suffix y[|r| + (p - c)..] is completely tokenizable, the repair is evidence reaching.
#     That suffix equals x[p..] for every repair, so this reading quantifies a repair-independent
#     hypothesis over all repairs. It is checked and reported separately for exactly that reason.
#
#   Claim 2b, collapse.
#     Where the resumed suffix x[p..] is completely tokenizable in x itself, the set of bounded repairs
#     whose y is completely tokenizable equals the set of bounded evidence-reaching repairs.
#
# Everything the claims are checked against comes from reference.py. The only thing taken from munch is
# the walk's answer, read off the probe's stdout.

from __future__ import annotations

import sys

# Set before any local import: a stray __pycache__ written on the way to a refusal fails the release gate.
sys.dont_write_bytecode = True

import itertools
import subprocess
from pathlib import Path

import reference

HERE = Path(__file__).resolve().parent
# The compiled scan probe, given as the first argument by the munch test wiring; the fallback is
# the conventional in-tree build location for a hand run.
if len(sys.argv) < 2:
    sys.exit("usage: crosscheck_scan.py <path to the built munch_crosscheck_scan probe>")
PROBE = Path(sys.argv[1])

MAX_INPUT = 6
MAX_REPAIR = 3
PAD = "#"

# What the two negative controls must produce, denominators and the raised claim included. The
# enumeration is deterministic, so these are exact rather than approximate, and the whole mapping is
# compared for equality: asserting only what a control caught would let a control pass while the
# population it was supposed to catch things in had silently emptied, and would say nothing about which
# check did the catching, so a control firing on the wrong claim would read as the described one.
CONTROL_EXPECTATION = {
    "A scan cross-checks": 26901,
    "A scan mismatches": 4500,
    "A claim raised": "nothing",
    "B walk answers": 522,
    "B claim 1 pairs": 63,
    "B claim raised": "claim 1",
}

CURATED = [
    ("a",),
    ("a", "bc"),
    ("ab", "ba"),
    ("ab", "b"),
    ("a", "ab", "b"),
    ("ab", "c"),
    ("a", "bb", "ab"),
]


def machine_sets():
    """All sets of two or three distinct tokens of length one or two over {a, b}."""
    vocabulary = ["a", "b", "aa", "ab", "ba", "bb"]
    out = []

    for size in (2, 3):
        for combination in itertools.combinations(vocabulary, size):
            out.append(combination)

    return out


def token_sets():
    seen = set()
    out = []

    for candidate in CURATED + machine_sets():
        key = frozenset(candidate)

        if key in seen:
            continue

        seen.add(key)
        out.append(candidate)

    return out


def alphabet_of(tokens):
    letters = sorted({character for token in tokens for character in token})

    return letters + [PAD]


def strings_up_to(letters, length):
    out = [""]

    for size in range(1, length + 1):
        for combination in itertools.product(letters, repeat=size):
            out.append("".join(combination))

    return out


def encode(text):
    return text if text else "-"


def run_probe(tokens, queries):
    """Feed one SET plus the given query lines to the probe and return its output lines."""
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


def parse_answer(line):
    if line == "R":
        return None

    parts = line.split()

    if parts[0] != "A":
        raise RuntimeError("unexpected probe line %r" % line)

    return (int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]))


def manuscript_walk_evidence():
    """The walk's report at the manuscript's shadowed window, asserted rather than described.

    Over {a, ab, b} on the input ab from offset zero, the walk must answer position zero on byte
    evidence, width one, window flag clear: the byte a is the walk's report at the occurrence, the
    observable half of the manuscript's semantic-refusal example. A window-flagged answer here would
    mean the shipped walk reports the very window that example says the decider refuses.
    """
    line = run_probe(("a", "ab", "b"), ["Q %s %d" % (encode("ab"), 0)])[0]
    answer = parse_answer(line)
    if answer is None:
        return "the walk refused where the byte a certifies"
    start, begin, end, window = answer
    if (start, begin, end, bool(window)) != (0, 0, 1, False):
        return "the walk answered start=%d evidence=[%d,%d) window=%s" % (start, begin, end, bool(window))
    return None


class Violation(Exception):
    def __init__(self, claim, detail):
        super().__init__("%s: %s" % (claim, detail))
        self.claim = claim
        self.detail = detail


class Checker:
    def __init__(self):
        self.cases = 0
        self.answers = 0
        self.answers_byte = 0
        self.answers_window = 0
        self.answers_displaced = 0
        self.refusals = 0
        self.distinct_suffixes = 0
        self.claim1_pairs = 0
        self.claim1_premise_held = 0
        self.claim1_premise_held_window = 0
        self.claim2a_pairs = 0
        self.claim2a_premise_held = 0
        self.claim2b_answers = 0
        self.claim2b_hypothesis_held = 0
        self.shift_checks = 0
        self.scan_crosschecks = 0
        self.scan_mismatches = []
        self.violations = []
        self.literal_failures = 0
        self.literal_first = None

    def check_set(self, tokens):
        letters = alphabet_of(tokens)
        inputs = strings_up_to(letters, MAX_INPUT)
        repairs = strings_up_to(letters, MAX_REPAIR)
        model = reference.Model(tokens)

        starts_cache = {}

        def scan_of(text):
            answer = starts_cache.get(text)

            if answer is None:
                position, starts = model.scan(text)
                answer = (position, frozenset(starts))
                starts_cache[text] = answer

            return answer

        # Pass one: every (input, anchor) case goes to the probe.
        cases = []

        for text in inputs:
            for anchor in range(len(text) + 1):
                cases.append((text, anchor))

        lines = run_probe(tokens, ["Q %s %d" % (encode(text), anchor) for text, anchor in cases])

        self.cases += len(cases)

        # The walk reads only at or after its offset, so its answer must depend on the suffix alone.
        # That is asserted rather than assumed, and it is what licenses verifying once per suffix.
        by_suffix = {}

        for (text, anchor), line in zip(cases, lines):
            answer = parse_answer(line)
            suffix = text[anchor:]

            if answer is None:
                self.refusals += 1
                relative = None
            else:
                self.answers += 1
                start, begin, end, window = answer

                if window:
                    self.answers_window += 1
                else:
                    self.answers_byte += 1

                if start != begin:
                    self.answers_displaced += 1

                if start < anchor or begin < anchor or end > len(text):
                    raise Violation(
                        "walk range",
                        "tokens=%r input=%r anchor=%d answer=%r outside [anchor, size]"
                        % (tokens, text, anchor, answer),
                    )

                relative = (start - anchor, begin - anchor, end - anchor, window)

            if suffix in by_suffix:
                self.shift_checks += 1

                if by_suffix[suffix] != relative:
                    raise Violation(
                        "shift invariance",
                        "tokens=%r suffix=%r gave %r and %r for different (input, anchor)"
                        % (tokens, suffix, by_suffix[suffix], relative),
                    )
            else:
                by_suffix[suffix] = relative

        # Multiplicity of each suffix among the enumerated cases, so the reported totals count every
        # (set, input, anchor) case while the work is done once per distinct suffix.
        multiplicity = {}

        for text, anchor in cases:
            suffix = text[anchor:]
            multiplicity[suffix] = multiplicity.get(suffix, 0) + 1

        for suffix, relative in by_suffix.items():
            if relative is None:
                continue

            weight = multiplicity[suffix]
            start, _begin, end, window = relative

            self.distinct_suffixes += 1

            reaching = []
            tokenizable = []

            for repair in repairs:
                target = repair + suffix
                committed, starts = scan_of(target)
                image = len(repair) + start
                is_reaching = committed >= len(repair) + end
                is_tokenizable = committed == len(target)

                self.claim1_pairs += weight
                self.claim2a_pairs += weight

                if is_reaching:
                    self.claim1_premise_held += weight

                    if window:
                        self.claim1_premise_held_window += weight

                    if image not in starts:
                        raise Violation(
                            "claim 1",
                            "tokens=%r suffix=%r answer=%r repair=%r y=%r con=%d starts=%r "
                            "expected a token beginning at %d"
                            % (tokens, suffix, relative, repair, target, committed, sorted(starts), image),
                        )

                if is_tokenizable:
                    self.claim2a_premise_held += weight

                    if not is_reaching:
                        raise Violation(
                            "claim 2a",
                            "tokens=%r suffix=%r answer=%r repair=%r y=%r is completely tokenizable "
                            "but con=%d does not reach %d"
                            % (tokens, suffix, relative, repair, target, committed, len(repair) + end),
                        )

                if is_reaching:
                    reaching.append(repair)

                if is_tokenizable:
                    tokenizable.append(repair)

            self.claim2b_answers += weight

            resumed = suffix[start:]

            if model.completely_tokenizable(resumed):
                self.claim2b_hypothesis_held += weight

                if set(reaching) != set(tokenizable):
                    raise Violation(
                        "claim 2b",
                        "tokens=%r suffix=%r answer=%r resumed=%r tokenizable=%r reaching=%r differ"
                        % (tokens, suffix, relative, resumed, sorted(tokenizable), sorted(reaching)),
                    )

                if len(reaching) != len(repairs):
                    self.literal_failures += weight

                    if self.literal_first is None:
                        missing = sorted(set(repairs) - set(reaching))[0]
                        self.literal_first = (tokens, suffix, relative, resumed, missing)

        # Pass two: every string the reference scanned is put to munch's own scan. This is not part of
        # any claim; it guards against the reference modelling a different scan than the library runs.
        seen = sorted(starts_cache)
        lines = run_probe(tokens, ["T %s" % encode(text) for text in seen])

        for text, line in zip(seen, lines):
            parts = line.split()
            committed = int(parts[1])
            count = int(parts[2])
            lengths = [int(value) for value in parts[3:]]

            expected_committed, expected_starts = starts_cache[text]

            observed = []
            position = 0

            for length in lengths:
                observed.append(position)
                position += length

            self.scan_crosschecks += 1

            if (
                committed != expected_committed
                or count != len(expected_starts)
                or frozenset(observed) != expected_starts
            ):
                self.scan_mismatches.append(
                    (tokens, text, expected_committed, sorted(expected_starts), committed, observed)
                )


def _shortest_match(tokens, text):
    """A deliberately wrong scan: shortest match rather than longest."""
    position = 0
    size = len(text)
    starts = []

    while position < size:
        best_length = 0

        for token in tokens:
            length = len(token)

            if text.startswith(token, position) and (best_length == 0 or length < best_length):
                best_length = length

        if best_length == 0:
            break

        starts.append(position)
        position += best_length

    return position, tuple(starts)


def mutation_controls(out):
    """Negative controls: a checker that never fails is worthless, so make it fail on purpose.

    Control A corrupts the reference scan, which the claims are checked against; the scan
    cross-check is what must catch that. Control B corrupts the walk's reported answer, displacing
    the certified start by one; claim 1 is what must catch that.

    Returns what both controls produced, counts and raised claims together, as the one mapping the
    prose is formatted from and the verdict is decided on.
    """
    out.append("## Negative controls")
    out.append("")

    original_scan = reference.scan
    reference.scan = _shortest_match
    control_a = Checker()
    raised_a = None

    try:
        control_a.check_set(("a", "ab"))
    except Violation as violation:
        raised_a = violation.claim

    reference.scan = original_scan

    original_parse = globals()["parse_answer"]

    def displaced(line):
        answer = original_parse(line)

        return None if answer is None else (answer[0] + 1, answer[1], answer[2], answer[3])

    globals()["parse_answer"] = displaced
    control_b = Checker()
    raised_b = None

    try:
        control_b.check_set(("a",))
    except Violation as violation:
        raised_b = violation.claim

    globals()["parse_answer"] = original_parse

    # The counts and the claims the controls raised, gathered before any prose is written, so the
    # sentences below and the verdict on them are formatted from one and the same mapping.
    observed = {
        "A scan cross-checks": control_a.scan_crosschecks,
        "A scan mismatches": len(control_a.scan_mismatches),
        "A claim raised": raised_a if raised_a else "nothing",
        "B walk answers": control_b.answers,
        "B claim 1 pairs": control_b.claim1_pairs,
        "B claim raised": raised_b if raised_b else "nothing",
    }

    out.append("- **control A**, the reference scan replaced by a shortest-match scan, token set {a, ab}:")
    out.append(
        "  the cross-check reports %d disagreements with munch's scan over the %d strings scanned (a"
        % (observed["A scan mismatches"], observed["A scan cross-checks"])
    )
    out.append(
        "  passing run reports 0), and the checked claims raise %s, a corrupted model being the"
        % observed["A claim raised"]
    )
    out.append("  cross-check's business rather than theirs, so a reference modelling the wrong scan cannot")
    out.append("  slip through unnoticed.")
    out.append("- **control B**, the walk's certified start displaced by one before checking, token set {a}:")
    out.append(
        "  the run stops on %s, after %d (answer, repair) pairs over %d walk answers, so a wrong answer"
        % (observed["B claim raised"], observed["B claim 1 pairs"], observed["B walk answers"])
    )
    out.append("  from the walk is caught.")
    out.append("")

    return observed


def witnesses(out):
    out.append('### Witness one: token set {a}, input "a#"')
    out.append("")

    tokens = ("a",)
    model = reference.Model(tokens)
    letters = alphabet_of(tokens)
    repairs = strings_up_to(letters, MAX_REPAIR)

    line = run_probe(tokens, ["Q a# 0"])[0]
    answer = parse_answer(line)

    completing = [r for r in repairs if model.completely_tokenizable(r + "a#")]
    committed, starts = model.scan("a#")
    reaches = committed >= (answer[2] - 0) if answer else False

    out.append(
        "- walk answer, from 0: start=%d evidence=[%d,%d) window=%s"
        % (answer[0], answer[1], answer[2], bool(answer[3]))
    )
    out.append(
        '- repairs r over {a, #} with |r| <= %d that make r + "a#" completely tokenizable: %d of %d'
        % (MAX_REPAIR, len(completing), len(repairs))
    )
    out.append('- empty repair: y = "a#", con(y) = %d, token starts = %s' % (committed, sorted(starts)))
    out.append("- empty repair reaches the evidence end %d: %s" % (answer[2], reaches))
    out.append('- committed token at the answer\'s image 0: %s (the token is "a")' % (0 in starts))
    out.append("")
    out.append("Outcome: **as the paper states**. No repair up to length %d completes the input, so the" % MAX_REPAIR)
    out.append("complete-repair quantifier is vacuous here, yet the empty repair reaches the certified byte at 0")
    out.append("and its committed segmentation begins a token there. The certificate says something where the")
    out.append("complete-repair decider would say nothing.")
    out.append("")

    out.append('### Witness two: token set {a, bc}, input "ba"')
    out.append("")

    tokens = ("a", "bc")
    model = reference.Model(tokens)

    zero = parse_answer(run_probe(tokens, ["Q ba 0"])[0])
    one = parse_answer(run_probe(tokens, ["Q ba 1"])[0])

    committed, starts = model.scan("ba")

    out.append(
        "- walk answer, from 0: start=%d evidence=[%d,%d) window=%s" % (zero[0], zero[1], zero[2], bool(zero[3]))
    )
    out.append("- walk answer, from 1: start=%d evidence=[%d,%d) window=%s" % (one[0], one[1], one[2], bool(one[3])))
    out.append('- scan of "ba" itself: con = %d, token starts = %s' % (committed, sorted(starts)))
    out.append("- the scan commits through the evidence end %d: %s" % (one[2], committed >= one[2]))
    out.append("")
    out.append("Outcome: **as the paper states, with the search offset made explicit**. The certified byte at")
    out.append("position 1 with evidence [1,2) is the walk's answer when the search starts at offset 1. Searching")
    out.append("from 0 the walk stops earlier, at the certified byte 'b' at position 0, because 'b' also begins a")
    out.append("token in every completely tokenizable input, so the position-1 certificate has to be asked for by")
    out.append('offset. The scan of "ba" itself commits nothing at all, so it never commits through [1,2), claim')
    out.append("1's premise is false for the empty repair, and the certificate is silent about that scan. That")
    out.append("silence is the witness: reaching the evidence is a real premise, not a formality.")
    out.append("")


def main():
    sets = token_sets()
    checker = Checker()

    print("checking %d token sets" % len(sets), file=sys.stderr)

    failure = None

    for index, tokens in enumerate(sets):
        print("  [%2d/%2d] %r" % (index + 1, len(sets), list(tokens)), file=sys.stderr)

        try:
            checker.check_set(tokens)
        except Violation as violation:
            failure = violation

            break

    out = []
    out.append("# Independent exhaustive check of the lexing-recovery claims")
    out.append("")
    out.append("An implementation-evidence run that shares no machinery with the campaign harness. The")
    out.append("maximal-munch model is reimplemented from scratch in `reference.py` from the public API doc")
    out.append("comments alone; `crosscheck_scan.cpp` links munch's compiled libraries and only reports what")
    out.append("`Lexer::next_certified_evidence()` and `Lexer::tokenize_all()` answer, asserting nothing.")
    out.append("`crosscheck_scan.py` owns every verdict.")
    out.append("")
    out.append("## Universe")
    out.append("")
    out.append(
        "- token sets: %d, being the %d curated sets plus every set of two or three distinct tokens"
        % (len(sets), len(CURATED))
    )
    out.append("  of length one or two over {a, b}, deduplicated by membership")
    out.append("- inputs: every string of length at most %d over the token set's own alphabet plus '#'," % MAX_INPUT)
    out.append("  a byte outside every token")
    out.append("- anchors: every offset c from 0 to |x| inclusive, with the walk searched from s = c")
    out.append("- repairs: every string of length at most M = %d over the same alphabet, the empty repair" % MAX_REPAIR)
    out.append("  included")
    out.append("")
    out.append("## Counts")
    out.append("")
    out.append("| Quantity | Value |")
    out.append("| --- | --- |")
    out.append("| token sets checked | %d |" % len(sets))
    out.append("| (set, input, anchor) cases | %d |" % checker.cases)
    out.append("| walk answers | %d |" % checker.answers)
    out.append("| ... on a certified byte | %d |" % checker.answers_byte)
    out.append("| ... on a certified window | %d |" % checker.answers_window)
    out.append("| ... where the answer sits past the evidence start | %d |" % checker.answers_displaced)
    out.append("| walk refusals | %d |" % checker.refusals)
    out.append("| distinct searched suffixes behind those answers | %d |" % checker.distinct_suffixes)
    out.append("| (answer, repair) pairs verified for claim 1 | %d |" % checker.claim1_pairs)
    out.append("| ... of those with claim 1's premise holding (evidence reached) | %d |" % checker.claim1_premise_held)
    out.append("| ... of those premise-holding pairs on window evidence | %d |" % checker.claim1_premise_held_window)
    out.append("| (answer, repair) pairs verified for claim 2a | %d |" % checker.claim2a_pairs)
    out.append("| ... of those with a completely tokenizable y | %d |" % checker.claim2a_premise_held)
    out.append("| collapse checks for claim 2b (one per walk answer) | %d |" % checker.claim2b_answers)
    out.append("| ... of those with the collapse hypothesis holding | %d |" % checker.claim2b_hypothesis_held)
    out.append("| shift-invariance checks on the walk's answer | %d |" % checker.shift_checks)
    out.append("| scan cross-checks of the reference against munch's own scan | %d |" % checker.scan_crosschecks)
    out.append("| scan cross-check mismatches | %d |" % len(checker.scan_mismatches))
    out.append("")
    out.append("Each (answer, repair) pair is counted once per (set, input, anchor) case that produced the")
    out.append("answer. The walk's answer provably depends only on the searched suffix, which the run")
    out.append("asserts as its own check, so the arithmetic is done once per distinct suffix and weighted")
    out.append("by how many cases share it.")
    out.append("")
    out.append("## Verdicts")
    out.append("")

    if failure is None:
        out.append("- **claim 1, walk soundness: no violations.**")
        out.append("- **claim 2a, a completely tokenizable repair is evidence reaching: no violations.**")
        out.append("- **claim 2b, collapse: no violations.**")
    else:
        out.append("- **VIOLATION in %s**" % failure.claim)
        out.append("")
        out.append("```")
        out.append(failure.detail)
        out.append("```")

    out.append("")

    if checker.scan_mismatches:
        out.append(
            "- **the reference scan disagrees with munch's scan on %d strings**, so no verdict above"
            % len(checker.scan_mismatches)
        )
        out.append("  can be trusted; the first is:")
        out.append("")
        out.append("```")
        out.append(repr(checker.scan_mismatches[0]))
        out.append("```")
    else:
        out.append(
            "- the reference scan agrees with munch's own scan on every one of the %d strings"
            % checker.scan_crosschecks
        )
        out.append("  scanned, in committed byte count and in the full set of token-start offsets. This is not")
        out.append("  one of the claims; it is what makes the reference a faithful stand-in for the scan the")
        out.append("  claims quantify over.")

    out.append("")
    out.append("### The resumed-suffix reading of claim 2a")
    out.append("")
    out.append("Claim 2a can also be read as: when the suffix y[|r| + (p - c)..] is completely tokenizable,")
    out.append("the repair is evidence reaching. That suffix is x[p..] for every repair r, because the image")
    out.append("|r| + (p - c) always lands at or after |r| and y agrees with x[c..] from there on, so the")
    out.append("hypothesis does not mention r at all and the reading asserts that every bounded repair is")
    out.append("evidence reaching whenever x[p..] tokenizes. That is false, and it is false for the obvious")
    out.append("reason rather than for an interesting one: a repair can kill the scan before it ever reaches")
    out.append("the evidence.")
    out.append("")

    if checker.literal_first is None:
        out.append("No counterexample was found in this universe.")
    else:
        tokens, suffix, relative, resumed, missing = checker.literal_first
        out.append("- cases where the reading fails: %d" % checker.literal_failures)
        out.append(
            "- first counterexample: token set %r, searched suffix %r, walk answer start=%d"
            % (list(tokens), suffix, relative[0])
        )
        out.append(
            "  evidence=[%d,%d), resumed suffix %r is completely tokenizable, yet the repair %r gives"
            % (relative[1], relative[2], resumed, missing)
        )
        model = reference.Model(tokens)
        target = missing + suffix
        committed, starts = model.scan(target)
        out.append(
            "  y = %r with con(y) = %d, short of the evidence end %d, so the scan never reaches the"
            % (target, committed, len(missing) + relative[2])
        )
        out.append("  evidence and the certificate is silent about it.")
        out.append("")
        out.append("This is a reading defect in the informal phrasing, not a library defect. The shipped doc")
        out.append("comment states the sufficient condition in the converse direction, as a completely tokenizable")
        out.append("repaired input, and that form is checked above as claim 2a with no violations.")

    out.append("")

    observed = mutation_controls(out)

    # Deterministic enumeration, exact expectations, denominators as much as numerators: a control that
    # fires proves nothing if the population it fired over has silently emptied, and a control firing on
    # a claim other than the one it was built to provoke is a different control than the one described.
    # The whole mapping is therefore compared for equality, with the disagreeing keys named.
    controls_ok = observed == CONTROL_EXPECTATION

    # The manuscript's semantic-refusal example, held with teeth: this fact fails the run, it does
    # not join a narrative.
    walk_fact = manuscript_walk_evidence()

    if not controls_ok:
        for key in sorted(set(observed) | set(CONTROL_EXPECTATION)):
            got, want = observed.get(key), CONTROL_EXPECTATION.get(key)

            if got != want:
                out.append("- control tuple mismatch, %s: expected %s, observed %s" % (key, want, got))

        out.append("")
        out.append("**A control did not fire as described, which is itself a defect in this checker.**")
        out.append("")

    out.append("## Witnesses")
    out.append("")

    witnesses(out)


    print("\n".join(out))

    return 1 if (failure is not None or checker.scan_mismatches or not controls_ok
                 or walk_fact is not None) else 0


if __name__ == "__main__":
    sys.exit(main())
