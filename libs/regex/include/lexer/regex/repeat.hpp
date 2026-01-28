#ifndef LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_REPEAT_HPP
#define LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_REPEAT_HPP

#include <memory>
#include <variant>

#include "lexer/regex/regex.hpp"

namespace lexer::regex
{
/**
 * @brief Regex node that represents repetition constructs (Kleene star, plus, optional, exact, at least, range).
 *
 * Use the static methods or the free helper functions to construct.
 */
class Repeat final : public Regex
{
    // Represents the Kleene star ('*') repetition, i.e. zero or more occurrences of the pattern.
    struct Kleene
    {
    };

    // Represents the Kleene plus ('+') repetition, i.e. one or more occurrences of the pattern.
    struct Plus
    {
    };

    // Represents an optional pattern ('?'), i.e. zero or one occurrence.
    struct Optional
    {
    };

    // Represents an exact repetition, i.e. exactly `count` occurrences.
    struct Exact
    {
        std::size_t count;
    };

    // Represents a lower-bound repetition, i.e. at least `min` occurrences.
    struct At_least
    {
        std::size_t min;
    };

    // Represents a bounded repetition, i.e. between `min` and `max` occurrences inclusive.
    struct Range
    {
        std::size_t min;
        std::size_t max;
    };

    using Variant_t = std::variant<Kleene, Plus, Optional, Exact, At_least, Range>;

public:
    /**
     * @brief Creates a Kleene star (zero or more) repetition node.
     * @param regex The regex to repeat.
     * @return Shared pointer to the created Repeat node.
     */
    static std::shared_ptr<Repeat> kleene(std::shared_ptr<const Regex> regex);

    /**
     * @brief Creates a Kleene plus (one or more) repetition node.
     * @param regex The regex to repeat.
     * @return Shared pointer to the created Repeat node.
     */
    static std::shared_ptr<Repeat> plus(std::shared_ptr<const Regex> regex);

    /**
     * @brief Creates an optional (zero or one) repetition node.
     * @param regex The regex to make optional.
     * @return Shared pointer to the created Repeat node.
     */
    static std::shared_ptr<Repeat> optional(std::shared_ptr<const Regex> regex);

    /**
     * @brief Creates an exact repetition node.
     * @param regex The regex to repeat.
     * @param count The exact number of repetitions.
     * @return Shared pointer to the created Repeat node.
     */
    static std::shared_ptr<Repeat> exact(std::shared_ptr<const Regex> regex, std::size_t count);

    /**
     * @brief Creates a lower-bound repetition node.
     * @param regex The regex to repeat.
     * @param min The minimum number of repetitions.
     * @return Shared pointer to the created Repeat node.
     */
    static std::shared_ptr<Repeat> at_least(std::shared_ptr<const Regex> regex, std::size_t min);

    /**
     * @brief Creates a bounded repetition node.
     * @param regex The regex to repeat.
     * @param min The minimum number of repetitions.
     * @param max The maximum number of repetitions.
     * @return Shared pointer to the created Repeat node.
     */
    static std::shared_ptr<Repeat> range(std::shared_ptr<const Regex> regex, std::size_t min, std::size_t max);

    Repeat(const Repeat&) = delete;
    Repeat& operator=(const Repeat&) = delete;

    /**
     * @brief Converts this regex node to an NFA builder.
     * @return NFA builder representing this regex.
     */
    [[nodiscard]] nfa::Builder to_nfa() const override;

private:
    Repeat(const Variant_t& variant, std::shared_ptr<const Regex> regex);

    [[nodiscard]] nfa::Builder to_kleene() const;

    [[nodiscard]] nfa::Builder to_plus() const;

    [[nodiscard]] nfa::Builder to_optional() const;

    [[nodiscard]] nfa::Builder to_exact(std::size_t count) const;

    [[nodiscard]] nfa::Builder to_at_least(std::size_t min) const;

    [[nodiscard]] nfa::Builder to_range(std::size_t min, std::size_t max) const;

    Variant_t variant_;

    std::shared_ptr<const Regex> regex_;
};

/**
 * @brief Helper function to create a Kleene star (zero or more) repetition node.
 * @param regex The regex to repeat.
 * @return Shared pointer to the created Regex node.
 */
std::shared_ptr<const Regex> kleene(std::shared_ptr<const Regex> regex);

/**
 * @brief Helper function to create a Kleene plus (one or more) repetition node.
 * @param regex The regex to repeat.
 * @return Shared pointer to the created Regex node.
 */
std::shared_ptr<const Regex> plus(std::shared_ptr<const Regex> regex);

/**
 * @brief Helper function to create an optional (zero or one) repetition node.
 * @param regex The regex to make optional.
 * @return Shared pointer to the created Regex node.
 */
std::shared_ptr<const Regex> optional(std::shared_ptr<const Regex> regex);

/**
 * @brief Helper function to create an exact repetition node.
 * @param regex The regex to repeat.
 * @param count The exact number of repetitions.
 * @return Shared pointer to the created Regex node.
 */
std::shared_ptr<const Regex> exact(std::shared_ptr<const Regex> regex, std::size_t count);

/**
 * @brief Helper function to create a lower-bound repetition node.
 * @param regex The regex to repeat.
 * @param min The minimum number of repetitions.
 * @return Shared pointer to the created Regex node.
 */
std::shared_ptr<const Regex> at_least(std::shared_ptr<const Regex> regex, std::size_t min);

/**
 * @brief Helper function to create a bounded repetition node.
 * @param regex The regex to repeat.
 * @param min The minimum number of repetitions.
 * @param max The maximum number of repetitions.
 * @return Shared pointer to the created Regex node.
 */
std::shared_ptr<const Regex> range(std::shared_ptr<const Regex> regex, std::size_t min, std::size_t max);

} // namespace lexer::regex

#endif // LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_REPEAT_HPP
