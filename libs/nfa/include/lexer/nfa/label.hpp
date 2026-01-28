#ifndef LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_LABEL_HPP
#define LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_LABEL_HPP

#include <cstddef>
#include <functional>
#include <variant>

namespace lexer::nfa
{
/**
 * @brief Represents an epsilon (empty string) transition label for NFA transitions.
 */
class Epsilon
{
public:
    /**
     * @brief Equality comparison operator for epsilon labels.
     * @return True if both are epsilon labels.
     */
    bool operator==(const Epsilon&) const noexcept;

    /**
     * @brief Hash functor for Epsilon, suitable for use in unordered containers.
     */
    struct Hash
    {
        std::size_t operator()(const Epsilon&) const noexcept;
    };
};

/**
 * @brief Represents a transition label for NFA transitions (symbol or epsilon).
 */
class Label
{
public:
    /**
     * @brief Symbol type used in NFA transitions.
     */
    using Symbol_t = char;

    /**
     * @brief Label variant type.
     *
     * Stores either an `Epsilon` (ε-transition) or a concrete `Symbol_t` character.
     */
    using Variant_t = std::variant<Symbol_t, Epsilon>;

    /**
     * @brief Constructs a label with the given symbol.
     * @param s The symbol for the label.
     */
    explicit Label(Symbol_t s) noexcept;

    /**
     * @brief Returns a label representing an epsilon transition.
     * @return An epsilon label.
     */
    [[nodiscard]] static Label epsilon() noexcept;

    /**
     * @brief Checks if the label is a symbol.
     * @return True if the label is a symbol, false if epsilon.
     */
    [[nodiscard]] bool is_symbol() const noexcept;

    /**
     * @brief Checks if the label is an epsilon transition.
     * @return True if the label is epsilon, false otherwise.
     */
    [[nodiscard]] bool is_epsilon() const noexcept;

    /**
     * @brief Returns the symbol associated with this label.
     * @return The symbol character.
     * @throws std::bad_variant_access if not a symbol.
     */
    [[nodiscard]] Symbol_t symbol() const;

    /**
     * @brief Returns the underlying variant (symbol or epsilon).
     * @return Reference to the variant.
     */
    [[nodiscard]] const Variant_t& variant() const noexcept;

    /**
     * @brief Equality comparison operator for labels.
     * @param other The label to compare with.
     * @return True if the variants are equal, false otherwise.
     */
    bool operator==(const Label& other) const noexcept;

    /**
     * @brief Hash functor for Label, suitable for use in unordered containers.
     */
    struct Hash
    {
        std::size_t operator()(const Label& label) const noexcept;
    };

private:
    explicit Label(Epsilon e) noexcept;

    Variant_t variant_;
};

} // namespace lexer::nfa

#endif // LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_LABEL_HPP
