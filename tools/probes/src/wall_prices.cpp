#include "munch/tools/probes/wall_prices.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <format>
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
/**
 * @brief The shortest window a hazard witness tries.
 */
constexpr std::size_t shortest_window{2};

/**
 * @brief The longest window a hazard witness tries.
 */
constexpr std::size_t longest_window{3};

/**
 * @brief The longest prefix a hazard witness tries, carrying the true flavor.
 */
constexpr std::size_t longest_prefix{6};

/**
 * @brief The longest suffix a hazard witness tries.
 */
constexpr std::size_t longest_suffix{2};

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
 * @brief Returns the flavor after a word, the word's bytes' permutations applied in order.
 * @param carry The carry.
 * @param flavor The flavor before the word.
 * @param word The word.
 * @return The flavor after it.
 */
int carry_word(const Carry& carry, int flavor, const std::string_view word)
{
    for (const auto letter : word)
    {
        const auto byte{static_cast<unsigned char>(letter)};

        flavor = carry.sigma[byte][static_cast<std::size_t>(flavor)];
    }

    return flavor;
}

/**
 * @brief Returns every word over an alphabet up to a length, by ascending length and within one length in the
 *        alphabet's order.
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
    const auto windows{words_up_to(alphabet, longest_window)};

    const auto prefixes{words_up_to(alphabet, longest_prefix)};

    const auto suffixes{words_up_to(alphabet, longest_suffix)};

    const auto completed{
            [&](const std::string& prefix, const std::string& window, const std::size_t origin,
                const std::size_t crossed) -> std::optional<Witness> {
                for (const auto& suffix : suffixes)
                {
                    const auto finished{scan_word(table, crossed, suffix)};

                    if (!finished || table.accept[*finished] == Flag::off)
                    {
                        continue;
                    }

                    const auto input{prefix + window + suffix};

                    const auto cut{prefix.size() + origin};

                    if (cut == 0 || cut >= input.size())
                    {
                        continue;
                    }

                    const auto serial{serial_boundaries(table, input)};

                    if (serial && !std::ranges::binary_search(*serial, cut))
                    {
                        return Witness{.input = input, .cut = cut};
                    }
                }

                return std::nullopt;
            }};

    for (const auto& window : windows)
    {
        if (window.size() < shortest_window)
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

            if (auto witness{completed(prefix, window, *origin, *crossed)})
            {
                return witness;
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief Returns the bits that name one of a number of values.
 * @param values The number of values.
 * @return The least b with 2^b at least the number, 0 for no values.
 */
std::size_t bits_for(const std::size_t values)
{
    if (values == 0)
    {
        return 0;
    }

    return static_cast<std::size_t>(std::bit_width(values - 1));
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

    assertions.expect(
            orbit.size() == carry.width, std::format("{}: the seed's orbit does not reach every flavor", name));

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
                    witness.has_value(),
                    std::format("{}: no witness separates flavors {} and {}", name, truth, assumed));

            if (!witness)
            {
                continue;
            }

            ++prices.witnesses;

            const auto& [input, cut]{*witness};

            const auto serial{serial_boundaries(table, input)};

            assertions.expect(
                    serial && !std::ranges::binary_search(*serial, cut),
                    std::format("{}: a recorded witness cut lies on the serial segmentation after all", name));
        }
    }

    std::cout << name << ": orbit " << prices.orbit << ", positional bits " << prices.positional
              << ", compositional bits " << prices.compositional << " (group " << carry.group.size()
              << "), summary bits " << prices.summary << " (semigroup " << carry.semigroup << "), witnesses "
              << prices.witnesses << "\n";

    return prices;
}

} // namespace munch::tools::probes
