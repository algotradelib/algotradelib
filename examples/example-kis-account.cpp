#include "kis_credentials.hpp"

#include <algotradelib/kis>

#include <boost/asio/ssl/context.hpp>

#include <exception>
#include <iostream>
#include <string_view>
#include <utility>

namespace ssl = boost::asio::ssl;

namespace {

void print_error(std::string_view operation, const algotradelib::BrokerError& error) {
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
}

void print_holdings(const algotradelib::HoldingsSnapshot& snapshot) {
  std::cout << "positions: " << snapshot.positions_.size() << '\n';
  for (const auto& position : snapshot.positions_) {
    std::cout << position.instrument_.symbol_ << "  " << position.name_ << '\n';
    std::cout << "  quantity: " << position.quantity_.str();
    if (position.sellable_quantity_) {
      std::cout << " (sellable " << position.sellable_quantity_->str() << ')';
    }
    std::cout << '\n';
    std::cout << "  last price: " << position.last_price_.str() << ' '
      << position.market_value_.currency_.str() << '\n';
    std::cout << "  market value: " << position.market_value_.amount_.str() << ' '
      << position.market_value_.currency_.str() << '\n';
    std::cout << "  unrealized P/L: " << position.unrealized_profit_loss_.amount_.str() << ' '
      << position.unrealized_profit_loss_.currency_.str() << '\n';
  }

  std::cout << "totals: " << snapshot.totals_.size() << '\n';
  for (const auto& total : snapshot.totals_) {
    std::cout << total.currency_.str() << "  purchase=" << total.purchase_value_.str()
      << "  market=" << total.market_value_.str()
      << "  unrealized=" << total.unrealized_profit_loss_.str() << '\n';
  }
}

} // namespace

int main(int argc, char* argv[]) {
  try {
    algotradelib::kis::KisConfig config;
    algotradelib::kis::KisAccount account;
    kis_credentials::load(kis_credentials::take_directory(argc, argv), config, &account, std::cout);

    std::cout << "environment: " << (config.is_demo_ ? "demo" : "live") << '\n';

    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    algotradelib::kis::KisBlockingClient client(tls, std::move(config), kis_credentials::paced_options());
    auto result = client.holdings(std::move(account));
    if (!result) {
      print_error("holdings request", result.error());
      return 1;
    }

    print_holdings(result.value());
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
