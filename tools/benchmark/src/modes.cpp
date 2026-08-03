// Scenario matrix for the modal driver: action frequency, run length between actions, go_to against push/pop, and
// mode-stack depth. Mode_lexer picks its action lookup per mode, so one corpus cannot say which path an edit moved.
// Interleaved, because run consecutively the first scenario absorbs the machine's drift.
//
// Usage: munch_benchmark_modes [input size in MiB] [passes]

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "munch/core/mode_builder.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/benchmark/harness.hpp"

namespace
{
using munch::tools::benchmark::Scenario;

using namespace munch::regex;

enum class Mode_token : std::size_t
{
    whitespace = 1,
    identifier,
    number,
    punctuation,
    quote,
    body,
    open_comment,
    close_comment
};

constexpr std::size_t kCode{0};

constexpr std::size_t kString{1};

constexpr std::size_t kComment{2};

/**
 * @brief A C-like modal grammar.
 * @param stack Whether entering and leaving a string pushes and pops, or merely goes to.
 * @param actions Whether any token changes the mode at all; false gives the driver its no-action path.
 */
munch::core::Mode_lexer build(const bool stack, const bool actions)
{
    munch::core::Mode_builder builder;

    builder.add_token(kCode, plus(any_of(Set{' ', '\t', '\n'})), Mode_token::whitespace, 2);
    builder.add_token(
            kCode, concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Mode_token::identifier,
            2);
    builder.add_token(kCode, plus(any_of(Set::digits())), Mode_token::number, 2);
    builder.add_token(kCode, any_of(Set{'(', ')', '{', '}', ';', ',', '='}), Mode_token::punctuation, 2);

    if (!actions)
    {
        builder.add_token(kCode, text("\""), Mode_token::quote, 1);

        return builder.build();
    }

    const munch::core::Mode_action into{
            .kind = stack ? munch::core::Mode_action_kind::push : munch::core::Mode_action_kind::go_to,
            .target = kString};

    const munch::core::Mode_action back{
            .kind = stack ? munch::core::Mode_action_kind::pop : munch::core::Mode_action_kind::go_to,
            .target = kCode};

    builder.add_token(kCode, text("\""), Mode_token::quote, 1, into);
    builder.add_token(kString, text("\""), Mode_token::quote, 1, back);
    builder.add_token(kString, plus(any_of(Set::all() - '"')), Mode_token::body, 2);

    builder.add_token(
            kCode, text("/*"), Mode_token::open_comment, 1,
            {.kind = munch::core::Mode_action_kind::push, .target = kComment});
    builder.add_token(
            kComment, text("/*"), Mode_token::open_comment, 1,
            {.kind = munch::core::Mode_action_kind::push, .target = kComment});
    builder.add_token(kComment, text("*/"), Mode_token::close_comment, 1, {.kind = munch::core::Mode_action_kind::pop});
    builder.add_token(kComment, any_of(Set::all()), Mode_token::body, 2);

    return builder.build();
}

/**
 * @brief Generates code whose shape the caller controls along the axes that matter.
 * @param size The minimum size of the input in bytes.
 * @param action_percent How many statements in a hundred carry a string literal, and so an action token.
 * @param body How long each string literal's body is, which sets the run length between actions.
 * @param depth How deeply the occasional comment nests; zero emits none.
 */
std::string generate(const std::size_t size, const int action_percent, const std::size_t body, const std::size_t depth)
{
    std::string input;

    input.reserve(size + 256);

    unsigned seed{12345};

    const auto random{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    const std::string filler(body, 'x');

    while (input.size() < size)
    {
        if (depth > 0 && random() % 100 < 10)
        {
            for (std::size_t level{0}; level < depth; ++level)
            {
                input += "/*";
            }

            input += " c ";

            for (std::size_t level{0}; level < depth; ++level)
            {
                input += "*/";
            }

            input += "\n";
        }
        else if (static_cast<int>(random() % 100) < action_percent)
        {
            input += "  m = \"";
            input += filler;
            input += "\";\n";
        }
        else
        {
            input += "  value";
            input += std::to_string(random() % 100);
            input += " = ";
            input += std::to_string(random() % 100000);
            input += ";\n";
        }
    }

    return input;
}
} // namespace

int main(const int argc, const char** argv)
{
    const std::size_t mebibytes{argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 4};

    const int passes{argc > 2 ? std::atoi(argv[2]) : 15};

    if (mebibytes == 0 || passes <= 0)
    {
        std::printf("usage: munch_benchmark_modes [input size in MiB > 0] [passes > 0]\n");

        return EXIT_FAILURE;
    }

    const auto bytes{mebibytes << 20U};

    const auto acting{build(true, true)};

    const auto inert{build(true, false)};

    const auto flat_modes{build(false, true)};

    // One row per axis point. The corpora are held for the whole run because the scenarios interleave.
    struct Row
    {
        const char* name;

        const munch::core::Mode_lexer* lexer;

        std::string input;
    };

    std::vector<Row> rows;

    rows.push_back({.name = "table-never-fires", .lexer = &acting, .input = generate(bytes, 0, 16, 0)});
    rows.push_back({.name = "no-actions-declared", .lexer = &inert, .input = generate(bytes, 0, 16, 0)});

    for (const int percent : {1, 10, 40, 90})
    {
        rows.push_back(
                {.name = percent == 1  ? "actions-1pc" :
                         percent == 10 ? "actions-10pc" :
                         percent == 40 ? "actions-40pc" :
                                         "actions-90pc",
                 .lexer = &acting,
                 .input = generate(bytes, percent, 16, 0)});
    }

    for (const std::size_t body : {8U, 32U, 128U, 512U})
    {
        rows.push_back(
                {.name = body == 8   ? "body-8" :
                         body == 32  ? "body-32" :
                         body == 128 ? "body-128" :
                                       "body-512",
                 .lexer = &acting,
                 .input = generate(bytes, 40, body, 0)});
    }

    rows.push_back({.name = "push-pop", .lexer = &acting, .input = generate(bytes, 40, 32, 0)});
    rows.push_back({.name = "go-to", .lexer = &flat_modes, .input = generate(bytes, 40, 32, 0)});

    for (const std::size_t depth : {1U, 4U, 16U})
    {
        rows.push_back(
                {.name = depth == 1 ? "depth-1" :
                         depth == 4 ? "depth-4" :
                                      "depth-16",
                 .lexer = &acting,
                 .input = generate(bytes, 10, 16, depth)});
    }

    std::vector<Scenario> scenarios;

    scenarios.reserve(rows.size());

    for (const auto& row : rows)
    {
        scenarios.push_back({.name = row.name, .bytes = row.input.size(), .pass = [&row] {
                                 std::size_t tokens{0};

                                 const auto consumed{row.lexer->tokenize_all<Mode_token>(
                                         row.input, [&tokens](Mode_token, std::size_t, std::size_t) { ++tokens; })};

                                 return consumed == row.input.size() ? tokens : 0;
                             }});
    }

    std::printf("modal driver, %zu MiB per scenario, %d interleaved rounds\n", mebibytes, passes);

    return munch::tools::benchmark::measure_interleaved(scenarios, passes, mebibytes, nullptr) ? EXIT_SUCCESS :
                                                                                                 EXIT_FAILURE;
}
