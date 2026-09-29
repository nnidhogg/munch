#include "munch/tools/audit/read_logos.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/tools/audit/logos_attributes.hpp"
#include "munch/tools/audit/logos_callback.hpp"
#include "munch/tools/audit/logos_pattern.hpp"
#include "munch/tools/audit/logos_regex.hpp"
#include "munch/tools/audit/rust_cursor.hpp"
#include "munch/tools/audit/rust_items.hpp"
#include "munch/tools/audit/rust_names.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements read_logos.hpp: the reading of one enum deriving Logos, its generics, its `#[logos(...)]` options and its
// variants, each with the rules its attributes define, and a rule's compilation are private to this unit.

/**
 * @brief One rule a variant's attribute defines: what the attribute says, and whether the pattern is a token's or a
 *        regex's.
 */
struct Variant_rule
{
    /**
     * @brief What the `#[token(...)]` or `#[regex(...)]` says.
     */
    Definition definition;

    /**
     * @brief What the pattern is to logos, a token matched as it stands or a regex.
     */
    Pattern_kind kind;
};

/**
 * @brief What the enum's `#[logos(...)]` attributes declare for its rules: the subpatterns, the skips, and the names
 *        the rules' callbacks are read under, the file's with the path `crate = ...` gives the crate.
 */
struct Logos_options
{
    /**
     * @brief The subpatterns, by name.
     */
    Subpatterns_t subpatterns;

    /**
     * @brief The skip definitions, in file order.
     */
    std::vector<Definition> skips;

    /**
     * @brief The names the callbacks are read under.
     */
    Names_t names;
};

/**
 * @brief A variant as read, with the rules its attributes define, which are read once every variant is, since a
 *        callback's result may name any of them.
 */
struct Read_variant
{
    /**
     * @brief The variant, its payload's type resolved.
     */
    Variant variant;

    /**
     * @brief The rules, in the order the attributes stand.
     */
    std::vector<Variant_rule> rules;
};

/**
 * @brief The rules a variant's attributes define, one per `#[token(...)]` and `#[regex(...)]`, in the order they stand.
 * @param attributes The variant's attributes.
 * @param cursor The cursor over the file the attributes' offsets index.
 * @return The rules.
 * @throws Spec_error For the legacy `#[error]` attribute, which logos 0.13 and later refuse (logos-codegen 0.15.1,
 *         lib.rs), or as read_definition() refuses an attribute's arguments.
 */
[[nodiscard]] std::vector<Variant_rule> variant_rules(
        const std::vector<Attribute>& attributes, const Rust_cursor& cursor)
{
    std::vector<Variant_rule> rules;

    for (const auto& attribute : attributes)
    {
        if (attribute.path == "error")
        {
            throw Spec_error{
                    "logos 0.15.1 refuses #[error]: Since 0.13 Logos no longer requires the #[error] variant",
                    attribute.line};
        }

        if (attribute.path == "token" || attribute.path == "regex")
        {
            rules.push_back(
                    {.definition =
                             read_definition(cursor.inside(attribute.begin, attribute.end), attribute.path, false),
                     .kind = attribute.path == "token" ? Pattern_kind::token : Pattern_kind::regex});
        }
    }

    return rules;
}

/**
 * @brief Reads the enum's generic parameters, where a `<` opens them: logos 0.15.1 takes one lifetime and type
 *        parameters each given a concrete type by `#[logos(type T = ...)]`, and refuses const generics and a second
 *        lifetime (logos-codegen 0.15.1, parser/mod.rs and parser/type_params.rs).
 * @param cursor The cursor, just past the enum's name and the trivia after it, left after the generics.
 * @param line The line of the derive naming Logos, for refusals.
 * @return The type parameters' names.
 * @throws Spec_error If the generics hold a second lifetime or a const generic, or are left open.
 */
[[nodiscard]] std::vector<std::string> read_generics(Rust_cursor& cursor, const std::size_t line)
{
    std::vector<std::string> type_parameters;

    if (cursor.peek() != '<')
    {
        return type_parameters;
    }

    const auto open{cursor.offset()};

    skip_generics(cursor);

    std::size_t lifetimes{0};

    // One parameter up to each comma outside nested generics: a lifetime, `const N: usize`, or a type.
    for (auto parameter{cursor.inside(open + 1, cursor.offset() - 1)}; parameter.skip_trivia(), !parameter.done();
         std::ignore = parameter.accept(','))
    {
        const auto first{parameter.word()};

        // A lifetime, its `'` under the cursor, is counted and names no type parameter, whatever word stood before it.
        const auto lifetime{parameter.at("'")};

        lifetimes += lifetime ? 1 : 0;

        if (lifetimes > 1)
        {
            throw Spec_error{"logos 0.15.1 refuses a second lifetime: Logos types can only have one lifetime", line};
        }

        if (!lifetime && first == "const")
        {
            throw Spec_error{"logos 0.15.1 refuses const generics: Logos doesn't support const generics.", line};
        }

        if (!lifetime && !first.empty())
        {
            type_parameters.emplace_back(first);
        }

        while (!parameter.done() && parameter.peek() != ',')
        {
            if (parameter.peek() == '<')
            {
                skip_generics(parameter);
            }
            else
            {
                parameter.skip_token();
            }
        }
    }

    return type_parameters;
}

/**
 * @brief Reads the enum's `#[logos(...)]` attributes, in order, into the specification and what its rules are read
 *        under.
 * @param attributes The enum's outer attributes.
 * @param cursor The cursor over the file the attributes' offsets index.
 * @param spec The specification, whose options and definitions are filled.
 * @param names The names the file binds.
 * @param module The module the enum is declared in, as a path from the crate root, empty at the root.
 * @return The subpatterns, the skips and the names the callbacks are read under.
 * @throws Spec_error If a `#[logos]` stands without its parentheses, or as read_logos_attribute() refuses an entry.
 */
[[nodiscard]] Logos_options read_logos_options(
        const std::vector<Attribute>& attributes, const Rust_cursor& cursor, Lexer_spec& spec, const Names_t& names,
        const std::string_view module)
{
    Logos_options options{.subpatterns = {}, .skips = {}, .names = names};

    for (const auto& attribute : attributes)
    {
        if (attribute.path != "logos")
        {
            continue;
        }

        if (!attribute.delimited)
        {
            throw Spec_error{
                    "logos 0.15.1 refuses a #[logos] without its parentheses: Expected #[logos(...)]", attribute.line};
        }

        read_logos_attribute(
                cursor.inside(attribute.begin, attribute.end), spec, options.subpatterns, options.skips, options.names,
                module);
    }

    return options;
}

/**
 * @brief Holds the enum's type parameters to the options: each `type T = ...` must name a parameter and each parameter
 *        must have one.
 * @param options The scanner's options.
 * @param type_parameters The enum's type parameters.
 * @param line The line of the derive naming Logos, for refusals.
 * @throws Spec_error If an assignment names no parameter, or a parameter has none.
 */
void check_type_parameters(
        const std::vector<std::string>& options, const std::vector<std::string>& type_parameters,
        const std::size_t line)
{
    for (const auto& option : options)
    {
        if (option.starts_with("type ") &&
            !std::ranges::contains(type_parameters, option.substr(5, option.find('=') - 5)))
        {
            throw Spec_error{
                    "logos 0.15.1 refuses the assignment: " + option.substr(5, option.find('=') - 5) +
                            " is not a declared type parameter",
                    line};
        }
    }

    for (const auto& parameter : type_parameters)
    {
        if (!option_given(options, "type " + parameter))
        {
            throw Spec_error{
                    "logos 0.15.1 refuses the enum: Generic type parameter without a concrete type; define a "
                    "concrete type Logos can use: #[logos(type " +
                            parameter + " = Type)]",
                    line};
        }
    }
}

/**
 * @brief Reads one variant, its attributes, its name, its payload and its discriminant, through the comma after it: a
 *        variant under a `cfg` false by its form is no rule, and its other attributes, its payload among them, are gone
 *        with it, since the `cfg` strips it before the derive reads the enum; what the reading assumed of a `cfg` or a
 *        `cfg_attr` the build alone decides is noted in the options.
 * @param cursor The cursor, at the variant, left after its comma or at the enum's closing brace.
 * @param options The scanner's options, added to.
 * @param names The names the payload's type is resolved under.
 * @param module The module the enum is declared in, as a path from the crate root, empty at the root.
 * @return The variant and its rules, or std::nullopt for a variant a `cfg` strips.
 * @throws Spec_error If the variant has no name or no comma after it, or an attribute or its payload is refused.
 */
[[nodiscard]] std::optional<Read_variant> read_variant(
        Rust_cursor& cursor, std::vector<std::string>& options, const Names_t& names, const std::string_view module)
{
    std::vector<Attribute> attributes;

    read_attributes(cursor, attributes);

    for (const auto& attribute : attributes)
    {
        note_assumed(attribute, cursor, options);
    }

    const auto stands{stands_by_form(attributes, cursor)};

    auto rules{stands ? variant_rules(attributes, cursor) : std::vector<Variant_rule>{}};

    Variant variant{.name = std::string{cursor.word()}, .payload = {}};

    if (variant.name.empty())
    {
        cursor.fail("expected a variant's name");
    }

    cursor.skip_trivia();

    if (cursor.peek() == '(' || cursor.peek() == '{')
    {
        if (stands)
        {
            variant.payload = canonical_type(names, variant_payload(cursor), module, Namespace::type);
        }
        else
        {
            cursor.skip_group();
        }

        cursor.skip_trivia();
    }

    if (cursor.accept('='))
    {
        while (!cursor.done() && cursor.peek() != ',' && cursor.peek() != '}')
        {
            cursor.skip_token();
        }
    }

    cursor.skip_trivia();

    if (cursor.peek() != '}')
    {
        cursor.expect(',', "',' or '}' after the variant '" + variant.name + "'");
    }

    if (!stands)
    {
        return std::nullopt;
    }

    return Read_variant{.variant = std::move(variant), .rules = std::move(rules)};
}

/**
 * @brief Adds one rule to a specification.
 * @param spec The specification.
 * @param definition The attribute's definition.
 * @param kind What the pattern is to logos, a token matched as it stands or a regex.
 * @param variant The variant, or std::nullopt for a skip, whose callback has no result to read, every result the crate
 *        admits there skipping or failing, and is read for its use of the lexer alone.
 * @param subpatterns The subpatterns declared.
 * @param context The enum, its variants, and what the file defines and binds.
 * @throws Spec_error If the pattern is refused, the callback's effect is not decidable from the source, or the callback
 *         moves the lexer.
 */
void add_rule(
        Lexer_spec& spec, const Definition& definition, const Pattern_kind kind, const std::optional<Variant>& variant,
        const Subpatterns_t& subpatterns, const Enum_context& context)
{
    const auto [expression, computed]{
            compile(definition.literal, kind, definition.folding, subpatterns, definition.line)};

    const auto emitted{Callback_reader{context, variant, definition.callback, definition.line}.token()};

    spec.rules.push_back(
            {.pattern = definition.literal.written,
             .expression = expression,
             .conditions = {},
             .action = definition.callback,
             .token = emitted,
             .priority = definition.priority.value_or(computed),
             .line = definition.line});
}

/**
 * @brief Reads the enum after its `enum` keyword into a specification: its generics, its `#[logos]` attributes, then
 *        its variants with their `#[token]` and `#[regex]` attributes, the variants all read before any callback is,
 *        since a callback's result may name any of them; a variant under a `#[cfg(...)]` false by its form is no rule,
 *        as stands_by_form() decides it, and what the reading assumed of a `cfg` or a `cfg_attr` the build alone
 *        decides is noted in the options, as note_assumed() has it.
 * @param cursor The cursor, just past `enum`.
 * @param attributes The enum's outer attributes, expanded as read_attributes() has them.
 * @param line The line of the derive naming Logos.
 * @param items What the file defines and binds, which a callback may name.
 * @param module The module the enum is declared in, as a path from the crate root, empty at the root.
 * @return The specification.
 * @throws Spec_error If the enum is malformed or left open, or an attribute, pattern or callback is refused.
 */
[[nodiscard]] Lexer_spec read_enum(
        Rust_cursor& cursor, const std::vector<Attribute>& attributes, const std::size_t line, const Items& items,
        const std::string_view module)
{
    Lexer_spec spec;

    spec.line = line;

    // Which language the classes were read as: `\d`, `\s` and `\w` are a Unicode version's, so the account of the
    // scanner names the one this reading modelled, the crate's own.
    spec.options.emplace_back("unicode-classes=" + std::string{unicode_classes_version()});

    // An enum under a `cfg` false by its form was passed over with the items and is no scanner; one under a predicate
    // the build alone decides is read as standing, and the options say so.
    for (const auto& attribute : attributes)
    {
        note_assumed(attribute, cursor, spec.options);
    }

    cursor.skip_trivia();

    const std::string enum_name{cursor.word()};

    if (enum_name.empty())
    {
        cursor.fail("expected the enum's name");
    }

    cursor.skip_trivia();

    const auto type_parameters{read_generics(cursor, line)};

    while (!cursor.done() && cursor.peek() != '{')
    {
        cursor.skip_token();
    }

    cursor.expect('{', "'{' to open the enum");

    const auto [subpatterns, skips, names]{read_logos_options(attributes, cursor, spec, items.names, module)};

    check_type_parameters(spec.options, type_parameters, line);

    std::vector<Read_variant> variants;

    for (cursor.skip_trivia(); !cursor.accept('}'); cursor.skip_trivia())
    {
        if (cursor.done())
        {
            cursor.fail("the enum is never closed");
        }

        if (auto variant{read_variant(cursor, spec.options, names, module)})
        {
            variants.push_back(std::move(*variant));
        }
    }

    std::vector<std::string> variant_names;

    for (const auto& [variant, rules] : variants)
    {
        variant_names.push_back(variant.name);
    }

    const Enum_context context{
            .name = enum_name,
            .variants = variant_names,
            .items = items,
            .names = names,
            .module = module};

    for (const auto& skip : skips)
    {
        add_rule(spec, skip, Pattern_kind::regex, std::nullopt, subpatterns, context);
    }

    for (const auto& [variant, rules] : variants)
    {
        for (const auto& [definition, kind] : rules)
        {
            add_rule(spec, definition, kind, variant, subpatterns, context);
        }
    }

    return spec;
}

} // namespace

std::vector<Lexer_spec> read_logos(const std::string_view source)
{
    // What the file defines and binds, the scanners among them, collected whole before any enum is read, since a
    // callback may name a function defined after the enum, and a name the file binds in the enum's scope.
    const auto items{collect_items(source)};

    std::vector<Lexer_spec> lexers;

    for (const auto& [offset, attributes, line, scope] : items.scanners)
    {
        Rust_cursor cursor{source, offset, source.size()};

        lexers.push_back(read_enum(cursor, attributes, line, items, scope));
    }

    return lexers;
}

} // namespace munch::tools::audit
