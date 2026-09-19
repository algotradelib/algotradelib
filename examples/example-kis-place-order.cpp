// Constructs and prints one representative KIS order without creating a
// client or performing network I/O. This example is always a dry-run.
#include <algotradelib/kis>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace atl = algotradelib;

namespace {

std::string environment_value_or(const char* name, std::string fallback) {
  const char* value = std::getenv(name);
  return value && *value != '\0' ? std::string(value) : std::move(fallback);
}

atl::OrderSide order_side() {
  const auto side = environment_value_or("ATL_ORDER_SIDE", "BUY");
  if (side == "BUY") {
    return atl::OrderSide::BUY;
  }
  if (side == "SELL") {
    return atl::OrderSide::SELL;
  }
  throw std::invalid_argument("ATL_ORDER_SIDE must be BUY or SELL");
}

atl::OrderType order_type() {
  const auto type = environment_value_or("ATL_ORDER_TYPE", "LIMIT");
  if (type == "LIMIT") {
    return atl::OrderType::LIMIT;
  }
  if (type == "MARKET") {
    return atl::OrderType::MARKET;
  }
  throw std::invalid_argument("ATL_ORDER_TYPE must be LIMIT or MARKET");
}

} // namespace

int main() {
  try {
    atl::OrderRequest order;
    order.instrument_ = atl::InstrumentId{
      atl::Country::KR,
      atl::Venue::COMBINED,
      environment_value_or("ATL_ORDER_SYMBOL", "005930"),
    };
    order.quantity_ = atl::Decimal64::parse(
      environment_value_or("ATL_ORDER_QUANTITY", "1")
    );
    order.side_ = order_side();
    order.type_ = order_type();
    if (order.type_ == atl::OrderType::LIMIT) {
      order.price_ = atl::Decimal64::parse(
        environment_value_or("ATL_ORDER_PRICE", "70000")
      );
    }

    std::cout << "dry-run: this example never sends an order\n";
    std::cout << (order.side_ == atl::OrderSide::BUY ? "BUY " : "SELL ")
      << order.quantity_.str() << ' ' << order.instrument_.symbol_;
    if (order.price_) {
      std::cout << " @ " << order.price_->str();
    } else {
      std::cout << " at market";
    }
    std::cout << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
