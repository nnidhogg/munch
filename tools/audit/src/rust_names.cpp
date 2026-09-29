#include "munch/tools/audit/rust_names.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements rust_names.hpp: the scope a module stands in, the names the crate and the prelude go by, a binding naming
// a crate, and a name's binding as a scope sees it are private to this unit.

/**
 * @brief A binding found, and the scope it was found in, which is the scope the name is qualified by to give the key
 *        holding it and not always the scope the binding is written in: a variant is bound under its enum, so `T::X` is
 *        held by the scope around the enum while the binding is written in the enum itself.
 */
struct Sighting
{
    /**
     * @brief The binding, or none when no scope in sight binds the name.
     */
    std::optional<std::reference_wrapper<const Binding>> binding;

    /**
     * @brief The scope holding it, as a path from the crate root, empty at the root.
     */
    std::string scope;
};

/**
 * @brief The bindings a path is followed through at most, more than any chain a file has, so that a cycle among them,
 *        which Rust refuses, ends rather than runs on.
 */
constexpr std::size_t binding_chain{8};

/**
 * @brief The names the crate's paths go by when the file binds them no other way, and the bare names the prelude's
 *        types go by whatever path spells them.
 */
constexpr std::array crate_names{
        std::pair{std::string_view{"Skip"}, std::string_view{"logos::Skip"}},
        std::pair{std::string_view{"Filter"}, std::string_view{"logos::Filter"}},
        std::pair{std::string_view{"FilterResult"}, std::string_view{"logos::FilterResult"}},
        std::pair{std::string_view{"skip"}, std::string_view{"logos::skip"}},
        std::pair{std::string_view{"logos"}, std::string_view{"logos"}},
        std::pair{std::string_view{"std::result::Result"}, std::string_view{"Result"}},
        std::pair{std::string_view{"core::result::Result"}, std::string_view{"Result"}},
        std::pair{std::string_view{"std::option::Option"}, std::string_view{"Option"}},
        std::pair{std::string_view{"core::option::Option"}, std::string_view{"Option"}}};

/**
 * @brief Whether a scope's path ends in a block's mark, `{` and an offset and `}`, rather than a module's name.
 * @param scope The scope's path from the crate root.
 * @return True for a block.
 */
[[nodiscard]] bool in_block(const std::string_view scope)
{
    return scope.ends_with('}');
}

/**
 * @brief The module a module stands in: `a` for `a::b`, empty for `a` and for the root.
 * @param module The module's path from the crate root.
 * @return The path of the one above.
 */
[[nodiscard]] std::string parent(const std::string_view module)
{
    const auto cut{module.rfind("::")};

    return std::string{cut == std::string_view::npos ? std::string_view{} : module.substr(0, cut)};
}

/**
 * @brief The module a scope stands in: the scope itself for a module, and for a block the module around it, the marks
 *        of the block and of the blocks it is inside stripped, which `self::` names from inside the block and `super::`
 *        names the parent of, as Rust has it.
 * @param scope The scope's path from the crate root.
 * @return The module's path from the crate root, empty for the root.
 */
[[nodiscard]] std::string module_of(std::string scope)
{
    while (in_block(scope))
    {
        scope = parent(scope);
    }

    return scope;
}

/**
 * @brief A name's binding in a namespace as seen from a scope: the scope's own, or, from a block, the nearest scope
 *        around it that has one, since a block sees the items of the module it stands in and of the blocks around it
 *        while a module sees its own alone, as Rust has it.
 * @param names The file's bindings.
 * @param scope The scope the name is written in, as a path from the crate root, empty at the root.
 * @param name The name, or a path's prefix.
 * @param space The namespace asked.
 * @return The binding and the scope holding it, no binding when no scope in sight binds the name there.
 */
[[nodiscard]] Sighting visible(
        const Names_t& names, std::string scope, const std::string_view name, const Namespace space)
{
    for (;;)
    {
        if (const auto found{names.find(qualified(scope, name))}; found != names.end() && found->second.in(space))
        {
            return {.binding = *found->second.in(space), .scope = std::move(scope)};
        }

        if (!in_block(scope))
        {
            return {};
        }

        scope = parent(scope);
    }
}

/**
 * @brief Whether a binding names an external crate, as a path with a leading `::` may follow it: one at the root, bound
 *        to a crate by its absolute path alone, `lx` to `::logos` under `extern crate logos as lx`; a module or an item
 *        of the file's, bound to its own path under `crate::`, and a `use` of a crate, `use logos as lx`, which Rust
 *        keeps out of the extern prelude, name none, whatever they are called.
 * @param binding The binding.
 * @return Whether it names a crate.
 */
[[nodiscard]] bool names_crate(const Binding& binding)
{
    return binding.module.empty() && binding.path.starts_with("::") && binding.path.find("::", 2) == std::string::npos;
}

} // namespace

std::optional<Binding>& Bindings::in(const Namespace space)
{
    return space == Namespace::type ? type : value;
}

const std::optional<Binding>& Bindings::in(const Namespace space) const
{
    return space == Namespace::type ? type : value;
}

std::string qualified(const std::string_view module, const std::string_view name)
{
    return module.empty() ? std::string{name} : std::string{module} + "::" + std::string{name};
}

std::string_view last_segment(const std::string_view path)
{
    return path.substr(path.rfind("::") == std::string_view::npos ? 0 : path.rfind("::") + 2);
}

bool is_within(const std::string_view module, const std::string_view scope)
{
    return scope.empty() || module == scope || module.starts_with(std::string{scope} + "::");
}

std::string canonical(
        const Names_t& names, std::string path, std::string module, const Namespace space, const std::size_t depth)
{
    // Whether the path is an external crate's, by a leading `::` or by what a binding of its prefix stood for, so that
    // only a root binding naming a crate is followed from then on.
    auto external{false};

    // A few passes resolve what is left of the path once a binding's own path replaces its prefix; a cycle, which Rust
    // refuses, ends where it started.
    for (std::size_t pass{0}; pass < binding_chain; ++pass)
    {
        if (path.starts_with("::"))
        {
            path.erase(0, 2);

            module.clear();

            external = true;
        }

        // The segment at the path's head, when it is one of the three that name a module rather than a binding, taken
        // off with its `::`; a path that is one of them alone names that module itself.
        const auto leading{[&path](const std::string_view segment) {
            if (path == segment || (path.starts_with(segment) && path.compare(segment.size(), 2, "::") == 0))
            {
                path.erase(0, std::min(segment.size() + 2, path.size()));

                return true;
            }

            return false;
        }};

        // `self` and `super` name modules, a block inside one being none, as Rust has it.
        for (auto relative{true}; relative;)
        {
            relative = false;

            if (leading("crate"))
            {
                module.clear();

                relative = true;
            }
            else if (leading("self"))
            {
                module = module_of(module);

                relative = true;
            }
            else if (leading("super"))
            {
                module = parent(module_of(module));

                relative = true;
            }
        }

        // A path that named a module alone stands for that module, by its own path from the root.
        if (path.empty())
        {
            return module.empty() ? "crate" : "crate::" + module;
        }

        auto resolved{false};

        for (auto cut{path.size()}; cut != std::string::npos && !resolved; cut = path.rfind("::", cut - 1))
        {
            if (cut == 0)
            {
                break;
            }

            const auto prefix{path.substr(0, cut)};

            // The whole path is read in the namespace asked and a prefix of it in the types', where a module, a type or
            // an enum stands; the binding is followed when a scope in sight has one there, and an external crate's path
            // only through one naming a crate.
            const auto [binding, holder]{visible(names, module, prefix, cut == path.size() ? space : Namespace::type)};

            const auto followed{binding.has_value() && (!external || names_crate(*binding))};

            const auto crate{std::ranges::find_if(crate_names, [&](const auto& pair) { return pair.first == prefix; })};

            if (!followed && crate == crate_names.end())
            {
                continue;
            }

            std::string value;

            if (!followed)
            {
                value = std::string{crate->second};
            }
            else
            {
                const auto& [bound, home, exported]{binding->get()};

                // An item the file defines stands for itself: the path is its own, from the scope holding the name.
                if (bound == "crate::" + qualified(holder, prefix))
                {
                    return "crate::" + qualified(holder, path);
                }

                // A name bound to its own spelling, `use logos;`, is a crate's and stands as written.
                if (bound == prefix && home == holder)
                {
                    return path;
                }

                // What the binding stands for is read in the module it is written in, whatever the file declares after
                // it, until a chain of bindings grows past any a file has.
                value = depth < binding_chain ? canonical_type(names, bound, home, space, depth + 1) : bound;
            }

            // An alias of a type with arguments stands for the whole; it prefixes nothing.
            if (value.find('<') != std::string::npos && cut != path.size())
            {
                return path;
            }

            if (value == prefix)
            {
                return path;
            }

            path = value + path.substr(cut);

            module.clear();

            // What the prefix stood for is the file's own, whose rest is read at the root, or a crate's, whose head no
            // module or item of the file's is.
            external = !files_own(path);

            resolved = true;
        }

        if (!resolved)
        {
            return path;
        }
    }

    return path;
}

bool files_own(const std::string_view path)
{
    return path == "crate" || path.starts_with("crate::");
}

std::string canonical_type(
        const Names_t& names, const std::string_view text, const std::string_view module, const Namespace space,
        const std::size_t depth)
{
    std::string result;

    for (std::size_t at{0}; at < text.size();)
    {
        // A path: an optional leading `::`, then words joined by `::`.
        auto end{at};

        if (text.compare(end, 2, "::") == 0)
        {
            end += 2;
        }

        while (end < text.size() && is_word_byte(text[end]))
        {
            for (++end; end < text.size() && is_word_byte(text[end]); ++end)
            {
            }

            if (text.compare(end, 2, "::") == 0 && end + 2 < text.size() && is_word_byte(text[end + 2]))
            {
                end += 2;

                continue;
            }

            break;
        }

        if (end == at || !is_word_byte(text[end - 1]))
        {
            result.push_back(text[at++]);

            continue;
        }

        result += canonical(names, std::string{text.substr(at, end - at)}, std::string{module}, space, depth);

        at = end;
    }

    return result;
}

void bind_item(
        Names_t& names, const std::string_view module, const std::string& name, const bool exported,
        const std::span<const Namespace> spaces)
{
    bind_name(names, module, name, "crate::" + qualified(module, name), exported, spaces);
}

void bind_name(
        Names_t& names, const std::string_view module, const std::string& name, const std::string& path,
        const bool exported, const std::span<const Namespace> spaces)
{
    for (const auto space : spaces)
    {
        names[qualified(module, name)].in(space) =
                Binding{.path = path, .module = std::string{module}, .exported = exported};
    }
}

} // namespace munch::tools::audit
