#!/usr/bin/env python3
# From-scratch reference model of a maximal-munch lexer.
#
# Written without reference to any munch implementation source: the only inputs to the design are the
# public API doc comments of Lexer::tokenize_all() and Lexer::next_certified_evidence(), which state
# that the scan is longest match, commits token by token, and reports the number of bytes tokenized.
#
# The model.
# A token set is an ordered list of literal byte strings. Priority is list order: lower index wins a
# tie between two matches of the same length. Longer matches always beat shorter ones.
#
# The scan starts at offset 0 and repeatedly picks the winning match at the current offset, commits it,
# and restarts from the end of what it committed. When no token matches at the current offset the scan
# stops there. con(y) is the number of bytes committed; y is completely tokenizable when con(y) == len(y).

from __future__ import annotations


def scan(tokens, text):
    """Maximal-munch scan of `text` under the ordered token list `tokens`.

    Returns (consumed, starts) where `starts` is the tuple of offsets at which a committed token
    begins, in scan order.
    """
    position = 0
    size = len(text)
    starts = []

    while position < size:
        best_length = 0

        for token in tokens:
            length = len(token)

            # Strictly greater keeps the earliest-registered token among equal-length matches,
            # which is the list-priority tie break.
            if length > best_length and text.startswith(token, position):
                best_length = length

        if best_length == 0:
            break

        starts.append(position)
        position += best_length

    return position, tuple(starts)


def consumed(tokens, text):
    """con(text): the number of bytes the scan commits."""
    return scan(tokens, text)[0]


def completely_tokenizable(tokens, text):
    """Whether the scan commits every byte of `text`."""
    return consumed(tokens, text) == len(text)


def token_starts(tokens, text):
    """The set of offsets at which a committed token begins."""
    return set(scan(tokens, text)[1])


class Model:
    """A token set with memoized scans, so a repeated string costs one dictionary lookup."""

    def __init__(self, tokens):
        for token in tokens:
            if len(token) == 0:
                raise ValueError("the reference model does not admit an empty token")

        self.tokens = tuple(tokens)
        self._cache = {}

    def scan(self, text):
        answer = self._cache.get(text)

        if answer is None:
            answer = scan(self.tokens, text)
            self._cache[text] = answer

        return answer

    def consumed(self, text):
        return self.scan(text)[0]

    def completely_tokenizable(self, text):
        position, _ = self.scan(text)

        return position == len(text)

    def starts(self, text):
        return self.scan(text)[1]

    def seen(self):
        return self._cache.keys()


if __name__ == "__main__":
    # Small self-checks of the model itself, run with: python3 reference.py
    assert scan(["a"], "aa") == (2, (0, 1))
    assert scan(["a"], "a#") == (1, (0,))
    assert scan(["a", "bc"], "ba") == (0, ())
    assert scan(["ab", "ba"], "bab") == (2, (0,))
    assert scan(["ab", "b"], "ab") == (2, (0,))
    assert scan(["b", "ab"], "ab") == (2, (0,))
    assert scan(["a", "ab", "b"], "ab") == (2, (0,))
    assert completely_tokenizable(["a"], "")
    assert not completely_tokenizable(["a"], "#")
    print("reference self-checks passed")
