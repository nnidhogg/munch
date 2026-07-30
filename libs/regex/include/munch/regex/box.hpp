#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_BOX_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_BOX_HPP

#include <memory>
#include <utility>

namespace munch::regex
{
/**
 * @brief A single heap-held value with value semantics, for nodes that contain the sum type they are part of.
 *
 * Regex is incomplete inside its own node types, so a node cannot hold a child by value; Box holds it through one
 * owning pointer while copying like a value: unlike std::unique_ptr, copying a Box copies the child. There is no
 * empty state to represent or check for; a moved-from Box may only be assigned to or destroyed.
 */
template <typename T>
class Box
{
public:
    /**
     * @brief Boxes a value.
     */
    explicit Box(T value) : value_{std::make_unique<T>(std::move(value))} {}

    Box(const Box& other) : value_{std::make_unique<T>(*other.value_)} {}

    Box(Box&& other) noexcept = default;

    Box& operator=(const Box& other)
    {
        value_ = std::make_unique<T>(*other.value_);

        return *this;
    }

    Box& operator=(Box&& other) noexcept = default;

    ~Box() = default;

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

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_BOX_HPP
