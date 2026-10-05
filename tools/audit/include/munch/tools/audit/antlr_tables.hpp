#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_TABLES_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_TABLES_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/antlr_rule.hpp"

/**
 * @brief What the reading of an ANTLR grammar records about the whole of it for the checks that wait until every rule
 *        is in, Grammar_tables, with a name or a literal and its line, Occurrence, a closure whose body's emptiness
 *        waits on the whole grammar, Closure, a `mode` line's section, Mode_section, and the default mode's name,
 *        default_mode.
 *
 * ANTLR resolves a rule's references, a command's argument and a parser literal's token over the whole grammar, before
 * or after the rule, so what those need is gathered here as the rules are read and answered once the grammar ends.
 */
namespace munch::tools::audit
{
/**
 * @brief The name the default mode goes by, which a `mode` line reopens it with.
 */
constexpr std::string_view default_mode{"DEFAULT_MODE"};

/**
 * @brief A name or a literal as the grammar writes it, with the line it stands on, which a refusal of it names.
 */
struct Occurrence
{
    /**
     * @brief The name, or the literal with its quotes.
     */
    std::string text{};

    /**
     * @brief The line.
     */
    std::size_t line{};
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
    std::string rule{};

    /**
     * @brief The line the closure's body opens on.
     */
    std::size_t line{};

    /**
     * @brief Whether the body matches the empty string.
     */
    Nullable_t body{};
};

/**
 * @brief A `mode` line's section: its name, its line and how many rules that are no fragments it holds, since ANTLR's
 *        error 145 refuses each section holding none, a reopened mode's empty section included.
 */
struct Mode_section
{
    /**
     * @brief The mode's name, DEFAULT_MODE where the section reopens the default mode.
     */
    std::string name{};

    /**
     * @brief The line of the `mode` line.
     */
    std::size_t line{};

    /**
     * @brief How many rules that are no fragments the section holds.
     */
    std::size_t rules{};
};

/**
 * @brief What the reading records about the whole grammar as it reads the rules, for the checks after the last one.
 */
struct Grammar_tables
{
    /**
     * @brief The literals the parser rules use, in order of first appearance, quotes included.
     */
    std::vector<Occurrence> parser_literals{};

    /**
     * @brief The literals the lexer rules spell in a shape ANTLR maps a parser literal onto, as written, and how many
     *        rules spell each: one makes the parser's literal that rule's token, two make it ANTLR's error 126.
     */
    std::map<std::string, std::size_t, std::less<>> aliases{};

    /**
     * @brief The channels the grammar's `channels` block declares, which a channel command may name.
     */
    std::set<std::string, std::less<>> channels{};

    /**
     * @brief The line of the `channels` block, where a channel's name conflicting with a token's or a mode's is refused
     *        as ANTLR's errors 161 and 162 refuse it.
     */
    std::size_t channels_line{0};

    /**
     * @brief The line of a lexer grammar's `tokens` block, where a token named DEFAULT_MODE is refused as ANTLR's error
     *        170 refuses it.
     */
    std::size_t tokens_line{0};

    /**
     * @brief The names a lexer grammar's `tokens` block declares, which ANTLR gives a token type before any rule, so
     *        that a rule of that name whose commands set its type to zero, ANTLR's value for none, emits its own token;
     *        a combined grammar's block goes to its parser alone and is left out.
     */
    std::set<std::string, std::less<>> tokens{};

    /**
     * @brief The rules whose body was read with a non-greedy loop in it, and the line each opens on.
     *
     * ANTLR stops such a loop where the rest of the surrounding lexical rule matches, and a rule another rule
     * references is inlined into that one, whose rest reaches past it; the reading is exact only while nothing
     * references the rule, which finish_grammar() checks once the whole grammar is in.
     */
    std::vector<Occurrence> lazy_rules{};

    /**
     * @brief The names of the rules read that are no fragments, which a `type` command may name, before or after the
     *        rule, since ANTLR resolves the name over the whole grammar.
     */
    std::set<std::string, std::less<>> rule_names{};

    /**
     * @brief The names `type` commands carry that are no numbers, each with its line, resolved once the grammar is
     *        read: ANTLR's error 175 refuses one that names no rule and no `tokens` entry.
     */
    std::vector<Occurrence> typed_names{};

    /**
     * @brief The `mode` sections in the order declared, DEFAULT_MODE's among them, which reopens the default mode.
     */
    std::vector<Mode_section> sections{};

    /**
     * @brief The names `mode` and `pushMode` commands carry that are no numbers, each with its line, resolved once the
     *        grammar is read: ANTLR's error 176 refuses one that names no mode.
     */
    std::vector<Occurrence> mode_names{};

    /**
     * @brief The closures read, each with the rule it stands in, so that a body reaching a rule written further down is
     *        decided once the whole grammar is in.
     */
    std::vector<Closure> closures{};

    /**
     * @brief The alternatives of every rule holding a non-greedy loop, with the loop's line, whose nullability the
     *        fixed point over the grammar decides once every rule is read.
     */
    std::vector<Closure> lazy_empty{};

    /**
     * @brief Whether each rule matches the empty string, as the formula over the rules it reaches.
     */
    std::map<std::string, Nullable_t, std::less<>> nullability{};

    /**
     * @brief The line each rule's definition opens on, fragments included, which a redefinition is refused against.
     */
    std::map<std::string, std::size_t, std::less<>> definition_lines{};
};

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_TABLES_HPP
