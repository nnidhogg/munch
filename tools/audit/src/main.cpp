#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/price.hpp"
#include "munch/tools/audit/read_antlr.hpp"
#include "munch/tools/audit/read_flex.hpp"
#include "munch/tools/audit/read_re2c.hpp"
#include "munch/tools/audit/report.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The usage text, printed on a command-line error.
 */
constexpr std::string_view usage{R"(usage: munch-audit [options] FILE...

Reads a flex, re2c or ANTLR 4 file and reports, per scanner and start condition, what the token set certifies: the
bytes and windows a parallel scan may cut at, why the other candidates fail, and what certifying one would cost.

  --flex, --re2c, --antlr
                        read every file as this kind; otherwise a file opening a re2c block is re2c, one opening
                        with a grammar declaration is ANTLR, the rest flex
  --flex-syntax         re2c's -F: flex-style definitions, {name} references, bare letters literal
  --case-inverted       re2c's --case-inverted: "..." case-insensitive and '...' exact
  --case-insensitive    re2c's --case-insensitive: both quotes case-insensitive
  --returns NAME        a form besides return an action returns a token through, NAME(x), NAME = x or NAME alone;
                        may repeat
  --condition NAME      audit this start condition only; may repeat
  --windows N           the longest window tried, 3 unless given; 4 is the planners' own limit
  --price BYTE          price this byte as well, written as a character, \n \t \r \0, or 0xHH; may repeat
  --json                one JSON document instead of text

Exit status is 0 when every scanner and condition audited, 1 when one was refused, 2 on a command-line error.
)"};

/**
 * @brief The generator a file was written for, which chooses its reader.
 */
enum class Kind : std::uint8_t
{
    flex,
    re2c,
    antlr
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

    if (text.starts_with("0x") && text.size() <= 4)
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
        else if (argument == "--condition")
        {
            options.conditions.emplace_back(value());
        }
        else if (argument == "--windows")
        {
            const auto text{value()};

            const auto limit{std::atoi(std::string{text}.c_str())};

            if (limit < 1 || limit > 8)
            {
                throw std::invalid_argument{std::format("--windows takes 1 to 8, not '{}'", text)};
            }

            options.window_limit = static_cast<std::size_t>(limit);
        }
        else if (argument == "--price")
        {
            options.priced.push_back(parse_byte(value()));
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
 * @brief The kind a file's text says: re2c when it opens a re2c block, which no other file does; ANTLR when its first
 *        item is a grammar declaration; flex otherwise. The name says nothing, since re2c lives in files of any
 *        extension and PHP's re2c scanners end in `.l`.
 * @param source The file's text.
 * @return The kind.
 */
[[nodiscard]] Kind kind_of(const std::string_view source) noexcept
{
    if (source.contains("/*!re2c") || source.contains("/*!rules:re2c"))
    {
        return Kind::re2c;
    }

    // The first item, comments stepped over.
    auto at{0UZ};

    for (;;)
    {
        at = std::min(source.find_first_not_of(" \t\r\n", at), source.size());

        if (source.substr(at).starts_with("//"))
        {
            at = std::min(source.find('\n', at), source.size());
        }
        else if (source.substr(at).starts_with("/*"))
        {
            at = std::min(source.find("*/", at + 2), source.size() - 2) + 2;
        }
        else
        {
            break;
        }
    }

    const auto head{source.substr(at)};

    return head.starts_with("grammar ") || head.starts_with("lexer grammar ") || head.starts_with("parser grammar ") ?
                   Kind::antlr :
                   Kind::flex;
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
    const auto& [pattern, expression, conditions, action, token, line]{spec.rules[rule]};

    auto name{token && token->size() <= 40 ? *token : pattern};

    if (name.size() > 60)
    {
        name = name.substr(0, 57) + "...";
    }

    return name;
}

/**
 * @brief Audits one start condition of a scanner.
 * @param spec The scanner.
 * @param condition The condition's name.
 * @param options The command line.
 * @return The outcome.
 */
[[nodiscard]] Outcome audit_condition(const Lexer_spec& spec, const std::string& condition, const Options& options)
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

        return Outcome{.condition = condition, .refused = {}, .report = std::move(report), .rules = rules};
    }
    catch (const Spec_error& error)
    {
        return Outcome{.condition = condition, .refused = error.what(), .report = std::nullopt, .rules = rules};
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
void write_text(std::ostream& out, const Lexer_spec& spec, const Outcome& outcome)
{
    const auto& [condition, refused, report, rules]{outcome};

    out << std::format(
            "-- scanner at line {}, condition {}: {} rule{}\n", spec.line, condition, rules, rules == 1 ? "" : "s");

    if (!report)
    {
        out << "refused: " << refused << "\n\n";

        return;
    }

    out << render(*report, [&spec](const std::size_t rule) { return label(spec, rule); }) << '\n';
}

/**
 * @brief Text as a JSON string, the quote, the backslash and the controls escaped.
 * @param text The text.
 * @return The JSON text, quotes included.
 */
[[nodiscard]] std::string json_string(const std::string_view text)
{
    std::string out{'"'};

    for (const auto byte : text)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (byte == '"' || byte == '\\')
        {
            out += std::string{'\\'} + byte;
        }
        else if (value < 0x20)
        {
            out += std::format(R"(\u{:04x})", value);
        }
        else
        {
            out.push_back(byte);
        }
    }

    return out + '"';
}

/**
 * @brief Writes one condition's outcome as the JSON object of a scanner's condition list.
 * @param out The stream.
 * @param spec The scanner.
 * @param outcome The outcome.
 */
void write_json(std::ostream& out, const Lexer_spec& spec, const Outcome& outcome)
{
    const auto& [condition, refused, report, rules]{outcome};

    out << std::format("        {{\"name\": {}, \"rules\": {}, ", json_string(condition), rules);

    if (!report)
    {
        out << std::format("\"refused\": {}, \"report\": null}}", json_string(refused));

        return;
    }

    std::string document{json(*report, [&spec](const std::size_t rule) { return label(spec, rule); })};

    // The report's own lines, indented to where they sit in the document.
    for (auto at{document.find('\n')}; at != std::string::npos; at = document.find('\n', at + 9))
    {
        document.insert(at + 1, "        ");
    }

    out << "\"refused\": null, \"report\": " << document << '}';
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
            scanners = kind == Kind::flex  ? std::vector{read_flex(source, options.returning)} :
                       kind == Kind::antlr ? std::vector{read_antlr(source)} :
                                             read_re2c(source, options.re2c_flags, options.returning);
        }
        catch (const Spec_error& error)
        {
            refused = error.what();

            every = false;
        }

        if (options.json)
        {
            out << std::format(
                    "    {{\"path\": {}, \"kind\": \"{}\", ", json_string(path),
                    kind == Kind::flex  ? "flex" :
                    kind == Kind::antlr ? "antlr" :
                                          "re2c");

            if (!refused.empty())
            {
                out << std::format("\"refused\": {}, \"scanners\": []}}", json_string(refused));
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

        for (std::size_t which{0}; which < scanners.size(); ++which)
        {
            const auto& spec{scanners[which]};

            const auto conditions{conditions_of(spec, options)};

            if (options.json)
            {
                out << std::format("      {{\"line\": {}, \"conditions\": [\n", spec.line);
            }

            for (std::size_t slot{0}; slot < conditions.size(); ++slot)
            {
                const auto outcome{audit_condition(spec, conditions[slot], options)};

                every = every && outcome.report.has_value();

                if (options.json)
                {
                    write_json(out, spec, outcome);

                    out << (slot + 1 < conditions.size() ? ",\n" : "\n");
                }
                else
                {
                    write_text(out, spec, outcome);
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

        return run(options, std::cout) ? EXIT_SUCCESS : EXIT_FAILURE;
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
