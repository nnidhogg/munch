// Measures the quality of certified error recovery against the classical panic-mode conventions, on the grammar rows
// the split-points study established, under a corruption model whose ground truth is exact by construction.
//
// Usage: munch_recovery_quality [corpus KiB] [trials per cell] [csv path] [real json corpus path] [seeds], every
// argument optional: 64 KiB, 60 trials and 3 seeds by default, and an empty path standing for none. The registered test
// runs `16 20 "" "" 1`; the campaign runs `512 500 out.csv twitter.json 3`. The real JSON document adds an ecological
// row, read verbatim and held to the same complete tokenizability assertion, damage protocol and oracle as the
// generated rows; each seed is a separate schedule of positions and payloads, salted per row, so no two rows share a
// stream. Every count must be a positive whole decimal number, and every corpus long enough for the widest damage and
// short enough for the position sampler's thirty-two-bit spans; anything else is refused with a diagnostic. It exits 0
// when every oracle and theorem assertion holds and every write succeeds, 1 otherwise.
//
// The streams are recovery_lcg, the generated corpora recovery_corpora, the damage and its coordinate map
// recovery_damage, the arms and their driver recovery_arms, the mapped pristine oracle recovery_oracle, the per-trial
// assertions recovery_checks, the archive recovery_archive and the tables recovery_report.
//
// The question. next_certified_start() returns positions with a soundness theorem: in every tokenizable repair of the
// input before an anchor, the image of a certified position begins a token. The library also ships the anchored
// machinery, next_anchored_start() and minimal_repair(), exact for the complete-repair predicate; the walk's soundness
// binds the larger class of repairs whose scans commit through the preserved evidence. The classical conventions, skip
// one byte, skip to a delimiter raw or token-aware, promise nothing. This probe drives every arm through completed
// incidents under one stopping rule and quantifies each against the same oracle: where the first answer lands, where
// the terminal one does, whether the incident completes, refuses, or exhausts its budget, how fast the resumed stream
// converges to the mapped pristine one, and how the walk's answers stratify by whether any repair exists at its anchor
// at all.
//
// Corruption model. A pristine corpus x, completely tokenizable by its row's grammar and asserted so, is damaged at a
// position p by one of three operations on k bytes: substitute k bytes with pseudo-random ones, delete k bytes, or
// insert k pseudo-random bytes. Every trial is deterministic: independently seeded linear congruential generators cover
// the corpus, the damage positions, and the damage payloads, so every run of this program performs the identical
// experiment.
//
// Ground truth. The token boundaries of the damaged input's own segmentation past the seam are unknowable without a
// repair oracle, so the study uses the seed note's definition: ground truth is the boundary set B of the pristine
// corpus, mapped into damaged coordinates. Each operation leaves a suffix of x intact, y[end..] equals x[c..] for the
// corruption end named below, and boundaries inside the damaged window have no image and are dropped:
//
//   substitute: y agrees with x outside [p, p+k); end = p + k, images shift by 0.
//   delete:     y[p..] equals x[p+k..);       end = p,     images past the cut shift by -k.
//   insert:     y[p+k..] equals x[p..);       end = p + k, images past the seam shift by +k.
//
// A resume position counts as landed when it is the image of a pristine boundary outside the damaged window. Near the
// seam the damaged input's true segmentation can genuinely diverge from the mapped pristine one; the same conservative
// oracle is applied uniformly to every strategy, though their near-seam exposure differs.
//
// The theorem gives teeth. The pristine corpus is itself a tokenizable repair of the damaged suffix: x equals x[0..c)
// concatenated with x[c..], the very suffix y preserves. So the repair-invariance theorems force any certified answer
// whose supporting occurrence lies wholly in the preserved suffix to map to a boundary of B. The occurrence begins at
// most three bytes before the answer, the longest window is four bytes with origin at most three, so every certified
// answer at or past end + 3 must land, and the harness asserts exactly that, failing the run on any violation. Answers
// in the seam band [end, end + 3) may rest on an occurrence straddling the seam, where the theorems bind only repairs
// that preserve the straddling evidence and predict nothing about the mapped-pristine oracle; they are measured, not
// asserted. A second hard assertion runs before any corruption: on the pristine corpus, every next_certified_start()
// answer from sampled offsets must be a boundary of B, the same oracle discipline the split-points report uses.
//
// Eleven arms share the completed-incident driver. certified is the evidence-order walk, its answers carrying the
// library's evidence interval, cross-checked on every first move against a replica of the walk coded apart from the
// library's search but running over its split-point and split-window predicates. The replica walks from one past the
// failure for the first certificate itself, never handed the library's answer, and the two are compared on existence
// first, so a library refusing where a certificate exists fails the run, and then on the answer position, both ends of
// the evidence and the byte-or-window class, the diagnostic naming the field that differed, so a defect in the walk's
// order or traversal is caught while one in the predicates would not be; every certified move's evidence is recorded
// and every covered move is asserted to land, not only the first per incident. certified-clean starts the walk at the
// corruption end or one past the failure, whichever is later, the oracle arm whose every answer is asserted covered and
// landed. exact is the anchored procedure over the library's complete-repair-invariance decider, the anchor advancing
// past a beyond-repair tail's poison until a certificate holds; the decider's direct answer at the blind anchor is
// archived separately per trial, and the cross-arm regressions test that direct call, never the advancing procedure.
// exact-clean anchors at the corruption end, its answers asserted to land since the pristine prefix is a repair of what
// precedes the preserved suffix. skip-one and the four raw delimiter placements are the classical conventions, the past
// placement returning the end-of-input offset at a final delimiter rather than refusing. token-newline and
// token-semicolon are the token-aware reading, synchronizing on a designated token: the delimiter's own punctuation
// token exactly, or an all-whitespace token carrying the newline, so a string or comment that merely contains the
// delimiter byte never synchronizes.
//
// Repairability stratifies every damaging trial: minimal_repair() at the blind anchor reports whether any completely
// tokenizable repair exists, every returned repair witness-verified by scanning repair plus tail to the end of input,
// and the summary counts the walk's answers on unrepairable tails apart. Two consistency regressions bind the routines
// at the blind anchor itself: on a repairable trial a walk answer implies a direct decider answer at or before it, and
// on an unrepairable trial the direct call must refuse; the two routines share their scenario machinery, so this is
// consistency, not independent proof. The sharper transfer assertion fires on the exact precondition: a certified
// answer whose evidence begins at or past the corruption end must land, asserted for every move; the conservative
// end-plus-three assertion stays beside it, and the summary reports covered and uncovered tallies with the
// nonminimality figure. The generated corpora are written beside the archive, as <archive path>.corpus-<slug>.bin, so
// the aggregate columns recompute from the archive alone and two campaigns in one directory keep their own; the
// mapped-oracle columns need this pinned source tree as well, the boundary oracle and the lexer being live machinery.
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
// The summary closes with Wilson 95 percent intervals on pooled first landing and completion per arm, the repairability
// tallies with the vacuous share, the exact arm's byte savings on repairable trials beside its signed net displacement
// over all pairs, and the duplicate count of the rejection-sampled positions.
//
// Baseline conventions. All arms search from e + 1 after every failure, the same progress contract recover() keeps, so
// no arm may retry the offending byte; the oracle arms floor their search at the corruption end. The driver alternates
// scan and recover under one stopping rule for every arm: end of input, refusal, or budget.
//
// Non-claims. Certified recovery answers with the first certificate in walk order, not the closest boundary, and
// refuses where nothing certifies; both behaviors are measured here, not excused. Nothing is claimed about the damaged
// input's own segmentation between the failure and the resume position. Printed figures are quotable only beside the
// clean commit of the collection ritual, exactly as the benchmark's are.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/tools/probes/files.hpp"
#include "munch/tools/probes/recovery_archive.hpp"
#include "munch/tools/probes/recovery_arms.hpp"
#include "munch/tools/probes/recovery_checks.hpp"
#include "munch/tools/probes/recovery_corpora.hpp"
#include "munch/tools/probes/recovery_damage.hpp"
#include "munch/tools/probes/recovery_lcg.hpp"
#include "munch/tools/probes/recovery_oracle.hpp"
#include "munch/tools/probes/recovery_report.hpp"
#include "munch/tools/probes/study_rows.hpp"

namespace
{
using figures::Token;
using munch::core::Builder;
using munch::tools::probes::Archive;
using munch::tools::probes::boundaries;
using munch::tools::probes::c_like_corpus;
using munch::tools::probes::C_like_features;
using munch::tools::probes::Campaign_totals;
using munch::tools::probes::Cell;
using munch::tools::probes::check_trial;
using munch::tools::probes::conventional_row;
using munch::tools::probes::damage;
using munch::tools::probes::Damage;
using munch::tools::probes::failure_offset;
using munch::tools::probes::first_true_boundary;
using munch::tools::probes::Incident;
using munch::tools::probes::json_corpus;
using munch::tools::probes::kArms;
using munch::tools::probes::kAttemptBudget;
using munch::tools::probes::kOps;
using munch::tools::probes::kWidths;
using munch::tools::probes::Lcg;
using munch::tools::probes::print_pooled;
using munch::tools::probes::print_seeds;
using munch::tools::probes::print_strata;
using munch::tools::probes::print_totals;
using munch::tools::probes::pristine_oracle;
using munch::tools::probes::read_bytes;
using munch::tools::probes::Row;
using munch::tools::probes::Row_tallies;
using munch::tools::probes::row_tallies;
using munch::tools::probes::run_incident;
using munch::tools::probes::score_of;
using munch::tools::probes::split_friendly_conventional_row;
using munch::tools::probes::tally_incident;
using munch::tools::probes::Trial;

/**
 * @brief The shortest corpus the campaign accepts: the widest damage plus the position sampler's margins of 64 bytes
 *        on each side, plus one.
 */
constexpr std::size_t kShortestCorpus{kWidths.back() + 128 + 1};

/**
 * @brief The widest span the 32-bit position samplers draw from; a corpus is at most one byte longer.
 */
constexpr std::size_t kWidestSpan{std::numeric_limits<std::uint32_t>::max()};

/**
 * @brief The pristine oracle's samples per row.
 */
constexpr std::size_t kOracleSamples{512};

/**
 * @brief The campaign's command line.
 */
struct Arguments
{
    /**
     * @brief The generated corpora's size in KiB.
     */
    std::size_t corpus_kib{64};

    /**
     * @brief The trials per cell.
     */
    std::size_t trials{60};

    /**
     * @brief The archive's path, std::nullopt for no archive.
     */
    std::optional<std::string_view> csv_path{};

    /**
     * @brief The real JSON document's path, std::nullopt for no real row.
     */
    std::optional<std::string_view> real_path{};

    /**
     * @brief The independent seeds.
     */
    std::size_t seeds{3};
};

/**
 * @brief A cell's two streams and the damage positions drawn from it so far.
 */
struct Cell_streams
{
    /**
     * @brief The schedule stream the damage positions are drawn from, seeded from 0x5eedc0de.
     */
    Lcg positions;

    /**
     * @brief The payload stream the damage bytes are drawn from, seeded from 0x5eedbeef.
     */
    Lcg payload;

    /**
     * @brief The positions drawn so far.
     */
    std::unordered_set<std::size_t> seen{};
};

/**
 * @brief Builds a damaging trial: the first true boundary, the blind tail's repair and the decider's direct answer at
 *        its anchor, one past the failure, and every arm's incident.
 * @param cell The trial's cell.
 * @param index The trial's index within its cell.
 * @param position The damage position.
 * @param failure The serial scan's failure offset on the damaged input, below its size.
 * @param damaged The damaged input.
 * @return The trial.
 */
Trial damaging_trial(
        const Cell& cell, const std::size_t index, const std::size_t position, const std::size_t failure,
        Damage damaged)
{
    const auto& row{cell.row};

    const auto first_true{first_true_boundary(row.begins, damaged)};

    const auto anchor{std::min(failure + 1, damaged.input.size())};

    const auto tail{std::string_view{damaged.input}.substr(anchor)};

    auto repair{row.lexer.minimal_repair(tail)};

    const auto found{row.lexer.next_anchored_start(tail, 0)};

    const auto direct{found ? std::optional{anchor + *found} : std::nullopt};

    std::array<Incident, kArms.size()> incidents{};

    for (std::size_t arm_index{0}; arm_index < kArms.size(); ++arm_index)
    {
        incidents[arm_index] =
                run_incident(row.lexer, damaged.input, failure, damaged.end, kArms[arm_index], kAttemptBudget);
    }

    return Trial{
            .cell = cell,
            .index = index,
            .position = position,
            .failure = failure,
            .damaged = std::move(damaged),
            .first_true = first_true,
            .repair = std::move(repair),
            .direct = direct,
            .incidents = std::move(incidents)};
}

/**
 * @brief Runs one trial of a cell: draws its position, then its damage, and either archives it as absorbed or checks
 *        it, tallies every arm's incident and archives each arm's row and moves, arm by arm.
 * @param cell The cell.
 * @param index The trial's index within the cell.
 * @param streams The cell's streams, drawn once for the position and then for the payload.
 * @param totals The campaign's totals.
 * @param tallies The row's tallies.
 * @param archive The archive.
 */
void run_trial(
        const Cell& cell, const std::size_t index, Cell_streams& streams, Campaign_totals& totals, Row_tallies& tallies,
        Archive& archive)
{
    const auto& row{cell.row};

    const auto span{row.corpus.size() - cell.k - 128};

    const auto position{64 + static_cast<std::size_t>(streams.positions.bounded(static_cast<std::uint32_t>(span)))};

    if (!streams.seen.insert(position).second)
    {
        ++totals.duplicate_positions;
    }

    auto damaged{damage(row.corpus, cell.op, position, cell.k, streams.payload)};

    const auto failure{failure_offset(row.lexer, damaged.input)};

    if (failure == damaged.input.size())
    {
        ++totals.absorbed_total;

        archive.absorbed_row(cell, index, position, damaged.end);

        return;
    }

    const auto trial{damaging_trial(cell, index, position, failure, std::move(damaged))};

    check_trial(trial, totals);

    for (std::size_t arm_index{0}; arm_index < kArms.size(); ++arm_index)
    {
        const auto& incident{trial.incidents[arm_index]};

        const auto score{score_of(row, trial.damaged, incident)};

        tally_incident(tallies, trial, arm_index, incident, score);

        archive.incident_row(trial, kArms[arm_index], incident, score);

        archive.move_rows(trial, kArms[arm_index], incident);
    }
}

/**
 * @brief A cell's stream seed: a base salted by the seed index, the row index, the width and the operation, so no two
 *        cells of a campaign share a stream.
 * @param base The stream's base seed.
 * @param cell The cell.
 * @return base + 0x01000193 seed + 0x9e3779b9 row + 7 k + 131 op, modulo 2^32.
 */
std::uint32_t cell_seed(const std::uint32_t base, const Cell& cell)
{
    return base + static_cast<std::uint32_t>(cell.seed) * 0x01000193U +
           static_cast<std::uint32_t>(cell.row_index) * 0x9e3779b9U + static_cast<std::uint32_t>(cell.k) * 7U +
           static_cast<std::uint32_t>(cell.op) * 131U;
}

/**
 * @brief Builds a row over its corpus: the lexer, then the corpus's boundaries.
 * @param label The row's label.
 * @param builder The row's grammar.
 * @param corpus The pristine corpus, which must tokenize completely.
 * @param generated Whether the corpus was generated.
 * @return The row.
 */
Row row_of(const std::string_view label, const Builder& builder, std::string corpus, const bool generated)
{
    auto lexer{builder.build()};

    auto begins{boundaries(lexer, corpus)};

    return Row{
            .label = label,
            .lexer = std::move(lexer),
            .corpus = std::move(corpus),
            .begins = std::move(begins),
            .generated = generated};
}

/**
 * @brief Adds the conventional C-like base with block comments alone.
 * @param builder The builder the row's tokens are added to.
 */
void block_comments_row(Builder& builder)
{
    figures::c_like(builder, false);

    builder.add_token(figures::block_comment(), Token::BlockComment, 1);
}

/**
 * @brief Adds the bare conventional C-like base: identifiers, numbers, operators and punctuation.
 * @param builder The builder the row's tokens are added to.
 */
void bare_row(Builder& builder)
{
    figures::c_like(builder, false);
}

/**
 * @brief Adds the RFC 8259 JSON lexical forms.
 * @param builder The builder the row's tokens are added to.
 */
void json_row(Builder& builder)
{
    figures::json(builder);
}

/**
 * @brief One generated row: its label, its grammar and the corpus it runs on.
 */
struct Row_recipe
{
    /**
     * @brief The row's label.
     */
    std::string_view label;

    /**
     * @brief Adds the row's tokens to a builder.
     */
    void (&add_tokens)(Builder&);

    /**
     * @brief The C-like corpus's token families, std::nullopt for the JSON corpus.
     */
    std::optional<C_like_features> c_like;
};

/**
 * @brief The five generated rows, in the order the campaign runs them.
 */
constexpr std::array<Row_recipe, 5> kRecipes{
        Row_recipe{
                .label = "c-like conventional with strings and line comments",
                .add_tokens = conventional_row,
                .c_like = C_like_features{.strings = true, .line_comments = true}},
        Row_recipe{
                .label = "c-like conventional plus block comments alone",
                .add_tokens = block_comments_row,
                .c_like = C_like_features{.block_comments = true}},
        Row_recipe{.label = "json rfc 8259 lexical forms", .add_tokens = json_row, .c_like = std::nullopt},
        Row_recipe{
                .label = "c-like split-friendly with strings and line comments",
                .add_tokens = split_friendly_conventional_row,
                .c_like = C_like_features{.strings = true, .line_comments = true}},
        Row_recipe{
                .label = "c-like bare: identifiers numbers operators punctuation",
                .add_tokens = bare_row,
                .c_like = C_like_features{}},
};

/**
 * @brief Runs every trial of one cell from its own two streams.
 * @param cell The cell.
 * @param trials The trials per cell.
 * @param totals The campaign's totals.
 * @param tallies The row's tallies.
 * @param archive The archive.
 */
void run_cell(
        const Cell& cell, const std::size_t trials, Campaign_totals& totals, Row_tallies& tallies, Archive& archive)
{
    Cell_streams streams{.positions = Lcg{cell_seed(0x5eedc0deU, cell)}, .payload = Lcg{cell_seed(0x5eedbeefU, cell)}};

    for (std::size_t index{0}; index < trials; ++index)
    {
        run_trial(cell, index, streams, totals, tallies, archive);
    }
}

/**
 * @brief Reads a positive whole decimal count, refusing a text that is not wholly one, a zero, and a value past the
 *        range of std::size_t.
 * @param text The argument.
 * @return The count, std::nullopt for a refusal.
 */
std::optional<std::size_t> positive_count(const std::string_view text)
{
    std::size_t value{0};

    const auto parsed{std::from_chars(text.data(), text.data() + text.size(), value)};

    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
    {
        return std::nullopt;
    }

    return value;
}

/**
 * @brief Builds the five generated rows, each grammar before its corpus.
 * @param bytes The corpora's size.
 * @return The rows, in kRecipes order.
 */
std::vector<Row> generated_rows(const std::size_t bytes)
{
    std::vector<Row> rows{};

    for (const auto& recipe : kRecipes)
    {
        Builder builder{};

        recipe.add_tokens(builder);

        auto corpus{recipe.c_like ? c_like_corpus(bytes, *recipe.c_like) : json_corpus(bytes)};

        rows.push_back(row_of(recipe.label, builder, std::move(corpus), true));
    }

    return rows;
}

/**
 * @brief Builds the real row over a JSON document read verbatim; a document that cannot be read or is empty is refused
 *        with `real corpus unreadable or empty: <path>` on standard error.
 * @param path The document's path.
 * @return The row, std::nullopt after a refusal.
 */
std::optional<Row> real_row(const std::string_view path)
{
    Builder builder{};

    json_row(builder);

    auto corpus{read_bytes(std::filesystem::path{path})};

    if (!corpus || corpus->empty())
    {
        std::fprintf(stderr, "real corpus unreadable or empty: %s\n", std::string{path}.c_str());

        return std::nullopt;
    }

    return row_of("json rfc 8259 lexical forms on a real-world document", builder, std::move(*corpus), false);
}

/**
 * @brief Whether every row's corpus is long enough for the widest damage and short enough for the position samplers;
 *        the first that is not is reported with `corpus outside the harness's lengths` on standard error.
 * @param rows The rows.
 * @return True when every corpus holds from kShortestCorpus to kWidestSpan + 1 bytes.
 */
bool is_within_lengths(const std::vector<Row>& rows)
{
    for (const auto& row : rows)
    {
        if (row.corpus.size() < kShortestCorpus || row.corpus.size() - 1 > kWidestSpan)
        {
            std::fprintf(
                    stderr, "corpus outside the harness's lengths: %s, %zu bytes, at least %zu and at most %zu\n",
                    std::string{row.label}.c_str(), row.corpus.size(), kShortestCorpus, kWidestSpan + 1);

            return false;
        }
    }

    return true;
}

/**
 * @brief Runs one seed of a row: every operation and, within it, every width, each cell from its own streams.
 * @param row The row.
 * @param row_index The row's index among the rows.
 * @param seed The seed index.
 * @param trials The trials per cell.
 * @param totals The campaign's totals.
 * @param tallies The row's tallies.
 * @param archive The archive.
 */
void run_seed(
        const Row& row, const std::size_t row_index, const std::size_t seed, const std::size_t trials,
        Campaign_totals& totals, Row_tallies& tallies, Archive& archive)
{
    for (std::size_t op_index{0}; op_index < kOps.size(); ++op_index)
    {
        for (std::size_t k_index{0}; k_index < kWidths.size(); ++k_index)
        {
            const Cell cell{
                    .row = row,
                    .row_index = row_index,
                    .op = kOps[op_index],
                    .op_index = op_index,
                    .k = kWidths[k_index],
                    .k_index = k_index,
                    .seed = seed};

            run_cell(cell, trials, totals, tallies, archive);
        }
    }
}

/**
 * @brief Reads the command line, refusing the counts in argument order with `<count> must be a positive whole number:
 *        <argument>` on standard error; an argument past the fifth is ignored.
 * @param command_line The arguments after the program's name.
 * @return The arguments, std::nullopt after a refusal.
 */
std::optional<Arguments> arguments_of(const std::vector<std::string_view>& command_line)
{
    Arguments arguments{};

    if (command_line.size() > 0)
    {
        const auto kib{positive_count(command_line[0])};

        if (!kib)
        {
            std::fprintf(
                    stderr, "corpus KiB must be a positive whole number: %s\n", std::string{command_line[0]}.c_str());

            return std::nullopt;
        }

        arguments.corpus_kib = *kib;
    }

    if (command_line.size() > 1)
    {
        const auto trials{positive_count(command_line[1])};

        if (!trials)
        {
            std::fprintf(
                    stderr, "trials per cell must be a positive whole number: %s\n",
                    std::string{command_line[1]}.c_str());

            return std::nullopt;
        }

        arguments.trials = *trials;
    }

    if (command_line.size() > 2 && !command_line[2].empty())
    {
        arguments.csv_path = command_line[2];
    }

    if (command_line.size() > 3 && !command_line[3].empty())
    {
        arguments.real_path = command_line[3];
    }

    if (command_line.size() > 4)
    {
        const auto seeds{positive_count(command_line[4])};

        if (!seeds)
        {
            std::fprintf(stderr, "seeds must be a positive whole number: %s\n", std::string{command_line[4]}.c_str());

            return std::nullopt;
        }

        arguments.seeds = *seeds;
    }

    return arguments;
}

/**
 * @brief Builds the campaign's rows: the five generated ones, then the real one when a document is named, every
 *        corpus held to the harness's lengths.
 * @param arguments The command line.
 * @return The rows, std::nullopt after a refusal.
 */
std::optional<std::vector<Row>> rows_of(const Arguments& arguments)
{
    auto rows{generated_rows(arguments.corpus_kib << 10U)};

    if (arguments.real_path)
    {
        auto real{real_row(*arguments.real_path)};

        if (!real)
        {
            return std::nullopt;
        }

        rows.push_back(std::move(*real));
    }

    if (!is_within_lengths(rows))
    {
        return std::nullopt;
    }

    return rows;
}

/**
 * @brief Runs the pristine oracle over every row, then prints its line and the campaign's stream seeds.
 * @param rows The rows.
 * @param seeds The independent seeds.
 * @return The oracle's violations over all rows.
 */
std::size_t pristine_violations(const std::vector<Row>& rows, const std::size_t seeds)
{
    std::size_t failures{0};

    for (const auto& row : rows)
    {
        failures += pristine_oracle(row, kOracleSamples);
    }

    std::printf("pristine oracle: %zu violations over %zu rows x 512 samples\n", failures, rows.size());

    std::printf(
            "deterministic: corpus seeds 0x5eed0001 and 0x5eed0002, the pristine-oracle sampling seed 0x5eed0003, "
            "schedule seed 0x5eedc0de and payload seed "
            "0x5eedbeef each offset per (row, seed, op, k) so no two rows share a stream, %zu independent seeds, "
            "positions by unbiased rejection sampling, attempt budget 100 per incident\n",
            seeds);

    return failures;
}

/**
 * @brief Runs one row under every seed in turn and prints its label, its stratified table, its pooled summary and its
 *        per-seed first landing.
 * @param row The row.
 * @param row_index The row's index among the rows.
 * @param trials The trials per cell.
 * @param seeds The seeds.
 * @param totals The campaign's totals.
 * @param archive The archive.
 */
void run_row(
        const Row& row, const std::size_t row_index, const std::size_t trials, const std::size_t seeds,
        Campaign_totals& totals, Archive& archive)
{
    std::printf("\n%s\n", std::string{row.label}.c_str());

    auto tallies{row_tallies(seeds)};

    for (std::size_t seed{0}; seed < seeds; ++seed)
    {
        run_seed(row, row_index, seed, trials, totals, tallies, archive);
    }

    print_strata(tallies);

    print_pooled(tallies);

    print_seeds(tallies);
}

/**
 * @brief Prints the verdict and checks that the whole summary reached standard output, its flush and its error
 *        indicator both; a summary that did not is reported with `summary write failed` on standard error.
 * @param oracle_failures The pristine oracle's violations.
 * @param totals The campaign's totals.
 * @return EXIT_SUCCESS when every assertion held and the summary was written, EXIT_FAILURE otherwise.
 */
int verdict(const std::size_t oracle_failures, const Campaign_totals& totals)
{
    if (oracle_failures != 0 || totals.theorem_failures != 0)
    {
        std::printf(
                "FAILED: %zu oracle violations, %zu theorem violations\n", oracle_failures, totals.theorem_failures);

        return EXIT_FAILURE;
    }

    std::printf("all oracle and theorem assertions held\n");

    const auto flushed{std::fflush(stdout)};

    if (flushed != 0 || std::ferror(stdout) != 0)
    {
        std::fprintf(stderr, "summary write failed\n");

        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

} // namespace

/**
 * @brief Runs the campaign: reads the command line, builds the rows, runs the pristine oracle, opens the archive when
 *        one is named, runs every row, closes the archive, and prints the closing summary and the verdict.
 * @param argc The argument count.
 * @param argv The corpus KiB, the trials per cell, the archive's path, the real document's path and the seeds, all
 *        optional.
 * @return 0 when every assertion held and every write succeeded, 1 otherwise.
 */
int main(const int argc, const char** argv)
{
    std::vector<std::string_view> command_line{};

    for (int index{1}; index < argc; ++index)
    {
        command_line.emplace_back(argv[index]);
    }

    const auto arguments{arguments_of(command_line)};

    if (!arguments)
    {
        return EXIT_FAILURE;
    }

    const auto rows{rows_of(*arguments)};

    if (!rows)
    {
        return EXIT_FAILURE;
    }

    const auto oracle_failures{pristine_violations(*rows, arguments->seeds)};

    Archive archive{};

    if (arguments->csv_path && !archive.open(*arguments->csv_path, *rows))
    {
        return EXIT_FAILURE;
    }

    Campaign_totals totals{};

    for (std::size_t row_index{0}; row_index < rows->size(); ++row_index)
    {
        run_row((*rows)[row_index], row_index, arguments->trials, arguments->seeds, totals, archive);
    }

    if (!archive.close())
    {
        return EXIT_FAILURE;
    }

    print_totals(totals);

    return verdict(oracle_failures, totals);
}
