#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <munch/core/builder.hpp>
#include <munch/regex/patterns.hpp>
#include <munch/regex/regex.hpp>
#include <munch/regex/unicode.hpp>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "munch/tools/benchmark/harness.hpp"
#include "munch/tools/tokenizer/tokenizer.hpp"

namespace
{
using namespace munch::tools::benchmark;

/**
 * @brief Tokenizes the whole input once through the Lexer.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @return The number of tokens matched, or 0 if the input was rejected.
 */
std::size_t tokenize(const munch::core::Lexer& lexer, const std::string& input)
{
    std::size_t offset{0};

    std::size_t tokens{0};

    while (offset < input.size())
    {
        const auto [token, length]{lexer.tokenize<Token>(input.cbegin() + offset, input.cend())};

        if (!token || length == 0)
        {
            std::printf("input rejected at offset %zu\n", offset);

            return 0;
        }

        offset += length;

        ++tokens;
    }

    return tokens;
}

/**
 * @brief Tokenizes the whole input once through the Tokenizer, measuring the driver layer as well.
 *
 * Flattened because next() only inlines into the loop on its own when the token type has internal linkage, and
 * whether it inlines swings the measurement by a third.
 * @param tokenizer The tokenizer to run, rewound before the pass.
 * @return The number of tokens matched, or 0 if the input was rejected.
 */
[[gnu::flatten]] std::size_t tokenize(munch::tools::tokenizer::Tokenizer& tokenizer)
{
    tokenizer.reset();

    std::size_t tokens{0};

    for (;;)
    {
        const auto result{tokenizer.next<Token>()};

        if (result.end_of_input())
        {
            return tokens;
        }

        if (result.has_error())
        {
            std::printf("input rejected at offset %zu\n", tokenizer.offset());

            return 0;
        }

        ++tokens;
    }
}

/**
 * @brief Tokenizes the whole input once through Lexer::tokenize_all, the batch entry point.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @return The number of tokens matched, or 0 if the input was rejected.
 */
std::size_t tokenize_all(const munch::core::Lexer& lexer, const std::string& input)
{
    std::size_t tokens{0};

    const auto consumed{lexer.tokenize_all<Token>(input, [&tokens](const Token, const std::size_t) { ++tokens; })};

    if (consumed != input.size())
    {
        std::printf("input rejected at offset %zu\n", consumed);

        return 0;
    }

    return tokens;
}

/**
 * @brief Tokenizes the input in parallel chunks through the library's tokenize_all_parallel entry point.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @param chunks The number of chunks to divide the input into.
 * @return The total number of tokens matched, or 0 if any chunk was rejected.
 */
std::size_t tokenize_chunked(const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    // One counter per cache line, as the entry point's contract asks: adjacent per-chunk counters would
    // false-share across the worker threads.
    struct alignas(64) Count
    {
        std::size_t tokens{0};
    };

    std::vector<Count> counts(chunks);

    const auto consumed{lexer.tokenize_all_parallel<Token>(
            input, chunks,
            [&counts](const std::size_t chunk, const Token, const std::size_t) { ++counts[chunk].tokens; })};

    const auto boundaries{lexer.chunk_boundaries(input, chunks)};

    std::size_t total{0};

    for (std::size_t chunk{0}; chunk < consumed.size(); ++chunk)
    {
        if (consumed[chunk] != boundaries[chunk + 1] - boundaries[chunk])
        {
            std::printf("chunk %zu rejected at offset %zu\n", chunk, boundaries[chunk] + consumed[chunk]);

            return 0;
        }

        total += counts[chunk].tokens;
    }

    return total;
}

/**
 * @brief Verifies that chunked tokenization is identical to the whole-input scan, token for token.
 *
 * Collects the exact (kind, length) stream of the serial scan and of the chunk-local scans spliced in order; the
 * two agree exactly when every token's kind and boundary match, so identical kinds split at different boundaries
 * cannot slip through the way a kind-only checksum would allow.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @param chunks The number of chunks to divide the input into.
 * @return True if the chunked token stream matches the serial one.
 */
bool validate_chunked(const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    using Stream_t = std::vector<std::pair<Token, std::size_t>>;

    const auto collect{[](Stream_t& stream) {
        return [&stream](const Token token, const std::size_t length) { stream.emplace_back(token, length); };
    }};

    Stream_t serial;

    if (lexer.tokenize_all<Token>(input, collect(serial)) != input.size())
    {
        return false;
    }

    const auto boundaries{lexer.chunk_boundaries(input, chunks)};

    Stream_t chunked;

    for (std::size_t index{0}; index + 1 < boundaries.size(); ++index)
    {
        const auto begin{input.cbegin() + static_cast<std::ptrdiff_t>(boundaries[index])};

        const auto end{input.cbegin() + static_cast<std::ptrdiff_t>(boundaries[index + 1])};

        if (lexer.tokenize_all<Token>(begin, end, collect(chunked)) != static_cast<std::size_t>(end - begin))
        {
            return false;
        }
    }

    if (chunked != serial)
    {
        std::printf("chunked token stream diverged from the serial scan\n");

        return false;
    }

    return true;
}

/**
 * @brief A Builder exposing its protected pipeline output, so the compiled automaton's size can be reported.
 */
struct Staged_builder : munch::core::Builder
{
    using Builder::dfa;
};

/**
 * @brief Builds a keyword-scale token set: 100 keywords plus identifier, literal, operator, and punctuation
 * patterns, approximating a real language front end.
 */
Staged_builder keyword_scale_builder()
{
    using namespace munch::regex;

    // Roughly the C++ keyword set plus common fixed-width type names: 100 entries.
    static constexpr const char* keywords[]{
            "alignas",     "alignof",   "and",        "and_eq",    "asm",      "auto",         "bitand",
            "bitor",       "bool",      "break",      "case",      "catch",    "char",         "char8_t",
            "char16_t",    "char32_t",  "class",      "compl",     "concept",  "const",        "consteval",
            "constexpr",   "constinit", "const_cast", "continue",  "co_await", "co_return",    "co_yield",
            "decltype",    "default",   "delete",     "do",        "double",   "dynamic_cast", "else",
            "enum",        "explicit",  "export",     "extern",    "false",    "float",        "for",
            "friend",      "goto",      "if",         "inline",    "int",      "long",         "mutable",
            "namespace",   "new",       "noexcept",   "not",       "not_eq",   "nullptr",      "operator",
            "or",          "or_eq",     "private",    "protected", "public",   "register",     "reinterpret_cast",
            "requires",    "return",    "short",      "signed",    "sizeof",   "static",       "static_assert",
            "static_cast", "struct",    "switch",     "template",  "this",     "thread_local", "throw",
            "true",        "try",       "typedef",    "typeid",    "typename", "union",        "unsigned",
            "using",       "virtual",   "void",       "volatile",  "wchar_t",  "while",        "xor",
            "xor_eq",      "final",     "override",   "import",    "module",   "int8_t",       "int16_t",
            "int32_t",     "int64_t"};

    Staged_builder builder;

    for (const auto* keyword : keywords)
    {
        builder.add_token(text(keyword), Token::keyword, 1);
    }

    builder.add_token(concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Token::identifier, 2);

    builder.add_token(patterns::decimal_float(), Token::number, 1);

    builder.add_token(patterns::decimal_integer(), Token::number, 1);

    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 1);

    for (const auto* op : {"==", "!=", "<=", ">=", "<<", ">>", "&&", "||", "++", "--", "->", "+=", "-=", "*=",
                           "/=", "+",  "-",  "*",  "/",  "%",  "=",  "<",  ">",  "!",  "~",  "&",  "|",  "^"})
    {
        builder.add_token(text(op), Token::operator_, 2);
    }

    for (const auto* punct : {"(", ")", "{", "}", "[", "]", ";", ",", ".", ":", "?"})
    {
        builder.add_token(text(punct), Token::punctuation, 2);
    }

    return builder;
}

/**
 * @brief Measures the construction cost of the keyword-scale token set, reported in milliseconds rather than
 * throughput, together with the compiled automaton's size.
 */
void measure_build(const int passes)
{
    const auto builder{keyword_scale_builder()};

    std::vector<double> milliseconds;

    milliseconds.reserve(static_cast<std::size_t>(passes));

    for (int index{0}; index < passes; ++index)
    {
        const auto start{std::chrono::steady_clock::now()};

        static_cast<void>(builder.build());

        const std::chrono::duration<double, std::milli> elapsed{std::chrono::steady_clock::now() - start};

        milliseconds.push_back(elapsed.count());
    }

    std::sort(milliseconds.begin(), milliseconds.end());

    const auto dfa{builder.dfa()};

    std::set<std::size_t> states{dfa.init_state()};

    for (const auto& [key, to] : dfa.transitions())
    {
        states.insert(key.first);

        states.insert(to);
    }

    const auto median{(milliseconds[(milliseconds.size() - 1) / 2] + milliseconds[milliseconds.size() / 2]) / 2.0};

    std::printf(
            "build/keywords   143 patterns, %zu states, %d passes: best %.1f, median %.1f, worst %.1f ms\n",
            states.size(), passes, milliseconds.front(), median, milliseconds.back());
}

/**
 * @brief Builds the benchmark lexer with the identifier class drawn from the XID properties.
 *
 * The same grammar as the UTF-8 lexer with the hand-rolled Greek range replaced by the full Unicode identifier
 * definition, so both scenarios tokenize the Greek input to the identical stream and the throughput difference
 * isolates the class size.
 */
munch::core::Lexer build_xid_lexer()
{
    using namespace munch::regex;

    munch::core::Builder builder;

    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 2);

    builder.add_token(
            concat(choice(text('_'), unicode::xid_start()), kleene(unicode::xid_continue())), Token::identifier, 2);

    builder.add_token(plus(any_of(Set::digits())), Token::number, 2);

    builder.add_token(choice(text("if"), text("else"), text("while"), text("return"), text("int")), Token::keyword, 1);

    builder.add_token(
            choice(text("=="), text("!="), text("<="), text(">="), text("+"), text("-"), text("*"), text("/"),
                   text("="), text("<"), text(">")),
            Token::operator_, 2);

    builder.add_token(any_of(Set{'(', ')', '{', '}', ';', ','}), Token::punctuation, 2);

    return builder.build();
}

/**
 * @brief Measures the construction cost of a Unicode identifier grammar over the XID properties.
 *
 * The identifier pattern expands the two XID property tables, 1497 code point ranges, into byte alternatives:
 * the construction stress case a generated property class poses. Three figures make the whole story visible:
 * register/xid covers expanding the properties into patterns and registering them, build/xid covers finalization
 * through build() as in the keyword-scale measurement, and total/xid is one pass through both.
 */
void measure_xid_build(const int passes)
{
    using namespace munch::regex;

    std::vector<double> registration;

    std::vector<double> finalization;

    registration.reserve(static_cast<std::size_t>(passes));

    finalization.reserve(static_cast<std::size_t>(passes));

    std::size_t state_count{0};

    for (int index{0}; index < passes; ++index)
    {
        const auto start{std::chrono::steady_clock::now()};

        Staged_builder builder;

        builder.add_token(
                concat(choice(text('_'), unicode::xid_start()), kleene(unicode::xid_continue())), Token::identifier, 2);

        builder.add_token(patterns::decimal_integer(), Token::number, 1);

        builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 1);

        const auto registered{std::chrono::steady_clock::now()};

        static_cast<void>(builder.build());

        const auto built{std::chrono::steady_clock::now()};

        registration.push_back(std::chrono::duration<double, std::milli>{registered - start}.count());

        finalization.push_back(std::chrono::duration<double, std::milli>{built - registered}.count());

        if (index == 0)
        {
            const auto dfa{builder.dfa()};

            std::set<std::size_t> states{dfa.init_state()};

            for (const auto& [key, to] : dfa.transitions())
            {
                states.insert(key.first);

                states.insert(to);
            }

            state_count = states.size();
        }
    }

    const auto report{[passes, state_count](const char* const name, std::vector<double> milliseconds) {
        std::sort(milliseconds.begin(), milliseconds.end());

        const auto median{(milliseconds[(milliseconds.size() - 1) / 2] + milliseconds[milliseconds.size() / 2]) / 2.0};

        std::printf(
                "%s     3 patterns, %zu states, %d passes: best %.1f, median %.1f, worst %.1f ms\n", name, state_count,
                passes, milliseconds.front(), median, milliseconds.back());
    }};

    std::vector<double> total;

    total.reserve(registration.size());

    for (std::size_t index{0}; index < registration.size(); ++index)
    {
        total.push_back(registration[index] + finalization[index]);
    }

    report("register/xid", std::move(registration));

    report("build/xid   ", std::move(finalization));

    report("total/xid   ", std::move(total));
}

/**
 * @brief Measures the cost of planning chunk boundaries as certified symbols grow scarce.
 *
 * chunk_boundaries() slides each interior boundary forward from its equal-division target to the next certified
 * byte, so its cost depends on how far it has to look. Four densities bound the range: a certified byte on almost
 * every line, one every megabyte, a certificate whose byte never occurs in the input at all, and a token set that
 * certifies nothing, which the planner answers without scanning. The planned input is never tokenized here; only
 * the plan is timed, and each pass repeats the plan until the sample is long enough to divide by, so the cheap
 * densities measure planning rather than the clock.
 */
bool measure_planning(const int passes)
{
    using namespace munch::regex;

    enum class Kind : std::size_t
    {
        Line,
        Newline,
    };

    // Newline as its own token certifies it; the line body cannot contain one, which is what makes the split safe.
    const auto certifying{[] {
        munch::core::Builder builder;

        builder.add_token(plus(any_of(Set::all() - '\n')), Kind::Line, 2);

        builder.add_token(text("\n"), Kind::Newline, 1);

        return builder.build();
    }()};

    // A line body that may contain any byte certifies nothing: every byte is consumable mid-token.
    const auto certifying_nothing{[] {
        munch::core::Builder builder;

        builder.add_token(plus(any_of(Set::all())), Kind::Line, 1);

        return builder.build();
    }()};

    // The certified bytes sit half a period ahead of every equal-division target, so each search scans half a
    // period rather than landing on a boundary by coincidence.
    const auto input{[](const std::size_t size, const std::size_t period) {
        std::string result(size, 'x');

        for (std::size_t offset{period / 2}; period != 0 && offset < size; offset += period)
        {
            result[offset] = '\n';
        }

        return result;
    }};

    constexpr std::size_t size{16U << 20U};

    constexpr std::size_t chunks{8};

    const auto frequent{input(size, 40)};

    const auto rare{input(size, 1U << 20U)};

    const auto absent{input(size, 0)};

    const auto plan{[passes](const char* const name, const munch::core::Lexer& lexer, const std::string& text) {
        std::vector<double> microseconds;

        microseconds.reserve(static_cast<std::size_t>(passes));

        std::size_t planned{0};

        for (int index{0}; index < passes; ++index)
        {
            // A plan over a dense certificate costs a fraction of a microsecond, which one clock reading cannot
            // resolve, so each pass repeats the plan until the sample outlasts the timer and divides. A plan that
            // scans the whole input passes the floor on its first call.
            constexpr std::chrono::milliseconds floor{2};

            std::size_t iterations{0};

            const auto start{std::chrono::steady_clock::now()};

            std::chrono::steady_clock::duration elapsed{};

            do
            {
                planned = lexer.chunk_boundaries(text, chunks).size();

                ++iterations;

                elapsed = std::chrono::steady_clock::now() - start;
            } while (elapsed < floor);

            microseconds.push_back(
                    std::chrono::duration<double, std::micro>{elapsed}.count() / static_cast<double>(iterations));
        }

        std::sort(microseconds.begin(), microseconds.end());

        const auto median{(microseconds[(microseconds.size() - 1) / 2] + microseconds[microseconds.size() / 2]) / 2.0};

        std::printf(
                "%s %zu of %zu chunks, %d passes: best %.1f, median %.1f, worst %.1f us\n", name, planned - 1, chunks,
                passes, microseconds.front(), median, microseconds.back());
    }};

    plan("plan/frequent  ", certifying, frequent);

    plan("plan/rare      ", certifying, rare);

    plan("plan/absent    ", certifying, absent);

    plan("plan/uncertified", certifying_nothing, frequent);

    // Planning and scanning timed over the same grammar and the same input, which is what makes the two comparable.
    // The plan-only rows above measure the planner in isolation; adding them to a scan of the C-like corpus would
    // compare different lexers over different inputs, so the end-to-end question needs its own scenario.
    const auto end_to_end{
            [passes](
                    const char* const name, const munch::core::Lexer& lexer, const std::string& text,
                    const bool parallel) -> bool {
                std::vector<double> milliseconds;

                milliseconds.reserve(static_cast<std::size_t>(passes));

                // One counter per chunk, each on its own cache line: a shared counter would be a data race, and an
                // unpadded array would put the workers' increments on the same line and charge the parallel case for
                // false sharing.
                struct alignas(64) Counter
                {
                    std::size_t value{0};
                };

                std::size_t tokens{0};

                // A pass that stopped early would still produce a plausible, and faster, timing row. Every pass is
                // therefore checked to have consumed its whole span and emitted the same count as the first.
                const auto boundaries{lexer.chunk_boundaries(text, chunks)};

                for (int index{0}; index < passes; ++index)
                {
                    std::vector<Counter> counters(chunks);

                    std::size_t serial_consumed{0};

                    std::vector<std::size_t> chunk_consumed;

                    const auto start{std::chrono::steady_clock::now()};

                    if (parallel)
                    {
                        // The whole cost a caller pays for choosing the parallel entry point: planning, then scanning.
                        chunk_consumed = lexer.tokenize_all_parallel<Kind>(
                                text, chunks,
                                [&counters](const std::size_t chunk, Kind, std::size_t) { ++counters[chunk].value; });
                    }
                    else
                    {
                        serial_consumed =
                                lexer.tokenize_all<Kind>(text, [&counters](Kind, std::size_t) { ++counters[0].value; });
                    }

                    const auto elapsed{std::chrono::steady_clock::now() - start};

                    milliseconds.push_back(std::chrono::duration<double, std::milli>{elapsed}.count());

                    std::size_t counted{0};

                    for (const auto& counter : counters)
                    {
                        counted += counter.value;
                    }

                    if (parallel && chunk_consumed.size() + 1 != boundaries.size())
                    {
                        std::printf(
                                "%s planned %zu chunks but scanned %zu\n", name, boundaries.size() - 1,
                                chunk_consumed.size());

                        return false;
                    }

                    for (std::size_t chunk{0}; parallel && chunk < chunk_consumed.size(); ++chunk)
                    {
                        if (const auto span{boundaries[chunk + 1] - boundaries[chunk]}; chunk_consumed[chunk] != span)
                        {
                            std::printf(
                                    "%s chunk %zu tokenized %zu of %zu bytes\n", name, chunk, chunk_consumed[chunk],
                                    span);

                            return false;
                        }
                    }

                    if (!parallel && serial_consumed != text.size())
                    {
                        std::printf("%s tokenized %zu of %zu bytes\n", name, serial_consumed, text.size());

                        return false;
                    }

                    if (index != 0 && counted != tokens)
                    {
                        std::printf("%s emitted %zu tokens, expected %zu\n", name, counted, tokens);

                        return false;
                    }

                    tokens = counted;
                }

                std::sort(milliseconds.begin(), milliseconds.end());

                const auto median{
                        (milliseconds[(milliseconds.size() - 1) / 2] + milliseconds[milliseconds.size() / 2]) / 2.0};

                std::printf(
                        "%s %zu tokens, %d passes: best %.1f, median %.1f, worst %.1f ms\n", name, tokens, passes,
                        milliseconds.front(), median, milliseconds.back());

                return true;
            }};

    auto ok{end_to_end("total/serial-frequent  ", certifying, frequent, false)};

    ok = end_to_end("total/parallel-frequent", certifying, frequent, true) && ok;

    ok = end_to_end("total/serial-absent    ", certifying, absent, false) && ok;

    ok = end_to_end("total/parallel-absent  ", certifying, absent, true) && ok;

    return ok;
}

/**
 * @brief Measures the thread coordination the parallel scan pays, separately from the scanning it overlaps.
 *
 * tokenize_all_parallel() spawns one jthread per chunk beyond the last and joins them when the scope closes,
 * exactly as timed here with empty bodies. Reporting that cost on its own lets the chunked throughput rows be
 * read as scan time plus a known constant, rather than as an undivided end-to-end number.
 */
void measure_threads(const int passes)
{
    for (const std::size_t threads : {1U, 2U, 4U, 8U})
    {
        std::vector<double> microseconds;

        microseconds.reserve(static_cast<std::size_t>(passes));

        for (int index{0}; index < passes; ++index)
        {
            constexpr std::chrono::milliseconds floor{2};

            std::size_t iterations{0};

            const auto start{std::chrono::steady_clock::now()};

            std::chrono::steady_clock::duration elapsed{};

            do
            {
                {
                    std::vector<std::jthread> workers;

                    workers.reserve(threads - 1);

                    for (std::size_t worker{0}; worker + 1 < threads; ++worker)
                    {
                        workers.emplace_back([] {});
                    }
                }

                ++iterations;

                elapsed = std::chrono::steady_clock::now() - start;
            } while (elapsed < floor);

            microseconds.push_back(
                    std::chrono::duration<double, std::micro>{elapsed}.count() / static_cast<double>(iterations));
        }

        std::sort(microseconds.begin(), microseconds.end());

        const auto median{(microseconds[(microseconds.size() - 1) / 2] + microseconds[microseconds.size() / 2]) / 2.0};

        std::printf(
                "threads/%zu        spawn and join, %d passes: best %.1f, median %.1f, worst %.1f us\n", threads,
                passes, microseconds.front(), median, microseconds.back());
    }
}

} // namespace

/**
 * @brief Measures tokenization throughput over generated pseudo-code.
 *
 * Reports the Lexer on ASCII input, the Tokenizer driver on the same input, the Lexer on input with Greek
 * identifiers matched through UTF-8 byte expansion, and chunked scans split at certified safe split points.
 * Usage: munch_benchmark [input size in MiB] [passes]
 */
// Two serial rows plus four chunk counts on two corpora; the names are generated, so they need storage that
// outlives the scenario list pointing at them.
constexpr std::size_t kScalingRows{10};

constexpr std::size_t kLabelWidth{24};

int main(const int argc, const char** argv)
{
    // A comma-separated list sweeps input sizes, so one run can cross the last-level cache: "1,16,256".
    std::vector<std::size_t> sizes;

    for (const char* cursor{argc > 1 ? argv[1] : "8"}; *cursor != '\0';)
    {
        char* end{nullptr};

        sizes.push_back(std::strtoull(cursor, &end, 10));

        cursor = *end == ',' ? end + 1 : end;
    }

    const int passes{argc > 2 ? std::atoi(argv[2]) : 15};

    const char* const observations{argc > 3 ? argv[3] : nullptr};

    if (sizes.empty() || std::find(sizes.begin(), sizes.end(), std::size_t{0}) != sizes.end() || passes <= 0)
    {
        std::printf("usage: munch_benchmark [input MiB > 0, comma separated] [passes > 0] [observations.csv]\n");

        return EXIT_FAILURE;
    }

    const auto mebibytes{sizes.front()};

    constexpr const char* ascii_identifiers[]{"foo", "bar_baz", "counter", "x1", "value2", "tmp"};

    constexpr const char* greek_identifiers[]{"foo", "αλφα", "counter", "βητα_1", "δελτα", "tmp_ω"};

    const auto ascii_lexer{build_lexer(false)};

    const auto greek_lexer{build_lexer(true)};

    const auto ascii_input{generate_input(mebibytes << 20U, ascii_identifiers)};

    const auto greek_input{generate_input(mebibytes << 20U, greek_identifiers)};

    munch::tools::tokenizer::Tokenizer tokenizer{ascii_lexer, ascii_input};

    auto ok{measure("lexer/ascii", ascii_input.size(), passes, [&ascii_lexer, &ascii_input] {
        return tokenize(ascii_lexer, ascii_input);
    })};

    ok = measure("tokenizer/ascii", ascii_input.size(), passes, [&tokenizer] { return tokenize(tokenizer); }) && ok;

    ok = measure("lexer_all/utf8", greek_input.size(), passes,
                 [&greek_lexer, &greek_input] { return tokenize_all(greek_lexer, greek_input); }) &&
         ok;

    const auto xid_lexer{build_xid_lexer()};

    ok = measure("lexer_all/xid", greek_input.size(), passes,
                 [&xid_lexer, &greek_input] { return tokenize_all(xid_lexer, greek_input); }) &&
         ok;

    const auto source_input{generate_source_input(mebibytes << 20U)};

    measure_build(passes);

    measure_xid_build(passes);

    ok = measure_planning(passes) && ok;

    measure_threads(passes);

    // The scaling scenarios are the ones whose ratios carry the result, so they run interleaved rather than one
    // after another, and they sweep the requested sizes: 16 MiB fits this machine's last-level cache, a larger
    // size does not, and the difference is what says when parallel execution starts paying.
    for (const auto size : sizes)
    {
        const auto dense{size == mebibytes ? ascii_input : generate_input(size << 20U, ascii_identifiers)};

        const auto source{size == mebibytes ? source_input : generate_source_input(size << 20U)};

        ok = validate_chunked(ascii_lexer, dense, 8) && validate_chunked(ascii_lexer, source, 8) && ok;

        std::vector<char> labels(kScalingRows * kLabelWidth);

        std::vector<Scenario> scenarios;

        const auto label{[&labels](const std::size_t row) { return labels.data() + row * kLabelWidth; }};

        std::snprintf(label(0), kLabelWidth, "lexer_all/ascii");

        scenarios.push_back(Scenario{.name = label(0), .bytes = dense.size(), .pass = [&ascii_lexer, &dense] {
                                         return tokenize_all(ascii_lexer, dense);
                                     }});

        std::snprintf(label(1), kLabelWidth, "lexer_all/source");

        scenarios.push_back(Scenario{.name = label(1), .bytes = source.size(), .pass = [&ascii_lexer, &source] {
                                         return tokenize_all(ascii_lexer, source);
                                     }});

        std::size_t row{2};

        // The single-chunk row runs the parallel API without spawning anything, so it separates the cost of that
        // API and its per-chunk sink from the parallelism the other rows add.
        for (const std::size_t threads : {1, 2, 4, 8})
        {
            std::snprintf(label(row), kLabelWidth, "chunked%zu/ascii", threads);

            scenarios.push_back(
                    Scenario{.name = label(row), .bytes = dense.size(), .pass = [&ascii_lexer, &dense, threads] {
                                 return tokenize_chunked(ascii_lexer, dense, threads);
                             }});

            ++row;

            std::snprintf(label(row), kLabelWidth, "chunked%zu/source", threads);

            scenarios.push_back(
                    Scenario{.name = label(row), .bytes = source.size(), .pass = [&ascii_lexer, &source, threads] {
                                 return tokenize_chunked(ascii_lexer, source, threads);
                             }});

            ++row;
        }

        std::printf("\nscaling at %zu MiB, %d interleaved rounds\n", size, passes);

        ok = measure_interleaved(scenarios, passes, size, observations) && ok;
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
