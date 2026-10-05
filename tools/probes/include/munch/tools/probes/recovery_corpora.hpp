#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_CORPORA_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_CORPORA_HPP

#include <cstddef>
#include <string>

/**
 * @brief The recovery study's generated corpora, C_like_features, c_like_corpus and json_corpus: each drawn from a
 *        fixed seed, so every run generates the same bytes, and complete by construction for the rows that request it.
 */
namespace munch::tools::probes
{
/**
 * @brief The token families a C-like corpus draws besides identifiers, numbers, operators and punctuation.
 */
struct C_like_features
{
    /**
     * @brief Whether a piece may be a two-word string literal.
     */
    bool strings{false};

    /**
     * @brief Whether a code line may end in a one-word line comment.
     */
    bool line_comments{false};

    /**
     * @brief Whether a draw may be a block comment of several lines in place of a code line.
     */
    bool block_comments{false};
};

/**
 * @brief Generates a C-like corpus from the stream seeded 0x5EED0001. With block comments, one draw in three is a
 *        comment of one to four lines whose interior holds no `*`; every other line is three to eight pieces, each
 *        followed by a space: a number, an identifier, an operator and an identifier, an identifier and a semicolon, a
 *        parenthesized identifier, or with strings a two-word string literal. With line comments one code line in four
 *        ends in a one-word comment. The corpus is cut back to its last newline within the size and padded with
 *        newlines to it, so no literal or comment is left open.
 * @param bytes The corpus size, at least one.
 * @param features The token families drawn.
 * @return The corpus, exactly bytes long.
 */
[[nodiscard]] std::string c_like_corpus(std::size_t bytes, const C_like_features& features);

/**
 * @brief Generates a corpus of lexically valid JSON lines from the stream seeded 0x5EED0002: each line an object of one
 *        to four members whose values are literal names or numbers, two-word strings, or two-element arrays. The corpus
 *        is cut back to its last newline within the size and padded with newlines to it.
 * @param bytes The corpus size, at least one.
 * @return The corpus, exactly bytes long.
 */
[[nodiscard]] std::string json_corpus(std::size_t bytes);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_CORPORA_HPP
