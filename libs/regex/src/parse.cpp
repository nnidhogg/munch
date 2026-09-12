#include "munch/regex/parse.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <set>
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
    std::optional<char> literal;

    /**
     * @brief The regex, when it is anything else.
     */
    std::optional<Regex> regex;
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
    char32_t value;

    /**
     * @brief Whether the value is a scalar rather than a byte.
     */
    bool scalar;
};

/**
 * @brief The POSIX bracket classes and the bytes each names, `[:alpha:]` and its kin.
 */
struct Posix_class
{
    /**
     * @brief The name between the colons.
     */
    std::string_view name;

    /**
     * @brief The bytes it stands for.
     */
    Set (*bytes)();
};

/**
 * @brief A recursive-descent reader over one pattern, producing the regex as it goes.
 *
 * The grammar is the usual one: an alternation of sequences, a sequence of repetitions, a repetition an atom under
 * zero or more postfix operators. Definitions are read by a fresh reader over the definition's own text, the chain of
 * names being expanded carried along so that a cycle is refused rather than followed.
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
     * @brief The POSIX classes a bracket expression may name.
     */
    static constexpr std::array<Posix_class, 12> classes{{
            {.name = "alpha", .bytes = [] { return Set::alpha(); }},
            {.name = "digit", .bytes = [] { return Set::digits(); }},
            {.name = "alnum", .bytes = [] { return Set::alphanum(); }},
            {.name = "upper", .bytes = [] { return Set::range('A', 'Z'); }},
            {.name = "lower", .bytes = [] { return Set::range('a', 'z'); }},
            {.name = "space", .bytes = [] { return Set{' ', '\t', '\n', '\r', '\f', '\v'}; }},
            {.name = "blank", .bytes = [] { return Set{' ', '\t'}; }},
            {.name = "punct", .bytes = [] { return Set::printable() - Set::alphanum() - Set{' '}; }},
            {.name = "print", .bytes = [] { return Set::printable(); }},
            {.name = "graph", .bytes = [] { return Set::printable() - Set{' '}; }},
            {.name = "cntrl", .bytes = [] { return Set::range('\x00', '\x1F') + Set{'\x7F'}; }},
            {.name = "xdigit", .bytes = [] { return Set::digits() + Set::range('a', 'f') + Set::range('A', 'F'); }},
    }};

    /**
     * @brief An alternation: sequences separated by `|`.
     * @param definitions The named patterns.
     * @return The regex, a choice when there is more than one sequence.
     */
    [[nodiscard]] Regex alternation(const Definitions_t& definitions);

    /**
     * @brief A sequence: repetitions up to a `|`, a `)` or the end, adjacent literals merged into one text node.
     * @param definitions The named patterns.
     * @return The regex.
     * @throws Syntax_error If the sequence is empty, which the standard refuses.
     */
    [[nodiscard]] Regex sequence(const Definitions_t& definitions);

    /**
     * @brief An atom under its postfix operators.
     * @param definitions The named patterns.
     * @return The piece.
     */
    [[nodiscard]] Piece repetition(const Definitions_t& definitions);

    /**
     * @brief One atom: a group, a bracket expression, a quoted literal, a definition, the dot, an escape or a byte.
     * @param definitions The named patterns.
     * @return The piece, a literal for a plain or escaped byte.
     * @throws Syntax_error For an anchor, trailing context, a start condition, or an operator with nothing before it.
     */
    [[nodiscard]] Piece atom(const Definitions_t& definitions);

    /**
     * @brief A bracket expression after its `[`, through its `]`.
     *
     * Over bytes unless a member is a code point escape, when every member is read as a scalar and the bracket
     * matches the UTF-8 encoding of one of them; negation then runs over the scalars rather than the bytes.
     * @return The regex it names: any_of over a set, or the encodings of the scalars.
     * @throws Syntax_error If a range is reversed, or bytes beyond ASCII stand beside code points.
     */
    [[nodiscard]] Regex bracket();

    /**
     * @brief A POSIX class after its `[:`, through its `:]`.
     * @return The bytes it names.
     * @throws Syntax_error If the name is not one of the twelve.
     */
    [[nodiscard]] Set posix_class();

    /**
     * @brief A double-quoted literal after its opening quote, through the closing one.
     * @return The decoded bytes.
     */
    [[nodiscard]] std::string quoted();

    /**
     * @brief The regex of a run of literal bytes: one text node, or under the caseless option the run with each
     *        letter widened to the set of its two cases and the bytes between letters kept as text.
     * @param run The bytes.
     * @return The regex.
     */
    [[nodiscard]] Regex literal(std::string run) const;

    /**
     * @brief An escape after its backslash: a named control, octal, hex, a code point `\u{...}`, or the byte itself.
     * @return The byte, or the scalar.
     * @throws Syntax_error If a hex or code point escape has no digits, or the code point is a surrogate or beyond
     *         U+10FFFF.
     */
    [[nodiscard]] Escaped escape();

    /**
     * @brief A counted repetition after its `{`, through its `}`.
     * @return The minimum and, when bounded, the maximum.
     * @throws Syntax_error If the bounds are missing, reversed, or too large to hold.
     */
    [[nodiscard]] std::pair<std::size_t, std::optional<std::size_t>> count();

    /**
     * @brief A definition reference after its `{`, through its `}`, expanded by a reader over its text.
     * @param open The offset of the `{`, where a fault inside the definition is reported.
     * @param definitions The named patterns.
     * @return The definition's regex.
     * @throws Syntax_error If the name is unknown or its expansion cycles.
     */
    [[nodiscard]] Regex definition(std::size_t open, const Definitions_t& definitions);

    /**
     * @brief The unsigned decimal at the current position.
     * @return The number, or std::nullopt when no digit stands here.
     * @throws Syntax_error If the number does not fit.
     */
    [[nodiscard]] std::optional<std::size_t> number();

    /**
     * @brief The byte at the current position, or nothing at the end.
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
     * @brief Consumes the byte given or refuses, naming what the syntax expected.
     * @param byte The byte required.
     * @param what What the syntax expected.
     */
    void expect(char byte, std::string_view what);

    /**
     * @brief Refuses the pattern at the current position.
     * @param message Why.
     */
    [[noreturn]] void fail(const std::string& message) const;

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
 * @brief The other case of an ASCII letter, or the byte itself when it is no letter, tested directly so no locale is
 *        consulted.
 * @param byte The byte.
 * @return The byte with its case swapped.
 */
[[nodiscard]] constexpr char swapped(const char byte) noexcept
{
    if (byte >= 'a' && byte <= 'z')
    {
        return static_cast<char>(byte - 'a' + 'A');
    }

    if (byte >= 'A' && byte <= 'Z')
    {
        return static_cast<char>(byte - 'A' + 'a');
    }

    return byte;
}

/**
 * @brief Whether a byte is a hexadecimal digit, tested directly so no locale is consulted.
 * @param byte The byte.
 * @return True for 0 to 9, a to f and A to F.
 */
[[nodiscard]] constexpr bool is_hex_digit(const char byte) noexcept
{
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

/**
 * @brief The UTF-8 encoding of a scalar.
 * @param scalar The scalar, at most U+10FFFF.
 * @return Its bytes.
 */
[[nodiscard]] std::string encoded(const char32_t scalar)
{
    std::string bytes;

    if (scalar < 0x80)
    {
        bytes.push_back(static_cast<char>(scalar));
    }
    else if (scalar < 0x800)
    {
        bytes.push_back(static_cast<char>(0xC0 | (scalar >> 6U)));
        bytes.push_back(static_cast<char>(0x80 | (scalar & 0x3FU)));
    }
    else if (scalar < 0x10000)
    {
        bytes.push_back(static_cast<char>(0xE0 | (scalar >> 12U)));
        bytes.push_back(static_cast<char>(0x80 | ((scalar >> 6U) & 0x3FU)));
        bytes.push_back(static_cast<char>(0x80 | (scalar & 0x3FU)));
    }
    else
    {
        bytes.push_back(static_cast<char>(0xF0 | (scalar >> 18U)));
        bytes.push_back(static_cast<char>(0x80 | ((scalar >> 12U) & 0x3FU)));
        bytes.push_back(static_cast<char>(0x80 | ((scalar >> 6U) & 0x3FU)));
        bytes.push_back(static_cast<char>(0x80 | (scalar & 0x3FU)));
    }

    return bytes;
}

/**
 * @brief The regex matching the UTF-8 encoding of one scalar from a set of ranges, the ranges sorted and merged
 *        first and the ones holding surrogates alone dropped, since no encoding has those.
 * @param ranges The ranges, in any order, possibly overlapping.
 * @return The regex, or std::nullopt when nothing remains.
 */
[[nodiscard]] std::optional<Regex> encodings(std::vector<utf8::Code_point_range> ranges)
{
    std::ranges::sort(ranges, {}, &utf8::Code_point_range::first);

    std::vector<utf8::Code_point_range> merged;

    for (const auto& [first, last] : ranges)
    {
        if (first >= 0xD800 && last <= 0xDFFF)
        {
            continue;
        }

        if (!merged.empty() && first <= merged.back().last + 1)
        {
            merged.back().last = std::max(merged.back().last, last);
        }
        else
        {
            merged.push_back({.first = first, .last = last});
        }
    }

    return merged.empty() ? std::nullopt : std::optional{utf8::ranges(merged)};
}

Reader::Reader(const std::string_view pattern, std::vector<std::string> expanding, const Parse_options options)
    : pattern_{pattern}, expanding_{std::move(expanding)}, options_{options}
{}

Regex Reader::read(const Definitions_t& definitions)
{
    auto regex{alternation(definitions)};

    if (peek())
    {
        fail(std::string{"unexpected '"} + *peek() + "'");
    }

    return regex;
}

Regex Reader::alternation(const Definitions_t& definitions)
{
    std::vector<Regex> branches;

    branches.push_back(sequence(definitions));

    while (accept('|'))
    {
        branches.push_back(sequence(definitions));
    }

    if (branches.size() == 1)
    {
        return std::move(branches.front());
    }

    return {.node = Choice{.regexes = std::move(branches)}};
}

Regex Reader::sequence(const Definitions_t& definitions)
{
    std::vector<Regex> parts;

    std::string run;

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

    if (parts.size() == 1)
    {
        return std::move(parts.front());
    }

    return {.node = Concat{.regexes = std::move(parts)}};
}

Piece Reader::repetition(const Definitions_t& definitions)
{
    auto piece{atom(definitions)};

    // A literal stays one until an operator claims it; the sequence merges the ones that stay.
    const auto claim{[this, &piece] {
        if (piece.literal)
        {
            piece.regex = literal({*piece.literal});

            piece.literal.reset();
        }
    }};

    for (;;)
    {
        if (accept('*'))
        {
            claim();

            piece.regex = kleene(std::move(*piece.regex));
        }
        else if (accept('+'))
        {
            claim();

            piece.regex = plus(std::move(*piece.regex));
        }
        else if (accept('?'))
        {
            claim();

            piece.regex = optional(std::move(*piece.regex));
        }
        else if (peek() == '{' && at_ + 1 < pattern_.size() && pattern_[at_ + 1] >= '0' && pattern_[at_ + 1] <= '9')
        {
            ++at_;

            const auto [min, max]{count()};

            claim();

            piece.regex = !max        ? at_least(std::move(*piece.regex), min) :
                          *max == min ? exact(std::move(*piece.regex), min) :
                                        range(std::move(*piece.regex), min, *max);
        }
        else
        {
            return piece;
        }
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
        // flex's `(?i:...)` and `(?-i:...)` set the case option inside the group alone; its other flags, `s` and
        // `x`, change what the dot and blanks mean and are not read.
        const auto saved{options_};

        if (accept('?'))
        {
            auto negated{false};

            for (auto flag{next("a flag or ':' after '(?'")}; flag != ':'; flag = next("a flag or ':' after '(?'"))
            {
                if (flag == '-')
                {
                    negated = true;
                }
                else if (flag == 'i')
                {
                    options_.caseless = !negated;
                }
                else
                {
                    --at_;

                    fail(std::string{"the group flag '"} + flag + "' is not read");
                }
            }
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
        auto literal{quoted()};

        if (literal.size() == 1)
        {
            return {.literal = literal.front(), .regex = std::nullopt};
        }

        return {.literal = std::nullopt, .regex = this->literal(std::move(literal))};
    }
    case '{':
        return {.literal = std::nullopt, .regex = definition(open, definitions)};
    case '.':
        return {.literal = std::nullopt, .regex = any_of(Set::all() - Set{'\n'})};
    case '\\':
    {
        const auto [value, scalar]{escape()};

        if (!scalar || value < 0x80)
        {
            return {.literal = static_cast<char>(value), .regex = std::nullopt};
        }

        return {.literal = std::nullopt, .regex = text(encoded(value))};
    }
    case '*':
    case '+':
    case '?':
        --at_;

        fail(std::string{"nothing for '"} + byte + "' to repeat");
    case ')':
        --at_;

        fail("unexpected ')'");
    case '^':
    case '$':
        // Anchors only where flex reads them as anchors, a '^' opening the pattern and a '$' closing it; a '$'
        // inside a pattern is the byte, which a Pascal hex literal or a shell variable spells.
        if ((byte == '^' && open == 0) || (byte == '$' && at_ == pattern_.size()))
        {
            --at_;

            fail(std::string{"the anchor '"} + byte +
                 "' conditions the context a match stands in, which a token language cannot say");
        }

        return {.literal = byte, .regex = std::nullopt};
    case '/':
        --at_;

        fail("trailing context conditions what follows a match, which a token language cannot say");
    case '<':
        --at_;

        fail("a start condition or <<EOF>> is the rule's, not the pattern's, and is read by the rule reader");
    default:
        return {.literal = byte, .regex = std::nullopt};
    }
}

Regex Reader::bracket()
{
    const auto opened{at_ - 1};

    const auto negated{accept('^')};

    Set set;

    // The members as scalars, kept beside the set until the bracket says which reading it takes.
    std::vector<utf8::Code_point_range> ranges;

    auto wide{false};

    auto byte_beyond_ascii{false};

    // A ']' first is a member rather than the close, as the standard has it.
    auto first{true};

    for (;;)
    {
        const auto open{at_};

        auto byte{static_cast<char32_t>(static_cast<unsigned char>(next("']' to close the bracket expression")))};

        if (byte == ']' && !first)
        {
            break;
        }

        first = false;

        if (byte == '[' && accept(':'))
        {
            const auto members{posix_class()};

            set += members;

            for (const auto value : members.symbols())
            {
                const auto scalar{static_cast<char32_t>(static_cast<unsigned char>(value))};

                ranges.push_back({.first = scalar, .last = scalar});
            }

            continue;
        }

        if (byte == '\\')
        {
            const auto [value, scalar]{escape()};

            byte = value;

            wide = wide || scalar;

            byte_beyond_ascii = byte_beyond_ascii || (!scalar && value >= 0x80);
        }
        else
        {
            byte_beyond_ascii = byte_beyond_ascii || byte >= 0x80;
        }

        auto last{byte};

        // A '-' between two members is a range; at either edge it is itself.
        if (peek() == '-' && at_ + 1 < pattern_.size() && pattern_[at_ + 1] != ']')
        {
            ++at_;

            last = static_cast<unsigned char>(next("the end of the range"));

            if (last == '\\')
            {
                const auto [value, scalar]{escape()};

                last = value;

                wide = wide || scalar;

                byte_beyond_ascii = byte_beyond_ascii || (!scalar && value >= 0x80);
            }
            else
            {
                byte_beyond_ascii = byte_beyond_ascii || last >= 0x80;
            }

            if (last < byte)
            {
                at_ = open;

                fail("the range ends before it starts");
            }
        }

        if (last < 0x100)
        {
            set += Set::range(static_cast<char>(byte), static_cast<char>(last));
        }

        ranges.push_back({.first = byte, .last = last});
    }

    // Under the caseless option both cases of every letter are members before any negation, as flex folds them.
    if (options_.caseless)
    {
        const Set letters{set};

        for (const auto symbol : letters.symbols())
        {
            if (swapped(symbol) != symbol)
            {
                const auto other{static_cast<char32_t>(static_cast<unsigned char>(swapped(symbol)))};

                set += swapped(symbol);

                ranges.push_back({.first = other, .last = other});
            }
        }
    }

    if (!wide)
    {
        return any_of(negated ? Set::all() - set : set);
    }

    // Read as scalars: a byte beyond ASCII is no scalar, so one beside a code point is refused.
    if (byte_beyond_ascii)
    {
        at_ = opened;

        fail("a bracket mixes bytes beyond ASCII with code points; write the bytes as code points");
    }

    if (negated)
    {
        std::vector<utf8::Code_point_range> complement;

        char32_t from{0};

        std::ranges::sort(ranges, {}, &utf8::Code_point_range::first);

        for (const auto& [low, high] : ranges)
        {
            if (low > from)
            {
                complement.push_back({.first = from, .last = low - 1});
            }

            from = std::max(from, static_cast<char32_t>(high + 1));
        }

        if (from <= 0x10FFFF)
        {
            complement.push_back({.first = from, .last = 0x10FFFF});
        }

        ranges = std::move(complement);
    }

    // The ASCII part as a set, the rest as encodings.
    Set ascii;

    std::vector<utf8::Code_point_range> beyond;

    for (const auto& [low, high] : ranges)
    {
        if (low < 0x80)
        {
            ascii += Set::range(static_cast<char>(low), static_cast<char>(std::min<char32_t>(high, 0x7F)));
        }

        if (high >= 0x80)
        {
            beyond.push_back({.first = std::max<char32_t>(low, 0x80), .last = high});
        }
    }

    auto rest{encodings(std::move(beyond))};

    if (ascii.symbols().empty())
    {
        if (!rest)
        {
            at_ = opened;

            fail("the bracket names no scalar");
        }

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

    std::string name;

    while (peek() && *peek() != ':')
    {
        name.push_back(next("a class name"));
    }

    expect(':', "':]' to close the class");
    expect(']', "':]' to close the class");

    for (const auto& [known, bytes] : classes)
    {
        if (known == name)
        {
            return bytes();
        }
    }

    at_ = open;

    fail("unknown class '" + name + "'");
}

std::string Reader::quoted()
{
    std::string literal;

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

        literal += scalar ? encoded(value) : std::string{static_cast<char>(value)};
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

    std::vector<Regex> pieces;

    std::string plain;

    for (const auto byte : run)
    {
        if (swapped(byte) == byte)
        {
            plain += byte;

            continue;
        }

        if (!plain.empty())
        {
            pieces.push_back(text(std::exchange(plain, {})));
        }

        pieces.push_back(any_of(Set{byte, swapped(byte)}));
    }

    if (pieces.empty())
    {
        return text(std::move(plain));
    }

    if (!plain.empty())
    {
        pieces.push_back(text(std::move(plain)));
    }

    if (pieces.size() == 1)
    {
        return std::move(pieces.front());
    }

    return {.node = Concat{.regexes = std::move(pieces)}};
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
        unsigned value{0};

        std::size_t digits{0};

        while (digits < 2 && peek() && is_hex_digit(*peek()))
        {
            const auto digit{next("a hex digit")};

            value = value * 16 + static_cast<unsigned>(
                                         digit >= 'a' ? digit - 'a' + 10 :
                                         digit >= 'A' ? digit - 'A' + 10 :
                                                        digit - '0');

            ++digits;
        }

        if (digits == 0)
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
            return {.value = static_cast<unsigned char>(byte), .scalar = false};
        }

        const auto open{at_ - 3};

        char32_t value{0};

        std::size_t digits{0};

        while (digits < 6 && peek() && is_hex_digit(*peek()))
        {
            const auto digit{next("a hex digit")};

            value = value * 16 + static_cast<char32_t>(
                                         digit >= 'a' ? digit - 'a' + 10 :
                                         digit >= 'A' ? digit - 'A' + 10 :
                                                        digit - '0');

            ++digits;
        }

        expect('}', "'}' to close the code point");

        if (digits == 0 || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
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
    if (byte >= '0' && byte <= '7')
    {
        unsigned value{static_cast<unsigned>(byte - '0')};

        for (std::size_t digits{1}; digits < 3 && peek() && *peek() >= '0' && *peek() <= '7'; ++digits)
        {
            value = value * 8 + static_cast<unsigned>(next("an octal digit") - '0');
        }

        if (value > std::numeric_limits<unsigned char>::max())
        {
            fail("the octal escape exceeds one byte");
        }

        return {.value = value, .scalar = false};
    }

    return {.value = static_cast<unsigned char>(byte), .scalar = false};
}

std::pair<std::size_t, std::optional<std::size_t>> Reader::count()
{
    const auto min{number()};

    if (!min)
    {
        fail("a count needs a number");
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

Regex Reader::definition(const std::size_t open, const Definitions_t& definitions)
{
    std::string name;

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

    const auto found{definitions.find(name)};

    if (found == definitions.end())
    {
        at_ = open;

        fail("unknown definition '" + name + "'");
    }

    for (const auto& outer : expanding_)
    {
        if (outer == name)
        {
            at_ = open;

            fail("definition '" + name + "' expands to itself");
        }
    }

    auto expanding{expanding_};

    expanding.push_back(name);

    try
    {
        return Reader{found->second, std::move(expanding), options_}.read(definitions);
    }
    catch (const Syntax_error& inner)
    {
        throw Syntax_error{
                "in definition '" + name + "' at offset " + std::to_string(inner.offset()) + ": " + inner.what(), open};
    }
}

std::optional<std::size_t> Reader::number()
{
    if (!peek() || *peek() < '0' || *peek() > '9')
    {
        return std::nullopt;
    }

    std::size_t value{0};

    while (peek() && *peek() >= '0' && *peek() <= '9')
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

std::optional<char> Reader::peek() const noexcept
{
    return at_ < pattern_.size() ? std::optional{pattern_[at_]} : std::nullopt;
}

char Reader::next(const std::string_view what)
{
    if (at_ >= pattern_.size())
    {
        fail("expected " + std::string{what} + " before the end of the pattern");
    }

    return pattern_[at_++];
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
    if (!accept(byte))
    {
        fail("expected " + std::string{what});
    }
}

void Reader::fail(const std::string& message) const
{
    throw Syntax_error{message, at_};
}

} // namespace

Syntax_error::Syntax_error(const std::string& message, const std::size_t offset)
    : std::invalid_argument{message + " at offset " + std::to_string(offset)}, offset_{offset}
{}

std::size_t Syntax_error::offset() const noexcept
{
    return offset_;
}

Regex parse(const std::string_view pattern, const Definitions_t& definitions, const Parse_options options)
{
    return Reader{pattern, {}, options}.read(definitions);
}

} // namespace munch::regex
