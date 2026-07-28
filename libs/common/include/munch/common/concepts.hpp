#ifndef MUNCH_LIBS_COMMON_INCLUDE_MUNCH_COMMON_CONCEPTS_HPP
#define MUNCH_LIBS_COMMON_INCLUDE_MUNCH_COMMON_CONCEPTS_HPP

#include <concepts>
#include <ranges>

namespace munch::common::concepts
{
/**
 * @brief Concept that checks if a type is an input iterator.
 * @tparam T The type to check.
 */
template <typename T>
concept Iterator = std::input_iterator<T>;

/**
 * @brief Concept that checks if a type is a range (iterable container).
 * @tparam T The type to check.
 */
template <typename T>
concept Iterable = std::ranges::range<T>;

} // namespace munch::common::concepts

#endif // MUNCH_LIBS_COMMON_INCLUDE_MUNCH_COMMON_CONCEPTS_HPP
