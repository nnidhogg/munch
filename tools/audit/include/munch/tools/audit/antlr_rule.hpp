#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_RULE_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_RULE_HPP

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/antlr_alphabet.hpp"

/**
 * @brief What a lexer rule's body is once read: its elements, Element, the sequences they make, Sequence, and the
 *        outermost alternatives with their commands, Alternative; whether each can match the empty string, as a formula
 *        over the rules it reaches, Nullable_t, joined as an alternation's and a sequence's are, and the refusal of a
 *        closure that can, closure_refusal(); and the rewriting a body's parts take for the pattern parser, the
 *        grouping a suffix needs, is_atomic(), the regex a non-greedy loop before a terminator stands for,
 *        rest_spelling() and avoiding(), and an alternation's join and group, join_onto() and grouped().
 */
namespace munch::tools::audit
{
/**
 * @brief Whether something matches the empty string, as a formula over the rules it reaches: it does when every rule of
 *        one term does, so a term of no rule is yes and no term at all is no.
 *
 * A rule's own answer waits for the whole grammar, since a rule may reference one written below it, and the closures
 * ANTLR rejects are the ones whose body can match the empty string.
 */
using Nullable_t = std::vector<std::vector<std::string>>;

/**
 * @brief One element of an alternative: its rewritten atom, what the atom admits when it is a set, the literal it
 *        spells when it is one, and its suffix.
 */
struct Element
{
    /**
     * @brief The atom in the parser's syntax, the suffix not yet applied.
     */
    std::string expression{};

    /**
     * @brief The literal's bytes, when the atom is an exact literal.
     */
    std::optional<std::string> literal{};

    /**
     * @brief The atom's text as written, quotes included, when it is a quoted literal, folded or not: what ANTLR
     *        matches a parser rule's literal against in a combined grammar. Empty for any other atom.
     */
    std::string spelling{};

    /**
     * @brief What the atom admits, when it is a set, `.`, or a one-character literal.
     */
    std::optional<Alphabet> alphabet{};

    /**
     * @brief The characters a match of the atom can begin with, when they are known: a literal's first character, a
     *        set's members, a group's alternatives' firsts; unknown for a reference.
     */
    std::optional<Beginning> first{};

    /**
     * @brief Whether the atom matches the empty string, the suffix not yet applied.
     */
    Nullable_t nullable{};

    /**
     * @brief Whether every match of the atom has one and the same length in bytes, which a literal's have and a set
     *        reaching past U+007F has not, its scalars being one to four bytes: what the alignment of a non-greedy
     *        loop's iterations rests on.
     */
    bool one_length{};

    /**
     * @brief The number of characters every match of the atom has, when they all have one, the suffix not yet applied:
     *        a literal's count of scalars, one for a set, a range or the dot, nothing for an inert action, a group's
     *        where its alternatives agree; unknown for a reference and where they do not.
     */
    std::optional<std::size_t> characters{};

    /**
     * @brief The suffix, `?`, `*`, `+`, or nothing.
     */
    char suffix{};

    /**
     * @brief Whether the suffix was non-greedy, `??`, `*?` or `+?`.
     */
    bool lazy{};
};

/**
 * @brief One sequence of elements as read: its expression, and what its matches begin with and measure, when known.
 */
struct Sequence
{
    /**
     * @brief The expression.
     */
    std::string expression{};

    /**
     * @brief The characters a match can begin with, when every element that can begin one says which.
     */
    std::optional<Beginning> first{};

    /**
     * @brief The number of characters every match has, when every element's matches have one and no suffix varies it;
     *        unknown otherwise.
     */
    std::optional<std::size_t> characters{};

    /**
     * @brief Whether an element of the sequence carries a non-greedy suffix.
     */
    bool lazy{};

    /**
     * @brief Whether the sequence has no element, an empty alternative.
     */
    bool empty{};

    /**
     * @brief Whether the sequence matches the empty string.
     */
    Nullable_t nullable{};

    /**
     * @brief The one literal's text as written when the sequence is that literal alone, or that literal and one inert
     *        action after it, the two shapes of an alternative ANTLR's own patterns for a rule spelling a parser
     *        literal match; empty otherwise.
     */
    std::string spelling{};

    /**
     * @brief Whether an inert action follows the literal the spelling names.
     */
    bool acted{};
};

/**
 * @brief One outermost alternative of a rule with its commands.
 */
struct Alternative
{
    /**
     * @brief The alternative's text as written, commands excluded.
     */
    std::string pattern{};

    /**
     * @brief The alternative in the parser's syntax.
     */
    std::string expression{};

    /**
     * @brief The commands after `->`, as written, empty when there are none.
     */
    std::string commands{};

    /**
     * @brief The offset in the grammar the commands' text begins at, from which a refusal of a command names the
     *        command's own line as ANTLR names it; the alternative's own offset when it has none.
     */
    std::size_t clause{};

    /**
     * @brief The one literal's text as written when the alternative is that literal alone or with one inert action
     *        after it, as the sequence has it; empty otherwise.
     */
    std::string spelling{};

    /**
     * @brief Whether an inert action follows the literal the spelling names.
     */
    bool acted{};

    /**
     * @brief Whether the alternative has no element, which makes the rule match the empty string too.
     */
    bool empty{};

    /**
     * @brief Whether the alternative matches the empty string, an empty one and `'a'?` alike.
     */
    Nullable_t nullable{};
};

/**
 * @brief Returns the formula of something that never matches the empty string.
 * @return No term.
 */
[[nodiscard]] Nullable_t never_empty();

/**
 * @brief Returns the formula of a match of either of two, an alternation's: their terms together.
 * @param left One formula.
 * @param right The other.
 * @return The disjunction, one term of no rule where either holds on its own.
 */
[[nodiscard]] Nullable_t either_empty(Nullable_t left, const Nullable_t& right);

/**
 * @brief Returns whether a formula holds, given the rules known to match the empty string.
 * @param formula The formula.
 * @param nullable The rules that match the empty string, none for the answer a formula gives on its own.
 * @return True when one term's every rule is among them.
 */
[[nodiscard]] bool matches_empty(const Nullable_t& formula, const std::set<std::string, std::less<>>& nullable);

/**
 * @brief Returns the formula of something that matches the empty string whatever the rules do.
 * @return One term of no rule.
 */
[[nodiscard]] Nullable_t always_empty();

/**
 * @brief Returns the formula of a match of both of two, a sequence's: every term of the one joined with every term of
 *        the other, so that a term holds when all the rules it gathered do.
 * @param left One formula.
 * @param right The other.
 * @return The conjunction, one term of no rule where both hold on their own.
 */
[[nodiscard]] Nullable_t both_empty(const Nullable_t& left, const Nullable_t& right);

/**
 * @brief Returns the refusal of a closure whose body can match the empty string, which ANTLR rejects as its error 153.
 * @param rule The rule the closure stands in, named as ANTLR names it.
 * @return The message.
 */
[[nodiscard]] std::string closure_refusal(std::string_view rule);

/**
 * @brief Returns whether an expression is one unit a suffix applies to as it stands: one bracket, one group, one
 *        reference or a quoted literal of one byte, so that it needs no grouping of its own.
 * @param expression The expression.
 * @return True when it is.
 */
[[nodiscard]] bool is_atomic(std::string_view expression);

/**
 * @brief Returns the one string the rest of a sequence spells, when every element of it is an exact ASCII literal that
 *        matches once: what ANTLR's fewest-characters rule stops a non-greedy loop before it at.
 *
 * An inert action matches nothing and is stepped over. Anything else, a set, a group, a reference, a suffixed element
 * or a literal with a letter under `caseInsensitive` or a byte beyond ASCII, leaves the rest more than one string and
 * the loop with no rewrite the automaton below can build.
 * @param elements The sequence's elements.
 * @param from The index of the first element after the loop.
 * @return The bytes, or std::nullopt when the rest is not one fixed ASCII string.
 */
[[nodiscard]] std::optional<std::string> rest_spelling(const std::vector<Element>& elements, std::size_t from);

/**
 * @brief Returns the regex over an alphabet of every string a non-greedy loop before a terminator matches: the loop
 *        stops at the first point the terminator can follow, so its body holds no occurrence of the terminator and does
 *        not end where the terminator would complete one, ANTLR's fewest characters that still let the rest match.
 *
 * Built as the automaton that tracks the longest prefix of the terminator ending at the byte just read, the steps that
 * would reach the whole terminator dropped, and then written out by eliminating its states one by one. A state the
 * terminator overlaps into an earlier occurrence is no end of the body, which is what keeps `.*? 'aa'` from matching
 * `aaa`: the terminator appended after its first j bytes spells an occurrence beginning j bytes early exactly when j is
 * a period of it, and the state's prefix, the longest one the body ends in, is the only one to test, since a shorter
 * one is a suffix of it and a periodic terminator's suffix of that kind makes the longest one a period too. The
 * terminator is ASCII, so a scalar beyond ASCII never extends a prefix and takes every state back to the start.
 * @param terminator The terminator's bytes, at least one and every one ASCII.
 * @param alphabet What the loop admits.
 * @return The expression, grouped.
 */
[[nodiscard]] std::string avoiding(std::string_view terminator, const Alphabet& alphabet);

/**
 * @brief Appends a text to a join, the separator before it unless the join is still empty.
 * @param joined The join, added to.
 * @param text The text.
 * @param separator What parts two texts.
 */
void join_onto(std::string& joined, std::string_view text, std::string_view separator);

/**
 * @brief Returns an alternation as one group, made optional when one of its alternatives is empty.
 * @param inner The alternation's text.
 * @param optional Whether an alternative is empty.
 * @return The group, `?` after it when optional.
 */
[[nodiscard]] std::string grouped(std::string_view inner, bool optional);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_RULE_HPP
