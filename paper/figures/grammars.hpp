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

void c_like(munch::core::Builder& builder, const bool split_friendly)
{
    builder.add_token(concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Token::Identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token::Number, 2);
    builder.add_token(any_of(operators()), Token::Operator, 2);
    builder.add_token(any_of(punctuation()), Token::Punctuation, 2);

    if (split_friendly)
    {
        // Newline is its own token and the whitespace run cannot contain one, which is the whole difference.
        builder.add_token(text("\n"), Token::Newline, 2);
        builder.add_token(plus(any_of(Set{' ', '\t'})), Token::Whitespace, 2);
    }
    else
    {
        builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::Whitespace, 2);
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

Regex line_comment()
{
    return concat(text("//"), kleene(any_of(Set::all() - Set{'\n'})));
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
