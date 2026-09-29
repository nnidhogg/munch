#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_CALLBACK_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_CALLBACK_HPP

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/rust_cursor.hpp"
#include "munch/tools/audit/rust_items.hpp"
#include "munch/tools/audit/rust_names.hpp"

/**
 * @brief What a `#[token]` or `#[regex]` callback does with the match, Callback_reader, read against the enum and what
 *        the file defines and binds, Enum_context, and the variant the rule stands on, Variant: every result its body
 *        produces, through blocks, returns, `?`, ifs and matches, each read against the variant's payload as the
 *        crate's `CallbackResult` converts the pair, and whether the body moves the lexer.
 */
namespace munch::tools::audit
{
/**
 * @brief What a callback is read against: the enum, its variants, and what the file defines and binds.
 */
struct Enum_context
{
    /**
     * @brief The enum's name, which a constructor of it opens with.
     */
    std::string_view name;

    /**
     * @brief The variants' names, which a constructor of the enum must be one of.
     */
    const std::vector<std::string>& variants;

    /**
     * @brief What the file defines and binds: its functions, its generic type aliases, its macros, whether it invokes a
     *        macro at item level whose expansion may define one, and the modules declaring a module named `std` or
     *        `core`.
     */
    const Items& items;

    /**
     * @brief The names the file binds, with the path `#[logos(crate = ...)]` gives the crate among them.
     */
    const Names_t& names;

    /**
     * @brief The scope the enum is declared in, as a path from the crate root, a block's marked, empty at the root,
     *        which the names in its callbacks resolve in.
     */
    std::string_view module;
};

/**
 * @brief The variant a rule stands on, against which its callback's results are read.
 */
struct Variant
{
    /**
     * @brief The variant's name.
     */
    std::string name;

    /**
     * @brief The payload's type as written, without trivia; empty for a unit variant, and `()` is a unit to logos too.
     */
    std::string payload;
};

/**
 * @brief Reads what a `#[token]` or `#[regex]` callback does with the match, as far as the source shows it.
 *
 * logos decides by the callback's type and the variant's payload at compile time, through the `CallbackResult`
 * conversion for the pair (logos 0.15.1, src/internal.rs and logos-codegen 0.15.1, generator/leaf.rs): for a variant
 * without a payload, `()` to the conversion, `Skip`, `Ok(Skip)` and the `Skip` arms of `Filter` and `FilterResult`
 * discard the match, `()`, `bool`, `Option<()>`, `Result<(), E>`, `Err`, the `Emit` arms and the `Error` arm leave the
 * variant's token or an error at the same boundary, and the enum returned, bare or in `Ok`, `Filter` or `FilterResult`,
 * is the token itself; for a variant with a payload, a value of the payload's type, bare or in `Some`, `Ok` or an
 * `Emit` arm, is that payload and the variant's token, so a `Skip` returned to a variant carrying `Skip` is the payload
 * and emits, the `Skip` arms alone still skip, and the enum, `()`, `bool` and a `Skip` the payload's type is not are
 * type errors the crate refuses. The reading has no types, only the text, so it reads what the text shows:
 * `logos::skip` or `skip` is the crate's function returning `Skip`; another path names a function this file defines,
 * whose return type decides, its body read where the type is `Filter`, `FilterResult`, `Result<Skip, E>` or the enum; a
 * closure's body is read for every result it produces, the tail expression, every `return`, the branches of an `if` and
 * the arms of a `match`, and each result must be visibly one thing: `Skip`, `Filter::Skip`, `FilterResult::Skip` or
 * `Ok` of one, a constructor of the enum, or `Some`, `None`, `Ok`, `Err`, `true`, `false`, a literal, `()`,
 * `Filter::Emit`, `FilterResult::Emit` or `FilterResult::Error`, each read against the payload the variant is written
 * with. A callback whose results all skip discards the rule, one whose results all emit one variant makes the rule that
 * variant's, and anything else, a function the file does not define, a result the text does not show, results that skip
 * on one path and emit on another, or a result the crate refuses for the variant's payload, is refused by name, since
 * the rule's token is then decided at run time or out of sight. logos 0.15.1 refuses a closure with a return type
 * annotation, so a closure's body is the only place to look, and a `skip(...)` attribute's callback has no result to
 * read, every result the crate admits there skipping or failing.
 *
 * The callback also holds the lexer, and may move it: logos 0.15.1's Lexer moves its cursor through `bump` alone among
 * its public methods, and through `bump_unchecked`, `trivia`, `error`, `end` and `set` of its `internal::LexerInternal`
 * trait, which a callback can reach by importing it (logos 0.15.1, src/lexer.rs and src/internal.rs), while `slice`,
 * `span`, `remainder` and `source` and the `extras` field read it and `clone` copies it. A match the callback extends
 * or empties is not the pattern's, so a body, a closure's or the named function's, is read only where its lexer
 * parameter is used through the reading members alone; one naming `bump`, `bump_unchecked` or `trivia` as a method on
 * anything, or using the parameter any other way, passing it to a function or a macro, calling another method on it or
 * binding it to a name, is refused by name, and so is a function declared without a body, whose use of the lexer is out
 * of sight.
 */
class Callback_reader
{
public:
    /**
     * @brief Binds the reading to one rule.
     * @param context The enum, its variants, and what the file defines and binds.
     * @param variant The variant the rule stands on, or std::nullopt for a `skip(...)` attribute's rule.
     * @param callback The callback's text, empty when there is none.
     * @param line The rule's line, for refusals.
     */
    Callback_reader(
            const Enum_context& context, const std::optional<Variant>& variant, std::string_view callback,
            std::size_t line);

    /**
     * @brief The token the rule carries.
     * @return The token, or std::nullopt when the rule is a skip's or the callback skips the match.
     * @throws Spec_error If what the callback does is not decidable from the source, or it moves the lexer.
     */
    [[nodiscard]] std::optional<std::string> token() const;

private:
    /**
     * @brief What a callback visibly does with the match on one path: skips it, or emits a token, named.
     */
    struct Outcome
    {
        /**
         * @brief Whether the match is skipped.
         */
        bool skips;

        /**
         * @brief The token emitted, empty when the match is skipped.
         */
        std::string token;

        /**
         * @brief Orders outcomes, skips before tokens and tokens by name, so that a set of them holds each once.
         */
        [[nodiscard]] auto operator<=>(const Outcome&) const = default;
    };

    /**
     * @brief The outcomes a callback shows, one per path.
     */
    using Outcomes_t = std::set<Outcome>;

    /**
     * @brief What a value visibly is, as far as reading it against the variant needs.
     */
    struct Value
    {
        /**
         * @brief The kinds a value is told apart into.
         */
        enum class Kind
        {
            /**
             * @brief The crate's `Skip`, bare.
             */
            skip,

            /**
             * @brief The `Skip` arm of `Filter` or `FilterResult`.
             */
            arm_skip,

            /**
             * @brief A constructor of the enum, `T::Name` or `T::Name(...)`.
             */
            variant,

            /**
             * @brief A literal, `true` or `false`: a payload of some type.
             */
            literal,

            /**
             * @brief `()`, or nothing.
             */
            unit,

            /**
             * @brief Anything else, whose type the text does not show.
             */
            opaque,
        };

        /**
         * @brief The kind.
         */
        Kind kind;

        /**
         * @brief The variant named, when the kind is variant.
         */
        std::string variant;
    };

    /**
     * @brief What the scan of a body for its returns has just read, as far as where it stands among the statements
     *        needs.
     */
    enum class Scanned
    {
        /**
         * @brief Something that ends an operand: a literal, a `?`, or a closure passed over whole.
         */
        operand,

        /**
         * @brief A `|` that is an operator, `a | b`, `a || b` or a match arm's alternative, and no closure.
         */
        operator_bar,

        /**
         * @brief A parenthesised or bracketed group.
         */
        group,

        /**
         * @brief A brace group that nothing continues with a `.` or a `?`.
         */
        brace,

        /**
         * @brief A brace group a `.` or a `?` continues.
         */
        continued_brace,

        /**
         * @brief A word opening a block Rust lets stand as a statement: `if`, `match`, `while`, `for`, `loop`,
         *        `unsafe` or `else`.
         */
        block_word,

        /**
         * @brief Any other word an expression begins after, `return`, `let` or `move` among them.
         */
        opener_word,

        /**
         * @brief Any other word, which ends an operand.
         */
        word,

        /**
         * @brief A semicolon.
         */
        semicolon,

        /**
         * @brief Any other byte.
         */
        byte,
    };

    /**
     * @brief Where the scan of a body for its returns stands among the statements, which decides whether a `|` opens a
     *        closure and whether a brace group ends a statement.
     */
    struct Statement
    {
        /**
         * @brief Moves the scan past what it read: a brace group ends the statement where it began at a statement's
         *        start or after a block-like word and nothing continues it, and opens the next, and any other group
         *        leaves a value, a parenthesised one between a block-like word and its brace, the condition of an `if`,
         *        keeping the statement open; a block-like word opening a statement keeps it standing, and a semicolon
         *        ends it.
         * @param scanned What was read.
         */
        void after(Scanned scanned) noexcept;

        /**
         * @brief Whether the token last read can end an expression, a name, a literal, a group's close or a `?`, so
         *        that a `|` after it is an operator, the bitwise or of `a | b` or the alternative of a pattern, `A | B
         *        =>`, and one after anything else, an operator, a comma, a `=`, a `return` or a `move`, opens a
         *        closure's parameters.
         */
        bool operand;

        /**
         * @brief Whether the cursor stands where a statement begins: at the body's start, after a semicolon, and after
         *        a block Rust read as a statement of its own.
         */
        bool opening;

        /**
         * @brief Whether a block-like expression was opened at a statement's start, so that its brace group ends the
         *        statement however many groups stand between the word and the brace, `if (cond) { }` among them.
         */
        bool standing;
    };

    /**
     * @brief The outcomes of the callback: a path's by the function it names, a closure's by its body.
     *
     * A closure written in the attribute is read where it stands, and the walk that binds a file's items does not enter
     * an attribute, so a name the body binds, wherever in the body it binds it, is bound nowhere the reading can find:
     * `|_| { use T::B as Skip; Skip }` makes `Skip` the variant for that body alone, and reading it in the scope around
     * the enum finds the crate's `Skip`. The body is refused while it binds anything, whatever shape it takes, rather
     * than read under a scope it is not in.
     * @return The outcomes.
     * @throws Spec_error If the callback is out of sight or malformed.
     */
    [[nodiscard]] Outcomes_t of_callback() const;

    /**
     * @brief Refuses a body that moves the lexer or lets it out of sight.
     * @param body The body's text, a closure's or a function's.
     * @param parameter The lexer parameter's name, empty when the callback binds none.
     * @throws Spec_error If the body names a method moving the cursor, uses the parameter other than through the
     *         members that read the lexer, or invokes a macro other than a standard one, whose expansion may return
     *         from the callback.
     */
    void check_lexer_use(std::string_view body, std::string_view parameter) const;

    /**
     * @brief Whether the file binds `std` or `core` to something of its own in sight of the text being read: a `mod
     *        std` or a binding of the name in the text's module or one of its ancestors, which is what a path from the
     *        text reaches, and not one in an unrelated module.
     * @return True when it does.
     */
    [[nodiscard]] bool binds_std() const;

    /**
     * @brief Refuses a macro invoked where a word stands before a `!`: a name is the standard macro only bare or under
     *        `std::` or `core::`, qualified by any other path, `checks::assert!`, it is a macro of that module's, and
     *        bare it is the file's own macro where the file defines or imports one by that name, since Rust resolves
     *        the textual macro first, or may be, where the file invokes a macro at item level whose expansion may
     *        define one.
     * @param cursor The cursor, just past the word.
     * @param word The word.
     * @param recent The tokens read so far, the word last among them.
     * @param std_bound Whether `std` or `core` is bound by the file or the body to something of its own.
     * @throws Spec_error If a macro is invoked whose expansion is out of sight.
     */
    void check_macro(
            const Rust_cursor& cursor, std::string_view word, const std::vector<std::string>& recent,
            bool std_bound) const;

    /**
     * @brief Refuses a use of the lexer parameter other than through a member that reads the lexer, `lex.slice()` or
     *        `lex.extras += 1`.
     * @param cursor The cursor, just past the parameter's name, left after the member's name.
     * @param parameter The parameter's name.
     * @throws Spec_error If the parameter is used any other way, or a member named moves the lexer.
     */
    void check_parameter_use(Rust_cursor& cursor, std::string_view parameter) const;

    /**
     * @brief Whether a stretch of a body declares anything: a `use`, an item, or a macro definition.
     * @param inside The stretch.
     * @return True when it binds a name.
     * @throws Spec_error If a group or a literal is left open.
     */
    [[nodiscard]] bool binds_names(std::string_view inside) const;

    /**
     * @brief The outcomes of an expression: a block's, an `if`'s branches, a `match`'s arms, a `return`'s value, or the
     *        one outcome a constructor shows.
     * @param text The expression's text.
     * @return The outcomes.
     * @throws Spec_error If the expression is not visibly a token or a skip.
     */
    [[nodiscard]] Outcomes_t of_value(std::string_view text) const;

    /**
     * @brief The outcome a bare result makes, read against the variant's payload: a skip for `Skip` where the variant
     *        has no payload and the `Skip` arms anywhere, the variant named by a constructor of the enum, the rule's
     *        own variant for a payload of the variant's type, `()` where it has none and `Skip` where that is its
     *        payload's type, and a refusal for what the crate refuses or the text does not show.
     * @param text The result's text.
     * @return The outcome.
     * @throws Spec_error If the result is not visibly a token or a skip, or is one the crate refuses for the variant.
     */
    [[nodiscard]] Outcome of_result(std::string_view text) const;

    /**
     * @brief What a value visibly is: `Skip`, a `Skip` arm, a constructor of the enum, a literal, `()` or opaque.
     * @param text The value's text.
     * @return The value.
     * @throws Spec_error If a block comment or a literal in the value is left open.
     */
    [[nodiscard]] Value classify(std::string_view text) const;

    /**
     * @brief Whether the rule's variant has no payload, `()` being none to logos as well.
     * @return True when it has none.
     */
    [[nodiscard]] bool is_unit() const;

    /**
     * @brief The variant a constructor of the enum names, `Enum::Variant` or `Enum::Variant(...)`, by whatever path the
     *        text's module binds the enum, `Self::Variant` in a function of the enum's impl blocks, and bare under `use
     *        Enum::*` or `use Enum::Variant`.
     * @param text The expression's text.
     * @return The variant, or std::nullopt when the expression is not such a constructor.
     * @throws Spec_error If a block comment or a literal in the expression is left open.
     */
    [[nodiscard]] std::optional<std::string> enum_variant(std::string_view text) const;

    /**
     * @brief The enum's path from the crate root, `crate::m::T` for `enum T` inside `mod m`, as the file's bindings
     *        resolve its name in its own module.
     * @return The path.
     */
    [[nodiscard]] std::string enum_path() const;

    /**
     * @brief Whether the rule's variant carries the crate's `Skip` as its payload, in any of its spellings.
     * @return True when it does.
     */
    [[nodiscard]] bool payload_is_skip() const;

    /**
     * @brief The outcome that emits the rule's own variant; not asked of a skip's rule, whose results are not read.
     * @return The outcome.
     */
    [[nodiscard]] Outcome emits() const;

    /**
     * @brief The variant with its payload, `V(u64)`, for refusals.
     * @return The text.
     */
    [[nodiscard]] std::string written_variant() const;

    /**
     * @brief The outcomes of a block, read in the scope the block itself is.
     *
     * A block is a scope of its own, so what it binds is bound under its own brace and the names it writes are read
     * there: `{ use T::X as Skip; Skip }` makes `Skip` the variant for that block alone, whether the block stands as a
     * value, as a statement or as a match arm's, where reading it in the scope around it finds the crate's `Skip` and
     * discards a match the crate emits. The brace places the block in the file, as the walk that bound its items placed
     * it.
     * @param block The block's text, its braces included, as it stands in the body being read.
     * @return The outcomes.
     * @throws Spec_error If a result is not visibly a token or a skip.
     */
    [[nodiscard]] Outcomes_t of_block(std::string_view block) const;

    /**
     * @brief This reading moved into the block a brace opens, whose scope the walk bound that block's items under.
     * @param block The text of the body being read from the brace on.
     * @return The reading, in the block's own scope; this reading unchanged for a closure, whose text is not the
     *         body's.
     */
    [[nodiscard]] Callback_reader scoped_at(std::string_view block) const;

    /**
     * @brief The outcomes of a block's content: every `return` in it, and its value, the tail expression or `()`.
     *
     * The statements are split at the semicolons outside any group, string or character literal, and after a block-like
     * expression, an `if`, a `match`, a loop or a block, that opens a statement and has nothing continuing it, which
     * Rust lets stand without a semicolon; the last one, when no semicolon or such an end closes it, is the block's
     * value, and a block ending in a `return` has no value of its own.
     * @param text The text between the braces.
     * @return The outcomes.
     * @throws Spec_error If a result is not visibly a token or a skip.
     */
    [[nodiscard]] Outcomes_t of_body(std::string_view text) const;

    /**
     * @brief Adds the outcomes of every `return` and every `?` in a text, at any depth, a closure's left out, since the
     *        closure is a callable of its own and they exit it rather than the text's function.
     * @param text The text.
     * @param outcomes The outcomes, added to.
     * @throws Spec_error If a returned value is not visibly a token or a skip.
     */
    void collect_returns(std::string_view text, Outcomes_t& outcomes) const;

    /**
     * @brief Adds the returns of a group opening at the cursor, a block's read in the block's own scope, and moves the
     *        statement past it: a block Rust reads as a statement of its own ends it, where it began at a statement's
     *        start or after a block-like word and nothing continues it, and any other group leaves a value.
     *
     * A block Rust reads as a statement of its own ends it, and what follows opens the next: the `{}` of `if (false)
     * {}` leaves no value behind, so a `||` after it opens a closure and is no operator. A block standing where a value
     * does leaves a value, so the `{ false }` of `let v = { false } || ...` is the operator's left operand, `Point { ..
     * }` is a value, and a block a `.` or a `?` continues is one too.
     * @param text The text being scanned.
     * @param cursor The cursor, at the group's opening delimiter, left after its close.
     * @param statement Where the scan stands among the statements.
     * @param outcomes The outcomes, added to.
     * @throws Spec_error If a returned value is not visibly a token or a skip.
     */
    void collect_group_returns(
            std::string_view text, Rust_cursor& cursor, Statement& statement, Outcomes_t& outcomes) const;

    /**
     * @brief Adds the outcome of a `return` at the cursor, its value running to the semicolon or the comma of a match
     *        arm outside any group, and moves the statement past a word or a byte: a block-like word opening a
     *        statement keeps it standing, and a semicolon ends it.
     * @param text The text being scanned.
     * @param cursor The cursor, at the word or byte, left after it or after the returned value.
     * @param statement Where the scan stands among the statements.
     * @param outcomes The outcomes, added to.
     * @throws Spec_error If a returned value is not visibly a token or a skip.
     */
    void collect_word_returns(
            std::string_view text, Rust_cursor& cursor, Statement& statement, Outcomes_t& outcomes) const;

    /**
     * @brief The outcomes of an `if`: the block after the condition, read in its own scope, then `else` and another
     *        `if` or block; without an `else` the value is `()`.
     * @param value The `if` expression's text.
     * @param cursor A cursor over the text, just past `if`.
     * @return The outcomes.
     * @throws Spec_error If what follows the block is no `else`, or a branch is not visibly a token or a skip.
     */
    [[nodiscard]] Outcomes_t of_if(std::string_view value, Rust_cursor cursor) const;

    /**
     * @brief The outcomes of a `match`: the arms between the braces after the scrutinee, each a pattern, `=>`, then a
     *        block or a value up to the comma, read one scope deeper than the match, since the braces holding the arms
     *        are a block of the walk's own.
     * @param value The `match` expression's text.
     * @param cursor A cursor over the text, just past `match`.
     * @return The outcomes.
     * @throws Spec_error If the match does not end the value, an arm has no `=>`, or an arm is not visibly a token or a
     *         skip.
     */
    [[nodiscard]] Outcomes_t of_match(std::string_view value, Rust_cursor cursor) const;

    /**
     * @brief The outcome a constructor's argument makes, read against the variant's payload as of_result() reads a bare
     *        result, except that only `Ok` skips on a `Skip`, `Some` takes no constructor of the enum, and `Err` and
     *        `FilterResult::Error` are an error at the boundary whatever they hold.
     *
     * For a variant without a payload the wrapper may carry `()` or any variant of the enum, `Filter<T>`,
     * `FilterResult<T, T::Error>` and `Result<T, T::Error>` being results logos 0.15.1 takes from a callback, so an
     * expression the text does not decide decides the token: `Filter::Emit(if c { T::X } else { T::Y })` emits Y on the
     * one path where the rule's variant is X.
     * @param constructor The constructor's path without trivia.
     * @param text The argument's text.
     * @return The outcome.
     * @throws Spec_error If the argument is one the crate refuses for the variant.
     */
    [[nodiscard]] Outcome of_argument(std::string_view constructor, std::string_view text) const;

    /**
     * @brief The outcomes of a function this file defines, by its return type, or by its body where the type leaves the
     *        decision to the value.
     *
     * Under a return type of `Result<Skip, E>` the body decides which arm the function takes and not which value the Ok
     * arm carries: the arm's payload is `Skip` by the declared type, so `Ok(skip_here())` skips as `Ok(Skip)` does,
     * whatever the expression inside is and whether or not this reading can see through it.
     * @param path The function's path from the crate root, as Functions_t keys it.
     * @return The outcomes.
     * @throws Spec_error If the file defines no such function, or more than one, or a bodiless one the type does not
     *         decide.
     */
    [[nodiscard]] Outcomes_t of_function(std::string_view path) const;

    /**
     * @brief The function this file defines under a path, its definition where a trait declares it and an impl defines
     *        it.
     * @param path The function's path from the crate root, as Functions_t keys it.
     * @return The function.
     * @throws Spec_error If the file defines no such function, or more than one.
     */
    [[nodiscard]] const Function& defined_function(std::string_view path) const;

    /**
     * @brief Refuses the callback.
     * @param why The reason.
     * @throws Spec_error Always.
     */
    [[noreturn]] void fail(const std::string& why) const;

    /**
     * @brief The enum, its variants, and what the file defines and binds.
     */
    const Enum_context& context_;

    /**
     * @brief The variant the rule stands on, or std::nullopt for a `skip(...)` attribute's rule.
     */
    const std::optional<Variant>& variant_;

    /**
     * @brief The callback's text, empty when there is none.
     */
    std::string_view callback_;

    /**
     * @brief The rule's line, for refusals.
     */
    std::size_t line_;

    /**
     * @brief Whether `Self` names the enum in the text being read, as it does in a function declared in one of the
     *        enum's impl blocks.
     */
    bool self_is_enum_{false};

    /**
     * @brief The module the text being read stands in, as a path from the crate root: the enum's for its callbacks, and
     *        a named function's for that function's return type and body.
     */
    std::string module_;

    /**
     * @brief Whether the body being read returns a `Result<Skip, E>`, whose Ok arm carries a `Skip` by its declared
     *        type and so skips however the expression inside it is written.
     */
    bool ok_skips_{false};

    /**
     * @brief The body being read, when the reading is inside a named function's body, so that a block inside it is
     *        placed in the file; none for a closure, whose text the attribute carries.
     */
    std::optional<std::string_view> body_;

    /**
     * @brief Where the body's first byte stands in the file.
     */
    std::size_t body_at_{0};
};

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_CALLBACK_HPP
