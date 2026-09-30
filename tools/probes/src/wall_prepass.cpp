// Synthesizes carry prepasses from the compiled tables and holds every product to the serial scan.
//
// Some token sets refuse every split certificate for a structural reason: two or more readings of any factor survive
// forever, none ever eliminated, so no byte and no window can pin a boundary. This probe derives, from the tables
// alone, the machinery that resolves such walls, and pins the whole pipeline:
//
//   - the zero-lag premise, decided on a (state, seen-accept) product of the live tables, with the end of input
//     counted as a death available at every position: once a run accepts, every later state accepts, so every
//     rollback, byte or end-of-input, is zero bytes wide; grammars that fail the premise are refused with a witness,
//     never measured approximately;
//   - the wall verdict from that subset graph, state-granular and therefore a lower bound on origin-distinguished
//     readings: a floor of at least two is an absolute wall and licenses the synthesis, while a smaller floor refuses
//     without concluding the absence of an origin-level wall;
//   - the carry: on an absolute wall every byte acts on the kernel's readings as a bijection, a flavor labeling with
//     one permutation of flavors per byte is searched exactly, the permutations generate the carry group, a factor's
//     carry is the ordered product of its bytes' permutations, and the true scan's flavor after any prefix is that
//     product applied to a derived boundary seed, checked here against the real maximal-munch loop at every position
//     of a deterministic corpus;
//   - certificates behind the resolved flavor: the window decision's cloud walk started from one flavor's states alone
//     certifies where the unconditional wall never can, the same soundness argument once the carry names the true
//     reading's flavor; planning driven by prefix carries recovers every target on generated corpora, every cut on
//     the serial segmentation, every spliced boundary sequence equal to the serial one, while the unconditional walk
//     certifies zero windows across the same campaign and deliberately wrong flavors cut wrongly every time;
//   - the price tower: positional control (log2 of the seed's orbit), compositional control (log2 of the group
//     order), and the exact summary (log2 of the kernel's transfer semigroup over nonempty factors), with a hazard
//     witness for every ordered flavor pair: a concrete input whose wrong-flavor cut leaves the serial segmentation.
//     The control prices are the storage of the conditioned-cut carry; the semigroup price calibrates the stronger
//     full-transfer interface, which the cut service never needs. A scheme may pay work instead of bits (rescanning
//     the prefix realizes the same cuts), so scheme-wide bounds need an explicit one-pass compositional model with
//     common-context fooling pairs. The two-string row separates all three prices strictly.
//
// The guarantees are conditional exactly as the window certificate's own: completely tokenizable input. On malformed
// input the carry names nothing past the failure, and the splice hazard is the documented one, no worse. Everything is
// deterministic; every count is pinned; a drifted number fails.
//
// Usage: munch_wall_prepass, with no arguments, as the registered test runs it. It prints each verdict, carry, planning
// campaign and price tower, every refusal's reason and every failed assertion on standard output, and exits 0 when
// every assertion holds, 1 otherwise.
//
// The tables and the serial scan are wall_table, the premise, the subset graph and the decider wall_subsets, the
// synthesis wall_carry, the window walk and the planning campaign wall_planning, the price tower wall_prices, the
// pinned rows wall_rows and the refusal battery wall_refusals.

#include <cstddef>
#include <iostream>
#include <optional>
#include <vector>

#include "munch/tools/probes/assertions.hpp"
#include "munch/tools/probes/wall_carry.hpp"
#include "munch/tools/probes/wall_planning.hpp"
#include "munch/tools/probes/wall_prices.hpp"
#include "munch/tools/probes/wall_refusals.hpp"
#include "munch/tools/probes/wall_rows.hpp"
#include "munch/tools/probes/wall_subsets.hpp"
#include "munch/tools/probes/wall_table.hpp"

namespace
{
using munch::tools::probes::Assertions;
using munch::tools::probes::c_like_row;
using munch::tools::probes::Carry;
using munch::tools::probes::check_refusals;
using munch::tools::probes::check_theorem;
using munch::tools::probes::csv_row;
using munch::tools::probes::decide;
using munch::tools::probes::gadget;
using munch::tools::probes::is_init_reentrant;
using munch::tools::probes::json_strict;
using munch::tools::probes::plan;
using munch::tools::probes::price;
using munch::tools::probes::rollback_family;
using munch::tools::probes::run;
using munch::tools::probes::synthesize;
using munch::tools::probes::two_string;
using munch::tools::probes::window_walk;
using munch::tools::probes::zero_lag;

/**
 * @brief Pins the wall verdicts, state-granular lower bounds under the premise and refusals without it: the parity
 *        gadget's and the two-string gadget's absolute walls, printed as `<row>: nodes ..., sustained ..., wall floor
 *        ...`, the premise failing on the CSV row, the strict-number row at state 12 and the rollback family, and the
 *        C-like row reaching total cloud death.
 * @param assertions The probe's assertions.
 */
void check_verdicts(Assertions& assertions)
{
    const auto verdict{decide(gadget())};

    std::cout << "parity gadget: nodes " << verdict.nodes << ", sustained " << verdict.sustained << ", wall floor "
              << verdict.wall_floor << "\n";

    assertions.expect(zero_lag(gadget()).holds, "the parity gadget fails the zero-lag premise");

    assertions.expect(
            verdict.nodes == 3 && verdict.sustained == 2 && verdict.wall_floor == 2,
            "the parity gadget's absolute wall drifted from 3 nodes, sustained 2, floor 2");

    const auto two{decide(two_string())};

    std::cout << "two-string gadget: nodes " << two.nodes << ", sustained " << two.sustained << ", wall floor "
              << two.wall_floor << "\n";

    assertions.expect(zero_lag(two_string()).holds, "the two-string gadget fails the zero-lag premise");

    assertions.expect(
            two.nodes == 4 && two.sustained == 3 && two.wall_floor == 3,
            "the two-string absolute wall drifted from 4 nodes, sustained 3, floor 3");

    // Under the end-of-input-aware premise the CSV row fails: the doubled-quote continuation re-enters the string
    // interior after the closed quote accepted, so an unterminated tail rolls back with lag. Its decider verdicts are
    // approximation-scoped and refused.
    assertions.expect(!zero_lag(csv_row()).holds, "the csv row passes the premise it must fail at end of input");

    const auto conventional{decide(c_like_row())};

    assertions.expect(zero_lag(c_like_row()).holds, "the c-like row fails the zero-lag premise");

    assertions.expect(
            conventional.wall_floor == 0 && conventional.floor_start == 0,
            "the c-like row does not reach total cloud death");

    const auto strict{zero_lag(json_strict())};

    assertions.expect(
            !strict.holds && strict.witness_state == 12,
            "the strict-number row's premise refusal moved off the number state");

    assertions.expect(!zero_lag(rollback_family()).holds, "the rollback family passes the premise it must fail");
}

/**
 * @brief Pins the synthesizer's refusal where its licence does not hold: the CSV row and the strict-number row both
 *        fail the end-of-input premise, and each refusal prints its reason.
 * @param assertions The probe's assertions.
 */
void check_synthesis_refusals(Assertions& assertions)
{
    assertions.expect(
            !synthesize(csv_row()).has_value(), "the synthesizer accepted the csv row although its premise fails");

    assertions.expect(
            !synthesize(json_strict()).has_value(), "the synthesizer accepted a grammar that fails the premise");
}

/**
 * @brief Pins the two synthesized carries and the tracking theorem: the parity carry's semigroup of 4 and group of 2,
 *        printed with the two-string carry's, its byte actions quote parity, the two-string carry's semigroup of 18
 *        and non-abelian group of 6, and 80,000 tracked positions for each.
 * @param assertions The probe's assertions.
 * @param gadget_carry The parity gadget's carry.
 * @param two_carry The two-string gadget's carry.
 */
void check_carries(Assertions& assertions, const Carry& gadget_carry, const Carry& two_carry)
{
    std::cout << "parity gadget carry: semigroup " << gadget_carry.semigroup << ", group " << gadget_carry.group.size()
              << "; two-string carry: semigroup " << two_carry.semigroup << ", group " << two_carry.group.size()
              << "\n";

    assertions.expect(
            gadget_carry.semigroup == 4 && gadget_carry.group.size() == 2,
            "the parity gadget's carry drifted from semigroup 4, group 2");

    auto only_quote{true};

    for (int byte{0}; byte < 256; ++byte)
    {
        const auto identity{gadget_carry.sigma[static_cast<std::size_t>(byte)][0] == 0};

        only_quote = only_quote && (identity == (byte != '"'));
    }

    assertions.expect(only_quote, "the parity gadget's homomorphism is not quote parity");

    assertions.expect(
            two_carry.semigroup == 18 && two_carry.group.size() == 6,
            "the two-string carry drifted from semigroup 18, group 6");

    const auto& quote{two_carry.sigma[static_cast<std::size_t>('"')]};

    const auto& tick{two_carry.sigma[static_cast<std::size_t>('`')]};

    std::vector<int> quote_tick(3);

    std::vector<int> tick_quote(3);

    for (std::size_t at{0}; at < 3; ++at)
    {
        quote_tick[at] = tick[static_cast<std::size_t>(quote[at])];

        tick_quote[at] = quote[static_cast<std::size_t>(tick[at])];
    }

    assertions.expect(quote_tick != tick_quote, "the two-string carry composition commutes although it must not");

    assertions.expect(
            check_theorem(gadget(), gadget_carry, "abcdefgh\"\"\"  ") == 80000,
            "the parity gadget's tracking check drifted or mismatched");

    assertions.expect(
            check_theorem(two_string(), two_carry, "abcdef\"\"``  ") == 80000,
            "the two-string tracking check drifted or mismatched");
}

/**
 * @brief Pins the certificates behind the resolved flavor and the plans they license: the quote-letter window's
 *        origins 0 and 1 under the two flavors and its unconditional refusal, both planning campaigns at 210 clean
 *        cuts and 210 wrong-flavor refutations, each printed as `<row> planning: cuts ..., off-boundary ..., splice
 *        mismatches ..., unconditional ..., wrong-flavor bad cuts ...`, and the strict-forward plan's cuts.
 * @param assertions The probe's assertions.
 * @param gadget_carry The parity gadget's carry.
 * @param two_carry The two-string gadget's carry.
 */
void check_certificates(Assertions& assertions, const Carry& gadget_carry, const Carry& two_carry)
{
    const auto reentrant{is_init_reentrant(gadget())};

    const auto outside{window_walk(gadget(), gadget_carry, reentrant, "\"a", 0)};

    const auto inside{window_walk(gadget(), gadget_carry, reentrant, "\"a", 1)};

    assertions.expect(
            outside == std::optional<std::size_t>{0} && inside == std::optional<std::size_t>{1},
            "the flavor-dependent origins of the quote-letter window drifted");

    assertions.expect(
            !window_walk(gadget(), gadget_carry, reentrant, "\"a", -1).has_value(),
            "the wall certified a window unconditionally");

    const auto one{run(gadget(), gadget_carry, false)};

    std::cout << "parity gadget planning: cuts " << one.cuts << ", off-boundary " << one.off_boundary
              << ", splice mismatches " << one.splice_mismatches << ", unconditional " << one.unconditional
              << ", wrong-flavor bad cuts " << one.teeth_bad << "\n";

    assertions.expect(
            one.cuts == 210 && one.off_boundary == 0 && one.splice_mismatches == 0 && one.unconditional == 0 &&
                    one.teeth_bad == 210,
            "the parity gadget's planning campaign drifted from 210 clean cuts and 210 wrong-flavor refutations");

    const auto both{run(two_string(), two_carry, true)};

    std::cout << "two-string planning: cuts " << both.cuts << ", off-boundary " << both.off_boundary
              << ", splice mismatches " << both.splice_mismatches << ", unconditional " << both.unconditional
              << ", wrong-flavor bad cuts " << both.teeth_bad << "\n";

    assertions.expect(
            both.cuts == 210 && both.off_boundary == 0 && both.splice_mismatches == 0 && both.unconditional == 0 &&
                    both.teeth_bad == 210,
            "the two-string planning campaign drifted from 210 clean cuts and 210 wrong-flavor refutations");

    // Strict forward progress, pinned as a malformed-plan determinism invariant: this input's last string never closes,
    // so the serial oracle refuses it, and no completely tokenizable input is known that separates the floors, so the
    // guard is load-bearing only here, where a relaxed floor adds a sixth cut at twelve. The planner has no
    // tokenizability precondition of its own, so its malformed behavior is pinned.
    const auto forward{plan(gadget(), gadget_carry, reentrant, "\"a\"b\"c\"d\"e\"f\"", 7, false)};

    assertions.expect(
            forward.cuts == (std::vector<std::size_t>{3, 4, 7, 8, 11}),
            "the strict-forward plan drifted from 3, 4, 7, 8, 11");
}

/**
 * @brief Pins both price towers with their witness matrices: the parity gadget's 2 flavors at bits 1, 1 and 2 with 2
 *        witnesses, and the two-string gadget's 3 flavors at bits 2, 3 and 5 with 6 witnesses, separating strictly.
 * @param assertions The probe's assertions.
 * @param gadget_carry The parity gadget's carry.
 * @param two_carry The two-string gadget's carry.
 */
void check_prices(Assertions& assertions, const Carry& gadget_carry, const Carry& two_carry)
{
    const auto one{price(assertions, "parity gadget", gadget(), gadget_carry, "a\"")};

    assertions.expect(
            one.orbit == 2 && one.positional == 1 && one.compositional == 1 && one.summary == 2 && one.witnesses == 2,
            "the parity gadget's price tower drifted from 2 flavors, bits 1, 1, 2, witnesses 2");

    const auto both{price(assertions, "two-string gadget", two_string(), two_carry, "a\"`")};

    assertions.expect(
            both.orbit == 3 && both.positional == 2 && both.compositional == 3 && both.summary == 5 &&
                    both.witnesses == 6,
            "the two-string price tower drifted from 3 flavors, bits 2, 3, 5, witnesses 6");

    assertions.expect(
            both.positional < both.compositional && both.compositional < both.summary,
            "the two-string tower does not separate strictly");
}

} // namespace

/**
 * @brief Runs the pipeline end to end: the wall verdicts, the synthesis refusals, the two carries, their certificates
 *        and plans, their price towers when both synthesized, the refusal battery, and the verdict line.
 * @return 0 when every assertion holds, 1 otherwise.
 */
int main()
{
    Assertions assertions{};

    check_verdicts(assertions);

    check_synthesis_refusals(assertions);

    const auto gadget_carry{synthesize(gadget())};

    assertions.expect(gadget_carry.has_value(), "the parity gadget refused to synthesize");

    const auto two_carry{synthesize(two_string())};

    assertions.expect(two_carry.has_value(), "the two-string gadget refused to synthesize");

    if (gadget_carry && two_carry)
    {
        check_carries(assertions, *gadget_carry, *two_carry);

        check_certificates(assertions, *gadget_carry, *two_carry);

        check_prices(assertions, *gadget_carry, *two_carry);
    }

    check_refusals(assertions);

    std::cout << (assertions.has_failures() ? "assertion failures\n" : "all assertions hold\n");

    return assertions.has_failures() ? 1 : 0;
}
