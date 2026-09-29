#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_PATTERN_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_PATTERN_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "munch/tools/audit/logos_regex.hpp"
#include "munch/tools/audit/rust_cursor.hpp"
#include "munch/tools/audit/scalar_set.hpp"

/**
 * @brief The regex crate's syntax, in Unicode or byte mode, read into a pattern's tree by Pattern_reader, with the
 *        Unicode version of the tables its `\d`, `\s` and `\w` are read by, unicode_classes_version().
 *
 * The tables are the ones the regex-syntax logos is locked to was generated from, which is the language a logos scanner
 * has whatever version the library itself pins, so the reading carries its own copy of them.
 */
namespace munch::tools::audit
{
/**
 * @brief The Unicode version of the tables `\d`, `\s` and `\w` are read by in Unicode mode, which a scanner's options
 *        name.
 * @return The version.
 */
[[nodiscard]] std::string_view unicode_classes_version() noexcept;

/**
 * @brief A recursive-descent reader over one pattern in the regex crate's syntax, producing the node tree.
 *
 * The grammar is the crate's: an alternation of concatenations, a concatenation of repeated atoms, an atom a group, a
 * class, the dot, an escape or a scalar. The flags travel with the reader and are saved and restored around every
 * group, so that `(?i)` reaches to the end of the group it stands in and no further, as the crate scopes it. A scalar
 * under `i` becomes the class of its cases, which is what the crate's translation makes of it and what logos therefore
 * counts. A subpattern reference becomes a Reference node carrying the flags in force, resolved once the whole pattern
 * is read.
 */
class Pattern_reader
{
public:
    /**
     * @brief Binds the reader to a pattern.
     * @param literal The pattern's literal, whose content is read and whose text names it in refusals.
     * @param flags The flags in force at the start.
     * @param line The line, for refusals.
     */
    Pattern_reader(String_literal literal, Flags flags, std::size_t line);

    /**
     * @brief Reads the whole content as a regex.
     * @return The tree.
     * @throws Spec_error If the pattern is refused.
     */
    [[nodiscard]] Node read();

    /**
     * @brief Reads the whole content as a literal, every scalar itself, which a `#[token]` is.
     * @return The tree.
     * @throws Spec_error If a non-ASCII scalar stands under `i`.
     */
    [[nodiscard]] Node read_literal();

private:
    /**
     * @brief One literal unit of a pattern: a scalar, or a byte, which `\xHH` and a byte string's bytes are outside
     *        Unicode mode; a scalar stands for its UTF-8 in either mode, as the crate has it.
     */
    struct Unit
    {
        /**
         * @brief The scalar, or the byte.
         */
        char32_t value;

        /**
         * @brief Whether the unit is a byte.
         */
        bool byte;
    };

    /**
     * @brief An alternation: concatenations separated by `|`.
     * @return The node.
     */
    [[nodiscard]] Node alternation();

    /**
     * @brief A concatenation: repetitions up to a `|`, a `)` or the end.
     * @return The node.
     */
    [[nodiscard]] Node concatenation();

    /**
     * @brief An atom under its postfix operators, a lazy marker after one refused as logos 0.15.1 refuses it.
     * @return The node.
     * @throws Spec_error For a repetition of nothing, a lazy operator, or as atom() refuses the atom.
     */
    [[nodiscard]] Node repetition();

    /**
     * @brief One atom: a group, a class, the dot, an escape or a scalar.
     * @return The node.
     * @throws Spec_error For an anchor, an operator with nothing before it, or a construct the rewriting refuses.
     */
    [[nodiscard]] Node atom();

    /**
     * @brief A group after its `(`: a capture, a non-capturing group, a flag setting, a flagged group, or a subpattern
     *        reference.
     * @return The node, Empty for a flag setting.
     * @throws Spec_error For lookaround, a flag the rewriting refuses, or an unclosed group.
     */
    [[nodiscard]] Node group();

    /**
     * @brief The content of a capture group, closed by its `)`, its outermost literal runs marked as bounded.
     * @return The node.
     * @throws Spec_error If the group is left open.
     */
    [[nodiscard]] Node captured();

    /**
     * @brief A class after its `[`, through its `]`, folded and negated as the flags and its `^` say.
     * @return The node.
     */
    [[nodiscard]] Node bracket();

    /**
     * @brief The node for a class under the flags: folded under `i`, complemented when negated, and the literal it is
     *        when one member remains.
     * @param members The members.
     * @param negated Whether the class is complemented, after folding, as the crate orders it.
     * @return The node.
     * @throws Spec_error If the class ends up empty, or folding it needs a case table the library has not got.
     */
    [[nodiscard]] Node class_node(Scalar_set members, bool negated);

    /**
     * @brief The members of a class after its `[` and optional `^`, through its `]`, ranges, escapes, POSIX classes and
     *        nested classes among them.
     * @return The members, before folding and negation.
     * @throws Spec_error For a class operator, a range ending in a class, or a range ending before it starts.
     */
    [[nodiscard]] Scalar_set members();

    /**
     * @brief An ASCII class after its `[:`, through its `:]`, negated when it opens with `^`.
     * @return The members.
     * @throws Spec_error If the name is not one of the crate's.
     */
    [[nodiscard]] Scalar_set posix_class();

    /**
     * @brief The set every class is complemented against: the scalars less the surrogates, or the bytes.
     * @return The universe.
     */
    [[nodiscard]] Scalar_set universe() const;

    /**
     * @brief A class's members folded under `i`, and themselves otherwise; what every bracket, nested ones and the
     *        ASCII classes included, is complemented from, since the crate folds before it negates.
     * @param members The members.
     * @return The members under the flags.
     */
    [[nodiscard]] Scalar_set cased(const Scalar_set& members) const;

    /**
     * @brief Refuses a byte beyond ASCII where the crate does: a `&str` pattern is parsed with the crate's UTF-8 check
     *        on, under which such a byte outside Unicode mode, alone or in a class, can match invalid UTF-8.
     * @param members The bytes a class admits, or the one byte.
     * @throws Spec_error If the pattern is a `&str` one read outside Unicode mode and a member is beyond ASCII.
     */
    void check_utf8(const Scalar_set& members) const;

    /**
     * @brief An escape after its backslash: a unit, or a class for `\d`, `\s`, `\w` and their negations, the ASCII
     *        forms under `(?-u)` and the crate's Unicode forms, Nd, White_Space and the word class, otherwise.
     * @return The unit or the class.
     * @throws Spec_error For an anchor, a `\p{...}` property class, or an escape the crate does not have.
     */
    [[nodiscard]] std::variant<Unit, Scalar_set> escape();

    /**
     * @brief A hex escape after its `\x`, `\u` or `\U`: the fixed number of digits, or any number in braces; a byte
     *        when it is the two-digit `\xHH` outside Unicode mode, a scalar otherwise.
     * @param kind The letter, which fixes the number of digits.
     * @return The unit.
     * @throws Spec_error If the digits are missing or the value is no scalar.
     */
    [[nodiscard]] Unit hex_escape(char kind);

    /**
     * @brief A unit as a member of a class: its value, which outside Unicode mode must be a byte or ASCII, since a byte
     *        class cannot hold a scalar's encoding.
     * @param unit The unit.
     * @return The member.
     * @throws Spec_error For a non-ASCII scalar in a class outside Unicode mode.
     */
    [[nodiscard]] char32_t member(Unit unit);

    /**
     * @brief Consumes the unit at the cursor: a scalar decoded from UTF-8, or a byte when the literal is a byte string.
     * @return The unit.
     * @throws Spec_error At the end.
     */
    [[nodiscard]] Unit next_unit();

    /**
     * @brief The node for one literal unit under the flags: its bytes, or the class of its cases when it is a letter
     *        under `i`.
     * @param unit The unit.
     * @return The node.
     * @throws Spec_error For a non-ASCII scalar under `i` in Unicode mode, whose folding is not modelled.
     */
    [[nodiscard]] Node unit_node(Unit unit);

    /**
     * @brief A counted repetition after its `{`, through its `}`.
     * @return The minimum and, when bounded, the maximum.
     * @throws Spec_error If the count is malformed or reversed.
     */
    [[nodiscard]] std::pair<std::size_t, std::optional<std::size_t>> count();

    /**
     * @brief The unsigned decimal at the cursor.
     * @return The number, or std::nullopt when no digit stands here.
     * @throws Spec_error If the number does not fit.
     */
    [[nodiscard]] std::optional<std::size_t> number();

    /**
     * @brief The byte at the cursor, or nothing at the end.
     * @return The byte.
     */
    [[nodiscard]] std::optional<char> peek() const noexcept;

    /**
     * @brief Whether the text at the cursor begins with the given characters.
     * @param prefix The characters.
     * @return True when it does.
     */
    [[nodiscard]] bool at(std::string_view prefix) const noexcept;

    /**
     * @brief Consumes the byte at the cursor if it is the one given.
     * @param byte The byte.
     * @return True when consumed.
     */
    [[nodiscard]] bool accept(char byte) noexcept;

    /**
     * @brief Consumes the byte given or refuses, naming what the syntax expected.
     * @param byte The byte.
     * @param what What the syntax expected.
     * @throws Spec_error If another byte, or the end, stands here.
     */
    void expect(char byte, std::string_view what);

    /**
     * @brief Consumes and returns the byte at the cursor.
     * @param what What the syntax expected, named when the pattern has ended.
     * @return The byte.
     * @throws Spec_error At the end of the pattern.
     */
    char next(std::string_view what);

    /**
     * @brief Refuses the pattern, naming it and the line.
     * @param message Why.
     */
    [[noreturn]] void fail(const std::string& message) const;

    /**
     * @brief The literal being read.
     */
    String_literal literal_;

    /**
     * @brief The flags in force.
     */
    Flags flags_;

    /**
     * @brief The line, for refusals.
     */
    std::size_t line_;

    /**
     * @brief The offset of the next byte of the content.
     */
    std::size_t at_{0};
};

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_PATTERN_HPP
