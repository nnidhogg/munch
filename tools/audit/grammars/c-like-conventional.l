/* The conventional C-like tokenization of the split-points paper, in flex's idiom: identifiers, numbers, one
 * operator byte, one punctuation byte, line-bounded strings without escapes, line comments, and a whitespace run
 * that folds the newline in. Rules returning nothing are the tokens a parser never sees. */

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
[ \t\n]+                 ;
%%
