/**
 * @file
 * @brief The live program to copy: KIS, Kiwoom or Toss, journalled, recoverable, stoppable.
 *
 * ```
 * example-live-program <kis|kiwoom|toss> <symbol> [--live] [--journal PATH] [--control PATH]
 *                      [--toss-account SEQUENCE] [--credentials DIR] [--market-data kis|toss]
 *                      [--kiwoom-account LABEL]
 * ```
 *
 * Without `--live` it **observes**: real market data, holdings and orders run through the real
 * pipeline, guard and gates, and the send step goes to an executor that holds no session, so nothing
 * can leave. With `--live` it trades, through a journal, after recovering whatever a previous run
 * left owed.
 *
 * **Demo or live is the environment's, not this program's.** KIS and Kiwoom are demo unless
 * `KIS_DEMO=0` / `KIWOOM_DEMO=0` says live, beside the credentials they belong to. **Toss has no
 * paper-trading environment**: `toss --live` trades real money.
 *
 * **KIS credentials and market data** (`kis_credentials.hpp`). With a credentials directory
 * (`--credentials DIR`, else `ATL_CREDENTIALS_DIR`) KIS reads `DIR/kis/demo.json` or `real.json`.
 * A KIS demo host refuses reference data and the calendar (`EGW02006`), so on demo the market reads
 * go to a second session that cannot trade:
 * - `--market-data kis`, the default on demo when `DIR/kis/real.json` exists: the real account's keys
 *   with an empty `KisAccount`;
 * - `--market-data toss`: Toss from `DIR/tossinvest/real.json`, with no account selected. The price
 *   band and reference data are then Toss's, which can differ from KIS's (after the close Toss
 *   publishes the next session's band, KIS keeps the day's): KIS may refuse an order clamped to
 *   Toss's band.
 *
 * Without a directory, or without `DIR/kis/real.json`, the demo session reads its own market data,
 * and stops at reference data. With `kis/real.json` the run reads reference data and the calendar
 * through the real keys, so the market phase is known and, with `--live`, the strategy trades.
 * Every KIS client here is paced at one request a second (`kis_credentials::paced_options`): KIS
 * refuses faster reads (`EGW00201`), and the calendar's pages take about 20 s at startup.
 *
 * **Kiwoom credentials** (`kiwoom_credentials.hpp`). Kiwoom issues one key per account, so with a
 * directory Kiwoom reads `DIR/kiwoom/demo-<label>.json` (or `real-<label>.json` with `KIWOOM_DEMO=0`),
 * the label from `--kiwoom-account` (default `kr`). With a directory, the `KIWOOM_*` variables are not
 * read; without one they are, and `--kiwoom-account` is refused.
 *
 * Toss reads its environment variables, so `--credentials` given to it is refused rather than ignored.
 *
 * The broker-free part -- the order of journal, recovery, loop and report -- is `live_program.hpp`,
 * which the test suite drives against an in-memory session. This file only chooses a broker and a
 * strategy. The strategy is deliberately trivial: `BuyAndHold` of one share. Replace it with yours,
 * and its limits in `settings_for`; on KIS and Toss nothing else has to change. On Kiwoom two
 * statements are yours to make before it can place (see `run_kiwoom`).
 */

#include "kis_credentials.hpp"
#include "kiwoom_credentials.hpp"
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
#include <filesystem>
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

/** @brief What the command line said. */
struct Arguments {
  std::string broker_;
  std::string symbol_;
  bool live_{false};
  std::optional<std::string> journal_;
  std::optional<std::string> control_;
  std::optional<std::int64_t> toss_account_;
  std::optional<std::filesystem::path> credentials_;
  bool credentials_from_flag_{false};
  std::optional<std::string> market_data_;
  std::optional<std::string> kiwoom_account_;
};

/** @brief The strategy this program runs: one share, held. The line a consumer replaces. */
vanilla::BuyAndHold strategy_for(const atl::InstrumentId& instrument) {
  vanilla::BuyAndHold holding;
  holding.params_.instrument_ = instrument;
  holding.params_.quantity_ = atl::Decimal64::parse("1");
  return holding;
}

/**
 * @brief Settings for one instrument, with every age the loop asks about stated.
 *
 * The limits are sized for one share on purpose. A consumer's own limits belong to their strategy.
 */
live_program::Settings settings_for(const atl::InstrumentId& instrument) {
  live_program::Settings settings;
  settings.universe_ = {instrument};

  settings.limits_.max_order_notional_ = atl::Decimal64::parse("500000");
  settings.limits_.max_position_quantity_ = atl::Decimal64::parse("1");
  settings.limits_.max_orders_per_window_ = 2;

  // Daily bars, `SessionSource`'s default. The loop reads the order list and holdings before it feeds
  // a bar. At start the strategy is shown the history read, but only the newest closed bar's decision
  // can be sent: at once if the market is open; otherwise an opening order waits for the first `OPEN`
  // iteration (which needs a calendar), and an exit goes out at once and is kept if the broker refuses
  // it. After that a day's bar is fed once the day has ended, at the first poll after midnight KST,
  // and the same rule applies ("kept for the next session" in running-a-strategy-live.md). Polled
  // every minute so that poll comes promptly; a minute-bar strategy sets `bar_interval_`.
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

/** @brief Keeps letters, digits and `-`, so an account label can name a file. */
std::string file_safe(std::string text) {
  for (auto& ch : text) {
    if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-') {
      ch = '_';
    }
  }
  return text;
}

/**
 * @brief Names the journal after the account, and scopes it to broker, environment and account.
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

/**
 * @brief Prints what is about to happen, before anything is read or sent.
 *
 * `demo_variable` names the broker's demo switch, or is null for a broker without one.
 */
void announce(
  const Arguments& arguments,
  const live_program::Settings& settings,
  bool demo,
  const char* demo_variable
) {
  std::cout << "broker     : " << arguments.broker_ << (demo ? " (demo)" : " (LIVE ACCOUNT)") << '\n';
  // Demo because nothing said live: say how to say it, since live keys on the demo endpoints fail with
  // the broker's own authentication error, which does not point here.
  if (demo && demo_variable != nullptr && std::getenv(demo_variable) == nullptr) {
    std::cout << "             demo by default; set " << demo_variable << "=0 to trade live\n";
  }
  std::cout << "instrument : " << arguments.symbol_ << '\n'
            << "mode       : " << (arguments.live_ ? "TRADE -- orders will be sent" : "observe") << '\n'
            << "limits     : at most " << settings.limits_.max_position_quantity_->str()
            << " share(s), " << settings.limits_.max_order_notional_->str() << " per order\n";
  if (settings.default_route_ != atl::Venue::UNSPECIFIED) {
    std::cout << "route      : " << atl::venue_name(settings.default_route_) << '\n';
  }
  if (arguments.live_) {
    std::cout << "journal    : " << settings.journal_path_ << " (scope " << settings.journal_scope_ << ")\n";
  }
}

/** @brief Runs the chosen mode, installing the signal stop for the run's duration. */
template <typename Session, typename MarketData>
int run(
  Session& session,
  MarketData& market_data,
  const atl::InstrumentId& instrument,
  const live_program::Settings& settings,
  bool live
) {
  runner::StopSource stop;
  runner::SignalStop::install(stop);
  const auto exit = live ?
    live_program::trade(session, market_data, strategy_for(instrument), settings, stop, std::cout) :
    live_program::observe(session, market_data, strategy_for(instrument), settings, stop, std::cout);
  runner::SignalStop::uninstall();
  return static_cast<int>(exit);
}

/** @brief Runs the chosen mode with one session for everything. */
template <typename Session>
int run(Session& session, const atl::InstrumentId& instrument, const live_program::Settings& settings, bool live) {
  return run(session, session, instrument, settings, live);
}

int run_kis(ssl::context& tls, const Arguments& arguments) {
  namespace kis = algotradelib::kis;

  kis::KisConfig config;
  kis::KisAccount account;
  kis_credentials::load(arguments.credentials_, config, &account, std::cout);
  const bool demo = config.is_demo_;

  const atl::InstrumentId instrument{atl::Country::KR, atl::Venue::COMBINED, arguments.symbol_};
  auto settings = settings_for(instrument);
  name_journal(settings, arguments, "kis", demo, account.account_number_ + '-' + account.product_code_);
  announce(arguments, settings, demo, "KIS_DEMO");

  atl::BlockingSession<kis::KisBlockingClient> session(
    std::make_shared<kis::KisBlockingClient>(tls, std::move(config), kis_credentials::paced_options()),
    std::move(account)
  );

  // Market data from a session with no account, so it cannot trade: swapped by mistake, every order
  // and account read through it is refused before any I/O.
  if (arguments.market_data_ == "toss") {
    if (!demo) {
      // Toss's band can differ from KIS's (`labs/FINDINGS.md` F27); not for real money until that
      // is explained.
      throw std::runtime_error("--market-data toss is for a KIS demo run only");
    }
    if (!arguments.credentials_) {
      throw std::runtime_error("--market-data toss reads tossinvest/real.json; give --credentials DIR");
    }
    const auto path = *arguments.credentials_ / "tossinvest" / "real.json";
    algotradelib::tossinvest::TossConfig toss_config;
    toss_config.from_json(path.string());
    atl::BlockingSession<algotradelib::tossinvest::TossBlockingClient> market_data(
      std::make_shared<algotradelib::tossinvest::TossBlockingClient>(tls, std::move(toss_config)), 0
    );
    std::cout << "market data: toss " << path.string() << " (no account: cannot trade)\n";
    return run(session, market_data, instrument, settings, arguments.live_);
  }
  if (!demo && arguments.market_data_ == "kis") {
    throw std::runtime_error("--market-data kis is for a demo run; a live run reads its own market data");
  }
  if (demo) {
    auto market_config = kis_credentials::market_data_config(arguments.credentials_);
    if (market_config) {
      atl::BlockingSession<kis::KisBlockingClient> market_data(
        std::make_shared<kis::KisBlockingClient>(
          tls, std::move(*market_config), kis_credentials::paced_options()
        ),
        kis::KisAccount{}
      );
      std::cout << "market data: kis " << (*arguments.credentials_ / "kis" / "real.json").string()
                << " (no account: cannot trade)\n";
      return run(session, market_data, instrument, settings, arguments.live_);
    }
    if (arguments.market_data_ == "kis") {
      throw std::runtime_error(
        arguments.credentials_ ? "--market-data kis reads kis/real.json, and there is none" :
                                 "--market-data kis reads kis/real.json; give --credentials DIR"
      );
    }
    std::cout << "market data: the demo account's own (no kis/real.json); a demo host refuses\n"
              << "             reference data, so this run stops there\n";
  }
  return run(session, instrument, settings, arguments.live_);
}

int run_kiwoom(ssl::context& tls, const Arguments& arguments) {
  namespace kiwoom = algotradelib::kiwoom;

  kiwoom::KiwoomConfig config;
  kiwoom_credentials::load(arguments.credentials_, arguments.kiwoom_account_, config, std::cout);
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
  // A Kiwoom order must name its venue (`dmst_stex_tp` has no default), and a strategy's orders name
  // none, so the binding states where they go. KRX here; `Venue::NXT` for Nextrade, or
  // `Venue::COMBINED` for Kiwoom's SOR, which its order endpoints document -- whether a demo account
  // accepts SOR, and whether the listing's `sor_yn` flag reads as documented, has not been observed,
  // so KRX is the route with the fewest open questions.
  settings.default_route_ = atl::Venue::KRX;
  name_journal(settings, arguments, "kiwoom", demo, accounts.value().front().display_number_);
  announce(arguments, settings, demo, "KIWOOM_DEMO");
  // Two reasons nothing is placed on Kiwoom yet, said up front so nobody waits for an order that
  // cannot come. Both are statements you can make:
  //  - Holdings refuse until the scopes above are asserted, so the loop has no portfolio and holds.
  //  - Kiwoom publishes no trading calendar, so the market phase stays unknown, and the default
  //    limits open a position only while the market is open.
  std::cout << "note       : on Kiwoom this program places nothing yet -- holdings need a stated\n"
            << "             scope, and the market phase is unknown without a calendar\n";
  return run(session, instrument, settings, arguments.live_);
}

int run_toss(ssl::context& tls, const Arguments& arguments) {
  namespace toss = algotradelib::tossinvest;

  toss::TossConfig config;
  config.from_env();
  std::cout << "credentials: environment variables (Toss reads no credentials directory)\n";
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
  announce(arguments, settings, false, nullptr);
  std::cout << "note       : Toss has no paper-trading environment; --live trades real money\n";

  atl::BlockingSession<toss::TossBlockingClient> session(std::move(client), chosen.selector_);
  return run(session, instrument, settings, arguments.live_);
}

/** @brief Parses the command line, or explains it. */
std::optional<Arguments> parse(int argc, char** argv) {
  Arguments arguments;
  const int given = argc;
  try {
    arguments.credentials_ = kis_credentials::take_directory(argc, argv);
    arguments.credentials_from_flag_ = argc != given;
    arguments.kiwoom_account_ = kiwoom_credentials::take_label(argc, argv);
  } catch (const std::runtime_error& error) {
    std::cerr << "example-live-program: " << error.what() << '\n';
    return std::nullopt;
  }
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
    } else if (argument == "--market-data" && has_value) {
      arguments.market_data_ = argv[++i];
      if (*arguments.market_data_ != "kis" && *arguments.market_data_ != "toss") {
        return std::nullopt;
      }
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
  // A setting the chosen broker would ignore is refused: ignored, it would trade whichever account
  // the environment names. Market data is KIS's; Toss reads only its environment variables; the
  // label selects a Kiwoom file.
  if (arguments.market_data_ && arguments.broker_ != "kis") {
    return std::nullopt;
  }
  if (arguments.credentials_from_flag_ && arguments.broker_ == "toss") {
    return std::nullopt;
  }
  if (arguments.kiwoom_account_ && arguments.broker_ != "kiwoom") {
    return std::nullopt;
  }
  return arguments;
}

} // namespace

/**
 * @brief Chooses a broker from the command line and observes or trades through it.
 * @param argc The argument count.
 * @param argv The arguments.
 * @return A `live_program::Exit` value, 64 for a usage error, or 70 if an exception escaped (what was
 *   in flight may then be unknown).
 */
int main(int argc, char** argv) {
  const auto arguments = parse(argc, argv);
  if (!arguments) {
    std::cerr
      << "usage: example-live-program <kis|kiwoom|toss> <symbol> [--live] [--journal PATH]\n"
      << "                            [--control PATH] [--toss-account SEQUENCE]\n"
      << "                            [--credentials DIR] [--market-data kis|toss]\n"
      << "                            [--kiwoom-account LABEL]\n\n"
      << "Without --live it observes: nothing can be sent. KIS and Kiwoom are demo unless\n"
      << "KIS_DEMO=0 / KIWOOM_DEMO=0; Toss has no demo, so toss --live trades real money.\n";
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
