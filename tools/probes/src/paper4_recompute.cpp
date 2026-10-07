// Recomputes what munch's own decisions reach of the certified-splitting paper's emissions, so that every line it
// recomputes can be held against the library line by line; what it does not reach the comparison names.
//
// The paper's data directory holds emissions written by explorations/certification in the research repository:
// splitting_measurements.py, campaign_inventory.py, frozen_inventory.py, depth_series.py, budget_coverage.py and the
// wide cutoff sweep of explore_certification.py. Each figure there is a decision of the Python's own deciders over a
// token set and a corpus slice, and this probe makes the same decisions with munch: the byte certificate is
// Lexer::is_split_point(), the exact window certificate is window_counterexample() exhaustive with no witness, the
// occurring reading is window_occurrence(), the anchor supply is tools/audit's supply() over a report holding those
// decisions, the gap corollary is anchor_free_span(), and the differential is boundary_difference(). Beside the exact
// window decision the conservative one, is_split_window(), is reported on a line of its own, so a reader sees what the
// shipped model gives where the paper's decider is the exact verifier.
//
// Every line written is in the format of the committed emission it recomputes, one file per emission under the output
// directory, and the comparison script, analysis/paper4-recompute/compare.py, diffs the two directories line by line.
// Nothing here is typed from the paper: the token sets are rebuilt as the Python builds them, the trained byte-pair
// vocabulary by the same merge procedure with the same tie rule and the GPT-2 subset from the same merge table, and
// each list is digested so that the configuration line's digest is what proves the token set is the paper's; the edit
// trials replay the Python's own seeded generator, ported here, so the draws are the paper's.
//
// Usage: munch_paper4_recompute <output directory> [<twitter.json> <gpt2-merges.txt> <campaign corpus directory>]
//                               [section...]
//
// The corpus is jsonexamples/twitter.json of simdjson v3.10.1, the merge table is the GPT-2 release's, and the campaign
// corpora are the recovery paper's archived r6 corpora, the four c-like rows; none is redistributed here, and the
// research repository's gate, scripts/check-paper4.sh, names where each lives. A section name restricts the run to it:
// vocabularies, budget, depth, frozen, campaign, sweep; with none every section runs. With the output directory alone
// the probe runs the sections that need no external file, the UTF-8 shape's sync distance and the wide cutoff sweep,
// which is what the test entry runs.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/audit/report.hpp"
#include "munch/tools/audit/supply.hpp"
#include "munch/tools/probes/files.hpp"
#include "munch/tools/probes/study_rows.hpp"
#include "munch/tools/probes/window_model.hpp"

namespace
{
using munch::core::Lexer;
using munch::tools::audit::Certified_window;
using munch::tools::audit::Report;
using munch::tools::probes::byte_count;
using munch::tools::probes::every_byte;

/**
 * @brief The corpus prefix the byte-pair vocabulary is trained on.
 */
constexpr std::size_t train_bytes{65'536};

/**
 * @brief The evaluation slice, the bytes after the training prefix.
 */
constexpr std::size_t eval_bytes{16'384};

/**
 * @brief The calibration slice, the bytes after the evaluation slice.
 */
constexpr std::size_t calibration_bytes{16'384};

/**
 * @brief The merge depth of the trained vocabulary.
 */
constexpr std::size_t merges_local{384};

/**
 * @brief The merge depths of the depth series, ascending.
 */
constexpr std::array<std::size_t, 4> depths{48, 96, 192, 384};

/**
 * @brief The rule count of the GPT-2 release's merge table.
 */
constexpr std::size_t gpt2_merges{50'000};

/**
 * @brief The merges of the GPT-2 table the paper's subset takes.
 */
constexpr std::size_t gpt2_prefix{4096};

/**
 * @brief The window budget, the widths decided exhaustively.
 */
constexpr std::array<std::size_t, 3> widths{2, 3, 4};

/**
 * @brief The width the edit trials probe past the budget.
 */
constexpr std::size_t probe_width{5};

/**
 * @brief The most frequent n-grams per width the sampled inventory decides.
 */
constexpr std::size_t sampled_per_width{20};

/**
 * @brief The seed of the edit trials' generator.
 */
constexpr std::uint32_t edit_seed{20260826};

/**
 * @brief The accepted edits the trials measure.
 */
constexpr std::size_t edit_trials_wanted{200};

/**
 * @brief The attempts the trials make at most.
 */
constexpr std::size_t edit_attempts_cap{4000};

/**
 * @brief The seed of the campaign's edit study.
 */
constexpr std::uint32_t campaign_edit_seed{20260827};

/**
 * @brief The cross-class substitutions the campaign's edit study retains per row.
 */
constexpr std::size_t campaign_cross_class_edits{60};

/**
 * @brief The same-class substitutions the campaign's edit study retains per row.
 */
constexpr std::size_t campaign_same_class_edits{12};

/**
 * @brief The longest prefix of a campaign corpus the edit study slices, cut back to a line end.
 */
constexpr std::size_t campaign_slice_bytes{32'768};

/**
 * @brief The block the per-block supply figures count over.
 */
constexpr std::size_t block_bytes{1024};

/**
 * @brief The archive the campaign corpora are named after, the recovery paper's r6 collection.
 */
constexpr std::string_view campaign_archive{"recovery-quality-six-rows-512k-500-r6.csv"};

/**
 * @brief The worker counts the snap figures are printed at, the supply tables' at the first.
 */
constexpr std::array<std::size_t, 2> snap_workers{8, 64};

/**
 * @brief The class letters of the campaign's block row, in the paper's order.
 */
constexpr std::string_view block_sigma{"LDSNACOPX"};

/**
 * @brief The length of each slice piece the divergence lines scan with both lexers.
 */
constexpr std::size_t divergence_piece_bytes{64};

/**
 * @brief The distance between the starts of two consecutive divergence pieces.
 */
constexpr std::size_t divergence_stride{256};

/**
 * @brief The sections there are, in the order the refusal of an unknown one names them.
 */
constexpr std::array<std::string_view, 6> known_sections{"vocabularies", "budget",   "depth",
                                                         "frozen",       "campaign", "sweep"};

/**
 * @brief The command line's first section argument, after the program and its four paths.
 */
constexpr int first_section_argument{5};

/**
 * @brief The hexadecimal digits of a SHA-256 digest the configuration lines print, its first sixteen.
 */
constexpr std::size_t digest_prefix_digits{16};

/**
 * @brief The file under the output directory naming every file this run wrote.
 */
constexpr std::string_view manifest_file{"manifest.txt"};

/**
 * @brief A class letter and the byte the campaign's edit study substitutes for it.
 */
struct Replacement
{
    /**
     * @brief The letter.
     */
    char letter{};

    /**
     * @brief The replacement byte.
     */
    char byte{};
};

/**
 * @brief The replacement byte of each letter, in the order campaign_inventory.py lists them.
 */
constexpr std::array replacements{Replacement{.letter = 'L', .byte = 'x'},   Replacement{.letter = 'D', .byte = '7'},
                                  Replacement{.letter = 'S', .byte = ' '},   Replacement{.letter = 'N', .byte = '\n'},
                                  Replacement{.letter = 'Q', .byte = '"'},   Replacement{.letter = 'C', .byte = '/'},
                                  Replacement{.letter = 'O', .byte = '+'},   Replacement{.letter = 'P', .byte = ';'},
                                  Replacement{.letter = 'X', .byte = '\x01'}};

/**
 * @brief One certified pair as the paper's inventories hold it: the window's bytes and the origin.
 */
struct Pair
{
    /**
     * @brief Orders by window, then by origin.
     */
    auto operator<=>(const Pair&) const = default;

    /**
     * @brief The window's bytes.
     */
    std::string window{};

    /**
     * @brief The origin, the offset into the window of the position it certifies.
     */
    std::size_t origin{0};
};

/**
 * @brief The lines of one emission, written to the file of the committed emission's name.
 */
struct Emission
{
    /**
     * @brief The file's name under the output directory.
     */
    std::string file{};

    /**
     * @brief The lines, each written with a newline after it.
     */
    std::vector<std::string> lines{};
};

/**
 * @brief Python's random.Random, ported: the Mersenne twister seeded by its init_by_array from one integer, with
 *        randrange() and choice() drawing through its rejection loop over getrandbits(), so that the edit trials replay
 *        the paper's draws exactly.
 */
class Python_random
{
public:
    /**
     * @brief Seeds as random.Random(seed) does for an integer below 2^32: init_by_array over the one word.
     * @param seed The seed.
     */
    explicit Python_random(const std::uint32_t seed)
    {
        state_[0] = genrand_seed;

        for (std::size_t index{1}; index < size; ++index)
        {
            state_[index] = genrand_multiplier * (state_[index - 1] ^ (state_[index - 1] >> fold_shift)) +
                            static_cast<std::uint32_t>(index);
        }

        std::size_t i{1};

        const auto advance{[&words = state_, &i] {
            if (++i < size)
            {
                return;
            }

            words[0] = words[size - 1];

            i = 1;
        }};

        for (std::size_t round{0}; round < size; ++round)
        {
            state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> fold_shift)) * key_multiplier)) + seed;

            advance();
        }

        for (std::size_t round{0}; round < size - 1; ++round)
        {
            state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> fold_shift)) * mix_multiplier)) -
                        static_cast<std::uint32_t>(i);

            advance();
        }

        state_[0] = upper_mask;
    }

    /**
     * @brief Returns random.randrange(n): a uniform draw below n by rejection over the draw's bit length.
     * @param n The exclusive bound, positive.
     * @return The draw.
     */
    [[nodiscard]] std::size_t randrange(const std::size_t n)
    {
        const auto bits{static_cast<unsigned>(std::bit_width(n))};

        auto draw{getrandbits(bits)};

        while (draw >= n)
        {
            draw = getrandbits(bits);
        }

        return draw;
    }

private:
    /**
     * @brief The twister's state words.
     */
    static constexpr std::size_t size{624};

    /**
     * @brief The first state word of init_genrand(), which init_by_array starts from.
     */
    static constexpr std::uint32_t genrand_seed{19650218U};

    /**
     * @brief The multiplier init_genrand() fills the state with.
     */
    static constexpr std::uint32_t genrand_multiplier{1812433253U};

    /**
     * @brief The right shift init_genrand() and init_by_array fold a word's high bits down with before multiplying.
     */
    static constexpr unsigned fold_shift{30U};

    /**
     * @brief The multiplier init_by_array mixes the key in with.
     */
    static constexpr std::uint32_t key_multiplier{1664525U};

    /**
     * @brief The multiplier of init_by_array's final mixing pass.
     */
    static constexpr std::uint32_t mix_multiplier{1566083941U};

    /**
     * @brief The most significant bit of a state word: the part of the current word a twist keeps, and the first word
     *        init_by_array leaves.
     */
    static constexpr std::uint32_t upper_mask{0x80000000U};

    /**
     * @brief The low 31 bits of a state word, the part of the next word a twist takes.
     */
    static constexpr std::uint32_t lower_mask{0x7FFFFFFFU};

    /**
     * @brief The offset of the word a twist mixes in.
     */
    static constexpr std::size_t middle_word{397};

    /**
     * @brief The twist's matrix row, mixed in when the combined word is odd.
     */
    static constexpr std::uint32_t matrix_a{0x9908B0DFU};

    /**
     * @brief The tempering's first right shift.
     */
    static constexpr unsigned tempering_shift_u{11U};

    /**
     * @brief The tempering's first left shift, masked by tempering_mask_b.
     */
    static constexpr unsigned tempering_shift_s{7U};

    /**
     * @brief The tempering's second left shift, masked by tempering_mask_c.
     */
    static constexpr unsigned tempering_shift_t{15U};

    /**
     * @brief The tempering's last right shift.
     */
    static constexpr unsigned tempering_shift_l{18U};

    /**
     * @brief The mask of the tempering's first left shift.
     */
    static constexpr std::uint32_t tempering_mask_b{0x9D2C5680U};

    /**
     * @brief The mask of the tempering's second left shift.
     */
    static constexpr std::uint32_t tempering_mask_c{0xEFC60000U};

    /**
     * @brief Returns random.getrandbits(k) for k up to 32: the top k bits of one output.
     * @param bits The bit count, one to 32.
     * @return The draw.
     */
    [[nodiscard]] std::size_t getrandbits(const unsigned bits) { return next() >> (32U - bits); }

    /**
     * @brief Returns the next 32-bit output of the twister.
     * @return The output.
     */
    [[nodiscard]] std::uint32_t next()
    {
        if (index_ >= size)
        {
            for (std::size_t k{0}; k < size; ++k)
            {
                const auto y{(state_[k] & upper_mask) | (state_[(k + 1) % size] & lower_mask)};

                state_[k] = state_[(k + middle_word) % size] ^ (y >> 1U) ^ ((y & 1U) != 0 ? matrix_a : 0U);
            }

            index_ = 0;
        }

        auto y{state_[index_++]};

        y ^= y >> tempering_shift_u;

        y ^= (y << tempering_shift_s) & tempering_mask_b;

        y ^= (y << tempering_shift_t) & tempering_mask_c;

        y ^= y >> tempering_shift_l;

        return y;
    }

    /**
     * @brief The state.
     */
    std::array<std::uint32_t, size> state_{};

    /**
     * @brief The next state word to temper, the state regenerated when it reaches the size.
     */
    std::size_t index_{size};
};

/**
 * @brief The tie accounting of a byte-pair training run, which the paper states as identifying the vocabulary.
 */
struct Merge_stats
{
    /**
     * @brief The merges made.
     */
    std::size_t merges{0};

    /**
     * @brief The steps whose most frequent count more than one pair attained.
     */
    std::size_t tied_steps{0};

    /**
     * @brief The tied steps at which the earliest occurring contender is not the pair the tie rule merged.
     */
    std::size_t first_occurrence_disagreements{0};
};

/**
 * @brief Two adjacent symbols of a training sequence, the left and the right, which a merge joins.
 */
using Symbol_pair_t = std::pair<std::string, std::string>;

/**
 * @brief A byte-pair training run's result: the merge table and its tie accounting.
 */
struct Trained_merges
{
    /**
     * @brief The merges, in order.
     */
    std::vector<Symbol_pair_t> table{};

    /**
     * @brief The tie accounting.
     */
    Merge_stats stats{};
};

/**
 * @brief The adjacent pairs of a training sequence, overlapping ones included, with how often and where first each
 *        occurs.
 */
struct Pair_counts
{
    /**
     * @brief Each pair's occurrences.
     */
    std::map<Symbol_pair_t, std::size_t> counts{};

    /**
     * @brief Each pair's first position.
     */
    std::map<Symbol_pair_t, std::size_t> first{};
};

/**
 * @brief The tie accounting of one merge step: whether more than one pair attained the most frequent count, and whether
 *        the earliest occurring of them is not the pair the tie rule merged.
 */
struct Tie
{
    /**
     * @brief Whether the most frequent count was tied.
     */
    bool tied{false};

    /**
     * @brief Whether first occurrence would have merged a different pair.
     */
    bool disagrees{false};
};

/**
 * @brief What deciding a set of pairs found: the pairs certified exactly, the pairs the conservative model certifies
 *        among them, and the decisions the exact search left unsettled at its cap.
 */
struct Decided
{
    /**
     * @brief The pairs window_counterexample() certifies exhaustively.
     */
    std::vector<Pair> exact{};

    /**
     * @brief The pairs is_split_window() certifies.
     */
    std::vector<Pair> conservative{};

    /**
     * @brief The pairs whose exact search stopped at its cap.
     */
    std::vector<Pair> unsettled{};
};

/**
 * @brief An exact decision of one pair.
 */
enum class Verdict : std::uint8_t
{
    /**
     * @brief The search found a counterexample.
     */
    refused,

    /**
     * @brief The search was exhaustive and found none.
     */
    certified,

    /**
     * @brief The search stopped at its cap.
     */
    unsettled
};

/**
 * @brief An inventory indexed for matching: the origins certified for each window, by window, and the widths held.
 */
struct Inventory
{
    /**
     * @brief Indexes the pairs.
     * @param certified The pairs.
     */
    explicit Inventory(const std::vector<Pair>& certified)
    {
        for (const auto& [window, origin] : certified)
        {
            origins_of[window].push_back(origin);

            lengths.insert(window.size());
        }
    }

    /**
     * @brief Reports the origins certified for the windows beginning at a position of a text: for each held width whose
     *        window there is in the inventory, that window's origins.
     * @tparam Take The receiver's type, callable with an origin.
     * @param text The text.
     * @param at The position.
     * @param take Receives each origin.
     */
    template <typename Take>
    void origins_at(const std::string_view text, const std::size_t at, const Take& take) const
    {
        for (const auto length : lengths)
        {
            if (at + length > text.size())
            {
                break;
            }

            const auto found{origins_of.find(text.substr(at, length))};

            if (found == origins_of.end())
            {
                continue;
            }

            const auto& [window, origins]{*found};

            for (const auto origin : origins)
            {
                take(origin);
            }
        }
    }

    /**
     * @brief The origins certified for each window, by window.
     */
    std::map<std::string, std::vector<std::size_t>, std::less<>> origins_of{};

    /**
     * @brief The window widths the inventory holds, ascending.
     */
    std::set<std::size_t> lengths{};
};

/**
 * @brief One attempted edit of the paper's edit trials, as splitting-edit-trials.csv records it.
 */
struct Edit_record
{
    /**
     * @brief The edited position.
     */
    std::size_t position{0};

    /**
     * @brief The replacement byte drawn.
     */
    unsigned char new_byte{0};

    /**
     * @brief Whether the edit was measured: the byte differs and the edited input tokenizes.
     */
    bool accepted{false};

    /**
     * @brief Whether the edited input stopped tokenizing.
     */
    bool untokenizable{false};

    /**
     * @brief The left end of the theorem's window, max(1, p - L + 2).
     */
    std::size_t left_admissible{0};

    /**
     * @brief The theorem's q under the ceiling inventory.
     */
    std::size_t right_anchor{0};

    /**
     * @brief The theorem's q under the frozen inventory.
     */
    std::size_t frozen_anchor{0};

    /**
     * @brief The boundaries the edit moved.
     */
    std::size_t moved{0};

    /**
     * @brief The span from the first moved boundary to the last, zero when none moved.
     */
    std::size_t hull{0};

    /**
     * @brief The width of the theorem's window under the ceiling inventory.
     */
    std::size_t admissible{0};

    /**
     * @brief The positions of that window no moved boundary occupies.
     */
    std::size_t unoccupied{0};

    /**
     * @brief The earlier anchor the width-five probe found, if any.
     */
    std::optional<std::size_t> probe_anchor{};
};

/**
 * @brief How often an n-gram occurs and where it first does.
 */
struct Occurrence_count
{
    /**
     * @brief The occurrences.
     */
    std::size_t count{0};

    /**
     * @brief The position of the first occurrence.
     */
    std::size_t first{0};
};

/**
 * @brief The sampled inventory's selection: the windows chosen, the twenty most frequent per width, and per width the
 *        head ranked above the last slot's count, the band tying it and the slots the band fills.
 */
struct Sampled_selection
{
    /**
     * @brief The windows chosen, width by width in rank order.
     */
    std::vector<std::string> chosen{};

    /**
     * @brief Each width's head, the windows ranked above the last slot's count.
     */
    std::vector<std::vector<std::string>> heads{};

    /**
     * @brief Each width's band, the windows tying the last slot's count.
     */
    std::vector<std::vector<std::string>> bands{};

    /**
     * @brief Each width's slots left to the band.
     */
    std::vector<std::size_t> slots{};
};

/**
 * @brief What every selection consistent with the ties certifies: the distinct certified-pair counts, the distinct
 *        supplies of the wider half alone and with the certified bytes, and the number of selections.
 */
struct Tie_family
{
    /**
     * @brief The certified-pair counts the selections reach.
     */
    std::set<std::size_t> counts{};

    /**
     * @brief The supplies of the selections' wider half.
     */
    std::set<double> wider_supply{};

    /**
     * @brief The supplies of the selections' wider half with the certified bytes.
     */
    std::set<double> whole_supply{};

    /**
     * @brief The selections scored.
     */
    std::size_t selections{0};
};

/**
 * @brief A member's place in a selection mask, ordered so that a mask with its taken members first is the greatest
 *        permutation.
 */
enum class Taken : std::uint8_t
{
    /**
     * @brief The member is left out.
     */
    no,

    /**
     * @brief The member is taken.
     */
    yes
};

/**
 * @brief One measured vocabulary: its name, tokens, lexer, certified bytes and the exhaustive inventory over the
 *        evaluation slice.
 */
struct Vocabulary
{
    /**
     * @brief Returns the ceiling inventory: the certified bytes and the exact wider pairs.
     * @return The inventory.
     */
    [[nodiscard]] std::vector<Pair> certified() const;

    /**
     * @brief The name the emission lines carry.
     */
    std::string name{};

    /**
     * @brief The label the tables carry.
     */
    std::string label{};

    /**
     * @brief The token list, in the order the Python holds it.
     */
    std::vector<std::string> tokens{};

    /**
     * @brief The compiled lexer.
     */
    Lexer lexer;

    /**
     * @brief The certified bytes, as pairs at origin zero.
     */
    std::vector<Pair> bytes{};

    /**
     * @brief The windows of the budget's widths occurring in the evaluation slice, ascending.
     */
    std::vector<std::string> windows{};

    /**
     * @brief The decisions over every origin of those windows.
     */
    Decided decided{};
};

/**
 * @brief The supply of one inventory as the paper prints it for a named row: its lines, its anchors and the library's
 *        figures behind them.
 */
struct Supply_report
{
    /**
     * @brief The lines: anchors per KiB, the gap figures and the longest anchorless stretch.
     */
    std::vector<std::string> lines{};

    /**
     * @brief The anchors, ascending.
     */
    std::vector<std::size_t> anchors{};

    /**
     * @brief The library's supply figures.
     */
    munch::tools::audit::Anchors supply{};
};

/**
 * @brief What one vocabulary contributes to the evaluation-slice emissions: its stats lines, its edit trials, its row
 *        of the supply table and its rows of the edit-slack table.
 */
struct Vocabulary_report
{
    /**
     * @brief The vocabulary's lines of splitting-stats.txt.
     */
    std::vector<std::string> lines{};

    /**
     * @brief The edit trials' records.
     */
    std::vector<Edit_record> trials{};

    /**
     * @brief The supply table's row.
     */
    std::string table_row{};

    /**
     * @brief The edit-slack table's two rows.
     */
    std::vector<std::string> slack_rows{};
};

/**
 * @brief A vocabulary's snap lines and the worst snap the supply table prints.
 */
struct Snap_lines
{
    /**
     * @brief The snap lines, the worst and the median snap at each of snap_workers.
     */
    std::vector<std::string> lines{};

    /**
     * @brief The worst snap at the first of snap_workers.
     */
    std::size_t table_snap_max{0};
};

/**
 * @brief The calibration slice's windows, and those of them the evaluation slice never poses.
 */
struct Calibration_census
{
    /**
     * @brief Every window of width two to four occurring in the calibration slice, sorted.
     */
    std::vector<std::string> windows{};

    /**
     * @brief The calibration windows the evaluation slice does not hold, in the calibration windows' order.
     */
    std::vector<std::string> only_here{};

    /**
     * @brief Every origin of every calibration-only window, the pairs decided fresh.
     */
    std::vector<Pair> only_here_pairs{};

    /**
     * @brief The (window, origin) decisions behind the calibration-only windows.
     */
    std::size_t decisions{0};

    /**
     * @brief The calibration-only windows counted by width, as the inventory line spells them.
     */
    std::string by_width_text{};
};

/**
 * @brief One vocabulary's frozen-inventory lines and its row of the transfer table.
 */
struct Frozen_report
{
    /**
     * @brief The vocabulary's lines of frozen-inventory-stats.txt.
     */
    std::vector<std::string> lines{};

    /**
     * @brief The transfer table's row.
     */
    std::string table_row{};
};

/**
 * @brief One campaign row: its name, corpus file, grammar, the class letters in the paper's order, the letter of each
 *        byte, and the parts of the campaign it takes part in beyond the stats and the supply table.
 */
struct Campaign_row
{
    /**
     * @brief The name the emission lines carry.
     */
    std::string name{};

    /**
     * @brief The corpus file's name after the archive's.
     */
    std::string corpus{};

    /**
     * @brief Adds the row's tokens to a builder.
     */
    std::function<void(munch::core::Builder&)> grammar{};

    /**
     * @brief The class letters in the paper's order.
     */
    std::string sigma{};

    /**
     * @brief The letter of each byte.
     */
    std::function<char(unsigned char)> classify{};

    /**
     * @brief The windows over letters decided past the budget, none for most rows.
     */
    std::vector<Pair> extra_windows{};

    /**
     * @brief Whether the row's certified pairs make the anchor table.
     */
    bool anchor_table{false};

    /**
     * @brief Whether the edit study replays the row.
     */
    bool edit_study{false};
};

/**
 * @brief What the campaign's edit study needs of a measured row: its lexer, its corpus, the corpus over class
 *        representatives, the letter of each byte, the letters in the paper's order and the inventory over
 *        representatives.
 */
struct Measured_row
{
    /**
     * @brief The row's name.
     */
    std::string name{};

    /**
     * @brief The row's lexer.
     */
    Lexer lexer;

    /**
     * @brief The corpus.
     */
    std::string corpus{};

    /**
     * @brief The corpus with every byte replaced by its letter's representative.
     */
    std::string class_text{};

    /**
     * @brief The letter of each byte.
     */
    std::function<char(unsigned char)> classify{};

    /**
     * @brief The class letters in the paper's order.
     */
    std::string sigma{};

    /**
     * @brief The certified pairs over representatives.
     */
    std::vector<Pair> certified{};
};

/**
 * @brief The slice the campaign's edit study edits: a line-ended prefix of the corpus, its class string, its token
 *        starts and every occurrence of every inventory window in the class string.
 */
struct Edit_slice
{
    /**
     * @brief The slice, a prefix of the row's corpus.
     */
    std::string_view slice{};

    /**
     * @brief The slice over class representatives.
     */
    std::string_view slice_classes{};

    /**
     * @brief The slice's token starts.
     */
    std::set<std::size_t> base{};

    /**
     * @brief Each inventory pair with the positions its window occurs at in the class string.
     */
    std::vector<std::pair<Pair, std::vector<std::size_t>>> occurrences{};
};

/**
 * @brief A campaign row's letters against its lexer's byte classes, every letter lying inside one class of the tables.
 */
struct Row_letters
{
    /**
     * @brief The bytes of each letter, ascending.
     */
    std::map<char, std::vector<unsigned char>> members_of{};

    /**
     * @brief Each letter's representative, its lowest byte.
     */
    std::map<char, unsigned char> representative{};

    /**
     * @brief Each byte's representative.
     */
    std::array<char, byte_count> canonical{};

    /**
     * @brief The letters' byte sets, ordered by lowest byte, the classes the report matches over.
     */
    std::vector<std::vector<unsigned char>> classes{};

    /**
     * @brief How many classes the lexer's tables hold.
     */
    std::size_t table_class_count{0};

    /**
     * @brief The letters the tables do not tell apart, as the byte-classes line spells them, empty when every letter is
     *        a class of its own.
     */
    std::string merged{};
};

/**
 * @brief The certified pairs of a row, in candidate order, over letters and over representatives alike.
 */
struct Certified_pairs
{
    /**
     * @brief The pairs over letters.
     */
    std::vector<Pair> over_letters{};

    /**
     * @brief The same pairs over representatives.
     */
    std::vector<Pair> over_bytes{};
};

/**
 * @brief The budget extension of a row: the extra windows decided over their representatives, the certified ones in
 *        their order, and the two lines reporting them.
 */
struct Extension
{
    /**
     * @brief The certified extra pairs, over letters and over representatives.
     */
    Certified_pairs certified{};

    /**
     * @brief The two lines.
     */
    std::vector<std::string> lines{};
};

/**
 * @brief The certified pairs whose window occurs in no completely tokenizable class string: their count and their
 *        listing.
 */
struct Vacuity
{
    /**
     * @brief How many certified pairs are vacuous.
     */
    std::size_t count{0};

    /**
     * @brief The vacuous pairs as the occurrence line lists them, comma separated.
     */
    std::string text{};
};

/**
 * @brief What one campaign row contributes: its stats lines, its anchor table lines, its supply table row, its
 *        certified and vacuous counts, and the row the edit study replays when it takes part.
 */
struct Campaign_row_report
{
    /**
     * @brief The stats file's lines.
     */
    std::vector<std::string> lines{};

    /**
     * @brief The anchor table's lines, none for a row outside it.
     */
    std::vector<std::string> anchor_lines{};

    /**
     * @brief The supply table's row.
     */
    std::string table_row{};

    /**
     * @brief The certified pairs.
     */
    std::size_t certified{0};

    /**
     * @brief The vacuous certified pairs.
     */
    std::size_t vacuous{0};

    /**
     * @brief The row the edit study replays, none for a row outside it.
     */
    std::optional<Measured_row> edit_row{};
};

/**
 * @brief A campaign row's abstraction check: the byte scan's token starts, and the row's first two lines.
 */
struct Row_abstraction
{
    /**
     * @brief The token starts of the byte scan of the corpus.
     */
    std::vector<std::size_t> byte_starts{};

    /**
     * @brief The abstraction and byte-classes lines.
     */
    std::vector<std::string> lines{};
};

/**
 * @brief A campaign row's certified pairs, the extension's included, and their lines.
 */
struct Row_certificates
{
    /**
     * @brief The certified pairs over letters and over representatives.
     */
    Certified_pairs certified{};

    /**
     * @brief The certified-pair lines, the refutation's and the extension's included.
     */
    std::vector<std::string> lines{};
};

/**
 * @brief A campaign row's anchors over its class string, their supply, and the supply and snap lines.
 */
struct Row_supply
{
    /**
     * @brief The anchors over the class string, ascending.
     */
    std::vector<std::size_t> anchors{};

    /**
     * @brief The gaps between anchors, the runs to the two ends counted.
     */
    std::vector<std::size_t> gaps{};

    /**
     * @brief The anchors per KiB by the library's supply.
     */
    double per_kibibyte{0.0};

    /**
     * @brief The worst snap at the first of snap_workers, as the table prints it.
     */
    std::string table_snap{};

    /**
     * @brief The supply and snap lines.
     */
    std::vector<std::string> lines{};
};

/**
 * @brief Returns the SHA-256 of a byte string, as hexadecimal, the digest the paper names token lists and slices by.
 * @param text The bytes.
 * @return The 64 hexadecimal digits.
 */
[[nodiscard]] std::string sha256(const std::string_view text)
{
    std::array<std::uint32_t, 8> state{0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                                       0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};

    // The message, padded: a one bit, zeros to 56 mod 64, and the bit length big-endian.
    std::string padded{text};

    padded.push_back(static_cast<char>(0x80));

    while (padded.size() % 64 != 56)
    {
        padded.push_back('\0');
    }

    const auto bit_length{static_cast<std::uint64_t>(text.size()) * 8};

    for (int shift{56}; shift >= 0; shift -= 8)
    {
        padded.push_back(static_cast<char>((bit_length >> static_cast<unsigned>(shift)) & 0xFF));
    }

    for (std::size_t block{0}; block < padded.size(); block += 64)
    {
        std::array<std::uint32_t, 64> schedule{};

        for (std::size_t word{0}; word < 16; ++word)
        {
            for (std::size_t byte{0}; byte < 4; ++byte)
            {
                schedule[word] =
                        (schedule[word] << 8U) | static_cast<unsigned char>(padded[block + (word * 4U) + byte]);
            }
        }

        for (std::size_t word{16}; word < 64; ++word)
        {
            const auto s0{
                    std::rotr(schedule[word - 15], 7) ^ std::rotr(schedule[word - 15], 18) ^
                    (schedule[word - 15] >> 3)};

            const auto s1{
                    std::rotr(schedule[word - 2], 17) ^ std::rotr(schedule[word - 2], 19) ^ (schedule[word - 2] >> 10)};

            schedule[word] = schedule[word - 16] + s0 + schedule[word - 7] + s1;
        }

        auto [a, b, c, d, e, f, g, h]{state};

        static constexpr std::array<std::uint32_t, 64> round_constants{
                0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
                0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
                0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
                0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
                0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
                0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
                0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
                0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2};

        for (std::size_t round{0}; round < 64; ++round)
        {
            const auto s1{std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)};

            const auto choice{(e & f) ^ (~e & g)};

            const auto t1{h + s1 + choice + round_constants[round] + schedule[round]};

            const auto s0{std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)};

            const auto majority{(a & b) ^ (a & c) ^ (b & c)};

            const auto t2{s0 + majority};

            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::string hex{};

    for (const auto word : state)
    {
        hex += std::format("{:08x}", word);
    }

    return hex;
}

/**
 * @brief Returns a byte string as Python encodes the latin-1 text it reads the corpus into: UTF-8, every byte from 0x80
 *        up two bytes, which is what the paper's token digests are taken over.
 * @param bytes The bytes.
 * @return The UTF-8 encoding.
 */
[[nodiscard]] std::string utf8_of_latin1(const std::string_view bytes)
{
    std::string out{};

    for (const auto byte : bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (value < 0x80)
        {
            out.push_back(byte);
        }
        else
        {
            out.push_back(static_cast<char>(0xC0 | (value >> 6U)));
            out.push_back(static_cast<char>(0x80 | (value & 0x3FU)));
        }
    }

    return out;
}

/**
 * @brief Returns the first sixteen hexadecimal digits of the paper's token-list digest: SHA-256 over the tokens joined
 *        by a zero byte, in the Python's UTF-8 encoding of its latin-1 strings.
 * @param tokens The token list, repeats included, in the order the Python holds it.
 * @return The digest prefix the configuration line carries.
 */
[[nodiscard]] std::string token_digest(const std::vector<std::string>& tokens)
{
    std::string joined{};

    std::ranges::copy(tokens | std::views::join_with('\0'), std::back_inserter(joined));

    const auto digest{sha256(utf8_of_latin1(joined))};

    return digest.substr(0, digest_prefix_digits);
}

/**
 * @brief Reads a whole file as bytes.
 * @param path The file.
 * @return Its bytes.
 * @throws std::runtime_error If the file cannot be read.
 */
[[nodiscard]] std::string read_file(const std::filesystem::path& path)
{
    auto bytes{munch::tools::probes::read_bytes(path)};

    if (!bytes)
    {
        throw std::runtime_error{std::format("cannot read {}", path.string())};
    }

    return std::move(*bytes);
}

/**
 * @brief Writes a note on the run to stderr with the elapsed time, never among the figures.
 * @param text The note.
 */
void note(const std::string_view text)
{
    static const auto began{std::chrono::steady_clock::now()};

    const auto since{std::chrono::steady_clock::now() - began};

    const auto elapsed{std::chrono::duration<double>(since).count()};

    std::cerr << std::format("[{:8.1f}s] {}\n", elapsed, text);
}

/**
 * @brief Writes an emission's lines to its file under the output directory, a newline after each, echoes them, and
 *        names the file in the run's manifest, which the comparison reads so that a file an earlier run left in the
 *        directory is no part of this one.
 * @param out The output directory.
 * @param emission The emission.
 * @throws std::runtime_error If the file or the manifest cannot be written.
 */
void write(const std::filesystem::path& out, const Emission& emission)
{
    const auto path{out / emission.file};

    std::ofstream file{path, std::ios::binary};

    if (!file)
    {
        throw std::runtime_error{std::format("cannot write {}", path.string())};
    }

    for (const auto& line : emission.lines)
    {
        file << line << '\n';

        std::cout << line << '\n';
    }

    std::cout.flush();

    // The manifest names only files written and closed.
    file.close();

    if (!file)
    {
        throw std::runtime_error{std::format("cannot write {}", path.string())};
    }

    const auto manifest_path{out / manifest_file};

    std::ofstream manifest{manifest_path, std::ios::binary | std::ios::app};

    if (!manifest)
    {
        throw std::runtime_error{std::format("cannot write {}", manifest_path.string())};
    }

    manifest << emission.file << '\n';

    note(std::format("wrote {}", emission.file));
}

/**
 * @brief Returns the paper's percentile: the order statistic at floor(q n) of the values ascending, capped at the last.
 * @tparam T The value type.
 * @param values The values, in any order.
 * @param q The quantile, as a fraction.
 * @return The percentile.
 */
template <typename T>
[[nodiscard]] T percentile(std::vector<T> values, const double q)
{
    std::ranges::sort(values);

    const auto index{static_cast<std::size_t>(q * static_cast<double>(values.size()))};

    return values[std::min(values.size() - 1, index)];
}

/**
 * @brief Returns a mean to three decimals, as edit_records.py prints one.
 * @param values The values.
 * @return The mean, formatted.
 */
[[nodiscard]] std::string mean(const std::vector<std::size_t>& values)
{
    const auto sum{std::ranges::fold_left(values, 0.0, std::plus{})};

    return std::format("{:.3f}", sum / static_cast<double>(values.size()));
}

/**
 * @brief Returns a density printed as the Python's exact_density prints one: six decimals with the trailing zeros
 *        trimmed.
 * @param value The density.
 * @return The text.
 */
[[nodiscard]] std::string exact_density(const double value)
{
    auto text{std::format("{:.6f}", value)};

    while (text.ends_with('0'))
    {
        text.pop_back();
    }

    if (text.ends_with('.'))
    {
        text.pop_back();
    }

    return text;
}

/**
 * @brief Counts every adjacent pair of a sequence, overlapping ones included, and where each first occurs.
 * @param sequence The training sequence.
 * @return The counts and first positions.
 */
[[nodiscard]] Pair_counts count_pairs(const std::vector<std::string>& sequence)
{
    Pair_counts pairs{};

    auto& [counts, first]{pairs};

    for (std::size_t at{0}; at + 1 < sequence.size(); ++at)
    {
        Symbol_pair_t pair{sequence[at], sequence[at + 1]};

        first.try_emplace(pair, at);

        ++counts[pair];
    }

    return pairs;
}

/**
 * @brief Returns the pair the tie rule merges: Python's max over (count, -len(l + r), (l, r)), the largest count, then
 *        the shortest merged string, then the greatest pair, strings compared by code point, which is unsigned byte
 *        order here; the first such entry in map order.
 * @param counts The pairs' counts, not empty.
 * @return The entry of the pair merged.
 */
[[nodiscard]] std::map<Symbol_pair_t, std::size_t>::const_iterator best_merge(
        const std::map<Symbol_pair_t, std::size_t>& counts)
{
    const auto length{[](const Symbol_pair_t& pair) {
        const auto& [left, right]{pair};

        return left.size() + right.size();
    }};

    const auto ranks_below{[&length]<typename Entry>(const Entry& best, const Entry& candidate) {
        const auto& [pair, n]{candidate};

        const auto& [best_pair, best_n]{best};

        const auto shorter{length(pair) < length(best_pair)};

        const auto greater{length(pair) == length(best_pair) && pair > best_pair};

        return n > best_n || (n == best_n && (shorter || greater));
    }};

    return std::ranges::max_element(counts, ranks_below);
}

/**
 * @brief Accounts one merge step's tie: the contenders are the pairs attaining the merged pair's count.
 * @param pairs The step's counts and first positions.
 * @param best_pair The pair the tie rule merged.
 * @param count Its count.
 * @return The step's tie accounting.
 */
[[nodiscard]] Tie account_tie(const Pair_counts& pairs, const Symbol_pair_t& best_pair, const std::size_t count)
{
    const auto& [counts, first]{pairs};

    const auto attains{[count]<typename Entry>(const Entry& entry) {
        const auto& [pair, n]{entry};

        return n == count;
    }};

    auto contenders{counts | std::views::filter(attains) | std::views::keys};

    if (std::ranges::distance(contenders) < 2)
    {
        return Tie{};
    }

    const auto first_position{[&first](const Symbol_pair_t& pair) { return first.at(pair); }};

    const auto earliest{std::ranges::min_element(contenders, {}, first_position)};

    return Tie{.tied = true, .disagrees = *earliest != best_pair};
}

/**
 * @brief Applies one merge to a sequence, left to right without overlap.
 * @param sequence The sequence.
 * @param left The merge's left symbol.
 * @param right The merge's right symbol.
 * @return The merged sequence.
 */
[[nodiscard]] std::vector<std::string> apply_merge(
        std::vector<std::string> sequence, const std::string& left, const std::string& right)
{
    const auto merged{left + right};

    std::vector<std::string> out{};

    out.reserve(sequence.size());

    for (std::size_t at{0}; at < sequence.size();)
    {
        if (at + 1 < sequence.size() && sequence[at] == left && sequence[at + 1] == right)
        {
            out.push_back(merged);

            at += 2;

            continue;
        }

        out.push_back(std::move(sequence[at]));

        ++at;
    }

    return out;
}

/**
 * @brief Trains byte-pair merges as vocab_experiment.py's train_bpe does: the most frequent adjacent pair is merged,
 *        every adjacent pair counted, overlapping ones included, ties going to the shorter concatenation and among
 *        those to the lexicographically greatest pair, and the merge applied left to right without overlap.
 * @param text The training text.
 * @param merges How many merges to make.
 * @return The merges, in order, with the tie accounting.
 */
[[nodiscard]] Trained_merges train_bpe(const std::string_view text, const std::size_t merges)
{
    std::vector<std::string> sequence{};

    sequence.reserve(text.size());

    for (const auto byte : text)
    {
        sequence.emplace_back(1, byte);
    }

    Trained_merges trained{};

    auto& [table, stats]{trained};

    for (std::size_t step{0}; step < merges; ++step)
    {
        const auto pairs{count_pairs(sequence)};

        if (pairs.counts.empty())
        {
            break;
        }

        const auto& [best_pair, count]{*best_merge(pairs.counts)};

        const auto [tied, disagrees]{account_tie(pairs, best_pair, count)};

        if (tied)
        {
            ++stats.tied_steps;
        }

        if (disagrees)
        {
            ++stats.first_occurrence_disagreements;
        }

        if (count < 2)
        {
            break;
        }

        const auto [left, right]{best_pair};

        table.emplace_back(left, right);

        sequence = apply_merge(std::move(sequence), left, right);
    }

    stats.merges = table.size();

    return trained;
}

/**
 * @brief Returns the 256 single-byte tokens every vocabulary of the paper starts from, in byte order.
 * @return The tokens.
 */
[[nodiscard]] std::vector<std::string> single_byte_tokens()
{
    std::vector<std::string> tokens{};

    for (const auto byte : every_byte())
    {
        tokens.emplace_back(1, byte);
    }

    return tokens;
}

/**
 * @brief Returns a trained vocabulary as the paper holds it: the 256 single bytes and then each merge's concatenation.
 * @param merges The merge table, in order.
 * @param depth How many merges the vocabulary takes, a prefix of the table since the procedure is sequential.
 * @return The token list, 256 + depth long.
 */
[[nodiscard]] std::vector<std::string> local_tokens(const std::vector<Symbol_pair_t>& merges, const std::size_t depth)
{
    auto tokens{single_byte_tokens()};

    for (std::size_t rank{0}; rank < depth; ++rank)
    {
        const auto& [left, right]{merges[rank]};

        tokens.push_back(left + right);
    }

    return tokens;
}

/**
 * @brief Returns the GPT-2 subset as the paper holds it: the 256 single bytes and then the first 4,096 merges of the
 *        release's table, each decoded from the file's byte-to-unicode spelling back to bytes.
 * @param merges_text The text of gpt2-merges.txt.
 * @return The token list, 4,352 long.
 * @throws std::runtime_error If the file is not the one the paper names, by its header, its rule count or a rule's
 *         shape.
 */
[[nodiscard]] std::vector<std::string> build_gpt2(const std::string_view merges_text)
{
    std::vector<std::string_view> lines{};

    for (const auto line : merges_text | std::views::split('\n'))
    {
        lines.emplace_back(line.begin(), line.end());
    }

    if (lines.size() < 2 || lines.front() != "#version: 0.2" || !lines.back().empty())
    {
        throw std::runtime_error{"gpt2-merges.txt must open with the '#version: 0.2' header and end with a newline"};
    }

    // The header line and the empty field after the last newline are no rules.
    const auto rule_count{lines.size() - 2};

    if (rule_count != gpt2_merges)
    {
        throw std::runtime_error{std::format("gpt2-merges.txt carries {} rules, not {}", rule_count, gpt2_merges)};
    }

    // The release's byte-to-unicode map, inverted: the printable bytes stand for themselves and the rest are numbered
    // from U+0100 in byte order.
    std::map<std::uint32_t, unsigned char> byte_of{};

    std::uint32_t extra{byte_count};

    for (const auto symbol : every_byte())
    {
        const std::uint32_t value{static_cast<unsigned char>(symbol)};

        const auto kept{(value >= '!' && value <= '~') || (value >= 0xA1 && value <= 0xAC) || value >= 0xAE};

        const auto point{kept ? value : extra++};

        byte_of[point] = static_cast<unsigned char>(value);
    }

    const auto decode{[&byte_of](const std::string_view field) {
        std::string bytes{};

        for (std::size_t at{0}; at < field.size();)
        {
            const auto lead{static_cast<unsigned char>(field[at])};

            const auto read_lead{[lead]() -> std::pair<std::uint32_t, std::size_t> {
                if (lead < 0x80)
                {
                    return {lead, 1};
                }

                if ((lead & 0xE0) == 0xC0)
                {
                    return {lead & 0x1FU, 2};
                }

                if ((lead & 0xF0) == 0xE0)
                {
                    return {lead & 0x0FU, 3};
                }

                throw std::runtime_error{"gpt2-merges.txt holds a code point beyond the byte map"};
            }};

            const auto [lead_bits, length]{read_lead()};

            auto point{lead_bits};

            for (std::size_t tail{1}; tail < length; ++tail)
            {
                point = (point << 6U) | (static_cast<unsigned char>(field[at + tail]) & 0x3FU);
            }

            bytes.push_back(static_cast<char>(byte_of.at(point)));

            at += length;
        }

        return bytes;
    }};

    auto tokens{single_byte_tokens()};

    for (std::size_t rank{0}; rank < gpt2_prefix; ++rank)
    {
        const auto rule{lines[rank + 1]};

        const auto space{rule.find(' ')};

        if (space == std::string_view::npos || rule.find(' ', space + 1) != std::string_view::npos)
        {
            throw std::runtime_error{std::format("gpt2-merges.txt rule {} is not a pair", rule)};
        }

        const auto left{decode(rule.substr(0, space))};

        const auto right{decode(rule.substr(space + 1))};

        tokens.push_back(left + right);
    }

    return tokens;
}

/**
 * @brief Compiles a token list as a lexer, each distinct token a literal at its own priority in list order.
 * @param tokens The tokens.
 * @return The lexer.
 */
[[nodiscard]] Lexer compile(const std::vector<std::string>& tokens)
{
    munch::core::Builder builder{};

    std::set<std::string> seen{};

    for (const auto& token : tokens)
    {
        const auto [at, inserted]{seen.insert(token)};

        if (inserted)
        {
            const auto id{seen.size() - 1};

            builder.add_token(munch::regex::text(token), id, id);
        }
    }

    return builder.build();
}

/**
 * @brief Returns the bytes some token carries, the paper's alphabet, ascending.
 * @param tokens The tokens.
 * @return The alphabet.
 */
[[nodiscard]] std::vector<unsigned char> alphabet(const std::vector<std::string>& tokens)
{
    std::set<unsigned char> bytes{};

    for (const auto& token : tokens)
    {
        for (const auto byte : token)
        {
            bytes.insert(static_cast<unsigned char>(byte));
        }
    }

    return {bytes.begin(), bytes.end()};
}

/**
 * @brief Returns the width-one pairs of the bytes the lexer certifies exactly, ascending: Lexer::is_split_point() over
 *        every byte.
 * @param lexer The lexer.
 * @return The certified bytes as pairs at origin zero.
 */
[[nodiscard]] std::vector<Pair> certified_bytes(const Lexer& lexer)
{
    std::vector<Pair> pairs{};

    for (const auto byte : every_byte())
    {
        if (lexer.is_split_point(byte))
        {
            pairs.push_back(Pair{.window = std::string(1, byte), .origin = 0});
        }
    }

    return pairs;
}

/**
 * @brief Returns the distinct windows of one width occurring in a text, ascending.
 * @param text The text.
 * @param width The width.
 * @return The windows.
 */
[[nodiscard]] std::set<std::string> windows_of_width(const std::string_view text, const std::size_t width)
{
    std::set<std::string> windows{};

    for (std::size_t at{0}; at + width <= text.size(); ++at)
    {
        windows.emplace(text.substr(at, width));
    }

    return windows;
}

/**
 * @brief Returns the distinct windows of the budget's widths occurring in a text, ascending, the candidate set the
 *        paper decides exhaustively.
 * @param text The text.
 * @return The windows.
 */
[[nodiscard]] std::vector<std::string> occurring_windows(const std::string_view text)
{
    std::set<std::string> windows{};

    for (const auto width : widths)
    {
        windows.merge(windows_of_width(text, width));
    }

    return {windows.begin(), windows.end()};
}

/**
 * @brief Runs a worker on every hardware thread, at least one, and waits for them all.
 * @tparam Worker The worker's type, callable with no arguments.
 * @param worker The worker, copied into each thread.
 */
template <typename Worker>
void on_every_thread(const Worker& worker)
{
    const auto thread_count{std::max(1U, std::jthread::hardware_concurrency())};

    std::vector<std::jthread> threads{};

    for (unsigned thread{0}; thread < thread_count; ++thread)
    {
        threads.emplace_back(worker);
    }
}

/**
 * @brief Decides pairs, exactly by window_counterexample() and conservatively by is_split_window(), the pairs shared
 *        out over every hardware thread.
 * @param lexer The lexer.
 * @param pairs The pairs.
 * @return What each decision certified and the exact decisions the cap stopped, each list sorted.
 */
[[nodiscard]] Decided decide(const Lexer& lexer, const std::vector<Pair>& pairs)
{
    // One slot per pair, so the threads never share one.
    std::vector<Verdict> verdicts(pairs.size(), Verdict::refused);

    std::atomic<std::size_t> next{0};

    const auto verdict_of{[&lexer](const Pair& pair) {
        const auto& [window, origin]{pair};

        const auto [witness, exhaustive]{lexer.window_counterexample(window, origin)};

        if (!exhaustive)
        {
            return Verdict::unsettled;
        }

        return witness.empty() ? Verdict::certified : Verdict::refused;
    }};

    const auto worker{[&] {
        for (auto index{next.fetch_add(1)}; index < pairs.size(); index = next.fetch_add(1))
        {
            verdicts[index] = verdict_of(pairs[index]);
        }
    }};

    on_every_thread(worker);

    Decided decided{};

    for (const auto& [pair, verdict] : std::views::zip(pairs, verdicts))
    {
        if (verdict == Verdict::certified)
        {
            decided.exact.push_back(pair);
        }
        else if (verdict == Verdict::unsettled)
        {
            decided.unsettled.push_back(pair);
        }

        const auto& [window, origin]{pair};

        if (lexer.is_split_window(window) == origin)
        {
            decided.conservative.push_back(pair);
        }
    }

    std::ranges::sort(decided.exact);

    std::ranges::sort(decided.conservative);

    std::ranges::sort(decided.unsettled);

    return decided;
}

/**
 * @brief Returns every origin of every window, the pairs an exhaustive inventory decides.
 * @param windows The windows.
 * @return The pairs, in window order.
 */
[[nodiscard]] std::vector<Pair> every_origin(const std::vector<std::string>& windows)
{
    std::vector<Pair> pairs{};

    for (const auto& window : windows)
    {
        for (std::size_t origin{0}; origin < window.size(); ++origin)
        {
            pairs.push_back(Pair{.window = window, .origin = origin});
        }
    }

    return pairs;
}

/**
 * @brief Counts the pairs every_origin() makes of some windows, one per origin of each.
 * @param windows The windows.
 * @return The sum of their widths.
 */
[[nodiscard]] std::size_t pair_count(const std::vector<std::string>& windows)
{
    const auto window_widths{windows | std::views::transform(std::ranges::size)};

    return std::ranges::fold_left(window_widths, std::size_t{0}, std::plus{});
}

/**
 * @brief Returns two pair lists, one after the other.
 * @param left The first list.
 * @param right The second list.
 * @return The pairs of the first, then those of the second.
 */
[[nodiscard]] std::vector<Pair> concatenated(const std::vector<Pair>& left, const std::vector<Pair>& right)
{
    auto pairs{left};

    pairs.insert(pairs.end(), right.begin(), right.end());

    return pairs;
}

std::vector<Pair> Vocabulary::certified() const
{
    return concatenated(bytes, decided.exact);
}

/**
 * @brief Returns the anchor positions an inventory witnesses in a text: for every occurrence of a certified window the
 *        position origin bytes into it, the text's two ends left out, ascending and distinct.
 * @param certified The inventory, width-one pairs standing for certified bytes.
 * @param text The text.
 * @return The anchors.
 */
[[nodiscard]] std::vector<std::size_t> anchor_positions(const std::vector<Pair>& certified, const std::string_view text)
{
    const Inventory inventory{certified};

    std::vector<bool> anchored(text.size(), false);

    for (std::size_t at{0}; at < text.size(); ++at)
    {
        const auto anchor{[&anchored, at](const std::size_t origin) {
            if (at + origin > 0)
            {
                anchored[at + origin] = true;
            }
        }};

        inventory.origins_at(text, at, anchor);
    }

    const auto is_anchored{[&anchored](const std::size_t at) { return anchored[at]; }};

    std::vector<std::size_t> anchors{};

    std::ranges::copy_if(std::views::iota(std::size_t{0}, anchored.size()), std::back_inserter(anchors), is_anchored);

    return anchors;
}

/**
 * @brief Returns the library's supply figures for an inventory on a text: tools/audit's supply() over a report holding
 *        the certified bytes and the wider pairs, its count held against anchor_positions().
 *
 * With no classes the width-one pairs are the report's exact bytes and the rest its windows; with classes, the
 * campaign's case, every pair is a window over class representatives and the report's classes carry it to every member,
 * the width-one ones included, since a class window's members are not one byte.
 * @param certified The inventory.
 * @param text The text, as the classes read it.
 * @param classes The byte classes the windows stand for, none for byte windows.
 * @return The supply of the bytes and windows together.
 * @throws std::runtime_error If supply() and anchor_positions() disagree on the anchor count.
 */
[[nodiscard]] munch::tools::audit::Anchors library_supply(
        const std::vector<Pair>& certified, const std::string_view text,
        const std::vector<std::vector<unsigned char>>& classes = {})
{
    Report report{};

    report.classes = classes;

    for (const auto& [window, origin] : certified)
    {
        if (window.size() == 1 && classes.empty())
        {
            report.exact.push_back(static_cast<unsigned char>(window.front()));
        }
        else
        {
            report.windows.push_back(Certified_window{.window = window, .origin = origin});
        }
    }

    std::ranges::sort(report.exact);

    const auto [bytes, tokenized, exact, modulo, windows]{munch::tools::audit::supply(report, text)};

    const auto anchors{windows.value_or(exact)};

    // The positions are matched over class representatives where the report matches over classes.
    std::string canonical{text};

    const auto to_representative{[&canonical](const std::vector<unsigned char>& members) {
        const auto in_class{[&members](const char byte) {
            return std::ranges::contains(members, static_cast<unsigned char>(byte));
        }};

        std::ranges::replace_if(canonical, in_class, static_cast<char>(members.front()));
    }};

    std::ranges::for_each(classes, to_representative);

    const auto probe_count{anchor_positions(certified, canonical).size()};

    if (anchors.count != probe_count)
    {
        throw std::runtime_error{"supply() and the probe's anchor positions disagree"};
    }

    return anchors;
}

/**
 * @brief Returns the anchors per KiB as the paper prints them, from the library's supply.
 * @param certified The inventory.
 * @param text The text.
 * @return The density.
 */
[[nodiscard]] double density(const std::vector<Pair>& certified, const std::string_view text)
{
    return library_supply(certified, text).per_kibibyte;
}

/**
 * @brief Returns the longest anchorless stretch: the paper's count of unanchored positions, the runs before the first
 *        anchor and after the last included.
 * @param anchors The anchors, ascending.
 * @param length The text's length.
 * @return The stretch.
 */
[[nodiscard]] std::size_t longest_stretch(const std::vector<std::size_t>& anchors, const std::size_t length)
{
    if (anchors.empty())
    {
        return length;
    }

    auto stretch{std::max(anchors.front(), length - anchors.back() - 1)};

    for (std::size_t index{1}; index < anchors.size(); ++index)
    {
        stretch = std::max(stretch, anchors[index] - anchors[index - 1] - 1);
    }

    return stretch;
}

/**
 * @brief Returns the worker-snap distances: how far each of the ideal cuts for the worker count moves to reach the
 *        nearest anchor.
 * @param anchors The anchors, ascending and non-empty.
 * @param length The input's length.
 * @param workers The worker count.
 * @return The distances, one per ideal cut.
 */
[[nodiscard]] std::vector<std::size_t> snap_distances(
        const std::vector<std::size_t>& anchors, const std::size_t length, const std::size_t workers)
{
    std::vector<std::size_t> distances{};

    for (std::size_t k{1}; k < workers; ++k)
    {
        const auto cut{length * k / workers};

        const auto above{std::ranges::lower_bound(anchors, cut)};

        auto nearest{std::numeric_limits<std::size_t>::max()};

        if (above != anchors.end())
        {
            nearest = std::min(nearest, *above - cut);
        }

        if (above != anchors.begin())
        {
            nearest = std::min(nearest, cut - *std::prev(above));
        }

        distances.push_back(nearest);
    }

    return distances;
}

/**
 * @brief Returns the token starts of the maximal-munch scan of a text, or nothing when the scan does not reach its end.
 * @param lexer The lexer.
 * @param text The text.
 * @return The starts, or std::nullopt.
 */
[[nodiscard]] std::optional<std::vector<std::size_t>> token_starts(const Lexer& lexer, const std::string_view text)
{
    std::vector<std::size_t> starts{};

    std::size_t at{0};

    const auto note_start{[&starts, &at](const std::size_t, const std::size_t length) {
        starts.push_back(at);

        at += length;
    }};

    const auto consumed{lexer.tokenize_all<std::size_t>(text, note_start)};

    if (consumed != text.size())
    {
        return std::nullopt;
    }

    return starts;
}

/**
 * @brief Returns the theorem's q: the least anchor past the edit witnessed by an occurrence lying wholly after it.
 * @param inventory The inventory.
 * @param edited The edited text.
 * @param edit The edited position.
 * @return The anchor, or the text's length when none.
 */
[[nodiscard]] std::size_t least_shared_suffix_anchor(
        const Inventory& inventory, const std::string_view edited, const std::size_t edit)
{
    auto best{edited.size()};

    for (auto start{edit + 1}; start < best; ++start)
    {
        const auto keep_nearest{[&best, start](const std::size_t origin) { best = std::min(best, start + origin); }};

        inventory.origins_at(edited, start, keep_nearest);
    }

    return best;
}

/**
 * @brief Visits the width-five probe of an edit.
 *
 * The probe is every window beginning after the edited position and before the last start whose origin names a position
 * before the anchor, each visited with the position it names.
 * @tparam Visit The visitor's type.
 * @param edited The edited text.
 * @param position The edited position.
 * @param last_start The last start.
 * @param anchor The anchor.
 * @param visit The visitor, given each named position and its window and origin.
 */
template <typename Visit>
void each_probe(
        const std::string_view edited, const std::size_t position, const std::size_t last_start,
        const std::size_t anchor, const Visit& visit)
{
    for (auto start{position + 1}; start < last_start; ++start)
    {
        for (std::size_t origin{0}; origin < probe_width && start + origin < anchor; ++origin)
        {
            visit(start + origin, Pair{.window = std::string{edited.substr(start, probe_width)}, .origin = origin});
        }
    }
}

/**
 * @brief Returns the boundaries an edit moved: the token starts in one scan and not the other.
 * @param before The unedited scan's token starts.
 * @param starts The edited scan's token starts.
 * @return The moved boundaries, ascending.
 */
[[nodiscard]] std::vector<std::size_t> moved_boundaries(
        const std::set<std::size_t>& before, const std::vector<std::size_t>& starts)
{
    const std::set<std::size_t> after{starts.begin(), starts.end()};

    std::vector<std::size_t> moved{};

    std::ranges::set_symmetric_difference(before, after, std::back_inserter(moved));

    return moved;
}

/**
 * @brief Returns the span of a list of moved boundaries.
 * @param moved The moved boundaries, ascending.
 * @return The span from the first to the last, zero when none moved.
 */
[[nodiscard]] std::size_t hull_of(const std::vector<std::size_t>& moved)
{
    return moved.empty() ? 0 : moved.back() - moved.front() + 1;
}

/**
 * @brief Checks that every moved boundary lies inside the theorem's window (max(a, p - L + 1), q): the first boundary
 *        past p - L is shared by both scans.
 * @param moved The moved boundaries.
 * @param position The edited position, the theorem's p.
 * @param longest The longest token's length, the theorem's L.
 * @param anchor The theorem's q.
 * @throws std::runtime_error If a moved boundary leaves the window, which would refute the theorem.
 */
void check_theorem_window(
        const std::vector<std::size_t>& moved, const std::size_t position, const std::size_t longest,
        const std::size_t anchor)
{
    for (const auto boundary : moved)
    {
        if (boundary + longest <= position + 1 || boundary >= anchor)
        {
            throw std::runtime_error{std::format("a moved boundary at {} lies outside the theorem's window", boundary)};
        }
    }
}

/**
 * @brief Returns the pairs in ascending order, each once.
 * @param pairs The pairs.
 * @return The sorted distinct pairs.
 */
[[nodiscard]] std::vector<Pair> sorted_distinct(std::vector<Pair> pairs)
{
    std::ranges::sort(pairs);

    const auto repeats{std::ranges::unique(pairs)};

    pairs.erase(repeats.begin(), repeats.end());

    return pairs;
}

/**
 * @brief Runs the width-five probe of an edit: every window beginning after the edit whose origin would name an anchor
 *        before the one the budget found, decided once each across the trials.
 * @param lexer The lexer.
 * @param edited The edited text.
 * @param position The edited position.
 * @param last_start The last start the probe takes.
 * @param anchor The anchor the budget found.
 * @param memo The decisions of earlier probes, by pair, which this probe's undecided pairs join.
 * @return The earliest position a certified probe pair names, or nothing.
 * @throws std::runtime_error If a probe decision is unsettled at the cap.
 */
[[nodiscard]] std::optional<std::size_t> probe_anchor(
        const Lexer& lexer, const std::string_view edited, const std::size_t position, const std::size_t last_start,
        const std::size_t anchor, std::map<Pair, bool>& memo)
{
    std::vector<Pair> undecided{};

    const auto collect_undecided{[&memo, &undecided](const std::size_t, const Pair& pair) {
        if (!memo.contains(pair))
        {
            undecided.push_back(pair);
        }
    }};

    each_probe(edited, position, last_start, anchor, collect_undecided);

    undecided = sorted_distinct(std::move(undecided));

    if (!undecided.empty())
    {
        const auto [exact, conservative, unsettled]{decide(lexer, undecided)};

        if (!unsettled.empty())
        {
            throw std::runtime_error{"a width-five probe decision was unsettled at the cap"};
        }

        for (const auto& pair : undecided)
        {
            memo[pair] = std::ranges::binary_search(exact, pair);
        }
    }

    std::optional<std::size_t> earlier{};

    const auto keep_earliest{[&memo, &earlier](const std::size_t named, const Pair& pair) {
        if (memo.at(pair) && (!earlier || named < *earlier))
        {
            earlier = named;
        }
    }};

    each_probe(edited, position, last_start, anchor, keep_earliest);

    return earlier;
}

/**
 * @brief Replays the edit trials of splitting_measurements.py over munch: the same seeded draws, each accepted edit
 *        retokenized by the lexer, the moved boundaries held inside the theorem's window under the ceiling inventory,
 *        the frozen inventory's endpoint beside it, and the width-five probe decided by window_counterexample().
 * @param lexer The lexer.
 * @param longest The longest token's length, the theorem's L.
 * @param sigma The alphabet the replacement byte is drawn from, ascending.
 * @param ceiling The ceiling inventory.
 * @param frozen The frozen inventory.
 * @param sample The evaluation slice.
 * @return The records, in draw order.
 * @throws std::runtime_error If a moved boundary leaves the theorem's window, which would refute the theorem.
 */
[[nodiscard]] std::vector<Edit_record> edit_trials(
        const Lexer& lexer, const std::size_t longest, const std::vector<unsigned char>& sigma,
        const std::vector<Pair>& ceiling, const std::vector<Pair>& frozen, const std::string_view sample)
{
    const auto sequential{token_starts(lexer, sample)};

    if (!sequential)
    {
        throw std::runtime_error{"the evaluation slice does not tokenize"};
    }

    const std::set<std::size_t> before{sequential->begin(), sequential->end()};

    const Inventory ceiling_index{ceiling};

    const Inventory frozen_index{frozen};

    Python_random chooser{edit_seed};

    std::map<Pair, bool> memo{};

    std::vector<Edit_record> records{};

    std::size_t measured{0};

    for (std::size_t attempts{0}; measured < edit_trials_wanted && attempts < edit_attempts_cap; ++attempts)
    {
        const auto position{chooser.randrange(sample.size())};

        const auto drawn{chooser.randrange(sigma.size())};

        const auto other{sigma[drawn]};

        Edit_record record{.position = position, .new_byte = other};

        if (other == static_cast<unsigned char>(sample[position]))
        {
            records.push_back(record);

            continue;
        }

        std::string edited{sample};

        edited[position] = static_cast<char>(other);

        const auto starts{token_starts(lexer, edited)};

        if (!starts)
        {
            record.untokenizable = true;

            records.push_back(record);

            continue;
        }

        const auto moved{moved_boundaries(before, *starts)};

        const auto anchor{least_shared_suffix_anchor(ceiling_index, edited, position)};

        check_theorem_window(moved, position, longest, anchor);

        const auto last_start{std::min(anchor, sample.size() - probe_width + 1)};

        const auto earlier{probe_anchor(lexer, edited, position, last_start, anchor, memo)};

        const auto reach_back{position + 2 > longest ? position + 2 - longest : 0};

        const auto low{std::max<std::size_t>(1, reach_back)};

        const auto hull{hull_of(moved)};

        const auto frozen_anchor{least_shared_suffix_anchor(frozen_index, edited, position)};

        const auto admissible{anchor - low};

        const auto unoccupied{admissible - moved.size()};

        const Edit_record accepted{
                .position = position,
                .new_byte = other,
                .accepted = true,
                .untokenizable = false,
                .left_admissible = low,
                .right_anchor = anchor,
                .frozen_anchor = frozen_anchor,
                .moved = moved.size(),
                .hull = hull,
                .admissible = admissible,
                .unoccupied = unoccupied,
                .probe_anchor = earlier};

        records.push_back(accepted);

        ++measured;
    }

    return records;
}

/**
 * @brief Appends lines to an emission's lines, in order.
 * @param lines The emission's lines so far.
 * @param more The lines appended.
 */
void append(std::vector<std::string>& lines, std::vector<std::string> more)
{
    std::ranges::move(more, std::back_inserter(lines));
}

/**
 * @brief Returns the paper's edit lines, computed from the records as edit_records.py computes them.
 * @param name The vocabulary's name.
 * @param records The records.
 * @return The lines.
 */
[[nodiscard]] std::vector<std::string> report_edits(
        const std::string_view name, const std::vector<Edit_record>& records)
{
    std::vector<Edit_record> accepted{};

    std::size_t untokenizable{0};

    std::size_t repeated{0};

    for (const auto& record : records)
    {
        if (record.accepted)
        {
            accepted.push_back(record);
        }
        else if (record.untokenizable)
        {
            ++untokenizable;
        }
        else
        {
            ++repeated;
        }
    }

    const auto column{[&accepted]<typename Field>(const Field& field) {
        std::vector<std::size_t> values{};

        std::ranges::transform(accepted, std::back_inserter(values), field);

        return values;
    }};

    const auto changed_column{[&accepted]<typename Field>(const Field& field) {
        std::vector<std::size_t> values{};

        for (const auto& record : accepted)
        {
            if (record.moved > 0)
            {
                values.push_back(std::invoke(field, record));
            }
        }

        return values;
    }};

    const auto hulls{column(&Edit_record::hull)};

    const auto conditional{changed_column(&Edit_record::hull)};

    const auto moved{column(&Edit_record::moved)};

    const auto moved_changed{changed_column(&Edit_record::moved)};

    const auto slack_of{[](const Edit_record& record) { return record.admissible - record.hull; }};

    const auto slacks{column(slack_of)};

    const auto unoccupied{column(&Edit_record::unoccupied)};

    std::vector<std::string> lines{};

    lines.push_back(std::format(
            "{} | edits measured: {} accepted of {} attempted, {} refused because the edited input stopped tokenizing "
            "and {} because the draw repeated the original byte",
            name, accepted.size(), records.size(), untokenizable, repeated));
    lines.push_back(std::format(
            "{} | edits moving no boundary: {} of {}", name, accepted.size() - conditional.size(), accepted.size()));
    lines.push_back(std::format("{} | boundary-difference hull p50: {}", name, percentile(hulls, 0.5)));
    lines.push_back(std::format("{} | boundary-difference hull p90: {}", name, percentile(hulls, 0.9)));
    lines.push_back(std::format("{} | boundary-difference hull max: {}", name, std::ranges::max(hulls)));
    lines.push_back(std::format("{} | boundary-difference hull mean: {}", name, mean(hulls)));
    lines.push_back(std::format(
            "{} | boundary-difference hull conditional on a change: n {}, p50 {}, p90 {}, max {}, mean {}", name,
            conditional.size(), percentile(conditional, 0.5), percentile(conditional, 0.9),
            std::ranges::max(conditional), mean(conditional)));
    lines.push_back(std::format(
            "{} | moved boundaries per edit: mean {} conditional on a change, {} unconditionally", name,
            mean(moved_changed), mean(moved)));
    lines.push_back(std::format("{} | hull slack p50: {}", name, percentile(slacks, 0.5)));
    lines.push_back(std::format("{} | hull slack p90: {}", name, percentile(slacks, 0.9)));
    lines.push_back(std::format("{} | unoccupied admissible p50: {}", name, percentile(unoccupied, 0.5)));
    lines.push_back(std::format("{} | unoccupied admissible p90: {}", name, percentile(unoccupied, 0.9)));

    const auto improves{[](const Edit_record& record) { return record.probe_anchor.has_value(); }};

    const auto improved{std::ranges::count_if(accepted, improves)};

    lines.push_back(std::format(
            "{} | trials whose anchor improves at width five: {} of {}, each an occurrence lying wholly in the "
            "unchanged suffix",
            name, improved, accepted.size()));

    return lines;
}

/**
 * @brief Returns the paper's paired comparison of the two vocabularies' trials, trial by trial.
 * @param left_name The first vocabulary's name.
 * @param left The first vocabulary's records.
 * @param right_name The second vocabulary's name.
 * @param right The second vocabulary's records.
 * @return The lines.
 * @throws std::runtime_error If the accepted trials do not pair at the same edits.
 */
[[nodiscard]] std::vector<std::string> measure_pairing(
        const std::string_view left_name, const std::vector<Edit_record>& left, const std::string_view right_name,
        const std::vector<Edit_record>& right)
{
    const auto accepted_of{[](const std::vector<Edit_record>& records) {
        std::vector<Edit_record> accepted{};

        std::ranges::copy_if(records, std::back_inserter(accepted), &Edit_record::accepted);

        return accepted;
    }};

    const auto ones{accepted_of(left)};

    const auto twos{accepted_of(right)};

    if (ones.size() != twos.size())
    {
        throw std::runtime_error{"the trials must pair"};
    }

    std::vector<long> hull_diffs{};

    std::vector<long> width_diffs{};

    std::map<std::pair<bool, bool>, std::size_t> movement{};

    for (const auto& [one, two] : std::views::zip(ones, twos))
    {
        if (one.position != two.position || one.new_byte != two.new_byte)
        {
            throw std::runtime_error{"the trials must pair at the same edit"};
        }

        hull_diffs.push_back(static_cast<long>(one.hull) - static_cast<long>(two.hull));

        width_diffs.push_back(static_cast<long>(one.admissible) - static_cast<long>(two.admissible));

        ++movement[{one.moved > 0, two.moved > 0}];
    }

    const auto count_where{
            [&hull_diffs]<typename Test>(const Test test) { return std::ranges::count_if(hull_diffs, test); }};

    const auto is_negative{[](const long difference) { return difference < 0; }};

    const auto is_zero{[](const long difference) { return difference == 0; }};

    const auto is_positive{[](const long difference) { return difference > 0; }};

    const auto negative{count_where(is_negative)};

    const auto zero{count_where(is_zero)};

    const auto positive{count_where(is_positive)};

    std::vector<std::string> lines{};

    lines.push_back(std::format(
            "{} against {} | paired hull difference p50: {}, p90 {} over {} paired trials", left_name, right_name,
            percentile(hull_diffs, 0.5), percentile(hull_diffs, 0.9), hull_diffs.size()));
    lines.push_back(std::format(
            "{} against {} | paired hull difference sign: negative {}, zero {}, positive {} of {}", left_name,
            right_name, negative, zero, positive, hull_diffs.size()));
    lines.push_back(std::format(
            "{} against {} | paired admissible-width difference p50: {}, p90 {}", left_name, right_name,
            percentile(width_diffs, 0.5), percentile(width_diffs, 0.9)));
    lines.push_back(std::format(
            "{} against {} | paired movement: both move {}, {} only {}, {} only {}, neither {} of {}", left_name,
            right_name, movement[{true, true}], left_name, movement[{true, false}], right_name, movement[{false, true}],
            movement[{false, false}], hull_diffs.size()));

    return lines;
}

/**
 * @brief Returns the edit-slack table's rows for one vocabulary: hull slack and unoccupied count under the ceiling and
 *        the frozen inventory, as write_slack_table() computes them.
 * @param label The row label.
 * @param records The records.
 * @return The two rows.
 */
[[nodiscard]] std::vector<std::string> slack_rows(const std::string_view label, const std::vector<Edit_record>& records)
{
    std::vector<std::size_t> ceiling_slack{};

    std::vector<std::size_t> frozen_slack{};

    std::vector<std::size_t> ceiling_unoccupied{};

    std::vector<std::size_t> frozen_unoccupied{};

    for (const auto& record : records)
    {
        if (!record.accepted)
        {
            continue;
        }

        const auto frozen_width{record.frozen_anchor - record.left_admissible};

        ceiling_slack.push_back(record.admissible - record.hull);

        frozen_slack.push_back(frozen_width - record.hull);

        ceiling_unoccupied.push_back(record.unoccupied);

        frozen_unoccupied.push_back(frozen_width - record.moved);
    }

    const auto slack_line{std::format(
            R"({} & hull slack & {} & {} & {} & {} \\)", label, percentile(ceiling_slack, 0.5),
            percentile(ceiling_slack, 0.9), percentile(frozen_slack, 0.5), percentile(frozen_slack, 0.9))};

    const auto unoccupied_line{std::format(
            R"({} & unoccupied count & {} & {} & {} & {} \\)", label, percentile(ceiling_unoccupied, 0.5),
            percentile(ceiling_unoccupied, 0.9), percentile(frozen_unoccupied, 0.5),
            percentile(frozen_unoccupied, 0.9))};

    return {slack_line, unoccupied_line};
}

/**
 * @brief Returns the n-grams of one width ranked as Counter.most_common ranks them: by frequency over every occurrence,
 *        ties in order of first occurrence.
 * @param text The text.
 * @param width The width.
 * @return The n-grams with their counts, ranked.
 */
[[nodiscard]] std::vector<std::pair<std::string, std::size_t>> frequency_ranking(
        const std::string_view text, const std::size_t width)
{
    using Entry_t = std::pair<std::string, Occurrence_count>;

    std::map<std::string, Occurrence_count, std::less<>> counts{};

    for (std::size_t at{0}; at + width <= text.size(); ++at)
    {
        auto& [count, first]{counts[std::string{text.substr(at, width)}]};

        if (count == 0)
        {
            first = at;
        }

        ++count;
    }

    std::vector<Entry_t> entries{counts.begin(), counts.end()};

    const auto ranks_before{[](const Entry_t& left, const Entry_t& right) {
        const auto& [left_window, left_tally]{left};

        const auto& [right_window, right_tally]{right};

        const auto& [left_count, left_first]{left_tally};

        const auto& [right_count, right_first]{right_tally};

        return left_count != right_count ? left_count > right_count : left_first < right_first;
    }};

    std::ranges::sort(entries, ranks_before);

    std::vector<std::pair<std::string, std::size_t>> ranked{};

    for (const auto& [window, tally] : entries)
    {
        const auto& [count, first]{tally};

        ranked.emplace_back(window, count);
    }

    return ranked;
}

/**
 * @brief Joins texts with a separator between each two.
 * @param parts The texts, in order.
 * @param separator The separator.
 * @return The joined text.
 */
[[nodiscard]] std::string joined(const std::vector<std::string>& parts, const std::string_view separator)
{
    std::string text{};

    std::ranges::copy(parts | std::views::join_with(separator), std::back_inserter(text));

    return text;
}

/**
 * @brief Selects the most frequent n-grams of each width, with the head and band each width's tie leaves.
 * @param sample The slice.
 * @return The selection.
 */
[[nodiscard]] Sampled_selection select_sampled(const std::string_view sample)
{
    Sampled_selection selection{};

    auto& [chosen, heads, bands, slots]{selection};

    for (const auto width : widths)
    {
        const auto ranked{frequency_ranking(sample, width)};

        for (const auto& [window, count] : ranked | std::views::take(sampled_per_width))
        {
            chosen.push_back(window);
        }

        // With no more than the slots ranked every n-gram is in the head; otherwise the head ranks above the last
        // slot's count and the band ties it.
        const auto all_fit{ranked.size() <= sampled_per_width};

        const auto cutoff_of{[&ranked, all_fit] {
            if (all_fit)
            {
                return std::size_t{0};
            }

            const auto& [window, count]{ranked[sampled_per_width - 1]};

            return count;
        }};

        const auto cutoff{cutoff_of()};

        std::vector<std::string> head{};

        std::vector<std::string> band{};

        for (const auto& [window, count] : ranked)
        {
            if (all_fit || count > cutoff)
            {
                head.push_back(window);
            }
            else if (count == cutoff)
            {
                band.push_back(window);
            }
        }

        slots.push_back(sampled_per_width - head.size());

        heads.push_back(std::move(head));

        bands.push_back(std::move(band));
    }

    return selection;
}

/**
 * @brief Decides every window a selection could pick once, then scores every selection consistent with the ties: the
 *        product of the bands' combinations, one width's choice at a time.
 * @param lexer The lexer.
 * @param sample The slice.
 * @param bytes The certified bytes as pairs.
 * @param selection The selection's heads, bands and slots.
 * @return The scores.
 * @throws std::runtime_error If a decision is unsettled at the cap.
 */
[[nodiscard]] Tie_family score_tie_family(
        const Lexer& lexer, const std::string_view sample, const std::vector<Pair>& bytes,
        const Sampled_selection& selection)
{
    const auto& [chosen, heads, bands, slots]{selection};

    std::vector<std::string> contested{};

    std::ranges::copy(heads | std::views::join, std::back_inserter(contested));

    std::ranges::copy(bands | std::views::join, std::back_inserter(contested));

    const auto contested_pairs{every_origin(contested)};

    const auto [contested_exact, contested_conservative, contested_unsettled]{decide(lexer, contested_pairs)};

    if (!contested_unsettled.empty())
    {
        throw std::runtime_error{"a tie-family decision was unsettled at the cap"};
    }

    const Inventory certified_of{contested_exact};

    Tie_family family{};

    auto& [counts, wider_supply, whole_supply, selections]{family};

    const auto score{[&](const std::vector<std::string>& selected_windows) {
        std::vector<Pair> selected{};

        for (const auto& window : selected_windows)
        {
            const auto found{certified_of.origins_of.find(window)};

            if (found == certified_of.origins_of.end())
            {
                continue;
            }

            const auto& [found_window, origins]{*found};

            for (const auto origin : origins)
            {
                selected.push_back(Pair{.window = window, .origin = origin});
            }
        }

        counts.insert(selected.size());

        wider_supply.insert(density(selected, sample));

        const auto whole{concatenated(bytes, selected)};

        whole_supply.insert(density(whole, sample));

        ++selections;
    }};

    std::vector<std::string> selected_windows{};

    std::ranges::copy(heads | std::views::join, std::back_inserter(selected_windows));

    const auto choose{[&]<typename Self>(const Self& self, const std::size_t width_index) -> void {
        if (width_index == bands.size())
        {
            score(selected_windows);

            return;
        }

        const auto& band{bands[width_index]};

        const auto free{slots[width_index]};

        std::vector<Taken> take(band.size(), Taken::no);

        std::ranges::fill(take | std::views::take(free), Taken::yes);

        do
        {
            const auto mark{selected_windows.size()};

            for (const auto& [member, taken] : std::views::zip(band, take))
            {
                if (taken == Taken::yes)
                {
                    selected_windows.push_back(member);
                }
            }

            self(self, width_index + 1);

            selected_windows.resize(mark);
        } while (std::ranges::prev_permutation(take).found);
    }};

    choose(choose, 0);

    return family;
}

/**
 * @brief Renders one density, or the lowest and highest, as the tie-invariance line prints them.
 * @param values The densities.
 * @return The rendering.
 */
[[nodiscard]] std::string spread(const std::set<double>& values)
{
    if (values.size() == 1)
    {
        return exact_density(*values.begin());
    }

    return std::format("{} to {}", exact_density(*values.begin()), exact_density(*values.rbegin()));
}

/**
 * @brief Returns the paper's sampled-inventory lines: the twenty most frequent n-grams per width decided, the supply
 *        they give, and the sensitivity of that figure to the tie at the twentieth rank, every tied selection scored.
 * @param name The vocabulary's name.
 * @param lexer The lexer.
 * @param sample The slice.
 * @param bytes The certified bytes as pairs.
 * @return The four lines.
 */
[[nodiscard]] std::vector<std::string> sampled_inventory_lines(
        const std::string_view name, const Lexer& lexer, const std::string_view sample, const std::vector<Pair>& bytes)
{
    const auto selection{select_sampled(sample)};

    const auto& [chosen, heads, bands, slots]{selection};

    const auto chosen_pairs{every_origin(chosen)};

    const auto [pairs, conservative, unsettled]{decide(lexer, chosen_pairs)};

    if (!unsettled.empty())
    {
        throw std::runtime_error{"a sampled-inventory decision was unsettled at the cap"};
    }

    const auto with_bytes{concatenated(bytes, pairs)};

    const auto decided_count{chosen_pairs.size()};

    std::vector<std::string> lines{};

    lines.push_back(std::format(
            "{} | sampled inventory supply: {} certified of {} decided over {} candidates, the {} most frequent slice "
            "n-grams per width, {:.1f} anchors per KiB against the exhaustive inventory's own figure",
            name, pairs.size(), decided_count, chosen.size(), sampled_per_width, density(pairs, sample)));
    lines.push_back(std::format(
            "{} | sampled inventory with the certified bytes: {:.1f} anchors per KiB, the sampled wider half added to "
            "the exact singleton half",
            name, density(with_bytes, sample)));

    const auto [counts, wider_supply, whole_supply, selections]{score_tie_family(lexer, sample, bytes, selection)};

    std::vector<std::string> band_parts{};

    for (const auto& [width, band, slot] : std::views::zip(widths, bands, slots))
    {
        band_parts.push_back(
                std::format("width {}: {} tied for {} of the {} slots", width, band.size(), slot, sampled_per_width));
    }

    const auto band_sizes{joined(band_parts, "; ")};

    lines.push_back(std::format(
            "{} | sampled inventory selection rule: rank distinct n-grams by frequency over every occurrence, the "
            "final n-gram included, and break frequency ties by first occurrence; the rank-{} frequency is itself "
            "tied at every width ({}), and over all {} selections consistent with those ties the certified-pair count "
            "runs from {} to {}",
            name, sampled_per_width, band_sizes, selections, *counts.begin(), *counts.rbegin()));

    std::vector<std::string> moved{};

    if (counts.size() > 1)
    {
        moved.emplace_back("the certified-pair count");
    }

    if (wider_supply.size() > 1 || whole_supply.size() > 1)
    {
        moved.emplace_back("the supply behind it");
    }

    const auto verdict{
            moved.empty() ? std::string{"neither the certified-pair count nor the supply behind it"} :
                            joined(moved, " and ")};

    lines.push_back(std::format(
            "{} | sampled inventory tie invariance: over all {} selections the sampled wider half supplies {} anchors "
            "per KiB and the same half added to the exact certified bytes supplies {}, so the tie rule moves {}",
            name, selections, spread(wider_supply), spread(whole_supply), verdict));

    return lines;
}

/**
 * @brief Returns the two newline lines: whether a cut before every token's newline, where one has any, is sound, which
 *        holds when no token carries the byte past its first position, the byte certificate, and whether a cut after it
 *        is, which holds when no token carries the byte before its last position, read off the compiled tables as the
 *        state a newline leads a live state into having no live move onward.
 * @param lexer The lexer.
 * @param name The row's name.
 * @return The two lines, none when no token holds a newline.
 */
[[nodiscard]] std::vector<std::string> newline_lines(const Lexer& lexer, const std::string_view name)
{
    const auto& simulator{lexer.simulator()};

    bool consumed{false};

    bool after_sound{true};

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (!simulator.is_live(state))
        {
            continue;
        }

        const auto into{simulator.step(state, '\n')};

        if (!into || !simulator.is_live(*into))
        {
            continue;
        }

        consumed = true;

        const auto live_onward{[&simulator, &into](const char byte) {
            const auto onward{simulator.step(*into, static_cast<unsigned char>(byte))};

            return onward && simulator.is_live(*onward);
        }};

        if (std::ranges::any_of(every_byte(), live_onward))
        {
            after_sound = false;
        }
    }

    if (!consumed)
    {
        return {};
    }

    const auto at_sound{lexer.is_split_point('\n')};

    const auto at_line{std::format("{} | newline at-split sound: {}", name, at_sound ? "yes" : "no")};

    const auto after_line{std::format("{} | newline after-split sound: {}", name, after_sound ? "yes" : "no")};

    return {at_line, after_line};
}

/**
 * @brief Returns the sync-distance line: anchor_free_span() over the certified bytes, the paper's gap corollary.
 * @param lexer The lexer.
 * @param name The row's name.
 * @return The line.
 */
[[nodiscard]] std::string sync_line(const Lexer& lexer, const std::string_view name)
{
    const auto span{lexer.anchor_free_span()};

    const auto distance{span ? std::to_string(*span) : std::string{"unbounded, a quiet cycle exists"}};

    return std::format("{} | sync distance: {}", name, distance);
}

/**
 * @brief Builds the UTF-8 shape's token set, the paper's designed contrast: one token per encoded length, with letters
 *        standing for the lead bytes and c for a continuation byte.
 * @return The lexer.
 */
[[nodiscard]] Lexer utf8_shape()
{
    return compile({"a", "2c", "3cc", "4ccc"});
}

/**
 * @brief Compiles a vocabulary and decides every window occurring in the slice at every origin.
 * @param name The name.
 * @param label The table label.
 * @param tokens The tokens.
 * @param sample The evaluation slice.
 * @return The vocabulary.
 */
[[nodiscard]] Vocabulary measure_vocabulary(
        const std::string_view name, const std::string_view label, std::vector<std::string> tokens,
        const std::string_view sample)
{
    auto lexer{compile(tokens)};

    note(std::format("{}: {} tokens compiled to {} states", name, tokens.size(), lexer.simulator().state_count()));

    auto bytes{certified_bytes(lexer)};

    auto windows{occurring_windows(sample)};

    const auto pairs{every_origin(windows)};

    auto decided{decide(lexer, pairs)};

    note(std::format(
            "{}: {} windows decided, {} pairs certified exactly, {} by the conservative model, {} unsettled", name,
            windows.size(), decided.exact.size(), decided.conservative.size(), decided.unsettled.size()));

    return Vocabulary{
            .name = std::string{name},
            .label = std::string{label},
            .tokens = std::move(tokens),
            .lexer = std::move(lexer),
            .bytes = std::move(bytes),
            .windows = std::move(windows),
            .decided = std::move(decided)};
}

/**
 * @brief Returns the supply of one inventory as the paper prints it for a named row: anchors per KiB, the gap figures
 *        and the longest anchorless stretch.
 * @param name The row's name.
 * @param certified The inventory.
 * @param text The slice.
 * @return The lines, the anchors and the figures.
 */
[[nodiscard]] Supply_report supply_lines(
        const std::string_view name, const std::vector<Pair>& certified, const std::string_view text)
{
    auto anchors{anchor_positions(certified, text)};

    const auto supply{library_supply(certified, text)};

    const auto& [count, per_kibibyte, gaps]{supply};

    std::vector<std::string> lines{std::format("{} | anchors per KiB: {:.1f}", name, per_kibibyte)};

    if (gaps)
    {
        lines.push_back(std::format("{} | anchor gap p50: {}", name, gaps->median));
        lines.push_back(std::format("{} | anchor gap p90: {}", name, gaps->ninetieth));
        lines.push_back(std::format("{} | anchor gap max: {}", name, gaps->longest));
    }

    const auto stretch{longest_stretch(anchors, text.size())};

    lines.push_back(std::format("{} | longest anchorless stretch: {}", name, stretch));

    return Supply_report{.lines = std::move(lines), .anchors = std::move(anchors), .supply = supply};
}

/**
 * @brief Returns the inventory through a window budget: the certified bytes and the exact pairs no wider than it, in
 *        the exact pairs' order.
 * @param bytes The certified bytes, as pairs.
 * @param exact The exact wider pairs.
 * @param budget The widest window kept.
 * @return The inventory.
 */
[[nodiscard]] std::vector<Pair> pairs_through(
        const std::vector<Pair>& bytes, const std::vector<Pair>& exact, const std::size_t budget)
{
    auto through{bytes};

    for (const auto& pair : exact)
    {
        const auto& [window, origin]{pair};

        if (window.size() <= budget)
        {
            through.push_back(pair);
        }
    }

    return through;
}

/**
 * @brief Returns the frozen inventory as the edit trials and the transfer see it: the certified bytes and the exact
 *        pairs whose windows the calibration slice holds, in the ceiling inventory's order.
 * @param vocabulary The vocabulary.
 * @param calibration_windows The windows occurring in the calibration slice, ascending.
 * @return The inventory.
 */
[[nodiscard]] std::vector<Pair> frozen_inventory(
        const Vocabulary& vocabulary, const std::vector<std::string>& calibration_windows)
{
    auto frozen{vocabulary.bytes};

    for (const auto& pair : vocabulary.decided.exact)
    {
        const auto& [window, origin]{pair};

        if (std::ranges::binary_search(calibration_windows, window))
        {
            frozen.push_back(pair);
        }
    }

    return frozen;
}

/**
 * @brief Counts the anchors inside one block of the slice.
 * @param anchors The anchors.
 * @param block The block's first position.
 * @return How many lie in [block, block + block_bytes).
 */
[[nodiscard]] std::size_t anchors_in_block(const std::vector<std::size_t>& anchors, const std::size_t block)
{
    const auto inside{[block](const std::size_t anchor) { return block <= anchor && anchor < block + block_bytes; }};

    return static_cast<std::size_t>(std::ranges::count_if(anchors, inside));
}

/**
 * @brief Returns the first positions of the slice's whole blocks, ascending.
 * @param size The slice's length.
 * @return The positions.
 */
[[nodiscard]] std::vector<std::size_t> block_starts(const std::size_t size)
{
    std::vector<std::size_t> starts{};

    for (std::size_t start{0}; start + block_bytes <= size; start += block_bytes)
    {
        starts.push_back(start);
    }

    return starts;
}

/**
 * @brief Returns a vocabulary's supply by window budget: the density of the certified bytes and the exact pairs through
 *        each budget, and the wider half the pairs add beyond the bytes.
 * @param vocabulary The vocabulary.
 * @param sample The evaluation slice.
 * @return The two lines.
 */
[[nodiscard]] std::vector<std::string> budget_curve_lines(const Vocabulary& vocabulary, const std::string_view sample)
{
    const auto& [name, label, tokens, lexer, bytes, windows, decided]{vocabulary};

    std::vector<double> curve{};

    std::vector<std::string> curve_parts{};

    for (std::size_t budget{1}; budget <= widths.back(); ++budget)
    {
        const auto through{pairs_through(bytes, decided.exact, budget)};

        curve.push_back(density(through, sample));

        curve_parts.push_back(std::format("H={}: {:.1f}", budget, curve.back()));
    }

    std::vector<std::string> lines{};

    lines.push_back(std::format(
            "{} | supply by window budget: {} anchors per KiB, each row exact for the slice through its budget", name,
            joined(curve_parts, ", ")));
    lines.push_back(std::format(
            "{} | wider half beyond the certified bytes: {:.1f} anchors per KiB", name, curve.back() - curve.front()));

    return lines;
}

/**
 * @brief Returns the worst and the median snap of a vocabulary's anchors at each of snap_workers.
 * @param name The name the lines carry.
 * @param anchors The anchors, ascending.
 * @param length The slice's length.
 * @return The lines and the worst snap at the first of snap_workers.
 */
[[nodiscard]] Snap_lines snap_lines(
        const std::string_view name, const std::vector<std::size_t>& anchors, const std::size_t length)
{
    Snap_lines snaps{};

    auto& [lines, table_snap_max]{snaps};

    for (const auto workers : snap_workers)
    {
        const auto distances{snap_distances(anchors, length, workers)};

        const auto snap_max{std::ranges::max(distances)};

        if (workers == snap_workers.front())
        {
            table_snap_max = snap_max;
        }

        lines.push_back(std::format("{} | snap max at {} workers: {}", name, workers, snap_max));
        lines.push_back(std::format("{} | snap p50 at {} workers: {}", name, workers, percentile(distances, 0.5)));
    }

    return snaps;
}

/**
 * @brief Returns the spread of a vocabulary's anchors over the slice's blocks of block_bytes.
 * @param name The name the line carries.
 * @param anchors The anchors, ascending.
 * @param length The slice's length.
 * @return The line.
 */
[[nodiscard]] std::string block_supply_line(
        const std::string_view name, const std::vector<std::size_t>& anchors, const std::size_t length)
{
    std::vector<std::size_t> per_block{};

    for (const auto start : block_starts(length))
    {
        per_block.push_back(anchors_in_block(anchors, start));
    }

    return std::format(
            "{} | anchors per block over {} blocks of {} bytes: {} to {}, p50 {}", name, per_block.size(), block_bytes,
            std::ranges::min(per_block), std::ranges::max(per_block), percentile(per_block, 0.5));
}

/**
 * @brief Returns the length of the longest token of a list.
 * @param tokens The tokens, not empty.
 * @return The longest token's length.
 */
[[nodiscard]] std::size_t longest_length(const std::vector<std::string>& tokens)
{
    return std::ranges::max(tokens | std::views::transform(std::ranges::size));
}

/**
 * @brief Returns one vocabulary's evaluation-slice figures: its configuration, the certified bytes and window pairs,
 *        the supply by window budget, the supply and snap figures, the edit trials, the supply per block, the sampled
 *        inventory, the sync distance and the newline lines.
 * @param vocabulary The vocabulary.
 * @param sample The evaluation slice.
 * @param calibration The calibration slice.
 * @return The vocabulary's lines and rows.
 */
[[nodiscard]] Vocabulary_report vocabulary_lines(
        const Vocabulary& vocabulary, const std::string_view sample, const std::string_view calibration)
{
    const auto& [name, label, tokens, lexer, bytes, windows, decided]{vocabulary};

    std::vector<std::string> lines{};

    lines.push_back(std::format(
            "{} | configuration: {} tokens digest {}, evaluation slice bytes [{}, {}) digest {}, "
            "window budget widths {} to {}",
            name, tokens.size(), token_digest(tokens), train_bytes, train_bytes + sample.size(),
            sha256(sample).substr(0, digest_prefix_digits), widths.front(), widths.back()));
    lines.push_back(std::format(
            "{} | certified bytes: {} of {} alphabet bytes, exact by the interior-byte lemma", name, bytes.size(),
            alphabet(tokens).size()));

    const auto decisions{pair_count(windows)};

    lines.push_back(std::format(
            "{} | window pairs: {} certified of {} decided over every one of the {} windows of width two to four "
            "occurring in the slice, exhaustive",
            name, decided.exact.size(), decisions, windows.size()));

    const auto unsettled_text{
            decided.unsettled.empty() ?
                    std::string{} :
                    std::format(", and {} exact decisions unsettled at the cap", decided.unsettled.size())};

    lines.push_back(std::format(
            "{} | window pairs by the conservative model: {} certified of the {} pairs, "
            "is_split_window() on the same windows{}",
            name, decided.conservative.size(), decisions, unsettled_text));

    append(lines, budget_curve_lines(vocabulary, sample));

    const auto certified{vocabulary.certified()};

    auto [supply_text, anchors, supply]{supply_lines(name, certified, sample)};

    append(lines, std::move(supply_text));

    auto [snap_text, table_snap_max]{snap_lines(name, anchors, sample.size())};

    append(lines, std::move(snap_text));

    const auto calibration_windows{occurring_windows(calibration)};

    const auto frozen{frozen_inventory(vocabulary, calibration_windows)};

    const auto sigma{alphabet(tokens)};

    const auto longest{longest_length(tokens)};

    lines.push_back(std::format(
            "{} | edit protocol: seed {} reset for this vocabulary, positions drawn uniformly over the slice and "
            "replacement bytes uniformly over its {}-byte alphabet, a draw repeating the original byte or leaving the "
            "input untokenizable rejected and redrawn, percentiles the order statistic at floor(q*n) capped at the "
            "last element",
            name, edit_seed, sigma.size()));

    auto trials{edit_trials(lexer, longest, sigma, certified, frozen, sample)};

    note(std::format("{}: {} edits attempted", name, trials.size()));

    append(lines, report_edits(name, trials));

    auto slack{slack_rows(label, trials)};

    lines.push_back(block_supply_line(name, anchors, sample.size()));

    append(lines, sampled_inventory_lines(name, lexer, sample, bytes));

    lines.push_back(sync_line(lexer, name));

    append(lines, newline_lines(lexer, name));

    const auto& [count, per_kibibyte, gaps]{supply};

    auto table_row{std::format(
            R"({} & {:.1f} & {} & {} & {} & {} \\)", label, per_kibibyte, gaps->median, gaps->ninetieth, gaps->longest,
            table_snap_max)};

    return Vocabulary_report{
            .lines = std::move(lines),
            .trials = std::move(trials),
            .table_row = std::move(table_row),
            .slack_rows = std::move(slack)};
}

/**
 * @brief Returns the anchor overlap of the two vocabularies' ceiling inventories over the slice's interior positions.
 * @param local The trained vocabulary.
 * @param gpt2 The GPT-2 subset.
 * @param sample The evaluation slice.
 * @return The line.
 */
[[nodiscard]] std::string overlap_line(const Vocabulary& local, const Vocabulary& gpt2, const std::string_view sample)
{
    const auto left{anchor_positions(local.certified(), sample)};

    const auto right{anchor_positions(gpt2.certified(), sample)};

    std::vector<std::size_t> shared{};

    std::ranges::set_intersection(left, right, std::back_inserter(shared));

    const auto both{shared.size()};

    const auto only_left{left.size() - both};

    const auto only_right{right.size() - both};

    const auto interior{sample.size() - 1};

    const auto in_union{both + only_left + only_right};

    const auto jaccard{in_union == 0 ? 0.0 : static_cast<double>(both) / static_cast<double>(in_union)};

    const auto neither{interior - in_union};

    return std::format(
            "{} against {} | anchor overlap: both {}, {} only {}, {} only {}, neither {} of {} interior positions, "
            "Jaccard {:.3f}",
            local.name, gpt2.name, both, local.name, only_left, gpt2.name, only_right, neither, interior, jaccard);
}

/**
 * @brief Returns the divergence of the trained vocabulary from its shallow sibling: over pieces of the slice both
 *        lexers scan completely, how many split alike, and whether any splits differently.
 * @param shallow The local-48 tokens.
 * @param local The trained vocabulary's lexer.
 * @param sample The evaluation slice.
 * @return The two lines.
 */
[[nodiscard]] std::vector<std::string> divergence_lines(
        const std::vector<std::string>& shallow, const Lexer& local, const std::string_view sample)
{
    const auto shallow_lexer{compile(shallow)};

    std::size_t agree{0};

    std::size_t total{0};

    bool witnessed{false};

    for (std::size_t offset{0}; offset < eval_bytes - divergence_piece_bytes; offset += divergence_stride)
    {
        const auto piece{sample.substr(offset, divergence_piece_bytes)};

        const auto shallow_starts{token_starts(shallow_lexer, piece)};

        const auto local_starts{token_starts(local, piece)};

        if (!shallow_starts || !local_starts)
        {
            continue;
        }

        ++total;

        if (*shallow_starts == *local_starts)
        {
            ++agree;

            continue;
        }

        witnessed = true;
    }

    const auto agreeing_line{std::format("local-48 against local-384 | slices agreeing: {} of {}", agree, total)};

    const auto witnessed_line{
            std::format("local-48 against local-384 | divergence witnessed: {}", witnessed ? "yes" : "no")};

    return {agreeing_line, witnessed_line};
}

/**
 * @brief Closes a LaTeX table: its rows after the head, then the bottom rule and the tabular's end.
 * @param table The head.
 * @param rows The rows.
 * @return The table's lines.
 */
[[nodiscard]] std::vector<std::string> closed_table(
        std::vector<std::string> table, const std::vector<std::string>& rows)
{
    table.insert(table.end(), rows.begin(), rows.end());

    table.insert(table.end(), {R"(\bottomrule)", R"(\end{tabular})"});

    return table;
}

/**
 * @brief Returns the evaluation-slice emissions of splitting_measurements.py: splitting-stats.txt, the supply table and
 *        the edit-slack table, for the two vocabularies, with the trained vocabulary's shallower sibling for the
 *        divergence lines.
 * @param vocabularies The two vocabularies, local-384 first.
 * @param stats The trained vocabulary's tie accounting.
 * @param shallow The local-48 tokens.
 * @param sample The evaluation slice.
 * @param calibration The calibration slice.
 * @return The three emissions.
 */
[[nodiscard]] std::vector<Emission> vocabulary_emissions(
        const std::vector<Vocabulary>& vocabularies, const Merge_stats& stats, const std::vector<std::string>& shallow,
        const std::string_view sample, const std::string_view calibration)
{
    std::vector<std::string> lines{std::format(
            "local-384 | merge tie rule: {} of {} merge steps had a most-frequent tie, and {} of those would have "
            "merged a different pair under first occurrence",
            stats.tied_steps, stats.merges, stats.first_occurrence_disagreements)};

    std::vector<std::vector<Edit_record>> trials{};

    std::vector<std::string> table_rows{};

    std::vector<std::string> slack_table_rows{};

    for (const auto& vocabulary : vocabularies)
    {
        auto [vocabulary_text, vocabulary_trials, table_row, slack]{vocabulary_lines(vocabulary, sample, calibration)};

        append(lines, std::move(vocabulary_text));

        trials.push_back(std::move(vocabulary_trials));

        table_rows.push_back(std::move(table_row));

        append(slack_table_rows, std::move(slack));
    }

    const auto& local{vocabularies.front()};

    const auto& gpt2{vocabularies.back()};

    append(lines, measure_pairing(local.name, trials.front(), gpt2.name, trials.back()));

    lines.push_back(overlap_line(local, gpt2, sample));

    lines.push_back(sync_line(utf8_shape(), "utf8-shape"));

    append(lines, divergence_lines(shallow, local.lexer, sample));

    auto table{closed_table(
            {R"(\begin{tabular}{@{}lrrrrr@{}})", R"(\toprule)",
             R"(Vocabulary & anchors/KiB & gap p50 & gap p90 & gap max & snap max (8) \\)", R"(\midrule)"},
            table_rows)};

    auto slack_table{closed_table(
            {R"(\begin{tabular}{@{}llrrrr@{}})", R"(\toprule)",
             R"( & & \multicolumn{2}{c}{ceiling inventory} & \multicolumn{2}{c}{frozen inventory} \\)",
             R"(\cmidrule(lr){3-4}\cmidrule(lr){5-6})", R"(Vocabulary & Statistic & p50 & p90 & p50 & p90 \\)",
             R"(\midrule)"},
            slack_table_rows)};

    return {Emission{.file = "splitting-stats.txt", .lines = std::move(lines)},
            Emission{.file = "splitting-supply-table.tex", .lines = std::move(table)},
            Emission{.file = "edit-slack-table.tex", .lines = std::move(slack_table)}};
}

/**
 * @brief Returns budget_coverage.py's emission: what each width bought, read off the decided inventories.
 * @param vocabularies The vocabularies.
 * @param sample The evaluation slice.
 * @return The emission.
 */
[[nodiscard]] Emission budget_emission(const std::vector<Vocabulary>& vocabularies, const std::string_view sample)
{
    std::vector<std::string> lines{};

    for (const auto& vocabulary : vocabularies)
    {
        const auto& [name, label, tokens, lexer, bytes, windows, decided]{vocabulary};

        std::size_t previous{0};

        for (std::size_t budget{1}; budget <= widths.back(); ++budget)
        {
            const auto through{pairs_through(bytes, decided.exact, budget)};

            const auto at_budget{[budget](const Pair& pair) { return pair.window.size() == budget; }};

            const auto certified_here{std::ranges::count_if(decided.exact, at_budget)};

            const auto anchors{anchor_positions(through, sample).size()};

            if (budget == 1)
            {
                lines.push_back(std::format(
                        "{} | budget H=1: {} anchor positions from {} certified bytes of {} in the alphabet, "
                        "settled by the interior-byte lemma rather than by a search",
                        name, anchors, bytes.size(), alphabet(tokens).size()));
            }
            else
            {
                const auto decided_here{windows_of_width(sample, budget).size() * budget};

                lines.push_back(std::format(
                        "{} | budget H={}: {} anchor positions, {} gained over the width before, {} pairs certified of "
                        "{} decided at this width alone",
                        name, budget, anchors, anchors - previous, certified_here, decided_here));
            }

            previous = anchors;
        }

        lines.push_back(std::format(
                "{} | the curve is reported through width {}, the declared budget, and the gain at that last width is "
                "the final row above; what lies past the budget is not estimated here beyond the width-five probe the "
                "edit trials run",
                name, widths.back()));
    }

    return Emission{.file = "budget-coverage-stats.txt", .lines = std::move(lines)};
}

/**
 * @brief Returns depth_series.py's emission: supply against merge depth, the three shallower vocabularies decided here
 *        and the deepest being the trained vocabulary already decided.
 * @param merges The trained merge table.
 * @param local The trained vocabulary, decided.
 * @param sample The evaluation slice.
 * @return The emission.
 */
[[nodiscard]] Emission depth_emission(
        const std::vector<Symbol_pair_t>& merges, const Vocabulary& local, const std::string_view sample)
{
    std::vector<std::string> lines{std::format(
            "series: byte-pair vocabularies over the same 256-symbol fallback base, "
            "trained on the same first {} corpus bytes, "
            "measured on the same evaluation slice [{}, {}) through the same width budget; "
            "merge depth is the only thing that varies",
            train_bytes, train_bytes, train_bytes + sample.size())};

    std::vector<double> densities{};

    for (const auto depth : depths)
    {
        const auto name{std::format("depth {}", depth)};

        std::optional<Vocabulary> own{};

        if (depth != merges_local)
        {
            own = measure_vocabulary(name, name, local_tokens(merges, depth), sample);
        }

        const auto& row{own ? *own : local};

        const auto certified{row.certified()};

        const auto anchors{anchor_positions(certified, sample)};

        const auto [count, per_kibibyte, gaps]{library_supply(certified, sample)};

        lines.push_back(std::format(
                "{} | tokens: {}, certified bytes {}, wider pairs {}", name, row.tokens.size(), row.bytes.size(),
                row.decided.exact.size()));
        lines.push_back(std::format("{} | anchors per KiB: {:.1f}", name, per_kibibyte));
        lines.push_back(std::format(
                "{} | anchor gap p50: {}, p90 {}, max {}", name, gaps->median, gaps->ninetieth, gaps->longest));

        const auto stretch{longest_stretch(anchors, sample.size())};

        lines.push_back(std::format("{} | longest anchorless stretch: {}", name, stretch));

        if (!row.decided.unsettled.empty())
        {
            lines.push_back(
                    std::format("{} | exact decisions unsettled at the cap: {}", name, row.decided.unsettled.size()));
        }

        densities.push_back(per_kibibyte);
    }

    std::vector<std::string> curve_parts{};

    for (const auto& [depth, value] : std::views::zip(depths, densities))
    {
        curve_parts.push_back(std::format("{}: {:.1f}", depth, value));
    }

    lines.push_back(std::format(
            "series | supply against depth: {} anchors per KiB, "
            "falling by a factor of {:.2f} from depth {} to depth {}",
            joined(curve_parts, ", "), densities.front() / densities.back(), depths.front(), depths.back()));

    const auto monotone{std::ranges::is_sorted(densities, std::ranges::greater_equal{})};

    lines.push_back(std::format(
            "series | the curve is {} over the depths measured, which is a fact about these four vocabularies on this "
            "slice and not a law",
            monotone ? "monotone decreasing" : "not monotone"));

    return Emission{.file = "depth-series-stats.txt", .lines = std::move(lines)};
}

/**
 * @brief Takes the census of the calibration slice's windows against the evaluation slice's.
 * @param evaluation_windows The windows the evaluation slice holds, sorted.
 * @param calibration The calibration slice.
 * @return The calibration windows, the calibration-only ones with their pairs and decisions, and their count by width.
 */
[[nodiscard]] Calibration_census calibration_census(
        const std::vector<std::string>& evaluation_windows, const std::string_view calibration)
{
    Calibration_census census{.windows = occurring_windows(calibration)};

    auto& [calibration_windows, only_here, only_here_pairs, decisions, by_width_text]{census};

    std::array<std::size_t, widths.size()> by_width{};

    for (const auto& window : calibration_windows)
    {
        if (std::ranges::binary_search(evaluation_windows, window))
        {
            continue;
        }

        only_here.push_back(window);

        decisions += window.size();

        ++by_width[window.size() - widths.front()];
    }

    std::vector<std::string> by_width_parts{};

    for (const auto& [count, width] : std::views::zip(by_width, widths))
    {
        by_width_parts.push_back(std::format("{} of width {}", count, width));
    }

    by_width_text = joined(by_width_parts, ", ");

    only_here_pairs = every_origin(only_here);

    return census;
}

/**
 * @brief Returns one vocabulary's frozen inventory against its post-hoc ceiling: the calibration-only windows decided
 *        fresh, the shared ones carrying the evaluation run's verdicts, the two supplies on the evaluation slice, and
 *        the share of each block's ceiling supply the frozen inventory retains.
 * @param vocabulary The vocabulary.
 * @param sample The evaluation slice.
 * @param census The calibration slice's census.
 * @return The vocabulary's lines and table row.
 * @throws std::runtime_error If a block of the slice has no ceiling supply.
 */
[[nodiscard]] Frozen_report frozen_vocabulary_lines(
        const Vocabulary& vocabulary, const std::string_view sample, const Calibration_census& census)
{
    const auto& [name, label, tokens, lexer, bytes, windows, decided]{vocabulary};

    const auto& [calibration_windows, only_here, only_here_pairs, decisions, by_width_text]{census};

    const auto [fresh_exact, fresh_conservative, fresh_unsettled]{decide(lexer, only_here_pairs)};

    note(std::format(
            "{}: {} calibration-only windows decided, {} pairs certified, {} unsettled", name, only_here.size(),
            fresh_exact.size(), fresh_unsettled.size()));

    const auto frozen_base{frozen_inventory(vocabulary, calibration_windows)};

    const auto frozen{concatenated(frozen_base, fresh_exact)};

    const auto ceiling{vocabulary.certified()};

    const auto frozen_anchors{anchor_positions(frozen, sample)};

    const auto ceiling_anchors{anchor_positions(ceiling, sample)};

    const auto frozen_density{density(frozen, sample)};

    const auto oracle_density{density(ceiling, sample)};

    std::vector<std::string> lines{};

    lines.push_back(std::format(
            "{} | frozen hybrid inventory: {} certified pairs, {} certified bytes from the token set alone and {} "
            "calibrated pairs of widths two to four from the calibration slice; {} of the (window, origin) "
            "decisions behind the calibrated pairs are ones the evaluation slice never poses, from {} "
            "calibration-only windows ({}); the evaluation slice poses every other one of them, and certification "
            "does not depend on the slice, so those verdicts are the same whichever run reached them",
            name, frozen.size(), bytes.size(), frozen.size() - bytes.size(), decisions, only_here.size(),
            by_width_text));

    const auto frozen_stretch{longest_stretch(frozen_anchors, sample.size())};

    const auto ceiling_stretch{longest_stretch(ceiling_anchors, sample.size())};

    const auto share{oracle_density == 0.0 ? 0.0 : 100.0 * frozen_density / oracle_density};

    lines.push_back(std::format(
            "{} | frozen supply on the evaluation slice: {:.1f} anchors per KiB, longest anchorless stretch {}", name,
            frozen_density, frozen_stretch));
    lines.push_back(std::format(
            "{} | post-hoc ceiling on the same slice: {:.1f} anchors per KiB, longest anchorless stretch {}", name,
            oracle_density, ceiling_stretch));
    lines.push_back(std::format("{} | frozen supply as a share of the ceiling: {:.1f} per cent", name, share));

    if (!fresh_unsettled.empty())
    {
        lines.push_back(std::format(
                "{} | calibration-only exact decisions unsettled at the cap: {}", name, fresh_unsettled.size()));
    }

    std::vector<double> shares{};

    for (const auto block : block_starts(sample.size()))
    {
        const auto held{anchors_in_block(ceiling_anchors, block)};

        if (held == 0)
        {
            throw std::runtime_error{std::format("the block at {} has no ceiling supply", block)};
        }

        const auto retained{anchors_in_block(frozen_anchors, block)};

        shares.push_back(100.0 * static_cast<double>(retained) / static_cast<double>(held));
    }

    auto table_row{std::format(
            R"({} & {:.1f} & {:.1f} & {:.1f} \\)", label, std::ranges::min(shares), percentile(shares, 0.5),
            std::ranges::max(shares))};

    return {.lines = std::move(lines), .table_row = std::move(table_row)};
}

/**
 * @brief Returns frozen_inventory.py's two emissions: the inventory frozen on the calibration slice applied to the
 *        evaluation slice beside the post-hoc ceiling, and the transfer as a distribution over the slice's blocks.
 * @param vocabularies The vocabularies.
 * @param sample The evaluation slice.
 * @param calibration The calibration slice.
 * @return The two emissions.
 */
[[nodiscard]] std::vector<Emission> frozen_emissions(
        const std::vector<Vocabulary>& vocabularies, const std::string_view sample, const std::string_view calibration)
{
    const auto start{train_bytes + sample.size()};

    std::vector<std::string> lines{std::format(
            "calibration slice: bytes [{}, {}), "
            "disjoint from the training bytes and from the evaluation slice [{}, {})",
            start, start + calibration.size(), train_bytes, train_bytes + sample.size())};

    const auto census{calibration_census(vocabularies.front().windows, calibration)};

    std::vector<std::string> table_rows{};

    for (const auto& vocabulary : vocabularies)
    {
        auto [vocabulary_text, table_row]{frozen_vocabulary_lines(vocabulary, sample, census)};

        append(lines, std::move(vocabulary_text));

        table_rows.push_back(std::move(table_row));
    }

    auto table{closed_table(
            {R"(\begin{tabular}{@{}lrrr@{}})", R"(\toprule)",
             R"( & \multicolumn{3}{c}{share of the block's ceiling supply retained, per cent} \\)",
             R"(\cmidrule(lr){2-4})", R"(Vocabulary & lowest block & upper median & highest block \\)", R"(\midrule)"},
            table_rows)};

    return {Emission{.file = "frozen-inventory-stats.txt", .lines = std::move(lines)},
            Emission{.file = "frozen-transfer-table.tex", .lines = std::move(table)}};
}

/**
 * @brief Returns the class letter every C-like row shares, or nothing where the row's own refinement decides; built
 *        from the sets c_like's tokens are built from: the identifier start, the digits, the blank, the newline and the
 *        punctuation.
 * @param byte The byte.
 * @return The letter, or std::nullopt.
 */
[[nodiscard]] std::optional<char> core_class(const unsigned char byte)
{
    const auto c{static_cast<char>(byte)};

    static const auto identifier_start{munch::regex::Set::alpha() + '_'};

    if (identifier_start.symbols().contains(c))
    {
        return 'L';
    }

    static const auto digits{munch::regex::Set::digits()};

    if (digits.symbols().contains(c))
    {
        return 'D';
    }

    static const munch::regex::Set blank{' ', '\t'};

    if (blank.symbols().contains(c))
    {
        return 'S';
    }

    if (c == '\n')
    {
        return 'N';
    }

    if (figures::punctuation().symbols().contains(c))
    {
        return 'P';
    }

    return std::nullopt;
}

/**
 * @brief Returns the letter of an operator byte or of a byte no class names, beside the shared and the row's own
 *        letters.
 * @param c The byte.
 * @return O for an operator, X otherwise.
 */
[[nodiscard]] char operator_or_other(const char c)
{
    return figures::operators().symbols().contains(c) ? 'O' : 'X';
}

/**
 * @brief Returns the four campaign rows, the grammars composed as recovery_quality.cpp composes them and the classes as
 *        campaign_inventory.py spells them.
 * @return The rows.
 */
[[nodiscard]] std::vector<Campaign_row> campaign_rows()
{
    const auto conventional_class{[](const unsigned char byte) {
        if (const auto shared{core_class(byte)})
        {
            return *shared;
        }

        const auto c{static_cast<char>(byte)};

        if (c == '"')
        {
            return 'Q';
        }

        if (c == '/')
        {
            return 'C';
        }

        return operator_or_other(c);
    }};

    const auto block_class{[](const unsigned char byte) {
        if (const auto shared{core_class(byte)})
        {
            return *shared;
        }

        const auto c{static_cast<char>(byte)};

        if (c == '/')
        {
            return 'C';
        }

        if (c == '*')
        {
            return 'A';
        }

        return operator_or_other(c);
    }};

    const auto bare_class{[](const unsigned char byte) {
        const auto shared{core_class(byte)};

        return shared ? *shared : operator_or_other(static_cast<char>(byte));
    }};

    const auto block_grammar{[](munch::core::Builder& builder) {
        figures::c_like(builder, false);

        builder.add_token(figures::block_comment(), figures::Token::block_comment, 1);
    }};

    const auto bare_grammar{[](munch::core::Builder& builder) { figures::c_like(builder, false); }};

    std::vector<Pair> close_shaped{};

    for (const auto left : block_sigma)
    {
        for (const auto right : block_sigma)
        {
            close_shaped.push_back(Pair{.window = std::format("{}AC{}", left, right), .origin = 3});
        }
    }

    return {Campaign_row{
                    .name = "campaign conventional",
                    .corpus = "corpus-c-like-conventional-with-strings-and-line-comments.bin",
                    .grammar = munch::tools::probes::conventional_row,
                    .sigma = "LDSNQCOPX",
                    .classify = conventional_class,
                    .extra_windows = {},
                    .anchor_table = true,
                    .edit_study = true},
            Campaign_row{
                    .name = "campaign split-friendly",
                    .corpus = "corpus-c-like-split-friendly-with-strings-and-line-comments.bin",
                    .grammar = munch::tools::probes::split_friendly_conventional_row,
                    .sigma = "LDSNQCOPX",
                    .classify = conventional_class,
                    .extra_windows = {},
                    .anchor_table = false,
                    .edit_study = true},
            Campaign_row{
                    .name = "campaign block",
                    .corpus = "corpus-c-like-conventional-plus-block-comments-alone.bin",
                    .grammar = block_grammar,
                    .sigma = std::string{block_sigma},
                    .classify = block_class,
                    .extra_windows = close_shaped,
                    .anchor_table = false,
                    .edit_study = false},
            Campaign_row{
                    .name = "campaign bare",
                    .corpus = "corpus-c-like-bare--identifiers-numbers-operators-punctuation.bin",
                    .grammar = bare_grammar,
                    .sigma = "LDSNOPX",
                    .classify = bare_class,
                    .extra_windows = {},
                    .anchor_table = false,
                    .edit_study = false}};
}

/**
 * @brief Returns the lowest byte of a class, its first member.
 * @param members The class's bytes, ascending and not empty.
 * @return The lowest byte.
 */
[[nodiscard]] unsigned char lowest_member(const std::vector<unsigned char>& members)
{
    return members.front();
}

/**
 * @brief Returns the byte classes of a lexer's tables, as tools/audit's report enumerates them: two bytes are one class
 *        when every state moves on both to the same state; each class ascending, the classes by lowest byte.
 * @param lexer The lexer.
 * @return The classes.
 */
[[nodiscard]] std::vector<std::vector<unsigned char>> byte_classes(const Lexer& lexer)
{
    const auto& simulator{lexer.simulator()};

    std::map<std::vector<std::optional<std::size_t>>, std::vector<unsigned char>> by_signature{};

    for (const auto symbol : every_byte())
    {
        const auto byte{static_cast<unsigned char>(symbol)};

        std::vector<std::optional<std::size_t>> signature{};

        for (std::size_t state{0}; state < simulator.state_count(); ++state)
        {
            signature.push_back(simulator.step(state, byte));
        }

        by_signature[std::move(signature)].push_back(byte);
    }

    std::vector<std::vector<unsigned char>> classes{};

    for (auto& [signature, members] : by_signature)
    {
        classes.push_back(std::move(members));
    }

    std::ranges::sort(classes, {}, lowest_member);

    return classes;
}

/**
 * @brief Returns the replacement byte of a letter.
 * @param letter The letter, one replacements lists.
 * @return The replacement byte.
 */
[[nodiscard]] char replacement_of(const char letter)
{
    const auto found{std::ranges::find(replacements, letter, &Replacement::letter)};

    const auto& [found_letter, byte]{*found};

    return byte;
}

/**
 * @brief Cuts the edit study's slice from a row: the slice ends at a line end, since a raw cut can open a string or
 *        comment it never closes, and every occurrence of every inventory window in its class string is found once.
 * @param row The row.
 * @return The slice and what the study reads of it.
 * @throws std::runtime_error If the slice does not tokenize.
 */
[[nodiscard]] Edit_slice edit_slice_of(const Measured_row& row)
{
    const auto& [name, lexer, corpus, class_text, classify, sigma, certified]{row};

    const auto line_end{corpus.rfind('\n', campaign_slice_bytes - 1)};

    const auto slice{std::string_view{corpus}.substr(0, line_end + 1)};

    const auto slice_classes{std::string_view{class_text}.substr(0, slice.size())};

    const auto base_starts{token_starts(lexer, slice)};

    if (!base_starts)
    {
        throw std::runtime_error{std::format("{}: the edit slice does not tokenize", name)};
    }

    std::vector<std::pair<Pair, std::vector<std::size_t>>> occurrences{};

    for (const auto& pair : certified)
    {
        const auto& [window, origin]{pair};

        std::vector<std::size_t> found{};

        for (auto at{slice_classes.find(window)}; at != std::string_view::npos; at = slice_classes.find(window, at + 1))
        {
            found.push_back(at);
        }

        occurrences.emplace_back(pair, std::move(found));
    }

    return Edit_slice{
            .slice = slice,
            .slice_classes = slice_classes,
            .base = {base_starts->begin(), base_starts->end()},
            .occurrences = std::move(occurrences)};
}

/**
 * @brief Returns the boundaries an edited slice moves against the slice's own scan.
 * @param lexer The row's lexer.
 * @param base The slice's token starts.
 * @param edited The edited slice.
 * @return The boundaries moved, or nothing when the slice stops tokenizing.
 */
[[nodiscard]] std::optional<std::vector<std::size_t>> moved_after(
        const Lexer& lexer, const std::set<std::size_t>& base, const std::string_view edited)
{
    const auto starts{token_starts(lexer, edited)};

    if (!starts)
    {
        return std::nullopt;
    }

    return moved_boundaries(base, *starts);
}

/**
 * @brief Returns the flanking anchors of an edit: the greatest witnessed wholly before it and the least witnessed
 *        wholly after.
 * @param edit_slice The slice and its occurrences.
 * @param position The edit's position.
 * @return The flanking anchors, low and high.
 */
[[nodiscard]] std::pair<std::size_t, std::size_t> flanking_anchors(
        const Edit_slice& edit_slice, const std::size_t position)
{
    std::size_t low{0};

    auto high{edit_slice.slice.size()};

    for (const auto& [pair, found] : edit_slice.occurrences)
    {
        const auto& [window, origin]{pair};

        for (const auto start : found)
        {
            const auto anchor{start + origin};

            if (start + window.size() <= position && anchor > 0 && anchor <= position)
            {
                low = std::max(low, anchor);
            }

            if (start > position && anchor > position)
            {
                high = std::min(high, anchor);
            }
        }
    }

    return std::pair{low, high};
}

/**
 * @brief Returns the edit bound: cross-class single-byte substitutions drawn until enough are retained, every moved
 *        boundary held strictly between the flanking anchors the inventory witnesses in the shared prefix and suffix.
 * @param row The row.
 * @param edit_slice The slice and its occurrences.
 * @param chooser The study's generator, drawn from in campaign_inventory.py's order.
 * @return The line.
 * @throws std::runtime_error If a moved boundary escapes the flanking anchors.
 */
[[nodiscard]] std::string cross_class_line(
        const Measured_row& row, const Edit_slice& edit_slice, Python_random& chooser)
{
    const auto& [name, lexer, corpus, class_text, classify, sigma, certified]{row};

    const auto& [slice, slice_classes, base, occurrences]{edit_slice};

    std::vector<std::size_t> radii{};

    std::size_t draws{0};

    std::size_t redrawn{0};

    while (radii.size() < campaign_cross_class_edits)
    {
        ++draws;

        const auto position{chooser.randrange(slice.size())};

        const auto current{classify(static_cast<unsigned char>(slice[position]))};

        std::string others{};

        const auto is_other{[current](const char letter) { return letter != current; }};

        std::ranges::copy_if(sigma, std::back_inserter(others), is_other);

        const auto drawn{chooser.randrange(others.size())};

        const auto target{others[drawn]};

        std::string edited{slice};

        edited[position] = replacement_of(target);

        const auto moved{moved_after(lexer, base, edited)};

        if (!moved)
        {
            ++redrawn;

            continue;
        }

        const auto [low, high]{flanking_anchors(edit_slice, position)};

        for (const auto boundary : *moved)
        {
            if (boundary <= low || boundary >= high)
            {
                throw std::runtime_error{
                        std::format("{}: a moved boundary at {} escapes the flanking anchors", name, boundary)};
            }
        }

        const auto hull{hull_of(*moved)};

        radii.push_back(hull);
    }

    return std::format(
            "{} | edit bound: {} cross-class substitutions retained from {} draws, {} redrawn as untokenizable, "
            "on a {}-byte slice, every moved boundary between the flanking anchors, "
            "boundary-difference hull p50 {} and max {}",
            name, radii.size(), draws, redrawn, slice.size(), percentile(radii, 0.5), std::ranges::max(radii));
}

/**
 * @brief Returns the class invariance: same-class substitutions drawn until enough are retained, each held to move
 *        nothing.
 * @param row The row.
 * @param edit_slice The slice and its occurrences.
 * @param chooser The study's generator, drawn from after the cross-class substitutions.
 * @return The line.
 * @throws std::runtime_error If a same-class substitution moves a boundary.
 */
[[nodiscard]] std::string class_invariance_line(
        const Measured_row& row, const Edit_slice& edit_slice, Python_random& chooser)
{
    const auto& [name, lexer, corpus, class_text, classify, sigma, certified]{row};

    const auto& [slice, slice_classes, base, occurrences]{edit_slice};

    std::size_t unchanged{0};

    std::size_t position_draws{0};

    while (unchanged < campaign_same_class_edits)
    {
        ++position_draws;

        const auto position{chooser.randrange(slice.size())};

        const auto letter{classify(static_cast<unsigned char>(slice[position]))};

        const auto same_class{[&](const Replacement& entry) {
            const auto& [entry_letter, byte]{entry};

            return classify(static_cast<unsigned char>(byte)) == letter && byte != slice[position];
        }};

        const auto same{std::ranges::find_if(replacements, same_class)};

        if (same == replacements.end())
        {
            continue;
        }

        const auto& [same_letter, same_byte]{*same};

        std::string edited{slice};

        edited[position] = same_byte;

        const auto moved{moved_after(lexer, base, edited)};

        if (!moved || !moved->empty())
        {
            throw std::runtime_error{
                    std::format("{}: a same-class substitution at {} moved a boundary", name, position)};
        }

        ++unchanged;
    }

    return std::format(
            "{} | class invariance: {} same-class substitutions retained from {} position draws moved no boundary",
            name, unchanged, position_draws);
}

/**
 * @brief Returns the campaign's between-anchors edit study on one row, campaign_inventory.py's draws replayed:
 *        cross-class single-byte substitutions over a line-ended slice of the corpus, every moved boundary held
 *        strictly between the flanking anchors the inventory witnesses in the shared prefix and suffix, and same-class
 *        substitutions held to move nothing.
 * @param row The row.
 * @return The two lines.
 * @throws std::runtime_error If a moved boundary escapes the flanking anchors or a same-class substitution moves one.
 */
[[nodiscard]] std::vector<std::string> campaign_edit_lines(const Measured_row& row)
{
    const auto edit_slice{edit_slice_of(row)};

    Python_random chooser{campaign_edit_seed};

    auto bound{cross_class_line(row, edit_slice, chooser)};

    auto invariance{class_invariance_line(row, edit_slice, chooser)};

    return {std::move(bound), std::move(invariance)};
}

/**
 * @brief Returns the paper's letters against the library's classes: every letter must lie inside one class of the
 *        tables, so that its bytes move every state alike and a window over letters decides as its representatives do.
 *        The letters may be finer than the tables' classes, as they are where a row treats the newline like a blank.
 * @tparam Classify The type of the letter function.
 * @param name The row's name.
 * @param lexer The row's lexer.
 * @param sigma The row's letters in the paper's order.
 * @param classify The letter of each byte.
 * @return The letters, their bytes and representatives.
 * @throws std::runtime_error If the letters are not the ones declared or a letter straddles two byte classes.
 */
template <typename Classify>
[[nodiscard]] Row_letters row_letters(
        const std::string_view name, const Lexer& lexer, const std::string_view sigma, const Classify& classify)
{
    const auto table_classes{byte_classes(lexer)};

    std::array<std::size_t, byte_count> class_of{};

    for (const auto& [index, members] : std::views::enumerate(table_classes))
    {
        for (const auto member : members)
        {
            class_of[member] = static_cast<std::size_t>(index);
        }
    }

    std::map<char, std::vector<unsigned char>> members_of{};

    for (const auto symbol : every_byte())
    {
        const auto byte{static_cast<unsigned char>(symbol)};

        members_of[classify(byte)].push_back(byte);
    }

    if (members_of.size() != sigma.size())
    {
        throw std::runtime_error{std::format("{}: {} letters for {} declared", name, members_of.size(), sigma.size())};
    }

    std::map<std::size_t, std::string> letters_in_class{};

    std::map<char, unsigned char> representative{};

    std::array<char, byte_count> canonical{};

    std::vector<std::vector<unsigned char>> classes{};

    for (const auto& [letter, members] : members_of)
    {
        const auto lowest{members.front()};

        for (const auto member : members)
        {
            if (class_of[member] != class_of[lowest])
            {
                throw std::runtime_error{std::format("{}: the letter {} straddles two byte classes", name, letter)};
            }

            canonical[member] = static_cast<char>(lowest);
        }

        letters_in_class[class_of[lowest]].push_back(letter);

        representative.emplace(letter, lowest);

        classes.push_back(members);
    }

    std::ranges::sort(classes, {}, lowest_member);

    std::string merged{};

    for (const auto& [index, letters] : letters_in_class)
    {
        if (letters.size() <= 1)
        {
            continue;
        }

        const auto joiner{merged.empty() ? ", the tables not telling " : " nor "};

        merged += std::format("{}{}", joiner, letters);
    }

    return Row_letters{
            .members_of = std::move(members_of),
            .representative = std::move(representative),
            .canonical = canonical,
            .classes = std::move(classes),
            .table_class_count = table_classes.size(),
            .merged = std::move(merged)};
}

/**
 * @brief Returns a string over letters spelled in the letters' representatives.
 * @param representative Each letter's representative.
 * @param letters The letters.
 * @return The bytes.
 */
[[nodiscard]] std::string bytes_of_letters(
        const std::map<char, unsigned char>& representative, const std::string_view letters)
{
    std::string bytes{};

    for (const auto letter : letters)
    {
        bytes.push_back(static_cast<char>(representative.at(letter)));
    }

    return bytes;
}

/**
 * @brief Returns the letters of a byte string.
 * @tparam Classify The type of the letter function.
 * @param classify The letter of each byte.
 * @param bytes The bytes.
 * @return The letters.
 */
template <typename Classify>
[[nodiscard]] std::string letters_of_bytes(const Classify& classify, const std::string_view bytes)
{
    const auto letter_of{[&classify](const char byte) { return classify(static_cast<unsigned char>(byte)); }};

    std::string letters{};

    std::ranges::transform(bytes, std::back_inserter(letters), letter_of);

    return letters;
}

/**
 * @brief Returns the pairs over letters spelled in the letters' representatives, in their order.
 * @param representative Each letter's representative.
 * @param over_letters The pairs over letters.
 * @return The pairs over bytes.
 */
[[nodiscard]] std::vector<Pair> pairs_over_bytes(
        const std::map<char, unsigned char>& representative, const std::vector<Pair>& over_letters)
{
    std::vector<Pair> bytes{};

    for (const auto& [window, origin] : over_letters)
    {
        bytes.push_back(Pair{.window = bytes_of_letters(representative, window), .origin = origin});
    }

    return bytes;
}

/**
 * @brief Returns every class window to length two at every origin, in the paper's enumeration order: each letter alone
 *        at origin zero, then every two letters at origins zero and one.
 * @param sigma The letters in the paper's order.
 * @return The candidate pairs over letters.
 */
[[nodiscard]] std::vector<Pair> class_candidates(const std::string_view sigma)
{
    std::vector<Pair> candidates{};

    for (const auto letter : sigma)
    {
        candidates.push_back(Pair{.window = std::string(1, letter), .origin = 0});
    }

    for (const auto left : sigma)
    {
        for (const auto right : sigma)
        {
            for (std::size_t origin{0}; origin < 2; ++origin)
            {
                candidates.push_back(Pair{.window = std::format("{}{}", left, right), .origin = origin});
            }
        }
    }

    return candidates;
}

/**
 * @brief Returns the refutation line of a row on which no two-class window certifies: how many two-class pairs are
 *        refuted and the deepest refutation the decider synthesizes, the first of that length in candidate order.
 * @tparam Classify The type of the letter function.
 * @param name The row's name.
 * @param lexer The row's lexer.
 * @param classify The letter of each byte.
 * @param candidates The candidate pairs over letters.
 * @param byte_candidates The same pairs over representatives.
 * @return The line, or nothing when no pair is refuted.
 */
template <typename Classify>
[[nodiscard]] std::optional<std::string> refutation_line(
        const std::string_view name, const Lexer& lexer, const Classify& classify, const std::vector<Pair>& candidates,
        const std::vector<Pair>& byte_candidates)
{
    std::size_t refused{0};

    std::size_t pairs{0};

    std::string deepest_witness{};

    std::optional<Pair> deepest_candidate{};

    for (const auto& [candidate, byte_candidate] : std::views::zip(candidates, byte_candidates))
    {
        const auto& [candidate_window, candidate_origin]{candidate};

        if (candidate_window.size() != 2)
        {
            continue;
        }

        ++pairs;

        const auto& [window, origin]{byte_candidate};

        const auto [witness, exhaustive]{lexer.window_counterexample(window, origin)};

        if (witness.empty())
        {
            continue;
        }

        ++refused;

        if (!deepest_candidate || witness.size() > deepest_witness.size())
        {
            deepest_witness = witness;

            deepest_candidate = candidate;
        }
    }

    if (!deepest_candidate)
    {
        return std::nullopt;
    }

    const auto& [deepest_window, deepest_origin]{*deepest_candidate};

    const auto deepest_letters{letters_of_bytes(classify, deepest_witness)};

    return std::format(
            "{} | no two-class window certifies: all {} of {} two-class pairs are refuted, and the deepest refutation "
            "the decider synthesizes runs to {} classes, {} for the window {} at origin {}",
            name, refused, pairs, deepest_witness.size(), deepest_letters, deepest_window, deepest_origin);
}

/**
 * @brief Returns the candidates certified exactly, in candidate order, over letters and over representatives alike.
 * @param candidates The candidate pairs over letters.
 * @param byte_candidates The same pairs over representatives.
 * @param exact The pairs over representatives certified exactly, sorted.
 * @return The certified pairs.
 */
[[nodiscard]] Certified_pairs certified_in_order(
        const std::vector<Pair>& candidates, const std::vector<Pair>& byte_candidates, const std::vector<Pair>& exact)
{
    Certified_pairs certified{};

    auto& [over_letters, over_bytes]{certified};

    for (const auto& [candidate, byte_candidate] : std::views::zip(candidates, byte_candidates))
    {
        if (std::ranges::binary_search(exact, byte_candidate))
        {
            over_letters.push_back(candidate);

            over_bytes.push_back(byte_candidate);
        }
    }

    return certified;
}

/**
 * @brief Decides a row's budget extension: the extra windows decided over their representatives.
 * @param name The row's name.
 * @param lexer The row's lexer.
 * @param representative Each letter's representative.
 * @param extra_windows The extra pairs over letters.
 * @return The certified extra pairs and the lines.
 * @throws std::runtime_error If a decision is unsettled at the cap.
 */
[[nodiscard]] Extension extension_lines(
        const std::string_view name, const Lexer& lexer, const std::map<char, unsigned char>& representative,
        const std::vector<Pair>& extra_windows)
{
    const auto extra_bytes{pairs_over_bytes(representative, extra_windows)};

    const auto [exact, conservative, unsettled]{decide(lexer, extra_bytes)};

    if (!unsettled.empty())
    {
        throw std::runtime_error{std::format("{}: a budget-extension decision was unsettled at the cap", name)};
    }

    auto certified{certified_in_order(extra_windows, extra_bytes, exact)};

    auto exact_line{std::format(
            "{} | budget extension: {} of {} close-shaped four-class windows certify at origin three", name,
            exact.size(), extra_windows.size())};

    auto conservative_line{std::format(
            "{} | budget extension by the conservative model: {} of {} close-shaped four-class windows, "
            "is_split_window() on the representatives",
            name, conservative.size(), extra_windows.size())};

    return Extension{.certified = std::move(certified), .lines = {std::move(exact_line), std::move(conservative_line)}};
}

/**
 * @brief Returns every byte window a class window stands for, each letter replaced by each of its bytes, the first
 *        letter varying slowest.
 * @param members_of The bytes of each letter.
 * @param window The class window.
 * @return The byte windows.
 */
[[nodiscard]] std::vector<std::string> expansions_of(
        const std::map<char, std::vector<unsigned char>>& members_of, const std::string_view window)
{
    std::vector<std::string> expansions{""};

    for (const auto letter : window)
    {
        std::vector<std::string> longer{};

        for (const auto& prefix : expansions)
        {
            for (const auto member : members_of.at(letter))
            {
                longer.push_back(prefix + static_cast<char>(member));
            }
        }

        expansions = std::move(longer);
    }

    return expansions;
}

/**
 * @brief Returns the gap corollary over a row's inventory: every class window expanded to its byte windows, since the
 *        walk reads bytes; the conservative model must certify each, so where it refuses one the verdict is over what
 *        it certifies.
 * @tparam Classify The type of the letter function.
 * @param lexer The row's lexer.
 * @param letters The row's letters.
 * @param classify The letter of each byte.
 * @param certified The certified pairs over letters.
 * @param certified_bytes_order The same pairs over representatives.
 * @return The verdict as the density line spells it.
 */
template <typename Classify>
[[nodiscard]] std::string density_verdict(
        const Lexer& lexer, const Row_letters& letters, const Classify& classify, const std::vector<Pair>& certified,
        const std::vector<Pair>& certified_bytes_order)
{
    if (certified.empty())
    {
        return "unbounded at this budget, no certified window to anchor on";
    }

    std::vector<std::string> certified_expansions{};

    for (const auto& [window, origin] : certified)
    {
        for (auto& expansion : expansions_of(letters.members_of, window))
        {
            if (lexer.is_split_window(expansion) == origin)
            {
                certified_expansions.push_back(std::move(expansion));
            }
        }
    }

    const Inventory index{certified_bytes_order};

    std::vector<std::pair<std::string_view, std::size_t>> inventory{};

    for (const auto& window : certified_expansions)
    {
        const auto window_letters{letters_of_bytes(classify, window)};

        const auto representatives{bytes_of_letters(letters.representative, window_letters)};

        for (const auto origin : index.origins_of.at(representatives))
        {
            inventory.emplace_back(window, origin);
        }
    }

    const auto span{lexer.anchor_free_span(inventory)};

    return span ? std::to_string(*span) : "unbounded";
}

/**
 * @brief Returns a campaign row's name without the campaign prefix, as the tables and the protocol line carry it.
 * @param name The row's name.
 * @return The short name.
 */
[[nodiscard]] std::string_view short_name_of(const std::string_view name)
{
    return name.substr(std::string_view{"campaign "}.size());
}

/**
 * @brief Returns a corpus over class representatives: every byte replaced by its letter's representative.
 * @param corpus The corpus.
 * @param canonical Each byte's representative.
 * @return The class text.
 */
[[nodiscard]] std::string class_text_of(const std::string_view corpus, const std::array<char, byte_count>& canonical)
{
    const auto representative_of{[&canonical](const char byte) { return canonical[static_cast<unsigned char>(byte)]; }};

    std::string class_text{};

    std::ranges::transform(corpus, std::back_inserter(class_text), representative_of);

    return class_text;
}

/**
 * @brief Returns the certified single classes as the certified-pairs line lists them, quoted and comma separated.
 * @param certified The certified pairs over letters.
 * @return The list, empty when no single class certifies.
 */
[[nodiscard]] std::string singles_of(const std::vector<Pair>& certified)
{
    std::vector<std::string> singles{};

    for (const auto& [window, origin] : certified)
    {
        if (window.size() == 1)
        {
            singles.push_back(std::format("'{}'", window));
        }
    }

    return joined(singles, ", ");
}

/**
 * @brief Finds the vacuous certified pairs: those whose window over representatives no completely tokenizable class
 *        string holds.
 * @param name The row's name.
 * @param lexer The row's lexer.
 * @param certified The certified pairs over letters and over representatives.
 * @param class_text The corpus over class representatives.
 * @return The vacuous pairs.
 * @throws std::runtime_error If an occurrence search is unsettled or a vacuous window occurs in the corpus.
 */
[[nodiscard]] Vacuity vacuity_of(
        const std::string_view name, const Lexer& lexer, const Certified_pairs& certified,
        const std::string_view class_text)
{
    const auto& [over_letters, over_bytes]{certified};

    std::vector<std::string> vacuous{};

    for (const auto& [pair, byte_pair] : std::views::zip(over_letters, over_bytes))
    {
        const auto& [byte_window, byte_origin]{byte_pair};

        const auto [witness, exhaustive]{lexer.window_occurrence(byte_window)};

        if (!exhaustive)
        {
            throw std::runtime_error{std::format("{}: an occurrence search was unsettled at the cap", name)};
        }

        if (!witness.empty())
        {
            continue;
        }

        const auto& [window, origin]{pair};

        vacuous.push_back(std::format("{} {}", window, origin));

        if (class_text.contains(byte_window))
        {
            throw std::runtime_error{std::format("{}: a vacuous window occurs in the corpus", name)};
        }
    }

    return Vacuity{.count = vacuous.size(), .text = joined(vacuous, ", ")};
}

/**
 * @brief Checks that every anchor is a token start of the corpus.
 * @param name The row's name.
 * @param anchors The anchors.
 * @param starts The corpus's token starts.
 * @throws std::runtime_error If an anchor is off the boundary set.
 */
void check_on_boundaries(
        const std::string_view name, const std::vector<std::size_t>& anchors, const std::vector<std::size_t>& starts)
{
    const std::set<std::size_t> boundary_set{starts.begin(), starts.end()};

    for (const auto anchor : anchors)
    {
        if (!boundary_set.contains(anchor))
        {
            throw std::runtime_error{std::format("{}: the anchor {} is off the boundary set", name, anchor)};
        }
    }
}

/**
 * @brief Returns the anchor gaps under the campaign's convention, the runs to the two ends counted.
 * @param anchors The anchors, ascending.
 * @param size The text's length.
 * @return The gaps.
 */
[[nodiscard]] std::vector<std::size_t> gaps_of(const std::vector<std::size_t>& anchors, const std::size_t size)
{
    std::vector<std::size_t> gaps{};

    std::size_t previous{0};

    for (const auto anchor : anchors)
    {
        gaps.push_back(anchor - previous);

        previous = anchor;
    }

    gaps.push_back(size - previous);

    return gaps;
}

/**
 * @brief Runs the split theorem whole: the corpus cut at every anchor, each chunk scanned on its own, the chunked token
 *        starts held to the sequential ones.
 * @param name The row's name.
 * @param lexer The row's lexer.
 * @param corpus The corpus.
 * @param anchors The anchors, ascending.
 * @param starts The sequential scan's token starts.
 * @throws std::runtime_error If a chunk does not tokenize or the chunked scans differ from the sequential one.
 */
void check_split_theorem(
        const std::string_view name, const Lexer& lexer, const std::string_view corpus,
        const std::vector<std::size_t>& anchors, const std::vector<std::size_t>& starts)
{
    std::vector<std::size_t> chunked{};

    std::size_t begin{0};

    auto cuts{anchors};

    cuts.push_back(corpus.size());

    for (const auto end : cuts)
    {
        const auto chunk{corpus.substr(begin, end - begin)};

        const auto chunk_starts{token_starts(lexer, chunk)};

        if (!chunk_starts)
        {
            throw std::runtime_error{std::format("{}: the chunk at {} does not tokenize", name, begin)};
        }

        for (const auto start : *chunk_starts)
        {
            chunked.push_back(begin + start);
        }

        begin = end;
    }

    if (chunked != starts)
    {
        throw std::runtime_error{std::format("{}: the chunked scans differ from the sequential scan", name)};
    }
}

/**
 * @brief Checks a campaign row's class abstraction: the scans of the corpus and of its class string agree token start
 *        for token start.
 * @param name The row's name.
 * @param sigma The row's letters.
 * @param lexer The row's lexer.
 * @param corpus The corpus.
 * @param letters The row's letters against the tables' classes.
 * @param class_text The corpus as class representatives.
 * @return The byte scan's token starts and the abstraction and byte-classes lines.
 * @throws std::runtime_error If either scan does not reach its end or the two disagree.
 */
[[nodiscard]] Row_abstraction abstraction_lines(
        const std::string_view name, const std::string_view sigma, const Lexer& lexer, const std::string_view corpus,
        const Row_letters& letters, const std::string_view class_text)
{
    const auto& [members_of, representative, canonical, classes, table_class_count, merged]{letters};

    const auto byte_starts{token_starts(lexer, corpus)};

    const auto class_starts{token_starts(lexer, class_text)};

    if (!byte_starts || !class_starts || *byte_starts != *class_starts)
    {
        throw std::runtime_error{std::format("{}: the class abstraction does not commute with the scan", name)};
    }

    std::vector<std::string> lines{};

    const auto merged_text{merged.empty() ? std::string{} : merged + " apart"};

    lines.push_back(std::format(
            "{} | abstraction: {} corpus bytes, byte and class scans agree on all {} token starts", name, corpus.size(),
            byte_starts->size()));
    lines.push_back(std::format(
            "{} | byte classes: the row's {} letters each lie inside one of the tables' {} classes{}", name,
            sigma.size(), table_class_count, merged_text));

    return {.byte_starts = *byte_starts, .lines = std::move(lines)};
}

/**
 * @brief Decides a campaign row's class windows to length two exactly and conservatively, refutes a row certifying no
 *        two-class window, and adds the windows past the budget the row names.
 * @param row The row.
 * @param lexer The row's lexer.
 * @param letters The row's letters against the tables' classes.
 * @return The certified pairs and their lines.
 * @throws std::runtime_error If a decision is unsettled at the cap.
 */
[[nodiscard]] Row_certificates certified_pair_lines(
        const Campaign_row& row, const Lexer& lexer, const Row_letters& letters)
{
    const auto& [name, corpus_file, grammar, sigma, classify, extra_windows, anchor_table, edit_study]{row};

    const auto& representative{letters.representative};

    const auto candidates{class_candidates(sigma)};

    const auto byte_candidates{pairs_over_bytes(representative, candidates)};

    const auto [exact, conservative, unsettled]{decide(lexer, byte_candidates)};

    if (!unsettled.empty())
    {
        throw std::runtime_error{std::format("{}: a class-window decision was unsettled at the cap", name)};
    }

    auto certified{certified_in_order(candidates, byte_candidates, exact)};

    auto& [certified_letters, certified_bytes]{certified};

    std::vector<std::string> lines{};

    const auto singles{singles_of(certified_letters)};

    const auto singles_text{singles.empty() ? std::string{"none"} : std::format("[{}]", singles)};

    lines.push_back(std::format(
            "{} | certified pairs: {} over class windows to length two, certified single classes at origin zero: {}",
            name, certified_letters.size(), singles_text));
    lines.push_back(std::format(
            "{} | certified pairs by the conservative model: {} of the {} class pairs to length two, "
            "is_split_window() on the representatives",
            name, conservative.size(), candidates.size()));

    const auto spans_two{[](const Pair& pair) { return pair.window.size() == 2; }};

    const auto two_class_certified{std::ranges::any_of(certified_letters, spans_two)};

    if (!two_class_certified)
    {
        if (auto refutation{refutation_line(name, lexer, classify, candidates, byte_candidates)})
        {
            lines.push_back(std::move(*refutation));
        }
    }

    if (!extra_windows.empty())
    {
        auto [extension, extension_text]{extension_lines(name, lexer, representative, extra_windows)};

        append(lines, std::move(extension_text));

        std::ranges::move(extension.over_letters, std::back_inserter(certified_letters));

        std::ranges::move(extension.over_bytes, std::back_inserter(certified_bytes));
    }

    return {.certified = std::move(certified), .lines = std::move(lines)};
}

/**
 * @brief Measures a campaign row's supply over its class string: the anchors, held to the byte scan's token starts,
 *        their gaps by the campaign's convention and by the library's supply, and the worst snap at each of
 *        snap_workers.
 * @param name The row's name.
 * @param corpus The corpus.
 * @param class_text The corpus as class representatives.
 * @param classes The letters' byte sets.
 * @param certified_bytes The certified pairs over representatives.
 * @param byte_starts The byte scan's token starts.
 * @return The anchors, their gaps and supply, the table's snap, and the supply and snap lines.
 * @throws std::runtime_error If an anchor is not a token start.
 */
[[nodiscard]] Row_supply supply_and_snap_lines(
        const std::string_view name, const std::string_view corpus, const std::string_view class_text,
        const std::vector<std::vector<unsigned char>>& classes, const std::vector<Pair>& certified_bytes,
        const std::vector<std::size_t>& byte_starts)
{
    // The anchors over the class string, the campaign's gap convention counting the runs to the two ends.
    auto anchors{anchor_positions(certified_bytes, class_text)};

    check_on_boundaries(name, anchors, byte_starts);

    auto gaps{gaps_of(anchors, class_text.size())};

    const auto [count, per_kibibyte, library_gaps]{library_supply(certified_bytes, corpus, classes)};

    std::vector<std::string> lines{};

    lines.push_back(std::format("{} | anchors per KiB: {:.1f}", name, per_kibibyte));
    lines.push_back(std::format("{} | anchor gap p50: {}", name, percentile(gaps, 0.5)));
    lines.push_back(std::format("{} | anchor gap p90: {}", name, percentile(gaps, 0.9)));
    lines.push_back(std::format("{} | anchor gap max: {}", name, std::ranges::max(gaps)));

    if (library_gaps)
    {
        lines.push_back(std::format(
                "{} | anchor gaps by supply(): p50 {}, p90 {}, max {}, the runs to the ends excluded", name,
                library_gaps->median, library_gaps->ninetieth, library_gaps->longest));
    }

    const auto snap_max{[&anchors, &class_text](const std::size_t workers) {
        if (anchors.empty())
        {
            return std::string{"no anchors"};
        }

        const auto distances{snap_distances(anchors, class_text.size(), workers)};

        return std::to_string(std::ranges::max(distances));
    }};

    for (const auto workers : snap_workers)
    {
        lines.push_back(std::format("{} | snap max at {} workers: {}", name, workers, snap_max(workers)));
    }

    auto table_snap{snap_max(snap_workers.front())};

    return {.anchors = std::move(anchors),
            .gaps = std::move(gaps),
            .per_kibibyte = per_kibibyte,
            .table_snap = std::move(table_snap),
            .lines = std::move(lines)};
}

/**
 * @brief Returns the anchor table's lines of a campaign row, one `window origin` per certified pair over letters.
 * @param row The row.
 * @param certified_letters The certified pairs over letters.
 * @return The lines, none for a row outside the anchor table.
 */
[[nodiscard]] std::vector<std::string> anchor_table_lines(
        const Campaign_row& row, const std::vector<Pair>& certified_letters)
{
    std::vector<std::string> anchor_lines{};

    if (!row.anchor_table)
    {
        return anchor_lines;
    }

    for (const auto& [window, origin] : certified_letters)
    {
        anchor_lines.push_back(std::format("{} {}", window, origin));
    }

    return anchor_lines;
}

/**
 * @brief Measures one campaign row: its class abstraction, its certified class windows decided exactly and
 *        conservatively, their occurrence, density verdict and supply over the corpus, and the split theorem run over
 *        the whole corpus.
 * @param row The row.
 * @param corpora The directory holding the archived campaign corpora.
 * @return The row's lines, table rows and counts.
 * @throws std::runtime_error If the row's byte classes are not the paper's letters, a decision is unsettled, or a
 *         corpus or chunk does not tokenize.
 */
[[nodiscard]] Campaign_row_report measure_campaign_row(const Campaign_row& row, const std::filesystem::path& corpora)
{
    const auto& [name, corpus_file, grammar, sigma, classify, extra_windows, anchor_table, edit_study]{row};

    munch::core::Builder builder{};

    grammar(builder);

    const auto lexer{builder.build()};

    const auto corpus_path{corpora / std::format("{}.{}", campaign_archive, corpus_file)};

    const auto corpus{read_file(corpus_path)};

    const auto letters{row_letters(name, lexer, sigma, classify)};

    const auto class_text{class_text_of(corpus, letters.canonical)};

    auto [byte_starts, lines]{abstraction_lines(name, sigma, lexer, corpus, letters, class_text)};

    auto [certified, certified_text]{certified_pair_lines(row, lexer, letters)};

    append(lines, std::move(certified_text));

    const auto& [certified_letters, certified_bytes]{certified};

    const auto [vacuous_count, vacuous]{vacuity_of(name, lexer, certified, class_text)};

    const auto vacuous_text{vacuous.empty() ? std::string{"none"} : vacuous};

    lines.push_back(std::format(
            "{} | occurring certificates: {} of {} certified pairs have a window occurring in some completely "
            "tokenizable class string, {} vacuous: {}",
            name, certified_letters.size() - vacuous_count, certified_letters.size(), vacuous_count, vacuous_text));

    const auto verdict{density_verdict(lexer, letters, classify, certified_letters, certified_bytes)};

    lines.push_back(std::format("{} | density verdict: {}", name, verdict));

    auto [anchors, gaps, per_kibibyte, table_snap,
          supply_text]{supply_and_snap_lines(name, corpus, class_text, letters.classes, certified_bytes, byte_starts)};

    append(lines, std::move(supply_text));

    check_split_theorem(name, lexer, corpus, anchors, byte_starts);

    lines.push_back(std::format(
            "{} | split theorem: {} bytes cut at {} anchors, "
            "chunked scans byte-identical to the sequential {}-token scan",
            name, corpus.size(), anchors.size(), byte_starts.size()));

    auto table_row{std::format(
            R"({} & {:.1f} & {} & {} & {} & {} \\)", short_name_of(name), per_kibibyte, percentile(gaps, 0.5),
            percentile(gaps, 0.9), std::ranges::max(gaps), table_snap)};

    auto anchor_lines{anchor_table_lines(row, certified_letters)};

    std::optional<Measured_row> edit_row{};

    if (edit_study)
    {
        edit_row = Measured_row{
                .name = name,
                .lexer = lexer,
                .corpus = corpus,
                .class_text = class_text,
                .classify = classify,
                .sigma = sigma,
                .certified = certified_bytes};
    }

    note(std::format("{}: measured", name));

    return Campaign_row_report{
            .lines = std::move(lines),
            .anchor_lines = std::move(anchor_lines),
            .table_row = std::move(table_row),
            .certified = certified_letters.size(),
            .vacuous = vacuous_count,
            .edit_row = std::move(edit_row)};
}

/**
 * @brief Runs the differential auditor on the campaign's design change, and on a self-pair.
 * @param conventional_row The conventional row.
 * @param split_friendly_row The split-friendly row.
 * @return The lines.
 * @throws std::runtime_error If the design change has no differential.
 */
[[nodiscard]] std::vector<std::string> differential_lines(
        const Measured_row& conventional_row, const Measured_row& split_friendly_row)
{
    const auto& conventional{conventional_row.lexer};

    const auto& split_friendly{split_friendly_row.lexer};

    const auto [witness, exhaustive]{conventional.boundary_difference(split_friendly)};

    if (witness.empty())
    {
        throw std::runtime_error{"the design change must have a differential"};
    }

    const auto letters{letters_of_bytes(conventional_row.classify, witness)};

    const auto shown{[](const std::vector<std::size_t>& starts) {
        std::vector<std::string> parts{};

        for (const auto start : starts)
        {
            parts.push_back(std::format("{}", start));
        }

        return std::format("[{}]", joined(parts, ", "));
    }};

    const auto conventional_starts{token_starts(conventional, witness)};

    const auto split_friendly_starts{token_starts(split_friendly, witness)};

    std::vector<std::string> lines{std::format(
            "campaign differential | conventional against split-friendly: witness '{}' of length {}, "
            "boundaries {} against {}",
            letters, witness.size(), shown(*conventional_starts), shown(*split_friendly_starts))};

    const auto [self_witness, self_exhaustive]{conventional.boundary_difference(conventional)};

    if (self_witness.empty() && self_exhaustive)
    {
        lines.emplace_back(
                "campaign differential | conventional against itself: proved differential-free over every input");
    }

    return lines;
}

/**
 * @brief Returns campaign_inventory.py's three emissions over the four c-like rows: the stats file, the conventional
 *        row's anchor table and the supply table.
 * @param corpora The directory holding the archived campaign corpora.
 * @return The three emissions.
 * @throws std::runtime_error If a row's byte classes are not the paper's letters, or a corpus does not tokenize.
 */
[[nodiscard]] std::vector<Emission> campaign_emissions(const std::filesystem::path& corpora)
{
    std::vector<std::string> lines{};

    std::vector<std::string> anchor_table{
            "# certified (window, origin) pairs of the conventional row, over classes",
            "# L alpha or underscore, D digit, S space or tab, N newline, Q quote,",
            "# C slash, O other operator, P punctuation, X everything else"};

    std::vector<std::string> table_rows{};

    std::size_t certified_total{0};

    std::size_t vacuous_total{0};

    std::vector<Measured_row> edit_rows{};

    for (const auto& row : campaign_rows())
    {
        auto [row_lines, anchor_lines, table_row, certified, vacuous, edit_row]{measure_campaign_row(row, corpora)};

        append(lines, std::move(row_lines));

        append(anchor_table, std::move(anchor_lines));

        table_rows.push_back(std::move(table_row));

        certified_total += certified;

        vacuous_total += vacuous;

        if (edit_row)
        {
            edit_rows.push_back(std::move(*edit_row));
        }
    }

    lines.push_back(std::format(
            "campaign occurrence | over the four rows: {} certified pairs, {} occurring and {} vacuous",
            certified_total, certified_total - vacuous_total, vacuous_total));

    const auto& conventional_row{edit_rows.front()};

    const auto& split_friendly_row{edit_rows.back()};

    lines.push_back(std::format(
            "campaign edit protocol | seed {}, rows {} and {}, {} cross-class and {} same-class substitutions per row, "
            "the generator reseeded per row",
            campaign_edit_seed, short_name_of(conventional_row.name), short_name_of(split_friendly_row.name),
            campaign_cross_class_edits, campaign_same_class_edits));

    for (const auto& row : edit_rows)
    {
        append(lines, campaign_edit_lines(row));

        note(std::format("{}: edits replayed", row.name));
    }

    append(lines, differential_lines(conventional_row, split_friendly_row));

    auto table{closed_table(
            {R"(\begin{tabular}{@{}lrrrrr@{}})", R"(\toprule)",
             R"(Campaign row & anchors/KiB & gap p50 & gap p90 & gap max & snap max (8) \\)", R"(\midrule)"},
            table_rows)};

    return {Emission{.file = "campaign-splitting-stats.txt", .lines = std::move(lines)},
            Emission{.file = "campaign-anchor-table.txt", .lines = std::move(anchor_table)},
            Emission{.file = "campaign-supply-table.tex", .lines = std::move(table)}};
}

/**
 * @brief Returns the wide cutoff sweep's decisions: over every token set of up to three tokens of up to three letters
 *        over the alphabet ab, every window of length two to four at every origin, decided by window_counterexample();
 *        the triples, the refuted ones, and the refuted ones whose shortest counterexample attains the cutoff |W| + 2L
 *        - 2.
 * @return The emission, the parameter lines as the Python writes them and the three counts it decides.
 */
[[nodiscard]] Emission sweep_emission()
{
    const std::string_view letters{"ab"};

    const auto strings_of{[letters](const std::size_t shortest, const std::size_t longest) {
        std::vector<std::string> strings{};

        for (auto length{shortest}; length <= longest; ++length)
        {
            const auto count{std::size_t{1} << length};

            for (std::size_t code{0}; code < count; ++code)
            {
                std::string text{};

                for (std::size_t at{0}; at < length; ++at)
                {
                    text.push_back(letters[(code >> (length - 1 - at)) & 1U]);
                }

                strings.push_back(text);
            }
        }

        return strings;
    }};

    constexpr std::size_t longest_pool_token{3};

    const auto pool{strings_of(1, longest_pool_token)};

    const auto windows{strings_of(widths.front(), widths.back())};

    const auto taken_of{[&pool](const std::vector<Taken>& take) {
        std::vector<std::string> tokens{};

        for (const auto& [token, taken] : std::views::zip(pool, take))
        {
            if (taken == Taken::yes)
            {
                tokens.push_back(token);
            }
        }

        return tokens;
    }};

    std::vector<std::vector<std::string>> universes{};

    constexpr std::size_t largest_universe{3};

    for (std::size_t size{1}; size <= largest_universe; ++size)
    {
        std::vector<Taken> take(pool.size(), Taken::no);

        std::ranges::fill(take | std::views::take(size), Taken::yes);

        do
        {
            universes.push_back(taken_of(take));
        } while (std::ranges::prev_permutation(take).found);
    }

    std::atomic<std::size_t> checked{0};

    std::atomic<std::size_t> refuted{0};

    std::atomic<std::size_t> tight{0};

    std::atomic<std::size_t> unsettled{0};

    std::atomic<std::size_t> next{0};

    const auto decide_universe{[&](const std::vector<std::string>& tokens) {
        const auto lexer{compile(tokens)};

        const auto longest{longest_length(tokens)};

        for (const auto& window : windows)
        {
            const auto cutoff{window.size() + (2 * longest) - 2};

            for (std::size_t origin{0}; origin < window.size(); ++origin)
            {
                const auto [witness, exhaustive]{lexer.window_counterexample(window, origin)};

                ++checked;

                if (!exhaustive)
                {
                    ++unsettled;

                    continue;
                }

                if (witness.empty())
                {
                    continue;
                }

                ++refuted;

                if (witness.size() == cutoff)
                {
                    ++tight;
                }
            }
        }
    }};

    const auto worker{[&] {
        for (auto index{next.fetch_add(1)}; index < universes.size(); index = next.fetch_add(1))
        {
            decide_universe(universes[index]);
        }
    }};

    on_every_thread(worker);

    std::vector<std::string> lines{
            "  alphabet ab, token length to 3, token-set size to 3, windows every",
            "  string of lengths 2 through 4, every origin, margin 3 above each cutoff",
            "  cutoff under test: N = |W| + 2L - 2, with L the longest token of the set",
            std::format("  triples checked: {}", checked.load()),
            std::format("  triples with a counterexample (refuted): {}", refuted.load()),
            std::format(
                    "  refuted triples whose shortest counterexample attains the cutoff (tight): {}", tight.load())};

    if (unsettled.load() > 0)
    {
        lines.push_back(std::format("  triples unsettled at the cap: {}", unsettled.load()));
    }

    return Emission{.file = "wide-cutoff-sweep.txt", .lines = std::move(lines)};
}

/**
 * @brief Returns the sections a command line names after its four paths: a name that is none of the known sections runs
 *        nothing, which is refused rather than run empty.
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The sections named, none when every section runs.
 * @throws std::runtime_error If a name is no section.
 */
[[nodiscard]] std::set<std::string> named_sections(const int argc, char** argv)
{
    const std::vector<std::string> leading{known_sections.begin(), std::prev(known_sections.end())};

    const auto known_list{std::format("{} and {}", joined(leading, ", "), known_sections.back())};

    std::set<std::string> sections{};

    for (int index{first_section_argument}; index < argc; ++index)
    {
        const std::string_view section{argv[index]};

        if (!std::ranges::contains(known_sections, section))
        {
            throw std::runtime_error{std::format("no section is named {}; the sections are {}", section, known_list)};
        }

        sections.emplace(section);
    }

    return sections;
}

/**
 * @brief Runs the sections that need no external file: the UTF-8 shape's sync distance and the wide cutoff sweep.
 * @param out The output directory.
 */
void run_offline(const std::filesystem::path& out)
{
    write(out, Emission{.file = "splitting-stats.txt", .lines = {sync_line(utf8_shape(), "utf8-shape")}});

    write(out, sweep_emission());
}

/**
 * @brief Runs the sections over the corpus's slices that the command line wants: the byte-pair vocabularies trained and
 *        measured, then the vocabulary, budget, depth and frozen emissions.
 * @tparam Wanted The type of the section filter.
 * @param out The output directory.
 * @param corpus_path The corpus.
 * @param merges_path The GPT-2 merge table.
 * @param wanted The predicate deciding whether a section runs.
 * @throws std::runtime_error If the corpus is too short or a file cannot be read.
 */
template <typename Wanted>
void run_slice_sections(
        const std::filesystem::path& out, const std::filesystem::path& corpus_path,
        const std::filesystem::path& merges_path, const Wanted& wanted)
{
    const auto corpus{read_file(corpus_path)};

    if (corpus.size() < train_bytes + eval_bytes + calibration_bytes)
    {
        throw std::runtime_error{"the corpus is too short for the training, evaluation and calibration slices"};
    }

    const std::string_view text{corpus};

    const auto sample{text.substr(train_bytes, eval_bytes)};

    const auto calibration{text.substr(train_bytes + eval_bytes, calibration_bytes)};

    const auto training{text.substr(0, train_bytes)};

    const auto [merges, stats]{train_bpe(training, merges_local)};

    note(std::format("{} merges trained", merges.size()));

    std::vector<Vocabulary> vocabularies{};

    auto local{local_tokens(merges, merges_local)};

    vocabularies.push_back(measure_vocabulary("local-384", "local-384", std::move(local), sample));

    const auto merges_text{read_file(merges_path)};

    auto gpt2{build_gpt2(merges_text)};

    vocabularies.push_back(measure_vocabulary("gpt2-4k", "GPT2-prefix-4k", std::move(gpt2), sample));

    if (wanted("vocabularies"))
    {
        const auto shallow{local_tokens(merges, depths.front())};

        for (const auto& emission : vocabulary_emissions(vocabularies, stats, shallow, sample, calibration))
        {
            write(out, emission);
        }
    }

    if (wanted("budget"))
    {
        const auto emission{budget_emission(vocabularies, sample)};

        write(out, emission);
    }

    if (wanted("depth"))
    {
        const auto emission{depth_emission(merges, vocabularies.front(), sample)};

        write(out, emission);
    }

    if (wanted("frozen"))
    {
        for (const auto& emission : frozen_emissions(vocabularies, sample, calibration))
        {
            write(out, emission);
        }
    }
}

} // namespace

/**
 * @brief Recomputes the emissions: with the output directory alone the offline sections, otherwise every section the
 *        command line names, or all of them, over the corpus, the GPT-2 merge table and the campaign corpora.
 * @param argc The argument count.
 * @param argv The output directory, then the corpus, the merge table, the campaign corpus directory and the sections.
 * @return EXIT_SUCCESS when every section ran, EXIT_FAILURE on a usage error or a failed section.
 */
int main(const int argc, char** argv)
{
    if (argc < 2 || (argc > 2 && argc < first_section_argument))
    {
        std::cerr << "usage: munch_paper4_recompute <output directory> [<twitter.json> <gpt2-merges.txt> "
                     "<campaign corpus directory>] [section...]\n";

        return EXIT_FAILURE;
    }

    try
    {
        const std::filesystem::path out{argv[1]};

        std::filesystem::create_directories(out);

        // The manifest is this run's, so it starts empty: a file an earlier run wrote stays on disk and is named by no
        // manifest, which is how the comparison tells the two apart.
        std::ofstream{out / manifest_file, std::ios::binary | std::ios::trunc};

        const auto sections{named_sections(argc, argv)};

        const auto wanted{[&sections](const std::string_view section) {
            return sections.empty() || sections.contains(std::string{section});
        }};

        if (argc == 2)
        {
            run_offline(out);

            return EXIT_SUCCESS;
        }

        if (wanted("sweep"))
        {
            write(out, sweep_emission());
        }

        if (wanted("campaign"))
        {
            for (const auto& emission : campaign_emissions(argv[4]))
            {
                write(out, emission);
            }
        }

        const auto slices{wanted("vocabularies") || wanted("budget") || wanted("depth") || wanted("frozen")};

        if (slices)
        {
            run_slice_sections(out, argv[2], argv[3], wanted);
        }

        return EXIT_SUCCESS;
    }
    catch (const std::exception& failure)
    {
        std::cerr << std::format("munch_paper4_recompute: {}\n", failure.what());

        return EXIT_FAILURE;
    }
}
