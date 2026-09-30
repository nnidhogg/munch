// A probe over munch's public certificate walk and its anchored comparator machinery.
//
// Its main reads the protocol's commands and hands each to the Crosscheck_session of munch_probes, which builds the
// Lexer from a literal token list and answers the query. Its SET, Q, T and END commands answer byte for byte what
// crosscheck_scan.cpp answers, so crosscheck_scan.py runs against either binary, and two more commands answer
// Lexer::next_anchored_start() and Lexer::minimal_repair(). Nothing is asserted here; crosscheck_anchor.py owns every
// verdict and compares against its own reference model.
//
// Protocol, line oriented on stdin, one field per whitespace-separated word:
//
//   SET <count> <token> ...   build a lexer over the listed literal tokens, priority = list order
//   Q <input> <from>          print the certificate walk's answer for (input, from)
//   T <input>                 print what the maximal-munch scan commits on input
//   N <tail> <from>           print next_anchored_start(tail, from)
//   M <tail>                  print minimal_repair(tail)
//   END                       stop
//
// A string field of "-" denotes the empty string; the driver's alphabets never contain '-'.
//
// Output, one line per Q, T, N or M query, in query order:
//
//   A <start> <evidence_begin> <evidence_end> <window>   the walk answered
//   R                                                    the walk refused
//   T <consumed> <token_count> <length> ...              the scan's committed lengths
//   N <position>                                         the anchored decider answered
//   NR                                                   the anchored decider refused
//   M <repair>                                           a minimal repair, "-" when it is empty
//   MR                                                   no repair exists, or the set is nullable

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
            session.run(std::cin, std::cout, std::cerr, "crosscheck_anchor", Crosscheck_session::Protocol::anchor)};

    return answered ? EXIT_SUCCESS : EXIT_FAILURE;
}
