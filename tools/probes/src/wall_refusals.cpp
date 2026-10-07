#include "munch/tools/probes/wall_refusals.hpp"

#include <cstddef>
#include <initializer_list>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/probes/assertions.hpp"
#include "munch/tools/probes/builder_dbg.hpp"
#include "munch/tools/probes/wall_carry.hpp"
#include "munch/tools/probes/wall_planning.hpp"
#include "munch/tools/probes/wall_rows.hpp"
#include "munch/tools/probes/wall_subsets.hpp"
#include "munch/tools/probes/wall_table.hpp"

namespace munch::tools::probes
{
namespace
{
/**
 * @brief The blocks of the six-block witness, and the members of each.
 */
constexpr int six_blocks{6};

/**
 * @brief The selector bytes of one target block of the six-block witness: a refused member and a twist each.
 */
constexpr int six_block_selectors{six_blocks * six_blocks};

/**
 * @brief The bytes that select a target of the six-block witness, every higher byte holding every member.
 */
constexpr int six_block_selector_bytes{six_blocks * six_block_selectors};

/**
 * @brief The six-block witness's states: the initial state and every block's members.
 */
constexpr std::size_t six_block_states{1 + six_blocks * six_blocks};

/**
 * @brief Returns the state of a block's member, the initial state being state 0.
 * @param block The block.
 * @param member The member.
 * @param width The members per block.
 * @return The state.
 */
int member_state(const int block, const int member, const int width)
{
    return 1 + block * width + member;
}

/**
 * @brief Builds one string type per delimiter, kinds Local 0, 1, ..., priority 1, beside a bare run of every other
 *        byte, Local::x at priority 2.
 * @param delimiters The delimiters.
 * @return The compiled table.
 */
Table string_types(const std::string& delimiters)
{
    using namespace regex;

    Builder_dbg builder{};

    Set bare_set{Set::all()};

    for (const auto delimiter : delimiters)
    {
        bare_set = bare_set - Set{delimiter};
    }

    for (const auto& [at, delimiter] : std::views::enumerate(delimiters))
    {
        builder.add_token(delimited(delimiter), static_cast<Local>(at), 1);
    }

    builder.add_token(plus(any_of(bare_set)), Local::x, 2);

    return extract(builder.dfa());
}

/**
 * @brief Builds a table of rotating bare phases beside one string interior: state 0 enters the interior on `"` and the
 *        first phase on every other byte, `r` advances a phase to the next, `"` kills a phase, the interior closes on
 *        `"` into an accepting state without transitions, and the phases accept. Its kernel holds one subset of width
 *        two per phase and one for the interior.
 * @param bare The number of phases.
 * @return The table of bare + 3 states.
 */
Table rotating_phases(const std::size_t bare)
{
    const auto interior{bare + 1};

    const auto closed{bare + 2};

    std::vector accept(bare + 3, Flag::off);

    constexpr std::size_t first_bare{1};

    for (std::size_t at{0}; at < bare; ++at)
    {
        accept[first_bare + at] = Flag::on;
    }

    accept[closed] = Flag::on;

    const auto target_of{[&](const std::size_t state, const int byte) {
        if (state == 0)
        {
            return byte == '"' ? static_cast<int>(interior) : static_cast<int>(first_bare);
        }

        if (state < interior && byte == '"')
        {
            return dead;
        }

        if (state < interior && byte == 'r')
        {
            return static_cast<int>(first_bare + (state - first_bare + 1) % bare);
        }

        if (state < interior)
        {
            return static_cast<int>(state);
        }

        if (state == interior)
        {
            return byte == '"' ? static_cast<int>(closed) : static_cast<int>(interior);
        }

        return dead;
    }};

    return table_of(bare + 3, std::move(accept), target_of);
}

/**
 * @brief Builds a single kernel subset rotating under one byte: state 0 enters the second member on `g` and the first
 *        on every other byte, `g` advances every member to the next, every other byte holds it, and every member
 *        accepts.
 * @param width The number of members.
 * @return The table of width + 1 states.
 */
Table rotation(const std::size_t width)
{
    std::vector accept(width + 1, Flag::on);

    accept[0] = Flag::off;

    const auto target_of{[width](const std::size_t state, const int byte) {
        if (state == 0)
        {
            return byte == 'g' ? 2 : 1;
        }

        return byte == 'g' ? static_cast<int>(1 + state % width) : static_cast<int>(state);
    }};

    return table_of(width + 1, std::move(accept), target_of);
}

/**
 * @brief Builds a table compilation could produce, every state reachable and co-accessible, where ordinary bytes hold
 *        position and one boundary byte swaps the accepting state with a non-accepting twin, so the only stale
 *        continuation crosses exactly that byte.
 * @param boundary The boundary byte.
 * @return The table of three states.
 */
Table swap_table(const int boundary)
{
    const auto target_of{[boundary](const std::size_t state, const int byte) {
        if (state == 2)
        {
            return byte == boundary ? 1 : 2;
        }

        return byte == boundary ? 2 : 1;
    }};

    return table_of(3, {Flag::off, Flag::on, Flag::off}, target_of);
}

/**
 * @brief Builds accepting blocks of equal width beside a non-accepting initial state, whose selector bytes shift
 *        members source-independently: byte b below blocks times width sends every member m of every block, and the
 *        initial state as member 0, to block b / width member (m + b % width) mod width, and every other byte to block
 *        0 with no shift, so a consistent labeling exists.
 * @param blocks The number of blocks.
 * @param width The members per block.
 * @return The table of 1 + blocks * width states.
 */
Table shifting_blocks(const int blocks, const int width)
{
    const auto states{static_cast<std::size_t>(1 + blocks * width)};

    std::vector accept(states, Flag::on);

    accept[0] = Flag::off;

    const auto target_of{[&](const std::size_t state, const int byte) {
        const auto target{byte < blocks * width ? byte / width : 0};

        const auto shift{byte < blocks * width ? byte % width : 0};

        if (state == 0)
        {
            return member_state(target, shift, width);
        }

        const auto member{(static_cast<int>(state) - 1) % width};

        return member_state(target, (member + shift) % width, width);
    }};

    return table_of(states, std::move(accept), target_of);
}

/**
 * @brief Returns the row of the six-block witness: state 1 + 6 i + m is block i member m, state 0 the initial state.
 * @param state The state.
 * @param byte The byte.
 * @return The target.
 */
int six_block_row(const std::size_t state, const int byte)
{
    if (byte >= six_block_selector_bytes)
    {
        return state == 0 ? dead : static_cast<int>(state);
    }

    const auto target{byte / six_block_selectors};

    const auto refused{(byte / six_blocks) % six_blocks};

    const auto twist{byte % six_blocks};

    if (state == 0)
    {
        return member_state(target, refused, six_blocks);
    }

    const auto member{(static_cast<int>(state) - 1) % six_blocks};

    const auto block{(static_cast<int>(state) - 1) / six_blocks};

    const auto image{(member + twist * block) % six_blocks};

    return image == refused ? dead : member_state(target, image, six_blocks);
}

/**
 * @brief Builds independently killable states that realize every subset of themselves beside the initial state and an
 *        absorbing accept: state 0 enters killable state i on byte 63 + i and holds on every other byte, killable state
 *        i dies on byte i - 1, byte 200 sends every state to the absorbing one, and every other byte holds.
 * @param killable The number of killable states.
 * @return The table of killable + 2 states.
 */
Table powerset(const std::size_t killable)
{
    const auto absorbing{killable + 1};

    std::vector accept(killable + 2, Flag::off);

    accept[absorbing] = Flag::on;

    constexpr int first_entry_byte{64};

    constexpr int absorbing_byte{200};

    const auto target_of{[&](const std::size_t state, const int byte) {
        if (state == absorbing || byte == absorbing_byte)
        {
            return static_cast<int>(absorbing);
        }

        if (state == 0)
        {
            const auto entered{byte >= first_entry_byte && byte < first_entry_byte + static_cast<int>(killable)};

            return entered ? byte - first_entry_byte + 1 : 0;
        }

        return static_cast<std::size_t>(byte) + 1 == state ? dead : static_cast<int>(state);
    }};

    return table_of(killable + 2, std::move(accept), target_of);
}

/**
 * @brief Returns the class of every byte but 0xff, the body of the fixtures delimited by the topmost byte.
 * @return The regex.
 */
regex::Regex every_byte_but_ff()
{
    return regex::any_of(regex::Set::all() - regex::Set{'\xff'});
}

/**
 * @brief Returns the delimiter 0xff at the top of the byte order, as the fixtures delimited by it spell it.
 * @return The regex.
 */
regex::Regex ff_delimiter()
{
    return regex::text("\xff");
}

/**
 * @brief Checks one token over B* d B* d B*, B every byte but the delimiter 0xff at the top of the byte order, whose
 *        initial state has a live image no kernel subset holds.
 *
 * The delimiter at the top of the byte order is what the fixture pins: the guard on unflavored images must refuse the
 * table, whose unflavored images all pass the seed's own consistency check, where an ASCII delimiter would refuse for
 * another reason.
 * @param assertions The probe's assertions.
 */
void check_unflavored_top_image(Assertions& assertions)
{
    using namespace regex;

    Builder_dbg builder{};

    const auto every_byte_but_ff_run{kleene(every_byte_but_ff())};

    const auto delimiter{ff_delimiter()};

    const auto after_first{concat(every_byte_but_ff_run, delimiter, every_byte_but_ff_run)};

    builder.add_token(concat(every_byte_but_ff_run, delimiter, after_first), Local::str, 1);

    const auto table{extract(builder.dfa())};

    assertions.expect(!synthesize(table).has_value(), "a live unflavored initial-state image was not refused");
}

/**
 * @brief Checks five string types, inside both dimensional caps, whose transfer closure is beyond any enumeration: the
 *        closure budget must refuse it promptly.
 * @param assertions The probe's assertions.
 */
void check_closure_budget(Assertions& assertions)
{
    const auto table{string_types(R"("`'#$)")};

    assertions.expect(!synthesize(table).has_value(), "the closure budget did not refuse promptly");
}

/**
 * @brief Checks seven kernel subsets of width two, which the labeling search could handle if admitted, inside every
 *        other bound, so only the subset-count cap can refuse them.
 * @param assertions The probe's assertions.
 */
void check_subset_count_cap(Assertions& assertions)
{
    const auto table{rotating_phases(6)};

    assertions.expect(!synthesize(table).has_value(), "the subset-count cap did not refuse the seven phases");
}

/**
 * @brief Checks a single kernel subset of width seven, which the identity labeling would accept if admitted, inside
 *        every other bound, so only the wall-width cap can refuse it.
 * @param assertions The probe's assertions.
 */
void check_wall_width_cap(Assertions& assertions)
{
    const auto table{rotation(7)};

    assertions.expect(!synthesize(table).has_value(), "the wall-width cap did not refuse the seven rotation");
}

/**
 * @brief Checks the parity pair with every initial-state image redirected to the bare state: the interior loses its
 *        entry, the pair collapses and the state-granular floor falls below two, so the synthesis must refuse.
 * @param assertions The probe's assertions.
 */
void check_floor_refusal(Assertions& assertions)
{
    const auto target_of{[](const std::size_t state, const int byte) {
        switch (state)
        {
        case 0:
            return 1;

        case 1:
            return byte == '"' ? dead : 1;

        case 2:
            return byte == '"' ? 3 : 2;

        default:
            return dead;
        }
    }};

    const auto redirected{table_of(4, {Flag::off, Flag::on, Flag::off, Flag::on}, target_of)};

    assertions.expect(!synthesize(redirected).has_value(), "a floor below two was not refused");
}

/**
 * @brief Checks a table whose only stale death is the end of input, which no byte-level check sees: the premise must
 *        refuse it all the same.
 * @param assertions The probe's assertions.
 */
void check_end_of_input_rollback(Assertions& assertions)
{
    const auto target_of{[](const std::size_t state, const int byte) {
        switch (state)
        {
        case 0:
            return byte == 'a' ? 2 : 1;

        case 1:
            return byte == 'a' ? 1 : 2;

        default:
            return byte == 'a' ? 0 : dead;
        }
    }};

    const auto rollback{table_of(3, {Flag::off, Flag::on, Flag::on}, target_of)};

    assertions.expect(!synthesize(rollback).has_value(), "an end-of-input rollback table was not refused");
}

/**
 * @brief Checks the parity gadget beside the nullable token e*: the synthesis mirrors the shipped certificate, which
 *        refuses nullable sets wholesale.
 * @param assertions The probe's assertions.
 */
void check_nullable_synthesis(Assertions& assertions)
{
    using namespace regex;

    Builder_dbg builder{};

    add_gadget_tokens(builder);

    builder.add_token(kleene(text("e")), Local::bare, 3);

    const auto table{extract(builder.dfa())};

    assertions.expect(!synthesize(table).has_value(), "a nullable token set was not refused");
}

/**
 * @brief Checks seven independent string types, an eight-subset, width-eight wall that passes every other licence: the
 *        subset-count cap must refuse it before any factorial labeling work, and promptly.
 * @param assertions The probe's assertions.
 */
void check_seven_string_cap(Assertions& assertions)
{
    const auto table{string_types(R"("`'#$%&)")};

    assertions.expect(!synthesize(table).has_value(), "the labeling cap did not refuse the seven-string wall");
}

/**
 * @brief Checks the parity gadget with the initial state's image at byte 255 poisoned to the interior, while the closed
 *        state absorbs that byte so no restart consults the poison and the wall stands: the synthesis must refuse on
 *        the byte-dependent seed, found only at byte 255, the last byte the seed loop reads.
 * @param assertions The probe's assertions.
 */
void check_poisoned_last_byte(Assertions& assertions)
{
    const auto target_of{[](const std::size_t state, const int byte) {
        switch (state)
        {
        case 0:
            return byte == 0xFF || byte == '"' ? 2 : 1;

        case 1:
            return byte == '"' ? dead : 1;

        case 2:
            return byte == '"' ? 3 : 2;

        default:
            return byte == 0xFF ? 3 : dead;
        }
    }};

    const auto topmost{table_of(4, {Flag::off, Flag::on, Flag::off, Flag::on}, target_of)};

    assertions.expect(!synthesize(topmost).has_value(), "a poisoned image at byte 255 was not refused");
}

/**
 * @brief Checks the premise at both byte ends of both products: with the swap at byte 0 and at byte 255, the witness
 *        product and the synthesis must both refuse, each product reading the byte range to both its ends.
 * @param assertions The probe's assertions.
 */
void check_swap_at_byte_ends(Assertions& assertions)
{
    for (const auto boundary : {0x00, 0xFF})
    {
        const auto table{swap_table(boundary)};

        assertions.expect(!zero_lag(table).holds, "the witness product missed the stale swap at a byte end");

        assertions.expect(!synthesize(table).has_value(), "the premise product missed the stale swap at a byte end");
    }
}

/**
 * @brief Checks a one-token grammar whose only unflavored initial image sits at byte zero, the mirror of the poisoned
 *        image at byte 255: the guard must refuse it, its loop reading from the first byte.
 * @param assertions The probe's assertions.
 */
void check_unflavored_byte_zero(Assertions& assertions)
{
    using namespace regex;

    Builder_dbg builder{};

    const auto zero{Set{'\x00'}};

    const auto delimiter{Set{'\x01'}};

    const auto undelimited_run{kleene(any_of(Set::all() - delimiter))};

    const auto bare{concat(any_of(Set::all() - zero - delimiter), undelimited_run)};

    const auto closed{concat(any_of(delimiter), undelimited_run)};

    const auto delimited_pair{concat(any_of(delimiter), undelimited_run, closed)};

    builder.add_token(concat(kleene(any_of(zero)), choice(bare, delimited_pair)), Local::str, 1);

    const auto table{extract(builder.dfa())};

    assertions.expect(!synthesize(table).has_value(), "an unflavored image at byte zero was not refused");
}

/**
 * @brief Checks three width-six shifting blocks, which have a consistent labeling: the assignments budget must refuse
 *        them with a tiny closure.
 * @param assertions The probe's assertions.
 */
void check_assignments_budget(Assertions& assertions)
{
    const auto table{shifting_blocks(3, 6)};

    assertions.expect(!synthesize(table).has_value(), "the assignments budget did not refuse the shifting blocks");
}

/**
 * @brief Checks the assignments-budget witness: one non-accepting initial state and six accepting blocks of width six,
 *        where byte (t, r, k), b = 36 t + 6 r + k below 216, sends block i member m to block t member (m + k i) mod 6,
 *        encoded as death plus restart when the image member equals r, and every higher byte holds every member. The
 *        closure holds 217 elements, far under its budget, while the labeling search would need six-factorial to the
 *        fifth power assignments: the budget must refuse before materializing any of it, and promptly.
 * @param assertions The probe's assertions.
 */
void check_six_block_witness(Assertions& assertions)
{
    std::vector accept(six_block_states, Flag::on);

    accept[0] = Flag::off;

    const auto table{table_of(six_block_states, std::move(accept), six_block_row)};

    assertions.expect(!synthesize(table).has_value(), "the assignments budget did not refuse the six-block witness");
}

/**
 * @brief Checks the window walk's own nullable clause: over the nullable pair, an accepting initial state beside the
 *        single token b, the nullable exclusion must keep the window b from being certified at origin zero through the
 *        rename rule.
 * @param assertions The probe's assertions.
 */
void check_nullable_walk(Assertions& assertions)
{
    const auto target_of{[](const std::size_t state, const int byte) { return state == 0 && byte == 'b' ? 1 : dead; }};

    const auto pair{table_of(2, {Flag::on, Flag::on}, target_of)};

    const Carry idle{.state_flavor = std::vector<int>(2, -1)};

    const auto reentrant{is_init_reentrant(pair)};

    assertions.expect(!window_walk(pair, idle, reentrant, "b", -1), "the walk certified a window over a nullable pair");
}

/**
 * @brief Checks the parity gadget with the delimiter 0xff at the top of the byte order, so the swap generator lives at
 *        byte 255 alone: the group ingestion reads every byte through 255 and finds the pinned order of two.
 * @param assertions The probe's assertions.
 */
void check_topmost_generator(Assertions& assertions)
{
    using namespace regex;

    Builder_dbg builder{};

    const auto delimiter{ff_delimiter()};

    builder.add_token(concat(delimiter, kleene(every_byte_but_ff()), delimiter), Local::str, 1);

    builder.add_token(plus(every_byte_but_ff()), Local::chunk, 2);

    const auto table{extract(builder.dfa())};

    const auto carry{synthesize(table)};

    assertions.expect(carry && carry->group.size() == 2, "the topmost-delimiter gadget lost its swap generator");
}

/**
 * @brief Checks twenty killable states, whose subset graph grows to about a million nodes before any dimensional check
 *        could see it: both the synthesizer's and the decider's subset budgets must refuse it promptly.
 * @param assertions The probe's assertions.
 */
void check_subset_budget(Assertions& assertions)
{
    const auto table{powerset(20)};

    assertions.expect(!synthesize(table).has_value(), "the subset budget did not refuse the powerset family");

    assertions.expect(decide(table).over_budget, "the decider's subset budget did not report the powerset as bounded");
}

/**
 * @brief Checks both caps exactly at their thresholds from the admitted side: five rotating bare phases beside a string
 *        interior make six kernel subsets of width two, and a six-member rotation one subset of width six; both must
 *        synthesize, each cap admitting its own boundary value.
 * @param assertions The probe's assertions.
 */
void check_caps_admitted(Assertions& assertions)
{
    const auto five_phases{rotating_phases(5)};

    const auto six_nodes{synthesize(five_phases)};

    assertions.expect(
            six_nodes && six_nodes->kernel_subsets == 6 && six_nodes->group.size() == 2,
            "the six-subset boundary instance did not synthesize its parity carry");

    const auto six_rotation{rotation(6)};

    const auto six_wide{synthesize(six_rotation)};

    assertions.expect(
            six_wide && six_wide->width == 6 && six_wide->group.size() == 6,
            "the width-six boundary instance did not synthesize its rotation carry");
}

/**
 * @brief Checks the seed consistency check in its other inequality direction: this table's derivation meets the larger
 *        flavor before the smaller one, the reverse of the poisoned image, and the check must refuse it in this
 *        direction too.
 * @param assertions The probe's assertions.
 */
void check_descending_seed(Assertions& assertions)
{
    const auto target_of{[](const std::size_t state, const int byte) {
        switch (state)
        {
        case 0:
            return byte == 0xFF || byte == '"' ? 1 : 2;

        case 1:
            return byte == '"' ? 3 : 1;

        case 2:
            return byte == '"' ? dead : 2;

        default:
            return byte == 0xFF ? 3 : dead;
        }
    }};

    const auto reversed{table_of(4, {Flag::off, Flag::off, Flag::on, Flag::on}, target_of)};

    assertions.expect(!synthesize(reversed).has_value(), "a descending byte-dependent seed was not refused");
}

/**
 * @brief Checks the caps from one past their thresholds: seven rotating phases beside the string interior, and one
 *        subset of width eight, each of which the caps must refuse.
 * @param assertions The probe's assertions.
 */
void check_caps_one_past(Assertions& assertions)
{
    const auto seven_phases{rotating_phases(7)};

    assertions.expect(!synthesize(seven_phases).has_value(), "eight subsets of width two were not refused");

    const auto eight_rotation{rotation(8)};

    assertions.expect(!synthesize(eight_rotation).has_value(), "one subset of width eight was not refused");
}

/**
 * @brief Checks the factorial's own lower bound: five width-four shifting blocks put the true assignment count at
 *        twenty-four to the fourth, which the budget must refuse, the factorial counting from its first term.
 * @param assertions The probe's assertions.
 */
void check_factorial_bound(Assertions& assertions)
{
    const auto table{shifting_blocks(5, 4)};

    assertions.expect(!synthesize(table).has_value(), "the factorial undercount admitted the five quads");
}

/**
 * @brief Checks the worst-case chain: over 512 states, byte 0 erodes the cloud by one member per step, state 0 entering
 *        the last, state 1 holding and every other state stepping down, while every other byte holds every state, so
 *        the subset graph is a chain. The floors propagate in a single pass at the subset walk's cost, and the
 *        synthesis refuses on the floor of one.
 * @param assertions The probe's assertions.
 */
void check_eroding_chain(Assertions& assertions)
{
    constexpr std::size_t links{512};

    std::vector accept(links, Flag::on);

    accept[0] = Flag::off;

    const auto target_of{[](const std::size_t state, const int byte) {
        if (byte != 0)
        {
            return static_cast<int>(state);
        }

        if (state == 0)
        {
            return static_cast<int>(links - 1);
        }

        return state == 1 ? 1 : static_cast<int>(state - 1);
    }};

    const auto chain{table_of(links, std::move(accept), target_of)};

    assertions.expect(!synthesize(chain).has_value(), "the eroding chain was not refused on its floor");
}

/**
 * @brief Checks the decider's own threshold, apart from the synthesizer's: eight killable states give at least 256
 *        subsets, past the decider's bound and far under the synthesizer's, so the decider must refuse on its own
 *        bound.
 * @param assertions The probe's assertions.
 */
void check_decider_threshold(Assertions& assertions)
{
    const auto table{powerset(8)};

    assertions.expect(decide(table).over_budget, "the decider's own threshold did not bound the small powerset");
}

} // namespace

void check_refusals(Assertions& assertions)
{
    check_unflavored_top_image(assertions);

    check_closure_budget(assertions);

    check_subset_count_cap(assertions);

    check_wall_width_cap(assertions);

    check_floor_refusal(assertions);

    check_end_of_input_rollback(assertions);

    check_nullable_synthesis(assertions);

    check_seven_string_cap(assertions);

    check_poisoned_last_byte(assertions);

    check_swap_at_byte_ends(assertions);

    check_unflavored_byte_zero(assertions);

    check_assignments_budget(assertions);

    check_six_block_witness(assertions);

    check_nullable_walk(assertions);

    check_topmost_generator(assertions);

    check_subset_budget(assertions);

    check_caps_admitted(assertions);

    check_descending_seed(assertions);

    check_caps_one_past(assertions);

    check_factorial_bound(assertions);

    check_eroding_chain(assertions);

    check_decider_threshold(assertions);
}

} // namespace munch::tools::probes
