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
#include <ranges>
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
/**
 * @brief The longest token a rule is named by in the report; a longer one is named by its pattern.
 */
constexpr std::size_t longest_token_label{40};

/**
 * @brief The widest rule name a row holds; a wider one is cut and ends with the ellipsis.
 */
constexpr std::size_t widest_label{60};

/**
 * @brief What ends a rule name cut to fit a row.
 */
constexpr std::string_view ellipsis{"..."};

/**
 * @brief The indent a condition's object and the report's own lines take where they sit in the JSON document.
 */
constexpr std::string_view report_indent{"        "};

/**
 * @brief The indent a scanner's own lines take in the JSON document.
 */
constexpr std::string_view scanner_indent{"      "};

/**
 * @brief The exit status when every condition audited and one did not certify a byte required of it.
 */
constexpr int exit_unmet_requirement{3};

/**
 * @brief The exit status on a command-line error, or on a file or an input the command cannot read.
 */
constexpr int exit_command_error{2};

/**
 * @brief What every diagnostic the command writes to the standard error opens with.
 */
constexpr std::string_view diagnostic_prefix{"munch-audit: "};

/**
 * @brief One start condition's outcome: its report, or why it was refused.
 */
struct Condition_outcome
{
    /**
     * @brief The condition's name.
     */
    std::string condition{};

    /**
     * @brief Why the token set could not be built, empty when it was.
     */
    std::string refused{};

    /**
     * @brief The report, when the token set was built.
     */
    std::optional<Report> report{};

    /**
     * @brief The report's certified-anchor supply on the input, when one was given and the report built.
     */
    std::optional<Supply> supply{};

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
    std::vector<std::string> unmet{};
};

/**
 * @brief What reading one file found: its scanners, and why the file was refused when it was.
 */
struct Reading
{
    /**
     * @brief The scanners the reading found, none when the reader refused the file.
     */
    std::vector<Lexer_spec> scanners{};

    /**
     * @brief Why the file was refused, empty when it was not.
     */
    std::string refused{};
};

/**
 * @brief Returns the name the report prints for a rule: what it returns when that is short, else its pattern, either
 *        cut to a width a row can hold.
 * @param spec The scanner.
 * @param rule The rule's index.
 * @return The name.
 */
[[nodiscard]] std::string label(const Lexer_spec& spec, const std::size_t rule)
{
    const auto& [pattern, expression, conditions, action, token, priority, line]{spec.rules[rule]};

    auto name{token && token->size() <= longest_token_label ? *token : pattern};

    if (name.size() > widest_label)
    {
        // The cut falls before a code point's first byte, or at the start where none of the bytes before the cut begins
        // a code point, so a multibyte name keeps whole characters.
        auto cut{widest_label - ellipsis.size()};

        while (cut > 0 && is_continuation(static_cast<unsigned char>(name[cut])))
        {
            --cut;
        }

        name = std::format("{}{}", name.substr(0, cut), ellipsis);
    }

    return name;
}

/**
 * @brief Returns the naming the report prints a scanner's rules by.
 * @param spec The scanner.
 * @return What names a rule by its index, as label() does.
 */
[[nodiscard]] auto rule_namer(const Lexer_spec& spec)
{
    return [&spec](const std::size_t rule) { return label(spec, rule); };
}

/**
 * @brief Returns the start conditions of a scanner the command line asks for, in the order to report them: INITIAL
 *        first, then the declared ones, each only when it has rules.
 * @param spec The scanner.
 * @param options The command line.
 * @return The names.
 */
[[nodiscard]] std::vector<std::string> conditions_of(const Lexer_spec& spec, const Options& options)
{
    std::vector<std::string> names{std::string{initial_condition}};

    for (const auto& [name, exclusive] : spec.conditions)
    {
        if (!std::ranges::contains(names, name))
        {
            names.push_back(name);
        }
    }

    const auto left_out{[&](const std::string& name) {
        const auto asked{options.conditions.empty() || std::ranges::contains(options.conditions, name)};

        return !asked || active_rules(spec, name).empty();
    }};

    std::erase_if(names, left_out);

    return names;
}

/**
 * @brief Writes a scanner's options, definitions and rules as the JSON of what the reader read: the options that
 *        governed the reading, the named patterns, then each rule's line, pattern as written, start conditions, action
 *        and token, so that a reading can be held to the generator's own account of the file.
 * @param out The stream.
 * @param spec The scanner.
 */
void write_rules(std::ostream& out, const Lexer_spec& spec)
{
    const auto definition_json{[]<typename Definition>(const Definition& definition) {
        const auto& [name, body]{definition};

        return std::format("{}: {}", json_string(name), json_string(body));
    }};

    const auto comma_joined{[]<typename Texts>(Texts&& texts) {
        std::string joined{};

        std::ranges::copy(
                std::forward<Texts>(texts) | std::views::join_with(std::string_view{", "}), std::back_inserter(joined));

        return joined;
    }};

    const auto definitions{comma_joined(spec.definitions | std::views::transform(definition_json))};

    out << R"("options": )" << options_json(spec.options) << R"(, "definitions": {)" << definitions;

    out << R"(}, "rules": [)";

    for (const auto [index, rule] : std::views::enumerate(spec.rules))
    {
        const auto& [pattern, expression, conditions, action, token, priority, line]{rule};

        const auto named{comma_joined(conditions | std::views::transform(json_string))};

        const std::string_view separator{index == 0 ? "" : ","};

        const auto lead{std::format("{}\n{}", separator, report_indent)};

        const auto token_json{token ? json_string(*token) : "null"};

        const auto record{std::format(
                R"({{"line": {}, "pattern": {}, "conditions": [{}], "action": {}, "token": {}}})", line,
                json_string(pattern), named, json_string(action), token_json)};

        out << lead << record;
    }

    const auto close{spec.rules.empty() ? std::string{"]"} : std::format("\n{}]", scanner_indent)};

    out << close;
}

/**
 * @brief Audits one start condition of a scanner, and measures the report's supply on the input when one was given.
 * @param spec The scanner.
 * @param condition The condition's name.
 * @param options The command line.
 * @param input The input the supply is measured on, none when none was given.
 * @return The outcome.
 */
[[nodiscard]] Condition_outcome audit_condition(
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
            const auto priced{std::ranges::contains(report.prices, byte, &Pricing::byte)};

            if (!priced && !std::ranges::binary_search(report.exact, byte))
            {
                report.prices.push_back(price(set, byte));
            }
        }

        std::optional<Supply> measured{};

        if (input)
        {
            const auto lexer{compile(set)};

            measured = supply(report, lexer, *input);
        }

        return Condition_outcome{
                .condition = condition,
                .refused = {},
                .report = std::move(report),
                .supply = std::move(measured),
                .rules = rules};
    }
    catch (const Spec_error& error)
    {
        return Condition_outcome{
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
void write_text(
        std::ostream& out, const Lexer_spec& spec, const Condition_outcome& outcome, const std::string_view input)
{
    const auto& [condition, refused, report, supply, rules]{outcome};

    out << std::format("-- scanner at line {}, condition {}: {} rule{}\n", spec.line, condition, rules, plural(rules));

    // What the reading was governed by, before the figures it governed.
    out << options_row(spec.options);

    if (!report)
    {
        out << std::format("refused: {}\n\n", refused);

        return;
    }

    out << render(*report, rule_namer(spec));

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
void write_json(
        std::ostream& out, const Lexer_spec& spec, const Condition_outcome& outcome, const std::string_view input)
{
    const auto& [condition, refused, report, supply, rules]{outcome};

    out << std::format(R"({}{{"name": {}, "rules": {}, )", report_indent, json_string(condition), rules);

    if (!report)
    {
        out << std::format(R"("refused": {}, "report": null}})", json_string(refused));

        return;
    }

    std::string document{json(*report, rule_namer(spec))};

    // The report's own lines, indented to where they sit in the document, each search resuming past the indent.
    for (auto at{document.find('\n')}; at != std::string::npos; at = document.find('\n', at + 1 + report_indent.size()))
    {
        document.insert(at + 1, report_indent);
    }

    out << R"("refused": null, "report": )" << document;

    if (supply)
    {
        out << R"(, "supply": )" << supply_json(*supply, input);
    }

    out << '}';
}

/**
 * @brief Writes the opening of a scanner's part of the JSON document, its rules as read, up to its condition list; the
 *        text report has no such opening.
 * @param out The stream.
 * @param spec The scanner.
 */
void write_scanner_head(std::ostream& out, const Lexer_spec& spec)
{
    out << std::format(R"({}{{"line": {}, )", scanner_indent, spec.line);

    write_rules(out, spec);

    out << ", \"conditions\": [\n";
}

/**
 * @brief Writes the close of a scanner's part of the JSON document, a comma after it unless it is its file's last; the
 *        text report has no such close.
 * @param out The stream.
 * @param last Whether the scanner is its file's last.
 */
void write_scanner_tail(std::ostream& out, const bool last)
{
    const std::string_view separator{last ? "" : ","};

    const auto close{std::format("{}]}}{}\n", scanner_indent, separator)};

    out << close;
}

/**
 * @brief Returns the whole of a file, when it can be opened.
 * @param path The file's path.
 * @return Its text, or std::nullopt when it cannot be opened.
 */
[[nodiscard]] std::optional<std::string> file_text(const std::filesystem::path& path)
{
    std::ifstream stream{path, std::ios::binary};

    if (!stream)
    {
        return std::nullopt;
    }

    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

/**
 * @brief Returns a file a scanner's code includes, looked for as a compiler resolves it: a quoted name beside the file
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
    std::vector<std::filesystem::path> where{};

    if (form == Include_form::quoted)
    {
        const auto including{from.empty() ? std::filesystem::path{path} : std::filesystem::path{from}};

        where.push_back(including.parent_path());
    }

    where.insert(where.end(), include_dirs.begin(), include_dirs.end());

    for (const auto& dir : where)
    {
        const auto file{dir / std::string{name}};

        auto text{file_text(file)};

        if (!text)
        {
            continue;
        }

        return Included{.text = std::move(*text), .path = file.string()};
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

    Findings findings{};

    if (options.json)
    {
        write_scanner_head(out, spec);
    }

    const auto check_requirements{[&](const std::string& condition, const Report& report) {
        for (const auto& [byte, modulo, text] : options.required)
        {
            const auto& certified{modulo ? report.modulo : report.exact};

            if (std::ranges::binary_search(certified, byte))
            {
                continue;
            }

            const std::string_view kind{modulo ? " modulo discarded" : ""};

            findings.unmet.push_back(std::format(
                    "{}, scanner at line {}, condition {}: '{}' is not certified{}", path, spec.line, condition, text,
                    kind));
        }
    }};

    for (const auto [slot, condition] : std::views::enumerate(conditions))
    {
        const auto outcome{audit_condition(spec, condition, options, input)};

        findings.every = findings.every && outcome.report;

        // A requirement is checked where the report stands.
        if (outcome.report)
        {
            check_requirements(condition, *outcome.report);
        }

        if (!options.json)
        {
            write_text(out, spec, outcome, options.input);

            continue;
        }

        write_json(out, spec, outcome, options.input);

        const auto more{static_cast<std::size_t>(slot) + 1 < conditions.size()};

        const std::string_view after{more ? ",\n" : "\n"};

        out << after;
    }

    if (options.json)
    {
        write_scanner_tail(out, last);
    }

    return findings;
}

/**
 * @brief Returns the whole of a file.
 * @param path The file's path.
 * @return Its text.
 * @throws std::runtime_error If it cannot be opened.
 */
[[nodiscard]] std::string contents(const std::string& path)
{
    auto text{file_text(path)};

    if (!text)
    {
        throw std::runtime_error{std::format("cannot read {}", path)};
    }

    return std::move(*text);
}

/**
 * @brief Returns a kind of file's name, as the JSON spells it.
 * @param kind The kind.
 * @return The name.
 */
[[nodiscard]] std::string_view kind_name(const Kind kind) noexcept
{
    switch (kind)
    {
    case Kind::flex:
        return "flex";
    case Kind::antlr:
        return "antlr";
    case Kind::logos:
        return "logos";
    case Kind::re2c:
        break;
    }

    return "re2c";
}

/**
 * @brief Returns what declares a scanner in a kind of file.
 * @param kind The kind.
 * @return The words, as a refusal of a file declaring none names it.
 */
[[nodiscard]] std::string_view scanner_declaration(const Kind kind) noexcept
{
    switch (kind)
    {
    case Kind::flex:
        return "rules section";
    case Kind::antlr:
        return "lexer rule";
    case Kind::logos:
        return "enum deriving Logos";
    case Kind::re2c:
        break;
    }

    return "re2c block with rules";
}

/**
 * @brief Adds what auditing more of the files found to what was found so far.
 * @param findings What was found so far.
 * @param more What the further auditing found.
 */
void absorb(Findings& findings, const Findings& more)
{
    const auto& [every, unmet]{more};

    findings.every = findings.every && every;

    findings.unmet.insert(findings.unmet.end(), unmet.begin(), unmet.end());
}

/**
 * @brief Writes the opening of a file's part of the text report: its heading, and its refusal when it was refused.
 * @param out The stream.
 * @param path The file's path.
 * @param refused Why the file was refused, empty when it was not.
 */
void write_file_head_text(std::ostream& out, const std::string& path, const std::string& refused)
{
    if (!refused.empty())
    {
        out << std::format("== {}\nrefused: {}\n\n", path, refused);

        return;
    }

    out << std::format(
            "== {}\na certificate holds while the scanner is in its start condition, so a cut needs the condition "
            "known\n\n",
            path);
}

/**
 * @brief Writes the opening of a file's part of the JSON document: its object up to its scanner list, closed at once
 *        for a refused file, whose list stays empty.
 * @param out The stream.
 * @param path The file's path.
 * @param kind The kind the file was read as.
 * @param refused Why the file was refused, empty when it was not.
 * @param last Whether the file is the last on the command line, whose JSON object no comma follows.
 */
void write_file_head_json(
        std::ostream& out, const std::string& path, const Kind kind, const std::string& refused, const bool last)
{
    const auto refused_json{refused.empty() ? std::string{"null"} : json_string(refused)};

    out << std::format(
            R"(    {{"path": {}, "kind": "{}", "refused": {}, "scanners": [)", json_string(path), kind_name(kind),
            refused_json);

    if (!refused.empty())
    {
        const std::string_view separator{last ? "\n" : ",\n"};

        out << "]}" << separator;

        return;
    }

    out << '\n';
}

/**
 * @brief Writes the close of an audited file's part of the JSON document, a comma after it unless it is the last; the
 *        text report has no such close.
 * @param out The stream.
 * @param last Whether the file is the last on the command line.
 */
void write_file_tail(std::ostream& out, const bool last)
{
    const std::string_view separator{last ? "\n" : ",\n"};

    out << "    ]}" << separator;
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

    const auto read{[&] {
        switch (kind)
        {
        case Kind::flex:
            return read_flex(source, options.returning, includes, options.flex_case_insensitive);
        case Kind::antlr:
            return read_antlr(source);
        case Kind::logos:
            return read_logos(source);
        case Kind::re2c:
            break;
        }

        return read_re2c(source, options.re2c_flags, options.returning, includes);
    }};

    std::vector<Lexer_spec> scanners{};

    try
    {
        scanners = read();
    }
    catch (const Spec_error& error)
    {
        return Reading{.scanners = {}, .refused = error.what()};
    }

    const auto has_no_rule{[](const Lexer_spec& spec) { return spec.rules.empty(); }};

    const auto ruleless{std::ranges::find_if(scanners, has_no_rule)};

    if (ruleless != scanners.end())
    {
        auto refused{std::format("the scanner at line {} has no rule, so it tokenizes nothing", ruleless->line)};

        return Reading{.scanners = std::move(scanners), .refused = std::move(refused)};
    }

    // A file the reading finds no scanner in audits nothing, which is no success.
    if (scanners.empty())
    {
        const auto missing{std::format("the file declares no scanner: no {}", scanner_declaration(kind))};

        return Reading{.scanners = {}, .refused = missing};
    }

    return Reading{.scanners = std::move(scanners), .refused = {}};
}

/**
 * @brief Returns the conditions the command line asks for that a scanner has with no rule standing in them, which name
 *        no token set to audit.
 * @param spec The scanner.
 * @param kind The kind its file was read as.
 * @param options The command line.
 * @return The names, in the order the command line gives them.
 */
[[nodiscard]] std::vector<std::string> ruleless_conditions(
        const Lexer_spec& spec, const Kind kind, const Options& options)
{
    const auto audited{conditions_of(spec, options)};

    // A rule naming no condition stands in INITIAL.
    const auto unconditioned{[](const Lexer_spec::Rule& rule) { return rule.conditions.empty(); }};

    // INITIAL is every scanner's default condition but a re2c scanner's whose rules name conditions, which has none.
    const auto initial{
            kind != Kind::re2c || std::ranges::any_of(spec.rules, unconditioned) ||
            std::ranges::contains(spec.conditions, initial_condition, &Lexer_spec::Condition::name)};

    std::vector<std::string> ruleless{};

    for (const auto& name : options.conditions)
    {
        const auto has{
                (name == initial_condition && initial) ||
                std::ranges::contains(spec.conditions, name, &Lexer_spec::Condition::name)};

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
 * @return Whether every scanner and condition audited, false for a refused file, and the requirements the audited ones
 *         did not meet.
 */
[[nodiscard]] Findings audit_file(
        std::ostream& out, const std::string& path, const Kind kind, const Reading& reading, const Options& options,
        const std::optional<std::string>& input, const bool last)
{
    const auto& [scanners, refused]{reading};

    if (options.json)
    {
        write_file_head_json(out, path, kind, refused, last);
    }
    else
    {
        write_file_head_text(out, path, refused);
    }

    // A refused file's scanners are not audited.
    if (!refused.empty())
    {
        return Findings{.every = false, .unmet = {}};
    }

    Findings findings{};

    for (const auto [which, spec] : std::views::enumerate(scanners))
    {
        const auto last_scanner{static_cast<std::size_t>(which) + 1 == scanners.size()};

        const auto audited{audit_scanner(out, path, spec, options, input, last_scanner)};

        absorb(findings, audited);
    }

    if (options.json)
    {
        write_file_tail(out, last);
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
    Findings findings{};

    // The conditions asked for that no scanner of any file has, which is a command line naming nothing, and among them
    // the ones some scanner has with no rule in them, which is a command line naming no token set.
    std::set<std::string> unmatched{options.conditions.begin(), options.conditions.end()};

    std::set<std::string> ruleless{};

    // Whether some file was refused: its refusal is the answer then, and a --condition it left unmatched is not a
    // second one.
    auto any_refused{false};

    // An empty input is measured like any other: it holds no anchor, which the supply says.
    std::optional<std::string> input{};

    if (!options.input.empty())
    {
        input = contents(options.input);
    }

    if (options.json)
    {
        out << "{\n  \"files\": [\n";
    }

    for (const auto [index, path] : std::views::enumerate(options.files))
    {
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
                ruleless.insert(name);
            }
        }

        const auto last_file{static_cast<std::size_t>(index) + 1 == options.files.size()};

        const auto audited{audit_file(out, path, kind, reading, options, input, last_file)};

        absorb(findings, audited);
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

    const std::string_view why{
            ruleless.contains(name) ? "no rule stands in, so there is no token set to audit under it" :
                                      "no scanner of the files has, so nothing was audited under it"};

    throw std::invalid_argument{std::format("--condition {} names a condition {}", name, why)};
}

} // namespace

} // namespace munch::tools::audit

int main(const int argc, char** argv)
{
    using namespace munch::tools::audit;

    const std::vector<std::string_view> arguments{argv + 1, argv + argc};

    try
    {
        const auto options{parse_options(arguments)};

        // Written whole once every file is audited, so that an error leaves no part of a document behind.
        std::ostringstream out{};

        const auto [every, unmet]{run(options, out)};

        std::cout << out.str();

        // The unmet requirements after the report, one line each; a refusal outranks them, since a refused condition's
        // requirements could not be checked.
        for (const auto& line : unmet)
        {
            std::cerr << std::format("{}{}\n", diagnostic_prefix, line);
        }

        if (!every)
        {
            return EXIT_FAILURE;
        }

        return unmet.empty() ? EXIT_SUCCESS : exit_unmet_requirement;
    }
    catch (const std::invalid_argument& error)
    {
        std::cerr << std::format("{}{}\n\n{}", diagnostic_prefix, error.what(), usage());

        return exit_command_error;
    }
    catch (const std::exception& error)
    {
        std::cerr << std::format("{}{}\n", diagnostic_prefix, error.what());

        return exit_command_error;
    }
}
