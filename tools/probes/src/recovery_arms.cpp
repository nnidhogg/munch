#include "munch/tools/probes/recovery_arms.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "grammars.hpp"
#include "munch/core/lexer.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements recovery_arms.hpp: the move each kind makes and the resumed scan are private to this unit.

using figures::Token;

/**
 * @brief Whether one emitted token synchronizes for a delimiter: for `;` the semicolon token exactly, otherwise an
 *        all-whitespace token containing the delimiter. A string or comment token that merely contains the delimiter
 *        byte does not synchronize.
 * @param text The token's text.
 * @param delimiter The delimiter.
 * @return True when the token synchronizes.
 */
bool is_synchronizer(const std::string_view text, const char delimiter)
{
    if (delimiter == ';')
    {
        return text == ";";
    }

    if (text.find(delimiter) == std::string_view::npos)
    {
        return false;
    }

    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

/**
 * @brief What one scan from an offset found for the token-aware delimiter move.
 */
struct Sync_scan
{
    /**
     * @brief The offset one past the first synchronizing token the scan emitted, std::nullopt when it emitted none.
     */
    std::optional<std::size_t> sync;

    /**
     * @brief The bytes the scan consumed before it stopped.
     */
    std::size_t consumed{0};
};

/**
 * @brief Scans from an offset to where the scan stops, noting the end of the first synchronizing token it emits.
 * @param lexer The row's lexer.
 * @param input The damaged input.
 * @param at The offset the scan starts at, below the input's size.
 * @param delimiter The delimiter.
 * @return The first synchronizing token's end and the bytes consumed.
 */
Sync_scan scan_to_synchronizer(
        const core::Lexer& lexer, const std::string_view input, const std::size_t at, const char delimiter)
{
    std::optional<std::size_t> sync{};

    std::size_t scan{at};

    const auto consumed{lexer.tokenize_all<Token>(
            std::string_view{input.data() + at, input.size() - at}, [&](const Token, const std::size_t length) {
                if (!sync && is_synchronizer(std::string_view{input.data() + scan, length}, delimiter))
                {
                    sync = scan + length;
                }

                scan += length;
            })};

    return Sync_scan{.sync = sync, .consumed = consumed};
}

/**
 * @brief The token-aware delimiter move: scans from the search start, and while a scan emits no synchronizing token,
 *        skips the byte it stopped at and scans again.
 * @param lexer The row's lexer.
 * @param input The damaged input.
 * @param from The search start.
 * @param delimiter The delimiter.
 * @return One past the first synchronizing token, std::nullopt when a scan reaches the end of input without one.
 */
std::optional<std::size_t> token_sync(
        const core::Lexer& lexer, const std::string_view input, const std::size_t from, const char delimiter)
{
    std::size_t at{from};

    while (at < input.size())
    {
        const auto scanned{scan_to_synchronizer(lexer, input, at, delimiter)};

        if (scanned.sync)
        {
            return scanned.sync;
        }

        if (at + scanned.consumed >= input.size())
        {
            return std::nullopt;
        }

        at += scanned.consumed + 1;
    }

    return std::nullopt;
}

/**
 * @brief The position one past the next occurrence of the delimiter at or after an offset, the classical
 *        discard-through-the-delimiter convention; one past a final delimiter is the end-of-input offset.
 * @param input The damaged input.
 * @param from The search start.
 * @param delimiter The delimiter.
 * @return One past the occurrence, std::nullopt when none follows.
 */
std::optional<std::size_t> past_next(const std::string_view input, const std::size_t from, const char delimiter)
{
    const auto at{input.find(delimiter, from)};

    if (at == std::string_view::npos)
    {
        return std::nullopt;
    }

    return at + 1;
}

/**
 * @brief The position of the next occurrence of the delimiter at or after an offset, the delimiter retained rather
 *        than consumed.
 * @param input The damaged input.
 * @param from The search start.
 * @param delimiter The delimiter.
 * @return The occurrence, std::nullopt when none follows.
 */
std::optional<std::size_t> at_next(const std::string_view input, const std::size_t from, const char delimiter)
{
    const auto at{input.find(delimiter, from)};

    if (at == std::string_view::npos)
    {
        return std::nullopt;
    }

    return at;
}

/**
 * @brief The anchored move: the first anchor from the search start at which core::Lexer::next_anchored_start()
 *        answers, the anchor advancing one byte at a time.
 * @param lexer The row's lexer.
 * @param input The damaged input.
 * @param start The search start.
 * @return The anchor plus its answer, std::nullopt when no anchor answers.
 */
std::optional<std::size_t> exact_resume(const core::Lexer& lexer, const std::string_view input, const std::size_t start)
{
    for (std::size_t anchor{start}; anchor < input.size(); ++anchor)
    {
        if (const auto found{
                    lexer.next_anchored_start(std::string_view{input.data() + anchor, input.size() - anchor}, 0)})
        {
            return anchor + *found;
        }
    }

    return std::nullopt;
}

/**
 * @brief One move of an arm.
 */
struct Move
{
    /**
     * @brief The resume position, std::nullopt for a refusal.
     */
    std::optional<std::size_t> resume{};

    /**
     * @brief The certified evidence behind the resume position, for a Certified arm that answered.
     */
    std::optional<core::Lexer::Certified_start> evidence{};
};

/**
 * @brief Makes one move of an arm from a search start.
 * @param lexer The row's lexer.
 * @param input The damaged input.
 * @param start The search start, below the input's size.
 * @param arm The arm.
 * @return The resume position, with its evidence for a Certified arm.
 */
Move move_of(const core::Lexer& lexer, const std::string_view input, const std::size_t start, const Arm& arm)
{
    switch (arm.kind)
    {
    case Kind::Certified:
    {
        const auto evidence{lexer.next_certified_evidence(input, start)};

        return Move{.resume = evidence ? std::optional{evidence->start} : std::nullopt, .evidence = evidence};
    }

    case Kind::Exact:
        return Move{.resume = exact_resume(lexer, input, start)};

    case Kind::Skip:
        return Move{.resume = start};

    case Kind::Delim:
        return Move{.resume = arm.past ? past_next(input, start, arm.delimiter) : at_next(input, start, arm.delimiter)};

    default:
        return Move{.resume = token_sync(lexer, input, start, arm.delimiter)};
    }
}

/**
 * @brief Scans one resumed segment from an offset, appending every absolute token start.
 * @param lexer The row's lexer.
 * @param input The damaged input.
 * @param base The resume position.
 * @param starts The token starts appended to.
 * @return The bytes the scan consumed.
 */
std::size_t segment_starts(
        const core::Lexer& lexer, const std::string_view input, const std::size_t base,
        std::vector<std::size_t>& starts)
{
    std::size_t at{base};

    return lexer.tokenize_all<Token>(
            std::string_view{input.data() + base, input.size() - base}, [&](const Token, const std::size_t length) {
                starts.push_back(at);

                at += length;
            });
}

} // namespace

std::string_view outcome_name(const Outcome outcome)
{
    switch (outcome)
    {
    case Outcome::Completed:
        return "completed";

    case Outcome::Refused:
        return "refused";

    default:
        return "capped";
    }
}

Incident run_incident(
        const core::Lexer& lexer, const std::string_view input, const std::size_t failure,
        const std::size_t clean_floor, const Arm& arm, const std::size_t budget)
{
    Incident incident{};

    std::size_t fail{failure};

    while (true)
    {
        const auto start{arm.clean ? std::max(clean_floor, fail + 1) : fail + 1};

        if (start >= input.size())
        {
            incident.terminal = input.size();

            incident.outcome = Outcome::Completed;

            return incident;
        }

        const auto [resume, evidence]{move_of(lexer, input, start, arm)};

        if (!resume)
        {
            incident.outcome = Outcome::Refused;

            return incident;
        }

        ++incident.attempts;

        if (!incident.first)
        {
            incident.first = *resume;

            incident.evidence = evidence;
        }

        if (evidence)
        {
            incident.moves.push_back({*resume, evidence->evidence_begin, evidence->evidence_end});
        }

        incident.terminal = *resume;

        if (*resume >= input.size())
        {
            incident.outcome = Outcome::Completed;

            return incident;
        }

        const auto consumed{segment_starts(lexer, input, *resume, incident.starts)};

        if (*resume + consumed == input.size())
        {
            incident.outcome = Outcome::Completed;

            return incident;
        }

        fail = *resume + consumed;

        if (incident.attempts >= budget)
        {
            incident.outcome = Outcome::Capped;

            return incident;
        }
    }
}

} // namespace munch::tools::probes
