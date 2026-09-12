#include "munch/tools/audit/report.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "munch/regex/parse.hpp"
#include "munch/tools/audit/price.hpp"
#include "munch/tools/audit/read_flex.hpp"
#include "munch/tools/audit/token_set.hpp"

using namespace munch::tools::audit;

namespace
{
/**
 * @brief Reads one of the grammars beside the tests and audits its INITIAL condition.
 */
struct Audited
{
    Lexer_spec file;

    Report report;
};

Audited audited(const std::string_view grammar, const std::size_t window_limit = 3)
{
    std::ifstream stream{std::string{SOURCE_DIR} + "/tools/audit/grammars/" + std::string{grammar}};

    const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};

    auto file{read_flex(source).front()};

    auto report{audit(token_set(file, "INITIAL"), window_limit)};

    return {.file = std::move(file), .report = std::move(report)};
}

/**
 * @brief The name the report prints for a rule: its returned token, or its pattern when it returns nothing.
 */
std::function<std::string(std::size_t)> names(const Lexer_spec& file)
{
    return [&file](const std::size_t rule) { return file.rules[rule].token.value_or(file.rules[rule].pattern); };
}

std::vector<unsigned char> bytes(const std::string_view text)
{
    return {text.begin(), text.end()};
}

} // namespace

TEST(Report, The_flex_grammars_reproduce_the_applicability_rows)
{
    // Each row of the split-points paper's table, read through a .l file rather than typed into the library.
    EXPECT_EQ(audited("c-like-conventional.l").report.exact, bytes(""));
    EXPECT_EQ(audited("c-like-conventional.l").report.modulo, bytes("\n"));

    EXPECT_EQ(audited("c-like-split-friendly.l").report.exact, bytes("\n"));
    EXPECT_EQ(audited("c-like-split-friendly.l").report.modulo, bytes("\n"));

    EXPECT_EQ(audited("c-like-block-comments.l").report.exact, bytes(""));
    EXPECT_EQ(audited("c-like-block-comments.l").report.modulo, bytes(""));

    EXPECT_EQ(audited("json.l").report.exact, bytes(""));
    EXPECT_EQ(audited("json.l").report.modulo, bytes("\t\n\r"));

    EXPECT_EQ(audited("log-lines.l").report.exact, bytes("\n"));
    EXPECT_EQ(audited("log-lines.l").report.modulo, bytes("\n"));
}

TEST(Report, Windows_and_the_core_answer_where_bytes_do_not)
{
    // The conventional row gains two-byte windows, among them the paper's own: a newline followed by a byte that
    // must begin a token, certified at the byte.
    const auto conventional{audited("c-like-conventional.l")};

    EXPECT_TRUE(std::ranges::any_of(conventional.report.windows, [](const Certified_window& window) {
        return window.window == "\n!" && window.origin == 1;
    }));

    // Block comments force "*/" into every certified window, and no two-byte window certifies.
    const auto blocks{audited("c-like-block-comments.l")};

    EXPECT_EQ(blocks.report.mandatory_core, "*/");
    EXPECT_TRUE(std::ranges::none_of(
            blocks.report.windows, [](const Certified_window& window) { return window.window.size() == 2; }));

    // Every real row has an unbounded byte span: an identifier of any length carries no anchor.
    EXPECT_FALSE(conventional.report.byte_span.has_value());
    EXPECT_FALSE(blocks.report.byte_span.has_value());
}

TEST(Report, Blame_names_the_token_that_consumes_a_candidate_mid_token)
{
    const auto [file, report]{audited("c-like-conventional.l")};

    // The newline is a candidate, and only the whitespace run consumes it mid-token.
    std::vector<std::size_t> newline_tokens;

    for (const auto& [byte, token, after] : report.blame)
    {
        if (byte == '\n')
        {
            newline_tokens.push_back(token);

            EXPECT_TRUE(file.rules[token].pattern == R"([ \t\n]+)") << file.rules[token].pattern;

            // A shortest input reaching the run's state is one blank, whichever the search met first.
            EXPECT_TRUE(after == " " || after == "\t") << after;
        }
    }

    EXPECT_EQ(newline_tokens.size(), 1u);

    // '!' is consumed inside strings and line comments, after a quote and after the two slashes respectively.
    std::vector<std::string> bang_after;

    for (const auto& [byte, token, after] : report.blame)
    {
        if (byte == '!')
        {
            bang_after.push_back(after);
        }
    }

    std::ranges::sort(bang_after);

    EXPECT_EQ(bang_after, (std::vector<std::string>{R"(")", "//"}));
}

TEST(Report, Rendering_reads_as_the_sections)
{
    const auto [file, report]{audited("c-like-conventional.l")};

    const auto text{render(report, names(file))};

    EXPECT_NE(text.find("certified bytes             none"), std::string::npos);
    EXPECT_NE(text.find(R"(certified modulo discarded  '\n')"), std::string::npos);

    // The two rules returning nothing are what the modulo row deleted, and the page says so.
    EXPECT_EQ(report.discarded, (std::vector<std::size_t>{5, 6}));
    EXPECT_NE(text.find(R"(discarded tokens            2: "//"[^\n]*, [ \t\n]+)"), std::string::npos);

    // The verdict leads, and the JSON form carries the same figures under their names.
    EXPECT_TRUE(text.starts_with("verdict                     no byte certifies exactly; 1 certifies once the "));

    const auto document{json(report, names(file))};

    EXPECT_NE(
            document.find(R"("verdict": "no byte certifies exactly; 1 certifies once the discarded tokens)"),
            std::string::npos);
    EXPECT_NE(document.find(R"("exact": [])"), std::string::npos);
    EXPECT_NE(document.find(R"("modulo": [10])"), std::string::npos);
    EXPECT_NE(
            document.find(R"("discarded": [{"id": 5, "name": "\"//\"[^\\n]*"}, {"id": 6, "name": "[ \\t\\n]+"}])"),
            std::string::npos);
    EXPECT_NE(document.find(R"("byte_span": "unbounded")"), std::string::npos);
    EXPECT_NE(document.find(R"("mandatory_core": "")"), std::string::npos);
    EXPECT_NE(text.find("anchor-free span, bytes     unbounded"), std::string::npos);
    EXPECT_NE(text.find("why candidate bytes do not certify"), std::string::npos);
    EXPECT_NE(text.find("STRING"), std::string::npos);
}

TEST(Report, Pricing_follows_the_design_rows_of_the_study)
{
    // The conventional row: one edit, the whitespace run loses the newline and the newline becomes a discarded
    // token of its own, and the newline certifies exactly. That is the split-friendly row.
    const auto conventional{price(token_set(audited("c-like-conventional.l").file, "INITIAL"), '\n')};

    EXPECT_FALSE(conventional.exact_before);
    EXPECT_TRUE(conventional.modulo_before);
    ASSERT_EQ(conventional.steps.size(), 1u);
    EXPECT_TRUE(conventional.steps[0].separated);
    EXPECT_TRUE(conventional.steps[0].separated_discarded);
    EXPECT_TRUE(conventional.steps[0].exact);
    EXPECT_TRUE(conventional.immovable.empty());

    // With block comments: bounding the comment to a line buys the newline modulo discarded tokens, splitting the
    // whitespace run then buys it exactly, in that order, as the paper's two design rows had it.
    const auto blocks{audited("c-like-block-comments.l")};

    const auto priced{price(token_set(blocks.file, "INITIAL"), '\n')};

    EXPECT_FALSE(priced.exact_before);
    EXPECT_FALSE(priced.modulo_before);
    ASSERT_EQ(priced.steps.size(), 2u);
    EXPECT_EQ(blocks.file.rules[priced.steps[0].token].pattern, R"("/*"([^*]|\*+[^*/])*\*+"/")");
    EXPECT_FALSE(priced.steps[0].separated);
    EXPECT_FALSE(priced.steps[0].exact);
    EXPECT_TRUE(priced.steps[0].modulo);
    EXPECT_EQ(blocks.file.rules[priced.steps[1].token].pattern, R"([ \t\n]+)");
    EXPECT_TRUE(priced.steps[1].separated);
    EXPECT_TRUE(priced.steps[1].exact);
    EXPECT_TRUE(priced.immovable.empty());

    // A byte a fixed spelling holds cannot certify while that token stays: '=' against "==".
    const auto fixed{
            read_flex("%%\n\"==\"      return EQUAL;\n[=]        return ASSIGN;\n[a-z]+     return WORD;\n").front()};

    const auto equals{price(token_set(fixed, "INITIAL"), '=')};

    EXPECT_FALSE(equals.exact_before);
    EXPECT_EQ(equals.immovable, (std::vector<std::size_t>{0}));
    EXPECT_TRUE(equals.steps.empty());
}

TEST(Report, Shapes_name_the_edit_an_author_would_make_and_each_is_tried_on_its_own)
{
    // The whitespace run is a run; the block comment is delimited; the line comment ending in its newline is
    // terminated (and delimited too, terminated winning the name); the two-byte spelling is fixed.
    const auto blocks{audited("c-like-block-comments.l").file};

    const auto rule{[&blocks](const std::string_view pattern) {
        return std::ranges::find(blocks.rules, pattern, &Lexer_spec::Rule::pattern)->expression;
    }};

    EXPECT_EQ(shape_of(munch::regex::parse(rule(R"([ \t\n]+)")), '\n'), Shape::run);
    EXPECT_EQ(shape_of(munch::regex::parse(rule(R"("/*"([^*]|\*+[^*/])*\*+"/")")), '\n'), Shape::delimited);
    EXPECT_EQ(shape_of(munch::regex::parse(R"("//"[^\n]*\n)"), '\n'), Shape::terminated);
    EXPECT_EQ(shape_of(munch::regex::parse(R"("==")"), '='), Shape::fixed);
    EXPECT_EQ(shape_of(munch::regex::parse(R"([a-z]+|"\n")"), '\n'), Shape::other);

    // A string and a line comment ending in its newline both consume the newline mid-token; the whitespace run, which
    // excludes it, does not. Each shape's edit alone leaves the other consumer, both together buy the byte.
    constexpr std::string_view with_comment{R"(%%
[a-z]+                 return WORD;
\"[^"]*\"              return STRING;
"//"[^\n]*\n           ;
[ \t]+                 ;
\n                     return NEWLINE;
)"};

    const auto strings{read_flex(with_comment).front()};

    const auto priced{price(token_set(strings, "INITIAL"), '\n')};

    EXPECT_FALSE(priced.exact_before);
    ASSERT_EQ(priced.steps.size(), 1u);
    EXPECT_EQ(priced.steps[0].shape, Shape::delimited);

    // The narrowing cannot take the newline out of the comment's fixed terminator, so it is immovable there; the
    // choices can: the string's delimited edit and the comment's terminated and delimited edits, each alone.
    EXPECT_EQ(priced.immovable, (std::vector<std::size_t>{2}));
    ASSERT_EQ(priced.choices.size(), 3u);
    EXPECT_EQ(priced.choices[0].token, 1u);
    EXPECT_EQ(priced.choices[0].shape, Shape::delimited);
    EXPECT_FALSE(priced.choices[0].after.exact);
    EXPECT_EQ(priced.choices[1].token, 2u);
    EXPECT_EQ(priced.choices[1].shape, Shape::terminated);
    EXPECT_FALSE(priced.choices[1].after.exact);
    EXPECT_EQ(priced.choices[2].token, 2u);
    EXPECT_EQ(priced.choices[2].shape, Shape::delimited);

    // Taken together, the string cut to its quote and the comment stopping short of its newline, the byte certifies.
    ASSERT_TRUE(priced.together.has_value());
    EXPECT_TRUE(priced.together->exact);

    // With the comment already stopping short of the newline, the string's edit alone buys the byte.
    constexpr std::string_view without_comment{R"(%%
[a-z]+                 return WORD;
\"[^"]*\"              return STRING;
[ \t]+                 ;
\n                     return NEWLINE;
)"};

    const auto alone{read_flex(without_comment).front()};

    const auto bought{price(token_set(alone, "INITIAL"), '\n')};

    ASSERT_EQ(bought.choices.size(), 1u);
    EXPECT_EQ(bought.choices[0].shape, Shape::delimited);
    EXPECT_TRUE(bought.choices[0].after.exact);
    EXPECT_FALSE(bought.choices[0].after.gained.empty());

    const auto text{render(audit(token_set(alone, "INITIAL")), names(alone))};

    EXPECT_NE(text.find("delimited: scan the body in a start condition of its own"), std::string::npos);
}

TEST(Report, The_report_over_patterns_prices_the_newline_and_the_near_misses)
{
    const auto [file, report]{audited("json.l")};

    // JSON certifies tab, newline and carriage return modulo whitespace; each is priced, the newline first.
    ASSERT_EQ(report.prices.size(), 3u);
    EXPECT_EQ(report.prices[0].byte, '\n');
    EXPECT_EQ(report.prices[1].byte, '\t');
    EXPECT_EQ(report.prices[2].byte, '\r');

    for (const auto& pricing : report.prices)
    {
        ASSERT_EQ(pricing.steps.size(), 1u) << pricing.byte;
        EXPECT_TRUE(pricing.steps[0].exact);
    }

    const auto text{render(report, names(file))};

    EXPECT_NE(text.find(R"(what it would cost to certify '\n')"), std::string::npos);
    EXPECT_NE(text.find("becomes a token of its own, discarded"), std::string::npos);
    EXPECT_NE(text.find("certifies exactly"), std::string::npos);
}
