# Grammars the auditor is tested on

Flex and re2c files written for this repository, each mirroring a row of the split-points paper's applicability
study so that the auditor's first check is that a token set read through a `.l` file answers exactly what the same
token set answered when it was typed into the library directly. Every grammar comes in both syntaxes, the `.re`
file the `.l` file's twin, and the second check is that the two readers build token sets that cut no input
differently. None is taken from another project; when one is, its origin and licence are recorded here beside it.

| Grammar | Mirrors |
|---|---|
| `c-like-conventional.l`, `.re` | the conventional C-like tokenization, whitespace runs including the newline; the `.re` adds case-insensitive keywords, re2c's idiom, which cut nothing differently |
| `c-like-split-friendly.l`, `.re` | the same language, newline its own token |
| `c-like-block-comments.l`, `.re` | the conventional tokenization plus block comments |
| `json.l`, `.re` | RFC 8259's lexical forms over bytes |
| `log-lines.l`, `.re` | a run of non-newline bytes, and the newline |
