#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_REGEX_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_REGEX_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "munch/regex/indirect.hpp"
#include "munch/tools/audit/rust_cursor.hpp"
#include "munch/tools/audit/scalar_set.hpp"

/**
 * @brief A logos pattern's tree as logos and the regex crate shape it, Node, read under the crate's Flags: what the
 *        crate's alternation merging sees of a node, merged(); a class folded under `i`, folded(); a pattern's text
 *        with its subpatterns pasted in as logos pastes them, substituted(); and a pattern compiled into the parser's
 *        expression and the priority logos gives it, compile().
 *
 * The tree keeps what logos's priority tells apart, a literal from a class, and what the crate's own simplifications
 * decide, a capture's bounds among them; the refusals of what logos 0.15.1 compiles into a scanner matching nothing are
 * made over it in logos_refusals, and the regex crate's syntax is read into it by Pattern_reader in logos_pattern.
 */
namespace munch::tools::audit
{
/**
 * @brief The regex crate's flags a pattern is read under, each scoped to the group it is set in.
 */
struct Flags
{
    /**
     * @brief `i`: a letter matches either case.
     */
    bool insensitive{false};

    /**
     * @brief `s`: the dot matches the newline as well.
     */
    bool dot_all{false};

    /**
     * @brief `u`: the pattern is over scalars, encoded as UTF-8; off, it is over bytes.
     */
    bool unicode{true};

    /**
     * @brief Whether the crate's UTF-8 check is on, as it is for a `&str` pattern and off for a byte string's: on, a
     *        byte beyond ASCII outside Unicode mode, alone or in a class, is refused as able to match invalid UTF-8. It
     *        is the pattern's, not a group's, so no flag changes it.
     */
    bool utf8{true};
};

/**
 * @brief Which case folding an `ignore(...)` argument asks of logos.
 *
 * The two flags reach the pattern by different routes, which is what the reading has to follow: `ignore(case)` hands
 * the crate's own case-insensitive parse, Unicode-aware for a string pattern and ASCII-only for a byte string, while
 * `ignore(ascii_case)` parses the pattern as it stands and folds the ASCII letters of the compiled tree afterwards,
 * except for a byte string, where logos takes the same case-insensitive parse as for `ignore(case)` (logos-codegen
 * 0.15.1, parser/definition.rs and parser/ignore_flags.rs).
 */
enum class Ignore_case
{
    /**
     * @brief No ignore flag was given.
     */
    none,

    /**
     * @brief `ignore(case)`.
     */
    unicode,

    /**
     * @brief `ignore(ascii_case)`.
     */
    ascii,
};

/**
 * @brief What a pattern is to logos, which decides how it is read and what is refused of it.
 */
enum class Pattern_kind
{
    /**
     * @brief A `#[token]`, matched as it stands.
     */
    token,

    /**
     * @brief A `#[regex]` or a `skip`, a rule whose pattern is a regex.
     */
    regex,

    /**
     * @brief A `subpattern` definition, text pasted into the patterns that reference it and no rule of its own.
     */
    definition,
};

/**
 * @brief A subpattern as a reference expands it: its literal, and its content as logos pastes it into a pattern.
 */
struct Subpattern
{
    /**
     * @brief The literal, whose text is read again in the mode and under the flags of every reference, since logos
     *        substitutes the text into the referencing pattern before the crate parses it.
     */
    String_literal literal;

    /**
     * @brief The content as substituted(): its own references substituted, and every byte beyond ASCII spelled as an
     *        escape, so that the text reads the same pasted into a pattern of either mode.
     */
    std::string text;

    /**
     * @brief Whether the definition matches only the empty string, which logos allows of a definition, the patterns
     *        pasting it in being the rules, and which a reference then expands to, since regex::parse() takes no empty
     *        definition.
     */
    bool empty;
};

/**
 * @brief The subpatterns declared so far, by name.
 */
using Subpatterns_t = std::map<std::string, Subpattern, std::less<>>;

struct Node;

/**
 * @brief The empty pattern, which matches the empty string and nothing else; what `(?i)` and an empty branch leave.
 */
struct Empty
{
};

/**
 * @brief A run of literal bytes, a scalar's UTF-8 in Unicode mode and the byte itself outside it.
 */
struct Bytes
{
    /**
     * @brief The bytes.
     */
    std::string bytes;

    /**
     * @brief Whether the run opens or closes a capture group, which keeps it apart from the run beside it, as the
     *        crate's literals stay apart across a capture and logos counts them apart.
     */
    bool bounded;
};

/**
 * @brief A class: the set of scalars, or of bytes, one of which is matched.
 */
struct Char_class
{
    /**
     * @brief The members.
     */
    Scalar_set set;

    /**
     * @brief Whether the members are scalars, matched as their UTF-8, rather than bytes.
     */
    bool unicode;

    /**
     * @brief Whether a capture group encloses the class alone, which hides it from the crate's check for the dot under
     *        an unbounded repetition, the capture being what that check compares and never sees through.
     */
    bool captured{false};
};

/**
 * @brief A reference to a subpattern, `(?&name)`, with the flags in force where it stands.
 */
struct Reference
{
    /**
     * @brief The subpattern's name.
     */
    std::string name;

    /**
     * @brief The flags in force at the reference, which logos lets reach into the expansion.
     */
    Flags flags;
};

/**
 * @brief A concatenation of two or more parts.
 */
struct Concat
{
    /**
     * @brief The parts, in order.
     */
    std::vector<Node> parts;
};

/**
 * @brief An alternation of two or more branches.
 */
struct Alternation
{
    /**
     * @brief The branches, in order.
     */
    std::vector<Node> branches;

    /**
     * @brief Whether a capture group encloses the alternation alone, which keeps the class the crate merges it into,
     *        when it merges it, out of the crate's check for the dot under an unbounded repetition.
     */
    bool captured{false};
};

/**
 * @brief A repetition of an operand between a minimum and a maximum number of times.
 */
struct Repeat
{
    /**
     * @brief The operand.
     */
    regex::Indirect<Node> operand;

    /**
     * @brief The least number of times.
     */
    std::size_t min;

    /**
     * @brief The most number of times, unbounded when std::nullopt.
     */
    std::optional<std::size_t> max;
};

/**
 * @brief One node of a pattern as read: the tree the priority is computed on and the expression is written from.
 *
 * The tree keeps what logos's priority tells apart, a literal from a class, and nothing the regex crate's own
 * simplifications would erase differently: a group is its content, a single-member class the literal it is.
 */
struct Node
{
    /**
     * @brief The node's kind and content.
     */
    std::variant<Empty, Bytes, Char_class, Reference, Concat, Alternation, Repeat> kind;
};

/**
 * @brief The expression a pattern was rewritten into and the priority logos gives it.
 */
struct Compiled
{
    /**
     * @brief The expression in the syntax regex::parse() reads.
     */
    std::string expression;

    /**
     * @brief The priority, logos's own number.
     */
    std::size_t priority;
};

/**
 * @brief A node as the regex crate's alternation merging sees it: a class, or a literal of one character or one byte,
 *        with the class of that one member.
 */
struct Merged
{
    /**
     * @brief The class: the class itself, or the one member of the literal.
     */
    Char_class cls;

    /**
     * @brief Whether the node is a literal to the crate rather than a class.
     */
    bool literal;
};

/**
 * @brief The set of a class under `i`, every ASCII letter joined by its other case and, over scalars, `k` and `s` by
 *        the Kelvin sign and the long s, the crate's simple case folding on what the reader admits.
 * @param set The members.
 * @param unicode Whether the members are scalars.
 * @return The folded set.
 */
[[nodiscard]] Scalar_set folded(const Scalar_set& set, bool unicode);

/**
 * @brief A node as the regex crate's alternation merging sees it, where it sees a class or a literal of one unit.
 *
 * The crate builds an alternation whose branches are all classes into their union, and one whose branches are all
 * literals of one character, or failing that all literals of one byte, into the class of them, nested alternations
 * built first, while a branch of any other kind, a literal of several characters, a capture, an empty branch or a mix
 * of classes and literals among them, leaves the alternation as it stands (regex-syntax 0.8.11, hir/mod.rs,
 * `Hir::alternation`). A one-member class is a literal to the crate, which is what it makes of one, so an alternation
 * of literals whose union is one character stays a literal. Byte classes join a Unicode union when they hold ASCII
 * alone, and Unicode classes a byte union on the same terms, as the crate converts them. A captured class, literal or
 * alternation is not returned, since the crate keeps the capture around what it merged.
 * @param node The node.
 * @return What the crate sees, or std::nullopt when it is neither a class nor a literal of one unit.
 */
[[nodiscard]] std::optional<Merged> merged(const Node& node);

/**
 * @brief Compiles a pattern: read, its references resolved, written for the parser, and given its priority.
 * @param literal The pattern's literal.
 * @param kind What the pattern is to logos: a token, matched as it stands, a regex, or a definition.
 * @param folding Which folding an `ignore(...)` argument asked for.
 * @param subpatterns The subpatterns declared.
 * @param line The line, for refusals.
 * @return The expression and the priority logos computes, twice a token's byte length, or the tree's for a regex and
 *         for a token an ignore flag makes one, the tree being the one the crate parses from the pattern with its
 *         subpatterns substituted as text, which a `priority = n` on the attribute overrides.
 * @throws Spec_error If the pattern is refused, or a token or a regex matches only the empty string once its
 *         subpatterns are pasted in, which is no rule to logos: it panics on such a token and compiles such a regex
 *         into a rule matching no input; a definition may be empty, since only the pattern pasting it in is a rule.
 */
[[nodiscard]] Compiled compile(
        const String_literal& literal, Pattern_kind kind, Ignore_case folding, const Subpatterns_t& subpatterns,
        std::size_t line);

/**
 * @brief A pattern's content as logos pastes it into a referencing pattern, and as the crate then parses it: every
 *        `(?&name)` replaced by the subpattern's own content in a non-capturing group, and every byte beyond ASCII
 *        spelled as an escape, `\xHH` for a byte string's byte, which is the crate's spelling, and `\x{HHHH}` for a
 *        string's scalar, so that the text reads the same in a pattern of either mode.
 *
 * logos substitutes the text before the crate parses anything (logos-codegen 0.15.1, parser/subpattern.rs), so the
 * crate's merging of adjacent literals runs across a reference and a group and a flag around one reach inside it: a
 * scalar whose UTF-8 is split between a pattern and a subpattern is one scalar to the count, not two runs of bytes. A
 * reference left unclosed is kept as it stands, for the parse to refuse as the crate refuses it.
 * @param literal The pattern's literal.
 * @param subpatterns The subpatterns declared.
 * @param line The line, for refusals.
 * @return The content.
 * @throws Spec_error If a reference names no subpattern declared before it.
 */
[[nodiscard]] std::string substituted(
        const String_literal& literal, const Subpatterns_t& subpatterns, std::size_t line);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_REGEX_HPP
