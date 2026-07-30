#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_INDIRECT_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_INDIRECT_HPP

#include <memory>
#include <utility>

namespace munch::regex
{
/**
 * @brief A single heap-held value with value semantics, for nodes that contain the sum type they are part of.
 *
 * Regex is incomplete inside its own node types, so a node cannot hold a child by value; Indirect holds it through
 * one owning pointer while copying like a value: unlike std::unique_ptr, copying an Indirect copies the child.
 * There is no empty state to represent or check for; a moved-from Indirect may only be assigned to or destroyed.
 * This is the shape of C++26's std::indirect, and it is meant to be replaced by the standard type once the
 * toolchain baseline provides it.
 */
template <typename T>
class Indirect
{
public:
    /**
     * @brief Boxes a value.
     */
    explicit Indirect(T value) : value_{std::make_unique<T>(std::move(value))} {}

    Indirect(const Indirect& other) : value_{std::make_unique<T>(*other.value_)} {}

    Indirect(Indirect&& other) noexcept = default;

    Indirect& operator=(const Indirect& other)
    {
        value_ = std::make_unique<T>(*other.value_);

        return *this;
    }

    Indirect& operator=(Indirect&& other) noexcept = default;

    ~Indirect() = default;

    /**
     * @brief The boxed value.
     */
    [[nodiscard]] const T& operator*() const noexcept { return *value_; }

    /**
     * @brief The boxed value.
     */
    [[nodiscard]] T& operator*() noexcept { return *value_; }

private:
    std::unique_ptr<T> value_;
};

} // namespace munch::regex

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_INDIRECT_HPP
