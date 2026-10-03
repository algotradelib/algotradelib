#include "kiwoom_credentials.hpp"

#include <algotradelib/kiwoom>
#include <iostream>

namespace net = boost::asio;
namespace ssl = net::ssl;

int main(int argc, char** argv) {
  // Credentials from one file per key (kiwoom_credentials.hpp):
  //   example-kiwoom-auth --credentials DIR [--kiwoom-account kr]   reads DIR/kiwoom/demo-kr.json
  // or, without a directory, from environment variables:
  //   export KIWOOM_APP_KEY=AxserEsdcredca.....
  //   export KIWOOM_SECRET_KEY=SEefdcwcforehDre2fdvc....
  // KIWOOM_DEMO=0 selects live (real-<label>.json); unset or 1 is paper trading.
  algotradelib::kiwoom::KiwoomConfig cfg;
  try {
    const auto directory = kis_credentials::take_directory(argc, argv);
    const auto label = kiwoom_credentials::take_label(argc, argv);
    kiwoom_credentials::refuse_unknown_flags(argc, argv);
    kiwoom_credentials::load(directory, label, cfg, std::cout);
  } catch (const std::exception& e) {
    std::cerr << "[error] " << e.what() << "\n";
    return 1;
  }

  std::cout << "env      : " << (cfg.is_demo_ ? "demo (paper)" : "real") << "\n";
  std::cout << "REST     : " << cfg.rest_host() << "\n";

  net::io_context ioc;
  ssl::context ctx(ssl::context::tlsv12_client); // TLS 1.2 or 1.3
  ctx.set_default_verify_paths();
  ctx.set_verify_mode(ssl::verify_peer);
  algotradelib::kiwoom::KiwoomAuth auth(ioc, ctx, cfg);

  std::cout << "Fetching access token ...";
  try {
    (void)auth.accessToken();
    std::cout << " OK" << "\n\n";
  } catch (const std::exception& e) {
    std::cerr << "  FAIL: " << e.what() << "\n\n";
    return 1;
  }

  return 0;
}
