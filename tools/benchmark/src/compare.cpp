#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <regex>
#include <string>
#include <string_view>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <re2/re2.h>

#include <boost/regex.hpp>
#include <ctre.hpp>

#include "harness.hpp"

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
 * The checksum folds the matched token kinds in order, so two engines agree on every token's kind and boundary
 * exactly when their tallies compare equal; a mere permutation of the same kinds does not collide.
 */
struct Tally
{
    std::size_t tokens;

    std::size_t checksum;

    bool operator==(const Tally&) const = default;
};

/**
 * @brief Tokenizes the whole input once through the munch Lexer's batch entry point, providing the reference tally.
 */
Tally run_munch(const munch::core::Lexer& lexer, const std::string& input)
{
    Tally tally{};

    const auto consumed{lexer.tokenize_all<Token>(input, [&tally](const Token token, const std::size_t) {
        tally.checksum = tally.checksum * 31 + static_cast<std::size_t>(token);

        ++tally.tokens;
    })};

    return consumed == input.size() ? tally : Tally{};
}

/**
 * @brief Tokenizes the whole input once through a CTRE pattern compiled from the shared alternation.
 */
Tally run_ctre(const std::string& input)
{
    static constexpr auto matcher{ctre::starts_with<ctll::fixed_string{pattern}>};

    Tally tally{};

    std::string_view remaining{input};

    while (!remaining.empty())
    {
        const auto match{matcher(remaining)};

        if (!match || match.to_view().empty())
        {
            return {};
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

        tally.checksum = tally.checksum * 31 + static_cast<std::size_t>(kind());

        ++tally.tokens;

        remaining.remove_prefix(match.to_view().size());
    }

    return tally;
}

/**
 * @brief Tokenizes the whole input once through RE2, consuming anchored matches with one capture per kind.
 */
Tally run_re2(const RE2& regex, const std::string& input)
{
    Tally tally{};

    absl::string_view remaining{input};

    std::array<absl::string_view, token_of_group.size()> groups{};

    while (!remaining.empty())
    {
        const auto before{remaining.size()};

        if (!RE2::Consume(&remaining, regex, &groups[0], &groups[1], &groups[2], &groups[3], &groups[4], &groups[5]) ||
            remaining.size() == before)
        {
            return {};
        }

        for (std::size_t group{0}; group < groups.size(); ++group)
        {
            // A group that did not participate in the match is reset to a null view.
            if (groups[group].data() != nullptr)
            {
                tally.checksum = tally.checksum * 31 + static_cast<std::size_t>(token_of_group[group]);

                break;
            }
        }

        ++tally.tokens;
    }

    return tally;
}

/**
 * @brief Tokenizes the whole input once through a std::regex or boost::regex, which share one API shape.
 */
template <typename Regex, typename Match, auto continuous>
Tally run_backtracker(const Regex& regex, const std::string& input)
{
    Tally tally{};

    Match match;

    auto it{input.cbegin()};

    while (it != input.cend())
    {
        if (!regex_search(it, input.cend(), match, regex, continuous) || match.length(0) == 0)
        {
            return {};
        }

        for (std::size_t group{1}; group <= token_of_group.size(); ++group)
        {
            if (match[group].matched)
            {
                tally.checksum = tally.checksum * 31 + static_cast<std::size_t>(token_of_group[group - 1]);

                break;
            }
        }

        ++tally.tokens;

        it += match.length(0);
    }

    return tally;
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
    Tally run(const std::string& input) const
    {
        Tally tally{};

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
                return {};
            }

            const auto group{static_cast<std::size_t>(rc) - 2};

            tally.checksum = tally.checksum * 31 + static_cast<std::size_t>(token_of_group[group]);

            ++tally.tokens;

            offset += ovector[1] - ovector[0];
        }

        return tally;
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

    // None of these may start with a keyword: the pattern above lists keywords before identifiers, so an entry like
    // "integer" would tokenize as "int" + "eger" under first-match alternation while munch's longest match keeps it
    // whole. The tally validation below fails loudly if this constraint is broken.
    constexpr const char* identifiers[]{"foo", "bar_baz", "counter", "x1", "value2", "tmp"};

    const auto input{generate_input(mebibytes << 20U, identifiers)};

    const auto lexer{build_lexer(false)};

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

        std::function<Tally()> run;
    };

    const Scenario scenarios[]{
            {.name = "munch", .run = [&] { return run_munch(lexer, input); }},
            {.name = "ctre", .run = [&] { return run_ctre(input); }},
            {.name = "pcre2-jit", .run = [&] { return pcre2.run(input); }},
            {.name = "re2", .run = [&] { return run_re2(re2, input); }},
            {.name = "boost-regex",
             .run =
                     [&] {
                         return run_backtracker<boost::regex, boost::smatch, boost::regex_constants::match_continuous>(
                                 boost_regex, input);
                     }},
            {.name = "std-regex",
             .run =
                     [&] {
                         return run_backtracker<std::regex, std::smatch, std::regex_constants::match_continuous>(
                                 std_regex, input);
                     }},
    };

    const auto reference{run_munch(lexer, input)};

    if (reference.tokens == 0)
    {
        std::printf("munch rejected the generated input\n");

        return EXIT_FAILURE;
    }

    bool ok{true};

    for (const auto& scenario : scenarios)
    {
        if (const auto tally{scenario.run()}; !(tally == reference))
        {
            std::printf(
                    "%s: tokenization disagrees with munch (%zu tokens, checksum %zu; expected %zu, %zu)\n",
                    scenario.name, tally.tokens, tally.checksum, reference.tokens, reference.checksum);

            ok = false;
        }
    }

    if (!ok)
    {
        return EXIT_FAILURE;
    }

    for (const auto& scenario : scenarios)
    {
        ok = measure(scenario.name, input.size(), passes, [&scenario] { return scenario.run().tokens; }) && ok;
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
