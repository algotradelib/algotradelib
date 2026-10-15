#include <algotradelib/src/network/ws_session.hpp>
#include <algotradelib/src/network/wss_session.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string_view>
#include <type_traits>

using namespace algotradelib;

namespace algotradelib::network {

/** Drives a session's connect stages directly, as if each had completed without an error. */
struct WebSocketSessionTestAccess {
  /** Gives a session the unconnected stream `do_connect()` would, without starting the lookup. */
  template <typename Session>
  static void with_stream(Session& session) {
    session.ws_ = std::make_unique<typename Session::Stream>(session.strand_);
  }

  template <typename Transport>
  static void with_tls_stream(Transport& session) {
    session.ws_ = std::make_unique<typename Transport::Stream>(session.strand_, session.context_);
  }

  /** The state `close()` leaves while a connect is in progress. */
  template <typename Session>
  static void connecting_then_closed(Session& session) {
    session.connecting_ = true;
    session.closing_ = true;
    session.stop_requested_ = true;
  }

  template <typename Session>
  static void resolved(Session& session) {
    session.on_resolve(beast::error_code{}, tcp::resolver::results_type{});
  }

  template <typename Session>
  static void connected(Session& session) {
    session.on_connect(beast::error_code{}, tcp::endpoint{});
  }

  static void tls_established(WssSession& session) { session.on_ssl_handshake(beast::error_code{}); }

  template <typename Session>
  static void upgraded(Session& session) {
    session.on_handshake(beast::error_code{});
  }

  template <typename Session>
  static bool idle(const Session& session) {
    return !session.connected_ && !session.connecting_ && !session.closing_;
  }
};

} // namespace algotradelib::network

namespace {

using network::WebSocketSessionTestAccess;

/** Counts the callbacks a session makes. */
template <typename Transport>
class Probe : public Transport {
public:
  using Transport::Transport;
  int opened_{0};
  int closed_{0};

protected:
  void on_message(std::string_view) override {}
  void on_open() override { ++opened_; }
  void on_closed() override { ++closed_; }
};

/** Delivers one connect stage after `close()`, then checks the session closed and can close again. */
template <typename Session, typename Stage>
void check_stage(net::io_context& ioc, const std::shared_ptr<Session>& session, Stage stage) {
  if constexpr (std::is_base_of_v<network::WssSession, Session>) {
    WebSocketSessionTestAccess::with_tls_stream(*session);
  } else {
    WebSocketSessionTestAccess::with_stream(*session);
  }
  WebSocketSessionTestAccess::connecting_then_closed(*session);
  stage(*session);
  CHECK(session->opened_ == 0);
  CHECK(session->closed_ == 1);
  CHECK(WebSocketSessionTestAccess::idle(*session));
  // The close is not left in progress, so a later one is not ignored.
  session->close();
  ioc.restart();
  ioc.run();
  CHECK(session->closed_ == 2);
}

} // namespace

TEST_CASE("a connect stage that completes after close() closes the session instead of opening it", "[network][websocket]") {
  net::io_context ioc;

  SECTION("WsSession: the lookup") {
    check_stage(ioc, std::make_shared<Probe<network::WsSession>>(ioc, "localhost", "1"), [](auto& session) {
      WebSocketSessionTestAccess::resolved(session);
    });
  }
  SECTION("WsSession: the TCP connect") {
    check_stage(ioc, std::make_shared<Probe<network::WsSession>>(ioc, "localhost", "1"), [](auto& session) {
      WebSocketSessionTestAccess::connected(session);
    });
  }
  SECTION("WsSession: the upgrade") {
    check_stage(ioc, std::make_shared<Probe<network::WsSession>>(ioc, "localhost", "1"), [](auto& session) {
      WebSocketSessionTestAccess::upgraded(session);
    });
  }

  ssl::context context(ssl::context::tls_client);
  SECTION("WssSession: the TCP connect") {
    check_stage(
      ioc, std::make_shared<Probe<network::WssSession>>(ioc, context, "localhost", "1"), [](auto& session) {
        WebSocketSessionTestAccess::connected(session);
      }
    );
  }
  SECTION("WssSession: the TLS handshake") {
    check_stage(
      ioc, std::make_shared<Probe<network::WssSession>>(ioc, context, "localhost", "1"), [](auto& session) {
        WebSocketSessionTestAccess::tls_established(session);
      }
    );
  }
  SECTION("WssSession: the upgrade") {
    check_stage(
      ioc, std::make_shared<Probe<network::WssSession>>(ioc, context, "localhost", "1"), [](auto& session) {
        WebSocketSessionTestAccess::upgraded(session);
      }
    );
  }
}
