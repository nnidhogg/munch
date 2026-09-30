#include "munch/tools/probes/crosscheck_session.hpp"

#include <cstddef>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/regex.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements crosscheck_session.hpp: the field coding and the lexer built from a token list are private to this unit.

/**
 * @brief Decodes a protocol field.
 * @param field The field as read.
 * @return The empty string for `-`, the field otherwise.
 */
std::string decode(const std::string& field)
{
    return field == "-" ? std::string{} : field;
}

/**
 * @brief Builds a lexer over literal tokens.
 * @param tokens The tokens, each one's kind and priority its position in the list.
 * @return The lexer.
 */
core::Lexer build(const std::vector<std::string>& tokens)
{
    core::Builder builder;

    for (std::size_t index{0}; index < tokens.size(); ++index)
    {
        builder.add_token(regex::text(tokens[index]), index, index);
    }

    return builder.build();
}

/**
 * @brief Encodes a string as a protocol field.
 * @param text The string.
 * @return `-` for the empty string, the string otherwise.
 */
std::string encode(const std::string& text)
{
    return text.empty() ? std::string{"-"} : text;
}
} // namespace

void Crosscheck_session::set(std::istream& in)
{
    std::size_t count{0};

    in >> count;

    std::vector<std::string> tokens;

    for (std::size_t index{0}; index < count; ++index)
    {
        std::string field;

        in >> field;

        tokens.push_back(decode(field));
    }

    lexer_.emplace(build(tokens));
}

bool Crosscheck_session::ready() const noexcept
{
    return lexer_.has_value();
}

void Crosscheck_session::query(std::istream& in, std::ostream& out) const
{
    std::string field;

    std::size_t from{0};

    in >> field >> from;

    const auto input{decode(field)};

    const auto found{lexer_->next_certified_evidence(input, from)};

    if (found)
    {
        out << "A " << found->start << ' ' << found->evidence_begin << ' ' << found->evidence_end << ' '
            << (found->window ? 1 : 0) << '\n';
    }
    else
    {
        out << "R\n";
    }
}

void Crosscheck_session::tokenize(std::istream& in, std::ostream& out) const
{
    std::string field;

    in >> field;

    const auto input{decode(field)};

    std::vector<std::size_t> lengths;

    const auto record{[&lengths](const std::size_t, const std::size_t length) { lengths.push_back(length); }};

    const auto committed{lexer_->tokenize_all<std::size_t>(input, record)};

    out << "T " << committed << ' ' << lengths.size();

    for (const auto length : lengths)
    {
        out << ' ' << length;
    }

    out << '\n';
}

void Crosscheck_session::anchored(std::istream& in, std::ostream& out) const
{
    std::string field;

    std::size_t from{0};

    in >> field >> from;

    const auto tail{decode(field)};

    const auto found{lexer_->next_anchored_start(tail, from)};

    if (found)
    {
        out << "N " << *found << '\n';
    }
    else
    {
        out << "NR\n";
    }
}

void Crosscheck_session::repair(std::istream& in, std::ostream& out) const
{
    std::string field;

    in >> field;

    const auto tail{decode(field)};

    const auto found{lexer_->minimal_repair(tail)};

    if (found)
    {
        out << "M " << encode(*found) << '\n';
    }
    else
    {
        out << "MR\n";
    }
}

} // namespace munch::tools::probes
