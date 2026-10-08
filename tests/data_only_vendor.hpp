#pragma once

// A market-data vendor written the way a third party would: a blocking and an asynchronous client, each
// with a `BrokerTraits` built on `DataOnlyTraits`. Shared by test-data-only-traits.cpp and the probes
// that must not compile (compile-fail/test-data-only-refused-*.cpp).
#include <algotradelib/broker>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>

#include <memory>
#include <optional>

using namespace algotradelib;

namespace vendor {

/** A market-data vendor's identity, as an out-of-tree adapter declares one. */
struct Tag {
  /** Normalized name. */
  static constexpr std::string_view name = "vendor";
};

/** What the vendor needs to connect; nothing here. */
struct Config {};

/** One scripted series, as a vendor would answer any bar query. */
inline BarSeries scripted_bars(const InstrumentId& instrument) {
  BarSeries series;
  series.instrument_ = instrument;
  series.interval_ = BarInterval::ONE_DAY;
  series.stamp_ = BarStamp::OPEN;
  for (int day = 0; day < 3; ++day) {
    Bar bar;
    bar.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * (20000 + day));
    bar.open_ = Decimal64::parse("100");
    bar.high_ = Decimal64::parse("101");
    bar.low_ = Decimal64::parse("99");
    bar.close_ = Decimal64::parse("100.5");
    bar.volume_ = Decimal64::parse("1000");
    series.bars_.push_back(bar);
  }
  return series;
}

/** A blocking vendor client: bars and quotes, nothing about any account. */
struct BlockingClient {
  /** How many bar requests reached the client. */
  int bar_calls_{0};
};

/** An asynchronous vendor client: completes on its own executor, as a real client completes on its strand. */
class AsyncClient {
public:
  /** Constructs a client whose completions run on `executor`. */
  explicit AsyncClient(net::any_io_executor executor) : executor_(std::move(executor)) {}

  /** Answers a bar request with the scripted series. */
  template <typename Token>
  auto async_bars(BarQuery query, Token&& token) {
    return net::async_initiate<Token, void(Result<BarSeries>)>(
      [this, query = std::move(query)](auto&& handler) mutable {
        net::post(executor_, [handler = std::forward<decltype(handler)>(handler), query]() mutable {
          std::move(handler)(Result<BarSeries>(scripted_bars(query.instrument_)));
        });
      },
      token
    );
  }

  /** Answers a quote request. */
  template <typename Token>
  auto async_quote(InstrumentId instrument, Token&& token) {
    return net::async_initiate<Token, void(Result<QuoteSnapshot>)>(
      [this, instrument = std::move(instrument)](auto&& handler) mutable {
        net::post(executor_, [handler = std::forward<decltype(handler)>(handler), instrument]() mutable {
          QuoteSnapshot quote;
          quote.instrument_ = instrument;
          quote.last_price_ = Decimal64::parse("100.5");
          std::move(handler)(Result<QuoteSnapshot>(quote));
        });
      },
      token
    );
  }

private:
  net::any_io_executor executor_;
};

} // namespace vendor

/** The blocking vendor's traits: what it serves, and the account side from the base. */
template <>
struct algotradelib::BrokerTraits<vendor::BlockingClient> : algotradelib::DataOnlyTraits<vendor::Tag> {
  /** The adapted client. */
  using Client = vendor::BlockingClient;
  /** The vendor's configuration. */
  using Config = vendor::Config;
  /** Normalized name. */
  static constexpr std::string_view name = vendor::Tag::name;
  /** A blocking facade. */
  static constexpr bool blocking = true;

  /** Daily bars only. */
  static constexpr bool supports_interval(BarInterval interval) { return interval == BarInterval::ONE_DAY; }

  /** Refuses an interval it does not serve. */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    if (supports_interval(query.interval_)) {
      return std::nullopt;
    }
    return data_only::refusal();
  }

  /** Answers a quote. */
  static Result<QuoteSnapshot> quote(Client&, InstrumentId instrument) {
    QuoteSnapshot quote;
    quote.instrument_ = std::move(instrument);
    quote.last_price_ = Decimal64::parse("100.5");
    return Result<QuoteSnapshot>(quote);
  }

  /** Answers bars. */
  static Result<BarSeries> bars(Client& client, BarQuery query) {
    ++client.bar_calls_;
    return Result<BarSeries>(vendor::scripted_bars(query.instrument_));
  }
};

/** The asynchronous vendor's traits. */
template <>
struct algotradelib::BrokerTraits<vendor::AsyncClient> : algotradelib::DataOnlyTraits<vendor::Tag> {
  /** The adapted client. */
  using Client = vendor::AsyncClient;
  /** The vendor's configuration. */
  using Config = vendor::Config;
  /** Normalized name. */
  static constexpr std::string_view name = vendor::Tag::name;
  /** Completes through a token. */
  static constexpr bool blocking = false;

  /** Daily bars only. */
  static constexpr bool supports_interval(BarInterval interval) { return interval == BarInterval::ONE_DAY; }

  /** Refuses an interval it does not serve. */
  static std::optional<BrokerError> validate_bars(const BarQuery& query) {
    if (supports_interval(query.interval_)) {
      return std::nullopt;
    }
    return data_only::refusal();
  }

  /** Forwards a quote request. */
  template <typename Token>
  static auto quote(Client& client, InstrumentId instrument, Token&& token) {
    return client.async_quote(std::move(instrument), std::forward<Token>(token));
  }

  /** Forwards a bar request. */
  template <typename Token>
  static auto bars(Client& client, BarQuery query, Token&& token) {
    return client.async_bars(std::move(query), std::forward<Token>(token));
  }
};


namespace vendor {

/**
 * A stand-in for a broker's client, used only as the trading side of a `SessionSource` whose market data
 * comes from the vendor. It reuses the vendor's shape and declares itself not data-only, which hides the
 * base's flag; nothing ever asks it for an order.
 */
struct TradingStandIn : BlockingClient {};

} // namespace vendor

/** The stand-in's traits: the blocking vendor's, minus the data-only mark. */
template <>
struct algotradelib::BrokerTraits<vendor::TradingStandIn> : algotradelib::BrokerTraits<vendor::BlockingClient> {
  /** The adapted client. */
  using Client = vendor::TradingStandIn;
  /** Not a market-data source, for this test's purpose. */
  static constexpr bool data_only = false;

  /** Answers a quote, as the vendor's. */
  static Result<QuoteSnapshot> quote(Client& client, InstrumentId instrument) {
    return BrokerTraits<vendor::BlockingClient>::quote(client, std::move(instrument));
  }

  /** Answers bars, as the vendor's. */
  static Result<BarSeries> bars(Client& client, BarQuery query) {
    return BrokerTraits<vendor::BlockingClient>::bars(client, std::move(query));
  }
};
