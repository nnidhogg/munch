#include "munch/tools/audit/report.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/dfa/simulator.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The most certified windows the span is decided over once each class is expanded to its bytes; beyond it
 *        the report says so, since the walk's node count can grow with every window when every token is bounded.
 */
constexpr std::size_t span_window_cap{1U << 17U};

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
 * @brief The byte a class is shown by: its first printable member when it has one, since a reader recognises that
 *        one, else its lowest; every member decides alike, so the choice is a matter of display alone.
 * @param members The class, ascending.
 * @return The representative.
 */
[[nodiscard]] unsigned char representative(const std::vector<unsigned char>& members) noexcept
{
    const auto printable{
            std::ranges::find_if(members, [](const unsigned char byte) { return byte > 0x20 && byte < 0x7F; })};

    return printable == members.end() ? members.front() : *printable;
}

/**
 * @brief The byte classes of the tables: two bytes are one class when every state moves on both to the same state,
 *        so any decision over transitions gives one answer for the whole class.
 * @param simulator The tables.
 * @return Every class's members, ascending, the classes in order of their lowest byte.
 */
[[nodiscard]] std::vector<std::vector<unsigned char>> byte_classes(const dfa::Simulator& simulator)
{
    std::map<std::vector<std::optional<std::size_t>>, std::vector<unsigned char>> by_signature;

    for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
    {
        std::vector<std::optional<std::size_t>> signature;

        signature.reserve(simulator.state_count());

        for (std::size_t state{0}; state < simulator.state_count(); ++state)
        {
            signature.push_back(simulator.step(state, static_cast<unsigned char>(value)));
        }

        by_signature[std::move(signature)].push_back(static_cast<unsigned char>(value));
    }

    std::vector<std::vector<unsigned char>> classes;

    for (auto& members : by_signature | std::views::values)
    {
        classes.push_back(std::move(members));
    }

    // Ordered by lowest byte, so the enumeration and the report are deterministic and read in byte order.
    std::ranges::sort(classes, {}, [](const std::vector<unsigned char>& members) { return members.front(); });

    return classes;
}

/**
 * @brief Every certified window over class representatives up to a width, widths ascending.
 * @param lexer The token set.
 * @param classes The byte classes.
 * @param limit The longest width tried.
 * @return The windows.
 */
[[nodiscard]] std::vector<Certified_window> certified_windows(
        const core::Lexer& lexer, const std::vector<std::vector<unsigned char>>& classes, const std::size_t limit)
{
    std::vector<Certified_window> windows;

    std::vector<std::string> frontier{""};

    for (std::size_t width{1}; width <= limit; ++width)
    {
        std::vector<std::string> next;

        for (const auto& prefix : frontier)
        {
            for (const auto& members : classes)
            {
                auto window{prefix};

                window.push_back(static_cast<char>(representative(members)));

                if (width >= 2)
                {
                    if (const auto origin{lexer.is_split_window(window)})
                    {
                        windows.push_back({.window = window, .origin = *origin});
                    }
                }

                next.push_back(std::move(window));
            }
        }

        frontier = std::move(next);
    }

    return windows;
}

/**
 * @brief How many byte strings the certified windows stand for once every representative expands to its class.
 *
 * A window stands for the product of its bytes' class sizes, and the windows an input can show are the sum over
 * the windows. Both are counted checked, since the count outgrows what holds it: eight bytes of one class of 256,
 * the widest window the command accepts, already stand for 2^64 strings, and a wrapped sum would be printed as a
 * fact. A byte class always has a member, so the products below divide by no zero.
 * @param windows The certified windows over class representatives.
 * @param classes The byte classes.
 * @return The count, or std::nullopt when it is more than a std::size_t can hold.
 */
[[nodiscard]] std::optional<std::size_t> expansion_count(
        const std::vector<Certified_window>& windows, const std::vector<std::vector<unsigned char>>& classes)
{
    constexpr auto most{std::numeric_limits<std::size_t>::max()};

    std::map<unsigned char, std::size_t> class_size;

    for (const auto& members : classes)
    {
        class_size[representative(members)] = members.size();
    }

    std::size_t total{0};

    for (const auto& [window, origin] : windows)
    {
        std::size_t count{1};

        for (const auto byte : window)
        {
            const auto size{class_size.at(static_cast<unsigned char>(byte))};

            if (count > most / size)
            {
                return std::nullopt;
            }

            count *= size;
        }

        if (total > most - count)
        {
            return std::nullopt;
        }

        total += count;
    }

    return total;
}

/**
 * @brief The certified windows with every class expanded to its member bytes, the inventory the span is decided
 *        over, or std::nullopt when there would be more than the cap.
 * @param windows The windows over representatives.
 * @param classes The byte classes.
 * @return The expanded inventory.
 */
[[nodiscard]] std::optional<std::vector<std::pair<std::string, std::size_t>>> expanded(
        const std::vector<Certified_window>& windows, const std::vector<std::vector<unsigned char>>& classes)
{
    std::map<unsigned char, std::size_t> class_of;

    for (std::size_t index{0}; index < classes.size(); ++index)
    {
        class_of[representative(classes[index])] = index;
    }

    std::vector<std::pair<std::string, std::size_t>> inventory;

    for (const auto& [window, origin] : windows)
    {
        std::vector<std::string> partial{""};

        for (const auto byte : window)
        {
            std::vector<std::string> longer;

            for (const auto& head : partial)
            {
                for (const auto member : classes[class_of.at(static_cast<unsigned char>(byte))])
                {
                    longer.push_back(head + static_cast<char>(member));

                    if (inventory.size() + longer.size() > span_window_cap)
                    {
                        return std::nullopt;
                    }
                }
            }

            partial = std::move(longer);
        }

        for (auto& expansion : partial)
        {
            inventory.emplace_back(std::move(expansion), origin);
        }
    }

    return inventory;
}

/**
 * @brief A shortest input reaching every reachable state, found breadth first from the initial state.
 * @param simulator The tables.
 * @return The input per state, absent for a state no input reaches.
 */
[[nodiscard]] std::vector<std::optional<std::string>> shortest_inputs(const dfa::Simulator& simulator)
{
    std::vector<std::optional<std::string>> input(simulator.state_count());

    input[simulator.init_state()] = std::string{};

    std::deque<std::size_t> pending{simulator.init_state()};

    while (!pending.empty())
    {
        const auto from{pending.front()};

        pending.pop_front();

        for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
        {
            const auto to{simulator.step(from, static_cast<unsigned char>(value))};

            if (to && !input[*to])
            {
                input[*to] = *input[from] + static_cast<char>(value);

                pending.push_back(*to);
            }
        }
    }

    return input;
}

/**
 * @brief A shortest nonempty input after which the scan stands in the initial state again.
 *
 * A shortest such input is a shortest input reaching a state that steps into the initial state, one byte longer,
 * so the inputs already found decide it without a walk of its own. It exists exactly where a reachable state steps
 * back into the initial state, which is what Simulator::init_reentrant() says of the tables.
 * @param simulator The tables.
 * @param inputs A shortest input per state, empty for the initial state.
 * @return The input, std::nullopt when no input returns to the initial state.
 */
[[nodiscard]] std::optional<std::string> shortest_re_entry(
        const dfa::Simulator& simulator, const std::vector<std::optional<std::string>>& inputs)
{
    std::optional<std::string> shortest;

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (!inputs[state])
        {
            continue;
        }

        for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
        {
            if (simulator.step(state, static_cast<unsigned char>(value)) != simulator.init_state())
            {
                continue;
            }

            auto returning{*inputs[state] + static_cast<char>(value)};

            if (!shortest || returning.size() < shortest->size())
            {
                shortest = std::move(returning);
            }
        }
    }

    return shortest;
}

/**
 * @brief The tokens reachable from each state: those the accepting states reachable from it accept, which are the
 *        tokens a match path through it can still be on its way to.
 *
 * Every reachable accepting state counts, not the nearest one, because an accepting state lies on the way to
 * longer tokens' accepting states: with the rules a[\nx] and a[\nx]b, the state accepting the shorter one is
 * where the scan of the longer one stands after the same bytes, so both tokens consume those bytes mid-token.
 * Once per state rather than once per state and byte, since the blame asks the same question of a state for
 * every byte that reaches it.
 * @param simulator The tables.
 * @return The token ids per state, ascending, empty for a state no accepting state is reachable from.
 */
[[nodiscard]] std::vector<std::set<std::size_t>> tokens_ahead(const dfa::Simulator& simulator)
{
    std::vector<std::set<std::size_t>> ahead(simulator.state_count());

    for (std::size_t from{0}; from < simulator.state_count(); ++from)
    {
        std::vector<bool> seen(simulator.state_count(), false);

        std::deque<std::size_t> pending{from};

        seen[from] = true;

        while (!pending.empty())
        {
            const auto at{pending.front()};

            pending.pop_front();

            if (const auto token{simulator.accepted(at)})
            {
                ahead[from].insert(token->id());
            }

            for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
            {
                if (const auto to{simulator.step(at, static_cast<unsigned char>(value))}; to && !seen[*to])
                {
                    seen[*to] = true;

                    pending.push_back(*to);
                }
            }
        }
    }

    return ahead;
}

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

    // A backslash is written as two, so that the two bytes of a backslash and an n are told apart from the one
    // byte of a newline, which is written `\n`.
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

        // A double quote inside the string is written as an escape, so the quotes the string is shown in are its
        // own and a witness holding one does not look as though it closed early.
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
[[nodiscard]] bool printable(const std::string_view window) noexcept
{
    return std::ranges::all_of(window, [](const char byte) { return byte >= 0x20 && byte < 0x7F; });
}

/**
 * @brief A byte string as a JSON string, each byte the code point of its value.
 * @param bytes The bytes.
 * @return The JSON text, quotes included.
 */
[[nodiscard]] std::string quoted(const std::string_view bytes)
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
 * @brief A span as JSON: the number, or "unbounded".
 * @param span The span.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_span(const std::optional<std::size_t>& span)
{
    return span ? std::to_string(*span) : R"("unbounded")";
}

/**
 * @brief A window count as JSON: the number, or the bound it passed as a string.
 * @param count The count, std::nullopt when it is more than a std::size_t can hold.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_count(const std::optional<std::size_t>& count)
{
    return count ? std::to_string(*count) : quoted(counted(count));
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
 * @brief A token as JSON: its id and its name.
 * @param token The token id.
 * @param name The naming.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_token(const std::size_t token, const std::function<std::string(std::size_t)>& name)
{
    return std::format(R"({{"id": {}, "name": {}}})", token, quoted(name(token)));
}

} // namespace

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

    return std::format(
            "nothing certifies up to width {}{}", report.window_limit,
            report.prices.empty() ? "" : "; what certifying a byte would cost is priced below");
}

std::vector<Blame> blame(const core::Lexer& lexer)
{
    const auto& simulator{lexer.simulator()};

    const auto inputs{shortest_inputs(simulator)};

    const auto ahead{tokens_ahead(simulator)};

    // The initial state is exempt on the entry before any input alone, where a byte may begin a token. Where a
    // nonempty input returns to it, the scan stands in it mid-token, so it blames like any other state, after that
    // input rather than after none.
    const auto re_entry{shortest_re_entry(simulator, inputs)};

    std::vector<Blame> blame;

    for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<unsigned char>(value)};

        const auto from_start{simulator.step(simulator.init_state(), byte)};

        // Only a byte the initial state consumes live could have certified; the rest never begin a token.
        if (!from_start || !simulator.is_live(*from_start) || lexer.is_split_point(static_cast<char>(byte)))
        {
            continue;
        }

        std::map<std::size_t, std::string> shortest_by_token;

        for (std::size_t state{0}; state < simulator.state_count(); ++state)
        {
            const auto& reached{state == simulator.init_state() ? re_entry : inputs[state]};

            if (!reached || !simulator.is_live(state))
            {
                continue;
            }

            const auto to{simulator.step(state, byte)};

            if (!to || !simulator.is_live(*to))
            {
                continue;
            }

            for (const auto token : ahead[*to])
            {
                const auto found{shortest_by_token.find(token)};

                if (found == shortest_by_token.end() || reached->size() < found->second.size())
                {
                    shortest_by_token.insert_or_assign(token, *reached);
                }
            }
        }

        for (const auto& [token, after] : shortest_by_token)
        {
            blame.push_back({.byte = byte, .token = token, .after = after});
        }
    }

    return blame;
}

Report audit(const Token_set& set, const std::size_t window_limit)
{
    auto report{audit(compile(set), window_limit)};

    for (const auto& [regex, id, priority, discarded] : set.rules)
    {
        if (discarded)
        {
            report.discarded.push_back(id);
        }
    }

    // The newline first, since it is the byte every designer asks about, then the near misses.
    std::vector<unsigned char> asked{'\n'};

    for (const auto byte : report.modulo)
    {
        if (byte != '\n' && !std::ranges::binary_search(report.exact, byte))
        {
            asked.push_back(byte);
        }
    }

    for (const auto byte : asked)
    {
        if (!std::ranges::binary_search(report.exact, byte))
        {
            report.prices.push_back(price(set, byte));
        }
    }

    return report;
}

Report audit(const core::Lexer& lexer, const std::size_t window_limit)
{
    const auto& simulator{lexer.simulator()};

    Report report{
            .nullable = simulator.nullable(),
            .exact = {},
            .modulo = {},
            .discarded = {},
            .classes = 0,
            .window_limit = window_limit,
            .windows = {},
            .window_count = std::nullopt,
            .mandatory_core = std::string{lexer.mandatory_core()},
            .byte_span = lexer.anchor_free_span(),
            .window_span = std::nullopt,
            .lag = lexer.lag(),
            .rescue = lexer.rescue(),
            .blame = {},
            .prices = {}};

    for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<char>(value)};

        if (lexer.is_split_point(byte))
        {
            report.exact.push_back(static_cast<unsigned char>(value));
        }

        if (lexer.is_split_point_ignoring(byte))
        {
            report.modulo.push_back(static_cast<unsigned char>(value));
        }
    }

    const auto classes{byte_classes(simulator)};

    report.classes = classes.size();

    report.windows = certified_windows(lexer, classes, window_limit);

    report.window_count = expansion_count(report.windows, classes);

    if (!report.windows.empty())
    {
        if (const auto inventory{expanded(report.windows, classes)})
        {
            std::vector<std::pair<std::string_view, std::size_t>> views;

            views.reserve(inventory->size());

            for (const auto& [window, origin] : *inventory)
            {
                views.emplace_back(window, origin);
            }

            report.window_span = lexer.anchor_free_span(views);
        }
    }

    report.blame = blame(lexer);

    return report;
}

std::string json(const Report& report, const std::function<std::string(std::size_t)>& name)
{
    std::string out{"{\n"};

    const auto member{[&out](const std::string_view key, const std::string& value) {
        out += std::format(R"({}  "{}": {})", out.size() == 2 ? "" : ",\n", key, value);
    }};

    const auto list{[]<typename Items, typename One>(const Items& items, const One& one) {
        std::string text{'['};

        for (const auto& item : items)
        {
            text += (text.size() == 1 ? "" : ", ") + one(item);
        }

        return text + ']';
    }};

    member("verdict", quoted(verdict(report)));

    member("nullable", report.nullable ? "true" : "false");

    member("exact", json_bytes(report.exact));

    member("modulo", json_bytes(report.modulo));

    member("discarded", list(report.discarded, [&name](const std::size_t token) { return json_token(token, name); }));

    member("byte_classes", std::to_string(report.classes));

    member("window_limit", std::to_string(report.window_limit));

    member("windows", list(report.windows, [](const Certified_window& certified) {
               const auto& [window, origin]{certified};

               return std::format(R"({{"window": {}, "origin": {}}})", quoted(window), origin);
           }));

    member("window_count", json_count(report.window_count));

    member("mandatory_core", quoted(report.mandatory_core));

    member("byte_span", json_span(report.byte_span));

    member("window_span", report.windows.empty() ? "null" :
                          report.window_span     ? json_span(*report.window_span) :
                                                   R"("undecided")");

    member("lag", json_span(report.lag));

    member("rescue_free", !report.rescue.exhaustive ? "null" : report.rescue.witness.empty() ? "true" : "false");

    member("rescue_witness", report.rescue.witness.empty() ? "null" : quoted(report.rescue.witness));

    member("blame", list(report.blame, [&name](const Blame& blamed) {
               const auto& [byte, token, after]{blamed};

               return std::format(
                       R"({{"byte": {}, "token": {}, "after": {}}})", byte, json_token(token, name), quoted(after));
           }));

    member("prices", list(report.prices, [&](const Pricing& pricing) {
               const auto steps_text{list(pricing.steps, [&name](const Price_step& step) {
                   const auto& [token, shape, separated, separated_discarded, exact, modulo]{step};

                   return std::format(
                           R"({{"token": {}, "shape": "{}", "separated": {}, "separated_discarded": {}, )"
                           R"("exact": {}, "modulo": {}}})",
                           json_token(token, name), shape_name(shape), separated, separated_discarded, exact, modulo);
               })};

               const auto immovable_text{
                       list(pricing.immovable, [&name](const std::size_t token) { return json_token(token, name); })};

               const auto undecided_text{
                       list(pricing.undecided, [&name](const std::size_t token) { return json_token(token, name); })};

               const auto json_outcome{[](const Outcome& after) {
                   return std::format(
                           R"({{"exact": {}, "modulo": {}, "gained": {}}})", after.exact, after.modulo,
                           json_bytes(after.gained));
               }};

               const auto choices_text{list(pricing.choices, [&name, &json_outcome](const Choice& choice) {
                   const auto& [token, shape, after]{choice};

                   return std::format(
                           R"({{"token": {}, "shape": "{}", "after": {}}})", json_token(token, name), shape_name(shape),
                           json_outcome(after));
               })};

               return std::format(
                       R"({{"byte": {}, "exact_before": {}, "modulo_before": {}, "given": {}, "steps": {}, )"
                       R"("immovable": {}, "undecided": {}, "gained": {}, "choices": {}, "together": {}}})",
                       pricing.byte, pricing.exact_before, pricing.modulo_before,
                       pricing.given ? json_outcome(*pricing.given) : "null", steps_text, immovable_text,
                       undecided_text, json_bytes(pricing.gained), choices_text,
                       pricing.together ? json_outcome(*pricing.together) : "null");
           }));

    return out + "\n}";
}

std::string render(const Report& report, const std::function<std::string(std::size_t)>& name)
{
    std::string out;

    const auto line{[&out](const std::string_view label, const std::string& value) {
        out += std::format("{:<28}{}\n", label, value);
    }};

    line("verdict", verdict(report));

    if (report.nullable)
    {
        line("", "a token matches the empty string: decided through the positive-width equivalent");
    }

    line("certified bytes", shown(report.exact));

    line("certified modulo discarded", shown(report.modulo));

    // The discarded tokens, so that what the modulo row deleted is on the page.
    if (!report.discarded.empty())
    {
        std::string names;

        for (const auto id : report.discarded | std::views::take(6))
        {
            names += (names.empty() ? "" : ", ") + name(id);
        }

        if (report.discarded.size() > 6)
        {
            names += std::format(", ... {} more", report.discarded.size() - 6);
        }

        line("discarded tokens", std::format("{}: {}", report.discarded.size(), names));
    }

    // Windows: the count over class representatives, widths ascending, and the first few as examples.
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

        line(std::format("certified windows (<= {})", report.window_limit),
             report.windows.empty() ? "none" :
                                      std::format(
                                              "{} over {} byte classes, {} once classes expand", summary,
                                              report.classes, counted(report.window_count)));

        // The examples: printable windows first, since a reader recognises those.
        auto examples{report.windows};

        std::ranges::stable_partition(
                examples, [](const Certified_window& certified) { return printable(certified.window); });

        for (const auto& [window, origin] : examples | std::views::take(6))
        {
            line("", std::format("{} at {}", shown(std::string_view{window}), origin));
        }

        if (report.windows.size() > 6)
        {
            line("", std::format("... {} more", report.windows.size() - 6));
        }
    }

    line("mandatory core", report.mandatory_core.empty() ? "none" : shown(std::string_view{report.mandatory_core}));

    line("anchor-free span, bytes", shown(report.byte_span));

    if (!report.windows.empty())
    {
        line("anchor-free span, windows",
             report.window_span ?
                     shown(*report.window_span) :
                     std::format("not decided: more than {} windows once classes expand", span_window_cap));
    }

    line("lag", shown(report.lag));

    line("rescue-free",
         !report.rescue.exhaustive ?
                 std::format("not decided: the search passed {} states", dfa::rescue_cap) :
         report.rescue.witness.empty() ?
                 "yes" :
                 std::format("no: the scan rolls back and continues on {}", shown(report.rescue.witness)));

    // Blame: grouped by token, the bytes it de-certifies and the shortest input after which it consumes one.
    if (!report.blame.empty())
    {
        out += "\nwhy candidate bytes do not certify\n";

        std::map<std::size_t, Consumed> by_token;

        for (const auto& [byte, token, after] : report.blame)
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
    }

    // Prices: per byte, the edits in order with the certificate after each, and what cannot move.
    for (const auto& pricing : report.prices)
    {
        const auto& [byte, exact_before, modulo_before, given, steps, immovable, undecided, gained, choices, together]{
                pricing};

        const auto stood{[](const Outcome& after) {
            return std::string{
                           after.exact  ? "certifies exactly" :
                           after.modulo ? "certifies once discarded tokens are deleted" :
                                          "still does not certify"} +
                   (after.gained.empty() ? "" : "; also certified: " + shown(after.gained));
        }};

        out += std::format("\nwhat it would cost to certify {}\n", shown(byte));

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

            out += std::format("  {:<26} {}\n", "", stood(*given));
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

        // The tokens the narrowing is not applied to and has no verdict about: some word of the token holds the byte
        // fixed only where a token may begin with it, so the impossibility above would be a claim the procedure has
        // not earned.
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
            out += std::format(
                    "\nwhat the shapes of the tokens consuming {} offer, each edit on its own\n", shown(byte));
        }

        for (const auto& [token, shape, after] : choices)
        {
            out += std::format("  {:<26} {}: {}\n", name(token), shape_name(shape), shape_edit(shape));

            out += std::format("  {:<26} {}\n", "", stood(after));
        }

        if (together)
        {
            out += std::format("  {:<26} {}\n", "every shape's edit together", stood(*together));
        }
    }

    return out;
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
        out += (out.size() == 1 ? "" : ", ") + quoted(option);
    }

    return out + ']';
}

} // namespace munch::tools::audit
