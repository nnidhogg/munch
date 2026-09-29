// Holds the verifier's window certificates against the library's window route on a JSON corpus and measures the
// certified positions each supplies.
//
// The RFC 8259 lexer of the papers is built through figures::json, its armed-run verifier beside it. Every distinct
// byte window of length one to three occurring in the corpus is decided at every origin inside it, by miscovering()
// on the verifier and by Lexer::is_split_window() on the lexer. An origin the window route certifies and the verifier
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

namespace
{
using namespace munch;

/**
 * @brief The window lengths decided, one to this many bytes.
 */
constexpr std::size_t longest_window{3};

/**
 * @brief A builder whose compiled DFA is public.
 */
class Exposed_builder final : public core::Builder
{
public:
    using core::Builder::dfa;
};

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
    verifier,
    window_route,
};

/**
 * @brief The certified origins of one window under one route.
 * @param origins The window's certified origins under both routes.
 * @param route The route.
 * @return The origins as bits over the in-window origins.
 */
unsigned origins_of(const Certified_origins& origins, const Route route)
{
    return route == Route::verifier ? origins.verifier : origins.window_route;
}

/**
 * @brief The certified origins of every distinct window of the corpus, keyed by the window's bytes in the corpus.
 */
using Decisions_t = std::unordered_map<std::string_view, Certified_origins>;

/**
 * @brief Reads a whole file as bytes.
 * @param path The file's path.
 * @return The bytes, or std::nullopt when the file cannot be read.
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
 * @brief Spells a window as lowercase hexadecimal, two digits per byte.
 * @param window The window.
 * @return The hexadecimal text.
 */
std::string hex(const std::string_view window)
{
    std::string text;

    for (const auto byte : window)
    {
        text += std::format("{:02x}", static_cast<unsigned char>(byte));
    }

    return text;
}

/**
 * @brief One window decided at every origin inside it: the certified origins under each route and the tallies.
 */
struct Decision
{
    /**
     * @brief The certified origins under each route.
     */
    Certified_origins origins;

    /**
     * @brief The origins the window route certifies and the verifier refuses, and a window route origin outside the
     *        window.
     */
    std::size_t disagreements{};

    /**
     * @brief The origins the verifier certifies and the window route refuses.
     */
    std::size_t refusals{};

    /**
     * @brief The refusals whose exact search finds a counterexample or stops at its cap.
     */
    std::size_t unconfirmed{};
};

/**
 * @brief Decides one window at every origin inside it under both routes, printing each origin where they differ.
 *
 * An origin the verifier certifies and the window route refuses is held to Lexer::window_counterexample(), which
 * must exhaust its search without a witness.
 * @param verifier The armed-run verifier.
 * @param lexer The lexer of the same token set.
 * @param window The window, nonempty.
 * @return The decision.
 */
Decision decide(const dfa::Verifier& verifier, const core::Lexer& lexer, const std::string_view window)
{
    const auto route_origin{lexer.is_split_window(window)};

    Decision decision{};

    if (route_origin.has_value() && *route_origin >= window.size())
    {
        std::cout << std::format("disagreement: window {} window route origin {} outside", hex(window), *route_origin)
                  << '\n';

        ++decision.disagreements;
    }

    for (std::size_t origin{0}; origin < window.size(); ++origin)
    {
        const auto by_verifier{!dfa::miscovering(verifier, window, origin).has_value()};

        const auto by_route{route_origin == origin};

        decision.origins.verifier |= by_verifier ? 1U << origin : 0U;
        decision.origins.window_route |= by_route ? 1U << origin : 0U;

        if (by_route && !by_verifier)
        {
            std::cout << std::format("disagreement: window {} origin {} verifier refused", hex(window), origin) << '\n';

            ++decision.disagreements;
        }

        if (by_verifier && !by_route)
        {
            const auto exact{lexer.window_counterexample(window, origin)};

            const auto confirmed{exact.exhaustive && exact.witness.empty()};

            const auto outcome{
                    confirmed        ? std::string{"certified"} :
                    exact.exhaustive ? std::format("refuted by {}", hex(exact.witness)) :
                                       std::string{"unsettled at the cap"}};

            std::cout << std::format(
                                 "window route refusal: window {} origin {} exact search {}", hex(window), origin,
                                 outcome)
                      << '\n';

            ++decision.refusals;
            decision.unconfirmed += confirmed ? 0 : 1;
        }
    }

    return decision;
}

/**
 * @brief The certified positions of the corpus under one route, as one flag per byte.
 */
using Positions_t = std::vector<bool>;

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

    for (std::size_t length{1}; length <= longest_window && length <= corpus.size(); ++length)
    {
        for (std::size_t k{0}; k + length <= corpus.size(); ++k)
        {
            const auto origins{origins_of(decisions.at(corpus.substr(k, length)), route)};

            for (std::size_t origin{0}; origin < length; ++origin)
            {
                positions[k + origin] = positions[k + origin] || ((origins >> origin) & 1U) != 0;
            }
        }
    }

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
            positions.empty() ? 0.0 : static_cast<double>(count) * 1024.0 / static_cast<double>(positions.size())};

    std::cout << std::format("{} certified positions: {}", route, count) << '\n';
    std::cout << std::format("{} certified positions per KiB: {:.2f}", route, per_kib) << '\n';
    std::cout << std::format("{} longest uncertified stretch bytes: {}", route, longest_uncertified_stretch(positions))
              << '\n';
}

} // namespace

int main(const int argc, const char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: munch_verifier_supply <corpus file>\n";

        return 1;
    }

    const auto corpus_bytes{read_file(std::filesystem::path{argv[1]})};

    if (!corpus_bytes.has_value())
    {
        std::cerr << std::format("munch_verifier_supply: cannot read {}", argv[1]) << '\n';

        return 1;
    }

    const std::string_view corpus{*corpus_bytes};

    Exposed_builder builder;

    figures::json(builder);

    const auto lexer{builder.build()};

    const auto verifier{dfa::armed_run(builder.dfa())};

    std::cout << std::format("corpus bytes: {}", corpus.size()) << '\n';
    std::cout << std::format("verifier states: {}", verifier.state_count()) << '\n';
    std::cout << std::format("verifier transitions: {}", verifier.transitions().size()) << '\n';

    Decisions_t decisions;

    std::size_t pairs{0};

    std::size_t verifier_pairs{0};

    std::size_t window_route_pairs{0};

    std::size_t disagreements{0};

    std::size_t refusals{0};

    std::size_t unconfirmed{0};

    for (std::size_t length{1}; length <= longest_window && length <= corpus.size(); ++length)
    {
        for (std::size_t k{0}; k + length <= corpus.size(); ++k)
        {
            const auto window{corpus.substr(k, length)};

            if (decisions.contains(window))
            {
                continue;
            }

            const auto decision{decide(verifier, lexer, window)};

            decisions.emplace(window, decision.origins);
            pairs += length;
            verifier_pairs += static_cast<std::size_t>(std::popcount(decision.origins.verifier));
            window_route_pairs += static_cast<std::size_t>(std::popcount(decision.origins.window_route));
            disagreements += decision.disagreements;
            refusals += decision.refusals;
            unconfirmed += decision.unconfirmed;
        }
    }

    std::cout << std::format("windows examined: {}", decisions.size()) << '\n';
    std::cout << std::format("window origin pairs: {}", pairs) << '\n';
    std::cout << std::format("verifier pairs certified: {}", verifier_pairs) << '\n';
    std::cout << std::format("window route pairs certified: {}", window_route_pairs) << '\n';
    std::cout << std::format("disagreements: {}", disagreements) << '\n';
    std::cout << std::format("window route refusals: {}", refusals) << '\n';
    std::cout << std::format("window route refusals unconfirmed: {}", unconfirmed) << '\n';

    if (disagreements != 0 || unconfirmed != 0)
    {
        return 1;
    }

    print_supply("verifier", certified_positions(corpus, decisions, Route::verifier));
    print_supply("window route", certified_positions(corpus, decisions, Route::window_route));

    return 0;
}
