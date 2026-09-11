#include "munch/regex/parse.hpp"

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
     * @brief Binds the reader to a pattern and the definitions it may expand.
     * @param pattern The pattern.
     * @param definitions The named patterns.
     * @param expanding The names whose definitions are being expanded above this reader, outermost first.
     */
    Reader(std::string_view pattern, const Definitions_t& definitions, std::vector<std::string> expanding);

    /**
     * @brief Reads the whole pattern.
     * @return The regex.
     * @throws Syntax_error If anything is left over or the pattern is refused.
     */
    [[nodiscard]] Regex read();

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
     * @return The regex, a choice when there is more than one sequence.
     */
    [[nodiscard]] Regex alternation();

    /**
     * @brief A sequence: repetitions up to a `|`, a `)` or the end, adjacent literals merged into one text node.
     * @return The regex.
     * @throws Syntax_error If the sequence is empty, which the standard refuses.
     */
    [[nodiscard]] Regex sequence();

    /**
     * @brief An atom under its postfix operators.
     * @return The piece.
     */
    [[nodiscard]] Piece repetition();

    /**
     * @brief One atom: a group, a bracket expression, a quoted literal, a definition, the dot, an escape or a byte.
     * @return The piece, a literal for a plain or escaped byte.
     * @throws Syntax_error For an anchor, trailing context, a start condition, or an operator with nothing before it.
     */
    [[nodiscard]] Piece atom();

    /**
     * @brief A bracket expression after its `[`, through its `]`.
     * @return The set it names, complemented when it began with `^`.
     */
    [[nodiscard]] Set bracket();

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
     * @brief An escape after its backslash: a named control, octal, hex, or the byte itself.
     * @return The byte.
     */
    [[nodiscard]] char escape();

    /**
     * @brief A counted repetition after its `{`, through its `}`.
     * @return The minimum and, when bounded, the maximum.
     * @throws Syntax_error If the bounds are missing, reversed, or too large to hold.
     */
    [[nodiscard]] std::pair<std::size_t, std::optional<std::size_t>> count();

    /**
     * @brief A definition reference after its `{`, through its `}`, expanded by a reader over its text.
     * @param open The offset of the `{`, where a fault inside the definition is reported.
     * @return The definition's regex.
     * @throws Syntax_error If the name is unknown or its expansion cycles.
     */
    [[nodiscard]] Regex definition(std::size_t open);

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
     * @brief The named patterns `{name}` may expand to.
     */
    const Definitions_t& definitions_;

    /**
     * @brief The names being expanded above this reader, so that a definition naming one of them is a cycle.
     */
    std::vector<std::string> expanding_;

    /**
     * @brief The offset of the next byte to read.
     */
    std::size_t at_{0};
};

/**
 * @brief Whether a byte is a hexadecimal digit, tested directly so no locale is consulted.
 * @param byte The byte.
 * @return True for 0 to 9, a to f and A to F.
 */
[[nodiscard]] constexpr bool is_hex_digit(const char byte) noexcept
{
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

Reader::Reader(const std::string_view pattern, const Definitions_t& definitions, std::vector<std::string> expanding)
    : pattern_{pattern}, definitions_{definitions}, expanding_{std::move(expanding)}
{}

Regex Reader::read()
{
    auto regex{alternation()};

    if (peek())
    {
        fail(std::string{"unexpected '"} + *peek() + "'");
    }

    return regex;
}

Regex Reader::alternation()
{
    std::vector<Regex> branches;

    branches.push_back(sequence());

    while (accept('|'))
    {
        branches.push_back(sequence());
    }

    if (branches.size() == 1)
    {
        return std::move(branches.front());
    }

    return {.node = Choice{.regexes = std::move(branches)}};
}

Regex Reader::sequence()
{
    std::vector<Regex> parts;

    std::string run;

    const auto flush{[&parts, &run] {
        if (!run.empty())
        {
            parts.push_back(text(std::exchange(run, {})));
        }
    }};

    while (peek() && *peek() != '|' && *peek() != ')')
    {
        auto [literal, regex]{repetition()};

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

Piece Reader::repetition()
{
    auto piece{atom()};

    // A literal stays one until an operator claims it; the sequence merges the ones that stay.
    const auto claim{[&piece] {
        if (piece.literal)
        {
            piece.regex = text(std::string(1, *piece.literal));

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

Piece Reader::atom()
{
    const auto open{at_};

    const auto byte{next("a pattern")};

    switch (byte)
    {
    case '(':
    {
        auto inner{alternation()};

        expect(')', "')' to close the group");

        return {.literal = std::nullopt, .regex = std::move(inner)};
    }
    case '[':
        return {.literal = std::nullopt, .regex = any_of(bracket())};
    case '"':
    {
        auto literal{quoted()};

        if (literal.size() == 1)
        {
            return {.literal = literal.front(), .regex = std::nullopt};
        }

        return {.literal = std::nullopt, .regex = text(std::move(literal))};
    }
    case '{':
        return {.literal = std::nullopt, .regex = definition(open)};
    case '.':
        return {.literal = std::nullopt, .regex = any_of(Set::all() - Set{'\n'})};
    case '\\':
        return {.literal = escape(), .regex = std::nullopt};
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

Set Reader::bracket()
{
    const auto negated{accept('^')};

    Set set;

    // A ']' first is a member rather than the close, as the standard has it.
    auto first{true};

    for (;;)
    {
        const auto open{at_};

        auto byte{next("']' to close the bracket expression")};

        if (byte == ']' && !first)
        {
            break;
        }

        first = false;

        if (byte == '[' && accept(':'))
        {
            set += posix_class();

            continue;
        }

        if (byte == '\\')
        {
            byte = escape();
        }

        // A '-' between two members is a range; at either edge it is itself.
        if (peek() == '-' && at_ + 1 < pattern_.size() && pattern_[at_ + 1] != ']')
        {
            ++at_;

            auto last{next("the end of the range")};

            if (last == '\\')
            {
                last = escape();
            }

            if (static_cast<unsigned char>(last) < static_cast<unsigned char>(byte))
            {
                at_ = open;

                fail("the range ends before it starts");
            }

            set += Set::range(byte, last);

            continue;
        }

        set += byte;
    }

    return negated ? Set::all() - set : set;
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
        const auto byte{next("'\"' to close the quoted text")};

        if (byte == '"')
        {
            break;
        }

        literal.push_back(byte == '\\' ? escape() : byte);
    }

    if (literal.empty())
    {
        fail("empty quoted text matches nothing");
    }

    return literal;
}

char Reader::escape()
{
    const auto byte{next("the escaped byte")};

    switch (byte)
    {
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case 'r':
        return '\r';
    case 'f':
        return '\f';
    case 'v':
        return '\v';
    case 'a':
        return '\a';
    case 'b':
        return '\b';
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
            fail("'\\x' needs a hex digit");
        }

        return static_cast<char>(value);
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

        return static_cast<char>(value);
    }

    return byte;
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

Regex Reader::definition(const std::size_t open)
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

    const auto found{definitions_.find(name)};

    if (found == definitions_.end())
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
        return Reader{found->second, definitions_, std::move(expanding)}.read();
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

Regex parse(const std::string_view pattern, const Definitions_t& definitions)
{
    return Reader{pattern, definitions, {}}.read();
}

} // namespace munch::regex
