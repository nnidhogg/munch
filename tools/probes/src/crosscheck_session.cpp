#include "munch/tools/probes/crosscheck_session.hpp"

#include <cstddef>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/regex.hpp"

namespace munch::tools::probes
{
namespace
{
/**
 * @brief Decodes a protocol field.
 * @param field The field as read.
 * @return The empty string for `-`, the field otherwise.
 */
std::string decode(const std::string& field)
{
    return field == "-" ? std::string{} : field;
}

} // namespace

bool Crosscheck_session::run(
        std::istream& in, std::ostream& out, std::ostream& err, const std::string_view program, const Protocol protocol)
{
    const auto anchors{protocol == Protocol::anchor};

    std::string command{};

    while (in >> command)
    {
        if (command != "SET" && command != "END" && !ready())
        {
            err << program << ": " << command << " before any SET\n";

            return false;
        }

        if (command == "END")
        {
            break;
        }

        if (command == "SET")
        {
            set(in);

            continue;
        }

        if (command == "Q")
        {
            query(in, out);

            continue;
        }

        if (anchors && command == "N")
        {
            anchored(in, out);

            continue;
        }

        if (anchors && command == "M")
        {
            repair(in, out);

            continue;
        }

        if (command == "T")
        {
            tokenize(in, out);

            continue;
        }

        err << program << ": unknown command " << command << '\n';

        return false;
    }

    out.flush();

    return true;
}

bool Crosscheck_session::ready() const noexcept
{
    return lexer_.has_value();
}

void Crosscheck_session::set(std::istream& in)
{
    std::size_t count{0};

    in >> count;

    core::Builder builder{};

    // Each token's kind and priority is its position in the list.
    for (std::size_t index{0}; index < count; ++index)
    {
        std::string field{};

        in >> field;

        const auto literal{decode(field)};

        builder.add_token(regex::text(literal), index, index);
    }

    lexer_.emplace(builder.build());
}

void Crosscheck_session::query(std::istream& in, std::ostream& out) const
{
    std::string field{};

    std::size_t from{0};

    in >> field >> from;

    const auto input{decode(field)};

    const auto found{lexer_->next_certified_evidence(input, from)};

    if (!found)
    {
        out << "R\n";

        return;
    }

    const auto [start, evidence_begin, evidence_end, window]{*found};

    out << "A " << start << ' ' << evidence_begin << ' ' << evidence_end << ' ' << (window ? 1 : 0) << '\n';
}

void Crosscheck_session::anchored(std::istream& in, std::ostream& out) const
{
    std::string field{};

    std::size_t from{0};

    in >> field >> from;

    const auto tail{decode(field)};

    const auto found{lexer_->next_anchored_start(tail, from)};

    if (!found)
    {
        out << "NR\n";

        return;
    }

    out << "N " << *found << '\n';
}

void Crosscheck_session::repair(std::istream& in, std::ostream& out) const
{
    std::string field{};

    in >> field;

    const auto tail{decode(field)};

    const auto found{lexer_->minimal_repair(tail)};

    if (!found)
    {
        out << "MR\n";

        return;
    }

    // An empty repair is written as `-`.
    const auto encoded{found->empty() ? std::string{"-"} : *found};

    out << "M " << encoded << '\n';
}

void Crosscheck_session::tokenize(std::istream& in, std::ostream& out) const
{
    std::string field{};

    in >> field;

    const auto input{decode(field)};

    std::vector<std::size_t> lengths{};

    /**
     * @brief Records each committed token's length.
     * @param length The token's length.
     */
    const auto record{[&lengths](const std::size_t, const std::size_t length) { lengths.push_back(length); }};

    const auto committed{lexer_->tokenize_all<std::size_t>(input, record)};

    out << "T " << committed << ' ' << lengths.size();

    for (const auto length : lengths)
    {
        out << ' ' << length;
    }

    out << '\n';
}

} // namespace munch::tools::probes
