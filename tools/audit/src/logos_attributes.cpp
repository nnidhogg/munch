#include "munch/tools/audit/logos_attributes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements logos_attributes.hpp: one attribute read from its `#` or its path, a `cfg` predicate by its form, the
// attributes a `cfg_attr` carries, a rule attribute's arguments, a `#[logos(...)]` entry read by its key, a priority's
// number and a tuple's fields are private to this unit.

/**
 * @brief The head of one entry of a `#[logos(...)]`: its key, where and on which line it stands, and the shape of the
 *        value after it.
 */
struct Logos_entry
{
    /**
     * @brief The key.
     */
    std::string key;

    /**
     * @brief The offset the key begins at.
     */
    std::size_t begin;

    /**
     * @brief The line the key stands on.
     */
    std::size_t line;

    /**
     * @brief Whether the value is a parenthesised group, `skip(...)` or `error(...)`.
     */
    bool group_valued;

    /**
     * @brief Whether the value follows an `=`.
     */
    bool assigned;
};

/**
 * @brief What the arguments of a `#[token(...)]`, a `#[regex(...)]` or a `skip(...)` read so far have given.
 */
struct Given_arguments
{
    /**
     * @brief Whether a callback has been given, in first position or as `callback = ...`.
     */
    bool callback;

    /**
     * @brief Whether a `priority = n` has been given.
     */
    bool priority;

    /**
     * @brief Whether an `ignore(...)` has been given, which closes the arguments.
     */
    bool ignore;
};

/**
 * @brief What a `cfg` predicate is by its form alone: `all(...)` is true when every argument is and false when one is,
 *        `any(...)` true when one is and false when every one is, so that `all()` of nothing is true and `any()` of
 *        nothing false, `not(p)` is the opposite of `p`, and a name or a `name = "value"` pair, `test`, `unix` or
 *        `feature = "x"`, is decided by the build alone, as is any other form.
 * @param predicate The cursor over the predicate, left after it.
 * @return True or false when the form decides it, std::nullopt when the build does.
 * @throws Spec_error If a group in the predicate is left open.
 */
[[nodiscard]] std::optional<bool> cfg_value(Rust_cursor& predicate)
{
    predicate.skip_trivia();

    const std::string word{predicate.word()};

    predicate.skip_trivia();

    if (predicate.peek() != '(')
    {
        // A name, with its value if any, up to the end of the argument it is.
        while (!predicate.done() && predicate.peek() != ',')
        {
            predicate.skip_token();
        }

        return std::nullopt;
    }

    const auto open{predicate.offset()};

    predicate.skip_group();

    auto arguments{predicate.inside(open + 1, predicate.offset() - 1)};

    if (word == "not")
    {
        const auto value{cfg_value(arguments)};

        return value ? std::optional{!*value} : std::nullopt;
    }

    if (word != "all" && word != "any")
    {
        return std::nullopt;
    }

    const auto all{word == "all"};

    auto decided{true};

    for (arguments.skip_trivia(); !arguments.done(); arguments.skip_trivia())
    {
        const auto value{cfg_value(arguments)};

        // One false argument decides an `all`, one true argument an `any`.
        if (value && *value != all)
        {
            return !all;
        }

        decided = decided && value.has_value();

        arguments.skip_trivia();

        if (!arguments.accept(','))
        {
            break;
        }
    }

    return decided ? std::optional{all} : std::nullopt;
}

/**
 * @brief Reads the attribute at the cursor, its `#[` and `]` aside: `path`, `path(...)` or `path = value`, the value
 *        running to a `]`, a `,` or the end.
 * @param cursor The cursor, at the path.
 * @param line The line the attribute opens on.
 * @return The attribute.
 * @throws Spec_error If the path is missing or a group is left open.
 */
[[nodiscard]] Attribute read_meta(Rust_cursor& cursor, const std::size_t line)
{
    std::string path{cursor.word()};

    for (cursor.skip_trivia(); cursor.at("::"); cursor.skip_trivia())
    {
        cursor.expect(':', "':'");
        cursor.expect(':', "':'");

        cursor.skip_trivia();

        path += "::" + std::string{cursor.word()};
    }

    if (path.empty())
    {
        cursor.fail("expected the attribute's name after '#['");
    }

    auto begin{cursor.offset()};

    auto end{begin};

    const auto delimited{cursor.peek() == '(' || cursor.peek() == '[' || cursor.peek() == '{'};

    if (delimited)
    {
        begin = cursor.offset() + 1;

        cursor.skip_group();

        end = cursor.offset() - 1;
    }
    else
    {
        while (!cursor.done() && cursor.peek() != ']' && cursor.peek() != ',')
        {
            cursor.skip_token();
        }
    }

    return {.path = std::move(path), .begin = begin, .end = end, .delimited = delimited, .line = line, .assumed = {}};
}

/**
 * @brief The unsigned decimal a `priority = n` gives.
 * @param text The value's text.
 * @param cursor The cursor, for the refusal's line.
 * @return The number.
 * @throws Spec_error If the text is not a decimal that fits.
 */
[[nodiscard]] std::size_t unsigned_value(const std::string_view text, const Rust_cursor& cursor)
{
    std::size_t value{0};

    for (const auto byte : text)
    {
        const auto digit{static_cast<std::size_t>(byte - '0')};

        if (!is_digit(byte) || value > (std::numeric_limits<std::size_t>::max() - digit) / 10)
        {
            cursor.fail("priority expects an unsigned integer, got '" + std::string{text} + "'");
        }

        value = value * 10 + digit;
    }

    if (text.empty())
    {
        cursor.fail("priority expects an unsigned integer");
    }

    return value;
}

/**
 * @brief The text of an argument from an offset to its end, its trailing blanks dropped.
 * @param content A cursor over the file the offsets index.
 * @param from The offset the text begins at.
 * @param end The offset the argument ends at.
 * @return The text.
 */
[[nodiscard]] std::string argument_text(const Rust_cursor& content, const std::size_t from, const std::size_t end)
{
    return without_trailing_blanks(std::string{content.slice(from, end)});
}

/**
 * @brief Refuses a `skip` or a `subpattern` whose value is of another shape than its key takes, in the crate's words:
 *        neither takes `= value`, `skip` takes a group or a literal and `subpattern` a name.
 * @param content The cursor, at the value.
 * @param entry The entry's head.
 * @param group Whether the key takes a parenthesised group.
 * @param keyword Whether the key takes a name.
 * @param expected The shape the key takes, in the crate's words.
 * @throws Spec_error If the value is of another shape.
 */
void check_shape(
        const Rust_cursor& content, const Logos_entry& entry, const bool group, const bool keyword,
        const std::string_view expected)
{
    const auto shape{
            !entry.assigned && (entry.group_valued  ? group :
                                content.at_string() ? entry.key == "skip" :
                                                      keyword)};

    if (!shape)
    {
        content.fail("logos 0.15.1 refuses this shape of `" + entry.key + "`: " + std::string{expected});
    }
}

/**
 * @brief The shape a key other than `skip` and `subpattern` takes, in the crate's words.
 * @param key The key, one of the six logos 0.15.1 records as an option.
 * @return The words.
 */
[[nodiscard]] std::string_view expected_shape(const std::string_view key)
{
    return key == "crate"      ? "Expected: #[logos(crate = path::to::logos)]" :
           key == "error"      ? "Expected: #[logos(error = SomeType)] or #[logos(error(SomeType[, callback))]" :
           key == "export_dir" ? R"(Expected #[logos(export_dir = "path/to/export/dir")])" :
           key == "extras"     ? "Expected: #[logos(extras = SomeType)]" :
           key == "source"     ? "Expected: #[logos(source = SomeType)]" :
                                 "Expected: #[logos(type T = SomeType)]";
}

/**
 * @brief The attributes an attribute stands for: itself, or, for `#[cfg_attr(predicate, a, b)]`, the `a` and `b` it
 *        applies where the predicate holds, none where it is false by its form, each expanded in turn where it is a
 *        `cfg_attr` itself, as rustc expands the attribute before any derive runs; under a predicate the build alone
 *        decides, `feature = "x"`, the attributes are applied, as an item under such a `#[cfg]` is read as standing,
 *        each carrying the predicate as assumed, for a scanner's options to say so.
 * @param attribute The attribute.
 * @param cursor The cursor over the file the attribute's offsets index.
 * @return The attributes, in the order written.
 * @throws Spec_error If a group in the content is left open or an attribute carried has no path.
 */
[[nodiscard]] std::vector<Attribute> expanded(const Attribute& attribute, const Rust_cursor& cursor)
{
    if (attribute.path != "cfg_attr" || !attribute.delimited)
    {
        return {attribute};
    }

    auto content{cursor.inside(attribute.begin, attribute.end)};

    content.skip_trivia();

    const auto begin{content.offset()};

    const auto value{cfg_value(content)};

    if (value && !*value)
    {
        return {};
    }

    const auto assumed{value ? std::string{} : compacted(cursor.slice(begin, content.offset()))};

    std::vector<Attribute> attributes;

    for (content.skip_trivia(); content.accept(','); content.skip_trivia())
    {
        content.skip_trivia();

        // A trailing comma carries nothing.
        if (content.done())
        {
            break;
        }

        for (auto& carried : expanded(read_meta(content, content.line()), cursor))
        {
            if (!assumed.empty())
            {
                carried.assumed.insert(carried.assumed.begin(), assumed);
            }

            attributes.push_back(std::move(carried));
        }
    }

    return attributes;
}

/**
 * @brief Reads the attribute opening at the cursor, `#[path]`, `#[path(...)]` or `#[path = ...]`.
 * @param cursor The cursor, at the `#`.
 * @return The attribute.
 * @throws Spec_error If the attribute is left open.
 */
[[nodiscard]] Attribute read_attribute(Rust_cursor& cursor)
{
    const auto line{cursor.line()};

    cursor.expect('#', "'#'");

    cursor.skip_trivia();

    cursor.expect('[', "'[' to open the attribute");

    cursor.skip_trivia();

    auto attribute{read_meta(cursor, line)};

    cursor.skip_trivia();

    cursor.expect(']', "']' to close the attribute");

    return attribute;
}

/**
 * @brief Reads a named argument after its key, `priority = n` or `callback = ...`; logos 0.15.1 knows `priority`,
 *        `callback` and `ignore` and calls anything else an unknown nested attribute, `allow_greedy` among them.
 * @param item A cursor over the argument, at its `=`.
 * @param key The argument's key.
 * @param end The offset the argument ends at.
 * @param definition The definition, whose priority or callback is set.
 * @param given What the arguments read so far have given, which the argument adds to.
 * @throws Spec_error If the key is given twice or is none logos knows, or a priority is no unsigned integer.
 */
void read_named_argument(
        Rust_cursor& item, const std::string& key, const std::size_t end, Definition& definition,
        Given_arguments& given)
{
    item.expect('=', "'='");

    item.skip_trivia();

    if (key == "priority")
    {
        if (std::exchange(given.priority, true))
        {
            item.fail("logos 0.15.1 refuses a second priority: Resetting previously set priority");
        }

        definition.priority = unsigned_value(argument_text(item, item.offset(), end), item);

        return;
    }

    if (key == "callback")
    {
        if (std::exchange(given.callback, true))
        {
            item.fail("logos 0.15.1 refuses a second callback: Callback has been already set");
        }

        definition.callback = argument_text(item, item.offset(), end);

        return;
    }

    item.fail("logos knows no argument '" + key + "'; expected callback, priority or ignore");
}

/**
 * @brief Reads the flags of an `ignore(...)`, `case` or `ascii_case`, into the definition's folding.
 * @param item A cursor over the argument, at the `(` after `ignore`.
 * @param end The offset the argument ends at.
 * @param definition The definition, whose folding is set.
 * @throws Spec_error If a flag is none logos has, or the two flags are given together.
 */
void read_ignore(const Rust_cursor& item, const std::size_t end, Definition& definition)
{
    auto flags{item.inside(item.offset() + 1, end)};

    for (flags.skip_trivia(); !flags.done() && flags.peek() != ')'; flags.skip_trivia())
    {
        const auto flag{flags.word()};

        if (flag != "case" && flag != "ascii_case")
        {
            flags.fail("ignore knows no flag '" + std::string{flag} + "'; expected case or ascii_case");
        }

        const auto asked{flag == "case" ? Ignore_case::unicode : Ignore_case::ascii};

        if (definition.folding != Ignore_case::none && definition.folding != asked)
        {
            flags.fail("logos refuses the flag case along with ascii_case");
        }

        definition.folding = asked;

        flags.skip_trivia();

        if (!flags.accept(','))
        {
            break;
        }
    }
}

/**
 * @brief Reads the head of the entry at the cursor, its key and the shape of its value: `= value`, `(group)`, `name =
 *        value` or a literal, or nothing at all, which is a bare key and an invalid nested attribute to the crate
 *        whatever the key.
 * @param content The cursor, at the entry, left at its value.
 * @return The entry's head.
 * @throws Spec_error If no key stands here, or the key is bare.
 */
[[nodiscard]] Logos_entry read_entry(Rust_cursor& content)
{
    const auto begin{content.offset()};

    const auto line{content.line()};

    std::string key{content.word()};

    if (key.empty())
    {
        content.fail("expected a key in #[logos(...)]");
    }

    content.skip_trivia();

    const auto group_valued{content.peek() == '('};

    const auto assigned{content.peek() == '=' && !content.at("==")};

    if (content.done() || content.peek() == ',')
    {
        content.fail("logos 0.15.1 refuses a bare `" + key + "` in #[logos(...)]: Invalid nested attribute");
    }

    return {.key = std::move(key), .begin = begin, .line = line, .group_valued = group_valued, .assigned = assigned};
}

/**
 * @brief Reads a `skip` entry's value, `skip "regex"` or `skip(...)`, into a skip definition.
 * @param content The cursor, at the value, left after it.
 * @param entry The entry's head.
 * @param skips The skip definitions collected so far, added to.
 * @throws Spec_error If the value is of another shape, or as read_definition() refuses a group's arguments.
 */
void read_skip(Rust_cursor& content, const Logos_entry& entry, std::vector<Definition>& skips)
{
    check_shape(content, entry, true, false, R"(Expected: #[logos(skip "regex literal")] or #[logos(skip(...))])");

    if (content.peek() == '(')
    {
        const auto open{content.offset()};

        content.skip_group();

        skips.push_back(read_definition(content.inside(open + 1, content.offset() - 1), "skip", true));

        return;
    }

    skips.push_back(
            {.literal = content.literal(),
             .callback = {},
             .priority = std::nullopt,
             .folding = Ignore_case::none,
             .line = entry.line});
}

/**
 * @brief Reads a `subpattern` entry's value, `subpattern name = "regex"`, compiled in order into the specification's
 *        definitions and the table.
 * @param content The cursor, at the value, left after the literal.
 * @param entry The entry's head.
 * @param spec The specification being filled.
 * @param subpatterns The subpatterns declared so far, added to.
 * @throws Spec_error If the value is of another shape, the subpattern is declared twice or its name would read as a
 *         count, or the pattern is refused.
 */
void read_subpattern(Rust_cursor& content, const Logos_entry& entry, Lexer_spec& spec, Subpatterns_t& subpatterns)
{
    check_shape(content, entry, false, true, R"(Expected: #[logos(subpattern name = r"regex")])");

    const std::string name{content.word()};

    content.skip_trivia();

    content.expect('=', "'=' after the subpattern's name");

    content.skip_trivia();

    if (name.empty() || is_digit(name.front()))
    {
        content.fail(
                "a subpattern needs a name that opens with a letter or an underscore, since {" + name +
                "} would read as a count");
    }

    if (subpatterns.contains(name))
    {
        content.fail("the subpattern '" + name + "' is declared twice");
    }

    auto literal{content.literal()};

    auto expression{compile(literal, Pattern_kind::definition, Ignore_case::none, subpatterns, entry.line).expression};

    auto text{substituted(literal, subpatterns, entry.line)};

    subpatterns.insert_or_assign(
            name, Subpattern{.literal = std::move(literal), .text = std::move(text), .empty = expression.empty()});

    spec.definitions.insert_or_assign(name, std::move(expression));
}

/**
 * @brief Records an entry of any key but `skip` and `subpattern` among the options, the key then the rest without its
 *        trivia, `extras=Extras`, `error(E,callback=f)`, `type S=&str`; `crate = path` also binds the path to the
 *        crate, since that is where the enum's generated code finds the crate, and so where a callback may too.
 * @param content The cursor, at the end of the entry's value.
 * @param entry The entry's head.
 * @param spec The specification, whose options are added to.
 * @param names The names bound for the enum, which a `crate = path` entry binds the path to the crate in.
 * @param module The module the enum is declared in, as a path from the crate root, which that binding stands in.
 * @throws Spec_error If the key is none logos 0.15.1 knows, the value is of another shape than the key takes or missing
 *         after its `=`, or `extras`, `error`, `source` or one parameter's type is given twice.
 */
void record_option(
        const Rust_cursor& content, const Logos_entry& entry, Lexer_spec& spec, Names_t& names,
        const std::string_view module)
{
    const auto& [key, begin, line, group_valued, assigned]{entry};

    // logos 0.15.1 knows eight keys and calls any other an unknown nested attribute (logos-codegen 0.15.1,
    // parser/mod.rs); `utf8` among the others is a later crate's.
    constexpr std::array known{std::string_view{"crate"},  std::string_view{"error"},  std::string_view{"export_dir"},
                               std::string_view{"extras"}, std::string_view{"source"}, std::string_view{"type"}};

    if (!std::ranges::contains(known, std::string_view{key}))
    {
        const auto message{
                "logos 0.15.1 knows no #[logos(" + key +
                ")] attribute; expected one of: crate, error, export_dir, extras, skip, source, subpattern, type"};

        throw Spec_error{message, line};
    }

    const auto rest{compacted(content.slice(begin + key.size(), content.offset()))};

    const auto option{key + (rest.starts_with('=') || rest.starts_with('(') ? "" : " ") + rest};

    // The shape each key takes, and the keys the crate takes once: `extras`, `error` and `source`, and the type of each
    // parameter.
    const auto shaped{
            key == "crate"      ? assigned :
            key == "error"      ? assigned || group_valued :
            key == "export_dir" ? assigned :
            key == "extras"     ? assigned :
            key == "source"     ? assigned :
                                  !assigned && !group_valued};

    if (!shaped || (key == "type" && option.find('=') == std::string::npos))
    {
        throw Spec_error{"logos 0.15.1 refuses this shape of `" + key + "`: " + std::string{expected_shape(key)}, line};
    }

    // A value must follow the `=`; the crate's parse of the type says so.
    if (rest.ends_with('='))
    {
        throw Spec_error{"logos 0.15.1 refuses `" + option + "` with nothing after it: expected type", line};
    }

    const auto once{key == "type" ? option.substr(0, option.find('=')) : key};

    if ((key == "extras" || key == "error" || key == "source" || key == "type") && option_given(spec.options, once))
    {
        const auto what{
                key == "extras" ? "Extras" :
                key == "error"  ? "Error type" :
                key == "source" ? "Source" :
                                  once.substr(5)};

        const auto message{
                "logos 0.15.1 refuses a second `" + once + "`: " + what +
                (key == "type" ? " can only have one type assigned to it" : " can be defined only once")};

        throw Spec_error{message, line};
    }

    spec.options.push_back(option);

    if (key == "crate" && rest.starts_with('='))
    {
        bind_name(
                names, module, std::string{rest.substr(rest.starts_with("=::") ? 3 : 1)}, "::logos", false,
                type_namespace);
    }
}

/**
 * @brief The fields of a variant's tuple, split at the commas outside groups and generics, each without its trivia and
 *        the attributes before its type; a trailing comma closes the last field rather than opening another.
 * @param fields A cursor over the text between the parentheses.
 * @return The fields' types.
 * @throws Spec_error If a group is left open.
 */
[[nodiscard]] std::vector<std::string> tuple_fields(Rust_cursor fields)
{
    std::vector<std::string> types;

    for (fields.skip_trivia(); !fields.done(); fields.skip_trivia())
    {
        while (at_attribute(fields))
        {
            std::ignore = fields.next("'#'");

            fields.skip_group();

            fields.skip_trivia();
        }

        const auto begin{fields.offset()};

        while (!fields.done() && fields.peek() != ',')
        {
            if (fields.peek() == '<')
            {
                skip_generics(fields);
            }
            else
            {
                fields.skip_token();
            }
        }

        types.push_back(compacted(fields.slice(begin, fields.offset())));

        std::ignore = fields.accept(',');
    }

    return types;
}

} // namespace

void read_attributes(Rust_cursor& cursor, std::vector<Attribute>& attributes)
{
    for (cursor.skip_trivia(); at_attribute(cursor); cursor.skip_trivia())
    {
        for (auto& attribute : expanded(read_attribute(cursor), cursor))
        {
            attributes.push_back(std::move(attribute));
        }
    }
}

bool stands_by_form(const std::vector<Attribute>& attributes, const Rust_cursor& cursor)
{
    return std::ranges::all_of(attributes, [&cursor](const Attribute& attribute) {
        if (attribute.path != "cfg" || !attribute.delimited)
        {
            return true;
        }

        auto predicate{cursor.inside(attribute.begin, attribute.end)};

        return cfg_value(predicate).value_or(true);
    });
}

void note_assumed(const Attribute& attribute, const Rust_cursor& cursor, std::vector<std::string>& options)
{
    auto predicates{attribute.assumed};

    if (attribute.path == "cfg" && attribute.delimited)
    {
        auto predicate{cursor.inside(attribute.begin, attribute.end)};

        if (!cfg_value(predicate))
        {
            predicates.push_back(compacted(cursor.slice(attribute.begin, attribute.end)));
        }
    }

    for (const auto& predicate : predicates)
    {
        if (const auto option{"cfg=" + predicate}; !std::ranges::contains(options, option))
        {
            options.push_back(option);
        }
    }
}

std::optional<std::size_t> derives_logos(const std::vector<Attribute>& attributes, const Rust_cursor& cursor)
{
    for (const auto& attribute : attributes)
    {
        if (attribute.path != "derive")
        {
            continue;
        }

        auto list{cursor.inside(attribute.begin, attribute.end)};

        for (list.skip_trivia(); !list.done(); list.skip_trivia())
        {
            std::string name{list.word()};

            for (list.skip_trivia(); list.at("::"); list.skip_trivia())
            {
                list.expect(':', "':'");
                list.expect(':', "':'");

                list.skip_trivia();

                name = std::string{list.word()};
            }

            if (name == "Logos")
            {
                return attribute.line;
            }

            if (!list.done())
            {
                list.expect(',', "',' between the derives");
            }
        }
    }

    return std::nullopt;
}

Definition read_definition(Rust_cursor content, const std::string_view attribute, const bool skip)
{
    const auto line{content.line()};

    content.skip_trivia();

    if (content.done())
    {
        content.fail(
                skip ? std::string{"logos 0.15.1 refuses an empty skip(...): Expected #[logos(skip(\"regex literal\"[, "
                                   "[callback = ] callback, priority = priority]))]"} :
                       "logos 0.15.1 refuses an empty #[" + std::string{attribute} + "(...)]: Expected #[" +
                                std::string{attribute} + "(...)]");
    }

    Definition definition{
            .literal = content.literal(),
            .callback = {},
            .priority = std::nullopt,
            .folding = Ignore_case::none,
            .line = line};

    Given_arguments given{.callback = false, .priority = false, .ignore = false};

    for (std::size_t position{0};; ++position)
    {
        content.skip_trivia();

        if (content.done())
        {
            return definition;
        }

        content.expect(',', "',' between the attribute's arguments");

        content.skip_trivia();

        if (content.done())
        {
            return definition;
        }

        if (given.ignore)
        {
            content.fail(
                    "logos 0.15.1 refuses an argument after ignore(...) in one attribute, the comma after the group "
                    "being left unread: Expected a named argument at this position; write the argument before "
                    "ignore(...)");
        }

        // One argument runs to the next comma outside any group, string or closure body.
        const auto begin{content.offset()};

        while (!content.done() && content.peek() != ',')
        {
            content.skip_token();
        }

        const auto end{content.offset()};

        auto item{content.inside(begin, end)};

        const std::string key{item.word()};

        item.skip_trivia();

        if (!key.empty() && item.peek() == '=' && !item.at("=="))
        {
            read_named_argument(item, key, end, definition, given);

            continue;
        }

        if ((key == "priority" || key == "callback") && item.peek() == '(')
        {
            item.fail(
                    "logos 0.15.1 refuses " + key +
                    "(...): Expected: " + (key == "priority" ? "priority = <integer>" : "callback = ..."));
        }

        if (key == "ignore" && item.peek() == '(')
        {
            if (skip)
            {
                item.fail("logos knows no argument 'ignore' on skip(...); expected callback or priority");
            }

            read_ignore(item, end, definition);

            given.ignore = true;

            continue;
        }

        if (position != 0)
        {
            item.fail(
                    "expected a named argument at this position; a callback after the first argument is written "
                    "callback = ...");
        }

        given.callback = true;

        definition.callback = argument_text(content, begin, end);
    }
}

bool option_given(const std::vector<std::string>& options, const std::string& key)
{
    return std::ranges::any_of(options, [&key](const std::string& option) {
        return option == key || option.starts_with(key + "=") || option.starts_with(key + "(");
    });
}

void read_logos_attribute(
        Rust_cursor content, Lexer_spec& spec, Subpatterns_t& subpatterns, std::vector<Definition>& skips,
        Names_t& names, const std::string_view module)
{
    for (content.skip_trivia(); !content.done(); content.skip_trivia())
    {
        const auto entry{read_entry(content)};

        if (entry.key == "skip")
        {
            read_skip(content, entry, skips);
        }
        else if (entry.key == "subpattern")
        {
            read_subpattern(content, entry, spec, subpatterns);
        }

        while (!content.done() && content.peek() != ',')
        {
            content.skip_token();
        }

        if (entry.key != "skip" && entry.key != "subpattern")
        {
            record_option(content, entry, spec, names, module);
        }

        if (content.done())
        {
            continue;
        }

        // logos 0.15.1 leaves the comma after `key(...)` unread, so the entry after it begins with that comma and is an
        // invalid nested attribute to it (logos-codegen 0.15.1, parser/nested.rs).
        if (entry.group_valued)
        {
            content.fail(
                    "logos 0.15.1 refuses an entry after " + entry.key +
                    "(...) in one #[logos(...)] as an invalid nested attribute; write it in a #[logos(...)] of "
                    "its own");
        }

        content.expect(',', "',' between the entries of #[logos(...)]");
    }
}

std::string variant_payload(Rust_cursor& cursor)
{
    const auto line{cursor.line()};

    const auto open{cursor.offset()};

    cursor.skip_group();

    if (cursor.slice(open, open + 1) == "{")
    {
        throw Spec_error{"logos 0.15.1 refuses named fields: Logos doesn't support named fields yet", line};
    }

    auto fields{tuple_fields(cursor.inside(open + 1, cursor.offset() - 1))};

    if (fields.size() != 1)
    {
        throw Spec_error{
                "logos 0.15.1 refuses the variant: Logos currently only supports variants with one field, found " +
                        std::to_string(fields.size()),
                line};
    }

    return std::move(fields.front());
}

} // namespace munch::tools::audit
