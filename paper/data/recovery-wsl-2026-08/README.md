# Certified recovery quality against the panic-mode conventions, 2026-08-09

Full output (summary.txt) and every per-trial observation (observations.csv) of the corruption campaign run by
tools/probes/recovery_quality.cpp at commit 0497499, clean tree, collected 2026-08-09 under WSL2 on the
development machine: three grammar rows in the split-points study's vocabulary, 256 KiB corpora, 1000 trials per
cell over substitute, delete, and insert at 1, 4, and 16 bytes, four resume strategies compared per damaging
trial. The corruption model, the ground-truth mapping, every metric's exact definition, and the two hard
assertions the run carries (the pristine oracle and the theorem transfer at and past the seam band) are stated in
the probe's header comment, which is the collection's specification.

Unlike the throughput collections beside this one, nothing here measures time: the campaign is exact integer
arithmetic over deterministic scans, seeded constants throughout, and a rerun reproduces observations.csv byte
for byte on any machine that builds the probe. The machine and platform are recorded for provenance habit, not
because they influence a figure. Trials whose damage the grammar absorbed without a scan failure appear as
absorbed rows and are set aside by every aggregate; 13147 of 27000 trials were absorbed, concentrated in the
delete operation, which the block-comment row absorbs entirely.
