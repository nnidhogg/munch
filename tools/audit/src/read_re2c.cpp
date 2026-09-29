#include "munch/tools/audit/read_re2c.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/re2c_block.hpp"
#include "munch/tools/audit/re2c_classes.hpp"
#include "munch/tools/audit/re2c_configuration.hpp"
#include "munch/tools/audit/re2c_regex.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements read_re2c.hpp: the included files' macros, a block's opener, the ranking of a scanner's rules and the
// reading of the file block by block are private to this unit.

/**
 * @brief A carried definition this block's flags cannot read, kept until the block says whether it names it.
 *
 * re2c compiles a definition's regex where it is used, so one another block wrote in a shape this block cannot read, a
 * byte beyond ASCII under the UTF-8 encoding among them, is refused where a pattern of this block names it and nowhere
 * else: a block that names none of them reads as re2c reads it.
 */
struct Unreadable_definition
{
    /**
     * @brief The name the definition binds.
     */
    std::string name;

    /**
     * @brief The refusal to raise where a pattern names it.
     */
    Spec_error refusal;
};

/**
 * @brief The kinds of block a re2c opener opens that carry rules or definitions.
 */
enum class Block_kind : std::uint8_t
{
    /**
     * @brief `!re2c`: a block whose definitions and configurations join the global scope.
     */
    global,

    /**
     * @brief `!local:re2c`: a block that reads the definitions and configurations so far and passes none of its own
     *        on.
     */
    local,

    /**
     * @brief `!rules:re2c`: a library for the blocks that use it, and no scanner itself.
     */
    rules,

    /**
     * @brief `!use:re2c`: a block that opens by using a rules block.
     */
    use,
};

/**
 * @brief A block's opener as re2c reads it: its kind, its name, where the block's text begins and the opener's line.
 */
struct Opener
{
    /**
     * @brief The block's kind.
     */
    Block_kind kind;

    /**
     * @brief The name after the opener's colon, `rules:re2c:name`, empty when it has none; a use block's names the
     *        block it uses.
     */
    std::string name;

    /**
     * @brief The offset just past the opener and its name.
     */
    std::size_t begin;

    /**
     * @brief The line the opener is on.
     */
    std::size_t line;
};

/**
 * @brief The line an offset of the file is on, counted from one.
 * @param source The file's text.
 * @param at The offset.
 * @return The line.
 */
[[nodiscard]] std::size_t line_at(const std::string_view source, const std::size_t at)
{
    return 1 + static_cast<std::size_t>(std::ranges::count(source.substr(0, at), '\n'));
}

/**
 * @brief Collects the definitions a regex's expression names: each reference, as reference_length() reads one, outside
 *        a quoted literal and a bracket, where the braces are text.
 * @param text The expression.
 * @param into The names, each reference's appended.
 */
void references(const std::string_view text, std::vector<std::string>& into)
{
    for (std::size_t at{0}; at < text.size(); ++at)
    {
        if (text[at] == '\\')
        {
            ++at;

            continue;
        }

        if (text[at] == '"' || text[at] == '[')
        {
            const auto close{text[at] == '"' ? '"' : ']'};

            for (++at; at < text.size() && text[at] != close; ++at)
            {
                at += text[at] == '\\' ? 1 : 0;
            }

            continue;
        }

        if (const auto length{reference_length(text, at)}; length > 0)
        {
            into.emplace_back(text.substr(at + 1, length - 2));
        }
    }
}

/**
 * @brief Reads the opener of a block that carries rules or definitions, at a comment opening with a bang.
 * @param source The file's text.
 * @param at The offset of the comment.
 * @return The opener, or std::nullopt for another block kind, which carries no rules.
 * @throws Spec_error If the opener runs on into anything but a blank, a newline, a colon and a name, or the block's
 *         close, which re2c refuses as an ill-formed start of a block: `!re2cx` after the comment's opening opens
 *         no block.
 */
[[nodiscard]] std::optional<Opener> opener_at(const std::string_view source, const std::size_t at)
{
    const auto rest{source.substr(at + 3)};

    const auto kind{
            rest.starts_with("re2c")       ? std::optional{Block_kind::global} :
            rest.starts_with("local:re2c") ? std::optional{Block_kind::local} :
            rest.starts_with("rules:re2c") ? std::optional{Block_kind::rules} :
            rest.starts_with("use:re2c")   ? std::optional{Block_kind::use} :
                                             std::nullopt};

    if (!kind)
    {
        return std::nullopt;
    }

    const auto length{
            kind == Block_kind::global ? std::size_t{4} :
            kind == Block_kind::use    ? std::size_t{8} :
                                         std::size_t{10}};

    const auto line{line_at(source, at)};

    // The block's name, `rules:re2c:name`, when it has one.
    std::string name;

    auto begin{at + 3 + length};

    if (source.substr(begin).starts_with(':'))
    {
        for (++begin; begin < source.size() && is_name_byte(source[begin]); ++begin)
        {
            name.push_back(source[begin]);
        }
    }

    // re2c ends the opener at a blank, a newline or the block's close.
    if (begin < source.size() && source[begin] != ' ' && source[begin] != '\t' && source[begin] != '\r' &&
        source[begin] != '\n' && !source.substr(begin).starts_with("*/"))
    {
        throw Spec_error{
                "ill-formed start of a block: re2c expects a blank, a newline, a colon and a name, or the block's "
                "close "
                "after the opener",
                line};
    }

    return Opener{.kind = *kind, .name = std::move(name), .begin = begin, .line = line};
}

/**
 * @brief Where a block of another kind ends, which carries no rules and is skipped through its close like any comment.
 * @param source The file's text.
 * @param at The offset of the comment opening it.
 * @return The offset just past its close.
 * @throws Spec_error If it is never closed.
 */
[[nodiscard]] std::size_t other_block_end(const std::string_view source, const std::size_t at)
{
    const auto close{source.find("*/", at + 3)};

    if (close == std::string_view::npos)
    {
        throw Spec_error{"a re2c block is never closed", line_at(source, at)};
    }

    return close + 2;
}

/**
 * @brief Places a scanner's rules as re2c 3.1 ranks them and records the conditions they name.
 *
 * A rule's index ranks it among the rules matching one lexeme, so the rules are placed as re2c 3.1 ranks them, whatever
 * their order in the block: a condition's own rules first, then the `<*>` rules, which re2c appends to each condition's
 * own, and the default rules after them all in the same two ranks, since a default rule has the lowest priority and a
 * condition's own beats the `<*>` one; each rank keeps the order it was read in, which is the file's, a used block's
 * rules standing where their directive does. re2c declares no conditions; the ones the block's rules name are the
 * scanner's, the end rule and the empty rule naming them as any rule does, and each is exclusive, since a rule is
 * active in a condition only by naming it or by `<*>`. INITIAL is the name the default condition is reported under, so
 * a rule naming it names that one, inclusive as it is everywhere, and the scanner is recorded as having it. A `<*>`
 * rule is appended to each condition the block's rules name, which is what re2c compiles, so it stands in those
 * conditions and in no other: a scanner whose rules all name conditions has no INITIAL for it to stand in. A block
 * whose rules name only `<*>` gives re2c no condition to compile them into and is refused, since the rules would stand
 * nowhere.
 * @param spec The scanner, its rules read in the block's order.
 * @param kinds The kinds of rule the scanner holds and the names they stand in.
 * @throws Spec_error If a rule names `<*>` where no rule names a condition.
 */
void place_rules(Lexer_spec& spec, const Rule_kinds& kinds)
{
    // The rules in re2c's ranks, each rank in the file's order.
    const auto rank{[](const Lexer_spec::Rule& rule) {
        return (rule.pattern == "*" ? 2 : 0) + (std::ranges::contains(rule.conditions, "*") ? 1 : 0);
    }};

    std::ranges::stable_sort(spec.rules, std::ranges::less{}, rank);

    // The conditions the rules name, each exclusive but INITIAL.
    for (const auto& name : kinds.named())
    {
        spec.conditions.push_back({.name = name, .exclusive = name != "INITIAL"});
    }

    // A `<*>` rule stands in each condition the rules name and in no other.
    for (auto& rule : spec.rules)
    {
        if (!std::ranges::contains(rule.conditions, "*"))
        {
            continue;
        }

        if (kinds.named().empty())
        {
            throw Spec_error{
                    "the rule names `<*>` where no rule of the block names a condition, so re2c compiles no condition "
                    "for it to stand in",
                    rule.line};
        }

        rule.conditions = kinds.named();
    }
}

/**
 * @brief The names a scanner's rules reach: every `{name}` reference in a rule's expression, and through the
 *        definitions those name, the references in theirs, however many definitions deep.
 *
 * re2c compiles a definition's regex where a rule uses it, so a definition no rule reaches, directly or through a chain
 * of definitions, is compiled nowhere in the block, and an alias of an unreadable definition is as unused as the
 * definition itself while no rule names either.
 * @param spec The specification, its rules and definitions read.
 * @return The names, whether or not a definition binds each.
 */
[[nodiscard]] std::set<std::string, std::less<>> reached(const Lexer_spec& spec)
{
    std::set<std::string, std::less<>> names;

    std::vector<std::string> pending;

    for (const auto& rule : spec.rules)
    {
        references(rule.expression, pending);
    }

    while (!pending.empty())
    {
        auto name{std::move(pending.back())};

        pending.pop_back();

        if (!names.insert(name).second)
        {
            continue;
        }

        if (const auto definition{spec.definitions.find(name)}; definition != spec.definitions.end())
        {
            references(definition->second, pending);
        }
    }

    return names;
}

/**
 * @brief Takes the macros of the files the code includes: a file the code includes defines macros as the file's own
 *        code does, `#define SLOT 0` among them, so its text is read for them, its own includes after it and beside
 *        it.
 *
 * An include is refused where no reader reaches the file, since what it defines is out of sight; an angle-bracket
 * include the reader does not find is a system header's, and one named by a macro is out of sight.
 * @param source The file's text.
 * @param includes How the caller reaches an included file.
 * @param macros The macros the file defines, the included files' added.
 * @throws Spec_error If an include is refused, or the code includes more files than the reading follows.
 */
void follow_includes(const std::string_view source, const Include_reader_t& includes, Macros_t& macros)
{
    std::vector<Included> texts{{.text = std::string{source}, .path = {}}};

    for (std::size_t at{0}; at < texts.size(); ++at)
    {
        for (const auto& directive : includes_of(texts[at].text))
        {
            const auto where{at == 0 ? directive.line + 1 : 1};

            const auto included{included_file(
                    directive, texts[at].path, includes, "the macros an action may use among them", where)};

            if (!included)
            {
                continue;
            }

            if (texts.size() > 64)
            {
                throw Spec_error{"the code includes more files than the reading follows", where};
            }

            take_macros(included->text, macros);

            texts.push_back(*included);
        }
    }
}

/**
 * @brief What the global blocks read so far leave the blocks after them: re2c carries a block's configurations and
 *        definitions into the global scope, and a local block's, a rules block's and a use block's stay in it.
 */
struct Carried
{
    /**
     * @brief The flags: the command line's, with the global blocks' configurations and the evidence of the flex syntax
     *        applied.
     */
    Re2c_flags flags;

    /**
     * @brief The configurations, as options.
     */
    std::vector<std::string> options;

    /**
     * @brief Where each definition's regex stands, so that the block using it translates the regex itself, never the
     *        rules.
     */
    std::vector<Definition_site> definitions;

    /**
     * @brief What the blocks so far call the scan pointers: re2c carries a `define:` configuration from the block it
     *        stands in to the blocks after it, so a block renaming none writes what the one above it left, and the
     *        file's last word on a name is the one its actions are read under.
     */
    Pointers_t pointers;

    /**
     * @brief Whether the blocks so far left the scanner under an API other than the default one, and where, carried as
     *        the pointer names are.
     */
    std::optional<std::size_t> api_custom;
};

/**
 * @brief What a pass over a block read: its scanner, where its definitions stand, the kinds of its rules, what its
 *        configurations leave, and where the block ends.
 */
struct Block_reading
{
    /**
     * @brief The scanner, its rules in the block's order.
     */
    Lexer_spec spec;

    /**
     * @brief Where each definition the block declared stands.
     */
    std::vector<Definition_site> sites;

    /**
     * @brief The kinds of rule the block holds and the names they stand in.
     */
    Rule_kinds kinds;

    /**
     * @brief The flags the block's configurations leave.
     */
    Re2c_flags flags;

    /**
     * @brief The pointer names the block's configurations leave.
     */
    Pointers_t pointers;

    /**
     * @brief The API setting the block's configurations leave.
     */
    std::optional<std::size_t> api_custom;

    /**
     * @brief The offset just past the block's close.
     */
    std::size_t end;
};

/**
 * @brief The re2c blocks of a file read in order, each under the flags in force where it is used, into the scanners
 *        they are.
 *
 * Configurations govern the whole block, wherever in it they stand, and of two assignments to one name the last is the
 * one that governs, so a pass over a block records what its configurations leave and translates no pattern under them:
 * the pass that reads under what they leave is the one the block means, and a block is read again until its pass reads
 * under what it leaves.
 */
class File_reader
{
public:
    /**
     * @brief Binds the reader to the file.
     * @param source The file's text.
     * @param flags The command line's flags.
     * @param returning The forms besides `return` an action returns a token through.
     * @param macros The macros the C around the blocks defines, the included files' among them.
     */
    File_reader(std::string_view source, Re2c_flags flags, const Returning_t& returning, const Macros_t& macros);

    /**
     * @brief Reads every block of the file.
     * @return The scanners, in file order.
     * @throws Spec_error If a block is refused.
     */
    [[nodiscard]] std::vector<Lexer_spec> read();

private:
    /**
     * @brief Reads one block: its passes, what it leaves the blocks after it, re2c's checks of it, and its scanner.
     * @param opener The block's opener.
     * @return The offset just past the block's close.
     * @throws Spec_error If the block is refused.
     */
    [[nodiscard]] std::size_t block(const Opener& opener);

    /**
     * @brief Where the rules block a use block uses begins: a use block names the rules block it uses, not itself, and
     *        with no name it uses the most recent one.
     * @param opener The block's opener.
     * @return The offset just past the used block's opener, or std::nullopt for a block that uses none.
     * @throws Spec_error If a use block names no block above it, or names none where no rules block is above it.
     */
    [[nodiscard]] std::optional<std::size_t> used_block(const Opener& opener) const;

    /**
     * @brief Reads a block until a pass reads it under the flags its configurations leave.
     *
     * The first pass reads under the inherited flags with the encoding at ASCII, the encoding that refuses the fewest
     * patterns, so that a block turning one off is read under what it left and never refused for what it inherited.
     * What the configurations leave is the inherited flags with the block's own applied, whichever pass reads it, and
     * the evidence of the flex syntax only ever turns its flag on, so the reading settles.
     * @param opener The block's opener.
     * @param used Where the rules block a use block uses begins.
     * @return What the settled pass read.
     * @throws Spec_error As pass() does.
     */
    [[nodiscard]] Block_reading settled(const Opener& opener, std::optional<std::size_t> used);

    /**
     * @brief Reads a block once, under the flags given; a use block's rules are the used block's, read at the head of
     *        the pass under the same flags as its own.
     *
     * A refusal the flags decide is the settled pass's to make: a class difference empty under the ASCII the first pass
     * reads under, `[^] \ [\x00-\xff]`, holds every code point past the bytes once the block turns UTF-8 on, and one
     * empty under the case flags inherited, `"a" \ 'A'`, holds the exact `a` once the block inverts which quote folds.
     * It is a scanner's to make, at that: a rules block's and a block of definitions alone are compiled where they are
     * used, under the flags there, and are refused there.
     * @param opener The block's opener.
     * @param used Where the rules block a use block uses begins.
     * @param reading The flags the pass translates the block's patterns under.
     * @return What the pass read.
     * @throws Spec_error If the block is refused, or its configurations leave an encoding the reading has not got; and
     *         for the pass reading under the flags the block leaves, if a rule reaches a carried definition those flags
     *         cannot read, or a class difference is left empty.
     */
    [[nodiscard]] Block_reading pass(const Opener& opener, std::optional<std::size_t> used, Re2c_flags reading);

    /**
     * @brief Translates the definitions the blocks above left again, under the flags this block reads its own patterns
     *        under: re2c compiles a definition's regex at every point of use, so `point = [^];` written where the
     *        encoding was ASCII admits one byte there and a whole code point in a block that turns UTF-8 on.
     * @param reading The flags the pass reads under.
     * @param spec The scanner, the definitions added to it.
     * @param pass What the blocks of the pass share, the definitions' classes added to it.
     * @return The definitions these flags cannot read, which wait for the block to name them.
     */
    [[nodiscard]] std::vector<Unreadable_definition> carried_definitions(
            Re2c_flags reading, Lexer_spec& spec, Pass_state& pass) const;

    /**
     * @brief Carries what a global block leaves into the global scope; a name the block declared again keeps the place
     *        it had, since a definition's own regex may name the ones declared before it.
     * @param outcome What the block's settled pass read.
     */
    void carry(const Block_reading& outcome);

    /**
     * @brief The file's text.
     */
    std::string_view source_;

    /**
     * @brief The forms besides `return` an action returns a token through.
     */
    const Returning_t& returning_;

    /**
     * @brief The macros the C around the blocks defines, every `#define` of the file's: an action calling one that
     *        returns or moves a pointer is refused by the macro's name, since re2c copies the action as written and
     *        the compiler expands it there.
     */
    const Macros_t& macros_;

    /**
     * @brief What the global blocks read so far leave the blocks after them.
     */
    Carried carried_;

    /**
     * @brief The blocks a later one may use by name; the unnamed rules block under the empty name.
     */
    Library_t library_;

    /**
     * @brief The rules block a use block with no name of its own refers to: the most recent one, named or not.
     */
    std::optional<std::string> recent_;

    /**
     * @brief The scanners read so far.
     */
    std::vector<Lexer_spec> scanners_;
};

File_reader::File_reader(
        const std::string_view source, const Re2c_flags flags, const Returning_t& returning, const Macros_t& macros)
    : source_{source}
    , returning_{returning}
    , macros_{macros}
    , carried_{
              .flags = flags,
              .options = {},
              .definitions = {},
              .pointers =
                      {{.canonical = "YYCURSOR", .name = "YYCURSOR"},
                       {.canonical = "YYMARKER", .name = "YYMARKER"},
                       {.canonical = "YYCTXMARKER", .name = "YYCTXMARKER"}},
              .api_custom = std::nullopt}
{}

std::vector<Lexer_spec> File_reader::read()
{
    for (auto at{source_.find("/*!")}; at != std::string_view::npos; at = source_.find("/*!", at))
    {
        const auto opener{opener_at(source_, at)};

        at = opener ? block(*opener) : other_block_end(source_, at);
    }

    return std::move(scanners_);
}

std::size_t File_reader::block(const Opener& opener)
{
    const auto rules{opener.kind == Block_kind::rules};

    const auto used{used_block(opener)};

    auto outcome{settled(opener, used)};

    if (rules)
    {
        recent_ = opener.name;
    }

    if (rules || (opener.kind != Block_kind::use && !opener.name.empty()))
    {
        library_.insert_or_assign(opener.name, opener.begin);
    }

    // A global block's names and configurations join the global scope; a local block's, a rules block's and a use
    // block's stay in it, so a use block that asks for an encoding leaves the blocks after it as they were.
    if (opener.kind == Block_kind::global)
    {
        carry(outcome);
    }

    // A rules block is a library for the blocks that use it and no scanner itself: re2c holds it to nothing until a use
    // block takes it up.
    if (rules)
    {
        return outcome.end;
    }

    outcome.kinds.refuse_end_rules(outcome.spec.options, outcome.spec.line);

    if (!outcome.spec.rules.empty())
    {
        place_rules(outcome.spec, outcome.kinds);

        scanners_.push_back(std::move(outcome.spec));
    }

    return outcome.end;
}

std::optional<std::size_t> File_reader::used_block(const Opener& opener) const
{
    if (opener.kind != Block_kind::use)
    {
        return std::nullopt;
    }

    if (opener.name.empty() && !recent_)
    {
        throw Spec_error{"a use block with no name needs a rules block above it", opener.line};
    }

    const auto& name{opener.name.empty() ? *recent_ : opener.name};

    const auto used{library_.find(name)};

    if (used == library_.end())
    {
        throw Spec_error{"the used block '" + name + "' is not above this one", opener.line};
    }

    return used->second;
}

Block_reading File_reader::settled(const Opener& opener, const std::optional<std::size_t> used)
{
    auto reading{carried_.flags};

    reading.encoding = Re2c_encoding::ascii;

    for (;;)
    {
        auto outcome{pass(opener, used, reading)};

        if (outcome.flags == reading)
        {
            return outcome;
        }

        reading = outcome.flags;
    }
}

Block_reading File_reader::pass(const Opener& opener, const std::optional<std::size_t> used, const Re2c_flags reading)
{
    const auto rules{opener.kind == Block_kind::rules};

    Lexer_spec spec;

    spec.parse = re2c_parse;

    spec.line = opener.line;

    spec.options = carried_.options;

    // The classes among the definitions, which a class difference takes its operands from, are the pass's as the
    // definitions are, and each pass begins from the pointer names and the API the blocks above this one left, as each
    // pass rereads this block alone.
    Pass_state state{.classes = {}, .pointers = carried_.pointers, .api_custom = carried_.api_custom};

    const auto unreadable_definitions{carried_definitions(reading, spec, state)};

    Block_reader block{source_, opener.begin, reading, carried_.flags, state, macros_};

    if (rules)
    {
        block.judge_later();
    }

    if (used)
    {
        block.use(*used, spec, library_, returning_);
    }

    const auto end{block.read(spec, library_, returning_)};

    // re2c compiles a rules block where it is used and refuses rules of both kinds there, so a rules block is checked
    // as part of the scanners using it and never on its own.
    if (!rules)
    {
        block.kinds().refuse_mixed();
    }

    // The encoding the configurations leave is what the block reads under, whichever pass; one the reading has not got
    // is refused before any pass reads under it.
    if (const auto refusal{unreadable(block.configured().encoding)}; !refusal.empty())
    {
        throw Spec_error{refusal, block.encoding_line()};
    }

    Block_reading outcome{
            .spec = std::move(spec),
            .sites = block.sites(),
            .kinds = block.kinds(),
            .flags = block.configured(),
            .pointers = std::move(state.pointers),
            .api_custom = state.api_custom,
            .end = end};

    if (outcome.flags != reading)
    {
        return outcome;
    }

    // A definition the settled flags cannot read is refused where a rule of this block reaches it, directly or through
    // the definitions the rule names, which is where re2c would compile it; one no rule reaches is compiled nowhere and
    // left out, and so is an alias of it that no rule names.
    const auto named{reached(outcome.spec)};

    for (const auto& [name, refusal] : unreadable_definitions)
    {
        if (named.contains(name))
        {
            throw refusal;
        }
    }

    // A class difference left empty is refused by the settled pass, and only in a scanner.
    if (block.deferred() && !rules && !outcome.spec.rules.empty())
    {
        throw *block.deferred();
    }

    return outcome;
}

std::vector<Unreadable_definition> File_reader::carried_definitions(
        const Re2c_flags reading, Lexer_spec& spec, Pass_state& pass) const
{
    std::vector<Unreadable_definition> refused;

    for (const auto& [name, at_definition, line_bound] : carried_.definitions)
    {
        // A flex-style definition's body is read under the flex syntax, whose literals bare names are, and ends where
        // its line does; the re2c spelling ends at the `;` that closes it.
        auto flags{reading};

        flags.flex_syntax = flags.flex_syntax || line_bound;

        std::optional<Spec_error> deferred;

        try
        {
            Regex_reader body{source_, at_definition, flags, line_bound, pass.classes, deferred};

            auto [pattern, expression, points]{body.regex_text(spec.definitions)};

            if (deferred)
            {
                throw *deferred;
            }

            spec.definitions.insert_or_assign(name, std::move(expression));

            note_class(pass.classes, name, std::move(points));
        }
        catch (const Spec_error& refusal)
        {
            spec.definitions.erase(name);

            pass.classes.erase(name);

            refused.push_back({.name = name, .refusal = refusal});
        }
    }

    return refused;
}

void File_reader::carry(const Block_reading& outcome)
{
    carried_.flags = outcome.flags;

    carried_.pointers = outcome.pointers;

    carried_.api_custom = outcome.api_custom;

    carried_.options = outcome.spec.options;

    for (const auto& site : outcome.sites)
    {
        const auto known{std::ranges::find(carried_.definitions, site.name, &Definition_site::name)};

        if (known == carried_.definitions.end())
        {
            carried_.definitions.push_back(site);
        }
        else
        {
            *known = site;
        }
    }
}

} // namespace

std::vector<Lexer_spec> read_re2c(
        const std::string_view source, const Re2c_flags flags, const Returning_t& returning,
        const Include_reader_t& includes)
{
    // An encoding comes from the command line as well as from a configuration, and the reading models the same two
    // either way, so the flags the caller passes are refused where a configuration setting them would be.
    if (const auto refusal{unreadable(flags.encoding)}; !refusal.empty())
    {
        throw Spec_error{refusal, 1};
    }

    Macros_t macros;

    take_macros(source, macros);

    follow_includes(source, includes, macros);

    return File_reader{source, flags, returning, macros}.read();
}

} // namespace munch::tools::audit
