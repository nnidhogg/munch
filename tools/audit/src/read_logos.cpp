#include "munch/tools/audit/read_logos.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "munch/regex/indirect.hpp"
#include "munch/tools/audit/cursor.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The regex crate's flags a pattern is read under, each scoped to the group it is set in.
 */
struct Flags
{
    /**
     * @brief `i`: a letter matches either case.
     */
    bool insensitive{false};

    /**
     * @brief `s`: the dot matches the newline as well.
     */
    bool dot_all{false};

    /**
     * @brief `u`: the pattern is over scalars, encoded as UTF-8; off, it is over bytes.
     */
    bool unicode{true};
};

/**
 * @brief A Rust string literal: the text exactly as written, its content decoded, and whether it is a byte string.
 */
struct Literal
{
    /**
     * @brief The literal as it stands in the source, prefix and quotes included.
     */
    std::string written;

    /**
     * @brief The content, the escapes of a plain string decoded and a raw string's taken verbatim.
     */
    std::string bytes;

    /**
     * @brief Whether the literal is a byte string, `b"..."`, whose pattern is over bytes.
     */
    bool byte_string;
};

/**
 * @brief One `#[path(...)]` attribute: its path, where the text between its delimiters lies, and its line.
 */
struct Attribute
{
    /**
     * @brief The path, `derive`, `logos`, `token`, `regex` or another.
     */
    std::string path;

    /**
     * @brief The offset of the first byte inside the delimiters, equal to `end` when there are none.
     */
    std::size_t begin;

    /**
     * @brief The offset just past the last byte inside the delimiters.
     */
    std::size_t end;

    /**
     * @brief The line the attribute opens on.
     */
    std::size_t line;
};

/**
 * @brief What a `#[token(...)]`, a `#[regex(...)]` or a `skip(...)` says: the literal and the arguments after it.
 */
struct Definition
{
    /**
     * @brief The pattern's literal.
     */
    Literal literal;

    /**
     * @brief The callback's text as written, empty when there is none.
     */
    std::string callback;

    /**
     * @brief The `priority = n` override, when given.
     */
    std::optional<std::size_t> priority;

    /**
     * @brief Whether `ignore(case)` was given.
     */
    bool insensitive;

    /**
     * @brief The line the attribute opens on.
     */
    std::size_t line;
};

/**
 * @brief A subpattern as a reference expands it: its literal and its priority under the default flags.
 */
struct Subpattern
{
    /**
     * @brief The literal, whose kind fixes the mode the subpattern is read in wherever it is referenced.
     */
    Literal literal;

    /**
     * @brief The priority of the subpattern read under the default flags.
     */
    std::size_t priority;
};

/**
 * @brief The subpatterns declared so far, by name.
 */
using Subpatterns_t = std::map<std::string, Subpattern, std::less<>>;

/**
 * @brief Inclusive byte ranges, the members of a bracket expression or the positions of a UTF-8 sequence.
 */
using Byte_ranges_t = std::vector<std::pair<unsigned char, unsigned char>>;

/**
 * @brief The last scalar, the ceiling of a hex escape and of every class over scalars.
 */
constexpr char32_t last_scalar{0x10FFFF};

/**
 * @brief One literal unit of a pattern: a scalar, or a byte, which `\xHH` and a byte string's bytes are outside
 *        Unicode mode; a scalar stands for its UTF-8 in either mode, as the crate has it.
 */
struct Unit
{
    /**
     * @brief The scalar, or the byte.
     */
    char32_t value;

    /**
     * @brief Whether the unit is a byte.
     */
    bool byte;
};

/**
 * @brief A set of scalars, or of bytes when the pattern is over bytes, as sorted disjoint ranges.
 */
class Scalar_set
{
public:
    /**
     * @brief An inclusive range.
     */
    using Range_t = std::pair<char32_t, char32_t>;

    /**
     * @brief Adds a range, merging it with the ranges it touches.
     * @param low The first member.
     * @param high The last member, at least the first.
     */
    void add(char32_t low, char32_t high);

    /**
     * @brief Adds every member of another set.
     * @param other The set.
     */
    void add(const Scalar_set& other);

    /**
     * @brief The members of this set that are not in another.
     * @param other The set to remove.
     * @return The difference.
     */
    [[nodiscard]] Scalar_set minus(const Scalar_set& other) const;

    /**
     * @brief The ranges, ascending and disjoint.
     * @return The ranges.
     */
    [[nodiscard]] const std::vector<Range_t>& ranges() const noexcept;

    /**
     * @brief Whether a value is a member.
     * @param value The value.
     * @return True when it is.
     */
    [[nodiscard]] bool contains(char32_t value) const noexcept;

    /**
     * @brief Whether the set has no member.
     * @return True when empty.
     */
    [[nodiscard]] bool empty() const noexcept;

    /**
     * @brief The one member, when there is exactly one.
     * @return The member, or std::nullopt.
     */
    [[nodiscard]] std::optional<char32_t> single() const noexcept;

private:
    /**
     * @brief The ranges, kept ascending, disjoint and apart.
     */
    std::vector<Range_t> ranges_;
};

struct Node;

/**
 * @brief The empty pattern, which matches the empty string and nothing else; what `(?i)` and an empty branch leave.
 */
struct Empty
{
};

/**
 * @brief A run of literal bytes, a scalar's UTF-8 in Unicode mode and the byte itself outside it.
 */
struct Bytes
{
    /**
     * @brief The bytes.
     */
    std::string bytes;

    /**
     * @brief Whether the run opens or closes a capture group, which keeps it apart from the run beside it, as the
     *        crate's literals stay apart across a capture and logos counts them apart.
     */
    bool bounded;
};

/**
 * @brief A class: the set of scalars, or of bytes, one of which is matched.
 */
struct Class
{
    /**
     * @brief The members.
     */
    Scalar_set set;

    /**
     * @brief Whether the members are scalars, matched as their UTF-8, rather than bytes.
     */
    bool unicode;
};

/**
 * @brief A reference to a subpattern, `(?&name)`, with the flags in force where it stands.
 */
struct Reference
{
    /**
     * @brief The subpattern's name.
     */
    std::string name;

    /**
     * @brief The flags in force at the reference, which logos lets reach into the expansion.
     */
    Flags flags;

    /**
     * @brief The subpattern's priority, filled in when the reference is resolved.
     */
    std::size_t priority;
};

/**
 * @brief A concatenation of two or more parts.
 */
struct Concat
{
    /**
     * @brief The parts, in order.
     */
    std::vector<Node> parts;
};

/**
 * @brief An alternation of two or more branches.
 */
struct Choice
{
    /**
     * @brief The branches, in order.
     */
    std::vector<Node> branches;
};

/**
 * @brief A repetition of an operand between a minimum and a maximum number of times.
 */
struct Repeat
{
    /**
     * @brief The operand.
     */
    regex::Indirect<Node> operand;

    /**
     * @brief The least number of times.
     */
    std::size_t min;

    /**
     * @brief The most number of times, unbounded when std::nullopt.
     */
    std::optional<std::size_t> max;
};

/**
 * @brief One node of a pattern as read: the tree the priority is computed on and the expression is written from.
 *
 * The tree keeps what logos's priority tells apart, a literal from a class, and nothing the regex crate's own
 * simplifications would erase differently: a group is its content, a lazy operator its greedy form, a single-member
 * class the literal it is.
 */
struct Node
{
    /**
     * @brief The node's kind and content.
     */
    std::variant<Empty, Bytes, Class, Reference, Concat, Choice, Repeat> kind;
};

/**
 * @brief The expression a pattern was rewritten into and the priority logos gives it.
 */
struct Compiled
{
    /**
     * @brief The expression in the syntax regex::parse() reads.
     */
    std::string expression;

    /**
     * @brief The priority, logos's own number.
     */
    std::size_t priority;
};

/**
 * @brief A cursor over a stretch of Rust source, reading it token by token as far as attributes and enums need.
 *
 * The cursor knows Rust's lexical shapes well enough never to be misled by them: comments of both styles, block
 * comments nesting, string literals in their plain, raw and byte forms, character literals as against lifetimes,
 * and the three kinds of delimited group. A cursor may be bounded to a span of the text, an attribute's content,
 * while still counting lines from the file's start.
 */
class Rust_cursor : public Cursor
{
public:
    /**
     * @brief Binds the cursor to a span of the text.
     * @param text The whole file.
     * @param begin The offset the cursor starts at.
     * @param end The offset the span ends at.
     */
    Rust_cursor(std::string_view text, std::size_t begin, std::size_t end);

    /**
     * @brief Skips blanks and comments.
     * @throws Spec_error If a block comment is never closed.
     */
    void skip_trivia();

    /**
     * @brief Skips one token: a comment, a string or character literal, a delimited group with everything in it,
     *        a word, or a single byte.
     * @throws Spec_error If a literal or a group is left open.
     */
    void skip_token();

    /**
     * @brief Skips the delimited group opening at the cursor, through its close.
     * @throws Spec_error If the group is left open or closed by the wrong delimiter.
     */
    void skip_group();

    /**
     * @brief Consumes the word at the cursor, letters, digits, underscores and non-ASCII bytes, a raw identifier's
     *        `r#` prefix included.
     * @return The word, empty when none stands here.
     */
    [[nodiscard]] std::string_view word();

    /**
     * @brief Consumes and decodes the string literal at the cursor.
     * @return The literal.
     * @throws Spec_error If no string literal stands here, it is left open, or an escape is not Rust's.
     */
    [[nodiscard]] Literal literal();

    /**
     * @brief A cursor over a span inside this one, counting lines from the same file start.
     * @param begin The span's first offset.
     * @param end The offset the span ends at.
     * @return The cursor.
     */
    [[nodiscard]] Rust_cursor inside(std::size_t begin, std::size_t end) const noexcept;

    /**
     * @brief The text between two offsets.
     * @param begin The first offset.
     * @param end The offset past the last.
     * @return The text.
     */
    [[nodiscard]] std::string_view slice(std::size_t begin, std::size_t end) const noexcept;

    /**
     * @brief Whether a string literal, in any of its prefixed forms, opens at the cursor.
     * @return True when one does.
     * @throws Spec_error If it is left open.
     */
    [[nodiscard]] bool at_string() const;

private:
    /**
     * @brief The offset just past the string literal at the cursor, its prefix, hashes and escapes honoured.
     * @return The offset, or std::nullopt when no string literal opens here.
     * @throws Spec_error If the literal is left open.
     */
    [[nodiscard]] std::optional<std::size_t> string_end() const;
};

/**
 * @brief A recursive-descent reader over one pattern in the regex crate's syntax, producing the node tree.
 *
 * The grammar is the crate's: an alternation of concatenations, a concatenation of repeated atoms, an atom a group,
 * a class, the dot, an escape or a scalar. The flags travel with the reader and are saved and restored around every
 * group, so that `(?i)` reaches to the end of the group it stands in and no further, as the crate scopes it. A
 * scalar under `i` becomes the class of its cases, which is what the crate's translation makes of it and what logos
 * therefore counts. A subpattern reference becomes a Reference node carrying the flags in force, resolved once the
 * whole pattern is read.
 */
class Pattern_reader
{
public:
    /**
     * @brief Binds the reader to a pattern.
     * @param literal The pattern's literal, whose content is read and whose text names it in refusals.
     * @param flags The flags in force at the start.
     * @param line The line, for refusals.
     */
    Pattern_reader(Literal literal, Flags flags, std::size_t line);

    /**
     * @brief Reads the whole content as a regex.
     * @return The tree.
     * @throws Spec_error If the pattern is refused.
     */
    [[nodiscard]] Node read();

    /**
     * @brief Reads the whole content as a literal, every scalar itself, which a `#[token]` is.
     * @return The tree.
     * @throws Spec_error If a non-ASCII scalar stands under `i`.
     */
    [[nodiscard]] Node read_literal();

private:
    /**
     * @brief An alternation: concatenations separated by `|`.
     * @return The node.
     */
    [[nodiscard]] Node alternation();

    /**
     * @brief A concatenation: repetitions up to a `|`, a `)` or the end.
     * @return The node.
     */
    [[nodiscard]] Node concatenation();

    /**
     * @brief An atom under its postfix operators, a lazy marker after one accepted and ignored.
     * @return The node.
     */
    [[nodiscard]] Node repetition();

    /**
     * @brief One atom: a group, a class, the dot, an escape or a scalar.
     * @return The node.
     * @throws Spec_error For an anchor, an operator with nothing before it, or a construct the rewriting refuses.
     */
    [[nodiscard]] Node atom();

    /**
     * @brief A group after its `(`: a capture, a non-capturing group, a flag setting, a flagged group, or a
     *        subpattern reference.
     * @return The node, Empty for a flag setting.
     * @throws Spec_error For lookaround, a flag the rewriting refuses, or an unclosed group.
     */
    [[nodiscard]] Node group();

    /**
     * @brief The content of a capture group, closed by its `)`, its outermost literal runs marked as bounded.
     * @return The node.
     * @throws Spec_error If the group is left open.
     */
    [[nodiscard]] Node captured();

    /**
     * @brief A class after its `[`, through its `]`, folded and negated as the flags and its `^` say.
     * @return The node.
     */
    [[nodiscard]] Node bracket();

    /**
     * @brief The members of a class after its `[` and optional `^`, through its `]`, ranges, escapes, POSIX
     *        classes and nested classes among them.
     * @return The members, before folding and negation.
     * @throws Spec_error For a class operator, a range ending in a class, or a range ending before it starts.
     */
    [[nodiscard]] Scalar_set members();

    /**
     * @brief An ASCII class after its `[:`, through its `:]`, negated when it opens with `^`.
     * @return The members.
     * @throws Spec_error If the name is not one of the crate's.
     */
    [[nodiscard]] Scalar_set posix_class();

    /**
     * @brief An escape after its backslash: a unit, or a class for `\d`, `\s`, `\w` and their negations.
     * @return The unit or the class.
     * @throws Spec_error For an anchor, a Unicode class, a Perl class in Unicode mode, or an escape the crate does
     *         not have.
     */
    [[nodiscard]] std::variant<Unit, Scalar_set> escape();

    /**
     * @brief A hex escape after its `\x`, `\u` or `\U`: the fixed number of digits, or any number in braces; a
     *        byte when it is the two-digit `\xHH` outside Unicode mode, a scalar otherwise.
     * @param kind The letter, which fixes the number of digits.
     * @return The unit.
     * @throws Spec_error If the digits are missing or the value is no scalar.
     */
    [[nodiscard]] Unit hex_escape(char kind);

    /**
     * @brief A unit as a member of a class: its value, which outside Unicode mode must be a byte or ASCII, since a
     *        byte class cannot hold a scalar's encoding.
     * @param unit The unit.
     * @return The member.
     * @throws Spec_error For a non-ASCII scalar in a class outside Unicode mode.
     */
    [[nodiscard]] char32_t member(Unit unit);

    /**
     * @brief A counted repetition after its `{`, through its `}`.
     * @return The minimum and, when bounded, the maximum.
     * @throws Spec_error If the count is malformed or reversed.
     */
    [[nodiscard]] std::pair<std::size_t, std::optional<std::size_t>> count();

    /**
     * @brief The unsigned decimal at the cursor.
     * @return The number, or std::nullopt when no digit stands here.
     * @throws Spec_error If the number does not fit.
     */
    [[nodiscard]] std::optional<std::size_t> number();

    /**
     * @brief The node for one literal unit under the flags: its bytes, or the class of its cases when it is a letter
     *        under `i`.
     * @param unit The unit.
     * @return The node.
     * @throws Spec_error For a non-ASCII scalar under `i` in Unicode mode, whose folding is not modelled.
     */
    [[nodiscard]] Node unit_node(Unit unit);

    /**
     * @brief The node for a class under the flags: folded under `i`, complemented when negated, and the literal it
     *        is when one member remains.
     * @param members The members.
     * @param negated Whether the class is complemented, after folding, as the crate orders it.
     * @return The node.
     * @throws Spec_error If the class ends up empty, or folding it needs the Unicode tables.
     */
    [[nodiscard]] Node class_node(Scalar_set members, bool negated);

    /**
     * @brief A class's members folded under `i`, and themselves otherwise; what every bracket, nested ones and the
     *        ASCII classes included, is complemented from, since the crate folds before it negates.
     * @param members The members.
     * @return The members under the flags.
     */
    [[nodiscard]] Scalar_set cased(const Scalar_set& members) const;

    /**
     * @brief The set every class is complemented against: the scalars less the surrogates, or the bytes.
     * @return The universe.
     */
    [[nodiscard]] Scalar_set universe() const;

    /**
     * @brief Consumes the unit at the cursor: a scalar decoded from UTF-8, or a byte when the literal is a byte
     *        string.
     * @return The unit.
     * @throws Spec_error At the end.
     */
    [[nodiscard]] Unit next_unit();

    /**
     * @brief The byte at the cursor, or nothing at the end.
     * @return The byte.
     */
    [[nodiscard]] std::optional<char> peek() const noexcept;

    /**
     * @brief Whether the text at the cursor begins with the given characters.
     * @param prefix The characters.
     * @return True when it does.
     */
    [[nodiscard]] bool at(std::string_view prefix) const noexcept;

    /**
     * @brief Consumes the byte at the cursor if it is the one given.
     * @param byte The byte.
     * @return True when consumed.
     */
    [[nodiscard]] bool accept(char byte) noexcept;

    /**
     * @brief Consumes the byte given or refuses, naming what the syntax expected.
     * @param byte The byte.
     * @param what What the syntax expected.
     * @throws Spec_error If another byte, or the end, stands here.
     */
    void expect(char byte, std::string_view what);

    /**
     * @brief Consumes and returns the byte at the cursor.
     * @param what What the syntax expected, named when the pattern has ended.
     * @return The byte.
     * @throws Spec_error At the end of the pattern.
     */
    char next(std::string_view what);

    /**
     * @brief Refuses the pattern, naming it and the line.
     * @param message Why.
     */
    [[noreturn]] void fail(const std::string& message) const;

    /**
     * @brief The literal being read.
     */
    Literal literal_;

    /**
     * @brief The flags in force.
     */
    Flags flags_;

    /**
     * @brief The line, for refusals.
     */
    std::size_t line_;

    /**
     * @brief The offset of the next byte of the content.
     */
    std::size_t at_{0};
};

/**
 * @brief Whether a byte is a decimal digit.
 * @param byte The byte.
 * @return True for 0 to 9.
 */
[[nodiscard]] constexpr bool is_digit(const char byte) noexcept
{
    return byte >= '0' && byte <= '9';
}

/**
 * @brief Whether a byte can begin or continue a Rust word: a letter, a digit, an underscore or a non-ASCII byte.
 * @param byte The byte.
 * @return True when it can.
 */
[[nodiscard]] constexpr bool is_word_byte(const char byte) noexcept
{
    return is_digit(byte) || is_letter(static_cast<unsigned char>(byte)) || byte == '_' ||
           static_cast<unsigned char>(byte) >= 0x80;
}

/**
 * @brief Whether a byte is a blank.
 * @param byte The byte.
 * @return True for a space, a tab, a newline or a carriage return.
 */
[[nodiscard]] constexpr bool is_blank(const char byte) noexcept
{
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/**
 * @brief The value of a hexadecimal digit.
 * @param byte The digit.
 * @return Its value.
 */
[[nodiscard]] constexpr unsigned hex_value(const char byte) noexcept
{
    return static_cast<unsigned>(is_digit(byte) ? byte - '0' : (byte | 0x20) - 'a' + 10);
}

/**
 * @brief The number of scalars a run of bytes encodes, when it is well-formed UTF-8.
 * @param bytes The run.
 * @return The count, or std::nullopt when the run is not UTF-8.
 */
[[nodiscard]] std::optional<std::size_t> scalar_count(const std::string_view bytes) noexcept
{
    std::size_t count{0};

    for (std::size_t at{0}; at < bytes.size(); ++count)
    {
        const auto lead{static_cast<unsigned char>(bytes[at])};

        const auto length{
                lead < 0x80                  ? 1UZ :
                lead >= 0xC2 && lead < 0xE0  ? 2UZ :
                lead >= 0xE0 && lead < 0xF0  ? 3UZ :
                lead >= 0xF0 && lead <= 0xF4 ? 4UZ :
                                               0UZ};

        if (length == 0 || at + length > bytes.size())
        {
            return std::nullopt;
        }

        for (std::size_t inner{1}; inner < length; ++inner)
        {
            if ((static_cast<unsigned char>(bytes[at + inner]) & 0xC0U) != 0x80U)
            {
                return std::nullopt;
            }
        }

        at += length;
    }

    return count;
}

/**
 * @brief The set of a class under `i`, every ASCII letter joined by its other case and, over scalars, `k` and `s`
 *        by the Kelvin sign and the long s, the crate's simple case folding on what the reader admits.
 * @param set The members.
 * @param unicode Whether the members are scalars.
 * @return The folded set.
 */
[[nodiscard]] Scalar_set folded(const Scalar_set& set, const bool unicode)
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
 * @brief A class in the syntax regex::parse() reads: over bytes, a bracket of its byte ranges; over scalars, a
 *        bracket of its ASCII members and its other ranges as code point escapes, which the parser reads as the
 *        encodings of the scalars they span.
 * @param cls The class.
 * @return The bracket.
 */
[[nodiscard]] std::string written(const Class& cls)
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
                else if constexpr (std::is_same_v<Kind, Class>)
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
                else if constexpr (std::is_same_v<Kind, Choice>)
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
 * @brief The priority logos computes for a node: two per scalar of a literal, two per byte when the run is not
 *        UTF-8; two for a class; the sum over a concatenation; the least over an alternation; a repetition's
 *        operand its minimum number of times; a reference its subpattern's.
 * @param node The node, its references resolved.
 * @return The priority.
 */
[[nodiscard]] std::size_t priority(const Node& node)
{
    return std::visit(
            []<typename Kind>(const Kind& kind) -> std::size_t {
                if constexpr (std::is_same_v<Kind, Empty>)
                {
                    return 0;
                }
                else if constexpr (std::is_same_v<Kind, Bytes>)
                {
                    return 2 * scalar_count(kind.bytes).value_or(kind.bytes.size());
                }
                else if constexpr (std::is_same_v<Kind, Class>)
                {
                    return 2;
                }
                else if constexpr (std::is_same_v<Kind, Reference>)
                {
                    return kind.priority;
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
                else if constexpr (std::is_same_v<Kind, Choice>)
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

/**
 * @brief A concatenation of parts, flattened, the empty parts dropped and adjacent literal runs merged unless a
 *        capture bounds one, which is how the crate builds one.
 * @param parts The parts.
 * @return The node: Empty for none, the part for one, a Concat otherwise.
 */
[[nodiscard]] Node concatenated(std::vector<Node> parts)
{
    std::vector<Node> flat;

    for (auto& part : parts)
    {
        if (std::holds_alternative<Concat>(part.kind))
        {
            std::ranges::move(std::get<Concat>(part.kind).parts, std::back_inserter(flat));
        }
        else if (!std::holds_alternative<Empty>(part.kind))
        {
            flat.push_back(std::move(part));
        }
    }

    std::vector<Node> merged;

    for (auto& part : flat)
    {
        const auto joins{
                !merged.empty() && std::holds_alternative<Bytes>(merged.back().kind) &&
                std::holds_alternative<Bytes>(part.kind) && !std::get<Bytes>(merged.back().kind).bounded &&
                !std::get<Bytes>(part.kind).bounded};

        if (joins)
        {
            std::get<Bytes>(merged.back().kind).bytes += std::get<Bytes>(part.kind).bytes;
        }
        else
        {
            merged.push_back(std::move(part));
        }
    }

    if (merged.empty())
    {
        return {.kind = Empty{}};
    }

    if (merged.size() == 1)
    {
        return std::move(merged.front());
    }

    return {.kind = Concat{.parts = std::move(merged)}};
}

/**
 * @brief An alternation of branches, flattened, an empty branch making the rest optional, which says the same.
 * @param branches The branches.
 * @return The node: Empty for none, the branch for one, a Choice otherwise, under `?` when a branch was empty.
 */
[[nodiscard]] Node alternated(std::vector<Node> branches)
{
    std::vector<Node> flat;

    auto nullable{false};

    for (auto& branch : branches)
    {
        if (std::holds_alternative<Choice>(branch.kind))
        {
            std::ranges::move(std::get<Choice>(branch.kind).branches, std::back_inserter(flat));
        }
        else if (std::holds_alternative<Empty>(branch.kind))
        {
            nullable = true;
        }
        else
        {
            flat.push_back(std::move(branch));
        }
    }

    if (flat.empty())
    {
        return {.kind = Empty{}};
    }

    Node node{flat.size() == 1 ? std::move(flat.front()) : Node{.kind = Choice{.branches = std::move(flat)}}};

    if (!nullable)
    {
        return node;
    }

    return {.kind = Repeat{.operand = regex::Indirect<Node>{std::move(node)}, .min = 0, .max = 1}};
}

/**
 * @brief A repetition of an operand, the forms that repeat nothing reduced.
 * @param operand The operand.
 * @param min The least number of times.
 * @param max The most, unbounded when std::nullopt.
 * @return The node: Empty when the operand is or the maximum is zero, the operand for exactly once, a Repeat else.
 */
[[nodiscard]] Node repeated(Node operand, const std::size_t min, const std::optional<std::size_t> max)
{
    if (std::holds_alternative<Empty>(operand.kind) || max == 0)
    {
        return {.kind = Empty{}};
    }

    if (min == 1 && max == 1)
    {
        return operand;
    }

    return {.kind = Repeat{.operand = regex::Indirect<Node>{std::move(operand)}, .min = min, .max = max}};
}

/**
 * @brief Resolves every subpattern reference in a tree: one under the default flags keeps its name and takes the
 *        subpattern's priority, one under `i` or `s` is replaced by the subpattern read again under those flags,
 *        as logos's textual substitution lets the flags reach in.
 * @param node The tree.
 * @param subpatterns The subpatterns declared.
 * @param line The line, for refusals.
 * @throws Spec_error If a reference names no subpattern declared before it.
 */
void resolve(Node& node, const Subpatterns_t& subpatterns, const std::size_t line)
{
    if (std::holds_alternative<Reference>(node.kind))
    {
        auto& [name, reference_flags, reference_priority]{std::get<Reference>(node.kind)};

        const auto found{subpatterns.find(name)};

        if (found == subpatterns.end())
        {
            throw Spec_error{"the subpattern '" + name + "' is not declared before its use", line};
        }

        const auto& subpattern{found->second};

        if (!reference_flags.insensitive && !reference_flags.dot_all)
        {
            reference_priority = subpattern.priority;

            return;
        }

        const Flags flags{
                .insensitive = reference_flags.insensitive,
                .dot_all = reference_flags.dot_all,
                .unicode = !subpattern.literal.byte_string};

        // The reference is gone once the node is replaced; nothing of it is read after this.
        node = Pattern_reader{subpattern.literal, flags, line}.read();

        resolve(node, subpatterns, line);

        return;
    }

    std::visit(
            [&]<typename Kind>(Kind& kind) {
                if constexpr (std::is_same_v<Kind, Concat>)
                {
                    for (auto& part : kind.parts)
                    {
                        resolve(part, subpatterns, line);
                    }
                }
                else if constexpr (std::is_same_v<Kind, Choice>)
                {
                    for (auto& branch : kind.branches)
                    {
                        resolve(branch, subpatterns, line);
                    }
                }
                else if constexpr (std::is_same_v<Kind, Repeat>)
                {
                    resolve(*kind.operand, subpatterns, line);
                }
            },
            node.kind);
}

/**
 * @brief Compiles a pattern: read, its references resolved, written for the parser, and given its priority.
 * @param literal The pattern's literal.
 * @param token Whether it is a `#[token]`, matched as it stands, rather than a regex.
 * @param insensitive Whether `ignore(case)` was given.
 * @param subpatterns The subpatterns declared.
 * @param line The line, for refusals.
 * @return The expression and the priority logos computes, twice a token's byte length or a regex's from its tree,
 *         which a `priority = n` on the attribute overrides.
 * @throws Spec_error If the pattern is refused, or matches only the empty string, which logos refuses too.
 */
[[nodiscard]] Compiled compile(
        const Literal& literal, const bool token, const bool insensitive, const Subpatterns_t& subpatterns,
        const std::size_t line)
{
    const Flags flags{.insensitive = insensitive, .dot_all = false, .unicode = !literal.byte_string};

    Pattern_reader reader{literal, flags, line};

    auto node{token ? reader.read_literal() : reader.read()};

    resolve(node, subpatterns, line);

    if (std::holds_alternative<Empty>(node.kind))
    {
        throw Spec_error{
                "the pattern " + literal.written + " matches only the empty string, which logos refuses", line};
    }

    return {.expression = written(node, true), .priority = token ? 2 * literal.bytes.size() : priority(node)};
}

/**
 * @brief Whether a callback skips the match by its spelling alone: `logos::skip` or `skip` as a path, or a closure
 *        whose whole body is `logos::Skip`.
 * @param callback The callback's text.
 * @return True when it does.
 */
[[nodiscard]] bool skips(const std::string_view callback)
{
    std::string text;

    std::ranges::copy_if(callback, std::back_inserter(text), [](const char byte) { return !is_blank(byte); });

    if (text.starts_with('|'))
    {
        text = text.substr(std::min(text.find('|', 1) + 1, text.size()));

        if (text.starts_with('{') && text.ends_with('}'))
        {
            text = text.substr(1, text.size() - 2);
        }

        return text == "Skip" || text == "logos::Skip" || text == "::logos::Skip";
    }

    return text == "skip" || text == "logos::skip" || text == "::logos::skip";
}

/**
 * @brief The unsigned decimal a `priority = n` gives.
 * @param text The value's text.
 * @param cursor The cursor, for the refusal's line.
 * @return The number.
 * @throws Spec_error If the text is not a decimal that fits.
 */
[[nodiscard]] std::size_t unsigned_value(const std::string_view text, const Rust_cursor& cursor)
{
    std::size_t value{0};

    for (const auto byte : text)
    {
        const auto digit{static_cast<std::size_t>(byte - '0')};

        if (!is_digit(byte) || value > (std::numeric_limits<std::size_t>::max() - digit) / 10)
        {
            cursor.fail("priority expects an unsigned integer, got '" + std::string{text} + "'");
        }

        value = value * 10 + digit;
    }

    if (text.empty())
    {
        cursor.fail("priority expects an unsigned integer");
    }

    return value;
}

/**
 * @brief Reads the attribute opening at the cursor, `#[path]`, `#[path(...)]` or `#[path = ...]`.
 * @param cursor The cursor, at the `#`.
 * @return The attribute.
 * @throws Spec_error If the attribute is left open.
 */
[[nodiscard]] Attribute read_attribute(Rust_cursor& cursor)
{
    const auto line{cursor.line()};

    cursor.expect('#', "'#'");

    cursor.expect('[', "'[' to open the attribute");

    cursor.skip_trivia();

    std::string path{cursor.word()};

    for (cursor.skip_trivia(); cursor.at("::"); cursor.skip_trivia())
    {
        cursor.expect(':', "':'");
        cursor.expect(':', "':'");

        cursor.skip_trivia();

        path += "::" + std::string{cursor.word()};
    }

    if (path.empty())
    {
        cursor.fail("expected the attribute's name after '#['");
    }

    auto begin{cursor.offset()};

    auto end{begin};

    if (cursor.peek() == '(' || cursor.peek() == '[' || cursor.peek() == '{')
    {
        begin = cursor.offset() + 1;

        cursor.skip_group();

        end = cursor.offset() - 1;
    }
    else
    {
        while (!cursor.done() && cursor.peek() != ']')
        {
            cursor.skip_token();
        }
    }

    cursor.skip_trivia();

    cursor.expect(']', "']' to close the attribute");

    return {.path = std::move(path), .begin = begin, .end = end, .line = line};
}

/**
 * @brief Reads the content of a `#[token(...)]`, a `#[regex(...)]` or a `skip(...)`: the literal, then a callback
 *        in first position or as `callback = ...`, `priority = n`, `ignore(case)` and `allow_greedy = ...`, comma
 *        separated, as logos reads them.
 * @param content A cursor over the content.
 * @return The definition.
 * @throws Spec_error If the literal is missing, an argument is one logos does not know or stands where logos
 *         refuses it, or `ignore` names a flag other than `case`.
 */
[[nodiscard]] Definition read_definition(Rust_cursor content)
{
    const auto line{content.line()};

    content.skip_trivia();

    Definition definition{
            .literal = content.literal(),
            .callback = {},
            .priority = std::nullopt,
            .insensitive = false,
            .line = line};

    for (std::size_t position{0};; ++position)
    {
        content.skip_trivia();

        if (content.done())
        {
            return definition;
        }

        content.expect(',', "',' between the attribute's arguments");

        content.skip_trivia();

        if (content.done())
        {
            return definition;
        }

        // One argument runs to the next comma outside any group, string or closure body.
        const auto begin{content.offset()};

        while (!content.done() && content.peek() != ',')
        {
            content.skip_token();
        }

        const auto end{content.offset()};

        // The text from an offset to the argument's end, its trailing blanks dropped.
        const auto rest{[&content, end](const std::size_t from) {
            auto text{content.slice(from, end)};

            while (!text.empty() && is_blank(text.back()))
            {
                text.remove_suffix(1);
            }

            return std::string{text};
        }};

        auto item{content.inside(begin, end)};

        const std::string key{item.word()};

        item.skip_trivia();

        if (!key.empty() && item.peek() == '=' && !item.at("=="))
        {
            item.expect('=', "'='");

            item.skip_trivia();

            if (key == "priority")
            {
                definition.priority = unsigned_value(rest(item.offset()), item);
            }
            else if (key == "callback")
            {
                definition.callback = rest(item.offset());
            }
            else if (key != "allow_greedy")
            {
                item.fail("logos knows no argument '" + key + "'; expected callback, priority, ignore or allow_greedy");
            }
        }
        else if (key == "ignore" && item.peek() == '(')
        {
            auto flags{item.inside(item.offset() + 1, end)};

            for (flags.skip_trivia(); !flags.done() && flags.peek() != ')'; flags.skip_trivia())
            {
                const auto flag{flags.word()};

                if (flag == "ascii_case")
                {
                    flags.fail(
                            "ignore(ascii_case) is no longer accepted by logos, and its ASCII-only folding is not "
                            "modelled");
                }

                if (flag != "case")
                {
                    flags.fail("ignore knows no flag '" + std::string{flag} + "'; expected case");
                }

                definition.insensitive = true;

                flags.skip_trivia();

                if (!flags.accept(','))
                {
                    break;
                }
            }
        }
        else if (position == 0)
        {
            definition.callback = rest(begin);
        }
        else
        {
            item.fail(
                    "expected a named argument at this position; a callback after the first argument is written "
                    "callback = ...");
        }
    }
}

/**
 * @brief Reads the content of one `#[logos(...)]`: its skips into the definitions, its subpatterns compiled in
 *        order into the specification's definitions and the table, and every other key into the options.
 * @param content A cursor over the content.
 * @param spec The specification being filled.
 * @param subpatterns The subpatterns declared so far, added to.
 * @param skips The skip definitions collected so far, added to.
 * @throws Spec_error If an entry is malformed, a subpattern is declared twice or its name would read as a count.
 */
void read_logos_attribute(
        Rust_cursor content, Lexer_spec& spec, Subpatterns_t& subpatterns, std::vector<Definition>& skips)
{
    for (content.skip_trivia(); !content.done(); content.skip_trivia())
    {
        const auto begin{content.offset()};

        const auto line{content.line()};

        const std::string key{content.word()};

        if (key.empty())
        {
            content.fail("expected a key in #[logos(...)]");
        }

        content.skip_trivia();

        if (key == "skip" && content.peek() == '(')
        {
            const auto open{content.offset()};

            content.skip_group();

            skips.push_back(read_definition(content.inside(open + 1, content.offset() - 1)));
        }
        else if (key == "skip")
        {
            skips.push_back(
                    {.literal = content.literal(),
                     .callback = {},
                     .priority = std::nullopt,
                     .insensitive = false,
                     .line = line});
        }
        else if (key == "subpattern")
        {
            const std::string name{content.word()};

            content.skip_trivia();

            content.expect('=', "'=' after the subpattern's name");

            content.skip_trivia();

            if (name.empty() || is_digit(name.front()))
            {
                content.fail(
                        "a subpattern needs a name that opens with a letter or an underscore, since {" + name +
                        "} would read as a count");
            }

            if (subpatterns.contains(name))
            {
                content.fail("the subpattern '" + name + "' is declared twice");
            }

            auto literal{content.literal()};

            const auto [expression, priority]{compile(literal, false, false, subpatterns, line)};

            spec.definitions.insert_or_assign(name, expression);

            subpatterns.insert_or_assign(name, Subpattern{.literal = std::move(literal), .priority = priority});
        }

        while (!content.done() && content.peek() != ',')
        {
            content.skip_token();
        }

        if (key != "skip" && key != "subpattern")
        {
            // The key, then the rest without its blanks, `extras=Extras`, `error(E,callback=f)`, `type S=&str`.
            std::string rest;

            std::ranges::copy_if(
                    content.slice(begin + key.size(), content.offset()), std::back_inserter(rest),
                    [](const char byte) { return !is_blank(byte); });

            spec.options.push_back(key + (rest.starts_with('=') || rest.starts_with('(') ? "" : " ") + rest);
        }

        if (!content.done())
        {
            content.expect(',', "',' between the entries of #[logos(...)]");
        }
    }
}

/**
 * @brief Adds one rule to a specification.
 * @param spec The specification.
 * @param definition The attribute's definition.
 * @param token Whether the definition is a `#[token]`, matched as it stands.
 * @param variant The variant's name, or std::nullopt for a skip.
 * @param subpatterns The subpatterns declared.
 * @throws Spec_error If the pattern is refused.
 */
void add_rule(
        Lexer_spec& spec, const Definition& definition, const bool token, const std::optional<std::string>& variant,
        const Subpatterns_t& subpatterns)
{
    const auto [expression, computed]{
            compile(definition.literal, token, definition.insensitive, subpatterns, definition.line)};

    const auto discarded{!variant || skips(definition.callback)};

    spec.rules.push_back(
            {.pattern = definition.literal.written,
             .expression = expression,
             .conditions = {},
             .action = definition.callback,
             .token = discarded ? std::nullopt : variant,
             .priority = definition.priority.value_or(computed),
             .line = definition.line});
}

/**
 * @brief Reads the enum after its `enum` keyword into a specification: its `#[logos]` attributes, then its
 *        variants with their `#[token]` and `#[regex]` attributes.
 * @param cursor The cursor, just past `enum`.
 * @param attributes The enum's outer attributes.
 * @param line The line of the derive naming Logos.
 * @return The specification.
 * @throws Spec_error If the enum is malformed or left open, or an attribute or pattern is refused.
 */
[[nodiscard]] Lexer_spec read_enum(
        Rust_cursor& cursor, const std::vector<Attribute>& attributes, const std::size_t line)
{
    Lexer_spec spec;

    spec.line = line;

    cursor.skip_trivia();

    if (cursor.word().empty())
    {
        cursor.fail("expected the enum's name");
    }

    while (!cursor.done() && cursor.peek() != '{')
    {
        cursor.skip_token();
    }

    cursor.expect('{', "'{' to open the enum");

    Subpatterns_t subpatterns;

    std::vector<Definition> skips;

    for (const auto& attribute : attributes)
    {
        if (attribute.path == "logos")
        {
            read_logos_attribute(cursor.inside(attribute.begin, attribute.end), spec, subpatterns, skips);
        }
    }

    for (const auto& skip : skips)
    {
        add_rule(spec, skip, false, std::nullopt, subpatterns);
    }

    for (cursor.skip_trivia(); !cursor.accept('}'); cursor.skip_trivia())
    {
        if (cursor.done())
        {
            cursor.fail("the enum is never closed");
        }

        std::vector<std::pair<Definition, bool>> definitions;

        while (cursor.at("#["))
        {
            const auto attribute{read_attribute(cursor)};

            if (attribute.path == "token" || attribute.path == "regex")
            {
                definitions.emplace_back(
                        read_definition(cursor.inside(attribute.begin, attribute.end)), attribute.path == "token");
            }

            cursor.skip_trivia();
        }

        const std::string variant{cursor.word()};

        if (variant.empty())
        {
            cursor.fail("expected a variant's name");
        }

        cursor.skip_trivia();

        if (cursor.peek() == '(' || cursor.peek() == '{')
        {
            cursor.skip_group();

            cursor.skip_trivia();
        }

        if (cursor.accept('='))
        {
            while (!cursor.done() && cursor.peek() != ',' && cursor.peek() != '}')
            {
                cursor.skip_token();
            }
        }

        for (const auto& [definition, token] : definitions)
        {
            add_rule(spec, definition, token, variant, subpatterns);
        }

        cursor.skip_trivia();

        if (cursor.peek() != '}')
        {
            cursor.expect(',', "',' or '}' after the variant '" + variant + "'");
        }
    }

    return spec;
}

/**
 * @brief The line of the attribute among a list of outer attributes whose derive names Logos, bare or by path.
 * @param attributes The attributes.
 * @param cursor The cursor over the file the offsets index.
 * @return The line, or std::nullopt when none does.
 */
[[nodiscard]] std::optional<std::size_t> derives_logos(
        const std::vector<Attribute>& attributes, const Rust_cursor& cursor)
{
    for (const auto& attribute : attributes)
    {
        if (attribute.path != "derive")
        {
            continue;
        }

        auto list{cursor.inside(attribute.begin, attribute.end)};

        for (list.skip_trivia(); !list.done(); list.skip_trivia())
        {
            std::string name{list.word()};

            for (list.skip_trivia(); list.at("::"); list.skip_trivia())
            {
                list.expect(':', "':'");
                list.expect(':', "':'");

                list.skip_trivia();

                name = std::string{list.word()};
            }

            if (name == "Logos")
            {
                return attribute.line;
            }

            if (!list.done())
            {
                list.expect(',', "',' between the derives");
            }
        }
    }

    return std::nullopt;
}

void Scalar_set::add(const char32_t low, const char32_t high)
{
    ranges_.emplace_back(low, high);

    std::ranges::sort(ranges_);

    std::vector<Range_t> merged;

    for (const auto& [from, to] : ranges_)
    {
        if (merged.empty() || from > merged.back().second + 1)
        {
            merged.emplace_back(from, to);

            continue;
        }

        auto& [low, high]{merged.back()};

        high = std::max(high, to);
    }

    ranges_ = std::move(merged);
}

void Scalar_set::add(const Scalar_set& other)
{
    for (const auto& [low, high] : other.ranges_)
    {
        add(low, high);
    }
}

Scalar_set Scalar_set::minus(const Scalar_set& other) const
{
    Scalar_set difference;

    for (const auto& [low, high] : ranges_)
    {
        auto from{low};

        for (const auto& [cut_low, cut_high] : other.ranges_)
        {
            if (cut_high < from)
            {
                continue;
            }

            if (cut_low > high)
            {
                break;
            }

            if (cut_low > from)
            {
                difference.add(from, cut_low - 1);
            }

            if (cut_high >= high)
            {
                from = high + 1;

                break;
            }

            from = std::max(from, static_cast<char32_t>(cut_high + 1));
        }

        if (from <= high)
        {
            difference.add(from, high);
        }
    }

    return difference;
}

const std::vector<Scalar_set::Range_t>& Scalar_set::ranges() const noexcept
{
    return ranges_;
}

bool Scalar_set::contains(const char32_t value) const noexcept
{
    return std::ranges::any_of(ranges_, [value](const Range_t& range) {
        const auto& [low, high]{range};

        return low <= value && value <= high;
    });
}

bool Scalar_set::empty() const noexcept
{
    return ranges_.empty();
}

std::optional<char32_t> Scalar_set::single() const noexcept
{
    if (ranges_.size() != 1)
    {
        return std::nullopt;
    }

    const auto& [low, high]{ranges_.front()};

    return low == high ? std::optional{low} : std::nullopt;
}

Rust_cursor::Rust_cursor(const std::string_view text, const std::size_t begin, const std::size_t end)
    : Cursor{text, begin, end}
{}

void Rust_cursor::skip_trivia()
{
    for (;;)
    {
        while (peek() && is_blank(*peek()))
        {
            ++at_;
        }

        if (at("//"))
        {
            while (peek() && *peek() != '\n')
            {
                ++at_;
            }
        }
        else if (at("/*"))
        {
            const auto line{this->line()};

            // Rust nests block comments.
            std::size_t depth{0};

            do
            {
                if (at_ >= end_)
                {
                    throw Spec_error{"a block comment is never closed", line};
                }

                if (at("/*"))
                {
                    ++depth;

                    at_ += 2;
                }
                else if (at("*/"))
                {
                    --depth;

                    at_ += 2;
                }
                else
                {
                    ++at_;
                }
            } while (depth > 0);
        }
        else
        {
            return;
        }
    }
}

void Rust_cursor::skip_token()
{
    if (at("//") || at("/*"))
    {
        skip_trivia();

        return;
    }

    if (const auto end{string_end()})
    {
        at_ = *end;

        return;
    }

    if (at("'") || at("b'"))
    {
        // A character literal closes after one scalar or one escape; a lifetime or label does not close at all.
        const auto open{at_ + (at("b'") ? 2 : 1)};

        auto close{open};

        if (close < end_ && text_[close] == '\\')
        {
            close = std::min(text_.find('\'', close + 2), end_);
        }
        else if (close < end_)
        {
            const auto lead{static_cast<unsigned char>(text_[close])};

            close += lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        }

        at_ = close < end_ && text_[close] == '\'' ? close + 1 : open;

        return;
    }

    if (peek() == '(' || peek() == '[' || peek() == '{')
    {
        skip_group();

        return;
    }

    if (word().empty())
    {
        ++at_;
    }
}

void Rust_cursor::skip_group()
{
    const auto open{next("a group")};

    const auto close{open == '(' ? ')' : open == '[' ? ']' : '}'};

    const auto line{this->line()};

    for (skip_trivia(); !accept(close); skip_trivia())
    {
        if (done())
        {
            throw Spec_error{std::string{"the '"} + open + "' is never closed", line};
        }

        if (peek() == ')' || peek() == ']' || peek() == '}')
        {
            fail(std::string{"'"} + *peek() + "' closes nothing, '" + close + "' was expected");
        }

        skip_token();
    }
}

std::string_view Rust_cursor::word()
{
    const auto begin{at_};

    if (at("r#") && at_ + 2 < end_ && is_word_byte(text_[at_ + 2]))
    {
        at_ += 2;
    }

    while (peek() && is_word_byte(*peek()))
    {
        ++at_;
    }

    return text_.substr(begin, at_ - begin);
}

Literal Rust_cursor::literal()
{
    const auto begin{at_};

    const auto end{string_end()};

    if (!end)
    {
        fail("expected a string literal");
    }

    if (peek() == 'c')
    {
        fail("a C string is no pattern; logos takes a &str or a &[u8] literal");
    }

    const auto byte_string{accept('b')};

    const auto raw{accept('r')};

    std::size_t hashes{0};

    while (accept('#'))
    {
        ++hashes;
    }

    expect('"', R"('"')");

    const auto content_end{*end - hashes - 1};

    Literal literal{.written = std::string{slice(begin, *end)}, .bytes = {}, .byte_string = byte_string};

    if (raw)
    {
        literal.bytes = slice(at_, content_end);

        at_ = *end;

        return literal;
    }

    while (at_ < content_end)
    {
        const auto byte{next("the string's content")};

        if (byte != '\\')
        {
            literal.bytes.push_back(byte);

            continue;
        }

        switch (const auto escaped{next("the escape")}; escaped)
        {
        case 'n':
            literal.bytes.push_back('\n');
            break;
        case 'r':
            literal.bytes.push_back('\r');
            break;
        case 't':
            literal.bytes.push_back('\t');
            break;
        case '0':
            literal.bytes.push_back('\0');
            break;
        case '\\':
        case '\'':
        case '"':
            literal.bytes.push_back(escaped);
            break;
        case 'x':
        {
            const auto high{next("a hex digit")};

            const auto low{next("a hex digit")};

            if (!is_hex_digit(high) || !is_hex_digit(low))
            {
                fail(R"(\x needs two hex digits)");
            }

            const auto value{hex_value(high) * 16 + hex_value(low)};

            if (value > 0x7F && !byte_string)
            {
                fail(R"(\x in a string reaches only \x7f; a higher scalar is written \u{...})");
            }

            literal.bytes.push_back(static_cast<char>(value));

            break;
        }
        case 'u':
        {
            if (byte_string)
            {
                fail(R"(a byte string has no \u escape)");
            }

            expect('{', R"('{' after \u)");

            char32_t value{0};

            std::size_t digits{0};

            for (; peek() && is_hex_digit(*peek()); ++digits)
            {
                value = value * 16 + hex_value(next("a hex digit"));
            }

            expect('}', R"('}' to close the \u escape)");

            if (digits == 0 || digits > 6 || value > last_scalar || (value >= 0xD800 && value <= 0xDFFF))
            {
                fail(R"(\u{...} needs one to six hex digits naming a scalar)");
            }

            literal.bytes += encoded(value);

            break;
        }
        case '\n':
        case '\r':
            // A backslash ending the line continues the string on the next, its leading blanks dropped.
            while (peek() && is_blank(*peek()))
            {
                ++at_;
            }

            break;
        default:
            fail(std::string{R"('\)"} + escaped + "' is not an escape Rust knows");
        }
    }

    at_ = *end;

    return literal;
}

Rust_cursor Rust_cursor::inside(const std::size_t begin, const std::size_t end) const noexcept
{
    return {text_, begin, end};
}

std::string_view Rust_cursor::slice(const std::size_t begin, const std::size_t end) const noexcept
{
    return text_.substr(begin, end - begin);
}

bool Rust_cursor::at_string() const
{
    return string_end().has_value();
}

std::optional<std::size_t> Rust_cursor::string_end() const
{
    auto at{at_};

    // The prefixes: b for a byte string, r for a raw one, br for both; c and cr, the C strings, have no pattern
    // to give but are skipped like the rest.
    if (at < end_ && (text_[at] == 'b' || text_[at] == 'c'))
    {
        ++at;
    }

    const auto raw{at < end_ && text_[at] == 'r'};

    if (raw)
    {
        ++at;
    }

    std::size_t hashes{0};

    for (; raw && at < end_ && text_[at] == '#'; ++at)
    {
        ++hashes;
    }

    if (at >= end_ || text_[at] != '"')
    {
        return std::nullopt;
    }

    const auto line{this->line()};

    for (++at; at < end_; ++at)
    {
        if (text_[at] == '\\' && !raw)
        {
            ++at;
        }
        else if (text_[at] == '"' && at + hashes < end_ && text_.substr(at + 1, hashes) == std::string(hashes, '#'))
        {
            return at + 1 + hashes;
        }
    }

    throw Spec_error{"a string literal is never closed", line};
}

Pattern_reader::Pattern_reader(Literal literal, const Flags flags, const std::size_t line)
    : literal_{std::move(literal)}, flags_{flags}, line_{line}
{}

Node Pattern_reader::read()
{
    auto node{alternation()};

    if (peek())
    {
        fail(std::string{"unexpected '"} + *peek() + "'");
    }

    return node;
}

Node Pattern_reader::read_literal()
{
    std::vector<Node> parts;

    while (peek())
    {
        parts.push_back(unit_node(next_unit()));
    }

    return concatenated(std::move(parts));
}

Node Pattern_reader::alternation()
{
    std::vector<Node> branches;

    branches.push_back(concatenation());

    while (accept('|'))
    {
        branches.push_back(concatenation());
    }

    return alternated(std::move(branches));
}

Node Pattern_reader::concatenation()
{
    std::vector<Node> parts;

    while (peek() && *peek() != '|' && *peek() != ')')
    {
        parts.push_back(repetition());
    }

    return concatenated(std::move(parts));
}

Node Pattern_reader::repetition()
{
    auto node{atom()};

    for (;;)
    {
        std::size_t min{0};

        std::optional<std::size_t> max;

        if (accept('*'))
        {
            max = std::nullopt;
        }
        else if (accept('+'))
        {
            min = 1;
        }
        else if (accept('?'))
        {
            max = 1;
        }
        else if (accept('{'))
        {
            std::tie(min, max) = count();
        }
        else
        {
            return node;
        }

        // Only an empty group or a flag setting stands before the operator: the crate refuses the second and logos
        // has no use for the first, so both are refused rather than read as repeating nothing.
        if (std::holds_alternative<Empty>(node.kind))
        {
            fail("a repetition operator needs something before it to repeat");
        }

        // The lazy marker changes which match the crate reports, never which matches exist, and logos takes the
        // longest.
        std::ignore = accept('?');

        node = repeated(std::move(node), min, max);
    }
}

Node Pattern_reader::atom()
{
    const auto byte{next("a pattern")};

    switch (byte)
    {
    case '(':
        return group();
    case '[':
        return bracket();
    case '.':
    {
        Scalar_set newline;

        newline.add('\n', '\n');

        return {.kind =
                        Class{.set = flags_.dot_all ? universe() : universe().minus(newline),
                              .unicode = flags_.unicode}};
    }
    case '\\':
    {
        auto escaped{escape()};

        if (std::holds_alternative<Unit>(escaped))
        {
            return unit_node(std::get<Unit>(escaped));
        }

        return class_node(std::move(std::get<Scalar_set>(escaped)), false);
    }
    case '*':
    case '+':
    case '?':
    case '{':
        --at_;

        fail(std::string{"nothing for '"} + byte + "' to repeat");
    case ')':
        --at_;

        fail("unexpected ')'");
    case '^':
    case '$':
        --at_;

        fail(std::string{"the anchor '"} + byte +
             "' conditions the context a match stands in, which a token language cannot say");
    default:
        --at_;

        return unit_node(next_unit());
    }
}

Node Pattern_reader::group()
{
    const auto saved{flags_};

    if (!accept('?'))
    {
        auto node{captured()};

        flags_ = saved;

        return node;
    }

    if (accept('&'))
    {
        std::string name;

        while (peek() && *peek() != ')')
        {
            name.push_back(next("the subpattern's name"));
        }

        expect(')', "')' to close the subpattern reference");

        return {.kind = Reference{.name = std::move(name), .flags = flags_, .priority = 0}};
    }

    if (at("P<") || (peek() == '<' && !at("<=") && !at("<!")))
    {
        std::ignore = accept('P');

        while (peek() && *peek() != '>')
        {
            ++at_;
        }

        expect('>', "'>' to close the capture's name");

        auto node{captured()};

        flags_ = saved;

        return node;
    }

    if (at("=") || at("!") || at("<=") || at("<!"))
    {
        fail("lookaround conditions the context a match stands in, which a token language cannot say");
    }

    auto negated{false};

    auto dangling{false};

    auto flags{flags_};

    for (auto seen{false};; seen = true)
    {
        const auto flag{next("a flag, ':' or ')'")};

        if (flag == ':' || flag == ')')
        {
            // A colon with no flag before it opens the plain non-capturing group.
            if ((!seen && flag == ')') || dangling)
            {
                fail(!seen ? "a flag group needs a flag" : "a '-' in a flag group needs a flag after it");
            }

            // Bare, the flags hold for the rest of the enclosing group; with a colon, for the group they open.
            flags_ = flags;

            if (flag == ')')
            {
                return {.kind = Empty{}};
            }

            auto node{alternation()};

            expect(')', "')' to close the flagged group");

            flags_ = saved;

            return node;
        }

        dangling = flag == '-';

        switch (flag)
        {
        case '-':
            if (negated)
            {
                fail("a flag group takes one '-'");
            }

            negated = true;
            break;
        case 'i':
            flags.insensitive = !negated;
            break;
        case 's':
            flags.dot_all = !negated;
            break;
        case 'u':
            flags.unicode = !negated;
            break;
        case 'x':
        case 'm':
        case 'U':
        case 'R':
            fail(std::string{"the flag '"} + flag + "' is not modelled");
        default:
            fail(std::string{"'"} + flag + "' is not a flag of the regex crate");
        }
    }
}

Node Pattern_reader::captured()
{
    auto node{alternation()};

    expect(')', "')' to close the group");

    const auto bound{[](Node& edge) {
        if (std::holds_alternative<Bytes>(edge.kind))
        {
            std::get<Bytes>(edge.kind).bounded = true;
        }
    }};

    if (std::holds_alternative<Concat>(node.kind))
    {
        bound(std::get<Concat>(node.kind).parts.front());

        bound(std::get<Concat>(node.kind).parts.back());
    }
    else
    {
        bound(node);
    }

    return node;
}

Node Pattern_reader::bracket()
{
    const auto negated{accept('^')};

    return class_node(members(), negated);
}

Scalar_set Pattern_reader::members()
{
    Scalar_set members;

    // Dashes first are members, and a ']' first is one too, as the crate has it.
    auto first{true};

    while (accept('-'))
    {
        members.add('-', '-');

        first = false;
    }

    if (first && accept(']'))
    {
        members.add(']', ']');
    }

    for (;;)
    {
        if (!peek())
        {
            fail("expected ']' to close the class before the end of the pattern");
        }

        if (accept(']'))
        {
            return members;
        }

        if (at("&&") || at("--") || at("~~"))
        {
            fail("the class operator '" + literal_.bytes.substr(at_, 2) + "' is not modelled");
        }

        std::optional<char32_t> low;

        if (at("[:"))
        {
            at_ += 2;

            members.add(posix_class());
        }
        else if (accept('['))
        {
            const auto negated{accept('^')};

            const auto nested{this->members()};

            members.add(negated ? universe().minus(cased(nested)) : nested);
        }
        else if (accept('\\'))
        {
            auto escaped{escape()};

            if (std::holds_alternative<Unit>(escaped))
            {
                low = member(std::get<Unit>(escaped));
            }
            else
            {
                members.add(std::get<Scalar_set>(escaped));

                if (peek() == '-' && at_ + 1 < literal_.bytes.size() && literal_.bytes[at_ + 1] != ']' &&
                    literal_.bytes[at_ + 1] != '-')
                {
                    fail("a range cannot start with a class");
                }
            }
        }
        else
        {
            low = member(next_unit());
        }

        if (!low)
        {
            continue;
        }

        // A '-' after a member opens a range, unless the close or another '-' follows it.
        const auto after{at_ + 1 < literal_.bytes.size() ? literal_.bytes[at_ + 1] : ']'};

        if (peek() != '-' || after == ']' || after == '-')
        {
            members.add(*low, *low);

            continue;
        }

        ++at_;

        if (peek() == '[')
        {
            fail("a range cannot end in a class");
        }

        char32_t high{0};

        if (accept('\\'))
        {
            auto escaped{escape()};

            if (!std::holds_alternative<Unit>(escaped))
            {
                fail("a range cannot end in a class");
            }

            high = member(std::get<Unit>(escaped));
        }
        else
        {
            high = member(next_unit());
        }

        if (high < *low)
        {
            fail("the range ends before it starts");
        }

        members.add(*low, high);
    }
}

Scalar_set Pattern_reader::posix_class()
{
    const auto negated{accept('^')};

    std::string name;

    while (peek() && *peek() != ':')
    {
        name.push_back(next("a class name"));
    }

    expect(':', "':]' to close the class");
    expect(']', "':]' to close the class");

    Scalar_set set;

    const auto add_range{[&set](const char32_t low, const char32_t high) { set.add(low, high); }};

    if (name == "alnum" || name == "alpha" || name == "word" || name == "xdigit" || name == "upper" ||
        name == "lower" || name == "digit")
    {
        if (name != "lower" && name != "digit")
        {
            add_range('A', name == "xdigit" ? 'F' : 'Z');
        }

        if (name != "upper" && name != "digit")
        {
            add_range('a', name == "xdigit" ? 'f' : 'z');
        }

        if (name != "alpha" && name != "upper" && name != "lower")
        {
            add_range('0', '9');
        }

        if (name == "word")
        {
            add_range('_', '_');
        }
    }
    else if (name == "ascii")
    {
        add_range(0, 0x7F);
    }
    else if (name == "blank")
    {
        add_range(' ', ' ');
        add_range('\t', '\t');
    }
    else if (name == "cntrl")
    {
        add_range(0, 0x1F);
        add_range(0x7F, 0x7F);
    }
    else if (name == "graph" || name == "print" || name == "punct")
    {
        add_range(name == "print" ? ' ' : '!', '~');

        if (name == "punct")
        {
            set = set.minus([] {
                Scalar_set alphanumeric;

                alphanumeric.add('0', '9');
                alphanumeric.add('A', 'Z');
                alphanumeric.add('a', 'z');

                return alphanumeric;
            }());
        }
    }
    else if (name == "space")
    {
        add_range('\t', '\r');
        add_range(' ', ' ');
    }
    else
    {
        fail("'[:" + name + ":]' is not an ASCII class of the regex crate");
    }

    return negated ? universe().minus(cased(set)) : set;
}

std::variant<Unit, Scalar_set> Pattern_reader::escape()
{
    const auto byte{next("the escaped character")};

    switch (byte)
    {
    case 'n':
        return Unit{.value = U'\n', .byte = false};
    case 't':
        return Unit{.value = U'\t', .byte = false};
    case 'r':
        return Unit{.value = U'\r', .byte = false};
    case 'f':
        return Unit{.value = U'\f', .byte = false};
    case 'v':
        return Unit{.value = U'\v', .byte = false};
    case 'a':
        return Unit{.value = U'\a', .byte = false};
    case 'x':
    case 'u':
    case 'U':
        return hex_escape(byte);
    case 'd':
    case 's':
    case 'w':
    case 'D':
    case 'S':
    case 'W':
    {
        if (flags_.unicode)
        {
            fail(std::string{R"('\)"} + byte +
                 "' in Unicode mode needs the Unicode tables the byte reading has not got; (?-u) scopes the ASCII "
                 "form, and [[:digit:]], [[:space:]] and [[:word:]] spell it");
        }

        const auto negated{byte == 'D' || byte == 'S' || byte == 'W'};

        const auto kind{static_cast<char>(byte | 0x20)};

        Scalar_set members;

        if (kind == 'd' || kind == 'w')
        {
            members.add('0', '9');
        }

        if (kind == 'w')
        {
            members.add('A', 'Z');
            members.add('a', 'z');
            members.add('_', '_');
        }

        if (kind == 's')
        {
            members.add('\t', '\r');
            members.add(' ', ' ');
        }

        return negated ? universe().minus(members) : members;
    }
    case 'p':
    case 'P':
        fail("a Unicode class needs the Unicode tables the byte reading has not got");
    case 'A':
    case 'z':
    case 'b':
    case 'B':
    case '<':
    case '>':
        fail(std::string{R"(the assertion '\)"} + byte +
             "' conditions the context a match stands in, which a token language cannot say");
    default:
        break;
    }

    if (is_digit(byte))
    {
        fail("the regex crate has neither octal escapes nor backreferences");
    }

    if (is_letter(static_cast<unsigned char>(byte)) || static_cast<unsigned char>(byte) >= 0x80)
    {
        fail(std::string{R"('\)"} + byte + "' is not an escape of the regex crate");
    }

    return Unit{.value = static_cast<unsigned char>(byte), .byte = false};
}

Unit Pattern_reader::hex_escape(const char kind)
{
    char32_t value{0};

    std::size_t digits{0};

    const auto braced{accept('{')};

    if (braced)
    {
        for (; peek() && is_hex_digit(*peek()); ++digits)
        {
            if (value > last_scalar)
            {
                fail("the hex escape exceeds any scalar");
            }

            value = value * 16 + hex_value(next("a hex digit"));
        }

        expect('}', "'}' to close the hex escape");
    }
    else
    {
        for (const auto wanted{kind == 'x' ? 2UZ : kind == 'u' ? 4UZ : 8UZ}; digits < wanted; ++digits)
        {
            const auto digit{next("a hex digit")};

            if (!is_hex_digit(digit))
            {
                fail(std::string{R"('\)"} + kind + "' needs " + std::to_string(wanted) + " hex digits, or braces");
            }

            value = value * 16 + hex_value(digit);
        }
    }

    if (digits == 0)
    {
        fail("the hex escape has no digits");
    }

    // The two-digit form is a byte outside Unicode mode; every other form names a scalar in either mode.
    const auto byte{!flags_.unicode && kind == 'x' && !braced};

    if (!byte && (value > last_scalar || (value >= 0xD800 && value <= 0xDFFF)))
    {
        fail("the hex escape names no scalar");
    }

    return {.value = value, .byte = byte};
}

char32_t Pattern_reader::member(const Unit unit)
{
    if (!unit.byte && !flags_.unicode && unit.value > 0x7F)
    {
        fail("a class outside Unicode mode holds bytes, not the encoding of a non-ASCII scalar");
    }

    return unit.value;
}

std::pair<std::size_t, std::optional<std::size_t>> Pattern_reader::count()
{
    const auto min{number()};

    if (!min)
    {
        fail("a count needs a number after '{'");
    }

    if (accept('}'))
    {
        return {*min, *min};
    }

    expect(',', "',' or '}' in the count");

    if (accept('}'))
    {
        return {*min, std::nullopt};
    }

    const auto max{number()};

    if (!max)
    {
        fail("a count's upper bound needs a number");
    }

    if (*max < *min)
    {
        fail("the count's upper bound is below its lower bound");
    }

    expect('}', "'}' to close the count");

    return {*min, *max};
}

std::optional<std::size_t> Pattern_reader::number()
{
    if (!peek() || !is_digit(*peek()))
    {
        return std::nullopt;
    }

    std::size_t value{0};

    while (peek() && is_digit(*peek()))
    {
        const auto digit{static_cast<std::size_t>(next("a digit") - '0')};

        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10)
        {
            fail("the count does not fit");
        }

        value = value * 10 + digit;
    }

    return value;
}

Node Pattern_reader::unit_node(const Unit unit)
{
    const auto [value, byte]{unit};

    if (flags_.insensitive && is_letter(value))
    {
        Scalar_set letter;

        letter.add(value, value);

        return {.kind = Class{.set = folded(letter, flags_.unicode), .unicode = flags_.unicode}};
    }

    if (flags_.insensitive && flags_.unicode && value > 0x7F)
    {
        fail("the case folding of a non-ASCII scalar under (?i) is not modelled");
    }

    return {.kind = Bytes{.bytes = byte ? std::string(1, static_cast<char>(value)) : encoded(value), .bounded = false}};
}

Node Pattern_reader::class_node(Scalar_set members, const bool negated)
{
    if (flags_.insensitive)
    {
        // The crate folds before it negates; folding is modelled for ASCII and the two scalars that fold to it.
        const auto foldable{std::ranges::all_of(members.ranges(), [](const Scalar_set::Range_t& range) {
            const auto& [low, high]{range};

            return high <= 0x7F || (low == high && (low == 0x17F || low == 0x212A));
        })};

        if (flags_.unicode && !foldable)
        {
            fail("the case folding of a class with non-ASCII members under (?i) is not modelled");
        }
    }

    members = negated ? universe().minus(cased(members)) : cased(members);

    if (members.empty())
    {
        fail("the class matches nothing");
    }

    if (const auto one{members.single()})
    {
        return {.kind =
                        Bytes{.bytes = flags_.unicode ? encoded(*one) : std::string(1, static_cast<char>(*one)),
                              .bounded = false}};
    }

    return {.kind = Class{.set = std::move(members), .unicode = flags_.unicode}};
}

Scalar_set Pattern_reader::cased(const Scalar_set& members) const
{
    return flags_.insensitive ? folded(members, flags_.unicode) : members;
}

Scalar_set Pattern_reader::universe() const
{
    Scalar_set set;

    if (flags_.unicode)
    {
        set.add(0, 0xD7FF);
        set.add(0xE000, last_scalar);
    }
    else
    {
        set.add(0, 0xFF);
    }

    return set;
}

Unit Pattern_reader::next_unit()
{
    const auto lead{static_cast<unsigned char>(next("a character"))};

    // A byte string's bytes reach the crate as `\xHH`, bytes outside Unicode mode and scalars inside it.
    if (literal_.byte_string || lead < 0x80)
    {
        return {.value = lead, .byte = !flags_.unicode};
    }

    const auto length{lead >= 0xF0 ? 4UZ : lead >= 0xE0 ? 3UZ : 2UZ};

    char32_t value{lead & (0xFFU >> (length + 1))};

    for (std::size_t index{1}; index < length; ++index)
    {
        value = (value << 6U) | (static_cast<unsigned char>(next("a UTF-8 continuation byte")) & 0x3FU);
    }

    return {.value = value, .byte = false};
}

std::optional<char> Pattern_reader::peek() const noexcept
{
    return at_ < literal_.bytes.size() ? std::optional{literal_.bytes[at_]} : std::nullopt;
}

bool Pattern_reader::at(const std::string_view prefix) const noexcept
{
    return std::string_view{literal_.bytes}.substr(at_).starts_with(prefix);
}

bool Pattern_reader::accept(const char byte) noexcept
{
    if (peek() != byte)
    {
        return false;
    }

    ++at_;

    return true;
}

void Pattern_reader::expect(const char byte, const std::string_view what)
{
    if (!accept(byte))
    {
        fail("expected " + std::string{what});
    }
}

char Pattern_reader::next(const std::string_view what)
{
    if (at_ >= literal_.bytes.size())
    {
        fail("expected " + std::string{what} + " before the end of the pattern");
    }

    return literal_.bytes[at_++];
}

void Pattern_reader::fail(const std::string& message) const
{
    throw Spec_error{"the pattern " + literal_.written + " is refused: " + message, line_};
}

} // namespace

std::vector<Lexer_spec> read_logos(const std::string_view source)
{
    Rust_cursor cursor{source, 0, source.size()};

    std::vector<Lexer_spec> lexers;

    // The outer attributes read since the last item keyword, which the item they belong to consumes.
    std::vector<Attribute> pending;

    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        if (cursor.at("#["))
        {
            pending.push_back(read_attribute(cursor));

            continue;
        }

        if (cursor.at("#!["))
        {
            cursor.expect('#', "'#'");
            cursor.expect('!', "'!'");

            cursor.skip_group();

            continue;
        }

        // A raw or byte string's prefix would read as a word otherwise.
        if (cursor.at_string())
        {
            cursor.skip_token();

            pending.clear();

            continue;
        }

        const auto word{cursor.word()};

        if (word == "pub")
        {
            cursor.skip_trivia();

            if (cursor.peek() == '(')
            {
                cursor.skip_group();
            }

            continue;
        }

        if (word == "enum")
        {
            if (const auto line{derives_logos(pending, cursor)})
            {
                lexers.push_back(read_enum(cursor, pending, *line));
            }
        }
        else if (word.empty() && (cursor.peek() == '(' || cursor.peek() == '[' || cursor.peek() == '{'))
        {
            // Descended into rather than skipped, so that an enum inside a module or a function is found too.
            std::ignore = cursor.next("a delimiter");
        }
        else if (word.empty())
        {
            cursor.skip_token();
        }

        pending.clear();
    }

    return lexers;
}

} // namespace munch::tools::audit
