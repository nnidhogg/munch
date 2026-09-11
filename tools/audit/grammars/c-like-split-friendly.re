// c-like-split-friendly.l written for re2c: the conventional rules with the newline its own token and the
// whitespace run holding spaces and tabs only. The slash in the operator class and the bracket in the punctuation
// class are escaped for the reasons c-like-conventional.re gives.
#include <stddef.h>

int lex(const char* YYCURSOR)
{
    const char* YYMARKER;

    /*!re2c
        re2c:define:YYCTYPE = char;
        re2c:yyfill:enable = 0;

        digit = [0-9];
        id    = [a-zA-Z_] [a-zA-Z0-9_]*;

        id                           { return IDENTIFIER; }
        digit+                       { return NUMBER; }
        [-+*\/<>=!&|^%~]             { return OPERATOR; }
        [\][(){};,.:?]               { return PUNCTUATION; }
        ["] [^"\n]* ["]              { return STRING; }
        "//" [^\n]*                  { continue; }
        "\n"                         { continue; }
        [ \t]+                       { continue; }
        *                            { return ERROR; }
        $                            { return END; }
    */
}
