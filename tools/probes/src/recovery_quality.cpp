// Measures the quality of certified error recovery against the classical panic-mode conventions, on the grammar
// rows the split-points study established, under a corruption model whose ground truth is exact by construction.
//
// The question. next_certified_start() returns positions with a soundness theorem: in every tokenizable repair of
// the input before an anchor, the image of a certified position begins a token. The library also ships the
// anchored-exact machinery, next_anchored_start() and minimal_repair(), exact where the walk is merely sound.
// The classical conventions, skip one byte, skip to a delimiter raw or token-aware, promise nothing. This probe
// drives every arm through completed incidents under one stopping rule and quantifies each against the same
// oracle: where the first answer lands, where the terminal one does, whether the incident completes, refuses, or
// exhausts its budget, how fast the resumed stream converges to the mapped pristine one, and how the walk's
// answers stratify by whether any repair exists at its anchor at all.
//
// Corruption model. A pristine corpus x, completely tokenizable by its row's grammar and asserted so, is damaged
// at a position p by one of three operations on k bytes: substitute k bytes with pseudo-random ones, delete k
// bytes, or insert k pseudo-random bytes. Every trial is deterministic: independently seeded linear congruential
// generators cover the corpus, the damage positions, and the damage payloads, so every run of this program
// performs the identical experiment.
//
// Ground truth. The token boundaries of the damaged input's own segmentation past the seam are unknowable without
// a repair oracle, so the study uses the seed note's definition: ground truth is the boundary set B of the
// pristine corpus, mapped into damaged coordinates. Each operation leaves a suffix of x intact, y[end..] equals
// x[c..] for the corruption end named below, and boundaries inside the damaged window have no image and are
// dropped:
//
//   substitute: y agrees with x outside [p, p+k); end = p + k, images shift by 0.
//   delete:     y[p..] equals x[p+k..);       end = p,     images past the cut shift by -k.
//   insert:     y[p+k..] equals x[p..);       end = p + k, images past the seam shift by +k.
//
// A resume position counts as landed when it is the image of a pristine boundary outside the damaged window. Near
// the seam the damaged input's true segmentation can genuinely diverge from the mapped pristine one; the same
// conservative oracle is applied uniformly to every strategy, though their near-seam exposure differs.
//
// The theorem gives teeth. The pristine corpus is itself a tokenizable repair of the damaged suffix: x equals
// x[0..c) concatenated with x[c..], the very suffix y preserves. So the repair-invariance theorems force any
// certified answer whose supporting occurrence lies wholly in the preserved suffix to map to a boundary of B.
// The occurrence begins at most three bytes before the answer, the longest window is four bytes with origin at
// most three, so every certified answer at or past end + 3 must land, and the harness asserts exactly that,
// failing the run on any violation. Answers in the seam band [end, end + 3) may rest on an occurrence straddling
// the seam, where the theorems bind only repairs that preserve the straddling evidence and predict nothing
// about the mapped-pristine oracle; they are measured, not asserted. A second hard assertion runs before any
// corruption: on the pristine corpus, every next_certified_start() answer from sampled offsets must be a boundary
// of B, the same oracle discipline the split-points report uses.
//
// Eleven arms share the completed-incident driver. certified is the evidence-order walk, its answers carrying
// the library's evidence interval, cross-checked on every first move against an independent replica of the walk,
// so a harness defect and a library defect cannot agree; every certified move's evidence is recorded and every
// covered move is asserted to land, not only the first per incident. certified-clean starts the walk at the
// corruption end or one past the failure, whichever is later, the oracle arm whose every answer is asserted
// covered and landed. exact is the anchored procedure over the library's complete-repair-invariance decider,
// the anchor advancing past a beyond-repair tail's poison until a certificate holds; the decider's direct
// answer at the blind anchor is archived separately per trial, and the cross-arm regressions test that direct
// call, never the advancing procedure. exact-clean anchors at the corruption end, its answers asserted to land
// since the pristine prefix is a repair of what precedes the preserved suffix. skip-one and the four raw
// delimiter placements are the classical conventions, the past placement repaired to return the end-of-input
// offset at a final delimiter rather than refusing. token-newline and token-semicolon are the token-aware
// reading a referee asked for, synchronizing on a designated token: the delimiter's own punctuation token
// exactly, or an all-whitespace token carrying the newline, so a string or comment that merely contains the
// delimiter byte never synchronizes.
//
// Repairability stratifies every trial: minimal_repair() at the blind anchor reports whether any completely
// tokenizable repair exists, every returned repair witness-verified by scanning repair plus tail to the end of
// input, and the summary counts the walk's answers on unrepairable tails apart. Two consistency regressions
// bind the routines at the blind anchor itself: on a repairable trial a walk answer implies a direct decider
// answer at or before it, and on an unrepairable trial the direct call must refuse; the two routines share
// their scenario machinery, so this is consistency, not independent proof. The sharper transfer assertion
// fires on the exact precondition: a certified answer whose evidence begins at or past the corruption end must
// land, asserted for every move; the conservative end-plus-three assertion stays beside it, and the summary
// reports covered and uncovered tallies with the nonminimality figure. The generated corpora are written
// beside the archive so every column recomputes from the archive alone.
//
// Metrics, per grammar row, operation, k, and arm, every (op, k, arm) cell pooled over independent seeds and the
// per-seed figures printed beside the pooled ones (damage the grammar absorbs is counted and set aside):
//
//   answers    incidents whose first move produced a resume position; refusals is its complement.
//   f-land     fraction of first answers that are images of pristine boundaries, as defined above.
//   t-land     fraction of interior terminal positions, the incident's last resume, that land.
//   complete   fraction of incidents that reached the end of input under the driver; capped counts incidents
//              that exhausted the attempt budget of one hundred moves.
//   attempts   mean recovery moves per incident.
//   conv       mean signed distance from the corruption end to where the resumed boundary stream and the
//              mapped pristine stream agree forever after, over completed incidents.
//   lost       mapped pristine boundaries from the corruption end to the convergence point, the initial
//              jump's skipped starts included, never recovered.
//   spur       emitted starts inside the divergence region that land on no mapped boundary, starts invented.
//   overshoot  signed distance from the first mapped boundary at or past the corruption end to the first answer.
//
// The summary closes with Wilson 95 percent intervals on pooled first landing and completion per arm, the
// repairability tallies with the vacuous share, the exact arm's byte savings on repairable trials beside its
// signed net displacement over all pairs, and the duplicate count of the rejection-sampled positions.
//
// Baseline conventions. All arms search from e + 1 after every failure, the same progress contract recover()
// keeps, so no arm may retry the offending byte; the oracle arms floor their search at the corruption end. The
// driver alternates scan and recover under one stopping rule for every arm: end of input, refusal, or budget.
//
// Non-claims. Certified recovery answers with the first certificate in walk order, not the closest boundary, and
// refuses where nothing certifies; both behaviors are measured here, not excused. Nothing is claimed about the
// damaged input's own segmentation between the failure and the resume position. Printed figures are quotable only
// beside the clean commit of the collection ritual, exactly as the benchmark's are.
//
// Usage. recovery_quality [corpus KiB] [trials per cell] [csv path] [real json corpus path] [seeds]
// Defaults are sized to run as a test; the campaign passes larger figures and archives the CSV. The optional
// fourth argument adds an ecological row: a real-world JSON document, read verbatim, held to the same complete
// tokenizability assertion, the same damage schedule, and the same oracle as the generated rows. The fifth is
// the number of independent seeds, three by default, each a fully separate schedule of positions and payloads.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"

namespace
{
using figures::Token;

/**
 * @brief Deterministic pseudo-random stream; one instance per independent purpose so streams never entangle.
 */
class Lcg
{
public:
    explicit Lcg(const std::uint32_t seed) : state_{seed} {}

    /**
     * @brief A full-width draw, the whole mixed state, so position sampling covers every offset of a span.
     *
     * The archived campaigns drew fifteen-bit values here, confining each cell's positions to a
     * multiplicatively spread lattice of 32,768 offsets, a disclosed limitation of those archives; this
     * widening postdates them and changes every future schedule.
     */
    std::uint32_t next()
    {
        state_ = state_ * 1664525U + 1013904223U;

        return state_ ^ (state_ >> 16U);
    }

    /**
     * @brief An unbiased draw from [0, span) by Lemire multiply-shift with rejection.
     */
    std::uint32_t bounded(const std::uint32_t span)
    {
        while (true)
        {
            const auto x{next()};

            const auto m{static_cast<std::uint64_t>(x) * span};

            if (static_cast<std::uint32_t>(m) >= span || static_cast<std::uint32_t>(m) >= (0U - span) % span)
            {
                return static_cast<std::uint32_t>(m >> 32U);
            }
        }
    }

    /**
     * @brief A byte with every value admitted, the damage model's alphabet.
     */
    char byte()
    {
        state_ = state_ * 1664525U + 1013904223U;

        return static_cast<char>((state_ >> 16U) & 0xffU);
    }

private:
    std::uint32_t state_;
};

/**
 * @brief A corpus for the C-like rows: identifier lines with numbers, operators, punctuation, and optionally
 *        string literals, line comments, or block comments, every line newline-terminated.
 *
 * The alphabet is restricted to what the requesting row tokenizes, and the caller asserts complete
 * tokenizability before any trial, so a generator slip fails loudly rather than skewing the study.
 */
std::string c_like_corpus(const std::size_t bytes, const bool strings, const bool comments, const bool blocks)
{
    std::string out;

    out.reserve(bytes + 128);

    Lcg random{0x5eed0001U};

    static constexpr std::string_view words[]{"count", "offset", "state", "token", "chunk", "origin", "table", "index"};

    static constexpr char ops[]{'+', '-', '*', '=', '<', '>', '&', '|'};

    while (out.size() < bytes)
    {
        // The block-comment row interleaves code lines with comments spanning several lines, at a density where
        // damage regularly lands inside one. That is the row's whole point: past a newline inside a comment is
        // not a token start, so the newline convention resumes mid-comment there and only the close certifies.
        if (blocks && random.next() % 3 == 0)
        {
            out += "/*";

            const auto lines{1 + random.next() % 4};

            for (std::size_t line{0}; line < lines; ++line)
            {
                out += '\n';

                const auto interior{2 + random.next() % 4};

                for (std::size_t piece{0}; piece < interior; ++piece)
                {
                    // The interior avoids '*' entirely, so the comment closes exactly where written.
                    out += ' ';

                    out += words[random.next() % 8];
                }
            }

            out += " */\n";

            continue;
        }

        const auto pieces{3 + random.next() % 6};

        for (std::size_t piece{0}; piece < pieces; ++piece)
        {
            switch (random.next() % 10)
            {
            case 0:
                out += std::to_string(random.next());

                break;

            case 1:
                if (strings)
                {
                    out += '"';

                    out += words[random.next() % 8];

                    out += ' ';

                    out += words[random.next() % 8];

                    out += '"';
                }
                else
                {
                    out += words[random.next() % 8];
                }

                break;

            case 2:
                out += ops[random.next() % 8];

                out += ' ';

                out += words[random.next() % 8];

                break;

            case 3:
                out += words[random.next() % 8];

                out += ';';

                break;

            case 4:
                out += '(';

                out += words[random.next() % 8];

                out += ')';

                break;

            default:
                out += words[random.next() % 8];

                out += ' ';

                break;
            }

            out += ' ';
        }

        if (comments && random.next() % 4 == 0)
        {
            out += "// ";

            out += words[random.next() % 8];
        }

        out += '\n';
    }

    // Truncating can cut a literal or comment open; cut back to the last complete line and pad with newlines,
    // which every requesting row tokenizes, so every requested size is valid by construction.
    const auto last_newline{out.rfind('\n', bytes - 1)};

    out.resize(last_newline + 1);

    out.append(bytes - out.size(), '\n');

    return out;
}

/**
 * @brief A corpus of lexically valid JSON lines: objects and arrays of strings, numbers, and the literal names.
 *
 * Lexical validity is all the lexer needs, and all that is claimed; the lines are grammatical anyway.
 */
std::string json_corpus(const std::size_t bytes)
{
    std::string out;

    out.reserve(bytes + 128);

    Lcg random{0x5eed0002U};

    static constexpr std::string_view keys[]{"count", "offset", "state", "token", "chunk", "origin", "table", "index"};

    static constexpr std::string_view values[]{"true", "false", "null", "42", "-1.5e3", "0", "271828", "-7"};

    while (out.size() < bytes)
    {
        out += '{';

        const auto members{1 + random.next() % 4};

        for (std::size_t member{0}; member < members; ++member)
        {
            if (member != 0)
            {
                out += ", ";
            }

            out += '"';

            out += keys[random.next() % 8];

            out += "\": ";

            switch (random.next() % 3)
            {
            case 0:
                out += values[random.next() % 8];

                break;

            case 1:
                out += '"';

                out += keys[random.next() % 8];

                out += ' ';

                out += keys[random.next() % 8];

                out += '"';

                break;

            default:
                out += '[';

                out += values[random.next() % 8];

                out += ", ";

                out += values[random.next() % 8];

                out += ']';

                break;
            }
        }

        out += "}\n";
    }

    const auto last_newline{out.rfind('\n', bytes - 1)};

    out.resize(last_newline + 1);

    out.append(bytes - out.size(), '\n');

    return out;
}

/**
 * @brief The serial scan's failure offset on the given input, or the size when it tokenizes completely.
 */
std::size_t failure_offset(const munch::core::Lexer& lexer, const std::string_view input)
{
    return lexer.tokenize_all<Token>(input, [](const Token, const std::size_t) {});
}

/**
 * @brief The boundary set of a completely tokenizable input: every offset a token of its segmentation begins at.
 */
std::vector<std::size_t> boundaries(const munch::core::Lexer& lexer, const std::string_view input)
{
    std::vector<std::size_t> begins;

    std::size_t at{0};

    const auto consumed{lexer.tokenize_all<Token>(input, [&](const Token, const std::size_t length) {
        begins.push_back(at);

        at += length;
    })};

    if (consumed != input.size())
    {
        std::fprintf(stderr, "corpus not completely tokenizable: %zu of %zu\n", consumed, input.size());

        std::exit(EXIT_FAILURE);
    }

    return begins;
}

enum class Op : std::size_t
{
    Substitute,
    Delete,
    Insert,
};

constexpr std::string_view name(const Op op)
{
    switch (op)
    {
    case Op::Substitute:
        return "substitute";

    case Op::Delete:
        return "delete";

    default:
        return "insert";
    }
}

/**
 * @brief One damaged input beside the coordinate map its operation induces.
 */
struct Damage
{
    std::string input;

    /// First damaged-coordinate offset at which the pristine suffix is preserved.
    std::size_t end{0};

    /// Added to a pristine boundary at or past the pristine cut to obtain its damaged-coordinate image.
    std::ptrdiff_t shift{0};

    /// Pristine boundaries below this offset are unchanged; those inside [low, cut) have no image.
    std::size_t low{0};

    /// Pristine boundaries at or past this offset map through the shift.
    std::size_t cut{0};
};

Damage damage(const std::string& pristine, const Op op, const std::size_t p, const std::size_t k, Lcg& random)
{
    switch (op)
    {
    case Op::Substitute:
    {
        std::string out{pristine};

        for (std::size_t i{0}; i < k; ++i)
        {
            out[p + i] = random.byte();
        }

        return Damage{.input = std::move(out), .end = p + k, .shift = 0, .low = p, .cut = p + k};
    }

    case Op::Delete:
    {
        std::string out{pristine.substr(0, p)};

        out += pristine.substr(p + k);

        return Damage{
                .input = std::move(out),
                .end = p,
                .shift = -static_cast<std::ptrdiff_t>(k),
                .low = p,
                .cut = p + k};
    }

    default:
    {
        std::string out{pristine.substr(0, p)};

        for (std::size_t i{0}; i < k; ++i)
        {
            out += random.byte();
        }

        out += pristine.substr(p);

        return Damage{
                .input = std::move(out),
                .end = p + k,
                .shift = static_cast<std::ptrdiff_t>(k),
                .low = p,
                .cut = p};
    }
    }
}

/**
 * @brief Whether a damaged-coordinate position is the image of a pristine boundary outside the damaged window.
 */
bool landed(const std::vector<std::size_t>& pristine, const Damage& y, const std::size_t at)
{
    if (at < y.low)
    {
        return std::binary_search(pristine.begin(), pristine.end(), at);
    }

    if (static_cast<std::ptrdiff_t>(at) < static_cast<std::ptrdiff_t>(y.cut) + y.shift)
    {
        return false;
    }

    const auto preimage{static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at) - y.shift)};

    return preimage >= y.cut && std::binary_search(pristine.begin(), pristine.end(), preimage);
}

/**
 * @brief The first image of a pristine boundary at or past the corruption end, when one exists.
 */
std::optional<std::size_t> first_true_boundary(const std::vector<std::size_t>& pristine, const Damage& y)
{
    const auto from{static_cast<std::size_t>(static_cast<std::ptrdiff_t>(y.end) - y.shift)};

    const auto found{std::lower_bound(pristine.begin(), pristine.end(), std::max(from, y.cut))};

    if (found == pristine.end())
    {
        return std::nullopt;
    }

    return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(*found) + y.shift);
}

/**
 * @brief The recovery move an arm makes, one of five kinds sharing the completed-incident driver.
 */
enum class Kind : std::size_t
{
    /// The certificate walk, byte and window evidence in evidence order.
    Certified,

    /// The anchored-exact procedure: the shipped strictly-stronger decider at the anchor, the anchor
    /// advancing past a beyond-repair tail's poison until a certificate holds, so refusal at one anchor
    /// is a decision, not a dead end.
    Exact,

    /// Resume at the search start itself, the skip-one convention.
    Skip,

    /// Raw-byte delimiter search, at or one past the next occurrence.
    Delim,

    /// Token-aware delimiter search: skip until the scan makes progress, discard emitted tokens through
    /// the first containing the delimiter, resume one past that token, classical two-phase panic made
    /// concrete at the lexical layer.
    TokenDelim,
};

/**
 * @brief One evaluated arm: a kind, its delimiter where one applies, and whether the search floor is the
 *        corruption end (the oracle arms, modeling a caller told the damage's extent) or the failure alone.
 */
struct Arm
{
    std::string_view name;

    Kind kind;

    char delimiter{'\0'};

    bool past{false};

    bool clean{false};
};

/**
 * @brief The position one past the next occurrence of the delimiter at or after from, the classical
 *        discard-through-the-delimiter convention.
 */
std::optional<std::size_t> past_next(const std::string_view input, const std::size_t from, const char delimiter)
{
    const auto at{input.find(delimiter, from)};

    if (at == std::string_view::npos)
    {
        return std::nullopt;
    }

    // One past a final delimiter is the end-of-input offset, a completed resume rather than a refusal.
    return at + 1;
}

/**
 * @brief The position of the next occurrence of the delimiter itself at or after from: the
 *        delimiter retained rather than consumed, the other classical reading, evaluated as
 *        its own named baseline because the two placements are different algorithms with
 *        outcome-critical differences.
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
 * @brief One arm's accumulated incident outcomes for a stratum: every figure the summary reports.
 */
struct Tally
{
    std::size_t trials{0};

    std::size_t answers{0};

    std::size_t refusals{0};

    std::size_t first_landings{0};

    std::size_t terminal_landings{0};

    std::size_t completions{0};

    std::size_t capped{0};

    std::size_t attempts_sum{0};

    std::ptrdiff_t conv_sum{0};

    std::size_t terminal_refused{0};

    std::size_t conv_count{0};

    std::size_t lost_sum{0};

    std::size_t spurious_sum{0};

    std::ptrdiff_t overshoot_sum{0};

    std::size_t overshoot_count{0};

    /// Terminal positions strictly inside the input, the terminal-landing denominator.
    std::size_t terminal_interior{0};
};

struct Row
{
    std::string_view label;

    munch::core::Lexer lexer;

    std::string corpus;

    std::vector<std::size_t> begins;
};

/**
 * @brief The evidence behind a walk answer: the certified byte's position or the window occurrence's start.
 *
 * Replicates next_certified_start()'s walk deterministically and reports where the answering certificate
 * begins, which the position-only return cannot carry; the support-aware classification and its assertion
 * read the theorems' exact precondition from this. A mismatch with the library's answer is a harness defect
 * and fails the run.
 */
struct Evidence
{
    std::size_t begin{0};

    bool byte{false};

    std::size_t length{0};

    std::size_t origin{0};
};

std::optional<Evidence> evidence_of(
        const munch::core::Lexer& lexer, const std::string_view input, const std::size_t from, const std::size_t answer)
{
    for (std::size_t at{from}; at < input.size(); ++at)
    {
        if (lexer.is_split_point(input[at]))
        {
            if (at == answer)
            {
                return Evidence{.begin = at, .byte = true, .length = 1, .origin = 0};
            }

            return std::nullopt;
        }

        const auto limit{std::min<std::size_t>(4, input.size() - at)};

        for (std::size_t length{2}; length <= limit; ++length)
        {
            if (const auto origin{lexer.is_split_window(input.substr(at, length))})
            {
                if (at + *origin == answer)
                {
                    return Evidence{.begin = at, .byte = false, .length = length, .origin = *origin};
                }

                return std::nullopt;
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief The smallest answer any certificate at or after the offset yields, for the nonminimality figure.
 *
 * Evidence past the walk's answer cannot yield a smaller one, so the scan stops there.
 */
std::size_t minimal_answer(
        const munch::core::Lexer& lexer, const std::string_view input, const std::size_t from, const std::size_t answer)
{
    auto minimal{answer};

    for (std::size_t at{from}; at <= answer && at < input.size(); ++at)
    {
        if (lexer.is_split_point(input[at]))
        {
            minimal = std::min(minimal, at);

            continue;
        }

        const auto limit{std::min<std::size_t>(4, input.size() - at)};

        for (std::size_t length{2}; length <= limit; ++length)
        {
            if (const auto origin{lexer.is_split_window(input.substr(at, length))})
            {
                minimal = std::min(minimal, at + *origin);
            }
        }
    }

    return minimal;
}

/**
 * @brief Hard oracle on the pristine corpus: every certified answer from sampled offsets must be a boundary.
 *
 * The corpus is completely tokenizable, so the certificates' own theorems apply to it directly, with no repair
 * quantifier and no seam; any violation is a defect in the certificate or the walk, and fails the run.
 */
std::size_t pristine_oracle(const Row& row, const std::size_t samples)
{
    Lcg random{0x5eed0003U};

    std::size_t failures{0};

    for (std::size_t sample{0}; sample < samples; ++sample)
    {
        // The widened generator broke the old 15-bit scaling here silently, every draw landing past the
        // corpus and the oracle checking nothing; unbiased rejection sampling replaces it.
        const auto from{static_cast<std::size_t>(random.bounded(static_cast<std::uint32_t>(row.corpus.size() - 1)))};

        if (const auto found{row.lexer.next_certified_start(row.corpus, from)})
        {
            if (!std::binary_search(row.begins.begin(), row.begins.end(), *found) || *found < from)
            {
                std::fprintf(
                        stderr, "PRISTINE ORACLE VIOLATION: %s from %zu answered %zu\n", std::string{row.label}.c_str(),
                        from, *found);

                ++failures;
            }
        }
    }

    return failures;
}

/**
 * @brief Wilson 95 percent score interval for successes out of n, both bounds in percent.
 */
std::pair<double, double> wilson(const std::size_t successes, const std::size_t n)
{
    if (n == 0)
    {
        return {0.0, 0.0};
    }

    const auto z{1.959963984540054};

    const auto total{static_cast<double>(n)};

    const auto rate{static_cast<double>(successes) / total};

    const auto denominator{1.0 + z * z / total};

    const auto center{rate + z * z / (2.0 * total)};

    const auto margin{z * std::sqrt(rate * (1.0 - rate) / total + z * z / (4.0 * total * total))};

    return {100.0 * (center - margin) / denominator, 100.0 * (center + margin) / denominator};
}

/**
 * @brief Scans one resumed segment from base, recording every absolute token start, returning bytes consumed.
 */
std::size_t segment_starts(
        const munch::core::Lexer& lexer, const std::string_view input, const std::size_t base,
        std::vector<std::size_t>& starts)
{
    std::size_t at{base};

    return lexer.tokenize_all<Token>(
            std::string_view{input.data() + base, input.size() - base}, [&](const Token, const std::size_t length) {
                starts.push_back(at);

                at += length;
            });
}

/**
 * @brief Whether one emitted token is a designated synchronizer for the delimiter: the delimiter's own
 *        punctuation token exactly, or an all-whitespace token carrying the newline. A string or comment
 *        token that merely contains the delimiter byte is not a synchronizer, which is the token-aware
 *        discipline's point.
 */
bool synchronizes(const std::string_view text, const char delimiter)
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
 * @brief The token-aware delimiter move: from the search start, skip bytes until the scan makes progress,
 *        then discard emitted tokens through the first designated synchronizer and resume one past it;
 *        refuses when no synchronizing token exists ahead of any resumable offset.
 */
std::optional<std::size_t> token_sync(
        const munch::core::Lexer& lexer, const std::string_view input, const std::size_t from, const char delimiter)
{
    std::size_t at{from};

    while (at < input.size())
    {
        std::optional<std::size_t> sync;

        std::size_t scan{at};

        const auto consumed{lexer.tokenize_all<Token>(
                std::string_view{input.data() + at, input.size() - at}, [&](const Token, const std::size_t length) {
                    if (!sync && synchronizes(std::string_view{input.data() + scan, length}, delimiter))
                    {
                        sync = scan + length;
                    }

                    scan += length;
                })};

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
 * @brief One arm's completed incident: driven from the first failure to the end of input, a refusal, or
 *        the attempt budget, under one stopping rule shared by every arm.
 */
struct Incident
{
    std::optional<std::size_t> first;

    std::optional<munch::core::Lexer::Certified_start> evidence;

    /// Every certified move's resume position with its evidence begin, so the theorem assertions and the
    /// covered tallies range over all answers, not the first per incident.
    std::vector<std::pair<std::size_t, std::size_t>> moves;

    std::optional<std::size_t> terminal;

    std::size_t attempts{0};

    /// 0 completed, 1 refused, 2 capped.
    std::size_t outcome{1};

    /// Absolute starts of every token emitted after the first resume.
    std::vector<std::size_t> starts;
};

constexpr std::string_view outcome_name(const std::size_t outcome)
{
    switch (outcome)
    {
    case 0:
        return "completed";

    case 1:
        return "refused";

    default:
        return "capped";
    }
}

Incident run_incident(
        const munch::core::Lexer& lexer, const std::string_view input, const std::size_t failure,
        const std::size_t clean_floor, const Arm& arm, const std::size_t cap)
{
    Incident incident{};

    std::size_t fail{failure};

    while (true)
    {
        const auto start{arm.clean ? std::max(clean_floor, fail + 1) : fail + 1};

        if (start >= input.size())
        {
            incident.terminal = input.size();

            incident.outcome = 0;

            break;
        }

        std::optional<std::size_t> resume;

        std::optional<munch::core::Lexer::Certified_start> evidence;

        switch (arm.kind)
        {
        case Kind::Certified:
            evidence = lexer.next_certified_evidence(input, start);

            if (evidence)
            {
                resume = evidence->start;
            }

            break;

        case Kind::Exact:
            for (std::size_t anchor{start}; anchor < input.size(); ++anchor)
            {
                if (const auto found{lexer.next_anchored_start(
                            std::string_view{input.data() + anchor, input.size() - anchor}, 0)})
                {
                    resume = anchor + *found;

                    break;
                }
            }

            break;

        case Kind::Skip:
            resume = start;

            break;

        case Kind::Delim:
            resume = arm.past ? past_next(input, start, arm.delimiter) : at_next(input, start, arm.delimiter);

            break;

        default:
            resume = token_sync(lexer, input, start, arm.delimiter);

            break;
        }

        if (!resume)
        {
            incident.outcome = 1;

            break;
        }

        ++incident.attempts;

        if (!incident.first)
        {
            incident.first = *resume;

            incident.evidence = evidence;
        }

        if (evidence)
        {
            incident.moves.emplace_back(*resume, evidence->evidence_begin);
        }

        incident.terminal = *resume;

        if (*resume >= input.size())
        {
            incident.outcome = 0;

            break;
        }

        const auto consumed{segment_starts(lexer, input, *resume, incident.starts)};

        if (*resume + consumed == input.size())
        {
            incident.outcome = 0;

            break;
        }

        fail = *resume + consumed;

        if (incident.attempts >= cap)
        {
            incident.outcome = 2;

            break;
        }
    }

    return incident;
}

/**
 * @brief Where the resumed stream and the mapped pristine stream agree forever after: the smallest emitted
 *        position from which the two boundary suffixes coincide, with the divergence region's mapped
 *        boundaries counted lost and its non-landing emitted starts counted spurious.
 */
struct Convergence
{
    std::size_t at{0};

    std::size_t lost{0};

    std::size_t spurious{0};
};

Convergence converge(
        const std::vector<std::size_t>& pristine, const Damage& y, const std::vector<std::size_t>& starts,
        const std::size_t floor)
{
    const auto image = [&](const std::size_t boundary) -> std::optional<std::size_t> {
        if (boundary < y.low)
        {
            return boundary;
        }

        if (boundary >= y.cut)
        {
            return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(boundary) + y.shift);
        }

        return std::nullopt;
    };

    // Walk both sorted sequences backward from their ends to the first disagreement.
    auto i{static_cast<std::ptrdiff_t>(starts.size()) - 1};

    auto j{static_cast<std::ptrdiff_t>(pristine.size()) - 1};

    std::optional<std::size_t> agreed;

    while (i >= 0 && starts[static_cast<std::size_t>(i)] >= floor)
    {
        // The next mapped pristine boundary at or above the floor, skipping the imageless window.
        std::optional<std::size_t> mapped;

        while (j >= 0)
        {
            mapped = image(pristine[static_cast<std::size_t>(j)]);

            if (mapped && *mapped < floor)
            {
                mapped = std::nullopt;

                j = -1;

                break;
            }

            if (mapped)
            {
                break;
            }

            --j;
        }

        if (!mapped || *mapped != starts[static_cast<std::size_t>(i)])
        {
            break;
        }

        agreed = *mapped;

        --i;

        --j;
    }

    Convergence result{};

    // Full agreement down to the floor on both sides converges at the floor; no common suffix converges
    // only at the end of input.
    const auto exhausted_pristine{[&] {
        while (j >= 0)
        {
            const auto mapped{image(pristine[static_cast<std::size_t>(j)])};

            if (mapped && *mapped >= floor)
            {
                return false;
            }

            --j;
        }

        return true;
    }};

    if ((i < 0 || starts[static_cast<std::size_t>(i)] < floor) && exhausted_pristine())
    {
        result.at = floor;
    }
    else
    {
        result.at = agreed ? *agreed : y.input.size();
    }

    // Lost boundaries are counted over the manuscript's region, from the corruption end to the
    // convergence point, so the initial jump's skipped starts are included; the matching floor above
    // stays at the first resume, where emitted starts begin.
    for (const auto boundary : pristine)
    {
        const auto mapped{image(boundary)};

        if (mapped && *mapped >= y.end && *mapped < result.at)
        {
            ++result.lost;
        }
    }

    for (const auto start : starts)
    {
        if (start >= floor && start < result.at && !landed(pristine, y, start))
        {
            ++result.spurious;
        }
    }

    return result;
}

} // namespace

int main(const int argc, const char** argv)
{
    const std::size_t corpus_kib{argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 64};

    const std::size_t trials{argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 60};

    // Empty strings stand for absent, so a caller can reach the seed argument without a csv or real corpus.
    const char* csv_path{argc > 3 && argv[3][0] != '\0' ? argv[3] : nullptr};

    const char* real_path{argc > 4 && argv[4][0] != '\0' ? argv[4] : nullptr};

    const std::size_t seeds{argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 3};

    const auto bytes{corpus_kib << 10U};

    std::vector<Row> rows;

    {
        munch::core::Builder b;

        figures::c_like(b, false);

        b.add_token(figures::string_literal(), Token::String, 2);

        b.add_token(figures::line_comment(), Token::LineComment, 1);

        auto corpus{c_like_corpus(bytes, true, true, false)};

        auto lexer{b.build()};

        auto begins{boundaries(lexer, corpus)};

        rows.push_back(
                Row{.label = "c-like conventional with strings and line comments",
                    .lexer = std::move(lexer),
                    .corpus = std::move(corpus),
                    .begins = std::move(begins)});
    }

    {
        munch::core::Builder b;

        figures::c_like(b, false);

        b.add_token(figures::block_comment(), Token::BlockComment, 1);

        auto corpus{c_like_corpus(bytes, false, false, true)};

        auto lexer{b.build()};

        auto begins{boundaries(lexer, corpus)};

        rows.push_back(
                Row{.label = "c-like conventional plus block comments alone",
                    .lexer = std::move(lexer),
                    .corpus = std::move(corpus),
                    .begins = std::move(begins)});
    }

    {
        munch::core::Builder b;

        figures::json(b);

        auto corpus{json_corpus(bytes)};

        auto lexer{b.build()};

        auto begins{boundaries(lexer, corpus)};

        rows.push_back(
                Row{.label = "json rfc 8259 lexical forms",
                    .lexer = std::move(lexer),
                    .corpus = std::move(corpus),
                    .begins = std::move(begins)});
    }

    {
        munch::core::Builder b;

        figures::c_like(b, true);

        b.add_token(figures::string_literal(), Token::String, 2);

        b.add_token(figures::line_comment(), Token::LineComment, 1);

        auto corpus{c_like_corpus(bytes, true, true, false)};

        auto lexer{b.build()};

        auto begins{boundaries(lexer, corpus)};

        rows.push_back(
                Row{.label = "c-like split-friendly with strings and line comments",
                    .lexer = std::move(lexer),
                    .corpus = std::move(corpus),
                    .begins = std::move(begins)});
    }

    {
        munch::core::Builder b;

        figures::c_like(b, false);

        auto corpus{c_like_corpus(bytes, false, false, false)};

        auto lexer{b.build()};

        auto begins{boundaries(lexer, corpus)};

        rows.push_back(
                Row{.label = "c-like bare: identifiers numbers operators punctuation",
                    .lexer = std::move(lexer),
                    .corpus = std::move(corpus),
                    .begins = std::move(begins)});
    }

    if (real_path)
    {
        munch::core::Builder b;

        figures::json(b);

        std::ifstream in{real_path, std::ios::binary};

        std::stringstream buffer;

        buffer << in.rdbuf();

        auto corpus{buffer.str()};

        if (corpus.empty())
        {
            std::fprintf(stderr, "real corpus unreadable or empty: %s\n", real_path);

            return EXIT_FAILURE;
        }

        auto lexer{b.build()};

        auto begins{boundaries(lexer, corpus)};

        rows.push_back(
                Row{.label = "json rfc 8259 lexical forms on a real-world document",
                    .lexer = std::move(lexer),
                    .corpus = std::move(corpus),
                    .begins = std::move(begins)});
    }

    std::size_t oracle_failures{0};

    for (const auto& row : rows)
    {
        oracle_failures += pristine_oracle(row, 512);
    }

    std::printf("pristine oracle: %zu violations over %zu rows x 512 samples\n", oracle_failures, rows.size());

    // The final-delimiter fixture, updated for the repaired past placement: one past a final delimiter is
    // now the end-of-input offset, a completed resume with nothing left to scan, rather than a refusal, so
    // the two placements answer together everywhere and differ by exactly the delimiter.
    {
        constexpr std::string_view final_newline{"aa\n"};

        if (past_next(final_newline, 1, '\n') != std::optional<std::size_t>{3} ||
            at_next(final_newline, 1, '\n') != std::optional<std::size_t>{2})
        {
            std::printf("FAILED: the final-delimiter fixture broke\n");

            return 1;
        }
    }

    std::printf(
            "deterministic: corpus seeds 0x5eed0001 through 0x5eed0003, schedule seed 0x5eedc0de and payload seed "
            "0x5eedbeef each offset per (row, seed, op, k) so no two rows share a stream, %zu independent seeds, "
            "positions by unbiased rejection sampling, attempt budget 100 per incident\n",
            seeds);

    // The generated corpora, written beside the archive so every column recomputes from the archive alone; the
    // real document is already on disk, hashed in the data notes.
    if (csv_path != nullptr)
    {
        for (const auto& row : rows)
        {
            if (real_path != nullptr && row.label == "json rfc 8259 lexical forms on a real-world document")
            {
                continue;
            }

            std::string slug{row.label};

            for (auto& byte : slug)
            {
                byte = static_cast<char>(std::isalnum(static_cast<unsigned char>(byte)) != 0 ? byte : '-');
            }

            std::ofstream out{"corpus-" + slug + ".bin", std::ios::binary};

            out.write(row.corpus.data(), static_cast<std::streamsize>(row.corpus.size()));

            // A truncated corpus beside the archive would break offline recomputation silently.
            if (!out)
            {
                std::fprintf(stderr, "corpus write failed: %s\n", slug.c_str());

                return EXIT_FAILURE;
            }
        }
    }

    std::FILE* csv{csv_path ? std::fopen(csv_path, "w") : nullptr};

    // One raw row per strategy per damaging trial, plus one row marking each trial the grammar absorbed, so any
    // statistic stays computable from the archive; overshoot is blank where no post-corruption boundary exists.
    if (csv)
    {
        std::fprintf(
                csv,
                "grammar,op,k,seed,trial,p,failure_offset,corruption_end,first_true,repairable,minimal_repair,"
                "exact_at_anchor,strategy,first,first_landed,evidence_begin,evidence_end,evidence_kind,minimal,"
                "terminal,terminal_landed,outcome,attempts,moves_covered,moves_covered_landed,converged,lost,"
                "spurious\n");
    }

    constexpr std::array<Op, 3> ops{Op::Substitute, Op::Delete, Op::Insert};

    constexpr std::array<std::size_t, 3> ks{1, 4, 16};

    std::size_t theorem_failures{0};

    std::size_t evidence_covered{0};

    std::size_t certified_moves_total{0};

    std::size_t certified_moves_covered{0};

    std::size_t evidence_uncovered{0};

    std::size_t evidence_uncovered_landed{0};

    std::size_t nonminimal_answers{0};

    std::size_t nonminimal_bytes{0};

    std::size_t clean_answers{0};

    std::size_t clean_refusals{0};

    std::size_t absorbed_total{0};

    std::size_t repairable_total{0};

    std::size_t unrepairable_total{0};

    std::size_t vacuous_walk_answers{0};

    std::size_t exact_answers_total{0};

    std::size_t exact_saved_bytes{0};

    std::ptrdiff_t exact_net_displacement{0};

    std::size_t exact_pairs{0};

    std::size_t duplicate_positions{0};

    constexpr std::array<Arm, 11> arms{
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

    constexpr std::size_t cap{100};

    for (std::size_t row_index{0}; row_index < rows.size(); ++row_index)
    {
        const auto& row{rows[row_index]};

        std::printf("\n%s\n", std::string{row.label}.c_str());

        // Tallies pooled over seeds per (op, k, arm) stratum; per-seed first-landing kept beside them so
        // seed stability is a printed figure rather than a claim.
        std::array<std::array<std::array<Tally, 11>, 3>, 3> tallies{};

        std::vector<std::array<std::size_t, 11>> seed_answers(seeds);

        std::vector<std::array<std::size_t, 11>> seed_landings(seeds);

        for (std::size_t seed{0}; seed < seeds; ++seed)
        {
            for (std::size_t op_index{0}; op_index < ops.size(); ++op_index)
            {
                const auto op{ops[op_index]};

                for (std::size_t k_index{0}; k_index < ks.size(); ++k_index)
                {
                    const auto k{ks[k_index]};

                    // The row index enters both seeds, so no two rows share a schedule or a payload
                    // stream; the third and fourth campaigns' cross-row pairing is gone by construction.
                    Lcg positions{
                            0x5eedc0deU + static_cast<std::uint32_t>(seed) * 0x01000193U +
                            static_cast<std::uint32_t>(row_index) * 0x9e3779b9U + static_cast<std::uint32_t>(k) * 7U +
                            static_cast<std::uint32_t>(op) * 131U};

                    Lcg payload{
                            0x5eedbeefU + static_cast<std::uint32_t>(seed) * 0x01000193U +
                            static_cast<std::uint32_t>(row_index) * 0x9e3779b9U + static_cast<std::uint32_t>(k) * 7U +
                            static_cast<std::uint32_t>(op) * 131U};

                    std::unordered_set<std::size_t> seen_positions;

                    for (std::size_t trial{0}; trial < trials; ++trial)
                    {
                        const auto span{row.corpus.size() - k - 128};

                        const auto p{
                                64 + static_cast<std::size_t>(positions.bounded(static_cast<std::uint32_t>(span)))};

                        if (!seen_positions.insert(p).second)
                        {
                            ++duplicate_positions;
                        }

                        auto y{damage(row.corpus, op, p, k, payload)};

                        const auto e{failure_offset(row.lexer, y.input)};

                        if (e == y.input.size())
                        {
                            ++absorbed_total;

                            if (csv)
                            {
                                std::fprintf(
                                        csv, "%s,%s,%zu,%zu,%zu,%zu,,%zu,,,,,absorbed,,,,,,,,,,,,,,,\n",
                                        std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, seed, trial,
                                        p, y.end);
                            }

                            continue;
                        }

                        const auto first_true{first_true_boundary(row.begins, y)};

                        // Repairability of the blind tail, the stratifier separating real answers from
                        // vacuous ones under the complete-repair reading; the label is the routine's
                        // reported verdict, and each returned repair is witness-verified below.
                        const auto tail{std::string_view{y.input}.substr(std::min(e + 1, y.input.size()))};

                        const auto repair{row.lexer.minimal_repair(tail)};

                        if (repair)
                        {
                            ++repairable_total;

                            // The witness is executed, not trusted: the returned repair prepended to the
                            // tail must scan to the end of input, turning the repairable label into a
                            // per-trial fact.
                            const auto witness{*repair + std::string{tail}};

                            if (failure_offset(row.lexer, witness) != witness.size())
                            {
                                std::fprintf(
                                        stderr, "REPAIR WITNESS VIOLATION: %s %s k=%zu p=%zu e=%zu\n",
                                        std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e);

                                ++theorem_failures;
                            }
                        }
                        else
                        {
                            ++unrepairable_total;
                        }

                        // The decider's own answer at the blind anchor, direct and archived, kept apart
                        // from the anchor-advancing procedure the exact arm runs.
                        const auto direct{row.lexer.next_anchored_start(tail, 0)};

                        std::optional<std::size_t> direct_at;

                        if (direct)
                        {
                            direct_at = std::min(e + 1, y.input.size()) + *direct;
                        }

                        std::array<Incident, 11> incidents{};

                        for (std::size_t s_index{0}; s_index < arms.size(); ++s_index)
                        {
                            incidents[s_index] = run_incident(row.lexer, y.input, e, y.end, arms[s_index], cap);
                        }

                        // Harness-independence: the library's evidence must match the replica's walk.
                        if (incidents[0].first)
                        {
                            const auto replica{evidence_of(row.lexer, y.input, e + 1, *incidents[0].first)};

                            if (!replica || !incidents[0].evidence ||
                                incidents[0].evidence->evidence_begin != replica->begin ||
                                incidents[0].evidence->window == replica->byte)
                            {
                                std::fprintf(
                                        stderr, "EVIDENCE MISMATCH: %s %s k=%zu p=%zu e=%zu answered %zu\n",
                                        std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e,
                                        *incidents[0].first);

                                ++theorem_failures;
                            }
                        }

                        // The sharp transfer: a certified answer whose evidence clears the corruption end
                        // must land; the conservative end-plus-three form stays beside it.
                        if (incidents[0].first)
                        {
                            const auto landed_first{landed(row.begins, y, *incidents[0].first)};

                            if (incidents[0].evidence && incidents[0].evidence->evidence_begin >= y.end)
                            {
                                ++evidence_covered;

                                if (!landed_first)
                                {
                                    std::fprintf(
                                            stderr, "EVIDENCE VIOLATION: %s %s k=%zu p=%zu e=%zu answered %zu\n",
                                            std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e,
                                            *incidents[0].first);

                                    ++theorem_failures;
                                }
                            }
                            else
                            {
                                ++evidence_uncovered;

                                if (landed_first)
                                {
                                    ++evidence_uncovered_landed;
                                }
                            }

                            if (*incidents[0].first >= y.end + 3 && !landed_first)
                            {
                                std::fprintf(
                                        stderr, "THEOREM VIOLATION: %s %s k=%zu p=%zu e=%zu answered %zu\n",
                                        std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e,
                                        *incidents[0].first);

                                ++theorem_failures;
                            }

                            const auto minimal{minimal_answer(row.lexer, y.input, e + 1, *incidents[0].first)};

                            if (minimal < *incidents[0].first)
                            {
                                ++nonminimal_answers;

                                nonminimal_bytes += *incidents[0].first - minimal;
                            }
                        }

                        // Every certified move faces the same transfer, not only the first per incident:
                        // any answer whose evidence clears the corruption end must land, asserted across
                        // both certified arms' whole incidents.
                        for (const std::size_t arm_index : {std::size_t{0}, std::size_t{1}})
                        {
                            for (const auto& [move_at, move_evidence] : incidents[arm_index].moves)
                            {
                                if (arm_index == 0)
                                {
                                    ++certified_moves_total;
                                }

                                if (move_evidence >= y.end)
                                {
                                    if (arm_index == 0)
                                    {
                                        ++certified_moves_covered;
                                    }

                                    if (move_at < y.input.size() && !landed(row.begins, y, move_at))
                                    {
                                        std::fprintf(
                                                stderr, "MOVE EVIDENCE VIOLATION: %s %s k=%zu p=%zu e=%zu at %zu\n",
                                                std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e,
                                                move_at);

                                        ++theorem_failures;
                                    }
                                }
                            }
                        }

                        // The clean certified arm's contract is total: search floor at the corruption end,
                        // evidence covered by construction, landing guaranteed, both asserted on every trial.
                        if (incidents[1].first)
                        {
                            ++clean_answers;

                            if (!incidents[1].evidence || incidents[1].evidence->evidence_begin < y.end ||
                                !landed(row.begins, y, *incidents[1].first))
                            {
                                std::fprintf(
                                        stderr, "CLEAN-ARM VIOLATION: %s %s k=%zu p=%zu e=%zu answered %zu\n",
                                        std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e,
                                        *incidents[1].first);

                                ++theorem_failures;
                            }
                        }
                        else
                        {
                            ++clean_refusals;
                        }

                        // Decider-versus-walk consistency, both directions at the blind anchor itself,
                        // the direct call above and never the advancing procedure: on a repairable tail a
                        // walk answer implies a direct answer at or before it; on an unrepairable tail
                        // the direct call must refuse rather than answer vacuously.
                        if (repair && incidents[0].first && (!direct_at || *direct_at > *incidents[0].first))
                        {
                            std::fprintf(
                                    stderr, "EXACT ORDER VIOLATION: %s %s k=%zu p=%zu e=%zu\n",
                                    std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e);

                            ++theorem_failures;
                        }

                        if (!repair && direct_at)
                        {
                            std::fprintf(
                                    stderr, "EXACT REFUSAL VIOLATION: %s %s k=%zu p=%zu e=%zu\n",
                                    std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e);

                            ++theorem_failures;
                        }

                        if (!repair && incidents[0].first)
                        {
                            ++vacuous_walk_answers;
                        }

                        if (incidents[2].first)
                        {
                            ++exact_answers_total;

                            if (incidents[0].first)
                            {
                                ++exact_pairs;

                                // Nonnegative by the order assertion on repairable trials, and measured
                                // against the decider's direct answer at the blind anchor, never the
                                // advancing procedure; on unrepairable trials the advanced anchor can land
                                // past the walk's vacuous answer, so the net crosses zero and is kept
                                // signed.
                                if (repair && direct_at)
                                {
                                    exact_saved_bytes += *incidents[0].first - *direct_at;
                                }

                                exact_net_displacement += static_cast<std::ptrdiff_t>(*incidents[0].first) -
                                                          static_cast<std::ptrdiff_t>(*incidents[2].first);
                            }
                        }

                        // The exact clean arm's answers must land: the pristine prefix is a repair of what
                        // precedes the preserved suffix, so an anchored-invariant position lies on a mapped
                        // pristine boundary.
                        if (incidents[3].first && *incidents[3].first < y.input.size() &&
                            !landed(row.begins, y, *incidents[3].first))
                        {
                            std::fprintf(
                                    stderr, "EXACT-CLEAN VIOLATION: %s %s k=%zu p=%zu e=%zu answered %zu\n",
                                    std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e,
                                    *incidents[3].first);

                            ++theorem_failures;
                        }

                        // The two delimiter placements now answer together everywhere and differ by exactly
                        // the delimiter, the repaired past placement having no refusal of its own.
                        for (const auto& [past_s, at_s, delimiter] :
                             {std::tuple<std::size_t, std::size_t, char>{5, 6, '\n'},
                              std::tuple<std::size_t, std::size_t, char>{7, 8, ';'}})
                        {
                            const auto& past_first{incidents[past_s].first};

                            const auto& at_first{incidents[at_s].first};

                            if (past_first.has_value() != at_first.has_value() ||
                                (past_first && (*past_first != *at_first + 1 || y.input[*at_first] != delimiter)))
                            {
                                std::fprintf(stderr, "CONVENTION VIOLATION\n");

                                ++theorem_failures;
                            }
                        }

                        for (std::size_t s_index{0}; s_index < arms.size(); ++s_index)
                        {
                            const auto& incident{incidents[s_index]};

                            auto& tally{tallies[op_index][k_index][s_index]};

                            ++tally.trials;

                            tally.attempts_sum += incident.attempts;

                            std::optional<bool> first_landed;

                            if (incident.first && *incident.first < y.input.size())
                            {
                                first_landed = landed(row.begins, y, *incident.first);
                            }

                            std::optional<bool> terminal_landed;

                            if (incident.terminal && *incident.terminal < y.input.size())
                            {
                                terminal_landed = landed(row.begins, y, *incident.terminal);

                                ++tally.terminal_interior;
                            }

                            if (incident.first)
                            {
                                ++tally.answers;

                                ++seed_answers[seed][s_index];

                                if (first_landed.value_or(false))
                                {
                                    ++tally.first_landings;

                                    ++seed_landings[seed][s_index];
                                }

                                if (first_true)
                                {
                                    tally.overshoot_sum += static_cast<std::ptrdiff_t>(*incident.first) -
                                                           static_cast<std::ptrdiff_t>(*first_true);

                                    ++tally.overshoot_count;
                                }
                            }
                            else
                            {
                                ++tally.refusals;
                            }

                            if (terminal_landed.value_or(false))
                            {
                                ++tally.terminal_landings;
                            }

                            std::optional<Convergence> convergence;

                            if (incident.outcome == 0)
                            {
                                ++tally.completions;

                                convergence = incident.first ?
                                                      converge(row.begins, y, incident.starts, *incident.first) :
                                                      Convergence{.at = y.input.size(), .lost = 0, .spurious = 0};

                                tally.conv_sum += static_cast<std::ptrdiff_t>(convergence->at) -
                                                  static_cast<std::ptrdiff_t>(y.end);

                                ++tally.conv_count;

                                tally.lost_sum += convergence->lost;

                                tally.spurious_sum += convergence->spurious;
                            }
                            else if (incident.outcome == 2)
                            {
                                ++tally.capped;
                            }
                            else
                            {
                                ++tally.terminal_refused;
                            }

                            if (csv)
                            {
                                std::fprintf(
                                        csv, "%s,%s,%zu,%zu,%zu,%zu,%zu,%zu,", std::string{row.label}.c_str(),
                                        std::string{name(op)}.c_str(), k, seed, trial, p, e, y.end);

                                if (first_true)
                                {
                                    std::fprintf(csv, "%zu", *first_true);
                                }

                                std::fprintf(csv, ",%d,", repair ? 1 : 0);

                                if (repair)
                                {
                                    std::fprintf(csv, "%zu", repair->size());
                                }

                                std::fprintf(csv, ",");

                                if (direct_at)
                                {
                                    std::fprintf(csv, "%zu", *direct_at);
                                }

                                std::fprintf(csv, ",%s,", std::string{arms[s_index].name}.c_str());

                                if (incident.first)
                                {
                                    std::fprintf(csv, "%zu", *incident.first);
                                }

                                std::fprintf(csv, ",");

                                if (first_landed)
                                {
                                    std::fprintf(csv, "%d", *first_landed ? 1 : 0);
                                }

                                if (incident.evidence)
                                {
                                    std::fprintf(
                                            csv, ",%zu,%zu,%s,", incident.evidence->evidence_begin,
                                            incident.evidence->evidence_end,
                                            incident.evidence->window ? "window" : "byte");

                                    std::fprintf(
                                            csv, "%zu",
                                            minimal_answer(
                                                    row.lexer, y.input,
                                                    arms[s_index].clean ? std::max(y.end, e + 1) : e + 1,
                                                    *incident.first));
                                }
                                else
                                {
                                    std::fprintf(csv, ",,,,");
                                }

                                std::fprintf(csv, ",");

                                if (incident.terminal)
                                {
                                    std::fprintf(csv, "%zu", *incident.terminal);
                                }

                                std::fprintf(csv, ",");

                                if (terminal_landed)
                                {
                                    std::fprintf(csv, "%d", *terminal_landed ? 1 : 0);
                                }

                                std::fprintf(
                                        csv, ",%s,%zu,", std::string{outcome_name(incident.outcome)}.c_str(),
                                        incident.attempts);

                                if (!incident.moves.empty())
                                {
                                    std::size_t covered_moves{0};

                                    std::size_t covered_landed{0};

                                    for (const auto& [move_at, move_evidence] : incident.moves)
                                    {
                                        if (move_evidence >= y.end)
                                        {
                                            ++covered_moves;

                                            if (move_at < y.input.size() && landed(row.begins, y, move_at))
                                            {
                                                ++covered_landed;
                                            }
                                        }
                                    }

                                    std::fprintf(csv, "%zu,%zu,", covered_moves, covered_landed);
                                }
                                else
                                {
                                    std::fprintf(csv, ",,");
                                }

                                if (convergence)
                                {
                                    std::fprintf(
                                            csv, "%zu,%zu,%zu\n", convergence->at, convergence->lost,
                                            convergence->spurious);
                                }
                                else
                                {
                                    std::fprintf(csv, ",,\n");
                                }
                            }
                        }
                    }
                }
            }
        }

        // The stratified table first, every (op, k, arm) cell pooled over seeds.
        std::printf(
                "  %-11s %2s  %-15s %7s %7s %7s %7s %8s %8s %6s %8s %8s %6s %6s %9s\n", "op", "k", "strategy",
                "answers", "refuse", "t-ref", "f-land", "t-land", "complete", "capped", "attempts", "conv", "lost",
                "spur", "overshoot");

        for (std::size_t op_index{0}; op_index < ops.size(); ++op_index)
        {
            for (std::size_t k_index{0}; k_index < ks.size(); ++k_index)
            {
                for (std::size_t s_index{0}; s_index < arms.size(); ++s_index)
                {
                    const auto& tally{tallies[op_index][k_index][s_index]};

                    const auto rate{[](const std::size_t hits, const std::size_t total) {
                        return total ? 100.0 * static_cast<double>(hits) / static_cast<double>(total) : 0.0;
                    }};

                    const auto mean{[](const auto sum, const std::size_t total) {
                        return total ? static_cast<double>(sum) / static_cast<double>(total) : 0.0;
                    }};

                    std::printf(
                            "  %-11s %2zu  %-15s %7zu %7zu %7zu %6.1f%% %7.1f%% %7.1f%% %6zu %8.2f %8.0f %6.2f "
                            "%6.2f %9.1f\n",
                            std::string{name(ops[op_index])}.c_str(), ks[k_index],
                            std::string{arms[s_index].name}.c_str(), tally.answers, tally.refusals,
                            tally.terminal_refused, rate(tally.first_landings, tally.answers),
                            rate(tally.terminal_landings, tally.terminal_interior),
                            rate(tally.completions, tally.trials), tally.capped, mean(tally.attempts_sum, tally.trials),
                            mean(tally.conv_sum, tally.conv_count), mean(tally.lost_sum, tally.conv_count),
                            mean(tally.spurious_sum, tally.conv_count),
                            mean(tally.overshoot_sum, tally.overshoot_count));
                }
            }
        }

        // The pooled row summary with Wilson 95 percent intervals on first landing and completion.
        std::printf("\n  pooled over all cells and seeds, Wilson 95%% intervals\n");

        for (std::size_t s_index{0}; s_index < arms.size(); ++s_index)
        {
            Tally pooled{};

            for (std::size_t op_index{0}; op_index < ops.size(); ++op_index)
            {
                for (std::size_t k_index{0}; k_index < ks.size(); ++k_index)
                {
                    const auto& tally{tallies[op_index][k_index][s_index]};

                    pooled.trials += tally.trials;

                    pooled.answers += tally.answers;

                    pooled.refusals += tally.refusals;

                    pooled.first_landings += tally.first_landings;

                    pooled.completions += tally.completions;

                    pooled.capped += tally.capped;

                    pooled.terminal_refused += tally.terminal_refused;
                }
            }

            const auto [land_low, land_high]{wilson(pooled.first_landings, pooled.answers)};

            const auto [complete_low, complete_high]{wilson(pooled.completions, pooled.trials)};

            std::printf(
                    "  %-15s answers %6zu initial-refusals %5zu terminal-refused %5zu first-landing [%5.1f%%, "
                    "%5.1f%%] completion [%5.1f%%, %5.1f%%] capped %zu\n",
                    std::string{arms[s_index].name}.c_str(), pooled.answers, pooled.refusals, pooled.terminal_refused,
                    land_low, land_high, complete_low, complete_high, pooled.capped);
        }

        for (std::size_t seed{0}; seed < seeds; ++seed)
        {
            std::printf("  seed %zu first-landing:", seed);

            for (std::size_t s_index{0}; s_index < arms.size(); ++s_index)
            {
                const auto answers{seed_answers[seed][s_index]};

                std::printf(
                        " %s %.1f%%", std::string{arms[s_index].name}.c_str(),
                        answers ? 100.0 * static_cast<double>(seed_landings[seed][s_index]) /
                                          static_cast<double>(answers) :
                                  0.0);
            }

            std::printf("\n");
        }
    }

    if (csv)
    {
        // A partial archive must never report success: stream errors and the close are
        // checked before any summary claims the run (a referee's artifact-hardening ask;
        // no byte of a healthy run changes).
        if (std::ferror(csv) != 0 || std::fclose(csv) != 0)
        {
            std::fprintf(stderr, "csv write or close failed\n");

            return EXIT_FAILURE;
        }
    }

    std::printf("\ndamage absorbed by the grammar without a scan failure: %zu trials\n", absorbed_total);

    std::printf(
            "evidence-covered first answers: %zu, all asserted to land; evidence-uncovered: %zu, of which %zu "
            "landed; certified moves in total: %zu, of which %zu covered, every covered move asserted to land\n",
            evidence_covered, evidence_uncovered, evidence_uncovered_landed, certified_moves_total,
            certified_moves_covered);

    std::printf("nonminimal answers: %zu, %zu extra bytes in total\n", nonminimal_answers, nonminimal_bytes);

    std::printf(
            "known-clean certified arm: %zu answers, every one asserted covered and landed; %zu refusals\n",
            clean_answers, clean_refusals);

    std::printf(
            "repairability at the blind anchor: %zu repairable, %zu unrepairable; the walk answered %zu of the "
            "unrepairable, the vacuous share its stratification labels\n",
            repairable_total, unrepairable_total, vacuous_walk_answers);

    std::printf(
            "exact anchored arm: %zu answers, the decider asserted at or before the walk on every repairable "
            "trial and refusing every unrepairable one before the anchor advances; %zu paired answers, %zu bytes "
            "saved on repairable trials, net displacement %td bytes over all pairs\n",
            exact_answers_total, exact_pairs, exact_saved_bytes, exact_net_displacement);

    std::printf("duplicate sampled positions across all cells: %zu\n", duplicate_positions);

    if (oracle_failures != 0 || theorem_failures != 0)
    {
        std::printf("FAILED: %zu oracle violations, %zu theorem violations\n", oracle_failures, theorem_failures);

        return EXIT_FAILURE;
    }

    std::printf("all oracle and theorem assertions held\n");

    return EXIT_SUCCESS;
}
