#include "munch/tools/audit/repair.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/regex/regex.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The id and the priority a token of a byte's own is given: the smallest id no rule carries, since one past the
 *        highest is no id at all where a rule carries the largest std::size_t, and a priority past every rule's, so
 *        nothing else moves.
 */
struct Own_token
{
    /**
     * @brief The id.
     */
    std::size_t id{};

    /**
     * @brief The priority.
     */
    std::size_t priority{};
};

/**
 * @brief Returns the id and the priority a token of a byte's own takes in a set.
 * @param set The set.
 * @return The id and the priority.
 */
[[nodiscard]] Own_token own_token(const Token_set& set)
{
    std::vector<std::size_t> ids{};

    std::ranges::transform(set.rules, std::back_inserter(ids), &Token_rule::id);

    std::ranges::sort(ids);

    auto next_id{0UZ};

    for (const auto id : ids)
    {
        if (id == next_id)
        {
            ++next_id;
        }
    }

    auto next_priority{0UZ};

    for (const auto& [rule_regex, id, priority, discarded] : set.rules)
    {
        next_priority = std::max(next_priority, priority + 1);
    }

    return {.id = next_id, .priority = next_priority};
}

/**
 * @brief Returns a byte's own token, as a rule of the set.
 * @param byte The byte.
 * @param own The id and the priority it takes.
 * @param discarded Whether it is discarded.
 * @return The rule.
 */
[[nodiscard]] Token_rule byte_rule(const unsigned char byte, const Own_token own, const bool discarded)
{
    const std::string spelled{static_cast<char>(byte)};

    return {.regex = regex::text(spelled), .id = own.id, .priority = own.priority, .discarded = discarded};
}

/**
 * @brief Returns whether a byte certifies over a compiled set, exactly or once the discarded tokens are deleted.
 * @param lexer The compiled set.
 * @param byte The byte.
 * @param modulo Whether the certificate asked for is the one modulo discarded tokens.
 * @return True when it certifies.
 */
[[nodiscard]] bool certifies(const core::Lexer& lexer, const unsigned char byte, const bool modulo)
{
    const auto value{static_cast<char>(byte)};

    return modulo ? lexer.is_split_point_ignoring(value) : lexer.is_split_point(value);
}

/**
 * @brief Returns what giving one byte a token of its own does to a set: the byte and the other bytes certified with it,
 *        when the byte certifies after the addition, in the sense the token's kind asks for.
 *
 * The token is the one the pricing gives a byte no token begins with: the byte alone, at a priority past every rule's,
 * so that no rule's match changes, under the smallest id no rule carries. A visible token is asked whether the byte
 * certifies exactly, a discarded one whether it certifies once the discarded tokens are deleted, each as the library
 * decides it over the set compiled with the token added.
 * @param set The set as given, which is not changed.
 * @param byte The byte.
 * @param discarded Whether the token is discarded.
 * @param before The bytes that certified in that sense before the addition, ascending.
 * @return The repair, or std::nullopt when the byte does not certify after the addition.
 */
[[nodiscard]] std::optional<Repair> repair(
        const Token_set& set, const unsigned char byte, const bool discarded, const std::vector<unsigned char>& before)
{
    auto edited{set};

    edited.rules.push_back(byte_rule(byte, own_token(set), discarded));

    const auto lexer{compile(edited)};

    if (!certifies(lexer, byte, discarded))
    {
        return std::nullopt;
    }

    const auto gained_now{[&](const std::size_t value) {
        const auto other{static_cast<unsigned char>(value)};

        return other != byte && !std::ranges::binary_search(before, other) && certifies(lexer, other, discarded);
    }};

    std::vector<unsigned char> gained{};

    for (const auto value : std::views::iota(0UZ, byte_values) | std::views::filter(gained_now))
    {
        gained.push_back(static_cast<unsigned char>(value));
    }

    return Repair{.byte = byte, .gained = std::move(gained)};
}

/**
 * @brief Returns a byte as the report prints it: the character when it is printable, an escape when it is a common
 *        control, the backslash or the quote, hex otherwise.
 * @param byte The byte.
 * @return The rendering, quoted.
 */
[[nodiscard]] std::string shown(const unsigned char byte)
{
    switch (byte)
    {
    case '\n':
        return R"('\n')";
    case '\t':
        return R"('\t')";
    case '\r':
        return R"('\r')";
    case '\\':
        return R"('\\')";
    case '\'':
        return R"('\'')";
    default:
        break;
    }

    if (is_printable(byte))
    {
        return std::format("'{}'", static_cast<char>(byte));
    }

    return std::format("0x{:02X}", byte);
}

/**
 * @brief Returns a list of bytes as the report prints it.
 * @param bytes The bytes.
 * @return The rendering, or "none".
 */
[[nodiscard]] std::string shown(const std::vector<unsigned char>& bytes)
{
    if (bytes.empty())
    {
        return "none";
    }

    const auto shown_byte{[](const unsigned char byte) { return shown(byte); }};

    std::string text{};

    std::ranges::copy(bytes | std::views::transform(shown_byte) | std::views::join_with(' '), std::back_inserter(text));

    return text;
}

} // namespace

Repairs repairs(const Token_set& set, const Report& report)
{
    Repairs found{};

    for (const auto value : std::views::iota(0UZ, byte_values))
    {
        const auto byte{static_cast<unsigned char>(value)};

        if (auto visible{repair(set, byte, false, report.exact)})
        {
            found.visible.push_back(std::move(*visible));
        }

        if (std::ranges::binary_search(report.modulo, byte))
        {
            continue;
        }

        if (auto discarded{repair(set, byte, true, report.modulo)})
        {
            found.discarded.push_back(std::move(*discarded));
        }
    }

    return found;
}

std::string repair_json(const Repairs& repairs)
{
    const auto& [visible, discarded]{repairs};

    const auto value{[](const unsigned char byte) { return std::format("{}", byte); }};

    const auto repair_entry{[&value](const Repair& repaired) {
        const auto& [byte, gained]{repaired};

        std::string values{};

        std::ranges::copy(
                gained | std::views::transform(value) | std::views::join_with(std::string_view{", "}),
                std::back_inserter(values));

        return std::format(R"({{"byte": {}, "gained": [{}]}})", byte, values);
    }};

    const auto entries{[&repair_entry](const std::vector<Repair>& repaired) {
        std::string listed{};

        std::ranges::copy(
                repaired | std::views::transform(repair_entry) | std::views::join_with(std::string_view{", "}),
                std::back_inserter(listed));

        return std::format("[{}]", listed);
    }};

    return std::format(R"({{"visible": {}, "discarded": {}}})", entries(visible), entries(discarded));
}

std::string repair_section(const Repairs& repairs)
{
    const auto& [visible, discarded]{repairs};

    std::string out{"\nwhat a token of one byte's own would certify, added at the lowest priority\n"};

    const auto suggestion{[&out](const Repair& repaired, const std::string_view kind, const std::string_view state) {
        const auto& [byte, gained]{repaired};

        const auto label{std::format("{} {}", kind, shown(byte))};

        const auto also{gained.empty() ? std::string{} : std::format("; also certified: {}", shown(gained))};

        out += std::format("  {:<26} {}{}\n", label, state, also);
    }};

    for (const auto& repaired : visible)
    {
        suggestion(repaired, "visible", "certifies exactly");
    }

    for (const auto& repaired : discarded)
    {
        suggestion(repaired, "discarded", "certifies once discarded tokens are deleted");
    }

    if (visible.empty() && discarded.empty())
    {
        out += std::format("  {:<26} {}\n", "no byte", "certifies once given a token of its own, visible or discarded");
    }

    return out;
}

} // namespace munch::tools::audit
