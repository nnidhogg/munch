#include "munch/tools/benchmark/harness.hpp"

#include <sys/utsname.h>

#include <array>
#include <ctime>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <string_view>
#include <thread>

#include "munch/core/builder.hpp"
#include "munch/regex/patterns.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/utf8.hpp"
#include "munch/tools/benchmark/provenance.hpp"

namespace munch::tools::benchmark
{
namespace
{
/**
 * @brief One timed pass of one scenario.
 */
struct Observation
{
    /**
     * @brief The scenario's index.
     */
    std::size_t scenario{0};

    /**
     * @brief The round the pass ran in.
     */
    int round{0};

    /**
     * @brief The pass's wall-clock time.
     */
    double seconds{0.0};
};

} // namespace

unsigned Corpus_random::operator()() noexcept
{
    return next_random(seed_, corpus_dropped_bits);
}

regex::Regex ascii_identifier()
{
    using namespace munch::regex;

    auto identifier{concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_')))};

    return identifier;
}

core::Lexer build_lexer(const bool greek_identifiers)
{
    using namespace munch::regex;

    const auto widened{[greek_identifiers](Regex ascii) {
        return greek_identifiers ? choice(std::move(ascii), utf8::range(U'Α', U'ω')) : std::move(ascii);
    }};

    auto identifier{concat(widened(any_of(Set::alpha() + '_')), kleene(widened(any_of(Set::alphanum() + '_'))))};

    return build_lexer(std::move(identifier));
}

core::Lexer build_lexer(regex::Regex identifier)
{
    using namespace munch::regex;

    core::Builder builder{};

    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 2);

    builder.add_token(std::move(identifier), Token::identifier, 2);

    builder.add_token(plus(any_of(Set::digits())), Token::number, 2);

    // Keywords outrank identifiers, exercising priority resolution on equal-length matches.
    builder.add_token(choice(text("if"), text("else"), text("while"), text("return"), text("int")), Token::keyword, 1);

    builder.add_token(
            choice(text("=="), text("!="), text("<="), text(">="), text("+"), text("-"), text("*"), text("/"),
                   text("="), text("<"), text(">")),
            Token::operator_, 2);

    builder.add_token(any_of(Set{'(', ')', '{', '}', ';', ','}), Token::punctuation, 2);

    return builder.build();
}

void keyword_scale_tokens(core::Builder& builder)
{
    using namespace munch::regex;

    // Roughly the C++ keyword set plus common fixed-width type names: 100 entries.
    static constexpr std::array<std::string_view, 100> keywords{
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

    for (const auto keyword : keywords)
    {
        builder.add_token(text(keyword), Token::keyword, 1);
    }

    builder.add_token(ascii_identifier(), Token::identifier, 2);

    builder.add_token(patterns::decimal_float(), Token::number, 1);

    builder.add_token(patterns::decimal_integer(), Token::number, 1);

    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 1);

    for (const std::string_view op : {"==", "!=", "<=", ">=", "<<", ">>", "&&", "||", "++", "--",
                                      "->", "+=", "-=", "*=", "/=", "+",  "-",  "*",  "/",  "%",
                                      "=",  "<",  ">",  "!",  "~",  "&",  "|",  "^"})
    {
        builder.add_token(text(op), Token::operator_, 2);
    }

    for (const std::string_view punct : {"(", ")", "{", "}", "[", "]", ";", ",", ".", ":", "?"})
    {
        builder.add_token(text(punct), Token::punctuation, 2);
    }
}

std::string generate_input(const std::size_t size, const std::span<const std::string_view> identifiers)
{
    std::string input{};

    input.reserve(size + 128);

    // A fixed-seed linear congruential generator keeps the input identical across runs and builds.
    Corpus_random random{};

    const auto draw_identifier{[&] { return identifiers[random() % identifiers.size()]; }};

    while (input.size() < size)
    {
        input += "while (";
        input += draw_identifier();
        input += " <= ";
        input += std::to_string(random() % 100000U);
        input += ") { ";
        input += draw_identifier();
        input += " = ";
        input += draw_identifier();
        input += " + ";
        input += std::to_string(random() % 997U);
        input += "; if (x1 != 42) { return counter; } }\n";
    }

    return input;
}

std::string generate_source_input(const std::size_t size)
{
    std::string input{};

    input.reserve(size + 256);

    // The same fixed-seed generator as generate_input(), so ports stay byte-identical.
    Corpus_random random{};

    constexpr std::array<std::string_view, 10> identifiers{
            "configuration_manager",
            "total_element_count",
            "process_next_request",
            "buffer_capacity",
            "initialize_state_machine",
            "compute_partial_checksum",
            "validation_result",
            "iterator_position",
            "acc",
            "idx"};

    const auto draw_identifier{[&] { return identifiers[random() % identifiers.size()]; }};

    while (input.size() < size)
    {
        input += "while (";
        input += draw_identifier();
        input += " <= ";
        input += std::to_string(random() % 10000000U);
        input += ") {\n    ";
        input += draw_identifier();
        input += " = ";
        input += draw_identifier();
        input += " * ";
        input += draw_identifier();
        input += " + ";
        input += std::to_string(random() % 100000U);
        input += ";\n    if (";
        input += draw_identifier();
        input += " != ";
        input += std::to_string(random() % 997U);
        input += ") { return ";
        input += draw_identifier();
        input += "; }\n}\n";
    }

    return input;
}

core::Lexer build_json_lexer(const bool discard_whitespace)
{
    using namespace munch::regex;

    core::Builder builder{};

    // ws = *( %x20 / %x09 / %x0A / %x0D ).
    builder.add_token(plus(any_of(Set{' ', '\t', '\n', '\r'})), Json_token::whitespace, 2);

    // unescaped = %x20-21 / %x23-5B / %x5D-10FFFF, taken over bytes, so the tail covers every continuation byte.
    const auto unescaped{Set::range(0x20, 0x21) + Set::range(0x23, 0x5B) + Set::range(0x5D, 0xFF)};

    const auto hex{Set::digits() + Set::range('a', 'f') + Set::range('A', 'F')};

    const auto hex_digit{any_of(hex)};

    const auto two_hex_digits{concat(hex_digit, hex_digit)};

    const auto three_hex_digits{concat(hex_digit, two_hex_digits)};

    const auto unicode_escape{concat(text("u"), concat(hex_digit, three_hex_digits))};

    const auto escape{
            concat(text(R"(\)"), choice(any_of(Set{'"', '\\', '/', 'b', 'f', 'n', 'r', 't'}), unicode_escape))};

    const auto string_body{kleene(choice(any_of(unescaped), escape))};

    builder.add_token(concat(text(R"(")"), concat(string_body, text(R"(")"))), Json_token::string, 2);

    const auto digits{plus(any_of(Set::digits()))};

    const auto integer{choice(text("0"), concat(any_of(Set::range('1', '9')), kleene(any_of(Set::digits()))))};

    const auto exponent{concat(any_of(Set{'e', 'E'}), concat(optional(any_of(Set{'+', '-'})), digits))};

    const auto fraction{concat(text("."), digits)};

    const auto fraction_and_exponent{concat(optional(fraction), optional(exponent))};

    const auto number{concat(optional(text("-")), concat(integer, fraction_and_exponent))};

    builder.add_token(number, Json_token::number, 2);

    builder.add_token(choice(text("true"), text("false"), text("null")), Json_token::literal, 1);

    builder.add_token(any_of(Set{'{', '}', '[', ']', ':', ','}), Json_token::structural, 2);

    if (discard_whitespace)
    {
        builder.set_ignored_tokens({Json_token::whitespace});
    }

    return builder.build();
}

std::string generate_json_input(const std::size_t size, const bool pretty)
{
    const std::string_view line_end{pretty ? "\n" : ""};

    const std::string_view indent{pretty ? "    " : ""};

    const std::string_view gap{pretty ? " " : ""};

    std::string input{};

    input.reserve(size + 256);

    input += "[";
    input += line_end;

    // The same fixed-seed generator as generate_input(), so both shapes carry the same values in the same order.
    Corpus_random random{};

    constexpr std::array<std::string_view, 6> keys{"configuration_manager", "total_element_count",
                                                   "process_next_request",  "buffer_capacity",
                                                   "validation_result",     "iterator_position"};

    const auto draw_key{[&] { return keys[random() % keys.size()]; }};

    while (input.size() < size)
    {
        input += indent;
        input += "{";
        input += line_end;

        constexpr std::size_t fields{6};

        for (std::size_t field{0}; field < fields; ++field)
        {
            input += indent;
            input += indent;
            input += R"(")";
            input += draw_key();
            input += R"(":)";
            input += gap;

            switch (random() % 4U)
            {
            case 0:
                input += std::to_string(random() % 1000000U);

                break;

            case 1:
                input += R"(")";
                input += draw_key();
                input += R"(")";

                break;

            case 2:
                input += random() % 2U != 0 ? "true" : "false";

                break;

            default:
                input += "null";

                break;
            }

            if (field + 1 < fields)
            {
                input += ",";
            }

            input += line_end;
        }

        input += indent;
        input += "},";
        input += line_end;
    }

    // A trailing empty object keeps the document well formed however the loop above happened to stop.
    input += indent;
    input += "{}";
    input += line_end;
    input += "]";

    return input;
}

double median_of(const std::vector<double>& sorted)
{
    const auto lower{sorted[(sorted.size() - 1) / 2]};

    const auto upper{sorted[sorted.size() / 2]};

    return (lower + upper) / 2.0;
}

void print_summary(
        const std::string_view name, const std::size_t bytes, const std::size_t tokens, const int passes,
        const std::vector<double>& sorted_seconds)
{
    const auto mib{static_cast<double>(bytes) / bytes_per_mebibyte};

    const auto median{median_of(sorted_seconds)};

    const auto best_rate{mib / sorted_seconds.front()};

    const auto median_rate{mib / median};

    const auto worst_rate{mib / sorted_seconds.back()};

    std::printf(
            "%-16s %.1f MiB, %zu tokens, %d passes: best %.1f, median %.1f, worst %.1f MiB/s\n",
            std::string{name}.c_str(), mib, tokens, passes, best_rate, median_rate, worst_rate);
}

bool measure_interleaved(
        const std::span<const Scenario> scenarios, const int passes, const std::size_t input_mebibytes,
        const std::optional<std::string_view> observations_path)
{
    std::vector<std::size_t> expected{};

    expected.reserve(scenarios.size());

    // The warmup pass also fixes the token count every timed pass must reproduce, so a scenario that stops scanning the
    // whole corpus fails loudly. Content is checked once, against the reference stream, before timing.
    for (const auto& scenario : scenarios)
    {
        expected.push_back(scenario.pass());

        if (expected.back() == 0)
        {
            return false;
        }
    }

    std::vector<std::vector<double>> seconds(scenarios.size());

    std::vector<std::size_t> order(scenarios.size());

    std::ranges::iota(order, std::size_t{0});

    // Seeded rather than random: the order must vary between rounds to spread drift, and repeat exactly between runs so
    // a measurement can be reproduced.
    std::mt19937 sequence{0x5EEDU};

    std::vector<Observation> observations{};

    observations.reserve(scenarios.size() * static_cast<std::size_t>(passes));

    for (int round{0}; round < passes; ++round)
    {
        std::ranges::shuffle(order, sequence);

        for (const auto index : order)
        {
            const auto start{std::chrono::steady_clock::now()};

            const auto result{scenarios[index].pass()};

            const std::chrono::duration<double> elapsed{std::chrono::steady_clock::now() - start};

            if (result != expected[index])
            {
                std::printf("%s: the result changed between passes\n", std::string{scenarios[index].name}.c_str());

                return false;
            }

            seconds[index].push_back(elapsed.count());

            observations.push_back(Observation{.scenario = index, .round = round, .seconds = elapsed.count()});
        }
    }

    for (std::size_t index{0}; index < scenarios.size(); ++index)
    {
        const auto& [name, bytes, pass]{scenarios[index]};

        auto samples{seconds[index]};

        std::ranges::sort(samples);

        print_summary(name, bytes, expected[index], passes, samples);
    }

    if (!observations_path)
    {
        return true;
    }

    const std::string path{*observations_path};

    // The CSV is appended to, and rounds restart at zero in every call, so without this the rows of separate runs are
    // separable only by position. One identifier per process keeps them apart.
    static const auto run{std::chrono::system_clock::now().time_since_epoch() / std::chrono::seconds{1}};

    const bool fresh{!std::ifstream{path}.good()};

    std::ofstream csv{path, std::ios::app};

    if (!csv)
    {
        std::printf("unable to write observations to %s\n", path.c_str());

        return false;
    }

    // Round-trip precision: the default six significant digits round each pass before it reaches the file, so a
    // statistic recomputed from the CSV can differ from the one the summary computed over the timings in memory.
    csv.precision(std::numeric_limits<double>::max_digits10);

    if (fresh)
    {
        csv << "run,commit,dirty,scenario,input_mib,round,seconds,mib_per_s\n";
    }

    // The commit rides on every row: a row separated from its header must still say which tree produced it.
    for (const auto& [scenario, round, seconds_taken] : observations)
    {
        const auto& [name, bytes, pass]{scenarios[scenario]};

        const auto mib{static_cast<double>(bytes) / bytes_per_mebibyte};

        csv << run << ',' << commit << ',' << (dirty ? "yes" : "no") << ',' << name << ',' << input_mebibytes << ','
            << round << ',' << seconds_taken << ',' << mib / seconds_taken << '\n';
    }

    return csv.flush().good();
}

void print_provenance(
        const std::string_view benchmark, const int passes, const std::optional<std::string_view> observations_path)
{
    const auto now{std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())};

    std::tm utc{};

    gmtime_r(&now, &utc);

    std::array<char, 32> stamp{};

    std::strftime(stamp.data(), stamp.size(), "%Y-%m-%d %H:%M:%S UTC", &utc);

    std::printf("%s\n", std::string{benchmark}.c_str());

    std::printf("  commit      %s%s\n", std::string{commit}.c_str(), dirty ? " (uncommitted changes present)" : "");

    std::printf("  collected   %s\n", stamp.data());

    std::printf("  passes      %d\n", passes);

    std::printf("  hardware    %u threads visible\n", std::thread::hardware_concurrency());

#ifdef __VERSION__
    std::printf("  compiler    %s\n", __VERSION__);
#endif

    // The kernel and architecture without the node name, the promise collect.sh makes about its archives.
    utsname system{};

    if (uname(&system) == 0)
    {
        std::printf("  system      %s %s %s\n", system.sysname, system.release, system.machine);
    }

    // The observations file's name with its directory stripped, so the record names no machine, or not recorded when
    // no path is given.
    const auto file_name{[&observations_path]() -> std::string {
        if (!observations_path)
        {
            return "not recorded";
        }

        const auto slash{observations_path->rfind('/')};

        if (slash == std::string_view::npos)
        {
            return std::string{*observations_path};
        }

        return std::string{observations_path->substr(slash + 1)};
    }()};

    std::printf("  observations %s\n\n", file_name.c_str());
}

} // namespace munch::tools::benchmark
