#include "munch/tools/audit/re2c_classes.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
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
/**
 * @brief Returns whether a class holds a code point.
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
 * @brief Returns the bytes a regex names when it is a class: a bracket, a one-byte literal, or an alternation of
 *        classes, which is what re2c lets the operands of its class difference be.
 * @param regex The regex.
 * @return The bytes, or std::nullopt when the regex is no class.
 */
[[nodiscard]] std::optional<regex::Set> class_of(const regex::Regex& regex)
{
    const auto bytes_of{[]<typename Node>(const Node& node) -> std::optional<regex::Set> {
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
            regex::Set all{};

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
    }};

    return std::visit(bytes_of, regex.node);
}

/**
 * @brief Returns the code points of two classes a predicate of their membership keeps, as a class: the union where it
 *        keeps a point in either, the difference where it keeps one in the left and not the right.
 * @tparam Keep The predicate's type.
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
    std::vector<char32_t> edges{};

    const auto add_edges{[&edges](const Class& points) {
        for (const auto& [first, last] : points)
        {
            edges.push_back(first);

            edges.push_back(last + 1);
        }
    }};

    add_edges(left);

    add_edges(right);

    // The edges ascending, each once.
    std::ranges::sort(edges);

    const auto [duplicates, end]{std::ranges::unique(edges)};

    edges.erase(duplicates, end);

    Class kept{};

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
    static constexpr std::string_view negated_opener{"[^"};

    const auto negated{bracket.starts_with(negated_opener)};

    static constexpr std::string_view opener{"["};

    const auto opener_size{negated ? negated_opener.size() : opener.size()};

    std::string members{};

    // The negation is taken over the code points here, so the parser is asked for the members alone; a member that is
    // itself a caret, `[^^]`, keeps its place with an escape rather than reading as a second negation.
    const auto past_negation{bracket.substr(negated_opener.size())};

    if (negated && past_negation.starts_with('^'))
    {
        members = std::format(R"(\{})", past_negation);
    }
    else
    {
        members = std::string{bracket.substr(opener_size)};
    }

    const auto positive{std::format("[{}", members)};

    std::vector admitted(space, false);

    const auto parsed_bytes{[&positive, &definitions]() -> std::optional<regex::Set> {
        try
        {
            const auto parsed{regex::parse(positive, definitions, re2c_parse)};

            return class_of(parsed);
        }
        catch (const regex::Syntax_error&)
        {
            return std::nullopt;
        }
    }};

    if (positive != "[]")
    {
        const auto bytes{parsed_bytes()};

        if (!bytes)
        {
            return std::nullopt;
        }

        for (const auto byte : bytes->symbols())
        {
            admitted[static_cast<unsigned char>(byte)] = true;
        }
    }

    Class ranges{};

    for (char32_t point{0}; point < space; ++point)
    {
        if (admitted[point] == negated)
        {
            continue;
        }

        const auto first{point};

        while (point + 1 < space && admitted[point + 1] != negated)
        {
            ++point;
        }

        ranges.push_back({.first = first, .last = point});
    }

    return ranges;
}

Class united(const Class& left, const Class& right)
{
    return combined(left, right, std::logical_or{});
}

Class subtracted(const Class& left, const Class& right)
{
    const auto left_only{[](const bool in_left, const bool in_right) { return in_left && !in_right; }};

    return combined(left, right, left_only);
}

std::string rendered(const Class& points, const Re2c_encoding encoding)
{
    if (encoding == Re2c_encoding::utf8 && !is_ascii(points))
    {
        return step(points);
    }

    regex::Set bytes{};

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
    const auto ascii_range{[](const regex::utf8::Code_point_range& range) { return range.last <= last_ascii; }};

    return std::ranges::all_of(points, ascii_range);
}

std::string step(const std::vector<regex::utf8::Code_point_range>& ranges)
{
    std::string members{};

    auto surrogates{false};

    for (const auto& [first, last] : ranges)
    {
        surrogates = surrogates || (first <= last_surrogate && last >= first_surrogate);

        if (first < first_surrogate)
        {
            const auto below{std::min<char32_t>(last, first_surrogate - 1)};

            members += code_point_member(first, below);
        }

        if (last > last_surrogate)
        {
            const auto above{std::max<char32_t>(first, last_surrogate + 1)};

            members += code_point_member(above, last);
        }
    }

    if (!surrogates)
    {
        return std::format("[{}]", members);
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
