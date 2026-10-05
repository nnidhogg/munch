#include "munch/tools/audit/re2c_configuration.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief An encoding and the three names the manual gives the configuration that sets it.
 */
struct Encoding_names
{
    /**
     * @brief The encoding.
     */
    Re2c_encoding encoding{};

    /**
     * @brief Its configuration's names: the canonical one and its two `flags:` aliases.
     */
    std::array<std::string_view, 3> names{};
};

/**
 * @brief The encodings a configuration can set, each under the three names the manual gives it.
 */
constexpr std::array<Encoding_names, 5> encodings{
        {{.encoding = Re2c_encoding::ebcdic, .names = {"encoding:ebcdic", "flags:ecb", "flags:e"}},
         {.encoding = Re2c_encoding::ucs2, .names = {"encoding:ucs2", "flags:wide-chars", "flags:w"}},
         {.encoding = Re2c_encoding::utf8, .names = {"encoding:utf8", "flags:utf-8", "flags:8"}},
         {.encoding = Re2c_encoding::utf16, .names = {"encoding:utf16", "flags:utf-16", "flags:x"}},
         {.encoding = Re2c_encoding::utf32, .names = {"encoding:utf32", "flags:unicode", "flags:u"}}}};

/**
 * @brief Returns the value a configuration gives a flag, when the configuration names it under one of its names: a flag
 *        is set by any value but `0`.
 * @param option The configuration, its blanks taken off.
 * @param names The names the flag's configuration goes by.
 * @return Whether the flag is set, or std::nullopt when the configuration is another.
 */
[[nodiscard]] std::optional<bool> assigned(const std::string& option, const std::span<const std::string_view> names)
{
    for (const auto name : names)
    {
        if (const auto key{std::format("{}=", name)}; option.starts_with(key))
        {
            return option.substr(key.size()) != "0";
        }
    }

    return std::nullopt;
}

/**
 * @brief Returns whether a name standing in parentheses is wrapped by them whole, the first one closing only at the
 *        last byte.
 * @param value The name, opening with `(` and closing with `)`.
 * @return True when the two parentheses at its ends are one pair.
 */
[[nodiscard]] bool is_wrapped(const std::string_view value) noexcept
{
    std::ptrdiff_t depth{0};

    for (std::size_t at{0}; at + 1 < value.size(); ++at)
    {
        if (value[at] == '(')
        {
            ++depth;
        }
        else if (value[at] == ')')
        {
            --depth;
        }

        if (depth == 0)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief Returns an encoding's name as a refusal spells it.
 * @param encoding The encoding.
 * @return The name re2c's own option gives it.
 */
[[nodiscard]] std::string_view named(const Re2c_encoding encoding)
{
    switch (encoding)
    {
    case Re2c_encoding::ebcdic:
        return "ebcdic";
    case Re2c_encoding::ucs2:
        return "ucs2";
    case Re2c_encoding::utf8:
        return "utf8";
    case Re2c_encoding::utf16:
        return "utf16";
    case Re2c_encoding::utf32:
        return "utf32";
    case Re2c_encoding::ascii:
        break;
    }

    return "ascii";
}

/**
 * @brief Applies a configuration of the case flags, `case-inverted` and `case-insensitive` under their canonical names
 *        and their `flags:` aliases.
 *
 * There is no configuration for the flex syntax, `re2c:flags:F` being a configuration re2c rejects, so only the command
 * line brings that one.
 * @param option The configuration, its blanks taken off.
 * @param flags The flags the block's configurations leave, the configuration's applied.
 */
void set_case_flags(const std::string& option, Re2c_flags& flags)
{
    static constexpr std::array<std::string_view, 2> inverted{"case-inverted", "flags:case-inverted"};

    if (const auto value{assigned(option, inverted)})
    {
        flags.case_inverted = *value;
    }

    static constexpr std::array<std::string_view, 2> insensitive{"case-insensitive", "flags:case-insensitive"};

    if (const auto value{assigned(option, insensitive)})
    {
        flags.case_insensitive = *value;
    }
}

/**
 * @brief Applies a configuration of an encoding: an encoding is a configuration of its own, under the three names the
 *        manual gives each, and setting one to 0 leaves the block reading ASCII again.
 * @param option The configuration, its blanks taken off.
 * @param flags The flags the block's configurations leave, the configuration's applied.
 * @return Whether the configuration is one of an encoding.
 */
[[nodiscard]] bool set_encoding(const std::string& option, Re2c_flags& flags)
{
    auto matched{false};

    for (const auto& [encoding, names] : encodings)
    {
        const auto value{assigned(option, names)};

        if (!value)
        {
            continue;
        }

        const auto unset{flags.encoding == encoding ? Re2c_encoding::ascii : flags.encoding};

        flags.encoding = *value ? encoding : unset;

        matched = true;
    }

    return matched;
}

/**
 * @brief Returns a scan pointer's name as a `define:` configuration gives it and the actions write it: re2c reads the
 *        value quoted or bare, `= "cur";` and `= cur;` naming the same pointer, so the quotes are not part of the name,
 *        and parentheses around the whole name name the same pointer, `"(cur)"` and `"cur"` alike, so the ones wrapping
 *        it are taken off.
 * @param value The configuration's value.
 * @return The name.
 */
[[nodiscard]] std::string pointer_name(std::string value)
{
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
    {
        value = std::string{without_delimiters(value)};
    }

    while (value.size() >= 2 && value.front() == '(' && value.back() == ')' && is_wrapped(value))
    {
        value = std::string{without_delimiters(value)};
    }

    return value;
}

} // namespace

std::string unreadable(const Re2c_encoding encoding)
{
    switch (encoding)
    {
    case Re2c_encoding::ascii:
    case Re2c_encoding::utf8:
        return {};
    case Re2c_encoding::ebcdic:
        // One byte a code point, as ASCII has it, but not the same code point for the same byte.
        return "the ebcdic encoding gives a byte another code point than ASCII does, a mapping the reading has not "
               "got";
    default:
        return std::format(
                "the {} encoding's code unit is not one byte, so its scanner reads no byte stream the audit can "
                "model",
                named(encoding));
    }
}

void configure(
        const std::string& option, const std::size_t line, Re2c_flags& flags, std::size_t& encoding_line,
        std::optional<std::size_t>& api_custom, Pointers_t& pointers)
{
    set_case_flags(option, flags);

    // Whether the encoding is one the reading has got is the settled configuration's to say, the last assignment
    // governing, so a block turning UTF-16 on and off again reads as ASCII; the caller asks after.
    if (set_encoding(option, flags))
    {
        encoding_line = line;
    }

    const auto sets{[&option](const std::string_view key) { return option.starts_with(key); }};

    static constexpr std::array<std::string_view, 2> policy_keys{"encoding-policy=", "flags:encoding-policy="};

    // The policy decides what becomes of the surrogates, which the default encoding of them as code points is.
    if (std::ranges::any_of(policy_keys, sets) && !option.ends_with("=ignore"))
    {
        throw Spec_error{
                "an encoding policy other than the default leaves the surrogates matched otherwise than the audit "
                "reads them",
                line};
    }

    static constexpr std::array<std::string_view, 2> api_keys{"api=", "flags:input="};

    // Which API the block reads under is refused at the block's end, the last assignment governing.
    if (std::ranges::any_of(api_keys, sets))
    {
        api_custom = option.ends_with("=default") ? std::nullopt : std::optional{line};
    }

    // A `define:` configuration renaming a scan pointer says what the actions of this block call it.
    for (auto& [canonical, name] : pointers)
    {
        if (const auto key{std::format("define:{}=", canonical)}; option.starts_with(key))
        {
            auto value{option.substr(key.size())};

            name = pointer_name(std::move(value));
        }
    }
}

} // namespace munch::tools::audit
