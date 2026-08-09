# **Design Decisions**

[performance.md](performance.md) explains why munch is fast and [limits.md](limits.md) states what it guarantees. This
note explains what makes both possible: a small number of architectural decisions, each of which protects the value of
the others, and one principle that connects them. None of the throughput comes from code being locally clever; all of it
comes from where the work was placed.

## **Narrow the Problem First**

Everything else is downstream of one refusal: regular languages only, longest match, anchored, bytes. This is the
meta-decision that creates the space the other decisions live in. The general-purpose engines munch outruns are not
slower because their authors were less careful; they are slower because their problem statement forbids this
architecture. A capture group or a lookaround anywhere in the feature set would invalidate the whole chain below.

## **Decide Everything at Build Time**

The pipeline determinizes and minimizes each pattern, recombines them, and determinizes and minimizes once more, so
alternation, ambiguity, and priority cease to exist before the first input byte arrives. The hot loop is fast because
there are no decisions left in it, only table reads. Most systems are slow because they make decisions at run time that
were decidable earlier; the discipline here is that anything decidable at build time is decided there, without
exception, including Unicode.

## **One Representation to Build, One to Run**

The `Dfa` is maps: inspectable, comparable against definitions, exportable to Graphviz, convenient to construct and
minimize. The `Simulator` is flat tables: opaque, cache-shaped, and built once from the `Dfa` by a one-way compile. A
single representation serving both roles would serve neither. This seam is also what makes the simulator's internals
freely rewritable: nothing above it can observe anything but its results.

## **Bytes at Run Time, Unicode at Build Time**

The engine reads bytes, never code points, which keeps the alphabet at 256, which is what makes symbol equivalence
classes possible, which is what keeps a realistic transition table small enough to stay resident in cache, where the
throughput lives. `utf8::range` expands code point ranges into byte sequences while patterns are being built, so Unicode
is handled by the same decision as everything else: earlier.

Those bytes travel as plain `char`, whose signedness the standard leaves to the platform, and the code treats that as a
division of labour rather than a hazard: `char` is the carrier, used only to store, compare for equality, and hash;
the unsigned byte value is the meaning, produced by a cast at exactly the site that indexes a table or names a symbol
value, never carried around. The projection is idempotent, casting a value that already went through it changes
nothing, so no site needs to know how many conversions came before it, which is what makes the convention trackable:
signedness is decided at each point of meaning instead of threaded through every layer in between. CI builds where
plain `char` is unsigned as well as signed, and the `std::byte` test instantiations exist to make any site that skips
its cast fail to compile rather than misbehave on the other platform.

## **Layers That Can Be Tested Alone**

regex lowers to nfa, nfa determinizes to dfa, core orchestrates, and each layer holds its own suite, with fixed-seed
property tests checking the compiled simulator against the definition maps it was built from. This is a performance
decision disguised as a testing decision: optimization experiments are only affordable when every variant is validated
instantly. Cheap, safe experiments mean more attempts, and the attempts that win are found rather than guessed.

## **Work Moves Across Seams**

The pattern across every measured change in this repository: attempts to make the hot loop locally faster lost, because
the loop's cost is a single dependent table load that nothing scalar can remove, while every win came from relocating
work across a structural boundary. The pipeline moves work from run time to build time. The table compile moves it from
lookup time to construction time. The whole-input entry point moves it from per-token call overhead into one live scan.
Even the parallel headroom follows the principle: threaded chunking scales because the automaton certifies safe split
points at build time, not because the loop got faster. In an architecture-dominated system, performance improves by
moving work between seams, not by doing the same work harder, and munch is deliberately such a system.

## **The Ceiling Is a Choice**

Architecture does not make a system fast; it chooses which ceiling the system can be driven to, and discipline does the
driving. The code-generating lexer class (logos in Rust, re2c in C) chose a higher throughput ceiling and a lower
flexibility ceiling: their token sets freeze at compile time. munch chose lexers built at run time from ordinary values,
and measurably sits at that ceiling. Its nearest relatives in the same class are lexertl in C++ and the dense DFAs of
Rust's regex-automata, which arrive at the same three load-bearing techniques: a minimized automaton, byte equivalence
classes, and flat premultiplied tables. That convergence is the strongest available evidence that, within this class,
these decisions are the right ones.
