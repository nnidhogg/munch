#include "munch/tools/audit/read_logos.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace munch::tools::audit;

namespace
{
/**
 * @brief The token of a discarded rule: none.
 */
const std::optional<std::string> discarded{};

/**
 * @brief The token of a rule of the variant `V`.
 */
const std::optional<std::string> variant_v{"V"};

/**
 * @brief The token of a rule of the variant `X`.
 */
const std::optional<std::string> variant_x{"X"};

/**
 * @brief The token of a rule of the variant `Other`.
 */
const std::optional<std::string> variant_other{"Other"};

/**
 * @brief The line an attribute_file()'s attribute stands on.
 */
constexpr std::size_t attribute_line{3};

/**
 * @brief What a callback_file() opens with by default: the crate's names imported, the derive and the extras.
 */
constexpr std::string_view callback_head{
        "use logos::{Filter, FilterResult, Lexer, Logos, Skip};\n#[derive(Logos)]\n#[logos(extras = usize)]\n"};

/**
 * @brief The line a callback_file()'s attribute stands on under a head of three lines, the default one among them.
 */
constexpr std::size_t callback_line{5};

/**
 * @brief Where a source is refused and in what words.
 */
struct Refusal
{
    /**
     * @brief The line the refusal names, none when the source is read.
     */
    std::optional<std::size_t> line{};

    /**
     * @brief The refusal's words, empty when the source is read.
     */
    std::string message{};
};

/**
 * @brief Returns the refusal of a source.
 * @param source The source.
 * @return The refusal, no line and no words when the source is read.
 */
Refusal refusal_at(const std::string_view source)
{
    try
    {
        std::ignore = read_logos(source);
    }
    catch (const Spec_error& error)
    {
        return {.line = error.line(), .message = error.what()};
    }

    return {.line = std::nullopt, .message = {}};
}

/**
 * @brief Returns the line a source is refused at.
 * @param source The source.
 * @return The line, none when the source is read.
 */
std::optional<std::size_t> line_of(const std::string_view source)
{
    return refusal_at(source).line;
}

/**
 * @brief Returns what the refusal of a source says.
 * @param source The source.
 * @return The refusal, empty when the source is read.
 */
std::string refusal_of(const std::string_view source)
{
    return refusal_at(source).message;
}

/**
 * @brief Returns the text of a file holding one enum deriving Logos with one variant, V, under the attribute given.
 * @param attribute The attribute's text, `#[...]` included.
 * @return The file's text.
 */
std::string attribute_file(const std::string_view attribute)
{
    return std::format("#[derive(Logos)]\nenum T {{\n    {}\n    V,\n}}\n", attribute);
}

/**
 * @brief Returns the one rule of an attribute_file().
 * @param attribute The attribute's text, `#[...]` included.
 * @return The rule.
 */
Lexer_spec::Rule rule_of(const std::string_view attribute)
{
    const auto lexers{read_logos(attribute_file(attribute))};

    return lexers.front().rules.front();
}

/**
 * @brief Returns the line an attribute_file() is refused at.
 * @param attribute The attribute's text, `#[...]` included.
 * @return The line, none when the file is read.
 */
std::optional<std::size_t> line_of_attribute(const std::string_view attribute)
{
    return line_of(attribute_file(attribute));
}

/**
 * @brief Returns the token of a file's first rule.
 * @param source The file's text.
 * @return The token, none for a discarded rule.
 */
std::optional<std::string> token_of(const std::string_view source)
{
    const auto lexers{read_logos(source)};

    return lexers.front().rules.front().token;
}

/**
 * @brief Returns a file whose enum carries a variant with the attribute given, then Other, and the items given after
 *        it.
 * @param attribute The variant's attribute.
 * @param after The items after the enum.
 * @param variant The variant, its payload included.
 * @param head What stands before the enum: its imports and its attributes.
 * @return The file's text.
 */
std::string callback_file(
        const std::string_view attribute, const std::string_view after, const std::string_view variant,
        const std::string_view head = callback_head)
{
    return std::format("{}enum T {{\n    {}\n    {},\n    Other,\n}}\n{}", head, attribute, variant, after);
}

/**
 * @brief Returns the token of the first rule of a callback_file().
 * @param attribute The variant's attribute.
 * @param after The items after the enum.
 * @param variant The variant, its payload included.
 * @param head What stands before the enum.
 * @return The token, none for a discarded rule.
 */
std::optional<std::string> callback_token(
        const std::string_view attribute, const std::string_view after = "", const std::string_view variant = "V",
        const std::string_view head = callback_head)
{
    return token_of(callback_file(attribute, after, variant, head));
}

/**
 * @brief Returns the line a callback_file() is refused at.
 * @param attribute The variant's attribute.
 * @param after The items after the enum.
 * @param variant The variant, its payload included.
 * @param head What stands before the enum.
 * @return The line, or none when the file is read.
 */
std::optional<std::size_t> callback_refused_at(
        const std::string_view attribute, const std::string_view after = "", const std::string_view variant = "V",
        const std::string_view head = callback_head)
{
    return line_of(callback_file(attribute, after, variant, head));
}

/**
 * @brief Returns the token a lexer's longest match at the start of an input emits.
 * @param lexer The lexer.
 * @param input The input.
 * @return The token, none where no token matches.
 */
std::optional<std::size_t> token_at(const munch::core::Lexer& lexer, const std::string_view input)
{
    const auto [token, length]{lexer.tokenize<std::size_t>(input)};

    return token;
}

/**
 * @brief Returns how long a lexer's longest match at the start of an input is.
 * @param lexer The lexer.
 * @param input The input.
 * @return The length, zero where no token matches.
 */
std::size_t length_at(const munch::core::Lexer& lexer, const std::string_view input)
{
    const auto [token, length]{lexer.tokenize<std::size_t>(input)};

    return length;
}

/**
 * @brief The logos attribute idioms in one file: skips in both spellings, subpatterns referring to each other, a token
 *        with a callback, a regex with a priority override, callbacks naming functions defined before and after the
 *        enum, a callback that skips, a skip through a closure, several attributes on one variant, variants with
 *        payloads and discriminants, attributes logos ignores, and a second enum, all among the Rust the reader must
 *        step over.
 *
 * Rust that logos 0.15.1 derives as it stands, given the `Extras` type: every variant is a unit or a tuple one, named
 * fields being what the crate says it "doesn't support yet", and the `#[default]` attribute has the derive that gives
 * it a meaning. The ranking test below holds the reading against the crate's own answers for this file, so the file has
 * to be one the crate compiles.
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

TEST(Read_logos_test, Reads_the_attribute_idioms_of_an_enum_deriving_Logos)
{
    const auto lexers{read_logos(idioms)};

    ASSERT_EQ(lexers.size(), 2U);

    const auto& spec{lexers.front()};

    EXPECT_EQ(spec.line, 10U);
    EXPECT_TRUE(spec.conditions.empty());

    // The enum's own keys, after the Unicode version the reader's classes were taken from.
    EXPECT_EQ(spec.options, (std::vector<std::string>{"unicode-classes=16.0.0", "extras=Extras"}));

    ASSERT_EQ(spec.definitions.size(), 2U);
    EXPECT_EQ(spec.definitions.at("digit"), "[0-9]");
    EXPECT_EQ(spec.definitions.at("number"), "{digit}+");

    // Two skips first, then the variants' attributes in order, one rule each.
    ASSERT_EQ(spec.rules.size(), 10U);

    EXPECT_EQ(spec.rules[0].pattern, R"(r"[ \t\n\f]+")");
    EXPECT_EQ(spec.rules[0].expression, R"([\t\n\x0c ]+)");
    EXPECT_FALSE(spec.rules[0].token.has_value());
    EXPECT_EQ(spec.rules[0].priority, std::optional<std::size_t>{2});
    EXPECT_EQ(spec.rules[0].line, 11U);

    EXPECT_EQ(spec.rules[1].pattern, R"("//[^\n]*")");
    EXPECT_FALSE(spec.rules[1].token.has_value());
    EXPECT_EQ(spec.rules[1].priority, std::optional<std::size_t>{3});
    EXPECT_EQ(spec.rules[1].line, 12U);

    EXPECT_EQ(spec.rules[2].pattern, R"("fn")");
    EXPECT_EQ(spec.rules[2].expression, R"("fn")");
    EXPECT_EQ(spec.rules[2].token, std::optional<std::string>{"Fn"});
    EXPECT_EQ(spec.rules[2].priority, std::optional<std::size_t>{4});
    EXPECT_EQ(spec.rules[2].line, 17U);

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

    EXPECT_EQ(active_rules(spec, "INITIAL").size(), 10U);

    // The second enum, inside a module and deriving by path.
    const auto& part{lexers.back()};

    EXPECT_EQ(part.line, 52U);
    ASSERT_EQ(part.rules.size(), 2U);
    EXPECT_EQ(part.rules[0].token, std::optional<std::string>{"Text"});
    EXPECT_EQ(part.rules[1].pattern, R"(r#"""#)");
    EXPECT_EQ(part.rules[1].expression, R"("\"")");
}

TEST(Read_logos_test, A_token_under_an_ignore_flag_is_the_regex_logos_escapes_it_into)
{
    // logos compiles a token under an ignore flag as a regex, the literal escaped for the regex crate: a byte string's
    // byte beyond ASCII is written out as the characters of its `\xNN` escape and the backslash escaped again, so the
    // token matches those eight characters and never the two bytes. logos 0.15.1 answers Folded on the text and
    // Explicit on the bytes, and its conflict hint for two such tokens names `priority = 17`, the priority sixteen the
    // escape text's eight characters give.
    const auto folded{rule_of(R"rs(#[token(b"\xC3\xA9", ignore(case))])rs")};

    EXPECT_EQ(folded.expression, R"("\\"[Xx][Cc]"3\\"[Xx][Aa]"9")");
    EXPECT_EQ(folded.priority, std::optional<std::size_t>{16});

    // With no ignore flag the token is a literal to logos, matched as its bytes at twice their number.
    const auto plain{rule_of(R"rs(#[token(b"\xC3\xA9")])rs")};

    EXPECT_EQ(plain.expression, R"("\xc3\xa9")");
    EXPECT_EQ(plain.priority, std::optional<std::size_t>{4});

    // An ASCII literal escapes into itself, so the count is twice its length under the flag as without it, and the hint
    // logos gives for the pair of them names `priority = 5`.
    EXPECT_EQ(rule_of(R"rs(#[token(b"is", ignore(case))])rs").priority, std::optional<std::size_t>{4});
    EXPECT_EQ(rule_of(R"rs(#[token("is", ignore(case))])rs").priority, std::optional<std::size_t>{4});
    EXPECT_EQ(rule_of(R"rs(#[token("is")])rs").priority, std::optional<std::size_t>{4});

    // Ranked against a regex of priority three over the same bytes, the escaped token loses the bytes and takes its own
    // text, which is the pair of answers logos gives.
    const auto file{std::format(
            "#[derive(Logos)]\n#[logos(source = [u8])]\nenum T {{\n{}\n    Folded,\n{}\n    Explicit,\n}}\n",
            R"rs(    #[token(b"\xC3\xA9", ignore(case))])rs", R"rs(    #[regex(b"\xC3\xA9", priority = 3)])rs")};

    const auto ranked{read_logos(file).front()};

    const auto lexer{build(ranked, "INITIAL")};

    EXPECT_EQ(token_at(lexer, "\xc3\xa9"), std::optional<std::size_t>{1});
    EXPECT_EQ(token_at(lexer, R"(\xc3\xa9)"), std::optional<std::size_t>{0});
    EXPECT_EQ(token_at(lexer, R"(\XC3\XA9)"), std::optional<std::size_t>{0});
}

TEST(Read_logos_test, A_repetition_logos_cannot_resolve_at_its_boundary_is_refused)
{
    // logos 0.15.1 decides a repetition's end on one byte, so a repetition whose body can begin with a byte that may
    // also follow it becomes a scanner that matches no input at all: the crate answers nothing for every input to each
    // of these, the flex spelling of the block comment among them, whose loop and closer both admit a star.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a+a")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r#"".*""#)])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"/\*([^*]|\*+[^*/])*\*+/")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"([a-z]+,)*x")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[[:word:]]+[[:^digit:]]")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a(b|c+d)*ce")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a(bc+)*ce")])rs"), attribute_line);

    // With the two byte sets apart the crate scans the language and the pattern reads: the block comment spelled so
    // that its loop cannot begin with a star, a nested plus whose follow it does not admit, and the study's own rules.
    // A bounded repetition needs no such decision, logos unrolling it, so an ambiguous one still reads.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"/\*[^*]*\*+([^*/][^*]*\*+)*/")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"([a-z]+,)*0")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a(bc+)*fe")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[0-9]+k")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[[:word:]]+[[:^word:]]")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"ab?be")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a{2,3}a")])rs"), std::nullopt);

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

TEST(Read_logos_test, An_ignore_ascii_case_flag_folds_the_ascii_letters_as_logos_folds_them)
{
    // logos parses a pattern under `ignore(ascii_case)` as it stands and folds the ASCII letters of the compiled tree,
    // so a class gains the other case of its ASCII members: logos 0.15.1 matches IS for the token and ABC for the
    // class, and leaves a folded non-ASCII scalar alone.
    EXPECT_EQ(rule_of(R"rs(#[regex("a", ignore(ascii_case))])rs").expression, "[Aa]");
    EXPECT_EQ(rule_of(R"rs(#[regex("[a-z]+", ignore(ascii_case))])rs").expression, "[A-Za-z]+");

    const auto ascii_folded{rule_of(R"rs(#[token("is", ignore(ascii_case))])rs")};

    EXPECT_EQ(ascii_folded.expression, "[Ii][Ss]");
    EXPECT_EQ(ascii_folded.priority, std::optional<std::size_t>{4});

    // `ignore(case)` is the crate's own case-insensitive parse instead, which reaches the long s.
    EXPECT_EQ(rule_of(R"rs(#[token("is", ignore(case))])rs").expression, R"([Ii][Ss\u{17f}])");

    // A literal is taken apart one piece per byte, so a scalar beyond ASCII counts two for each of its bytes, the four
    // logos's own conflict hint names, and the folding leaves it matching only itself.
    constexpr std::string_view acute_source{R"rs(#[derive(Logos)]
enum T {
    #[token("\u{e9}", ignore(ascii_case))]
    V,
}
)rs"};

    const auto acute{read_logos(acute_source).front()};

    EXPECT_EQ(acute.rules.front().priority, std::optional<std::size_t>{4});

    const auto lexer{build(acute, "INITIAL")};

    EXPECT_EQ(length_at(lexer, "\xc3\xa9"), 2U);
    EXPECT_FALSE(token_at(lexer, "\xc3\x89").has_value());

    // A byte string takes the crate's binary case-insensitive parse under either flag, ASCII-only either way.
    EXPECT_EQ(rule_of(R"rs(#[token(b"is", ignore(ascii_case))])rs").expression, "[Ii][Ss]");

    // An `(?i)` inside the pattern keeps the crate's own folding for its scope, the ASCII pass adding nothing.
    EXPECT_EQ(rule_of(R"rs(#[regex("(?i)s", ignore(ascii_case))])rs").expression, R"([Ss\u{17f}])");

    // logos 0.15.1 knows priority, callback and ignore and no other argument, `allow_greedy` among the ones it calls an
    // unknown nested attribute, on a variant's attribute and in a skip alike.
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

TEST(Read_logos_test, The_built_lexer_ranks_as_logos_ranks)
{
    const auto scanners{read_logos(idioms)};

    const auto lexer{build(scanners.front(), "INITIAL")};

    // Longest match first: an identifier beats the keyword it extends; at equal length the higher priority wins, so
    // `fn` is the token and a digit run is Number over nothing shorter.
    EXPECT_EQ(token_at(lexer, "fn"), std::optional<std::size_t>{2});
    EXPECT_EQ(token_at(lexer, "fnord"), std::optional<std::size_t>{3});
    EXPECT_EQ(token_at(lexer, "42"), std::optional<std::size_t>{4});
    EXPECT_EQ(token_at(lexer, "42k"), std::optional<std::size_t>{9});

    // A skip's rule carries a lower id than any variant's and is discarded; the newline certifies neither way, since
    // the discarded block comment folds it in, which is the split-points paper's finding on that token.
    EXPECT_EQ(token_at(lexer, "  x"), std::optional<std::size_t>{0});
    EXPECT_FALSE(lexer.is_split_point('\n'));
    EXPECT_FALSE(lexer.is_split_point_ignoring('\n'));
    EXPECT_FALSE(lexer.is_split_point(' '));
}

TEST(Read_logos_test, Priorities_are_computed_as_the_handbook_documents)
{
    const auto priority_of{[](const std::string_view attribute) { return *rule_of(attribute).priority; }};

    // The handbook's own examples, and the shapes around them.
    EXPECT_EQ(priority_of(R"rs(#[regex("[a-zA-Z]+")])rs"), 2U);
    EXPECT_EQ(priority_of(R"rs(#[regex("foobar")])rs"), 12U);
    EXPECT_EQ(priority_of(R"rs(#[regex("(foo|hello)(bar)?")])rs"), 6U);
    EXPECT_EQ(priority_of(R"rs(#[regex("a|b")])rs"), 2U);
    EXPECT_EQ(priority_of(R"rs(#[regex("[a-b]")])rs"), 2U);
    EXPECT_EQ(priority_of(R"rs(#[regex("(foo)+")])rs"), 6U);
    EXPECT_EQ(priority_of(R"rs(#[regex("(fooz|bar)+qux")])rs"), 12U);
    EXPECT_EQ(priority_of(R"rs(#[regex("a{3}")])rs"), 6U);
    EXPECT_EQ(priority_of(R"rs(#[regex("a{2,5}b*")])rs"), 4U);
    EXPECT_EQ(priority_of(R"rs(#[regex("Été")])rs"), 6U);
    EXPECT_EQ(priority_of(R"rs(#[regex(".")])rs"), 2U);
    EXPECT_EQ(priority_of(R"rs(#[regex("(?i)ab")])rs"), 4U);

    // A token counts its bytes, a regex its scalars, a run that is not UTF-8 its bytes, and a capture keeps the runs on
    // either side of it apart; an override stands whatever the shape.
    EXPECT_EQ(priority_of(R"rs(#[token("é")])rs"), 4U);
    EXPECT_EQ(priority_of(R"rs(#[regex("é")])rs"), 2U);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xC3\xA9")])rs"), 2U);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xC3(\xA9)")])rs"), 4U);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xC3(?:\xA9)")])rs"), 2U);
    EXPECT_EQ(priority_of(R"rs(#[token("foobar", priority = 20)])rs"), 20U);
    EXPECT_EQ(priority_of(R"rs(#[regex("[a-z]+", |_| (), priority = 7)])rs"), 7U);

    // What counts as UTF-8 is what Rust's own validation takes, since logos asks `std::str::from_utf8`: a run that
    // encodes a surrogate, an overlong form or a scalar above U+10FFFF is bytes, not characters, and scores twice its
    // byte count. logos 0.15.1 itself names these priorities, one higher, in the hint of its conflict error.
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xED\xA0\x80")])rs"), 6U);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xE0\x80\x80")])rs"), 6U);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xF4\x90\x80\x80")])rs"), 8U);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xC0\xAF")])rs"), 4U);
    EXPECT_EQ(priority_of(R"rs(#[regex(b"\xF0\x9F\x98\x80")])rs"), 2U);
}

TEST(Read_logos_test, Patterns_are_rewritten_over_the_UTF8_bytes)
{
    // The dot is every scalar but the newline; the scalars beyond ASCII are written as the code point ranges the parser
    // reads as their encodings.
    EXPECT_EQ(rule_of(R"rs(#[regex(".")])rs").expression, R"([\x00-\t\x0b-\x7f\u{80}-\u{d7ff}\u{e000}-\u{10ffff}])");
    EXPECT_EQ(rule_of(R"rs(#[regex("(?s).")])rs").expression, R"([\x00-\x7f\u{80}-\u{d7ff}\u{e000}-\u{10ffff}])");

    // A negated class over ASCII exclusions admits every other scalar.
    EXPECT_EQ(
            rule_of(R"rs(#[regex(r#"[^"\\]+"#)])rs").expression,
            R"([\x00-!#-\[\]-\x7f\u{80}-\u{d7ff}\u{e000}-\u{10ffff}]+)");

    // Non-ASCII scalars, typed or escaped, single or in ranges, are their UTF-8; a plain string decodes Rust's escapes
    // before the regex reads what is left.
    EXPECT_EQ(rule_of(R"rs(#[regex("é+")])rs").expression, R"("\xc3\xa9"+)");
    EXPECT_EQ(rule_of(R"rs(#[token("a\tb\u{e9}")])rs").expression, R"("a\tb\xc3\xa9")");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"[ ,\ufeff]+")])rs").expression, R"([ ,\u{feff}]+)");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"[\u005D-\u00FF]")])rs").expression, R"([\]-\x7f\u{80}-\u{ff}])");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"\x{1F600}")])rs").expression, R"("\xf0\x9f\x98\x80")");

    // Under `i` a letter is the class of its cases, `k` and `s` the Kelvin sign's and the long s's too, scoped to the
    // group the flag stands in; a token's ignore(case) is the same reading.
    EXPECT_EQ(rule_of(R"rs(#[regex("(?i)ab")])rs").expression, "[Aa][Bb]");
    EXPECT_EQ(rule_of(R"rs(#[regex("(?i:k)s")])rs").expression, R"([Kk\u{212a}]"s")");
    EXPECT_EQ(rule_of(R"rs(#[regex("(?i)[a-c]1")])rs").expression, R"([A-Ca-c]"1")");

    const auto folded{rule_of(R"rs(#[token("is", ignore(case))])rs")};

    EXPECT_EQ(folded.expression, R"([Ii][Ss\u{17f}])");
    EXPECT_EQ(folded.priority, std::optional<std::size_t>{4});

    // The other constructs: groups of every kind, a capture keeping the runs beside it apart, counts, empty branches,
    // ASCII classes, the ASCII forms of the Perl classes, a byte string pattern over bytes, a nested class and an ASCII
    // class folded before their negation, and the bracket's own characters.
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?P<n>a)(?:b)(?<m>c)")])rs").expression, R"("a""b""c")");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"a(?:b)c")])rs").expression, R"("abc")");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(ab){2,}(cd){3}(ef){1,2}")])rs").expression, R"("ab"{2,}"cd"{3}"ef"{1,2})");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"x(a|)")])rs").expression, R"("x""a"?)");
    EXPECT_EQ(
            rule_of(R"rs(#[regex(r"[[:word:]]+[[:^word:]]")])rs").expression,
            R"([0-9A-Z_a-z]+[\x00-/:-@\[-\^`{-\x7f\u{80}-\u{d7ff}\u{e000}-\u{10ffff}])");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?-u)\d+\s")])rs").expression, R"([0-9]+[\t-\r ])");
    EXPECT_EQ(rule_of(R"rs(#[regex(br"(?i)[[^b]]")])rs").expression, R"([\x00-AC-ac-\xff])");
    EXPECT_EQ(rule_of(R"rs(#[regex(br"(?i)[[:^lower:]]x")])rs").expression, R"([\x00-@\[-`{-\xff][Xx])");
    EXPECT_EQ(rule_of(R"rs(#[regex(b"\xFF[^\n]")])rs").expression, R"("\xff"[\x00-\t\x0b-\xff])");
    EXPECT_EQ(rule_of(R"rs(#[regex(r"[]a-]")])rs").expression, R"([\-\]a])");
}

TEST(Read_logos_test, Refusals_name_the_line_and_the_construct)
{
    // Needing tables the library has not got: the property classes, and a non-ASCII scalar under `i`.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"\p{Letter}")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[\P{L}]")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?i)é")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[token("élan", ignore(case))])rs"), attribute_line);

    // Conditions on the context, which a token language cannot say.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"^a")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a$")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"\ba")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a(?=b)")])rs"), attribute_line);

    // The flags and operators not modelled, the arguments logos has not got, and the empty pattern.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?x) a b")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[a-z&&[^aeiou]]")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[\d-z]")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex("a", greedy = true)])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex("a", priority = 2, cb)])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[token("")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex("(?&nope)")])rs"), attribute_line);

    // Malformed Rust around the attributes, at the line it goes wrong; an enum left open is refused at its brace, as
    // any group left open is.
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T {\n    #[token(\"a\")]\n    V\n    W,\n}\n"), 5);
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T {\n    #[token(\"a\")]\n    V,\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(subpattern d = r\"[0-9]\", subpattern d = r\"x\")]\nenum T {}\n"), 2);

    // The message names the pattern as written and what was refused.
    const auto [property_line, property_message]{
            refusal_at("#[derive(Logos)]\nenum T {\n    #[regex(r\"\\p{L}+\")]\n    V,\n}\n")};

    EXPECT_TRUE(property_line.has_value());
    EXPECT_TRUE(property_message.contains(R"(r"\p{L}+")")) << property_message;
    EXPECT_TRUE(property_message.contains("property class")) << property_message;
}

TEST(Read_logos_test, Unicode_perl_classes_are_the_crates_at_the_version_the_crate_pins)
{
    // logos 0.15.1 is locked to regex-syntax 0.8.11, whose tables are the Unicode 16.0.0 database's, so that is the
    // language the reader models, and it says so among the scanner's options. The library pins a later database, in
    // which U+11DE0 is a decimal digit; logos, asked for `\d`, matches nothing there, as running it shows.
    const auto digits{rule_of(R"rs(#[regex(r"\d+")])rs")};

    const auto plain{read_logos("#[derive(Logos)]\nenum T {\n    V,\n}\n").front()};

    EXPECT_EQ(plain.options, (std::vector<std::string>{"unicode-classes=16.0.0"}));
    EXPECT_FALSE(digits.expression.contains(R"(\u{11de0})"));

    // Built, the digit class takes the ASCII digit and leaves that code point's encoding to its negation.
    const auto lexer_of{[](const std::string_view attribute) {
        const auto file{read_logos(attribute_file(attribute)).front()};

        return build(file, "INITIAL");
    }};

    const auto digit{lexer_of(R"rs(#[regex(r"\d+")])rs")};

    EXPECT_EQ(length_at(digit, "7"), 1U);
    EXPECT_EQ(length_at(digit, "\xf0\x91\xb7\xa0"), 0U);

    const auto non_digit{lexer_of(R"rs(#[regex(r"\D")])rs")};

    const auto non_word{lexer_of(R"rs(#[regex(r"\W")])rs")};

    EXPECT_EQ(length_at(non_digit, "\xf0\x91\xb7\xa0"), 4U);
    EXPECT_EQ(length_at(non_word, "\xf0\x91\xb7\xa0"), 4U);

    // `\s` is White_Space: the ASCII run, the next line, the no-break space and the other separators, twenty-five
    // scalars in all, written out; `\d` is Nd and `\w` the word class, whose first rows past ASCII are the Arabic-Indic
    // digits and the feminine ordinal indicator, and whose negations begin at the null byte.
    EXPECT_EQ(
            rule_of(R"rs(#[regex(r"\s+")])rs").expression,
            R"([\t-\r \u{85}\u{a0}\u{1680}\u{2000}-\u{200a}\u{2028}-\u{2029}\u{202f}\u{205f}\u{3000}]+)");
    EXPECT_TRUE(digits.expression.starts_with(R"([0-9\u{660}-\u{669}\u{6f0}-\u{6f9})"));
    EXPECT_TRUE(rule_of(R"rs(#[regex(r"[\w]")])rs").expression.starts_with(R"([0-9A-Z_a-z\u{aa}\u{b5}\u{ba})"));
    EXPECT_TRUE(rule_of(R"rs(#[regex(r"\D")])rs").expression.starts_with(R"([\x00-/:-\x7f\u{80}-\u{65f})"));
    EXPECT_TRUE(
            rule_of(R"rs(#[regex(r"[^\s]")])rs").expression.starts_with(R"([\x00-\x08\x0e-\x1f!-\x7f\u{80}-\u{84})"));

    // Under `(?-u)` the same escapes are their ASCII forms.
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?-u)\w+")])rs").expression, "[0-9A-Z_a-z]+");
}

TEST(Read_logos_test, A_subpattern_is_read_in_the_mode_of_the_pattern_referencing_it)
{
    // logos substitutes a subpattern's text, a byte string's bytes beyond ASCII spelled `\xHH`, into the pattern before
    // the crate parses it, so the definition is read in the referencing pattern's mode: `b"\xc3"` referenced from a
    // string pattern is the scalar U+00C3, which logos 0.15.1 matches as its two bytes, a byte class the scalars of its
    // range, a byte dot any scalar but the newline, and under `ignore(case)` the string pattern's Unicode folding, the
    // Kelvin sign with `k`, while a string's `\xe9` referenced from a byte pattern is that byte and its typed scalar
    // its UTF-8. The priority follows the mode too: two scalars count four, the crate's conflict check answering so.
    const auto first_rule{[](const std::string_view definition, const std::string_view attribute) {
        const auto file{std::format(
                "#[derive(Logos)]\n#[logos(subpattern {})]\nenum T {{\n{}\n    V,\n}}\n", definition, attribute)};

        const auto lexers{read_logos(file)};

        return lexers.front().rules.front();
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
    EXPECT_EQ(first_rule(R"rs(e = r"\xe9")rs", R"rs(#[regex(b"(?&e)")])rs").expression, R"("\xe9")");
    EXPECT_EQ(first_rule(R"rs(e = "é")rs", R"rs(#[regex(b"(?&e)")])rs").expression, R"("\xc3\xa9")");
    EXPECT_EQ(
            first_rule(R"rs(any = ".")rs", R"rs(#[regex(b"x(?&any)")])rs").expression,
            rule_of(R"rs(#[regex(b"x.")])rs").expression);

    // In its own mode under no flag the reference keeps the definition's name and priority, the definition compiled in
    // that mode.
    constexpr std::string_view own_source{R"rs(#[derive(Logos)]
#[logos(subpattern hi = b"\xc3\xa9")]
enum T {
    #[regex(b"(?&hi)")]
    V,
}
)rs"};

    const auto own{read_logos(own_source).front()};

    EXPECT_EQ(own.rules.front().expression, "{hi}");
    EXPECT_EQ(own.rules.front().priority, std::optional<std::size_t>{2});
    EXPECT_EQ(own.definitions.at("hi"), R"("\xc3\xa9")");

    // A string's class with a non-ASCII member is valid where it is declared and refused, as the crate refuses it,
    // where a byte pattern references it.
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(subpattern e = \"[é]\")]\nenum T {\n    #[regex(b\"(?&e)\")]\n"
                    "    V,\n}\n"),
            4);
}

TEST(Read_logos_test, An_empty_subpattern_is_valid_and_emptiness_is_decided_on_the_rule)
{
    // logos 0.15.1 takes an empty definition, a pattern pasting it in matching what the rest of it matches, so `x` is
    // the token V to it; the reference expands to nothing, since regex::parse() takes no empty definition. A rule that
    // is empty once its subpatterns are pasted in is no token to logos: it panics on an empty token and compiles an
    // empty regex, or an empty skip, into a rule matching no input, so each is refused at its own line.
    constexpr std::string_view spec_source{R"rs(#[derive(Logos)]
#[logos(subpattern empty = "")]
enum T {
    #[regex("x(?&empty)")]
    V,
    #[regex("[a-w]+")]
    Word,
}
)rs"};

    const auto spec{read_logos(spec_source).front()};

    ASSERT_EQ(spec.rules.size(), 2U);
    EXPECT_EQ(spec.rules.front().expression, R"("x")");
    EXPECT_EQ(spec.rules.front().priority, std::optional<std::size_t>{2});

    const auto lexer{build(spec, "INITIAL")};

    EXPECT_EQ(token_at(lexer, "xa"), std::optional<std::size_t>{0});

    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(subpattern empty = \"\")]\nenum T {\n    V,\n}\n"), std::nullopt);

    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(subpattern empty = \"\")]\nenum T {\n    #[regex(\"(?&empty)\")]\n"
                    "    V,\n}\n"),
            4);
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T {\n    #[regex(\"\")]\n    V,\n}\n"), 3);
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T {\n    #[token(\"\")]\n    V,\n}\n"), 3);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip \"\")]\nenum T {\n    V,\n}\n"), 2);

    EXPECT_TRUE(refusal_of("#[derive(Logos)]\nenum T {\n    #[token(\"\")]\n    V,\n}\n").contains("panics"));
    EXPECT_TRUE(
            refusal_of("#[derive(Logos)]\nenum T {\n    #[regex(\"\")]\n    V,\n}\n").contains("matching no input"));
}

TEST(Read_logos_test, A_subpattern_is_substituted_as_text_before_the_priority_is_computed)
{
    // logos pastes a subpattern's text into the pattern before the crate parses it, so the crate's merging of adjacent
    // literals runs across the reference: the UTF-8 of one scalar split between the pattern and the subpattern is one
    // scalar and counts two, while a run that is UTF-8 on its own and not once joined counts its bytes. logos 0.15.1's
    // conflict hint names each of these priorities one higher, `priority = 3` for the first and third and `priority =
    // 7` for the second, and a capture around the reference keeps the runs apart, four, which lets that pair build.
    const auto first_rule{[](const std::string_view definitions, const std::string_view attribute) {
        const auto file{std::format("#[derive(Logos)]\n{}enum T {{\n{}\n    V,\n}}\n", definitions, attribute)};

        const auto lexers{read_logos(file)};

        return lexers.front().rules.front();
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
    EXPECT_EQ(first_rule(low, R"rs(#[regex(b"\xC3(?&lo)")])rs").expression, R"("\xc3"{lo})");

    // A string's scalar pasted into a byte pattern is its UTF-8 and counts two as well, and a flag before the reference
    // reaches in: `(?i)(?&k)` is a class, two.
    EXPECT_EQ(
            first_rule("#[logos(subpattern e = \"\\u{e9}\")]\n", R"rs(#[regex(b"(?&e)")])rs").priority,
            std::optional<std::size_t>{2});
    EXPECT_EQ(
            first_rule("#[logos(subpattern k = \"k\")]\n", R"rs(#[regex("(?i)(?&k)")])rs").priority,
            std::optional<std::size_t>{2});

    // Ranked against a whole-scalar rule at priority three, the split one loses on the scalar, as logos answers.
    constexpr std::string_view ranked_source{R"rs(#[derive(Logos)]
#[logos(subpattern lo = b"\xA9")]
enum T {
    #[regex(b"\xC3(?&lo)")]
    Split,
    #[regex(b"\xC3\xA9", priority = 3)]
    Whole,
}
)rs"};

    const auto ranked{read_logos(ranked_source).front()};

    const auto ranked_lexer{build(ranked, "INITIAL")};

    EXPECT_EQ(token_at(ranked_lexer, "\xc3\xa9"), std::optional<std::size_t>{1});
}

TEST(Read_logos_test, What_logos_refuses_in_a_pattern_is_refused)
{
    // The lazy operators: logos 0.15.1 answers "non-greedy parsing is currently unsupported" to every one, in a regex,
    // a skip, a subpattern and a byte pattern alike, so none is read as its greedy form; a token is a literal and keeps
    // its question mark.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a*?")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a+?")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a??")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a{1,3}?")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"a+?")])rs"), attribute_line);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip r\"a+?\")]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(subpattern a = r\"a+?\")]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(rule_of(R"rs(#[token("a*?")])rs").expression, R"("a*?")");

    // The dot under `s`, and any class of every scalar or every byte, under `*`, `+`, `{0,}` or `{1,}`: the crate
    // refuses these as consuming the source to its end, wherever they stand, under either ignore flag, and through a
    // subpattern. The plain dot leaves out the newline and passes, as do `?`, a bounded count, a count from two, a
    // token's escaped text, and a captured dot, which the crate's comparison never sees through.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).+")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).{0,}")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).{1,}")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s:.)*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?is).*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a|(?s).*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"((?s).*)")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[\s\S]*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[\d\D]*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[\x00-\x{10FFFF}]*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).*", ignore(case))])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).*", ignore(ascii_case))])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"(?s).*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"(?s).+")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"[\x00-\xff]*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"(?u)(?s).*")])rs"), attribute_line);
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(subpattern any = r\"(?s).\")]\nenum T {\n    #[regex(r\"(?&any)*\")]\n"
                    "    V,\n}\n"),
            4);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r".*")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r".+")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br".*")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).?")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).{0,3}")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s).{2,}")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s)(.)*")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s)(.)+")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[token(".*", ignore(case))])rs"), std::nullopt);

    // The regex crate merges an alternation of classes into one class, and one of single characters into the class of
    // them, before logos looks, so those are the dot when their union is everything; a class alternated with a literal
    // stays an alternation, a captured alternation stays captured, and `[\x00-\x{D7FF}\x{E000}-\x{10FFFF}]` is two
    // ranges to the crate where its dot is one, so each of those builds.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s)(?:.|.)+")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a(?:[^\n]|[\n\r])*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?:[a-z]|[^a-z])+")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?:(?:\n|\r)|[^\n\r])+")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(br"a(?:[\x00-\x7f]|[\x80-\xff])*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"a(?:[^\n]|\n)*")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?:\n|\r|[^\n\r])+")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s)([^\n]|[\n\r])+")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[\x00-\x{D7FF}\x{E000}-\x{10FFFF}]+")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"[\x00-\x{10FFFF}]+")])rs"), attribute_line);

    // An ignore flag on a skip: logos 0.15.1 knows callback and priority there and calls ignore an unknown nested
    // attribute, whichever flag it names.
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip(\"a\", ignore(case)))]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip(\"a\", ignore(ascii_case)))]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip(\"a\", priority = 3))]\nenum T {\n    V,\n}\n"), std::nullopt);
}

TEST(Read_logos_test, A_callback_is_read_by_what_it_visibly_returns)
{
    // logos decides by the callback's type: Skip, Result<Skip, E> and the Skip arms of Filter and FilterResult discard
    // the match, anything else leaves the variant's token or an error at the same boundary, and the enum returned is
    // the token (logos 0.15.1, src/internal.rs). The reading has the text: a closure by every result its body produces,
    // through blocks, returns, ifs and matches, and a function this file defines, before or after the enum, in a module
    // or an impl block, by its return type. Each file here derives with logos 0.15.1, a payload variant carrying the
    // callbacks that return one, and the crate discards on each of the first group, emits V, or an error at V's
    // boundary for `erring`, on each of the second and Other on each of the third, as running it shows; the two bare
    // `Filter::Skip` and `FilterResult::Skip` closures are the exception, which rustc refuses alone as needing a type
    // annotation and compiles once another path fixes the `Emit` type, skipping on that arm.
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| { return logos::Skip; })])rs"), discarded);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |lex| { lex.extras += 1; logos::Skip })])rs"), discarded);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| Skip)])rs"), discarded);
    EXPECT_EQ(callback_token(R"rs(#[token("#", callback = |_| ::logos::Skip)])rs"), discarded);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| Filter::Skip)])rs"), discarded);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| logos::FilterResult::Skip)])rs"), discarded);
    EXPECT_EQ(
            callback_token(R"rs(#[token("#", |lex| if lex.extras > 0 { return Skip; } else { Skip })])rs"), discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[regex("[#@]", |lex| match lex.slice() { "#" => Skip, _ => { return logos::Skip; } })])rs"),
            discarded);
    EXPECT_EQ(
            callback_token(R"rs(#[token("#", drop_it)])rs", "fn drop_it(_lex: &mut Lexer<T>) -> Skip { Skip }\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", later)])rs",
                    "fn later<'a>(_lex: &mut Lexer<'a, T>) -> logos::Skip where { logos::Skip }\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", tried)])rs", "fn tried(_lex: &mut Lexer<T>) -> Result<Skip, ()> { Ok(Skip) }\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", callbacks::drop_it)])rs",
                    "mod callbacks {\n"
                    "    pub fn drop_it(_lex: &mut logos::Lexer<super::T>) -> logos::Skip { logos::Skip }\n}\n"),
            discarded);

    // A comment inside a return type, a result or a path is trivia to Rust and read past here, so each of these skips
    // as logos 0.15.1 skips on them.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", drop_it)])rs",
                    "fn drop_it(_lex: &mut Lexer<T>) -> logos::/* c */Skip { logos::Skip }\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", tried)])rs",
                    "fn tried(_lex: &mut Lexer<T>) -> Result</*c*/ logos::Skip, ()> { Ok(logos::Skip) }\n"),
            discarded);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| logos::/* c */Skip)])rs"), discarded);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| /* c */ Skip)])rs"), discarded);
    EXPECT_EQ(callback_token(R"rs(#[token("#", logos:: /* c */ skip)])rs"), discarded);

    // A statement-like `if` or `match` before the tail needs no semicolon, and the tail is still the value.
    EXPECT_EQ(callback_token(R"rs(#[token("#", |lex| { if lex.extras > 0 { return Skip; } Skip })])rs"), discarded);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |lex| { if lex.extras > 0 { lex.extras -= 1; } Skip })])rs"), discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", |lex| { match lex.extras { 0 => {} _ => { lex.extras -= 1; } } Skip })])rs"),
            discarded);

    EXPECT_EQ(callback_token(R"rs(#[token("#", |lex| { lex.extras += 1; })])rs"), variant_v);
    EXPECT_EQ(callback_token(R"rs(#[regex("[0-9]+", |lex| Some(lex.slice().len()))])rs", "", "V(usize)"), variant_v);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| 42)])rs", "", "V(u64)"), variant_v);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| ())])rs"), variant_v);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| Filter::Emit(()))])rs"), variant_v);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |lex| if lex.extras > 0 { Some(()) } else { None })])rs"), variant_v);
    EXPECT_EQ(
            callback_token(R"rs(#[token("#", count)])rs", "fn count(lex: &mut Lexer<T>) -> bool { lex.extras > 0 }\n"),
            variant_v);
    EXPECT_EQ(
            callback_token(R"rs(#[token("#", note)])rs", "fn note(lex: &mut Lexer<T>) { lex.extras += 1; }\n"),
            variant_v);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", always)])rs",
                    "fn always(_lex: &mut Lexer<T>) -> Filter<()> { Filter::Emit(()) }\n"),
            variant_v);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", erring)])rs",
                    "fn erring(_lex: &mut Lexer<T>) -> FilterResult<(), ()> { FilterResult::Error(()) }\n"),
            variant_v);

    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| T::Other)])rs"), variant_other);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| T:: /* c */ Other)])rs"), variant_other);
    EXPECT_EQ(callback_token(R"rs(#[regex("[0-9]+", |lex| { lex.extras += 1; T::Other })])rs"), variant_other);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| Filter::Emit(T::Other))])rs"), variant_other);
    EXPECT_EQ(
            callback_token(R"rs(#[token("#", other)])rs", "fn other(_lex: &mut Lexer<T>) -> T { T::Other }\n"),
            variant_other);

    // In one of the enum's impl blocks `Self` is the enum, in the return type and in the body; in another type's it is
    // not, and the type is a payload.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", T::mark)])rs",
                    "impl T {\n    fn mark(_lex: &mut Lexer<T>) -> Self { T::Other }\n}\n"),
            variant_other);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", T::mark)])rs",
                    "impl T {\n    fn mark(_lex: &mut Lexer<T>) -> Filter<Self> { Filter::Emit(Self::Other) }\n}\n"),
            variant_other);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", T::mark)])rs",
                    "trait Marker { fn mark(lex: &mut Lexer<T>) -> Self; }\nimpl Marker for T where T: Sized {\n"
                    "    fn mark(_lex: &mut Lexer<T>) -> Self { Self::V }\n}\n"),
            variant_v);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", Wrapper::mark)])rs",
                    "struct Wrapper;\nimpl Wrapper {\n    fn mark(_lex: &mut Lexer<T>) -> Self { Wrapper }\n}\n",
                    "V(Wrapper)"),
            variant_v);
}

TEST(Read_logos_test, A_callback_whose_result_or_macro_is_out_of_sight_is_refused_by_name)
{
    // Refused by name, at the rule's line: a function the file does not define or defines twice, or declares without
    // the body its type leaves the decision to; a result the text does not show, a call among them; results that skip
    // on one path and emit on another, or emit two variants; and a closure that is not one of one parameter.
    EXPECT_EQ(callback_refused_at(R"rs(#[regex("[0-9]+k", callback = kilo)])rs"), callback_line);

    // A macro's expansion is out of sight and may return from the callback: logos 0.15.1 on "#y" under a callback whose
    // body is `redirect!(); T::V`, with `macro_rules! redirect { () => { return T::Other; }; }` in the file, emits
    // Other for the `#` and not V, as running it shows, so invoking a macro of the file's or a crate's is refused by
    // name in a function's body and a closure's alike; a standard macro expands to no return and is read past.
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "macro_rules! redirect { () => { return T::Other; }; }\n"
                    "fn cb(_lex: &mut Lexer<T>) -> T { redirect!(); T::V }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", |_| { redirect!(); T::V })])rs",
                    "macro_rules! redirect { () => { return T::Other; }; }\n"),
            callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("#", |_| { println!("seen"); T::V })])rs"), std::nullopt);

    // Blanks and comments may part the name from its `!`, and a standard macro's name is the file's own macro where the
    // file defines one: logos 0.15.1 emits Other for the `#` under each of these, as running them shows.
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "macro_rules! redirect { () => { return T::Other; }; }\n"
                    "fn cb(_lex: &mut Lexer<T>) -> T { redirect !(); T::V }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "macro_rules! redirect { () => { return T::Other; }; }\n"
                    "fn cb(_lex: &mut Lexer<T>) -> T { redirect /* chosen */ !(); T::V }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "macro_rules! println { () => { return T::Other; }; }\n"
                    "fn cb(_lex: &mut Lexer<T>) -> T { println!(); T::V }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", |_| { format!(); T::V })])rs",
                    "macro_rules! format { () => { return T::Other; }; }\n"),
            callback_line);

    // A standard name qualified by any path but `std::` or `core::` is that module's macro, `checks::assert!` from a
    // module the file declares elsewhere, whose expansion is out of sight; `std::assert!` is the standard one. And a
    // file invoking a macro at item level, `generate!();`, may have it expand to a `macro_rules!` under a standard
    // name, so the standard names are trusted in no callback of that file; a callback invoking none is read.
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "mod checks;\n"
                    "fn cb(lex: &mut Lexer<T>) -> Filter<()> { checks::assert!(lex.span().start == 0); "
                    "Filter::Emit(()) }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "fn cb(lex: &mut Lexer<T>) -> Filter<()> { std::assert!(lex.span().start < 9); "
                    "Filter::Emit(()) }\n"),
            std::nullopt);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs", "generate!();\nfn cb(_lex: &mut Lexer<T>) -> T { println!(); T::V }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(R"rs(#[token("#", cb)])rs", "generate!();\nfn cb(_lex: &mut Lexer<T>) -> T { T::V }\n"),
            std::nullopt);

    // The whole path before the name decides, read over the tokens so a comment inside it hides nothing:
    // `compat::std::println!` reaches a module of the file's own that re-exports a macro under the standard path's last
    // segment, and logos 0.15.1 emits Other for each x of "xx" under it where the callback names V; `checks:: /* c */
    // assert!` is `checks::assert!`; `::std::println!` and `core::assert!` are the standard ones. And an item-level
    // invocation of a file-defined macro, `assert!();` under `macro_rules! assert`, may expand to a definition as any
    // other does.
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "mod compat { pub mod std { macro_rules! println { () => { return crate::T::Other; }; } "
                    "pub(crate) use println; } }\n"
                    "fn cb(_lex: &mut Lexer<T>) -> T { compat::std::println!(); T::V }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "mod checks;\n"
                    "fn cb(lex: &mut Lexer<T>) -> Filter<()> { checks:: /* c */ assert!(lex.span().start == 0); "
                    "Filter::Emit(()) }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "fn cb(_lex: &mut Lexer<T>) -> T { ::std::println!(\"seen\"); core::assert!(true); T::V }\n"),
            std::nullopt);

    // `std` and `core` are the crates only while nothing of the file's own bears the name: a `mod std` in the file, a
    // `use crate::local as std;` at item level or in the body each stand before the crate under a path, and logos
    // 0.15.1 emits Other, or skips, under each where the callback names V.
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "mod std { macro_rules! assert { ($c:expr) => { if !$c { return Filter::Skip; } }; } "
                    "pub(crate) use assert; }\n"
                    "fn cb(lex: &mut Lexer<T>) -> Filter<()> { std::assert!(lex.span().start == 0); "
                    "Filter::Emit(()) }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "mod local { macro_rules! println { () => { return crate::T::Other; }; } pub(crate) use "
                    "println; }\n"
                    "fn cb(_lex: &mut Lexer<T>) -> T { use crate::local as std; std::println!(); T::V }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "mod local { macro_rules! println { () => { return crate::T::Other; }; } pub(crate) use "
                    "println; }\nuse crate::local as std;\n"
                    "fn cb(_lex: &mut Lexer<T>) -> T { std::println!(); T::V }\n"),
            callback_line);

    // A `mod std` in a module the callback does not stand in binds nothing on the callback's path, and `::std::` names
    // the crate through the extern prelude whatever the file binds.
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "mod other { mod std { pub fn f() {} } }\n"
                    "fn cb(_lex: &mut Lexer<T>) -> T { std::println!(\"x\"); T::V }\n"),
            std::nullopt);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "mod local { macro_rules! println { () => { return crate::T::Other; }; } pub(crate) use "
                    "println; }\nuse crate::local as std;\n"
                    "fn cb(_lex: &mut Lexer<T>) -> T { ::std::println!(\"x\"); T::V }\n"),
            std::nullopt);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "macro_rules! assert { () => { macro_rules! println { () => { return T::Other; }; } }; }\n"
                    "assert!();\nfn cb(_lex: &mut Lexer<T>) -> T { println!(); T::V }\n"),
            callback_line);
}

TEST(Read_logos_test, A_body_s_use_and_mod_items_bind_std_throughout_their_block)
{
    // A `use` or a `mod` in a block binds its name throughout the block, wherever in it it stands, so `std::println!`
    // before `use crate::fake as std;` is the file's macro, and so is one after a grouped `use crate::{fake as std, T
    // as U};` or a block-level `mod std`: logos 0.15.1 emits Other for each under a macro returning it where the
    // callback names its own variant. A binding in an inner block binds nothing outside it, and `std::println!("x")`
    // after `{ use crate::fake as std; }` is the standard one, the scanner printing x and emitting X.
    const auto refused{[](const std::string_view body) {
        return callback_refused_at(
                R"rs(#[token("#", cb)])rs",
                std::format(
                        "mod fake {{ macro_rules! println {{ () => {{ return crate::T::Other; }}; }} pub(crate) use "
                        "println; }}\nfn cb(_lex: &mut Lexer<T>) -> T {{ {} }}\n",
                        body));
    }};

    EXPECT_EQ(refused("std::println!(); use crate::fake as std; T::V"), callback_line);
    EXPECT_EQ(refused("let _n = 1; std::println!(); use crate::fake as std; T::V"), callback_line);
    EXPECT_EQ(refused("use crate::{fake as std, T as U}; std::println!(); T::V"), callback_line);
    EXPECT_EQ(refused("use crate::{T as U, fake as std}; std::println!(); T::V"), callback_line);
    EXPECT_EQ(refused("{ use crate::fake as std; std::println!(); } T::V"), callback_line);
    EXPECT_EQ(refused("{ std::println!(); use crate::{fake as std, T as U}; } T::V"), callback_line);
    EXPECT_EQ(
            refused("mod std { macro_rules! println { () => { return crate::T::Other; }; } pub(crate) use println; } "
                    "std::println!(); T::V"),
            callback_line);
    EXPECT_EQ(refused("{ use crate::fake as std; } std::println!(\"x\"); T::V"), std::nullopt);
    EXPECT_EQ(refused("{ mod std {} } std::println!(\"x\"); T::V"), std::nullopt);

    // A closure in the attribute binds its names nowhere the walk reaches, a skip's as a variant's, so one binding a
    // name is refused whatever it returns.
    EXPECT_EQ(
            line_of("use logos::Logos;\nmod fake { macro_rules! println { () => {}; } pub(crate) use println; }\n"
                    "#[derive(Logos)]\n#[logos(skip(\"x\", |_| { std::println!(); use crate::fake as std; }))]\n"
                    "enum T {\n    #[token(\"y\")]\n    V,\n}\n"),
            4);
}

TEST(Read_logos_test, A_wrapped_result_is_read_against_the_variant_s_payload)
{
    // For a variant without a payload, `Filter::Emit`, `FilterResult::Emit` and `Ok` may wrap `()` or any variant of
    // the enum, `Filter<T>` and `Result<T, T::Error>` being results the crate takes, so an expression the text does not
    // decide decides the token: logos 0.15.1 emits V then Other on "##" under each of these, where the rule's variant
    // is V; `Filter::Emit(())` and `Ok(T::V)` are read.
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "fn cb(lex: &mut Lexer<T>) -> Filter<T> { Filter::Emit(if lex.span().start == 0 { T::V } else "
                    "{ T::Other }) }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "fn cb(lex: &mut Lexer<T>) -> Result<T, ()> { Ok(if lex.span().start == 0 { T::V } else { "
                    "T::Other }) }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs",
                    "fn cb(lex: &mut Lexer<T>) -> FilterResult<T, ()> { FilterResult::Emit(if lex.span().start == "
                    "0 { T::V } else { T::Other }) }\n"),
            callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("#", |_| Filter::Emit(()))])rs"), std::nullopt);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", cb)])rs", "fn cb(_lex: &mut Lexer<T>) -> Result<T, ()> { Ok(T::V) }\n"),
            std::nullopt);
    EXPECT_EQ(callback_refused_at(R"rs(#[regex("[0-9]+", |lex| lex.slice().parse().ok())])rs"), callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", |lex| helper(lex))])rs", "fn helper(_lex: &mut Lexer<T>) -> Skip { Skip }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", maybe)])rs",
                    "fn maybe(lex: &mut Lexer<T>) -> Filter<()> { if lex.extras > 0 { Filter::Skip } else { "
                    "Filter::Emit(()) } }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", |lex| if lex.extras > 0 { Filter::Skip } else { Filter::Emit(()) })])rs"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(R"rs(#[token("#", |lex| if lex.extras > 0 { T::Other } else { T::V })])rs"),
            callback_line);

    // A match arm's pattern may carry braces, `| Point { value: 1 } =>`, so the bar opening it opens no closure and the
    // arm's own exit is the function's, which leaves the two outcomes the crate has.
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", arms)])rs",
                    "struct Point { value: u8 }\n"
                    "fn arms(_lex: &mut Lexer<T>) -> Result<Skip, ()> { match (Point { value: 1 }) { | Point { "
                    "value: 1 } => return Err(()), _ => {} }; Ok(Skip) }\n"),
            callback_line);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", arms)])rs",
                    "struct Point { value: u8 }\n"
                    "fn arms(_lex: &mut Lexer<T>) -> T { match (Point { value: 1 }) { | Point { value: 1 } => "
                    "T::Other, _ => T::Other } }\n"),
            variant_other);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("#", twice)])rs",
                    "fn twice(_lex: &mut Lexer<T>) -> Skip { Skip }\nfn twice() -> bool { true }\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(R"rs(#[token("#", X::f)])rs", "trait X { fn f(lex: &mut Lexer<T>) -> Filter<()>; }\n"),
            callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("#", |a, b| Skip)])rs"), callback_line);

    // A skip attribute's callback has no result to read: every result the crate admits there skips or fails.
    EXPECT_EQ(
            token_of("#[derive(Logos)]\n#[logos(extras = usize)]\n#[logos(skip(\"x\", |lex| lex.extras += 1))]\n"
                     "enum T {\n    V,\n}\n"),
            discarded);

    // The refusal names the callback and the reason.
    const auto deciding{callback_file(R"rs(#[token("#", |lex| if lex.extras > 0 { Skip } else { () })])rs", "", "V")};

    const auto [deciding_line, what]{refusal_at(deciding)};

    EXPECT_TRUE(deciding_line.has_value());
    EXPECT_TRUE(what.contains("|lex| if lex.extras > 0 { Skip } else { () }")) << what;
    EXPECT_TRUE(what.contains("run time")) << what;
}

TEST(Read_logos_test, A_callback_s_names_are_read_in_the_scope_its_text_stands_in)
{
    // A function's body is a scope of its own, so a `use` it writes binds there and the result is read there: the local
    // name is the variant it imports and not the crate's `Skip` of the same spelling, which the declaration module
    // would give it. The crate emits Other over the match for the first and discards it for the second.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", local)])rs",
                    "fn local(_lex: &mut Lexer<T>) -> T { use T::Other as Chosen; Chosen }\n"),
            variant_other);
    EXPECT_EQ(
            callback_token(R"rs(#[token("#", local)])rs", "fn local(_lex: &mut Lexer<T>) -> Skip { Skip }\n"),
            discarded);

    // A variant is bound under its enum, so the name holding it is `T::Other` in the scope around the enum while the
    // binding is written in the enum itself. The case is timed, since a qualified variant read against the enum's own
    // scope would ask whether it stands for `crate::T::T::Other`, which no item does, and resolve the canonical path as
    // itself once per pass a chain is allowed and once more inside each.
    const auto began{std::chrono::steady_clock::now()};

    EXPECT_EQ(
            callback_token(R"rs(#[token("#", qualified)])rs", "fn qualified(_lex: &mut Lexer<T>) -> T { T::Other }\n"),
            variant_other);

    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds{5});

    // A block inside the body is a scope of its own, so what it binds is bound under its own brace and the names it
    // writes are read there: the crate emits Other over the match for the first below.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", nested)])rs",
                    "fn nested(_lex: &mut Lexer<T>) -> T { { use T::Other as Chosen; Chosen } }\n"),
            variant_other);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", nested)])rs",
                    "fn nested(_lex: &mut Lexer<T>) -> T { { { use T::Other as Chosen; Chosen } } }\n"),
            variant_other);

    // The Ok arm of a `Result<Skip, E>` carries a `Skip` by its declared type, so it skips however the expression
    // inside it is written, while the Err arm is an error token at the boundary.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", tried)])rs",
                    "fn helper() -> Skip { Skip }\n"
                    "fn tried(_lex: &mut Lexer<T>) -> Result<Skip, ()> { Ok(helper()) }\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", tried)])rs", "fn tried(_lex: &mut Lexer<T>) -> Result<Skip, ()> { Ok(Skip) }\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", tried)])rs", "fn tried(_lex: &mut Lexer<T>) -> Result<Skip, ()> { Err(()) }\n"),
            variant_v);

    // A block Rust reads as a statement of its own ends it, so a `||` after `if false {}` opens a closure and is no
    // operator: the closure is never called and its own `return` is not the callback's.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", after)])rs",
                    "fn after(_lex: &mut Lexer<T>) -> T { if false {} || { return T::V; }; T::Other }\n"),
            variant_other);

    // Which of the two a brace group is depends on where it began: a block that opens a statement ends one, so the `||`
    // after it opens a closure whether or not the condition it follows is parenthesised, while a block standing where a
    // value does is the operator's left operand and the `||` after it is the operator.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", after)])rs",
                    "fn after(_lex: &mut Lexer<T>) -> T { if (false) {} || { return T::V; }; T::Other }\n"),
            variant_other);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", after)])rs",
                    "fn after(_lex: &mut Lexer<T>) -> T { match () { () => {} } || { return T::V; }; T::Other }\n"),
            variant_other);

    // A match arm's block is a scope of its own, as a body's block is, and it stands under the braces holding the arms,
    // which are a block too: the arm is read in its own block's scope, and `{ use T::Other as Skip; Skip }` names the
    // variant it imports.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", arms)])rs",
                    "fn arms(_lex: &mut Lexer<T>) -> T { match () { () => { use T::Other as Skip; Skip } } }\n"),
            variant_other);

    // A block's returns are read in the block's own scope, as its value is.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", nested)])rs",
                    "fn nested(_lex: &mut Lexer<T>) -> T { { { use T::Other as Skip; return Skip; } } }\n"),
            variant_other);

    // A closure written in the attribute binds its names nowhere the walk reaches, so a body that binds one is refused
    // whatever shape it takes: a block, a block one deeper, a parenthesised block or an `if` branch. A body binding
    // nothing is read.
    EXPECT_EQ(callback_refused_at(R"rs(#[token("#", |_| { use T::Other as Skip; Skip })])rs"), callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("#", |_| { { use T::Other as Skip; Skip } })])rs"), callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("#", |_| ({ use T::Other as Skip; Skip }))])rs"), callback_line);
    EXPECT_EQ(
            callback_refused_at(R"rs(#[token("#", |_| if true { use T::Other as Skip; Skip } else { Skip })])rs"),
            callback_line);
    EXPECT_EQ(callback_token(R"rs(#[token("#", |_| { { Skip } })])rs"), discarded);

    // An `if` branch is a block of its own and is read in its own scope.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("#", branch)])rs",
                    "fn branch(_lex: &mut Lexer<T>) -> T { use T::V as Choice; if true { use T::Other as Choice; "
                    "Choice } else { T::Other } }\n"),
            variant_other);
}

TEST(Read_logos_test, A_callback_result_is_read_against_the_variants_payload)
{
    // logos 0.15.1 converts the callback's result through `CallbackResult<P, T>` for the variant's payload type P, `()`
    // for a unit variant (logos-codegen 0.15.1, generator/leaf.rs): a value of type P, bare or in `Some`, `Ok` or an
    // `Emit` arm, is the payload and emits the variant, so on `V(logos::Skip)` each of `Skip`, `Some(Skip)`,
    // `Filter::Emit(Skip)`, `Ok(Skip)` and a function returning `Skip` or `Result<Skip, ()>` emits V, `Filter::Skip`
    // alone still skips, and `V(())` is a unit to it; on a unit variant `Some(Skip)` and `Filter::Emit(Skip)` are type
    // errors, as are `Skip`, `()`, `true`, `false` and the enum on `V(u64)` and a literal on a unit variant, which the
    // crate refuses and the reading refuses by name. Running the crate answers so on each, `Ok(Skip)` once its error
    // type is spelled, which rustc cannot infer from the bare closure.
    static constexpr std::string_view payload_head{
            "use logos::{Filter, Lexer, Logos, Skip};\n#[derive(Logos)]\n#[logos(extras = usize)]\n"};

    EXPECT_EQ(callback_token(R"rs(#[token("x", |_| logos::Skip)])rs", "", "V(logos::Skip)", payload_head), variant_v);
    EXPECT_EQ(
            callback_token(R"rs(#[token("x", |_| Some(logos::Skip))])rs", "", "V(logos::Skip)", payload_head),
            variant_v);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", |_| logos::Filter::Emit(logos::Skip))])rs", "", "V(logos::Skip)", payload_head),
            variant_v);
    EXPECT_EQ(
            callback_token(R"rs(#[token("x", |_| Ok(logos::Skip))])rs", "", "V(logos::Skip)", payload_head), variant_v);
    EXPECT_EQ(callback_token(R"rs(#[token("x", |_| Skip)])rs", "", "V(Skip)", payload_head), variant_v);
    EXPECT_EQ(
            callback_token(R"rs(#[token("x", |_| logos::Filter::Skip)])rs", "", "V(logos::Skip)", payload_head),
            discarded);
    EXPECT_EQ(callback_token(R"rs(#[token("x", |_| logos::Skip)])rs", "", "V(())", payload_head), discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", drop_it)])rs", "fn drop_it(_lex: &mut Lexer<T>) -> Skip { Skip }\n", "V(Skip)",
                    payload_head),
            variant_v);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", tried)])rs", "fn tried(_lex: &mut Lexer<T>) -> Result<Skip, ()> { Ok(Skip) }\n",
                    "V(Skip)", payload_head),
            variant_v);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", drop_it)])rs", "fn drop_it(_lex: &mut Lexer<T>) -> Skip { Skip }\n", "V",
                    payload_head),
            discarded);

    // A payload of another type takes the payload values, a skip arm still skips, and `None` and `Err` are an error at
    // the boundary whatever the variant.
    EXPECT_EQ(
            callback_token(R"rs(#[regex("[0-9]+", |lex| Some(lex.slice().len()))])rs", "", "V(usize)", payload_head),
            variant_v);
    EXPECT_EQ(callback_token(R"rs(#[token("x", |_| 42)])rs", "", "V(u64)", payload_head), variant_v);
    EXPECT_EQ(callback_token(R"rs(#[token("x", |_| Filter::Emit(42))])rs", "", "V(u64)", payload_head), variant_v);
    EXPECT_EQ(callback_token(R"rs(#[token("x", |_| Filter::Skip)])rs", "", "V(u64)", payload_head), discarded);
    EXPECT_EQ(callback_token(R"rs(#[token("x", |_| None)])rs", "", "V(u64)", payload_head), variant_v);
    EXPECT_EQ(callback_token(R"rs(#[token("x", |_| Err(()))])rs", "", "V(u64)", payload_head), variant_v);
    EXPECT_EQ(callback_token(R"rs(#[token("x", |_| None)])rs", "", "V", payload_head), variant_v);

    // What the crate refuses for the variant's payload is refused at the rule's line, in a closure and by a function's
    // return type alike.
    EXPECT_EQ(callback_refused_at(R"rs(#[token("x", |_| logos::Skip)])rs", "", "V(u64)", payload_head), callback_line);
    EXPECT_EQ(
            callback_refused_at(R"rs(#[token("x", |_| Ok(logos::Skip))])rs", "", "V(u64)", payload_head),
            callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("x", |_| T::Other)])rs", "", "V(u64)", payload_head), callback_line);
    EXPECT_EQ(
            callback_refused_at(R"rs(#[token("x", |_| Filter::Emit(T::Other))])rs", "", "V(u64)", payload_head),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(R"rs(#[token("x", |lex| { lex.extras += 1; })])rs", "", "V(u64)", payload_head),
            callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("x", |_| ())])rs", "", "V(u64)", payload_head), callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("x", |_| Some(()))])rs", "", "V(u64)", payload_head), callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("x", |_| true)])rs", "", "V(u64)", payload_head), callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("x", |_| Some(logos::Skip))])rs", "", "V", payload_head), callback_line);
    EXPECT_EQ(
            callback_refused_at(R"rs(#[token("x", |_| logos::Filter::Emit(logos::Skip))])rs", "", "V", payload_head),
            callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("x", |_| Some(T::Other))])rs", "", "V", payload_head), callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("x", |_| "payload")])rs", "", "V", payload_head), callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("x", |_| Some(42))])rs", "", "V", payload_head), callback_line);
    EXPECT_EQ(callback_refused_at(R"rs(#[token("x", |_| Ok(Filter::Skip))])rs", "", "V", payload_head), callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("x", drop_it)])rs", "fn drop_it(_lex: &mut Lexer<T>) -> Skip { Skip }\n", "V(u64)",
                    payload_head),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("x", maybe)])rs", "fn maybe(_lex: &mut Lexer<T>) -> Option<Skip> { None }\n", "V",
                    payload_head),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("x", count)])rs", "fn count(lex: &mut Lexer<T>) -> bool { true }\n", "V(u64)",
                    payload_head),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("x", note)])rs", "fn note(lex: &mut Lexer<T>) { lex.extras += 1; }\n", "V(u64)",
                    payload_head),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("x", other)])rs", "fn other(_lex: &mut Lexer<T>) -> T { T::Other }\n", "V(u64)",
                    payload_head),
            callback_line);

    // The refusal names the variant with its payload.
    const auto mismatched{callback_file(R"rs(#[token("x", |_| logos::Skip)])rs", "", "V(u64)", payload_head)};

    const auto [mismatched_line, mismatched_message]{refusal_at(mismatched)};

    EXPECT_TRUE(mismatched_line.has_value());
    EXPECT_TRUE(mismatched_message.contains("V(u64)")) << mismatched_message;
}

TEST(Read_logos_test, A_callback_is_read_through_the_names_the_file_binds)
{
    // The crate's names reach a callback as the file binds them: `use logos as lx` with `#[logos(crate = lx)]`, `use
    // logos::Skip as Drop`, a `type` alias of `Skip` or of `Result<Skip, ()>`, and a local `fn skip` in place of the
    // crate's are each read as what they stand for, and a local `struct Skip` is a payload, which logos 0.15.1 emits on
    // `V(Skip)` and refuses on a unit variant, while a call of an associated function, `T::make(0)`, names no variant
    // and is out of sight. Running the crate answers so on each.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", |_| lx::Skip)])rs", "", "V",
                    "use logos as lx;\nuse lx::Logos;\n#[logos(crate = lx)]\n#[derive(Logos)]\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", lx::skip)])rs", "", "V",
                    "use logos as lx;\nuse lx::Logos;\n#[logos(crate = lx)]\n#[derive(Logos)]\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", |_| Drop)])rs", "", "V", "use logos::{Logos, Skip as Drop};\n#[derive(Logos)]\n"),
            discarded);
    EXPECT_EQ(
            callback_token(R"rs(#[token("x", |_| logos::Skip)])rs", "", "V", "use logos::Logos;\n#[derive(Logos)]\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", drop_it)])rs", "fn drop_it(_lex: &mut Lexer<T>) -> Discard { logos::Skip }\n",
                    "V", "use logos::{Lexer, Logos};\ntype Discard = logos::Skip;\n#[derive(Logos)]\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", drop_it)])rs", "fn drop_it(_lex: &mut Lexer<T>) -> Discard { logos::Skip }\n",
                    "V(Discard)", "use logos::{Lexer, Logos};\ntype Discard = logos::Skip;\n#[derive(Logos)]\n"),
            variant_v);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", tried)])rs", "fn tried(_lex: &mut Lexer<T>) -> Outcome { Ok(logos::Skip) }\n",
                    "V", "use logos::{Lexer, Logos};\ntype Outcome = Result<logos::Skip, ()>;\n#[derive(Logos)]\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", skip)])rs", "", "V",
                    "use logos::{Lexer, Logos};\nfn skip(_lex: &mut Lexer<T>) -> bool { true }\n#[derive(Logos)]\n"),
            variant_v);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", |_| Skip)])rs", "", "V(Skip)",
                    "use logos::Logos;\nstruct Skip;\n#[derive(Logos)]\n"),
            variant_v);

    // The prelude's `Result` and `Option` are the same types by their paths in std and core and by an import of those:
    // the crate skips on `std::result::Result<Skip, ()>`, `::core::result::Result<Skip, ()>` and `R<Skip, ()>` under
    // `use std::result::Result as R`, and emits V on `std::option::Option<()>` returning `Some(())`.
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", tried)])rs",
                    "fn tried(_lex: &mut Lexer<T>) -> std::result::Result<Skip, ()> { Ok(Skip) }\n", "V",
                    "use logos::{Lexer, Logos, Skip};\n#[derive(Logos)]\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", tried)])rs",
                    "fn tried(_lex: &mut Lexer<T>) -> ::core::result::Result<Skip, ()> { Ok(Skip) }\n", "V",
                    "use logos::{Lexer, Logos, Skip};\n#[derive(Logos)]\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", tried)])rs", "fn tried(_lex: &mut Lexer<T>) -> R<Skip, ()> { Ok(Skip) }\n", "V",
                    "use logos::{Lexer, Logos, Skip};\nuse std::result::Result as R;\n#[derive(Logos)]\n"),
            discarded);
    EXPECT_EQ(
            callback_token(
                    R"rs(#[token("x", tried)])rs",
                    "fn tried(_lex: &mut Lexer<T>) -> std::option::Option<()> { Some(()) }\n", "V",
                    "use logos::{Lexer, Logos};\n#[derive(Logos)]\n"),
            variant_v);

    // A generic alias, `type R<T> = std::result::Result<T, ()>`, stands for what its arguments make of it, which the
    // crate skips on for `R<Skip>` and the reading does not substitute: refused by the alias's name, at the rule's
    // line, and never as a type the crate refuses.
    const auto aliased{callback_file(
            R"rs(#[token("x", tried)])rs", "fn tried(_lex: &mut Lexer<T>) -> R<Skip> { Ok(Skip) }\n", "V",
            "use logos::{Lexer, Logos, Skip};\ntype R<T> = std::result::Result<T, ()>;\n#[derive(Logos)]\n")};

    const auto [aliased_line, aliased_message]{refusal_at(aliased)};

    EXPECT_EQ(aliased_line, callback_line);
    EXPECT_TRUE(aliased_message.contains("through the generic alias `R`")) << aliased_message;
    EXPECT_FALSE(aliased_message.contains("the crate refuses it")) << aliased_message;

    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("x", |_| Skip)])rs", "", "V", "use logos::Logos;\nstruct Skip;\n#[derive(Logos)]\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("x", |_| logos::Skip)])rs", "", "V",
                    "use logos::Logos;\nmod logos { pub struct Skip; }\n#[derive(Logos)]\n"),
            callback_line);
    EXPECT_EQ(
            callback_refused_at(
                    R"rs(#[token("x", |_| T::make(0))])rs", "impl T {\n    fn make(_n: u8) -> T { T::Other }\n}\n", "V",
                    "use logos::Logos;\n#[derive(Logos)]\n"),
            4);
}

TEST(Read_logos_test, A_name_is_read_as_the_module_the_callback_stands_in_binds_it)
{
    // Rust binds a name in the module it stands in and nowhere else, and so does the reading: running logos 0.15.1 on
    // "xy" under each of these scanners answers as the assertions say. A `struct Skip` inside `mod unrelated` is no
    // binding where the enum stands, so the imported `logos::Skip` a callback returns skips x, as it does under `use
    // logos::*`, where the crate's `Skip` reaches the callback through the glob.
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos, Skip};\n#[derive(Logos)]\nenum T {\n    #[token(\"x\", drop_it)]\n"
                     "    V,\n    #[token(\"y\")]\n    Y,\n}\nfn drop_it(_: &mut Lexer<T>) -> Skip { Skip }\n"
                     "mod unrelated { pub struct Skip; }\n"),
            discarded);
    EXPECT_EQ(
            token_of("use logos::*;\n#[derive(Logos)]\nenum T {\n    #[token(\"x\", |_| Skip)]\n    V,\n"
                     "    #[token(\"y\")]\n    Y,\n}\nmod unrelated { pub struct Skip; }\n"),
            discarded);

    // A function of the enum's module is the one its bare name reaches, another module's `twice` notwithstanding; one
    // in a module is reached by its path, its own module's bindings resolving its type, `super::Discard` the root's
    // alias of the crate's `Skip`.
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos};\n#[derive(Logos)]\nenum T {\n    #[token(\"x\", twice)]\n    V,\n"
                     "    #[token(\"y\")]\n    Y,\n}\nfn twice(_: &mut Lexer<T>) -> logos::Skip { logos::Skip }\n"
                     "mod m { pub fn twice() -> bool { true } }\n"),
            discarded);
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos};\ntype Discard = logos::Skip;\n#[derive(Logos)]\nenum T {\n"
                     "    #[token(\"x\", callbacks::drop_it)]\n    V,\n    #[token(\"y\")]\n    Y,\n}\n"
                     "mod callbacks {\n"
                     "    pub fn drop_it(_: &mut super::Lexer<super::T>) -> super::Discard { logos::Skip }\n}\n"),
            discarded);

    // An enum inside a module reads the module's own bindings: the `struct Skip` beside it is its variant's payload,
    // which the crate emits, and a `use logos::Skip` at the root is no binding there.
    EXPECT_EQ(
            token_of(
                    "use logos::Skip;\nmod inner {\n    use logos::Logos;\n    pub struct Skip;\n    #[derive(Logos)]\n"
                    "    pub enum T {\n        #[token(\"x\", |_| Skip)]\n        V(Skip),\n        #[token(\"y\")]\n"
                    "        Y,\n    }\n}\n"),
            variant_v);
    EXPECT_EQ(
            token_of("mod inner {\n    use logos::{Logos, Skip};\n    #[derive(Logos)]\n    pub enum T {\n"
                     "        #[token(\"x\", |_| Skip)]\n        V,\n        #[token(\"y\")]\n        Y,\n    }\n}\n"
                     "struct Skip;\n"),
            discarded);

    // A glob of a module the file defines brings that module's bindings in, items declared after the `use` as well:
    // `use super::*;` inside `mod m` lets `m::f` return the root's `struct Skip`, the variant's payload, which the
    // crate emits as V(Skip), and `use self::inner::*` brings a nested module's struct to the enum; a glob of a local
    // module binding nothing relevant leaves the crate's `Skip` the crate's.
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos};\npub struct Skip;\n"
                     "mod m { use super::*; pub fn f(_: &mut Lexer<T>) -> Skip { Skip } }\n#[derive(Logos)]\n"
                     "enum T {\n    #[token(\"x\", m::f)]\n    V(Skip),\n    #[token(\"y\")]\n    Y,\n}\n"),
            variant_v);
    EXPECT_EQ(
            token_of("use logos::Logos;\npub mod inner { pub struct Skip; }\nuse self::inner::*;\n#[derive(Logos)]\n"
                     "enum T {\n    #[token(\"x\", |_| Skip)]\n    V(Skip),\n    #[token(\"y\")]\n    Y,\n}\n"),
            variant_v);
    EXPECT_EQ(
            token_of("use logos::{Logos, Skip};\npub mod other { pub fn helper() -> bool { true } }\n"
                     "use other::*;\n#[derive(Logos)]\nenum T {\n    #[token(\"x\", |_| Skip)]\n    V,\n"
                     "    #[token(\"y\")]\n    Y,\n}\n"),
            discarded);

    // A binding stands for the same wherever it is declared among the file's items, as Rust has it: `type D = Drop;`
    // inside `mod m` above `use logos::{Skip as Drop}` is the crate's `Skip` as it is below it, and so is `use
    // self::inner::Drop as D` inside `m` above the `mod inner` that binds `Drop`; the crate discards x on each.
    const auto aliased{[](const std::string_view first, const std::string_view second) {
        return std::format(
                "use logos::Logos;\nmod m {{\n    {}\n    {}\n    #[derive(Logos)]\n    pub enum T {{\n        "
                "#[token(\"x\", drop_it)]\n        V,\n        #[token(\"y\")]\n        Y,\n    }}\n    fn drop_it(_: "
                "&mut Lexer<T>) -> D {{ logos::Skip }}\n}}\n",
                first, second);
    }};

    EXPECT_EQ(token_of(aliased("use logos::{Lexer, Logos, Skip as Drop};", "type D = Drop;")), discarded);
    EXPECT_EQ(token_of(aliased("type D = Drop;", "use logos::{Lexer, Logos, Skip as Drop};")), discarded);
    EXPECT_EQ(
            token_of("use logos::Logos;\nmod m {\n    use logos::{Lexer, Logos};\n    use self::inner::Drop as D;\n"
                     "    #[derive(Logos)]\n    pub enum T {\n        #[token(\"x\", drop_it)]\n        V,\n"
                     "        #[token(\"y\")]\n        Y,\n    }\n"
                     "    fn drop_it(_: &mut Lexer<T>) -> D { logos::Skip }\n"
                     "    mod inner { pub use logos::Skip as Drop; }\n}\n"),
            discarded);
    EXPECT_EQ(
            token_of("use logos::Logos;\nmod m {\n    use logos::{Lexer, Logos};\n"
                     "    mod inner { pub use logos::Skip as Drop; }\n    use self::inner::Drop as D;\n"
                     "    #[derive(Logos)]\n    pub enum T {\n        #[token(\"x\", drop_it)]\n        V,\n"
                     "        #[token(\"y\")]\n        Y,\n    }\n"
                     "    fn drop_it(_: &mut Lexer<T>) -> D { logos::Skip }\n}\n"),
            discarded);

    // A method is the enum's whichever path its impl block spells the type by, as the block's module binds it: `impl
    // m::T`, `impl crate::m::T` and `impl self::m::T` at the root and `impl U` under `use self::m::T as U` define the
    // `T::drop_it` the enum inside `m` names, as `impl T` inside `m` does, and `Self` in each is the enum, so that
    // `Self::Y` returned is Y; the crate discards x under each and emits Y for the last.
    const auto impl_of{[](const std::string_view attribute, const std::string_view block) {
        return std::format(
                "use logos::{{Lexer, Logos, Skip}};\nuse self::m::T as U;\nmod m {{\n    use logos::Logos;\n    "
                "#[derive(Logos)]\n    pub enum T {{\n        #[token(\"x\", {})]\n        V,\n        "
                "#[token(\"y\")]\n        Y,\n    }}\n}}\n{}",
                attribute, block);
    }};

    EXPECT_EQ(
            token_of("use logos::Logos;\nmod m {\n    use logos::{Lexer, Logos, Skip};\n    #[derive(Logos)]\n"
                     "    pub enum T {\n        #[token(\"x\", T::drop_it)]\n        V,\n        #[token(\"y\")]\n"
                     "        Y,\n    }\n    impl T {\n        fn drop_it(_: &mut Lexer<Self>) -> Skip { Skip }\n"
                     "    }\n}\n"),
            discarded);
    EXPECT_EQ(
            token_of(impl_of("T::drop_it", "impl m::T {\n    fn drop_it(_: &mut Lexer<Self>) -> Skip { Skip }\n}\n")),
            discarded);
    EXPECT_EQ(
            token_of(impl_of(
                    "T::drop_it", "impl crate::m::T {\n    fn drop_it(_: &mut Lexer<Self>) -> Skip { Skip }\n}\n")),
            discarded);
    EXPECT_EQ(
            token_of(impl_of(
                    "T::drop_it", "impl self::m::T {\n    fn drop_it(_: &mut Lexer<Self>) -> Skip { Skip }\n}\n")),
            discarded);
    EXPECT_EQ(
            token_of(impl_of("T::drop_it", "impl U {\n    fn drop_it(_: &mut Lexer<Self>) -> Skip { Skip }\n}\n")),
            discarded);
    EXPECT_EQ(
            token_of(impl_of(
                    "T::mark", "impl crate::m::T {\n    fn mark(_: &mut Lexer<Self>) -> Self { Self::Y }\n}\n")),
            std::optional<std::string>{"Y"});
}

TEST(Read_logos_test, A_self_import_is_read_where_rustc_takes_one_and_refused_in_its_words_at_its_line)
{
    // rustc takes a `self` alone in a brace group under a module, which binds the module, and refuses a `self` ending
    // any other path, its error E0429, and one alone in a group under no module, its error E0431; each refusal names
    // the line of the `self`.
    const auto reason{[](const std::string_view items) {
        const auto file{std::format(
                "use logos::Logos;\n{}#[derive(Logos)]\nenum T {{\n    #[token(\"x\")]\n    V,\n}}\n", items)};

        return refusal_of(file);
    }};

    const std::string unbraced{
            "rustc refuses a `self` import outside a { } list: `self` imports are only allowed within a { } list"};

    EXPECT_EQ(reason("use self;\n"), "line 2: " + unbraced);
    EXPECT_EQ(reason("use self as n;\n"), "line 2: " + unbraced);
    EXPECT_EQ(reason("mod m { pub struct S; }\nuse m::self;\n"), "line 3: " + unbraced);
    EXPECT_EQ(reason("mod m {\n    use super::self;\n}\n"), "line 3: " + unbraced);
    EXPECT_EQ(reason("mod m { pub struct S; }\nuse {m::self};\n"), "line 3: " + unbraced);
    EXPECT_EQ(reason("use self\n;\n"), "line 2: " + unbraced);
    EXPECT_EQ(reason("use self // c\n ;\n"), "line 2: " + unbraced);

    const std::string unprefixed{
            "rustc refuses a `self` import with no module before it: "
            "`self` import can only appear in an import list with a non-empty prefix"};

    EXPECT_EQ(reason("use {self};\n"), "line 2: " + unprefixed);
    EXPECT_EQ(reason("use {self as n};\n"), "line 2: " + unprefixed);
    EXPECT_EQ(reason("use ::{self};\n"), "line 2: " + unprefixed);
    EXPECT_EQ(reason("use {\n    self\n};\n"), "line 3: " + unprefixed);

    // The module a `self` in a group binds is the one its path names: a callback returning `m::Skip` or `n::Skip`
    // through it skips x where the module re-exports the crate's `Skip`, and emits V, its payload, where the module
    // holds a struct of that name.
    const auto token_through{[](const std::string_view item, const std::string_view import,
                                const std::string_view name) {
        const auto skip{std::format("{}::Skip", name)};

        const auto variant{item.starts_with("pub struct") ? std::format("V({})", skip) : std::string{"V"}};

        const auto source{std::format(
                "use logos::Logos;\nmod outer {{\n    pub mod m {{ {} }}\n}}\n{}\n#[derive(Logos)]\nenum T {{\n    "
                "#[token(\"x\", |_| {})]\n    {},\n    #[token(\"y\")]\n    Y,\n}}\n",
                item, import, skip, variant)};

        return token_of(source);
    }};

    EXPECT_EQ(token_through("pub use logos::Skip;", "use outer::m::{self};", "m"), discarded);
    EXPECT_EQ(token_through("pub struct Skip;", "use outer::m::{self};", "m"), variant_v);
    EXPECT_EQ(token_through("pub use logos::Skip;", "use outer::m::{self as n};", "n"), discarded);
    EXPECT_EQ(token_through("pub struct Skip;", "use outer::m::{self as n};", "n"), variant_v);
}

TEST(Read_logos_test, A_macros_text_and_an_expression_block_declare_no_item_of_the_module)
{
    // A `macro_rules!` body is text for the macro until it is expanded, so the enum inside an unused one is no scanner:
    // logos 0.15.1 on "xy" under the file with `T` and the macro prints `Ok(V) 0..1` and `Err(()) 1..2`, T's answer
    // alone; a `struct Skip` in such a body binds nothing by the same rule, and the callback's `Skip` stays the
    // imported crate's.
    constexpr std::string_view ghost{
            "use logos::Logos;\nmacro_rules! unused {\n    () => {\n        #[derive(Logos)]\n"
            "        enum Ghost { #[token(\"g\")] G }\n    };\n}\n#[derive(Logos, Debug)]\npub enum T {\n"
            "    #[token(\"x\")] V,\n}\n"};

    const auto ghost_scanners{read_logos(ghost)};

    EXPECT_EQ(ghost_scanners.size(), 1U);
    EXPECT_EQ(ghost_scanners.front().line, 8U);
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos, Skip};\nmacro_rules! unused { () => { struct Skip; }; }\n"
                     "#[derive(Logos)]\nenum T {\n    #[token(\"x\", drop_it)]\n    V,\n    #[token(\"y\")]\n"
                     "    Y,\n}\nfn drop_it(_: &mut Lexer<T>) -> Skip { Skip }\n"),
            discarded);

    // A block that is an expression is no module: the `struct Skip` of `const _: () = { struct Skip; };` is no binding
    // at the root, so the callback's `Skip` is the imported crate's and x is discarded, as the crate prints `Ok(Y)
    // 1..2` alone; a static's initializer is an expression too. An enum inside such a block, or inside a function's
    // body, is derived where it stands and is a scanner: the crate prints `Ok(Y) 1..2` for the file with `U` in a
    // constant's block before `T`, and `Ok(V) 0..1` and `Err(()) 1..2` for the enum inside `main`.
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos, Skip};\nconst _: () = { struct Skip; };\n#[derive(Logos, Debug)]\n"
                     "pub enum T {\n    #[token(\"x\", drop_it)] V,\n    #[token(\"y\")] Y,\n}\n"
                     "fn drop_it(_: &mut Lexer<T>) -> Skip { Skip }\n"),
            discarded);
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos, Skip};\nstatic N: u8 = { struct Skip; 1 };\n#[derive(Logos, Debug)]\n"
                     "pub enum T {\n    #[token(\"x\", drop_it)] V,\n    #[token(\"y\")] Y,\n}\n"
                     "fn drop_it(_: &mut Lexer<T>) -> Skip { Skip }\n"),
            discarded);

    const auto inside_const{
            read_logos("use logos::{Lexer, Logos, Skip};\nconst _: () = {\n    #[derive(Logos, Debug)]\n"
                       "    enum U { #[token(\"u\")] U1 }\n};\n#[derive(Logos, Debug)]\npub enum T {\n"
                       "    #[token(\"x\", drop_it)] V,\n    #[token(\"y\")] Y,\n}\n"
                       "fn drop_it(_: &mut Lexer<T>) -> Skip { Skip }\n")};

    ASSERT_EQ(inside_const.size(), 2U);
    EXPECT_EQ(inside_const.at(0).line, 3U);
    EXPECT_EQ(inside_const.at(1).rules.front().token, discarded);

    const auto inside_main{
            read_logos("use logos::Logos;\nfn main() {\n    #[derive(Logos, Debug)]\n"
                       "    enum T { #[token(\"x\")] V }\n    let mut l = T::lexer(\"xy\");\n"
                       "    while let Some(t) = l.next() { println!(\"{:?}\", t); }\n}\n")};

    ASSERT_EQ(inside_main.size(), 1U);
    EXPECT_EQ(inside_main.front().line, 3U);

    // A constant's item runs to its semicolon and a struct's generic parameters declare no item, so a `const N` among
    // them, with a default or without, leaves the `use` after the struct standing.
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos};\nstruct S<const N: usize = 3> { bytes: [u8; N] }\n"
                     "use logos::Skip as Drop;\n#[derive(Logos)]\nenum T {\n    #[token(\"x\", drop_it)]\n    V,\n"
                     "    #[token(\"y\")]\n    Y,\n}\nfn drop_it(_: &mut Lexer<T>) -> Drop { Drop }\n"),
            discarded);
}

TEST(Read_logos_test, A_glob_brings_in_what_the_importing_module_may_see)
{
    // A glob of a child module brings in its `pub` items alone, as Rust has it: `use inner::*` at the root leaves the
    // private `struct Skip` of `mod inner` where it is, so the callback's `Skip` is the crate's through `use logos::*`
    // and x is discarded, as logos 0.15.1 on "xy" prints `Ok(Y) 1..2` alone. A `pub struct Skip` in the child is
    // brought in and is the variant's payload, which the crate emits as `Ok(V(Skip)) 0..1` then `Ok(Y) 1..2`, with the
    // crate's names imported one by one, since `use logos::*` beside `use inner::*` would leave `Skip` ambiguous
    // between the two globs, an error Rust refuses rather than a shadow; `pub(crate)` counts as `pub`, and a `pub use`
    // of the child is brought in as the child's own item is.
    EXPECT_EQ(
            token_of("use logos::*;\nmod inner { pub struct Other; struct Skip; }\nuse inner::*;\n"
                     "#[derive(Logos, Debug)]\npub enum T {\n    #[token(\"x\", drop_it)] V,\n"
                     "    #[token(\"y\")] Y,\n}\nfn drop_it(_: &mut Lexer<T>) -> Skip { Skip }\n"),
            discarded);
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos};\nmod inner { pub struct Other; #[derive(Debug)] pub struct Skip; }\n"
                     "use inner::*;\n#[derive(Logos, Debug)]\npub enum T {\n    #[token(\"x\", drop_it)] V(Skip),\n"
                     "    #[token(\"y\")] Y,\n}\nfn drop_it(_: &mut Lexer<T>) -> Skip { Skip }\n"),
            variant_v);
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos};\nmod inner { pub(crate) struct Skip; }\nuse inner::*;\n"
                     "#[derive(Logos)]\nenum T {\n    #[token(\"x\", drop_it)] V(Skip),\n    #[token(\"y\")] Y,\n}\n"
                     "fn drop_it(_: &mut Lexer<T>) -> Skip { Skip }\n"),
            variant_v);
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos};\nmod inner { pub use logos::Skip as Discard; }\nuse inner::*;\n"
                     "#[derive(Logos)]\nenum T {\n    #[token(\"x\", drop_it)] V,\n    #[token(\"y\")] Y,\n}\n"
                     "fn drop_it(_: &mut Lexer<T>) -> Discard { Discard }\n"),
            discarded);

    // A glob of a module the importing one stands under sees everything of it: `use super::*;` inside `mod m` brings
    // the root's private `struct Skip` in, so `m::drop_it` returns the variant's payload, which the crate emits as
    // `Ok(V(Skip)) 0..1` then `Ok(Y) 1..2`.
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos};\n#[derive(Debug)]\nstruct Skip;\n"
                     "mod m { use super::*; pub fn drop_it(_: &mut Lexer<T>) -> Skip { Skip } }\n"
                     "#[derive(Logos, Debug)]\npub enum T {\n    #[token(\"x\", m::drop_it)] V(Skip),\n"
                     "    #[token(\"y\")] Y,\n}\n"),
            variant_v);
}

TEST(Read_logos_test, A_pub_glob_exports_the_binding_a_private_glob_of_the_same_module_brought_in)
{
    // Two globs of one module bring in the same binding, as Rust has it, one binding exported where either glob is:
    // `use crate::a::*;` beside `pub use crate::a::*;` inside `mod b` makes `b::Drop` the crate's `Skip` and public, in
    // either order, so `use b::*` at the root reaches it and x is discarded, as logos 0.15.1 on "xy" prints `Ok(Y)
    // 1..2` alone. A binding of the module's own under the name, `pub struct Drop` inside `b`, shadows both globs and
    // is the variant's payload, which the crate emits as `Ok(V(Drop)) 0..1` then `Ok(Y) 1..2`.
    const auto through{[](const std::string_view b, const std::string_view payload) {
        return std::format(
                "use logos::{{Lexer, Logos}};\nmod a {{ pub use logos::Skip as Drop; }}\nmod b {{ {} }}\nuse "
                "b::*;\n#[derive(Logos, Debug)]\npub enum T {{\n    #[token(\"x\", drop_it)] V{},\n    #[token(\"y\")] "
                "Y,\n}}\nfn drop_it(_: &mut Lexer<T>) -> Drop {{ Drop }}\n",
                b, payload);
    }};

    EXPECT_EQ(token_of(through("use crate::a::*; pub use crate::a::*;", "")), discarded);
    EXPECT_EQ(token_of(through("pub use crate::a::*; use crate::a::*;", "")), discarded);
    EXPECT_EQ(
            token_of(through("#[derive(Debug)] pub struct Drop; use crate::a::*; pub use crate::a::*;", "(Drop)")),
            variant_v);
}

TEST(Read_logos_test, A_leading_double_colon_names_the_crate_whatever_the_file_defines_under_the_name)
{
    // A path with a leading `::` is an external crate's, as Rust 2021 has it, whatever the root defines under its head:
    // `use ::logos::{Lexer, Logos, Skip}` beside `mod logos {}` is the crate's, and so are `::lx::Skip` and `lx::Skip`
    // under `extern crate logos as lx`, the alias followed and no module of the file's after it, so x is discarded on
    // each, as logos 0.15.1 on "xy" prints `Ok(Y) 1..2` alone. `crate::logos::Skip` and `self::logos::Skip` reach the
    // module of the file's, whose `pub struct Skip` is the variant's payload, which the crate emits as `Ok(V(Skip))
    // 0..1` then `Ok(Y) 1..2`.
    const auto beside{[](const std::string_view head, const std::string_view payload) {
        return std::format(
                "{}#[derive(Logos, Debug)]\npub enum T {{\n    #[token(\"x\", drop_it)] V{},\n    #[token(\"y\")] "
                "Y,\n}}\nfn drop_it(_: &mut Lexer<T>) -> Skip {{ Skip }}\n",
                head, payload);
    }};

    EXPECT_EQ(token_of(beside("mod logos {}\nuse ::logos::{Lexer, Logos, Skip};\n", "")), discarded);
    EXPECT_EQ(
            token_of(beside("extern crate logos as lx;\nmod logos {}\nuse ::lx::{Lexer, Logos, Skip};\n", "")),
            discarded);
    EXPECT_EQ(
            token_of(beside("extern crate logos as lx;\nmod logos {}\nuse lx::{Lexer, Logos, Skip};\n", "")),
            discarded);
    EXPECT_EQ(
            token_of(
                    beside("mod logos { #[derive(Debug)] pub struct Skip; }\nuse ::logos::{Lexer, Logos};\n"
                           "use crate::logos::Skip;\n",
                           "(Skip)")),
            variant_v);
    EXPECT_EQ(
            token_of(
                    beside("mod logos { #[derive(Debug)] pub struct Skip; }\nuse ::logos::{Lexer, Logos};\n"
                           "use self::logos::Skip;\n",
                           "(Skip)")),
            variant_v);
}

TEST(Read_logos_test, An_enum_or_a_variant_a_cfg_strips_is_no_scanner_or_rule)
{
    // A `cfg` predicate false by its form alone strips the item before the derive runs, as rustc has it: `Ghost` under
    // `#[cfg(any())]` is no scanner and the file holds `T` alone, at its own line, as building it shows; `all()` and
    // `not(any())` are true by form and leave the enum standing, `all(all(), any())` false. A predicate the build alone
    // decides, `feature = "never"`, leaves the enum read as standing, and the options say so. A variant under
    // `#[cfg(any())]` is no rule of its enum.
    const auto ghost{[](const std::string_view predicate) {
        return std::format(
                "use logos::Logos;\n#[cfg({})]\n#[derive(Logos, Debug, PartialEq)]\npub enum Ghost {{ #[token(\"q\")] "
                "Q }}\n#[derive(Logos, Debug, PartialEq)]\npub enum T {{ #[token(\"x\")] X, #[token(\"y\")] Y }}\n",
                predicate);
    }};

    const auto never{read_logos(ghost("any()"))};

    const auto never_nested{read_logos(ghost("all(all(), any())"))};

    const auto always{read_logos(ghost("all()"))};

    const auto never_negated{read_logos(ghost("not(any())"))};

    const auto built{read_logos(ghost(R"(feature = "never")"))};

    EXPECT_EQ(never.size(), 1U);
    EXPECT_EQ(never.front().line, 5U);
    EXPECT_EQ(never_nested.size(), 1U);
    EXPECT_EQ(always.size(), 2U);
    EXPECT_EQ(never_negated.size(), 2U);
    EXPECT_EQ(built.size(), 2U);
    EXPECT_EQ(built.front().options, (std::vector<std::string>{"unicode-classes=16.0.0", R"(cfg=feature="never")"}));
    EXPECT_EQ(always.front().options, (std::vector<std::string>{"unicode-classes=16.0.0"}));

    const auto stripped{
            read_logos("use logos::Logos;\n#[derive(Logos, Debug, PartialEq)]\npub enum T {\n"
                       "    #[cfg(any())] #[token(\"q\")] Q,\n    #[token(\"x\")] X,\n    #[token(\"y\")] Y,\n}\n")};

    ASSERT_EQ(stripped.size(), 1U);
    EXPECT_EQ(stripped.front().rules.size(), 2U);
    EXPECT_EQ(stripped.front().rules.front().token, variant_x);

    // What a false predicate strips binds no name either, since rustc strips the item before a name is resolved: with
    // the variant `Skip` stripped, the callback's `Skip` is the crate's and the crate discards x, printing Y alone at
    // 1..2 on "xy"; the same holds for a stripped enum of that name.
    const auto variant{
            read_logos("use logos::*;\nuse T::*;\n#[derive(Logos)]\npub enum T {\n    #[cfg(any())] Skip,\n"
                       "    #[token(\"x\", |_| Skip)] X,\n    #[token(\"y\")] Y,\n}\n")};

    ASSERT_EQ(variant.size(), 1U);
    EXPECT_EQ(variant.front().rules.front().token, discarded);

    const auto shadow{
            read_logos("use logos::*;\n#[cfg(any())]\nenum Skip {}\n#[derive(Logos)]\npub enum T {\n"
                       "    #[token(\"x\", |_| Skip)] X,\n    #[token(\"y\")] Y,\n}\n")};

    ASSERT_EQ(shadow.size(), 1U);
    EXPECT_EQ(shadow.front().rules.front().token, discarded);

    // An attribute's `#` and `[` take whatever blanks and comments Rust allows between them, so `# [derive(Logos)]`
    // declares the scanner `#[derive(Logos)]` does.
    const auto spaced{
            read_logos("use logos::{Lexer, Logos, Skip};\n# [derive(Logos)]\npub enum T {\n"
                       "    #[token(\"x\", cb)] X,\n    #[token(\"y\")] Y,\n}\n"
                       "fn cb(_: &mut Lexer<T>) -> Skip { Skip }\n")};

    ASSERT_EQ(spaced.size(), 1U);
    EXPECT_EQ(spaced.front().rules.size(), 2U);
    EXPECT_EQ(spaced.front().rules.front().token, discarded);
}

TEST(Read_logos_test, An_item_a_cfg_strips_declares_nothing_a_pub_before_it_notwithstanding)
{
    // rustc strips an item under a `#[cfg(...)]` false by its form before a name is resolved, so a stripped function
    // neither binds its name nor answers for a callback: with the live `callback` returning `bool` imported from `mod
    // live` and a `#[cfg(any())] fn callback` returning `Skip` declared at the root, logos 0.15.1 on "xxy" prints
    // `Ok(X) 0..2` then `Ok(Y) 2..3`; the same file with both functions at the root prints the same. The attributes
    // stand before a `pub`, which is the item's too, so a stripped `pub const Skip` and a stripped `pub enum Skip` are
    // gone as the private ones are: the crate prints `Ok(Y) 2..3` alone for the closure's `Skip` beside the constant,
    // and for a function returning the enum's `Skip`.
    EXPECT_EQ(
            token_of("use logos::{Logos, Skip};\nmod live {\n"
                     "    pub fn callback(_: &mut logos::Lexer<super::T>) -> bool { true }\n}\nuse live::callback;\n"
                     "#[cfg(any())]\nfn callback(_: &mut logos::Lexer<T>) -> Skip { Skip }\n#[derive(Logos)]\n"
                     "enum T {\n    #[regex(\"x+\", callback)] X,\n    #[token(\"y\")] Y,\n}\n"),
            variant_x);
    EXPECT_EQ(
            token_of("use logos::{Logos, Skip};\nfn callback(_: &mut logos::Lexer<T>) -> bool { true }\n"
                     "#[cfg(any())]\nfn callback(_: &mut logos::Lexer<T>) -> Skip { Skip }\n#[derive(Logos)]\n"
                     "enum T {\n    #[regex(\"x+\", callback)] X,\n    #[token(\"y\")] Y,\n}\n"),
            variant_x);
    EXPECT_EQ(
            token_of("use logos::{Logos, Skip};\n#[cfg(any())]\npub const Skip: () = ();\n#[derive(Logos)]\n"
                     "enum T {\n    #[regex(\"x+\", |_| Skip)] X,\n    #[token(\"y\")] Y,\n}\n"),
            discarded);
    EXPECT_EQ(
            token_of("use logos::{Logos, Skip};\n#[cfg(any())]\npub enum Skip {}\n"
                     "fn drop_it(_: &mut logos::Lexer<T>) -> Skip { Skip }\n#[derive(Logos)]\n"
                     "enum T {\n    #[regex(\"x+\", drop_it)] X,\n    #[token(\"y\")] Y,\n}\n"),
            discarded);
}

TEST(Read_logos_test, A_block_is_a_scope_of_its_own_whose_items_a_scanner_inside_it_sees)
{
    // A block, a function's body among them, is a scope of its own: an item declared in it is in sight inside the block
    // and the blocks within it and nowhere outside, and the block sees the module it stands in, as Rust has it. A `fn
    // skip` returning `bool` declared in `main` beside a scanner, with `logos::skip` imported at the root, is that
    // scanner's callback: logos 0.15.1 on "xxy" prints `Ok(X) 0..2` then `Ok(Y) 2..3`; a bare block inside `main`
    // holding both prints the same. A `fn skip` inside `main` is out of sight of a scanner at the root, whose `skip` is
    // the root's function returning `logos::Skip`, the crate printing `Ok(Y) 2..3` alone, and a scanner inside `main`
    // reaches `drop_it` at the root through the module the block stands in, printing `Ok(Y) 2..3` alone as well. The
    // walk that binds the items is the one that finds the scanners, so an enum inside a function a `cfg` strips is
    // none: the crate builds the file with `Ghost` inside `#[cfg(any())] fn unused()` and runs `T` alone.
    EXPECT_EQ(
            token_of("use logos::{skip, Logos};\nfn main() {\n    fn skip(_: &mut logos::Lexer<T>) -> bool { true }\n"
                     "    #[derive(Logos)]\n    enum T {\n        #[regex(\"x+\", skip)] X,\n"
                     "        #[token(\"y\")] Y,\n    }\n}\n"),
            variant_x);
    EXPECT_EQ(
            token_of("use logos::{skip, Logos};\nfn main() {\n    {\n"
                     "        fn skip(_: &mut logos::Lexer<T>) -> bool { true }\n        #[derive(Logos)]\n"
                     "        enum T {\n            #[regex(\"x+\", skip)] X,\n            #[token(\"y\")] Y,\n"
                     "        }\n    }\n}\n"),
            variant_x);
    EXPECT_EQ(
            token_of("use logos::Logos;\nfn skip(_: &mut logos::Lexer<T>) -> logos::Skip { logos::Skip }\n"
                     "#[derive(Logos)]\nenum T {\n    #[regex(\"x+\", skip)] X,\n    #[token(\"y\")] Y,\n}\n"
                     "fn main() {\n    fn skip(_: &mut logos::Lexer<T>) -> bool { true }\n}\n"),
            discarded);
    EXPECT_EQ(
            token_of("use logos::Logos;\n"
                     "fn drop_it<'a, T: Logos<'a>>(_: &mut logos::Lexer<'a, T>) -> logos::Skip { logos::Skip }\n"
                     "fn main() {\n    #[derive(Logos)]\n    enum T {\n        #[regex(\"x+\", drop_it)] X,\n"
                     "        #[token(\"y\")] Y,\n    }\n}\n"),
            discarded);

    const auto stripped{
            read_logos("use logos::Logos;\n#[cfg(any())]\nfn unused() {\n    #[derive(Logos)]\n"
                       "    enum Ghost { #[token(\"q\")] Q }\n}\n#[derive(Logos)]\n"
                       "enum T { #[token(\"x\")] X, #[token(\"y\")] Y }\n")};

    ASSERT_EQ(stripped.size(), 1U);
    EXPECT_EQ(stripped.front().line, 7U);
}

TEST(Read_logos_test, A_cfg_attr_applies_the_attributes_it_carries_as_its_predicate_decides)
{
    // rustc expands `#[cfg_attr(predicate, a, b)]` into `#[a] #[b]` where the predicate holds and into nothing where it
    // does not, before any derive runs. Under `all()` the `regex("x+", priority = 3)` on `Run` is a rule: logos 0.15.1
    // on "xxy" prints `Ok(Run) 0..2` then `Ok(Y) 2..3`; under `any()` the variant has no rule, and the crate prints
    // `Ok(X) 0..1`, `Ok(X) 1..2` and `Ok(Y) 2..3`. Under a predicate the build alone decides, `not(feature = "never")`,
    // the attributes are applied, as an enum under such a `cfg` is read as standing, and the options say so; built
    // without the feature, the crate prints `Ok(Run) 0..2` then `Ok(Y) 2..3` for the file whose derive stands under a
    // `cfg_attr(all(), ...)` as well. On the enum, a `logos(skip ...)` under `cfg_attr(all(), ...)` is a discarded rule
    // ahead of the variants: the crate prints `Ok(X) 2..3` then `Ok(Y) 3..4` on "zzxy".
    const auto run{[](const std::string_view predicate) {
        return std::format(
                "use logos::Logos;\n#[derive(Logos)]\nenum T {{\n    #[cfg_attr({}, regex(\"x+\", priority = 3))]\n    "
                "Run,\n    #[token(\"x\", priority = 2)]\n    X,\n    #[token(\"y\")]\n    Y,\n}}\n",
                predicate);
    }};

    const auto applied{read_logos(run("all()"))};

    ASSERT_EQ(applied.front().rules.size(), 3U);

    const auto& run_rule{applied.front().rules.front()};

    EXPECT_EQ(run_rule.token, std::optional<std::string>{"Run"});
    EXPECT_EQ(run_rule.pattern, R"("x+")");
    EXPECT_EQ(run_rule.priority, std::optional<std::size_t>{3});
    EXPECT_EQ(applied.front().options, (std::vector<std::string>{"unicode-classes=16.0.0"}));

    const auto stripped{read_logos(run("any()"))};

    ASSERT_EQ(stripped.front().rules.size(), 2U);
    EXPECT_EQ(stripped.front().rules.front().token, variant_x);

    const auto assumed{
            read_logos("use logos::Logos;\n#[cfg_attr(all(), derive(Logos))]\nenum T {\n"
                       "    #[cfg_attr(not(feature = \"never\"), regex(\"x+\", priority = 3))]\n    Run,\n"
                       "    #[token(\"x\", priority = 2)]\n    X,\n    #[token(\"y\")]\n    Y,\n}\n")};

    ASSERT_EQ(assumed.size(), 1U);
    EXPECT_EQ(assumed.front().line, 2U);
    ASSERT_EQ(assumed.front().rules.size(), 3U);
    EXPECT_EQ(assumed.front().rules.front().token, std::optional<std::string>{"Run"});
    EXPECT_EQ(
            assumed.front().options,
            (std::vector<std::string>{"unicode-classes=16.0.0", R"(cfg=not(feature="never"))"}));

    const auto skipping{
            read_logos("use logos::Logos;\n#[derive(Logos)]\n#[cfg_attr(all(), logos(skip r\"z+\"))]\nenum T {\n"
                       "    #[token(\"x\", priority = 2)]\n    X,\n    #[token(\"y\")]\n    Y,\n}\n")};

    ASSERT_EQ(skipping.front().rules.size(), 3U);
    EXPECT_EQ(skipping.front().rules.front().token, discarded);
    EXPECT_EQ(skipping.front().rules.front().pattern, R"(r"z+")");
}

TEST(Read_logos_test, A_function_returning_Result_of_Skip_is_read_by_its_body)
{
    // `Result<Skip, E>` skips only on `Ok(Skip)`: an `Err` is an error token at the same boundary, as logos 0.15.1 on
    // "xy" prints `Err(()) 0..1` then `Ok(Y) 1..2` for a function returning `Err(())`; `Ok(Skip)` discards x, the crate
    // printing `Ok(Y) 1..2` alone, and a body that skips on one path and errs on another is refused, the rule's token
    // being decided at run time.
    const auto returning{[](const std::string_view body) {
        return std::format(
                "use logos::{{Lexer, Logos, Skip}};\n#[derive(Logos, Debug, PartialEq)]\npub enum T {{\n    "
                "#[token(\"x\", cb)] X,\n    #[token(\"y\")] Y,\n}}\nfn cb(lex: &mut Lexer<T>) -> Result<Skip, ()> {{ "
                "{} }}\n",
                body);
    }};

    EXPECT_EQ(token_of(returning("Err(())")), variant_x);
    EXPECT_EQ(token_of(returning("Ok(Skip)")), discarded);
    EXPECT_NE(line_of(returning(R"(if lex.slice() == "x" { Ok(Skip) } else { Err(()) })")), std::nullopt);

    // A `?` returns the `Err` of its value before anything after it runs, an error token at the boundary: the crate
    // prints `Err(()) 0..1` on "xy" for the body below; with the tail a skip the body is mixed, and refused as one.
    EXPECT_NE(line_of(returning("Err::<(), ()>(())?; Ok(Skip)")), std::nullopt);
    EXPECT_EQ(token_of(returning("Err::<(), ()>(())?; Err(())")), variant_x);
}

TEST(Read_logos_test, A_closures_returns_and_question_marks_are_its_own)
{
    // A closure is a callable of its own, so a `return` in its body returns from it and a `?` returns its `Err` from
    // it, neither from the callback around it: logos 0.15.1 on "xy" prints `Ok(Y) 1..2` alone for `cb` returning
    // `Ok(Skip)` after `let _unused = || -> Result<(), ()> { Err::<(), ()>(())?; Ok(()) };`, and `Ok(X) 0..1` then
    // `Ok(Y) 1..2` for `cb` returning `T::X` after `let _unused = move || -> T { return T::Y; };` and `let _bits = 1 |
    // 2;`, whose bar is the bitwise or. A `?` in the callback's own body still exits it: the crate prints `Err(())
    // 0..1` then `Ok(Y) 1..2` on "xy" for `Err::<(), ()>(())?; Err(())`, and a body skipping on the tail and erring on
    // the `?` is refused as mixed.
    const auto returning{[](const std::string_view type, const std::string_view body) {
        return std::format(
                "use logos::{{Lexer, Logos, Skip}};\n#[derive(Logos, Debug, PartialEq)]\npub enum T {{\n    "
                "#[token(\"x\", cb)] X,\n    #[token(\"y\")] Y,\n}}\nfn cb(_: &mut Lexer<T>) -> {} {{\n    {}\n}}\n",
                type, body);
    }};

    EXPECT_EQ(
            token_of(returning(
                    "Result<Skip, ()>",
                    "let _unused = || -> Result<(), ()> {\n        Err::<(), ()>(())?;\n        Ok(())\n    };\n"
                    "    Ok(Skip)")),
            discarded);
    EXPECT_EQ(
            token_of(returning(
                    "T",
                    "let _unused = move || -> T {\n        return T::Y;\n    };\n    let _bits = 1 | 2;\n    T::X")),
            variant_x);
    EXPECT_EQ(token_of(returning("Result<Skip, ()>", "Err::<(), ()>(())?; Err(())")), variant_x);
    EXPECT_NE(line_of(returning("Result<Skip, ()>", "Err::<(), ()>(())?; Ok(Skip)")), std::nullopt);
}

TEST(Read_logos_test, A_function_returning_the_payloads_own_type_returns_the_payload)
{
    // A value of the payload's own type is the payload whatever the type, as logos's blanket conversion has it: a
    // function returning `Filter<()>` to `V(Filter<()>)` emits V, the crate on "xy" emitting a token at 0..1 and Y at
    // 1..2, `Filter<()>` carrying no `Debug` for the fixture to print; one returning `Filter<Filter<()>>` is read by
    // its arm, `Filter::Skip` discarding x as the crate prints `Ok(Y) 1..2` alone.
    const auto returning{[](const std::string_view type) {
        return std::format(
                "use logos::{{Filter, Lexer, Logos}};\n#[derive(Logos)]\npub enum T {{\n    #[token(\"x\", cb)] "
                "V(Filter<()>),\n    #[token(\"y\")] Y,\n}}\nfn cb(_: &mut Lexer<T>) -> {} {{ Filter::Skip }}\n",
                type);
    }};

    EXPECT_EQ(token_of(returning("Filter<()>")), variant_v);
    EXPECT_EQ(token_of(returning("Filter<Filter<()>>")), discarded);
}

TEST(Read_logos_test, A_pub_self_item_is_private)
{
    // `pub(self)` is private, as Rust has it, so `use m::*` at the root brings no `pub(self) fn skip` of `mod m` in and
    // the callback's `skip` is the crate's through `use logos::*`, discarding x as logos 0.15.1 on "xy" prints `Ok(Y)
    // 1..2` alone; `pub(super)` reaches the root, whose glob brings the function in, a bool that emits X, as the crate
    // prints `Ok(X) 0..1` then `Ok(Y) 1..2`.
    const auto under{[](const std::string_view imports, const std::string_view visibility) {
        return std::format(
                "{}mod m {{ {} fn skip(_: &mut logos::Lexer<crate::T>) -> bool {{ true }} }}\nuse "
                "m::*;\n#[derive(Logos, Debug, PartialEq)]\npub enum T {{\n    #[token(\"x\", skip)] X,\n    "
                "#[token(\"y\")] Y,\n}}\n",
                imports, visibility);
    }};

    EXPECT_EQ(token_of(under("use logos::*;\n", "pub(self)")), discarded);
    EXPECT_EQ(token_of(under("use logos::*;\n", "pub(in self)")), discarded);
    EXPECT_EQ(token_of(under("use logos::{Lexer, Logos};\n", "pub(super)")), variant_x);
}

TEST(Read_logos_test, An_associated_constant_binds_nothing_in_the_module)
{
    // A constant of an impl block is the type's, as Rust has it: `impl T { const Skip: u8 = 0; }` leaves the callback's
    // `Skip` the crate's, imported, and x is discarded, as logos 0.15.1 on "xy" prints `Ok(Y) 1..2` alone. A constant
    // of the module is an item of it, `const Skip: T = T::Y;` at the root, whose value a callback returning the enum
    // names out of sight, so the callback is refused, where the crate prints `Ok(Y) 0..1` then `Ok(Y) 1..2`.
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos, Skip};\n#[derive(Logos, Debug, PartialEq)]\npub enum T {\n"
                     "    #[token(\"x\", cb)] X,\n    #[token(\"y\")] Y,\n}\nimpl T { const Skip: u8 = 0; }\n"
                     "fn cb(_: &mut Lexer<T>) -> Skip { Skip }\n"),
            discarded);
    EXPECT_NE(
            line_of("use logos::{Lexer, Logos};\nconst Skip: T = T::Y;\n"
                    "#[derive(Logos, Debug, PartialEq)]\npub enum T {\n    #[token(\"x\", cb)] X,\n"
                    "    #[token(\"y\")] Y,\n}\nfn cb(_: &mut Lexer<T>) -> T { Skip }\n"),
            std::nullopt);
}

TEST(Read_logos_test, A_name_is_read_in_the_namespace_the_text_asks)
{
    // Rust keeps types and values apart: `const Skip: T = T::Y;` beside `type Skip = logos::Skip;` is the constant
    // where a value stands, so a callback returning the enum whose result is `Skip` returns the constant, a value out
    // of sight, and is refused in either order of the two, which logos 0.15.1 on "xy" emits as `Ok(Y) 0..1` then `Ok(Y)
    // 1..2`; and a `const Skip: u8` under `use logos::*` leaves the type `Skip` the crate's, so a callback returning
    // `Skip` with the crate's value discards x, as the crate prints `Ok(Y) 1..2` alone.
    const auto enum_of{[](const std::string_view head, const std::string_view function) {
        return std::format(
                "{}#[derive(Logos, Debug, PartialEq)]\npub enum T {{\n    #[token(\"x\", cb)] X,\n    #[token(\"y\")] "
                "Y,\n}}\n{}",
                head, function);
    }};

    EXPECT_NE(
            line_of(
                    enum_of("use logos::{Lexer, Logos};\nconst Skip: T = T::Y;\ntype Skip = logos::Skip;\n",
                            "fn cb(_: &mut Lexer<T>) -> T { Skip }\n")),
            std::nullopt);
    EXPECT_NE(
            line_of(
                    enum_of("use logos::{Lexer, Logos};\ntype Skip = logos::Skip;\nconst Skip: T = T::Y;\n",
                            "fn cb(_: &mut Lexer<T>) -> T { Skip }\n")),
            std::nullopt);
    EXPECT_EQ(
            token_of(enum_of(
                    "use logos::*;\nconst Skip: u8 = 0;\n", "fn cb(_: &mut Lexer<T>) -> Skip { logos::Skip }\n")),
            discarded);
}

TEST(Read_logos_test, A_glob_of_the_enum_brings_its_variants_in)
{
    // The variants of an enum the file defines are items of it, as Rust has it: `use T::*` brings them in and `use
    // T::Skip` names one, so a callback returning the enum whose result is a bare `Skip` under either names the variant
    // `Skip` and emits it, as logos 0.15.1 on "xy" prints `Ok(Skip) 0..1` then `Ok(Y) 1..2`. A `use logos::Skip` beside
    // the glob shadows the glob's variant, as Rust's own imports do, so a callback returning `Skip` returns the crate's
    // and x is discarded, as the crate prints `Ok(Y) 1..2` alone; and a constructor written as `T::Skip` or
    // `crate::T::Skip` names the variant.
    const auto through{[](const std::string_view imports, const std::string_view returns) {
        return std::format(
                "{}#[derive(Logos, Debug, PartialEq)]\npub enum T {{\n    #[token(\"x\", cb)] Skip,\n    "
                "#[token(\"y\")] Y,\n}}\nfn cb(_: &mut Lexer<T>) -> {} {{ Skip }}\n",
                imports, returns);
    }};

    const std::optional<std::string> skip{"Skip"};

    EXPECT_EQ(token_of(through("use logos::{Lexer, Logos};\nuse T::*;\n", "T")), skip);
    EXPECT_EQ(token_of(through("use logos::{Lexer, Logos};\nuse T::Skip;\n", "T")), skip);
    EXPECT_EQ(token_of(through("use logos::{Lexer, Logos, Skip};\nuse T::*;\n", "Skip")), discarded);
    EXPECT_EQ(
            token_of("use logos::{Lexer, Logos};\n#[derive(Logos, Debug, PartialEq)]\npub enum T {\n"
                     "    #[token(\"x\", cb)] Skip,\n    #[token(\"y\")] Y,\n}\n"
                     "fn cb(_: &mut Lexer<T>) -> T { crate::T::Skip }\n"),
            skip);
}

TEST(Read_logos_test, What_the_crate_refuses_of_attributes_and_variants_is_refused)
{
    // Each of these is a file logos 0.15.1 refuses, in the words quoted, as building it shows; the reading refuses it
    // at the line of the attribute or variant in question rather than read a scanner the crate never makes. The last
    // group builds and is read.
    const auto enum_of{[](const std::string_view head, const std::string_view body) {
        return std::format("{}enum T {{\n{}\n}}\n", head, body);
    }};

    constexpr std::string_view word{R"rs(    #[regex("[a-w]+")]
    Word,)rs"};

    // "Since 0.13 Logos no longer requires the #[error] variant", "Logos currently only supports variants with one
    // field, found 2" and "Logos doesn't support named fields yet", at the variant.
    for (const std::string_view second : {"#[error]\n    Error,", "Pair(u8, u8),", "Named { a: u8 },"})
    {
        const auto body{std::format("{}\n    {}", word, second)};

        EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", body)), 5) << second;
    }

    // A variant a `cfg` strips is gone before the derive reads the enum, so its fields are fields the crate never sees
    // and refuses nothing: the same shapes stand where the attribute is false by form, and are refused again where it
    // is true.
    const auto under_cfg{[&enum_of, word](const std::string_view cfg, const std::string_view variant) {
        const auto body{std::format("{}\n    {}\n    {}", word, cfg, variant)};

        return enum_of("#[derive(Logos)]\n", body);
    }};

    EXPECT_EQ(line_of(under_cfg("#[cfg(any())]", "Named { a: u8 },")), std::nullopt);
    EXPECT_EQ(line_of(under_cfg("#[cfg(any())]", "Pair(u8, u8),")), std::nullopt);
    EXPECT_EQ(line_of(under_cfg("#[cfg(all())]", "Named { a: u8 },")), 6);

    const auto on_word{[&enum_of](const std::string_view attribute) {
        const auto variant{std::format("    {}\n    Word,", attribute)};

        return enum_of("#[derive(Logos)]\n", variant);
    }};

    // "Resetting previously set priority", "Callback has been already set", "Expected: priority = <integer>",
    // "Expected: callback = ..." and, for anything after `ignore(...)`, whose closing comma the crate leaves unread,
    // "Expected a named argument at this position".
    EXPECT_EQ(line_of(on_word(R"rs(#[regex("[a-w]+", priority = 1, priority = 2)])rs")), 3);
    EXPECT_EQ(line_of(on_word(R"rs(#[regex("[a-w]+", |_| (), callback = |_| ())])rs")), 3);
    EXPECT_EQ(line_of(on_word(R"rs(#[regex("[a-w]+", callback = |_| (), callback = |_| ())])rs")), 3);
    EXPECT_EQ(line_of(on_word(R"rs(#[regex("[a-w]+", priority(3))])rs")), 3);
    EXPECT_EQ(line_of(on_word(R"rs(#[regex("[a-w]+", callback(logos::skip))])rs")), 3);
    EXPECT_EQ(line_of(on_word(R"rs(#[regex("[a-w]+", ignore(case), priority = 3)])rs")), 3);
    EXPECT_EQ(line_of(on_word(R"rs(#[regex("[a-w]+", ignore(case), ignore(case))])rs")), 3);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(skip(\"x\", priority = 1, priority = 2))]\n", word)), 2);

    // Rust writes an underscore between the digits of a numeric escape as it writes one in a number, so `\u{1F_600}` is
    // the scalar `\u{1F600}` and rustc compiles the token it spells.
    const auto underscored{std::format("    #[token(\"\\u{{1F_600}}\")]\n    V,\n{}", word)};

    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", underscored)), std::nullopt);

    const auto escaped{std::format("    #[token(\"\\u{{1F600}}\")]\n    V,\n{}", word)};

    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", escaped)), std::nullopt);

    const auto digitless{std::format("    #[token(\"\\u{{_}}\")]\n    V,\n{}", word)};

    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", digitless)), 3);

    // "Expected #[token(...)]" for the attribute without its parentheses or with nothing in them, and the same for
    // `#[logos]`; "Invalid nested attribute" for a bare key; the shape each key expects for another shape; and "can be
    // defined only once" for `extras`, `error`, `source` and the type of one parameter given twice, across attributes
    // as within one.
    const auto bare_token{std::format("    #[token]\n    V,\n{}", word)};

    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", bare_token)), 3);

    const auto assigned_token{std::format("    #[token = \"x\"]\n    V,\n{}", word)};

    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", assigned_token)), 3);

    const auto empty_regex{std::format("    #[regex()]\n    V,\n{}", word)};

    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", empty_regex)), 3);
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
            line_of("#[derive(Logos)]\n#[logos(type S = u8, type S = u16)]\nenum T<S> {\n"
                    "    #[regex(\"[a-w]+\", |_| 1)]\n    Word(S),\n}\n"),
            2);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(type S = u8)]\n", word)), 1);
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T<S> {\n    #[regex(\"[a-w]+\", |_| 1)]\n    Word(S),\n}\n"), 1);
    EXPECT_EQ(
            line_of("#[derive(Logos)]\nenum T<'a, 'b> {\n    #[regex(\"[a-w]+\", |lex| lex.slice())]\n"
                    "    Word(&'a str),\n    #[regex(\"[0-9]+\", |lex| lex.slice())]\n    Num(&'b str),\n}\n"),
            1);
    EXPECT_EQ(line_of("#[derive(Logos)]\nenum T<const N: usize> {\n    #[regex(\"[a-w]+\")]\n    Word,\n}\n"), 1);

    // What the crate takes is read: `#[logos()]`, `crate` twice, a trailing comma, a field attribute, a discriminant,
    // `priority` before `ignore(...)`, and one lifetime with a type parameter assigned.
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos()]\n", word)), std::nullopt);
    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n#[logos(crate = logos, crate = logos)]\n", word)), std::nullopt);

    const auto trailing_comma{std::format("    #[token(\"x\",)]\n    V,\n{}", word)};

    EXPECT_EQ(line_of(enum_of("#[derive(Logos)]\n", trailing_comma)), std::nullopt);
    EXPECT_EQ(
            line_of(enum_of(
                    "#[derive(Logos)]\n", "    #[regex(\"[a-w]+\", |_| 1)]\n    Word(#[allow(dead_code)] u8,),")),
            std::nullopt);
    EXPECT_EQ(
            line_of(enum_of(
                    "#[derive(Logos)]\n#[repr(u8)]\n", "    #[regex(\"[a-w]+\")]\n    Word = 1,\n    Other = 2,")),
            std::nullopt);

    const auto ranked{read_logos(on_word(R"rs(#[regex("[a-w]+", priority = 3, ignore(case))])rs"))};

    EXPECT_EQ(ranked.front().rules.front().priority, std::optional<std::size_t>{3});
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(type S = u8)]\nenum T<'a, S> {\n    #[regex(\"[a-w]+\", |_| \"x\")]\n"
                    "    Word(&'a str),\n    #[regex(\"[0-9]+\", |_| 1)]\n    Num(S),\n}\n"),
            std::nullopt);
}

TEST(Read_logos_test, A_callback_that_moves_the_lexer_is_refused)
{
    // logos 0.15.1 hands the callback the lexer, whose `bump` extends the match, and whose internal `bump_unchecked`,
    // `trivia`, `error`, `end` and `set` move it too once the trait is imported: on `#y` the closure `|lex| {
    // lex.bump(1); }` under `#[token("#")]` gives V the span 0..2 and a skip's callback bumping the same way discards
    // both bytes, as running the crate shows. A body is read only where its lexer parameter is used through `slice`,
    // `span`, `remainder`, `source`, `extras` or `clone`; one naming a moving method, or letting the parameter out of
    // sight, is refused by name at the rule's line, in a closure, in a function the callback names and in a skip's
    // callback alike, as is a function declared without a body.
    const auto file_of{[](const std::string_view attribute, const std::string_view after) {
        return std::format(
                "use logos::{{Lexer, Logos, Skip}};\nuse "
                "logos::internal::LexerInternal;\n#[derive(Logos)]\n#[logos(extras = usize)]\nenum T {{\n    {}\n    "
                "V,\n    #[regex(\"[a-z]+\")]\n    Word,\n}}\n{}",
                attribute, after);
    }};

    const auto refused_at{[&file_of](const std::string_view attribute, const std::string_view after = "") {
        return line_of(file_of(attribute, after));
    }};

    // The line the variant's attribute stands on in a file of file_of(), which a refusal names.
    constexpr std::size_t attribute_at{6};

    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.bump(1); })])rs"), attribute_at);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.bump_unchecked(1); })])rs"), attribute_at);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.trivia(); })])rs"), attribute_at);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.end(); })])rs"), attribute_at);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.set(Ok(T::V)); })])rs"), attribute_at);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { lex.clone().bump(1); })])rs"), attribute_at);
    EXPECT_EQ(
            refused_at(R"rs(#[token("#", |lex| { advance(lex); })])rs", "fn advance(lex: &mut Lexer<T>) {}\n"),
            attribute_at);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { let l = lex; Skip })])rs"), attribute_at);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { dbg!(lex); })])rs"), attribute_at);
    EXPECT_EQ(
            refused_at(R"rs(#[token("#", eat)])rs", "fn eat(lex: &mut Lexer<T>) -> Skip { lex.bump(1); Skip }\n"),
            attribute_at);
    EXPECT_EQ(
            refused_at(
                    R"rs(#[token("#", eat)])rs",
                    "fn eat(mut lex: &mut Lexer<T>) -> Skip { Lexer::bump(&mut lex, 1); Skip }\n"),
            attribute_at);
    EXPECT_EQ(
            refused_at(R"rs(#[token("#", eat)])rs", "trait X { fn eat(lex: &mut Lexer<T>) -> Skip; }\n"), attribute_at);
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(skip(\"#\", |lex| lex.bump(1)))]\nenum T {\n    #[regex(\"[a-z]+\")]\n"
                    "    Word,\n}\n"),
            2);
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(skip(\"#\", eat))]\nenum T {\n    #[regex(\"[a-z]+\")]\n    Word,\n}\n"
                    "fn eat(lex: &mut logos::Lexer<T>) { lex.bump(1); }\n"),
            2);

    // The parameter binds the lexer under whatever pattern Rust binds a name with: on "#y" the crate runs `fn eat(ref
    // mut lex: &mut Lexer<T>) -> Skip { let _ = lex.next(); Skip }` and emits nothing, the call consuming y, so the
    // function is refused for its use of the lexer as it is written `mut lex` or `lex`; `ref lex` reading the lexer and
    // `_` binding nothing skip # and emit Word, so they are read; and a pattern of another shape, `lex @ _`, which the
    // crate runs as `lex`, or `Lexer { extras, .. }`, is refused since its bindings are not followed.
    EXPECT_EQ(
            refused_at(
                    R"rs(#[token("#", eat)])rs",
                    "fn eat(ref mut lex: &mut Lexer<T>) -> Skip { let _ = lex.next(); Skip }\n"),
            attribute_at);
    EXPECT_EQ(
            refused_at(
                    R"rs(#[token("#", eat)])rs",
                    "fn eat(ref lex: &mut Lexer<T>) -> Skip { let _ = lex.slice(); Skip }\n"),
            std::nullopt);
    EXPECT_EQ(refused_at(R"rs(#[token("#", eat)])rs", "fn eat(_: &mut Lexer<T>) -> Skip { Skip }\n"), std::nullopt);
    EXPECT_EQ(
            refused_at(
                    R"rs(#[token("#", eat)])rs",
                    "fn eat(lex @ _: &mut Lexer<T>) -> Skip { let _ = lex.next(); Skip }\n"),
            attribute_at);
    EXPECT_EQ(
            refused_at(
                    R"rs(#[token("#", eat)])rs",
                    "fn eat(Lexer { extras, .. }: &mut Lexer<T>) -> Skip { *extras += 1; Skip }\n"),
            attribute_at);

    const auto skipping{read_logos(file_of(
            R"rs(#[token("#", eat)])rs", "fn eat(ref lex: &mut Lexer<T>) -> Skip { let _ = lex.slice(); Skip }\n"))};

    EXPECT_EQ(skipping.front().rules.front().token, discarded);

    // Reading the lexer leaves the match the pattern's, so each of these is read; the crate emits V and W on them.
    const auto read{read_logos(file_of(
            R"rs(#[token("#", |lex| { let _ = (lex.slice(), lex.span(), lex.remainder()); lex.extras += 1; })])rs",
            "fn note(lex: &mut Lexer<T>) -> bool { lex.extras += lex.slice().len() + lex.span().len(); true }\n"))};

    EXPECT_EQ(read.front().rules.front().token, variant_v);
    EXPECT_EQ(
            refused_at(R"rs(#[token("#", note)])rs", "fn note(lex: &mut Lexer<T>) -> bool { lex.extras > 0 }\n"),
            std::nullopt);
    EXPECT_EQ(refused_at(R"rs(#[token("#", |lex| { let _ = format!("{}", lex.slice()); Skip })])rs"), std::nullopt);

    // The refusal names the method.
    const auto bumping{file_of(R"rs(#[token("#", |lex| { lex.bump(1); })])rs", "")};

    const auto [bumping_line, bumping_message]{refusal_at(bumping)};

    EXPECT_TRUE(bumping_line.has_value());
    EXPECT_TRUE(bumping_message.contains("`bump`")) << bumping_message;
}

TEST(Read_logos_test, What_the_crate_refuses_around_a_string_pattern_is_refused)
{
    // logos parses a string pattern with the regex crate's UTF-8 check on, so under `(?-u)` a byte beyond ASCII, alone,
    // written in a class whatever the class comes to, admitted by a class, nested ones checked on their own, by the dot
    // or by a negated class, is "pattern can match invalid UTF-8" to the crate; ASCII classes, the ASCII escapes, a
    // folded ASCII letter and a scalar, which stays its UTF-8, pass, and a byte string's pattern has no such check. A
    // byte subpattern referenced under `(?-u)` from a string pattern is refused where it is referenced. logos 0.15.1
    // answers so to each.
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)\xc3")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u).")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?s-u).*")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[^a]")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[^\x00-\x7f]")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)\D")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[^[^\x00-\x7f]]")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[^[^a]]")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[[^\x80-\xff]]")])rs"), attribute_line);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[a[b-c]]+")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)[a-z]+")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?-u)\d+\s")])rs"), std::nullopt);
    EXPECT_EQ(line_of_attribute(R"rs(#[regex(r"(?i-u)k")])rs"), std::nullopt);
    EXPECT_EQ(rule_of(R"rs(#[regex(r"(?-u)é")])rs").expression, R"("\xc3\xa9")");
    EXPECT_EQ(rule_of(R"rs(#[regex(b"\xFF[^\n]")])rs").expression, R"("\xff"[\x00-\t\x0b-\xff])");
    EXPECT_EQ(
            line_of("#[derive(Logos)]\n#[logos(subpattern hi = b\"\\xc3\")]\nenum T {\n    #[regex(\"(?-u)(?&hi)\")]\n"
                    "    V,\n}\n"),
            4);

    // logos 0.15.1 knows eight `#[logos(...)]` keys and calls any other an unknown nested attribute, a later crate's
    // `utf8` among them; `type S` assigns the enum's own type parameter.
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(utf8 = false)]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(frobnicate = 1)]\nenum T {\n    V,\n}\n"), 2);

    const auto every_key{
            read_logos("#[derive(Logos)]\n"
                       "#[logos(crate = logos, extras = (), source = str, error = E, export_dir = \"out\", "
                       "type S = &str)]\nenum T<S> {\n    V(S),\n}\n")};

    EXPECT_EQ(every_key.front().options.size(), 7U);

    // The crate leaves the comma after a parenthesised entry unread, so an entry after `skip(...)` in one attribute is
    // an invalid nested attribute to it, while one before it is fine.
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip(\"x\"), extras = usize)]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip(\"x\"), skip(\"y\"))]\nenum T {\n    V,\n}\n"), 2);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(extras = usize, skip(\"x\"))]\nenum T {\n    V,\n}\n"), std::nullopt);
    EXPECT_EQ(line_of("#[derive(Logos)]\n#[logos(skip \"x\", skip \"y\")]\nenum T {\n    V,\n}\n"), std::nullopt);
}
