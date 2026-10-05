#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_CLASSES_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_CLASSES_HPP

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "munch/regex/parse.hpp"
#include "munch/regex/utf8.hpp"
#include "munch/tools/audit/read_re2c.hpp"

/**
 * @brief What re2c calls a char set, as the code points it admits under the block's encoding, Class, and how one is
 *        written for the pattern parser: the code points of a bracket, code_points(), the union and the difference
 *        re2c's class difference takes, united() and subtracted(), and the rendering under an encoding, rendered() and
 *        step(), with the classes among a block's definitions kept by name, Classes_t and note_class(), and the class
 *        of every byte, any_byte.
 *
 * A class is held as code points rather than bytes, so that a difference is taken before any encoding, as re2c takes
 * it: under UTF-8 `[^] \ [\x00-\x7f]` is every code point beyond ASCII, which is no set of bytes. Only once it is
 * written for the parser is a class turned into the bytes an encoding spells it in.
 */
namespace munch::tools::audit
{
/**
 * @brief The code points a class admits, as ascending, disjoint ranges: each a byte's value under the byte encodings
 *        and any scalar under UTF-8. What re2c calls a char set, which is all its class difference takes.
 */
using Class = std::vector<regex::utf8::Code_point_range>;

/**
 * @brief The classes among the definitions in force, by name: a definition whose regex is one class, which a class
 *        difference naming it takes as an operand; a definition that is no class has no entry.
 */
using Classes_t = std::map<std::string, Class, std::less<>>;

/**
 * @brief What the parser reads every re2c pattern under: a bracket range written backwards spans its members as re2c
 *        reads it, `[z-a]` being `[a-z]`; the case flags re2c has are spelled into the patterns instead.
 */
constexpr regex::Parse_options re2c_parse{.caseless = false, .ranges_either_way = true};

/**
 * @brief The class of every byte, which `[^]` and the default rule `*` stand for where they match one byte.
 */
constexpr std::string_view any_byte{R"([\x00-\xff])"};

/**
 * @brief Returns the code points there are under an encoding: the byte values under the byte encodings, every scalar
 *        under UTF-8, which is what a negated class admits the rest of.
 * @param encoding The encoding.
 * @return One past the last code point.
 */
[[nodiscard]] constexpr char32_t code_space(const Re2c_encoding encoding) noexcept
{
    return encoding == Re2c_encoding::utf8 ? 0x110000 : 0x100;
}

/**
 * @brief Returns the code points a class admits, when the class is one the byte parser can read.
 *
 * Every class member the reader accepts is a byte value, the Unicode escapes being refused, so the byte parser's own
 * reading of the class is its members; under an encoding whose code space is wider than a byte those values are code
 * points, and a negated class admits every other code point of the space rather than every other byte.
 * @param bracket The class as written, its brackets included.
 * @param definitions The definitions, which the parser needs for nothing here but is given for uniformity.
 * @param space One past the last code point there is, which a negated class runs to.
 * @return The class, or std::nullopt when the parser reads the bracket as something else.
 */
[[nodiscard]] std::optional<Class> code_points(
        std::string_view bracket, const regex::Definitions_t& definitions, char32_t space);

/**
 * @brief Returns the union of two classes.
 * @param left The left class.
 * @param right The right class.
 * @return The class holding the code points of either.
 */
[[nodiscard]] Class united(const Class& left, const Class& right);

/**
 * @brief Returns the difference of two classes, re2c's `left \ right`.
 * @param left The left class.
 * @param right The right class.
 * @return The class holding the code points of the left that the right has not got.
 */
[[nodiscard]] Class subtracted(const Class& left, const Class& right);

/**
 * @brief Returns a class as the pattern parser's syntax under an encoding: under UTF-8 one step of its code points
 *        where it reaches past ASCII, otherwise the bracket of its bytes, which every code point of it then is; `[]`
 *        for an empty class, which no settled pass compiles.
 * @param points The class.
 * @param encoding The encoding.
 * @return The expression, one atom.
 */
[[nodiscard]] std::string rendered(const Class& points, Re2c_encoding encoding);

/**
 * @brief Returns whether every code point of a class is ASCII, whose encoding under UTF-8 is the byte itself.
 * @param points The class.
 * @return True when it is.
 */
[[nodiscard]] bool is_ascii(const Class& points);

/**
 * @brief Returns a set of code points as one step of the pattern parser's syntax under the UTF-8 encoding.
 *
 * The ranges become a bracket of `\u{...}` members, which the parser matches as their UTF-8 encodings. The surrogates
 * are the one part it cannot take: no encoding has them, so the parser refuses them, while re2c under its default
 * encoding policy encodes them like any other code point; a set holding them, which is every set written as a negation,
 * carries their three-byte spelling beside the bracket instead.
 * @param ranges The ranges, ascending and disjoint, at least one and not the surrogates alone.
 * @return The expression, one atom, grouped where it is more than the bracket.
 */
[[nodiscard]] std::string step(const std::vector<regex::utf8::Code_point_range>& ranges);

/**
 * @brief Notes what a definition is to a class difference: its class where its regex is one class, and nothing where it
 *        is not, a name defined again dropping the class it had.
 * @param classes The classes among the definitions in force.
 * @param name The definition's name.
 * @param points Its class, when its regex is one.
 */
void note_class(Classes_t& classes, const std::string& name, std::optional<Class> points);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_CLASSES_HPP
