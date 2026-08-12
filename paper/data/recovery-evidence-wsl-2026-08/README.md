# Recovery quality campaign with evidence classification, 2026-08

The same deterministic campaign as `recovery-wsl-2026-08`, rerun by the harness at the commit that taught it
to replicate each certified answer's evidence: `tools/probes/src/recovery_quality.cpp` with the corpus at 256 KiB
and 1000 trials per operation and k, collected on the development machine under WSL2. The campaign measures
recovery quality, not throughput, so the machine does not gate what these figures support, exactly as for the
sibling collection. Every trial is derived from the same linear congruential generator as before, so the
shared columns of `observations.csv` reproduce the earlier collection row for row; reruns are byte-identical.

Two columns extend the earlier schema, filled for certified-strategy rows: `evidence` is the begin offset of
the certificate the walk answered through, recovered by replicating the walk (a mismatch with the library's
answer fails the run), and `minimal` is the smallest answer any certificate at or after the search start
would have produced. The position-only interface of the earlier collection could not derive either.

The summary the run printed is `summary.txt`, and its support-aware figures are the reason this collection
exists: of the campaign's 13853 certified answers, 13016 rest on evidence lying wholly in the preserved
suffix and every one of them landed, an assertion of the run rather than an observation, since the harness
now fails on any evidence-covered answer that misses; 837 rest on evidence straddling the corruption, where
the theorems bind only repairs preserving that evidence and predict nothing about the mapped-pristine
oracle, and there 447 landed. The conservative position-only band, answers at or past the corruption end
plus three, covers 12065 of the 13016. Walk-order answers were nonminimal 51 times, all on the JSON row,
each by exactly one byte.

No record here is edited retroactively; this README is the only authored file.
