#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_WINDOW_PLANNER_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_WINDOW_PLANNER_HPP

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "munch/common/concepts.hpp"
#include "munch/dfa/simulator.hpp"
#include "munch/dfa/split_window.hpp"

namespace munch::core
{
/**
 * @brief The window search behind Lexer::chunk_boundaries_with_windows() and next_certified_evidence(): where in an
 *        input a certified split window first licenses a cut.
 *
 * One planner lives for one plan and holds that plan's state alone, the window budget, the memo and the barren offset;
 * the token set is handed in with each question, so the planner owns nothing it could outlive. It memoizes every window
 * decision by byte string, so the decisions, the costly part, are bounded by the distinct windows tried while the
 * positional walks scale with the positions examined; and when the token set proved a mandatory core it searches only
 * where the core occurs, tightening the barren offset across calls so that targets falling in a tail already proved
 * occurrence-free refuse without rescanning it. Byte certificates are not its business: the callers decide when windows
 * are consulted at all.
 */
class Window_planner
{
public:
    /**
     * @brief Constructs a planner whose searches try windows of two to four bytes, longest_window.
     */
    Window_planner() = default;

    /**
     * @brief Constructs a planner whose searches try windows of two to the given number of bytes, the window budget.
     *
     * A budget above four lets the searches find windows the default misses, at the cost of one certification per new
     * distinct spelling; under two no window is tried and every search finds nothing.
     * @param longest The longest window the searches try, in bytes.
     */
    explicit Window_planner(std::size_t longest) noexcept;

    /**
     * @brief Returns the first window beginning at a position that certifies: lengths ascending from two to the budget,
     *        first certificate wins.
     * @tparam Iterator Random access iterator type.
     * @param simulator The token set the plan is for.
     * @param begin Iterator to the beginning of the input.
     * @param size The input's size.
     * @param at The position the window begins at; at or past the input's size finds nothing, as the searches beginning
     *        at an offset have it.
     * @return The certified origin and the window's length, or std::nullopt when no window there certifies.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> window_at(
            const dfa::Simulator& simulator, Iterator begin, const std::size_t size, const std::size_t at)
    {
        const auto limit{at < size ? std::min(longest_, size - at) : 0UZ};

        for (auto length{shortest_window}; length <= limit; ++length)
        {
            if (const auto origin{certified_origin(simulator, begin, at, length)})
            {
                return std::pair{*origin, length};
            }
        }

        return std::nullopt;
    }

    /**
     * @brief Returns the first cut a certified window licenses at or after a floor: the occurrence plus the certified
     *        origin, in the exhaustive walk's position-then-length order whether or not a core filters the walk.
     * @tparam Iterator Random access iterator type.
     * @param simulator The token set the plan is for.
     * @param begin Iterator to the beginning of the input.
     * @param size The input's size.
     * @param floor The position the search starts at; at or past the input's size no window begins at or after it, so
     *        the search finds nothing, as the search at one position has it.
     * @return The cut, or std::nullopt when no window at or after the floor certifies.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::optional<std::size_t> cut(
            const dfa::Simulator& simulator, Iterator begin, const std::size_t size, const std::size_t floor)
    {
        if (floor >= size)
        {
            return std::nullopt;
        }

        return simulator.mandatory_core().empty() ? exhaustive(simulator, begin, size, floor) :
                                                    at_core(simulator, begin, size, floor);
    }

    /**
     * @brief The shortest window the searches try, two bytes. The bound is not a guard: the planners consult windows
     *        only when no exact byte certifies and the set is not nullable, where the length-one equivalence theorem
     *        makes every one-byte window refuse, so skipping length one is provably inert rather than something a test
     *        could pin.
     */
    static constexpr std::size_t shortest_window{2};

    /**
     * @brief The longest window the searches try unless the planner is constructed with another budget, four bytes. A
     *        grammar needing longer windows than the budget degrades to fewer chunks, never to a cut that is unsafe on
     *        a completely tokenizable input.
     */
    static constexpr std::size_t longest_window{4};

private:
    /**
     * @brief One decision per distinct byte string per plan.
     */
    using Memo_t = std::map<std::string, std::optional<std::size_t>, std::less<>>;

    /**
     * @brief Reads one input element as the scanners read it, through unsigned char, so every byte-domain element type
     *        forms the same memo key; the string constructor's implicit conversion would reject std::byte.
     * @tparam Iterator Random access iterator type.
     * @param begin Iterator to the beginning of the input.
     * @param at The element's position.
     * @return The element as a char.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] static char byte(Iterator begin, const std::size_t at)
    {
        const auto element{begin[static_cast<std::ptrdiff_t>(at)]};

        const auto value{static_cast<unsigned char>(element)};

        return static_cast<char>(value);
    }

    /**
     * @brief Returns the memoized window decision at one occurrence: the certified origin, if any.
     * @tparam Iterator Random access iterator type.
     * @param simulator The token set the plan is for.
     * @param begin Iterator to the beginning of the input.
     * @param at The position the window begins at.
     * @param length The window's length.
     * @return The certified origin, or std::nullopt when the window is refused.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::optional<std::size_t> certified_origin(
            const dfa::Simulator& simulator, Iterator begin, const std::size_t at, const std::size_t length)
    {
        std::string window{};

        window.reserve(length);

        for (std::size_t offset{0}; offset < length; ++offset)
        {
            window.push_back(byte(begin, at + offset));
        }

        if (const auto found{memo_.find(window)}; found != memo_.end())
        {
            const auto& [known_window, known_verdict]{*found};

            return known_verdict;
        }

        const auto verdict{dfa::is_split_window(simulator, window)};

        memo_.emplace(std::move(window), verdict);

        return verdict;
    }

    /**
     * @brief Runs the exhaustive search: every position from the floor, lengths ascending, first certificate wins.
     * @tparam Iterator Random access iterator type.
     * @param simulator The token set the plan is for.
     * @param begin Iterator to the beginning of the input.
     * @param size The input's size.
     * @param floor The position the search starts at.
     * @return The cut, or std::nullopt when no window at or after the floor certifies.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::optional<std::size_t> exhaustive(
            const dfa::Simulator& simulator, Iterator begin, const std::size_t size, const std::size_t floor)
    {
        for (auto occurrence{floor}; occurrence + shortest_window <= size; ++occurrence)
        {
            if (const auto found{window_at(simulator, begin, size, occurrence)})
            {
                const auto [origin, length]{*found};

                return occurrence + origin;
            }
        }

        return std::nullopt;
    }

    /**
     * @brief Runs the core-filtered search: every certifying window provably contains the core with a byte after it, so
     *        candidates exist only where the core occurs, and visiting them in the exhaustive walk's own
     *        position-then-length order gives that walk's plan, refusals included.
     *
     * Positions are still scanned one by one, each compared with the core, a comparison that may examine the whole
     * core; windows are built and certified only near occurrences. A start's candidates are the windows holding the
     * first core occurrence at or after it plus a byte, the lengths from c + m + 1 - t to the budget for the m-byte
     * core at c and the start t, so visiting the starts ascending and each start's lengths ascending is the exhaustive
     * walk's own order, and a start more than budget - (m + 1) bytes before every occurrence has no candidate. A proved
     * core longer than the budget minus its trailing byte admits no candidate at all, so every target refuses at once,
     * exactly as the exhaustive walk would conclude after scanning to the end of the input.
     * @tparam Iterator Random access iterator type.
     * @param simulator The token set the plan is for.
     * @param begin Iterator to the beginning of the input.
     * @param size The input's size.
     * @param floor The position the search starts at.
     * @return The cut, or std::nullopt when no window at or after the floor certifies.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::optional<std::size_t> at_core(
            const dfa::Simulator& simulator, Iterator begin, const std::size_t size, const std::size_t floor)
    {
        const auto core{simulator.mandatory_core()};

        if (core.size() + 1 > longest_ || floor >= barren_)
        {
            return std::nullopt;
        }

        const auto matches{[&](const std::size_t at) {
            const auto agrees{[&](const std::size_t offset) { return byte(begin, at + offset) == core[offset]; }};

            return std::ranges::all_of(std::views::iota(std::size_t{0}, core.size()), agrees);
        }};

        std::size_t cursor{floor};

        std::size_t latest{floor};

        // Advances the cursor past the occurrence it returns, and tightens the barren offset once none is left.
        const auto next_occurrence{[&]() -> std::optional<std::size_t> {
            for (; cursor + core.size() <= size; ++cursor)
            {
                if (matches(cursor))
                {
                    latest = cursor + 1;

                    return cursor++;
                }
            }

            barren_ = std::min(barren_, latest);

            return std::nullopt;
        }};

        std::size_t settled{floor};

        // Each start's candidate lengths run from the shortest window holding its first occurrence plus a byte to the
        // budget, so visiting starts ascending and lengths ascending is the exhaustive walk's own order.
        while (const auto at{next_occurrence()})
        {
            const auto reach{*at + core.size() + 1};

            const auto lowest{reach > longest_ ? reach - longest_ : std::size_t{0}};

            for (auto start{std::max(settled, lowest)}; start <= *at; ++start)
            {
                const auto limit{std::min(longest_, size - start)};

                for (auto length{reach - start}; length <= limit; ++length)
                {
                    if (const auto origin{certified_origin(simulator, begin, start, length)})
                    {
                        return start + *origin;
                    }
                }
            }

            settled = *at + 1;
        }

        return std::nullopt;
    }

    /**
     * @brief The window decisions made so far, by byte string.
     */
    Memo_t memo_;

    /**
     * @brief The longest window the searches try, the window budget.
     */
    std::size_t longest_{longest_window};

    /**
     * @brief The offset at or after which no core occurrence begins, tightened by a scan that drains the input.
     */
    std::size_t barren_{std::numeric_limits<std::size_t>::max()};
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_WINDOW_PLANNER_HPP
