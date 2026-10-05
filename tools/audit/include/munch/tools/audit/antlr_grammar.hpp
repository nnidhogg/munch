#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_GRAMMAR_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_GRAMMAR_HPP

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/antlr_commands.hpp"
#include "munch/tools/audit/antlr_cursor.hpp"
#include "munch/tools/audit/antlr_members.hpp"
#include "munch/tools/audit/antlr_rule.hpp"
#include "munch/tools/audit/antlr_tables.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

/**
 * @brief An ANTLR grammar file read item by item into the scanner its lexer rules declare, Grammar_reader: the
 *        declaration, the prelude of options, named actions and blocks, the `mode` lines and the rules, each lexer
 *        rule's body read by Expression_reader and its commands applied as ANTLR applies them, and the whole grammar
 *        held to ANTLR's checks by finish_grammar() once the last rule is in.
 */
namespace munch::tools::audit
{
/**
 * @brief A cursor over a grammar's text, reading it item by item into a specification.
 *
 * The grammar is ANTLR 4's: a declaration, then options, named blocks, `mode` lines and rules. A lexer rule's body is
 * read element by element and rewritten for the pattern parser as it goes; a parser rule is skipped, its literals kept
 * for the implicit tokens a combined grammar makes of them.
 */
class Grammar_reader : public Antlr_cursor
{
public:
    /**
     * @brief Binds the cursor to a grammar's text.
     * @param source The text.
     */
    explicit Grammar_reader(std::string_view source);

    /**
     * @brief Reads the whole grammar.
     * @return The scanner.
     * @throws Spec_error If the grammar is malformed or uses a refused construct.
     */
    [[nodiscard]] Lexer_spec read();

private:
    /**
     * @brief What the items read so far leave in force for the next: whether the grammar is a lexer grammar, the case
     *        option, the mode the rules are in, and the actions written into the lexer class.
     */
    struct Item_context
    {
        /**
         * @brief Whether the grammar is a lexer grammar, which alone may declare modes and channels.
         */
        bool lexer_only{};

        /**
         * @brief Whether the grammar's `caseInsensitive` option is set.
         */
        bool case_insensitive{};

        /**
         * @brief The mode the rules read next are in, empty for the default mode.
         */
        std::string mode{};

        /**
         * @brief The lexer's members actions, read once the grammar's options are known, since the option naming the
         *        target language may stand after them and decides what their code is written in.
         */
        std::vector<Members_action> members{};

        /**
         * @brief The code of every named action, whose macros the members may use.
         */
        std::string actions_code{};
    };

    /**
     * @brief A lexer rule's name as read, and whether the rule is a fragment.
     */
    struct Rule_name
    {
        /**
         * @brief The name.
         */
        std::string name{};

        /**
         * @brief Whether `fragment` stands before it.
         */
        bool fragment{};
    };

    /**
     * @brief Reads the grammar's declaration, `lexer grammar NAME;` or `grammar NAME;`, the scanner's line its line.
     * @param spec The specification, whose line is set.
     * @return Whether the grammar is a lexer grammar.
     * @throws Spec_error If there is no declaration, the grammar is a parser grammar or has no name.
     */
    [[nodiscard]] bool grammar_declaration(Lexer_spec& spec);

    /**
     * @brief Reads one item at the top level: a named action, an options, tokens or channels block, an import, a `mode`
     *        line or a rule.
     * @param spec The specification being filled.
     * @param context What the items read so far leave in force, updated.
     * @throws Spec_error If the item is malformed or refused.
     */
    void item(Lexer_spec& spec, Item_context& context);

    /**
     * @brief Reads a named action from its `@`, `@header { }` or `@lexer::members { }`: its code is the target
     *        language's and is skipped, but kept for its macros, and an action ANTLR writes into the lexer class is
     *        kept for refuse_lexer_class().
     *
     * ANTLR's own lexer parts the action's name from the `@`, the `::` and the scope by nothing, so blanks and comments
     * may stand at each of those joints: `@lexer :: members` and `@ members` name what `@lexer::members` names, and a
     * grammar writing one of them is the grammar writing the other.
     *
     * A `lexer::members` action is the lexer's wherever it stands, and an unscoped `members` action is written into
     * both classes a combined grammar generates, the lexer's among them, as ANTLR's grammar documentation has it; only
     * a `parser::members` action leaves the lexer alone, so a combined grammar's unscoped members override the lexer's
     * own nextToken as a `lexer::members` action does. The actions written into the lexer class: `members` under every
     * target, and the C++ target's `declarations`, which its template writes inside the class in the header; its
     * `definitions` go to the source file at namespace scope, where a function is no method of the lexer's. Every
     * action's code, `header` among them, may define a macro the members' initializers use.
     * @param context What the items read so far leave in force, whose actions gain this one.
     * @throws Spec_error If the action's block never closes.
     */
    void named_action(Item_context& context);

    /**
     * @brief Reads the `options { name = value; ... }` block after its keyword, recording the options.
     * @param options Where the options go.
     * @return The value `caseInsensitive` was set to, or std::nullopt where no `true` or `false` set it.
     * @throws Spec_error If the block is malformed: an option with no name, no `=`, no value or no `;`.
     */
    [[nodiscard]] std::optional<bool> options_block(std::vector<std::string>& options);

    /**
     * @brief Reads a `tokens` block after its keyword.
     * @param opened The offset of the keyword, whose line a refusal of a token's name names.
     * @param lexer_only Whether the grammar is a lexer grammar, whose block alone gives its lexer token types.
     * @throws Spec_error As names_block() refuses the block.
     */
    void tokens_spec(std::size_t opened, bool lexer_only);

    /**
     * @brief Reads a `tokens { NAME, ... }` or `channels { NAME, ... }` block after its keyword, recording the names,
     *        as ANTLR's parser reads one: names parted by commas and nothing else, a tokens block alone allowed to hold
     *        nothing (ANTLRParser.g's tokensSpec and channelsSpec), so that a comma missing or trailing, `{ ONE TWO }`
     *        and `{ ONE, }`, is its error 50, a syntax error at the byte after the name or the comma.
     * @param names Where the names go.
     * @param kind What the block declares, `token` or `channel`, which a refusal names.
     * @param may_be_empty Whether the block may hold no name.
     * @throws Spec_error As ANTLR's parser rejects a block of any other shape, at the byte it rejects.
     */
    void names_block(std::set<std::string, std::less<>>& names, std::string_view kind, bool may_be_empty);

    /**
     * @brief Reads a `channels` block after its keyword.
     * @param opened The offset of the keyword, whose line a refusal of a channel's name names.
     * @param lexer_only Whether the grammar is a lexer grammar, which alone may declare channels.
     * @throws Spec_error In a combined grammar, ANTLR's error 164, or as names_block() refuses the block.
     */
    void channels_spec(std::size_t opened, bool lexer_only);

    /**
     * @brief Reads a `mode` line after its keyword, opening or reopening the mode the rules after it are in.
     * @param spec The specification, whose conditions gain a mode declared for the first time.
     * @param opened The offset of the keyword.
     * @param context What the items read so far leave in force, whose mode is set.
     * @throws Spec_error In a combined grammar, for a mode with no name or named INITIAL, or before any rule.
     */
    void mode_spec(Lexer_spec& spec, std::size_t opened, Item_context& context);

    /**
     * @brief Reads a lexer rule from its name, or from `fragment`, through its `;`.
     * @param spec The specification being filled.
     * @param mode The mode the rule is in, empty for the default mode.
     * @param case_insensitive Whether the grammar's option is set.
     * @throws Spec_error As rule_name(), the body's reading, define() and apply_commands() refuse the rule, or for
     *         commands on a rule of several alternatives.
     */
    void lexer_rule(Lexer_spec& spec, const std::string& mode, bool case_insensitive);

    /**
     * @brief Reads a lexer rule's name, after `fragment` where the rule is one.
     * @return The name, and whether the rule is a fragment.
     * @throws Spec_error If the rule has no name, a fragment's is no lexer rule's, or the name is one ANTLR reserves,
     *         its error 159.
     */
    [[nodiscard]] Rule_name rule_name();

    /**
     * @brief Reads a lexer rule's `options { ... }` block from its keyword, which sets nothing but `caseInsensitive`.
     * @param case_insensitive Whether the grammar's option is set, which holds where the rule's block does not set it.
     * @return Whether the option is in force for the rule.
     * @throws Spec_error As options_block() refuses the block.
     */
    [[nodiscard]] bool rule_options(bool case_insensitive);

    /**
     * @brief Records a lexer rule's definition: the expression a reference to it expands to, the line it opens on and
     *        the formula of whether it matches the empty string.
     * @param spec The specification being filled, whose definitions gain the rule's.
     * @param name The rule's name.
     * @param alternatives The rule's outermost alternatives.
     * @param opened The offset the rule opens at.
     * @return The expression, which the rule's own token matches too.
     * @throws Spec_error If the rule matches nothing but the empty string, or its name is defined already, ANTLR's
     *         error 51.
     */
    [[nodiscard]] std::string define(
            Lexer_spec& spec, const std::string& name, const std::vector<Alternative>& alternatives,
            std::size_t opened);

    /**
     * @brief Records a lexer rule that is no fragment as one of the scanner's rules, its commands applied to its token.
     * @param spec The specification being filled, whose rules gain the rule.
     * @param mode The mode the rule is in, empty for the default mode.
     * @param alternatives The rule's outermost alternatives.
     * @param whole The expression the rule's token matches.
     * @param rule The rule's name, line and whether it has a token type of its own.
     * @param aliased Whether the rule spells a parser literal ANTLR maps onto it.
     * @throws Spec_error As apply_commands() refuses the commands.
     */
    void record_rule(
            Lexer_spec& spec, const std::string& mode, const std::vector<Alternative>& alternatives,
            const std::string& whole, const Commanded_rule& rule, bool aliased);

    /**
     * @brief Skips a parser rule from its name through its `;` and the `catch` and `finally` blocks after it,
     *        collecting the literals it uses.
     * @throws Spec_error If the rule never ends, or a block in it never closes.
     */
    void parser_rule();

    /**
     * @brief Skips a parser rule's argument block from its `[`, the arguments, `returns`, `locals`, a rule reference's
     *        arguments or a `catch` clause's, as ANTLR's lexer reads an ARG_ACTION: brackets nest, a `"..."` or a
     *        `'...'` inside is skipped whole, a backslash escaping the byte after it, and no comment is recognised, so
     *        a `]` inside a quoted string is no closer and one inside what looks like a comment is.
     * @throws Spec_error If the block or a quoted string inside it never closes.
     */
    void skip_argument();

    /**
     * @brief What the reading records about the whole grammar.
     */
    Grammar_tables tables_;
};

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_GRAMMAR_HPP
