#include <algotradelib/kis>
#include <iostream>
#include <string>

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;

int main() {
  // Load credentials from environment variables:
  // export KIS_APP_KEY=PSxxxxxxxxxxxxxxxx
  // export KIS_APP_SECRET=xxxxxxxx...
  // export KIS_DEMO=0   (1 = paper trading)
  algotradelib::kis::KisConfig cfg;
  try {
    cfg.from_env();
  } catch (const std::exception& e) {
    std::cerr << "[error] " << e.what() << "\n";
    return 1;
  }

  std::cout << "env      : " << (cfg.is_demo_ ? "demo (paper)" : "real") << "\n";
  std::cout << "REST     : " << cfg.rest_host() << ":" << cfg.rest_port() << "\n";
  std::cout << "WS       : " << cfg.ws_host() << ":" << cfg.ws_port() << "\n\n";

  net::io_context ioc;
  ssl::context ctx(ssl::context::tlsv12_client); // TLS 1.2 or 1.3
  ctx.set_default_verify_paths();
  ctx.set_verify_mode(ssl::verify_peer);
  algotradelib::kis::KisAuth auth(ioc, ctx, cfg);

  std::cout << "Fetching access token ...";
  try {
    (void)auth.accessToken();
    std::cout << " OK" << "\n\n";
  } catch (const std::exception& e) {
    std::cerr << "  FAIL: " << e.what() << "\n\n";
    return 1;
  }

  std::cout << "Fetching WS approval key ...";
  try {
    (void)auth.accessWebsocket();
    std::cout << " OK" << "\n\n";
  } catch (const std::exception& e) {
    std::cerr << "  FAIL: " << e.what() << "\n\n";
    return 1;
  }

  return 0;
}
