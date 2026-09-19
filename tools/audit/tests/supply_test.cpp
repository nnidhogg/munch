#include "munch/tools/audit/supply.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/read_flex.hpp"
#include "munch/tools/audit/report.hpp"

using namespace munch::tools::audit;

namespace
{
/**
 * @brief A condition's report with the token set it audits, which the supply scans the input with.
 */
struct Audited
{
    /**
     * @brief The report.
     */
    Report report;

    /**
     * @brief The token set, compiled.
     */
    munch::core::Lexer lexer;
};

/**
 * @brief Audits the INITIAL condition of a flex file given as text.
 * @param source The file's text.
 * @param window_limit The longest window tried.
 * @return The report and the token set.
 */
Audited audited(const std::string_view source, const std::size_t window_limit = 3)
{
    const auto set{token_set(read_flex(std::string{source}).front(), "INITIAL")};

    return {.report = audit(set, window_limit), .lexer = compile(set)};
}

/**
 * @brief Audits the INITIAL condition of one of the grammars beside the tests.
 * @param grammar The grammar's file name.
 * @return The report and the token set.
 */
Audited audited_grammar(const std::string_view grammar)
{
    std::ifstream stream{std::string{SOURCE_DIR} + "/tools/audit/grammars/" + std::string{grammar}};

    return audited(std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()});
}

/**
 * @brief Measures the supply of a condition's certificates on an input.
 * @param audited The report and its token set.
 * @param input The input.
 * @return The supply.
 */
Supply measured(const Audited& audited, const std::string_view input)
{
    return supply(audited.report, audited.lexer, input);
}

/**
 * @brief Words, numbers and newlines, with the blank run discarded: the newline certifies exactly, the blank and the
 *        tab once the run is deleted, and every other byte is consumed mid-token by a word or a number.
 */
constexpr std::string_view words{R"(%option noyywrap nodefault
%%
[a-z]+      return WORD;
[0-9]+      return NUMBER;
\n          return NEWLINE;
[ \t]+      ;
%%
)"};

} // namespace

TEST(Supply, Anchors_are_the_interior_positions_before_a_certified_byte)
{
    // "ab 1\nc d\n" is nine bytes, positions 0 to 8. The newlines stand at 4 and 8, so the exact certificate
    // anchors two positions, 1024 * 2 / 9 = 227.6 per KiB, with the one gap 8 - 4 = 4. The blank and the tab join
    // once the run is deleted: the blanks stand at 2 and 6, so the modulo certificate anchors 2, 4, 6 and 8,
    // 1024 * 4 / 9 = 455.1 per KiB, every gap 2. The grammar certifies windows too: a letter, a digit, a blank or a
    // newline followed by a byte of another kind begins a token there, so every token boundary of the input is a
    // window's origin: 2, 3, 4, 5, 6, 7 and 8, seven anchors, 1024 * 7 / 9 = 796.4 per KiB, every gap 1.
    const auto words_audited{audited(words)};

    ASSERT_EQ(words_audited.report.exact, (std::vector<unsigned char>{'\n'}));
    ASSERT_EQ(words_audited.report.modulo, (std::vector<unsigned char>{'\t', '\n', ' '}));
    ASSERT_FALSE(words_audited.report.windows.empty());

    const auto [bytes, tokenized, exact, modulo, windows]{measured(words_audited, "ab 1\nc d\n")};

    EXPECT_EQ(bytes, 9U);
    EXPECT_EQ(tokenized, std::optional<std::size_t>{9});

    EXPECT_EQ(exact.count, 2U);
    EXPECT_DOUBLE_EQ(exact.per_kibibyte, 1024.0 * 2 / 9);
    ASSERT_TRUE(exact.gaps.has_value());
    EXPECT_EQ(exact.gaps->median, 4U);
    EXPECT_EQ(exact.gaps->ninetieth, 4U);
    EXPECT_EQ(exact.gaps->ninety_ninth, 4U);
    EXPECT_EQ(exact.gaps->longest, 4U);

    EXPECT_EQ(modulo.count, 4U);
    EXPECT_DOUBLE_EQ(modulo.per_kibibyte, 1024.0 * 4 / 9);
    ASSERT_TRUE(modulo.gaps.has_value());
    EXPECT_EQ(modulo.gaps->median, 2U);
    EXPECT_EQ(modulo.gaps->longest, 2U);

    ASSERT_TRUE(windows.has_value());
    EXPECT_EQ(windows->count, 7U);
    EXPECT_DOUBLE_EQ(windows->per_kibibyte, 1024.0 * 7 / 9);
    ASSERT_TRUE(windows->gaps.has_value());
    EXPECT_EQ(windows->gaps->median, 1U);
    EXPECT_EQ(windows->gaps->ninety_ninth, 1U);
    EXPECT_EQ(windows->gaps->longest, 1U);
}

TEST(Supply, The_section_and_the_object_carry_the_same_figures)
{
    const auto lines{measured(audited(words), "ab 1\nc d\n")};

    EXPECT_EQ(
            supply_section(lines, "lines.txt"),
            "\ncertified-anchor supply on lines.txt, 9 bytes\n"
            "  serial scan                tokenizes the input completely, so every anchor is a token boundary, the "
            "modulo row's once discarded tokens are deleted\n"
            "  exact bytes                2 anchors, 227.6 per KiB, gaps p50 4, p90 4, p99 4, max 4\n"
            "  modulo discarded bytes     4 anchors, 455.1 per KiB, gaps p50 2, p90 2, p99 2, max 2\n"
            "  exact bytes and windows    7 anchors, 796.4 per KiB, gaps p50 1, p90 1, p99 1, max 1\n");

    EXPECT_EQ(
            supply_json(lines, "lines.txt"), R"({"input": "lines.txt", "bytes": 9, "tokenized": 9, )"
                                             R"("exact": {"anchors": 2, "per_kibibyte": 227.6, )"
                                             R"("gap_p50": 4, "gap_p90": 4, "gap_p99": 4, "gap_max": 4}, )"
                                             R"("modulo": {"anchors": 4, "per_kibibyte": 455.1, )"
                                             R"("gap_p50": 2, "gap_p90": 2, "gap_p99": 2, "gap_max": 2}, )"
                                             R"("windows": {"anchors": 7, "per_kibibyte": 796.4, )"
                                             R"("gap_p50": 1, "gap_p90": 1, "gap_p99": 1, "gap_max": 1}})");
}

TEST(Supply, Percentiles_are_the_order_statistic_at_the_floor_of_q_n)
{
    // Five words of lengths 1, 2, 3, 4 and 5 on their own lines: the newlines stand at 1, 4, 8, 13 and 19, so the
    // four gaps ascending are 3, 4, 5 and 6. The paper's percentile is the order statistic at floor(q n) capped at
    // the last: floor(0.5 * 4) = 2 gives 5, floor(0.9 * 4) = 3 gives 6, floor(0.99 * 4) = 3 gives 6, the maximum 6.
    const auto [bytes, tokenized, exact, modulo, windows]{measured(audited(words), "a\nbb\nccc\ndddd\neeeee\n")};

    EXPECT_EQ(bytes, 20U);
    EXPECT_EQ(exact.count, 5U);
    ASSERT_TRUE(exact.gaps.has_value());
    EXPECT_EQ(exact.gaps->median, 5U);
    EXPECT_EQ(exact.gaps->ninetieth, 6U);
    EXPECT_EQ(exact.gaps->ninety_ninth, 6U);
    EXPECT_EQ(exact.gaps->longest, 6U);
}

TEST(Supply, The_ends_of_the_input_are_no_anchors_and_fewer_than_two_leave_no_gap)
{
    // A newline first and last in "\na\n": position 0 is no cut, so only the newline at 2 counts, one anchor,
    // 1024 / 3 = 341.3 per KiB, and one anchor has no gap to a next. On the block-comment row nothing certifies
    // and no window is found, so every inventory is empty and the windows row is absent; the section says so in
    // words and the object in nulls.
    const auto [bytes, tokenized, exact, modulo, windows]{measured(audited(words), "\na\n")};

    EXPECT_EQ(bytes, 3U);
    EXPECT_EQ(exact.count, 1U);
    EXPECT_DOUBLE_EQ(exact.per_kibibyte, 1024.0 / 3);
    EXPECT_FALSE(exact.gaps.has_value());

    const auto none{measured(audited_grammar("c-like-block-comments.l"), "int x; /* a\n comment */\n")};

    EXPECT_EQ(none.exact.count, 0U);
    EXPECT_EQ(none.modulo.count, 0U);
    EXPECT_FALSE(none.windows.has_value());

    EXPECT_EQ(
            supply_section(none, "x.c"),
            "\ncertified-anchor supply on x.c, 24 bytes\n"
            "  serial scan                tokenizes the input completely, so every anchor is a token boundary, the "
            "modulo row's once discarded tokens are deleted\n"
            "  exact bytes                0 anchors, 0.0 per KiB, no gaps, fewer than two anchors\n"
            "  modulo discarded bytes     0 anchors, 0.0 per KiB, no gaps, fewer than two anchors\n");

    EXPECT_EQ(
            supply_json(none, "x.c"),
            R"({"input": "x.c", "bytes": 24, "tokenized": 24, )"
            R"("exact": {"anchors": 0, "per_kibibyte": 0.0, "gap_p50": null, "gap_p90": null, "gap_p99": null, )"
            R"("gap_max": null}, )"
            R"("modulo": {"anchors": 0, "per_kibibyte": 0.0, "gap_p50": null, "gap_p90": null, "gap_p99": null, )"
            R"("gap_max": null}, "windows": null})");
}

TEST(Supply, An_input_the_scan_stops_short_of_is_counted_and_said_to_promise_no_boundary)
{
    // Over "0", "00" and "01" the window "01" is certified at origin 0 and "001" at origin 1, each occurring in the
    // completely tokenizable input "0001". On "001" both place an anchor at position 1, but flex 2.6.4 consumes "00"
    // there and jams on the "1": the certificates promise a boundary on input the scan tokenizes completely, which
    // this is not, so the supply counts the one anchor as it stands and says the scan tokenized two bytes.
    constexpr std::string_view binary{R"(%option noyywrap nodefault
%%
"0"         return ZERO;
"00"        return DOUBLE;
"01"        return PAIR;
%%
)"};

    const auto [bytes, tokenized, exact, modulo, windows]{measured(audited(binary), "001")};

    EXPECT_EQ(bytes, 3U);
    EXPECT_EQ(tokenized, std::optional<std::size_t>{2});
    EXPECT_EQ(exact.count, 0U);
    ASSERT_TRUE(windows.has_value());
    EXPECT_EQ(windows->count, 1U);

    const auto malformed{measured(audited(binary), "001")};

    EXPECT_EQ(
            supply_section(malformed, "in.txt"),
            "\ncertified-anchor supply on in.txt, 3 bytes\n"
            "  serial scan                stops at offset 2, so the rows count occurrences and promise no boundary, "
            "the exact byte row alone keeping tokenize_all_parallel()'s serial-prefix relation, which the window "
            "rows have not got\n"
            "  exact bytes                0 anchors, 0.0 per KiB, no gaps, fewer than two anchors\n"
            "  modulo discarded bytes     0 anchors, 0.0 per KiB, no gaps, fewer than two anchors\n"
            "  exact bytes and windows    1 anchor, 341.3 per KiB, no gaps, fewer than two anchors\n");
    EXPECT_EQ(supply_json(malformed, "in.txt").substr(0, 48), R"({"input": "in.txt", "bytes": 3, "tokenized": 2, )");

    EXPECT_EQ(measured(audited(binary), "0001").tokenized, std::optional<std::size_t>{4});

    // Over the report alone the input is not scanned, the paper's own measurement, and the section has no row for it.
    const auto raw{supply(audited(binary).report, "001")};

    EXPECT_FALSE(raw.tokenized.has_value());
    EXPECT_EQ(raw.windows->count, 1U);
    EXPECT_EQ(
            supply_section(raw, "in.txt").substr(0, 82),
            "\ncertified-anchor supply on in.txt, 3 bytes\n  exact bytes                0 anchors");
    EXPECT_EQ(supply_json(raw, "in.txt").substr(0, 51), R"({"input": "in.txt", "bytes": 3, "tokenized": null, )");
}

TEST(Supply, A_window_anchors_every_input_its_class_string_stands_for)
{
    // The conventional row certifies "\n!" at 1, the newline followed by a byte that must begin a token, and the
    // report spells the window with the operator class's representative. In "ab\n+\n" the newline at 2 is followed
    // by a plus, another byte of that class, so the window anchors 3; "b\n" anchors 2 and "+\n" anchors 4, the
    // newline beginning a token after a byte no whitespace run holds. No byte certifies exactly, the newline does
    // once the run is deleted, at 2 and 4, and the windows anchor 2, 3 and 4, 1024 * 3 / 5 = 614.4 per KiB.
    const auto [bytes, tokenized, exact, modulo, windows]{
            measured(audited_grammar("c-like-conventional.l"), "ab\n+\n")};

    EXPECT_EQ(bytes, 5U);
    EXPECT_EQ(exact.count, 0U);
    EXPECT_EQ(modulo.count, 2U);
    ASSERT_TRUE(modulo.gaps.has_value());
    EXPECT_EQ(modulo.gaps->median, 2U);

    ASSERT_TRUE(windows.has_value());
    EXPECT_EQ(windows->count, 3U);
    EXPECT_DOUBLE_EQ(windows->per_kibibyte, 614.4);
    ASSERT_TRUE(windows->gaps.has_value());
    EXPECT_EQ(windows->gaps->median, 1U);
    EXPECT_EQ(windows->gaps->longest, 1U);
}

TEST(Supply, The_figures_are_the_certified_splitting_papers_on_a_shared_corpus)
{
    // The paper's supply computation, splitting_measurements.py, run on a vocabulary of nine literal tokens and a
    // 76-byte corpus fed to both: its interior-byte lemma names the certified bytes, every window of width two to
    // four occurring in the corpus is decided at every origin by its decider, anchor_positions() counts the distinct
    // interior anchors, and its percentile() takes the order statistic at floor(q n). It printed:
    //
    //   certified bytes: 3 of 6; the interior ones: 'abc'
    //   window pairs: 116 certified over 132 occurring windows
    //   corpus bytes: 76
    //   bytes | anchors: 28
    //   bytes | anchors per KiB: 377.3
    //   bytes | anchor gap p50: 1
    //   bytes | anchor gap p90: 7
    //   bytes | anchor gap p99: 9
    //   bytes | anchor gap max: 9
    //   bytes and windows | anchors: 51
    //   bytes and windows | anchors per KiB: 687.2
    //   bytes and windows | anchor gap p50: 1
    //   bytes and windows | anchor gap p90: 2
    //   bytes and windows | anchor gap p99: 2
    //   bytes and windows | anchor gap max: 2
    //
    // The vocabulary is prefix-free, no token a proper prefix of another, so that the report's windows, the
    // conservative model's, are the paper's decider's as well; the test after this one shows where they part. The
    // windows are enumerated over byte classes through width four, the paper's budget, and expanded over the corpus.
    constexpr std::string_view vocabulary{R"(%option noyywrap nodefault
%%
"ab"        return AB;
"ba"        return BA;
"ac"        return AC;
"ca"        return CA;
"bc"        return BC;
"cb"        return CB;
"d"         return D;
\n          return NEWLINE;
" "         return BLANK;
%%
)"};

    constexpr std::string_view corpus{
            "ab ba d\n"
            "abbaacca cb bc d ab\n"
            "d abab baba d cbaccb\n"
            "ac d bccacb\n"
            "\n"
            "d d ab abbaac\n"};

    ASSERT_EQ(corpus.size(), 76U);

    const auto [report, lexer]{audited(vocabulary, 4)};

    EXPECT_EQ(report.exact, (std::vector<unsigned char>{'\n', ' ', 'd'}));
    EXPECT_EQ(report.modulo, report.exact);

    const auto corpus_supply{supply(report, lexer, corpus)};

    EXPECT_EQ(
            supply_section(corpus_supply, "corpus.txt"),
            "\ncertified-anchor supply on corpus.txt, 76 bytes\n"
            "  serial scan                tokenizes the input completely, so every anchor is a token boundary, the "
            "modulo row's once discarded tokens are deleted\n"
            "  exact bytes                28 anchors, 377.3 per KiB, gaps p50 1, p90 7, p99 9, max 9\n"
            "  modulo discarded bytes     28 anchors, 377.3 per KiB, gaps p50 1, p90 7, p99 9, max 9\n"
            "  exact bytes and windows    51 anchors, 687.2 per KiB, gaps p50 1, p90 2, p99 2, max 2\n");
}

TEST(Supply, The_bytes_agree_with_the_paper_on_a_byte_fallback_vocabulary_and_the_windows_are_the_reports_own)
{
    // The same program on a vocabulary of the paper's own shape, seven merged tokens over a byte fallback, and a
    // 164-byte corpus. It printed:
    //
    //   certified bytes: 249 of 256; the interior ones: ' bcdfyz'
    //   window pairs: 249 certified over 265 occurring windows
    //   corpus bytes: 164
    //   bytes | anchors: 46
    //   bytes | anchors per KiB: 287.2
    //   bytes | anchor gap p50: 2
    //   bytes | anchor gap p90: 7
    //   bytes | anchor gap p99: 19
    //   bytes | anchor gap max: 19
    //   bytes and windows | anchors: 111
    //   bytes and windows | anchors per KiB: 693.1
    //   bytes and windows | anchor gap p50: 1
    //   bytes and windows | anchor gap p90: 3
    //   bytes and windows | anchor gap p99: 3
    //   bytes and windows | anchor gap max: 3
    //
    // The byte rows are the paper's. The windows row is the report's own: its windows are the conservative model's,
    // which lets a token end wherever a state accepts, so over a fallback vocabulary, where every token has an
    // accepted proper prefix, it refuses "bcd" at 2, which the paper's decider certifies knowing the longest match
    // takes "bc", and the report's inventory anchors 107 positions where the paper's anchors 111. The merges are
    // literals and one rule takes any other byte, since the certificates are about boundaries and not about which
    // single-byte token a byte is.
    constexpr std::string_view vocabulary{R"(%option noyywrap nodefault
%%
"ab"        return AB;
"abc"       return ABC;
"bc"        return BC;
"cd"        return CD;
"e f"       return E_F;
"xyz"       return XYZ;
"yz"        return YZ;
.|\n        return BYTE;
%%
)"};

    constexpr std::string_view corpus{
            "abc cd e f xyz abcd bcd yz ab ef xy z a b c d\n"
            "xabc yz e fabc e  f cd bc abc ab\n"
            "the cab sat on the mat; abcabc bcbc cdcd yzyz e f e f\n"
            "zebra xyz yz bcd abcd abc ab a\n"};

    ASSERT_EQ(corpus.size(), 164U);

    const auto [report, lexer]{audited(vocabulary, 4)};

    EXPECT_EQ(report.exact.size(), 249U);
    EXPECT_EQ(report.modulo, report.exact);
    EXPECT_FALSE(report.windows.empty());

    const auto corpus_supply{supply(report, lexer, corpus)};

    EXPECT_EQ(
            supply_section(corpus_supply, "corpus.txt"),
            "\ncertified-anchor supply on corpus.txt, 164 bytes\n"
            "  serial scan                tokenizes the input completely, so every anchor is a token boundary, the "
            "modulo row's once discarded tokens are deleted\n"
            "  exact bytes                46 anchors, 287.2 per KiB, gaps p50 2, p90 7, p99 19, max 19\n"
            "  modulo discarded bytes     46 anchors, 287.2 per KiB, gaps p50 2, p90 7, p99 19, max 19\n"
            "  exact bytes and windows    107 anchors, 668.1 per KiB, gaps p50 1, p90 3, p99 4, max 4\n");
}
