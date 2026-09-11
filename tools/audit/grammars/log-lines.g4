// log-lines.l written for ANTLR 4: every line one record, the newline between them.
lexer grammar LogLines;

LINE    : ~[\n]+ ;
NEWLINE : '\n' ;
