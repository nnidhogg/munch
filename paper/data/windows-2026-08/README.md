# Window campaign, 2026-08: archival record and erratum

Campaign records collected at clean commit bed90d6: environment.txt, summary.txt, windows.txt, windows.csv,
modes.txt, modes.csv, and observations.csv; every CSV row additionally stamps its own run, commit, and dirty
flag. corpus.txt is different: a byte-level corpus-statistics preview from a DEV RUN at
commit eeb9afe with uncommitted changes present, kept for its per-file byte statistics after that run's
throughput observations were discarded per protocol, as its own header states. No record is edited
retroactively; this README is the only authored file here.

Erratum: the capture label "at construction time" in windows.txt and corpus.txt names a timed interval that
begins only after the automaton and lexer are built. The measured interval is post-construction enumeration of
the two-byte window table from the compiled tables; probe versions after v1.3.2 print "post-construction
analysis" for the same interval. The timing figures themselves are unaffected by the label.
