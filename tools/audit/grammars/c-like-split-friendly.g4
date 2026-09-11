// c-like-split-friendly.l written for ANTLR 4: the conventional rules with the newline its own token and the
// whitespace run holding spaces and tabs only.
lexer grammar CLikeSplitFriendly;

IDENTIFIER  : [a-zA-Z_] [a-zA-Z0-9_]* ;
NUMBER      : [0-9]+ ;
OPERATOR    : [-+*/<>=!&|^%~] ;
PUNCTUATION : [\][(){};,.:?] ;
STRING      : '"' ~["\n]* '"' ;
LINE_COMMENT: '//' ~[\n]* -> skip ;
NEWLINE     : '\n' -> skip ;
WHITESPACE  : [ \t]+ -> skip ;
