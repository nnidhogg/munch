/* The same language as c-like-conventional.l with one change to the tokenization: the newline is its own token
 * and the whitespace run holds spaces and tabs only. Every byte tokenizes as before; only the cuts move. */

%option noyywrap

ID       [a-zA-Z_][a-zA-Z0-9_]*
DIGIT    [0-9]

%%
{ID}                     return IDENTIFIER;
{DIGIT}+                 return NUMBER;
[-+*/<>=!&|^%~]          return OPERATOR;
[][(){};,.:?]            return PUNCTUATION;
\"[^"\n]*\"              return STRING;
"//"[^\n]*               ;
\n                       ;
[ \t]+                   ;
%%
