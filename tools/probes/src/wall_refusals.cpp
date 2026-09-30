#include "munch/tools/probes/wall_refusals.hpp"

#include <cstddef>
#include <initializer_list>
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
// Implements wall_refusals.hpp: the fixtures' tables and one check per fixture are private to this unit.

using namespace regex;

/**
 * @brief One string type per delimiter, kinds Local 0, 1, ..., priority 1, beside a bare run of every other byte,
 *        Local::X at priority 2.
 * @param delimiters The delimiters.
 * @return The compiled table.
 */
Table string_types(const std::string& delimiters)
{
    Builder_dbg builder{};

    Set bare_set{Set::all()};

    for (const auto delimiter : delimiters)
    {
        bare_set = bare_set - Set{delimiter};
    }

    for (std::size_t at{0}; at < delimiters.size(); ++at)
    {
        const auto delimiter{delimiters[at]};

        builder.add_token(
                concat(text(std::string(1, delimiter)),
                       concat(kleene(any_of(Set::all() - Set{delimiter})), text(std::string(1, delimiter)))),
                static_cast<Local>(at), 1);
    }

    builder.add_token(plus(any_of(bare_set)), Local::X, 2);

    return extract(builder.dfa());
}

/**
 * @brief A table of rotating bare phases beside one string interior: state 0 enters the interior on `"` and the first
 *        phase on every other byte, `r` advances a phase to the next, `"` kills a phase, the interior closes on `"`
 *        into an accepting state without transitions, and the phases accept. Its kernel holds one subset of width two
 *        per phase and one for the interior.
 * @param bare The number of phases.
 * @return The table of bare + 3 states.
 */
Table rotating_phases(const std::size_t bare)
{
    constexpr std::size_t first_bare{1};

    const auto interior{bare + 1};

    const auto closed{bare + 2};

    std::vector<char> accept(bare + 3, 0);

    for (std::size_t at{0}; at < bare; ++at)
    {
        accept[first_bare + at] = 1;
    }

    accept[closed] = 1;

    return table_of(bare + 3, std::move(accept), [&](const std::size_t state, const int byte) {
        if (state == 0)
        {
            return byte == '"' ? static_cast<int>(interior) : static_cast<int>(first_bare);
        }

        if (state < interior)
        {
            return byte == '"' ? kDead :
                   byte == 'r' ? static_cast<int>(first_bare + (state - first_bare + 1) % bare) :
                                 static_cast<int>(state);
        }

        if (state == interior)
        {
            return byte == '"' ? static_cast<int>(closed) : static_cast<int>(interior);
        }

        return kDead;
    });
}

/**
 * @brief A single kernel subset rotating under one byte: state 0 enters the second member on `g` and the first on every
 *        other byte, `g` advances every member to the next, every other byte holds it, and every member accepts.
 * @param width The number of members.
 * @return The table of width + 1 states.
 */
Table rotation(const std::size_t width)
{
    std::vector<char> accept(width + 1, 1);

    accept[0] = 0;

    return table_of(width + 1, std::move(accept), [&](const std::size_t state, const int byte) {
        if (state == 0)
        {
            return byte == 'g' ? 2 : 1;
        }

        return byte == 'g' ? static_cast<int>(1 + state % width) : static_cast<int>(state);
    });
}

/**
 * @brief A table compilation could produce, every state reachable and co-accessible, where ordinary bytes hold position
 *        and one boundary byte swaps the accepting state with a non-accepting twin, so the only stale continuation
 *        crosses exactly that byte.
 * @param boundary The boundary byte.
 * @return The table of three states.
 */
Table swap_table(const int boundary)
{
    return table_of(3, {0, 1, 0}, [boundary](const std::size_t state, const int byte) {
        if (state == 2)
        {
            return byte == boundary ? 1 : 2;
        }

        return byte == boundary ? 2 : 1;
    });
}

/**
 * @brief Accepting blocks of equal width beside a non-accepting initial state, whose selector bytes shift members
 *        source-independently: byte b below blocks times width sends every member m of every block, and the initial
 *        state as member 0, to block b / width member (m + b % width) mod width, and every other byte to block 0 with
 *        no shift, so a consistent labeling exists.
 * @param blocks The number of blocks.
 * @param width The members per block.
 * @return The table of 1 + blocks * width states.
 */
Table shifting_blocks(const int blocks, const int width)
{
    const auto cell{[width](const int block, const int member) { return 1 + block * width + member; }};

    const auto states{static_cast<std::size_t>(1 + blocks * width)};

    std::vector<char> accept(states, 1);

    accept[0] = 0;

    return table_of(states, std::move(accept), [&](const std::size_t state, const int byte) {
        const auto target{byte < blocks * width ? byte / width : 0};

        const auto shift{byte < blocks * width ? byte % width : 0};

        if (state == 0)
        {
            return cell(target, shift);
        }

        const auto member{(static_cast<int>(state) - 1) % width};

        return cell(target, (member + shift) % width);
    });
}

/**
 * @brief The row of the six-block witness: state 1 + 6 i + m is block i member m, state 0 the initial state.
 * @param state The state.
 * @param byte The byte.
 * @return The target.
 */
int six_block_row(const std::size_t state, const int byte)
{
    if (byte >= 216)
    {
        return state == 0 ? kDead : static_cast<int>(state);
    }

    const auto place{[](const int block, const int member) { return 1 + block * 6 + member; }};

    const auto target{byte / 36};

    const auto refused{(byte / 6) % 6};

    const auto twist{byte % 6};

    if (state == 0)
    {
        return place(target, refused);
    }

    const auto block{(static_cast<int>(state) - 1) / 6};

    const auto image{((static_cast<int>(state) - 1) % 6 + twist * block) % 6};

    return image == refused ? kDead : place(target, image);
}

/**
 * @brief Independently killable states that realize every subset of themselves beside the initial state and an
 *        absorbing accept: state 0 enters killable state i on byte 63 + i and holds on every other byte, killable state
 *        i dies on byte i - 1, byte 200 sends every state to the absorbing one, and every other byte holds.
 * @param killable The number of killable states.
 * @return The table of killable + 2 states.
 */
Table powerset(const std::size_t killable)
{
    const auto absorbing{killable + 1};

    std::vector<char> accept(killable + 2, 0);

    accept[absorbing] = 1;

    return table_of(killable + 2, std::move(accept), [&](const std::size_t state, const int byte) {
        if (state == absorbing || byte == 200)
        {
            return static_cast<int>(absorbing);
        }

        if (state == 0)
        {
            return byte >= 64 && byte < 64 + static_cast<int>(killable) ? byte - 63 : 0;
        }

        return static_cast<std::size_t>(byte) + 1 == state ? kDead : static_cast<int>(state);
    });
}

/**
 * @brief One token over B* d B* d B*, B every byte but the delimiter 0xff at the top of the byte order, whose initial
 *        state has a live image no kernel subset holds.
 * @param assertions The probe's assertions.
 */
void check_unflavored_top_image(Assertions& assertions)
{
    // A weakened guard runs past every unflavored image without tripping the seed's own consistency check and
    // synthesizes an unsound carry, while an ASCII delimiter would merely refuse for the wrong reason.
    Builder_dbg builder{};

    const auto nond{any_of(Set::all() - Set{'\xff'})};

    builder.add_token(
            concat(kleene(nond), concat(text("\xff"), concat(kleene(nond), concat(text("\xff"), kleene(nond))))),
            Local::Str, 1);

    assertions.expect(
            !synthesize(extract(builder.dfa())).has_value(), "a live unflavored initial-state image was not refused");
}

/**
 * @brief Five string types, inside both dimensional caps, whose transfer closure is beyond any enumeration: the closure
 *        budget must refuse it promptly.
 * @param assertions The probe's assertions.
 */
void check_closure_budget(Assertions& assertions)
{
    assertions.expect(!synthesize(string_types("\"`'#$")).has_value(), "the closure budget did not refuse promptly");
}

/**
 * @brief Seven kernel subsets of width two, which the labeling search could handle if admitted: the subset-count cap
 *        must refuse them, so a relaxed cap synthesizes and fails.
 * @param assertions The probe's assertions.
 */
void check_subset_count_cap(Assertions& assertions)
{
    assertions.expect(
            !synthesize(rotating_phases(6)).has_value(), "the subset-count cap did not refuse the seven phases");
}

/**
 * @brief A single kernel subset of width seven, which the identity labeling would accept if admitted: the wall-width
 *        cap must refuse it, so a relaxed cap synthesizes and fails.
 * @param assertions The probe's assertions.
 */
void check_wall_width_cap(Assertions& assertions)
{
    assertions.expect(!synthesize(rotation(7)).has_value(), "the wall-width cap did not refuse the seven rotation");
}

/**
 * @brief The parity pair with every initial-state image redirected to the bare state: the interior loses its entry, the
 *        pair collapses and the state-granular floor falls below two, so the synthesis must refuse.
 * @param assertions The probe's assertions.
 */
void check_floor_refusal(Assertions& assertions)
{
    const auto redirected{table_of(4, {0, 1, 0, 1}, [](const std::size_t state, const int byte) {
        switch (state)
        {
        case 0:
            return 1;

        case 1:
            return byte == '"' ? kDead : 1;

        case 2:
            return byte == '"' ? 3 : 2;

        default:
            return kDead;
        }
    })};

    assertions.expect(!synthesize(redirected).has_value(), "a floor below two was not refused");
}

/**
 * @brief A table whose only stale death is the end of input, which no byte-level check sees: the premise must refuse it
 *        all the same.
 * @param assertions The probe's assertions.
 */
void check_end_of_input_rollback(Assertions& assertions)
{
    const auto rollback{table_of(3, {0, 1, 1}, [](const std::size_t state, const int byte) {
        switch (state)
        {
        case 0:
            return byte == 'a' ? 2 : 1;

        case 1:
            return byte == 'a' ? 1 : 2;

        default:
            return byte == 'a' ? 0 : kDead;
        }
    })};

    assertions.expect(!synthesize(rollback).has_value(), "an end-of-input rollback table was not refused");
}

/**
 * @brief The parity gadget beside the nullable token e*: the synthesis mirrors the shipped certificate, which refuses
 *        nullable sets wholesale.
 * @param assertions The probe's assertions.
 */
void check_nullable_synthesis(Assertions& assertions)
{
    Builder_dbg builder{};

    builder.add_token(concat(text("\""), kleene(any_of(Set::all() - Set{'"'})), text("\"")), Local::Str, 1);

    builder.add_token(plus(any_of(Set::all() - Set{'"'})), Local::Chunk, 2);

    builder.add_token(kleene(text("e")), Local::Bare, 3);

    assertions.expect(!synthesize(extract(builder.dfa())).has_value(), "a nullable token set was not refused");
}

/**
 * @brief Seven independent string types, an eight-subset, width-eight wall that passes every other licence: the
 *        subset-count cap must refuse it before any factorial labeling work, and promptly.
 * @param assertions The probe's assertions.
 */
void check_seven_string_cap(Assertions& assertions)
{
    assertions.expect(
            !synthesize(string_types("\"`'#$%&")).has_value(), "the labeling cap did not refuse the seven-string wall");
}

/**
 * @brief The parity gadget with the initial state's image at byte 255 poisoned to the interior, while the closed state
 *        absorbs that byte so no restart consults the poison and the wall stands: the synthesis refuses on the
 *        byte-dependent seed, found only at byte 255, so a seed loop that stops one byte short synthesizes an unsound
 *        carry and fails.
 * @param assertions The probe's assertions.
 */
void check_poisoned_last_byte(Assertions& assertions)
{
    const auto topmost{table_of(4, {0, 1, 0, 1}, [](const std::size_t state, const int byte) {
        switch (state)
        {
        case 0:
            return byte == 0xff || byte == '"' ? 2 : 1;

        case 1:
            return byte == '"' ? kDead : 1;

        case 2:
            return byte == '"' ? 3 : 2;

        default:
            return byte == 0xff ? 3 : kDead;
        }
    })};

    assertions.expect(!synthesize(topmost).has_value(), "a poisoned image at byte 255 was not refused");
}

/**
 * @brief The premise at both byte ends of both products: with the swap at byte 0 and at byte 255, the witness product
 *        and the synthesis must both refuse, so a product truncated at either end reports zero lag and synthesizes the
 *        width-two swap carry, failing these rows.
 * @param assertions The probe's assertions.
 */
void check_swap_at_byte_ends(Assertions& assertions)
{
    for (const auto boundary : {0, 255})
    {
        const auto table{swap_table(boundary)};

        assertions.expect(!zero_lag(table).holds, "the witness product missed the stale swap at a byte end");

        assertions.expect(!synthesize(table).has_value(), "the premise product missed the stale swap at a byte end");
    }
}

/**
 * @brief A one-token grammar whose only unflavored initial image sits at byte zero, the mirror of the poisoned image at
 *        byte 255: a guard that skips the loop's first byte synthesizes an unsound carry and fails.
 * @param assertions The probe's assertions.
 */
void check_unflavored_byte_zero(Assertions& assertions)
{
    Builder_dbg builder{};

    const auto x{Set{'\x00'}};

    const auto d{Set{'\x01'}};

    builder.add_token(
            concat(kleene(any_of(x)),
                   choice(concat(any_of(Set::all() - x - d), kleene(any_of(Set::all() - d))),
                          concat(any_of(d), concat(kleene(any_of(Set::all() - d)),
                                                   concat(any_of(d), kleene(any_of(Set::all() - d))))))),
            Local::Str, 1);

    assertions.expect(
            !synthesize(extract(builder.dfa())).has_value(), "an unflavored image at byte zero was not refused");
}

/**
 * @brief Three width-six shifting blocks, whose consistent labeling a deleted or miscounted assignments budget would
 *        synthesize: the budget must refuse them with a tiny closure.
 * @param assertions The probe's assertions.
 */
void check_assignments_budget(Assertions& assertions)
{
    assertions.expect(
            !synthesize(shifting_blocks(3, 6)).has_value(),
            "the assignments budget did not refuse the shifting blocks");
}

/**
 * @brief The assignments-budget witness: one non-accepting initial state and six accepting blocks of width six, where
 *        byte (t, r, k), b = 36 t + 6 r + k below 216, sends block i member m to block t member (m + k i) mod 6,
 *        encoded as death plus restart when the image member equals r, and every higher byte holds every member. The
 *        closure holds 217 elements, far under its budget, while the labeling search would need six-factorial to the
 *        fifth power assignments: the budget must refuse before materializing any of it, and promptly.
 * @param assertions The probe's assertions.
 */
void check_six_block_witness(Assertions& assertions)
{
    std::vector<char> accept(37, 1);

    accept[0] = 0;

    assertions.expect(
            !synthesize(table_of(37, std::move(accept), six_block_row)).has_value(),
            "the assignments budget did not refuse the six-block witness");
}

/**
 * @brief The window walk's own nullable clause: over the nullable pair, an accepting initial state beside the single
 *        token b, the unguarded cloud would certify the window b at origin zero through the rename rule, which the
 *        nullable exclusion forbids.
 * @param assertions The probe's assertions.
 */
void check_nullable_walk(Assertions& assertions)
{
    const auto pair{table_of(
            2, {1, 1}, [](const std::size_t state, const int byte) { return state == 0 && byte == 'b' ? 1 : kDead; })};

    const Carry idle{.state_flavor = std::vector<int>(2, -1)};

    assertions.expect(
            !window_walk(pair, idle, is_init_reentrant(pair), "b", -1).has_value(),
            "the walk certified a window over a nullable pair");
}

/**
 * @brief The parity gadget with the delimiter 0xff at the top of the byte order, so the swap generator lives at byte
 *        255 alone: a group ingestion truncated by one byte reports a trivial group and fails the pinned order of two.
 * @param assertions The probe's assertions.
 */
void check_topmost_generator(Assertions& assertions)
{
    Builder_dbg builder{};

    const auto nonff{any_of(Set::all() - Set{'\xff'})};

    builder.add_token(concat(text("\xff"), concat(kleene(nonff), text("\xff"))), Local::Str, 1);

    builder.add_token(plus(nonff), Local::Chunk, 2);

    const auto carry{synthesize(extract(builder.dfa()))};

    assertions.expect(
            carry.has_value() && carry->group.size() == 2, "the topmost-delimiter gadget lost its swap generator");
}

/**
 * @brief Twenty killable states, whose subset walk explodes long before any dimensional check could see it: both the
 *        synthesizer's and the decider's subset budgets must refuse promptly, where their removal would wander a
 *        million-node graph.
 * @param assertions The probe's assertions.
 */
void check_subset_budget(Assertions& assertions)
{
    const auto table{powerset(20)};

    assertions.expect(!synthesize(table).has_value(), "the subset budget did not refuse the powerset family");

    assertions.expect(decide(table).bounded, "the decider's subset budget did not report the powerset as bounded");
}

/**
 * @brief Both caps exactly at their thresholds from the admitted side: five rotating bare phases beside a string
 *        interior make six kernel subsets of width two, and a six-member rotation one subset of width six; both must
 *        synthesize, so a cap tightened to refuse its own boundary fails these rows.
 * @param assertions The probe's assertions.
 */
void check_caps_admitted(Assertions& assertions)
{
    const auto six_nodes{synthesize(rotating_phases(5))};

    assertions.expect(
            six_nodes.has_value() && six_nodes->kernel_subsets == 6 && six_nodes->group.size() == 2,
            "the six-subset boundary instance did not synthesize its parity carry");

    const auto six_wide{synthesize(rotation(6))};

    assertions.expect(
            six_wide.has_value() && six_wide->width == 6 && six_wide->group.size() == 6,
            "the width-six boundary instance did not synthesize its rotation carry");
}

/**
 * @brief The seed consistency check in its other inequality direction: this table's derivation meets the larger flavor
 *        before the smaller one, the reverse of the poisoned image, so a check weakened to one direction synthesizes
 *        here and fails.
 * @param assertions The probe's assertions.
 */
void check_descending_seed(Assertions& assertions)
{
    const auto reversed{table_of(4, {0, 0, 1, 1}, [](const std::size_t state, const int byte) {
        switch (state)
        {
        case 0:
            return byte == 0xff || byte == '"' ? 1 : 2;

        case 1:
            return byte == '"' ? 3 : 1;

        case 2:
            return byte == '"' ? kDead : 2;

        default:
            return byte == 0xff ? 3 : kDead;
        }
    })};

    assertions.expect(!synthesize(reversed).has_value(), "a descending byte-dependent seed was not refused");
}

/**
 * @brief The caps from one past their thresholds: seven rotating phases beside the string interior, and one subset of
 *        width eight, each refused, so a cap warped to refuse only its exact boundary value admits them and fails.
 * @param assertions The probe's assertions.
 */
void check_caps_one_past(Assertions& assertions)
{
    assertions.expect(!synthesize(rotating_phases(7)).has_value(), "eight subsets of width two were not refused");

    assertions.expect(!synthesize(rotation(8)).has_value(), "one subset of width eight was not refused");
}

/**
 * @brief The factorial's own lower bound: five width-four shifting blocks put the true assignment count at twenty-four
 *        to the fourth, refused, while a factorial started one term late counts twelve to the fourth and admits the
 *        search, which then synthesizes the consistent shift labeling and fails.
 * @param assertions The probe's assertions.
 */
void check_factorial_bound(Assertions& assertions)
{
    assertions.expect(
            !synthesize(shifting_blocks(5, 4)).has_value(), "the factorial undercount admitted the five quads");
}

/**
 * @brief The worst-case chain: over 512 states, byte 0 erodes the cloud by one member per step, state 0 entering the
 *        last, state 1 holding and every other state stepping down, while every other byte holds every state, so the
 *        subset graph is a chain whose floors a naive relaxation would propagate one node per pass. The single-pass
 *        propagation keeps the cost at the subset walk's, and the synthesis refuses on the floor of one.
 * @param assertions The probe's assertions.
 */
void check_eroding_chain(Assertions& assertions)
{
    constexpr std::size_t links{512};

    std::vector<char> accept(links, 1);

    accept[0] = 0;

    const auto chain{table_of(links, std::move(accept), [](const std::size_t state, const int byte) {
        if (byte != 0)
        {
            return static_cast<int>(state);
        }

        if (state == 0)
        {
            return static_cast<int>(links - 1);
        }

        return state == 1 ? 1 : static_cast<int>(state - 1);
    })};

    assertions.expect(!synthesize(chain).has_value(), "the eroding chain was not refused on its floor");
}

/**
 * @brief The decider's own threshold, apart from the synthesizer's: eight killable states give at least 256 subsets,
 *        past the decider's bound and far under the synthesizer's, so a decider bound raised to the larger constant
 *        completes unbounded and fails.
 * @param assertions The probe's assertions.
 */
void check_decider_threshold(Assertions& assertions)
{
    assertions.expect(decide(powerset(8)).bounded, "the decider's own threshold did not bound the small powerset");
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
