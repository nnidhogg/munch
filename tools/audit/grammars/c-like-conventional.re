// The conventional C-like tokenization of c-like-conventional.l, written for re2c: the same seven rules over the
// same bytes, so the two readers can be held to one answer. Keywords are spelled case-insensitively here, which is
// re2c's own idiom, and add nothing to what certifies since an identifier admits every keyword byte anyway. The
// slash in the operator class is escaped because the block is a C comment, which a star and a slash would close,
// and the bracket in the punctuation class is escaped because re2c closes a class at the first bare one.
#include <stddef.h>

int lex(const char* YYCURSOR)
{
    const char* YYMARKER;

    /*!re2c
        re2c:define:YYCTYPE = char;
        re2c:yyfill:enable = 0;

        digit = [0-9];
        id    = [a-zA-Z_] [a-zA-Z0-9_]*;

        'if' | 'else' | 'while'      { return KEYWORD; }
        id                           { return IDENTIFIER; }
        digit+                       { return NUMBER; }
        [-+*\/<>=!&|^%~]             { return OPERATOR; }
        [\][(){};,.:?]               { return PUNCTUATION; }
        ["] [^"\n]* ["]              { return STRING; }
        "//" [^\n]*                  { continue; }
        [ \t\n]+                     { continue; }
        *                            { return ERROR; }
        $                            { return END; }
    */
}
