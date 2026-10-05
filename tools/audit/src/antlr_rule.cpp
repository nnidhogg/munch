#include "munch/tools/audit/antlr_rule.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace munch::tools::audit
{
Nullable_t never_empty()
{
    return {};
}

Nullable_t either_empty(Nullable_t left, const Nullable_t& right)
{
    left.insert(left.end(), right.begin(), right.end());

    return matches_empty(left, {}) ? always_empty() : left;
}

bool matches_empty(const Nullable_t& formula, const std::set<std::string, std::less<>>& nullable)
{
    const auto known_empty{[&nullable](const std::string& name) { return nullable.contains(name); }};

    const auto term_holds{
            [&known_empty](const std::vector<std::string>& term) { return std::ranges::all_of(term, known_empty); }};

    return std::ranges::any_of(formula, term_holds);
}

Nullable_t always_empty()
{
    return {{}};
}

Nullable_t both_empty(const Nullable_t& left, const Nullable_t& right)
{
    Nullable_t joined{};

    for (const auto& outer : left)
    {
        for (const auto& inner : right)
        {
            auto term{outer};

            term.insert(term.end(), inner.begin(), inner.end());

            joined.push_back(std::move(term));
        }
    }

    return matches_empty(joined, {}) ? always_empty() : joined;
}

std::string closure_refusal(const std::string_view rule)
{
    return std::format(
            "the rule {} contains a closure with at least one alternative that can match the empty string, which ANTLR "
            "rejects",
            rule);
}

bool is_atomic(const std::string_view expression)
{
    if (expression.size() < 2)
    {
        return true;
    }

    const auto opener{expression.front()};

    if (opener == '"')
    {
        const auto size{expression.size()};

        // The three literals that are one byte: the byte itself, an escape and a hex escape.
        static constexpr std::string_view plain{R"("x")"};

        static constexpr std::string_view escaped{R"("\n")"};

        static constexpr std::string_view hex{R"("\xHH")"};

        return size == plain.size() || (size == escaped.size() && expression[1] == '\\') ||
               (size == hex.size() && expression.substr(1, 2) == R"(\x)");
    }

    if (opener != '[' && opener != '(' && opener != '{')
    {
        return false;
    }

    const auto closer{[opener] {
        switch (opener)
        {
        case '[':
            return ']';
        case '(':
            return ')';
        default:
            return '}';
        }
    }()};

    // The unit closes at the end if no closer of its kind comes earlier at depth one; escapes are stepped over.
    std::size_t depth{0};

    for (std::size_t at{0}; at < expression.size(); ++at)
    {
        if (expression[at] == '\\')
        {
            ++at;

            continue;
        }

        if (expression[at] == opener && (opener != '[' || depth == 0))
        {
            ++depth;

            continue;
        }

        if (expression[at] != closer)
        {
            continue;
        }

        --depth;

        if (depth == 0)
        {
            return at + 1 == expression.size();
        }
    }

    return false;
}

std::optional<std::string> rest_spelling(const std::vector<Element>& elements, const std::size_t from)
{
    std::string bytes{};

    for (const auto& element : elements | std::views::drop(from))
    {
        if (element.expression.empty())
        {
            continue;
        }

        const auto& literal{element.literal};

        const auto ascii{literal && !holds_beyond_ascii(*literal)};

        if (!ascii || element.suffix != 0)
        {
            return std::nullopt;
        }

        bytes += *literal;
    }

    return bytes.empty() ? std::nullopt : std::optional{std::move(bytes)};
}

std::string avoiding(const std::string_view terminator, const Alphabet& alphabet)
{
    const auto states{terminator.size()};

    const auto final{states};

    // Labels between states as regex text: absent for no edge, empty for the empty string.
    std::vector label(states + 1, std::vector<std::optional<std::string>>(states + 1));

    const auto join{[](std::optional<std::string>& into, const std::string& more) {
        if (!into)
        {
            into = more;
        }
        else if (into->empty() || more.empty())
        {
            const auto& present{into->empty() ? more : *into};

            into = std::format("({})?", present);
        }
        else
        {
            into = std::format("({}|{})", *into, more);
        }
    }};

    const auto next_state{[terminator, states](const std::size_t from, const std::size_t byte) {
        std::string tail{terminator.substr(0, from)};

        tail.push_back(static_cast<char>(byte));

        auto to{std::min(tail.size(), states)};

        while (to > 0 && terminator.substr(0, to) != std::string_view{tail}.substr(tail.size() - to))
        {
            --to;
        }

        return to;
    }};

    const auto is_overlapped{[terminator](const std::size_t length) {
        return length > 0 && terminator.substr(length) == terminator.substr(0, terminator.size() - length);
    }};

    // The prefix automaton: from a prefix of length i on byte b, the longest prefix of the terminator that is a suffix
    // of the prefix followed by b.
    for (std::size_t from{0}; from < states; ++from)
    {
        std::vector<Ascii_t> onto(states + 1);

        for (std::size_t byte{0}; byte < Ascii_t{}.size(); ++byte)
        {
            if (!alphabet.ascii.test(byte))
            {
                continue;
            }

            const auto to{next_state(from, byte)};

            onto[to].set(byte);
        }

        for (std::size_t to{0}; to < states; ++to)
        {
            if (!onto[to].any())
            {
                continue;
            }

            const auto members{bracket(onto[to])};

            join(label[from][to], members);
        }

        if (!alphabet.beyond.empty())
        {
            const auto beyond_step{step(Alphabet{.ascii = {}, .beyond = alphabet.beyond})};

            join(label[from][0], beyond_step);
        }

        if (!is_overlapped(from))
        {
            join(label[from][final], "");
        }
    }

    // Adds to each edge out of `from` the paths through the eliminated state onward, its loop between.
    const auto bypass{[&label, &join, final](const std::size_t from, const std::size_t gone, const std::string& loop) {
        for (std::size_t to{0}; to <= final; ++to)
        {
            if (to == gone || !label[gone][to])
            {
                continue;
            }

            const auto through{std::format("{}{}{}", *label[from][gone], loop, *label[gone][to])};

            join(label[from][to], through);
        }
    }};

    const auto loop_of{[&label](const std::size_t state) {
        return label[state][state] ? std::format("({})*", *label[state][state]) : std::string{};
    }};

    // Elimination of every state but the start and the final one.
    for (std::size_t gone{1}; gone < states; ++gone)
    {
        const auto loop{loop_of(gone)};

        for (std::size_t from{0}; from <= final; ++from)
        {
            if (from == gone || !label[from][gone])
            {
                continue;
            }

            bypass(from, gone, loop);
        }

        for (std::size_t other{0}; other <= final; ++other)
        {
            label[gone][other] = std::nullopt;

            label[other][gone] = std::nullopt;
        }
    }

    const auto loop{loop_of(0)};

    const auto rest{label[0][final].value_or("")};

    // Where the body can only be empty, `'a'*? 'aa'` over the one character the terminator begins with, the loop
    // contributes nothing and the terminator alone is the match; an empty group is no pattern the parser reads.
    return loop.empty() && rest.empty() ? std::string{} : std::format("({}{})", loop, rest);
}

void join_onto(std::string& joined, const std::string_view text, const std::string_view separator)
{
    if (!joined.empty())
    {
        joined += separator;
    }

    joined += text;
}

std::string grouped(const std::string_view inner, const bool optional)
{
    const std::string_view suffix{optional ? "?" : ""};

    return std::format("({}){}", inner, suffix);
}

} // namespace munch::tools::audit
