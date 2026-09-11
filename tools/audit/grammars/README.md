# Grammars the auditor is tested on

Flex, re2c, ANTLR and logos files written for this repository, each mirroring a row of the split-points paper's
applicability study so that the auditor's first check is that a token set read through a `.l` file answers exactly
what the same token set answered when it was typed into the library directly. Every grammar comes in every syntax,
the `.re`, `.g4` and `.rs` files the `.l` file's twins, and the second check is that the readers build token sets
that cut no input differently. The ANTLR and logos twins read characters where the flex files read bytes, so their
negated sets admit the UTF-8 encodings alone: the bytes no encoding uses are left out of the byte comparison, and
the twins part from the flex file only on input they refuse. None is taken from another project; when one is, its
origin and licence are recorded here beside it.

| Grammar | Mirrors |
|---|---|
| `c-like-conventional.l`, `.re`, `.g4`, `.rs` | the conventional C-like tokenization, whitespace runs including the newline; the `.re` adds case-insensitive keywords, re2c's idiom, which cut nothing differently; the `.rs` allows the line comment's dot-shaped repetition, as logos requires |
| `c-like-split-friendly.l`, `.re`, `.g4`, `.rs` | the same language, newline its own token |
| `c-like-block-comments.l`, `.re`, `.g4`, `.rs` | the conventional tokenization plus block comments; the `.g4` writes the comment as ANTLR does, a non-greedy loop |
| `json.l`, `.re`, `.g4`, `.rs` | RFC 8259's lexical forms over bytes; the `.g4` and `.rs` run the unescaped string character to U+10FFFF as the RFC has it, the `.rs` through subpatterns named as the flex definitions are |
| `log-lines.l`, `.re`, `.g4`, `.rs` | a run of non-newline bytes, and the newline |
