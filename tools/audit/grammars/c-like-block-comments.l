/* The conventional tokenization with block comments added: the one token kind the split-points paper found
 * sufficient to remove every useful certificate, since a comment may hold any byte over any number of lines. */

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
"/*"([^*]|\*+[^*/])*\*+"/"   ;
[ \t\n]+                 ;
%%
