#include "munch/tools/audit/obstruction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/dfa/simulator.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief A state of the verifier under exploration: the DFA state of the current token's run, begun at the last
 *        boundary, and the DFA states of the runs of the tokens that ended at earlier boundaries and are still live.
 */
struct Configuration
{
    /**
     * @brief Ordered member by member, so configurations key a map.
     */
    auto operator<=>(const Configuration&) const = default;

    /**
     * @brief The DFA state of the current token's run.
     */
    std::size_t unarmed{};

    /**
     * @brief The DFA states of the earlier tokens' runs, ascending and without repeats.
     */
    std::vector<std::size_t> armed{};
};

/**
 * @brief The verifier as explored: per state, the state each marked symbol leads to, and whether the state accepts.
 *
 * A marked symbol is a byte class and whether a token boundary follows the byte, numbered twice the class plus the bit.
 */
struct Marked_automaton
{
    /**
     * @brief Per state and marked symbol, the state reached, std::nullopt where the step is refused.
     */
    std::vector<std::vector<std::optional<std::size_t>>> steps{};

    /**
     * @brief Per state, whether an input may end there.
     */
    std::vector<bool> accepting{};
};

/**
 * @brief The byte projection of the verifier's minimal automaton: per byte class and state, the states the class leads
 *        to under either mark, ascending.
 */
struct Projection
{
    /**
     * @brief The number of states.
     */
    std::size_t state_count{};

    /**
     * @brief Per byte class and state, the states reached.
     */
    std::vector<std::vector<std::vector<std::size_t>>> steps{};
};

/**
 * @brief A relation on the states of a projection, its pairs ascending and without repeats.
 */
using Relation_t = std::vector<std::pair<std::size_t, std::size_t>>;

/**
 * @brief A relation the search reached: the relation, the relation it extends and the byte class read after it.
 */
struct Reached
{
    /**
     * @brief The relation.
     */
    Relation_t relation{};

    /**
     * @brief The index of the relation it extends, zero for the identity and for the relations of one byte.
     */
    std::size_t from{};

    /**
     * @brief The byte class read after the relation it extends.
     */
    std::size_t symbol{};
};

/**
 * @brief Returns the byte a class is shown by: its first graphic member when it has one, since a reader recognises that
 *        one, else its lowest; every member steps alike, so the choice is a matter of display alone.
 * @param members The class, ascending.
 * @return The representative.
 */
[[nodiscard]] unsigned char representative(const std::vector<unsigned char>& members) noexcept
{
    const auto graphic{std::ranges::find_if(members, is_graphic)};

    return graphic == members.end() ? members.front() : *graphic;
}

/**
 * @brief The verifier of maximal munch over a compiled token set under exploration: the set, one byte of every class,
 *        the configuration each state reads as, and the configurations interned with their states.
 *
 * State zero is the unconsumed start, which reads as the configuration of one fresh run and accepts, the empty input
 * being tokenized completely; it is not interned, so a step reaching that configuration after a boundary reaches a
 * state of its own, which does not accept.
 */
class Exploration
{
public:
    /**
     * @brief Takes the set and the bytes it is read over, the unconsumed start as state zero, nothing explored yet.
     * @param simulator The compiled token set.
     * @param symbols One byte of every byte class.
     */
    Exploration(const dfa::Simulator& simulator, const std::vector<unsigned char>& symbols);

    /**
     * @brief Explores every state reachable from the unconsumed start over every marked symbol.
     * @return The verifier as explored.
     */
    [[nodiscard]] Marked_automaton run() &&;

private:
    /**
     * @brief Reads one marked symbol into a configuration.
     *
     * The byte is read into every run, an earlier token's run with no live transition dropping. The step is refused
     * where the current run has none or an earlier token's run accepts, that token then having been longer. On a
     * boundary the current run must accept, joins the earlier ones, and a fresh run begins at the initial state.
     * @param from The configuration read from.
     * @param byte The byte read.
     * @param boundary_after Whether a token boundary follows the byte.
     * @return The configuration after the symbol, or std::nullopt where the step is refused.
     */
    [[nodiscard]] std::optional<Configuration> successor(
            const Configuration& from, unsigned char byte, bool boundary_after) const;

    /**
     * @brief Returns the state of a configuration, interning it as a new state when it is first found.
     * @param configuration The configuration.
     * @return The configuration's state.
     */
    [[nodiscard]] std::size_t intern(Configuration configuration);

    /**
     * @brief Returns whether a configuration accepts: its current run accepts and no earlier token's run does.
     * @param configuration The configuration.
     * @return True when an input may end in the configuration.
     */
    [[nodiscard]] bool is_accepting(const Configuration& configuration) const;

    /**
     * @brief The compiled token set.
     */
    const dfa::Simulator& simulator_;

    /**
     * @brief The configuration each state reads as, by state.
     */
    std::vector<Configuration> sources_{};

    /**
     * @brief One byte of every byte class.
     */
    const std::vector<unsigned char>& symbols_;

    /**
     * @brief The interned configurations with their states.
     */
    std::map<Configuration, std::size_t> interned_{};
};

Exploration::Exploration(const dfa::Simulator& simulator, const std::vector<unsigned char>& symbols)
    : simulator_{simulator}, sources_{Configuration{.unarmed = simulator.init_state(), .armed = {}}}, symbols_{symbols}
{}

Marked_automaton Exploration::run() &&
{
    Marked_automaton automaton{};

    for (std::size_t state{0}; state < sources_.size(); ++state)
    {
        // The state's configuration, read from a copy while interning grows the sources.
        const auto from{sources_[state]};

        std::vector<std::optional<std::size_t>> row{};

        for (const auto byte : symbols_)
        {
            for (const auto boundary_after : {false, true})
            {
                auto next{successor(from, byte, boundary_after)};

                if (!next)
                {
                    row.emplace_back();

                    continue;
                }

                const auto to{intern(std::move(*next))};

                row.emplace_back(to);
            }
        }

        automaton.steps.push_back(std::move(row));
    }

    for (const auto& configuration : sources_)
    {
        automaton.accepting.push_back(is_accepting(configuration));
    }

    // The unconsumed start accepts the empty input.
    automaton.accepting.front() = true;

    return automaton;
}

std::optional<Configuration> Exploration::successor(
        const Configuration& from, const unsigned char byte, const bool boundary_after) const
{
    const auto& [unarmed, armed]{from};

    const auto stepped{simulator_.step(unarmed, byte)};

    if (!stepped || !simulator_.is_live(*stepped))
    {
        return std::nullopt;
    }

    Configuration next{.unarmed = *stepped, .armed = {}};

    for (const auto state : armed)
    {
        const auto moved{simulator_.step(state, byte)};

        if (!moved || !simulator_.is_live(*moved))
        {
            continue;
        }

        if (simulator_.is_accepting(*moved))
        {
            return std::nullopt;
        }

        next.armed.push_back(*moved);
    }

    if (boundary_after && !simulator_.is_accepting(next.unarmed))
    {
        return std::nullopt;
    }

    if (boundary_after)
    {
        next.armed.push_back(next.unarmed);

        next.unarmed = simulator_.init_state();
    }

    std::ranges::sort(next.armed);

    const auto repeats{std::ranges::unique(next.armed)};

    next.armed.erase(repeats.begin(), repeats.end());

    return next;
}

std::size_t Exploration::intern(Configuration configuration)
{
    const auto [entry, inserted]{interned_.try_emplace(configuration, sources_.size())};

    const auto& [key, state]{*entry};

    if (inserted)
    {
        sources_.push_back(std::move(configuration));
    }

    return state;
}

bool Exploration::is_accepting(const Configuration& configuration) const
{
    const auto& [unarmed, armed]{configuration};

    const auto accepts{[this](const std::size_t state) { return simulator_.is_accepting(state); }};

    return accepts(unarmed) && std::ranges::none_of(armed, accepts);
}

/**
 * @brief Returns which states of the explored verifier can complete, reaching an accepting state: one backward closure
 *        from the accepting states.
 * @param automaton The verifier as explored.
 * @return One flag per state.
 */
[[nodiscard]] std::vector<bool> completing_states(const Marked_automaton& automaton)
{
    const auto& [steps, accepting]{automaton};

    std::vector<std::vector<std::size_t>> predecessors(steps.size());

    for (std::size_t state{0}; state < steps.size(); ++state)
    {
        for (const auto& to : steps[state])
        {
            if (to)
            {
                predecessors[*to].push_back(state);
            }
        }
    }

    std::vector<bool> completes(steps.size(), false);

    std::deque<std::size_t> frontier{};

    for (std::size_t state{0}; state < steps.size(); ++state)
    {
        if (!accepting[state])
        {
            continue;
        }

        completes[state] = true;

        frontier.push_back(state);
    }

    while (!frontier.empty())
    {
        const auto state{frontier.front()};

        frontier.pop_front();

        for (const auto from : predecessors[state])
        {
            if (completes[from])
            {
                continue;
            }

            completes[from] = true;

            frontier.push_back(from);
        }
    }

    return completes;
}

/**
 * @brief Returns the classes of states no marked continuation tells apart, Moore's refinement from the colours that
 *        cannot complete, accepting and not accepting, over the states and one more index standing for the absent
 *        state, which shares the first colour.
 * @param automaton The verifier as explored.
 * @param completes Which states can complete.
 * @return The class of each state, and at the index of the state count the absent state's.
 */
[[nodiscard]] std::vector<std::size_t> minimal_classes(
        const Marked_automaton& automaton, const std::vector<bool>& completes)
{
    const auto& [steps, accepting]{automaton};

    const auto states{steps.size()};

    // What tells states apart before any symbol is read.
    enum class Shade : std::uint8_t
    {
        dead,
        accepting,
        open
    };

    const auto shade{[&](const std::size_t state) {
        if (state == states || !completes[state])
        {
            return Shade::dead;
        }

        return accepting[state] ? Shade::accepting : Shade::open;
    }};

    std::vector<std::size_t> current(states + 1U);

    std::map<Shade, std::size_t> first_classes{};

    for (std::size_t state{0}; state <= states; ++state)
    {
        const auto state_shade{shade(state)};

        const auto [found, inserted]{first_classes.try_emplace(state_shade, first_classes.size())};

        const auto& [known_shade, number]{*found};

        current[state] = number;
    }

    auto count{first_classes.size()};

    const auto symbol_count{steps.front().size()};

    for (;;)
    {
        std::map<std::vector<std::size_t>, std::size_t> signatures{};

        std::vector<std::size_t> next(states + 1U);

        for (std::size_t state{0}; state <= states; ++state)
        {
            std::vector<std::size_t> signature{current[state]};

            for (std::size_t symbol{0}; symbol < symbol_count; ++symbol)
            {
                const auto to{state == states ? std::nullopt : steps[state][symbol]};

                const auto target{to ? *to : states};

                signature.push_back(current[target]);
            }

            const auto fresh{signatures.size()};

            const auto [found, inserted]{signatures.try_emplace(std::move(signature), fresh)};

            const auto& [state_signature, number]{*found};

            next[state] = number;
        }

        current = std::move(next);

        // A refinement only splits classes, so an unchanged count is the fixpoint.
        if (signatures.size() == count)
        {
            return current;
        }

        count = signatures.size();
    }
}

/**
 * @brief Returns the byte projection of the verifier's minimal automaton, its states the classes of the states that can
 *        complete, numbered in the order of their least states, the unconsumed start's first.
 * @param automaton The verifier as explored.
 * @param completes Which states can complete.
 * @param classes The class of each state.
 * @return The projection.
 */
[[nodiscard]] Projection projection_of(
        const Marked_automaton& automaton, const std::vector<bool>& completes, const std::vector<std::size_t>& classes)
{
    const auto& [steps, accepting]{automaton};

    std::map<std::size_t, std::size_t> dense{};

    std::vector<std::size_t> representatives{};

    for (std::size_t state{0}; state < steps.size(); ++state)
    {
        if (!completes[state])
        {
            continue;
        }

        const auto [found, inserted]{dense.try_emplace(classes[state], representatives.size())};

        if (inserted)
        {
            representatives.push_back(state);
        }
    }

    const auto class_count{steps.front().size() / 2U};

    std::vector projected(class_count, std::vector<std::vector<std::size_t>>(representatives.size()));

    for (std::size_t symbol{0}; symbol < class_count; ++symbol)
    {
        for (std::size_t from{0}; from < representatives.size(); ++from)
        {
            auto& targets{projected[symbol][from]};

            for (const auto marked : {2U * symbol, 2U * symbol + 1U})
            {
                const auto to{steps[representatives[from]][marked]};

                if (to && completes[*to])
                {
                    targets.push_back(dense.at(classes[*to]));
                }
            }

            std::ranges::sort(targets);

            const auto repeats{std::ranges::unique(targets)};

            targets.erase(repeats.begin(), repeats.end());
        }
    }

    return {.state_count = representatives.size(), .steps = std::move(projected)};
}

/**
 * @brief Returns the identity relation on a number of states, the relation of the empty string.
 * @param count The number of states.
 * @return The identity.
 */
[[nodiscard]] Relation_t identity_of(const std::size_t count)
{
    Relation_t identity{};

    for (std::size_t state{0}; state < count; ++state)
    {
        identity.emplace_back(state, state);
    }

    return identity;
}

/**
 * @brief Returns the relation of a string followed by one more byte class: the pairs of states it joins under some
 *        marks.
 * @param projection The projection.
 * @param relation The relation of the string.
 * @param symbol The byte class.
 * @return The relation of the longer string.
 */
[[nodiscard]] Relation_t relation_after(
        const Projection& projection, const Relation_t& relation, const std::size_t symbol)
{
    Relation_t extended{};

    for (const auto& [from, through] : relation)
    {
        for (const auto to : projection.steps[symbol][through])
        {
            extended.emplace_back(from, to);
        }
    }

    std::ranges::sort(extended);

    const auto repeats{std::ranges::unique(extended)};

    extended.erase(repeats.begin(), repeats.end());

    return extended;
}

/**
 * @brief Returns the relations of the nonempty strings over a projection's byte classes, after the empty string's
 *        identity, each nonzero and with how the search reached it, or std::nullopt past a limit.
 *
 * Found breadth first from the empty string, each string extended by the classes in order and a relation kept where it
 * is first reached, so the relations follow their shortest strings, the least in class order among the shortest, and a
 * nonempty string whose relation is the identity's is kept beside it.
 * @param projection The projection.
 * @param limit The most relations of nonempty strings kept.
 * @return The identity and then the relations, in the order reached; std::nullopt when they are more than the limit.
 */
[[nodiscard]] std::optional<std::vector<Reached>> relations_of(const Projection& projection, const std::size_t limit)
{
    std::vector<Reached> reached{{.relation = identity_of(projection.state_count), .from = 0, .symbol = 0}};

    std::set<Relation_t> seen{};

    for (std::size_t next{0}; next < reached.size(); ++next)
    {
        // A copy, since the relations grow below.
        const auto [relation, from, read]{reached[next]};

        for (std::size_t symbol{0}; symbol < projection.steps.size(); ++symbol)
        {
            auto extended{relation_after(projection, relation, symbol)};

            if (extended.empty())
            {
                continue;
            }

            const auto [entry, inserted]{seen.insert(extended)};

            if (!inserted)
            {
                continue;
            }

            // The identity leads the list and is no nonempty string's.
            if (reached.size() > limit)
            {
                return std::nullopt;
            }

            reached.push_back({.relation = std::move(extended), .from = next, .symbol = symbol});
        }
    }

    return reached;
}

/**
 * @brief Returns whether a nonzero relation has rank one, a rectangle: it joins every state it reads from to every
 *        state it leads to, so that its pairs are exactly as many as those two sets' product.
 * @param relation The relation, nonzero.
 * @return True when it is a rectangle.
 */
[[nodiscard]] bool is_rank_one(const Relation_t& relation)
{
    std::set<std::size_t> sources{};

    std::set<std::size_t> targets{};

    for (const auto& [from, to] : relation)
    {
        sources.insert(from);

        targets.insert(to);
    }

    const auto product{sources.size() * targets.size()};

    return relation.size() == product;
}

/**
 * @brief Returns the shortest string of a reached relation, spelled by the byte each class is shown by.
 * @param reached The relations reached.
 * @param index The relation's index.
 * @param symbols One byte of every byte class.
 * @return The string, empty for the identity.
 */
[[nodiscard]] std::string spelled(
        const std::vector<Reached>& reached, const std::size_t index, const std::vector<unsigned char>& symbols)
{
    std::string text{};

    for (auto at{index}; at != 0;)
    {
        const auto& [relation, from, symbol]{reached[at]};

        text.push_back(static_cast<char>(symbols[symbol]));

        at = from;
    }

    std::ranges::reverse(text);

    return text;
}

/**
 * @brief Returns the composition of two relations, the pairs joined by a pair of the first and then one of the second.
 * @param first The first relation.
 * @param second The second relation.
 * @return The composition.
 */
[[nodiscard]] Relation_t composed(const Relation_t& first, const Relation_t& second)
{
    Relation_t composition{};

    for (const auto& [from, through] : first)
    {
        const auto onward{std::ranges::equal_range(second, through, {}, &std::pair<std::size_t, std::size_t>::first)};

        for (const auto& [middle, to] : onward)
        {
            composition.emplace_back(from, to);
        }
    }

    std::ranges::sort(composition);

    const auto repeats{std::ranges::unique(composition)};

    composition.erase(repeats.begin(), repeats.end());

    return composition;
}

/**
 * @brief Returns the states a relation joins to themselves, ascending.
 * @param relation The relation.
 * @return The fixed points.
 */
[[nodiscard]] std::vector<std::size_t> fixed_points(const Relation_t& relation)
{
    std::vector<std::size_t> fixed{};

    for (const auto& [from, to] : relation)
    {
        if (from == to)
        {
            fixed.push_back(from);
        }
    }

    return fixed;
}

/**
 * @brief Returns the permutation a relation induces on the fixed points of an idempotent, when it induces one: each
 *        point joined to exactly one point, and no point reached twice.
 * @param relation The relation, e m e for the idempotent e.
 * @param fixed The idempotent's fixed points, ascending.
 * @return The image of each point, as its position among the points; std::nullopt when the relation is no permutation
 *         of them.
 */
[[nodiscard]] std::optional<std::vector<std::size_t>> permutation_of(
        const Relation_t& relation, const std::vector<std::size_t>& fixed)
{
    std::vector<std::size_t> image{};

    for (const auto point : fixed)
    {
        const auto row{std::ranges::equal_range(relation, point, {}, &std::pair<std::size_t, std::size_t>::first)};

        const auto in_fixed{[&fixed](const std::pair<std::size_t, std::size_t>& pair) {
            const auto& [from, to]{pair};

            return std::ranges::binary_search(fixed, to);
        }};

        const auto joined{std::ranges::count_if(row, in_fixed)};

        if (joined != 1)
        {
            return std::nullopt;
        }

        const auto target{std::ranges::find_if(row, in_fixed)};

        const auto& [from, to]{*target};

        const auto position{std::ranges::lower_bound(fixed, to) - fixed.begin()};

        image.push_back(static_cast<std::size_t>(position));
    }

    auto sorted{image};

    std::ranges::sort(sorted);

    const auto repeats{std::ranges::adjacent_find(sorted)};

    if (repeats != sorted.end())
    {
        return std::nullopt;
    }

    return image;
}

/**
 * @brief Returns the order of a permutation, the number of times it is applied before every point returns.
 * @param permutation The image of each point.
 * @return The order, one for the identity.
 */
[[nodiscard]] std::size_t order_of(const std::vector<std::size_t>& permutation)
{
    std::vector<std::size_t> identity(permutation.size());

    std::iota(identity.begin(), identity.end(), std::size_t{0});

    auto power{permutation};

    std::size_t order{1};

    while (power != identity)
    {
        std::vector<std::size_t> next{};

        for (const auto image : power)
        {
            next.push_back(permutation[image]);
        }

        power = std::move(next);

        ++order;
    }

    return order;
}

/**
 * @brief Returns the group of the first nonzero idempotent of the fewest fixed points among the relations of nonempty
 *        strings, or std::nullopt when none is idempotent.
 *
 * The group is the permutations of the idempotent e's fixed points that the relations e m e induce over every relation
 * m reached, the identity's among them; a generator is a permutation whose order is the group's, and the one named is
 * the first m in the order reached that induces one.
 * @param reached The relations reached, the identity first.
 * @param symbols One byte of every byte class.
 * @return The group, with the idempotent's string and a generator's.
 */
[[nodiscard]] std::optional<Phase_group> phase_group(
        const std::vector<Reached>& reached, const std::vector<unsigned char>& symbols)
{
    const auto is_idempotent{[&reached](const std::size_t index) {
        const auto& [relation, from, symbol]{reached[index]};

        return composed(relation, relation) == relation;
    }};

    const auto degree_of{[&reached](const std::size_t index) {
        const auto& [relation, from, symbol]{reached[index]};

        return fixed_points(relation).size();
    }};

    auto idempotents{std::views::iota(std::size_t{1}, reached.size()) | std::views::filter(is_idempotent)};

    // The first idempotent of the least degree has the shortest string, the least in class order among the shortest.
    const auto least{std::ranges::min_element(idempotents, {}, degree_of)};

    if (least == idempotents.end())
    {
        return std::nullopt;
    }

    const auto& [idempotent, from, symbol]{reached[*least]};

    const auto fixed{fixed_points(idempotent)};

    // Each permutation with the first relation that induces it.
    std::map<std::vector<std::size_t>, std::size_t> group{};

    for (std::size_t index{0}; index < reached.size(); ++index)
    {
        const auto& [relation, parent, read]{reached[index]};

        const auto left{composed(idempotent, relation)};

        const auto sandwiched{composed(left, idempotent)};

        const auto permutation{permutation_of(sandwiched, fixed)};

        if (permutation)
        {
            group.try_emplace(*permutation, index);
        }
    }

    const auto order{group.size()};

    std::optional<std::size_t> generating{};

    for (const auto& [permutation, index] : group)
    {
        const auto generates{order > 1 && order_of(permutation) == order};

        if (generates && (!generating || index < *generating))
        {
            generating = index;
        }
    }

    const auto generator{generating ? std::optional{spelled(reached, *generating, symbols)} : std::nullopt};

    return Phase_group{
            .degree = fixed.size(),
            .idempotent = spelled(reached, *least, symbols),
            .order = order,
            .generator = generator};
}

/**
 * @brief Returns a byte string as a JSON string, each byte the code point of its value.
 * @param bytes The bytes.
 * @return The JSON text, quotes included.
 */
[[nodiscard]] std::string json_byte_string(const std::string_view bytes)
{
    std::string out{'"'};

    for (const auto byte : bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (byte == '"' || byte == '\\')
        {
            out += std::format(R"(\{})", byte);

            continue;
        }

        if (!is_printable(value))
        {
            out += std::format(R"(\u{:04x})", value);

            continue;
        }

        out += byte;
    }

    return out + '"';
}

/**
 * @brief Returns the group's members of the obstruction's JSON object: the degree, the idempotent's string, the order
 *        and the generator's string, each null where there is no group, and the generator null where none is named.
 * @param group The group, std::nullopt where no string recurs or the relations passed the limit.
 * @return The members, without the braces of their object.
 */
[[nodiscard]] std::string group_json(const std::optional<Phase_group>& group)
{
    if (!group)
    {
        return R"("degree": null, "idempotent": null, "order": null, "generator": null)";
    }

    const auto& [degree, idempotent, order, generator]{*group};

    const auto idempotent_text{json_byte_string(idempotent)};

    const auto generator_text{generator ? json_byte_string(*generator) : std::string{"null"}};

    return std::format(
            R"("degree": {}, "idempotent": {}, "order": {}, "generator": {})", degree, idempotent_text, order,
            generator_text);
}

/**
 * @brief Returns the escape a byte is written as inside the quotes a string is shown in, when it has one: a common
 *        control, the backslash, and the quote.
 * @param byte The byte.
 * @return The escape, or std::nullopt for a byte written as itself or in hex.
 */
[[nodiscard]] std::optional<std::string_view> quoted_escape(const unsigned char byte)
{
    switch (byte)
    {
    case '\n':
        return R"(\n)";
    case '\t':
        return R"(\t)";
    case '\r':
        return R"(\r)";
    case '\\':
        return R"(\\)";
    case '"':
        return R"(\")";
    default:
        break;
    }

    return std::nullopt;
}

/**
 * @brief Returns a byte string as the report prints it, each byte as its escape, as itself when it is printable, and in
 *        hex as the escape `\xHH` otherwise.
 * @param bytes The bytes.
 * @return The rendering, quoted once.
 */
[[nodiscard]] std::string shown(const std::string_view bytes)
{
    std::string out{'"'};

    for (const auto byte : bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (const auto escape{quoted_escape(value)})
        {
            out += *escape;
        }
        else if (is_printable(value))
        {
            out += byte;
        }
        else
        {
            out += std::format(R"(\x{:02X})", value);
        }
    }

    return out + '"';
}

/**
 * @brief Returns the phase a group counts, as the report's phase row says it: parity for a cyclic group of order two,
 *        a phase modulo the order for a larger cyclic one, each with the string that advances it.
 * @param group The group, of a set with no relation of rank one.
 * @return The rendering.
 */
[[nodiscard]] std::string phase_of(const Phase_group& group)
{
    const auto& [degree, idempotent, order, generator]{group};

    if (order == 1)
    {
        return "none, since the group is trivial";
    }

    if (!generator)
    {
        return std::format("not one count, since the group of order {} is not cyclic", order);
    }

    if (order == 2)
    {
        return std::format("parity, which {} flips", shown(*generator));
    }

    return std::format("a phase modulo {}, which {} advances by one", order, shown(*generator));
}

} // namespace

Obstruction obstruction(const Report& report, const core::Lexer& lexer)
{
    std::vector<unsigned char> symbols{};

    std::ranges::transform(report.classes, std::back_inserter(symbols), representative);

    const auto automaton{Exploration{lexer.simulator(), symbols}.run()};

    const auto completes{completing_states(automaton)};

    const auto classes{minimal_classes(automaton, completes)};

    const auto projection{projection_of(automaton, completes, classes)};

    const auto reached{relations_of(projection, relation_limit)};

    if (!reached)
    {
        return {.class_count = symbols.size(),
                .state_count = projection.state_count,
                .relation_count = std::nullopt,
                .rank_one = std::nullopt,
                .group = std::nullopt};
    }

    const auto is_rectangle{[&reached](const std::size_t index) {
        const auto& [relation, from, symbol]{(*reached)[index]};

        return is_rank_one(relation);
    }};

    const auto nonempty{std::views::iota(std::size_t{1}, reached->size())};

    const auto rectangle{std::ranges::find_if(nonempty, is_rectangle)};

    const auto rank_one{
            rectangle == nonempty.end() ? std::nullopt : std::optional{spelled(*reached, *rectangle, symbols)}};

    auto group{phase_group(*reached, symbols)};

    return {.class_count = symbols.size(),
            .state_count = projection.state_count,
            .relation_count = reached->size() - 1U,
            .rank_one = rank_one,
            .group = std::move(group)};
}

std::string obstruction_json(const Obstruction& found)
{
    const auto& [class_count, state_count, relation_count, rank_one, group]{found};

    const auto count{relation_count ? std::to_string(*relation_count) : std::string{"null"}};

    const auto rectangle{rank_one ? json_byte_string(*rank_one) : std::string{"null"}};

    const auto members{group_json(group)};

    return std::format(
            R"({{"states": {}, "relations": {}, "limit": {}, "rank_one": {}, {}}})", state_count, count, relation_limit,
            rectangle, members);
}

std::string obstruction_section(const Obstruction& found)
{
    const auto& [class_count, state_count, relation_count, rank_one, group]{found};

    std::string out{"\nwhy no byte certifies, read off the verifier's relation monoid, a diagnostic\n"};

    const auto row{[&out](const std::string_view label, const std::string_view value) {
        out += std::format("  {:<26} {}\n", label, value);
    }};

    if (!relation_count)
    {
        row("relations",
            std::format("not counted: more than {}, the limit, so no obstruction is named", relation_limit));

        return out;
    }

    const auto relations{std::format(
            "{} over {} byte class{}, joining {} state{}", *relation_count, class_count, class_count == 1 ? "" : "es",
            state_count, plural(state_count))};

    row("relations", relations);

    row("rank one",
        rank_one ? std::format("{}, whose relation is a nonzero rectangle", shown(*rank_one)) : std::string{"none"});

    if (!group)
    {
        row("degree", "none, since no string recurs");

        return out;
    }

    const auto& [degree, idempotent, order, generator]{*group};

    row("degree", std::format("{}, the fixed points of the idempotent of {}", degree, shown(idempotent)));

    const auto cyclic{
            generator ? std::format("cyclic of order {}, generated by {}", order, shown(*generator)) :
                        std::format("order {} on {} fixed points, not cyclic", order, degree)};

    row("group", order == 1 ? std::string{"trivial"} : cyclic);

    row("phase", rank_one ? std::string{"none, since a rank-one relation forgets what came before"} : phase_of(*group));

    return out;
}

} // namespace munch::tools::audit
