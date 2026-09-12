# Auditing an Existing Scanner

`munch-audit` reads the file a lexer generator was given, flex's `.l`, re2c's blocks inside a C or C++ source, an ANTLR
4 grammar, or a Rust file whose enums derive logos's `Logos`, builds the token set each start condition scans with, and
prints what the library decides about it: the bytes and windows a parallel scan may cut at, the length of the stretches
no certificate reaches, why every other candidate fails, and what it would cost to make one certify. Nothing in the
output is estimated or sampled; every figure is a decision over the compiled tables, the same decisions
[docs/split_points.md](split_points.md) and [docs/split_windows.md](split_windows.md) derive, applied to a token set
that was written for another generator.

The tool exists because a scanner author who wants a parallel or resumable scan has a question the generator cannot
answer: is this token set one where a cut can be certified, and if not, which rule stands in the way. The answer is a
property of the token set, not of any corpus, and the report states it as such.

## Running It

```
munch-audit [options] FILE...
```

A file that opens a re2c block (`/*!re2c` or `/*!rules:re2c`) is read as re2c, one with a derive naming `Logos` as
logos, one whose first item is a grammar declaration as ANTLR, any other as flex; `--flex`, `--re2c`, `--antlr` and
`--logos` force the kind. The options follow the generators' own:

| Option | Meaning |
|---|---|
| `--flex-syntax` | re2c's `-F`: definitions as `NAME regex` lines, references as `{name}`, bare letters literal. A file holding a `NAME regex` line is read this way regardless, since only that syntax accepts one. |
| `--case-inverted` | re2c's `--case-inverted`: `"..."` is the case-insensitive literal and `'...'` the exact one. |
| `--case-insensitive` | re2c's `--case-insensitive`: both quote kinds case-insensitive. |
| `--returns NAME` | A form besides `return` through which an action returns a token: `NAME(x)` returns its first argument, `NAME = x` the expression assigned, `NAME` alone itself. Repeatable. PHP's scanners need `--returns RETURN_TOKEN` and its siblings; ninja's needs `--returns token`. |
| `--condition NAME` | Audit this start condition only. Repeatable. Without it every condition with rules is audited, INITIAL first. |
| `--windows N` | The longest window tried, 3 unless given; 4 is the planners' own limit. The enumeration tries every string of byte-class representatives up to this width, so it is the one cost that grows with the grammar. |
| `--price BYTE` | Price this byte as well as the newline and the near misses. A character, `\n`, `\t`, `\r`, `\0` or `0xHH`. Repeatable. |
| `--json` | One JSON document for the whole run instead of text. |

The exit status is 0 when every scanner and condition audited, 1 when one was refused, 2 on a command-line error.

## Reading the Report

Every start condition of every scanner gets one report. A flex file is one scanner; a re2c file holds one per block with
rules, named by the line its block opens on, and a re2c file's conditions are the ones its rules name; a Rust file holds
one per enum deriving `Logos`, named by the derive's line, with no conditions. The caveat printed above the reports is
the one that matters most for a real scanner: a certificate holds while the scanner is in that start condition, so a cut
is safe only where the condition is known. A scanner whose strings live in another condition can certify almost every
byte in INITIAL and still leave a cut inside a string unsafe.

This is the report for `tools/audit/grammars/c-like-split-friendly.l`, the study's split-friendly C-like tokenization,
where the newline is a token of its own:

```
== tools/audit/grammars/c-like-split-friendly.l
a certificate holds while the scanner is in its start condition, so a cut needs the condition known

-- scanner at line 9, condition INITIAL: 8 rules
verdict                     1 byte certifies exactly: a cut is safe at any occurrence
certified bytes             '\n'
certified modulo discarded  '\n'
discarded tokens            3: "//"[^\n]*, \n, [ \t]+
certified windows (<= 3)    16 at width 2, 187 at width 3 over 9 byte classes, 93577 once classes expand
                            "#\n" at 1
                            "\t\n" at 1
                            "\n\t" at 1
                            "\n\n" at 1
                            "\n!" at 1
                            "\n"" at 1
                            ... 197 more
mandatory core              none
anchor-free span, bytes     unbounded
anchor-free span, windows   unbounded
lag                         0
rescue-free                 yes

why candidate bytes do not certify
  IDENTIFIER                 consumes 63 candidate bytes mid-token, e.g. '0' after "A"
  NUMBER                     consumes 10 candidate bytes mid-token, e.g. '0' after "0"
  STRING                     consumes 90 candidate bytes mid-token, e.g. '\t' after """
  "//"[^\n]*                 consumes 90 candidate bytes mid-token, e.g. '/' after "/"
  [ \t]+                     consumes 2 candidate bytes mid-token, e.g. '\t' after "\t"
```

And this is `tools/audit/grammars/c-like-block-comments.l`, the conventional tokenization with block comments, which
the split-points paper found sufficient to remove every useful certificate:

```
== tools/audit/grammars/c-like-block-comments.l
a certificate holds while the scanner is in its start condition, so a cut needs the condition known

-- scanner at line 9, condition INITIAL: 8 rules
verdict                     nothing certifies up to width 3; what certifying a byte would cost is priced below
certified bytes             none
certified modulo discarded  none
discarded tokens            3: "//"[^\n]*, "/*"([^*]|\*+[^*/])*\*+"/", [ \t\n]+
certified windows (<= 3)    none
mandatory core              "*/"
anchor-free span, bytes     unbounded
lag                         unbounded
rescue-free                 not established

why candidate bytes do not certify
  IDENTIFIER                 consumes 63 candidate bytes mid-token, e.g. '0' after "A"
  NUMBER                     consumes 10 candidate bytes mid-token, e.g. '0' after "0"
  STRING                     consumes 90 candidate bytes mid-token, e.g. '\t' after """
  "//"[^\n]*                 consumes 90 candidate bytes mid-token, e.g. '/' after "/"
  "/*"([^*]|\*+[^*/])*\*+"/" consumes 91 candidate bytes mid-token, e.g. '*' after "/"
  [ \t\n]+                   consumes 3 candidate bytes mid-token, e.g. '\t' after "\t"

what it would cost to certify '\n'
  1. "/*"([^*]|\*+[^*/])*\*+"/" no longer admits '\n'
                              certifies once discarded tokens are deleted
  2. [ \t\n]+                 no longer admits '\n', and '\n' becomes a token of its own, discarded
                              certifies exactly
```

Row by row:

- **verdict**: the answer in one sentence, with where to read on.
- **certified bytes**: the bytes every occurrence of which begins a token, in every input the condition tokenizes. A
  parallel scan may cut before any occurrence with no coordination, the byte certificate of
  [docs/split_points.md](split_points.md).
- **certified modulo discarded**: the same once the tokens a parser never sees, the ones whose actions return nothing,
  are deleted from both streams. A byte certified here but not above certifies the parser's token stream while the
  raw stream may differ in where whitespace or comments are split.
- **discarded tokens**: which rules were read as returning nothing, so that a misread action is visible on the page
  rather than folded silently into the row above. Name the forms with `--returns` when a scanner returns through
  macros or an assignment.
- **certified windows**: the byte strings whose every occurrence has a token boundary at a fixed offset inside them,
  the window certificate of [docs/split_windows.md](split_windows.md); a cut falls back to these where no byte
  certifies. Counted over byte classes, bytes the tables cannot tell apart, and then once every class stands for all
  its members; the examples prefer printable bytes and each names its origin, the offset the boundary sits at.
- **mandatory core**: a byte string every certifying window provably contains, when there is one, which is where a
  planner looks for windows in an input.
- **anchor-free span, bytes and windows**: the longest run of positions a tokenizable input can carry with no
  certificate among them, the gap a chunk plan may be asked to span. Unbounded is the common answer, since no
  certificate anchors a position inside a token and an identifier or a run of blanks has no longest form; it is
  finite only when every long input is forced to carry a certificate.
- **lag** and **rescue-free**: the recovery figures, how far a scan resumed at a certified byte can trail the true
  token stream before it agrees with it, and whether the token set admits no input on which a resumed scan needs a
  rescue.
- **why candidate bytes do not certify**: for every byte the start state consumes, each rule that also consumes it
  in the middle of a token, with a shortest input after which it does. This is the row a designer reads first: the
  token named is the one to change.
- **what it would cost to certify a byte**: the one edit the analysis knows, excluding the byte from a rule's
  character sets so that the rule can no longer run across it, applied rule by rule in file order with the
  certificate recompiled after each step; a rule that spells the byte out cannot lose it and is listed as
  immovable. The steps reproduce the study's designed rows: bounding the block comment to a line certifies the
  newline once discarded tokens are deleted, and splitting the whitespace run at newlines makes it exact.

## What Is Read, and What Is Refused

The readers take the file as the generator would and refuse what the byte-level token language cannot express, with
the line that holds it, rather than read it as something else:

- flex: definitions, `%s` and `%x`, `%option`, `%{ %}` and `%top{ }` blocks, start-condition scopes `<s>{ }` with
  indented rules, the `{` on the prefix's line or the next as bison's scanners write it, `|` shared actions,
  actions running to the first line end at which their braces balance, `<<EOF>>` rules (not tokens), and
  `%option case-insensitive`, every letter of every parsed pattern folded to either case. Refused: `^` and `$`
  anchors and `/` trailing context, which condition a match on its context and are no token language.
- re2c: `/*!re2c` and `/*!rules:re2c` blocks, closed as re2c closes them (a star-slash inside a literal, a class,
  an action or a comment is content), `re2c:` configurations recorded as options and the case and flex-syntax flags
  among them honoured, `name = regex;` and flex-style definitions, conditions and `<*>`, `=>` and `:=>` transitions,
  `:=` actions, the default, end and setup rules (not tokens). The dialect is rewritten for the pattern parser and
  the rewriting is kept beside the pattern as written: bare names become `{name}`, `'abc'` becomes
  `[aA][bB][cC]`, `[^]` is spelled out. Refused: the Unicode escapes `\u`, `\U` and `\X`, which need an encoding the
  byte reading has not got, and the class difference `\`.
- flex and re2c: whether an action returns a token is read from the action's text, a bare `return` or a form named
  with `--returns`, and nothing else in the action is interpreted.
- ANTLR 4: `lexer grammar` and combined `grammar` files, modes as the start conditions, `fragment` rules as
  definitions, a reference to any lexer rule as `{NAME}`, commands per outermost alternative (`skip` and any
  `channel` discard, `type(X)` renames, `mode`, `pushMode` and `popMode` are kept as text), the literals of a
  combined grammar's parser rules as implicit tokens ahead of every explicit rule, `caseInsensitive` at the grammar
  or on a rule, actions inside rules skipped, empty alternatives making a rule optional. ANTLR reads characters, so
  `.`, negated sets, sets and ranges beyond ASCII are written for the parser as code point ranges, `\u{...}`, and
  match the UTF-8 encodings. A non-greedy loop is read where it is a regular rewrite: over one set, dot or
  character before a literal it becomes the strings avoiding that literal, the block comment's `'/*' .*? '*/'` in
  particular; over a group none of whose alternatives can begin with the literal's first byte it is the greedy loop.
  Refused: `import`, `-> more`, `EOF` inside a rule, semantic predicates `{...}?`, Unicode property classes
  `\p{...}`, a rule reaching itself, and a non-greedy loop before anything but a literal or over a group that can
  begin with it.
- logos: every enum deriving `Logos`, its `#[logos(skip ...)]` attributes as discarded rules ahead of the variants,
  `subpattern` definitions referenced as `(?&name)`, `#[token]` and `#[regex]` attributes with their callbacks
  (`logos::skip` and a closure returning `logos::Skip` discard), `priority = n`, `ignore(case)` and
  `allow_greedy`. The priority is logos's own, computed as logos 0.14 and later compute it or taken from
  `priority = n`, higher winning, and mapped onto the builder's scale. The regex is the regex crate's in Unicode
  mode, rewritten over the UTF-8 bytes the lexer scans: classes, the dot and negated classes as code point ranges,
  the flags `i`, `s` and `u` with their scoping, lazy operators as their greedy forms since logos takes the longest
  match either way. Refused: `\d`, `\w`, `\s` and `\p{...}` in Unicode mode, which need the Unicode tables, a
  non-ASCII scalar under `i`, anchors and lookaround, the flags `x`, `m`, `U` and `R`, the class operators, and a
  pattern matching only the empty string.

The grammars under `tools/audit/grammars/` are the study's rows in every syntax, and the tests hold each `.re`, `.g4`
and `.rs` file to its `.l` twin: the readers must build token sets that cut no input differently, decided by
`boundary_difference()` over every input rather than a sample.

## The JSON Form

With `--json` the run is one document: an array `files`, each with its `path`, `kind`, a `refused` message or null,
and its `scanners`, each with the `line` it opens on and its `conditions`, each with `name`, `rules`, `refused` or
null, and the `report`. A report carries the same figures as the text under the row names above, `verdict`, `exact`
and `modulo` as arrays of byte values, `discarded`, `windows` with their origins, `window_count`, `mandatory_core`,
`byte_span` and `window_span` as numbers or the string `unbounded` (the window span also `undecided` when the
windows were too many, and null when there were none), `lag`, `rescue_free`, `blame` and `prices`, tokens given as
their id and name. Byte strings are JSON strings holding each byte as the code point of its value, so a reader
recovers the bytes exactly.
