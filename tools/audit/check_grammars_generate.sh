#!/usr/bin/env bash
#
# Fails unless every bundled flex, re2c and ANTLR grammar is accepted by its own generator.
#
# The audit reads these files as the generators would, so a file the generator itself refuses would be audited as a
# scanner that does not exist. That happened: the five re2c grammars carried an end rule without re2c:eof, which
# re2c 3.1 refuses, and nothing ran re2c over them. The logos grammars are Rust and need the crate built, which this
# check does not do.
#
# Usage: tools/audit/check_grammars_generate.sh ANTLR_JAR

set -euo pipefail

jar="${1:?usage: check_grammars_generate.sh ANTLR_JAR}"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

for grammar in tools/audit/grammars/*.l; do
    flex -o "$out/lex.c" "$grammar"
done

for grammar in tools/audit/grammars/*.re; do
    re2c -o "$out/re2c.c" "$grammar"
done

# ANTLR requires a grammar's file to carry the grammar's own name, which the bundled files, named for their token
# sets, do not; each is generated from a copy under its declared name.
for grammar in tools/audit/grammars/*.g4; do
    name="$(sed -n 's/^\(lexer \)\{0,1\}grammar[[:space:]]\{1,\}\([A-Za-z_][A-Za-z0-9_]*\).*/\2/p' "$grammar" | head -1)"
    cp "$grammar" "$out/$name.g4"
    java -jar "$jar" -o "$out/antlr" "$out/$name.g4"
done

echo "every bundled flex, re2c and ANTLR grammar is accepted by its generator"
