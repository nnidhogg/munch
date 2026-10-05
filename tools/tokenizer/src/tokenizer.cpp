#include "munch/tools/tokenizer/tokenizer.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace munch::tools::tokenizer
{
Tokenizer::Tokenizer(core::Lexer lexer) : lexer_{std::move(lexer)}
{}

Tokenizer::Tokenizer(core::Lexer lexer, std::string input) : input_{std::move(input)}, lexer_{std::move(lexer)}
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
    const auto before{offset_};

    const auto found{recover_from_failure()};

    if (!found)
    {
        return std::nullopt;
    }

    const auto& [start, evidence_begin, evidence_end, window]{*found};

    return start - before;
}

std::optional<core::Lexer::Certified_start> Tokenizer::recover_from_failure()
{
    return recover_from_clean(0);
}

std::optional<core::Lexer::Certified_start> Tokenizer::recover_from_clean(const std::size_t clean_from)
{
    const auto from{std::max(clean_from, offset_ + 1)};

    const auto found{lexer_.next_certified_evidence(input_, from)};

    if (!found)
    {
        return std::nullopt;
    }

    const auto& [start, evidence_begin, evidence_end, window]{*found};

    offset_ = start;

    return found;
}

Error Tokenizer::unrecognized() const
{
    return Error{std::format("Unrecognized character at position {}", offset_), offset_};
}

Error Tokenizer::zero_width() const
{
    return Error{std::format("Zero-width match at position {}", offset_), offset_};
}

} // namespace munch::tools::tokenizer
