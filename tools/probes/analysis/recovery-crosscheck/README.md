# Recovery cross-check

An implementation-fidelity check for the certified-recovery results: a from-scratch maximal-munch
reference model is compared against the shipped munch library over bounded exhaustive token-set
sweeps, and any disagreement fails the run. It is the executable backing for the paper's claim that
its proofs, stated over the recovery semantics, describe the code munch actually ships.

## Why the reference is Python

`reference.py` is written from scratch against only munch's public API doc comments, in a different
language from the library on purpose. Agreement between two implementations that share a language,
a compiler, and the same idioms can be a shared-idiom coincidence; agreement between an independent
Python model and the compiled C++ library is evidence. Only the probes that call munch's API are
C++, as they must be.

## Layout

- `reference.py`, the independent maximal-munch scanner and its own self-checks.
- `crosscheck_scan.py`, walk soundness and the collapse proposition. Enumerates small non-nullable literal
  token sets and every input and repair up to a bounded length, runs the reference against the
  library through the scan probe, and fails on any disagreement or on a raised claim its premise
  does not license.
- `crosscheck_anchor.py`, the anchored decider: the library's `next_anchored_start` against the reference,
  including that the decider never answers later than the walk where both answer.
- The C++ probes that expose munch's API live in `../../src/crosscheck_scan.cpp` and
  `../../src/crosscheck_anchor.cpp`, built as `munch_crosscheck_scan` and `munch_crosscheck_anchor`.

## Running

Through the test suite, where the library is built:

```
ctest --test-dir <build> -R munch_crosscheck
```

Or directly, passing the built probe as the first argument:

```
python3 crosscheck_scan.py  <build>/tools/probes/munch_crosscheck_scan
python3 crosscheck_anchor.py <build>/tools/probes/munch_crosscheck_anchor
```

The sweep bounds are `MAX_INPUT` and `MAX_REPAIR` at the top of each driver; the committed values are
the ones the paper's reported denominators are taken from.
