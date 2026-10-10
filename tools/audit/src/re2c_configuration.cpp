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
 * @brief The configurations whose value re2c 3.1 reads as a number, under every name its configuration lexer gives
 *        them.
 */
constexpr std::array<std::string_view, 62> number_configurations{
        "eof",
        "sentinel",
        "yyfill:enable",
        "yyfill:parameter",
        "yyfill:check",
        "tags",
        "flags:tags",
        "flags:T",
        "leftmost-captures",
        "flags:leftmost-captures",
        "posix-captures",
        "flags:posix-captures",
        "flags:P",
        "invert-captures",
        "define:YYFILL:naked",
        "define:YYGETCONDITION:naked",
        "define:YYGETSTATE:naked",
        "define:YYSETCONDITION:naked",
        "define:YYSETSTATE:naked",
        "variable:yych:conversion",
        "yych:conversion",
        "variable:yych:emit",
        "yych:emit",
        "variable:yybm:hex",
        "yybm:hex",
        "state:abort",
        "state:nextlabel",
        "bit-vectors",
        "flags:bit-vectors",
        "flags:b",
        "debug-output",
        "flags:debug-output",
        "flags:d",
        "computed-gotos",
        "flags:computed-gotos",
        "flags:g",
        "nested-ifs",
        "flags:nested-ifs",
        "flags:s",
        "case-insensitive",
        "flags:case-insensitive",
        "case-inverted",
        "flags:case-inverted",
        "case-ranges",
        "flags:case-ranges",
        "unsafe",
        "flags:unsafe",
        "encoding:ebcdic",
        "flags:ecb",
        "flags:e",
        "encoding:utf32",
        "flags:unicode",
        "flags:u",
        "encoding:ucs2",
        "flags:wide-chars",
        "flags:w",
        "encoding:utf16",
        "flags:utf-16",
        "flags:x",
        "encoding:utf8",
        "flags:utf-8",
        "flags:8"};

/**
 * @brief The configurations whose value re2c 3.1 reads as a number that is not negative.
 */
constexpr std::array<std::string_view, 3> nonnegative_configurations{
        "computed-gotos:threshold", "cgoto:threshold", "indent:top"};

/**
 * @brief The configurations whose value re2c 3.1 reads as a string, under every name its configuration lexer gives
 *        them.
 */
constexpr std::array<std::string_view, 58> string_configurations{
        "api:sigil",
        "header",
        "flags:type-header",
        "flags:t",
        "tags:prefix",
        "tags:expression",
        "define:YYBACKUP",
        "define:YYBACKUPCTX",
        "define:YYCONDTYPE",
        "define:YYCTYPE",
        "define:YYCTXMARKER",
        "define:YYCURSOR",
        "define:YYDEBUG",
        "define:YYFILL",
        "define:YYFILL@len",
        "define:YYGETCONDITION",
        "define:YYGETSTATE",
        "define:YYLESSTHAN",
        "define:YYLIMIT",
        "define:YYMARKER",
        "define:YYMTAGN",
        "define:YYMTAGP",
        "define:YYPEEK",
        "define:YYRESTORE",
        "define:YYRESTORECTX",
        "define:YYRESTORETAG",
        "define:YYSETCONDITION",
        "define:YYSETCONDITION@cond",
        "define:YYSETSTATE",
        "define:YYSETSTATE@state",
        "define:YYSHIFT",
        "define:YYSHIFTSTAG",
        "define:YYSHIFTMTAG",
        "define:YYSKIP",
        "define:YYSTAGN",
        "define:YYSTAGP",
        "variable:yyctable",
        "variable:yyaccept",
        "variable:yytarget",
        "variable:yystate",
        "variable:yych",
        "variable:yybm",
        "variable:yystable",
        "cond:prefix",
        "condprefix",
        "cond:enumprefix",
        "condenumprefix",
        "cond:divider",
        "cond:divider@cond",
        "cond:goto",
        "cond:goto@cond",
        "indent:string",
        "label:prefix",
        "labelprefix",
        "label:yyfill",
        "label:yyFillLabel",
        "label:yyloop",
        "label:yyNext"};

/**
 * @brief The names of the start label's configuration, whose value re2c 3.1 reads as a number where one follows the
 *        `=` and as a string where none does.
 */
constexpr std::array<std::string_view, 2> label_configurations{"label:start", "startlabel"};

/**
 * @brief A set of configurations whose values re2c 3.1 reads alike: their names and what it reads the value as.
 */
struct Valued_configurations
{
    /**
     * @brief The names, every alias among them.
     */
    std::span<const std::string_view> names{};

    /**
     * @brief What the value is read as.
     */
    Configuration_value value{};
};

/**
 * @brief The configurations whose value is no choice among words, by what re2c 3.1 reads the value as.
 */
constexpr std::array<Valued_configurations, 4> valued_configurations{
        {{.names = number_configurations, .value = Configuration_value::number},
         {.names = nonnegative_configurations, .value = Configuration_value::nonnegative_number},
         {.names = string_configurations, .value = Configuration_value::string},
         {.names = label_configurations, .value = Configuration_value::number_or_string}}};

/**
 * @brief The words the API configuration chooses among.
 */
constexpr std::array<std::string_view, 2> api_words{"default", "custom"};

/**
 * @brief The words the API style configuration chooses among.
 */
constexpr std::array<std::string_view, 2> api_style_words{"functions", "free-form"};

/**
 * @brief The words the encoding policy configuration chooses among.
 */
constexpr std::array<std::string_view, 3> policy_words{"ignore", "substitute", "fail"};

/**
 * @brief The words the empty class configuration chooses among.
 */
constexpr std::array<std::string_view, 3> empty_class_words{"match-empty", "match-none", "error"};

/**
 * @brief A configuration that chooses among words: a name it goes by and the words.
 */
struct Choice_configuration
{
    /**
     * @brief The name.
     */
    std::string_view name{};

    /**
     * @brief The words, in the order re2c lists them.
     */
    std::span<const std::string_view> words{};
};

/**
 * @brief The configurations that choose among words, under every name re2c 3.1's configuration lexer gives them.
 */
constexpr std::array<Choice_configuration, 7> choice_configurations{
        {{.name = "api", .words = api_words},
         {.name = "flags:input", .words = api_words},
         {.name = "api:style", .words = api_style_words},
         {.name = "encoding-policy", .words = policy_words},
         {.name = "flags:encoding-policy", .words = policy_words},
         {.name = "empty-class", .words = empty_class_words},
         {.name = "flags:empty-class", .words = empty_class_words}}};

/**
 * @brief Returns the value a configuration gives a flag, when the configuration names it under one of its names: a flag
 *        is set by any value but `0`.
 * @param option The configuration as read, its name, `=` and its value.
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
 * @param option The configuration as read, its name, `=` and its value.
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
 * @param option The configuration as read, its name, `=` and its value.
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

std::optional<Configuration_syntax> syntax_of(const std::string_view name)
{
    const auto chooser{std::ranges::find(choice_configurations, name, &Choice_configuration::name)};

    if (chooser != choice_configurations.end())
    {
        return Configuration_syntax{.value = Configuration_value::choice, .choices = chooser->words};
    }

    for (const auto& [names, value] : valued_configurations)
    {
        if (std::ranges::contains(names, name))
        {
            return Configuration_syntax{.value = value, .choices = {}};
        }
    }

    return std::nullopt;
}

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
    for (auto& [canonical, name, named_at] : pointers)
    {
        if (const auto key{std::format("define:{}=", canonical)}; option.starts_with(key))
        {
            auto value{option.substr(key.size())};

            name = pointer_name(std::move(value));

            named_at = line;
        }
    }
}

} // namespace munch::tools::audit
