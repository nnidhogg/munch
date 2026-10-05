#include "munch/tools/audit/report.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/dfa/boundary_search.hpp"
#include "munch/dfa/simulator.hpp"
#include "munch/regex/parse.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/price.hpp"
#include "munch/tools/audit/read_flex.hpp"
#include "munch/tools/audit/read_re2c.hpp"
#include "munch/tools/audit/token_set.hpp"

using namespace munch::tools::audit;
using munch::regex::any_of;
using munch::regex::choice;
using munch::regex::concat;
using munch::regex::kleene;
using munch::regex::parse;
using munch::regex::Set;
using munch::regex::text;

namespace
{
/**
 * @brief A grammar read and the report on its INITIAL condition.
 */
struct Audited
{
    /**
     * @brief The grammar's scanner as the flex reader reads it.
     */
    Lexer_spec file{};

    /**
     * @brief The report on its INITIAL condition.
     */
    Report report{};
};

/**
 * @brief One row of the split-points paper's applicability table: a grammar and the bytes it certifies.
 */
struct Applicability_row
{
    /**
     * @brief The grammar's file name.
     */
    std::string_view grammar{};

    /**
     * @brief The bytes it certifies exactly.
     */
    std::string_view exact{};

    /**
     * @brief The bytes it certifies once the discarded tokens are deleted.
     */
    std::string_view modulo{};
};

/**
 * @brief Reads one of the grammars beside the tests and audits its INITIAL condition.
 * @param grammar The grammar's file name.
 * @param window_limit The longest window tried.
 * @return The scanner and its report.
 */
Audited audited(const std::string_view grammar, const std::size_t window_limit = default_window_limit)
{
    const auto path{std::format("{}/tools/audit/grammars/{}", SOURCE_DIR, grammar)};

    std::ifstream stream{path};

    const std::string source{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};

    auto file{read_flex(source).front()};

    const auto set{token_set(file, "INITIAL")};

    auto report{audit(set, window_limit)};

    return {.file = std::move(file), .report = std::move(report)};
}

/**
 * @brief Returns the naming the report takes for a scanner's rules: a rule's returned token, or its pattern when it
 *        returns nothing.
 * @param file The scanner.
 * @return The naming.
 */
auto names(const Lexer_spec& file)
{
    return [&file](const std::size_t rule) {
        const auto& [pattern, expression, conditions, action, token, priority, line]{file.rules[rule]};

        return token.value_or(pattern);
    };
}

/**
 * @brief Returns the bytes of a text, as the report lists certified bytes.
 * @param text The text.
 * @return The bytes in order.
 */
std::vector<unsigned char> bytes(const std::string_view text)
{
    return {text.begin(), text.end()};
}

/**
 * @brief Returns a rule of a token set that is kept, not discarded.
 * @param pattern The rule's pattern, as regex::parse() reads it.
 * @param id The token id.
 * @param priority The rule's priority.
 * @return The rule.
 */
Token_rule rule(const std::string_view pattern, const std::size_t id, const std::size_t priority)
{
    return {.regex = parse(pattern), .id = id, .priority = priority, .discarded = false};
}

/**
 * @brief Returns whether a blame entry is the newline's.
 * @param entry The entry.
 * @return True when it is.
 */
bool on_newline(const Blame& entry)
{
    return entry.byte == '\n';
}

/**
 * @brief Returns the text report of a set with a byte's pricing added, as the command adds one it is asked for.
 * @param set The token set.
 * @param priced The pricing.
 * @param file The scanner the set was built from, whose rules name the tokens.
 * @return The text.
 */
std::string priced_page(const Token_set& set, const Pricing& priced, const Lexer_spec& file)
{
    auto report{audit(set)};

    report.prices.push_back(priced);

    return render(report, names(file));
}

} // namespace

TEST(Report_test, The_flex_grammars_reproduce_the_applicability_rows)
{
    // Each row of the split-points paper's table, read through a .l file rather than typed into the library.
    const std::vector<Applicability_row> rows{
            {.grammar = "c-like-conventional.l", .exact = "", .modulo = "\n"},
            {.grammar = "c-like-split-friendly.l", .exact = "\n", .modulo = "\n"},
            {.grammar = "c-like-block-comments.l", .exact = "", .modulo = ""},
            {.grammar = "json.l", .exact = "", .modulo = "\t\n\r"},
            {.grammar = "log-lines.l", .exact = "\n", .modulo = "\n"}};

    for (const auto& [grammar, exact, modulo] : rows)
    {
        const auto [file, report]{audited(grammar)};

        EXPECT_EQ(report.exact, bytes(exact)) << grammar;
        EXPECT_EQ(report.modulo, bytes(modulo)) << grammar;
    }
}

TEST(Report_test, Windows_and_the_core_answer_where_bytes_do_not)
{
    // The conventional row gains two-byte windows, among them the paper's own: a newline followed by a byte that must
    // begin a token, certified at the byte.
    const auto conventional{audited("c-like-conventional.l")};

    const auto papers_own{[](const Certified_window& certified) {
        const auto& [window, origin]{certified};

        return window == "\n!" && origin == 1;
    }};

    EXPECT_TRUE(std::ranges::any_of(conventional.report.windows, papers_own));

    // Block comments force "*/" into every certified window, and no two-byte window certifies.
    const auto blocks{audited("c-like-block-comments.l")};

    EXPECT_EQ(blocks.report.mandatory_core, "*/");

    const auto two_wide{[](const Certified_window& certified) { return certified.window.size() == 2; }};

    EXPECT_TRUE(std::ranges::none_of(blocks.report.windows, two_wide));

    // Every real row has an unbounded byte span: an identifier of any length carries no anchor.
    EXPECT_FALSE(conventional.report.byte_span.has_value());
    EXPECT_FALSE(blocks.report.byte_span.has_value());
}

TEST(Report_test, Every_certified_window_of_the_conventional_row_occurs)
{
    // The certified-splitting paper splits a row's certificates into occurring and vacuous ones, a vacuous window being
    // one no completely tokenizable input contains. Over the conventional row read as bytes every certified window
    // occurs, and the decision places each in an input that tokenizes completely and contains it: the conservative
    // model refuses a window no live history crosses, so the vacuous certificates the paper's class abstraction reports
    // for this row, a newline followed by a byte only a string or a line comment holds, come back here refused rather
    // than certified, and occurring nowhere.
    const auto [file, report]{audited("c-like-conventional.l")};

    const auto lexer{build(file, "INITIAL")};

    ASSERT_FALSE(report.windows.empty());

    const auto ignore_token{[](std::size_t, std::size_t) {}};

    for (const auto& [window, origin] : report.windows)
    {
        const auto [witness, exhaustive]{lexer.window_occurrence(window)};

        ASSERT_TRUE(exhaustive) << window;
        ASSERT_FALSE(witness.empty()) << window;
        EXPECT_TRUE(witness.contains(window)) << window;

        const auto consumed{lexer.tokenize_all<std::size_t>(witness, ignore_token)};

        EXPECT_EQ(consumed, witness.size()) << window;
    }

    EXPECT_FALSE(lexer.is_split_window("\n#").has_value());

    const auto [refused_witness, refused_exhaustive]{lexer.window_occurrence("\n#")};

    EXPECT_TRUE(refused_exhaustive);
    EXPECT_TRUE(refused_witness.empty());
}

TEST(Report_test, Every_certified_window_of_the_conventional_row_is_exact)
{
    // The report certifies its windows through the conservative model, and the certified-splitting paper's decision
    // holds each against every completely tokenizable input: an exhaustive search with no counterexample proves the
    // certificate exact at its origin, which every window certified at width 3 on the conventional row is. The window
    // the model refuses and no input holds has no counterexample either, its certificate being vacuous.
    const auto [file, report]{audited("c-like-conventional.l")};

    const auto lexer{build(file, "INITIAL")};

    ASSERT_FALSE(report.windows.empty());

    for (const auto& [window, origin] : report.windows)
    {
        const auto [witness, exhaustive]{lexer.window_counterexample(window, origin)};

        ASSERT_TRUE(exhaustive) << window;
        EXPECT_TRUE(witness.empty()) << window << " failed by " << witness;
    }

    const auto [vacuous_witness, vacuous_exhaustive]{lexer.window_counterexample("\n#", 1)};

    EXPECT_TRUE(vacuous_exhaustive);
    EXPECT_TRUE(vacuous_witness.empty());
}

TEST(Report_test, The_conventional_and_split_friendly_rows_separate_on_the_boundary_half)
{
    // The two rows of the split-points study tokenize the same inputs and differ in their whitespace treatment alone,
    // the split-friendly row making the newline its own token, so full equivalence decided directly separates them on
    // the boundary half: the shortest marked run only one side accepts is a whitespace byte then a newline, one token
    // under the conventional row and two under the split-friendly one, the minimal disagreement the certified-splitting
    // paper's auditor synthesizes. A boundary witness is a boundary_difference() witness, and the conventional row
    // against itself is one segmentation function over every input.
    const auto conventional_audit{audited("c-like-conventional.l")};

    const auto friendly_audit{audited("c-like-split-friendly.l")};

    const auto conventional{build(conventional_audit.file, "INITIAL")};

    const auto friendly{build(friendly_audit.file, "INITIAL")};

    const auto [witness, half, exhaustive]{conventional.segmentation_difference(friendly)};

    ASSERT_TRUE(exhaustive);
    EXPECT_EQ(witness, "\t\n");
    EXPECT_EQ(half, munch::dfa::Separation_half::boundary);

    const auto [boundary_witness, boundary_exhaustive]{conventional.boundary_difference(friendly)};

    const auto [reversed_witness, reversed_half, reversed_exhaustive]{friendly.segmentation_difference(conventional)};

    EXPECT_EQ(boundary_witness.size(), witness.size());
    EXPECT_EQ(reversed_half, munch::dfa::Separation_half::boundary);

    const auto [none, no_half, settled]{conventional.segmentation_difference(conventional)};

    EXPECT_TRUE(settled);
    EXPECT_TRUE(none.empty());
    EXPECT_FALSE(no_half.has_value());
}

TEST(Report_test, Blame_names_the_token_that_consumes_a_candidate_mid_token)
{
    const auto [file, report]{audited("c-like-conventional.l")};

    // The newline is a candidate, and only the whitespace run consumes it mid-token.
    std::vector<std::size_t> newline_tokens{};

    for (const auto& [byte, token, after] : report.blame | std::views::filter(on_newline))
    {
        newline_tokens.push_back(token);

        EXPECT_EQ(file.rules[token].pattern, R"([ \t\n]+)");

        // A shortest input reaching the run's state is one blank, whichever the search met first.
        EXPECT_TRUE(after == " " || after == "\t") << after;
    }

    EXPECT_EQ(newline_tokens.size(), 1U);

    const auto on_bang{[](const Blame& entry) { return entry.byte == '!'; }};

    // '!' is consumed inside strings and line comments, after a quote and after the two slashes respectively.
    std::vector<std::string> bang_after{};

    std::ranges::copy(
            report.blame | std::views::filter(on_bang) | std::views::transform(&Blame::after),
            std::back_inserter(bang_after));

    std::ranges::sort(bang_after);

    EXPECT_EQ(bang_after, (std::vector<std::string>{R"(")", "//"}));
}

TEST(Report_test, Blame_names_a_longer_token_whose_accept_lies_beyond_a_shorter_one)
{
    // The newline itself, a[\nx] and a[\nx]b: after the "a" the newline leads to the state accepting the second rule,
    // and the third rule's scan stands in that same state, so both consume the newline mid-token.
    const Token_set set{.rules = {rule(R"(\n)", 0, 1), rule(R"(a[\nx])", 1, 1), rule(R"(a[\nx]b)", 2, 1)}};

    const auto entries{blame(compile(set))};

    std::vector<std::size_t> blamed{};

    for (const auto& [byte, token, after] : entries | std::views::filter(on_newline))
    {
        blamed.push_back(token);

        EXPECT_EQ(after, "a") << token;
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

TEST(Report_test, An_alternative_matching_nothing_is_no_part_of_the_pattern_the_price_reads)
{
    // A set assembled through the API may hold an alternative matching no word at all, any_of(""), which no file syntax
    // spells; the choice matches what its other alternative does, so `[ab]\n` and the choice of it with nothing are one
    // pattern to the scanner and must be one to the price: the same terminated shape, the same steps and the same
    // outcome.
    const Token_set plain{.rules = {rule(R"([ab]\n)", 0, 1), rule("x", 1, 1)}};

    const Token_set with_nothing{
            .rules = {
                    {.regex = choice(concat(any_of(Set{'a', 'b'}), text('\n')), any_of(Set{})),
                     .id = 0,
                     .priority = 1,
                     .discarded = false},
                    rule("x", 1, 1)}};

    // A repetition of nothing that need not repeat it is the empty word, and no part of a sequence: `[ab]\n` followed
    // by `(any_of(""))*` is `[ab]\n` too.
    const Token_set with_trailing{
            .rules = {
                    {.regex = concat(any_of(Set{'a', 'b'}), text('\n'), kleene(any_of(Set{}))),
                     .id = 0,
                     .priority = 1,
                     .discarded = false},
                    rule("x", 1, 1)}};

    // A sequence whose parts all match the empty word alone is the empty word and no sequence of nothing: pricing
    // `a(x{0}y{0}|b)c`, which matches "ac" and "abc", throws nothing.
    const Token_set emptied{.rules = {rule("a(x{0}y{0}|b)c", 0, 1)}};

    EXPECT_NO_THROW(std::ignore = price(emptied, 'c'));

    const auto expected{price(plain, '\n')};

    const auto same_as_plain{[&expected](const Token_set& set) {
        const auto priced{price(set, '\n')};

        EXPECT_EQ(priced.exact_before, expected.exact_before);
        EXPECT_EQ(priced.modulo_before, expected.modulo_before);
        EXPECT_EQ(priced.immovable, expected.immovable);
        EXPECT_EQ(priced.undecided, expected.undecided);
        EXPECT_EQ(priced.choices.size(), expected.choices.size());
        EXPECT_EQ(priced.together.has_value(), expected.together.has_value());
        ASSERT_EQ(priced.steps.size(), expected.steps.size());

        for (const auto& [ours, theirs] : std::views::zip(priced.steps, expected.steps))
        {
            EXPECT_EQ(ours.token, theirs.token);
            EXPECT_EQ(ours.shape, theirs.shape);
            EXPECT_EQ(ours.separated, theirs.separated);
            EXPECT_EQ(ours.exact, theirs.exact);
            EXPECT_EQ(ours.modulo, theirs.modulo);
        }
    }};

    same_as_plain(with_nothing);

    same_as_plain(with_trailing);
}

TEST(Report_test, A_re_entrant_initial_state_is_blamed_for_what_it_consumes_mid_token)
{
    // The star at the head of [\n]*b returns the scan to the initial state, so on "\n\nb" the second newline stands
    // mid-token in the very state a byte may also begin a token in. The initial state is blamed there like any other
    // state, so the byte the certificate refuses has a consumer the price names.
    const Token_set set{.rules = {rule(R"([\n]*b)", 0, 1)}};

    const auto lexer{compile(set)};

    EXPECT_TRUE(lexer.simulator().init_reentrant());
    EXPECT_FALSE(lexer.is_split_point('\n'));

    std::vector<std::string> newline_after{};

    const auto blamed{blame(lexer)};

    for (const auto& [byte, token, after] : blamed | std::views::filter(on_newline))
    {
        EXPECT_EQ(token, 0U);

        newline_after.push_back(after);
    }

    // The input reported is a shortest one that returns to the initial state, never the empty one it is entered by.
    EXPECT_EQ(newline_after, (std::vector<std::string>{"\n"}));

    // The price then names the consumer, and the newline taken out of the run and given a token of its own certifies.
    const auto priced{price(set, '\n')};

    ASSERT_EQ(priced.steps.size(), 1U);
    EXPECT_EQ(priced.steps.front().token, 0U);
    EXPECT_TRUE(priced.steps.front().separated);
    EXPECT_TRUE(priced.steps.front().exact);

    const auto report{audit(set)};

    const auto name{[](const std::size_t) { return std::string{"B"}; }};

    const auto text{render(report, name)};

    EXPECT_TRUE(text.contains(R"(what it would cost to certify '\n')"));
    EXPECT_TRUE(
            text.contains(R"(B                        no longer admits '\n', and '\n' becomes a token of its own)"));
}

TEST(Report_test, Rendering_reads_as_the_sections)
{
    const auto [file, report]{audited("c-like-conventional.l")};

    const auto text{render(report, names(file))};

    EXPECT_TRUE(text.contains("certified bytes             none"));
    EXPECT_TRUE(text.contains(R"(certified modulo discarded  '\n')"));

    // The two rules returning nothing are what the modulo row deleted, and the page says so.
    EXPECT_EQ(report.discarded, (std::vector<std::size_t>{5, 6}));
    EXPECT_TRUE(text.contains(R"(discarded tokens            2: "//"[^\n]*, [ \t\n]+)"));

    // The verdict leads, and the JSON form carries the same figures under their names.
    EXPECT_TRUE(text.starts_with("verdict                     no byte certifies exactly; 1 certifies once the "));

    const auto document{json(report, names(file))};

    EXPECT_TRUE(document.contains(R"("verdict": "no byte certifies exactly; 1 certifies once the discarded tokens)"));
    EXPECT_TRUE(document.contains(R"("exact": [])"));
    EXPECT_TRUE(document.contains(R"("modulo": [10])"));
    EXPECT_TRUE(
            document.contains(R"("discarded": [{"id": 5, "name": "\"//\"[^\\n]*"}, {"id": 6, "name": "[ \\t\\n]+"}])"));
    EXPECT_TRUE(document.contains(R"("byte_span": "unbounded")"));
    EXPECT_TRUE(document.contains(R"("mandatory_core": "")"));
    EXPECT_TRUE(text.contains("anchor-free span, bytes     unbounded"));
    EXPECT_TRUE(text.contains("why candidate bytes do not certify"));
    EXPECT_TRUE(text.contains("STRING"));
}

TEST(Report_test, A_window_count_no_size_t_holds_is_reported_as_the_bound_it_passed)
{
    // One token matching any single byte: every byte moves every state alike, so the tables hold one byte class of 256
    // and one certified window per width. A window of the widest width the command accepts, eight, therefore stands for
    // 256^8 byte strings, which is one more than a std::size_t counts, and the widths below it sum to
    // 0x0101010101010100. The count is multiplied checked, so the wider one is reported as the bound it passed and not
    // as a wrapped number.
    const Token_set any{.rules = {{.regex = any_of(Set::all()), .id = 0, .priority = 1, .discarded = false}}};

    const auto lexer{compile(any)};

    const auto name{[](const std::size_t) { return std::string{"any"}; }};

    const auto narrow{audit(lexer, 7)};

    const auto narrow_document{json(narrow, name)};

    // The sum of 256^w over the widths w from one to seven, 0x0101010101010100.
    constexpr std::size_t narrow_count{72340172838076416U};

    ASSERT_TRUE(narrow.window_count.has_value());
    EXPECT_EQ(*narrow.window_count, narrow_count);
    EXPECT_TRUE(narrow_document.contains(std::format(R"("window_count": {})", narrow_count)));

    const auto wide{audit(lexer, 8)};

    EXPECT_FALSE(wide.window_count.has_value());

    const auto text{render(wide, name)};

    const auto wide_document{json(wide, name)};

    const auto most{std::numeric_limits<std::size_t>::max()};

    EXPECT_TRUE(text.contains(std::format("more than {} once classes expand", most)));
    EXPECT_FALSE(text.contains(std::to_string(narrow_count)));
    EXPECT_TRUE(wide_document.contains(std::format(R"("window_count": "more than {}")", most)));
}

TEST(Report_test, The_options_that_governed_the_reading_reach_both_forms_of_the_report)
{
    // A flex file's `%option` words, in the reader's own order, as the row the report prints under the scanner and as
    // the array its JSON account carries; a scanner that declares none prints no row at all.
    const auto [file, report]{audited("c-like-split-friendly.l")};

    EXPECT_EQ(file.options, (std::vector<std::string>{"noyywrap", "nodefault"}));
    EXPECT_EQ(options_row(file.options), "options                     noyywrap, nodefault\n");
    EXPECT_EQ(options_json(file.options), R"(["noyywrap", "nodefault"])");

    EXPECT_EQ(options_row({}), "");
    EXPECT_EQ(options_json({}), "[]");

    // Several options are one row, comma-separated; what a logos reading notes of the Unicode version its classes came
    // from is one of them, so the page says which language was analysed.
    const std::vector<std::string> several{"unicode-classes=16.0.0", "extras=Extras"};

    EXPECT_EQ(options_row(several), "options                     unicode-classes=16.0.0, extras=Extras\n");
    EXPECT_EQ(options_json(several), R"(["unicode-classes=16.0.0", "extras=Extras"])");
}

TEST(Report_test, Pricing_follows_the_design_rows_of_the_study)
{
    // The conventional row: one edit, the whitespace run loses the newline and the newline becomes a discarded token of
    // its own, and the newline certifies exactly. That is the split-friendly row.
    const auto [conventional_file, conventional_report]{audited("c-like-conventional.l")};

    const auto conventional_set{token_set(conventional_file, "INITIAL")};

    const auto conventional{price(conventional_set, '\n')};

    EXPECT_FALSE(conventional.exact_before);
    EXPECT_TRUE(conventional.modulo_before);
    ASSERT_EQ(conventional.steps.size(), 1U);
    EXPECT_TRUE(conventional.steps[0].separated);
    EXPECT_TRUE(conventional.steps[0].separated_discarded);
    EXPECT_TRUE(conventional.steps[0].exact);
    EXPECT_TRUE(conventional.immovable.empty());

    // With block comments: bounding the comment to a line buys the newline modulo discarded tokens, splitting the
    // whitespace run then buys it exactly, in that order, as the paper's two design rows had it.
    const auto blocks{audited("c-like-block-comments.l")};

    const auto blocks_set{token_set(blocks.file, "INITIAL")};

    const auto priced{price(blocks_set, '\n')};

    EXPECT_FALSE(priced.exact_before);
    EXPECT_FALSE(priced.modulo_before);
    ASSERT_EQ(priced.steps.size(), 2U);

    const auto& comment_rule{blocks.file.rules[priced.steps[0].token]};

    const auto& run_rule{blocks.file.rules[priced.steps[1].token]};

    EXPECT_EQ(comment_rule.pattern, R"("/*"([^*]|\*+[^*/])*\*+"/")");
    EXPECT_FALSE(priced.steps[0].separated);
    EXPECT_FALSE(priced.steps[0].exact);
    EXPECT_TRUE(priced.steps[0].modulo);
    EXPECT_EQ(run_rule.pattern, R"([ \t\n]+)");
    EXPECT_TRUE(priced.steps[1].separated);
    EXPECT_TRUE(priced.steps[1].exact);
    EXPECT_TRUE(priced.immovable.empty());

    // The comment is discarded, but its step separated nothing, so there is no token of the byte's own to be discarded
    // there; the run's step separated one, discarded as the run is.
    EXPECT_EQ(comment_rule.token, std::nullopt);
    EXPECT_FALSE(priced.steps[0].separated_discarded);
    EXPECT_TRUE(priced.steps[1].separated_discarded);

    // A byte a fixed spelling holds cannot certify while that token stays: '=' against "==".
    const auto fixed{
            read_flex("%%\n\"==\"      return EQUAL;\n[=]        return ASSIGN;\n[a-z]+     return WORD;\n").front()};

    const auto fixed_set{token_set(fixed, "INITIAL")};

    const auto equals{price(fixed_set, '=')};

    EXPECT_FALSE(equals.exact_before);
    EXPECT_EQ(equals.immovable, (std::vector<std::size_t>{0}));
    EXPECT_TRUE(equals.steps.empty());
}

TEST(Report_test, Pricing_narrows_the_consumer_an_earlier_edit_exposes)
{
    // A's match of "a\n" is what keeps B from consuming the newline, so the blame names A alone; once A loses the byte,
    // B wins that match and must be narrowed too. The price asks the blame again after every edit, so it takes both
    // steps.
    const Token_set exposed{.rules = {rule(R"(\n)", 0, 0), rule(R"(a[\nx])", 1, 1), rule(R"(a[\ny])", 2, 2)}};

    const auto priced{price(exposed, '\n')};

    ASSERT_EQ(priced.steps.size(), 2U);
    EXPECT_EQ(priced.steps.front().token, 1U);
    EXPECT_FALSE(priced.steps.front().exact);
    EXPECT_EQ(priced.steps.back().token, 2U);
    EXPECT_TRUE(priced.steps.back().exact);
    EXPECT_TRUE(priced.immovable.empty());

    // The same where priority alone hides the second consumer: the first rule and the second match the same runs, the
    // first wins them, and the second is the run that consumes the newline once the first no longer does.
    const Token_set shadowed{.rules = {rule(R"([\nx]+)", 0, 0), rule(R"([\nx]+|y)", 1, 1)}};

    const auto repriced{price(shadowed, '\n')};

    ASSERT_EQ(repriced.steps.size(), 2U);
    EXPECT_EQ(repriced.steps.front().token, 0U);
    EXPECT_FALSE(repriced.steps.front().exact);
    EXPECT_EQ(repriced.steps.back().token, 1U);
    EXPECT_TRUE(repriced.steps.back().separated);
    EXPECT_TRUE(repriced.steps.back().exact);

    // The token the byte was given of its own is the analysis's, none of the set's rules, so no round of the repricing
    // narrows it and nothing the report names is an id the caller cannot name.
    EXPECT_LT(repriced.steps.back().token, shadowed.rules.size());
    EXPECT_TRUE(repriced.immovable.empty());
    EXPECT_TRUE(repriced.undecided.empty());
}

TEST(Report_test, A_byte_no_token_begins_with_is_given_one_before_it_is_priced)
{
    // The newline here ends a line and begins nothing, so neither certificate reports it and the blame is silent about
    // it. The byte is given a token of its own first, and the line token is then priced as any consumer is: immovable
    // for the narrowing, and its terminated shape leaves the newline to the token it now has; the opener matches two
    // words, so that edit is the one offered.
    const Token_set lines{.rules = {rule(R"([ab]\n)", 0, 0)}};

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
    const Token_set absent{.rules = {rule(R"([a])", 0, 0)}};

    const auto held{price(absent, '\n')};

    ASSERT_TRUE(held.given.has_value());
    EXPECT_TRUE(held.given->exact);
    EXPECT_TRUE(held.steps.empty());
    EXPECT_TRUE(held.immovable.empty());

    // A byte some token begins with is priced with nothing given.
    const Token_set begun{.rules = {rule(R"(\n)", 0, 0), rule(R"(a[\nx])", 1, 1)}};

    const auto begun_price{price(begun, '\n')};

    EXPECT_FALSE(begun_price.given.has_value());

    // The report prices the newline on its own, and both forms say what the case is rather than nothing.
    const auto report{audit(lines)};

    ASSERT_EQ(report.prices.size(), 1U);
    EXPECT_TRUE(report.prices[0].given.has_value());

    const auto name{[](const std::size_t) { return std::string{"LINE"}; }};

    const auto text{render(report, name)};

    EXPECT_TRUE(text.contains(
            R"(no token begins with '\n'  neither certificate reports it; it is given a token of its own)"));
    EXPECT_TRUE(text.contains(R"(LINE                       spells '\n' out and cannot be narrowed; its shape)"));

    const auto document{json(report, name)};

    const auto absent_report{audit(absent)};

    const auto absent_text{render(absent_report, name)};

    EXPECT_TRUE(document.contains(R"("given": {"exact": false, "modulo": false, "gained": []})"));
    EXPECT_TRUE(absent_text.contains("certifies exactly"));
}

TEST(Report_test, Steps_follow_the_order_of_the_rules_and_not_of_their_ids)
{
    // Both openers consume the newline before any edit, so both are answered before any other, in the order the set
    // lists them: the rule with id 9 stands first.
    const Token_set set{.rules = {rule(R"(\n)", 5, 0), rule(R"(a[\nx])", 9, 1), rule(R"(b[\nx])", 2, 2)}};

    const auto priced{price(set, '\n')};

    ASSERT_EQ(priced.steps.size(), 2U);
    EXPECT_EQ(priced.steps.front().token, 9U);
    EXPECT_FALSE(priced.steps.front().exact);
    EXPECT_EQ(priced.steps.back().token, 2U);
    EXPECT_TRUE(priced.steps.back().exact);
}

TEST(Report_test, The_token_a_byte_is_given_of_its_own_takes_an_id_no_rule_carries_whatever_the_ids_are)
{
    // The ids are the caller's and any std::size_t is one: with `a[xy]` at the largest and `x` one below it, x does not
    // certify and narrowing the opener certifies it, as it does under ids 0 and 1. The byte's own token takes an id no
    // rule carries, so the opener is the one step whatever the ids are.
    constexpr auto largest{std::numeric_limits<std::size_t>::max()};

    for (const auto& [opener, terminator] : {std::pair{0UZ, 1UZ}, std::pair{largest, largest - 1}, std::pair{1UZ, 0UZ}})
    {
        const Token_set set{.rules = {rule(R"(a[xy])", opener, 0), rule("x", terminator, 1)}};

        const auto lexer{compile(set)};

        EXPECT_FALSE(lexer.is_split_point('x'));

        const auto priced{price(set, 'x')};

        ASSERT_EQ(priced.steps.size(), 1U) << opener;
        EXPECT_EQ(priced.steps.front().token, opener);
        EXPECT_TRUE(priced.steps.front().exact);
        EXPECT_TRUE(priced.immovable.empty());
        EXPECT_TRUE(priced.undecided.empty());
    }

    // The token the byte is given where none begins with it takes the smallest id no rule carries, 1 between 0 and 2,
    // which no step names, so the one step is the opener's and certifies.
    const Token_set unmatched{.rules = {rule(R"(a[xy])", 0, 0), rule("b", 2, 1)}};

    const auto priced{price(unmatched, 'x')};

    ASSERT_TRUE(priced.given.has_value());
    ASSERT_EQ(priced.steps.size(), 1U);
    EXPECT_EQ(priced.steps.front().token, 0U);
    EXPECT_TRUE(priced.steps.front().exact);
}

TEST(Report_test, A_byte_a_token_begins_with_obstructs_nothing_and_is_not_called_immovable)
{
    // The opener's newline is the rule's fixed occurrence of the byte, and it is the one the initial state consumes,
    // where a byte may begin a token. The narrowing cannot take a byte out of a spelling, and that leaves the byte
    // buyable while the rule stays: the class alone narrowed, it certifies. The opener may be a class of the one byte,
    // a repetition of the text, a group, or one alternative among others that begin the same way; each is undecided and
    // none immovable.
    const auto priced{[](const std::string_view opener) {
        const Token_set set{.rules = {rule(R"(\n)", 0, 0), rule(opener, 1, 1)}};

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
        const Token_set set{.rules = {rule(R"(\n)", 0, 0), rule(narrowed, 1, 1)}};

        const auto lexer{compile(set)};

        EXPECT_TRUE(lexer.is_split_point('\n')) << narrowed;
    }

    // A fixed occurrence past the first byte on every path is the obstruction the necessity theorem names, and it stays
    // immovable: spelled mid-token, spelled twice, or repeated to a second occurrence.
    for (const auto inside : {R"([x]\n[x])", R"([xy]\n[x])", R"(\n\n)", R"(\n{2})", R"((\n[x]|x\n)\n)"})
    {
        const auto pricing{priced(inside)};

        EXPECT_EQ(pricing.immovable, (std::vector<std::size_t>{1})) << inside;
        EXPECT_TRUE(pricing.undecided.empty()) << inside;
    }

    // The page says which of the two it is, and claims impossibility only where the theorem proves it and no shape
    // offers an edit: the opener of [xy]\n[x] matches two words, so none does.
    const Token_set set{.rules = {rule(R"(\n)", 0, 0), rule(R"([\n][\nx])", 1, 1)}};

    const Token_set inside{.rules = {rule(R"(\n)", 0, 0), rule(R"([xy]\n[x])", 1, 1)}};

    const auto report{audit(set)};

    const auto name{[](const std::size_t id) { return std::string{id == 0 ? "NEWLINE" : "OPENER"}; }};

    const auto text{render(report, name)};

    EXPECT_TRUE(text.contains(R"(OPENER                     spells '\n' out, on some path only as its first byte)"));
    EXPECT_TRUE(text.contains("no narrowing applies to a fixed spelling, so this edit decides nothing"));
    EXPECT_FALSE(text.contains("the byte cannot certify while it stays"));

    const auto inside_report{audit(inside)};

    const auto inside_name{[](const std::size_t id) { return std::string{id == 0 ? "NEWLINE" : "INSIDE"}; }};

    const auto inside_text{render(inside_report, inside_name)};

    EXPECT_TRUE(inside_text.contains("the byte cannot certify while it stays"));
}

TEST(Report_test, Shapes_name_the_edit_an_author_would_make_and_each_is_tried_on_its_own)
{
    // The whitespace run is a run; the block comment is delimited; the line comment ending in its newline is terminated
    // (and delimited too, terminated winning the name); the two-byte spelling is fixed.
    const auto [blocks, blocks_report]{audited("c-like-block-comments.l")};

    const auto expression_of{[&blocks](const std::string_view pattern) {
        return std::ranges::find(blocks.rules, pattern, &Lexer_spec::Rule::pattern)->expression;
    }};

    const auto run_expression{expression_of(R"([ \t\n]+)")};

    const auto comment_expression{expression_of(R"("/*"([^*]|\*+[^*/])*\*+"/")")};

    EXPECT_EQ(shape_of(parse(run_expression), '\n'), Shape::run);
    EXPECT_EQ(shape_of(parse(comment_expression), '\n'), Shape::delimited);
    EXPECT_EQ(shape_of(parse(R"("//"[^\n]*\n)"), '\n'), Shape::terminated);
    EXPECT_EQ(shape_of(parse(R"("==")"), '='), Shape::fixed);
    EXPECT_EQ(shape_of(parse(R"([a-z]+|"\n")"), '\n'), Shape::other);

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

    const auto strings_set{token_set(strings, "INITIAL")};

    const auto priced{price(strings_set, '\n')};

    EXPECT_FALSE(priced.exact_before);
    ASSERT_EQ(priced.steps.size(), 1U);
    EXPECT_EQ(priced.steps[0].shape, Shape::delimited);

    // The narrowing cannot take the newline out of the comment's fixed terminator, so it is immovable there; the
    // choices can: the string's delimited edit and the comment's terminated and delimited edits, each alone.
    EXPECT_EQ(priced.immovable, (std::vector<std::size_t>{2}));
    ASSERT_EQ(priced.choices.size(), 3U);
    EXPECT_EQ(priced.choices[0].token, 1U);
    EXPECT_EQ(priced.choices[0].shape, Shape::delimited);
    EXPECT_FALSE(priced.choices[0].after.exact);
    EXPECT_EQ(priced.choices[1].token, 2U);
    EXPECT_EQ(priced.choices[1].shape, Shape::terminated);
    EXPECT_FALSE(priced.choices[1].after.exact);
    EXPECT_EQ(priced.choices[2].token, 2U);
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

    const auto alone_set{token_set(alone, "INITIAL")};

    const auto bought{price(alone_set, '\n')};

    ASSERT_EQ(bought.choices.size(), 1U);
    EXPECT_EQ(bought.choices[0].shape, Shape::delimited);
    EXPECT_TRUE(bought.choices[0].after.exact);
    EXPECT_FALSE(bought.choices[0].after.gained.empty());

    const auto alone_report{audit(alone_set)};

    const auto text{render(alone_report, names(alone))};

    EXPECT_TRUE(text.contains("delimited: scan the body in a start condition of its own"));
}

TEST(Report_test, A_terminated_shape_names_the_edit_that_was_evaluated_and_no_other)
{
    // The shape's edit deletes the token's last component, so the shape holds only where that component is the
    // terminator. The last component of [ab]"xb" admits the 'x' while the token ends in 'b', so deleting it deletes the
    // token's own 'b' too and leaves no terminator to the token after it. The class of [ab][xb] admits the 'x' the same
    // way: the class holds the byte, and it is not the byte alone. The openers here match two words, so no delimited
    // edit stands in for the terminated one.
    EXPECT_EQ(shape_of(parse(R"([ab]"xb")"), 'x'), Shape::fixed);
    EXPECT_EQ(shape_of(parse(R"([ab][xb])"), 'x'), Shape::other);
    EXPECT_EQ(shape_of(parse(R"([ab]"x")"), 'x'), Shape::terminated);
    EXPECT_EQ(shape_of(parse(R"([ab][x])"), 'x'), Shape::terminated);
    EXPECT_EQ(shape_of(parse(R"("//"[^\n]*\n)"), '\n'), Shape::terminated);

    // The class narrows to [b] instead, which is the step the page reports, and no shape's edit is offered.
    const Token_set classed{.rules = {rule(R"([ab][xb])", 0, 0), rule("x", 1, 1)}};

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
    const auto text{priced_page(set, priced, spelled)};

    EXPECT_FALSE(text.contains("leave the terminator to the token after it"));
    EXPECT_TRUE(text.contains("spells 'x' out and cannot be narrowed; the byte cannot certify while it stays"));
}

TEST(Report_test, An_opener_is_read_by_what_it_matches_and_not_by_its_spelling)
{
    // flex scans [x][ab]* and x{1}[ab]* as it scans "x"[ab]*, so the shape is the same: the first component matches one
    // fixed word the byte is not in, however the tree spells that. Each is delimited, and its edit certifies.
    for (const auto delimited :
         {R"("x"[ab]*)", R"([x][ab]*)", R"(x{1}[ab]*)", R"([x]{1}[ab]*)", R"((x|[x])[ab]*)", R"("xy"{2}[ab]*)",
          R"([a]"xb")", R"([a][xb])"})
    {
        EXPECT_EQ(shape_of(parse(delimited), 'b'), Shape::delimited) << delimited;
    }

    // An opener matching two words opens nothing the body can leave, and one holding the byte spells it fixed.
    EXPECT_EQ(shape_of(parse(R"([xy][ab]*)"), 'b'), Shape::other);
    EXPECT_EQ(shape_of(parse(R"(x?[ab]*)"), 'b'), Shape::other);
    EXPECT_EQ(shape_of(parse(R"(x{1,2}[ab]*)"), 'b'), Shape::other);
    EXPECT_EQ(shape_of(parse(R"([b][ab]*)"), 'b'), Shape::fixed);
    EXPECT_EQ(shape_of(parse(R"(b{1}[ab]*)"), 'b'), Shape::fixed);

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

    const auto text{priced_page(set, priced, file)};

    EXPECT_TRUE(text.contains("delimited: scan the body in a start condition of its own, the opener staying here"));
}

TEST(Report_test, A_terminator_is_read_by_what_it_matches_and_not_by_its_spelling)
{
    // flex scans [a]x{1}, [a]x{1,1} and [a][x]{1} as it scans [a]x, so the shape is the same: the last component
    // matches the one byte and nothing else, however the tree spells that. Each is terminated, and its edit certifies.
    for (const auto terminated :
         {R"([a]x)", R"([a]x{1})", R"([a]x{1,1})", R"([a][x]{1})", R"([a](x|[x]))", R"([a](x{1}){1})"})
    {
        EXPECT_EQ(shape_of(parse(terminated), 'x'), Shape::terminated) << terminated;
    }

    // The tree can carry an empty text beside the byte, which exclude() leaves where a repetition stood.
    const auto with_empty{concat(text(""), text("x"))};

    const auto tree{concat(any_of(Set{'a'}), with_empty)};

    EXPECT_EQ(shape_of(tree, 'x'), Shape::terminated);

    // A last component matching more than the one byte, or another one, is no terminator.
    EXPECT_EQ(shape_of(parse(R"([ab]x{2})"), 'x'), Shape::fixed);
    EXPECT_EQ(shape_of(parse(R"([ab]x{1,2})"), 'x'), Shape::fixed);
    EXPECT_EQ(shape_of(parse(R"([ab]x?)"), 'x'), Shape::other);
    EXPECT_EQ(shape_of(parse(R"([ab](x|y))"), 'x'), Shape::other);
    EXPECT_EQ(shape_of(parse(R"([ab]y{1})"), 'x'), Shape::other);

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

    const auto text{priced_page(set, priced, file)};

    EXPECT_TRUE(text.contains("terminated: leave the terminator to the token after it"));
    EXPECT_FALSE(text.contains("the byte cannot certify while it stays"));
}

TEST(Report_test, An_exact_zero_repetition_is_the_empty_word_whatever_it_repeats)
{
    // re2c 3.1 scans `[ab]([cd]{0}"x")` as it scans `[ab]"x"`, BODY of length two on "ax" and "bx" and X on "x", since
    // a repetition of exactly zero matches the empty word whatever it repeats, so the last component matches the one
    // byte and the shape is terminated, the opener forms delimited. A repetition that may run once is read by its
    // class, so the byte stays fixed in the component.
    for (const auto terminated : {R"([ab]([cd]{0}x))", R"([ab]([cd]{0,0}x))", R"([ab](([cd]{0}){2}x))"})
    {
        EXPECT_EQ(shape_of(parse(terminated), 'x'), Shape::terminated) << terminated;
    }

    for (const auto delimited : {R"(([cd]{0}a)[bx]*)", R"((a[cd]{0})[bx]*)"})
    {
        EXPECT_EQ(shape_of(parse(delimited), 'x'), Shape::delimited) << delimited;
    }

    // The same whether the zero repetition stands in the sequence or in a group of its own, and whatever it repeats,
    // the byte itself included: `[ab][x]{0}"x"` and `[ab]([x]{0}"x")` are both `[ab]"x"` to re2c 3.1, so the shape is
    // terminated either way, the repetition admitting no byte.
    for (const auto same : {R"([ab][x]{0}"x")", R"([ab]([x]{0}"x"))", R"([ab][x]{0,0}x)", R"([ab]([x]{0}){3}x)"})
    {
        EXPECT_EQ(shape_of(parse(same), 'x'), Shape::terminated) << same;
    }

    EXPECT_EQ(shape_of(parse(R"([ab][x]{0,1}"x")"), 'x'), Shape::fixed);
    EXPECT_EQ(shape_of(parse(R"([ab]([cd]{0,1}x))"), 'x'), Shape::fixed);
    EXPECT_EQ(shape_of(parse(R"([ab]([cd]{1}x))"), 'x'), Shape::fixed);

    // And the same at either end of the sequence: re2c 3.1 scans `[ab]"x"[cd]{0}` as `[ab]"x"` and `[cd]{0}"a"[bx]*` as
    // `"a"[bx]*`, the zero repetition being no part of the word, so the last component is the terminator and the first
    // the opener there too. The two spellings of one language are priced alike.
    for (const auto terminated : {R"([ab]"x"[cd]{0})", R"([ab]x[cd]{0}[cd]{0})", R"([cd]{0}[ab]"x"[cd]{0,0})"})
    {
        EXPECT_EQ(shape_of(parse(terminated), 'x'), Shape::terminated) << terminated;
    }

    for (const auto delimited : {R"([cd]{0}"a"[bx]*)", R"([cd]{0}[cd]{0}a[bx]*)", R"([cd]{0,0}a[bx]*[cd]{0})"})
    {
        EXPECT_EQ(shape_of(parse(delimited), 'x'), Shape::delimited) << delimited;
    }

    EXPECT_EQ(shape_of(parse(R"([cd]{0}[ \t\n]+)"), '\n'), Shape::run);

    // What a repetition repeats is normalised too, so a run written through a repetition of exactly one is the run it
    // matches.
    EXPECT_EQ(shape_of(parse(R"(([ \t\n]{1})+)"), '\n'), Shape::run);
    EXPECT_EQ(shape_of(parse(R"((([ \t\n]{1}){1})+)"), '\n'), Shape::run);
    EXPECT_EQ(shape_of(parse(R"(([ \t\n][cd]{0})+)"), '\n'), Shape::run);

    // flex refuses a count of zero, so the scanner is re2c's; the terminated edit certifies x exactly.
    constexpr std::string_view zero{R"(/*!re2c
        [ab]([cd]{0}"x")    { return BODY; }
        "x"                 { return X; }
    */
)"};

    const auto file{read_re2c(zero).front()};

    const auto set{token_set(file, "INITIAL")};

    const auto lexer{compile(set)};

    const auto [token, length]{lexer.tokenize<std::size_t>(std::string_view{"ax"})};

    EXPECT_EQ(length, 2U);

    const auto priced{price(set, 'x')};

    EXPECT_EQ(priced.immovable, (std::vector<std::size_t>{0}));
    ASSERT_EQ(priced.choices.size(), 1U);
    EXPECT_EQ(priced.choices.front().token, 0U);
    EXPECT_EQ(priced.choices.front().shape, Shape::terminated);
    EXPECT_TRUE(priced.choices.front().after.exact);

    const auto text{priced_page(set, priced, file)};

    EXPECT_TRUE(text.contains("terminated: leave the terminator to the token after it"));
    EXPECT_FALSE(text.contains("the byte cannot certify while it stays"));
}

TEST(Report_test, A_consumer_an_edit_exposes_offers_its_own_shape_among_the_choices)
{
    // Narrowing one consumer leaves the byte to a rule whose match it had won, and that rule's own shape is an edit
    // too: the shapes are read from what the edits so far leave, so the second rule's shape is among the choices. Here
    // the first rule wins every match the second would, so the second is blamed for nothing until the first stops
    // admitting the newline.
    const Token_set exposed{.rules = {rule(R"(a[\nx])", 1, 1), rule(R"([a]\n)", 2, 1), rule(R"([ \t]+)", 3, 1)}};

    const auto exposed_price{price(exposed, '\n')};

    EXPECT_TRUE(std::ranges::contains(exposed_price.choices, 2U, &Choice::token));
}

TEST(Report_test, A_repetition_that_may_run_zero_times_loses_the_byte_its_class_spells)
{
    // The class under the star in a[\n]*b holds nothing but the newline, so narrowing the class empties it and the star
    // runs zero times, leaving "ab" matching. The rule is no obstruction, and the price names the edit an author would
    // make.
    EXPECT_TRUE(can_lose(parse(R"(a[\n]*b)"), '\n'));
    EXPECT_TRUE(can_lose(parse(R"(a[\n]?b)"), '\n'));
    EXPECT_TRUE(can_lose(parse(R"(a[\n]{0,3}b)"), '\n'));

    // A repetition that must run at least once leaves the byte unavoidable, as a fixed spelling does.
    EXPECT_FALSE(can_lose(parse(R"(a[\n]+b)"), '\n'));
    EXPECT_FALSE(can_lose(parse(R"(a[\n]b)"), '\n'));

    // The edit performs the exclusion: what is left matches "ab" and no longer a newline between the two.
    auto narrowed{parse(R"(a[\n]*b)")};

    exclude(narrowed, '\n');

    const Token_set only{.rules = {{.regex = narrowed, .id = 0, .priority = 1, .discarded = false}}};

    const auto narrow_lexer{compile(only)};

    const auto [ab_token, ab_length]{narrow_lexer.tokenize<std::size_t>(std::string_view{"ab"})};

    const auto [split_token, split_length]{narrow_lexer.tokenize<std::size_t>(std::string_view{"a\nb"})};

    EXPECT_EQ(ab_length, 2U);
    EXPECT_EQ(split_length, 0U);

    // The price then names a step rather than an immovable rule, and the newline certifies after it.
    const Token_set set{.rules = {rule(R"(\n)", 0, 1), rule(R"(a[\n]*b)", 1, 1)}};

    const auto priced{price(set, '\n')};

    EXPECT_TRUE(priced.immovable.empty());
    ASSERT_EQ(priced.steps.size(), 1U);
    EXPECT_EQ(priced.steps.front().token, 1U);
    EXPECT_TRUE(priced.steps.front().exact);
}

TEST(Report_test, The_report_over_patterns_prices_the_newline_and_the_near_misses)
{
    const auto [file, report]{audited("json.l")};

    // JSON certifies tab, newline and carriage return modulo whitespace; each is priced, the newline first.
    ASSERT_EQ(report.prices.size(), 3U);
    EXPECT_EQ(report.prices[0].byte, '\n');
    EXPECT_EQ(report.prices[1].byte, '\t');
    EXPECT_EQ(report.prices[2].byte, '\r');

    for (const auto& pricing : report.prices)
    {
        ASSERT_EQ(pricing.steps.size(), 1U) << pricing.byte;
        EXPECT_TRUE(pricing.steps[0].exact);
    }

    const auto text{render(report, names(file))};

    EXPECT_TRUE(text.contains(R"(what it would cost to certify '\n')"));
    EXPECT_TRUE(text.contains("becomes a token of its own, discarded"));
    EXPECT_TRUE(text.contains("certifies exactly"));
}

TEST(Report_test, A_json_string_escapes_the_quote_the_backslash_and_the_controls_and_passes_utf8_through)
{
    // The quote and the backslash by their short escapes, every control by its backslash-u form, both ends of that
    // range pinned; 0x7F and a UTF-8 sequence stand as they are, since the text is UTF-8 and a byte string is rendered
    // elsewhere as the code points of its bytes.
    const std::vector<std::pair<std::string_view, std::string>> cases{
            {"", R"("")"},
            {"plain", R"("plain")"},
            {R"(a"b\c)", R"("a\"b\\c")"},
            {"\n\t\r", R"("\u000a\u0009\u000d")"},
            {std::string_view{"\0\x1F", 2}, R"("\u0000\u001f")"},
            {"\x7F", "\"\x7F\""},
            {"\xC3\xA9", "\"\xC3\xA9\""},
            {"\xE2\x82\xAC\xF0\x9F\x98\x80", "\"\xE2\x82\xAC\xF0\x9F\x98\x80\""}};

    for (const auto& [text, expected] : cases)
    {
        EXPECT_EQ(json_string(text), expected) << expected;
    }

    // A byte that is part of no well-formed UTF-8 sequence, a lone continuation byte, a lead byte its file ends inside,
    // an overlong encoding, a surrogate or a code point past U+10FFFF, is escaped as the code point of its value, so a
    // path or a name of any bytes still makes a JSON document.
    const std::vector<std::pair<std::string_view, std::string>> malformed{
            {"\xA9", R"("\u00a9")"},
            {"\xC3", R"("\u00c3")"},
            {"\xC0\x80", R"("\u00c0\u0080")"},
            {"\xED\xA0\x80", R"("\u00ed\u00a0\u0080")"},
            {"\xF4\x90\x80\x80", R"("\u00f4\u0090\u0080\u0080")"},
            {"a\xFF"
             "b",
             R"("a\u00ffb")"}};

    for (const auto& [text, expected] : malformed)
    {
        EXPECT_EQ(json_string(text), expected) << expected;
    }

    // A token's name is text, rendered as such wherever the report names a token, so a name in UTF-8 reads the same
    // under `blame` and `prices` as under the rules, where a byte string would be escaped byte by byte.
    const Token_set accented{.rules = {rule("a+", 0, 0), rule("b", 1, 1)}};

    const auto accented_report{audit(accented)};

    const auto name{[](const std::size_t) { return std::string{"\xC3\x89"}; }};

    const auto document{json(accented_report, name)};

    EXPECT_TRUE(document.contains("\"name\": \"\xC3\x89\""));
    EXPECT_FALSE(document.contains(R"(\u00c3)"));
}
