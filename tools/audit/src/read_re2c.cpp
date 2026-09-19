#include "munch/tools/audit/read_re2c.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "munch/regex/parse.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/regex/utf8.hpp"
#include "munch/tools/audit/cursor.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief Why a braced hexadecimal escape is refused.
 *
 * re2c's hexadecimal escape is `\xHH`, two digits and no braces, and re2c 3.1 answers `\x{...}` with "syntax error
 * in hexadecimal escape sequence" wherever it stands, in a class or in a literal, so a file holding one is no re2c
 * file and the code point the pattern parser would read there is nobody's.
 */
constexpr std::string_view braced_escape{
        "re2c's hexadecimal escape is a backslash, an x and two digits, and it takes no braces, so re2c answers this "
        "one with a syntax error in a hexadecimal escape sequence"};

/**
 * @brief The bytes a regex names when it is a class: a bracket, a one-byte literal, or an alternation of classes,
 *        which is what re2c lets the operands of its class difference be.
 * @param regex The regex.
 * @return The bytes, or std::nullopt when the regex is no class.
 */
[[nodiscard]] std::optional<regex::Set> class_of(const regex::Regex& regex)
{
    return std::visit(
            []<typename Node>(const Node& node) -> std::optional<regex::Set> {
                if constexpr (std::is_same_v<Node, regex::Any_of>)
                {
                    return node.set;
                }
                else if constexpr (std::is_same_v<Node, regex::Text>)
                {
                    return node.text.size() == 1 ? std::optional{regex::Set{node.text.front()}} : std::nullopt;
                }
                else if constexpr (std::is_same_v<Node, regex::Choice>)
                {
                    regex::Set all;

                    for (const auto& branch : node.regexes)
                    {
                        const auto bytes{class_of(branch)};

                        if (!bytes)
                        {
                            return std::nullopt;
                        }

                        all += *bytes;
                    }

                    return all;
                }
                else
                {
                    return std::nullopt;
                }
            },
            regex.node);
}

/**
 * @brief The bytes a regex spells when it is one run of text, which is what a literal is.
 * @param regex The regex.
 * @return The bytes, or std::nullopt when the regex is no run of text.
 */
[[nodiscard]] std::optional<std::string> text_of(const regex::Regex& regex)
{
    return std::visit(
            []<typename Node>(const Node& node) -> std::optional<std::string> {
                if constexpr (std::is_same_v<Node, regex::Text>)
                {
                    return node.text;
                }
                else
                {
                    return std::nullopt;
                }
            },
            regex.node);
}

/**
 * @brief An encoding's name as a refusal spells it.
 * @param encoding The encoding.
 * @return The name re2c's own option gives it.
 */
[[nodiscard]] std::string_view named(const Re2c_encoding encoding)
{
    switch (encoding)
    {
    case Re2c_encoding::ebcdic:
        return "ebcdic";
    case Re2c_encoding::ucs2:
        return "ucs2";
    case Re2c_encoding::utf8:
        return "utf8";
    case Re2c_encoding::utf16:
        return "utf16";
    case Re2c_encoding::utf32:
        return "utf32";
    default:
        return "ascii";
    }
}

/**
 * @brief Why an encoding is one no reading over bytes can follow, when it is: its own reason, named.
 * @param encoding The encoding.
 * @return The refusal's words, empty for the encodings the reading models, ASCII and UTF-8.
 */
[[nodiscard]] std::string unreadable(const Re2c_encoding encoding)
{
    switch (encoding)
    {
    case Re2c_encoding::ascii:
    case Re2c_encoding::utf8:
        return {};
    case Re2c_encoding::ebcdic:
        // One byte a code point, as ASCII has it, but not the same code point for the same byte.
        return "the ebcdic encoding gives a byte another code point than ASCII does, a mapping the reading has not "
               "got";
    default:
        return std::format(
                "the {} encoding's code unit is not one byte, so its scanner reads no byte stream the audit can "
                "model",
                named(encoding));
    }
}

/**
 * @brief The code points a class admits, as ascending, disjoint ranges: each a byte's value under the byte encodings
 *        and any scalar under UTF-8. What re2c calls a char set, which is all its class difference takes.
 */
using Class = std::vector<regex::utf8::Code_point_range>;

/**
 * @brief The classes among the definitions in force, by name: a definition whose regex is one class, which a class
 *        difference naming it takes as an operand; a definition that is no class has no entry.
 */
using Classes_t = std::map<std::string, Class, std::less<>>;

/**
 * @brief What the file's blocks call the scan pointers, each under the canonical name a configuration renames it
 *        by: re2c carries a `define:` configuration from the block it stands in to the blocks after it, so the
 *        names are the file's and not one block's, and a block renaming none writes what the one above it left.
 */
using Pointers_t = std::vector<std::pair<std::string, std::string>>;

/**
 * @brief What the parser reads every re2c pattern under: a bracket range written backwards spans its members as
 *        re2c reads it, `[z-a]` being `[a-z]`; the case flags re2c has are spelled into the patterns instead.
 */
constexpr regex::Parse_options re2c_parse{.caseless = false, .ranges_either_way = true};

/**
 * @brief The code points there are under an encoding: the byte values under the byte encodings, every scalar under
 *        UTF-8, which is what a negated class admits the rest of.
 * @param encoding The encoding.
 * @return One past the last code point.
 */
[[nodiscard]] constexpr char32_t code_space(const Re2c_encoding encoding) noexcept
{
    return encoding == Re2c_encoding::utf8 ? 0x110000 : 0x100;
}

/**
 * @brief The code points a class admits, when the class is one the byte parser can read.
 *
 * Every class member the reader accepts is a byte value, the Unicode escapes being refused, so the byte parser's own
 * reading of the class is its members; under an encoding whose code space is wider than a byte those values are code
 * points, and a negated class admits every other code point of the space rather than every other byte.
 * @param bracket The class as written, its brackets included.
 * @param definitions The definitions, which the parser needs for nothing here but is given for uniformity.
 * @param space One past the last code point there is, which a negated class runs to.
 * @return The class, or std::nullopt when the parser reads the bracket as something else.
 */
[[nodiscard]] std::optional<Class> code_points(
        const std::string_view bracket, const regex::Definitions_t& definitions, const char32_t space)
{
    const auto negated{bracket.starts_with("[^")};

    // The negation is taken over the code points here, so the parser is asked for the members alone; a member that
    // is itself a caret, `[^^]`, keeps its place with an escape rather than reading as a second negation.
    const std::string members{
            negated && bracket.substr(2).starts_with('^') ? "\\" + std::string{bracket.substr(2)} :
                                                            std::string{bracket.substr(negated ? 2 : 1)}};

    const std::string positive{'[' + members};

    std::vector<bool> member(space, false);

    if (positive != "[]")
    {
        const auto bytes{[&positive, &definitions]() -> std::optional<regex::Set> {
            try
            {
                return class_of(regex::parse(positive, definitions, re2c_parse));
            }
            catch (const regex::Syntax_error&)
            {
                return std::nullopt;
            }
        }()};

        if (!bytes)
        {
            return std::nullopt;
        }

        for (const auto byte : bytes->symbols())
        {
            member[static_cast<unsigned char>(byte)] = true;
        }
    }

    Class ranges;

    for (char32_t point{0}; point < space; ++point)
    {
        if (member[point] == negated)
        {
            continue;
        }

        const auto first{point};

        while (point + 1 < space && member[point + 1] != negated)
        {
            ++point;
        }

        ranges.push_back({.first = first, .last = point});
    }

    return ranges;
}

/**
 * @brief Whether a class holds a code point.
 * @param points The class.
 * @param point The code point.
 * @return True when it does.
 */
[[nodiscard]] bool holds(const Class& points, const char32_t point)
{
    const auto after{
            std::ranges::upper_bound(points, point, std::ranges::less{}, &regex::utf8::Code_point_range::first)};

    return after != points.begin() && std::prev(after)->last >= point;
}

/**
 * @brief The code points of two classes a predicate of their membership keeps, as a class: the union where it keeps
 *        a point in either, the difference where it keeps one in the left and not the right.
 * @param left The left class.
 * @param right The right class.
 * @param keep Whether a code point is kept, given whether the left class holds it and whether the right does.
 * @return The class, ascending and disjoint, empty when nothing is kept.
 */
template <typename Keep>
[[nodiscard]] Class combined(const Class& left, const Class& right, Keep keep)
{
    // Membership in either operand changes only where one of its ranges begins or just past where one ends, so
    // the code points between two such edges in a row are all kept or all dropped.
    std::vector<char32_t> edges;

    for (const auto* operand : {&left, &right})
    {
        for (const auto& [first, last] : *operand)
        {
            edges.push_back(first);

            edges.push_back(last + 1);
        }
    }

    std::ranges::sort(edges);

    const auto [duplicates, end]{std::ranges::unique(edges)};

    edges.erase(duplicates, end);

    Class kept;

    for (std::size_t index{0}; index + 1 < edges.size(); ++index)
    {
        const auto first{edges[index]};

        if (!keep(holds(left, first), holds(right, first)))
        {
            continue;
        }

        const auto last{edges[index + 1] - 1};

        if (!kept.empty() && kept.back().last + 1 == first)
        {
            kept.back().last = last;
        }
        else
        {
            kept.push_back({.first = first, .last = last});
        }
    }

    return kept;
}

/**
 * @brief The union of two classes.
 * @param left The left class.
 * @param right The right class.
 * @return The class holding the code points of either.
 */
[[nodiscard]] Class united(const Class& left, const Class& right)
{
    return combined(left, right, [](const bool in_left, const bool in_right) { return in_left || in_right; });
}

/**
 * @brief The difference of two classes, re2c's `left \ right`.
 * @param left The left class.
 * @param right The right class.
 * @return The class holding the code points of the left that the right has not got.
 */
[[nodiscard]] Class subtracted(const Class& left, const Class& right)
{
    return combined(left, right, [](const bool in_left, const bool in_right) { return in_left && !in_right; });
}

/**
 * @brief The byte a named escape stands for, as the pattern parser decodes it: the controls `\n`, `\t`, `\r`, `\f`,
 *        `\v`, `\a` and `\b`, and any other escaped byte itself.
 * @param escaped The byte after the backslash, not a hex or octal digit.
 * @return The value.
 */
[[nodiscard]] constexpr char32_t decoded(const char escaped) noexcept
{
    switch (escaped)
    {
    case 'n':
        return U'\n';
    case 't':
        return U'\t';
    case 'r':
        return U'\r';
    case 'f':
        return U'\f';
    case 'v':
        return U'\v';
    case 'a':
        return U'\a';
    case 'b':
        return U'\b';
    default:
        return static_cast<unsigned char>(escaped);
    }
}

/**
 * @brief Whether every code point of a class is ASCII, whose encoding under UTF-8 is the byte itself.
 * @param points The class.
 * @return True when it is.
 */
[[nodiscard]] bool ascii(const Class& points)
{
    return std::ranges::all_of(points, [](const regex::utf8::Code_point_range& range) { return range.last < 0x80; });
}

/**
 * @brief A set of code points as one step of the pattern parser's syntax under the UTF-8 encoding.
 *
 * The ranges become a bracket of `\u{...}` members, which the parser matches as their UTF-8 encodings. The
 * surrogates are the one part it cannot take: no encoding has them, so the parser refuses them, while re2c under its
 * default encoding policy encodes them like any other code point; a set holding them, which is every set written as
 * a negation, carries their three-byte spelling beside the bracket instead.
 * @param ranges The ranges, ascending and disjoint, at least one and not the surrogates alone.
 * @return The expression, one atom, grouped where it is more than the bracket.
 */
[[nodiscard]] std::string step(const std::vector<regex::utf8::Code_point_range>& ranges)
{
    constexpr char32_t first_surrogate{0xD800};

    constexpr char32_t last_surrogate{0xDFFF};

    std::string members;

    auto surrogates{false};

    const auto member{[&members](const char32_t first, const char32_t last) {
        members += first == last ? std::format(R"(\u{{{:x}}})", static_cast<std::uint32_t>(first)) :
                                   std::format(
                                           R"(\u{{{:x}}}-\u{{{:x}}})", static_cast<std::uint32_t>(first),
                                           static_cast<std::uint32_t>(last));
    }};

    for (const auto& [first, last] : ranges)
    {
        surrogates = surrogates || (first <= last_surrogate && last >= first_surrogate);

        if (first < first_surrogate)
        {
            member(first, std::min<char32_t>(last, first_surrogate - 1));
        }

        if (last > last_surrogate)
        {
            member(std::max<char32_t>(first, last_surrogate + 1), last);
        }
    }

    if (!surrogates)
    {
        return '[' + members + ']';
    }

    // The surrogates encode as ED A0 80 through ED BF BF, the whole block of them, since a class names no part of it.
    return std::format(R"(([{}]|"\xed"[\xa0-\xbf][\x80-\xbf]))", members);
}

/**
 * @brief A class as the pattern parser's syntax under an encoding: under UTF-8 one step of its code points where it
 *        reaches past ASCII, otherwise the bracket of its bytes, which every code point of it then is; `[]` for an
 *        empty class, which no settled pass compiles.
 * @param points The class.
 * @param encoding The encoding.
 * @return The expression, one atom.
 */
[[nodiscard]] std::string rendered(const Class& points, const Re2c_encoding encoding)
{
    if (encoding == Re2c_encoding::utf8 && !ascii(points))
    {
        return step(points);
    }

    regex::Set bytes;

    for (const auto& [first, last] : points)
    {
        for (auto point{first}; point <= last; ++point)
        {
            bytes += static_cast<char>(point);
        }
    }

    return bracket(bytes);
}

/**
 * @brief Notes what a definition is to a class difference: its class where its regex is one class, and nothing where
 *        it is not, a name defined again dropping the class it had.
 * @param classes The classes among the definitions in force.
 * @param name The definition's name.
 * @param points Its class, when its regex is one.
 */
void note_class(Classes_t& classes, const std::string& name, std::optional<Class> points)
{
    if (points)
    {
        classes.insert_or_assign(name, std::move(*points));
    }
    else
    {
        classes.erase(name);
    }
}

/**
 * @brief Where each block a later one may use begins, by name: a `rules:re2c:name` block or any other block opened
 *        with a name, kept as the offset just past its opener.
 *
 * An offset is all a use needs, and more faithful than the reading itself would be: re2c compiles a rules block's
 * regexes at every point of use, under the configurations in force there, so a `!use:name;` and a use block read the
 * block's source again rather than copy what it was read as here. The same block can then be one scanner's under
 * one encoding and another's under another, which is what re2c's own multiple-encoding example does.
 */
using Library_t = std::map<std::string, std::size_t, std::less<>>;

/**
 * @brief One definition a later block may use: its name and where its regex stands.
 *
 * An offset is all a later block needs, and more faithful than the expression this one translated: re2c compiles a
 * definition's regex at every point of use, under the configuration in force there, so a definition written where
 * the encoding was ASCII is a whole code point in a block that turns UTF-8 on, and one written under the flex
 * syntax is read under it again wherever it is used.
 */
struct Definition_site
{
    /**
     * @brief The name the definition binds.
     */
    std::string name;

    /**
     * @brief The offset its regex begins at, just past the `=` or past the name under the flex syntax.
     */
    std::size_t begin;

    /**
     * @brief Whether the regex ends at the line's end, which a flex-style definition's does.
     */
    bool line_bound;
};

/**
 * @brief A carried definition this block's flags cannot read, kept until the block says whether it names it.
 *
 * re2c compiles a definition's regex where it is used, so one another block wrote in a shape this block cannot
 * read, a byte beyond ASCII under the UTF-8 encoding among them, is refused where a pattern of this block names it
 * and nowhere else: a block that names none of them reads as re2c reads it.
 */
struct Unreadable_definition
{
    /**
     * @brief The name the definition binds.
     */
    std::string name;

    /**
     * @brief The refusal to raise where a pattern names it.
     */
    Spec_error refusal;
};

/**
 * @brief The names a scanner's rules reach: every `{name}` reference in a rule's expression, and through the
 *        definitions those name, the references in theirs, however many definitions deep.
 *
 * re2c compiles a definition's regex where a rule uses it, so a definition no rule reaches, directly or through a
 * chain of definitions, is compiled nowhere in the block, and an alias of an unreadable definition is as unused as
 * the definition itself while no rule names either.
 * @param spec The specification, its rules and definitions read.
 * @return The names, whether or not a definition binds each.
 */
[[nodiscard]] std::set<std::string, std::less<>> reached(const Lexer_spec& spec)
{
    // A reference is a brace, a name not opening with a digit and a brace, outside a quoted literal and a bracket,
    // where the braces are text; `{2,5}` is a count and no reference.
    const auto references{[](const std::string_view text, std::vector<std::string>& into) {
        for (std::size_t at{0}; at < text.size(); ++at)
        {
            if (text[at] == '\\')
            {
                ++at;

                continue;
            }

            if (text[at] == '"' || text[at] == '[')
            {
                const auto close{text[at] == '"' ? '"' : ']'};

                for (++at; at < text.size() && text[at] != close; ++at)
                {
                    at += text[at] == '\\' ? 1 : 0;
                }

                continue;
            }

            if (text[at] != '{')
            {
                continue;
            }

            auto end{at + 1};

            while (end < text.size() && is_name_byte(text[end]))
            {
                ++end;
            }

            const auto named{end > at + 1 && !(text[at + 1] >= '0' && text[at + 1] <= '9')};

            if (named && end < text.size() && text[end] == '}')
            {
                into.emplace_back(text.substr(at + 1, end - at - 1));
            }
        }
    }};

    std::set<std::string, std::less<>> names;

    std::vector<std::string> pending;

    for (const auto& rule : spec.rules)
    {
        references(rule.expression, pending);
    }

    while (!pending.empty())
    {
        auto name{std::move(pending.back())};

        pending.pop_back();

        if (!names.insert(name).second)
        {
            continue;
        }

        if (const auto definition{spec.definitions.find(name)}; definition != spec.definitions.end())
        {
            references(definition->second, pending);
        }
    }

    return names;
}

/**
 * @brief Whether a default rule stands in every condition: it names none, or names `*`.
 * @param rule The default rule.
 * @return True when it does.
 */
[[nodiscard]] bool in_every_condition(const Lexer_spec::Rule& rule)
{
    return rule.conditions.empty() || std::ranges::contains(rule.conditions, "*");
}

/**
 * @brief Settles a block's default rules as re2c 3.1 settles them once the block is read: a second default rule of
 *        the block's own for a condition it already gave one is refused, as re2c refuses it, and a default rule a
 *        `!use:` directive brought in yields to the block's own in every condition the block's own stands in, since
 *        the using block's default rule overrides the used block's wherever the two stand.
 *
 * A default rule in every condition, `<*> *` or one naming no condition, and one naming conditions are rules of
 * different conditions to re2c, so the two stand together, and a used one of either kind yields only to an own one
 * of the same kind.
 * @param spec The specification the block filled, its rules in the order they were read.
 * @param first The index into spec.rules of the block's first rule, the ones before it being another block's, which
 *        read this one through a `!use:` directive and settles its own once it is read.
 * @param used The indices into spec.rules of the default rules the block's `!use:` directives brought in.
 * @throws Spec_error If the block gives one condition two default rules of its own, at the second's line.
 */
void settle_defaults(Lexer_spec& spec, const std::size_t first, const std::vector<std::size_t>& used)
{
    const auto shared{[](const Lexer_spec::Rule& one, const Lexer_spec::Rule& other) {
        if (in_every_condition(one) || in_every_condition(other))
        {
            return in_every_condition(one) && in_every_condition(other);
        }

        return std::ranges::any_of(one.conditions, [&other](const std::string& name) {
            return std::ranges::contains(other.conditions, name);
        });
    }};

    std::vector<std::size_t> own;

    for (auto index{first}; index < spec.rules.size(); ++index)
    {
        if (spec.rules[index].pattern == "*" && !std::ranges::contains(used, index))
        {
            own.push_back(index);
        }
    }

    for (auto second{own.begin()}; second != own.end(); ++second)
    {
        for (auto first{own.begin()}; first != second; ++first)
        {
            if (shared(spec.rules[*first], spec.rules[*second]))
            {
                throw Spec_error{
                        std::format(
                                "the default rule for this condition is already defined at line {}, which re2c "
                                "refuses",
                                spec.rules[*first].line),
                        spec.rules[*second].line};
            }
        }
    }

    // A used rule loses the conditions an own rule of its kind stands in, and goes when none is left; the indices
    // ascend as the directives were read, so the rules that go are erased from the back.
    std::vector<std::size_t> yielded;

    for (const auto index : used)
    {
        auto& rule{spec.rules[index]};

        const auto everywhere{in_every_condition(rule)};

        auto yields{false};

        for (const auto own_index : own)
        {
            const auto& other{spec.rules[own_index]};

            if (everywhere != in_every_condition(other))
            {
                continue;
            }

            if (everywhere)
            {
                yields = true;

                break;
            }

            std::erase_if(rule.conditions, [&other](const std::string& name) {
                return std::ranges::contains(other.conditions, name);
            });

            yields = rule.conditions.empty();
        }

        if (yields)
        {
            yielded.push_back(index);
        }
    }

    for (const auto index : yielded | std::views::reverse)
    {
        spec.rules.erase(spec.rules.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

/**
 * @brief Why an action leaves a scan pointer elsewhere than where the match ended, when it does: re2c's `YYCURSOR`,
 *        `YYMARKER` or `YYCTXMARKER` under whatever names the block's configurations give them, moved by an
 *        assignment or a step, `YYCURSOR = p`, `++YYCURSOR`, `in->cur += 2`; or a pointer whose configured spelling
 *        the reading cannot settle.
 * @param code The action's text.
 * @param pointers The names the block gives the scan pointers.
 * @param macros The macros the file defines: a transparent one, `#define SLOT 0`, stands for its value in a
 *        configured name and in an action alike, so `cursors[SLOT]` and `cursors[0]` are one spelling to the
 *        comparison, whichever side writes which; one defined more than once stands for each of its values in
 *        turn, since which is live at the action is not decided here; a function-like one stands for its value
 *        whatever its arguments; and a pointer named through an opaque one is out of sight.
 * @return What the refusal says after "the action", or std::nullopt when the action leaves every pointer where
 *         it was.
 */
[[nodiscard]] std::optional<std::string> moved_cursor(
        const std::string_view code, std::span<const std::string> pointers, const Macros_t& macros)
{
    const auto tokens{c_tokens(code)};

    // `0U`, `0x0`, `00` and `0'0` are the one index 0: an integer literal is read to its value, its digit
    // separators, base prefix and suffix taken off, so that no spelling of an index is another index.
    const auto canonical{[](const std::string& word) {
        if (word.empty() || std::isdigit(static_cast<unsigned char>(word.front())) == 0)
        {
            return word;
        }

        std::string digits;

        std::ranges::copy_if(word, std::back_inserter(digits), [](const char byte) { return byte != '\''; });

        unsigned base{10};

        std::size_t at{0};

        if (digits.starts_with("0x") || digits.starts_with("0X"))
        {
            base = 16;

            at = 2;
        }
        else if (digits.starts_with("0b") || digits.starts_with("0B"))
        {
            base = 2;

            at = 2;
        }
        else if (digits.size() > 1 && digits.front() == '0')
        {
            base = 8;

            at = 1;
        }

        unsigned long long value{0};

        for (; at < digits.size(); ++at)
        {
            const auto byte{static_cast<unsigned char>(digits[at])};

            const auto digit{
                    std::isdigit(byte) != 0 ? static_cast<unsigned>(byte - '0') :
                    std::isalpha(byte) != 0 ? static_cast<unsigned>(std::tolower(byte) - 'a' + 10) :
                                              base};

            if (digit >= base || value > (std::numeric_limits<unsigned long long>::max() - digit) / base)
            {
                break;
            }

            value = value * base + digit;
        }

        for (; at < digits.size(); ++at)
        {
            if (std::string_view{"uUlLzZ"}.find(digits[at]) == std::string_view::npos)
            {
                return word;
            }
        }

        return std::to_string(value);
    }};

    // The macros the action and the pointers' spellings name, each with the values it stands for; a pointer spelled
    // through a macro that stands for no one value is out of sight, since what the scanner steps is then unknown.
    std::map<std::string, std::vector<std::vector<std::string>>, std::less<>> values;

    const auto note{[&values, &macros](const std::string& word) {
        if (macros.contains(word) && !values.contains(word))
        {
            values.emplace(word, plain_values(word, macros));
        }
    }};

    std::vector<std::vector<C_token>> spelled;

    for (const auto& pointer : pointers)
    {
        spelled.push_back(c_tokens(pointer));

        for (const auto& token : spelled.back())
        {
            note(token.text);

            if (macros.contains(token.text) && values[token.text].empty())
            {
                return "names a scan pointer through " + token.text +
                       ", a macro whose replacement is more than a value, so where the pointer stands is out of "
                       "sight";
            }
        }
    }

    for (const auto& token : tokens)
    {
        note(token.text);
    }

    // Every way the transparent macros may be defined at the action, one value each; an opaque macro in the action
    // stands as its name, macro_use() refusing its use where a pointer's spelling could hide in it.
    using Choice_t = std::map<std::string, std::vector<std::string>, std::less<>>;

    std::vector<Choice_t> choices{{}};

    for (const auto& [name, candidates] : values)
    {
        if (candidates.empty())
        {
            continue;
        }

        std::vector<Choice_t> next;

        for (const auto& choice : choices)
        {
            for (const auto& candidate : candidates)
            {
                next.push_back(choice);

                next.back().emplace(name, candidate);
            }
        }

        if (next.size() > 64)
        {
            return "names " + name +
                   ", a macro defined in more ways than the reading follows together, so the pointers' spellings "
                   "are out of sight";
        }

        choices = std::move(next);
    }

    // A spelling reduced until nothing changes: parentheses around one name or one value are grouping and name
    // nothing, `(in)->cur` being `in->cur` and `(cursors)[0]` being `cursors[0]`; a dereference and a dot are an
    // arrow, `(*in).cur` being `in->cur`; every other parenthesis is a call's or an index's and stays.
    const auto reduce{[](std::vector<std::pair<std::string, std::size_t>>& out) {
        for (auto changed{true}; changed;)
        {
            changed = false;

            const auto word{[&out](const std::size_t index) {
                return index < out.size() ? std::string_view{out[index].first} : std::string_view{};
            }};

            const auto named{[](const std::string_view text) {
                return !text.empty() &&
                       (std::isalpha(static_cast<unsigned char>(text.front())) != 0 || text.front() == '_');
            }};

            std::vector<std::pair<std::string, std::size_t>> next;

            for (std::size_t at{0}; at < out.size(); ++at)
            {
                if (word(at) == "(" && word(at + 1) == "*" && named(word(at + 2)) && word(at + 3) == ")" &&
                    word(at + 4) == ".")
                {
                    next.push_back(out[at + 2]);

                    next.emplace_back("->", out[at + 4].second);

                    at += 4;

                    changed = true;

                    continue;
                }

                // A value in parentheses is the value, `cursors[(0)]` being `cursors[0]`.
                const auto valued{[](const std::string_view text) {
                    return !text.empty() && std::isdigit(static_cast<unsigned char>(text.front())) != 0;
                }};

                if (word(at) == "(" && (named(word(at + 1)) || valued(word(at + 1))) && word(at + 2) == ")")
                {
                    next.push_back(out[at + 1]);

                    at += 2;

                    changed = true;

                    continue;
                }

                next.push_back(out[at]);
            }

            out = std::move(next);
        }
    }};

    // The arrays the configured spellings index, each run before a bracket of a reduced spelling: `cursors` of
    // `cursors[0]`, `in->cursors` of `in->cursors[0]`, and both `slots` and `slots[0].cursors` of
    // `slots[0].cursors[0]`. An index into one of them, in the action or in a configured spelling, is read to its
    // value when it is a constant expression, `+0`, `1-1` and `(0)` being `0`, and is out of sight when it is not,
    // `cursors[i]` and `cursors[SLOT]` under `constexpr unsigned SLOT` standing for any slot, the scan pointer's
    // among them.
    std::vector<std::vector<std::string>> arrays;

    for (const auto& pointer : spelled)
    {
        std::vector<std::pair<std::string, std::size_t>> reduced;

        for (std::size_t at{0}; at < pointer.size(); ++at)
        {
            reduced.emplace_back(canonical(pointer[at].text), at);
        }

        reduce(reduced);

        for (std::size_t at{1}; at < reduced.size(); ++at)
        {
            if (reduced[at].first == "[")
            {
                std::vector<std::string> run;

                for (std::size_t piece{0}; piece < at; ++piece)
                {
                    run.push_back(reduced[piece].first);
                }

                arrays.push_back(std::move(run));
            }
        }
    }

    std::optional<std::string> unread;

    // The value of a constant integer expression over the tokens given: decimal literals within an int's range,
    // `+`, `-`, `*`, `/`, `%`, unary signs and parentheses, with C's precedence, every value kept within an int's
    // range; or nothing where a token is anything else, a shift, a bitwise operator or a suffixed or hexadecimal
    // literal among them, whose type and width the reading does not model, `~0U >> 31` being 1 and
    // `(1U << 31) << 1` being 0 under the compiler where a signed reading says otherwise.
    const auto evaluated{[](const std::vector<std::string>& words) -> std::optional<long long> {
        std::size_t at{0};

        constexpr long long bound{2147483647};

        std::function<std::optional<long long>(int)> level;

        const auto peek{
                [&words, &at]() { return at < words.size() ? std::string_view{words[at]} : std::string_view{}; }};

        const auto unary{[&]() -> std::optional<long long> {
            const auto word{peek()};

            if (word == "+" || word == "-")
            {
                ++at;

                const auto inner{level(-1)};

                return inner ? std::optional{word == "+" ? *inner : -*inner} : std::nullopt;
            }

            if (word == "(")
            {
                ++at;

                const auto inner{level(1)};

                if (!inner || peek() != ")")
                {
                    return std::nullopt;
                }

                ++at;

                return inner;
            }

            if (word.empty() || std::isdigit(static_cast<unsigned char>(word.front())) == 0)
            {
                return std::nullopt;
            }

            long long value{0};

            for (const char byte : word)
            {
                if (std::isdigit(static_cast<unsigned char>(byte)) == 0 || value > (bound - (byte - '0')) / 10)
                {
                    return std::nullopt;
                }

                value = value * 10 + (byte - '0');
            }

            ++at;

            return value;
        }};

        static constexpr std::string_view operators[][3]{{"*", "/", "%"}, {"+", "-", ""}};

        level = [&](const int depth) -> std::optional<long long> {
            if (depth < 0)
            {
                return unary();
            }

            auto left{level(depth - 1)};

            while (left)
            {
                const auto word{peek()};

                if (word.empty() || std::ranges::find(operators[depth], word) == std::ranges::end(operators[depth]))
                {
                    break;
                }

                ++at;

                const auto right{level(depth - 1)};

                if (!right || ((word == "/" || word == "%") && *right == 0))
                {
                    return std::nullopt;
                }

                left = word == "*" ? *left * *right :
                       word == "/" ? *left / *right :
                       word == "%" ? *left % *right :
                       word == "+" ? *left + *right :
                                     *left - *right;

                if (*left > bound || *left < -bound - 1)
                {
                    return std::nullopt;
                }
            }

            return left;
        };

        const auto value{level(1)};

        return at == words.size() ? value : std::nullopt;
    }};

    // A configured name is an expression and not always one word: `re2c:define:YYCURSOR = "in->cur";` names a
    // member, which re2c writes into the scanner as it stands, so the name is looked for as the run of tokens it
    // is and an operator beside that run moves it as one beside a bare name would.
    // A spelling of a scan pointer as the reading compares it: a transparent macro is its value first, as the
    // preprocessor makes it before the compiler compares anything, a function-like one's arguments dropped with
    // it; an integer literal is its value; parentheses around one name are grouping and name nothing, `(in)->cur`
    // being `in->cur`; a dereference and a dot are an arrow, `(*in).cur` being `in->cur`; every other parenthesis
    // is a call's or an index's and stays, since `in->cursor()` reaches the pointer through a call and
    // `slots[(i+j)*k]` is not `slots[i+(j*k)]`. Each token keeps the index it came from.
    const auto plain{[&macros, &canonical, &arrays, &evaluated, &unread, &reduce](
                             const std::vector<C_token>& raw, const Choice_t& choice) {
        // Pairs of text and origin, reduced in passes until nothing changes, so that a group inside a group,
        // `((in))->cur`, comes down to the name as `(in)->cur` does.
        std::vector<std::pair<std::string, std::size_t>> out;

        for (std::size_t at{0}; at < raw.size(); ++at)
        {
            // A function-like macro stands for its value only where it is invoked, `SLOT()`; the bare name is a
            // name of the file's own, `constexpr unsigned SLOT = 1` beside `#define SLOT() 0`.
            const auto function_like{macros.contains(raw[at].text) && macros.find(raw[at].text)->second.function_like};

            const auto invoked{at + 1 < raw.size() && raw[at + 1].text == "("};

            if (const auto value{choice.find(raw[at].text)}; value != choice.end() && (!function_like || invoked))
            {
                // The value's words are read as the action's own are, so `#define SLOT 0U` indexes slot 0.
                for (const auto& word : value->second)
                {
                    out.emplace_back(canonical(word), at);
                }

                if (function_like)
                {
                    auto groups{0};

                    for (++at; at < raw.size(); ++at)
                    {
                        groups += raw[at].text == "(" ? 1 : raw[at].text == ")" ? -1 : 0;

                        if (groups == 0)
                        {
                            break;
                        }
                    }
                }

                continue;
            }

            out.emplace_back(canonical(raw[at].text), at);
        }

        reduce(out);

        // Every index into one of the arrays, read to its value or marked out of sight, the earlier of two in one
        // spelling first so that `slots[1-1].cursors` is `slots[0].cursors` before the second is looked at.
        for (std::size_t at{0}; at + 1 < out.size(); ++at)
        {
            // The array's run ending at this token, when one of the arrays does.
            const auto indexed{[&out, &arrays, at]() -> std::optional<std::string> {
                if (out[at + 1].first != "[")
                {
                    return std::nullopt;
                }

                for (const auto& run : arrays)
                {
                    if (run.size() > at + 1)
                    {
                        continue;
                    }

                    auto matched{true};

                    for (std::size_t step{0}; matched && step < run.size(); ++step)
                    {
                        matched = out[at - step].first == run[run.size() - 1 - step];
                    }

                    if (matched)
                    {
                        std::string joined;

                        for (const auto& word : run)
                        {
                            joined += word;
                        }

                        return joined;
                    }
                }

                return std::nullopt;
            }};

            const auto array{indexed()};

            if (!array)
            {
                continue;
            }

            auto close{at + 1};

            for (auto groups{0}; close < out.size(); ++close)
            {
                groups += out[close].first == "[" ? 1 : out[close].first == "]" ? -1 : 0;

                if (groups == 0)
                {
                    break;
                }
            }

            if (close >= out.size())
            {
                break;
            }

            std::vector<std::string> words;

            for (auto piece{at + 2}; piece < close; ++piece)
            {
                words.push_back(out[piece].first);
            }

            if (const auto value{evaluated(words)})
            {
                out.erase(
                        out.begin() + static_cast<std::ptrdiff_t>(at) + 2,
                        out.begin() + static_cast<std::ptrdiff_t>(close));

                out.insert(out.begin() + static_cast<std::ptrdiff_t>(at) + 2, {std::to_string(*value), out[at].second});
            }
            else if (!unread)
            {
                unread = *array;
            }
        }

        reduce(out);

        return out;
    }};

    // The action under one way of defining the macros: the pointer it moves, when it moves one.
    const auto moved_under{[&](const Choice_t& choice) -> std::optional<std::string> {
        std::vector<std::vector<std::string>> spellings;

        for (const auto& pointer : spelled)
        {
            std::vector<std::string> spelling;

            for (const auto& [text, index] : plain(pointer, choice))
            {
                spelling.push_back(text);
            }

            if (!spelling.empty())
            {
                spellings.push_back(std::move(spelling));
            }
        }

        const auto plainly{plain(tokens, choice)};

        for (std::size_t here{0}; here < plainly.size(); ++here)
        {
            // The run of tokens the name is, in the reading's own spelling of both.
            const auto matched{[&](const std::vector<std::string>& spelling) {
                if (here + spelling.size() > plainly.size())
                {
                    return false;
                }

                for (std::size_t step{0}; step < spelling.size(); ++step)
                {
                    if (plainly[here + step].first != spelling[step])
                    {
                        return false;
                    }
                }

                return true;
            }};

            const auto spelling{std::ranges::find_if(spellings, matched)};

            if (spelling == std::ranges::end(spellings))
            {
                continue;
            }

            // The neighbours are read in the same spelling, so the `++` of `++(*in).cur` stands beside `in` as it
            // stands beside `in->cur`. Parentheses around the whole name, `(in->cur)++`, change nothing an operator
            // beside them does, so the operators are looked for outside every pair that wraps the name.
            auto first{here};

            auto last{here + spelling->size() - 1};

            const auto ptext{[&plainly](const std::size_t index) {
                return index < plainly.size() ? std::string_view{plainly[index].first} : std::string_view{};
            }};

            // The pointer handed on rather than moved here is out of sight, judged before the parentheses that
            // wrap the name alone are stepped over, since `advance(cursors[0])` wraps it in a call's: its address
            // taken, `&cursors[0]`, a reference bound to it, `auto& cursor = cursors[0]`, or the pointer passed to a
            // call, `advance(cursors[0])`, which may take it by reference; a cast's or a grouping's parentheses pass
            // nothing.
            const auto value_before{[&ptext](const std::size_t index) {
                const auto word{ptext(index)};

                return !word.empty() && (std::isalnum(static_cast<unsigned char>(word.front())) != 0 ||
                                         word.front() == '_' || word == ")" || word == "]");
            }};

            // The pointer handed on is out of sight at every level of parentheses around the name, `(cursors[0])`
            // bound to a reference or passed as `cursors[0]` is: the checks below run on the name and again on
            // each pair wrapping it.
            for (auto outer_first{first}, outer_last{last};;)
            {
                const auto first{outer_first};

                const auto last{outer_last};

                const auto addressed{first >= 1 && ptext(first - 1) == "&" && !(first >= 2 && value_before(first - 2))};

                const auto declared{[&ptext, &value_before](const std::size_t name) {
                    return value_before(name) && name >= 1 && (ptext(name - 1) == "&" || ptext(name - 1) == "*");
                }};

                const auto bound{
                        (first >= 3 && ptext(first - 1) == "=" && ptext(first - 2) != "=" && declared(first - 2)) ||
                        (first >= 3 && ptext(first - 1) == "{" && ptext(last + 1) == "}" && declared(first - 2))};

                auto passed{false};

                for (auto back{first}, groups{0UZ}; back > 0; --back)
                {
                    const auto word{ptext(back - 1)};

                    if (word == ")" || word == "]")
                    {
                        ++groups;
                    }
                    else if ((word == "(" || word == "[") && groups > 0)
                    {
                        --groups;
                    }
                    else if (word == "(" && back >= 2)
                    {
                        static constexpr std::string_view keywords[]{"if",     "while",  "for",
                                                                     "switch", "return", "sizeof"};

                        // A parenthesised group before the `(` is a cast, `(int)(...)` or `(const char*)(...)`, when it
                        // opens with a type's word or holds a `*` or `&` after a name, and a call through a pointer or
                        // an expression otherwise, `(*f)(...)`.
                        static constexpr std::string_view types[]{
                                "int",    "char", "long",  "short",  "unsigned",  "signed", "void",    "float",
                                "double", "bool", "const", "size_t", "ptrdiff_t", "auto",   "volatile"};

                        const auto opener{ptext(back - 2)};

                        // `(int)` came down to `int` with its grouping parentheses, so a type's word before the `(` is
                        // a cast as well.
                        auto cast{std::ranges::find(types, opener) != std::ranges::end(types)};

                        if (opener == ")")
                        {
                            auto open{back - 2};

                            for (auto inner{0UZ}; open > 0; --open)
                            {
                                inner += ptext(open) == ")" ? 1 : ptext(open) == "(" ? -1 : 0;

                                if (inner == 0)
                                {
                                    break;
                                }
                            }

                            const auto head{ptext(open + 1)};

                            auto stars{false};

                            for (auto piece{open + 2}; piece + 1 < back - 1; ++piece)
                            {
                                stars = stars || ptext(piece) == "*" || ptext(piece) == "&";
                            }

                            cast = std::ranges::find(types, head) != std::ranges::end(types) ||
                                   (stars && !head.empty() &&
                                    (std::isalpha(static_cast<unsigned char>(head.front())) != 0 ||
                                     head.front() == '_'));
                        }

                        passed = value_before(back - 2) && !cast &&
                                 std::ranges::find(keywords, opener) == std::ranges::end(keywords);

                        // A grouping's or a cast's parenthesis is stepped out of, since `f((cursors[0]))` passes the
                        // pointer as `f(cursors[0])` does.
                        if (passed)
                        {
                            break;
                        }
                    }
                    else if (word == ";" || word == "{" || word == "}")
                    {
                        break;
                    }
                }

                if (addressed || bound || passed)
                {
                    std::string name;

                    for (const auto& word : *spelling)
                    {
                        name += word;
                    }

                    return "hands on " + name +
                           ", by its address, a reference bound to it or a call it is passed to, so what becomes of it";
                }

                if (outer_first > 0 && ptext(outer_first - 1) == "(" && ptext(outer_last + 1) == ")")
                {
                    --outer_first;

                    ++outer_last;

                    continue;
                }

                break;
            }

            while (first > 0 && ptext(first - 1) == "(" && ptext(last + 1) == ")")
            {
                --first;

                ++last;
            }

            // `++` and `--` are two tokens the source writes together; `1 - -YYCURSOR[0]` is a difference of a
            // negation and moves nothing, so the two must be one operator in the text the compiler reads, which is
            // the text with its line splices taken out: only a splice may stand between them.
            const auto joined{[&tokens, &plainly, code](const std::size_t one, const std::size_t other) {
                if (other >= plainly.size())
                {
                    return false;
                }

                const auto& left{tokens[plainly[one].second]};

                const auto& right{tokens[plainly[other].second]};

                auto between{code.substr(left.end, right.at - left.end)};

                while (between.starts_with('\\'))
                {
                    between.remove_prefix(1);

                    between.remove_prefix(std::min(between.find_first_not_of(" \t"), between.size()));

                    between.remove_prefix(between.starts_with('\r') ? 1 : 0);

                    if (!between.starts_with('\n'))
                    {
                        return false;
                    }

                    between.remove_prefix(1);
                }

                return between.empty();
            }};

            const auto stepped{
                    (ptext(last + 1) == ptext(last + 2) && (ptext(last + 1) == "+" || ptext(last + 1) == "-") &&
                     joined(last + 1, last + 2)) ||
                    (first >= 2 && ptext(first - 1) == ptext(first - 2) &&
                     (ptext(first - 1) == "+" || ptext(first - 1) == "-") && joined(first - 2, first - 1))};

            const auto assigned{
                    (ptext(last + 1) == "=" && ptext(last + 2) != "=") ||
                    ((ptext(last + 1) == "+" || ptext(last + 1) == "-") && ptext(last + 2) == "=")};

            if (stepped || assigned)
            {
                std::string name;

                for (const auto& word : *spelling)
                {
                    name += word;
                }

                return name;
            }
        }

        return std::nullopt;
    }};

    for (const auto& choice : choices)
    {
        if (const auto name{moved_under(choice)})
        {
            return name->starts_with("hands on ") ?
                           *name + " is out of sight" :
                           "moves " + *name + ", so the next token does not begin where the match ends";
        }

        if (unread)
        {
            return "indexes " + *unread +
                   " by an expression the reading does not evaluate, so whether it names the scan pointer is out "
                   "of sight";
        }
    }

    return std::nullopt;
}

/**
 * @brief A cursor over one re2c block, from just past its opener to the comment close that ends it.
 *
 * The block is read item by item: a configuration, a definition or a rule, each ending where re2c's own grammar ends
 * it, and the regex text of a definition or a rule is rewritten for the pattern parser as it is read. The close is
 * found the way re2c finds it, as the first star-slash between items: one inside a quoted literal, a class, an
 * action or a comment is content, since the file is read by re2c and not by a C compiler. Definitions come in two
 * spellings, re2c's `name = regex;` and the flex one, a name followed by a blank and regex to the end of the line,
 * which re2c accepts with its flex-syntax flag wherever the name stands and which a name opening an item, followed
 * by a blank and then something other than a brace, identifies.
 */
class Block : public Cursor
{
public:
    /**
     * @brief Binds the cursor to the source just past a block's opener.
     * @param source The whole file.
     * @param begin The offset just past the opener.
     * @param reading The flags every pattern of the block is translated under, which is the configuration the whole
     *        block leaves once the reading has settled on it.
     * @param configured The flags the configurations have left so far, the same as `reading` at a block's head and
     *        the using block's where a used block is read at its `!use:` directive.
     * @param classes The classes among the definitions in force, which the block's own definitions join and its
     *        class differences take their operands from; shared with the blocks read under the same pass.
     */
    Block(std::string_view source, std::size_t begin, Re2c_flags reading, Re2c_flags configured, Classes_t& classes,
          Pointers_t& pointers, const Macros_t& macros, std::optional<std::size_t>& api_custom);

    /**
     * @brief Reads every item of the block into the specification, through the block's close.
     * @param spec The specification being filled.
     * @param library The named blocks read so far, which a `!use:name;` item merges into the specification.
     * @param returning The forms besides `return` an action returns a token through.
     * @return The offset just past the close.
     * @throws Spec_error If an item is malformed or left open, a used block is unknown, or the block never closes.
     */
    [[nodiscard]] std::size_t read(Lexer_spec& spec, const Library_t& library, const Returning_t& returning);

    /**
     * @brief Reads a rules block into the specification as used here, a `!use:name;` directive or a use block's
     *        opener: the used block is read again at this point, under the flags this block's patterns are read
     *        under, so that its rules are the rules this block compiles rather than the ones its own block was read
     *        as; its configurations stand where the directive does, and the ones after it may override them in
     *        turn; its default rules are noted, since the block's own override them once the block is read.
     * @param begin The offset just past the used block's opener.
     * @param spec The specification being filled.
     * @param library The named blocks read so far, which the used block's own `!use:name;` items may name.
     * @param returning The forms besides `return` an action returns a token through.
     * @throws Spec_error As read() does for the used block.
     */
    void use(std::size_t begin, Lexer_spec& spec, const Library_t& library, const Returning_t& returning);

    /**
     * @brief Refuses the block as re2c 3.1 refuses a scanner whose rules are of both kinds, ones naming a condition,
     *        `<*>` included, and ones naming none, the rules its `!use:` directives brought in counted with its own;
     *        for a scanner, since a rules block is compiled where it is used and not on its own.
     * @throws Spec_error If rules of both kinds were read, at the first naming none, in re2c's words; or if the only
     *         rule naming none is the end rule `$`, which re2c refuses in words of its own, at that rule.
     */
    void refuse_mixed_kinds() const;

    /**
     * @brief Refuses every action of the block that moves a scan pointer, the pointers named as the block's own
     *        configurations name them.
     *
     * re2c applies a configuration to the whole block it stands in, so one written after the rules renames the
     * pointer for them too, and the check waits until the block is read rather than asking what a rule stood
     * under. A rule's own action, a setup rule's and the entry rule's are all code re2c runs at a match.
     * @throws Spec_error If an action moves one, at that action's line.
     */
    void refuse_moved_cursors();

    /**
     * @brief Leaves this block's actions and API to the block that uses it: a rules library is compiled where it is
     *        used, under the using block's configurations, and is judged nowhere else.
     */
    void judge_later() noexcept;

    /**
     * @brief The flags the block's configurations and the evidence of the flex syntax have left, which the next
     *        pass translates its patterns under and the next block inherits.
     * @return The flags.
     */
    [[nodiscard]] Re2c_flags flags() const noexcept;

    /**
     * @brief Where each definition the block declared stands, in the order it declared them, the ones a `!use:`
     *        directive brought in included.
     * @return The sites.
     */
    [[nodiscard]] const std::vector<Definition_site>& sites() const noexcept;

    /**
     * @brief The conditions the block's rules name, `*` aside, in the order first named; see named_.
     * @return The names.
     */
    [[nodiscard]] const std::vector<std::string>& named() const noexcept;

    /**
     * @brief The names some rule other than the end rule stands in; see ruled_.
     * @return The names.
     */
    [[nodiscard]] const std::vector<std::string>& ruled() const noexcept;

    /**
     * @brief The end rules read, each name with its line; see ends_.
     * @return The end rules.
     */
    [[nodiscard]] const std::vector<std::pair<std::string, std::size_t>>& ends() const noexcept;

    /**
     * @brief The first refusal this pass's flags decided and the settled flags may not: a class difference left
     *        empty, which `[^] \ [\x00-\xff]` is under ASCII and is not under UTF-8, held rather than thrown, since
     *        the pass reading under the flags the block leaves is the one that answers it.
     * @return The refusal, at its rule's line, or std::nullopt when the pass decided none.
     */
    [[nodiscard]] const std::optional<Spec_error>& deferred() const noexcept;

    /**
     * @brief The line of the configuration that last set the block's encoding, where a refusal of the encoding the
     *        configurations leave points; the block's opening line while none has set it.
     * @return The line.
     */
    [[nodiscard]] std::size_t encoding_line() const noexcept;

    /**
     * @brief The expression of the definition whose regex the cursor stands at, translated under this block's flags,
     *        and its class where the regex is one class.
     * @param line_bound Whether the regex ends at the line's end, which a flex-style definition's does and which
     *        also makes the flex syntax the one it is read in.
     * @param definitions The definitions in force here, which this one's regex may name.
     * @return The expression and the class.
     * @throws Spec_error If the regex is one this block's flags cannot read, a byte beyond ASCII under UTF-8 among
     *         them.
     */
    [[nodiscard]] std::pair<std::string, std::optional<Class>> definition(
            bool line_bound, const regex::Definitions_t& definitions);

private:
    /**
     * @brief Reads a `re2c:` configuration through its `;` into the options, a flag among them into the flags the
     *        configurations leave, never into the ones the patterns of this pass are read under.
     * @param spec The specification being filled.
     */
    void configuration(Lexer_spec& spec);

    /**
     * @brief Reads a `<...>` condition list after its `<`, through its `>`.
     * @return The names, `*` for all; std::nullopt for a `<!...>` setup rule, which is no token.
     */
    [[nodiscard]] std::optional<std::vector<std::string>> conditions();

    /**
     * @brief Reads regex text up to what ends it: a bare `=` for a definition, a `;` closing a definition's body, or
     *        the start of an action, `{`, `:=` or `=>`.
     *
     * The text as written and the text rewritten for the pattern parser are both returned, the rewriting done token
     * by token: bare names become `{name}` unless the flex syntax makes them literals, quoted literals other than an
     * exact double-quoted one become bracket sequences, blanks are dropped, and a class difference `A \ B` becomes
     * the class of the code points left, its operands the char sets re2c takes there. With them comes the class the
     * whole regex is, where it is one, which a definition keeps for the differences that name it.
     * @param definitions The definitions read so far, which a difference's operand may name.
     * @return The pattern as written, its expression, both empty when an action follows at once, and its class.
     * @throws Spec_error If a quote or bracket is left open, or the regex uses a refused construct, an operand of a
     *         class difference that is no char set among them.
     */
    [[nodiscard]] std::tuple<std::string, std::string, std::optional<Class>> regex_text(
            const regex::Definitions_t& definitions);

    /**
     * @brief Reads an action starting at the cursor: a brace block, or `:=` and the rest of the line, a `=> c` or
     *        `:=> c` transition included in the text.
     * @return The action's text.
     * @throws Spec_error If a brace block never closes.
     */
    [[nodiscard]] std::string action();

    /**
     * @brief A quoted literal after its opening quote, through the closing one, as a bracket sequence, one bracket per
     *        character and an escape kept as written inside its bracket, an escape naming a code point beyond ASCII
     *        under UTF-8 excepted, which becomes the step of that code point.
     * @param quote The closing quote.
     * @param insensitive Whether a letter's bracket holds both cases.
     * @return The expression, and the class the literal is where it is one character, which re2c takes for a char
     *         set.
     * @throws Spec_error If the literal is empty or never closes, or an escape is a Unicode one.
     */
    [[nodiscard]] std::pair<std::string, std::optional<Class>> literal(char quote, bool insensitive);

    /**
     * @brief The length of a flex-style reference at the cursor, `{name}` exactly, or zero when there is none.
     * @return The length, brackets included.
     */
    [[nodiscard]] std::size_t reference_length() const noexcept;

    /**
     * @brief The flags every pattern of the block is translated under, the same for the whole pass except that the
     *        evidence of a flex-style definition turns that flag on for the rest of the block.
     */
    /**
     * @brief The whole file, for the labels its C code declares outside every block, which a `goto` in an action
     *        restarts the scan by.
     */
    std::string_view source_;

    /**
     * @brief The offset just past the block's opener, before which a label restarts the scan and after which it
     *        leaves it.
     */
    std::size_t opener_;

    Re2c_flags flags_;

    /**
     * @brief The flags the configurations read so far have left, which the next pass reads the block under.
     */
    Re2c_flags configured_;

    /**
     * @brief The names the block's configurations give the scan pointers, re2c's own until one is renamed.
     */
    Pointers_t& pointers_;

    /**
     * @brief The macros the C around the blocks defines, which an action may call: a call that moves the pointer
     *        or returns through one is out of sight, so such a call is refused by the macro's name.
     */
    const Macros_t& macros_;

    /**
     * @brief Every action of this block with the line to report it at and how a refusal names it, kept until the
     *        block's configurations are all read: re2c applies a configuration to the whole block it stands in,
     *        wherever in the block it is written, so what the actions call the scan pointers is settled by the
     *        block and not by what stood above a rule.
     */
    std::vector<std::tuple<std::string, std::size_t, std::string, bool>> actions_;

    /**
     * @brief Whether this block's actions are the using block's to refuse, which a block read through a `!use:`
     *        directive leaves to the block using it.
     */
    bool deferred_cursors_{false};

    /**
     * @brief The forms besides `return` an action returns a token through, as read() was given them, for the actions
     *        judged once the block's configurations are all read.
     */
    Returning_t returning_;

    /**
     * @brief The line of the configuration that left the scanner under an API other than the default one, none
     *        while it reads under the default: the last assignment governs, as for every configuration, and the
     *        setting carries from a block to the blocks after it and from a used block into the block using it,
     *        as re2c carries it, so it is the file's and not one block's, like the pointer names.
     */
    std::optional<std::size_t>& api_custom_;

    /**
     * @brief Where each definition the block declared stands, for the blocks that use them.
     */
    std::vector<Definition_site> sites_;

    /**
     * @brief Whether regex text ends at the line's end, which a flex-style definition's does.
     */
    bool line_bound_{false};

    /**
     * @brief The indices into the specification's rules of the default rules the `!use:` directives brought in,
     *        ascending, which the block's own default rules override once the block is read.
     */
    std::vector<std::size_t> used_defaults_;

    /**
     * @brief The classes among the definitions in force, shared with the blocks read under the same pass.
     */
    Classes_t& classes_;

    /**
     * @brief Whether a rule naming a condition, `<*>` included, was read, the used blocks' rules counted.
     */
    bool conditioned_{false};

    /**
     * @brief The conditions the block's rules name, `*` aside, in the order first named, the end rule and the empty
     *        rule counted with the rest and the used blocks' rules too: re2c compiles a condition for every name a
     *        rule carries, whether or not that rule is a token.
     */
    std::vector<std::string> named_;

    /**
     * @brief The condition names, `*` and the empty name for a rule naming none included, that some rule other than
     *        the end rule stands in, the used blocks' rules counted: re2c refuses an end rule whose name has no other
     *        rule, in its words.
     */
    std::vector<std::string> ruled_;

    /**
     * @brief The end rules `$` read, each name they stand in with the rule's line, the empty name for one naming no
     *        condition; re2c holds them against `re2c:eof` once the block is read.
     */
    std::vector<std::pair<std::string, std::size_t>> ends_;

    /**
     * @brief The line of the first rule naming no condition, the end rule aside, the used blocks' rules counted.
     */
    std::optional<std::size_t> plain_;

    /**
     * @brief The line of the first end rule `$` naming no condition, the used blocks' rules counted.
     */
    std::optional<std::size_t> plain_end_;

    /**
     * @brief The first refusal held for the settled flags, the used blocks' counted; see deferred().
     */
    std::optional<Spec_error> deferred_;

    /**
     * @brief The line of the configuration that last set the encoding, for the refusal of one the reading has not
     *        got; the block's opening line while none has.
     */
    std::size_t encoding_line_;
};

Block::Block(
        const std::string_view source, const std::size_t begin, const Re2c_flags reading, const Re2c_flags configured,
        Classes_t& classes, Pointers_t& pointers, const Macros_t& macros, std::optional<std::size_t>& api_custom)
    : Cursor{source, begin, source.size()}
    , source_{source}
    , opener_{begin}
    , flags_{reading}
    , configured_{configured}
    , pointers_{pointers}
    , macros_{macros}
    , api_custom_{api_custom}
    , classes_{classes}
    , encoding_line_{line()}
{}

std::size_t Block::read(Lexer_spec& spec, const Library_t& library, const Returning_t& returning)
{
    returning_ = returning;

    // The rules already there are the using block's when this one is read through a `!use:` directive; the block's
    // own begin here, and its default rules are settled among these alone.
    const auto first{spec.rules.size()};

    for (skip_blanks(); !at("*/"); skip_blanks())
    {
        if (!peek())
        {
            fail("the block never closes");
        }

        if (at("re2c:"))
        {
            configuration(spec);

            continue;
        }

        if (at("!use:"))
        {
            at_ += 5;

            std::string name;

            while (peek() && is_name_byte(*peek()))
            {
                name.push_back(next("a block name"));
            }

            skip_blanks();

            expect(';', "';' to end the use directive");

            const auto found{library.find(name)};

            if (found == library.end())
            {
                fail("the used block '" + name + "' is not above this one");
            }

            use(found->second, spec, library, returning);

            continue;
        }

        if (at("!include"))
        {
            fail("the block includes a file, which is not here to read");
        }

        const auto line{this->line()};

        const auto listed{peek() == '<'};

        std::optional<std::vector<std::string>> named{std::vector<std::string>{}};

        if (listed)
        {
            ++at_;

            named = conditions();
        }

        // A flex-style definition, as re2c 3.1 reads one under its flex syntax: a name followed by a blank and then
        // regex, wherever the name stands, at the line's start, indented or after an item on the same line, with the
        // regex running to the end of the line; a `{` after the blanks opens an action or a reference instead and
        // leaves the name a rule's literal. Read as a trial, bound to the line. An action turning up on the line
        // after all is refused, since the definition re2c opened there admits none and re2c answers it with a syntax
        // error. Without the flex flag the trial is made only for a name no definition has, since a defined one
        // opening a rule is normal syntax and an undefined one followed by a blank is nothing else, an undefined
        // symbol to re2c without the flex syntax and a definition under it.
        if (named && named->empty() && is_name_byte(*peek()) && !(*peek() >= '0' && *peek() <= '9'))
        {
            const auto opened{at_};

            const auto name_end{std::min(
                    text_.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", at_),
                    text_.size())};

            const std::string name{text_.substr(at_, name_end - at_)};

            const auto blank_after{name_end < text_.size() && (text_[name_end] == ' ' || text_[name_end] == '\t')};

            const auto after_blanks{std::min(text_.find_first_not_of(" \t", name_end), text_.size())};

            const auto opens_definition{blank_after && after_blanks < text_.size() && text_[after_blanks] != '{'};

            if (opens_definition && (flags_.flex_syntax || !spec.definitions.contains(name)))
            {
                const auto line_end{std::min(text_.find('\n', at_), text_.size())};

                // The body is read under the flex syntax, whose literals bare names are; the flag stays if it is one.
                const auto flex_before{std::exchange(flags_.flex_syntax, true)};

                at_ = name_end;

                line_bound_ = true;

                auto [body, expression, points]{regex_text(spec.definitions)};

                line_bound_ = false;

                if (at_ >= line_end && !body.empty())
                {
                    spec.definitions.insert_or_assign(name, expression);

                    note_class(classes_, name, std::move(points));

                    sites_.push_back({.name = name, .begin = name_end, .line_bound = true});

                    // The evidence of the flex syntax is no configuration: it governs the reading from here on and
                    // the next pass and block alike.
                    configured_.flex_syntax = true;

                    continue;
                }

                if (!body.empty())
                {
                    fail("under the flex syntax '" + name +
                         "' followed by a blank opens a definition, which ends with its line, so re2c answers what "
                         "follows the regex on this line with a syntax error" +
                         (flex_before ? std::string{} :
                                        ", and without that syntax '" + name + "' is a symbol no definition binds"));
                }

                flags_.flex_syntax = flex_before;

                at_ = opened;
            }
        }

        auto [pattern, expression, points]{regex_text(spec.definitions)};

        // re2c's own definition: a name, `=`, its body and `;`. The name was read as regex text, one bare name.
        if (peek() == '=' && !at("=>"))
        {
            ++at_;

            const auto body_begin{at_};

            auto [body, body_expression, body_points]{regex_text(spec.definitions)};

            if (body.empty())
            {
                fail("the definition '" + pattern + "' has no regex");
            }

            if (peek() != ';')
            {
                fail("expected ';' to close the definition '" + pattern + "'");
            }

            ++at_;

            spec.definitions.insert_or_assign(pattern, body_expression);

            note_class(classes_, pattern, std::move(body_points));

            sites_.push_back({.name = pattern, .begin = body_begin, .line_bound = false});

            continue;
        }

        // The entry rule `<>`, an empty condition list and no regex, whose code re2c runs in the condition it numbers
        // zero before any rule is tried, and a `<!c>` setup rule, whose code it runs before every action of its
        // conditions, match nothing and are no tokens; one whose code returns would return in that condition before
        // any rule's own action, which no token set is, so it is refused.
        const auto entry{listed && named && named->empty() && pattern.empty()};

        if (named && !entry && pattern.empty())
        {
            fail("a rule has no regex");
        }

        auto code{action()};

        if (entry || !named)
        {
            if (returned(code, returning))
            {
                fail(entry ? "the entry rule <> returns, so the scanner returns in its first condition before any rule "
                             "is tried" :
                             "a setup rule returns, so every rule of its conditions returns it before its own action");
            }

            // re2c writes this code into the actions it runs before, so a scan pointer it moves is moved there,
            // leaving the next token beginning somewhere other than where a match ended, exactly as a rule's own
            // action moving it would. The pointer's name is the block's, which the block settles, so the check
            // waits for the block's last configuration.
            actions_.emplace_back(code, line, entry ? "the entry rule <> moves " : "a setup rule moves ", false);

            continue;
        }

        // re2c 3.1 compiles a block's rules with conditions or without, never both: a rule naming a condition, `<*>`
        // included, is one kind and a rule naming none, the empty rule and the default rule among them, the other,
        // and a block holding both is refused once it is read, at the first rule naming none; the end rule `$` is
        // the one rule naming none that re2c counts otherwise, alone beside conditions it is refused in its own
        // words. A used block's rules count with the using block's own, where the directive stands.
        if (named && !named->empty())
        {
            conditioned_ = true;

            for (const auto& name : *named)
            {
                if (name != "*" && !std::ranges::contains(named_, name))
                {
                    named_.push_back(name);
                }
            }
        }
        else if (named && pattern == "$")
        {
            plain_end_ = plain_end_ ? plain_end_ : std::optional{line};
        }
        else if (named)
        {
            plain_ = plain_ ? plain_ : std::optional{line};
        }

        // The end rule is no token, and neither is the empty rule `""`, with or without trailing context, which
        // consumes nothing where nothing else matches.
        const auto empty{
                (pattern.starts_with("\"\"") || pattern.starts_with("''")) &&
                (pattern.size() == 2 || pattern.find_first_not_of(' ', 2) == pattern.find('/', 2))};

        // Which names the rule stands in, for re2c's own checks of the end rule: a rule naming none stands in the
        // empty name, and `<*>` is a name of its own here, since re2c counts an end rule under `<*>` against the
        // other `<*>` rules and not against each condition's.
        for (const auto& name : named->empty() ? std::vector<std::string>{""} : *named)
        {
            if (pattern == "$")
            {
                ends_.emplace_back(name, line);
            }
            else if (!std::ranges::contains(ruled_, name))
            {
                ruled_.push_back(name);
            }
        }

        if (pattern == "$" || empty)
        {
            continue;
        }

        // The default rule `*` is a rule like the others as far as its action goes: re2c 3.1 runs it where no other
        // rule matches, over one code unit, which is one byte under every encoding the reading follows, UTF-8
        // included, where `[^]` is a whole code point and `*` a byte of one. Its priority is the lowest wherever in
        // the block it stands, which read_re2c() settles once the block's rules are all read.
        if (pattern == "*")
        {
            expression = R"([\x00-\xff])";
        }

        actions_.emplace_back(code, line, "the action ", true);

        auto token{returned(code, returning)};

        spec.rules.push_back(
                {.pattern = std::move(pattern),
                 .expression = std::move(expression),
                 .conditions = std::move(*named),
                 .action = std::move(code),
                 .token = std::move(token),
                 .priority = std::nullopt,
                 .line = line});
    }

    settle_defaults(spec, first, used_defaults_);

    // An imported default rule this block's own default overrides is gone from the scanner, and its action with it:
    // re2c emits no code for it, so what that code moves moves nothing.
    std::erase_if(actions_, [&spec](const auto& entry) {
        const auto& [code, line, what, of_rule]{entry};

        return of_rule && std::ranges::none_of(spec.rules, [&line, &code](const auto& rule) {
                   return rule.line == line && rule.action == code;
               });
    });

    // A block read through a `!use:` directive is compiled where it is used, under the using block's
    // configurations, which may stand after the directive: its actions wait for that block to finish.
    if (!deferred_cursors_)
    {
        refuse_moved_cursors();
    }

    return at_ + 2;
}

void Block::judge_later() noexcept
{
    deferred_cursors_ = true;
}

void Block::refuse_moved_cursors()
{
    // The API is refused where it is read under, by a block with rules or actions of its own: a block that only
    // configures leaves the setting to the blocks after it, which may set it back before any rule.
    if (api_custom_ && !actions_.empty())
    {
        throw Spec_error{
                "the block sets api, so the scanner advances by calling the API's own operations rather than by "
                "stepping a scan pointer, and an action moving the match is out of the audit's sight",
                *api_custom_};
    }

    std::vector<std::string> names;

    for (const auto& [canonical, pointer] : pointers_)
    {
        names.push_back(pointer);
    }

    std::vector<std::string_view> meaningful{names.begin(), names.end()};

    // The labels the file's C code declares outside every block and before this one, as c_tokens() reads it with
    // the blocks' comments dropped: a name and a colon opening a statement, `loop:`, which a `goto loop;` in an
    // action restarts the scan by when nothing between the label and the block leaves; `case` and `default` labels
    // and a `::` are none, and a label after the block is left for.
    std::set<std::string, std::less<>> restarts;

    const auto outside{c_tokens(source_)};

    for (std::size_t at{0}; at + 1 < outside.size(); ++at)
    {
        const auto opens{
                at == 0 || outside[at - 1].text == ";" || outside[at - 1].text == "{" || outside[at - 1].text == "}" ||
                outside[at - 1].text == ")" || outside[at - 1].text == ":"};

        const auto name{
                std::isalpha(static_cast<unsigned char>(outside[at].text.front())) != 0 ||
                outside[at].text.front() == '_'};

        const auto colon{
                outside[at + 1].text == ":" && (at + 2 >= outside.size() || outside[at + 2].text != ":") &&
                (at == 0 || outside[at - 1].text != ":")};

        // A label after the block, `done:` past the scanning loop, is left for, not restarted at; and a label
        // before it restarts the scan only where the code from the label to the block's opener runs into the
        // block on every path, holding no return, no jump and no other label, since `emit_token: return 9;` and
        // `loop: if (finished) return 0;` before the block leave the scan where a `goto` reaches them.
        auto inert{outside[at].at < opener_};

        for (auto piece{at + 2}; inert && piece < outside.size() && outside[piece].at < opener_; ++piece)
        {
            const auto& text{outside[piece].text};

            const auto labels{
                    piece + 1 < outside.size() && outside[piece + 1].text == ":" &&
                    (piece + 2 >= outside.size() || outside[piece + 2].text != ":") &&
                    (std::isalpha(static_cast<unsigned char>(text.front())) != 0 || text.front() == '_') &&
                    text != "case" && text != "default"};

            inert = text != "return" && text != "goto" && text != "break" && text != "continue" && !labels &&
                    !std::ranges::contains(returning_, text);
        }

        if (opens && name && colon && outside[at].text != "case" && outside[at].text != "default" && inert)
        {
            restarts.emplace(outside[at].text);
        }
    }

    for (const auto& [code, line, what, of_rule] : actions_)
    {
        if (const auto moved{moved_cursor(code, names, macros_)})
        {
            throw Spec_error{what + *moved, line};
        }

        if (const auto use{directive_use(code)})
        {
            throw Spec_error{"the action " + *use, line};
        }

        // A rule's action falls into the next rule's unless it leaves; a shortcut rule, `:=> COMMENT`, has no code
        // of its own and re2c writes the jump to the condition itself, and a transition rule's `=> COMMENT` before
        // its code sets the condition and is no statement of the action's.
        auto judged{std::string_view{code}};

        judged.remove_prefix(std::min(judged.find_first_not_of(" \t\r\n"), judged.size()));

        const auto shortcut{judged.starts_with(":=>")};

        if (judged.starts_with("=>"))
        {
            judged.remove_prefix(2);

            judged.remove_prefix(std::min(judged.find_first_not_of(" \t"), judged.size()));

            judged.remove_prefix(std::min(
                    judged.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"),
                    judged.size()));
        }

        if (const auto use{returns_undecided(judged, returning_, false, restarts, of_rule && !shortcut)})
        {
            throw Spec_error{"the action " + *use, line};
        }

        if (const auto use{macro_use(code, macros_, meaningful)})
        {
            throw Spec_error{"the action " + *use, line};
        }
    }

    actions_.clear();
}

void Block::refuse_mixed_kinds() const
{
    if (conditioned_ && plain_)
    {
        throw Spec_error{
                "cannot mix conditions with normal rules, as re2c answers a scanner holding a rule that names a "
                "condition beside one that names none",
                *plain_};
    }

    if (conditioned_ && plain_end_)
    {
        throw Spec_error{
                "EOF rule without other rules doesn't make sense, as re2c answers an end rule naming no condition in a "
                "scanner whose other rules name one",
                *plain_end_};
    }
}

void Block::use(const std::size_t begin, Lexer_spec& spec, const Library_t& library, const Returning_t& returning)
{
    Block used{text_, begin, flags_, configured_, classes_, pointers_, macros_, api_custom_};

    used.deferred_cursors_ = true;

    const auto before{spec.rules.size()};

    std::ignore = used.read(spec, library, returning);

    // The used block's actions are this block's to refuse, under the names this block's configurations leave.
    actions_.insert(actions_.end(), used.actions_.begin(), used.actions_.end());

    for (auto index{before}; index < spec.rules.size(); ++index)
    {
        if (spec.rules[index].pattern == "*")
        {
            used_defaults_.push_back(index);
        }
    }

    // The used block's rules are of the kinds they are wherever they stand, the first of a kind the using block's
    // own if it read one before the directive.
    conditioned_ = conditioned_ || used.conditioned_;

    for (const auto& name : used.named_)
    {
        if (!std::ranges::contains(named_, name))
        {
            named_.push_back(name);
        }
    }

    for (const auto& name : used.ruled_)
    {
        if (!std::ranges::contains(ruled_, name))
        {
            ruled_.push_back(name);
        }
    }

    ends_.insert(ends_.end(), used.ends_.begin(), used.ends_.end());

    plain_ = plain_ ? plain_ : used.plain_;

    plain_end_ = plain_end_ ? plain_end_ : used.plain_end_;

    deferred_ = deferred_ ? deferred_ : used.deferred_;

    configured_ = used.flags();

    sites_.insert(sites_.end(), used.sites().begin(), used.sites().end());
}

Re2c_flags Block::flags() const noexcept
{
    return configured_;
}

const std::vector<std::string>& Block::named() const noexcept
{
    return named_;
}

const std::vector<std::string>& Block::ruled() const noexcept
{
    return ruled_;
}

const std::vector<std::pair<std::string, std::size_t>>& Block::ends() const noexcept
{
    return ends_;
}

const std::vector<Definition_site>& Block::sites() const noexcept
{
    return sites_;
}

const std::optional<Spec_error>& Block::deferred() const noexcept
{
    return deferred_;
}

std::size_t Block::encoding_line() const noexcept
{
    return encoding_line_;
}

std::pair<std::string, std::optional<Class>> Block::definition(
        const bool line_bound, const regex::Definitions_t& definitions)
{
    // A flex-style definition's body is read under the flex syntax, whose literals bare names are, and ends where
    // its line does; the re2c spelling ends at the `;` that closes it. Both leave the block as they found it, a
    // refused regex excepted, which leaves the block to the caller to drop.
    const auto flex{std::exchange(flags_.flex_syntax, flags_.flex_syntax || line_bound)};

    line_bound_ = line_bound;

    auto [body, expression, points]{regex_text(definitions)};

    line_bound_ = false;

    flags_.flex_syntax = flex;

    return {std::move(expression), std::move(points)};
}

void Block::configuration(Lexer_spec& spec)
{
    // The value may be a quoted string holding a ';' of its own, as a YYFILL definition usually does.
    auto end{at_};

    while (end < end_ && text_[end] != ';')
    {
        if (text_[end] == '"' || text_[end] == '\'')
        {
            for (const auto quote{text_[end++]}; end < end_ && text_[end] != quote; ++end)
            {
                end += text_[end] == '\\' ? 1 : 0;
            }
        }

        ++end;
    }

    if (end >= end_)
    {
        fail("a configuration is never closed with ';'");
    }

    std::string option{text_.substr(at_ + 5, end - at_ - 5)};

    // Blanks around the '=' say nothing; one spelling per configuration keeps the options comparable.
    std::erase_if(option, [](const char byte) { return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r'; });

    // The flags that change the reading are honoured when set in the file, under every spelling the manual's
    // configuration list gives them: the canonical name and its `flags:` aliases. A configuration governs the whole
    // block wherever in it it stands, and the last assignment is the one that governs, so what is set here is what
    // the block leaves and the next pass reads every pattern of it under; nothing here changes this pass. There is
    // no configuration for the flex syntax, `re2c:flags:F` being a configuration re2c rejects, so only the command
    // line brings that one.
    const auto set{[&option](const auto& names) -> std::optional<bool> {
        for (const auto name : names)
        {
            if (const auto key{std::string{name} + '='}; option.starts_with(key))
            {
                return option.substr(key.size()) != "0";
            }
        }

        return std::nullopt;
    }};

    static constexpr std::array<std::string_view, 2> inverted{"case-inverted", "flags:case-inverted"};

    static constexpr std::array<std::string_view, 2> insensitive{"case-insensitive", "flags:case-insensitive"};

    if (const auto value{set(inverted)})
    {
        configured_.case_inverted = *value;
    }

    if (const auto value{set(insensitive)})
    {
        configured_.case_insensitive = *value;
    }

    // An encoding is a configuration of its own, under the three names the manual gives each; setting one to 0
    // leaves the block reading ASCII again.
    struct Encoding_names
    {
        Re2c_encoding encoding;

        std::array<std::string_view, 3> names;
    };

    static constexpr std::array<Encoding_names, 5> encodings{
            {{.encoding = Re2c_encoding::ebcdic, .names = {"encoding:ebcdic", "flags:ecb", "flags:e"}},
             {.encoding = Re2c_encoding::ucs2, .names = {"encoding:ucs2", "flags:wide-chars", "flags:w"}},
             {.encoding = Re2c_encoding::utf8, .names = {"encoding:utf8", "flags:utf-8", "flags:8"}},
             {.encoding = Re2c_encoding::utf16, .names = {"encoding:utf16", "flags:utf-16", "flags:x"}},
             {.encoding = Re2c_encoding::utf32, .names = {"encoding:utf32", "flags:unicode", "flags:u"}}}};

    for (const auto& [encoding, names] : encodings)
    {
        const auto value{set(names)};

        if (!value)
        {
            continue;
        }

        configured_.encoding = *value                           ? encoding :
                               configured_.encoding == encoding ? Re2c_encoding::ascii :
                                                                  configured_.encoding;

        // Whether the encoding is one the reading has got is the settled configuration's to say, the last
        // assignment governing, so a block turning UTF-16 on and off again reads as ASCII; the caller asks after.
        encoding_line_ = line();
    }

    // The policy decides what becomes of the surrogates, which the default encoding of them as code points is.
    if (option.starts_with("encoding-policy=") || option.starts_with("flags:encoding-policy="))
    {
        if (!option.ends_with("=ignore"))
        {
            fail("an encoding policy other than the default leaves the surrogates matched otherwise than the audit "
                 "reads them");
        }
    }

    // The reading models re2c's default API, where the scanner advances by stepping a pointer an action can be
    // seen to step too. Under the custom API it advances by calling `YYSKIP`, and backs up and restores by calling
    // `YYBACKUP` and `YYRESTORE`, which an action may call as plainly as the scanner does and which name no
    // pointer at all, so an action moving the match is out of this reading's sight. Which API the block reads
    // under is the block's last word on it, as every configuration is, so the choice is recorded and the block is
    // refused at its end, and `api = custom;` followed by `api = default;` reads under the default. The style
    // names the spelling of those operations and decides nothing while the API is the default one.
    for (const auto key : {"api=", "flags:input="})
    {
        if (option.starts_with(key))
        {
            api_custom_ = option.ends_with("=default") ? std::nullopt : std::optional{line()};
        }
    }

    // A `define:` configuration renaming a scan pointer says what the actions of this block call it. re2c reads
    // the value quoted or bare, `= "cur";` and `= cur;` naming the same pointer, so the quotes are not part of the
    // name the actions then write.
    for (auto& [canonical, pointer] : pointers_)
    {
        if (const auto key{"define:" + canonical + '='}; option.starts_with(key))
        {
            auto value{option.substr(key.size())};

            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            {
                value = value.substr(1, value.size() - 2);
            }

            // Parentheses around the whole name name the same pointer, `"(cur)"` and `"cur"` alike, and the
            // actions write it without them, so the ones wrapping it are taken off.
            for (auto wrapped{true}; wrapped && value.size() >= 2 && value.front() == '(' && value.back() == ')';)
            {
                for (std::size_t at{0}, depth{0}; at + 1 < value.size(); ++at)
                {
                    depth += value[at] == '(' ? 1 : value[at] == ')' ? -1 : 0;

                    wrapped = depth > 0;

                    if (!wrapped)
                    {
                        break;
                    }
                }

                value = wrapped ? value.substr(1, value.size() - 2) : value;
            }

            pointer = std::move(value);
        }
    }

    spec.options.push_back(std::move(option));

    at_ = end + 1;
}

std::optional<std::vector<std::string>> Block::conditions()
{
    // A setup rule's `!` may stand after blanks, `< ! C >`, as re2c reads it.
    const auto mark{text_.find_first_not_of(" \t", at_)};

    const auto setup{mark != std::string_view::npos && text_[mark] == '!'};

    std::vector<std::string> names;

    std::string name;

    for (;;)
    {
        const auto byte{next("'>' to close the condition list")};

        if (byte == '>' || byte == ',')
        {
            if (!name.empty())
            {
                names.push_back(std::exchange(name, {}));
            }

            if (byte == '>')
            {
                break;
            }

            continue;
        }

        if (byte != ' ' && byte != '\t' && byte != '!')
        {
            name.push_back(byte);
        }
    }

    return setup ? std::nullopt : std::optional{std::move(names)};
}

std::tuple<std::string, std::string, std::optional<Class>> Block::regex_text(const regex::Definitions_t& definitions)
{
    std::string pattern;

    std::string expression;

    const auto utf8{flags_.encoding == Re2c_encoding::utf8};

    // The expression is shaped as re2c's grammar shapes it, so that a class difference `A \ B` takes the operands
    // re2c gives it: an alternation is terms joined by `|`, a term is what stands concatenated since the last `|`
    // or the group's opening, and the difference takes the whole term on either side, so `[y] [a-z] \ [x]` and
    // `[a-z] \ [x] [y]` are both differences of a concatenation, which re2c refuses as no char set. A char set is
    // one class atom and nothing more: a bracket, the dot, a one-character literal, a name defined as one, or a
    // group whose alternatives are each one, which re2c merges into one class. The pattern parser has no
    // difference, so the term is replaced by the class of the code points left as soon as the right operand's term
    // ends, at the next `|`, `\`, `/`, the group's close or the regex's end.
    struct Level
    {
        // Where the current term's text begins in the expression.
        std::size_t term;

        // The atoms placed in the current term.
        std::size_t atoms;

        // The class the term is while it is one class atom and nothing more.
        std::optional<Class> single;

        // The left operand of the difference the current term is the right operand of, once a `\` was read.
        std::optional<Class> left;

        // Whether every term closed in this group so far was a class.
        bool classes;

        // The union of those, which is the group's class when they all were.
        Class branches;
    };

    std::vector<Level> levels{
            {.term = 0, .atoms = 0, .single = std::nullopt, .left = std::nullopt, .classes = true, .branches = {}}};

    // An atom joins the current term: the first atom's class is the term's until anything more joins it.
    const auto place{[&levels](std::optional<Class> points) {
        auto& level{levels.back()};

        level.single = level.atoms == 0 ? std::move(points) : std::nullopt;

        ++level.atoms;
    }};

    // The term ends as the right operand of the difference a `\` opened: the class of the code points left replaces
    // the text of both operands.
    const auto resolve{[this, &expression, &levels]() {
        auto& level{levels.back()};

        if (!level.left)
        {
            return;
        }

        if (!level.single)
        {
            fail(R"(re2c can only difference char sets, and what follows the '\' is no class)");
        }

        auto remaining{subtracted(*level.left, *level.single)};

        // Which code points the operands hold is the flags' to say, so a difference empty under this pass's flags
        // is refused only once the flags have settled; the pass goes on with the empty class.
        if (remaining.empty() && !deferred_)
        {
            deferred_ = Spec_error{"the class difference leaves an empty class, which matches nothing", line()};
        }

        expression.erase(level.term);

        expression += rendered(remaining, flags_.encoding);

        level.single = std::move(remaining);

        level.left.reset();
    }};

    // A term ends at `|`, `/`, a group's close or the regex's end, and joins the group's alternatives, which are a
    // class together only while each is one.
    const auto close_term{[&levels, &resolve]() {
        resolve();

        auto& level{levels.back()};

        if (level.single)
        {
            level.branches = united(level.branches, *level.single);
        }
        else
        {
            level.classes = false;
        }
    }};

    // A new term begins where the expression now ends.
    const auto open_term{[&levels, &expression]() {
        auto& level{levels.back()};

        level.term = expression.size();

        level.atoms = 0;

        level.single.reset();
    }};

    // The class a definition is, when it is one.
    const auto defined{[this](const std::string_view name) -> std::optional<Class> {
        const auto found{classes_.find(name)};

        return found == classes_.end() ? std::nullopt : std::optional{found->second};
    }};

    for (;;)
    {
        if (!peek() || at("*/"))
        {
            fail("expected an action before the end of the block");
        }

        const auto byte{*peek()};

        if (line_bound_ && byte == '\n')
        {
            break;
        }

        // What ends the regex, all at the top level: a bare '=', the ';' of a definition's body, or an action.
        // A '{' opens an action unless it is a count, {2,5}, or a flex-style reference, {name}, which re2c reads
        // with its flex-syntax flag and which the parser reads as it stands.
        const auto counted{byte == '{' && at_ + 1 < text_.size() && text_[at_ + 1] >= '0' && text_[at_ + 1] <= '9'};

        const auto referenced{byte == '{' && reference_length() > 0};

        if (byte == ';' || (byte == '{' && !counted && !referenced) || at("=>") || at(":=") || byte == '=')
        {
            break;
        }

        if (referenced)
        {
            const auto length{reference_length()};

            pattern += text_.substr(at_, length);

            expression += text_.substr(at_, length);

            place(defined(text_.substr(at_ + 1, length - 2)));

            at_ += length;

            continue;
        }

        if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r')
        {
            ++at_;

            pattern.push_back(' ');

            continue;
        }

        if (at("//") || at("/*"))
        {
            const auto line_end{std::min(text_.find('\n', at_), text_.size())};

            skip_blanks();

            // Bound to a line, the regex ends with the line, a comment closing on it notwithstanding.
            if (line_bound_ && at_ >= line_end)
            {
                break;
            }

            continue;
        }

        // Which quote is the case-insensitive one is the flags' to say; the exact double-quoted literal is the
        // parser's own, the others become bracket sequences.
        const auto insensitive{flags_.case_insensitive || (byte == '\'') != flags_.case_inverted};

        if ((byte == '"' && !insensitive) || byte == '[')
        {
            // Copied through with its escapes: the parser reads both forms as they stand.
            const auto close{byte == '"' ? '"' : ']'};

            const auto opened{at_};

            std::string copied{next("a quote")};

            // re2c closes a bracket at the first unescaped ']', a literal one being spelled '\]'.
            for (;;)
            {
                const auto inner{next(close == '"' ? R"('"' to close the quoted text)" : "']' to close the bracket")};

                copied.push_back(inner);

                if (inner == '\\')
                {
                    copied.push_back(next("the escaped byte"));

                    continue;
                }

                if (inner == close)
                {
                    break;
                }
            }

            // An escape's first byte is the backslash, so `\\u` is a backslash and a letter, not a code point.
            for (std::size_t index{0}; index + 1 < copied.size(); ++index)
            {
                if (copied[index] != '\\')
                {
                    continue;
                }

                if (copied[index + 1] == 'u' || copied[index + 1] == 'U' || copied[index + 1] == 'X')
                {
                    at_ = opened;

                    fail("a Unicode escape needs an encoding the byte reading has not got");
                }

                if (copied[index + 1] == 'x' && index + 2 < copied.size() && copied[index + 2] == '{')
                {
                    at_ = opened;

                    fail(std::string{braced_escape});
                }

                ++index;
            }

            if (copied == "[]")
            {
                at_ = opened;

                fail("an empty class matches nothing");
            }

            if (utf8 &&
                std::ranges::any_of(copied, [](const char one) { return static_cast<unsigned char>(one) >= 0x80; }))
            {
                at_ = opened;

                fail("a byte beyond ASCII in the source stands for the code points the --input-encoding option says, "
                     "which no file carries, so under the utf8 encoding it is not read");
            }

            pattern += copied;

            if (byte == '[')
            {
                // The byte parser's reading of the bracket gives the code points it admits, which under UTF-8 the
                // expression must spell as their encodings, so a bracket the parser reads otherwise is refused
                // there, while under the byte encodings it stands as written for the parser to read when the
                // scanner is built; re2c's [^] is any byte there, which the parser would read as a member.
                auto points{code_points(copied, definitions, code_space(flags_.encoding))};

                if (utf8 && !points)
                {
                    at_ = opened;

                    fail("the class '" + copied + "' is no class whose code points the reading can take");
                }

                if (utf8)
                {
                    expression += ascii(*points) ? copied : step(*points);
                }
                else
                {
                    expression += copied == "[^]" ? std::string{R"([\x00-\xff])"} : copied;
                }

                place(std::move(points));

                continue;
            }

            // The bytes the literal spells, as the parser reads them; the empty literal spells none, matching nothing.
            const auto bytes{[&copied, &definitions]() -> std::optional<std::string> {
                if (copied == R"("")")
                {
                    return std::string{};
                }

                try
                {
                    return text_of(regex::parse(copied, definitions, re2c_parse));
                }
                catch (const regex::Syntax_error&)
                {
                    return std::nullopt;
                }
            }()};

            if (utf8 && !bytes)
            {
                at_ = opened;

                fail("the literal " + copied + " is no text whose code points the reading can take");
            }

            const auto beyond_ascii{[](const char one) { return static_cast<unsigned char>(one) >= 0x80; }};

            // Under UTF-8 every character of the literal is a code point of its own, which the encoding spells in one
            // byte or two; where all are ASCII, and under the byte encodings, the literal stands as written.
            if (utf8 && std::ranges::any_of(*bytes, beyond_ascii))
            {
                for (const auto one : *bytes)
                {
                    const auto point{static_cast<char32_t>(static_cast<unsigned char>(one))};

                    expression += step({{.first = point, .last = point}});
                }
            }
            else
            {
                expression += copied;
            }

            // A literal of one character is a char set to re2c.
            if (bytes && bytes->size() == 1)
            {
                const auto point{static_cast<char32_t>(static_cast<unsigned char>(bytes->front()))};

                place(Class{{.first = point, .last = point}});
            }
            else
            {
                place(std::nullopt);
            }

            continue;
        }

        if (byte == '\'' || byte == '"')
        {
            const auto opened{at_};

            ++at_;

            auto [rewritten, points]{literal(byte, insensitive)};

            const auto quoted{text_.substr(opened, at_ - opened)};

            if (utf8 &&
                std::ranges::any_of(quoted, [](const char one) { return static_cast<unsigned char>(one) >= 0x80; }))
            {
                at_ = opened;

                fail("a byte beyond ASCII in the source stands for the code points the --input-encoding option says, "
                     "which no file carries, so under the utf8 encoding it is not read");
            }

            pattern += text_.substr(opened, at_ - opened);

            expression += rewritten;

            place(std::move(points));

            continue;
        }

        if (is_name_byte(byte) && !(byte >= '0' && byte <= '9'))
        {
            std::string name;

            while (peek() && is_name_byte(*peek()))
            {
                name.push_back(next("a name"));
            }

            pattern += name;

            // Under the flex syntax a bare name is the literal it spells, a char set to re2c where it is one letter.
            if (flags_.flex_syntax)
            {
                expression += name;

                const auto point{static_cast<char32_t>(static_cast<unsigned char>(name.front()))};

                place(name.size() == 1 ? std::optional{Class{{.first = point, .last = point}}} : std::nullopt);
            }
            else
            {
                expression += '{' + name + '}';

                place(defined(name));
            }

            continue;
        }

        // re2c's dot is any code point but the newline, which under UTF-8 is every encoding but that byte's.
        if (byte == '.')
        {
            ++at_;

            pattern.push_back(byte);

            Class points{
                    {.first = 0, .last = U'\n' - 1},
                    {.first = U'\n' + 1, .last = code_space(flags_.encoding) - 1}};

            expression += utf8 ? step(points) : std::string{byte};

            place(std::move(points));

            continue;
        }

        // A tag, `@name` or `#name`, marks a position and matches nothing; kept as written and dropped from the
        // expression, and no char set to re2c.
        if ((byte == '@' || byte == '#') && at_ + 1 < end_ && is_name_byte(text_[at_ + 1]))
        {
            const auto begin{at_};

            for (++at_; peek() && is_name_byte(*peek()); ++at_)
            {
            }

            pattern += text_.substr(begin, at_ - begin);

            place(std::nullopt);

            continue;
        }

        // The class difference: the term before it is the left operand, a chain's earlier difference resolved
        // first, and the term after it, whose text joins the left one's until the resolution replaces both, is the
        // right one.
        if (byte == '\\')
        {
            if (levels.back().atoms == 0)
            {
                fail("the class difference has no class before it");
            }

            resolve();

            auto& level{levels.back()};

            if (!level.single)
            {
                fail(R"(re2c can only difference char sets, and what stands before the '\' is no class)");
            }

            level.left = std::move(level.single);

            level.single.reset();

            level.atoms = 0;

            ++at_;

            pattern.push_back(byte);

            continue;
        }

        ++at_;

        pattern.push_back(byte);

        if (byte == '(')
        {
            expression.push_back(byte);

            // `(!R)` is R in a group that captures nothing, re2c's spelling, the mark standing after the opening
            // whatever blanks or comments come between, as re2c reads its regex tokens, a newline and a comment
            // among them where no line bounds the regex; the mark is no member of the group.
            const auto mark{[this]() {
                auto found{at_};

                for (;;)
                {
                    const std::string_view blanks{line_bound_ ? " \t" : " \t\r\n"};

                    found = std::min(text_.find_first_not_of(blanks, found), end_);

                    const auto rest{text_.substr(found, end_ - found)};

                    if (line_bound_ || !(rest.starts_with("/*") || rest.starts_with("//")))
                    {
                        return found;
                    }

                    const auto close{rest.starts_with("//") ? text_.find('\n', found) : text_.find("*/", found + 2)};

                    const auto width{rest.starts_with("//") ? 1UZ : 2UZ};

                    if (close == std::string_view::npos || close + width > end_)
                    {
                        return found;
                    }

                    found = close + width;
                }
            }()};

            if (mark < end_ && text_[mark] == '!')
            {
                pattern += text_.substr(at_, mark + 1 - at_);

                at_ = mark + 1;
            }

            levels.push_back(
                    {.term = expression.size(),
                     .atoms = 0,
                     .single = std::nullopt,
                     .left = std::nullopt,
                     .classes = true,
                     .branches = {}});

            continue;
        }

        if (byte == ')' && levels.size() > 1)
        {
            close_term();

            auto group{std::move(levels.back())};

            levels.pop_back();

            expression.push_back(byte);

            place(group.classes ? std::optional{std::move(group.branches)} : std::nullopt);

            continue;
        }

        // An alternative ends the term, and so does a trailing context, which makes the regex no class.
        if (byte == '|' || byte == '/')
        {
            close_term();

            levels.back().classes = levels.back().classes && byte == '|';

            expression.push_back(byte);

            open_term();

            continue;
        }

        // Anything else, a repetition, a count or a byte the parser will refuse, makes the term more than one class.
        expression.push_back(byte);

        levels.back().single.reset();
    }

    close_term();

    while (!pattern.empty() && pattern.back() == ' ')
    {
        pattern.pop_back();
    }

    while (!pattern.empty() && pattern.front() == ' ')
    {
        pattern.erase(0, 1);
    }

    // The regex is a class where its alternatives, at the top level, are each one class atom, as re2c merges them.
    auto points{
            levels.size() == 1 && levels.front().classes ? std::optional{std::move(levels.front().branches)} :
                                                           std::nullopt};

    return {std::move(pattern), std::move(expression), std::move(points)};
}

std::string Block::action()
{
    std::string code;

    // A shortcut rule, `:=> condition`, has no code at all: it ends with the condition's name, so the line ends it
    // and the lines after it are items of their own.
    if (at(":=>"))
    {
        while (peek() && *peek() != ';' && *peek() != '\n')
        {
            code.push_back(next("the condition the shortcut rule jumps to"));
        }

        if (peek() == ';')
        {
            code.push_back(next("';'"));
        }

        return code;
    }

    // A transition names a condition first and an action of either kind follows it; it is kept as text, since which
    // condition follows says nothing about the token.
    if (at("=>"))
    {
        while (peek() && *peek() != '{' && !at(":=") && *peek() != ';' && *peek() != '\n')
        {
            code.push_back(next("the transition"));
        }

        if (peek() == ';')
        {
            code.push_back(next("';'"));

            return code;
        }
    }

    if (at(":="))
    {
        // A `:=` action ends on a newline followed by a non-whitespace character, so a line beginning with a blank,
        // or an empty line, continues it; the block's own close, at the line's start, ends it like any other.
        const auto ends{[this] {
            const auto after{at_ + 1};

            return after >= end_ ||
                   !(text_[after] == ' ' || text_[after] == '\t' || text_[after] == '\r' || text_[after] == '\n');
        }};

        while (peek() && !(*peek() == '\n' && ends()))
        {
            code.push_back(next("the action"));
        }

        return code;
    }

    if (peek() != '{')
    {
        fail("expected an action, a '{' block or ':=' and the rest of the line");
    }

    const auto close{brace_close(text_.substr(at_))};

    if (!close)
    {
        fail("the action's braces never close");
    }

    code += text_.substr(at_, *close);

    at_ += *close;

    return code;
}

std::pair<std::string, std::optional<Class>> Block::literal(const char quote, const bool insensitive)
{
    std::string expression;

    // The characters read, and the class of the first, which is the literal's where it is the only one.
    std::size_t characters{0};

    Class first;

    const auto character{[&characters, &first](Class points) {
        if (++characters == 1)
        {
            first = std::move(points);
        }
    }};

    const auto one{[](const char32_t point) { return Class{{.first = point, .last = point}}; }};

    for (;;)
    {
        auto byte{next(std::string{"'"} + quote + "' to close the quoted text")};

        if (byte == quote)
        {
            break;
        }

        if (byte == '\\')
        {
            // An escape stands for one byte, which the parser decodes inside a bracket as well; one spelling a
            // letter, `\x41`, `\101` or `\A`, is decoded here so its bracket can hold both cases, as re2c folds it.
            const auto escaped{next("the escaped byte")};

            if (escaped == 'u' || escaped == 'U' || escaped == 'X')
            {
                fail("a Unicode escape needs an encoding the byte reading has not got");
            }

            if (escaped == 'x' && peek() == '{')
            {
                fail(std::string{braced_escape});
            }

            std::string text{'\\', escaped};

            const auto numeric{escaped == 'x' || (escaped >= '0' && escaped <= '7')};

            auto value{0};

            if (escaped == 'x')
            {
                while (text.size() < 4 && peek() && is_hex_digit(*peek()))
                {
                    const auto digit{next("a hex digit")};

                    text.push_back(digit);

                    value = value * 16 + (digit <= '9' ? digit - '0' : (digit | 0x20) - 'a' + 10);
                }
            }
            else if (escaped >= '0' && escaped <= '7')
            {
                value = escaped - '0';

                while (text.size() < 4 && peek() && *peek() >= '0' && *peek() <= '7')
                {
                    const auto digit{next("an octal digit")};

                    text.push_back(digit);

                    value = value * 8 + (digit - '0');
                }
            }

            const auto stands{numeric ? static_cast<char32_t>(value) : decoded(escaped)};

            if (insensitive && stands < 0x80 && is_letter(static_cast<char>(stands)))
            {
                byte = static_cast<char>(stands);
            }
            else if (value >= 0x80 && flags_.encoding == Re2c_encoding::utf8)
            {
                // The escape names a code point, which this encoding spells in two bytes rather than one.
                expression += step(one(static_cast<char32_t>(value)));

                character(one(static_cast<char32_t>(value)));

                continue;
            }
            else
            {
                expression += '[' + text + ']';

                character(one(numeric ? static_cast<char32_t>(value) : decoded(escaped)));

                continue;
            }
        }

        if (insensitive && is_letter(byte))
        {
            const auto lower{static_cast<char>(byte | 0x20)};

            const auto upper{static_cast<char>(byte & ~0x20)};

            expression += std::string{'['} + lower + upper + ']';

            character(
                    {{.first = static_cast<char32_t>(upper), .last = static_cast<char32_t>(upper)},
                     {.first = static_cast<char32_t>(lower), .last = static_cast<char32_t>(lower)}});

            continue;
        }

        expression += '[' + bracket_member(static_cast<unsigned char>(byte)) + ']';

        character(one(static_cast<unsigned char>(byte)));
    }

    if (expression.empty())
    {
        fail("an empty quoted literal matches nothing");
    }

    return {std::move(expression), characters == 1 ? std::optional{std::move(first)} : std::nullopt};
}

std::size_t Block::reference_length() const noexcept
{
    if (peek() != '{')
    {
        return 0;
    }

    auto end{at_ + 1};

    while (end < text_.size() && is_name_byte(text_[end]))
    {
        ++end;
    }

    const auto named{end > at_ + 1 && !(text_[at_ + 1] >= '0' && text_[at_ + 1] <= '9')};

    return named && end < text_.size() && text_[end] == '}' ? end + 1 - at_ : 0;
}

} // namespace

std::vector<Lexer_spec> read_re2c(
        const std::string_view source, Re2c_flags flags, const Returning_t& returning, const Include_reader_t& includes)
{
    // An encoding comes from the command line as well as from a configuration, and the reading models the same two
    // either way, so the flags the caller passes are refused where a configuration setting them would be.
    if (const auto refusal{unreadable(flags.encoding)}; !refusal.empty())
    {
        throw Spec_error{refusal, 1};
    }

    std::vector<Lexer_spec> scanners;

    // What one block leaves for the next: the configurations as options, and where each definition's regex
    // stands, so that the block using it translates the regex itself, never the rules.
    std::vector<std::string> carried_options;

    std::vector<Definition_site> carried_definitions;

    // What the blocks so far call the scan pointers: re2c carries a `define:` configuration from the block it
    // stands in to the blocks after it, so a block renaming none writes what the one above it left, and the file's
    // last word on a name is the one its actions are read under.
    Pointers_t carried_pointers{{"YYCURSOR", "YYCURSOR"}, {"YYMARKER", "YYMARKER"}, {"YYCTXMARKER", "YYCTXMARKER"}};

    // Whether the blocks so far left the scanner under an API other than the default one, and where, carried as
    // the pointer names are.
    std::optional<std::size_t> carried_api;

    // The macros the C around the blocks defines, every `#define` of the file's: an action calling one that
    // returns or moves a pointer is refused by the macro's name, since re2c copies the action as written and the
    // compiler expands it there.
    Macros_t macros;

    take_macros(source, macros);

    // A file the code includes defines macros as the file's own code does, `#define SLOT 0` among them; its text is
    // read for them, its own includes after it and beside it, or the include is refused where no reader reaches the
    // file, since what it defines is out of sight; an angle-bracket include the reader does not find is a system
    // header's, and one named by a macro is out of sight.
    std::vector<Included> texts{{.text = std::string{source}, .path = {}}};

    for (std::size_t at{0}; at < texts.size(); ++at)
    {
        for (const auto& [name, line, form] : includes_of(texts[at].text))
        {
            const auto where{at == 0 ? line + 1 : 1};

            if (form == Include_form::computed)
            {
                throw Spec_error{
                        "the code includes a file named by the macro " + name +
                                ", which the reading does not expand, so what the file defines is out of sight, the "
                                "macros an action may use among them",
                        where};
            }

            const auto included{includes ? includes(name, texts[at].path, form) : std::nullopt};

            if (!included && form == Include_form::angled)
            {
                continue;
            }

            if (!included)
            {
                throw Spec_error{
                        "the code includes \"" + name + "\", a file of its own " +
                                (includes ? "not found beside the file including it or on the include path" :
                                            "the reading does not reach") +
                                ", whose definitions are out of sight, the macros an action may use among them",
                        where};
            }

            if (texts.size() > 64)
            {
                throw Spec_error{"the code includes more files than the reading follows", where};
            }

            take_macros(included->text, macros);

            texts.push_back(*included);
        }
    }

    // The blocks a later one may use by name; the unnamed rules block under the empty name.
    Library_t library;

    // The rules block a use block with no name of its own refers to: the most recent one, named or not.
    std::optional<std::string> recent;

    for (auto at{source.find("/*!")}; at != std::string_view::npos; at = source.find("/*!", at))
    {
        const auto rest{source.substr(at + 3)};

        // A local block reads the definitions and configurations so far and passes none of its own on; a rules
        // block is a library for the blocks that use it and no scanner itself; a use block opens by using one.
        const auto local{rest.starts_with("local:re2c")};

        const auto rules{rest.starts_with("rules:re2c")};

        const auto use{rest.starts_with("use:re2c")};

        const auto opener{
                rest.starts_with("re2c") ? std::size_t{4} :
                rules || local           ? std::size_t{10} :
                use                      ? std::size_t{8} :
                                           0};

        const auto line{1 + static_cast<std::size_t>(std::ranges::count(source.substr(0, at), '\n'))};

        if (opener == 0)
        {
            // Another block kind, which carries no rules; skipped through its close like any comment.
            const auto close{source.find("*/", at + 3)};

            if (close == std::string_view::npos)
            {
                throw Spec_error{"a re2c block is never closed", line};
            }

            at = close + 2;

            continue;
        }

        // The block's name, `rules:re2c:name`, when it has one.
        std::string name;

        auto begin{at + 3 + opener};

        if (source.substr(begin).starts_with(':'))
        {
            for (++begin; begin < source.size() && is_name_byte(source[begin]); ++begin)
            {
                name.push_back(source[begin]);
            }
        }

        // re2c ends the opener at a blank, a newline or the block's close, so `/*!re2cx` opens no block and is refused
        // as re2c refuses it, an ill-formed start of a block.
        if (begin < source.size() && source[begin] != ' ' && source[begin] != '\t' && source[begin] != '\r' &&
            source[begin] != '\n' && !source.substr(begin).starts_with("*/"))
        {
            throw Spec_error{
                    "ill-formed start of a block: re2c expects a blank, a newline, a colon and a name, or "
                    "the block's close after the opener",
                    line};
        }

        // A use block names the rules block it uses, not itself, and with no name it uses the most recent one.
        if (use && name.empty())
        {
            if (!recent)
            {
                throw Spec_error{"a use block with no name needs a rules block above it", line};
            }

            name = *recent;
        }

        const auto used{use ? library.find(name) : library.end()};

        if (use && used == library.end())
        {
            throw Spec_error{"the used block '" + name + "' is not above this one", line};
        }

        Lexer_spec inherited;

        inherited.parse = re2c_parse;

        inherited.line = line;

        inherited.options = carried_options;

        // Configurations govern the whole block, wherever in it they stand, and of two assignments to one name the
        // last is the one that governs, so a pass records what the block's configurations leave and translates no
        // pattern under them: the pass that reads under what they leave is the one the block means. The first pass
        // reads under the inherited flags with the encoding at ASCII, the encoding that refuses the fewest
        // patterns, so that a block turning one off is read under what it left and never refused for what it
        // inherited. What the configurations leave is the inherited flags with the block's own applied, whichever
        // pass reads it, and the evidence of the flex syntax only ever turns its flag on, so the reading settles.
        // A use block's rules are the used block's, read at the head of every pass under the same flags as its own.
        auto reading{flags};

        reading.encoding = Re2c_encoding::ascii;

        // Why the encoding a block's configurations leave cannot be read, when it cannot.
        const auto encoding_refusal{[](const Re2c_flags& left) { return unreadable(left.encoding); }};

        auto left{flags};

        Lexer_spec spec;

        std::vector<Definition_site> block_sites;
        std::vector<std::string> block_named;
        std::vector<std::string> block_ruled;

        std::vector<std::pair<std::string, std::size_t>> block_ends;

        // What this block's own configurations leave, which the blocks after it inherit.
        Pointers_t settled_pointers{carried_pointers};

        auto settled_api{carried_api};

        for (;;)
        {
            spec = inherited;

            auto pass_flags{reading};

            // re2c compiles a definition's regex at every point of use, so the definitions the blocks above left
            // are translated again here, under the flags this block reads its own patterns under: `point = [^];`
            // written where the encoding was ASCII admits one byte there and a whole code point in a block that
            // turns UTF-8 on. One this block's flags cannot read waits for the block to name it.
            std::vector<Unreadable_definition> unreadable;

            // The classes among the definitions, which a class difference takes its operands from, are the pass's
            // as the definitions are.
            Classes_t classes;

            // Each pass begins from what the blocks above this one left, as each pass rereads this block alone.
            Pointers_t pointers{carried_pointers};

            auto api_custom{carried_api};

            for (const auto& [name, at_definition, line_bound] : carried_definitions)
            {
                Block body{source, at_definition, pass_flags, pass_flags, classes, pointers, macros, api_custom};

                try
                {
                    auto [expression, points]{body.definition(line_bound, spec.definitions)};

                    if (body.deferred())
                    {
                        throw *body.deferred();
                    }

                    spec.definitions.insert_or_assign(name, std::move(expression));

                    note_class(classes, name, std::move(points));
                }
                catch (const Spec_error& refusal)
                {
                    spec.definitions.erase(name);

                    classes.erase(name);

                    unreadable.push_back({.name = name, .refusal = refusal});
                }
            }

            Block block{source, begin, pass_flags, flags, classes, pointers, macros, api_custom};

            if (rules)
            {
                block.judge_later();
            }

            if (use)
            {
                block.use(used->second, spec, library, returning);
            }

            at = block.read(spec, library, returning);

            // re2c compiles a rules block where it is used and refuses rules of both kinds there, so a rules block
            // is checked as part of the scanners using it and never on its own.
            if (!rules)
            {
                block.refuse_mixed_kinds();
            }

            block_sites = block.sites();

            block_named = block.named();

            block_ruled = block.ruled();

            block_ends = block.ends();

            left = block.flags();

            settled_pointers = pointers;

            settled_api = api_custom;

            // The encoding the configurations leave is what the block reads under, whichever pass; one the
            // reading has not got is refused before any pass reads under it.
            if (const auto refusal{encoding_refusal(left)}; !refusal.empty())
            {
                throw Spec_error{refusal, block.encoding_line()};
            }

            if (left != pass_flags)
            {
                reading = left;

                continue;
            }

            // A definition the settled flags cannot read is refused where a rule of this block reaches it, directly
            // or through the definitions the rule names, which is where re2c would compile it; one no rule reaches
            // is compiled nowhere and left out, and so is an alias of it that no rule names.
            const auto named{reached(spec)};

            for (const auto& [name, refusal] : unreadable)
            {
                if (named.contains(name))
                {
                    throw refusal;
                }
            }

            // A refusal the flags decide is the settled pass's to make: a class difference empty under the ASCII
            // the first pass reads under, `[^] \ [\x00-\xff]`, holds every code point past the bytes once the block
            // turns UTF-8 on, and one empty under the case flags inherited, `"a" \ 'A'`, holds the exact `a` once
            // the block inverts which quote folds. It is a scanner's to make, at that: a rules block's and a block of
            // definitions alone are compiled where they are used, under the flags there, and are refused there.
            if (block.deferred() && !rules && !spec.rules.empty())
            {
                throw *block.deferred();
            }

            break;
        }

        if (rules)
        {
            recent = name;
        }

        if (rules || (!use && !name.empty()))
        {
            library.insert_or_assign(name, begin);
        }

        // A global block's names and configurations join the global scope; a local block's, a rules block's and a
        // use block's stay in it, so a use block that asks for an encoding leaves the blocks after it as they were.
        if (!local && !rules && !use)
        {
            flags = left;

            carried_pointers = settled_pointers;

            carried_api = settled_api;

            carried_options = spec.options;

            // A name this block declared again keeps the place it had, since a definition's own regex may name the
            // ones declared before it.
            for (const auto& site : block_sites)
            {
                const auto known{std::ranges::find(carried_definitions, site.name, &Definition_site::name)};

                if (known == carried_definitions.end())
                {
                    carried_definitions.push_back(site);
                }
                else
                {
                    *known = site;
                }
            }
        }

        // re2c's own checks of the end rule, in its words, held where re2c holds them: a block of its own once it
        // is read, its rules all tokens or none, and a rules block only where a use block takes it up, its rules and
        // end rules counted with the using block's, since re2c reads a rules block as a library and holds nothing
        // against it until then. An end rule whose name has no other rule, `<*>` counted as a name of its own,
        // "doesn't make sense"; an end rule needs `re2c:eof` set; and `re2c:eof` set needs an end rule in every
        // condition, its own or under `<*>`, and in a block naming none one end rule.
        if (!rules && (!block_ruled.empty() || !block_ends.empty()))
        {
            for (const auto& [name, line] : block_ends)
            {
                if (!std::ranges::contains(block_ruled, name))
                {
                    throw Spec_error{
                            name.empty() ?
                                    "EOF rule without other rules doesn't make sense" :
                                    "EOF rule in condition '" + name + "' without other rules doesn't make sense",
                            line};
                }
            }

            auto eof_set{false};

            for (const auto& option : spec.options)
            {
                if (option.starts_with("eof="))
                {
                    eof_set = option.substr(4) != "-1";
                }
            }

            for (const auto& [name, line] : block_ends)
            {
                if (!eof_set)
                {
                    throw Spec_error{
                            name.empty() ?
                                    "$ rule found, but 're2c:eof' configuration is not set" :
                                    "in condition '" + name + "' $ rule found, but 're2c:eof' configuration is not set",
                            line};
                }
            }

            if (eof_set)
            {
                const auto ended{[&block_ends](const std::string& name) {
                    return std::ranges::any_of(block_ends, [&name](const std::pair<std::string, std::size_t>& end) {
                        return end.first == name || end.first == "*";
                    });
                }};

                if (block_named.empty() && !ended(""))
                {
                    throw Spec_error{"'re2c:eof' configuration is set, but no $ rule found", spec.line};
                }

                for (const auto& name : block_named)
                {
                    if (!ended(name))
                    {
                        throw Spec_error{
                                "in condition '" + name + "' 're2c:eof' configuration is set, but no $ rule found",
                                spec.line};
                    }
                }
            }
        }

        if (spec.rules.empty() || rules)
        {
            continue;
        }

        // A rule's index ranks it among the rules matching one lexeme, so the rules are placed as re2c 3.1 ranks
        // them, whatever their order in the block: a condition's own rules first, then the `<*>` rules, which re2c
        // appends to each condition's own, and the default rules after them all in the same two ranks, since a
        // default rule has the lowest priority and a condition's own beats the `<*>` one; each rank keeps the order
        // it was read in, which is the file's, a used block's rules standing where their directive does.
        const auto rank{[](const Lexer_spec::Rule& rule) {
            return (rule.pattern == "*" ? 2 : 0) + (std::ranges::contains(rule.conditions, "*") ? 1 : 0);
        }};

        std::ranges::stable_sort(spec.rules, std::ranges::less{}, rank);

        // re2c declares no conditions; the ones the block's rules name are the scanner's, the end rule and the empty
        // rule naming them as any rule does, and each is exclusive, since a rule is active in a condition only by
        // naming it or by `<*>`. INITIAL is the name the default condition is reported under, so a rule naming it
        // names that one, inclusive as it is everywhere, and the scanner is recorded as having it.
        for (const auto& name : block_named)
        {
            spec.conditions.push_back({.name = name, .exclusive = name != "INITIAL"});
        }

        // A `<*>` rule is appended to each condition the block's rules name, which is what re2c compiles, so it
        // stands in those conditions and in no other: a scanner whose rules all name conditions has no INITIAL for
        // it to stand in. A block whose rules name only `<*>` gives re2c no condition to compile them into and
        // is refused, since the rules would stand nowhere.
        for (auto& rule : spec.rules)
        {
            if (std::ranges::contains(rule.conditions, "*"))
            {
                if (block_named.empty())
                {
                    throw Spec_error{
                            "the rule names `<*>` where no rule of the block names a condition, so re2c "
                            "compiles no condition for it to stand in",
                            rule.line};
                }

                rule.conditions = block_named;
            }
        }

        scanners.push_back(std::move(spec));
    }

    return scanners;
}

} // namespace munch::tools::audit
