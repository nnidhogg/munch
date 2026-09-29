#include "munch/tools/audit/antlr_expression.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/antlr_commands.hpp"
#include "munch/tools/audit/cursor.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements antlr_expression.hpp: what a sequence begins with and measures, an element before its atom, an inert
// action and the words of ANTLR's errors 144 and 174 are private to this unit.

/**
 * @brief What a sequence can begin with: every element up to and including the first that cannot match the empty
 *        string, an element under `?` or `*` and a group with an empty alternative among those that can, an inert
 *        action stepped over.
 * @param elements The sequence's elements.
 * @return The characters, or std::nullopt where an element that can begin a match does not say which, a reference among
 *         them, or whose emptiness waits on a rule.
 */
[[nodiscard]] std::optional<Beginning> beginning_of(const std::vector<Element>& elements)
{
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
            return std::nullopt;
        }

        first->join(*element.first);

        if (!skippable)
        {
            return first;
        }
    }

    return first;
}

/**
 * @brief How many characters a match of a sequence's first elements has, when every one's matches have one length and
 *        no suffix varies it.
 * @param elements The sequence's elements.
 * @param count How many of them, from the first.
 * @return The count of characters, or std::nullopt where they have no one length.
 */
[[nodiscard]] std::optional<std::size_t> characters_of(const std::vector<Element>& elements, const std::size_t count)
{
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
}

/**
 * @brief An element before its atom is read: no expression and no literal, nothing it admits or begins with known,
 *        never empty, of no known length and with no suffix.
 * @return The element.
 */
[[nodiscard]] Element no_element()
{
    return {.expression = {},
            .literal = std::nullopt,
            .spelling = {},
            .alphabet = std::nullopt,
            .first = std::nullopt,
            .nullable = never_empty(),
            .one_length = false,
            .characters = std::nullopt,
            .suffix = 0,
            .lazy = false};
}

/**
 * @brief Whether the body of an action provably does nothing the token stream can see: blanks and comments only, so
 *        that no call runs at all.
 *
 * Any statement in an action may reach the lexer's own state, `more()`, `skip()`, `setType()` and `setText()` among the
 * calls ANTLR's runtime offers, and a call to a member of the grammar's own `@members` block may reach them indirectly,
 * so nothing but an empty body is inert.
 * @param text The grammar's text.
 * @param begin The offset just past the action's `{`.
 * @param end The offset of its `}`.
 * @return True when the body holds nothing but blanks and comments, a `//` comment ending at a carriage return as
 *         ANTLR's lexer ends one, so an action after a bare return is code and not comment.
 */
[[nodiscard]] bool is_inert(const std::string_view text, const std::size_t begin, const std::size_t end)
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
 * @brief The element of an atom that admits an alphabet in one step: the alphabet's step for the pattern parser, of one
 *        length in bytes where it stays within ASCII.
 * @param alphabet What the atom admits.
 * @return The element.
 */
[[nodiscard]] Element alphabet_element(Alphabet alphabet)
{
    auto element{no_element()};

    element.expression = step(alphabet);

    element.one_length = alphabet.beyond.empty();

    element.alphabet = std::move(alphabet);

    return element;
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

} // namespace

Expression_reader::Expression_reader(
        const std::string_view text, const std::size_t begin, Grammar_tables& tables, std::string rule,
        const bool case_insensitive)
    : Antlr_cursor{text, begin}, tables_{tables}, rule_{std::move(rule)}, case_insensitive_{case_insensitive}
{}

std::vector<Alternative> Expression_reader::alternatives()
{
    std::vector<Alternative> read;

    // What the alternatives read so far can begin with, unknown once one of them cannot say.
    std::optional<Beginning> earlier{Beginning{}};

    // Where a non-greedy loop stood in some alternative, and whether some alternative can match the empty string, known
    // or through a reference.
    std::optional<std::size_t> lazy_at;

    std::vector<Nullable_t> formulas;

    for (;;)
    {
        skip_blanks();

        const auto opened{at_};

        auto [expression, first, characters, lazy, empty, nullable, spelling, acted]{sequence(true)};

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

            clause = at_;

            for (; peek() && *peek() != ';' && *peek() != '|'; skip_blanks())
            {
                ++at_;
            }

            commands = text_.substr(clause, at_ - clause);

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

        if (lazy_at)
        {
            for (auto& formula : formulas)
            {
                tables_.lazy_empty.push_back({.rule = rule_, .line = line_of(*lazy_at), .body = std::move(formula)});
            }
        }

        return read;
    }
}

bool Expression_reader::is_lazy() const noexcept
{
    return lazy_;
}

Sequence Expression_reader::sequence(const bool outermost)
{
    std::vector<Element> elements;

    for (skip_blanks(); peek() && *peek() != '|' && *peek() != ')' && *peek() != ';' && !at("->"); skip_blanks())
    {
        elements.push_back(element());
    }

    std::string out;

    for (std::size_t index{0}; index < elements.size(); ++index)
    {
        const auto& [expression, literal, spelling, alphabet, begins, nullable, one_length, characters, suffix, lazy]{
                elements[index]};

        if (lazy)
        {
            out += lazy_element(elements, index, outermost);

            continue;
        }

        out += suffix == 0 || is_atomic(expression) ? expression + (suffix == 0 ? "" : std::string{suffix}) :
                                                      std::format("({}){}", expression, suffix);
    }

    // The sequence matches the empty string when every element can, and an element under `?` or `*` always can.
    auto nullable{always_empty()};

    for (const auto& element : elements)
    {
        nullable = both_empty(
                nullable, element.suffix == '?' || element.suffix == '*' ? always_empty() : element.nullable);
    }

    // ANTLR's patterns for a rule spelling a parser literal match the literal alone, unsuffixed, or the literal and one
    // action after it; an inert action is the only kind read this far.
    const auto acted{elements.size() == 2 && elements.back().expression.empty()};

    const auto spelled{(elements.size() == 1 || acted) && elements.front().suffix == 0};

    // An alternative of inert actions alone matches the empty string as one of no elements does: ANTLR takes `({} |
    // 'a') 'b'` over `b`, the action-only branch matching nothing and standing.
    const auto blank{std::ranges::all_of(elements, [](const Element& element) { return element.expression.empty(); })};

    return {.expression = std::move(out),
            .first = beginning_of(elements),
            .characters = characters_of(elements, elements.size()),
            .lazy = std::ranges::any_of(elements, &Element::lazy),
            .empty = blank,
            .nullable = std::move(nullable),
            .spelling = spelled ? elements.front().spelling : std::string{},
            .acted = acted};
}

std::string Expression_reader::lazy_element(
        const std::vector<Element>& elements, const std::size_t index, const bool outermost)
{
    const auto& [expression, literal, spelling, alphabet, begins, nullable, one_length, characters, suffix, lazy]{
            elements[index]};

    lazy_ = true;

    if (!outermost)
    {
        fail("a non-greedy loop is read only in an outermost alternative of a rule, since ANTLR stops it where the "
             "rest of the whole rule matches, which reaches past the group it stands in");
    }

    const auto next{rest_spelling(elements, index + 1)};

    if (!next)
    {
        fail("a non-greedy loop is read only where the rest of the rule spells one ASCII string, which is where ANTLR "
             "stops the loop; the fewest characters that let the rest match are no regular rewrite here");
    }

    if (!characters_of(elements, index))
    {
        fail("a non-greedy loop is read only after elements whose every match has one length in characters, since "
             "ANTLR takes the paths through the elements before it in order and the first to reach the rule's end "
             "stops the loop on every later one, which the byte reading cannot express where the paths reach the loop "
             "at different characters");
    }

    const auto opener{static_cast<unsigned char>(next->front())};

    if (suffix == '?')
    {
        if (!begins || begins->ascii.test(opener))
        {
            fail("a non-greedy option before a string it could begin is not modelled");
        }

        if (!characters)
        {
            fail("a non-greedy option is read over a body whose every match has one length in characters, since ANTLR "
                 "takes the body's alternatives in order and the first to reach the rule's end stops the others, "
                 "which the byte reading cannot express where they reach the rest at different characters");
        }

        return is_atomic(expression) ? expression + "?" : std::format("({})?", expression);
    }

    if (alphabet)
    {
        // A `+?` loop reads its first character whatever follows, since it cannot stop before it has one, and the rest
        // of its body is the `*?` body from there on.
        const auto unit{is_atomic(expression) ? expression : "(" + expression + ")"};

        const auto body{avoiding(*next, *alphabet)};

        return suffix == '+' ? unit + body : body;
    }

    if (!one_length || !begins || begins->ascii.test(opener))
    {
        fail("a non-greedy loop is read over a set, a dot, one character, or a literal of one length whose first byte "
             "the rest cannot begin with; over any other body ANTLR stops it at the fewest characters that let the "
             "rest match, a group's alternatives taken in order, which the byte reading cannot express");
    }

    return is_atomic(expression) ? expression + suffix : std::format("({}){}", expression, suffix);
}

Element Expression_reader::element()
{
    const auto opened{at_};

    const auto byte{*peek()};

    if (byte == '{')
    {
        return action_element(opened);
    }

    auto [element, optionable]{atom(byte, opened)};

    // Element options after an atom that takes them are metadata on it, `'x'<a=b>`, and set nothing the reading needs;
    // after any other atom the `<` is the byte ANTLR's parser rejects, refused below as one no element begins.
    skip_blanks();

    if (optionable && peek() == '<')
    {
        ++at_;

        element_options();

        // A literal they follow is no alias spelling: ANTLR's alias pattern matches the bare literal alone, so `A :
        // 'a'<> -> skip ;` leaves the parser's 'a' an implicit token ahead of A, which antlr 4.13.2 emits.
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

    // ANTLR rejects a closure whose body can match the empty string, `('b' | )*` and a star over a nullable rule among
    // them, as its error 153. A body that reaches no rule answers here; one that reaches a rule waits for
    // finish_grammar(), since the rule it reaches may be written further down.
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

        tables_.closures.push_back({.rule = rule_, .line = line_of(opened), .body = element.nullable});
    }

    return element;
}

Element Expression_reader::action_element(const std::size_t opened)
{
    const auto body{at_ + 1};

    skip_block();

    const auto closed{at_ - 1};

    skip_blanks();

    if (peek() == '?')
    {
        at_ = opened;

        fail("a semantic predicate conditions the match on code, which a token language cannot say");
    }

    if (!is_inert(text_, body, closed))
    {
        at_ = opened;

        fail("an action inside a rule runs code that can change the token the rule produces, more() and "
             "setType() among them, which the byte reading cannot model; only blanks and comments are inert");
    }

    auto element{no_element()};

    // An inert action matches nothing at all, so it never stands in the way of an empty match.
    element.nullable = always_empty();

    element.characters = 0;

    return element;
}

Expression_reader::Atom Expression_reader::atom(const char byte, const std::size_t opened)
{
    if (byte == '\'')
    {
        return quoted_element(opened);
    }

    if (byte == '[')
    {
        return {.element = set_element(opened), .optionable = false};
    }

    if (byte == '~')
    {
        return {.element = negation_element(opened), .optionable = false};
    }

    if (byte == '.')
    {
        return {.element = dot_element(), .optionable = true};
    }

    if (byte == '(')
    {
        return {.element = group_element(opened), .optionable = false};
    }

    return {.element = reference_element(opened, byte), .optionable = true};
}

Expression_reader::Atom Expression_reader::quoted_element(const std::size_t opened)
{
    ++at_;

    const auto start{literal()};

    const std::string spelling{text_.substr(opened, at_ - opened)};

    skip_blanks();

    if (at(".."))
    {
        return {.element = range_element(opened, start, spelling), .optionable = false};
    }

    return {.element = literal_element(opened, start.bytes, spelling), .optionable = true};
}

Element Expression_reader::range_element(const std::size_t opened, const Literal& start, const std::string& spelling)
{
    const auto& [bytes, single]{start};

    const auto end{range_end()};

    if (!single)
    {
        at_ = opened;

        fail(multi_character(spelling));
    }

    const auto low{*decoded(bytes)};

    const auto high{range_high(opened, low, spelling, end)};

    refuse_unfoldable(opened, high >= 0x80);

    return alphabet_element(spanning(low, high, case_insensitive_));
}

Expression_reader::Range_end Expression_reader::range_end()
{
    at_ += 2;

    skip_blanks();

    const auto second{at_};

    expect('\'', "a quote to open the range's end");

    auto end{literal()};

    return {.end = std::move(end), .spelling = std::string{text_.substr(second, at_ - second)}};
}

char32_t Expression_reader::range_high(
        const std::size_t opened, const char32_t low, const std::string& spelling, const Range_end& end)
{
    if (!end.end.single)
    {
        at_ = opened;

        fail(multi_character(end.spelling));
    }

    const auto high{*decoded(end.end.bytes)};

    if (high < low)
    {
        at_ = opened;

        fail(empty_range(spelling + ".." + end.spelling));
    }

    return high;
}

void Expression_reader::refuse_unfoldable(const std::size_t opened, const bool beyond)
{
    if (case_insensitive_ && beyond)
    {
        at_ = opened;

        fail(std::string{unfoldable});
    }
}

Element Expression_reader::literal_element(
        const std::size_t opened, const std::string& bytes, const std::string& spelling)
{
    if (bytes.empty())
    {
        at_ = opened;

        fail("an empty literal matches nothing");
    }

    refuse_unfoldable(opened, holds_beyond_ascii(bytes));

    auto element{no_element()};

    const auto lettered{
            std::ranges::any_of(bytes, [](const char one) { return is_letter(static_cast<unsigned char>(one)); })};

    if (case_insensitive_ && lettered)
    {
        element.expression = caseless(bytes);
    }
    else
    {
        element.expression = quoted(bytes);

        element.literal = bytes;
    }

    element.spelling = spelling;

    // Every match of a literal is the literal, folded or not, so they all have its length, in bytes and in characters,
    // of which every byte but a continuation byte begins one.
    element.one_length = true;

    element.characters = static_cast<std::size_t>(std::ranges::count_if(
            bytes, [](const char one) { return !is_continuation(static_cast<unsigned char>(one)); }));

    // A literal's characters fold one by one, each as a range of itself.
    element.first = Beginning{};

    if (const auto lead{static_cast<unsigned char>(bytes.front())}; lead < 0x80)
    {
        element.first->ascii = spanning(lead, lead, case_insensitive_).ascii;
    }
    else
    {
        element.first->beyond = true;
    }

    if (const auto scalar{decoded(bytes)})
    {
        element.alphabet = spanning(*scalar, *scalar, case_insensitive_);
    }

    return element;
}

Element Expression_reader::set_element(const std::size_t opened)
{
    ++at_;

    auto alphabet{set()};

    // A set of nothing but surrogates is ANTLR's transition no UTF-8 input decodes a code point for, which its lexer
    // never takes; the byte reading has no set that never matches, so it refuses.
    if (alphabet.ascii.none() && alphabet.beyond.empty())
    {
        const std::string spelling{text_.substr(opened, at_ - opened)};

        at_ = opened;

        fail("the set " + spelling +
             " holds nothing but surrogates, which no UTF-8 input decodes to, so ANTLR's lexer never matches it");
    }

    refuse_unfoldable(opened, !alphabet.beyond.empty());

    return alphabet_element(std::move(alphabet));
}

Alphabet Expression_reader::set()
{
    const auto opened{at_ - 1};

    Alphabet alphabet;

    const auto take{[this, &alphabet](const char32_t first, const char32_t last) {
        if (!(is_surrogate(first) && is_surrogate(last)))
        {
            admit(alphabet, first, last, case_insensitive_);
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

Element Expression_reader::negation_element(const std::size_t opened)
{
    ++at_;

    skip_blanks();

    const auto negated{negatable()};

    refuse_unfoldable(opened, !negated.beyond.empty());

    return alphabet_element(complement(negated));
}

Alphabet Expression_reader::negatable()
{
    const auto opened{at_};

    if (peek() == '[')
    {
        ++at_;

        return set();
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

        // A range inside the negation, ~('0'..'9' | '^'), as Clojure's grammar writes it; a literal that is no range
        // may carry element options, `~'x'<a=b>`, which set nothing here.
        if (!at(".."))
        {
            if (peek() == '<')
            {
                ++at_;

                element_options();
            }

            return spanning(scalar, scalar, case_insensitive_);
        }

        const auto end{range_end()};

        return spanning(scalar, range_high(opened, scalar, spelling, end), case_insensitive_);
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

        const auto part{negatable()};

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

Element Expression_reader::dot_element()
{
    ++at_;

    Alphabet all{.ascii = {}, .beyond = {{.first = 0x80, .last = last_scalar}}};

    all.ascii.set();

    return alphabet_element(std::move(all));
}

Element Expression_reader::group_element(const std::size_t opened)
{
    ++at_;

    auto element{no_element()};

    std::string inner;

    auto optional{false};

    element.first = Beginning{};

    // The alternatives' lengths agree until one differs or is unknown, the first alternative setting the mark.
    auto agreed{true};

    for (auto index{0UZ};; ++index)
    {
        const auto [expression, first, characters, lazy, empty, nullable, spelling, acted]{sequence(false)};

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

        if (index == 0)
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

    return element;
}

Element Expression_reader::reference_element(const std::size_t opened, const char byte)
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

    auto element{no_element()};

    element.expression = "{" + name + "}";

    element.nullable = {{name}};

    return element;
}

} // namespace munch::tools::audit
