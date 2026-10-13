#include "munch/tools/audit/window_modulo.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <iterator>
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
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief How a hypothesis crossed the gap before the cut's byte: not yet, at a token boundary, inside a token at a
 *        state that passes the restart test, or inside one at a state that does not.
 */
enum class Gap : std::uint8_t
{
    unset,
    boundary,
    harmless,
    harmful
};

/**
 * @brief One hypothesis of a cloud: a state, the offset its token began at, and how it crossed the cut's gap.
 */
struct Hypothesis
{
    /**
     * @brief Ordered member by member, so a cloud sorts and its duplicates meet.
     */
    auto operator<=>(const Hypothesis&) const = default;

    /**
     * @brief The state.
     */
    std::size_t state{};

    /**
     * @brief The offset the state's token began at, before_window when it began before the window.
     */
    std::size_t origin{};

    /**
     * @brief How the hypothesis crossed the cut's gap.
     */
    Gap gap{};
};

/**
 * @brief A cloud of hypotheses, kept ascending and without duplicates between positions.
 */
using Cloud_t = std::vector<Hypothesis>;

/**
 * @brief Whether reading each byte from each state passes the restart test, by state and then byte.
 */
using Restarts_t = std::vector<std::vector<bool>>;

/**
 * @brief The origin that marks a hypothesis whose token began before the window, a value no in-window offset can take.
 */
constexpr std::size_t before_window{std::numeric_limits<std::size_t>::max()};

/**
 * @brief How many example windows the section shows before it counts the rest.
 */
constexpr std::size_t shown_examples{6};

/**
 * @brief Returns which states accept a discarded token.
 * @param simulator The compiled token set.
 * @param discarded The discarded tokens, by id.
 * @return One flag per state.
 */
[[nodiscard]] std::vector<bool> discarded_accepting(
        const dfa::Simulator& simulator, const std::vector<std::size_t>& discarded)
{
    std::vector<bool> accepts(simulator.state_count(), false);

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        const auto token{simulator.accepted(state)};

        accepts[state] = token && std::ranges::contains(discarded, token->id());
    }

    return accepts;
}

/**
 * @brief Returns which states some input leads to a state accepting a kept token, the state itself among them: one
 *        backward closure from the kept accepting states.
 * @param simulator The compiled token set.
 * @param accepts_discarded Which states accept a discarded token.
 * @param symbols One byte of every byte class.
 * @return One flag per state.
 */
[[nodiscard]] std::vector<bool> reaching_kept(
        const dfa::Simulator& simulator, const std::vector<bool>& accepts_discarded,
        const std::vector<unsigned char>& symbols)
{
    const auto states{simulator.state_count()};

    std::vector<std::vector<std::size_t>> predecessors(states);

    for (std::size_t state{0}; state < states; ++state)
    {
        for (const auto symbol : symbols)
        {
            if (const auto to{simulator.step(state, symbol)})
            {
                predecessors[*to].push_back(state);
            }
        }
    }

    std::vector<bool> reaches(states, false);

    std::deque<std::size_t> frontier{};

    for (std::size_t state{0}; state < states; ++state)
    {
        if (simulator.is_accepting(state) && !accepts_discarded[state])
        {
            reaches[state] = true;

            frontier.push_back(state);
        }
    }

    while (!frontier.empty())
    {
        const auto state{frontier.front()};

        frontier.pop_front();

        for (const auto from : predecessors[state])
        {
            if (reaches[from])
            {
                continue;
            }

            reaches[from] = true;

            frontier.push_back(from);
        }
    }

    return reaches;
}

/**
 * @brief Returns the classes of states no continuation tells apart once every discarded token is one colour, Moore's
 *        refinement from the colours nonaccepting, discarded and each kept token, over the states and one more index
 *        standing for the absent state.
 * @param simulator The compiled token set.
 * @param accepts_discarded Which states accept a discarded token.
 * @param symbols One byte of every byte class.
 * @return The class of each state, and at the index state_count() the absent state's.
 */
[[nodiscard]] std::vector<std::size_t> observed_classes(
        const dfa::Simulator& simulator, const std::vector<bool>& accepts_discarded,
        const std::vector<unsigned char>& symbols)
{
    const auto states{simulator.state_count()};

    // What colours a state before its token is told apart.
    enum class Shade : std::uint8_t
    {
        nonaccepting,
        discarded,
        kept
    };

    // A pair, so that no kept token ID can collide with the discarded or the nonaccepting colour.
    const auto colour{[&](const std::size_t state) -> std::pair<Shade, std::size_t> {
        if (state == states || !simulator.is_accepting(state))
        {
            return {Shade::nonaccepting, 0};
        }

        if (accepts_discarded[state])
        {
            return {Shade::discarded, 0};
        }

        return {Shade::kept, simulator.accepted(state)->id()};
    }};

    const auto target{[&](const std::size_t state, const unsigned char symbol) {
        const auto to{state == states ? std::nullopt : simulator.step(state, symbol)};

        return to ? *to : states;
    }};

    std::vector<std::size_t> current(states + 1U);

    std::map<std::pair<Shade, std::size_t>, std::size_t> first_classes{};

    for (std::size_t state{0}; state <= states; ++state)
    {
        const auto state_colour{colour(state)};

        const auto [found, inserted]{first_classes.try_emplace(state_colour, first_classes.size())};

        const auto& [known_colour, number]{*found};

        current[state] = number;
    }

    auto count{first_classes.size()};

    for (;;)
    {
        std::map<std::vector<std::size_t>, std::size_t> signatures{};

        std::vector<std::size_t> next(states + 1U);

        for (std::size_t state{0}; state <= states; ++state)
        {
            std::vector<std::size_t> signature{current[state]};

            for (const auto symbol : symbols)
            {
                const auto to{target(state, symbol)};

                signature.push_back(current[to]);
            }

            const auto fresh{signatures.size()};

            const auto [found, inserted]{signatures.try_emplace(std::move(signature), fresh)};

            const auto& [state_signature, number]{*found};

            next[state] = number;
        }

        current = std::move(next);

        // A refinement only splits classes, so an unchanged count is the fixpoint.
        if (signatures.size() == count)
        {
            return current;
        }

        count = signatures.size();
    }
}

/**
 * @brief Returns, for every state and byte, whether reading the byte from the state passes the restart test of
 *        is_split_point_ignoring(): the state accepts a discarded token, its successor reaches no kept token, and the
 *        successor lies in the observed class of the initial state's successor on the byte.
 * @param simulator The compiled token set.
 * @param discarded The discarded tokens, by id.
 * @param classes The byte classes.
 * @return The test's answers, by state and then byte; false wherever the state has no transition on the byte.
 */
[[nodiscard]] Restarts_t restart_test(
        const dfa::Simulator& simulator, const std::vector<std::size_t>& discarded,
        const std::vector<std::vector<unsigned char>>& classes)
{
    std::vector<unsigned char> symbols{};

    std::ranges::transform(classes, std::back_inserter(symbols), [](const std::vector<unsigned char>& members) {
        return members.front();
    });

    const auto accepts_discarded{discarded_accepting(simulator, discarded)};

    const auto reaches_kept{reaching_kept(simulator, accepts_discarded, symbols)};

    const auto observed{observed_classes(simulator, accepts_discarded, symbols)};

    const auto states{simulator.state_count()};

    Restarts_t passes(states, std::vector<bool>(byte_values, false));

    for (std::size_t state{0}; state < states; ++state)
    {
        for (const auto value : std::views::iota(0UZ, byte_values))
        {
            const auto byte{static_cast<unsigned char>(value)};

            const auto to{simulator.step(state, byte)};

            if (!to || !accepts_discarded[state] || reaches_kept[*to])
            {
                continue;
            }

            const auto restarted{simulator.step(simulator.init_state(), byte)};

            const auto restarted_class{observed[restarted ? *restarted : states]};

            passes[state][value] = observed[*to] == restarted_class;
        }
    }

    return passes;
}

/**
 * @brief Returns the byte a class is shown by: its first graphic member when it has one, since a reader recognises that
 *        one, else its lowest; every member decides alike, so the choice is a matter of display alone.
 * @param members The class, ascending.
 * @return The representative.
 */
[[nodiscard]] unsigned char representative(const std::vector<unsigned char>& members) noexcept
{
    const auto graphic{std::ranges::find_if(members, is_graphic)};

    return graphic == members.end() ? members.front() : *graphic;
}

/**
 * @brief Returns how a hypothesis reading the cut's byte crosses the gap before it.
 * @param begins Whether it reads the byte from an initial state that nothing re-enters, beginning a token there.
 * @param passes Whether it reads the byte from a state that passes the restart test.
 * @return The crossing.
 */
[[nodiscard]] Gap crossing(const bool begins, const bool passes) noexcept
{
    if (begins)
    {
        return Gap::boundary;
    }

    return passes ? Gap::harmless : Gap::harmful;
}

/**
 * @brief Returns the cloud one byte leads a cloud to, as is_split_window() steps its cloud, with how each hypothesis
 *        crossed the cut's gap.
 *
 * Every hypothesis advances or is dropped, its origin renamed to this offset where it reads from an initial state that
 * nothing re-enters, and a fresh hypothesis begins here wherever some hypothesis had just accepted. At the cut's byte a
 * direct step takes its crossing from the restart test and a renamed or fresh hypothesis the boundary; elsewhere a
 * direct step keeps its crossing and a fresh hypothesis enters once with the crossing of each accepting hypothesis it
 * follows.
 * @param simulator The compiled token set.
 * @param restarts The restart test's answers.
 * @param cloud The cloud advanced from.
 * @param byte The byte read.
 * @param at The byte's offset in the window.
 * @param cut The cut's offset, which may lie past the byte.
 * @return The cloud, ascending and without duplicates.
 */
[[nodiscard]] Cloud_t step(
        const dfa::Simulator& simulator, const Restarts_t& restarts, const Cloud_t& cloud, const unsigned char byte,
        const std::size_t at, const std::size_t cut)
{
    Cloud_t next{};

    std::set<Gap> followed{};

    for (const auto& [state, origin, gap] : cloud)
    {
        if (simulator.is_accepting(state))
        {
            followed.insert(gap);
        }

        const auto to{simulator.step(state, byte)};

        if (!to || !simulator.is_live(*to))
        {
            continue;
        }

        const auto begins{state == simulator.init_state() && !simulator.init_reentrant()};

        const auto stepped_origin{begins ? at : origin};

        const auto stepped_gap{at == cut ? crossing(begins, restarts[state][byte]) : gap};

        next.push_back({.state = *to, .origin = stepped_origin, .gap = stepped_gap});
    }

    const auto fresh{simulator.step(simulator.init_state(), byte)};

    // At the cut's byte the token before it ended at the gap, so a fresh hypothesis crossed it at a boundary.
    if (at == cut && !followed.empty())
    {
        followed = {Gap::boundary};
    }

    if (fresh && simulator.is_live(*fresh))
    {
        for (const auto gap : followed)
        {
            next.push_back({.state = *fresh, .origin = at, .gap = gap});
        }
    }

    std::ranges::sort(next);

    const auto duplicates{std::ranges::unique(next)};

    next.erase(duplicates.begin(), duplicates.end());

    return next;
}

/**
 * @brief Returns whether a cloud past a window's last byte certifies the cut: it is nonempty and each hypothesis began
 *        its token at the cut or crossed the gap before it harmlessly.
 * @param cloud The cloud.
 * @param cut The cut's offset.
 * @return True when it certifies.
 */
[[nodiscard]] bool certifies(const Cloud_t& cloud, const std::size_t cut)
{
    const auto cut_safe{[cut](const Hypothesis& hypothesis) {
        const auto& [state, origin, gap]{hypothesis};

        return origin == cut || gap == Gap::harmless;
    }};

    return !cloud.empty() && std::ranges::all_of(cloud, cut_safe);
}

/**
 * @brief The search under way: the compiled set, the restart test's answers, the byte classes and the longest width,
 *        and the windows found so far, by width.
 *
 * The search runs depth first over the windows of class representatives, and each window carries one cloud per cut it
 * may certify and one whose cut lies past its last byte, so that a window extends its prefix's walks by one step each
 * rather than walking again from its first byte. A window whose last cloud is empty occurs in no completely tokenizable
 * input, nor does any window extending it, so the search does not go past it.
 */
class Search
{
public:
    /**
     * @brief Starts the search, nothing searched yet.
     * @param lexer The compiled token set.
     * @param restarts The restart test's answers.
     * @param classes The byte classes.
     * @param limit The longest width tried.
     */
    Search(const core::Lexer& lexer, const Restarts_t& restarts, const std::vector<std::vector<unsigned char>>& classes,
           std::size_t limit);

    /**
     * @brief Searches every window over class representatives up to the longest width for those that certify a cut once
     *        the discarded tokens are deleted and are not certified split windows.
     * @return The windows, widths ascending and each width in the order of its classes, each at its least cut.
     */
    [[nodiscard]] std::vector<Window_cut> run() &&;

private:
    /**
     * @brief Extends a window by one representative of every class in turn, records each extension that certifies a cut
     *        and the exact certificate refuses, and searches past each extension that occurs.
     * @param window The window extended, restored before returning.
     * @param clouds The window's clouds: one per cut at an offset inside it, and last the one whose cut lies past it.
     */
    void extend(std::string& window, const std::vector<Cloud_t>& clouds);

    /**
     * @brief The longest width tried.
     */
    std::size_t limit_{};

    /**
     * @brief The compiled token set.
     */
    const core::Lexer& lexer_;

    /**
     * @brief The restart test's answers.
     */
    const Restarts_t& restarts_;

    /**
     * @brief The windows found, by width.
     */
    std::vector<std::vector<Window_cut>> found_{};

    /**
     * @brief The byte classes.
     */
    const std::vector<std::vector<unsigned char>>& classes_;
};

Search::Search(
        const core::Lexer& lexer, const Restarts_t& restarts, const std::vector<std::vector<unsigned char>>& classes,
        const std::size_t limit)
    : limit_{limit}, lexer_{lexer}, restarts_{restarts}, found_(limit + 1), classes_{classes}
{}

std::vector<Window_cut> Search::run() &&
{
    const auto& simulator{lexer_.simulator()};

    Cloud_t cloud{};

    const auto is_live{[&simulator](const std::size_t state) { return simulator.is_live(state); }};

    const auto states{std::views::iota(std::size_t{0}, simulator.state_count())};

    for (const auto state : states | std::views::filter(is_live))
    {
        cloud.push_back({.state = state, .origin = before_window, .gap = Gap::unset});
    }

    std::string window{};

    extend(window, {cloud});

    std::vector<Window_cut> windows{};

    for (auto& width : found_)
    {
        std::ranges::move(width, std::back_inserter(windows));
    }

    return windows;
}

void Search::extend(std::string& window, const std::vector<Cloud_t>& clouds)
{
    const auto& simulator{lexer_.simulator()};

    const auto at{window.size()};

    for (const auto& members : classes_)
    {
        const auto byte{representative(members)};

        std::vector<Cloud_t> next{};

        // The cut at offset `at` and the one past it both continue the walk whose cut lay ahead.
        for (std::size_t cut{0}; cut <= at + 1; ++cut)
        {
            const auto& from{clouds[std::min(cut, at)]};

            auto stepped{step(simulator, restarts_, from, byte, at, cut)};

            next.push_back(std::move(stepped));
        }

        if (next.back().empty())
        {
            continue;
        }

        window.push_back(static_cast<char>(byte));

        const auto cut_certified{[&next](const std::size_t cut) { return certifies(next[cut], cut); }};

        const auto cuts{std::views::iota(0UZ, window.size())};

        const auto least{std::ranges::find_if(cuts, cut_certified)};

        // A window is two bytes at least, and one the exact certificate certifies is the report's own.
        if (window.size() >= 2 && least != cuts.end() && !lexer_.is_split_window(window))
        {
            found_[window.size()].push_back({.window = window, .cut = *least});
        }

        if (window.size() < limit_)
        {
            extend(window, next);
        }

        window.pop_back();
    }
}

/**
 * @brief Returns how many byte strings the windows stand for once every representative expands to its class, counted
 *        checked, since the count can outgrow what holds it.
 * @param windows The windows over class representatives.
 * @param classes The byte classes.
 * @return The count, or std::nullopt when it is more than a std::size_t can hold.
 */
[[nodiscard]] std::optional<std::size_t> expansion_count(
        const std::vector<Window_cut>& windows, const std::vector<std::vector<unsigned char>>& classes)
{
    std::map<unsigned char, std::size_t> class_size{};

    for (const auto& members : classes)
    {
        class_size[representative(members)] = members.size();
    }

    std::size_t total{0};

    static constexpr auto most{std::numeric_limits<std::size_t>::max()};

    for (const auto& [window, cut] : windows)
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
 * @brief Returns a window count as the report prints it: the number, or the bound the count passed when none holds it.
 * @param count The count, std::nullopt when it is more than a std::size_t can hold.
 * @return The rendering.
 */
[[nodiscard]] std::string counted(const std::optional<std::size_t>& count)
{
    return count ? std::to_string(*count) : std::format("more than {}", std::numeric_limits<std::size_t>::max());
}

/**
 * @brief Returns the escape a byte is written as inside the quotes a window is shown in, when it has one: a common
 *        control, the backslash, and the quote.
 * @param byte The byte.
 * @return The escape, or std::nullopt for a byte written as itself or in hex.
 */
[[nodiscard]] std::optional<std::string_view> quoted_escape(const unsigned char byte)
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
        return R"(\\)";
    case '"':
        return R"(\")";
    default:
        break;
    }

    return std::nullopt;
}

/**
 * @brief Returns a byte string as the report prints it, each byte as its escape, as itself when it is printable, and in
 *        hex as the escape `\xHH` otherwise.
 * @param bytes The bytes.
 * @return The rendering, quoted once.
 */
[[nodiscard]] std::string shown(const std::string_view bytes)
{
    std::string out{'"'};

    for (const auto byte : bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (const auto escape{quoted_escape(value)})
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
 * @brief Returns a byte string as a JSON string, each byte the code point of its value.
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
            out += std::format(R"(\{})", byte);

            continue;
        }

        if (!is_printable(value))
        {
            out += std::format(R"(\u{:04x})", value);

            continue;
        }

        out += byte;
    }

    return out + '"';
}

} // namespace

Windows_modulo windows_modulo(const Report& report, const core::Lexer& lexer)
{
    if (report.discarded.empty())
    {
        return {.discards = false, .class_count = report.classes.size(), .windows = {}, .window_count = 0};
    }

    const auto restarts{restart_test(lexer.simulator(), report.discarded, report.classes)};

    auto windows{Search{lexer, restarts, report.classes, report.window_limit}.run()};

    const auto window_count{expansion_count(windows, report.classes)};

    return {.discards = true,
            .class_count = report.classes.size(),
            .windows = std::move(windows),
            .window_count = window_count};
}

std::string windows_modulo_json(const Windows_modulo& found)
{
    const auto& [discards, class_count, windows, window_count]{found};

    if (!discards)
    {
        return "null";
    }

    const auto window_json{[](const Window_cut& certified) {
        const auto& [window, cut]{certified};

        return std::format(R"({{"window": {}, "cut": {}}})", json_byte_string(window), cut);
    }};

    std::string entries{};

    auto joined{windows | std::views::transform(window_json) | std::views::join_with(std::string_view{", "})};

    std::ranges::copy(joined, std::back_inserter(entries));

    // A count past what a std::size_t holds prints as the bound it passed, a string.
    const auto count{window_count ? std::to_string(*window_count) : json_byte_string(counted(window_count))};

    return std::format(R"({{"windows": [{}], "window_count": {}}})", entries, count);
}

std::string windows_modulo_section(const Windows_modulo& found)
{
    const auto& [discards, class_count, windows, window_count]{found};

    std::string out{"\nwhat deleting the discarded tokens adds to the windows\n"};

    const auto row{[&out](const std::string_view label, const std::string_view value) {
        out += std::format("  {:<26} {}\n", label, value);
    }};

    const std::string_view label{"windows modulo discarded"};

    if (!discards)
    {
        row(label, "none, since no token is discarded");

        return out;
    }

    if (windows.empty())
    {
        row(label, "none");

        return out;
    }

    std::map<std::size_t, std::size_t> per_width{};

    for (const auto& [window, cut] : windows)
    {
        ++per_width[window.size()];
    }

    const auto at_width{[]<typename Entry>(const Entry& entry) {
        const auto& [width, count]{entry};

        return std::format("{} at width {}", count, width);
    }};

    std::string summary{};

    auto joined{per_width | std::views::transform(at_width) | std::views::join_with(std::string_view{", "})};

    std::ranges::copy(joined, std::back_inserter(summary));

    const auto expanded{counted(window_count)};

    const auto counts{std::format(
            "{} over {} byte classes, {} once classes expand, the exact ones left out", summary, class_count,
            expanded)};

    row(label, counts);

    // A window of printable ASCII makes the better example.
    const auto printable_window{[](const Window_cut& certified) {
        const auto& [window, cut]{certified};

        const auto printable{[](const char byte) { return is_printable(static_cast<unsigned char>(byte)); }};

        return std::ranges::all_of(window, printable);
    }};

    auto examples{windows};

    std::ranges::stable_partition(examples, printable_window);

    for (const auto& [window, cut] : examples | std::views::take(shown_examples))
    {
        row("", std::format("{} cut at {}", shown(window), cut));
    }

    if (windows.size() > shown_examples)
    {
        row("", std::format("... {} more", windows.size() - shown_examples));
    }

    return out;
}

} // namespace munch::tools::audit
