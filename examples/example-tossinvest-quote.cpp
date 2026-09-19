#include <algotradelib/tossinvest>

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
    algotradelib::tossinvest::TossConfig config;
    config.from_env();

    std::cout << "broker: Toss Securities" << "\n";
    std::cout << "symbol: " << symbol << '\n';

    net::io_context ioc;
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    auto client = std::make_shared<algotradelib::tossinvest::TossClient>(
      ioc.get_executor(), tls, std::move(config)
    );
    int exit_code = 1;
    client->async_quote(
      {algotradelib::Country::KR, algotradelib::Venue::COMBINED, symbol},
      [&exit_code](algotradelib::Result<algotradelib::QuoteSnapshot> result) {
        if (!result) {
          const auto& error = result.error();
          std::cerr << "quote failed";
          if (!error.broker_code.empty()) {
            std::cerr << " [" << error.broker_code << ']';
          }
          if (!error.request_id.empty()) {
            std::cerr << " request=" << error.request_id;
          }
          if (error.http_status != 0) {
            std::cerr << " HTTP " << error.http_status;
          }
          if (!error.message.empty()) {
            std::cerr << ": " << error.message;
          }
          std::cerr << '\n';
          return;
        }

        const auto& quote = result.value();
        std::cout << quote.instrument.symbol << ' ' << quote.last_price.str() << ' '
          << quote.currency.str() << '\n';
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
