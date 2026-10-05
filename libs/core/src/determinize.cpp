#include "munch/core/determinize.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <boost/container_hash/hash.hpp>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/exceptions/state_limit_error.hpp"
#include "munch/dfa/builder.hpp"

namespace munch::core
{
namespace
{
/**
 * @brief Determinizes an NFA by subset construction over dense bit sets.
 *
 * A reachable state set of a byte-expanded Unicode class holds thousands of members, and the construction visits every
 * set once per distinct symbol, so set membership, union, and identity must not cost a tree node each. The states are
 * projected onto dense indices once; a set is then a vector of words, a closure is a precomputed member list unioned by
 * setting bits, and a set's identity is the hash of its words. Closure is transitive, so a target already present
 * contributes nothing and its closure is skipped whole.
 */
class Determinizer
{
public:
    /**
     * @brief Projects the NFA onto dense state indices: its symbol moves, epsilon targets and accepting tokens.
     * @param nfa The NFA to determinize.
     * @param state_limit The largest number of DFA states to discover before throwing; zero means unlimited.
     * @throws std::runtime_error If the NFA numbers a state beyond the 32-bit dense index.
     */
    explicit Determinizer(const nfa::Nfa& nfa, const std::size_t state_limit)
        : Determinizer{nfa, state_limit, state_count(nfa)}
    {}

    /**
     * @brief Runs the construction and returns the built DFA.
     * @return The DFA, its states numbered in discovery order.
     * @throws State_limit_error If a non-zero state limit is exceeded.
     */
    [[nodiscard]] dfa::Dfa run()
    {
        return walk([](const Members_t&) {});
    }

    /**
     * @brief Runs the construction and returns the accepting candidates of every reachable subset instead.
     *
     * The single traversal serves both construction and diagnostics, so diagnostics see exactly the subsets the build
     * discovers by definition rather than by a mirrored reimplementation; the DFA built along the way is simply not
     * kept.
     * @return The accepting candidate tokens, one list per accepting subset, in discovery order.
     * @throws State_limit_error If a non-zero state limit is exceeded.
     */
    [[nodiscard]] std::vector<std::vector<nfa::Token>> candidates()
    {
        std::vector<std::vector<nfa::Token>> found{};

        const auto keep_accepting{[this, &found](const Members_t& members) {
            auto candidates{candidates_of(members)};

            if (!candidates.empty())
            {
                found.push_back(std::move(candidates));
            }
        }};

        std::ignore = walk(keep_accepting);

        return found;
    }

private:
    /**
     * @brief The number of bits in one word of a state set.
     */
    static constexpr std::size_t word_bits{64};

    /**
     * @brief The number of byte values, one edge list each.
     */
    static constexpr std::size_t symbol_count{256};

    /**
     * @brief A set of NFA states as one bit per dense state index.
     */
    using Bits_t = std::vector<std::uint64_t>;

    /**
     * @brief A list of NFA states as dense indices.
     */
    using Members_t = std::vector<std::uint32_t>;

    /**
     * @brief The symbol transition of one state: the consumed symbol and the target states.
     */
    struct Move
    {
        /**
         * @brief The consumed symbol, as a table row index.
         */
        unsigned char symbol{};

        /**
         * @brief The states the symbol leads to.
         */
        Members_t targets{};
    };

    /**
     * @brief A move of a member state, named by the member and the move's place in its list.
     */
    struct Edge
    {
        /**
         * @brief The member state whose move this is, as a dense index.
         */
        std::uint32_t member{};

        /**
         * @brief The move's index in the member's list of moves.
         */
        std::uint32_t move{};
    };

    /**
     * @brief The edges of one state set grouped by symbol, one list per byte value.
     */
    using Buckets_t = std::array<std::vector<Edge>, symbol_count>;

    /**
     * @brief Hasher for a set of states, combining the hashes of its words.
     */
    struct Words_hash
    {
        /**
         * @brief Hashes the set's words.
         * @param bits The set to hash.
         * @return The combined hash of the words.
         */
        std::size_t operator()(const Bits_t& bits) const noexcept
        {
            return boost::hash_range(bits.cbegin(), bits.cend());
        }
    };

    /**
     * @brief The DFA state of every state set discovered so far.
     */
    using Ids_t = std::unordered_map<Bits_t, dfa::Dfa::State_t, Words_hash>;

    /**
     * @brief The discovered state sets not yet expanded, with their DFA states, in discovery order.
     */
    using Queue_t = std::queue<std::pair<Bits_t, dfa::Dfa::State_t>>;

    /**
     * @brief Sizes every per-state table for the NFA's dense indices and files its transitions and accept states.
     * @param nfa The NFA to determinize.
     * @param state_limit The largest number of DFA states to discover before throwing; zero means unlimited.
     * @param count The number of dense indices the NFA's states need, as state_count() returns it.
     */
    Determinizer(const nfa::Nfa& nfa, const std::size_t state_limit, const std::size_t count)
        : state_limit_{state_limit}
        , words_{(count + word_bits - 1) / word_bits}
        , init_{static_cast<std::uint32_t>(nfa.init_state())}
        , moves_(count)
        , epsilon_(count)
        , closures_(count)
        , accepts_(count)
    {
        index_transitions(nfa);

        index_accepts(nfa);
    }

    /**
     * @brief Returns the number of dense indices the NFA's states need: one past its highest state identifier.
     *
     * The highest identifier rather than the count, so an oversized NFA is rejected before the increment, which would
     * wrap for a hand-built NFA whose highest state is the largest std::size_t. Dense indices are 32 bits, and
     * identifiers are used as given rather than remapped, so a sparse NFA numbering states beyond that range is
     * rejected rather than silently truncated onto them.
     * @param nfa The NFA to measure.
     * @return One past the highest state identifier the NFA names.
     * @throws std::runtime_error If that identifier does not fit the 32-bit dense index.
     */
    [[nodiscard]] static std::size_t state_count(const nfa::Nfa& nfa)
    {
        auto highest{nfa.init_state()};

        for (const auto& [key, targets] : nfa.transitions())
        {
            const auto& [state, label]{key};

            highest = std::max(highest, state);

            for (const auto target : targets)
            {
                highest = std::max(highest, target);
            }
        }

        for (const auto state : std::views::keys(nfa.accept_states()))
        {
            highest = std::max(highest, state);
        }

        if (highest >= std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error{"NFA has too many states for the determinizer's dense index"};
        }

        return highest + 1;
    }

    /**
     * @brief Files every NFA transition under its source state: a symbol one as a move, an epsilon one as the state's
     *        epsilon targets.
     * @param nfa The NFA whose transitions are filed.
     */
    void index_transitions(const nfa::Nfa& nfa)
    {
        for (const auto& [key, targets] : nfa.transitions())
        {
            const auto& [state, label]{key};

            Members_t list{targets.cbegin(), targets.cend()};

            if (!label.is_symbol())
            {
                epsilon_[state] = std::move(list);

                continue;
            }

            const auto symbol{static_cast<unsigned char>(label.symbol())};

            moves_[state].push_back({.symbol = symbol, .targets = std::move(list)});
        }
    }

    /**
     * @brief Files the accepting token of every NFA accept state.
     * @param nfa The NFA whose accept states are filed.
     */
    void index_accepts(const nfa::Nfa& nfa)
    {
        for (const auto& [state, token] : nfa.accept_states())
        {
            accepts_[state] = token;
        }
    }

    /**
     * @brief Runs the subset walk, building the DFA and showing each expanded state set's members to a visitor.
     *
     * Each set is expanded once, in discovery order, and its successors are taken in ascending symbol order, which
     * keeps state discovery, and with it the DFA's numbering, deterministic.
     * @tparam Visit A callable taking the members of a state set.
     * @param visit Called once per expanded state set, in discovery order, with its members.
     * @return The DFA, its states numbered in discovery order.
     * @throws State_limit_error If a non-zero state limit is exceeded.
     */
    template <std::invocable<const Members_t&> Visit>
    [[nodiscard]] dfa::Dfa walk(Visit visit)
    {
        dfa::Builder builder{};

        Bits_t initial(words_, 0);

        add_closure(initial, init_);

        Ids_t ids{{initial, builder.init_state()}};

        Queue_t queue{};

        queue.emplace(std::move(initial), builder.init_state());

        Members_t members{};

        std::vector<unsigned char> symbols{};

        Buckets_t buckets{};

        Bits_t scratch(words_, 0);

        const auto state_of{[&](const Bits_t& set) {
            if (const auto found{ids.find(set)}; found != ids.cend())
            {
                const auto& [known_set, known_state]{*found};

                return known_state;
            }

            if (state_limit_ != 0 && ids.size() >= state_limit_)
            {
                throw State_limit_error{state_limit_};
            }

            const auto state{builder.next_state()};

            ids.emplace(set, state);

            queue.emplace(set, state);

            return state;
        }};

        while (!queue.empty())
        {
            const auto [bits, dfa_state]{std::move(queue.front())};

            queue.pop();

            list_members(bits, members);

            if (const auto token{accept_of(members)})
            {
                builder.add_accept_state(dfa_state, dfa::Token{token->id()});
            }

            visit(members);

            bucket_moves(members, buckets, symbols);

            std::ranges::sort(symbols);

            for (const auto symbol : symbols)
            {
                unite_targets(buckets[symbol], scratch);

                buckets[symbol].clear();

                const auto target{state_of(scratch)};

                const dfa::Label label{static_cast<char>(symbol)};

                builder.add_transition(dfa_state, label, target);
            }
        }

        return std::move(builder).build();
    }

    /**
     * @brief Adds a state's epsilon closure to a set, unless the set already holds the state.
     *
     * Closure is transitive, so a state already present has its whole closure present too.
     * @param bits The set to add to.
     * @param state The state whose closure is added.
     */
    void add_closure(Bits_t& bits, const std::uint32_t state)
    {
        if (contains(bits, state))
        {
            return;
        }

        for (const auto member : closure_of(state))
        {
            insert(bits, member);
        }
    }

    /**
     * @brief Returns whether the set holds the state.
     * @param bits The set.
     * @param state The state to look for.
     * @return Whether the state's bit is set.
     */
    [[nodiscard]] static bool contains(const Bits_t& bits, const std::uint32_t state) noexcept
    {
        return ((bits[state / word_bits] >> (state % word_bits)) & 1U) != 0;
    }

    /**
     * @brief Returns the memoized epsilon closure of one state, itself included, as a dense member list.
     *
     * A closure always contains its own state, so an empty slot marks one not yet computed.
     * @param state The state whose closure is wanted.
     * @return The closure, in the breadth-first order it was discovered in.
     */
    const Members_t& closure_of(const std::uint32_t state)
    {
        if (!closures_[state].empty())
        {
            return closures_[state];
        }

        Members_t result{state};

        Bits_t seen(words_, 0);

        insert(seen, state);

        for (std::size_t index{0}; index < result.size(); ++index)
        {
            for (const auto next : epsilon_[result[index]])
            {
                if (contains(seen, next))
                {
                    continue;
                }

                insert(seen, next);

                result.push_back(next);
            }
        }

        closures_[state] = std::move(result);

        return closures_[state];
    }

    /**
     * @brief Inserts the state into the set.
     * @param bits The set.
     * @param state The state to insert.
     */
    static void insert(Bits_t& bits, const std::uint32_t state) noexcept
    {
        bits[state / word_bits] |= 1ULL << (state % word_bits);
    }

    /**
     * @brief Lists the members of a state set in ascending order.
     * @param bits The state set.
     * @param members Receives the members, replacing what it held.
     */
    void list_members(const Bits_t& bits, Members_t& members) const
    {
        members.clear();

        for (std::size_t word{0}; word < words_; ++word)
        {
            for (auto rest{bits[word]}; rest != 0; rest &= rest - 1)
            {
                const auto bit{static_cast<std::size_t>(std::countr_zero(rest))};

                const auto member{static_cast<std::uint32_t>(word * word_bits + bit)};

                members.push_back(member);
            }
        }
    }

    /**
     * @brief Returns the winning token among the accepting members, if any; the minimum resolves priority then
     *        identifier.
     * @param members The members of a state set.
     * @return The winning token, or nothing when no member accepts.
     */
    [[nodiscard]] std::optional<nfa::Token> accept_of(const Members_t& members) const
    {
        std::optional<nfa::Token> best{};

        for (const auto member : members)
        {
            const auto& candidate{accepts_[member]};

            if (candidate && (!best || *candidate < *best))
            {
                best = candidate;
            }
        }

        return best;
    }

    /**
     * @brief Returns the accepting tokens of a state set's members.
     * @param members The members of a state set.
     * @return The tokens, in member order; empty when no member accepts.
     */
    [[nodiscard]] std::vector<nfa::Token> candidates_of(const Members_t& members) const
    {
        std::vector<nfa::Token> candidates{};

        for (const auto member : members)
        {
            if (accepts_[member])
            {
                candidates.push_back(*accepts_[member]);
            }
        }

        return candidates;
    }

    /**
     * @brief Files every move of a state set's members under its symbol, noting each symbol when first filed.
     * @param members The members of a state set.
     * @param buckets The per-symbol edge lists, empty on entry; receives the edges.
     * @param symbols Receives the distinct symbols in the order they were first filed, replacing what it held.
     */
    void bucket_moves(const Members_t& members, Buckets_t& buckets, std::vector<unsigned char>& symbols) const
    {
        symbols.clear();

        for (const auto member : members)
        {
            for (const auto& [index, move] : std::views::enumerate(moves_[member]))
            {
                if (buckets[move.symbol].empty())
                {
                    symbols.push_back(move.symbol);
                }

                const auto place{static_cast<std::uint32_t>(index)};

                buckets[move.symbol].push_back({.member = member, .move = place});
            }
        }
    }

    /**
     * @brief Replaces a set with the union of the closures of the targets of one symbol's edges.
     * @param edges The edges of one symbol.
     * @param scratch Receives the union, replacing what it held.
     */
    void unite_targets(const std::vector<Edge>& edges, Bits_t& scratch)
    {
        std::ranges::fill(scratch, 0);

        for (const auto [member, index] : edges)
        {
            for (const auto target : moves_[member][index].targets)
            {
                add_closure(scratch, target);
            }
        }
    }

    /**
     * @brief The determinization cap; zero means unlimited.
     */
    std::size_t state_limit_;

    /**
     * @brief The number of 64-bit words a state set occupies.
     */
    std::size_t words_;

    /**
     * @brief The NFA's initial state as a dense index.
     */
    std::uint32_t init_;

    /**
     * @brief The symbol transitions of each state.
     */
    std::vector<std::vector<Move>> moves_;

    /**
     * @brief The epsilon targets of each state.
     */
    std::vector<Members_t> epsilon_;

    /**
     * @brief The memoized epsilon closures; see closure_of().
     */
    std::vector<Members_t> closures_;

    /**
     * @brief The accepting token of each state, if any.
     */
    std::vector<std::optional<nfa::Token>> accepts_;
};

} // namespace

dfa::Dfa determinize(const nfa::Nfa& nfa, const std::size_t state_limit)
{
    return Determinizer{nfa, state_limit}.run();
}

std::vector<std::vector<nfa::Token>> Builder::reachable_candidates(const nfa::Nfa& nfa, const std::size_t state_limit)
{
    return Determinizer{nfa, state_limit}.candidates();
}

} // namespace munch::core
