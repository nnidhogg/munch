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
 *
 * Checked against const T with matching begin and end types: the container overloads take a const reference and
 * forward a begin/end pair to a function templated on one iterator type, so a range that only iterates mutably, or
 * whose sentinel is not its iterator, would satisfy a looser concept and still fail inside the body.
 * @tparam T The type to check.
 */
template <typename T>
concept Iterable = std::ranges::common_range<const T>;

/**
 * @brief Concept that checks if a type is a range whose iterators offer random access.
 *
 * Required wherever an algorithm indexes a container or takes iterator differences, rather than merely walking it.
 * Checked against const T with matching begin and end types, for the reason Iterable states.
 * @tparam T The type to check.
 */
template <typename T>
concept Random_access_iterable = std::ranges::common_range<const T> && std::ranges::random_access_range<const T>;

} // namespace munch::common::concepts

#endif // MUNCH_LIBS_COMMON_INCLUDE_MUNCH_COMMON_CONCEPTS_HPP
