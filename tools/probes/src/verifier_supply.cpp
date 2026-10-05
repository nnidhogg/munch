// Holds the verifier's window certificates against the library's window route on a JSON corpus and measures the
// certified positions each supplies.
//
// The RFC 8259 lexer of the papers is built through figures::json, its armed-run verifier beside it. Every distinct
// byte window of length one to three occurring in the corpus is decided at every origin inside it, by miscovering() on
// the verifier and by Lexer::is_split_window() on the lexer. An origin the window route certifies and the verifier
// refuses is a disagreement, printed, and fails the run. An origin the verifier certifies and the window route refuses
// is a refusal of the route's conservative model, printed and held to the exact search of
// Lexer::window_counterexample(), which must certify it; one it refutes or leaves unsettled fails the run. The supply
// is then the set of corpus positions k + o over every occurrence at k of every certified (window, o), counted per
// route, per KiB, and by the longest stretch of bytes no certified position falls in. Every line is "name: value" and
// stable.
//
// Usage: munch_verifier_supply <corpus file>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/dfa/verifier.hpp"
#include "munch/dfa/verifier_decisions.hpp"
#include "munch/tools/probes/builder_dbg.hpp"

namespace
{
using namespace munch;

/**
 * @brief The window lengths decided, one to this many bytes.
 */
constexpr std::size_t longest_window{3};

/**
 * @brief The bytes in a KiB, the unit the supply is reported per.
 */
constexpr double bytes_per_kib{1024.0};

/**
 * @brief The certified origins of one window under each route, as bits over the in-window origins.
 */
struct Certified_origins
{
    /**
     * @brief Bit o is set when miscovering() finds no miscovering at origin o.
     */
    unsigned verifier{};

    /**
     * @brief Bit o is set when is_split_window() returns o.
     */
    unsigned window_route{};
};

/**
 * @brief The two routes whose certificates the probe compares.
 */
enum class Route
{
    /**
     * @brief The armed-run verifier's miscovering() decision.
     */
    verifier,

    /**
     * @brief The lexer's is_split_window() decision.
     */
    window_route
};

/**
 * @brief The certified origins of every distinct window of the corpus, keyed by the window's bytes in the corpus.
 */
using Decisions_t = std::unordered_map<std::string_view, Certified_origins>;

/**
 * @brief The certified positions of the corpus under one route, as one flag per byte.
 */
using Positions_t = std::vector<bool>;

/**
 * @brief One window decided at every origin inside it: the certified origins under each route and the tallies.
 */
struct Decision
{
    /**
     * @brief The certified origins under each route.
     */
    Certified_origins origins{};

    /**
     * @brief The origins the window route certifies and the verifier refuses, and a window route origin outside the
     *        window.
     */
    std::size_t disagreements{0};

    /**
     * @brief The origins the verifier certifies and the window route refuses.
     */
    std::size_t refusals{0};

    /**
     * @brief The refusals whose exact search finds a counterexample or stops at its cap.
     */
    std::size_t unconfirmed{0};
};

/**
 * @brief The tallies over every distinct window of the corpus.
 */
struct Tally
{
    /**
     * @brief The (window, origin) pairs decided.
     */
    std::size_t pairs{0};

    /**
     * @brief The pairs the verifier certifies.
     */
    std::size_t verifier_pairs{0};

    /**
     * @brief The pairs the window route certifies.
     */
    std::size_t window_route_pairs{0};

    /**
     * @brief The disagreements of every window.
     */
    std::size_t disagreements{0};

    /**
     * @brief The window route's refusals of every window.
     */
    std::size_t refusals{0};

    /**
     * @brief The refusals the exact search does not confirm.
     */
    std::size_t unconfirmed{0};
};

/**
 * @brief Every distinct window of the corpus decided, with the tallies.
 */
struct Decided
{
    /**
     * @brief The decided windows.
     */
    Decisions_t decisions{};

    /**
     * @brief The tallies over them.
     */
    Tally tally{};
};

/**
 * @brief Reads a whole file as bytes, refusing a corpus it cannot read whole.
 *
 * Unlike files.hpp's read_bytes(), which keeps the bytes read before a read that fails part way, a failing read here
 * leaves the file buffer's std::ios_base::failure uncaught, so the probe never measures a truncated corpus.
 * @param path The file's path.
 * @return The bytes, or std::nullopt when the file cannot be opened.
 */
std::optional<std::string> read_file(const std::filesystem::path& path)
{
    std::ifstream stream{path, std::ios::binary};

    if (!stream)
    {
        return std::nullopt;
    }

    std::string bytes{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};

    if (stream.bad())
    {
        return std::nullopt;
    }

    return bytes;
}

/**
 * @brief Visits every window of one to longest_window bytes of the corpus, the lengths ascending and each length's
 *        positions ascending.
 * @tparam Visit The visitor's type, callable with a position and the window there.
 * @param corpus The corpus.
 * @param visit The visitor.
 */
template <typename Visit>
void for_each_window(const std::string_view corpus, const Visit& visit)
{
    for (std::size_t length{1}; length <= longest_window && length <= corpus.size(); ++length)
    {
        for (std::size_t k{0}; k + length <= corpus.size(); ++k)
        {
            visit(k, corpus.substr(k, length));
        }
    }
}

/**
 * @brief Spells a window as lowercase hexadecimal, two digits per byte.
 * @param window The window.
 * @return The hexadecimal text.
 */
std::string hex(const std::string_view window)
{
    std::string text{};

    for (const auto byte : window)
    {
        text += std::format("{:02x}", static_cast<unsigned char>(byte));
    }

    return text;
}

/**
 * @brief Decides one window at every origin inside it under both routes, printing each origin where they differ.
 *
 * An origin the verifier certifies and the window route refuses is held to Lexer::window_counterexample(), which must
 * exhaust its search without a witness.
 * @param verifier The armed-run verifier.
 * @param lexer The lexer of the same token set.
 * @param window The window, nonempty.
 * @return The decision.
 */
Decision decide(const dfa::Verifier& verifier, const core::Lexer& lexer, const std::string_view window)
{
    const auto route_origin{lexer.is_split_window(window)};

    Decision decision{};

    auto& [origins, disagreements, refusals, unconfirmed]{decision};

    auto& [verifier_origins, route_origins]{origins};

    if (route_origin.has_value() && *route_origin >= window.size())
    {
        std::cout << std::format("disagreement: window {} window route origin {} outside", hex(window), *route_origin)
                  << '\n';

        ++disagreements;
    }

    for (std::size_t origin{0}; origin < window.size(); ++origin)
    {
        const auto miscovered{dfa::miscovering(verifier, window, origin)};

        const auto by_verifier{!miscovered.has_value()};

        const auto by_route{route_origin == origin};

        verifier_origins |= by_verifier ? 1U << origin : 0U;

        route_origins |= by_route ? 1U << origin : 0U;

        if (by_route && !by_verifier)
        {
            std::cout << std::format("disagreement: window {} origin {} verifier refused", hex(window), origin) << '\n';

            ++disagreements;
        }

        if (!by_verifier || by_route)
        {
            continue;
        }

        const auto [witness, exhaustive]{lexer.window_counterexample(window, origin)};

        const auto confirmed{exhaustive && witness.empty()};

        const auto outcome_of{[confirmed, exhaustive, &witness] {
            if (confirmed)
            {
                return std::string{"certified"};
            }

            if (exhaustive)
            {
                return std::format("refuted by {}", hex(witness));
            }

            return std::string{"unsettled at the cap"};
        }};

        const auto outcome{outcome_of()};

        std::cout << std::format(
                             "window route refusal: window {} origin {} exact search {}", hex(window), origin, outcome)
                  << '\n';

        ++refusals;

        if (!confirmed)
        {
            ++unconfirmed;
        }
    }

    return decision;
}

/**
 * @brief Decides every distinct window of the corpus once, where for_each_window() first meets it, and adds up the
 *        tallies.
 * @param corpus The corpus.
 * @param verifier The armed-run verifier.
 * @param lexer The lexer of the same token set.
 * @return The decided windows and the tallies.
 */
Decided decide_all(const std::string_view corpus, const dfa::Verifier& verifier, const core::Lexer& lexer)
{
    Decided decided{};

    auto& [decisions, tally]{decided};

    auto& [pairs, verifier_pairs, window_route_pairs, disagreements, refusals, unconfirmed]{tally};

    const auto decide_new{[&](const std::size_t, const std::string_view window) {
        if (decisions.contains(window))
        {
            return;
        }

        const auto [origins, window_disagreements, window_refusals, window_unconfirmed]{
                decide(verifier, lexer, window)};

        const auto& [verifier_origins, route_origins]{origins};

        decisions.emplace(window, origins);

        pairs += window.size();

        verifier_pairs += static_cast<std::size_t>(std::popcount(verifier_origins));

        window_route_pairs += static_cast<std::size_t>(std::popcount(route_origins));

        disagreements += window_disagreements;

        refusals += window_refusals;

        unconfirmed += window_unconfirmed;
    }};

    for_each_window(corpus, decide_new);

    return decided;
}

/**
 * @brief Marks k + o for every occurrence at k of every window certified at o under one route.
 * @param corpus The corpus.
 * @param decisions The decided windows of the corpus.
 * @param route The route whose certified origins are marked.
 * @return The certified positions.
 */
Positions_t certified_positions(const std::string_view corpus, const Decisions_t& decisions, const Route route)
{
    Positions_t positions(corpus.size(), false);

    const auto origins_of{[route](const Certified_origins& origins) {
        const auto& [verifier, window_route]{origins};

        return route == Route::verifier ? verifier : window_route;
    }};

    const auto mark{[&positions, &decisions, &origins_of](const std::size_t k, const std::string_view window) {
        const auto origins{origins_of(decisions.at(window))};

        for (std::size_t origin{0}; origin < window.size(); ++origin)
        {
            const auto certified{((origins >> origin) & 1U) != 0};

            if (certified)
            {
                positions[k + origin] = true;
            }
        }
    }};

    for_each_window(corpus, mark);

    return positions;
}

/**
 * @brief Returns the longest run of consecutive bytes none of which is a certified position.
 * @param positions The certified positions.
 * @return The run's length in bytes.
 */
std::size_t longest_uncertified_stretch(const Positions_t& positions)
{
    std::size_t longest{0};

    std::size_t current{0};

    for (const auto certified : positions)
    {
        current = certified ? 0 : current + 1;

        longest = std::max(longest, current);
    }

    return longest;
}

/**
 * @brief Prints the supply of one route: the certified position count, the count per KiB and the longest stretch.
 * @param route The route's name as the lines carry it.
 * @param positions The route's certified positions.
 */
void print_supply(const std::string_view route, const Positions_t& positions)
{
    const auto count{static_cast<std::size_t>(std::ranges::count(positions, true))};

    const auto per_kib{
            positions.empty() ? 0.0 :
                                static_cast<double>(count) * bytes_per_kib / static_cast<double>(positions.size())};

    std::cout << std::format("{} certified positions: {}", route, count) << '\n';

    std::cout << std::format("{} certified positions per KiB: {:.2f}", route, per_kib) << '\n';

    const auto stretch{longest_uncertified_stretch(positions)};

    std::cout << std::format("{} longest uncertified stretch bytes: {}", route, stretch) << '\n';
}

} // namespace

/**
 * @brief Decides every window of one to three bytes of the corpus under both routes, prints the tallies, and when the
 *        routes agree prints each route's supply.
 * @param argc The argument count.
 * @param argv The corpus file.
 * @return EXIT_SUCCESS when the routes agree and every refusal is confirmed, EXIT_FAILURE on a usage error, an
 *         unreadable corpus, a disagreement or an unconfirmed refusal.
 */
int main(const int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: munch_verifier_supply <corpus file>\n";

        return EXIT_FAILURE;
    }

    const auto corpus_bytes{read_file(std::filesystem::path{argv[1]})};

    if (!corpus_bytes.has_value())
    {
        std::cerr << std::format("munch_verifier_supply: cannot read {}", argv[1]) << '\n';

        return EXIT_FAILURE;
    }

    const std::string_view corpus{*corpus_bytes};

    tools::probes::Builder_dbg builder{};

    figures::json(builder);

    const auto lexer{builder.build()};

    const auto verifier{dfa::armed_run(builder.dfa())};

    std::cout << std::format("corpus bytes: {}", corpus.size()) << '\n';

    std::cout << std::format("verifier states: {}", verifier.state_count()) << '\n';

    std::cout << std::format("verifier transitions: {}", verifier.transitions().size()) << '\n';

    const auto [decisions, tally]{decide_all(corpus, verifier, lexer)};

    const auto& [pairs, verifier_pairs, window_route_pairs, disagreements, refusals, unconfirmed]{tally};

    std::cout << std::format("windows examined: {}", decisions.size()) << '\n';

    std::cout << std::format("window origin pairs: {}", pairs) << '\n';

    std::cout << std::format("verifier pairs certified: {}", verifier_pairs) << '\n';

    std::cout << std::format("window route pairs certified: {}", window_route_pairs) << '\n';

    std::cout << std::format("disagreements: {}", disagreements) << '\n';

    std::cout << std::format("window route refusals: {}", refusals) << '\n';

    std::cout << std::format("window route refusals unconfirmed: {}", unconfirmed) << '\n';

    if (disagreements != 0 || unconfirmed != 0)
    {
        return EXIT_FAILURE;
    }

    const auto by_verifier{certified_positions(corpus, decisions, Route::verifier)};

    print_supply("verifier", by_verifier);

    const auto by_window_route{certified_positions(corpus, decisions, Route::window_route)};

    print_supply("window route", by_window_route);

    return EXIT_SUCCESS;
}
