#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/core/mode_builder.hpp"
#include "munch/core/mode_lexer.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/benchmark/harness.hpp"

namespace
{
using namespace munch::tools::benchmark;

/**
 * @brief The modal grammar's token kinds.
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
     * @brief A punctuation byte.
     */
    punctuation,

    /**
     * @brief A string's opening or closing quote.
     */
    quote,

    /**
     * @brief A string's or a comment's body.
     */
    body,

    /**
     * @brief A comment opener.
     */
    open_comment,

    /**
     * @brief A comment closer.
     */
    close_comment
};

/**
 * @brief The code mode.
 */
constexpr std::size_t code_mode{0};

/**
 * @brief The string mode.
 */
constexpr std::size_t string_mode{1};

/**
 * @brief The comment mode.
 */
constexpr std::size_t comment_mode{2};

/**
 * @brief How many of mode zero's tokens carry an action, which is what the rows vary.
 */
enum class Acting : std::size_t
{
    /**
     * @brief No token carries an action.
     */
    none,

    /**
     * @brief The quote alone carries one.
     */
    one,

    /**
     * @brief The quote and the comment opener carry one.
     */
    all
};

/**
 * @brief How entering and leaving a string changes the mode.
 */
enum class Entry : std::size_t
{
    /**
     * @brief Entering pushes and leaving pops.
     */
    push_pop,

    /**
     * @brief Entering and leaving each go to the target mode.
     */
    go_to
};

/**
 * @brief Builds a C-like modal grammar.
 * @param entry How entering and leaving a string changes the mode.
 * @param acting How many tokens change the mode; none of them leaves the mode on the driver's action-free path.
 * @return The lexer.
 */
munch::core::Mode_lexer build(const Entry entry, const Acting acting)
{
    using namespace munch::regex;

    munch::core::Mode_builder builder{};

    builder.add_token(code_mode, plus(any_of(Set{' ', '\t', '\n'})), Mode_token::whitespace, 2);

    builder.add_token(code_mode, ascii_identifier(), Mode_token::identifier, 2);

    builder.add_token(code_mode, plus(any_of(Set::digits())), Mode_token::number, 2);

    builder.add_token(code_mode, any_of(Set{'(', ')', '{', '}', ';', ',', '='}), Mode_token::punctuation, 2);

    // Every variant registers the same code_mode patterns, so the rows differ in their actions and not in the DFA.
    if (acting == Acting::none)
    {
        builder.add_token(code_mode, text(R"(")"), Mode_token::quote, 1);

        builder.add_token(code_mode, text("/*"), Mode_token::open_comment, 1);

        return builder.build();
    }

    const auto stack{entry == Entry::push_pop};

    const munch::core::Mode_action into{
            .kind = stack ? munch::core::Mode_action_kind::push : munch::core::Mode_action_kind::go_to,
            .target = string_mode};

    const munch::core::Mode_action back{
            .kind = stack ? munch::core::Mode_action_kind::pop : munch::core::Mode_action_kind::go_to,
            .target = code_mode};

    builder.add_token(code_mode, text(R"(")"), Mode_token::quote, 1, into);

    builder.add_token(string_mode, text(R"(")"), Mode_token::quote, 1, back);

    builder.add_token(string_mode, plus(any_of(Set::all() - '"')), Mode_token::body, 2);

    // Leaving the comment opener inert gives mode zero one action token rather than two.
    if (acting == Acting::one)
    {
        builder.add_token(code_mode, text("/*"), Mode_token::open_comment, 1);
    }
    else
    {
        builder.add_token(
                code_mode, text("/*"), Mode_token::open_comment, 1,
                {.kind = munch::core::Mode_action_kind::push, .target = comment_mode});
    }

    // Registered either way, so the two grammars hold the same modes and differ only in mode zero's action count.
    builder.add_token(
            comment_mode, text("/*"), Mode_token::open_comment, 1,
            {.kind = munch::core::Mode_action_kind::push, .target = comment_mode});

    builder.add_token(
            comment_mode, text("*/"), Mode_token::close_comment, 1, {.kind = munch::core::Mode_action_kind::pop});

    builder.add_token(comment_mode, any_of(Set::all()), Mode_token::body, 2);

    return builder.build();
}

/**
 * @brief Generates code whose shape the caller controls along the axes that matter.
 * @param size The minimum size of the input in bytes.
 * @param action_percent How many statements in a hundred carry a string literal, and so an action token.
 * @param body How long each string literal's body is, which sets the run length between actions.
 * @param depth How deeply the occasional comment nests; zero emits none.
 * @return The generated input.
 */
std::string generate(const std::size_t size, const int action_percent, const std::size_t body, const std::size_t depth)
{
    std::string input{};

    input.reserve(size + 256);

    Corpus_random random{};

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
            input += R"(  m = ")";
            input += filler;
            input += R"(";)";
            input += "\n";
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

/**
 * @brief Measures the modal driver's scenario matrix in interleaved rounds.
 *
 * The matrix spans action frequency, run length between actions, go_to against push/pop, mode-stack depth, and the
 * batch entry point against the per-token one. The batch driver reads each action from the matched token's payload
 * while the per-token driver searches for it, and no single corpus says which an edit moved. Interleaved, because run
 * consecutively the first scenario absorbs the machine's drift. Usage: munch_benchmark_modes [input size in MiB]
 * [passes] [observations CSV]
 * @param argc The argument count.
 * @param argv The input size in MiB, the passes and the observations CSV, all optional.
 * @return EXIT_SUCCESS when every scenario reproduced its warmup result, EXIT_FAILURE otherwise.
 */
int main(const int argc, char** argv)
{
    // One row per axis point; the corpora are held for the whole run because the scenarios interleave.
    struct Row
    {
        std::string_view name{};
        const munch::core::Mode_lexer& lexer;
        std::string input{};
        // Whether the row drives tokenize_all() rather than the per-token entry point.
        bool batched{true};
    };

    const std::size_t mebibytes{argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 4};

    const int passes{argc > 2 ? std::atoi(argv[2]) : default_passes};

    const auto observations{argc > 3 ? std::optional<std::string_view>{argv[3]} : std::nullopt};

    if (mebibytes == 0 || passes <= 0)
    {
        std::printf("usage: munch_benchmark_modes [input size in MiB > 0] [passes > 0] [observations CSV]\n");

        return EXIT_FAILURE;
    }

    const auto bytes{bytes_of(mebibytes)};

    const auto acting{build(Entry::push_pop, Acting::all)};

    const auto inert{build(Entry::push_pop, Acting::none)};

    const auto single{build(Entry::push_pop, Acting::one)};

    const auto flat_modes{build(Entry::go_to, Acting::all)};

    std::vector<Row> rows{};

    rows.push_back({.name = "action-never-fires", .lexer = acting, .input = generate(bytes, 0, 16, 0)});

    rows.push_back({.name = "per-token", .lexer = acting, .input = generate(bytes, 40, 32, 0), .batched = false});

    rows.push_back({.name = "batched", .lexer = acting, .input = generate(bytes, 40, 32, 0)});

    rows.push_back({.name = "no-actions-declared", .lexer = inert, .input = generate(bytes, 0, 16, 0)});

    rows.push_back({.name = "one-action-never-fires", .lexer = single, .input = generate(bytes, 0, 16, 0)});

    constexpr std::array<std::pair<int, std::string_view>, 4> action_rows{{
            {1, "actions-1pc"},
            {10, "actions-10pc"},
            {40, "actions-40pc"},
            {90, "actions-90pc"},
    }};

    for (const auto& [percent, name] : action_rows)
    {
        rows.push_back({.name = name, .lexer = acting, .input = generate(bytes, percent, 16, 0)});
    }

    constexpr std::array<std::pair<std::size_t, std::string_view>, 4> body_rows{{
            {8, "body-8"},
            {32, "body-32"},
            {128, "body-128"},
            {512, "body-512"},
    }};

    for (const auto& [body, name] : body_rows)
    {
        rows.push_back({.name = name, .lexer = acting, .input = generate(bytes, 40, body, 0)});
    }

    rows.push_back({.name = "push-pop", .lexer = acting, .input = generate(bytes, 40, 32, 0)});

    rows.push_back({.name = "go-to", .lexer = flat_modes, .input = generate(bytes, 40, 32, 0)});

    constexpr std::array<std::pair<std::size_t, std::string_view>, 3> depth_rows{{
            {1, "depth-1"},
            {4, "depth-4"},
            {16, "depth-16"},
    }};

    for (const auto& [depth, name] : depth_rows)
    {
        rows.push_back({.name = name, .lexer = acting, .input = generate(bytes, 10, 16, depth)});
    }

    std::vector<Scenario> scenarios{};

    scenarios.reserve(rows.size());

    const auto pass_of{[](const Row& row) {
        return [&row] {
            std::size_t tokens{0};

            if (!row.batched)
            {
                munch::core::Mode_stack stack{};

                std::size_t offset{0};

                while (offset < row.input.size())
                {
                    const auto [token, length]{row.lexer.tokenize<Mode_token>(
                            row.input.cbegin() + static_cast<std::ptrdiff_t>(offset), row.input.cend(), stack)};

                    if (!token || length == 0)
                    {
                        break;
                    }

                    offset += length;

                    ++tokens;
                }

                return offset == row.input.size() ? tokens : 0;
            }

            const auto consumed{row.lexer.tokenize_all<Mode_token>(
                    row.input, [&tokens](Mode_token, std::size_t, std::size_t) { ++tokens; })};

            return consumed == row.input.size() ? tokens : 0;
        };
    }};

    for (const auto& row : rows)
    {
        scenarios.push_back({.name = row.name, .bytes = row.input.size(), .pass = pass_of(row)});
    }

    print_provenance("modal driver scenario matrix", passes, observations);

    std::printf("modal driver, %zu MiB per scenario, %d interleaved rounds\n", mebibytes, passes);

    const auto ok{measure_interleaved(scenarios, passes, mebibytes, observations)};

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
