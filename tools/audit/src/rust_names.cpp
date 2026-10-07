#include "munch/tools/audit/rust_names.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
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
    std::optional<Binding> binding{};

    /**
     * @brief The scope holding it, as a path from the crate root, empty at the root.
     */
    std::string scope{};
};

/**
 * @brief A path as its resolution stands between two passes: what is left of it, the module it is read in, and whether
 *        it is an external crate's.
 */
struct Resolution
{
    /**
     * @brief The path.
     */
    std::string path{};

    /**
     * @brief The module it is read in, as a path from the crate root, empty at the root.
     */
    std::string module{};

    /**
     * @brief Whether the path is an external crate's, by a leading `::` or by what a binding of its prefix stood for,
     *        so that only a root binding naming a crate is followed from then on.
     */
    bool external{};
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
 * @brief Returns whether a scope's path ends in a block's mark, `{` and an offset and `}`, rather than a module's name.
 * @param scope The scope's path from the crate root.
 * @return True for a block.
 */
[[nodiscard]] bool in_block(const std::string_view scope)
{
    return scope.ends_with('}');
}

/**
 * @brief Returns the module a scope stands in: the scope itself for a module, and for a block the module around it, the
 *        marks of the block and of the blocks it is inside stripped, which `self::` names from inside the block and
 *        `super::` names the parent of, as Rust has it.
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
 * @brief Returns a name's binding in a namespace as seen from a scope: the scope's own, or, from a block, the nearest
 *        scope around it that has one, since a block sees the items of the module it stands in and of the blocks around
 *        it while a module sees its own alone, as Rust has it.
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
        const auto key{qualified(scope, name)};

        const auto found{names.find(key)};

        const auto binding{[&found, &names, space]() -> std::optional<Binding> {
            if (found == names.end())
            {
                return std::nullopt;
            }

            const auto& [held, bindings]{*found};

            return bindings.in(space);
        }()};

        if (binding)
        {
            return {.binding = *binding, .scope = std::move(scope)};
        }

        if (!in_block(scope))
        {
            return {};
        }

        scope = parent(scope);
    }
}

/**
 * @brief Returns whether a binding names an external crate, as a path with a leading `::` may follow it: one at the
 *        root, bound to a crate by its absolute path alone, `lx` to `::logos` under `extern crate logos as lx`; a
 *        module or an item of the file's, bound to its own path under `crate::`, and a `use` of a crate, `use logos as
 *        lx`, which Rust keeps out of the extern prelude, name none, whatever they are called.
 * @param binding The binding.
 * @return Whether it names a crate.
 */
[[nodiscard]] bool names_crate(const Binding& binding)
{
    const auto& [bound, home, exported]{binding};

    if (!home.empty() || !bound.starts_with(path_separator))
    {
        return false;
    }

    const auto crate{std::string_view{bound}.substr(path_separator.size())};

    return !crate.contains(path_separator);
}

/**
 * @brief Takes a segment off a path's head with its `::`, when the path begins with it as a segment; a path that is the
 *        segment alone names that module itself and is left empty.
 * @param path The path.
 * @param segment The segment, one of the three that name a module rather than a binding.
 * @return Whether it was taken off.
 */
[[nodiscard]] bool take_leading(std::string& path, const std::string_view segment)
{
    const auto prefixed{
            path.starts_with(segment) && path.compare(segment.size(), path_separator.size(), path_separator) == 0};

    const auto heads{path == segment || prefixed};

    if (!heads)
    {
        return false;
    }

    const auto taken{std::min(segment.size() + path_separator.size(), path.size())};

    path.erase(0, taken);

    return true;
}

/**
 * @brief Takes the segments that name a module off a path's head, moving the module it is read in: a leading `::` names
 *        the extern prelude, `crate` the root, and `self` and `super` modules, a block inside one being none, as Rust
 *        has it.
 * @param state The path and the module it is read in.
 */
void strip_leading_segments(Resolution& state)
{
    auto& [path, module, external]{state};

    if (path.starts_with(path_separator))
    {
        path.erase(0, path_separator.size());

        module.clear();

        external = true;
    }

    for (;;)
    {
        if (take_leading(path, crate_root))
        {
            module.clear();

            continue;
        }

        if (take_leading(path, "self"))
        {
            module = module_of(module);

            continue;
        }

        if (!take_leading(path, "super"))
        {
            return;
        }

        module = parent(module_of(module));
    }
}

/**
 * @brief Returns the path a followed binding answers outright, the path standing for itself: an item the file defines
 *        stands for itself, the path being its own from the scope holding the name, and a name bound to its own
 *        spelling, `use logos;`, is a crate's and stands as written.
 * @param path The path.
 * @param binding The binding of the path's prefix.
 * @param holder The scope holding the binding.
 * @param prefix The prefix it binds.
 * @return The path, or std::nullopt when what the binding stands for replaces the prefix.
 */
[[nodiscard]] std::optional<std::string> settled(
        const std::string& path, const Binding& binding, const std::string& holder, const std::string& prefix)
{
    const auto& [bound, home, exported]{binding};

    const auto held_prefix{qualified(holder, prefix)};

    const auto rooted_prefix{rooted_path(held_prefix)};

    if (bound == rooted_prefix)
    {
        const auto held_path{qualified(holder, path)};

        return rooted_path(held_path);
    }

    if (bound == prefix && home == holder)
    {
        return path;
    }

    return std::nullopt;
}

/**
 * @brief Returns the path the crate's names give a prefix when the file binds it no other way.
 * @param prefix The prefix.
 * @return The path, or std::nullopt when the prefix is none of the crate's names.
 */
[[nodiscard]] std::optional<std::string_view> crate_path(const std::string_view prefix)
{
    const auto found{std::ranges::find(crate_names, prefix, &std::pair<std::string_view, std::string_view>::first)};

    if (found == crate_names.end())
    {
        return std::nullopt;
    }

    const auto& [spelling, stands]{*found};

    return stands;
}

/**
 * @brief Replaces the longest prefix of the path that a binding in sight or the crate's names stand for, by what it
 *        stands for, read from the root.
 *
 * The whole path is read in the namespace asked and a prefix of it in the types', where a module, a type or an enum
 * stands; the binding is followed when a scope in sight has one there, and an external crate's path only through one
 * naming a crate. What a followed binding stands for is read in the module it is written in, whatever the file declares
 * after it, until a chain of bindings grows past any a file has.
 * @param names The file's bindings.
 * @param state The path, the module it is read in and whether it is a crate's; the path replaced and read at the root
 *        when a prefix is.
 * @param space The namespace asked.
 * @param depth The bindings followed to reach the path.
 * @return The path's answer when the reading ends here, or std::nullopt when a prefix was replaced and the path is read
 *         again.
 */
[[nodiscard]] std::optional<std::string> resolve_prefix(
        const Names_t& names, Resolution& state, const Namespace space, const std::size_t depth)
{
    auto& [path, module, external]{state};

    for (auto cut{path.size()}; cut != std::string::npos; cut = path.rfind(path_separator, cut - 1))
    {
        if (cut == 0)
        {
            break;
        }

        const auto prefix{path.substr(0, cut)};

        const auto prefix_space{cut == path.size() ? space : Namespace::type};

        const auto [binding, holder]{visible(names, module, prefix, prefix_space)};

        const auto followed{binding && (!external || names_crate(*binding))};

        const auto crate{crate_path(prefix)};

        if (!followed && !crate)
        {
            continue;
        }

        if (followed)
        {
            if (const auto answer{settled(path, *binding, holder, prefix)})
            {
                return *answer;
            }
        }

        const auto value{[&]() -> std::string {
            if (!followed)
            {
                return std::string{*crate};
            }

            const auto& [bound, home, exported]{*binding};

            if (depth >= binding_chain)
            {
                return bound;
            }

            return canonical_type(names, bound, home, space, depth + 1);
        }()};

        // An alias of a type with arguments stands for the whole; it prefixes nothing.
        if (value.contains('<') && cut != path.size())
        {
            return path;
        }

        if (value == prefix)
        {
            return path;
        }

        path = std::format("{}{}", value, path.substr(cut));

        module.clear();

        // What the prefix stood for is the file's own, whose rest is read at the root, or a crate's, whose head no
        // module or item of the file's is.
        external = !files_own(path);

        return std::nullopt;
    }

    return path;
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
    return module.empty() ? std::string{name} : std::format("{}{}{}", module, path_separator, name);
}

std::string_view last_segment(const std::string_view path)
{
    const auto cut{path.rfind(path_separator)};

    const auto begin{cut == std::string_view::npos ? 0 : cut + path_separator.size()};

    return path.substr(begin);
}

bool is_within(const std::string_view module, const std::string_view scope)
{
    return scope.empty() || module == scope || module.starts_with(std::format("{}{}", scope, path_separator));
}

std::string parent(const std::string_view module)
{
    const auto cut{module.rfind(path_separator)};

    return std::string{cut == std::string_view::npos ? std::string_view{} : module.substr(0, cut)};
}

std::string rooted_path(const std::string_view path)
{
    return std::format("{}{}", crate_prefix, path);
}

std::string_view from_root(const std::string_view path) noexcept
{
    return path.starts_with(crate_prefix) ? path.substr(crate_prefix.size()) : path;
}

std::string block_mark(const std::size_t brace)
{
    return std::format("{{{}}}", brace);
}

std::string canonical(
        const Names_t& names, std::string path, std::string module, const Namespace space, const std::size_t depth)
{
    Resolution state{.path = std::move(path), .module = std::move(module), .external = false};

    // A few passes resolve what is left of the path once a binding's own path replaces its prefix; a cycle, which Rust
    // refuses, ends where it started.
    for (std::size_t pass{0}; pass < binding_chain; ++pass)
    {
        strip_leading_segments(state);

        // A path that named a module alone stands for that module, by its own path from the root.
        if (state.path.empty())
        {
            return state.module.empty() ? std::string{crate_root} : rooted_path(state.module);
        }

        if (auto answer{resolve_prefix(names, state, space, depth)})
        {
            return std::move(*answer);
        }
    }

    return state.path;
}

bool files_own(const std::string_view path)
{
    return path == crate_root || path.starts_with(crate_prefix);
}

std::string canonical_type(
        const Names_t& names, const std::string_view text, const std::string_view module, const Namespace space,
        const std::size_t depth)
{
    std::string result{};

    for (std::size_t at{0}; at < text.size();)
    {
        // A path: an optional leading `::`, then words joined by `::`.
        auto end{at};

        if (text.compare(end, path_separator.size(), path_separator) == 0)
        {
            end += path_separator.size();
        }

        // Each segment is a word, and another follows where `::` and a word's byte stand after it.
        auto segment_follows{end < text.size() && is_word_byte(text[end])};

        while (segment_follows)
        {
            while (end < text.size() && is_word_byte(text[end]))
            {
                ++end;
            }

            const auto next{end + path_separator.size()};

            const auto separated{text.compare(end, path_separator.size(), path_separator) == 0};

            segment_follows = separated && next < text.size() && is_word_byte(text[next]);

            if (segment_follows)
            {
                end = next;
            }
        }

        if (end == at || !is_word_byte(text[end - 1]))
        {
            result.push_back(text[at]);

            ++at;

            continue;
        }

        const auto spelled{text.substr(at, end - at)};

        std::string path{spelled};

        std::string scope{module};

        result += canonical(names, std::move(path), std::move(scope), space, depth);

        at = end;
    }

    return result;
}

void bind_item(
        Names_t& names, const std::string_view module, const std::string& name, const bool exported,
        const std::span<const Namespace> spaces)
{
    const auto item{qualified(module, name)};

    const auto path{rooted_path(item)};

    bind_name(names, module, name, path, exported, spaces);
}

void bind_name(
        Names_t& names, const std::string_view module, const std::string& name, const std::string& path,
        const bool exported, const std::span<const Namespace> spaces)
{
    const auto key{qualified(module, name)};

    for (const auto space : spaces)
    {
        names[key].in(space) = Binding{.path = path, .module = std::string{module}, .exported = exported};
    }
}

} // namespace munch::tools::audit
