#include "munch/tools/tokenizer/tokenizer.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace munch::tools::tokenizer
{
Tokenizer::Tokenizer(core::Lexer lexer) : mode_{0}, offset_{0}
{
    lexers_.push_back(std::move(lexer));
}

Tokenizer::Tokenizer(core::Lexer lexer, std::string input) : mode_{0}, input_{std::move(input)}, offset_{0}
{
    lexers_.push_back(std::move(lexer));
}

Tokenizer::Tokenizer(std::vector<core::Lexer> lexers) : mode_{0}, offset_{0}, lexers_{std::move(lexers)}
{
    if (lexers_.empty())
    {
        throw std::invalid_argument("A tokenizer needs at least one lexer");
    }
}

Tokenizer::Tokenizer(std::vector<core::Lexer> lexers, std::string input) : Tokenizer{std::move(lexers)}
{
    input_ = std::move(input);
}

Tokenizer::Tokenizer(core::Mode_lexer lexer) : mode_{0}, offset_{0}, automatic_{std::move(lexer)}
{}

Tokenizer::Tokenizer(core::Mode_lexer lexer, std::string input)
    : mode_{0}, input_{std::move(input)}, offset_{0}, automatic_{std::move(lexer)}
{}

void Tokenizer::load(std::string input)
{
    input_ = std::move(input);

    offset_ = 0;

    stack_.saved.clear();
}

void Tokenizer::reset() noexcept
{
    offset_ = 0;
}

void Tokenizer::seek(const std::size_t offset) noexcept
{
    offset_ = std::min(offset, input_.size());
}

std::size_t Tokenizer::mode() const noexcept
{
    return mode_;
}

std::size_t Tokenizer::depth() const noexcept
{
    return stack_.saved.size();
}

std::size_t Tokenizer::offset() const noexcept
{
    return offset_;
}

std::string_view Tokenizer::input() const noexcept
{
    return input_;
}

} // namespace munch::tools::tokenizer
