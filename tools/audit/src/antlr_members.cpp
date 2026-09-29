#include "munch/tools/audit/antlr_members.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

#include "munch/tools/audit/c_tokens.hpp"
#include "munch/tools/audit/directives.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements antlr_members.hpp: the reading of one members action, Members_reader, is private to this unit.

/**
 * @brief One `members` action's code read as the target language reads it, refusing what stands in place of the
 *        runtime's own methods or runs when the lexer is built.
 *
 * The code is read by a tokenizer that splices no lines and ends a line comment at a carriage return: a comment ending
 * in a backslash hides nothing under it, and a name inside a comment or a string declares nothing. A method is a name,
 * a parameter list and a body, the body a braced block or `=>` and an expression, with a `throws` clause allowed
 * between; a call is not one, and neither is a name reached through a value. The members' own braces are part of the
 * code, so a member of the lexer class stands one brace deep; a method of a class declared inside the block is deeper
 * and belongs to that class.
 *
 * ANTLR writes the action's code into the body of the lexer class it generates, so a method defined there stands in
 * place of the runtime's own: `nextToken` decides what the next token is and where it ends, `emit` and `emitEOF` what a
 * match becomes, `skip` and `more` whether it becomes a token at all, the mode methods which rules the next match is
 * tried against, `reset` and `consume` where it begins, `setType` and `setChannel` which token it is, and `recover`
 * what an error consumes. The rules still describe the matches, but the tokens are no longer the matches, which is what
 * a certificate is about. A `skip();` inside another method is the ordinary way a rule's action asks for a skip and
 * stays read.
 */
class Members_reader
{
public:
    /**
     * @brief Binds the reader to one action's code.
     * @param code The action's code, its braces included.
     * @param line The line the code begins on, which a refusal names.
     * @param csharp Whether the target is C#, whose property accessors run when the property is used.
     * @param macros The macros the grammar's actions define, which a name in an initializer may stand for.
     */
    Members_reader(std::string_view code, std::size_t line, bool csharp, const Macros_t& macros);

    /**
     * @brief Reads the code at the class's own level, member by member, refusing an initializer block, an initializer
     *        that is more than a value, an accessor with a body and a method.
     * @throws Spec_error At the first of them.
     */
    void refuse() const;

private:
    /**
     * @brief The text of a token, empty past the last one.
     * @param index The token's index.
     * @return The text.
     */
    [[nodiscard]] std::string_view text(std::size_t index) const;

    /**
     * @brief Refuses a brace at the class's own level that no declaration opens, an initializer block, which runs when
     *        the lexer is built and may set its mode before any token, so what it does is out of sight.
     * @param index The brace's index.
     * @throws Spec_error If the brace opens one.
     */
    void refuse_initializer_block(std::size_t index) const;

    /**
     * @brief Where a brace initializer ends, when a brace at the class's own level opens one, its values read.
     *
     * A C++ field may be initialized with braces, `int initial{(setMode(IN), 0)};` or `Initializer startup{this};`,
     * whose constructor may set the mode through the lexer it is handed, which runs when the lexer is built as `=`
     * does, so `this` is no value an initializer may hold: a brace at the class's own level that a name opens, where
     * the declaration before it is no type's, `class Inner {`, and no method's, whose parameter list stands before its
     * body, is such an initializer, and it is read by the rule an `=` initializer is read by. An array's brackets may
     * stand between the name and the brace, `int initial[1]{...}`. A parenthesis inside an array's bound, `int a[(1)]`,
     * opens no parameter list. A type is defined where its keyword's name is followed by the body or a base clause,
     * `struct Inner {` and `class D : B {`; `struct Initializer startup{this};` names a type and declares a field,
     * whose initializer is read as any other's. A C# property's accessors, `{ get; set; }`, run when the property is
     * used and not when the lexer is built, and initialize nothing; under the C++ target `get` is a name like any
     * other, a type's among them, and no accessor.
     * @param index The brace's index.
     * @return The index of the closing brace, or std::nullopt where the brace opens no initializer.
     * @throws Spec_error If the initializer is more than a value, or an accessor has a body.
     */
    [[nodiscard]] std::optional<std::size_t> brace_initializer_end(std::size_t index) const;

    /**
     * @brief Where the statement a token stands in begins: just past the `;`, `{` or `}` before it.
     * @param index The token's index.
     * @return The index of the statement's first token.
     */
    [[nodiscard]] std::size_t statement_start(std::size_t index) const;

    /**
     * @brief Refuses an accessor with a body, `get { ... }` or `get => ...`, which is a method the runtime may call
     *        through a virtual property, `CharIndex` among them, and is refused as a method is; an auto-property's
     *        `get;` and `set;` run nothing of the grammar's own.
     * @param index The index of the brace opening the accessors.
     * @param close The index of the brace closing them.
     * @param declarator The index of the property's name.
     * @throws Spec_error If an accessor has a body.
     */
    void refuse_accessor_body(std::size_t index, std::size_t close, std::size_t declarator) const;

    /**
     * @brief Whether a token of a brace initializer is plain: a value, a dot, a comma, a brace, or a sign opening a
     *        number after a brace or a comma.
     * @param piece The token's index.
     * @return True when it is.
     */
    [[nodiscard]] bool is_plain_in_braces(std::size_t piece) const;

    /**
     * @brief Whether a token is a sign opening a number, `-` or `+` before a token beginning with a digit.
     * @param piece The token's index.
     * @return True when it is.
     */
    [[nodiscard]] bool opens_number(std::size_t piece) const;

    /**
     * @brief Whether a word of an initializer is a value that decides nothing of the scanner: a name, a number or a
     *        quoted literal, `this` excepted, and a name a macro of an action's defines, `START_IN_MODE` under `#define
     *        START_IN_MODE (setMode(IN), 0)` in the header, standing for its replacement, which is a value only when
     *        the macro is transparent.
     * @param word The word.
     * @return True when it is.
     */
    [[nodiscard]] bool is_value(std::string_view word) const;

    /**
     * @brief Refuses a field whose initializer is more than a value, which runs when the lexer is built and may decide
     *        its mode before any token.
     * @param name The field's name.
     * @throws Spec_error Always.
     */
    [[noreturn]] void refuse_value(std::string_view name) const;

    /**
     * @brief Where a field's `=` initializer ends, when a token at the class's own level is its `=`, its tokens read.
     *
     * A field's initializer runs when the lexer is built too, `int startupMode = (_mode = IN);` and `int initial[] = {
     * _mode = IN };` alike, and the Java runtime's `_mode` is a field the members can assign. An initializer that is
     * values, names and dotted names, with the commas, braces and brackets that group them and a sign opening a number,
     * decides nothing of the scanner; any other, an assignment, a call, a `new`, a step or an operator among them, is
     * out of sight and refused. The tokenizer keeps `==`, `<=`, `!=`, `+=` and `=>` as separate bytes, so the `=` of a
     * declaration is one with no operator byte beside it.
     * @param index The token's index.
     * @return The index of the `;` ending the initializer, or std::nullopt where the token is no declaration's `=`.
     * @throws Spec_error If the initializer is more than a value.
     */
    [[nodiscard]] std::optional<std::size_t> field_initializer_end(std::size_t index) const;

    /**
     * @brief Whether a token of an `=` initializer is plain: a value, a grouping, or a sign opening a number after the
     *        `=`, a comma or a brace.
     * @param piece The token's index.
     * @return True when it is.
     */
    [[nodiscard]] bool is_plain_after_equals(std::size_t piece) const;

    /**
     * @brief Refuses a method the members define at a name at the class's own level.
     *
     * The generated lexer calls its own methods, and which of them decide the tokens is the runtime's to know and not
     * this reading's: `emit` reaches the token's end through `getCharIndex`, and a list of the methods that matter
     * would be a guess at the runtime's virtual calls. A method the members define may therefore stand in place of one
     * the runtime calls at every token, and any one of them is refused. `sizeof(int)` inside an array's bound, `int
     * data[sizeof(int)]{0};`, is a call inside brackets and no method's parameter list.
     * @param index The token's index.
     * @throws Spec_error If a parameter list and a body follow the name.
     */
    void refuse_method(std::size_t index) const;

    /**
     * @brief The code's tokens.
     */
    std::vector<C_token> tokens_;

    /**
     * @brief The line the code begins on.
     */
    std::size_t line_;

    /**
     * @brief Whether the target is C#.
     */
    bool csharp_;

    /**
     * @brief The macros the grammar's actions define.
     */
    const Macros_t& macros_;
};

Members_reader::Members_reader(
        const std::string_view code, const std::size_t line, const bool csharp, const Macros_t& macros)
    : tokens_{java_tokens(code)}, line_{line}, csharp_{csharp}, macros_{macros}
{}

void Members_reader::refuse() const
{
    auto depth{0};

    for (std::size_t index{0}; index < tokens_.size(); ++index)
    {
        if (depth == 1 && text(index) == "{")
        {
            refuse_initializer_block(index);

            if (const auto close{brace_initializer_end(index)})
            {
                index = *close;

                continue;
            }
        }

        depth += text(index) == "{" ? 1 : text(index) == "}" ? -1 : 0;

        if (depth != 1)
        {
            continue;
        }

        if (const auto end{field_initializer_end(index)})
        {
            index = *end;

            continue;
        }

        refuse_method(index);
    }
}

std::string_view Members_reader::text(const std::size_t index) const
{
    return index < tokens_.size() ? std::string_view{tokens_[index].text} : std::string_view{};
}

void Members_reader::refuse_initializer_block(const std::size_t index) const
{
    const auto before{index > 0 ? text(index - 1) : std::string_view{}};

    if (before.empty() || before == ";" || before == "}" || before == "{" || before == "static")
    {
        throw Spec_error{
                "the lexer's members hold an initializer block, which runs when the lexer is built and may decide its "
                "mode before any token, so what it does is out of the audit's sight",
                line_};
    }
}

std::optional<std::size_t> Members_reader::brace_initializer_end(const std::size_t index) const
{
    auto declarator{index > 0 ? index - 1 : 0};

    for (auto groups{0}; declarator > 0 && (text(declarator) == "]" || groups > 0); --declarator)
    {
        groups += text(declarator) == "]" ? 1 : text(declarator) == "[" ? -1 : 0;
    }

    if (index == 0 || !starts_name(text(declarator)))
    {
        return std::nullopt;
    }

    const auto start{statement_start(index)};

    static constexpr std::string_view types[]{"class", "struct", "union", "enum", "interface", "record"};

    auto declares_type{false};

    auto parameters{false};

    for (auto piece{start}, brackets{0UZ}; piece < index; ++piece)
    {
        const auto keyword{std::ranges::find(types, text(piece)) != std::ranges::end(types)};

        declares_type = declares_type ||
                        (keyword && starts_name(text(piece + 1)) && (text(piece + 2) == "{" || text(piece + 2) == ":"));

        brackets += text(piece) == "[" ? 1 : text(piece) == "]" && brackets > 0 ? -1 : 0;

        parameters = parameters || (text(piece) == "(" && brackets == 0);
    }

    if (declares_type || parameters)
    {
        return std::nullopt;
    }

    const auto close{group_close(tokens_, index, "{", "}")};

    const auto accessors{
            csharp_ && (text(index + 1) == "get" || text(index + 1) == "set" || text(index + 1) == "init")};

    if (accessors)
    {
        refuse_accessor_body(index, close, declarator);

        return close;
    }

    for (auto piece{index + 1}; piece < close; ++piece)
    {
        if (!is_plain_in_braces(piece))
        {
            refuse_value(text(declarator));
        }
    }

    return close;
}

std::size_t Members_reader::statement_start(const std::size_t index) const
{
    auto start{index};

    while (start > 0 && text(start - 1) != ";" && text(start - 1) != "}" && text(start - 1) != "{")
    {
        --start;
    }

    return start;
}

void Members_reader::refuse_accessor_body(
        const std::size_t index, const std::size_t close, const std::size_t declarator) const
{
    for (auto piece{index + 1}; piece < close; ++piece)
    {
        if (text(piece) == "{" || (text(piece) == "=" && text(piece + 1) == ">"))
        {
            throw Spec_error{
                    std::format(
                            "the lexer's members define an accessor of {} with a body, and the generated lexer reads "
                            "its own properties to decide the tokens without this reading knowing which, so the tokens "
                            "the rules describe may not be the tokens the scanner emits",
                            text(declarator)),
                    line_};
        }
    }
}

bool Members_reader::is_plain_in_braces(const std::size_t piece) const
{
    const auto word{text(piece)};

    const auto sign{(text(piece - 1) == "{" || text(piece - 1) == ",") && opens_number(piece)};

    return word == "." || word == "," || word == "{" || word == "}" || sign || is_value(word);
}

bool Members_reader::opens_number(const std::size_t piece) const
{
    const auto word{text(piece)};

    return (word == "-" || word == "+") && !text(piece + 1).empty() && is_digit(text(piece + 1).front());
}

bool Members_reader::is_value(const std::string_view word) const
{
    const auto opaque_macro{macros_.contains(word) && plain_values(word, macros_).empty()};

    return (starts_name(word) && word != "this" && !opaque_macro) || word.starts_with('"') || word.starts_with('\'') ||
           is_digit(word.front());
}

void Members_reader::refuse_value(const std::string_view name) const
{
    throw Spec_error{
            std::format(
                    "the lexer's members initialize {} with more than a value, which runs when the lexer is built and "
                    "may decide its mode before any token, so what it does is out of the audit's sight",
                    name),
            line_};
}

std::optional<std::size_t> Members_reader::field_initializer_end(const std::size_t index) const
{
    static constexpr std::string_view operators[]{"=", "!", "<", ">", "+", "-", "*", "/", "%", "&", "|", "^"};

    const auto before{index > 0 ? text(index - 1) : std::string_view{}};

    if (text(index) != "=" || text(index + 1) == "=" || text(index + 1) == ">" ||
        std::ranges::find(operators, before) != std::ranges::end(operators))
    {
        return std::nullopt;
    }

    auto scan{index + 1};

    for (auto groups{0UZ}; scan < tokens_.size() && !(groups == 0 && text(scan) == ";"); ++scan)
    {
        groups += text(scan) == "(" || text(scan) == "{" || text(scan) == "[" ? 1 :
                  text(scan) == ")" || text(scan) == "}" || text(scan) == "]" ? -1 :
                                                                                0;
    }

    // The field's name stands before the `=`, an array declarator's brackets stepped over.
    auto field{index - 1};

    while (field > 0 && (text(field) == "[" || text(field) == "]"))
    {
        --field;
    }

    for (auto piece{index + 1}; piece < scan; ++piece)
    {
        if (!is_plain_after_equals(piece))
        {
            refuse_value(text(field));
        }
    }

    return scan;
}

bool Members_reader::is_plain_after_equals(const std::size_t piece) const
{
    const auto word{text(piece)};

    const auto grouping{word == "." || word == "," || word == "{" || word == "}" || word == "[" || word == "]"};

    const auto sign_stands{text(piece - 1) == "=" || text(piece - 1) == "," || text(piece - 1) == "{"};

    const auto signed_number{sign_stands && opens_number(piece)};

    return grouping || signed_number || is_value(word);
}

void Members_reader::refuse_method(const std::size_t index) const
{
    if (!starts_name(text(index)) || text(index + 1) != "(")
    {
        return;
    }

    auto bracketed{false};

    for (auto back{index}, opened{0UZ}; back > statement_start(index); --back)
    {
        opened += text(back - 1) == "[" ? 1 : text(back - 1) == "]" && opened > 0 ? -1 : 0;

        bracketed = bracketed || (text(back - 1) == "[" && opened == 1);
    }

    if (bracketed || (index > 0 && (text(index - 1) == "." || text(index - 1) == "::" || text(index - 1) == "new")))
    {
        return;
    }

    auto scan{group_close(tokens_, index + 1, "(", ")")};

    for (++scan; scan < tokens_.size() && text(scan) != "{" && text(scan) != ";" &&
                 !(text(scan) == "=" && text(scan + 1) == ">");
         ++scan)
    {
    }

    if (scan < tokens_.size() && text(scan) != ";")
    {
        throw Spec_error{
                std::format(
                        "the lexer's members define {}(), and the generated lexer calls its own methods to decide the "
                        "tokens without this reading knowing which, so the tokens the rules describe may not be the "
                        "tokens the scanner emits",
                        text(index)),
                line_};
    }
}

} // namespace

void refuse_lexer_class(
        const Lexer_spec& spec, const std::vector<Members_action>& members, const std::string_view actions_code)
{
    const auto target{[&spec]() -> std::string_view {
        for (const auto& option : spec.options)
        {
            if (option.starts_with("language="))
            {
                return std::string_view{option}.substr(9);
            }
        }

        return "Java";
    }()};

    // A superclass named by the grammar may define any method of the lexer's, `nextToken` among them, and its code is
    // not in the grammar to read, so a grammar naming one is refused by name.
    for (const auto& option : spec.options)
    {
        if (option.starts_with("superClass="))
        {
            throw Spec_error{
                    "the grammar sets superClass, and the class it names may define the methods that decide the "
                    "tokens, which are out of the audit's sight",
                    spec.line};
        }
    }

    static constexpr std::string_view known[]{"Java", "Cpp", "CSharp"};

    if (!members.empty() && std::ranges::find(known, target) == std::ranges::end(known))
    {
        throw Spec_error{
                std::format(
                        "the grammar's target language is {}, whose members declare a method otherwise than this "
                        "reading reads one, so whether one of them decides the tokens is out of the audit's sight",
                        target),
                members.front().line};
    }

    Macros_t macros;

    take_macros(actions_code, macros);

    for (const auto& [code, line] : members)
    {
        Members_reader{code, line, target == "CSharp", macros}.refuse();
    }
}

} // namespace munch::tools::audit
