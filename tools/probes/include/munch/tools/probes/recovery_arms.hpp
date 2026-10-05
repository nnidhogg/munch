#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_ARMS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_ARMS_HPP

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "munch/core/lexer.hpp"

/**
 * @brief The recovery study's eleven arms and the driver they share, Kind, Arm, arms, attempt_budget, Outcome,
 *        Move_record, Incident, outcome_name, search_start and run_incident: each arm driven from the first failure to
 *        the end of input, a refusal, or the attempt budget, under one stopping rule.
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
    certified,

    /**
     * @brief The anchored procedure: core::Lexer::next_anchored_start() at the anchor, the anchor advancing one byte at
     *        a time past a tail beyond repair until a certificate holds.
     */
    exact,

    /**
     * @brief Resumes at the search start itself, the skip-one convention.
     */
    skip,

    /**
     * @brief Resumes at, or one past, the next occurrence of the delimiter byte.
     */
    delim,

    /**
     * @brief The token-aware delimiter search: skip bytes until the scan makes progress, discard emitted tokens through
     *        the first synchronizing token for the delimiter, and resume one past it.
     */
    token_delim
};

/**
 * @brief One evaluated arm.
 */
struct Arm
{
    /**
     * @brief The arm's name, as the tables and the archive print it.
     */
    std::string_view name{};

    /**
     * @brief The move the arm makes.
     */
    Kind kind{Kind::certified};

    /**
     * @brief The delimiter a delim or token_delim arm searches for.
     */
    char delimiter{'\0'};

    /**
     * @brief Whether a delim arm resumes one past the delimiter rather than at it.
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
inline constexpr std::array arms{
        Arm{.name = "certified", .kind = Kind::certified},
        Arm{.name = "certified-clean", .kind = Kind::certified, .clean = true},
        Arm{.name = "exact", .kind = Kind::exact},
        Arm{.name = "exact-clean", .kind = Kind::exact, .clean = true},
        Arm{.name = "skip-one", .kind = Kind::skip},
        Arm{.name = "newline", .kind = Kind::delim, .delimiter = '\n', .past = true},
        Arm{.name = "newline-at", .kind = Kind::delim, .delimiter = '\n'},
        Arm{.name = "semicolon", .kind = Kind::delim, .delimiter = ';', .past = true},
        Arm{.name = "semicolon-at", .kind = Kind::delim, .delimiter = ';'},
        Arm{.name = "token-newline", .kind = Kind::token_delim, .delimiter = '\n'},
        Arm{.name = "token-semicolon", .kind = Kind::token_delim, .delimiter = ';'},
};

/**
 * @brief The index in arms of the certificate walk searching from one past the failure.
 */
inline constexpr std::size_t certified_arm{0};

/**
 * @brief The index in arms of the certificate walk floored at the corruption end.
 */
inline constexpr std::size_t certified_clean_arm{1};

/**
 * @brief The index in arms of the anchored procedure searching from one past the failure.
 */
inline constexpr std::size_t exact_arm{2};

/**
 * @brief The index in arms of the anchored procedure floored at the corruption end.
 */
inline constexpr std::size_t exact_clean_arm{3};

/**
 * @brief The index in arms of the arm resuming one past the next newline.
 */
inline constexpr std::size_t newline_arm{5};

/**
 * @brief The index in arms of the arm resuming at the next newline.
 */
inline constexpr std::size_t newline_at_arm{6};

/**
 * @brief The index in arms of the arm resuming one past the next semicolon.
 */
inline constexpr std::size_t semicolon_arm{7};

/**
 * @brief The index in arms of the arm resuming at the next semicolon.
 */
inline constexpr std::size_t semicolon_at_arm{8};

/**
 * @brief The recovery moves an incident may make before it ends as capped.
 */
inline constexpr std::size_t attempt_budget{100};

/**
 * @brief How an incident ended.
 */
enum class Outcome : std::size_t
{
    /**
     * @brief A resumed scan reached the end of input, or a search start or a resume position lay at or past it.
     */
    completed,

    /**
     * @brief The arm's move found no resume position.
     */
    refused,

    /**
     * @brief The attempt budget ran out with the input unfinished.
     */
    capped
};

/**
 * @brief One certified move of an incident: where it resumed and the evidence behind it.
 */
struct Move_record
{
    /**
     * @brief The resume position the move answered.
     */
    std::size_t answer{0};

    /**
     * @brief The first byte of the move's certified evidence.
     */
    std::size_t evidence_begin{0};

    /**
     * @brief One past the last byte of the move's certified evidence.
     */
    std::size_t evidence_end{0};
};

/**
 * @brief One arm's completed incident: every move from the first failure to the end of input, a refusal, or the attempt
 *        budget.
 */
struct Incident
{
    /**
     * @brief The first move's resume position, std::nullopt when the first move refused.
     */
    std::optional<std::size_t> first{};

    /**
     * @brief The first move's certified evidence, for a certified arm that answered.
     */
    std::optional<core::Lexer::Certified_start> evidence{};

    /**
     * @brief Every certified move's resume position, evidence begin and evidence end, in move order.
     */
    std::vector<Move_record> moves{};

    /**
     * @brief The last resume position, the input's size when a search start lay at or past its end; std::nullopt when
     *        the first move refused.
     */
    std::optional<std::size_t> terminal{};

    /**
     * @brief The moves that produced a resume position.
     */
    std::size_t attempts{0};

    /**
     * @brief How the incident ended.
     */
    Outcome outcome{Outcome::refused};

    /**
     * @brief The absolute start of every token the resumed scans emitted, in order.
     */
    std::vector<std::size_t> starts{};
};

/**
 * @brief Returns the outcome's name, as the archive prints it.
 * @param outcome The outcome.
 * @return `completed`, `refused` or `capped`.
 */
[[nodiscard]] std::string_view outcome_name(Outcome outcome);

/**
 * @brief Returns where an arm's search starts after a failure: one past the failure, or the clean floor when the arm is
 *        clean and that is later.
 * @param arm The arm.
 * @param failure The failure offset.
 * @param clean_floor The corruption end, the search floor of a clean arm.
 * @return The search start.
 */
[[nodiscard]] std::size_t search_start(const Arm& arm, std::size_t failure, std::size_t clean_floor);

/**
 * @brief Drives one arm through an incident: from one past the failure, or from the clean floor when the arm is clean
 *        and that is later, the arm moves to a resume position and the scan resumes there, until the scan reaches the
 *        end of input, a move refuses, or the budget of moves is spent. A search start at or past the end completes the
 *        incident with the input's size as its terminal position.
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
