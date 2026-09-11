#include "munch/tools/audit/read_re2c.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace munch::tools::audit
{
namespace
{
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
class Block
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
     * @param returning The forms besides `return` an action returns a token through.
     * @return The offset just past the close.
     * @throws Spec_error If an item is malformed or left open, or the block never closes.
     */
    [[nodiscard]] std::size_t read(Lexer_spec& spec, const Returning_t& returning);

    /**
     * @brief The flags in force when the block closed, for the next block: a configuration or the evidence of the
     *        flex syntax may have changed them.
     * @return The flags.
     */
    [[nodiscard]] Re2c_flags flags() const noexcept;

private:
    /**
     * @brief Skips blanks and comments of either C style.
     */
    void skip_blanks();

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
     * exact double-quoted one become bracket sequences, blanks are dropped.
     * @return The pattern as written and its expression, both empty when an action follows at once.
     * @throws Spec_error If a quote or bracket is left open, or the regex uses a refused construct.
     */
    [[nodiscard]] std::pair<std::string, std::string> regex_text();

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
     * @brief Whether the text at the cursor begins with the given characters.
     * @param prefix The characters.
     * @return True when it does.
     */
    [[nodiscard]] bool at(std::string_view prefix) const noexcept;

    /**
     * @brief The byte at the cursor, or nothing at the end.
     * @return The byte.
     */
    [[nodiscard]] std::optional<char> peek() const noexcept;

    /**
     * @brief Consumes and returns the byte at the cursor.
     * @param what What the syntax expected, named when the block has ended.
     * @return The byte.
     * @throws Spec_error At the end of the block.
     */
    char next(std::string_view what);

    /**
     * @brief Refuses the block at the cursor's line.
     * @param message Why.
     */
    [[noreturn]] void fail(const std::string& message) const;

    /**
     * @brief The whole file.
     */
    std::string_view text_;

    /**
     * @brief The offset of the byte under the cursor.
     */
    std::size_t at_;

    /**
     * @brief The flags in force.
     */
    Re2c_flags flags_;

    /**
     * @brief Whether regex text ends at the line's end, which a flex-style definition's does.
     */
    bool line_bound_{false};
};

/**
 * @brief Whether a byte can begin or continue a bare name, which outside the flex syntax refers to a definition.
 * @param byte The byte.
 * @return True for a letter, a digit or an underscore.
 */
[[nodiscard]] constexpr bool is_name_byte(const char byte) noexcept
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '_';
}

/**
 * @brief Whether a byte is a letter with two cases, which a case-insensitive literal spells both of.
 * @param byte The byte.
 * @return True for a to z and A to Z.
 */
[[nodiscard]] constexpr bool is_letter(const char byte) noexcept
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

/**
 * @brief Whether a byte is a hexadecimal digit, which a `\x` escape is followed by up to two of.
 * @param byte The byte.
 * @return True for 0 to 9, a to f and A to F.
 */
[[nodiscard]] constexpr bool is_hex_digit(const char byte) noexcept
{
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

/**
 * @brief A byte as a bracket member, escaped where the bracket syntax would read it otherwise.
 * @param byte The byte.
 * @return The member text.
 */
[[nodiscard]] std::string bracket_member(const char byte)
{
    if (byte == ']' || byte == '\\' || byte == '^' || byte == '-' || byte == '[')
    {
        return std::string{'\\'} + byte;
    }

    return std::string(1, byte);
}

Block::Block(const std::string_view source, const std::size_t begin, const Re2c_flags flags)
    : text_{source}, at_{begin}, flags_{flags}
{}

std::size_t Block::read(Lexer_spec& spec, const Returning_t& returning)
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

        const auto line{1 + static_cast<std::size_t>(std::ranges::count(text_.substr(0, at_), '\n'))};

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

                auto [body, expression]{regex_text()};

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

        auto [pattern, expression]{regex_text()};

        // re2c's own definition: a name, `=`, its body and `;`. The name was read as regex text, one bare name.
        if (peek() == '=' && !at("=>"))
        {
            ++at_;

            auto [body, body_expression]{regex_text()};

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

        // The default rule, the end rule and setup rules, which have no regex at all, are not tokens.
        if (!named || pattern == "*" || pattern == "$")
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
                 .line = line});
    }

    return at_ + 2;
}

Re2c_flags Block::flags() const noexcept
{
    return flags_;
}

void Block::skip_blanks()
{
    for (;;)
    {
        while (peek() && (*peek() == ' ' || *peek() == '\t' || *peek() == '\n' || *peek() == '\r'))
        {
            ++at_;
        }

        if (at("//"))
        {
            while (peek() && *peek() != '\n')
            {
                ++at_;
            }
        }
        else if (at("/*"))
        {
            const auto close{text_.find("*/", at_ + 2)};

            if (close == std::string_view::npos)
            {
                fail("a comment is never closed");
            }

            at_ = close + 2;
        }
        else
        {
            return;
        }
    }
}

void Block::configuration(Lexer_spec& spec)
{
    const auto end{text_.find(';', at_)};

    if (end == std::string_view::npos)
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

std::pair<std::string, std::string> Block::regex_text()
{
    std::string pattern;

    std::string expression;

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

            pattern += text_.substr(at_, length);

            expression += text_.substr(at_, length);

            at_ += length;

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
                const auto inner{next(close == '"' ? "'\"' to close the quoted text" : "']' to close the bracket")};

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

            if (copied.find(R"(\u)") != std::string::npos || copied.find(R"(\U)") != std::string::npos ||
                copied.find(R"(\X)") != std::string::npos)
            {
                at_ = opened;

                fail("a Unicode escape needs an encoding the byte reading has not got");
            }

            if (copied == "[]")
            {
                at_ = opened;

                fail("an empty class matches nothing");
            }

            pattern += copied;

            // re2c's [^] is any byte; the pattern parser would read the ']' as a member, so it is spelled out.
            expression += copied == "[^]" ? std::string{R"([\x00-\xff])"} : copied;

            continue;
        }

        if (byte == '\'' || byte == '"')
        {
            const auto opened{at_};

            ++at_;

            const auto rewritten{literal(byte, insensitive)};

            pattern += text_.substr(opened, at_ - opened);

            expression += rewritten;

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

            expression += flags_.flex_syntax ? name : '{' + name + '}';

            continue;
        }

        if (byte == '\\')
        {
            fail(R"(the class difference '\' is not the pattern parser's)");
        }

        ++at_;

        pattern.push_back(byte);

        expression.push_back(byte);
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

        expression += '[' + bracket_member(byte) + ']';
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

bool Block::at(const std::string_view prefix) const noexcept
{
    return text_.substr(at_).starts_with(prefix);
}

std::optional<char> Block::peek() const noexcept
{
    return at_ < text_.size() ? std::optional{text_[at_]} : std::nullopt;
}

char Block::next(const std::string_view what)
{
    if (at_ >= text_.size())
    {
        fail("expected " + std::string{what} + " before the end of the block");
    }

    return text_[at_++];
}

void Block::fail(const std::string& message) const
{
    const auto line{
            1 + static_cast<std::size_t>(std::ranges::count(text_.substr(0, std::min(at_, text_.size())), '\n'))};

    throw Spec_error{message, line};
}

} // namespace

std::vector<Lexer_spec> read_re2c(const std::string_view source, Re2c_flags flags, const Returning_t& returning)
{
    std::vector<Lexer_spec> scanners;

    // What one block leaves for the next: the definitions and the configurations, never the rules.
    Lexer_spec carried;

    for (auto at{source.find("/*!")}; at != std::string_view::npos; at = source.find("/*!", at))
    {
        const auto rest{source.substr(at + 3)};

        const auto opener{
                rest.starts_with("re2c")       ? std::size_t{4} :
                rest.starts_with("rules:re2c") ? std::size_t{10} :
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

        Lexer_spec spec{carried};

        spec.line = line;

        Block block{source, at + 3 + opener, flags};

        at = block.read(spec, returning);

        flags = block.flags();

        carried.definitions = spec.definitions;

        carried.options = spec.options;

        if (spec.rules.empty())
        {
            continue;
        }

        // re2c declares no conditions; the ones the block's rules name are the scanner's, and each is exclusive,
        // since a rule is active in a condition only by naming it or by `<*>`.
        for (const auto& [pattern, expression, conditions, action, token, rule_line] : spec.rules)
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
