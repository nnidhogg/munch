#include "munch/regex/parse.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/regex/set.hpp"
#include "munch/regex/utf8.hpp"

namespace munch::regex
{
namespace
{
/**
 * @brief One parsed atom with its postfix operators applied: a single literal byte, kept apart so that adjacent
 *        literals merge into one text node, or a finished regex.
 */
struct Piece
{
    /**
     * @brief The byte, when the piece is one literal with no operator on it.
     */
    std::optional<char> literal{};

    /**
     * @brief The regex, when it is anything else.
     */
    std::optional<Regex> regex{};
};

/**
 * @brief A decoded escape: one byte, or one scalar from a `\u{...}` escape, which a literal spells as UTF-8 and a
 *        bracket holds as a code point.
 */
struct Escaped
{
    /**
     * @brief The byte's or the scalar's value.
     */
    char32_t value{};

    /**
     * @brief Whether the value is a scalar rather than a byte.
     */
    bool scalar{};
};

/**
 * @brief The largest Unicode scalar value.
 */
constexpr char32_t max_scalar{0x10FFFF};

/**
 * @brief The first code point of the surrogate gap, which no scalar value occupies.
 */
constexpr char32_t surrogate_first{0xD800};

/**
 * @brief The last code point of the surrogate gap.
 */
constexpr char32_t surrogate_last{0xDFFF};

/**
 * @brief The first code point beyond ASCII.
 */
constexpr char32_t first_beyond_ascii{0x80};

/**
 * @brief The first code point beyond a byte, past every member a bracket reads as bytes.
 */
constexpr char32_t first_beyond_byte{0x100};

/**
 * @brief flex's class difference operator, `{-}`.
 */
constexpr std::string_view difference_operator{"{-}"};

/**
 * @brief flex's class union operator, `{+}`.
 */
constexpr std::string_view union_operator{"{+}"};

/**
 * @brief One bracket expression's members as read up to its close, held both ways, since which reading the bracket
 *        takes is known only there; once bracket_members() has settled the reading, only the side wide selects is
 *        meaningful.
 */
struct Bracket_reading
{
    /**
     * @brief The members below 0x100, as bytes.
     */
    Set bytes{};

    /**
     * @brief Every member, as code point ranges.
     */
    std::vector<utf8::Code_point_range> scalars{};

    /**
     * @brief Whether a member is a code point escape, which reads the bracket over scalars.
     */
    bool wide{};

    /**
     * @brief Whether a member is a byte beyond ASCII, which no scalar reading admits.
     */
    bool byte_beyond_ascii{};
};

/**
 * @brief The bounds of a counted repetition.
 */
struct Count
{
    /**
     * @brief The fewest repetitions.
     */
    std::size_t min{};

    /**
     * @brief The most repetitions, or std::nullopt when unbounded.
     */
    std::optional<std::size_t> max{};
};

/**
 * @brief A recursive-descent reader over one pattern, producing the regex as it goes.
 *
 * The grammar is the usual one: an alternation of sequences, a sequence of repetitions, a repetition an atom under zero
 * or more postfix operators. Definitions are read by a fresh reader over the definition's own text, the chain of names
 * being expanded carried along so that a cycle is refused rather than followed.
 */
class Reader
{
public:
    /**
     * @brief Binds the reader to a pattern.
     * @param pattern The pattern.
     * @param expanding The names whose definitions are being expanded above this reader, outermost first.
     * @param options What the pattern is read under.
     */
    Reader(std::string_view pattern, std::vector<std::string> expanding, Parse_options options);

    /**
     * @brief Reads the whole pattern.
     * @param definitions The named patterns `{name}` may expand to.
     * @return The regex.
     * @throws Syntax_error If anything is left over or the pattern is refused.
     */
    [[nodiscard]] Regex read(const Definitions_t& definitions);

private:
    /**
     * @brief Reads an alternation: sequences separated by `|`.
     * @param definitions The named patterns.
     * @return The regex, a choice when there is more than one sequence.
     */
    [[nodiscard]] Regex alternation(const Definitions_t& definitions);

    /**
     * @brief Reads a sequence: repetitions up to a `|`, a `)` or the end, adjacent literals merged into one text node.
     * @param definitions The named patterns.
     * @return The regex.
     * @throws Syntax_error If the sequence is empty, which the standard refuses.
     */
    [[nodiscard]] Regex sequence(const Definitions_t& definitions);

    /**
     * @brief Reads an atom under its postfix operators.
     * @param definitions The named patterns.
     * @return The piece.
     */
    [[nodiscard]] Piece repetition(const Definitions_t& definitions);

    /**
     * @brief Reads one atom: a group, a bracket expression, a quoted literal, a definition, the dot, an escape or a
     *        byte.
     * @param definitions The named patterns.
     * @return The piece, a literal for a plain or escaped byte.
     * @throws Syntax_error For an anchor, trailing context, a start-condition prefix at the pattern's start, or an
     *         operator with nothing before it.
     */
    [[nodiscard]] Piece atom(const Definitions_t& definitions);

    /**
     * @brief Reads the flags of a flag group after its `(?`, through the `:`, setting the case option they name.
     *
     * flex's `(?i:...)` and `(?-i:...)` set the case option inside the group alone; its other flags, `s` and `x`,
     * change what the dot and blanks mean and are not read. flex takes one '-' among the flags, everything after it
     * turned off; a second is its "bad character" error, `(?i--i:` and `(?-i-s:` both, so it is refused here too.
     * @throws Syntax_error If a flag other than `i` stands among them, or a second '-'.
     */
    void group_flags();

    /**
     * @brief Reads a bracket expression after its `[`, through its `]`, with the class operators that follow it.
     *
     * Over bytes unless a member is a code point escape, when every member is read as a scalar and the bracket matches
     * the UTF-8 encoding of one of them; negation then runs over the scalars rather than the bytes. flex's class
     * operators `{-}` and `{+}`, the difference and the union, each take a further bracket on their right and bind
     * tighter than every postfix operator, so the operator repeats the whole combined class.
     * @return The regex it names: any_of over a set, or the encodings of the scalars.
     * @throws Syntax_error If a range is reversed, bytes beyond ASCII stand beside code points, a class operator has no
     *         bracket on its right or combines brackets of code points, or nothing is left to match.
     */
    [[nodiscard]] Regex bracket();

    /**
     * @brief Reads one bracket expression's members after its `[`, through its `]`, settles whether the bracket reads
     *        over bytes or over scalars, and applies its negation to that reading.
     * @return The members, over bytes or over scalars.
     * @throws Syntax_error If a range is reversed, or bytes beyond ASCII stand beside code points.
     */
    [[nodiscard]] Bracket_reading bracket_members();

    /**
     * @brief Reads one bracket expression's members after its `[` and any negating `^`, through its `]`, holding each
     *        both as bytes and as scalars, with both cases of every letter under the caseless option.
     * @return The members, held both ways.
     * @throws Syntax_error If a range is reversed.
     */
    [[nodiscard]] Bracket_reading read_both_ways();

    /**
     * @brief Returns the regex matching one member of a bracket.
     * @param members The members, their negation and the class operators already applied.
     * @param opened The offset of the `[` they were read from, which an empty bracket is refused at.
     * @return The regex: any_of over the bytes, or the encodings of the scalars.
     * @throws Syntax_error If the members are empty, so that nothing can match.
     */
    [[nodiscard]] Regex matching(const Bracket_reading& members, std::size_t opened);

    /**
     * @brief Reads a POSIX class after its `[:`, through its `:]`, negated when a `^` opens its name.
     * @return The bytes it names, or every other byte when it is negated.
     * @throws Syntax_error If the name is not one of the twelve, or a negated case class stands under the case option,
     *         which flex calls ambiguous.
     */
    [[nodiscard]] Set posix_class();

    /**
     * @brief Returns whether a POSIX class stands at an offset: `[:`, an optional negating `^`, letters and `:]`, which
     *        is the only shape flex lexes as a class, its CCL_EXPR.
     * @param at The offset of the `[`.
     * @return True when a class stands there.
     */
    [[nodiscard]] bool class_stands(std::size_t at) const noexcept;

    /**
     * @brief Reads a double-quoted literal after its opening quote, through the closing one.
     * @return The decoded bytes.
     */
    [[nodiscard]] std::string quoted();

    /**
     * @brief Returns the regex of a run of literal bytes: one text node, or under the caseless option the run with each
     *        letter widened to the set of its two cases and the bytes between letters kept as text.
     * @param run The bytes.
     * @return The regex.
     */
    [[nodiscard]] Regex literal(std::string run) const;

    /**
     * @brief Reads an escape after its backslash: a named control, octal, hex, a code point `\u{...}`, or the byte
     *        itself.
     * @return The byte, or the scalar.
     * @throws Syntax_error If a hex or code point escape has no digits, or the code point is a surrogate or beyond
     *         U+10FFFF.
     */
    [[nodiscard]] Escaped escape();

    /**
     * @brief Reads a counted repetition after its `{`, through its `}`.
     * @return The minimum and, when bounded, the maximum.
     * @throws Syntax_error If the bounds are missing, reversed, or too large to hold.
     */
    [[nodiscard]] Count count();

    /**
     * @brief Reads a definition reference after its `{`, through its `}`, expanded by a reader over its text.
     * @param open The offset of the `{`, where a fault inside the definition is reported.
     * @param definitions The named patterns.
     * @return The definition's regex.
     * @throws Syntax_error If the name is unknown or its expansion cycles.
     */
    [[nodiscard]] Regex definition(std::size_t open, const Definitions_t& definitions);

    /**
     * @brief Returns whether this reader reads a definition's expansion, which flex encloses in parentheses, so that
     *        the pattern's edges are not the rule's and nothing standing only at a rule's edge stands at them.
     * @return True inside an expansion.
     */
    [[nodiscard]] bool expansion() const noexcept;

    /**
     * @brief Reads the unsigned decimal at the current position.
     * @return The number, or std::nullopt when no digit stands here.
     * @throws Syntax_error If the number does not fit.
     */
    [[nodiscard]] std::optional<std::size_t> number();

    /**
     * @brief Reads up to a limit of digits in a base from the current position, advancing past them.
     * @param base The base, 8 or 16.
     * @param limit The most digits read.
     * @return The value read and how many digits made it; no digit leaves the position where it was.
     */
    [[nodiscard]] std::pair<char32_t, std::size_t> digits(int base, std::size_t limit);

    /**
     * @brief Returns the byte at the current position, or nothing at the end.
     * @return The byte.
     */
    [[nodiscard]] std::optional<char> peek() const noexcept;

    /**
     * @brief Consumes and returns the byte at the current position.
     * @param what What the syntax expected here, named when the pattern has ended instead.
     * @return The byte.
     * @throws Syntax_error At the end of the pattern.
     */
    char next(std::string_view what);

    /**
     * @brief Consumes the byte if it is the one given.
     * @param byte The byte asked for.
     * @return True when consumed.
     */
    [[nodiscard]] bool accept(char byte) noexcept;

    /**
     * @brief Consumes the byte given or refuses, naming what the syntax expected and what stands there instead.
     * @param byte The byte required.
     * @param what What the syntax expected.
     * @throws Syntax_error If the byte is not the one required, or the pattern has ended.
     */
    void expect(char byte, std::string_view what);

    /**
     * @brief Refuses the pattern at the current position.
     * @param message Why.
     */
    [[noreturn]] void fail(std::string_view message) const;

    /**
     * @brief Refuses the pattern for ending where the syntax expected more.
     * @param what What the syntax expected.
     */
    [[noreturn]] void fail_at_end(std::string_view what) const;

    /**
     * @brief The pattern being read.
     */
    std::string_view pattern_;

    /**
     * @brief The names being expanded above this reader, so that a definition naming one of them is a cycle.
     */
    std::vector<std::string> expanding_;

    /**
     * @brief What the pattern is read under.
     */
    Parse_options options_;

    /**
     * @brief The offset of the next byte to read.
     */
    std::size_t at_{0};
};

/**
 * @brief Returns the one regex of a list, or the node joining them when there are several.
 * @tparam Node The joining node, Choice or Concat.
 * @param parts The regexes, at least one.
 * @return The regex.
 */
template <typename Node>
[[nodiscard]] Regex one_or(std::vector<Regex> parts)
{
    if (parts.size() == 1)
    {
        return std::move(parts.front());
    }

    return {.node = Node{.regexes = std::move(parts)}};
}

/**
 * @brief Widens a byte to the scalar of the same value, so a byte beyond ASCII carries no sign extension.
 * @param byte The byte.
 * @return Its value as a scalar.
 */
[[nodiscard]] constexpr char32_t widened(const char byte) noexcept
{
    return static_cast<unsigned char>(byte);
}

/**
 * @brief Returns whether a scalar is an ASCII letter, which is what the case option folds, tested directly so no locale
 *        is consulted.
 * @param scalar The scalar.
 * @return True for a to z and A to Z.
 */
[[nodiscard]] constexpr bool has_case(const char32_t scalar) noexcept
{
    return (scalar >= 'a' && scalar <= 'z') || (scalar >= 'A' && scalar <= 'Z');
}

/**
 * @brief Returns the other case of an ASCII letter, or the scalar itself when it is no letter, tested directly so no
 *        locale is consulted.
 * @param scalar The scalar.
 * @return The scalar with its case swapped.
 */
[[nodiscard]] constexpr char32_t swapped(const char32_t scalar) noexcept
{
    if (scalar >= 'a' && scalar <= 'z')
    {
        return scalar - 'a' + 'A';
    }

    if (scalar >= 'A' && scalar <= 'Z')
    {
        return scalar - 'A' + 'a';
    }

    return scalar;
}

/**
 * @brief Returns the members the case option adds for one bracket member or range, as flex folds them.
 *
 * flex swaps the case of a member letter, and of both ends of a range, adding the range the swapped ends span, so an
 * ambiguous range such as [A-t] gains the empty range a to T and keeps its numeric span exactly, which is what the
 * table under Patterns calls the literal range, and a range whose end has no case at all, [_-{] or [@-C], folds no
 * letter.
 *
 * A range reaching past ASCII has no swapped end, since only the code point escape flex has not got writes one: the
 * folding there runs over the range's ASCII intersection, which ends past every letter, so the other case of every
 * ASCII letter the range holds is a member and `[\u{61}-\u{100}]` folds exactly as `[\u{61}-\u{7f}]` does.
 * @param low The member, or the first of the range.
 * @param high The member again, or the last of the range.
 * @param wide Whether the bracket reads its members as scalars, the one reading a range reaches past ASCII in.
 * @return The ranges to add, empty when the folding adds none.
 */
[[nodiscard]] std::vector<utf8::Code_point_range> folded(const char32_t low, const char32_t high, const bool wide)
{
    if (wide && high >= first_beyond_ascii)
    {
        std::vector<utf8::Code_point_range> ranges{};

        constexpr std::array<utf8::Code_point_range, 2> cases{
                {{.first = 'a', .last = 'z'}, {.first = 'A', .last = 'Z'}}};

        for (const auto& [first, last] : cases)
        {
            if (low <= last)
            {
                ranges.push_back({.first = swapped(std::max(low, first)), .last = swapped(last)});
            }
        }

        return ranges;
    }

    if (!has_case(low) || !has_case(high))
    {
        return {};
    }

    const auto first{swapped(low)};

    const auto last{swapped(high)};

    if (last < first)
    {
        return {};
    }

    return {{.first = first, .last = last}};
}

/**
 * @brief Returns the members the case option adds for every member or range of a bracket, in their order.
 * @param ranges The bracket's members and ranges as scalars.
 * @param wide Whether the bracket reads its members as scalars.
 * @return The ranges to add.
 */
[[nodiscard]] std::vector<utf8::Code_point_range> case_folds(
        const std::vector<utf8::Code_point_range>& ranges, const bool wide)
{
    std::vector<utf8::Code_point_range> added{};

    for (const auto& [low, high] : ranges)
    {
        const auto others{folded(low, high, wide)};

        added.insert(added.end(), others.begin(), others.end());
    }

    return added;
}

/**
 * @brief Returns the scalars no range of a set holds, as ranges ascending.
 * @param ranges The ranges, in any order, possibly overlapping.
 * @return The complement over every scalar up to U+10FFFF.
 */
[[nodiscard]] std::vector<utf8::Code_point_range> complement_of(std::vector<utf8::Code_point_range> ranges)
{
    std::vector<utf8::Code_point_range> complement{};

    char32_t from{0};

    std::ranges::sort(ranges, {}, &utf8::Code_point_range::first);

    for (const auto& [low, high] : ranges)
    {
        if (low > from)
        {
            complement.push_back({.first = from, .last = low - 1U});
        }

        from = std::max(from, static_cast<char32_t>(high + 1U));
    }

    if (from <= max_scalar)
    {
        complement.push_back({.first = from, .last = max_scalar});
    }

    return complement;
}

/**
 * @brief Returns whether a byte is a decimal digit, tested directly so no locale is consulted.
 * @param byte The byte.
 * @return True for 0 to 9.
 */
[[nodiscard]] constexpr bool is_digit(const char byte) noexcept
{
    return byte >= '0' && byte <= '9';
}

/**
 * @brief Returns whether a byte is an octal digit, tested directly so no locale is consulted.
 * @param byte The byte.
 * @return True for 0 to 7.
 */
[[nodiscard]] constexpr bool is_octal_digit(const char byte) noexcept
{
    return byte >= '0' && byte <= '7';
}

/**
 * @brief Returns the regex matching the UTF-8 encoding of one scalar from a set of ranges, the ranges sorted and merged
 *        first and the ones holding surrogates alone dropped, since no encoding has those.
 * @param ranges The ranges, in any order, possibly overlapping.
 * @return The regex, or std::nullopt when nothing remains.
 */
[[nodiscard]] std::optional<Regex> encodings(std::vector<utf8::Code_point_range> ranges)
{
    std::ranges::sort(ranges, {}, &utf8::Code_point_range::first);

    std::vector<utf8::Code_point_range> merged{};

    for (const auto& [first, last] : ranges)
    {
        if (first >= surrogate_first && last <= surrogate_last)
        {
            continue;
        }

        if (merged.empty() || first > merged.back().last + 1U)
        {
            merged.push_back({.first = first, .last = last});

            continue;
        }

        merged.back().last = std::max(merged.back().last, last);
    }

    if (merged.empty())
    {
        return std::nullopt;
    }

    return utf8::ranges(merged);
}

/**
 * @brief Returns the bytes a POSIX bracket class names, `[:alpha:]` and its kin.
 * @param name The name between the colons.
 * @return Its bytes, or std::nullopt for a name no class carries.
 */
[[nodiscard]] std::optional<Set> posix_bytes(const std::string_view name)
{
    if (name == "alpha")
    {
        return Set::alpha();
    }

    if (name == "digit")
    {
        return Set::digits();
    }

    if (name == "alnum")
    {
        return Set::alphanum();
    }

    if (name == "upper")
    {
        return Set::range('A', 'Z');
    }

    if (name == "lower")
    {
        return Set::range('a', 'z');
    }

    if (name == "space")
    {
        return Set{' ', '\t', '\n', '\r', '\f', '\v'};
    }

    if (name == "blank")
    {
        return Set{' ', '\t'};
    }

    if (name == "punct")
    {
        return Set::printable() - Set::alphanum() - Set{' '};
    }

    if (name == "print")
    {
        return Set::printable();
    }

    if (name == "graph")
    {
        return Set::printable() - Set{' '};
    }

    if (name == "cntrl")
    {
        return Set::range('\x00', '\x1F') + Set{'\x7F'};
    }

    if (name == "xdigit")
    {
        return Set::digits() + Set::range('a', 'f') + Set::range('A', 'F');
    }

    return std::nullopt;
}

Reader::Reader(const std::string_view pattern, std::vector<std::string> expanding, const Parse_options options)
    : pattern_{pattern}, expanding_{std::move(expanding)}, options_{options}
{}

Regex Reader::read(const Definitions_t& definitions)
{
    auto regex{alternation(definitions)};

    if (const auto left{peek()})
    {
        const auto message{std::format("unexpected '{}'", *left)};

        fail(message);
    }

    return regex;
}

Regex Reader::alternation(const Definitions_t& definitions)
{
    std::vector<Regex> branches{};

    branches.push_back(sequence(definitions));

    while (accept('|'))
    {
        branches.push_back(sequence(definitions));
    }

    return one_or<Choice>(std::move(branches));
}

Regex Reader::sequence(const Definitions_t& definitions)
{
    std::vector<Regex> parts{};

    std::string run{};

    const auto flush{[this, &parts, &run] {
        if (!run.empty())
        {
            parts.push_back(literal(std::exchange(run, {})));
        }
    }};

    while (peek() && *peek() != '|' && *peek() != ')')
    {
        auto [literal, regex]{repetition(definitions)};

        if (literal)
        {
            run.push_back(*literal);

            continue;
        }

        flush();

        parts.push_back(std::move(*regex));
    }

    flush();

    if (parts.empty())
    {
        fail("empty pattern: a branch or group must match at least one byte");
    }

    return one_or<Concat>(std::move(parts));
}

Piece Reader::repetition(const Definitions_t& definitions)
{
    auto piece{atom(definitions)};

    const auto claim{[this, &piece] {
        if (piece.literal)
        {
            piece.regex = literal({*piece.literal});

            piece.literal.reset();
        }
    }};

    const auto count_opens{
            [this] { return peek() == '{' && at_ + 1U < pattern_.size() && is_digit(pattern_[at_ + 1U]); }};

    const auto repeat_by{[&piece, &claim](const char postfix) {
        claim();

        auto body{std::move(*piece.regex)};

        switch (postfix)
        {
        case '*':
            piece.regex = kleene(std::move(body));
            break;

        case '+':
            piece.regex = plus(std::move(body));
            break;

        default:
            piece.regex = optional(std::move(body));
            break;
        }
    }};

    for (;;)
    {
        if (const auto postfix{peek()}; postfix == '*' || postfix == '+' || postfix == '?')
        {
            ++at_;

            repeat_by(*postfix);

            continue;
        }

        if (!count_opens())
        {
            return piece;
        }

        ++at_;

        const auto [min, max]{count()};

        claim();

        if (!max)
        {
            piece.regex = at_least(std::move(*piece.regex), min);

            continue;
        }

        if (*max == min)
        {
            piece.regex = exact(std::move(*piece.regex), min);

            continue;
        }

        piece.regex = range(std::move(*piece.regex), min, *max);
    }
}

Piece Reader::atom(const Definitions_t& definitions)
{
    const auto open{at_};

    const auto byte{next("a pattern")};

    switch (byte)
    {
    case '(':
    {
        // The case option a flag group sets holds inside the group alone.
        const auto saved{options_};

        if (accept('?'))
        {
            group_flags();
        }

        auto inner{alternation(definitions)};

        expect(')', "')' to close the group");

        options_ = saved;

        return {.literal = std::nullopt, .regex = std::move(inner)};
    }
    case '[':
        return {.literal = std::nullopt, .regex = bracket()};
    case '"':
    {
        auto quoted_text{quoted()};

        if (quoted_text.size() == 1U)
        {
            return {.literal = quoted_text.front(), .regex = std::nullopt};
        }

        return {.literal = std::nullopt, .regex = literal(std::move(quoted_text))};
    }
    case '{':
        return {.literal = std::nullopt, .regex = definition(open, definitions)};
    case '.':
        return {.literal = std::nullopt, .regex = any_of(Set::all() - Set{'\n'})};
    case '\\':
    {
        const auto [value, scalar]{escape()};

        if (!scalar || value < first_beyond_ascii)
        {
            return {.literal = static_cast<char>(value), .regex = std::nullopt};
        }

        return {.literal = std::nullopt, .regex = text(utf8::encode(value))};
    }
    case '*':
    case '+':
    case '?':
    {
        --at_;

        const auto message{std::format("nothing for '{}' to repeat", byte)};

        fail(message);
    }
    case ')':
        --at_;

        fail("unexpected ')'");
    case '^':
    case '$':
    {
        // Anchors only where flex reads them as anchors, a '^' opening the pattern and a '$' closing it; a '$' inside a
        // pattern is the byte, which a Pascal hex literal or a shell variable spells.
        const auto anchors{(byte == '^' && open == 0U) || (byte == '$' && at_ == pattern_.size())};

        if (!anchors)
        {
            return {.literal = byte, .regex = std::nullopt};
        }

        --at_;

        const auto message{std::format(
                "the anchor '{}' conditions the context a match stands in, which a token language cannot say", byte)};

        fail(message);
    }
    case '/':
        --at_;

        fail("trailing context conditions what follows a match, which a token language cannot say");
    case '<':
    {
        // A start-condition prefix stands at the start of the rule's pattern and nowhere else, so a '<' past it, or
        // anywhere inside an expansion, is the byte a comparison operator spells; <<EOF>> is a token flex's scanner
        // reads wherever it stands, and refuses away from the start.
        const auto end_of_file{pattern_.substr(open).starts_with("<<EOF>>")};

        const auto prefix_stands{open == 0U && !expansion()};

        if (!end_of_file && !prefix_stands)
        {
            return {.literal = byte, .regex = std::nullopt};
        }

        --at_;

        fail("a start condition or <<EOF>> is the rule's, not the pattern's, and is read by the rule reader");
    }
    default:
        return {.literal = byte, .regex = std::nullopt};
    }
}

void Reader::group_flags()
{
    constexpr std::string_view wanted{"a flag or ':' after '(?'"};

    auto negated{false};

    for (auto flag{next(wanted)}; flag != ':'; flag = next(wanted))
    {
        if (flag == 'i')
        {
            options_.caseless = !negated;

            continue;
        }

        if (flag != '-')
        {
            --at_;

            const auto message{std::format("the group flag '{}' is not read", flag)};

            fail(message);
        }

        if (negated)
        {
            --at_;

            fail("a second '-' among the group flags, which flex refuses");
        }

        negated = true;
    }
}

Regex Reader::bracket()
{
    const auto opened{at_ - 1U};

    auto members{bracket_members()};

    const auto operator_stands{[this] {
        const auto rest{pattern_.substr(at_)};

        return rest.starts_with(difference_operator) || rest.starts_with(union_operator);
    }};

    // flex's class operators, `{-}` for the difference and `{+}` for the union, each taking a bracket on its right and
    // left associative, so `[abc]{-}[b]{-}[c]` is `[a]`.
    while (operator_stands())
    {
        const auto operator_at{at_};

        const auto difference{pattern_[at_ + 1U] == '-'};

        at_ += difference_operator.size();

        if (!accept('['))
        {
            at_ = operator_at;

            const auto message{std::format(
                    "flex's class {} takes a bracket expression on its right", difference ? "difference" : "union")};

            fail(message);
        }

        const auto right{bracket_members()};

        if (members.wide || right.wide)
        {
            at_ = operator_at;

            fail("flex's class operators combine brackets of bytes, which a bracket of code points is not");
        }

        members.bytes = difference ? members.bytes - right.bytes : members.bytes + right.bytes;
    }

    return matching(members, opened);
}

Bracket_reading Reader::bracket_members()
{
    const auto opened{at_ - 1U};

    const auto negated{accept('^')};

    auto reading{read_both_ways()};

    if (!reading.wide)
    {
        if (negated)
        {
            reading.bytes = Set::all() - reading.bytes;
        }

        return reading;
    }

    // Read as scalars: a byte beyond ASCII is no scalar, so one beside a code point is refused.
    if (reading.byte_beyond_ascii)
    {
        at_ = opened;

        fail("a bracket mixes bytes beyond ASCII with code points; write the bytes as code points");
    }

    if (negated)
    {
        reading.scalars = complement_of(std::move(reading.scalars));
    }

    return reading;
}

Bracket_reading Reader::read_both_ways()
{
    Bracket_reading reading{};

    const auto add{[&reading](const char32_t low, const char32_t high) {
        if (high < first_beyond_byte)
        {
            reading.bytes += Set::range(static_cast<char>(low), static_cast<char>(high));
        }

        reading.scalars.push_back({.first = low, .last = high});
    }};

    const auto member{[this, &reading](const char32_t read) -> char32_t {
        if (read != '\\')
        {
            reading.byte_beyond_ascii = reading.byte_beyond_ascii || read >= first_beyond_ascii;

            return read;
        }

        const auto [value, scalar]{escape()};

        reading.wide = reading.wide || scalar;

        reading.byte_beyond_ascii = reading.byte_beyond_ascii || (!scalar && value >= first_beyond_ascii);

        return value;
    }};

    // A ']' leading the bracket is a member rather than the close, as the standard has it.
    auto leading{true};

    for (;;)
    {
        const auto open{at_};

        const auto byte{widened(next("']' to close the bracket expression"))};

        if (byte == ']' && !leading)
        {
            break;
        }

        leading = false;

        // Only the shape flex lexes as a class is one, so a `[:` of any other shape leaves the `[` an ordinary member:
        // flex reads `[[:al]pha:]` as the bracket `[[:al]` and the text `pha:]` after it.
        if (byte == '[' && class_stands(open) && accept(':'))
        {
            const auto class_bytes{posix_class()};

            for (const auto value : class_bytes.symbols())
            {
                const auto scalar{widened(value)};

                // A negated class names the bytes beyond ASCII too, and those are no scalars either.
                reading.byte_beyond_ascii = reading.byte_beyond_ascii || scalar >= first_beyond_ascii;

                add(scalar, scalar);
            }

            continue;
        }

        auto first{member(byte)};

        auto last{first};

        // A '-' between two members is a range; at either edge it is itself.
        if (peek() == '-' && at_ + 1U < pattern_.size() && pattern_[at_ + 1U] != ']')
        {
            ++at_;

            const auto end{widened(next("the end of the range"))};

            last = member(end);

            if (last < first && options_.ranges_either_way)
            {
                std::swap(first, last);
            }

            if (last < first)
            {
                at_ = open;

                fail("the range ends before it starts");
            }
        }

        add(first, last);
    }

    // Under the caseless option both cases of every letter are members before any negation, as flex folds them, member
    // by member and range by range rather than over the members as a whole.
    if (options_.caseless)
    {
        for (const auto& [first, last] : case_folds(reading.scalars, reading.wide))
        {
            add(first, last);
        }
    }

    return reading;
}

Regex Reader::matching(const Bracket_reading& members, const std::size_t opened)
{
    [[maybe_unused]] const auto& [bytes, scalars, wide, byte_beyond_ascii]{members};

    if (!wide && bytes.symbols().empty())
    {
        at_ = opened;

        fail("the bracket names no byte");
    }

    if (!wide)
    {
        return any_of(bytes);
    }

    // The ASCII part as a set, the rest as encodings.
    Set ascii{};

    std::vector<utf8::Code_point_range> beyond{};

    for (const auto& [low, high] : scalars)
    {
        if (low < first_beyond_ascii)
        {
            const auto ascii_high{std::min<char32_t>(high, first_beyond_ascii - 1U)};

            ascii += Set::range(static_cast<char>(low), static_cast<char>(ascii_high));
        }

        if (high >= first_beyond_ascii)
        {
            beyond.push_back({.first = std::max<char32_t>(low, first_beyond_ascii), .last = high});
        }
    }

    auto rest{encodings(std::move(beyond))};

    if (ascii.symbols().empty() && !rest)
    {
        at_ = opened;

        fail("the bracket names no scalar");
    }

    if (ascii.symbols().empty())
    {
        return std::move(*rest);
    }

    if (!rest)
    {
        return any_of(ascii);
    }

    return choice(any_of(ascii), std::move(*rest));
}

Set Reader::posix_class()
{
    const auto open{at_};

    // flex negates a class by a '^' before its name, `[:^digit:]`, over every byte rather than over ASCII alone, since
    // its CCL_NEG_EXPR drops the isascii() test its CCL_EXPR makes.
    const auto negated{accept('^')};

    std::string name{};

    while (peek() && *peek() != ':')
    {
        name.push_back(next("a class name"));
    }

    expect(':', "':]' to close the class");

    expect(']', "':]' to close the class");

    const auto named{posix_bytes(name)};

    if (!named)
    {
        at_ = open;

        const auto message{std::format("unknown class '{}{}'", negated ? "^" : "", name)};

        fail(message);
    }

    // flex calls a negated case class ambiguous under the case option and leaves it empty, so the rule holding one
    // cannot match; it is refused by name rather than read as something that matches.
    if (negated && options_.caseless && (name == "upper" || name == "lower"))
    {
        at_ = open;

        const auto message{std::format(
                "'[:^{}:]' is ambiguous in a case-insensitive scanner, which flex leaves matching nothing", name)};

        fail(message);
    }

    if (negated)
    {
        return Set::all() - *named;
    }

    return *named;
}

bool Reader::class_stands(const std::size_t at) const noexcept
{
    constexpr std::string_view opener{"[:"};

    const auto rest{pattern_.substr(at)};

    if (!rest.starts_with(opener))
    {
        return false;
    }

    constexpr std::string_view negated_opener{"[:^"};

    const auto negated{rest.starts_with(negated_opener)};

    const auto name{at + (negated ? negated_opener.size() : opener.size())};

    auto past{name};

    while (past < pattern_.size() && has_case(widened(pattern_[past])))
    {
        ++past;
    }

    return past > name && pattern_.substr(past).starts_with(":]");
}

std::string Reader::quoted()
{
    std::string literal{};

    for (;;)
    {
        const auto byte{next(R"('"' to close the quoted text)")};

        if (byte == '"')
        {
            break;
        }

        if (byte != '\\')
        {
            literal.push_back(byte);

            continue;
        }

        const auto [value, scalar]{escape()};

        literal += scalar ? utf8::encode(value) : std::string{static_cast<char>(value)};
    }

    if (literal.empty())
    {
        fail("empty quoted text matches nothing");
    }

    return literal;
}

Regex Reader::literal(std::string run) const
{
    if (!options_.caseless)
    {
        return text(std::move(run));
    }

    std::vector<Regex> pieces{};

    std::string plain{};

    for (const auto byte : run)
    {
        const auto scalar{widened(byte)};

        if (!has_case(scalar))
        {
            plain += byte;

            continue;
        }

        if (!plain.empty())
        {
            pieces.push_back(text(std::exchange(plain, {})));
        }

        const auto other{static_cast<char>(swapped(scalar))};

        pieces.push_back(any_of(Set{byte, other}));
    }

    if (pieces.empty())
    {
        return text(std::move(plain));
    }

    if (!plain.empty())
    {
        pieces.push_back(text(std::move(plain)));
    }

    return one_or<Concat>(std::move(pieces));
}

Escaped Reader::escape()
{
    const auto byte{next("the escaped byte")};

    switch (byte)
    {
    case 'n':
        return {.value = '\n', .scalar = false};
    case 't':
        return {.value = '\t', .scalar = false};
    case 'r':
        return {.value = '\r', .scalar = false};
    case 'f':
        return {.value = '\f', .scalar = false};
    case 'v':
        return {.value = '\v', .scalar = false};
    case 'a':
        return {.value = '\a', .scalar = false};
    case 'b':
        return {.value = '\b', .scalar = false};
    case 'x':
    {
        constexpr std::size_t max_hex_byte_digits{2};

        const auto [value, read]{digits(16, max_hex_byte_digits)};

        if (read == 0U)
        {
            fail(R"('\x' needs a hex digit)");
        }

        return {.value = value, .scalar = false};
    }
    case 'u':
    {
        // A code point, \u{X...}: what flex has not got and a reader of a character-level generator writes.
        if (!accept('{'))
        {
            return {.value = widened(byte), .scalar = false};
        }

        constexpr std::string_view code_point_opener{R"(\u{)"};

        const auto open{at_ - code_point_opener.size()};

        constexpr std::size_t max_code_point_digits{6};

        const auto [value, read]{digits(16, max_code_point_digits)};

        expect('}', "'}' to close the code point");

        if (read == 0U || value > max_scalar || (value >= surrogate_first && value <= surrogate_last))
        {
            at_ = open;

            fail("a code point escape takes one to six hex digits below U+110000 and outside the surrogates");
        }

        return {.value = value, .scalar = true};
    }
    default:
        break;
    }

    // Up to three octal digits, the first already read; anything else escaped is itself.
    if (is_octal_digit(byte))
    {
        constexpr std::size_t max_octal_byte_digits{3};

        --at_;

        const auto [value, read]{digits(8, max_octal_byte_digits)};

        if (value > std::numeric_limits<unsigned char>::max())
        {
            fail("the octal escape exceeds one byte");
        }

        return {.value = value, .scalar = false};
    }

    return {.value = widened(byte), .scalar = false};
}

Count Reader::count()
{
    const auto min{number()};

    if (!min)
    {
        fail("a count needs a number");
    }

    if (accept('}'))
    {
        return {.min = *min, .max = *min};
    }

    expect(',', "',' or '}' in the count");

    if (accept('}'))
    {
        return {.min = *min, .max = std::nullopt};
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

    return {.min = *min, .max = *max};
}

Regex Reader::definition(const std::size_t open, const Definitions_t& definitions)
{
    std::string name{};

    while (peek() && *peek() != '}')
    {
        name.push_back(next("a definition name"));
    }

    expect('}', "'}' to close the definition name");

    // {,n} is a count missing its lower bound, not a name; say so rather than report an unknown definition.
    if (name.starts_with(','))
    {
        at_ = open;

        fail("a count needs its lower bound");
    }

    // {-} and {+} are flex's class operators, which combine bracket expressions and are read after one.
    if (name == "-" || name == "+")
    {
        at_ = open;

        const auto message{std::format(
                "flex's class {} '{{{}}}' combines bracket expressions, and none stands before it",
                name == "-" ? "difference" : "union", name)};

        fail(message);
    }

    const auto found{definitions.find(name)};

    if (found == definitions.end())
    {
        at_ = open;

        const auto message{std::format("unknown definition '{}'", name)};

        fail(message);
    }

    if (std::ranges::contains(expanding_, name))
    {
        at_ = open;

        const auto message{std::format("definition '{}' expands to itself", name)};

        fail(message);
    }

    const auto& [defined, text]{*found};

    // flex splices a definition opening with '^' or closing with '$' without the parentheses it gives every other
    // expansion, so the definition's edge becomes the rule's, where a '^' anchors it and a '$' is its trailing context,
    // and no postfix operator after the reference reaches the whole expansion.
    if (text.starts_with('^') || text.ends_with('$'))
    {
        at_ = open;

        const auto edge{text.starts_with('^') ? "opens with '^'" : "closes with '$'"};

        const auto message{std::format(
                "definition '{}' {}, which flex splices bare, so the reference is no atom and its edge is the rule's",
                name, edge)};

        fail(message);
    }

    auto expanding{expanding_};

    expanding.push_back(name);

    try
    {
        Reader reader{text, std::move(expanding), options_};

        return reader.read(definitions);
    }
    catch (const Syntax_error& inner)
    {
        const auto message{std::format("in definition '{}' at offset {}: {}", name, inner.offset(), inner.what())};

        throw Syntax_error{message, open};
    }
}

bool Reader::expansion() const noexcept
{
    return !expanding_.empty();
}

std::optional<std::size_t> Reader::number()
{
    if (!peek() || !is_digit(*peek()))
    {
        return std::nullopt;
    }

    std::size_t value{0};

    while (peek() && is_digit(*peek()))
    {
        const auto digit{static_cast<std::size_t>(next("a digit") - '0')};

        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U)
        {
            fail("the count does not fit");
        }

        value = value * 10U + digit;
    }

    return value;
}

std::pair<char32_t, std::size_t> Reader::digits(const int base, const std::size_t limit)
{
    const auto text{pattern_.substr(at_, limit)};

    std::uint32_t value{0};

    const auto [end, error]{std::from_chars(text.data(), text.data() + text.size(), value, base)};

    const auto count{static_cast<std::size_t>(end - text.data())};

    at_ += count;

    return {static_cast<char32_t>(value), count};
}

std::optional<char> Reader::peek() const noexcept
{
    if (at_ >= pattern_.size())
    {
        return std::nullopt;
    }

    return pattern_[at_];
}

char Reader::next(const std::string_view what)
{
    if (at_ >= pattern_.size())
    {
        fail_at_end(what);
    }

    const auto byte{pattern_[at_]};

    ++at_;

    return byte;
}

bool Reader::accept(const char byte) noexcept
{
    if (peek() != byte)
    {
        return false;
    }

    ++at_;

    return true;
}

void Reader::expect(const char byte, const std::string_view what)
{
    if (accept(byte))
    {
        return;
    }

    // A refusal names what was found as well as what the syntax admits, so the byte stands in the message unless the
    // pattern has ended before it.
    if (const auto found{peek()})
    {
        const auto message{std::format("expected {}, got '{}'", what, *found)};

        fail(message);
    }

    fail_at_end(what);
}

void Reader::fail(const std::string_view message) const
{
    throw Syntax_error{message, at_};
}

void Reader::fail_at_end(const std::string_view what) const
{
    const auto message{std::format("expected {} before the end of the pattern", what)};

    fail(message);
}

} // namespace

Syntax_error::Syntax_error(const std::string_view message, const std::size_t offset)
    : std::invalid_argument{std::format("{} at offset {}", message, offset)}, offset_{offset}
{}

std::size_t Syntax_error::offset() const noexcept
{
    return offset_;
}

Regex parse(const std::string_view pattern, const Definitions_t& definitions, const Parse_options options)
{
    Reader reader{pattern, {}, options};

    return reader.read(definitions);
}

} // namespace munch::regex
