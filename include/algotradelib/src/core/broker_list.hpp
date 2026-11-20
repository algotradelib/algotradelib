#pragma once

#include "./broker_traits.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>

namespace algotradelib {

/** @brief A type carried as a value, so a C++17 visitor can be called with a type. */
template <typename T>
struct type_tag {
  /** @brief The type. */
  using type = T;
};

/** @brief A compile-time list of broker client types, for `with_broker`. */
template <typename... Clients>
struct type_list {
  /** @brief How many clients the list holds. */
  static constexpr std::size_t size = sizeof...(Clients);
};

namespace port_detail {

template <typename List>
struct is_type_list : std::false_type {};

template <typename First, typename... Rest>
struct is_type_list<type_list<First, Rest...>> : std::true_type {};

// Whether no two clients share `BrokerTraits<Client>::name`.
template <typename... Clients>
constexpr bool distinct_names() {
  const std::string_view names[] = {BrokerTraits<Clients>::name...};
  for (std::size_t i = 0; i < sizeof...(Clients); ++i) {
    for (std::size_t j = i + 1; j < sizeof...(Clients); ++j) {
      if (names[i] == names[j]) {
        return false;
      }
    }
  }
  return true;
}

template <typename Visit, typename First, typename... Rest>
auto visit_named(type_list<First, Rest...>, std::string_view name, Visit& visit) {
  using R = std::invoke_result_t<Visit&, type_tag<First>>;
  static_assert(
    (std::is_same_v<R, std::invoke_result_t<Visit&, type_tag<Rest>>> && ...),
    "with_broker: visit must return the same type for every client in the list"
  );
  static_assert(
    !std::is_void_v<R> && !std::is_reference_v<R>,
    "with_broker: visit must return a value (not void, not a reference); the result is a std::optional"
  );
  static_assert(
    distinct_names<First, Rest...>(),
    "with_broker: the clients in a type_list must have distinct names (BrokerTraits<Client>::name); "
    "a broker's asynchronous and blocking clients share one"
  );
  std::optional<R> result;
  const auto matches = [&](auto tag) {
    if (name != BrokerTraits<typename decltype(tag)::type>::name) {
      return false;
    }
    result.emplace(visit(tag));
    return true;
  };
  static_cast<void>((matches(type_tag<First>{}) || ... || matches(type_tag<Rest>{})));
  return result;
}

} // namespace port_detail

/**
 * @brief Calls `visit` with the `type_tag` of the client in `List` whose `BrokerTraits<Client>::name` is `name`.
 *
 * Every listed client's `visit` is compiled where this is called. To keep each broker in its own translation
 * unit, let the visitor call a function template declared `extern template` there and explicitly instantiated
 * in that broker's source.
 *
 * @tparam List A non-empty `type_list` of clients with distinct names (checked at compile time).
 * @param name The broker's name, compared exactly (case and all).
 * @param visit Called as `visit(type_tag<Client>{})`; every client must give the same type `R`, a value.
 * @return What `visit` returned, as a `std::optional<R>`, or nothing if no listed client has that name (then
 *   `visit` is not called).
 */
template <typename List, typename Visit>
auto with_broker(std::string_view name, Visit&& visit) {
  static_assert(port_detail::is_type_list<List>::value, "with_broker: List must be a non-empty type_list");
  return port_detail::visit_named(List{}, name, visit);
}

} // namespace algotradelib
