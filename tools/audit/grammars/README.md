# Grammars the auditor is tested on

Flex, re2c and ANTLR files written for this repository, each mirroring a row of the split-points paper's
applicability study so that the auditor's first check is that a token set read through a `.l` file answers exactly
what the same token set answered when it was typed into the library directly. Every grammar comes in every syntax,
the `.re` and `.g4` files the `.l` file's twins, and the second check is that the readers build token sets that cut
no input differently. The ANTLR twins read characters where the flex files read bytes, so their negated sets admit
the UTF-8 encodings alone and the bytes no encoding uses are left out of the byte comparison. None is taken from
another project; when one is, its origin and licence are recorded here beside it.

| Grammar | Mirrors |
|---|---|
| `c-like-conventional.l`, `.re`, `.g4` | the conventional C-like tokenization, whitespace runs including the newline; the `.re` adds case-insensitive keywords, re2c's idiom, which cut nothing differently |
| `c-like-split-friendly.l`, `.re`, `.g4` | the same language, newline its own token |
| `c-like-block-comments.l`, `.re`, `.g4` | the conventional tokenization plus block comments; the `.g4` writes the comment as ANTLR does, a non-greedy loop |
| `json.l`, `.re`, `.g4` | RFC 8259's lexical forms over bytes |
| `log-lines.l`, `.re`, `.g4` | a run of non-newline bytes, and the newline |
