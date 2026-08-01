/*
 * Reproduces the applicability table of the report by compiling each token set and reading the shipped predicate
 * for all 256 byte values. No hand analysis is involved: the useful set is exactly what is_split_point() reports.
 *
 * Build and run from the repository root against an existing build tree:
 *   c++ -std=c++23 -I libs/common/include -I libs/core/include -I libs/dfa/include -I libs/nfa/include \
 *       -I libs/regex/include paper/figures/applicability.cpp -o /tmp/applicability \
 *       -L build/libs/core -L build/libs/dfa -L build/libs/nfa -L build/libs/regex \
 *       -lmunch_core -lmunch_dfa -lmunch_nfa -lmunch_regex -lpthread
 *   /tmp/applicability
 */

#include <cstddef>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/regex/patterns.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace
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

// The two benchmark rows publish their cells as "N of its own M" rather than as a byte list, and a miscounted M is
// exactly the kind of error the byte-list assertion cannot see. So those rows assert both numbers: M is the distinct
// operator and punctuation bytes the grammar registers, and N is how many of them certify.
std::size_t certified_among(const munch::core::Lexer& lexer, const std::string& candidates)
{
    std::size_t count{0};

    for (const auto candidate : candidates)
    {
        if (lexer.is_split_point(candidate))
        {
            ++count;
        }
    }

    return count;
}

// Renders the useful certified set the way the table's second column reads.
std::string certified(const munch::core::Lexer& lexer)
{
    std::vector<int> bytes;

    for (int value{0}; value < 256; ++value)
    {
        if (lexer.is_split_point(static_cast<char>(value)))
        {
            bytes.push_back(value);
        }
    }

    if (bytes.empty())
    {
        return "none";
    }

    Set combined{operators()};

    for (const auto symbol : punctuation().symbols())
    {
        combined = combined + symbol;
    }

    const auto& expected{combined.symbols()};

    if (bytes.size() == expected.size())
    {
        auto matches{true};

        for (const auto byte : bytes)
        {
            matches = matches && expected.contains(static_cast<char>(byte));
        }

        if (matches)
        {
            return "all operator and punctuation bytes";
        }
    }

    std::string rendered;

    for (const auto byte : bytes)
    {
        rendered += rendered.empty() ? "" : " ";
        rendered += byte == '\n' ? "\\n" : std::string(1, static_cast<char>(byte));
    }

    return rendered;
}

} // namespace

int main()
{
    int failures{0};

    // The candidate bytes of each benchmark grammar: every distinct byte occurring in an operator or punctuation
    // literal it registers. Written out rather than derived, so a wrong count in the paper cannot match a wrong count
    // here by construction.
    const std::string keyword_scale_candidates{"=!<>&|+-*/%~^(){}[];,.:?"};
    const std::string scaling_candidates{"=!<>+-*/(){};,"};

    const auto check{[&failures](const std::string& name, const std::string& expected, munch::core::Builder& b) {
        const auto actual{certified(b.build())};

        std::cout << (actual == expected ? "  ok   " : "  FAIL ") << name << '\n';

        if (actual != expected)
        {
            std::cout << "         expected: " << expected << "\n         actual:   " << actual << '\n';

            ++failures;
        }
    }};

    const auto ratio{[&failures](
                             const std::string& name, const std::string& candidates, const std::size_t total,
                             const std::size_t expected, munch::core::Builder& b) {
        const std::set<char> distinct{candidates.begin(), candidates.end()};
        const auto actual{certified_among(b.build(), candidates)};
        const auto agrees{distinct.size() == total && actual == expected};

        std::cout << (agrees ? "  ok   " : "  FAIL ") << name << '\n';

        if (!agrees)
        {
            std::cout << "         candidates: " << distinct.size() << " distinct, cell says " << total
                      << "\n         certified:  " << actual << ", cell says " << expected << '\n';

            ++failures;
        }
    }};

    {
        munch::core::Builder b;
        c_like(b, false);
        check("C-like: identifiers, numbers, ws runs, operators, punct", "all operator and punctuation bytes", b);
    }
    // Each of the next three adds exactly one token kind to that same base, so each collapse is attributable to the
    // token kind named rather than to an accumulation of them.
    {
        munch::core::Builder b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        check("the first row plus strings, alone (no raw newline inside)", "none", b);
    }
    {
        munch::core::Builder b;
        c_like(b, false);
        b.add_token(line_comment(), Token::LineComment, 1);
        check("the first row plus // line comments, alone", "none", b);
    }
    {
        munch::core::Builder b;
        c_like(b, false);
        b.add_token(block_comment(), Token::BlockComment, 1);
        check("the first row plus block comments, alone", "none", b);
    }
    {
        munch::core::Builder b;
        json(b);
        check("JSON, the RFC 8259 lexical forms over bytes", "none", b);
    }
    {
        munch::core::Builder b;
        b.add_token(plus(any_of(Set::all() - Set{'\n'})), Token::LogLine, 2);
        b.add_token(text("\n"), Token::Newline, 2);
        check("log lines ([^\\n]+ and \\n)", "\\n", b);
    }
    // The next two recognize exactly the same byte language and differ only in how it is cut into tokens: the
    // conventional one folds newline into the whitespace run, the split-friendly one gives newline its own token and
    // leaves spaces and tabs as a run. That is the pair the text needs to claim certification depends on the
    // tokenization rather than on the recognized language.
    {
        munch::core::Builder b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        check("C-like, conventional tokenization (whitespace runs include newline)", "none", b);
    }
    {
        munch::core::Builder b;
        c_like(b, true);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        check("the same language, split-friendly tokenization", "\\n", b);
    }
    {
        munch::core::Builder b;
        // Deliberately cumulative: this is the same grammar as the row above with block comments added, so the
        // pair is a true before/after on the one token kind that spans lines.
        c_like(b, true);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        b.add_token(block_comment(), Token::BlockComment, 1);
        check("the same plus block comments", "none", b);
    }
    {
        munch::core::Builder b;
        keyword_scale_grammar(b);
        check("keyword_scale_builder() grammar (construction cost)", "! % ( ) * , / : ; ? [ ] ^ { } ~", b);
        ratio("keyword_scale_builder() published as 16 of its own 24", keyword_scale_candidates, 24, 16, b);
    }
    {
        munch::core::Builder b;
        scaling_grammar(b);
        check("build_lexer(false) grammar (scaling table)", "! ( ) * + , - / ; < > { }", b);
        ratio("build_lexer(false) published as 13 of its own 14", scaling_candidates, 14, 13, b);
    }

    std::cout << (failures == 0 ? "\nall rows reproduce the table\n" : "\nrows disagreeing with the table\n");

    return failures == 0 ? 0 : 1;
}
