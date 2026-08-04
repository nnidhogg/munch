/*
 * Reproduces the applicability table of the report by compiling each token set and reading the shipped predicate
 * for all 256 byte values. No hand analysis is involved: the useful set is exactly what is_split_point() reports.
 *
 * Build and run from the repository root against an existing build tree:
 *   c++ -std=c++23 -I libs/common/include -I libs/core/include -I libs/dfa/include -I libs/nfa/include \
 *       -I libs/regex/include -I tools/benchmark/include \
 *       paper/figures/applicability.cpp tools/benchmark/src/harness.cpp -o /tmp/applicability \
 *       -L build/libs/core -L build/libs/dfa -L build/libs/nfa -L build/libs/regex \
 *       -lmunch_core -lmunch_dfa -lmunch_nfa -lmunch_regex -lpthread
 *   /tmp/applicability
 */

#include <cstddef>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/benchmark/harness.hpp"

namespace
{
using namespace munch::regex;
using namespace figures;

// The two benchmark rows publish their cells as "N of its own M" rather than as a byte list, and a miscounted M is
// exactly the kind of error the byte-list assertion cannot see. So those rows assert both numbers: M is the distinct
// operator and punctuation bytes the grammar registers, and N is how many of them certify.
std::size_t certified_among(const munch::core::Lexer& lexer, const std::string& candidates)
{
    std::size_t count{0};

    for (const auto candidate : candidates)
    {
        if (lexer.is_split_point(candidate))
        {
            ++count;
        }
    }

    return count;
}

// Renders the useful certified set the way the table's second column reads.
std::string certified(const munch::core::Lexer& lexer)
{
    std::vector<int> bytes;

    for (int value{0}; value < 256; ++value)
    {
        if (lexer.is_split_point(static_cast<char>(value)))
        {
            bytes.push_back(value);
        }
    }

    if (bytes.empty())
    {
        return "none";
    }

    Set combined{operators()};

    for (const auto symbol : punctuation().symbols())
    {
        combined = combined + symbol;
    }

    const auto& expected{combined.symbols()};

    if (bytes.size() == expected.size())
    {
        auto matches{true};

        for (const auto byte : bytes)
        {
            matches = matches && expected.contains(static_cast<char>(byte));
        }

        if (matches)
        {
            return "all operator and punctuation bytes";
        }
    }

    std::string rendered;

    for (const auto byte : bytes)
    {
        rendered += rendered.empty() ? "" : " ";
        rendered += byte == '\n' ? "\\n" : std::string(1, static_cast<char>(byte));
    }

    return rendered;
}

// ---------------------------------------------------------------------------------------------------------------
// A brute-force oracle for the relaxed column. The report claims the condition is not merely sound on these token
// sets but agrees with splitting on every candidate byte, so the claim is checked by splitting rather than by
// trusting the rule. Every declared candidate must also be exercised, or the agreement would be vacuous.
//
// The corpus is built by substituting every candidate byte into every container template. Sampling instead reports
// a byte as safe whenever no input happens to place it inside a string or a comment, which is indistinguishable
// from the byte genuinely being safe; that produced two wrong readings while this was being developed.
// ---------------------------------------------------------------------------------------------------------------

using Kinds = std::set<std::size_t>;

using Stream = std::vector<std::pair<std::size_t, std::size_t>>;

// The relaxed set as a byte list. The table renderer below says "the same" relationally, which reads well in a
// cell but cannot be compared against the brute-force oracle's output; this is what the cross-check uses.
std::string certified_modulo_bytes(const munch::core::Lexer& lexer)
{
    std::string rendered;

    for (int value{0}; value < 256; ++value)
    {
        const auto byte{static_cast<char>(value)};

        if (!lexer.is_split_point_ignoring(byte))
        {
            continue;
        }

        rendered += rendered.empty() ? "" : " ";
        rendered += byte == '\n' ? "\\n" :
                    byte == '\t' ? "\\t" :
                    byte == '\r' ? "\\r" :
                    byte == ' '  ? "SP" :
                                   std::string(1, byte);
    }

    return rendered.empty() ? "none" : rendered;
}

// Renders the relaxed column of the table. Where the relaxed set is the exact one plus the whitespace bytes, it
// says so relationally rather than repeating a long byte list, so the printed cell and the asserted string are the
// same text and the table cannot drift from the assertion.
std::string certified_modulo(const munch::core::Lexer& lexer)
{
    std::set<char> exact, relaxed;

    for (int value{0}; value < 256; ++value)
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

    for (const auto byte : {' ', '\t', '\n'})
    {
        with_whitespace.insert(byte);
    }

    if (relaxed == with_whitespace)
    {
        return "the same, plus space, tab and newline";
    }

    std::string rendered;

    for (const auto byte : relaxed)
    {
        rendered += rendered.empty() ? "" : " ";
        rendered += byte == '\n' ? "\\n" :
                    byte == '\t' ? "\\t" :
                    byte == '\r' ? "\\r" :
                    byte == ' '  ? "SP" :
                                   std::string(1, byte);
    }

    return rendered.empty() ? "none" : rendered;
}

Stream scan(const munch::core::Lexer& lexer, const std::string& text, std::size_t& consumed)
{
    Stream stream;

    consumed = lexer.tokenize_all<std::size_t>(
            text, [&stream](const std::size_t kind, const std::size_t length) { stream.emplace_back(kind, length); });

    return stream;
}

Stream without(const Stream& stream, const Kinds& ignored)
{
    Stream kept;

    for (const auto& token : stream)
    {
        if (!ignored.contains(token.first))
        {
            kept.push_back(token);
        }
    }

    return kept;
}

std::vector<std::string> expand(const std::vector<std::string>& templates, const std::string& fillers)
{
    std::vector<std::string> pieces;

    for (const auto& form : templates)
    {
        if (form.find('@') == std::string::npos)
        {
            pieces.push_back(form);

            continue;
        }

        for (const auto filler : fillers)
        {
            std::string piece;

            for (const auto symbol : form)
            {
                symbol == '@' ? piece += filler : piece += symbol;
            }

            pieces.push_back(piece);
        }
    }

    return pieces;
}

// Deterministic, so a disagreement this reports can be reproduced exactly.
std::vector<std::string> documents(const std::vector<std::string>& pieces, const std::size_t count)
{
    std::vector<std::string> corpus;

    auto seed{20260801U};

    const auto next{[&seed](const unsigned bound) {
        seed = seed * 1664525U + 1013904223U;

        return (seed >> 8U) % bound;
    }};

    // Every piece gets a document of its own before any sampling. Random selection alone left candidates
    // unexercised, which reads exactly like a candidate the condition rejects.
    for (const auto& piece : pieces)
    {
        corpus.push_back(piece + piece);
    }

    for (std::size_t index{0}; index < count; ++index)
    {
        std::string text;

        for (auto parts{4U + next(10)}; parts > 0; --parts)
        {
            text += pieces[next(static_cast<unsigned>(pieces.size()))];
        }

        corpus.push_back(text);
    }

    return corpus;
}

// The bytes worth asking about: whitespace, plus the operator and punctuation bytes a C-like or JSON row uses.
const std::string& oracle_candidates()
{
    static const std::string candidates{" \t\n\r+;(),:[]{}-t"};

    return candidates;
}

// Splits every document at every occurrence of every candidate byte and reports the set that survives modulo the
// ignored kinds, rendered the same way certified_modulo() renders its verdict.
std::string surviving_modulo(
        const munch::core::Lexer& lexer, const Kinds& ignored, const std::vector<std::string>& corpus,
        std::string& unexercised)
{
    std::string rendered;

    for (const auto candidate : oracle_candidates())
    {
        auto exercised{false}, survives{true};

        for (const auto& text : corpus)
        {
            std::size_t consumed{0};

            const auto serial{scan(lexer, text, consumed)};

            if (consumed != text.size())
            {
                continue; // the guarantee is stated only for input that tokenizes completely
            }

            // exempted the cut before the final byte from every check.
            for (std::size_t at{1}; at < text.size(); ++at)
            {
                if (text[at] != candidate)
                {
                    continue;
                }

                exercised = true;

                std::size_t left_used{0}, right_used{0};

                const auto left{scan(lexer, text.substr(0, at), left_used)};

                const auto right{scan(lexer, text.substr(at), right_used)};

                if (left_used != at || right_used != text.size() - at)
                {
                    survives = false;

                    continue;
                }

                Stream spliced{left};

                spliced.insert(spliced.end(), right.begin(), right.end());

                survives = survives && without(spliced, ignored) == without(serial, ignored);
            }
        }

        if (!exercised)
        {
            // Reported rather than skipped: a candidate the corpus never placed inside a token would otherwise be
            // indistinguishable from one the condition genuinely rejects.
            unexercised += unexercised.empty() ? "" : " ";
            unexercised += candidate == '\n' ? "\\n" :
                           candidate == '\t' ? "\\t" :
                           candidate == '\r' ? "\\r" :
                                               std::string(1, candidate);

            continue;
        }

        if (!survives)
        {
            continue;
        }

        rendered += rendered.empty() ? "" : " ";
        rendered += candidate == '\n' ? "\\n" :
                    candidate == '\t' ? "\\t" :
                    candidate == '\r' ? "\\r" :
                                        std::string(1, candidate);
    }

    return rendered.empty() ? "none" : rendered;
}

// The two rows above restate grammars that live in the benchmark tool. A copy that agrees today can drift silently
// while every assertion here still passes, because the assertions compare the copy with itself. This binds the one
// copy whose original is reachable: harness.cpp compiles standalone, so the real build_lexer() can be built here and
// compared against grammars.hpp's transcription of it. Agreement on all 256 certified bits and on the exact token
// lengths of a corpus is far stronger than reading the two side by side.
//
bool scaling_grammar_matches_the_benchmark()
{
    munch::core::Builder transcribed;

    scaling_grammar(transcribed);

    const auto copy{transcribed.build()};

    const auto original{munch::tools::benchmark::build_lexer(false)};

    for (int byte{0}; byte < 256; ++byte)
    {
        const auto symbol{static_cast<char>(byte)};

        if (copy.is_split_point(symbol) != original.is_split_point(symbol))
        {
            std::cout << "         certified sets differ at byte " << byte << '\n';

            return false;
        }
    }

    // Certified sets alone would miss a change that moves a token boundary without moving a certificate, so compare
    // what the two scanners actually emit. Kinds cannot be compared directly, the two enumerations differ.
    const std::string corpus{
            "while (counter <= 4711) { x1 = bar_baz + 97; if (x1 != 42) { return counter; } }\n\tint value2 = 0;\n"};

    std::vector<std::size_t> copy_lengths, original_lengths;

    const auto copy_consumed{copy.tokenize_all<std::size_t>(
            corpus, [&copy_lengths](std::size_t, const std::size_t length) { copy_lengths.push_back(length); })};

    const auto original_consumed{original.tokenize_all<std::size_t>(
            corpus,
            [&original_lengths](std::size_t, const std::size_t length) { original_lengths.push_back(length); })};

    if (copy_consumed != original_consumed || copy_lengths != original_lengths)
    {
        std::cout << "         token streams differ: consumed " << copy_consumed << " against " << original_consumed
                  << '\n';

        return false;
    }

    return true;
}

// The same discipline for the construction-cost row: keyword_scale_tokens() is the grammar the benchmark compiles,
// linked from the harness, so the published 16-of-24 cell cannot drift from it undetected.
bool keyword_scale_grammar_matches_the_benchmark()
{
    munch::core::Builder transcribed;

    keyword_scale_grammar(transcribed);

    const auto copy{transcribed.build()};

    munch::core::Builder linked;

    munch::tools::benchmark::keyword_scale_tokens(linked);

    const auto original{linked.build()};

    for (int byte{0}; byte < 256; ++byte)
    {
        const auto symbol{static_cast<char>(byte)};

        if (copy.is_split_point(symbol) != original.is_split_point(symbol))
        {
            std::cout << "         certified sets differ at byte " << byte << '\n';

            return false;
        }
    }

    // Certified sets alone would miss a change that moves a token boundary without moving a certificate, so compare
    // what the two scanners actually emit. Kinds cannot be compared directly, the two enumerations differ.
    const std::string corpus{
            "while (alignas != 4711) { co_await counter++; } constexpr double x2 = -3.5e7 % rate;\n\tstatic_cast\n"};

    std::vector<std::size_t> copy_lengths, original_lengths;

    const auto copy_consumed{copy.tokenize_all<std::size_t>(
            corpus, [&copy_lengths](std::size_t, const std::size_t length) { copy_lengths.push_back(length); })};

    const auto original_consumed{original.tokenize_all<std::size_t>(
            corpus,
            [&original_lengths](std::size_t, const std::size_t length) { original_lengths.push_back(length); })};

    if (copy_consumed != original_consumed || copy_lengths != original_lengths)
    {
        std::cout << "         token streams differ: consumed " << copy_consumed << " against " << original_consumed
                  << '\n';

        return false;
    }

    return true;
}

} // namespace

int main()
{
    int failures{0};

    // The candidate bytes of each benchmark grammar: every distinct byte occurring in an operator or punctuation
    // literal it registers. Written out rather than derived, so a wrong count in the paper cannot match a wrong count
    // here by construction.
    const std::string keyword_scale_candidates{"=!<>&|+-*/%~^(){}[];,.:?"};
    const std::string scaling_candidates{"=!<>+-*/(){};,"};

    // Every row asserts both published columns: what the exact certificate admits, and what it admits once the
    // caller's discarded tokens are deleted. The relaxed column is read from the shipped predicate, which needs no
    // corpus; the four rows with corpora below additionally confirm it by splitting.
    // The report states how many rows the relaxation moves. Counting it here rather than trusting prose is the
    // point: an earlier text-parsing check miscounted by reading "the same, plus ..." as unchanged.
    int changed{0};

    const auto check{[&failures, &changed](
                             const std::string& name, const std::string& exact, const std::string& relaxed,
                             const Kinds& ignored, munch::core::Builder& b) {
        b.set_ignored_tokens(std::vector<std::size_t>{ignored.begin(), ignored.end()});

        const auto lexer{b.build()};

        const auto actual_exact{certified(lexer)};

        const auto actual_relaxed{certified_modulo(lexer)};

        changed += actual_relaxed == "the same" ? 0 : 1;

        const auto agrees{actual_exact == exact && actual_relaxed == relaxed};

        std::cout << (agrees ? "  ok   " : "  FAIL ") << name << '\n';

        if (!agrees)
        {
            std::cout << "         certified: " << actual_exact << ", cell says " << exact
                      << "\n         modulo:    " << actual_relaxed << ", cell says " << relaxed << '\n';

            ++failures;
        }
    }};

    const auto ratio{[&failures](
                             const std::string& name, const std::string& candidates, const std::size_t total,
                             const std::size_t expected, munch::core::Builder& b) {
        const std::set<char> distinct{candidates.begin(), candidates.end()};
        const auto actual{certified_among(b.build(), candidates)};
        const auto agrees{distinct.size() == total && actual == expected};

        std::cout << (agrees ? "  ok   " : "  FAIL ") << name << '\n';

        if (!agrees)
        {
            std::cout << "         candidates: " << distinct.size() << " distinct, cell says " << total
                      << "\n         certified:  " << actual << ", cell says " << expected << '\n';

            ++failures;
        }
    }};

    {
        munch::core::Builder b;
        c_like(b, false);
        check("C-like: identifiers, numbers, ws runs, operators, punct", "all operator and punctuation bytes",
              "the same, plus space, tab and newline", ignoring({Token::Whitespace}), b);
    }
    // Each of the next three adds exactly one token kind to that same base, so each collapse is attributable to the
    // token kind named rather than to an accumulation of them.
    {
        munch::core::Builder b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        check("the first row plus strings, alone (no raw newline inside)", "none", "\\n", ignoring({Token::Whitespace}),
              b);
    }
    {
        munch::core::Builder b;
        c_like(b, false);
        b.add_token(line_comment(), Token::LineComment, 1);
        check("the first row plus // line comments, alone", "none", "\\n",
              ignoring({Token::Whitespace, Token::LineComment}), b);
    }
    {
        munch::core::Builder b;
        c_like(b, false);
        b.add_token(block_comment(), Token::BlockComment, 1);
        check("the first row plus block comments, alone", "none", "the same",
              ignoring({Token::Whitespace, Token::BlockComment}), b);
    }
    {
        munch::core::Builder b;
        json(b);
        check("JSON, the RFC 8259 lexical forms over bytes", "none", "\\t \\n \\r", ignoring({Token::Whitespace}), b);
    }
    {
        munch::core::Builder b;
        b.add_token(plus(any_of(Set::all() - Set{'\n'})), Token::LogLine, 2);
        b.add_token(text("\n"), Token::Newline, 2);
        check("log lines ([^\\n]+ and \\n)", "\\n", "the same", ignoring({}), b);
    }
    // The next two recognize exactly the same byte language and differ only in how it is cut into tokens: the
    // conventional one folds newline into the whitespace run, the split-friendly one gives newline its own token and
    // leaves spaces and tabs as a run. That is the pair the text needs to claim certification depends on the
    // tokenization rather than on the recognized language.
    {
        munch::core::Builder b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        check("C-like, conventional tokenization (whitespace runs include newline)", "none", "\\n",
              ignoring({Token::Whitespace, Token::LineComment}), b);
    }
    {
        munch::core::Builder b;
        c_like(b, true);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        check("the same language, split-friendly tokenization", "\\n", "the same",
              ignoring({Token::Whitespace, Token::Newline, Token::LineComment}), b);
    }
    {
        munch::core::Builder b;
        // Deliberately cumulative: this is the same grammar as the row above with block comments added, so the
        // pair is a true before/after on the one token kind that spans lines.
        c_like(b, true);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        b.add_token(block_comment(), Token::BlockComment, 1);
        check("the same plus block comments", "none", "the same",
              ignoring({Token::Whitespace, Token::Newline, Token::LineComment, Token::BlockComment}), b);
    }
    {
        munch::core::Builder b;
        keyword_scale_grammar(b);
        check("keyword_scale_builder() grammar (construction cost)", "! % ( ) * , / : ; ? [ ] ^ { } ~",
              "the same, plus space, tab and newline", ignoring({Token::Whitespace}), b);
        ratio("keyword_scale_builder() published as 16 of its own 24", keyword_scale_candidates, 24, 16, b);
    }
    {
        munch::core::Builder b;
        scaling_grammar(b);
        check("build_lexer(false) grammar (scaling table)", "! ( ) * + , - / ; < > { }",
              "the same, plus space, tab and newline", ignoring({Token::Whitespace}), b);
        ratio("build_lexer(false) published as 13 of its own 14", scaling_candidates, 14, 13, b);
    }

    // ---------------------------------------------------------------------------------------------------------
    // The four rows the report cross-checks by splitting as well as by reading the predicate. Each asserts three
    // things: what the shipped predicate certifies, what the relaxed condition certifies, and that splitting really
    // does survive at exactly the relaxed set over every declared candidate byte. That is weaker than exactness on
    // these token sets, since only the declared candidates are tried, and the report says so.
    // ---------------------------------------------------------------------------------------------------------

    std::cout << '\n';

    // The report states how many candidate bytes the oracle tries, so the count is asserted rather than counted by
    // hand; it was published as fifteen while the set held sixteen.
    {
        const std::set<char> distinct{oracle_candidates().begin(), oracle_candidates().end()};

        const auto agrees{distinct.size() == 16};

        std::cout << (agrees ? "  ok   " : "  FAIL ") << "oracle candidate bytes: " << distinct.size() << '\n';

        if (!agrees)
        {
            std::cout << "         the report says sixteen\n";

            ++failures;
        }
    }

    const auto modulo{[&failures](
                              const std::string& name, const std::string& exact, const std::string& relaxed,
                              const Kinds& ignored, const std::vector<std::string>& corpus, munch::core::Builder& b) {
        b.set_ignored_tokens(std::vector<std::size_t>{ignored.begin(), ignored.end()});

        const auto lexer{b.build()};

        const auto actual_exact{certified(lexer)};

        const auto actual_relaxed{certified_modulo_bytes(lexer)};

        std::string unexercised;

        const auto actual_survives{surviving_modulo(lexer, ignored, corpus, unexercised)};

        const auto agrees{
                actual_exact == exact && actual_relaxed == relaxed && actual_survives == relaxed &&
                unexercised.empty()};

        std::cout << (agrees ? "  ok   " : "  FAIL ") << name << '\n';

        if (!agrees)
        {
            std::cout << "         certified:      " << actual_exact << ", cell says " << exact
                      << "\n         modulo ignored: " << actual_relaxed << ", cell says " << relaxed
                      << "\n         survives split: " << actual_survives << ", cell says " << relaxed << '\n';

            if (!unexercised.empty())
            {
                std::cout << "         NEVER EXERCISED by the corpus: " << unexercised << '\n';
            }

            ++failures;
        }
    }};

    // Containers carrying an '@' hole have every candidate byte substituted into them, so no byte is judged safe
    // merely because the corpus never placed it inside a token.
    const auto c_like_corpus{documents(
            expand({"ab", "+", "(", ";", "12", "@", "@@", "a@b", "\"x@y\"", "//c@d\n", "@\n@"}, oracle_candidates()),
            200)};

    const auto block_corpus{documents(
            expand({"ab", "+", ";", "12", "@", "@@", "a@b", "\"x@y\"", "//c@d\n", "/*a@b*/", "/*a@b@c*/", "@\n@"},
                   oracle_candidates()),
            200)};

    const auto json_corpus{documents(
            expand({"{", "}", "[", "]", ":", ",", "42", "true", "-1.5e3", "\"k\"", "@", "@@", "\"a@b\"", "\"a\\nb\""},
                   oracle_candidates()),
            200)};

    {
        munch::core::Builder b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        modulo("C-like conventional: none, and newline modulo whitespace and comments", "none", "\\n",
               ignoring({Token::Whitespace, Token::LineComment}), c_like_corpus, b);
    }
    {
        munch::core::Builder b;
        c_like(b, true);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        modulo("the same language, split-friendly: newline either way", "\\n", "\\n",
               ignoring({Token::Whitespace, Token::Newline, Token::LineComment}), c_like_corpus, b);
    }
    {
        // Cumulative on the split-friendly row above, exactly as the exact-column row of the same name is.
        munch::core::Builder b;
        c_like(b, true);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        b.add_token(block_comment(), Token::BlockComment, 1);
        modulo("the same plus block comments: none either way", "none", "none",
               ignoring({Token::Whitespace, Token::Newline, Token::LineComment, Token::BlockComment}), block_corpus, b);
    }
    {
        munch::core::Builder b;
        json(b);
        modulo("JSON: none, and tab, newline and carriage return modulo whitespace", "none", "\\t \\n \\r",
               ignoring({Token::Whitespace}), json_corpus, b);
    }

    // The framing construction: a byte no token admits in its interior, given a rule of its own, is certified
    // outright by the shipped predicate even for the token set that certifies nothing. 0x1E is ASCII RECORD
    // SEPARATOR, reserved for exactly this.
    {
        munch::core::Builder b;

        const auto interior{Set::all() - Set{'\x1E'}};

        c_like(b, false);

        b.add_token(concat(text("\""), kleene(any_of(interior - Set{'"'} - Set{'\n'})), text("\"")), Token::String, 2);

        b.add_token(concat(text("//"), kleene(any_of(interior - Set{'\n'}))), Token::LineComment, 1);

        b.add_token(
                concat(text("/*"),
                       kleene(
                               choice(any_of(interior - Set{'*'}),
                                      concat(plus(any_of(Set{'*'})), any_of(interior - Set{'*'} - Set{'/'})))),
                       plus(any_of(Set{'*'})), text("/")),
                Token::BlockComment, 1);

        b.add_token(text("\x1E"), Token::Separator, 1);

        const auto actual{certified(b.build())};

        const auto agrees{actual == "\x1E"};

        std::cout << (agrees ? "  ok   " : "  FAIL ") << "block comments, reserving 0x1E as a separator: certified\n";

        if (!agrees)
        {
            std::cout << "         expected 0x1E certified, got: " << actual << '\n';

            ++failures;
        }
    }

    {
        const auto agrees{changed == 7};

        std::cout << (agrees ? "\n  ok   " : "\n  FAIL ") << "rows the relaxation moves: " << changed << '\n';

        if (!agrees)
        {
            std::cout << "         the report says seven\n";

            ++failures;
        }
    }

    {
        const auto bound{scaling_grammar_matches_the_benchmark()};

        std::cout << (bound ? "\n  ok   " : "\n  FAIL ")
                  << "the transcribed scaling grammar still matches build_lexer(false)\n";

        if (!bound)
        {
            ++failures;
        }

        const auto scale_bound{keyword_scale_grammar_matches_the_benchmark()};

        std::cout << (scale_bound ? "  ok   " : "  FAIL ")
                  << "the transcribed construction-cost grammar still matches keyword_scale_tokens()\n";

        if (!scale_bound)
        {
            ++failures;
        }
    }

    std::cout << (failures == 0 ? "\nall rows reproduce the table\n" : "\nrows disagreeing with the table\n");

    return failures == 0 ? 0 : 1;
}
