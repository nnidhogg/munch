// Searches for candidate multi-byte WINDOWS in the grammars whose single-byte certificate is empty, and checks the
// search's model against the shipped scanner.
//
// THE MODEL BELOW IS CONSERVATIVE AND PROVED SOUND. It reports windows it can justify and refuses ones it cannot,
// so its counts are a lower bound on what genuinely certifies, never an upper one. The proof is stated here in
// full, because the note carrying it is not public. A second adversarial read confirmed it on 2026-08-05,
// independently re-deriving the representation lemma and the quotient congruence and reproducing every figure.
//
// ASSUMPTIONS. The DFA is trimmed to its live states L, meaning reachable and co-accessible; q0 is live; no token
// matches the empty string; and the input considered is one the scanner tokenizes completely. Note that "the scan
// is in a live state" is a claim about the FINAL segmentation's token prefixes, which is what the lemma tracks.
// Speculative lookahead that maximal munch later rewinds away can sit in a state that is not co-accessible at all.
//
// NOTATION. Fix an input x whose window W = w_0 ... w_{k-1} occupies offsets [t, t+k), with k >= 1. Maximal munch
// gives x a unique boundary set. C_j is the cloud after the model has consumed the first j bytes of the window. For
// j in 1..k let sigma_j be the start of the token containing the byte at t+j-1 IN THE FINAL GREEDY SEGMENTATION,
// let rho_j = delta*(q0, x[sigma_j .. t+j)) be that token's prefix state after consuming through byte t+j-1, and
// let omega_j be sigma_j - t, or "before" when sigma_j < t.
//
// REPRESENTATION LEMMA. For every j in 1..k, the pair (rho_j, omega_j) is in C_j. Note what this does and does not
// say: rho_j is the prefix state of the token the FINAL segmentation assigns to that byte, not wherever the
// scanner's read head happens to be. Speculative lookahead that is later rewound away occupies other states, and
// the lemma says nothing about them.
//
// Every rho_j is live: reachable by construction, and co-accessible because its token ends at some e >= t+j in an
// accepting state with x[t+j .. e) carrying rho_j there.
//
// BASE, j = 1. If sigma_1 < t then omega_1 is "before", the state p = delta*(q0, x[sigma_1 .. t)) is live and so
// lies in C_0, which is all of L paired with "before", and the direct branch carries it to (rho_1, before) with the
// origin intact. The begins rename cannot interfere for the same reason it cannot in the step: p has consumed at
// least one byte, so p = q0 only if a non-empty live path returns to q0, which makes init_reentrant true and begins
// false. If
// sigma_1 = t then rho_1 = delta(q0, w_0) and the seed fires, because C_0 is all of L and contains at least one
// live accepting state: the grammar has a token, and that token's accepting state is reachable and trivially
// co-accessible.
//
// STEP, j to j+1. Two cases for the byte at t+j.
//
//   It continues its token, sigma_{j+1} = sigma_j. Then rho_{j+1} = delta(rho_j, w_j), live as above, and the
//   direct branch carries the pair with its origin unchanged. The begins rename does not overwrite that origin,
//   which is the one place the implementation's "begins ? at : origin" needs its own argument: the rename fires
//   only when the pre-step state is q0 AND q0 is not re-entrant. Here rho_j has consumed at least the byte at
//   t+j-1, so if rho_j = q0 then a non-empty live path returns to q0, which is exactly what init_reentrant
//   detects, so it is true, begins is false, and the origin survives. Where q0 is genuinely not re-entrant,
//   rho_j = q0 cannot arise and the case is vacuous.
//
//   It begins a token, sigma_{j+1} = t+j. Then omega_{j+1} = j and rho_{j+1} = delta(q0, w_j), live because the new
//   token runs to an accepting state through it. The seed emits exactly that pair, so all that is needed is that
//   the seed fires, which needs some state in C_j to accept. It does: the token that ended at t+j ended there
//   because x[sigma_j .. t+j) is a token, so delta*(q0, x[sigma_j .. t+j)) accepts, and that state is rho_j, in
//   C_j by hypothesis.
//
// SOUNDNESS. If every pair in C_k carries the same origin o and o is not "before", then (rho_k, omega_k) is in C_k
// by the lemma, so omega_k = o, so sigma_k = t + o, which is a boundary. Since x was an arbitrary completely
// tokenized input containing W at t, the certificate holds in every context.
//
// Backup never appears in the argument. The model tracks where tokens BEGIN rather than what the scanner reads, so
// a boundary backup later exposes was already seeded when the accepting position justifying it was crossed.
//
// It replaced an earlier model whose soundness could not be argued, and the defect is worth keeping in view. That
// model restarted a trajectory whenever it could not consume the byte. Over tokens {a, abc, bx, x} and the window
// "abx" the trajectory following "ab" toward "abc" is killed by "x", restarts there, and carries a token beginning
// at offset 2, where the scanner backs up to the accepted "a" and matches "bx", cutting at 0 and 1. The complete
// old cloud refused that window rather than reporting 2, so this is a gap in the argument, not a wrong answer
// anyone observed.
//
// MEASURED, so the claim stays honest: that is the behaviour of one trajectory, not of the model. The old model
// starts from EVERY live state, and those trajectories disagree, so it REFUSES "abx" rather than mispredicting it.
// No input is known on which the old model reports a boundary the scanner does not take. Its problem is that no
// soundness argument closes, not that a counterexample exists. A trajectory that cannot consume a byte is an
// impossible history rather than a token boundary, and restarting it is what the argument cannot justify.
//
// The repair: drop the failure restart, and seed one fresh trajectory at the window's first byte and thereafter
// only where some tracked state accepts, because a token can only end where the automaton accepted. The cloud then
// contains the final segmentation's actual token-prefix history, alongside conservative hypotheses, so backup never
// has to be simulated. It is conservative in the other direction: over {a, ab, b} the scanner always takes "ab",
// but the accepting "a" also seeds "b" and the window is refused.
//
// Acceptance gating alone does not terminate, since a grammar like a+ accepts after every byte and accumulates
// origins without bound. The search therefore deduplicates on a finite quotient rather than on the cloud: which
// states carry the pre-window origin, and how many in-window origins each state carries, saturated at two. Two
// clouds sharing a key have identical futures FOR CERTIFICATION, which is all the walk asks of them, so exploring
// one of them loses nothing and the walk decides this model exactly. The walk also carries a subset budget, and a
// search that exhausts it is reported inconclusive rather than negative. What exhaustion does not give is semantic
// non-existence: the model itself refuses windows a greedy scanner would allow.
//
// The single-byte certificate answers "which bytes always begin a token". Six rows of the applicability table answer
// none, which is the published result's sharpest limitation. A window generalizes the question: a byte string after
// which the current token's start is known whatever preceded it. A certified byte is the length-one case, so the
// search must reproduce is_split_point() exactly at that length, and this program asserts that before searching.
//
// The model. A worker cutting blind knows only that the scan is in one of the trim states, so the cloud starts as
// all of them carrying a token that began before the window. Reading a byte maps that uncertainty forward: a state
// consuming the byte into a trim state does so and keeps its token's start offset, and a state that cannot is an
// impossible history and is dropped. Separately, one fresh trajectory is seeded at the window's first byte and
// thereafter wherever some tracked state accepts, because a token can only begin where the previous one ended and
// one can only end where the automaton accepted. The cloud therefore represents every way the input can be cut into
// token words, not only the greedy way, which is what makes it independent of backup and also what makes it
// conservative. The window is certified when every surviving trajectory agrees on a start offset INSIDE it.
// Agreement on the state alone is not enough, since learning that the scan is inside a string literal is knowledge
// rather than a boundary.
//
// Longest-match backup is what makes the model non-obvious. A token that cannot extend does not end at the byte
// that killed it; the scan rewinds to the last accepting position and re-reads. The model never simulates that,
// because it tracks where tokens BEGIN rather than what the scanner reads: a boundary backup later exposes was
// seeded when the accepting position justifying it was crossed. That is the proof's step for a byte beginning a
// token, and it is why the failure restart had to go rather than be repaired. backup_disagreements() is therefore
// a check on the implementation rather than evidence for the model, and it asserts the scanner never disagrees. Its
// prefixes cover (state, distance past the last accepting position) pairs, each optionally preceded by an accepted
// word so the last boundary sits at varying distances before the window, which is what decides where a rewind lands.
// The prefix count is reported per row, since a coverage widening that widens nothing looks exactly like one that
// works.
//
// A backup check over inputs that never rewind proves nothing, so each row declares whether its own check exercises
// a rewind and that declaration is asserted. The first five rows do not, which is a fact about the inputs this
// search builds rather than about the grammars: the block-comment grammar CAN rewind, on an unfinished comment
// opener after the slash has been accepted, but no window it reports produces one. Seven rows carry the backup
// evidence over six distinct mechanisms, two of the rows sharing the short-token-then-longer-token gap: that gap, a
// numeric exponent, a float competing with a range operator, an operator ladder, a keyword extending an identifier,
// and a seven-byte rewind depth.
//
// Random grammars are the strongest check here. Hand-picked ones are what hid the origin defect, and they hid a
// second: the model treated reading from the initial state as always beginning a token, which is false when a
// nullable pattern makes that state re-entrant. An earlier UNFILTERED sweep of four hundred found 18 disagreements
// from that one cause, and none of the named rows had a re-entrant initial state to expose it. The sweep now
// excludes nullable grammars, since the soundness proof does not cover them, so that count is history rather than
// something this program still reports.
//
// The same caution applies to the length-one agreement. The first five rows certify no byte at all, so agreeing with
// is_split_point is 0 == 0 there and proves little. Six of the seven rewinding rows have non-empty certificates,
// the float-against-range one being the exception with a shortest window of two, and adding
// the first of them caught a real defect the vacuous comparisons had hidden: a trajectory reading from the initial
// state begins a token at that offset rather than inheriting the origin it carried in.

#include <cstddef>
#include <cstdio>
#include <deque>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "grammars.hpp"

namespace
{
using figures::Token;

using munch::dfa::Dfa;

/**
 * @brief A Builder exposing its compiled automaton, the way the unit tests expose it.
 */
class Builder_dbg : public munch::core::Builder
{
public:
    using Builder::dfa;
};

using States_t = std::set<Dfa::State_t>;

/**
 * @brief A token started before the window, so its boundary is unknown to a worker cutting here.
 */
constexpr std::size_t kBefore{static_cast<std::size_t>(-1)};

/**
 * @brief One final-token-prefix hypothesis: a state, and the offset at which its current token began.
 */
using Trajectory_t = std::pair<Dfa::State_t, std::size_t>;

using Cloud_t = std::set<Trajectory_t>;

/**
 * @brief The trim states, reachable from the initial state and able to still reach an accepting one.
 */
States_t trim(const Dfa& dfa)
{
    States_t reachable{dfa.init_state()};

    std::deque<Dfa::State_t> pending{dfa.init_state()};

    while (!pending.empty())
    {
        const auto state{pending.front()};

        pending.pop_front();

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            if (const auto next{dfa.advance(state, static_cast<char>(symbol))}; next && !reachable.contains(*next))
            {
                reachable.insert(*next);

                pending.push_back(*next);
            }
        }
    }

    States_t co_accessible;

    for (const auto state : reachable)
    {
        if (dfa.has_accept_token(state))
        {
            co_accessible.insert(state);
        }
    }

    for (auto grew{true}; grew;)
    {
        grew = false;

        for (const auto state : reachable)
        {
            if (co_accessible.contains(state))
            {
                continue;
            }

            for (int symbol{0}; symbol < 256; ++symbol)
            {
                if (const auto next{dfa.advance(state, static_cast<char>(symbol))};
                    next && co_accessible.contains(*next))
                {
                    co_accessible.insert(state);

                    grew = true;

                    break;
                }
            }
        }
    }

    return co_accessible;
}

/**
 * @brief Whether any live transition re-enters the initial state.
 *
 * Where it does, arriving at the initial state no longer proves the scan is between tokens, so the model must not
 * rename a trajectory's origin on reaching it. A nullable pattern is one way to produce that, minimizing to an
 * accepting start state with a self-loop, but it is not the only one: any cycle back to the start does it.
 */
bool init_reentrant(const Dfa& dfa, const States_t& live)
{
    for (const auto state : live)
    {
        for (int symbol{0}; symbol < 256; ++symbol)
        {
            if (const auto next{dfa.advance(state, static_cast<char>(symbol))}; next && *next == dfa.init_state())
            {
                return true;
            }
        }
    }

    return false;
}

std::optional<Cloud_t> step(
        const Dfa& dfa, const States_t& live, const Cloud_t& from, const char symbol, const std::size_t at,
        const bool reentrant)
{
    const auto restart{dfa.advance(dfa.init_state(), symbol)};

    const auto restart_ok{restart && live.contains(*restart)};

    // A token can only end where the automaton accepted, so a boundary BEFORE this byte is possible exactly where
    // some tracked state accepts. Hence the test runs on the cloud as it stands, ahead of the step.
    auto accepting{false};

    for (const auto& [state, origin] : from)
    {
        accepting = accepting || dfa.has_accept_token(state);
    }

    Cloud_t next;

    for (const auto& [state, origin] : from)
    {
        if (const auto direct{dfa.advance(state, symbol)}; direct && live.contains(*direct))
        {
            // Reading from the initial state BEGINS a token here, so the origin is this offset rather than whatever
            // the trajectory carried in. That holds only while nothing re-enters the initial state: a nullable
            // pattern minimizes to an accepting start state with a self-loop, and then arriving there no longer
            // proves the scan is between tokens. The shipped predicate withdraws its own exemption for the same
            // reason, so the model must too or it certifies bytes the library correctly rejects.
            const auto begins{state == dfa.init_state() && !reentrant};

            next.emplace(*direct, begins ? at : origin);
        }

        // No restart when a trajectory dies. A state that cannot consume the byte is an impossible history, not a
        // token boundary, and restarting there is what made the old model predict boundaries the scanner does not
        // take: over {a, abc, bx, x} and "abx" one trajectory carried a boundary at offset 2, where the scanner
        // cuts at 0 and 1. The complete old cloud refused that window rather than reporting 2, so this is a gap in
        // the argument rather than an observed wrong answer.
    }

    // One fresh trajectory wherever the automaton had just accepted, which is the only place a token can begin.
    // No special case for the window's first byte: the initial cloud is every live state and so contains an
    // accepting one, making the guard true there anyway, and the transition stays the same at every offset. The
    // final segmentation's actual token-prefix history is then contained in the cloud, alongside conservative
    // hypotheses, so backup never has to be simulated.
    if (restart_ok && accepting)
    {
        next.emplace(*restart, at);
    }

    return next.empty() ? std::nullopt : std::optional{next};
}

/**
 * @brief The finite quotient the search deduplicates on: which states carry the pre-window origin, and how many
 *        distinct in-window origins each state carries, saturated at two.
 *
 * Acceptance gating seeds an origin wherever the automaton accepts, so `a+` alone accumulates origins without bound
 * and renaming them to rank order no longer bounds the space. Exactness rests on an invariant of the clouds this
 * model can reach: every in-window origin occupies exactly one state, because it is seeded once into a single state
 * and the deterministic step moves it to at most one successor. An arbitrary cloud placing one origin in two states
 * would defeat the saturated counts, but no such cloud is reachable. Under that invariant, saturating at two is
 * exact for the question asked, since two origins meeting in one deterministic state follow identical futures under
 * this quotient and can never separate again, and summing counts across predecessors is exact rather than an
 * over-approximation, since origins arriving from different predecessors are necessarily distinct and cannot be
 * double counted. The pre-window origin is held apart in its own support set. The quotient is therefore a
 * transition congruence over reachable clouds, and the walk decides exactly whether a window exists under this
 * model and what its minimum length is. It does NOT enumerate every certified
 * word: prefixes reaching the same key collapse to one representative, so witnesses are examples rather than the
 * full set. At most 6^|Q+| configurations, so it terminates.
 */
using Quotient_t = std::pair<std::set<Dfa::State_t>, std::map<Dfa::State_t, int>>;

Quotient_t quotient(const Cloud_t& cloud)
{
    Quotient_t out;

    for (const auto& [state, origin] : cloud)
    {
        if (origin == kBefore)
        {
            out.first.insert(state);
        }
        else
        {
            auto& count{out.second[state]};

            count = std::min(count + 1, 2);
        }
    }

    return out;
}

/**
 * @brief Whether every trajectory agrees the current token began at the same offset inside the window.
 */
bool certified(const Cloud_t& cloud)
{
    const auto origin{cloud.begin()->second};

    if (origin == kBefore)
    {
        return false;
    }

    for (const auto& [state, at] : cloud)
    {
        if (at != origin)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief The maximum uncertainty: every trim state, each mid-token from before the window.
 */
Cloud_t unknown(const States_t& live)
{
    Cloud_t cloud;

    for (const auto state : live)
    {
        cloud.emplace(state, kBefore);
    }

    return cloud;
}

/**
 * @brief The offset at which the model says a token begins, or nothing if the window is not certified.
 */
std::optional<std::size_t> predicted(
        const Dfa& dfa, const States_t& live, const std::string& window, const bool reentrant)
{
    auto cloud{unknown(live)};

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto next{step(dfa, live, cloud, window[at], at, reentrant)};

        if (!next)
        {
            return std::nullopt;
        }

        cloud = *next;
    }

    return certified(cloud) ? std::optional{cloud.begin()->second} : std::nullopt;
}

/**
 * @brief How many bytes the model and the shipped predicate disagree about at length one.
 */
std::size_t single_byte_disagreements(const Dfa& dfa, const munch::core::Lexer& lexer, const States_t& live)
{
    const auto reentrant{init_reentrant(dfa, live)};

    std::size_t disagreements{0};

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const std::string one(1, static_cast<char>(symbol));

        disagreements +=
                predicted(dfa, live, one, reentrant).has_value() != lexer.is_split_point(static_cast<char>(symbol));
    }

    return disagreements;
}

/**
 * @brief The length of the shortest certified window, or zero if none exists within the bound.
 */
std::pair<std::size_t, std::vector<std::string>> shortest_windows(const Dfa& dfa, const States_t& live, bool& exhausted)
{
    exhausted = true;

    // Bounded so a grammar with many certified windows cannot make this a long-running test.
    constexpr std::size_t kKeep{400};

    // The finite quotient is what terminates the search, so this cap is a safety net rather than the bound. Whether
    // it was hit is reported, so "no window under this model" is never confused with "none found in time".
    constexpr std::size_t kSubsetBudget{200000};

    std::vector<std::string> found;

    std::size_t shortest{0};

    const auto reentrant{init_reentrant(dfa, live)};

    std::map<Quotient_t, std::string> seen;

    std::deque<std::pair<Cloud_t, std::string>> queue{{unknown(live), ""}};

    seen[quotient(unknown(live))] = "";

    while (!queue.empty())
    {
        const auto [current, word]{queue.front()};

        queue.pop_front();

        // Once a length has produced certified windows, deeper ones cannot be shorter.
        if ((shortest != 0 && word.size() >= shortest) || seen.size() > kSubsetBudget)
        {
            exhausted = exhausted && seen.size() <= kSubsetBudget;

            continue;
        }

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto next{step(dfa, live, current, static_cast<char>(symbol), word.size(), reentrant)};

            if (!next)
            {
                continue;
            }

            const auto extended{word + static_cast<char>(symbol)};

            if (certified(*next))
            {
                shortest = shortest != 0 ? shortest : extended.size();

                if (found.size() < kKeep)
                {
                    found.push_back(extended);
                }

                continue;
            }

            if (const auto key{quotient(*next)}; !seen.contains(key))
            {
                seen[key] = extended;

                queue.emplace_back(*next, extended);
            }
        }
    }

    return {shortest, found};
}

/**
 * @brief How many rewinds a full scan performs: getting stuck past the last accepting position.
 *
 * A backup check over inputs that never rewind proves nothing about backup, so the caller asserts this is non-zero
 * wherever the grammar can produce one.
 */
std::size_t rewinds(const Dfa& dfa, const std::string& input)
{
    std::size_t count{0};

    std::size_t at{0};

    while (at < input.size())
    {
        auto state{dfa.init_state()};

        auto accepted{false};

        auto last_accept{at};

        auto reached{at};

        while (reached < input.size())
        {
            const auto next{dfa.advance(state, input[reached])};

            if (!next)
            {
                break;
            }

            state = *next;

            ++reached;

            if (dfa.has_accept_token(state))
            {
                last_accept = reached;

                accepted = true;
            }
        }

        if (!accepted)
        {
            break;
        }

        count += reached > last_accept ? 1 : 0;

        at = last_accept;
    }

    return count;
}

/**
 * @brief Counts places where the real scanner disagrees with the origin the model predicts.
 *
 * For every trim state it builds a shortest prefix reaching that state, lexes prefix + window + tail, and requires
 * the token CONTAINING the window's last byte to start exactly at the predicted offset. That is the model's actual
 * claim; merely finding a boundary somewhere would be weaker, since over {a, abc, bx, x} and "abx" both 0 and 1 are
 * boundaries and only 1 is the origin. Inputs whose scan dies before the window ends test nothing and are skipped.
 * A single disagreement means the model is unsound and its windows are not certificates.
 */
std::size_t backup_disagreements(
        const Dfa& dfa, const munch::core::Lexer& lexer, const States_t& live, const std::vector<std::string>& windows,
        std::size_t& exercised, std::size_t& prefixes)
{
    const auto reentrant{init_reentrant(dfa, live)};

    exercised = 0;

    prefixes = 0;

    // One prefix per state is not enough. What decides how far a rewind travels is the distance the scan has run
    // PAST its last accepting position, so the prefix set covers (state, distance) pairs rather than states. A
    // grammar that rewinds seven bytes needs a prefix that is seven bytes past an accepting position to exercise
    // it. Coverage is therefore over the reachable live (state, distance) pairs with distance at most kMaxDistance,
    // not over every input the grammar admits.
    constexpr std::size_t kMaxDistance{9};

    std::map<std::pair<Dfa::State_t, std::size_t>, std::string> prefix;

    std::deque<std::pair<Dfa::State_t, std::size_t>> pending;

    const auto seed{std::pair{dfa.init_state(), std::size_t{0}}};

    prefix[seed] = "";

    pending.push_back(seed);

    while (!pending.empty())
    {
        const auto [state, distance]{pending.front()};

        pending.pop_front();

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto next{dfa.advance(state, static_cast<char>(symbol))};

            if (!next || !live.contains(*next))
            {
                continue;
            }

            // An accepting state resets the distance; anything else moves one byte further from the last one.
            const auto moved{dfa.has_accept_token(*next) ? std::size_t{0} : distance + 1};

            if (moved > kMaxDistance)
            {
                continue;
            }

            if (const auto key{std::pair{*next, moved}}; !prefix.contains(key))
            {
                prefix[key] = prefix[{state, distance}] + static_cast<char>(symbol);

                pending.push_back(key);
            }
        }
    }

    // Every prefix above ends mid-token starting from the initial state. Prepending an accepted word varies the
    // POTENTIAL boundary distance ahead of the window: it does not place a boundary, because under maximal munch
    // the concatenation can extend that word rather than end it. Without this the prefix set exercises one such
    // distance.
    std::vector<std::string> completed{""};

    for (const auto& [key, head] : prefix)
    {
        const auto& [state, distance]{key};

        if (dfa.has_accept_token(state) && !head.empty() && completed.size() < 6)
        {
            completed.push_back(head);
        }
    }

    std::vector<std::string> heads;

    for (const auto& done : completed)
    {
        for (const auto& [key, head] : prefix)
        {
            heads.push_back(done + head);
        }
    }

    prefixes = heads.size();

    constexpr const char* tails[]{"", " ", "\n", " x", ";\n", "\n}\n", " 1 ", "\"s\"\n"};

    std::size_t disagreements{0};

    for (const auto& window : windows)
    {
        {
            const auto at{predicted(dfa, live, window, reentrant)};

            if (!at)
            {
                continue;
            }

            for (const auto& head : heads)
            {
                for (const auto* tail : tails)
                {
                    const auto input{head + window + tail};

                    // The model's claim is not "a token starts there" but "the token containing the window's last
                    // byte starts there", which is strictly stronger: over {a, abc, bx, x} and "abx" both 0 and 1
                    // are token starts, and only 1 is the origin of the token covering the final byte.
                    const auto last{head.size() + window.size() - 1};

                    std::size_t containing{0};

                    auto covered{false};

                    std::size_t offset{0};

                    const auto consumed{lexer.tokenize_all<Token>(input, [&](const Token, const std::size_t length) {
                        if (offset <= last && last < offset + length)
                        {
                            containing = offset;

                            covered = true;
                        }

                        offset += length;
                    })};

                    if (consumed < head.size() + window.size())
                    {
                        continue;
                    }

                    exercised += rewinds(dfa, input) > 0 ? 1 : 0;

                    disagreements += covered && containing == head.size() + *at ? 0 : 1;
                }
            }
        }
    }

    return disagreements;
}

/**
 * @brief A deterministic pseudo-random regex over a three-symbol alphabet.
 *
 * Hand-picked grammars are what hid the origin defect: five of them certified no byte, so the model agreeing with
 * is_split_point meant 0 == 0. Random grammars remove that selection, and a small alphabet keeps the automata small
 * enough to search exhaustively.
 */
munch::regex::Regex random_regex(unsigned& seed, const std::size_t depth)
{
    using namespace munch::regex;

    const auto next{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    if (depth == 0 || next() % 3 == 0)
    {
        constexpr const char* atoms[]{"a", "b", "c", "ab", "bc", "ca"};

        return text(atoms[next() % std::size(atoms)]);
    }

    // Sequenced through locals rather than written as two arguments. Both calls advance the seed, and the order in
    // which a compiler evaluates function arguments is unspecified, so GCC and Clang generated different grammars
    // from the same seed and disagreed on every count derived from them.
    switch (next() % 4)
    {
    case 0:
    {
        auto left{random_regex(seed, depth - 1)};

        auto right{random_regex(seed, depth - 1)};

        return concat(std::move(left), std::move(right));
    }

    case 1:
    {
        auto left{random_regex(seed, depth - 1)};

        auto right{random_regex(seed, depth - 1)};

        return choice(std::move(left), std::move(right));
    }

    case 2:
        return plus(random_regex(seed, depth - 1));

    default:
        return kleene(random_regex(seed, depth - 1));
    }
}

/**
 * @brief Whether this grammar is outside the soundness proof, which assumes no token matches the empty string.
 *
 * The initial state accepting is exactly that condition. Checked at every entry point rather than only in the
 * random sweep, so a named row cannot quietly drift outside the theorem it is evidence for.
 */
bool nullable_grammar(const Dfa& dfa)
{
    return dfa.has_accept_token(dfa.init_state()).has_value();
}

/**
 * @brief Checks the model against the shipped predicate and scanner over many random grammars.
 * @return The number of disagreement instances, counting each disagreeing byte or input, not the grammars.
 */
std::size_t random_grammars(
        const std::size_t count, std::size_t& usable, std::size_t& nullable, std::size_t& with_certificate,
        std::size_t& rescued, std::size_t& proved_none, std::size_t& inconclusive)
{
    usable = 0;

    nullable = 0;

    with_certificate = 0;

    rescued = 0;

    proved_none = 0;

    inconclusive = 0;

    std::size_t disagreements{0};

    unsigned seed{20260803};

    for (std::size_t index{0}; index < count; ++index)
    {
        Builder_dbg builder;

        builder.set_state_limit(400);

        const auto tokens{2 + (seed >> 8U) % 4};

        for (std::size_t token{0}; token < tokens; ++token)
        {
            builder.add_token(random_regex(seed, 3), token, 1 + token % 2);
        }

        try
        {
            const auto dfa{builder.dfa()};

            const auto lexer{builder.build()};

            const auto live{trim(dfa)};

            if (live.empty())
            {
                continue;
            }

            // The soundness proof assumes no token matches the empty string, so a grammar whose initial state
            // accepts is outside what the theorem covers and must not be counted toward the figure the theorem
            // backs. The generator produces them freely: two thirds of the sample.
            if (nullable_grammar(dfa))
            {
                ++nullable;

                continue;
            }

            ++usable;

            const auto reentrant{init_reentrant(dfa, live)};

            std::size_t certified_bytes{0};

            for (int symbol{0}; symbol < 256; ++symbol)
            {
                const std::string one(1, static_cast<char>(symbol));

                const auto model{predicted(dfa, live, one, reentrant).has_value()};

                const auto shipped{lexer.is_split_point(static_cast<char>(symbol))};

                certified_bytes += shipped ? 1 : 0;

                disagreements += model != shipped ? 1 : 0;
            }

            with_certificate += certified_bytes > 0 ? 1 : 0;

            // The applicability question for windows: among grammars the single-byte certificate cannot serve, how
            // many does a multi-byte window rescue, and for how many does this conservative model find none?
            if (certified_bytes == 0)
            {
                auto exhausted{false};

                const auto [length, found]{shortest_windows(dfa, live, exhausted)};

                rescued += length > 0 ? 1 : 0;

                proved_none += length == 0 && exhausted ? 1 : 0;

                inconclusive += length == 0 && !exhausted ? 1 : 0;
            }

            std::vector<std::string> windows;

            for (int first{0}; first < 256; ++first)
            {
                for (int second{0}; second < 256; ++second)
                {
                    const std::string pair{static_cast<char>(first), static_cast<char>(second)};

                    if (predicted(dfa, live, pair, reentrant))
                    {
                        windows.push_back(pair);
                    }
                }
            }

            std::size_t exercised{0};

            std::size_t prefixes{0};

            disagreements += backup_disagreements(dfa, lexer, live, windows, exercised, prefixes);
        }
        catch (const std::exception&)
        {
            // A grammar that blows the state limit tells us nothing about the model; skip it.
        }
    }

    return disagreements;
}

/**
 * @brief A grammar under test and the shortest window length previously measured for it.
 */
struct Row
{
    const char* name;

    std::size_t shortest;

    /**
     * @brief Whether the backup check's own generated inputs are expected to rewind.
     *
     * A claim about those inputs, not about the grammar: several rows whose grammars CAN rewind produce no rewind
     * over the windows this search reports. The C-like block-comment grammar is one, since an unfinished comment
     * opener rewinds onto the accepted slash. Where a rewind IS expected the check must be shown to exercise one,
     * or its agreement proves nothing about backup.
     */
    bool rewinds_expected;
};

/**
 * @brief Every rewinding execution the named rows exercised, asserted in total because the notes quote the figure.
 */
std::size_t g_exercised_total{0};

/**
 * @brief Whether the live set can carry evidence at all: non-empty and containing the initial state.
 *
 * A grammar whose trimmed automaton lost the initial state accepts nothing, so every check over it would pass
 * vacuously; refusing it here keeps an all-green run from ever resting on empty evidence.
 */
bool live_usable(const Dfa& dfa, const States_t& live)
{
    return !live.empty() && live.contains(dfa.init_state());
}

/**
 * @brief Asserts that a NAMED window this grammar would otherwise never be checked at agrees with the scanner.
 *
 * run() only checks the windows the search reports, so a grammar whose shortest window is one byte never exercises a
 * longer one. The window that exposed the old proof gap is three bytes long, and this is what keeps it under test.
 */
bool named_window_agrees(const char* name, const std::string& window, std::size_t expected, Builder_dbg& builder)
{
    const auto dfa{builder.dfa()};

    if (nullable_grammar(dfa))
    {
        std::printf("  %-30s REJECTED: a token matches the empty string, outside the proof\n", name);

        return false;
    }

    const auto lexer{builder.build()};

    const auto live{trim(dfa)};

    if (!live_usable(dfa, live))
    {
        std::printf("  %-30s REJECTED: no live path from the initial state, evidence would be vacuous\n", name);

        return false;
    }

    std::size_t exercised{0};

    std::size_t prefixes{0};

    const auto disagreements{backup_disagreements(dfa, lexer, live, {window}, exercised, prefixes)};

    g_exercised_total += exercised;

    const auto reentrant{init_reentrant(dfa, live)};

    const auto at{predicted(dfa, live, window, reentrant)};

    std::printf(
            "  %-30s window \"%s\" %s, backup %zu over %zu rewinding executions%s\n", name, window.c_str(),
            at ? ("origin " + std::to_string(*at)).c_str() : "refused", disagreements, exercised,
            disagreements == 0 ? "" : "   <- MODEL IS WRONG");

    // A refusal is a fine outcome for a conservative model in general, but not here: this window is the one the
    // gated step exists to certify, and the failure-restart step refuses it. Pinning the origin as well as the
    // agreement is what stops a model that certifies the window at the wrong offset from passing.
    return disagreements == 0 && exercised > 0 && at && *at == expected;
}

bool run(const Row& row, Builder_dbg& builder)
{
    const auto dfa{builder.dfa()};

    if (nullable_grammar(dfa))
    {
        std::printf("  %-30s REJECTED: a token matches the empty string, outside the proof\n", row.name);

        return false;
    }

    const auto lexer{builder.build()};

    const auto live{trim(dfa)};

    if (!live_usable(dfa, live))
    {
        std::printf("  %-30s REJECTED: no live path from the initial state, evidence would be vacuous\n", row.name);

        return false;
    }

    const auto disagreements{single_byte_disagreements(dfa, lexer, live)};

    const auto reentrant{init_reentrant(dfa, live)};

    auto exhausted{false};

    const auto [shortest, found]{shortest_windows(dfa, live, exhausted)};

    // The backup check needs the windows the search actually reports, so a grammar whose shortest window is four is
    // checked at four rather than at two. It also needs volume, and a grammar whose shortest is one would otherwise
    // be checked only on single bytes, the case the published theorem already covers. So both are tested: up to 400
    // representative witnesses at the reported minimum length, the search collapsing prefixes that share a quotient
    // key and capping the list, plus every certified two-byte window.
    auto windows{found};

    for (int first{0}; first < 256; ++first)
    {
        for (int second{0}; second < 256; ++second)
        {
            const std::string pair{static_cast<char>(first), static_cast<char>(second)};

            if (predicted(dfa, live, pair, reentrant))
            {
                windows.push_back(pair);
            }
        }
    }

    std::size_t exercised{0};

    std::size_t prefixes{0};

    const auto backup{backup_disagreements(dfa, lexer, live, windows, exercised, prefixes)};

    g_exercised_total += exercised;

    // Agreement over inputs that never rewind says nothing about backup, so the row's declaration is asserted too.
    const auto covered{row.rewinds_expected == (exercised > 0)};

    // Reporting "no window" counts only when the search exhausted the quotient space rather than hitting the
    // budget, which would be inconclusive. Even exhausted it means none under this conservative model, never none
    // in the maximal-munch sense: the quotient decides the model exactly, but the model refuses windows a greedy
    // scanner would allow.
    const auto conclusive{shortest != 0 || exhausted};

    const auto ok{disagreements == 0 && shortest == row.shortest && backup == 0 && covered && conclusive};

    std::printf(
            "  %-30s k=1 %zu, window %zu/%zu, backup %zu over %7zu rewinding executions from %4zu prefixes%s\n",
            row.name, disagreements, shortest, row.shortest, backup, exercised, prefixes, ok ? "" : "   <- MOVED");

    return ok;
}
} // namespace

int main()
{
    using namespace figures;

    std::printf(
            "windows where no single byte is certified, under the proved conservative model described in the header\n");

    auto ok{true};

    {
        Builder_dbg b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        ok = run({.name = "C-like + string literals", .shortest = 2, .rewinds_expected = false}, b) && ok;
    }
    {
        Builder_dbg b;
        c_like(b, false);
        b.add_token(line_comment(), Token::LineComment, 1);
        ok = run({.name = "C-like + // line comments", .shortest = 2, .rewinds_expected = false}, b) && ok;
    }
    {
        Builder_dbg b;
        c_like(b, false);
        b.add_token(block_comment(), Token::BlockComment, 1);
        ok = run({.name = "C-like + block comments", .shortest = 4, .rewinds_expected = false}, b) && ok;
    }
    {
        Builder_dbg b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        b.add_token(block_comment(), Token::BlockComment, 1);
        ok = run({.name = "C-like conventional", .shortest = 4, .rewinds_expected = false}, b) && ok;
    }
    {
        Builder_dbg b;
        json(b);
        // JSON rewinds readily in general, but not on the inputs this check builds: its certified windows end their
        // tokens cleanly, so no prefix reaching a trim state produces one. The backup evidence comes from the seven
        // rows below that do exercise one.
        ok = run({.name = "JSON, RFC 8259", .shortest = 2, .rewinds_expected = false}, b) && ok;
    }
    {
        // The rows above produce no rewind over the windows this search reports, which is a fact about those inputs
        // rather than about the grammars: the block-comment one rewinds on an unfinished opener after the slash has
        // been accepted. So their agreement says nothing about backup. This grammar rewinds constantly. "a"
        // accepts, "ab" does not, "abc" does, so scanning "abd" accepts "a" at offset 1 and rewinds two bytes.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(text("a"), Token::Identifier, 2);
        b.add_token(text("abc"), Token::Keyword, 1);
        b.add_token(text("b"), Token::Number, 2);
        b.add_token(text("d"), Token::Operator, 2);
        b.add_token(plus(any_of(Set{' '})), Token::Whitespace, 2);
        ok = run({.name = "rewind stress: a | abc | b | d", .shortest = 1, .rewinds_expected = true}, b) && ok;
    }
    {
        // The grammar that exposed the gap in the model this program used to run. Scanning "abx", the old step
        // followed "ab"
        // toward "abc", saw "x" kill that path, restarted there, and carried a token beginning at offset 2. The
        // scanner backs up to the accepted "a", emits it, and matches "bx", so its boundaries are 0 and 1. The old
        // model as a whole refused the window rather than answering 2, since its other trajectories disagreed, so
        // this witnesses the proof gap rather than a wrong prediction. The row exists so that restarting a dead
        // trajectory cannot come back unnoticed.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(text("a"), Token::Identifier, 2);
        b.add_token(text("abc"), Token::Keyword, 1);
        b.add_token(text("bx"), Token::Number, 1);
        b.add_token(text("x"), Token::Operator, 2);
        b.add_token(plus(any_of(Set{' '})), Token::Whitespace, 2);
        ok = run({.name = "proof-gap witness grammar", .shortest = 1, .rewinds_expected = true}, b) && ok;

        // The search stops at one byte here, so the three-byte window that exposed the defect has to be named.
        ok = named_window_agrees("proof-gap witness abx", "abx", 1, b) && ok;
    }
    // Five more rewinding grammars, each backing up for a structurally different reason, so the backup evidence does
    // not rest on one shape. Every one of them accepts a short prefix, then continues into a longer token that can
    // die, forcing the scan back to an accepting position strictly before where it got stuck.
    {
        // Numeric exponent, the classic real case: "12e" followed by a non-digit accepts "12" and rewinds one byte.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(plus(any_of(Set::digits())), Token::Number, 2);
        b.add_token(
                concat(plus(any_of(Set::digits())), concat(text("e"), plus(any_of(Set::digits())))), Token::Literal, 1);
        b.add_token(any_of(Set::alpha()), Token::Identifier, 2);
        b.add_token(plus(any_of(Set{' '})), Token::Whitespace, 2);
        ok = run({.name = "rewind: digits | digits e digits", .shortest = 1, .rewinds_expected = true}, b) && ok;
    }
    {
        // A float whose dot may instead begin a range operator: "1..2" accepts "1" and rewinds off the dot.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(plus(any_of(Set::digits())), Token::Number, 2);
        b.add_token(
                concat(plus(any_of(Set::digits())), concat(text("."), plus(any_of(Set::digits())))), Token::Literal, 1);
        b.add_token(text(".."), Token::Operator, 1);
        b.add_token(plus(any_of(Set{' '})), Token::Whitespace, 2);
        ok = run({.name = "rewind: float vs range operator", .shortest = 2, .rewinds_expected = true}, b) && ok;
    }
    {
        // An operator ladder with a gap in the middle: "<<x" accepts "<" and rewinds, since "<<" accepts nothing.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(text("<"), Token::Operator, 2);
        b.add_token(text("<<="), Token::Punctuation, 1);
        b.add_token(any_of(Set::alpha()), Token::Identifier, 2);
        b.add_token(plus(any_of(Set{' '})), Token::Whitespace, 2);
        ok = run({.name = "rewind: < | <<=", .shortest = 1, .rewinds_expected = true}, b) && ok;
    }
    {
        // A keyword strictly extending a shorter token: "fox" accepts "f" and rewinds, since "fo" accepts nothing.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(text("f"), Token::Identifier, 2);
        b.add_token(text("for"), Token::Keyword, 1);
        b.add_token(any_of(Set::alpha()), Token::Separator, 2);
        b.add_token(plus(any_of(Set{' '})), Token::Whitespace, 2);
        ok = run({.name = "rewind: f | for", .shortest = 1, .rewinds_expected = true}, b) && ok;
    }
    {
        // A deep rewind: seven bytes are scanned past the accepting position before the token dies.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(text("a"), Token::Identifier, 2);
        b.add_token(text("abcdefgh"), Token::Keyword, 1);
        b.add_token(any_of(Set::alpha()), Token::Separator, 2);
        b.add_token(plus(any_of(Set{' '})), Token::Whitespace, 2);
        ok = run({.name = "rewind: a | abcdefgh (depth 7)", .shortest = 1, .rewinds_expected = true}, b) && ok;
    }
    {
        // One token that repeats. Every byte continues the run as readily as it starts one, so no window pins a
        // boundary and the search returns nothing at any length. `a+` rather than `a*`: the soundness proof assumes
        // no token matches the empty string, so a nullable grammar is outside what the theorem covers and makes a
        // poor negative example. The negative here means "none under this conservative model", not "none exists".
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(plus(text("a")), Token::Identifier, 1);
        ok = run({.name = "a+: no window found", .shortest = 0, .rewinds_expected = false}, b) && ok;
    }

    std::size_t usable{0};

    std::size_t nullable{0};

    std::size_t with_certificate{0};

    std::size_t rescued{0};

    std::size_t proved_none{0};

    std::size_t inconclusive{0};

    const auto random_disagreements{
            random_grammars(400, usable, nullable, with_certificate, rescued, proved_none, inconclusive)};

    std::printf(
            "\n  %-30s %zu disagreements over %zu non-nullable grammars, %zu with a non-empty certificate, %zu "
            "nullable ones excluded%s\n",
            "random grammars", random_disagreements, usable, with_certificate, nullable,
            random_disagreements == 0 ? "" : "   <- MODEL IS WRONG");

    std::printf(
            "  %-30s of %zu certifying no byte: %zu certified, %zu with none found, %zu inconclusive\n",
            "window applicability", usable - with_certificate, rescued, proved_none, inconclusive);

    std::printf("  %-30s %zu rewinding executions across every named row\n", "backup total", g_exercised_total);

    // Asserted rather than printed, because these figures are quoted in the notes and a loose bound would let one
    // move without anything failing. They are the NON-NULLABLE sample, which is what the soundness proof covers;
    // two thirds of what the generator produces is nullable and is excluded rather than counted. Pinning them
    // exactly is only meaningful because random_regex() sequences its recursive calls: while it left them as
    // function arguments the sweep depended on evaluation order and GCC and Clang produced different grammars.
    // Inconclusive must stay zero: hitting the budget means the traversal stopped before exhausting the bounded
    // quotient, not that the quotient stopped bounding anything. A negative is "no window under this conservative
    // model", never "no window exists", since it refuses windows a greedy scanner would allow.
    // The aggregate is asserted for the same reason as the sweep figures: the notes quote 1,079,392 rewinding
    // executions, and only a pinned total keeps that sentence honest when a row or the input builder changes.
    ok = random_disagreements == 0 && usable == 134 && nullable == 266 && with_certificate == 39 &&
         usable - with_certificate == 95 && rescued == 91 && proved_none == 4 && inconclusive == 0 &&
         g_exercised_total == 1'079'392 && ok;

    std::printf(
            "\n%s\n", ok ? "The model reproduces is_split_point at length one on six named grammars with a "
                           "non-empty certificate and on the 134 non-nullable grammars of a 400-grammar sweep, and "
                           "wherever it predicts an origin the shipped scanner starts the token covering the window's "
                           "last byte exactly there. The model is proved sound, so this checks the implementation "
                           "rather than the argument." :
                           "A measurement moved. The window results must be re-derived before being relied on.");

    return ok ? 0 : 1;
}
