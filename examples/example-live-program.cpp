/**
 * @file
 * @brief The live program to copy: KIS, Kiwoom or Toss, journalled, recoverable, stoppable.
 *
 * ```
 * example-live-program <kis|kiwoom|toss> <symbol> [--live] [--journal PATH] [--control PATH]
 *                      [--toss-account SEQUENCE]
 * ```
 *
 * Without `--live` it **observes**: real market data, holdings and orders run through the real
 * pipeline, guard and gates, and the send step goes to an executor that holds no session, so nothing
 * can leave. With `--live` it trades, through a journal, after recovering whatever a previous run
 * left owed.
 *
 * **Demo or live is the environment's, not this program's.** `KIS_DEMO` and `KIWOOM_DEMO` are
 * required (`1` or `0`) beside the credentials they belong to. **Toss has no paper-trading
 * environment**: `toss --live` trades real money.
 *
 * The broker-free part -- the order of journal, recovery, loop and report -- is `live_program.hpp`,
 * which the test suite drives against an in-memory session. This file only chooses a broker and a
 * strategy. The strategy is deliberately trivial: `BuyAndHold` of one share. Replace it with yours,
 * and its limits in `settings_for`; on KIS and Toss nothing else has to change. On Kiwoom it places
 * nothing yet (see `run_kiwoom`).
 */

#include "live_program.hpp"

#include <algotradelib/kis>
#include <algotradelib/kiwoom>
#include <algotradelib/tossinvest>
#include <algotradelib/vanilla>

#include <boost/asio/ssl/context.hpp>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ssl = boost::asio::ssl;
namespace atl = algotradelib;
namespace runner = algotradelib::runner;
namespace vanilla = algotradelib::strategy::vanilla;

/** What the command line said. */
struct Arguments {
  std::string broker_;
  std::string symbol_;
  bool live_{false};
  std::optional<std::string> journal_;
  std::optional<std::string> control_;
  std::optional<std::int64_t> toss_account_;
};

/** The strategy this program runs: one share, held. The line a consumer replaces. */
vanilla::BuyAndHold strategy_for(const atl::InstrumentId& instrument) {
  vanilla::BuyAndHold holding;
  holding.params_.instrument_ = instrument;
  holding.params_.quantity_ = atl::Decimal64::parse("1");
  return holding;
}

/**
 * Settings for one instrument, with every age the loop asks about stated.
 *
 * The limits are sized for one share on purpose. A consumer's own limits belong to their strategy.
 */
live_program::Settings settings_for(const atl::InstrumentId& instrument) {
  live_program::Settings settings;
  settings.universe_ = {instrument};

  settings.limits_.max_order_notional_ = atl::Decimal64::parse("500000");
  settings.limits_.max_position_quantity_ = atl::Decimal64::parse("1");
  settings.limits_.max_orders_per_window_ = 2;

  // Daily bars, `SessionSource`'s default: the strategy decides on the history read at start -- the
  // loop reads the order list and holdings before it feeds a bar -- and again at each close. Polled
  // every minute so a close is seen promptly; a minute-bar strategy sets `bar_interval_`.
  settings.schedule_.bars_ = std::chrono::minutes(1);
  settings.schedule_.holdings_ = std::chrono::seconds(30);
  settings.schedule_.working_orders_ = std::chrono::seconds(30);
  // A band is a per-trading-day value; refresh it well inside a session.
  settings.schedule_.price_limits_ = std::chrono::minutes(10);
  // The calendar keeps its six-hour default.

  // Every age stated. A holdings snapshot older than two minutes -- four missed polls -- stops
  // sizing; a band older than twenty -- two missed polls -- stops clamping.
  settings.maximum_portfolio_age_ = std::chrono::minutes(2);
  settings.maximum_price_limits_age_ = std::chrono::minutes(20);
  return settings;
}

/** Keeps letters, digits and `-`, so an account label can name a file. */
std::string file_safe(std::string text) {
  for (auto& ch : text) {
    if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-') {
      ch = '_';
    }
  }
  return text;
}

/**
 * Names the journal after the account, and scopes it to broker, environment and account.
 *
 * The scope is written into the journal's header, and a file whose header names another scope
 * refuses to open. So a demo journal cannot be continued by a live run, and one account's unresolved
 * orders cannot be read as another's.
 */
void name_journal(
  live_program::Settings& settings,
  const Arguments& arguments,
  const std::string& broker,
  bool demo,
  const std::string& account
) {
  settings.journal_scope_ = broker + ':' + (demo ? "demo" : "live") + ':' + account;
  const std::string stem = "atl-" + broker + '-' + (demo ? "demo" : "live") + '-' + file_safe(account);
  settings.journal_path_ = arguments.journal_ ? *arguments.journal_ : stem + ".jsonl";
  settings.control_file_ = arguments.control_ ? *arguments.control_ : stem + ".stop";
}

/** Prints what is about to happen, before anything is read or sent. */
void announce(const Arguments& arguments, const live_program::Settings& settings, bool demo) {
  std::cout << "broker     : " << arguments.broker_ << (demo ? " (demo)" : " (LIVE ACCOUNT)") << '\n'
            << "instrument : " << arguments.symbol_ << '\n'
            << "mode       : " << (arguments.live_ ? "TRADE -- orders will be sent" : "observe") << '\n'
            << "limits     : at most " << settings.limits_.max_position_quantity_->str()
            << " share(s), " << settings.limits_.max_order_notional_->str() << " per order\n";
  if (arguments.live_) {
    std::cout << "journal    : " << settings.journal_path_ << " (scope " << settings.journal_scope_ << ")\n";
  }
}

/** Runs the chosen mode, installing the signal stop for the run's duration. */
template <typename Session>
int run(Session& session, const atl::InstrumentId& instrument, const live_program::Settings& settings, bool live) {
  runner::StopSource stop;
  runner::SignalStop::install(stop);
  const auto exit = live ?
    live_program::trade(session, strategy_for(instrument), settings, stop, std::cout) :
    live_program::observe(session, strategy_for(instrument), settings, stop, std::cout);
  runner::SignalStop::uninstall();
  return static_cast<int>(exit);
}

int run_kis(ssl::context& tls, const Arguments& arguments) {
  namespace kis = algotradelib::kis;

  kis::KisConfig config;
  config.from_env();
  kis::KisAccount account;
  account.from_env();
  const bool demo = config.is_demo_;

  const atl::InstrumentId instrument{atl::Country::KR, atl::Venue::COMBINED, arguments.symbol_};
  auto settings = settings_for(instrument);
  name_journal(settings, arguments, "kis", demo, account.account_number_ + '-' + account.product_code_);
  announce(arguments, settings, demo);

  atl::BlockingSession<kis::KisBlockingClient> session(
    std::make_shared<kis::KisBlockingClient>(tls, std::move(config)), std::move(account)
  );
  return run(session, instrument, settings, arguments.live_);
}

int run_kiwoom(ssl::context& tls, const Arguments& arguments) {
  namespace kiwoom = algotradelib::kiwoom;

  kiwoom::KiwoomConfig config;
  config.from_env();
  // Kiwoom's balance inquiry names one venue and does not say whether that filters the positions or
  // only values them, so the adapter refuses holdings until you state both. Until then the loop has
  // no portfolio and holds: this program runs and places nothing. Run the experiment in
  // issues/kiwoom-api-holdings-venue-scope.md, then assert what you measured:
  config.holdings_venue_ = atl::Venue::KRX;
  // config.holdings_position_scope_ = kiwoom::KiwoomPositionScope::WHOLE_HOLDING;
  // config.holdings_sellable_scope_ = kiwoom::KiwoomSellableScope::WHOLE_HOLDING;
  const bool demo = config.is_demo_;

  atl::BlockingSession<kiwoom::KiwoomBlockingClient> session(
    std::make_shared<kiwoom::KiwoomBlockingClient>(tls, std::move(config)), kiwoom::KiwoomAccount{}
  );
  // Kiwoom takes no account number on a request -- the application key is the account -- so the
  // journal is named after the account the key drives, as Kiwoom reports it.
  const auto accounts = session.accounts();
  if (!accounts || accounts.value().empty()) {
    std::cerr << "could not read which account this Kiwoom key drives"
              << (accounts ? std::string() : ": " + accounts.error().message_) << '\n';
    return static_cast<int>(live_program::Exit::ACCOUNT_UNRESOLVED);
  }

  const atl::InstrumentId instrument{atl::Country::KR, atl::Venue::COMBINED, arguments.symbol_};
  auto settings = settings_for(instrument);
  name_journal(settings, arguments, "kiwoom", demo, accounts.value().front().display_number_);
  announce(arguments, settings, demo);
  // Three reasons nothing is placed on Kiwoom yet, said up front so nobody waits for an order that
  // cannot come. The first two are statements you can make; the third has no setting today.
  //  - Holdings refuse until the scopes above are asserted, so the loop has no portfolio and holds.
  //  - Kiwoom publishes no trading calendar, so the market phase stays unknown, and the default
  //    limits open a position only while the market is open.
  //  - A Kiwoom order must name its venue (`dmst_stex_tp` has no default), and a strategy's orders
  //    carry none, so the adapter refuses each one before sending it. See
  //    issues/live-program-kiwoom-strategy-venue.md.
  std::cout << "note       : on Kiwoom this program places nothing yet -- holdings need a stated\n"
            << "             scope, the market phase is unknown without a calendar, and a strategy's\n"
            << "             orders name no venue, which Kiwoom requires\n";
  return run(session, instrument, settings, arguments.live_);
}

int run_toss(ssl::context& tls, const Arguments& arguments) {
  namespace toss = algotradelib::tossinvest;

  toss::TossConfig config;
  config.from_env();
  auto client = std::make_shared<toss::TossBlockingClient>(tls, std::move(config));

  // Toss selects an account by sequence number and publishes the list. With one account that is the
  // one; with several, the program will not guess which should trade, because a sequence is a request
  // selector rather than an identity.
  const int unresolved = static_cast<int>(live_program::Exit::ACCOUNT_UNRESOLVED);
  auto accounts = atl::list_accounts(*client);
  if (!accounts || accounts.value().empty()) {
    std::cerr << "could not list the accounts these Toss credentials reach"
              << (accounts ? std::string() : ": " + accounts.error().message_) << '\n';
    return unresolved;
  }
  if (!arguments.toss_account_ && accounts.value().size() > 1) {
    std::cerr << "these Toss credentials reach " << accounts.value().size()
              << " accounts; choose one with --toss-account:\n";
    for (const auto& account : accounts.value()) {
      std::cerr << "  --toss-account " << account.selector_ << "   (" << account.type_ << ")\n";
    }
    return unresolved;
  }
  auto chosen = accounts.value().front();
  if (arguments.toss_account_) {
    bool found = false;
    for (const auto& account : accounts.value()) {
      if (account.selector_ == *arguments.toss_account_) {
        chosen = account;
        found = true;
      }
    }
    if (!found) {
      std::cerr << "no Toss account has sequence " << *arguments.toss_account_ << '\n';
      return unresolved;
    }
  }
  if (chosen.display_number_.empty()) {
    std::cerr << "Toss reported no account number for sequence " << chosen.selector_
              << ", so the journal could not be named after the account\n";
    return unresolved;
  }

  const atl::InstrumentId instrument{atl::Country::KR, atl::Venue::COMBINED, arguments.symbol_};
  auto settings = settings_for(instrument);
  // Named after the account number, not the sequence: two credential sets can both have a sequence 1,
  // and a journal that one account's run could continue would read its orders as the other's.
  name_journal(settings, arguments, "toss", false, chosen.display_number_);
  announce(arguments, settings, false);
  std::cout << "note       : Toss has no paper-trading environment; --live trades real money\n";

  atl::BlockingSession<toss::TossBlockingClient> session(std::move(client), chosen.selector_);
  return run(session, instrument, settings, arguments.live_);
}

/** Parses the command line, or explains it. */
std::optional<Arguments> parse(int argc, char** argv) {
  Arguments arguments;
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const bool has_value = i + 1 < argc;
    if (argument == "--live") {
      arguments.live_ = true;
    } else if (argument == "--journal" && has_value) {
      arguments.journal_ = argv[++i];
    } else if (argument == "--control" && has_value) {
      arguments.control_ = argv[++i];
    } else if (argument == "--toss-account" && has_value) {
      arguments.toss_account_ = std::strtoll(argv[++i], nullptr, 10);
    } else if (argument.rfind("--", 0) == 0) {
      return std::nullopt;
    } else {
      positional.push_back(argument);
    }
  }
  if (positional.size() != 2) {
    return std::nullopt;
  }
  arguments.broker_ = positional[0];
  arguments.symbol_ = positional[1];
  if (arguments.broker_ != "kis" && arguments.broker_ != "kiwoom" && arguments.broker_ != "toss") {
    return std::nullopt;
  }
  return arguments;
}

} // namespace

/** Chooses a broker from the command line and observes or trades through it. */
int main(int argc, char** argv) {
  const auto arguments = parse(argc, argv);
  if (!arguments) {
    std::cerr
      << "usage: example-live-program <kis|kiwoom|toss> <symbol> [--live] [--journal PATH]\n"
      << "                            [--control PATH] [--toss-account SEQUENCE]\n\n"
      << "Without --live it observes: nothing can be sent. KIS_DEMO / KIWOOM_DEMO (1 or 0) choose\n"
      << "demo or live; Toss has no demo, so toss --live trades real money.\n";
    return 64;
  }

  try {
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    if (arguments->broker_ == "kis") {
      return run_kis(tls, *arguments);
    }
    if (arguments->broker_ == "kiwoom") {
      return run_kiwoom(tls, *arguments);
    }
    return run_toss(tls, *arguments);
  } catch (const std::exception& exception) {
    std::cerr << "example-live-program: " << exception.what() << '\n';
    return 70;
  }
}
