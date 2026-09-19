#include "munch/tools/audit/report.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/dfa/simulator.hpp"
#include "munch/regex/parse.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/audit/price.hpp"
#include "munch/tools/audit/read_flex.hpp"
#include "munch/tools/audit/read_re2c.hpp"
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

TEST(Report, Blame_names_a_longer_token_whose_accept_lies_beyond_a_shorter_one)
{
    // The newline itself, a[\nx] and a[\nx]b: after the "a" the newline leads to the state accepting the second
    // rule, and the third rule's scan stands in that same state, so both consume the newline mid-token. Reading
    // only the nearest accepting state blamed the second rule alone, and the price then narrowed that one rule and
    // reported the byte still uncertified with nothing further to try.
    const Token_set set{
            .rules = {
                    {.regex = munch::regex::parse(R"(\n)"), .id = 0, .priority = 1, .discarded = false},
                    {.regex = munch::regex::parse(R"(a[\nx])"), .id = 1, .priority = 1, .discarded = false},
                    {.regex = munch::regex::parse(R"(a[\nx]b)"), .id = 2, .priority = 1, .discarded = false}}};

    std::vector<std::size_t> blamed;

    for (const auto& [byte, token, after] : blame(compile(set)))
    {
        if (byte == '\n')
        {
            blamed.push_back(token);

            EXPECT_EQ(after, "a") << token;
        }
    }

    std::ranges::sort(blamed);

    EXPECT_EQ(blamed, (std::vector<std::size_t>{1, 2}));

    // The price narrows every blamed token in rule order, so the byte certifies once the longer one is narrowed too.
    const auto pricing{price(set, '\n')};

    ASSERT_EQ(pricing.steps.size(), 2U);
    EXPECT_EQ(pricing.steps.front().token, 1U);
    EXPECT_FALSE(pricing.steps.front().exact);
    EXPECT_EQ(pricing.steps.back().token, 2U);
    EXPECT_TRUE(pricing.steps.back().exact);
}

TEST(Report, An_alternative_matching_nothing_is_no_part_of_the_pattern_the_price_reads)
{
    // A set assembled through the API may hold an alternative matching no word at all, any_of(""), which no file
    // syntax spells; the choice matches what its other alternative does, so `[ab]\n` and the choice of it with
    // nothing are one pattern to the scanner and must be one to the price: the same terminated shape, the same
    // steps and the same outcome, where reading the alternative as a word left the newline a possible opener and
    // lost the terminated edit.
    using munch::regex::any_of;
    using munch::regex::choice;
    using munch::regex::concat;
    using munch::regex::text;

    const Token_set plain{
            .rules = {
                    {.regex = munch::regex::parse(R"([ab]\n)"), .id = 0, .priority = 1, .discarded = false},
                    {.regex = munch::regex::parse("x"), .id = 1, .priority = 1, .discarded = false}}};

    const Token_set with_nothing{
            .rules = {
                    {.regex = choice(
                             concat(any_of(munch::regex::Set{'a', 'b'}), text('\n')), any_of(munch::regex::Set{})),
                     .id = 0,
                     .priority = 1,
                     .discarded = false},
                    {.regex = munch::regex::parse("x"), .id = 1, .priority = 1, .discarded = false}}};

    // A repetition of nothing that need not repeat it is the empty word, and no part of a sequence: `[ab]\n` followed
    // by `(any_of(""))*` is `[ab]\n` too.
    const Token_set with_trailing{
            .rules = {
                    {.regex =
                             concat(any_of(munch::regex::Set{'a', 'b'}), text('\n'),
                                    munch::regex::kleene(any_of(munch::regex::Set{}))),
                     .id = 0,
                     .priority = 1,
                     .discarded = false},
                    {.regex = munch::regex::parse("x"), .id = 1, .priority = 1, .discarded = false}}};

    // A sequence whose parts all match the empty word alone is the empty word and no sequence of nothing: pricing
    // `a(x{0}y{0}|b)c`, which matches "ac" and "abc", built an empty sequence and threw.
    const Token_set emptied{
            .rules = {{.regex = munch::regex::parse("a(x{0}y{0}|b)c"), .id = 0, .priority = 1, .discarded = false}}};

    EXPECT_NO_THROW(std::ignore = price(emptied, 'c'));

    const auto expected{price(plain, '\n')};

    for (const auto* set : {&with_nothing, &with_trailing})
    {
        const auto priced{price(*set, '\n')};

        EXPECT_EQ(priced.exact_before, expected.exact_before);
        EXPECT_EQ(priced.modulo_before, expected.modulo_before);
        EXPECT_EQ(priced.immovable, expected.immovable);
        EXPECT_EQ(priced.undecided, expected.undecided);
        EXPECT_EQ(priced.choices.size(), expected.choices.size());
        EXPECT_EQ(priced.together.has_value(), expected.together.has_value());
        ASSERT_EQ(priced.steps.size(), expected.steps.size());

        for (std::size_t step{0}; step < expected.steps.size(); ++step)
        {
            EXPECT_EQ(priced.steps[step].token, expected.steps[step].token) << step;
            EXPECT_EQ(priced.steps[step].shape, expected.steps[step].shape) << step;
            EXPECT_EQ(priced.steps[step].separated, expected.steps[step].separated) << step;
            EXPECT_EQ(priced.steps[step].exact, expected.steps[step].exact) << step;
            EXPECT_EQ(priced.steps[step].modulo, expected.steps[step].modulo) << step;
        }
    }
}

TEST(Report, A_re_entrant_initial_state_is_blamed_for_what_it_consumes_mid_token)
{
    // The star at the head of [\n]*b returns the scan to the initial state, so on "\n\nb" the second newline stands
    // mid-token in the very state a byte may also begin a token in. Skipping the initial state unconditionally left
    // the blame empty for a byte the certificate had already refused, and the price section printed nothing at all.
    const Token_set set{
            .rules = {{.regex = munch::regex::parse(R"([\n]*b)"), .id = 0, .priority = 1, .discarded = false}}};

    const auto lexer{compile(set)};

    EXPECT_TRUE(lexer.simulator().init_reentrant());
    EXPECT_FALSE(lexer.is_split_point('\n'));

    std::vector<std::string> newline_after;

    for (const auto& [byte, token, after] : blame(lexer))
    {
        if (byte == '\n')
        {
            EXPECT_EQ(token, 0U);

            newline_after.push_back(after);
        }
    }

    // The input reported is a shortest one that returns to the initial state, never the empty one it is entered by.
    EXPECT_EQ(newline_after, (std::vector<std::string>{"\n"}));

    // The price then names the consumer, and the newline taken out of the run and given a token of its own certifies.
    const auto priced{price(set, '\n')};

    ASSERT_EQ(priced.steps.size(), 1U);
    EXPECT_EQ(priced.steps.front().token, 0U);
    EXPECT_TRUE(priced.steps.front().separated);
    EXPECT_TRUE(priced.steps.front().exact);

    const auto text{render(audit(set), [](const std::size_t) { return std::string{"B"}; })};

    EXPECT_NE(text.find(R"(what it would cost to certify '\n')"), std::string::npos);
    EXPECT_NE(
            text.find(R"(B                        no longer admits '\n', and '\n' becomes a token of its own)"),
            std::string::npos);
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

TEST(Report, A_window_count_no_size_t_holds_is_reported_as_the_bound_it_passed)
{
    // One token matching any single byte: every byte moves every state alike, so the tables hold one byte class of
    // 256 and one certified window per width. A window of the widest width the command accepts, eight, therefore
    // stands for 256^8 byte strings, which is one more than a std::size_t counts, and the widths below it sum to
    // 0x0101010101010100. Multiplying the class sizes unchecked wrapped the wider count back onto that sum and
    // printed it as the number of windows an input can show.
    const Token_set any{
            .rules = {{.regex = any_of(munch::regex::Set::all()), .id = 0, .priority = 1, .discarded = false}}};

    const auto lexer{compile(any)};

    const auto name{[](const std::size_t) { return std::string{"any"}; }};

    const auto narrow{audit(lexer, 7)};

    ASSERT_TRUE(narrow.window_count.has_value());
    EXPECT_EQ(*narrow.window_count, 72340172838076416U);
    EXPECT_NE(json(narrow, name).find(R"("window_count": 72340172838076416)"), std::string::npos);

    const auto wide{audit(lexer, 8)};

    EXPECT_FALSE(wide.window_count.has_value());

    const auto text{render(wide, name)};

    EXPECT_NE(text.find("more than 18446744073709551615 once classes expand"), std::string::npos);
    EXPECT_EQ(text.find("72340172838076416"), std::string::npos);
    EXPECT_NE(json(wide, name).find(R"("window_count": "more than 18446744073709551615")"), std::string::npos);
}

TEST(Report, The_options_that_governed_the_reading_reach_both_forms_of_the_report)
{
    // A flex file's `%option` words, in the reader's own order, as the row the report prints under the scanner and
    // as the array its JSON account carries; a scanner that declares none prints no row at all.
    const auto [file, report]{audited("c-like-split-friendly.l")};

    EXPECT_EQ(file.options, (std::vector<std::string>{"noyywrap", "nodefault"}));
    EXPECT_EQ(options_row(file.options), "options                     noyywrap, nodefault\n");
    EXPECT_EQ(options_json(file.options), R"(["noyywrap", "nodefault"])");

    EXPECT_EQ(options_row({}), "");
    EXPECT_EQ(options_json({}), "[]");

    // Several options are one row, comma-separated; what a logos reading notes of the Unicode version its classes
    // came from is one of them, so the page says which language was analysed.
    const std::vector<std::string> several{"unicode-classes=16.0.0", "extras=Extras"};

    EXPECT_EQ(options_row(several), "options                     unicode-classes=16.0.0, extras=Extras\n");
    EXPECT_EQ(options_json(several), R"(["unicode-classes=16.0.0", "extras=Extras"])");
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

    // The comment is discarded, but its step separated nothing, so there is no token of the byte's own to be
    // discarded there; the run's step separated one, discarded as the run is.
    EXPECT_TRUE(blocks.file.rules[priced.steps[0].token].token == std::nullopt);
    EXPECT_FALSE(priced.steps[0].separated_discarded);
    EXPECT_TRUE(priced.steps[1].separated_discarded);

    // A byte a fixed spelling holds cannot certify while that token stays: '=' against "==".
    const auto fixed{
            read_flex("%%\n\"==\"      return EQUAL;\n[=]        return ASSIGN;\n[a-z]+     return WORD;\n").front()};

    const auto equals{price(token_set(fixed, "INITIAL"), '=')};

    EXPECT_FALSE(equals.exact_before);
    EXPECT_EQ(equals.immovable, (std::vector<std::size_t>{0}));
    EXPECT_TRUE(equals.steps.empty());
}

TEST(Report, Pricing_narrows_the_consumer_an_earlier_edit_exposes)
{
    // A's match of "a\n" is what keeps B from consuming the newline, so the blame names A alone; once A loses the
    // byte, B wins that match and must be narrowed too. Pricing from the list the blame gave before any edit
    // stopped after one step and reported the byte still uncertified with nothing immovable and nothing to try.
    const Token_set exposed{
            .rules = {
                    {.regex = munch::regex::parse(R"(\n)"), .id = 0, .priority = 0, .discarded = false},
                    {.regex = munch::regex::parse(R"(a[\nx])"), .id = 1, .priority = 1, .discarded = false},
                    {.regex = munch::regex::parse(R"(a[\ny])"), .id = 2, .priority = 2, .discarded = false}}};

    const auto priced{price(exposed, '\n')};

    ASSERT_EQ(priced.steps.size(), 2U);
    EXPECT_EQ(priced.steps.front().token, 1U);
    EXPECT_FALSE(priced.steps.front().exact);
    EXPECT_EQ(priced.steps.back().token, 2U);
    EXPECT_TRUE(priced.steps.back().exact);
    EXPECT_TRUE(priced.immovable.empty());

    // The same where priority alone hides the second consumer: FIRST and SECOND match the same runs, FIRST wins
    // them, and SECOND is the run that consumes the newline once FIRST no longer does.
    const Token_set shadowed{
            .rules = {
                    {.regex = munch::regex::parse(R"([\nx]+)"), .id = 0, .priority = 0, .discarded = false},
                    {.regex = munch::regex::parse(R"([\nx]+|y)"), .id = 1, .priority = 1, .discarded = false}}};

    const auto repriced{price(shadowed, '\n')};

    ASSERT_EQ(repriced.steps.size(), 2U);
    EXPECT_EQ(repriced.steps.front().token, 0U);
    EXPECT_FALSE(repriced.steps.front().exact);
    EXPECT_EQ(repriced.steps.back().token, 1U);
    EXPECT_TRUE(repriced.steps.back().separated);
    EXPECT_TRUE(repriced.steps.back().exact);

    // The token the byte was given of its own is the analysis's, none of the set's rules, so no round of the
    // repricing narrows it and nothing the report names is an id the caller cannot name.
    EXPECT_LT(repriced.steps.back().token, shadowed.rules.size());
    EXPECT_TRUE(repriced.immovable.empty());
    EXPECT_TRUE(repriced.undecided.empty());
}

TEST(Report, A_byte_no_token_begins_with_is_given_one_before_it_is_priced)
{
    // The newline here ends a line and begins nothing, so neither certificate reports it and the blame is silent
    // about it; the page printed the heading and nothing under it. The byte is given a token of its own first, and
    // the line token is then priced as any consumer is: immovable for the narrowing, and its terminated shape
    // leaves the newline to the token it now has; the opener matches two words, so that edit is the one offered.
    const Token_set lines{
            .rules = {{.regex = munch::regex::parse(R"([ab]\n)"), .id = 0, .priority = 0, .discarded = false}}};

    const auto priced{price(lines, '\n')};

    EXPECT_FALSE(priced.exact_before);
    ASSERT_TRUE(priced.given.has_value());
    EXPECT_FALSE(priced.given->exact);
    EXPECT_TRUE(priced.steps.empty());
    EXPECT_EQ(priced.immovable, (std::vector<std::size_t>{0}));
    ASSERT_EQ(priced.choices.size(), 1U);
    EXPECT_EQ(priced.choices[0].shape, Shape::terminated);
    EXPECT_TRUE(priced.choices[0].after.exact);
    ASSERT_TRUE(priced.together.has_value());
    EXPECT_TRUE(priced.together->exact);

    // A byte no token holds at all certifies exactly once given a token, with nothing left to narrow.
    const Token_set absent{
            .rules = {{.regex = munch::regex::parse(R"([a])"), .id = 0, .priority = 0, .discarded = false}}};

    const auto held{price(absent, '\n')};

    ASSERT_TRUE(held.given.has_value());
    EXPECT_TRUE(held.given->exact);
    EXPECT_TRUE(held.steps.empty());
    EXPECT_TRUE(held.immovable.empty());

    // A byte some token begins with is priced as before, nothing given.
    const Token_set begun{
            .rules = {
                    {.regex = munch::regex::parse(R"(\n)"), .id = 0, .priority = 0, .discarded = false},
                    {.regex = munch::regex::parse(R"(a[\nx])"), .id = 1, .priority = 1, .discarded = false}}};

    EXPECT_FALSE(price(begun, '\n').given.has_value());

    // The report prices the newline on its own, and both forms say what the case is rather than nothing.
    const auto report{audit(lines)};

    ASSERT_EQ(report.prices.size(), 1U);
    EXPECT_TRUE(report.prices[0].given.has_value());

    const auto name{[](const std::size_t) { return std::string{"LINE"}; }};

    const auto text{render(report, name)};

    EXPECT_NE(
            text.find("no token begins with '\\n'  neither certificate reports it; it is given a token of its own"),
            std::string::npos);
    EXPECT_NE(
            text.find("LINE                       spells '\\n' out and cannot be narrowed; its shape"),
            std::string::npos);
    EXPECT_NE(
            json(report, name).find(R"("given": {"exact": false, "modulo": false, "gained": []})"), std::string::npos);
    EXPECT_NE(render(audit(absent), name).find("certifies exactly"), std::string::npos);
}

TEST(Report, Steps_follow_the_order_of_the_rules_and_not_of_their_ids)
{
    // Both openers consume the newline before any edit, so both are answered before any other, in the order the set
    // lists them: the rule with id 9 stands first. Ordering by id took the rule with id 2 first, against what the
    // page promises of the steps.
    const Token_set set{
            .rules = {
                    {.regex = munch::regex::parse(R"(\n)"), .id = 5, .priority = 0, .discarded = false},
                    {.regex = munch::regex::parse(R"(a[\nx])"), .id = 9, .priority = 1, .discarded = false},
                    {.regex = munch::regex::parse(R"(b[\nx])"), .id = 2, .priority = 2, .discarded = false}}};

    const auto priced{price(set, '\n')};

    ASSERT_EQ(priced.steps.size(), 2U);
    EXPECT_EQ(priced.steps.front().token, 9U);
    EXPECT_FALSE(priced.steps.front().exact);
    EXPECT_EQ(priced.steps.back().token, 2U);
    EXPECT_TRUE(priced.steps.back().exact);
}

TEST(Report, The_token_a_byte_is_given_of_its_own_takes_an_id_no_rule_carries_whatever_the_ids_are)
{
    // The ids are the caller's and any std::size_t is one: with `a[xy]` at the largest and `x` one below it, x does
    // not certify and narrowing the opener certifies it, as it does under ids 0 and 1. Taking one past the highest
    // id for the byte's own token wrapped to 0, and seeding the answered set with the id of `a[xy]`'s successor
    // left the consumer unread: no step, nothing immovable, nothing undecided, while x stayed uncertified.
    constexpr auto largest{std::numeric_limits<std::size_t>::max()};

    for (const auto& [opener, terminator] : {std::pair{0UZ, 1UZ}, std::pair{largest, largest - 1}, std::pair{1UZ, 0UZ}})
    {
        const Token_set set{
                .rules = {
                        {.regex = munch::regex::parse(R"(a[xy])"), .id = opener, .priority = 0, .discarded = false},
                        {.regex = munch::regex::parse("x"), .id = terminator, .priority = 1, .discarded = false}}};

        EXPECT_FALSE(compile(set).is_split_point('x'));

        const auto priced{price(set, 'x')};

        ASSERT_EQ(priced.steps.size(), 1U) << opener;
        EXPECT_EQ(priced.steps.front().token, opener);
        EXPECT_TRUE(priced.steps.front().exact);
        EXPECT_TRUE(priced.immovable.empty());
        EXPECT_TRUE(priced.undecided.empty());
    }

    // The token the byte is given where none begins with it takes the smallest id no rule carries, 1 between 0 and
    // 2, which no step names, so the one step is the opener's and certifies.
    const Token_set unmatched{
            .rules = {
                    {.regex = munch::regex::parse(R"(a[xy])"), .id = 0, .priority = 0, .discarded = false},
                    {.regex = munch::regex::parse("b"), .id = 2, .priority = 1, .discarded = false}}};

    const auto priced{price(unmatched, 'x')};

    ASSERT_TRUE(priced.given.has_value());
    ASSERT_EQ(priced.steps.size(), 1U);
    EXPECT_EQ(priced.steps.front().token, 0U);
    EXPECT_TRUE(priced.steps.front().exact);
}

TEST(Report, A_byte_a_token_begins_with_obstructs_nothing_and_is_not_called_immovable)
{
    // The opener's newline is the rule's fixed occurrence of the byte, and it is the one the initial state
    // consumes, where a byte may begin a token. The narrowing cannot take a byte out of a spelling, but reporting
    // that as the byte being unbuyable while the rule stays was false: the class alone narrowed, it certifies. The
    // opener may be a class of the one byte, a repetition of the text, a group, or one alternative among others
    // that begin the same way; each was called immovable while a leading text alone was recognised.
    const auto priced{[](const std::string_view opener) {
        const Token_set set{
                .rules = {
                        {.regex = munch::regex::parse(R"(\n)"), .id = 0, .priority = 0, .discarded = false},
                        {.regex = munch::regex::parse(opener), .id = 1, .priority = 1, .discarded = false}}};

        return price(set, '\n');
    }};

    for (const auto opener : {R"(\n[\nx])", R"([\n][\nx])", R"((\n[\nx]|\ny))", R"(\n{1}[\nx])", R"((\n[xy])[\nx])"})
    {
        const auto pricing{priced(opener)};

        EXPECT_TRUE(pricing.steps.empty()) << opener;
        EXPECT_TRUE(pricing.immovable.empty()) << opener;
        EXPECT_EQ(pricing.undecided, (std::vector<std::size_t>{1})) << opener;
    }

    // The edit the analysis does not reach, the later class narrowed or an alternative dropped, and the rule keeping
    // its opener: the byte certifies with the rule standing.
    for (const auto narrowed : {R"(\n[x])", R"([\n][x])", R"((\n[x]|\ny))", R"(\n{1}[x])", R"((\n[xy])[x])"})
    {
        const Token_set set{
                .rules = {
                        {.regex = munch::regex::parse(R"(\n)"), .id = 0, .priority = 0, .discarded = false},
                        {.regex = munch::regex::parse(narrowed), .id = 1, .priority = 1, .discarded = false}}};

        EXPECT_TRUE(compile(set).is_split_point('\n')) << narrowed;
    }

    // A fixed occurrence past the first byte on every path is the obstruction the necessity theorem names, and it
    // stays immovable: spelled mid-token, spelled twice, or repeated to a second occurrence.
    for (const auto inside : {R"([x]\n[x])", R"([xy]\n[x])", R"(\n\n)", R"(\n{2})", R"((\n[x]|x\n)\n)"})
    {
        const auto pricing{priced(inside)};

        EXPECT_EQ(pricing.immovable, (std::vector<std::size_t>{1})) << inside;
        EXPECT_TRUE(pricing.undecided.empty()) << inside;
    }

    // The page says which of the two it is, and claims impossibility only where the theorem proves it and no shape
    // offers an edit: the opener of [xy]\n[x] matches two words, so none does.
    const Token_set set{
            .rules = {
                    {.regex = munch::regex::parse(R"(\n)"), .id = 0, .priority = 0, .discarded = false},
                    {.regex = munch::regex::parse(R"([\n][\nx])"), .id = 1, .priority = 1, .discarded = false}}};

    const Token_set inside{
            .rules = {
                    {.regex = munch::regex::parse(R"(\n)"), .id = 0, .priority = 0, .discarded = false},
                    {.regex = munch::regex::parse(R"([xy]\n[x])"), .id = 1, .priority = 1, .discarded = false}}};

    const auto text{render(audit(set), [](const std::size_t rule) { return rule == 0 ? "NEWLINE" : "OPENER"; })};

    EXPECT_NE(
            text.find(R"(OPENER                     spells '\n' out, on some path only as its first byte)"),
            std::string::npos);
    EXPECT_NE(text.find("no narrowing applies to a fixed spelling, so this edit decides nothing"), std::string::npos);
    EXPECT_EQ(text.find("the byte cannot certify while it stays"), std::string::npos);

    EXPECT_NE(
            render(audit(inside), [](const std::size_t rule) { return rule == 0 ? "NEWLINE" : "INSIDE"; })
                    .find("the byte cannot certify while it stays"),
            std::string::npos);
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

TEST(Report, A_terminated_shape_names_the_edit_that_was_evaluated_and_no_other)
{
    // The shape's edit deletes the token's last component, so the shape holds only where that component is the
    // terminator. The last component of [ab]"xb" admits the 'x' while the token ends in 'b': the page reported an
    // edit leaving a terminator to the token after it, while what was evaluated deleted the token's own 'b' too.
    // The class of [ab][xb] admits the 'x' the same way: the class holds the byte, and it is not the byte alone.
    // The openers here match two words, so no delimited edit stands in for the terminated one.
    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab]"xb")"), 'x'), Shape::fixed);
    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab][xb])"), 'x'), Shape::other);
    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab]"x")"), 'x'), Shape::terminated);
    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab][x])"), 'x'), Shape::terminated);
    EXPECT_EQ(shape_of(munch::regex::parse(R"("//"[^\n]*\n)"), '\n'), Shape::terminated);

    // The class narrows to [b] instead, which is the step the page reports, and no shape's edit is offered.
    const Token_set classed{
            .rules = {
                    {.regex = munch::regex::parse(R"([ab][xb])"), .id = 0, .priority = 0, .discarded = false},
                    {.regex = munch::regex::parse("x"), .id = 1, .priority = 1, .discarded = false}}};

    const auto narrowed{price(classed, 'x')};

    ASSERT_EQ(narrowed.steps.size(), 1U);
    EXPECT_EQ(narrowed.steps.front().token, 0U);
    EXPECT_EQ(narrowed.steps.front().shape, Shape::other);
    EXPECT_TRUE(narrowed.steps.front().exact);
    EXPECT_TRUE(narrowed.choices.empty());
    EXPECT_FALSE(narrowed.together.has_value());

    constexpr std::string_view spelling{R"(%%
[ab]"xb"               return AXB;
x                      return X;
)"};

    const auto spelled{read_flex(spelling).front()};

    const auto set{token_set(spelled, "INITIAL")};

    const auto priced{price(set, 'x')};

    EXPECT_TRUE(priced.choices.empty());
    EXPECT_FALSE(priced.together.has_value());
    EXPECT_EQ(priced.immovable, (std::vector<std::size_t>{0}));

    // The byte is asked for as the command asks for it, and the page offers no edit it did not evaluate.
    auto report{audit(set)};

    report.prices.push_back(priced);

    const auto text{render(report, names(spelled))};

    EXPECT_EQ(text.find("leave the terminator to the token after it"), std::string::npos);
    EXPECT_NE(
            text.find("spells 'x' out and cannot be narrowed; the byte cannot certify while it stays"),
            std::string::npos);
}

TEST(Report, An_opener_is_read_by_what_it_matches_and_not_by_its_spelling)
{
    // flex scans [x][ab]* and x{1}[ab]* as it scans "x"[ab]*, so the shape is the same: the first component matches
    // one fixed word the byte is not in, however the tree spells that. Reading the spelling called the first two
    // other and offered no edit, while the quoted one was delimited and its edit certified.
    for (const auto delimited :
         {R"("x"[ab]*)", R"([x][ab]*)", R"(x{1}[ab]*)", R"([x]{1}[ab]*)", R"((x|[x])[ab]*)", R"("xy"{2}[ab]*)",
          R"([a]"xb")", R"([a][xb])"})
    {
        EXPECT_EQ(shape_of(munch::regex::parse(delimited), 'b'), Shape::delimited) << delimited;
    }

    // An opener matching two words opens nothing the body can leave, and one holding the byte spells it fixed.
    EXPECT_EQ(shape_of(munch::regex::parse(R"([xy][ab]*)"), 'b'), Shape::other);
    EXPECT_EQ(shape_of(munch::regex::parse(R"(x?[ab]*)"), 'b'), Shape::other);
    EXPECT_EQ(shape_of(munch::regex::parse(R"(x{1,2}[ab]*)"), 'b'), Shape::other);
    EXPECT_EQ(shape_of(munch::regex::parse(R"([b][ab]*)"), 'b'), Shape::fixed);
    EXPECT_EQ(shape_of(munch::regex::parse(R"(b{1}[ab]*)"), 'b'), Shape::fixed);

    constexpr std::string_view classed{R"(%%
[x][ab]*               return T;
b                      return B;
)"};

    const auto file{read_flex(classed).front()};

    const auto set{token_set(file, "INITIAL")};

    const auto priced{price(set, 'b')};

    // The step narrows the body, and the shape's edit, the token cut to its opener, is offered beside it.
    ASSERT_EQ(priced.steps.size(), 1U);
    EXPECT_EQ(priced.steps.front().shape, Shape::delimited);
    ASSERT_EQ(priced.choices.size(), 1U);
    EXPECT_EQ(priced.choices.front().token, 0U);
    EXPECT_EQ(priced.choices.front().shape, Shape::delimited);
    EXPECT_TRUE(priced.choices.front().after.exact);
    ASSERT_TRUE(priced.together.has_value());
    EXPECT_TRUE(priced.together->exact);

    auto report{audit(set)};

    report.prices.push_back(priced);

    const auto text{render(report, names(file))};

    EXPECT_NE(
            text.find("delimited: scan the body in a start condition of its own, the opener staying here"),
            std::string::npos);
}

TEST(Report, A_terminator_is_read_by_what_it_matches_and_not_by_its_spelling)
{
    // flex scans [a]x{1}, [a]x{1,1} and [a][x]{1} as it scans [a]x, so the shape is the same: the last component
    // matches the one byte and nothing else, however the tree spells that. Reading the spelling called the three
    // with a count fixed and the byte unbuyable, while removing the count certified it.
    for (const auto terminated :
         {R"([a]x)", R"([a]x{1})", R"([a]x{1,1})", R"([a][x]{1})", R"([a](x|[x]))", R"([a](x{1}){1})"})
    {
        EXPECT_EQ(shape_of(munch::regex::parse(terminated), 'x'), Shape::terminated) << terminated;
    }

    // The tree can carry an empty text beside the byte, which exclude() leaves where a repetition stood.
    EXPECT_EQ(
            shape_of(
                    munch::regex::concat(
                            munch::regex::any_of(munch::regex::Set{'a'}),
                            munch::regex::concat(munch::regex::text(""), munch::regex::text("x"))),
                    'x'),
            Shape::terminated);

    // A last component matching more than the one byte, or another one, is no terminator.
    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab]x{2})"), 'x'), Shape::fixed);
    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab]x{1,2})"), 'x'), Shape::fixed);
    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab]x?)"), 'x'), Shape::other);
    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab](x|y))"), 'x'), Shape::other);
    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab]y{1})"), 'x'), Shape::other);

    // The opener matches two words, so the terminated edit is the one offered.
    constexpr std::string_view spelled_once{R"(%%
[ab]x{1}               return AX;
x                      return X;
)"};

    const auto file{read_flex(spelled_once).front()};

    const auto set{token_set(file, "INITIAL")};

    const auto priced{price(set, 'x')};

    // The narrowing cannot free the fixed x, and the shape's edit, the terminator left to the token after it, can.
    EXPECT_EQ(priced.immovable, (std::vector<std::size_t>{0}));
    ASSERT_EQ(priced.choices.size(), 1U);
    EXPECT_EQ(priced.choices.front().token, 0U);
    EXPECT_EQ(priced.choices.front().shape, Shape::terminated);
    EXPECT_TRUE(priced.choices.front().after.exact);
    ASSERT_TRUE(priced.together.has_value());
    EXPECT_TRUE(priced.together->exact);

    auto report{audit(set)};

    report.prices.push_back(priced);

    const auto text{render(report, names(file))};

    EXPECT_NE(text.find("terminated: leave the terminator to the token after it"), std::string::npos);
    EXPECT_EQ(text.find("the byte cannot certify while it stays"), std::string::npos);
}

TEST(Report, An_exact_zero_repetition_is_the_empty_word_whatever_it_repeats)
{
    // re2c 3.1 scans `[ab]([cd]{0}"x")` as it scans `[ab]"x"`, BODY of length two on "ax" and "bx" and X on "x",
    // since a repetition of exactly zero matches the empty word whatever it repeats, so the last component matches
    // the one byte and the shape is terminated; reading the repeated class first called the component two words
    // and the byte unbuyable, and the opener forms other. A repetition that may run once is read by its class, so
    // the byte stays fixed in the component.
    for (const auto terminated : {R"([ab]([cd]{0}x))", R"([ab]([cd]{0,0}x))", R"([ab](([cd]{0}){2}x))"})
    {
        EXPECT_EQ(shape_of(munch::regex::parse(terminated), 'x'), Shape::terminated) << terminated;
    }

    for (const auto delimited : {R"(([cd]{0}a)[bx]*)", R"((a[cd]{0})[bx]*)"})
    {
        EXPECT_EQ(shape_of(munch::regex::parse(delimited), 'x'), Shape::delimited) << delimited;
    }

    // The same whether the zero repetition stands in the sequence or in a group of its own, and whatever it repeats,
    // the byte itself included: `[ab][x]{0}"x"` and `[ab]([x]{0}"x")` are both `[ab]"x"` to re2c 3.1, so the shape
    // is terminated either way, the repetition admitting no byte.
    for (const auto same : {R"([ab][x]{0}"x")", R"([ab]([x]{0}"x"))", R"([ab][x]{0,0}x)", R"([ab]([x]{0}){3}x)"})
    {
        EXPECT_EQ(shape_of(munch::regex::parse(same), 'x'), Shape::terminated) << same;
    }

    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab][x]{0,1}"x")"), 'x'), Shape::fixed);

    // And the same at either end of the sequence: re2c 3.1 scans `[ab]"x"[cd]{0}` as `[ab]"x"` and `[cd]{0}"a"[bx]*`
    // as `"a"[bx]*`, the zero repetition being no part of the word, so the last component is the terminator and the
    // first the opener there too. Reading the sequence's ends by position saw the repetition as the last or first
    // component, called the terminated one delimited alone and the delimited one other, and priced the two spellings
    // of one language differently.
    for (const auto terminated : {R"([ab]"x"[cd]{0})", R"([ab]x[cd]{0}[cd]{0})", R"([cd]{0}[ab]"x"[cd]{0,0})"})
    {
        EXPECT_EQ(shape_of(munch::regex::parse(terminated), 'x'), Shape::terminated) << terminated;
    }

    for (const auto delimited : {R"([cd]{0}"a"[bx]*)", R"([cd]{0}[cd]{0}a[bx]*)", R"([cd]{0,0}a[bx]*[cd]{0})"})
    {
        EXPECT_EQ(shape_of(munch::regex::parse(delimited), 'x'), Shape::delimited) << delimited;
    }

    EXPECT_EQ(shape_of(munch::regex::parse(R"([cd]{0}[ \t\n]+)"), '\n'), Shape::run);

    // What a repetition repeats is normalised too, so a run written through a repetition of exactly one is the run
    // it matches, where taking an exactly-one repetition off at the top alone left these as a shape of no name.
    EXPECT_EQ(shape_of(munch::regex::parse(R"(([ \t\n]{1})+)"), '\n'), Shape::run);
    EXPECT_EQ(shape_of(munch::regex::parse(R"((([ \t\n]{1}){1})+)"), '\n'), Shape::run);
    EXPECT_EQ(shape_of(munch::regex::parse(R"(([ \t\n][cd]{0})+)"), '\n'), Shape::run);

    // Narrowing one consumer leaves the byte to a rule whose match it had won, and that rule's own shape is an
    // edit too: the shapes are read from what the edits so far leave, where reading the consumers once named the
    // first rule's shape alone and called the combined edit short of certifying. Here the first rule wins every
    // match the second would, so the second is blamed for nothing until the first stops admitting the newline.
    {
        const Token_set exposed{
                .rules = {
                        {.regex = munch::regex::parse(R"(a[\nx])"), .id = 1, .priority = 1, .discarded = false},
                        {.regex = munch::regex::parse(R"([a]\n)"), .id = 2, .priority = 1, .discarded = false},
                        {.regex = munch::regex::parse(R"([ \t]+)"), .id = 3, .priority = 1, .discarded = false}}};

        const auto priced{price(exposed, '\n')};

        EXPECT_TRUE(std::ranges::any_of(priced.choices, [](const auto& choice) { return choice.token == 2; }));
    }

    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab]([cd]{0,1}x))"), 'x'), Shape::fixed);
    EXPECT_EQ(shape_of(munch::regex::parse(R"([ab]([cd]{1}x))"), 'x'), Shape::fixed);

    // flex refuses a count of zero, so the scanner is re2c's; the terminated edit certifies x exactly.
    constexpr std::string_view zero{R"(/*!re2c
        [ab]([cd]{0}"x")    { return BODY; }
        "x"                 { return X; }
    */
)"};

    const auto file{read_re2c(zero).front()};

    const auto set{token_set(file, "INITIAL")};

    EXPECT_EQ(compile(set).tokenize<std::size_t>(std::string_view{"ax"}).length, 2U);

    const auto priced{price(set, 'x')};

    EXPECT_EQ(priced.immovable, (std::vector<std::size_t>{0}));
    ASSERT_EQ(priced.choices.size(), 1U);
    EXPECT_EQ(priced.choices.front().token, 0U);
    EXPECT_EQ(priced.choices.front().shape, Shape::terminated);
    EXPECT_TRUE(priced.choices.front().after.exact);

    auto report{audit(set)};

    report.prices.push_back(priced);

    const auto text{render(report, names(file))};

    EXPECT_NE(text.find("terminated: leave the terminator to the token after it"), std::string::npos);
    EXPECT_EQ(text.find("the byte cannot certify while it stays"), std::string::npos);
}

TEST(Report, A_repetition_that_may_run_zero_times_loses_the_byte_its_class_spells)
{
    // The class under the star in a[\n]*b holds nothing but the newline, so narrowing the class empties it and the
    // star runs zero times, leaving "ab" matching. Reading the sub-pattern alone called the rule immovable and
    // priced the newline as unbuyable while the edit an author would make was there all along.
    EXPECT_TRUE(can_lose(munch::regex::parse(R"(a[\n]*b)"), '\n'));
    EXPECT_TRUE(can_lose(munch::regex::parse(R"(a[\n]?b)"), '\n'));
    EXPECT_TRUE(can_lose(munch::regex::parse(R"(a[\n]{0,3}b)"), '\n'));

    // A repetition that must run at least once leaves the byte unavoidable, as a fixed spelling does.
    EXPECT_FALSE(can_lose(munch::regex::parse(R"(a[\n]+b)"), '\n'));
    EXPECT_FALSE(can_lose(munch::regex::parse(R"(a[\n]b)"), '\n'));

    // The edit performs the exclusion: what is left matches "ab" and no longer a newline between the two.
    auto narrowed{munch::regex::parse(R"(a[\n]*b)")};

    exclude(narrowed, '\n');

    const Token_set only{.rules = {{.regex = narrowed, .id = 0, .priority = 1, .discarded = false}}};

    const auto narrow_lexer{compile(only)};

    EXPECT_EQ(narrow_lexer.tokenize<std::size_t>(std::string{"ab"}).length, 2U);
    EXPECT_EQ(narrow_lexer.tokenize<std::size_t>(std::string{"a\nb"}).length, 0U);

    // The price then names a step rather than an immovable rule, and the newline certifies after it.
    const Token_set set{
            .rules = {
                    {.regex = munch::regex::parse(R"(\n)"), .id = 0, .priority = 1, .discarded = false},
                    {.regex = munch::regex::parse(R"(a[\n]*b)"), .id = 1, .priority = 1, .discarded = false}}};

    const auto priced{price(set, '\n')};

    EXPECT_TRUE(priced.immovable.empty());
    ASSERT_EQ(priced.steps.size(), 1U);
    EXPECT_EQ(priced.steps.front().token, 1U);
    EXPECT_TRUE(priced.steps.front().exact);
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
