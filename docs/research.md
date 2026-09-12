# **Munch as a Research Instrument**

Most lexer generators compile the automaton away; munch keeps it. That difference makes the library usable as a
laboratory bench for anyone studying tokenization itself.

**What the bench provides.** A grammar is built inline in a few lines of combinators and compiled at runtime, with no
separate generator step in the edit-run loop. The minimized DFA stays inspectable: `advance()`, `has_accept_token()`,
and the initial state are queryable, so a probe can walk the compiled tables directly. The scanner is genuine maximal
munch with real rewind behavior, not an approximation, and its semantics are pinned by the test suite. Properties of the
compiled token set are first-class queries: `is_split_point()` derives certified bytes and `is_split_window()`
decides certified split windows from the compiled token set before input exists; `chunk_boundaries()` applies the
byte certificates to supplied input unconditionally, and `chunk_boundaries_with_windows()` additionally recovers
window cuts under the window certificate's completely-tokenizable condition. `boundary_difference()` compares two
compiled token sets and decides whether any input both tokenize is cut differently, returning an input that shows it,
so a tokenizer change can be checked against every input rather than against a corpus. `anchor_free_span()` prices
the plan in advance: the longest stretch a tokenizable input can carry with no certified byte, or no origin of a
certified window from a supplied inventory, decided bounded with its value or unbounded.

**The methodology ships with the code.** The probes under `tools/probes/` show the working pattern: figures are printed
and asserted, so a drifted number fails the build rather than a reader; oracles are exhaustive over declared finite
input spaces with their occurrence counts pinned; expected-negative rows keep the oracles honest by asserting known
violation counts; positive claims carry concrete witness inputs asserted per row; and every CSV the collector writes
stamps each row with the commit and dirty state it was built from, while externally contributed runs carry that
provenance in companion metadata files beside the CSV. The pattern transfers to any certificate-flavored empirical work:
state the claim, generate the evidence, and pin it so it cannot silently rot.

The intended research user is comfortable with modern C++ and wants the tokenizer as an inspectable object; the library
deliberately covers flat regular tokenization plus the composed mode layer.
