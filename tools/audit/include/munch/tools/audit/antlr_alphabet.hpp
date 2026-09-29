#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_ALPHABET_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_ALPHABET_HPP

#include <bitset>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief The characters an ANTLR set or literal admits, Alphabet, and what a match can begin with, Beginning: a range
 *        folded as ANTLR folds one under `caseInsensitive`, admit() and spanning(), a set's complement over the
 *        scalars, complement(), and how either is written over UTF-8 bytes for the pattern parser, step(), bracket()
 *        and caseless(), with the one scalar a literal's bytes encode, decoded().
 *
 * ANTLR reads characters, UTF-16 units in the Java strings it holds the grammar in, and the audit reads bytes, so an
 * alphabet keeps the ASCII part bit by bit and the scalars beyond it as ranges, and is written out as the UTF-8
 * encodings of what it admits.
 */
namespace munch::tools::audit
{
/**
 * @brief Why a character beyond ASCII is refused where `caseInsensitive` is in force.
 *
 * ANTLR's option rewrites every character to both of its cases, one code point each, `[äéöüß]` matching `äéöüßÄÉÖÜß`
 * (antlr4/doc/options.md), and the library carries the Unicode property tables but no case mappings, so folding such a
 * character is not something this reading can do; folding its ASCII neighbours alone would analyse another language
 * than the lexer's.
 */
constexpr std::string_view unfoldable{
        "a character beyond ASCII under caseInsensitive needs the Unicode case mappings, which the byte reading has "
        "not got"};

/**
 * @brief The ASCII bytes, one bit each.
 */
using Ascii_t = std::bitset<128>;

/**
 * @brief One inclusive range of scalars.
 */
struct Scalar_range
{
    /**
     * @brief The first scalar.
     */
    char32_t first;

    /**
     * @brief The last scalar.
     */
    char32_t last;
};

/**
 * @brief What one atom of a rule admits in one step, when that is a set of characters: the ASCII part, and the scalars
 *        beyond it. What a non-greedy loop needs to know about the atom it repeats.
 */
struct Alphabet
{
    /**
     * @brief The ASCII bytes admitted.
     */
    Ascii_t ascii;

    /**
     * @brief The scalars from U+0080 up admitted, as ranges in any order.
     */
    std::vector<Scalar_range> beyond;
};

/**
 * @brief The characters a match of something can begin with, when they are known: the ASCII bytes one by one, and
 *        whether any scalar beyond ASCII is among them, which ones not being kept.
 */
struct Beginning
{
    /**
     * @brief The ASCII bytes a match can begin with.
     */
    Ascii_t ascii;

    /**
     * @brief Whether a match can begin with a scalar beyond ASCII.
     */
    bool beyond;

    /**
     * @brief Adds the characters another can begin with.
     * @param other The other.
     */
    void join(const Beginning& other) noexcept;

    /**
     * @brief Whether one character may begin both this and another, two scalars beyond ASCII taken to be one.
     * @param other The other.
     * @return True when one may.
     */
    [[nodiscard]] bool overlaps(const Beginning& other) const noexcept;
};

/**
 * @brief Whether bytes hold one beyond ASCII.
 * @param bytes The bytes.
 * @return True when one is.
 */
[[nodiscard]] bool holds_beyond_ascii(std::string_view bytes);

/**
 * @brief The one scalar a literal's UTF-8 bytes encode, when they encode exactly one.
 * @param bytes The bytes.
 * @return The scalar, or std::nullopt for none or several.
 */
[[nodiscard]] std::optional<char32_t> decoded(std::string_view bytes);

/**
 * @brief An alphabet as one step of the parser's syntax: a bracket over bytes when it stays within ASCII, else a
 *        bracket over scalars, every member a code point escape, which is how the parser reads one.
 * @param alphabet The alphabet.
 * @return The bracket.
 */
[[nodiscard]] std::string step(const Alphabet& alphabet);

/**
 * @brief A set of ASCII bytes as a bracket, runs of three or more as ranges.
 * @param ascii The bytes.
 * @return The bracket text.
 */
[[nodiscard]] std::string bracket(const Ascii_t& ascii);

/**
 * @brief Every scalar an alphabet does not admit.
 * @param alphabet The alphabet.
 * @return Its complement over the scalars.
 */
[[nodiscard]] Alphabet complement(const Alphabet& alphabet);

/**
 * @brief The bytes of a literal with every ASCII letter in both cases, one bracket per byte.
 * @param bytes The bytes.
 * @return The expression.
 */
[[nodiscard]] std::string caseless(std::string_view bytes);

/**
 * @brief The characters of a range, `'a'..'z'`, as an alphabet.
 * @param low The first character.
 * @param high The last, no lower than the first.
 * @param case_insensitive Whether the range folds as ANTLR folds one under `caseInsensitive`.
 * @return The alphabet.
 */
[[nodiscard]] Alphabet spanning(char32_t low, char32_t high, bool case_insensitive);

/**
 * @brief Adds a range of characters, a set's member or span or a `'a'..'z'`, to an alphabet, folded as ANTLR folds a
 *        range under `caseInsensitive`.
 *
 * ANTLR folds each range by its two ends alone (LexerATNFactory.checkRangeAndAddToSet over RangeBorderCharactersData):
 * where neither end changes case or the ends differ in case, one being a letter of the other case or no letter, or the
 * copies of the ends in lower and in upper case are not one width apart, the range stands as written; otherwise the
 * copy in lower case and the copy in upper case are both added. So `[a-z]` and `'q'` gain `A-Z` and `Q`, while `[A-t]`,
 * `[0-Z]` and `[a-]` admit exactly what they spell, the letters inside them folded no further, which ANTLR's warning
 * 185 remarks on for the first two; the set is not closed under case. Beyond ASCII, where ANTLR's case mappings are
 * Unicode's, the range is added as written, the caller refusing such a range under the option.
 * @param alphabet The alphabet, widened on return.
 * @param first The first character.
 * @param last The last, no lower than the first.
 * @param case_insensitive Whether the option is in force.
 */
void admit(Alphabet& alphabet, char32_t first, char32_t last, bool case_insensitive);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_ALPHABET_HPP
