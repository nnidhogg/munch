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

A file that opens a re2c block (`/*!re2c`, `/*!rules:re2c` or `/*!local:re2c`) is read as re2c, one with a derive
naming `Logos` as logos, one whose first item is a grammar declaration as ANTLR, any other as flex; `--flex`,
`--re2c`, `--antlr` and `--logos` force the kind. The options follow the generators' own:

| Option | Meaning |
|---|---|
| `--flex-syntax` | re2c's `-F`: definitions as `NAME regex` lines, references as `{name}`, bare letters literal. A file holding a `NAME regex` line is read this way regardless, since only that syntax accepts one. |
| `--case-inverted` | re2c's `--case-inverted`: `"..."` is the case-insensitive literal and `'...'` the exact one. |
| `--case-insensitive` | re2c's `--case-insensitive`: both quote kinds case-insensitive. |
| `--returns NAME` | A form besides `return` through which an action returns a token: `NAME(x)` returns its first argument, `NAME = x` the expression assigned, `NAME` alone itself. Repeatable. PHP's scanners need `--returns RETURN_TOKEN` and its siblings; ninja's needs `--returns token`. |
| `--include DIR` | A directory the scanner's includes are looked for in, as the compiler's `-I` names it: an angle-bracket include is looked for there alone, a quoted one beside the file including it first; one not found is refused when quoted and taken for a system header's, which defines nothing of the scanner's, when in angle brackets. Repeatable. |
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
options                     noyywrap, nodefault
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
options                     noyywrap, nodefault
verdict                     no byte certifies, and no window up to width 3 in the model; what certifying a byte would cost is priced below
certified bytes             none
certified modulo discarded  none
discarded tokens            3: "//"[^\n]*, "/*"([^*]|\*+[^*/])*\*+"/", [ \t\n]+
certified windows (<= 3)    none
mandatory core              "*/"
anchor-free span, bytes     unbounded
lag                         unbounded
rescue-free                 no: the scan rolls back and continues on "/*"

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
  2. [ \t\n]+                 the run no longer admits '\n', and '\n' becomes a token of its own, discarded
                              certifies exactly

what the shapes of the tokens consuming '\n' offer, each edit on its own
  "/*"([^*]|\*+[^*/])*\*+"/" delimited: scan the body in a start condition of its own, the opener staying here
                             certifies once discarded tokens are deleted
  every shape's edit together certifies exactly

```

Row by row:

- **options**: the options the reading was governed by, the file's own and what a reader notes of its own, absent when a
scanner declares none. Several of them decide what a rule matches, flex's `case-insensitive`, re2c's
`encoding:utf8` and the Unicode version a logos scanner's classes were taken from among them, so they stand where
the figures they governed begin.
- **verdict**: the answer in one sentence, with where to read on; where it finds no window it says so of the model
  the windows are decided in, up to the width tried, since `window_counterexample()` may certify a window the conservative
  model refuses.
- **certified bytes**: the bytes every occurrence of which begins a token, in every input the condition tokenizes. A
  parallel scan may cut before any occurrence with no coordination, the byte certificate of
  [docs/split_points.md](split_points.md).
- **certified modulo discarded**: the same once the tokens a parser never sees, the ones whose actions return nothing,
  are deleted from both streams. A byte certified here but not above certifies the parser's token stream while the
  raw stream may differ in where whitespace or comments are split.
- **discarded tokens**: which rules were read as returning nothing, so that a misread action is visible on the page
  rather than folded silently into the row above; a flex scanner without `%option nodefault` has flex's default rule
  among them, printed as `.|\n`. Name the forms with `--returns` when a scanner returns through macros or an assignment.
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
- **lag** and **rescue-free**: the recovery figures, how far a scan resumed at a certified byte can trail the true token
  stream before it agrees with it, and whether the token set admits no completely tokenizable input on which the scan
  reads past a token's end and rolls back, decided exactly, the shortest such input shown when one exists.
- **why candidate bytes do not certify**: for every byte the start state consumes, each rule that also consumes it in
  the middle of a token, with a shortest input after which it does; a start state that some nonempty input returns to is
  blamed for what it consumes on re-entry, the exemption being the entry before any input and not the state. This is the
  row a designer reads first: the token named is the one to change.
- **what it would cost to certify a byte**: the one edit the analysis knows, excluding the byte from a rule's character
  sets so that the rule can no longer run across it, applied one step at a time: after each step the certificate is
  recompiled and the consumers read again from the recompiled table, and the first of them not yet answered, in rule
  order, the order the set lists its rules and the file's for a set read from one, is narrowed next, so a rule an
  earlier edit exposes is priced as soon as it is first in that order and no rule is answered twice; the steps are
  listed in the order made. A byte no token begins with, which neither certificate reports since no occurrence of it can
  begin a token, is given a token of its own before any other edit, visible, and priced from there. A rule that cannot
  lose the byte, every word of it holding the byte in a fixed spelling or a class of the one byte, takes no step: it is
  listed as immovable when every word holds such an occurrence past the token's first byte, since no narrowing frees a
  fixed occurrence and the byte cannot certify while the token stays in any narrowed form, and as undecided when some
  word holds the byte fixed only as the token's first byte, where a token may begin with it, since the narrowing is not
  applied there and decides nothing about it. The steps reproduce the study's designed rows: bounding the block comment
  to a line certifies the newline once discarded tokens are deleted, and splitting the whitespace run at newlines makes
  it exact.
- **what the shapes of the consuming tokens offer**: the edit an author would actually make, read off each
  token's pattern. A *run* over a class the byte is in, `[ \t\n]+`, is the step above: the byte leaves the class and
  becomes a token of its own. A *terminated* token, `"//"[^\n]*\n`, can leave its terminator to the token after it, the
  terminator being whatever matches the one byte and nothing else, `\n`, `[\n]`, `\n{1}` or `[cd]{0}\n` alike, a
  repetition of exactly zero being the empty word whatever it repeats. A *delimited* token, a fixed opener followed by a
  body the byte is in, a block comment or a string, can have its body scanned in a start condition of its own with the
  opener staying, which is how scanners that certify most of their bytes are written; the opener too is whatever matches
  one fixed word, `"x"`, `[x]` or `x{1}` alike. Each such edit is tried on its own token with the rest of the set as it
  stands and its certificate reported, then every consuming token takes its own shape's edit together; a terminated
  token's fixed newline, immovable for the narrowing above, is movable here. A token whose fixed spelling holds the byte
  and has none of these shapes stays *fixed*.

## What Is Read, and What Is Refused

The readers take the file as the generator would and refuse what the byte-level token language cannot express, with
the line that holds it, rather than read it as something else:

- flex: definitions, `%s` and `%x`, `%option` lines word by word, a quoted value one word with the name it follows
  whatever blanks it holds and the word after its closing quote the next, a blank between them or none, as flex lexes
  them, `%{ %}` blocks closed wherever a line holds `%}`, `%top{ }` blocks, the section delimiters as flex lexes them,
  `%%` at the margin with anything after it on the line dropped, a comment usually, while an indented `%%` is code in
  the first section and a rule in the second, the rules section's prologue of indented code, which flex copies into the
  scanner ahead of the rules and which the first line at the margin ends, and from there a rule on every line, indented
  or not, as flex reads them, start-condition scopes `<s>{ }`, the `{` on the prefix's line or the next as bison's
  scanners write it and after it, as after the `}` closing the scope, whatever code its line holds, which flex copies
  out and drops, read to the end an action is read to, so that a comment or a brace block there runs on to a later line,
  indented comments, which are such code too, `|` shared actions, whose line flex takes unread, actions running to the
  first line end at which their braces balance, a stray close counting below zero, as flex 2.6.4's action scanner reads
  one: a brace inside a `/* */` comment or a string or character literal not counting, the literal ending at its closing
  quote or at its line's end, whichever comes first, a backslash before the newline carrying it on to the next line as C
  splices lines, a `//` comment hiding nothing, since that scanner has no state for one, so that a brace after it on the
  line counts and a quote there opens a literal, and an action opening with `%{` running to the end of the first line
  holding `%}`, no comment or literal read inside it, and `<<EOF>>` rules (not tokens). After the file's rules stands
  flex's default rule, the one it adds once the section is read, which matches one byte where no rule of the file's does
  and echoes it: read as the rule `.|\n` in every start condition, at the lowest priority and returning nothing, so that
  such a byte is a one-byte discarded token wherever the scanner stands and every byte begins a token, with the line the
  rules section ends on; `%option nodefault` drops it, since under it such a byte stops the scanner with a fatal error,
  which is what a token set answers of itself where no rule matches, and `%option default` restores it. The grammars
  under `tools/audit/grammars/` say `nodefault`, being the study's token sets, which have no such rule. A rule's pattern
  ends at the first blank outside a quote and a bracket expression, whose own first `]` is a member and whose
  `[:class:]` is one token, so that the blank of `[[:alpha:] ]+` is a member and `[^^]` is every byte but the caret,
  its members the ASCII ones whatever locale flex runs under, which is how flex fills the class, and its negation
  `[:^class:]` refused, since flex fills that one under the locale it runs under, dropping the bytes that locale
  counts in the class beside the ASCII ones, which the file does not decide, as a byte beyond ASCII in a pattern or
  a definition is refused under the case option, and once a `(?i:` group folds case anywhere in the file, written
  out, as `\xHH` or as an octal escape, since flex folds it under that locale too, giving it the other case the
  locale has for it; the ASCII letters fold as the C locale folds them, each to its own other case, which every
  locale but the Turkic ones does too, where flex pairs `i` and `I` with their dotted and dotless forms instead, a
  scanner generated under one of those being outside the reading; a `[:` in any other
  shape is the `[` and the `:` as members, as flex lexes them, which leaves `[[:al]pha:]` the bracket `[[:al]` and the
  text `pha:]`. Since flex sets every option before it parses a rule, a setting is the one the last
  `%option` word naming it leaves standing: `caseless`, `case-insensitive`, `nocaseful` and `nocase-sensitive` turn the
  case option on and `caseful`, `case-sensitive`, `nocaseless` and `nocase-insensitive` turn it off, each further `no`
  flipping the sense again as flex lexes one, and under it every letter of every parsed pattern folds to either case, as
  `(?i:...)` groups do inside themselves. Refused: `^` and `$` anchors and `/` trailing context, which condition a match
  on its context and are no token language; a comment at the margin of the rules section, whose slash flex reads as the
  start of a rule and refuses as unrecognized, where flex's manual asks for an indented one; a start-condition scope
  never closed, a parse error to flex; the `s` and `x` group flags; a quote left open on an `%option`, `%s` or `%x`
  line, which flex refuses; a rule's action leaving a quote open at the end of a line where its braces balance, which
  flex ends the action at inside the literal without closing the code it emits for it, so that the m4 it runs stops with
  an end of file in string, and a brace, comment or `%{` block left open at the end of the file, which flex refuses as
  an end of file inside an action; `%option lex-compat` and `%option posix-compat`, flags of their own of which either
  suffices, under which a counted repetition binds the whole expression before it, so that `ab{3}` matches `ababab`; the
  options that leave flex building its tables over the 128 bytes of ASCII, `%option 7bit` and a `full` or `fast` table
  with the equivalence classes off, under which flex refuses outright a pattern that names a byte above 127, while the
  reading here is over all 256; an action that moves the bounds of its match or reruns it, which the token language has
  no place for, named with its line: `yymore()`, which appends the next match to this one, `REJECT`, which drops the
  match for the next rule's, `yyless()`, which gives the end of the match back to be matched again, `unput()`, which
  pushes a byte onto the input, `input()` or `yyinput()`, which consume bytes no rule matched, `yyterminate()`, which
  ends the scan with no token, and the calls and the `YY_FLUSH_BUFFER` that switch, push, pop, flush or restart a
  buffer, `yy_scan_string()` and `yyrestart()` among them, after which the next token is not matched against the
  input's next bytes, the action read as C reads it, token by token, a comment or a literal holding none
  of them, a call being a whole word whose next token is a parenthesis, whatever blanks or comments stand between,
  through a member of anything alike, `this->yyinput()`, an alias's `self->yyinput()` and `(self->yyinput)()`, `REJECT`
  a whole word in capitals as flex takes it, and a `|` line taken unread as flex takes it; and `%option reject` and
  `%option yymore`, which declare such a use where flex cannot see it, through a macro or code of the file's own.
  `BEGIN`, `yy_push_state`, `yy_pop_state` and `yy_top_state` change the start condition and nothing else, which the
  caveat above every report covers, so an action calling them is read as any other.
- re2c: `/*!re2c`, `/*!local:re2c`, `/*!rules:re2c` and `/*!use:re2c` blocks, named or not, closed as re2c closes them
  (a star-slash inside a literal, a class, an action or a comment is content); a rules block is a library the
  `!use:name;` directive and a use block merge, definitions, configurations and rules, and is no scanner itself; a use
  block with no name of its own takes the most recent rules block, named or not, and the name in its opener is the block
  it uses; a used block is read again where it is used, under the flags in force there, since re2c compiles a rules
  block's regexes at every point of use: one rules block can be a scanner under one encoding and another under another,
  and a configuration of the using block governs the rules it takes as well as the ones it writes, while a use block's
  own names and configurations stay in it as a local block's do; a definition another block uses is translated again
  there, out of the regex as written and under that block's own flags, for the same reason, so a definition of `[^]`
  written where the encoding was ASCII admits one byte in its own block and a whole code point in a block that turns
  UTF-8 on, and a flex-style definition is read under the flex syntax wherever it is used, while one another block
  declared that this block's flags cannot read at all is refused where a rule of this block reaches it, directly or
  through the definitions the rule names, and nowhere else, one no rule reaches being left out, an alias of it that no
  rule names along with it, and one the block itself declares is read where it stands and refused there; `re2c:`
  configurations recorded as options (a quoted value may hold a `;`) and honoured under the canonical name and the
  `flags:` aliases the manual's configuration list gives, the case flags and the encoding among them; a configuration
  governs the whole block wherever in it it stands, as re2c scopes it, and of two assignments to one name the last is
  the one that governs, so every pattern of a block is translated under what its configurations leave and a rule
  standing above an assignment reads under it like the rest, while the flex syntax has no configuration and comes from
  the command line or from the evidence of a definition written that way; `name = regex;` and flex-style definitions, a
  name followed by a blank and regex to the end of the line wherever the name stands, as re2c -F reads them, a `{` after
  the blank making the name a rule's literal instead and an action on the definition's line refused as the syntax error
  re2c answers it with, conditions, each the scanner's when any rule names it, the end rule and the empty rule counted,
  so that a scanner whose rules name conditions has no INITIAL unless a rule names one, and `<*>`, whose rules re2c
  appends to each condition's own, so that they stand in every condition a rule names and in no other, rank below a
  condition's own rules wherever they stand and are placed after every rule naming a condition, a block whose rules
  name only `<*>` refused, since re2c compiles no condition for them to stand in, the end rule `$` held as re2c holds
  it, in its words: one whose condition, `<*>` counted as one of its own, has no other rule is refused, one without
  `re2c:eof` set is refused, and `re2c:eof` set without an end rule in some condition, its own or under `<*>`, or
  in a block naming none, is refused, a block of its own held once it is read, whether or not its rules are tokens,
  and a `rules:re2c` block only where a `use:re2c` block takes it up, with the rules and configurations that block
  supplies, `=>` transitions, the
  `:=> c` shortcut rule, which carries no code and ends with its condition, `:=` actions, whose code runs on to the
  first line that begins with a character other than a blank, tags `@name` and `#name` (dropped, they match nothing),
  the default rule `*`, which re2c runs where no other rule matches, over one byte under ASCII and UTF-8 alike, where
  `[^]` is a whole code point, and at the lowest priority wherever it stands, so it is read as the class of every byte
  and placed after every other rule of its scanner, a `<*> *` after a named condition's own default rule, which beats it
  there, a second one of a block's own for a condition it already gave one refused as re2c refuses it and one a `!use:`
  directive or a use block brought in yielding to the block's own in every condition the own one stands in, and the end,
  setup, entry `<>` and empty `""` rules (not tokens; a setup or entry rule whose code returns is refused, since re2c
  runs that code before any rule's own action); a scanner's rules name conditions or name none, never both, one holding
  a rule of each kind, a used block's rules counted, refused as re2c refuses it, which cannot mix conditions with normal
  rules, at the first rule naming none. The dialect is rewritten for the pattern parser and the rewriting is kept beside
  the pattern as written: bare names become `{name}`, `'abc'` becomes `[aA][bB][cC]`, `[^]` is spelled out as the bytes
  it admits, and a class difference `A \ B` becomes the class of the code points left, its operands the char sets re2c
  takes there, a bracket, the dot, a one-character literal, a name defined as one of these or a group of alternatives
  that each are, and on either side the whole term as re2c's grammar has it, so that a concatenation, a repetition or a
  two-character literal beside one, `[a-z] \ [x] [y]` or `[a-z] \ "xy"`, is refused as re2c refuses it, which can only
  difference char sets, and a difference leaving no code point is refused under the configuration the block leaves and
  no other, `[^] \ [\x00-\xff]` being every code point past the bytes once the block turns UTF-8 on. Under the UTF-8
  encoding a pattern names code points and the scanner reads their encodings, so a class, the dot, `[^]`, which is any
  code point and not any byte, and a class difference, subtracted over code points before any encoding, so that `[^] \
  [\x00-\x7f]` is every code point beyond ASCII, become the code point ranges they admit, written as `\u{...}` members
  with the three bytes of the surrogates beside them where the set holds those, which re2c's default encoding policy
  encodes like any other code point; an all-ASCII class or literal stands as it is, its encoding being itself. Refused:
  `!include`, whose file is not there to read; the Unicode escapes `\u`, `\U` and `\X`, which need an encoding the byte
  reading has not got, and a braced hexadecimal escape `\x{...}`, which re2c has no form for and answers with a syntax
  error, its own being `\xHH`; the encodings a reading over bytes cannot follow, wherever they come from, a
  configuration or the flags the caller passes, each with its own reason, EBCDIC giving a byte another code point than
  ASCII does and UCS-2, UTF-16 and UTF-32 having a code unit of more than one byte; an `encoding-policy` other than the
  default, which leaves the surrogates matched otherwise; and a byte beyond ASCII written straight into the source under
  UTF-8, since the code points it stands for are the `--input-encoding` option's to say and no file carries it. What a
  command line asks for beyond those flags is beyond the reading: an `--encoding-policy` there is taken to be the
  default one, as an `--input-encoding` is taken to be ASCII.
- flex and re2c: whether an action returns a token is read from the action's text as C reads it, a `return` or a form
  named with `--returns` standing outside every comment and literal, on every path through the action, else the action
  is refused; the token is named by the expression returned as it is written, `7`, `IDENT` or `yyleng == 1 ? 7 : 8`,
  since a rule is one token to the certificates whatever value its action computes; and, for a flex action, whether it
  makes one of the calls named above, and for a re2c action whether it moves one of the scan pointers, `YYCURSOR`,
  `YYMARKER` or `YYCTXMARKER` under whatever names the block's configurations give them, which leaves the next token
  beginning elsewhere than where the match ended, or hands one on, by its address, a reference bound to it or a call it
  is passed to, or indexes the array a pointer is configured in by an expression the reading does not evaluate, each
  of which is refused; and how the action leaves, since re2c writes the actions one after another and control falls
  from an action's end into the next rule's: an action returns on every path, or leaves by `continue` or by a `goto`
  to a label before the block from which nothing but the block follows, which rescans and discards the match, or by
  a `break` after a stored token, and any other end, a `break`, a `goto` elsewhere or no jump at all, is refused.
  Nothing else in the action is interpreted.
- ANTLR 4: `lexer grammar` and combined `grammar` files, modes as the start conditions, a lexer grammar's alone, a byte
  order mark as a blank wherever it stands outside a literal, a set or an action, as ANTLR's lexer drops one, `fragment`
  rules as definitions, a reference to any lexer rule as `{NAME}`, a rule's commands, which end its single outermost
  alternative and are read as the names and arguments ANTLR's own lexer reads there, the grammar's blanks and comments
  between them no part of any command (`skip` and `type(X)` both set the token's type and a `channel` other than the
  default one sets a field of its own, the rightmost command for a field winning, so a hidden channel and a type
  together leave a renamed token a parser never sees, the channel's argument resolved as ANTLR resolves it, `HIDDEN` and
  `DEFAULT_TOKEN_CHANNEL` its constants, a name a lexer grammar's `channels` block declares a channel of its own and
  anything else a decimal number, `00` being zero and the default channel; a type's argument a token's name or its
  number, `type(0)` in any spelling of zero setting ANTLR's value for no type, so that the token keeps the rule's own,
  its name where a lexer grammar's `tokens` block names it or the rule spells a parser literal's shape and ANTLR's type
  zero otherwise, kept as `0`; `mode`, `pushMode` and `popMode` are kept as text), so that a rule of several
  alternatives is one token whatever they are, a `tokens` or `channels` block as names parted by commas, the tokens
  block alone allowed to hold none, the literals of a combined grammar's parser rules as implicit tokens ahead of every
  explicit rule, a parser rule's argument block `[...]` skipped as ANTLR's lexer reads one, brackets nested and a quoted
  string whole, so a literal inside one is none, element options `<name=value, ...>` on a predicate, a token reference,
  a literal, a rule reference or the dot as the metadata they are, a string among their values no implicit token, unless
  a lexer rule spells the literal in a shape ANTLR maps it onto, a rule of no options and one alternative that is the
  literal alone, the literal and one action, or the literal and one or two commands of which at most one takes an
  argument, whatever comments stand in the rule, when the parser's literal is that rule's token, skipped or renamed as
  the rule says, an option's value as the one token ANTLR's lexer reads there, a name, a number, a string or a brace
  block, the comments beside it no part of it, `caseInsensitive` at the grammar or on a rule folding as ANTLR folds, a
  literal's ASCII letters and a set's members each in both cases and a range by its two ends alone, `[a-z]` gaining
  `A-Z` while `[A-t]` and `[0-Z]`, whose ends differ in case or are no letters, admit exactly what they spell, `true`
  and `false` the spellings ANTLR takes for it and any other its warning 84 that sets nothing, empty alternatives making
  a rule optional. ANTLR reads characters, so `.`, negated sets, sets and ranges beyond ASCII are written for the parser
  as code point ranges, `\u{...}`, and match the UTF-8 encodings; a high surrogate escape and a low one after it in a
  literal are the character the pair encodes, as ANTLR joins them, and a surrogate on its own, a code point no UTF-8
  input decodes to, is left out of a set as the member ANTLR never matches. A non-greedy loop is read where the rest of
  the rule spells one ASCII string and the loop stands in an outermost alternative of a rule nothing else references,
  which is where the rest of the rule is the rest the loop can see, after elements whose every match has one length in
  characters and in an alternative no earlier alternative of the rule can begin with the same character as, what an
  alternative can begin with reaching past every element of it that can match the empty string, since ANTLR follows the
  paths through a rule in the order its alternatives give them and the first path to reach the rule's end stops every
  later one that has passed the loop's decision, `('a'|'aa') .*? 'a'` and `'ab' | 'a' .*? 'c'` ending where their
  shorter path does; ANTLR's fewest characters that still let the rest match are then what stops it: over one set, dot
  or character the loop becomes the strings that hold no occurrence of that string and do not end where the string's own
  bytes would complete one, the overlap that keeps `.*? 'aa'` from matching `aaa`, the block comment's `'/*' .*? '*/'`
  reading as ever, and where the body can only be empty the string matches alone; over a literal of one length whose
  first byte that string cannot begin with, its letters folded or not, the iterations are aligned to that length and the
  greedy loop is the same language; `+?` reads one character before the rest can stop it; and a non-greedy option whose
  body cannot begin the rest and has one length in characters is the greedy one. Refused: `import`, `-> more`, `->
  type(EOF)`, which ends the token stream where the rule matches, whatever its channel, a command ANTLR has not got or
  one of its seven given an argument it takes none of or none where it takes one, its errors 149, 150 and 151 in its own
  words at the command's line, a rule defined twice, its error 51, as is a rule named `T__k` where a combined
  grammar's k-th implicit token is, a parser rule in a lexer grammar, its error 53, a rule named as its commands
  and channels reserve, DEFAULT_MODE, SKIP, MORE, EOF, HIDDEN and the rest, its error 159, a mode named as a token
  is, its error 170, a channel named as a token or a mode is, its errors 161 and 162, a `type` naming no token the
  grammar has, its
  error 175, where the grammar's tokens are its `tokens` entries, its rules that are no fragments and carry no
  `type` command or spell one literal, and `T__k`, the implicit tokens a combined grammar makes of the parser's
  literals no rule spells, numbered in the order the parser uses them and spelled exactly so, a rule typed `T__k`
  emitting that literal's token, a `mode` section holding no rule of its own, its error 145, where a mode named
  again reopens it and `mode DEFAULT_MODE` reopens the default mode, each section held on its own, a `mode` or
  `pushMode` naming no mode the grammar declares, DEFAULT_MODE and a number being modes, its error 176, a mode line
  before any rule, its syntax error, each in its words, a mode named INITIAL, since the audit reports the default mode
  under that name and could not tell the two apart, and what ANTLR's parser rejects as a syntax error, at that
  byte's line in words naming
  the cause: a command with parens holding nothing, `skip()`, the syntax error its parser reports at the `)`, a command
  with no comma before it, `skip type(B)`, the syntax error its parser reports at `type`, a comma no command name
  follows, `, skip`, `skip,, type(B)` or `skip,`, the syntax error it reports at the comma, an arrow no command follows,
  `-> ;`, parens holding more than one token or anything but a name or a number, `type(Y Z)` or `type(-1)`, parens never
  closed, `type(Y`, any other byte where a name, an argument or a comma should stand, the second `)` of `type(Y))`, and
  a `tokens` or `channels` block whose names no commas part or a comma ends, `{ ONE TWO }` and `{ ONE, }`, a channel
  named by another reserved name, `SKIP`, its error 172, by a number beyond its int or by a name nothing declares, its
  error 177, and a `channels` block in a combined grammar, its error 164, each in its words, one of the seven with its
  first letter capitalised, `Skip`, which names a code template of ANTLR's target that the generated lexer runs as an
  action and ANTLR's own interpreter leaves out, a parser literal two lexer rules spell, its error 126, a literal
  holding a surrogate on its own or a set holding nothing else, which ANTLR's lexer never matches, a range's end or a
  negated literal ANTLR's error 144 calls multi-character, a pair of escapes or a character beyond the basic
  multilingual plane written out among them, a range whose end is below its start or an empty set, its error 174, an
  escape ANTLR has not got, `'\q'`, or a braced Unicode escape whose closing brace stands twelve or more UTF-16 units
  into its literal, which its lexer counts from the quote, its error 156, a raw line break inside a literal, its error
  152, or a set, its syntax error at the break, each in its words, element options on a set, a range or a group, which
  take none, `EOF` inside a rule, semantic predicates `{...}?`, an action inside a rule whose body is anything but
  blanks and comments, since ANTLR runs it where it stands and its code may produce another token than the rule's own,
  Unicode property classes `\p{...}`, a character beyond ASCII named under `caseInsensitive`, whose Unicode case
  mappings the library has not got, the forms ANTLR itself rejects, a command on the alternatives of a rule with
  several, a `mode` line in a combined grammar and a closure, `*` or `+` in either form, whose body can match the empty
  string, which is its error 153 and runs through every rule the body reaches, a rule reaching itself, and a non-greedy
  loop before a rest of any other shape, one whose rest reaches past the sequence it stands in, inside a group or in a
  rule another rule inlines, one after elements of more than one length or of a length unknown, a reference among them,
  one in an alternative an earlier alternative can begin with the same character as, one in a rule an alternative of
  which can match the empty string, since the empty match reaches the rule's end at the loop's decision and stops it,
  one over a body of several lengths, a group among them, whose alternatives ANTLR takes in order, stopping at the
  fewest characters of them all, which `('x'|'xa')*? 'a'` and `('xa'|'x')*? 'a'` answer differently on "xaa" and no
  greedy loop over the group tells apart, and a non-greedy option over such a body, `('x'|'xa')?? 'a'` and `('xa'|'x')??
  'a'` answering the same way, or before a string its body could begin, which the bypass ANTLR tries first ends the rule
  with at once.
- logos: every enum deriving `Logos`, except one a `#[cfg(...)]` strips, its predicate false by its form alone, `any()`
  of nothing among them, as a variant under one is no rule and any other item under one, a function or a constant, binds
  no name, a `pub` before it notwithstanding, while an enum or a variant under a predicate the build alone decides,
  `feature = "x"`, is read as standing and the options row says so, `cfg=` and the predicate, and a
  `#[cfg_attr(predicate, ...)]` is the attributes it carries where the predicate is true by its form, nothing where it
  is false, and the attributes as well, the options row saying so, where the build decides; its `#[logos(skip ...)]`
  attributes as discarded rules ahead of the variants, `subpattern` definitions referenced as `(?&name)`, each read in
  the mode and under the flags of the pattern referencing it, since logos substitutes the definition's text before the
  crate parses it, so a byte string's subpattern referenced from a string pattern is over scalars, its `\xHH` the scalar
  U+00HH, and a string's referenced from a byte pattern is over bytes, `#[token]` and `#[regex]` attributes with their
  callbacks, `priority = n`, `ignore(case)` or `ignore(ascii_case)`. A callback's result is read against the variant's
  payload as logos's `CallbackResult` converts the pair: for a variant without a payload, `Skip`, `Ok(Skip)` and the
  `Skip` arms of `Filter` and `FilterResult` discard the match, `()`, `bool`, `Option<()>`, `Result<(), E>`, `Err` and
  the `Emit` and `Error` arms leave the variant's token or an error at the same boundary, and the enum returned is the
  token itself; for a variant with a payload, a value of the payload's type, bare or in `Some`, `Ok` or an `Emit` arm,
  is the payload and the variant's token, so a `Skip` returned to a variant carrying `Skip` emits it and the `Skip` arms
  alone still skip; the reading has the text, not the types, its blanks and comments dropped as Rust's lexer drops them
  and every name read as the file binds it in the module the text stands in, the enum's for a callback and a function's
  own for its type and body, and in the namespace the text asks, a type's or a value's, as Rust keeps them apart, a
  binding inside a `mod` block being none outside it unless a glob of that module brings it in, and a block, a
  function's body among them, a scope of its own, whose items a scanner inside it sees and one outside does not, the
  block seeing the module it stands in, through a `use` import or rename, a `type` alias, the path `#[logos(crate =
  ...)]` gives the crate and the items the file defines, the prelude's `Result` and `Option` the same types by their
  paths in `std` and `core`, so it reads `logos::skip`, a function the file defines by its return type, any type but
  those and `Self` in one of the enum's impl blocks being a payload, and, where that type is `Result<Skip, E>`,
  `Filter`, `FilterResult` or the enum, by its body, and a closure by every result its body produces, through blocks,
  returns, `?` operators, ifs and matches, a closure inside the body being a callable of its own whose returns and `?`
  are not the callback's, each of which must be visibly a skip, a constructor of the enum naming one of its variants, or
  `Some`, `None`, `Ok`, `Err`, a literal, `()` or an `Emit` or `Error` arm; a callback the file does not define, a
  result the text does not show, a type written through a generic alias of the file's, whose arguments are not
  substituted, results that skip on one path and emit on another, and a result the crate refuses for the variant's
  payload, a `Skip` to `V(u64)`, the enum or `()` to a variant with a payload or a literal to one without, are refused
  by name, since the rule's token is then decided at run time or out of sight. The callback also holds the lexer, whose
  `bump` extends the match and whose internal `bump_unchecked`, `trivia`, `error`, `end` and `set` move it too, so a
  body, a closure's, a named function's or a skip's, is read only where its lexer parameter is used through `slice`,
  `span`, `remainder`, `source`, `extras` and `clone`; one naming `bump`, `bump_unchecked` or `trivia` as a method, or
  using the parameter any other way, is refused by name, as is a function declared without a body or one binding the
  lexer with a pattern rather than a name, `lex`, `mut lex`, `ref lex`, `ref mut lex` or `_`. `ignore(case)` hands the
  pattern to the regex crate's case-insensitive parse, Unicode-aware in a string pattern and ASCII-only in a byte
  string, while `ignore(ascii_case)` parses the pattern as it stands and folds the ASCII letters of the compiled tree
  afterwards, a class gaining the other case of its ASCII members and a literal being taken apart one piece per byte,
  except in a byte string, where logos hands it the same parse as the other flag; logos refuses the two flags together.
  The priority is logos's own, computed as logos 0.14 and later compute it, over the pattern with its subpatterns pasted
  in as text, which is what the crate parses, merging adjacent literals across a reference, or taken from `priority =
  n`, higher winning, and mapped onto the builder's scale; a literal counts two per character, and two per byte where
  the run is not UTF-8 as strictly as Rust's own validation reads it, an encoded surrogate, an overlong form and a
  scalar above U+10FFFF being bytes rather than characters, since logos asks `std::str::from_utf8` and counts bytes
  where it fails, so that a scalar whose UTF-8 is split between a pattern and a subpattern is one scalar and a run the
  pasting joins into invalid UTF-8 is bytes. An ignore flag takes a `#[token]` out of the literals: logos escapes the
  literal for the regex crate and compiles that regex, so the priority comes from its tree like any other regex's, and
  the escaping writes a byte string's byte beyond ASCII out as the characters of its `\xNN` escape and escapes the
  backslash again, which is why `#[token(b"\xC3\xA9", ignore(case))]` matches those eight characters and never the two
  bytes. The regex is the regex crate's in Unicode mode, rewritten over the UTF-8 bytes the lexer scans: classes, the
  dot and negated classes as code point ranges, the flags `i`, `s` and `u` with their scoping, and `\d`, `\w` and `\s`
  in Unicode mode as the crate's classes, Nd, White_Space and the word class, over the tables of the Unicode version the
  regex-syntax logos is locked to was generated from rather than the library's own pinned one, since the audited
  language is the scanner's; the report's options name that version, `unicode-classes=16.0.0` for logos 0.15.1. Refused:
  `\p{...}`, whose property tables the library has not got, a non-ASCII scalar under `i`, anchors and lookaround, the
  flags `x`, `m`, `U` and `R`, the class operators, the lazy operators, which logos 0.15.1 refuses as unsupported
  non-greedy parsing, a `*` or `+` over the dot under `s` or over a class of every scalar or every byte, an alternation
  of classes the regex crate merges into one among them, which logos 0.15.1 refuses as consuming the source to its end,
  while the plain dot, a captured one and `[\x00-\x{D7FF}\x{E000}-\x{10FFFF}]`, two ranges to the crate where its dot is
  one, pass as they pass the crate, an `allow_greedy` argument, which logos 0.15.1 does not know and calls an unknown
  nested attribute, as it calls an `ignore` flag on a skip, any `#[logos(...)]` key beyond the eight it knows, an entry
  after a `skip(...)` or `error(...)` in one attribute, a bare key, a key with a value of another shape than it takes,
  `extras`, `error`, `source` or the type of one parameter given twice, `#[logos]`, `#[token]` or `#[regex]` without its
  parentheses or with nothing in them, a second `priority` or callback in one attribute, `priority(...)`, any argument
  after `ignore(...)`, the legacy `#[error]` attribute, a variant with several or named fields, a second lifetime, a
  const generic and a type parameter without its `type T = ...` or a `type` for none, each as the crate refuses it, a
  byte beyond ASCII written or admitted under `(?-u)` in a string pattern, a nested class checked on its own, which the
  regex crate refuses as able to match invalid UTF-8, a token or regex matching only the empty string once its
  subpatterns are pasted in, which logos 0.15.1 panics on as a token and compiles into a rule matching no input as a
  regex, while an empty `subpattern` definition is valid and adds nothing to the patterns referencing it, and an
  unbounded repetition whose body can begin with a byte that may also follow it, which logos 0.15.1 compiles into a
  scanner matching no input at all, its graph deciding a repetition's end on one byte: the flex spelling of the block
  comment is one of those, so the `.rs` grammars spell it with a loop that cannot begin with a star, the same language
  and the one the crate scans, while a bounded repetition, which the crate unrolls, is read however its boundary falls.

The grammars under `tools/audit/grammars/` are the study's rows in every syntax, and the tests hold each `.re`, `.g4`
and `.rs` file to its `.l` twin: the readers must build token sets that cut no input differently, decided by
`boundary_difference()` over every input rather than a sample.

## The JSON Form

With `--json` the run is one document: an array `files`, each with its `path`, `kind`, a `refused` message or null, and
its `scanners`, each with the `line` it opens on, its `options` as the array the row above prints, its `definitions` and
its `rules` as the reader read them (each rule's `line`, `pattern` as written, `conditions`, `action` and `token` or
null, the account to hold against the generator's own; flex's default rule stands last among them, with the line its
rules section ends on), and its `conditions`, each with `name`, `rules`, `refused` or null, and the `report`. A report
carries the same figures as the text under the row names above, `verdict`, `exact` and `modulo` as arrays of byte
values, `discarded`, `windows` with their origins, `window_count`, `mandatory_core`, `byte_span` and `window_span` as
numbers or the string `unbounded` (the window span also `undecided` when the windows were too many, and null when there
were none), `lag`, `rescue_free` as true, false or null when the search stopped at its cap, `rescue_witness` as the
shortest completely tokenizable input on which the scan rolls back or null, `blame` and `prices`, tokens given as their
id and name. Byte strings are JSON strings holding each byte as the code point of its value, so a reader recovers the
bytes exactly.
