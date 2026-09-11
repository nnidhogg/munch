// c-like-block-comments.l written for ANTLR 4: the conventional rules plus the block comment in ANTLR's own idiom, a
// non-greedy loop up to the closing star-slash, which the reader turns into the loop over what holds no star-slash.
lexer grammar CLikeBlockComments;

IDENTIFIER   : [a-zA-Z_] [a-zA-Z0-9_]* ;
NUMBER       : [0-9]+ ;
OPERATOR     : [-+*/<>=!&|^%~] ;
PUNCTUATION  : [\][(){};,.:?] ;
STRING       : '"' ~["\n]* '"' ;
LINE_COMMENT : '//' ~[\n]* -> skip ;
BLOCK_COMMENT: '/*' .*? '*/' -> skip ;
WHITESPACE   : [ \t\n]+ -> skip ;
