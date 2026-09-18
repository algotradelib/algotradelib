#pragma once

#include <algotradelib/core>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace algotradelib {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace network {

using HttpResponse = http::response<http::string_body>;

struct HttpRequest {
  http::verb method{http::verb::get};
  std::string target{"/"};
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  int version{11};
};

struct HttpClientOptions {
  std::chrono::seconds timeout{30};
  std::size_t body_limit{8 * 1024 * 1024};
  std::size_t queue_limit{1024};
};

inline BrokerError transport_error(
  std::string stage,
  beast::error_code ec
) {
  BrokerError error;
  error.category =
    ec == net::error::operation_aborted ? ErrorCategory::CANCELLED : ErrorCategory::TRANSPORT;
  error.broker_code = std::move(stage);
  error.message = ec.message();
  return error;
}

inline std::string make_host_header(
  const std::string& host,
  const std::string& port
) {
  return port == "443" ? host : host + ":" + port;
}

// Shuts the SSL layer down (best-effort close_notify, bounded by `timeout`
// so an unresponsive peer cannot stall the caller) before hard-closing the
// underlying TCP socket.
inline void shutdown_ssl_stream(
  beast::ssl_stream<beast::tcp_stream>& stream,
  std::chrono::seconds timeout
) noexcept {
  beast::error_code ignored;
  beast::get_lowest_layer(stream).expires_after(timeout);
  stream.shutdown(ignored);
  beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
  beast::get_lowest_layer(stream).socket().close(ignored);
}

inline void close_ssl_stream(
  beast::ssl_stream<beast::tcp_stream>& stream
) noexcept {
  beast::error_code ignored;
  beast::get_lowest_layer(stream).cancel();
  beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
  beast::get_lowest_layer(stream).socket().close(ignored);
}

class HttpClient {
public:
  HttpClient(
    net::io_context& ioc,
    ssl::context& context,
    std::string host,
    std::string port,
    HttpClientOptions options = {}
  )
  : resolver_(ioc), context_(context), host_(std::move(host)), port_(std::move(port)),
    options_(options) {}

  HttpResponse get(
    const std::string& target,
    const std::map<std::string, std::string>& headers = {}
  ) {
    return request(http::verb::get, target, "", headers);
  }

  HttpResponse post(
    const std::string& target,
    const std::string& body,
    const std::map<std::string, std::string>& headers = {}
  ) {
    return request(http::verb::post, target, body, headers);
  }

  HttpResponse request(
    http::verb method,
    const std::string& target,
    const std::string& body = {},
    const std::map<std::string, std::string>& headers = {}
  ) {
    for (int attempt = 0;; ++attempt) {
      try {
        ensure_connected();
        http::request<http::string_body> request_value{method, target, 11};
        request_value.set(http::field::host, host_header());
        request_value.set(http::field::user_agent, "algotradelib");
        request_value.keep_alive(true);
        for (const auto& header : headers) {
          request_value.set(header.first, header.second);
        }
        request_value.body() = body;
        if (!body.empty()) {
          request_value.prepare_payload();
        }

        beast::get_lowest_layer(*stream_).expires_after(options_.timeout);
        http::write(*stream_, request_value);
        http::response_parser<http::string_body> parser;
        parser.body_limit(options_.body_limit);
        http::read(*stream_, buffer_, parser);
        auto response = parser.release();
        if (!response.keep_alive() || response.need_eof()) {
          disconnect();
        }
        return response;
      } catch (const beast::system_error&) {
        disconnect();
        if (method != http::verb::get || attempt > 0) {
          throw;
        }
      }
    }
  }

  void close() { disconnect(); }

private:
  tcp::resolver resolver_;
  ssl::context& context_;
  std::string host_;
  std::string port_;
  HttpClientOptions options_;
  std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream_;
  beast::flat_buffer buffer_;

  void ensure_connected() {
    if (stream_ && beast::get_lowest_layer(*stream_).socket().is_open()) {
      return;
    }
    stream_ =
      std::make_unique<beast::ssl_stream<beast::tcp_stream>>(resolver_.get_executor(), context_);
    if (!SSL_set_tlsext_host_name(stream_->native_handle(), host_.c_str())) {
      throw beast::system_error(
        static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()
      );
    }
    stream_->set_verify_callback(ssl::host_name_verification(host_));
    const auto endpoints = resolver_.resolve(host_, port_);
    beast::get_lowest_layer(*stream_).expires_after(options_.timeout);
    beast::get_lowest_layer(*stream_).connect(endpoints);
    beast::get_lowest_layer(*stream_).expires_after(options_.timeout);
    stream_->handshake(ssl::stream_base::client);
  }

  std::string host_header() const { return make_host_header(host_, port_); }

  void disconnect() noexcept {
    if (!stream_) {
      return;
    }
    shutdown_ssl_stream(*stream_, std::chrono::seconds(1));
    stream_.reset();
    buffer_.consume(buffer_.size());
  }
};

class AsyncHttpClient : public std::enable_shared_from_this<AsyncHttpClient> {
public:
  using CompletionHandler = net::any_completion_handler<void(Result<HttpResponse>)>;

  AsyncHttpClient(
    net::any_io_executor executor,
    ssl::context& context,
    std::string host,
    std::string port,
    HttpClientOptions options = {}
  )
  : strand_(net::make_strand(std::move(executor))), resolver_(strand_), resolve_timer_(strand_),
    context_(context), host_(std::move(host)), port_(std::move(port)), options_(options) {}

  template <typename CompletionToken>
  auto async_request(
    HttpRequest request_value,
    CompletionToken&& token
  ) {
    return net::async_initiate<CompletionToken, void(Result<HttpResponse>)>(
      [self = shared_from_this(),
       request_value = std::move(request_value)](auto&& handler) mutable {
        Pending pending(
          std::move(request_value), CompletionHandler(std::forward<decltype(handler)>(handler))
        );
        net::post(self->strand_, [self, pending = std::move(pending)]() mutable {
          self->enqueue(std::move(pending));
        });
      },
      token
    );
  }

  void close() {
    net::post(strand_, [self = shared_from_this()] {
      self->stopped_ = true;
      self->resolver_.cancel();
      self->resolve_timer_.cancel();
      self->disconnect();
      BrokerError error;
      error.category = ErrorCategory::CANCELLED;
      error.broker_code = "close";
      error.message = "HTTP client closed";
      while (!self->queue_.empty()) {
        auto pending = std::move(self->queue_.front());
        self->queue_.pop_front();
        self->complete(std::move(pending.handler), Result<HttpResponse>(error));
      }
    });
  }

private:
  enum class State {
    DISCONNECTED,
    RESOLVING,
    CONNECTING,
    HANDSHAKING,
    READY,
    WRITING,
    READING,
  };

  struct Pending {
    Pending(
      HttpRequest request_value,
      CompletionHandler completion
    )
    : request(std::move(request_value)), handler(std::move(completion)) {}
    HttpRequest request;
    CompletionHandler handler;
    bool retried{false};
  };

  net::strand<net::any_io_executor> strand_;
  tcp::resolver resolver_;
  net::steady_timer resolve_timer_;
  ssl::context& context_;
  std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream_;
  beast::flat_buffer buffer_;
  std::string host_;
  std::string port_;
  HttpClientOptions options_;
  State state_{State::DISCONNECTED};
  bool stopped_{false};
  bool resolve_timed_out_{false};
  std::deque<Pending> queue_;
  std::optional<http::request<http::string_body>> active_request_;
  std::optional<http::response_parser<http::string_body>> parser_;

  void enqueue(
    Pending pending
  ) {
    if (stopped_) {
      BrokerError error;
      error.category = ErrorCategory::CANCELLED;
      error.broker_code = "enqueue";
      error.message = "HTTP client is closed";
      complete(std::move(pending.handler), Result<HttpResponse>(std::move(error)));
      return;
    }
    if (queue_.size() >= options_.queue_limit) {
      BrokerError error;
      error.category = ErrorCategory::TRANSPORT;
      error.broker_code = "queue_full";
      error.message = "HTTP request queue limit exceeded";
      complete(std::move(pending.handler), Result<HttpResponse>(std::move(error)));
      return;
    }
    queue_.push_back(std::move(pending));
    pump();
  }

  void pump() {
    if (stopped_ || queue_.empty()) {
      return;
    }
    if (state_ == State::DISCONNECTED) {
      start_resolve();
    } else if (state_ == State::READY) {
      start_write();
    }
  }

  void start_resolve() {
    stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(strand_, context_);
    state_ = State::RESOLVING;
    resolve_timed_out_ = false;
    resolve_timer_.expires_after(options_.timeout);
    resolve_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
      if (!ec && self->state_ == State::RESOLVING) {
        self->resolve_timed_out_ = true;
        self->resolver_.cancel();
      }
    });
    resolver_.async_resolve(
      host_,
      port_,
      [self = shared_from_this()](beast::error_code ec, tcp::resolver::results_type endpoints) {
        self->resolve_timer_.cancel();
        if (ec) {
          if (self->resolve_timed_out_) {
            ec = beast::error_code(net::error::timed_out);
          }
          self->fail("resolve", ec);
          return;
        }
        self->state_ = State::CONNECTING;
        beast::get_lowest_layer(*self->stream_).expires_after(self->options_.timeout);
        beast::get_lowest_layer(*self->stream_)
          .async_connect(
            endpoints,
            [self](beast::error_code connect_ec, tcp::resolver::results_type::endpoint_type) {
              if (connect_ec) {
                self->fail("connect", connect_ec);
                return;
              }
              self->start_handshake();
            }
          );
      }
    );
  }

  void start_handshake() {
    if (!SSL_set_tlsext_host_name(stream_->native_handle(), host_.c_str())) {
      fail(
        "sni",
        beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category())
      );
      return;
    }
    stream_->set_verify_callback(ssl::host_name_verification(host_));
    state_ = State::HANDSHAKING;
    beast::get_lowest_layer(*stream_).expires_after(options_.timeout);
    stream_->async_handshake(
      ssl::stream_base::client, [self = shared_from_this()](beast::error_code ec) {
        if (ec) {
          self->fail("tls_handshake", ec);
          return;
        }
        self->state_ = State::READY;
        self->pump();
      }
    );
  }

  void start_write() {
    const auto& request_value = queue_.front().request;
    active_request_.emplace(request_value.method, request_value.target, request_value.version);
    active_request_->set(http::field::host, host_header());
    active_request_->set(http::field::user_agent, "algotradelib");
    active_request_->keep_alive(true);
    for (const auto& header : request_value.headers) {
      active_request_->set(header.first, header.second);
    }
    active_request_->body() = request_value.body;
    if (!request_value.body.empty()) {
      active_request_->prepare_payload();
    }

    state_ = State::WRITING;
    beast::get_lowest_layer(*stream_).expires_after(options_.timeout);
    http::async_write(
      *stream_, *active_request_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
        if (ec) {
          self->fail("write", ec);
          return;
        }
        self->start_read();
      }
    );
  }

  void start_read() {
    parser_.emplace();
    parser_->body_limit(options_.body_limit);
    state_ = State::READING;
    beast::get_lowest_layer(*stream_).expires_after(options_.timeout);
    http::async_read(
      *stream_, buffer_, *parser_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
        if (ec) {
          self->fail("read", ec);
          return;
        }
        auto response = self->parser_->release();
        const bool keep_alive = response.keep_alive() && !response.need_eof();
        self->parser_.reset();
        self->active_request_.reset();
        auto pending = std::move(self->queue_.front());
        self->queue_.pop_front();
        if (keep_alive) {
          self->state_ = State::READY;
        } else {
          self->disconnect();
        }
        self->complete(std::move(pending.handler), Result<HttpResponse>(std::move(response)));
        self->pump();
      }
    );
  }

  void fail(
    const char* stage,
    beast::error_code ec
  ) {
    disconnect();
    if (queue_.empty()) {
      return;
    }
    auto& pending = queue_.front();
    if (
      pending.request.method == http::verb::get && !pending.retried &&
      ec != net::error::operation_aborted
    ) {
      pending.retried = true;
      pump();
      return;
    }
    auto completed = std::move(pending);
    queue_.pop_front();
    complete(std::move(completed.handler), Result<HttpResponse>(transport_error(stage, ec)));
    pump();
  }

  void disconnect() noexcept {
    resolve_timer_.cancel();
    if (stream_) {
      close_ssl_stream(*stream_);
      stream_.reset();
    }
    buffer_.consume(buffer_.size());
    parser_.reset();
    active_request_.reset();
    state_ = State::DISCONNECTED;
  }

  std::string host_header() const { return make_host_header(host_, port_); }

  void complete(
    CompletionHandler handler,
    Result<HttpResponse> result
  ) {
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }
};

} // namespace network
} // namespace algotradelib
