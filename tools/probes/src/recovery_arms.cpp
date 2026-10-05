#include "munch/tools/probes/recovery_arms.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/lexer.hpp"
#include "munch/tools/probes/recovery_damage.hpp"

namespace munch::tools::probes
{
namespace
{
using figures::Token;

/**
 * @brief What one scan from an offset found for the token-aware delimiter move.
 */
struct Sync_scan
{
    /**
     * @brief The offset one past the first synchronizing token the scan emitted, std::nullopt when it emitted none.
     */
    std::optional<std::size_t> sync{};

    /**
     * @brief The bytes the scan consumed before it stopped.
     */
    std::size_t consumed{0};
};

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
     * @brief The certified evidence behind the resume position, for a certified arm that answered.
     */
    std::optional<core::Lexer::Certified_start> evidence{};
};

/**
 * @brief Returns whether one emitted token synchronizes for a delimiter: for `;` the semicolon token exactly, otherwise
 *        an all-whitespace token containing the delimiter. A string or comment token that merely contains the delimiter
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

    if (!text.contains(delimiter))
    {
        return false;
    }

    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

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

    const auto rest{input.substr(at)};

    const auto note_synchronizer{[&sync, &scan, input, delimiter](const Token, const std::size_t length) {
        const auto token{input.substr(scan, length)};

        if (!sync && is_synchronizer(token, delimiter))
        {
            sync = scan + length;
        }

        scan += length;
    }};

    const auto consumed{lexer.tokenize_all<Token>(rest, note_synchronizer)};

    return Sync_scan{.sync = sync, .consumed = consumed};
}

/**
 * @brief Makes the token-aware delimiter move: scans from the search start, and while a scan emits no synchronizing
 *        token, skips the byte it stopped at and scans again.
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
        const auto [sync, consumed]{scan_to_synchronizer(lexer, input, at, delimiter)};

        if (sync)
        {
            return sync;
        }

        if (at + consumed >= input.size())
        {
            return std::nullopt;
        }

        at += consumed + 1;
    }

    return std::nullopt;
}

/**
 * @brief Returns the position of the next occurrence of the delimiter at or after an offset, the delimiter retained
 *        rather than consumed.
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
 * @brief Returns the position one past the next occurrence of the delimiter at or after an offset, the classical
 *        discard-through-the-delimiter convention; one past a final delimiter is the end-of-input offset.
 * @param input The damaged input.
 * @param from The search start.
 * @param delimiter The delimiter.
 * @return One past the occurrence, std::nullopt when none follows.
 */
std::optional<std::size_t> past_next(const std::string_view input, const std::size_t from, const char delimiter)
{
    const auto at{at_next(input, from, delimiter)};

    const auto one_past{[](const std::size_t occurrence) { return occurrence + 1; }};

    return at.transform(one_past);
}

/**
 * @brief Makes the anchored move: the first anchor from the search start at which core::Lexer::next_anchored_start()
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
        const auto tail{input.substr(anchor)};

        if (const auto found{lexer.next_anchored_start(tail, 0)})
        {
            return anchor + *found;
        }
    }

    return std::nullopt;
}

/**
 * @brief Makes one move of an arm from a search start.
 * @param lexer The row's lexer.
 * @param input The damaged input.
 * @param start The search start, below the input's size.
 * @param arm The arm.
 * @return The resume position, with its evidence for a certified arm.
 */
Move move_of(const core::Lexer& lexer, const std::string_view input, const std::size_t start, const Arm& arm)
{
    switch (arm.kind)
    {
    case Kind::certified:
    {
        const auto evidence{lexer.next_certified_evidence(input, start)};

        const auto start_of{[](const core::Lexer::Certified_start& found) {
            const auto& [certified_start, evidence_begin, evidence_end, window]{found};

            return certified_start;
        }};

        const auto resume{evidence.transform(start_of)};

        return Move{.resume = resume, .evidence = evidence};
    }

    case Kind::exact:
        return Move{.resume = exact_resume(lexer, input, start)};

    case Kind::skip:
        return Move{.resume = start};

    case Kind::delim:
    {
        const auto resume{arm.past ? past_next(input, start, arm.delimiter) : at_next(input, start, arm.delimiter)};

        return Move{.resume = resume};
    }

    case Kind::token_delim:
        return Move{.resume = token_sync(lexer, input, start, arm.delimiter)};
    }

    std::unreachable();
}

} // namespace

std::string_view outcome_name(const Outcome outcome)
{
    switch (outcome)
    {
    case Outcome::completed:
        return "completed";

    case Outcome::refused:
        return "refused";

    case Outcome::capped:
        return "capped";
    }

    std::unreachable();
}

std::size_t search_start(const Arm& arm, const std::size_t failure, const std::size_t clean_floor)
{
    return arm.clean ? std::max(clean_floor, failure + 1) : failure + 1;
}

Incident run_incident(
        const core::Lexer& lexer, const std::string_view input, const std::size_t failure,
        const std::size_t clean_floor, const Arm& arm, const std::size_t budget)
{
    Incident incident{};

    std::size_t fail{failure};

    while (true)
    {
        const auto start{search_start(arm, fail, clean_floor)};

        if (start >= input.size())
        {
            incident.terminal = input.size();

            incident.outcome = Outcome::completed;

            return incident;
        }

        const auto [resume, evidence]{move_of(lexer, input, start, arm)};

        if (!resume)
        {
            incident.outcome = Outcome::refused;

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
            const auto& [certified_start, evidence_begin, evidence_end, window]{*evidence};

            incident.moves.push_back(
                    Move_record{.answer = *resume, .evidence_begin = evidence_begin, .evidence_end = evidence_end});
        }

        incident.terminal = *resume;

        if (*resume >= input.size())
        {
            incident.outcome = Outcome::completed;

            return incident;
        }

        const auto [segment, consumed]{token_starts(lexer, input, *resume)};

        incident.starts.insert(incident.starts.end(), segment.begin(), segment.end());

        if (*resume + consumed == input.size())
        {
            incident.outcome = Outcome::completed;

            return incident;
        }

        fail = *resume + consumed;

        if (incident.attempts >= budget)
        {
            incident.outcome = Outcome::capped;

            return incident;
        }
    }
}

} // namespace munch::tools::probes
