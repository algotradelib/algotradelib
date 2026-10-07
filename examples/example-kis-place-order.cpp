// Constructs and prints one representative KIS order without creating a
// client or performing network I/O. This example is always a dry-run.
#include <algotradelib/kis>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atl = algotradelib;

namespace {

/** @brief Parses one command-line order side. */
atl::OrderSide parse_order_side(std::string_view side) {
  if (side == "BUY") {
    return atl::OrderSide::BUY;
  }
  if (side == "SELL") {
    return atl::OrderSide::SELL;
  }
  throw std::invalid_argument("side must be BUY or SELL");
}

/** @brief Parses one command-line order type. */
atl::OrderType parse_order_type(std::string_view type) {
  if (type == "LIMIT") {
    return atl::OrderType::LIMIT;
  }
  if (type == "MARKET") {
    return atl::OrderType::MARKET;
  }
  throw std::invalid_argument("type must be LIMIT or MARKET");
}

/** @brief Prints the positional dry-run interface. */
void print_usage(const char* program) {
  std::cerr << "usage: " << program << " [symbol] [quantity] [BUY|SELL] "
    << "[LIMIT|MARKET] [limit-price]\n";
}

} // namespace

/** @brief Constructs and prints one KIS order request without network I/O. */
int main(int argc, char* argv[]) {
  try {
    if (argc > 6) {
      throw std::invalid_argument("too many arguments");
    }

    atl::OrderRequest order;
    order.instrument_ = atl::InstrumentId{
      atl::Country::KR,
      atl::Venue::COMBINED,
      argc > 1 ? argv[1] : "005930",
    };
    order.quantity_ = atl::Decimal64::parse(argc > 2 ? argv[2] : "1");
    order.side_ = parse_order_side(argc > 3 ? argv[3] : "BUY");
    order.type_ = parse_order_type(argc > 4 ? argv[4] : "LIMIT");
    if (order.type_ == atl::OrderType::LIMIT) {
      order.price_ = atl::Decimal64::parse(argc > 5 ? argv[5] : "70000");
    } else if (argc > 5) {
      throw std::invalid_argument("a MARKET dry-run must not include a limit price");
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
    print_usage(argv[0]);
    return 1;
  }
}
