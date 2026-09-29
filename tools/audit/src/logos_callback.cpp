#include "munch/tools/audit/logos_callback.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/rust_cursor.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements logos_callback.hpp: the crate's names a result is told by, the lexer's members that read or move it, the
// standard macros and a macro's path, the words an expression begins after, and the skips over a block-like expression,
// a closure and a match arm's bar a body is split by are private to this unit.

/**
 * @brief The standard library's macros, whose expansions return from no function: each expands to an expression, a
 *        panic or a compile-time value. Any other macro's expansion is out of sight, and `return T::Other;` in a
 *        `macro_rules!` body returns from the callback that invokes it, so the invocation is refused by name.
 */
constexpr std::array standard{
        std::string_view{"println"},
        std::string_view{"print"},
        std::string_view{"eprintln"},
        std::string_view{"eprint"},
        std::string_view{"format"},
        std::string_view{"format_args"},
        std::string_view{"write"},
        std::string_view{"writeln"},
        std::string_view{"vec"},
        std::string_view{"assert"},
        std::string_view{"assert_eq"},
        std::string_view{"assert_ne"},
        std::string_view{"debug_assert"},
        std::string_view{"debug_assert_eq"},
        std::string_view{"debug_assert_ne"},
        std::string_view{"panic"},
        std::string_view{"unreachable"},
        std::string_view{"todo"},
        std::string_view{"unimplemented"},
        std::string_view{"matches"},
        std::string_view{"dbg"},
        std::string_view{"concat"},
        std::string_view{"stringify"},
        std::string_view{"line"},
        std::string_view{"column"},
        std::string_view{"file"},
        std::string_view{"cfg"},
        std::string_view{"env"},
        std::string_view{"option_env"},
        std::string_view{"include_str"},
        std::string_view{"include_bytes"},
        std::string_view{"module_path"}};

/**
 * @brief The path before a macro's name, read back over the tokens before it: `compat::std` of `compat::std::println!`,
 *        since a module of the file's own may re-export a macro under a standard path's last segment.
 */
struct Macro_path
{
    /**
     * @brief The path's segments joined by `::`, empty for a bare name.
     */
    std::string qualifier;

    /**
     * @brief Whether the path begins with `::`, naming a crate through the extern prelude.
     */
    bool absolute;
};

/**
 * @brief The methods that move the lexer's cursor, whatever they are called on.
 */
constexpr std::array moving_methods{
        std::string_view{"bump"}, std::string_view{"bump_unchecked"}, std::string_view{"trivia"}};

/**
 * @brief The members of the lexer that only read it, or copy it.
 */
constexpr std::array reading_members{std::string_view{"slice"},     std::string_view{"span"},
                                     std::string_view{"remainder"}, std::string_view{"source"},
                                     std::string_view{"extras"},    std::string_view{"clone"}};

/**
 * @brief The constructors whose argument is read by of_argument(), as canonical() spells them: `Ok`, which may hold a
 *        `Skip` or the enum, and the others, which emit or fail.
 */
constexpr std::array constructors{
        std::string_view{"Ok"},
        std::string_view{"Some"},
        std::string_view{"Err"},
        std::string_view{"logos::Filter::Emit"},
        std::string_view{"logos::FilterResult::Emit"},
        std::string_view{"logos::FilterResult::Error"}};

/**
 * @brief The crate's `Skip`, the value and the type, as canonical() spells it.
 */
constexpr std::string_view crate_skip{"logos::Skip"};

/**
 * @brief The `Skip` arms of `Filter` and `FilterResult`, as canonical() spells them.
 */
constexpr std::array skip_arms{std::string_view{"logos::Filter::Skip"}, std::string_view{"logos::FilterResult::Skip"}};

/**
 * @brief The keywords an expression begins after rather than ends with, so that a `|` after one opens a closure's
 *        parameters, `return |x| x` or `move |x| x`, where one after a name, a literal or a group's close is an
 *        operator.
 */
constexpr std::array expression_openers{
        std::string_view{"return"}, std::string_view{"break"}, std::string_view{"continue"}, std::string_view{"yield"},
        std::string_view{"move"},   std::string_view{"async"}, std::string_view{"else"},     std::string_view{"in"},
        std::string_view{"let"},    std::string_view{"if"},    std::string_view{"match"},    std::string_view{"while"},
        std::string_view{"loop"},   std::string_view{"for"},   std::string_view{"unsafe"}};

/**
 * @brief A text without the blanks at either end.
 * @param text The text.
 * @return The text trimmed.
 */
[[nodiscard]] std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && is_blank(text.front()))
    {
        text.remove_prefix(1);
    }

    while (!text.empty() && is_blank(text.back()))
    {
        text.remove_suffix(1);
    }

    return text;
}

/**
 * @brief Whether a `use` statement binds `std` or `core`: an alias `as std` or a path ending in `std` binds the name.
 * @param look A cursor just past the `use`, over the statement's words to its `;`.
 * @return True when it does.
 * @throws Spec_error If a block comment in the statement is left open.
 */
[[nodiscard]] bool use_binds_std(Rust_cursor look)
{
    std::vector<std::string> path;

    for (look.skip_trivia(); !look.done() && !look.at(";"); look.skip_trivia())
    {
        if (const auto piece{look.word()}; !piece.empty())
        {
            path.emplace_back(piece);
        }
        else
        {
            std::ignore = look.next("a token");
        }
    }

    return !path.empty() && (path.back() == "std" || path.back() == "core");
}

/**
 * @brief The path before a macro's name, as Macro_path has it.
 * @param recent The tokens read before the name, the name last among them.
 * @return The path.
 */
[[nodiscard]] Macro_path macro_path(const std::vector<std::string>& recent)
{
    Macro_path path{.qualifier = {}, .absolute = false};

    const auto token{[&recent](const std::size_t back) {
        return back < recent.size() ? std::string_view{recent[recent.size() - 1 - back]} : std::string_view{};
    }};

    for (std::size_t back{1}; token(back) == ":" && token(back + 1) == ":"; back += 3)
    {
        const auto segment{token(back + 2)};

        if (!starts_name(segment))
        {
            path.absolute = true;

            break;
        }

        path.qualifier = std::string{segment} + (path.qualifier.empty() ? "" : "::" + path.qualifier);
    }

    return path;
}

/**
 * @brief Whether a text is one delimited group and nothing else, `(...)` or `{...}` through its own close.
 * @param text The text, trimmed.
 * @param open The opening delimiter.
 * @return True when the group opening the text closes at its end.
 * @throws Spec_error If the group is left open.
 */
[[nodiscard]] bool is_group(const std::string_view text, const char open)
{
    if (!text.starts_with(open))
    {
        return false;
    }

    Rust_cursor cursor{text, 0, text.size()};

    cursor.skip_group();

    return cursor.done();
}

/**
 * @brief Skips the block-like expression opening at the cursor: an `if` with its `else` chain, a `match`, a `while`,
 *        `for` or `loop`, an `unsafe` block or a bare block, the forms Rust lets stand as a statement with no semicolon
 *        after them.
 * @param cursor The cursor, at a statement's start.
 * @param text The text the cursor runs over.
 * @return True when one stood there and was skipped; false, the cursor unmoved, otherwise.
 * @throws Spec_error If a group is left open.
 */
[[nodiscard]] bool skip_block_like(Rust_cursor& cursor, const std::string_view text)
{
    if (cursor.peek() == '{')
    {
        cursor.skip_group();

        return true;
    }

    Rust_cursor look{text, cursor.offset(), text.size()};

    const std::string word{look.word()};

    if (word != "if" && word != "match" && word != "while" && word != "for" && word != "loop" && word != "unsafe")
    {
        return false;
    }

    // The head, a condition, a scrutinee or a label, up to the block, then the block; an `if` then takes its `else`
    // chain, each `else` followed by another head and block or by a block alone.
    for (;;)
    {
        while (!look.done() && look.peek() != '{')
        {
            look.skip_token();
        }

        look.skip_group();

        cursor = look;

        if (word != "if")
        {
            return true;
        }

        look.skip_trivia();

        if (look.word() != "else")
        {
            return true;
        }
    }
}

/**
 * @brief Whether a bare `|` at the cursor opens a match arm, `| 1 => ...`, rather than a closure: the arm's `=>` stands
 *        at the depth the `|` does, where a closure's parameters are followed by its body, and reading such a `|` as a
 *        closure would pass over the arms and with them every outcome they carry.
 * @param from The cursor, at the `|`.
 * @return True when an arm's `=>` follows before anything that ends the group the bar stands in.
 * @throws Spec_error If a group is left open.
 */
[[nodiscard]] bool at_match_arm(const Rust_cursor& from)
{
    auto look{from};

    std::ignore = look.accept('|');

    for (look.skip_trivia(); !look.done(); look.skip_trivia())
    {
        if (look.at("=>"))
        {
            return true;
        }

        // A pattern is one group and never a list of them: a `,`, a `;` or a bracket it does not open ends whatever the
        // bar opened, and a closure's body reaching one of those is no arm.
        if (look.peek() == ';' || look.peek() == ',' || look.peek() == '}' || look.peek() == ')' || look.peek() == ']')
        {
            return false;
        }

        // A struct pattern carries braces, `| Point { value: 1 } =>`, so a group of any kind is part of the pattern and
        // stepped over whole.
        if (look.peek() == '(' || look.peek() == '[' || look.peek() == '{')
        {
            look.skip_group();

            continue;
        }

        look.skip_token();
    }

    return false;
}

/**
 * @brief Skips the closure opening at the cursor: its parameters between the bars, `||` for none, then its body, the
 *        block alone where a return type is written, `|x| -> T { ... }`, and otherwise the expression up to the comma,
 *        semicolon or closing delimiter ending it, which is left, `|x| x + 1` in an argument list or a `let`.
 * @param cursor The cursor, at the first bar.
 * @throws Spec_error If a group or a literal is left open.
 */
void skip_closure(Rust_cursor& cursor)
{
    cursor.expect('|', "'|'");

    // The parameters run to the next bar outside any group, `|(a, b): (u8, u8)|` among them.
    while (!cursor.done() && !cursor.accept('|'))
    {
        cursor.skip_token();
    }

    cursor.skip_trivia();

    if (cursor.at("->"))
    {
        while (!cursor.done() && cursor.peek() != '{')
        {
            cursor.skip_token();
        }

        cursor.skip_group();

        return;
    }

    while (!cursor.done() && cursor.peek() != ',' && cursor.peek() != ';' && cursor.peek() != ')' &&
           cursor.peek() != ']' && cursor.peek() != '}')
    {
        cursor.skip_token();
    }
}

/**
 * @brief Whether a type's text names a path as a whole, not as a part of a longer path or name.
 * @param type The type, its paths resolved.
 * @param path The path.
 * @return True when it does.
 */
[[nodiscard]] bool names_path(const std::string_view type, const std::string_view path)
{
    for (auto at{type.find(path)}; at != std::string_view::npos; at = type.find(path, at + 1))
    {
        const auto before{at == 0 || (!is_word_byte(type[at - 1]) && type[at - 1] != ':')};

        const auto after{
                at + path.size() == type.size() ||
                (!is_word_byte(type[at + path.size()]) && type[at + path.size()] != ':')};

        if (before && after)
        {
            return true;
        }
    }

    return false;
}

} // namespace

Callback_reader::Callback_reader(
        const Enum_context& context, const std::optional<Variant>& variant, const std::string_view callback,
        const std::size_t line)
    : context_{context}, variant_{variant}, callback_{callback}, line_{line}, module_{context.module}
{}

std::optional<std::string> Callback_reader::token() const
{
    if (trimmed(callback_).empty())
    {
        return variant_ ? std::optional{variant_->name} : std::nullopt;
    }

    const auto outcomes{of_callback()};

    if (outcomes.size() == 1)
    {
        const auto& [skips, token]{*outcomes.begin()};

        return skips ? std::nullopt : std::optional{token};
    }

    std::string results;

    for (const auto& [skips, token] : outcomes)
    {
        results += (results.empty() ? "" : ", ") + (skips ? std::string{"a skip"} : "the token " + token);
    }

    fail("its results differ from one path to another, " + results + ", so which the rule gets is decided at run time");
}

Callback_reader::Outcomes_t Callback_reader::of_callback() const
{
    const auto text{trimmed(callback_)};

    if (text.starts_with('|'))
    {
        Rust_cursor cursor{text, 0, text.size()};

        cursor.expect('|', "'|'");

        cursor.skip_trivia();

        const auto parameter{cursor.word()};

        cursor.skip_trivia();

        if (parameter.empty() || !cursor.accept('|'))
        {
            fail("logos 0.15.1 reads an inline callback only as a closure with exactly one parameter");
        }

        const auto body{text.substr(cursor.offset())};

        check_lexer_use(body, parameter == "_" ? std::string_view{} : parameter);

        // A name the attribute's closure binds is bound nowhere the reading can find, so a body binding one is refused.
        if (variant_ && binds_names(body))
        {
            fail("its body binds a name of its own, and a name bound inside a callback written in the attribute is "
                 "out of this reading's sight, so what the body returns cannot be read");
        }

        // A skip attribute's callback has no result to read: every one the crate admits skips or fails.
        return variant_ ? of_value(body) : Outcomes_t{Outcome{.skips = true, .token = {}}};
    }

    const auto path{canonical(context_.names, compacted(text), std::string{module_}, Namespace::value)};

    if (path == "logos::skip")
    {
        return {Outcome{.skips = true, .token = {}}};
    }

    if (path.starts_with("logos::"))
    {
        fail("it names `" + path +
             "` of the crate, which is not its `skip` function, so what it returns is out of sight");
    }

    const auto name{last_segment(path)};

    if (name.empty() || !std::ranges::all_of(name, is_word_byte))
    {
        fail("it is neither a path nor a closure, so what it returns is out of sight");
    }

    // A function of the file is named by its path from the root, which the file's bindings resolve it to.
    return of_function(path.starts_with("crate::") ? path.substr(7) : path);
}

void Callback_reader::check_lexer_use(const std::string_view body, const std::string_view parameter) const
{
    Rust_cursor cursor{body, 0, body.size()};

    // The dots read just before the token at the cursor: one is the dot of a method call or field access.
    std::size_t dots{0};

    // The tokens read so far, words and bytes, blanks and comments left out, so that the path before a macro's name,
    // `compat::std` of `compat::std::println!` with any comment inside it, is read as Rust reads it.
    std::vector<std::string> recent;

    // Whether the body or the file binds `std` or `core` to something of its own: a `use crate::local as std;` in the
    // body or at item level, or a `mod std`, stands before the crate under a path.
    auto std_bound{binds_std()};

    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        if (cursor.at_string() || cursor.at("'") || cursor.at("b'"))
        {
            cursor.skip_token();

            dots = 0;

            continue;
        }

        const auto word{cursor.word()};

        if (word.empty())
        {
            const auto byte{cursor.next("a token")};

            dots = byte == '.' ? dots + 1 : 0;

            recent.emplace_back(1, byte);

            continue;
        }

        recent.emplace_back(word);

        if (word == "use")
        {
            std_bound = use_binds_std(cursor) || std_bound;
        }

        const auto after_dot{dots == 1};

        dots = 0;

        check_macro(cursor, word, recent, std_bound);

        if (after_dot && std::ranges::contains(moving_methods, word))
        {
            fail("its body names `" + std::string{word} +
                 "`, which moves the lexer's cursor, so the match it leaves is not the pattern's");
        }

        if (!after_dot && !parameter.empty() && word == parameter)
        {
            check_parameter_use(cursor, parameter);
        }
    }
}

bool Callback_reader::binds_std() const
{
    // A `mod std` or a binding of the name in the callback's own module or one of its ancestors is what a path from the
    // callback reaches; one in an unrelated module is not.
    const auto in_scope{[this](const std::string_view declared) { return is_within(module_, declared); }};

    return std::ranges::any_of(context_.items.std_modules, in_scope) ||
           std::ranges::any_of(context_.names, [&in_scope](const auto& bound) {
               const auto& key{bound.first};

               for (const std::string_view name : {"std", "core"})
               {
                   if (key == name)
                   {
                       return in_scope("");
                   }

                   if (key.ends_with("::" + std::string{name}))
                   {
                       return in_scope(std::string_view{key}.substr(0, key.size() - name.size() - 2));
                   }
               }

               return false;
           });
}

void Callback_reader::check_macro(
        const Rust_cursor& cursor, const std::string_view word, const std::vector<std::string>& recent,
        const bool std_bound) const
{
    // Blanks and comments may stand between the name and its `!`, `choose !()`, as Rust reads them.
    Rust_cursor look{cursor};

    look.skip_trivia();

    if (!look.at("!") || look.at("!="))
    {
        return;
    }

    auto [qualifier, absolute]{macro_path(recent)};

    // `::std::` names the crate whatever the file binds, through the extern prelude.
    const auto of_std{(qualifier == "std" || qualifier == "core") && (absolute || !std_bound)};

    if (absolute && qualifier.empty())
    {
        qualifier = "::";
    }

    const auto shadowed{
            context_.items.macros.contains(word) || std::ranges::any_of(context_.names, [word](const auto& bound) {
                return bound.first == word || bound.first.ends_with("::" + std::string{word});
            })};

    if (!qualifier.empty() && !of_std)
    {
        fail("its body invokes the macro `" + qualifier + "::" + std::string{word} +
             "!`, reached through a path of the file's own" +
             (std_bound ? ", `std` or `core` being bound by the file or the body," : "") +
             " whose expansion is out of sight and may return from the callback, so what the match "
             "becomes is out of sight");
    }

    if (!std::ranges::contains(standard, word))
    {
        fail("its body invokes the macro `" + std::string{word} +
             "!`, whose expansion is out of sight and may return from the callback, so what the match "
             "becomes is out of sight");
    }

    if (!of_std && shadowed)
    {
        fail("its body invokes the macro `" + std::string{word} +
             "!`, which the file defines or imports under a standard macro's name, so its expansion is out "
             "of sight and may return from the callback, so what the match becomes is out of sight");
    }

    if (!of_std && context_.items.generated)
    {
        fail("its body invokes the macro `" + std::string{word} +
             "!` by a standard macro's name in a file that invokes a macro at item level, whose expansion "
             "may define that name, so what the invocation expands to is out of sight");
    }
}

void Callback_reader::check_parameter_use(Rust_cursor& cursor, const std::string_view parameter) const
{
    // The parameter may only be read through a member: `lex.slice()`, `lex.extras += 1`.
    cursor.skip_trivia();

    const auto dotted{cursor.accept('.')};

    cursor.skip_trivia();

    const auto member{dotted ? cursor.word() : std::string_view{}};

    if (std::ranges::contains(moving_methods, member))
    {
        fail("its body names `" + std::string{member} +
             "`, which moves the lexer's cursor, so the match it leaves is not the pattern's");
    }

    if (!std::ranges::contains(reading_members, member))
    {
        fail("its body uses the lexer `" + std::string{parameter} +
             "` other than through slice, span, remainder, source, extras or clone, so what becomes of the "
             "match is out of sight");
    }
}

bool Callback_reader::binds_names(const std::string_view inside) const
{
    static constexpr std::string_view items[]{"use",    "fn",   "struct", "enum",  "union", "const",
                                              "static", "type", "mod",    "trait", "impl",  "macro_rules"};

    Rust_cursor cursor{inside, 0, inside.size()};

    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        // A group is entered rather than passed over: a block one deeper, a parenthesised block and an `if` branch bind
        // names as the outermost block does.
        if (cursor.peek() == '(' || cursor.peek() == '[' || cursor.peek() == '{')
        {
            const auto open{cursor.offset()};

            cursor.skip_group();

            if (binds_names(inside.substr(open + 1, cursor.offset() - 2 - open)))
            {
                return true;
            }

            continue;
        }

        if (const auto word{cursor.word()}; std::ranges::find(items, word) != std::ranges::end(items))
        {
            return true;
        }
        else if (word.empty())
        {
            cursor.skip_token();
        }
    }

    return false;
}

Callback_reader::Outcomes_t Callback_reader::of_value(const std::string_view text) const
{
    const auto value{trimmed(text)};

    // `()` is a value, and a parenthesised value is that value.
    if (value.empty())
    {
        return {of_result({})};
    }

    if (is_group(value, '('))
    {
        return of_value(value.substr(1, value.size() - 2));
    }

    if (is_group(value, '{'))
    {
        return of_block(value);
    }

    Rust_cursor cursor{value, 0, value.size()};

    const auto word{cursor.word()};

    if (word == "return")
    {
        return of_value(value.substr(cursor.offset()));
    }

    if (word == "if")
    {
        return of_if(value, cursor);
    }

    if (word == "match")
    {
        return of_match(value, cursor);
    }

    const auto compact{compacted(value)};

    // `None` is an error at the boundary whatever the payload; a bool is a result for a variant without one.
    if (compact == "None")
    {
        return {emits()};
    }

    if (compact == "true" || compact == "false")
    {
        if (!is_unit())
        {
            fail("its result `" + std::string{value} +
                 "` is a bool, which logos 0.15.1 takes from a callback only for a variant without a payload, and " +
                 written_variant() + " carries one");
        }

        return {emits()};
    }

    // A constructor: its path up to the parenthesis, which must close the value.
    if (const auto paren{value.find('(')}; paren != std::string_view::npos && is_group(value.substr(paren), '('))
    {
        if (const auto constructor{canonical(
                    context_.names, compacted(value.substr(0, paren)), std::string{module_}, Namespace::value)};
            std::ranges::contains(constructors, std::string_view{constructor}))
        {
            return {of_argument(constructor, value.substr(paren + 1, value.size() - 2 - paren))};
        }
    }

    return {of_result(value)};
}

Callback_reader::Outcome Callback_reader::of_result(const std::string_view text) const
{
    const auto value{trimmed(text)};

    const auto [kind, variant]{classify(value)};

    switch (kind)
    {
    case Value::Kind::skip:
        if (is_unit())
        {
            return {.skips = true, .token = {}};
        }

        if (payload_is_skip())
        {
            return emits();
        }

        fail("its result `" + std::string{value} + "` is the payload of " + written_variant() +
             " to logos 0.15.1, which takes a callback's Skip as a skip only for a variant without a payload, and a "
             "type error unless `" +
             variant_->payload + "` is Skip under another name, so the rule's token is out of sight");
    case Value::Kind::arm_skip:
        return {.skips = true, .token = {}};
    case Value::Kind::variant:
        if (!is_unit())
        {
            fail("its result `" + std::string{value} +
                 "` is the enum, which logos 0.15.1 takes from a callback only for a variant without a payload, and " +
                 written_variant() + " carries one");
        }

        return {.skips = false, .token = variant};
    case Value::Kind::literal:
        if (is_unit())
        {
            fail("its result `" + std::string{value} + "` is a payload, and the variant " + variant_->name +
                 " carries none, so logos 0.15.1 refuses the file");
        }

        return emits();
    case Value::Kind::unit:
        if (!is_unit())
        {
            fail("its result is `()`, which is no payload for " + written_variant() +
                 ", so logos 0.15.1 refuses the file");
        }

        return emits();
    case Value::Kind::opaque:
        break;
    }

    fail("its result `" + std::string{value} +
         "` is not visibly a token or a skip; a result is read when it is logos::Skip, Filter::Skip, "
         "FilterResult::Skip or Ok of one, a constructor of the enum, Some, None, Ok, Err, true, false, a literal, "
         "(), Filter::Emit, FilterResult::Emit or FilterResult::Error, and a function this file defines is read by "
         "its return type instead");
}

Callback_reader::Value Callback_reader::classify(const std::string_view text) const
{
    const auto compact{compacted(text)};

    if (compact.empty() || compact == "()")
    {
        return {.kind = Value::Kind::unit, .variant = {}};
    }

    if (auto named{enum_variant(text)})
    {
        return {.kind = Value::Kind::variant, .variant = std::move(*named)};
    }

    const auto path{canonical(context_.names, compact, std::string{module_}, Namespace::value)};

    if (path == crate_skip)
    {
        return {.kind = Value::Kind::skip, .variant = {}};
    }

    if (std::ranges::contains(skip_arms, std::string_view{path}))
    {
        return {.kind = Value::Kind::arm_skip, .variant = {}};
    }

    // A literal, or a unit struct spelled as the payload's own type, which the blanket conversion takes as the payload
    // whatever the type is.
    if (compact == "true" || compact == "false" || is_digit(compact.front()) || compact.starts_with('"') ||
        compact.starts_with('\'') || compact.starts_with("b\"") || compact.starts_with("b'") ||
        compact.starts_with("r\"") || compact.starts_with("r#") || compact.starts_with("br\"") ||
        compact.starts_with("br#") || (!is_unit() && path == variant_->payload))
    {
        return {.kind = Value::Kind::literal, .variant = {}};
    }

    return {.kind = Value::Kind::opaque, .variant = {}};
}

bool Callback_reader::is_unit() const
{
    return variant_->payload.empty() || variant_->payload == "()";
}

std::optional<std::string> Callback_reader::enum_variant(const std::string_view text) const
{
    const auto compact{compacted(text)};

    // The constructor's path, before its arguments if any.
    const auto head{compact.substr(0, compact.find('('))};

    const auto after{std::string_view{compact}.substr(head.size())};

    if (head.empty() || !is_word_byte(head.back()) || (!after.empty() && !is_group(after, '(')))
    {
        return std::nullopt;
    }

    const auto owner{enum_path()};

    // `Self` is the enum where the text is a function's of the enum's impl blocks; any other path is resolved as the
    // text's module binds it.
    const auto path{
            self_is_enum_ && (head == "Self" || head.starts_with("Self::")) ?
                    owner + head.substr(4) :
                    canonical(context_.names, head, std::string{module_}, Namespace::value)};

    if (!path.starts_with(owner + "::"))
    {
        return std::nullopt;
    }

    const auto name{path.substr(owner.size() + 2)};

    // A name that is no variant's is an associated function's, `T::make(lex)`, whose result is out of sight.
    if (!std::ranges::all_of(name, is_word_byte) || !std::ranges::contains(context_.variants, name))
    {
        return std::nullopt;
    }

    return name;
}

std::string Callback_reader::enum_path() const
{
    return canonical(context_.names, std::string{context_.name}, std::string{context_.module}, Namespace::type);
}

bool Callback_reader::payload_is_skip() const
{
    return variant_->payload == crate_skip;
}

Callback_reader::Outcome Callback_reader::emits() const
{
    return {.skips = false, .token = variant_ ? variant_->name : std::string{}};
}

std::string Callback_reader::written_variant() const
{
    return is_unit() ? variant_->name : variant_->name + "(" + variant_->payload + ")";
}

Callback_reader::Outcomes_t Callback_reader::of_block(const std::string_view block) const
{
    return scoped_at(block).of_body(block.substr(1, block.size() - 2));
}

Callback_reader Callback_reader::scoped_at(const std::string_view block) const
{
    Callback_reader inner{*this};

    if (body_)
    {
        inner.module_ = qualified(
                module_, "{" + std::to_string(body_at_ + static_cast<std::size_t>(block.data() - body_->data())) + "}");
    }

    return inner;
}

Callback_reader::Outcomes_t Callback_reader::of_body(const std::string_view text) const
{
    Outcomes_t outcomes;

    collect_returns(text, outcomes);

    // The statements, split at the semicolons and after a block-like expression that nothing continues.
    Rust_cursor cursor{text, 0, text.size()};

    // The last statement, or the tail expression, and whether a semicolon ended it.
    std::string_view last;

    auto terminated{true};

    // Whether the cursor stands where a statement begins.
    auto opening{true};

    for (auto begin{cursor.offset()};;)
    {
        cursor.skip_trivia();

        if (cursor.done())
        {
            if (const auto tail{trimmed(text.substr(begin, cursor.offset() - begin))}; !tail.empty())
            {
                last = tail;

                terminated = false;
            }

            break;
        }

        if (cursor.accept(';'))
        {
            last = trimmed(text.substr(begin, cursor.offset() - 1 - begin));

            terminated = true;

            begin = cursor.offset();

            opening = true;

            continue;
        }

        if (opening && skip_block_like(cursor, text))
        {
            Rust_cursor look{cursor};

            look.skip_trivia();

            if (!look.done() && look.peek() != ';' && look.peek() != '.' && look.peek() != '?')
            {
                last = trimmed(text.substr(begin, cursor.offset() - begin));

                terminated = true;

                begin = cursor.offset();

                continue;
            }
        }
        else
        {
            cursor.skip_token();
        }

        opening = false;
    }

    const auto returns{last.starts_with("return") && (last.size() == 6 || !is_word_byte(last[6]))};

    if (!terminated)
    {
        outcomes.merge(of_value(last));
    }
    else if (!returns)
    {
        outcomes.insert(of_result({}));
    }

    return outcomes;
}

void Callback_reader::collect_returns(const std::string_view text, Outcomes_t& outcomes) const
{
    Rust_cursor cursor{text, 0, text.size()};

    Statement statement{.operand = false, .opening = true, .standing = false};

    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        if (cursor.at_string() || cursor.at("'") || cursor.at("b'"))
        {
            cursor.skip_token();

            statement.after(Scanned::operand);

            continue;
        }

        if (cursor.peek() == '(' || cursor.peek() == '[' || cursor.peek() == '{')
        {
            collect_group_returns(text, cursor, statement, outcomes);

            continue;
        }

        // A `?` after a value returns its `Err` from the function before anything after it runs, and an `Err` at the
        // boundary is an error token: `Err::<(), ()>(())?; Ok(Skip)` emits, the crate printing `Err(()) 0..1` on "xy",
        // so the arm is one of the body's outcomes wherever the operator stands.
        if (cursor.accept('?'))
        {
            outcomes.insert(emits());

            statement.after(Scanned::operand);

            continue;
        }

        // A closure is a callable of its own: a `return` in its body returns from it and a `?` returns its `Err` from
        // it, neither from the function around it, so the body is passed over whole; `a || b` and `a |= b` are the
        // operators, whose second byte would open a closure otherwise.
        if (cursor.peek() == '|' && (statement.operand || at_match_arm(cursor)))
        {
            cursor.skip_token();

            std::ignore = cursor.accept('|');

            statement.after(Scanned::operator_bar);

            continue;
        }

        if (cursor.peek() == '|')
        {
            skip_closure(cursor);

            statement.after(Scanned::operand);

            continue;
        }

        collect_word_returns(text, cursor, statement, outcomes);
    }
}

void Callback_reader::Statement::after(const Scanned scanned) noexcept
{
    switch (scanned)
    {
    case Scanned::operand:
        operand = true;
        break;
    case Scanned::operator_bar:
    case Scanned::byte:
        operand = false;
        break;
    case Scanned::group:
        operand = true;
        opening = standing;
        break;
    case Scanned::brace:
        operand = !standing && !opening;
        opening = !operand;
        standing = false;
        break;
    case Scanned::continued_brace:
        operand = true;
        opening = false;
        standing = false;
        break;
    case Scanned::block_word:
        standing = standing || opening;
        operand = false;
        break;
    case Scanned::opener_word:
        opening = false;
        operand = false;
        break;
    case Scanned::word:
        opening = false;
        operand = true;
        break;
    case Scanned::semicolon:
        opening = true;
        standing = false;
        operand = false;
        break;
    }
}

void Callback_reader::collect_group_returns(
        const std::string_view text, Rust_cursor& cursor, Statement& statement, Outcomes_t& outcomes) const
{
    const auto open{cursor.offset()};

    const auto braced{cursor.peek() == '{'};

    cursor.skip_group();

    // A block's returns are read in the block's own scope, as its value is: `{ use T::B as Skip; return Skip; }`
    // returns the variant it imports.
    (braced ? scoped_at(text.substr(open)) : *this)
            .collect_returns(text.substr(open + 1, cursor.offset() - 2 - open), outcomes);

    auto look{cursor};

    look.skip_trivia();

    const auto continued{!look.done() && (look.peek() == '.' || look.peek() == '?')};

    statement.after(!braced ? Scanned::group : continued ? Scanned::continued_brace : Scanned::brace);
}

void Callback_reader::collect_word_returns(
        const std::string_view text, Rust_cursor& cursor, Statement& statement, Outcomes_t& outcomes) const
{
    const auto word{cursor.word()};

    // The words that open a block Rust lets stand as a statement, whose block therefore ends one, and only where the
    // word itself opens a statement: a `match` in `let v = match ...` leaves a value.
    static constexpr std::string_view block_words[]{"if", "match", "while", "for", "loop", "unsafe", "else"};

    if (std::ranges::find(block_words, word) != std::ranges::end(block_words))
    {
        statement.after(Scanned::block_word);

        return;
    }

    if (word.empty())
    {
        statement.after(cursor.peek() == ';' ? Scanned::semicolon : Scanned::byte);

        cursor.skip_token();

        return;
    }

    statement.after(std::ranges::contains(expression_openers, word) ? Scanned::opener_word : Scanned::word);

    if (word != "return")
    {
        return;
    }

    // The value runs to the semicolon or the comma of a match arm outside any group, or to the end of the block; its
    // own blocks are read by of_value().
    cursor.skip_trivia();

    const auto begin{cursor.offset()};

    while (!cursor.done() && cursor.peek() != ';' && cursor.peek() != ',')
    {
        cursor.skip_token();
    }

    outcomes.merge(of_value(text.substr(begin, cursor.offset() - begin)));
}

Callback_reader::Outcomes_t Callback_reader::of_if(const std::string_view value, Rust_cursor cursor) const
{
    while (!cursor.done() && cursor.peek() != '{')
    {
        cursor.skip_token();
    }

    const auto open{cursor.offset()};

    cursor.skip_group();

    // The branch is a block of its own, read in its own scope as any block is.
    auto outcomes{of_block(value.substr(open, cursor.offset() - open))};

    cursor.skip_trivia();

    if (cursor.done())
    {
        outcomes.insert(of_result({}));

        return outcomes;
    }

    if (cursor.word() != "else")
    {
        fail("its result `" + std::string{value} + "` is not visibly a token or a skip");
    }

    outcomes.merge(of_value(value.substr(cursor.offset())));

    return outcomes;
}

Callback_reader::Outcomes_t Callback_reader::of_match(const std::string_view value, Rust_cursor cursor) const
{
    while (!cursor.done() && cursor.peek() != '{')
    {
        cursor.skip_token();
    }

    const auto open{cursor.offset()};

    cursor.skip_group();

    if (!cursor.done())
    {
        fail("its result `" + std::string{value} + "` is not visibly a token or a skip");
    }

    const auto arms{value.substr(open + 1, value.size() - 2 - open)};

    // The braces holding the arms are a block of the walk's own, so the arms stand one scope deeper than the match does
    // and an arm's own block deeper still; reading an arm in the scope around the match would look for its names under
    // a scope the walk never bound them in.
    const auto inside{scoped_at(value.substr(open))};

    Rust_cursor arm{arms, 0, arms.size()};

    Outcomes_t outcomes;

    for (arm.skip_trivia(); !arm.done(); arm.skip_trivia())
    {
        while (!arm.done() && !arm.at("=>"))
        {
            arm.skip_token();
        }

        if (arm.done())
        {
            fail("its result `" + std::string{value} + "` is not visibly a token or a skip");
        }

        arm.expect('=', "'=>' after a match arm's pattern");
        arm.expect('>', "'=>' after a match arm's pattern");

        arm.skip_trivia();

        const auto begin{arm.offset()};

        if (arm.peek() == '{')
        {
            arm.skip_group();

            outcomes.merge(inside.of_block(arms.substr(begin, arm.offset() - begin)));
        }
        else
        {
            while (!arm.done() && arm.peek() != ',')
            {
                arm.skip_token();
            }

            outcomes.merge(inside.of_value(arms.substr(begin, arm.offset() - begin)));
        }

        arm.skip_trivia();

        std::ignore = arm.accept(',');
    }

    return outcomes;
}

Callback_reader::Outcome Callback_reader::of_argument(
        const std::string_view constructor, const std::string_view text) const
{
    // An error at the boundary, whatever it holds.
    if (constructor == "Err" || constructor == "logos::FilterResult::Error")
    {
        return emits();
    }

    // The Ok arm of a `Result<Skip, E>` carries a `Skip` by its own type, so it skips whatever the expression spells
    // it.
    if (ok_skips_ && constructor == "Ok")
    {
        return {.skips = true, .token = {}};
    }

    const auto value{trimmed(text)};

    const auto [kind, variant]{classify(value)};

    switch (kind)
    {
    case Value::Kind::skip:
        if (is_unit() && constructor == "Ok")
        {
            return {.skips = true, .token = {}};
        }

        if (!is_unit() && payload_is_skip())
        {
            return emits();
        }

        fail("its result wraps `" + std::string{value} + "` in `" + std::string{constructor} +
             "`, which logos 0.15.1 takes as a skip only as Ok(Skip) for a variant without a payload, and as the "
             "payload of a variant carrying `Skip`; " +
             written_variant() + " is neither, so the crate refuses it and the rule's token is out of sight");
    case Value::Kind::arm_skip:
        fail("its result wraps `" + std::string{value} + "` in `" + std::string{constructor} +
             "`, which is no result logos 0.15.1 takes from a callback");
    case Value::Kind::variant:
        if (!is_unit() || constructor == "Some")
        {
            fail("its result wraps `" + std::string{value} + "` in `" + std::string{constructor} +
                 "`, and logos 0.15.1 takes the enum from a callback only bare or in Ok, Filter::Emit or "
                 "FilterResult::Emit, for a variant without a payload, which " +
                 written_variant() + (is_unit() ? " is" : " is not"));
        }

        return {.skips = false, .token = variant};
    case Value::Kind::literal:
        if (is_unit())
        {
            fail("its result wraps the payload `" + std::string{value} + "` in `" + std::string{constructor} +
                 "`, and the variant " + variant_->name + " carries none, so logos 0.15.1 refuses the file");
        }

        return emits();
    case Value::Kind::unit:
        if (!is_unit())
        {
            fail("its result wraps `()` in `" + std::string{constructor} + "`, which is no payload for " +
                 written_variant() + ", so logos 0.15.1 refuses the file");
        }

        return emits();
    case Value::Kind::opaque:
        // For a variant without a payload, an expression the text does not decide decides the token.
        if (is_unit() &&
            (constructor == "Ok" || constructor == "logos::Filter::Emit" || constructor == "logos::FilterResult::Emit"))
        {
            fail("its result wraps `" + std::string{value} + "` in `" + std::string{constructor} +
                 "`, an expression the text does not decide, which for a variant without a payload may be `()` or "
                 "any variant of the enum, so the rule's token is out of sight");
        }

        break;
    }

    return emits();
}

Callback_reader::Outcomes_t Callback_reader::of_function(const std::string_view path) const
{
    const auto name{last_segment(path)};

    const auto& [written, body, parameter, self_type, module, scope, body_at]{defined_function(path)};

    if (!body)
    {
        fail("the function `" + std::string{name} +
             "` is declared without a body, so what it does with the lexer and what it returns are out of sight");
    }

    if (!parameter)
    {
        fail("the function `" + std::string{name} +
             "` binds the lexer with a pattern rather than a name, so what it does with the lexer is out of sight");
    }

    check_lexer_use(*body, *parameter);

    if (!variant_)
    {
        return {Outcome{.skips = true, .token = {}}};
    }

    // In a function declared in one of the enum's impl blocks, `Self` is the enum, in the type and in the body, and the
    // names of both resolve in the function's own module.
    Callback_reader inner{*this};

    inner.self_is_enum_ = self_type == enum_path();

    // The body's names are the body's own: a `use` it writes binds in its block and not in the module around it, so a
    // result is read in that block, which sees the module's names through the scopes above it. The return type stands
    // outside the body and is read in the module, above.
    inner.module_ = scope;

    // Where the body stands in the file, so that a block inside it can be named by its own brace's offset, which is the
    // name the walk bound that block's items under.
    inner.body_ = *body;

    inner.body_at_ = body_at;

    // The return type with its aliases and imports resolved: `Filter`, `FilterResult` and `Result<Skip, E>` leave the
    // decision to the value; the enum takes the variant returned, for a variant without a payload; the crate's `Skip`
    // alone is a skip, and a payload where the variant carries it.
    const auto returns{canonical_type(context_.names, written, module, Namespace::type)};

    // A type written through a generic alias of the file's, `R<Skip>` under `type R<T> = Result<T, ()>`, is what the
    // alias's arguments make of it, which the reading does not substitute.
    for (const auto& alias : context_.items.generic_aliases)
    {
        if (names_path(returns, alias))
        {
            fail("the function `" + std::string{name} + "` returns `" + written + "` through the generic alias `" +
                 std::string{last_segment(alias)} +
                 "`, whose arguments the reading does not substitute, so what it returns is out of sight");
        }
    }

    // A value of the payload's own type is the payload, whatever the type, as the crate's blanket conversion has it: a
    // `Filter<()>` returned to `V(Filter<()>)` is the payload and emits, where a `Filter<Filter<()>>` is the arm.
    if (!is_unit() && returns == variant_->payload)
    {
        return {emits()};
    }

    if (names_path(returns, "logos::Filter") || names_path(returns, "logos::FilterResult"))
    {
        return inner.of_body(*body);
    }

    if (names_path(returns, enum_path()) || (inner.self_is_enum_ && names_path(returns, "Self")))
    {
        if (!is_unit())
        {
            fail("the function `" + std::string{name} +
                 "` returns the enum, which logos 0.15.1 takes from a callback only for a variant without a payload, "
                 "and " +
                 written_variant() + " carries one");
        }

        return inner.of_body(*body);
    }

    if (returns == crate_skip && is_unit())
    {
        return {Outcome{.skips = true, .token = {}}};
    }

    // `Ok(Skip)` skips and `Err(e)` is an error token at the same boundary, so the body decides which arm it takes.
    if (returns.starts_with("Result<logos::Skip,") && is_unit())
    {
        inner.ok_skips_ = true;

        return inner.of_body(*body);
    }

    if (names_path(returns, crate_skip) && !payload_is_skip())
    {
        fail("the function `" + std::string{name} + "` returns `" + written +
             "`, which logos 0.15.1 takes as a skip only for a variant without a payload, written `Skip` or as the "
             "`Ok(Skip)` of a `Result<Skip, E>`, and as the payload of a variant carrying `Skip`; " +
             written_variant() + " is neither, so the crate refuses it and the rule's token is out of sight");
    }

    if (returns == "bool" && !is_unit())
    {
        fail("the function `" + std::string{name} +
             "` returns a bool, which logos 0.15.1 takes from a callback only for a variant without a payload, and " +
             written_variant() + " carries one");
    }

    // No return type is `()`, the variant's token where it has no payload and no payload where it has one.
    return {returns.empty() ? of_result({}) : emits()};
}

const Function& Callback_reader::defined_function(const std::string_view path) const
{
    const auto found{context_.items.functions.find(path)};

    if (found == context_.items.functions.end())
    {
        fail("it names no function this file defines, so what it returns is out of sight; a callback is read when "
             "it is logos::skip, a function this file defines, or a closure whose every result is visibly a token "
             "or a skip");
    }

    // A trait declares a method without a body and its impl defines it with one; the definition is the one read.
    const auto defined{std::ranges::count_if(found->second, [](const Function& one) { return one.body.has_value(); })};

    if (found->second.size() > 1 && defined != 1)
    {
        fail("this file defines `" + std::string{last_segment(path)} +
             "` more than once, so which one it names is out of sight");
    }

    return defined == 1 ?
                   *std::ranges::find_if(found->second, [](const Function& one) { return one.body.has_value(); }) :
                   found->second.front();
}

void Callback_reader::fail(const std::string& why) const
{
    throw Spec_error{"the callback `" + std::string{callback_} + "` is refused: " + why, line_};
}

} // namespace munch::tools::audit
