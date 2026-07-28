#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_SET_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_SET_HPP

#include <initializer_list>
#include <unordered_set>

namespace munch::regex
{
/**
 * @brief Represents a set of characters for use in regex character classes.
 *
 * Provides methods for constructing, combining, and querying sets of characters.
 */
class Set
{
public:
    /**
     * @brief Symbol type used inside a character set.
     */
    using Symbol_t = char;

    /**
     * @brief Underlying container for character symbols in a set.
     */
    using Symbols_t = std::unordered_set<Symbol_t>;

    /**
     * @brief Default-constructs an empty set.
     */
    Set() = default;

    /**
     * @brief Constructs a set from an initializer list of symbols.
     * @param symbols The symbols to include in the set.
     */
    Set(std::initializer_list<Symbol_t> symbols);

    /**
     * @brief Constructs a set from a set of symbols (copy).
     * @param symbols The symbols to include in the set.
     */
    explicit Set(const Symbols_t& symbols);

    /**
     * @brief Constructs a set from a set of symbols (move).
     * @param symbols The symbols to include in the set.
     */
    explicit Set(Symbols_t&& symbols);

    /**
     * @brief Returns the symbols in the set.
     * @return Reference to the set of symbols.
     */
    const Symbols_t& symbols() const noexcept;

    /**
     * @brief Creates a set containing a single symbol.
     * @param s The symbol to include.
     * @return The created set.
     */
    static Set from(Symbol_t s);

    /**
     * @brief Creates a set from an initializer list of symbols.
     * @param symbols The symbols to include.
     * @return The created set.
     */
    static Set from(std::initializer_list<Symbol_t> symbols);

    /**
     * @brief Creates a set containing all symbols in the range [start, end].
     * @param start The starting symbol.
     * @param end The ending symbol.
     * @return The created set.
     */
    static Set range(Symbol_t start, Symbol_t end);

    /**
     * @brief Creates a set of digit characters ('0'-'9').
     * @return The created set.
     */
    static Set digits();

    /**
     * @brief Creates a set of alphabetic characters (A-Z, a-z).
     * @return The created set.
     */
    static Set alpha();

    /**
     * @brief Creates a set of alphanumeric characters (A-Z, a-z, 0-9).
     * @return The created set.
     */
    static Set alphanum();

    /**
     * @brief Creates a set of printable characters.
     * @return The created set.
     */
    static Set printable();

    /**
     * @brief Creates a set of escape characters (e.g. '\n', '\t', etc.).
     * @return The created set.
     */
    static Set escape();

    /**
     * @brief Creates a set of newline characters (e.g. '\n', '\r').
     * @return The created set.
     */
    static Set newline();

    /**
     * @brief Creates a set of standard whitespace characters (e.g. ' ', '\t').
     * @return The created set.
     */
    static Set whitespace();

    /**
     * @brief Creates a set containing all possible characters.
     * @return The created set.
     */
    static Set all();

    /**
     * @brief Adds all symbols from another set to this set.
     * @param other The set to add.
     * @return Reference to this set.
     */
    Set& operator+=(const Set& other);

    /**
     * @brief Adds a symbol to this set.
     * @param s The symbol to add.
     * @return Reference to this set.
     */
    Set& operator+=(Symbol_t s);

    /**
     * @brief Removes all symbols from another set from this set.
     * @param other The set to remove.
     * @return Reference to this set.
     */
    Set& operator-=(const Set& other);

    /**
     * @brief Removes a symbol from this set.
     * @param s The symbol to remove.
     * @return Reference to this set.
     */
    Set& operator-=(Symbol_t s);

    /**
     * @brief Returns the union of two sets.
     * @param lhs The left-hand set.
     * @param rhs The right-hand set.
     * @return The union of the two sets.
     */
    friend Set operator+(Set lhs, const Set& rhs);

    /**
     * @brief Returns the union of a set and a symbol.
     * @param lhs The set.
     * @param s The symbol.
     * @return The union of the set and the symbol.
     */
    friend Set operator+(Set lhs, Symbol_t s);

    /**
     * @brief Returns the difference of two sets.
     * @param lhs The left-hand set.
     * @param rhs The right-hand set.
     * @return The difference of the two sets.
     */
    friend Set operator-(Set lhs, const Set& rhs);

    /**
     * @brief Returns the difference of a set and a symbol.
     * @param lhs The set.
     * @param s The symbol.
     * @return The difference of the set and the symbol.
     */
    friend Set operator-(Set lhs, Symbol_t s);

    /**
     * @brief Returns the union of a symbol and a set.
     * @param s The symbol.
     * @param rhs The set.
     * @return The union of the symbol and the set.
     */
    friend Set operator+(Symbol_t s, Set rhs);

private:
    Symbols_t symbols_;
};

} // namespace munch::regex

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_SET_HPP
