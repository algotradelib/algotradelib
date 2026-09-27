// Backtest a strategy on bars fetched from a real broker.
//
// Two phases, deliberately separated: **read, then replay**. The session is used once, to fetch bars
// and reference data, and then closed out of the picture — the replay itself touches nothing. That
// separation is the point. A backtest that held a session open could accidentally reach for data
// mid-replay, which is how a backtest ends up using information the strategy could not have had.
//
// It places no orders. There is no code path here that could: the engine's executor queues fills and
// has no session at all.
//
// Run it as:
//
//   example-backtest-broker kis 005930
//   example-backtest-broker tossinvest 005930 [account-sequence]
//
// Credentials come from the same environment variables the other examples document. What you can fetch
// bounds what you can test: KIS serves about 5,000 daily bars per request, which is roughly twenty
// years of KRX trading days, but serves minute bars one trading day at a time.

#include <algotradelib/backtest>
#include <algotradelib/kis>
#include <algotradelib/tossinvest>
#include <algotradelib/vanilla>

#include <boost/asio/ssl/context.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <memory>
#include <string>
#include <utility>

namespace ssl = boost::asio::ssl;

namespace {

namespace strategy = algotradelib::strategy;
namespace backtest = algotradelib::backtest;
namespace vanilla = algotradelib::strategy::vanilla;
namespace math = algotradelib::math;

/**
 * Returns a monetary figure, or why there is none.
 *
 * A total that spans currencies is absent rather than summed, and so is one from a replay that
 * executed nothing -- neither is a charge of zero, and printing `0` would say it was.
 */
std::string money_or_absent(const std::optional<algotradelib::Decimal64>& amount) {
  return amount ? amount->str() : std::string("(not a single currency)");
}


using algotradelib::BarInterval;
using algotradelib::BarQuery;
using algotradelib::BarSeries;
using algotradelib::BlockingSession;
using algotradelib::Country;
using algotradelib::Currency;
using algotradelib::Decimal64;
using algotradelib::Instrument;
using algotradelib::InstrumentId;
using algotradelib::Venue;

/** What the session was read for. Nothing else is needed, and nothing else is kept. */
struct FetchedHistory {
  Instrument reference_;
  BarSeries bars_;
};

/**
 * Reads reference data and bars, and names no broker in doing it.
 *
 * The same portability the strategy layer has: the session arrives as a template parameter and the body
 * asks only questions both brokers answer.
 */
template <typename Session>
std::optional<FetchedHistory> read_history(
  Session& session,
  const InstrumentId& instrument,
  std::size_t count
) {
  auto reference = session.instrument(instrument);
  if (!reference) {
    std::cerr << "could not read reference data: " << reference.error().message_ << '\n';
    return std::nullopt;
  }

  BarQuery query;
  query.instrument_ = instrument;
  query.interval_ = BarInterval::ONE_DAY;
  query.count_ = count;
  // Adjusted, and this matters more than it looks: unadjusted prices invent a signal at every split
  // and every dividend, so a backtest on them measures corporate actions rather than a strategy.
  query.adjusted_ = true;

  auto bars = session.bars(query);
  if (!bars) {
    std::cerr << "could not read bars: " << bars.error().message_ << '\n';
    return std::nullopt;
  }
  if (bars.value().bars_.size() < 2) {
    std::cerr << "the broker returned too few bars to replay\n";
    return std::nullopt;
  }

  FetchedHistory history;
  history.reference_ = std::move(reference.value());
  history.bars_ = std::move(bars.value());
  return history;
}

#if defined(ATL_HAS_EIGEN)
/** Replays `history` through one strategy and prints the report. */
template <typename Strategy>
void replay_and_report(const char* title, Strategy instance, const FetchedHistory& history) {
  backtest::ReplayConfig config;
  config.cash_ = Decimal64::parse("100000000");
  config.instruments_ = {history.reference_};
  config.currency_ = history.bars_.currency_;
  config.series_capacity_ = 512;
  config.limits_.permitted_phases_ = std::nullopt;

  backtest::Backtester<Strategy> engine(
    std::move(instance),
    std::move(config),
    backtest::NextBarFillModel(
      backtest::CommissionModel::of(Decimal64::parse("0.00015"), Decimal64::parse("100")),
      backtest::SlippageModel::of_ticks(1)
    )
  );

  for (const auto& bar : history.bars_.bars_) {
    engine.feed(
      strategy::make_bar_event(
        history.bars_.instrument_, history.bars_.interval_, history.bars_.stamp_, bar
      )
    );
  }
  engine.finish();

  const auto periods =
    backtest::periods_per_year(history.bars_.interval_, strategy::SessionWindow::krx(), 245.0);
  const auto report = backtest::summarize(engine.result(), engine.fills(), periods);

  const auto show = [](math::Scalar value, bool as_percent) {
    std::ostringstream text;
    if (!math::observed(value)) {
      text << (math::is_undefined(value) ? "undefined" : "--");
      return text.str();
    }
    text << std::fixed << std::setprecision(as_percent ? 2 : 3)
         << (as_percent ? value * 100.0 : value) << (as_percent ? " %" : "");
    return text.str();
  };

  std::cout << title << "\n"
            << "  bars            " << report.bars_ << "\n"
            << "  executions      " << report.executions_ << "\n"
            << "  total return    " << show(report.total_return_, true) << "\n"
            << "  Sharpe          " << show(report.sharpe_, false) << " per bar, "
            << show(report.annualized_sharpe_, false) << " annualized\n"
            << "  Sortino         " << show(report.sortino_, false) << "\n"
            << "  max drawdown    " << show(report.max_drawdown_, true) << "\n"
            << "  turnover        " << show(report.total_turnover_, true) << "\n"
            << "  commission      " << money_or_absent(report.commission_paid_) << "\n"
            << "  slippage        " << report.slippage_paid_.str() << "\n"
            << "  not modelled    volume, queue position, market impact, borrow\n\n";
}
#endif

/** Reads history, then replays it. The session is finished with before the replay starts. */
template <typename Session>
int backtest_with(Session& session, const std::string& symbol) {
  const InstrumentId instrument{Country::KR, Venue::COMBINED, symbol};

  const auto history = read_history(session, instrument, 400);
  if (!history) {
    return 1;
  }

  std::cout << "broker: " << Session::BrokerTag::name << "   instrument: " << symbol
            << "   bars: " << history->bars_.bars_.size()
            << (history->bars_.adjusted_ ? "   adjusted" : "   UNADJUSTED") << "\n"
            << "the session is now finished with: nothing below reads it\n\n";

#if defined(ATL_HAS_EIGEN)
  vanilla::BuyAndHold holding;
  holding.params_.instrument_ = instrument;
  holding.params_.quantity_ = Decimal64::parse("100");
  replay_and_report("BuyAndHold -- the benchmark every strategy is measured against", holding,
                    *history);

  vanilla::MovingAverageCrossover::Params crossing;
  crossing.instrument_ = instrument;
  crossing.fast_ = 20;
  crossing.slow_ = 50;
  crossing.quantity_ = Decimal64::parse("100");
  replay_and_report("SmaCrossover(20, 50)", vanilla::SmaCrossover{crossing}, *history);

  vanilla::DonchianBreakout breaking;
  breaking.params_.instrument_ = instrument;
  breaking.params_.lookback_ = 20;
  breaking.params_.quantity_ = Decimal64::parse("100");
  replay_and_report("DonchianBreakout(20)", breaking, *history);

  std::cout
    << "These are real prices and a simulated fill model. Every figure assumes the whole order was\n"
    << "available at the price shown, which no bar can tell us -- so treat a small edge here as\n"
    << "noise rather than as a result.\n";
#else
  std::cout << "The report needs a linear algebra backend; configure with -DATL_USE_EIGEN=ON.\n";
#endif
  return 0;
}

int backtest_kis(ssl::context& tls, const std::string& symbol) {
  namespace kis = algotradelib::kis;

  kis::KisConfig config;
  config.from_env();
  kis::KisAccount account;
  account.from_env();

  auto client = std::make_shared<kis::KisBlockingClient>(tls, std::move(config));
  BlockingSession<kis::KisBlockingClient> session(std::move(client), std::move(account));
  return backtest_with(session, symbol);
}

int backtest_tossinvest(ssl::context& tls, const std::string& symbol, const char* sequence) {
  namespace toss = algotradelib::tossinvest;

  toss::TossConfig config;
  config.from_env();
  auto client = std::make_shared<toss::TossBlockingClient>(tls, std::move(config));

  std::int64_t selected = 0;
  if (sequence != nullptr) {
    selected = std::strtoll(sequence, nullptr, 10);
  } else {
    auto accounts = algotradelib::list_accounts(*client);
    if (!accounts || accounts.value().empty()) {
      std::cerr << "could not discover a Toss account\n";
      return 1;
    }
    selected = accounts.value().front().selector_;
  }

  BlockingSession<toss::TossBlockingClient> session(std::move(client), selected);
  return backtest_with(session, symbol);
}

} // namespace

int main(int argc, char** argv) {
  const std::string broker = argc > 1 ? argv[1] : "";
  if (broker != "kis" && broker != "tossinvest") {
    std::cerr
      << "usage: example-backtest-broker <kis|tossinvest> [symbol] [account-sequence]\n\n"
      << "Fetches history and replays it. Places no order: the engine has no session to send to.\n";
    return 2;
  }
  const std::string symbol = argc > 2 ? argv[2] : "005930";

  try {
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    if (broker == "kis") {
      return backtest_kis(tls, symbol);
    }
    return backtest_tossinvest(tls, symbol, argc > 3 ? argv[3] : nullptr);
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
