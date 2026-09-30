#include "munch/tools/probes/study_rows.hpp"

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements study_rows.hpp: the conventional token set over either C-like base is private to this unit.

/**
 * @brief Adds the C-like base with string literals and line comments.
 * @param builder The builder the tokens are added to.
 * @param split_friendly Whether the base is the split-friendly one, newline its own token.
 */
void add_conventional(core::Builder& builder, const bool split_friendly)
{
    using figures::Token;

    figures::c_like(builder, split_friendly);
    builder.add_token(figures::string_literal(), Token::String, 2);
    builder.add_token(figures::line_comment(), Token::LineComment, 1);
}
} // namespace

void consumption_complete_c_row(core::Builder& builder)
{
    using namespace munch::regex;
    using figures::Token;

    builder.add_token(concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Token::Identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token::Number, 2);
    builder.add_token(any_of(figures::operators()), Token::Operator, 2);
    builder.add_token(any_of(figures::punctuation() + '#' + '\\' + '@' + '`' + '$' + '\''), Token::Punctuation, 2);
    builder.add_token(plus(any_of(Set{' ', '\t', '\n', '\r'})), Token::Whitespace, 2);

    const auto escape{concat(text("\\"), any_of(Set::all()))};

    builder.add_token(
            concat(text("\""), kleene(choice(any_of(Set::all() - Set{'"', '\\', '\n'}), escape)), text("\"")),
            Token::String, 1);

    builder.add_token(
            concat(text("'"), plus(choice(any_of(Set::all() - Set{'\'', '\\', '\n'}), escape)), text("'")),
            Token::Literal, 1);

    builder.add_token(figures::line_comment(), Token::LineComment, 1);
    builder.add_token(figures::block_comment(), Token::BlockComment, 1);
}

void published_cumulative_row(core::Builder& builder)
{
    using figures::Token;

    figures::c_like(builder, false);
    builder.add_token(figures::string_literal(), Token::String, 2);
    builder.add_token(figures::line_comment(), Token::LineComment, 1);
    builder.add_token(figures::block_comment(), Token::BlockComment, 1);
}

void conventional_row(core::Builder& builder)
{
    add_conventional(builder, false);
}

void split_friendly_conventional_row(core::Builder& builder)
{
    add_conventional(builder, true);
}

} // namespace munch::tools::probes
