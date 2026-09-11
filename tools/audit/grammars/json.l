/* RFC 8259's lexical forms over bytes: the structural bytes, the three literals, numbers, and strings with the
 * escapes the RFC admits. Whitespace is the RFC's four bytes and is discarded. */

%option noyywrap

DIGIT     [0-9]
HEX       [0-9a-fA-F]
UNESCAPED [\x20-\x21\x23-\x5b\x5d-\xff]
ESCAPE    \\(["\\/bfnrt]|u{HEX}{HEX}{HEX}{HEX})

%%
"{"|"}"|"["|"]"|":"|","      return STRUCTURAL;
"true"|"false"|"null"        return LITERAL;
-?(0|[1-9]{DIGIT}*)(\.{DIGIT}+)?([eE][-+]?{DIGIT}+)?   return NUMBER;
\"({UNESCAPED}|{ESCAPE})*\"  return STRING;
[ \t\n\r]+                   ;
%%
