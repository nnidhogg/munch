#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_ARMS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_ARMS_HPP

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "munch/core/lexer.hpp"

/**
 * @brief The recovery study's eleven arms and the driver they share, Kind, Arm, kArms, Outcome, Incident and
 *        run_incident: each arm driven from the first failure to the end of input, a refusal, or the attempt budget,
 *        under one stopping rule.
 */
namespace munch::tools::probes
{
/**
 * @brief The recovery move an arm makes.
 */
enum class Kind : std::size_t
{
    /**
     * @brief The certificate walk, core::Lexer::next_certified_evidence(): byte and window evidence in evidence order.
     */
    Certified,

    /**
     * @brief The anchored procedure: core::Lexer::next_anchored_start() at the anchor, the anchor advancing one byte
     *        at a time past a tail beyond repair until a certificate holds.
     */
    Exact,

    /**
     * @brief Resume at the search start itself, the skip-one convention.
     */
    Skip,

    /**
     * @brief Resume at, or one past, the next occurrence of the delimiter byte.
     */
    Delim,

    /**
     * @brief The token-aware delimiter search: skip bytes until the scan makes progress, discard emitted tokens
     *        through the first synchronizing token for the delimiter, and resume one past it.
     */
    TokenDelim,
};

/**
 * @brief One evaluated arm.
 */
struct Arm
{
    /**
     * @brief The arm's name, as the tables and the archive print it.
     */
    std::string_view name;

    /**
     * @brief The move the arm makes.
     */
    Kind kind;

    /**
     * @brief The delimiter a Delim or TokenDelim arm searches for.
     */
    char delimiter{'\0'};

    /**
     * @brief Whether a Delim arm resumes one past the delimiter rather than at it.
     */
    bool past{false};

    /**
     * @brief Whether the search is floored at the corruption end, which models a caller told the damage's extent;
     *        otherwise it starts one past the failure alone.
     */
    bool clean{false};
};

/**
 * @brief The eleven arms, in the order the tables and the archive print them.
 */
inline constexpr std::array<Arm, 11> kArms{
        Arm{.name = "certified", .kind = Kind::Certified},
        Arm{.name = "certified-clean", .kind = Kind::Certified, .clean = true},
        Arm{.name = "exact", .kind = Kind::Exact},
        Arm{.name = "exact-clean", .kind = Kind::Exact, .clean = true},
        Arm{.name = "skip-one", .kind = Kind::Skip},
        Arm{.name = "newline", .kind = Kind::Delim, .delimiter = '\n', .past = true},
        Arm{.name = "newline-at", .kind = Kind::Delim, .delimiter = '\n'},
        Arm{.name = "semicolon", .kind = Kind::Delim, .delimiter = ';', .past = true},
        Arm{.name = "semicolon-at", .kind = Kind::Delim, .delimiter = ';'},
        Arm{.name = "token-newline", .kind = Kind::TokenDelim, .delimiter = '\n'},
        Arm{.name = "token-semicolon", .kind = Kind::TokenDelim, .delimiter = ';'},
};

/**
 * @brief The index in kArms of the certificate walk searching from one past the failure.
 */
inline constexpr std::size_t kCertifiedArm{0};

/**
 * @brief The index in kArms of the certificate walk floored at the corruption end.
 */
inline constexpr std::size_t kCertifiedCleanArm{1};

/**
 * @brief The index in kArms of the anchored procedure searching from one past the failure.
 */
inline constexpr std::size_t kExactArm{2};

/**
 * @brief The index in kArms of the anchored procedure floored at the corruption end.
 */
inline constexpr std::size_t kExactCleanArm{3};

/**
 * @brief The recovery moves an incident may make before it ends as capped.
 */
inline constexpr std::size_t kAttemptBudget{100};

/**
 * @brief How an incident ended.
 */
enum class Outcome : std::size_t
{
    /**
     * @brief A resumed scan reached the end of input, or a search start or a resume position lay at or past it.
     */
    Completed,

    /**
     * @brief The arm's move found no resume position.
     */
    Refused,

    /**
     * @brief The attempt budget ran out with the input unfinished.
     */
    Capped,
};

/**
 * @brief The outcome's name, as the archive prints it.
 * @param outcome The outcome.
 * @return `completed`, `refused` or `capped`.
 */
[[nodiscard]] std::string_view outcome_name(Outcome outcome);

/**
 * @brief One arm's completed incident: every move from the first failure to the end of input, a refusal, or the
 *        attempt budget.
 */
struct Incident
{
    /**
     * @brief The first move's resume position, std::nullopt when the first move refused.
     */
    std::optional<std::size_t> first;

    /**
     * @brief The first move's certified evidence, for a Certified arm that answered.
     */
    std::optional<core::Lexer::Certified_start> evidence;

    /**
     * @brief Every certified move's resume position, evidence begin and evidence end, in move order.
     */
    std::vector<std::array<std::size_t, 3>> moves;

    /**
     * @brief The last resume position, the input's size when a search start lay at or past its end; std::nullopt
     *        when the first move refused.
     */
    std::optional<std::size_t> terminal;

    /**
     * @brief The moves that produced a resume position.
     */
    std::size_t attempts{0};

    /**
     * @brief How the incident ended.
     */
    Outcome outcome{Outcome::Refused};

    /**
     * @brief The absolute start of every token the resumed scans emitted, in order.
     */
    std::vector<std::size_t> starts;
};

/**
 * @brief Drives one arm through an incident: from one past the failure, or from the clean floor when the arm is clean
 *        and that is later, the arm moves to a resume position and the scan resumes there, until the scan reaches the
 *        end of input, a move refuses, or the budget of moves is spent. A search start at or past the end completes
 *        the incident with the input's size as its terminal position.
 * @param lexer The row's lexer.
 * @param input The damaged input.
 * @param failure The serial scan's failure offset.
 * @param clean_floor The corruption end, the search floor of a clean arm.
 * @param arm The arm.
 * @param budget The moves after which an unfinished incident ends as capped.
 * @return The incident.
 */
[[nodiscard]] Incident run_incident(
        const core::Lexer& lexer, std::string_view input, std::size_t failure, std::size_t clean_floor, const Arm& arm,
        std::size_t budget);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_ARMS_HPP
