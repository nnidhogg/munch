#include "munch/tools/audit/logos_regex.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
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
/**
 * @brief What logos adds to a pattern's priority for each character it matches, a literal's or a class's.
 */
constexpr std::size_t character_priority{2};

/**
 * @brief The opener of a subpattern reference, `(?&name)`.
 */
constexpr std::string_view reference_opener{"(?&"};

/**
 * @brief One inclusive range of bytes.
 */
struct Byte_range
{
    /**
     * @brief The range's first byte.
     */
    unsigned char first{};

    /**
     * @brief Its last.
     */
    unsigned char last{};
};

/**
 * @brief Inclusive byte ranges, the members of a bracket expression.
 */
using Byte_ranges_t = std::vector<Byte_range>;

/**
 * @brief Returns the subpattern a reference names, which must be declared before it.
 * @param subpatterns The subpatterns declared so far.
 * @param name The name.
 * @param line The line of the pattern, for the refusal.
 * @return The subpattern.
 * @throws Spec_error If no subpattern of the name is declared.
 */
[[nodiscard]] const Subpattern& declared_subpattern(
        const Subpatterns_t& subpatterns, const std::string_view name, const std::size_t line)
{
    const auto found{subpatterns.find(name)};

    if (found == subpatterns.end())
    {
        throw Spec_error{std::format("the subpattern '{}' is not declared before its use", name), line};
    }

    const auto& [declared, subpattern]{*found};

    return subpattern;
}

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

        const auto& subpattern{declared_subpattern(subpatterns, name, line)};

        // The definition was compiled in its own mode, so the reference can stand for it only where that mode is the
        // one in force and no flag is; an empty definition is expanded to the nothing it adds.
        if (!expand && !subpattern.empty && !reference_flags.insensitive && !reference_flags.dot_all &&
            reference_flags.unicode == !subpattern.literal.byte_string)
        {
            return;
        }

        // logos pastes the definition's text into the pattern before the crate parses it, so the definition is read
        // under the flags and in the mode of the reference; the reference is gone once the node is replaced.
        Pattern_reader reader{subpattern.literal, reference_flags, line};

        node = reader.read();

        resolve(node, subpatterns, line, expand);

        return;
    }

    const auto resolve_inside{[&]<typename Kind>(Kind& kind) {
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
    }};

    std::visit(resolve_inside, node.kind);
}

/**
 * @brief Returns one byte of a literal with its ASCII case folded, as logos folds a byte under `ignore(ascii_case)`.
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
        return {.kind = Bytes{.bytes = std::string{byte}, .bounded = false}};
    }

    Scalar_set both{};

    both.add(value | case_bit, value | case_bit);

    both.add(value & ~case_bit, value & ~case_bit);

    return {.kind = Char_class{.set = std::move(both), .unicode = unicode}};
}

/**
 * @brief Returns a tree with the ASCII letters of every literal and class folded, which is what `ignore(ascii_case)`
 *        leaves.
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

        const auto folded_byte{[](const char byte) { return ascii_folded_byte(byte, false); }};

        std::vector<Node> pieces{};

        std::ranges::transform(bytes, std::back_inserter(pieces), folded_byte);

        if (pieces.size() == 1)
        {
            return std::move(pieces.front());
        }

        return {.kind = Concat{.parts = std::move(pieces)}};
    }

    if (std::holds_alternative<Char_class>(node.kind))
    {
        auto& set{std::get<Char_class>(node.kind).set};

        set = folded(set, false);

        return node;
    }

    const auto fold_inside{[]<typename Kind>(Kind& kind) {
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
    }};

    std::visit(fold_inside, node.kind);

    return node;
}

/**
 * @brief Returns byte ranges as the members of a bracket, a run of three or more as a range.
 * @param ranges The ranges, ascending.
 * @return The members' text, without the brackets.
 */
[[nodiscard]] std::string members(const Byte_ranges_t& ranges)
{
    std::string text{};

    for (const auto& [low, high] : ranges)
    {
        text += bracket_run(low, high);
    }

    return text;
}

/**
 * @brief Returns the number of scalars a run of bytes encodes, when it is well-formed UTF-8 as strictly as Rust reads
 *        it.
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
 * @brief Returns the scalar a run of bytes encodes.
 * @param bytes The run, the UTF-8 of exactly one scalar as scalar_count() validates it.
 * @return The scalar.
 */
[[nodiscard]] char32_t decoded(const std::string_view bytes) noexcept
{
    const auto lead{static_cast<unsigned char>(bytes.front())};

    if (lead <= last_ascii)
    {
        return lead;
    }

    // The length marker is dropped by the run's own length, which a run cut short at a literal's end may make shorter
    // than its lead byte says.
    const auto marker_bits{bytes.size() + 1};

    char32_t value{lead & (0xFFU >> marker_bits)};

    for (const char byte : bytes.substr(1))
    {
        value = continued(value, static_cast<unsigned char>(byte));
    }

    return value;
}

/**
 * @brief Returns a byte as the `\xNN` escape logos writes it in, its hex digits in lower case.
 * @param byte The byte.
 * @return The escape's four characters.
 */
[[nodiscard]] std::string byte_escape(const unsigned char byte)
{
    return std::format(R"(\x{:02x})", static_cast<unsigned>(byte));
}

/**
 * @brief Returns a literal as logos escapes it for the regex crate, which is what it compiles a token under an ignore
 *        flag from.
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

    std::string bytes{};

    for (const auto byte : literal.bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (value <= last_ascii)
        {
            bytes.push_back(byte);
        }
        else
        {
            bytes += byte_escape(value);
        }
    }

    return {.written = literal.written, .bytes = std::move(bytes), .byte_string = true};
}

/**
 * @brief Returns a class in the syntax regex::parse() reads: over bytes, a bracket of its byte ranges; over scalars, a
 *        bracket of its ASCII members and its other ranges as code point escapes, which the parser reads as the
 *        encodings of the scalars they span.
 * @param char_class The class.
 * @return The bracket.
 */
[[nodiscard]] std::string written(const Char_class& char_class)
{
    Byte_ranges_t direct{};

    std::string beyond{};

    const auto ceiling{char_class.unicode ? last_ascii : last_byte};

    for (const auto& [low, high] : char_class.set.ranges())
    {
        if (low <= ceiling)
        {
            const auto last{std::min(high, ceiling)};

            direct.push_back({.first = static_cast<unsigned char>(low), .last = static_cast<unsigned char>(last)});
        }

        if (high > ceiling)
        {
            const auto first_beyond{std::max(low, static_cast<char32_t>(ceiling + 1))};

            beyond += code_point_member(first_beyond, high);
        }
    }

    return std::format("[{}{}]", members(direct), beyond);
}

/**
 * @brief Returns a node in the syntax regex::parse() reads.
 * @param node The node.
 * @param top Whether the node is the whole pattern, which an alternation needs no parentheses around.
 * @return The text.
 */
[[nodiscard]] std::string written(const Node& node, const bool top)
{
    const auto written_kind{[top]<typename Kind>(const Kind& kind) -> std::string {
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
            return reference(kind.name);
        }
        else if constexpr (std::is_same_v<Kind, Concat>)
        {
            std::string text{};

            for (const auto& part : kind.parts)
            {
                text += written(part, false);
            }

            return text;
        }
        else if constexpr (std::is_same_v<Kind, Alternation>)
        {
            const auto branch_text{[](const Node& branch) { return written(branch, false); }};

            std::string text{top ? "" : "("};

            const auto branches{kind.branches | std::views::transform(branch_text)};

            std::ranges::copy(branches | std::views::join_with('|'), std::back_inserter(text));

            if (top)
            {
                return text;
            }

            return std::format("{})", text);
        }
        else
        {
            static_assert(std::is_same_v<Kind, Repeat>);

            const auto& operand{*kind.operand};

            const auto grouped{
                    std::holds_alternative<Concat>(operand.kind) || std::holds_alternative<Repeat>(operand.kind)};

            const auto inner{written(operand, false)};

            const auto text{grouped ? std::format("({})", inner) : inner};

            if (!kind.max && kind.min == 0)
            {
                return std::format("{}*", text);
            }

            if (!kind.max && kind.min == 1)
            {
                return std::format("{}+", text);
            }

            if (!kind.max)
            {
                return std::format("{}{{{},}}", text, kind.min);
            }

            if (kind.min == 0 && *kind.max == 1)
            {
                return std::format("{}?", text);
            }

            if (*kind.max == kind.min)
            {
                return std::format("{}{{{}}}", text, kind.min);
            }

            return std::format("{}{{{},{}}}", text, kind.min, *kind.max);
        }
    }};

    return std::visit(written_kind, node.kind);
}

/**
 * @brief Returns the priority logos computes for a node: two per scalar of a literal, two per byte when the run is not
 *        UTF-8; two for a class; the sum over a concatenation; the least over an alternation; a repetition's operand
 *        its minimum number of times.
 *
 * A reference is not expected here, the caller having read the pattern with every subpattern substituted as logos
 * substitutes it, and counts nothing.
 * @param node The node.
 * @return The priority.
 */
[[nodiscard]] std::size_t priority(const Node& node)
{
    const auto priority_of{[]<typename Kind>(const Kind& kind) -> std::size_t {
        if constexpr (std::is_same_v<Kind, Empty> || std::is_same_v<Kind, Reference>)
        {
            return 0;
        }
        else if constexpr (std::is_same_v<Kind, Bytes>)
        {
            const auto characters{scalar_count(kind.bytes).value_or(kind.bytes.size())};

            return character_priority * characters;
        }
        else if constexpr (std::is_same_v<Kind, Char_class>)
        {
            return character_priority;
        }
        else if constexpr (std::is_same_v<Kind, Concat>)
        {
            auto priorities{kind.parts | std::views::transform(priority)};

            return std::ranges::fold_left(priorities, 0UZ, std::plus{});
        }
        else if constexpr (std::is_same_v<Kind, Alternation>)
        {
            auto priorities{kind.branches | std::views::transform(priority)};

            return std::ranges::fold_left(priorities, std::numeric_limits<std::size_t>::max(), std::ranges::min);
        }
        else
        {
            static_assert(std::is_same_v<Kind, Repeat>);

            return kind.min * priority(*kind.operand);
        }
    }};

    return std::visit(priority_of, node.kind);
}

} // namespace

Scalar_set folded(const Scalar_set& set, const bool unicode)
{
    auto result{set};

    for (char32_t lower{'a'}; lower <= 'z'; ++lower)
    {
        const auto upper{lower & ~case_bit};

        if (set.contains(lower) || set.contains(upper))
        {
            result.add(lower, lower);

            result.add(upper, upper);
        }
    }

    // Under the Unicode flag, a set holding the letter or the scalar gets both cases and the scalar.
    const auto join_fold{[&result, &set, unicode](const char32_t lower, const char32_t upper, const char32_t special) {
        if (!unicode || !(result.contains(lower) || set.contains(special)))
        {
            return;
        }

        result.add(lower, lower);

        result.add(upper, upper);

        result.add(special, special);
    }};

    join_fold('s', 'S', long_s);

    join_fold('k', 'K', kelvin);

    return result;
}

std::optional<Merged> merged(const Node& node)
{
    if (std::holds_alternative<Char_class>(node.kind))
    {
        const auto& given{std::get<Char_class>(node.kind)};

        return given.captured ? std::nullopt : std::optional{Merged{.char_class = given, .literal = false}};
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

        Scalar_set one{};

        if (scalar_count(bytes) == 1)
        {
            const auto scalar{decoded(bytes)};

            one.add(scalar, scalar);

            return Merged{.char_class = {.set = std::move(one), .unicode = true, .captured = false}, .literal = true};
        }

        if (bytes.size() == 1)
        {
            const auto byte{static_cast<unsigned char>(bytes.front())};

            one.add(byte, byte);

            return Merged{.char_class = {.set = std::move(one), .unicode = false, .captured = false}, .literal = true};
        }

        return std::nullopt;
    }

    if (!std::holds_alternative<Alternation>(node.kind))
    {
        return std::nullopt;
    }

    const auto& [alternatives, captured]{std::get<Alternation>(node.kind)};

    if (captured)
    {
        return std::nullopt;
    }

    std::vector<Merged> branches{};

    for (const auto& branch : alternatives)
    {
        auto seen{merged(branch)};

        // All classes or all literals; a mix stays an alternation.
        if (!seen || (!branches.empty() && seen->literal != branches.front().literal))
        {
            return std::nullopt;
        }

        branches.push_back(std::move(*seen));
    }

    const auto ascii_only{[](const Merged& one) {
        const auto& set{one.char_class.set};

        if (set.empty())
        {
            return true;
        }

        const auto [low, high]{set.ranges().back()};

        return high <= last_ascii;
    }};

    // Unicode first: every byte class must be ASCII; then bytes: every Unicode class must be.
    for (const auto unicode : {true, false})
    {
        const auto fits{[&ascii_only, unicode](const Merged& one) {
            return one.char_class.unicode == unicode || ascii_only(one);
        }};

        if (!std::ranges::all_of(branches, fits))
        {
            continue;
        }

        Merged whole{.char_class = {.set = {}, .unicode = unicode, .captured = false}, .literal = false};

        for (const auto& one : branches)
        {
            whole.char_class.set.add(one.char_class.set);
        }

        whole.literal = whole.char_class.set.single().has_value();

        return whole;
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
        auto bytes{substituted(literal, subpatterns, line)};

        const String_literal pasted{
                .written = literal.written,
                .bytes = std::move(bytes),
                .byte_string = literal.byte_string};

        Pattern_reader pasted_reader{pasted, flags, line};

        whole = pasted_reader.read();

        if (fold_ascii)
        {
            whole = ascii_folded(std::move(whole));
        }
    }

    // Emptiness is a rule's matter, decided once the subpatterns are pasted in (logos-codegen 0.15.1,
    // parser/subpattern.rs and graph/regex.rs).
    if (kind != Pattern_kind::definition && std::holds_alternative<Empty>(whole.kind))
    {
        const std::string_view consequence{
                token ? ", which logos 0.15.1 panics on" :
                        ", which logos 0.15.1 compiles into a rule matching no input, so it is no token"};

        const auto message{std::format("the pattern {} matches only the empty string{}", literal.written, consequence)};

        throw Spec_error{message, line};
    }

    check_dot_repetitions(whole, literal.written, line);

    check_repetitions(whole, {}, line);

    auto expression{written(node, true)};

    if (token && folding == Ignore_case::none)
    {
        return {.expression = std::move(expression), .priority = character_priority * literal.bytes.size()};
    }

    return {.expression = std::move(expression), .priority = priority(whole)};
}

std::string substituted(const String_literal& literal, const Subpatterns_t& subpatterns, const std::size_t line)
{
    std::string text{};

    const auto& bytes{literal.bytes};

    for (std::size_t at{0}; at < bytes.size();)
    {
        const auto opens{std::string_view{bytes}.substr(at).starts_with(reference_opener)};

        const auto name_at{at + reference_opener.size()};

        const auto close{opens ? bytes.find(')', name_at) : std::string::npos};

        if (close != std::string::npos)
        {
            const auto name{bytes.substr(name_at, close - name_at)};

            const auto& subpattern{declared_subpattern(subpatterns, name, line)};

            text += std::format("(?:{})", subpattern.text);

            at = close + 1;

            continue;
        }

        const auto lead{static_cast<unsigned char>(bytes[at])};

        if (lead <= last_ascii)
        {
            text.push_back(bytes[at]);

            ++at;
        }
        else if (literal.byte_string)
        {
            text += byte_escape(lead);

            ++at;
        }
        else
        {
            // A string literal is UTF-8, so the lead byte gives the scalar's length.
            const auto length{sequence_length(lead)};

            const auto scalar{decoded(bytes.substr(at, length))};

            text += std::format(R"(\x{{{:x}}})", static_cast<std::uint32_t>(scalar));

            at += length;
        }
    }

    return text;
}

} // namespace munch::tools::audit
