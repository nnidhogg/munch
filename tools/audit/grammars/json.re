// json.l written for re2c: RFC 8259's lexical forms over bytes, the definitions named as the flex file names them.
// The escape's slash is escaped because the block is a C comment.
#include <stddef.h>

int lex(const char* YYCURSOR)
{
    const char* YYMARKER;

    /*!re2c
        re2c:define:YYCTYPE = char;
        re2c:yyfill:enable = 0;

        digit     = [0-9];
        hex       = [0-9a-fA-F];
        unescaped = [\x20-\x21\x23-\x5b\x5d-\xff];
        escape    = "\\" (["\\\/bfnrt] | "u" hex hex hex hex);

        "{" | "}" | "[" | "]" | ":" | ","                          { return STRUCTURAL; }
        "true" | "false" | "null"                                  { return LITERAL; }
        "-"? ("0" | [1-9] digit*) ("." digit+)? ([eE] [-+]? digit+)?  { return NUMBER; }
        ["] (unescaped | escape)* ["]                              { return STRING; }
        [ \t\n\r]+                                                 { continue; }
        *                                                          { return ERROR; }
        $                                                          { return END; }
    */
}
