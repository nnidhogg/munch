// Measures the quality of certified error recovery against the classical panic-mode conventions, on the grammar
// rows the split-points study established, under a corruption model whose ground truth is exact by construction.
//
// The question. next_certified_start() returns positions with a soundness theorem: in every tokenizable repair of
// the input before an anchor, the image of a certified position begins a token. The classical conventions, skip
// one byte, skip past the next newline, skip past the next semicolon, promise nothing. This probe quantifies what
// the theorem is worth in practice: how often each strategy resumes at a true token boundary, how far past the
// first true boundary it lands, and how much of the stream each one loses per error.
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
// The position-only answer hides its evidence, so the harness replicates the walk to recover it: each
// certified answer's CSV row carries five extra columns, the evidence's begin offset, the minimal answer
// any certificate at or after the search start would have produced, the evidence's kind (byte or window),
// its length, and its origin, so byte and window answers are auditable apart even when an origin-zero
// window's begin equals its answer. A sharper transfer assertion fires on
// the exact precondition rather than the three-byte margin: a certified answer whose evidence begins at or
// past the corruption end must land. The conservative end-plus-three assertion stays beside it, and the
// summary reports the covered and uncovered tallies with the nonminimality figure. A fifth arm,
// certified-clean, models the caller who knows the damage's true extent: the same walk started at the
// corruption end or one past the failure, whichever is later, so every answer's evidence is covered by
// construction, asserted to be so and to land on every trial. The generated corpora are written beside the
// archive so every column recomputes from the archive alone.
//
// Metrics, per grammar row, operation, k, and strategy, aggregated over trials whose damage actually broke the
// serial scan (damage the grammar absorbs is counted and set aside):
//
//   answers   trials where the strategy produced a resume position inside the input.
//   refusals  trials where it produced none: no certificate for certified recovery, no delimiter for the
//             delimiter conventions, resume past the end for all.
//   landing   fraction of answers that are images of pristine boundaries, as defined above.
//   overshoot signed distance in bytes from the first mapped boundary at or past the corruption end to the
//             answer; negative when the answer lands before the corruption end, in the unchanged prefix.
//   lost      mapped boundaries in (e, r): token starts the error and the resume choice together skipped, where
//             e is the failure offset, the first byte the serial scan left unconsumed.
//   cascade   resume events needed to reach the end of the input when the driver loop alternates scan and
//             recover, capped by the progress lemma's bound; refusal mid-cascade ends the loop.
//
// Baseline conventions. All strategies search from e + 1, the same progress contract recover() keeps, so no
// strategy may retry the offending byte. Skip-one resumes at e + 1. The delimiter conventions resume one past the
// next delimiter at or after e + 1, the classical discard-through-the-delimiter reading of panic mode.
//
// Non-claims. Certified recovery answers with the first certificate in walk order, not the closest boundary, and
// refuses where nothing certifies; both behaviors are measured here, not excused. Nothing is claimed about the
// damaged input's own segmentation between the failure and the resume position. Printed figures are quotable only
// beside the clean commit of the collection ritual, exactly as the benchmark's are.
//
// Usage. recovery_quality [corpus KiB] [trials per cell] [csv path] [real json corpus path]
// Defaults are sized to run as a test; the campaign passes larger figures and archives the CSV. The optional
// fourth argument adds an ecological row: a real-world JSON document, read verbatim, held to the same complete
// tokenizability assertion, the same damage schedule, and the same oracle as the generated rows.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
    explicit Lcg(const unsigned seed) : state_{seed} {}

    unsigned next()
    {
        state_ = state_ * 1664525U + 1013904223U;

        return (state_ >> 16U) & 0x7fffU;
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
    unsigned state_;
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
 * @brief Mapped boundaries in the open interval (e, r): the token starts the error and the resume skipped.
 */
std::size_t lost_starts(
        const std::vector<std::size_t>& pristine, const Damage& y, const std::size_t e, const std::size_t r)
{
    std::size_t count{0};

    for (const auto b : pristine)
    {
        std::ptrdiff_t image{};

        if (b < y.low)
        {
            image = static_cast<std::ptrdiff_t>(b);
        }
        else if (b >= y.cut)
        {
            image = static_cast<std::ptrdiff_t>(b) + y.shift;
        }
        else
        {
            continue;
        }

        if (image > static_cast<std::ptrdiff_t>(e) && image < static_cast<std::ptrdiff_t>(r))
        {
            ++count;
        }
    }

    return count;
}

/**
 * @brief One resume convention: a name and a rule producing a resume position from the failure offset.
 */
struct Strategy
{
    std::string_view name;

    std::function<std::optional<std::size_t>(const std::string_view, const std::size_t)> resume;

    /// The known-clean-end arm: the walk starts at the corruption end or one past the failure,
    /// whichever is later, modeling a caller (an editor knows its edit span) told the damage's extent.
    bool clean{false};
};

/**
 * @brief The position one past the next occurrence of the delimiter at or after from, the classical
 *        discard-through-the-delimiter convention.
 */
std::optional<std::size_t> past_next(const std::string_view input, const std::size_t from, const char delimiter)
{
    const auto at{input.find(delimiter, from)};

    if (at == std::string_view::npos || at + 1 >= input.size())
    {
        return std::nullopt;
    }

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

struct Cell
{
    std::size_t trials{0};

    std::size_t answers{0};

    std::size_t refusals{0};

    std::size_t landings{0};

    std::ptrdiff_t overshoot_sum{0};

    std::size_t overshoot_count{0};

    std::size_t lost_sum{0};

    std::size_t cascade_sum{0};

    std::size_t cascade_count{0};
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
        const auto from{random.next() * (row.corpus.size() - 1) / 0x7fffU};

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

} // namespace

int main(const int argc, const char** argv)
{
    const std::size_t corpus_kib{argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 64};

    const std::size_t trials{argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 60};

    const char* csv_path{argc > 3 ? argv[3] : nullptr};

    const char* real_path{argc > 4 ? argv[4] : nullptr};

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

    // The end-of-input asymmetry, constructed rather than awaited: on an input whose last byte is the
    // delimiter, the at placement answers at that final byte while the past placement refuses, the one
    // availability difference the paired regression's opportunistic branch guards. No archived campaign
    // trial ever reached it, since damage sampling keeps clear of both corpus edges, so this fixture
    // forces the branch's premise directly.
    {
        constexpr std::string_view final_newline{"aa\n"};

        if (past_next(final_newline, 1, '\n') || at_next(final_newline, 1, '\n') != std::optional<std::size_t>{2})
        {
            std::printf("FAILED: the final-delimiter fixture broke\n");

            return 1;
        }
    }

    std::printf(
            "deterministic: corpus seeds 0x5eed0001 through 0x5eed0003, schedule seed 0x5eedc0de and payload seed "
            "0x5eedbeef each offset per (op, k)\n");

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
        }
    }

    std::FILE* csv{csv_path ? std::fopen(csv_path, "w") : nullptr};

    // One raw row per strategy per damaging trial, plus one row marking each trial the grammar absorbed, so any
    // statistic stays computable from the archive; overshoot is blank where no post-corruption boundary exists.
    if (csv)
    {
        std::fprintf(
                csv,
                "grammar,op,k,trial,p,failure_offset,corruption_end,strategy,answer,landed,overshoot,lost,cascade,"
                "evidence,minimal,kind,length,origin\n");
    }

    constexpr std::array<Op, 3> ops{Op::Substitute, Op::Delete, Op::Insert};

    constexpr std::array<std::size_t, 3> ks{1, 4, 16};

    std::size_t theorem_failures{0};

    std::size_t evidence_covered{0};

    std::size_t evidence_uncovered{0};

    std::size_t evidence_uncovered_landed{0};

    std::size_t nonminimal_answers{0};

    std::size_t nonminimal_bytes{0};

    std::size_t clean_answers{0};

    std::size_t clean_refusals{0};

    std::size_t absorbed_total{0};

    for (const auto& row : rows)
    {
        std::printf("\n%s\n", std::string{row.label}.c_str());

        std::printf(
                "  %-11s %2s  %-10s %8s %8s %9s %10s %7s %9s\n", "op", "k", "strategy", "answers", "refuse", "landing",
                "overshoot", "lost", "cascade");

        const std::array<Strategy, 7> strategies{
                Strategy{
                        .name = "certified",
                        .resume =
                                [&row](const std::string_view input, const std::size_t from) {
                                    return row.lexer.next_certified_start(input, from);
                                }},
                Strategy{
                        .name = "certified-clean",
                        .resume =
                                [&row](const std::string_view input, const std::size_t from) {
                                    return row.lexer.next_certified_start(input, from);
                                },
                        .clean = true},
                Strategy{
                        .name = "skip-one",
                        .resume = [](const std::string_view input,
                                     const std::size_t from) -> std::optional<std::size_t> {
                            if (from >= input.size())
                            {
                                return std::nullopt;
                            }

                            return from;
                        }},
                Strategy{
                        .name = "newline",
                        .resume = [](const std::string_view input,
                                     const std::size_t from) { return past_next(input, from, '\n'); }},
                Strategy{
                        .name = "newline-at",
                        .resume = [](const std::string_view input,
                                     const std::size_t from) { return at_next(input, from, '\n'); }},
                Strategy{
                        .name = "semicolon",
                        .resume = [](const std::string_view input,
                                     const std::size_t from) { return past_next(input, from, ';'); }},
                Strategy{
                        .name = "semicolon-at",
                        .resume = [](const std::string_view input,
                                     const std::size_t from) { return at_next(input, from, ';'); }},
        };

        for (const auto op : ops)
        {
            for (const auto k : ks)
            {
                std::array<Cell, 7> cells{};

                Lcg positions{0x5eedc0deU + static_cast<unsigned>(k) * 7U + static_cast<unsigned>(op) * 131U};

                Lcg payload{0x5eedbeefU + static_cast<unsigned>(k) * 7U + static_cast<unsigned>(op) * 131U};

                std::size_t absorbed{0};

                for (std::size_t trial{0}; trial < trials; ++trial)
                {
                    const auto span{row.corpus.size() - k - 128};

                    const auto p{64 + (static_cast<std::size_t>(positions.next()) * 48271U) % span};

                    auto y{damage(row.corpus, op, p, k, payload)};

                    const auto e{failure_offset(row.lexer, y.input)};

                    if (e == y.input.size())
                    {
                        ++absorbed;

                        if (csv)
                        {
                            std::fprintf(
                                    csv, "%s,%s,%zu,%zu,%zu,,%zu,absorbed,,,,,,,,,,\n", std::string{row.label}.c_str(),
                                    std::string{name(op)}.c_str(), k, trial, p, y.end);
                        }

                        continue;
                    }

                    const auto first{first_true_boundary(row.begins, y)};

                    std::array<std::optional<std::size_t>, 7> got{};

                    for (std::size_t s{0}; s < strategies.size(); ++s)
                    {
                        auto& cell{cells[s]};

                        ++cell.trials;

                        const auto start{strategies[s].clean ? std::max(e + 1, y.end) : e + 1};

                        const auto r{strategies[s].resume(y.input, start)};

                        got[s] = r;

                        if (!r)
                        {
                            ++cell.refusals;

                            if (strategies[s].clean)
                            {
                                ++clean_refusals;
                            }

                            if (csv)
                            {
                                std::fprintf(
                                        csv, "%s,%s,%zu,%zu,%zu,%zu,%zu,%s,,,,,,,,,,\n", std::string{row.label}.c_str(),
                                        std::string{name(op)}.c_str(), k, trial, p, e, y.end,
                                        std::string{strategies[s].name}.c_str());
                            }

                            continue;
                        }

                        ++cell.answers;

                        const auto did_land{landed(row.begins, y, *r)};

                        cell.landings += did_land ? 1 : 0;

                        // Support-aware classification: replicate the walk's evidence and assert the sharp
                        // form of the transfer, a certified answer whose evidence clears the corruption end
                        // must land; the conservative end-plus-three assertion stays below.
                        std::optional<Evidence> evidence;

                        auto minimal{*r};

                        if (s == 0 || strategies[s].clean)
                        {
                            evidence = evidence_of(row.lexer, y.input, start, *r);

                            if (!evidence)
                            {
                                std::fprintf(
                                        stderr, "EVIDENCE MISMATCH: %s %s k=%zu p=%zu e=%zu answered %zu\n",
                                        std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e, *r);

                                ++theorem_failures;
                            }
                            else if (s == 0)
                            {
                                minimal = minimal_answer(row.lexer, y.input, start, *r);

                                if (minimal < *r)
                                {
                                    ++nonminimal_answers;

                                    nonminimal_bytes += *r - minimal;
                                }

                                if (evidence->begin >= y.end)
                                {
                                    ++evidence_covered;

                                    if (!did_land)
                                    {
                                        std::fprintf(
                                                stderr, "EVIDENCE VIOLATION: %s %s k=%zu p=%zu e=%zu answered %zu\n",
                                                std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e,
                                                *r);

                                        ++theorem_failures;
                                    }
                                }
                                else
                                {
                                    ++evidence_uncovered;

                                    if (did_land)
                                    {
                                        ++evidence_uncovered_landed;
                                    }
                                }
                            }
                            else
                            {
                                // The clean arm's contract is total: the search began at or past the
                                // corruption end, so the evidence is covered by construction and the
                                // answer must land, both asserted on every trial.
                                minimal = minimal_answer(row.lexer, y.input, start, *r);

                                ++clean_answers;

                                if (evidence->begin < y.end || !did_land)
                                {
                                    std::fprintf(
                                            stderr, "CLEAN-ARM VIOLATION: %s %s k=%zu p=%zu e=%zu answered %zu\n",
                                            std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e, *r);

                                    ++theorem_failures;
                                }
                            }
                        }

                        // The theorem transfer: a certified answer clear of the seam band must land, because the
                        // pristine corpus is itself a tokenizable repair of the preserved suffix.
                        if (s == 0 && *r >= y.end + 3 && !did_land)
                        {
                            std::fprintf(
                                    stderr, "THEOREM VIOLATION: %s %s k=%zu p=%zu e=%zu answered %zu\n",
                                    std::string{row.label}.c_str(), std::string{name(op)}.c_str(), k, p, e, *r);

                            ++theorem_failures;
                        }

                        if (first)
                        {
                            cell.overshoot_sum += static_cast<std::ptrdiff_t>(*r) - static_cast<std::ptrdiff_t>(*first);

                            ++cell.overshoot_count;
                        }

                        const auto lost{lost_starts(row.begins, y, e, *r)};

                        cell.lost_sum += lost;

                        // The driver loop of the progress lemma: alternate scan and resume until the end or a
                        // refusal, counting resume events; the lemma bounds the loop by the input's size.
                        std::size_t events{0};

                        std::size_t at{*r};

                        ++events;

                        while (at < y.input.size() && events <= y.input.size())
                        {
                            const std::string_view suffix{y.input.data() + at, y.input.size() - at};

                            const auto consumed{failure_offset(row.lexer, suffix)};

                            if (consumed == suffix.size())
                            {
                                break;
                            }

                            const auto again{strategies[s].resume(y.input, at + consumed + 1)};

                            if (!again || *again <= at + consumed)
                            {
                                break;
                            }

                            at = *again;

                            ++events;
                        }

                        cell.cascade_sum += events;

                        ++cell.cascade_count;

                        if (csv)
                        {
                            std::fprintf(
                                    csv, "%s,%s,%zu,%zu,%zu,%zu,%zu,%s,%zu,%d,", std::string{row.label}.c_str(),
                                    std::string{name(op)}.c_str(), k, trial, p, e, y.end,
                                    std::string{strategies[s].name}.c_str(), *r, did_land ? 1 : 0);

                            if (first)
                            {
                                std::fprintf(
                                        csv, "%td",
                                        static_cast<std::ptrdiff_t>(*r) - static_cast<std::ptrdiff_t>(*first));
                            }

                            std::fprintf(csv, ",%zu,%zu", lost, events);

                            if (evidence)
                            {
                                std::fprintf(
                                        csv, ",%zu,%zu,%s,%zu,%zu\n", evidence->begin, minimal,
                                        evidence->byte ? "byte" : "window", evidence->length, evidence->origin);
                            }
                            else
                            {
                                std::fprintf(csv, ",,,,,\n");
                            }
                        }
                    }

                    // The two delimiter placements must differ by exactly the delimiter,
                    // and interchanging them fails here, the regression a referee asked
                    // for: the after form refuses where the delimiter is the input's last
                    // byte, and the at form still answers there.
                    for (const auto& [after_s, at_s, delimiter] :
                         {std::tuple<std::size_t, std::size_t, char>{3, 4, '\n'},
                          std::tuple<std::size_t, std::size_t, char>{5, 6, ';'}})
                    {
                        if (got[after_s])
                        {
                            if (!got[at_s] || *got[at_s] + 1 != *got[after_s] || y.input[*got[at_s]] != delimiter)
                            {
                                std::fprintf(stderr, "CONVENTION VIOLATION\n");

                                ++theorem_failures;
                            }
                        }
                        else if (got[at_s] && *got[at_s] + 1 < y.input.size())
                        {
                            std::fprintf(stderr, "CONVENTION VIOLATION\n");

                            ++theorem_failures;
                        }
                    }
                }

                absorbed_total += absorbed;

                for (std::size_t s{0}; s < strategies.size(); ++s)
                {
                    const auto& cell{cells[s]};

                    const auto landing{
                            cell.answers ?
                                    100.0 * static_cast<double>(cell.landings) / static_cast<double>(cell.answers) :
                                    0.0};

                    const auto overshoot{
                            cell.overshoot_count ? static_cast<double>(cell.overshoot_sum) /
                                                           static_cast<double>(cell.overshoot_count) :
                                                   0.0};

                    const auto lost{
                            cell.answers ? static_cast<double>(cell.lost_sum) / static_cast<double>(cell.answers) :
                                           0.0};

                    const auto cascade{
                            cell.cascade_count ?
                                    static_cast<double>(cell.cascade_sum) / static_cast<double>(cell.cascade_count) :
                                    0.0};

                    std::printf(
                            "  %-11s %2zu  %-10s %8zu %8zu %8.1f%% %10.1f %7.1f %9.2f\n", std::string{name(op)}.c_str(),
                            k, std::string{strategies[s].name}.c_str(), cell.answers, cell.refusals, landing, overshoot,
                            lost, cascade);
                }
            }
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
            "evidence-covered answers: %zu, all asserted to land; evidence-uncovered: %zu, of which %zu landed\n",
            evidence_covered, evidence_uncovered, evidence_uncovered_landed);

    std::printf("nonminimal answers: %zu, %zu extra bytes in total\n", nonminimal_answers, nonminimal_bytes);

    std::printf(
            "known-clean arm: %zu answers, every one asserted covered and landed; %zu refusals\n", clean_answers,
            clean_refusals);

    if (oracle_failures != 0 || theorem_failures != 0)
    {
        std::printf("FAILED: %zu oracle violations, %zu theorem violations\n", oracle_failures, theorem_failures);

        return EXIT_FAILURE;
    }

    std::printf("all oracle and theorem assertions held\n");

    return EXIT_SUCCESS;
}
