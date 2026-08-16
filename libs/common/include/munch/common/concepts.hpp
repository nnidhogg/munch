#ifndef MUNCH_LIBS_COMMON_INCLUDE_MUNCH_COMMON_CONCEPTS_HPP
#define MUNCH_LIBS_COMMON_INCLUDE_MUNCH_COMMON_CONCEPTS_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <type_traits>

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

/**
 * @brief Concept that checks if a dereferenced element reads as a byte.
 *
 * Integral elements qualify, wider ones under the scanners' documented modulo-256 reading, and so does std::byte.
 * Floating-point elements do not: converting an unrepresentable floating value to an integral type is undefined,
 * so such inputs are rejected at overload resolution rather than deep inside a scan.
 * @tparam T The dereferenced element type to check.
 */
template <typename T>
concept Byte = std::integral<std::remove_cvref_t<T>> || std::same_as<std::remove_cvref_t<T>, std::byte>;

/**
 * @brief Concept that checks if a type is an input iterator over bytes.
 * @tparam T The type to check.
 */
template <typename T>
concept Byte_iterator = Iterator<T> && Byte<std::iter_reference_t<T>>;

/**
 * @brief Concept that checks if a type is a random-access iterator over bytes.
 * @tparam T The type to check.
 */
template <typename T>
concept Random_access_byte_iterator = std::random_access_iterator<T> && Byte<std::iter_reference_t<T>>;

/**
 * @brief Concept that checks if a type is a range iterating as bytes.
 *
 * The element is checked against const T, for the reason Iterable states: a range that yields bytes only from
 * mutable iteration would satisfy a looser concept and still fail inside the body.
 * @tparam T The type to check.
 */
template <typename T>
concept Byte_iterable = Iterable<T> && Byte<std::ranges::range_reference_t<const T>>;

/**
 * @brief Concept that checks if a type is a random-access range iterating as bytes.
 * @tparam T The type to check.
 */
template <typename T>
concept Random_access_byte_iterable = Random_access_iterable<T> && Byte<std::ranges::range_reference_t<const T>>;

/**
 * @brief Concept that checks if a type can name a token: an enumeration or an integral type.
 *
 * Every tokenizing entry point takes its token type through this concept, in place of a requires clause repeated
 * at each declaration.
 * @tparam T The type to check.
 */
template <typename T>
concept Token_id = std::integral<T> || std::is_enum_v<T>;

/**
 * @brief Concept for a sink receiving matched tokens.
 *
 * A sink is invocable with the token and its length, or with the token, its length, and the payload attached at
 * build time; the scanners deliver the payload and drop it for sinks of the shorter shape.
 * @tparam Sink The callable to check.
 * @tparam T The token type the sink receives.
 */
template <typename Sink, typename T>
concept Token_sink = std::invocable<Sink&, T, std::size_t> || std::invocable<Sink&, T, std::size_t, std::uint64_t>;

} // namespace munch::common::concepts

#endif // MUNCH_LIBS_COMMON_INCLUDE_MUNCH_COMMON_CONCEPTS_HPP
