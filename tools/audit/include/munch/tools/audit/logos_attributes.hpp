#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_ATTRIBUTES_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_ATTRIBUTES_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/logos_regex.hpp"
#include "munch/tools/audit/rust_cursor.hpp"
#include "munch/tools/audit/rust_names.hpp"

/**
 * @brief The `#[...]` attributes of a Rust item as rustc expands them, Attribute, a `cfg_attr` into what it carries and
 *        a `cfg` by its form, read_attributes() and stands_by_form(), with what the reading assumed of the build,
 *        note_assumed(); and the attributes logos reads: a derive naming Logos, derives_logos(), the arguments of a
 *        `#[token]`, a `#[regex]` or a `skip(...)`, Definition and read_definition(), the entries of a `#[logos(...)]`,
 *        read_logos_attribute() and option_given(), and a variant's payload as logos takes it, variant_payload().
 *
 * What logos 0.15.1 refuses of an attribute is refused in the crate's words, so that a refusal reads as the error the
 * crate would give the file.
 */
namespace munch::tools::audit
{
/**
 * @brief What the option a `#[logos(type ...)]` entry records opens with, its key and the blank before the parameter it
 *        types, `type T=u8`.
 */
constexpr std::string_view type_key{"type "};

/**
 * @brief One `#[path(...)]` attribute: its path, where the text between its delimiters lies, and its line.
 */
struct Attribute
{
    /**
     * @brief The path, `derive`, `logos`, `token`, `regex` or another.
     */
    std::string path{};

    /**
     * @brief The offset of the first byte inside the delimiters, equal to `end` when there are none.
     */
    std::size_t begin{};

    /**
     * @brief The offset just past the last byte inside the delimiters.
     */
    std::size_t end{};

    /**
     * @brief Whether the path is followed by a delimited group, `#[path(...)]`, rather than nothing or `= value`.
     */
    bool delimited{};

    /**
     * @brief The line the attribute opens on.
     */
    std::size_t line{};

    /**
     * @brief The predicates the build alone decides that the attribute was applied under, `feature = "x"` for one
     *        written `#[cfg_attr(feature = "x", ...)]`, outermost first, each read as holding, which a scanner's
     *        options say, `cfg=` and the predicate; empty for an attribute written as it stands.
     */
    std::vector<std::string> assumed{};
};

/**
 * @brief What a `#[token(...)]`, a `#[regex(...)]` or a `skip(...)` says: the literal and the arguments after it.
 */
struct Definition
{
    /**
     * @brief The pattern's literal.
     */
    String_literal literal{};

    /**
     * @brief The callback's text as written, empty when there is none.
     */
    std::string callback{};

    /**
     * @brief The `priority = n` override, when given.
     */
    std::optional<std::size_t> priority{};

    /**
     * @brief Which folding an `ignore(...)` argument asked for.
     */
    Ignore_case folding{};

    /**
     * @brief The line the attribute opens on.
     */
    std::size_t line{};
};

/**
 * @brief Reads the outer attributes standing at the cursor, each expanded as expanded() has it, leaving the cursor at
 *        what they stand on.
 * @param cursor The cursor.
 * @param attributes The attributes, added to.
 * @throws Spec_error If an attribute is left open.
 */
void read_attributes(Rust_cursor& cursor, std::vector<Attribute>& attributes);

/**
 * @brief Returns whether an item carrying these attributes stands, by the form of its `#[cfg(...)]` predicates alone:
 *        one false whatever the build says, `any()` of nothing among them, strips the item and with it every name it
 *        would bind, as rustc strips it before a name is resolved. A predicate the build decides leaves the item
 *        standing, which note_assumed() records in a scanner's options.
 * @param attributes The attributes read for the item.
 * @param cursor The cursor over the file the attributes' offsets index.
 * @return True when the item stands.
 */
[[nodiscard]] bool stands_by_form(const std::vector<Attribute>& attributes, const Rust_cursor& cursor);

/**
 * @brief Notes among a scanner's options what the reading assumed to read an attribute: each predicate the build alone
 *        decides that a `cfg_attr` applied it under, and, for a `#[cfg(...)]`, its own such predicate, under which the
 *        item is read as standing; each as `cfg=` and the predicate, once, so that the account says what was assumed.
 * @param attribute The attribute.
 * @param cursor The cursor over the file the attribute's offsets index.
 * @param options The scanner's options, added to.
 * @throws Spec_error If a group in the predicate is left open.
 */
void note_assumed(const Attribute& attribute, const Rust_cursor& cursor, std::vector<std::string>& options);

/**
 * @brief Returns the line of the attribute among a list of outer attributes whose derive names Logos, bare or by path.
 * @param attributes The attributes.
 * @param cursor The cursor over the file the offsets index.
 * @return The line, or std::nullopt when none does.
 * @throws Spec_error If a derive's list is malformed or a block comment in it is left open.
 */
[[nodiscard]] std::optional<std::size_t> derives_logos(
        const std::vector<Attribute>& attributes, const Rust_cursor& cursor);

/**
 * @brief Reads the content of a `#[token(...)]`, a `#[regex(...)]` or a `skip(...)`: the literal, then a callback in
 *        first position or as `callback = ...`, `priority = n` and, on a token or a regex, `ignore(case)` or
 *        `ignore(ascii_case)`, comma separated, as logos reads them.
 *
 * What logos 0.15.1 refuses of the arguments is refused in its words (logos-codegen 0.15.1, parser/definition.rs,
 * parser/skip.rs and parser/nested.rs): a second `priority`, "Resetting previously set priority"; a second callback,
 * positional or named, "Callback has been already set"; `priority(...)` and `callback(...)`, which expect `= value`; an
 * argument logos does not know, an unknown nested attribute; and any argument after `ignore(...)`, since the crate
 * leaves the comma after the group unread and reads what follows as an unnamed argument out of place, "Expected a named
 * argument at this position".
 * @param content A cursor over the content.
 * @param attribute The attribute's name, `token`, `regex` or `skip`, for refusals.
 * @param skip Whether the content is a `skip(...)`'s, on which logos 0.15.1 knows no `ignore` and calls it an unknown
 *        nested attribute (logos-codegen 0.15.1, parser/skip.rs).
 * @return The definition.
 * @throws Spec_error If the literal is missing, an argument is one logos does not know or stands where logos refuses
 *         it, `ignore` names a flag logos has not got, or an argument is given twice.
 */
[[nodiscard]] Definition read_definition(Rust_cursor content, std::string_view attribute, bool skip);

/**
 * @brief Returns whether an option with a key, `extras`, `error`, `source` or `type S`, has been recorded already.
 * @param options The options recorded so far.
 * @param key The key, `type S` for a type parameter's assignment.
 * @return True when one has.
 */
[[nodiscard]] bool option_given(const std::vector<std::string>& options, const std::string& key);

/**
 * @brief Reads the content of one `#[logos(...)]`: its skips into the definitions, its subpatterns compiled in order
 *        into the specification's definitions and the table, and every other key into the options.
 *
 * What logos 0.15.1 refuses of the entries is refused in its words (logos-codegen 0.15.1, parser/mod.rs and
 * parser/nested.rs): a bare key, "Invalid nested attribute"; a key with a value of the wrong shape, `extras(T)`, `skip
 * = "x"` or `type = T`, each with the shape expected; and `extras`, `error`, `source` and the type of one parameter
 * given twice, across the enum's attributes as within one, "can be defined only once".
 * @param content A cursor over the content.
 * @param spec The specification being filled.
 * @param subpatterns The subpatterns declared so far, added to.
 * @param skips The skip definitions collected so far, added to.
 * @param names The names bound for the enum, which a `crate = path` entry binds the path to the crate in.
 * @param module The module the enum is declared in, as a path from the crate root, which that binding stands in.
 * @throws Spec_error If an entry is malformed, given twice where the crate takes one, a subpattern is declared twice or
 *         its name would read as a count.
 */
void read_logos_attribute(
        Rust_cursor content, Lexer_spec& spec, Subpatterns_t& subpatterns, std::vector<Definition>& skips,
        Names_t& names, std::string_view module);

/**
 * @brief Reads the payload of the variant at the cursor's group: the one field's type without its trivia.
 *
 * logos 0.15.1 takes a variant with one unnamed field or none, and refuses, whether the variant carries a pattern or
 * not, one with several, "Logos currently only supports variants with one field", and one with named fields, "Logos
 * doesn't support named fields yet" (logos-codegen 0.15.1, lib.rs).
 * @param cursor The cursor, at the `(` or `{` after the variant's name.
 * @return The payload's text.
 * @throws Spec_error If the group is left open, or the fields are several or named.
 */
[[nodiscard]] std::string variant_payload(Rust_cursor& cursor);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_ATTRIBUTES_HPP
