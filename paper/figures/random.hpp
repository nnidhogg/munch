#ifndef MUNCH_PAPER_FIGURES_RANDOM_HPP
#define MUNCH_PAPER_FIGURES_RANDOM_HPP

/*
 * The fixed-seed stream the figure programs draw their corpora and token sets from, apart from grammars.hpp so that the
 * probes sharing those grammars do not need the benchmark's harness header.
 */

#include <cstddef>
#include <string>
#include <vector>

#include "munch/tools/benchmark/harness.hpp"

namespace figures
{
/**
 * @brief The seed every figure program's fixed 32-bit stream starts from.
 */
inline constexpr unsigned figure_seed{20260801U};

/**
 * @brief A deterministic 32-bit linear congruential stream, drawn through next_random().
 */
class Random
{
public:
    /**
     * @brief Starts the stream at a seed.
     * @param seed The initial state.
     */
    explicit Random(const unsigned seed) : seed_{seed} {}

    /**
     * @brief Advances the stream and draws a value below a bound from its high bits.
     * @param bound The exclusive bound.
     * @return The draw.
     */
    unsigned next(const unsigned bound)
    {
        return munch::tools::benchmark::next_random(seed_, munch::tools::benchmark::figure_dropped_bits) % bound;
    }

private:
    /**
     * @brief The current state.
     */
    unsigned seed_{0};
};

/**
 * @brief Draws documents of whole pieces from a stream started at figure_seed, so a corpus is identical across runs:
 *        each document a run of fewest to fewest + spread - 1 pieces, its length drawn first and then each piece.
 * @param pieces The pieces a document is drawn from.
 * @param count The documents drawn.
 * @param fewest The fewest pieces in a document.
 * @param spread How many piece counts a document's length is drawn from, starting at fewest.
 * @return The documents, in the order drawn.
 */
inline std::vector<std::string> drawn(
        const std::vector<std::string>& pieces, const std::size_t count, const unsigned fewest, const unsigned spread)
{
    Random random{figure_seed};

    std::vector<std::string> documents{};

    for (std::size_t index{0}; index < count; ++index)
    {
        std::string text{};

        for (auto remaining{fewest + random.next(spread)}; remaining > 0; --remaining)
        {
            const auto piece{random.next(static_cast<unsigned>(pieces.size()))};

            text += pieces[piece];
        }

        documents.push_back(text);
    }

    return documents;
}

} // namespace figures

#endif // MUNCH_PAPER_FIGURES_RANDOM_HPP
