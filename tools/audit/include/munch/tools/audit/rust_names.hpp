#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RUST_NAMES_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RUST_NAMES_HPP

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief The names a Rust file binds, Names_t, each by the scope it is bound in and in the namespace it stands in,
 *        Namespace, types apart from values as Rust keeps them; the functions the file defines, Function and
 *        Functions_t; a name qualified by its module, qualified(), and bound there, bind_name() and bind_item(); a
 *        module's parent, parent(); a path from the crate root, rooted_path() and from_root(), and a block's mark,
 *        block_mark(); and a path as the file's bindings resolve it from a scope, canonical(), with every path of a
 *        type resolved, canonical_type().
 *
 * A binding is kept as written, with the module it is written in, and resolved when a name is looked up rather than
 * when it is bound, so that where it stands among the file's items does not matter, as it does not to Rust.
 */
namespace munch::tools::audit
{
/**
 * @brief What parts the segments of a path.
 */
constexpr std::string_view path_separator{"::"};

/**
 * @brief The path of the crate root.
 */
constexpr std::string_view crate_root{"crate"};

/**
 * @brief What a path from the crate root opens with.
 */
constexpr std::string_view crate_prefix{"crate::"};

/**
 * @brief A function the file defines, as far as a callback naming it needs: its return type and its body.
 */
struct Function
{
    /**
     * @brief The return type's text without its trivia, empty when the function returns `()`.
     */
    std::string returns{};

    /**
     * @brief The body's text between its braces, or std::nullopt for a declaration without one.
     */
    std::optional<std::string> body{};

    /**
     * @brief The name the first parameter binds, which is the lexer's when logos calls the function: the identifier of
     *        `lex`, `mut lex`, `ref lex` or `ref mut lex`; empty when the parameter is `_` or there is none; and
     *        std::nullopt when it is a pattern of any other shape, whose bindings the reading does not follow.
     */
    std::optional<std::string> parameter{};

    /**
     * @brief The type an enclosing `impl` block is for, which `Self` names in the function's return type and body: its
     *        path as the impl block's head writes it while the file's items are collected, and as canonical() spells it
     *        once they all are, `crate::m::T` for `impl m::T` at the root as for `impl T` inside `m`; empty where the
     *        function is not declared directly in an impl block or the block's type is not a path.
     */
    std::string self_type{};

    /**
     * @brief The scope the function is declared in, as a path from the crate root, `a::b` inside `mod a { mod b { ... }
     *        }`, a block's mark under it for a function inside another's body, and empty at the top, which the names in
     *        its return type resolve in, the type standing outside the body.
     */
    std::string module{};

    /**
     * @brief The scope the function's body is: the scope it is declared in with the mark of its own block under it,
     *        which the names the body writes resolve in, since a `use` or an item the body declares is bound there and
     *        nowhere above it, `use T::X as Skip;` making `Skip` the variant inside that body alone; empty for a
     *        declaration without a body.
     */
    std::string scope{};

    /**
     * @brief Where the body's first byte stands in the file, so that a block inside the body is named by the offset of
     *        its own brace, as the walk that bound its items named it; zero for a declaration without a body.
     */
    std::size_t body_at{0};
};

/**
 * @brief The functions the file defines, by path from the crate root: a free function's name in its module,
 *        `callbacks::drop_it`, and a method's through its type, `T::mark` or `a::T::mark`, whichever path its impl
 *        block spells the type by; several under one path when the file defines it more than once, a trait's
 *        declaration and an impl's definition among them.
 */
using Functions_t = std::map<std::string, std::vector<Function>, std::less<>>;

/**
 * @brief The namespaces Rust binds a name in, kept apart: types, where a module, a struct, an enum, a union, a trait, a
 *        type alias and a crate's name stand, and values, where a function, a constant, a static and the constructor of
 *        a unit or tuple struct stand; a variant stands in both, and a `use` binds its name in both, since the item it
 *        imports may stand in either. A type is read in the types' namespace and a result or a callback's path in the
 *        values', so that a `const Skip` beside `type Skip = logos::Skip` is the constant where a value stands and the
 *        crate's `Skip` where a type does, as Rust has it.
 */
enum class Namespace
{
    /**
     * @brief The types', where a module, a type, a trait, a type alias and a crate's name stand.
     */
    type,

    /**
     * @brief The values', where a function, a constant, a static and a unit or tuple struct's constructor stand.
     */
    value,
};

/**
 * @brief The type namespace alone, where an enum, a union, a trait, a module, a type alias or a crate's name stands.
 */
constexpr std::array type_namespace{Namespace::type};

/**
 * @brief The value namespace alone, where a function, a constant or a static stands.
 */
constexpr std::array value_namespace{Namespace::value};

/**
 * @brief Both namespaces, where a struct, a variant or a `use` import stands.
 */
constexpr std::array both_namespaces{Namespace::type, Namespace::value};

/**
 * @brief What a bound name stands for, as the file writes it where it binds the name. The path or type is resolved
 *        through the bindings of its own module when the name is looked up and not when it is bound, since a `use` or a
 *        `type` may name a binding or an item declared after it, `type D = Drop;` above `use logos::Skip as Drop;`,
 *        which Rust reads as it reads one declared before.
 */
struct Binding
{
    /**
     * @brief The path or type the name stands for, without trivia: the path a `use` imports, the type of a `type`
     *        alias, or the name's own path from the crate root under `crate::` for an item the file defines.
     */
    std::string path{};

    /**
     * @brief The module the binding is written in, as a path from the crate root, empty at the root, whose bindings the
     *        path resolves through.
     */
    std::string module{};

    /**
     * @brief Whether the binding is declared `pub`, in any of its forms, `pub(crate)` and `pub(super)` among them, so
     *        that a glob of its module from outside that module brings it in; a private one is seen only from inside
     *        the module and the modules under it, as Rust has it.
     */
    bool exported{};
};

/**
 * @brief A name's bindings in a module, one per namespace, so that a `const Skip` and a `type Skip` stand side by side,
 *        the constant read where a value is written and the alias where a type is.
 */
struct Bindings
{
    /**
     * @brief Returns the binding in a namespace.
     * @param space The namespace.
     * @return The binding, or none.
     */
    [[nodiscard]] std::optional<Binding>& in(Namespace space);

    /**
     * @brief Returns the binding in a namespace.
     * @param space The namespace.
     * @return The binding, or none.
     */
    [[nodiscard]] const std::optional<Binding>& in(Namespace space) const;

    /**
     * @brief The binding in the type namespace, or none.
     */
    std::optional<Binding> type{};

    /**
     * @brief The binding in the value namespace, or none.
     */
    std::optional<Binding> value{};
};

/**
 * @brief The names a file binds, each qualified by the module it is bound in, `a::Skip` for a binding inside `mod a`
 *        and `Skip` for one at the crate root, to what they stand for in each namespace: a `use` binding to the path it
 *        imports, a `type` alias to its type, an item the file defines, a struct, an enum, a module or a function among
 *        them, to its own path from the root under `crate::`, and a path `#[logos(crate = ...)]` names, as `extern
 *        crate logos as lx` its name `lx`, to the crate by its absolute path, `::logos`; each as written, with the
 *        module it is written in, for canonical() to resolve through the table when the name is looked up, and whether
 *        it is declared `pub`, which a glob of its module from outside asks. A binding is visible in its own module and
 *        nowhere else, as Rust has it, so a name bound only inside `mod a` is no binding at the root and one bound at
 *        the root none inside `mod a`, a glob bringing in what the importing module may see. A block, a function's body
 *        or a constant's initializer, is a scope of its own under the module it stands in, its path the module's with
 *        the block's mark, as block_mark() spells it: a name bound in it is seen from inside the block and the blocks
 *        within it, and the block sees the module's names. The crate's names, `Skip`, `Filter`, `FilterResult` and
 *        `skip`, are the crate's wherever the file binds them no other way, since a bare one reaches a callback only
 *        through an import the reading may not see, `use logos::*` among them; and the prelude's `Result` and `Option`
 *        are spelled bare however the file reaches them, by their paths in `std` or `core` or an import of those.
 */
using Names_t = std::map<std::string, Bindings, std::less<>>;

/**
 * @brief Returns a name as a module qualifies it: `a::name` in the module `a`, the name itself at the crate root.
 * @param module The module's path from the crate root, empty at the root.
 * @param name The name, or a path.
 * @return The qualified name.
 */
[[nodiscard]] std::string qualified(std::string_view module, std::string_view name);

/**
 * @brief Returns a path's last segment, the name of what it names: `f` for `m::f`, the path itself for a bare name.
 * @param path The path.
 * @return The segment.
 */
[[nodiscard]] std::string_view last_segment(std::string_view path);

/**
 * @brief Returns whether a module stands in a scope or under it: the scope itself or a module inside it, and every
 *        module when the scope is the crate root.
 * @param module The module's path from the crate root.
 * @param scope The scope's path from the crate root, empty for the root.
 * @return True when it does.
 */
[[nodiscard]] bool is_within(std::string_view module, std::string_view scope);

/**
 * @brief Returns the module a module stands in: `a` for `a::b`, empty for `a` and for the root.
 * @param module The module's path from the crate root.
 * @return The path of the one above.
 */
[[nodiscard]] std::string parent(std::string_view module);

/**
 * @brief Returns a path from the crate root as canonical() spells it, under `crate::`.
 * @param path The path from the root, without the prefix.
 * @return The path under the prefix.
 */
[[nodiscard]] std::string rooted_path(std::string_view path);

/**
 * @brief Returns a path under `crate::` as the path from the crate root, without the prefix, and any other path as it
 *        stands.
 * @param path The path.
 * @return The path without the prefix.
 */
[[nodiscard]] std::string_view from_root(std::string_view path) noexcept;

/**
 * @brief Returns the mark a block is named by among the scopes, its brace's offset in braces, as the walk marks every
 *        block it enters.
 * @param brace The offset of the block's `{`.
 * @return The mark.
 */
[[nodiscard]] std::string block_mark(std::size_t brace);

/**
 * @brief Returns a path as the file's bindings resolve it from a module: `crate::` starts it at the root, `self::` in
 *        the module itself and `super::` in the one above, one of the three alone naming that module by its own path
 *        from the root, `crate` for the root, the longest prefix the module binds is replaced by what it stands for,
 *        read in the module binding it, and again on the result, so that `lx::Skip` under `use logos as lx` and `Drop`
 *        under `use logos::Skip as Drop` are both `logos::Skip`, `std::result::Result` and `R` under `use
 *        core::result::Result as R` are both `Result`, and an item the file defines is `crate::` and its path from the
 *        root, `crate::a::Skip` for the `struct Skip` of `mod a`, named as `Skip` inside that module and as `a::Skip`
 *        at the root. A leading `::` makes the path an external crate's, as Rust 2018 and later have it, which follows
 *        the root's bindings that name a crate alone by its absolute path, `::logos`: `::lx::Skip` under `extern crate
 *        logos as lx` is `logos::Skip`, and `::logos::Skip` is the crate's beside a `mod logos` of the file's, which
 *        `crate::logos::Skip` names.
 *
 * An item the file defines stands for itself, its path taken from the scope the sighting names rather than the one the
 * binding is written in, which for a variant is the enum: reading `T::X` against the enum's own scope would ask whether
 * the binding is `crate::T::T::X`, which no item is, and read the path again as itself, once per pass a chain is
 * allowed and once more inside each, for a name that was already canonical.
 * @param names The file's bindings.
 * @param path The path without trivia.
 * @param module The module the path is written in, as a path from the crate root, empty at the root.
 * @param space The namespace the path names in, the types' for a type and the values' for a result or a callback's
 *        path, which its last segment is read in; a prefix names a module, a type or an enum, in the types'.
 * @param depth The bindings followed to reach the path, none for a path the file's text writes: each binding found is
 *        read in its own module one deeper, and a chain past the few bindings followed, a cycle, is left as it stands.
 * @return The canonical path.
 */
[[nodiscard]] std::string canonical(
        const Names_t& names, std::string path, std::string module, Namespace space, std::size_t depth = 0);

/**
 * @brief Returns whether a canonical path is the file's own, `crate` for the root or a module's or item's under
 *        `crate::`, rather than a crate's.
 * @param path The canonical path.
 * @return Whether the file defines what it names.
 */
[[nodiscard]] bool files_own(std::string_view path);

/**
 * @brief Returns a type's text with every path in it resolved by canonical(), the rest kept as written.
 * @param names The file's bindings.
 * @param text The type without trivia.
 * @param module The module the type is written in, as a path from the crate root, empty at the root.
 * @param space The namespace the paths name in, the types' for a type, and the one a binding's path is followed in.
 * @param depth The bindings followed to reach the type, as canonical() counts them, none for a type the file's text
 *        writes.
 * @return The canonical type.
 */
[[nodiscard]] std::string canonical_type(
        const Names_t& names, std::string_view text, std::string_view module, Namespace space, std::size_t depth = 0);

/**
 * @brief Binds an item the file defines in a module, a struct, an enum, a module or a function, to its own path from
 *        the crate root, which is canonical as it stands.
 * @param names The bindings, added to.
 * @param module The module the item stands in, as a path from the crate root, empty at the root.
 * @param name The item's name.
 * @param exported Whether the item is declared `pub`.
 * @param spaces The namespaces the item stands in.
 */
void bind_item(
        Names_t& names, std::string_view module, const std::string& name, bool exported,
        std::span<const Namespace> spaces);

/**
 * @brief Binds a name in a module to a path or type as written, which canonical() resolves through that module's
 *        bindings when the name is looked up, so that where the binding stands among the file's items does not matter.
 * @param names The bindings, added to.
 * @param module The module the binding stands in, as a path from the crate root, empty at the root.
 * @param name The name as written.
 * @param path The path or type it stands for, without trivia.
 * @param exported Whether the binding is declared `pub`.
 * @param spaces The namespaces the name stands in.
 */
void bind_name(
        Names_t& names, std::string_view module, const std::string& name, const std::string& path, bool exported,
        std::span<const Namespace> spaces);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RUST_NAMES_HPP
