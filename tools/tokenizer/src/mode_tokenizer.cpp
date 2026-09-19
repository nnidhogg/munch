#include "munch/tools/tokenizer/mode_tokenizer.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace munch::tools::tokenizer
{
Mode_tokenizer::Mode_tokenizer(std::vector<core::Lexer> lexers)
    : Mode_tokenizer{core::Mode_lexer{std::move(lexers)}, std::string{}, true}
{}

Mode_tokenizer::Mode_tokenizer(std::vector<core::Lexer> lexers, std::string input)
    : Mode_tokenizer{core::Mode_lexer{std::move(lexers)}, std::move(input), true}
{}

Mode_tokenizer::Mode_tokenizer(core::Mode_lexer lexer) : Mode_tokenizer{std::move(lexer), std::string{}, false}
{}

Mode_tokenizer::Mode_tokenizer(core::Mode_lexer lexer, std::string input)
    : Mode_tokenizer{std::move(lexer), std::move(input), false}
{}

Mode_tokenizer::Mode_tokenizer(core::Mode_lexer lexer, std::string input, const bool caller_driven)
    : input_{std::move(input)}, offset_{0}, lexer_{std::move(lexer)}, caller_driven_{caller_driven}
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

    // Under a mode lexer the stack describes the text just rewound past, so it is rewound to mode 0 whatever set
    // it, set_mode() included. Under caller-supplied lexers the current mode is the caller's and survives.
    if (!caller_driven_)
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
    // The search starts past the current position: after an error that position is the failure offset, the scan's
    // final committed offset where the failed token attempt began, and recovering to where the scan already stands
    // would not be a recovery.
    const auto before{offset_};

    const auto found{recover_from_failure()};

    return found ? std::optional{found->start - before} : std::nullopt;
}

std::optional<core::Lexer::Certified_start> Mode_tokenizer::recover_from_failure()
{
    return recover_from_clean(0);
}

std::optional<core::Lexer::Certified_start> Mode_tokenizer::recover_from_clean(const std::size_t clean_from)
{
    const auto& lexer{lexer_.mode(stack_.current)};

    const auto found{lexer.next_certified_evidence(input_, std::max(clean_from, offset_ + 1))};

    if (!found)
    {
        return std::nullopt;
    }

    offset_ = found->start;

    return found;
}

} // namespace munch::tools::tokenizer
