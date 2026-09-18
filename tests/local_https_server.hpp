#pragma once

#include <algotradelib/core>

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace algotradelib::test {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

inline constexpr std::string_view LOCALHOST_CERTIFICATE = R"(-----BEGIN CERTIFICATE-----
MIIDHzCCAgegAwIBAgIUDPB/ppDbPzW9VYCOuxw3EBnOhycwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDkxODA2NDMzMFoXDTM2MDkx
NTA2NDMzMFowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAtHKP+PI5U4I9yz8hLBCEsD1cVqPk7r6dy/dLdFMEC9fr
Kmm79cWBjIH1ndyDqhBkQroFcBLF5kHs8BtdPI+ZsXnZLSjE/xPFTe50MonBseyo
qujeOZPLFuxM9zjIji32TOiY8BLlgFeSO4L1d2H6oimTahE4Kl/nf0lA4VX0Hado
8KGQ93hRiWos6OsMSDaXoUJK12v+Qtw0eFU4SZ6+oBAYH6DdHsU3Bowo/x0G9UrL
+RYWlRRYxYB43Qq1hEhJ7woqZ7ZHZdg1T7uOAyW8rtr0sDwU3ggElwxq6mheI1yM
0YST2rBfgumbpD1o35QC85jGf1DOibEenQRwfZzq+wIDAQABo2kwZzAdBgNVHQ4E
FgQUQWa8Q5/3HA76+LO/raAbYjs5PU8wHwYDVR0jBBgwFoAUQWa8Q5/3HA76+LO/
raAbYjs5PU8wDwYDVR0TAQH/BAUwAwEB/zAUBgNVHREEDTALgglsb2NhbGhvc3Qw
DQYJKoZIhvcNAQELBQADggEBAG4BA6iK7+dtPTBlxhv0+5jsPNML0waM5UZYK75k
buE+2zmMRkQHFh3nBP8zk41yTsLIBMW2jc9cbwGvBW68PP/SWerOgKpzGem4kVZu
HV5Q5L+s8J2g4BJtVG0LUGA4nXkaBKTOmhv1TzONSGs+oNOftGiVYa9doUF70Vtu
8fImBiSLrUvHyTjrOrTCQ50MikELOhS+fbryBQoruV/Tam/BAnu6WNV0gi7AT80C
ee2zdl6zhc4Jvech9m9EuN0OKhVWi3fOq1C/aMfiBMC997KrXULdAOLhx8K1sWNA
mavbpV5W8SbPPU77vM2/FBXwjqFUL14KDHoJgyuMNW2UH1o=
-----END CERTIFICATE-----
)";

inline constexpr std::string_view LOCALHOST_PRIVATE_KEY = R"(-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQC0co/48jlTgj3L
PyEsEISwPVxWo+Tuvp3L90t0UwQL1+sqabv1xYGMgfWd3IOqEGRCugVwEsXmQezw
G108j5mxedktKMT/E8VN7nQyicGx7Kiq6N45k8sW7Ez3OMiOLfZM6JjwEuWAV5I7
gvV3YfqiKZNqETgqX+d/SUDhVfQdp2jwoZD3eFGJaizo6wxINpehQkrXa/5C3DR4
VThJnr6gEBgfoN0exTcGjCj/HQb1Ssv5FhaVFFjFgHjdCrWESEnvCipntkdl2DVP
u44DJbyu2vSwPBTeCASXDGrqaF4jXIzRhJPasF+C6ZukPWjflALzmMZ/UM6JsR6d
BHB9nOr7AgMBAAECggEAFxafgFFx71L3WCWsN2inqas4QOe4l6mxJBNwxqIGnwNS
FjuwLprGYDWSuU+pUx3Ezlrh7jygsL0XtYMy01hmaHR4StRwHFkpwjZ0DLFepMbs
r+2a3ZAhCL5UfL95veN4o8+RHgq07c6GqfGwOcXCHXSnZYdtgdKCmuFDGXH3icGW
ZQHhmiiypplO11My8sMbeZNbkA976EV7BIqH2edAN92/n3BUzkXNdP4+Mh9Tzua/
80mCHRxnfyITA2vqLyw8ohM3RcBscp4UiCjg3nR4Do+Yr8d0oTUpObCoyeR9bkvi
vZhcmaCwQTYZJ7wnfzIh1tafsioBc52KX3kr3DJulQKBgQDZZ0sjCvkuCsKCzu4D
LYPE2QSkhWCo2ook6lPtRCkdkHssYaAIYQtgLV5Ny12TvfL8nCF7NyzshmkGChLS
cu2RPYdjOWkFqE/SPNCkvuYrW2dFLa9iTqFFCTHUM06R1/stB/nyuKVh5MtxXCVl
d1Z2LK8TzkaJs5sP9977/AODXwKBgQDUe6riXBnhUHdLE4fECKtlydlEN+j9VtDO
0UoL8141s9QCV9b37Cg1ncOInD9eRDGjXq6AFeqUTJk1BLuLHIPddLeZAcT4QY3Y
3L8xZWxC+QFgu+TOgR7HSFAEzQMTHNwlHIAq1Xe5EW4QNwyai4cjg6iroW8GpS3a
1k/Nwu/55QKBgBcMYGbirvA2q9B6Z7lmheXErD1U9p5wLN0lCqjI5SMBQk2+O90n
jLExniHZQZ58MNqfIt2RR+37gf7l6oDtNkqDn2rhl0TGR1AeqnAM3RITqqsm4MUn
WOmcw++fUOEdaL+pbNJSyRXLz9XA0yYitMPqkNUktU28AOB8guNsIGM5AoGAJ25z
veIdo2SNXs9XCJgbRp54xWIiyLDNuzO3rBQTzZ97npuydvvKUnjeioGEePfqxkiR
1Y87ox276MziCOTXy8cXMzvS08dGjZIZoQ8TUjJngQ4dXf00OwM+ZkWtZmSargBm
AIzw1NVZOYUdv6hZAcwzBvbEZdVNdykCUkr0wckCgYABbrErIvrcMwl1yjXDNoXf
QRyuaneIPoOijAwlSPUq7LGrWjViuEaygiu8WFFMWmWzJMe87psGHRhSJ7G8fcmo
6wND1ASNUYmKJAyMDK8ugxFQ5ATHmxt2Lf0cRD+zpmmD4+/3qCoMZucHXFcq89VZ
OV9IxaWtgjumdgBw478W4w==
-----END PRIVATE KEY-----
)";

inline void trustLocalhostCertificate(
  ssl::context& context) {
  context.set_verify_mode(ssl::verify_peer);
  context.add_certificate_authority(net::buffer(LOCALHOST_CERTIFICATE));
}

class LocalHttpsServer {
public:
  LocalHttpsServer()
  : context_(ssl::context::tls_server),
    acceptor_(ioc_, tcp::endpoint(net::ip::address_v4::loopback(), 0)) {
    context_.use_certificate_chain(net::buffer(LOCALHOST_CERTIFICATE));
    context_.use_private_key(net::buffer(LOCALHOST_PRIVATE_KEY), ssl::context::pem);
    acceptor_.non_blocking(true);
    thread_ = std::thread([this] { run(); });
  }

  ~LocalHttpsServer() {
    stopped_ = true;
    beast::error_code ignored;
    acceptor_.close(ignored);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  LocalHttpsServer(const LocalHttpsServer&) = delete;
  LocalHttpsServer& operator=(const LocalHttpsServer&) = delete;

  std::string port() const { return std::to_string(acceptor_.local_endpoint().port()); }

  std::size_t connectionCount() const noexcept { return connection_count_; }

  std::size_t requestCount(
    std::string_view target) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = request_counts_.find(std::string(target));
    return found == request_counts_.end() ? 0 : found->second;
  }

private:
  net::io_context ioc_;
  ssl::context context_;
  tcp::acceptor acceptor_;
  std::atomic<bool> stopped_{false};
  std::atomic<std::size_t> connection_count_{0};
  mutable std::mutex mutex_;
  std::map<std::string, std::size_t> request_counts_;
  std::thread thread_;

  void run() noexcept {
    try {
      while (!stopped_) {
        tcp::socket socket(ioc_);
        beast::error_code ec;
        acceptor_.accept(socket, ec);
        if (ec == net::error::would_block || ec == net::error::try_again) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        if (ec) {
          if (!stopped_) {
            std::this_thread::yield();
          }
          continue;
        }
        ++connection_count_;
        serve(std::move(socket));
      }
    } catch (...) {
      stopped_ = true;
    }
  }

  void serve(
    tcp::socket socket) {
    beast::ssl_stream<beast::tcp_stream> stream(std::move(socket), context_);
    beast::error_code ec;
    stream.handshake(ssl::stream_base::server, ec);
    if (ec) {
      return;
    }

    beast::flat_buffer buffer;
    while (!stopped_) {
      http::request<http::string_body> request;
      http::read(stream, buffer, request, ec);
      if (ec) {
        return;
      }

      const std::string target(request.target());
      const auto count = recordRequest(target);
      if ((target == "/drop-get-once" && count == 1) || target == "/drop-post") {
        close(stream);
        return;
      }

      http::response<http::string_body> response(http::status::ok, request.version());
      response.set(http::field::server, "algotradelib-test");
      response.set(http::field::content_type, "text/plain");
      response.keep_alive(target != "/close" && request.keep_alive());
      response.body() = target == "/large" ? std::string(4096, 'x') : target;
      response.prepare_payload();
      http::write(stream, response, ec);
      if (ec || !response.keep_alive()) {
        beast::error_code ignored;
        stream.shutdown(ignored);
        return;
      }
    }
  }

  std::size_t recordRequest(
    const std::string& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ++request_counts_[target];
  }

  static void close(
    beast::ssl_stream<beast::tcp_stream>& stream) noexcept {
    beast::error_code ignored;
    beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
    beast::get_lowest_layer(stream).socket().close(ignored);
  }
};

} // namespace algotradelib::test
