#ifndef LEXER_LIBS_DFA_INCLUDE_LEXER_DFA_MINIMIZE_HPP
#define LEXER_LIBS_DFA_INCLUDE_LEXER_DFA_MINIMIZE_HPP

#include "lexer/dfa/dfa.hpp"

namespace lexer::dfa
{
/**
 * @brief Returns the minimal DFA equivalent to the given one.
 *
 * States are merged when no input distinguishes them, using Moore partition refinement: states start out grouped by
 * their accept token, and groups are split until every pair of states in a group agrees, symbol by symbol, on the
 * group of the state it moves to. States accepting different tokens are never merged, so the token reported for any
 * input is unchanged.
 * @param dfa The DFA to minimize.
 * @return An equivalent DFA with no two states interchangeable.
 */
[[nodiscard]] Dfa minimize(const Dfa& dfa);

} // namespace lexer::dfa

#endif // LEXER_LIBS_DFA_INCLUDE_LEXER_DFA_MINIMIZE_HPP
