// Shows which account a Kiwoom credential drives, what it holds, and what it may spend.
//
// Kiwoom takes no account number on any request -- the application key *is* the account -- so
// `ka00001` is the only way to check the credential points where you meant. The session binds an
// empty selector for the same reason.

#include <algotradelib/kiwoom>

#include <boost/asio/ssl/context.hpp>

#include <exception>
#include <memory>
#include <iostream>
#include <utility>

namespace net = boost::asio;
namespace ssl = net::ssl;

int main() {
  using algotradelib::BlockingSession;
  using algotradelib::BrokerCaps;
  using algotradelib::kiwoom::KiwoomBlockingClient;

  try {
    algotradelib::kiwoom::KiwoomConfig config;
    config.from_env();
    // Kiwoom's balance inquiry names one venue and publishes no combined value, and whether that
    // selects which positions are listed or only how they are valued is not stated anywhere. So
    // there is no default and a caller states it deliberately; see
    // issues/kiwoom-api-holdings-venue-scope.md for the one request pair that settles it.
    config.holdings_venue_ = algotradelib::Venue::KRX;

    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    BlockingSession<KiwoomBlockingClient> session(
      std::make_shared<KiwoomBlockingClient>(tls, std::move(config)),
      algotradelib::kiwoom::KiwoomAccount{}
    );

    static_assert(
      BrokerCaps<KiwoomBlockingClient>::has_account_discovery,
      "Kiwoom publishes the account its credential drives"
    );
    const auto accounts = session.accounts();
    if (!accounts) {
      std::cerr << "account listing failed: " << accounts.error().message_ << '\n';
      return 1;
    }
    for (const auto& account : accounts.value()) {
      std::cout << "account: " << account.display_number_ << '\n';
    }

    // **The normalized `session.holdings()` verb refuses**, because kt00018 names one venue and
    // nothing says whether that filters the positions or only values them -- so this library will
    // not present one venue's answer as the account's holdings. The broker-specific inquiry is
    // what answers for the configured venue, and running it for KRX and again for NXT is the
    // experiment that settles the question; see issues/kiwoom-api-holdings-venue-scope.md.
    const auto holdings = session.client().holdings(algotradelib::kiwoom::KiwoomAccount{});
    if (!holdings) {
      std::cerr << "holdings failed: " << holdings.error().message_ << '\n';
      return 1;
    }
    std::cout << "holdings as Kiwoom reported them for one venue:\n";
    for (const auto& position : holdings.value().positions_) {
      std::cout << position.instrument_.symbol_ << ' ' << position.name_ << ' '
        << position.quantity_.str();
      if (position.sellable_quantity_) {
        std::cout << " (sellable " << position.sellable_quantity_->str() << ')';
      }
      std::cout << '\n';
    }

    const auto budget = session.buying_power(algotradelib::Currency::krw());
    if (!budget) {
      std::cerr << "buying power failed: " << budget.error().message_ << '\n';
      return 1;
    }
    // The credit-free figure. Kiwoom's 주문가능금액 is larger and includes what margin and 대용금
    // allow, which is why it is not what this reports; read it from `client().deposit()`.
    std::cout << "spendable: " << budget.value().cash_.amount_.str() << ' '
      << budget.value().cash_.currency_.str() << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
