#pragma once

#include "./calendar_cache.hpp"
#include "./http_client.hpp"
#include "./token_store.hpp"

#include <memory>
#include <type_traits>
#include <utility>

namespace algotradelib {
namespace network {

/** @brief What the clients of a run share, across processes too; each takes those its constructor accepts. */
struct ClientStores {
  /** @brief Where clients share access tokens, or null for each to keep its own (Toss needs it shared). */
  std::shared_ptr<TokenStore> tokens_;
  /** @brief Where clients share a trading calendar, or null; only KIS's client takes one, to save requests. */
  std::shared_ptr<CalendarCache> calendars_;
};

/**
 * @brief Constructs a blocking client with the run's stores; sends nothing.
 *
 * The client gets the token store always, and the calendar cache when its constructor takes one as a fifth
 * argument. A client of another shape, or whose constructor is a variadic forwarding template (it counts as
 * taking five arguments and then fails in its own body), is constructed by hand. The stores are passed as const
 * lvalues, so a constructor taking `std::shared_ptr<TokenStore>&&` fails inside `std::make_shared`.
 *
 * @tparam Client A client constructible as `Client(ssl::context&, Config, HttpClientOptions,
 *   std::shared_ptr<TokenStore>)`, optionally with a trailing `std::shared_ptr<CalendarCache>`.
 * @tparam Config The client's configuration type, `BrokerTraits<Client>::Config`.
 * @param context The TLS context; borrowed, and must outlive the client.
 * @param config The credentials and environment.
 * @param options HTTP options, pacing included; nothing is added (unset pacing is unpaced).
 * @param stores The run's stores.
 * @return The client.
 */
template <typename Client, typename Config>
std::shared_ptr<Client> make_client(
  ssl::context& context, Config config, HttpClientOptions options, const ClientStores& stores
) {
  constexpr bool takes4 =
    std::is_constructible_v<Client, ssl::context&, Config, HttpClientOptions, std::shared_ptr<TokenStore>>;
  constexpr bool takes5 = std::is_constructible_v<
    Client, ssl::context&, Config, HttpClientOptions, std::shared_ptr<TokenStore>, std::shared_ptr<CalendarCache>
  >;
  static_assert(
    takes4 || takes5,
    "Client is not constructible from (ssl::context&, Config, HttpClientOptions, shared_ptr<TokenStore>[, "
    "shared_ptr<CalendarCache>]): check that Config is BrokerTraits<Client>::Config, or construct it by hand"
  );
  if constexpr (takes5) {
    return std::make_shared<Client>(context, std::move(config), std::move(options), stores.tokens_, stores.calendars_);
  } else if constexpr (takes4) {
    return std::make_shared<Client>(context, std::move(config), std::move(options), stores.tokens_);
  }
}

} // namespace network
} // namespace algotradelib
