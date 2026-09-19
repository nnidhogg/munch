#include "munch/tools/audit/read_logos.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "munch/regex/indirect.hpp"
#include "munch/regex/utf8.hpp"
#include "munch/tools/audit/cursor.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
using regex::utf8::Code_point_range;

// What logos's `\d`, `\s` and `\w` admit: the tables of the Unicode version that the regex-syntax logos is locked to
// was generated from. They are part of the token set a logos scanner has, whatever version the library itself pins,
// so the audit carries its own copy of them and tools/unicode/generate_classes.py generates both.
#include "logos_class_ranges.inc"

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
     *        byte beyond ASCII outside Unicode mode, alone or in a class, is refused as able to match invalid UTF-8.
     *        It is the pattern's, not a group's, so no flag changes it.
     */
    bool utf8{true};
};

/**
 * @brief A Rust string literal: the text exactly as written, its content decoded, and whether it is a byte string.
 */
struct Literal
{
    /**
     * @brief The literal as it stands in the source, prefix and quotes included.
     */
    std::string written;

    /**
     * @brief The content, the escapes of a plain string decoded and a raw string's taken verbatim.
     */
    std::string bytes;

    /**
     * @brief Whether the literal is a byte string, `b"..."`, whose pattern is over bytes.
     */
    bool byte_string;
};

/**
 * @brief One `#[path(...)]` attribute: its path, where the text between its delimiters lies, and its line.
 */
struct Attribute
{
    /**
     * @brief The path, `derive`, `logos`, `token`, `regex` or another.
     */
    std::string path;

    /**
     * @brief The offset of the first byte inside the delimiters, equal to `end` when there are none.
     */
    std::size_t begin;

    /**
     * @brief The offset just past the last byte inside the delimiters.
     */
    std::size_t end;

    /**
     * @brief Whether the path is followed by a delimited group, `#[path(...)]`, rather than nothing or `= value`.
     */
    bool delimited;

    /**
     * @brief The line the attribute opens on.
     */
    std::size_t line;
};

/**
 * @brief Which case folding an `ignore(...)` argument asks of logos.
 *
 * The two flags reach the pattern by different routes, which is what the reading has to follow: `ignore(case)` hands
 * the crate's own case-insensitive parse, Unicode-aware for a string pattern and ASCII-only for a byte string, while
 * `ignore(ascii_case)` parses the pattern as it stands and folds the ASCII letters of the compiled tree afterwards,
 * except for a byte string, where logos takes the same case-insensitive parse as for `ignore(case)`
 * (logos-codegen 0.15.1, parser/definition.rs and parser/ignore_flags.rs).
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
 * @brief What a `#[token(...)]`, a `#[regex(...)]` or a `skip(...)` says: the literal and the arguments after it.
 */
struct Definition
{
    /**
     * @brief The pattern's literal.
     */
    Literal literal;

    /**
     * @brief The callback's text as written, empty when there is none.
     */
    std::string callback;

    /**
     * @brief The `priority = n` override, when given.
     */
    std::optional<std::size_t> priority;

    /**
     * @brief Which folding an `ignore(...)` argument asked for.
     */
    Ignore_case folding;

    /**
     * @brief The line the attribute opens on.
     */
    std::size_t line;
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
    Literal literal;

    /**
     * @brief The content as substituted(): its own references substituted, and every byte beyond ASCII spelled as an
     *        escape, so that the text reads the same pasted into a pattern of either mode.
     */
    std::string text;

    /**
     * @brief Whether the definition matches only the empty string, which logos allows of a definition, the patterns
     *        pasting it in being the rules, and which a reference then expands to, since regex::parse() takes no
     *        empty definition.
     */
    bool empty;
};

/**
 * @brief The subpatterns declared so far, by name.
 */
using Subpatterns_t = std::map<std::string, Subpattern, std::less<>>;

/**
 * @brief Inclusive byte ranges, the members of a bracket expression or the positions of a UTF-8 sequence.
 */
using Byte_ranges_t = std::vector<std::pair<unsigned char, unsigned char>>;

/**
 * @brief The last scalar, the ceiling of a hex escape and of every class over scalars.
 */
constexpr char32_t last_scalar{0x10FFFF};

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
 * @brief A set of scalars, or of bytes when the pattern is over bytes, as sorted disjoint ranges.
 */
class Scalar_set
{
public:
    /**
     * @brief An inclusive range.
     */
    using Range_t = std::pair<char32_t, char32_t>;

    /**
     * @brief Adds a range, merging it with the ranges it touches.
     * @param low The first member.
     * @param high The last member, at least the first.
     */
    void add(char32_t low, char32_t high);

    /**
     * @brief Adds every member of another set.
     * @param other The set.
     */
    void add(const Scalar_set& other);

    /**
     * @brief The members of this set that are not in another.
     * @param other The set to remove.
     * @return The difference.
     */
    [[nodiscard]] Scalar_set minus(const Scalar_set& other) const;

    /**
     * @brief The ranges, ascending and disjoint.
     * @return The ranges.
     */
    [[nodiscard]] const std::vector<Range_t>& ranges() const noexcept;

    /**
     * @brief Whether a value is a member.
     * @param value The value.
     * @return True when it is.
     */
    [[nodiscard]] bool contains(char32_t value) const noexcept;

    /**
     * @brief Whether the set has no member.
     * @return True when empty.
     */
    [[nodiscard]] bool empty() const noexcept;

    /**
     * @brief The one member, when there is exactly one.
     * @return The member, or std::nullopt.
     */
    [[nodiscard]] std::optional<char32_t> single() const noexcept;

    /**
     * @brief Whether the regex crate keeps a range of this set running across the surrogate gap, from below U+D800
     *        to above U+DFFF.
     *
     * The crate's classes hold their ranges as the crate built them, and it never merges a range ending at U+D7FF
     * with one beginning at U+E000, so a class admitting every scalar is the dot to it, one range, only when made
     * from a range across the gap: the dot itself, a negation, or a range written across it, while
     * `[\x00-\x{D7FF}\x{E000}-\x{10FFFF}]` stays two ranges. The flag follows the crate's construction: a range added
     * across the gap sets it, a union keeps it from either side, and a difference keeps it unless the removed set
     * holds U+D7FF or U+E000, which is where the crate's negation puts a boundary.
     * @return True when one does.
     */
    [[nodiscard]] bool spans_gap() const noexcept;

private:
    /**
     * @brief Whether a range runs across the surrogate gap as the crate holds the set.
     */
    bool spans_gap_{false};

    /**
     * @brief The ranges, kept ascending, disjoint and apart.
     */
    std::vector<Range_t> ranges_;
};

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
struct Class
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
     * @brief Whether a capture group encloses the class alone, which hides it from the crate's check for the dot
     *        under an unbounded repetition, the capture being what that check compares and never sees through.
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
struct Choice
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
    std::variant<Empty, Bytes, Class, Reference, Concat, Choice, Repeat> kind;
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
 * @brief A cursor over a stretch of Rust source, reading it token by token as far as attributes and enums need.
 *
 * The cursor knows Rust's lexical shapes well enough never to be misled by them: comments of both styles, block
 * comments nesting, string literals in their plain, raw and byte forms, character literals as against lifetimes,
 * and the three kinds of delimited group. A cursor may be bounded to a span of the text, an attribute's content,
 * while still counting lines from the file's start.
 */
class Rust_cursor : public Cursor
{
public:
    /**
     * @brief Binds the cursor to a span of the text.
     * @param text The whole file.
     * @param begin The offset the cursor starts at.
     * @param end The offset the span ends at.
     */
    Rust_cursor(std::string_view text, std::size_t begin, std::size_t end);

    /**
     * @brief Skips blanks and comments.
     * @throws Spec_error If a block comment is never closed.
     */
    void skip_trivia();

    /**
     * @brief Skips one token: a comment, a string or character literal, a delimited group with everything in it,
     *        a word, or a single byte.
     * @throws Spec_error If a literal or a group is left open.
     */
    void skip_token();

    /**
     * @brief Skips the delimited group opening at the cursor, through its close.
     * @throws Spec_error If the group is left open or closed by the wrong delimiter.
     */
    void skip_group();

    /**
     * @brief Consumes the word at the cursor, letters, digits, underscores and non-ASCII bytes, a raw identifier's
     *        `r#` prefix included.
     * @return The word, empty when none stands here.
     */
    [[nodiscard]] std::string_view word();

    /**
     * @brief Consumes and decodes the string literal at the cursor.
     * @return The literal.
     * @throws Spec_error If no string literal stands here, it is left open, or an escape is not Rust's.
     */
    [[nodiscard]] Literal literal();

    /**
     * @brief A cursor over a span inside this one, counting lines from the same file start.
     * @param begin The span's first offset.
     * @param end The offset the span ends at.
     * @return The cursor.
     */
    [[nodiscard]] Rust_cursor inside(std::size_t begin, std::size_t end) const noexcept;

    /**
     * @brief The text between two offsets.
     * @param begin The first offset.
     * @param end The offset past the last.
     * @return The text.
     */
    [[nodiscard]] std::string_view slice(std::size_t begin, std::size_t end) const noexcept;

    /**
     * @brief Whether a string literal, in any of its prefixed forms, opens at the cursor.
     * @return True when one does.
     * @throws Spec_error If it is left open.
     */
    [[nodiscard]] bool at_string() const;

private:
    /**
     * @brief The offset just past the string literal at the cursor, its prefix, hashes and escapes honoured.
     * @return The offset, or std::nullopt when no string literal opens here.
     * @throws Spec_error If the literal is left open.
     */
    [[nodiscard]] std::optional<std::size_t> string_end() const;
};

/**
 * @brief A recursive-descent reader over one pattern in the regex crate's syntax, producing the node tree.
 *
 * The grammar is the crate's: an alternation of concatenations, a concatenation of repeated atoms, an atom a group,
 * a class, the dot, an escape or a scalar. The flags travel with the reader and are saved and restored around every
 * group, so that `(?i)` reaches to the end of the group it stands in and no further, as the crate scopes it. A
 * scalar under `i` becomes the class of its cases, which is what the crate's translation makes of it and what logos
 * therefore counts. A subpattern reference becomes a Reference node carrying the flags in force, resolved once the
 * whole pattern is read.
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
    Pattern_reader(Literal literal, Flags flags, std::size_t line);

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
     */
    [[nodiscard]] Node repetition();

    /**
     * @brief One atom: a group, a class, the dot, an escape or a scalar.
     * @return The node.
     * @throws Spec_error For an anchor, an operator with nothing before it, or a construct the rewriting refuses.
     */
    [[nodiscard]] Node atom();

    /**
     * @brief A group after its `(`: a capture, a non-capturing group, a flag setting, a flagged group, or a
     *        subpattern reference.
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
     * @brief The members of a class after its `[` and optional `^`, through its `]`, ranges, escapes, POSIX
     *        classes and nested classes among them.
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
     * @brief An escape after its backslash: a unit, or a class for `\d`, `\s`, `\w` and their negations, the ASCII
     *        forms under `(?-u)` and the crate's Unicode forms, Nd, White_Space and the word class, otherwise.
     * @return The unit or the class.
     * @throws Spec_error For an anchor, a `\p{...}` property class, or an escape the crate does not have.
     */
    [[nodiscard]] std::variant<Unit, Scalar_set> escape();

    /**
     * @brief A hex escape after its `\x`, `\u` or `\U`: the fixed number of digits, or any number in braces; a
     *        byte when it is the two-digit `\xHH` outside Unicode mode, a scalar otherwise.
     * @param kind The letter, which fixes the number of digits.
     * @return The unit.
     * @throws Spec_error If the digits are missing or the value is no scalar.
     */
    [[nodiscard]] Unit hex_escape(char kind);

    /**
     * @brief A unit as a member of a class: its value, which outside Unicode mode must be a byte or ASCII, since a
     *        byte class cannot hold a scalar's encoding.
     * @param unit The unit.
     * @return The member.
     * @throws Spec_error For a non-ASCII scalar in a class outside Unicode mode.
     */
    [[nodiscard]] char32_t member(Unit unit);

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
     * @brief The node for one literal unit under the flags: its bytes, or the class of its cases when it is a letter
     *        under `i`.
     * @param unit The unit.
     * @return The node.
     * @throws Spec_error For a non-ASCII scalar under `i` in Unicode mode, whose folding is not modelled.
     */
    [[nodiscard]] Node unit_node(Unit unit);

    /**
     * @brief The node for a class under the flags: folded under `i`, complemented when negated, and the literal it
     *        is when one member remains.
     * @param members The members.
     * @param negated Whether the class is complemented, after folding, as the crate orders it.
     * @return The node.
     * @throws Spec_error If the class ends up empty, or folding it needs a case table the library has not got.
     */
    [[nodiscard]] Node class_node(Scalar_set members, bool negated);

    /**
     * @brief A class's members folded under `i`, and themselves otherwise; what every bracket, nested ones and the
     *        ASCII classes included, is complemented from, since the crate folds before it negates.
     * @param members The members.
     * @return The members under the flags.
     */
    [[nodiscard]] Scalar_set cased(const Scalar_set& members) const;

    /**
     * @brief The set every class is complemented against: the scalars less the surrogates, or the bytes.
     * @return The universe.
     */
    [[nodiscard]] Scalar_set universe() const;

    /**
     * @brief Refuses a byte beyond ASCII where the crate does: a `&str` pattern is parsed with the crate's UTF-8
     *        check on, under which such a byte outside Unicode mode, alone or in a class, can match invalid UTF-8.
     * @param members The bytes a class admits, or the one byte.
     * @throws Spec_error If the pattern is a `&str` one read outside Unicode mode and a member is beyond ASCII.
     */
    void check_utf8(const Scalar_set& members) const;

    /**
     * @brief Consumes the unit at the cursor: a scalar decoded from UTF-8, or a byte when the literal is a byte
     *        string.
     * @return The unit.
     * @throws Spec_error At the end.
     */
    [[nodiscard]] Unit next_unit();

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
    Literal literal_;

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

/**
 * @brief Whether a byte is a decimal digit.
 * @param byte The byte.
 * @return True for 0 to 9.
 */
[[nodiscard]] constexpr bool is_digit(const char byte) noexcept
{
    return byte >= '0' && byte <= '9';
}

/**
 * @brief Whether a byte can begin or continue a Rust word: a letter, a digit, an underscore or a non-ASCII byte.
 * @param byte The byte.
 * @return True when it can.
 */
[[nodiscard]] constexpr bool is_word_byte(const char byte) noexcept
{
    return is_digit(byte) || is_letter(static_cast<unsigned char>(byte)) || byte == '_' ||
           static_cast<unsigned char>(byte) >= 0x80;
}

/**
 * @brief Whether a byte is a blank.
 * @param byte The byte.
 * @return True for a space, a tab, a newline or a carriage return.
 */
[[nodiscard]] constexpr bool is_blank(const char byte) noexcept
{
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/**
 * @brief The value of a hexadecimal digit.
 * @param byte The digit.
 * @return Its value.
 */
[[nodiscard]] constexpr unsigned hex_value(const char byte) noexcept
{
    return static_cast<unsigned>(is_digit(byte) ? byte - '0' : (byte | 0x20) - 'a' + 10);
}

/**
 * @brief The number of scalars a run of bytes encodes, when it is well-formed UTF-8 as strictly as Rust reads it.
 *
 * logos takes a literal's priority from `std::str::from_utf8` and falls back to the byte length where that fails, so
 * the validation has to be that one: the shortest form only, no encoding of a surrogate and nothing above U+10FFFF,
 * which the second byte's narrower range after a lead of E0, ED, F0 or F4 is what rules out. A byte string that only
 * looks like UTF-8, `ED A0 80` among them, counts as its bytes and scores twice as much.
 * @param bytes The run.
 * @return The count, or std::nullopt when the run is not UTF-8.
 */
[[nodiscard]] std::optional<std::size_t> scalar_count(const std::string_view bytes) noexcept
{
    std::size_t count{0};

    for (std::size_t at{0}; at < bytes.size(); ++count)
    {
        const auto lead{static_cast<unsigned char>(bytes[at])};

        const auto length{
                lead < 0x80                  ? 1UZ :
                lead >= 0xC2 && lead < 0xE0  ? 2UZ :
                lead >= 0xE0 && lead < 0xF0  ? 3UZ :
                lead >= 0xF0 && lead <= 0xF4 ? 4UZ :
                                               0UZ};

        if (length == 0 || at + length > bytes.size())
        {
            return std::nullopt;
        }

        for (std::size_t inner{1}; inner < length; ++inner)
        {
            const auto byte{static_cast<unsigned>(static_cast<unsigned char>(bytes[at + inner]))};

            const auto low{inner > 1 ? 0x80U : lead == 0xE0 ? 0xA0U : lead == 0xF0 ? 0x90U : 0x80U};

            const auto high{inner > 1 ? 0xBFU : lead == 0xED ? 0x9FU : lead == 0xF4 ? 0x8FU : 0xBFU};

            if (byte < low || byte > high)
            {
                return std::nullopt;
            }
        }

        at += length;
    }

    return count;
}

/**
 * @brief The scalar a run of bytes encodes.
 * @param bytes The run, the UTF-8 of exactly one scalar as scalar_count() validates it.
 * @return The scalar.
 */
[[nodiscard]] char32_t decoded(const std::string_view bytes) noexcept
{
    const auto lead{static_cast<unsigned char>(bytes.front())};

    if (lead < 0x80)
    {
        return lead;
    }

    char32_t value{lead & (0xFFU >> (bytes.size() + 1))};

    for (const char byte : bytes.substr(1))
    {
        value = (value << 6U) | (static_cast<unsigned char>(byte) & 0x3FU);
    }

    return value;
}

/**
 * @brief The set of a class under `i`, every ASCII letter joined by its other case and, over scalars, `k` and `s`
 *        by the Kelvin sign and the long s, the crate's simple case folding on what the reader admits.
 * @param set The members.
 * @param unicode Whether the members are scalars.
 * @return The folded set.
 */
[[nodiscard]] Scalar_set folded(const Scalar_set& set, const bool unicode)
{
    constexpr char32_t long_s{0x17F};

    constexpr char32_t kelvin{0x212A};

    auto result{set};

    for (char32_t lower{'a'}; lower <= 'z'; ++lower)
    {
        const auto upper{lower - 0x20};

        if (set.contains(lower) || set.contains(upper))
        {
            result.add(lower, lower);

            result.add(upper, upper);
        }
    }

    if (unicode && (result.contains('s') || set.contains(long_s)))
    {
        result.add('s', 's');
        result.add('S', 'S');
        result.add(long_s, long_s);
    }

    if (unicode && (result.contains('k') || set.contains(kelvin)))
    {
        result.add('k', 'k');
        result.add('K', 'K');
        result.add(kelvin, kelvin);
    }

    return result;
}

/**
 * @brief Byte ranges as the members of a bracket, a run of three or more as a range.
 * @param ranges The ranges, ascending.
 * @return The members' text, without the brackets.
 */
[[nodiscard]] std::string members(const Byte_ranges_t& ranges)
{
    std::string text;

    for (const auto& [low, high] : ranges)
    {
        text += bracket_member(low);

        if (high > low + 1)
        {
            text += '-';
        }

        if (high > low)
        {
            text += bracket_member(high);
        }
    }

    return text;
}

/**
 * @brief A class in the syntax regex::parse() reads: over bytes, a bracket of its byte ranges; over scalars, a
 *        bracket of its ASCII members and its other ranges as code point escapes, which the parser reads as the
 *        encodings of the scalars they span.
 * @param cls The class.
 * @return The bracket.
 */
[[nodiscard]] std::string written(const Class& cls)
{
    Byte_ranges_t direct;

    std::string beyond;

    const char32_t ceiling{cls.unicode ? 0x7FU : 0xFFU};

    for (const auto& [low, high] : cls.set.ranges())
    {
        if (low <= ceiling)
        {
            direct.emplace_back(static_cast<unsigned char>(low), static_cast<unsigned char>(std::min(high, ceiling)));
        }

        if (high > ceiling)
        {
            const auto from{static_cast<std::uint32_t>(std::max(low, static_cast<char32_t>(ceiling + 1)))};

            beyond += from == high ? std::format(R"(\u{{{:x}}})", from) :
                                     std::format(R"(\u{{{:x}}}-\u{{{:x}}})", from, static_cast<std::uint32_t>(high));
        }
    }

    return '[' + members(direct) + beyond + ']';
}

/**
 * @brief A node in the syntax regex::parse() reads.
 * @param node The node.
 * @param top Whether the node is the whole pattern, which an alternation needs no parentheses around.
 * @return The text.
 */
[[nodiscard]] std::string written(const Node& node, const bool top)
{
    return std::visit(
            [top]<typename Kind>(const Kind& kind) -> std::string {
                if constexpr (std::is_same_v<Kind, Empty>)
                {
                    return {};
                }
                else if constexpr (std::is_same_v<Kind, Bytes>)
                {
                    return quoted(kind.bytes);
                }
                else if constexpr (std::is_same_v<Kind, Class>)
                {
                    return written(kind);
                }
                else if constexpr (std::is_same_v<Kind, Reference>)
                {
                    return '{' + kind.name + '}';
                }
                else if constexpr (std::is_same_v<Kind, Concat>)
                {
                    std::string text;

                    for (const auto& part : kind.parts)
                    {
                        text += written(part, false);
                    }

                    return text;
                }
                else if constexpr (std::is_same_v<Kind, Choice>)
                {
                    std::string text{top ? "" : "("};

                    for (std::size_t index{0}; index < kind.branches.size(); ++index)
                    {
                        text += (index == 0 ? "" : "|") + written(kind.branches[index], false);
                    }

                    return top ? text : text + ')';
                }
                else
                {
                    static_assert(std::is_same_v<Kind, Repeat>);

                    const auto& operand{*kind.operand};

                    const auto grouped{
                            std::holds_alternative<Concat>(operand.kind) ||
                            std::holds_alternative<Repeat>(operand.kind)};

                    const auto text{grouped ? '(' + written(operand, false) + ')' : written(operand, false)};

                    if (!kind.max)
                    {
                        return kind.min == 0 ? text + '*' :
                               kind.min == 1 ? text + '+' :
                                               text + '{' + std::to_string(kind.min) + ",}";
                    }

                    if (kind.min == 0 && *kind.max == 1)
                    {
                        return text + '?';
                    }

                    return text + '{' + std::to_string(kind.min) +
                           (*kind.max == kind.min ? "" : ',' + std::to_string(*kind.max)) + '}';
                }
            },
            node.kind);
}

/**
 * @brief The priority logos computes for a node: two per scalar of a literal, two per byte when the run is not
 *        UTF-8; two for a class; the sum over a concatenation; the least over an alternation; a repetition's
 *        operand its minimum number of times.
 *
 * A reference is not expected here, the caller having read the pattern with every subpattern substituted as logos
 * substitutes it, and counts nothing.
 * @param node The node.
 * @return The priority.
 */
[[nodiscard]] std::size_t priority(const Node& node)
{
    return std::visit(
            []<typename Kind>(const Kind& kind) -> std::size_t {
                if constexpr (std::is_same_v<Kind, Empty> || std::is_same_v<Kind, Reference>)
                {
                    return 0;
                }
                else if constexpr (std::is_same_v<Kind, Bytes>)
                {
                    return 2 * scalar_count(kind.bytes).value_or(kind.bytes.size());
                }
                else if constexpr (std::is_same_v<Kind, Class>)
                {
                    return 2;
                }
                else if constexpr (std::is_same_v<Kind, Concat>)
                {
                    std::size_t sum{0};

                    for (const auto& part : kind.parts)
                    {
                        sum += priority(part);
                    }

                    return sum;
                }
                else if constexpr (std::is_same_v<Kind, Choice>)
                {
                    auto least{std::numeric_limits<std::size_t>::max()};

                    for (const auto& branch : kind.branches)
                    {
                        least = std::min(least, priority(branch));
                    }

                    return least;
                }
                else
                {
                    static_assert(std::is_same_v<Kind, Repeat>);

                    return kind.min * priority(*kind.operand);
                }
            },
            node.kind);
}

/**
 * @brief A concatenation of parts, flattened, the empty parts dropped and adjacent literal runs merged unless a
 *        capture bounds one, which is how the crate builds one.
 * @param parts The parts.
 * @return The node: Empty for none, the part for one, a Concat otherwise.
 */
[[nodiscard]] Node concatenated(std::vector<Node> parts)
{
    std::vector<Node> flat;

    for (auto& part : parts)
    {
        if (std::holds_alternative<Concat>(part.kind))
        {
            std::ranges::move(std::get<Concat>(part.kind).parts, std::back_inserter(flat));
        }
        else if (!std::holds_alternative<Empty>(part.kind))
        {
            flat.push_back(std::move(part));
        }
    }

    std::vector<Node> merged;

    for (auto& part : flat)
    {
        const auto joins{
                !merged.empty() && std::holds_alternative<Bytes>(merged.back().kind) &&
                std::holds_alternative<Bytes>(part.kind) && !std::get<Bytes>(merged.back().kind).bounded &&
                !std::get<Bytes>(part.kind).bounded};

        if (joins)
        {
            std::get<Bytes>(merged.back().kind).bytes += std::get<Bytes>(part.kind).bytes;
        }
        else
        {
            merged.push_back(std::move(part));
        }
    }

    if (merged.empty())
    {
        return {.kind = Empty{}};
    }

    if (merged.size() == 1)
    {
        return std::move(merged.front());
    }

    return {.kind = Concat{.parts = std::move(merged)}};
}

/**
 * @brief A node as the regex crate's alternation merging sees it: a class, or a literal of one character or one byte,
 *        with the class of that one member.
 */
struct Merged
{
    /**
     * @brief The class: the class itself, or the one member of the literal.
     */
    Class cls;

    /**
     * @brief Whether the node is a literal to the crate rather than a class.
     */
    bool literal;
};

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
[[nodiscard]] std::optional<Merged> merged(const Node& node)
{
    if (std::holds_alternative<Class>(node.kind))
    {
        const auto& given{std::get<Class>(node.kind)};

        return given.captured ? std::nullopt : std::optional{Merged{.cls = given, .literal = false}};
    }

    // A literal of one character, in either mode, is a one-character literal to the crate's merging, and a lone byte
    // beyond ASCII a one-byte literal; a literal a capture bounds is not a literal to it.
    if (std::holds_alternative<Bytes>(node.kind))
    {
        const auto& [bytes, bounded]{std::get<Bytes>(node.kind)};

        if (bounded)
        {
            return std::nullopt;
        }

        Scalar_set one;

        if (scalar_count(bytes) == 1)
        {
            one.add(decoded(bytes), decoded(bytes));

            return Merged{.cls = {.set = std::move(one), .unicode = true, .captured = false}, .literal = true};
        }

        if (bytes.size() == 1)
        {
            one.add(static_cast<unsigned char>(bytes.front()), static_cast<unsigned char>(bytes.front()));

            return Merged{.cls = {.set = std::move(one), .unicode = false, .captured = false}, .literal = true};
        }

        return std::nullopt;
    }

    if (!std::holds_alternative<Choice>(node.kind) || std::get<Choice>(node.kind).captured)
    {
        return std::nullopt;
    }

    std::vector<Merged> branches;

    for (const auto& branch : std::get<Choice>(node.kind).branches)
    {
        auto seen{merged(branch)};

        // All classes or all literals; a mix stays an alternation.
        if (!seen || (!branches.empty() && seen->literal != branches.front().literal))
        {
            return std::nullopt;
        }

        branches.push_back(std::move(*seen));
    }

    // Unicode first: every byte class must be ASCII; then bytes: every Unicode class must be.
    const auto ascii_only{
            [](const Merged& one) { return one.cls.set.empty() || one.cls.set.ranges().back().second <= 0x7F; }};

    for (const auto unicode : {true, false})
    {
        if (std::ranges::all_of(
                    branches, [&](const Merged& one) { return one.cls.unicode == unicode || ascii_only(one); }))
        {
            Merged whole{.cls = {.set = {}, .unicode = unicode, .captured = false}, .literal = false};

            for (const auto& one : branches)
            {
                whole.cls.set.add(one.cls.set);
            }

            whole.literal = whole.cls.set.single().has_value();

            return whole;
        }
    }

    return std::nullopt;
}

/**
 * @brief An alternation of branches, flattened as the crate flattens it, an empty branch making the rest optional,
 *        which says the same.
 *
 * The crate builds an alternation bottom up, so a nested one it has merged into a class, or wrapped in a capture, is
 * one branch to the outer alternation, while any other nested alternation is flattened into it (regex-syntax 0.8.11,
 * hir/mod.rs, `Hir::alternation`); which is which decides whether the outer one merges in turn.
 * @param branches The branches.
 * @return The node: Empty for none, the branch for one, a Choice otherwise, under `?` when a branch was empty.
 */
[[nodiscard]] Node alternated(std::vector<Node> branches)
{
    std::vector<Node> flat;

    auto nullable{false};

    for (auto& branch : branches)
    {
        if (std::holds_alternative<Choice>(branch.kind) && !std::get<Choice>(branch.kind).captured && !merged(branch))
        {
            std::ranges::move(std::get<Choice>(branch.kind).branches, std::back_inserter(flat));
        }
        else if (std::holds_alternative<Empty>(branch.kind))
        {
            nullable = true;
        }
        else
        {
            flat.push_back(std::move(branch));
        }
    }

    if (flat.empty())
    {
        return {.kind = Empty{}};
    }

    Node node{flat.size() == 1 ? std::move(flat.front()) : Node{.kind = Choice{.branches = std::move(flat)}}};

    if (!nullable)
    {
        return node;
    }

    return {.kind = Repeat{.operand = regex::Indirect<Node>{std::move(node)}, .min = 0, .max = 1}};
}

/**
 * @brief A repetition of an operand, the forms that repeat nothing reduced.
 * @param operand The operand.
 * @param min The least number of times.
 * @param max The most, unbounded when std::nullopt.
 * @return The node: Empty when the operand is or the maximum is zero, the operand for exactly once, a Repeat else.
 */
[[nodiscard]] Node repeated(Node operand, const std::size_t min, const std::optional<std::size_t> max)
{
    if (std::holds_alternative<Empty>(operand.kind) || max == 0)
    {
        return {.kind = Empty{}};
    }

    if (min == 1 && max == 1)
    {
        return operand;
    }

    return {.kind = Repeat{.operand = regex::Indirect<Node>{std::move(operand)}, .min = min, .max = max}};
}

/**
 * @brief Resolves every subpattern reference in a tree: one in the subpattern's own mode under the default flags keeps
 *        its name, one under `i` or `s` or in the other mode, or every one where the caller asks, is replaced by the
 *        subpattern read again under those flags and in that mode, as logos's textual substitution lets the flags and
 *        the mode reach in.
 * @param node The tree.
 * @param subpatterns The subpatterns declared.
 * @param line The line, for refusals.
 * @param expand Whether every reference is replaced by its subpattern, which a folding over the whole pattern needs,
 *        logos having substituted the text before it compiled anything.
 * @throws Spec_error If a reference names no subpattern declared before it.
 */
void resolve(Node& node, const Subpatterns_t& subpatterns, const std::size_t line, const bool expand)
{
    if (std::holds_alternative<Reference>(node.kind))
    {
        const auto& [name, reference_flags]{std::get<Reference>(node.kind)};

        const auto found{subpatterns.find(name)};

        if (found == subpatterns.end())
        {
            throw Spec_error{"the subpattern '" + name + "' is not declared before its use", line};
        }

        const auto& subpattern{found->second};

        // The definition was compiled in its own mode, so the reference can stand for it only where that mode is
        // the one in force and no flag is; an empty definition is expanded to the nothing it adds.
        if (!expand && !subpattern.empty && !reference_flags.insensitive && !reference_flags.dot_all &&
            reference_flags.unicode == !subpattern.literal.byte_string)
        {
            return;
        }

        // logos substitutes the definition's text, a byte string's bytes beyond ASCII spelled `\xHH`, into the
        // pattern before the crate parses it, so the definition is read under the flags and in the mode of the
        // reference, a byte string's `\xHH` the scalar U+00HH inside Unicode mode and a string's scalar its UTF-8
        // outside it (logos-codegen 0.15.1, parser/subpattern.rs and parser/definition.rs). The reference is gone
        // once the node is replaced; nothing of it is read after this.
        node = Pattern_reader{subpattern.literal, reference_flags, line}.read();

        resolve(node, subpatterns, line, expand);

        return;
    }

    std::visit(
            [&]<typename Kind>(Kind& kind) {
                if constexpr (std::is_same_v<Kind, Concat>)
                {
                    for (auto& part : kind.parts)
                    {
                        resolve(part, subpatterns, line, expand);
                    }
                }
                else if constexpr (std::is_same_v<Kind, Choice>)
                {
                    for (auto& branch : kind.branches)
                    {
                        resolve(branch, subpatterns, line, expand);
                    }
                }
                else if constexpr (std::is_same_v<Kind, Repeat>)
                {
                    resolve(*kind.operand, subpatterns, line, expand);
                }
            },
            node.kind);
}

/**
 * @brief A pattern's content as logos pastes it into a referencing pattern, and as the crate then parses it: every
 *        `(?&name)` replaced by the subpattern's own content in a non-capturing group, and every byte beyond ASCII
 *        spelled as an escape, `\xHH` for a byte string's byte, which is the crate's spelling, and `\x{HHHH}` for a
 *        string's scalar, so that the text reads the same in a pattern of either mode.
 *
 * logos substitutes the text before the crate parses anything (logos-codegen 0.15.1, parser/subpattern.rs), so the
 * crate's merging of adjacent literals runs across a reference and a group and a flag around one reach inside it: a
 * scalar whose UTF-8 is split between a pattern and a subpattern is one scalar to the count, not two runs of bytes.
 * A reference left unclosed is kept as it stands, for the parse to refuse as the crate refuses it.
 * @param literal The pattern's literal.
 * @param subpatterns The subpatterns declared.
 * @param line The line, for refusals.
 * @return The content.
 * @throws Spec_error If a reference names no subpattern declared before it.
 */
[[nodiscard]] std::string substituted(const Literal& literal, const Subpatterns_t& subpatterns, const std::size_t line)
{
    std::string text;

    const auto& bytes{literal.bytes};

    for (std::size_t at{0}; at < bytes.size();)
    {
        const auto close{bytes.compare(at, 3, "(?&") == 0 ? bytes.find(')', at + 3) : std::string::npos};

        if (close != std::string::npos)
        {
            const auto name{bytes.substr(at + 3, close - at - 3)};

            const auto found{subpatterns.find(name)};

            if (found == subpatterns.end())
            {
                throw Spec_error{"the subpattern '" + name + "' is not declared before its use", line};
            }

            text += "(?:" + found->second.text + ")";

            at = close + 1;

            continue;
        }

        const auto lead{static_cast<unsigned char>(bytes[at])};

        if (lead < 0x80)
        {
            text.push_back(bytes[at++]);
        }
        else if (literal.byte_string)
        {
            text += std::format(R"(\x{:02x})", static_cast<unsigned>(lead));

            ++at;
        }
        else
        {
            // A string literal is UTF-8, so the lead byte gives the scalar's length.
            const auto length{lead < 0xE0 ? 2UZ : lead < 0xF0 ? 3UZ : 4UZ};

            text += std::format(R"(\x{{{:x}}})", static_cast<std::uint32_t>(decoded(bytes.substr(at, length))));

            at += length;
        }
    }

    return text;
}

/**
 * @brief A literal as logos escapes it for the regex crate, which is what it compiles a token under an ignore flag
 *        from.
 *
 * A byte string's bytes are written out as text first, a byte beyond ASCII as the four characters of its `\xNN`
 * escape in lower case, and that text is then escaped for the crate, which escapes the backslash just written: the
 * pattern the crate compiles matches the escape's own characters, so the token never matches the byte it names.
 * A string literal keeps its characters, the escaping of a metacharacter leaving the same one character to match.
 * @param literal The literal.
 * @return The literal over the bytes logos compiles.
 */
[[nodiscard]] Literal escaped_literal(const Literal& literal)
{
    if (!literal.byte_string)
    {
        return literal;
    }

    std::string bytes;

    for (const auto byte : literal.bytes)
    {
        const auto value{static_cast<unsigned>(static_cast<unsigned char>(byte))};

        if (value < 0x80)
        {
            bytes.push_back(byte);
        }
        else
        {
            bytes += std::format(R"(\x{:02x})", value);
        }
    }

    return {.written = literal.written, .bytes = std::move(bytes), .byte_string = true};
}

/**
 * @brief One byte of a literal with its ASCII case folded, as logos folds a byte under `ignore(ascii_case)`.
 *
 * An ASCII letter becomes the two cases, which logos writes as an alternation of two one-byte literals and this
 * reads as the class of them, the two being one language and one priority; every other byte stays the byte it is.
 * @param byte The byte.
 * @param unicode Whether the pattern's classes are over scalars, which the class this may build inherits.
 * @return The node.
 */
[[nodiscard]] Node ascii_folded_byte(const char byte, const bool unicode)
{
    const auto value{static_cast<unsigned char>(byte)};

    if (!is_letter(value))
    {
        return {.kind = Bytes{.bytes = std::string(1, byte), .bounded = false}};
    }

    Scalar_set both;

    both.add(value | 0x20U, value | 0x20U);

    both.add(value & ~0x20U, value & ~0x20U);

    return {.kind = Class{.set = std::move(both), .unicode = unicode}};
}

/**
 * @brief A tree with the ASCII letters of every literal and class folded, which is what `ignore(ascii_case)` leaves.
 *
 * logos parses the pattern as it stands and then walks the compiled tree: a class gains the other case of its ASCII
 * letters, and a literal is taken apart into one piece per byte, each of them the two cases where the byte is an
 * ASCII letter. The pieces stay apart, so a run of several bytes counts two for each of them rather than two for
 * each character, which is the priority logos ends up with (logos-codegen 0.15.1, parser/ignore_flags.rs).
 * @param node The tree, its references expanded.
 * @return The folded tree.
 */
[[nodiscard]] Node ascii_folded(Node node)
{
    if (std::holds_alternative<Bytes>(node.kind))
    {
        const auto& [bytes, bounded]{std::get<Bytes>(node.kind)};

        std::vector<Node> pieces;

        for (const auto byte : bytes)
        {
            pieces.push_back(ascii_folded_byte(byte, false));
        }

        return pieces.size() == 1 ? std::move(pieces.front()) : Node{.kind = Concat{.parts = std::move(pieces)}};
    }

    if (std::holds_alternative<Class>(node.kind))
    {
        auto& set{std::get<Class>(node.kind).set};

        set = folded(set, false);

        return node;
    }

    std::visit(
            []<typename Kind>(Kind& kind) {
                if constexpr (std::is_same_v<Kind, Concat>)
                {
                    for (auto& part : kind.parts)
                    {
                        part = ascii_folded(std::move(part));
                    }
                }
                else if constexpr (std::is_same_v<Kind, Choice>)
                {
                    for (auto& branch : kind.branches)
                    {
                        branch = ascii_folded(std::move(branch));
                    }
                }
                else if constexpr (std::is_same_v<Kind, Repeat>)
                {
                    *kind.operand = ascii_folded(std::move(*kind.operand));
                }
            },
            node.kind);

    return node;
}

/**
 * @brief Whether a node matches the empty string.
 *
 * A reference is not expected here, the caller having expanded every one, and answers yes, which widens a follow
 * set and refuses rather than reads too much.
 * @param node The node.
 * @return True when it does.
 */
[[nodiscard]] bool matches_empty(const Node& node)
{
    return std::visit(
            []<typename Kind>(const Kind& kind) -> bool {
                if constexpr (std::is_same_v<Kind, Empty> || std::is_same_v<Kind, Reference>)
                {
                    return true;
                }
                else if constexpr (std::is_same_v<Kind, Bytes>)
                {
                    return kind.bytes.empty();
                }
                else if constexpr (std::is_same_v<Kind, Class>)
                {
                    return false;
                }
                else if constexpr (std::is_same_v<Kind, Concat>)
                {
                    return std::ranges::all_of(kind.parts, matches_empty);
                }
                else if constexpr (std::is_same_v<Kind, Choice>)
                {
                    return std::ranges::any_of(kind.branches, matches_empty);
                }
                else
                {
                    static_assert(std::is_same_v<Kind, Repeat>);

                    return kind.min == 0 || matches_empty(*kind.operand);
                }
            },
            node.kind);
}

/**
 * @brief The first byte of a scalar's UTF-8, which is what a scanner over bytes decides on.
 * @param scalar The scalar.
 * @return The byte.
 */
[[nodiscard]] unsigned lead_byte(const char32_t scalar) noexcept
{
    if (scalar < 0x80)
    {
        return static_cast<unsigned>(scalar);
    }

    if (scalar < 0x800)
    {
        return 0xC0U | static_cast<unsigned>(scalar >> 6U);
    }

    if (scalar < 0x10000)
    {
        return 0xE0U | static_cast<unsigned>(scalar >> 12U);
    }

    return 0xF0U | static_cast<unsigned>(scalar >> 18U);
}

/**
 * @brief The bytes a match of a node can begin with.
 *
 * A lead byte is what the scanner decides on, so a class of scalars answers with the lead bytes of its ranges,
 * which run with the scalars. A reference is not expected here, the caller having expanded every one, and answers
 * with every byte so that a caller that forgot refuses rather than reads too much.
 * @param node The node.
 * @return The bytes.
 */
[[nodiscard]] std::bitset<256> first_bytes(const Node& node)
{
    return std::visit(
            []<typename Kind>(const Kind& kind) -> std::bitset<256> {
                std::bitset<256> bytes;

                if constexpr (std::is_same_v<Kind, Bytes>)
                {
                    if (!kind.bytes.empty())
                    {
                        bytes.set(static_cast<unsigned char>(kind.bytes.front()));
                    }
                }
                else if constexpr (std::is_same_v<Kind, Class>)
                {
                    for (const auto& [low, high] : kind.set.ranges())
                    {
                        const auto first{kind.unicode ? lead_byte(low) : static_cast<unsigned>(low)};

                        const auto last{kind.unicode ? lead_byte(high) : static_cast<unsigned>(high)};

                        for (auto byte{first}; byte <= std::min(last, 0xFFU); ++byte)
                        {
                            bytes.set(byte);
                        }
                    }
                }
                else if constexpr (std::is_same_v<Kind, Reference>)
                {
                    bytes.set();
                }
                else if constexpr (std::is_same_v<Kind, Concat>)
                {
                    for (const auto& part : kind.parts)
                    {
                        bytes |= first_bytes(part);

                        if (!matches_empty(part))
                        {
                            break;
                        }
                    }
                }
                else if constexpr (std::is_same_v<Kind, Choice>)
                {
                    for (const auto& branch : kind.branches)
                    {
                        bytes |= first_bytes(branch);
                    }
                }
                else if constexpr (std::is_same_v<Kind, Repeat>)
                {
                    bytes |= first_bytes(*kind.operand);
                }

                return bytes;
            },
            node.kind);
}

/**
 * @brief Refuses a repetition logos 0.15.1 cannot resolve at its boundary, whose scanner then matches nothing.
 *
 * The crate's graph decides a repetition's end on one byte, so an unbounded repetition whose operand can begin with
 * a byte that may also follow it leaves it nowhere to go: logos compiles `a+a`, a dot-star between quotes and the
 * flex spelling of the block comment, whose loop and closer both admit a star, into scanners that match no input at
 * all, while the same patterns with the two byte sets apart, `[0-9]+k` and the block comment spelled so that its
 * loop cannot begin with a star, it scans as their language says. A bounded repetition is unrolled and needs no
 * such decision, so `ab?be` and `a{2,3}a` are read as they stand.
 * @param node The node, its references expanded.
 * @param follow The bytes that may follow a match of this node.
 * @param line The line, for refusals.
 * @throws Spec_error If a repetition's operand and its follow share a byte.
 */
void check_repetitions(const Node& node, const std::bitset<256>& follow, const std::size_t line)
{
    std::visit(
            [&]<typename Kind>(const Kind& kind) {
                if constexpr (std::is_same_v<Kind, Concat>)
                {
                    auto rest{follow};

                    for (const auto& part : kind.parts | std::views::reverse)
                    {
                        check_repetitions(part, rest, line);

                        rest = matches_empty(part) ? (rest | first_bytes(part)) : first_bytes(part);
                    }
                }
                else if constexpr (std::is_same_v<Kind, Choice>)
                {
                    for (const auto& branch : kind.branches)
                    {
                        check_repetitions(branch, follow, line);
                    }
                }
                else if constexpr (std::is_same_v<Kind, Repeat>)
                {
                    const auto once{kind.max == 1};

                    const auto inside{once ? follow : (follow | first_bytes(*kind.operand))};

                    if (!kind.max.has_value() && (first_bytes(*kind.operand) & follow).any())
                    {
                        throw Spec_error{
                                "a repetition whose body can begin with a byte that may also follow it is one logos "
                                "0.15.1 compiles into a scanner matching no input at all, its graph deciding the "
                                "repetition's end on one byte, so the token set cannot be read from it",
                                line};
                    }

                    check_repetitions(*kind.operand, inside, line);
                }
            },
            node.kind);
}

/**
 * @brief The set every class is complemented against in a mode: the scalars less the surrogates, or the bytes.
 * @param unicode Whether the mode is Unicode.
 * @return The universe.
 */
[[nodiscard]] Scalar_set universe_of(const bool unicode)
{
    Scalar_set set;

    if (!unicode)
    {
        set.add(0, 0xFF);

        return set;
    }

    // Made as the crate makes its dot, one range across the surrogate gap, with the surrogates then taken out.
    set.add(0, last_scalar);

    Scalar_set surrogates;

    surrogates.add(0xD800, 0xDFFF);

    return set.minus(surrogates);
}

/**
 * @brief Refuses an unbounded repetition of the dot under `s`, which logos 0.15.1 refuses.
 *
 * Before it builds anything, the crate compares the operand of every `*`, `+`, `{0,}` and `{1,}` with the dot that
 * matches every scalar, or every byte outside Unicode mode, and refuses the pattern when they are equal, since the
 * repetition would consume the source to its end with no backtracking to give any of it up: `(?s).*`, `(?s).+`,
 * `[\s\S]*`, `[\x00-\x{10FFFF}]*` and `(?:[^\n]|[\n\r])*`, an alternation of classes the crate merges into one, are
 * all that dot to it; the plain dot, which leaves out the newline, is not, nor is `[\x00-\x{D7FF}\x{E000}-\x{10FFFF}]`,
 * which the crate holds as two ranges where its dot is one, and a captured dot, `(?s)(.)*`, escapes the comparison,
 * which the crate makes before it strips the capture (logos-codegen 0.15.1, mir.rs). A bounded repetition and one of
 * at least two are unrolled and pass.
 * @param node The node, its references expanded.
 * @param written The pattern as written, for the refusal.
 * @param line The line, for the refusal.
 * @throws Spec_error If such a repetition stands anywhere in the node.
 */
void check_dot_repetitions(const Node& node, const std::string& written, const std::size_t line)
{
    std::visit(
            [&]<typename Kind>(const Kind& kind) {
                if constexpr (std::is_same_v<Kind, Concat>)
                {
                    for (const auto& part : kind.parts)
                    {
                        check_dot_repetitions(part, written, line);
                    }
                }
                else if constexpr (std::is_same_v<Kind, Choice>)
                {
                    for (const auto& branch : kind.branches)
                    {
                        check_dot_repetitions(branch, written, line);
                    }
                }
                else if constexpr (std::is_same_v<Kind, Repeat>)
                {
                    if (const auto seen{kind.max.has_value() || kind.min > 1 ? std::nullopt : merged(*kind.operand)})
                    {
                        const auto& [set, unicode, captured]{seen->cls};

                        if (!seen->literal && universe_of(unicode).minus(set).empty() && (!unicode || set.spans_gap()))
                        {
                            throw Spec_error{
                                    "the pattern " + written +
                                            " is refused: a `*` or `+` over the dot under `s`, or over a class "
                                            "of every scalar or every byte, is one logos 0.15.1 refuses, since it "
                                            "would consume the source to its end as logos does not backtrack",
                                    line};
                        }
                    }

                    check_dot_repetitions(*kind.operand, written, line);
                }
            },
            node.kind);
}

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
        const Literal& literal, const Pattern_kind kind, const Ignore_case folding, const Subpatterns_t& subpatterns,
        const std::size_t line)
{
    const auto token{kind == Pattern_kind::token};

    // `ignore(case)` is the crate's case-insensitive parse, and so is `ignore(ascii_case)` over a byte string, which
    // logos hands the same binary parse; over a string pattern the ASCII flag folds the tree afterwards instead.
    const auto parsed_insensitive{
            folding == Ignore_case::unicode || (folding == Ignore_case::ascii && literal.byte_string)};

    const auto fold_ascii{folding == Ignore_case::ascii && !literal.byte_string};

    const Flags flags{
            .insensitive = parsed_insensitive,
            .dot_all = false,
            .unicode = !literal.byte_string,
            .utf8 = !literal.byte_string};

    // An ignore flag takes a `#[token]` out of the literals and into the regexes: logos escapes the literal for the
    // regex crate, compiles that regex under the crate's case folding, and takes the priority from it rather than
    // from the byte length (logos-codegen 0.15.1, lib.rs and parser/definition.rs).
    const auto compiled{token && folding != Ignore_case::none ? escaped_literal(literal) : literal};

    Pattern_reader reader{compiled, flags, line};

    auto node{token ? reader.read_literal() : reader.read()};

    resolve(node, subpatterns, line, fold_ascii);

    if (fold_ascii)
    {
        node = ascii_folded(std::move(node));
    }

    // The priority and the boundaries are decided over the whole pattern as logos compiled it, read from the text
    // the crate's own tree and graph were built from, the subpatterns substituted into it; a token has no reference
    // to substitute, and logos hands a token under an ignore flag no subpatterns at all.
    auto whole{node};

    if (!token)
    {
        const Literal pasted{
                .written = literal.written,
                .bytes = substituted(literal, subpatterns, line),
                .byte_string = literal.byte_string};

        whole = Pattern_reader{pasted, flags, line}.read();

        if (fold_ascii)
        {
            whole = ascii_folded(std::move(whole));
        }
    }

    // Emptiness is a rule's matter, decided once the subpatterns are pasted in: logos 0.15.1 panics on an empty
    // token and compiles an empty regex into a rule matching no input, so neither is a rule to read, while an empty
    // definition is valid and adds nothing to the patterns referencing it (logos-codegen 0.15.1, parser/subpattern.rs
    // and graph/regex.rs).
    if (kind != Pattern_kind::definition && std::holds_alternative<Empty>(whole.kind))
    {
        throw Spec_error{
                "the pattern " + literal.written + " matches only the empty string" +
                        (token ? ", which logos 0.15.1 panics on" :
                                 ", which logos 0.15.1 compiles into a rule matching no input, so it is no token"),
                line};
    }

    check_dot_repetitions(whole, literal.written, line);

    check_repetitions(whole, {}, line);

    return {.expression = written(node, true),
            .priority = token && folding == Ignore_case::none ? 2 * literal.bytes.size() : priority(whole)};
}

/**
 * @brief A function the file defines, as far as a callback naming it needs: its return type and its body.
 */
struct Function
{
    /**
     * @brief The return type's text without its trivia, empty when the function returns `()`.
     */
    std::string returns;

    /**
     * @brief The body's text between its braces, or std::nullopt for a declaration without one.
     */
    std::optional<std::string> body;

    /**
     * @brief The name the first parameter binds, which is the lexer's when logos calls the function; empty when the
     *        parameter is `_` or a pattern, or there is none.
     */
    std::string parameter;

    /**
     * @brief The last segment of the type an enclosing `impl` block is for, which `Self` names in the function's
     *        return type and body; empty where the function is not declared directly in an impl block.
     */
    std::string self_type;
};

/**
 * @brief The functions the file defines, by name; several under one name when the file defines it more than once,
 *        in impl blocks or modules of their own.
 */
using Functions_t = std::map<std::string, std::vector<Function>, std::less<>>;

/**
 * @brief The names a file binds, as written, to what they stand for: a `use` binding to the path it imports, a
 *        `type` alias to its type, an item the file defines, a struct, an enum, a module or a function among them,
 *        to itself under `self::`, and a path `#[logos(crate = ...)]` names to `logos`; every value canonical, its
 *        own paths resolved through the table. The crate's names, `Skip`, `Filter`, `FilterResult` and `skip`, are
 *        the crate's unless the file binds them otherwise, since a bare one reaches a callback only through an import
 *        the reading may not see, `use logos::*` among them.
 */
using Names_t = std::map<std::string, std::string, std::less<>>;

/**
 * @brief The names the crate's paths go by when the file binds them no other way.
 */
constexpr std::array crate_names{
        std::pair{std::string_view{"Skip"}, std::string_view{"logos::Skip"}},
        std::pair{std::string_view{"Filter"}, std::string_view{"logos::Filter"}},
        std::pair{std::string_view{"FilterResult"}, std::string_view{"logos::FilterResult"}},
        std::pair{std::string_view{"skip"}, std::string_view{"logos::skip"}},
        std::pair{std::string_view{"logos"}, std::string_view{"logos"}}};

/**
 * @brief A path as the file's bindings resolve it: a leading `::` dropped, the longest prefix the table binds
 *        replaced by what it stands for, and again on the result, so that `lx::Skip` under `use logos as lx` and
 *        `Drop` under `use logos::Skip as Drop` are both `logos::Skip`, and a name the file defines is `self::name`.
 * @param names The file's bindings.
 * @param path The path without trivia.
 * @return The canonical path.
 */
[[nodiscard]] std::string canonical(const Names_t& names, std::string path)
{
    if (path.starts_with("::"))
    {
        path.erase(0, 2);
    }

    // A few passes resolve a chain of bindings; a cycle, which Rust refuses, ends where it started.
    for (std::size_t pass{0}; pass < 8; ++pass)
    {
        auto resolved{false};

        for (auto cut{path.size()}; cut != std::string::npos && !resolved; cut = path.rfind("::", cut - 1))
        {
            if (cut == 0)
            {
                break;
            }

            const auto prefix{path.substr(0, cut)};

            const auto found{names.find(prefix)};

            const auto crate{std::ranges::find_if(crate_names, [&](const auto& pair) { return pair.first == prefix; })};

            if (found != names.end() || crate != crate_names.end())
            {
                const auto& value{found != names.end() ? std::string_view{found->second} : crate->second};

                // An alias of a type with arguments stands for the whole; it prefixes nothing.
                if (value.find('<') != std::string_view::npos && cut != path.size())
                {
                    return path;
                }

                if (value == prefix)
                {
                    return path;
                }

                path = std::string{value} + path.substr(cut);

                resolved = true;
            }
        }

        if (!resolved)
        {
            return path;
        }
    }

    return path;
}

/**
 * @brief A type's text with every path in it resolved by canonical(), the rest kept as written.
 * @param names The file's bindings.
 * @param text The type without trivia.
 * @return The canonical type.
 */
[[nodiscard]] std::string canonical_type(const Names_t& names, const std::string_view text)
{
    std::string result;

    for (std::size_t at{0}; at < text.size();)
    {
        // A path: an optional leading `::`, then words joined by `::`.
        auto end{at};

        if (text.compare(end, 2, "::") == 0)
        {
            end += 2;
        }

        while (end < text.size() && is_word_byte(text[end]))
        {
            for (++end; end < text.size() && is_word_byte(text[end]); ++end)
            {
            }

            if (text.compare(end, 2, "::") == 0 && end + 2 < text.size() && is_word_byte(text[end + 2]))
            {
                end += 2;

                continue;
            }

            break;
        }

        if (end == at || !is_word_byte(text[end - 1]))
        {
            result.push_back(text[at++]);

            continue;
        }

        result += canonical(names, std::string{text.substr(at, end - at)});

        at = end;
    }

    return result;
}

/**
 * @brief A text without its trivia: the blanks and the comments dropped, as Rust's lexer drops them before anything
 *        reads a type or a path, and every token, string and character literals included, kept as written.
 * @param text The text.
 * @return The text with its trivia dropped.
 * @throws Spec_error If a block comment or a literal is left open.
 */
[[nodiscard]] std::string compacted(const std::string_view text)
{
    std::string compact;

    Rust_cursor cursor{text, 0, text.size()};

    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        const auto begin{cursor.offset()};

        // A literal is copied whole, trivia inside it being none; a group's delimiters are single bytes here, so
        // that the trivia inside the group is dropped as well.
        if (cursor.at_string() || cursor.at("'") || cursor.at("b'"))
        {
            cursor.skip_token();
        }
        else if (cursor.word().empty())
        {
            std::ignore = cursor.next("a token");
        }

        compact += text.substr(begin, cursor.offset() - begin);
    }

    return compact;
}

/**
 * @brief A text without the blanks at either end.
 * @param text The text.
 * @return The text trimmed.
 */
[[nodiscard]] std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && is_blank(text.front()))
    {
        text.remove_prefix(1);
    }

    while (!text.empty() && is_blank(text.back()))
    {
        text.remove_suffix(1);
    }

    return text;
}

/**
 * @brief Whether a text is one delimited group and nothing else, `(...)` or `{...}` through its own close.
 * @param text The text, trimmed.
 * @param open The opening delimiter.
 * @return True when the group opening the text closes at its end.
 * @throws Spec_error If the group is left open.
 */
[[nodiscard]] bool is_group(const std::string_view text, const char open)
{
    if (!text.starts_with(open))
    {
        return false;
    }

    Rust_cursor cursor{text, 0, text.size()};

    cursor.skip_group();

    return cursor.done();
}

/**
 * @brief Skips the block-like expression opening at the cursor: an `if` with its `else` chain, a `match`, a `while`,
 *        `for` or `loop`, an `unsafe` block or a bare block, the forms Rust lets stand as a statement with no
 *        semicolon after them.
 * @param cursor The cursor, at a statement's start.
 * @param text The text the cursor runs over.
 * @return True when one stood there and was skipped; false, the cursor unmoved, otherwise.
 * @throws Spec_error If a group is left open.
 */
[[nodiscard]] bool skip_block_like(Rust_cursor& cursor, const std::string_view text)
{
    if (cursor.peek() == '{')
    {
        cursor.skip_group();

        return true;
    }

    Rust_cursor look{text, cursor.offset(), text.size()};

    const std::string word{look.word()};

    if (word != "if" && word != "match" && word != "while" && word != "for" && word != "loop" && word != "unsafe")
    {
        return false;
    }

    // The head, a condition, a scrutinee or a label, up to the block, then the block; an `if` then takes its `else`
    // chain, each `else` followed by another head and block or by a block alone.
    for (;;)
    {
        while (!look.done() && look.peek() != '{')
        {
            look.skip_token();
        }

        look.skip_group();

        cursor = look;

        if (word != "if")
        {
            return true;
        }

        look.skip_trivia();

        if (look.word() != "else")
        {
            return true;
        }
    }
}

/**
 * @brief Skips the angle-bracketed generics opening at the cursor, through their close.
 * @param cursor The cursor, at the `<`.
 * @throws Spec_error If the text ends first.
 */
void skip_generics(Rust_cursor& cursor)
{
    std::size_t depth{0};

    do
    {
        const auto byte{cursor.next("'>' to close the generics")};

        if (byte == '<')
        {
            ++depth;
        }
        else if (byte == '>')
        {
            --depth;
        }
        else if (byte == '-' && cursor.peek() == '>')
        {
            // The arrow of a function type among the bounds, `F: Fn() -> u8`.
            std::ignore = cursor.next("'>'");
        }
    } while (depth > 0);
}

/**
 * @brief Reads the type an `impl` block is for, `impl Type`, `impl Trait for Type`, either with generics and a where
 *        clause, leaving the cursor at the block's brace.
 * @param cursor The cursor, just past `impl`.
 * @return The last segment of the type's path, or empty when the type is not a path.
 * @throws Spec_error If a group or a literal is left open.
 */
[[nodiscard]] std::string impl_type(Rust_cursor& cursor)
{
    // A path, each segment's generics skipped; the last segment is what `Self::Variant` is read against.
    const auto path{[&cursor] {
        std::string last;

        for (;;)
        {
            cursor.skip_trivia();

            last = std::string{cursor.word()};

            cursor.skip_trivia();

            if (cursor.peek() == '<')
            {
                skip_generics(cursor);

                cursor.skip_trivia();
            }

            if (!cursor.at("::"))
            {
                return last;
            }

            cursor.expect(':', "':'");
            cursor.expect(':', "':'");
        }
    }};

    cursor.skip_trivia();

    if (cursor.peek() == '<')
    {
        skip_generics(cursor);
    }

    auto type{path()};

    // `impl Trait for Type`: the path read so far was the trait's.
    if (Rust_cursor look{cursor}; look.word() == "for")
    {
        cursor = look;

        type = path();
    }

    while (!cursor.done() && cursor.peek() != '{' && cursor.peek() != ';')
    {
        cursor.skip_token();
    }

    return type;
}

/**
 * @brief What the file defines and binds, as far as reading a callback needs: its functions and its names.
 */
struct Items
{
    /**
     * @brief The functions, by name.
     */
    Functions_t functions;

    /**
     * @brief The names bound, by their spelling.
     */
    Names_t names;
};

/**
 * @brief Binds a name to a path, the path resolved through the bindings so far.
 * @param names The bindings, added to.
 * @param name The name as written.
 * @param path The path or type it stands for, without trivia.
 */
void bind_name(Names_t& names, const std::string& name, const std::string& path)
{
    names.insert_or_assign(name, canonical_type(names, path));
}

/**
 * @brief Reads the `use` tree at the cursor and binds every name it brings in: `a::b::C` binds `C`, `a::b::C as D`
 *        binds `D`, `a::{B, c::D}` each of its branches under `a`, `a::b::{self}` binds `b`, and a glob binds
 *        nothing the reading can name.
 * @param cursor The cursor, at the tree's first segment or brace.
 * @param prefix The path the tree stands under, `a::` for its branches, empty at the top.
 * @param names The bindings, added to.
 * @throws Spec_error If a brace is left open.
 */
void read_use_tree(Rust_cursor& cursor, const std::string& prefix, Names_t& names)
{
    cursor.skip_trivia();

    if (cursor.peek() == '{')
    {
        const auto open{cursor.offset()};

        cursor.skip_group();

        auto branch{cursor.inside(open + 1, cursor.offset() - 1)};

        for (branch.skip_trivia(); !branch.done(); branch.skip_trivia())
        {
            read_use_tree(branch, prefix, names);

            branch.skip_trivia();

            if (!branch.accept(','))
            {
                break;
            }
        }

        return;
    }

    if (cursor.accept('*'))
    {
        return;
    }

    std::string path{prefix};

    std::string last;

    for (;;)
    {
        if (cursor.at("::"))
        {
            cursor.expect(':', "':'");
            cursor.expect(':', "':'");

            path += path.empty() ? "" : "::";
        }

        cursor.skip_trivia();

        if (cursor.peek() == '{' || cursor.peek() == '*')
        {
            read_use_tree(cursor, path, names);

            return;
        }

        last = std::string{cursor.word()};

        if (last.empty())
        {
            return;
        }

        path += last;

        cursor.skip_trivia();

        if (!cursor.at("::"))
        {
            break;
        }
    }

    // `self` at the end names the module before it.
    if (last == "self")
    {
        path.erase(path.size() - 6);

        last = path.substr(path.rfind("::") == std::string::npos ? 0 : path.rfind("::") + 2);
    }

    if (Rust_cursor look{cursor}; look.word() == "as")
    {
        look.skip_trivia();

        last = std::string{look.word()};

        cursor = look;
    }

    if (!last.empty() && last != "_")
    {
        bind_name(names, last, path);
    }
}

/**
 * @brief Collects what the file defines and binds, at any depth: every `fn` item, its name, the return type written
 *        after `->` up to the body or a `where` clause, the body, the first parameter's name, and the type of the
 *        `impl` block it is declared in, when it is; every `use`, `type` alias and `extern crate` binding; and the
 *        name of every struct, enum, union, trait, module, constant and static, bound to itself.
 * @param source The file's text.
 * @return The items.
 * @throws Spec_error If a group or a literal is left open.
 */
[[nodiscard]] Items collect_items(const std::string_view source)
{
    Rust_cursor cursor{source, 0, source.size()};

    Items items;

    auto& [functions, names]{items};

    // Whether the keyword `where` stands at the cursor, as a whole word.
    const auto at_where{[&cursor, source] {
        return cursor.at("where") &&
               (cursor.offset() + 5 >= source.size() || !is_word_byte(source[cursor.offset() + 5]));
    }};

    // One entry per open delimiter: the impl block's type for a brace opening one, empty otherwise.
    std::vector<std::string> scopes;

    // Whether each open delimiter is the brace of an impl or trait block, whose `type` items are associated types.
    std::vector<bool> associated;

    // The type of the impl block whose brace is next to open, and whether an impl or trait block is next to open.
    std::string pending_impl;

    auto pending_associated{false};

    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        if (cursor.at_string())
        {
            cursor.skip_token();

            continue;
        }

        // An attribute's content is no item, `#[logos(type S = &str)]` among them.
        if (cursor.at("#[") || cursor.at("#!["))
        {
            while (cursor.peek() != '[')
            {
                std::ignore = cursor.next("'['");
            }

            cursor.skip_group();

            continue;
        }

        const auto word{cursor.word()};

        if (word == "impl")
        {
            auto type{impl_type(cursor)};

            // An `impl Trait` elsewhere, in a type alias say, opens no block.
            pending_impl = cursor.peek() == '{' ? std::move(type) : std::string{};

            pending_associated = cursor.peek() == '{';

            continue;
        }

        if (word == "trait")
        {
            pending_associated = true;
        }

        if (word == "use")
        {
            read_use_tree(cursor, {}, names);

            continue;
        }

        if (word == "extern")
        {
            // `extern crate logos as lx;` binds the crate's name; anything else is a block or a declaration.
            if (Rust_cursor look{cursor}; look.word() == "crate")
            {
                look.skip_trivia();

                const std::string crate{look.word()};

                look.skip_trivia();

                std::string name{crate};

                if (Rust_cursor rename{look}; rename.word() == "as")
                {
                    rename.skip_trivia();

                    name = std::string{rename.word()};

                    look = rename;
                }

                cursor = look;

                if (!crate.empty() && !name.empty() && name != "_")
                {
                    bind_name(names, name, crate);
                }
            }

            continue;
        }

        if (word == "type" && (associated.empty() || !associated.back()))
        {
            cursor.skip_trivia();

            const std::string name{cursor.word()};

            cursor.skip_trivia();

            // A generic alias stands for no one type; its name is bound to itself.
            const auto generic{cursor.peek() == '<'};

            while (!cursor.done() && cursor.peek() != '=' && cursor.peek() != ';' && cursor.peek() != '{')
            {
                cursor.skip_token();
            }

            if (cursor.accept('='))
            {
                const auto begin{cursor.offset()};

                while (!cursor.done() && cursor.peek() != ';')
                {
                    cursor.skip_token();
                }

                if (!name.empty())
                {
                    bind_name(names, name, generic ? "self::" + name : compacted(cursor.slice(begin, cursor.offset())));
                }
            }

            continue;
        }

        if (word == "struct" || word == "enum" || word == "union" || word == "trait" || word == "mod" ||
            word == "const" || word == "static")
        {
            cursor.skip_trivia();

            if (const std::string name{cursor.word()}; !name.empty() && name != "_")
            {
                bind_name(names, name, "self::" + name);
            }

            continue;
        }

        if (word == "fn")
        {
            cursor.skip_trivia();

            const std::string name{cursor.word()};

            // A function pointer type, `fn(u8) -> u8`, names nothing.
            if (name.empty())
            {
                continue;
            }

            // A free function's name is in scope where the enum is; a method's is reached through its type.
            if (scopes.empty() || (scopes.back().empty() && !associated.back()))
            {
                bind_name(names, name, "self::" + name);
            }

            // Generic parameters stand between the name and the parameter list.
            while (!cursor.done() && cursor.peek() != '(' && cursor.peek() != '{' && cursor.peek() != ';')
            {
                cursor.skip_token();
            }

            if (cursor.peek() != '(')
            {
                continue;
            }

            const auto parameters{cursor.offset()};

            cursor.skip_group();

            // The first parameter's name, `mut` and attributes before it stepped over; `_` or a pattern binds none.
            auto first{cursor.inside(parameters + 1, cursor.offset() - 1)};

            for (first.skip_trivia(); first.at("#["); first.skip_trivia())
            {
                std::ignore = first.next("'#'");

                first.skip_group();
            }

            auto parameter{std::string{first.word()}};

            if (parameter == "mut")
            {
                first.skip_trivia();

                parameter = std::string{first.word()};
            }

            cursor.skip_trivia();

            Function function{
                    .returns = {},
                    .body = std::nullopt,
                    .parameter = parameter == "_" ? std::string{} : std::move(parameter),
                    .self_type = scopes.empty() ? std::string{} : scopes.back()};

            if (cursor.at("->"))
            {
                cursor.expect('-', "'-'");
                cursor.expect('>', "'>'");

                const auto begin{cursor.offset()};

                while (!cursor.done() && cursor.peek() != '{' && cursor.peek() != ';' && !at_where())
                {
                    cursor.skip_token();
                }

                function.returns = compacted(cursor.slice(begin, cursor.offset()));
            }

            // A where clause, then the body or the semicolon of a declaration.
            while (!cursor.done() && cursor.peek() != '{' && cursor.peek() != ';')
            {
                cursor.skip_token();
            }

            if (cursor.peek() == '{')
            {
                const auto open{cursor.offset()};

                cursor.skip_group();

                function.body = std::string{cursor.slice(open + 1, cursor.offset() - 1)};
            }

            functions[name].push_back(std::move(function));

            continue;
        }

        if (word.empty() && (cursor.peek() == '(' || cursor.peek() == '[' || cursor.peek() == '{'))
        {
            // Descended into rather than skipped, so that a function inside a module or an impl block is found too;
            // a brace takes the impl type, and the impl or trait mark, waiting for it, if any.
            const auto open{cursor.next("a delimiter")};

            scopes.push_back(open == '{' ? std::exchange(pending_impl, {}) : std::string{});

            associated.push_back(open == '{' && std::exchange(pending_associated, false));
        }
        else if (word.empty() && (cursor.peek() == ')' || cursor.peek() == ']' || cursor.peek() == '}'))
        {
            if (!scopes.empty())
            {
                scopes.pop_back();

                associated.pop_back();
            }

            std::ignore = cursor.next("a delimiter");
        }
        else if (word.empty())
        {
            cursor.skip_token();
        }
    }

    return items;
}

/**
 * @brief What a callback visibly does with the match on one path: skips it, or emits a token, named.
 */
struct Outcome
{
    /**
     * @brief Whether the match is skipped.
     */
    bool skips;

    /**
     * @brief The token emitted, empty when the match is skipped.
     */
    std::string token;

    /**
     * @brief Orders outcomes, skips before tokens and tokens by name, so that a set of them holds each once.
     */
    [[nodiscard]] auto operator<=>(const Outcome&) const = default;
};

/**
 * @brief The outcomes a callback shows, one per path.
 */
using Outcomes_t = std::set<Outcome>;

/**
 * @brief What a callback is read against: the enum, its variants, and what the file defines and binds.
 */
struct Enum_context
{
    /**
     * @brief The enum's name, which a constructor of it opens with.
     */
    std::string_view name;

    /**
     * @brief The variants' names, which a constructor of the enum must be one of.
     */
    const std::vector<std::string>& variants;

    /**
     * @brief The functions the file defines.
     */
    const Functions_t& functions;

    /**
     * @brief The names the file binds, the path `#[logos(crate = ...)]` gives the crate among them.
     */
    const Names_t& names;
};

/**
 * @brief The variant a rule stands on, against which its callback's results are read.
 */
struct Variant
{
    /**
     * @brief The variant's name.
     */
    std::string name;

    /**
     * @brief The payload's type as written, without trivia; empty for a unit variant, and `()` is a unit to logos too.
     */
    std::string payload;
};

/**
 * @brief What a value visibly is, as far as reading it against the variant needs.
 */
struct Value
{
    /**
     * @brief The kinds a value is told apart into.
     */
    enum class Kind
    {
        /**
         * @brief The crate's `Skip`, bare.
         */
        skip,

        /**
         * @brief The `Skip` arm of `Filter` or `FilterResult`.
         */
        arm_skip,

        /**
         * @brief A constructor of the enum, `T::Name` or `T::Name(...)`.
         */
        variant,

        /**
         * @brief A literal, `true` or `false`: a payload of some type.
         */
        literal,

        /**
         * @brief `()`, or nothing.
         */
        unit,

        /**
         * @brief Anything else, whose type the text does not show.
         */
        opaque,
    };

    /**
     * @brief The kind.
     */
    Kind kind;

    /**
     * @brief The variant named, when the kind is variant.
     */
    std::string variant;
};

/**
 * @brief Reads what a `#[token]` or `#[regex]` callback does with the match, as far as the source shows it.
 *
 * logos decides by the callback's type and the variant's payload at compile time, through the `CallbackResult`
 * conversion for the pair (logos 0.15.1, src/internal.rs and logos-codegen 0.15.1, generator/leaf.rs): for a variant
 * without a payload, `()` to the conversion, `Skip` and `Result<Skip, E>` and the `Skip` arms of `Filter` and
 * `FilterResult` discard the match, `()`, `bool`, `Option<()>`, `Result<(), E>`, the `Emit` arms and the `Error` arm
 * leave the variant's token or an error at the same boundary, and the enum returned, bare or in `Ok`, `Filter` or
 * `FilterResult`, is the token itself; for a variant with a payload, a value of the payload's type, bare or in `Some`,
 * `Ok` or an `Emit` arm, is that payload and the variant's token, so a `Skip` returned to a variant carrying `Skip` is
 * the payload and emits, the `Skip` arms alone still skip, and the enum, `()`, `bool` and a `Skip` the payload's type
 * is not are type errors the crate refuses. The reading has no types, only the text, so it reads what the text shows:
 * `logos::skip` or `skip` is the crate's function returning `Skip`; another path names a function this file defines,
 * whose return type decides, its body read where the type is `Filter`, `FilterResult` or the enum; a closure's body is
 * read for every result it produces, the tail expression, every `return`, the branches of an `if` and the arms of a
 * `match`, and each result must be visibly one thing: `Skip`, `Filter::Skip`, `FilterResult::Skip` or `Ok` of one, a
 * constructor of the enum, or `Some`, `None`, `Ok`, `Err`, `true`, `false`, a literal, `()`, `Filter::Emit`,
 * `FilterResult::Emit` or `FilterResult::Error`, each read against the payload the variant is written with. A callback
 * whose results all skip discards the rule, one whose results all emit one variant makes the rule that variant's, and
 * anything else, a function the file does not define, a result the text does not show, results that skip on one path
 * and emit on another, or a result the crate refuses for the variant's payload, is refused by name, since the rule's
 * token is then decided at run time or out of sight. logos 0.15.1 refuses a closure with a return type annotation, so
 * a closure's body is the only place to look, and a `skip(...)` attribute's callback has no result to read, every
 * result the crate admits there skipping or failing.
 *
 * The callback also holds the lexer, and may move it: logos 0.15.1's Lexer moves its cursor through `bump` alone among
 * its public methods, and through `bump_unchecked`, `trivia`, `error`, `end` and `set` of its `internal::LexerInternal`
 * trait, which a callback can reach by importing it (logos 0.15.1, src/lexer.rs and src/internal.rs), while `slice`,
 * `span`, `remainder` and `source` and the `extras` field read it and `clone` copies it. A match the callback extends
 * or empties is not the pattern's, so a body, a closure's or the named function's, is read only where its lexer
 * parameter is used through the reading members alone; one naming `bump`, `bump_unchecked` or `trivia` as a method on
 * anything, or using the parameter any other way, passing it to a function or a macro, calling another method on it or
 * binding it to a name, is refused by name, and so is a function declared without a body, whose use of the lexer is
 * out of sight.
 */
class Callback_reader
{
public:
    /**
     * @brief Binds the reading to one rule.
     * @param context The enum, its variants, and what the file defines and binds.
     * @param variant The variant the rule stands on, or std::nullopt for a `skip(...)` attribute's rule.
     * @param callback The callback's text, empty when there is none.
     * @param line The rule's line, for refusals.
     */
    Callback_reader(
            const Enum_context& context, const std::optional<Variant>& variant, std::string_view callback,
            std::size_t line);

    /**
     * @brief The token the rule carries.
     * @return The token, or std::nullopt when the rule is a skip's or the callback skips the match.
     * @throws Spec_error If what the callback does is not decidable from the source, or it moves the lexer.
     */
    [[nodiscard]] std::optional<std::string> token() const;

private:
    /**
     * @brief Refuses a body that moves the lexer or lets it out of sight.
     * @param body The body's text, a closure's or a function's.
     * @param parameter The lexer parameter's name, empty when the callback binds none.
     * @throws Spec_error If the body names a method moving the cursor, or uses the parameter other than through the
     *         members that read the lexer.
     */
    void check_lexer_use(std::string_view body, std::string_view parameter) const;

    /**
     * @brief The outcomes of the callback: a path's by the function it names, a closure's by its body.
     * @return The outcomes.
     * @throws Spec_error If the callback is out of sight or malformed.
     */
    [[nodiscard]] Outcomes_t of_callback() const;

    /**
     * @brief The outcomes of a function this file defines, by its return type, or by its body where the type leaves
     *        the decision to the value.
     * @param name The function's name.
     * @return The outcomes.
     * @throws Spec_error If the file defines no such function, or more than one, or a bodiless one the type does not
     *         decide.
     */
    [[nodiscard]] Outcomes_t of_function(std::string_view name) const;

    /**
     * @brief The outcomes of a block's content: every `return` in it, and its value, the tail expression or `()`.
     * @param text The text between the braces.
     * @return The outcomes.
     * @throws Spec_error If a result is not visibly a token or a skip.
     */
    [[nodiscard]] Outcomes_t of_body(std::string_view text) const;

    /**
     * @brief The outcomes of an expression: a block's, an `if`'s branches, a `match`'s arms, a `return`'s value, or
     *        the one outcome a constructor shows.
     * @param text The expression's text.
     * @return The outcomes.
     * @throws Spec_error If the expression is not visibly a token or a skip.
     */
    [[nodiscard]] Outcomes_t of_value(std::string_view text) const;

    /**
     * @brief The outcome a bare result makes, read against the variant's payload: a skip for `Skip` where the variant
     *        has no payload and the `Skip` arms anywhere, the variant named by a constructor of the enum, the rule's
     *        own variant for a payload of the variant's type, `()` where it has none and `Skip` where that is its
     *        payload's type, and a refusal for what the crate refuses or the text does not show.
     * @param text The result's text.
     * @return The outcome.
     * @throws Spec_error If the result is not visibly a token or a skip, or is one the crate refuses for the variant.
     */
    [[nodiscard]] Outcome of_result(std::string_view text) const;

    /**
     * @brief The outcome a constructor's argument makes, read against the variant's payload as of_result() reads a
     *        bare result, except that only `Ok` skips on a `Skip`, `Some` takes no constructor of the enum, and
     *        `Err` and `FilterResult::Error` are an error at the boundary whatever they hold.
     * @param constructor The constructor's path without trivia.
     * @param text The argument's text.
     * @return The outcome.
     * @throws Spec_error If the argument is one the crate refuses for the variant.
     */
    [[nodiscard]] Outcome of_argument(std::string_view constructor, std::string_view text) const;

    /**
     * @brief What a value visibly is: `Skip`, a `Skip` arm, a constructor of the enum, a literal, `()` or opaque.
     * @param text The value's text.
     * @return The value.
     */
    [[nodiscard]] Value classify(std::string_view text) const;

    /**
     * @brief The variant a constructor of the enum names, `Enum::Variant` or `Enum::Variant(...)`.
     * @param text The expression's text.
     * @return The variant, or std::nullopt when the expression is not such a constructor.
     */
    [[nodiscard]] std::optional<std::string> enum_variant(std::string_view text) const;

    /**
     * @brief Whether the rule's variant has no payload, `()` being none to logos as well.
     * @return True when it has none.
     */
    [[nodiscard]] bool unit() const;

    /**
     * @brief Whether the rule's variant carries the crate's `Skip` as its payload, in any of its spellings.
     * @return True when it does.
     */
    [[nodiscard]] bool payload_is_skip() const;

    /**
     * @brief The variant with its payload, `V(u64)`, for refusals.
     * @return The text.
     */
    [[nodiscard]] std::string written_variant() const;

    /**
     * @brief Adds the outcomes of every `return` in a text, at any depth.
     * @param text The text.
     * @param outcomes The outcomes, added to.
     * @throws Spec_error If a returned value is not visibly a token or a skip.
     */
    void collect_returns(std::string_view text, Outcomes_t& outcomes) const;

    /**
     * @brief The outcome that emits the rule's own variant; not asked of a skip's rule, whose results are not read.
     * @return The outcome.
     */
    [[nodiscard]] Outcome emits() const;

    /**
     * @brief Refuses the callback.
     * @param why The reason.
     * @throws Spec_error Always.
     */
    [[noreturn]] void fail(const std::string& why) const;

    const Enum_context& context_;
    const std::optional<Variant>& variant_;
    std::string_view callback_;
    std::size_t line_;

    /**
     * @brief Whether `Self` names the enum in the text being read, as it does in a function declared in one of the
     *        enum's impl blocks.
     */
    bool self_is_enum_{false};
};

/**
 * @brief The crate's `Skip`, the value and the type, as canonical() spells it.
 */
constexpr std::string_view crate_skip{"logos::Skip"};

/**
 * @brief The `Skip` arms of `Filter` and `FilterResult`, as canonical() spells them.
 */
constexpr std::array skip_arms{std::string_view{"logos::Filter::Skip"}, std::string_view{"logos::FilterResult::Skip"}};

/**
 * @brief The constructors whose argument is read by of_argument(), as canonical() spells them: `Ok`, which may hold
 *        a `Skip` or the enum, and the others, which emit or fail.
 */
constexpr std::array constructors{
        std::string_view{"Ok"},
        std::string_view{"Some"},
        std::string_view{"Err"},
        std::string_view{"logos::Filter::Emit"},
        std::string_view{"logos::FilterResult::Emit"},
        std::string_view{"logos::FilterResult::Error"}};

Callback_reader::Callback_reader(
        const Enum_context& context, const std::optional<Variant>& variant, const std::string_view callback,
        const std::size_t line)
    : context_{context}, variant_{variant}, callback_{callback}, line_{line}
{}

std::optional<std::string> Callback_reader::token() const
{
    if (trimmed(callback_).empty())
    {
        return variant_ ? std::optional{variant_->name} : std::nullopt;
    }

    const auto outcomes{of_callback()};

    if (outcomes.size() == 1)
    {
        const auto& [skips, token]{*outcomes.begin()};

        return skips ? std::nullopt : std::optional{token};
    }

    std::string results;

    for (const auto& [skips, token] : outcomes)
    {
        results += (results.empty() ? "" : ", ") + (skips ? std::string{"a skip"} : "the token " + token);
    }

    fail("its results differ from one path to another, " + results + ", so which the rule gets is decided at run time");
}

Outcomes_t Callback_reader::of_callback() const
{
    const auto text{trimmed(callback_)};

    if (text.starts_with('|'))
    {
        Rust_cursor cursor{text, 0, text.size()};

        cursor.expect('|', "'|'");

        cursor.skip_trivia();

        const auto parameter{cursor.word()};

        cursor.skip_trivia();

        if (parameter.empty() || !cursor.accept('|'))
        {
            fail("logos 0.15.1 reads an inline callback only as a closure with exactly one parameter");
        }

        const auto body{text.substr(cursor.offset())};

        check_lexer_use(body, parameter == "_" ? std::string_view{} : parameter);

        // A skip attribute's callback has no result to read: every one the crate admits skips or fails.
        return variant_ ? of_value(body) : Outcomes_t{Outcome{.skips = true, .token = {}}};
    }

    const auto path{canonical(context_.names, compacted(text))};

    if (path == "logos::skip")
    {
        return {Outcome{.skips = true, .token = {}}};
    }

    if (path.starts_with("logos::"))
    {
        fail("it names `" + path +
             "` of the crate, which is not its `skip` function, so what it returns is out of sight");
    }

    const auto name{path.substr(path.rfind("::") == std::string::npos ? 0 : path.rfind("::") + 2)};

    if (name.empty() || !std::ranges::all_of(name, is_word_byte))
    {
        fail("it is neither a path nor a closure, so what it returns is out of sight");
    }

    return of_function(name);
}

Outcomes_t Callback_reader::of_function(const std::string_view name) const
{
    const auto found{context_.functions.find(name)};

    if (found == context_.functions.end())
    {
        fail("it names no function this file defines, so what it returns is out of sight; a callback is read when "
             "it is logos::skip, a function this file defines, or a closure whose every result is visibly a token "
             "or a skip");
    }

    // A trait declares a method without a body and its impl defines it with one; the definition is the one read.
    const auto defined{std::ranges::count_if(found->second, [](const Function& one) { return one.body.has_value(); })};

    if (found->second.size() > 1 && defined != 1)
    {
        fail("this file defines `" + std::string{name} + "` more than once, so which one it names is out of sight");
    }

    const auto& [written, body, parameter, self_type]{
            defined == 1 ?
                    *std::ranges::find_if(found->second, [](const Function& one) { return one.body.has_value(); }) :
                    found->second.front()};

    if (!body)
    {
        fail("the function `" + std::string{name} +
             "` is declared without a body, so what it does with the lexer and what it returns are out of sight");
    }

    check_lexer_use(*body, parameter);

    if (!variant_)
    {
        return {Outcome{.skips = true, .token = {}}};
    }

    // In a function declared in one of the enum's impl blocks, `Self` is the enum, in the type and in the body.
    Callback_reader inner{*this};

    inner.self_is_enum_ = self_type == context_.name;

    // The return type with its aliases and imports resolved: `Filter` and `FilterResult` leave the decision to the
    // value; the enum takes the variant returned, for a variant without a payload; the crate's `Skip` anywhere is a
    // skip in the two shapes the conversion skips on and a payload elsewhere.
    const auto returns{canonical_type(context_.names, written)};

    const auto has_path{[&returns](const std::string_view path) {
        for (auto at{returns.find(path)}; at != std::string::npos; at = returns.find(path, at + 1))
        {
            const auto before{at == 0 || (!is_word_byte(returns[at - 1]) && returns[at - 1] != ':')};

            const auto after{
                    at + path.size() == returns.size() ||
                    (!is_word_byte(returns[at + path.size()]) && returns[at + path.size()] != ':')};

            if (before && after)
            {
                return true;
            }
        }

        return false;
    }};

    if (has_path("logos::Filter") || has_path("logos::FilterResult"))
    {
        return inner.of_body(*body);
    }

    // The enum is named as the file binds it, `self::T` where the file defines `enum T`.
    if (has_path(canonical(context_.names, std::string{context_.name})) || (inner.self_is_enum_ && has_path("Self")))
    {
        if (!unit())
        {
            fail("the function `" + std::string{name} +
                 "` returns the enum, which logos 0.15.1 takes from a callback only for a variant without a payload, "
                 "and " +
                 written_variant() + " carries one");
        }

        return inner.of_body(*body);
    }

    if ((returns == crate_skip || returns.starts_with("Result<logos::Skip,")) && unit())
    {
        return {Outcome{.skips = true, .token = {}}};
    }

    if (has_path(crate_skip) && !payload_is_skip())
    {
        fail("the function `" + std::string{name} + "` returns `" + written +
             "`, which logos 0.15.1 takes as a skip only for a variant without a payload and in the shapes `Skip` and "
             "`Result<Skip, E>`, and as the payload of a variant carrying `Skip`; " +
             written_variant() + " is neither, so the crate refuses it and the rule's token is out of sight");
    }

    if (returns == "bool" && !unit())
    {
        fail("the function `" + std::string{name} +
             "` returns a bool, which logos 0.15.1 takes from a callback only for a variant without a payload, and " +
             written_variant() + " carries one");
    }

    // No return type is `()`, the variant's token where it has no payload and no payload where it has one.
    return {returns.empty() ? of_result({}) : emits()};
}

Outcome Callback_reader::of_result(const std::string_view text) const
{
    const auto value{trimmed(text)};

    const auto [kind, variant]{classify(value)};

    switch (kind)
    {
    case Value::Kind::skip:
        if (unit())
        {
            return {.skips = true, .token = {}};
        }

        if (payload_is_skip())
        {
            return emits();
        }

        fail("its result `" + std::string{value} + "` is the payload of " + written_variant() +
             " to logos 0.15.1, which takes a callback's Skip as a skip only for a variant without a payload, and a "
             "type error unless `" +
             variant_->payload + "` is Skip under another name, so the rule's token is out of sight");
    case Value::Kind::arm_skip:
        return {.skips = true, .token = {}};
    case Value::Kind::variant:
        if (!unit())
        {
            fail("its result `" + std::string{value} +
                 "` is the enum, which logos 0.15.1 takes from a callback only for a variant without a payload, and " +
                 written_variant() + " carries one");
        }

        return {.skips = false, .token = variant};
    case Value::Kind::literal:
        if (unit())
        {
            fail("its result `" + std::string{value} + "` is a payload, and the variant " + variant_->name +
                 " carries none, so logos 0.15.1 refuses the file");
        }

        return emits();
    case Value::Kind::unit:
        if (!unit())
        {
            fail("its result is `()`, which is no payload for " + written_variant() +
                 ", so logos 0.15.1 refuses the file");
        }

        return emits();
    case Value::Kind::opaque:
        break;
    }

    fail("its result `" + std::string{value} +
         "` is not visibly a token or a skip; a result is read when it is logos::Skip, Filter::Skip, "
         "FilterResult::Skip or Ok of one, a constructor of the enum, Some, None, Ok, Err, true, false, a literal, "
         "(), Filter::Emit, FilterResult::Emit or FilterResult::Error, and a function this file defines is read by "
         "its return type instead");
}

Outcome Callback_reader::of_argument(const std::string_view constructor, const std::string_view text) const
{
    // An error at the boundary, whatever it holds.
    if (constructor == "Err" || constructor == "logos::FilterResult::Error")
    {
        return emits();
    }

    const auto value{trimmed(text)};

    const auto [kind, variant]{classify(value)};

    switch (kind)
    {
    case Value::Kind::skip:
        if (unit() && constructor == "Ok")
        {
            return {.skips = true, .token = {}};
        }

        if (!unit() && payload_is_skip())
        {
            return emits();
        }

        fail("its result wraps `" + std::string{value} + "` in `" + std::string{constructor} +
             "`, which logos 0.15.1 takes as a skip only as Ok(Skip) for a variant without a payload, and as the "
             "payload of a variant carrying `Skip`; " +
             written_variant() + " is neither, so the crate refuses it and the rule's token is out of sight");
    case Value::Kind::arm_skip:
        fail("its result wraps `" + std::string{value} + "` in `" + std::string{constructor} +
             "`, which is no result logos 0.15.1 takes from a callback");
    case Value::Kind::variant:
        if (!unit() || constructor == "Some")
        {
            fail("its result wraps `" + std::string{value} + "` in `" + std::string{constructor} +
                 "`, and logos 0.15.1 takes the enum from a callback only bare or in Ok, Filter::Emit or "
                 "FilterResult::Emit, for a variant without a payload, which " +
                 written_variant() + (unit() ? " is" : " is not"));
        }

        return {.skips = false, .token = variant};
    case Value::Kind::literal:
        if (unit())
        {
            fail("its result wraps the payload `" + std::string{value} + "` in `" + std::string{constructor} +
                 "`, and the variant " + variant_->name + " carries none, so logos 0.15.1 refuses the file");
        }

        return emits();
    case Value::Kind::unit:
        if (!unit())
        {
            fail("its result wraps `()` in `" + std::string{constructor} + "`, which is no payload for " +
                 written_variant() + ", so logos 0.15.1 refuses the file");
        }

        return emits();
    case Value::Kind::opaque:
        break;
    }

    return emits();
}

Value Callback_reader::classify(const std::string_view text) const
{
    const auto compact{compacted(text)};

    if (compact.empty() || compact == "()")
    {
        return {.kind = Value::Kind::unit, .variant = {}};
    }

    if (auto named{enum_variant(text)})
    {
        return {.kind = Value::Kind::variant, .variant = std::move(*named)};
    }

    const auto path{canonical(context_.names, compact)};

    if (path == crate_skip)
    {
        return {.kind = Value::Kind::skip, .variant = {}};
    }

    if (std::ranges::contains(skip_arms, std::string_view{path}))
    {
        return {.kind = Value::Kind::arm_skip, .variant = {}};
    }

    // A literal, or a unit struct spelled as the payload's own type, which the blanket conversion takes as the payload
    // whatever the type is.
    if (compact == "true" || compact == "false" || is_digit(compact.front()) || compact.starts_with('"') ||
        compact.starts_with('\'') || compact.starts_with("b\"") || compact.starts_with("b'") ||
        compact.starts_with("r\"") || compact.starts_with("r#") || compact.starts_with("br\"") ||
        compact.starts_with("br#") || (!unit() && path == variant_->payload))
    {
        return {.kind = Value::Kind::literal, .variant = {}};
    }

    return {.kind = Value::Kind::opaque, .variant = {}};
}

bool Callback_reader::unit() const
{
    return variant_->payload.empty() || variant_->payload == "()";
}

bool Callback_reader::payload_is_skip() const
{
    return variant_->payload == crate_skip;
}

std::string Callback_reader::written_variant() const
{
    return unit() ? variant_->name : variant_->name + "(" + variant_->payload + ")";
}

void Callback_reader::check_lexer_use(const std::string_view body, const std::string_view parameter) const
{
    // The members that only read the lexer, or copy it.
    constexpr std::array reading{std::string_view{"slice"},  std::string_view{"span"},   std::string_view{"remainder"},
                                 std::string_view{"source"}, std::string_view{"extras"}, std::string_view{"clone"}};

    // The methods that move the cursor, whatever they are called on.
    constexpr std::array moving{
            std::string_view{"bump"}, std::string_view{"bump_unchecked"}, std::string_view{"trivia"}};

    Rust_cursor cursor{body, 0, body.size()};

    // The dots read just before the token at the cursor: one is the dot of a method call or field access.
    std::size_t dots{0};

    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        if (cursor.at_string() || cursor.at("'") || cursor.at("b'"))
        {
            cursor.skip_token();

            dots = 0;

            continue;
        }

        const auto word{cursor.word()};

        if (word.empty())
        {
            dots = cursor.next("a token") == '.' ? dots + 1 : 0;

            continue;
        }

        const auto after_dot{dots == 1};

        dots = 0;

        if (after_dot && std::ranges::contains(moving, word))
        {
            fail("its body names `" + std::string{word} +
                 "`, which moves the lexer's cursor, so the match it leaves is not the pattern's");
        }

        if (!after_dot && !parameter.empty() && word == parameter)
        {
            // The parameter may only be read through a member: `lex.slice()`, `lex.extras += 1`.
            cursor.skip_trivia();

            const auto dotted{cursor.accept('.')};

            cursor.skip_trivia();

            const auto member{dotted ? cursor.word() : std::string_view{}};

            if (std::ranges::contains(moving, member))
            {
                fail("its body names `" + std::string{member} +
                     "`, which moves the lexer's cursor, so the match it leaves is not the pattern's");
            }

            if (!std::ranges::contains(reading, member))
            {
                fail("its body uses the lexer `" + std::string{parameter} +
                     "` other than through slice, span, remainder, source, extras or clone, so what becomes of the "
                     "match is out of sight");
            }
        }
    }
}

Outcomes_t Callback_reader::of_body(const std::string_view text) const
{
    Outcomes_t outcomes;

    collect_returns(text, outcomes);

    // The statements, split at the semicolons outside any group, string or character literal, and after a
    // block-like expression, an `if`, a `match`, a loop or a block, that opens a statement and has nothing
    // continuing it, which Rust lets stand without a semicolon; the last one, when no semicolon or such an end
    // closes it, is the block's value, and a block ending in a `return` has no value of its own.
    Rust_cursor cursor{text, 0, text.size()};

    // The last statement, or the tail expression, and whether a semicolon ended it.
    std::string_view last;

    auto terminated{true};

    // Whether the cursor stands where a statement begins.
    auto opening{true};

    for (auto begin{cursor.offset()};;)
    {
        cursor.skip_trivia();

        if (cursor.done())
        {
            if (const auto tail{trimmed(text.substr(begin, cursor.offset() - begin))}; !tail.empty())
            {
                last = tail;

                terminated = false;
            }

            break;
        }

        if (cursor.accept(';'))
        {
            last = trimmed(text.substr(begin, cursor.offset() - 1 - begin));

            terminated = true;

            begin = cursor.offset();

            opening = true;

            continue;
        }

        if (opening && skip_block_like(cursor, text))
        {
            Rust_cursor look{cursor};

            look.skip_trivia();

            if (!look.done() && look.peek() != ';' && look.peek() != '.' && look.peek() != '?')
            {
                last = trimmed(text.substr(begin, cursor.offset() - begin));

                terminated = true;

                begin = cursor.offset();

                continue;
            }
        }
        else
        {
            cursor.skip_token();
        }

        opening = false;
    }

    const auto returns{last.starts_with("return") && (last.size() == 6 || !is_word_byte(last[6]))};

    if (!terminated)
    {
        outcomes.merge(of_value(last));
    }
    else if (!returns)
    {
        outcomes.insert(of_result({}));
    }

    return outcomes;
}

Outcomes_t Callback_reader::of_value(const std::string_view text) const
{
    const auto value{trimmed(text)};

    // `()` is a value, and a parenthesised value is that value.
    if (value.empty())
    {
        return {of_result({})};
    }

    if (is_group(value, '('))
    {
        return of_value(value.substr(1, value.size() - 2));
    }

    if (is_group(value, '{'))
    {
        return of_body(value.substr(1, value.size() - 2));
    }

    Rust_cursor cursor{value, 0, value.size()};

    const auto word{cursor.word()};

    if (word == "return")
    {
        return of_value(value.substr(cursor.offset()));
    }

    // The block after the condition, then `else` and another `if` or block; without an `else` the value is `()`.
    if (word == "if")
    {
        while (!cursor.done() && cursor.peek() != '{')
        {
            cursor.skip_token();
        }

        const auto open{cursor.offset()};

        cursor.skip_group();

        auto outcomes{of_body(value.substr(open + 1, cursor.offset() - 2 - open))};

        cursor.skip_trivia();

        if (cursor.done())
        {
            outcomes.insert(of_result({}));

            return outcomes;
        }

        if (cursor.word() != "else")
        {
            fail("its result `" + std::string{value} + "` is not visibly a token or a skip");
        }

        outcomes.merge(of_value(value.substr(cursor.offset())));

        return outcomes;
    }

    // The arms between the braces after the scrutinee: a pattern, `=>`, then a block or a value up to the comma.
    if (word == "match")
    {
        while (!cursor.done() && cursor.peek() != '{')
        {
            cursor.skip_token();
        }

        const auto open{cursor.offset()};

        cursor.skip_group();

        if (!cursor.done())
        {
            fail("its result `" + std::string{value} + "` is not visibly a token or a skip");
        }

        const auto arms{value.substr(open + 1, value.size() - 2 - open)};

        Rust_cursor arm{arms, 0, arms.size()};

        Outcomes_t outcomes;

        for (arm.skip_trivia(); !arm.done(); arm.skip_trivia())
        {
            while (!arm.done() && !arm.at("=>"))
            {
                arm.skip_token();
            }

            if (arm.done())
            {
                fail("its result `" + std::string{value} + "` is not visibly a token or a skip");
            }

            arm.expect('=', "'=>' after a match arm's pattern");
            arm.expect('>', "'=>' after a match arm's pattern");

            arm.skip_trivia();

            const auto begin{arm.offset()};

            if (arm.peek() == '{')
            {
                arm.skip_group();

                outcomes.merge(of_body(arms.substr(begin + 1, arm.offset() - 2 - begin)));
            }
            else
            {
                while (!arm.done() && arm.peek() != ',')
                {
                    arm.skip_token();
                }

                outcomes.merge(of_value(arms.substr(begin, arm.offset() - begin)));
            }

            arm.skip_trivia();

            std::ignore = arm.accept(',');
        }

        return outcomes;
    }

    const auto compact{compacted(value)};

    // `None` is an error at the boundary whatever the payload; a bool is a result for a variant without one.
    if (compact == "None")
    {
        return {emits()};
    }

    if (compact == "true" || compact == "false")
    {
        if (!unit())
        {
            fail("its result `" + std::string{value} +
                 "` is a bool, which logos 0.15.1 takes from a callback only for a variant without a payload, and " +
                 written_variant() + " carries one");
        }

        return {emits()};
    }

    // A constructor: its path up to the parenthesis, which must close the value.
    if (const auto paren{value.find('(')}; paren != std::string_view::npos && is_group(value.substr(paren), '('))
    {
        if (const auto constructor{canonical(context_.names, compacted(value.substr(0, paren)))};
            std::ranges::contains(constructors, std::string_view{constructor}))
        {
            return {of_argument(constructor, value.substr(paren + 1, value.size() - 2 - paren))};
        }
    }

    return {of_result(value)};
}

std::optional<std::string> Callback_reader::enum_variant(const std::string_view text) const
{
    const auto compact{compacted(text)};

    const auto prefix{
            compact.starts_with("Self::") && self_is_enum_ ? std::string{"Self::"} : std::string{context_.name} + "::"};

    if (!compact.starts_with(prefix))
    {
        return std::nullopt;
    }

    const std::string_view rest{std::string_view{compact}.substr(prefix.size())};

    const auto name_end{std::ranges::find_if_not(rest, is_word_byte) - rest.begin()};

    const auto name{rest.substr(0, static_cast<std::size_t>(name_end))};

    // A name that is no variant's is an associated function's, `T::make(lex)`, whose result is out of sight.
    if (!std::ranges::contains(context_.variants, name))
    {
        return std::nullopt;
    }

    const auto after{rest.substr(name.size())};

    if (after.empty() || is_group(after, '('))
    {
        return std::string{name};
    }

    return std::nullopt;
}

void Callback_reader::collect_returns(const std::string_view text, Outcomes_t& outcomes) const
{
    Rust_cursor cursor{text, 0, text.size()};

    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        if (cursor.at_string())
        {
            cursor.skip_token();

            continue;
        }

        if (cursor.peek() == '(' || cursor.peek() == '[' || cursor.peek() == '{')
        {
            const auto open{cursor.offset()};

            cursor.skip_group();

            collect_returns(text.substr(open + 1, cursor.offset() - 2 - open), outcomes);

            continue;
        }

        const auto word{cursor.word()};

        if (word == "return")
        {
            // The value runs to the semicolon or the comma of a match arm outside any group, or to the end of the
            // block; its own blocks are read by of_value().
            cursor.skip_trivia();

            const auto begin{cursor.offset()};

            while (!cursor.done() && cursor.peek() != ';' && cursor.peek() != ',')
            {
                cursor.skip_token();
            }

            outcomes.merge(of_value(text.substr(begin, cursor.offset() - begin)));
        }
        else if (word.empty())
        {
            cursor.skip_token();
        }
    }
}

Outcome Callback_reader::emits() const
{
    return {.skips = false, .token = variant_ ? variant_->name : std::string{}};
}

void Callback_reader::fail(const std::string& why) const
{
    throw Spec_error{"the callback `" + std::string{callback_} + "` is refused: " + why, line_};
}

/**
 * @brief The unsigned decimal a `priority = n` gives.
 * @param text The value's text.
 * @param cursor The cursor, for the refusal's line.
 * @return The number.
 * @throws Spec_error If the text is not a decimal that fits.
 */
[[nodiscard]] std::size_t unsigned_value(const std::string_view text, const Rust_cursor& cursor)
{
    std::size_t value{0};

    for (const auto byte : text)
    {
        const auto digit{static_cast<std::size_t>(byte - '0')};

        if (!is_digit(byte) || value > (std::numeric_limits<std::size_t>::max() - digit) / 10)
        {
            cursor.fail("priority expects an unsigned integer, got '" + std::string{text} + "'");
        }

        value = value * 10 + digit;
    }

    if (text.empty())
    {
        cursor.fail("priority expects an unsigned integer");
    }

    return value;
}

/**
 * @brief Reads the attribute opening at the cursor, `#[path]`, `#[path(...)]` or `#[path = ...]`.
 * @param cursor The cursor, at the `#`.
 * @return The attribute.
 * @throws Spec_error If the attribute is left open.
 */
[[nodiscard]] Attribute read_attribute(Rust_cursor& cursor)
{
    const auto line{cursor.line()};

    cursor.expect('#', "'#'");

    cursor.expect('[', "'[' to open the attribute");

    cursor.skip_trivia();

    std::string path{cursor.word()};

    for (cursor.skip_trivia(); cursor.at("::"); cursor.skip_trivia())
    {
        cursor.expect(':', "':'");
        cursor.expect(':', "':'");

        cursor.skip_trivia();

        path += "::" + std::string{cursor.word()};
    }

    if (path.empty())
    {
        cursor.fail("expected the attribute's name after '#['");
    }

    auto begin{cursor.offset()};

    auto end{begin};

    const auto delimited{cursor.peek() == '(' || cursor.peek() == '[' || cursor.peek() == '{'};

    if (delimited)
    {
        begin = cursor.offset() + 1;

        cursor.skip_group();

        end = cursor.offset() - 1;
    }
    else
    {
        while (!cursor.done() && cursor.peek() != ']')
        {
            cursor.skip_token();
        }
    }

    cursor.skip_trivia();

    cursor.expect(']', "']' to close the attribute");

    return {.path = std::move(path), .begin = begin, .end = end, .delimited = delimited, .line = line};
}

/**
 * @brief Reads the content of a `#[token(...)]`, a `#[regex(...)]` or a `skip(...)`: the literal, then a callback
 *        in first position or as `callback = ...`, `priority = n` and, on a token or a regex, `ignore(case)` or
 *        `ignore(ascii_case)`, comma separated, as logos reads them.
 *
 * What logos 0.15.1 refuses of the arguments is refused in its words (logos-codegen 0.15.1, parser/definition.rs,
 * parser/skip.rs and parser/nested.rs): a second `priority`, "Resetting previously set priority"; a second callback,
 * positional or named, "Callback has been already set"; `priority(...)` and `callback(...)`, which expect `= value`;
 * an argument logos does not know, an unknown nested attribute; and any argument after `ignore(...)`, since the crate
 * leaves the comma after the group unread and reads what follows as an unnamed argument out of place, "Expected a named
 * argument at this position".
 * @param content A cursor over the content.
 * @param attribute The attribute's name, `token`, `regex` or `skip`, for refusals.
 * @param skip Whether the content is a `skip(...)`'s, on which logos 0.15.1 knows no `ignore` and calls it an
 *        unknown nested attribute (logos-codegen 0.15.1, parser/skip.rs).
 * @return The definition.
 * @throws Spec_error If the literal is missing, an argument is one logos does not know or stands where logos
 *         refuses it, `ignore` names a flag logos has not got, or an argument is given twice.
 */
[[nodiscard]] Definition read_definition(Rust_cursor content, const std::string_view attribute, const bool skip)
{
    const auto line{content.line()};

    content.skip_trivia();

    if (content.done())
    {
        content.fail(
                skip ? std::string{"logos 0.15.1 refuses an empty skip(...): Expected #[logos(skip(\"regex literal\"[, "
                                   "[callback = ] callback, priority = priority]))]"} :
                       "logos 0.15.1 refuses an empty #[" + std::string{attribute} + "(...)]: Expected #[" +
                                std::string{attribute} + "(...)]");
    }

    Definition definition{
            .literal = content.literal(),
            .callback = {},
            .priority = std::nullopt,
            .folding = Ignore_case::none,
            .line = line};

    // Whether a callback and a priority have been given, and whether an `ignore(...)` closed the arguments.
    auto callback_given{false};

    auto priority_given{false};

    auto ignored{false};

    for (std::size_t position{0};; ++position)
    {
        content.skip_trivia();

        if (content.done())
        {
            return definition;
        }

        content.expect(',', "',' between the attribute's arguments");

        content.skip_trivia();

        if (content.done())
        {
            return definition;
        }

        if (ignored)
        {
            content.fail(
                    "logos 0.15.1 refuses an argument after ignore(...) in one attribute, the comma after the group "
                    "being left unread: Expected a named argument at this position; write the argument before "
                    "ignore(...)");
        }

        // One argument runs to the next comma outside any group, string or closure body.
        const auto begin{content.offset()};

        while (!content.done() && content.peek() != ',')
        {
            content.skip_token();
        }

        const auto end{content.offset()};

        // The text from an offset to the argument's end, its trailing blanks dropped.
        const auto rest{[&content, end](const std::size_t from) {
            auto text{content.slice(from, end)};

            while (!text.empty() && is_blank(text.back()))
            {
                text.remove_suffix(1);
            }

            return std::string{text};
        }};

        auto item{content.inside(begin, end)};

        const std::string key{item.word()};

        item.skip_trivia();

        if (!key.empty() && item.peek() == '=' && !item.at("=="))
        {
            item.expect('=', "'='");

            item.skip_trivia();

            if (key == "priority")
            {
                if (std::exchange(priority_given, true))
                {
                    item.fail("logos 0.15.1 refuses a second priority: Resetting previously set priority");
                }

                definition.priority = unsigned_value(rest(item.offset()), item);
            }
            else if (key == "callback")
            {
                if (std::exchange(callback_given, true))
                {
                    item.fail("logos 0.15.1 refuses a second callback: Callback has been already set");
                }

                definition.callback = rest(item.offset());
            }
            else
            {
                // logos 0.15.1 knows `priority`, `callback` and `ignore` and calls anything else an unknown nested
                // attribute, `allow_greedy` among them.
                item.fail("logos knows no argument '" + key + "'; expected callback, priority or ignore");
            }
        }
        else if ((key == "priority" || key == "callback") && item.peek() == '(')
        {
            item.fail(
                    "logos 0.15.1 refuses " + key +
                    "(...): Expected: " + (key == "priority" ? "priority = <integer>" : "callback = ..."));
        }
        else if (key == "ignore" && item.peek() == '(')
        {
            if (skip)
            {
                item.fail("logos knows no argument 'ignore' on skip(...); expected callback or priority");
            }

            auto flags{item.inside(item.offset() + 1, end)};

            for (flags.skip_trivia(); !flags.done() && flags.peek() != ')'; flags.skip_trivia())
            {
                const auto flag{flags.word()};

                if (flag != "case" && flag != "ascii_case")
                {
                    flags.fail("ignore knows no flag '" + std::string{flag} + "'; expected case or ascii_case");
                }

                const auto asked{flag == "case" ? Ignore_case::unicode : Ignore_case::ascii};

                if (definition.folding != Ignore_case::none && definition.folding != asked)
                {
                    flags.fail("logos refuses the flag case along with ascii_case");
                }

                definition.folding = asked;

                flags.skip_trivia();

                if (!flags.accept(','))
                {
                    break;
                }
            }

            ignored = true;
        }
        else if (position == 0)
        {
            callback_given = true;

            definition.callback = rest(begin);
        }
        else
        {
            item.fail(
                    "expected a named argument at this position; a callback after the first argument is written "
                    "callback = ...");
        }
    }
}

/**
 * @brief Whether an option with a key, `extras`, `error`, `source` or `type S`, has been recorded already.
 * @param options The options recorded so far.
 * @param key The key, `type S` for a type parameter's assignment.
 * @return True when one has.
 */
[[nodiscard]] bool option_given(const std::vector<std::string>& options, const std::string& key)
{
    return std::ranges::any_of(options, [&key](const std::string& option) {
        return option == key || option.starts_with(key + "=") || option.starts_with(key + "(");
    });
}

/**
 * @brief Reads the content of one `#[logos(...)]`: its skips into the definitions, its subpatterns compiled in
 *        order into the specification's definitions and the table, and every other key into the options.
 *
 * What logos 0.15.1 refuses of the entries is refused in its words (logos-codegen 0.15.1, parser/mod.rs and
 * parser/nested.rs): a bare key, "Invalid nested attribute"; a key with a value of the wrong shape, `extras(T)`,
 * `skip = "x"` or `type = T`, each with the shape expected; and `extras`, `error`, `source` and the type of one
 * parameter given twice, across the enum's attributes as within one, "can be defined only once".
 * @param content A cursor over the content.
 * @param spec The specification being filled.
 * @param subpatterns The subpatterns declared so far, added to.
 * @param skips The skip definitions collected so far, added to.
 * @param names The names bound for the enum, which a `crate = path` entry binds the path to the crate in.
 * @throws Spec_error If an entry is malformed, given twice where the crate takes one, a subpattern is declared twice
 *         or its name would read as a count.
 */
void read_logos_attribute(
        Rust_cursor content, Lexer_spec& spec, Subpatterns_t& subpatterns, std::vector<Definition>& skips,
        Names_t& names)
{
    for (content.skip_trivia(); !content.done(); content.skip_trivia())
    {
        const auto begin{content.offset()};

        const auto line{content.line()};

        const std::string key{content.word()};

        if (key.empty())
        {
            content.fail("expected a key in #[logos(...)]");
        }

        content.skip_trivia();

        // Whether the entry's value is a parenthesised group, `skip(...)` or `error(...)`.
        const auto group_valued{content.peek() == '('};

        // The shape of the value: `= value`, `(group)`, `name = value`, a literal, or nothing at all, which is a
        // bare key and an invalid nested attribute to the crate whatever the key.
        const auto assigned{content.peek() == '=' && !content.at("==")};

        if (content.done() || content.peek() == ',')
        {
            content.fail("logos 0.15.1 refuses a bare `" + key + "` in #[logos(...)]: Invalid nested attribute");
        }

        // The shape each key expects, in the crate's words, for a key given another.
        const auto expects{[&content, &key, group_valued, assigned](
                                   const bool assign, const bool group, const bool keyword,
                                   const std::string_view expected) {
            const auto shape{assigned ? assign : group_valued ? group : content.at_string() ? key == "skip" : keyword};

            if (!shape)
            {
                content.fail("logos 0.15.1 refuses this shape of `" + key + "`: " + std::string{expected});
            }
        }};

        if (key == "skip")
        {
            expects(false, true, false, R"(Expected: #[logos(skip "regex literal")] or #[logos(skip(...))])");
        }

        if (key == "skip" && content.peek() == '(')
        {
            const auto open{content.offset()};

            content.skip_group();

            skips.push_back(read_definition(content.inside(open + 1, content.offset() - 1), "skip", true));
        }
        else if (key == "skip")
        {
            skips.push_back(
                    {.literal = content.literal(),
                     .callback = {},
                     .priority = std::nullopt,
                     .folding = Ignore_case::none,
                     .line = line});
        }
        else if (key == "subpattern")
        {
            expects(false, false, true, R"(Expected: #[logos(subpattern name = r"regex")])");

            const std::string name{content.word()};

            content.skip_trivia();

            content.expect('=', "'=' after the subpattern's name");

            content.skip_trivia();

            if (name.empty() || is_digit(name.front()))
            {
                content.fail(
                        "a subpattern needs a name that opens with a letter or an underscore, since {" + name +
                        "} would read as a count");
            }

            if (subpatterns.contains(name))
            {
                content.fail("the subpattern '" + name + "' is declared twice");
            }

            auto literal{content.literal()};

            auto expression{
                    compile(literal, Pattern_kind::definition, Ignore_case::none, subpatterns, line).expression};

            auto text{substituted(literal, subpatterns, line)};

            subpatterns.insert_or_assign(
                    name,
                    Subpattern{.literal = std::move(literal), .text = std::move(text), .empty = expression.empty()});

            spec.definitions.insert_or_assign(name, std::move(expression));
        }

        while (!content.done() && content.peek() != ',')
        {
            content.skip_token();
        }

        if (key != "skip" && key != "subpattern")
        {
            // logos 0.15.1 knows eight keys and calls any other an unknown nested attribute (logos-codegen 0.15.1,
            // parser/mod.rs); `utf8` among the others is a later crate's.
            constexpr std::array known{std::string_view{"crate"},      std::string_view{"error"},
                                       std::string_view{"export_dir"}, std::string_view{"extras"},
                                       std::string_view{"source"},     std::string_view{"type"}};

            if (!std::ranges::contains(known, std::string_view{key}))
            {
                throw Spec_error{
                        "logos 0.15.1 knows no #[logos(" + key +
                                ")] attribute; expected one of: crate, error, export_dir, extras, skip, source, "
                                "subpattern, type",
                        line};
            }

            // The key, then the rest without its trivia, `extras=Extras`, `error(E,callback=f)`, `type S=&str`.
            const auto rest{compacted(content.slice(begin + key.size(), content.offset()))};

            const auto option{key + (rest.starts_with('=') || rest.starts_with('(') ? "" : " ") + rest};

            // The shape each key takes, and the keys the crate takes once: `extras`, `error` and `source`, and the
            // type of each parameter.
            const auto shaped{
                    key == "crate"      ? assigned :
                    key == "error"      ? assigned || group_valued :
                    key == "export_dir" ? assigned :
                    key == "extras"     ? assigned :
                    key == "source"     ? assigned :
                                          !assigned && !group_valued};

            if (!shaped || (key == "type" && option.find('=') == std::string::npos))
            {
                const auto expected{
                        key == "crate" ?
                                "Expected: #[logos(crate = path::to::logos)]" :
                        key == "error" ?
                                "Expected: #[logos(error = SomeType)] or #[logos(error(SomeType[, callback))]" :
                        key == "export_dir" ? R"(Expected #[logos(export_dir = "path/to/export/dir")])" :
                        key == "extras"     ? "Expected: #[logos(extras = SomeType)]" :
                        key == "source"     ? "Expected: #[logos(source = SomeType)]" :
                                              "Expected: #[logos(type T = SomeType)]"};

                throw Spec_error{"logos 0.15.1 refuses this shape of `" + key + "`: " + expected, line};
            }

            // A value must follow the `=`; the crate's parse of the type says so.
            if (rest.ends_with('='))
            {
                throw Spec_error{"logos 0.15.1 refuses `" + option + "` with nothing after it: expected type", line};
            }

            const auto once{key == "type" ? option.substr(0, option.find('=')) : key};

            if ((key == "extras" || key == "error" || key == "source" || key == "type") &&
                option_given(spec.options, once))
            {
                const auto what{
                        key == "extras" ? "Extras" :
                        key == "error"  ? "Error type" :
                        key == "source" ? "Source" :
                                          once.substr(5)};

                throw Spec_error{
                        "logos 0.15.1 refuses a second `" + once + "`: " + what +
                                (key == "type" ? " can only have one type assigned to it" :
                                                 " can be defined only once"),
                        line};
            }

            spec.options.push_back(option);

            // `crate = path` is where the enum's generated code finds the crate, and so where a callback may too.
            if (key == "crate" && rest.starts_with('='))
            {
                names.insert_or_assign(rest.substr(rest.starts_with("=::") ? 3 : 1), "logos");
            }
        }

        if (!content.done())
        {
            // logos 0.15.1 leaves the comma after `key(...)` unread, so the entry after it begins with that comma
            // and is an invalid nested attribute to it (logos-codegen 0.15.1, parser/nested.rs).
            if (group_valued)
            {
                content.fail(
                        "logos 0.15.1 refuses an entry after " + key +
                        "(...) in one #[logos(...)] as an invalid nested attribute; write it in a #[logos(...)] of "
                        "its own");
            }

            content.expect(',', "',' between the entries of #[logos(...)]");
        }
    }
}

/**
 * @brief The fields of a variant's tuple, split at the commas outside groups and generics, each without its trivia
 *        and the attributes before its type; a trailing comma closes the last field rather than opening another.
 * @param fields A cursor over the text between the parentheses.
 * @return The fields' types.
 * @throws Spec_error If a group is left open.
 */
[[nodiscard]] std::vector<std::string> tuple_fields(Rust_cursor fields)
{
    std::vector<std::string> types;

    for (fields.skip_trivia(); !fields.done(); fields.skip_trivia())
    {
        while (fields.at("#["))
        {
            std::ignore = fields.next("'#'");

            fields.skip_group();

            fields.skip_trivia();
        }

        const auto begin{fields.offset()};

        while (!fields.done() && fields.peek() != ',')
        {
            if (fields.peek() == '<')
            {
                skip_generics(fields);
            }
            else
            {
                fields.skip_token();
            }
        }

        types.push_back(compacted(fields.slice(begin, fields.offset())));

        std::ignore = fields.accept(',');
    }

    return types;
}

/**
 * @brief Reads the payload of the variant at the cursor's group: the one field's type without its trivia.
 *
 * logos 0.15.1 takes a variant with one unnamed field or none, and refuses, whether the variant carries a pattern
 * or not, one with several, "Logos currently only supports variants with one field", and one with named fields,
 * "Logos doesn't support named fields yet" (logos-codegen 0.15.1, lib.rs).
 * @param cursor The cursor, at the `(` or `{` after the variant's name.
 * @return The payload's text.
 * @throws Spec_error If the group is left open, or the fields are several or named.
 */
[[nodiscard]] std::string variant_payload(Rust_cursor& cursor)
{
    const auto line{cursor.line()};

    const auto open{cursor.offset()};

    cursor.skip_group();

    if (cursor.slice(open, open + 1) == "{")
    {
        throw Spec_error{"logos 0.15.1 refuses named fields: Logos doesn't support named fields yet", line};
    }

    auto fields{tuple_fields(cursor.inside(open + 1, cursor.offset() - 1))};

    if (fields.size() != 1)
    {
        throw Spec_error{
                "logos 0.15.1 refuses the variant: Logos currently only supports variants with one field, found " +
                        std::to_string(fields.size()),
                line};
    }

    return std::move(fields.front());
}

/**
 * @brief Adds one rule to a specification.
 * @param spec The specification.
 * @param definition The attribute's definition.
 * @param kind What the pattern is to logos, a token matched as it stands or a regex.
 * @param variant The variant, or std::nullopt for a skip, whose callback has no result to read, every result the
 *        crate admits there skipping or failing, and is read for its use of the lexer alone.
 * @param subpatterns The subpatterns declared.
 * @param context The enum, its variants, and what the file defines and binds.
 * @throws Spec_error If the pattern is refused, the callback's effect is not decidable from the source, or the
 *         callback moves the lexer.
 */
void add_rule(
        Lexer_spec& spec, const Definition& definition, const Pattern_kind kind, const std::optional<Variant>& variant,
        const Subpatterns_t& subpatterns, const Enum_context& context)
{
    const auto [expression, computed]{
            compile(definition.literal, kind, definition.folding, subpatterns, definition.line)};

    const auto emitted{Callback_reader{context, variant, definition.callback, definition.line}.token()};

    spec.rules.push_back(
            {.pattern = definition.literal.written,
             .expression = expression,
             .conditions = {},
             .action = definition.callback,
             .token = emitted,
             .priority = definition.priority.value_or(computed),
             .line = definition.line});
}

/**
 * @brief Reads the enum after its `enum` keyword into a specification: its `#[logos]` attributes, then its
 *        variants with their `#[token]` and `#[regex]` attributes, the variants all read before any callback is,
 *        since a callback's result may name any of them.
 * @param cursor The cursor, just past `enum`.
 * @param attributes The enum's outer attributes.
 * @param line The line of the derive naming Logos.
 * @param items What the file defines and binds, which a callback may name.
 * @return The specification.
 * @throws Spec_error If the enum is malformed or left open, or an attribute, pattern or callback is refused.
 */
[[nodiscard]] Lexer_spec read_enum(
        Rust_cursor& cursor, const std::vector<Attribute>& attributes, const std::size_t line, const Items& items)
{
    Lexer_spec spec;

    spec.line = line;

    // Which language the classes were read as: `\d`, `\s` and `\w` are a Unicode version's, so the account of the
    // scanner names the one this reading modelled, the crate's own.
    spec.options.emplace_back("unicode-classes=" + std::string{class_unicode_version});

    cursor.skip_trivia();

    const std::string enum_name{cursor.word()};

    if (enum_name.empty())
    {
        cursor.fail("expected the enum's name");
    }

    // The generic parameters: logos 0.15.1 takes one lifetime and type parameters each given a concrete type by
    // `#[logos(type T = ...)]`, and refuses const generics and a second lifetime (logos-codegen 0.15.1,
    // parser/mod.rs and parser/type_params.rs).
    std::vector<std::string> type_parameters;

    cursor.skip_trivia();

    if (cursor.peek() == '<')
    {
        const auto open{cursor.offset()};

        skip_generics(cursor);

        std::size_t lifetimes{0};

        // One parameter up to each comma outside nested generics: a lifetime, `const N: usize`, or a type.
        for (auto parameter{cursor.inside(open + 1, cursor.offset() - 1)}; parameter.skip_trivia(), !parameter.done();
             std::ignore = parameter.accept(','))
        {
            const auto first{parameter.word()};

            if (parameter.at("'"))
            {
                if (++lifetimes > 1)
                {
                    throw Spec_error{
                            "logos 0.15.1 refuses a second lifetime: Logos types can only have one lifetime", line};
                }
            }
            else if (first == "const")
            {
                throw Spec_error{"logos 0.15.1 refuses const generics: Logos doesn't support const generics.", line};
            }
            else if (!first.empty())
            {
                type_parameters.emplace_back(first);
            }

            while (!parameter.done() && parameter.peek() != ',')
            {
                if (parameter.peek() == '<')
                {
                    skip_generics(parameter);
                }
                else
                {
                    parameter.skip_token();
                }
            }
        }
    }

    while (!cursor.done() && cursor.peek() != '{')
    {
        cursor.skip_token();
    }

    cursor.expect('{', "'{' to open the enum");

    Subpatterns_t subpatterns;

    std::vector<Definition> skips;

    // The file's names, and the path the enum's own attribute gives the crate.
    auto names{items.names};

    for (const auto& attribute : attributes)
    {
        if (attribute.path != "logos")
        {
            continue;
        }

        if (!attribute.delimited)
        {
            throw Spec_error{
                    "logos 0.15.1 refuses a #[logos] without its parentheses: Expected #[logos(...)]", attribute.line};
        }

        read_logos_attribute(cursor.inside(attribute.begin, attribute.end), spec, subpatterns, skips, names);
    }

    // Each `type T = ...` must name a parameter and each parameter must have one.
    for (const auto& option : spec.options)
    {
        if (option.starts_with("type ") &&
            !std::ranges::contains(type_parameters, option.substr(5, option.find('=') - 5)))
        {
            throw Spec_error{
                    "logos 0.15.1 refuses the assignment: " + option.substr(5, option.find('=') - 5) +
                            " is not a declared type parameter",
                    line};
        }
    }

    for (const auto& parameter : type_parameters)
    {
        if (!option_given(spec.options, "type " + parameter))
        {
            throw Spec_error{
                    "logos 0.15.1 refuses the enum: Generic type parameter without a concrete type; define a "
                    "concrete type Logos can use: #[logos(type " +
                            parameter + " = Type)]",
                    line};
        }
    }

    // The variants, each with its rules' definitions, a token's marked.
    std::vector<std::pair<Variant, std::vector<std::pair<Definition, bool>>>> variants;

    for (cursor.skip_trivia(); !cursor.accept('}'); cursor.skip_trivia())
    {
        if (cursor.done())
        {
            cursor.fail("the enum is never closed");
        }

        std::vector<std::pair<Definition, bool>> definitions;

        while (cursor.at("#["))
        {
            const auto attribute{read_attribute(cursor)};

            if (attribute.path == "token" || attribute.path == "regex")
            {
                definitions.emplace_back(
                        read_definition(cursor.inside(attribute.begin, attribute.end), attribute.path, false),
                        attribute.path == "token");
            }
            else if (attribute.path == "error")
            {
                // The error variant of logos 0.12 and before; 0.13 and later refuse the attribute (logos-codegen
                // 0.15.1, lib.rs).
                throw Spec_error{
                        "logos 0.15.1 refuses #[error]: Since 0.13 Logos no longer requires the #[error] variant",
                        attribute.line};
            }

            cursor.skip_trivia();
        }

        Variant variant{.name = std::string{cursor.word()}, .payload = {}};

        if (variant.name.empty())
        {
            cursor.fail("expected a variant's name");
        }

        cursor.skip_trivia();

        if (cursor.peek() == '(' || cursor.peek() == '{')
        {
            variant.payload = canonical_type(names, variant_payload(cursor));

            cursor.skip_trivia();
        }

        if (cursor.accept('='))
        {
            while (!cursor.done() && cursor.peek() != ',' && cursor.peek() != '}')
            {
                cursor.skip_token();
            }
        }

        cursor.skip_trivia();

        if (cursor.peek() != '}')
        {
            cursor.expect(',', "',' or '}' after the variant '" + variant.name + "'");
        }

        variants.emplace_back(std::move(variant), std::move(definitions));
    }

    std::vector<std::string> variant_names;

    for (const auto& [variant, definitions] : variants)
    {
        variant_names.push_back(variant.name);
    }

    const Enum_context context{
            .name = enum_name,
            .variants = variant_names,
            .functions = items.functions,
            .names = names};

    for (const auto& skip : skips)
    {
        add_rule(spec, skip, Pattern_kind::regex, std::nullopt, subpatterns, context);
    }

    for (const auto& [variant, definitions] : variants)
    {
        for (const auto& [definition, token] : definitions)
        {
            add_rule(
                    spec, definition, token ? Pattern_kind::token : Pattern_kind::regex, variant, subpatterns, context);
        }
    }

    return spec;
}

/**
 * @brief The line of the attribute among a list of outer attributes whose derive names Logos, bare or by path.
 * @param attributes The attributes.
 * @param cursor The cursor over the file the offsets index.
 * @return The line, or std::nullopt when none does.
 */
[[nodiscard]] std::optional<std::size_t> derives_logos(
        const std::vector<Attribute>& attributes, const Rust_cursor& cursor)
{
    for (const auto& attribute : attributes)
    {
        if (attribute.path != "derive")
        {
            continue;
        }

        auto list{cursor.inside(attribute.begin, attribute.end)};

        for (list.skip_trivia(); !list.done(); list.skip_trivia())
        {
            std::string name{list.word()};

            for (list.skip_trivia(); list.at("::"); list.skip_trivia())
            {
                list.expect(':', "':'");
                list.expect(':', "':'");

                list.skip_trivia();

                name = std::string{list.word()};
            }

            if (name == "Logos")
            {
                return attribute.line;
            }

            if (!list.done())
            {
                list.expect(',', "',' between the derives");
            }
        }
    }

    return std::nullopt;
}

void Scalar_set::add(const char32_t low, const char32_t high)
{
    spans_gap_ = spans_gap_ || (low <= 0xD7FF && high >= 0xE000);

    ranges_.emplace_back(low, high);

    std::ranges::sort(ranges_);

    std::vector<Range_t> merged;

    for (const auto& [from, to] : ranges_)
    {
        if (merged.empty() || from > merged.back().second + 1)
        {
            merged.emplace_back(from, to);

            continue;
        }

        auto& [low, high]{merged.back()};

        high = std::max(high, to);
    }

    ranges_ = std::move(merged);
}

void Scalar_set::add(const Scalar_set& other)
{
    spans_gap_ = spans_gap_ || other.spans_gap_;

    for (const auto& [low, high] : other.ranges_)
    {
        add(low, high);
    }
}

Scalar_set Scalar_set::minus(const Scalar_set& other) const
{
    Scalar_set difference;

    difference.spans_gap_ = spans_gap_ && !other.contains(0xD7FF) && !other.contains(0xE000);

    for (const auto& [low, high] : ranges_)
    {
        auto from{low};

        for (const auto& [cut_low, cut_high] : other.ranges_)
        {
            if (cut_high < from)
            {
                continue;
            }

            if (cut_low > high)
            {
                break;
            }

            if (cut_low > from)
            {
                difference.add(from, cut_low - 1);
            }

            if (cut_high >= high)
            {
                from = high + 1;

                break;
            }

            from = std::max(from, static_cast<char32_t>(cut_high + 1));
        }

        if (from <= high)
        {
            difference.add(from, high);
        }
    }

    return difference;
}

const std::vector<Scalar_set::Range_t>& Scalar_set::ranges() const noexcept
{
    return ranges_;
}

bool Scalar_set::contains(const char32_t value) const noexcept
{
    return std::ranges::any_of(ranges_, [value](const Range_t& range) {
        const auto& [low, high]{range};

        return low <= value && value <= high;
    });
}

bool Scalar_set::empty() const noexcept
{
    return ranges_.empty();
}

std::optional<char32_t> Scalar_set::single() const noexcept
{
    if (ranges_.size() != 1)
    {
        return std::nullopt;
    }

    const auto& [low, high]{ranges_.front()};

    return low == high ? std::optional{low} : std::nullopt;
}

bool Scalar_set::spans_gap() const noexcept
{
    return spans_gap_;
}

Rust_cursor::Rust_cursor(const std::string_view text, const std::size_t begin, const std::size_t end)
    : Cursor{text, begin, end}
{}

void Rust_cursor::skip_trivia()
{
    for (;;)
    {
        while (peek() && is_blank(*peek()))
        {
            ++at_;
        }

        if (at("//"))
        {
            while (peek() && *peek() != '\n')
            {
                ++at_;
            }
        }
        else if (at("/*"))
        {
            const auto line{this->line()};

            // Rust nests block comments.
            std::size_t depth{0};

            do
            {
                if (at_ >= end_)
                {
                    throw Spec_error{"a block comment is never closed", line};
                }

                if (at("/*"))
                {
                    ++depth;

                    at_ += 2;
                }
                else if (at("*/"))
                {
                    --depth;

                    at_ += 2;
                }
                else
                {
                    ++at_;
                }
            } while (depth > 0);
        }
        else
        {
            return;
        }
    }
}

void Rust_cursor::skip_token()
{
    if (at("//") || at("/*"))
    {
        skip_trivia();

        return;
    }

    if (const auto end{string_end()})
    {
        at_ = *end;

        return;
    }

    if (at("'") || at("b'"))
    {
        // A character literal closes after one scalar or one escape; a lifetime or label does not close at all.
        const auto open{at_ + (at("b'") ? 2 : 1)};

        auto close{open};

        if (close < end_ && text_[close] == '\\')
        {
            close = std::min(text_.find('\'', close + 2), end_);
        }
        else if (close < end_)
        {
            const auto lead{static_cast<unsigned char>(text_[close])};

            close += lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        }

        at_ = close < end_ && text_[close] == '\'' ? close + 1 : open;

        return;
    }

    if (peek() == '(' || peek() == '[' || peek() == '{')
    {
        skip_group();

        return;
    }

    if (word().empty())
    {
        ++at_;
    }
}

void Rust_cursor::skip_group()
{
    const auto open{next("a group")};

    const auto close{open == '(' ? ')' : open == '[' ? ']' : '}'};

    const auto line{this->line()};

    for (skip_trivia(); !accept(close); skip_trivia())
    {
        if (done())
        {
            throw Spec_error{std::string{"the '"} + open + "' is never closed", line};
        }

        if (peek() == ')' || peek() == ']' || peek() == '}')
        {
            fail(std::string{"'"} + *peek() + "' closes nothing, '" + close + "' was expected");
        }

        skip_token();
    }
}

std::string_view Rust_cursor::word()
{
    const auto begin{at_};

    if (at("r#") && at_ + 2 < end_ && is_word_byte(text_[at_ + 2]))
    {
        at_ += 2;
    }

    while (peek() && is_word_byte(*peek()))
    {
        ++at_;
    }

    return text_.substr(begin, at_ - begin);
}

Literal Rust_cursor::literal()
{
    const auto begin{at_};

    const auto end{string_end()};

    if (!end)
    {
        fail("expected a string literal");
    }

    if (peek() == 'c')
    {
        fail("a C string is no pattern; logos takes a &str or a &[u8] literal");
    }

    const auto byte_string{accept('b')};

    const auto raw{accept('r')};

    std::size_t hashes{0};

    while (accept('#'))
    {
        ++hashes;
    }

    expect('"', R"('"')");

    const auto content_end{*end - hashes - 1};

    Literal literal{.written = std::string{slice(begin, *end)}, .bytes = {}, .byte_string = byte_string};

    if (raw)
    {
        literal.bytes = slice(at_, content_end);

        at_ = *end;

        return literal;
    }

    while (at_ < content_end)
    {
        const auto byte{next("the string's content")};

        if (byte != '\\')
        {
            literal.bytes.push_back(byte);

            continue;
        }

        switch (const auto escaped{next("the escape")}; escaped)
        {
        case 'n':
            literal.bytes.push_back('\n');
            break;
        case 'r':
            literal.bytes.push_back('\r');
            break;
        case 't':
            literal.bytes.push_back('\t');
            break;
        case '0':
            literal.bytes.push_back('\0');
            break;
        case '\\':
        case '\'':
        case '"':
            literal.bytes.push_back(escaped);
            break;
        case 'x':
        {
            const auto high{next("a hex digit")};

            const auto low{next("a hex digit")};

            if (!is_hex_digit(high) || !is_hex_digit(low))
            {
                fail(R"(\x needs two hex digits)");
            }

            const auto value{hex_value(high) * 16 + hex_value(low)};

            if (value > 0x7F && !byte_string)
            {
                fail(R"(\x in a string reaches only \x7f; a higher scalar is written \u{...})");
            }

            literal.bytes.push_back(static_cast<char>(value));

            break;
        }
        case 'u':
        {
            if (byte_string)
            {
                fail(R"(a byte string has no \u escape)");
            }

            expect('{', R"('{' after \u)");

            char32_t value{0};

            std::size_t digits{0};

            for (; peek() && is_hex_digit(*peek()); ++digits)
            {
                value = value * 16 + hex_value(next("a hex digit"));
            }

            expect('}', R"('}' to close the \u escape)");

            if (digits == 0 || digits > 6 || value > last_scalar || (value >= 0xD800 && value <= 0xDFFF))
            {
                fail(R"(\u{...} needs one to six hex digits naming a scalar)");
            }

            literal.bytes += encoded(value);

            break;
        }
        case '\n':
        case '\r':
            // A backslash ending the line continues the string on the next, its leading blanks dropped.
            while (peek() && is_blank(*peek()))
            {
                ++at_;
            }

            break;
        default:
            fail(std::string{R"('\)"} + escaped + "' is not an escape Rust knows");
        }
    }

    at_ = *end;

    return literal;
}

Rust_cursor Rust_cursor::inside(const std::size_t begin, const std::size_t end) const noexcept
{
    return {text_, begin, end};
}

std::string_view Rust_cursor::slice(const std::size_t begin, const std::size_t end) const noexcept
{
    return text_.substr(begin, end - begin);
}

bool Rust_cursor::at_string() const
{
    return string_end().has_value();
}

std::optional<std::size_t> Rust_cursor::string_end() const
{
    auto at{at_};

    // The prefixes: b for a byte string, r for a raw one, br for both; c and cr, the C strings, have no pattern
    // to give but are skipped like the rest.
    if (at < end_ && (text_[at] == 'b' || text_[at] == 'c'))
    {
        ++at;
    }

    const auto raw{at < end_ && text_[at] == 'r'};

    if (raw)
    {
        ++at;
    }

    std::size_t hashes{0};

    for (; raw && at < end_ && text_[at] == '#'; ++at)
    {
        ++hashes;
    }

    if (at >= end_ || text_[at] != '"')
    {
        return std::nullopt;
    }

    const auto line{this->line()};

    for (++at; at < end_; ++at)
    {
        if (text_[at] == '\\' && !raw)
        {
            ++at;
        }
        else if (text_[at] == '"' && at + hashes < end_ && text_.substr(at + 1, hashes) == std::string(hashes, '#'))
        {
            return at + 1 + hashes;
        }
    }

    throw Spec_error{"a string literal is never closed", line};
}

Pattern_reader::Pattern_reader(Literal literal, const Flags flags, const std::size_t line)
    : literal_{std::move(literal)}, flags_{flags}, line_{line}
{}

Node Pattern_reader::read()
{
    auto node{alternation()};

    if (peek())
    {
        fail(std::string{"unexpected '"} + *peek() + "'");
    }

    return node;
}

Node Pattern_reader::read_literal()
{
    std::vector<Node> parts;

    while (peek())
    {
        parts.push_back(unit_node(next_unit()));
    }

    return concatenated(std::move(parts));
}

Node Pattern_reader::alternation()
{
    std::vector<Node> branches;

    branches.push_back(concatenation());

    while (accept('|'))
    {
        branches.push_back(concatenation());
    }

    return alternated(std::move(branches));
}

Node Pattern_reader::concatenation()
{
    std::vector<Node> parts;

    while (peek() && *peek() != '|' && *peek() != ')')
    {
        parts.push_back(repetition());
    }

    return concatenated(std::move(parts));
}

Node Pattern_reader::repetition()
{
    auto node{atom()};

    for (;;)
    {
        std::size_t min{0};

        std::optional<std::size_t> max;

        if (accept('*'))
        {
            max = std::nullopt;
        }
        else if (accept('+'))
        {
            min = 1;
        }
        else if (accept('?'))
        {
            max = 1;
        }
        else if (accept('{'))
        {
            std::tie(min, max) = count();
        }
        else
        {
            return node;
        }

        // Only an empty group or a flag setting stands before the operator: the crate refuses the second and logos
        // has no use for the first, so both are refused rather than read as repeating nothing.
        if (std::holds_alternative<Empty>(node.kind))
        {
            fail("a repetition operator needs something before it to repeat");
        }

        // logos 0.15.1 refuses the lazy forms outright (logos-codegen 0.15.1, mir.rs), so they are refused here too
        // rather than read as the greedy form the crate never compiles.
        if (accept('?'))
        {
            fail("the lazy operator is one logos 0.15.1 refuses: non-greedy parsing is currently unsupported");
        }

        node = repeated(std::move(node), min, max);
    }
}

Node Pattern_reader::atom()
{
    const auto byte{next("a pattern")};

    switch (byte)
    {
    case '(':
        return group();
    case '[':
        return bracket();
    case '.':
    {
        Scalar_set newline;

        newline.add('\n', '\n');

        auto members{flags_.dot_all ? universe() : universe().minus(newline)};

        check_utf8(members);

        return {.kind = Class{.set = std::move(members), .unicode = flags_.unicode}};
    }
    case '\\':
    {
        auto escaped{escape()};

        if (std::holds_alternative<Unit>(escaped))
        {
            return unit_node(std::get<Unit>(escaped));
        }

        return class_node(std::move(std::get<Scalar_set>(escaped)), false);
    }
    case '*':
    case '+':
    case '?':
    case '{':
        --at_;

        fail(std::string{"nothing for '"} + byte + "' to repeat");
    case ')':
        --at_;

        fail("unexpected ')'");
    case '^':
    case '$':
        --at_;

        fail(std::string{"the anchor '"} + byte +
             "' conditions the context a match stands in, which a token language cannot say");
    default:
        --at_;

        return unit_node(next_unit());
    }
}

Node Pattern_reader::group()
{
    const auto saved{flags_};

    if (!accept('?'))
    {
        auto node{captured()};

        flags_ = saved;

        return node;
    }

    if (accept('&'))
    {
        std::string name;

        while (peek() && *peek() != ')')
        {
            name.push_back(next("the subpattern's name"));
        }

        expect(')', "')' to close the subpattern reference");

        return {.kind = Reference{.name = std::move(name), .flags = flags_}};
    }

    if (at("P<") || (peek() == '<' && !at("<=") && !at("<!")))
    {
        std::ignore = accept('P');

        while (peek() && *peek() != '>')
        {
            ++at_;
        }

        expect('>', "'>' to close the capture's name");

        auto node{captured()};

        flags_ = saved;

        return node;
    }

    if (at("=") || at("!") || at("<=") || at("<!"))
    {
        fail("lookaround conditions the context a match stands in, which a token language cannot say");
    }

    auto negated{false};

    auto dangling{false};

    auto flags{flags_};

    for (auto seen{false};; seen = true)
    {
        const auto flag{next("a flag, ':' or ')'")};

        if (flag == ':' || flag == ')')
        {
            // A colon with no flag before it opens the plain non-capturing group.
            if ((!seen && flag == ')') || dangling)
            {
                fail(!seen ? "a flag group needs a flag" : "a '-' in a flag group needs a flag after it");
            }

            // Bare, the flags hold for the rest of the enclosing group; with a colon, for the group they open.
            flags_ = flags;

            if (flag == ')')
            {
                return {.kind = Empty{}};
            }

            auto node{alternation()};

            expect(')', "')' to close the flagged group");

            flags_ = saved;

            return node;
        }

        dangling = flag == '-';

        switch (flag)
        {
        case '-':
            if (negated)
            {
                fail("a flag group takes one '-'");
            }

            negated = true;
            break;
        case 'i':
            flags.insensitive = !negated;
            break;
        case 's':
            flags.dot_all = !negated;
            break;
        case 'u':
            flags.unicode = !negated;
            break;
        case 'x':
        case 'm':
        case 'U':
        case 'R':
            fail(std::string{"the flag '"} + flag + "' is not modelled");
        default:
            fail(std::string{"'"} + flag + "' is not a flag of the regex crate");
        }
    }
}

Node Pattern_reader::captured()
{
    auto node{alternation()};

    expect(')', "')' to close the group");

    const auto bound{[](Node& edge) {
        if (std::holds_alternative<Bytes>(edge.kind))
        {
            std::get<Bytes>(edge.kind).bounded = true;
        }
    }};

    if (std::holds_alternative<Concat>(node.kind))
    {
        bound(std::get<Concat>(node.kind).parts.front());

        bound(std::get<Concat>(node.kind).parts.back());
    }
    else if (std::holds_alternative<Class>(node.kind))
    {
        // The crate compares a repetition's operand with the dot before it strips captures, so a captured class is
        // never the dot it refuses, nor is a captured alternation it would merge into one.
        std::get<Class>(node.kind).captured = true;
    }
    else if (std::holds_alternative<Choice>(node.kind))
    {
        std::get<Choice>(node.kind).captured = true;
    }
    else
    {
        bound(node);
    }

    return node;
}

Node Pattern_reader::bracket()
{
    const auto negated{accept('^')};

    return class_node(members(), negated);
}

Scalar_set Pattern_reader::members()
{
    Scalar_set members;

    // Dashes first are members, and a ']' first is one too, as the crate has it.
    auto first{true};

    while (accept('-'))
    {
        members.add('-', '-');

        first = false;
    }

    if (first && accept(']'))
    {
        members.add(']', ']');
    }

    for (;;)
    {
        if (!peek())
        {
            fail("expected ']' to close the class before the end of the pattern");
        }

        if (accept(']'))
        {
            return members;
        }

        if (at("&&") || at("--") || at("~~"))
        {
            fail("the class operator '" + literal_.bytes.substr(at_, 2) + "' is not modelled");
        }

        std::optional<char32_t> low;

        if (at("[:"))
        {
            at_ += 2;

            members.add(posix_class());
        }
        else if (accept('['))
        {
            const auto negated{accept('^')};

            const auto nested{this->members()};

            auto inner{negated ? universe().minus(cased(nested)) : nested};

            // The crate checks every bracket as it translates it, a nested one included, so a nested negation that
            // reaches beyond ASCII outside Unicode mode is refused though the outer class may not.
            check_utf8(inner);

            members.add(std::move(inner));
        }
        else if (accept('\\'))
        {
            auto escaped{escape()};

            if (std::holds_alternative<Unit>(escaped))
            {
                low = member(std::get<Unit>(escaped));
            }
            else
            {
                members.add(std::get<Scalar_set>(escaped));

                if (peek() == '-' && at_ + 1 < literal_.bytes.size() && literal_.bytes[at_ + 1] != ']' &&
                    literal_.bytes[at_ + 1] != '-')
                {
                    fail("a range cannot start with a class");
                }
            }
        }
        else
        {
            low = member(next_unit());
        }

        if (!low)
        {
            continue;
        }

        // A '-' after a member opens a range, unless the close or another '-' follows it.
        const auto after{at_ + 1 < literal_.bytes.size() ? literal_.bytes[at_ + 1] : ']'};

        if (peek() != '-' || after == ']' || after == '-')
        {
            members.add(*low, *low);

            continue;
        }

        ++at_;

        if (peek() == '[')
        {
            fail("a range cannot end in a class");
        }

        char32_t high{0};

        if (accept('\\'))
        {
            auto escaped{escape()};

            if (!std::holds_alternative<Unit>(escaped))
            {
                fail("a range cannot end in a class");
            }

            high = member(std::get<Unit>(escaped));
        }
        else
        {
            high = member(next_unit());
        }

        if (high < *low)
        {
            fail("the range ends before it starts");
        }

        members.add(*low, high);
    }
}

Scalar_set Pattern_reader::posix_class()
{
    const auto negated{accept('^')};

    std::string name;

    while (peek() && *peek() != ':')
    {
        name.push_back(next("a class name"));
    }

    expect(':', "':]' to close the class");
    expect(']', "':]' to close the class");

    Scalar_set set;

    const auto add_range{[&set](const char32_t low, const char32_t high) { set.add(low, high); }};

    if (name == "alnum" || name == "alpha" || name == "word" || name == "xdigit" || name == "upper" ||
        name == "lower" || name == "digit")
    {
        if (name != "lower" && name != "digit")
        {
            add_range('A', name == "xdigit" ? 'F' : 'Z');
        }

        if (name != "upper" && name != "digit")
        {
            add_range('a', name == "xdigit" ? 'f' : 'z');
        }

        if (name != "alpha" && name != "upper" && name != "lower")
        {
            add_range('0', '9');
        }

        if (name == "word")
        {
            add_range('_', '_');
        }
    }
    else if (name == "ascii")
    {
        add_range(0, 0x7F);
    }
    else if (name == "blank")
    {
        add_range(' ', ' ');
        add_range('\t', '\t');
    }
    else if (name == "cntrl")
    {
        add_range(0, 0x1F);
        add_range(0x7F, 0x7F);
    }
    else if (name == "graph" || name == "print" || name == "punct")
    {
        add_range(name == "print" ? ' ' : '!', '~');

        if (name == "punct")
        {
            set = set.minus([] {
                Scalar_set alphanumeric;

                alphanumeric.add('0', '9');
                alphanumeric.add('A', 'Z');
                alphanumeric.add('a', 'z');

                return alphanumeric;
            }());
        }
    }
    else if (name == "space")
    {
        add_range('\t', '\r');
        add_range(' ', ' ');
    }
    else
    {
        fail("'[:" + name + ":]' is not an ASCII class of the regex crate");
    }

    return negated ? universe().minus(cased(set)) : set;
}

std::variant<Unit, Scalar_set> Pattern_reader::escape()
{
    const auto byte{next("the escaped character")};

    switch (byte)
    {
    case 'n':
        return Unit{.value = U'\n', .byte = false};
    case 't':
        return Unit{.value = U'\t', .byte = false};
    case 'r':
        return Unit{.value = U'\r', .byte = false};
    case 'f':
        return Unit{.value = U'\f', .byte = false};
    case 'v':
        return Unit{.value = U'\v', .byte = false};
    case 'a':
        return Unit{.value = U'\a', .byte = false};
    case 'x':
    case 'u':
    case 'U':
        return hex_escape(byte);
    case 'd':
    case 's':
    case 'w':
    case 'D':
    case 'S':
    case 'W':
    {
        const auto negated{byte == 'D' || byte == 'S' || byte == 'W'};

        const auto kind{static_cast<char>(byte | 0x20)};

        Scalar_set members;

        if (flags_.unicode)
        {
            // The crate's Unicode forms: Nd, White_Space and the word class, from the tables of the database the
            // locked regex-syntax was generated from, which is the language the scanner has rather than the one the
            // library pins; the two databases differ by ten digits and thousands of word characters.
            const std::span<const Code_point_range> ranges{
                    kind == 'd' ? std::span<const Code_point_range>{decimal_digit_ranges} :
                    kind == 's' ? std::span<const Code_point_range>{white_space_ranges} :
                                  std::span<const Code_point_range>{word_ranges}};

            for (const auto& [first, last] : ranges)
            {
                members.add(first, last);
            }
        }
        else
        {
            if (kind == 'd' || kind == 'w')
            {
                members.add('0', '9');
            }

            if (kind == 'w')
            {
                members.add('A', 'Z');
                members.add('a', 'z');
                members.add('_', '_');
            }

            if (kind == 's')
            {
                members.add('\t', '\r');
                members.add(' ', ' ');
            }
        }

        return negated ? universe().minus(members) : members;
    }
    case 'p':
    case 'P':
        fail("a Unicode property class needs tables the library has not got: only the digit, space and word classes "
             "are supplied");
    case 'A':
    case 'z':
    case 'b':
    case 'B':
    case '<':
    case '>':
        fail(std::string{R"(the assertion '\)"} + byte +
             "' conditions the context a match stands in, which a token language cannot say");
    default:
        break;
    }

    if (is_digit(byte))
    {
        fail("the regex crate has neither octal escapes nor backreferences");
    }

    if (is_letter(static_cast<unsigned char>(byte)) || static_cast<unsigned char>(byte) >= 0x80)
    {
        fail(std::string{R"('\)"} + byte + "' is not an escape of the regex crate");
    }

    return Unit{.value = static_cast<unsigned char>(byte), .byte = false};
}

Unit Pattern_reader::hex_escape(const char kind)
{
    char32_t value{0};

    std::size_t digits{0};

    const auto braced{accept('{')};

    if (braced)
    {
        for (; peek() && is_hex_digit(*peek()); ++digits)
        {
            if (value > last_scalar)
            {
                fail("the hex escape exceeds any scalar");
            }

            value = value * 16 + hex_value(next("a hex digit"));
        }

        expect('}', "'}' to close the hex escape");
    }
    else
    {
        for (const auto wanted{kind == 'x' ? 2UZ : kind == 'u' ? 4UZ : 8UZ}; digits < wanted; ++digits)
        {
            const auto digit{next("a hex digit")};

            if (!is_hex_digit(digit))
            {
                fail(std::string{R"('\)"} + kind + "' needs " + std::to_string(wanted) + " hex digits, or braces");
            }

            value = value * 16 + hex_value(digit);
        }
    }

    if (digits == 0)
    {
        fail("the hex escape has no digits");
    }

    // The two-digit form is a byte outside Unicode mode; every other form names a scalar in either mode.
    const auto byte{!flags_.unicode && kind == 'x' && !braced};

    if (!byte && (value > last_scalar || (value >= 0xD800 && value <= 0xDFFF)))
    {
        fail("the hex escape names no scalar");
    }

    return {.value = value, .byte = byte};
}

char32_t Pattern_reader::member(const Unit unit)
{
    if (!unit.byte && !flags_.unicode && unit.value > 0x7F)
    {
        fail("a class outside Unicode mode holds bytes, not the encoding of a non-ASCII scalar");
    }

    // The crate refuses a byte beyond ASCII where it is written, before the class it stands in is negated.
    if (unit.byte)
    {
        Scalar_set one;

        one.add(unit.value, unit.value);

        check_utf8(one);
    }

    return unit.value;
}

std::pair<std::size_t, std::optional<std::size_t>> Pattern_reader::count()
{
    const auto min{number()};

    if (!min)
    {
        fail("a count needs a number after '{'");
    }

    if (accept('}'))
    {
        return {*min, *min};
    }

    expect(',', "',' or '}' in the count");

    if (accept('}'))
    {
        return {*min, std::nullopt};
    }

    const auto max{number()};

    if (!max)
    {
        fail("a count's upper bound needs a number");
    }

    if (*max < *min)
    {
        fail("the count's upper bound is below its lower bound");
    }

    expect('}', "'}' to close the count");

    return {*min, *max};
}

std::optional<std::size_t> Pattern_reader::number()
{
    if (!peek() || !is_digit(*peek()))
    {
        return std::nullopt;
    }

    std::size_t value{0};

    while (peek() && is_digit(*peek()))
    {
        const auto digit{static_cast<std::size_t>(next("a digit") - '0')};

        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10)
        {
            fail("the count does not fit");
        }

        value = value * 10 + digit;
    }

    return value;
}

Node Pattern_reader::unit_node(const Unit unit)
{
    const auto [value, byte]{unit};

    if (flags_.insensitive && is_letter(value))
    {
        Scalar_set letter;

        letter.add(value, value);

        return {.kind = Class{.set = folded(letter, flags_.unicode), .unicode = flags_.unicode}};
    }

    if (flags_.insensitive && flags_.unicode && value > 0x7F)
    {
        fail("the case folding of a non-ASCII scalar under (?i) is not modelled");
    }

    if (byte)
    {
        Scalar_set one;

        one.add(value, value);

        check_utf8(one);
    }

    return {.kind = Bytes{.bytes = byte ? std::string(1, static_cast<char>(value)) : encoded(value), .bounded = false}};
}

Node Pattern_reader::class_node(Scalar_set members, const bool negated)
{
    if (flags_.insensitive)
    {
        // The crate folds before it negates; folding is modelled for ASCII and the two scalars that fold to it.
        const auto foldable{std::ranges::all_of(members.ranges(), [](const Scalar_set::Range_t& range) {
            const auto& [low, high]{range};

            return high <= 0x7F || (low == high && (low == 0x17F || low == 0x212A));
        })};

        if (flags_.unicode && !foldable)
        {
            fail("the case folding of a class with non-ASCII members under (?i) is not modelled");
        }
    }

    members = negated ? universe().minus(cased(members)) : cased(members);

    if (members.empty())
    {
        fail("the class matches nothing");
    }

    check_utf8(members);

    if (const auto one{members.single()})
    {
        return {.kind =
                        Bytes{.bytes = flags_.unicode ? encoded(*one) : std::string(1, static_cast<char>(*one)),
                              .bounded = false}};
    }

    return {.kind = Class{.set = std::move(members), .unicode = flags_.unicode}};
}

Scalar_set Pattern_reader::cased(const Scalar_set& members) const
{
    return flags_.insensitive ? folded(members, flags_.unicode) : members;
}

Scalar_set Pattern_reader::universe() const
{
    return universe_of(flags_.unicode);
}

void Pattern_reader::check_utf8(const Scalar_set& members) const
{
    if (!flags_.unicode && flags_.utf8 && !members.empty() && members.ranges().back().second > 0x7F)
    {
        fail("a byte beyond ASCII outside Unicode mode can match invalid UTF-8, which the regex crate refuses in a "
             "string pattern, logos 0.15.1 having no option to turn that check off");
    }
}

Unit Pattern_reader::next_unit()
{
    const auto lead{static_cast<unsigned char>(next("a character"))};

    // A byte string's bytes reach the crate as `\xHH`, bytes outside Unicode mode and scalars inside it.
    if (literal_.byte_string || lead < 0x80)
    {
        return {.value = lead, .byte = !flags_.unicode};
    }

    const auto length{lead >= 0xF0 ? 4UZ : lead >= 0xE0 ? 3UZ : 2UZ};

    char32_t value{lead & (0xFFU >> (length + 1))};

    for (std::size_t index{1}; index < length; ++index)
    {
        value = (value << 6U) | (static_cast<unsigned char>(next("a UTF-8 continuation byte")) & 0x3FU);
    }

    return {.value = value, .byte = false};
}

std::optional<char> Pattern_reader::peek() const noexcept
{
    return at_ < literal_.bytes.size() ? std::optional{literal_.bytes[at_]} : std::nullopt;
}

bool Pattern_reader::at(const std::string_view prefix) const noexcept
{
    return std::string_view{literal_.bytes}.substr(at_).starts_with(prefix);
}

bool Pattern_reader::accept(const char byte) noexcept
{
    if (peek() != byte)
    {
        return false;
    }

    ++at_;

    return true;
}

void Pattern_reader::expect(const char byte, const std::string_view what)
{
    if (!accept(byte))
    {
        fail("expected " + std::string{what});
    }
}

char Pattern_reader::next(const std::string_view what)
{
    if (at_ >= literal_.bytes.size())
    {
        fail("expected " + std::string{what} + " before the end of the pattern");
    }

    return literal_.bytes[at_++];
}

void Pattern_reader::fail(const std::string& message) const
{
    throw Spec_error{"the pattern " + literal_.written + " is refused: " + message, line_};
}

} // namespace

std::vector<Lexer_spec> read_logos(const std::string_view source)
{
    Rust_cursor cursor{source, 0, source.size()};

    std::vector<Lexer_spec> lexers;

    // A callback may name a function defined anywhere in the file, after the enum as well as before it, and a name the
    // file binds anywhere.
    const auto items{collect_items(source)};

    // The outer attributes read since the last item keyword, which the item they belong to consumes.
    std::vector<Attribute> pending;

    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        if (cursor.at("#["))
        {
            pending.push_back(read_attribute(cursor));

            continue;
        }

        if (cursor.at("#!["))
        {
            cursor.expect('#', "'#'");
            cursor.expect('!', "'!'");

            cursor.skip_group();

            continue;
        }

        // A raw or byte string's prefix would read as a word otherwise.
        if (cursor.at_string())
        {
            cursor.skip_token();

            pending.clear();

            continue;
        }

        const auto word{cursor.word()};

        if (word == "pub")
        {
            cursor.skip_trivia();

            if (cursor.peek() == '(')
            {
                cursor.skip_group();
            }

            continue;
        }

        if (word == "enum")
        {
            if (const auto line{derives_logos(pending, cursor)})
            {
                lexers.push_back(read_enum(cursor, pending, *line, items));
            }
        }
        else if (word.empty() && (cursor.peek() == '(' || cursor.peek() == '[' || cursor.peek() == '{'))
        {
            // Descended into rather than skipped, so that an enum inside a module or a function is found too.
            std::ignore = cursor.next("a delimiter");
        }
        else if (word.empty())
        {
            cursor.skip_token();
        }

        pending.clear();
    }

    return lexers;
}

} // namespace munch::tools::audit
