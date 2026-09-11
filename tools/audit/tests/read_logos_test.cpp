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
 *        token with a callback, a regex with a priority override, a callback that skips, a skip through a closure,
 *        several attributes on one variant, variants with payloads and discriminants, attributes logos ignores, and
 *        a second enum, all among the Rust the reader must step over.
 */
constexpr std::string_view idioms{R"rs(//! A lexer, with "#[derive(Logos)]" in a doc comment to step over.
use logos::{Lexer, Logos};

/* A block comment /* nesting */ with an enum Token { in it. */
fn callback<'a>(lex: &mut Lexer<'a, Token<'a>>) -> Option<&'a str> {
    let c = '"'; let s = "enum Nope {"; let r = r#"#[token("x")]"#;
    Some(lex.slice())
}

#[derive(Logos, Debug, PartialEq)]
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

    #[regex("(?&number)", |lex| lex.slice().parse().ok(), priority = 5)]
    Number(u64),

    #[token("+")]
    #[token("-")]
    Sign { kind: u8 },

    #[regex(r"/\*([^*]|\*+[^*/])*\*+/", logos::skip)]
    Comment,

    #[token("#", |_| logos::Skip)]
    Hash,

    #[regex(r"[0-9]+k", callback = kilo)]
    Kilo(u64),

    #[default]
    Error,
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
    EXPECT_EQ(spec.options, (std::vector<std::string>{"extras=Extras"}));

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
    EXPECT_EQ(spec.rules[4].action, "|lex| lex.slice().parse().ok()");
    EXPECT_EQ(spec.rules[4].priority, std::optional<std::size_t>{5});

    EXPECT_EQ(spec.rules[5].expression, R"("+")");
    EXPECT_EQ(spec.rules[5].token, std::optional<std::string>{"Sign"});
    EXPECT_EQ(spec.rules[6].expression, R"("-")");
    EXPECT_EQ(spec.rules[6].token, std::optional<std::string>{"Sign"});

    // A callback spelled logos::skip discards, as does a closure whose whole body is logos::Skip.
    EXPECT_TRUE(spec.rules[7].expression.starts_with(R"("/*"([\x00-)+-\x7f\u{80}-)")) << spec.rules[7].expression;
    EXPECT_FALSE(spec.rules[7].token.has_value());
    EXPECT_EQ(spec.rules[7].action, "logos::skip");
    EXPECT_FALSE(spec.rules[8].token.has_value());

    EXPECT_EQ(spec.rules[9].token, std::optional<std::string>{"Kilo"});
    EXPECT_EQ(spec.rules[9].action, "kilo");
    EXPECT_EQ(spec.rules[9].priority, std::optional<std::size_t>{4});

    EXPECT_EQ(active_rules(spec, "INITIAL").size(), 10u);

    // The second enum, inside a module and deriving by path.
    const auto& part{lexers.back()};

    EXPECT_EQ(part.line, 44u);
    ASSERT_EQ(part.rules.size(), 2u);
    EXPECT_EQ(part.rules[0].token, std::optional<std::string>{"Text"});
    EXPECT_EQ(part.rules[1].pattern, R"(r#"""#)");
    EXPECT_EQ(part.rules[1].expression, R"("\"")");
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
    EXPECT_EQ(priority_of(R"rs(#[regex("[a-z]+", my_callback, priority = 7)])rs"), 7u);
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

    // The other constructs: groups of every kind, a capture keeping the runs beside it apart, lazy operators,
    // counts, empty branches, ASCII classes, the ASCII forms of the Perl classes, a byte string pattern over bytes,
    // a nested class and an ASCII class folded before their negation, and the bracket's own characters.
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?P<n>a)(?:b)(?<m>c)")])rs").expression, "\"a\"\"b\"\"c\"");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"a(?:b)c")])rs").expression, "\"abc\"");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"a+?b*?c??")])rs").expression, "\"a\"+\"b\"*\"c\"?");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(ab){2,}(cd){3}(ef){1,2}")])rs").expression, "\"ab\"{2,}\"cd\"{3}\"ef\"{1,2}");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"x(a|)")])rs").expression, "\"x\"\"a\"?");
    EXPECT_EQ(
            rule_of(R"rs(#[regex(r"[[:word:]]+[[:^digit:]]")])rs").expression,
            R"([0-9A-Z_a-z]+[\x00-/:-\x7f\u{80}-\u{d7ff}\u{e000}-\u{10ffff}])");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?-u)\d+\s")])rs").expression, "[0-9]+[\\t-\\r ]");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?i-u)[[^b]]")])rs").expression, "[\\x00-AC-ac-\\xff]");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?i-u)[[:^lower:]]x")])rs").expression, "[\\x00-@\\[-`{-\\xff][Xx]");
    EXPECT_EQ(rule_of(R"rs(#[regex(b"\xFF[^\n]")])rs").expression, "\"\\xff\"[\\x00-\\t\\x0b-\\xff]");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"[]a-]")])rs").expression, "[\\-\\]a]");
}

TEST(Read_logos, Refusals_name_the_line_and_the_construct)
{
    const auto refused{[](const std::string_view attribute) {
        return line_of("#[derive(Logos)]\nenum T {\n" + std::string{attribute} + "\n    V,\n}\n");
    }};

    // Needing the Unicode tables: the Perl and Unicode classes, and a non-ASCII scalar under `i`.
    EXPECT_EQ(refused(R"rs(#[regex(r"\d+")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex(r"[\w]")])rs"), 3);
    EXPECT_EQ(refused(R"rs(#[regex(r"\p{Letter}")])rs"), 3);
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
    EXPECT_EQ(refused(R"rs(#[regex("a", ignore(ascii_case))])rs"), 3);
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
        std::ignore = read_logos("#[derive(Logos)]\nenum T {\n    #[regex(r\"\\w+\")]\n    V,\n}\n");

        FAIL() << "a Unicode word class must be refused";
    }
    catch (const Spec_error& error)
    {
        EXPECT_NE(std::string_view{error.what()}.find(R"(r"\w+")"), std::string_view::npos) << error.what();
        EXPECT_NE(std::string_view{error.what()}.find("Unicode tables"), std::string_view::npos) << error.what();
    }
}
