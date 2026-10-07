#ifndef MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_LABEL_HPP
#define MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_LABEL_HPP

#include <cstddef>
#include <variant>

namespace munch::nfa
{
/**
 * @brief Represents an epsilon (empty string) transition label for NFA transitions.
 */
class Epsilon
{
public:
    /**
     * @brief Hash functor for Epsilon, suitable for use in unordered containers.
     */
    struct Hash
    {
        /**
         * @brief Hashes an epsilon label; every one hashes alike.
         * @return Zero.
         */
        std::size_t operator()(const Epsilon&) const noexcept;
    };

    /**
     * @brief Equal always, every epsilon label being the same.
     */
    bool operator==(const Epsilon&) const noexcept = default;
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
     * @brief Hash functor for Label, suitable for use in unordered containers.
     */
    struct Hash
    {
        /**
         * @brief Hashes a label by its symbol or as epsilon.
         * @param label The label.
         * @return The hash of the label's alternative.
         */
        std::size_t operator()(const Label& label) const noexcept;
    };

    /**
     * @brief Constructs a label with the given symbol.
     * @param symbol The symbol for the label.
     */
    explicit Label(Symbol_t symbol) noexcept;

    /**
     * @brief Equal when both hold the same symbol or both are epsilon.
     */
    bool operator==(const Label&) const noexcept = default;

    /**
     * @brief Returns a label representing an epsilon transition.
     * @return An epsilon label.
     */
    [[nodiscard]] static Label epsilon() noexcept;

    /**
     * @brief Returns whether the label is a symbol.
     * @return True if the label is a symbol, false if epsilon.
     */
    [[nodiscard]] bool is_symbol() const noexcept;

    /**
     * @brief Returns whether the label is an epsilon transition.
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

private:
    /**
     * @brief Constructs an epsilon label.
     * @param epsilon The epsilon marker.
     */
    explicit Label(Epsilon epsilon) noexcept;

    /**
     * @brief The underlying symbol-or-epsilon value.
     */
    Variant_t variant_;
};

} // namespace munch::nfa

#endif // MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_LABEL_HPP
