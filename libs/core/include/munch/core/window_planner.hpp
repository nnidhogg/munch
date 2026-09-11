#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_WINDOW_PLANNER_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_WINDOW_PLANNER_HPP

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/common/concepts.hpp"
#include "munch/dfa/simulator.hpp"
#include "munch/dfa/split_window.hpp"

namespace munch::core
{
/**
 * @brief The window search behind Lexer::chunk_boundaries_with_windows() and next_certified_evidence(): where in an
 *        input a certified split window first licenses a cut.
 *
 * One planner lives for one plan and holds that plan's state alone, the memo and the barren offset; the token set
 * is handed in with each question, so the planner owns nothing it could outlive. It memoizes every
 * window decision by byte string, so the decisions, the costly part, are bounded by the distinct windows tried
 * while the positional walks scale with the positions examined; and when the token set proved a mandatory core it
 * searches only where the core occurs, tightening the barren offset across calls so that targets falling in a
 * tail already proved occurrence-free refuse without rescanning it. Byte certificates are not its business: the
 * callers decide when windows are consulted at all.
 */
class Window_planner
{
public:
    /**
     * @brief The longest window the searches try, four bytes. A grammar needing longer windows degrades to fewer
     *        chunks, never to an unsafe cut. The shortest tried is two, and that bound is not a guard: the planners
     *        consult windows only when no exact byte certifies and the set is not nullable, where the length-one
     *        equivalence theorem makes every one-byte window refuse, so skipping length one is provably inert rather
     *        than something a test could pin.
     */
    static constexpr std::size_t longest_window{4};

    /**
     * @brief The first window beginning at a position that certifies: lengths ascending from two, first certificate
     *        wins.
     * @tparam Iterator Random access iterator type.
     * @param simulator The token set the plan is for.
     * @param begin Iterator to the beginning of the input.
     * @param size The input's size.
     * @param at The position the window begins at.
     * @return The certified origin and the window's length, or std::nullopt when no window there certifies.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> window_at(
            const dfa::Simulator& simulator, Iterator begin, const std::size_t size, const std::size_t at)
    {
        const auto limit{std::min(longest_window, size - at)};

        for (std::size_t length{2}; length <= limit; ++length)
        {
            if (const auto origin{certified_origin(simulator, begin, at, length)})
            {
                return std::pair{*origin, length};
            }
        }

        return std::nullopt;
    }

    /**
     * @brief The first cut a certified window licenses at or after a floor: the occurrence plus the certified
     *        origin, in the exhaustive walk's position-then-length order whether or not a core filters the walk.
     * @tparam Iterator Random access iterator type.
     * @param simulator The token set the plan is for.
     * @param begin Iterator to the beginning of the input.
     * @param size The input's size.
     * @param floor The position the search starts at.
     * @return The cut, or std::nullopt when no window at or after the floor certifies.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::optional<std::size_t> cut(
            const dfa::Simulator& simulator, Iterator begin, const std::size_t size, const std::size_t floor)
    {
        return simulator.mandatory_core().empty() ? exhaustive(simulator, begin, size, floor) :
                                                    at_core(simulator, begin, size, floor);
    }

private:
    /**
     * @brief One decision per distinct byte string per plan.
     */
    using Memo_t = std::map<std::string, std::optional<std::size_t>, std::less<>>;

    /**
     * @brief One input element read as the scanners read it, through unsigned char, so every byte-domain element
     *        type forms the same memo key; the string constructor's implicit conversion would reject std::byte.
     * @tparam Iterator Random access iterator type.
     * @param begin Iterator to the beginning of the input.
     * @param at The element's position.
     * @return The element as a char.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] static char byte(Iterator begin, const std::size_t at)
    {
        return static_cast<char>(static_cast<unsigned char>(begin[static_cast<std::ptrdiff_t>(at)]));
    }

    /**
     * @brief The memoized window decision at one occurrence: the certified origin, if any.
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
        std::string window;

        window.reserve(length);

        for (std::size_t offset{0}; offset < length; ++offset)
        {
            window.push_back(byte(begin, at + offset));
        }

        auto found{memo_.find(window)};

        if (found == memo_.end())
        {
            const auto verdict{dfa::is_split_window(simulator, window)};

            found = memo_.emplace(std::move(window), verdict).first;
        }

        return found->second;
    }

    /**
     * @brief The exhaustive search: every position from the floor, lengths ascending, first certificate wins.
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
        for (auto occurrence{floor}; occurrence + 2 <= size; ++occurrence)
        {
            if (const auto found{window_at(simulator, begin, size, occurrence)})
            {
                return occurrence + found->first;
            }
        }

        return std::nullopt;
    }

    /**
     * @brief The core-filtered search: every certifying window provably contains the core with a byte after it,
     *        so candidates exist only where the core occurs, and visiting them in the exhaustive walk's own
     *        position-then-length order gives that walk's plan, refusals included.
     *
     * Positions are still scanned one by one, but for a byte comparison each; windows are built and certified only
     * at occurrences. A proved core longer than the longest window minus its trailing byte admits no candidate at
     * all, so every target refuses at once, exactly as the exhaustive walk would conclude after scanning to the end
     * of the input.
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

        if (core.size() + 1 > longest_window || floor >= barren_)
        {
            return std::nullopt;
        }

        const auto matches{[&](const std::size_t at) {
            for (std::size_t offset{0}; offset < core.size(); ++offset)
            {
                if (byte(begin, at + offset) != core[offset])
                {
                    return false;
                }
            }

            return true;
        }};

        std::size_t cursor{floor};

        std::size_t latest{floor};

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

        std::vector<std::pair<std::size_t, std::size_t>> heap;

        // A window of length in (m, longest] starting at t holds the m-byte core occurring at c, plus a byte after
        // it, exactly when t lies in [c + m + 1 - length, c].
        const auto ingest{[&](const std::size_t at) {
            for (auto length{core.size() + 1}; length <= longest_window; ++length)
            {
                const auto lowest{at + core.size() + 1 > length ? at + core.size() + 1 - length : std::size_t{0}};

                for (auto t{std::max(floor, lowest)}; t <= at && t + length <= size; ++t)
                {
                    heap.emplace_back(t, length);

                    std::ranges::push_heap(heap, std::greater{});
                }
            }
        }};

        auto pending{next_occurrence()};

        std::optional<std::pair<std::size_t, std::size_t>> last;

        // A pop waits until no unread occurrence can still contribute a smaller pair, which holds once the next
        // occurrence starts past t + longest - m - 1; overlapping occurrences propose duplicate pairs, which pop
        // adjacently and are skipped.
        while (true)
        {
            while (pending && (heap.empty() || *pending <= heap.front().first + longest_window - core.size() - 1))
            {
                ingest(*pending);

                pending = next_occurrence();
            }

            if (heap.empty())
            {
                return std::nullopt;
            }

            std::ranges::pop_heap(heap, std::greater{});

            const auto candidate{heap.back()};

            heap.pop_back();

            if (last == std::optional{candidate})
            {
                continue;
            }

            last = candidate;

            if (const auto origin{certified_origin(simulator, begin, candidate.first, candidate.second)})
            {
                return candidate.first + *origin;
            }
        }
    }

    /**
     * @brief The window decisions made so far, by byte string.
     */
    Memo_t memo_;

    /**
     * @brief No core occurrence begins at or after this offset; a scan that drains the input tightens it.
     */
    std::size_t barren_{std::numeric_limits<std::size_t>::max()};
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_WINDOW_PLANNER_HPP
