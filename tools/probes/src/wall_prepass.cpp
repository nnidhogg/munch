// Synthesizes carry prepasses from the compiled tables and holds every product to the serial scan.
//
// Some token sets refuse every split certificate for a structural reason: two or more readings of any factor survive
// forever, none ever eliminated, so no byte and no window can pin a boundary. This probe derives, from the tables
// alone, the machinery that resolves such walls, and pins the whole pipeline:
//
//   - the zero-lag premise, decided on a (state, seen-accept) product of the live tables, with the end of input counted
//     as a death available at every position: once a run accepts, every later state accepts, so every rollback, byte or
//     end-of-input, is zero bytes wide; grammars that fail the premise are refused with a witness, never measured
//     approximately;
//   - the wall verdict from that subset graph, state-granular and therefore a lower bound on origin-distinguished
//     readings: a floor of at least two is an absolute wall and licenses the synthesis, while a smaller floor refuses
//     without concluding the absence of an origin-level wall;
//   - the carry: on an absolute wall every byte acts on the kernel's readings as a bijection, a flavor labeling with
//     one permutation of flavors per byte is searched exactly, the permutations generate the carry group, a factor's
//     carry is the ordered product of its bytes' permutations, and the true scan's flavor after any prefix is that
//     product applied to a derived boundary seed, checked here against the real maximal-munch loop at every position of
//     a deterministic corpus;
//   - certificates behind the resolved flavor: the window decision's cloud walk started from one flavor's states alone
//     certifies where the unconditional wall never can, the same soundness argument once the carry names the true
//     reading's flavor; planning driven by prefix carries recovers every target on generated corpora, every cut on the
//     serial segmentation, every spliced boundary sequence equal to the serial one, while the unconditional walk
//     certifies zero windows across the same campaign and deliberately wrong flavors cut wrongly every time;
//   - the price tower: positional control (log2 of the seed's orbit), compositional control (log2 of the group order),
//     and the exact summary (log2 of the kernel's transfer semigroup over nonempty factors), with a hazard witness for
//     every ordered flavor pair: a concrete input whose wrong-flavor cut leaves the serial segmentation. The control
//     prices are the storage of the conditioned-cut carry; the semigroup price calibrates the stronger full-transfer
//     interface, which the cut service never needs. A scheme may pay work instead of bits (rescanning the prefix
//     realizes the same cuts), so scheme-wide bounds need an explicit one-pass compositional model with common-context
//     fooling pairs. The two-string row separates all three prices strictly.
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

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <iostream>
#include <optional>
#include <ranges>
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
using munch::tools::probes::byte_count;
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
using munch::tools::probes::run_planning;
using munch::tools::probes::synthesize;
using munch::tools::probes::Table;
using munch::tools::probes::two_string;
using munch::tools::probes::window_walk;
using munch::tools::probes::zero_lag;

/**
 * @brief The pinned rows' tables, each compiled once.
 */
struct Pinned
{
    /**
     * @brief The parity gadget.
     */
    Table gadget{};

    /**
     * @brief The two-string gadget.
     */
    Table two_string{};

    /**
     * @brief The CSV row.
     */
    Table csv{};

    /**
     * @brief The strict-number JSON row.
     */
    Table json_strict{};

    /**
     * @brief The C-like row.
     */
    Table c_like{};

    /**
     * @brief The rollback family.
     */
    Table rollback{};
};

/**
 * @brief Pins the wall verdicts, state-granular lower bounds under the premise and refusals without it: the parity
 *        gadget's and the two-string gadget's absolute walls, printed as `<row>: nodes ..., sustained ..., wall floor
 *        ...`, the premise failing on the CSV row, the strict-number row at state 12 and the rollback family, and the
 *        C-like row reaching total cloud death.
 * @param assertions The probe's assertions.
 * @param pinned The pinned rows' tables.
 */
void check_verdicts(Assertions& assertions, const Pinned& pinned)
{
    const auto [nodes, sustained, floor_start, wall_floor, over_budget]{decide(pinned.gadget)};

    std::cout << std::format("parity gadget: nodes {}, sustained {}, wall floor {}\n", nodes, sustained, wall_floor);

    assertions.expect(zero_lag(pinned.gadget).holds, "the parity gadget fails the zero-lag premise");

    assertions.expect(
            nodes == 3 && sustained == 2 && wall_floor == 2,
            "the parity gadget's absolute wall drifted from 3 nodes, sustained 2, floor 2");

    const auto [two_nodes, two_sustained, two_floor_start, two_wall_floor, two_over_budget]{decide(pinned.two_string)};

    std::cout << std::format(
            "two-string gadget: nodes {}, sustained {}, wall floor {}\n", two_nodes, two_sustained, two_wall_floor);

    assertions.expect(zero_lag(pinned.two_string).holds, "the two-string gadget fails the zero-lag premise");

    assertions.expect(
            two_nodes == 4 && two_sustained == 3 && two_wall_floor == 3,
            "the two-string absolute wall drifted from 4 nodes, sustained 3, floor 3");

    // Under the end-of-input-aware premise the CSV row fails: the doubled-quote continuation re-enters the string
    // interior after the closed quote accepted, so an unterminated tail rolls back with lag. Its decider verdicts are
    // approximation-scoped and refused.
    assertions.expect(!zero_lag(pinned.csv).holds, "the csv row passes the premise it must fail at end of input");

    const auto [c_nodes, c_sustained, c_floor_start, c_wall_floor, c_over_budget]{decide(pinned.c_like)};

    assertions.expect(zero_lag(pinned.c_like).holds, "the c-like row fails the zero-lag premise");

    assertions.expect(c_wall_floor == 0 && c_floor_start == 0, "the c-like row does not reach total cloud death");

    const auto [strict_holds, witness_state, witness_byte]{zero_lag(pinned.json_strict)};

    assertions.expect(
            !strict_holds && witness_state == 12, "the strict-number row's premise refusal moved off the number state");

    assertions.expect(!zero_lag(pinned.rollback).holds, "the rollback family passes the premise it must fail");
}

/**
 * @brief Pins the synthesizer's refusal where its licence does not hold: the CSV row and the strict-number row both
 *        fail the end-of-input premise, and each refusal prints its reason.
 * @param assertions The probe's assertions.
 * @param pinned The pinned rows' tables.
 */
void check_synthesis_refusals(Assertions& assertions, const Pinned& pinned)
{
    const auto csv_carry{synthesize(pinned.csv)};

    assertions.expect(!csv_carry.has_value(), "the synthesizer accepted the csv row although its premise fails");

    const auto strict_carry{synthesize(pinned.json_strict)};

    assertions.expect(!strict_carry.has_value(), "the synthesizer accepted a grammar that fails the premise");
}

/**
 * @brief Pins the two synthesized carries and the tracking theorem: the parity carry's semigroup of 4 and group of 2,
 *        printed with the two-string carry's, its byte actions quote parity, the two-string carry's semigroup of 18 and
 *        non-abelian group of 6, and 80,000 tracked positions for each.
 * @param assertions The probe's assertions.
 * @param pinned The pinned rows' tables.
 * @param gadget_carry The parity gadget's carry.
 * @param two_carry The two-string gadget's carry.
 */
void check_carries(Assertions& assertions, const Pinned& pinned, const Carry& gadget_carry, const Carry& two_carry)
{
    std::cout << std::format(
            "parity gadget carry: semigroup {}, group {}; two-string carry: semigroup {}, group {}\n",
            gadget_carry.semigroup, gadget_carry.group.size(), two_carry.semigroup, two_carry.group.size());

    assertions.expect(
            gadget_carry.semigroup == 4 && gadget_carry.group.size() == 2,
            "the parity gadget's carry drifted from semigroup 4, group 2");

    const auto acts_as_quote_parity{[&gadget_carry](const int byte) {
        const auto identity{gadget_carry.sigma[static_cast<std::size_t>(byte)][0] == 0};

        return identity == (byte != '"');
    }};

    const auto only_quote{std::ranges::all_of(std::views::iota(0, byte_count), acts_as_quote_parity)};

    assertions.expect(only_quote, "the parity gadget's homomorphism is not quote parity");

    assertions.expect(
            two_carry.semigroup == 18 && two_carry.group.size() == 6,
            "the two-string carry drifted from semigroup 18, group 6");

    const auto& quote{two_carry.sigma[static_cast<std::size_t>('"')]};

    const auto& tick{two_carry.sigma[static_cast<std::size_t>('`')]};

    std::vector<int> quote_tick(two_carry.width);

    std::vector<int> tick_quote(two_carry.width);

    for (std::size_t at{0}; at < two_carry.width; ++at)
    {
        quote_tick[at] = tick[static_cast<std::size_t>(quote[at])];

        tick_quote[at] = quote[static_cast<std::size_t>(tick[at])];
    }

    assertions.expect(quote_tick != tick_quote, "the two-string carry composition commutes although it must not");

    const auto gadget_tracked{check_theorem(pinned.gadget, gadget_carry, R"(abcdefgh"""  )")};

    assertions.expect(gadget_tracked == 80'000, "the parity gadget's tracking check drifted or mismatched");

    const auto two_tracked{check_theorem(pinned.two_string, two_carry, R"(abcdef""``  )")};

    assertions.expect(two_tracked == 80'000, "the two-string tracking check drifted or mismatched");
}

/**
 * @brief Pins the certificates behind the resolved flavor and the plans they license: the quote-letter window's origins
 *        0 and 1 under the two flavors and its unconditional refusal, both planning campaigns at 210 clean cuts and 210
 *        wrong-flavor refutations, each printed as `<row> planning: cuts ..., off-boundary ..., splice mismatches ...,
 *        unconditional ..., wrong-flavor bad cuts ...`, and the strict-forward plan's cuts.
 * @param assertions The probe's assertions.
 * @param pinned The pinned rows' tables.
 * @param gadget_carry The parity gadget's carry.
 * @param two_carry The two-string gadget's carry.
 */
void check_certificates(Assertions& assertions, const Pinned& pinned, const Carry& gadget_carry, const Carry& two_carry)
{
    const auto& gadget_table{pinned.gadget};

    const auto& two_table{pinned.two_string};

    const auto reentrant{is_init_reentrant(gadget_table)};

    const auto outside{window_walk(gadget_table, gadget_carry, reentrant, R"("a)", 0)};

    const auto inside{window_walk(gadget_table, gadget_carry, reentrant, R"("a)", 1)};

    assertions.expect(
            outside == std::optional<std::size_t>{0} && inside == std::optional<std::size_t>{1},
            "the flavor-dependent origins of the quote-letter window drifted");

    const auto unconditional_origin{window_walk(gadget_table, gadget_carry, reentrant, R"("a)", -1)};

    assertions.expect(!unconditional_origin.has_value(), "the wall certified a window unconditionally");

    constexpr auto double_quoted_only{false};

    const auto [cuts, off_boundary, splice_mismatches, unconditional, wrong_flavor_off_boundary]{
            run_planning(gadget_table, gadget_carry, double_quoted_only)};

    std::cout << std::format(
            "parity gadget planning: cuts {}, off-boundary {}, splice mismatches {}, unconditional {}, wrong-flavor "
            "bad cuts {}\n",
            cuts, off_boundary, splice_mismatches, unconditional, wrong_flavor_off_boundary);

    assertions.expect(
            cuts == 210 && off_boundary == 0 && splice_mismatches == 0 && unconditional == 0 &&
                    wrong_flavor_off_boundary == 210,
            "the parity gadget's planning campaign drifted from 210 clean cuts and 210 wrong-flavor refutations");

    constexpr auto with_ticks{true};

    const auto [two_cuts, two_off_boundary, two_splice_mismatches, two_unconditional, two_wrong_flavor_off_boundary]{
            run_planning(two_table, two_carry, with_ticks)};

    std::cout << std::format(
            "two-string planning: cuts {}, off-boundary {}, splice mismatches {}, unconditional {}, wrong-flavor bad "
            "cuts {}\n",
            two_cuts, two_off_boundary, two_splice_mismatches, two_unconditional, two_wrong_flavor_off_boundary);

    assertions.expect(
            two_cuts == 210 && two_off_boundary == 0 && two_splice_mismatches == 0 && two_unconditional == 0 &&
                    two_wrong_flavor_off_boundary == 210,
            "the two-string planning campaign drifted from 210 clean cuts and 210 wrong-flavor refutations");

    constexpr std::size_t forward_chunks{7};

    constexpr auto right_flavors{false};

    // Strict forward progress on malformed input, which the planner does not refuse: the last string never closes, so
    // the serial oracle refuses the input, and the strict-forward plan keeps exactly the five cuts 3, 4, 7, 8 and 11.
    // No completely tokenizable input is known to separate the strict forward floor from a relaxed one.
    const auto [forward_cuts, forward_unconditional]{
            plan(gadget_table, gadget_carry, reentrant, R"("a"b"c"d"e"f")", forward_chunks, right_flavors)};

    assertions.expect(
            forward_cuts == (std::vector<std::size_t>{3, 4, 7, 8, 11}),
            "the strict-forward plan drifted from 3, 4, 7, 8, 11");
}

/**
 * @brief Pins both price towers with their witness matrices: the parity gadget's 2 flavors at bits 1, 1 and 2 with 2
 *        witnesses, and the two-string gadget's 3 flavors at bits 2, 3 and 5 with 6 witnesses, separating strictly.
 * @param assertions The probe's assertions.
 * @param pinned The pinned rows' tables.
 * @param gadget_carry The parity gadget's carry.
 * @param two_carry The two-string gadget's carry.
 */
void check_prices(Assertions& assertions, const Pinned& pinned, const Carry& gadget_carry, const Carry& two_carry)
{
    const auto [orbit, positional, compositional, summary, witnesses]{
            price(assertions, "parity gadget", pinned.gadget, gadget_carry, R"(a")")};

    assertions.expect(
            orbit == 2 && positional == 1 && compositional == 1 && summary == 2 && witnesses == 2,
            "the parity gadget's price tower drifted from 2 flavors, bits 1, 1, 2, witnesses 2");

    const auto [two_orbit, two_positional, two_compositional, two_summary, two_witnesses]{
            price(assertions, "two-string gadget", pinned.two_string, two_carry, R"(a"`)")};

    assertions.expect(
            two_orbit == 3 && two_positional == 2 && two_compositional == 3 && two_summary == 5 && two_witnesses == 6,
            "the two-string price tower drifted from 3 flavors, bits 2, 3, 5, witnesses 6");

    assertions.expect(
            two_positional < two_compositional && two_compositional < two_summary,
            "the two-string tower does not separate strictly");
}

} // namespace

/**
 * @brief Runs the pipeline end to end: the wall verdicts, the synthesis refusals, the two carries, their certificates
 *        and plans, their price towers when both synthesized, the refusal battery, and the verdict line.
 * @return EXIT_SUCCESS when every assertion holds, EXIT_FAILURE otherwise.
 */
int main()
{
    Assertions assertions{};

    const Pinned pinned{
            .gadget = gadget(),
            .two_string = two_string(),
            .csv = csv_row(),
            .json_strict = json_strict(),
            .c_like = c_like_row(),
            .rollback = rollback_family()};

    check_verdicts(assertions, pinned);

    check_synthesis_refusals(assertions, pinned);

    const auto gadget_carry{synthesize(pinned.gadget)};

    assertions.expect(gadget_carry.has_value(), "the parity gadget refused to synthesize");

    const auto two_carry{synthesize(pinned.two_string)};

    assertions.expect(two_carry.has_value(), "the two-string gadget refused to synthesize");

    if (gadget_carry && two_carry)
    {
        check_carries(assertions, pinned, *gadget_carry, *two_carry);

        check_certificates(assertions, pinned, *gadget_carry, *two_carry);

        check_prices(assertions, pinned, *gadget_carry, *two_carry);
    }

    check_refusals(assertions);

    std::cout << (assertions.has_failures() ? "assertion failures\n" : "all assertions hold\n");

    return assertions.has_failures() ? EXIT_FAILURE : EXIT_SUCCESS;
}
