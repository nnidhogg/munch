#include "munch/tools/tokenizer/tokenizer.hpp"

#include <algorithm>
#include <utility>

namespace munch::tools::tokenizer
{
Tokenizer::Tokenizer(core::Lexer lexer) : offset_{0}, lexer_{std::move(lexer)}
{}

Tokenizer::Tokenizer(core::Lexer lexer, std::string input)
    : input_{std::move(input)}, offset_{0}, lexer_{std::move(lexer)}
{}

std::string_view Tokenizer::input() const noexcept
{
    return input_;
}

std::size_t Tokenizer::offset() const noexcept
{
    return offset_;
}

const core::Lexer& Tokenizer::lexer() const noexcept
{
    return lexer_;
}

void Tokenizer::load(std::string input)
{
    input_ = std::move(input);

    offset_ = 0;
}

void Tokenizer::reset() noexcept
{
    offset_ = 0;
}

void Tokenizer::seek(const std::size_t offset) noexcept
{
    offset_ = std::min(offset, input_.size());
}

std::optional<std::size_t> Tokenizer::recover()
{
    // The search starts past the current position: after an error that position is the failure offset, the scan's
    // final committed offset where the failed token attempt began, and recovering to where the scan already stands
    // would not be a recovery.
    const auto before{offset_};

    const auto found{recover_from_failure()};

    return found ? std::optional{found->start - before} : std::nullopt;
}

std::optional<core::Lexer::Certified_start> Tokenizer::recover_from_failure()
{
    return recover_from_clean(0);
}

std::optional<core::Lexer::Certified_start> Tokenizer::recover_from_clean(const std::size_t clean_from)
{
    const auto found{lexer_.next_certified_evidence(input_, std::max(clean_from, offset_ + 1))};

    if (!found)
    {
        return std::nullopt;
    }

    offset_ = found->start;

    return found;
}

} // namespace munch::tools::tokenizer
