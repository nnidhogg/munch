# **Debugging and Visualization**

How to dump any NFA or DFA the library builds to Graphviz DOT, and what the rendered automata of a keyword alternation,
a floating-point literal and a minimization look like.

## **Generating Debugging Files**

Debugging is a crucial step in understanding and verifying the behavior of the lexer. The library supports generating
`.dot` files for visualizing NFAs and DFAs, which can help identify issues or optimize the tokenization process. These
files can be converted to `.svg` for easier viewing.

1. Include the appropriate `graphviz` header for NFA or DFA.
2. Call the `to_file` method from the `Graphviz` class, passing the NFA or DFA object and the desired file path to
   generate `.dot` files for debugging.

### **Converting `.dot` to `.svg`**

Use the `dot` command-line tool from Graphviz to convert `.dot` files to `.svg`:

```bash
dot -Tsvg <name>.dot -o <name>.svg
```

> **Note:** Ensure Graphviz is installed on your system before running these commands. You can download it
> from [Graphviz.org](https://graphviz.org/download/).

## **Visualizing NFAs and DFAs**

Below are examples of how an NFA and its corresponding DFA might look:

### **Keyword Alternation NFA Example**

![Keyword NFA](keyword_nfa.svg)

The NFA exactly as Thompson construction emits it for `choice(text("int"), text("if"), text("in"))`: one fresh start
state ε-fans out to every alternative, and each branch spells its keyword independently. The non-determinism is visible
from the start state, whose ε-closure reaches three different states on the same `i`.

### **Keyword Alternation DFA Example**

![Keyword DFA](keyword_dfa.svg)

The same pattern after the full pipeline. Subset construction shares the prefixes the branches spell in parallel,
collapsing the three `i` edges into one spine, and minimization merges the interchangeable accept states of `if` and
`int`. The state reached by `in` is worth a look: it is accepting yet still has an outgoing `t`, which is exactly how
the simulator implements longest-match, recording the accept and reading on.

### **Floating Point Literal NFA Example**

![Floating Point Literal NFA](floating_point_literal_nfa.svg)

The Thompson NFA for a floating point literal with an optional sign and exponent. Every combinator contributes its own
small fragment glued together with ε-transitions, which is why the raw automaton sprawls: nearly seventy states, most of
them connected by ε-edges rather than input.

### **Floating Point Literal DFA Example**

![Floating Point Literal DFA](floating_point_literal_dfa.svg)

The same literal after determinization and minimization: the ε-riddled NFA collapses into a handful of states with
purely deterministic transitions. This collapse is what the pipeline buys, and the flat tables the simulator compiles
from it are what make matching fast.

### **DFA Minimization Example**

Subset construction builds the DFA for `choice(text("let"), text("set"))` with a separate branch per alternative:

![DFA before minimization](minimization_before.svg)

Subset construction cannot merge these branches itself: it identifies states reached by the same input prefixes, and
these alternatives share none. Their redundancy lies in the shared suffix, i.e. in their futures, which is exactly what
minimization examines: it merges every pair of states no remaining input can distinguish. The two accept states are
interchangeable, and so are the interior states of the two branches pair by pair, collapsing the automaton into a single
shared chain:

![DFA after minimization](minimization_after.svg)

States accepting different tokens are never merged, so tokenization is unchanged. The builder minimizes after each
subset construction, so every DFA it produces is minimized; smaller automata also shrink the transition tables the
simulator compiles, keeping more of them in cache. The result is minimal in the usual sense when the input automaton is
trim; a subexpression denoting the empty language can leave states no input can reach acceptance from, so that case is
an exception; see [docs/limits.md](limits.md).
