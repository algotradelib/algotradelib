// One strategy, compiled for both brokers from this one source, bound at a single line in `main`.
//
// The point of the whole branch, demonstrated: `trade()` below names no broker. It takes a session
// by template parameter, and everything broker-specific is either behind a capability gate or
// inside the adapter. Swapping KIS for Toss changes which overload `main` calls, nothing else.
//
// **It can place real orders.** Run it with `--dry-run` to see every decision with nothing sent,
// which needs no credentials at all. Without that flag it binds a live session and will trade.
//
// Run it as:
//
//   example-strategy-live --dry-run
//   example-strategy-live kis 005930 [--live]
//   example-strategy-live tossinvest 005930 [account-sequence] [--live]
//
// Credentials come from the same environment variables the other examples document. Without
// `--live` a bound session still only *reads*: it fetches bars and prints what it would do.

#include <algotradelib/kis>
#include <algotradelib/live>
#include <algotradelib/strategies>
#include <algotradelib/tossinvest>

#include <boost/asio/ssl/context.hpp>

#include <chrono>
#include <cstddef>
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

namespace strategy = algotradelib::strategy;
namespace runner = algotradelib::runner;
namespace live = algotradelib::live;
namespace vanilla = algotradelib::strategy::vanilla;

using algotradelib::BarInterval;
using algotradelib::BarQuery;
using algotradelib::BlockingSession;
using algotradelib::Country;
using algotradelib::Decimal64;
using algotradelib::InstrumentId;
using algotradelib::OrderSide;
using algotradelib::Venue;

/** How the example is being run, which decides whether anything may be sent. */
enum class Mode {
  /** Read market data, print decisions, send nothing. */
  OBSERVE,
  /** Send what the strategy decides. */
  TRADE,
};

/** The strategy this example runs, chosen because it needs no series and so builds everywhere. */
vanilla::BuyAndHold strategy_for(const InstrumentId& instrument) {
  vanilla::BuyAndHold holding;
  holding.params_.instrument_ = instrument;
  holding.params_.quantity_ = Decimal64::parse("1");
  return holding;
}

/** Limits an example should not run without. Small on purpose. */
strategy::RiskLimits example_limits() {
  strategy::RiskLimits limits;
  // One share of a Korean blue chip is well under this, so a configuration mistake that asked for
  // a hundred is refused rather than filled.
  limits.max_order_notional_ = Decimal64::parse("500000");
  limits.max_position_quantity_ = Decimal64::parse("1");
  limits.max_orders_per_window_ = 2;
  return limits;
}

/**
 * Runs the strategy against `session`, and names no broker anywhere in its body.
 *
 * This is the portability claim in executable form. The only broker-shaped thing here is the
 * template parameter; the capability gate below is how a broker-specific feature is reached when one
 * is wanted, and it compiles to nothing on the broker that lacks it.
 */
template <typename Session>
int trade(Session& session, const InstrumentId& instrument, Mode mode) {
  using Caps = typename Session::Caps;
  using Tag = typename Session::BrokerTag;
  using Executor = live::SessionExecutor<Session>;

  std::cout << "broker: " << Tag::name << "   instrument: " << instrument.symbol_
            << "   mode: " << (mode == Mode::TRADE ? "TRADE" : "observe") << "\n\n";

  if constexpr (Caps::has_client_order_id) {
    std::cout << "this broker accepts an idempotency key, so a retry could be made safe\n";
  } else {
    std::cout << "this broker accepts no idempotency key, so no order is ever replayed\n";
  }

  // Reference data first: without a tick grid and a lot size there is nothing reconciliation can
  // build, and it would refuse every order by name.
  auto reference = session.instrument(instrument);
  if (!reference) {
    std::cerr << "could not read reference data: " << reference.error().message_ << '\n';
    return 1;
  }

  BarQuery query;
  query.instrument_ = instrument;
  query.interval_ = BarInterval::ONE_DAY;
  query.count_ = 30;
  auto bars = session.bars(query);
  if (!bars) {
    std::cerr << "could not read bars: " << bars.error().message_ << '\n';
    return 1;
  }
  if (bars.value().bars_.empty()) {
    std::cerr << "the broker returned no bars\n";
    return 1;
  }

  auto holdings = session.holdings();
  if (!holdings) {
    std::cerr << "could not read holdings: " << holdings.error().message_ << '\n';
    return 1;
  }

  Executor executor(session);
  runner::Runner<vanilla::BuyAndHold, Caps, Tag, Executor> live_runner(
    strategy_for(instrument), example_limits(), 512, Executor(session)
  );
  live_runner.context().set_instrument(reference.value());
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);

  strategy::Portfolio<Tag> portfolio;
  portfolio.positions_ = holdings.value().positions_;
  live_runner.context().set_portfolio(std::move(portfolio));

  // Observing is enforced by halting the guard, not by skipping the send. The pipeline then runs
  // exactly as it would live, and what stops the order is the same kill switch an operator uses.
  if (mode == Mode::OBSERVE) {
    live_runner.guard().halt();
  }

  const auto& series = bars.value();
  const auto& last = series.bars_.back();
  live_runner.context().set_last_price(instrument, last.close_);
  const auto report = live_runner.feed(
    strategy::make_bar_event(instrument, series.interval_, series.stamp_, last)
  );

  for (const auto& order : report.would_place_) {
    std::cout << "sent: " << (order.side_ == OrderSide::BUY ? "BUY " : "SELL") << ' '
              << order.quantity_.str() << " at "
              << (order.price_ ? order.price_->str() : std::string("market")) << '\n';
  }
  for (const auto& refusal : report.refused_) {
    std::cout << "refused by a guard: " << strategy::guard_reason_name(refusal.reason_) << '\n';
  }
  for (const auto& rejection : report.rejected_) {
    std::cout << "could not build an order: "
              << strategy::reject_reason_name(rejection.reason_) << '\n';
  }
  for (const auto& held : report.withheld_) {
    std::cout << "withheld pending reconciliation: " << held.detail_ << '\n';
  }
  for (const auto& order : report.indeterminate_) {
    std::cout << "UNKNOWN OUTCOME for " << order.instrument_.symbol_
              << ": read the broker's order list before trading it again\n";
  }
  if (report.quiet()) {
    std::cout << "the strategy asked for nothing: the target is already met\n";
  }
  if (!live_runner.ledger().empty()) {
    std::cout << "\nreconciliation required before this instrument may be traded again\n";
    return 1;
  }
  return 0;
}

int trade_kis(ssl::context& tls, const std::string& symbol, Mode mode) {
  namespace kis = algotradelib::kis;

  kis::KisConfig config;
  config.from_env();
  kis::KisAccount account;
  account.from_env();

  auto client = std::make_shared<kis::KisBlockingClient>(tls, std::move(config));
  BlockingSession<kis::KisBlockingClient> session(std::move(client), std::move(account));
  return trade(session, InstrumentId{Country::KR, Venue::COMBINED, symbol}, mode);
}

int trade_tossinvest(
  ssl::context& tls,
  const std::string& symbol,
  const char* sequence,
  Mode mode
) {
  namespace toss = algotradelib::tossinvest;

  toss::TossConfig config;
  config.from_env();
  auto client = std::make_shared<toss::TossBlockingClient>(tls, std::move(config));

  // Toss selects an account by sequence number and publishes the list, so an unstated one is
  // discovered rather than guessed. KIS takes the account from the environment instead, which is
  // exactly the kind of difference the port leaves to the adapter.
  std::int64_t selected = 0;
  if (sequence != nullptr) {
    selected = std::strtoll(sequence, nullptr, 10);
  } else {
    auto accounts = algotradelib::list_accounts(*client);
    if (!accounts) {
      std::cerr << "account discovery failed: " << accounts.error().message_ << '\n';
      return 1;
    }
    if (accounts.value().empty()) {
      std::cerr << "no Toss accounts are available for these credentials\n";
      return 1;
    }
    selected = accounts.value().front().selector_;
  }

  BlockingSession<toss::TossBlockingClient> session(std::move(client), selected);
  return trade(session, InstrumentId{Country::KR, Venue::COMBINED, symbol}, mode);
}

/** Runs the same strategy with no broker at all, which needs no credentials. */
int trade_dry(const std::string& symbol) {
  const InstrumentId instrument{Country::KR, Venue::COMBINED, symbol};
  std::cout << "no broker bound: nothing in this path can reach a market\n\n";

  runner::DryRunner<vanilla::BuyAndHold> dry(strategy_for(instrument), example_limits());
  algotradelib::Instrument reference;
  reference.id_ = instrument;
  reference.type_ = algotradelib::SecurityType::STOCK;
  reference.currency_ = algotradelib::Currency{"KRW"};
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = algotradelib::PriceRule::krx_equity();
  dry.context().set_instrument(reference);
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  dry.context().set_last_price(instrument, Decimal64::parse("70000"));

  algotradelib::Bar bar;
  bar.timestamp_ = std::chrono::system_clock::now();
  bar.high_ = Decimal64::parse("70500");
  bar.low_ = Decimal64::parse("69400");
  bar.close_ = Decimal64::parse("70000");
  bar.volume_ = Decimal64::parse("1200000");

  const auto report = dry.feed(
    strategy::make_bar_event(
      instrument, BarInterval::ONE_DAY, algotradelib::BarStamp::CLOSE, bar
    )
  );
  for (const auto& order : report.would_place_) {
    std::cout << "would send: " << (order.side_ == OrderSide::BUY ? "BUY " : "SELL") << ' '
              << order.quantity_.str() << " at market\n";
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments(argv + 1, argv + argc);
  Mode mode = Mode::OBSERVE;
  bool dry = false;
  std::vector<std::string> positional;
  for (const auto& argument : arguments) {
    if (argument == "--live") {
      mode = Mode::TRADE;
    } else if (argument == "--dry-run") {
      dry = true;
    } else {
      positional.push_back(argument);
    }
  }

  const std::string symbol = positional.size() > 1 ? positional[1] : "005930";
  if (dry) {
    return trade_dry(symbol);
  }

  const std::string broker = positional.empty() ? "" : positional[0];
  if (broker != "kis" && broker != "tossinvest") {
    std::cerr
      << "usage: example-strategy-live --dry-run\n"
      << "       example-strategy-live <kis|tossinvest> [symbol] [account-sequence] [--live]\n\n"
      << "Without --live a bound session only reads and prints what it would do.\n";
    return 2;
  }

  try {
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    if (broker == "kis") {
      return trade_kis(tls, symbol, mode);
    }
    return trade_tossinvest(
      tls, symbol, positional.size() > 2 ? positional[2].c_str() : nullptr, mode
    );
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
