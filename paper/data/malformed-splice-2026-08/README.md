# Malformed-input splicing measurement, 2026-08

The measurement behind the recovery report's motivation figure: on input the grammar cannot tokenize
completely, blind splicing of window-planned chunks overproduces relative to the stop-on-first-error serial
scan. The program is `tools/probes/malformed_splice.cpp`, whose test form pins the hazard's shape on a
deterministic corpus in continuous integration; this collection is its campaign form on real input.

The corpus is the Rust standard library's sources, `library/std/src` and `library/core/src` of
rust-lang/rust at commit `ab8058a`, 934 `.rs` files concatenated in sorted path order with a newline after
each. The grammar is the consumption-complete C row stated in the probe, deliberately mismatched to Rust,
whose plain strings span newlines; the stream is therefore malformed under the grammar, which is the
condition the measurement exists to price. Token counts are exact integer arithmetic over deterministic
scans, so the figures are machine-independent; `output.txt` is the run's verbatim output.

The figures: the serial scan consumes 3,504,937 of 11,495,688 bytes (30.5 percent) and emits 596,401 tokens
before failing. The window plan still recovers eight chunks; five of them report short consumption, which a
caller checking the per-chunk counts would catch, and the concatenated chunk scans emit 1,276,875 tokens,
2.14 times the serial count. Full per-chunk consumption checking flags this stream; accepting later-chunk
output without it silently more than doubles the token stream.

No record here is edited retroactively; this README is the only authored file.
