// log-lines.l written for re2c: every line one record, the newline between them.
#include <stddef.h>

int lex(const char* YYCURSOR)
{
    const char* YYMARKER;

    /*!re2c
        re2c:define:YYCTYPE = char;
        re2c:yyfill:enable = 0;

        [^\n]+       { return LINE; }
        "\n"         { return NEWLINE; }
        *            { return ERROR; }
        $            { return END; }
    */
}
