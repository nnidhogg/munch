// A standalone probe over munch's public certificate walk, for an independent exhaustive checker.
//
// It shares no machinery with the campaign harness under tools/probes: it builds a Lexer from a
// literal token list and reports, for each query, exactly what Lexer::next_certified_evidence()
// answers, plus what Lexer::tokenize_all() commits. Nothing is asserted here; the driver owns the
// verdicts and compares against its own reference model.
//
// Protocol, line oriented on stdin, one token per whitespace-separated field:
//
//   SET <count> <token> ...   build a lexer over the listed literal tokens, priority = list order
//   Q <input> <from>          print the walk's answer for (input, from)
//   T <input>                 print what the maximal-munch scan commits on input
//   END                       stop
//
// A string field of "-" denotes the empty string; the driver's alphabets never contain '-'.
//
// Output, one line per Q or T query, in query order:
//
//   A <start> <evidence_begin> <evidence_end> <window>   the walk answered
//   R                                                    the walk refused
//   T <consumed> <token_count> <length> ...              the scan's committed lengths

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/regex.hpp"

namespace
{
std::string decode(const std::string& field)
{
    return field == "-" ? std::string{} : field;
}

munch::core::Lexer build(const std::vector<std::string>& tokens)
{
    munch::core::Builder builder;

    for (std::size_t index{0}; index < tokens.size(); ++index)
    {
        builder.add_token(munch::regex::text(tokens[index]), index, index);
    }

    return builder.build();
}
} // namespace

int main()
{
    std::ios::sync_with_stdio(false);

    // Zero or one current lexer, re-seated by each SET; optional says exactly that,
    // where a single-slot vector only implied it.
    std::optional<munch::core::Lexer> lexer;

    std::string command;

    while (std::cin >> command)
    {
        if (command != "SET" && command != "END" && !lexer)
        {
            std::cerr << "crosscheck_scan: " << command << " before any SET\n";
            return 1;
        }

        if (command == "END")
        {
            break;
        }

        if (command == "SET")
        {
            std::size_t count{0};

            std::cin >> count;

            std::vector<std::string> tokens;

            for (std::size_t index{0}; index < count; ++index)
            {
                std::string field;

                std::cin >> field;

                tokens.push_back(decode(field));
            }

            lexer.emplace(build(tokens));

            continue;
        }

        if (command == "Q")
        {
            std::string field;

            std::size_t from{0};

            std::cin >> field >> from;

            const auto input{decode(field)};

            const auto found{lexer->next_certified_evidence(input, from)};

            if (found)
            {
                std::cout << "A " << found->start << ' ' << found->evidence_begin << ' ' << found->evidence_end << ' '
                          << (found->window ? 1 : 0) << '\n';
            }
            else
            {
                std::cout << "R\n";
            }

            continue;
        }

        if (command == "T")
        {
            std::string field;

            std::cin >> field;

            const auto input{decode(field)};

            std::vector<std::size_t> lengths;

            const auto record{[&lengths](const std::size_t, const std::size_t length) { lengths.push_back(length); }};

            const auto committed{lexer->tokenize_all<std::size_t>(input, record)};

            std::cout << "T " << committed << ' ' << lengths.size();

            for (const auto length : lengths)
            {
                std::cout << ' ' << length;
            }

            std::cout << '\n';

            continue;
        }

        std::cerr << "crosscheck_scan: unknown command " << command << '\n';

        return 1;
    }

    std::cout.flush();

    return 0;
}
