#include "munch/tools/tokenizer/mode_tokenizer.hpp"

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

namespace munch::tools::tokenizer
{
Mode_tokenizer::Mode_tokenizer(std::vector<core::Lexer> lexers)
    : Mode_tokenizer{core::Mode_lexer{std::move(lexers)}, std::string{}, Driver::caller}
{}

Mode_tokenizer::Mode_tokenizer(std::vector<core::Lexer> lexers, std::string input)
    : Mode_tokenizer{core::Mode_lexer{std::move(lexers)}, std::move(input), Driver::caller}
{}

Mode_tokenizer::Mode_tokenizer(core::Mode_lexer lexer)
    : Mode_tokenizer{std::move(lexer), std::string{}, Driver::grammar}
{}

Mode_tokenizer::Mode_tokenizer(core::Mode_lexer lexer, std::string input)
    : Mode_tokenizer{std::move(lexer), std::move(input), Driver::grammar}
{}

Mode_tokenizer::Mode_tokenizer(core::Mode_lexer lexer, std::string input, const Driver driver)
    : input_{std::move(input)}, lexer_{std::move(lexer)}, driver_{driver}
{}

std::string_view Mode_tokenizer::input() const noexcept
{
    return input_;
}

std::size_t Mode_tokenizer::offset() const noexcept
{
    return offset_;
}

std::size_t Mode_tokenizer::mode() const noexcept
{
    return stack_.current;
}

std::size_t Mode_tokenizer::depth() const noexcept
{
    return stack_.saved.size();
}

void Mode_tokenizer::load(std::string input)
{
    input_ = std::move(input);

    reset();
}

void Mode_tokenizer::reset() noexcept
{
    offset_ = 0;

    if (driver_ == Driver::grammar)
    {
        stack_ = core::Mode_stack{};
    }
}

void Mode_tokenizer::seek(const std::size_t offset) noexcept
{
    offset_ = std::min(offset, input_.size());
}

std::optional<std::size_t> Mode_tokenizer::recover()
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

std::optional<core::Lexer::Certified_start> Mode_tokenizer::recover_from_failure()
{
    return recover_from_clean(0);
}

std::optional<core::Lexer::Certified_start> Mode_tokenizer::recover_from_clean(const std::size_t clean_from)
{
    const auto& lexer{lexer_.mode(stack_.current)};

    const auto from{std::max(clean_from, offset_ + 1)};

    const auto found{lexer.next_certified_evidence(input_, from)};

    if (!found)
    {
        return std::nullopt;
    }

    const auto& [start, evidence_begin, evidence_end, window]{*found};

    offset_ = start;

    return found;
}

Error Mode_tokenizer::unrecognized() const
{
    return Error{std::format("Unrecognized character at position {}", offset_), offset_};
}

Error Mode_tokenizer::zero_width() const
{
    return Error{std::format("Zero-width match at position {}", offset_), offset_};
}

} // namespace munch::tools::tokenizer
