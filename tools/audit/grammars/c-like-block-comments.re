// c-like-block-comments.l written for re2c: the conventional rules plus the block comment. The comment's own
// closer is spelled as two literals, since the block is a C comment and a star followed by a slash would end it;
// the slash in the operator class and the bracket in the punctuation class are escaped as c-like-conventional.re
// explains.
#include <stddef.h>

int lex(const char* YYCURSOR)
{
    const char* YYMARKER;

    /*!re2c
        re2c:define:YYCTYPE = char;
        re2c:yyfill:enable = 0;

        digit = [0-9];
        id    = [a-zA-Z_] [a-zA-Z0-9_]*;

        id                                   { return IDENTIFIER; }
        digit+                               { return NUMBER; }
        [-+*\/<>=!&|^%~]                     { return OPERATOR; }
        [\][(){};,.:?]                       { return PUNCTUATION; }
        ["] [^"\n]* ["]                      { return STRING; }
        "//" [^\n]*                          { continue; }
        "/*" ([^*] | "*"+ [^*\/])* "*"+ "/"  { continue; }
        [ \t\n]+                             { continue; }
        *                                    { return ERROR; }
        $                                    { return END; }
    */
}
