#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RUST_ITEMS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RUST_ITEMS_HPP

#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/logos_attributes.hpp"
#include "munch/tools/audit/rust_names.hpp"

/**
 * @brief What a Rust file defines and binds at any depth, Items, and the enums deriving Logos among its items, Scanner,
 *        collected in one walk over the file, collect_items().
 *
 * The walk enters every group a module, an impl or trait block, a function's body or a block stands for, and passes
 * over a macro's text, an attribute's content and an item a `cfg` strips by its form, so that what a callback names is
 * found wherever the file defines it and nothing is taken for an item that rustc never sees as one.
 */
namespace munch::tools::audit
{
/**
 * @brief An enum deriving `Logos`, as the walk over the file finds it: where it stands, its outer attributes, the line
 *        of the derive naming Logos and the scope it is declared in.
 */
struct Scanner
{
    /**
     * @brief The offset just past the `enum` keyword, where read_enum() starts.
     */
    std::size_t offset;

    /**
     * @brief The enum's outer attributes, the derive naming Logos and its `#[logos(...)]` attributes among them.
     */
    std::vector<Attribute> attributes;

    /**
     * @brief The line of the derive naming Logos, which names the scanner.
     */
    std::size_t line;

    /**
     * @brief The scope the enum is declared in, as a path from the crate root, empty at the root, which the names in
     *        its callbacks resolve in.
     */
    std::string scope;
};

/**
 * @brief What the file defines and binds, as far as reading a callback needs: its functions, its names, the type
 *        aliases among its items that take arguments, and the enums deriving `Logos` among its items.
 */
struct Items
{
    /**
     * @brief The functions, by their paths from the crate root.
     */
    Functions_t functions;

    /**
     * @brief The names bound, each by its spelling in the module binding it.
     */
    Names_t names;

    /**
     * @brief The generic type aliases, `type R<T> = ...`, by their paths from the crate root under `crate::`, as
     *        canonical() spells an item: an alias whose arguments the reading does not substitute, so a type written
     *        through one is out of sight.
     */
    std::set<std::string> generic_aliases;

    /**
     * @brief The names the file's `macro_rules!` items define, wherever they stand, since one of them shadows a
     *        standard macro of the same name where a callback invokes it.
     */
    std::set<std::string, std::less<>> macros;

    /**
     * @brief Whether the file invokes a macro of its own or a crate's at item level, `generate!();`, whose expansion
     *        may define a macro under any name, a standard one's included.
     */
    bool generated{false};

    /**
     * @brief The modules in which the file declares a module named `std` or `core`, `mod std { ... }`, as paths from
     *        the crate root, since a path through that name reaches the module before the crate of the same name from
     *        inside the declaring module and its descendants.
     */
    std::set<std::string> std_modules;

    /**
     * @brief The enums deriving `Logos`, in file order, each read once every item is collected, since a callback may
     *        name a function declared after the enum.
     */
    std::vector<Scanner> scanners;
};

/**
 * @brief Collects what the file defines and binds, at any depth: every `fn` item, its name, the return type written
 *        after `->` up to the body or a `where` clause, the body, the first parameter's name, the type of the `impl`
 *        block it is declared in, when it is, and the module it stands in; every `use`, `type` alias and `extern crate`
 *        binding; and the name of every struct, enum, union, trait, module, constant and static, bound to itself, an
 *        associated constant or type of an impl or trait block being the type's and bound nowhere; each binding in the
 *        scope it stands in, and exported where a `pub` stands before it; and every enum deriving `Logos`, where it
 *        stands, for read_enum() to read once the items are all collected. A block that is an expression, a function's
 *        body or a constant's initializer, is a scope of its own, entered as a `mod` block is, so an item declared
 *        inside one is bound there, in sight of what the block holds and of nothing outside; a macro's text, an
 *        invocation's or a `macro_rules!` body, is not entered; and an item under a `#[cfg(...)]` false by its form,
 *        `#[cfg(any())] pub fn f()`, is passed over whole, as rustc strips it before a name is resolved, so it binds
 *        and declares nothing, a `pub` before it notwithstanding, and holds no scanner.
 * @param source The file's text.
 * @return The items.
 * @throws Spec_error If a group or a literal is left open.
 */
[[nodiscard]] Items collect_items(std::string_view source);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RUST_ITEMS_HPP
