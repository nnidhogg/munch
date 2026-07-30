#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <re2/re2.h>

#include <boost/regex.hpp>
#include <ctre.hpp>
#include <lexertl/generator.hpp>
#include <lexertl/lookup.hpp>
#include <lexertl/state_machine.hpp>

#include "munch/tools/benchmark/harness.hpp"

namespace
{
using namespace munch::tools::benchmark;

/**
 * @brief The token set of harness.hpp as one alternation per kind, for the engines that take a regex string.
 *
 * Multi-character operators precede their single-character prefixes and keywords precede identifiers, so engines
 * with first-match alternation semantics produce the same tokenization as munch's longest-match, priority-resolved
 * one on the generated corpus.
 */
constexpr char pattern[]{
        "([ \\t\\n]+)|(if|else|while|return|int)|([A-Za-z_][A-Za-z0-9_]*)|([0-9]+)"
        "|(==|!=|<=|>=|[\\-+*/=<>])|([(){};,])"};

/**
 * @brief The token kind each capture group of the pattern corresponds to, indexed by group number minus one.
 */
constexpr std::array token_of_group{Token::whitespace, Token::keyword,   Token::identifier,
                                    Token::number,     Token::operator_, Token::punctuation};

/**
 * @brief The outcome of tokenizing the whole input once, zeroed if the input was rejected.
 *
 * The checksum folds the matched token kinds in order, a per-pass sanity signal inside the timed loops; the proof
 * that engines agree token for token is the exact kind-and-length stream comparison run once before timing.
 */
struct Tally
{
    std::size_t tokens;

    std::size_t checksum;

    bool operator==(const Tally&) const = default;
};

/**
 * @brief One engine's full tokenization as (kind, length) pairs, collected once per corpus for exact validation.
 */
using Stream_t = std::vector<std::pair<std::size_t, std::size_t>>;

/**
 * @brief Runs a scan with the tally-folding sink every timed scenario measures.
 * @param scan Callable invoking its sink as sink(kind, length) per token and returning false on rejection.
 */
template <typename Scan>
Tally tally_of(Scan&& scan)
{
    Tally tally{};

    const auto ok{scan([&tally](const std::size_t kind, const std::size_t) {
        tally.checksum = tally.checksum * 31 + kind;

        ++tally.tokens;
    })};

    return ok ? tally : Tally{};
}

/**
 * @brief Runs a scan collecting the exact token stream, or nullopt when the engine rejected the input.
 */
template <typename Scan>
std::optional<Stream_t> stream_of(Scan&& scan)
{
    Stream_t stream;

    const auto ok{
            scan([&stream](const std::size_t kind, const std::size_t length) { stream.emplace_back(kind, length); })};

    return ok ? std::optional<Stream_t>{std::move(stream)} : std::nullopt;
}

/**
 * @brief Tokenizes the whole input once through the munch Lexer's batch entry point, providing the reference.
 */
template <typename Sink>
bool scan_munch(const munch::core::Lexer& lexer, const std::string& input, Sink&& sink)
{
    const auto consumed{lexer.tokenize_all<Token>(input, [&sink](const Token token, const std::size_t length) {
        sink(static_cast<std::size_t>(token), length);
    })};

    return consumed == input.size();
}

/**
 * @brief Raises 31 to the given power with wraparound, for splicing per-chunk checksums in stream order.
 */
std::size_t pow31(std::size_t exponent)
{
    std::size_t result{1};

    std::size_t base{31};

    for (; exponent != 0; exponent >>= 1U)
    {
        if ((exponent & 1U) != 0)
        {
            result *= base;
        }

        base *= base;
    }

    return result;
}

/**
 * @brief Tokenizes the input in parallel chunks through the library's tokenize_all_parallel entry point.
 *
 * The per-chunk checksums are spliced in stream order, so the tally equals the serial scan's exactly when the
 * chunked token stream is identical, and the validation against the reference stays as strict as for every other
 * scenario.
 */
Tally run_munch_threaded(const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    // One tally per cache line, as the entry point's contract asks: adjacent per-chunk tallies would false-share
    // across the worker threads.
    struct alignas(64) Padded
    {
        Tally tally;
    };

    std::vector<Padded> tallies(chunks);

    const auto consumed{lexer.tokenize_all_parallel<Token>(
            input, chunks, [&tallies](const std::size_t chunk, const Token token, const std::size_t) {
                auto& tally{tallies[chunk].tally};

                tally.checksum = tally.checksum * 31 + static_cast<std::size_t>(token);

                ++tally.tokens;
            })};

    const auto boundaries{lexer.chunk_boundaries(input, chunks)};

    Tally total{};

    for (std::size_t chunk{0}; chunk < consumed.size(); ++chunk)
    {
        if (consumed[chunk] != boundaries[chunk + 1] - boundaries[chunk])
        {
            return {};
        }

        total.checksum = total.checksum * pow31(tallies[chunk].tally.tokens) + tallies[chunk].tally.checksum;

        total.tokens += tallies[chunk].tally.tokens;
    }

    return total;
}

/**
 * @brief Collects the parallel scan's exact token stream, chunks spliced in input order.
 */
std::optional<Stream_t> stream_munch_threaded(
        const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    std::vector<Stream_t> streams(chunks);

    const auto consumed{lexer.tokenize_all_parallel<Token>(
            input, chunks, [&streams](const std::size_t chunk, const Token token, const std::size_t length) {
                streams[chunk].emplace_back(static_cast<std::size_t>(token), length);
            })};

    const auto boundaries{lexer.chunk_boundaries(input, chunks)};

    Stream_t total;

    for (std::size_t chunk{0}; chunk < consumed.size(); ++chunk)
    {
        if (consumed[chunk] != boundaries[chunk + 1] - boundaries[chunk])
        {
            return std::nullopt;
        }

        total.insert(total.end(), streams[chunk].cbegin(), streams[chunk].cend());
    }

    return total;
}

/**
 * @brief Builds the lexertl state machine for the shared token set.
 *
 * lexertl is munch's nearest relative in the comparison: a lexer built at run time from rules and compiled to a
 * DFA. Rule identifiers are the harness Token values, so its tally is directly comparable, and keywords precede
 * the identifier rule because lexertl breaks equal-length matches by rule order where munch uses priorities.
 */
lexertl::state_machine build_lexertl()
{
    lexertl::rules rules;

    rules.push("[ \t\n]+", 1);
    rules.push("if|else|while|return|int", 4);
    rules.push("[A-Za-z_][A-Za-z0-9_]*", 2);
    rules.push("[0-9]+", 3);
    rules.push("==|!=|<=|>=|[-+*/=<>]", 5);
    rules.push("[(){};,]", 6);

    lexertl::state_machine sm;

    lexertl::generator::build(rules, sm);

    sm.minimise();

    return sm;
}

/**
 * @brief Tokenizes the whole input once through a lexertl state machine.
 */
template <typename Sink>
bool scan_lexertl(const lexertl::state_machine& sm, const std::string& input, Sink&& sink)
{
    lexertl::match_results<std::string::const_iterator> results(input.cbegin(), input.cend());

    for (;;)
    {
        lexertl::lookup(sm, results);

        if (results.id == 0)
        {
            return true;
        }

        if (results.id == results.npos() || results.first == results.second)
        {
            return false;
        }

        sink(static_cast<std::size_t>(results.id), static_cast<std::size_t>(results.second - results.first));
    }
}

/**
 * @brief Tokenizes the whole input once through a CTRE pattern compiled from the shared alternation.
 */
template <typename Sink>
bool scan_ctre(const std::string& input, Sink&& sink)
{
    static constexpr auto matcher{ctre::starts_with<ctll::fixed_string{pattern}>};

    std::string_view remaining{input};

    while (!remaining.empty())
    {
        const auto match{matcher(remaining)};

        if (!match || match.to_view().empty())
        {
            return false;
        }

        const auto kind{[&match] {
            if (match.template get<1>())
                return Token::whitespace;
            if (match.template get<2>())
                return Token::keyword;
            if (match.template get<3>())
                return Token::identifier;
            if (match.template get<4>())
                return Token::number;
            if (match.template get<5>())
                return Token::operator_;
            return Token::punctuation;
        }};

        sink(static_cast<std::size_t>(kind()), match.to_view().size());

        remaining.remove_prefix(match.to_view().size());
    }

    return true;
}

/**
 * @brief Tokenizes the whole input once through RE2, consuming anchored matches with one capture per kind.
 */
template <typename Sink>
bool scan_re2(const RE2& regex, const std::string& input, Sink&& sink)
{
    absl::string_view remaining{input};

    std::array<absl::string_view, token_of_group.size()> groups{};

    while (!remaining.empty())
    {
        const auto before{remaining.size()};

        if (!RE2::Consume(&remaining, regex, &groups[0], &groups[1], &groups[2], &groups[3], &groups[4], &groups[5]) ||
            remaining.size() == before)
        {
            return false;
        }

        std::size_t kind{0};

        for (std::size_t group{0}; group < groups.size(); ++group)
        {
            // A group that did not participate in the match is reset to a null view.
            if (groups[group].data() != nullptr)
            {
                kind = static_cast<std::size_t>(token_of_group[group]);

                break;
            }
        }

        sink(kind, before - remaining.size());
    }

    return true;
}

/**
 * @brief Tokenizes the whole input once through a std::regex or boost::regex, which share one API shape.
 */
template <typename Regex, typename Match, auto continuous, typename Sink>
bool scan_backtracker(const Regex& regex, const std::string& input, Sink&& sink)
{
    Match match;

    auto it{input.cbegin()};

    while (it != input.cend())
    {
        if (!regex_search(it, input.cend(), match, regex, continuous) || match.length(0) == 0)
        {
            return false;
        }

        std::size_t kind{0};

        for (std::size_t group{1}; group <= token_of_group.size(); ++group)
        {
            if (match[group].matched)
            {
                kind = static_cast<std::size_t>(token_of_group[group - 1]);

                break;
            }
        }

        sink(kind, static_cast<std::size_t>(match.length(0)));

        it += match.length(0);
    }

    return true;
}

/**
 * @brief Owns a JIT-compiled PCRE2 pattern with its match data and runs tokenization passes through it.
 */
class Pcre2
{
public:
    explicit Pcre2(const char* expression)
    {
        int error{0};

        PCRE2_SIZE offset{0};

        code_ = pcre2_compile(
                reinterpret_cast<PCRE2_SPTR>(expression), PCRE2_ZERO_TERMINATED, PCRE2_ANCHORED, &error, &offset,
                nullptr);

        if (code_ != nullptr && pcre2_jit_compile(code_, PCRE2_JIT_COMPLETE) == 0)
        {
            data_ = pcre2_match_data_create_from_pattern(code_, nullptr);
        }
    }

    Pcre2(const Pcre2&) = delete;

    Pcre2& operator=(const Pcre2&) = delete;

    ~Pcre2()
    {
        pcre2_match_data_free(data_);

        pcre2_code_free(code_);
    }

    [[nodiscard]] bool ok() const { return data_ != nullptr; }

    /**
     * @brief Tokenizes the whole input once, matching anchored at the current offset.
     */
    template <typename Sink>
    bool scan(const std::string& input, Sink&& sink) const
    {
        std::size_t offset{0};

        const auto* subject{reinterpret_cast<PCRE2_SPTR>(input.data())};

        while (offset < input.size())
        {
            // pcre2_jit_match skips the per-call validation of pcre2_match; ok() guarantees the pattern is
            // JIT-compiled, which is the one precondition the fast path does not re-check.
            const int rc{pcre2_jit_match(code_, subject, input.size(), offset, 0, data_, nullptr)};

            const auto* ovector{pcre2_get_ovector_pointer(data_)};

            // The alternatives are exclusive, so the highest captured group is the one that matched.
            if (rc < 2 || ovector[1] == ovector[0])
            {
                return false;
            }

            const auto group{static_cast<std::size_t>(rc) - 2};

            sink(static_cast<std::size_t>(token_of_group[group]), ovector[1] - ovector[0]);

            offset += ovector[1] - ovector[0];
        }

        return true;
    }

private:
    pcre2_code* code_{nullptr};

    pcre2_match_data* data_{nullptr};
};

} // namespace

/**
 * @brief Measures tokenization throughput of munch against common regex engines on identical generated pseudo-code.
 *
 * Every engine extracts the same information per token, its kind and length, and every engine's full tokenization
 * is validated against munch's before anything is timed.
 * Usage: munch_benchmark_compare [input size in MiB] [passes]
 */
int main(const int argc, const char** argv)
{
    const std::size_t mebibytes{argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8};

    const int passes{argc > 2 ? std::atoi(argv[2]) : 15};

    if (mebibytes == 0 || passes <= 0)
    {
        std::printf("usage: munch_benchmark_compare [input size in MiB > 0] [passes > 0]\n");

        return EXIT_FAILURE;
    }

    // None of these may start with a keyword: the pattern above lists keywords before identifiers, so an entry like
    // "integer" would tokenize as "int" + "eger" under first-match alternation while munch's longest match keeps it
    // whole. The tally validation below fails loudly if this constraint is broken.
    constexpr const char* identifiers[]{"foo", "bar_baz", "counter", "x1", "value2", "tmp"};

    // Two corpus shapes falsify each other's conclusions: dense punishes per-token overhead, source shows how
    // the gaps change once realistic token lengths amortize it.
    const struct
    {
        const char* name;

        std::string input;
    } corpora[]{
            {.name = "dense", .input = generate_input(mebibytes << 20U, identifiers)},
            {.name = "source", .input = generate_source_input(mebibytes << 20U)},
    };

    const auto lexer{build_lexer(false)};

    const auto lexertl_sm{build_lexertl()};

    const std::regex std_regex{pattern, std::regex::optimize};

    const boost::regex boost_regex{pattern};

    const RE2 re2{pattern};

    const Pcre2 pcre2{pattern};

    if (!re2.ok() || !pcre2.ok())
    {
        std::printf("failed to compile the comparison pattern\n");

        return EXIT_FAILURE;
    }

    struct Scenario
    {
        const char* name;

        std::function<Tally(const std::string&)> run;

        std::function<std::optional<Stream_t>(const std::string&)> stream;
    };

    const auto scan_boost{[&](const std::string& input, auto&& sink) {
        return scan_backtracker<boost::regex, boost::smatch, boost::regex_constants::match_continuous>(
                boost_regex, input, sink);
    }};

    const auto scan_std{[&](const std::string& input, auto&& sink) {
        return scan_backtracker<std::regex, std::smatch, std::regex_constants::match_continuous>(
                std_regex, input, sink);
    }};

    const Scenario scenarios[]{
            {.name = "munch",
             .run =
                     [&](const std::string& input) {
                         return tally_of([&](auto&& s) { return scan_munch(lexer, input, s); });
                     },
             .stream =
                     [&](const std::string& input) {
                         return stream_of([&](auto&& s) { return scan_munch(lexer, input, s); });
                     }},
            {.name = "munch-mt4",
             .run = [&](const std::string& input) { return run_munch_threaded(lexer, input, 4); },
             .stream = [&](const std::string& input) { return stream_munch_threaded(lexer, input, 4); }},
            {.name = "munch-mt8",
             .run = [&](const std::string& input) { return run_munch_threaded(lexer, input, 8); },
             .stream = [&](const std::string& input) { return stream_munch_threaded(lexer, input, 8); }},
            {.name = "lexertl",
             .run =
                     [&](const std::string& input) {
                         return tally_of([&](auto&& s) { return scan_lexertl(lexertl_sm, input, s); });
                     },
             .stream =
                     [&](const std::string& input) {
                         return stream_of([&](auto&& s) { return scan_lexertl(lexertl_sm, input, s); });
                     }},
            {.name = "ctre",
             .run = [&](const std::string& input) { return tally_of([&](auto&& s) { return scan_ctre(input, s); }); },
             .stream =
                     [&](const std::string& input) {
                         return stream_of([&](auto&& s) { return scan_ctre(input, s); });
                     }},
            {.name = "pcre2-jit",
             .run = [&](const std::string& input) { return tally_of([&](auto&& s) { return pcre2.scan(input, s); }); },
             .stream =
                     [&](const std::string& input) {
                         return stream_of([&](auto&& s) { return pcre2.scan(input, s); });
                     }},
            {.name = "re2",
             .run =
                     [&](const std::string& input) {
                         return tally_of([&](auto&& s) { return scan_re2(re2, input, s); });
                     },
             .stream =
                     [&](const std::string& input) {
                         return stream_of([&](auto&& s) { return scan_re2(re2, input, s); });
                     }},
            {.name = "boost-regex",
             .run = [&](const std::string& input) { return tally_of([&](auto&& s) { return scan_boost(input, s); }); },
             .stream =
                     [&](const std::string& input) {
                         return stream_of([&](auto&& s) { return scan_boost(input, s); });
                     }},
            {.name = "std-regex",
             .run = [&](const std::string& input) { return tally_of([&](auto&& s) { return scan_std(input, s); }); },
             .stream =
                     [&](const std::string& input) { return stream_of([&](auto&& s) { return scan_std(input, s); }); }},
    };

    bool ok{true};

    for (const auto& [corpus, input] : corpora)
    {
        // The exact comparison runs once per corpus, before timing: every engine must reproduce munch's token
        // stream to the kind and the length, which is what licenses comparing their throughputs at all. The timed
        // loops keep only the light count-and-kind tally as a per-pass sanity signal.
        const auto reference{stream_of([&](auto&& s) { return scan_munch(lexer, input, s); })};

        if (!reference || reference->empty())
        {
            std::printf("munch rejected the %s corpus\n", corpus);

            return EXIT_FAILURE;
        }

        for (const auto& scenario : scenarios)
        {
            const auto stream{scenario.stream(input)};

            if (!stream)
            {
                std::printf("%s/%s: rejected the input\n", scenario.name, corpus);

                ok = false;

                continue;
            }

            if (*stream != *reference)
            {
                std::size_t index{0};

                while (index < stream->size() && index < reference->size() && (*stream)[index] == (*reference)[index])
                {
                    ++index;
                }

                std::printf(
                        "%s/%s: token stream diverges from munch at token %zu (%zu tokens vs %zu)\n", scenario.name,
                        corpus, index, stream->size(), reference->size());

                ok = false;
            }
        }

        if (!ok)
        {
            return EXIT_FAILURE;
        }

        std::printf(
                "corpus %s: %.2f bytes per token\n", corpus,
                static_cast<double>(input.size()) / static_cast<double>(reference->size()));

        for (const auto& scenario : scenarios)
        {
            ok = measure(
                         scenario.name, input.size(), passes, [&scenario, &input] { return scenario.run(input); },
                         [](const Tally& tally) { return tally.tokens; }) &&
                 ok;
        }
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
