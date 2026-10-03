#!/usr/bin/env python3
"""The exact decision procedure for the evidence-reaching quantifier, checked and tabulated.

    python3 repair_profile.py [OUTDIR]

Section "Deciding the quantifier" of paper/panic-mode/panic-mode.tex quotes nothing from this program by hand: the
program writes the section's tables and every number its prose carries into OUTDIR (default: ../data/exact beside this
script) and the CMake test compares that output byte for byte against the committed files. It needs only the standard
library.

What it computes, for one deterministic token automaton and one preserved tail t of length m:

  - the last-accept recurrence L_i(q), the jump forest J, the terminals T;
  - the representative repairs, the empty one plus a shortest positive word to each positively reachable state whose
    run over the tail accepts somewhere;
  - the profile E, F, g, C and the at-most-three repair basis;
  - a shortest reaching repair and a shortest counterrepair to a proposed position.

What it checks, exiting nonzero on any disagreement:

  - every decision against a brute-force enumeration of all repairs up to a length bound, scanned by a reference
    maximal-munch scanner written here, over random partial automata and random literal vocabularies;
  - the two tight families at every state count up to a bound, by brute force at the small counts;
  - the three-token basis example, including that its three repairs are the only reaching behaviours;
  - the shipped routines, transcribed from libs/dfa/src/recovery.cpp, against the h = m case of the construction, an
    accepting start unrolled first as the library's Simulator does it, and the empty tail's trivial answers.

The CMake test checks this transcription; the C++ itself is bound by munch_crosscheck_anchor in tools/probes.
"""
import itertools
import os
import random
import sys
from collections import deque

EPSILON = r'$\varepsilon$'


class Dfa:
    """A partial deterministic automaton over single-character symbols; state 0 is initial."""

    def __init__(self, n, accepting, delta):
        self.n = n
        self.accepting = frozenset(accepting)
        self.delta = dict(delta)
        self.alphabet = sorted({sym for (_, sym) in delta})

    def step(self, state, sym):
        return self.delta.get((state, sym))


def unroll(dfa):
    """The library's positive-width equivalent: a fresh nonaccepting start carrying the old start's transitions.

    The fresh start takes index 0 and every given state moves up by one, so the initial state stays 0 here; the
    library instead appends the fresh start, which changes no decision.
    """
    if 0 not in dfa.accepting:
        return dfa
    delta = {(q + 1, sym): to + 1 for ((q, sym), to) in dfa.delta.items()}
    delta.update({(0, sym): to + 1 for ((q, sym), to) in dfa.delta.items() if q == 0})
    return Dfa(dfa.n + 1, {q + 1 for q in dfa.accepting}, delta)


def literal_dfa(tokens):
    """The trie automaton of a literal vocabulary, every token a word over its characters."""
    states = {'': 0}
    delta = {}
    accepting = set()
    for token in sorted(tokens):
        for i in range(1, len(token) + 1):
            prefix = token[:i]
            if prefix not in states:
                states[prefix] = len(states)
            delta[(states[token[:i - 1]], token[i - 1])] = states[prefix]
        accepting.add(states[token])
    return Dfa(len(states), accepting, delta)


def scan(dfa, word):
    """Maximal munch with positive-width tokens: the committed (start, end) pairs, in order."""
    tokens = []
    pos = 0
    while pos < len(word):
        state = 0
        last = None
        for i in range(pos, len(word)):
            state = dfa.step(state, word[i])
            if state is None:
                break
            if state in dfa.accepting:
                last = i + 1
        if last is None:
            break
        tokens.append((pos, last))
        pos = last
    return tokens


def observe(dfa, repair, tail):
    """The relative terminal e(r) and the in-tail committed starts B(r) of the repaired input."""
    tokens = scan(dfa, repair + tail)
    con = tokens[-1][1] if tokens else 0
    starts = frozenset(s - len(repair) for (s, _) in tokens if s >= len(repair))
    return con - len(repair), starts


class Analyzer:
    """The proved construction: representatives, forest, profile and basis for one tail."""

    def __init__(self, dfa, tail):
        self.dfa = dfa
        self.tail = tail
        m = len(tail)
        self.m = m
        # The last-accept recurrence, two rolling rows; J and the final row L_0 are what survives.
        row = [m if q in dfa.accepting else None for q in range(dfa.n)]
        self.jump = [None] * m
        self.cells = 0
        for i in range(m - 1, -1, -1):
            new = [None] * dfa.n
            for q in range(dfa.n):
                self.cells += 1
                nxt = dfa.step(q, tail[i])
                later = row[nxt] if nxt is not None else None
                if later is not None:
                    new[q] = later
                elif q in dfa.accepting:
                    new[q] = i
            self.jump[i] = new[0] if new[0] is not None and new[0] > i else None
            row = new
        self.last0 = row
        self.terminal = [None] * (m + 1)
        self.terminal[m] = m
        for i in range(m - 1, -1, -1):
            self.terminal[i] = self.terminal[self.jump[i]] if self.jump[i] is not None else i
        # Positive reachability, breadth first from the initial state's edges, the initial state itself unvisited.
        parent = {}
        queue = deque()
        for sym in dfa.alphabet:
            to = dfa.step(0, sym)
            if to is not None and to not in parent:
                parent[to] = (0, sym)
                queue.append(to)
        while queue:
            q = queue.popleft()
            for sym in dfa.alphabet:
                to = dfa.step(q, sym)
                if to is not None and to not in parent:
                    parent[to] = (q, sym)
                    queue.append(to)
        self.parent = parent
        # Representatives: (repair word, entry offset).
        self.scenarios = [('', 0)]
        for q in sorted(parent):
            if self.last0[q] is not None:
                self.scenarios.append((self.word(q), self.last0[q]))
        self.profile()

    def word(self, q):
        """The representative's word, read back through the breadth-first parents to the initial state's edge."""
        out = []
        while True:
            q, sym = self.parent[q]
            out.append(sym)
            if q == 0:
                return ''.join(reversed(out))

    def starts(self, entry):
        """The committed starts of the fresh scan entering at the offset: its chain to the terminal, excluded."""
        chain = []
        at = entry
        while at < self.m and self.jump[at] is not None:
            chain.append(at)
            at = self.jump[at]
        return frozenset(chain)

    def profile(self):
        terminals = sorted({self.terminal[d] for (_, d) in self.scenarios}, reverse=True)
        self.E = terminals[0]
        self.F = terminals[1] if len(terminals) > 1 and terminals[1] > 0 else 0
        maximal = [(r, d) for (r, d) in self.scenarios if self.terminal[d] == self.E]
        # Counts at the maximal entries, propagated along J in increasing offset order; the first node carrying
        # every count is g, and the entries propagated with the counts identify the basis.
        count = [0] * (self.m + 1)
        carried = [[] for _ in range(self.m + 1)]
        for index, (_, d) in enumerate(maximal):
            count[d] += 1
            carried[d].append(index)
        self.g = None
        for node in range(self.m + 1):
            if count[node] == len(maximal):
                self.g = node
                break
            if node < self.m and self.jump[node] is not None and count[node]:
                count[self.jump[node]] += count[node]
                carried[self.jump[node]].extend(carried[node])
        assert self.g is not None
        self.C = self.starts(self.g)
        at_g = [index for index in carried[self.g] if maximal[index][1] == self.g]
        if at_g:
            basis = [maximal[at_g[0]]]
        else:
            children = [node for node in range(self.g) if self.jump[node] == self.g and count[node]]
            assert len(children) >= 2
            basis = [maximal[carried[children[0]][0]], maximal[carried[children[1]][0]]]
        if self.F > 0:
            basis.append(next((r, d) for (r, d) in self.scenarios if self.terminal[d] == self.F))
        self.basis = basis

    def reaching(self, h):
        return [(r, d) for (r, d) in self.scenarios if self.terminal[d] >= h]

    def invariants(self, h):
        """The nonvacuous invariant positions at endpoint h, or None when no repair reaches h."""
        reach = self.reaching(h)
        if not reach:
            return None
        common = None
        for (_, d) in reach:
            common = self.starts(d) if common is None else common & self.starts(d)
        return common

    def profile_invariants(self, h):
        if h > self.E:
            return None
        return frozenset() if h <= self.F else self.C

    def shortest_reaching(self, h):
        reach = self.reaching(h)
        return min((len(r) for (r, _) in reach), default=None)

    def shortest_counter(self, h, p):
        refuting = [len(r) for (r, d) in self.reaching(h) if p not in self.starts(d)]
        return min(refuting, default=None)

    def basis_invariants(self, h):
        reach = [(r, d) for (r, d) in self.basis if self.terminal[d] >= h]
        if not reach:
            return None
        common = None
        for (_, d) in reach:
            common = self.starts(d) if common is None else common & self.starts(d)
        return common


def shipped_jump_table(dfa, tail):
    """build_jump_table of libs/dfa/src/recovery.cpp, transcribed, counting the transitions it takes."""
    m = len(tail)
    end = [None] * m
    tokenizes = [False] * (m + 1)
    tokenizes[m] = True
    steps = 0
    for offset in range(m - 1, -1, -1):
        last = offset if 0 in dfa.accepting else None
        state = 0
        for at in range(offset, m):
            nxt = dfa.step(state, tail[at])
            if nxt is None:
                break
            steps += 1
            state = nxt
            if state in dfa.accepting:
                last = at + 1
        end[offset] = last
        # A run accepting only at its own offset reads the entry not yet set, false: no zero-width token.
        tokenizes[offset] = last is not None and tokenizes[last]
    return end, tokenizes, steps


def shipped_crossing_entries(dfa):
    """crossing_entries of recovery.cpp, transcribed: breadth first, a shortest word per state, the start unvisited."""
    seen = {}
    frontier = []

    def expand(src, via):
        for sym in dfa.alphabet:
            to = dfa.step(src, sym)
            if to is not None and to not in seen:
                seen[to] = via + sym
                frontier.append(to)

    expand(0, '')
    at = 0
    while at < len(frontier):
        expand(frontier[at], seen[frontier[at]])
        at += 1
    return sorted(seen.items())


def shipped_boundary(dfa, tail, entry):
    """scenario_boundary of recovery.cpp: the maximal run from the entry over the whole tail."""
    last = 0 if entry in dfa.accepting else None
    state = entry
    for at in range(len(tail)):
        state = dfa.step(state, tail[at])
        if state is None:
            break
        if state in dfa.accepting:
            last = at + 1
    return last


def shipped_minimal_repair(dfa, tail):
    _, tokenizes, _ = shipped_jump_table(dfa, tail)
    if tokenizes[0]:
        return ''
    best = None
    for entry, via in shipped_crossing_entries(dfa):
        boundary = shipped_boundary(dfa, tail, entry)
        if boundary is not None and tokenizes[boundary] and (best is None or len(via) < len(best)):
            best = via
    return best


def shipped_next_anchored_start(dfa, tail, start):
    if start >= len(tail):
        return None
    end, tokenizes, _ = shipped_jump_table(dfa, tail)
    votes = [0] * len(tail)
    completing = 0

    def vote(first):
        nonlocal completing
        completing += 1
        at = first
        while at < len(tail):
            votes[at] += 1
            at = end[at]

    if tokenizes[0]:
        vote(0)
    for entry, _ in shipped_crossing_entries(dfa):
        boundary = shipped_boundary(dfa, tail, entry)
        if boundary is not None and tokenizes[boundary]:
            vote(boundary)
    if completing == 0:
        return None
    for at in range(start, len(tail)):
        if votes[at] == completing:
            return at
    return None


def all_words(alphabet, upto):
    for length in range(upto + 1):
        for tup in itertools.product(alphabet, repeat=length):
            yield ''.join(tup)


class Checker:
    def __init__(self):
        self.instances = 0
        self.queries = 0
        self.scans = 0
        self.failures = []

    def fail(self, what):
        self.failures.append(what)
        print('DISAGREEMENT', what)

    def check(self, dfa, tail, bound, alphabet=None):
        """Every decision of the construction against brute force over repairs up to the length bound."""
        alphabet = alphabet or dfa.alphabet
        m = len(tail)
        if m == 0:
            return
        self.instances += 1
        analyzer = Analyzer(dfa, tail)
        observations = {}
        for r in all_words(alphabet, bound):
            e, starts = observe(dfa, r, tail)
            self.scans += 1
            observations.setdefault((e, starts), []).append(len(r))
        # Soundness: each representative is an actual repair whose observation is the predicted one.
        for (r, d) in analyzer.scenarios:
            e, starts = observe(dfa, r, tail)
            self.scans += 1
            if (e, starts) != (analyzer.terminal[d], analyzer.starts(d)) or len(r) > dfa.n:
                self.fail(('representative', tail, r, d))
        predicted = {(analyzer.terminal[d], analyzer.starts(d)) for (_, d) in analyzer.scenarios}
        for (e, starts) in observations:
            if e >= 1 and (e, starts) not in predicted:
                self.fail(('unrepresented observation', tail, e, sorted(starts)))
        for h in range(1, m + 1):
            reach = [(obs, lengths) for (obs, lengths) in observations.items() if obs[0] >= h]
            truth = None
            if reach:
                truth = frozenset.intersection(*(obs[1] for (obs, _) in reach))
            self.queries += 1
            if analyzer.invariants(h) != truth or analyzer.profile_invariants(h) != truth or \
                    analyzer.basis_invariants(h) != truth:
                self.fail(('invariants', tail, h, truth, analyzer.invariants(h), analyzer.profile_invariants(h)))
            shortest = min((min(lengths) for (_, lengths) in reach), default=None)
            claimed = analyzer.shortest_reaching(h)
            if claimed != shortest and not (shortest is None and claimed is not None and claimed > bound):
                self.fail(('shortest reaching', tail, h, shortest, claimed))
            if claimed is not None and claimed > dfa.n - 1:
                self.fail(('reaching bound', tail, h, claimed))
            for p in range(m):
                self.queries += 1
                refuting = min((min(lengths) for (obs, lengths) in reach if p not in obs[1]), default=None)
                claimed = analyzer.shortest_counter(h, p)
                if claimed != refuting and not (refuting is None and claimed is not None and claimed > bound):
                    self.fail(('shortest counterrepair', tail, h, p, refuting, claimed))
                if claimed is not None and claimed > dfa.n:
                    self.fail(('counterrepair bound', tail, h, p, claimed))
        # Complete repairs: the shipped routines are the h = m case, run on the automaton the library compiles. Its
        # representatives are the given automaton's: the same words entering at the same offsets.
        compiled = unroll(dfa)
        if compiled is not dfa:
            mirrored = Analyzer(compiled, tail)
            if sorted(mirrored.scenarios) != sorted(analyzer.scenarios) or 0 in mirrored.parent:
                self.fail(('unrolled representatives', tail, analyzer.scenarios, mirrored.scenarios))
        if shipped_minimal_repair(compiled, '') != '' or shipped_next_anchored_start(compiled, '', 0) is not None:
            self.fail(('empty tail', tail))
        complete = analyzer.shortest_reaching(m)
        shipped = shipped_minimal_repair(compiled, tail)
        if (shipped is None) != (complete is None) or (shipped is not None and len(shipped) != complete):
            self.fail(('minimal_repair', tail, shipped, complete))
        if shipped is not None and observe(dfa, shipped, tail)[0] != m:
            self.fail(('minimal_repair witness', tail, shipped))
        invariants = analyzer.invariants(m)
        for start in range(m):
            expected = None
            if invariants is not None:
                expected = min((p for p in invariants if p >= start), default=None)
            self.queries += 1
            if shipped_next_anchored_start(compiled, tail, start) != expected:
                self.fail(('next_anchored_start', tail, start, expected))
        return analyzer


def random_dfa(rng, n, alphabet, density):
    delta = {}
    for q in range(n):
        for sym in alphabet:
            if rng.random() < density:
                delta[(q, sym)] = rng.randrange(n)
    accepting = {q for q in range(n) if rng.random() < 0.5}
    if not accepting:
        accepting = {rng.randrange(n)}
    return Dfa(n, accepting, delta)


def reaching_family(n):
    """A chain of n states on a, a b-loop at the last, which alone accepts: tail b needs a^(n-1)."""
    delta = {(i, 'a'): i + 1 for i in range(n - 1)}
    delta[(n - 1, 'b')] = n - 1
    return Dfa(n, {n - 1}, delta)


def counter_family(n):
    """An a-cycle of n states with q0 -b-> q1, q1 alone accepting: refuting the start at 0 of tail b needs a^n."""
    if n == 1:
        return Dfa(1, {0}, {(0, 'a'): 0})
    delta = {(i, 'a'): (i + 1) % n for i in range(n)}
    delta[(0, 'b')] = 1
    return Dfa(n, {1}, delta)


def with_accepting_start(dfa):
    return Dfa(dfa.n, dfa.accepting | {0}, dfa.delta)


def tex_set(positions):
    if not positions:
        return r'$\varnothing$'
    return '$\\{' + ','.join(str(p) for p in sorted(positions)) + '\\}$'


def tex_code(word):
    return r'\code{' + word.replace('#', r'\#') + '}'


def tex_word(word):
    return EPSILON if word == '' else tex_code(word)


def tex_tokens(dfa, word):
    return r'\,'.join(tex_code(word[s:e]) for (s, e) in scan(dfa, word))


def tex_int(value):
    return f'{value:,}'.replace(',', '{,}')


def tex_row(cells, width=120):
    """One table row, wrapped at cell boundaries so no emitted line exceeds the width."""
    lines = []
    current = ''
    for i, cell in enumerate(cells):
        piece = cell + (' &' if i + 1 < len(cells) else r' \\')
        if current and len(current) + 1 + len(piece) > width:
            lines.append(current)
            current = '    ' + piece
        else:
            current = (current + ' ' + piece) if current else piece
    lines.append(current)
    return '\n'.join(lines) + '\n'


def main():
    if len(sys.argv) > 2 or (len(sys.argv) == 2 and sys.argv[1] in ('-h', '--help')):
        print(__doc__.strip())
        return 0 if len(sys.argv) == 2 else 2
    here = os.path.dirname(os.path.abspath(__file__))
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'data', 'exact')
    os.makedirs(outdir, exist_ok=True)
    checker = Checker()
    macros = {}

    # The basis example: three literal tokens, three reaching behaviours, two needed at one endpoint, three at all.
    basis_tokens = ['aba', 'bab', 'ab']
    basis_tail = 'abababbab'
    basis_bound = 8
    dfa = literal_dfa(basis_tokens)
    analyzer = checker.check(dfa, basis_tail, basis_bound)
    observed = {}
    for r in all_words(dfa.alphabet, basis_bound):
        e, starts = observe(dfa, r, basis_tail)
        if e >= 1:
            observed.setdefault((e, starts), r)
    representatives = {(analyzer.terminal[d], analyzer.starts(d)): r for (r, d) in reversed(analyzer.scenarios)}
    if set(observed) != set(representatives) or len(observed) != 3:
        checker.fail(('basis example behaviours', sorted(observed)))
    types = sorted(representatives.items(), key=lambda item: (-item[0][0], len(item[1])))
    with open(os.path.join(outdir, 'basis-types.tex'), 'w', newline='\n') as out:
        for (e, starts), r in types:
            out.write(tex_row([tex_word(r), tex_tokens(dfa, r + basis_tail), tex_set(starts), f'${e}$']))
    rows = []
    for h in range(1, len(basis_tail) + 1):
        reach = tuple(r for ((e, _), r) in types if e >= h)
        key = (reach, analyzer.invariants(h))
        if rows and rows[-1][0] == key:
            rows[-1][1][1] = h
        else:
            rows.append((key, [h, h]))
    with open(os.path.join(outdir, 'basis-profile.tex'), 'w', newline='\n') as out:
        for (reach, invariants), (lo, hi) in rows:
            span = f'$h = {lo}$' if lo == hi else f'${lo} \\le h \\le {hi}$'
            names = ', '.join(tex_word(r) for r in reach)
            out.write(tex_row([span, names, tex_set(invariants)]))
    # Tightness: at h = 6 neither terminal-9 type alone gives {6}; at h = 1 the two together miss the empty answer.
    terminal_e = [starts for (e, starts) in representatives if e == analyzer.E]
    if analyzer.invariants(6) != frozenset({6}) or any(s == frozenset({6}) for s in terminal_e) \
            or analyzer.invariants(1) != frozenset() or frozenset.intersection(*terminal_e) != frozenset({6}):
        checker.fail(('basis tightness', analyzer.invariants(6), analyzer.invariants(1), terminal_e))
    macros['ExactBasisBound'] = str(basis_bound)

    # Worked profiles: the paper's own witnesses and the equal-endpoint example.
    profiles = [
        (['ab', ';'], 'ab;#'),
        (['ab', 'ba'], 'ab'),
        (['a', 'ba'], 'a'),
        (basis_tokens, basis_tail),
    ]
    with open(os.path.join(outdir, 'profiles.tex'), 'w', newline='\n') as out:
        for tokens, tail in profiles:
            dfa = literal_dfa(tokens)
            analyzer = checker.check(dfa, tail, 6, alphabet=sorted(set(''.join(tokens) + tail)))
            vocabulary = '$\\{' + ', '.join(tex_code(t) for t in tokens) + '\\}$'
            basis = ', '.join(tex_word(r) for (r, _) in analyzer.basis)
            out.write(tex_row([vocabulary, tex_code(tail), f'${analyzer.m}$', f'${analyzer.E}$', f'${analyzer.F}$',
                               tex_set(analyzer.C), basis]))

    # The tight families, brute force at the small state counts and the construction up to the large one.
    family_brute = 12
    family_max = 64
    for n in range(1, family_max + 1):
        for variant in (lambda d: d, with_accepting_start):
            reaching = variant(reaching_family(n))
            counter = variant(counter_family(n))
            counter_tail = 'a' if n == 1 else 'b'
            if n <= family_brute:
                checker.check(reaching, 'b', n + 1)
                checker.check(counter, counter_tail, n + 1)
            if Analyzer(reaching, 'b').shortest_reaching(1) != n - 1:
                checker.fail(('reaching family', n))
            if Analyzer(counter, counter_tail).shortest_counter(1, 0) != n:
                checker.fail(('counterrepair family', n))
    macros['ExactFamilyBrute'] = str(family_brute)
    macros['ExactFamilyMax'] = str(family_max)

    # Random automata and literal vocabularies against brute force.
    rng = random.Random(20261003)
    for _ in range(600):
        dfa = random_dfa(rng, rng.randrange(1, 5), ['a', 'b'], 0.7)
        tail = ''.join(rng.choice('ab') for _ in range(rng.randrange(1, 6)))
        checker.check(dfa, tail, 5)
    words = ['a', 'b', 'aa', 'ab', 'ba', 'bb']
    for _ in range(300):
        tokens = [w for w in words if rng.random() < 0.5] or [rng.choice(words)]
        tail = ''.join(rng.choice('ab') for _ in range(rng.randrange(1, 7)))
        checker.check(literal_dfa(tokens), tail, 5, alphabet=['a', 'b'])
    macros['ExactInstances'] = tex_int(checker.instances)
    macros['ExactQueries'] = tex_int(checker.queries)
    macros['ExactScans'] = tex_int(checker.scans)

    # Work counts: the shipped jump table against the recurrence on a unary tail.
    unary = Dfa(2, {1}, {(0, 'a'): 1, (1, 'a'): 1})
    unary_length = 2048
    _, _, steps = shipped_jump_table(unary, 'a' * unary_length)
    cells = Analyzer(unary, 'a' * unary_length).cells
    if steps != unary_length * (unary_length + 1) // 2 or cells != 2 * unary_length:
        checker.fail(('work counts', steps, cells))
    macros['ExactUnaryLength'] = tex_int(unary_length)
    macros['ExactShippedSteps'] = tex_int(steps)
    macros['ExactRecurrenceCells'] = tex_int(cells)

    with open(os.path.join(outdir, 'macros.tex'), 'w', newline='\n') as out:
        out.write('% Written by paper/panic-mode/checks/repair_profile.py; the CMake test holds it to the program.\n')
        for name in sorted(macros):
            out.write(f'\\newcommand{{\\{name}}}{{{macros[name]}}}\n')

    print(f'instances {checker.instances}, queries {checker.queries}, scans {checker.scans}, '
          f'failures {len(checker.failures)}')
    for name in sorted(macros):
        print(f'{name} = {macros[name]}')
    return 1 if checker.failures else 0


if __name__ == '__main__':
    sys.exit(main())
