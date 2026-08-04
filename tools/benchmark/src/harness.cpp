#include "munch/tools/benchmark/harness.hpp"

#include <sys/utsname.h>

#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <thread>

#include "munch/core/builder.hpp"
#include "munch/regex/patterns.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/utf8.hpp"
#include "munch/tools/benchmark/provenance.hpp"

namespace munch::tools::benchmark
{
core::Lexer build_lexer(const bool greek_identifiers)
{
    using namespace munch::regex;

    const auto letter{[greek_identifiers](Regex ascii) {
        return greek_identifiers ? choice(std::move(ascii), utf8::range(U'Α', U'ω')) : std::move(ascii);
    }};

    core::Builder builder;

    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 2);

    builder.add_token(
            concat(letter(any_of(Set::alpha() + '_')), kleene(letter(any_of(Set::alphanum() + '_')))),
            Token::identifier, 2);

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
}

std::string generate_input(const std::size_t size, const std::span<const char* const> identifiers)
{
    std::string input;

    input.reserve(size + 128);

    // A fixed-seed linear congruential generator keeps the input identical across runs and builds.
    unsigned seed{12345};

    const auto random{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    while (input.size() < size)
    {
        input += "while (";
        input += identifiers[random() % identifiers.size()];
        input += " <= ";
        input += std::to_string(random() % 100000);
        input += ") { ";
        input += identifiers[random() % identifiers.size()];
        input += " = ";
        input += identifiers[random() % identifiers.size()];
        input += " + ";
        input += std::to_string(random() % 997);
        input += "; if (x1 != 42) { return counter; } }\n";
    }

    return input;
}

std::string generate_source_input(const std::size_t size)
{
    constexpr const char* identifiers[]{
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

    std::string input;

    input.reserve(size + 256);

    // The same fixed-seed generator as generate_input(), so ports stay byte-identical.
    unsigned seed{12345};

    const auto random{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    const auto identifier{[&] { return identifiers[random() % std::size(identifiers)]; }};

    while (input.size() < size)
    {
        input += "while (";
        input += identifier();
        input += " <= ";
        input += std::to_string(random() % 10000000);
        input += ") {\n    ";
        input += identifier();
        input += " = ";
        input += identifier();
        input += " * ";
        input += identifier();
        input += " + ";
        input += std::to_string(random() % 100000);
        input += ";\n    if (";
        input += identifier();
        input += " != ";
        input += std::to_string(random() % 997);
        input += ") { return ";
        input += identifier();
        input += "; }\n}\n";
    }

    return input;
}

core::Lexer build_json_lexer(const bool discard_whitespace)
{
    using namespace munch::regex;

    core::Builder builder;

    // ws = *( %x20 / %x09 / %x0A / %x0D ).
    builder.add_token(plus(any_of(Set{' ', '\t', '\n', '\r'})), Json_token::whitespace, 2);

    // unescaped = %x20-21 / %x23-5B / %x5D-10FFFF, taken over bytes, so the tail covers every continuation byte.
    const auto unescaped{Set::range(0x20, 0x21) + Set::range(0x23, 0x5B) + Set::range(0x5D, 0xFF)};

    const auto hex{Set::digits() + Set::range('a', 'f') + Set::range('A', 'F')};

    const auto escape{concat(
            text("\\"),
            choice(any_of(Set{'"', '\\', '/', 'b', 'f', 'n', 'r', 't'}),
                   concat(text("u"), concat(any_of(hex), concat(any_of(hex), concat(any_of(hex), any_of(hex)))))))};

    builder.add_token(
            concat(text("\""), concat(kleene(choice(any_of(unescaped), escape)), text("\""))), Json_token::string, 2);

    const auto digits{plus(any_of(Set::digits()))};

    const auto integer{choice(text("0"), concat(any_of(Set::range('1', '9')), kleene(any_of(Set::digits()))))};

    const auto exponent{concat(any_of(Set{'e', 'E'}), concat(optional(any_of(Set{'+', '-'})), digits))};

    builder.add_token(
            concat(optional(text("-")),
                   concat(integer, concat(optional(concat(text("."), digits)), optional(exponent)))),
            Json_token::number, 2);

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
    constexpr const char* keys[]{"configuration_manager", "total_element_count", "process_next_request",
                                 "buffer_capacity",       "validation_result",   "iterator_position"};

    constexpr std::size_t fields{6};

    const char* const line_end{pretty ? "\n" : ""};

    const char* const indent{pretty ? "    " : ""};

    const char* const gap{pretty ? " " : ""};

    std::string input;

    input.reserve(size + 256);

    input += "[";
    input += line_end;

    // The same fixed-seed generator as generate_input(), so both shapes carry the same values in the same order.
    unsigned seed{12345};

    const auto random{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    while (input.size() < size)
    {
        input += indent;
        input += "{";
        input += line_end;

        for (std::size_t field{0}; field < fields; ++field)
        {
            input += indent;
            input += indent;
            input += "\"";
            input += keys[random() % std::size(keys)];
            input += "\":";
            input += gap;

            switch (random() % 4)
            {
            case 0:
                input += std::to_string(random() % 1000000);
                break;
            case 1:
                input += "\"";
                input += keys[random() % std::size(keys)];
                input += "\"";
                break;
            case 2:
                input += random() % 2 != 0 ? "true" : "false";
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

namespace
{
/**
 * @brief One timed pass of one scenario.
 */
struct Observation
{
    std::size_t scenario;
    int round;
    double seconds;
};
} // namespace

bool measure_interleaved(
        const std::span<const Scenario> scenarios, const int passes, const std::size_t input_mebibytes,
        const char* const observations_path)
{
    std::vector<std::size_t> expected;

    expected.reserve(scenarios.size());

    // The warmup pass also fixes the token count every timed pass must reproduce, so a scenario that stops
    // scanning the whole corpus fails loudly. Content is checked once, against the reference stream, before timing.
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

    std::iota(order.begin(), order.end(), std::size_t{0});

    // Seeded rather than random: the order must vary between rounds to spread drift, and repeat exactly between
    // runs so a measurement can be reproduced.
    std::mt19937 sequence{0x5eedU};

    std::vector<Observation> observations;

    observations.reserve(scenarios.size() * static_cast<std::size_t>(passes));

    for (int round{0}; round < passes; ++round)
    {
        std::shuffle(order.begin(), order.end(), sequence);

        for (const auto index : order)
        {
            const auto start{std::chrono::steady_clock::now()};

            const auto result{scenarios[index].pass()};

            const std::chrono::duration<double> elapsed{std::chrono::steady_clock::now() - start};

            if (result != expected[index])
            {
                std::printf("%s: the result changed between passes\n", scenarios[index].name);

                return false;
            }

            seconds[index].push_back(elapsed.count());

            observations.push_back(Observation{.scenario = index, .round = round, .seconds = elapsed.count()});
        }
    }

    for (std::size_t index{0}; index < scenarios.size(); ++index)
    {
        auto samples{seconds[index]};

        std::sort(samples.begin(), samples.end());

        const auto mib{static_cast<double>(scenarios[index].bytes) / (1024.0 * 1024.0)};

        const auto median{(samples[(samples.size() - 1) / 2] + samples[samples.size() / 2]) / 2.0};

        std::printf(
                "%-16s %.1f MiB, %zu tokens, %d passes: best %.1f, median %.1f, worst %.1f MiB/s\n",
                scenarios[index].name, mib, expected[index], passes, mib / samples.front(), mib / median,
                mib / samples.back());
    }

    if (observations_path == nullptr)
    {
        return true;
    }

    // The CSV is appended to, and rounds restart at zero in every call, so without this the rows of separate runs
    // are separable only by position. One identifier per process keeps them apart.
    static const auto run{std::chrono::system_clock::now().time_since_epoch() / std::chrono::seconds{1}};

    const bool fresh{!std::ifstream{observations_path}.good()};

    std::ofstream csv{observations_path, std::ios::app};

    if (!csv)
    {
        std::printf("unable to write observations to %s\n", observations_path);

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
    for (const auto& observation : observations)
    {
        const auto mib{static_cast<double>(scenarios[observation.scenario].bytes) / (1024.0 * 1024.0)};

        csv << run << ',' << kCommit << ',' << (kDirty ? "yes" : "no") << ',' << scenarios[observation.scenario].name
            << ',' << input_mebibytes << ',' << observation.round << ',' << observation.seconds << ','
            << mib / observation.seconds << '\n';
    }

    return csv.flush().good();
}

void print_provenance(const char* const benchmark, const int passes, const char* const observations_path)
{
    const auto now{std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())};

    std::tm utc{};

    gmtime_r(&now, &utc);

    char stamp[32]{};

    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC", &utc);

    std::printf("%s\n", benchmark);
    std::printf("  commit      %s%s\n", kCommit, kDirty ? " (uncommitted changes present)" : "");
    std::printf("  collected   %s\n", stamp);
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

    // The file name, never the path. This line is archived beside the CSV it names, and a directory from whoever
    // ran the benchmark identifies their machine, which tools/benchmark/collect.sh promises its output does not.
    const auto* const name{observations_path == nullptr ? nullptr : std::strrchr(observations_path, '/')};

    std::printf(
            "  observations %s\n\n",
            observations_path == nullptr ? "not recorded" : (name != nullptr ? name + 1 : observations_path));
}

} // namespace munch::tools::benchmark
