#include "munch/verifier/k_copy_scan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/verifier/verifier.hpp"
#include "munch/verifier/verifier_decisions.hpp"

namespace munch::verifier
{
namespace
{
/**
 * @brief A verifier state, as the decisions name one.
 */
using State_t = Verifier::State_t;

/**
 * @brief A set of states of a verifier's minimal automaton, one bit per class.
 */
using Class_set_t = std::vector<bool>;

/**
 * @brief The two boundary bits a byte can carry, no boundary first.
 */
constexpr std::array marks{false, true};

/**
 * @brief The byte projection of a verifier's minimal automaton: the class of each verifier state, the classes being the
 *        states no marked continuation tells apart, the least state of each class, the accepting classes, and per byte
 *        the steps from each class.
 *
 * The verifier is trim, so every class can complete but the start's of an empty domain, which takes no step.
 */
struct Projection
{
    /**
     * @brief The bytes on the verifier's transitions, ascending.
     */
    std::vector<unsigned char> alphabet{};

    /**
     * @brief Per verifier state, its class, the classes numbered in the order of their least states.
     */
    std::vector<std::size_t> class_of{};

    /**
     * @brief Per class, its least verifier state.
     */
    std::vector<State_t> representatives{};

    /**
     * @brief Per class, whether it accepts.
     */
    Class_set_t accepting{};

    /**
     * @brief Per byte of the alphabet and class, the steps the byte takes from it as the boundary bit and the class
     *        entered, no boundary first.
     */
    std::vector<std::vector<std::vector<std::pair<bool, std::size_t>>>> steps{};
};

/**
 * @brief A member of a family of state sets: the set, and the least of the shortest byte strings that give it.
 */
struct Family_member
{
    /**
     * @brief The set.
     */
    Class_set_t states{};

    /**
     * @brief The least of the shortest byte strings that give the set.
     */
    std::string word{};
};

/**
 * @brief A state a copy reaches on a byte of its chunk: the state, the index of the state it was read from in the layer
 *        of the byte before, and the boundary bit the byte was read under.
 */
struct Reached
{
    /**
     * @brief The state reached.
     */
    std::size_t state{};

    /**
     * @brief The index of the state it was read from in the layer before.
     */
    std::size_t from{};

    /**
     * @brief The boundary bit the byte was read under.
     */
    bool boundary_after{};
};

/**
 * @brief An exit a copy offers: the state it reaches at the chunk's end and the marking of the chunk that reaches it.
 */
struct Exit
{
    /**
     * @brief The state reached at the chunk's end.
     */
    std::size_t state{};

    /**
     * @brief The marking of the chunk, one bit per byte.
     */
    std::vector<bool> boundaries{};
};

/**
 * @brief A chunk as its worker reads it: its bytes, the configurations of the cut it begins at, the states it may exit
 *        in, the bytes past it it reads, and whether those run to the input's end.
 */
struct Worker_chunk
{
    /**
     * @brief The chunk's bytes.
     */
    std::string_view bytes{};

    /**
     * @brief The configurations of the cut the chunk begins at, the start alone for the first chunk.
     */
    Class_set_t entries{};

    /**
     * @brief The states an exit may reach: the configurations of the cut ending the chunk, the accepting states for the
     *        last chunk.
     */
    Class_set_t exits{};

    /**
     * @brief The bytes the worker reads past the chunk, empty for the last chunk.
     */
    std::string_view lookahead{};

    /**
     * @brief Whether the lookahead runs to the input's end, so that an exit must reach acceptance on it.
     */
    bool to_end{};
};

/**
 * @brief A chain the stitch keeps at a cut: the state it is at, the index of the chain it extends at the cut before,
 *        and the marking of the chunk between.
 */
struct Candidate
{
    /**
     * @brief The state at the cut.
     */
    std::size_t state{};

    /**
     * @brief The index of the chain it extends at the cut before, zero at the start.
     */
    std::size_t parent{};

    /**
     * @brief The marking of the chunk between the two cuts.
     */
    std::vector<bool> boundaries{};
};

/**
 * @brief Per chunk and state, the exits the copy beginning at that state offers, none for a state that is no entry.
 */
using Tables_t = std::vector<std::vector<std::vector<Exit>>>;

/**
 * @brief Per cut, from the start to the input's end, the chains the stitch keeps there.
 */
using Levels_t = std::vector<std::vector<Candidate>>;

/**
 * @brief Sorts a vector and removes its repeated values.
 * @tparam T The element type, ordered.
 * @param values The vector.
 */
template <typename T>
void dedup(std::vector<T>& values)
{
    std::ranges::sort(values);

    const auto repeats{std::ranges::unique(values)};

    values.erase(repeats.begin(), repeats.end());
}

/**
 * @brief Returns the bytes on a verifier's transitions, ascending and without repeats.
 * @param verifier The verifier.
 * @return The alphabet.
 */
[[nodiscard]] std::vector<unsigned char> alphabet_of(const Verifier& verifier)
{
    std::vector<unsigned char> alphabet{};

    for (const auto& [from, symbol] : verifier.transitions() | std::views::keys)
    {
        alphabet.push_back(symbol.byte);
    }

    dedup(alphabet);

    return alphabet;
}

/**
 * @brief Returns the classes of a verifier's states under Moore refinement: the states are told apart by acceptance and
 *        then by the class each marked symbol leads to or its refusal, until no class splits.
 * @param verifier The verifier, trim.
 * @param alphabet The bytes on its transitions.
 * @return Per state, its class, the classes numbered in the order of their least states.
 */
[[nodiscard]] std::vector<std::size_t> classes_of(const Verifier& verifier, const std::vector<unsigned char>& alphabet)
{
    const auto count{verifier.state_count()};

    const auto numbered{[count]<typename Signature>(const Signature& signature_of) {
        std::map<std::vector<std::size_t>, std::size_t> numbers{};

        std::vector<std::size_t> classes(count, 0);

        for (State_t state{0}; state < count; ++state)
        {
            auto signature{signature_of(state)};

            const auto [entry, inserted]{numbers.try_emplace(std::move(signature), numbers.size())};

            const auto& [interned, number]{*entry};

            classes[state] = number;
        }

        return std::pair{classes, numbers.size()};
    }};

    const auto acceptance{[&verifier](const State_t state) {
        return std::vector{verifier.accepts(state) ? std::size_t{1} : std::size_t{0}};
    }};

    auto [classes, class_count]{numbered(acceptance)};

    // A refused step is told apart from a step into any class.
    const auto refined_signature{[&](const State_t state) {
        std::vector signature{classes[state]};

        for (const auto byte : alphabet)
        {
            for (const auto boundary_after : marks)
            {
                const auto to{verifier.step(state, {.byte = byte, .boundary_after = boundary_after})};

                const auto entered{to ? classes[*to] + 1 : 0};

                signature.push_back(entered);
            }
        }

        return signature;
    }};

    for (;;)
    {
        auto [refined, refined_count]{numbered(refined_signature)};

        if (refined_count == class_count)
        {
            return refined;
        }

        classes = std::move(refined);
        class_count = refined_count;
    }
}

/**
 * @brief Returns the byte projection of a verifier's minimal automaton.
 * @param verifier The verifier, trim.
 * @return The projection.
 */
[[nodiscard]] Projection projection_of(const Verifier& verifier)
{
    auto alphabet{alphabet_of(verifier)};

    auto class_of{classes_of(verifier, alphabet)};

    std::vector<State_t> representatives{};

    Class_set_t accepting{};

    for (State_t state{0}; state < verifier.state_count(); ++state)
    {
        if (class_of[state] == representatives.size())
        {
            representatives.push_back(state);
            accepting.push_back(verifier.accepts(state));
        }
    }

    const auto steps_from{[&](const unsigned char byte, const State_t state) {
        std::vector<std::pair<bool, std::size_t>> steps{};

        for (const auto boundary_after : marks)
        {
            if (const auto to{verifier.step(state, {.byte = byte, .boundary_after = boundary_after})})
            {
                steps.emplace_back(boundary_after, class_of[*to]);
            }
        }

        return steps;
    }};

    std::vector<std::vector<std::vector<std::pair<bool, std::size_t>>>> steps{};

    for (const auto byte : alphabet)
    {
        auto& row{steps.emplace_back()};

        for (const auto representative : representatives)
        {
            row.push_back(steps_from(byte, representative));
        }
    }

    return {.alphabet = std::move(alphabet),
            .class_of = std::move(class_of),
            .representatives = std::move(representatives),
            .accepting = std::move(accepting),
            .steps = std::move(steps)};
}

/**
 * @brief Returns whether a set of states holds a state.
 * @param states The set.
 * @return True if some state is in the set.
 */
[[nodiscard]] bool is_nonempty(const Class_set_t& states)
{
    return std::ranges::contains(states, true);
}

/**
 * @brief Returns the states a byte leads to under some mark from a set of states of a projection.
 * @param projection The projection.
 * @param states The states read from.
 * @param index The byte's index in the projection's alphabet.
 * @return The states reached.
 */
[[nodiscard]] Class_set_t image_by(const Projection& projection, const Class_set_t& states, const std::size_t index)
{
    Class_set_t image(states.size(), false);

    for (std::size_t from{0}; from < states.size(); ++from)
    {
        if (!states[from])
        {
            continue;
        }

        for (const auto to : projection.steps[index][from] | std::views::values)
        {
            image[to] = true;
        }
    }

    return image;
}

/**
 * @brief Returns the forward images of the nonempty byte strings, each from every state of a projection, with the least
 *        of the shortest strings that give each, by breadth-first search appending one byte at a time.
 *
 * Within a length the members are found in the order of their strings, since each extends a member of the length
 * before, themselves in that order, by the bytes in ascending order, so the first string found for a set is its least.
 * @param projection The projection.
 * @return The nonempty images, by length and then by string.
 */
[[nodiscard]] std::vector<Family_member> forward_family(const Projection& projection)
{
    const Class_set_t every_class(projection.representatives.size(), true);

    std::set<Class_set_t> seen{};

    std::vector<Family_member> members{};

    std::vector<Family_member> level{{.states = every_class, .word = {}}};

    while (!level.empty())
    {
        std::vector<Family_member> next{};

        for (const auto& [states, word] : level)
        {
            for (std::size_t index{0}; index < projection.alphabet.size(); ++index)
            {
                auto image{image_by(projection, states, index)};

                if (!is_nonempty(image) || !seen.insert(image).second)
                {
                    continue;
                }

                auto extended{word + static_cast<char>(projection.alphabet[index])};

                next.push_back({.states = std::move(image), .word = std::move(extended)});
            }
        }

        members.insert(members.end(), next.cbegin(), next.cend());

        level = std::move(next);
    }

    return members;
}

/**
 * @brief Returns the states some byte enters under some mark from a state of a projection.
 * @param projection The projection.
 * @return The entered states.
 */
[[nodiscard]] Class_set_t entered_classes(const Projection& projection)
{
    Class_set_t entered(projection.representatives.size(), false);

    for (const auto to : projection.steps | std::views::join | std::views::join | std::views::values)
    {
        entered[to] = true;
    }

    return entered;
}

/**
 * @brief Returns the states from which a byte leads under some mark into a set of states of a projection.
 * @param projection The projection.
 * @param states The states led into.
 * @param index The byte's index in the projection's alphabet.
 * @return The states read from.
 */
[[nodiscard]] Class_set_t preimage_by(const Projection& projection, const Class_set_t& states, const std::size_t index)
{
    Class_set_t preimage(states.size(), false);

    const auto into_states{[&states](const std::size_t to) { return states[to]; }};

    for (std::size_t from{0}; from < states.size(); ++from)
    {
        preimage[from] = std::ranges::any_of(projection.steps[index][from] | std::views::values, into_states);
    }

    return preimage;
}

/**
 * @brief Returns the backward domains of the nonempty byte strings, the states of a projection from which each can be
 *        read under some marks, with the least of the shortest strings that give each, by breadth-first search
 *        prepending one byte at a time.
 *
 * Within a length the members are found in the order of their strings, since each prepends the bytes in ascending order
 * and, for each byte, extends the members of the length before in their order, so the first string found for a set is
 * its least.
 * @param projection The projection.
 * @return The nonempty domains, by length and then by string.
 */
[[nodiscard]] std::vector<Family_member> backward_family(const Projection& projection)
{
    const Class_set_t every_class(projection.representatives.size(), true);

    std::set<Class_set_t> seen{};

    std::vector<Family_member> members{};

    std::vector<Family_member> level{{.states = every_class, .word = {}}};

    while (!level.empty())
    {
        std::vector<Family_member> next{};

        for (std::size_t index{0}; index < projection.alphabet.size(); ++index)
        {
            for (const auto& [states, word] : level)
            {
                auto preimage{preimage_by(projection, states, index)};

                if (!is_nonempty(preimage) || !seen.insert(preimage).second)
                {
                    continue;
                }

                auto extended{static_cast<char>(projection.alphabet[index]) + word};

                next.push_back({.states = std::move(preimage), .word = std::move(extended)});
            }
        }

        members.insert(members.end(), next.cbegin(), next.cend());

        level = std::move(next);
    }

    return members;
}

/**
 * @brief Returns the number of states two sets of states share.
 * @param first The first set.
 * @param second The second set.
 * @return The size of their intersection.
 */
[[nodiscard]] std::size_t shared_count(const Class_set_t& first, const Class_set_t& second)
{
    std::size_t count{0};

    for (std::size_t state{0}; state < first.size(); ++state)
    {
        count += first[state] && second[state] ? 1 : 0;
    }

    return count;
}

/**
 * @brief Returns the index of a byte in a projection's alphabet.
 * @param projection The projection.
 * @param byte The byte.
 * @return The index, or std::nullopt when no transition carries the byte.
 */
[[nodiscard]] std::optional<std::size_t> byte_index(const Projection& projection, const unsigned char byte)
{
    const auto found{std::ranges::lower_bound(projection.alphabet, byte)};

    if (found == projection.alphabet.cend() || *found != byte)
    {
        return std::nullopt;
    }

    const auto index{std::ranges::distance(projection.alphabet.cbegin(), found)};

    return static_cast<std::size_t>(index);
}

/**
 * @brief Returns the state a byte leads to from a state of a projection under a mark, the refusal being the state one
 *        past the last.
 * @param projection The projection.
 * @param state The state, or the refusal.
 * @param index The byte's index in the projection's alphabet.
 * @param boundary_after The mark.
 * @return The state reached, or the refusal.
 */
[[nodiscard]] std::size_t stepped(
        const Projection& projection, const std::size_t state, const std::size_t index, const bool boundary_after)
{
    const auto refusal{projection.representatives.size()};

    if (state == refusal)
    {
        return refusal;
    }

    for (const auto& [mark, to] : projection.steps[index][state])
    {
        if (mark == boundary_after)
        {
            return to;
        }
    }

    return refusal;
}

/**
 * @brief Returns whether some marking of a byte string leads two states of a projection to different states, a refusal
 *        counted as a state.
 * @param projection The projection.
 * @param first The first state.
 * @param second The second state.
 * @param bytes The byte string.
 * @return True if a marking tells the two apart.
 */
[[nodiscard]] bool told_apart(
        const Projection& projection, const std::size_t first, const std::size_t second, const std::string_view bytes)
{
    const auto refusal{projection.representatives.size()};

    std::set<std::pair<std::size_t, std::size_t>> pairs{{first, second}};

    for (const auto byte : bytes)
    {
        const auto index{byte_index(projection, static_cast<unsigned char>(byte))};

        // No marking reads the byte, so every one refuses both.
        if (!index)
        {
            return false;
        }

        std::set<std::pair<std::size_t, std::size_t>> next{};

        for (const auto& [left, right] : pairs)
        {
            for (const auto boundary_after : marks)
            {
                const auto left_to{stepped(projection, left, *index, boundary_after)};

                const auto right_to{stepped(projection, right, *index, boundary_after)};

                // Both refused, the two agree under every longer marking.
                if (left_to == refusal && right_to == refusal)
                {
                    continue;
                }

                next.emplace(left_to, right_to);
            }
        }

        pairs = std::move(next);
    }

    const auto differ{[](const std::pair<std::size_t, std::size_t>& pair) { return pair.first != pair.second; }};

    return std::ranges::any_of(pairs, differ);
}

/**
 * @brief Returns the configurations a pair's cut can carry, as states of a projection, by fanout().
 * @param verifier The verifier.
 * @param projection The projection of its minimal automaton.
 * @param pair The pair.
 * @return The configurations.
 */
[[nodiscard]] Class_set_t configurations_of(
        const Verifier& verifier, const Projection& projection, const Fanout_pair& pair)
{
    const auto [rank, configurations]{fanout(verifier, pair.window, pair.origin)};

    Class_set_t states(projection.representatives.size(), false);

    for (const auto configuration : configurations)
    {
        states[projection.class_of[configuration]] = true;
    }

    return states;
}

/**
 * @brief Returns the anchors of an inventory in an input, each with its configurations, the intersection of those of
 *        the pairs anchoring it.
 * @param verifier The verifier.
 * @param projection The projection of its minimal automaton.
 * @param input The input.
 * @param inventory The pairs.
 * @return Per anchor, ascending, its configurations.
 */
[[nodiscard]] std::map<std::size_t, Class_set_t> anchors_of(
        const Verifier& verifier, const Projection& projection, const std::string_view input,
        const std::span<const Fanout_pair> inventory)
{
    std::map<std::size_t, Class_set_t> anchors{};

    for (const auto& pair : inventory)
    {
        const auto configurations{configurations_of(verifier, projection, pair)};

        for (auto at{input.find(pair.window)}; at != std::string_view::npos; at = input.find(pair.window, at + 1))
        {
            const auto cut{at + pair.origin};

            if (cut == 0)
            {
                continue;
            }

            const auto [entry, inserted]{anchors.try_emplace(cut, configurations)};

            auto& [position, states]{*entry};

            std::ranges::transform(states, configurations, states.begin(), std::logical_and{});
        }
    }

    return anchors;
}

/**
 * @brief Returns the cuts of a scan: each interior cut the first anchor at or after the later of its equal-division
 *        target and one past the previous cut, the search ending at a target with no anchor at or after it.
 * @param anchors The anchors, ascending.
 * @param size The input's size.
 * @param chunks The chunks aimed for, zero behaving as one.
 * @return The cut positions, from zero to the input's size inclusive.
 */
[[nodiscard]] std::vector<std::size_t> cuts_of(
        const std::map<std::size_t, Class_set_t>& anchors, const std::size_t size, const std::size_t chunks)
{
    // A chunk needs at least one byte, so asking for more chunks than bytes adds targets that can find nothing.
    const auto usable{std::min(chunks, size)};

    std::vector<std::size_t> cuts{0};

    for (std::size_t index{1}; index < usable; ++index)
    {
        const auto target{index * (size / usable) + index * (size % usable) / usable};

        const auto floor{std::max(target, cuts.back() + 1)};

        const auto found{anchors.lower_bound(floor)};

        // Every later floor lies at or after this one, so no later target finds an anchor either.
        if (found == anchors.cend())
        {
            break;
        }

        cuts.push_back(found->first);
    }

    cuts.push_back(size);

    return cuts;
}

/**
 * @brief Returns the chunks of a scan as their workers read them.
 * @param projection The projection of the verifier's minimal automaton.
 * @param input The input.
 * @param anchors The anchors with their configurations.
 * @param cuts The cut positions, from zero to the input's size inclusive.
 * @param start The start's state in the projection.
 * @param lookahead The bytes a worker reads past its chunk.
 * @return The chunks in input order.
 */
[[nodiscard]] std::vector<Worker_chunk> worker_chunks(
        const Projection& projection, const std::string_view input, const std::map<std::size_t, Class_set_t>& anchors,
        const std::vector<std::size_t>& cuts, const std::size_t start, const std::size_t lookahead)
{
    Class_set_t start_alone(projection.representatives.size(), false);

    start_alone[start] = true;

    std::vector<Worker_chunk> chunks{};

    for (std::size_t chunk{0}; chunk + 1 < cuts.size(); ++chunk)
    {
        const auto begin{cuts[chunk]};

        const auto end{cuts[chunk + 1]};

        const auto last{end == input.size()};

        const auto bytes{input.substr(begin, end - begin)};

        auto entries{chunk == 0 ? start_alone : anchors.at(begin)};

        auto exits{last ? projection.accepting : anchors.at(end)};

        const auto read{last ? std::string_view{} : input.substr(end, lookahead)};

        const auto to_end{!last && end + lookahead >= input.size()};

        chunks.push_back(
                {.bytes = bytes,
                 .entries = std::move(entries),
                 .exits = std::move(exits),
                 .lookahead = read,
                 .to_end = to_end});
    }

    return chunks;
}

/**
 * @brief Returns the states a chunk's copies may exit in: those among the chunk's exits from which its lookahead can be
 *        read under some marks, reaching acceptance on it when it runs to the input's end, by one walk back over the
 *        lookahead from the states it may end in.
 * @param projection The projection.
 * @param chunk The chunk.
 * @return The exit states that survive the lookahead.
 */
[[nodiscard]] Class_set_t readable_exits(const Projection& projection, const Worker_chunk& chunk)
{
    const Class_set_t every_class(projection.representatives.size(), true);

    auto readable{chunk.to_end ? projection.accepting : every_class};

    for (const auto byte : chunk.lookahead | std::views::reverse)
    {
        const auto index{byte_index(projection, static_cast<unsigned char>(byte))};

        if (!index)
        {
            return Class_set_t(projection.representatives.size(), false);
        }

        readable = preimage_by(projection, readable, *index);
    }

    std::ranges::transform(readable, chunk.exits, readable.begin(), std::logical_and{});

    return readable;
}

/**
 * @brief Runs one copy of a chunk: reads the chunk from an entry state under every marking the projection admits, one
 *        layer of reached states per byte, and returns each exit it offers with the marking that reaches it.
 * @param projection The projection.
 * @param bytes The chunk's bytes.
 * @param exit_states The states an exit may reach, readable_exits()'s.
 * @param entry The entry state.
 * @return The exits, in the order of the last layer.
 */
[[nodiscard]] std::vector<Exit> copy_exits(
        const Projection& projection, const std::string_view bytes, const Class_set_t& exit_states,
        const std::size_t entry)
{
    std::vector<std::vector<Reached>> layers{{{.state = entry, .from = 0, .boundary_after = false}}};

    for (const auto byte : bytes)
    {
        const auto index{byte_index(projection, static_cast<unsigned char>(byte))};

        if (!index)
        {
            return {};
        }

        const auto& layer{layers.back()};

        std::vector<Reached> next{};

        for (std::size_t from{0}; from < layer.size(); ++from)
        {
            for (const auto& [boundary_after, to] : projection.steps[*index][layer[from].state])
            {
                next.push_back({.state = to, .from = from, .boundary_after = boundary_after});
            }
        }

        if (next.empty())
        {
            return {};
        }

        layers.push_back(std::move(next));
    }

    std::vector<Exit> exits{};

    const auto& last{layers.back()};

    for (std::size_t at{0}; at < last.size(); ++at)
    {
        const auto state{last[at].state};

        if (!exit_states[state])
        {
            continue;
        }

        std::vector<bool> boundaries(bytes.size(), false);

        auto position{at};

        for (auto byte{bytes.size()}; byte > 0; --byte)
        {
            const auto& [reached, from, boundary_after]{layers[byte][position]};

            boundaries[byte - 1] = boundary_after;
            position = from;
        }

        exits.push_back({.state = state, .boundaries = std::move(boundaries)});
    }

    return exits;
}

/**
 * @brief Runs every worker, each on its own thread and the last on the calling thread, one copy per entry state of its
 *        chunk.
 * @param projection The projection.
 * @param chunks The chunks.
 * @return Per chunk and state, the exits the copy beginning there offers.
 */
[[nodiscard]] Tables_t worker_tables(const Projection& projection, const std::vector<Worker_chunk>& chunks)
{
    Tables_t tables(chunks.size());

    const auto scan{[&](const std::size_t chunk) {
        const auto& worker_chunk{chunks[chunk]};

        const auto exits{readable_exits(projection, worker_chunk)};

        auto& table{tables[chunk]};

        table.resize(projection.representatives.size());

        for (std::size_t entry{0}; entry < table.size(); ++entry)
        {
            if (worker_chunk.entries[entry])
            {
                table[entry] = copy_exits(projection, worker_chunk.bytes, exits, entry);
            }
        }
    }};

    {
        std::vector<std::jthread> workers{};

        workers.reserve(chunks.size() - 1);

        for (std::size_t chunk{0}; chunk + 1 < chunks.size(); ++chunk)
        {
            workers.emplace_back(scan, chunk);
        }

        scan(chunks.size() - 1);
    }

    return tables;
}

/**
 * @brief Stitches the workers' copies: from the start, each chain's state selects the copy of the next chunk, and every
 *        exit a selected copy offers is kept with a back-pointer to the chain it extends.
 * @param tables Per chunk and state, the exits the copy beginning there offers.
 * @param start The start's state in the projection.
 * @return Per cut, the chains kept there, or std::nullopt when none reaches the input's end.
 */
[[nodiscard]] std::optional<Levels_t> stitched(const Tables_t& tables, const std::size_t start)
{
    Levels_t levels{{{.state = start, .parent = 0, .boundaries = {}}}};

    for (const auto& table : tables)
    {
        const auto& candidates{levels.back()};

        std::vector<Candidate> next{};

        for (std::size_t parent{0}; parent < candidates.size(); ++parent)
        {
            for (const auto& [state, boundaries] : table[candidates[parent].state])
            {
                next.push_back({.state = state, .parent = parent, .boundaries = boundaries});
            }
        }

        if (next.empty())
        {
            return std::nullopt;
        }

        levels.push_back(std::move(next));
    }

    return levels;
}

/**
 * @brief Returns the segmentation the one chain at the input's end carries, its chunks' markings in order.
 * @param input The input.
 * @param levels Per cut, the chains kept there, one at the input's end.
 * @return The segmentation.
 */
[[nodiscard]] Marked_string segmentation_of(const std::string_view input, const Levels_t& levels)
{
    std::vector<std::vector<bool>> pieces{};

    std::size_t at{0};

    for (auto level{levels.size() - 1}; level > 0; --level)
    {
        const auto& [state, parent, boundaries]{levels[level][at]};

        pieces.push_back(boundaries);
        at = parent;
    }

    std::ranges::reverse(pieces);

    std::vector<bool> boundaries{};

    for (const auto& piece : pieces)
    {
        boundaries.insert(boundaries.end(), piece.cbegin(), piece.cend());
    }

    return {.bytes = std::string{input}, .boundaries = std::move(boundaries)};
}

/**
 * @brief Returns the cut of every level at which the chains kept there last share a chain: the level of their nearest
 *        common ancestor, the level itself when one chain is kept.
 * @param levels Per cut, the chains kept there.
 * @return Per level, the level of the chains' nearest common ancestor.
 */
[[nodiscard]] std::vector<std::size_t> common_levels(const Levels_t& levels)
{
    std::vector<std::size_t> common{};

    for (std::size_t level{0}; level < levels.size(); ++level)
    {
        std::vector<std::size_t> at(levels[level].size());

        std::iota(at.begin(), at.end(), std::size_t{0});

        auto shared{level};

        while (at.size() > 1)
        {
            const auto parent_of{[&](const std::size_t index) { return levels[shared][index].parent; }};

            std::ranges::transform(at, at.begin(), parent_of);

            dedup(at);

            --shared;
        }

        common.push_back(shared);
    }

    return common;
}

} // namespace

std::vector<Fanout_window> shortest_fanout_windows(const Verifier& verifier)
{
    const auto projection{projection_of(verifier)};

    auto lefts{forward_family(projection)};

    // At origin zero the cut follows the byte before the occurrence, so its configuration is one a byte enters.
    lefts.push_back({.states = entered_classes(projection), .word = {}});

    const auto rights{backward_family(projection)};

    const auto precedes{[](const Fanout_window& first, const Fanout_window& second) {
        const auto first_key{std::tuple{first.window.size(), std::string_view{first.window}, first.origin}};

        const auto second_key{std::tuple{second.window.size(), std::string_view{second.window}, second.origin}};

        return first_key < second_key;
    }};

    // Per rank, the least window of exactly that rank.
    std::map<std::size_t, Fanout_window> least_of_rank{};

    for (const auto& [left_states, left_word] : lefts)
    {
        for (const auto& [right_states, right_word] : rights)
        {
            const auto rank{shared_count(left_states, right_states)};

            if (rank == 0)
            {
                continue;
            }

            Fanout_window candidate{
                    .bound = rank,
                    .window = left_word + right_word,
                    .origin = left_word.size(),
                    .rank = rank};

            const auto [entry, inserted]{least_of_rank.try_emplace(rank, candidate)};

            auto& [entry_rank, least]{*entry};

            if (precedes(candidate, least))
            {
                least = std::move(candidate);
            }
        }
    }

    std::vector<Fanout_window> windows{};

    if (least_of_rank.empty())
    {
        return windows;
    }

    const auto least_rank{least_of_rank.cbegin()->first};

    const auto largest_rank{least_of_rank.crbegin()->first};

    auto shortest{least_of_rank.cbegin()->second};

    for (auto bound{least_rank}; bound <= largest_rank; ++bound)
    {
        const auto found{least_of_rank.find(bound)};

        if (found != least_of_rank.cend() && precedes(found->second, shortest))
        {
            shortest = found->second;
        }

        shortest.bound = bound;

        windows.push_back(shortest);
    }

    return windows;
}

Relative_fanout relative_fanout(const Verifier& verifier, const std::string_view window, const std::size_t origin)
{
    if (window.empty() || origin >= window.size())
    {
        throw std::invalid_argument{"relative_fanout: the window is empty or its origin lies outside it"};
    }

    const auto projection{projection_of(verifier)};

    const auto [rank, configurations]{fanout(verifier, window, origin)};

    const auto remainder{window.substr(origin)};

    std::vector<std::vector<State_t>> classes{};

    for (const auto configuration : configurations)
    {
        const auto state{projection.class_of[configuration]};

        const auto agrees{[&](const std::vector<State_t>& relative_class) {
            const auto least{projection.class_of[relative_class.front()]};

            return !told_apart(projection, least, state, remainder);
        }};

        const auto found{std::ranges::find_if(classes, agrees)};

        if (found == classes.end())
        {
            classes.push_back({configuration});

            continue;
        }

        found->push_back(configuration);
    }

    const auto relative_rank{classes.size()};

    return Relative_fanout{.rank = relative_rank, .classes = std::move(classes)};
}

std::optional<K_copy_scan> k_copy_scan(
        const Verifier& verifier, const std::string_view input, const Scan_request& request)
{
    const auto malformed{
            [](const Fanout_pair& pair) { return pair.window.empty() || pair.origin >= pair.window.size(); }};

    if (std::ranges::any_of(request.inventory, malformed))
    {
        throw std::invalid_argument{"k_copy_scan: a window of the inventory is empty or its origin lies outside it"};
    }

    const auto projection{projection_of(verifier)};

    const auto start{projection.class_of[verifier.start()]};

    const auto anchors{anchors_of(verifier, projection, input, request.inventory)};

    const auto cuts{cuts_of(anchors, input.size(), request.chunks)};

    const auto chunks{worker_chunks(projection, input, anchors, cuts, start, request.lookahead)};

    const auto tables{worker_tables(projection, chunks)};

    const auto levels{stitched(tables, start)};

    if (!levels)
    {
        return std::nullopt;
    }

    auto segmentation{segmentation_of(input, *levels)};

    const auto common{common_levels(*levels)};

    std::vector<Scanned_chunk> scanned{};

    for (std::size_t chunk{0}; chunk < chunks.size(); ++chunk)
    {
        const auto resolves{[&](const std::size_t level) { return common[level] > chunk; }};

        const auto later{std::views::iota(chunk + 1, common.size())};

        const auto resolving{*std::ranges::find_if(later, resolves)};

        const auto copies{static_cast<std::size_t>(std::ranges::count(chunks[chunk].entries, true))};

        scanned.push_back(
                {.begin = cuts[chunk], .end = cuts[chunk + 1], .copies = copies, .resolved = cuts[resolving]});
    }

    return K_copy_scan{.segmentation = std::move(segmentation), .chunks = std::move(scanned)};
}

} // namespace munch::verifier
