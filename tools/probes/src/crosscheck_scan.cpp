// A probe over munch's public certificate walk, for an independent exhaustive checker.
//
// Its main reads the protocol's commands and hands each to the Crosscheck_session of munch_probes, which builds a Lexer
// from a literal token list and reports, for each query, exactly what Lexer::next_certified_evidence() answers, plus
// what Lexer::tokenize_all() commits. It shares no machinery with the campaign harness under tools/probes. Nothing is
// asserted here; the driver owns the verdicts and compares against its own reference model.
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

#include <cstdlib>
#include <iostream>

#include "munch/tools/probes/crosscheck_session.hpp"

/**
 * @brief Reads the protocol's commands from standard input and answers each through the session, refusing a query
 *        before any SET on standard error.
 * @return EXIT_SUCCESS, or EXIT_FAILURE after a refused command.
 */
int main()
{
    using munch::tools::probes::Crosscheck_session;

    std::ios::sync_with_stdio(false);

    Crosscheck_session session{};

    const auto answered{
            session.run(std::cin, std::cout, std::cerr, "crosscheck_scan", Crosscheck_session::Protocol::scan)};

    return answered ? EXIT_SUCCESS : EXIT_FAILURE;
}
