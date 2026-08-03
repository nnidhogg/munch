// Searches for certified multi-byte WINDOWS in the grammars whose single-byte certificate is empty, and checks the
// search's model against the shipped scanner.
//
// The single-byte certificate answers "which bytes always begin a token". Six rows of the applicability table answer
// none, which is the published result's sharpest limitation. A window generalizes the question: a byte string after
// which the current token's start is known whatever preceded it. A certified byte is the length-one case, so the
// search must reproduce is_split_point() exactly at that length, and this program asserts that before searching.
//
// The model. A worker cutting blind knows only that the scan is in one of the trim states. Reading a byte maps that
// uncertainty forward: a state consuming the byte into a trim state does so, carrying its token's start offset; a
// state that cannot has ended its token, so the lexer resets and the byte begins a new token at this offset. The
// window is certified when every surviving trajectory agrees on a start offset INSIDE the window. Agreement on the
// state alone is not enough, since learning that the scan is inside a string literal is knowledge rather than a
// boundary.
//
// Longest-match backup is what makes the model non-obvious. A token that cannot extend does not end at the byte that
// killed it; the scan rewinds to the last accepting position and re-reads. The reason the model survives that is
// that the uncertainty starts as EVERY trim state, so a rewind moves a trajectory onto one already tracked rather
// than out of the set. backup_disagreements() tests exactly that against the real scanner and asserts it never
// disagrees. Its prefixes cover (state, distance past the last accepting position) pairs, each optionally preceded
// by a completed token so the last boundary sits at varying distances before the window, which is what decides where
// a rewind lands. The prefix count is reported per row, since a coverage widening that widens nothing looks exactly
// like one that works.
//
// A backup check over inputs that never rewind proves nothing, so each row declares whether its own check exercises
// a rewind and that declaration is asserted. The first five rows do not: the C-like grammars cannot rewind at all,
// since every accepting prefix of their tokens stays accepting as it extends, and JSON's certified windows end their
// tokens cleanly enough that no prefix reaching a trim state produces one. The six rewinding rows carry the backup
// evidence, and they back up for structurally different reasons: a gap between a short token and a longer one, a
// numeric exponent, a float competing with a range operator, an operator ladder, a keyword extending an identifier,
// and a seven-byte rewind depth.
//
// Random grammars are the strongest check here. Hand-picked ones are what hid the origin defect, and they hid a
// second: the model treated reading from the initial state as always beginning a token, which is false when a
// nullable pattern makes that state re-entrant. Four hundred random grammars found 18 disagreements from that one
// cause, and none of the eleven named rows had a re-entrant initial state to expose it.
//
// The same caution applies to the length-one agreement. The first five rows certify no byte at all, so agreeing with
// is_split_point is 0 == 0 there and proves little. The rewinding rows all have non-empty certificates, and adding
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
 * @brief One possible scan: a state, and the offset at which its current token began.
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
 * @brief Maps the uncertainty forward over one byte, or nothing if no trajectory survives it.
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
        else if (restart_ok)
        {
            next.emplace(*restart, at);
        }
    }

    return next.empty() ? std::nullopt : std::optional{next};
}

/**
 * @brief Renames origins to their rank order, leaving the cloud's behaviour unchanged but the space finite.
 *
 * Origins are absolute offsets, so a naive search never repeats a state and cannot terminate. They are only ever
 * compared for equality, though, and a restart always assigns the current offset, which is larger than every origin
 * already present and therefore always fresh. So only the PARTITION of trajectories by origin matters, not the
 * values. Renaming to first-appearance order collapses the space to something finite, which is what turns the search
 * from an unbounded hunt into a decision procedure: exhausting it proves no window exists at any length.
 */
Cloud_t canonical(const Cloud_t& cloud)
{
    std::map<std::size_t, std::size_t> rank;

    Cloud_t out;

    for (const auto& [state, origin] : cloud)
    {
        if (origin != kBefore && !rank.contains(origin))
        {
            rank[origin] = rank.size();
        }
    }

    for (const auto& [state, origin] : cloud)
    {
        out.emplace(state, origin == kBefore ? kBefore : rank[origin]);
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

    // Canonical clouds make the space finite, so this cap is a safety net rather than the thing that terminates the
    // search. Whether it was hit is reported, so "no window exists" is never confused with "none found in time".
    constexpr std::size_t kSubsetBudget{200000};

    std::vector<std::string> found;

    std::size_t shortest{0};

    const auto reentrant{init_reentrant(dfa, live)};

    std::map<Cloud_t, std::string> seen;

    std::deque<std::pair<Cloud_t, std::string>> queue{{unknown(live), ""}};

    seen[canonical(unknown(live))] = "";

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

            if (const auto key{canonical(*next)}; !seen.contains(key))
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
 * @brief Counts places where the real scanner puts no token boundary where the model says one belongs.
 *
 * For every trim state it builds a shortest prefix reaching that state, lexes prefix + window + tail, and checks the
 * predicted offset against the boundaries the shipped scanner actually produced. Inputs whose scan dies before the
 * window ends test nothing and are skipped. A single disagreement means the model over-approximates and its windows
 * are not certificates.
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
    // grammar that rewinds seven bytes needs a prefix that is seven bytes past an accepting position to exercise it.
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

    // Every prefix above ends mid-token starting from the initial state, so the last token boundary always sits at
    // the prefix's start. Prepending completed tokens moves that boundary further back, which is what varies where a
    // rewind lands. Without this the prefix set exercises exactly one boundary distance.
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

                    std::set<std::size_t> starts;

                    std::size_t offset{0};

                    const auto consumed{lexer.tokenize_all<Token>(input, [&](const Token, const std::size_t length) {
                        starts.insert(offset);

                        offset += length;
                    })};

                    if (consumed < head.size() + window.size())
                    {
                        continue;
                    }

                    exercised += rewinds(dfa, input) > 0 ? 1 : 0;

                    disagreements += starts.contains(head.size() + *at) ? 0 : 1;
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

    switch (next() % 4)
    {
    case 0:
        return concat(random_regex(seed, depth - 1), random_regex(seed, depth - 1));

    case 1:
        return choice(random_regex(seed, depth - 1), random_regex(seed, depth - 1));

    case 2:
        return plus(random_regex(seed, depth - 1));

    default:
        return kleene(random_regex(seed, depth - 1));
    }
}

/**
 * @brief Checks the model against the shipped predicate and scanner over many random grammars.
 * @return The number of random grammars on which they disagreed.
 */
std::size_t random_grammars(
        const std::size_t count, std::size_t& usable, std::size_t& with_certificate, std::size_t& rescued,
        std::size_t& proved_none, std::size_t& inconclusive)
{
    usable = 0;

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
            // many does a multi-byte window rescue, and for how many is the answer provably no?
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
     * @brief Whether the grammar can rewind at all. Where it can, the backup check must be shown to exercise one,
     *        or its agreement is vacuous.
     */
    bool rewinds_possible;
};

bool run(const Row& row, Builder_dbg& builder)
{
    const auto dfa{builder.dfa()};

    const auto lexer{builder.build()};

    const auto live{trim(dfa)};

    const auto disagreements{single_byte_disagreements(dfa, lexer, live)};

    const auto reentrant{init_reentrant(dfa, live)};

    auto exhausted{false};

    const auto [shortest, found]{shortest_windows(dfa, live, exhausted)};

    // The backup check needs the windows the search actually reports, so a grammar whose shortest window is four is
    // checked at four rather than at two. It also needs volume, and a grammar whose shortest is one would otherwise
    // be checked only on single bytes, the case the published theorem already covers. So both are tested: every
    // certified window at the reported length, plus every certified two-byte window.
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

    // Agreement over inputs that never rewind says nothing about backup, so the row's declaration is asserted too.
    const auto covered{row.rewinds_possible == (exercised > 0)};

    // Reporting "no window" is a claim of non-existence, so it counts only when the search exhausted the canonical
    // cloud space. Hitting the safety cap instead is an inconclusive result and must not pass as a negative.
    const auto conclusive{shortest != 0 || exhausted};

    const auto ok{disagreements == 0 && shortest == row.shortest && backup == 0 && covered && conclusive};

    std::printf(
            "  %-30s k=1 %zu, window %zu/%zu, backup %zu over %7zu rewinding inputs from %4zu prefixes%s\n", row.name,
            disagreements, shortest, row.shortest, backup, exercised, prefixes, ok ? "" : "   <- MOVED");

    return ok;
}
} // namespace

int main()
{
    using namespace figures;

    std::printf("certified windows where no single byte is certified\n");

    auto ok{true};

    {
        Builder_dbg b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        ok = run({.name = "C-like + string literals", .shortest = 2, .rewinds_possible = false}, b) && ok;
    }
    {
        Builder_dbg b;
        c_like(b, false);
        b.add_token(line_comment(), Token::LineComment, 1);
        ok = run({.name = "C-like + // line comments", .shortest = 2, .rewinds_possible = false}, b) && ok;
    }
    {
        Builder_dbg b;
        c_like(b, false);
        b.add_token(block_comment(), Token::BlockComment, 1);
        ok = run({.name = "C-like + block comments", .shortest = 4, .rewinds_possible = false}, b) && ok;
    }
    {
        Builder_dbg b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        b.add_token(block_comment(), Token::BlockComment, 1);
        ok = run({.name = "C-like conventional", .shortest = 4, .rewinds_possible = false}, b) && ok;
    }
    {
        Builder_dbg b;
        json(b);
        // JSON rewinds readily in general, but not on the inputs this check builds: its CERTIFIED windows end their
        // tokens cleanly, so no prefix reaching a trim state produces one. The backup evidence comes from the
        // stress row below, which is the only row here that exercises a rewind at all.
        ok = run({.name = "JSON, RFC 8259", .shortest = 2, .rewinds_possible = false}, b) && ok;
    }
    {
        // The C-like rows above cannot rewind at all: every accepting prefix of their tokens stays accepting as it
        // extends, so agreement there says nothing about backup. This grammar is built to rewind constantly. "a"
        // accepts, "ab" does not, "abc" does, so scanning "abd" accepts "a" at offset 1 and rewinds two bytes.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(text("a"), Token::Identifier, 2);
        b.add_token(text("abc"), Token::Keyword, 1);
        b.add_token(text("b"), Token::Number, 2);
        b.add_token(text("d"), Token::Operator, 2);
        b.add_token(plus(any_of(Set{' '})), Token::Whitespace, 2);
        ok = run({.name = "rewind stress: a | abc | b | d", .shortest = 1, .rewinds_possible = true}, b) && ok;
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
        ok = run({.name = "rewind: digits | digits e digits", .shortest = 1, .rewinds_possible = true}, b) && ok;
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
        ok = run({.name = "rewind: float vs range operator", .shortest = 2, .rewinds_possible = true}, b) && ok;
    }
    {
        // An operator ladder with a gap in the middle: "<<x" accepts "<" and rewinds, since "<<" accepts nothing.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(text("<"), Token::Operator, 2);
        b.add_token(text("<<="), Token::Punctuation, 1);
        b.add_token(any_of(Set::alpha()), Token::Identifier, 2);
        b.add_token(plus(any_of(Set{' '})), Token::Whitespace, 2);
        ok = run({.name = "rewind: < | <<=", .shortest = 1, .rewinds_possible = true}, b) && ok;
    }
    {
        // A keyword strictly extending a shorter token: "fox" accepts "f" and rewinds, since "fo" accepts nothing.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(text("f"), Token::Identifier, 2);
        b.add_token(text("for"), Token::Keyword, 1);
        b.add_token(any_of(Set::alpha()), Token::Separator, 2);
        b.add_token(plus(any_of(Set{' '})), Token::Whitespace, 2);
        ok = run({.name = "rewind: f | for", .shortest = 1, .rewinds_possible = true}, b) && ok;
    }
    {
        // A deep rewind: seven bytes are scanned past the accepting position before the token dies.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(text("a"), Token::Identifier, 2);
        b.add_token(text("abcdefgh"), Token::Keyword, 1);
        b.add_token(any_of(Set::alpha()), Token::Separator, 2);
        b.add_token(plus(any_of(Set{' '})), Token::Whitespace, 2);
        ok = run({.name = "rewind: a | abcdefgh (depth 7)", .shortest = 1, .rewinds_possible = true}, b) && ok;
    }
    {
        // A nullable token ALONE minimizes to a single accepting state with a self-loop, so the initial state is
        // re-entrant and arriving there proves nothing about being between tokens. No window of any length can
        // certify a boundary, and the search reports that as PROVED rather than as not-found, having exhausted the
        // canonical cloud space. Adding a second token would defeat the case: minimization then separates the start
        // state from the looping one, the start state stops being re-entrant, and the certificate comes back.
        using namespace munch::regex;

        Builder_dbg b;
        b.add_token(kleene(text("a")), Token::Identifier, 1);
        ok = run({.name = "nullable a*: provably no window", .shortest = 0, .rewinds_possible = false}, b) && ok;
    }

    std::size_t usable{0};

    std::size_t with_certificate{0};

    std::size_t rescued{0};

    std::size_t proved_none{0};

    std::size_t inconclusive{0};

    const auto random_disagreements{random_grammars(400, usable, with_certificate, rescued, proved_none, inconclusive)};

    std::printf(
            "\n  %-30s %zu disagreements over %zu random grammars, %zu with a non-empty certificate%s\n",
            "random grammars", random_disagreements, usable, with_certificate,
            random_disagreements == 0 ? "" : "   <- MODEL IS WRONG");

    std::printf(
            "  %-30s of %zu certifying no byte: %zu rescued by a window, %zu provably none, %zu inconclusive\n",
            "window applicability", usable - with_certificate, rescued, proved_none, inconclusive);

    // Inconclusive must stay zero: the whole point of canonicalising origins is that the search decides existence
    // rather than giving up. A non-zero count means the space stopped being finite and the negatives are no longer
    // proofs.
    ok = random_disagreements == 0 && usable > 100 && with_certificate > 20 && inconclusive == 0 && rescued > 300 &&
         proved_none > 0 && ok;

    std::printf(
            "\n%s\n", ok ? "The model reproduces is_split_point at length one on six named grammars "
                           "with a non-empty certificate and on 400 random ones, and the shipped scanner puts "
                           "a boundary wherever it predicts one across six distinct rewind mechanisms." :
                           "A measurement moved. The window results must be re-derived before being relied on.");

    return ok ? 0 : 1;
}
