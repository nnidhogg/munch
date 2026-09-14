#ifndef MUNCH_PAPER_FIGURES_GRAMMARS_HPP
#define MUNCH_PAPER_FIGURES_GRAMMARS_HPP

/*
 * The token sets the published tables are computed over, shared by the figure programs so that two programs cannot
 * drift into measuring two different grammars. That has happened: a scratchpad measurement of the block-comment row
 * used the conventional whitespace variant while the published row is cumulative on the split-friendly one.
 *
 * The using-directive is scoped to this namespace rather than the global one, and the figure programs want it too.
 */

#include <cstddef>
#include <initializer_list>
#include <set>

#include "munch/core/builder.hpp"
#include "munch/regex/patterns.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace figures
{
using namespace munch::regex;

enum class Token : std::size_t
{
    Identifier,
    Number,
    Whitespace,
    Operator,
    Punctuation,
    String,
    LineComment,
    BlockComment,
    Newline,
    LogLine,
    Keyword,
    Literal,
    Separator,
};

// The operator and punctuation bytes the C-like rows use, kept in one place so the expected column can name them.
const Set& operators()
{
    static const Set set{'+', '-', '*', '/', '<', '>', '=', '!', '&', '|', '^', '%', '~'};

    return set;
}

const Set& punctuation()
{
    static const Set set{'(', ')', '[', ']', '{', '}', ';', ',', '.', ':', '?'};

    return set;
}

// A string literal whose interior admits any byte except the quote and a raw newline.
Regex string_literal()
{
    return concat(text("\""), kleene(any_of(Set::all() - Set{'"'} - Set{'\n'})), text("\""));
}

// /* ( [^*] | *+ [^*/] )* *+ /, as docs/split_points.md states it.
Regex block_comment()
{
    const auto not_star{any_of(Set::all() - Set{'*'})};

    const auto stars_then_other{concat(plus(any_of(Set{'*'})), any_of(Set::all() - Set{'*'} - Set{'/'}))};

    return concat(text("/*"), kleene(choice(not_star, stars_then_other)), plus(any_of(Set{'*'})), text("/"));
}

// The blank set is the whitespace run's bytes other than newline: space and tab for the C-like rows, space alone for
// the Zig subset, whose reference's skip rule admits space and newline only.
void c_like(munch::core::Builder& builder, const bool split_friendly, const Set& blank = Set{' ', '\t'})
{
    builder.add_token(concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Token::Identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token::Number, 2);
    builder.add_token(any_of(operators()), Token::Operator, 2);
    builder.add_token(any_of(punctuation()), Token::Punctuation, 2);

    if (split_friendly)
    {
        // Newline is its own token and the whitespace run cannot contain one, which is the whole difference.
        builder.add_token(text("\n"), Token::Newline, 2);
        builder.add_token(plus(any_of(blank)), Token::Whitespace, 2);
    }
    else
    {
        builder.add_token(plus(any_of(blank + '\n')), Token::Whitespace, 2);
    }
}

// An exact duplicate of keyword_scale_builder() in tools/benchmark/src/main.cpp, which the evaluation uses for
// construction cost: the keyword list, the operator and punctuation literals, and the priorities are copied verbatim.
// That function returns a Staged_builder, which only exposes Builder's protected pipeline output for size reporting and
// so compiles the same automaton as the plain Builder used here. The grammar differs from the surveyed C-like row above
// in two ways that both cost certificates: operators are multi-byte literals, and numbers admit a decimal point.
void keyword_scale_grammar(munch::core::Builder& builder)
{
    // Roughly the C++ keyword set plus common fixed-width type names: 100 entries.
    static constexpr const char* keywords[]{
            "alignas",     "alignof",   "and",        "and_eq",    "asm",      "auto",         "bitand",
            "bitor",       "bool",      "break",      "case",      "catch",    "char",         "char8_t",
            "char16_t",    "char32_t",  "class",      "compl",     "concept",  "const",        "consteval",
            "constexpr",   "constinit", "const_cast", "continue",  "co_await", "co_return",    "co_yield",
            "decltype",    "default",   "delete",     "do",        "double",   "dynamic_cast", "else",
            "enum",        "explicit",  "export",     "extern",    "false",    "float",        "for",
            "friend",      "goto",      "if",         "inline",    "int",      "long",         "mutable",
            "namespace",   "new",       "noexcept",   "not",       "not_eq",   "nullptr",      "operator",
            "or",          "or_eq",     "private",    "protected", "public",   "register",     "reinterpret_cast",
            "requires",    "return",    "short",      "signed",    "sizeof",   "static",       "static_assert",
            "static_cast", "struct",    "switch",     "template",  "this",     "thread_local", "throw",
            "true",        "try",       "typedef",    "typeid",    "typename", "union",        "unsigned",
            "using",       "virtual",   "void",       "volatile",  "wchar_t",  "while",        "xor",
            "xor_eq",      "final",     "override",   "import",    "module",   "int8_t",       "int16_t",
            "int32_t",     "int64_t"};

    for (const auto* keyword : keywords)
    {
        builder.add_token(text(keyword), Token::Keyword, 1);
    }

    builder.add_token(concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Token::Identifier, 2);
    builder.add_token(patterns::decimal_float(), Token::Number, 1);
    builder.add_token(patterns::decimal_integer(), Token::Number, 1);
    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::Whitespace, 1);

    for (const auto* op : {"==", "!=", "<=", ">=", "<<", ">>", "&&", "||", "++", "--", "->", "+=", "-=", "*=",
                           "/=", "+",  "-",  "*",  "/",  "%",  "=",  "<",  ">",  "!",  "~",  "&",  "|",  "^"})
    {
        builder.add_token(text(op), Token::Operator, 2);
    }

    for (const auto* punct : {"(", ")", "{", "}", "[", "]", ";", ",", ".", ":", "?"})
    {
        builder.add_token(text(punct), Token::Punctuation, 2);
    }
}

// The grammar of build_lexer(false) in tools/benchmark/src/harness.cpp, which produces the scaling table. Its operators
// are also multi-byte literals, but every one of them has '=' as its only continuation byte, so '=' is the only
// candidate lost.
void scaling_grammar(munch::core::Builder& builder)
{
    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::Whitespace, 2);
    builder.add_token(concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Token::Identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token::Number, 2);
    builder.add_token(choice(text("if"), text("else"), text("while"), text("return"), text("int")), Token::Keyword, 1);

    builder.add_token(
            choice(text("=="), text("!="), text("<="), text(">="), text("+"), text("-"), text("*"), text("/"),
                   text("="), text("<"), text(">")),
            Token::Operator, 2);

    builder.add_token(any_of(Set{'(', ')', '{', '}', ';', ','}), Token::Punctuation, 2);
}

// The RFC 8259 lexical forms over byte input, not a JSON-like stand-in: strings carry the full escape and \uXXXX forms
// and exclude raw
// control bytes, numbers carry sign, fraction and exponent, the three literal names are present, and each structural
// character is its own token. This row is stated as a real JSON lexer because the surrounding text reconciles it with
// published results about splitting JSON at newline.
// This is a lexer over bytes, not a conforming JSON processor: UTF-8 well-formedness, which RFC 8259 requires of
// JSON exchanged outside a closed ecosystem, is
// assumed of the input rather than checked. Validating it could only remove bytes from string interiors, so it cannot
// de-certify anything that certifies without it, and the row's result is unaffected.
void json(munch::core::Builder& builder)
{
    const auto hex{any_of(Set::digits() + Set{'a', 'b', 'c', 'd', 'e', 'f', 'A', 'B', 'C', 'D', 'E', 'F'})};

    const auto escape{concat(
            text("\\"),
            choice(any_of(Set{'"', '\\', '/', 'b', 'f', 'n', 'r', 't'}), concat(text("u"), hex, hex, hex, hex)))};

    // Any byte from 0x20 up except quote and backslash, so UTF-8 continuation bytes pass through unexamined.
    auto unescaped{Set::all() - Set{'"'} - Set{'\\'}};

    for (int value{0}; value < 0x20; ++value)
    {
        unescaped = unescaped - Set{static_cast<char>(value)};
    }

    builder.add_token(concat(text("\""), kleene(choice(any_of(unescaped), escape)), text("\"")), Token::String, 2);

    const auto digits{plus(any_of(Set::digits()))};
    const auto integer{choice(text("0"), concat(any_of(Set::digits() - Set{'0'}), kleene(any_of(Set::digits()))))};
    const auto fraction{concat(text("."), digits)};
    const auto exponent{concat(any_of(Set{'e', 'E'}), optional(any_of(Set{'+', '-'})), digits)};

    builder.add_token(concat(optional(text("-")), integer, optional(fraction), optional(exponent)), Token::Number, 2);
    builder.add_token(choice(text("true"), text("false"), text("null")), Token::Literal, 1);
    builder.add_token(any_of(Set{'{', '}', '[', ']', ':', ','}), Token::Punctuation, 2);
    builder.add_token(plus(any_of(Set{' ', '\t', '\n', '\r'})), Token::Whitespace, 2);
}

// The Zig subset, the designed-success row: the Zig language reference states that there are no multiline comments and
// that "each line of code can be tokenized out of context", and this subset reads every string, comment and char
// literal one line at a time. The subset is adapted from the released 0.16.0 reference's grammar appendix, read as a
// token set over the C-like base above, with the adaptations stated here. Transcribed are the appendix's byte classes,
// non_control_ascii the bytes 0x20 to 0x7E, non_control_utf8 the bytes 0x20 to 0xFF, and multibyte_utf8 the well-formed
// two- to four-byte sequences its table lists, and from them the bodies: a string of string_char bodies, each a
// multibyte_utf8 sequence, a strict escape, \x with two hex digits, \u{ hex+ } or \ before one of n r \ t ' ", or a
// non_control_ascii byte that is neither backslash nor the quote, so that a lone backslash, a lone high byte and the
// delete byte are not string bytes; a quoted identifier, @ followed by such a string; a char literal of exactly one
// char_char, the same bodies over ' in place of "; a line string, \\ followed by non_control_utf8 bytes; and a line
// comment, // followed by non_control_utf8 bytes. Adapted are the tokens around those bodies: the whitespace run is
// reduced to the two bytes the skip rule admits, space and newline, and is read as tokens of its own, one run when
// split_friendly is false and newline apart from the space runs when it is true, as is the line comment, where the
// grammar folds skip, whitespace and line comments alike, into every token's tail and gives a line string and a doc
// comment, of either spelling, a trailing run of space and newline bytes of their own besides, so here the newline that
// ends a line string is outside its token, and a multiline string and a doc comment, each of which the grammar groups
// over consecutive lines into one token, are read one line at a time; and the plain, doc and container-doc comment
// spellings, which the grammar tells apart, are one token here, since as byte strings they are together every line
// beginning //. In its byte classes the subset follows the appendix rather than the language of conforming source: the
// source-encoding section forbids the delete byte and malformed UTF-8 everywhere, which the appendix's line bodies
// admit, and so do the subset's. Tab, which the reference's source encoding admits as a token separator, and carriage
// return, which it admits only immediately before a line feed, occur in no rule of the grammar appendix and so in no
// token of this subset; they are certified vacuously and withheld.
void zig(munch::core::Builder& builder, const bool split_friendly)
{
    c_like(builder, split_friendly, Set{' '});

    const auto bytes{[](const int low, const int high) {
        return any_of(Set::range(static_cast<char>(low), static_cast<char>(high)));
    }};

    const auto non_control_ascii{Set::range('\x20', '\x7E')};
    const auto non_control_utf8{Set::range('\x20', '\xFF')};

    const auto multibyte_utf8{
            choice(concat(bytes(0xF4, 0xF4), bytes(0x80, 0x8F), bytes(0x80, 0xBF), bytes(0x80, 0xBF)),
                   concat(bytes(0xF1, 0xF3), bytes(0x80, 0xBF), bytes(0x80, 0xBF), bytes(0x80, 0xBF)),
                   concat(bytes(0xF0, 0xF0), bytes(0x90, 0xBF), bytes(0x80, 0xBF), bytes(0x80, 0xBF)),
                   concat(bytes(0xEE, 0xEF), bytes(0x80, 0xBF), bytes(0x80, 0xBF)),
                   concat(bytes(0xED, 0xED), bytes(0x80, 0x9F), bytes(0x80, 0xBF)),
                   concat(bytes(0xE1, 0xEC), bytes(0x80, 0xBF), bytes(0x80, 0xBF)),
                   concat(bytes(0xE0, 0xE0), bytes(0xA0, 0xBF), bytes(0x80, 0xBF)),
                   concat(bytes(0xC2, 0xDF), bytes(0x80, 0xBF)))};

    const auto hex{any_of(Set::digits() + Set{'a', 'b', 'c', 'd', 'e', 'f', 'A', 'B', 'C', 'D', 'E', 'F'})};

    const auto escape{
            choice(concat(text("\\x"), hex, hex), concat(text("\\u{"), plus(hex), text("}")),
                   concat(text("\\"), any_of(Set{'n', 'r', '\\', 't', '\'', '"'})))};

    const auto body{[&](const char quote) {
        return choice(multibyte_utf8, escape, any_of(non_control_ascii - Set{quote} - Set{'\\'}));
    }};

    const auto string{concat(text("\""), kleene(body('"')), text("\""))};

    builder.add_token(string, Token::String, 2);
    builder.add_token(concat(text("@"), string), Token::Identifier, 2);
    builder.add_token(concat(text("\\\\"), kleene(any_of(non_control_utf8))), Token::String, 2);
    builder.add_token(concat(text("'"), body('\''), text("'")), Token::Literal, 2);

    builder.add_token(concat(text("//"), kleene(any_of(non_control_utf8))), Token::LineComment, 1);
}

Regex line_comment()
{
    return concat(text("//"), kleene(any_of(Set::all() - Set{'\n'})));
}

// The separated repertoire: strings and both comment forms, with every body barred from holding a byte that opens
// another of them, the obvious repair for the block-comment collapse. The row exists to price it, and the applicability
// table records that it buys no useful certificate: the language loses the ability to write a slash or a quote inside
// a string or a comment and gains nothing for it.
Regex separated_string()
{
    return concat(text("\""), kleene(any_of(Set::all() - Set{'"'} - Set{'/'} - Set{'\n'})), text("\""));
}

Regex separated_line_comment()
{
    return concat(text("//"), kleene(any_of(Set::all() - Set{'"'} - Set{'/'} - Set{'\n'})));
}

Regex separated_block_comment()
{
    const auto body{any_of(Set::all() - Set{'*'} - Set{'"'} - Set{'/'})};

    const auto stars_then_other{concat(plus(any_of(Set{'*'})), body)};

    return concat(text("/*"), kleene(choice(body, stars_then_other)), plus(any_of(Set{'*'})), text("/"));
}

Regex line_bounded_plain_block_comment()
{
    const auto body{any_of(Set::all() - Set{'*'} - Set{'\n'})};

    const auto stars_then_other{concat(plus(any_of(Set{'*'})), any_of(Set::all() - Set{'*'} - Set{'/'} - Set{'\n'}))};

    return concat(text("/*"), kleene(choice(body, stars_then_other)), plus(any_of(Set{'*'})), text("/"));
}

// The UTF-8 encoding forms of RFC 3629 section 4, one token per form and transcribed range for range: the
// certificate paper uses the characteristic the RFC lists, that character boundaries are found from anywhere in an
// octet stream, as the instance of its theorem every reader has met, and this is the token set that claim is
// asserted on, for the useful certificates; the bytes no form contains are certified vacuously and withheld.
Set octets(const int start, const int end)
{
    return Set::range(static_cast<char>(start), static_cast<char>(end));
}

void utf8(munch::core::Builder& builder)
{
    const auto tail{any_of(octets(0x80, 0xBF))};

    builder.add_token(any_of(octets(0x00, 0x7F)), Token::Literal, 1);
    builder.add_token(concat(any_of(octets(0xC2, 0xDF)), tail), Token::Literal, 1);
    builder.add_token(
            choice(concat(any_of(octets(0xE0, 0xE0)), any_of(octets(0xA0, 0xBF)), tail),
                   concat(any_of(octets(0xE1, 0xEC)), tail, tail),
                   concat(any_of(octets(0xED, 0xED)), any_of(octets(0x80, 0x9F)), tail),
                   concat(any_of(octets(0xEE, 0xEF)), tail, tail)),
            Token::Literal, 1);
    builder.add_token(
            choice(concat(any_of(octets(0xF0, 0xF0)), any_of(octets(0x90, 0xBF)), tail, tail),
                   concat(any_of(octets(0xF1, 0xF3)), tail, tail, tail),
                   concat(any_of(octets(0xF4, 0xF4)), any_of(octets(0x80, 0x8F)), tail, tail)),
            Token::Literal, 1);
}

// Names an ignored set in terms of the Token enum above; the certificate itself takes plain token ids.
std::set<std::size_t> ignoring(const std::initializer_list<Token> tokens)
{
    std::set<std::size_t> kinds;

    for (const auto token : tokens)
    {
        kinds.insert(static_cast<std::size_t>(token));
    }

    return kinds;
}

} // namespace figures

#endif // MUNCH_PAPER_FIGURES_GRAMMARS_HPP
