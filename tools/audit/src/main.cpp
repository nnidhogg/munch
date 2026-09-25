#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/price.hpp"
#include "munch/tools/audit/read_antlr.hpp"
#include "munch/tools/audit/read_flex.hpp"
#include "munch/tools/audit/read_logos.hpp"
#include "munch/tools/audit/read_re2c.hpp"
#include "munch/tools/audit/report.hpp"
#include "munch/tools/audit/supply.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The usage text, printed on a command-line error.
 */
constexpr std::string_view usage{R"(usage: munch-audit [options] FILE...

Reads a flex, re2c, ANTLR 4 or logos file and reports, per scanner and start condition, what the token set
certifies: the bytes and windows a parallel scan may cut at, why the other candidates fail, and what certifying one
would cost.

  --flex, --re2c, --antlr, --logos
                        read every file as this kind; otherwise a file opening a re2c block is re2c, one opening
                        with a grammar declaration is ANTLR, one deriving Logos is logos, the rest flex
  --flex-syntax         re2c's -F: flex-style definitions, {name} references, bare letters literal
  --case-inverted       re2c's --case-inverted: "..." case-insensitive and '...' exact
  --case-insensitive    re2c's --case-insensitive: both quotes case-insensitive
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
  --json                one JSON document instead of text

Exit status is 0 when every scanner and condition audited, 1 when one was refused, a file with no scanner among
them, 2 on a command-line error, a --condition no scanner has or no rule stands in among them.
)"};

/**
 * @brief The generator a file was written for, which chooses its reader.
 */
enum class Kind : std::uint8_t
{
    flex,
    re2c,
    antlr,
    logos
};

/**
 * @brief What the command line asked for.
 */
struct Options
{
    /**
     * @brief The kind every file is read as, or by its name when not given.
     */
    std::optional<Kind> kind;

    /**
     * @brief re2c's command-line flags, for its files.
     */
    Re2c_flags re2c_flags;

    /**
     * @brief The forms besides `return` an action returns a token through.
     */
    Returning_t returning;

    /**
     * @brief The start conditions to audit, every one with rules when empty.
     */
    std::vector<std::string> conditions;

    /**
     * @brief The bytes to price besides the newline and the near misses.
     */
    std::vector<unsigned char> priced;

    /**
     * @brief The files.
     */
    std::vector<std::string> files;

    /**
     * @brief The directories an include is looked for in, as the compiler's `-I` names them, in order.
     */
    std::vector<std::string> include_dirs;

    /**
     * @brief The path of the input the certified-anchor supply is measured on, none when empty.
     */
    std::string input;

    /**
     * @brief The longest window tried.
     */
    std::size_t window_limit{3};

    /**
     * @brief Whether the output is JSON rather than text.
     */
    bool json{false};
};

/**
 * @brief One start condition's outcome: its report, or why it was refused.
 */
struct Outcome
{
    /**
     * @brief The condition's name.
     */
    std::string condition;

    /**
     * @brief Why the token set could not be built, empty when it was.
     */
    std::string refused;

    /**
     * @brief The report, when the token set was built.
     */
    std::optional<Report> report;

    /**
     * @brief The report's certified-anchor supply on the input, when one was given and the report built.
     */
    std::optional<Supply> supply;

    /**
     * @brief How many rules the condition holds.
     */
    std::size_t rules{};
};

/**
 * @brief A byte as the command line spells one.
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

    if (text.starts_with("0x") && text.size() >= 3 && text.size() <= 4)
    {
        auto value{0U};

        for (const auto digit : text.substr(2))
        {
            const auto lowered{static_cast<char>(digit | 0x20)};

            const auto hex{
                    lowered >= '0' && lowered <= '9' ? lowered - '0' :
                    lowered >= 'a' && lowered <= 'f' ? lowered - 'a' + 10 :
                                                       -1};

            if (hex < 0)
            {
                throw std::invalid_argument{std::format("'{}' is not a byte", text)};
            }

            value = value * 16 + static_cast<unsigned>(hex);
        }

        return static_cast<unsigned char>(value);
    }

    throw std::invalid_argument{std::format("'{}' is not a byte", text)};
}

/**
 * @brief Reads the command line.
 * @param arguments The arguments after the program's name.
 * @return The options.
 * @throws std::invalid_argument If an option is unknown, lacks its value, or no file is named.
 */
[[nodiscard]] Options parse_options(const std::span<const std::string_view> arguments)
{
    Options options;

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

            std::size_t limit{0};

            const auto [end, error]{std::from_chars(text.data(), text.data() + text.size(), limit)};

            if (error != std::errc{} || end != text.data() + text.size() || limit < 1 || limit > 8)
            {
                throw std::invalid_argument{std::format("--windows takes 1 to 8, not '{}'", text)};
            }

            options.window_limit = limit;
        }
        else if (argument == "--price")
        {
            options.priced.push_back(parse_byte(value()));
        }
        else if (argument == "--input")
        {
            options.input = value();
        }
        else if (argument == "--json")
        {
            options.json = true;
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

/**
 * @brief The kind a file's text says: ANTLR when its first item is a grammar declaration; logos when a derive names
 *        Logos; re2c when it opens a re2c block, which no other file does; flex otherwise. The name says nothing,
 *        since re2c lives in files of any extension and PHP's re2c scanners end in `.l`. Each question is asked of
 *        the text in the language it asks about, since the three do not lex alike: Rust nests its block comments
 *        and writes a lifetime where C writes a character literal, so a marker inside a nested comment or after a
 *        lifetime opens nothing, and an ANTLR character set holding a re2c opener is the set's bytes. A literal,
 *        comment and a raw string hold no marker in any of them.
 * @param source The file's text.
 * @return The kind.
 */
[[nodiscard]] Kind kind_of(const std::string_view source) noexcept
{
    constexpr std::string_view blanks{" \t\r\n"};

    constexpr auto npos{std::string_view::npos};

    const auto is_name{[](const char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
               byte == '_';
    }};

    // Where a comment or a literal opening at `at` ends, read as the named language reads it: a block comment at
    // its close, Rust's nesting so that an inner `/*` takes another `*/` to close; a line comment at the line's
    // end, or at a carriage return where the language ends one there; a quoted literal at its closing quote, an
    // escaped byte carried; and a raw string, `r"..."`, `r#"..."#` or C++'s `R"d(...)d"`, at the delimiter that
    // closes it, no escape read. Rust's `'` opens a literal only when one byte or an escape closes it, since
    // `'static` is a lifetime and not a literal that swallows the text after it.
    const auto ends{[&](const std::size_t at, const bool rust) -> std::optional<std::size_t> {
        const auto rest{source.substr(at)};

        if (rest.starts_with("/*"))
        {
            auto depth{1};

            for (auto scan{at + 2}; scan + 1 < source.size();)
            {
                if (rust && source.substr(scan).starts_with("/*"))
                {
                    ++depth;

                    scan += 2;
                }
                else if (source.substr(scan).starts_with("*/"))
                {
                    --depth;

                    scan += 2;

                    if (depth == 0)
                    {
                        return scan;
                    }
                }
                else
                {
                    ++scan;
                }
            }

            return source.size();
        }

        if (rest.starts_with("//"))
        {
            return std::min(source.find_first_of(rust ? "\n" : "\n\r", at), source.size());
        }

        // A `'` after a name byte is C++'s digit separator, `1'000`, and opens no literal.
        if (rest.starts_with('\'') && !(at > 0 && is_name(source[at - 1])))
        {
            const auto shut{rest.starts_with("'\\") ? rest.find('\'', 3) : rest.find('\'', 2)};

            if (rust && (shut == npos || shut > 4))
            {
                return at + 1;
            }

            return shut == npos ? source.size() : at + shut + 1;
        }

        if (rest.starts_with('"'))
        {
            auto end{at + 1};

            for (; end < source.size() && source[end] != '"' && source[end] != '\n'; ++end)
            {
                end += source[end] == '\\' ? 1 : 0;
            }

            return std::min(end + 1, source.size());
        }

        // A raw string, which the two languages write differently and which both passes read either way, since the
        // pass looking for a marker does not yet know the language it is looking in: C++ takes the delimiter of
        // `R"d(...)d"`, an encoding prefix allowed before the `R`, and closes at its `)d"`; Rust counts the hashes
        // of `r#"..."#`. An embedded quote closes neither, which is what a marker hides behind.
        const auto raw{[&]() -> std::size_t {
            for (const std::string_view mark : {"u8R\"", "uR\"", "UR\"", "LR\"", "R\""})
            {
                if (rest.starts_with(mark))
                {
                    return mark.size() - 1;
                }
            }

            return 0UZ;
        }()};

        if (raw > 0 && !(at > 0 && is_name(source[at - 1])))
        {
            const auto open{rest.find('(', raw + 1)};

            // C++ writes the delimiter without blanks, parentheses or a backslash, and in sixteen bytes at most,
            // so a quote that opens none of that is the ordinary string the branch above reads.
            const auto delimiter{open == npos ? std::string_view{} : rest.substr(raw + 1, open - raw - 1)};

            if (open != npos && delimiter.size() <= 16 && delimiter.find_first_of(" \t\r\n()\\") == npos)
            {
                const auto shut{std::string{')'} + std::string{delimiter} + '"'};

                const auto close{source.find(shut, at + open)};

                return close == npos ? source.size() : close + shut.size();
            }
        }

        const auto prefix{rust ? (rest.starts_with("br") ? 2UZ : rest.starts_with('r') ? 1UZ : 0UZ) : 0UZ};

        if (prefix == 0 || (at > 0 && is_name(source[at - 1])))
        {
            return std::nullopt;
        }

        const auto hashes{rest.find_first_not_of('#', prefix)};

        if (hashes == npos || rest[hashes] != '"')
        {
            return std::nullopt;
        }

        for (auto close{source.find('"', at + hashes + 1)}; close != npos; close = source.find('"', close + 1))
        {
            const auto after{source.substr(close + 1, hashes - prefix)};

            if (after.size() == hashes - prefix && after.find_first_not_of('#') == npos)
            {
                return close + 1 + after.size();
            }
        }

        return source.size();
    }};

    // The first item, blanks, comments and byte order marks stepped over, as ANTLR's lexer steps over them.
    const auto trivia{[&](std::size_t at) {
        for (;;)
        {
            at = std::min(source.find_first_not_of(blanks, at), source.size());

            if (source.substr(at).starts_with("\xEF\xBB\xBF"))
            {
                at += 3;
            }
            else if (source.substr(at).starts_with("//") || source.substr(at).starts_with("/*"))
            {
                at = *ends(at, false);
            }
            else
            {
                return at;
            }
        }
    }};

    // The position after the keyword when it stands at `at` and a blank or a comment follows it, so that `grammarx`
    // is no declaration; nothing otherwise.
    const auto word{[&](const std::size_t at, const std::string_view keyword) -> std::optional<std::size_t> {
        const auto end{at + keyword.size()};

        if (!source.substr(at).starts_with(keyword) || end >= source.size())
        {
            return std::nullopt;
        }

        const auto follows{source.substr(end)};

        return blanks.contains(follows.front()) || follows.starts_with("//") || follows.starts_with("/*") ?
                       std::optional{end} :
                       std::nullopt;
    }};

    auto at{trivia(0)};

    if (const auto lexer{word(at, "lexer")})
    {
        at = trivia(*lexer);
    }
    else if (const auto parser{word(at, "parser")})
    {
        at = trivia(*parser);
    }

    if (word(at, "grammar"))
    {
        return Kind::antlr;
    }

    // Past the blanks and comments after a position, for the pieces of an attribute, which Rust lets stand apart.
    const auto past{[&](std::size_t from, const bool rust) {
        for (from = std::min(source.find_first_not_of(blanks, from), source.size());
             from < source.size() && (source.substr(from).starts_with("//") || source.substr(from).starts_with("/*"));
             from = std::min(source.find_first_not_of(blanks, *ends(from, rust)), source.size()))
        {
        }

        return from;
    }};

    // A derive naming Logos, the file read as Rust: `#`, `[`, the word `derive` or the `cfg_attr` that applies one,
    // and its list, with blanks and comments allowed between every two of them, and Logos the last segment of one of
    // the list's paths. Whether a `cfg_attr` predicate holds is the reader's reading and not this choice of reader:
    // a Rust file is read by the Rust reader either way, which then says what the derive it applies scans.
    for (std::size_t scan{0}; scan < source.size();)
    {
        if (const auto end{ends(scan, true)})
        {
            scan = *end > scan ? *end : scan + 1;

            continue;
        }

        if (!source.substr(scan).starts_with('#'))
        {
            ++scan;

            continue;
        }

        auto open{past(scan + 1, true)};

        if (open >= source.size() || source[open] != '[')
        {
            ++scan;

            continue;
        }

        open = past(open + 1, true);

        const auto applies{source.substr(open).starts_with("cfg_attr")};

        if (!applies && !source.substr(open).starts_with("derive"))
        {
            ++scan;

            continue;
        }

        open = past(open + (applies ? 8 : 6), true);

        // The list's own closing parenthesis, the ones inside a comment or a string of it passed over, so that
        // `#[derive(/* ) */ Logos)]` names Logos as the attribute Rust reads there does.
        const auto close{[&]() -> std::size_t {
            if (open >= source.size() || source[open] != '(')
            {
                return npos;
            }

            for (auto scan{open}, depth{0UZ}; scan < source.size();)
            {
                if (const auto past{ends(scan, true)}; past && *past > scan)
                {
                    scan = *past;

                    continue;
                }

                depth += source[scan] == '(' ? 1 : source[scan] == ')' ? -1 : 0;

                if (source[scan] == ')' && depth == 0)
                {
                    return scan;
                }

                ++scan;
            }

            return npos;
        }()};

        const auto list{close == npos ? std::string_view{} : source.substr(open, close - open)};

        // A `cfg_attr` that applies no derive names no scanner, whatever else its list holds.
        if (applies && list.find("derive") == npos)
        {
            ++scan;

            continue;
        }

        for (auto found{list.find("Logos")}; found != npos; found = list.find("Logos", found + 5))
        {
            const auto after{found + 5 < list.size() ? list[found + 5] : ' '};

            if (!is_name(list[found - 1]) && !is_name(after))
            {
                return Kind::logos;
            }
        }

        ++scan;
    }

    // A re2c block's opener, the file read as C, which is what a re2c block lives in.
    for (std::size_t scan{0}; scan < source.size();)
    {
        const auto rest{source.substr(scan)};

        if (rest.starts_with("/*!re2c") || rest.starts_with("/*!rules:re2c") || rest.starts_with("/*!local:re2c") ||
            rest.starts_with("/*!use:re2c"))
        {
            return Kind::re2c;
        }

        const auto end{ends(scan, false)};

        scan = end && *end > scan ? *end : scan + 1;
    }

    return Kind::flex;
}

/**
 * @brief The whole of a file.
 * @param path The file's path.
 * @return Its text.
 * @throws std::runtime_error If it cannot be opened.
 */
[[nodiscard]] std::string contents(const std::string& path)
{
    std::ifstream stream{path, std::ios::binary};

    if (!stream)
    {
        throw std::runtime_error{std::format("cannot read {}", path)};
    }

    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/**
 * @brief The name the report prints for a rule: what it returns when that is short, else its pattern, either cut
 *        to a width a row can hold.
 * @param spec The scanner.
 * @param rule The rule's index.
 * @return The name.
 */
[[nodiscard]] std::string label(const Lexer_spec& spec, const std::size_t rule)
{
    const auto& [pattern, expression, conditions, action, token, priority, line]{spec.rules[rule]};

    auto name{token && token->size() <= 40 ? *token : pattern};

    if (name.size() > 60)
    {
        // The cut falls before a code point's first byte, so a multibyte name keeps whole characters.
        auto cut{57UZ};

        while ((static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80)
        {
            --cut;
        }

        name = name.substr(0, cut) + "...";
    }

    return name;
}

/**
 * @brief Audits one start condition of a scanner, and measures the report's supply on the input when one was given.
 * @param spec The scanner.
 * @param condition The condition's name.
 * @param options The command line.
 * @param input The input the supply is measured on, none when none was given.
 * @return The outcome.
 */
[[nodiscard]] Outcome audit_condition(
        const Lexer_spec& spec, const std::string& condition, const Options& options,
        const std::optional<std::string>& input)
{
    const auto rules{active_rules(spec, condition).size()};

    try
    {
        const auto set{token_set(spec, condition)};

        auto report{audit(set, options.window_limit)};

        // The bytes asked for besides the report's own, unless already priced or certified.
        for (const auto byte : options.priced)
        {
            const auto priced{std::ranges::any_of(
                    report.prices, [byte](const Pricing& pricing) { return pricing.byte == byte; })};

            if (!priced && !std::ranges::binary_search(report.exact, byte))
            {
                report.prices.push_back(price(set, byte));
            }
        }

        auto measured{input ? std::optional{supply(report, compile(set), *input)} : std::nullopt};

        return Outcome{
                .condition = condition,
                .refused = {},
                .report = std::move(report),
                .supply = std::move(measured),
                .rules = rules};
    }
    catch (const Spec_error& error)
    {
        return Outcome{
                .condition = condition,
                .refused = error.what(),
                .report = std::nullopt,
                .supply = std::nullopt,
                .rules = rules};
    }
}

/**
 * @brief The start conditions of a scanner the command line asks for, in the order to report them: INITIAL first,
 *        then the declared ones, each only when it has rules.
 * @param spec The scanner.
 * @param options The command line.
 * @return The names.
 */
[[nodiscard]] std::vector<std::string> conditions_of(const Lexer_spec& spec, const Options& options)
{
    std::vector<std::string> names{"INITIAL"};

    for (const auto& [name, exclusive] : spec.conditions)
    {
        names.push_back(name);
    }

    std::erase_if(names, [&](const std::string& name) {
        const auto asked{options.conditions.empty() || std::ranges::contains(options.conditions, name)};

        return !asked || active_rules(spec, name).empty();
    });

    return names;
}

/**
 * @brief Writes one condition's outcome as text.
 * @param out The stream.
 * @param spec The scanner.
 * @param outcome The outcome.
 */
void write_text(std::ostream& out, const Lexer_spec& spec, const Outcome& outcome, const std::string_view input)
{
    const auto& [condition, refused, report, supply, rules]{outcome};

    out << std::format(
            "-- scanner at line {}, condition {}: {} rule{}\n", spec.line, condition, rules, rules == 1 ? "" : "s");

    // What the reading was governed by, before the figures it governed.
    out << options_row(spec.options);

    if (!report)
    {
        out << "refused: " << refused << "\n\n";

        return;
    }

    out << render(*report, [&spec](const std::size_t rule) { return label(spec, rule); });

    if (supply)
    {
        out << supply_section(*supply, input);
    }

    out << '\n';
}

/**
 * @brief Writes a scanner's options, definitions and rules as the JSON of what the reader read: the options that
 *        governed the reading, the named patterns, then each rule's line, pattern as written, start conditions,
 *        action and token, so that a reading can be held to the generator's own account of the file.
 * @param out The stream.
 * @param spec The scanner.
 */
void write_rules(std::ostream& out, const Lexer_spec& spec)
{
    out << "\"options\": " << options_json(spec.options) << ", \"definitions\": {";

    for (auto first{true}; const auto& [name, body] : spec.definitions)
    {
        out << (first ? "" : ", ") << json_string(name) << ": " << json_string(body);

        first = false;
    }

    out << "}, \"rules\": [";

    for (std::size_t index{0}; index < spec.rules.size(); ++index)
    {
        const auto& [pattern, expression, conditions, action, token, priority, line]{spec.rules[index]};

        std::string named;

        for (const auto& condition : conditions)
        {
            named += (named.empty() ? "" : ", ") + json_string(condition);
        }

        out << (index == 0 ? "\n        " : ",\n        ")
            << std::format(
                       R"({{"line": {}, "pattern": {}, "conditions": [{}], "action": {}, "token": {}}})", line,
                       json_string(pattern), named, json_string(action), token ? json_string(*token) : "null");
    }

    out << (spec.rules.empty() ? "]" : "\n      ]");
}

/**
 * @brief Writes one condition's outcome as the JSON object of a scanner's condition list, the supply on the input
 *        beside the report when one was measured.
 * @param out The stream.
 * @param spec The scanner.
 * @param outcome The outcome.
 * @param input The input the supply was measured on, empty when none was given.
 */
void write_json(std::ostream& out, const Lexer_spec& spec, const Outcome& outcome, const std::string_view input)
{
    const auto& [condition, refused, report, supply, rules]{outcome};

    out << std::format(R"(        {{"name": {}, "rules": {}, )", json_string(condition), rules);

    if (!report)
    {
        out << std::format(R"("refused": {}, "report": null}})", json_string(refused));

        return;
    }

    std::string document{json(*report, [&spec](const std::size_t rule) { return label(spec, rule); })};

    // The report's own lines, indented to where they sit in the document.
    for (auto at{document.find('\n')}; at != std::string::npos; at = document.find('\n', at + 9))
    {
        document.insert(at + 1, "        ");
    }

    out << R"("refused": null, "report": )" << document;

    if (supply)
    {
        out << R"(, "supply": )" << supply_json(*supply, input);
    }

    out << '}';
}

/**
 * @brief Audits every file, writing as it goes.
 * @param options The command line.
 * @param out The stream.
 * @return Whether every scanner and condition audited.
 */
[[nodiscard]] bool run(const Options& options, std::ostream& out)
{
    auto every{true};

    // The conditions asked for that no scanner of any file has, which is a command line naming nothing, and among
    // them the ones some scanner has with no rule in them, which is a command line naming no token set.
    std::set<std::string> unmatched(options.conditions.begin(), options.conditions.end());

    std::set<std::string> empty;

    const auto note_empty{[&options, &empty](const Lexer_spec& spec) {
        const auto audited{conditions_of(spec, options)};

        for (const auto& name : options.conditions)
        {
            const auto has{name == "INITIAL" || std::ranges::any_of(spec.conditions, [&name](const auto& condition) {
                               return condition.name == name;
                           })};

            if (has && !std::ranges::contains(audited, name))
            {
                empty.insert(name);
            }
        }
    }};

    // An empty input is measured like any other: it holds no anchor, which the supply says.
    const auto input{options.input.empty() ? std::nullopt : std::optional{contents(options.input)}};

    if (options.json)
    {
        out << "{\n  \"files\": [\n";
    }

    for (std::size_t index{0}; index < options.files.size(); ++index)
    {
        const auto& path{options.files[index]};

        const auto source{contents(path)};

        const auto kind{options.kind.value_or(kind_of(source))};

        std::vector<Lexer_spec> scanners;

        std::string refused;

        try
        {
            // A file the code includes is read as a compiler resolves it: a quoted name beside the file including it
            // and then on the include path, an angle-bracket name on the include path alone; a quoted one not
            // found is refused by the reader, an angle-bracket one not found taken for a system header's.
            const Include_reader_t includes{
                    [&path, &options](
                            const std::string_view name, const std::string_view from,
                            const Include_form form) -> std::optional<Included> {
                        std::vector<std::filesystem::path> where;

                        if (form == Include_form::quoted)
                        {
                            const auto including{
                                    from.empty() ? std::filesystem::path{path} : std::filesystem::path{from}};

                            where.push_back(including.parent_path());
                        }

                        for (const auto& dir : options.include_dirs)
                        {
                            where.emplace_back(dir);
                        }

                        for (const auto& dir : where)
                        {
                            const auto file{dir / std::string{name}};

                            std::ifstream in{file, std::ios::binary};

                            if (in)
                            {
                                return Included{
                                        .text =
                                                std::string{
                                                        std::istreambuf_iterator<char>{in},
                                                        std::istreambuf_iterator<char>{}},
                                        .path = file.string()};
                            }
                        }

                        return std::nullopt;
                    }};

            scanners = kind == Kind::flex  ? read_flex(source, options.returning, includes) :
                       kind == Kind::antlr ? read_antlr(source) :
                       kind == Kind::logos ? read_logos(source) :
                                             read_re2c(source, options.re2c_flags, options.returning, includes);
        }
        catch (const Spec_error& error)
        {
            refused = error.what();

            every = false;
        }

        // A scanner with no rule at all tokenizes nothing, so it certifies nothing and an empty report would say
        // otherwise: an empty flex rules section under `%option nodefault`, and an enum deriving Logos with no
        // variant, are files of that shape, and one such scanner beside others is refused with its line, since the
        // report would pass it over in silence.
        if (const auto empty{std::ranges::find_if(scanners, [](const Lexer_spec& spec) { return spec.rules.empty(); })};
            refused.empty() && empty != scanners.end())
        {
            refused = std::format("the scanner at line {} has no rule, so it tokenizes nothing", empty->line);

            every = false;
        }

        // A file the reading finds no scanner in audits nothing, which is no success.
        if (scanners.empty() && refused.empty())
        {
            refused = kind == Kind::flex  ? "the file declares no scanner: no rules section" :
                      kind == Kind::antlr ? "the file declares no scanner: no lexer rule" :
                      kind == Kind::logos ? "the file declares no scanner: no enum deriving Logos" :
                                            "the file declares no scanner: no re2c block with rules";

            every = false;
        }

        if (options.json)
        {
            out << std::format(
                    R"(    {{"path": {}, "kind": "{}", )", json_string(path),
                    kind == Kind::flex  ? "flex" :
                    kind == Kind::antlr ? "antlr" :
                    kind == Kind::logos ? "logos" :
                                          "re2c");

            if (!refused.empty())
            {
                out << std::format(R"("refused": {}, "scanners": []}})", json_string(refused));
            }
            else
            {
                out << "\"refused\": null, \"scanners\": [\n";
            }
        }
        else if (!refused.empty())
        {
            out << std::format("== {}\nrefused: {}\n\n", path, refused);
        }
        else
        {
            out << std::format(
                    "== {}\na certificate holds while the scanner is in its start condition, so a cut needs the "
                    "condition known\n\n",
                    path);
        }

        // A refused file's conditions are still the file's, so a --condition naming one of them named something,
        // whatever the refusal was.
        if (!refused.empty())
        {
            for (const auto& spec : scanners)
            {
                for (const auto& name : conditions_of(spec, options))
                {
                    unmatched.erase(name);
                }

                note_empty(spec);
            }
        }

        // A refused file has written its refusal and closed its object, so its scanners are not serialised after it.
        for (std::size_t which{0}; which < (refused.empty() ? scanners.size() : 0); ++which)
        {
            const auto& spec{scanners[which]};

            const auto conditions{conditions_of(spec, options)};

            for (const auto& name : conditions)
            {
                unmatched.erase(name);
            }

            note_empty(spec);

            if (options.json)
            {
                out << std::format("      {{\"line\": {}, ", spec.line);

                write_rules(out, spec);

                out << ", \"conditions\": [\n";
            }

            for (std::size_t slot{0}; slot < conditions.size(); ++slot)
            {
                const auto outcome{audit_condition(spec, conditions[slot], options, input)};

                every = every && outcome.report.has_value();

                if (options.json)
                {
                    write_json(out, spec, outcome, options.input);

                    out << (slot + 1 < conditions.size() ? ",\n" : "\n");
                }
                else
                {
                    write_text(out, spec, outcome, options.input);
                }
            }

            if (options.json)
            {
                out << (which + 1 < scanners.size() ? "      ]},\n" : "      ]}\n");
            }
        }

        if (options.json && refused.empty())
        {
            out << "    ]}";
        }

        if (options.json)
        {
            out << (index + 1 < options.files.size() ? ",\n" : "\n");
        }
    }

    if (options.json)
    {
        out << "  ]\n}\n";
    }

    if (!unmatched.empty())
    {
        const auto& name{*unmatched.begin()};

        throw std::invalid_argument{
                empty.contains(name) ?
                        std::format(
                                "--condition {} names a condition no rule stands in, so there is no token set to "
                                "audit under it",
                                name) :
                        std::format(
                                "--condition {} names a condition no scanner of the files has, so nothing was "
                                "audited under it",
                                name)};
    }

    return every;
}

} // namespace

} // namespace munch::tools::audit

int main(const int argc, char** argv)
{
    using namespace munch::tools::audit;

    const std::vector<std::string_view> arguments(argv + 1, argv + argc);

    try
    {
        const auto options{parse_options(arguments)};

        // Written whole once every file is audited, so that an error leaves no part of a document behind.
        std::ostringstream out;

        const auto every{run(options, out)};

        std::cout << out.str();

        return every ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    catch (const std::invalid_argument& error)
    {
        std::cerr << "munch-audit: " << error.what() << "\n\n" << usage;

        return 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << "munch-audit: " << error.what() << '\n';

        return 2;
    }
}
