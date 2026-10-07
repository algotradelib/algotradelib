// One strategy, compiled for both brokers from this one source, bound at a single line in `main`.
//
// The point of the whole branch, demonstrated: `trade()` below names no broker. It takes a session
// by template parameter, and everything broker-specific is either behind a capability gate or
// inside the adapter. Swapping KIS for Toss changes which overload `main` calls, nothing else.
//
// **It can place real orders.** Three modes, and only the last one trades:
//
//   --dry-run     no session at all, bars built in code, no credentials needed
//   (default)     a real session for market data, but the send step goes to an executor that holds
//                 no session, so there is nothing for an order to be sent through
//   --live        the send step goes to the bound session
//
// Run it as:
//
//   example-strategy-live --dry-run
//   example-strategy-live kis 005930 [--live]
//   example-strategy-live tossinvest 005930 [account-sequence] [--live]
//
// Credentials come from the same environment variables the other examples document. Without
// `--live` a bound session still only *reads*: it fetches bars and prints what it would do.
//
// **On interrupting it.** A `Ctrl+C` stops this program, and that is all it does. An order the broker
// already accepted stays live on the exchange, which knows nothing about a process that died -- and an
// order *in flight* at that moment never reaches the classification that would have recorded its
// outcome as unknown, because the default signal handler terminates immediately. So after
// interrupting a `--live` run, read the broker's order list before running it again. This example
// places at most one order per run, which bounds what can be in doubt to that one.
//
// **On running it again.** Each run reads the open orders before the holdings, so a buy still waiting
// to fill counts toward the one-share limit. A buy that has filled but does not show in the holdings
// yet is in neither, and is bought again: wait until the holdings show it. Never run two at once.

#include "kis_credentials.hpp"

#include <algotradelib/kis>
#include <algotradelib/live>
#include <algotradelib/vanilla>
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

/** @brief How the example is being run, which decides where the send step goes. */
enum class Mode {
  /**
   * @brief Read real market data, run the real pipeline, and send through an executor with no session.
   *
   * Not a check that declines to send: there is nothing to send *through*.
   */
  OBSERVE,
  /** @brief Send what the strategy decides, through the bound session. */
  TRADE,
};

/** @brief The strategy this example runs, chosen because it needs no series and so builds everywhere. */
vanilla::BuyAndHold strategy_for(const InstrumentId& instrument) {
  vanilla::BuyAndHold holding;
  holding.params_.instrument_ = instrument;
  holding.params_.quantity_ = Decimal64::parse("1");
  return holding;
}

/** @brief Limits an example should not run without. Small on purpose. */
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
 * @brief Runs the strategy against `session`, and names no broker anywhere in its body.
 *
 * This is the portability claim in executable form. The only broker-shaped thing here is the
 * template parameter; the capability gate below is how a broker-specific feature is reached when one
 * is wanted, and it compiles to nothing on the broker that lacks it.
 */
/** @brief What the session was read for, so the pipeline can be driven without reading it twice. */
struct MarketState {
  algotradelib::Instrument reference_;
  algotradelib::BarSeries bars_;
  algotradelib::HoldingsSnapshot holdings_;
};

/**
 * @brief Runs the pipeline over `state` with an already-bound runner.
 *
 * Names no broker: the tag arrives as a template parameter and the runner was bound by
 * `live::bind` or `live::bind_observer`, which read the broker's capabilities and tag off the
 * session. That is the portability claim in executable form -- and it is also how observing works.
 */
template <typename Tag, typename R>
int drive(
  const InstrumentId& instrument, const MarketState& state,
  const algotradelib::WorkingOrders<Tag>& open_orders, R live_runner
) {
  live_runner.context().set_instrument(state.reference_);
  // A single pass driven by hand, so the phase is this program's to state: only
  // `runner::PollingLoop` computes one, from a session window and a polled calendar. A program that
  // runs continuously uses the loop and does not set this at all -- see `example-live-loop.cpp`.
  live_runner.context().set_phase(strategy::MarketPhase::OPEN);

  strategy::Portfolio<Tag> portfolio;
  portfolio.positions_ = state.holdings_.positions_;
  // **Stamped with when the broker's answer arrived, and that is not optional.** A runner bound to a
  // session refuses to size an order against a portfolio it cannot date: an unstamped one reads as
  // an account nobody has read, and an empty account is the permissive answer to every ceiling.
  // Copying the rows without the timestamp is the mistake this line exists to prevent.
  portfolio.as_of_ = state.holdings_.received_at_;
  // **And the orders still open, with when they were listed.** Without them a buy that is still
  // waiting to fill is invisible, and the runner buys again past `max_position_quantity_`.
  portfolio.working_ = open_orders.orders_;
  portfolio.working_as_of_ = open_orders.received_at_;
  // **And with what the broker's sellable figures are evidence of, for the same reason.** A guard
  // enforcing `enforce_sellable_quantity_` refuses a figure whose coverage nobody stated, so a hand-
  // built portfolio that copies the rows without this refuses every sell -- and one that invented an
  // enforceable basis would measure a sale against a number it had not characterized. Copy it; do not
  // construct it. This example does not enable that limit, and the line is here because the example is
  // a pattern people copy into programs that do.
  portfolio.sellable_basis_ = state.holdings_.sellable_basis_;
  live_runner.context().set_portfolio(std::move(portfolio));

  const auto& last = state.bars_.bars_.back();
  live_runner.context().set_last_price(instrument, last.close_);
  const auto report = live_runner.feed(
    strategy::make_bar_event(instrument, state.bars_.interval_, state.bars_.stamp_, last)
  );

  for (const auto& order : report.would_place_) {
    std::cout << "order: " << (order.side_ == OrderSide::BUY ? "BUY " : "SELL") << ' '
              << order.quantity_.str() << " at "
              << (order.price_ ? order.price_->str() : std::string("market")) << '\n';
  }
  for (const auto& refusal : report.refused_) {
    std::cout << "refused by a guard: " << strategy::guard_reason_name(refusal.reason_) << '\n';
  }
  for (const auto& rejection : report.rejected_) {
    // Not always "could not build": the bucket also holds a broker's refusal and one this binding
    // made without asking a broker, and the reason on the entry is what says which.
    std::cout << "not placed: " << strategy::reject_reason_name(rejection.reason_) << '\n';
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

/** @brief Warns, after a live run, about what an interruption would have left unrecorded. */
void note_interruption_hazard() {
  std::cout
    << "\nnote: interrupting a live run does not withdraw an accepted order, and an order in\n"
    << "      flight at that moment leaves no record that its outcome is unknown. Read the\n"
    << "      broker's order list before running again, and wait until the holdings show any\n"
    << "      fill: a filled buy they do not show yet is bought again.\n";
}

/**
 * @brief Reads what the strategy needs from `session`, then drives the pipeline over it.
 *
 * **How observing works, and why it is not a flag.** Both modes run the identical pipeline over
 * identical real market data; they differ only in which executor the send step goes to. Trading
 * supplies `SessionExecutor`, which calls the broker. Observing supplies `RecordingExecutor`, which
 * holds no session at all -- so there is nothing for an order to be sent *through*, rather than a
 * check that decides not to send it.
 *
 * That is better than the obvious alternative of halting the guard. A halt would report every
 * decision as `refused_` with `TRADING_HALTED`, which reads like a failure when it is the intended
 * mode, and it would leave the send path present and merely unreached.
 */
template <typename Session>
int trade(Session& session, const InstrumentId& instrument, Mode mode) {
  using Caps = typename Session::Caps;
  using Tag = typename Session::BrokerTag;

  std::cout << "broker: " << Tag::name << "   instrument: " << instrument.symbol_
            << "   mode: " << (mode == Mode::TRADE ? "TRADE" : "observe") << "\n\n";

  if constexpr (Caps::has_client_order_id) {
    std::cout << "this broker accepts an idempotency key, so a retry could be made safe\n";
  } else {
    std::cout << "this broker accepts no idempotency key, so no order is ever replayed\n";
  }

  MarketState state;

  // Reference data first: without a tick grid and a lot size there is nothing reconciliation can
  // build, and it would refuse every order by name.
  auto reference = session.instrument(instrument);
  if (!reference) {
    std::cerr << "could not read reference data: " << reference.error().message_ << '\n';
    return 1;
  }
  state.reference_ = std::move(reference.value());

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
  state.bars_ = std::move(bars.value());

  // **Open orders before holdings.** An order that fills between the two reads then shows in the
  // holdings. Read the other way round, it would be in neither.
  algotradelib::WorkingOrders<Tag> open_orders;
  if constexpr (Caps::has_working_orders) {
    auto listed = session.working_orders();
    if (!listed) {
      std::cerr << "could not read open orders: " << listed.error().message_ << '\n';
      return 1;
    }
    open_orders = std::move(listed.value());
  } else {
    if (mode == Mode::TRADE) {
      std::cerr << "this broker lists no open orders, so a live run could not see an earlier order "
                   "still waiting to fill; use example-live-program\n";
      return 1;
    }
  }

  auto holdings = session.holdings();
  if (!holdings) {
    std::cerr << "could not read holdings: " << holdings.error().message_ << '\n';
    return 1;
  }
  state.holdings_ = std::move(holdings.value());

  // One line per mode. The factories read the capabilities and the tag off the session, so the two
  // runners differ in exactly one thing -- where the send goes -- and cannot differ in any other.
  if (mode == Mode::TRADE) {
    const auto status = drive<Tag>(
      instrument, state, open_orders, live::bind(session, strategy_for(instrument), example_limits())
    );
    note_interruption_hazard();
    return status;
  }
  return drive<Tag>(
    instrument, state, open_orders, live::bind_observer(session, strategy_for(instrument), example_limits())
  );
}

int trade_kis(ssl::context& tls, const std::string& symbol, Mode mode) {
  namespace kis = algotradelib::kis;

  kis::KisConfig config;
  kis::KisAccount account;
  kis_credentials::load(kis_credentials::environment_directory(), config, &account, std::cout);

  auto client =
    std::make_shared<kis::KisBlockingClient>(tls, std::move(config), kis_credentials::paced_options());
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

/** @brief Runs the same strategy with no broker at all, which needs no credentials. */
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
      << "Without --live the session is used for market data only: the send step goes to an\n"
      << "executor that holds no session, so there is nothing for an order to be sent through.\n";
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
