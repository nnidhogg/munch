#include "munch/tools/audit/re2c_classes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements re2c_classes.hpp: what a parsed regex is as a class, membership, and the edge walk both operations share
// are private to this unit.

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
    // Membership in either operand changes only where one of its ranges begins or just past where one ends, so the code
    // points between two such edges in a row are all kept or all dropped.
    std::vector<char32_t> edges;

    for (const auto& [first, last] : left)
    {
        edges.push_back(first);

        edges.push_back(last + 1);
    }

    for (const auto& [first, last] : right)
    {
        edges.push_back(first);

        edges.push_back(last + 1);
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

} // namespace

std::optional<Class> code_points(
        const std::string_view bracket, const regex::Definitions_t& definitions, const char32_t space)
{
    const auto negated{bracket.starts_with("[^")};

    // The negation is taken over the code points here, so the parser is asked for the members alone; a member that is
    // itself a caret, `[^^]`, keeps its place with an escape rather than reading as a second negation.
    const std::string members{
            negated && bracket.substr(2).starts_with('^') ? R"(\)" + std::string{bracket.substr(2)} :
                                                            std::string{bracket.substr(negated ? 2 : 1)}};

    const std::string positive{'[' + members};

    std::vector member(space, false);

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

Class united(const Class& left, const Class& right)
{
    return combined(left, right, [](const bool in_left, const bool in_right) { return in_left || in_right; });
}

Class subtracted(const Class& left, const Class& right)
{
    return combined(left, right, [](const bool in_left, const bool in_right) { return in_left && !in_right; });
}

std::string rendered(const Class& points, const Re2c_encoding encoding)
{
    if (encoding == Re2c_encoding::utf8 && !is_ascii(points))
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

bool is_ascii(const Class& points)
{
    return std::ranges::all_of(points, [](const regex::utf8::Code_point_range& range) { return range.last < 0x80; });
}

std::string step(const std::vector<regex::utf8::Code_point_range>& ranges)
{
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

} // namespace munch::tools::audit
