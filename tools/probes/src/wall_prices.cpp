#include "munch/tools/probes/wall_prices.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/probes/assertions.hpp"
#include "munch/tools/probes/wall_carry.hpp"
#include "munch/tools/probes/wall_planning.hpp"
#include "munch/tools/probes/wall_table.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements wall_prices.hpp: the bit counts, the carried words and the witness search are private to this unit.

/**
 * @brief The flavor after a word, the word's bytes' permutations applied in order.
 * @param carry The carry.
 * @param flavor The flavor before the word.
 * @param word The word.
 * @return The flavor after it.
 */
int carry_word(const Carry& carry, int flavor, const std::string_view word)
{
    for (const auto letter : word)
    {
        flavor = carry.sigma[static_cast<unsigned char>(letter)][static_cast<std::size_t>(flavor)];
    }

    return flavor;
}

/**
 * @brief Every word over an alphabet up to a length, by ascending length and within one length in the alphabet's order.
 * @param alphabet The alphabet.
 * @param longest The longest length.
 * @return The words, the empty word first.
 */
std::vector<std::string> words_up_to(const std::string& alphabet, const std::size_t longest)
{
    std::vector<std::string> words{""};

    for (std::size_t from{0}, length{0}; length < longest; ++length)
    {
        const auto until{words.size()};

        for (; from < until; ++from)
        {
            for (const auto letter : alphabet)
            {
                words.push_back(words[from] + letter);
            }
        }
    }

    return words;
}

/**
 * @brief A hazard witness for an ordered flavor pair: an input carrying the true flavor at an occurrence whose
 *        assumed-flavor certificate cuts off the serial segmentation.
 */
struct Witness
{
    /**
     * @brief The input, prefix, window and suffix.
     */
    std::string input{};

    /**
     * @brief The cut the assumed-flavor certificate licenses.
     */
    std::size_t cut{};
};

/**
 * @brief Searches the first hazard witness for an ordered flavor pair, windows of two or three bytes outermost, then
 *        prefixes of up to six bytes whose carry is the true flavor, then suffixes of up to two bytes that end the scan
 *        accepting, each in words_up_to's order.
 * @param table The table.
 * @param carry The table's carry.
 * @param reentrant Whether some transition enters the initial state.
 * @param alphabet The bytes the words are built from.
 * @param truth The true flavor.
 * @param assumed The assumed flavor.
 * @return The first witness, std::nullopt when none exists within the bounds.
 */
std::optional<Witness> find_witness(
        const Table& table, const Carry& carry, const bool reentrant, const std::string& alphabet, const int truth,
        const int assumed)
{
    const auto windows{words_up_to(alphabet, 3)};

    const auto prefixes{words_up_to(alphabet, 6)};

    const auto suffixes{words_up_to(alphabet, 2)};

    for (const auto& window : windows)
    {
        if (window.size() < 2)
        {
            continue;
        }

        const auto origin{window_walk(table, carry, reentrant, window, assumed)};

        if (!origin)
        {
            continue;
        }

        for (const auto& prefix : prefixes)
        {
            if (carry_word(carry, carry.seed, prefix) != truth)
            {
                continue;
            }

            const auto entered{scan_word(table, table.init, prefix)};

            if (!entered)
            {
                continue;
            }

            const auto crossed{scan_word(table, *entered, window)};

            if (!crossed)
            {
                continue;
            }

            for (const auto& suffix : suffixes)
            {
                const auto finished{scan_word(table, *crossed, suffix)};

                if (!finished || table.accept[*finished] == 0)
                {
                    continue;
                }

                const auto input{prefix + window + suffix};

                const auto cut{prefix.size() + *origin};

                if (cut == 0 || cut >= input.size())
                {
                    continue;
                }

                const auto serial{serial_boundaries(table, input)};

                if (!serial)
                {
                    continue;
                }

                if (!std::ranges::binary_search(*serial, cut))
                {
                    return Witness{.input = input, .cut = cut};
                }
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief The bits that name one of a number of values.
 * @param values The number of values.
 * @return The least b with 2^b at least the number.
 */
std::size_t bits_for(const std::size_t values)
{
    std::size_t bits{0};

    while ((std::size_t{1} << bits) < values)
    {
        ++bits;
    }

    return bits;
}

} // namespace

Prices price(
        Assertions& assertions, const std::string& name, const Table& table, const Carry& carry,
        const std::string& alphabet)
{
    const auto reentrant{is_init_reentrant(table)};

    // The seed's orbit under the group; transitivity means every flavor occurs as a true prefix carry.
    std::set<int> orbit{};

    for (const auto& element : carry.group)
    {
        orbit.insert(element[static_cast<std::size_t>(carry.seed)]);
    }

    const auto width{carry.group.begin()->size()};

    assertions.expect(orbit.size() == width, name + ": the seed's orbit does not reach every flavor");

    // Faithfulness holds by construction: group elements are stored as permutations, so distinct elements differ on
    // some flavor. What the witnesses below establish is a hazard relation: conditioning on the wrong flavor licenses a
    // cut off the serial segmentation. The three prices bind three distinct services, named exactly: the orbit prices
    // the conditioned flavor choice at a position; the group prices composable flavor transfer, owed only by a service
    // required to compose arbitrary factors; the semigroup prices exact kernel transfer, a stronger service the cut
    // machinery never needs. None of the three binds every scheme providing the same cuts: a serial flavor prepass
    // realizes them without composing anything, and rescanning the raw prefix from the initial state at each query
    // carries zero bits, paying work instead. A scheme-wide bit bound needs an explicit one-pass compositional
    // interface and common-context fooling pairs, which live with the width program's summary model, not here.

    Prices prices{
            .orbit = orbit.size(),
            .positional = bits_for(orbit.size()),
            .compositional = bits_for(carry.group.size()),
            .summary = bits_for(carry.semigroup)};

    for (const auto truth : orbit)
    {
        for (const auto assumed : orbit)
        {
            if (truth == assumed)
            {
                continue;
            }

            const auto witness{find_witness(table, carry, reentrant, alphabet, truth, assumed)};

            assertions.expect(
                    witness.has_value(), name + ": no witness separates flavors " + std::to_string(truth) + " and " +
                                                 std::to_string(assumed));

            if (witness)
            {
                ++prices.witnesses;

                const auto serial{serial_boundaries(table, witness->input)};

                assertions.expect(
                        serial && !std::ranges::binary_search(*serial, witness->cut),
                        name + ": a recorded witness cut lies on the serial segmentation after all");
            }
        }
    }

    std::cout << name << ": orbit " << prices.orbit << ", positional bits " << prices.positional
              << ", compositional bits " << prices.compositional << " (group " << carry.group.size()
              << "), summary bits " << prices.summary << " (semigroup " << carry.semigroup << "), witnesses "
              << prices.witnesses << "\n";

    return prices;
}

} // namespace munch::tools::probes
