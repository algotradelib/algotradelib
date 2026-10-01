#include "kis_credentials.hpp"

#include <algotradelib/kis>

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
  const auto directory = kis_credentials::take_directory(argc, argv);
  const std::string symbol = argc > 1 ? argv[1] : "005930";

  try {
    algotradelib::kis::KisConfig config;
    kis_credentials::load(directory, config, nullptr, std::cout);

    std::cout << "environment: " << (config.is_demo_ ? "demo" : "live") << '\n';
    std::cout << "symbol: " << symbol << '\n';

    net::io_context ioc;
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    auto client = std::make_shared<algotradelib::kis::KisClient>(
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
