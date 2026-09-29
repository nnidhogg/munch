#include "munch/tools/audit/word_kinds.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief A set of the kinds of word a pattern matches for a byte, held as bits, one per kind.
 */
class Word_kinds
{
public:
    /**
     * @brief One kind of word, told apart by where the byte stands fixed in it; each is one bit of the set.
     */
    enum class Word : std::uint8_t
    {
        /**
         * @brief The empty word.
         */
        empty = 1U,

        /**
         * @brief A nonempty word with no fixed occurrence of the byte.
         */
        plain = 2U,

        /**
         * @brief A word whose one fixed occurrence of the byte is its first byte.
         */
        leading = 4U,

        /**
         * @brief A word with a fixed occurrence of the byte past its first byte.
         */
        mid = 8U
    };

    /**
     * @brief Constructs the empty set.
     */
    Word_kinds() noexcept = default;

    /**
     * @brief Constructs the set of one kind.
     * @param word The kind.
     */
    explicit Word_kinds(Word word) noexcept;

    /**
     * @brief Equal when the two sets hold the same kinds.
     * @param other The other set.
     * @return True when they do.
     */
    [[nodiscard]] bool operator==(const Word_kinds& other) const noexcept = default;

    /**
     * @brief Adds the other set's kinds to this one.
     * @param other The other set.
     * @return This set.
     */
    Word_kinds& operator|=(Word_kinds other) noexcept;

    /**
     * @brief Returns the kinds of word a pattern matches, for a byte.
     * @param regex The pattern.
     * @param byte The byte.
     * @return The kinds, at least one.
     */
    [[nodiscard]] static Word_kinds of(const regex::Regex& regex, unsigned char byte);

    /**
     * @brief Returns the kinds of word every word of this set makes followed by every word of another.
     * @param second The kinds of the second words.
     * @return The kinds of their concatenations.
     */
    [[nodiscard]] Word_kinds followed_by(Word_kinds second) const noexcept;

    /**
     * @brief Returns the kinds of word a repetition of words of this set makes, over the counts allowed.
     *
     * The kinds of n repetitions follow from the kinds of n - 1, and there are sixteen sets of kinds, so the sets from
     * the minimum count on repeat within sixteen steps; the union is complete at the first repeat, which is also where
     * an unbounded repetition stops.
     * @param min The least count.
     * @param max The greatest count, or none for an unbounded repetition.
     * @return The kinds of the repetitions' words.
     */
    [[nodiscard]] Word_kinds repeated(std::size_t min, std::optional<std::size_t> max) const noexcept;

private:
    /**
     * @brief Returns whether the set holds a kind.
     * @param word The kind.
     * @return True when it does.
     */
    [[nodiscard]] bool has(Word word) const noexcept;

    /**
     * @brief Returns the kind of the word one kind of word makes followed by another.
     * @param first The kind of the first word.
     * @param second The kind of the second.
     * @return The kind of their concatenation.
     */
    [[nodiscard]] static Word join(Word first, Word second) noexcept;

    /**
     * @brief The kinds, one bit each.
     */
    std::uint8_t bits_{0};
};

Word_kinds::Word_kinds(const Word word) noexcept : bits_{std::to_underlying(word)}
{}

Word_kinds& Word_kinds::operator|=(const Word_kinds other) noexcept
{
    bits_ |= other.bits_;

    return *this;
}

Word_kinds Word_kinds::of(const regex::Regex& regex, const unsigned char byte)
{
    /**
     * @brief Returns the kinds of word a pattern of one kind matches.
     * @tparam Node The node's type.
     * @param node The pattern.
     * @return The kinds.
     */
    const auto kinds_of{[byte]<typename Node>(const Node& node) -> Word_kinds {
        if constexpr (std::is_same_v<Node, regex::Any_of>)
        {
            // A class of the one byte matches nothing else, so its occurrence is as fixed as a text's.
            const auto only_byte{node.set.symbols() == regex::Set::Symbols_t{static_cast<char>(byte)}};

            return Word_kinds{only_byte ? Word::leading : Word::plain};
        }
        else if constexpr (std::is_same_v<Node, regex::Text>)
        {
            if (node.text.empty())
            {
                return Word_kinds{Word::empty};
            }

            const auto after_first{std::string_view{node.text}.substr(1)};

            if (after_first.contains(static_cast<char>(byte)))
            {
                return Word_kinds{Word::mid};
            }

            return Word_kinds{node.text.front() == static_cast<char>(byte) ? Word::leading : Word::plain};
        }
        else if constexpr (std::is_same_v<Node, regex::Concat>)
        {
            Word_kinds out{Word::empty};

            for (const auto& part : node.regexes)
            {
                const auto part_kinds{of(part, byte)};

                out = out.followed_by(part_kinds);
            }

            return out;
        }
        else if constexpr (std::is_same_v<Node, regex::Choice>)
        {
            Word_kinds out{};

            for (const auto& part : node.regexes)
            {
                out |= of(part, byte);
            }

            return out;
        }
        else
        {
            static_assert(std::is_same_v<Node, regex::Repeat>);

            const auto inner{of(*node.regex, byte)};

            /**
             * @brief Returns the kinds of word a repetition of one kind makes of the repeated words.
             * @tparam Kind The kind's type.
             * @param kind The repetition's kind.
             * @return The kinds.
             */
            const auto repeated_by{[inner]<typename Kind>(const Kind& kind) {
                if constexpr (std::is_same_v<Kind, regex::Kleene>)
                {
                    return inner.repeated(0, std::nullopt);
                }
                else if constexpr (std::is_same_v<Kind, regex::Plus>)
                {
                    return inner.repeated(1, std::nullopt);
                }
                else if constexpr (std::is_same_v<Kind, regex::Optional>)
                {
                    return inner.repeated(0, 1);
                }
                else if constexpr (std::is_same_v<Kind, regex::Exact>)
                {
                    return inner.repeated(kind.count, kind.count);
                }
                else if constexpr (std::is_same_v<Kind, regex::At_least>)
                {
                    return inner.repeated(kind.min, std::nullopt);
                }
                else
                {
                    static_assert(std::is_same_v<Kind, regex::Range>);

                    return inner.repeated(kind.min, kind.max);
                }
            }};

            return std::visit(repeated_by, node.kind);
        }
    }};

    return std::visit(kinds_of, regex.node);
}

Word_kinds Word_kinds::followed_by(const Word_kinds second) const noexcept
{
    static constexpr std::array words{Word::empty, Word::plain, Word::leading, Word::mid};

    Word_kinds out{};

    for (const auto first_word : words)
    {
        if (!has(first_word))
        {
            continue;
        }

        for (const auto second_word : words)
        {
            if (second.has(second_word))
            {
                out |= Word_kinds{join(first_word, second_word)};
            }
        }
    }

    return out;
}

bool Word_kinds::has(const Word word) const noexcept
{
    return (bits_ & std::to_underlying(word)) != 0;
}

Word_kinds::Word Word_kinds::join(const Word first, const Word second) noexcept
{
    if (first == Word::mid || second == Word::mid)
    {
        return Word::mid;
    }

    if (first == Word::empty)
    {
        return second;
    }

    if (second == Word::empty)
    {
        return first;
    }

    // A fixed first byte of the second word stands past the first byte of a nonempty first word.
    return second == Word::leading ? Word::mid : first;
}

Word_kinds Word_kinds::repeated(const std::size_t min, const std::optional<std::size_t> max) const noexcept
{
    Word_kinds out{};

    Word_kinds count{Word::empty};

    std::uint16_t seen{0};

    for (std::size_t n{0}; !max || n <= *max; ++n)
    {
        if (n >= min)
        {
            const auto bit{1U << count.bits_};

            if ((seen & bit) != 0)
            {
                break;
            }

            seen |= static_cast<std::uint16_t>(bit);

            out |= count;
        }

        count = count.followed_by(*this);
    }

    return out;
}

} // namespace

bool fixed_mid_token(const regex::Regex& regex, const unsigned char byte)
{
    return Word_kinds::of(regex, byte) == Word_kinds{Word_kinds::Word::mid};
}

} // namespace munch::tools::audit
