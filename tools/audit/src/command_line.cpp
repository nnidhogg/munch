#include "munch/tools/audit/command_line.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>

#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The shortest window `--windows` takes.
 */
constexpr std::size_t shortest_window_limit{1};

/**
 * @brief The longest window `--windows` takes.
 */
constexpr std::size_t longest_window_limit{8};

/**
 * @brief What a byte written in hex opens with, `0xHH`.
 */
constexpr std::string_view hex_prefix{"0x"};

/**
 * @brief The most hex digits a byte written in hex takes.
 */
constexpr std::size_t most_hex_digits{2};

/**
 * @brief Returns the longest window `--windows` gives.
 * @param text The option's value, a decimal.
 * @return The limit.
 * @throws std::invalid_argument If the text is no decimal from shortest_window_limit to longest_window_limit.
 */
[[nodiscard]] std::size_t parse_window_limit(const std::string_view text)
{
    std::size_t limit{};

    const auto text_end{text.data() + text.size()};

    const auto [end, error]{std::from_chars(text.data(), text_end, limit)};

    if (error != std::errc{} || end != text_end || limit < shortest_window_limit || limit > longest_window_limit)
    {
        const auto message{
                std::format("--windows takes {} to {}, not '{}'", shortest_window_limit, longest_window_limit, text)};

        throw std::invalid_argument{message};
    }

    return limit;
}

/**
 * @brief Returns a byte as the command line spells one.
 * @param text A single character, one of the escapes `\n`, `\t`, `\r`, `\0`, or `0xHH`.
 * @return The byte.
 * @throws std::invalid_argument If the text spells none.
 */
[[nodiscard]] unsigned char parse_byte(const std::string_view text)
{
    if (text.size() == 1)
    {
        return static_cast<unsigned char>(text.front());
    }

    if (text == R"(\n)")
    {
        return '\n';
    }

    if (text == R"(\t)")
    {
        return '\t';
    }

    if (text == R"(\r)")
    {
        return '\r';
    }

    if (text == R"(\0)")
    {
        return 0;
    }

    const auto digits{text.starts_with(hex_prefix) ? text.substr(hex_prefix.size()) : std::string_view{}};

    const auto hex{!digits.empty() && digits.size() <= most_hex_digits && std::ranges::all_of(digits, is_hex_digit)};

    if (hex)
    {
        unsigned value{};

        const auto digits_end{digits.data() + digits.size()};

        std::ignore = std::from_chars(digits.data(), digits_end, value, hex_base);

        return static_cast<unsigned char>(value);
    }

    throw std::invalid_argument{std::format("'{}' is not a byte", text)};
}

/**
 * @brief The usage text, printed on a command-line error.
 */
constexpr std::string_view usage_text{R"(usage: munch-audit [options] FILE...

Reads a flex, re2c, ANTLR 4 or logos file and reports, per scanner and start condition, what the token set
certifies: the bytes and windows a parallel scan may cut at, why the other candidates fail, and what certifying one
would cost.

  --flex, --re2c, --antlr, --logos
                        read every file as this kind; otherwise a file opening a re2c block is re2c, one opening
                        with a grammar declaration is ANTLR, one deriving Logos is logos, the rest flex
  --flex-syntax         re2c's -F: flex-style definitions, {name} references, bare letters literal
  --case-inverted       re2c's --case-inverted: "..." case-insensitive and '...' exact
  --case-insensitive    re2c's --case-insensitive: both quotes case-insensitive; flex's -i: the case option on,
                        the file's own %option words overriding it
  --returns NAME        a form besides return an action returns a token through, NAME(x), NAME = x or NAME alone;
                        may repeat
  --include DIR         a directory the scanner's includes are looked for in, as the compiler's -I names it: an
                        angle-bracket include is looked for there alone, a quoted one beside the file including it
                        first; may repeat
  --condition NAME      audit this start condition only; may repeat
  --windows N           the longest window tried, 3 unless given; 4 is the planners' own limit
  --price BYTE          price this byte as well, written as a character, \n \t \r \0, or 0xHH; may repeat
  --input FILE          measure the certified-anchor supply on this file: how many positions its certificates cut
                        at, per kibibyte, and the gaps between them
  --repairs             where no byte certifies, try every byte as a token of its own, visible and discarded, and
                        report each that certifies once added
  --json                one JSON document instead of text
  --require-certified BYTE
                        require every audited scanner and condition to certify this byte exactly, written as for
                        --price; may repeat
  --require-certified-modulo BYTE
                        require it certified once the discarded tokens are deleted, the report's certified modulo
                        discarded row; may repeat

Exit status is 0 when every scanner and condition audited and met every requirement, 1 when one was refused, a file
with no scanner among them, whatever the requirements found, 2 on a command-line error, a --condition no scanner has
or no rule stands in among them, and 3 when every one audited and one did not certify a byte required of it, each
such byte named on standard error after the whole report.
)"};

} // namespace

Options parse_options(const std::span<const std::string_view> arguments)
{
    Options options{};

    for (std::size_t at{0}; at < arguments.size(); ++at)
    {
        const auto argument{arguments[at]};

        const auto value{[&]() -> std::string_view {
            if (at + 1 >= arguments.size())
            {
                throw std::invalid_argument{std::format("{} needs a value", argument)};
            }

            return arguments[++at];
        }};

        if (argument == "--flex")
        {
            options.kind = Kind::flex;
        }
        else if (argument == "--re2c")
        {
            options.kind = Kind::re2c;
        }
        else if (argument == "--antlr")
        {
            options.kind = Kind::antlr;
        }
        else if (argument == "--logos")
        {
            options.kind = Kind::logos;
        }
        else if (argument == "--flex-syntax")
        {
            options.re2c_flags.flex_syntax = true;
        }
        else if (argument == "--case-inverted")
        {
            options.re2c_flags.case_inverted = true;
        }
        else if (argument == "--case-insensitive")
        {
            options.re2c_flags.case_insensitive = true;

            options.flex_case_insensitive = true;
        }
        else if (argument == "--returns")
        {
            options.returning.emplace_back(value());
        }
        else if (argument == "--include")
        {
            options.include_dirs.emplace_back(value());
        }
        else if (argument == "--condition")
        {
            options.conditions.emplace_back(value());
        }
        else if (argument == "--windows")
        {
            const auto text{value()};

            options.window_limit = parse_window_limit(text);
        }
        else if (argument == "--price")
        {
            const auto text{value()};

            options.priced.push_back(parse_byte(text));
        }
        else if (argument == "--input")
        {
            options.input = value();
        }
        else if (argument == "--repairs")
        {
            options.repairs = true;
        }
        else if (argument == "--json")
        {
            options.json = true;
        }
        else if (argument == "--require-certified" || argument == "--require-certified-modulo")
        {
            const auto text{value()};

            options.required.push_back(Requirement{
                    .byte = parse_byte(text),
                    .modulo = argument == "--require-certified-modulo",
                    .text = std::string{text}});
        }
        else if (argument.starts_with("--"))
        {
            throw std::invalid_argument{std::format("unknown option {}", argument)};
        }
        else
        {
            options.files.emplace_back(argument);
        }
    }

    if (options.files.empty())
    {
        throw std::invalid_argument{"no file named"};
    }

    return options;
}

std::string_view usage() noexcept
{
    return usage_text;
}

} // namespace munch::tools::audit
