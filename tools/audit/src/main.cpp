#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/command_line.hpp"
#include "munch/tools/audit/directives.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/file_kind.hpp"
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
// Implements the munch-audit command: each file read by its reader, each condition audited, and the report written as
// it goes, are private to this unit.

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
 * @brief What a run found besides the report it wrote: whether every scanner and condition audited, and which
 *        requirements the audited ones did not meet.
 */
struct Findings
{
    /**
     * @brief Whether every scanner and condition audited.
     */
    bool every{true};

    /**
     * @brief One line per requirement an audited condition did not meet, naming the file, the scanner, the condition
     *        and the byte, in the order the report reaches them.
     */
    std::vector<std::string> unmet;
};

/**
 * @brief What reading one file found: its scanners, and why the file was refused when it was.
 */
struct Reading
{
    /**
     * @brief The scanners the reading found, none when the reader refused the file.
     */
    std::vector<Lexer_spec> scanners;

    /**
     * @brief Why the file was refused, empty when it was not.
     */
    std::string refused;
};

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
        // The cut falls before a code point's first byte, or at the start where none of the first 58 bytes begins a
        // code point, so a multibyte name keeps whole characters.
        auto cut{57UZ};

        while (cut > 0 && is_continuation(static_cast<unsigned char>(name[cut])))
        {
            --cut;
        }

        name = name.substr(0, cut) + "...";
    }

    return name;
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
        if (!std::ranges::contains(names, name))
        {
            names.push_back(name);
        }
    }

    std::erase_if(names, [&](const std::string& name) {
        const auto asked{options.conditions.empty() || std::ranges::contains(options.conditions, name)};

        return !asked || active_rules(spec, name).empty();
    });

    return names;
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
 * @brief Writes one condition's outcome as text.
 * @param out The stream.
 * @param spec The scanner.
 * @param outcome The outcome.
 * @param input The input the supply was measured on, empty when none was given.
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
 * @brief A file a scanner's code includes, looked for as a compiler resolves it: a quoted name beside the file
 *        including it and then on the include path, an angle-bracket name on the include path alone. A quoted one not
 *        found is refused by the reader, an angle-bracket one not found taken for a system header's.
 * @param name The name the include gives.
 * @param from The path of the file holding the include, empty when it is the file named on the command line.
 * @param form How the include spells the name.
 * @param path The path of the file named on the command line.
 * @param include_dirs The include path, in order.
 * @return The file's text and path, none when it is found nowhere.
 */
[[nodiscard]] std::optional<Included> find_include(
        const std::string_view name, const std::string_view from, const Include_form form, const std::string& path,
        const std::vector<std::string>& include_dirs)
{
    std::vector<std::filesystem::path> where;

    if (form == Include_form::quoted)
    {
        const auto including{from.empty() ? std::filesystem::path{path} : std::filesystem::path{from}};

        where.push_back(including.parent_path());
    }

    for (const auto& dir : include_dirs)
    {
        where.emplace_back(dir);
    }

    for (const auto& dir : where)
    {
        const auto file{dir / std::string{name}};

        std::ifstream in{file, std::ios::binary};

        if (!in)
        {
            continue;
        }

        return Included{
                .text = std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}},
                .path = file.string()};
    }

    return std::nullopt;
}

/**
 * @brief Audits one scanner's conditions and writes its part of the report, checking every requirement against each
 *        condition's report.
 * @param out The stream.
 * @param path The path of the file the scanner is in.
 * @param spec The scanner.
 * @param options The command line.
 * @param input The input the supply is measured on, none when none was given.
 * @param last Whether the scanner is its file's last, whose JSON object no comma follows.
 * @return Whether every condition audited, and the requirements the audited ones did not meet.
 */
[[nodiscard]] Findings audit_scanner(
        std::ostream& out, const std::string& path, const Lexer_spec& spec, const Options& options,
        const std::optional<std::string>& input, const bool last)
{
    const auto conditions{conditions_of(spec, options)};

    Findings findings;

    if (options.json)
    {
        out << std::format("      {{\"line\": {}, ", spec.line);

        write_rules(out, spec);

        out << ", \"conditions\": [\n";
    }

    for (std::size_t slot{0}; slot < conditions.size(); ++slot)
    {
        const auto outcome{audit_condition(spec, conditions[slot], options, input)};

        findings.every = findings.every && outcome.report.has_value();

        // A requirement is checked where the report stands; a refused condition's refusal is its answer.
        for (const auto& [byte, modulo, text] : options.required)
        {
            if (!outcome.report ||
                std::ranges::binary_search(modulo ? outcome.report->modulo : outcome.report->exact, byte))
            {
                continue;
            }

            findings.unmet.push_back(std::format(
                    "{}, scanner at line {}, condition {}: '{}' is not certified{}", path, spec.line, conditions[slot],
                    text, modulo ? " modulo discarded" : ""));
        }

        if (!options.json)
        {
            write_text(out, spec, outcome, options.input);

            continue;
        }

        write_json(out, spec, outcome, options.input);

        out << (slot + 1 < conditions.size() ? ",\n" : "\n");
    }

    if (options.json)
    {
        out << (last ? "      ]}\n" : "      ]},\n");
    }

    return findings;
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
 * @brief Reads one file's scanners with its includes resolved, and refuses a file the reading finds no scanner in or a
 *        scanner with no rule in. A scanner with no rule at all tokenizes nothing, so it certifies nothing and an empty
 *        report would say otherwise: an empty flex rules section under `%option nodefault`, and an enum deriving Logos
 *        with no variant, are files of that shape, and one such scanner beside others is refused with its line, since
 *        the report would pass it over in silence.
 * @param path The file's path.
 * @param source The file's text.
 * @param kind The kind the file is read as.
 * @param options The command line.
 * @return The scanners, and why the file was refused, empty when it was not.
 */
[[nodiscard]] Reading read_scanners(
        const std::string& path, const std::string_view source, const Kind kind, const Options& options)
{
    const Include_reader_t includes{
            [&path, &options](const std::string_view name, const std::string_view from, const Include_form form) {
                return find_include(name, from, form, path, options.include_dirs);
            }};

    std::vector<Lexer_spec> scanners;

    try
    {
        scanners = kind == Kind::flex  ? read_flex(source, options.returning, includes, options.flex_case_insensitive) :
                   kind == Kind::antlr ? read_antlr(source) :
                   kind == Kind::logos ? read_logos(source) :
                                         read_re2c(source, options.re2c_flags, options.returning, includes);
    }
    catch (const Spec_error& error)
    {
        return Reading{.scanners = {}, .refused = error.what()};
    }

    if (const auto empty{std::ranges::find_if(scanners, [](const Lexer_spec& spec) { return spec.rules.empty(); })};
        empty != scanners.end())
    {
        auto refused{std::format("the scanner at line {} has no rule, so it tokenizes nothing", empty->line)};

        return Reading{.scanners = std::move(scanners), .refused = std::move(refused)};
    }

    // A file the reading finds no scanner in audits nothing, which is no success.
    if (scanners.empty())
    {
        return Reading{
                .scanners = {},
                .refused = kind == Kind::flex  ? "the file declares no scanner: no rules section" :
                           kind == Kind::antlr ? "the file declares no scanner: no lexer rule" :
                           kind == Kind::logos ? "the file declares no scanner: no enum deriving Logos" :
                                                 "the file declares no scanner: no re2c block with rules"};
    }

    return Reading{.scanners = std::move(scanners), .refused = {}};
}

/**
 * @brief The conditions the command line asks for that a scanner has with no rule standing in them, which name no
 *        token set to audit.
 * @param spec The scanner.
 * @param kind The kind its file was read as.
 * @param options The command line.
 * @return The names, in the order the command line gives them.
 */
[[nodiscard]] std::vector<std::string> ruleless_conditions(
        const Lexer_spec& spec, const Kind kind, const Options& options)
{
    const auto audited{conditions_of(spec, options)};

    // INITIAL is every scanner's default condition but a re2c scanner's whose rules name conditions, which has none.
    const auto initial{
            kind != Kind::re2c ||
            std::ranges::any_of(spec.rules, [](const Lexer_spec::Rule& rule) { return rule.conditions.empty(); }) ||
            std::ranges::any_of(spec.conditions, [](const Lexer_spec::Condition& condition) {
                return condition.name == "INITIAL";
            })};

    std::vector<std::string> ruleless;

    for (const auto& name : options.conditions)
    {
        const auto has{
                (name == "INITIAL" && initial) || std::ranges::any_of(spec.conditions, [&name](const auto& condition) {
                    return condition.name == name;
                })};

        if (has && !std::ranges::contains(audited, name))
        {
            ruleless.push_back(name);
        }
    }

    return ruleless;
}

/**
 * @brief Audits one file's scanners and writes its part of the report: its path, then its refusal when it was refused
 *        and its scanners when it was not.
 * @param out The stream.
 * @param path The file's path.
 * @param kind The kind the file was read as.
 * @param reading The file's scanners, and why it was refused, empty when it was not.
 * @param options The command line.
 * @param input The input the supply is measured on, none when none was given.
 * @param last Whether the file is the last on the command line, whose JSON object no comma follows.
 * @return Whether every scanner and condition audited, false for a refused file, and the requirements the audited
 *         ones did not meet.
 */
[[nodiscard]] Findings audit_file(
        std::ostream& out, const std::string& path, const Kind kind, const Reading& reading, const Options& options,
        const std::optional<std::string>& input, const bool last)
{
    const auto& [scanners, refused]{reading};

    if (!options.json)
    {
        out << (refused.empty() ?
                        std::format(
                                "== {}\na certificate holds while the scanner is in its start condition, so a cut "
                                "needs the condition known\n\n",
                                path) :
                        std::format("== {}\nrefused: {}\n\n", path, refused));
    }
    else
    {
        out << std::format(
                R"(    {{"path": {}, "kind": "{}", "refused": {}, "scanners": [)", json_string(path),
                kind == Kind::flex  ? "flex" :
                kind == Kind::antlr ? "antlr" :
                kind == Kind::logos ? "logos" :
                                      "re2c",
                refused.empty() ? std::string{"null"} : json_string(refused));
    }

    const auto separator{last ? "\n" : ",\n"};

    // A refused file's scanners are not audited, so its list stays empty.
    if (!refused.empty())
    {
        if (options.json)
        {
            out << "]}" << separator;
        }

        return Findings{.every = false, .unmet = {}};
    }

    if (options.json)
    {
        out << '\n';
    }

    Findings findings;

    for (std::size_t which{0}; which < scanners.size(); ++which)
    {
        const auto [every, unmet]{
                audit_scanner(out, path, scanners[which], options, input, which + 1 == scanners.size())};

        findings.every = findings.every && every;

        findings.unmet.insert(findings.unmet.end(), unmet.begin(), unmet.end());
    }

    if (options.json)
    {
        out << "    ]}" << separator;
    }

    return findings;
}

/**
 * @brief Audits every file, writing as it goes.
 * @param options The command line.
 * @param out The stream.
 * @return Whether every scanner and condition audited, and the requirements the audited ones did not meet.
 * @throws std::runtime_error If a file or the input cannot be read.
 * @throws std::invalid_argument If a --condition names a condition no scanner of the files has, or one no rule stands
 *         in, and no file was refused.
 */
[[nodiscard]] Findings run(const Options& options, std::ostream& out)
{
    Findings findings;

    // The conditions asked for that no scanner of any file has, which is a command line naming nothing, and among them
    // the ones some scanner has with no rule in them, which is a command line naming no token set.
    std::set<std::string> unmatched(options.conditions.begin(), options.conditions.end());

    std::set<std::string> empty;

    // Whether some file was refused: its refusal is the answer then, and a --condition it left unmatched is not a
    // second one.
    auto any_refused{false};

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

        const auto reading{read_scanners(path, source, kind, options)};

        any_refused = any_refused || !reading.refused.empty();

        for (const auto& spec : reading.scanners)
        {
            // A refused file's conditions are still the file's, so a --condition naming one of them named something,
            // whatever the refusal was.
            for (const auto& name : conditions_of(spec, options))
            {
                unmatched.erase(name);
            }

            for (const auto& name : ruleless_conditions(spec, kind, options))
            {
                empty.insert(name);
            }
        }

        const auto [every, unmet]{
                audit_file(out, path, kind, reading, options, input, index + 1 == options.files.size())};

        findings.every = findings.every && every;

        findings.unmet.insert(findings.unmet.end(), unmet.begin(), unmet.end());
    }

    if (options.json)
    {
        out << "  ]\n}\n";
    }

    if (unmatched.empty() || any_refused)
    {
        return findings;
    }

    const auto& name{*unmatched.begin()};

    throw std::invalid_argument{std::format(
            "--condition {} names a condition {}", name,
            empty.contains(name) ? "no rule stands in, so there is no token set to audit under it" :
                                   "no scanner of the files has, so nothing was audited under it")};
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

        const auto [every, unmet]{run(options, out)};

        std::cout << out.str();

        // The unmet requirements after the report, one line each; a refusal outranks them, since a refused condition's
        // requirements could not be checked.
        for (const auto& line : unmet)
        {
            std::cerr << "munch-audit: " << line << '\n';
        }

        return !every ? EXIT_FAILURE : unmet.empty() ? EXIT_SUCCESS : 3;
    }
    catch (const std::invalid_argument& error)
    {
        std::cerr << "munch-audit: " << error.what() << "\n\n" << usage();

        return 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << "munch-audit: " << error.what() << '\n';

        return 2;
    }
}
