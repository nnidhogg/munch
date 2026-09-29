#include "munch/tools/audit/antlr_commands.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "munch/tools/audit/antlr_cursor.hpp"
#include "munch/tools/audit/antlr_rule.hpp"
#include "munch/tools/audit/antlr_tables.hpp"
#include "munch/tools/audit/cursor.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements antlr_commands.hpp: the token's state as the commands apply, each command's application, a channel's
// resolution, the words of error 50 for empty parens and the reading of a command's argument are private to this unit.

/**
 * @brief The words of ANTLR's error 50 for a `)` closing parens that hold nothing, `skip()` and `type( )`, which its
 *        parser rejects at that `)`.
 */
constexpr std::string_view surprise{"syntax error: ')' came as a complete surprise to me while matching a lexer rule"};

/**
 * @brief The type a rule's commands give its token, as they are applied in order: the rightmost command for a field
 *        winning, a skip and a type setting the type alike and a channel a field of its own.
 */
struct Token_state
{
    /**
     * @brief The token's type, the rule's own name while no command has set one, nothing once a skip has.
     */
    std::optional<std::string> type;

    /**
     * @brief The line of the command that set the type last, the rule's own while none has: where a refusal of the type
     *        the commands set is reported.
     */
    std::size_t typed_at;

    /**
     * @brief Whether a channel command sends the token to a channel a parser does not read.
     */
    bool channelled;

    /**
     * @brief Whether a type command sets the type.
     */
    bool retyped;
};

/**
 * @brief Refuses a command ANTLR has not got, one given an argument it takes none of or none where it takes one, and
 *        one of the seven with its first letter capitalised.
 *
 * ANTLR knows seven commands and no other, three taking no argument and four taking one, and rejects a grammar naming
 * anything else as its errors 149, 150 and 151, in its own words. The seven with their first letter capitalised,
 * `Skip`, name the code templates of ANTLR's targets, `LexerSkipCommand`, which ANTLR expands into an action the
 * generated lexer runs and its own interpreter leaves out; they pass its argument checks under their own spelling.
 * @param command The command's name.
 * @param argument What its parens hold, empty when it has none.
 * @param line The command's line.
 * @throws Spec_error As ANTLR's errors 149, 150 and 151 refuse the command, or as a code template of ANTLR's target.
 */
void refuse_unknown(const std::string& command, const std::string& argument, const std::size_t line)
{
    const auto templated{
            command == "Skip" || command == "More" || command == "PopMode" || command == "Type" ||
            command == "Channel" || command == "Mode" || command == "PushMode"};

    auto lowered{command};

    if (templated)
    {
        lowered.front() = static_cast<char>(lowered.front() | 0x20);
    }

    const auto plain{lowered == "skip" || lowered == "more" || lowered == "popMode"};

    const auto called{lowered == "type" || lowered == "channel" || lowered == "mode" || lowered == "pushMode"};

    if (!plain && !called)
    {
        throw Spec_error{
                "lexer command " + command + " does not exist or is not supported by the current target", line};
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
        const auto message{
                "lexer command " + command +
                " names a code template of ANTLR's target, expanded into an action the generated lexer runs and "
                "ANTLR's own interpreter leaves out, so what the token stream holds is the target's to say"};

        throw Spec_error{message, line};
    }
}

/**
 * @brief Applies a `type` command.
 *
 * A number is the type ANTLR numbers a token by, read as Integer.parseInt reads it, so `0` and `00` are zero, ANTLR's
 * value for no type set, and the lexer emits the rule's own type in its place: its name where ANTLR gives the rule one
 * and zero otherwise, a token no rule names, kept as `0`.
 * @param argument The command's argument, a token's name or number.
 * @param line The command's line.
 * @param rule The rule the command stands in.
 * @param state The token's type so far, set on return.
 * @param tables What the reading records about the whole grammar, where a name the argument carries is resolved once
 *        the grammar is read.
 */
void apply_type(
        const std::string& argument, const std::size_t line, const Commanded_rule& rule, Token_state& state,
        Grammar_tables& tables)
{
    const auto zero{argument.find_first_not_of('0') == std::string::npos};

    state.type = zero ? (rule.typed ? rule.name : "0") : argument;

    state.typed_at = line;

    state.retyped = true;

    // EOF is a token ANTLR always has; whether a rule may end on it is settled once every command is applied.
    if (!zero && argument != "EOF" && !is_number(argument))
    {
        tables.typed_names.push_back({.text = argument, .line = line});
    }
}

/**
 * @brief Whether a channel command's argument names a channel other than the default one, the one a parser reads,
 *        resolved as ANTLR resolves it (LexerATNFactory.getChannelConstantValue): `HIDDEN` and `DEFAULT_TOKEN_CHANNEL`
 *        are its constants one and zero, another of its reserved names is its error 172, a name the grammar's
 *        `channels` block declares is a channel from two up, and anything else is read as a decimal number, `00` and
 *        `000` being zero and the default channel and every other number a channel of its own, a number beyond its int
 *        or a name nothing declares being its error 177, each in its words.
 * @param argument The argument as written.
 * @param declared The channels the grammar declares.
 * @param line The command's line, which a refusal names.
 * @return True when the token goes to a channel a parser does not read.
 * @throws Spec_error As ANTLR's errors 172 and 177 refuse the argument.
 */
[[nodiscard]] bool is_hidden(
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

    if (std::ranges::contains(reserved_names, argument))
    {
        throw Spec_error{"cannot use or declare channel with reserved name " + argument, line};
    }

    if (declared.contains(argument))
    {
        return true;
    }

    // Integer.parseInt: the digits, leading zeros dropped, up to 2147483647.
    constexpr std::string_view largest{"2147483647"};

    const auto digits{std::string_view{argument}.substr(std::min(argument.find_first_not_of('0'), argument.size()))};

    if (is_number(argument) &&
        (digits.size() < largest.size() || (digits.size() == largest.size() && digits <= largest)))
    {
        return !digits.empty();
    }

    throw Spec_error{argument + " is not a recognized channel name", line};
}

/**
 * @brief Applies one command ANTLR has got to the token's type: a skip, a type, a channel, a mode's name recorded for
 *        the checks after the last rule, and a `more` refused.
 * @param command The command.
 * @param line The command's line.
 * @param rule The rule the command stands in.
 * @param state The token's type so far, set on return.
 * @param tables What the reading records about the whole grammar.
 * @throws Spec_error As is_hidden() refuses a channel, or for `more`, which joins the match onto the next token's.
 */
void apply_command(
        const Command& command, const std::size_t line, const Commanded_rule& rule, Token_state& state,
        Grammar_tables& tables)
{
    const auto& [name, argument, offset]{command};

    if (name == "skip")
    {
        state.type = std::nullopt;

        state.typed_at = line;

        return;
    }

    if (name == "type")
    {
        apply_type(argument, line, rule, state, tables);

        return;
    }

    if (name == "channel")
    {
        state.channelled = is_hidden(argument, tables.channels, line);

        return;
    }

    if ((name == "mode" || name == "pushMode") && !is_number(argument))
    {
        tables.mode_names.push_back({.text = argument, .line = line});

        return;
    }

    if (name == "more")
    {
        throw Spec_error{
                "'-> more' joins the match onto the next token's, which the byte reading cannot express", line};
    }
}

/**
 * @brief The byte under the cursor, quoted for a refusal.
 * @param cursor The cursor, before the end of its span.
 * @return The byte between single quotes.
 */
[[nodiscard]] std::string quoted_byte(const Antlr_cursor& cursor)
{
    return std::format("'{}'", *cursor.peek());
}

/**
 * @brief Reads the argument a command's parens hold, a name or else a number of digits alone, as ANTLR's lexer reads
 *        them, through the `)` closing the parens.
 * @param cursor The cursor, just past the `(`, left past the `)` once the argument is read.
 * @param name The command's name, for refusals.
 * @return The argument, or the syntax error ANTLR's parser gives at the first byte it rejects.
 */
[[nodiscard]] std::expected<std::string, Syntax_error> command_argument(Antlr_cursor& cursor, const std::string& name)
{
    // The words of the refusal for parens the clause ends inside of, `type(Y` and `type(`.
    const auto unclosed{std::format("syntax error: the '(' after {} is never closed by ')'{}", name, rejected)};

    cursor.skip_blanks();

    const auto inside{cursor.offset()};

    if (cursor.done())
    {
        return std::unexpected{Syntax_error{.offset = inside, .message = unclosed}};
    }

    if (cursor.peek() == ')')
    {
        return std::unexpected{Syntax_error{.offset = inside, .message = std::string{surprise}}};
    }

    auto argument{cursor.identifier()};

    if (argument.empty())
    {
        while (cursor.peek() && is_digit(*cursor.peek()))
        {
            argument.push_back(cursor.next("a digit"));
        }
    }

    if (argument.empty())
    {
        return std::unexpected{Syntax_error{
                .offset = inside,
                .message = std::format(
                        "syntax error: {} stands where the argument of {} should be a name or a number{}",
                        quoted_byte(cursor), name, rejected)}};
    }

    cursor.skip_blanks();

    if (cursor.done())
    {
        return std::unexpected{Syntax_error{.offset = cursor.offset(), .message = unclosed}};
    }

    if (!cursor.accept(')'))
    {
        return std::unexpected{Syntax_error{
                .offset = cursor.offset(),
                .message = std::format(
                        "syntax error: {} stands after the argument of {} where ')' should close it{}",
                        quoted_byte(cursor), name, rejected)}};
    }

    return argument;
}

} // namespace

bool is_number(const std::string_view text)
{
    return !text.empty() && std::ranges::all_of(text, is_digit);
}

Commanded_token apply_commands(
        const Alternative& alternative, const Commanded_rule& rule, const Cursor& grammar, Grammar_tables& tables)
{
    Token_state state{.type = rule.name, .typed_at = rule.line, .channelled = false, .retyped = false};

    const auto [commands, error]{commands_of(alternative.commands)};

    if (error)
    {
        throw Spec_error{error->message, grammar.line_of(alternative.clause + error->offset)};
    }

    for (const auto& command : commands)
    {
        const auto line{grammar.line_of(alternative.clause + command.offset)};

        refuse_unknown(command.name, command.argument, line);

        apply_command(command, line, rule, state, tables);
    }

    // ANTLR's `EOF` is the type minus one, the token that ends the stream, so a rule whose type it is ends the stream
    // where it matches, on any channel: nothing after the match is a token, and the token language has no place for a
    // match that ends the stream.
    if (state.type == "EOF")
    {
        throw Spec_error{
                "'-> type(EOF)' ends the token stream where the rule matches, so nothing after the match is a token, "
                "which the byte reading cannot express",
                state.typed_at};
    }

    return {.token = state.channelled ? std::nullopt : state.type, .retyped = state.retyped};
}

Commands commands_of(const std::string_view text)
{
    Commands read;

    auto& [commands, error]{read};

    Antlr_cursor cursor{text};

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
                    .message = std::format(
                            "syntax error: {} stands where a command's name should{}", quoted_byte(cursor), rejected)};

            return read;
        }

        cursor.skip_blanks();

        std::string argument;

        if (cursor.accept('('))
        {
            auto parenthesised{command_argument(cursor, name)};

            if (!parenthesised)
            {
                error = std::move(parenthesised.error());

                return read;
            }

            argument = std::move(*parenthesised);
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
                                           quoted_byte(cursor), name, rejected) :
                                   std::format(
                                           "syntax error: '{}' stands with no comma before it{}", following, rejected)};

        return read;
    }
}

} // namespace munch::tools::audit
