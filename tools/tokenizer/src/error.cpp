#include "munch/tools/tokenizer/error.hpp"

namespace munch::tools::tokenizer
{
Error::Error(std::string message, const std::size_t position) : message_{std::move(message)}, position_{position}
{}

const std::string& Error::message() const noexcept
{
    return message_;
}

std::size_t Error::position() const noexcept
{
    return position_;
}

} // namespace munch::tools::tokenizer
