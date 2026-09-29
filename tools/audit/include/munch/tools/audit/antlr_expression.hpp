#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_EXPRESSION_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_EXPRESSION_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/antlr_alphabet.hpp"
#include "munch/tools/audit/antlr_cursor.hpp"
#include "munch/tools/audit/antlr_rule.hpp"
#include "munch/tools/audit/antlr_tables.hpp"

/**
 * @brief A lexer rule's body read into its outermost alternatives, Expression_reader, each element rewritten for the
 *        pattern parser as it is read: a literal, a range, a set, a negation, the dot, a group, a reference or an inert
 *        action, with its suffix, and a non-greedy loop before the string that ends it.
 *
 * The reader stands on the grammar's text where the body begins, after the rule's `:`, and is left after the `;` that
 * ends it, which the grammar takes back as its own offset; what the whole grammar has to answer, a closure's body
 * reaching a rule written further down and a loop's rule matching the empty string, is recorded in the grammar's tables
 * for the checks after the last rule.
 */
namespace munch::tools::audit
{
/**
 * @brief A cursor over a lexer rule's body, reading it element by element into its alternatives.
 */
class Expression_reader : public Antlr_cursor
{
public:
    /**
     * @brief Binds the reader to a rule's body.
     * @param text The grammar's text.
     * @param begin The offset the body begins at, after the rule's `:`.
     * @param tables What the reading records about the whole grammar, which the body's closures and loops join.
     * @param rule The rule's name, which a closure is recorded under.
     * @param case_insensitive Whether the `caseInsensitive` option is in force for the rule, which folds its letters.
     */
    Expression_reader(
            std::string_view text, std::size_t begin, Grammar_tables& tables, std::string rule, bool case_insensitive);

    /**
     * @brief Reads the outermost alternatives of a rule, through the `;`.
     *
     * ANTLR's lexer follows the rule's alternatives at once, in their order, and the first path to reach the rule's end
     * stops every later path that has passed a non-greedy decision: `'ab' | 'a' .*? 'c'` on "abc" ends at the second
     * character, where `'a' .*? 'c' | 'ab'` takes all three. An earlier alternative that no character begins together
     * with this one is dead before the loop's decision is reached, so the loop is read where every earlier alternative
     * is such.
     *
     * A comment between the commands is the grammar's and may hold a `;` or a `|` of its own, so the clause ends at the
     * first of those the reading stands on rather than at the first in the text; the clause is kept through that byte,
     * exclusive, so that a refusal of parens it ends inside of names the line of the `;`, as ANTLR names it.
     *
     * ANTLR's empty match reaches the rule's end at the loop's decision, in whichever alternative it stands, and stops
     * the loop there: `X : | .*? 'a' ;` on "aa" emits no X at all and `X : .*? 'a' | ;` two of one character, where the
     * greedy reading spans both, so the loop is read only in a rule no alternative of which can match the empty string.
     * Whether an alternative matches the empty string is a formula over the rules it reaches, which the least fixed
     * point over the whole grammar answers, so the loop's own decision waits for finish_grammar(): a reference to a
     * rule that matches no empty string, `FIELD : '"' .*? '"' | WORD ;`, leaves a formula that is not empty and an
     * alternative that is not nullable, which ANTLR reads and emits.
     * @return The alternatives.
     * @throws Spec_error If a rule's alternative is malformed or refused, a non-greedy loop after an alternative that
     *         can begin with the same character among them, or `->` has no command after it.
     */
    [[nodiscard]] std::vector<Alternative> alternatives();

    /**
     * @brief Whether the body holds a non-greedy loop, which the grammar records with the rule's name.
     * @return True when it does.
     */
    [[nodiscard]] bool is_lazy() const noexcept;

private:
    /**
     * @brief An atom as read, and whether ANTLR's parser takes element options after it, `'x'<a=b>`: after a literal
     *        that is no range, a reference or the dot (ANTLRParser.g's terminal and wildcard), and after no set, range,
     *        negation or group.
     */
    struct Atom
    {
        /**
         * @brief The atom, its suffix not yet read.
         */
        Element element;

        /**
         * @brief Whether element options may follow it.
         */
        bool optionable;
    };

    /**
     * @brief A range's end as read, and as written, quotes included.
     */
    struct Range_end
    {
        /**
         * @brief The end.
         */
        Literal end;

        /**
         * @brief The end as written.
         */
        std::string spelling;
    };

    /**
     * @brief Reads one sequence of elements, up to `|`, `)`, `->` or `;`, and composes their expression.
     * @param outermost Whether this sequence is a whole alternative of a rule rather than a group's inside, which is
     *        what a non-greedy loop needs, since ANTLR stops one where the rest of the whole rule matches.
     * @return The sequence.
     * @throws Spec_error As element() and lazy_element() refuse an element.
     */
    [[nodiscard]] Sequence sequence(bool outermost);

    /**
     * @brief Rewrites a non-greedy element of a sequence for the pattern parser, the loop or option stopping where
     *        ANTLR's fewest characters that still let the rest of the rule match stop it.
     *
     * The rest a loop stops at is the whole rest of the rule, not the element after it: the rest is a regular rewrite
     * only where it spells one ASCII string, and `.*? 'a' 'b'` stops at `ab`, not at `a`. Inside a group the rest of
     * the rule reaches past what the sequence holds, `('a' .*? 'b') 'c'` stopping at `bc` and not at `b`, so the loop
     * is read in an outermost alternative alone. ANTLR's lexer follows every path through the rule at once, in the
     * order the alternatives before the loop give them, and the first path to reach the rule's end stops every later
     * path that has passed the loop's decision: where the elements before the loop match one length in characters,
     * every path reaches the decision at the same character and the order decides nothing; where they do not,
     * `('a'|'aa') .*? 'a'` on "aaa", the path through 'a' ends at the second character and stops the path through 'aa'
     * there, while `('aa'|'a') .*? 'a'` takes all three, a difference the greedy rewrite of the group cannot keep.
     *
     * The bypass of a non-greedy option comes first among the paths, and the body's alternatives after it in order; so
     * the body may not begin the rest, which the bypass would end the rule with at once, and its alternatives must
     * reach the rest at one character together, `('x'|'xa')?? 'a'` on "xaa" stopping after "xa" where `('xa'|'x')??
     * 'a'` takes all three. Over one set, the dot or one character the loop is the strings that stop at the rest. Over
     * a body of one length, a literal of several characters folded or not, the iterations are aligned to that length,
     * and where the rest cannot begin with the body's first byte no repetition of it can hold the rest, so the greedy
     * loop is the same language. Over a body of several lengths it is neither: ANTLR stops the loop at the fewest
     * characters that let the rest match and takes a group's alternatives in order, so `('x'|'xa')*? 'a'` stops on
     * "xaa" after "xa" while `('xa'|'x')*? 'a'` takes all three, a difference no greedy rewrite over the group can
     * keep.
     * @param elements The sequence's elements.
     * @param index The index of the non-greedy element.
     * @param outermost Whether the sequence is a whole alternative of the rule.
     * @return The element's expression.
     * @throws Spec_error Where the loop or the option is of a shape the byte reading cannot express.
     */
    [[nodiscard]] std::string lazy_element(const std::vector<Element>& elements, std::size_t index, bool outermost);

    /**
     * @brief Reads one element: an atom and its suffix.
     * @return The element.
     * @throws Spec_error As the reading of its atom refuses it, or for a closure whose body can match the empty string,
     *         ANTLR's error 153.
     */
    [[nodiscard]] Element element();

    /**
     * @brief Reads an action `{...}` in a rule, which ANTLR runs at this point of the match and whose code can make the
     *        rule produce another token than its own, `{more();}` joining the match onto the next token's and
     *        `{setType(X);}` renaming it; only a body of blanks and comments runs nothing and is inert, matching
     *        nothing. A predicate `{...}?` conditions the match itself.
     * @param opened The offset of the `{`.
     * @return The element, which matches the empty string alone.
     * @throws Spec_error For a predicate, or an action that is not inert.
     */
    [[nodiscard]] Element action_element(std::size_t opened);

    /**
     * @brief Reads the atom an opening byte begins, other than an action: a literal or a range, a set, a negation, the
     *        dot, a group or a reference.
     * @param byte The opening byte.
     * @param opened The offset of the byte.
     * @return The atom, and whether ANTLR's parser takes element options after it.
     * @throws Spec_error As the reading of the atom's kind refuses it.
     */
    [[nodiscard]] Atom atom(char byte, std::size_t opened);

    /**
     * @brief Reads a quoted literal from its opening quote, and the range `'a'..'z'` it begins where `..` follows it.
     * @param opened The offset of the quote.
     * @return The literal or the range, and whether element options may follow it, as they may a literal alone.
     * @throws Spec_error As literal(), range_element() and literal_element() refuse it.
     */
    [[nodiscard]] Atom quoted_element(std::size_t opened);

    /**
     * @brief Reads a range of characters from its `..`, `'a'..'z'`: each end one character as ANTLR reads one, its
     *        error 144 otherwise, and the end no lower than the start, its error 174 otherwise.
     * @param opened The offset of the range's opening quote.
     * @param start The literal the range starts with.
     * @param spelling That literal as written, quotes included.
     * @return The element, which admits the range.
     * @throws Spec_error As ANTLR's errors 144 and 174 refuse the range, or where it reaches past ASCII under the
     *         option.
     */
    [[nodiscard]] Element range_element(std::size_t opened, const Literal& start, const std::string& spelling);

    /**
     * @brief Reads a range's end from its `..`, through the end's closing quote.
     * @return The end and its spelling.
     * @throws Spec_error If no quote opens the end, or as literal() refuses it.
     */
    [[nodiscard]] Range_end range_end();

    /**
     * @brief The last character of a range, `'a'..'z'`: its end one character as ANTLR reads one, its error 144
     *        otherwise, and no lower than the start, its error 174 otherwise.
     * @param opened The offset of the range's opening quote, which a refusal names.
     * @param low The range's first character.
     * @param spelling The start as written, quotes included.
     * @param end The range's end as read.
     * @return The character.
     * @throws Spec_error As ANTLR's errors 144 and 174 refuse the range.
     */
    [[nodiscard]] char32_t range_high(
            std::size_t opened, char32_t low, const std::string& spelling, const Range_end& end);

    /**
     * @brief Refuses an atom reaching past ASCII where `caseInsensitive` is in force, since ANTLR folds such a
     *        character with the Unicode case mappings the library has not got.
     * @param opened The offset of the atom, which the refusal names.
     * @param beyond Whether the atom admits a character beyond ASCII.
     * @throws Spec_error If it does under the option.
     */
    void refuse_unfoldable(std::size_t opened, bool beyond);

    /**
     * @brief The element a quoted literal no range follows is: its bytes, folded where letters double their case, of
     *        one length, beginning with its first character, and admitting that character where it is one.
     * @param opened The offset of the literal's opening quote.
     * @param bytes The literal's bytes.
     * @param spelling The literal as written, quotes included, which a rule spelling a parser literal is matched by.
     * @return The element.
     * @throws Spec_error If the literal is empty, or holds a byte beyond ASCII under the option.
     */
    [[nodiscard]] Element literal_element(std::size_t opened, const std::string& bytes, const std::string& spelling);

    /**
     * @brief Reads a set from its `[`.
     * @param opened The offset of the `[`.
     * @return The element, which admits the set.
     * @throws Spec_error If the set holds nothing but surrogates, or reaches past ASCII under the option.
     */
    [[nodiscard]] Element set_element(std::size_t opened);

    /**
     * @brief Reads a set after its `[`, through the `]`.
     *
     * Each member and each span folds on its own, as ANTLR folds them: `[xA-t9]` gains `X` and nothing of `A-t`. A
     * member or a span of nothing but surrogates is ANTLR's member no UTF-8 input decodes to, which its lexer never
     * matches, and is left out; a span reaching past them keeps what lies on either side. A member waits until the next
     * one shows whether a '-' spans them; an escaped `\-` is a member, not a span.
     * @return What it admits.
     * @throws Spec_error If a span's end is below its start, the set is empty, ANTLR's error 174, or a character is
     *         refused.
     */
    [[nodiscard]] Alphabet set();

    /**
     * @brief Reads a negation from its `~`.
     * @param opened The offset of the `~`.
     * @return The element, which admits every scalar the negated one does not.
     * @throws Spec_error As negatable() refuses what is negated, or where it reaches past ASCII under the option.
     */
    [[nodiscard]] Element negation_element(std::size_t opened);

    /**
     * @brief Reads what a `~` negates: a set, a one-character literal, or a group of those separated by `|`.
     * @return What is negated.
     * @throws Spec_error If what follows is none of those, a literal is not one character, ANTLR's error 144, or a
     *         range's end is below its start, its error 174.
     */
    [[nodiscard]] Alphabet negatable();

    /**
     * @brief Reads the dot, which admits every scalar.
     * @return The element.
     */
    [[nodiscard]] Element dot_element();

    /**
     * @brief Reads a group from its `(`, through its `)`: its alternatives, which it begins with and matches the empty
     *        string as any of them does, of their length where they agree on one, and optional where one is empty.
     * @param opened The offset of the `(`.
     * @return The element.
     * @throws Spec_error If every alternative is empty, or as sequence() refuses one.
     */
    [[nodiscard]] Element group_element(std::size_t opened);

    /**
     * @brief Reads a reference to a rule, `{NAME}` to the pattern parser, which matches the empty string as the rule
     *        does.
     * @param opened The offset of the name.
     * @param byte The byte the name opens with, which a refusal names where no name begins.
     * @return The element.
     * @throws Spec_error If no name begins at the byte, or the name is EOF.
     */
    [[nodiscard]] Element reference_element(std::size_t opened, char byte);

    /**
     * @brief What the reading records about the whole grammar, the closures and the non-greedy loops' alternatives
     *        among them.
     */
    Grammar_tables& tables_;

    /**
     * @brief The rule being read, which a closure is recorded under.
     */
    std::string rule_;

    /**
     * @brief Whether the `caseInsensitive` option is in force for the rule, which doubles a letter's case.
     */
    bool case_insensitive_;

    /**
     * @brief Whether the rule holds a non-greedy loop, which the grammar records with the rule's name.
     */
    bool lazy_{false};
};

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_EXPRESSION_HPP
