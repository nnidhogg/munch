#include "munch/tools/audit/logos_refusals.hpp"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <variant>

#include "munch/regex/utf8.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief Returns the first byte of a scalar's UTF-8, which is what a scanner over bytes decides on.
 * @param scalar The scalar.
 * @return The byte.
 */
[[nodiscard]] unsigned lead_byte(const char32_t scalar)
{
    const auto bytes{regex::utf8::encode(scalar)};

    return static_cast<unsigned char>(bytes.front());
}

/**
 * @brief Returns whether a node matches the empty string.
 *
 * A reference is not expected here, the caller having expanded every one, and answers yes, which widens a follow set
 * and refuses rather than reads too much.
 * @param node The node.
 * @return True when it does.
 */
[[nodiscard]] bool matches_empty(const Node& node)
{
    const auto empty_of{[]<typename Kind>(const Kind& kind) -> bool {
        if constexpr (std::is_same_v<Kind, Empty> || std::is_same_v<Kind, Reference>)
        {
            return true;
        }
        else if constexpr (std::is_same_v<Kind, Bytes>)
        {
            return kind.bytes.empty();
        }
        else if constexpr (std::is_same_v<Kind, Char_class>)
        {
            return false;
        }
        else if constexpr (std::is_same_v<Kind, Concat>)
        {
            return std::ranges::all_of(kind.parts, matches_empty);
        }
        else if constexpr (std::is_same_v<Kind, Alternation>)
        {
            return std::ranges::any_of(kind.branches, matches_empty);
        }
        else
        {
            static_assert(std::is_same_v<Kind, Repeat>);

            return kind.min == 0 || matches_empty(*kind.operand);
        }
    }};

    return std::visit(empty_of, node.kind);
}

/**
 * @brief Returns the bytes a match of a node can begin with.
 *
 * A lead byte is what the scanner decides on, so a class of scalars answers with the lead bytes of its ranges, which
 * run with the scalars. A reference is not expected here, the caller having expanded every one, and answers with every
 * byte so that a caller that forgot refuses rather than reads too much.
 * @param node The node.
 * @return The bytes.
 */
[[nodiscard]] Byte_set_t first_bytes(const Node& node)
{
    const auto first_of{[]<typename Kind>(const Kind& kind) -> Byte_set_t {
        Byte_set_t bytes{};

        if constexpr (std::is_same_v<Kind, Bytes>)
        {
            if (!kind.bytes.empty())
            {
                bytes.set(static_cast<unsigned char>(kind.bytes.front()));
            }
        }
        else if constexpr (std::is_same_v<Kind, Char_class>)
        {
            for (const auto& [low, high] : kind.set.ranges())
            {
                const auto first{kind.unicode ? lead_byte(low) : static_cast<unsigned>(low)};

                const auto last{kind.unicode ? lead_byte(high) : static_cast<unsigned>(high)};

                const auto end{std::min(last, unsigned{last_byte})};

                for (auto byte{first}; byte <= end; ++byte)
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
        else if constexpr (std::is_same_v<Kind, Alternation>)
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
    }};

    return std::visit(first_of, node.kind);
}

} // namespace

void check_repetitions(const Node& node, const Byte_set_t& follow, const std::size_t line)
{
    const auto check{[&]<typename Kind>(const Kind& kind) {
        if constexpr (std::is_same_v<Kind, Concat>)
        {
            auto rest{follow};

            for (const auto& part : kind.parts | std::views::reverse)
            {
                check_repetitions(part, rest, line);

                const auto empty{matches_empty(part)};

                const auto begins{first_bytes(part)};

                rest = empty ? (rest | begins) : begins;
            }
        }
        else if constexpr (std::is_same_v<Kind, Alternation>)
        {
            for (const auto& branch : kind.branches)
            {
                check_repetitions(branch, follow, line);
            }
        }
        else if constexpr (std::is_same_v<Kind, Repeat>)
        {
            const auto once{kind.max == 1};

            const auto begins{first_bytes(*kind.operand)};

            const auto inside{once ? follow : (follow | begins)};

            if (!kind.max && (begins & follow).any())
            {
                throw Spec_error{
                        "a repetition whose body can begin with a byte that may also follow it is one logos 0.15.1 "
                        "compiles into a scanner matching no input at all, its graph deciding the repetition's end on "
                        "one byte, so the token set cannot be read from it",
                        line};
            }

            check_repetitions(*kind.operand, inside, line);
        }
    }};

    std::visit(check, node.kind);
}

void check_dot_repetitions(const Node& node, const std::string& written, const std::size_t line)
{
    const auto every_scalar{[](const Merged& body) {
        const auto& [char_class, literal]{body};

        const auto& [set, unicode, captured]{char_class};

        if (literal)
        {
            return false;
        }

        const auto left{universe_of(unicode).minus(set)};

        return left.empty() && (!unicode || set.spans_gap());
    }};

    const auto check{[&]<typename Kind>(const Kind& kind) {
        if constexpr (std::is_same_v<Kind, Concat>)
        {
            for (const auto& part : kind.parts)
            {
                check_dot_repetitions(part, written, line);
            }
        }
        else if constexpr (std::is_same_v<Kind, Alternation>)
        {
            for (const auto& branch : kind.branches)
            {
                check_dot_repetitions(branch, written, line);
            }
        }
        else if constexpr (std::is_same_v<Kind, Repeat>)
        {
            const auto seen{kind.max || kind.min > 1 ? std::nullopt : merged(*kind.operand)};

            if (seen && every_scalar(*seen))
            {
                const auto message{std::format(
                        "the pattern {} is refused: a `*` or `+` over the dot under `s`, or over a class of every "
                        "scalar or every byte, is one logos 0.15.1 refuses, since it would consume the source to its "
                        "end as logos does not backtrack",
                        written)};

                throw Spec_error{message, line};
            }

            check_dot_repetitions(*kind.operand, written, line);
        }
    }};

    std::visit(check, node.kind);
}

} // namespace munch::tools::audit
