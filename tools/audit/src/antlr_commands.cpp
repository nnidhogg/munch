#include "munch/tools/audit/antlr_commands.hpp"

#include <algorithm>
#include <array>
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
    std::optional<std::string> type{};

    /**
     * @brief The line of the command that set the type last, the rule's own while none has: where a refusal of the type
     *        the commands set is reported.
     */
    std::size_t typed_at{};

    /**
     * @brief Whether a channel command sends the token to a channel a parser does not read.
     */
    bool channelled{};

    /**
     * @brief Whether a type command sets the type.
     */
    bool retyped{};
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
    static constexpr std::array<std::string_view, 7> templates{"Skip",    "More", "PopMode", "Type",
                                                               "Channel", "Mode", "PushMode"};

    const auto templated{std::ranges::contains(templates, command)};

    auto lowered{command};

    if (templated)
    {
        lowered.front() = static_cast<char>(lowered.front() | case_bit);
    }

    static constexpr std::array<std::string_view, 3> plain_commands{"skip", "more", "popMode"};

    const auto plain{std::ranges::contains(plain_commands, lowered)};

    static constexpr std::array<std::string_view, 4> called_commands{"type", "channel", "mode", "pushMode"};

    const auto called{std::ranges::contains(called_commands, lowered)};

    if (!plain && !called)
    {
        const auto message{
                std::format("lexer command {} does not exist or is not supported by the current target", command)};

        throw Spec_error{message, line};
    }

    if (called && argument.empty())
    {
        const auto message{std::format("missing argument for lexer command {}", command)};

        throw Spec_error{message, line};
    }

    if (plain && !argument.empty())
    {
        const auto message{std::format("lexer command {} does not take any arguments", command)};

        throw Spec_error{message, line};
    }

    if (templated)
    {
        const auto message{std::format(
                "lexer command {} names a code template of ANTLR's target, expanded into an action the generated lexer "
                "runs and ANTLR's own interpreter leaves out, so what the token stream holds is the target's to say",
                command)};

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

    const auto rule_type{rule.typed ? rule.name : std::string{"0"}};

    state.type = zero ? rule_type : argument;

    state.typed_at = line;

    state.retyped = true;

    // EOF is a token ANTLR always has; whether a rule may end on it is settled once every command is applied.
    if (!zero && argument != "EOF" && !is_number(argument))
    {
        tables.typed_names.push_back({.text = argument, .line = line});
    }
}

/**
 * @brief Returns whether a channel command's argument names a channel other than the default one, the one a parser
 *        reads, resolved as ANTLR resolves it (LexerATNFactory.getChannelConstantValue): `HIDDEN` and
 *        `DEFAULT_TOKEN_CHANNEL` are its constants one and zero, another of its reserved names is its error 172, a name
 *        the grammar's `channels` block declares is a channel from two up, and anything else is read as a decimal
 *        number, `00` and `000` being zero and the default channel and every other number a channel of its own, a
 *        number beyond its int or a name nothing declares being its error 177, each in its words.
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
        const auto message{std::format("cannot use or declare channel with reserved name {}", argument)};

        throw Spec_error{message, line};
    }

    if (declared.contains(argument))
    {
        return true;
    }

    const auto nonzero{argument.find_first_not_of('0')};

    const auto first_digit{std::min(nonzero, argument.size())};

    const auto digits{std::string_view{argument}.substr(first_digit)};

    // Integer.parseInt: the digits, leading zeros dropped, up to 2147483647.
    static constexpr std::string_view largest{"2147483647"};

    if (is_number(argument) &&
        (digits.size() < largest.size() || (digits.size() == largest.size() && digits <= largest)))
    {
        return !digits.empty();
    }

    const auto message{std::format("{} is not a recognized channel name", argument)};

    throw Spec_error{message, line};
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
 * @brief Returns the byte under the cursor, quoted for a refusal.
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
        auto message{std::format(
                "syntax error: {} stands where the argument of {} should be a name or a number{}", quoted_byte(cursor),
                name, rejected)};

        return std::unexpected{Syntax_error{.offset = inside, .message = std::move(message)}};
    }

    cursor.skip_blanks();

    if (cursor.done())
    {
        return std::unexpected{Syntax_error{.offset = cursor.offset(), .message = unclosed}};
    }

    if (!cursor.accept(')'))
    {
        auto message{std::format(
                "syntax error: {} stands after the argument of {} where ')' should close it{}", quoted_byte(cursor),
                name, rejected)};

        return std::unexpected{Syntax_error{.offset = cursor.offset(), .message = std::move(message)}};
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
        const auto& [offset, message]{*error};

        throw Spec_error{message, grammar.line_of(alternative.clause + offset)};
    }

    for (const auto& command : commands)
    {
        const auto& [name, argument, offset]{command};

        const auto line{grammar.line_of(alternative.clause + offset)};

        refuse_unknown(name, argument, line);

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
    Commands read{};

    auto& [commands, error]{read};

    Antlr_cursor cursor{text};

    // The offset of the comma taken as a separator before the command about to be read, stray where none follows.
    std::optional<std::size_t> comma{};

    for (;;)
    {
        cursor.skip_blanks();

        const auto offset{cursor.offset()};

        // A comma where a command should stand, before the first, after another or ending the clause, is stray.
        if (cursor.done() || cursor.peek() == ',')
        {
            if (comma || !cursor.done())
            {
                auto message{std::format("syntax error: ',' stands where no command name follows it{}", rejected)};

                error = Syntax_error{.offset = cursor.done() ? *comma : offset, .message = std::move(message)};
            }

            return read;
        }

        const auto name{cursor.identifier()};

        if (name.empty())
        {
            auto message{std::format(
                    "syntax error: {} stands where a command's name should{}", quoted_byte(cursor), rejected)};

            error = Syntax_error{.offset = offset, .message = std::move(message)};

            return read;
        }

        cursor.skip_blanks();

        std::string argument{};

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

        std::string message{};

        if (following.empty())
        {
            message = std::format(
                    "syntax error: {} stands after the command {} where ',' or ';' should{}", quoted_byte(cursor), name,
                    rejected);
        }
        else
        {
            message = std::format("syntax error: '{}' stands with no comma before it{}", following, rejected);
        }

        error = Syntax_error{.offset = after, .message = std::move(message)};

        return read;
    }
}

} // namespace munch::tools::audit
