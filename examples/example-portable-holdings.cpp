// One account report written once and compiled for every supported broker.
//
// `report()` below is the point of this example: it names no vendor type, no
// account selector, and no broker-specific acknowledgement. It is instantiated
// twice — once for KIS and once for Toss Securities — and the only thing that
// differs between the two runs is the type parameter chosen in `main()`.
//
// Run it as:
//
//   example-portable-holdings kis
//   example-portable-holdings tossinvest [account-sequence]
//
// KIS reads its account selector from `KIS_CANO` and the optional
// `KIS_ACNT_PRDT_CD`; Toss discovers its accounts over the network and uses the
// first one unless a sequence is supplied. Credentials come from the same
// environment variables the other examples document, and no order is ever placed.

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

namespace ssl = boost::asio::ssl;

namespace {

using algotradelib::BlockingSession;
using algotradelib::BrokerCaps;
using algotradelib::BrokerError;
using algotradelib::HoldingsSnapshot;

/** Prints a structured broker or transport failure. */
void print_error(std::string_view operation, const BrokerError& error) {
  std::cerr << operation << " failed";
  if (!error.broker_code_.empty()) {
    std::cerr << " [" << error.broker_code_ << ']';
  }
  if (error.http_status_ != 0) {
    std::cerr << " HTTP " << error.http_status_;
  }
  if (!error.message_.empty()) {
    std::cerr << ": " << error.message_;
  }
  if (error.retry_after_) {
    std::cerr << " (retry after " << error.retry_after_->count() << "s)";
  }
  std::cerr << '\n';
}

/** Prints normalized positions and per-currency totals. */
void print_holdings(const HoldingsSnapshot& snapshot) {
  std::cout << "positions: " << snapshot.positions_.size() << '\n';
  for (const auto& position : snapshot.positions_) {
    std::cout << "  " << position.instrument_.symbol_ << "  " << position.name_
      << "  quantity=" << position.quantity_.str()
      << "  market=" << position.market_value_.amount_.str() << ' '
      << position.market_value_.currency_.str() << '\n';
  }
  for (const auto& total : snapshot.totals_) {
    std::cout << "  total " << total.currency_.str()
      << "  purchase=" << total.purchase_value_.str()
      << "  market=" << total.market_value_.str()
      << "  unrealized=" << total.unrealized_profit_loss_.str() << '\n';
  }
}

/** Prints normalized working orders and the reference each amendment would use. */
template <typename Broker>
void print_working_orders(const algotradelib::WorkingOrders<Broker>& orders) {
  std::cout << "working orders: " << orders.orders_.size() << '\n';
  for (const auto& working : orders.orders_) {
    std::cout << "  " << working.order_.instrument_.symbol_
      << "  reference=" << working.ref_.str()
      << "  ordered=" << working.order_.quantity_.str()
      << "  filled=" << working.order_.execution_.filled_quantity_.str();
    if (working.amendable_quantity_) {
      std::cout << "  amendable=" << working.amendable_quantity_->str();
    } else {
      std::cout << "  amendable=not reported";
    }
    std::cout << '\n';
  }
}

/**
 * Reports one account through the broker port only.
 *
 * Nothing here mentions a broker. The two capability gates are the whole of the
 * broker-specific surface, and each states what the difference actually is
 * rather than naming a broker.
 */
template <typename Client>
int report(BlockingSession<Client>& session) {
  using Caps = BrokerCaps<Client>;

  std::cout << "broker: " << session.broker_name() << '\n';
  std::cout << "reports an amendable quantity: "
    << (Caps::has_amendable_quantity ? "yes" : "no") << '\n';
  std::cout << "accepts an idempotency key: "
    << (Caps::has_client_order_id ? "yes" : "no") << '\n';

  auto holdings = session.holdings();
  if (!holdings) {
    print_error("holdings request", holdings.error());
    return 1;
  }
  print_holdings(holdings.value());

  if constexpr (Caps::has_working_orders) {
    auto working = session.working_orders();
    if (!working) {
      // KIS does not serve this inquiry to a demo account, which is a run-time
      // condition of the configuration rather than a missing feature, so the
      // report continues instead of failing.
      print_error("working-order inquiry", working.error());
    } else {
      print_working_orders(working.value());
    }
  }

  return 0;
}

/** Builds a KIS session from the environment and reports it. */
int report_kis(ssl::context& tls) {
  namespace kis = algotradelib::kis;

  kis::KisConfig config;
  kis::KisAccount account;
  kis_credentials::load(kis_credentials::environment_directory(), config, &account, std::cout);

  std::cout << "environment: " << (config.is_demo_ ? "demo" : "live") << '\n';

  auto client =
    std::make_shared<kis::KisBlockingClient>(tls, std::move(config), kis_credentials::paced_options());
  BlockingSession<kis::KisBlockingClient> session(std::move(client), std::move(account));
  return report(session);
}

/** Builds a Toss session, discovering the account when none is supplied. */
int report_tossinvest(ssl::context& tls, const char* requested_sequence) {
  namespace toss = algotradelib::tossinvest;

  toss::TossConfig config;
  config.from_env();

  auto client = std::make_shared<toss::TossBlockingClient>(tls, std::move(config));

  std::int64_t sequence = 0;
  if (requested_sequence) {
    sequence = std::strtoll(requested_sequence, nullptr, 10);
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
    for (const auto& account : accounts.value()) {
      std::cout << "account " << account.display_number_ << "  type=" << account.type_
        << "  sequence=" << account.selector_ << '\n';
    }
    sequence = accounts.value().front().selector_;
  }

  BlockingSession<toss::TossBlockingClient> session(std::move(client), sequence);
  return report(session);
}

} // namespace

int main(int argc, char** argv) {
  const std::string broker = argc > 1 ? argv[1] : "";
  if (broker != "kis" && broker != "tossinvest") {
    std::cerr << "usage: example-portable-holdings <kis|tossinvest> [account-sequence]\n";
    return 2;
  }

  try {
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    if (broker == "kis") {
      return report_kis(tls);
    }
    return report_tossinvest(tls, argc > 2 ? argv[2] : nullptr);
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
