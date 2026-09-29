#include "munch/tools/audit/logos_pattern.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "munch/regex/utf8.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements logos_pattern.hpp: the classes' tables and the builders that flatten a concatenation, an alternation and a
// repetition as the crate builds them are private to this unit.

using regex::utf8::Code_point_range;

// What logos's `\d`, `\s` and `\w` admit: the tables of the Unicode version that the regex-syntax logos is locked to
// was generated from. They are part of the token set a logos scanner has, whatever version the library itself pins, so
// the audit carries its own copy of them and tools/unicode/generate_classes.py generates both.
#include "logos_class_ranges.inc"

/**
 * @brief A concatenation of parts, flattened, the empty parts dropped and adjacent literal runs merged unless a capture
 *        bounds one, which is how the crate builds one.
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
 * @brief An alternation of branches, flattened as the crate flattens it, an empty branch making the rest optional,
 *        which says the same.
 *
 * The crate builds an alternation bottom up, so a nested one it has merged into a class, or wrapped in a capture, is
 * one branch to the outer alternation, while any other nested alternation is flattened into it (regex-syntax 0.8.11,
 * hir/mod.rs, `Hir::alternation`); which is which decides whether the outer one merges in turn.
 * @param branches The branches.
 * @return The node: Empty for none, the branch for one, an Alternation otherwise, under `?` when a branch was empty.
 */
[[nodiscard]] Node alternated(std::vector<Node> branches)
{
    std::vector<Node> flat;

    auto nullable{false};

    for (auto& branch : branches)
    {
        if (std::holds_alternative<Alternation>(branch.kind) && !std::get<Alternation>(branch.kind).captured &&
            !merged(branch))
        {
            std::ranges::move(std::get<Alternation>(branch.kind).branches, std::back_inserter(flat));
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

    Node node{flat.size() == 1 ? std::move(flat.front()) : Node{.kind = Alternation{.branches = std::move(flat)}}};

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

} // namespace

std::string_view unicode_classes_version() noexcept
{
    return class_unicode_version;
}

Pattern_reader::Pattern_reader(String_literal literal, const Flags flags, const std::size_t line)
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

        // Only an empty group or a flag setting stands before the operator: the crate refuses the second and logos has
        // no use for the first, so both are refused rather than read as repeating nothing.
        if (std::holds_alternative<Empty>(node.kind))
        {
            fail("a repetition operator needs something before it to repeat");
        }

        // logos 0.15.1 refuses the lazy forms outright (logos-codegen 0.15.1, mir.rs), so they are refused here too
        // rather than read as the greedy form the crate never compiles.
        if (accept('?'))
        {
            fail("the lazy operator is one logos 0.15.1 refuses: non-greedy parsing is currently unsupported");
        }

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

        auto members{flags_.dot_all ? universe() : universe().minus(newline)};

        check_utf8(members);

        return {.kind = Char_class{.set = std::move(members), .unicode = flags_.unicode}};
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

        return {.kind = Reference{.name = std::move(name), .flags = flags_}};
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
    else if (std::holds_alternative<Char_class>(node.kind))
    {
        // The crate compares a repetition's operand with the dot before it strips captures, so a captured class is
        // never the dot it refuses, nor is a captured alternation it would merge into one.
        std::get<Char_class>(node.kind).captured = true;
    }
    else if (std::holds_alternative<Alternation>(node.kind))
    {
        std::get<Alternation>(node.kind).captured = true;
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

    check_utf8(members);

    if (const auto one{members.single()})
    {
        return {.kind =
                        Bytes{.bytes = flags_.unicode ? encoded(*one) : std::string(1, static_cast<char>(*one)),
                              .bounded = false}};
    }

    return {.kind = Char_class{.set = std::move(members), .unicode = flags_.unicode}};
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

            auto inner{negated ? universe().minus(cased(nested)) : nested};

            // The crate checks every bracket as it translates it, a nested one included, so a nested negation that
            // reaches beyond ASCII outside Unicode mode is refused though the outer class may not.
            check_utf8(inner);

            members.add(std::move(inner));
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

Scalar_set Pattern_reader::universe() const
{
    return universe_of(flags_.unicode);
}

Scalar_set Pattern_reader::cased(const Scalar_set& members) const
{
    return flags_.insensitive ? folded(members, flags_.unicode) : members;
}

void Pattern_reader::check_utf8(const Scalar_set& members) const
{
    if (!flags_.unicode && flags_.utf8 && !members.empty() && members.ranges().back().second > 0x7F)
    {
        fail("a byte beyond ASCII outside Unicode mode can match invalid UTF-8, which the regex crate refuses in a "
             "string pattern, logos 0.15.1 having no option to turn that check off");
    }
}

std::variant<Pattern_reader::Unit, Scalar_set> Pattern_reader::escape()
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
        const auto negated{byte == 'D' || byte == 'S' || byte == 'W'};

        const auto kind{static_cast<char>(byte | 0x20)};

        Scalar_set members;

        if (flags_.unicode)
        {
            // The crate's Unicode forms: Nd, White_Space and the word class, from the tables of the database the locked
            // regex-syntax was generated from, which is the language the scanner has rather than the one the library
            // pins; the two databases differ by ten digits and thousands of word characters.
            const std::span<const Code_point_range> ranges{
                    kind == 'd' ? std::span<const Code_point_range>{decimal_digit_ranges} :
                    kind == 's' ? std::span<const Code_point_range>{white_space_ranges} :
                                  std::span<const Code_point_range>{word_ranges}};

            for (const auto& [first, last] : ranges)
            {
                members.add(first, last);
            }
        }
        else
        {
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
        }

        return negated ? universe().minus(members) : members;
    }
    case 'p':
    case 'P':
        fail("a Unicode property class needs tables the library has not got: only the digit, space and word classes "
             "are supplied");
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

Pattern_reader::Unit Pattern_reader::hex_escape(const char kind)
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

    if (!byte && (value > last_scalar || is_surrogate(value)))
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

    // The crate refuses a byte beyond ASCII where it is written, before the class it stands in is negated.
    if (unit.byte)
    {
        Scalar_set one;

        one.add(unit.value, unit.value);

        check_utf8(one);
    }

    return unit.value;
}

Pattern_reader::Unit Pattern_reader::next_unit()
{
    const auto lead{static_cast<unsigned char>(next("a character"))};

    // A byte string's bytes reach the crate as `\xHH`, bytes outside Unicode mode and scalars inside it.
    if (literal_.byte_string || lead < 0x80)
    {
        return {.value = lead, .byte = !flags_.unicode};
    }

    const auto length{sequence_length(lead)};

    auto value{lead_bits(lead)};

    for (std::size_t index{1}; index < length; ++index)
    {
        value = (value << 6U) | (static_cast<unsigned char>(next("a UTF-8 continuation byte")) & 0x3FU);
    }

    return {.value = value, .byte = false};
}

Node Pattern_reader::unit_node(const Unit unit)
{
    const auto [value, byte]{unit};

    if (flags_.insensitive && is_letter(value))
    {
        Scalar_set letter;

        letter.add(value, value);

        return {.kind = Char_class{.set = folded(letter, flags_.unicode), .unicode = flags_.unicode}};
    }

    if (flags_.insensitive && flags_.unicode && value > 0x7F)
    {
        fail("the case folding of a non-ASCII scalar under (?i) is not modelled");
    }

    if (byte)
    {
        Scalar_set one;

        one.add(value, value);

        check_utf8(one);
    }

    return {.kind = Bytes{.bytes = byte ? std::string(1, static_cast<char>(value)) : encoded(value), .bounded = false}};
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

} // namespace munch::tools::audit
