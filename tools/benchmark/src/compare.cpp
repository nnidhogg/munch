#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <numeric>
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

#include "munch/core/mode_builder.hpp"
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

    // Chunks are disjoint and a scan cannot leave its chunk, so summing to the input's size means full consumption.
    if (std::reduce(consumed.cbegin(), consumed.cend(), std::size_t{0}) != input.size())
    {
        return {};
    }

    Tally total{};

    for (std::size_t chunk{0}; chunk < consumed.size(); ++chunk)
    {
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
 * @brief The token kinds of the mode comparison, shared by both engines so their streams are comparable.
 */
enum class Mode_token : std::size_t
{
    whitespace = 1,
    identifier,
    number,
    op,
    punctuation,
    quote,
    text,
    comment_open,
    comment_close,
    comment_text
};

/**
 * @brief Drives lexertl over a machine whose rules push and pop start states.
 *
 * recursive_match_results rather than match_results: the stack the pushes use lives on that type, and lookup()
 * dereferences it whether or not the plain type has one.
 */
template <typename Sink>
bool scan_lexertl_nested(const lexertl::state_machine& sm, const std::string& input, Sink&& sink)
{
    lexertl::recursive_match_results<std::string::const_iterator> results(input.cbegin(), input.cend());

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
 * @brief Code carrying block comments that nest, which the measured grammars count rather than bound.
 *
 * The language of arbitrarily nested comments is not regular, since counting to an unbounded depth is what one
 * finite automaton cannot do. This corpus nests only to depth four, and a bounded depth is regular: a flat grammar
 * could unroll four levels into distinct states and tokenize it. What the corpus measures is therefore the cost of
 * the stack both engines actually use, not a reach a flat grammar is denied here.
 * @param size The minimum size of the input in bytes.
 */
std::string generate_nested_input(const std::size_t size)
{
    std::string input;

    input.reserve(size + 256);

    unsigned seed{12345};

    const auto random{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    while (input.size() < size)
    {
        input += "  name";
        input += std::to_string(random() % 100);
        input += " = value";
        input += std::to_string(random() % 100);
        input += ";\n";

        // Depths one to four, so the stack is exercised rather than merely entered.
        const auto depth{random() % 4 + 1};

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
 * @brief The nesting grammar in munch, where an inner opener pushes and a closer pops.
 */
munch::core::Mode_lexer build_nested_lexer()
{
    using namespace munch::regex;

    constexpr std::size_t code{0};

    constexpr std::size_t comment{1};

    munch::core::Mode_builder builder;

    builder.add_token(code, plus(any_of(Set{' ', '\t', '\n'})), Mode_token::whitespace, 2);
    builder.add_token(
            code, concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Mode_token::identifier, 2);
    builder.add_token(code, plus(any_of(Set::digits())), Mode_token::number, 2);
    builder.add_token(code, choice(text("=="), text("+"), text("-"), text("=")), Mode_token::op, 2);
    builder.add_token(code, any_of(Set{'(', ')', '{', '}', ';', ','}), Mode_token::punctuation, 2);
    builder.add_token(
            code, text("/*"), Mode_token::comment_open, 1,
            {.kind = munch::core::Mode_action_kind::push, .target = comment});

    builder.add_token(
            comment, text("/*"), Mode_token::comment_open, 1,
            {.kind = munch::core::Mode_action_kind::push, .target = comment});
    builder.add_token(comment, text("*/"), Mode_token::comment_close, 1, {.kind = munch::core::Mode_action_kind::pop});
    builder.add_token(comment, plus(any_of(Set::all() - '*' - '/')), Mode_token::comment_text, 2);
    builder.add_token(comment, any_of(Set{'*', '/'}), Mode_token::comment_text, 3);

    return builder.build();
}

/**
 * @brief The same nesting grammar in lexertl, whose start states push with ">" and pop with "<".
 */
lexertl::state_machine build_lexertl_nested()
{
    lexertl::rules rules;

    rules.push_state("COMMENT");

    rules.push("INITIAL", "[ \t\n]+", static_cast<std::size_t>(Mode_token::whitespace), ".");
    rules.push("INITIAL", "[A-Za-z_][A-Za-z0-9_]*", static_cast<std::size_t>(Mode_token::identifier), ".");
    rules.push("INITIAL", "[0-9]+", static_cast<std::size_t>(Mode_token::number), ".");
    rules.push("INITIAL", "==|[-+=]", static_cast<std::size_t>(Mode_token::op), ".");
    rules.push("INITIAL", "[(){};,]", static_cast<std::size_t>(Mode_token::punctuation), ".");
    rules.push("INITIAL", "\\/\\*", static_cast<std::size_t>(Mode_token::comment_open), ">COMMENT");

    rules.push("COMMENT", "\\/\\*", static_cast<std::size_t>(Mode_token::comment_open), ">COMMENT");
    rules.push("COMMENT", "\\*\\/", static_cast<std::size_t>(Mode_token::comment_close), "<");
    rules.push("COMMENT", "[^*/]+", static_cast<std::size_t>(Mode_token::comment_text), ".");
    rules.push("COMMENT", "[*/]", static_cast<std::size_t>(Mode_token::comment_text), ".");

    lexertl::state_machine machine;

    lexertl::generator::build(rules, machine);

    machine.minimise();

    return machine;
}

/**
 * @brief A grammar whose string interiors are scanned in a second mode, built with munch's mode support.
 */
munch::core::Mode_lexer build_mode_lexer()
{
    using namespace munch::regex;

    constexpr std::size_t code{0};

    constexpr std::size_t string{1};

    munch::core::Mode_builder builder;

    builder.add_token(code, plus(any_of(Set{' ', '\t', '\n'})), Mode_token::whitespace, 2);
    builder.add_token(
            code, concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Mode_token::identifier, 2);
    builder.add_token(code, plus(any_of(Set::digits())), Mode_token::number, 2);
    builder.add_token(code, choice(text("=="), text("+"), text("-"), text("=")), Mode_token::op, 2);
    builder.add_token(code, any_of(Set{'(', ')', '{', '}', ';', ','}), Mode_token::punctuation, 2);
    builder.add_token(
            code, text("\""), Mode_token::quote, 1, {.kind = munch::core::Mode_action_kind::go_to, .target = string});

    builder.add_token(
            string, text("\""), Mode_token::quote, 1, {.kind = munch::core::Mode_action_kind::go_to, .target = code});
    builder.add_token(string, plus(any_of(Set::all() - '"')), Mode_token::text, 2);

    return builder.build();
}

/**
 * @brief The same language as build_mode_lexer(), expressed with lexertl's start states.
 *
 * lexertl is the only engine in this comparison with the feature: a lexer built at run time from rules, with start
 * states and a next-state per rule. The general-purpose regex engines have no mode concept at all, so a caller
 * would switch patterns by hand, which measures their per-match cost rather than their mode support and is already
 * what the tables above report.
 */
lexertl::state_machine build_lexertl_modes()
{
    lexertl::rules rules;

    rules.push_state("STR");

    rules.push("INITIAL", "[ \t\n]+", static_cast<std::size_t>(Mode_token::whitespace), ".");
    rules.push("INITIAL", "[A-Za-z_][A-Za-z0-9_]*", static_cast<std::size_t>(Mode_token::identifier), ".");
    rules.push("INITIAL", "[0-9]+", static_cast<std::size_t>(Mode_token::number), ".");
    rules.push("INITIAL", "==|[-+=]", static_cast<std::size_t>(Mode_token::op), ".");
    rules.push("INITIAL", "[(){};,]", static_cast<std::size_t>(Mode_token::punctuation), ".");
    rules.push("INITIAL", "\\\"", static_cast<std::size_t>(Mode_token::quote), "STR");

    rules.push("STR", "\\\"", static_cast<std::size_t>(Mode_token::quote), "INITIAL");
    rules.push("STR", "[^\\\"]+", static_cast<std::size_t>(Mode_token::text), ".");

    lexertl::state_machine machine;

    lexertl::generator::build(rules, machine);

    machine.minimise();

    return machine;
}

/**
 * @brief Generates code carrying string literals with bodies, the construct the modes exist for.
 */
std::string generate_mode_input(const std::size_t size)
{
    std::string input;

    input.reserve(size + 256);

    unsigned seed{12345};

    const auto random{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    while (input.size() < size)
    {
        input += "  name";
        input += std::to_string(random() % 100);
        input += " = value";
        input += std::to_string(random() % 100);
        input += " + ";
        input += std::to_string(random() % 100000);
        input += ";\n  label = \"text body ";
        input += std::to_string(random() % 1000);
        input += " with words\";\n";
    }

    return input;
}

/**
 * @brief Measures the two engines that carry mode transitions in the grammar on input requiring a mode stack.
 *
 * Kept apart from compare_modes(): that corpus needs no stack, while here both engines push and pop one. No flat
 * row, because the grammar under test is the counting one; a flat grammar unrolling the corpus's four levels would
 * be a different grammar answering a different question.
 */
bool compare_nested(const std::size_t mebibytes, const int passes, const char* const observations)
{
    using Scenario_t = munch::tools::benchmark::Scenario;

    const auto input{generate_nested_input(mebibytes << 20U)};

    const auto lexer{build_nested_lexer()};

    const auto machine{build_lexertl_nested()};

    const auto munch_stream{stream_of([&](auto&& sink) {
        const auto consumed{lexer.tokenize_all<Mode_token>(
                input, [&sink](const Mode_token token, const std::size_t length, const std::size_t) {
                    sink(static_cast<std::size_t>(token), length);
                })};

        return consumed == input.size();
    })};

    const auto lexertl_stream{stream_of([&](auto&& sink) { return scan_lexertl_nested(machine, input, sink); })};

    if (!munch_stream || !lexertl_stream)
    {
        std::printf("nested: an engine rejected the input\n");

        return false;
    }

    if (*munch_stream != *lexertl_stream)
    {
        std::printf(
                "nested: the two engines disagree (%zu tokens vs %zu)\n", munch_stream->size(), lexertl_stream->size());

        return false;
    }

    std::printf(
            "\ncorpus nested: %.2f bytes per token, comments nesting to depth 4, which the grammars count rather "
            "than bound\n",
            static_cast<double>(input.size()) / static_cast<double>(munch_stream->size()));

    const Scenario_t scenarios[]{
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
                         return tally_of([&](auto&& sink) { return scan_lexertl_nested(machine, input, sink); }).tokens;
                     }},
    };

    return measure_interleaved(scenarios, passes, mebibytes, observations);
}

/**
 * @brief Compares munch's modes against lexertl's start states on a corpus where modes are optional.
 *
 * Kept apart from the tables above rather than folded in, because the streams are deliberately different: a mode
 * grammar scans string interiors and so emits more tokens than a flat grammar that treats a literal as one token.
 * Validating against munch's flat stream would fail by construction, so the two mode engines validate against each
 * other instead.
 * @return True if both engines tokenized the corpus completely and agreed on every token.
 */
bool compare_modes(const std::size_t mebibytes, const int passes, const char* const observations)
{
    using Scenario_t = munch::tools::benchmark::Scenario;

    const auto input{generate_mode_input(mebibytes << 20U)};

    const auto lexer{build_mode_lexer()};

    const auto machine{build_lexertl_modes()};

    const auto munch_stream{stream_of([&](auto&& sink) {
        const auto consumed{lexer.tokenize_all<Mode_token>(
                input, [&sink](const Mode_token token, const std::size_t length, const std::size_t) {
                    sink(static_cast<std::size_t>(token), length);
                })};

        return consumed == input.size();
    })};

    const auto lexertl_stream{stream_of([&](auto&& sink) { return scan_lexertl(machine, input, sink); })};

    if (!munch_stream || !lexertl_stream)
    {
        std::printf("modes: an engine rejected the input\n");

        return false;
    }

    if (*munch_stream != *lexertl_stream)
    {
        std::printf(
                "modes: the two engines disagree (%zu tokens vs %zu)\n", munch_stream->size(), lexertl_stream->size());

        return false;
    }

    std::printf(
            "\ncorpus modes: %.2f bytes per token, string interiors scanned by both\n",
            static_cast<double>(input.size()) / static_cast<double>(munch_stream->size()));

    // The same language expressed without modes, as a flat grammar treating a string literal as one token. Its
    // stream is deliberately different, so it is validated only for completeness rather than against the two mode
    // engines; the point of the row is the price of modes, which docs/limits.md quotes.
    using namespace munch::regex;

    munch::core::Builder flat;

    flat.add_token(plus(any_of(Set{' ', '\t', '\n'})), Mode_token::whitespace, 2);
    flat.add_token(
            concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Mode_token::identifier, 2);
    flat.add_token(plus(any_of(Set::digits())), Mode_token::number, 2);
    flat.add_token(choice(text("=="), text("+"), text("-"), text("=")), Mode_token::op, 2);
    flat.add_token(any_of(Set{'(', ')', '{', '}', ';', ','}), Mode_token::punctuation, 2);
    flat.add_token(concat(text("\""), concat(kleene(any_of(Set::all() - '"')), text("\""))), Mode_token::text, 2);

    const auto flat_lexer{flat.build()};

    // Interleaved rather than one engine's passes then the next, so that thermal and scheduling drift lands on
    // every scenario alike instead of accumulating on whichever ran last.
    const Scenario_t scenarios[]{
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
             .pass = [&] { return tally_of([&](auto&& sink) { return scan_lexertl(machine, input, sink); }).tokens; }},
    };

    return measure_interleaved(scenarios, passes, mebibytes, observations);
}

/**
 * @brief Measures tokenization throughput of munch against common regex engines on identical generated pseudo-code.
 *
 * Every engine extracts the same information per token, its kind and length, and every engine's full tokenization
 * is validated against munch's before anything is timed.
 * Usage: munch_benchmark_compare [input size in MiB] [passes] [observations CSV]
 */
int main(const int argc, const char** argv)
{
    const std::size_t mebibytes{argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8};

    const int passes{argc > 2 ? std::atoi(argv[2]) : 15};

    const char* const observations{argc > 3 ? argv[3] : nullptr};

    if (mebibytes == 0 || passes <= 0)
    {
        std::printf("usage: munch_benchmark_compare [input size in MiB > 0] [passes > 0] [observations CSV]\n");

        return EXIT_FAILURE;
    }

    print_provenance("engine comparison", passes, observations);

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

    ok = compare_modes(mebibytes, passes, observations) && ok;

    ok = compare_nested(mebibytes, passes, observations) && ok;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
