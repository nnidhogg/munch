#include "munch/tools/probes/study_rows.hpp"

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace munch::tools::probes
{
void consumption_complete_c_row(core::Builder& builder)
{
    using namespace munch::regex;
    using figures::Token;

    const auto identifier{figures::identifier()};

    const auto punctuation{figures::punctuation() + '#' + '\\' + '@' + '`' + '$' + '\''};

    const auto whitespace{plus(any_of(Set{' ', '\t', '\n', '\r'}))};

    builder.add_token(identifier, Token::identifier, 2);

    builder.add_token(plus(any_of(Set::digits())), Token::number, 2);

    builder.add_token(any_of(figures::operators()), Token::operator_, 2);

    builder.add_token(any_of(punctuation), Token::punctuation, 2);

    builder.add_token(whitespace, Token::whitespace, 2);

    const auto escape{concat(text(R"(\)"), any_of(Set::all()))};

    const auto string_character{choice(any_of(Set::all() - Set{'"', '\\', '\n'}), escape)};

    const auto string{concat(text(R"(")"), kleene(string_character), text(R"(")"))};

    const auto literal_character{choice(any_of(Set::all() - Set{'\'', '\\', '\n'}), escape)};

    const auto literal{concat(text("'"), plus(literal_character), text("'"))};

    builder.add_token(string, Token::string, 1);

    builder.add_token(literal, Token::literal, 1);

    builder.add_token(figures::line_comment(), Token::line_comment, 1);

    builder.add_token(figures::block_comment(), Token::block_comment, 1);
}

void published_cumulative_row(core::Builder& builder)
{
    figures::c_like_with_block_comments(builder, false);
}

void conventional_row(core::Builder& builder)
{
    figures::c_like_with_comments(builder, false);
}

void split_friendly_conventional_row(core::Builder& builder)
{
    figures::c_like_with_comments(builder, true);
}

} // namespace munch::tools::probes
