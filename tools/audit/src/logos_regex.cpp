#include "munch/tools/audit/logos_regex.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/logos_pattern.hpp"
#include "munch/tools/audit/logos_refusals.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements logos_regex.hpp: a node written for the parser and the priority logos counts over it, the counts over
// UTF-8 they rest on, the references resolved, a token escaped for an ignore flag and the ASCII folding of
// `ignore(ascii_case)` are private to this unit.

/**
 * @brief Inclusive byte ranges, the members of a bracket expression or the positions of a UTF-8 sequence.
 */
using Byte_ranges_t = std::vector<std::pair<unsigned char, unsigned char>>;

/**
 * @brief Resolves every subpattern reference in a tree: one in the subpattern's own mode under the default flags keeps
 *        its name, one under `i` or `s` or in the other mode, or every one where the caller asks, is replaced by the
 *        subpattern read again under those flags and in that mode, as logos's textual substitution lets the flags and
 *        the mode reach in.
 *
 * logos substitutes the definition's text, a byte string's bytes beyond ASCII spelled `\xHH`, into the pattern before
 * the crate parses it, so a byte string's `\xHH` is the scalar U+00HH inside Unicode mode and a string's scalar its
 * UTF-8 outside it (logos-codegen 0.15.1, parser/subpattern.rs and parser/definition.rs).
 * @param node The tree.
 * @param subpatterns The subpatterns declared.
 * @param line The line, for refusals.
 * @param expand Whether every reference is replaced by its subpattern, which a folding over the whole pattern needs,
 *        logos having substituted the text before it compiled anything.
 * @throws Spec_error If a reference names no subpattern declared before it.
 */
void resolve(Node& node, const Subpatterns_t& subpatterns, const std::size_t line, const bool expand)
{
    if (std::holds_alternative<Reference>(node.kind))
    {
        const auto& [name, reference_flags]{std::get<Reference>(node.kind)};

        const auto found{subpatterns.find(name)};

        if (found == subpatterns.end())
        {
            throw Spec_error{"the subpattern '" + name + "' is not declared before its use", line};
        }

        const auto& subpattern{found->second};

        // The definition was compiled in its own mode, so the reference can stand for it only where that mode is the
        // one in force and no flag is; an empty definition is expanded to the nothing it adds.
        if (!expand && !subpattern.empty && !reference_flags.insensitive && !reference_flags.dot_all &&
            reference_flags.unicode == !subpattern.literal.byte_string)
        {
            return;
        }

        // logos pastes the definition's text into the pattern before the crate parses it, so the definition is read
        // under the flags and in the mode of the reference; the reference is gone once the node is replaced.
        node = Pattern_reader{subpattern.literal, reference_flags, line}.read();

        resolve(node, subpatterns, line, expand);

        return;
    }

    std::visit(
            [&]<typename Kind>(Kind& kind) {
                if constexpr (std::is_same_v<Kind, Concat>)
                {
                    for (auto& part : kind.parts)
                    {
                        resolve(part, subpatterns, line, expand);
                    }
                }
                else if constexpr (std::is_same_v<Kind, Alternation>)
                {
                    for (auto& branch : kind.branches)
                    {
                        resolve(branch, subpatterns, line, expand);
                    }
                }
                else if constexpr (std::is_same_v<Kind, Repeat>)
                {
                    resolve(*kind.operand, subpatterns, line, expand);
                }
            },
            node.kind);
}

/**
 * @brief One byte of a literal with its ASCII case folded, as logos folds a byte under `ignore(ascii_case)`.
 *
 * An ASCII letter becomes the two cases, which logos writes as an alternation of two one-byte literals and this reads
 * as the class of them, the two being one language and one priority; every other byte stays the byte it is.
 * @param byte The byte.
 * @param unicode Whether the pattern's classes are over scalars, which the class this may build inherits.
 * @return The node.
 */
[[nodiscard]] Node ascii_folded_byte(const char byte, const bool unicode)
{
    const auto value{static_cast<unsigned char>(byte)};

    if (!is_letter(value))
    {
        return {.kind = Bytes{.bytes = std::string(1, byte), .bounded = false}};
    }

    Scalar_set both;

    both.add(value | 0x20U, value | 0x20U);

    both.add(value & ~0x20U, value & ~0x20U);

    return {.kind = Char_class{.set = std::move(both), .unicode = unicode}};
}

/**
 * @brief A tree with the ASCII letters of every literal and class folded, which is what `ignore(ascii_case)` leaves.
 *
 * logos parses the pattern as it stands and then walks the compiled tree: a class gains the other case of its ASCII
 * letters, and a literal is taken apart into one piece per byte, each of them the two cases where the byte is an ASCII
 * letter. The pieces stay apart, so a run of several bytes counts two for each of them rather than two for each
 * character, which is the priority logos ends up with (logos-codegen 0.15.1, parser/ignore_flags.rs).
 * @param node The tree, its references expanded.
 * @return The folded tree.
 */
[[nodiscard]] Node ascii_folded(Node node)
{
    if (std::holds_alternative<Bytes>(node.kind))
    {
        const auto& [bytes, bounded]{std::get<Bytes>(node.kind)};

        std::vector<Node> pieces;

        for (const auto byte : bytes)
        {
            pieces.push_back(ascii_folded_byte(byte, false));
        }

        return pieces.size() == 1 ? std::move(pieces.front()) : Node{.kind = Concat{.parts = std::move(pieces)}};
    }

    if (std::holds_alternative<Char_class>(node.kind))
    {
        auto& set{std::get<Char_class>(node.kind).set};

        set = folded(set, false);

        return node;
    }

    std::visit(
            []<typename Kind>(Kind& kind) {
                if constexpr (std::is_same_v<Kind, Concat>)
                {
                    for (auto& part : kind.parts)
                    {
                        part = ascii_folded(std::move(part));
                    }
                }
                else if constexpr (std::is_same_v<Kind, Alternation>)
                {
                    for (auto& branch : kind.branches)
                    {
                        branch = ascii_folded(std::move(branch));
                    }
                }
                else if constexpr (std::is_same_v<Kind, Repeat>)
                {
                    *kind.operand = ascii_folded(std::move(*kind.operand));
                }
            },
            node.kind);

    return node;
}

/**
 * @brief Byte ranges as the members of a bracket, a run of three or more as a range.
 * @param ranges The ranges, ascending.
 * @return The members' text, without the brackets.
 */
[[nodiscard]] std::string members(const Byte_ranges_t& ranges)
{
    std::string text;

    for (const auto& [low, high] : ranges)
    {
        text += bracket_member(low);

        if (high > low + 1)
        {
            text += '-';
        }

        if (high > low)
        {
            text += bracket_member(high);
        }
    }

    return text;
}

/**
 * @brief The number of scalars a run of bytes encodes, when it is well-formed UTF-8 as strictly as Rust reads it.
 *
 * logos takes a literal's priority from `std::str::from_utf8` and falls back to the byte length where that fails, so
 * the validation is that one, well_formed_length()'s: the shortest form only, no encoding of a surrogate and nothing
 * above U+10FFFF. A byte string that only looks like UTF-8, `ED A0 80` among them, counts as its bytes and scores twice
 * as much.
 * @param bytes The run.
 * @return The count, or std::nullopt when the run is not UTF-8.
 */
[[nodiscard]] std::optional<std::size_t> scalar_count(const std::string_view bytes) noexcept
{
    std::size_t count{0};

    for (std::size_t at{0}; at < bytes.size(); ++count)
    {
        const auto length{well_formed_length(bytes, at)};

        if (length == 0)
        {
            return std::nullopt;
        }

        at += length;
    }

    return count;
}

/**
 * @brief The scalar a run of bytes encodes.
 * @param bytes The run, the UTF-8 of exactly one scalar as scalar_count() validates it.
 * @return The scalar.
 */
[[nodiscard]] char32_t decoded(const std::string_view bytes) noexcept
{
    const auto lead{static_cast<unsigned char>(bytes.front())};

    if (lead < 0x80)
    {
        return lead;
    }

    char32_t value{lead & (0xFFU >> (bytes.size() + 1))};

    for (const char byte : bytes.substr(1))
    {
        value = (value << 6U) | (static_cast<unsigned char>(byte) & 0x3FU);
    }

    return value;
}

/**
 * @brief A literal as logos escapes it for the regex crate, which is what it compiles a token under an ignore flag
 *        from.
 *
 * A byte string's bytes are written out as text first, a byte beyond ASCII as the four characters of its `\xNN` escape
 * in lower case, and that text is then escaped for the crate, which escapes the backslash just written: the pattern the
 * crate compiles matches the escape's own characters, so the token never matches the byte it names. A string literal
 * keeps its characters, the escaping of a metacharacter leaving the same one character to match.
 * @param literal The literal.
 * @return The literal over the bytes logos compiles.
 */
[[nodiscard]] String_literal escaped_literal(const String_literal& literal)
{
    if (!literal.byte_string)
    {
        return literal;
    }

    std::string bytes;

    for (const auto byte : literal.bytes)
    {
        const auto value{static_cast<unsigned>(static_cast<unsigned char>(byte))};

        if (value < 0x80)
        {
            bytes.push_back(byte);
        }
        else
        {
            bytes += std::format(R"(\x{:02x})", value);
        }
    }

    return {.written = literal.written, .bytes = std::move(bytes), .byte_string = true};
}

/**
 * @brief A class in the syntax regex::parse() reads: over bytes, a bracket of its byte ranges; over scalars, a bracket
 *        of its ASCII members and its other ranges as code point escapes, which the parser reads as the encodings of
 *        the scalars they span.
 * @param cls The class.
 * @return The bracket.
 */
[[nodiscard]] std::string written(const Char_class& cls)
{
    Byte_ranges_t direct;

    std::string beyond;

    const char32_t ceiling{cls.unicode ? 0x7FU : 0xFFU};

    for (const auto& [low, high] : cls.set.ranges())
    {
        if (low <= ceiling)
        {
            direct.emplace_back(static_cast<unsigned char>(low), static_cast<unsigned char>(std::min(high, ceiling)));
        }

        if (high > ceiling)
        {
            const auto from{static_cast<std::uint32_t>(std::max(low, static_cast<char32_t>(ceiling + 1)))};

            beyond += from == high ? std::format(R"(\u{{{:x}}})", from) :
                                     std::format(R"(\u{{{:x}}}-\u{{{:x}}})", from, static_cast<std::uint32_t>(high));
        }
    }

    return '[' + members(direct) + beyond + ']';
}

/**
 * @brief A node in the syntax regex::parse() reads.
 * @param node The node.
 * @param top Whether the node is the whole pattern, which an alternation needs no parentheses around.
 * @return The text.
 */
[[nodiscard]] std::string written(const Node& node, const bool top)
{
    return std::visit(
            [top]<typename Kind>(const Kind& kind) -> std::string {
                if constexpr (std::is_same_v<Kind, Empty>)
                {
                    return {};
                }
                else if constexpr (std::is_same_v<Kind, Bytes>)
                {
                    return quoted(kind.bytes);
                }
                else if constexpr (std::is_same_v<Kind, Char_class>)
                {
                    return written(kind);
                }
                else if constexpr (std::is_same_v<Kind, Reference>)
                {
                    return '{' + kind.name + '}';
                }
                else if constexpr (std::is_same_v<Kind, Concat>)
                {
                    std::string text;

                    for (const auto& part : kind.parts)
                    {
                        text += written(part, false);
                    }

                    return text;
                }
                else if constexpr (std::is_same_v<Kind, Alternation>)
                {
                    std::string text{top ? "" : "("};

                    for (std::size_t index{0}; index < kind.branches.size(); ++index)
                    {
                        text += (index == 0 ? "" : "|") + written(kind.branches[index], false);
                    }

                    return top ? text : text + ')';
                }
                else
                {
                    static_assert(std::is_same_v<Kind, Repeat>);

                    const auto& operand{*kind.operand};

                    const auto grouped{
                            std::holds_alternative<Concat>(operand.kind) ||
                            std::holds_alternative<Repeat>(operand.kind)};

                    const auto text{grouped ? '(' + written(operand, false) + ')' : written(operand, false)};

                    if (!kind.max)
                    {
                        return kind.min == 0 ? text + '*' :
                               kind.min == 1 ? text + '+' :
                                               text + '{' + std::to_string(kind.min) + ",}";
                    }

                    if (kind.min == 0 && *kind.max == 1)
                    {
                        return text + '?';
                    }

                    return text + '{' + std::to_string(kind.min) +
                           (*kind.max == kind.min ? "" : ',' + std::to_string(*kind.max)) + '}';
                }
            },
            node.kind);
}

/**
 * @brief The priority logos computes for a node: two per scalar of a literal, two per byte when the run is not UTF-8;
 *        two for a class; the sum over a concatenation; the least over an alternation; a repetition's operand its
 *        minimum number of times.
 *
 * A reference is not expected here, the caller having read the pattern with every subpattern substituted as logos
 * substitutes it, and counts nothing.
 * @param node The node.
 * @return The priority.
 */
[[nodiscard]] std::size_t priority(const Node& node)
{
    return std::visit(
            []<typename Kind>(const Kind& kind) -> std::size_t {
                if constexpr (std::is_same_v<Kind, Empty> || std::is_same_v<Kind, Reference>)
                {
                    return 0;
                }
                else if constexpr (std::is_same_v<Kind, Bytes>)
                {
                    return 2 * scalar_count(kind.bytes).value_or(kind.bytes.size());
                }
                else if constexpr (std::is_same_v<Kind, Char_class>)
                {
                    return 2;
                }
                else if constexpr (std::is_same_v<Kind, Concat>)
                {
                    std::size_t sum{0};

                    for (const auto& part : kind.parts)
                    {
                        sum += priority(part);
                    }

                    return sum;
                }
                else if constexpr (std::is_same_v<Kind, Alternation>)
                {
                    auto least{std::numeric_limits<std::size_t>::max()};

                    for (const auto& branch : kind.branches)
                    {
                        least = std::min(least, priority(branch));
                    }

                    return least;
                }
                else
                {
                    static_assert(std::is_same_v<Kind, Repeat>);

                    return kind.min * priority(*kind.operand);
                }
            },
            node.kind);
}

} // namespace

Scalar_set folded(const Scalar_set& set, const bool unicode)
{
    constexpr char32_t long_s{0x17F};

    constexpr char32_t kelvin{0x212A};

    auto result{set};

    for (char32_t lower{'a'}; lower <= 'z'; ++lower)
    {
        const auto upper{lower - 0x20};

        if (set.contains(lower) || set.contains(upper))
        {
            result.add(lower, lower);

            result.add(upper, upper);
        }
    }

    if (unicode && (result.contains('s') || set.contains(long_s)))
    {
        result.add('s', 's');
        result.add('S', 'S');
        result.add(long_s, long_s);
    }

    if (unicode && (result.contains('k') || set.contains(kelvin)))
    {
        result.add('k', 'k');
        result.add('K', 'K');
        result.add(kelvin, kelvin);
    }

    return result;
}

std::optional<Merged> merged(const Node& node)
{
    if (std::holds_alternative<Char_class>(node.kind))
    {
        const auto& given{std::get<Char_class>(node.kind)};

        return given.captured ? std::nullopt : std::optional{Merged{.cls = given, .literal = false}};
    }

    // A literal of one character, in either mode, is a one-character literal to the crate's merging, and a lone byte
    // beyond ASCII a one-byte literal; a literal a capture bounds is not a literal to it.
    if (std::holds_alternative<Bytes>(node.kind))
    {
        const auto& [bytes, bounded]{std::get<Bytes>(node.kind)};

        if (bounded)
        {
            return std::nullopt;
        }

        Scalar_set one;

        if (scalar_count(bytes) == 1)
        {
            one.add(decoded(bytes), decoded(bytes));

            return Merged{.cls = {.set = std::move(one), .unicode = true, .captured = false}, .literal = true};
        }

        if (bytes.size() == 1)
        {
            one.add(static_cast<unsigned char>(bytes.front()), static_cast<unsigned char>(bytes.front()));

            return Merged{.cls = {.set = std::move(one), .unicode = false, .captured = false}, .literal = true};
        }

        return std::nullopt;
    }

    if (!std::holds_alternative<Alternation>(node.kind) || std::get<Alternation>(node.kind).captured)
    {
        return std::nullopt;
    }

    std::vector<Merged> branches;

    for (const auto& branch : std::get<Alternation>(node.kind).branches)
    {
        auto seen{merged(branch)};

        // All classes or all literals; a mix stays an alternation.
        if (!seen || (!branches.empty() && seen->literal != branches.front().literal))
        {
            return std::nullopt;
        }

        branches.push_back(std::move(*seen));
    }

    // Unicode first: every byte class must be ASCII; then bytes: every Unicode class must be.
    const auto ascii_only{
            [](const Merged& one) { return one.cls.set.empty() || one.cls.set.ranges().back().second <= 0x7F; }};

    for (const auto unicode : {true, false})
    {
        if (std::ranges::all_of(
                    branches, [&](const Merged& one) { return one.cls.unicode == unicode || ascii_only(one); }))
        {
            Merged whole{.cls = {.set = {}, .unicode = unicode, .captured = false}, .literal = false};

            for (const auto& one : branches)
            {
                whole.cls.set.add(one.cls.set);
            }

            whole.literal = whole.cls.set.single().has_value();

            return whole;
        }
    }

    return std::nullopt;
}

Compiled compile(
        const String_literal& literal, const Pattern_kind kind, const Ignore_case folding,
        const Subpatterns_t& subpatterns, const std::size_t line)
{
    const auto token{kind == Pattern_kind::token};

    // `ignore(case)` is the crate's case-insensitive parse, and so is `ignore(ascii_case)` over a byte string, which
    // logos hands the same binary parse; over a string pattern the ASCII flag folds the tree afterwards instead.
    const auto parsed_insensitive{
            folding == Ignore_case::unicode || (folding == Ignore_case::ascii && literal.byte_string)};

    const auto fold_ascii{folding == Ignore_case::ascii && !literal.byte_string};

    const Flags flags{
            .insensitive = parsed_insensitive,
            .dot_all = false,
            .unicode = !literal.byte_string,
            .utf8 = !literal.byte_string};

    // An ignore flag takes a `#[token]` out of the literals and into the regexes: logos escapes the literal for the
    // regex crate, compiles that regex under the crate's case folding, and takes the priority from it rather than from
    // the byte length (logos-codegen 0.15.1, lib.rs and parser/definition.rs).
    const auto compiled{token && folding != Ignore_case::none ? escaped_literal(literal) : literal};

    Pattern_reader reader{compiled, flags, line};

    auto node{token ? reader.read_literal() : reader.read()};

    resolve(node, subpatterns, line, fold_ascii);

    if (fold_ascii)
    {
        node = ascii_folded(std::move(node));
    }

    // The priority and the boundaries are decided over the whole pattern as logos compiled it, read from the text the
    // crate's own tree and graph were built from, the subpatterns substituted into it; a token has no reference to
    // substitute, and logos hands a token under an ignore flag no subpatterns at all.
    auto whole{node};

    if (!token)
    {
        const String_literal pasted{
                .written = literal.written,
                .bytes = substituted(literal, subpatterns, line),
                .byte_string = literal.byte_string};

        whole = Pattern_reader{pasted, flags, line}.read();

        if (fold_ascii)
        {
            whole = ascii_folded(std::move(whole));
        }
    }

    // Emptiness is a rule's matter, decided once the subpatterns are pasted in (logos-codegen 0.15.1,
    // parser/subpattern.rs and graph/regex.rs).
    if (kind != Pattern_kind::definition && std::holds_alternative<Empty>(whole.kind))
    {
        throw Spec_error{
                "the pattern " + literal.written + " matches only the empty string" +
                        (token ? ", which logos 0.15.1 panics on" :
                                 ", which logos 0.15.1 compiles into a rule matching no input, so it is no token"),
                line};
    }

    check_dot_repetitions(whole, literal.written, line);

    check_repetitions(whole, {}, line);

    return {.expression = written(node, true),
            .priority = token && folding == Ignore_case::none ? 2 * literal.bytes.size() : priority(whole)};
}

std::string substituted(const String_literal& literal, const Subpatterns_t& subpatterns, const std::size_t line)
{
    std::string text;

    const auto& bytes{literal.bytes};

    for (std::size_t at{0}; at < bytes.size();)
    {
        const auto close{bytes.compare(at, 3, "(?&") == 0 ? bytes.find(')', at + 3) : std::string::npos};

        if (close != std::string::npos)
        {
            const auto name{bytes.substr(at + 3, close - at - 3)};

            const auto found{subpatterns.find(name)};

            if (found == subpatterns.end())
            {
                throw Spec_error{"the subpattern '" + name + "' is not declared before its use", line};
            }

            text += "(?:" + found->second.text + ")";

            at = close + 1;

            continue;
        }

        const auto lead{static_cast<unsigned char>(bytes[at])};

        if (lead < 0x80)
        {
            text.push_back(bytes[at++]);
        }
        else if (literal.byte_string)
        {
            text += std::format(R"(\x{:02x})", static_cast<unsigned>(lead));

            ++at;
        }
        else
        {
            // A string literal is UTF-8, so the lead byte gives the scalar's length.
            const auto length{sequence_length(lead)};

            text += std::format(R"(\x{{{:x}}})", static_cast<std::uint32_t>(decoded(bytes.substr(at, length))));

            at += length;
        }
    }

    return text;
}

} // namespace munch::tools::audit
