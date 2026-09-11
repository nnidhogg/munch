// The conventional C-like tokenization of c-like-conventional.l, written for ANTLR 4: the same seven rules over the
// same characters, so the readers can be held to one answer. ANTLR reads characters and the audit reads bytes, which
// coincide here since every set is ASCII; a negated set such as ~[\n] admits every other scalar, read as its UTF-8
// bytes.
lexer grammar CLikeConventional;

IDENTIFIER  : [a-zA-Z_] [a-zA-Z0-9_]* ;
NUMBER      : [0-9]+ ;
OPERATOR    : [-+*/<>=!&|^%~] ;
PUNCTUATION : [\][(){};,.:?] ;
STRING      : '"' ~["\n]* '"' ;
LINE_COMMENT: '//' ~[\n]* -> skip ;
WHITESPACE  : [ \t\n]+ -> skip ;
