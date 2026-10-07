// One market-data report written once and compiled for every supported broker.
//
// `report()` names no vendor type. Everything that differs between the two brokers
// appears as an `if constexpr` on a capability, which is what the port is for: the
// common questions are asked the same way, and each broker's own strengths are
// reached deliberately rather than faked for the other.
//
// Run it as:
//
//   example-portable-market-data kis 005930
//   example-portable-market-data tossinvest 005930 [account-sequence]
//
// Credentials come from the same environment variables the other examples document.
// It reads market data only and can place no order.

#include "kis_credentials.hpp"

#include <algotradelib/kis>
#include <algotradelib/tossinvest>

#include <boost/asio/ssl/context.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssl = boost::asio::ssl;

namespace {

using algotradelib::BarInterval;
using algotradelib::BarQuery;
using algotradelib::BarSeries;
using algotradelib::BlockingSession;
using algotradelib::BrokerCaps;
using algotradelib::BrokerError;
using algotradelib::Country;
using algotradelib::DepthSnapshot;
using algotradelib::Instrument;
using algotradelib::InstrumentId;
using algotradelib::OrderSide;
using algotradelib::PriceLimits;
using algotradelib::StatedDate;
using algotradelib::TradingCalendar;
using algotradelib::Venue;
using algotradelib::bar_interval_name;

/** @brief Prints a structured broker or transport failure. */
void print_error(std::string_view operation, const BrokerError& error) {
  std::cerr << "  " << operation << " failed";
  if (!error.broker_code_.empty()) {
    std::cerr << " [" << error.broker_code_ << ']';
  }
  if (!error.message_.empty()) {
    std::cerr << ": " << error.message_;
  }
  std::cerr << '\n';
}

/** @brief Prints the oldest and newest bar, and the convention their stamps follow. */
void print_bars(const BarSeries& series) {
  std::cout << "bars: " << series.bars_.size() << ' '
    << bar_interval_name(series.interval_) << (series.adjusted_ ? " adjusted" : " raw")
    << ' ' << series.currency_.str() << ", stamped at each period's "
    << (series.stamp_ == algotradelib::BarStamp::OPEN ? "open" : "close") << '\n';
  if (series.bars_.empty()) {
    return;
  }
  std::cout << "  ascending: " << (series.ascending() ? "yes" : "no") << '\n';
  for (const auto* bar : {&series.bars_.front(), &series.bars_.back()}) {
    std::cout << "  " << (bar == &series.bars_.front() ? "oldest" : "newest")
      << " close=" << bar->close_.str() << " volume=" << bar->volume_.str()
      << " open=" << (bar->open_ ? bar->open_->str() : std::string("not reported")) << '\n';
  }
  if (series.next_cursor_) {
    std::cout << "  more history available from " << *series.next_cursor_ << '\n';
  }
}

/** @brief Prints the top of the book and what it implies. */
void print_depth(const DepthSnapshot& book) {
  std::cout << "book: " << book.bids_.size() << " bids, " << book.asks_.size() << " asks\n";
  const auto bid = book.best_bid();
  const auto ask = book.best_ask();
  std::cout << "  best bid: " << (bid ? bid->price_.str() : std::string("none"))
    << "  best ask: " << (ask ? ask->price_.str() : std::string("none")) << '\n';
  const auto spread = book.spread();
  const auto mid = book.mid();
  std::cout << "  spread: " << (spread ? spread->str() : std::string("not both sides"))
    << "  mid: " << (mid ? mid->str() : std::string("not both sides")) << '\n';
}

/** @brief Prints the price bounds, distinguishing absent from zero. */
void print_price_limits(const PriceLimits& limits) {
  std::cout << "price bounds: "
    << (limits.lower_ ? limits.lower_->str() : std::string("no lower limit")) << " .. "
    << (limits.upper_ ? limits.upper_->str() : std::string("no upper limit")) << ' '
    << limits.currency_.str() << '\n';
}

/** @brief Prints reference data, and what the library's own price grid says about it. */
void print_instrument(const Instrument& instrument) {
  std::cout << "instrument: " << instrument.name_;
  if (!instrument.english_name_.empty()) {
    std::cout << " (" << instrument.english_name_ << ')';
  }
  std::cout << "\n  market: " << instrument.segment_.str();
  if (!instrument.isin_.empty()) {
    std::cout << "  isin: " << instrument.isin_;
  }
  switch (instrument.listed_on_.state()) {
    case StatedDate::State::STATED: {
      const auto& listed = instrument.listed_on_.date();
      std::cout << "  listed: " << listed.year_ << '-' << listed.month_ << '-' << listed.day_;
      break;
    }
    case StatedDate::State::UNREADABLE:
      // The broker stated a date nobody can read, which is not the same as stating none.
      std::cout << "  listed: (unreadable)";
      break;
    case StatedDate::State::NOT_STATED:
      break;
  }
  std::cout << '\n';
  std::cout << "  price grid: " << instrument.price_rule_.source_ << '\n';
}

/** @brief Prints which of the reported dates the market actually opens on. */
void print_calendar(const TradingCalendar& calendar) {
  std::cout << "calendar: " << calendar.days_.size() << " date(s) covered\n";
  for (const auto& day : calendar.days_) {
    std::cout << "  " << day.date_.year_ << '-' << day.date_.month_ << '-' << day.date_.day_
      << (day.open_ ? "  market opens" : "  market closed") << '\n';
  }
}

/**
 * @brief Reports one instrument's market data through the port only.
 *
 * The capability gates below are the entire broker-specific surface, and each says
 * what the difference *is* rather than naming a broker.
 */
template <typename Client>
int report(BlockingSession<Client>& session, const InstrumentId& subject) {
  using Caps = BrokerCaps<Client>;

  std::cout << "broker: " << session.broker_name() << "\n\n";

  algotradelib::PriceRule reference_rule;
  if (auto reference = session.instrument(subject)) {
    print_instrument(reference.value());
    reference_rule = reference.value().price_rule_;
  } else {
    print_error("reference data", reference.error());
  }

  BarQuery daily;
  daily.instrument_ = subject;
  daily.interval_ = BarInterval::ONE_DAY;
  daily.count_ = 120;
  if (auto bars = session.bars(daily)) {
    print_bars(bars.value());
  } else {
    print_error("daily bars", bars.error());
  }

  if constexpr (Caps::supports_interval(BarInterval::ONE_WEEK)) {
    // Only one broker publishes bars longer than a day. They are not aggregated for
    // the other, because resampling answers a different question from the one asked.
    BarQuery weekly = daily;
    weekly.interval_ = BarInterval::ONE_WEEK;
    weekly.count_ = 12;
    if (auto bars = session.bars(weekly)) {
      print_bars(bars.value());
    } else {
      print_error("weekly bars", bars.error());
    }
  } else {
    std::cout << "bars: this broker publishes no period longer than a day\n";
  }

  if constexpr (Caps::has_depth) {
    if (auto book = session.depth(subject)) {
      print_depth(book.value());

      // A computed price has to sit on the exchange's grid, and the grid is the
      // library's own because no broker publishes one.
      if (const auto mid = book.value().mid()) {
        // The grid comes from the instrument, because it depends on what the
        // instrument is: KRX prices funds and notes on a ladder this library does not
        // carry, and those refuse to round rather than rounding onto the wrong one.
        const auto rule = reference_rule;
        const auto buy = rule.round_to_tick(*mid, OrderSide::BUY);
        std::cout << "  a buy at the midpoint would rest at "
          << (buy ? buy->str() : std::string("no grid known for this instrument")) << '\n';
      }
    } else {
      print_error("order book", book.error());
    }
  }

  if constexpr (Caps::has_price_limits) {
    if (auto limits = session.price_limits(subject)) {
      print_price_limits(limits.value());
    } else {
      print_error("price bounds", limits.error());
    }
  }

  if constexpr (Caps::has_trade_prints) {
    if (auto prints = session.trades(subject, 5)) {
      std::cout << "recent executions: " << prints.value().size() << '\n';
      for (const auto& print : prints.value()) {
        std::cout << "  " << print.price_.str() << " x " << print.quantity_.str() << '\n';
      }
    } else {
      print_error("recent executions", prints.error());
    }
  }

  if constexpr (Caps::has_batch_prices) {
    // One request for several instruments, where the broker offers it. Looping
    // single quotes instead would spend several times the request budget.
    if (auto quotes = session.prices({subject})) {
      std::cout << "batch quotes: " << quotes.value().size() << '\n';
    } else {
      print_error("batch quotes", quotes.error());
    }
  } else {
    std::cout << "batch quotes: this broker quotes one instrument per request\n";
  }

  if constexpr (Caps::has_calendar) {
    if (auto calendar = session.calendar(subject.country_)) {
      print_calendar(calendar.value());
      if constexpr (!Caps::has_calendar_range) {
        std::cout << "  one request covers the requested day and the ones either side\n";
      }
    } else {
      print_error("calendar", calendar.error());
    }
  }

  if constexpr (Caps::has_calendar_sessions) {
    std::cout << "sessions: this broker publishes intraday session windows; read them "
      << "through session.client()\n";
  }

  return 0;
}

/** @brief Builds a KIS session from the environment and reports through it. */
int report_kis(ssl::context& tls, const std::string& symbol) {
  namespace kis = algotradelib::kis;

  kis::KisConfig config;
  kis::KisAccount account;
  kis_credentials::load(kis_credentials::environment_directory(), config, &account, std::cout);

  auto client =
    std::make_shared<kis::KisBlockingClient>(tls, std::move(config), kis_credentials::paced_options());
  BlockingSession<kis::KisBlockingClient> session(std::move(client), std::move(account));
  return report(session, InstrumentId{Country::KR, Venue::COMBINED, symbol});
}

/** @brief Builds a Toss session, discovering the account when none is supplied. */
int report_tossinvest(ssl::context& tls, const std::string& symbol, const char* sequence) {
  namespace toss = algotradelib::tossinvest;

  toss::TossConfig config;
  config.from_env();
  auto client = std::make_shared<toss::TossBlockingClient>(tls, std::move(config));

  std::int64_t selected = 0;
  if (sequence) {
    selected = std::strtoll(sequence, nullptr, 10);
  } else {
    auto accounts = algotradelib::list_accounts(*client);
    if (!accounts) {
      print_error("account discovery", accounts.error());
      return 1;
    }
    if (accounts.value().empty()) {
      std::cerr << "no Toss accounts are available for these credentials\n";
      return 1;
    }
    selected = accounts.value().front().selector_;
  }

  BlockingSession<toss::TossBlockingClient> session(std::move(client), selected);
  return report(session, InstrumentId{Country::KR, Venue::COMBINED, symbol});
}

} // namespace

int main(int argc, char** argv) {
  const std::string broker = argc > 1 ? argv[1] : "";
  if (broker != "kis" && broker != "tossinvest") {
    std::cerr
      << "usage: example-portable-market-data <kis|tossinvest> [symbol] [account-sequence]\n";
    return 2;
  }
  const std::string symbol = argc > 2 ? argv[2] : "005930";

  try {
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    if (broker == "kis") {
      return report_kis(tls, symbol);
    }
    return report_tossinvest(tls, symbol, argc > 3 ? argv[3] : nullptr);
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
