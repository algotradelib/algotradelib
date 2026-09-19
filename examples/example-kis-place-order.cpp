// Places one real KIS order. This example transmits a live instruction and is
// therefore inert unless it is explicitly confirmed:
//
//   ATL_CONFIRM_LIVE_ORDER=1
//     required before any order is sent, in every environment;
//   ATL_ORDER_SYMBOL, ATL_ORDER_QUANTITY, ATL_ORDER_PRICE
//     the order to place; a missing price means a market order;
//   ATL_ORDER_SIDE
//     BUY (default) or SELL.
//
// Building or running the example without the confirmation prints what it
// would do and exits, so an accidental run cannot reach the broker.
#include <algotradelib/kis>

#include <boost/asio/ssl/context.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ssl = boost::asio::ssl;
namespace atl = algotradelib;

namespace {

void print_error(std::string_view operation, const atl::BrokerError& error) {
  std::cerr << operation << " failed";
  if (!error.broker_code_.empty()) {
    std::cerr << " [" << error.broker_code_ << ']';
  }
  if (error.http_status_ != 0) {
    std::cerr << " HTTP " << error.http_status_;
  }
  if (!error.message_.empty()) {
    std::cerr << ": " << error.message_;
  }
  std::cerr << '\n';
  if (error.category_ == atl::ErrorCategory::TRANSPORT) {
    std::cerr
      << "  the order may or may not have reached KIS. Reconcile through the\n"
         "  modifiable-order or order-history inquiry before resubmitting; KIS\n"
         "  offers no idempotency key for individual customers.\n";
  }
}

std::optional<std::string> environment_value(const char* name) {
  const char* value = std::getenv(name);
  if (!value || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

std::string required_environment_value(const char* name) {
  auto value = environment_value(name);
  if (!value) {
    throw std::runtime_error(std::string(name) + " is not set");
  }
  return *value;
}

} // namespace

int main() {
  try {
    atl::OrderRequest order;
    order.instrument_ = atl::InstrumentId{
      atl::Country::KR, atl::Venue::COMBINED, required_environment_value("ATL_ORDER_SYMBOL")
    };
    order.quantity_ = atl::Decimal64::parse(required_environment_value("ATL_ORDER_QUANTITY"));
    order.side_ = environment_value("ATL_ORDER_SIDE") == std::optional<std::string>("SELL") ?
      atl::OrderSide::SELL :
      atl::OrderSide::BUY;
    if (const auto price = environment_value("ATL_ORDER_PRICE")) {
      order.type_ = atl::OrderType::LIMIT;
      order.price_ = atl::Decimal64::parse(*price);
    } else {
      order.type_ = atl::OrderType::MARKET;
    }

    atl::kis::KisConfig config;
    config.from_env();

    atl::kis::KisAccount account;
    account.from_env();

    std::cout << "environment: " << (config.is_demo_ ? "demo" : "live") << '\n';
    std::cout << (order.side_ == atl::OrderSide::BUY ? "BUY " : "SELL ")
      << order.quantity_.str() << ' ' << order.instrument_.symbol_;
    if (order.price_) {
      std::cout << " @ " << order.price_->str();
    } else {
      std::cout << " at market";
    }
    std::cout << '\n';

    if (environment_value("ATL_CONFIRM_LIVE_ORDER") != std::optional<std::string>("1")) {
      std::cout << "ATL_CONFIRM_LIVE_ORDER is not 1; no order was sent.\n";
      return 0;
    }

    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    atl::kis::KisBlockingClient client(tls, std::move(config));
    auto result = client.place_order(std::move(account), std::move(order));
    if (!result) {
      print_error("order placement", result.error());
      return 1;
    }

    std::cout << "accepted order " << result.value().order_.order_number_ << " at branch "
      << result.value().order_.branch_number_ << " (broker time "
      << result.value().broker_order_time_ << ")\n";
    std::cout << "acceptance is not execution; read the order back through an inquiry.\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
