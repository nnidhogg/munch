#include "munch/tools/audit/read_re2c.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "munch/regex/parse.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/audit/cursor.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The bytes a regex names when it is a class: a bracket, a one-byte literal, or an alternation of classes,
 *        which is what re2c lets the operands of its class difference be.
 * @param regex The regex.
 * @return The bytes, or std::nullopt when the regex is no class.
 */
[[nodiscard]] std::optional<regex::Set> class_of(const regex::Regex& regex)
{
    return std::visit(
            []<typename Node>(const Node& node) -> std::optional<regex::Set> {
                if constexpr (std::is_same_v<Node, regex::Any_of>)
                {
                    return node.set;
                }
                else if constexpr (std::is_same_v<Node, regex::Text>)
                {
                    return node.text.size() == 1 ? std::optional{regex::Set{node.text.front()}} : std::nullopt;
                }
                else if constexpr (std::is_same_v<Node, regex::Choice>)
                {
                    regex::Set all;

                    for (const auto& branch : node.regexes)
                    {
                        const auto bytes{class_of(branch)};

                        if (!bytes)
                        {
                            return std::nullopt;
                        }

                        all += *bytes;
                    }

                    return all;
                }
                else
                {
                    return std::nullopt;
                }
            },
            regex.node);
}

/**
 * @brief The blocks read so far by name, a `rules:re2c:name` block or any other block opened with a name, which a
 *        later `!use:name;` merges into the block using it: its definitions, its configurations and its rules.
 */
using Library_t = std::map<std::string, Lexer_spec, std::less<>>;

/**
 * @brief Merges a used block into the block using it: definitions the user has not got, and every configuration and
 *        rule, the rules in the order the used block gave them.
 * @param used The block named by the directive.
 * @param spec The specification being filled.
 */
void merge(const Lexer_spec& used, Lexer_spec& spec)
{
    for (const auto& [name, body] : used.definitions)
    {
        spec.definitions.emplace(name, body);
    }

    spec.options.insert(spec.options.end(), used.options.begin(), used.options.end());

    spec.rules.insert(spec.rules.end(), used.rules.begin(), used.rules.end());
}

/**
 * @brief A cursor over one re2c block, from just past its opener to the comment close that ends it.
 *
 * The block is read item by item: a configuration, a definition or a rule, each ending where re2c's own grammar ends
 * it, and the regex text of a definition or a rule is rewritten for the pattern parser as it is read. The close is
 * found the way re2c finds it, as the first star-slash between items: one inside a quoted literal, a class, an
 * action or a comment is content, since the file is read by re2c and not by a C compiler. Definitions come in two
 * spellings, re2c's `name = regex;` and the flex one `name regex` on a line of its own, which re2c accepts with its
 * flex-syntax flag and which a name at the start of a line followed by more regex and no action on that line
 * identifies.
 */
class Block : public Cursor
{
public:
    /**
     * @brief Binds the cursor to the source just past a block's opener.
     * @param source The whole file.
     * @param begin The offset just past the opener.
     * @param flags The flags in force when the block opens.
     */
    Block(std::string_view source, std::size_t begin, Re2c_flags flags);

    /**
     * @brief Reads every item of the block into the specification, through the block's close.
     * @param spec The specification being filled.
     * @param library The named blocks read so far, which a `!use:name;` item merges into the specification.
     * @param returning The forms besides `return` an action returns a token through.
     * @return The offset just past the close.
     * @throws Spec_error If an item is malformed or left open, a used block is unknown, or the block never closes.
     */
    [[nodiscard]] std::size_t read(Lexer_spec& spec, const Library_t& library, const Returning_t& returning);

    /**
     * @brief The flags in force when the block closed, for the next block: a configuration or the evidence of the
     *        flex syntax may have changed them.
     * @return The flags.
     */
    [[nodiscard]] Re2c_flags flags() const noexcept;

private:
    /**
     * @brief Reads a `re2c:` configuration through its `;` into the options, a flag among them into the flags.
     * @param spec The specification being filled.
     */
    void configuration(Lexer_spec& spec);

    /**
     * @brief Reads a `<...>` condition list after its `<`, through its `>`.
     * @return The names, `*` for all; std::nullopt for a `<!...>` setup rule, which is no token.
     */
    [[nodiscard]] std::optional<std::vector<std::string>> conditions();

    /**
     * @brief Reads regex text up to what ends it: a bare `=` for a definition, a `;` closing a definition's body, or
     *        the start of an action, `{`, `:=` or `=>`.
     *
     * The text as written and the text rewritten for the pattern parser are both returned, the rewriting done token
     * by token: bare names become `{name}` unless the flex syntax makes them literals, quoted literals other than an
     * exact double-quoted one become bracket sequences, blanks are dropped, and a class difference `A \ B` becomes
     * the bracket of the bytes left, its operands parsed against the definitions so far.
     * @param definitions The definitions read so far, which a difference's operand may name.
     * @return The pattern as written and its expression, both empty when an action follows at once.
     * @throws Spec_error If a quote or bracket is left open, or the regex uses a refused construct.
     */
    [[nodiscard]] std::pair<std::string, std::string> regex_text(const regex::Definitions_t& definitions);

    /**
     * @brief Reads an action starting at the cursor: a brace block, or `:=` and the rest of the line, a `=> c` or
     *        `:=> c` transition included in the text.
     * @return The action's text.
     * @throws Spec_error If a brace block never closes.
     */
    [[nodiscard]] std::string action();

    /**
     * @brief A quoted literal after its opening quote, through the closing one, as a bracket sequence, one bracket per
     *        byte and an escape kept as written inside its bracket.
     * @param quote The closing quote.
     * @param insensitive Whether a letter's bracket holds both cases.
     * @return The expression.
     * @throws Spec_error If the literal is empty or never closes, or an escape is a Unicode one.
     */
    [[nodiscard]] std::string literal(char quote, bool insensitive);

    /**
     * @brief The length of a flex-style reference at the cursor, `{name}` exactly, or zero when there is none.
     * @return The length, brackets included.
     */
    [[nodiscard]] std::size_t reference_length() const noexcept;

    /**
     * @brief The flags in force.
     */
    Re2c_flags flags_;

    /**
     * @brief Whether regex text ends at the line's end, which a flex-style definition's does.
     */
    bool line_bound_{false};
};

Block::Block(const std::string_view source, const std::size_t begin, const Re2c_flags flags)
    : Cursor{source, begin, source.size()}, flags_{flags}
{}

std::size_t Block::read(Lexer_spec& spec, const Library_t& library, const Returning_t& returning)
{
    for (skip_blanks(); !at("*/"); skip_blanks())
    {
        if (!peek())
        {
            fail("the block never closes");
        }

        if (at("re2c:"))
        {
            configuration(spec);

            continue;
        }

        if (at("!use:"))
        {
            at_ += 5;

            std::string name;

            while (peek() && is_name_byte(*peek()))
            {
                name.push_back(next("a block name"));
            }

            skip_blanks();

            expect(';', "';' to end the use directive");

            const auto found{library.find(name)};

            if (found == library.end())
            {
                fail("the used block '" + name + "' is not above this one");
            }

            merge(found->second, spec);

            continue;
        }

        if (at("!include"))
        {
            fail("the block includes a file, which is not here to read");
        }

        const auto line{this->line()};

        const auto at_line_start{at_ == 0 || text_[at_ - 1] == '\n'};

        std::optional<std::vector<std::string>> named{std::vector<std::string>{}};

        if (peek() == '<')
        {
            ++at_;

            named = conditions();
        }

        // A flex-style definition, as re2c's flex syntax reads one: a name opening the line and followed by a blank,
        // with the regex after it running to the end of the line and no action there. Read as a trial, bound to the
        // line, and rewound when an action turns up on it after all. Without the flex flag the trial is made only
        // for a name no definition has, since a defined one opening a rule is normal syntax and an undefined one
        // opening a line is nothing else.
        if (at_line_start && named && named->empty() && is_name_byte(*peek()) && !(*peek() >= '0' && *peek() <= '9'))
        {
            const auto opened{at_};

            const auto name_end{std::min(
                    text_.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", at_),
                    text_.size())};

            const std::string name{text_.substr(at_, name_end - at_)};

            const auto blank_after{name_end < text_.size() && (text_[name_end] == ' ' || text_[name_end] == '\t')};

            if (blank_after && (flags_.flex_syntax || !spec.definitions.contains(name)))
            {
                const auto line_end{std::min(text_.find('\n', at_), text_.size())};

                // The body is read under the flex syntax, whose literals bare names are; the flag stays if it is one.
                const auto flex_before{std::exchange(flags_.flex_syntax, true)};

                at_ = name_end;

                line_bound_ = true;

                auto [body, expression]{regex_text(spec.definitions)};

                line_bound_ = false;

                if (at_ >= line_end && !body.empty())
                {
                    spec.definitions.insert_or_assign(name, expression);

                    continue;
                }

                flags_.flex_syntax = flex_before;

                at_ = opened;
            }
        }

        auto [pattern, expression]{regex_text(spec.definitions)};

        // re2c's own definition: a name, `=`, its body and `;`. The name was read as regex text, one bare name.
        if (peek() == '=' && !at("=>"))
        {
            ++at_;

            auto [body, body_expression]{regex_text(spec.definitions)};

            if (body.empty())
            {
                fail("the definition '" + pattern + "' has no regex");
            }

            if (peek() != ';')
            {
                fail("expected ';' to close the definition '" + pattern + "'");
            }

            ++at_;

            spec.definitions.insert_or_assign(pattern, body_expression);

            continue;
        }

        if (named && pattern.empty())
        {
            fail("a rule has no regex");
        }

        auto code{action()};

        // The default rule, the end rule and setup rules, which have no regex at all, are not tokens, and neither is
        // the empty rule `""`, with or without trailing context, which consumes nothing where nothing else matches.
        const auto empty{
                (pattern.starts_with("\"\"") || pattern.starts_with("''")) &&
                (pattern.size() == 2 || pattern.find_first_not_of(' ', 2) == pattern.find('/', 2))};

        if (!named || pattern == "*" || pattern == "$" || empty)
        {
            continue;
        }

        auto token{returned(code, returning)};

        spec.rules.push_back(
                {.pattern = std::move(pattern),
                 .expression = std::move(expression),
                 .conditions = std::move(*named),
                 .action = std::move(code),
                 .token = std::move(token),
                 .priority = std::nullopt,
                 .line = line});
    }

    return at_ + 2;
}

Re2c_flags Block::flags() const noexcept
{
    return flags_;
}

void Block::configuration(Lexer_spec& spec)
{
    // The value may be a quoted string holding a ';' of its own, as a YYFILL definition usually does.
    auto end{at_};

    while (end < end_ && text_[end] != ';')
    {
        if (text_[end] == '"' || text_[end] == '\'')
        {
            for (const auto quote{text_[end++]}; end < end_ && text_[end] != quote; ++end)
            {
                end += text_[end] == '\\' ? 1 : 0;
            }
        }

        ++end;
    }

    if (end >= end_)
    {
        fail("a configuration is never closed with ';'");
    }

    std::string option{text_.substr(at_ + 5, end - at_ - 5)};

    // Blanks around the '=' say nothing; one spelling per configuration keeps the options comparable.
    std::erase_if(option, [](const char byte) { return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r'; });

    // The flags that change the reading are honoured when set in the file, `re2c:flags:name = 1;`.
    const auto set{[&option](const std::string_view name) -> std::optional<bool> {
        const auto key{"flags:" + std::string{name} + '='};

        return option.starts_with(key) ? std::optional{option.substr(key.size()) != "0"} : std::nullopt;
    }};

    if (const auto value{set("case-inverted")})
    {
        flags_.case_inverted = *value;
    }

    if (const auto value{set("case-insensitive")})
    {
        flags_.case_insensitive = *value;
    }

    if (const auto value{set("flex-syntax").or_else([&set] { return set("F"); })})
    {
        flags_.flex_syntax = *value;
    }

    spec.options.push_back(std::move(option));

    at_ = end + 1;
}

std::optional<std::vector<std::string>> Block::conditions()
{
    const auto setup{peek() == '!'};

    std::vector<std::string> names;

    std::string name;

    for (;;)
    {
        const auto byte{next("'>' to close the condition list")};

        if (byte == '>' || byte == ',')
        {
            if (!name.empty())
            {
                names.push_back(std::exchange(name, {}));
            }

            if (byte == '>')
            {
                break;
            }

            continue;
        }

        if (byte != ' ' && byte != '\t' && byte != '!')
        {
            name.push_back(byte);
        }
    }

    return setup ? std::nullopt : std::optional{std::move(names)};
}

std::pair<std::string, std::string> Block::regex_text(const regex::Definitions_t& definitions)
{
    std::string pattern;

    std::string expression;

    // The atoms of the expression at each depth of parentheses, by where each begins, and at each depth the atom a
    // class difference is waiting to subtract the next one from: re2c's `A \ B` over classes, which the pattern
    // parser has not got, is resolved to one bracket as soon as B is complete.
    struct Level
    {
        std::size_t opened;

        std::vector<std::size_t> atoms;

        std::optional<std::pair<std::size_t, std::size_t>> difference;
    };

    std::vector<Level> levels{{.opened = 0, .atoms = {}, .difference = std::nullopt}};

    const auto atom_done{[this, &expression, &levels, &definitions](const std::size_t begin) {
        auto& [opened, atoms, difference]{levels.back()};

        if (!difference)
        {
            atoms.push_back(begin);

            return;
        }

        const auto [left_begin, left_end]{*difference};

        const auto bytes_of{[this, &definitions](const std::string_view operand) {
            try
            {
                const auto bytes{class_of(regex::parse(operand, definitions))};

                if (!bytes)
                {
                    fail("the class difference's operand '" + std::string{operand} + "' is no class");
                }

                return *bytes;
            }
            catch (const regex::Syntax_error& refused)
            {
                fail("the class difference's operand '" + std::string{operand} + "' is refused: " + refused.what());
            }
        }};

        const auto left{bytes_of(std::string_view{expression}.substr(left_begin, left_end - left_begin))};

        const auto right{bytes_of(std::string_view{expression}.substr(left_end))};

        const auto remaining{left - right};

        if (remaining.symbols().empty())
        {
            fail("the class difference leaves no byte");
        }

        expression.erase(left_begin);

        expression += bracket(remaining);

        difference.reset();
    }};

    for (;;)
    {
        if (!peek() || at("*/"))
        {
            fail("expected an action before the end of the block");
        }

        const auto byte{*peek()};

        if (line_bound_ && byte == '\n')
        {
            break;
        }

        // What ends the regex, all at the top level: a bare '=', the ';' of a definition's body, or an action.
        // A '{' opens an action unless it is a count, {2,5}, or a flex-style reference, {name}, which re2c reads
        // with its flex-syntax flag and which the parser reads as it stands.
        const auto counted{byte == '{' && at_ + 1 < text_.size() && text_[at_ + 1] >= '0' && text_[at_ + 1] <= '9'};

        const auto referenced{byte == '{' && reference_length() > 0};

        if (byte == ';' || (byte == '{' && !counted && !referenced) || at("=>") || at(":=") || byte == '=')
        {
            break;
        }

        if (referenced)
        {
            const auto length{reference_length()};

            const auto begin{expression.size()};

            pattern += text_.substr(at_, length);

            expression += text_.substr(at_, length);

            at_ += length;

            atom_done(begin);

            continue;
        }

        if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r')
        {
            ++at_;

            pattern.push_back(' ');

            continue;
        }

        if (at("//") || at("/*"))
        {
            const auto line_end{std::min(text_.find('\n', at_), text_.size())};

            skip_blanks();

            // Bound to a line, the regex ends with the line, a comment closing on it notwithstanding.
            if (line_bound_ && at_ >= line_end)
            {
                break;
            }

            continue;
        }

        // Which quote is the case-insensitive one is the flags' to say; the exact double-quoted literal is the
        // parser's own, the others become bracket sequences.
        const auto insensitive{flags_.case_insensitive || (byte == '\'') != flags_.case_inverted};

        if ((byte == '"' && !insensitive) || byte == '[')
        {
            // Copied through with its escapes: the parser reads both forms as they stand.
            const auto close{byte == '"' ? '"' : ']'};

            const auto opened{at_};

            std::string copied{next("a quote")};

            // re2c closes a bracket at the first unescaped ']', a literal one being spelled '\]'.
            for (;;)
            {
                const auto inner{next(close == '"' ? R"('"' to close the quoted text)" : "']' to close the bracket")};

                copied.push_back(inner);

                if (inner == '\\')
                {
                    copied.push_back(next("the escaped byte"));

                    continue;
                }

                if (inner == close)
                {
                    break;
                }
            }

            // An escape's first byte is the backslash, so `\\u` is a backslash and a letter, not a code point.
            for (std::size_t index{0}; index + 1 < copied.size(); ++index)
            {
                if (copied[index] != '\\')
                {
                    continue;
                }

                if (copied[index + 1] == 'u' || copied[index + 1] == 'U' || copied[index + 1] == 'X')
                {
                    at_ = opened;

                    fail("a Unicode escape needs an encoding the byte reading has not got");
                }

                ++index;
            }

            if (copied == "[]")
            {
                at_ = opened;

                fail("an empty class matches nothing");
            }

            pattern += copied;

            const auto begin{expression.size()};

            // re2c's [^] is any byte; the pattern parser would read the ']' as a member, so it is spelled out.
            expression += copied == "[^]" ? std::string{R"([\x00-\xff])"} : copied;

            atom_done(begin);

            continue;
        }

        if (byte == '\'' || byte == '"')
        {
            const auto opened{at_};

            ++at_;

            const auto rewritten{literal(byte, insensitive)};

            pattern += text_.substr(opened, at_ - opened);

            const auto begin{expression.size()};

            expression += rewritten;

            atom_done(begin);

            continue;
        }

        if (is_name_byte(byte) && !(byte >= '0' && byte <= '9'))
        {
            std::string name;

            while (peek() && is_name_byte(*peek()))
            {
                name.push_back(next("a name"));
            }

            pattern += name;

            const auto begin{expression.size()};

            expression += flags_.flex_syntax ? name : '{' + name + '}';

            atom_done(begin);

            continue;
        }

        // A tag, `@name` or `#name`, marks a position and matches nothing; kept as written and dropped from the
        // expression.
        if ((byte == '@' || byte == '#') && at_ + 1 < end_ && is_name_byte(text_[at_ + 1]))
        {
            const auto begin{at_};

            for (++at_; peek() && is_name_byte(*peek()); ++at_)
            {
            }

            pattern += text_.substr(begin, at_ - begin);

            continue;
        }

        if (byte == '\\')
        {
            if (levels.back().atoms.empty())
            {
                fail("the class difference has no class before it");
            }

            levels.back().difference = {levels.back().atoms.back(), expression.size()};

            ++at_;

            pattern.push_back(byte);

            continue;
        }

        ++at_;

        pattern.push_back(byte);

        if (byte == '(')
        {
            levels.push_back({.opened = expression.size(), .atoms = {}, .difference = std::nullopt});
        }

        expression.push_back(byte);

        if (byte == ')' && levels.size() > 1)
        {
            const auto begin{levels.back().opened};

            levels.pop_back();

            atom_done(begin);
        }
        else if (byte == '.')
        {
            atom_done(expression.size() - 1);
        }
    }

    while (!pattern.empty() && pattern.back() == ' ')
    {
        pattern.pop_back();
    }

    while (!pattern.empty() && pattern.front() == ' ')
    {
        pattern.erase(0, 1);
    }

    return {std::move(pattern), std::move(expression)};
}

std::string Block::action()
{
    std::string code;

    // A transition names a condition first; it is kept as text, since which condition follows says nothing about
    // the token.
    if (at("=>") || at(":=>"))
    {
        while (peek() && *peek() != '{' && !at(":=") && *peek() != ';' && *peek() != '\n')
        {
            code.push_back(next("the transition"));
        }

        if (peek() == ';')
        {
            code.push_back(next("';'"));

            return code;
        }
    }

    if (at(":="))
    {
        while (peek() && *peek() != '\n')
        {
            code.push_back(next("the action"));
        }

        return code;
    }

    if (peek() != '{')
    {
        fail("expected an action, a '{' block or ':=' and the rest of the line");
    }

    const auto close{brace_close(text_.substr(at_))};

    if (!close)
    {
        fail("the action's braces never close");
    }

    code += text_.substr(at_, *close);

    at_ += *close;

    return code;
}

std::string Block::literal(const char quote, const bool insensitive)
{
    std::string expression;

    for (;;)
    {
        auto byte{next(std::string{"'"} + quote + "' to close the quoted text")};

        if (byte == quote)
        {
            break;
        }

        if (byte == '\\')
        {
            // An escape stands for one byte, which the parser decodes inside a bracket as well; only a hex or octal
            // one can spell a letter, and that one is decoded here so its bracket can hold both cases.
            const auto escaped{next("the escaped byte")};

            if (escaped == 'u' || escaped == 'U' || escaped == 'X')
            {
                fail("a Unicode escape needs an encoding the byte reading has not got");
            }

            std::string text{'\\', escaped};

            auto value{0};

            if (escaped == 'x')
            {
                while (text.size() < 4 && peek() && is_hex_digit(*peek()))
                {
                    const auto digit{next("a hex digit")};

                    text.push_back(digit);

                    value = value * 16 + (digit <= '9' ? digit - '0' : (digit | 0x20) - 'a' + 10);
                }
            }
            else if (escaped >= '0' && escaped <= '7')
            {
                value = escaped - '0';

                while (text.size() < 4 && peek() && *peek() >= '0' && *peek() <= '7')
                {
                    const auto digit{next("an octal digit")};

                    text.push_back(digit);

                    value = value * 8 + (digit - '0');
                }
            }

            if (insensitive && is_letter(static_cast<char>(value)))
            {
                byte = static_cast<char>(value);
            }
            else
            {
                expression += '[' + text + ']';

                continue;
            }
        }

        if (insensitive && is_letter(byte))
        {
            const auto lower{static_cast<char>(byte | 0x20)};

            const auto upper{static_cast<char>(byte & ~0x20)};

            expression += std::string{'['} + lower + upper + ']';

            continue;
        }

        expression += '[' + bracket_member(static_cast<unsigned char>(byte)) + ']';
    }

    if (expression.empty())
    {
        fail("an empty quoted literal matches nothing");
    }

    return expression;
}

std::size_t Block::reference_length() const noexcept
{
    if (peek() != '{')
    {
        return 0;
    }

    auto end{at_ + 1};

    while (end < text_.size() && is_name_byte(text_[end]))
    {
        ++end;
    }

    const auto named{end > at_ + 1 && !(text_[at_ + 1] >= '0' && text_[at_ + 1] <= '9')};

    return named && end < text_.size() && text_[end] == '}' ? end + 1 - at_ : 0;
}

} // namespace

std::vector<Lexer_spec> read_re2c(const std::string_view source, Re2c_flags flags, const Returning_t& returning)
{
    std::vector<Lexer_spec> scanners;

    // What one block leaves for the next: the definitions and the configurations, never the rules.
    Lexer_spec carried;

    // The blocks a later one may use by name; the unnamed rules block under the empty name.
    Library_t library;

    for (auto at{source.find("/*!")}; at != std::string_view::npos; at = source.find("/*!", at))
    {
        const auto rest{source.substr(at + 3)};

        // A local block reads the definitions and configurations so far and passes none of its own on; a rules
        // block is a library for the blocks that use it and no scanner itself; a use block opens by using one.
        const auto local{rest.starts_with("local:re2c")};

        const auto rules{rest.starts_with("rules:re2c")};

        const auto use{rest.starts_with("use:re2c")};

        const auto opener{
                rest.starts_with("re2c") ? std::size_t{4} :
                rules || local           ? std::size_t{10} :
                use                      ? std::size_t{8} :
                                           0};

        const auto line{1 + static_cast<std::size_t>(std::ranges::count(source.substr(0, at), '\n'))};

        if (opener == 0)
        {
            // Another block kind, which carries no rules; skipped through its close like any comment.
            const auto close{source.find("*/", at + 3)};

            if (close == std::string_view::npos)
            {
                throw Spec_error{"a re2c block is never closed", line};
            }

            at = close + 2;

            continue;
        }

        // The block's name, `rules:re2c:name`, when it has one.
        std::string name;

        auto begin{at + 3 + opener};

        if (source.substr(begin).starts_with(':'))
        {
            for (++begin; begin < source.size() && is_name_byte(source[begin]); ++begin)
            {
                name.push_back(source[begin]);
            }
        }

        Lexer_spec spec{carried};

        spec.line = line;

        if (use)
        {
            const auto found{library.find(name)};

            if (found == library.end())
            {
                throw Spec_error{"the used block '" + name + "' is not above this one", line};
            }

            merge(found->second, spec);
        }

        Block block{source, begin, flags};

        at = block.read(spec, library, returning);

        if (rules || !name.empty())
        {
            library.insert_or_assign(name, spec);
        }

        if (!local && !rules)
        {
            flags = block.flags();

            carried.definitions = spec.definitions;

            carried.options = spec.options;
        }

        if (spec.rules.empty() || rules)
        {
            continue;
        }

        // re2c declares no conditions; the ones the block's rules name are the scanner's, and each is exclusive,
        // since a rule is active in a condition only by naming it or by `<*>`.
        for (const auto& [pattern, expression, conditions, action, token, priority, rule_line] : spec.rules)
        {
            for (const auto& name : conditions)
            {
                const auto known{std::ranges::any_of(spec.conditions, [&name](const Lexer_spec::Condition& condition) {
                    return condition.name == name;
                })};

                if (name != "*" && name != "INITIAL" && !known)
                {
                    spec.conditions.push_back({.name = name, .exclusive = true});
                }
            }
        }

        scanners.push_back(std::move(spec));
    }

    return scanners;
}

} // namespace munch::tools::audit
