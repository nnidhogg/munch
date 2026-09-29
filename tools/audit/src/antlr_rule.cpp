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
namespace
{
// Implements antlr_rule.hpp: whether a loop's body is past the terminator's occurrence is private to this unit.

/**
 * @brief Whether the loop's body, having read the terminator's first `length` bytes and nothing longer of it, is
 *        already past the point ANTLR's loop stops at, because the terminator appended there spells an occurrence of
 *        itself that begins inside the body.
 *
 * Appending the terminator after a prefix of it of length j spells an occurrence beginning j bytes early exactly when
 * the terminator's own bytes from j on are its first bytes, that is when j is a period of it. `'aa'` after one `a` is
 * the case: the body's `a` and the terminator's first `a` are an occurrence of `aa`, so ANTLR's fewest characters
 * stopped a byte earlier and the body may not end there. The longest prefix the body ends in is the only one to test: a
 * shorter prefix the body also ends in is a suffix of the longest one, and a periodic terminator's suffix of that kind
 * makes the longest one a period too.
 * @param terminator The terminator's bytes.
 * @param length The length of the longest prefix of the terminator the body ends with, below the whole of it.
 * @return True when the terminator completes that prefix into an occurrence of itself.
 */
[[nodiscard]] bool is_overlapped(const std::string_view terminator, const std::size_t length)
{
    return length > 0 && terminator.substr(length) == terminator.substr(0, terminator.size() - length);
}

} // namespace

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
    return std::ranges::any_of(formula, [&nullable](const std::vector<std::string>& term) {
        return std::ranges::all_of(term, [&nullable](const std::string& name) { return nullable.contains(name); });
    });
}

Nullable_t always_empty()
{
    return {{}};
}

Nullable_t both_empty(const Nullable_t& left, const Nullable_t& right)
{
    Nullable_t joined;

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

bool is_atomic(const std::string_view expression)
{
    if (expression.size() < 2)
    {
        return true;
    }

    const auto opener{expression.front()};

    if (opener == '"')
    {
        return expression.size() == 3 || (expression.size() == 4 && expression[1] == '\\') ||
               (expression.size() == 6 && expression.substr(1, 2) == R"(\x)");
    }

    if (opener != '[' && opener != '(' && opener != '{')
    {
        return false;
    }

    // The unit closes at the end if no closer of its kind comes earlier at depth one; escapes are stepped over.
    const auto closer{opener == '[' ? ']' : opener == '(' ? ')' : '}'};

    std::size_t depth{0};

    for (std::size_t at{0}; at < expression.size(); ++at)
    {
        if (expression[at] == '\\')
        {
            ++at;
        }
        else if (expression[at] == opener && (opener != '[' || depth == 0))
        {
            ++depth;
        }
        else if (expression[at] == closer && --depth == 0)
        {
            return at + 1 == expression.size();
        }
    }

    return false;
}

std::optional<std::string> rest_spelling(const std::vector<Element>& elements, const std::size_t from)
{
    std::string bytes;

    for (const auto& [expression, literal, spelling, alphabet, first, nullable, one_length, characters, suffix, lazy] :
         elements | std::views::drop(from))
    {
        if (expression.empty())
        {
            continue;
        }

        const auto ascii{literal && !holds_beyond_ascii(*literal)};

        if (!ascii || suffix != 0)
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
            into = std::format("({})?", into->empty() ? more : *into);
        }
        else
        {
            into = std::format("({}|{})", *into, more);
        }
    }};

    // The prefix automaton: from a prefix of length i on byte b, the longest prefix of the terminator that is a suffix
    // of the prefix followed by b.
    for (std::size_t from{0}; from < states; ++from)
    {
        std::vector<Ascii_t> onto(states + 1);

        for (std::size_t byte{0}; byte < 128; ++byte)
        {
            if (!alphabet.ascii.test(byte))
            {
                continue;
            }

            std::string tail{terminator.substr(0, from)};

            tail.push_back(static_cast<char>(byte));

            auto to{std::min(tail.size(), states)};

            while (to > 0 && terminator.substr(0, to) != std::string_view{tail}.substr(tail.size() - to))
            {
                --to;
            }

            onto[to].set(byte);
        }

        for (std::size_t to{0}; to < states; ++to)
        {
            if (!onto[to].any())
            {
                continue;
            }

            join(label[from][to], bracket(onto[to]));
        }

        if (!alphabet.beyond.empty())
        {
            join(label[from][0], step(Alphabet{.ascii = {}, .beyond = alphabet.beyond}));
        }

        if (!is_overlapped(terminator, from))
        {
            join(label[from][final], "");
        }
    }

    // Elimination of every state but the start and the final one.
    for (std::size_t gone{1}; gone < states; ++gone)
    {
        const auto loop{label[gone][gone] ? std::format("({})*", *label[gone][gone]) : std::string{}};

        for (std::size_t from{0}; from <= final; ++from)
        {
            if (from == gone || !label[from][gone])
            {
                continue;
            }

            for (std::size_t to{0}; to <= final; ++to)
            {
                if (to == gone || !label[gone][to])
                {
                    continue;
                }

                join(label[from][to], *label[from][gone] + loop + *label[gone][to]);
            }
        }

        for (std::size_t other{0}; other <= final; ++other)
        {
            label[gone][other] = std::nullopt;

            label[other][gone] = std::nullopt;
        }
    }

    const auto loop{label[0][0] ? std::format("({})*", *label[0][0]) : std::string{}};

    const auto rest{label[0][final].value_or("")};

    // Where the body can only be empty, `'a'*? 'aa'` over the one character the terminator begins with, the loop
    // contributes nothing and the terminator alone is the match; an empty group is no pattern the parser reads.
    return loop.empty() && rest.empty() ? std::string{} : std::format("({}{})", loop, rest);
}

} // namespace munch::tools::audit
