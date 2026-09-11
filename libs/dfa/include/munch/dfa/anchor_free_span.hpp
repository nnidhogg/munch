#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_ANCHOR_FREE_SPAN_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_ANCHOR_FREE_SPAN_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "munch/dfa/simulator.hpp"

namespace munch::dfa
{
/**
 * @brief The longest run of positions a completely tokenizable input can carry with no certified byte among
 *        them, or nothing when such runs are unbounded.
 *
 * What a planner starves on. Certified bytes are where chunk_boundaries() may cut, so this is the worst gap it
 * can be asked to span: an input exists carrying a stretch this long with nowhere to cut inside it, and none
 * carrying a longer one. Unbounded is the common answer and is not a defect, only the statement that no finite
 * chunk count is guaranteed for every input.
 *
 * Decided over the same guessed markings the other decisions use, so the stretches counted are the ones a
 * maximal-munch scan actually produces rather than every marking the tables admit. A state from which no input
 * can be completed is dropped first, since a stretch that never finishes is not a stretch of any input, and a
 * cycle of uncertified positions among the states that remain is exactly an unbounded answer.
 *
 * The inventory is the simulator's certified bytes. The overload taking windows answers the same question for a
 * caller planning with those instead, and this one is its width-one case.
 * @param simulator The compiled token set.
 * @return The exact supremum, or std::nullopt when it is unbounded.
 */
[[nodiscard]] std::optional<std::size_t> anchor_free_span(const Simulator& simulator);

/**
 * @brief The same, over a supplied inventory of certified windows rather than over the certified bytes.
 *
 * A window anchors a position inside its occurrence, at the origin, rather than at the byte just read, so a
 * position's status is only settled once the rest of the window has arrived. The walk therefore carries the
 * last few bytes and a flag per position still waiting, and a position leaves that buffer anchored or not once
 * no window can still reach back to it. That is the whole difference from the byte case, which is this with a
 * buffer of nothing.
 *
 * Windows matter here because a grammar that certifies no byte can still certify windows, so this can return a
 * bound where the byte version cannot. It is a question about the supplied inventory: anchors outside it are not
 * counted, and supplying a pair the simulator does not certify is a caller error rather than a weaker answer.
 * An empty inventory asks how long an input with no anchor at all can be, which is unbounded as soon as any
 * token matches, since a match repeats, and zero when nothing does.
 * @param simulator The compiled token set.
 * @param inventory The certified windows and their origins, each refused by is_split_window() being an error.
 * @return The exact supremum, or std::nullopt when it is unbounded.
 * @throws std::invalid_argument If the inventory holds a window the simulator does not certify at the stated
 *         origin, or one longer than the buffer this walk can carry.
 */
[[nodiscard]] std::optional<std::size_t> anchor_free_span(
        const Simulator& simulator, std::span<const std::pair<std::string_view, std::size_t>> inventory);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_ANCHOR_FREE_SPAN_HPP
