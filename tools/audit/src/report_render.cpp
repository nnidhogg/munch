#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "munch/dfa/boundary_search.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/report.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements report.hpp's rendering: how a byte, a byte string, a span, a count and a shape are written in the text and
// in the JSON is private to this unit.

/**
 * @brief What the blame section says per token: how many candidate bytes it consumes mid-token, and one of them
 *        with the shortest input after which it does.
 */
struct Consumed
{
    /**
     * @brief How many candidate bytes the token consumes mid-token.
     */
    std::size_t bytes{0};

    /**
     * @brief The byte shown as the example.
     */
    unsigned char example{0};

    /**
     * @brief The shortest input after which the token consumes the example.
     */
    std::string after;
};

/**
 * @brief A byte as the report prints it: the character when it is printable, an escape when it is a common
 *        control, hex otherwise.
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
    case ' ':
        return "' '";
    case '\'':
        return R"('\'')";
    default:
        break;
    }

    // A backslash is written as two, so that the two bytes of a backslash and an n are told apart from the one byte of
    // a newline, which is written `\n`.
    if (byte == '\\')
    {
        return R"('\\')";
    }

    if (byte > 0x20 && byte < 0x7F)
    {
        return std::string{'\''} + static_cast<char>(byte) + '\'';
    }

    return std::format("0x{:02X}", byte);
}

/**
 * @brief A byte string as the report prints it, each byte shown as above but without its own quotes.
 * @param bytes The bytes.
 * @return The rendering, quoted once.
 */
[[nodiscard]] std::string shown(const std::string_view bytes)
{
    std::string out{'"'};

    for (const auto byte : bytes)
    {
        auto one{shown(static_cast<unsigned char>(byte))};

        if (one.front() == '\'')
        {
            one = one.substr(1, one.size() - 2);
        }

        if (one == R"(\')")
        {
            one = "'";
        }

        // A double quote inside the string is written as an escape, so the quotes the string is shown in are its own
        // and a witness holding one does not look as though it closed early.
        if (one == "\"")
        {
            one = R"(\")";
        }

        out += one.starts_with("0x") ? R"(\x)" + one.substr(2) : one;
    }

    return out + '"';
}

/**
 * @brief A list of bytes as the report prints it.
 * @param bytes The bytes.
 * @return The rendering, or "none".
 */
[[nodiscard]] std::string shown(const std::vector<unsigned char>& bytes)
{
    if (bytes.empty())
    {
        return "none";
    }

    std::string out;

    for (const auto byte : bytes)
    {
        out += (out.empty() ? "" : " ") + shown(byte);
    }

    return out;
}

/**
 * @brief A span as the report prints it.
 * @param span The span.
 * @return The number, or "unbounded".
 */
[[nodiscard]] std::string shown(const std::optional<std::size_t>& span)
{
    return span ? std::to_string(*span) : "unbounded";
}

/**
 * @brief A list of bytes as a JSON array of their values.
 * @param bytes The bytes.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_bytes(const std::vector<unsigned char>& bytes)
{
    std::string out{'['};

    for (const auto byte : bytes)
    {
        out += std::format("{}{}", out.size() == 1 ? "" : ", ", byte);
    }

    return out + ']';
}

/**
 * @brief One row of the text report: its label, padded to the column the values stand in, and its value.
 * @param label The label, empty for a row continuing the one before.
 * @param value The value.
 * @return The row, its newline included.
 */
[[nodiscard]] std::string row(const std::string_view label, const std::string& value)
{
    return std::format("{:<28}{}\n", label, value);
}

/**
 * @brief A window count as the report prints it: the number, or the bound the count passed when none holds it.
 * @param count The count, std::nullopt when it is more than a std::size_t can hold.
 * @return The rendering.
 */
[[nodiscard]] std::string counted(const std::optional<std::size_t>& count)
{
    return count ? std::to_string(*count) : std::format("more than {}", std::numeric_limits<std::size_t>::max());
}

/**
 * @brief Whether every byte of a window is printable ASCII, which makes it the better example.
 * @param window The window.
 * @return True when it is.
 */
[[nodiscard]] bool is_printable(const std::string_view window) noexcept
{
    return std::ranges::all_of(window, [](const char byte) { return byte >= 0x20 && byte < 0x7F; });
}

/**
 * @brief Where the byte stands after an edit, said to the author, with the other bytes it certifies.
 * @param after The outcome.
 * @return The sentence.
 */
[[nodiscard]] std::string standing(const Outcome& after)
{
    return std::string{
                   after.exact  ? "certifies exactly" :
                   after.modulo ? "certifies once discarded tokens are deleted" :
                                  "still does not certify"} +
           (after.gained.empty() ? "" : "; also certified: " + shown(after.gained));
}

/**
 * @brief A shape's name, as the text and the JSON spell it.
 * @param shape The shape.
 * @return The name.
 */
[[nodiscard]] std::string_view shape_name(const Shape shape)
{
    switch (shape)
    {
    case Shape::run:
        return "run";
    case Shape::terminated:
        return "terminated";
    case Shape::delimited:
        return "delimited";
    case Shape::fixed:
        return "fixed";
    case Shape::other:
        break;
    }

    return "other";
}

/**
 * @brief What a shape's edit is, said to the author.
 * @param shape The shape.
 * @return The edit.
 */
[[nodiscard]] std::string_view shape_edit(const Shape shape)
{
    switch (shape)
    {
    case Shape::terminated:
        return "leave the terminator to the token after it";
    case Shape::delimited:
        return "scan the body in a start condition of its own, the opener staying here";
    case Shape::run:
    case Shape::fixed:
    case Shape::other:
        break;
    }

    return "";
}

/**
 * @brief A byte string as a JSON string, each byte the code point of its value.
 * @param bytes The bytes.
 * @return The JSON text, quotes included.
 */
[[nodiscard]] std::string json_byte_string(const std::string_view bytes)
{
    std::string out{'"'};

    for (const auto byte : bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (byte == '"' || byte == '\\')
        {
            out += std::string{'\\'} + byte;
        }
        else if (value < 0x20 || value >= 0x7F)
        {
            out += std::format(R"(\u{:04x})", value);
        }
        else
        {
            out.push_back(byte);
        }
    }

    return out + '"';
}

/**
 * @brief A token as JSON: its id and its name.
 * @param token The token id.
 * @param name The naming.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_token(const std::size_t token, const std::function<std::string(std::size_t)>& name)
{
    return std::format(R"({{"id": {}, "name": {}}})", token, json_string(name(token)));
}

/**
 * @brief A list as a JSON array, each item written by the function given.
 * @tparam Items The type of the items' range.
 * @tparam One The type of what writes one item.
 * @param items The items.
 * @param one What an item is written as.
 * @return The JSON text.
 */
template <typename Items, typename One>
[[nodiscard]] std::string json_list(const Items& items, const One& one)
{
    std::string text{'['};

    for (const auto& item : items)
    {
        text += (text.size() == 1 ? "" : ", ") + one(item);
    }

    return text + ']';
}

/**
 * @brief Where the byte stands after an edit, as JSON.
 * @param after The outcome.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_outcome(const Outcome& after)
{
    return std::format(
            R"({{"exact": {}, "modulo": {}, "gained": {}}})", after.exact, after.modulo, json_bytes(after.gained));
}

/**
 * @brief The row naming the discarded tokens, so that what the modulo row deleted is on the page: their count and the
 *        first six names.
 * @param discarded The discarded tokens, by id.
 * @param name The name to print for a token id.
 * @return The row.
 */
[[nodiscard]] std::string discarded_row(
        const std::vector<std::size_t>& discarded, const std::function<std::string(std::size_t)>& name)
{
    std::string names;

    for (const auto id : discarded | std::views::take(6))
    {
        names += (names.empty() ? "" : ", ") + name(id);
    }

    if (discarded.size() > 6)
    {
        names += std::format(", ... {} more", discarded.size() - 6);
    }

    return row("discarded tokens", std::format("{}: {}", discarded.size(), names));
}

/**
 * @brief The windows' rows: the count over class representatives, widths ascending, and the first few as examples,
 *        printable windows first, since a reader recognises those.
 * @param report The report.
 * @return The rows.
 */
[[nodiscard]] std::string window_rows(const Report& report)
{
    std::map<std::size_t, std::size_t> per_width;

    for (const auto& [window, origin] : report.windows)
    {
        ++per_width[window.size()];
    }

    std::string summary;

    for (const auto& [width, count] : per_width)
    {
        summary += std::format("{}{} at width {}", summary.empty() ? "" : ", ", count, width);
    }

    auto out{
            row(std::format("certified windows (<= {})", report.window_limit),
                report.windows.empty() ? "none" :
                                         std::format(
                                                 "{} over {} byte classes, {} once classes expand", summary,
                                                 report.classes.size(), counted(report.window_count)))};

    auto examples{report.windows};

    std::ranges::stable_partition(
            examples, [](const Certified_window& certified) { return is_printable(certified.window); });

    for (const auto& [window, origin] : examples | std::views::take(6))
    {
        out += row("", std::format("{} at {}", shown(std::string_view{window}), origin));
    }

    if (report.windows.size() > 6)
    {
        out += row("", std::format("... {} more", report.windows.size() - 6));
    }

    return out;
}

/**
 * @brief The blame section, grouped by token: the candidate bytes each de-certifies and the shortest input after which
 *        it consumes one.
 * @param blame The blame, by byte then token.
 * @param name The name to print for a token id.
 * @return The section, its heading first.
 */
[[nodiscard]] std::string blame_section(
        const std::vector<Blame>& blame, const std::function<std::string(std::size_t)>& name)
{
    std::string out{"\nwhy candidate bytes do not certify\n"};

    std::map<std::size_t, Consumed> by_token;

    for (const auto& [byte, token, after] : blame)
    {
        auto& [bytes, example, shortest]{by_token[token]};

        ++bytes;

        if (bytes == 1 || after.size() < shortest.size())
        {
            example = byte;

            shortest = after;
        }
    }

    for (const auto& [token, consumed] : by_token)
    {
        out += std::format(
                "  {:<26} consumes {} candidate byte{} mid-token, e.g. {} after {}\n", name(token), consumed.bytes,
                consumed.bytes == 1 ? "" : "s", shown(consumed.example), shown(std::string_view{consumed.after}));
    }

    return out;
}

/**
 * @brief One byte's price section: the edits in order with the certificate after each, what cannot move, and what the
 *        shapes of the tokens consuming it offer.
 * @param pricing The pricing.
 * @param name The name to print for a token id.
 * @return The section, its heading first.
 */
[[nodiscard]] std::string price_section(const Pricing& pricing, const std::function<std::string(std::size_t)>& name)
{
    const auto& [byte, exact_before, modulo_before, given, steps, immovable, undecided, gained, choices, together]{
            pricing};

    auto out{std::format("\nwhat it would cost to certify {}\n", shown(byte))};

    if (modulo_before)
    {
        out += "  certifies already once discarded tokens are deleted; the steps below make it exact\n";
    }

    // A byte no token begins with has nothing to certify until one does; the token given is the first edit.
    if (given)
    {
        out += std::format(
                "  {:<26} neither certificate reports it; it is given a token of its own before any edit\n",
                std::format("no token begins with {}", shown(byte)));

        out += std::format("  {:<26} {}\n", "", standing(*given));
    }

    for (std::size_t index{0}; index < steps.size(); ++index)
    {
        const auto& [token, shape, separated, separated_discarded, exact, modulo]{steps[index]};

        out += std::format(
                "  {}. {:<24} {}no longer admits {}", index + 1, name(token), shape == Shape::run ? "the run " : "",
                shown(byte));

        if (separated)
        {
            out += std::format(
                    ", and {} becomes a token of its own{}", shown(byte), separated_discarded ? ", discarded" : "");
        }

        out += std::format(
                "\n     {:<24} {}\n", "",
                exact  ? "certifies exactly" :
                modulo ? "certifies once discarded tokens are deleted" :
                         "still does not certify");
    }

    for (const auto token : immovable)
    {
        const auto offered{std::ranges::contains(choices, token, &Choice::token)};

        out += std::format(
                "  {:<26} spells {} out and cannot be narrowed; {}\n", name(token), shown(byte),
                offered ? "its shape offers an edit below" : "the byte cannot certify while it stays");
    }

    // The tokens the narrowing is not applied to and has no verdict about: some word of the token holds the byte fixed
    // only where a token may begin with it, so the impossibility above would be a claim not earned.
    for (const auto token : undecided)
    {
        out += std::format(
                "  {:<26} spells {} out, on some path only as its first byte, where a token may begin with it\n",
                name(token), shown(byte));

        out += std::format(
                "  {:<26} {}\n", "", "no narrowing applies to a fixed spelling, so this edit decides nothing");
    }

    if (!gained.empty())
    {
        out += std::format("  {:<26} {}\n", "also certified after", shown(gained));
    }

    if (!choices.empty())
    {
        out += std::format("\nwhat the shapes of the tokens consuming {} offer, each edit on its own\n", shown(byte));
    }

    for (const auto& [token, shape, after] : choices)
    {
        out += std::format("  {:<26} {}: {}\n", name(token), shape_name(shape), shape_edit(shape));

        out += std::format("  {:<26} {}\n", "", standing(after));
    }

    if (together)
    {
        out += std::format("  {:<26} {}\n", "every shape's edit together", standing(*together));
    }

    return out;
}

/**
 * @brief A window count as JSON: the number, or the bound it passed as a string.
 * @param count The count, std::nullopt when it is more than a std::size_t can hold.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_count(const std::optional<std::size_t>& count)
{
    return count ? std::to_string(*count) : json_byte_string(counted(count));
}

/**
 * @brief A span as JSON: the number, or "unbounded".
 * @param span The span.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_span(const std::optional<std::size_t>& span)
{
    return span ? std::to_string(*span) : R"("unbounded")";
}

/**
 * @brief One byte's pricing as JSON: the certificates before any edit, the token given, the steps, the tokens no step
 *        answers, the bytes gained, the shapes' choices and every shape's edit together.
 * @param pricing The pricing.
 * @param name The naming.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_pricing(const Pricing& pricing, const std::function<std::string(std::size_t)>& name)
{
    const auto token{[&name](const std::size_t token) { return json_token(token, name); }};

    const auto steps{json_list(pricing.steps, [&name](const Price_step& step) {
        const auto& [token, shape, separated, separated_discarded, exact, modulo]{step};

        return std::format(
                R"({{"token": {}, "shape": "{}", "separated": {}, "separated_discarded": {}, )"
                R"("exact": {}, "modulo": {}}})",
                json_token(token, name), shape_name(shape), separated, separated_discarded, exact, modulo);
    })};

    const auto choices{json_list(pricing.choices, [&name](const Choice& choice) {
        const auto& [token, shape, after]{choice};

        return std::format(
                R"({{"token": {}, "shape": "{}", "after": {}}})", json_token(token, name), shape_name(shape),
                json_outcome(after));
    })};

    return std::format(
            R"({{"byte": {}, "exact_before": {}, "modulo_before": {}, "given": {}, "steps": {}, )"
            R"("immovable": {}, "undecided": {}, "gained": {}, "choices": {}, "together": {}}})",
            pricing.byte, pricing.exact_before, pricing.modulo_before,
            pricing.given ? json_outcome(*pricing.given) : "null", steps, json_list(pricing.immovable, token),
            json_list(pricing.undecided, token), json_bytes(pricing.gained), choices,
            pricing.together ? json_outcome(*pricing.together) : "null");
}

} // namespace

std::string render(const Report& report, const std::function<std::string(std::size_t)>& name)
{
    auto out{row("verdict", verdict(report))};

    if (report.nullable)
    {
        out += row("", "a token matches the empty string: decided through the positive-width equivalent");
    }

    out += row("certified bytes", shown(report.exact));

    out += row("certified modulo discarded", shown(report.modulo));

    if (!report.discarded.empty())
    {
        out += discarded_row(report.discarded, name);
    }

    out += window_rows(report);

    out += row(
            "mandatory core", report.mandatory_core.empty() ? "none" : shown(std::string_view{report.mandatory_core}));

    out += row("anchor-free span, bytes", shown(report.byte_span));

    if (!report.windows.empty())
    {
        out +=
                row("anchor-free span, windows",
                    report.window_span ?
                            shown(*report.window_span) :
                            std::format("not decided: more than {} windows once classes expand", span_window_cap));
    }

    out += row("lag", shown(report.lag));

    out +=
            row("rescue-free",
                !report.rescue.exhaustive ?
                        std::format("not decided: the search passed {} states", dfa::rescue_cap) :
                report.rescue.witness.empty() ?
                        "yes" :
                        std::format("no: the scan rolls back and continues on {}", shown(report.rescue.witness)));

    if (!report.blame.empty())
    {
        out += blame_section(report.blame, name);
    }

    for (const auto& pricing : report.prices)
    {
        out += price_section(pricing, name);
    }

    return out;
}

std::string verdict(const Report& report)
{
    if (!report.exact.empty())
    {
        return std::format(
                "{} byte{} certif{} exactly: a cut is safe at any occurrence", report.exact.size(),
                report.exact.size() == 1 ? "" : "s", report.exact.size() == 1 ? "ies" : "y");
    }

    if (!report.modulo.empty())
    {
        return std::format(
                "no byte certifies exactly; {} certif{} once the discarded tokens are deleted{}", report.modulo.size(),
                report.modulo.size() == 1 ? "ies" : "y", report.prices.empty() ? "" : ", priced below");
    }

    if (!report.windows.empty())
    {
        return std::format(
                "no byte certifies; windows do: {} up to width {}, {} once classes expand", report.windows.size(),
                report.window_limit, counted(report.window_count));
    }

    // The windows are the conservative model's, so their absence up to the width is the model's finding and no proof
    // that the exact decision certifies none.
    return std::format(
            "no byte certifies, and no window up to width {} in the model{}", report.window_limit,
            report.prices.empty() ? "" : "; what certifying a byte would cost is priced below");
}

std::string json(const Report& report, const std::function<std::string(std::size_t)>& name)
{
    std::string out{"{\n"};

    const auto member{[&out](const std::string_view key, const std::string& value) {
        out += std::format(R"({}  "{}": {})", out.size() == 2 ? "" : ",\n", key, value);
    }};

    const auto token{[&name](const std::size_t token) { return json_token(token, name); }};

    member("verdict", json_byte_string(verdict(report)));

    member("nullable", report.nullable ? "true" : "false");

    member("exact", json_bytes(report.exact));

    member("modulo", json_bytes(report.modulo));

    member("discarded", json_list(report.discarded, token));

    member("byte_classes", std::to_string(report.classes.size()));

    member("window_limit", std::to_string(report.window_limit));

    member("windows", json_list(report.windows, [](const Certified_window& certified) {
               const auto& [window, origin]{certified};

               return std::format(R"({{"window": {}, "origin": {}}})", json_byte_string(window), origin);
           }));

    member("window_count", json_count(report.window_count));

    member("mandatory_core", json_byte_string(report.mandatory_core));

    member("byte_span", json_span(report.byte_span));

    member("window_span", report.windows.empty() ? "null" :
                          report.window_span     ? json_span(*report.window_span) :
                                                   R"("undecided")");

    member("lag", json_span(report.lag));

    member("rescue_free", !report.rescue.exhaustive ? "null" : report.rescue.witness.empty() ? "true" : "false");

    member("rescue_witness", report.rescue.witness.empty() ? "null" : json_byte_string(report.rescue.witness));

    member("blame", json_list(report.blame, [&name](const Blame& blamed) {
               const auto& [byte, token, after]{blamed};

               return std::format(
                       R"({{"byte": {}, "token": {}, "after": {}}})", byte, json_token(token, name),
                       json_byte_string(after));
           }));

    member("prices", json_list(report.prices, [&name](const Pricing& pricing) { return json_pricing(pricing, name); }));

    return out + "\n}";
}

std::string options_row(const std::vector<std::string>& options)
{
    if (options.empty())
    {
        return {};
    }

    std::string named;

    for (const auto& option : options)
    {
        named += (named.empty() ? "" : ", ") + option;
    }

    return std::format("{:<28}{}\n", "options", named);
}

std::string options_json(const std::vector<std::string>& options)
{
    std::string out{'['};

    for (const auto& option : options)
    {
        out += (out.size() == 1 ? "" : ", ") + json_string(option);
    }

    return out + ']';
}

std::string json_string(const std::string_view text)
{
    std::string out{'"'};

    for (std::size_t at{0}; at < text.size();)
    {
        const auto byte{text[at]};

        const auto value{static_cast<unsigned char>(byte)};

        const auto length{well_formed_length(text, at)};

        if (byte == '"' || byte == '\\')
        {
            out += std::string{'\\'} + byte;
        }
        else if (value < 0x20 || length == 0)
        {
            out += std::format(R"(\u{:04x})", value);
        }
        else
        {
            out += text.substr(at, length);
        }

        at += std::max(length, 1UZ);
    }

    return out + '"';
}

} // namespace munch::tools::audit
