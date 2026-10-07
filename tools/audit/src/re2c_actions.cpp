#include "munch/tools/audit/re2c_actions.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/c_tokens.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/re2c_regex.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief A word of a spelling as the reading compares it, with the token it came from.
 */
struct Word
{
    /**
     * @brief The word: the token's text, an integer literal's value or one word of a macro's value in its place.
     */
    std::string text{};

    /**
     * @brief The index of the token it came from in the spelling read, a macro's words all coming from its name.
     */
    std::size_t origin{};
};

/**
 * @brief Every way of defining the transparent macros at an action, one value each, by name.
 */
using Choice_t = std::map<std::string, std::vector<std::string>, std::less<>>;

/**
 * @brief The macros an action and the scan pointers' spellings name, each with the values it stands for.
 */
using Values_t = std::map<std::string, std::vector<std::vector<std::string>>, std::less<>>;

/**
 * @brief The most ways of defining the transparent macros at an action the reading follows together.
 */
constexpr std::size_t most_choices{64};

/**
 * @brief Returns a text without the bytes of a set that open it.
 * @param text The text.
 * @param dropped The bytes dropped.
 * @return The text from its first byte outside the set, empty when every byte is in it.
 */
[[nodiscard]] std::string_view without_leading(const std::string_view text, const std::string_view dropped) noexcept
{
    const auto first{text.find_first_not_of(dropped)};

    return text.substr(std::min(first, text.size()));
}

/**
 * @brief Returns a word of a spelling, or nothing past its end.
 * @param words The spelling.
 * @param index The word's index.
 * @return The word's text, empty past the end.
 */
[[nodiscard]] std::string_view text_at(const std::vector<Word>& words, const std::size_t index) noexcept
{
    return index < words.size() ? std::string_view{words[index].text} : std::string_view{};
}

/**
 * @brief Returns whether a word ends a value: a name, a literal, or a closing parenthesis or bracket.
 * @param words The spelling.
 * @param index The word's index.
 * @return True when it does.
 */
[[nodiscard]] bool value_before(const std::vector<Word>& words, const std::size_t index) noexcept
{
    const auto word{text_at(words, index)};

    return !word.empty() && (is_name_byte(word.front()) || word == ")" || word == "]");
}

/**
 * @brief Returns whether a word is a name declared as a reference or a pointer, `auto& cursor`, `char* cursor`.
 * @param words The action's words.
 * @param name The index of the declared name.
 * @return True when it is.
 */
[[nodiscard]] bool declares(const std::vector<Word>& words, const std::size_t name) noexcept
{
    return value_before(words, name) && name >= 1 &&
           (text_at(words, name - 1) == "&" || text_at(words, name - 1) == "*");
}

/**
 * @brief Returns whether the parenthesis opening at a word is a cast's, `(int)(...)` or `(const char*)(...)`: the group
 *        before it opens with a type's word or holds a `*` or `&` after a name, and a call through a pointer or an
 *        expression otherwise, `(*f)(...)`; `(int)` came down to `int` with its grouping parentheses, so a type's word
 *        before the `(` is a cast as well.
 * @param words The action's words.
 * @param back One past the parenthesis's index, at least 2.
 * @return True for a cast.
 */
[[nodiscard]] bool is_cast(const std::vector<Word>& words, const std::size_t back)
{
    const auto opener{text_at(words, back - 2)};

    static constexpr std::array<std::string_view, 15> types{"int",    "char",   "long",      "short",  "unsigned",
                                                            "signed", "void",   "float",     "double", "bool",
                                                            "const",  "size_t", "ptrdiff_t", "auto",   "volatile"};

    if (opener != ")")
    {
        return std::ranges::contains(types, opener);
    }

    auto open{back - 2};

    for (auto inner{0}; open > 0; --open)
    {
        inner += depth_step(text_at(words, open), ")", "(");

        if (inner == 0)
        {
            break;
        }
    }

    const auto head{text_at(words, open + 1)};

    const auto indirect{
            [&words](const std::size_t piece) { return text_at(words, piece) == "*" || text_at(words, piece) == "&"; }};

    const auto from{open + 2};

    const auto group_words{std::views::iota(from, std::max(from, back - 2))};

    const auto stars{std::ranges::any_of(group_words, indirect)};

    return std::ranges::contains(types, head) || (stars && starts_name(head));
}

/**
 * @brief Returns whether a reference or a pointer is bound to a name, `auto& cursor = cursors[0]` or `auto&
 *        cursor{cursors[0]}`.
 * @param words The action's words.
 * @param first The index of the name's first word.
 * @param last The index of its last word.
 * @return True when one is.
 */
[[nodiscard]] bool is_bound(const std::vector<Word>& words, const std::size_t first, const std::size_t last) noexcept
{
    if (first < 3)
    {
        return false;
    }

    const auto assigned{text_at(words, first - 1) == "=" && text_at(words, first - 2) != "="};

    const auto braced{text_at(words, first - 1) == "{" && text_at(words, last + 1) == "}"};

    return (assigned || braced) && declares(words, first - 2);
}

/**
 * @brief Returns whether a name stands among the arguments of a call, `advance(cursors[0])`, which may take it by
 *        reference: the scan runs back from the name over the groups closed before it to the first parenthesis left
 *        open, which is a call's when a value stands before it that is no keyword and no cast; a grouping's or a cast's
 *        parenthesis is stepped out of, since `f((cursors[0]))` passes the pointer as `f(cursors[0])` does, and the
 *        statement's start ends the scan.
 * @param words The action's words.
 * @param first The index of the name's first word.
 * @return True when it does.
 */
[[nodiscard]] bool is_passed(const std::vector<Word>& words, const std::size_t first)
{
    for (auto back{first}, groups{0UZ}; back > 0; --back)
    {
        const auto word{text_at(words, back - 1)};

        if (word == ")" || word == "]")
        {
            ++groups;

            continue;
        }

        if ((word == "(" || word == "[") && groups > 0)
        {
            --groups;

            continue;
        }

        if (word == ";" || word == "{" || word == "}")
        {
            return false;
        }

        if (word != "(" || back < 2)
        {
            continue;
        }

        static constexpr std::array<std::string_view, 6> keywords{"if", "while", "for", "switch", "return", "sizeof"};

        if (value_before(words, back - 2) && !is_cast(words, back) &&
            !std::ranges::contains(keywords, text_at(words, back - 2)))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Returns whether the run of words from one index is a spelling, word for word, in the reading's own spelling of
 *        both.
 * @param words The action's words.
 * @param here The index the run begins at.
 * @param spelling The spelling.
 * @return True when it is.
 */
[[nodiscard]] bool spells_at(
        const std::vector<Word>& words, const std::size_t here, const std::vector<std::string>& spelling) noexcept
{
    if (here + spelling.size() > words.size())
    {
        return false;
    }

    const auto run{std::span{words}.subspan(here, spelling.size())};

    return std::ranges::equal(run, spelling, {}, &Word::text);
}

/**
 * @brief Returns whether one pair of parentheses wraps a name and nothing else.
 * @param words The action's words.
 * @param first The index of the name's first word.
 * @param last The index of its last word.
 * @return True when a `(` stands right before it and a `)` right after.
 */
[[nodiscard]] bool is_wrapped(const std::vector<Word>& words, const std::size_t first, const std::size_t last) noexcept
{
    return first > 0 && text_at(words, first - 1) == "(" && text_at(words, last + 1) == ")";
}

/**
 * @brief Returns whether a name is handed on rather than moved where it stands, which leaves what becomes of it out of
 *        sight: its address taken, `&cursors[0]`, a reference bound to it, `auto& cursor = cursors[0]`, or the name
 *        passed to a call, `advance(cursors[0])`, which may take it by reference; a cast's or a grouping's parentheses
 *        pass nothing. It is handed on as much at every level of parentheses around it, `(cursors[0])` bound to a
 *        reference or passed as `cursors[0]` is, so the checks run on the name and again on each pair wrapping it.
 * @param words The action's words.
 * @param first The index of the name's first word.
 * @param last The index of its last word.
 * @return True when it is handed on.
 */
[[nodiscard]] bool is_handed_on(const std::vector<Word>& words, std::size_t first, std::size_t last)
{
    // The `&` takes the address where no value stands before it, which would make it a conjunction.
    const auto is_addressed{[&words](const std::size_t at) {
        return at >= 1 && text_at(words, at - 1) == "&" && !(at >= 2 && value_before(words, at - 2));
    }};

    for (;;)
    {
        if (is_addressed(first) || is_bound(words, first, last) || is_passed(words, first))
        {
            return true;
        }

        if (!is_wrapped(words, first, last))
        {
            return false;
        }

        --first;

        ++last;
    }
}

/**
 * @brief Returns words written together, as a name or an array's run is.
 * @param words The words.
 * @return Their concatenation.
 */
[[nodiscard]] std::string concatenated(const std::vector<std::string>& words)
{
    return std::ranges::fold_left(words, std::string{}, std::plus{});
}

/**
 * @brief Reduces a spelling until nothing changes: parentheses around one name or one value are grouping and name
 *        nothing, `(in)->cur` being `in->cur`, `(cursors)[0]` being `cursors[0]` and `cursors[(0)]` being `cursors[0]`;
 *        a dereference and a dot are an arrow, `(*in).cur` being `in->cur`; every other parenthesis is a call's or an
 *        index's and stays.
 * @param out The spelling, reduced in place.
 */
void reduce(std::vector<Word>& out)
{
    const auto word{[&out](const std::size_t index) { return text_at(out, index); }};

    const auto valued{[](const std::string_view text) { return !text.empty() && is_digit(text.front()); }};

    auto changed{false};

    do
    {
        changed = false;

        std::vector<Word> next{};

        for (std::size_t at{0}; at < out.size(); ++at)
        {
            if (word(at) == "(" && word(at + 1) == "*" && starts_name(word(at + 2)) && word(at + 3) == ")" &&
                word(at + 4) == ".")
            {
                next.push_back(out[at + 2]);

                next.push_back({.text = "->", .origin = out[at + 4].origin});

                at += 4;

                changed = true;

                continue;
            }

            // A value in parentheses is the value, `cursors[(0)]` being `cursors[0]`.
            if (word(at) == "(" && (starts_name(word(at + 1)) || valued(word(at + 1))) && word(at + 2) == ")")
            {
                next.push_back(out[at + 1]);

                at += 2;

                changed = true;

                continue;
            }

            next.push_back(out[at]);
        }

        out = std::move(next);
    } while (changed);
}

/**
 * @brief Returns an integer literal read to its value, its digit separators, base prefix and suffix taken off, so that
 *        no spelling of an index is another index: `0U`, `0x0`, `00` and `0'0` are the one index 0.
 * @param word The word.
 * @return The value in decimal, or the word itself when it is no integer literal.
 */
[[nodiscard]] std::string canonical(const std::string& word)
{
    if (word.empty() || !is_digit(word.front()))
    {
        return word;
    }

    std::string digits{word};

    std::erase(digits, '\'');

    unsigned base{10U};

    std::size_t at{0};

    static constexpr std::string_view hex_prefix{"0x"};

    static constexpr std::string_view capital_hex_prefix{"0X"};

    static constexpr std::string_view binary_prefix{"0b"};

    static constexpr std::string_view capital_binary_prefix{"0B"};

    static constexpr std::string_view octal_prefix{"0"};

    if (digits.starts_with(hex_prefix) || digits.starts_with(capital_hex_prefix))
    {
        base = 16U;

        at = hex_prefix.size();
    }
    else if (digits.starts_with(binary_prefix) || digits.starts_with(capital_binary_prefix))
    {
        base = 2U;

        at = binary_prefix.size();
    }
    else if (digits.size() > octal_prefix.size() && digits.starts_with(octal_prefix))
    {
        base = 8U;

        at = octal_prefix.size();
    }

    // A byte that is no digit counts as the base itself, which no base takes.
    const auto digit_of{[base](const char written) {
        const auto byte{static_cast<unsigned char>(written)};

        if (is_digit(written))
        {
            return static_cast<unsigned>(byte - '0');
        }

        if (is_letter(byte))
        {
            return static_cast<unsigned>((byte | case_bit) - 'a' + 10);
        }

        return base;
    }};

    unsigned long long value{0};

    for (; at < digits.size(); ++at)
    {
        const auto digit{digit_of(digits[at])};

        if (digit >= base || value > (std::numeric_limits<unsigned long long>::max() - digit) / base)
        {
            break;
        }

        value = value * base + digit;
    }

    const auto suffix_letter{[](const char byte) { return std::string_view{"uUlLzZ"}.contains(byte); }};

    const auto suffix{std::string_view{digits}.substr(at)};

    if (!std::ranges::all_of(suffix, suffix_letter))
    {
        return word;
    }

    return std::to_string(value);
}

/**
 * @brief Returns an arithmetic operator applied to its two operands.
 * @param symbol The operator, `*`, `/`, `%`, `+` or `-`.
 * @param left The left operand.
 * @param right The right operand, not zero under `/` and `%`.
 * @return The value.
 */
[[nodiscard]] long long applied(const std::string_view symbol, const long long left, const long long right) noexcept
{
    if (symbol == "*")
    {
        return left * right;
    }

    if (symbol == "/")
    {
        return left / right;
    }

    if (symbol == "%")
    {
        return left % right;
    }

    if (symbol == "+")
    {
        return left + right;
    }

    return left - right;
}

/**
 * @brief The value of a constant integer expression over the tokens given: decimal literals within an int's range, `+`,
 *        `-`, `*`, `/`, `%`, unary signs and parentheses, with C's precedence, every value kept within an int's range;
 *        or nothing where a token is anything else, a shift, a bitwise operator or a suffixed or hexadecimal literal
 *        among them, whose type and width the reading does not model, `~0U >> 31` being 1 and `(1U << 31) << 1` being 0
 *        under the compiler where a signed reading says otherwise.
 *
 * The expression is read by precedence climbing: a level reads the operands of its operators at the level below, and
 * the lowest level, a unary sign, a parenthesised expression or a literal, reads a whole expression again inside
 * parentheses.
 */
class Constant_reader
{
public:
    /**
     * @brief Binds the reader to the expression's tokens.
     * @param words The tokens.
     */
    explicit Constant_reader(std::span<const std::string> words) noexcept;

    /**
     * @brief Returns the expression's value, all its tokens read.
     * @return The value, or std::nullopt when a token is outside what the reading evaluates, a value leaves an int's
     *         range, a division is by zero or tokens are left over.
     */
    [[nodiscard]] std::optional<long long> value();

private:
    /**
     * @brief Returns the operands of one precedence level joined by its operators, the multiplicative ones at 0 and the
     *        additive ones at 1, and a unary operand below 0.
     * @param depth The level.
     * @return The value, or std::nullopt when the reading stops.
     */
    [[nodiscard]] std::optional<long long> level(int depth);

    /**
     * @brief Returns a unary sign and its operand, a parenthesised expression or a decimal literal within an int's
     *        range.
     * @return The value, or std::nullopt when the reading stops.
     */
    [[nodiscard]] std::optional<long long> unary();

    /**
     * @brief Returns the token under the reader.
     * @return The token, empty once they are all read.
     */
    [[nodiscard]] std::string_view peek() const noexcept;

    /**
     * @brief The largest value an int holds, which every value read is kept within, the smallest being one below its
     *        negation.
     */
    static constexpr long long bound{std::numeric_limits<int>::max()};

    /**
     * @brief The tokens.
     */
    std::span<const std::string> words_;

    /**
     * @brief The index of the token under the reader.
     */
    std::size_t at_{0};
};

Constant_reader::Constant_reader(const std::span<const std::string> words) noexcept : words_{words}
{}

std::optional<long long> Constant_reader::value()
{
    const auto value{level(1)};

    return at_ == words_.size() ? value : std::nullopt;
}

std::optional<long long> Constant_reader::level(const int depth)
{
    if (depth < 0)
    {
        return unary();
    }

    auto left{level(depth - 1)};

    static constexpr std::array<std::array<std::string_view, 3>, 2> operators{{{"*", "/", "%"}, {"+", "-", ""}}};

    while (left)
    {
        const auto word{peek()};

        if (word.empty() || !std::ranges::contains(operators[depth], word))
        {
            break;
        }

        ++at_;

        const auto right{level(depth - 1)};

        if (!right || ((word == "/" || word == "%") && *right == 0))
        {
            return std::nullopt;
        }

        left = applied(word, *left, *right);

        if (*left > bound || *left < -bound - 1)
        {
            return std::nullopt;
        }
    }

    return left;
}

std::optional<long long> Constant_reader::unary()
{
    const auto word{peek()};

    if (word == "+" || word == "-")
    {
        ++at_;

        const auto inner{level(-1)};

        const auto plus{word == "+"};

        const auto signed_value{[plus](const long long operand) { return plus ? operand : -operand; }};

        return inner.transform(signed_value);
    }

    if (word == "(")
    {
        ++at_;

        const auto inner{level(1)};

        if (!inner || peek() != ")")
        {
            return std::nullopt;
        }

        ++at_;

        return inner;
    }

    if (word.empty() || !is_digit(word.front()))
    {
        return std::nullopt;
    }

    long long value{0};

    for (const char byte : word)
    {
        const auto digit{byte - '0'};

        if (!is_digit(byte) || value > (bound - digit) / 10)
        {
            return std::nullopt;
        }

        value = value * 10 + digit;
    }

    ++at_;

    return value;
}

std::string_view Constant_reader::peek() const noexcept
{
    return at_ < words_.size() ? std::string_view{words_[at_]} : std::string_view{};
}

/**
 * @brief An action and the scan pointers' configured spellings read under one way of defining the macros, and what the
 *        action does to a pointer there.
 *
 * A configured name is an expression and not always one word: `re2c:define:YYCURSOR = "in->cur";` names a member, which
 * re2c writes into the scanner as it stands, so the name is looked for as the run of tokens it is and an operator
 * beside that run moves it as one beside a bare name would. Both sides are read in one spelling, plain(), so that the
 * spellings the compiler takes for one are one to the comparison.
 */
class Pointer_reader
{
public:
    /**
     * @brief Binds the reader to the action, the pointers' spellings and one way of defining the macros.
     * @param code The action's text.
     * @param tokens The action's tokens, as c_tokens() reads them.
     * @param spelled The pointers' configured spellings, as c_tokens() reads each.
     * @param macros The macros the file defines.
     * @param arrays The arrays the configured spellings index, each as the run of words before its bracket.
     * @param choice The way the transparent macros are defined, one value each.
     */
    Pointer_reader(
            std::string_view code, const std::vector<C_token>& tokens, const std::vector<std::vector<C_token>>& spelled,
            const Macros_t& macros, const std::vector<std::vector<std::string>>& arrays,
            const Choice_t& choice) noexcept;

    /**
     * @brief Returns what the action does to a scan pointer under this way of defining the macros, when it moves one or
     *        hands one on.
     *
     * The neighbours of the name are read in the same spelling, so the `++` of `++(*in).cur` stands beside `in` as it
     * stands beside `in->cur`. The pointer handed on is judged before the parentheses that wrap the name alone are
     * stepped over, since `advance(cursors[0])` wraps it in a call's; parentheses around the whole name, `(in->cur)++`,
     * change nothing an operator beside them does, so the operators moving it are looked for outside every pair that
     * wraps the name.
     * @return What the refusal says after "the action", or std::nullopt when it leaves every pointer where it was.
     */
    [[nodiscard]] std::optional<std::string> refusal();

    /**
     * @brief Returns the first array the reading indexed by an expression it does not evaluate, which stands for any
     *        slot, the scan pointer's among them.
     * @return The array's run, or std::nullopt when every index was read to its value.
     */
    [[nodiscard]] const std::optional<std::string>& unread() const noexcept;

private:
    /**
     * @brief Returns a run of tokens as the reading compares it: a transparent macro is its value first, as the
     *        preprocessor makes it before the compiler compares anything, a function-like one's arguments dropped with
     *        it; an integer literal is its value; the spelling is reduced, reduce(); and an index into one of the
     *        arrays is read to its value. Every other parenthesis is a call's or an index's and stays, since
     *        `in->cursor()` reaches the pointer through a call and `slots[(i+j)*k]` is not `slots[i+(j*k)]`.
     * @param raw The tokens.
     * @return The words, each with the index of the token it came from.
     */
    [[nodiscard]] std::vector<Word> plain(const std::vector<C_token>& raw);

    /**
     * @brief Returns the tokens with each transparent macro replaced by its value, read as the action's own words are,
     *        so that `#define SLOT 0U` indexes slot 0. A function-like macro stands for its value only where it is
     *        invoked, `SLOT()`; the bare name is a name of the file's own, `constexpr unsigned SLOT = 1` beside
     *        `#define SLOT() 0`.
     * @param raw The tokens.
     * @return The words.
     */
    [[nodiscard]] std::vector<Word> substituted(const std::vector<C_token>& raw) const;

    /**
     * @brief Reads every index into one of the arrays to its value, or marks the array out of sight, the earlier of two
     *        in one spelling first so that `slots[1-1].cursors` is `slots[0].cursors` before the second is looked at.
     * @param out The spelling, its indices replaced by their values where they are constant.
     */
    void evaluate_indices(std::vector<Word>& out);

    /**
     * @brief Returns the array whose run ends at a word followed by an index's `[`, when one of the arrays does.
     * @param out The spelling.
     * @param at The index of the run's last word.
     * @return The array, its run written together, or std::nullopt.
     */
    [[nodiscard]] std::optional<std::string> indexed(const std::vector<Word>& out, std::size_t at) const;

    /**
     * @brief Returns whether the name between two words is moved: stepped by `++` or `--` on either side, or assigned
     *        by `=`, `+=` or `-=` after it.
     * @param plainly The action's words.
     * @param first The index of the name's first word, past the parentheses wrapping it.
     * @param last The index of its last word, before them.
     * @return True when it is.
     */
    [[nodiscard]] bool is_moved(const std::vector<Word>& plainly, std::size_t first, std::size_t last) const;

    /**
     * @brief Returns whether two words are one operator in the text the compiler reads: `++` and `--` are two tokens
     *        the source writes together, and `1 - -YYCURSOR[0]` is a difference of a negation that moves nothing, so
     *        only a line splice may stand between the two, the text the compiler reads having its splices taken out.
     * @param plainly The action's words.
     * @param one The index of the first.
     * @param other The index of the second.
     * @return True when they are.
     */
    [[nodiscard]] bool is_joined(const std::vector<Word>& plainly, std::size_t one, std::size_t other) const;

    /**
     * @brief The action's text.
     */
    std::string_view code_;

    /**
     * @brief The action's tokens.
     */
    const std::vector<C_token>& tokens_;

    /**
     * @brief The pointers' configured spellings.
     */
    const std::vector<std::vector<C_token>>& spelled_;

    /**
     * @brief The macros the file defines.
     */
    const Macros_t& macros_;

    /**
     * @brief The arrays the configured spellings index.
     */
    const std::vector<std::vector<std::string>>& arrays_;

    /**
     * @brief The way the transparent macros are defined.
     */
    const Choice_t& choice_;

    /**
     * @brief The first array indexed by an expression the reading does not evaluate.
     */
    std::optional<std::string> unread_;
};

Pointer_reader::Pointer_reader(
        const std::string_view code, const std::vector<C_token>& tokens,
        const std::vector<std::vector<C_token>>& spelled, const Macros_t& macros,
        const std::vector<std::vector<std::string>>& arrays, const Choice_t& choice) noexcept
    : code_{code}, tokens_{tokens}, spelled_{spelled}, macros_{macros}, arrays_{arrays}, choice_{choice}
{}

std::optional<std::string> Pointer_reader::refusal()
{
    std::vector<std::vector<std::string>> spellings{};

    for (const auto& pointer : spelled_)
    {
        const auto words{plain(pointer)};

        std::vector<std::string> spelling{};

        std::ranges::transform(words, std::back_inserter(spelling), &Word::text);

        if (!spelling.empty())
        {
            spellings.push_back(std::move(spelling));
        }
    }

    const auto plainly{plain(tokens_)};

    for (std::size_t here{0}; here < plainly.size(); ++here)
    {
        const auto spelled_here{
                [&plainly, here](const std::vector<std::string>& one) { return spells_at(plainly, here, one); }};

        const auto spelling{std::ranges::find_if(spellings, spelled_here)};

        if (spelling == std::ranges::end(spellings))
        {
            continue;
        }

        auto first{here};

        auto last{here + spelling->size() - 1};

        if (is_handed_on(plainly, first, last))
        {
            return std::format(
                    "hands on {}, by its address, a reference bound to it or a call it is passed to, so what becomes "
                    "of it is out of sight",
                    concatenated(*spelling));
        }

        while (is_wrapped(plainly, first, last))
        {
            --first;

            ++last;
        }

        if (is_moved(plainly, first, last))
        {
            return std::format(
                    "moves {}, so the next token does not begin where the match ends", concatenated(*spelling));
        }
    }

    return std::nullopt;
}

const std::optional<std::string>& Pointer_reader::unread() const noexcept
{
    return unread_;
}

std::vector<Word> Pointer_reader::plain(const std::vector<C_token>& raw)
{
    // The words are reduced in passes until nothing changes, so that a group inside a group, `((in))->cur`, comes down
    // to the name as `(in)->cur` does, and again once the indices are read to their values.
    auto out{substituted(raw)};

    reduce(out);

    evaluate_indices(out);

    reduce(out);

    return out;
}

std::vector<Word> Pointer_reader::substituted(const std::vector<C_token>& raw) const
{
    std::vector<Word> out{};

    for (std::size_t at{0}; at < raw.size(); ++at)
    {
        const auto& text{raw[at].text};

        const auto found{macros_.find(text)};

        const auto function_like{[&found, this] {
            if (found == macros_.end())
            {
                return false;
            }

            const auto& [name, macro]{*found};

            return macro.function_like;
        }()};

        const auto invoked{at + 1 < raw.size() && raw[at + 1].text == "("};

        const auto value{choice_.find(text)};

        if (value == choice_.end() || (function_like && !invoked))
        {
            out.push_back({.text = canonical(text), .origin = at});

            continue;
        }

        const auto& [macro, words]{*value};

        for (const auto& word : words)
        {
            out.push_back({.text = canonical(word), .origin = at});
        }

        if (function_like)
        {
            at = group_close(raw, at + 1, "(", ")");
        }
    }

    return out;
}

void Pointer_reader::evaluate_indices(std::vector<Word>& out)
{
    for (std::size_t at{0}; at + 1 < out.size(); ++at)
    {
        const auto array{indexed(out, at)};

        if (!array)
        {
            continue;
        }

        const auto close{group_close(out, at + 1, "[", "]")};

        if (close >= out.size())
        {
            break;
        }

        const auto index{std::span{out}.subspan(at + 2, close - at - 2)};

        std::vector<std::string> words{};

        std::ranges::transform(index, std::back_inserter(words), &Word::text);

        Constant_reader reader{words};

        if (const auto value{reader.value()})
        {
            const auto index_begin{out.begin() + static_cast<std::ptrdiff_t>(at) + 2};

            const auto index_end{out.begin() + static_cast<std::ptrdiff_t>(close)};

            const auto position{out.erase(index_begin, index_end)};

            out.insert(position, {.text = std::to_string(*value), .origin = out[at].origin});
        }
        else if (!unread_)
        {
            unread_ = *array;
        }
    }
}

std::optional<std::string> Pointer_reader::indexed(const std::vector<Word>& out, const std::size_t at) const
{
    if (out[at + 1].text != "[")
    {
        return std::nullopt;
    }

    for (const auto& run : arrays_)
    {
        if (run.size() > at + 1)
        {
            continue;
        }

        const auto ending{std::span{out}.subspan(at + 1 - run.size(), run.size())};

        if (std::ranges::equal(ending, run, {}, &Word::text))
        {
            return concatenated(run);
        }
    }

    return std::nullopt;
}

bool Pointer_reader::is_moved(const std::vector<Word>& plainly, const std::size_t first, const std::size_t last) const
{
    const auto word{[&plainly](const std::size_t index) { return text_at(plainly, index); }};

    const auto is_sign{[&word](const std::size_t index) {
        const auto text{word(index)};

        return text == "+" || text == "-";
    }};

    const auto stepped{
            (word(last + 1) == word(last + 2) && is_sign(last + 1) && is_joined(plainly, last + 1, last + 2)) ||
            (first >= 2 && word(first - 1) == word(first - 2) && is_sign(first - 1) &&
             is_joined(plainly, first - 2, first - 1))};

    const auto assigned{
            (word(last + 1) == "=" && word(last + 2) != "=") || (is_sign(last + 1) && word(last + 2) == "=")};

    return stepped || assigned;
}

bool Pointer_reader::is_joined(const std::vector<Word>& plainly, const std::size_t one, const std::size_t other) const
{
    if (other >= plainly.size())
    {
        return false;
    }

    const auto& left{tokens_[plainly[one].origin]};

    const auto& right{tokens_[plainly[other].origin]};

    auto between{code_.substr(left.end, right.at - left.end)};

    while (between.starts_with('\\'))
    {
        between.remove_prefix(1);

        between = without_leading(between, " \t");

        between.remove_prefix(between.starts_with('\r') ? 1 : 0);

        if (!between.starts_with('\n'))
        {
            return false;
        }

        between.remove_prefix(1);
    }

    return between.empty();
}

/**
 * @brief Returns the macros the pointers' spellings and the action name, each with the values it stands for, the one
 *        value of each definition that is a value and none for one that is more.
 * @param macros The macros the file defines.
 * @param spelled The pointers' configured spellings, as c_tokens() reads each.
 * @param tokens The action's tokens.
 * @return The values, by macro.
 */
[[nodiscard]] Values_t macro_values(
        const Macros_t& macros, const std::vector<std::vector<C_token>>& spelled, const std::vector<C_token>& tokens)
{
    Values_t values{};

    const auto note{[&values, &macros](const std::string& word) {
        if (macros.contains(word) && !values.contains(word))
        {
            values.emplace(word, plain_values(word, macros));
        }
    }};

    for (const auto& [at, end, text] : spelled | std::views::join)
    {
        note(text);
    }

    for (const auto& [at, end, text] : tokens)
    {
        note(text);
    }

    return values;
}

/**
 * @brief Returns the first macro a pointer is spelled through that stands for no one value, which leaves where the
 *        pointer stands out of sight, since what the scanner steps is then unknown.
 * @param spelled The pointers' configured spellings.
 * @param macros The macros the file defines.
 * @param values The values the macros stand for.
 * @return What the refusal says after "the action", or std::nullopt when every macro a spelling names is a value.
 */
[[nodiscard]] std::optional<std::string> hidden_pointer(
        const std::vector<std::vector<C_token>>& spelled, const Macros_t& macros, const Values_t& values)
{
    for (const auto& [at, end, text] : spelled | std::views::join)
    {
        if (macros.contains(text) && values.at(text).empty())
        {
            return std::format(
                    "names a scan pointer through {}, a macro whose replacement is more than a value, so where the "
                    "pointer stands is out of sight",
                    text);
        }
    }

    return std::nullopt;
}

/**
 * @brief Returns every way the transparent macros may be defined at the action, one value each; an opaque macro in the
 *        action stands as its name, macro_use() refusing its use where a pointer's spelling could hide in it.
 * @param values The values the macros stand for.
 * @return The ways, or what the refusal says after "the action" when they are more than the reading follows together.
 */
[[nodiscard]] std::expected<std::vector<Choice_t>, std::string> macro_choices(const Values_t& values)
{
    std::vector<Choice_t> choices{{}};

    for (const auto& [name, candidates] : values)
    {
        if (candidates.empty())
        {
            continue;
        }

        std::vector<Choice_t> next{};

        for (const auto& choice : choices)
        {
            for (const auto& candidate : candidates)
            {
                next.push_back(choice);

                next.back().emplace(name, candidate);
            }
        }

        if (next.size() > most_choices)
        {
            auto refusal{std::format(
                    "names {}, a macro defined in more ways than the reading follows together, so the pointers' "
                    "spellings are out of sight",
                    name)};

            return std::unexpected{std::move(refusal)};
        }

        choices = std::move(next);
    }

    return choices;
}

/**
 * @brief Returns the arrays the configured spellings index, each run before a bracket of a reduced spelling: `cursors`
 *        of `cursors[0]`, `in->cursors` of `in->cursors[0]`, and both `slots` and `slots[0].cursors` of
 *        `slots[0].cursors[0]`.
 *
 * An index into one of them, in the action or in a configured spelling, is read to its value when it is a constant
 * expression, `+0`, `1-1` and `(0)` being `0`, and is out of sight when it is not, `cursors[i]` and `cursors[SLOT]`
 * under `constexpr unsigned SLOT` standing for any slot, the scan pointer's among them.
 * @param spelled The pointers' configured spellings.
 * @return The arrays, each as the run of words before its bracket.
 */
[[nodiscard]] std::vector<std::vector<std::string>> indexed_arrays(const std::vector<std::vector<C_token>>& spelled)
{
    std::vector<std::vector<std::string>> arrays{};

    for (const auto& pointer : spelled)
    {
        // An integer literal reads as its value.
        const auto as_word{[]<typename Entry>(const Entry& entry) {
            const auto& [at, token]{entry};

            return Word{.text = canonical(token.text), .origin = static_cast<std::size_t>(at)};
        }};

        std::vector<Word> reduced{};

        std::ranges::transform(std::views::enumerate(pointer), std::back_inserter(reduced), as_word);

        reduce(reduced);

        for (std::size_t at{1}; at < reduced.size(); ++at)
        {
            if (reduced[at].text != "[")
            {
                continue;
            }

            std::vector<std::string> run{};

            std::ranges::transform(reduced | std::views::take(at), std::back_inserter(run), &Word::text);

            arrays.push_back(std::move(run));
        }
    }

    return arrays;
}

/**
 * @brief Returns whether a token names a label: a name followed by a lone colon, `loop:`, a `::` and the `case` and
 *        `default` labels of a switch being none.
 * @param tokens The tokens, as c_tokens() reads them.
 * @param at The index of the name.
 * @return True when it does.
 */
[[nodiscard]] bool is_label(const std::vector<C_token>& tokens, const std::size_t at)
{
    return at + 1 < tokens.size() && tokens[at + 1].text == ":" &&
           (at + 2 >= tokens.size() || tokens[at + 2].text != ":") && starts_name(tokens[at].text) &&
           tokens[at].text != "case" && tokens[at].text != "default";
}

/**
 * @brief Returns why an action leaves a scan pointer elsewhere than where the match ended, when it does: re2c's
 *        `YYCURSOR`, `YYMARKER` or `YYCTXMARKER` under whatever names the block's configurations give them, moved by an
 *        assignment or a step, `YYCURSOR = p`, `++YYCURSOR`, `in->cur += 2`; or a pointer whose configured spelling the
 *        reading cannot settle.
 * @param code The action's text.
 * @param pointers The names the block gives the scan pointers.
 * @param macros The macros the file defines: a transparent one, `#define SLOT 0`, stands for its value in a configured
 *        name and in an action alike, so `cursors[SLOT]` and `cursors[0]` are one spelling to the comparison, whichever
 *        side writes which; one defined more than once stands for each of its values in turn, since which is live at
 *        the action is not decided here; a function-like one stands for its value whatever its arguments; and a pointer
 *        named through an opaque one is out of sight.
 * @return What the refusal says after "the action", or std::nullopt when the action leaves every pointer where it was.
 */
[[nodiscard]] std::optional<std::string> moved_cursor(
        const std::string_view code, std::span<const std::string> pointers, const Macros_t& macros)
{
    const auto tokens{c_tokens(code)};

    std::vector<std::vector<C_token>> spelled{};

    std::ranges::transform(pointers, std::back_inserter(spelled), c_tokens);

    const auto values{macro_values(macros, spelled, tokens)};

    if (auto hidden{hidden_pointer(spelled, macros, values)})
    {
        return hidden;
    }

    const auto choices{macro_choices(values)};

    if (!choices)
    {
        return choices.error();
    }

    const auto arrays{indexed_arrays(spelled)};

    for (const auto& choice : *choices)
    {
        Pointer_reader reader{code, tokens, spelled, macros, arrays, choice};

        if (auto refusal{reader.refusal()})
        {
            return refusal;
        }

        if (reader.unread())
        {
            return std::format(
                    "indexes {} by an expression the reading does not evaluate, so whether it names the scan pointer "
                    "is out of sight",
                    *reader.unread());
        }
    }

    return std::nullopt;
}

/**
 * @brief Returns an action's code as its returns are judged: before a transition rule's code, its `=> COMMENT`, which
 *        sets the condition and is no statement of the action's, dropped.
 * @param code The action's code, its leading blanks dropped.
 * @return The code judged.
 */
[[nodiscard]] std::string_view transition_stripped(std::string_view code)
{
    if (!code.starts_with(transition_opener))
    {
        return code;
    }

    code.remove_prefix(transition_opener.size());

    code = without_leading(code, " \t");

    const auto name_end{std::ranges::find_if_not(code, is_name_byte)};

    code.remove_prefix(static_cast<std::size_t>(name_end - code.begin()));

    return code;
}

} // namespace

std::set<std::string, std::less<>> restart_labels(
        const std::string_view source, const std::size_t opener, const Returning_t& returning)
{
    // The labels outside every block, as c_tokens() reads the file with the blocks' comments dropped.
    std::set<std::string, std::less<>> restarts{};

    const auto outside{c_tokens(source)};

    static constexpr std::array<std::string_view, 4> statement_ends{";", "{", "}", ")"};

    for (std::size_t at{0}; at + 1 < outside.size(); ++at)
    {
        const auto opens{at == 0 || std::ranges::contains(statement_ends, outside[at - 1].text)};

        // A label restarts the scan only before the block and where nothing between it and the block leaves.
        auto inert{outside[at].at < opener};

        for (auto piece{at + 2}; inert && piece < outside.size() && outside[piece].at < opener; ++piece)
        {
            const auto& text{outside[piece].text};

            inert = text != "return" && text != "goto" && text != "break" && text != "continue" &&
                    !is_label(outside, piece) && !std::ranges::contains(returning, text);
        }

        if (opens && is_label(outside, at) && inert)
        {
            restarts.emplace(outside[at].text);
        }
    }

    return restarts;
}

void refuse_action(
        const Action& action, const Pointers_t& pointers, const Macros_t& macros, const Returning_t& returning,
        const std::set<std::string, std::less<>>& restarts)
{
    std::vector<std::string> names{};

    std::ranges::transform(pointers, std::back_inserter(names), &Pointer_name::name);

    const auto& [code, line, what, of_rule]{action};

    const auto refuse_use{[&line](const std::string_view use) { throw Spec_error{action_refusal(use), line}; }};

    if (const auto moved{moved_cursor(code, names, macros)})
    {
        throw Spec_error{std::format("{}{}", what, *moved), line};
    }

    if (const auto use{directive_use(code)})
    {
        refuse_use(*use);
    }

    // A rule's action falls into the next rule's unless it leaves; a shortcut rule, `:=> COMMENT`, has no code of its
    // own and re2c writes the jump to the condition itself.
    const auto opened{without_leading(code, " \t\r\n")};

    const auto shortcut{opened.starts_with(shortcut_opener)};

    const auto judged{transition_stripped(opened)};

    if (const auto use{returns_undecided(judged, returning, false, restarts, of_rule && !shortcut)})
    {
        refuse_use(*use);
    }

    const std::vector<std::string_view> pointer_names{names.begin(), names.end()};

    if (const auto use{macro_use(code, macros, pointer_names)})
    {
        refuse_use(*use);
    }
}

} // namespace munch::tools::audit
