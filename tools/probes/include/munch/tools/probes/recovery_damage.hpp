#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_DAMAGE_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_DAMAGE_HPP

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/tools/probes/recovery_lcg.hpp"

/**
 * @brief One damaged input of the recovery study and the map from pristine to damaged coordinates, Op, Damage, damage,
 *        shifted, is_landed and first_true_boundary, with the serial scan's failure offset, the Token_starts of a scan
 *        from an offset, token_starts, and a pristine corpus's boundaries.
 */
namespace munch::tools::probes
{
/**
 * @brief The tokens a scan from an offset emitted, as absolute starts, and the bytes it consumed.
 */
struct Token_starts
{
    /**
     * @brief The offsets the emitted tokens begin at, ascending.
     */
    std::vector<std::size_t> starts{};

    /**
     * @brief The bytes the scan consumed before it ended or stopped.
     */
    std::size_t consumed{0};
};

/**
 * @brief A damage operation on k bytes at a position.
 */
enum class Op : std::size_t
{
    /**
     * @brief The k bytes are replaced by pseudo-random ones.
     */
    substitution,

    /**
     * @brief The k bytes are removed.
     */
    deletion,

    /**
     * @brief K pseudo-random bytes are inserted before the position.
     */
    insertion
};

/**
 * @brief The operations every row is damaged by, in the order its tables print them.
 */
inline constexpr std::array ops{Op::substitution, Op::deletion, Op::insertion};

/**
 * @brief The damage widths k, in the order its tables print them.
 */
inline constexpr std::array<std::size_t, 3> widths{1, 4, 16};

/**
 * @brief One damaged input beside the coordinate map its operation induces: a pristine boundary below low keeps its
 *        offset, one in [low, cut) has no image, and one at or past cut maps to itself plus shift.
 */
struct Damage
{
    /**
     * @brief The damaged input.
     */
    std::string input{};

    /**
     * @brief The corruption end: the first damaged offset from which the input equals the pristine suffix.
     */
    std::size_t end{0};

    /**
     * @brief Added to a pristine boundary at or past cut to give its damaged image.
     */
    std::ptrdiff_t shift{0};

    /**
     * @brief The damage position: pristine boundaries below it keep their offset.
     */
    std::size_t low{0};

    /**
     * @brief The first pristine offset past the damaged window, from which boundaries map through the shift.
     */
    std::size_t cut{0};
};

/**
 * @brief Scans an input serially with maximal munch until it ends or no token matches.
 * @param lexer The row's lexer.
 * @param input The input.
 * @return The offset the scan stopped at, the input's size when it tokenizes completely.
 */
[[nodiscard]] std::size_t failure_offset(const core::Lexer& lexer, std::string_view input);

/**
 * @brief Scans an input from an offset with maximal munch until it ends or no token matches, noting where each token
 *        begins.
 * @param lexer The row's lexer.
 * @param input The input.
 * @param base The offset the scan starts at, at most the input's size.
 * @return The emitted tokens' starts, as offsets into the whole input, and the bytes consumed from the base.
 */
[[nodiscard]] Token_starts token_starts(const core::Lexer& lexer, std::string_view input, std::size_t base);

/**
 * @brief Returns the boundary set of a completely tokenizable input: every offset a token of its segmentation begins
 *        at. An input that does not tokenize completely ends the program with exit status one after printing `corpus
 *        not completely tokenizable: <consumed> of <size>` on standard error.
 * @param lexer The row's lexer.
 * @param input The input.
 * @return The token starts, ascending.
 */
[[nodiscard]] std::vector<std::size_t> boundaries(const core::Lexer& lexer, std::string_view input);

/**
 * @brief Returns the operation's name, as the tables and the archive print it.
 * @param op The operation.
 * @return `substitute`, `delete` or `insert`.
 */
[[nodiscard]] std::string_view name(Op op);

/**
 * @brief Damages a pristine corpus by one operation on a width of bytes at a position.
 * @param pristine The pristine corpus, at least position + width bytes long.
 * @param op The operation.
 * @param position The damage position p.
 * @param width The damage width k.
 * @param random The payload stream, drawn width times for a substitution or an insertion and not at all for a deletion.
 * @return The damaged input and its coordinate map.
 */
[[nodiscard]] Damage damage(const std::string& pristine, Op op, std::size_t position, std::size_t width, Lcg& random);

/**
 * @brief Moves an offset by a signed shift, the coordinate map's step between pristine and damaged offsets.
 * @param at The offset.
 * @param shift The signed shift, never taking the offset below zero.
 * @return The shifted offset.
 */
[[nodiscard]] std::size_t shifted(std::size_t at, std::ptrdiff_t shift);

/**
 * @brief Returns whether a damaged position is the image of a pristine boundary outside the damaged window, which is
 *        what the study counts as landed.
 * @param pristine The pristine boundaries, ascending.
 * @param damaged The damaged input's coordinate map.
 * @param at The damaged position.
 * @return True when the position lies below low and is a pristine boundary, or maps back to a pristine boundary at or
 *         past cut.
 */
[[nodiscard]] bool is_landed(const std::vector<std::size_t>& pristine, const Damage& damaged, std::size_t at);

/**
 * @brief Returns the first image of a pristine boundary at or past the corruption end.
 * @param pristine The pristine boundaries, ascending.
 * @param damaged The damaged input's coordinate map.
 * @return The image, std::nullopt when no pristine boundary lies past the damaged window.
 */
[[nodiscard]] std::optional<std::size_t> first_true_boundary(
        const std::vector<std::size_t>& pristine, const Damage& damaged);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_DAMAGE_HPP
