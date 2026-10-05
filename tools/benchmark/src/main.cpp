#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/patterns.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/regex/unicode.hpp"
#include "munch/tools/benchmark/harness.hpp"
#include "munch/tools/tokenizer/tokenizer.hpp"

namespace
{
using namespace munch::tools::benchmark;

/**
 * @brief The chunk count the planning rows, the window-plan row and the scaling validations ask for.
 */
constexpr std::size_t plan_chunks{8};

/**
 * @brief The size of the planning rows' inputs, in MiB.
 */
constexpr std::size_t planning_mebibytes{16};

/**
 * @brief The period of the certified bytes in the planning rows' frequent input.
 */
constexpr std::size_t frequent_period{40};

/**
 * @brief The period of the certified bytes in the planning rows' rare input, one MiB.
 */
constexpr std::size_t rare_period{bytes_of(1)};

/**
 * @brief The shortest sample a plan or a thread spawn is repeated to, so that one clock reading resolves it.
 */
constexpr std::chrono::milliseconds timer_floor{2};

/**
 * @brief The patterns the keyword-scale token set registers: 100 keywords, an identifier, two numbers, a whitespace
 *        run, 28 operators and 11 punctuation bytes.
 */
constexpr std::size_t keyword_scale_patterns{143};

/**
 * @brief The patterns the XID construction rows register: an identifier, a number and a whitespace run.
 */
constexpr std::size_t xid_patterns{3};

/**
 * @brief The width a scaling row's name is held to, one byte more than the longest name reported.
 */
constexpr std::size_t label_width{24};

/**
 * @brief The number of byte values, the symbols a certificate is asked about.
 */
constexpr int byte_values{256};

/**
 * @brief The planning grammars' token kinds.
 */
enum class Planning_token : std::size_t
{
    /**
     * @brief A line's body.
     */
    line,

    /**
     * @brief A newline.
     */
    newline
};

/**
 * @brief A Builder exposing its protected pipeline output, so the compiled automaton's size can be reported.
 */
struct Builder_dbg : munch::core::Builder
{
    /**
     * @brief The Builder's compiled automaton, made public.
     */
    using Builder::dfa;
};

/**
 * @brief The lexers the scaling scenarios run and their corpora at the first size.
 */
struct Scaling_inputs
{
    /**
     * @brief The C-like lexer.
     */
    const munch::core::Lexer& ascii_lexer;

    /**
     * @brief The conventional-row lexer.
     */
    const munch::core::Lexer& conventional;

    /**
     * @brief The dense corpus at the first size.
     */
    const std::string& ascii_input;

    /**
     * @brief The source-shaped corpus at the first size.
     */
    const std::string& source_input;

    /**
     * @brief The conventional-row corpus at the first size.
     */
    const std::string& conventional_input;
};

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
 * Flattened because next() only inlines into the loop on its own when the token type has internal linkage, and whether
 * it inlines swings the measurement by a third.
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
    std::vector<Padded<std::size_t>> counts(chunks);

    const auto consumed{lexer.tokenize_all_parallel<Token>(
            input, chunks,
            [&counts](const std::size_t chunk, const Token, const std::size_t) { ++counts[chunk].value; })};

    const auto boundaries{lexer.chunk_boundaries(input, chunks)};

    std::size_t total{0};

    for (std::size_t chunk{0}; chunk < consumed.size(); ++chunk)
    {
        if (consumed[chunk] != boundaries[chunk + 1] - boundaries[chunk])
        {
            const auto rejected_at{boundaries[chunk] + consumed[chunk]};

            std::printf("chunk %zu rejected at offset %zu\n", chunk, rejected_at);

            return 0;
        }

        total += counts[chunk].value;
    }

    return total;
}

/**
 * @brief Verifies that a plan's chunk-local scans, spliced in order, are identical to the whole-input scan, token for
 *        token.
 *
 * Collects the exact (kind, length) stream of the serial scan and of the chunk-local scans spliced in order; the two
 * agree exactly when every token's kind and boundary match, so identical kinds split at different boundaries cannot
 * slip through the way a kind-only checksum would allow. The plan is made after the serial scan succeeds.
 * @tparam Plan The planner's type.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @param plan Returns the plan's boundaries, both ends included, or std::nullopt when the plan itself is rejected.
 * @param kind The plan's kind, which opens the printed divergence line.
 * @return True if the chunked token stream matches the serial one.
 */
template <typename Plan>
bool validate_plan(
        const munch::core::Lexer& lexer, const std::string& input, const Plan& plan, const std::string_view kind)
{
    using Stream_t = std::vector<std::pair<Token, std::size_t>>;

    const auto collect{[](Stream_t& stream) {
        return [&stream](const Token token, const std::size_t length) { stream.emplace_back(token, length); };
    }};

    Stream_t serial{};

    const auto serial_consumed{lexer.tokenize_all<Token>(input, collect(serial))};

    if (serial_consumed != input.size())
    {
        return false;
    }

    const auto boundaries{plan()};

    if (!boundaries)
    {
        return false;
    }

    Stream_t chunked{};

    for (std::size_t index{0}; index + 1 < boundaries->size(); ++index)
    {
        const auto begin{input.cbegin() + static_cast<std::ptrdiff_t>((*boundaries)[index])};

        const auto end{input.cbegin() + static_cast<std::ptrdiff_t>((*boundaries)[index + 1])};

        const auto consumed{lexer.tokenize_all<Token>(begin, end, collect(chunked))};

        if (consumed != static_cast<std::size_t>(end - begin))
        {
            return false;
        }
    }

    if (chunked != serial)
    {
        std::printf("%s token stream diverged from the serial scan\n", std::string{kind}.c_str());

        return false;
    }

    return true;
}

/**
 * @brief Verifies that the byte-planned chunked tokenization is identical to the whole-input scan.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @param chunks The number of chunks to divide the input into.
 * @return True if the chunked token stream matches the serial one.
 */
bool validate_chunked(const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    const auto plan{[&lexer, &input, chunks] { return std::optional{lexer.chunk_boundaries(input, chunks)}; }};

    return validate_plan(lexer, input, plan, "chunked");
}

/**
 * @brief Builds a keyword-scale token set: 100 keywords plus identifier, literal, operator, and punctuation patterns,
 *        approximating a real language front end.
 * @return The builder holding the token set.
 */
Builder_dbg keyword_scale_builder()
{
    Builder_dbg builder{};

    keyword_scale_tokens(builder);

    return builder;
}

/**
 * @brief Counts the states a compiled automaton names: its initial state and every transition's ends.
 * @param dfa The automaton.
 * @return The number of states.
 */
std::size_t state_count_of(const munch::dfa::Dfa& dfa)
{
    std::set<std::size_t> states{dfa.init_state()};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        states.insert(from);

        states.insert(to);
    }

    return states.size();
}

/**
 * @brief Measures the construction cost of the keyword-scale token set, reported in milliseconds rather than
 *        throughput, together with the compiled automaton's size.
 * @param passes The timed passes.
 */
void measure_build(const int passes)
{
    const auto builder{keyword_scale_builder()};

    std::vector<double> milliseconds{};

    milliseconds.reserve(static_cast<std::size_t>(passes));

    for (int index{0}; index < passes; ++index)
    {
        const auto start{std::chrono::steady_clock::now()};

        std::ignore = builder.build();

        const std::chrono::duration<double, std::milli> elapsed{std::chrono::steady_clock::now() - start};

        milliseconds.push_back(elapsed.count());
    }

    std::ranges::sort(milliseconds);

    const auto dfa{builder.dfa()};

    const auto states{state_count_of(dfa)};

    const auto median{median_of(milliseconds)};

    std::printf(
            "build/keywords   %zu patterns, %zu states, %d passes: best %.1f, median %.1f, worst %.1f ms\n",
            keyword_scale_patterns, states, passes, milliseconds.front(), median, milliseconds.back());
}

/**
 * @brief Builds the Unicode identifier pattern: an underscore or an XID_Start code point, then XID_Continue code
 *        points.
 * @return The pattern.
 */
munch::regex::Regex xid_identifier()
{
    using namespace munch::regex;

    return concat(choice(text('_'), unicode::xid_start()), kleene(unicode::xid_continue()));
}

/**
 * @brief Builds the benchmark lexer with the identifier class drawn from the XID properties.
 *
 * The same grammar as the UTF-8 lexer with the hand-rolled Greek range replaced by the full Unicode identifier
 * definition, so both scenarios tokenize the Greek input to the identical stream and the throughput difference isolates
 * the class size.
 * @return The lexer.
 */
munch::core::Lexer build_xid_lexer()
{
    auto identifier{xid_identifier()};

    return build_lexer(std::move(identifier));
}

/**
 * @brief Measures the construction cost of a Unicode identifier grammar over the XID properties.
 *
 * The identifier pattern expands the two XID property tables, 1497 code point ranges, into byte alternatives: the
 * construction stress case a generated property class poses. Three figures make the whole story visible: register/xid
 * covers expanding the properties into patterns and registering them, build/xid covers finalization through build() as
 * in the keyword-scale measurement, and total/xid is one pass through both.
 * @param passes The timed passes.
 */
void measure_xid_build(const int passes)
{
    using namespace munch::regex;

    std::vector<double> registration{};

    std::vector<double> finalization{};

    registration.reserve(static_cast<std::size_t>(passes));

    finalization.reserve(static_cast<std::size_t>(passes));

    std::size_t state_count{0};

    for (int index{0}; index < passes; ++index)
    {
        const auto start{std::chrono::steady_clock::now()};

        Builder_dbg builder{};

        builder.add_token(xid_identifier(), Token::identifier, 2);

        builder.add_token(patterns::decimal_integer(), Token::number, 1);

        builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 1);

        const auto registered{std::chrono::steady_clock::now()};

        std::ignore = builder.build();

        const auto built{std::chrono::steady_clock::now()};

        const std::chrono::duration<double, std::milli> registering{registered - start};

        const std::chrono::duration<double, std::milli> finalizing{built - registered};

        registration.push_back(registering.count());

        finalization.push_back(finalizing.count());

        if (index == 0)
        {
            const auto dfa{builder.dfa()};

            state_count = state_count_of(dfa);
        }
    }

    const auto report{[passes, state_count](const std::string_view name, std::vector<double> milliseconds) {
        std::ranges::sort(milliseconds);

        const auto median{median_of(milliseconds)};

        std::printf(
                "%s     %zu patterns, %zu states, %d passes: best %.1f, median %.1f, worst %.1f ms\n",
                std::string{name}.c_str(), xid_patterns, state_count, passes, milliseconds.front(), median,
                milliseconds.back());
    }};

    std::vector<double> total{};

    total.reserve(registration.size());

    std::ranges::transform(registration, finalization, std::back_inserter(total), std::plus{});

    report("register/xid", std::move(registration));

    report("build/xid   ", std::move(finalization));

    report("total/xid   ", std::move(total));
}

/**
 * @brief Times the plan alone and prints its figures.
 *
 * A plan over a dense certificate costs a fraction of a microsecond, which one clock reading cannot resolve, so each
 * pass repeats the plan until the sample outlasts the timer and divides. A plan that scans the whole input passes the
 * floor on its first call.
 * @param passes The timed passes.
 * @param name The row's name.
 * @param lexer The lexer planned with.
 * @param text The text planned over.
 */
void time_plan(const int passes, const std::string_view name, const munch::core::Lexer& lexer, const std::string& text)
{
    std::vector<double> microseconds{};

    microseconds.reserve(static_cast<std::size_t>(passes));

    std::size_t planned{0};

    for (int index{0}; index < passes; ++index)
    {
        std::size_t iterations{0};

        const auto start{std::chrono::steady_clock::now()};

        std::chrono::steady_clock::duration elapsed{};

        do
        {
            planned = lexer.chunk_boundaries(text, plan_chunks).size();

            ++iterations;

            elapsed = std::chrono::steady_clock::now() - start;
        } while (elapsed < timer_floor);

        const std::chrono::duration<double, std::micro> sample{elapsed};

        const auto per_plan{sample.count() / static_cast<double>(iterations)};

        microseconds.push_back(per_plan);
    }

    std::ranges::sort(microseconds);

    const auto median{median_of(microseconds)};

    const auto planned_chunks{planned - 1};

    std::printf(
            "%s %zu of %zu chunks, %d passes: best %.1f, median %.1f, worst %.1f us\n", std::string{name}.c_str(),
            planned_chunks, plan_chunks, passes, microseconds.front(), median, microseconds.back());
}

/**
 * @brief Times planning and scanning together over one grammar and one input and prints the row's figures.
 *
 * Every pass is checked to have consumed its whole span and emitted the same count as the first, so a pass that stopped
 * early fails the row rather than printing a faster timing. The parallel row times the whole cost a caller pays for
 * choosing the parallel entry point: planning, then scanning.
 * @param passes The timed passes.
 * @param name The row's name.
 * @param lexer The lexer scanned with.
 * @param text The text scanned.
 * @param parallel Whether the row scans through the parallel entry point rather than serially.
 * @return True when every pass consumed its whole span and emitted the same token count.
 */
bool time_end_to_end(
        const int passes, const std::string_view name, const munch::core::Lexer& lexer, const std::string& text,
        const bool parallel)
{
    // One counter per chunk, each on its own cache line, so the workers' increments neither race nor share a line.
    using Counter = Padded<std::size_t>;

    const std::string label{name};

    std::vector<double> milliseconds{};

    milliseconds.reserve(static_cast<std::size_t>(passes));

    std::size_t tokens{0};

    const auto boundaries{lexer.chunk_boundaries(text, plan_chunks)};

    const auto serial_consumed_whole{[&label, &text](const std::size_t consumed) {
        if (consumed == text.size())
        {
            return true;
        }

        std::printf("%s tokenized %zu of %zu bytes\n", label.c_str(), consumed, text.size());

        return false;
    }};

    const auto chunks_consumed_whole{[&label, &boundaries](const std::vector<std::size_t>& chunk_consumed) {
        if (chunk_consumed.size() + 1 != boundaries.size())
        {
            const auto planned{boundaries.size() - 1};

            std::printf("%s planned %zu chunks but scanned %zu\n", label.c_str(), planned, chunk_consumed.size());

            return false;
        }

        for (std::size_t chunk{0}; chunk < chunk_consumed.size(); ++chunk)
        {
            if (const auto span{boundaries[chunk + 1] - boundaries[chunk]}; chunk_consumed[chunk] != span)
            {
                std::printf(
                        "%s chunk %zu tokenized %zu of %zu bytes\n", label.c_str(), chunk, chunk_consumed[chunk], span);

                return false;
            }
        }

        return true;
    }};

    for (int index{0}; index < passes; ++index)
    {
        std::vector<Counter> counters(plan_chunks);

        std::size_t serial_consumed{0};

        std::vector<std::size_t> chunk_consumed{};

        const auto start{std::chrono::steady_clock::now()};

        if (parallel)
        {
            chunk_consumed = lexer.tokenize_all_parallel<Planning_token>(
                    text, plan_chunks,
                    [&counters](const std::size_t chunk, Planning_token, std::size_t) { ++counters[chunk].value; });
        }
        else
        {
            serial_consumed = lexer.tokenize_all<Planning_token>(
                    text, [&counters](Planning_token, std::size_t) { ++counters[0].value; });
        }

        const auto elapsed{std::chrono::steady_clock::now() - start};

        const std::chrono::duration<double, std::milli> sample{elapsed};

        milliseconds.push_back(sample.count());

        const auto counted{
                std::ranges::fold_left(counters | std::views::transform(&Counter::value), std::size_t{0}, std::plus{})};

        const auto consumed_whole{
                parallel ? chunks_consumed_whole(chunk_consumed) : serial_consumed_whole(serial_consumed)};

        if (!consumed_whole)
        {
            return false;
        }

        if (index != 0 && counted != tokens)
        {
            std::printf("%s emitted %zu tokens, expected %zu\n", label.c_str(), counted, tokens);

            return false;
        }

        tokens = counted;
    }

    std::ranges::sort(milliseconds);

    const auto median{median_of(milliseconds)};

    std::printf(
            "%s %zu tokens, %d passes: best %.1f, median %.1f, worst %.1f ms\n", label.c_str(), tokens, passes,
            milliseconds.front(), median, milliseconds.back());

    return true;
}

/**
 * @brief Measures the cost of planning chunk boundaries as certified symbols grow scarce.
 *
 * chunk_boundaries() slides each interior boundary forward from its equal-division target to the next certified byte,
 * so its cost depends on how far it has to look. Four densities bound the range: a certified byte on almost every line,
 * one every megabyte, a certificate whose byte never occurs in the input at all, and a token set that certifies
 * nothing, which the planner answers without scanning. The planned input is never tokenized by the plan-only rows; only
 * the plan is timed, and each pass repeats the plan until the sample is long enough to divide by, so the cheap
 * densities measure planning rather than the clock. The end-to-end rows then time planning and scanning over the same
 * grammar and the same input.
 * @param passes The timed passes.
 * @return True when every end-to-end pass consumed its whole span and emitted the same token count.
 */
bool measure_planning(const int passes)
{
    using namespace munch::regex;

    // Newline as its own token certifies it; the line body cannot contain one, which is what makes the split safe.
    const auto certifying{[] {
        munch::core::Builder builder{};

        builder.add_token(plus(any_of(Set::all() - '\n')), Planning_token::line, 2);

        builder.add_token(text("\n"), Planning_token::newline, 1);

        return builder.build();
    }()};

    // A line body that may contain any byte certifies nothing: every byte is consumable mid-token.
    const auto certifying_nothing{[] {
        munch::core::Builder builder{};

        builder.add_token(plus(any_of(Set::all())), Planning_token::line, 1);

        return builder.build();
    }()};

    // Certified bytes sit half a period ahead of every equal-division target, so each search scans half a period.
    const auto input{[](const std::size_t size, const std::size_t period) {
        std::string result(size, 'x');

        for (std::size_t offset{period / 2}; period != 0 && offset < size; offset += period)
        {
            result[offset] = '\n';
        }

        return result;
    }};

    constexpr std::size_t planning_bytes{bytes_of(planning_mebibytes)};

    const auto frequent{input(planning_bytes, frequent_period)};

    const auto rare{input(planning_bytes, rare_period)};

    const auto absent{input(planning_bytes, 0)};

    time_plan(passes, "plan/frequent  ", certifying, frequent);

    time_plan(passes, "plan/rare      ", certifying, rare);

    time_plan(passes, "plan/absent    ", certifying, absent);

    time_plan(passes, "plan/uncertified", certifying_nothing, frequent);

    auto ok{time_end_to_end(passes, "total/serial-frequent  ", certifying, frequent, false)};

    ok = time_end_to_end(passes, "total/parallel-frequent", certifying, frequent, true) && ok;

    ok = time_end_to_end(passes, "total/serial-absent    ", certifying, absent, false) && ok;

    ok = time_end_to_end(passes, "total/parallel-absent  ", certifying, absent, true) && ok;

    return ok;
}

/**
 * @brief Measures the thread coordination the parallel scan pays, separately from the scanning it overlaps.
 *
 * tokenize_all_parallel() spawns one jthread per chunk beyond the last and joins them when the scope closes, exactly as
 * timed here with empty bodies. Reporting that cost on its own lets the chunked throughput rows be read as scan time
 * plus a known constant, rather than as an undivided end-to-end number.
 * @param passes The timed passes.
 */
void measure_threads(const int passes)
{
    for (const std::size_t threads : {1U, 2U, 4U, 8U})
    {
        std::vector<double> microseconds{};

        microseconds.reserve(static_cast<std::size_t>(passes));

        for (int index{0}; index < passes; ++index)
        {
            std::size_t iterations{0};

            const auto start{std::chrono::steady_clock::now()};

            std::chrono::steady_clock::duration elapsed{};

            do
            {
                {
                    std::vector<std::jthread> workers{};

                    workers.reserve(threads - 1);

                    for (std::size_t worker{0}; worker + 1 < threads; ++worker)
                    {
                        workers.emplace_back([] {});
                    }
                }

                ++iterations;

                elapsed = std::chrono::steady_clock::now() - start;
            } while (elapsed < timer_floor);

            const std::chrono::duration<double, std::micro> sample{elapsed};

            const auto per_spawn{sample.count() / static_cast<double>(iterations)};

            microseconds.push_back(per_spawn);
        }

        std::ranges::sort(microseconds);

        const auto median{median_of(microseconds)};

        std::printf(
                "threads/%zu        spawn and join, %d passes: best %.1f, median %.1f, worst %.1f us\n", threads,
                passes, microseconds.front(), median, microseconds.back());
    }
}

/**
 * @brief Tokenizes JSON input serially, reporting 0 if any byte was rejected.
 * @param lexer The JSON lexer.
 * @param input The input.
 * @return The tokens matched, or 0 if the input was rejected.
 */
std::size_t tokenize_json(const munch::core::Lexer& lexer, const std::string& input)
{
    std::size_t tokens{0};

    const auto consumed{
            lexer.tokenize_all<Json_token>(input, [&tokens](const Json_token, const std::size_t) { ++tokens; })};

    if (consumed != input.size())
    {
        std::printf("json input rejected at offset %zu\n", consumed);

        return 0;
    }

    return tokens;
}

/**
 * @brief Reports how many split points a corpus actually contains, and how far apart they lie.
 *
 * A certificate says which bytes are safe; it does not say whether the corpus contains any. This separates the two,
 * because a grammar admitting a byte the data never carries yields no parallelism at all.
 * @param name The corpus's name, which opens the printed line.
 * @param lexer The lexer, its discarded tokens set.
 * @param input The corpus.
 */
void report_density(const std::string_view name, const munch::core::Lexer& lexer, const std::string& input)
{
    std::size_t hits{0};

    std::size_t previous{0};

    std::size_t widest{0};

    for (std::size_t offset{0}; offset < input.size(); ++offset)
    {
        if (lexer.is_split_point_ignoring(input[offset]))
        {
            ++hits;

            widest = std::max(widest, offset - previous);

            previous = offset;
        }
    }

    const auto mean{hits == 0 ? 0.0 : static_cast<double>(input.size()) / static_cast<double>(hits)};

    std::printf(
            "%-16s %zu split points, mean gap %.1f, widest gap %zu\n", std::string{name}.c_str(), hits, mean, widest);
}

/**
 * @brief Measures the JSON corpora, where the exact certificate is empty and the relaxed one is not.
 *
 * The two shapes recognize the same documents and differ only in whitespace, which isolates the corpus from the
 * grammar: the certificate is identical for both, and only one of them carries a byte it admits. Throughput is reported
 * serially because chunk_boundaries() plans with the exact certificate, which JSON leaves empty, so the parallel entry
 * point divides either shape into a single chunk however many newlines it holds.
 * @param mebibytes The corpora's size in MiB.
 * @param passes The timed passes.
 * @return True when every pass tokenized its corpus completely and consistently.
 */
bool measure_json(const std::size_t mebibytes, const int passes)
{
    const auto exact{build_json_lexer(false)};

    const auto relaxed{build_json_lexer(true)};

    const auto symbols{std::views::iota(0, byte_values)};

    const auto exactly_certified{
            [&exact](const int symbol) { return exact.is_split_point(static_cast<char>(symbol)); }};

    const auto relaxed_certified{
            [&relaxed](const int symbol) { return relaxed.is_split_point_ignoring(static_cast<char>(symbol)); }};

    const auto exact_bytes{static_cast<std::size_t>(std::ranges::count_if(symbols, exactly_certified))};

    const auto relaxed_bytes{static_cast<std::size_t>(std::ranges::count_if(symbols, relaxed_certified))};

    std::printf("\njson certificate: %zu bytes exact, %zu modulo discarded whitespace\n", exact_bytes, relaxed_bytes);

    const auto bytes{bytes_of(mebibytes)};

    const auto pretty{generate_json_input(bytes, true)};

    const auto minified{generate_json_input(bytes, false)};

    report_density("json/pretty", relaxed, pretty);

    report_density("json/minified", relaxed, minified);

    auto ok{measure(
            "lexer_all/pretty", pretty.size(), passes, [&relaxed, &pretty] { return tokenize_json(relaxed, pretty); })};

    ok = measure("lexer_all/minified", minified.size(), passes,
                 [&relaxed, &minified] { return tokenize_json(relaxed, minified); }) &&
         ok;

    return ok;
}

/**
 * @brief Builds the conventional-row lexer of the windows study: no byte certifies once strings, comments, and
 *        newline-crossing whitespace runs are present, and the two-byte window of a newline before an operator
 *        certifies at the operator, so this is the grammar whose parallel plan exists only through windows.
 * @return The lexer.
 */
munch::core::Lexer conventional_lexer()
{
    using namespace munch::regex;

    munch::core::Builder builder{};

    builder.add_token(ascii_identifier(), Token::identifier, 2);

    builder.add_token(plus(any_of(Set::digits())), Token::number, 2);

    builder.add_token(concat(text(R"(")"), kleene(any_of(Set::printable())), text(R"(")")), Token::string_, 2);

    builder.add_token(concat(text("//"), kleene(any_of(Set::all() - '\n'))), Token::comment, 1);

    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token::whitespace, 1);

    for (const std::string_view op : {"!", "=", "+", ";", "(", ")", "{", "}"})
    {
        builder.add_token(text(op), Token::operator_, 3);
    }

    return builder.build();
}

/**
 * @brief Generates source-like text for the conventional row, with operator-initial lines throughout so the certified
 *        newline-then-operator window occurs near every equal-division target.
 * @param bytes The least size of the text.
 * @return The text.
 */
std::string generate_conventional_input(const std::size_t bytes)
{
    std::string input{};

    input.reserve(bytes + 64);

    static constexpr std::array<std::string_view, 4> lines{
            "count = count + 42; // trailing note\n",
            "!(flag) name = \"a (string) with // inside\";\n",
            "!done = 1;\n",
            "total = total + count; // fold\n",
    };

    for (std::size_t index{0}; input.size() < bytes; ++index)
    {
        input += lines[index % lines.size()];
    }

    return input;
}

/**
 * @brief Tokenizes the input in parallel chunks cut by the window planner.
 *
 * The plan runs inside the pass, exactly as tokenize_all_parallel() replans on every call, so the byte-planned and
 * window-planned rows carry their planning cost identically and their ratio compares like with like.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @param chunks The number of chunks the plan asks for.
 * @return The total number of tokens matched, or 0 if any chunk was rejected.
 */
std::size_t windowed_chunked(const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    struct Count
    {
        std::size_t tokens{0};
        std::size_t consumed{0};
    };

    const auto boundaries{lexer.chunk_boundaries_with_windows(input, chunks)};

    std::vector<Padded<Count>> counts(boundaries.size() - 1);

    {
        std::vector<std::jthread> workers{};

        workers.reserve(counts.size());

        for (std::size_t index{0}; index < counts.size(); ++index)
        {
            workers.emplace_back([&lexer, &input, &boundaries, &counts, index] {
                const auto begin{input.cbegin() + static_cast<std::ptrdiff_t>(boundaries[index])};

                const auto end{input.cbegin() + static_cast<std::ptrdiff_t>(boundaries[index + 1])};

                counts[index].value.consumed = lexer.tokenize_all<Token>(
                        begin, end, [&counts, index](const Token, const std::size_t) { ++counts[index].value.tokens; });
            });
        }
    }

    std::size_t tokens{0};

    std::size_t consumed{0};

    for (const auto& [count] : counts)
    {
        tokens += count.tokens;

        consumed += count.consumed;
    }

    return consumed == input.size() ? tokens : 0;
}

/**
 * @brief Checks that the window-planned chunk streams rejoin to the serial scan and the plan is complete.
 *
 * A degenerate plan would time the serial scan under a parallel label, so the expected boundary count is part of the
 * validation, not only stream equality.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @param chunks The number of chunks the plan asks for.
 * @return True if the plan is complete and the chunked token stream matches the serial one.
 */
bool validate_windowed(const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    const auto plan{[&lexer, &input, chunks]() -> std::optional<std::vector<std::size_t>> {
        auto boundaries{lexer.chunk_boundaries_with_windows(input, chunks)};

        const auto expected{chunks + 1};

        if (boundaries.size() != expected)
        {
            std::printf("window plan found %zu boundaries where %zu were expected\n", boundaries.size(), expected);

            return std::nullopt;
        }

        return boundaries;
    }};

    return validate_plan(lexer, input, plan, "windowed");
}

/**
 * @brief Reads a comma-separated list of counts, each read as std::strtoull reads it.
 * @param list The list.
 * @return The counts, in list order.
 */
std::vector<std::size_t> counts_of(const std::string_view list)
{
    const std::string text{list};

    std::vector<std::size_t> counts{};

    for (std::size_t at{0}; at < text.size();)
    {
        char* end{nullptr};

        counts.push_back(std::strtoull(text.c_str() + at, &end, 10));

        const auto parsed{static_cast<std::size_t>(end - text.c_str())};

        at = parsed < text.size() && text[parsed] == ',' ? parsed + 1 : parsed;
    }

    return counts;
}

/**
 * @brief Returns a scaling row's name as the row list prints it, cut to label_width - 1 bytes when longer.
 * @param name The generated name.
 * @return The name the row reports.
 */
std::string label_of(std::string name)
{
    if (name.size() >= label_width)
    {
        name.resize(label_width - 1);
    }

    return name;
}

/**
 * @brief Measures the scaling scenarios in interleaved rounds at every requested size, each size's plans validated
 *        first.
 *
 * The scaling scenarios are the ones whose ratios carry the result, so they run interleaved rather than one after
 * another, and they sweep the requested sizes: 16 MiB fits this machine's last-level cache, a larger size does not, and
 * the difference is what says when parallel execution starts paying.
 * @param sizes The input sizes in MiB, the first one the size of the given corpora.
 * @param chunk_counts The chunk counts of the chunked and windowed rows.
 * @param passes The interleaved rounds.
 * @param observations The CSV every observation is appended to, std::nullopt for none.
 * @param inputs The lexers and their corpora at the first size.
 * @return True when every plan validated and every scenario reproduced its warmup result.
 */
bool measure_scaling(
        const std::span<const std::size_t> sizes, const std::span<const std::size_t> chunk_counts, const int passes,
        const std::optional<std::string_view> observations, const Scaling_inputs& inputs)
{
    const auto& [ascii_lexer, conventional, ascii_input, source_input, conventional_input]{inputs};

    const auto mebibytes{sizes.front()};

    auto ok{true};

    for (const auto size : sizes)
    {
        const auto bytes{bytes_of(size)};

        const auto dense{size == mebibytes ? ascii_input : generate_input(bytes, ascii_identifiers)};

        const auto source{size == mebibytes ? source_input : generate_source_input(bytes)};

        const auto conventional_corpus{size == mebibytes ? conventional_input : generate_conventional_input(bytes)};

        const auto dense_valid{validate_chunked(ascii_lexer, dense, plan_chunks)};

        // The source corpus is validated only once the dense one passed.
        const auto chunked_valid{dense_valid && validate_chunked(ascii_lexer, source, plan_chunks)};

        ok = chunked_valid && ok;

        ok = validate_windowed(conventional, conventional_corpus, plan_chunks) && ok;

        // Two serial rows plus the chunk counts on two corpora, then the windowed rows. The names are generated, so
        // they live in a deque that outlives the scenario list naming them and never moves a name it holds.
        std::deque<std::string> labels{};

        // Keeps the generated name alive in labels and returns the view the scenario reports.
        const auto label{[&labels](std::string name) -> std::string_view {
            labels.push_back(label_of(std::move(name)));

            return labels.back();
        }};

        std::vector<Scenario> scenarios{};

        const auto ascii_name{label("lexer_all/ascii")};

        scenarios.push_back(Scenario{.name = ascii_name, .bytes = dense.size(), .pass = [&ascii_lexer, &dense] {
                                         return tokenize_all(ascii_lexer, dense);
                                     }});

        const auto serial_source_name{label("lexer_all/source")};

        scenarios.push_back(
                Scenario{.name = serial_source_name, .bytes = source.size(), .pass = [&ascii_lexer, &source] {
                             return tokenize_all(ascii_lexer, source);
                         }});

        // The single-chunk row runs the parallel API without spawning anything, so it separates the cost of that API
        // and its per-chunk sink from the parallelism the other rows add.
        for (const std::size_t threads : chunk_counts)
        {
            const auto dense_name{label(std::format("chunked{}/ascii", threads))};

            scenarios.push_back(
                    Scenario{.name = dense_name, .bytes = dense.size(), .pass = [&ascii_lexer, &dense, threads] {
                                 return tokenize_chunked(ascii_lexer, dense, threads);
                             }});

            const auto source_name{label(std::format("chunked{}/source", threads))};

            scenarios.push_back(
                    Scenario{.name = source_name, .bytes = source.size(), .pass = [&ascii_lexer, &source, threads] {
                                 return tokenize_chunked(ascii_lexer, source, threads);
                             }});
        }

        const auto conventional_name{label("lexer_all/conv")};

        scenarios.push_back(Scenario{
                .name = conventional_name,
                .bytes = conventional_corpus.size(),
                .pass = [&conventional, &conventional_corpus] {
                    return tokenize_all(conventional, conventional_corpus);
                }});

        for (const std::size_t threads : chunk_counts)
        {
            const auto windowed_name{label(std::format("windowed{}/conv", threads))};

            scenarios.push_back(Scenario{
                    .name = windowed_name,
                    .bytes = conventional_corpus.size(),
                    .pass = [&conventional, &conventional_corpus, threads] {
                        return windowed_chunked(conventional, conventional_corpus, threads);
                    }});
        }

        std::printf("\nscaling at %zu MiB, %d interleaved rounds\n", size, passes);

        ok = measure_interleaved(scenarios, passes, size, observations) && ok;
    }

    return ok;
}

} // namespace

/**
 * @brief Measures tokenization throughput over generated pseudo-code.
 *
 * Reports the Lexer on ASCII input, the Tokenizer driver on the same input, the Lexer on input with Greek identifiers
 * matched through UTF-8 byte expansion, and chunked scans split at certified safe split points. Usage: munch_benchmark
 * [input sizes in MiB, comma separated] [passes] [observations CSV] [chunk counts, comma separated]
 * @param argc The argument count.
 * @param argv The input sizes, the passes, the observations CSV and the chunk counts, all optional.
 * @return EXIT_SUCCESS when every scenario reproduced its warmup result, EXIT_FAILURE otherwise.
 */
int main(const int argc, char** argv)
{
    // A comma-separated list sweeps input sizes, so one run can cross the last-level cache: "1,16,256".
    const auto sizes{counts_of(argc > 1 ? argv[1] : "8")};

    const int passes{argc > 2 ? std::atoi(argv[2]) : default_passes};

    const auto observations{argc > 3 ? std::optional<std::string_view>{argv[3]} : std::nullopt};

    // An optional comma-separated chunk list widens the scaling sweep past the default 1,2,4,8: the pinned thread sweep
    // wants 3, 6, 12, and 16 to map where per-core efficiency falls on a hybrid part.
    const auto chunk_counts{counts_of(argc > 4 ? argv[4] : "1,2,4,8")};

    if (chunk_counts.empty() || std::ranges::contains(chunk_counts, std::size_t{0}))
    {
        std::fprintf(stderr, "chunk counts must be positive\n");

        return EXIT_FAILURE;
    }

    print_provenance("munch benchmark", passes, observations);

    if (sizes.empty() || std::ranges::contains(sizes, std::size_t{0}) || passes <= 0)
    {
        std::printf("usage: munch_benchmark [input MiB > 0, comma separated] [passes > 0] [observations.csv]\n");

        return EXIT_FAILURE;
    }

    const auto mebibytes{sizes.front()};

    const auto ascii_lexer{build_lexer(false)};

    const auto greek_lexer{build_lexer(true)};

    const auto bytes{bytes_of(mebibytes)};

    const auto ascii_input{generate_input(bytes, ascii_identifiers)};

    constexpr std::array<std::string_view, 6> greek_identifiers{"foo", "αλφα", "counter", "βητα_1", "δελτα", "tmp_ω"};

    const auto greek_input{generate_input(bytes, greek_identifiers)};

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

    const auto source_input{generate_source_input(bytes)};

    const auto conventional{conventional_lexer()};

    const auto conventional_input{generate_conventional_input(bytes)};

    measure_build(passes);

    measure_xid_build(passes);

    ok = measure_planning(passes) && ok;

    // The conventional row certifies no byte, so its plan exists only through windows; the plan alone is timed against
    // the corpus it serves, pricing planning as overhead per input byte rather than in the abstract.
    ok = measure("window_plan/conv", conventional_input.size(), passes,
                 [&conventional, &conventional_input] {
                     return conventional.chunk_boundaries_with_windows(conventional_input, plan_chunks).size();
                 }) &&
         ok;

    ok = measure_json(mebibytes, passes) && ok;

    measure_threads(passes);

    const Scaling_inputs scaling_inputs{
            .ascii_lexer = ascii_lexer,
            .conventional = conventional,
            .ascii_input = ascii_input,
            .source_input = source_input,
            .conventional_input = conventional_input};

    ok = measure_scaling(sizes, chunk_counts, passes, observations, scaling_inputs) && ok;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
