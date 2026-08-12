# Malformed-input splicing measurement, 2026-08

The measurement behind the recovery report's motivation figure: on input the grammar cannot tokenize
completely, blind splicing of window-planned chunks overproduces relative to the stop-on-first-error serial
scan. The program is `tools/probes/src/malformed_splice.cpp`, whose test form pins the hazard's shape on a
deterministic corpus in continuous integration; this collection is its campaign form on real input.

The corpus is the Rust standard library's sources, `library/std/src` and `library/core/src` of
rust-lang/rust at commit `ab8058aa09e8c14b86b3d06c08ade66f863d22fe`, 934 `.rs` files concatenated in sorted
path order with a newline after each. Stage the corpus and reproduce the run, from the repository root and
against an existing build tree, with:

```
git clone --depth 1 --filter=blob:none --sparse https://github.com/rust-lang/rust /tmp/rust-corpus
git -C /tmp/rust-corpus sparse-checkout set library/std/src library/core/src
git -C /tmp/rust-corpus checkout ab8058aa09e8c14b86b3d06c08ade66f863d22fe
./build/tools/probes/munch_malformed_splice /tmp/rust-corpus .rs
```

A depth-one clone fetches the branch tip, so if the tip has moved past the pinned commit, deepen the fetch
with `git -C /tmp/rust-corpus fetch --depth 100 origin` before the checkout, or drop `--depth` entirely. The grammar is the consumption-complete C row stated in the probe, deliberately mismatched to Rust,
whose plain strings span newlines; the stream is therefore malformed under the grammar, which is the
condition the measurement exists to price. Token counts are exact integer arithmetic over deterministic
scans, so the figures are machine-independent; `output.txt` is the run's verbatim output.

The figures: the serial scan consumes 3,504,937 of 11,495,688 bytes (30.5 percent) and emits 596,401 tokens
before failing. The window plan still recovers eight chunks; five of them report short consumption, which a
caller checking the per-chunk counts would catch, and the concatenated chunk scans emit 1,276,875 tokens,
2.14 times the serial count. Full per-chunk consumption checking flags this stream; accepting later-chunk
output without it silently more than doubles the token stream.

No record here is edited retroactively; this README is the only authored file.
