// Searches for candidate multi-byte windows in the grammars whose single-byte certificate is empty, and checks the
// search's model against the shipped scanner and the shipped window decision: fifteen named rows, two strictness rows
// and the oracle's teeth row, the failure-restart regression, the false-origin controls, the vacuity witness and a
// sweep of 400 random grammars, every figure pinned. The quotient search is gate_search, the scanner evidence
// gate_evidence, the sweep gate_sweep and the running totals gate_totals.
//
// Usage: munch_window_gate, with no arguments. It exits 0 when every row, control and pinned figure holds, 1 otherwise.
//
// The model, implemented in window_model.cpp of munch_probes, is conservative and proved sound in this comment. It
// reports windows it can justify and refuses ones it cannot, so its counts are a lower bound on what genuinely
// certifies, never an upper one. The proof is stated here in full so the probe stands alone, with or without the
// companion paper that states the same model and argument.
//
// Assumptions. The DFA is trimmed to its live states L, meaning reachable and co-accessible; q0 is live; no token
// matches the empty string; and the input considered is one the scanner tokenizes completely. Note that "the scan is in
// a live state" is a claim about the final segmentation's token prefixes, which is what the lemma tracks. Speculative
// lookahead that maximal munch later rewinds away can sit in a state that is not co-accessible at all.
//
// Notation. Fix an input x whose window W = w_0 ... w_{k-1} occupies offsets [t, t+k), with k >= 1. Maximal munch gives
// x a unique boundary set. C_j is the cloud after the model has consumed the first j bytes of the window. For j in 1..k
// let sigma_j be the start of the token containing the byte at t+j-1 in the final greedy segmentation, let rho_j =
// delta*(q0, x[sigma_j .. t+j)) be that token's prefix state after consuming through byte t+j-1, and let omega_j be
// sigma_j - t, or "before" when sigma_j < t.
//
// Representation lemma. For every j in 1..k, the pair (rho_j, omega_j) is in C_j. Note what this does and does not say:
// rho_j is the prefix state of the token the final segmentation assigns to that byte, not wherever the scanner's read
// head happens to be. Speculative lookahead that is later rewound away occupies other states, and the lemma says
// nothing about them.
//
// Every rho_j is live: reachable by construction, and co-accessible because its token ends at some e >= t+j in an
// accepting state with x[t+j .. e) carrying rho_j there.
//
// Base, j = 1. If sigma_1 < t then omega_1 is "before", the state p = delta*(q0, x[sigma_1 .. t)) is live and so lies
// in C_0, which is all of L paired with "before", and the direct branch carries it to (rho_1, before) with the origin
// intact. The begins rename cannot interfere for the same reason it cannot in the step: p has consumed at least one
// byte, so p = q0 only if a non-empty live path returns to q0, which makes init_reentrant true and begins false. If
// sigma_1 = t then rho_1 = delta(q0, w_0) and the seed fires, because C_0 is all of L and contains at least one live
// accepting state: the grammar has a token, and that token's accepting state is reachable and trivially co-accessible.
//
// Step, j to j+1. Two cases for the byte at t+j.
//
//   It continues its token, sigma_{j+1} = sigma_j. Then rho_{j+1} = delta(rho_j, w_j), live as above, and the direct
//   branch carries the pair with its origin unchanged. The begins rename does not overwrite that origin, which is the
//   one place the implementation's "begins ? at : origin" needs its own argument: the rename fires only when the
//   pre-step state is q0 and q0 is not re-entrant. Here rho_j has consumed at least the byte at t+j-1, so if rho_j = q0
//   then a non-empty live path returns to q0, which is exactly what init_reentrant detects, so it is true, begins is
//   false, and the origin survives. Where q0 is genuinely not re-entrant, rho_j = q0 cannot arise and the case is
//   vacuous.
//
//   It begins a token, sigma_{j+1} = t+j. Then omega_{j+1} = j and rho_{j+1} = delta(q0, w_j), live because the new
//   token runs to an accepting state through it. The seed emits exactly that pair, so all that is needed is that the
//   seed fires, which needs some state in C_j to accept. It does: the token that ended at t+j ended there because
//   x[sigma_j .. t+j) is a token, so delta*(q0, x[sigma_j .. t+j)) accepts, and that state is rho_j, in C_j by
//   hypothesis.
//
// Soundness. If every pair in C_k carries the same origin o and o is not "before", then (rho_k, omega_k) is in C_k by
// the lemma, so omega_k = o, so sigma_k = t + o, which is a boundary. Since x was an arbitrary completely tokenized
// input containing W at t, the certificate holds in every context.
//
// Backup never appears in the argument. The model tracks where tokens begin rather than what the scanner reads, so a
// boundary backup later exposes was already seeded when the accepting position justifying it was crossed.
//
// A transition that restarts a trajectory whenever it cannot consume the byte, in place of the acceptance-gated seed,
// is refuted, not merely unproved: over tokens {a, abc, bx, x} and the window "abx" its cloud collapses to one
// trajectory per step and ends certifying origin 2, yet the input "abx" itself tokenizes as a|bx with the covering
// token beginning at offset 1, a false certificate at a witnessed occurrence. A trajectory that cannot consume a byte
// is an impossible history rather than a token boundary, and restarting it manufactures support no execution justifies.
// The failure-restart regression keeps that refutation executable.
//
// The model seeds one fresh trajectory at the window's first byte and thereafter only where some tracked state accepts,
// because a token can only end where the automaton accepted. The cloud then contains the final segmentation's actual
// token-prefix history, alongside conservative hypotheses, so backup never has to be simulated. It is conservative in
// the other direction: over {a, ab, b} the scanner always takes "ab", but the accepting "a" also seeds "b" and the
// window is refused.
//
// Acceptance gating alone does not terminate, since a grammar like a+ accepts after every byte and accumulates origins
// without bound. The search therefore deduplicates on a finite quotient rather than on the cloud: which states carry
// the pre-window origin, and how many in-window origins each state carries, saturated at two. Two clouds sharing a key
// have identical futures for certification, which is all the walk asks of them, so exploring one of them loses nothing
// and the walk decides this model exactly. The walk also carries a safety threshold on retained keys, and a search that
// exceeds it is reported inconclusive rather than negative; exhausting the finite quotient below the threshold is a
// conclusive model-negative. What even exhaustion does not give is semantic non-existence: the model itself refuses
// windows a greedy scanner would allow.
//
// The single-byte certificate answers "which bytes always begin a token". Six rows of the applicability table answer
// none, which is the published result's sharpest limitation. A window generalizes the question: a byte string after
// which the current token's start is known whatever preceded it. A certified byte is the length-one case, so the search
// must reproduce is_split_point() exactly at that length, and this program asserts that before searching.
//
// The model. A worker cutting blind knows only that the scan is in one of the trim states, so the cloud starts as all
// of them carrying a token that began before the window. Reading a byte maps that uncertainty forward: a state
// consuming the byte into a trim state does so and keeps its token's start offset, and a state that cannot is an
// impossible history and is dropped. Separately, one fresh trajectory is seeded at the window's first byte and
// thereafter wherever some tracked state accepts, because a token can only begin where the previous one ended and one
// can only end where the automaton accepted. The cloud therefore represents every way the input can be cut into token
// words, not only the greedy way, which is what makes it independent of backup and also what makes it conservative. The
// window is certified when every surviving trajectory agrees on a start offset inside it. Agreement on the state alone
// is not enough, since learning that the scan is inside a string literal is knowledge rather than a boundary.
//
// Longest-match backup is what makes the model non-obvious. A token that cannot extend does not end at the byte that
// killed it; the scan rewinds to the last accepting position and re-reads. The model never simulates that, because it
// tracks where tokens begin rather than what the scanner reads: a boundary backup later exposes was seeded when the
// accepting position justifying it was crossed. That is the proof's step for a byte beginning a token, and it is why a
// failure restart cannot be part of the model. backup_disagreements() is therefore a check on the implementation rather
// than evidence for the model, and it asserts the scanner never disagrees. Its prefixes cover (state, distance past the
// last accepting position) pairs, each optionally preceded by an accepted word so the last boundary sits at varying
// distances before the window, which is what decides where a rewind lands. The prefix count is reported per row, since
// a coverage widening that widens nothing looks exactly like one that works.
//
// A backup check over inputs that never rewind proves nothing, so each row declares whether its own check exercises a
// rewind and that declaration is asserted. The first seven rows do not, which is a fact about the inputs this search
// builds rather than about the grammars: the block-comment grammar can rewind, on an unfinished comment opener after
// the slash has been accepted, but no window it reports produces one. Seven search rows plus the named abx check carry
// the backup evidence over six distinct mechanisms, two of the rows sharing the short-token-then-longer-token gap: that
// gap, a numeric exponent, a float competing with a range operator, an operator ladder, a keyword extending an
// identifier, and a seven-byte rewind depth.
//
// Random grammars are the strongest check here, since hand-picked ones need not reach every case: reading from the
// initial state does not begin a token when a nullable pattern makes that state re-entrant, and none of the named rows
// has a re-entrant initial state. The sweep decides nullable grammars as the library does, through the positive-width
// equivalent unroll_start() compiles, whose start neither accepts nor is re-entered, and counts them, so the
// cross-check against the shipped scanner covers the two thirds of the sample they make up.
//
// The same caution applies to the length-one agreement. The first seven rows certify no byte at all, so agreeing with
// is_split_point is 0 == 0 there and proves little. Six of the seven rewinding rows have non-empty certificates, the
// float-against-range one being the exception with a shortest window of two, and on them the comparison has teeth: a
// trajectory reading from the initial state begins a token at that offset rather than inheriting the origin it carried
// in.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/lexer.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/dfa/unroll_start.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/probes/builder_dbg.hpp"
#include "munch/tools/probes/gate_evidence.hpp"
#include "munch/tools/probes/gate_search.hpp"
#include "munch/tools/probes/gate_sweep.hpp"
#include "munch/tools/probes/gate_totals.hpp"
#include "munch/tools/probes/study_rows.hpp"
#include "munch/tools/probes/window_model.hpp"

namespace
{
using figures::Token;
using munch::tools::probes::Backup;
using munch::tools::probes::backup_disagreements;
using munch::tools::probes::Builder_dbg;
using munch::tools::probes::certified_pairs;
using munch::tools::probes::Certified_window;
using munch::tools::probes::Cloud_t;
using munch::tools::probes::conventional_row;
using munch::tools::probes::covering_violations;
using munch::tools::probes::every_byte;
using munch::tools::probes::find_witness;
using munch::tools::probes::Gate_totals;
using munch::tools::probes::is_certified;
using munch::tools::probes::is_init_reentrant;
using munch::tools::probes::live_states;
using munch::tools::probes::predicted;
using munch::tools::probes::published_cumulative_row;
using munch::tools::probes::random_grammars;
using munch::tools::probes::shortest_windows;
using munch::tools::probes::States_t;
using munch::tools::probes::subset_budget;
using munch::tools::probes::Sweep;
using munch::tools::probes::unknown_cloud;
using namespace munch::regex;

using munch::dfa::Dfa;

/**
 * @brief The random grammars the sweep draws, every one of which the pinned figures count as usable.
 */
constexpr std::size_t sweep_grammars{400};

/**
 * @brief A named row: the grammar's name and every figure its check pins.
 */
struct Row
{
    /**
     * @brief The row's name, printed in its first column.
     */
    std::string_view name{};

    /**
     * @brief The length of the shortest certified window, 0 for a row with none.
     */
    std::size_t shortest{0};

    /**
     * @brief Whether the backup check's own generated inputs rewind at least once, which the row asserts.
     */
    bool rewinds_expected{false};

    /**
     * @brief The quotient keys the shortest-window search retains.
     */
    std::size_t keys{0};

    /**
     * @brief The displayed example window, escaped; empty for a row with none.
     */
    std::string_view example{};

    /**
     * @brief The origin the model certifies the example window at.
     */
    std::size_t example_origin{0};

    /**
     * @brief The completely tokenizable input the witness search finds around the example window, escaped; empty for a
     *        row with no window.
     */
    std::string_view witness{};
};

/**
 * @brief What the covering-token check of a claim found: whether the model refuses the window, and the window's
 *        occurrences and violations over every completely tokenizable input up to the bound.
 */
struct Claim_counts
{
    /**
     * @brief Whether the model refuses the window.
     */
    bool refused{false};

    /**
     * @brief The window's occurrences.
     */
    std::size_t occurrences{0};

    /**
     * @brief The occurrences whose covering token begins elsewhere than the claimed origin.
     */
    std::size_t violations{0};
};

/**
 * @brief A strictness or teeth claim about one window: where its covering tokens begin, over which inputs, and the
 *        counts its check pins.
 */
struct Claim
{
    /**
     * @brief The row's name, printed in its first column.
     */
    std::string_view name{};

    /**
     * @brief The window.
     */
    std::string window{};

    /**
     * @brief The claimed origin inside the window.
     */
    std::size_t origin{0};

    /**
     * @brief The bytes the inputs are drawn from.
     */
    std::string alphabet{};

    /**
     * @brief The longest input.
     */
    std::size_t max_length{0};

    /**
     * @brief The occurrences pinned.
     */
    std::size_t occurrences_expected{0};

    /**
     * @brief The violations pinned, for a teeth claim.
     */
    std::size_t violations_expected{0};
};

/**
 * @brief One grammar compiled for a check: the positive-width automaton the model walks and the lexer that scans.
 */
struct Compiled
{
    /**
     * @brief The grammar's automaton through unroll_start(), whose start neither accepts nor is re-entered.
     */
    Dfa dfa;

    /**
     * @brief The lexer built from the grammar.
     */
    munch::core::Lexer lexer;

    /**
     * @brief The automaton's trim states.
     */
    States_t live{};
};

/**
 * @brief Compiles a grammar for a check.
 * @param builder The grammar.
 * @return The positive-width automaton, the lexer and the trim states.
 */
Compiled compile(const Builder_dbg& builder)
{
    auto dfa{munch::dfa::unroll_start(builder.dfa())};

    auto lexer{builder.build()};

    auto live{live_states(dfa)};

    return {.dfa = std::move(dfa), .lexer = std::move(lexer), .live = std::move(live)};
}

/**
 * @brief Returns whether a compiled grammar can carry evidence: its trim states are not empty and contain the initial
 *        state. Prints the row's rejection when they cannot.
 * @param name The row's name.
 * @param compiled The compiled grammar.
 * @return True when the initial state is live.
 */
bool usable_or_rejected(const std::string_view name, const Compiled& compiled)
{
    const auto& [dfa, lexer, live]{compiled};

    if (!live.empty() && live.contains(dfa.init_state()))
    {
        return true;
    }

    std::printf(
            "  %-30s rejected: no live path from the initial state, evidence would be vacuous\n",
            std::string{name}.c_str());

    return false;
}

/**
 * @brief Counts the bytes the model and the shipped is_split_point() disagree about at length one, cross-checking the
 *        shipped window decision on every byte.
 * @param totals The gate's totals, which count the cross-checks.
 * @param compiled The compiled grammar.
 * @return The bytes on which the model's certificate and is_split_point() differ.
 */
std::size_t single_byte_disagreements(Gate_totals& totals, const Compiled& compiled)
{
    const auto& [dfa, lexer, live]{compiled};

    const auto reentrant{is_init_reentrant(dfa, live)};

    std::size_t disagreements{0};

    for (const auto symbol : every_byte())
    {
        const std::string one{symbol};

        const auto at{predicted(dfa, live, one, reentrant)};

        totals.cross_check(lexer, one, at);

        const auto differs{at.has_value() != lexer.is_split_point(symbol)};

        if (differs)
        {
            ++disagreements;
        }
    }

    return disagreements;
}

/**
 * @brief Returns whether a byte is printable ASCII, a space through a tilde.
 * @param byte The byte.
 * @return Whether it is.
 */
bool is_printable(const char byte)
{
    return byte >= ' ' && byte <= '~';
}

/**
 * @brief Returns the example a row displays: the first of the found windows whose bytes are printable, newline or tab,
 *        else the first found.
 * @param found The windows the search found, in its order.
 * @return The example, empty when nothing was found.
 */
std::string readable_example(const std::vector<std::string>& found)
{
    const auto readable{[](const char byte) { return is_printable(byte) || byte == '\n' || byte == '\t'; }};

    const auto all_readable{
            [&readable](const std::string& candidate) { return std::ranges::all_of(candidate, readable); }};

    if (const auto first_readable{std::ranges::find_if(found, all_readable)}; first_readable != found.end())
    {
        return *first_readable;
    }

    if (found.empty())
    {
        return {};
    }

    return found.front();
}

/**
 * @brief Renders a window printably: newline, tab and carriage return as their escapes, other bytes outside the
 *        printable range as `\xhh`.
 * @param window The window.
 * @return The rendering.
 */
std::string escaped(const std::string& window)
{
    std::string out{};

    for (const auto byte : window)
    {
        switch (byte)
        {
        case '\n':
            out += R"(\n)";

            break;

        case '\t':
            out += R"(\t)";

            break;

        case '\r':
            out += R"(\r)";

            break;

        default:
            if (is_printable(byte))
            {
                out += byte;
            }
            else
            {
                const unsigned value{static_cast<unsigned char>(byte)};

                out += std::format(R"(\x{:02x})", value);
            }

            break;
        }
    }

    return out;
}

/**
 * @brief Returns whether a strictness claim is well formed: the window is not empty and the origin lies inside it.
 *        Prints the row's rejection when it is not.
 * @param name The row's name.
 * @param window The window.
 * @param origin The claimed origin.
 * @return True when the claim is well formed.
 */
bool well_formed_claim(const std::string_view name, const std::string& window, const std::size_t origin)
{
    if (!window.empty() && origin < window.size())
    {
        return true;
    }

    std::printf(
            "  %-30s rejected: empty window or origin outside it, the claim is malformed\n", std::string{name}.c_str());

    return false;
}

/**
 * @brief Runs the backup check of a row over the windows of the reported minimum length and every certified two-byte
 *        window, cross-checking each certified pair, and adds its rewinding executions to the gate's totals.
 * @param totals The gate's totals.
 * @param compiled The row's compiled grammar.
 * @param found The windows of the reported minimum length.
 * @param reentrant Whether a live transition re-enters the initial state.
 * @return The backup check's counts.
 */
Backup row_backup(
        Gate_totals& totals, const Compiled& compiled, const std::vector<std::string>& found, const bool reentrant)
{
    const auto& [dfa, lexer, live]{compiled};

    auto windows{found};

    const auto pairs{certified_pairs(dfa, live, reentrant)};

    std::ranges::transform(pairs, std::back_inserter(windows), &Certified_window::window);

    const auto backup{backup_disagreements(dfa, lexer, live, windows)};

    totals.exercised_total += backup.exercised;

    totals.exercised_tokenizable += backup.tokenizable;

    return backup;
}

/**
 * @brief Returns the origin the model certifies a row's displayed example at, cross-checked against the shipped window
 *        decision.
 * @param totals The gate's totals.
 * @param compiled The row's compiled grammar.
 * @param example The displayed example, empty for none.
 * @param reentrant Whether a live transition re-enters the initial state.
 * @return The origin, std::nullopt when there is no example or the model refuses it.
 */
std::optional<std::size_t> example_origin(
        Gate_totals& totals, const Compiled& compiled, const std::string& example, const bool reentrant)
{
    if (example.empty())
    {
        return std::nullopt;
    }

    const auto& [dfa, lexer, live]{compiled};

    const auto at{predicted(dfa, live, example, reentrant)};

    totals.cross_check(lexer, example, at);

    return at;
}

/**
 * @brief Returns the witness around a row's displayed example: the search is restricted to that example at its
 *        certified origin.
 * @param totals The gate's totals.
 * @param compiled The row's compiled grammar.
 * @param example The displayed example.
 * @param origin The example's certified origin, std::nullopt when it has none.
 * @return The witness input, escaped, std::nullopt when the search found none.
 */
std::optional<std::string> example_witness(
        Gate_totals& totals, const Compiled& compiled, const std::string& example,
        const std::optional<std::size_t> origin)
{
    const auto& [dfa, lexer, live]{compiled};

    std::vector<Certified_window> witness_words{};

    if (origin)
    {
        witness_words.push_back({.window = example, .origin = *origin});
    }

    const auto witness{find_witness(totals, dfa, lexer, live, witness_words)};

    if (!witness)
    {
        return std::nullopt;
    }

    const auto& [input, window]{*witness};

    return escaped(input);
}

/**
 * @brief Checks one named row and prints its line: the length-one agreement, the shortest window search, the backup
 *        check over the found windows and every certified two-byte window, the displayed example with its origin and
 *        the witness around it, each against the row's pinned figure; every answer the model gives on the row is
 *        cross-checked against the shipped window decision.
 * @param totals The gate's totals.
 * @param row The row's pinned figures.
 * @param builder The row's grammar.
 * @return True when every pinned figure holds, the search is conclusive and the backup check agrees.
 */
bool run(Gate_totals& totals, const Row& row, const Builder_dbg& builder)
{
    const auto& [name, pinned_shortest, rewinds_expected, pinned_keys, pinned_example, pinned_origin, pinned_witness]{
            row};

    const auto compiled{compile(builder)};

    if (!usable_or_rejected(name, compiled))
    {
        return false;
    }

    const auto& [dfa, lexer, live]{compiled};

    const auto disagreements{single_byte_disagreements(totals, compiled)};

    const auto reentrant{is_init_reentrant(dfa, live)};

    const auto [shortest, found, exhausted, visited]{shortest_windows(dfa, live)};

    const auto [backup, exercised, tokenizable, prefixes]{row_backup(totals, compiled, found, reentrant)};

    const auto covered{rewinds_expected == (exercised > 0)};

    // A search without a window is conclusive only when it exhausted the quotient.
    const auto conclusive{shortest != 0 || exhausted};

    const auto example{readable_example(found)};

    const auto origin{example_origin(totals, compiled, example, reentrant)};

    const auto witness{example_witness(totals, compiled, example, origin)};

    const auto witness_input{witness.value_or("")};

    const auto no_witness_expected{!witness && pinned_witness.empty()};

    const auto witness_pinned{witness && witness_input == pinned_witness};

    const auto witness_ok{shortest == 0 ? no_witness_expected : witness_pinned};

    const auto shown_example{escaped(example)};

    const auto origin_ok{example.empty() || origin == pinned_origin};

    const auto ok{
            disagreements == 0 && shortest == pinned_shortest && backup == 0 && covered && conclusive &&
            visited == pinned_keys && shown_example == pinned_example && origin_ok && witness_ok};

    const auto search_status{exhausted ? "exhausted" : "inconclusive"};

    const auto status{shortest != 0 ? "certified" : search_status};

    const auto origin_text{origin ? std::format(R"(" at {})", *origin) : std::string{}};

    const auto witness_text{witness ? std::format(R"(, witness "{}")", witness_input) : std::string{}};

    const auto example_lead{example.empty() ? "" : R"(, e.g. ")"};

    const auto marker{ok ? "" : "   <- moved"};

    std::printf(
            "  %-30s k=1 %zu, window %zu/%zu %s, backup %zu over %7zu rewinding executions from %4zu prefixes, "
            "%4zu keys%s%s%s%s%s\n",
            std::string{name}.c_str(), disagreements, shortest, pinned_shortest, status, backup, exercised, prefixes,
            visited, example_lead, shown_example.c_str(), origin_text.c_str(), witness_text.c_str(), marker);

    return ok;
}

/**
 * @brief Builds the C-like row with string literals.
 * @return The grammar.
 */
Builder_dbg c_like_strings()
{
    Builder_dbg builder{};

    figures::c_like(builder, false);

    builder.add_token(figures::string_literal(), Token::string, 2);

    return builder;
}

/**
 * @brief Builds the C-like row with `//` line comments.
 * @return The grammar.
 */
Builder_dbg c_like_line_comments()
{
    Builder_dbg builder{};

    figures::c_like(builder, false);

    builder.add_token(figures::line_comment(), Token::line_comment, 1);

    return builder;
}

/**
 * @brief Builds the C-like row with block comments.
 * @return The grammar.
 */
Builder_dbg c_like_block_comments()
{
    Builder_dbg builder{};

    figures::c_like(builder, false);

    builder.add_token(figures::block_comment(), Token::block_comment, 1);

    return builder;
}

/**
 * @brief Builds the conventional C-like row: strings and line comments over whitespace runs that include newline, with
 *        no block-comment token.
 * @return The grammar.
 */
Builder_dbg conventional()
{
    Builder_dbg builder{};

    conventional_row(builder);

    return builder;
}

/**
 * @brief Builds the split-friendly C-like base, newline its own token, with strings, line comments and block comments.
 * @return The grammar.
 */
Builder_dbg split_friendly_block_comments()
{
    Builder_dbg builder{};

    figures::c_like(builder, true);

    builder.add_token(figures::string_literal(), Token::string, 2);

    builder.add_token(figures::line_comment(), Token::line_comment, 1);

    builder.add_token(figures::block_comment(), Token::block_comment, 1);

    return builder;
}

/**
 * @brief Builds the published cumulative C-like row: the conventional base with strings, line comments and block
 *        comments.
 * @return The grammar.
 */
Builder_dbg cumulative()
{
    Builder_dbg builder{};

    published_cumulative_row(builder);

    return builder;
}

/**
 * @brief Builds the RFC 8259 JSON row.
 * @return The grammar.
 */
Builder_dbg rfc_json()
{
    Builder_dbg builder{};

    figures::json(builder);

    return builder;
}

/**
 * @brief Checks a named window against the scanner and the model on a grammar whose search stops at a shorter length:
 *        the backup check over the window must agree and rewind, and the model must certify the window at the expected
 *        origin. Prints the row's line.
 * @param totals The gate's totals.
 * @param name The row's name.
 * @param window The window.
 * @param expected The origin the model must certify.
 * @param builder The grammar.
 * @return True when the backup check agrees and rewinds and the model certifies the window at the expected origin.
 */
bool named_window_agrees(
        Gate_totals& totals, const std::string_view name, const std::string& window, const std::size_t expected,
        const Builder_dbg& builder)
{
    const auto compiled{compile(builder)};

    if (!usable_or_rejected(name, compiled))
    {
        return false;
    }

    const auto& [dfa, lexer, live]{compiled};

    const auto [disagreements, exercised, tokenizable, prefixes]{backup_disagreements(dfa, lexer, live, {window})};

    totals.exercised_total += exercised;

    totals.exercised_tokenizable += tokenizable;

    const auto reentrant{is_init_reentrant(dfa, live)};

    const auto at{predicted(dfa, live, window, reentrant)};

    totals.cross_check(lexer, window, at);

    const auto verdict{at ? std::format("origin {}", *at) : std::string{"refused"}};

    const auto marker{disagreements == 0 ? "" : "   <- model is wrong"};

    std::printf(
            R"(  %-30s window "%s" %s, backup %zu over %zu rewinding executions%s)"
            "\n",
            std::string{name}.c_str(), window.c_str(), verdict.c_str(), disagreements, exercised, marker);

    return disagreements == 0 && exercised > 0 && at == expected;
}

/**
 * @brief Builds a run of spaces, the whitespace token of the literal-token grammars.
 * @return The regex.
 */
Regex space_run()
{
    return plus(any_of(Set{' '}));
}

/**
 * @brief Builds the grammar of {a, abc, b, d} and a space run: "a" accepts, "ab" does not, "abc" does, so scanning
 *        "abd" accepts "a" and rewinds two bytes.
 * @return The grammar.
 */
Builder_dbg rewind_stress()
{
    Builder_dbg builder{};

    builder.add_token(text("a"), Token::identifier, 2);

    builder.add_token(text("abc"), Token::keyword, 1);

    builder.add_token(text("b"), Token::number, 2);

    builder.add_token(text("d"), Token::operator_, 2);

    builder.add_token(space_run(), Token::whitespace, 2);

    return builder;
}

/**
 * @brief Builds the grammar of {a, abc, bx, x} and a space run, "bx" at priority 1: a failure-restart transition
 *        certifies "abx" at origin 2, while the scanner backs up to the accepted "a" and matches "bx", with boundaries
 *        0 and 1.
 * @return The grammar.
 */
Builder_dbg refuted_model_witness()
{
    Builder_dbg builder{};

    builder.add_token(text("a"), Token::identifier, 2);

    builder.add_token(text("abc"), Token::keyword, 1);

    builder.add_token(text("bx"), Token::number, 1);

    builder.add_token(text("x"), Token::operator_, 2);

    builder.add_token(space_run(), Token::whitespace, 2);

    return builder;
}

/**
 * @brief Runs the shared step of the strictness and teeth rows: checks the claim is well formed and the grammar usable,
 *        asks the model about the window, cross-checks the refusal it expects against the shipped window decision, and
 *        counts the window's occurrences and violations over every completely tokenizable input on the alphabet up to
 *        the bound. Prints the row's rejection when the claim or the grammar is not usable.
 * @param totals The gate's totals.
 * @param claim The claim.
 * @param builder The grammar.
 * @return The counts, std::nullopt when the row was rejected.
 */
std::optional<Claim_counts> claim_counts(Gate_totals& totals, const Claim& claim, const Builder_dbg& builder)
{
    const auto& [name, window, origin, alphabet, max_length, occurrences_expected, violations_expected]{claim};

    if (!well_formed_claim(name, window, origin))
    {
        return std::nullopt;
    }

    const auto compiled{compile(builder)};

    if (!usable_or_rejected(name, compiled))
    {
        return std::nullopt;
    }

    const auto& [dfa, lexer, live]{compiled};

    const auto reentrant{is_init_reentrant(dfa, live)};

    const auto refused{!predicted(dfa, live, window, reentrant)};

    totals.cross_check(lexer, window, std::nullopt);

    const auto [occurrences, violations]{covering_violations(lexer, window, origin, alphabet, max_length)};

    return Claim_counts{.refused = refused, .occurrences = occurrences, .violations = violations};
}

/**
 * @brief Checks a strictness claim: the model refuses the window, and over every completely tokenizable input on the
 *        alphabet up to the bound the window occurs the expected number of times, each with its covering token at the
 *        claimed origin. Prints the row's line.
 * @param totals The gate's totals.
 * @param claim The claim, its violations pinned at none.
 * @param builder The grammar.
 * @return True when the model refuses, no occurrence violates the claim, and the occurrence count holds.
 */
bool strict_refusal(Gate_totals& totals, const Claim& claim, const Builder_dbg& builder)
{
    const auto counts{claim_counts(totals, claim, builder)};

    const auto& [name, window, origin, alphabet, max_length, occurrences_expected, violations_expected]{claim};

    if (!counts)
    {
        return false;
    }

    const auto& [refused, occurrences, violations]{*counts};

    const auto ok{refused && violations == 0 && occurrences == occurrences_expected};

    const auto answer{refused ? "refuses" : "certifies"};

    const auto marker{ok ? "" : "   <- strictness claim moved"};

    std::printf(
            "  %-30s model %s, %zu violations over %zu occurrences%s\n", std::string{name}.c_str(), answer, violations,
            occurrences, marker);

    return ok;
}

/**
 * @brief Checks the oracle's teeth: the model refuses the window, and over every completely tokenizable input on the
 *        alphabet up to the bound the covering-token check counts the expected occurrences and violations, at least
 *        one. Prints the row's line.
 * @param totals The gate's totals.
 * @param claim The claim.
 * @param builder The grammar.
 * @return True when the model refuses and both counts hold with a violation among them.
 */
bool oracle_teeth(Gate_totals& totals, const Claim& claim, const Builder_dbg& builder)
{
    const auto counts{claim_counts(totals, claim, builder)};

    const auto& [name, window, origin, alphabet, max_length, occurrences_expected, violations_expected]{claim};

    if (!counts)
    {
        return false;
    }

    const auto& [refused, occurrences, violations]{*counts};

    const auto ok{
            refused && occurrences == occurrences_expected && violations == violations_expected && violations > 0};

    const auto answer{refused ? "refuses" : "certifies"};

    const auto marker{ok ? "" : "   <- oracle lost its teeth"};

    std::printf(
            "  %-30s model %s, oracle counts %zu violations over %zu occurrences%s\n", std::string{name}.c_str(),
            answer, violations, occurrences, marker);

    return ok;
}

/**
 * @brief Builds the grammar of {a, ab, b}, all at priority 1: the scanner always takes "ab", but the accepting "a"
 *        seeds a competing origin for "b", so the model refuses "ab" at origin 0.
 * @return The grammar.
 */
Builder_dbg strict_a_ab_b()
{
    Builder_dbg builder{};

    builder.add_token(text("a"), Token::identifier, 1);

    builder.add_token(text("ab"), Token::keyword, 1);

    builder.add_token(text("b"), Token::operator_, 1);

    return builder;
}

/**
 * @brief Builds the grammar of {ab, abc, c}, all at priority 1: the competing origin comes from the accepting proper
 *        prefix "ab" inside "abc", two bytes into the window.
 * @return The grammar.
 */
Builder_dbg strict_ab_abc_c()
{
    Builder_dbg builder{};

    builder.add_token(text("ab"), Token::identifier, 1);

    builder.add_token(text("abc"), Token::keyword, 1);

    builder.add_token(text("c"), Token::operator_, 1);

    return builder;
}

/**
 * @brief Builds the grammar of {a, abx, b, x}, all at priority 1: "ab" tokenizes as a|b, so a token begins at offset 0
 *        while the token covering the window's final byte begins at 1.
 * @return The grammar.
 */
Builder_dbg teeth_a_abx_b_x()
{
    Builder_dbg builder{};

    builder.add_token(text("a"), Token::identifier, 1);

    builder.add_token(text("abx"), Token::keyword, 1);

    builder.add_token(text("b"), Token::operator_, 1);

    builder.add_token(text("x"), Token::separator, 1);

    return builder;
}

/**
 * @brief Builds the grammar of digits, digits `e` digits, a letter and a space run: "12e" followed by a non-digit
 *        accepts "12" and rewinds one byte.
 * @return The grammar.
 */
Builder_dbg digits_exponent()
{
    Builder_dbg builder{};

    const auto digits{plus(any_of(Set::digits()))};

    const auto exponent{concat(text("e"), digits)};

    builder.add_token(digits, Token::number, 2);

    builder.add_token(concat(digits, exponent), Token::literal, 1);

    builder.add_token(any_of(Set::alpha()), Token::identifier, 2);

    builder.add_token(space_run(), Token::whitespace, 2);

    return builder;
}

/**
 * @brief Builds the grammar of digits, a float, the range operator `..` and a space run: "1..2" accepts "1" and rewinds
 *        off the dot.
 * @return The grammar.
 */
Builder_dbg float_range()
{
    Builder_dbg builder{};

    const auto digits{plus(any_of(Set::digits()))};

    const auto fraction{concat(text("."), digits)};

    builder.add_token(digits, Token::number, 2);

    builder.add_token(concat(digits, fraction), Token::literal, 1);

    builder.add_token(text(".."), Token::operator_, 1);

    builder.add_token(space_run(), Token::whitespace, 2);

    return builder;
}

/**
 * @brief Builds the grammar of `<`, `<<=`, a letter and a space run: "<<x" accepts "<" and rewinds, since "<<" accepts
 *        nothing.
 * @return The grammar.
 */
Builder_dbg operator_ladder()
{
    Builder_dbg builder{};

    builder.add_token(text("<"), Token::operator_, 2);

    builder.add_token(text("<<="), Token::punctuation, 1);

    builder.add_token(any_of(Set::alpha()), Token::identifier, 2);

    builder.add_token(space_run(), Token::whitespace, 2);

    return builder;
}

/**
 * @brief Builds the grammar of `f`, `for`, a letter and a space run: "fox" accepts "f" and rewinds, since "fo" accepts
 *        nothing.
 * @return The grammar.
 */
Builder_dbg keyword_extension()
{
    Builder_dbg builder{};

    builder.add_token(text("f"), Token::identifier, 2);

    builder.add_token(text("for"), Token::keyword, 1);

    builder.add_token(any_of(Set::alpha()), Token::separator, 2);

    builder.add_token(space_run(), Token::whitespace, 2);

    return builder;
}

/**
 * @brief Builds the grammar of `a`, `abcdefgh`, a letter and a space run: seven bytes are scanned past the accepting
 *        "a" before the token dies.
 * @return The grammar.
 */
Builder_dbg deep_rewind()
{
    Builder_dbg builder{};

    builder.add_token(text("a"), Token::identifier, 2);

    builder.add_token(text("abcdefgh"), Token::keyword, 1);

    builder.add_token(any_of(Set::alpha()), Token::separator, 2);

    builder.add_token(space_run(), Token::whitespace, 2);

    return builder;
}

/**
 * @brief Builds the grammar of `a+` alone: every byte continues the run as readily as it starts one, so the search
 *        exhausts the quotient without a window.
 * @return The grammar.
 */
Builder_dbg a_plus()
{
    Builder_dbg builder{};

    builder.add_token(plus(text("a")), Token::identifier, 1);

    return builder;
}

/**
 * @brief Builds the grammar of {a, abc, bx, x}, "abc" at priority 1 and the others at 2, with no space run: the grammar
 *        of the failure-restart regression and the false-origin controls.
 * @return The grammar.
 */
Builder_dbg restart_grammar()
{
    Builder_dbg builder{};

    builder.add_token(text("a"), Token::identifier, 2);

    builder.add_token(text("abc"), Token::keyword, 1);

    builder.add_token(text("bx"), Token::number, 2);

    builder.add_token(text("x"), Token::operator_, 2);

    return builder;
}

/**
 * @brief Walks a window from the maximum uncertainty under the failure-restart transition: a trajectory that consumes
 *        the byte into a live state moves on, its origin renamed where it leaves a non-re-entered initial state, and
 *        one that cannot restarts at this offset from the initial state's successor when that is live.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @param window The window.
 * @param reentrant Whether a live transition re-enters the initial state.
 * @return The cloud at the window's end, std::nullopt when it empties.
 */
std::optional<Cloud_t> restart_cloud(
        const Dfa& dfa, const States_t& live, const std::string& window, const bool reentrant)
{
    auto cloud{unknown_cloud(live)};

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto restart{dfa.advance(dfa.init_state(), window[at])};

        const auto restart_ok{restart && live.contains(*restart)};

        Cloud_t next{};

        for (const auto& [state, origin] : cloud)
        {
            if (const auto direct{dfa.advance(state, window[at])}; direct && live.contains(*direct))
            {
                const auto begins{state == dfa.init_state() && !reentrant};

                next.emplace(*direct, begins ? at : origin);
            }
            else if (restart_ok)
            {
                next.emplace(*restart, at);
            }
        }

        if (next.empty())
        {
            return std::nullopt;
        }

        cloud = std::move(next);
    }

    return cloud;
}

/**
 * @brief Builds the grammar of {0, 00, 01}, all at priority 2: the model certifies "1001" at origin 2, and no
 *        completely tokenizable input contains it.
 * @return The grammar.
 */
Builder_dbg vacuity_grammar()
{
    Builder_dbg builder{};

    builder.add_token(text("0"), Token::number, 2);

    builder.add_token(text("00"), Token::identifier, 2);

    builder.add_token(text("01"), Token::keyword, 2);

    return builder;
}

/**
 * @brief Checks the study-table rows, none of which certifies a byte, each checked by run(): C-like with strings, with
 *        line comments, with block comments, the conventional row, the split-friendly row with block comments, the
 *        cumulative row and JSON. Their backup checks' inputs do not rewind.
 * @param totals The gate's totals.
 * @return True when every row holds.
 */
bool study_table_rows(Gate_totals& totals)
{
    auto ok{true};

    const Row c_like_strings_pins{
            .name = "C-like + string literals",
            .shortest = 2,
            .rewinds_expected = false,
            .keys = 24,
            .example = R"(\n!)",
            .example_origin = 1,
            .witness = R"(\n!)"};

    ok = run(totals, c_like_strings_pins, c_like_strings()) && ok;

    const Row c_like_line_comments_pins{
            .name = "C-like + // line comments",
            .shortest = 2,
            .rewinds_expected = false,
            .keys = 18,
            .example = R"(\n!)",
            .example_origin = 1,
            .witness = R"(\n!)"};

    ok = run(totals, c_like_line_comments_pins, c_like_line_comments()) && ok;

    const Row c_like_block_comments_pins{
            .name = "C-like + block comments",
            .shortest = 4,
            .rewinds_expected = false,
            .keys = 53,
            .example = R"(\t*/\t)",
            .example_origin = 3,
            .witness = R"(\t*/\t)"};

    ok = run(totals, c_like_block_comments_pins, c_like_block_comments()) && ok;

    const Row conventional_pins{
            .name = "C-like conventional",
            .shortest = 2,
            .rewinds_expected = false,
            .keys = 27,
            .example = R"(\n!)",
            .example_origin = 1,
            .witness = R"(\n!)"};

    ok = run(totals, conventional_pins, conventional()) && ok;

    const Row split_friendly_block_comments_pins{
            .name = "split-friendly + block comments",
            .shortest = 4,
            .rewinds_expected = false,
            .keys = 188,
            .example = R"(\n*/\t)",
            .example_origin = 3,
            .witness = R"(\n*/\t)"};

    ok = run(totals, split_friendly_block_comments_pins, split_friendly_block_comments()) && ok;

    const Row cumulative_pins{
            .name = "C-like cumulative (new here)",
            .shortest = 4,
            .rewinds_expected = false,
            .keys = 189,
            .example = R"(\n*/\t)",
            .example_origin = 3,
            .witness = R"(\n*/\t)"};

    ok = run(totals, cumulative_pins, cumulative()) && ok;

    const Row rfc_json_pins{
            .name = "JSON, RFC 8259",
            .shortest = 2,
            .rewinds_expected = false,
            .keys = 69,
            .example = R"(\t")",
            .example_origin = 1,
            .witness = R"(\t"")"};

    ok = run(totals, rfc_json_pins, rfc_json()) && ok;

    return ok;
}

/**
 * @brief Checks the two rewinding rows over grammars built from literal tokens, checked by run(), and the three-byte
 *        window "abx" the second one's search never reaches, checked by named_window_agrees().
 * @param totals The gate's totals.
 * @return True when the three hold.
 */
bool rewind_stress_rows(Gate_totals& totals)
{
    auto ok{true};

    const Row rewind_stress_pins{
            .name = "rewind stress: a | abc | b | d",
            .shortest = 1,
            .rewinds_expected = true,
            .keys = 4,
            .example = "a",
            .example_origin = 0,
            .witness = "a"};

    ok = run(totals, rewind_stress_pins, rewind_stress()) && ok;

    const Row refuted_model_witness_pins{
            .name = "refuted-model witness grammar",
            .shortest = 1,
            .rewinds_expected = true,
            .keys = 5,
            .example = "a",
            .example_origin = 0,
            .witness = "a"};

    ok = run(totals, refuted_model_witness_pins, refuted_model_witness()) && ok;

    ok = named_window_agrees(totals, "refuted-model witness abx", "abx", 1, refuted_model_witness()) && ok;

    return ok;
}

/**
 * @brief Checks the two strictness rows, windows the model refuses although every occurrence's covering token begins at
 *        the claimed origin, and the oracle's teeth row, a window whose occurrences violate the claim.
 * @param totals The gate's totals.
 * @return True when the three hold.
 */
bool strictness_rows(Gate_totals& totals)
{
    auto ok{true};

    const Claim strict_a_ab_b_claim{
            .name = R"(strict: {a, ab, b} at "ab")",
            .window = "ab",
            .origin = 0,
            .alphabet = "ab",
            .max_length = 14,
            .occurrences_expected = 98'305};

    ok = strict_refusal(totals, strict_a_ab_b_claim, strict_a_ab_b()) && ok;

    const Claim strict_ab_abc_c_claim{
            .name = R"(strict: {ab, abc, c} at "abc")",
            .window = "abc",
            .origin = 0,
            .alphabet = "abc",
            .max_length = 12,
            .occurrences_expected = 932};

    ok = strict_refusal(totals, strict_ab_abc_c_claim, strict_ab_abc_c()) && ok;

    const Claim teeth_claim{
            .name = R"(teeth: {a, abx, b, x} at "ab")",
            .window = "ab",
            .origin = 0,
            .alphabet = "abx",
            .max_length = 10,
            .occurrences_expected = 83'653,
            .violations_expected = 59'049};

    ok = oracle_teeth(totals, teeth_claim, teeth_a_abx_b_x()) && ok;

    return ok;
}

/**
 * @brief Checks the five rewinding rows, each accepting a short prefix and continuing into a longer token that can die:
 *        a numeric exponent, a float against a range operator, an operator ladder, a keyword extending a shorter token
 *        and a seven-byte rewind; each checked by run().
 * @param totals The gate's totals.
 * @return True when every row holds.
 */
bool rewind_rows(Gate_totals& totals)
{
    auto ok{true};

    const Row digits_exponent_pins{
            .name = "rewind: digits | digits e digits",
            .shortest = 1,
            .rewinds_expected = true,
            .keys = 4,
            .example = "A",
            .example_origin = 0,
            .witness = "A"};

    ok = run(totals, digits_exponent_pins, digits_exponent()) && ok;

    const Row float_range_pins{
            .name = "rewind: float vs range operator",
            .shortest = 2,
            .rewinds_expected = true,
            .keys = 9,
            .example = " .",
            .example_origin = 1,
            .witness = " .."};

    ok = run(totals, float_range_pins, float_range()) && ok;

    const Row operator_ladder_pins{
            .name = "rewind: < | <<=",
            .shortest = 1,
            .rewinds_expected = true,
            .keys = 4,
            .example = "A",
            .example_origin = 0,
            .witness = "A"};

    ok = run(totals, operator_ladder_pins, operator_ladder()) && ok;

    const Row keyword_extension_pins{
            .name = "rewind: f | for",
            .shortest = 1,
            .rewinds_expected = true,
            .keys = 4,
            .example = "A",
            .example_origin = 0,
            .witness = "A"};

    ok = run(totals, keyword_extension_pins, keyword_extension()) && ok;

    const Row deep_rewind_pins{
            .name = "rewind: a | abcdefgh (depth 7)",
            .shortest = 1,
            .rewinds_expected = true,
            .keys = 9,
            .example = "A",
            .example_origin = 0,
            .witness = "A"};

    ok = run(totals, deep_rewind_pins, deep_rewind()) && ok;

    return ok;
}

/**
 * @brief Checks the row with no window, `a+`, whose negative is conclusive under the model.
 * @param totals The gate's totals.
 * @return True when the row holds.
 */
bool no_window_row(Gate_totals& totals)
{
    const Row a_plus_pins{
            .name = "a+: no window found",
            .shortest = 0,
            .rewinds_expected = false,
            .keys = 3,
            .example = "",
            .example_origin = 0,
            .witness = ""};

    return run(totals, a_plus_pins, a_plus());
}

/**
 * @brief Checks the failure-restart regression on "abx" over the failure-restart grammar: the failure-restart
 *        transition certifies it at 2, the model at 1, and the scan consumes it with the tokens covering its bytes
 *        beginning at 0, 1 and 1. Prints the row's line.
 * @param totals The gate's totals.
 * @return True when all three hold.
 */
bool restart_regression(Gate_totals& totals)
{
    const auto builder{restart_grammar()};

    const auto dfa{builder.dfa()};

    const auto live{live_states(dfa)};

    const auto reentrant{is_init_reentrant(dfa, live)};

    const std::string window{"abx"};

    const auto cloud{restart_cloud(dfa, live, window, reentrant)};

    const auto restart_certified{cloud && is_certified(*cloud)};

    const auto lexer{builder.build()};

    std::vector<std::size_t> covering(window.size(), window.size());

    std::size_t offset{0};

    const auto cover{[&covering, &offset](const Token, const std::size_t length) {
        for (std::size_t inside{0}; inside < length; ++inside)
        {
            covering[offset + inside] = offset;
        }

        offset += length;
    }};

    const auto consumed{lexer.tokenize_all<Token>(window, cover)};

    const auto scan_ok{consumed == window.size() && covering[0] == 0 && covering[1] == 1 && covering[2] == 1};

    const auto repaired{predicted(dfa, live, window, reentrant)};

    totals.cross_check(lexer, window, repaired);

    const auto restart_origin{[&cloud, restart_certified] {
        if (!restart_certified)
        {
            return std::size_t{0};
        }

        const auto& [state, origin]{*cloud->begin()};

        return origin;
    }()};

    const auto regression_holds{scan_ok && repaired == 1 && restart_certified && restart_origin == 2};

    const auto restart_text{restart_certified ? std::to_string(restart_origin) : std::string{"none"}};

    const auto repaired_text{repaired ? std::to_string(*repaired) : std::string{"refused"}};

    const auto marker{regression_holds ? "" : "   <- moved"};

    std::printf(
            R"(  %-30s restart transition certifies "abx" at %s, repaired model at %s, the scan covers its )"
            "final byte from %zu%s\n",
            "legacy: {a, abc, bx, x} at abx", restart_text.c_str(), repaired_text.c_str(), covering[2], marker);

    return regression_holds;
}

/**
 * @brief Checks the false-origin controls on "abx" over the failure-restart grammar: the witness search at the false
 *        origin 2 finds nothing after rejecting 30 candidates, which count into totals of their own; at the true origin
 *        1 it finds a witness; and the backup check forced to origin 2 counts 200 disagreements, its executions outside
 *        the gate's totals. Prints the row's line.
 * @param totals The gate's totals.
 * @return True when all three hold.
 */
bool false_origin_controls(Gate_totals& totals)
{
    const auto builder{restart_grammar()};

    const auto dfa{builder.dfa()};

    const auto lexer{builder.build()};

    const auto live{live_states(dfa)};

    Gate_totals scratch{};

    const auto wrong{find_witness(scratch, dfa, lexer, live, {{.window = "abx", .origin = 2}})};

    const auto rejected{scratch.witness_disagreements};

    const auto right{find_witness(totals, dfa, lexer, live, {{.window = "abx", .origin = 1}})};

    const auto [forced, exercised, tokenizable, prefixes]{backup_disagreements(dfa, lexer, live, {"abx"}, 2)};

    const auto fixture_ok{!wrong && rejected == 30 && right && forced == 200};

    const auto marker{fixture_ok ? "" : "   <- control lost its teeth"};

    std::printf(
            "  %-30s witness at 2 refused with %zu rejections, witnessed at 1, forced origin 2 counts %zu "
            "disagreements%s\n",
            "false-origin controls: abx", rejected, forced, marker);

    return fixture_ok;
}

/**
 * @brief Checks the vacuity witness over {0, 00, 01}: the model certifies "1001" at origin 2 and the bounded witness
 *        search for exactly that word comes back empty. Prints the row's line.
 * @param totals The gate's totals.
 * @return True when both hold.
 */
bool vacuity_witness(Gate_totals& totals)
{
    const auto builder{vacuity_grammar()};

    const auto dfa{builder.dfa()};

    const auto lexer{builder.build()};

    const auto live{live_states(dfa)};

    const auto reentrant{is_init_reentrant(dfa, live)};

    const auto at{predicted(dfa, live, "1001", reentrant)};

    totals.cross_check(lexer, "1001", at);

    const std::vector<Certified_window> only{{.window = "1001", .origin = at.value_or(0)}};

    const auto witness{at ? find_witness(totals, dfa, lexer, live, only) : std::nullopt};

    const auto vacuous_ok{at == 2 && !witness};

    const auto at_text{at ? std::to_string(*at) : std::string{"none"}};

    const auto search_empty{witness ? "no" : "yes"};

    const auto marker{vacuous_ok ? "" : "   <- moved"};

    std::printf(
            R"(  %-30s model certifies "1001" at %s, bounded witness search empty: %s%s)"
            "\n",
            R"(vacuity: {0, 00, 01} at "1001")", at_text.c_str(), search_empty, marker);

    return vacuous_ok;
}

/**
 * @brief Prints the summary: the random sweep's disagreements and applicability, the witnessed applicability, the named
 *        rows' rewinding executions and witness disagreements, the retained search keys, and the shipped window
 *        decision's cross-checks.
 * @param totals The gate's totals.
 * @param sweep The random sweep's counts.
 */
void print_summary(const Gate_totals& totals, const Sweep& sweep)
{
    const auto model_marker{sweep.disagreements == 0 ? "" : "   <- model is wrong"};

    std::printf(
            "\n  %-30s %zu disagreements over %zu grammars, %zu with a non-empty certificate, %zu of them nullable "
            "and decided through their positive-width equivalent%s\n",
            "random grammars", sweep.disagreements, sweep.usable, sweep.with_certificate, sweep.nullable, model_marker);

    const auto no_byte{sweep.usable - sweep.with_certificate};

    std::printf(
            "  %-30s of %zu certifying no byte: %zu model-positive, %zu with none found, %zu inconclusive\n",
            "window applicability", no_byte, sweep.rescued, sweep.proved_none, sweep.inconclusive);

    const auto unresolved{sweep.rescued - sweep.witnessed_rescued};

    std::printf(
            "  %-30s %zu of %zu model-positive grammars have an occurrence-witnessed certificate, %zu unresolved\n",
            "witnessed applicability", sweep.witnessed_rescued, sweep.rescued, unresolved);

    const auto malformed{totals.exercised_total - totals.exercised_tokenizable};

    std::printf(
            "  %-30s %zu rewinding executions across every named row, %zu with a completely tokenizable input, "
            "%zu with a malformed suffix, %zu witness origin disagreements\n",
            "backup total", totals.exercised_total, totals.exercised_tokenizable, malformed,
            totals.witness_disagreements);

    // Keys retained before shortest-window stopping ends each search, not the complete reachable quotient space.
    const auto mean_keys{no_byte == 0 ? 0.0 : static_cast<double>(totals.visited_total) / static_cast<double>(no_byte)};

    std::printf(
            "  %-30s max %zu, mean %.1f over the no-byte grammars, total %zu, against a 6^|Q+| worst case\n",
            "retained search keys", totals.visited_max, mean_keys, totals.visited_total);

    const auto port_marker{totals.port_disagreements == 0 ? "" : "   <- port diverges"};

    std::printf(
            "  %-30s %zu checks against the probe's model, %zu disagreements%s\n", "shipped window decision",
            totals.port_checks, totals.port_disagreements, port_marker);
}

/**
 * @brief Returns whether the pinned figures of the sweep and the totals hold: no disagreement anywhere, 107,198 port
 *        checks, 400 usable grammars of which 266 nullable and 63 with a certificate, 326 of the 337 no-byte grammars
 *        rescued and 322 witnessed, 11 proved empty and none inconclusive, 1,079,392 rewinding executions of which
 *        418,466 tokenize completely, and 32 and 3,343 retained keys at most and in total.
 * @param totals The gate's totals.
 * @param sweep The random sweep's counts.
 * @return True when every figure holds.
 */
bool has_pinned_figures(const Gate_totals& totals, const Sweep& sweep)
{
    const auto malformed{totals.exercised_total - totals.exercised_tokenizable};

    const auto totals_hold{
            totals.port_disagreements == 0 && totals.port_checks == 107'198 && totals.exercised_total == 1'079'392 &&
            totals.exercised_tokenizable == 418'466 && malformed == 660'926 && totals.witness_disagreements == 0 &&
            totals.visited_max == 32 && totals.visited_total == 3'343};

    const auto no_byte{sweep.usable - sweep.with_certificate};

    const auto sweep_holds{
            sweep.disagreements == 0 && sweep.usable == sweep_grammars && sweep.nullable == 266 &&
            sweep.with_certificate == 63 && no_byte == 337 && sweep.rescued == 326 && sweep.witnessed_rescued == 322 &&
            sweep.proved_none == 11 && sweep.inconclusive == 0};

    return totals_hold && sweep_holds;
}

} // namespace

/**
 * @brief Runs the gate: every named row, the strictness and teeth rows, the failure-restart regression, the
 *        false-origin controls, the vacuity witness and the random sweep, then prints the summary and the verdict.
 * @return EXIT_SUCCESS when every row, control and pinned figure holds, EXIT_FAILURE otherwise.
 */
int main()
{
    std::printf(
            "windows where no single byte is certified, under the proved conservative model described in the header\n");

    std::printf(
            "  subset search threshold: inconclusive beyond %zu retained keys, pinned by static_assert\n",
            subset_budget);

    Gate_totals totals{};

    auto ok{true};

    ok = study_table_rows(totals) && ok;

    ok = rewind_stress_rows(totals) && ok;

    ok = strictness_rows(totals) && ok;

    ok = rewind_rows(totals) && ok;

    ok = no_window_row(totals) && ok;

    ok = restart_regression(totals) && ok;

    ok = false_origin_controls(totals) && ok;

    ok = vacuity_witness(totals) && ok;

    const auto sweep{random_grammars(totals, sweep_grammars)};

    print_summary(totals, sweep);

    ok = has_pinned_figures(totals, sweep) && ok;

    const std::string verdict{
            ok ? "The model reproduces is_split_point at length one on six named grammars with a non-empty certificate "
                 "and on all 400 grammars of the random sweep, the 266 nullable ones decided through their "
                 "positive-width equivalent, and wherever it predicts an origin the shipped scanner starts the token "
                 "covering the window's last byte exactly there. The model is proved sound, so this checks the "
                 "implementation rather than the argument." :
                 "A measurement moved. The window results must be re-derived before being relied on."};

    std::printf("\n%s\n", verdict.c_str());

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
