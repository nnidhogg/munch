#include "munch/tools/audit/rust_items.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/rust_cursor.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements rust_items.hpp: the walk over the file, Item_collector, with a glob import, a declared function and an
// open delimiter as it holds them, the skips over an item and a macro's text, an impl block's type, a `use` tree, the
// variants an enum binds, a function's first parameter and the globs followed are private to this unit.

/**
 * @brief A glob import, `use path::*;`, which brings the bindings of the module it names that the importing module may
 *        see into the module it stands in; followed once the file's items are all collected, since it brings in items
 *        declared after it too.
 */
struct Glob
{
    /**
     * @brief The module the import stands in, as a path from the crate root, empty at the root.
     */
    std::string module;

    /**
     * @brief The path before the `*`, without trivia, `super` for `use super::*;`.
     */
    std::string path;

    /**
     * @brief Whether the import is declared `pub`, `pub use path::*;`, so that what it brings in is exported from the
     *        module it stands in.
     */
    bool exported;
};

/**
 * @brief One open delimiter of the file, and what its brace opens where it opens a block.
 */
struct Opened
{
    /**
     * @brief The impl block's type for the brace of one, its path as impl_type() reads it, empty otherwise.
     */
    std::string impl_type;

    /**
     * @brief Whether the delimiter is the brace of an impl or trait block, whose `type` items are associated types.
     */
    bool associated;

    /**
     * @brief What the brace adds to the path of the scope it opens: a `mod` block's name; nothing for an impl or trait
     *        block, whose items are the type's; and for any other brace, a function's body, a constant's initializer or
     *        a block inside either, the block's own mark, `{` and the brace's byte offset and `}`, which no Rust path
     *        can spell, since such a block is a scope of its own. Empty for a parenthesis or a bracket, which open no
     *        scope.
     */
    std::string segment;
};

/**
 * @brief A function as the walk declares it, before the path it is reached by is known: its name and what it is.
 */
struct Declared_function
{
    /**
     * @brief The function's name.
     */
    std::string name;

    /**
     * @brief The function.
     */
    Function function;
};

/**
 * @brief Brings the bindings of the module one glob imports from into the module importing it, as follow_globs() has
 *        it.
 * @param glob The glob import.
 * @param names The bindings, added to.
 * @return Whether it brought in a binding, or exported one standing, that was not there before.
 */
[[nodiscard]] bool follow_glob(const Glob& glob, Names_t& names)
{
    const auto& [module, path, exported]{glob};

    const auto resolved{canonical(names, path, module, Namespace::type)};

    // The module named as a path from the root: `crate::m` for a module of the file's, `crate` for the root.
    if (!files_own(resolved))
    {
        return false;
    }

    const auto target{resolved == "crate" ? std::string{} : resolved.substr(7)};

    // The importing module sees everything of a module it stands in or under.
    const auto within{is_within(module, target)};

    auto brought{false};

    for (const auto& [key, bindings] : Names_t{names})
    {
        const auto cut{key.rfind("::")};

        const auto owner{cut == std::string::npos ? std::string_view{} : std::string_view{key}.substr(0, cut)};

        const auto name{cut == std::string::npos ? key : key.substr(cut + 2)};

        if (owner != target)
        {
            continue;
        }

        for (const auto space : both_namespaces)
        {
            const auto& value{bindings.in(space)};

            if (!value || !(within || value->exported))
            {
                continue;
            }

            auto& standing{names[qualified(module, name)].in(space)};

            if (!standing)
            {
                standing = Binding{.path = value->path, .module = value->module, .exported = exported};

                brought = true;

                continue;
            }

            // The binding standing under the name shadows the glob's unless it is the same one, brought in by another
            // glob of the module, `use crate::a::*;` beside `pub use crate::a::*;`: one binding then, exported where
            // either glob is.
            if (standing->path == value->path && standing->module == value->module && exported && !standing->exported)
            {
                standing->exported = true;

                brought = true;
            }
        }
    }

    return brought;
}

/**
 * @brief Reads a path in an `impl` block's head, each segment's generics skipped: what a method's path and
 *        `Self::Variant` are read against, resolved through the block's module once the file's bindings are all
 *        collected.
 * @param cursor The cursor, at the path, left after it.
 * @return The segments joined by `::`, without trivia and generics.
 * @throws Spec_error If the generics are left open.
 */
[[nodiscard]] std::string impl_path(Rust_cursor& cursor)
{
    std::string segments;

    for (;;)
    {
        cursor.skip_trivia();

        segments += cursor.word();

        cursor.skip_trivia();

        if (cursor.peek() == '<')
        {
            skip_generics(cursor);

            cursor.skip_trivia();
        }

        if (!cursor.at("::"))
        {
            return segments;
        }

        cursor.expect(':', "':'");
        cursor.expect(':', "':'");

        segments += "::";
    }
}

/**
 * @brief Brings the bindings of every module a glob imports from into the module importing it, where that module is one
 *        the file defines, as far as the importing module may see them, as Rust has it: every binding of a module the
 *        importing one stands in or under, `use super::*;` inside `mod m` bringing the root's into `m`, private ones
 *        included, and the `pub` bindings alone of any other module, `use self::m::*` or `use crate::m::*` at the root
 *        bringing `m`'s into the root and `use crate::a::*` inside `mod b` bringing `a`'s into `b`; each brought in as
 *        the glob is declared, exported by `pub use` and private otherwise, and a binding the importing module has
 *        already standing, as Rust's own, shadows a glob's, unless it is the same binding brought in by another glob of
 *        that module, `use crate::a::*;` beside `pub use crate::a::*;`, which is one binding, exported where either
 *        glob is. A glob of a module the file does not define, `use logos::*`, brings in nothing the reading can name,
 *        and stays as it is.
 * @param globs The glob imports.
 * @param names The bindings, added to.
 */
void follow_globs(const std::vector<Glob>& globs, Names_t& names)
{
    // A glob's module may itself be filled by a glob, so the passes run until none brings in anything more.
    for (auto brought{true}; brought;)
    {
        brought = false;

        for (const auto& glob : globs)
        {
            brought = follow_glob(glob, names) || brought;
        }
    }
}

/**
 * @brief Passes over the rest of the item an attribute stands on, its first word read already: the tokens up to a
 *        semicolon, which is taken, `struct S;` and `use a::b;` ending there, or through a brace group, `fn f() { ...
 *        }` and `mod m { ... }` ending with their bodies, or up to a closing delimiter, which is left, where the
 *        attribute stands on a field, an arm or an argument inside a group.
 * @param cursor The cursor, just past the item's first word.
 * @throws Spec_error If a group or a literal is left open.
 */
void skip_item(Rust_cursor& cursor)
{
    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        if (cursor.accept(';') || cursor.peek() == ')' || cursor.peek() == ']' || cursor.peek() == '}')
        {
            return;
        }

        const auto body{cursor.peek() == '{'};

        cursor.skip_token();

        if (body)
        {
            return;
        }
    }
}

/**
 * @brief Skips the macro invocation the word just read begins, where it begins one: `name!` and the group after it,
 *        `println!(...)` or `macro_rules! name { ... }`, whose content is text for the macro and no item of the file's,
 *        so that an enum or a struct written there is none until the macro is expanded, which the reading does not do.
 * @param cursor The cursor, just past the word.
 * @param word The word just read.
 * @return True when an invocation was skipped, the cursor past it; false with the cursor where it stood.
 * @throws Spec_error If the group is left open.
 */
[[nodiscard]] bool skip_macro_invocation(Rust_cursor& cursor, const std::string_view word)
{
    if (word.empty())
    {
        return false;
    }

    Rust_cursor look{cursor};

    look.skip_trivia();

    // `a != b` compares.
    if (!look.accept('!') || look.peek() == '=')
    {
        return false;
    }

    look.skip_trivia();

    // `macro_rules! name` names the macro it defines before its body.
    if (word == "macro_rules")
    {
        std::ignore = look.word();

        look.skip_trivia();
    }

    if (look.peek() != '(' && look.peek() != '[' && look.peek() != '{')
    {
        return false;
    }

    look.skip_group();

    cursor = look;

    return true;
}

/**
 * @brief Reads the type an `impl` block is for, `impl Type`, `impl Trait for Type`, either with generics and a where
 *        clause, leaving the cursor at the block's brace.
 * @param cursor The cursor, just past `impl`.
 * @return The type's path as written, without trivia and with each segment's generics dropped, `m::T` for `impl
 *         m::T<'a>`, or empty when the type is not a path.
 * @throws Spec_error If a group or a literal is left open.
 */
[[nodiscard]] std::string impl_type(Rust_cursor& cursor)
{
    cursor.skip_trivia();

    if (cursor.peek() == '<')
    {
        skip_generics(cursor);
    }

    auto type{impl_path(cursor)};

    // `impl Trait for Type`: the path read so far was the trait's.
    if (Rust_cursor look{cursor}; look.word() == "for")
    {
        cursor = look;

        type = impl_path(cursor);
    }

    while (!cursor.done() && cursor.peek() != '{' && cursor.peek() != ';')
    {
        cursor.skip_token();
    }

    return type;
}

/**
 * @brief Reads the `use` tree at the cursor and binds every name it brings in: `a::b::C` binds `C`, `a::b::C as D`
 *        binds `D`, `a::{B, c::D}` each of its branches under `a`, `a::b::{self}` binds `b`, and a glob is noted for
 *        the bindings of the module it names to be brought in once the file's items are all collected.
 * @param cursor The cursor, at the tree's first segment or brace.
 * @param prefix The path the tree stands under, `a::` for its branches, empty at the top.
 * @param module The module the `use` stands in, as a path from the crate root, empty at the root.
 * @param exported Whether the `use` is declared `pub`, which every name it binds is then.
 * @param names The bindings, added to.
 * @param globs The glob imports, added to.
 * @throws Spec_error If a brace is left open.
 */
void read_use_tree(
        Rust_cursor& cursor, const std::string& prefix, const std::string_view module, const bool exported,
        Names_t& names, std::vector<Glob>& globs)
{
    cursor.skip_trivia();

    if (cursor.peek() == '{')
    {
        const auto open{cursor.offset()};

        cursor.skip_group();

        auto branch{cursor.inside(open + 1, cursor.offset() - 1)};

        for (branch.skip_trivia(); !branch.done(); branch.skip_trivia())
        {
            read_use_tree(branch, prefix, module, exported, names, globs);

            branch.skip_trivia();

            if (!branch.accept(','))
            {
                break;
            }
        }

        return;
    }

    if (cursor.accept('*'))
    {
        globs.push_back(
                {.module = std::string{module},
                 .path = prefix.ends_with("::") ? prefix.substr(0, prefix.size() - 2) : prefix,
                 .exported = exported});

        return;
    }

    std::string path{prefix};

    std::string last;

    for (;;)
    {
        if (cursor.at("::"))
        {
            cursor.expect(':', "':'");
            cursor.expect(':', "':'");

            path += "::";
        }

        cursor.skip_trivia();

        if (cursor.peek() == '{' || cursor.peek() == '*')
        {
            read_use_tree(cursor, path, module, exported, names, globs);

            return;
        }

        last = std::string{cursor.word()};

        if (last.empty())
        {
            return;
        }

        path += last;

        cursor.skip_trivia();

        if (!cursor.at("::"))
        {
            break;
        }
    }

    // `self` at the end names the module before it.
    if (last == "self")
    {
        path.erase(path.size() - 6);

        last = last_segment(path);
    }

    if (Rust_cursor look{cursor}; look.word() == "as")
    {
        look.skip_trivia();

        last = std::string{look.word()};

        cursor = look;
    }

    if (!last.empty() && last != "_")
    {
        bind_name(names, module, last, path, exported, both_namespaces);
    }
}

/**
 * @brief The scope the open delimiters stand in, as a path from the crate root: the segments of the `mod` blocks and
 *        the blocks among them joined by `::`, empty at the root.
 * @param opened The open delimiters, outermost first.
 * @return The scope's path.
 */
[[nodiscard]] std::string scope_of(const std::vector<Opened>& opened)
{
    std::string scope;

    for (const auto& [impl_type, associated, segment] : opened)
    {
        if (!segment.empty())
        {
            scope = qualified(scope, segment);
        }
    }

    return scope;
}

/**
 * @brief Binds the variants of an enum's body as items of the enum, `T::Skip` for the `Skip` of `enum T`, each as
 *        visible as the enum, so that `use T::*` brings them in and `use T::Skip` names one, as Rust has it; the body
 *        is passed over whole, a variant's attributes, payload and discriminant declaring nothing.
 * @param cursor The cursor, at the body's brace, left after it.
 * @param names The bindings, added to.
 * @param module The module the enum stands in, as a path from the crate root, empty at the root.
 * @param name The enum's name.
 * @throws Spec_error If the body or a group in it is left open.
 */
void bind_variants(Rust_cursor& cursor, Names_t& names, const std::string_view module, const std::string& name)
{
    const auto open{cursor.offset()};

    cursor.skip_group();

    auto body{cursor.inside(open + 1, cursor.offset() - 1)};

    for (body.skip_trivia(); !body.done(); body.skip_trivia())
    {
        std::vector<Attribute> attributes;

        read_attributes(body, attributes);

        const std::string variant{body.word()};

        if (!variant.empty() && stands_by_form(attributes, body))
        {
            bind_item(names, qualified(module, name), variant, true, both_namespaces);
        }

        // The payload and the discriminant, up to the comma after the variant.
        for (body.skip_trivia(); !body.done() && !body.accept(','); body.skip_trivia())
        {
            body.skip_token();
        }
    }
}

/**
 * @brief The name a function's first parameter binds, which is the lexer's when logos calls the function, the
 *        attributes before it stepped over: a binding, `lex`, `mut lex`, `ref lex` or `ref mut lex`, names the lexer,
 *        and a binding is followed by the colon of its type, or by nothing where the function takes no parameter.
 * @param first A cursor over the text between the parameter list's parentheses.
 * @return The name; empty for `_`, which binds none; std::nullopt for a pattern of any other shape, which is left
 *         unresolved.
 * @throws Spec_error If an attribute's group is left open.
 */
[[nodiscard]] std::optional<std::string> first_parameter(Rust_cursor first)
{
    for (first.skip_trivia(); at_attribute(first); first.skip_trivia())
    {
        std::ignore = first.next("'#'");

        first.skip_group();
    }

    auto binding{std::string{first.word()}};

    if (binding == "ref")
    {
        first.skip_trivia();

        binding = std::string{first.word()};
    }

    if (binding == "mut")
    {
        first.skip_trivia();

        binding = std::string{first.word()};
    }

    first.skip_trivia();

    if (first.done() || first.peek() == ':')
    {
        return binding == "_" ? std::string{} : std::move(binding);
    }

    return std::nullopt;
}

/**
 * @brief The walk over a file that collects what it defines and binds, item by item, each kind of item read by a member
 *        of its own, with the open delimiters it stands in and what the next brace and the next item take.
 */
class Item_collector
{
public:
    /**
     * @brief Binds the walk to a file.
     * @param source The file's text.
     */
    explicit Item_collector(std::string_view source);

    /**
     * @brief Walks the whole file, then follows its glob imports and indexes its functions by path.
     * @return The items.
     * @throws Spec_error If a group or a literal is left open.
     */
    [[nodiscard]] Items collect();

private:
    /**
     * @brief Reads what stands at the cursor, one token, attribute, visibility or item, by its first word.
     * @throws Spec_error If a group or a literal is left open.
     */
    void step();

    /**
     * @brief Passes over an inner attribute, `#![...]`, which is no item's.
     * @throws Spec_error If its group is left open.
     */
    void skip_inner_attribute();

    /**
     * @brief Reads a visibility after its `pub`: `pub`, `pub(crate)`, `pub(super)` and `pub(in path)` alike export the
     *        item they stand before, `pub(self)` and `pub(in self)` are private, as Rust has it, and the attributes
     *        before the modifier are the item's too.
     * @throws Spec_error If its group is left open.
     */
    void read_visibility();

    /**
     * @brief Notes the name a `macro_rules!` defines, since a callback invoking a standard macro's name invokes the
     *        file's own where the file defines one.
     * @throws Spec_error If a block comment is left open.
     */
    void note_macro_name();

    /**
     * @brief Reads an `impl` block's head after its `impl`, its type waiting for the brace it opens; an `impl Trait`
     *        elsewhere, in a type alias say, opens no block.
     * @throws Spec_error If a group or a literal is left open.
     */
    void read_impl();

    /**
     * @brief Reads a `use` after its `use`, binding what its tree brings in.
     * @throws Spec_error If a brace is left open.
     */
    void read_use();

    /**
     * @brief Reads what follows an `extern`: `extern crate logos as lx;` binds `lx` to the crate, `::logos`; anything
     *        else is a block or a declaration, read on by the walk.
     * @throws Spec_error If a block comment is left open.
     */
    void read_extern();

    /**
     * @brief Reads a `type` alias after its `type`, binding its name to the type it stands for; a generic alias stands
     *        for no one type, and its name is bound to itself and noted, so that a type written through it is refused
     *        as one the reading does not follow.
     * @throws Spec_error If a group or a literal is left open.
     */
    void read_type_alias();

    /**
     * @brief Reads a `const` or `static` item after its keyword, `const NAME: Type = expr;` or `static mut NAME: Type =
     *        expr;`, which has its type's colon after the name; a `const fn` and a `*const T` are read on by what
     *        follows the keyword. The initializer, a block among them, is left for the walk to enter as the block it
     *        is.
     * @throws Spec_error If a block comment is left open.
     */
    void read_constant();

    /**
     * @brief Reads a struct, an enum, a union, a trait or a module after its keyword: its name bound, an enum deriving
     *        `Logos` noted as a scanner, an enum's variants bound, and a `mod` block's name waiting for its brace.
     * @param word The keyword.
     * @param attributes The item's outer attributes, which say whether an enum derives `Logos`.
     * @throws Spec_error If a group or a literal is left open.
     */
    void read_named_item(std::string_view word, const std::vector<Attribute>& attributes);

    /**
     * @brief Reads a function after its `fn`: its name bound in its module, a method's reached through its type, and
     *        its first parameter, its return type written after `->` up to the body or a `where` clause, and its body
     *        kept for a callback naming it; a function pointer type, `fn(u8) -> u8`, names nothing.
     * @throws Spec_error If a group or a literal is left open.
     */
    void read_function();

    /**
     * @brief Reads a function's return type after its parameters, written after `->` up to the body, a `where` clause
     *        or the semicolon of a declaration.
     * @return The type without its trivia, empty when none is written.
     * @throws Spec_error If a group or a literal is left open.
     */
    [[nodiscard]] std::string return_type();

    /**
     * @brief Whether the keyword `where` stands at the cursor, as a whole word.
     * @return True when it does.
     */
    [[nodiscard]] bool at_where() const;

    /**
     * @brief Reads a byte no word begins at: a delimiter opening or closing a group, or any other byte, after which a
     *        `pub` before it, a field's, is no item's.
     */
    void read_punctuation();

    /**
     * @brief Opens a group at its delimiter, descended into rather than skipped, so that a function inside a module or
     *        an impl block is found too: a brace opens the `mod` block named for it, an impl or trait block, or a block
     *        of its own, marked by where it stands.
     */
    void open_group();

    /**
     * @brief Indexes the functions declared: a method is reached through its type, by the type's path from the root as
     *        the bindings its impl block sees resolve it, `m::T::mark` for `impl m::T` at the root as for `impl T`
     *        inside `m`, and a free function by its name in its module.
     */
    void index_functions();

    /**
     * @brief The file's text.
     */
    std::string_view source_;

    /**
     * @brief The cursor the walk reads the file by.
     */
    Rust_cursor cursor_;

    /**
     * @brief What the walk has collected.
     */
    Items items_;

    /**
     * @brief The open delimiters, outermost first.
     */
    std::vector<Opened> opened_;

    /**
     * @brief The glob imports, followed once every item is collected.
     */
    std::vector<Glob> globs_;

    /**
     * @brief The functions declared, each with its name, indexed once every item is collected, since a method's type is
     *        resolved through bindings its impl block may precede.
     */
    std::vector<Declared_function> declared_;

    /**
     * @brief What the brace next to open opens: the impl block's type, whether an impl or trait block, and the module's
     *        name for a `mod` block, as its segment.
     */
    Opened next_brace_{.impl_type = {}, .associated = false, .segment = {}};

    /**
     * @brief The outer attributes read for the item next declared.
     */
    std::vector<Attribute> attributes_;

    /**
     * @brief Whether a `pub`, in any of its forms, stands before the item next declared, which takes it.
     */
    bool exported_{false};
};

Item_collector::Item_collector(const std::string_view source) : source_{source}, cursor_{source, 0, source.size()}
{}

Items Item_collector::collect()
{
    for (cursor_.skip_trivia(); !cursor_.done(); cursor_.skip_trivia())
    {
        step();
    }

    follow_globs(globs_, items_.names);

    index_functions();

    return std::move(items_);
}

void Item_collector::step()
{
    if (cursor_.at_string())
    {
        cursor_.skip_token();

        return;
    }

    // An attribute's content is no item, `#[logos(type S = &str)]` among them; an outer one is kept, since a
    // `#[cfg(...)]` false by its form strips the item it stands on and every name that item would bind.
    if (at_attribute(cursor_))
    {
        read_attributes(cursor_, attributes_);

        return;
    }

    if (cursor_.at("#!["))
    {
        skip_inner_attribute();

        return;
    }

    const auto word{cursor_.word()};

    if (word == "pub")
    {
        read_visibility();

        return;
    }

    const auto attributes{std::exchange(attributes_, {})};

    // An item under a `#[cfg(...)]` false by its form is gone before rustc resolves a name, and passed over whole: it
    // binds nothing, declares nothing, and a `pub` before it exports nothing.
    if (!stands_by_form(attributes, cursor_))
    {
        skip_item(cursor_);

        exported_ = false;

        return;
    }

    if (word == "macro_rules")
    {
        note_macro_name();
    }

    // A macro's text declares nothing until it is expanded; an invocation at item level of any macro but a standard one
    // may expand to a `macro_rules!` under any name, so the standard names are trusted in no callback of the file's
    // after it.
    if (skip_macro_invocation(cursor_, word))
    {
        // Any invocation at item level, one of a file-defined `assert!` among them, may expand to a definition.
        if (word != "macro_rules")
        {
            items_.generated = true;
        }

        return;
    }

    if (word == "impl")
    {
        read_impl();

        return;
    }

    if (word == "trait")
    {
        next_brace_.associated = true;
    }

    if (word == "use")
    {
        read_use();

        return;
    }

    if (word == "extern")
    {
        read_extern();

        return;
    }

    if (word == "type" && (opened_.empty() || !opened_.back().associated))
    {
        read_type_alias();

        return;
    }

    if (word == "const" || word == "static")
    {
        read_constant();

        return;
    }

    if (word == "struct" || word == "enum" || word == "union" || word == "trait" || word == "mod")
    {
        read_named_item(word, attributes);

        return;
    }

    if (word == "fn")
    {
        read_function();

        return;
    }

    if (word.empty())
    {
        read_punctuation();
    }
}

void Item_collector::skip_inner_attribute()
{
    while (cursor_.peek() != '[')
    {
        std::ignore = cursor_.next("'['");
    }

    cursor_.skip_group();
}

void Item_collector::read_visibility()
{
    cursor_.skip_trivia();

    std::string scope;

    if (cursor_.peek() == '(')
    {
        const auto open{cursor_.offset()};

        cursor_.skip_group();

        auto inside{cursor_.inside(open + 1, cursor_.offset() - 1)};

        inside.skip_trivia();

        scope = std::string{inside.word()};

        if (scope == "in")
        {
            inside.skip_trivia();

            scope = std::string{inside.word()};
        }
    }

    exported_ = scope != "self";
}

void Item_collector::note_macro_name()
{
    Rust_cursor look{cursor_};

    look.skip_trivia();

    if (!look.accept('!'))
    {
        return;
    }

    look.skip_trivia();

    if (const auto name{look.word()}; !name.empty())
    {
        items_.macros.emplace(name);
    }
}

void Item_collector::read_impl()
{
    auto type{impl_type(cursor_)};

    next_brace_.impl_type = cursor_.peek() == '{' ? std::move(type) : std::string{};

    next_brace_.associated = cursor_.peek() == '{';
}

void Item_collector::read_use()
{
    read_use_tree(cursor_, {}, scope_of(opened_), std::exchange(exported_, false), items_.names, globs_);
}

void Item_collector::read_extern()
{
    Rust_cursor look{cursor_};

    look.skip_trivia();

    if (look.word() != "crate")
    {
        return;
    }

    look.skip_trivia();

    const std::string crate{look.word()};

    look.skip_trivia();

    std::string name{crate};

    if (Rust_cursor rename{look}; rename.word() == "as")
    {
        rename.skip_trivia();

        name = std::string{rename.word()};

        look = rename;
    }

    cursor_ = look;

    const auto exported{std::exchange(exported_, false)};

    if (!crate.empty() && !name.empty() && name != "_")
    {
        bind_name(items_.names, scope_of(opened_), name, "::" + crate, exported, type_namespace);
    }
}

void Item_collector::read_type_alias()
{
    cursor_.skip_trivia();

    const std::string name{cursor_.word()};

    const auto exported{std::exchange(exported_, false)};

    cursor_.skip_trivia();

    const auto generic{cursor_.peek() == '<'};

    while (!cursor_.done() && cursor_.peek() != '=' && cursor_.peek() != ';' && cursor_.peek() != '{')
    {
        cursor_.skip_token();
    }

    if (!cursor_.accept('='))
    {
        return;
    }

    const auto begin{cursor_.offset()};

    while (!cursor_.done() && cursor_.peek() != ';')
    {
        cursor_.skip_token();
    }

    if (!name.empty() && generic)
    {
        bind_item(items_.names, scope_of(opened_), name, exported, type_namespace);

        items_.generic_aliases.insert("crate::" + qualified(scope_of(opened_), name));
    }
    else if (!name.empty())
    {
        bind_name(
                items_.names, scope_of(opened_), name, compacted(cursor_.slice(begin, cursor_.offset())), exported,
                type_namespace);
    }
}

void Item_collector::read_constant()
{
    Rust_cursor look{cursor_};

    look.skip_trivia();

    std::string name{look.word()};

    if (name == "mut")
    {
        look.skip_trivia();

        name = std::string{look.word()};
    }

    look.skip_trivia();

    if (look.peek() != ':')
    {
        return;
    }

    cursor_ = look;

    const auto exported{std::exchange(exported_, false)};

    // An associated constant, one of an impl or trait block, is the type's and binds nothing in the module.
    if (name != "_" && (opened_.empty() || !opened_.back().associated))
    {
        bind_item(items_.names, scope_of(opened_), name, exported, value_namespace);
    }
}

void Item_collector::read_named_item(const std::string_view word, const std::vector<Attribute>& attributes)
{
    // An enum deriving `Logos` is a scanner, read once every item is collected.
    if (const auto line{word == "enum" ? derives_logos(attributes, cursor_) : std::nullopt})
    {
        items_.scanners.push_back(
                {.offset = cursor_.offset(), .attributes = attributes, .line = *line, .scope = scope_of(opened_)});
    }

    cursor_.skip_trivia();

    const std::string name{cursor_.word()};

    const auto exported{std::exchange(exported_, false)};

    // A struct's name is taken as its constructor's as well, which a unit or a tuple struct's is; a braced struct binds
    // no value to rustc, so a callback naming one is refused here rather than read as its own.
    if (!name.empty() && name != "_")
    {
        bind_item(
                items_.names, scope_of(opened_), name, exported,
                word == "struct" ? std::span<const Namespace>{both_namespaces} :
                                   std::span<const Namespace>{type_namespace});
    }

    cursor_.skip_trivia();

    // The generic parameters declare no item, a `const N: usize` among them none of that name.
    if (cursor_.peek() == '<')
    {
        skip_generics(cursor_);

        cursor_.skip_trivia();
    }

    // An enum's body declares its variants, items of the enum; `enum Never {}` none.
    if (word == "enum" && !name.empty() && cursor_.peek() == '{')
    {
        bind_variants(cursor_, items_.names, scope_of(opened_), name);

        return;
    }

    // A `mod` block's brace opens a module of that name; `mod name;` opens none.
    next_brace_.segment = word == "mod" && cursor_.peek() == '{' ? name : std::string{};

    // A module of the file's own named `std` or `core` stands before the crate under a path, inside the module
    // declaring it.
    if (word == "mod" && (name == "std" || name == "core"))
    {
        items_.std_modules.emplace(scope_of(opened_));
    }
}

void Item_collector::read_function()
{
    cursor_.skip_trivia();

    const std::string name{cursor_.word()};

    if (name.empty())
    {
        return;
    }

    const auto module{scope_of(opened_)};

    const auto exported{std::exchange(exported_, false)};

    // A free function's name is bound in its module; a method's is reached through its type.
    if (opened_.empty() || (opened_.back().impl_type.empty() && !opened_.back().associated))
    {
        bind_item(items_.names, module, name, exported, value_namespace);
    }

    // Generic parameters stand between the name and the parameter list.
    while (!cursor_.done() && cursor_.peek() != '(' && cursor_.peek() != '{' && cursor_.peek() != ';')
    {
        cursor_.skip_token();
    }

    if (cursor_.peek() != '(')
    {
        return;
    }

    const auto parameters{cursor_.offset()};

    cursor_.skip_group();

    auto parameter{first_parameter(cursor_.inside(parameters + 1, cursor_.offset() - 1))};

    cursor_.skip_trivia();

    Function function{
            .returns = return_type(),
            .body = std::nullopt,
            .parameter = std::move(parameter),
            .self_type = opened_.empty() ? std::string{} : opened_.back().impl_type,
            .module = module,
            .scope = {},
            .body_at = 0};

    // A where clause, then the body or the semicolon of a declaration.
    while (!cursor_.done() && cursor_.peek() != '{' && cursor_.peek() != ';')
    {
        cursor_.skip_token();
    }

    // The body is kept whole for a callback naming the function, and left for the walk to enter as the block it is, so
    // that an item declared in it is in scope for a scanner declared beside it.
    if (cursor_.peek() == '{')
    {
        Rust_cursor body{cursor_};

        // The body's own block, marked by where its brace stands, as the walk marks every block it enters, so that what
        // the body binds is looked up under the same name the walk bound it under.
        function.scope = qualified(module, "{" + std::to_string(cursor_.offset()) + "}");

        function.body_at = cursor_.offset() + 1;

        body.skip_group();

        function.body = std::string{cursor_.slice(cursor_.offset() + 1, body.offset() - 1)};
    }

    declared_.push_back({.name = name, .function = std::move(function)});
}

std::string Item_collector::return_type()
{
    if (!cursor_.at("->"))
    {
        return {};
    }

    cursor_.expect('-', "'-'");
    cursor_.expect('>', "'>'");

    const auto begin{cursor_.offset()};

    while (!cursor_.done() && cursor_.peek() != '{' && cursor_.peek() != ';' && !at_where())
    {
        cursor_.skip_token();
    }

    return compacted(cursor_.slice(begin, cursor_.offset()));
}

bool Item_collector::at_where() const
{
    return cursor_.at("where") &&
           (cursor_.offset() + 5 >= source_.size() || !is_word_byte(source_[cursor_.offset() + 5]));
}

void Item_collector::read_punctuation()
{
    if (cursor_.peek() == '(' || cursor_.peek() == '[' || cursor_.peek() == '{')
    {
        open_group();

        // A `pub` before a delimiter, a field's, is no item's.
        exported_ = false;

        return;
    }

    if (cursor_.peek() == ')' || cursor_.peek() == ']' || cursor_.peek() == '}')
    {
        if (!opened_.empty())
        {
            opened_.pop_back();
        }

        std::ignore = cursor_.next("a delimiter");

        exported_ = false;

        return;
    }

    cursor_.skip_token();

    // A `pub` before a punctuation byte, a field's before its colon, is no item's.
    exported_ = false;
}

void Item_collector::open_group()
{
    const auto open{cursor_.next("a delimiter")};

    const auto associated{open == '{' && std::exchange(next_brace_.associated, false)};

    auto segment{open == '{' ? std::exchange(next_brace_.segment, {}) : std::string{}};

    if (open == '{' && segment.empty() && !associated)
    {
        segment = "{" + std::to_string(cursor_.offset() - 1) + "}";
    }

    opened_.push_back(
            {.impl_type = open == '{' ? std::exchange(next_brace_.impl_type, {}) : std::string{},
             .associated = associated,
             .segment = std::move(segment)});
}

void Item_collector::index_functions()
{
    for (auto& [name, function] : declared_)
    {
        if (!function.self_type.empty())
        {
            function.self_type = canonical(items_.names, function.self_type, function.module, Namespace::type);
        }

        const auto& type{function.self_type};

        const auto owner{
                type.empty() ? qualified(function.module, name) :
                               (type.starts_with("crate::") ? type.substr(7) : type) + "::" + name};

        items_.functions[owner].push_back(std::move(function));
    }
}

} // namespace

Items collect_items(const std::string_view source)
{
    return Item_collector{source}.collect();
}

} // namespace munch::tools::audit
