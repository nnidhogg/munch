#include "munch/tools/audit/read_logos.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace munch::tools::audit;

namespace
{
/**
 * @brief The one rule of a file holding one enum with one variant carrying the given attribute.
 * @param attribute The attribute's text, `#[...]` included.
 * @return The rule.
 */
Lexer_spec::Rule rule_of(const std::string_view attribute)
{
    const auto lexers{read_logos("#[derive(Logos)]\nenum T {\n" + std::string{attribute} + "\n    V,\n}\n")};

    return lexers.at(0).rules.at(0);
}

/**
 * @brief The line a source is refused at, or -1 when it is read.
 * @param source The source.
 * @return The line.
 */
long line_of(const std::string_view source)
{
    try
    {
        std::ignore = read_logos(source);
    }
    catch (const Spec_error& error)
    {
        return static_cast<long>(error.line());
    }

    return -1L;
}

/**
 * @brief The logos attribute idioms in one file: skips in both spellings, subpatterns referring to each other, a
 *        token with a callback, a regex with a priority override, callbacks naming functions defined before and after
 *        the enum, a callback that skips, a skip through a closure, several attributes on one variant, variants with
 *        payloads and discriminants, attributes logos ignores, and a second enum, all among the Rust the reader must
 *        step over.
 *
 * Rust that logos 0.15.1 derives as it stands, given the `Extras` type: every variant is a unit or a tuple one, named
 * fields being what the crate says it "doesn't support yet", and the `#[default]` attribute has the derive that gives
 * it a meaning. The ranking test below holds the reading against the crate's own answers for this file, so the file
 * has to be one the crate compiles.
 */
constexpr std::string_view idioms{R"rs(//! A lexer, with "#[derive(Logos)]" in a doc comment to step over.
use logos::{Lexer, Logos};

/* A block comment /* nesting */ with an enum Token { in it. */
fn callback<'a>(lex: &mut Lexer<'a, Token<'a>>) -> Option<&'a str> {
    let c = '"'; let s = "enum Nope {"; let r = r#"#[token("x")]"#;
    Some(lex.slice())
}

#[derive(Logos, Debug, Default, PartialEq)]
#[logos(skip r"[ \t\n\f]+", extras = Extras)]
#[logos(skip("//[^\n]*", priority = 3))]
#[logos(subpattern digit = r"[0-9]")]
#[logos(subpattern number = r"(?&digit)+")]
#[repr(u16)]
pub enum Token<'a> {
    #[token("fn")]
    Fn = 1,

    #[regex("[a-zA-Z_][a-zA-Z0-9_]*", callback)]
    Ident(&'a str),

    #[regex("(?&number)", number, priority = 5)]
    Number(u64),

    #[token("+")]
    #[token("-")]
    Sign,

    #[regex(r"/\*[^*]*\*+([^*/][^*]*\*+)*/", logos::skip)]
    Comment,

    #[token("#", |_| logos::Skip)]
    Hash,

    #[regex(r"[0-9]+k", callback = kilo)]
    Kilo(u64),

    #[default]
    Error,
}

fn number<'a>(lex: &mut Lexer<'a, Token<'a>>) -> Option<u64> {
    lex.slice().parse().ok()
}

fn kilo<'a>(lex: &mut Lexer<'a, Token<'a>>) -> Option<u64> {
    lex.slice().trim_end_matches('k').parse().ok()
}

mod strings {
    #[derive(Clone, logos::Logos)]
    pub enum Part {
        #[regex(r#"[^"\\]+"#)]
        Text,
        #[token(r#"""#)]
        Quote,
    }
}
)rs"};

} // namespace

TEST(Read_logos, Reads_the_attribute_idioms_of_an_enum_deriving_Logos)
{
    const auto lexers{read_logos(idioms)};

    ASSERT_EQ(lexers.size(), 2u);

    const auto& spec{lexers.front()};

    EXPECT_EQ(spec.line, 10u);
    EXPECT_TRUE(spec.conditions.empty());

    // The enum's own keys, after the Unicode version the reader's classes were taken from.
    EXPECT_EQ(spec.options, (std::vector<std::string>{"unicode-classes=16.0.0", "extras=Extras"}));

    ASSERT_EQ(spec.definitions.size(), 2u);
    EXPECT_EQ(spec.definitions.at("digit"), "[0-9]");
    EXPECT_EQ(spec.definitions.at("number"), "{digit}+");

    // Two skips first, then the variants' attributes in order, one rule each.
    ASSERT_EQ(spec.rules.size(), 10u);

    EXPECT_EQ(spec.rules[0].pattern, R"(r"[ \t\n\f]+")");
    EXPECT_EQ(spec.rules[0].expression, R"([\t\n\x0c ]+)");
    EXPECT_FALSE(spec.rules[0].token.has_value());
    EXPECT_EQ(spec.rules[0].priority, std::optional<std::size_t>{2});
    EXPECT_EQ(spec.rules[0].line, 11u);

    EXPECT_EQ(spec.rules[1].pattern, R"("//[^\n]*")");
    EXPECT_FALSE(spec.rules[1].token.has_value());
    EXPECT_EQ(spec.rules[1].priority, std::optional<std::size_t>{3});
    EXPECT_EQ(spec.rules[1].line, 12u);

    EXPECT_EQ(spec.rules[2].pattern, R"("fn")");
    EXPECT_EQ(spec.rules[2].expression, R"("fn")");
    EXPECT_EQ(spec.rules[2].token, std::optional<std::string>{"Fn"});
    EXPECT_EQ(spec.rules[2].priority, std::optional<std::size_t>{4});
    EXPECT_EQ(spec.rules[2].line, 17u);

    EXPECT_EQ(spec.rules[3].expression, "[A-Z_a-z][0-9A-Z_a-z]*");
    EXPECT_EQ(spec.rules[3].token, std::optional<std::string>{"Ident"});
    EXPECT_EQ(spec.rules[3].action, "callback");
    EXPECT_EQ(spec.rules[3].priority, std::optional<std::size_t>{2});

    EXPECT_EQ(spec.rules[4].expression, "{number}");
    EXPECT_EQ(spec.rules[4].token, std::optional<std::string>{"Number"});
    EXPECT_EQ(spec.rules[4].action, "number");
    EXPECT_EQ(spec.rules[4].priority, std::optional<std::size_t>{5});

    EXPECT_EQ(spec.rules[5].expression, R"("+")");
    EXPECT_EQ(spec.rules[5].token, std::optional<std::string>{"Sign"});
    EXPECT_EQ(spec.rules[6].expression, R"("-")");
    EXPECT_EQ(spec.rules[6].token, std::optional<std::string>{"Sign"});

    // A callback spelled logos::skip discards, as does a closure whose whole body is logos::Skip.
    EXPECT_TRUE(spec.rules[7].expression.starts_with(R"("/*"[\x00-)+-\x7f\u{80}-)")) << spec.rules[7].expression;
    EXPECT_FALSE(spec.rules[7].token.has_value());
    EXPECT_EQ(spec.rules[7].action, "logos::skip");
    EXPECT_FALSE(spec.rules[8].token.has_value());

    EXPECT_EQ(spec.rules[9].token, std::optional<std::string>{"Kilo"});
    EXPECT_EQ(spec.rules[9].action, "kilo");
    EXPECT_EQ(spec.rules[9].priority, std::optional<std::size_t>{4});

    EXPECT_EQ(active_rules(spec, "INITIAL").size(), 10u);

    // The second enum, inside a module and deriving by path.
    const auto& part{lexers.back()};

    EXPECT_EQ(part.line, 52u);
    ASSERT_EQ(part.rules.size(), 2u);
    EXPECT_EQ(part.rules[0].token, std::optional<std::string>{"Text"});
    EXPECT_EQ(part.rules[1].pattern, R"(r#"""#)");
    EXPECT_EQ(part.rules[1].expression, R"("\"")");
}

TEST(Read_logos, A_token_under_an_ignore_flag_is_the_regex_logos_escapes_it_into)
{
    // logos compiles a token under an ignore flag as a regex, the literal escaped for the regex crate: a byte
    // string's byte beyond ASCII is written out as the characters of its `\xNN` escape and the backslash escaped
    // again, so the token matches those eight characters and never the two bytes. logos 0.15.1 answers Folded on
    // the text and Explicit on the bytes, and its conflict hint for two such tokens names `priority = 17`, the
    // priority sixteen the escape text's eight characters give.
    const auto folded{rule_of(R"rs(#[token(b"\xC3\xA9", ignore(case))])rs")};

    EXPECT_EQ(folded.expression, R"("\\"[Xx][Cc]"3\\"[Xx][Aa]"9")");
    EXPECT_EQ(folded.priority, std::optional<std::size_t>{16});

    // With no ignore flag the token is a literal to logos, matched as its bytes at twice their number.
    const auto plain{rule_of(R"rs(#[token(b"\xC3\xA9")])rs")};

    EXPECT_EQ(plain.expression, R"("\xc3\xa9")");
    EXPECT_EQ(plain.priority, std::optional<std::size_t>{4});

    // An ASCII literal escapes into itself, so the count is twice its length under the flag as without it, and the
    // hint logos gives for the pair of them names `priority = 5`.
    EXPECT_EQ(rule_of(R"rs(#[token(b"is", ignore(case))])rs").priority, std::optional<std::size_t>{4});
    EXPECT_EQ(rule_of(R"rs(#[token("is", ignore(case))])rs").priority, std::optional<std::size_t>{4});
    EXPECT_EQ(rule_of(R"rs(#[token("is")])rs").priority, std::optional<std::size_t>{4});

    // Ranked against a regex of priority three over the same bytes, the escaped token loses the bytes and takes
    // its own text, which is the pair of answers logos gives.
    const auto ranked{read_logos(
                              "#[derive(Logos)]\n#[logos(source = [u8])]\nenum T {\n" +
                              std::string{R"rs(    #[token(b"\xC3\xA9", ignore(case))])rs"} + "\n    Folded,\n" +
                              std::string{R"rs(    #[regex(b"\xC3\xA9", priority = 3)])rs"} + "\n    Explicit,\n}\n")
                              .front()};

    const auto lexer{build(ranked, "INITIAL")};

    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\xc3\xa9"}).token, std::optional<std::size_t>{1});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{R"(\xc3\xa9)"}).token, std::optional<std::size_t>{0});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{R"(\XC3\XA9)"}).token, std::optional<std::size_t>{0});
}

TEST(Read_logos, A_repetition_logos_cannot_resolve_at_its_boundary_is_refused)
{
    const auto line_of_attribute{[](const std::string_view attribute) {
        return line_of("#[derive(Logos)]\nenum T {\n    " + std::string{attribute} + "\n    V,\n}\n");
    }};

    // logos 0.15.1 decides a repetition's end on one byte, so a repetition whose body can begin with a byte that may
    // also follow it becomes a scanner that matches no input at all: the crate answers nothing for every input to
    // each of these, the flex spelling of the block comment among them, whose loop and closer both admit a star.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a+a")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r#"".*""#)])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"/\*([^*]|\*+[^*/])*\*+/")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"([a-z]+,)*x")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[[:word:]]+[[:^digit:]]")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a(b|c+d)*ce")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a(bc+)*ce")])rs"), 3);

    // With the two byte sets apart the crate scans the language and the pattern reads: the block comment spelled so
    // that its loop cannot begin with a star, a nested plus whose follow it does not admit, and the study's own
    // rules. A bounded repetition needs no such decision, logos unrolling it, so an ambiguous one still reads.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"/\*[^*]*\*+([^*/][^*]*\*+)*/")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"([a-z]+,)*0")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a(bc+)*fe")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[0-9]+k")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[[:word:]]+[[:^word:]]")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"ab?be")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a{2,3}a")])rs"), -1);

    // The check reads the pattern logos compiled, so a subpattern's repetition is decided where it is used.
    EXPECT_EQ(
            line_of(R"rs(#[derive(Logos)]
#[logos(subpattern run = r"[a-z]+")]
enum T {
    #[regex(r"(?&run)x")]
    V,
}
)rs"),
            4);
}

TEST(Read_logos, An_ignore_ascii_case_flag_folds_the_ascii_letters_as_logos_folds_them)
{
    // logos parses a pattern under `ignore(ascii_case)` as it stands and folds the ASCII letters of the compiled
    // tree, so a class gains the other case of its ASCII members: logos 0.15.1 matches IS for the token and ABC for
    // the class, and leaves a folded non-ASCII scalar alone.
    EXPECT_EQ(rule_of(R"rs(#[regex("a", ignore(ascii_case))])rs").expression, "[Aa]");
    EXPECT_EQ(rule_of(R"rs(#[regex("[a-z]+", ignore(ascii_case))])rs").expression, "[A-Za-z]+");
    EXPECT_EQ(rule_of(R"rs(#[token("is", ignore(ascii_case))])rs").expression, "[Ii][Ss]");
    EXPECT_EQ(rule_of(R"rs(#[token("is", ignore(ascii_case))])rs").priority, std::optional<std::size_t>{4});

    // `ignore(case)` is the crate's own case-insensitive parse instead, which reaches the long s.
    EXPECT_EQ(rule_of(R"rs(#[token("is", ignore(case))])rs").expression, R"([Ii][Ss\u{17f}])");

    // A literal is taken apart one piece per byte, so a scalar beyond ASCII counts two for each of its bytes, the
    // four logos's own conflict hint names, and the folding leaves it matching only itself.
    const auto acute{read_logos(R"rs(#[derive(Logos)]
enum T {
    #[token("\u{e9}", ignore(ascii_case))]
    V,
}
)rs")
                             .front()};

    EXPECT_EQ(acute.rules.front().priority, std::optional<std::size_t>{4});

    const auto lexer{build(acute, "INITIAL")};

    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\xc3\xa9"}).length, 2u);
    EXPECT_FALSE(lexer.tokenize<std::size_t>(std::string{"\xc3\x89"}).token.has_value());

    // A byte string takes the crate's binary case-insensitive parse under either flag, ASCII-only either way.
    EXPECT_EQ(rule_of(R"rs(#[token(b"is", ignore(ascii_case))])rs").expression, "[Ii][Ss]");

    // An `(?i)` inside the pattern keeps the crate's own folding for its scope, the ASCII pass adding nothing.
    EXPECT_EQ(rule_of(R"rs(#[regex("(?i)s", ignore(ascii_case))])rs").expression, R"([Ss\u{17f}])");

    // logos 0.15.1 knows priority, callback and ignore and no other argument, `allow_greedy` among the ones it
    // calls an unknown nested attribute, on a variant's attribute and in a skip alike.
    EXPECT_EQ(
            line_of(R"rs(#[derive(Logos)]
enum T {
    #[regex("a", allow_greedy = true)]
    V,
}
)rs"),
            3);
    EXPECT_EQ(
            line_of(R"rs(#[derive(Logos)]
#[logos(skip("a", allow_greedy = true))]
enum T {
    #[regex("b")]
    V,
}
)rs"),
            2);

    // logos refuses the two flags together, and knows no third one.
    EXPECT_EQ(
            line_of(R"rs(#[derive(Logos)]
enum T {
    #[regex("a", ignore(case, ascii_case))]
    V,
}
)rs"),
            3);
    EXPECT_EQ(
            line_of(R"rs(#[derive(Logos)]
enum T {
    #[regex("a", ignore(fold))]
    V,
}
)rs"),
            3);
}

TEST(Read_logos, The_built_lexer_ranks_as_logos_ranks)
{
    const auto lexer{build(read_logos(idioms).front(), "INITIAL")};

    // Longest match first: an identifier beats the keyword it extends; at equal length the higher priority wins,
    // so `fn` is the token and a digit run is Number over nothing shorter.
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"fn"}).token, std::optional<std::size_t>{2});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"fnord"}).token, std::optional<std::size_t>{3});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"42"}).token, std::optional<std::size_t>{4});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"42k"}).token, std::optional<std::size_t>{9});

    // A skip's rule carries a lower id than any variant's and is discarded; the newline certifies neither way,
    // since the discarded block comment folds it in, which is the split-points paper's finding on that token.
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"  x"}).token, std::optional<std::size_t>{0});
    EXPECT_FALSE(lexer.is_split_point('\n'));
    EXPECT_FALSE(lexer.is_split_point_ignoring('\n'));
    EXPECT_FALSE(lexer.is_split_point(' '));
}

TEST(Read_logos, Priorities_are_computed_as_the_handbook_documents)
{
    const auto priority_of{[](const std::string_view attribute) { return *rule_of(attribute).priority; }};

    // The handbook's own examples, and the shapes around them.
    EXPECT_EQ(priority_of(R"rs(#[regex("[a-zA-Z]+")])rs"), 2u);
    EXPECT_EQ(priority_of(R"rs(#[regex("foobar")])rs"), 12u);
    EXPECT_EQ(priority_of(R"rs(#[regex("(foo|hello)(bar)?")])rs"), 6u);
    EXPECT_EQ(priority_of(R"rs(#[regex("a|b")])rs"), 2u);
    EXPECT_EQ(priority_of(R"rs(#[regex("[a-b]")])rs"), 2u);
    EXPECT_EQ(priority_of(R"rs(#[regex("(foo)+")])rs"), 6u);
    EXPECT_EQ(priority_of(R"rs(#[regex("(fooz|bar)+qux")])rs"), 12u);
    EXPECT_EQ(priority_of(R"rs(#[regex("a{3}")])rs"), 6u);
    EXPECT_EQ(priority_of(R"rs(#[regex("a{2,5}b*")])rs"), 4u);
    EXPECT_EQ(priority_of(R"rs(#[regex("Été")])rs"), 6u);
    EXPECT_EQ(priority_of(R"rs(#[regex(".")])rs"), 2u);
    EXPECT_EQ(priority_of(R"rs(#[regex("(?i)ab")])rs"), 4u);

    // A token counts its bytes, a regex its scalars, a run that is not UTF-8 its bytes, and a capture keeps the
    // runs on either side of it apart; an override stands whatever the shape.
    EXPECT_EQ(priority_of(R"rs(#[token("é")])rs"), 4u);
    EXPECT_EQ(priority_of(R"rs(#[regex("é")])rs"), 2u);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xC3\xA9")])rs"), 2u);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xC3(\xA9)")])rs"), 4u);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xC3(?:\xA9)")])rs"), 2u);
    EXPECT_EQ(priority_of(R"rs(#[token("foobar", priority = 20)])rs"), 20u);
    EXPECT_EQ(priority_of(R"rs(#[regex("[a-z]+", |_| (), priority = 7)])rs"), 7u);

    // What counts as UTF-8 is what Rust's own validation takes, since logos asks `std::str::from_utf8`: a run that
    // encodes a surrogate, an overlong form or a scalar above U+10FFFF is bytes, not characters, and scores twice
    // its byte count. logos 0.15.1 itself names these priorities, one higher, in the hint of its conflict error.
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xED\xA0\x80")])rs"), 6u);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xE0\x80\x80")])rs"), 6u);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xF4\x90\x80\x80")])rs"), 8u);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xC0\xAF")])rs"), 4u);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xF0\x9F\x98\x80")])rs"), 2u);
}

TEST(Read_logos, Patterns_are_rewritten_over_the_UTF8_bytes)
{
    // The dot is every scalar but the newline; the scalars beyond ASCII are written as the code point ranges the
    // parser reads as their encodings.
    EXPECT_EQ(rule_of(R"rs(#[regex(".")])rs").expression, R"([\x00-\t\x0b-\x7f\u{80}-\u{d7ff}\u{e000}-\u{10ffff}])");
    EXPECT_EQ(rule_of(R"rs(#[regex("(?s).")])rs").expression, R"([\x00-\x7f\u{80}-\u{d7ff}\u{e000}-\u{10ffff}])");

    // A negated class over ASCII exclusions admits every other scalar.
    EXPECT_EQ(
            rule_of(R"rs(#[regex(r#"[^"\\]+"#)])rs").expression,
            R"([\x00-!#-\[\]-\x7f\u{80}-\u{d7ff}\u{e000}-\u{10ffff}]+)");

    // Non-ASCII scalars, typed or escaped, single or in ranges, are their UTF-8; a plain string decodes Rust's
    // escapes before the regex reads what is left.
    EXPECT_EQ(rule_of(R"rs(#[regex("é+")])rs").expression, "\"\\xc3\\xa9\"+");
    EXPECT_EQ(rule_of(R"rs(#[token("a\tb\u{e9}")])rs").expression, "\"a\\tb\\xc3\\xa9\"");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"[ ,\ufeff]+")])rs").expression, R"([ ,\u{feff}]+)");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"[\u005D-\u00FF]")])rs").expression, R"([\]-\x7f\u{80}-\u{ff}])");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"\x{1F600}")])rs").expression, "\"\\xf0\\x9f\\x98\\x80\"");

    // Under `i` a letter is the class of its cases, `k` and `s` the Kelvin sign's and the long s's too, scoped to
    // the group the flag stands in; a token's ignore(case) is the same reading.
    EXPECT_EQ(rule_of(R"rs(#[regex("(?i)ab")])rs").expression, "[Aa][Bb]");
    EXPECT_EQ(rule_of(R"rs(#[regex("(?i:k)s")])rs").expression, R"([Kk\u{212a}]"s")");
    EXPECT_EQ(rule_of(R"rs(#[regex("(?i)[a-c]1")])rs").expression, "[A-Ca-c]\"1\"");
    EXPECT_EQ(rule_of(R"rs(#[token("is", ignore(case))])rs").expression, R"([Ii][Ss\u{17f}])");
    EXPECT_EQ(rule_of(R"rs(#[token("is", ignore(case))])rs").priority, std::optional<std::size_t>{4});

    // The other constructs: groups of every kind, a capture keeping the runs beside it apart, counts, empty
    // branches, ASCII classes, the ASCII forms of the Perl classes, a byte string pattern over bytes, a nested class
    // and an ASCII class folded before their negation, and the bracket's own characters.
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?P<n>a)(?:b)(?<m>c)")])rs").expression, "\"a\"\"b\"\"c\"");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"a(?:b)c")])rs").expression, "\"abc\"");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(ab){2,}(cd){3}(ef){1,2}")])rs").expression, "\"ab\"{2,}\"cd\"{3}\"ef\"{1,2}");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"x(a|)")])rs").expression, "\"x\"\"a\"?");
    EXPECT_EQ(
            rule_of(R"rs(#[regex(r"[[:word:]]+[[:^word:]]")])rs").expression,
            R"([0-9A-Z_a-z]+[\x00-/:-@\[-\^`{-\x7f\u{80}-\u{d7ff}\u{e000}-\u{10ffff}])");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?-u)\d+\s")])rs").expression, "[0-9]+[\\t-\\r ]");
    EXPECT_EQ(rule_of(R"rs(#[regex(br"(?i)[[^b]]")])rs").expression, "[\\x00-AC-ac-\\xff]");
    EXPECT_EQ(rule_of(R"rs(#[regex(br"(?i)[[:^lower:]]x")])rs").expression, "[\\x00-@\\[-`{-\\xff][Xx]");
    EXPECT_EQ(rule_of(R"rs(#[regex(b"\xFF[^\n]")])rs").expression, "\"\\xff\"[\\x00-\\t\\x0b-\\xff]");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"[]a-]")])rs").expression, "[\\-\\]a]");
}

TEST(Read_logos, Refusals_name_the_line_and_the_construct)
{
    const auto refused{[](const std::string_view attribute) {
        return line_of("#[derive(Logos)]\nenum T {\n" + std::string{attribute} + "\n    V,\n}\n");
    }};

    // Needing tables the library has not got: the property classes, and a non-ASCII scalar under `i`.
    EXPECT_EQ(refused(R"rs(#[regex(r"\p{Letter}")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex(r"[\P{L}]")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex(r"(?i)é")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[token("élan", ignore(case))])rs"), 3);

    // Conditions on the context, which a token language cannot say.
    EXPECT_EQ(refused(R"rs(#[regex(r"^a")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex(r"a$")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex(r"\ba")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex(r"a(?=b)")])rs"), 3);

    // The flags and operators not modelled, the arguments logos has not got, and the empty pattern.
    EXPECT_EQ(refused(R"rs(#[regex(r"(?x) a b")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex(r"[a-z&&[^aeiou]]")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex(r"(?-u)[\d-z]")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex("a", greedy = true)])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex("a", priority = 2, cb)])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[token("")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex("(?&nope)")])rs"), 3);

    // Malformed Rust around the attributes, at the line it goes wrong; an enum left open is refused on the last
    // line there is.
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T {\n    #[token(\"a\")]\n    V\n    W,\n}\n"), 5);
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T {\n    #[token(\"a\")]\n    V,\n"), 4);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(subpattern d = r\"[0-9]\", subpattern d = r\"x\")]\nenum T {}\n"), 2);

    // The message names the pattern as written and what was refused.
    try
    {
        std::ignore = read_logos("#[derive(Logos)]\nenum T {\n    #[regex(r\"\\p{L}+\")]\n    V,\n}\n");

        FAIL() << "a Unicode property class must be refused";
    }
    catch (const Spec_error& error)
    {
        EXPECT_NE(std::string_view{error.what()}.find(R"(r"\p{L}+")"), std::string_view::npos) << error.what();
        EXPECT_NE(std::string_view{error.what()}.find("property class"), std::string_view::npos) << error.what();
    }
}

TEST(Read_logos, Unicode_perl_classes_are_the_crates_at_the_version_the_crate_pins)
{
    // logos 0.15.1 is locked to regex-syntax 0.8.11, whose tables are the Unicode 16.0.0 database's, so that is the
    // language the reader models, and it says so among the scanner's options. The library pins a later database, in
    // which U+11DE0 is a decimal digit; logos, asked for `\d`, matches nothing there, as running it shows.
    const auto digits{rule_of(R"rs(#[regex(r"\d+")])rs")};

    EXPECT_EQ(
            read_logos("#[derive(Logos)]\nenum T {\n    V,\n}\n").front().options,
            (std::vector<std::string>{"unicode-classes=16.0.0"}));
    EXPECT_EQ(digits.expression.find(R"(\u{11de0})"), std::string::npos);

    // Built, the digit class takes the ASCII digit and leaves that code point's encoding to its negation.
    const auto lexer_of{[](const std::string_view attribute) {
        return build(
                read_logos("#[derive(Logos)]\nenum T {\n" + std::string{attribute} + "\n    V,\n}\n").front(),
                "INITIAL");
    }};

    const auto digit{lexer_of(R"rs(#[regex(r"\d+")])rs")};

    EXPECT_EQ(digit.tokenize<std::size_t>(std::string{"7"}).length, 1u);
    EXPECT_EQ(digit.tokenize<std::size_t>(std::string{"\xf0\x91\xb7\xa0"}).length, 0u);
    EXPECT_EQ(lexer_of(R"rs(#[regex(r"\D")])rs").tokenize<std::size_t>(std::string{"\xf0\x91\xb7\xa0"}).length, 4u);
    EXPECT_EQ(lexer_of(R"rs(#[regex(r"\W")])rs").tokenize<std::size_t>(std::string{"\xf0\x91\xb7\xa0"}).length, 4u);

    // `\s` is White_Space: the ASCII run, the next line, the no-break space and the other separators, twenty-five
    // scalars in all, written out; `\d` is Nd and `\w` the word class, whose first rows past ASCII are the
    // Arabic-Indic digits and the feminine ordinal indicator, and whose negations begin at the null byte.
    EXPECT_EQ(
            rule_of(R"rs(#[regex(r"\s+")])rs").expression,
            R"([\t-\r \u{85}\u{a0}\u{1680}\u{2000}-\u{200a}\u{2028}-\u{2029}\u{202f}\u{205f}\u{3000}]+)");
    EXPECT_TRUE(rule_of(R"rs(#[regex(r"\d+")])rs").expression.starts_with(R"([0-9\u{660}-\u{669}\u{6f0}-\u{6f9})"));
    EXPECT_TRUE(rule_of(R"rs(#[regex(r"[\w]")])rs").expression.starts_with(R"([0-9A-Z_a-z\u{aa}\u{b5}\u{ba})"));
    EXPECT_TRUE(rule_of(R"rs(#[regex(r"\D")])rs").expression.starts_with(R"([\x00-/:-\x7f\u{80}-\u{65f})"));
    EXPECT_TRUE(
            rule_of(R"rs(#[regex(r"[^\s]")])rs").expression.starts_with(R"([\x00-\x08\x0e-\x1f!-\x7f\u{80}-\u{84})"));

    // Under `(?-u)` the same escapes are their ASCII forms, as before.
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?-u)\w+")])rs").expression, "[0-9A-Z_a-z]+");
}

TEST(Read_logos, A_subpattern_is_read_in_the_mode_of_the_pattern_referencing_it)
{
    // logos substitutes a subpattern's text, a byte string's bytes beyond ASCII spelled `\xHH`, into the pattern
    // before the crate parses it, so the definition is read in the referencing pattern's mode: `b"\xc3"` referenced
    // from a string pattern is the scalar U+00C3, which logos 0.15.1 matches as its two bytes, a byte class the
    // scalars of its range, a byte dot any scalar but the newline, and under `ignore(case)` the string pattern's
    // Unicode folding, the Kelvin sign with `k`, while a string's `\xe9` referenced from a byte pattern is that byte
    // and its typed scalar its UTF-8. The priority follows the mode too: two scalars count four, the crate's conflict
    // check answering so.
    const auto first_rule{[](const std::string_view definition, const std::string_view attribute) {
        return read_logos(
                       "#[derive(Logos)]\n#[logos(subpattern " + std::string{definition} + ")]\nenum T {\n" +
                       std::string{attribute} + "\n    V,\n}\n")
                .front()
                .rules.front();
    }};

    EXPECT_EQ(
            first_rule(R"rs(hi = b"\xc3")rs", R"rs(#[regex("(?&hi)")])rs").expression,
            rule_of(R"rs(#[regex("\u{c3}")])rs").expression);
    EXPECT_EQ(
            first_rule(R"rs(hi = b"\xc3\xa9")rs", R"rs(#[regex("(?&hi)")])rs").priority, std::optional<std::size_t>{4});
    EXPECT_EQ(
            first_rule(R"rs(hb = b"[\x80-\xff]")rs", R"rs(#[regex("(?&hb)+")])rs").expression,
            rule_of(R"rs(#[regex("[\u{80}-\u{ff}]+")])rs").expression);
    EXPECT_EQ(
            first_rule(R"rs(any = b".")rs", R"rs(#[regex("x(?&any)")])rs").expression,
            rule_of(R"rs(#[regex("x.")])rs").expression);
    EXPECT_EQ(first_rule(R"rs(k = b"k")rs", R"rs(#[regex("(?&k)", ignore(case))])rs").expression, R"([Kk\u{212a}])");
    EXPECT_EQ(first_rule(R"rs(e = r"\xe9")rs", R"rs(#[regex(b"(?&e)")])rs").expression, "\"\\xe9\"");
    EXPECT_EQ(first_rule(R"rs(e = "é")rs", R"rs(#[regex(b"(?&e)")])rs").expression, "\"\\xc3\\xa9\"");
    EXPECT_EQ(
            first_rule(R"rs(any = ".")rs", R"rs(#[regex(b"x(?&any)")])rs").expression,
            rule_of(R"rs(#[regex(b"x.")])rs").expression);

    // In its own mode under no flag the reference keeps the definition's name and priority, the definition compiled
    // in that mode.
    const auto own{read_logos(R"rs(#[derive(Logos)]
#[logos(subpattern hi = b"\xc3\xa9")]
enum T {
    #[regex(b"(?&hi)")]
    V,
}
)rs")
                           .front()};

    EXPECT_EQ(own.rules.front().expression, "{hi}");
    EXPECT_EQ(own.rules.front().priority, std::optional<std::size_t>{2});
    EXPECT_EQ(own.definitions.at("hi"), "\"\\xc3\\xa9\"");

    // A string's class with a non-ASCII member is valid where it is declared and refused, as the crate refuses it,
    // where a byte pattern references it.
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(subpattern e = \"[é]\")]\nenum T {\n    #[regex(b\"(?&e)\")]\n"
                    "    V,\n}\n"),
            4);
}

TEST(Read_logos, An_empty_subpattern_is_valid_and_emptiness_is_decided_on_the_rule)
{
    // logos 0.15.1 takes an empty definition, a pattern pasting it in matching what the rest of it matches, so `x`
    // is the token V to it; the reference expands to nothing, since regex::parse() takes no empty definition. A
    // rule that is empty once its subpatterns are pasted in is no token to logos: it panics on an empty token and
    // compiles an empty regex, or an empty skip, into a rule matching no input, so each is refused at its own line.
    const auto spec{read_logos(R"rs(#[derive(Logos)]
#[logos(subpattern empty = "")]
enum T {
    #[regex("x(?&empty)")]
    V,
    #[regex("[a-w]+")]
    Word,
}
)rs")
                            .front()};

    ASSERT_EQ(spec.rules.size(), 2u);
    EXPECT_EQ(spec.rules.front().expression, "\"x\"");
    EXPECT_EQ(spec.rules.front().priority, std::optional<std::size_t>{2});
    EXPECT_EQ(build(spec, "INITIAL").tokenize<std::size_t>(std::string{"xa"}).token, std::optional<std::size_t>{0});
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(subpattern empty = \"\")]\nenum T {\n    V,\n}\n"), -1);

    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(subpattern empty = \"\")]\nenum T {\n    #[regex(\"(?&empty)\")]\n"
                    "    V,\n}\n"),
            4);
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T {\n    #[regex(\"\")]\n    V,\n}\n"), 3);
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T {\n    #[token(\"\")]\n    V,\n}\n"), 3);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip \"\")]\nenum T {\n    V,\n}\n"), 2);

    const auto reason{[](const std::string_view source) {
        try
        {
            std::ignore = read_logos(source);
        }
        catch (const Spec_error& error)
        {
            return std::string{error.what()};
        }

        return std::string{};
    }};

    EXPECT_NE(reason("#[derive(Logos)]\nenum T {\n    #[token(\"\")]\n    V,\n}\n").find("panics"), std::string::npos);
    EXPECT_NE(
            reason("#[derive(Logos)]\nenum T {\n    #[regex(\"\")]\n    V,\n}\n").find("matching no input"),
            std::string::npos);
}

TEST(Read_logos, A_subpattern_is_substituted_as_text_before_the_priority_is_computed)
{
    // logos pastes a subpattern's text into the pattern before the crate parses it, so the crate's merging of
    // adjacent literals runs across the reference: the UTF-8 of one scalar split between the pattern and the
    // subpattern is one scalar and counts two, while a run that is UTF-8 on its own and not once joined counts its
    // bytes. logos 0.15.1's conflict hint names each of these priorities one higher, `priority = 3` for the first
    // and third and `priority = 7` for the second, and a capture around the reference keeps the runs apart, four,
    // which lets that pair build.
    const auto first_rule{[](const std::string_view definitions, const std::string_view attribute) {
        return read_logos(
                       "#[derive(Logos)]\n" + std::string{definitions} + "enum T {\n" + std::string{attribute} +
                       "\n    V,\n}\n")
                .front()
                .rules.front();
    }};

    constexpr std::string_view low{"#[logos(subpattern lo = b\"\\xA9\")]\n"};

    EXPECT_EQ(first_rule(low, R"rs(#[regex(b"\xC3(?&lo)")])rs").priority, std::optional<std::size_t>{2});
    EXPECT_EQ(first_rule(low, R"rs(#[regex(b"\xC3\xA9(?&lo)")])rs").priority, std::optional<std::size_t>{6});
    EXPECT_EQ(
            first_rule(
                    "#[logos(subpattern hi = b\"\\xC3\")]\n#[logos(subpattern lo = b\"\\xA9\")]\n",
                    R"rs(#[regex(b"(?&hi)(?&lo)")])rs")
                    .priority,
            std::optional<std::size_t>{2});
    EXPECT_EQ(first_rule(low, R"rs(#[regex(b"\xC3((?&lo))")])rs").priority, std::optional<std::size_t>{4});

    // The expression still names the definition where the reference stands in its own mode.
    EXPECT_EQ(first_rule(low, R"rs(#[regex(b"\xC3(?&lo)")])rs").expression, "\"\\xc3\"{lo}");

    // A string's scalar pasted into a byte pattern is its UTF-8 and counts two as well, and a flag before the
    // reference reaches in: `(?i)(?&k)` is a class, two.
    EXPECT_EQ(
            first_rule("#[logos(subpattern e = \"\\u{e9}\")]\n", R"rs(#[regex(b"(?&e)")])rs").priority,
            std::optional<std::size_t>{2});
    EXPECT_EQ(
            first_rule("#[logos(subpattern k = \"k\")]\n", R"rs(#[regex("(?i)(?&k)")])rs").priority,
            std::optional<std::size_t>{2});

    // Ranked against a whole-scalar rule at priority three, the split one loses on the scalar, as logos answers.
    const auto ranked{read_logos(R"rs(#[derive(Logos)]
#[logos(subpattern lo = b"\xA9")]
enum T {
    #[regex(b"\xC3(?&lo)")]
    Split,
    #[regex(b"\xC3\xA9", priority = 3)]
    Whole,
}
)rs")
                              .front()};

    EXPECT_EQ(
            build(ranked, "INITIAL").tokenize<std::size_t>(std::string{"\xc3\xa9"}).token,
            std::optional<std::size_t>{1});
}

TEST(Read_logos, What_logos_refuses_in_a_pattern_is_refused)
{
    const auto line_of_attribute{[](const std::string_view attribute) {
        return line_of("#[derive(Logos)]\nenum T {\n    " + std::string{attribute} + "\n    V,\n}\n");
    }};

    // The lazy operators: logos 0.15.1 answers "non-greedy parsing is currently unsupported" to every one, in a
    // regex, a skip, a subpattern and a byte pattern alike, so none is read as its greedy form; a token is a literal
    // and keeps its question mark.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a*?")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a+?")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a??")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a{1,3}?")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"a+?")])rs"), 3);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip r\"a+?\")]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(subpattern a = r\"a+?\")]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(rule_of(R"rs(#[token("a*?")])rs").expression, "\"a*?\"");

    // The dot under `s`, and any class of every scalar or every byte, under `*`, `+`, `{0,}` or `{1,}`: the crate
    // refuses these as consuming the source to its end, wherever they stand, under either ignore flag, and through
    // a subpattern. The plain dot leaves out the newline and passes, as do `?`, a bounded count, a count from two,
    // a token's escaped text, and a captured dot, which the crate's comparison never sees through.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).+")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).{0,}")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).{1,}")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s:.)*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?is).*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a|(?s).*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"((?s).*)")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[\s\S]*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[\d\D]*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[\x00-\x{10FFFF}]*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).*", ignore(case))])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).*", ignore(ascii_case))])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"(?s).*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"(?s).+")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"[\x00-\xff]*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"(?u)(?s).*")])rs"), 3);
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(subpattern any = r\"(?s).\")]\nenum T {\n    #[regex(r\"(?&any)*\")]\n"
                    "    V,\n}\n"),
            4);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r".*")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r".+")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br".*")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).?")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).{0,3}")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).{2,}")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s)(.)*")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s)(.)+")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[token(".*", ignore(case))])rs"), -1);

    // The regex crate merges an alternation of classes into one class, and one of single characters into the class
    // of them, before logos looks, so those are the dot when their union is everything; a class alternated with a
    // literal stays an alternation, a captured alternation stays captured, and `[\x00-\x{D7FF}\x{E000}-\x{10FFFF}]`
    // is two ranges to the crate where its dot is one, so each of those builds.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s)(?:.|.)+")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a(?:[^\n]|[\n\r])*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?:[a-z]|[^a-z])+")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?:(?:\n|\r)|[^\n\r])+")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"a(?:[\x00-\x7f]|[\x80-\xff])*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a(?:[^\n]|\n)*")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?:\n|\r|[^\n\r])+")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s)([^\n]|[\n\r])+")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[\x00-\x{D7FF}\x{E000}-\x{10FFFF}]+")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[\x00-\x{10FFFF}]+")])rs"), 3);

    // An ignore flag on a skip: logos 0.15.1 knows callback and priority there and calls ignore an unknown nested
    // attribute, whichever flag it names.
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip(\"a\", ignore(case)))]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip(\"a\", ignore(ascii_case)))]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip(\"a\", priority = 3))]\nenum T {\n    V,\n}\n"), -1);
}

TEST(Read_logos, A_callback_is_read_by_what_it_visibly_returns)
{
    // logos decides by the callback's type: Skip, Result<Skip, E> and the Skip arms of Filter and FilterResult
    // discard the match, anything else leaves the variant's token or an error at the same boundary, and the enum
    // returned is the token (logos 0.15.1, src/internal.rs). The reading has the text: a closure by every result its
    // body produces, through blocks, returns, ifs and matches, and a function this file defines, before or after the
    // enum, in a module or an impl block, by its return type. Each file here derives with logos 0.15.1, a payload
    // variant carrying the callbacks that return one, and the crate discards on each of the first group, emits V, or
    // an error at V's boundary for `erring`, on each of the second and Other on each of the third, as running it
    // shows; the two bare `Filter::Skip` and `FilterResult::Skip` closures are the exception, which rustc refuses
    // alone as needing a type annotation and compiles once another path fixes the `Emit` type, skipping on that arm.
    const auto file_of{[](const std::string_view attribute, const std::string_view after,
                          const std::string_view variant) {
        return "use logos::{Filter, FilterResult, Lexer, Logos, Skip};\n#[derive(Logos)]\n#[logos(extras = usize)]\n"
               "enum T {\n    " +
               std::string{attribute} + "\n    " + std::string{variant} + ",\n    Other,\n}\n" + std::string{after};
    }};

    const auto token_of{[&file_of](
                                const std::string_view attribute, const std::string_view after = "",
                                const std::string_view variant = "V") {
        return read_logos(file_of(attribute, after, variant)).front().rules.front().token;
    }};

    const auto refused_at{[&file_of](const std::string_view attribute, const std::string_view after = "") {
        return line_of(file_of(attribute, after, "V"));
    }};

    const std::optional<std::string> discarded;

    EXPECT_EQ(token_of(R"rs(#[token("#", |_| { return logos::Skip; })])rs"), discarded);
    EXPECT_EQ(token_of(R"rs(#[token("#", |lex| { lex.extras += 1; logos::Skip })])rs"), discarded);
    EXPECT_EQ(token_of(R"rs(#[token("#", |_| Skip)])rs"), discarded);
    EXPECT_EQ(token_of(R"rs(#[token("#", callback = |_| ::logos::Skip)])rs"), discarded);
    EXPECT_EQ(token_of(R"rs(#[token("#", |_| Filter::Skip)])rs"), discarded);
    EXPECT_EQ(token_of(R"rs(#[token("#", |_| logos::FilterResult::Skip)])rs"), discarded);
    EXPECT_EQ(token_of(R"rs(#[token("#", |lex| if lex.extras > 0 { return Skip; } else { Skip })])rs"), discarded);
    EXPECT_EQ(
            token_of(R"rs(#[regex("[#@]", |lex| match lex.slice() { "#" => Skip, _ => { return logos::Skip; } })])rs"),
            discarded);
    EXPECT_EQ(
            token_of(R"rs(#[token("#", drop_it)])rs", "fn drop_it(_lex: &mut Lexer<T>) -> Skip { Skip }\n"), discarded);
    EXPECT_EQ(
            token_of(
                    R"rs(#[token("#", later)])rs",
                    "fn later<'a>(_lex: &mut Lexer<'a, T>) -> logos::Skip where { logos::Skip }\n"),
            discarded);
    EXPECT_EQ(
            token_of(R"rs(#[token("#", tried)])rs", "fn tried(_lex: &mut Lexer<T>) -> Result<Skip, ()> { Ok(Skip) }\n"),
            discarded);
    EXPECT_EQ(
            token_of(
                    R"rs(#[token("#", callbacks::drop_it)])rs",
                    "mod callbacks {\n    pub fn drop_it(_lex: &mut logos::Lexer<super::T>) -> logos::Skip { "
                    "logos::Skip "
                    "}\n}\n"),
            discarded);

    // A comment inside a return type, a result or a path is trivia to Rust and read past here, so each of these
    // skips as logos 0.15.1 skips on them.
    EXPECT_EQ(
            token_of(
                    R"rs(#[token("#", drop_it)])rs",
                    "fn drop_it(_lex: &mut Lexer<T>) -> logos::/* c */Skip { logos::Skip }\n"),
            discarded);
    EXPECT_EQ(
            token_of(
                    R"rs(#[token("#", tried)])rs",
                    "fn tried(_lex: &mut Lexer<T>) -> Result</*c*/ logos::Skip, ()> { Ok(logos::Skip) }\n"),
            discarded);
    EXPECT_EQ(token_of(R"rs(#[token("#", |_| logos::/* c */Skip)])rs"), discarded);
    EXPECT_EQ(token_of(R"rs(#[token("#", |_| /* c */ Skip)])rs"), discarded);
    EXPECT_EQ(token_of(R"rs(#[token("#", logos:: /* c */ skip)])rs"), discarded);

    // A statement-like `if` or `match` before the tail needs no semicolon, and the tail is still the value.
    EXPECT_EQ(token_of(R"rs(#[token("#", |lex| { if lex.extras > 0 { return Skip; } Skip })])rs"), discarded);
    EXPECT_EQ(token_of(R"rs(#[token("#", |lex| { if lex.extras > 0 { lex.extras -= 1; } Skip })])rs"), discarded);
    EXPECT_EQ(
            token_of(R"rs(#[token("#", |lex| { match lex.extras { 0 => {} _ => { lex.extras -= 1; } } Skip })])rs"),
            discarded);

    const std::optional<std::string> v{"V"};

    EXPECT_EQ(token_of(R"rs(#[token("#", |lex| { lex.extras += 1; })])rs"), v);
    EXPECT_EQ(token_of(R"rs(#[regex("[0-9]+", |lex| Some(lex.slice().len()))])rs", "", "V(usize)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("#", |_| 42)])rs", "", "V(u64)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("#", |_| ())])rs"), v);
    EXPECT_EQ(token_of(R"rs(#[token("#", |_| Filter::Emit(()))])rs"), v);
    EXPECT_EQ(token_of(R"rs(#[token("#", |lex| if lex.extras > 0 { Some(()) } else { None })])rs"), v);
    EXPECT_EQ(token_of(R"rs(#[token("#", count)])rs", "fn count(lex: &mut Lexer<T>) -> bool { lex.extras > 0 }\n"), v);
    EXPECT_EQ(token_of(R"rs(#[token("#", note)])rs", "fn note(lex: &mut Lexer<T>) { lex.extras += 1; }\n"), v);
    EXPECT_EQ(
            token_of(
                    R"rs(#[token("#", always)])rs",
                    "fn always(_lex: &mut Lexer<T>) -> Filter<()> { Filter::Emit(()) }\n"),
            v);
    EXPECT_EQ(
            token_of(
                    R"rs(#[token("#", erring)])rs",
                    "fn erring(_lex: &mut Lexer<T>) -> FilterResult<(), ()> { FilterResult::Error(()) }\n"),
            v);

    const std::optional<std::string> other{"Other"};

    EXPECT_EQ(token_of(R"rs(#[token("#", |_| T::Other)])rs"), other);
    EXPECT_EQ(token_of(R"rs(#[token("#", |_| T:: /* c */ Other)])rs"), other);
    EXPECT_EQ(token_of(R"rs(#[regex("[0-9]+", |lex| { lex.extras += 1; T::Other })])rs"), other);
    EXPECT_EQ(token_of(R"rs(#[token("#", |_| Filter::Emit(T::Other))])rs"), other);
    EXPECT_EQ(token_of(R"rs(#[token("#", other)])rs", "fn other(_lex: &mut Lexer<T>) -> T { T::Other }\n"), other);

    // In one of the enum's impl blocks `Self` is the enum, in the return type and in the body; in another type's it
    // is not, and the type is a payload.
    EXPECT_EQ(
            token_of(
                    R"rs(#[token("#", T::mark)])rs",
                    "impl T {\n    fn mark(_lex: &mut Lexer<T>) -> Self { T::Other }\n}\n"),
            other);
    EXPECT_EQ(
            token_of(
                    R"rs(#[token("#", T::mark)])rs",
                    "impl T {\n    fn mark(_lex: &mut Lexer<T>) -> Filter<Self> { Filter::Emit(Self::Other) }\n}\n"),
            other);
    EXPECT_EQ(
            token_of(
                    R"rs(#[token("#", T::mark)])rs",
                    "trait Marker { fn mark(lex: &mut Lexer<T>) -> Self; }\nimpl Marker for T where T: Sized {\n"
                    "    fn mark(_lex: &mut Lexer<T>) -> Self { Self::V }\n}\n"),
            v);
    EXPECT_EQ(
            token_of(
                    R"rs(#[token("#", Wrapper::mark)])rs",
                    "struct Wrapper;\nimpl Wrapper {\n    fn mark(_lex: &mut Lexer<T>) -> Self { Wrapper }\n}\n",
                    "V(Wrapper)"),
            v);

    // Refused by name, at the rule's line: a function the file does not define or defines twice, or declares without
    // the body its type leaves the decision to; a result the text does not show, a call among them; results that
    // skip on one path and emit on another, or emit two variants; and a closure that is not one of one parameter.
    EXPECT_EQ(refused_at(R"rs(#[regex("[0-9]+k", callback = kilo)])rs"), 5);
    EXPECT_EQ(refused_at(R"rs(#[regex("[0-9]+", |lex| lex.slice().parse().ok())])rs"), 5);
    EXPECT_EQ(
            refused_at(R"rs(#[token("#", |lex| helper(lex))])rs", "fn helper(_lex: &mut Lexer<T>) -> Skip { Skip }\n"),
            5);
    EXPECT_EQ(
            refused_at(
                    R"rs(#[token("#", maybe)])rs",
                    "fn maybe(lex: &mut Lexer<T>) -> Filter<()> { if lex.extras > 0 { Filter::Skip } else { "
                    "Filter::Emit(()) } }\n"),
            5);
    EXPECT_EQ(
            refused_at(R"rs(#[token("#", |lex| if lex.extras > 0 { Filter::Skip } else { Filter::Emit(()) })])rs"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| if lex.extras > 0 { T::Other } else { T::V })])rs"), 5);
    EXPECT_EQ(
            refused_at(
                    R"rs(#[token("#", twice)])rs",
                    "fn twice(_lex: &mut Lexer<T>) -> Skip { Skip }\nmod m { pub fn twice() -> bool { true } }\n"),
            5);
    EXPECT_EQ(refused_at(R"rs(#[token("#", X::f)])rs", "trait X { fn f(lex: &mut Lexer<T>) -> Filter<()>; }\n"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |a, b| Skip)])rs"), 5);

    // A skip attribute's callback has no result to read: every result the crate admits there skips or fails.
    EXPECT_EQ(
            read_logos("#[derive(Logos)]\n#[logos(extras = usize)]\n#[logos(skip(\"x\", |lex| lex.extras += 1))]\n"
                       "enum T {\n    V,\n}\n")
                    .front()
                    .rules.front()
                    .token,
            discarded);

    // The refusal names the callback and the reason.
    try
    {
        std::ignore =
                read_logos(file_of(R"rs(#[token("#", |lex| if lex.extras > 0 { Skip } else { () })])rs", "", "V"));

        FAIL() << "a callback deciding at run time must be refused";
    }
    catch (const Spec_error& error)
    {
        const std::string_view what{error.what()};

        EXPECT_NE(what.find("|lex| if lex.extras > 0 { Skip } else { () }"), std::string_view::npos) << what;
        EXPECT_NE(what.find("run time"), std::string_view::npos) << what;
    }
}

TEST(Read_logos, A_callback_result_is_read_against_the_variants_payload)
{
    // logos 0.15.1 converts the callback's result through `CallbackResult<P, T>` for the variant's payload type P,
    // `()` for a unit variant (logos-codegen 0.15.1, generator/leaf.rs): a value of type P, bare or in `Some`, `Ok`
    // or an `Emit` arm, is the payload and emits the variant, so on `V(logos::Skip)` each of `Skip`, `Some(Skip)`,
    // `Filter::Emit(Skip)`, `Ok(Skip)` and a function returning `Skip` or `Result<Skip, ()>` emits V, `Filter::Skip`
    // alone still skips, and `V(())` is a unit to it; on a unit variant `Some(Skip)` and `Filter::Emit(Skip)` are
    // type errors, as are `Skip`, `()`, `true`, `false` and the enum on `V(u64)` and a literal on a unit variant,
    // which the crate refuses and the reading refuses by name. Running the crate answers so on each, `Ok(Skip)`
    // once its error type is spelled, which rustc cannot infer from the bare closure.
    const auto file_of{[](const std::string_view attribute, const std::string_view after,
                          const std::string_view variant) {
        return "use logos::{Filter, Lexer, Logos, Skip};\n#[derive(Logos)]\n#[logos(extras = usize)]\nenum T {\n    " +
               std::string{attribute} + "\n    " + std::string{variant} + ",\n    Other,\n}\n" + std::string{after};
    }};

    const auto token_of{[&file_of](
                                const std::string_view attribute, const std::string_view variant,
                                const std::string_view after = "") {
        return read_logos(file_of(attribute, after, variant)).front().rules.front().token;
    }};

    const auto refused_at{
            [&file_of](
                    const std::string_view attribute, const std::string_view variant,
                    const std::string_view after = "") { return line_of(file_of(attribute, after, variant)); }};

    const std::optional<std::string> discarded;

    const std::optional<std::string> v{"V"};

    EXPECT_EQ(token_of(R"rs(#[token("x", |_| logos::Skip)])rs", "V(logos::Skip)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| Some(logos::Skip))])rs", "V(logos::Skip)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| logos::Filter::Emit(logos::Skip))])rs", "V(logos::Skip)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| Ok(logos::Skip))])rs", "V(logos::Skip)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| Skip)])rs", "V(Skip)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| logos::Filter::Skip)])rs", "V(logos::Skip)"), discarded);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| logos::Skip)])rs", "V(())"), discarded);
    EXPECT_EQ(
            token_of(R"rs(#[token("x", drop_it)])rs", "V(Skip)", "fn drop_it(_lex: &mut Lexer<T>) -> Skip { Skip }\n"),
            v);
    EXPECT_EQ(
            token_of(
                    R"rs(#[token("x", tried)])rs", "V(Skip)",
                    "fn tried(_lex: &mut Lexer<T>) -> Result<Skip, ()> { Ok(Skip) }\n"),
            v);
    EXPECT_EQ(
            token_of(R"rs(#[token("x", drop_it)])rs", "V", "fn drop_it(_lex: &mut Lexer<T>) -> Skip { Skip }\n"),
            discarded);

    // A payload of another type takes the payload values as before, a skip arm still skips, and `None` and `Err` are
    // an error at the boundary whatever the variant.
    EXPECT_EQ(token_of(R"rs(#[regex("[0-9]+", |lex| Some(lex.slice().len()))])rs", "V(usize)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| 42)])rs", "V(u64)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| Filter::Emit(42))])rs", "V(u64)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| Filter::Skip)])rs", "V(u64)"), discarded);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| None)])rs", "V(u64)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| Err(()))])rs", "V(u64)"), v);
    EXPECT_EQ(token_of(R"rs(#[token("x", |_| None)])rs", "V"), v);

    // What the crate refuses for the variant's payload is refused at the rule's line, in a closure and by a function's
    // return type alike.
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| logos::Skip)])rs", "V(u64)"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| Ok(logos::Skip))])rs", "V(u64)"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| T::Other)])rs", "V(u64)"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| Filter::Emit(T::Other))])rs", "V(u64)"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |lex| { lex.extras += 1; })])rs", "V(u64)"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| ())])rs", "V(u64)"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| Some(()))])rs", "V(u64)"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| true)])rs", "V(u64)"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| Some(logos::Skip))])rs", "V"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| logos::Filter::Emit(logos::Skip))])rs", "V"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| Some(T::Other))])rs", "V"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| "payload")])rs", "V"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| Some(42))])rs", "V"), 5);
    EXPECT_EQ(refused_at(R"rs(#[token("x", |_| Ok(Filter::Skip))])rs", "V"), 5);
    EXPECT_EQ(
            refused_at(R"rs(#[token("x", drop_it)])rs", "V(u64)", "fn drop_it(_lex: &mut Lexer<T>) -> Skip { Skip }\n"),
            5);
    EXPECT_EQ(
            refused_at(R"rs(#[token("x", maybe)])rs", "V", "fn maybe(_lex: &mut Lexer<T>) -> Option<Skip> { None }\n"),
            5);
    EXPECT_EQ(
            refused_at(R"rs(#[token("x", count)])rs", "V(u64)", "fn count(lex: &mut Lexer<T>) -> bool { true }\n"), 5);
    EXPECT_EQ(
            refused_at(R"rs(#[token("x", note)])rs", "V(u64)", "fn note(lex: &mut Lexer<T>) { lex.extras += 1; }\n"),
            5);
    EXPECT_EQ(
            refused_at(R"rs(#[token("x", other)])rs", "V(u64)", "fn other(_lex: &mut Lexer<T>) -> T { T::Other }\n"),
            5);

    // The refusal names the variant with its payload.
    try
    {
        std::ignore = read_logos(file_of(R"rs(#[token("x", |_| logos::Skip)])rs", "", "V(u64)"));

        FAIL() << "a Skip returned to a variant with another payload must be refused";
    }
    catch (const Spec_error& error)
    {
        EXPECT_NE(std::string_view{error.what()}.find("V(u64)"), std::string_view::npos) << error.what();
    }
}

TEST(Read_logos, A_callback_is_read_through_the_names_the_file_binds)
{
    // The crate's names reach a callback as the file binds them: `use logos as lx` with `#[logos(crate = lx)]`,
    // `use logos::Skip as Drop`, a `type` alias of `Skip` or of `Result<Skip, ()>`, and a local `fn skip` in place
    // of the crate's are each read as what they stand for, and a local `struct Skip` is a payload, which logos
    // 0.15.1 emits on `V(Skip)` and refuses on a unit variant, while a call of an associated function, `T::make(0)`,
    // names no variant and is out of sight. Running the crate answers so on each.
    const auto file_of{[](const std::string_view before, const std::string_view attribute,
                          const std::string_view variant, const std::string_view after) {
        return std::string{before} + "#[derive(Logos)]\nenum T {\n    " + std::string{attribute} + "\n    " +
               std::string{variant} + ",\n    Other,\n}\n" + std::string{after};
    }};

    const auto token_of{[&file_of](
                                const std::string_view before, const std::string_view attribute,
                                const std::string_view variant = "V", const std::string_view after = "") {
        return read_logos(file_of(before, attribute, variant, after)).front().rules.front().token;
    }};

    const std::optional<std::string> discarded;

    const std::optional<std::string> v{"V"};

    EXPECT_EQ(
            token_of("use logos as lx;\nuse lx::Logos;\n#[logos(crate = lx)]\n", R"rs(#[token("x", |_| lx::Skip)])rs"),
            discarded);
    EXPECT_EQ(
            token_of("use logos as lx;\nuse lx::Logos;\n#[logos(crate = lx)]\n", R"rs(#[token("x", lx::skip)])rs"),
            discarded);
    EXPECT_EQ(token_of("use logos::{Logos, Skip as Drop};\n", R"rs(#[token("x", |_| Drop)])rs"), discarded);
    EXPECT_EQ(token_of("use logos::Logos;\n", R"rs(#[token("x", |_| logos::Skip)])rs"), discarded);
    EXPECT_EQ(
            token_of(
                    "use logos::{Lexer, Logos};\ntype Discard = logos::Skip;\n", R"rs(#[token("x", drop_it)])rs", "V",
                    "fn drop_it(_lex: &mut Lexer<T>) -> Discard { logos::Skip }\n"),
            discarded);
    EXPECT_EQ(
            token_of(
                    "use logos::{Lexer, Logos};\ntype Discard = logos::Skip;\n", R"rs(#[token("x", drop_it)])rs",
                    "V(Discard)", "fn drop_it(_lex: &mut Lexer<T>) -> Discard { logos::Skip }\n"),
            v);
    EXPECT_EQ(
            token_of(
                    "use logos::{Lexer, Logos};\ntype Outcome = Result<logos::Skip, ()>;\n",
                    R"rs(#[token("x", tried)])rs", "V",
                    "fn tried(_lex: &mut Lexer<T>) -> Outcome { Ok(logos::Skip) }\n"),
            discarded);
    EXPECT_EQ(
            token_of(
                    "use logos::{Lexer, Logos};\nfn skip(_lex: &mut Lexer<T>) -> bool { true }\n",
                    R"rs(#[token("x", skip)])rs"),
            v);
    EXPECT_EQ(token_of("use logos::Logos;\nstruct Skip;\n", R"rs(#[token("x", |_| Skip)])rs", "V(Skip)"), v);

    EXPECT_EQ(line_of(file_of("use logos::Logos;\nstruct Skip;\n", R"rs(#[token("x", |_| Skip)])rs", "V", "")), 5);
    EXPECT_EQ(
            line_of(
                    file_of("use logos::Logos;\nmod logos { pub struct Skip; }\n",
                            R"rs(#[token("x", |_| logos::Skip)])rs", "V", "")),
            5);
    EXPECT_EQ(
            line_of(
                    file_of("use logos::Logos;\n", R"rs(#[token("x", |_| T::make(0))])rs", "V",
                            "impl T {\n    fn make(_n: u8) -> T { T::Other }\n}\n")),
            4);
}

TEST(Read_logos, What_the_crate_refuses_of_attributes_and_variants_is_refused)
{
    // Each of these is a file logos 0.15.1 refuses, in the words quoted, as building it shows; the reading refuses
    // it at the line of the attribute or variant in question rather than read a scanner the crate never makes. The
    // last group builds and is read.
    const auto enum_of{[](const std::string_view head, const std::string_view body) {
        return std::string{head} + "enum T {\n" + std::string{body} + "\n}\n";
    }};

    constexpr std::string_view word{R"rs(    #[regex("[a-w]+")]
    Word,)rs"};

    // "Since 0.13 Logos no longer requires the #[error] variant", "Logos currently only supports variants with one
    // field, found 2" and "Logos doesn't support named fields yet", at the variant.
    EXPECT_EQ(
            line_of(enum_of("#[derive(Logos)]\n", "    #[regex(\"[a-w]+\")]\n    Word,\n    #[error]\n    Error,")), 5);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", "    #[regex(\"[a-w]+\")]\n    Word,\n    Pair(u8, u8),")), 5);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", "    #[regex(\"[a-w]+\")]\n    Word,\n    Named { a: u8 },")), 5);

    // "Resetting previously set priority", "Callback has been already set", "Expected: priority = <integer>",
    // "Expected: callback = ..." and, for anything after `ignore(...)`, whose closing comma the crate leaves unread,
    // "Expected a named argument at this position".
    EXPECT_EQ(
            line_of(enum_of("#[derive(Logos)]\n", R"rs(    #[regex("[a-w]+", priority = 1, priority = 2)]
    Word,)rs")),
            3);
    EXPECT_EQ(
            line_of(enum_of("#[derive(Logos)]\n", R"rs(    #[regex("[a-w]+", |_| (), callback = |_| ())]
    Word,)rs")),
            3);
    EXPECT_EQ(
            line_of(enum_of("#[derive(Logos)]\n", R"rs(    #[regex("[a-w]+", callback = |_| (), callback = |_| ())]
    Word,)rs")),
            3);
    EXPECT_EQ(
            line_of(enum_of("#[derive(Logos)]\n", R"rs(    #[regex("[a-w]+", priority(3))]
    Word,)rs")),
            3);
    EXPECT_EQ(
            line_of(enum_of("#[derive(Logos)]\n", R"rs(    #[regex("[a-w]+", callback(logos::skip))]
    Word,)rs")),
            3);
    EXPECT_EQ(
            line_of(enum_of("#[derive(Logos)]\n", R"rs(    #[regex("[a-w]+", ignore(case), priority = 3)]
    Word,)rs")),
            3);
    EXPECT_EQ(
            line_of(enum_of("#[derive(Logos)]\n", R"rs(    #[regex("[a-w]+", ignore(case), ignore(case))]
    Word,)rs")),
            3);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(skip(\"x\", priority = 1, priority = 2))]\n", word)), 2);

    // "Expected #[token(...)]" for the attribute without its parentheses or with nothing in them, and the same for
    // `#[logos]`; "Invalid nested attribute" for a bare key; the shape each key expects for another shape; and
    // "can be defined only once" for `extras`, `error`, `source` and the type of one parameter given twice, across
    // attributes as within one.
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", "    #[token]\n    V,\n" + std::string{word})), 3);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", "    #[token = \"x\"]\n    V,\n" + std::string{word})), 3);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", "    #[regex()]\n    V,\n" + std::string{word})), 3);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos = \"x\"]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(extras)]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(skip)]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(extras(usize))]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(source(str))]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(crate(logos))]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(skip = \"x\")]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(skip())]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(type = u8)]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(subpattern = \"x\")]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(extras =)]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(extras = usize)]\n#[logos(extras = u8)]\n", word)), 3);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(extras = usize, extras = u8)]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(source = str, source = str)]\n", word)), 2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(error = ())]\n#[logos(error = ())]\n", word)), 3);

    // The generics: "S can only have one type assigned to it", "S is not a declared type parameter", "Generic type
    // parameter without a concrete type", "Logos types can only have one lifetime" and "Logos doesn't support const
    // generics."
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(type S = u8, type S = u16)]\nenum T<S> {\n    #[regex(\"[a-w]+\", |_| "
                    "1)]\n    Word(S),\n}\n"),
            2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(type S = u8)]\n", word)), 1);
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T<S> {\n    #[regex(\"[a-w]+\", |_| 1)]\n    Word(S),\n}\n"), 1);
    EXPECT_EQ(
            line_of("#[derive(Logos)]\nenum T<'a, 'b> {\n    #[regex(\"[a-w]+\", |lex| lex.slice())]\n    Word(&'a "
                    "str),\n    #[regex(\"[0-9]+\", |lex| lex.slice())]\n    Num(&'b str),\n}\n"),
            1);
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T<const N: usize> {\n    #[regex(\"[a-w]+\")]\n    Word,\n}\n"), 1);

    // What the crate takes is read: `#[logos()]`, `crate` twice, a trailing comma, a field attribute, a discriminant,
    // `priority` before `ignore(...)`, and one lifetime with a type parameter assigned.
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos()]\n", word)), -1);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(crate = logos, crate = logos)]\n", word)), -1);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", "    #[token(\"x\",)]\n    V,\n" + std::string{word})), -1);
    EXPECT_EQ(
            line_of(enum_of(
                    "#[derive(Logos)]\n", "    #[regex(\"[a-w]+\", |_| 1)]\n    Word(#[allow(dead_code)] u8,),")),
            -1);
    EXPECT_EQ(
            line_of(enum_of(
                    "#[derive(Logos)]\n#[repr(u8)]\n", "    #[regex(\"[a-w]+\")]\n    Word = 1,\n    Other = 2,")),
            -1);
    EXPECT_EQ(
            read_logos(enum_of("#[derive(Logos)]\n", R"rs(    #[regex("[a-w]+", priority = 3, ignore(case))]
    Word,)rs"))
                    .front()
                    .rules.front()
                    .priority,
            std::optional<std::size_t>{3});
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(type S = u8)]\nenum T<'a, S> {\n    #[regex(\"[a-w]+\", |_| \"x\")]\n"
                    "    Word(&'a str),\n    #[regex(\"[0-9]+\", |_| 1)]\n    Num(S),\n}\n"),
            -1);
}

TEST(Read_logos, A_callback_that_moves_the_lexer_is_refused)
{
    // logos 0.15.1 hands the callback the lexer, whose `bump` extends the match, and whose internal
    // `bump_unchecked`, `trivia`, `error`, `end` and `set` move it too once the trait is imported: on `#y` the
    // closure `|lex| { lex.bump(1); }` under `#[token("#")]` gives V the span 0..2 and a skip's callback bumping
    // the same way discards both bytes, as running the crate shows. A body is read only where its lexer parameter
    // is used through `slice`, `span`, `remainder`, `source`, `extras` or `clone`; one naming a moving method, or
    // letting the parameter out of sight, is refused by name at the rule's line, in a closure, in a function the
    // callback names and in a skip's callback alike, as is a function declared without a body.
    const auto file_of{[](const std::string_view attribute, const std::string_view after) {
        return "use logos::{Lexer, Logos, Skip};\nuse logos::internal::LexerInternal;\n#[derive(Logos)]\n"
               "#[logos(extras = usize)]\nenum T {\n    " +
               std::string{attribute} + "\n    V,\n    #[regex(\"[a-z]+\")]\n    Word,\n}\n" + std::string{after};
    }};

    const auto refused_at{[&file_of](const std::string_view attribute, const std::string_view after = "") {
        return line_of(file_of(attribute, after));
    }};

    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.bump(1); })])rs"), 6);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.bump_unchecked(1); })])rs"), 6);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.trivia(); })])rs"), 6);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.end(); })])rs"), 6);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.set(Ok(T::V)); })])rs"), 6);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.clone().bump(1); })])rs"), 6);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { advance(lex); })])rs", "fn advance(lex: &mut Lexer<T>) {}\n"), 6);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { let l = lex; Skip })])rs"), 6);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { dbg!(lex); })])rs"), 6);
    EXPECT_EQ(refused_at(R"rs(#[token("#", eat)])rs", "fn eat(lex: &mut Lexer<T>) -> Skip { lex.bump(1); Skip }\n"), 6);
    EXPECT_EQ(
            refused_at(
                    R"rs(#[token("#", eat)])rs",
                    "fn eat(mut lex: &mut Lexer<T>) -> Skip { Lexer::bump(&mut lex, 1); Skip }\n"),
            6);
    EXPECT_EQ(refused_at(R"rs(#[token("#", eat)])rs", "trait X { fn eat(lex: &mut Lexer<T>) -> Skip; }\n"), 6);
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(skip(\"#\", |lex| lex.bump(1)))]\nenum T {\n    #[regex(\"[a-z]+\")]\n"
                    "    Word,\n}\n"),
            2);
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(skip(\"#\", eat))]\nenum T {\n    #[regex(\"[a-z]+\")]\n    Word,\n}\n"
                    "fn eat(lex: &mut logos::Lexer<T>) { lex.bump(1); }\n"),
            2);

    // Reading the lexer leaves the match the pattern's, so each of these is read; the crate emits V and W on them.
    const auto read{read_logos(file_of(
            R"rs(#[token("#", |lex| { let _ = (lex.slice(), lex.span(), lex.remainder()); lex.extras += 1; })])rs",
            "fn note(lex: &mut Lexer<T>) -> bool { lex.extras += lex.slice().len() + lex.span().len(); true }\n"))};

    EXPECT_EQ(read.front().rules.front().token, std::optional<std::string>{"V"});
    EXPECT_EQ(refused_at(R"rs(#[token("#", note)])rs", "fn note(lex: &mut Lexer<T>) -> bool { lex.extras > 0 }\n"), -1);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { let _ = format!("{}", lex.slice()); Skip })])rs"), -1);

    // The refusal names the method.
    try
    {
        std::ignore = read_logos(file_of(R"rs(#[token("#", |lex| { lex.bump(1); })])rs", ""));

        FAIL() << "a callback bumping the lexer must be refused";
    }
    catch (const Spec_error& error)
    {
        EXPECT_NE(std::string_view{error.what()}.find("`bump`"), std::string_view::npos) << error.what();
    }
}

TEST(Read_logos, What_the_crate_refuses_around_a_string_pattern_is_refused)
{
    const auto line_of_attribute{[](const std::string_view attribute) {
        return line_of("#[derive(Logos)]\nenum T {\n    " + std::string{attribute} + "\n    V,\n}\n");
    }};

    // logos parses a string pattern with the regex crate's UTF-8 check on, so under `(?-u)` a byte beyond ASCII,
    // alone, written in a class whatever the class comes to, admitted by a class, nested ones checked on their own,
    // by the dot or by a negated class, is "pattern can match invalid UTF-8" to the crate; ASCII
    // classes, the ASCII escapes, a folded ASCII letter and a scalar, which stays its UTF-8, pass, and a byte string's
    // pattern has no such check. A byte subpattern referenced under `(?-u)` from a string pattern is refused where it
    // is referenced. logos 0.15.1 answers so to each.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)\xc3")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u).")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s-u).*")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[^a]")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[^\x00-\x7f]")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)\D")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[^[^\x00-\x7f]]")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[^[^a]]")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[[^\x80-\xff]]")])rs"), 3);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[a[b-c]]+")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[a-z]+")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)\d+\s")])rs"), -1);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?i-u)k")])rs"), -1);
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?-u)é")])rs").expression, "\"\\xc3\\xa9\"");
    EXPECT_EQ(rule_of(R"rs(#[regex(b"\xFF[^\n]")])rs").expression, "\"\\xff\"[\\x00-\\t\\x0b-\\xff]");
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(subpattern hi = b\"\\xc3\")]\nenum T {\n    #[regex(\"(?-u)(?&hi)\")]\n"
                    "    V,\n}\n"),
            4);

    // logos 0.15.1 knows eight `#[logos(...)]` keys and calls any other an unknown nested attribute, a later crate's
    // `utf8` among them; `type S` assigns the enum's own type parameter.
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(utf8 = false)]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(frobnicate = 1)]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(
            read_logos("#[derive(Logos)]\n#[logos(crate = logos, extras = (), source = str, error = E, "
                       "export_dir = \"out\", type S = &str)]\nenum T<S> {\n    V(S),\n}\n")
                    .front()
                    .options.size(),
            7u);

    // The crate leaves the comma after a parenthesised entry unread, so an entry after `skip(...)` in one attribute
    // is an invalid nested attribute to it, while one before it is fine.
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip(\"x\"), extras = usize)]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip(\"x\"), skip(\"y\"))]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(extras = usize, skip(\"x\"))]\nenum T {\n    V,\n}\n"), -1);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip \"x\", skip \"y\")]\nenum T {\n    V,\n}\n"), -1);
}
