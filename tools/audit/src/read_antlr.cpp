#include "munch/tools/audit/read_antlr.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/cursor.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The last scalar there is.
 */
constexpr char32_t last_scalar{0x10FFFF};

/**
 * @brief Why a character beyond ASCII is refused where `caseInsensitive` is in force.
 *
 * ANTLR's option rewrites every character to both of its cases, one code point each, `[äéöüß]` matching
 * `äéöüßÄÉÖÜß` (antlr4/doc/options.md), and the library carries the Unicode property tables but no case mappings,
 * so folding such a character is not something this reading can do; folding its ASCII neighbours alone would
 * analyse another language than the lexer's.
 */
constexpr std::string_view unfoldable{
        "a character beyond ASCII under caseInsensitive needs the Unicode case mappings, which the byte reading has "
        "not got"};

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
 * @brief The characters a match of something can begin with, when they are known: the ASCII bytes one by one, and
 *        whether any scalar beyond ASCII is among them, which ones not being kept.
 */
struct Beginning
{
    /**
     * @brief The ASCII bytes a match can begin with.
     */
    Ascii_t ascii;

    /**
     * @brief Whether a match can begin with a scalar beyond ASCII.
     */
    bool beyond;

    /**
     * @brief Adds the characters another can begin with.
     * @param other The other.
     */
    void join(const Beginning& other) noexcept
    {
        ascii |= other.ascii;

        beyond = beyond || other.beyond;
    }

    /**
     * @brief Whether one character may begin both this and another, two scalars beyond ASCII taken to be one.
     * @param other The other.
     * @return True when one may.
     */
    [[nodiscard]] bool overlaps(const Beginning& other) const noexcept
    {
        return (ascii & other.ascii).any() || (beyond && other.beyond);
    }
};

/**
 * @brief Whether something matches the empty string, as a formula over the rules it reaches: it does when every
 *        rule of one term does, so a term of no rule is yes and no term at all is no.
 *
 * A rule's own answer waits for the whole grammar, since a rule may reference one written below it, and the
 * closures ANTLR rejects are the ones whose body can match the empty string.
 */
using Nullable_t = std::vector<std::vector<std::string>>;

/**
 * @brief One command after a rule's `->`: its name and the argument its parens hold.
 */
struct Command
{
    /**
     * @brief The name, `skip`, `more`, `type`, `channel`, `mode`, `pushMode` or `popMode`.
     */
    std::string name;

    /**
     * @brief What the parens hold, a token or a mode name or a number, empty when the command takes none.
     */
    std::string argument;

    /**
     * @brief The offset of the name within the clause, which names the command's own line in a refusal.
     */
    std::size_t offset;
};

/**
 * @brief One byte of a `->` clause ANTLR's parser rejects, its error 50, and the refusal's words for it.
 */
struct Syntax_error
{
    /**
     * @brief The offset of the byte within the clause, the clause's length where the clause ends before something
     *        ANTLR's parser still needs.
     */
    std::size_t offset;

    /**
     * @brief The refusal, which names the cause where ANTLR's own words vary with what its parser had taken before.
     */
    std::string message;
};

/**
 * @brief The commands of a `->` clause, and the first byte of it ANTLR's parser rejects.
 */
struct Commands
{
    /**
     * @brief The commands in order, through the first syntax error where there is one.
     */
    std::vector<Command> commands;

    /**
     * @brief The first syntax error of the clause, nothing where ANTLR's parser takes the whole of it.
     */
    std::optional<Syntax_error> error;
};

/**
 * @brief A quoted literal as read: its bytes, and whether ANTLR takes it as one character where it needs one.
 */
struct Literal
{
    /**
     * @brief The bytes, each character's UTF-8 encoding, a surrogate pair's the scalar the pair encodes.
     */
    std::string bytes;

    /**
     * @brief Whether ANTLR reads the literal as one character where a range's end or a negated literal needs one
     *        (CharSupport.getCharValueFromGrammarCharLiteral): one character of the basic multilingual plane written
     *        out, or one escape of any kind. A character beyond that plane written out and a surrogate pair of two
     *        escapes are two UTF-16 units to it, and its error 144 calls the literal multi-character, as it calls the
     *        empty one.
     */
    bool single;
};

/**
 * @brief One closure of a rule, `*` or `+` in either form: the rule it stands in, its line, and whether its body
 *        matches the empty string, which ANTLR rejects as its error 153.
 */
struct Closure
{
    /**
     * @brief The rule the closure stands in, which the refusal names as ANTLR names it.
     */
    std::string rule;

    /**
     * @brief The line the closure's body opens on.
     */
    std::size_t line;

    /**
     * @brief Whether the body matches the empty string.
     */
    Nullable_t body;
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
     * @brief The atom's text as written, quotes included, when it is a quoted literal, folded or not: what ANTLR
     *        matches a parser rule's literal against in a combined grammar. Empty for any other atom.
     */
    std::string spelling;

    /**
     * @brief What the atom admits, when it is a set, `.`, or a one-character literal.
     */
    std::optional<Alphabet> alphabet;

    /**
     * @brief The characters a match of the atom can begin with, when they are known: a literal's first character, a
     *        set's members, a group's alternatives' firsts; unknown for a reference.
     */
    std::optional<Beginning> first;

    /**
     * @brief Whether the atom matches the empty string, the suffix not yet applied.
     */
    Nullable_t nullable;

    /**
     * @brief Whether every match of the atom has one and the same length in bytes, which a literal's have and a set
     *        reaching past U+007F has not, its scalars being one to four bytes: what the alignment of a non-greedy
     *        loop's iterations rests on.
     */
    bool one_length;

    /**
     * @brief The number of characters every match of the atom has, when they all have one, the suffix not yet
     *        applied: a literal's count of scalars, one for a set, a range or the dot, nothing for an inert action,
     *        a group's where its alternatives agree; unknown for a reference and where they do not.
     */
    std::optional<std::size_t> characters;

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
 * @brief One sequence of elements as read: its expression, and what its matches begin with and measure, when known.
 */
struct Sequence
{
    /**
     * @brief The expression.
     */
    std::string expression;

    /**
     * @brief The characters a match can begin with, when every element that can begin one says which.
     */
    std::optional<Beginning> first;

    /**
     * @brief The number of characters every match has, when every element's matches have one and no suffix varies
     *        it; unknown otherwise.
     */
    std::optional<std::size_t> characters;

    /**
     * @brief Whether an element of the sequence carries a non-greedy suffix.
     */
    bool lazy;

    /**
     * @brief Whether the sequence has no element, an empty alternative.
     */
    bool empty;

    /**
     * @brief Whether the sequence matches the empty string.
     */
    Nullable_t nullable;

    /**
     * @brief The one literal's text as written when the sequence is that literal alone, or that literal and one
     *        inert action after it, the two shapes of an alternative ANTLR's own patterns for a rule spelling a
     *        parser literal match; empty otherwise.
     */
    std::string spelling;

    /**
     * @brief Whether an inert action follows the literal the spelling names.
     */
    bool acted;
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
     * @brief The offset in the grammar the commands' text begins at, from which a refusal of a command names the
     *        command's own line as ANTLR names it; the alternative's own offset when it has none.
     */
    std::size_t clause;

    /**
     * @brief The one literal's text as written when the alternative is that literal alone or with one inert action
     *        after it, as the sequence has it; empty otherwise.
     */
    std::string spelling;

    /**
     * @brief Whether an inert action follows the literal the spelling names.
     */
    bool acted;

    /**
     * @brief Whether the alternative has no element, which makes the rule match the empty string too.
     */
    bool empty;

    /**
     * @brief Whether the alternative matches the empty string, an empty one and `'a'?` alike.
     */
    Nullable_t nullable;
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

    /**
     * @brief Skips blanks, comments and byte order marks: ANTLR's lexer reads a mark, U+FEFF, as a token of its own
     *        that it drops wherever one stands outside a literal, a set or an action (ANTLRLexer.g's UnicodeBOM),
     *        so a grammar may open with one or hold one between any two tokens, and it is a blank here.
     * @throws Spec_error If a block comment never closes.
     */
    void skip_blanks();

private:
    /**
     * @brief Skips a brace block from its `{`, however nested.
     * @throws Spec_error If the block never closes.
     */
    void skip_block();

    /**
     * @brief Refuses a lexer `members` action that defines one of the methods deciding the token stream.
     *
     * ANTLR writes the action's code into the body of the lexer class it generates, so a method defined there
     * stands in place of the runtime's own: `nextToken` decides what the next token is and where it ends, `emit`
     * and `emitEOF` what a match becomes, `skip` and `more` whether it becomes a token at all, the mode methods
     * which rules the next match is tried against, `reset` and `consume` where it begins, `setType` and
     * `setChannel` which token it is, and `recover` what an error consumes. The rules still describe the matches,
     * but the tokens are no longer the matches, which is what a certificate is about.
     *
     * A call is not a definition: the name has to be followed by a parameter list and a body, `throws` and the
     * exceptions it names allowed between the two, so that a `skip();` inside another method is the ordinary way a
     * rule's action asks for a skip and stays read.
     * @param code The action's code, its braces included.
     * @param at Where the code begins, which a refusal reports at.
     * @throws Spec_error If the code defines one of them.
     */
    void overriding(std::string_view code, std::size_t at, bool csharp, const Macros_t& macros);

    /**
     * @brief Skips a parser rule's argument block from its `[`, the arguments, `returns`, `locals`, a rule
     *        reference's arguments or a `catch` clause's, as ANTLR's lexer reads an ARG_ACTION: brackets nest, a
     *        `"..."` or a `'...'` inside is skipped whole, a backslash escaping the byte after it, and no comment is
     *        recognised, so a `]` inside a quoted string is no closer and one inside what looks like a comment is.
     * @throws Spec_error If the block never closes.
     */
    void skip_argument();

    /**
     * @brief Reads the `options { name = value; ... }` block after its keyword, recording the options.
     * @param options Where the options go.
     * @return Whether `caseInsensitive` was set among them.
     */
    [[nodiscard]] std::optional<bool> options_block(std::vector<std::string>& options);

    /**
     * @brief Reads a `tokens { NAME, ... }` or `channels { NAME, ... }` block after its keyword, recording the
     *        names, as ANTLR's parser reads one: names parted by commas and nothing else, a tokens block alone
     *        allowed to hold nothing (ANTLRParser.g's tokensSpec and channelsSpec), so that a comma missing or
     *        trailing, `{ ONE TWO }` and `{ ONE, }`, is its error 50, a syntax error at the byte after the name or
     *        the comma.
     * @param names Where the names go.
     * @param kind What the block declares, `token` or `channel`, which a refusal names.
     * @param may_be_empty Whether the block may hold no name.
     * @throws Spec_error As ANTLR's parser rejects a block of any other shape, at the byte it rejects.
     */
    void names_block(std::set<std::string, std::less<>>& names, std::string_view kind, bool may_be_empty);

    /**
     * @brief Reads element options after their `<`, through the `>`, as ANTLR's parser reads them
     *        (ANTLRParser.g's elementOptions): names, dotted or not, each alone or, undotted, with `=` and a value
     *        that is a name, a number, a quoted string or a brace block, parted by commas, or nothing at all
     *        between the angles. An option is metadata on the element before it, `<fail='z'>` on a predicate and
     *        `<assoc=right>` on a token, and no token of the grammar's lexer, so the string among the values is no
     *        literal a parser rule uses.
     * @throws Spec_error If the options are of another shape, which ANTLR's parser rejects.
     */
    void element_options();

    /**
     * @brief The commands a `->` clause holds, as their names and arguments, blanks and comments dropped, and the
     *        first byte of the clause ANTLR's parser rejects, where there is one.
     *
     * ANTLR reads a clause with the lexer it reads the grammar with, so a comment inside one is no part of any
     * command: a clause of `skip` and a block comment after it is the skip command, and comparing the clause's
     * text as written would make it a command of another name and leave the token in the stream. The blanks and
     * comments inside the parens go the same way, so a type command with them around its token names that token.
     * ANTLR's parser takes the clause as commands parted by commas, a command a name alone or a name and parens
     * holding one name or one number (ANTLRParser.g's lexerCommand and lexerCommandExpr), a name beginning with a
     * letter as its lexer reads one; so a comma no command name follows, a command with no comma before it, parens
     * holding nothing, more than one token or anything but a name or a number, parens never closed, and any other
     * byte where a name, an argument or a comma should stand are its error 50, a syntax error at that byte, before
     * any command is looked at. The first such byte is noted with the refusal's words.
     * @param text The clause as written, its `->` excluded, through the `;` or `|` that ends it, exclusive.
     * @return The commands in order, and the first syntax error.
     */
    [[nodiscard]] static Commands commands_of(std::string_view text);

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
     * @param outermost Whether this sequence is a whole alternative of a rule rather than a group's inside, which
     *        is what a non-greedy loop needs, since ANTLR stops one where the rest of the whole rule matches.
     * @return The sequence.
     */
    [[nodiscard]] Sequence sequence(bool case_insensitive, bool outermost);

    /**
     * @brief Reads one element: an atom and its suffix.
     * @param case_insensitive Whether letters double their case.
     * @return The element.
     */
    [[nodiscard]] Element element(bool case_insensitive);

    /**
     * @brief Reads a quoted literal after its opening quote, through the closing one, decoding its escapes; a high
     *        surrogate escape and a low one after it are the one character the pair encodes, and a surrogate standing
     *        alone is refused.
     * @return The literal.
     */
    [[nodiscard]] Literal literal();

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
     * @brief Reads an identifier at the cursor, as ANTLR's lexer reads a name: a letter, then letters, digits and
     *        underscores (ANTLRLexer.g's NameStartChar and NameChar within ASCII), so that `_x` and `2x` begin
     *        none, which ANTLR's parser rejects as a syntax error at the underscore or the number.
     * @return The identifier, empty when none begins here.
     */
    [[nodiscard]] std::string identifier();

    /**
     * @brief The literals the parser rules use, in order of first appearance, quotes included.
     */
    std::vector<std::pair<std::string, std::size_t>> parser_literals_;

    /**
     * @brief The literals the lexer rules spell in a shape ANTLR maps a parser literal onto, as written, and how
     *        many rules spell each: one makes the parser's literal that rule's token, two make it ANTLR's error 126.
     */
    std::map<std::string, std::size_t, std::less<>> aliases_;

    /**
     * @brief The channels the grammar's `channels` block declares, which a channel command may name.
     */
    std::set<std::string, std::less<>> channels_;

    /**
     * @brief The line of the `channels` block, where a channel's name conflicting with a token's or a mode's is
     *        refused as ANTLR's errors 161 and 162 refuse it.
     */
    std::size_t channels_line_{0};

    /**
     * @brief The names a lexer grammar's `tokens` block declares, which ANTLR gives a token type before any rule,
     *        so that a rule of that name whose commands set its type to zero, ANTLR's value for none, emits its
     *        own token; a combined grammar's block goes to its parser alone and is left out.
     */
    std::set<std::string, std::less<>> tokens_;

    /**
     * @brief The rules whose body was read with a non-greedy loop in it, and the line each opens on.
     *
     * ANTLR stops such a loop where the rest of the surrounding lexical rule matches, and a rule another rule
     * references is inlined into that one, whose rest reaches past it; the reading is exact only while nothing
     * references the rule, which read() checks once the whole grammar is in.
     */
    std::vector<std::pair<std::string, std::size_t>> lazy_rules_;

    /**
     * @brief The names of the rules read that are no fragments, which a `type` command may name, before or after
     *        the rule, since ANTLR resolves the name over the whole grammar.
     */
    std::set<std::string, std::less<>> rule_names_;

    /**
     * @brief The names `type` commands carry that are no numbers, each with its line, resolved once the grammar is
     *        read: ANTLR's error 175 refuses one that names no rule and no `tokens` entry.
     */
    std::vector<std::pair<std::string, std::size_t>> typed_names_;

    /**
     * @brief A `mode` line's section: its name, its line and how many rules that are no fragments it holds, since
     *        ANTLR's error 145 refuses each section holding none, a reopened mode's empty section included.
     */
    struct Section
    {
        std::string name;

        std::size_t line{0};

        std::size_t rules{0};
    };

    /**
     * @brief The `mode` sections in the order declared, DEFAULT_MODE's among them, which reopens the default mode.
     */
    std::vector<Section> sections_;

    /**
     * @brief The names `mode` and `pushMode` commands carry that are no numbers, each with its line, resolved once
     *        the grammar is read: ANTLR's error 176 refuses one that names no mode.
     */
    std::vector<std::pair<std::string, std::size_t>> mode_names_;

    /**
     * @brief The closures read, each with the rule it stands in, so that a body reaching a rule written further
     *        down is decided once the whole grammar is in.
     */
    std::vector<Closure> closures_;

    /**
     * @brief The alternatives of every rule holding a non-greedy loop, with the loop's line, whose nullability the
     *        fixed point over the grammar decides once every rule is read.
     */
    std::vector<Closure> lazy_empty_;

    /**
     * @brief Whether each rule matches the empty string, as the formula over the rules it reaches.
     */
    std::map<std::string, Nullable_t, std::less<>> nullability_;

    /**
     * @brief The line each rule's definition opens on, fragments included, which a redefinition is refused against.
     */
    std::map<std::string, std::size_t, std::less<>> definition_lines_;

    /**
     * @brief The rule being read, which a closure is recorded under.
     */
    std::string rule_;

    /**
     * @brief Whether the rule being read holds a non-greedy loop, which lexer_rule() records with its name.
     */
    bool lazy_{false};
};

/**
 * @brief The first surrogate, the last high one, the first low one and the last: the code points UTF-16 spends on
 *        pairs, which no character is and no UTF-8 input decodes to.
 */
constexpr char32_t first_surrogate{0xD800};

constexpr char32_t last_high_surrogate{0xDBFF};

constexpr char32_t first_low_surrogate{0xDC00};

constexpr char32_t last_surrogate{0xDFFF};

/**
 * @brief Whether a scalar is a surrogate.
 * @param scalar The scalar.
 * @return True when it is.
 */
[[nodiscard]] constexpr bool surrogate(const char32_t scalar) noexcept
{
    return scalar >= first_surrogate && scalar <= last_surrogate;
}

/**
 * @brief The UTF-16 code units a span of UTF-8 text holds, which is how ANTLR's lexer, reading the grammar into Java
 *        strings, measures it: one per character up to U+FFFF, two per character beyond.
 * @param text The text.
 * @param begin The span's first offset.
 * @param end The offset past its last.
 * @return The count.
 */
[[nodiscard]] std::size_t units(const std::string_view text, const std::size_t begin, const std::size_t end)
{
    std::size_t count{0};

    for (const auto byte : text.substr(begin, end - begin))
    {
        const auto value{static_cast<unsigned char>(byte)};

        count += (value & 0xC0U) == 0x80U ? 0 : value >= 0xF0 ? 2 : 1;
    }

    return count;
}

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
 * @brief Adds a range of characters, a set's member or span or a `'a'..'z'`, to an alphabet, folded as ANTLR folds
 *        a range under `caseInsensitive`.
 *
 * ANTLR folds each range by its two ends alone (LexerATNFactory.checkRangeAndAddToSet over
 * RangeBorderCharactersData): where neither end changes case or the ends differ in case, one being a letter of the
 * other case or no letter, or the copies of the ends in lower and in upper case are not one width apart, the range
 * stands as written; otherwise the copy in lower case and the copy in upper case are both added. So `[a-z]` and `'q'`
 * gain `A-Z` and `Q`, while `[A-t]`, `[0-Z]` and `[a-]` admit exactly what they spell, the letters inside them
 * folded no further, which ANTLR's warning 185 remarks on for the first two; the set is not closed under case.
 * Beyond ASCII, where ANTLR's case mappings are Unicode's, the range is added as written, the caller refusing such
 * a range under the option.
 * @param alphabet The alphabet, widened on return.
 * @param first The first character.
 * @param last The last, no lower than the first.
 * @param case_insensitive Whether the option is in force.
 */
void admit(Alphabet& alphabet, const char32_t first, const char32_t last, const bool case_insensitive)
{
    const auto add{[&alphabet](const char32_t low, const char32_t high) {
        for (auto value{low}; value <= std::min<char32_t>(high, 0x7F); ++value)
        {
            alphabet.ascii.set(value);
        }

        if (high >= 0x80)
        {
            alphabet.beyond.push_back({.first = std::max<char32_t>(low, 0x80), .last = high});
        }
    }};

    const auto lower{[](const char32_t value) {
        return value >= 'A' && value <= 'Z' ? static_cast<char32_t>(value | 0x20U) : value;
    }};

    const auto upper{[](const char32_t value) {
        return value >= 'a' && value <= 'z' ? static_cast<char32_t>(value & ~0x20U) : value;
    }};

    const auto lower_first{lower(first)};

    const auto upper_first{upper(first)};

    const auto lower_last{lower(last)};

    const auto upper_last{upper(last)};

    const auto mixed{(lower_first == first) != (lower_last == last)};

    const auto single{
            (lower_first == upper_first && lower_last == upper_last) || mixed ||
            lower_last + upper_first != upper_last + lower_first};

    if (!case_insensitive || single)
    {
        add(first, last);

        return;
    }

    add(lower_first, lower_last);

    add(upper_first, upper_last);
}

/**
 * @brief The characters of a range, `'a'..'z'`, as an alphabet.
 * @param low The first character.
 * @param high The last, no lower than the first.
 * @param case_insensitive Whether the range folds as ANTLR folds one under `caseInsensitive`.
 * @return The alphabet.
 */
[[nodiscard]] Alphabet spanning(const char32_t low, const char32_t high, const bool case_insensitive)
{
    Alphabet alphabet;

    admit(alphabet, low, high, case_insensitive);

    return alphabet;
}

/**
 * @brief Whether a channel command's argument names a channel other than the default one, the one a parser reads,
 *        resolved as ANTLR resolves it (LexerATNFactory.getChannelConstantValue): `HIDDEN` and
 *        `DEFAULT_TOKEN_CHANNEL` are its constants one and zero, another of its reserved names is its error 172, a
 *        name the grammar's `channels` block declares is a channel from two up, and anything else is read as a
 *        decimal number, `00` and `000` being zero and the default channel and every other number a channel of its
 *        own, a number beyond its int or a name nothing declares being its error 177, each in its words.
 * @param argument The argument as written.
 * @param declared The channels the grammar declares.
 * @param line The command's line, which a refusal names.
 * @return True when the token goes to a channel a parser does not read.
 * @throws Spec_error As ANTLR's errors 172 and 177 refuse the argument.
 */
[[nodiscard]] bool hidden(
        const std::string& argument, const std::set<std::string, std::less<>>& declared, const std::size_t line)
{
    if (argument == "HIDDEN")
    {
        return true;
    }

    if (argument == "DEFAULT_TOKEN_CHANNEL")
    {
        return false;
    }

    for (const std::string_view reserved : {"DEFAULT_MODE", "SKIP", "MORE", "EOF", "MAX_CHAR_VALUE", "MIN_CHAR_VALUE"})
    {
        if (argument == reserved)
        {
            throw Spec_error{"cannot use or declare channel with reserved name " + argument, line};
        }
    }

    if (declared.contains(argument))
    {
        return true;
    }

    // Integer.parseInt: the digits, leading zeros dropped, up to 2147483647.
    constexpr std::string_view largest{"2147483647"};

    const auto digits{std::string_view{argument}.substr(std::min(argument.find_first_not_of('0'), argument.size()))};

    const auto numeric{!argument.empty() && std::ranges::all_of(argument, [](const char byte) {
        return byte >= '0' && byte <= '9';
    })};

    if (numeric && (digits.size() < largest.size() || (digits.size() == largest.size() && digits <= largest)))
    {
        return !digits.empty();
    }

    throw Spec_error{argument + " is not a recognized channel name", line};
}

/**
 * @brief How a refusal of a `->` clause's syntax ends: ANTLR's parser rejects the byte named before any command is
 *        looked at, its error 50 while matching a lexer rule.
 */
constexpr std::string_view rejected{", which ANTLR's parser rejects while matching a lexer rule"};

/**
 * @brief The words of ANTLR's error 50 for a `)` closing parens that hold nothing, `skip()` and `type( )`, which its
 *        parser rejects at that `)`.
 */
constexpr std::string_view surprise{"syntax error: ')' came as a complete surprise to me while matching a lexer rule"};

/**
 * @brief A text with the blanks that end it dropped, what a rule's pattern and its clause are kept as.
 * @param text The text.
 * @return The text through its last byte that is no blank.
 */
[[nodiscard]] std::string without_trailing_blanks(std::string text)
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r'))
    {
        text.pop_back();
    }

    return text;
}

/**
 * @brief The formula of something that never matches the empty string.
 * @return No term.
 */
[[nodiscard]] Nullable_t never_empty()
{
    return {};
}

/**
 * @brief The formula of something that matches the empty string whatever the rules do.
 * @return One term of no rule.
 */
[[nodiscard]] Nullable_t always_empty()
{
    return {{}};
}

/**
 * @brief Whether a formula holds, given the rules known to match the empty string.
 * @param formula The formula.
 * @param nullable The rules that match the empty string, none for the answer a formula gives on its own.
 * @return True when one term's every rule is among them.
 */
[[nodiscard]] bool matches_empty(const Nullable_t& formula, const std::set<std::string, std::less<>>& nullable)
{
    return std::ranges::any_of(formula, [&nullable](const std::vector<std::string>& term) {
        return std::ranges::all_of(term, [&nullable](const std::string& name) { return nullable.contains(name); });
    });
}

/**
 * @brief The formula of a match of either of two, an alternation's: their terms together.
 * @param left One formula.
 * @param right The other.
 * @return The disjunction, one term of no rule where either holds on its own.
 */
[[nodiscard]] Nullable_t either_empty(Nullable_t left, const Nullable_t& right)
{
    left.insert(left.end(), right.begin(), right.end());

    return matches_empty(left, {}) ? always_empty() : left;
}

/**
 * @brief The formula of a match of both of two, a sequence's: every term of the one joined with every term of the
 *        other, so that a term holds when all the rules it gathered do.
 * @param left One formula.
 * @param right The other.
 * @return The conjunction, one term of no rule where both hold on their own.
 */
[[nodiscard]] Nullable_t both_empty(const Nullable_t& left, const Nullable_t& right)
{
    Nullable_t joined;

    for (const auto& outer : left)
    {
        for (const auto& inner : right)
        {
            auto term{outer};

            term.insert(term.end(), inner.begin(), inner.end());

            joined.push_back(std::move(term));
        }
    }

    return matches_empty(joined, {}) ? always_empty() : joined;
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
 * @brief Whether the body of an action provably does nothing the token stream can see: blanks and comments only, so
 *        that no call runs at all.
 *
 * Any statement in an action may reach the lexer's own state, `more()`, `skip()`, `setType()` and `setText()` among
 * the calls ANTLR's runtime offers, and a call to a member of the grammar's own `@members` block may reach them
 * indirectly, so nothing but an empty body is inert.
 * @param text The grammar's text.
 * @param begin The offset just past the action's `{`.
 * @param end The offset of its `}`.
 * @return True when the body holds nothing but blanks and comments, a `//` comment ending at a carriage return as
 *         ANTLR's lexer ends one, so an action after a bare return is code and not comment.
 */
[[nodiscard]] bool inert(const std::string_view text, const std::size_t begin, const std::size_t end)
{
    try
    {
        Cursor body{text, begin, end, Line_comment_end::newline_or_return};

        body.skip_blanks();

        return body.done();
    }
    catch (const Spec_error&)
    {
        // A comment left open inside the body is no proof of anything.
        return false;
    }
}

/**
 * @brief The one string the rest of a sequence spells, when every element of it is an exact ASCII literal that
 *        matches once: what ANTLR's fewest-characters rule stops a non-greedy loop before it at.
 *
 * An inert action matches nothing and is stepped over. Anything else, a set, a group, a reference, a suffixed
 * element or a literal with a letter under `caseInsensitive` or a byte beyond ASCII, leaves the rest more than one
 * string and the loop with no rewrite the automaton below can build.
 * @param elements The sequence's elements.
 * @param from The index of the first element after the loop.
 * @return The bytes, or std::nullopt when the rest is not one fixed ASCII string.
 */
[[nodiscard]] std::optional<std::string> rest_spelling(const std::vector<Element>& elements, const std::size_t from)
{
    std::string bytes;

    for (const auto& [expression, literal, spelling, alphabet, first, nullable, one_length, characters, suffix, lazy] :
         elements | std::views::drop(from))
    {
        if (expression.empty())
        {
            continue;
        }

        const auto ascii{literal && std::ranges::none_of(*literal, [](const char byte) {
                             return static_cast<unsigned char>(byte) >= 0x80;
                         })};

        if (!ascii || suffix != 0)
        {
            return std::nullopt;
        }

        bytes += *literal;
    }

    return bytes.empty() ? std::nullopt : std::optional{std::move(bytes)};
}

/**
 * @brief Whether the loop's body, having read the terminator's first `length` bytes and nothing longer of it, is
 *        already past the point ANTLR's loop stops at, because the terminator appended there spells an occurrence of
 *        itself that begins inside the body.
 *
 * Appending the terminator after a prefix of it of length j spells an occurrence beginning j bytes early exactly
 * when the terminator's own bytes from j on are its first bytes, that is when j is a period of it. `'aa'` after one
 * `a` is the case: the body's `a` and the terminator's first `a` are an occurrence of `aa`, so ANTLR's fewest
 * characters stopped a byte earlier and the body may not end there. The longest prefix the body ends in is the only
 * one to test: a shorter prefix the body also ends in is a suffix of the longest one, and a periodic terminator's
 * suffix of that kind makes the longest one a period too.
 * @param terminator The terminator's bytes.
 * @param length The length of the longest prefix of the terminator the body ends with, below the whole of it.
 * @return True when the terminator completes that prefix into an occurrence of itself.
 */
[[nodiscard]] bool overlapped(const std::string_view terminator, const std::size_t length)
{
    return length > 0 && terminator.substr(length) == terminator.substr(0, terminator.size() - length);
}

/**
 * @brief The regex over an alphabet of every string a non-greedy loop before a terminator matches: the loop stops at
 *        the first point the terminator can follow, so its body holds no occurrence of the terminator and does not
 *        end where the terminator would complete one, ANTLR's fewest characters that still let the rest match.
 *
 * Built as the automaton that tracks the longest prefix of the terminator ending at the byte just read, the steps
 * that would reach the whole terminator dropped, and then written out by eliminating its states one by one. A state
 * the terminator overlaps into an earlier occurrence is no end of the body, which is what keeps `.*? 'aa'` from
 * matching `aaa`. The terminator is ASCII, so a scalar beyond ASCII never extends a prefix and takes every state
 * back to the start.
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
            if (!onto[to].any())
            {
                continue;
            }

            join(label[from][to], bracket(onto[to]));
        }

        if (!alphabet.beyond.empty())
        {
            join(label[from][0], step(Alphabet{.ascii = {}, .beyond = alphabet.beyond}));
        }

        if (!overlapped(terminator, from))
        {
            join(label[from][final], "");
        }
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

    const auto rest{label[0][final].value_or("")};

    // Where the body can only be empty, `'a'*? 'aa'` over the one character the terminator begins with, the loop
    // contributes nothing and the terminator alone is the match; an empty group is no pattern the parser reads.
    return loop.empty() && rest.empty() ? std::string{} : std::format("({}{})", loop, rest);
}

/**
 * @brief The words of ANTLR's error 144 for a literal a range's end or a negation needs one character of and does not
 *        get: a multi-character literal, an empty one, a character beyond the basic multilingual plane written out, or
 *        a surrogate pair of two escapes, the last two being two UTF-16 units to it.
 * @param spelling The literal as written, quotes included.
 * @return The message.
 */
[[nodiscard]] std::string multi_character(const std::string_view spelling)
{
    return "multi-character literals are not allowed in lexer sets: " + std::string{spelling};
}

/**
 * @brief The words of ANTLR's error 174 for a range whose end is below its start and for an empty set.
 * @param spelling The range or set as written.
 * @return The message.
 */
[[nodiscard]] std::string empty_range(const std::string_view spelling)
{
    return "string literals and sets cannot be empty: " + std::string{spelling};
}

Grammar::Grammar(const std::string_view source) : Cursor{source, Line_comment_end::newline_or_return}
{}

void Grammar::skip_blanks()
{
    for (Cursor::skip_blanks(); at("\xEF\xBB\xBF"); Cursor::skip_blanks())
    {
        at_ += 3;
    }
}

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

    // Only a lexer grammar may declare modes, so where the rules come from decides whether a `mode` line is ANTLR.
    const auto lexer_only{keyword.ends_with("lexer")};

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

    // The lexer's members actions, read once the grammar's options are known, since the option naming the target
    // language may stand after them and decides what their code is written in.
    std::vector<std::pair<std::string_view, std::size_t>> members;

    std::string actions_code;

    for (skip_blanks(); peek(); skip_blanks())
    {
        const auto opened{at_};

        if (peek() == '@')
        {
            // A named action, `@header { }` or `@lexer::members { }`. Its code is the target language's and is
            // skipped, but a `members` action of the lexer's is written into the generated lexer class, where a
            // definition of one of the methods deciding the token stream replaces the runtime's own: a `nextToken`
            // of one's own can join two matches into one token, so the tokens the rules describe are not the tokens
            // the scanner emits. Such a definition is refused by name, as a scanner hook is wherever one stands.
            ++at_;

            // ANTLR's own lexer parts the action's name from the `@`, the `::` and the scope by nothing, so blanks
            // and comments may stand at each of those joints: `@lexer :: members` and `@ members` name what
            // `@lexer::members` names, and a grammar writing one of them is the grammar writing the other.
            skip_blanks();

            auto named{identifier()};

            skip_blanks();

            if (at("::"))
            {
                at_ += 2;

                skip_blanks();

                named = named + "::" + identifier();
            }

            while (peek() && *peek() != '{')
            {
                ++at_;
            }

            const auto opened{at_};

            skip_block();

            // A `lexer::members` action is the lexer's wherever it stands, and an unscoped `members` action is
            // written into both classes a combined grammar generates, the lexer's among them, as ANTLR's grammar
            // documentation has it; only a `parser::members` action leaves the lexer alone. The twelfth bundle
            // read this the other way, on a recollection rather than the document, and accepted a combined
            // grammar whose unscoped members override the lexer's own nextToken.
            // The actions written into the lexer class: `members` under every target, and the C++ target's
            // `declarations`, which its template writes inside the class in the header; its `definitions` go to
            // the source file at namespace scope, where a function is no method of the lexer's. Every action's
            // code, `header` among them, may define a macro the members' initializers use.
            static constexpr std::string_view insertions[]{
                    "members", "lexer::members", "declarations", "lexer::declarations"};

            actions_code += text_.substr(opened, at_ - opened);

            actions_code += '\n';

            if (std::ranges::find(insertions, named) != std::ranges::end(insertions))
            {
                members.emplace_back(text_.substr(opened, at_ - opened), opened);
            }

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

        if (word == "tokens")
        {
            // A lexer grammar's block gives its names token types ahead of every rule; a combined grammar's goes to
            // the parser it builds, its lexer none the wiser.
            std::set<std::string, std::less<>> names;

            names_block(names, "token", true);

            if (lexer_only)
            {
                tokens_.merge(names);
            }

            continue;
        }

        if (word == "channels")
        {
            // Only a lexer grammar may declare channels, ANTLR's error 164 in a combined one.
            if (!lexer_only)
            {
                fail("custom channels are not supported in combined grammars");
            }

            channels_line_ = line_of(opened);

            names_block(channels_, "channel", false);

            continue;
        }

        if (word == "import")
        {
            fail("the grammar imports another, whose rules are not here to read");
        }

        if (word == "mode")
        {
            if (!lexer_only)
            {
                fail("lexical modes are only allowed in lexer grammars, so a combined grammar declaring one is no "
                     "ANTLR grammar");
            }

            skip_blanks();

            mode = identifier();

            if (mode.empty())
            {
                fail("a mode needs a name");
            }

            skip_blanks();

            expect(';', "';' to end the mode line");

            // The default mode is reported under the name INITIAL, as every reader's default condition is, so a
            // mode of the grammar's own named INITIAL, which ANTLR allows, could not be told from it.
            if (mode == "INITIAL")
            {
                fail("a mode named INITIAL is refused: the audit reports ANTLR's default mode under that name, so "
                     "a mode of the grammar's own called INITIAL cannot be told from it");
            }

            // ANTLR's parser takes a mode line only after a rule, a fragment counting, its syntax error 50 in
            // its own words where none stands before it; and a mode named again reopens it, the sections' rules
            // one mode's.
            if (spec.definitions.empty())
            {
                at_ = opened;

                fail("syntax error: 'mode' came as a complete surprise to me");
            }

            // A mode named again reopens it, DEFAULT_MODE reopening the default mode, whose rules name no
            // condition; each section is held to ANTLR's error 145 on its own.
            const auto declared{
                    std::ranges::any_of(sections_, [&mode](const Section& section) { return section.name == mode; })};

            sections_.push_back({.name = mode, .line = line_of(opened), .rules = 0});

            if (mode == "DEFAULT_MODE")
            {
                mode.clear();
            }
            else if (!declared)
            {
                spec.conditions.push_back({.name = mode, .exclusive = true});
            }

            continue;
        }

        at_ = opened;

        if (word == "fragment" || (word.front() >= 'A' && word.front() <= 'Z'))
        {
            lexer_rule(spec, mode, case_insensitive);
        }
        else if (lexer_only)
        {
            // ANTLR's error 53, in its words: a lexer grammar holds lexer rules alone.
            fail(std::format("parser rule {} not allowed in lexer", word));
        }
        else
        {
            parser_rule();
        }
    }

    // The lexer's members, read now that the options are known. ANTLR writes them into the lexer class it
    // generates for the target language, and the reading reads one language's declarations, Java's, which is
    // ANTLR's own default; under another target the same members are written in that language, whose declarations
    // this reading does not know, so a grammar naming one is refused by name rather than read as though its
    // members declared nothing.
    const auto target{[&spec]() -> std::string_view {
        for (const auto& option : spec.options)
        {
            if (option.starts_with("language="))
            {
                return std::string_view{option}.substr(9);
            }
        }

        return "Java";
    }()};

    // A superclass named by the grammar may define any method of the lexer's, `nextToken` among them, and its code
    // is not in the grammar to read, so a grammar naming one is refused by name.
    for (const auto& option : spec.options)
    {
        if (option.starts_with("superClass="))
        {
            at_ = declared;

            fail(std::format(
                    "the grammar sets superClass, and the class it names may define the methods that decide the "
                    "tokens, which are out of the audit's sight"));
        }
    }

    // The three targets whose members this reading reads: Java, C++ and C# all write a method as a name, a
    // parameter list and a body. Another target writes one its own way, JavaScript assigning a function to an
    // instance member among them, which is no declaration this reading would find at all, so a grammar naming one
    // is refused by name rather than read as though its members declared nothing.
    static constexpr std::string_view known[]{"Java", "Cpp", "CSharp"};

    for (const auto& [code, at] : members)
    {
        if (std::ranges::find(known, target) == std::ranges::end(known))
        {
            at_ = at;

            fail(std::format(
                    "the grammar's target language is {}, whose members declare a method otherwise than this "
                    "reading reads one, so whether one of them decides the tokens is out of the audit's sight",
                    target));
        }

        Macros_t macros;

        take_macros(actions_code, macros);

        overriding(code, at, target == "CSharp", macros);
    }

    // A rule matches the empty string when one of its alternatives does, and an alternative through the rules it
    // reaches, so the answer is the least fixed point over the grammar: nothing nullable, then whatever the
    // formulas add, until they add nothing. A closure over a body that matches it is ANTLR's error 153.
    std::set<std::string, std::less<>> nullable;

    for (auto growing{true}; growing;)
    {
        growing = false;

        for (const auto& [name, formula] : nullability_)
        {
            if (!nullable.contains(name) && matches_empty(formula, nullable))
            {
                nullable.insert(name);

                growing = true;
            }
        }
    }

    for (const auto& [rule, line, body] : closures_)
    {
        if (matches_empty(body, nullable))
        {
            throw Spec_error{
                    "the rule " + rule +
                            " contains a closure with at least one alternative that can match the empty string, "
                            "which ANTLR rejects",
                    line};
        }
    }

    // ANTLR's empty match reaches the rule's end at the loop's decision and stops the loop, wherever the empty
    // alternative stands, so a loop in such a rule is refused; the alternatives were kept until the fixed point
    // above could say which of them matches the empty string.
    for (const auto& [rule, line, body] : lazy_empty_)
    {
        if (matches_empty(body, nullable))
        {
            throw Spec_error{
                    "a non-greedy loop is read only in a rule no alternative of which can match the empty string, "
                    "since ANTLR's empty match reaches the rule's end at the loop's decision and stops the loop, "
                    "which the byte reading cannot express",
                    line};
        }
    }

    // A rule another rule references is inlined into that one, whose rest reaches past it, and a non-greedy loop
    // stops where the rest of the surrounding rule matches; so a rule holding such a loop is read only while
    // nothing references it, which the whole grammar has to be in to say.
    for (const auto& [name, line] : lazy_rules_)
    {
        const auto reference{'{' + name + '}'};

        const auto referenced{
                std::ranges::any_of(
                        spec.rules,
                        [&reference](const Lexer_spec::Rule& rule) { return rule.expression.contains(reference); }) ||
                std::ranges::any_of(spec.definitions, [&reference, &name](const auto& definition) {
                    return definition.first != name && definition.second.contains(reference);
                })};

        if (referenced)
        {
            throw Spec_error{
                    "the rule " + name +
                            " holds a non-greedy loop and another rule references it, so what ANTLR stops the loop "
                            "at is the rest of that rule and not of this one",
                    line};
        }
    }

    // The literals the parser rules use are implicit tokens ahead of every explicit rule, unless a rule spells
    // exactly that literal in a shape ANTLR maps it onto, when the parser's literal is that rule's token; two rules
    // spelling it leave ANTLR no token to map it onto, and it rejects the parser's use of the literal.
    std::vector<Lexer_spec::Rule> implicit;

    for (const auto& [text, line] : parser_literals_)
    {
        const auto aliased{aliases_.find(text)};

        if (aliased != aliases_.end() && aliased->second > 1)
        {
            throw Spec_error{
                    "two lexer rules spell " + text +
                            ", so ANTLR maps it onto neither and rejects the parser's use of it: cannot create "
                            "implicit token for string literal in non-combined grammar: " +
                            text,
                    line};
        }

        const auto placed{
                std::ranges::any_of(implicit, [&text](const Lexer_spec::Rule& rule) { return rule.pattern == text; })};

        if (aliased != aliases_.end() || placed)
        {
            continue;
        }

        Grammar reader{text};

        ++reader.at_;

        const auto bytes{reader.literal().bytes};

        const auto beyond{
                std::ranges::any_of(bytes, [](const char one) { return static_cast<unsigned char>(one) >= 0x80; })};

        if (case_insensitive && beyond)
        {
            throw Spec_error{std::string{unfoldable}, line};
        }

        implicit.push_back(
                {.pattern = text,
                 .expression = case_insensitive ? caseless(bytes) : quoted(bytes),
                 .conditions = {},
                 .action = {},
                 .token = text,
                 .priority = std::nullopt,
                 .line = line});
    }

    // ANTLR's error 51 again: a combined grammar's implicit tokens are rules named `T__k`, so an explicit rule of
    // that name is a redefinition, in ANTLR's words, the implicit one having no line.
    for (std::size_t index{0}; index < implicit.size(); ++index)
    {
        const auto name{"T__" + std::to_string(index)};

        if (spec.definitions.contains(name))
        {
            throw Spec_error{std::format("rule {} redefinition; previous at line 0", name), definition_lines_.at(name)};
        }
    }

    // ANTLR's errors 170, 161 and 162: a mode's name is no token's, a channel's name is no token's and no mode's,
    // the tokens being the rules with a token type of their own and the `tokens` entries, anywhere in the grammar.
    for (const auto& [name, line, rules] : sections_)
    {
        if (name != "DEFAULT_MODE" && (rule_names_.contains(name) || tokens_.contains(name)))
        {
            throw Spec_error{std::format("mode {} conflicts with token with same name", name), line};
        }
    }

    for (const auto& channel : channels_)
    {
        if (rule_names_.contains(channel) || tokens_.contains(channel))
        {
            throw Spec_error{std::format("channel {} conflicts with token with same name", channel), channels_line_};
        }

        if (std::ranges::any_of(sections_, [&channel](const Section& section) { return section.name == channel; }))
        {
            throw Spec_error{std::format("channel {} conflicts with mode with same name", channel), channels_line_};
        }
    }

    // ANTLR's error 175: a `type` names a token the grammar has, anywhere in it: a rule that is no fragment and
    // has a token type of its own, a `tokens` entry, or `T__k`, the k-th of the implicit tokens a combined grammar
    // makes of the parser's literals no rule spells, numbered in the order the parser rules use them; a rule typed
    // `T__k` emits that literal's token, named by the literal as the implicit rule's is.
    for (const auto& [name, line] : typed_names_)
    {
        if (rule_names_.contains(name) || tokens_.contains(name))
        {
            continue;
        }

        const auto digits{name.starts_with("T__") ? name.substr(3) : std::string{}};

        // The spelling is exact, `T__0` and never `T__00`, as ANTLR's own table names them.
        const auto numbered{
                !digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos && digits.size() <= 6 &&
                digits == std::to_string(std::stoul(digits)) && std::stoul(digits) < implicit.size()};

        if (!numbered)
        {
            throw Spec_error{name + " is not a recognized token name", line};
        }

        const auto& literal{*implicit[std::stoul(digits)].token};

        for (auto& rule : spec.rules)
        {
            if (rule.token == name)
            {
                rule.token = literal;
            }
        }
    }

    spec.rules.insert(
            spec.rules.begin(), std::make_move_iterator(implicit.begin()), std::make_move_iterator(implicit.end()));

    // ANTLR's error 145: each `mode` section holds a rule that is no fragment, a reopened mode's sections each.
    for (const auto& [mode, line, rules] : sections_)
    {
        if (rules == 0)
        {
            throw Spec_error{std::format("lexer mode {} must contain at least one non-fragment rule", mode), line};
        }
    }

    // ANTLR's error 176: a `mode` or `pushMode` names a mode the grammar declares, before or after it, or
    // DEFAULT_MODE; a number names a mode by its index and is kept as written.
    for (const auto& [name, line] : mode_names_)
    {
        const auto declared{name == "DEFAULT_MODE" || std::ranges::any_of(sections_, [&name](const Section& section) {
                                return section.name == name;
                            })};

        if (!declared)
        {
            throw Spec_error{name + " is not a recognized mode name", line};
        }
    }

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

void Grammar::overriding(const std::string_view code, const std::size_t at, const bool csharp, const Macros_t& macros)
{
    // The code is the target language's and is read as that language reads it, by a tokenizer that splices no
    // lines and ends a line comment at a carriage return: a comment ending in a backslash hides nothing under it,
    // and a name inside a comment or a string declares nothing. A method is a name, a parameter list and a body,
    // the body a braced block or `=>` and an expression, with a `throws` clause allowed between; a call is not
    // one, and neither is a name reached through a value. The members' own braces are part of the code, so a
    // member of the lexer class stands one brace deep; a method of a class declared inside the block is deeper
    // and belongs to that class.
    const auto tokens{java_tokens(code)};

    const auto text{[&tokens](const std::size_t index) {
        return index < tokens.size() ? std::string_view{tokens[index].text} : std::string_view{};
    }};

    const auto named{[](const std::string_view word) {
        return !word.empty() && (std::isalpha(static_cast<unsigned char>(word.front())) != 0 || word.front() == '_');
    }};

    auto depth{0};

    for (std::size_t index{0}; index < tokens.size(); ++index)
    {
        // A brace at the class's own level that no declaration opens is an initializer block, which runs when the
        // lexer is built and may set its mode before any token; what it does is out of sight, so it is refused.
        if (depth == 1 && text(index) == "{")
        {
            const auto before{index > 0 ? text(index - 1) : std::string_view{}};

            if (before.empty() || before == ";" || before == "}" || before == "{" || before == "static")
            {
                at_ = at;

                fail("the lexer's members hold an initializer block, which runs when the lexer is built and may "
                     "decide its mode before any token, so what it does is out of the audit's sight");
            }
        }

        // A C++ field may be initialized with braces, `int initial{(setMode(IN), 0)};` or `Initializer startup{this};`
        // whose constructor may set the mode through the lexer it is handed, which runs when the lexer is built as
        // `=` does, so `this` is no value an initializer may hold: a brace at the class's own level that a name opens,
        // where the declaration before it is no type's, `class Inner {`, and no method's, whose parameter list stands
        // before its body, is such an initializer, and it is read by the rule an `=` initializer is read by. An array's
        // brackets may stand between the name and the brace, `int initial[1]{...}`.
        auto declarator{index > 0 ? index - 1 : 0};

        for (auto groups{0}; declarator > 0 && (text(declarator) == "]" || groups > 0); --declarator)
        {
            groups += text(declarator) == "]" ? 1 : text(declarator) == "[" ? -1 : 0;
        }

        if (depth == 1 && text(index) == "{" && index > 0 && named(text(declarator)))
        {
            auto start{index};

            while (start > 0 && text(start - 1) != ";" && text(start - 1) != "}" && text(start - 1) != "{")
            {
                --start;
            }

            static constexpr std::string_view types[]{"class", "struct", "union", "enum", "interface", "record"};

            auto declares_type{false};

            auto parameters{false};

            // A parenthesis inside an array's bound, `int a[(1)]`, opens no parameter list. A type is defined
            // where its keyword's name is followed by the body or a base clause, `struct Inner {` and `class D :
            // B {`; `struct Initializer startup{this};` names a type and declares a field, whose initializer is
            // read as any other's.
            for (auto piece{start}, brackets{0UZ}; piece < index; ++piece)
            {
                const auto keyword{std::ranges::find(types, text(piece)) != std::ranges::end(types)};

                declares_type = declares_type || (keyword && named(text(piece + 1)) &&
                                                  (text(piece + 2) == "{" || text(piece + 2) == ":"));

                brackets += text(piece) == "[" ? 1 : text(piece) == "]" && brackets > 0 ? -1 : 0;

                parameters = parameters || (text(piece) == "(" && brackets == 0);
            }

            // A C# property's accessors, `{ get; set; }`, run when the property is used and not when the lexer is
            // built, and initialize nothing; under the C++ target `get` is a name like any other, a type's among
            // them, and no accessor.
            const auto accessors{
                    csharp && (text(index + 1) == "get" || text(index + 1) == "set" || text(index + 1) == "init")};

            if (!declares_type && !parameters)
            {
                auto close{index};

                for (auto groups{0}; close < tokens.size(); ++close)
                {
                    groups += text(close) == "{" ? 1 : text(close) == "}" ? -1 : 0;

                    if (groups == 0)
                    {
                        break;
                    }
                }

                if (accessors)
                {
                    // An accessor with a body, `get { ... }` or `get => ...`, is a method the runtime may call
                    // through a virtual property, `CharIndex` among them, and is refused as a method is; an
                    // auto-property's `get;` and `set;` run nothing of the grammar's own.
                    for (auto piece{index + 1}; piece < close; ++piece)
                    {
                        if (text(piece) == "{" || (text(piece) == "=" && text(piece + 1) == ">"))
                        {
                            at_ = at;

                            fail(std::format(
                                    "the lexer's members define an accessor of {} with a body, and the generated "
                                    "lexer reads its own properties to decide the tokens without this reading "
                                    "knowing which, so the tokens the rules describe may not be the tokens the "
                                    "scanner emits",
                                    text(declarator)));
                        }
                    }

                    index = close;

                    continue;
                }

                const auto plain{[&text, &named, &macros](const std::size_t piece) {
                    const auto word{text(piece)};

                    const auto sign{
                            (word == "-" || word == "+") && (text(piece - 1) == "{" || text(piece - 1) == ",") &&
                            !text(piece + 1).empty() &&
                            std::isdigit(static_cast<unsigned char>(text(piece + 1).front())) != 0};

                    // A name a macro of an action's defines, `START_IN_MODE` under `#define START_IN_MODE
                    // (setMode(IN), 0)` in the header, stands for its replacement, which is a value only when the
                    // macro is transparent.
                    const auto opaque_macro{macros.contains(word) && plain_values(word, macros).empty()};

                    return (named(word) && word != "this" && !opaque_macro) || word == "." || word == "," ||
                           word == "{" || word == "}" || sign || word.starts_with('"') || word.starts_with('\'') ||
                           std::isdigit(static_cast<unsigned char>(word.front())) != 0;
                }};

                for (auto piece{index + 1}; piece < close; ++piece)
                {
                    if (!plain(piece))
                    {
                        at_ = at;

                        fail(std::format(
                                "the lexer's members initialize {} with more than a value, which runs when the lexer "
                                "is built and may decide its mode before any token, so what it does is out of the "
                                "audit's sight",
                                text(declarator)));
                    }
                }

                index = close;

                continue;
            }
        }

        depth += text(index) == "{" ? 1 : text(index) == "}" ? -1 : 0;

        // A field's initializer runs when the lexer is built too, `int startupMode = (_mode = IN);` and
        // `int initial[] = { _mode = IN };` alike, and the Java runtime's `_mode` is a field the members can assign.
        // An initializer that is values, names and dotted names, with the commas, braces and brackets that group
        // them and a sign opening a number, decides nothing of the scanner; any other, an assignment, a call, a
        // `new`, a step or an operator among them, is out of sight and refused. The tokenizer keeps `==`, `<=`,
        // `!=`, `+=` and `=>` as separate bytes, so the `=` of a declaration is one with no operator byte beside it.
        static constexpr std::string_view operators[]{"=", "!", "<", ">", "+", "-", "*", "/", "%", "&", "|", "^"};

        const auto before{index > 0 ? text(index - 1) : std::string_view{}};

        if (depth == 1 && text(index) == "=" && text(index + 1) != "=" && text(index + 1) != ">" &&
            std::ranges::find(operators, before) == std::ranges::end(operators))
        {
            auto scan{index + 1};

            for (auto groups{0UZ}; scan < tokens.size() && !(groups == 0 && text(scan) == ";"); ++scan)
            {
                groups += text(scan) == "(" || text(scan) == "{" || text(scan) == "[" ? 1 :
                          text(scan) == ")" || text(scan) == "}" || text(scan) == "]" ? -1 :
                                                                                        0;
            }

            const auto plain{[&text, &named, &macros](const std::size_t piece) {
                const auto word{text(piece)};

                const auto grouping{
                        word == "." || word == "," || word == "{" || word == "}" || word == "[" || word == "]"};

                const auto sign_stands{text(piece - 1) == "=" || text(piece - 1) == "," || text(piece - 1) == "{"};

                const auto signed_number{
                        sign_stands && (word == "-" || word == "+") && !text(piece + 1).empty() &&
                        std::isdigit(static_cast<unsigned char>(text(piece + 1).front())) != 0};

                const auto opaque_macro{macros.contains(word) && plain_values(word, macros).empty()};

                return (named(word) && word != "this" && !opaque_macro) || grouping || signed_number ||
                       word.starts_with('"') || word.starts_with('\'') ||
                       std::isdigit(static_cast<unsigned char>(word.front())) != 0;
            }};

            // The field's name stands before the `=`, an array declarator's brackets stepped over.
            auto field{index - 1};

            while (field > 0 && (text(field) == "[" || text(field) == "]"))
            {
                --field;
            }

            for (auto piece{index + 1}; piece < scan; ++piece)
            {
                if (!plain(piece))
                {
                    at_ = at;

                    fail(std::format(
                            "the lexer's members initialize {} with more than a value, which runs when the lexer is "
                            "built and may decide its mode before any token, so what it does is out of the audit's "
                            "sight",
                            text(field)));
                }
            }

            index = scan;

            continue;
        }

        if (depth != 1 || !named(text(index)) || text(index + 1) != "(")
        {
            continue;
        }

        // `sizeof(int)` inside an array's bound, `int data[sizeof(int)]{0};`, is a call inside brackets and no
        // method's parameter list.
        auto bracketed{false};

        for (auto back{index}, opened{0UZ};
             back > 0 && text(back - 1) != ";" && text(back - 1) != "}" && text(back - 1) != "{"; --back)
        {
            opened += text(back - 1) == "[" ? 1 : text(back - 1) == "]" && opened > 0 ? -1 : 0;

            bracketed = bracketed || (text(back - 1) == "[" && opened == 1);
        }

        if (bracketed)
        {
            continue;
        }

        if (index > 0 && (text(index - 1) == "." || text(index - 1) == "::" || text(index - 1) == "new"))
        {
            continue;
        }

        auto scan{index + 1};

        for (auto groups{0UZ}; scan < tokens.size(); ++scan)
        {
            groups += text(scan) == "(" ? 1 : text(scan) == ")" ? -1 : 0;

            if (groups == 0)
            {
                break;
            }
        }

        for (++scan; scan < tokens.size() && text(scan) != "{" && text(scan) != ";" &&
                     !(text(scan) == "=" && text(scan + 1) == ">");
             ++scan)
        {
        }

        // The generated lexer calls its own methods, and which of them decide the tokens is the runtime's to know
        // and not this reading's: `emit` reaches the token's end through `getCharIndex`, and a list of the
        // methods that matter would be a guess at the runtime's virtual calls. A method the members define may
        // therefore stand in place of one the runtime calls at every token, and any one of them is refused.
        if (scan < tokens.size() && text(scan) != ";")
        {
            at_ = at;

            fail(std::format(
                    "the lexer's members define {}(), and the generated lexer calls its own methods to decide the "
                    "tokens without this reading knowing which, so the tokens the rules describe may not be the "
                    "tokens the scanner emits",
                    text(index)));
        }
    }
}

void Grammar::skip_argument()
{
    const auto opened{at_};

    std::size_t depth{0};

    do
    {
        if (!peek())
        {
            at_ = opened;

            fail("an argument block never closes");
        }

        const auto byte{next("']'")};

        if (byte == '\'' || byte == '"')
        {
            while (peek() && *peek() != byte)
            {
                at_ += *peek() == '\\' ? 2 : 1;
            }

            ++at_;
        }
        else if (byte == '[')
        {
            ++depth;
        }
        else if (byte == ']')
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

        // ANTLR reads the value with the lexer it reads the grammar with, one token: a name, dotted or not, a
        // number, a quoted string or a brace block, the blanks and comments around it no part of it.
        const auto begin{at_};

        if (peek() == '\'')
        {
            ++at_;

            std::ignore = literal();
        }
        else if (peek() == '{')
        {
            skip_block();
        }
        else
        {
            while (peek() && (is_name_byte(*peek()) || *peek() == '.'))
            {
                ++at_;
            }
        }

        const std::string value{text_.substr(begin, at_ - begin)};

        if (value.empty())
        {
            fail("an option needs a value");
        }

        skip_blanks();

        expect(';', "';' to end the option");

        options.push_back(name + '=' + value);

        // ANTLR takes `true` and `false` and no other spelling, `TRUE` among them: another value is its warning 84
        // and sets nothing, so the grammar's option stays off and a rule's option leaves the grammar's in force.
        if (name == "caseInsensitive" && (value == "true" || value == "false"))
        {
            case_insensitive = value == "true";
        }
    }

    ++at_;

    return case_insensitive;
}

void Grammar::names_block(
        std::set<std::string, std::less<>>& names, const std::string_view kind, const bool may_be_empty)
{
    skip_blanks();

    expect('{', std::format("'{{' to open the {}s block", kind));

    skip_blanks();

    if (may_be_empty && accept('}'))
    {
        return;
    }

    for (;;)
    {
        if (!peek())
        {
            fail(std::format("expected '}}' to close the {}s block", kind));
        }

        const auto name{identifier()};

        if (name.empty())
        {
            fail(std::format(
                    "syntax error: '{}' stands where a {} name should, which ANTLR's parser rejects while matching a "
                    "{}s block",
                    *peek(), kind, kind));
        }

        names.insert(name);

        skip_blanks();

        if (accept(','))
        {
            skip_blanks();

            continue;
        }

        if (accept('}'))
        {
            return;
        }

        if (!peek())
        {
            fail(std::format("expected '}}' to close the {}s block", kind));
        }

        // The word standing where a comma or the brace should, `TWO` in `{ ONE TWO }`, or else the byte there.
        const auto next{identifier()};

        fail(std::format(
                "syntax error: '{}' stands after the {} {} where ',' or '}}' should, which ANTLR's parser rejects "
                "while matching a {}s block",
                next.empty() ? std::string{*peek()} : next, kind, name, kind));
    }
}

void Grammar::element_options()
{
    skip_blanks();

    if (accept('>'))
    {
        return;
    }

    for (;;)
    {
        // A name, dotted or not, each dot a token of its own to ANTLR's lexer with blanks and comments allowed on
        // either side of it; a value follows `=` after an undotted name alone, a name dotted the same way, a number,
        // a quoted string or a brace block.
        const auto qualified{[this](std::string name) {
            skip_blanks();

            while (!name.empty() && accept('.'))
            {
                skip_blanks();

                const auto part{identifier()};

                name = part.empty() ? std::string{} : name + '.' + part;

                skip_blanks();
            }

            return name;
        }};

        const auto first{identifier()};

        const auto name{qualified(first)};

        if (name.empty())
        {
            fail("an element option needs a name");
        }

        if (name == first && accept('='))
        {
            skip_blanks();

            if (peek() == '\'')
            {
                ++at_;

                std::ignore = literal();
            }
            else if (peek() == '{')
            {
                skip_block();
            }
            else if (const auto value{identifier()}; !value.empty())
            {
                if (qualified(value).empty())
                {
                    fail("an element option's value needs a name after its dot");
                }
            }
            else
            {
                const auto begin{at_};

                while (peek() && *peek() >= '0' && *peek() <= '9')
                {
                    ++at_;
                }

                if (at_ == begin)
                {
                    fail("an element option needs a value");
                }
            }

            skip_blanks();
        }

        if (accept(','))
        {
            skip_blanks();

            continue;
        }

        expect('>', "'>' to close the element options");

        return;
    }
}

Commands Grammar::commands_of(const std::string_view text)
{
    Commands read;

    auto& [commands, error]{read};

    Grammar cursor{text};

    // The byte under the cursor, quoted for a refusal.
    const auto quoted{[&cursor] { return std::format("'{}'", *cursor.peek()); }};

    // A number as ANTLR's lexer reads one, digits alone; empty where none begins at the cursor.
    const auto number{[&cursor] {
        std::string digits;

        while (cursor.peek() && *cursor.peek() >= '0' && *cursor.peek() <= '9')
        {
            digits.push_back(cursor.next("a digit"));
        }

        return digits;
    }};

    // The words of the refusal for parens a command opens and the clause ends inside of, `type(Y` and `type(`.
    const auto unclosed{[](const std::string& name) {
        return std::format("syntax error: the '(' after {} is never closed by ')'{}", name, rejected);
    }};

    // The offset of the comma taken as a separator before the command about to be read, stray where none follows.
    std::optional<std::size_t> comma;

    for (;;)
    {
        cursor.skip_blanks();

        const auto offset{cursor.offset()};

        // A comma where a command should stand, before the first, after another or ending the clause, is stray.
        if (cursor.done() || cursor.peek() == ',')
        {
            if (comma || !cursor.done())
            {
                error = Syntax_error{
                        .offset = cursor.done() ? *comma : offset,
                        .message = std::string{"syntax error: ',' stands where no command name follows it"} +
                                   std::string{rejected}};
            }

            return read;
        }

        const auto name{cursor.identifier()};

        if (name.empty())
        {
            error = Syntax_error{
                    .offset = offset,
                    .message =
                            std::format("syntax error: {} stands where a command's name should{}", quoted(), rejected)};

            return read;
        }

        cursor.skip_blanks();

        std::string argument;

        if (cursor.accept('('))
        {
            cursor.skip_blanks();

            const auto inside{cursor.offset()};

            if (cursor.done())
            {
                error = Syntax_error{.offset = inside, .message = unclosed(name)};

                return read;
            }

            if (cursor.peek() == ')')
            {
                error = Syntax_error{.offset = inside, .message = std::string{surprise}};

                return read;
            }

            argument = cursor.identifier();

            if (argument.empty())
            {
                argument = number();
            }

            if (argument.empty())
            {
                error = Syntax_error{
                        .offset = inside,
                        .message = std::format(
                                "syntax error: {} stands where the argument of {} should be a name or a number{}",
                                quoted(), name, rejected)};

                return read;
            }

            cursor.skip_blanks();

            if (cursor.done())
            {
                error = Syntax_error{.offset = cursor.offset(), .message = unclosed(name)};

                return read;
            }

            if (!cursor.accept(')'))
            {
                error = Syntax_error{
                        .offset = cursor.offset(),
                        .message = std::format(
                                "syntax error: {} stands after the argument of {} where ')' should close it{}",
                                quoted(), name, rejected)};

                return read;
            }
        }

        commands.push_back({.name = name, .argument = std::move(argument), .offset = offset});

        cursor.skip_blanks();

        if (cursor.done())
        {
            return read;
        }

        const auto after{cursor.offset()};

        if (cursor.accept(','))
        {
            comma = after;

            continue;
        }

        // A command with no comma before it, the `type(B)` of `skip type(B)`, or any other byte.
        const auto following{cursor.identifier()};

        error = Syntax_error{
                .offset = after,
                .message = following.empty() ?
                                   std::format(
                                           "syntax error: {} stands after the command {} where ',' or ';' should{}",
                                           quoted(), name, rejected) :
                                   std::format(
                                           "syntax error: '{}' stands with no comma before it{}", following, rejected)};

        return read;
    }
}

void Grammar::parser_rule()
{
    // Through the `;` that ends the rule, actions and argument blocks stepped over as ANTLR's lexer reads them, the
    // literals kept: a quoted `]` inside an argument block closes nothing, so `r[const char* s="]'x'"]` uses no 'x'.
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

        if (byte == '{')
        {
            skip_block();

            continue;
        }

        if (byte == '[')
        {
            skip_argument();

            continue;
        }

        if (byte == '<')
        {
            // Element options, `<fail='z'>` on a predicate: metadata on the element, and a string among the values
            // is no literal the parser uses.
            ++at_;

            element_options();

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
            skip_argument();

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

    // ANTLR's error 159: the names its lexer commands and channels reserve are no rule's.
    for (const std::string_view reserved :
         {"DEFAULT_MODE", "SKIP", "MORE", "EOF", "MAX_CHAR_VALUE", "MIN_CHAR_VALUE", "HIDDEN", "DEFAULT_TOKEN_CHANNEL"})
    {
        if (name == reserved)
        {
            at_ = opened;

            fail(std::format("cannot declare a rule with reserved name {}", name));
        }
    }

    auto caseless_rule{case_insensitive};

    skip_blanks();

    const auto optioned{at("options")};

    if (optioned)
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

    lazy_ = false;

    rule_ = name;

    const auto alternatives{this->alternatives(caseless_rule)};

    if (lazy_)
    {
        lazy_rules_.emplace_back(name, line_of(opened));
    }

    // In a combined grammar ANTLR maps a parser rule's literal onto the lexer rule that spells it, and what spells
    // it is what ANTLR's own tree patterns match: a rule that is no fragment and carries no options, of one
    // alternative that is the literal alone, bare of element options, the literal and one action, or the literal and
    // one or two commands of which at most one takes an argument; the grammar's comments are in no pattern. A rule
    // of any other shape spelling the literal leaves the parser's literal an implicit token of its own, placed ahead
    // of the rule.
    const auto aliased{[&] {
        const auto& [pattern, expression, commands, clause, spelling, acted, empty, nullable]{alternatives.front()};

        if (fragment || optioned || alternatives.size() != 1 || spelling.empty())
        {
            return false;
        }

        const auto& [read, error]{commands_of(commands)};

        const auto called{
                std::ranges::count_if(read, [](const Command& command) { return !command.argument.empty(); })};

        return read.empty() || (!acted && read.size() <= 2 && called <= 1);
    }()};

    if (aliased)
    {
        ++aliases_[alternatives.front().spelling];
    }

    // ANTLR gives a rule a token type of its own where a lexer grammar's tokens block names it or the rule spells a
    // literal in the shape above, and no type at all to another rule whose commands set the type: its own type is
    // what a type command setting zero, ANTLR's value for none, leaves the token with.
    const auto typed{aliased || tokens_.contains(name)};

    // The whole rule is what a reference to it expands to, and it matches the empty string when one of its
    // alternatives does, which is what a closure over a reference to it needs; an empty alternative makes it
    // optional.
    std::string whole;

    auto filled{0UZ};

    auto optional{false};

    auto nullable{never_empty()};

    for (const auto& [pattern, expression, commands, clause, spelling, acted, empty, alternative_nullable] :
         alternatives)
    {
        nullable = either_empty(std::move(nullable), alternative_nullable);

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

    // ANTLR's error 51: a rule name is the grammar's, whatever mode either definition stands in.
    if (spec.definitions.contains(name))
    {
        throw Spec_error{
                std::format("rule {} redefinition; previous at line {}", name, definition_lines_.at(name)),
                line_of(opened)};
    }

    spec.definitions.insert_or_assign(name, whole);

    definition_lines_.insert_or_assign(name, line_of(opened));

    nullability_.insert_or_assign(name, std::move(nullable));

    if (fragment)
    {
        return;
    }

    // A command ends the rule's single outermost alternative, so a rule of several alternatives carries none: one
    // rule is one token however many alternatives it has.
    const auto commanded{std::ranges::any_of(
            alternatives, [](const Alternative& alternative) { return !alternative.commands.empty(); })};

    if (alternatives.size() > 1 && commanded)
    {
        at_ = opened;

        fail(std::format(
                "a command must be the last element of the single outermost alternative of a lexer rule, so the "
                "commands on the alternatives of {} are no ANTLR grammar",
                name));
    }

    const auto line{line_of(opened)};

    // `skip` and `type(X)` both set the token's type and a channel sets a field of its own, the rightmost command
    // for a field winning in either case: `-> channel(HIDDEN), type(B)` leaves a token named B on the hidden
    // channel, which a parser never sees, and `-> skip, type(B)` leaves B in the stream, the type overriding the
    // skip, which is what ANTLR's warning 179 on the pair says of it. A channel command naming the default
    // channel, by name or as zero in any spelling, leaves the token where a parser reads it.
    // Whether a command sets the type: ANTLR gives such a rule no token type of its own, unless it spells one
    // literal, so another rule's `type` cannot name it.
    auto retyped{false};

    const auto token_of{
            [&name, line, typed, &retyped, this](const Alternative& alternative) -> std::optional<std::string> {
                auto channelled{false};

                std::optional<std::string> type{name};

                // The line of the command that set the type last, the rule's own while none has: where a refusal of the
                // type the commands set is reported.
                auto typed_at{line};

                const auto [commands, error]{commands_of(alternative.commands)};

                // ANTLR's parser rejects a clause it cannot read before any command is looked at, its error 50 at the
                // byte, in words that vary with what it had taken before, `came as a complete surprise` after a bare
                // command and `expecting SEMI` after an argument; the refusal names the cause in its own, at that
                // byte's line.
                if (error)
                {
                    throw Spec_error{error->message, line_of(alternative.clause + error->offset)};
                }

                for (const auto& [command, argument, offset] : commands)
                {
                    const auto line{line_of(alternative.clause + offset)};

                    // ANTLR knows seven commands and no other, three taking no argument and four taking one, and
                    // rejects a grammar naming anything else as its errors 149, 150 and 151, in its own words. The
                    // seven with their first letter capitalised, `Skip`, name the code templates of ANTLR's targets,
                    // `LexerSkipCommand`, which ANTLR expands into an action the generated lexer runs and its own
                    // interpreter leaves out; they pass its argument checks under their own spelling.
                    const auto templated{
                            command == "Skip" || command == "More" || command == "PopMode" || command == "Type" ||
                            command == "Channel" || command == "Mode" || command == "PushMode"};

                    auto lowered{command};

                    if (templated)
                    {
                        lowered.front() = static_cast<char>(lowered.front() | 0x20);
                    }

                    const auto plain{lowered == "skip" || lowered == "more" || lowered == "popMode"};

                    const auto called{
                            lowered == "type" || lowered == "channel" || lowered == "mode" || lowered == "pushMode"};

                    if (!plain && !called)
                    {
                        throw Spec_error{
                                "lexer command " + command +
                                        " does not exist or is not supported by the current target",
                                line};
                    }

                    if (called && argument.empty())
                    {
                        throw Spec_error{"missing argument for lexer command " + command, line};
                    }

                    if (plain && !argument.empty())
                    {
                        throw Spec_error{"lexer command " + command + " does not take any arguments", line};
                    }

                    if (templated)
                    {
                        throw Spec_error{
                                "lexer command " + command +
                                        " names a code template of ANTLR's target, expanded into an action the "
                                        "generated "
                                        "lexer runs and ANTLR's own interpreter leaves out, so what the token stream "
                                        "holds "
                                        "is the target's to say",
                                line};
                    }

                    if (command == "skip")
                    {
                        type = std::nullopt;

                        typed_at = line;
                    }
                    else if (command == "type")
                    {
                        // A number is the type ANTLR numbers a token by, read as Integer.parseInt reads it, so `0` and
                        // `00` are zero, ANTLR's value for no type set, and the lexer emits the rule's own type in its
                        // place: its name where ANTLR gives the rule one and zero otherwise, a token no rule names,
                        // kept as `0`.
                        const auto zero{argument.find_first_not_of('0') == std::string::npos};

                        type = zero ? (typed ? name : "0") : argument;

                        typed_at = line;

                        retyped = true;

                        // EOF is a token ANTLR always has; whether a rule may end on it is settled below.
                        if (!zero && argument != "EOF" && argument.find_first_not_of("0123456789") != std::string::npos)
                        {
                            typed_names_.emplace_back(argument, line);
                        }
                    }
                    else if (command == "channel")
                    {
                        channelled = hidden(argument, channels_, line);
                    }
                    else if (
                            (command == "mode" || command == "pushMode") &&
                            argument.find_first_not_of("0123456789") != std::string::npos)
                    {
                        mode_names_.emplace_back(argument, line);
                    }
                    else if (command == "more")
                    {
                        throw Spec_error{
                                "'-> more' joins the match onto the next token's, which the byte reading cannot "
                                "express",
                                line};
                    }
                }

                // ANTLR's `EOF` is the type minus one, the token that ends the stream, so a rule whose type it is ends
                // the stream where it matches, on any channel: nothing after the match is a token, and the token
                // language has no place for a match that ends the stream.
                if (type == "EOF")
                {
                    throw Spec_error{
                            "'-> type(EOF)' ends the token stream where the rule matches, so nothing after the match "
                            "is a "
                            "token, which the byte reading cannot express",
                            typed_at};
                }

                return channelled ? std::nullopt : type;
            }};

    std::string pattern;

    for (const auto& [text, expression, commands, clause, spelling, acted, empty, alternative_nullable] : alternatives)
    {
        pattern += (pattern.empty() ? "" : " | ") + text;
    }

    const auto& commands{alternatives.front().commands};

    spec.rules.push_back(
            {.pattern = std::move(pattern),
             .expression = whole,
             .conditions = mode.empty() ? std::vector<std::string>{} : std::vector{mode},
             .action = commands.empty() ? std::string{} : "-> " + without_trailing_blanks(commands),
             .token = token_of(alternatives.front()),
             .priority = std::nullopt,
             .line = line});

    if (aliased || !retyped)
    {
        rule_names_.insert(name);
    }

    if (!sections_.empty())
    {
        ++sections_.back().rules;
    }
}

std::vector<Alternative> Grammar::alternatives(const bool case_insensitive)
{
    std::vector<Alternative> read;

    // What the alternatives read so far can begin with, unknown once one of them cannot say.
    std::optional<Beginning> earlier{Beginning{}};

    // Where a non-greedy loop stood in some alternative, and whether some alternative can match the empty string,
    // known or through a reference.
    std::optional<std::size_t> lazy_at;

    std::vector<Nullable_t> formulas;

    for (;;)
    {
        skip_blanks();

        const auto opened{at_};

        auto [expression, first, characters, lazy, empty, nullable, spelling, acted]{sequence(case_insensitive, true)};

        // ANTLR's lexer follows the rule's alternatives at once, in their order, and the first path to reach the
        // rule's end stops every later path that has passed a non-greedy decision: `'ab' | 'a' .*? 'c'` on "abc"
        // ends at the second character, where `'a' .*? 'c' | 'ab'` takes all three. An earlier alternative that no
        // character begins together with this one is dead before the loop's decision is reached, so the loop is
        // read where every earlier alternative is such.
        if (lazy && !(earlier && first && !earlier->overlaps(*first)))
        {
            throw Spec_error{
                    "a non-greedy loop is read only where no earlier alternative of the rule can begin with the same "
                    "character, since ANTLR takes the alternatives in order and an earlier one reaching the rule's "
                    "end stops the loop, which the byte reading cannot express",
                    line_of(opened)};
        }

        lazy_at = lazy ? std::optional{opened} : lazy_at;

        formulas.push_back(nullable);

        if (earlier && first)
        {
            earlier->join(*first);
        }
        else
        {
            earlier = std::nullopt;
        }

        auto pattern{without_trailing_blanks(std::string{text_.substr(opened, at_ - opened)})};

        std::string commands;

        auto clause{opened};

        if (at("->"))
        {
            at_ += 2;

            skip_blanks();

            const auto begin{at_};

            clause = begin;

            // A comment between the commands is the grammar's and may hold a `;` or a `|` of its own, so the
            // clause ends at the first of those the reading stands on rather than at the first in the text; the
            // clause is kept through that byte, exclusive, so that a refusal of parens it ends inside of names the
            // line of the `;`, as ANTLR names it.
            for (skip_blanks(); peek() && *peek() != ';' && *peek() != '|'; skip_blanks())
            {
                ++at_;
            }

            commands = text_.substr(begin, at_ - begin);

            // ANTLR's parser takes a command after the arrow, so `-> ;` is its error 50, a syntax error at the `;`.
            if (commands.empty())
            {
                fail(std::format("syntax error: '->' has no command after it{}", rejected));
            }
        }

        read.push_back(
                {.pattern = std::move(pattern),
                 .expression = std::move(expression),
                 .commands = std::move(commands),
                 .clause = clause,
                 .spelling = std::move(spelling),
                 .acted = acted,
                 .empty = empty,
                 .nullable = std::move(nullable)});

        if (peek() == '|')
        {
            ++at_;

            continue;
        }

        skip_blanks();

        expect(';', "';' to end the rule");

        // ANTLR's empty match reaches the rule's end at the loop's decision, in whichever alternative it stands, and
        // stops the loop there: `X : | .*? 'a' ;` on "aa" emits no X at all and `X : .*? 'a' | ;` two of one
        // character, where the greedy reading spans both, so the loop is read only in a rule no alternative of which
        // can match the empty string.
        // Whether an alternative matches the empty string is a formula over the rules it reaches, which the least
        // fixed point over the whole grammar answers, so the loop's own decision waits for it below: a reference to
        // a rule that matches no empty string, `FIELD : \'"\' .*? \'"\' | WORD ;`, leaves a formula that is not
        // empty and an alternative that is not nullable, which ANTLR reads and emits.
        if (lazy_at)
        {
            for (auto& formula : formulas)
            {
                lazy_empty_.push_back({.rule = rule_, .line = line_of(*lazy_at), .body = std::move(formula)});
            }
        }

        return read;
    }
}

Sequence Grammar::sequence(const bool case_insensitive, const bool outermost)
{
    std::vector<Element> elements;

    for (skip_blanks(); peek() && *peek() != '|' && *peek() != ')' && *peek() != ';' && !at("->"); skip_blanks())
    {
        elements.push_back(element(case_insensitive));
    }

    // What the sequence can begin with: every element up to and including the first that cannot match the empty
    // string, an element under `?` or `*` and a group with an empty alternative among those that can; one whose
    // emptiness waits on a rule leaves the answer unknown, as a reference does.
    std::optional<Beginning> first{Beginning{}};

    for (const auto& element : elements)
    {
        if (element.expression.empty())
        {
            continue;
        }

        const auto skippable{element.suffix == '?' || element.suffix == '*' || matches_empty(element.nullable, {})};

        if (!element.first || (!skippable && !element.nullable.empty()))
        {
            first = std::nullopt;

            break;
        }

        first->join(*element.first);

        if (!skippable)
        {
            break;
        }
    }

    // How many characters a match has, when every element's matches have one length and no suffix varies it.
    const auto measured{[&elements](const std::size_t count) -> std::optional<std::size_t> {
        std::size_t total{0};

        for (const auto& element : elements | std::views::take(count))
        {
            if (!element.characters || element.suffix != 0)
            {
                return std::nullopt;
            }

            total += *element.characters;
        }

        return total;
    }};

    std::string out;

    for (std::size_t index{0}; index < elements.size(); ++index)
    {
        const auto& [expression, literal, spelling, alphabet, begins, nullable, one_length, characters, suffix, lazy]{
                elements[index]};

        if (!lazy)
        {
            out += suffix == 0 || atomic(expression) ? expression + (suffix == 0 ? "" : std::string{suffix}) :
                                                       std::format("({}){}", expression, suffix);

            continue;
        }

        // A non-greedy loop stops at the fewest characters that still let the rest of the rule match, so what it
        // stops at is the whole rest of the rule, not the element after it: the rest is a regular rewrite only where
        // it spells one ASCII string, and `.*? 'a' 'b'` stops at `ab`, not at `a`. Inside a group the rest of the
        // rule reaches past what this sequence holds, `('a' .*? 'b') 'c'` stopping at `bc` and not at `b`, so the
        // loop is read in an outermost alternative alone.
        lazy_ = true;

        if (!outermost)
        {
            fail("a non-greedy loop is read only in an outermost alternative of a rule, since ANTLR stops it where "
                 "the rest of the whole rule matches, which reaches past the group it stands in");
        }

        const auto next{rest_spelling(elements, index + 1)};

        if (!next)
        {
            fail("a non-greedy loop is read only where the rest of the rule spells one ASCII string, which is where "
                 "ANTLR stops the loop; the fewest characters that let the rest match are no regular rewrite here");
        }

        // ANTLR's lexer follows every path through the rule at once, in the order the alternatives before the loop
        // give them, and the first path to reach the rule's end stops every later path that has passed the loop's
        // decision. Where the elements before the loop match one length in characters, every path reaches the
        // decision at the same character and the order decides nothing; where they do not, `('a'|'aa') .*? 'a'` on
        // "aaa", the path through 'a' ends at the second character and stops the path through 'aa' there, while
        // `('aa'|'a') .*? 'a'` takes all three, a difference the greedy rewrite of the group cannot keep.
        if (!measured(index))
        {
            fail("a non-greedy loop is read only after elements whose every match has one length in characters, "
                 "since ANTLR takes the paths through the elements before it in order and the first to reach the "
                 "rule's end stops the loop on every later one, which the byte reading cannot express where the "
                 "paths reach the loop at different characters");
        }

        const auto opener{static_cast<unsigned char>(next->front())};

        if (suffix == '?')
        {
            // The bypass of a non-greedy option comes first among the paths, and the body's alternatives after it
            // in order; so the body may not begin the rest, which the bypass would end the rule with at once, and
            // its alternatives must reach the rest at one character together, `('x'|'xa')?? 'a'` on "xaa" stopping
            // after "xa" where `('xa'|'x')?? 'a'` takes all three.
            if (!begins || begins->ascii.test(opener))
            {
                fail("a non-greedy option before a string it could begin is not modelled");
            }

            if (!elements[index].characters)
            {
                fail("a non-greedy option is read over a body whose every match has one length in characters, since "
                     "ANTLR takes the body's alternatives in order and the first to reach the rule's end stops the "
                     "others, which the byte reading cannot express where they reach the rest at different "
                     "characters");
            }

            out += atomic(expression) ? expression + "?" : std::format("({})?", expression);

            continue;
        }

        // Over one set, the dot or one character the loop is the strings that stop at the rest. Over a body of
        // one length, a literal of several characters folded or not, the iterations are aligned to that length, and
        // where the rest cannot begin with the body's first byte no repetition of it can hold the rest, so the
        // greedy loop is the same language. Over a body of several lengths it is neither: ANTLR stops the loop at
        // the fewest characters that let the rest match and takes a group's alternatives in order, so
        // `('x'|'xa')*? 'a'` stops on "xaa" after "xa" while `('xa'|'x')*? 'a'` takes all three, a difference no
        // greedy rewrite over the group can keep.
        if (alphabet)
        {
            // A `+?` loop reads its first character whatever follows, since it cannot stop before it has one, and
            // the rest of its body is the `*?` body from there on.
            const auto unit{atomic(expression) ? expression : "(" + expression + ")"};

            const auto body{avoiding(*next, *alphabet)};

            out += suffix == '+' ? unit + body : body;
        }
        else if (one_length && begins && !begins->ascii.test(opener))
        {
            out += atomic(expression) ? expression + suffix : std::format("({}){}", expression, suffix);
        }
        else
        {
            fail("a non-greedy loop is read over a set, a dot, one character, or a literal of one length whose first "
                 "byte the rest cannot begin with; over any other body ANTLR stops it at the fewest characters that "
                 "let the rest match, a group's alternatives taken in order, which the byte reading cannot express");
        }
    }

    // The sequence matches the empty string when every element can, and an element under `?` or `*` always can.
    auto nullable{always_empty()};

    for (const auto& element : elements)
    {
        nullable = both_empty(
                nullable, element.suffix == '?' || element.suffix == '*' ? always_empty() : element.nullable);
    }

    // ANTLR's patterns for a rule spelling a parser literal match the literal alone, unsuffixed, or the literal and
    // one action after it; an inert action is the only kind read this far.
    const auto acted{elements.size() == 2 && elements.back().expression.empty()};

    const auto spelled{(elements.size() == 1 || acted) && elements.front().suffix == 0};

    return {.expression = std::move(out),
            .first = first,
            .characters = measured(elements.size()),
            .lazy = std::ranges::any_of(elements, &Element::lazy),
            .empty = elements.empty(),
            .nullable = std::move(nullable),
            .spelling = spelled ? elements.front().spelling : std::string{},
            .acted = acted};
}

Element Grammar::element(const bool case_insensitive)
{
    const auto opened{at_};

    Element element{
            .expression = {},
            .literal = std::nullopt,
            .spelling = {},
            .alphabet = std::nullopt,
            .first = std::nullopt,
            .nullable = never_empty(),
            .one_length = false,
            .characters = std::nullopt,
            .suffix = 0,
            .lazy = false};

    const auto byte{*peek()};

    // Whether the atom is one ANTLR's parser takes element options on, `'x'<a=b>`: a literal that is no range, a
    // reference or the dot (ANTLRParser.g's terminal and wildcard), and no set, range, negation or group.
    auto optionable{false};

    if (byte == '{')
    {
        // An action, which ANTLR runs at this point of the match and whose code can make the rule produce another
        // token than its own, `{more();}` joining the match onto the next token's and `{setType(X);}` renaming it;
        // only a body of blanks and comments runs nothing and is inert. A predicate conditions the match itself.
        const auto body{at_ + 1};

        skip_block();

        const auto closed{at_ - 1};

        skip_blanks();

        if (peek() == '?')
        {
            at_ = opened;

            fail("a semantic predicate conditions the match on code, which a token language cannot say");
        }

        if (!inert(text_, body, closed))
        {
            at_ = opened;

            fail("an action inside a rule runs code that can change the token the rule produces, more() and "
                 "setType() among them, which the byte reading cannot model; only blanks and comments are inert");
        }

        // An inert action matches nothing at all, so it never stands in the way of an empty match.
        element.nullable = always_empty();

        element.characters = 0;

        return element;
    }

    if (byte == '\'')
    {
        ++at_;

        auto [bytes, single]{literal()};

        const std::string spelling{text_.substr(opened, at_ - opened)};

        skip_blanks();

        if (at(".."))
        {
            // A range of characters, 'a'..'z': each end one character as ANTLR reads one, its error 144 otherwise,
            // and the end no lower than the start, its error 174 otherwise.
            at_ += 2;

            skip_blanks();

            const auto second{at_};

            expect('\'', "a quote to open the range's end");

            const auto end{literal()};

            const std::string end_spelling{text_.substr(second, at_ - second)};

            if (!single || !end.single)
            {
                at_ = opened;

                fail(multi_character(single ? end_spelling : spelling));
            }

            const auto low{decoded(bytes)};

            const auto high{decoded(end.bytes)};

            if (*high < *low)
            {
                at_ = opened;

                fail(empty_range(spelling + ".." + end_spelling));
            }

            if (case_insensitive && *high >= 0x80)
            {
                at_ = opened;

                fail(std::string{unfoldable});
            }

            auto alphabet{spanning(*low, *high, case_insensitive)};

            element.expression = step(alphabet);

            element.one_length = alphabet.beyond.empty();

            element.alphabet = std::move(alphabet);
        }
        else
        {
            optionable = true;

            if (bytes.empty())
            {
                at_ = opened;

                fail("an empty literal matches nothing");
            }

            const auto beyond{
                    std::ranges::any_of(bytes, [](const char one) { return static_cast<unsigned char>(one) >= 0x80; })};

            if (case_insensitive && beyond)
            {
                at_ = opened;

                fail(std::string{unfoldable});
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

            element.spelling = spelling;

            // Every match of a literal is the literal, folded or not, so they all have its length, in bytes and in
            // characters, of which every byte but a continuation byte begins one.
            element.one_length = true;

            element.characters = static_cast<std::size_t>(std::ranges::count_if(
                    bytes, [](const char one) { return (static_cast<unsigned char>(one) & 0xC0U) != 0x80U; }));

            // A literal's characters fold one by one, each as a range of itself.
            element.first = Beginning{};

            if (const auto lead{static_cast<unsigned char>(bytes.front())}; lead < 0x80)
            {
                element.first->ascii = spanning(lead, lead, case_insensitive).ascii;
            }
            else
            {
                element.first->beyond = true;
            }

            if (const auto scalar{decoded(bytes)})
            {
                element.alphabet = spanning(*scalar, *scalar, case_insensitive);
            }
        }
    }
    else if (byte == '[')
    {
        ++at_;

        auto alphabet{set(case_insensitive)};

        // A set of nothing but surrogates is ANTLR's transition no UTF-8 input decodes a code point for, which its
        // lexer never takes; the byte reading has no set that never matches, so it refuses.
        if (alphabet.ascii.none() && alphabet.beyond.empty())
        {
            const std::string spelling{text_.substr(opened, at_ - opened)};

            at_ = opened;

            fail("the set " + spelling +
                 " holds nothing but surrogates, which no UTF-8 input decodes to, so ANTLR's lexer never matches it");
        }

        if (case_insensitive && !alphabet.beyond.empty())
        {
            at_ = opened;

            fail(std::string{unfoldable});
        }

        element.expression = step(alphabet);

        element.one_length = alphabet.beyond.empty();

        element.alphabet = std::move(alphabet);
    }
    else if (byte == '~')
    {
        ++at_;

        skip_blanks();

        const auto negated{negatable(case_insensitive)};

        if (case_insensitive && !negated.beyond.empty())
        {
            at_ = opened;

            fail(std::string{unfoldable});
        }

        element.alphabet = complement(negated);

        element.expression = step(*element.alphabet);

        element.one_length = element.alphabet->beyond.empty();
    }
    else if (byte == '.')
    {
        ++at_;

        Alphabet all{.ascii = {}, .beyond = {{.first = 0x80, .last = last_scalar}}};

        all.ascii.set();

        element.expression = step(all);

        element.alphabet = std::move(all);

        element.one_length = false;

        optionable = true;
    }
    else if (byte == '(')
    {
        ++at_;

        std::string inner;

        auto optional{false};

        element.first = Beginning{};

        // The alternatives' lengths agree until one differs or is unknown, the first alternative setting the mark.
        auto agreed{true};

        for (auto alternatives{0UZ};; ++alternatives)
        {
            const auto [expression, first, characters, lazy, empty, nullable, spelling, acted]{
                    sequence(case_insensitive, false)};

            element.nullable = either_empty(std::move(element.nullable), nullable);

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
                element.first->join(*first);
            }
            else
            {
                element.first = std::nullopt;
            }

            if (alternatives == 0)
            {
                element.characters = characters;
            }

            agreed = agreed && characters && element.characters == characters;

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

        if (!agreed)
        {
            element.characters = std::nullopt;
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

        element.nullable = {{name}};

        optionable = true;
    }

    // Element options after an atom that takes them are metadata on it, `'x'<a=b>`, and set nothing the reading
    // needs; after any other atom the `<` is the byte ANTLR's parser rejects, refused below as one no element begins.
    skip_blanks();

    if (optionable && peek() == '<')
    {
        ++at_;

        element_options();

        // A literal they follow is no alias spelling: ANTLR's alias pattern matches the bare literal alone, so
        // `A : 'a'<> -> skip ;` leaves the parser's 'a' an implicit token ahead of A, which antlr 4.13.2 emits.
        element.spelling.clear();
    }

    // A set, a range, the dot, a negation and a one-character literal all match one character.
    if (element.alphabet)
    {
        element.first = Beginning{.ascii = element.alphabet->ascii, .beyond = !element.alphabet->beyond.empty()};

        element.characters = 1;
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

    // ANTLR rejects a closure whose body can match the empty string, `('b' | )*` and a star over a nullable rule
    // among them, as its error 153. A body that reaches no rule answers here; one that reaches a rule waits for
    // read(), since the rule it reaches may be written further down.
    if (element.suffix == '*' || element.suffix == '+')
    {
        if (matches_empty(element.nullable, {}))
        {
            at_ = opened;

            fail(std::format(
                    "the rule {} contains a closure with at least one alternative that can match the empty string, "
                    "which ANTLR rejects",
                    rule_));
        }

        closures_.push_back({.rule = rule_, .line = line_of(opened), .body = element.nullable});
    }

    return element;
}

Literal Grammar::literal()
{
    const auto quote{at_ - 1};

    std::string bytes;

    // ANTLR reads the literal into a UTF-16 string and walks it by code point (CharSupport and
    // LexerATNFactory.stringLiteral), so a high surrogate escape and a low one after it, `'\uD83D\uDE00'`, are the
    // one character the pair encodes, and a surrogate on its own is a transition on a code point no UTF-8 input
    // decodes to, which its lexer never takes: the byte reading has no literal that never matches, so it refuses.
    std::optional<char32_t> high;

    auto pieces{0UZ};

    auto wide{false};

    const auto lone{[this](const char32_t scalar) {
        fail(std::format(
                R"(the literal holds a lone surrogate, \u{:04X}, which no UTF-8 input decodes to, so ANTLR's lexer )"
                R"(never matches it; a high surrogate and a low one after it are the character the pair encodes)",
                static_cast<std::uint32_t>(scalar)));
    }};

    for (;;)
    {
        const auto written{peek() != '\\'};

        const auto braced{at("\\u{")};

        const auto scalar{character('\'')};

        if (!scalar)
        {
            break;
        }

        // ANTLR's lexer counts a braced escape's digits from the literal's opening quote rather than from the escape
        // (ANTLRLexer.g's UNICODE_EXTENDED_ESC), so one whose closing brace stands twelve or more UTF-16 units past
        // the quote, `'abcdef\u{41}'` and a second braced escape in one literal, is its error 156, in its words: the
        // literal from its quote through the closing brace.
        if (braced && units(text_, quote, at_ - 1) >= 12)
        {
            const std::string sequence{text_.substr(quote, at_ - quote)};

            at_ = quote;

            fail("invalid escape sequence " + sequence);
        }

        ++pieces;

        wide = wide || (written && *scalar > 0xFFFF);

        if (high && *scalar >= first_low_surrogate && *scalar <= last_surrogate)
        {
            bytes += encoded(0x10000 + ((*high - first_surrogate) << 10U) + (*scalar - first_low_surrogate));

            high = std::nullopt;

            continue;
        }

        if (high)
        {
            lone(*high);
        }

        if (*scalar >= first_surrogate && *scalar <= last_high_surrogate)
        {
            high = *scalar;

            continue;
        }

        if (surrogate(*scalar))
        {
            lone(*scalar);
        }

        bytes += encoded(*scalar);
    }

    if (high)
    {
        lone(*high);
    }

    return {.bytes = std::move(bytes), .single = pieces == 1 && !wide};
}

Alphabet Grammar::set(const bool case_insensitive)
{
    const auto opened{at_ - 1};

    Alphabet alphabet;

    // Each member and each span folds on its own, as ANTLR folds them: `[xA-t9]` gains `X` and nothing of `A-t`.
    // A member or a span of nothing but surrogates is ANTLR's member no UTF-8 input decodes to, which its lexer never
    // matches, and is left out; a span reaching past them keeps what lies on either side. A member waits until the
    // next one shows whether a '-' spans them; an escaped `\-` is a member, not a span.
    const auto take{[&alphabet, case_insensitive](const char32_t first, const char32_t last) {
        if (!(surrogate(first) && surrogate(last)))
        {
            admit(alphabet, first, last, case_insensitive);
        }
    }};

    std::optional<char32_t> pending;

    auto written{false};

    for (;;)
    {
        const auto escaped{peek() == '\\'};

        const auto scalar{character(']')};

        if (!scalar)
        {
            break;
        }

        written = true;

        if (pending && *scalar == '-' && !escaped && peek() != ']')
        {
            const auto last{character(']')};

            if (!last || *last < *pending)
            {
                at_ = opened;

                fail("a set range needs an end no lower than its start");
            }

            take(*pending, *last);

            pending = std::nullopt;

            continue;
        }

        if (pending)
        {
            take(*pending, *pending);
        }

        pending = *scalar;
    }

    if (pending)
    {
        take(*pending, *pending);
    }

    if (!written)
    {
        at_ = opened;

        fail(empty_range("[]"));
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

        // One character as ANTLR reads one, its error 144 otherwise.
        const auto [bytes, single]{literal()};

        const std::string spelling{text_.substr(opened, at_ - opened)};

        if (!single)
        {
            at_ = opened;

            fail(multi_character(spelling));
        }

        const auto scalar{*decoded(bytes)};

        skip_blanks();

        // A range inside the negation, ~('0'..'9' | '^'), as Clojure's grammar writes it; a literal that is no
        // range may carry element options, `~'x'<a=b>`, which set nothing here.
        if (!at(".."))
        {
            if (peek() == '<')
            {
                ++at_;

                element_options();
            }

            return spanning(scalar, scalar, case_insensitive);
        }

        at_ += 2;

        skip_blanks();

        const auto second{at_};

        expect('\'', "a quote to open the range's end");

        const auto end{literal()};

        const std::string end_spelling{text_.substr(second, at_ - second)};

        if (!end.single)
        {
            at_ = opened;

            fail(multi_character(end_spelling));
        }

        const auto high{*decoded(end.bytes)};

        if (high < scalar)
        {
            at_ = opened;

            fail(empty_range(spelling + ".." + end_spelling));
        }

        return spanning(scalar, high, case_insensitive);
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

    // ANTLR's lexer takes no raw line break inside a literal or a set (ANTLRLexer.g's STRING_LITERAL and
    // LEXER_CHAR_SET): a literal holding one ends unterminated at the break, its error 152, and a set holding one
    // is its error 50 at the break, each in its words at the line the literal or set opened on.
    if (byte == '\n' || byte == '\r')
    {
        --at_;

        fail(closing == '\'' ? std::string{"unterminated string literal"} :
                               std::format(
                                       "syntax error: mismatched character '{}' expecting ']'",
                                       byte == '\n' ? R"(\n)" : R"(\r)"));
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

    // The escapes ANTLR takes: in a literal, its lexer's ESC_SEQ, \b \t \n \f \r \' \\ and the two Unicode forms;
    // in a set, EscapeSequenceParsing's, \b \t \n \f \r \\ \] \- \p{...} \P{...} and the two Unicode forms. Any
    // other is its error 156, in its words: `'\q'`, `'\]'` and `[\']` among them.
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
    case '\\':
        return '\\';
    case '\'':
        if (closing == '\'')
        {
            return '\'';
        }

        break;
    case ']':
    case '-':
        if (closing == ']')
        {
            return static_cast<unsigned char>(escaped);
        }

        break;
    case 'p':
    case 'P':
        if (closing == ']')
        {
            --at_;

            fail("a Unicode property class needs tables the byte reading has not got");
        }

        break;
    default:
        break;
    }

    if (escaped != 'u')
    {
        // The sequence as written, the escaped character whole where it is more than one byte.
        const auto begin{at_ - 2};

        auto end{at_};

        while (end < end_ && (static_cast<unsigned char>(text_[end]) & 0xC0U) == 0x80U)
        {
            ++end;
        }

        const std::string sequence{text_.substr(begin, end - begin)};

        at_ = begin;

        fail("invalid escape sequence " + sequence);
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

    if (!peek() || !is_letter(static_cast<unsigned char>(*peek())))
    {
        return name;
    }

    while (peek() && is_name_byte(*peek()))
    {
        name.push_back(next("a name"));
    }

    return name;
}

} // namespace

std::vector<Lexer_spec> read_antlr(const std::string_view source)
{
    auto spec{Grammar{source}.read()};

    // A grammar whose lexer rules are all fragments, or a parser grammar, emits no token, so it declares no scanner.
    if (spec.rules.empty())
    {
        return {};
    }

    return {std::move(spec)};
}

} // namespace munch::tools::audit
