#include "munch/tools/probes/recovery_corpora.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "munch/tools/probes/recovery_lcg.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements recovery_corpora.hpp: the pieces a line is drawn from are private to this unit.

/**
 * @brief The words both corpora draw identifiers, string contents and object keys from.
 */
constexpr std::array<std::string_view, 8> kWords{"count", "offset", "state", "token",
                                                 "chunk", "origin", "table", "index"};

/**
 * @brief The operators a C-like piece draws.
 */
constexpr std::array<char, 8> kOperators{'+', '-', '*', '=', '<', '>', '&', '|'};

/**
 * @brief The literal names and numbers a JSON value draws.
 */
constexpr std::array<std::string_view, 8> kValues{"true", "false", "null", "42", "-1.5e3", "0", "271828", "-7"};

/**
 * @brief Appends a two-word string literal, the words drawn in order.
 * @param out The corpus appended to.
 * @param random The corpus stream, drawn once per word.
 */
void append_two_word_string(std::string& out, Lcg& random)
{
    out += '"';

    out += kWords[random.next() % 8];

    out += ' ';

    out += kWords[random.next() % 8];

    out += '"';
}

/**
 * @brief Appends one C-like piece, chosen by one draw in ten: a number, a string literal (an identifier without
 *        strings), an operator and an identifier, an identifier and a semicolon, a parenthesized identifier, or an
 *        identifier and a space.
 * @param out The corpus appended to.
 * @param random The corpus stream, drawn for the choice and then for the piece's number or words.
 * @param strings Whether string literals are drawn.
 */
void append_piece(std::string& out, Lcg& random, const bool strings)
{
    switch (random.next() % 10)
    {
    case 0:
        out += std::to_string(random.next());

        break;

    case 1:
        if (strings)
        {
            append_two_word_string(out, random);
        }
        else
        {
            out += kWords[random.next() % 8];
        }

        break;

    case 2:
        out += kOperators[random.next() % 8];

        out += ' ';

        out += kWords[random.next() % 8];

        break;

    case 3:
        out += kWords[random.next() % 8];

        out += ';';

        break;

    case 4:
        out += '(';

        out += kWords[random.next() % 8];

        out += ')';

        break;

    default:
        out += kWords[random.next() % 8];

        out += ' ';

        break;
    }
}

/**
 * @brief Appends one JSON value, chosen by one draw in three: a literal name or number, a two-word string, or an
 *        array of two literal names or numbers.
 * @param out The corpus appended to.
 * @param random The corpus stream, drawn for the choice and then for each element.
 */
void append_json_value(std::string& out, Lcg& random)
{
    switch (random.next() % 3)
    {
    case 0:
        out += kValues[random.next() % 8];

        break;

    case 1:
        append_two_word_string(out, random);

        break;

    default:
        out += '[';

        out += kValues[random.next() % 8];

        out += ", ";

        out += kValues[random.next() % 8];

        out += ']';

        break;
    }
}

/**
 * @brief Appends a block comment of one to four lines, each a newline and two to five space-led words; the interior
 *        holds no `*`, so the comment closes exactly where written.
 * @param out The corpus appended to.
 * @param random The corpus stream, drawn for the line count, then per line for its word count and each word.
 */
void append_block_comment(std::string& out, Lcg& random)
{
    out += "/*";

    const auto lines{1 + random.next() % 4};

    for (std::size_t line{0}; line < lines; ++line)
    {
        out += '\n';

        const auto interior{2 + random.next() % 4};

        for (std::size_t piece{0}; piece < interior; ++piece)
        {
            out += ' ';

            out += kWords[random.next() % 8];
        }
    }

    out += " */\n";
}

/**
 * @brief Appends one newline-terminated code line of three to eight pieces, each followed by a space, and with line
 *        comments, on one draw in four, a one-word line comment.
 * @param out The corpus appended to.
 * @param random The corpus stream, drawn for the piece count, the pieces, and with line comments the comment.
 * @param features The token families drawn.
 */
void append_code_line(std::string& out, Lcg& random, const C_like_features& features)
{
    const auto pieces{3 + random.next() % 6};

    for (std::size_t piece{0}; piece < pieces; ++piece)
    {
        append_piece(out, random, features.strings);

        out += ' ';
    }

    if (features.line_comments && random.next() % 4 == 0)
    {
        out += "// ";

        out += kWords[random.next() % 8];
    }

    out += '\n';
}

/**
 * @brief Cuts a generated corpus back to its last newline within a size and pads it with newlines to that size, which
 *        closes every literal and comment the cut would leave open.
 * @param out The corpus, at least bytes long and holding a newline within its first bytes.
 * @param bytes The size.
 */
void cut_to_size(std::string& out, const std::size_t bytes)
{
    const auto last_newline{out.rfind('\n', bytes - 1)};

    out.resize(last_newline + 1);

    out.append(bytes - out.size(), '\n');
}

/**
 * @brief Appends one newline-terminated JSON object of one to four members, each a quoted key and a value.
 * @param out The corpus appended to.
 * @param random The corpus stream, drawn for the member count, then per member for its key and its value.
 */
void append_json_line(std::string& out, Lcg& random)
{
    out += '{';

    const auto members{1 + random.next() % 4};

    for (std::size_t member{0}; member < members; ++member)
    {
        if (member != 0)
        {
            out += ", ";
        }

        out += '"';

        out += kWords[random.next() % 8];

        out += "\": ";

        append_json_value(out, random);
    }

    out += "}\n";
}

} // namespace

std::string c_like_corpus(const std::size_t bytes, const C_like_features& features)
{
    std::string out{};

    out.reserve(bytes + 128);

    Lcg random{0x5eed0001U};

    while (out.size() < bytes)
    {
        if (features.block_comments && random.next() % 3 == 0)
        {
            append_block_comment(out, random);
        }
        else
        {
            append_code_line(out, random, features);
        }
    }

    cut_to_size(out, bytes);

    return out;
}

std::string json_corpus(const std::size_t bytes)
{
    std::string out{};

    out.reserve(bytes + 128);

    Lcg random{0x5eed0002U};

    while (out.size() < bytes)
    {
        append_json_line(out, random);
    }

    cut_to_size(out, bytes);

    return out;
}

} // namespace munch::tools::probes
