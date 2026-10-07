#include "munch/tools/audit/re2c_block.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/tools/audit/c_tokens.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief What a configuration opens with.
 */
constexpr std::string_view configuration_prefix{"re2c:"};

/**
 * @brief What a use directive opens with.
 */
constexpr std::string_view use_prefix{"!use:"};

/**
 * @brief What an include directive opens with.
 */
constexpr std::string_view include_prefix{"!include"};

/**
 * @brief Returns the indices of the default rules, `*`, from an index of the specification's rules on.
 * @param spec The specification.
 * @param first The index the rules looked at begin at.
 * @return The indices, ascending.
 */
[[nodiscard]] std::vector<std::size_t> default_rules_from(const Lexer_spec& spec, const std::size_t first)
{
    std::vector<std::size_t> defaults{};

    for (auto index{first}; index < spec.rules.size(); ++index)
    {
        if (spec.rules[index].pattern == default_rule)
        {
            defaults.push_back(index);
        }
    }

    return defaults;
}

/**
 * @brief Returns where a configuration ends: at its first `;` outside a quoted string, which may hold a `;` of its own,
 *        as a YYFILL definition usually does.
 * @param text The text.
 * @param from The offset of the configuration's first byte.
 * @param end The offset the text is read up to.
 * @return The offset of the `;`, or one at or past the end when none stands.
 */
[[nodiscard]] std::size_t configuration_end(const std::string_view text, const std::size_t from, const std::size_t end)
{
    auto scan{from};

    while (scan < end && text[scan] != ';')
    {
        if (text[scan] == '"' || text[scan] == '\'')
        {
            const auto quote{text[scan]};

            for (++scan; scan < end && text[scan] != quote; ++scan)
            {
                scan += text[scan] == '\\' ? 1 : 0;
            }
        }

        ++scan;
    }

    return scan;
}

/**
 * @brief Appends a name to a list unless the list holds it already.
 * @param names The list.
 * @param name The name.
 */
void add_unique(std::vector<std::string>& names, const std::string& name)
{
    if (!std::ranges::contains(names, name))
    {
        names.push_back(name);
    }
}

/**
 * @brief Returns whether a default rule stands in every condition: it names none, or names `*`.
 * @param rule The default rule.
 * @return True when it does.
 */
[[nodiscard]] bool in_every_condition(const Lexer_spec::Rule& rule)
{
    return rule.conditions.empty() || std::ranges::contains(rule.conditions, every_condition);
}

/**
 * @brief Returns the test of whether a rule stands in a condition, by the condition's name.
 * @param rule The rule, which outlives the test.
 * @return The test, true for a condition the rule names.
 */
[[nodiscard]] auto in_conditions_of(const Lexer_spec::Rule& rule)
{
    const auto in_conditions{[&rule](const std::string& name) { return std::ranges::contains(rule.conditions, name); }};

    return in_conditions;
}

/**
 * @brief Returns whether two default rules stand in one condition to re2c: a default rule in every condition, `<*> *`
 *        or one naming no condition, and one naming conditions are rules of different conditions, so they stand
 *        together, and two naming conditions share one when a name is in both.
 * @param one A default rule.
 * @param other Another.
 * @return True when they share a condition.
 */
[[nodiscard]] bool share_a_condition(const Lexer_spec::Rule& one, const Lexer_spec::Rule& other)
{
    if (in_every_condition(one) || in_every_condition(other))
    {
        return in_every_condition(one) && in_every_condition(other);
    }

    const auto in_other{in_conditions_of(other)};

    return std::ranges::any_of(one.conditions, in_other);
}

/**
 * @brief Refuses a block's second default rule of its own for a condition it already gave one, as re2c 3.1 refuses it.
 * @param spec The specification the block filled.
 * @param own The indices into spec.rules of the block's own default rules, ascending.
 * @throws Spec_error If two of them share a condition, at the second's line.
 */
void refuse_doubled_defaults(const Lexer_spec& spec, const std::vector<std::size_t>& own)
{
    for (auto second{own.begin()}; second != own.end(); ++second)
    {
        const auto shares{[&spec, second](const std::size_t index) {
            return share_a_condition(spec.rules[index], spec.rules[*second]);
        }};

        const auto first{std::ranges::find_if(own.begin(), second, shares)};

        if (first != second)
        {
            const auto message{std::format(
                    "the default rule for this condition is already defined at line {}, which re2c refuses",
                    spec.rules[*first].line)};

            throw Spec_error{message, spec.rules[*second].line};
        }
    }
}

/**
 * @brief Settles a block's default rules as re2c 3.1 settles them once the block is read: a second default rule of the
 *        block's own for a condition it already gave one is refused, as re2c refuses it, and a default rule a `!use:`
 *        directive brought in yields to the block's own in every condition the block's own stands in, since the using
 *        block's default rule overrides the used block's wherever the two stand.
 *
 * A default rule in every condition, `<*> *` or one naming no condition, and one naming conditions are rules of
 * different conditions to re2c, so the two stand together, and a used one of either kind yields only to an own one of
 * the same kind.
 * @param spec The specification the block filled, its rules in the order they were read.
 * @param first The index into spec.rules of the block's first rule, the ones before it being another block's, which
 *        read this one through a `!use:` directive and settles its own once it is read.
 * @param used The indices into spec.rules of the default rules the block's `!use:` directives brought in.
 * @throws Spec_error If the block gives one condition two default rules of its own, at the second's line.
 */
void settle_defaults(Lexer_spec& spec, const std::size_t first, const std::vector<std::size_t>& used)
{
    const auto is_used{[&used](const std::size_t index) { return std::ranges::contains(used, index); }};

    auto own{default_rules_from(spec, first)};

    std::erase_if(own, is_used);

    refuse_doubled_defaults(spec, own);

    // Drops from a used rule naming conditions those an own one takes; it yields once none is left.
    const auto yields_to_own{[&spec, &own](Lexer_spec::Rule& rule) {
        const auto everywhere{in_every_condition(rule)};

        auto yields{false};

        for (const auto own_index : own)
        {
            const auto& other{spec.rules[own_index]};

            if (everywhere != in_every_condition(other))
            {
                continue;
            }

            if (everywhere)
            {
                return true;
            }

            const auto taken{in_conditions_of(other)};

            std::erase_if(rule.conditions, taken);

            yields = rule.conditions.empty();
        }

        return yields;
    }};

    std::vector<std::size_t> yielded{};

    for (const auto index : used)
    {
        if (yields_to_own(spec.rules[index]))
        {
            yielded.push_back(index);
        }
    }

    // The indices ascend as the directives were read, so the rules that go are erased from the back.
    for (const auto index : yielded | std::views::reverse)
    {
        const auto position{spec.rules.begin() + static_cast<std::ptrdiff_t>(index)};

        spec.rules.erase(position);
    }
}

} // namespace

void Rule_kinds::note(const std::vector<std::string>& named, const std::string& pattern, const std::size_t line)
{
    if (!named.empty())
    {
        conditioned_ = true;

        const auto own_condition{[](const std::string& name) { return name != every_condition; }};

        for (const auto& name : named | std::views::filter(own_condition))
        {
            add_unique(named_, name);
        }
    }
    else if (pattern == end_rule && !plain_end_)
    {
        plain_end_ = line;
    }
    else if (pattern != end_rule && !plain_)
    {
        plain_ = line;
    }

    // Which names the rule stands in, for re2c's own checks of the end rule: a rule naming none stands in the empty
    // name, and `<*>` is a name of its own here, since re2c counts an end rule under `<*>` against the other `<*>`
    // rules and not against each condition's.
    const auto names{named.empty() ? std::vector<std::string>{""} : named};

    for (const auto& name : names)
    {
        if (pattern == end_rule)
        {
            ends_.push_back({.name = name, .line = line});

            continue;
        }

        add_unique(ruled_, name);
    }
}

void Rule_kinds::merge(const Rule_kinds& used)
{
    // The used block's rules are of the kinds they are wherever they stand, the first of a kind the using block's own
    // if it read one before the directive.
    conditioned_ = conditioned_ || used.conditioned_;

    for (const auto& name : used.named_)
    {
        add_unique(named_, name);
    }

    for (const auto& name : used.ruled_)
    {
        add_unique(ruled_, name);
    }

    ends_.insert(ends_.end(), used.ends_.begin(), used.ends_.end());

    if (!plain_)
    {
        plain_ = used.plain_;
    }

    if (!plain_end_)
    {
        plain_end_ = used.plain_end_;
    }
}

void Rule_kinds::refuse_mixed() const
{
    if (conditioned_ && plain_)
    {
        throw Spec_error{
                "cannot mix conditions with normal rules, as re2c answers a scanner holding a rule that names a "
                "condition beside one that names none",
                *plain_};
    }

    if (conditioned_ && plain_end_)
    {
        throw Spec_error{
                "EOF rule without other rules doesn't make sense, as re2c answers an end rule naming no condition in a "
                "scanner whose other rules name one",
                *plain_end_};
    }
}

void Rule_kinds::refuse_end_rules(const std::vector<std::string>& options, const std::size_t line) const
{
    if (ruled_.empty() && ends_.empty())
    {
        return;
    }

    for (const auto& [name, at] : ends_)
    {
        if (std::ranges::contains(ruled_, name))
        {
            continue;
        }

        if (name.empty())
        {
            throw Spec_error{"EOF rule without other rules doesn't make sense", at};
        }

        const auto message{std::format("EOF rule in condition '{}' without other rules doesn't make sense", name)};

        throw Spec_error{message, at};
    }

    static constexpr std::string_view eof_key{"eof="};

    const auto sets_eof{[](const std::string& option) { return option.starts_with(eof_key); }};

    // The last setting stands.
    const auto last_eof{std::ranges::find_last_if(options, sets_eof)};

    const auto eof_set{!last_eof.empty() && last_eof.front().substr(eof_key.size()) != "-1"};

    if (!eof_set && !ends_.empty())
    {
        const auto& [name, at]{ends_.front()};

        if (name.empty())
        {
            throw Spec_error{"$ rule found, but 're2c:eof' configuration is not set", at};
        }

        const auto message{
                std::format("in condition '{}' $ rule found, but 're2c:eof' configuration is not set", name)};

        throw Spec_error{message, at};
    }

    if (!eof_set)
    {
        return;
    }

    const auto ended{[this](const std::string& name) {
        const auto stands_in{[&name](const End_rule& end) { return end.name == name || end.name == every_condition; }};

        return std::ranges::any_of(ends_, stands_in);
    }};

    if (named_.empty() && !ended(""))
    {
        throw Spec_error{"'re2c:eof' configuration is set, but no $ rule found", line};
    }

    for (const auto& name : named_)
    {
        if (!ended(name))
        {
            const auto message{
                    std::format("in condition '{}' 're2c:eof' configuration is set, but no $ rule found", name)};

            throw Spec_error{message, line};
        }
    }
}

const std::vector<std::string>& Rule_kinds::named() const noexcept
{
    return named_;
}

Block_reader::Block_reader(
        const std::string_view source, const std::size_t begin, const Re2c_flags reading, const Re2c_flags configured,
        Pass_state& pass, const Macros_t& macros)
    : Cursor{source, begin, source.size()}
    , opener_{begin}
    , reading_{reading}
    , configured_{configured}
    , pass_{pass}
    , macros_{macros}
    , encoding_line_{line()}
{}

void Block_reader::judge_later() noexcept
{
    judged_later_ = true;
}

void Block_reader::use(
        const std::size_t begin, Lexer_spec& spec, const Library_t& library, const Returning_t& returning)
{
    Block_reader used{text_, begin, reading_, configured_, pass_, macros_};

    used.judge_later();

    const auto before{spec.rules.size()};

    std::ignore = used.read(spec, library, returning);

    // The used block's actions are this block's to refuse, under the names this block's configurations leave.
    actions_.insert(actions_.end(), used.actions_.begin(), used.actions_.end());

    const auto brought{default_rules_from(spec, before)};

    used_defaults_.insert(used_defaults_.end(), brought.begin(), brought.end());

    kinds_.merge(used.kinds_);

    if (!deferred_)
    {
        deferred_ = used.deferred_;
    }

    configured_ = used.configured();

    const auto& used_sites{used.sites()};

    sites_.insert(sites_.end(), used_sites.begin(), used_sites.end());
}

std::size_t Block_reader::read(Lexer_spec& spec, const Library_t& library, const Returning_t& returning)
{
    returning_ = returning;

    // The rules already there are the using block's when this one is read through a `!use:` directive; the block's own
    // begin here, and its default rules are settled among these alone.
    const auto first{spec.rules.size()};

    for (skip_blanks(); !at(comment_closer); skip_blanks())
    {
        if (!peek())
        {
            fail("the block never closes");
        }

        if (at(configuration_prefix))
        {
            configuration(spec);

            continue;
        }

        if (at(use_prefix))
        {
            use_directive(spec, library);

            continue;
        }

        if (at(include_prefix))
        {
            fail("the block includes a file, which is not here to read");
        }

        item(spec);
    }

    settle_defaults(spec, first, used_defaults_);

    // An imported default rule the block's own default overrides is gone, and re2c emits no code for its action.
    const auto dropped{[&spec](const Action& entry) {
        const auto& [code, line, what, of_rule]{entry};

        const auto owns{
                [&line, &code](const Lexer_spec::Rule& rule) { return rule.line == line && rule.action == code; }};

        return of_rule && std::ranges::none_of(spec.rules, owns);
    }};

    std::erase_if(actions_, dropped);

    // A block read through a `!use:` directive is compiled where it is used, under the using block's configurations,
    // which may stand after the directive: its actions wait for that block to finish.
    if (!judged_later_)
    {
        refuse_actions();
    }

    return at_ + comment_closer.size();
}

const Rule_kinds& Block_reader::kinds() const noexcept
{
    return kinds_;
}

Re2c_flags Block_reader::configured() const noexcept
{
    return configured_;
}

std::size_t Block_reader::encoding_line() const noexcept
{
    return encoding_line_;
}

const std::vector<Definition_site>& Block_reader::sites() const noexcept
{
    return sites_;
}

const std::optional<Spec_error>& Block_reader::deferred() const noexcept
{
    return deferred_;
}

void Block_reader::configuration(Lexer_spec& spec)
{
    const auto end{configuration_end(text_, at_, end_)};

    if (end >= end_)
    {
        fail("a configuration is never closed with ';'");
    }

    const auto value_at{at_ + configuration_prefix.size()};

    std::string option{text_.substr(value_at, end - value_at)};

    // Blanks around the '=' say nothing; one spelling per configuration keeps the options comparable.
    std::erase_if(option, is_blank);

    configure(option, line(), configured_, encoding_line_, pass_.api_custom, pass_.pointers);

    spec.options.push_back(std::move(option));

    at_ = end + 1;
}

void Block_reader::use_directive(Lexer_spec& spec, const Library_t& library)
{
    at_ += use_prefix.size();

    std::string name{};

    while (peek() && is_name_byte(*peek()))
    {
        name.push_back(next("a block name"));
    }

    skip_blanks();

    expect(';', "';' to end the use directive");

    const auto found{library.find(name)};

    if (found == library.end())
    {
        fail(std::format("the used block '{}' is not above this one", name));
    }

    const auto& [used_name, begin]{*found};

    use(begin, spec, library, returning_);
}

void Block_reader::item(Lexer_spec& spec)
{
    const auto rule_line{line()};

    const auto listed{peek() == '<'};

    std::optional<std::vector<std::string>> named{std::vector<std::string>{}};

    if (listed)
    {
        ++at_;

        named = conditions();
    }

    if (named && named->empty() && take_flex_definition(spec))
    {
        return;
    }

    auto [pattern, expression, points]{regex_text(spec.definitions, false)};

    if (take_definition(pattern, spec))
    {
        return;
    }

    // The entry rule `<>`, an empty condition list and no regex, whose code re2c runs in the condition it numbers zero
    // before any rule is tried, and a `<!c>` setup rule, whose code it runs before every action of its conditions,
    // match nothing and are no tokens.
    const auto entry{listed && named && named->empty() && pattern.empty()};

    if (named && !entry && pattern.empty())
    {
        fail("a rule has no regex");
    }

    auto code{action()};

    if (entry || !named)
    {
        setup_action(std::move(code), rule_line, entry);

        return;
    }

    rule(spec, *std::move(named), std::move(pattern), std::move(expression), std::move(code), rule_line);
}

std::optional<std::vector<std::string>> Block_reader::conditions()
{
    // A setup rule's `!` may stand after blanks, `< ! C >`, as re2c reads it.
    const auto mark{text_.find_first_not_of(" \t", at_)};

    const auto setup{mark != std::string_view::npos && text_[mark] == '!'};

    std::vector<std::string> names{};

    std::string name{};

    for (;;)
    {
        const auto byte{next("'>' to close the condition list")};

        if (byte == '>' || byte == ',')
        {
            if (!name.empty())
            {
                names.push_back(std::exchange(name, {}));
            }

            if (byte == '>')
            {
                break;
            }

            continue;
        }

        if (byte != ' ' && byte != '\t' && byte != '!')
        {
            name.push_back(byte);
        }
    }

    return setup ? std::nullopt : std::optional{std::move(names)};
}

bool Block_reader::take_flex_definition(Lexer_spec& spec)
{
    if (!is_name_start(*peek()))
    {
        return false;
    }

    const auto opened{at_};

    const auto rest{text_.substr(at_)};

    const auto past_name{std::ranges::find_if_not(rest, is_name_byte)};

    const auto name_end{at_ + static_cast<std::size_t>(past_name - rest.begin())};

    const std::string name{text_.substr(at_, name_end - at_)};

    const auto blank_after{name_end < text_.size() && (text_[name_end] == ' ' || text_[name_end] == '\t')};

    const auto other{text_.find_first_not_of(" \t", name_end)};

    const auto after_blanks{std::min(other, text_.size())};

    const auto opens_definition{blank_after && after_blanks < text_.size() && text_[after_blanks] != '{'};

    if (!opens_definition || (!reading_.flex_syntax && spec.definitions.contains(name)))
    {
        return false;
    }

    const auto newline{text_.find('\n', at_)};

    const auto line_end{std::min(newline, text_.size())};

    // The body is read under the flex syntax, whose literals bare names are; the flag stays if it is one.
    const auto flex_before{std::exchange(reading_.flex_syntax, true)};

    at_ = name_end;

    auto [body, expression, points]{regex_text(spec.definitions, true)};

    if (at_ >= line_end && !body.empty())
    {
        spec.definitions.insert_or_assign(name, expression);

        note_class(pass_.classes, name, std::move(points));

        sites_.push_back({.name = name, .begin = name_end, .line_bound = true});

        // The evidence of the flex syntax is no configuration: it governs the reading from here on and the next pass
        // and block alike.
        configured_.flex_syntax = true;

        return true;
    }

    if (!body.empty())
    {
        std::string unbound{};

        if (!flex_before)
        {
            unbound = std::format(", and without that syntax '{}' is a symbol no definition binds", name);
        }

        fail(std::format(
                "under the flex syntax '{}' followed by a blank opens a definition, which ends with its line, so re2c "
                "answers what follows the regex on this line with a syntax error{}",
                name, unbound));
    }

    reading_.flex_syntax = flex_before;

    at_ = opened;

    return false;
}

Regex_text Block_reader::regex_text(const regex::Definitions_t& definitions, const bool line_bound)
{
    Regex_reader reader{text_, at_, reading_, line_bound, pass_.classes, deferred_};

    auto read{reader.regex_text(definitions)};

    at_ = reader.offset();

    return read;
}

bool Block_reader::take_definition(const std::string& name, Lexer_spec& spec)
{
    if (peek() != '=' || at(transition_opener))
    {
        return false;
    }

    ++at_;

    const auto body_begin{at_};

    auto [body, body_expression, body_points]{regex_text(spec.definitions, false)};

    if (body.empty())
    {
        fail(std::format("the definition '{}' has no regex", name));
    }

    if (peek() != ';')
    {
        fail(std::format("expected ';' to close the definition '{}'", name));
    }

    ++at_;

    spec.definitions.insert_or_assign(name, body_expression);

    note_class(pass_.classes, name, std::move(body_points));

    sites_.push_back({.name = name, .begin = body_begin, .line_bound = false});

    return true;
}

std::string Block_reader::action()
{
    std::string code{};

    // A shortcut rule, `:=> condition`, has no code at all: it ends with the condition's name, so the line ends it and
    // the lines after it are items of their own.
    if (at(shortcut_opener))
    {
        while (peek() && *peek() != ';' && *peek() != '\n')
        {
            code.push_back(next("the condition the shortcut rule jumps to"));
        }

        if (peek() == ';')
        {
            code.push_back(next("';'"));
        }

        return code;
    }

    // A transition names a condition first and an action of either kind follows it; it is kept as text, since which
    // condition follows says nothing about the token.
    if (at(transition_opener))
    {
        while (peek() && *peek() != '{' && !at(line_action_opener) && *peek() != ';' && *peek() != '\n')
        {
            code.push_back(next("the transition"));
        }

        if (peek() == ';')
        {
            code.push_back(next("';'"));

            return code;
        }
    }

    if (at(line_action_opener))
    {
        // A line beginning with a blank, or an empty line, continues a `:=` action; the block's close ends it too.
        const auto ends{[this] {
            const auto after{at_ + 1};

            return after >= end_ || !is_blank(text_[after]);
        }};

        while (peek() && !(*peek() == '\n' && ends()))
        {
            code.push_back(next("the action"));
        }

        return code;
    }

    if (peek() != '{')
    {
        fail("expected an action, a '{' block or ':=' and the rest of the line");
    }

    const auto close{brace_close(text_.substr(at_))};

    if (!close)
    {
        fail("the action's braces never close");
    }

    code += text_.substr(at_, *close);

    at_ += *close;

    return code;
}

void Block_reader::setup_action(std::string code, const std::size_t line, const bool entry)
{
    // One whose code returns would return in its condition before any rule's own action, which no token set is.
    if (returned(code, returning_))
    {
        const std::string_view refusal{
                entry ? "the entry rule <> returns, so the scanner returns in its first condition before any rule is "
                        "tried" :
                        "a setup rule returns, so every rule of its conditions returns it before its own action"};

        fail(std::string{refusal});
    }

    // The check waits for the block's last configuration, which names the pointers.
    actions_.push_back(
            {.code = std::move(code),
             .line = line,
             .what = entry ? "the entry rule <> moves " : "a setup rule moves ",
             .of_rule = false});
}

void Block_reader::rule(
        Lexer_spec& spec, std::vector<std::string> named, std::string pattern, std::string expression, std::string code,
        const std::size_t line)
{
    kinds_.note(named, pattern, line);

    static constexpr std::string_view double_quoted_empty{R"("")"};

    static constexpr std::string_view single_quoted_empty{"''"};

    // The end rule is no token, and neither is the empty rule `""`, with or without trailing context, which consumes
    // nothing where nothing else matches.
    const auto past{double_quoted_empty.size()};

    const auto empty{
            (pattern.starts_with(double_quoted_empty) || pattern.starts_with(single_quoted_empty)) &&
            (pattern.size() == past || pattern.find_first_not_of(' ', past) == pattern.find('/', past))};

    if (pattern == end_rule || empty)
    {
        return;
    }

    // The default rule `*` matches one code unit, one byte under every encoding the reading follows.
    if (pattern == default_rule)
    {
        expression = any_byte;
    }

    actions_.push_back({.code = code, .line = line, .what = "the action ", .of_rule = true});

    auto token{returned(code, returning_)};

    spec.rules.push_back(
            {.pattern = std::move(pattern),
             .expression = std::move(expression),
             .conditions = std::move(named),
             .action = std::move(code),
             .token = std::move(token),
             .priority = std::nullopt,
             .line = line});
}

void Block_reader::refuse_actions()
{
    // The API is refused where it is read under, by a block with rules or actions of its own: a block that only
    // configures leaves the setting to the blocks after it, which may set it back before any rule.
    if (pass_.api_custom && !actions_.empty())
    {
        throw Spec_error{
                "the block sets api, so the scanner advances by calling the API's own operations rather than by "
                "stepping a scan pointer, and an action moving the match is out of the audit's sight",
                *pass_.api_custom};
    }

    const auto restarts{restart_labels(text_, opener_, returning_)};

    for (const auto& action : actions_)
    {
        refuse_action(action, pass_.pointers, macros_, returning_, restarts);
    }

    actions_.clear();
}

} // namespace munch::tools::audit
