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

} // namespace

/**
 * @brief Measures tokenization throughput over generated pseudo-code.
 *
 * Reports the Lexer on ASCII input, the Tokenizer driver on the same input, the Lexer on input with Greek
 * identifiers matched through UTF-8 byte expansion, and chunked scans split at certified safe split points.
 * Usage: munch_benchmark [input size in MiB] [passes]
 */
int main(const int argc, const char** argv)
{
    const std::size_t mebibytes{argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8};

    const int passes{argc > 2 ? std::atoi(argv[2]) : 15};

    if (mebibytes == 0 || passes <= 0)
    {
        std::printf("usage: munch_benchmark [input size in MiB > 0] [passes > 0]\n");

        return EXIT_FAILURE;
    }

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

    ok = measure("lexer_all/ascii", ascii_input.size(), passes,
                 [&ascii_lexer, &ascii_input] { return tokenize_all(ascii_lexer, ascii_input); }) &&
         ok;

    ok = measure("tokenizer/ascii", ascii_input.size(), passes, [&tokenizer] { return tokenize(tokenizer); }) && ok;

    ok = measure("lexer_all/utf8", greek_input.size(), passes,
                 [&greek_lexer, &greek_input] { return tokenize_all(greek_lexer, greek_input); }) &&
         ok;

    const auto xid_lexer{build_xid_lexer()};

    ok = measure("lexer_all/xid", greek_input.size(), passes,
                 [&xid_lexer, &greek_input] { return tokenize_all(xid_lexer, greek_input); }) &&
         ok;

    const auto source_input{generate_source_input(mebibytes << 20U)};

    ok = validate_chunked(ascii_lexer, ascii_input, 8) && validate_chunked(ascii_lexer, source_input, 8) && ok;

    measure_build(passes);

    measure_xid_build(passes);

    ok = measure("lexer_all/source", source_input.size(), passes,
                 [&ascii_lexer, &source_input] { return tokenize_all(ascii_lexer, source_input); }) &&
         ok;

    for (const std::size_t threads : {2, 4, 8})
    {
        char name[32];

        std::snprintf(name, sizeof(name), "chunked%zu/ascii", threads);

        ok = measure(name, ascii_input.size(), passes,
                     [&ascii_lexer, &ascii_input, threads] {
                         return tokenize_chunked(ascii_lexer, ascii_input, threads);
                     }) &&
             ok;

        std::snprintf(name, sizeof(name), "chunked%zu/source", threads);

        ok = measure(name, source_input.size(), passes,
                     [&ascii_lexer, &source_input, threads] {
                         return tokenize_chunked(ascii_lexer, source_input, threads);
                     }) &&
             ok;
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
