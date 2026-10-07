#ifndef MUNCH_PAPER_FIGURES_GRAMMARS_HPP
#define MUNCH_PAPER_FIGURES_GRAMMARS_HPP

/*
 * The token sets the published tables are computed over, shared by the figure programs so that two programs cannot
 * drift into measuring two different grammars, such as the block-comment row measured over the conventional whitespace
 * variant where the published row is cumulative on the split-friendly one. The scan, the cut check, the joining and the
 * verdict printing the figure programs share live here too, so two programs cannot check a figure two ways.
 *
 * The using-directive is scoped to this namespace rather than the global one, and the figure programs want it too.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/patterns.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace figures
{
using namespace munch::regex;

/**
 * @brief The number of byte values, every one of which a certificate is read at.
 */
inline constexpr int byte_values{256};

/**
 * @brief The token kinds of the figures' rows.
 */
enum class Token : std::size_t
{
    /**
     * @brief An identifier.
     */
    identifier,

    /**
     * @brief A number.
     */
    number,

    /**
     * @brief A whitespace run.
     */
    whitespace,

    /**
     * @brief An operator.
     */
    operator_,

    /**
     * @brief A punctuation byte.
     */
    punctuation,

    /**
     * @brief A string literal.
     */
    string,

    /**
     * @brief A line comment.
     */
    line_comment,

    /**
     * @brief A block comment.
     */
    block_comment,

    /**
     * @brief A newline of its own.
     */
    newline,

    /**
     * @brief A log line's text.
     */
    log_line,

    /**
     * @brief A keyword.
     */
    keyword,

    /**
     * @brief A literal name, character literal or encoding form.
     */
    literal,

    /**
     * @brief A separator byte.
     */
    separator
};

/**
 * @brief Token kinds, as the certificate's ignored set names them.
 */
using Kinds_t = std::set<std::size_t>;

/**
 * @brief A scan's tokens, each a kind and a length, in stream order.
 */
using Stream_t = std::vector<std::pair<std::size_t, std::size_t>>;

/**
 * @brief A Builder whose compiled automaton, protected in Builder, is public, the way the unit tests expose it.
 */
class Builder_dbg : public munch::core::Builder
{
public:
    /**
     * @brief The Builder's compiled automaton, made public.
     */
    using Builder::dfa;
};

/**
 * @brief Returns the operator bytes the C-like rows use, kept in one place so the expected column can name them.
 * @return The set.
 */
inline const Set& operators()
{
    static const Set set{'+', '-', '*', '/', '<', '>', '=', '!', '&', '|', '^', '%', '~'};

    return set;
}

/**
 * @brief Returns the punctuation bytes the C-like rows use.
 * @return The set.
 */
inline const Set& punctuation()
{
    static const Set set{'(', ')', '[', ']', '{', '}', ';', ',', '.', ':', '?'};

    return set;
}

/**
 * @brief Returns the hexadecimal digits, both cases, as the JSON and Zig escapes write them.
 * @return The set.
 */
inline const Set& hex_digits()
{
    static const Set set{Set::digits() + Set{'a', 'b', 'c', 'd', 'e', 'f', 'A', 'B', 'C', 'D', 'E', 'F'}};

    return set;
}

/**
 * @brief Builds an identifier: a letter or an underscore, then letters, digits and underscores.
 * @return The regex.
 */
inline Regex identifier()
{
    return concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_')));
}

/**
 * @brief Builds a string literal whose interior admits any byte except the quote and a raw newline.
 * @return The regex.
 */
inline Regex string_literal()
{
    return concat(text(R"(")"), kleene(any_of(Set::all() - Set{'"'} - Set{'\n'})), text(R"(")"));
}

/**
 * @brief Builds a line comment: two slashes and every byte up to the newline.
 * @return The regex.
 */
inline Regex line_comment()
{
    return concat(text("//"), kleene(any_of(Set::all() - Set{'\n'})));
}

/**
 * @brief Builds a block comment whose body also bars some bytes: the opener, then any bytes outside the barred ones in
 *        which no star run is followed by a slash, then a star run and the slash.
 * @param barred The bytes the body may not hold besides the star run's rule.
 * @return The regex.
 */
inline Regex block_comment_barring(const Set& barred)
{
    const auto body{any_of(Set::all() - Set{'*'} - barred)};

    const auto stars_then_other{concat(plus(any_of(Set{'*'})), any_of(Set::all() - Set{'*'} - Set{'/'} - barred))};

    return concat(text("/*"), kleene(choice(body, stars_then_other)), plus(any_of(Set{'*'})), text("/"));
}

/**
 * @brief Builds a block comment as docs/split_points.md states it: the opener, then any bytes in which no star run is
 *        followed by a slash, then a star run and the slash.
 * @return The regex.
 */
inline Regex block_comment()
{
    return block_comment_barring(Set{});
}

/**
 * @brief Adds the C-like base: identifiers, numbers, single-byte operators and punctuation, and a whitespace run.
 * @param builder The builder the tokens are added to.
 * @param split_friendly Whether newline is its own token, the whitespace run then holding no newline.
 * @param blank The whitespace run's bytes other than newline: space and tab for the C-like rows, space alone for the
 *        Zig subset, whose reference's skip rule admits space and newline only.
 */
inline void c_like(munch::core::Builder& builder, const bool split_friendly, const Set& blank = Set{' ', '\t'})
{
    builder.add_token(identifier(), Token::identifier, 2);

    builder.add_token(plus(any_of(Set::digits())), Token::number, 2);

    builder.add_token(any_of(operators()), Token::operator_, 2);

    builder.add_token(any_of(punctuation()), Token::punctuation, 2);

    if (split_friendly)
    {
        // Newline is its own token and the whitespace run cannot contain one, which is the whole difference.
        builder.add_token(text("\n"), Token::newline, 2);

        builder.add_token(plus(any_of(blank)), Token::whitespace, 2);
    }
    else
    {
        builder.add_token(plus(any_of(blank + '\n')), Token::whitespace, 2);
    }
}

/**
 * @brief Adds the C-like base with strings and line comments.
 * @param builder The builder the tokens are added to.
 * @param split_friendly Whether newline is its own token, the whitespace run then holding no newline.
 */
inline void c_like_with_comments(munch::core::Builder& builder, const bool split_friendly)
{
    c_like(builder, split_friendly);

    builder.add_token(string_literal(), Token::string, 2);

    builder.add_token(line_comment(), Token::line_comment, 1);
}

/**
 * @brief Adds the C-like base with strings, line comments and block comments.
 * @param builder The builder the tokens are added to.
 * @param split_friendly Whether newline is its own token, the whitespace run then holding no newline.
 */
inline void c_like_with_block_comments(munch::core::Builder& builder, const bool split_friendly)
{
    c_like_with_comments(builder, split_friendly);

    builder.add_token(block_comment(), Token::block_comment, 1);
}

/**
 * @brief Adds an exact duplicate of keyword_scale_tokens() in tools/benchmark/src/harness.cpp, the grammar the
 *        evaluation compiles for construction cost.
 *
 * The keyword list, the operator and punctuation literals, and the priorities are copied verbatim, and the
 * applicability program binds the copy to the original by comparing the two compiled scanners. The benchmark's
 * keyword_scale_builder() adds those tokens to a Builder_dbg, which only exposes Builder's protected pipeline output
 * for size reporting and so compiles the same automaton as the plain Builder used here. The grammar differs from the
 * surveyed C-like row above in two ways that both cost certificates: operators are multi-byte literals, and numbers
 * admit a decimal point.
 * @param builder The builder the tokens are added to.
 */
inline void keyword_scale_grammar(munch::core::Builder& builder)
{
    // Roughly the C++ keyword set plus common fixed-width type names: 100 entries.
    static constexpr std::array<std::string_view, 100> keywords{
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

    for (const auto keyword : keywords)
    {
        builder.add_token(text(keyword), Token::keyword, 1);
    }

    builder.add_token(identifier(), Token::identifier, 2);

    builder.add_token(patterns::decimal_float(), Token::number, 1);

    builder.add_token(patterns::decimal_integer(), Token::number, 1);

    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 1);

    for (const std::string_view op : {"==", "!=", "<=", ">=", "<<", ">>", "&&", "||", "++", "--",
                                      "->", "+=", "-=", "*=", "/=", "+",  "-",  "*",  "/",  "%",
                                      "=",  "<",  ">",  "!",  "~",  "&",  "|",  "^"})
    {
        builder.add_token(text(op), Token::operator_, 2);
    }

    for (const std::string_view punct : {"(", ")", "{", "}", "[", "]", ";", ",", ".", ":", "?"})
    {
        builder.add_token(text(punct), Token::punctuation, 2);
    }
}

/**
 * @brief Adds the grammar of build_lexer(false) in tools/benchmark/src/harness.cpp, which produces the scaling table.
 *
 * Its operators are also multi-byte literals, but every one of them has '=' as its only continuation byte, so '=' is
 * the only candidate lost.
 * @param builder The builder the tokens are added to.
 */
inline void scaling_grammar(munch::core::Builder& builder)
{
    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 2);

    builder.add_token(identifier(), Token::identifier, 2);

    builder.add_token(plus(any_of(Set::digits())), Token::number, 2);

    builder.add_token(choice(text("if"), text("else"), text("while"), text("return"), text("int")), Token::keyword, 1);

    builder.add_token(
            choice(text("=="), text("!="), text("<="), text(">="), text("+"), text("-"), text("*"), text("/"),
                   text("="), text("<"), text(">")),
            Token::operator_, 2);

    builder.add_token(any_of(Set{'(', ')', '{', '}', ';', ','}), Token::punctuation, 2);
}

/**
 * @brief Adds the RFC 8259 lexical forms over byte input, not a JSON-like stand-in: strings carry the full escape and
 *        \uXXXX forms and exclude raw control bytes, numbers carry sign, fraction and exponent, the three literal names
 *        are present, and each structural character is its own token.
 *
 * This row is stated as a real JSON lexer because the surrounding text reconciles it with published results about
 * splitting JSON at newline.
 *
 * This is a lexer over bytes, not a conforming JSON processor: UTF-8 well-formedness, which RFC 8259 requires of JSON
 * exchanged outside a closed ecosystem, is assumed of the input rather than checked. Validating it could only remove
 * bytes from string interiors, so it cannot de-certify anything that certifies without it, and the row's result is
 * unaffected.
 * @param builder The builder the tokens are added to.
 */
inline void json(munch::core::Builder& builder)
{
    const auto hex{any_of(hex_digits())};

    const auto escape{concat(
            text(R"(\)"),
            choice(any_of(Set{'"', '\\', '/', 'b', 'f', 'n', 'r', 't'}), concat(text("u"), hex, hex, hex, hex)))};

    // Any byte from 0x20 up except quote and backslash, so UTF-8 continuation bytes pass through unexamined.
    const auto unescaped{Set::all() - Set{'"'} - Set{'\\'} - Set::range('\x00', '\x1F')};

    builder.add_token(concat(text(R"(")"), kleene(choice(any_of(unescaped), escape)), text(R"(")")), Token::string, 2);

    const auto digits{plus(any_of(Set::digits()))};

    const auto integer{choice(text("0"), concat(any_of(Set::digits() - Set{'0'}), kleene(any_of(Set::digits()))))};

    const auto fraction{concat(text("."), digits)};

    const auto exponent{concat(any_of(Set{'e', 'E'}), optional(any_of(Set{'+', '-'})), digits)};

    builder.add_token(concat(optional(text("-")), integer, optional(fraction), optional(exponent)), Token::number, 2);

    builder.add_token(choice(text("true"), text("false"), text("null")), Token::literal, 1);

    builder.add_token(any_of(Set{'{', '}', '[', ']', ':', ','}), Token::punctuation, 2);

    builder.add_token(plus(any_of(Set{' ', '\t', '\n', '\r'})), Token::whitespace, 2);
}

/**
 * @brief Returns the bytes of a range, as RFC 3629 writes its octet ranges.
 * @param start The first byte.
 * @param end The last byte.
 * @return The set.
 */
inline Set octets(const int start, const int end)
{
    return Set::range(static_cast<char>(start), static_cast<char>(end));
}

/**
 * @brief Builds the well-formed multibyte UTF-8 sequences of RFC 3629 section 4, transcribed range for range, one
 *        expression per row of its table in the table's order: the two-byte form, the four three-byte forms and the
 *        three four-byte forms.
 * @return The forms.
 */
inline std::array<Regex, 8> multibyte_utf8_forms()
{
    const auto tail{any_of(octets(0x80, 0xBF))};

    return {concat(any_of(octets(0xC2, 0xDF)), tail),
            concat(any_of(octets(0xE0, 0xE0)), any_of(octets(0xA0, 0xBF)), tail),
            concat(any_of(octets(0xE1, 0xEC)), tail, tail),
            concat(any_of(octets(0xED, 0xED)), any_of(octets(0x80, 0x9F)), tail),
            concat(any_of(octets(0xEE, 0xEF)), tail, tail),
            concat(any_of(octets(0xF0, 0xF0)), any_of(octets(0x90, 0xBF)), tail, tail),
            concat(any_of(octets(0xF1, 0xF3)), tail, tail, tail),
            concat(any_of(octets(0xF4, 0xF4)), any_of(octets(0x80, 0x8F)), tail, tail)};
}

/**
 * @brief Adds the Zig subset, the designed-success row: the Zig language reference states that there are no multiline
 *        comments and that "each line of code can be tokenized out of context", and this subset reads every string,
 *        comment and char literal one line at a time.
 *
 * The subset is adapted from the released 0.16.0 reference's grammar appendix, read as a token set over the C-like base
 * above, with the adaptations stated here. Transcribed are the appendix's byte classes, non_control_ascii the bytes
 * 0x20 to 0x7E, non_control_utf8 the bytes 0x20 to 0xFF, and multibyte_utf8 the well-formed two- to four-byte sequences
 * its table lists, and from them the bodies: a string of string_char bodies, each a multibyte_utf8 sequence, a strict
 * escape, \x with two hex digits, \u{ hex+ } or \ before one of n r \ t ' ", or a non_control_ascii byte that is
 * neither backslash nor the quote, so that a lone backslash, a lone high byte and the delete byte are not string bytes;
 * a quoted identifier, @ followed by such a string; a char literal of exactly one char_char, the same bodies over ' in
 * place of "; a line string, \\ followed by non_control_utf8 bytes; and a line comment, // followed by non_control_utf8
 * bytes. Adapted are the tokens around those bodies: the whitespace run is reduced to the two bytes the skip rule
 * admits, space and newline, and is read as tokens of its own, one run when split_friendly is false and newline apart
 * from the space runs when it is true, as is the line comment, where the grammar folds skip, whitespace and line
 * comments alike, into every token's tail and gives a line string and a doc comment, of either spelling, a trailing run
 * of space and newline bytes of their own besides, so here the newline that ends a line string is outside its token,
 * and a multiline string and a doc comment, each of which the grammar groups over consecutive lines into one token, are
 * read one line at a time; and the plain, doc and container-doc comment spellings, which the grammar tells apart, are
 * one token here, since as byte strings they are together every line beginning //. In its byte classes the subset
 * follows the appendix rather than the language of conforming source: the source-encoding section forbids the delete
 * byte and malformed UTF-8 everywhere, which the appendix's line bodies admit, and so do the subset's. Tab, which the
 * reference's source encoding admits as a token separator, and carriage return, which it admits only immediately before
 * a line feed, occur in no rule of the grammar appendix and so in no token of this subset; they are certified vacuously
 * and withheld.
 * @param builder The builder the tokens are added to.
 * @param split_friendly Whether newline is its own token, apart from the space runs.
 */
inline void zig(munch::core::Builder& builder, const bool split_friendly)
{
    c_like(builder, split_friendly, Set{' '});

    const auto non_control_ascii{Set::range('\x20', '\x7E')};

    const auto non_control_utf8{Set::range('\x20', '\xFF')};

    const auto [two, e0, e1_ec, ed, ee_ef, f0, f1_f3, f4]{multibyte_utf8_forms()};

    const auto multibyte_utf8{choice(f4, f1_f3, f0, ee_ef, ed, e1_ec, e0, two)};

    const auto hex{any_of(hex_digits())};

    const auto escape{
            choice(concat(text(R"(\x)"), hex, hex), concat(text(R"(\u{)"), plus(hex), text("}")),
                   concat(text(R"(\)"), any_of(Set{'n', 'r', '\\', 't', '\'', '"'})))};

    const auto body{[&](const char quote) {
        return choice(multibyte_utf8, escape, any_of(non_control_ascii - Set{quote} - Set{'\\'}));
    }};

    const auto string{concat(text(R"(")"), kleene(body('"')), text(R"(")"))};

    builder.add_token(string, Token::string, 2);

    builder.add_token(concat(text("@"), string), Token::identifier, 2);

    builder.add_token(concat(text(R"(\\)"), kleene(any_of(non_control_utf8))), Token::string, 2);

    builder.add_token(concat(text("'"), body('\''), text("'")), Token::literal, 2);

    builder.add_token(concat(text("//"), kleene(any_of(non_control_utf8))), Token::line_comment, 1);
}

/**
 * @brief Builds the separated repertoire's string: strings and both comment forms, with every body barred from holding
 *        a byte that opens another of them, the obvious repair for the block-comment collapse.
 *
 * The row exists to price it, and the applicability table records that it buys no useful certificate: the language
 * loses the ability to write a slash or a quote inside a string or a comment and gains nothing for it.
 * @return The regex.
 */
inline Regex separated_string()
{
    return concat(text(R"(")"), kleene(any_of(Set::all() - Set{'"'} - Set{'/'} - Set{'\n'})), text(R"(")"));
}

/**
 * @brief Builds the separated repertoire's line comment, its body barred from the quote and the slash.
 * @return The regex.
 */
inline Regex separated_line_comment()
{
    return concat(text("//"), kleene(any_of(Set::all() - Set{'"'} - Set{'/'} - Set{'\n'})));
}

/**
 * @brief Builds the separated repertoire's block comment, its body barred from the quote and the slash.
 * @return The regex.
 */
inline Regex separated_block_comment()
{
    return block_comment_barring(Set{'"', '/'});
}

/**
 * @brief Builds a block comment that may not cross a line: the plain block comment with newline barred from its body.
 * @return The regex.
 */
inline Regex line_bounded_plain_block_comment()
{
    return block_comment_barring(Set{'\n'});
}

/**
 * @brief Adds the UTF-8 encoding forms of RFC 3629 section 4, one token per form and transcribed range for range: the
 *        certificate paper uses the characteristic the RFC lists, that character boundaries are found from anywhere in
 *        an octet stream, as the instance of its theorem every reader has met, and this is the token set that claim is
 *        asserted on, for the useful certificates; the bytes no form contains are certified vacuously and withheld.
 * @param builder The builder the tokens are added to.
 */
inline void utf8(munch::core::Builder& builder)
{
    const auto [two, e0, e1_ec, ed, ee_ef, f0, f1_f3, f4]{multibyte_utf8_forms()};

    builder.add_token(any_of(octets(0x00, 0x7F)), Token::literal, 1);

    builder.add_token(two, Token::literal, 1);

    builder.add_token(choice(e0, e1_ec, ed, ee_ef), Token::literal, 1);

    builder.add_token(choice(f0, f1_f3, f4), Token::literal, 1);
}

/**
 * @brief Names an ignored set in terms of the Token enum above; the certificate itself takes plain token ids.
 * @param tokens The kinds.
 * @return The kinds as token ids.
 */
inline Kinds_t ignoring(const std::initializer_list<Token> tokens)
{
    const auto id_of{[](const Token token) { return std::to_underlying(token); }};

    const auto ids{tokens | std::views::transform(id_of)};

    return Kinds_t{ids.begin(), ids.end()};
}

/**
 * @brief Builds a lexer whose relaxed certificate discards the ignored kinds.
 * @param builder The builder holding the token set.
 * @param ignored The kinds discarded.
 * @return The lexer.
 */
inline munch::core::Lexer build_ignoring(munch::core::Builder& builder, const Kinds_t& ignored)
{
    const std::vector<std::size_t> ignored_tokens{ignored.begin(), ignored.end()};

    builder.set_ignored_tokens(ignored_tokens);

    return builder.build();
}

/**
 * @brief Scans a text, recording every token's kind and length.
 * @param lexer The lexer.
 * @param text The text.
 * @return The tokens, and the bytes the scan consumed.
 */
inline std::pair<Stream_t, std::size_t> scan(const munch::core::Lexer& lexer, const std::string& text)
{
    Stream_t stream{};

    const auto record{
            [&stream](const std::size_t kind, const std::size_t length) { stream.emplace_back(kind, length); }};

    const auto consumed{lexer.tokenize_all<std::size_t>(text, record)};

    return {std::move(stream), consumed};
}

/**
 * @brief Returns the stream without the tokens of the ignored kinds.
 * @param stream The stream.
 * @param ignored The kinds deleted.
 * @return The tokens kept, in order.
 */
inline Stream_t without(const Stream_t& stream, const Kinds_t& ignored)
{
    Stream_t kept{};

    const auto is_kept{[&ignored](const std::pair<std::size_t, std::size_t>& token) {
        const auto& [kind, length]{token};

        return !ignored.contains(kind);
    }};

    std::ranges::copy_if(stream, std::back_inserter(kept), is_kept);

    return kept;
}

/**
 * @brief Returns whether the cut at an offset scans both halves completely and splices to the serial stream modulo the
 *        ignored kinds.
 * @param lexer The lexer.
 * @param ignored The ignored kinds.
 * @param text The document.
 * @param serial The document's serial token stream.
 * @param at The offset of the cut.
 * @return Whether the cut survives.
 */
inline bool cut_survives(
        const munch::core::Lexer& lexer, const Kinds_t& ignored, const std::string& text, const Stream_t& serial,
        const std::size_t at)
{
    const auto [left, left_used]{scan(lexer, text.substr(0, at))};

    const auto [right, right_used]{scan(lexer, text.substr(at))};

    if (left_used != at || right_used != text.size() - at)
    {
        return false;
    }

    Stream_t spliced{left};

    spliced.insert(spliced.end(), right.begin(), right.end());

    return without(spliced, ignored) == without(serial, ignored);
}

/**
 * @brief Joins parts with a separator between each two.
 * @tparam Parts The parts' range type.
 * @param parts The parts, in order.
 * @param separator The separator.
 * @return The joined text, empty when there are no parts.
 */
template <typename Parts>
std::string joined(const Parts& parts, const std::string_view separator)
{
    std::string text{};

    for (const auto& part : parts)
    {
        text += text.empty() ? "" : separator;

        text += part;
    }

    return text;
}

/**
 * @brief Returns the verdict that opens a figure program's checked line.
 * @param agrees Whether the figure agrees with the published one.
 * @return `  ok   ` or `  fail `.
 */
inline std::string_view verdict_label(const bool agrees)
{
    return agrees ? "  ok   " : "  fail ";
}

/**
 * @brief Prints figures against the report's and counts the disagreements.
 */
class Figure_check
{
public:
    /**
     * @brief Prints one figure against the report's, counting a disagreement.
     * @tparam Actual The computed figure's type.
     * @tparam Expected The report's figure's type.
     * @param what What the figure is.
     * @param actual The figure computed.
     * @param expected The figure the report gives.
     */
    template <typename Actual, typename Expected>
    void operator()(const std::string& what, const Actual actual, const Expected expected)
    {
        const auto agrees{actual == expected};

        std::cout << verdict_label(agrees) << what << ": " << actual << '\n';

        if (!agrees)
        {
            std::cout << "         report says " << expected << '\n';

            ++failures_;
        }
    }

    /**
     * @brief Returns how many figures disagreed with the report.
     * @return The disagreements counted.
     */
    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    /**
     * @brief The disagreements counted.
     */
    int failures_{0};
};

} // namespace figures

#endif // MUNCH_PAPER_FIGURES_GRAMMARS_HPP
