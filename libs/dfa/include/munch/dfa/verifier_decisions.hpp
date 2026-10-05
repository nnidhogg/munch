#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_VERIFIER_DECISIONS_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_VERIFIER_DECISIONS_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "munch/dfa/verifier.hpp"

/**
 * @file
 * @brief The four decisions over a verifier: miscovering(), boundary_gap(), realizable() and divergence().
 *
 * Each is a product with the verifier and a search on the product, and each witness is replayable through
 * Verifier::step().
 */
namespace munch::dfa
{
/**
 * @brief An input with its boundary marking, one bit per byte saying whether a boundary follows it.
 */
struct Marked_string
{
    /**
     * @brief The bytes.
     */
    std::string bytes{};

    /**
     * @brief Whether a boundary follows each byte, as many bits as bytes.
     */
    std::vector<bool> boundaries{};
};

/**
 * @brief An accepted marked string with an occurrence of a window whose covering boundary is not the origin, and that
 *        occurrence.
 */
struct Miscovering
{
    /**
     * @brief The accepted marked string, the shortest one.
     */
    Marked_string segmentation{};

    /**
     * @brief The index of the window's first byte at an occurrence whose covering boundary is not the origin.
     */
    std::size_t occurrence{};
};

/**
 * @brief Three marked strings such that stem, then loop repeated any number of times, then suffix is accepted, the loop
 *        carrying no boundary.
 */
struct Lasso
{
    /**
     * @brief The marked string from the start to the loop.
     */
    Marked_string stem{};

    /**
     * @brief The markless cycle, nonempty.
     */
    Marked_string loop{};

    /**
     * @brief The marked string from the loop to an accepting state.
     */
    Marked_string suffix{};
};

/**
 * @brief The supremum of the distance between consecutive boundaries over the accepted marked strings, or the lasso
 *        whose pumping makes it unbounded.
 */
using Boundary_gap = std::variant<std::size_t, Lasso>;

/**
 * @brief Per state, the marked symbols whose steps keep completion possible, in ascending order.
 */
using Mask = std::vector<std::vector<Marked>>;

/**
 * @brief The half of full equivalence a diverging input falls in.
 */
enum class Half : std::size_t
{
    /**
     * @brief Both verifiers accept the input under markings that differ.
     */
    boundary,

    /**
     * @brief Exactly one verifier accepts the input under some marking.
     */
    domain
};

/**
 * @brief A text on which two verifiers diverge, and the half it diverges in.
 */
struct Divergence
{
    /**
     * @brief The half the witness falls in.
     */
    Half half{};

    /**
     * @brief The diverging input, the shortest one of its half, marked as the first verifier accepts it for the
     *        boundary half and as the accepting verifier accepts it for the domain half.
     */
    Marked_string witness{};
};

/**
 * @brief Decides the window certificate (window, origin): the token containing an occurrence's final byte begins at the
 *        occurrence's byte origin, at every occurrence in every accepted marked string.
 *
 * A breadth-first search over the product of the verifier with the window's matching progress, the boundary marks of
 * the last bytes read and a sticky flag raised at an occurrence whose covering boundary is elsewhere; the first
 * accepting product state with the flag raised gives the shortest miscovering.
 * @param verifier The verifier.
 * @param window The window, nonempty.
 * @param origin The certified origin, a byte index of the window.
 * @return std::nullopt when the certificate holds, otherwise the shortest accepted marked string with an occurrence
 *         whose covering boundary is not the origin.
 * @throws std::invalid_argument If the window is empty or the origin lies outside it.
 */
[[nodiscard]] std::optional<Miscovering> miscovering(
        const Verifier& verifier, std::string_view window, std::size_t origin);

/**
 * @brief Decides whether the distance between consecutive boundaries is bounded over the accepted marked strings.
 *
 * The distance counts the bytes of a segment, the final one closed by the end of the input. A markless cycle gives the
 * lasso: the shortest stem from the start to a state on it, the cycle from that state, and the shortest suffix from it
 * to an accepting state. Without one the markless steps form an acyclic graph, and the supremum is the longest markless
 * path, one more when a marked step leaves its end and as is when its end accepts.
 * @param verifier The verifier, of a nonempty domain.
 * @return The supremum, or the lasso when the distance is unbounded.
 * @throws std::invalid_argument If the domain is empty.
 */
[[nodiscard]] Boundary_gap boundary_gap(const Verifier& verifier);

/**
 * @brief Decides whether generation restricted to completion-preserving steps never strands, and returns the mask.
 *
 * Every state of a trim verifier can complete but the start of an empty domain, so the mask admits every transition,
 * and from every state generation can stop at an accepting state or take an admitted step.
 * @param verifier The verifier.
 * @return std::nullopt when the domain is empty, otherwise each state's admitted steps.
 */
[[nodiscard]] std::optional<Mask> realizable(const Verifier& verifier);

/**
 * @brief Decides whether two verifiers accept the same marked strings, and returns a text they diverge on when not.
 *
 * The boundary half first, a breadth-first search over the product of the two verifiers reading one byte of the
 * alphabets' intersection under a marking each, latching once the markings differ; then the domain half, a
 * breadth-first search over the subset construction of each verifier over the union of the alphabets, the empty input
 * first. A verifier's alphabet is the bytes on its transitions. Both verifiers are functional, accepting one marking
 * per input of their domain, as armed_run() builds them and a caller's table must be; the two halves then decide
 * equality of the accepted marked languages.
 * @param a The first verifier, functional.
 * @param b The second verifier, functional.
 * @return std::nullopt when the two are equivalent, otherwise the shortest witness of the first half that has one.
 */
[[nodiscard]] std::optional<Divergence> divergence(const Verifier& a, const Verifier& b);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_VERIFIER_DECISIONS_HPP
