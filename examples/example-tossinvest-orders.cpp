#include <algotradelib/tossinvest>

#include <boost/asio/ssl/context.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ssl = boost::asio::ssl;
namespace atl = algotradelib;

namespace {

/** Prints one structured broker failure and its retry delay when supplied. */
void print_error(std::string_view operation, const atl::BrokerError& error) {
  std::cerr << operation << " failed";
  if (!error.broker_code_.empty()) {
    std::cerr << " [" << error.broker_code_ << ']';
  }
  if (!error.request_id_.empty()) {
    std::cerr << " request=" << error.request_id_;
  }
  if (error.http_status_ != 0) {
    std::cerr << " HTTP " << error.http_status_;
  }
  if (!error.message_.empty()) {
    std::cerr << ": " << error.message_;
  }
  std::cerr << '\n';
  if (error.retry_after_) {
    std::cerr << "  retry after " << error.retry_after_->count() << " seconds\n";
  }
}

/** Returns a printable name for a normalized order status. */
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

/** Prints one normalized order and its aggregate fill state. */
void print_order(const atl::OrderSnapshot& order) {
  std::cout << order.instrument_.symbol_ << ' '
    << (order.side_ == atl::OrderSide::BUY ? "BUY" : "SELL") << ' '
    << status_name(order.status_) << " qty=" << order.quantity_.str();
  if (order.price_) {
    std::cout << " price=" << order.price_->str();
  }
  std::cout << " filled=" << order.execution_.filled_quantity_.str();
  if (order.execution_.average_filled_price_) {
    std::cout << " avg=" << order.execution_.average_filled_price_->str();
  }
  std::cout << " " << order.currency_.str() << " id=" << order.order_id_ << '\n';
}

/** Parses a positive Toss account sequence from one command-line argument. */
std::int64_t parse_account_sequence(const char* value) {
  const std::string text(value);
  std::size_t parsed = 0;
  const auto sequence = std::stoll(text, &parsed);
  if (parsed != text.size() || sequence <= 0) {
    throw std::invalid_argument("account sequence must be a positive integer");
  }
  return sequence;
}

} // namespace

/** Lists open orders and the first page of closed Toss orders. */
int main(int argc, char* argv[]) {
  try {
    atl::tossinvest::TossConfig config;
    config.from_env();

    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    atl::tossinvest::TossBlockingClient client(tls, std::move(config));

    std::int64_t sequence = 0;
    if (argc > 1) {
      sequence = parse_account_sequence(argv[1]);
    } else {
      auto accounts_result = client.accounts();
      if (!accounts_result) {
        print_error("account discovery", accounts_result.error());
        return 1;
      }
      if (accounts_result.value().empty()) {
        std::cout << "no accounts available\n";
        return 0;
      }
      sequence = accounts_result.value().front().sequence_;
    }
    std::cout << "selected account: " << sequence << '\n';

    atl::tossinvest::TossOrderQuery open_query;
    open_query.account_sequence_ = sequence;
    open_query.scope_ = atl::tossinvest::TossOrderScope::OPEN;
    auto open_result = client.orders(open_query);
    if (!open_result) {
      print_error("open order listing", open_result.error());
      return 1;
    }
    std::cout << "open orders: " << open_result.value().orders_.size() << '\n';
    for (const auto& order : open_result.value().orders_) {
      print_order(order);
    }

    // Fetch only the first closed page. Following every cursor automatically
    // can consume the ORDER_HISTORY request budget; callers decide explicitly
    // whether and when to request another page.
    atl::tossinvest::TossOrderQuery closed_query;
    closed_query.account_sequence_ = sequence;
    closed_query.scope_ = atl::tossinvest::TossOrderScope::CLOSED;
    closed_query.limit_ = 20;
    auto closed_result = client.orders(closed_query);
    if (!closed_result) {
      print_error("closed order listing", closed_result.error());
      return 1;
    }
    std::cout << "closed orders on first page: " << closed_result.value().orders_.size() << '\n';
    std::cout << "more closed orders: "
      << (closed_result.value().next_cursor_ ? "yes" : "no") << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
