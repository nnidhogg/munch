#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/core/mode.hpp"
#include "munch/core/mode_builder.hpp"
#include "munch/core/mode_lexer.hpp"
#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/read_antlr.hpp"

using namespace munch::tools::audit;

namespace
{
/**
 * @brief A scanned token stream: each token by the name the report gives its rule, its token or its pattern when it
 *        returns none, with the text it matched.
 */
using Stream_t = std::vector<std::pair<std::string, std::string>>;

/**
 * @brief A scan of the Lean 4 grammar: every token it emits, the ones a parser sees, and where it stopped.
 */
struct Scanned
{
    /**
     * @brief Every token in input order, the discarded ones included.
     */
    Stream_t stream{};

    /**
     * @brief The tokens a parser sees, the ones whose rule returns a token.
     */
    Stream_t visible{};

    /**
     * @brief The number of input bytes the scan consumed.
     */
    std::size_t consumed{};

    /**
     * @brief The mode stack the scan ended with.
     */
    munch::core::Mode_stack stack{};
};

/**
 * @brief Returns the Lean 4 grammar beside the tests, read as its scanner.
 * @return The scanner.
 */
Lexer_spec lean4()
{
    const auto path{std::format("{}/tools/audit/grammars/lean4.g4", SOURCE_DIR)};

    std::ifstream stream{path};

    const std::string source{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};

    const auto scanners{read_antlr(source)};

    return scanners.front();
}

/**
 * @brief Returns the modes of a scanner by index, INITIAL first and the others in the order the grammar declares them.
 * @param spec The scanner.
 * @return The mode names.
 */
std::vector<std::string> modes_of(const Lexer_spec& spec)
{
    std::vector<std::string> modes{std::string{initial_condition}};

    for (const auto& [name, exclusive] : spec.conditions)
    {
        modes.push_back(name);
    }

    return modes;
}

/**
 * @brief Returns what a rule's ANTLR commands do to the mode stack, read from the action's text, where the reader keeps
 *        `pushMode`, `popMode` and `mode`.
 * @param action The rule's action, `-> skip, pushMode(M)` or the like.
 * @param modes The modes by index, INITIAL first.
 * @return The action; stay where no command changes the mode.
 */
munch::core::Mode_action mode_action(const std::string_view action, const std::vector<std::string>& modes)
{
    const auto target_of{[&modes, action](const std::size_t open) {
        const auto close{action.find(')', open)};

        const auto name{action.substr(open, close - open)};

        const auto mode{name == "DEFAULT_MODE" ? initial_condition : name};

        const auto found{std::ranges::find(modes, mode)};

        const auto index{std::ranges::distance(modes.begin(), found)};

        return static_cast<std::size_t>(index);
    }};

    if (action.contains("popMode"))
    {
        return {.kind = munch::core::Mode_action_kind::pop};
    }

    constexpr std::string_view push{"pushMode("};

    if (const auto at{action.find(push)}; at != std::string_view::npos)
    {
        return {.kind = munch::core::Mode_action_kind::push, .target = target_of(at + push.size())};
    }

    constexpr std::string_view go_to{"mode("};

    if (const auto at{action.find(go_to)}; at != std::string_view::npos)
    {
        return {.kind = munch::core::Mode_action_kind::go_to, .target = target_of(at + go_to.size())};
    }

    return {};
}

/**
 * @brief Compiles a scanner's modes into one modal lexer, every mode's token set as the audit reads it and each rule's
 *        mode commands as its action, so that the scan pushes and pops as ANTLR's does.
 * @param spec The scanner.
 * @return The lexer, mode zero INITIAL.
 */
munch::core::Mode_lexer modal(const Lexer_spec& spec)
{
    const auto modes{modes_of(spec)};

    munch::core::Mode_builder builder{};

    for (std::size_t mode{0}; mode < modes.size(); ++mode)
    {
        const auto set{token_set(spec, modes[mode])};

        for (const auto& [regex, id, priority, discarded] : set.rules)
        {
            const auto action{mode_action(spec.rules[id].action, modes)};

            builder.add_token(mode, regex, id, priority, action);
        }
    }

    return builder.build();
}

/**
 * @brief Scans a text with the Lean 4 grammar's modes, pushing and popping as its commands say.
 * @param input The text.
 * @return The tokens, the bytes consumed and the stack the scan ended with.
 */
Scanned scan(const std::string& input)
{
    const auto spec{lean4()};

    const auto lexer{modal(spec)};

    Scanned scanned{};

    std::size_t offset{0};

    const auto sink{[&](const std::size_t rule, const std::size_t length, std::size_t) {
        const auto& [pattern, expression, conditions, action, token, priority, line]{spec.rules[rule]};

        const auto text{input.substr(offset, length)};

        offset += length;

        scanned.stream.emplace_back(token.value_or(pattern), text);

        if (token)
        {
            scanned.visible.emplace_back(*token, text);
        }
    }};

    scanned.consumed = lexer.tokenize_all<std::size_t>(input, sink, scanned.stack);

    return scanned;
}

} // namespace

TEST(Lean4_test, Comments_nest_three_deep_each_opener_pushing_and_each_closer_popping)
{
    // A module doc comment holding two levels more, which a parser sees, and a comment three deep, which it does not.
    const std::string input{R"(/-! Module doc /- nested /- twice -/ -/ -/
/- outer /- middle /- inner -/ -/ -/
)"};

    const auto [stream, visible, consumed, stack]{scan(input)};

    EXPECT_EQ(consumed, input.size());
    EXPECT_EQ(stack, munch::core::Mode_stack{});

    const std::string blank{R"((' ' | '\r'? '\n')+)"};

    const std::string text{R"((~[/\-]+ | [/\-]))"};

    const Stream_t expected{
            {"MODULE_DOC_OPEN", "/-!"},
            {"DOC_COMMENT_TEXT", " Module doc "},
            {"DOC_COMMENT_NEST", "/-"},
            {"DOC_COMMENT_TEXT", " nested "},
            {"DOC_COMMENT_NEST", "/-"},
            {"DOC_COMMENT_TEXT", " twice "},
            {"DOC_COMMENT_CLOSE", "-/"},
            {"DOC_COMMENT_TEXT", " "},
            {"DOC_COMMENT_CLOSE", "-/"},
            {"DOC_COMMENT_TEXT", " "},
            {"DOC_COMMENT_CLOSE", "-/"},
            {blank, "\n"},
            {"'/-'", "/-"},
            {text, " outer "},
            {"'/-'", "/-"},
            {text, " middle "},
            {"'/-'", "/-"},
            {text, " inner "},
            {"'-/'", "-/"},
            {text, " "},
            {"'-/'", "-/"},
            {text, " "},
            {"'-/'", "-/"},
            {blank, "\n"}};

    EXPECT_EQ(stream, expected);
}

TEST(Lean4_test, Code_scans_into_the_tokens_lean_reads_with_an_interpolated_string_nesting_braces_back_into_code)
{
    // Subscripts, a letter-like identifier, numerals in three forms, a quoted part, and an interpolated string whose
    // braces hold a structure instance and a string of their own.
    const std::string input{R"(def x₁ : ℕ := 0x1F + 1_000 + 2.5e-3 -- trailing
theorem «my name».α' (h : a₀ ≤ b) : True := trivial
def msg := s!"sum {f {a := 1}} and {"in"}" ++ "tab\t" ++ toString 'c' ++ toString '\''
)"};

    const auto [stream, visible, consumed, stack]{scan(input)};

    EXPECT_EQ(consumed, input.size());
    EXPECT_EQ(stack, munch::core::Mode_stack{});

    const Stream_t expected{
            {"IDENTIFIER", "def"},
            {"IDENTIFIER", "x₁"},
            {"OTHER", ":"},
            {"IDENTIFIER", "ℕ"},
            {"SYMBOL", ":="},
            {"NUMBER", "0x1F"},
            {"OTHER", "+"},
            {"NUMBER", "1_000"},
            {"OTHER", "+"},
            {"NUMBER", "2.5e-3"},
            {"IDENTIFIER", "theorem"},
            {"IDENTIFIER", "«my name».α'"},
            {"OTHER", "("},
            {"IDENTIFIER", "h"},
            {"OTHER", ":"},
            {"IDENTIFIER", "a₀"},
            {"OTHER", "≤"},
            {"IDENTIFIER", "b"},
            {"OTHER", ")"},
            {"OTHER", ":"},
            {"IDENTIFIER", "True"},
            {"SYMBOL", ":="},
            {"IDENTIFIER", "trivial"},
            {"IDENTIFIER", "def"},
            {"IDENTIFIER", "msg"},
            {"SYMBOL", ":="},
            {"INTERPOLATED_OPEN", R"(s!")"},
            {"INTERPOLATED_TEXT", "sum "},
            {"INTERPOLATED_BRACE", "{"},
            {"IDENTIFIER", "f"},
            {"LEFT_BRACE", "{"},
            {"IDENTIFIER", "a"},
            {"SYMBOL", ":="},
            {"NUMBER", "1"},
            {"RIGHT_BRACE", "}"},
            {"RIGHT_BRACE", "}"},
            {"INTERPOLATED_TEXT", " and "},
            {"INTERPOLATED_BRACE", "{"},
            {"STRING_OPEN", R"(")"},
            {"STRING_TEXT", "in"},
            {"STRING_CLOSE", R"(")"},
            {"RIGHT_BRACE", "}"},
            {"INTERPOLATED_CLOSE", R"(")"},
            {"SYMBOL", "++"},
            {"STRING_OPEN", R"(")"},
            {"STRING_TEXT", R"(tab\t)"},
            {"STRING_CLOSE", R"(")"},
            {"SYMBOL", "++"},
            {"IDENTIFIER", "toString"},
            {"CHARACTER", "'c'"},
            {"SYMBOL", "++"},
            {"IDENTIFIER", "toString"},
            {"CHARACTER", R"('\'')"}};

    EXPECT_EQ(visible, expected);
}

TEST(Lean4_test, A_character_literal_holding_a_line_break_is_one_token_whether_the_lines_end_in_lf_or_in_crlf)
{
    // Lean turns every CRLF into LF before it scans, so the second text reads as the first does.
    const std::string lf{"#eval '\n'\n"};

    const std::string crlf{"#eval '\r\n'\r\n"};

    const auto [lf_stream, lf_visible, lf_consumed, lf_stack]{scan(lf)};

    const auto [crlf_stream, crlf_visible, crlf_consumed, crlf_stack]{scan(crlf)};

    EXPECT_EQ(lf_consumed, lf.size());
    EXPECT_EQ(crlf_consumed, crlf.size());

    const Stream_t lf_expected{{"OTHER", "#"}, {"IDENTIFIER", "eval"}, {"CHARACTER", "'\n'"}};

    const Stream_t crlf_expected{{"OTHER", "#"}, {"IDENTIFIER", "eval"}, {"CHARACTER", "'\r\n'"}};

    EXPECT_EQ(lf_visible, lf_expected);
    EXPECT_EQ(crlf_visible, crlf_expected);
}
