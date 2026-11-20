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

template <typename Visit, typename First, typename... Clients>
auto visit_named(type_list<First, Clients...>, std::string_view name, Visit& visit) {
  using R = std::invoke_result_t<Visit&, type_tag<First>>;
  static_assert(
    (std::is_same_v<R, std::invoke_result_t<Visit&, type_tag<Clients>>> && ...),
    "with_broker: visit must return the same type for every client"
  );
  static_assert(
    !std::is_void_v<R> && !std::is_reference_v<R>, "with_broker: visit must return a value, not void or a reference"
  );
  static_assert(
    distinct_names<First, Clients...>(),
    "with_broker: listed clients need distinct names; a broker's asynchronous and blocking clients share one"
  );
  std::optional<R> result;
  const auto match = [&](auto tag) {
    return name == BrokerTraits<typename decltype(tag)::type>::name &&
      (static_cast<void>(result.emplace(visit(tag))), true);
  };
  static_cast<void>((match(type_tag<First>{}) || ... || match(type_tag<Clients>{})));
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
 * @return What `visit` returned, or nothing (and `visit` is not called) if no listed client has that name.
 */
template <typename List, typename Visit>
auto with_broker(std::string_view name, Visit&& visit) {
  static_assert(port_detail::is_type_list<List>::value, "with_broker: List must be a non-empty type_list");
  return port_detail::visit_named(List{}, name, visit);
}

} // namespace algotradelib
