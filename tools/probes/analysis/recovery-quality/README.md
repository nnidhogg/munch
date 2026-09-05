# Campaign analysis programs

The analysis side of the recovery-quality campaign, public here beside the harness that produces the
archive (`tools/probes/src/recovery_quality.cpp`, pinned per campaign revision in the data notes that
travel with the archive).

- `analyze_r6.py` audits the revision-six archive fail-closed and emits the figures the accompanying
  paper quotes: the stats ledger, the pooled table body taken up verbatim, and the landing-figure
  coordinates.
- `analyze_r6_mechanism.py` emits the coverage-mechanism figures: the overhang identity checked per row,
  its binned rates, the certificate-shape split, per-operation and per-size coverage, the uncovered
  geometry, and the bare row's within-row decomposition, and writes the overhang figure's plotted
  coordinates to the data file the paper's plot reads.
- `test_analyze_r6.py` proves the audit fail-closed by execution: a byte-identical pristine
  baseline, a mechanism baseline covering both of that program's emissions, and one hundred and
  seventy-six staged archive corruptions that must each be rejected by the guard built for them, eleven
  of them aimed at `analyze_r6_mechanism.py` rather than at the auditing analyzer, beside one
  control case staged unchanged and one commitment-boundary case, the population pinned as a
  multiset. Invoked as `test_analyze_r6.py --prove-detection` it proves its own detection as well,
  disarming one corruption so the archive reaches the analyzer unchanged and exiting 0 only when the
  ordinary run reports the resulting acceptance as a failure.

All three are deterministic and stdlib-only, refuse an optimized interpreter, and run against the
archive published with the paper; the copies shipped inside that archive match these as of the
archive's version named in its ledger; this release widens the analyzer's summary pattern to accept
the earlier wording as well, and the archive's next version carries the same copy.
