#include "munch/tools/audit/read_antlr.hpp"

#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/antlr_grammar.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
std::vector<Lexer_spec> read_antlr(const std::string_view source)
{
    auto spec{Grammar_reader{source}.read()};

    // A grammar whose lexer rules are all fragments, or a parser grammar, emits no token, so it declares no scanner.
    if (spec.rules.empty())
    {
        return {};
    }

    return {std::move(spec)};
}

} // namespace munch::tools::audit
