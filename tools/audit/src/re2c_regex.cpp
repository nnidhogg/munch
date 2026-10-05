#include "munch/tools/audit/re2c_regex.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "munch/regex/regex.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief Why a braced hexadecimal escape is refused.
 *
 * re2c's hexadecimal escape is `\xHH`, two digits and no braces, and re2c 3.1 answers `\x{...}` with "syntax error in
 * hexadecimal escape sequence" wherever it stands, in a class or in a literal, so a file holding one is no re2c file
 * and the code point the pattern parser would read there is nobody's.
 */
constexpr std::string_view braced_escape{
        "re2c's hexadecimal escape is a backslash, an x and two digits, and it takes no braces, so re2c answers this "
        "one with a syntax error in a hexadecimal escape sequence"};

/**
 * @brief The widest numeric escape of a literal: the backslash, an `x` or a first octal digit, and two digits more.
 */
constexpr std::size_t widest_escape{4};

/**
 * @brief What ends a line comment.
 */
constexpr std::string_view line_comment_closer{"\n"};

/**
 * @brief Why a byte beyond ASCII written into the source is refused under the UTF-8 encoding.
 */
constexpr std::string_view beyond_ascii_source{
        "a byte beyond ASCII in the source stands for the code points the --input-encoding option says, which no file "
        "carries, so under the utf8 encoding it is not read"};

/**
 * @brief Returns the code point a byte stands for under the byte encodings.
 * @param byte The byte.
 * @return The code point, 0 to 255.
 */
[[nodiscard]] constexpr char32_t point_of(const char byte) noexcept
{
    return static_cast<char32_t>(static_cast<unsigned char>(byte));
}

/**
 * @brief Returns whether text holds a byte beyond ASCII.
 * @param text The text.
 * @return True when it does.
 */
[[nodiscard]] bool spells_beyond_ascii(const std::string_view text)
{
    const auto beyond{[](const char one) { return point_of(one) > last_ascii; }};

    return std::ranges::any_of(text, beyond);
}

/**
 * @brief Returns the bytes a quoted literal spells, as the parser reads them; the empty literal spells none, matching
 *        nothing.
 * @param copied The literal as written, its quotes included.
 * @param definitions The definitions in force, which the parser is given.
 * @return The bytes, or std::nullopt when the parser reads the literal as something else or refuses it.
 */
[[nodiscard]] std::optional<std::string> literal_bytes(
        const std::string& copied, const regex::Definitions_t& definitions)
{
    if (copied == R"("")")
    {
        return std::string{};
    }

    const auto text_of{[]<typename Node>(const Node& node) -> std::optional<std::string> {
        if constexpr (std::is_same_v<Node, regex::Text>)
        {
            return node.text;
        }
        else
        {
            return std::nullopt;
        }
    }};

    try
    {
        const auto parsed{regex::parse(copied, definitions, re2c_parse)};

        return std::visit(text_of, parsed.node);
    }
    catch (const regex::Syntax_error&)
    {
        return std::nullopt;
    }
}

/**
 * @brief Returns the class of one code point.
 * @param point The code point.
 * @return The class holding it alone.
 */
[[nodiscard]] Class one_point(const char32_t point)
{
    return {{.first = point, .last = point}};
}

/**
 * @brief Returns the byte a named escape stands for, as the pattern parser decodes it: the controls `\n`, `\t`, `\r`,
 *        `\f`, `\v`, `\a` and `\b`, and any other escaped byte itself.
 * @param escaped The byte after the backslash, not a hex or octal digit.
 * @return The value.
 */
[[nodiscard]] constexpr char32_t decoded(const char escaped) noexcept
{
    switch (escaped)
    {
    case 'n':
        return U'\n';
    case 't':
        return U'\t';
    case 'r':
        return U'\r';
    case 'f':
        return U'\f';
    case 'v':
        return U'\v';
    case 'a':
        return U'\a';
    case 'b':
        return U'\b';
    default:
        return static_cast<unsigned char>(escaped);
    }
}

} // namespace

std::size_t reference_length(const std::string_view text, const std::size_t at) noexcept
{
    if (at >= text.size() || text[at] != '{')
    {
        return 0;
    }

    auto end{at + 1};

    while (end < text.size() && is_name_byte(text[end]))
    {
        ++end;
    }

    const auto named{end > at + 1 && !is_digit(text[at + 1])};

    return named && end < text.size() && text[end] == '}' ? end + 1 - at : 0;
}

std::string_view reference_name(const std::string_view text, const std::size_t at) noexcept
{
    const auto length{reference_length(text, at)};

    return length == 0 ? std::string_view{} : text.substr(at + 1, length - 2);
}

Regex_reader::Regex_reader(
        const std::string_view text, const std::size_t begin, const Re2c_flags flags, const bool line_bound,
        const Classes_t& classes, std::optional<Spec_error>& deferred)
    : Cursor{text, begin, text.size()}
    , flags_{flags}
    , line_bound_{line_bound}
    , classes_{classes}
    , deferred_{deferred}
    , levels_{Level{}}
{}

Regex_text Regex_reader::regex_text(const regex::Definitions_t& definitions)
{
    for (;;)
    {
        if (!peek() || at(comment_closer))
        {
            fail("expected an action before the end of the block");
        }

        if (at_regex_end())
        {
            break;
        }

        if (at(line_comment_opener) || at(comment_opener))
        {
            if (comment_ends_regex())
            {
                break;
            }

            continue;
        }

        if (take_reference() || take_blank() || take_bracket(definitions) || take_exact_literal(definitions) ||
            take_literal() || take_name() || take_dot() || take_tag() || take_difference())
        {
            continue;
        }

        take_operator();
    }

    close_term();

    const auto last{pattern_.find_last_not_of(' ')};

    pattern_.erase(last == std::string::npos ? 0 : last + 1);

    const auto first{pattern_.find_first_not_of(' ')};

    pattern_.erase(0, std::min(first, pattern_.size()));

    // The regex is a class where its alternatives, at the top level, are each one class atom, as re2c merges them.
    const auto whole{levels_.size() == 1 && levels_.front().classes};

    std::optional<Class> points{};

    if (whole)
    {
        points = std::move(levels_.front().branches);
    }

    return {.pattern = std::move(pattern_), .expression = std::move(expression_), .points = std::move(points)};
}

bool Regex_reader::at_regex_end() const noexcept
{
    const auto byte{*peek()};

    if (line_bound_ && byte == '\n')
    {
        return true;
    }

    // What ends the regex, all at the top level: a bare '=', the ';' of a definition's body, or an action. A '{' opens
    // an action unless it is a count, {2,5}, or a flex-style reference, {name}, which re2c reads with its flex-syntax
    // flag and which the parser reads as it stands.
    const auto counted{byte == '{' && at_ + 1 < text_.size() && is_digit(text_[at_ + 1])};

    const auto referenced{byte == '{' && reference_length(text_, at_) > 0};

    return byte == ';' || (byte == '{' && !counted && !referenced) || at(transition_opener) || at(line_action_opener) ||
           byte == '=';
}

bool Regex_reader::comment_ends_regex()
{
    const auto newline{text_.find('\n', at_)};

    const auto line_end{std::min(newline, text_.size())};

    skip_blanks();

    // Bound to a line, the regex ends with the line, a comment closing on it notwithstanding.
    return line_bound_ && at_ >= line_end;
}

bool Regex_reader::take_reference()
{
    const auto length{reference_length(text_, at_)};

    if (length == 0)
    {
        return false;
    }

    const auto reference{text_.substr(at_, length)};

    const auto name{reference_name(text_, at_)};

    pattern_ += reference;

    expression_ += reference;

    auto points{defined(name)};

    place(std::move(points));

    at_ += length;

    return true;
}

void Regex_reader::place(std::optional<Class> points)
{
    auto& level{levels_.back()};

    level.single = level.atoms == 0 ? std::move(points) : std::nullopt;

    ++level.atoms;
}

std::optional<Class> Regex_reader::defined(const std::string_view name) const
{
    const auto found{classes_.find(name)};

    if (found == classes_.end())
    {
        return std::nullopt;
    }

    const auto& [defined_name, points]{*found};

    return points;
}

bool Regex_reader::take_blank()
{
    if (!is_blank(*peek()))
    {
        return false;
    }

    ++at_;

    pattern_.push_back(' ');

    return true;
}

bool Regex_reader::take_bracket(const regex::Definitions_t& definitions)
{
    if (peek() != '[')
    {
        return false;
    }

    const auto opened{at_};

    const auto copied{copied_text(']')};

    pattern_ += copied;

    // Under UTF-8 the expression spells the bracket's code points, which the byte parser's reading of it gives.
    const auto space{code_space(flags_.encoding)};

    auto points{code_points(copied, definitions, space)};

    if (in_utf8() && !points)
    {
        fail_at(opened, std::format("the class '{}' is no class whose code points the reading can take", copied));
    }

    if (in_utf8())
    {
        expression_ += is_ascii(*points) ? copied : step(*points);
    }
    else
    {
        expression_ += copied == "[^]" ? std::string{any_byte} : copied;
    }

    place(std::move(points));

    return true;
}

void Regex_reader::fail_at(const std::size_t at, const std::string& why)
{
    at_ = at;

    fail(why);
}

std::string Regex_reader::copied_text(const char close)
{
    const auto opened{at_};

    std::string copied{next("a quote")};

    const std::string_view closing{close == '"' ? R"('"' to close the quoted text)" : "']' to close the bracket"};

    // re2c closes a bracket at the first unescaped ']', a literal one being spelled '\]'.
    for (;;)
    {
        const auto inner{next(closing)};

        copied.push_back(inner);

        if (inner == '\\')
        {
            const auto escaped{next("the escaped byte")};

            copied.push_back(escaped);

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

        const auto escaped{copied[index + 1]};

        const auto braced{index + 2 < copied.size() && copied[index + 2] == '{'};

        refuse_unread_escape(escaped, braced, opened);

        ++index;
    }

    if (copied == "[]")
    {
        fail_at(opened, "an empty class matches nothing");
    }

    if (in_utf8() && spells_beyond_ascii(copied))
    {
        fail_at(opened, std::string{beyond_ascii_source});
    }

    return copied;
}

void Regex_reader::refuse_unread_escape(const char escaped, const bool braced, const std::size_t at)
{
    if (escaped == 'u' || escaped == 'U' || escaped == 'X')
    {
        fail_at(at, "a Unicode escape needs an encoding the byte reading has not got");
    }

    if (escaped == 'x' && braced)
    {
        fail_at(at, std::string{braced_escape});
    }
}

bool Regex_reader::in_utf8() const noexcept
{
    return flags_.encoding == Re2c_encoding::utf8;
}

bool Regex_reader::take_exact_literal(const regex::Definitions_t& definitions)
{
    if (peek() != '"' || is_insensitive('"'))
    {
        return false;
    }

    const auto opened{at_};

    const auto copied{copied_text('"')};

    pattern_ += copied;

    const auto bytes{literal_bytes(copied, definitions)};

    if (in_utf8() && !bytes)
    {
        fail_at(opened, std::format("the literal {} is no text whose code points the reading can take", copied));
    }

    // Under UTF-8 every character of the literal is a code point of its own, which the encoding spells in one byte or
    // two; where all are ASCII, and under the byte encodings, the literal stands as written.
    if (in_utf8() && spells_beyond_ascii(*bytes))
    {
        for (const auto one : *bytes)
        {
            const auto points{one_point(point_of(one))};

            expression_ += step(points);
        }
    }
    else
    {
        expression_ += copied;
    }

    // A literal of one character is a char set to re2c.
    if (bytes && bytes->size() == 1)
    {
        place(one_point(point_of(bytes->front())));
    }
    else
    {
        place(std::nullopt);
    }

    return true;
}

bool Regex_reader::is_insensitive(const char quote) const noexcept
{
    return flags_.case_insensitive || (quote == '\'') != flags_.case_inverted;
}

bool Regex_reader::take_literal()
{
    const auto quote{*peek()};

    if (quote != '\'' && (quote != '"' || !is_insensitive(quote)))
    {
        return false;
    }

    const auto opened{at_};

    ++at_;

    auto [rewritten, points]{literal(quote, is_insensitive(quote))};

    const auto quoted{text_.substr(opened, at_ - opened)};

    if (in_utf8() && spells_beyond_ascii(quoted))
    {
        fail_at(opened, std::string{beyond_ascii_source});
    }

    pattern_ += quoted;

    expression_ += rewritten;

    place(std::move(points));

    return true;
}

Regex_reader::Rewritten_literal Regex_reader::literal(const char quote, const bool insensitive)
{
    std::string expression{};

    // The characters read, and the class of the first, which is the literal's where it is the only one.
    std::size_t characters{0};

    Class first{};

    // Counts a character read and keeps the class of the first.
    const auto character{[&characters, &first](Class points) {
        if (++characters == 1)
        {
            first = std::move(points);
        }
    }};

    const auto closing{std::format("'{}' to close the quoted text", quote)};

    for (;;)
    {
        auto byte{next(closing)};

        if (byte == quote)
        {
            break;
        }

        if (byte == '\\')
        {
            auto [letter, written, points]{literal_escape(insensitive)};

            if (!letter)
            {
                expression += written;

                character(std::move(points));

                continue;
            }

            byte = *letter;
        }

        if (insensitive && is_letter(byte))
        {
            const auto lower{static_cast<char>(byte | case_bit)};

            const auto upper{static_cast<char>(byte & ~case_bit)};

            expression += std::format("[{}{}]", lower, upper);

            Class both_cases{
                    {.first = static_cast<char32_t>(upper), .last = static_cast<char32_t>(upper)},
                    {.first = static_cast<char32_t>(lower), .last = static_cast<char32_t>(lower)}};

            character(std::move(both_cases));

            continue;
        }

        const auto value{static_cast<unsigned char>(byte)};

        const auto member{bracket_member(value)};

        expression += std::format("[{}]", member);

        character(one_point(value));
    }

    if (expression.empty())
    {
        fail("an empty quoted literal matches nothing");
    }

    std::optional<Class> points{};

    if (characters == 1)
    {
        points = std::move(first);
    }

    return {.expression = std::move(expression), .points = std::move(points)};
}

Regex_reader::Literal_escape Regex_reader::literal_escape(const bool insensitive)
{
    const auto escaped{next("the escaped byte")};

    refuse_unread_escape(escaped, peek() == '{', at_);

    std::string text{'\\', escaped};

    const auto numeric{escaped == 'x' || is_octal_digit(escaped)};

    const auto value{numeric_value(escaped, text)};

    const auto stands{numeric ? static_cast<char32_t>(value) : decoded(escaped)};

    if (insensitive && stands <= last_ascii && is_letter(static_cast<char>(stands)))
    {
        return {.letter = static_cast<char>(stands), .written = {}, .points = {}};
    }

    // An escape naming a code point beyond ASCII under UTF-8 is spelled in two bytes rather than one.
    if (value > static_cast<int>(last_ascii) && in_utf8())
    {
        auto points{one_point(stands)};

        auto written{step(points)};

        return {.letter = std::nullopt, .written = std::move(written), .points = std::move(points)};
    }

    auto written{std::format("[{}]", text)};

    return {.letter = std::nullopt, .written = std::move(written), .points = one_point(stands)};
}

int Regex_reader::numeric_value(const char escaped, std::string& text)
{
    auto value{0};

    if (escaped == 'x')
    {
        while (text.size() < widest_escape && peek() && is_hex_digit(*peek()))
        {
            const auto digit{next("a hex digit")};

            text.push_back(digit);

            value = value * 16 + static_cast<int>(hex_value(digit));
        }

        return value;
    }

    if (!is_octal_digit(escaped))
    {
        return value;
    }

    value = escaped - '0';

    while (text.size() < widest_escape && peek() && is_octal_digit(*peek()))
    {
        const auto digit{next("an octal digit")};

        text.push_back(digit);

        value = value * 8 + (digit - '0');
    }

    return value;
}

bool Regex_reader::take_name()
{
    const auto byte{*peek()};

    if (!is_name_start(byte))
    {
        return false;
    }

    std::string name{};

    while (peek() && is_name_byte(*peek()))
    {
        name.push_back(next("a name"));
    }

    pattern_ += name;

    // Under the flex syntax a bare name is the literal it spells, a char set to re2c where it is one letter.
    if (flags_.flex_syntax)
    {
        expression_ += name;

        std::optional<Class> points{};

        if (name.size() == 1)
        {
            points = one_point(point_of(name.front()));
        }

        place(std::move(points));
    }
    else
    {
        expression_ += reference(name);

        place(defined(name));
    }

    return true;
}

bool Regex_reader::take_dot()
{
    if (peek() != '.')
    {
        return false;
    }

    ++at_;

    pattern_.push_back('.');

    // re2c's dot is any code point but the newline, which under UTF-8 is every encoding but that byte's.
    Class points{{.first = 0, .last = U'\n' - 1}, {.first = U'\n' + 1, .last = code_space(flags_.encoding) - 1}};

    expression_ += in_utf8() ? step(points) : std::string{'.'};

    place(std::move(points));

    return true;
}

bool Regex_reader::take_tag()
{
    const auto byte{*peek()};

    if ((byte != '@' && byte != '#') || at_ + 1 >= end_ || !is_name_byte(text_[at_ + 1]))
    {
        return false;
    }

    // A tag, `@name` or `#name`, marks a position and matches nothing; kept as written and dropped from the expression,
    // and no char set to re2c.
    const auto begin{at_};

    ++at_;

    while (peek() && is_name_byte(*peek()))
    {
        ++at_;
    }

    pattern_ += text_.substr(begin, at_ - begin);

    place(std::nullopt);

    return true;
}

bool Regex_reader::take_difference()
{
    if (peek() != '\\')
    {
        return false;
    }

    if (levels_.back().atoms == 0)
    {
        fail("the class difference has no class before it");
    }

    resolve();

    auto& level{levels_.back()};

    if (!level.single)
    {
        fail(R"(re2c can only difference char sets, and what stands before the '\' is no class)");
    }

    level.left = std::move(level.single);

    level.single.reset();

    level.atoms = 0;

    ++at_;

    pattern_.push_back('\\');

    return true;
}

void Regex_reader::resolve()
{
    auto& level{levels_.back()};

    if (!level.left)
    {
        return;
    }

    if (!level.single)
    {
        fail(R"(re2c can only difference char sets, and what follows the '\' is no class)");
    }

    auto remaining{subtracted(*level.left, *level.single)};

    // Which code points the operands hold is the flags' to say, so a difference empty under this pass's flags is
    // refused only once the flags have settled; the pass goes on with the empty class.
    if (remaining.empty() && !deferred_)
    {
        deferred_ = Spec_error{"the class difference leaves an empty class, which matches nothing", line()};
    }

    expression_.erase(level.term);

    expression_ += rendered(remaining, flags_.encoding);

    level.single = std::move(remaining);

    level.left.reset();
}

void Regex_reader::take_operator()
{
    const auto byte{*peek()};

    ++at_;

    pattern_.push_back(byte);

    if (byte == '(')
    {
        open_group();

        return;
    }

    if (byte == ')' && levels_.size() > 1)
    {
        close_group();

        return;
    }

    // An alternative ends the term, and so does a trailing context, which makes the regex no class.
    if (byte == '|' || byte == '/')
    {
        alternative(byte);

        return;
    }

    // Anything else, a repetition, a count or a byte the parser will refuse, makes the term more than one class.
    expression_.push_back(byte);

    levels_.back().single.reset();
}

void Regex_reader::open_group()
{
    expression_.push_back('(');

    // `(!R)` is R in a group that captures nothing, re2c's spelling; the mark is no member of the group.
    if (const auto mark{group_mark()}; mark < end_ && text_[mark] == '!')
    {
        pattern_ += text_.substr(at_, mark + 1 - at_);

        at_ = mark + 1;
    }

    levels_.push_back({.term = expression_.size()});
}

std::size_t Regex_reader::group_mark() const noexcept
{
    const std::string_view blanks{line_bound_ ? " \t" : " \t\r\n"};

    auto found{at_};

    for (;;)
    {
        const auto first_other{text_.find_first_not_of(blanks, found)};

        found = std::min(first_other, end_);

        const auto rest{text_.substr(found, end_ - found)};

        const auto line_comment{rest.starts_with(line_comment_opener)};

        if (line_bound_ || !(rest.starts_with(comment_opener) || line_comment))
        {
            return found;
        }

        const auto closer{line_comment ? line_comment_closer : comment_closer};

        const auto from{line_comment ? found : found + comment_opener.size()};

        const auto close{text_.find(closer, from)};

        if (close == std::string_view::npos || close + closer.size() > end_)
        {
            return found;
        }

        found = close + closer.size();
    }
}

void Regex_reader::close_group()
{
    close_term();

    auto group{std::move(levels_.back())};

    levels_.pop_back();

    expression_.push_back(')');

    std::optional<Class> points{};

    if (group.classes)
    {
        points = std::move(group.branches);
    }

    place(std::move(points));
}

void Regex_reader::close_term()
{
    resolve();

    auto& level{levels_.back()};

    if (level.single)
    {
        level.branches = united(level.branches, *level.single);
    }
    else
    {
        level.classes = false;
    }
}

void Regex_reader::alternative(const char byte)
{
    close_term();

    auto& level{levels_.back()};

    level.classes = level.classes && byte == '|';

    expression_.push_back(byte);

    open_term();
}

void Regex_reader::open_term()
{
    auto& level{levels_.back()};

    level.term = expression_.size();

    level.atoms = 0;

    level.single.reset();
}

} // namespace munch::tools::audit
