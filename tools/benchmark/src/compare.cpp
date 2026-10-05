#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <span>
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

#include "munch/core/lexer.hpp"
#include "munch/core/mode_builder.hpp"
#include "munch/core/mode_lexer.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/benchmark/harness.hpp"

namespace
{
using namespace munch::tools::benchmark;

/**
 * @brief The token set of harness.hpp as one alternation per kind, for the engines that take a regex string.
 *
 * Multi-character operators precede their single-character prefixes and keywords precede identifiers, so engines with
 * first-match alternation semantics produce the same tokenization as munch's longest-match, priority-resolved one on
 * the generated corpus.
 */
constexpr char pattern[]{R"re(([ \t\n]+)|(if|else|while|return|int)|([A-Za-z_][A-Za-z0-9_]*)|([0-9]+))re"
                         R"re(|(==|!=|<=|>=|[\-+*/=<>])|([(){};,]))re"};

/**
 * @brief The token kind each capture group of the pattern corresponds to, indexed by group number minus one.
 */
constexpr std::array token_of_group{Token::whitespace, Token::keyword,   Token::identifier,
                                    Token::number,     Token::operator_, Token::punctuation};

/**
 * @brief The base of the polynomial hash a tally folds its token kinds into.
 */
constexpr std::size_t checksum_base{31};

/**
 * @brief The deepest a comment of the nested corpus nests, each comment drawn one to this deep.
 */
constexpr unsigned deepest_nesting{4};

/**
 * @brief The mode both munch mode grammars scan code in: mode 0, where a scan begins.
 */
constexpr std::size_t code_mode{0};

/**
 * @brief The match results lexertl scans a machine without start-state pushes with.
 */
using Flat_results = lexertl::match_results<std::string::const_iterator>;

/**
 * @brief The match results lexertl scans a machine whose rules push and pop start states with: the stack the pushes use
 *        lives on this type, and lookup() dereferences it whether or not the plain type has one.
 */
using Nested_results = lexertl::recursive_match_results<std::string::const_iterator>;

/**
 * @brief The outcome of tokenizing the whole input once, zeroed if the input was rejected.
 *
 * The checksum folds the matched token kinds in order, a per-pass sanity signal inside the timed loops; the proof that
 * engines agree token for token is the exact kind-and-length stream comparison run once before timing.
 */
struct Tally
{
    /**
     * @brief Equal when both counted the same tokens with the same checksum.
     */
    bool operator==(const Tally&) const = default;

    /**
     * @brief The tokens matched.
     */
    std::size_t tokens{0};

    /**
     * @brief The token kinds folded in order as a polynomial hash of base checksum_base.
     */
    std::size_t checksum{0};
};

/**
 * @brief One engine's full tokenization as (kind, length) pairs, collected once per corpus for exact validation.
 */
using Stream_t = std::vector<std::pair<std::size_t, std::size_t>>;

/**
 * @brief One engine: its timed run and its exact stream over a corpus.
 */
struct Engine
{
    /**
     * @brief The engine's name.
     */
    std::string_view name{};

    /**
     * @brief One timed pass over a corpus.
     */
    std::function<Tally(const std::string&)> run{};

    /**
     * @brief The exact token stream over a corpus, std::nullopt on rejection.
     */
    std::function<std::optional<Stream_t>(const std::string&)> stream{};
};

/**
 * @brief The token kinds of the mode comparison, shared by both engines so their streams are comparable.
 */
enum class Mode_token : std::size_t
{
    /**
     * @brief A whitespace run.
     */
    whitespace = 1,

    /**
     * @brief An identifier.
     */
    identifier,

    /**
     * @brief A number.
     */
    number,

    /**
     * @brief An operator.
     */
    op,

    /**
     * @brief A punctuation byte.
     */
    punctuation,

    /**
     * @brief A string's quote.
     */
    quote,

    /**
     * @brief A string's body, or a whole string in the flat grammar.
     */
    text,

    /**
     * @brief A comment opener.
     */
    comment_open,

    /**
     * @brief A comment closer.
     */
    comment_close,

    /**
     * @brief A comment's body.
     */
    comment_text
};

/**
 * @brief Runs a scan with the tally-folding sink every timed scenario measures.
 * @tparam Scan The scan's type.
 * @param scan Callable invoking its sink as sink(kind, length) per token and returning false on rejection.
 * @return The tally, zeroed when the scan rejected the input.
 */
template <typename Scan>
Tally tally_of(Scan&& scan)
{
    Tally tally{};

    const auto ok{scan([&tally](const std::size_t kind, const std::size_t) {
        tally.checksum = tally.checksum * checksum_base + kind;

        ++tally.tokens;
    })};

    return ok ? tally : Tally{};
}

/**
 * @brief Runs a scan collecting the exact token stream.
 * @tparam Scan The scan's type.
 * @param scan Callable invoking its sink as sink(kind, length) per token and returning false on rejection.
 * @return The stream, std::nullopt when the engine rejected the input.
 */
template <typename Scan>
std::optional<Stream_t> stream_of(Scan&& scan)
{
    Stream_t stream{};

    const auto append{
            [&stream](const std::size_t kind, const std::size_t length) { stream.emplace_back(kind, length); }};

    const auto ok{scan(append)};

    return ok ? std::optional<Stream_t>{std::move(stream)} : std::nullopt;
}

/**
 * @brief Tokenizes the whole input once through the munch Lexer's batch entry point, providing the reference.
 * @tparam Sink The sink's type.
 * @param lexer The lexer.
 * @param input The input.
 * @param sink Receives each token's kind and length.
 * @return True when the whole input was consumed.
 */
template <typename Sink>
bool scan_munch(const munch::core::Lexer& lexer, const std::string& input, Sink&& sink)
{
    const auto consumed{lexer.tokenize_all<Token>(
            input, [&sink](const Token token, const std::size_t length) { sink(std::to_underlying(token), length); })};

    return consumed == input.size();
}

/**
 * @brief Raises checksum_base to the given power with wraparound, for splicing per-chunk checksums in stream order.
 * @param exponent The power.
 * @return checksum_base to the power, modulo 2 to the width of std::size_t.
 */
std::size_t checksum_power(std::size_t exponent)
{
    std::size_t result{1};

    std::size_t base{checksum_base};

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
 * The per-chunk checksums are spliced in stream order, so the tally equals the serial scan's exactly when the chunked
 * token stream is identical, and the validation against the reference stays as strict as for every other scenario.
 * @param lexer The lexer.
 * @param input The input.
 * @param chunks The chunks the input is divided into.
 * @return The spliced tally, zeroed when a chunk was rejected.
 */
Tally run_munch_threaded(const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    std::vector<Padded<Tally>> tallies(chunks);

    const auto consumed{lexer.tokenize_all_parallel<Token>(
            input, chunks, [&tallies](const std::size_t chunk, const Token token, const std::size_t) {
                auto& tally{tallies[chunk].value};

                tally.checksum = tally.checksum * checksum_base + std::to_underlying(token);

                ++tally.tokens;
            })};

    // Chunks are disjoint and a scan cannot leave its chunk, so summing to the input's size means full consumption.
    const auto total_consumed{std::ranges::fold_left(consumed, std::size_t{0}, std::plus{})};

    if (total_consumed != input.size())
    {
        return {};
    }

    Tally total{};

    for (std::size_t chunk{0}; chunk < consumed.size(); ++chunk)
    {
        const auto& [tokens, checksum]{tallies[chunk].value};

        total.checksum = total.checksum * checksum_power(tokens) + checksum;

        total.tokens += tokens;
    }

    return total;
}

/**
 * @brief Collects the parallel scan's exact token stream, chunks spliced in input order.
 * @param lexer The lexer.
 * @param input The input.
 * @param chunks The chunks the input is divided into.
 * @return The stream, std::nullopt when a chunk was rejected.
 */
std::optional<Stream_t> stream_munch_threaded(
        const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    std::vector<Stream_t> streams(chunks);

    const auto append{[&streams](const std::size_t chunk, const Token token, const std::size_t length) {
        streams[chunk].emplace_back(std::to_underlying(token), length);
    }};

    const auto consumed{lexer.tokenize_all_parallel<Token>(input, chunks, append)};

    const auto boundaries{lexer.chunk_boundaries(input, chunks)};

    Stream_t total{};

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
 * lexertl is munch's nearest relative in the comparison: a lexer built at run time from rules and compiled to a DFA.
 * Rule identifiers are the harness Token values, so its tally is directly comparable, and keywords precede the
 * identifier rule because lexertl breaks equal-length matches by rule order where munch uses priorities.
 * @return The minimised state machine.
 */
lexertl::state_machine build_lexertl()
{
    lexertl::rules rules{};

    rules.push("[ \t\n]+", std::to_underlying(Token::whitespace));

    rules.push("if|else|while|return|int", std::to_underlying(Token::keyword));

    rules.push("[A-Za-z_][A-Za-z0-9_]*", std::to_underlying(Token::identifier));

    rules.push("[0-9]+", std::to_underlying(Token::number));

    rules.push("==|!=|<=|>=|[-+*/=<>]", std::to_underlying(Token::operator_));

    rules.push("[(){};,]", std::to_underlying(Token::punctuation));

    lexertl::state_machine machine{};

    lexertl::generator::build(rules, machine);

    machine.minimise();

    return machine;
}

/**
 * @brief Tokenizes the whole input once through a lexertl state machine.
 * @tparam Results The match results type, Nested_results for a machine whose rules push and pop start states and
 *         Flat_results otherwise.
 * @tparam Sink The sink's type.
 * @param machine The state machine.
 * @param input The input.
 * @param sink Receives each token's kind and length.
 * @return True when the whole input was consumed.
 */
template <typename Results, typename Sink>
bool scan_lexertl(const lexertl::state_machine& machine, const std::string& input, Sink&& sink)
{
    Results results(input.cbegin(), input.cend());

    for (;;)
    {
        lexertl::lookup(machine, results);

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
 * @tparam Sink The sink's type.
 * @param input The input.
 * @param sink Receives each token's kind and length.
 * @return True when the whole input was consumed.
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
            {
                return Token::whitespace;
            }

            if (match.template get<2>())
            {
                return Token::keyword;
            }

            if (match.template get<3>())
            {
                return Token::identifier;
            }

            if (match.template get<4>())
            {
                return Token::number;
            }

            if (match.template get<5>())
            {
                return Token::operator_;
            }

            return Token::punctuation;
        }};

        sink(std::to_underlying(kind()), match.to_view().size());

        remaining.remove_prefix(match.to_view().size());
    }

    return true;
}

/**
 * @brief Tokenizes the whole input once through RE2, consuming anchored matches with one capture per kind.
 * @tparam Sink The sink's type.
 * @param regex The compiled pattern.
 * @param input The input.
 * @param sink Receives each token's kind and length.
 * @return True when the whole input was consumed.
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
                kind = std::to_underlying(token_of_group[group]);

                break;
            }
        }

        sink(kind, before - remaining.size());
    }

    return true;
}

/**
 * @brief Tokenizes the whole input once through a std::regex or boost::regex, which share one API shape.
 * @tparam Regex The engine's regex type.
 * @tparam Match The engine's match type.
 * @tparam continuous The engine's match-continuous flag.
 * @tparam Sink The sink's type.
 * @param regex The compiled pattern.
 * @param input The input.
 * @param sink Receives each token's kind and length.
 * @return True when the whole input was consumed.
 */
template <typename Regex, typename Match, auto continuous, typename Sink>
bool scan_backtracker(const Regex& regex, const std::string& input, Sink&& sink)
{
    Match match{};

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
                kind = std::to_underlying(token_of_group[group - 1]);

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
    /**
     * @brief Compiles a pattern anchored and JIT-compiles it; ok() says whether both succeeded.
     * @param expression The pattern.
     */
    explicit Pcre2(const std::string& expression)
    {
        int error{0};

        PCRE2_SIZE offset{0};

        code_.reset(pcre2_compile(
                reinterpret_cast<PCRE2_SPTR>(expression.c_str()), PCRE2_ZERO_TERMINATED, PCRE2_ANCHORED, &error,
                &offset, nullptr));

        if (code_ != nullptr && pcre2_jit_compile(code_.get(), PCRE2_JIT_COMPLETE) == 0)
        {
            data_.reset(pcre2_match_data_create_from_pattern(code_.get(), nullptr));
        }
    }

    /**
     * @brief Returns whether the pattern compiled, JIT-compiled and has its match data.
     * @return True when the engine can scan.
     */
    [[nodiscard]] bool ok() const { return data_ != nullptr; }

    /**
     * @brief Tokenizes the whole input once, matching anchored at the current offset.
     * @tparam Sink The sink's type.
     * @param input The input.
     * @param sink Receives each token's kind and length.
     * @return True when the whole input was consumed.
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
            const int rc{pcre2_jit_match(code_.get(), subject, input.size(), offset, 0, data_.get(), nullptr)};

            const auto* ovector{pcre2_get_ovector_pointer(data_.get())};

            // The alternatives are exclusive, so the highest captured group is the one that matched.
            if (rc < 2 || ovector[1] == ovector[0])
            {
                return false;
            }

            const auto group{static_cast<std::size_t>(rc) - 2};

            sink(std::to_underlying(token_of_group[group]), ovector[1] - ovector[0]);

            offset += ovector[1] - ovector[0];
        }

        return true;
    }

private:
    /**
     * @brief Frees a compiled pattern.
     */
    struct Code_free
    {
        /**
         * @brief Frees the pattern.
         * @param code The pattern.
         */
        void operator()(pcre2_code* const code) const noexcept { pcre2_code_free(code); }
    };

    /**
     * @brief Frees a pattern's match data.
     */
    struct Data_free
    {
        /**
         * @brief Frees the match data.
         * @param data The match data.
         */
        void operator()(pcre2_match_data* const data) const noexcept { pcre2_match_data_free(data); }
    };

    /**
     * @brief The compiled pattern, null when compilation failed.
     */
    std::unique_ptr<pcre2_code, Code_free> code_{};

    /**
     * @brief The match data, null unless the pattern JIT-compiled.
     */
    std::unique_ptr<pcre2_match_data, Data_free> data_{};
};

/**
 * @brief Checks that every engine reproduces munch's token stream over one corpus, printing each engine that does not.
 *
 * The exact comparison runs once per corpus, before timing: every engine must reproduce munch's token stream to the
 * kind and the length, which is what licenses comparing their throughputs at all.
 * @param engines The engines.
 * @param corpus The corpus's name, as the printed lines name it.
 * @param input The corpus.
 * @param reference munch's token stream over the corpus.
 * @return True when every engine reproduced the reference.
 */
bool engines_agree(
        const std::span<const Engine> engines, const std::string& corpus, const std::string& input,
        const Stream_t& reference)
{
    auto ok{true};

    for (const auto& [name, run, scan_stream] : engines)
    {
        const std::string engine{name};

        const auto stream{scan_stream(input)};

        if (!stream)
        {
            std::printf("%s/%s: rejected the input\n", engine.c_str(), corpus.c_str());

            ok = false;

            continue;
        }

        if (*stream == reference)
        {
            continue;
        }

        const auto [diverging, reference_diverging]{std::ranges::mismatch(*stream, reference)};

        const auto index{static_cast<std::size_t>(diverging - stream->begin())};

        std::printf(
                "%s/%s: token stream diverges from munch at token %zu (%zu tokens vs %zu)\n", engine.c_str(),
                corpus.c_str(), index, stream->size(), reference.size());

        ok = false;
    }

    return ok;
}

/**
 * @brief Registers the code tokens the three munch grammars of the mode comparisons share, in their shared order.
 * @tparam Add The registration's type.
 * @param add Registers one token as add(regex, kind, priority).
 */
template <typename Add>
void add_code_tokens(const Add& add)
{
    using namespace munch::regex;

    add(plus(any_of(Set{' ', '\t', '\n'})), Mode_token::whitespace, 2);

    add(ascii_identifier(), Mode_token::identifier, 2);

    add(plus(any_of(Set::digits())), Mode_token::number, 2);

    add(choice(text("=="), text("+"), text("-"), text("=")), Mode_token::op, 2);

    add(any_of(Set{'(', ')', '{', '}', ';', ','}), Mode_token::punctuation, 2);
}

/**
 * @brief Pushes the code rules the two lexertl mode grammars share onto their INITIAL state, in their shared order.
 * @param rules The rules.
 */
void push_code_rules(lexertl::rules& rules)
{
    rules.push("INITIAL", "[ \t\n]+", std::to_underlying(Mode_token::whitespace), ".");

    rules.push("INITIAL", "[A-Za-z_][A-Za-z0-9_]*", std::to_underlying(Mode_token::identifier), ".");

    rules.push("INITIAL", "[0-9]+", std::to_underlying(Mode_token::number), ".");

    rules.push("INITIAL", "==|[-+=]", std::to_underlying(Mode_token::op), ".");

    rules.push("INITIAL", "[(){};,]", std::to_underlying(Mode_token::punctuation), ".");
}

/**
 * @brief Tokenizes the whole input once through a munch mode lexer, outside the timed passes.
 * @tparam Sink The sink's type.
 * @param lexer The mode lexer.
 * @param input The input.
 * @param sink Receives each token's kind and length.
 * @return True when the whole input was consumed.
 */
template <typename Sink>
bool scan_mode_lexer(const munch::core::Mode_lexer& lexer, const std::string& input, Sink&& sink)
{
    const auto hand_on{[&sink](const Mode_token token, const std::size_t length, const std::size_t) {
        sink(std::to_underlying(token), length);
    }};

    const auto consumed{lexer.tokenize_all<Mode_token>(input, hand_on)};

    return consumed == input.size();
}

/**
 * @brief Checks that both mode engines accepted the corpus and agree on every token, printing why when they do not.
 * @param label The comparison's name, which opens the printed line.
 * @param munch_stream munch's stream, std::nullopt on rejection.
 * @param lexertl_stream lexertl's stream, std::nullopt on rejection.
 * @return True when both streams exist and are equal.
 */
bool mode_engines_agree(
        const std::string_view label, const std::optional<Stream_t>& munch_stream,
        const std::optional<Stream_t>& lexertl_stream)
{
    const std::string name{label};

    if (!munch_stream || !lexertl_stream)
    {
        std::printf("%s: an engine rejected the input\n", name.c_str());

        return false;
    }

    if (*munch_stream != *lexertl_stream)
    {
        std::printf(
                "%s: the two engines disagree (%zu tokens vs %zu)\n", name.c_str(), munch_stream->size(),
                lexertl_stream->size());

        return false;
    }

    return true;
}

/**
 * @brief Generates code carrying string literals with bodies, the construct the modes exist for.
 * @param size The minimum size of the input in bytes.
 * @return The generated input.
 */
std::string generate_mode_input(const std::size_t size)
{
    std::string input{};

    input.reserve(size + 256);

    Corpus_random random{};

    while (input.size() < size)
    {
        input += "  name";
        input += std::to_string(random() % 100);
        input += " = value";
        input += std::to_string(random() % 100);
        input += " + ";
        input += std::to_string(random() % 100000);
        input += ";\n";
        input += R"(  label = "text body )";
        input += std::to_string(random() % 1000);
        input += R"( with words";)";
        input += "\n";
    }

    return input;
}

/**
 * @brief Builds a grammar whose string interiors are scanned in a second mode, built with munch's mode support.
 * @return The mode lexer.
 */
munch::core::Mode_lexer build_mode_lexer()
{
    using namespace munch::regex;

    munch::core::Mode_builder builder{};

    const auto add_code{[&builder](const Regex& regex, const Mode_token kind, const std::size_t priority) {
        builder.add_token(code_mode, regex, kind, priority);
    }};

    add_code_tokens(add_code);

    constexpr std::size_t string_mode{1};

    builder.add_token(
            code_mode, text(R"(")"), Mode_token::quote, 1,
            {.kind = munch::core::Mode_action_kind::go_to, .target = string_mode});

    builder.add_token(
            string_mode, text(R"(")"), Mode_token::quote, 1,
            {.kind = munch::core::Mode_action_kind::go_to, .target = code_mode});

    builder.add_token(string_mode, plus(any_of(Set::all() - '"')), Mode_token::text, 2);

    return builder.build();
}

/**
 * @brief Builds the same language as build_mode_lexer(), expressed with lexertl's start states.
 *
 * lexertl is the only engine in this comparison with the feature: a lexer built at run time from rules, with start
 * states and a next-state per rule. The general-purpose regex engines have no mode concept at all, so a caller would
 * switch patterns by hand, which measures their per-match cost rather than their mode support and is already what the
 * tables above report.
 * @return The minimised state machine.
 */
lexertl::state_machine build_lexertl_modes()
{
    lexertl::rules rules{};

    rules.push_state("STR");

    push_code_rules(rules);

    rules.push("INITIAL", R"(\")", std::to_underlying(Mode_token::quote), "STR");

    rules.push("STR", R"(\")", std::to_underlying(Mode_token::quote), "INITIAL");

    rules.push("STR", R"([^\"]+)", std::to_underlying(Mode_token::text), ".");

    lexertl::state_machine machine{};

    lexertl::generator::build(rules, machine);

    machine.minimise();

    return machine;
}

/**
 * @brief Compares munch's modes against lexertl's start states on a corpus where modes are optional.
 *
 * Kept apart from the tables above rather than folded in, because the streams are deliberately different: a mode
 * grammar scans string interiors and so emits more tokens than a flat grammar that treats a literal as one token.
 * Validating against munch's flat stream would fail by construction, so the two mode engines validate against each
 * other instead.
 * @param mebibytes The corpus size in MiB.
 * @param passes The interleaved rounds.
 * @param observations The CSV every observation is appended to, std::nullopt for none.
 * @return True if both engines tokenized the corpus completely and agreed on every token.
 */
bool compare_modes(const std::size_t mebibytes, const int passes, const std::optional<std::string_view> observations)
{
    using namespace munch::regex;

    const auto bytes{bytes_of(mebibytes)};

    const auto input{generate_mode_input(bytes)};

    const auto lexer{build_mode_lexer()};

    const auto machine{build_lexertl_modes()};

    const auto scan_munch_corpus{[&]<typename Sink>(Sink&& sink) { return scan_mode_lexer(lexer, input, sink); }};

    const auto munch_stream{stream_of(scan_munch_corpus)};

    const auto scan_lexertl_corpus{
            [&]<typename Sink>(Sink&& sink) { return scan_lexertl<Flat_results>(machine, input, sink); }};

    const auto lexertl_stream{stream_of(scan_lexertl_corpus)};

    if (!mode_engines_agree("modes", munch_stream, lexertl_stream))
    {
        return false;
    }

    const auto bytes_per_token{static_cast<double>(input.size()) / static_cast<double>(munch_stream->size())};

    std::printf("\ncorpus modes: %.2f bytes per token, string interiors scanned by both\n", bytes_per_token);

    // The same language expressed without modes, as a flat grammar treating a string literal as one token. Its stream
    // is deliberately different, so it is validated only for completeness rather than against the two mode engines; the
    // point of the row is the price of modes, which docs/limits.md quotes.
    munch::core::Builder flat{};

    const auto add_flat{[&flat](const Regex& regex, const Mode_token kind, const std::size_t priority) {
        flat.add_token(regex, kind, priority);
    }};

    add_code_tokens(add_flat);

    flat.add_token(concat(text(R"(")"), concat(kleene(any_of(Set::all() - '"')), text(R"(")"))), Mode_token::text, 2);

    const auto flat_lexer{flat.build()};

    // Interleaved rather than one engine's passes then the next, so that thermal and scheduling drift lands on every
    // scenario alike instead of accumulating on whichever ran last.
    const std::array<Scenario, 3> scenarios{{
            {.name = "munch-flat",
             .bytes = input.size(),
             .pass =
                     [&] {
                         return tally_of([&](auto&& sink) {
                                    const auto consumed{flat_lexer.tokenize_all<Mode_token>(
                                            input, [&sink](const Mode_token token, const std::size_t length) {
                                                sink(static_cast<std::size_t>(token), length);
                                            })};

                                    return consumed == input.size();
                                })
                                 .tokens;
                     }},
            {.name = "munch-modes",
             .bytes = input.size(),
             .pass =
                     [&] {
                         return tally_of([&](auto&& sink) {
                                    const auto consumed{lexer.tokenize_all<Mode_token>(
                                            input, [&sink](const Mode_token token, const std::size_t length,
                                                           const std::size_t) {
                                                sink(static_cast<std::size_t>(token), length);
                                            })};

                                    return consumed == input.size();
                                })
                                 .tokens;
                     }},
            {.name = "lexertl-modes",
             .bytes = input.size(),
             .pass =
                     [&] {
                         return tally_of([&](auto&& sink) { return scan_lexertl<Flat_results>(machine, input, sink); })
                                 .tokens;
                     }},
    }};

    return measure_interleaved(scenarios, passes, mebibytes, observations);
}

/**
 * @brief Generates code carrying block comments that nest, which the measured grammars count rather than bound.
 *
 * The language of arbitrarily nested comments is not regular, since counting to an unbounded depth is what one finite
 * automaton cannot do. This corpus nests only to depth four, and a bounded depth is regular: a flat grammar could
 * unroll four levels into distinct states and tokenize it. What the corpus measures is therefore the cost of the stack
 * both engines actually use, not a reach a flat grammar is denied here.
 * @param size The minimum size of the input in bytes.
 * @return The generated input.
 */
std::string generate_nested_input(const std::size_t size)
{
    std::string input{};

    input.reserve(size + 256);

    Corpus_random random{};

    while (input.size() < size)
    {
        input += "  name";
        input += std::to_string(random() % 100);
        input += " = value";
        input += std::to_string(random() % 100);
        input += ";\n";

        // Depths one to deepest_nesting, so the stack is exercised rather than merely entered.
        const auto depth{random() % deepest_nesting + 1};

        for (std::size_t level{0}; level < depth; ++level)
        {
            input += "/* outer ";
        }

        input += "note ";
        input += std::to_string(random() % 1000);

        for (std::size_t level{0}; level < depth; ++level)
        {
            input += " */";
        }

        input += "\n";
    }

    return input;
}

/**
 * @brief Builds the nesting grammar in munch, where an inner opener pushes and a closer pops.
 * @return The mode lexer.
 */
munch::core::Mode_lexer build_nested_lexer()
{
    using namespace munch::regex;

    munch::core::Mode_builder builder{};

    const auto add_code{[&builder](const Regex& regex, const Mode_token kind, const std::size_t priority) {
        builder.add_token(code_mode, regex, kind, priority);
    }};

    add_code_tokens(add_code);

    constexpr std::size_t comment_mode{1};

    builder.add_token(
            code_mode, text("/*"), Mode_token::comment_open, 1,
            {.kind = munch::core::Mode_action_kind::push, .target = comment_mode});

    builder.add_token(
            comment_mode, text("/*"), Mode_token::comment_open, 1,
            {.kind = munch::core::Mode_action_kind::push, .target = comment_mode});

    builder.add_token(
            comment_mode, text("*/"), Mode_token::comment_close, 1, {.kind = munch::core::Mode_action_kind::pop});

    builder.add_token(comment_mode, plus(any_of(Set::all() - '*' - '/')), Mode_token::comment_text, 2);

    builder.add_token(comment_mode, any_of(Set{'*', '/'}), Mode_token::comment_text, 3);

    return builder.build();
}

/**
 * @brief Builds the same nesting grammar in lexertl, whose start states push with ">" and pop with "<".
 * @return The minimised state machine.
 */
lexertl::state_machine build_lexertl_nested()
{
    lexertl::rules rules{};

    rules.push_state("COMMENT");

    push_code_rules(rules);

    rules.push("INITIAL", R"(\/\*)", std::to_underlying(Mode_token::comment_open), ">COMMENT");

    rules.push("COMMENT", R"(\/\*)", std::to_underlying(Mode_token::comment_open), ">COMMENT");

    rules.push("COMMENT", R"(\*\/)", std::to_underlying(Mode_token::comment_close), "<");

    rules.push("COMMENT", "[^*/]+", std::to_underlying(Mode_token::comment_text), ".");

    rules.push("COMMENT", "[*/]", std::to_underlying(Mode_token::comment_text), ".");

    lexertl::state_machine machine{};

    lexertl::generator::build(rules, machine);

    machine.minimise();

    return machine;
}

/**
 * @brief Measures the two engines that carry mode transitions in the grammar on input requiring a mode stack.
 *
 * Kept apart from compare_modes(): that corpus needs no stack, while here both engines push and pop one. No flat row,
 * because the grammar under test is the counting one; a flat grammar unrolling the corpus's four levels would be a
 * different grammar answering a different question.
 * @param mebibytes The corpus size in MiB.
 * @param passes The interleaved rounds.
 * @param observations The CSV every observation is appended to, std::nullopt for none.
 * @return True if both engines tokenized the corpus completely and agreed on every token.
 */
bool compare_nested(const std::size_t mebibytes, const int passes, const std::optional<std::string_view> observations)
{
    const auto bytes{bytes_of(mebibytes)};

    const auto input{generate_nested_input(bytes)};

    const auto lexer{build_nested_lexer()};

    const auto machine{build_lexertl_nested()};

    const auto scan_munch_corpus{[&]<typename Sink>(Sink&& sink) { return scan_mode_lexer(lexer, input, sink); }};

    const auto munch_stream{stream_of(scan_munch_corpus)};

    const auto scan_lexertl_corpus{
            [&]<typename Sink>(Sink&& sink) { return scan_lexertl<Nested_results>(machine, input, sink); }};

    const auto lexertl_stream{stream_of(scan_lexertl_corpus)};

    if (!mode_engines_agree("nested", munch_stream, lexertl_stream))
    {
        return false;
    }

    const auto bytes_per_token{static_cast<double>(input.size()) / static_cast<double>(munch_stream->size())};

    std::printf(
            "\ncorpus nested: %.2f bytes per token, comments nesting to depth 4, which the grammars count rather "
            "than bound\n",
            bytes_per_token);

    const std::array<Scenario, 2> scenarios{{
            {.name = "munch-nested",
             .bytes = input.size(),
             .pass =
                     [&] {
                         return tally_of([&](auto&& sink) {
                                    const auto consumed{lexer.tokenize_all<Mode_token>(
                                            input, [&sink](const Mode_token token, const std::size_t length,
                                                           const std::size_t) {
                                                sink(static_cast<std::size_t>(token), length);
                                            })};

                                    return consumed == input.size();
                                })
                                 .tokens;
                     }},
            {.name = "lexertl-nested",
             .bytes = input.size(),
             .pass =
                     [&] {
                         return tally_of(
                                        [&](auto&& sink) { return scan_lexertl<Nested_results>(machine, input, sink); })
                                 .tokens;
                     }},
    }};

    return measure_interleaved(scenarios, passes, mebibytes, observations);
}

} // namespace

/**
 * @brief Measures tokenization throughput of munch against common regex engines on identical generated pseudo-code.
 *
 * Every engine extracts the same information per token, its kind and length, and every engine's full tokenization is
 * validated against munch's before anything is timed. Usage: munch_benchmark_compare [input size in MiB] [passes]
 * [observations CSV]
 * @param argc The argument count.
 * @param argv The input size in MiB, the passes and the observations CSV, all optional.
 * @return EXIT_SUCCESS when every engine reproduced munch's stream and every pass its warmup result, EXIT_FAILURE
 *         otherwise.
 */
int main(const int argc, char** argv)
{
    struct Corpus
    {
        std::string_view name{};
        std::string input{};
    };

    const std::size_t mebibytes{argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8};

    const int passes{argc > 2 ? std::atoi(argv[2]) : default_passes};

    const auto observations{argc > 3 ? std::optional<std::string_view>{argv[3]} : std::nullopt};

    if (mebibytes == 0 || passes <= 0)
    {
        std::printf("usage: munch_benchmark_compare [input size in MiB > 0] [passes > 0] [observations CSV]\n");

        return EXIT_FAILURE;
    }

    print_provenance("engine comparison", passes, observations);

    const auto bytes{bytes_of(mebibytes)};

    // Two corpus shapes falsify each other's conclusions: dense punishes per-token overhead, source shows how the gaps
    // change once realistic token lengths amortize it.
    const std::array<Corpus, 2> corpora{{
            {.name = "dense", .input = generate_input(bytes, ascii_identifiers)},
            {.name = "source", .input = generate_source_input(bytes)},
    }};

    const auto lexer{build_lexer(false)};

    const auto lexertl_machine{build_lexertl()};

    const std::regex std_regex{pattern, std::regex::optimize};

    const boost::regex boost_regex{pattern};

    const RE2 re2{pattern};

    const Pcre2 pcre2{pattern};

    if (!re2.ok() || !pcre2.ok())
    {
        std::printf("failed to compile the comparison pattern\n");

        return EXIT_FAILURE;
    }

    const auto scan_boost{[&]<typename Sink>(const std::string& input, Sink&& sink) {
        return scan_backtracker<boost::regex, boost::smatch, boost::regex_constants::match_continuous>(
                boost_regex, input, sink);
    }};

    const auto scan_std{[&]<typename Sink>(const std::string& input, Sink&& sink) {
        return scan_backtracker<std::regex, std::smatch, std::regex_constants::match_continuous>(
                std_regex, input, sink);
    }};

    const std::array<Engine, 9> engines{{
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
                         return tally_of(
                                 [&](auto&& s) { return scan_lexertl<Flat_results>(lexertl_machine, input, s); });
                     },
             .stream =
                     [&](const std::string& input) {
                         return stream_of(
                                 [&](auto&& s) { return scan_lexertl<Flat_results>(lexertl_machine, input, s); });
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
    }};

    auto ok{true};

    for (const auto& [corpus_name, input] : corpora)
    {
        const std::string corpus{corpus_name};

        // The reference stream, scanned once per corpus before timing; the timed loops keep only the tally.
        const auto scan{[&]<typename Sink>(Sink&& sink) { return scan_munch(lexer, input, sink); }};

        const auto reference{stream_of(scan)};

        if (!reference || reference->empty())
        {
            std::printf("munch rejected the %s corpus\n", corpus.c_str());

            return EXIT_FAILURE;
        }

        const auto agree{engines_agree(engines, corpus, input, *reference)};

        if (!agree || !ok)
        {
            return EXIT_FAILURE;
        }

        const auto bytes_per_token{static_cast<double>(input.size()) / static_cast<double>(reference->size())};

        std::printf("corpus %s: %.2f bytes per token\n", corpus.c_str(), bytes_per_token);

        for (const auto& [name, run, stream] : engines)
        {
            const auto pass{[&run, &input] { return run(input); }};

            const auto tokens_of{[](const Tally& tally) { return tally.tokens; }};

            const auto measured{measure(name, input.size(), passes, pass, tokens_of)};

            ok = measured && ok;
        }
    }

    ok = compare_modes(mebibytes, passes, observations) && ok;

    ok = compare_nested(mebibytes, passes, observations) && ok;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
