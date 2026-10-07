#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_LABEL_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_LABEL_HPP

#include <cstddef>

namespace munch::dfa
{
/**
 * @brief Represents a transition label (symbol) for DFA transitions.
 */
class Label
{
public:
    /**
     * @brief Symbol type for DFA labels.
     *
     * Each label represents a single input character used on transitions.
     */
    using Symbol_t = char;

    /**
     * @brief Hash functor for Label, suitable for use in unordered containers.
     */
    struct Hash
    {
        /**
         * @brief Computes the hash of a label.
         * @param label The label to hash.
         * @return The hash value.
         */
        std::size_t operator()(const Label& label) const noexcept;
    };

    /**
     * @brief Constructs a label with the given symbol.
     * @param symbol The symbol for the label.
     */
    explicit Label(Symbol_t symbol) noexcept;

    /**
     * @brief Equal when both symbols are equal.
     */
    bool operator==(const Label&) const noexcept = default;

    /**
     * @brief Returns the symbol associated with this label.
     * @return The symbol character.
     */
    [[nodiscard]] Symbol_t symbol() const noexcept;

private:
    /**
     * @brief The symbol the label carries.
     */
    Symbol_t symbol_;
};

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_LABEL_HPP
