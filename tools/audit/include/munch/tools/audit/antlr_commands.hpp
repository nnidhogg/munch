#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_COMMANDS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_COMMANDS_HPP

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/antlr_rule.hpp"
#include "munch/tools/audit/antlr_tables.hpp"
#include "munch/tools/audit/cursor.hpp"

/**
 * @brief ANTLR's action model in a lexer rule: the commands of a `->` clause read as ANTLR's own lexer and parser read
 *        them, Commands and commands_of(), with the first byte its parser rejects, Syntax_error, a channel's argument
 *        resolved as ANTLR resolves it, the names ANTLR reserves for its commands and channels, reserved_names, the
 *        commands applied to the token a rule matches, apply_commands().
 *
 * ANTLR reads a clause with the lexer it reads the grammar with, so a comment inside one is no part of any command, and
 * its errors are refused here in its own words, by number, at the line ANTLR names.
 */
namespace munch::tools::audit
{
/**
 * @brief One command after a rule's `->`: its name and the argument its parens hold.
 */
struct Command
{
    /**
     * @brief The name, `skip`, `more`, `type`, `channel`, `mode`, `pushMode` or `popMode`.
     */
    std::string name{};

    /**
     * @brief What the parens hold, a token or a mode name or a number, empty when the command takes none.
     */
    std::string argument{};

    /**
     * @brief The offset of the name within the clause, which names the command's own line in a refusal.
     */
    std::size_t offset{};
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
    std::size_t offset{};

    /**
     * @brief The refusal, which names the cause where ANTLR's own words vary with what its parser had taken before.
     */
    std::string message{};
};

/**
 * @brief The commands of a `->` clause, and the first byte of it ANTLR's parser rejects.
 */
struct Commands
{
    /**
     * @brief The commands in order, through the first syntax error where there is one.
     */
    std::vector<Command> commands{};

    /**
     * @brief The first syntax error of the clause, nothing where ANTLR's parser takes the whole of it.
     */
    std::optional<Syntax_error> error{};
};

/**
 * @brief A lexer rule as its commands see it: its name, its line and whether ANTLR gives it a token type of its own.
 */
struct Commanded_rule
{
    /**
     * @brief The rule's name, the token's type while no command sets another.
     */
    std::string name{};

    /**
     * @brief The rule's line, where a refusal of the type is reported while no command has set one.
     */
    std::size_t line{};

    /**
     * @brief Whether ANTLR gives the rule a token type of its own, a lexer grammar's tokens block naming it or the rule
     *        spelling a parser literal, which a type command setting zero leaves the token with.
     */
    bool typed{};
};

/**
 * @brief What a rule's commands make of the token it matches.
 */
struct Commanded_token
{
    /**
     * @brief The token the rule returns, nothing where a skip or a channel a parser does not read leaves it out.
     */
    std::optional<std::string> token{};

    /**
     * @brief Whether a type command sets the type: ANTLR gives such a rule no token type of its own, unless it spells
     *        one literal, so another rule's `type` cannot name it.
     */
    bool retyped{};
};

/**
 * @brief The names ANTLR reserves for its lexer commands and channels, which no rule, mode or channel may take.
 */
constexpr std::array<std::string_view, 8> reserved_names{
        "DEFAULT_MODE", "SKIP", "MORE", "EOF", "MAX_CHAR_VALUE", "MIN_CHAR_VALUE", "HIDDEN", "DEFAULT_TOKEN_CHANNEL"};

/**
 * @brief How a refusal of a `->` clause's syntax ends: ANTLR's parser rejects the byte named before any command is
 *        looked at, its error 50 while matching a lexer rule.
 */
constexpr std::string_view rejected{", which ANTLR's parser rejects while matching a lexer rule"};

/**
 * @brief Returns whether a text is a number as ANTLR's lexer reads one, decimal digits alone.
 * @param text The text.
 * @return True when it is one digit or more and nothing else.
 */
[[nodiscard]] bool is_number(std::string_view text);

/**
 * @brief Applies a rule's commands to the token it matches, as ANTLR applies them.
 *
 * `skip` and `type(X)` both set the token's type and a channel sets a field of its own, the rightmost command for a
 * field winning in either case: `-> channel(HIDDEN), type(B)` leaves a token named B on the hidden channel, which a
 * parser never sees, and `-> skip, type(B)` leaves B in the stream, the type overriding the skip, which is what ANTLR's
 * warning 179 on the pair says of it. A channel command naming the default channel, by name or as zero in any spelling,
 * leaves the token where a parser reads it.
 *
 * ANTLR's parser rejects a clause it cannot read before any command is looked at, its error 50 at the byte, in words
 * that vary with what it had taken before, `came as a complete surprise` after a bare command and `expecting SEMI`
 * after an argument; the refusal names the cause in its own, at that byte's line.
 * @param alternative The rule's one outermost alternative, whose `->` clause holds the commands.
 * @param rule The rule.
 * @param grammar The cursor over the grammar, which counts the lines a refusal names.
 * @param tables What the reading records about the whole grammar: the channels a channel command may name, and the
 *        names type and mode commands carry, resolved once the grammar is read.
 * @return The token, and whether a command set its type.
 * @throws Spec_error As ANTLR's parser rejects the clause, as its errors 149, 150 and 151 refuse a command, as its
 *         errors 172 and 177 refuse a channel's argument, for `more`, and for a type of EOF, which ends the token
 *         stream where the rule matches.
 */
[[nodiscard]] Commanded_token apply_commands(
        const Alternative& alternative, const Commanded_rule& rule, const Cursor& grammar, Grammar_tables& tables);

/**
 * @brief Returns the commands a `->` clause holds, as their names and arguments, blanks and comments dropped, and the
 *        first byte of the clause ANTLR's parser rejects, where there is one.
 *
 * ANTLR reads a clause with the lexer it reads the grammar with, so a comment inside one is no part of any command: a
 * clause of `skip` and a block comment after it is the skip command, and comparing the clause's text as written would
 * make it a command of another name and leave the token in the stream. The blanks and comments inside the parens go the
 * same way, so a type command with them around its token names that token. ANTLR's parser takes the clause as commands
 * parted by commas, a command a name alone or a name and parens holding one name or one number (ANTLRParser.g's
 * lexerCommand and lexerCommandExpr), a name beginning with a letter as its lexer reads one; so a comma no command name
 * follows, a command with no comma before it, parens holding nothing, more than one token or anything but a name or a
 * number, parens never closed, and any other byte where a name, an argument or a comma should stand are its error 50, a
 * syntax error at that byte, before any command is looked at. The first such byte is noted with the refusal's words.
 * @param text The clause as written, its `->` excluded, through the `;` or `|` that ends it, exclusive.
 * @return The commands in order, and the first syntax error.
 * @throws Spec_error If a block comment inside the clause never closes.
 */
[[nodiscard]] Commands commands_of(std::string_view text);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_COMMANDS_HPP
