#include "munch/tools/audit/read_antlr.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/cursor.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The last scalar there is.
 */
constexpr char32_t last_scalar{0x10FFFF};

/**
 * @brief The ASCII bytes, one bit each.
 */
using Ascii_t = std::bitset<128>;

/**
 * @brief One inclusive range of scalars.
 */
struct Scalar_range
{
    /**
     * @brief The first scalar.
     */
    char32_t first;

    /**
     * @brief The last scalar.
     */
    char32_t last;
};

/**
 * @brief What one atom of a rule admits in one step, when that is a set of characters: the ASCII part, and the
 *        scalars beyond it. What a non-greedy loop needs to know about the atom it repeats.
 */
struct Alphabet
{
    /**
     * @brief The ASCII bytes admitted.
     */
    Ascii_t ascii;

    /**
     * @brief The scalars from U+0080 up admitted, as ranges in any order.
     */
    std::vector<Scalar_range> beyond;
};

/**
 * @brief One element of an alternative: its rewritten atom, what the atom admits when it is a set, the literal it
 *        spells when it is one, and its suffix.
 */
struct Element
{
    /**
     * @brief The atom in the parser's syntax, the suffix not yet applied.
     */
    std::string expression;

    /**
     * @brief The literal's bytes, when the atom is an exact literal.
     */
    std::optional<std::string> literal;

    /**
     * @brief What the atom admits, when it is a set, `.`, or a one-character literal.
     */
    std::optional<Alphabet> alphabet;

    /**
     * @brief The ASCII bytes a match of the atom can begin with, when they are known: a literal's first byte, a
     *        set's members, a group's alternatives' firsts; unknown for a reference.
     */
    std::optional<Ascii_t> first;

    /**
     * @brief The suffix, `?`, `*`, `+`, or nothing.
     */
    char suffix;

    /**
     * @brief Whether the suffix was non-greedy, `??`, `*?` or `+?`.
     */
    bool lazy;
};

/**
 * @brief One sequence of elements as read: its expression, and the ASCII bytes a match can begin with, when known.
 */
struct Sequence
{
    /**
     * @brief The expression.
     */
    std::string expression;

    /**
     * @brief The ASCII bytes a match can begin with, when every element that can begin one says which.
     */
    std::optional<Ascii_t> first;

    /**
     * @brief Whether the sequence has no element, an empty alternative.
     */
    bool empty;
};

/**
 * @brief One outermost alternative of a rule with its commands.
 */
struct Alternative
{
    /**
     * @brief The alternative's text as written, commands excluded.
     */
    std::string pattern;

    /**
     * @brief The alternative in the parser's syntax.
     */
    std::string expression;

    /**
     * @brief The commands after `->`, as written, empty when there are none.
     */
    std::string commands;

    /**
     * @brief Whether the alternative has no element, which makes the rule match the empty string too.
     */
    bool empty;
};

/**
 * @brief A cursor over a grammar's text, reading it item by item into a specification.
 *
 * The grammar is ANTLR 4's: a declaration, then options, named blocks, `mode` lines and rules. A lexer rule's body is
 * read element by element and rewritten for the pattern parser as it goes; a parser rule is skipped, its literals
 * kept for the implicit tokens a combined grammar makes of them.
 */
class Grammar : public Cursor
{
public:
    /**
     * @brief Binds the cursor to a grammar's text.
     * @param source The text.
     */
    explicit Grammar(std::string_view source);

    /**
     * @brief Reads the whole grammar.
     * @return The scanner.
     * @throws Spec_error If the grammar is malformed or uses a refused construct.
     */
    [[nodiscard]] Lexer_spec read();

private:
    /**
     * @brief Skips a brace block from its `{`, however nested.
     * @throws Spec_error If the block never closes.
     */
    void skip_block();

    /**
     * @brief Reads the `options { name = value; ... }` block after its keyword, recording the options.
     * @param options Where the options go.
     * @return Whether `caseInsensitive` was set among them.
     */
    [[nodiscard]] std::optional<bool> options_block(std::vector<std::string>& options);

    /**
     * @brief Skips a parser rule from its name through its `;` and the `catch` and `finally` blocks after it,
     *        collecting the literals it uses.
     */
    void parser_rule();

    /**
     * @brief Reads a lexer rule from its name, or from `fragment`, through its `;`.
     * @param spec The specification being filled.
     * @param mode The mode the rule is in, empty for the default mode.
     * @param case_insensitive Whether the grammar's option is set.
     */
    void lexer_rule(Lexer_spec& spec, const std::string& mode, bool case_insensitive);

    /**
     * @brief Reads the outermost alternatives of a rule, through the `;`.
     * @param case_insensitive Whether letters double their case.
     * @return The alternatives.
     */
    [[nodiscard]] std::vector<Alternative> alternatives(bool case_insensitive);

    /**
     * @brief Reads one sequence of elements, up to `|`, `)`, `->` or `;`, and composes their expression.
     * @param case_insensitive Whether letters double their case.
     * @return The sequence.
     */
    [[nodiscard]] Sequence sequence(bool case_insensitive);

    /**
     * @brief Reads one element: an atom and its suffix.
     * @param case_insensitive Whether letters double their case.
     * @return The element.
     */
    [[nodiscard]] Element element(bool case_insensitive);

    /**
     * @brief Reads a quoted literal after its opening quote, through the closing one, decoding its escapes.
     * @return The bytes.
     */
    [[nodiscard]] std::string literal();

    /**
     * @brief Reads a set after its `[`, through the `]`.
     * @param case_insensitive Whether letters double their case.
     * @return What it admits.
     */
    [[nodiscard]] Alphabet set(bool case_insensitive);

    /**
     * @brief Reads what a `~` negates: a set, a one-character literal, or a group of those separated by `|`.
     * @param case_insensitive Whether letters double their case.
     * @return What is negated.
     */
    [[nodiscard]] Alphabet negatable(bool case_insensitive);

    /**
     * @brief Reads one character of a literal or a set, an escape decoded.
     * @param closing The byte that closes the literal or set, which a bare one of ends the reading.
     * @return The scalar, or std::nullopt at the closing byte.
     */
    [[nodiscard]] std::optional<char32_t> character(char closing);

    /**
     * @brief Reads an identifier at the cursor.
     * @return The identifier, empty when none begins here.
     */
    [[nodiscard]] std::string identifier();

    /**
     * @brief The literals the parser rules use, in order of first appearance, quotes included.
     */
    std::vector<std::pair<std::string, std::size_t>> parser_literals_;
};

/**
 * @brief The one scalar a literal's UTF-8 bytes encode, when they encode exactly one.
 * @param bytes The bytes.
 * @return The scalar, or std::nullopt for none or several.
 */
[[nodiscard]] std::optional<char32_t> decoded(const std::string_view bytes)
{
    if (bytes.empty())
    {
        return std::nullopt;
    }

    const auto lead{static_cast<unsigned char>(bytes.front())};

    const auto length{lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2};

    if (bytes.size() != static_cast<std::size_t>(length))
    {
        return std::nullopt;
    }

    auto scalar{static_cast<char32_t>(length == 1 ? lead : lead & (0xFF >> (length + 1)))};

    for (const auto byte : bytes.substr(1))
    {
        scalar = (scalar << 6U) | (static_cast<unsigned char>(byte) & 0x3FU);
    }

    return scalar;
}

/**
 * @brief A set of ASCII bytes as a bracket, runs of three or more as ranges.
 * @param ascii The bytes.
 * @return The bracket text.
 */
[[nodiscard]] std::string bracket(const Ascii_t& ascii)
{
    std::string out{'['};

    for (std::size_t byte{0}; byte < ascii.size();)
    {
        if (!ascii.test(byte))
        {
            ++byte;

            continue;
        }

        auto end{byte};

        while (end + 1 < ascii.size() && ascii.test(end + 1))
        {
            ++end;
        }

        out += bracket_member(static_cast<unsigned char>(byte));

        if (end > byte + 1)
        {
            out += '-';
        }

        if (end > byte)
        {
            out += bracket_member(static_cast<unsigned char>(end));
        }

        byte = end + 1;
    }

    return out + ']';
}

/**
 * @brief An alphabet as one step of the parser's syntax: a bracket over bytes when it stays within ASCII, else a
 *        bracket over scalars, every member a code point escape, which is how the parser reads one.
 * @param alphabet The alphabet.
 * @return The bracket.
 */
[[nodiscard]] std::string step(const Alphabet& alphabet)
{
    if (alphabet.beyond.empty())
    {
        return bracket(alphabet.ascii);
    }

    const auto escaped{[](const char32_t first, const char32_t last) {
        return first == last ? std::format(R"(\u{{{:x}}})", static_cast<std::uint32_t>(first)) :
                               std::format(
                                       R"(\u{{{:x}}}-\u{{{:x}}})", static_cast<std::uint32_t>(first),
                                       static_cast<std::uint32_t>(last));
    }};

    std::string out{'['};

    for (std::size_t byte{0}; byte < alphabet.ascii.size();)
    {
        if (!alphabet.ascii.test(byte))
        {
            ++byte;

            continue;
        }

        auto end{byte};

        while (end + 1 < alphabet.ascii.size() && alphabet.ascii.test(end + 1))
        {
            ++end;
        }

        out += escaped(static_cast<char32_t>(byte), static_cast<char32_t>(end));

        byte = end + 1;
    }

    for (const auto& [first, last] : alphabet.beyond)
    {
        out += escaped(first, last);
    }

    return out + ']';
}

/**
 * @brief Every scalar an alphabet does not admit.
 * @param alphabet The alphabet.
 * @return Its complement over the scalars.
 */
[[nodiscard]] Alphabet complement(const Alphabet& alphabet)
{
    auto beyond{alphabet.beyond};

    std::ranges::sort(beyond, {}, &Scalar_range::first);

    std::vector<Scalar_range> rest;

    char32_t from{0x80};

    for (const auto& [first, last] : beyond)
    {
        if (first > from)
        {
            rest.push_back({.first = from, .last = first - 1});
        }

        from = std::max(from, static_cast<char32_t>(last + 1));
    }

    if (from <= last_scalar)
    {
        rest.push_back({.first = from, .last = last_scalar});
    }

    return {.ascii = ~alphabet.ascii, .beyond = std::move(rest)};
}

/**
 * @brief The bytes of a literal with every ASCII letter in both cases, one bracket per byte.
 * @param bytes The bytes.
 * @return The expression.
 */
[[nodiscard]] std::string caseless(const std::string_view bytes)
{
    std::string out;

    for (const auto byte : bytes)
    {
        if (is_letter(static_cast<unsigned char>(byte)))
        {
            out += std::format("[{}{}]", static_cast<char>(byte | 0x20), static_cast<char>(byte & ~0x20));
        }
        else
        {
            out += '[' + bracket_member(static_cast<unsigned char>(byte)) + ']';
        }
    }

    return out;
}

/**
 * @brief Adds the other case of every letter in a set.
 * @param ascii The set, widened on return.
 */
void double_case(Ascii_t& ascii)
{
    for (std::size_t byte{'A'}; byte <= 'Z'; ++byte)
    {
        if (ascii.test(byte) || ascii.test(byte | 0x20U))
        {
            ascii.set(byte);

            ascii.set(byte | 0x20U);
        }
    }
}

/**
 * @brief The characters of a range, `'a'..'z'`, as an alphabet.
 * @param low The first character.
 * @param high The last, no lower than the first.
 * @param case_insensitive Whether the letters among them double their case.
 * @return The alphabet.
 */
[[nodiscard]] Alphabet spanning(const char32_t low, const char32_t high, const bool case_insensitive)
{
    Alphabet alphabet;

    for (auto value{low}; value <= std::min<char32_t>(high, 0x7F); ++value)
    {
        alphabet.ascii.set(value);
    }

    if (high >= 0x80)
    {
        alphabet.beyond.push_back({.first = std::max<char32_t>(low, 0x80), .last = high});
    }

    if (case_insensitive)
    {
        double_case(alphabet.ascii);
    }

    return alphabet;
}

/**
 * @brief Whether an expression is one unit a suffix applies to as it stands: one bracket, one group, one reference
 *        or a quoted literal of one byte, so that it needs no grouping of its own.
 * @param expression The expression.
 * @return True when it is.
 */
[[nodiscard]] bool atomic(const std::string_view expression)
{
    if (expression.size() < 2)
    {
        return true;
    }

    const auto opener{expression.front()};

    if (opener == '"')
    {
        return expression.size() == 3 || (expression.size() == 4 && expression[1] == '\\') ||
               (expression.size() == 6 && expression.substr(1, 2) == R"(\x)");
    }

    if (opener != '[' && opener != '(' && opener != '{')
    {
        return false;
    }

    // The unit closes at the end if no closer of its kind comes earlier at depth one; escapes are stepped over.
    const auto closer{opener == '[' ? ']' : opener == '(' ? ')' : '}'};

    std::size_t depth{0};

    for (std::size_t at{0}; at < expression.size(); ++at)
    {
        if (expression[at] == '\\')
        {
            ++at;
        }
        else if (expression[at] == opener && (opener != '[' || depth == 0))
        {
            ++depth;
        }
        else if (expression[at] == closer && --depth == 0)
        {
            return at + 1 == expression.size();
        }
    }

    return false;
}

/**
 * @brief The regex over an alphabet of every string with no occurrence of a terminator inside, which is what a
 *        non-greedy loop before that terminator matches: it stops at the first occurrence.
 *
 * Built as the automaton that tracks the longest prefix of the terminator ending at the byte just read, the one
 * that reaching the whole terminator would leave, and then written out by eliminating its states one by one. The
 * terminator is ASCII, so a scalar beyond ASCII never extends a prefix and takes every state back to the start.
 * @param terminator The terminator's bytes, at least one and every one ASCII.
 * @param alphabet What the loop admits.
 * @return The expression, grouped.
 */
[[nodiscard]] std::string avoiding(const std::string_view terminator, const Alphabet& alphabet)
{
    const auto states{terminator.size()};

    const auto final{states};

    // Labels between states as regex text: absent for no edge, empty for the empty string.
    std::vector<std::vector<std::optional<std::string>>> label(
            states + 1, std::vector<std::optional<std::string>>(states + 1));

    const auto join{[](std::optional<std::string>& into, const std::string& more) {
        if (!into)
        {
            into = more;
        }
        else if (into->empty() || more.empty())
        {
            into = std::format("({})?", into->empty() ? more : *into);
        }
        else
        {
            into = std::format("({}|{})", *into, more);
        }
    }};

    // The prefix automaton: from a prefix of length i on byte b, the longest prefix of the terminator that is a
    // suffix of the prefix followed by b.
    for (std::size_t from{0}; from < states; ++from)
    {
        std::vector<Ascii_t> onto(states + 1);

        for (std::size_t byte{0}; byte < 128; ++byte)
        {
            if (!alphabet.ascii.test(byte))
            {
                continue;
            }

            std::string tail{terminator.substr(0, from)};

            tail.push_back(static_cast<char>(byte));

            auto to{std::min(tail.size(), states)};

            while (to > 0 && terminator.substr(0, to) != std::string_view{tail}.substr(tail.size() - to))
            {
                --to;
            }

            onto[to].set(byte);
        }

        for (std::size_t to{0}; to < states; ++to)
        {
            if (onto[to].any())
            {
                join(label[from][to], bracket(onto[to]));
            }
        }

        if (!alphabet.beyond.empty())
        {
            join(label[from][0], step(Alphabet{.ascii = {}, .beyond = alphabet.beyond}));
        }

        join(label[from][final], "");
    }

    // Elimination of every state but the start and the final one.
    for (std::size_t gone{1}; gone < states; ++gone)
    {
        const auto loop{label[gone][gone] ? std::format("({})*", *label[gone][gone]) : std::string{}};

        for (std::size_t from{0}; from <= final; ++from)
        {
            if (from == gone || !label[from][gone])
            {
                continue;
            }

            for (std::size_t to{0}; to <= final; ++to)
            {
                if (to == gone || !label[gone][to])
                {
                    continue;
                }

                join(label[from][to], *label[from][gone] + loop + *label[gone][to]);
            }
        }

        for (std::size_t other{0}; other <= final; ++other)
        {
            label[gone][other] = std::nullopt;

            label[other][gone] = std::nullopt;
        }
    }

    const auto loop{label[0][0] ? std::format("({})*", *label[0][0]) : std::string{}};

    return std::format("({}{})", loop, label[0][final].value_or(""));
}

Grammar::Grammar(const std::string_view source) : Cursor{source}
{}

Lexer_spec Grammar::read()
{
    Lexer_spec spec;

    skip_blanks();

    const auto declared{at_};

    auto keyword{identifier()};

    if (keyword == "lexer" || keyword == "parser")
    {
        skip_blanks();

        keyword = identifier() + " " + keyword;
    }

    if (!keyword.starts_with("grammar"))
    {
        fail("expected a grammar declaration");
    }

    if (keyword.ends_with("parser"))
    {
        fail("a parser grammar has no lexer rules");
    }

    skip_blanks();

    if (identifier().empty())
    {
        fail("the grammar has no name");
    }

    skip_blanks();

    expect(';', "';' to end the grammar declaration");

    spec.line = line_of(declared);

    auto case_insensitive{false};

    std::string mode;

    for (skip_blanks(); peek(); skip_blanks())
    {
        const auto opened{at_};

        if (peek() == '@')
        {
            // A named action, `@header { }` or `@lexer::members { }`: code, skipped.
            while (peek() && *peek() != '{')
            {
                ++at_;
            }

            skip_block();

            continue;
        }

        const auto word{identifier()};

        if (word.empty())
        {
            fail(std::format("unexpected '{}' at the top level", *peek()));
        }

        if (word == "options")
        {
            if (const auto set{options_block(spec.options)})
            {
                case_insensitive = *set;
            }

            continue;
        }

        if (word == "tokens" || word == "channels")
        {
            skip_blanks();

            skip_block();

            continue;
        }

        if (word == "import")
        {
            fail("the grammar imports another, whose rules are not here to read");
        }

        if (word == "mode")
        {
            skip_blanks();

            mode = identifier();

            if (mode.empty())
            {
                fail("a mode needs a name");
            }

            skip_blanks();

            expect(';', "';' to end the mode line");

            spec.conditions.push_back({.name = mode, .exclusive = true});

            continue;
        }

        at_ = opened;

        if (word == "fragment" || (word.front() >= 'A' && word.front() <= 'Z'))
        {
            lexer_rule(spec, mode, case_insensitive);
        }
        else
        {
            parser_rule();
        }
    }

    // The literals the parser rules use are implicit tokens ahead of every explicit rule, unless a rule spells
    // exactly that literal already.
    std::vector<Lexer_spec::Rule> implicit;

    for (const auto& [text, line] : parser_literals_)
    {
        const auto spelled{std::ranges::any_of(spec.rules, [&text](const Lexer_spec::Rule& rule) {
            return rule.pattern == text && rule.conditions.empty();
        })};

        const auto placed{
                std::ranges::any_of(implicit, [&text](const Lexer_spec::Rule& rule) { return rule.pattern == text; })};

        if (spelled || placed)
        {
            continue;
        }

        Grammar reader{text};

        ++reader.at_;

        const auto bytes{reader.literal()};

        implicit.push_back(
                {.pattern = text,
                 .expression = case_insensitive ? caseless(bytes) : quoted(bytes),
                 .conditions = {},
                 .action = {},
                 .token = text,
                 .priority = std::nullopt,
                 .line = line});
    }

    spec.rules.insert(
            spec.rules.begin(), std::make_move_iterator(implicit.begin()), std::make_move_iterator(implicit.end()));

    return spec;
}

void Grammar::skip_block()
{
    const auto opened{at_};

    std::size_t depth{0};

    do
    {
        // A comment's quotes are prose, the apostrophe of "the parser's" among them, so comments are skipped whole.
        skip_blanks();

        if (!peek())
        {
            at_ = opened;

            fail("a brace block never closes");
        }

        const auto byte{next("'}'")};

        if (byte == '\'' || byte == '"')
        {
            while (peek() && *peek() != byte)
            {
                at_ += *peek() == '\\' ? 2 : 1;
            }

            ++at_;
        }
        else if (byte == '{')
        {
            ++depth;
        }
        else if (byte == '}')
        {
            --depth;
        }
    } while (depth > 0);
}

std::optional<bool> Grammar::options_block(std::vector<std::string>& options)
{
    skip_blanks();

    expect('{', "'{' to open the options block");

    std::optional<bool> case_insensitive;

    for (skip_blanks(); peek() != '}'; skip_blanks())
    {
        const auto name{identifier()};

        if (name.empty())
        {
            fail("an option needs a name");
        }

        skip_blanks();

        expect('=', "'=' after the option's name");

        skip_blanks();

        std::string value;

        while (peek() && *peek() != ';' && *peek() != '}')
        {
            value.push_back(next("the option's value"));
        }

        skip_blanks();

        expect(';', "';' to end the option");

        options.push_back(
                name + '=' +
                std::string{std::string_view{value} | std::views::take(value.find_last_not_of(" \t\r\n") + 1)});

        if (name == "caseInsensitive")
        {
            case_insensitive = options.back().ends_with("true");
        }
    }

    ++at_;

    return case_insensitive;
}

void Grammar::parser_rule()
{
    // Through the `;` that ends the rule, blocks and literals stepped over, the literals kept.
    for (;;)
    {
        skip_blanks();

        if (!peek())
        {
            fail("a rule never ends");
        }

        const auto byte{*peek()};

        if (byte == ';')
        {
            ++at_;

            break;
        }

        if (byte == '{' || byte == '[')
        {
            if (byte == '[')
            {
                while (peek() && *peek() != ']')
                {
                    ++at_;
                }

                ++at_;
            }
            else
            {
                skip_block();
            }

            continue;
        }

        if (byte == '\'')
        {
            const auto opened{at_};

            ++at_;

            std::ignore = literal();

            const std::string text{text_.substr(opened, at_ - opened)};

            const auto known{
                    std::ranges::any_of(parser_literals_, [&text](const std::pair<std::string, std::size_t>& seen) {
                        const auto& [spelling, line]{seen};

                        return spelling == text;
                    })};

            if (!known)
            {
                parser_literals_.emplace_back(text, line_of(opened));
            }

            continue;
        }

        ++at_;
    }

    // Exception handlers after the rule: the keywords whole, since a rule may be named catchProduction.
    const auto handler{[this] {
        for (const std::string_view keyword : {"catch", "finally"})
        {
            if (at(keyword) && !(at_ + keyword.size() < end_ && is_name_byte(text_[at_ + keyword.size()])))
            {
                return true;
            }
        }

        return false;
    }};

    for (skip_blanks(); handler(); skip_blanks())
    {
        std::ignore = identifier();

        skip_blanks();

        if (peek() == '[')
        {
            while (peek() && *peek() != ']')
            {
                ++at_;
            }

            ++at_;

            skip_blanks();
        }

        skip_block();
    }
}

void Grammar::lexer_rule(Lexer_spec& spec, const std::string& mode, const bool case_insensitive)
{
    const auto opened{at_};

    auto name{identifier()};

    const auto fragment{name == "fragment"};

    if (fragment)
    {
        skip_blanks();

        name = identifier();
    }

    if (name.empty())
    {
        fail("a rule needs a name");
    }

    auto caseless_rule{case_insensitive};

    skip_blanks();

    if (at("options"))
    {
        std::ignore = identifier();

        skip_blanks();

        std::vector<std::string> ignored;

        if (const auto set{options_block(ignored)})
        {
            caseless_rule = *set;
        }
    }

    skip_blanks();

    expect(':', std::format("':' after the rule {}", name));

    const auto alternatives{this->alternatives(caseless_rule)};

    // The whole rule is what a reference to it expands to; an empty alternative makes it optional.
    std::string whole;

    auto filled{0UZ};

    auto optional{false};

    for (const auto& [pattern, expression, commands, empty] : alternatives)
    {
        if (empty)
        {
            optional = true;

            continue;
        }

        whole += (whole.empty() ? "" : "|") + expression;

        ++filled;
    }

    if (filled == 0)
    {
        at_ = opened;

        fail(std::format("the rule {} matches nothing but the empty string", name));
    }

    if (filled > 1 || optional)
    {
        whole = "(" + whole + ")" + (optional ? "?" : "");
    }

    spec.definitions.insert_or_assign(name, whole);

    if (fragment)
    {
        return;
    }

    const auto shared{std::ranges::all_of(alternatives, [&alternatives](const Alternative& alternative) {
        return alternative.commands == alternatives.front().commands;
    })};

    const auto line{line_of(opened)};

    const auto token_of{[&name, this](const std::string_view commands) -> std::optional<std::string> {
        std::optional<std::string> token{name};

        for (const auto part : commands | std::views::split(','))
        {
            std::string_view command{part};

            while (!command.empty() && (command.front() == ' ' || command.front() == '\t' || command.front() == '\n'))
            {
                command.remove_prefix(1);
            }

            while (!command.empty() && (command.back() == ' ' || command.back() == '\t' || command.back() == '\n'))
            {
                command.remove_suffix(1);
            }

            if (command == "skip" || command.starts_with("channel"))
            {
                token = std::nullopt;
            }
            else if (command.starts_with("type"))
            {
                const auto open{command.find('(')};

                const auto close{command.find(')')};

                token = std::string{command.substr(open + 1, close - open - 1)};
            }
            else if (command == "more")
            {
                fail("'-> more' joins the match onto the next token's, which the byte reading cannot express");
            }
        }

        return token;
    }};

    if (shared)
    {
        std::string pattern;

        for (const auto& [text, expression, commands, empty] : alternatives)
        {
            pattern += (pattern.empty() ? "" : " | ") + text;
        }

        spec.rules.push_back(
                {.pattern = std::move(pattern),
                 .expression = whole,
                 .conditions = mode.empty() ? std::vector<std::string>{} : std::vector{mode},
                 .action =
                         alternatives.front().commands.empty() ? std::string{} : "-> " + alternatives.front().commands,
                 .token = token_of(alternatives.front().commands),
                 .priority = std::nullopt,
                 .line = line});

        return;
    }

    // Alternatives with different commands are ranked as ANTLR ranks them, first alternative first; an empty one is
    // no token.
    for (const auto& [pattern, expression, commands, empty] : alternatives)
    {
        if (empty)
        {
            continue;
        }

        spec.rules.push_back(
                {.pattern = pattern,
                 .expression = expression,
                 .conditions = mode.empty() ? std::vector<std::string>{} : std::vector{mode},
                 .action = commands.empty() ? std::string{} : "-> " + commands,
                 .token = token_of(commands),
                 .priority = std::nullopt,
                 .line = line});
    }
}

std::vector<Alternative> Grammar::alternatives(const bool case_insensitive)
{
    std::vector<Alternative> read;

    for (;;)
    {
        skip_blanks();

        const auto opened{at_};

        auto [expression, first, empty]{sequence(case_insensitive)};

        std::string pattern{text_.substr(opened, at_ - opened)};

        while (!pattern.empty() &&
               (pattern.back() == ' ' || pattern.back() == '\t' || pattern.back() == '\n' || pattern.back() == '\r'))
        {
            pattern.pop_back();
        }

        std::string commands;

        if (at("->"))
        {
            at_ += 2;

            skip_blanks();

            const auto begin{at_};

            while (peek() && *peek() != ';' && *peek() != '|')
            {
                ++at_;
            }

            commands = text_.substr(begin, at_ - begin);

            while (!commands.empty() && (commands.back() == ' ' || commands.back() == '\t' || commands.back() == '\n' ||
                                         commands.back() == '\r'))
            {
                commands.pop_back();
            }
        }

        read.push_back(
                {.pattern = std::move(pattern),
                 .expression = std::move(expression),
                 .commands = std::move(commands),
                 .empty = empty});

        if (peek() == '|')
        {
            ++at_;

            continue;
        }

        skip_blanks();

        expect(';', "';' to end the rule");

        return read;
    }
}

Sequence Grammar::sequence(const bool case_insensitive)
{
    std::vector<Element> elements;

    for (skip_blanks(); peek() && *peek() != '|' && *peek() != ')' && *peek() != ';' && !at("->"); skip_blanks())
    {
        elements.push_back(element(case_insensitive));
    }

    // What the sequence can begin with: every element up to and including the first mandatory one.
    std::optional<Ascii_t> first{Ascii_t{}};

    for (const auto& element : elements)
    {
        if (element.expression.empty())
        {
            continue;
        }

        if (!element.first)
        {
            first = std::nullopt;

            break;
        }

        *first |= *element.first;

        if (element.suffix != '?' && element.suffix != '*')
        {
            break;
        }
    }

    std::string out;

    for (std::size_t index{0}; index < elements.size(); ++index)
    {
        const auto& [expression, literal, alphabet, begins, suffix, lazy]{elements[index]};

        if (!lazy)
        {
            out += suffix == 0 || atomic(expression) ? expression + (suffix == 0 ? "" : std::string{suffix}) :
                                                       std::format("({}){}", expression, suffix);

            continue;
        }

        // A non-greedy loop stops at the first point where what follows matches, so before a literal it matches
        // whatever holds no occurrence of the literal; before anything else it is not a regular rewrite.
        const auto next{index + 1 < elements.size() ? elements[index + 1].literal : std::nullopt};

        if (!next || next->empty())
        {
            fail("a non-greedy loop is read only before a literal, which is where it stops");
        }

        if (std::ranges::any_of(*next, [](const char byte) { return static_cast<unsigned char>(byte) >= 0x80; }))
        {
            fail("a non-greedy loop before a literal beyond ASCII is not modelled");
        }

        const auto opener{static_cast<unsigned char>(next->front())};

        if (suffix == '?')
        {
            if (!begins || begins->test(opener))
            {
                fail("a non-greedy option before a literal it could begin is not modelled");
            }

            out += atomic(expression) ? expression + "?" : std::format("({})?", expression);

            continue;
        }

        const auto unit{atomic(expression) ? expression : "(" + expression + ")"};

        // Over one set the loop is the strings avoiding the literal; over a group none of whose alternatives can
        // begin with the literal's first byte, the loop stops where a greedy one does, since neither can step over
        // that byte.
        if (alphabet)
        {
            const auto free{avoiding(*next, *alphabet)};

            out += suffix == '+' ? unit + free : free;
        }
        else if (begins && !begins->test(opener))
        {
            out += unit + suffix;
        }
        else
        {
            fail("a non-greedy loop is read only over a set, a dot, one character, or a group that cannot begin with "
                 "the literal it stops at");
        }
    }

    return {.expression = std::move(out), .first = first, .empty = elements.empty()};
}

Element Grammar::element(const bool case_insensitive)
{
    const auto opened{at_};

    Element element{
            .expression = {},
            .literal = std::nullopt,
            .alphabet = std::nullopt,
            .first = std::nullopt,
            .suffix = 0,
            .lazy = false};

    const auto byte{*peek()};

    if (byte == '{')
    {
        // An action, which changes nothing a rule matches; a predicate would, and is refused.
        skip_block();

        skip_blanks();

        if (peek() == '?')
        {
            at_ = opened;

            fail("a semantic predicate conditions the match on code, which a token language cannot say");
        }

        return element;
    }

    if (byte == '\'')
    {
        ++at_;

        auto bytes{literal()};

        skip_blanks();

        if (at(".."))
        {
            // A range of characters, 'a'..'z'.
            at_ += 2;

            skip_blanks();

            expect('\'', "a quote to open the range's end");

            const auto low{decoded(bytes)};

            const auto high{decoded(literal())};

            if (!low || !high || *high < *low)
            {
                at_ = opened;

                fail("a character range takes one character at each end, the end no lower than the start");
            }

            auto alphabet{spanning(*low, *high, case_insensitive)};

            element.expression = step(alphabet);

            element.alphabet = std::move(alphabet);
        }
        else
        {
            if (bytes.empty())
            {
                at_ = opened;

                fail("an empty literal matches nothing");
            }

            const auto lettered{std::ranges::any_of(
                    bytes, [](const char one) { return is_letter(static_cast<unsigned char>(one)); })};

            if (case_insensitive && lettered)
            {
                element.expression = caseless(bytes);
            }
            else
            {
                element.expression = quoted(bytes);

                element.literal = bytes;
            }

            element.first = Ascii_t{};

            if (const auto lead{static_cast<unsigned char>(bytes.front())}; lead < 0x80)
            {
                element.first->set(lead);

                if (case_insensitive)
                {
                    double_case(*element.first);
                }
            }

            if (const auto scalar{decoded(bytes)})
            {
                Alphabet alphabet;

                if (*scalar < 0x80)
                {
                    alphabet.ascii.set(*scalar);

                    if (case_insensitive)
                    {
                        double_case(alphabet.ascii);
                    }
                }
                else
                {
                    alphabet.beyond.push_back({.first = *scalar, .last = *scalar});
                }

                element.alphabet = std::move(alphabet);
            }
        }
    }
    else if (byte == '[')
    {
        ++at_;

        auto alphabet{set(case_insensitive)};

        element.expression = step(alphabet);

        element.alphabet = std::move(alphabet);
    }
    else if (byte == '~')
    {
        ++at_;

        skip_blanks();

        element.alphabet = complement(negatable(case_insensitive));

        element.expression = step(*element.alphabet);
    }
    else if (byte == '.')
    {
        ++at_;

        Alphabet all{.ascii = {}, .beyond = {{.first = 0x80, .last = last_scalar}}};

        all.ascii.set();

        element.expression = step(all);

        element.alphabet = std::move(all);
    }
    else if (byte == '(')
    {
        ++at_;

        std::string inner;

        auto optional{false};

        element.first = Ascii_t{};

        for (;;)
        {
            const auto [expression, first, empty]{sequence(case_insensitive)};

            if (empty)
            {
                optional = true;
            }
            else
            {
                inner += (inner.empty() ? "" : "|") + expression;
            }

            if (element.first && first)
            {
                *element.first |= *first;
            }
            else
            {
                element.first = std::nullopt;
            }

            if (peek() == '|')
            {
                ++at_;

                continue;
            }

            skip_blanks();

            expect(')', "')' to close the group");

            break;
        }

        if (inner.empty())
        {
            at_ = opened;

            fail("a group with only empty alternatives matches nothing but the empty string");
        }

        // An empty alternative makes the group optional.
        element.expression = optional ? "(" + inner + ")?" : "(" + inner + ")";
    }
    else
    {
        const auto name{identifier()};

        if (name.empty())
        {
            fail(std::format("unexpected '{}' in a rule", byte));
        }

        if (name == "EOF")
        {
            at_ = opened;

            fail("EOF conditions the match on the end of input, which a token language cannot say");
        }

        element.expression = "{" + name + "}";
    }

    if (element.alphabet)
    {
        element.first = element.alphabet->ascii;
    }

    skip_blanks();

    if (peek() == '?' || peek() == '*' || peek() == '+')
    {
        element.suffix = next("the suffix");

        if (peek() == '?')
        {
            ++at_;

            element.lazy = true;
        }
    }

    return element;
}

std::string Grammar::literal()
{
    std::string bytes;

    while (const auto scalar{character('\'')})
    {
        bytes += encoded(*scalar);
    }

    return bytes;
}

Alphabet Grammar::set(const bool case_insensitive)
{
    const auto opened{at_ - 1};

    Alphabet alphabet;

    const auto admit{[&alphabet](const char32_t first, const char32_t last) {
        for (auto value{first}; value <= std::min<char32_t>(last, 0x7F); ++value)
        {
            alphabet.ascii.set(value);
        }

        if (last >= 0x80)
        {
            alphabet.beyond.push_back({.first = std::max<char32_t>(first, 0x80), .last = last});
        }
    }};

    // A member waits until the next one shows whether a '-' spans them; an escaped `\-` is a member, not a span.
    std::optional<char32_t> pending;

    for (;;)
    {
        const auto escaped{peek() == '\\'};

        const auto scalar{character(']')};

        if (!scalar)
        {
            break;
        }

        if (pending && *scalar == '-' && !escaped && peek() != ']')
        {
            const auto last{character(']')};

            if (!last || *last < *pending)
            {
                at_ = opened;

                fail("a set range needs an end no lower than its start");
            }

            admit(*pending, *last);

            pending = std::nullopt;

            continue;
        }

        if (pending)
        {
            admit(*pending, *pending);
        }

        pending = *scalar;
    }

    if (pending)
    {
        admit(*pending, *pending);
    }

    if (case_insensitive)
    {
        double_case(alphabet.ascii);
    }

    return alphabet;
}

Alphabet Grammar::negatable(const bool case_insensitive)
{
    const auto opened{at_};

    if (peek() == '[')
    {
        ++at_;

        return set(case_insensitive);
    }

    if (peek() == '\'')
    {
        ++at_;

        const auto scalar{decoded(literal())};

        if (!scalar)
        {
            at_ = opened;

            fail("'~' before a literal takes one character");
        }

        skip_blanks();

        // A range inside the negation, ~('0'..'9' | '^'), as Clojure's grammar writes it.
        if (!at(".."))
        {
            return spanning(*scalar, *scalar, case_insensitive);
        }

        at_ += 2;

        skip_blanks();

        expect('\'', "a quote to open the range's end");

        const auto high{decoded(literal())};

        if (!high || *high < *scalar)
        {
            at_ = opened;

            fail("a character range takes one character at each end, the end no lower than the start");
        }

        return spanning(*scalar, *high, case_insensitive);
    }

    if (peek() != '(')
    {
        fail("'~' takes a set, one character, or a group of those");
    }

    // A group of sets and characters, each alternative one of them.
    ++at_;

    Alphabet joined;

    for (;;)
    {
        skip_blanks();

        const auto part{negatable(case_insensitive)};

        joined.ascii |= part.ascii;

        joined.beyond.insert(joined.beyond.end(), part.beyond.begin(), part.beyond.end());

        skip_blanks();

        if (peek() == '|')
        {
            ++at_;

            continue;
        }

        skip_blanks();

        expect(')', "')' to close the negated group");

        return joined;
    }
}

std::optional<char32_t> Grammar::character(const char closing)
{
    const auto byte{next(std::format("'{}'", closing))};

    if (byte == closing)
    {
        return std::nullopt;
    }

    if (byte != '\\')
    {
        // A scalar typed directly in UTF-8: its lead byte says how many follow.
        const auto value{static_cast<unsigned char>(byte)};

        if (value < 0x80)
        {
            return value;
        }

        const auto length{value >= 0xF0 ? 4 : value >= 0xE0 ? 3 : 2};

        auto scalar{static_cast<char32_t>(value & (0xFF >> (length + 1)))};

        for (auto count{1}; count < length; ++count)
        {
            scalar = (scalar << 6U) | (static_cast<unsigned char>(next("a continuation byte")) & 0x3FU);
        }

        return scalar;
    }

    const auto escaped{next("the escaped character")};

    switch (escaped)
    {
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    case 'u':
        break;
    case 'p':
    case 'P':
        --at_;

        fail("a Unicode property class needs tables the byte reading has not got");
    default:
        return static_cast<unsigned char>(escaped);
    }

    // \uXXXX, or \u{X...} of one to six digits.
    const auto braced{peek() == '{'};

    if (braced)
    {
        ++at_;
    }

    char32_t scalar{0};

    std::size_t digits{0};

    while (digits < (braced ? 6U : 4U) && peek() && std::isxdigit(static_cast<unsigned char>(*peek())) != 0)
    {
        const auto digit{next("a hex digit")};

        scalar = scalar * 16 + static_cast<char32_t>(digit <= '9' ? digit - '0' : (digit | 0x20) - 'a' + 10);

        ++digits;
    }

    if (digits == 0 || (!braced && digits < 4) || (braced && next("'}'") != '}') || scalar > 0x10FFFF)
    {
        fail(R"(a Unicode escape is \uXXXX or \u{X...} up to U+10FFFF)");
    }

    return scalar;
}

std::string Grammar::identifier()
{
    std::string name;

    while (peek() && is_name_byte(*peek()))
    {
        name.push_back(next("a name"));
    }

    return name;
}

} // namespace

std::vector<Lexer_spec> read_antlr(const std::string_view source)
{
    return {Grammar{source}.read()};
}

} // namespace munch::tools::audit
