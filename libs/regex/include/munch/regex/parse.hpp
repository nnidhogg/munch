#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_PARSE_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_PARSE_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

#include "munch/regex/regex.hpp"

namespace munch::regex
{
/**
 * @brief The named patterns a pattern may refer to as {name}, each a pattern in the same syntax.
 */
using Definitions_t = std::map<std::string, std::string, std::less<>>;

/**
 * @brief A pattern the parser refuses, with the offset it was refused at.
 *
 * Every refusal names what was found and what the syntax admits there, and the offset indexes the pattern as given;
 * for a fault inside a definition the offset is that of the {name} in the pattern that expanded it, and the message
 * names the definition.
 */
class Syntax_error : public std::invalid_argument
{
public:
    /**
     * @brief Constructs the error from its message and the offset it points at.
     * @param message What was refused and why.
     * @param offset The byte offset into the pattern.
     */
    Syntax_error(const std::string& message, std::size_t offset);

    /**
     * @brief The byte offset into the pattern the refusal points at.
     * @return The offset.
     */
    [[nodiscard]] std::size_t offset() const noexcept;

private:
    /**
     * @brief The byte offset into the pattern the refusal points at.
     */
    std::size_t offset_;
};

/**
 * @brief Reads a pattern written in the syntax lexer generators take, flex's dialect of POSIX extended regular
 *        expressions, into the same nodes the combinators build.
 *
 * The syntax is the one a flex or re2c rule body is written in, read over bytes as those tools read it: alternation
 * with `|`, grouping with parentheses, the postfix operators `*`, `+`, `?` and the counted `{n}`, `{n,}` and `{n,m}`;
 * a dot for any byte but the newline; bracket expressions with ranges, negation, the POSIX classes such as
 * `[:alpha:]`, and a leading `]` or an edge `-` taken literally; escapes `\n`, `\t`, `\r`, `\f`, `\v`, `\a`, `\b`,
 * octal `\ooo` and hex `\xhh`, and any other escaped byte standing for itself; a double-quoted literal, its escapes
 * decoded; and `{name}` expanding to a definition, itself parsed in the same syntax, definitions nesting but never
 * cycling. Bytes outside ASCII are literals, so a UTF-8 sequence in the pattern is the run of its bytes, and a
 * bracket lists bytes rather than characters, exactly as flex does.
 *
 * What the syntax has and a token language cannot say is refused rather than approximated: the anchors, a `^`
 * opening the pattern and a `$` closing it, flex's trailing context `/`, its start-condition prefix `<s>` and
 * `<<EOF>>` are conditions on the context a match stands in, not on the match, so a pattern carrying one raises
 * rather than matching something else; a `^` or `$` anywhere else is the byte, as flex reads it. Empty alternatives
 * and groups are refused as the standard refuses them.
 *
 * The result is exactly what the combinators would have built: runs of literal bytes become one text node, brackets
 * and the dot become any_of over a set, and the operators become the repeat nodes, so a parsed pattern and a
 * hand-built one compile to the same automaton.
 * @param pattern The pattern.
 * @param definitions The named patterns `{name}` may expand to; a name not among them is refused.
 * @return The regex.
 * @throws Syntax_error If the pattern is refused, with the offset and the reason.
 */
[[nodiscard]] Regex parse(std::string_view pattern, const Definitions_t& definitions = {});

} // namespace munch::regex

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_PARSE_HPP
