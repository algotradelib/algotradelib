#include <algotradelib/tossinvest>
#include <iostream>
#include <string>

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;

int main() {
  // Load credentials from environment variables:
  // export TOSS_CLIENT_ID=c_01HXYZABCDEFG123456789
  // export TOSS_CLIENT_SECRET=xxxxxxxx...
  algotradelib::tossinvest::TossConfig cfg;
  try {
    cfg.from_env();
  } catch (const std::exception& e) {
    std::cerr << "[error] " << e.what() << "\n";
    return 1;
  }
	
	net::io_context ioc;
	ssl::context ctx(ssl::context::tlsv12_client); // TLS 1.2 or 1.3
	ctx.set_default_verify_paths();
	ctx.set_verify_mode(ssl::verify_peer);
  algotradelib::tossinvest::TossAuth auth(ioc, ctx, cfg);

  std::cout << "Fetching access token ...";
  try {
    const std::string& token = auth.accessToken();
    std::cout << " OK" << "\n\n";
  } catch (const std::exception& e) {
    std::cerr << "  FAIL: " << e.what() << "\n\n";
    return 1;
  }

  return 0;
}
