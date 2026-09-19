#include <algotradelib/kis>

#include <boost/asio/ssl/context.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iostream>
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
      << "  the outcome is indeterminate; reconcile through the order inquiries "
         "before resubmitting\n";
  }
}

std::string_view status_name(atl::OrderStatus status) {
  switch (status) {
    case atl::OrderStatus::PENDING:
      return "PENDING";
    case atl::OrderStatus::PENDING_CANCEL:
      return "PENDING_CANCEL";
    case atl::OrderStatus::PENDING_REPLACE:
      return "PENDING_REPLACE";
    case atl::OrderStatus::PARTIAL_FILLED:
      return "PARTIAL_FILLED";
    case atl::OrderStatus::FILLED:
      return "FILLED";
    case atl::OrderStatus::CANCELED:
      return "CANCELED";
    case atl::OrderStatus::REJECTED:
      return "REJECTED";
    case atl::OrderStatus::CANCEL_REJECTED:
      return "CANCEL_REJECTED";
    case atl::OrderStatus::REPLACE_REJECTED:
      return "REPLACE_REJECTED";
    case atl::OrderStatus::REPLACED:
      return "REPLACED";
    default:
      return "UNKNOWN";
  }
}

void print_order(const atl::OrderSnapshot& order) {
  std::cout << order.order_id_ << "  " << order.instrument_.symbol_ << "  " << order.name_ << '\n';
  std::cout << "  " << (order.side_ == atl::OrderSide::BUY ? "BUY" : "SELL") << "  "
    << status_name(order.status_) << "  quantity " << order.quantity_.str();
  if (order.price_) {
    std::cout << " @ " << order.price_->str();
  }
  std::cout << '\n';
  std::cout << "  filled: " << order.execution_.filled_quantity_.str();
  if (order.execution_.average_filled_price_) {
    std::cout << " @ average " << order.execution_.average_filled_price_->str();
  }
  std::cout << '\n';
}

/** Returns today's date in the local calendar for a default history range. */
atl::Date today() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  return atl::Date{
    local.tm_year + 1900,
    static_cast<unsigned>(local.tm_mon + 1),
    static_cast<unsigned>(local.tm_mday),
  };
}

} // namespace

int main() {
  try {
    atl::kis::KisConfig config;
    config.from_env();
    const bool demo = config.is_demo_;

    atl::kis::KisAccount account;
    account.from_env();

    std::cout << "environment: " << (demo ? "demo" : "live") << '\n';

    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    atl::kis::KisBlockingClient client(tls, std::move(config));

    // KIS does not offer the modifiable-order inquiry on demo accounts, so it
    // is skipped there rather than issuing a request that cannot succeed.
    if (!demo) {
      auto open_result = client.open_orders(account);
      if (!open_result) {
        print_error("modifiable-order inquiry", open_result.error());
        return 1;
      }
      std::cout << "open orders: " << open_result.value().orders_.size() << '\n';
      for (const auto& open_order : open_result.value().orders_) {
        print_order(open_order.order_);
        std::cout << "  amendable quantity: " << open_order.amendable_quantity_.str()
          << "  branch " << open_order.order_ref_.branch_number_ << '\n';
      }
    }

    const auto day = today();
    auto history_result = client.order_history(account, atl::kis::KisHistoryRange{day, day});
    if (!history_result) {
      print_error("order-history inquiry", history_result.error());
      return 1;
    }
    std::cout << "orders today: " << history_result.value().orders_.size() << '\n';
    for (const auto& order : history_result.value().orders_) {
      print_order(order);
    }
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
