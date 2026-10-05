#include "munch/tools/audit/logos_pattern.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
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
using regex::utf8::Code_point_range;

// What logos's `\d`, `\s` and `\w` admit: the tables of the Unicode version that the regex-syntax logos is locked to
// was generated from. They are part of the token set a logos scanner has, whatever version the library itself pins, so
// the audit carries its own copy of them and tools/unicode/generate_classes.py generates both.
#include "logos_class_ranges.inc"

/**
 * @brief One run of an ASCII class, its first scalar and its last.
 */
struct Ascii_run
{
    /**
     * @brief The run's first scalar.
     */
    char32_t low{};

    /**
     * @brief Its last.
     */
    char32_t high{};
};

/**
 * @brief An ASCII class of the regex crate, `[:name:]`, as the runs it admits.
 */
struct Ascii_class
{
    /**
     * @brief The class's name.
     */
    std::string_view name{};

    /**
     * @brief The runs, ascending, the ones past the count left empty.
     */
    std::array<Ascii_run, 4> runs{};

    /**
     * @brief How many runs the class has.
     */
    std::size_t count{};
};

/**
 * @brief The regex crate's ASCII classes.
 */
constexpr std::array<Ascii_class, 14> ascii_classes{
        Ascii_class{
                .name = "alnum",
                .runs = {{{.low = '0', .high = '9'}, {.low = 'A', .high = 'Z'}, {.low = 'a', .high = 'z'}}},
                .count = 3},
        Ascii_class{.name = "alpha", .runs = {{{.low = 'A', .high = 'Z'}, {.low = 'a', .high = 'z'}}}, .count = 2},
        Ascii_class{.name = "ascii", .runs = {{{.low = 0, .high = last_ascii}}}, .count = 1},
        Ascii_class{.name = "blank", .runs = {{{.low = '\t', .high = '\t'}, {.low = ' ', .high = ' '}}}, .count = 2},
        Ascii_class{
                .name = "cntrl",
                .runs = {{{.low = 0, .high = 0x1F}, {.low = last_ascii, .high = last_ascii}}},
                .count = 2},
        Ascii_class{.name = "digit", .runs = {{{.low = '0', .high = '9'}}}, .count = 1},
        Ascii_class{.name = "graph", .runs = {{{.low = '!', .high = '~'}}}, .count = 1},
        Ascii_class{.name = "lower", .runs = {{{.low = 'a', .high = 'z'}}}, .count = 1},
        Ascii_class{.name = "print", .runs = {{{.low = ' ', .high = '~'}}}, .count = 1},
        Ascii_class{
                .name = "punct",
                .runs =
                        {{{.low = '!', .high = '/'},
                          {.low = ':', .high = '@'},
                          {.low = '[', .high = '`'},
                          {.low = '{', .high = '~'}}},
                .count = 4},
        Ascii_class{.name = "space", .runs = {{{.low = '\t', .high = '\r'}, {.low = ' ', .high = ' '}}}, .count = 2},
        Ascii_class{.name = "upper", .runs = {{{.low = 'A', .high = 'Z'}}}, .count = 1},
        Ascii_class{
                .name = "word",
                .runs =
                        {{{.low = '0', .high = '9'},
                          {.low = 'A', .high = 'Z'},
                          {.low = '_', .high = '_'},
                          {.low = 'a', .high = 'z'}}},
                .count = 4},
        Ascii_class{
                .name = "xdigit",
                .runs = {{{.low = '0', .high = '9'}, {.low = 'A', .high = 'F'}, {.low = 'a', .high = 'f'}}},
                .count = 3}};

/**
 * @brief Returns the class of one scalar.
 * @param scalar The scalar.
 * @return The class holding it alone.
 */
[[nodiscard]] Scalar_set one_scalar(const char32_t scalar)
{
    Scalar_set set{};

    set.add(scalar, scalar);

    return set;
}

/**
 * @brief Returns a scalar's bytes in a pattern: the one byte it is, or its UTF-8.
 * @param scalar The scalar.
 * @param as_byte Whether it stands for a byte, outside Unicode mode or as a byte string's.
 * @return The bytes.
 */
[[nodiscard]] std::string scalar_bytes(const char32_t scalar, const bool as_byte)
{
    return as_byte ? std::string{static_cast<char>(scalar)} : regex::utf8::encode(scalar);
}

/**
 * @brief Returns the members of an ASCII class.
 * @param ascii The class.
 * @return The members, its runs.
 */
[[nodiscard]] Scalar_set class_members(const Ascii_class& ascii)
{
    const auto& [name, runs, count]{ascii};

    Scalar_set set{};

    for (const auto& [low, high] : runs | std::views::take(count))
    {
        set.add(low, high);
    }

    return set;
}

/**
 * @brief Returns a concatenation of parts, flattened, the empty parts dropped and adjacent literal runs merged unless a
 *        capture bounds one, which is how the crate builds one.
 * @param parts The parts.
 * @return The node: Empty for none, the part for one, a Concat otherwise.
 */
[[nodiscard]] Node concatenated(std::vector<Node> parts)
{
    std::vector<Node> flat{};

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

    std::vector<Node> merged{};

    // A literal run no capture bounds merges with its neighbours of the same kind.
    const auto unbounded_bytes{[](const Node& node) {
        return std::holds_alternative<Bytes>(node.kind) && !std::get<Bytes>(node.kind).bounded;
    }};

    for (auto& part : flat)
    {
        const auto joins{!merged.empty() && unbounded_bytes(merged.back()) && unbounded_bytes(part)};

        if (!joins)
        {
            merged.push_back(std::move(part));

            continue;
        }

        auto& joined{std::get<Bytes>(merged.back().kind)};

        const auto& [bytes, bounded]{std::get<Bytes>(part.kind)};

        joined.bytes += bytes;
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
 * @brief Returns an alternation of branches, flattened as the crate flattens it, an empty branch making the rest
 *        optional, which says the same.
 *
 * The crate builds an alternation bottom up, so a nested one it has merged into a class, or wrapped in a capture, is
 * one branch to the outer alternation, while any other nested alternation is flattened into it (regex-syntax 0.8.11,
 * hir/mod.rs, `Hir::alternation`); which is which decides whether the outer one merges in turn.
 * @param branches The branches.
 * @return The node: Empty for none, the branch for one, an Alternation otherwise, under `?` when a branch was empty.
 */
[[nodiscard]] Node alternated(std::vector<Node> branches)
{
    std::vector<Node> flat{};

    auto nullable{false};

    const auto flattens{[](const Node& branch) {
        return std::holds_alternative<Alternation>(branch.kind) && !std::get<Alternation>(branch.kind).captured &&
               !merged(branch);
    }};

    for (auto& branch : branches)
    {
        if (flattens(branch))
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
 * @brief Returns a repetition of an operand, the forms that repeat nothing reduced.
 * @param operand The operand.
 * @param min The least number of times.
 * @param max The greatest number of times, std::nullopt when unbounded.
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
        fail(std::format("unexpected '{}'", *peek()));
    }

    return node;
}

Node Pattern_reader::read_literal()
{
    std::vector<Node> parts{};

    while (peek())
    {
        const auto unit{next_unit()};

        parts.push_back(unit_node(unit));
    }

    return concatenated(std::move(parts));
}

Node Pattern_reader::alternation()
{
    std::vector<Node> branches{};

    branches.push_back(concatenation());

    while (accept('|'))
    {
        branches.push_back(concatenation());
    }

    return alternated(std::move(branches));
}

Node Pattern_reader::concatenation()
{
    std::vector<Node> parts{};

    while (peek() && *peek() != '|' && *peek() != ')')
    {
        parts.push_back(repetition());
    }

    return concatenated(std::move(parts));
}

Node Pattern_reader::repetition()
{
    auto node{atom()};

    const auto bounds{[this]() -> std::optional<Count> {
        if (accept('*'))
        {
            return Count{.min = 0, .max = std::nullopt};
        }

        if (accept('+'))
        {
            return Count{.min = 1, .max = std::nullopt};
        }

        if (accept('?'))
        {
            return Count{.min = 0, .max = 1};
        }

        if (accept('{'))
        {
            return count();
        }

        return std::nullopt;
    }};

    for (;;)
    {
        const auto taken{bounds()};

        if (!taken)
        {
            return node;
        }

        const auto [min, max]{*taken};

        // Only an empty group or a flag setting stands before the operator: the crate refuses the second and logos has
        // no use for the first, so both are refused.
        if (std::holds_alternative<Empty>(node.kind))
        {
            fail("a repetition operator needs something before it to repeat");
        }

        // logos 0.15.1 refuses the lazy forms outright (logos-codegen 0.15.1, mir.rs), so they are refused here too.
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
        const auto newline{one_scalar('\n')};

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

        fail(std::format("nothing for '{}' to repeat", byte));
    case ')':
        --at_;

        fail("unexpected ')'");
    case '^':
    case '$':
        --at_;

        fail(std::format(
                "the anchor '{}' conditions the context a match stands in, which a token language cannot say", byte));
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
        std::string name{};

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

    return flag_group(saved);
}

Node Pattern_reader::flag_group(const Flags& saved)
{
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
            fail(std::format("the flag '{}' is not modelled", flag));
        default:
            fail(std::format("'{}' is not a flag of the regex crate", flag));
        }
    }
}

Node Pattern_reader::captured()
{
    auto node{alternation()};

    expect(')', "')' to close the group");

    // No run outside the capture merges with a bounded run.
    const auto bound{[](Node& edge) {
        if (std::holds_alternative<Bytes>(edge.kind))
        {
            std::get<Bytes>(edge.kind).bounded = true;
        }
    }};

    if (std::holds_alternative<Concat>(node.kind))
    {
        auto& parts{std::get<Concat>(node.kind).parts};

        bound(parts.front());

        bound(parts.back());
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

Node Pattern_reader::class_node(Scalar_set admitted, const bool negated)
{
    if (flags_.insensitive)
    {
        const auto foldable_run{[](const Scalar_set::Range_t& range) {
            const auto& [low, high]{range};

            return high <= last_ascii || (low == high && (low == long_s || low == kelvin));
        }};

        // The crate folds before it negates; folding is modelled for ASCII and the two scalars that fold to it.
        const auto foldable{std::ranges::all_of(admitted.ranges(), foldable_run)};

        if (flags_.unicode && !foldable)
        {
            fail("the case folding of a class with non-ASCII members under (?i) is not modelled");
        }
    }

    const auto cased_members{cased(admitted)};

    admitted = negated_if(cased_members, negated);

    if (admitted.empty())
    {
        fail("the class matches nothing");
    }

    check_utf8(admitted);

    if (const auto one{admitted.single()})
    {
        auto bytes{scalar_bytes(*one, !flags_.unicode)};

        return {.kind = Bytes{.bytes = std::move(bytes), .bounded = false}};
    }

    return {.kind = Char_class{.set = std::move(admitted), .unicode = flags_.unicode}};
}

Scalar_set Pattern_reader::members()
{
    Scalar_set admitted{};

    const auto opens_range{[this] {
        const auto after{at_ + 1 < literal_.bytes.size() ? literal_.bytes[at_ + 1] : ']'};

        return peek() == '-' && after != ']' && after != '-';
    }};

    const auto escaped_member{[this, &admitted, &opens_range]() -> std::optional<char32_t> {
        auto escaped{escape()};

        if (std::holds_alternative<Unit>(escaped))
        {
            return member(std::get<Unit>(escaped));
        }

        admitted.add(std::get<Scalar_set>(escaped));

        if (opens_range())
        {
            fail("a range cannot start with a class");
        }

        return std::nullopt;
    }};

    const auto at_operator{[this](const std::string_view written) { return at(written); }};

    // Dashes first are members, and a ']' first is one too, as the crate has it.
    auto first{true};

    while (accept('-'))
    {
        admitted.add('-', '-');

        first = false;
    }

    if (first && accept(']'))
    {
        admitted.add(']', ']');
    }

    for (;;)
    {
        if (!peek())
        {
            fail("expected ']' to close the class before the end of the pattern");
        }

        if (accept(']'))
        {
            return admitted;
        }

        static constexpr std::array<std::string_view, 3> class_operators{"&&", "--", "~~"};

        const auto written{std::ranges::find_if(class_operators, at_operator)};

        if (written != class_operators.end())
        {
            fail(std::format("the class operator '{}' is not modelled", *written));
        }

        std::optional<char32_t> low{};

        static constexpr std::string_view class_opener{"[:"};

        if (at(class_opener))
        {
            at_ += class_opener.size();

            admitted.add(posix_class());
        }
        else if (accept('['))
        {
            const auto negated{accept('^')};

            const auto nested{members()};

            // A nested class is folded only where it is negated, the crate folding before it negates.
            const auto read{negated ? cased(nested) : nested};

            auto inner{negated_if(read, negated)};

            // The crate checks every bracket as it translates it, a nested one included, so a nested negation that
            // reaches beyond ASCII outside Unicode mode is refused though the outer class may not.
            check_utf8(inner);

            admitted.add(std::move(inner));
        }
        else if (accept('\\'))
        {
            low = escaped_member();
        }
        else
        {
            const auto unit{next_unit()};

            low = member(unit);
        }

        if (!low)
        {
            continue;
        }

        if (!opens_range())
        {
            admitted.add(*low, *low);

            continue;
        }

        const auto high{range_end()};

        if (high < *low)
        {
            fail("the range ends before it starts");
        }

        admitted.add(*low, high);
    }
}

char32_t Pattern_reader::range_end()
{
    ++at_;

    if (peek() == '[')
    {
        fail("a range cannot end in a class");
    }

    if (!accept('\\'))
    {
        const auto unit{next_unit()};

        return member(unit);
    }

    auto escaped{escape()};

    if (!std::holds_alternative<Unit>(escaped))
    {
        fail("a range cannot end in a class");
    }

    return member(std::get<Unit>(escaped));
}

Scalar_set Pattern_reader::posix_class()
{
    const auto negated{accept('^')};

    std::string name{};

    while (peek() && *peek() != ':')
    {
        name.push_back(next("a class name"));
    }

    expect(':', "':]' to close the class");

    expect(']', "':]' to close the class");

    const auto found{std::ranges::find(ascii_classes, std::string_view{name}, &Ascii_class::name)};

    if (found == ascii_classes.end())
    {
        fail(std::format("'[:{}:]' is not an ASCII class of the regex crate", name));
    }

    const auto set{class_members(*found)};

    if (!negated)
    {
        return set;
    }

    const auto cased_set{cased(set)};

    return negated_if(cased_set, true);
}

Scalar_set Pattern_reader::universe() const
{
    return universe_of(flags_.unicode);
}

Scalar_set Pattern_reader::cased(const Scalar_set& members) const
{
    return flags_.insensitive ? folded(members, flags_.unicode) : members;
}

Scalar_set Pattern_reader::negated_if(const Scalar_set& members, const bool negated) const
{
    if (!negated)
    {
        return members;
    }

    return universe().minus(members);
}

void Pattern_reader::check_utf8(const Scalar_set& members) const
{
    if (flags_.unicode || !flags_.utf8 || members.empty())
    {
        return;
    }

    const auto [low, high]{members.ranges().back()};

    if (high > last_ascii)
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

        const auto kind{static_cast<char>(byte | case_bit)};

        const auto members{perl_class(kind)};

        return negated_if(members, negated);
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
        fail(std::format(
                "the assertion '\\{}' conditions the context a match stands in, which a token language cannot say",
                byte));
    default:
        break;
    }

    if (is_digit(byte))
    {
        fail("the regex crate has neither octal escapes nor backreferences");
    }

    if (is_letter(static_cast<unsigned char>(byte)) || static_cast<unsigned char>(byte) > last_ascii)
    {
        fail(std::format("'\\{}' is not an escape of the regex crate", byte));
    }

    return Unit{.value = static_cast<unsigned char>(byte), .byte = false};
}

Scalar_set Pattern_reader::perl_class(const char kind) const
{
    if (!flags_.unicode)
    {
        const auto name{[kind]() -> std::string_view {
            switch (kind)
            {
            case 'd':
                return "digit";
            case 's':
                return "space";
            default:
                return "word";
            }
        }()};

        const auto found{std::ranges::find(ascii_classes, name, &Ascii_class::name)};

        return class_members(*found);
    }

    // The tables are the database's the locked regex-syntax was generated from, which is the language the scanner has
    // rather than the one the library pins; the two databases differ by ten digits and thousands of word characters.
    const auto ranges{[kind]() -> std::span<const Code_point_range> {
        switch (kind)
        {
        case 'd':
            return decimal_digit_ranges;
        case 's':
            return white_space_ranges;
        default:
            return word_ranges;
        }
    }()};

    Scalar_set members{};

    for (const auto& [first, last] : ranges)
    {
        members.add(first, last);
    }

    return members;
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

            const auto digit{next("a hex digit")};

            value = value * 16 + hex_value(digit);
        }

        expect('}', "'}' to close the hex escape");
    }
    else
    {
        const auto wanted{[kind] {
            switch (kind)
            {
            case 'x':
                return 2UZ;
            case 'u':
                return 4UZ;
            default:
                return 8UZ;
            }
        }()};

        for (; digits < wanted; ++digits)
        {
            const auto digit{next("a hex digit")};

            if (!is_hex_digit(digit))
            {
                fail(std::format("'\\{}' needs {} hex digits, or braces", kind, wanted));
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
    const auto [value, byte]{unit};

    if (!byte && !flags_.unicode && value > last_ascii)
    {
        fail("a class outside Unicode mode holds bytes, not the encoding of a non-ASCII scalar");
    }

    // The crate refuses a byte beyond ASCII where it is written, before the class it stands in is negated.
    if (byte)
    {
        check_utf8(one_scalar(value));
    }

    return value;
}

Pattern_reader::Unit Pattern_reader::next_unit()
{
    const auto lead{static_cast<unsigned char>(next("a character"))};

    // A byte string's bytes reach the crate as `\xHH`, bytes outside Unicode mode and scalars inside it.
    if (literal_.byte_string || lead <= last_ascii)
    {
        return {.value = lead, .byte = !flags_.unicode};
    }

    const auto length{sequence_length(lead)};

    auto value{lead_bits(lead)};

    for (std::size_t index{1}; index < length; ++index)
    {
        const auto continuation{static_cast<unsigned char>(next("a UTF-8 continuation byte"))};

        value = continued(value, continuation);
    }

    return {.value = value, .byte = false};
}

Node Pattern_reader::unit_node(const Unit unit)
{
    const auto [value, byte]{unit};

    if (flags_.insensitive && is_letter(value))
    {
        const auto letter{one_scalar(value)};

        auto cases{folded(letter, flags_.unicode)};

        return {.kind = Char_class{.set = std::move(cases), .unicode = flags_.unicode}};
    }

    if (flags_.insensitive && flags_.unicode && value > last_ascii)
    {
        fail("the case folding of a non-ASCII scalar under (?i) is not modelled");
    }

    if (byte)
    {
        check_utf8(one_scalar(value));
    }

    auto bytes{scalar_bytes(value, byte)};

    return {.kind = Bytes{.bytes = std::move(bytes), .bounded = false}};
}

Pattern_reader::Count Pattern_reader::count()
{
    const auto min{number()};

    if (!min)
    {
        fail("a count needs a number after '{'");
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

        const auto appended{appended_digit(value, digit)};

        if (!appended)
        {
            fail("the count does not fit");
        }

        value = *appended;
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
        fail(std::format("expected {}", what));
    }
}

char Pattern_reader::next(const std::string_view what)
{
    if (at_ >= literal_.bytes.size())
    {
        fail(std::format("expected {} before the end of the pattern", what));
    }

    return literal_.bytes[at_++];
}

void Pattern_reader::fail(const std::string& message) const
{
    throw Spec_error{std::format("the pattern {} is refused: {}", literal_.written, message), line_};
}

} // namespace munch::tools::audit
