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
  std::cout << order.order_id_ << "  " << order.instrument_.symbol_ << "  "
    << order.currency_.str() << '\n';
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
  if (order.execution_.commission_) {
    std::cout << "  commission " << order.execution_.commission_->amount_.str();
  }
  std::cout << '\n';
}

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

    // A closed listing pages; the returned cursor is passed back unchanged
    // until Toss reports no further page.
    atl::tossinvest::TossOrderQuery closed_query;
    closed_query.account_sequence_ = sequence;
    closed_query.scope_ = atl::tossinvest::TossOrderScope::CLOSED;
    closed_query.limit_ = 20;

    std::size_t closed_count = 0;
    for (std::size_t page = 0; page < 10; ++page) {
      auto closed_result = client.orders(closed_query);
      if (!closed_result) {
        print_error("closed order listing", closed_result.error());
        return 1;
      }
      for (const auto& order : closed_result.value().orders_) {
        print_order(order);
        ++closed_count;
      }
      if (!closed_result.value().next_cursor_) {
        break;
      }
      closed_query.cursor_ = closed_result.value().next_cursor_;
    }
    std::cout << "closed orders listed: " << closed_count << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
