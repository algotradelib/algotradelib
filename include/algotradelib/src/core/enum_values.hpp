#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>

namespace algotradelib {

/**
 * @file
 * @brief A compile-time check that a list names every value of an enum, and a parser over the list.
 *
 * The library keeps a list of every value next to several enums (`all_venues`, `all_halt_reasons`, ...)
 * and derives counts, parsers and tests from it. Each list is checked with `lists_every_value` in a
 * `static_assert`, so a value added to the enum and left out of its list fails to compile.
 * `value_named` reads a name back through the same list.
 */

/// \cond ATL_ENUM_VALUES_DETAIL
namespace enum_values_detail {

template <typename Enum, typename IsEnumerator>
constexpr bool lists_every_value(const Enum* values, std::size_t count, IsEnumerator is_enumerator) noexcept {
  using Underlying = std::underlying_type_t<Enum>;
  for (std::size_t index = 0; index < count; ++index) {
    const auto value = static_cast<Underlying>(values[index]);
    if constexpr (std::is_signed<Underlying>::value) {
      if (value < 0) {
        return false;
      }
    }
    if (static_cast<std::size_t>(value) >= count || !is_enumerator(values[index])) {
      return false;
    }
    for (std::size_t other = 0; other < index; ++other) {
      if (values[other] == values[index]) {
        return false;
      }
    }
  }
  return !is_enumerator(static_cast<Enum>(count));
}

} // namespace enum_values_detail
/// \endcond

/**
 * @brief Tests whether a list holds every value of an enum exactly once, in any order.
 *
 * The enum's values must be `0` to `N - 1`: no enumerator may state its own value. Then `N` distinct
 * enumerators below `N` are all of them, and `N` itself must not be one, or the enum is longer than the
 * list.
 *
 * Use it in a `static_assert` beside the list. Pass a test that is `true` exactly for the enum's values,
 * such as the enum's name function, whose `switch` has a case for each value and returns `""` after it:
 *
 * @code
 * static_assert(lists_every_value(all_venues, [](Venue venue) { return !venue_name(venue).empty(); }));
 * @endcode
 *
 * @tparam Enum The enum.
 * @tparam N The list's length.
 * @tparam IsEnumerator A callable taking an `Enum` and returning `bool`.
 * @param values The list.
 * @param is_enumerator Whether a value is one of the enum's.
 * @return `true` if `values` lists each of the enum's values once.
 */
template <typename Enum, std::size_t N, typename IsEnumerator>
constexpr bool lists_every_value(const std::array<Enum, N>& values, IsEnumerator is_enumerator) noexcept {
  static_assert(std::is_enum<Enum>::value, "lists_every_value checks a list of an enum's values");
  return enum_values_detail::lists_every_value(values.data(), N, is_enumerator);
}

/**
 * @brief Tests whether a list holds every value of an enum exactly once, in any order.
 *
 * The same check for a built-in array, such as `runner::all_poll_kinds`.
 *
 * @tparam Enum The enum.
 * @tparam N The list's length.
 * @tparam IsEnumerator A callable taking an `Enum` and returning `bool`.
 * @param values The list.
 * @param is_enumerator Whether a value is one of the enum's.
 * @return `true` if `values` lists each of the enum's values once.
 */
template <typename Enum, std::size_t N, typename IsEnumerator>
constexpr bool lists_every_value(const Enum (&values)[N], IsEnumerator is_enumerator) noexcept {
  static_assert(std::is_enum<Enum>::value, "lists_every_value checks a list of an enum's values");
  return enum_values_detail::lists_every_value(values, N, is_enumerator);
}

/**
 * @brief Returns the value of a list whose name is `text`, so a parser reads exactly what its writer
 *   writes.
 *
 * @tparam Enum The enum.
 * @tparam N The list's length.
 * @tparam Name A callable taking an `Enum` and returning something comparable with `std::string_view`.
 * @param values Every value of the enum, such as `all_venues`.
 * @param name The name function the writer uses.
 * @param text The text to read.
 * @return The value named `text`, or `std::nullopt` if none is, and always for an empty `text` (what a
 *   name function returns for a value that is not an enumerator).
 */
template <typename Enum, std::size_t N, typename Name>
constexpr std::optional<Enum> value_named(
  const std::array<Enum, N>& values,
  Name name,
  std::string_view text
) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  for (const auto value : values) {
    if (name(value) == text) {
      return value;
    }
  }
  return std::nullopt;
}

} // namespace algotradelib
