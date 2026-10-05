/*
 * Reproduces the applicability table of the report by compiling each token set and reading the shipped predicate for
 * all 256 byte values. No hand analysis is involved: the useful set is exactly what is_split_point() reports.
 *
 * Build and run from the repository root against an existing build tree:
 *   c++ -std=c++23 -I libs/common/include -I libs/core/include -I libs/dfa/include -I libs/nfa/include \
 *       -I libs/regex/include -I tools/benchmark/include -I build/generated \
 *       paper/figures/applicability.cpp tools/benchmark/src/harness.cpp -o /tmp/applicability \
 *       -L build/libs/core -L build/libs/dfa -L build/libs/nfa -L build/libs/regex \
 *       -lmunch_core -lmunch_dfa -lmunch_nfa -lmunch_regex -lpthread
 *   /tmp/applicability
 */

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <iostream>
#include <iterator>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/benchmark/harness.hpp"
#include "random.hpp"

namespace
{
using namespace figures;

/**
 * @brief The distinct candidate bytes the report states the oracle tries, sixteen.
 */
constexpr std::size_t oracle_candidate_total{16};

/**
 * @brief The documents drawn for each of the oracle's corpora, beside every piece doubled.
 */
constexpr std::size_t oracle_documents{200};

/**
 * @brief The rows the report states the relaxation moves, eight.
 */
constexpr int moved_rows{8};

/**
 * @brief The fewest pieces in a drawn oracle document.
 */
constexpr unsigned fewest_pieces{4};

/**
 * @brief How many piece counts a drawn oracle document's length is drawn from, so a document holds four to thirteen
 *        pieces.
 */
constexpr unsigned piece_spread{10};

/**
 * @brief The ASCII record separator, 0x1E, the byte the framing row reserves as a separator.
 */
constexpr char record_separator{'\x1E'};

/**
 * @brief The brute-force oracle's answer over a row's candidates.
 */
struct Oracle_verdict
{
    /**
     * @brief The surviving candidates separated by spaces, `none` when none survives.
     */
    std::string surviving{};

    /**
     * @brief The candidates no document placed, escaped and separated by spaces; a candidate the corpus never placed
     *        inside a token is reported rather than skipped, since it would otherwise be indistinguishable from one the
     *        condition genuinely rejects.
     */
    std::string unexercised{};
};

/**
 * @brief Counts how many of a benchmark row's candidate bytes certify.
 *
 * The two benchmark rows publish their cells as "N of its own M" rather than as a byte list, and a miscounted M is
 * exactly the kind of error the byte-list assertion cannot see, so those rows assert both numbers: M is the distinct
 * operator and punctuation bytes the grammar registers, and N is how many of them certify.
 * @param lexer The row's lexer.
 * @param candidates The candidate bytes.
 * @return The candidates that are split points, each occurrence counted.
 */
std::size_t certified_among(const munch::core::Lexer& lexer, const std::string& candidates)
{
    const auto certifies{[&lexer](const char candidate) { return lexer.is_split_point(candidate); }};

    const auto count{std::ranges::count_if(candidates, certifies)};

    return static_cast<std::size_t>(count);
}

/**
 * @brief Renders the useful certified set the way the table's second column reads.
 * @param lexer The row's lexer.
 * @return `none`, `all operator and punctuation bytes`, or the certified bytes separated by spaces, newline as `\n`.
 */
std::string certified(const munch::core::Lexer& lexer)
{
    const auto certifies{[&lexer](const int value) { return lexer.is_split_point(static_cast<char>(value)); }};

    std::vector<int> bytes{};

    std::ranges::copy_if(std::views::iota(0, byte_values), std::back_inserter(bytes), certifies);

    if (bytes.empty())
    {
        return "none";
    }

    const Set combined{operators() + punctuation()};

    const auto& expected{combined.symbols()};

    const auto is_expected{[&expected](const int byte) { return expected.contains(static_cast<char>(byte)); }};

    if (bytes.size() == expected.size() && std::ranges::all_of(bytes, is_expected))
    {
        return "all operator and punctuation bytes";
    }

    std::vector<std::string> parts{};

    for (const auto byte : bytes)
    {
        parts.push_back(byte == '\n' ? R"(\n)" : std::string{static_cast<char>(byte)});
    }

    return joined(parts, " ");
}

/**
 * @brief Renders a byte as the relaxed renderings print it: newline, tab and carriage return as their escapes, every
 *        other byte as itself.
 * @param byte The byte.
 * @return The rendering.
 */
std::string shown(const char byte)
{
    switch (byte)
    {
    case '\n':
        return R"(\n)";
    case '\t':
        return R"(\t)";
    case '\r':
        return R"(\r)";
    default:
        return {byte};
    }
}

/**
 * @brief Renders a byte as the relaxed byte lists print it: space as `SP`, every other byte as shown() renders it.
 * @param byte The byte.
 * @return The rendering.
 */
std::string relaxed_byte(const char byte)
{
    return byte == ' ' ? "SP" : shown(byte);
}

/**
 * @brief Joins rendered bytes with spaces, as the relaxed column and the oracle list them.
 * @param parts The rendered bytes, in order.
 * @return The joined bytes, `none` when there are none.
 */
std::string listed(const std::vector<std::string>& parts)
{
    const auto rendered{joined(parts, " ")};

    return rendered.empty() ? "none" : rendered;
}

/**
 * @brief Lists the relaxed set as bytes, space as `SP`.
 *
 * The table renderer says "the same" relationally, which reads well in a cell but cannot be compared against the
 * brute-force oracle's output; this is what the cross-check uses.
 * @param lexer The row's lexer, its ignored tokens set.
 * @return The bytes separated by spaces, `none` when there are none.
 */
std::string certified_modulo_bytes(const munch::core::Lexer& lexer)
{
    std::vector<std::string> parts{};

    for (int value{0}; value < byte_values; ++value)
    {
        const auto byte{static_cast<char>(value)};

        if (!lexer.is_split_point_ignoring(byte))
        {
            continue;
        }

        parts.push_back(relaxed_byte(byte));
    }

    return listed(parts);
}

/**
 * @brief Renders the relaxed column of the table.
 *
 * Where the relaxed set is the exact one plus the whitespace bytes, it says so relationally rather than repeating a
 * long byte list, so the printed cell and the asserted string are the same text and the table cannot drift from the
 * assertion.
 * @param lexer The row's lexer, its ignored tokens set.
 * @return `the same`, `the same, plus space, tab and newline`, or the relaxed bytes as certified_modulo_bytes() lists
 *         them.
 */
std::string certified_modulo(const munch::core::Lexer& lexer)
{
    std::set<char> exact{};

    std::set<char> relaxed{};

    for (int value{0}; value < byte_values; ++value)
    {
        const auto byte{static_cast<char>(value)};

        if (lexer.is_split_point(byte))
        {
            exact.insert(byte);
        }

        if (lexer.is_split_point_ignoring(byte))
        {
            relaxed.insert(byte);
        }
    }

    if (relaxed == exact)
    {
        return "the same";
    }

    auto with_whitespace{exact};

    with_whitespace.insert({' ', '\t', '\n'});

    if (relaxed == with_whitespace)
    {
        return "the same, plus space, tab and newline";
    }

    std::vector<std::string> parts{};

    for (const auto byte : relaxed)
    {
        parts.push_back(relaxed_byte(byte));
    }

    return listed(parts);
}

/**
 * @brief Expands the templates into the pieces a corpus is drawn from: every template without an `@` hole as it is, and
 *        every template with one once per filler byte, the filler substituted into every hole.
 * @param templates The templates.
 * @param fillers The bytes substituted.
 * @return The pieces, in template order and within one template in filler order.
 */
std::vector<std::string> expand(const std::vector<std::string>& templates, const std::string& fillers)
{
    std::vector<std::string> pieces{};

    for (const auto& form : templates)
    {
        if (!form.contains('@'))
        {
            pieces.push_back(form);

            continue;
        }

        for (const auto filler : fillers)
        {
            auto piece{form};

            std::ranges::replace(piece, '@', filler);

            pieces.push_back(piece);
        }
    }

    return pieces;
}

/**
 * @brief Draws a deterministic corpus, so a disagreement it reports can be reproduced exactly: every piece doubled as a
 *        document of its own, so no candidate is left unexercised by random selection, which would read exactly like a
 *        candidate the condition rejects, then documents of four to thirteen pieces drawn from a fixed 32-bit stream.
 * @param pieces The pieces.
 * @param count The drawn documents.
 * @return The documents.
 */
std::vector<std::string> documents(const std::vector<std::string>& pieces, const std::size_t count)
{
    std::vector<std::string> corpus{};

    for (const auto& piece : pieces)
    {
        corpus.push_back(piece + piece);
    }

    const auto drawn_documents{drawn(pieces, count, fewest_pieces, piece_spread)};

    std::ranges::copy(drawn_documents, std::back_inserter(corpus));

    return corpus;
}

/**
 * @brief Returns the bytes worth asking about, sixteen in all: the four whitespace bytes, eleven operator and
 *        punctuation bytes a C-like or JSON row uses, and the letter t.
 * @return The candidates.
 */
const std::string& oracle_candidates()
{
    static const std::string candidates{" \t\n\r+;(),:[]{}-t"};

    return candidates;
}

/**
 * @brief Runs the brute-force oracle for the relaxed column.
 *
 * The report claims the condition is not merely sound on these token sets but agrees with splitting on every candidate
 * byte, so the claim is checked by splitting rather than by trusting the rule, and every declared candidate must also
 * be exercised, or the agreement would be vacuous. The corpus substitutes every candidate byte into every container
 * template, since sampling reports a byte as safe whenever no input happens to place it inside a string or a comment,
 * which is indistinguishable from the byte genuinely being safe.
 *
 * Every document is split at every occurrence of every candidate byte, the cut before the first byte exempted, and the
 * set that survives modulo the ignored kinds is rendered the way certified_modulo() renders its verdict. The guarantee
 * is stated only for input that tokenizes completely, so a document that does not is skipped.
 * @param lexer The row's lexer.
 * @param ignored The ignored kinds.
 * @param corpus The documents.
 * @return The surviving candidates and the candidates no document placed.
 */
Oracle_verdict surviving_modulo(
        const munch::core::Lexer& lexer, const Kinds_t& ignored, const std::vector<std::string>& corpus)
{
    const auto verdict_of{[&](const char candidate) {
        auto exercised{false};

        auto survives{true};

        for (const auto& text : corpus)
        {
            const auto [serial, consumed]{scan(lexer, text)};

            if (consumed != text.size())
            {
                continue;
            }

            for (std::size_t at{1}; at < text.size(); ++at)
            {
                if (text[at] != candidate)
                {
                    continue;
                }

                exercised = true;

                survives = cut_survives(lexer, ignored, text, serial, at) && survives;
            }
        }

        return std::pair{exercised, survives};
    }};

    std::vector<std::string> surviving{};

    std::vector<std::string> unexercised{};

    for (const auto candidate : oracle_candidates())
    {
        const auto [exercised, survives]{verdict_of(candidate)};

        if (!exercised)
        {
            unexercised.push_back(shown(candidate));

            continue;
        }

        if (!survives)
        {
            continue;
        }

        surviving.push_back(shown(candidate));
    }

    const auto surviving_text{listed(surviving)};

    const auto unexercised_text{joined(unexercised, " ")};

    return {.surviving = surviving_text, .unexercised = unexercised_text};
}

/**
 * @brief Returns whether two lexers certify the same bytes and emit the same token lengths over a corpus, printing the
 *        first difference.
 *
 * Certified sets alone would miss a change that moves a token boundary without moving a certificate, so the emitted
 * lengths are compared too; kinds cannot be compared directly, the two enumerations differ.
 * @param copy The transcribed grammar's lexer.
 * @param original The benchmark's own lexer.
 * @param corpus The corpus.
 * @return True when both agree.
 */
bool same_scanner(const munch::core::Lexer& copy, const munch::core::Lexer& original, const std::string& corpus)
{
    const auto differs{[&copy, &original](const int byte) {
        const auto symbol{static_cast<char>(byte)};

        return copy.is_split_point(symbol) != original.is_split_point(symbol);
    }};

    const auto bytes{std::views::iota(0, byte_values)};

    if (const auto first{std::ranges::find_if(bytes, differs)}; first != bytes.end())
    {
        std::cout << "         certified sets differ at byte " << *first << '\n';

        return false;
    }

    const auto [copy_stream, copy_consumed]{scan(copy, corpus)};

    const auto [original_stream, original_consumed]{scan(original, corpus)};

    const auto same_lengths{std::ranges::equal(copy_stream | std::views::values, original_stream | std::views::values)};

    if (copy_consumed != original_consumed || !same_lengths)
    {
        std::cout << "         token streams differ: consumed " << copy_consumed << " against " << original_consumed
                  << '\n';

        return false;
    }

    return true;
}

/**
 * @brief Binds the scaling row's transcription to its original.
 *
 * The benchmark rows restate grammars that live in the benchmark tool, and a copy that agrees today can drift silently
 * while every assertion here still passes, because the assertions compare the copy with itself. harness.cpp compiles
 * standalone, so the real build_lexer() is built here and compared against grammars.hpp's transcription of it;
 * agreement on all 256 certified bits and on the exact token lengths of a corpus is far stronger than reading the two
 * side by side.
 * @return True when the transcription matches build_lexer(false).
 */
bool scaling_grammar_matches_the_benchmark()
{
    munch::core::Builder transcribed{};

    scaling_grammar(transcribed);

    const auto copy{transcribed.build()};

    const auto original{munch::tools::benchmark::build_lexer(false)};

    const std::string corpus{
            "while (counter <= 4711) { x1 = bar_baz + 97; if (x1 != 42) { return counter; } }\n\tint value2 = 0;\n"};

    return same_scanner(copy, original, corpus);
}

/**
 * @brief Binds the construction-cost row's transcription the same way: keyword_scale_tokens() is the grammar the
 *        benchmark compiles, linked from the harness, so the published 16-of-24 cell cannot drift from it undetected.
 * @return True when the transcription matches keyword_scale_tokens().
 */
bool keyword_scale_grammar_matches_the_benchmark()
{
    munch::core::Builder transcribed{};

    keyword_scale_grammar(transcribed);

    const auto copy{transcribed.build()};

    munch::core::Builder linked{};

    munch::tools::benchmark::keyword_scale_tokens(linked);

    const auto original{linked.build()};

    const std::string corpus{
            "while (alignas != 4711) { co_await counter++; } constexpr double x2 = -3.5e7 % rate;\n\tstatic_cast\n"};

    return same_scanner(copy, original, corpus);
}

/**
 * @brief Checks the table's rows against what the shipped predicate answers, printing each row's verdict, and counts
 *        the rows and checks that disagree and the rows whose relaxed column moves.
 */
class Table_checker
{
public:
    /**
     * @brief Checks the C-like base row, the three rows adding exactly one token kind to that same base, so each
     *        collapse is attributable to the token kind named rather than to an accumulation of them, and the JSON and
     *        log-line rows.
     */
    void base_rows();

    /**
     * @brief Checks the tokenization rows.
     *
     * The first two recognize exactly the same byte language and differ only in how it is cut into tokens: the
     * conventional one folds newline into the whitespace run, the split-friendly one gives newline its own token and
     * leaves spaces and tabs as a run. That is the pair the text needs to claim certification depends on the
     * tokenization rather than on the recognized language. The third is deliberately cumulative, the split-friendly
     * grammar with block comments added, so the pair is a true before/after on the one token kind that spans lines.
     *
     * The fourth is the priced-failure counterpart to the Zig rows: the same three kinds, kept, with every body barred
     * from the bytes that open another. It exists to price the obvious repair, and the price buys nothing: no useful
     * byte certifies exactly or modulo, while the language has lost the slash and the quote inside strings and
     * comments. The fifth is the cheap success, and the pair's point: the ordinary repertoire, nothing separated, with
     * one restriction, that the block comment may not cross a line, on the split-friendly base where newline is already
     * its own token. Newline certifies outright, so the lever is line-boundedness and not separation, and the price of
     * a certificate is multi-line comments, which is the choice Zig's reference states and a far smaller one than the
     * fourth row pays for nothing.
     */
    void tokenization_rows();

    /**
     * @brief Checks the designed-success pair: a shipped language whose reference states the property the
     *        split-friendly row constructs.
     *
     * As the subset reads them, one line at a time where the 0.16.0 grammar appendix groups a multiline string or a doc
     * comment over consecutive lines into one token, Zig's strings, comments and char literals all end at the line, so
     * newline is recovered modulo the discarded tokens in the conventional tokenization and certified outright once it
     * is its own token, with the appendix's string, comment and char-literal bodies present; in its byte classes the
     * subset follows the appendix, not the language of conforming source, whose encoding rules forbid bytes the
     * appendix's line bodies admit. Tab and carriage return occur in no rule of that appendix and so in no token of the
     * subset adapted from it, so they are certified vacuously and withheld; space stays inside the strings and
     * comments.
     */
    void zig_rows();

    /**
     * @brief Checks the two benchmark grammars' rows, each with its "N of its own M" cell; the candidates are every
     *        distinct byte occurring in an operator or punctuation literal the grammar registers.
     */
    void benchmark_rows();

    /**
     * @brief Asserts the number of candidate bytes the oracle tries, which the report states as sixteen.
     */
    void oracle_candidate_count();

    /**
     * @brief Checks the four rows the report cross-checks by splitting.
     *
     * That is weaker than exactness on these token sets, since only the declared candidates are tried, and the report
     * says so. Containers carrying an `@` hole have every candidate byte substituted into them, so no byte is judged
     * safe merely because the corpus never placed it inside a token; the block-comment row is cumulative on the
     * split-friendly row, exactly as the exact-column row of the same name is.
     */
    void oracle_rows();

    /**
     * @brief Checks the framing construction: a byte no token admits in its interior, given a rule of its own, is
     *        certified outright by the shipped predicate even for the token set that certifies nothing.
     *
     * 0x1E is the ASCII record separator, reserved for exactly this. Prints the row's verdict.
     */
    void framing_row();

    /**
     * @brief Checks the certificate section's worked instance: over RFC 3629's encoding forms, the useful certified
     *        bytes are exactly the lead bytes and no continuation byte, which is the property the RFC lists among
     *        UTF-8's characteristics, asserted byte for byte over all 256 rather than read off a printed list.
     *
     * Prints the row's verdict.
     */
    void utf8_row();

    /**
     * @brief Asserts how many rows the relaxation moves, which the report states as eight, counted from the rows rather
     *        than from the prose.
     */
    void relaxation_count();

    /**
     * @brief Binds both transcribed benchmark grammars to the grammars the benchmark compiles.
     *
     * Prints both verdicts.
     */
    void benchmark_bindings();

    /**
     * @brief Returns how many rows and checks disagreed with the table.
     * @return The disagreements counted.
     */
    [[nodiscard]] int failures() const noexcept;

private:
    /**
     * @brief Checks one row's two published columns: what the exact certificate admits, and what it admits once the
     *        caller's discarded tokens are deleted, read from the shipped predicate, which needs no corpus.
     *
     * Prints the row's verdict and counts a row whose relaxed column moves.
     * @param name The row's name.
     * @param exact The exact cell.
     * @param relaxed The relaxed cell.
     * @param ignored The discarded kinds.
     * @param builder The row's grammar.
     */
    void check(
            const std::string& name, const std::string& exact, const std::string& relaxed, const Kinds_t& ignored,
            munch::core::Builder& builder);

    /**
     * @brief Prints a row's or a check's verdict line, counting a disagreement; the caller prints any detail after it.
     * @param agrees Whether it agrees with the table.
     * @param line The text after the verdict label.
     */
    void record(bool agrees, std::string_view line);

    /**
     * @brief Checks a benchmark row's "N of its own M" cell: the distinct candidate bytes are M and the certified ones
     *        N.
     *
     * Prints the row's verdict.
     * @param name The row's name.
     * @param candidates The candidate bytes, written out rather than derived, so a wrong count in the paper cannot
     *        match a wrong count here by construction.
     * @param total The published M.
     * @param expected The published N.
     * @param builder The row's grammar.
     */
    void ratio(
            const std::string& name, const std::string& candidates, const std::size_t total, const std::size_t expected,
            munch::core::Builder& builder);

    /**
     * @brief Checks one of the rows the report cross-checks by splitting as well as by reading the predicate: what the
     *        shipped predicate certifies, what the relaxed condition certifies, and that splitting survives at exactly
     *        the relaxed set over every declared candidate byte, every candidate exercised.
     *
     * Prints the row's verdict.
     * @param name The row's name.
     * @param exact The exact cell.
     * @param relaxed The relaxed cell, as certified_modulo_bytes() lists it.
     * @param ignored The discarded kinds.
     * @param corpus The documents split.
     * @param builder The row's grammar.
     */
    void modulo(
            const std::string& name, const std::string& exact, const std::string& relaxed, const Kinds_t& ignored,
            const std::vector<std::string>& corpus, munch::core::Builder& builder);

    /**
     * @brief The rows and checks disagreeing with the table.
     */
    int failures_{0};

    /**
     * @brief The rows whose relaxed column differs from their exact one.
     */
    int changed_{0};
};

void Table_checker::base_rows()
{
    munch::core::Builder base{};

    c_like(base, false);

    check("C-like: identifiers, numbers, ws runs, operators, punct", "all operator and punctuation bytes",
          "the same, plus space, tab and newline", ignoring({Token::whitespace}), base);

    munch::core::Builder strings{};

    c_like(strings, false);

    strings.add_token(string_literal(), Token::string, 2);

    check("the first row plus strings, alone (no raw newline inside)", "none", R"(\n)", ignoring({Token::whitespace}),
          strings);

    munch::core::Builder line_comments{};

    c_like(line_comments, false);

    line_comments.add_token(line_comment(), Token::line_comment, 1);

    check("the first row plus // line comments, alone", "none", R"(\n)",
          ignoring({Token::whitespace, Token::line_comment}), line_comments);

    munch::core::Builder block_comments{};

    c_like(block_comments, false);

    block_comments.add_token(block_comment(), Token::block_comment, 1);

    check("the first row plus block comments, alone", "none", "the same",
          ignoring({Token::whitespace, Token::block_comment}), block_comments);

    munch::core::Builder json_row{};

    json(json_row);

    check("JSON, the RFC 8259 lexical forms over bytes", "none", R"(\t \n \r)", ignoring({Token::whitespace}),
          json_row);

    munch::core::Builder log_lines{};

    log_lines.add_token(plus(any_of(Set::all() - Set{'\n'})), Token::log_line, 2);

    log_lines.add_token(text("\n"), Token::newline, 2);

    check(R"(log lines ([^\n]+ and \n))", R"(\n)", "the same", ignoring({}), log_lines);
}

void Table_checker::check(
        const std::string& name, const std::string& exact, const std::string& relaxed, const Kinds_t& ignored,
        munch::core::Builder& builder)
{
    const auto lexer{build_ignoring(builder, ignored)};

    const auto actual_exact{certified(lexer)};

    const auto actual_relaxed{certified_modulo(lexer)};

    if (actual_relaxed != "the same")
    {
        ++changed_;
    }

    const auto agrees{actual_exact == exact && actual_relaxed == relaxed};

    record(agrees, name);

    if (!agrees)
    {
        std::cout << "         certified: " << actual_exact << ", cell says " << exact
                  << "\n         modulo:    " << actual_relaxed << ", cell says " << relaxed << '\n';
    }
}

void Table_checker::record(const bool agrees, const std::string_view line)
{
    std::cout << verdict_label(agrees) << line << '\n';

    if (!agrees)
    {
        ++failures_;
    }
}

void Table_checker::tokenization_rows()
{
    munch::core::Builder conventional{};

    c_like_with_comments(conventional, false);

    check("C-like, conventional tokenization (whitespace runs include newline)", "none", R"(\n)",
          ignoring({Token::whitespace, Token::line_comment}), conventional);

    munch::core::Builder split_friendly{};

    c_like_with_comments(split_friendly, true);

    check("the same language, split-friendly tokenization", R"(\n)", "the same",
          ignoring({Token::whitespace, Token::newline, Token::line_comment}), split_friendly);

    munch::core::Builder split_friendly_block_comments{};

    c_like_with_block_comments(split_friendly_block_comments, true);

    check("the same plus block comments", "none", "the same",
          ignoring({Token::whitespace, Token::newline, Token::line_comment, Token::block_comment}),
          split_friendly_block_comments);

    munch::core::Builder separated{};

    c_like(separated, true);

    separated.add_token(separated_string(), Token::string, 2);

    separated.add_token(separated_line_comment(), Token::line_comment, 1);

    separated.add_token(separated_block_comment(), Token::block_comment, 1);

    check("the same three kinds, bodies separated from each other's openers", "none", "the same",
          ignoring({Token::whitespace, Token::newline, Token::line_comment, Token::block_comment}), separated);

    munch::core::Builder line_bounded{};

    c_like(line_bounded, true);

    line_bounded.add_token(string_literal(), Token::string, 2);

    line_bounded.add_token(line_comment(), Token::line_comment, 1);

    line_bounded.add_token(line_bounded_plain_block_comment(), Token::block_comment, 1);

    check("the ordinary repertoire, block comment alone barred from crossing a line", R"(\n)", "the same",
          ignoring({Token::whitespace, Token::newline, Token::line_comment, Token::block_comment}), line_bounded);
}

void Table_checker::zig_rows()
{
    munch::core::Builder zig_conventional{};

    zig(zig_conventional, false);

    check("Zig subset, conventional tokenization", "none", R"(\n)", ignoring({Token::whitespace, Token::line_comment}),
          zig_conventional);

    munch::core::Builder zig_split_friendly{};

    zig(zig_split_friendly, true);

    check("the Zig subset, split-friendly tokenization", R"(\n)", "the same",
          ignoring({Token::whitespace, Token::newline, Token::line_comment}), zig_split_friendly);
}

void Table_checker::benchmark_rows()
{
    munch::core::Builder keyword_scale{};

    keyword_scale_grammar(keyword_scale);

    check("keyword_scale_builder() grammar (construction cost)", "! % ( ) * , / : ; ? [ ] ^ { } ~",
          "the same, plus space, tab and newline", ignoring({Token::whitespace}), keyword_scale);

    const std::string keyword_scale_candidates{"=!<>&|+-*/%~^(){}[];,.:?"};

    ratio("keyword_scale_builder() published as 16 of its own 24", keyword_scale_candidates, 24, 16, keyword_scale);

    munch::core::Builder scaling{};

    scaling_grammar(scaling);

    check("build_lexer(false) grammar (scaling table)", "! ( ) * + , - / ; < > { }",
          "the same, plus space, tab and newline", ignoring({Token::whitespace}), scaling);

    const std::string scaling_candidates{"=!<>+-*/(){};,"};

    ratio("build_lexer(false) published as 13 of its own 14", scaling_candidates, 14, 13, scaling);
}

void Table_checker::ratio(
        const std::string& name, const std::string& candidates, const std::size_t total, const std::size_t expected,
        munch::core::Builder& builder)
{
    const std::set<char> distinct{candidates.begin(), candidates.end()};

    const auto lexer{builder.build()};

    const auto actual{certified_among(lexer, candidates)};

    const auto agrees{distinct.size() == total && actual == expected};

    record(agrees, name);

    if (!agrees)
    {
        std::cout << "         candidates: " << distinct.size() << " distinct, cell says " << total
                  << "\n         certified:  " << actual << ", cell says " << expected << '\n';
    }
}

void Table_checker::oracle_candidate_count()
{
    const auto& candidates{oracle_candidates()};

    const std::set<char> distinct{candidates.begin(), candidates.end()};

    const auto agrees{distinct.size() == oracle_candidate_total};

    const auto line{std::format("oracle candidate bytes: {}", distinct.size())};

    record(agrees, line);

    if (!agrees)
    {
        std::cout << "         the report says sixteen\n";
    }
}

void Table_checker::oracle_rows()
{
    const auto c_like_pieces{
            expand({"ab", "+", "(", ";", "12", "@", "@@", "a@b", R"("x@y")", "//c@d\n", "@\n@"}, oracle_candidates())};

    const auto c_like_corpus{documents(c_like_pieces, oracle_documents)};

    const auto block_pieces{
            expand({"ab", "+", ";", "12", "@", "@@", "a@b", R"("x@y")", "//c@d\n", "/*a@b*/", "/*a@b@c*/", "@\n@"},
                   oracle_candidates())};

    const auto block_corpus{documents(block_pieces, oracle_documents)};

    const auto json_pieces{
            expand({"{", "}", "[", "]", ":", ",", "42", "true", "-1.5e3", R"("k")", "@", "@@", R"("a@b")", R"("a\nb")"},
                   oracle_candidates())};

    const auto json_corpus{documents(json_pieces, oracle_documents)};

    munch::core::Builder c_like_conventional{};

    c_like_with_comments(c_like_conventional, false);

    modulo("C-like conventional: none, and newline modulo whitespace and comments", "none", R"(\n)",
           ignoring({Token::whitespace, Token::line_comment}), c_like_corpus, c_like_conventional);

    munch::core::Builder c_like_split_friendly{};

    c_like_with_comments(c_like_split_friendly, true);

    modulo("the same language, split-friendly: newline either way", R"(\n)", R"(\n)",
           ignoring({Token::whitespace, Token::newline, Token::line_comment}), c_like_corpus, c_like_split_friendly);

    munch::core::Builder c_like_block_comments{};

    c_like_with_block_comments(c_like_block_comments, true);

    modulo("the same plus block comments: none either way", "none", "none",
           ignoring({Token::whitespace, Token::newline, Token::line_comment, Token::block_comment}), block_corpus,
           c_like_block_comments);

    munch::core::Builder json_oracle{};

    json(json_oracle);

    modulo("JSON: none, and tab, newline and carriage return modulo whitespace", "none", R"(\t \n \r)",
           ignoring({Token::whitespace}), json_corpus, json_oracle);
}

void Table_checker::modulo(
        const std::string& name, const std::string& exact, const std::string& relaxed, const Kinds_t& ignored,
        const std::vector<std::string>& corpus, munch::core::Builder& builder)
{
    const auto lexer{build_ignoring(builder, ignored)};

    const auto actual_exact{certified(lexer)};

    const auto actual_relaxed{certified_modulo_bytes(lexer)};

    const auto [actual_survives, unexercised]{surviving_modulo(lexer, ignored, corpus)};

    const auto agrees{
            actual_exact == exact && actual_relaxed == relaxed && actual_survives == relaxed && unexercised.empty()};

    record(agrees, name);

    if (!agrees)
    {
        std::cout << "         certified:      " << actual_exact << ", cell says " << exact
                  << "\n         modulo ignored: " << actual_relaxed << ", cell says " << relaxed
                  << "\n         survives split: " << actual_survives << ", cell says " << relaxed << '\n';
    }

    if (!unexercised.empty())
    {
        std::cout << "         never exercised by the corpus: " << unexercised << '\n';
    }
}

void Table_checker::framing_row()
{
    munch::core::Builder builder{};

    const auto interior{Set::all() - Set{record_separator}};

    c_like(builder, false);

    const auto string_body{kleene(any_of(interior - Set{'"'} - Set{'\n'}))};

    builder.add_token(concat(text(R"(")"), string_body, text(R"(")")), Token::string, 2);

    builder.add_token(concat(text("//"), kleene(any_of(interior - Set{'\n'}))), Token::line_comment, 1);

    builder.add_token(block_comment_barring(Set{record_separator}), Token::block_comment, 1);

    builder.add_token(text(record_separator), Token::separator, 1);

    const auto lexer{builder.build()};

    const auto actual{certified(lexer)};

    const auto agrees{actual == std::string{record_separator}};

    record(agrees, "block comments, reserving 0x1E as a separator: certified");

    if (!agrees)
    {
        std::cout << "         expected 0x1E certified, got: " << actual << '\n';
    }
}

void Table_checker::utf8_row()
{
    munch::core::Builder builder{};

    utf8(builder);

    const auto lexer{builder.build()};

    const auto is_lead_byte{[](const int value) {
        return value <= 0x7F || (0xC2 <= value && value <= 0xDF) || (0xE0 <= value && value <= 0xEF) ||
               (0xF0 <= value && value <= 0xF4);
    }};

    std::vector<std::string> disagreeing_bytes{};

    for (int value{0}; value < byte_values; ++value)
    {
        if (lexer.is_split_point(static_cast<char>(value)) == is_lead_byte(value))
        {
            continue;
        }

        disagreeing_bytes.push_back(std::format("{:#04x}", value));
    }

    const auto disagreeing{joined(disagreeing_bytes, " ")};

    const auto agrees{disagreeing.empty()};

    record(agrees, "UTF-8 forms of RFC 3629: usefully certified exactly at the lead bytes");

    if (!agrees)
    {
        std::cout << "         bytes disagreeing with the lead-byte set: " << disagreeing << '\n';
    }
}

void Table_checker::relaxation_count()
{
    const auto agrees{changed_ == moved_rows};

    const auto line{std::format("rows the relaxation moves: {}", changed_)};

    std::cout << '\n';

    record(agrees, line);

    if (!agrees)
    {
        std::cout << "         the report says eight\n";
    }
}

void Table_checker::benchmark_bindings()
{
    const auto bound{scaling_grammar_matches_the_benchmark()};

    std::cout << '\n';

    record(bound, "the transcribed scaling grammar still matches build_lexer(false)");

    const auto scale_bound{keyword_scale_grammar_matches_the_benchmark()};

    record(scale_bound, "the transcribed construction-cost grammar still matches keyword_scale_tokens()");
}

int Table_checker::failures() const noexcept
{
    return failures_;
}

} // namespace

/**
 * @brief Reproduces the applicability table: every row's two columns, the oracle's cross-checks by splitting, the
 *        framing and UTF-8 instances, the count of rows the relaxation moves and the bindings to the benchmark's
 *        grammars, then prints the verdict.
 * @return EXIT_SUCCESS when every row reproduces the table, EXIT_FAILURE otherwise.
 */
int main()
{
    Table_checker checker{};

    checker.base_rows();

    checker.tokenization_rows();

    checker.zig_rows();

    checker.benchmark_rows();

    std::cout << '\n';

    checker.oracle_candidate_count();

    checker.oracle_rows();

    checker.framing_row();

    checker.utf8_row();

    checker.relaxation_count();

    checker.benchmark_bindings();

    const auto failures{checker.failures()};

    std::cout << (failures == 0 ? "\nall rows reproduce the table\n" : "\nrows disagreeing with the table\n");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
