// Reads one Kiwoom quote, its price bounds and the top of its book.
//
// All three come from one `ka10007` response, so this costs one request rather than three.

#include <algotradelib/kiwoom>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace net = boost::asio;
namespace ssl = net::ssl;

int main(int argc, char* argv[]) {
  const std::string symbol = argc > 1 ? argv[1] : "005930";

  try {
    algotradelib::kiwoom::KiwoomConfig config;
    config.from_env();

    std::cout << "environment: " << (config.is_demo_ ? "demo" : "live") << '\n';
    std::cout << "symbol: " << symbol << '\n';
    // One setting for every market-data verb; the consolidated book by default.
    std::cout << "tape suffix: '" << algotradelib::kiwoom::tape_suffix(config.market_tape_)
      << "'\n";

    net::io_context ioc;
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    auto client = std::make_shared<algotradelib::kiwoom::KiwoomClient>(
      ioc.get_executor(), tls, std::move(config)
    );
    int exit_code = 1;
    client->async_quote(
      {algotradelib::Country::KR, algotradelib::Venue::COMBINED, symbol},
      [&exit_code](algotradelib::Result<algotradelib::QuoteSnapshot> result) {
        if (!result) {
          const auto& error = result.error();
          std::cerr << "quote failed";
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
          return;
        }

        const auto& quote = result.value();
        std::cout << quote.instrument_.symbol_ << ' ' << quote.last_price_.str() << ' '
          << quote.currency_.str() << '\n';
        if (quote.volume_) {
          std::cout << "volume: " << quote.volume_->str() << '\n';
        }
        if (quote.top_of_book_) {
          const auto& top = *quote.top_of_book_;
          std::cout << "bid: " << (top.bid_price_ ? top.bid_price_->str() : "-") << " x "
            << (top.bid_quantity_ ? top.bid_quantity_->str() : "-") << '\n';
          std::cout << "ask: " << (top.ask_price_ ? top.ask_price_->str() : "-") << " x "
            << (top.ask_quantity_ ? top.ask_quantity_->str() : "-") << '\n';
        }
        exit_code = 0;
      }
    );
    ioc.run();
    return exit_code;
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
