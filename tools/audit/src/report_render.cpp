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
/**
 * @brief How many examples a list in the text report shows before it counts the rest.
 */
constexpr std::size_t shown_examples{6};

/**
 * @brief What the blame section says per token: how many candidate bytes it consumes mid-token, and one of them with
 *        the shortest input after which it does.
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
    std::string after{};
};

/**
 * @brief Returns the items written one after another with a separator between every two.
 * @tparam Items The type of the items' range.
 * @tparam One The type of what writes one item.
 * @param items The items.
 * @param separator What stands between two items.
 * @param one What an item is written as.
 * @return The text, empty for no item.
 */
template <typename Items, typename One>
[[nodiscard]] std::string joined(const Items& items, const std::string_view separator, const One& one)
{
    std::string text{};

    for (const auto& item : items)
    {
        if (!text.empty())
        {
            text += separator;
        }

        text += one(item);
    }

    return text;
}

/**
 * @brief Writes one character of a JSON string: a quote or a backslash escaped, a byte that may not stand bare as the
 *        escape of its value, and any other as written.
 * @param out The JSON text, the character added.
 * @param byte The character's first byte.
 * @param bare Whether the character may stand as written.
 * @param written The character as written.
 */
void put_json_character(std::string& out, const char byte, const bool bare, const std::string_view written)
{
    if (byte == '"' || byte == '\\')
    {
        out += std::format(R"(\{})", byte);

        return;
    }

    if (!bare)
    {
        out += std::format(R"(\u{:04x})", static_cast<unsigned char>(byte));

        return;
    }

    out += written;
}

/**
 * @brief Returns the ending `certif` takes for a count of bytes, `certifies` for one and `certify` for any other.
 * @param count The count.
 * @return `ies` for one, `y` for any other.
 */
[[nodiscard]] constexpr std::string_view certifies_ending(const std::size_t count) noexcept
{
    return count == 1 ? "ies" : "y";
}

/**
 * @brief Returns what a certificate says of a byte, by whether it certifies exactly or once the discarded tokens are
 *        deleted.
 * @param exact Whether it certifies exactly.
 * @param modulo Whether it certifies once the discarded tokens are deleted.
 * @return The words.
 */
[[nodiscard]] std::string_view certificate_state(const bool exact, const bool modulo) noexcept
{
    if (exact)
    {
        return "certifies exactly";
    }

    if (modulo)
    {
        return "certifies once discarded tokens are deleted";
    }

    return "still does not certify";
}

/**
 * @brief Returns the escape a byte is written as inside the quotes the report shows it in, when it has one: a common
 *        control, the backslash, and the quote around it.
 * @param byte The byte.
 * @param quote The quote around it, `'` for a byte shown alone and `"` for one of a string.
 * @return The escape, or std::nullopt for a byte written as itself or in hex.
 */
[[nodiscard]] std::optional<std::string_view> quoted_escape(const unsigned char byte, const char quote)
{
    switch (byte)
    {
    case '\n':
        return R"(\n)";
    case '\t':
        return R"(\t)";
    case '\r':
        return R"(\r)";
    case '\\':
        // A backslash is written as two, so that the two bytes of a backslash and an n are told apart from the one byte
        // of a newline, which is written `\n`.
        return R"(\\)";
    default:
        break;
    }

    // The quote inside the quotes is written as an escape, so the quotes the byte is shown in are their own and a
    // witness holding one does not look as though it closed early.
    if (byte == static_cast<unsigned char>(quote))
    {
        return quote == '\'' ? R"(\')" : R"(\")";
    }

    return std::nullopt;
}

/**
 * @brief Returns a byte as the report prints it: the character when it is printable, an escape when it is a common
 *        control, hex otherwise.
 * @param byte The byte.
 * @return The rendering, quoted.
 */
[[nodiscard]] std::string shown(const unsigned char byte)
{
    if (const auto escape{quoted_escape(byte, '\'')})
    {
        return std::format("'{}'", *escape);
    }

    if (is_printable(byte))
    {
        return std::format("'{}'", static_cast<char>(byte));
    }

    return std::format("0x{:02X}", byte);
}

/**
 * @brief Returns a byte string as the report prints it, each byte shown as above but without its own quotes, and in hex
 *        as the escape `\xHH`.
 * @param bytes The bytes.
 * @return The rendering, quoted once.
 */
[[nodiscard]] std::string shown(const std::string_view bytes)
{
    std::string out{'"'};

    for (const auto byte : bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (const auto escape{quoted_escape(value, '"')})
        {
            out += *escape;
        }
        else if (is_printable(value))
        {
            out += byte;
        }
        else
        {
            out += std::format(R"(\x{:02X})", value);
        }
    }

    return out + '"';
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

    return joined(bytes, " ", shown_byte);
}

/**
 * @brief Returns a span as the report prints it.
 * @param span The span.
 * @return The number, or "unbounded".
 */
[[nodiscard]] std::string shown(const std::optional<std::size_t>& span)
{
    return span ? std::to_string(*span) : "unbounded";
}

/**
 * @brief Returns a list of bytes as a JSON array of their values.
 * @param bytes The bytes.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_bytes(const std::vector<unsigned char>& bytes)
{
    const auto value{[](const unsigned char byte) { return std::format("{}", byte); }};

    return std::format("[{}]", joined(bytes, ", ", value));
}

/**
 * @brief Returns one row of the text report: its label, padded to the column the values stand in, and its value.
 * @param label The label, empty for a row continuing the one before.
 * @param value The value.
 * @return The row, its newline included.
 */
[[nodiscard]] std::string row(const std::string_view label, const std::string& value)
{
    return std::format("{:<28}{}\n", label, value);
}

/**
 * @brief Returns a window count as the report prints it: the number, or the bound the count passed when none holds it.
 * @param count The count, std::nullopt when it is more than a std::size_t can hold.
 * @return The rendering.
 */
[[nodiscard]] std::string counted(const std::optional<std::size_t>& count)
{
    return count ? std::to_string(*count) : std::format("more than {}", std::numeric_limits<std::size_t>::max());
}

/**
 * @brief Returns where the byte stands after an edit, said to the author, with the other bytes it certifies.
 * @param after The outcome.
 * @return The sentence.
 */
[[nodiscard]] std::string standing(const Outcome& after)
{
    const auto state{certificate_state(after.exact, after.modulo)};

    if (after.gained.empty())
    {
        return std::string{state};
    }

    return std::format("{}; also certified: {}", state, shown(after.gained));
}

/**
 * @brief Returns a shape's name, as the text and the JSON spell it.
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
 * @brief Returns what a shape's edit is, said to the author.
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
 * @brief Returns a byte string as a JSON string, each byte the code point of its value.
 * @param bytes The bytes.
 * @return The JSON text, quotes included.
 */
[[nodiscard]] std::string json_byte_string(const std::string_view bytes)
{
    std::string out{'"'};

    for (std::size_t at{0}; at < bytes.size(); ++at)
    {
        const auto byte{bytes[at]};

        const auto bare{is_printable(static_cast<unsigned char>(byte))};

        const auto written{bytes.substr(at, 1)};

        put_json_character(out, byte, bare, written);
    }

    return out + '"';
}

/**
 * @brief Returns a token as JSON: its id and its name.
 * @tparam Name The type of the naming.
 * @param token The token id.
 * @param name The naming.
 * @return The JSON text.
 */
template <typename Name>
[[nodiscard]] std::string json_token(const std::size_t token, const Name& name)
{
    return std::format(R"({{"id": {}, "name": {}}})", token, json_string(name(token)));
}

/**
 * @brief Returns a list as a JSON array, each item written by the function given.
 * @tparam Items The type of the items' range.
 * @tparam One The type of what writes one item.
 * @param items The items.
 * @param one What an item is written as.
 * @return The JSON text.
 */
template <typename Items, typename One>
[[nodiscard]] std::string json_list(const Items& items, const One& one)
{
    return std::format("[{}]", joined(items, ", ", one));
}

/**
 * @brief Returns where the byte stands after an edit, as JSON.
 * @param after The outcome.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_outcome(const Outcome& after)
{
    return std::format(
            R"({{"exact": {}, "modulo": {}, "gained": {}}})", after.exact, after.modulo, json_bytes(after.gained));
}

/**
 * @brief Returns the row naming the discarded tokens, so that what the modulo row deleted is on the page: their count
 *        and the first six names.
 * @tparam Name The type of the naming.
 * @param discarded The discarded tokens, by id.
 * @param name The name to print for a token id.
 * @return The row.
 */
template <typename Name>
[[nodiscard]] std::string discarded_row(const std::vector<std::size_t>& discarded, const Name& name)
{
    auto names{joined(discarded | std::views::take(shown_examples), ", ", name)};

    if (discarded.size() > shown_examples)
    {
        names += std::format(", ... {} more", discarded.size() - shown_examples);
    }

    return row("discarded tokens", std::format("{}: {}", discarded.size(), names));
}

/**
 * @brief Returns the windows' rows: the count over class representatives, widths ascending, and the first few as
 *        examples, printable windows first, since a reader recognises those.
 * @param report The report.
 * @return The rows.
 */
[[nodiscard]] std::string window_rows(const Report& report)
{
    std::map<std::size_t, std::size_t> per_width{};

    for (const auto& [window, origin] : report.windows)
    {
        ++per_width[window.size()];
    }

    const auto at_width{[]<typename Entry>(const Entry& entry) {
        const auto& [width, count]{entry};

        return std::format("{} at width {}", count, width);
    }};

    const auto summary{joined(per_width, ", ", at_width)};

    const auto label{std::format("certified windows (<= {})", report.window_limit)};

    const auto expanded{counted(report.window_count)};

    const auto value{[&]() -> std::string {
        if (report.windows.empty())
        {
            return "none";
        }

        return std::format("{} over {} byte classes, {} once classes expand", summary, report.classes.size(), expanded);
    }()};

    auto out{row(label, value)};

    // A window of printable ASCII makes the better example.
    const auto printable_window{[](const Certified_window& certified) {
        const auto& [window, origin]{certified};

        const auto printable{[](const char byte) { return is_printable(static_cast<unsigned char>(byte)); }};

        return std::ranges::all_of(window, printable);
    }};

    auto examples{report.windows};

    std::ranges::stable_partition(examples, printable_window);

    for (const auto& [window, origin] : examples | std::views::take(shown_examples))
    {
        out += row("", std::format("{} at {}", shown(std::string_view{window}), origin));
    }

    if (report.windows.size() > shown_examples)
    {
        out += row("", std::format("... {} more", report.windows.size() - shown_examples));
    }

    return out;
}

/**
 * @brief Returns the blame section, grouped by token: the candidate bytes each de-certifies and the shortest input
 *        after which it consumes one.
 * @tparam Name The type of the naming.
 * @param blame The blame, by byte then token.
 * @param name The name to print for a token id.
 * @return The section, its heading first.
 */
template <typename Name>
[[nodiscard]] std::string blame_section(const std::vector<Blame>& blame, const Name& name)
{
    std::string out{"\nwhy candidate bytes do not certify\n"};

    std::map<std::size_t, Consumed> by_token{};

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
        const auto& [bytes, example, after]{consumed};

        const auto example_shown{shown(example)};

        const auto after_shown{shown(std::string_view{after})};

        out += std::format(
                "  {:<26} consumes {} candidate byte{} mid-token, e.g. {} after {}\n", name(token), bytes,
                plural(bytes), example_shown, after_shown);
    }

    return out;
}

/**
 * @brief Returns one byte's price section: the edits in order with the certificate after each, what cannot move, and
 *        what the shapes of the tokens consuming it offer.
 * @tparam Name The type of the naming.
 * @param pricing The pricing.
 * @param name The name to print for a token id.
 * @return The section, its heading first.
 */
template <typename Name>
[[nodiscard]] std::string price_section(const Pricing& pricing, const Name& name)
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
        const auto label{std::format("no token begins with {}", shown(byte))};

        out += std::format(
                "  {:<26} neither certificate reports it; it is given a token of its own before any edit\n", label);

        out += std::format("  {:<26} {}\n", "", standing(*given));
    }

    for (const auto [index, step] : std::views::enumerate(steps))
    {
        const auto& [token, shape, separated, separated_discarded, exact, modulo]{step};

        const std::string_view run{shape == Shape::run ? "the run " : ""};

        out += std::format("  {}. {:<24} {}no longer admits {}", index + 1, name(token), run, shown(byte));

        if (separated)
        {
            const std::string_view discarded{separated_discarded ? ", discarded" : ""};

            out += std::format(", and {} becomes a token of its own{}", shown(byte), discarded);
        }

        out += std::format("\n     {:<24} {}\n", "", certificate_state(exact, modulo));
    }

    for (const auto token : immovable)
    {
        const auto offered{std::ranges::contains(choices, token, &Choice::token)};

        const std::string_view remedy{
                offered ? "its shape offers an edit below" : "the byte cannot certify while it stays"};

        out += std::format("  {:<26} spells {} out and cannot be narrowed; {}\n", name(token), shown(byte), remedy);
    }

    // The tokens the narrowing is not applied to and has no verdict about: some word of the token holds the byte fixed
    // only where a token may begin with it, so the impossibility above would be a claim not earned.
    for (const auto token : undecided)
    {
        out += std::format(
                "  {:<26} spells {} out, on some path only as its first byte, where a token may begin with it\n",
                name(token), shown(byte));

        out += std::format("  {:<26} no narrowing applies to a fixed spelling, so this edit decides nothing\n", "");
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
 * @brief Returns a span as JSON: the number, or "unbounded".
 * @param span The span.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_span(const std::optional<std::size_t>& span)
{
    return span ? std::to_string(*span) : R"("unbounded")";
}

/**
 * @brief Returns one byte's pricing as JSON: the certificates before any edit, the token given, the steps, the tokens
 *        no step answers, the bytes gained, the shapes' choices and every shape's edit together.
 * @tparam Name The type of the naming.
 * @param pricing The pricing.
 * @param name The naming.
 * @return The JSON text.
 */
template <typename Name>
[[nodiscard]] std::string json_pricing(const Pricing& pricing, const Name& name)
{
    const auto& [byte, exact_before, modulo_before, given, steps, immovable, undecided, gained, choices, together]{
            pricing};

    const auto token{[&name](const std::size_t id) { return json_token(id, name); }};

    const auto step_json{[&name](const Price_step& step) {
        const auto& [id, shape, separated, separated_discarded, exact, modulo]{step};

        return std::format(
                R"({{"token": {}, "shape": "{}", "separated": {}, "separated_discarded": {}, )"
                R"("exact": {}, "modulo": {}}})",
                json_token(id, name), shape_name(shape), separated, separated_discarded, exact, modulo);
    }};

    const auto choice_json{[&name](const Choice& choice) {
        const auto& [id, shape, after]{choice};

        return std::format(
                R"({{"token": {}, "shape": "{}", "after": {}}})", json_token(id, name), shape_name(shape),
                json_outcome(after));
    }};

    const auto steps_json{json_list(steps, step_json)};

    const auto choices_json{json_list(choices, choice_json)};

    const auto given_json{given ? json_outcome(*given) : "null"};

    const auto immovable_json{json_list(immovable, token)};

    const auto undecided_json{json_list(undecided, token)};

    const auto gained_json{json_bytes(gained)};

    const auto together_json{together ? json_outcome(*together) : "null"};

    return std::format(
            R"({{"byte": {}, "exact_before": {}, "modulo_before": {}, "given": {}, "steps": {}, )"
            R"("immovable": {}, "undecided": {}, "gained": {}, "choices": {}, "together": {}}})",
            byte, exact_before, modulo_before, given_json, steps_json, immovable_json, undecided_json, gained_json,
            choices_json, together_json);
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

    const auto core{
            report.mandatory_core.empty() ? std::string{"none"} : shown(std::string_view{report.mandatory_core})};

    out += row("mandatory core", core);

    out += row("anchor-free span, bytes", shown(report.byte_span));

    if (!report.windows.empty())
    {
        const auto span{[&report] {
            if (report.window_span)
            {
                return shown(*report.window_span);
            }

            return std::format("not decided: more than {} windows once classes expand", span_window_cap);
        }()};

        out += row("anchor-free span, windows", span);
    }

    out += row("lag", shown(report.lag));

    const auto& [witness, exhaustive]{report.rescue};

    const auto rescue{[&exhaustive, &witness]() -> std::string {
        if (!exhaustive)
        {
            return std::format("not decided: the search passed {} states", dfa::rescue_cap);
        }

        if (witness.empty())
        {
            return "yes";
        }

        return std::format("no: the scan rolls back and continues on {}", shown(witness));
    }()};

    out += row("rescue-free", rescue);

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
        const auto count{report.exact.size()};

        const auto verb{certifies_ending(count)};

        return std::format("{} byte{} certif{} exactly: a cut is safe at any occurrence", count, plural(count), verb);
    }

    if (!report.modulo.empty())
    {
        const auto count{report.modulo.size()};

        const auto verb{certifies_ending(count)};

        const std::string_view priced{report.prices.empty() ? "" : ", priced below"};

        return std::format(
                "no byte certifies exactly; {} certif{} once the discarded tokens are deleted{}", count, verb, priced);
    }

    if (!report.windows.empty())
    {
        return std::format(
                "no byte certifies; windows do: {} up to width {}, {} once classes expand", report.windows.size(),
                report.window_limit, counted(report.window_count));
    }

    // The windows are the conservative model's, so their absence up to the width is the model's finding and no proof
    // that the exact decision certifies none.
    const std::string_view costed{report.prices.empty() ? "" : "; what certifying a byte would cost is priced below"};

    return std::format("no byte certifies, and no window up to width {} in the model{}", report.window_limit, costed);
}

std::string json(const Report& report, const std::function<std::string(std::size_t)>& name)
{
    std::string out{"{\n"};

    auto first_member{true};

    const auto member{[&out, &first_member](const std::string_view key, const std::string& value) {
        const std::string_view separator{first_member ? "" : ",\n"};

        first_member = false;

        out += std::format(R"({}  "{}": {})", separator, key, value);
    }};

    const auto token{[&name](const std::size_t id) { return json_token(id, name); }};

    const auto window_json{[](const Certified_window& certified) {
        const auto& [window, origin]{certified};

        return std::format(R"({{"window": {}, "origin": {}}})", json_byte_string(window), origin);
    }};

    const auto blame_json{[&name](const Blame& blamed) {
        const auto& [byte, id, after]{blamed};

        return std::format(
                R"({{"byte": {}, "token": {}, "after": {}}})", byte, json_token(id, name), json_byte_string(after));
    }};

    const auto pricing_json{[&name](const Pricing& pricing) { return json_pricing(pricing, name); }};

    // A count past what a std::size_t holds prints as the bound it passed, a string.
    const auto count_json{[](const std::optional<std::size_t>& count) {
        return count ? std::to_string(*count) : json_byte_string(counted(count));
    }};

    member("verdict", json_byte_string(verdict(report)));

    member("nullable", report.nullable ? "true" : "false");

    member("exact", json_bytes(report.exact));

    member("modulo", json_bytes(report.modulo));

    member("discarded", json_list(report.discarded, token));

    member("byte_classes", std::to_string(report.classes.size()));

    member("window_limit", std::to_string(report.window_limit));

    member("windows", json_list(report.windows, window_json));

    member("window_count", count_json(report.window_count));

    member("mandatory_core", json_byte_string(report.mandatory_core));

    member("byte_span", json_span(report.byte_span));

    const auto window_span{[&report]() -> std::string {
        if (report.windows.empty())
        {
            return "null";
        }

        if (report.window_span)
        {
            return json_span(*report.window_span);
        }

        return R"("undecided")";
    }()};

    member("window_span", window_span);

    member("lag", json_span(report.lag));

    const auto& [witness, exhaustive]{report.rescue};

    const auto rescue_free{[&exhaustive, &witness]() -> std::string_view {
        if (!exhaustive)
        {
            return "null";
        }

        return witness.empty() ? "true" : "false";
    }()};

    member("rescue_free", std::string{rescue_free});

    const auto rescue_witness{witness.empty() ? std::string{"null"} : json_byte_string(witness)};

    member("rescue_witness", rescue_witness);

    member("blame", json_list(report.blame, blame_json));

    member("prices", json_list(report.prices, pricing_json));

    return std::format("{}\n}}", out);
}

std::string options_row(const std::vector<std::string>& options)
{
    if (options.empty())
    {
        return {};
    }

    return row("options", joined(options, ", ", std::identity{}));
}

std::string options_json(const std::vector<std::string>& options)
{
    return json_list(options, json_string);
}

std::string json_string(const std::string_view text)
{
    std::string out{'"'};

    for (std::size_t at{0}; at < text.size();)
    {
        const auto byte{text[at]};

        const auto value{static_cast<unsigned char>(byte)};

        const auto length{well_formed_length(text, at)};

        put_json_character(out, byte, value >= ' ' && length != 0, text.substr(at, length));

        at += std::max(length, 1UZ);
    }

    return out + '"';
}

} // namespace munch::tools::audit
