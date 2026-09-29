#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_CHECKS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_CHECKS_HPP

#include <string_view>

#include "munch/tools/audit/antlr_tables.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

/**
 * @brief What ANTLR holds a grammar to once every rule is read, finish_grammar(): its errors 153, 126, 51, 173, 172,
 *        170, 161, 162, 171, 175, 145 and 176 and the non-greedy loops' refusals that wait on the whole grammar, and
 *        the implicit tokens a combined grammar places ahead of its rules, with whether a mode is declared,
 *        declares_mode().
 */
namespace munch::tools::audit
{
/**
 * @brief Holds a read grammar to what ANTLR holds the whole of it to and places its implicit tokens, in ANTLR's order:
 *        the closures and loops that can match the empty string, the loops another rule references, the parser's
 *        literals as implicit tokens, the names reserved or taken twice, the `type` commands' names, and the modes.
 * @param tables What the reading recorded about the grammar.
 * @param case_insensitive Whether the grammar's `caseInsensitive` option is set, which folds an implicit token.
 * @param spec The scanner read, its implicit tokens placed before every rule on return and its rules' tokens resolved.
 * @throws Spec_error At the first check the grammar fails, in ANTLR's words.
 */
void finish_grammar(const Grammar_tables& tables, bool case_insensitive, Lexer_spec& spec);

/**
 * @brief Whether a `mode` line of the grammar names a mode, DEFAULT_MODE's reopening among them.
 * @param tables What the reading recorded about the grammar, its sections so far.
 * @param name The name.
 * @return True when one does.
 */
[[nodiscard]] bool declares_mode(const Grammar_tables& tables, std::string_view name);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_CHECKS_HPP
