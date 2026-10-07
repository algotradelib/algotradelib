// Shows which account a Kiwoom credential drives, what it holds, and what it may spend.
//
// Kiwoom takes no account number on any request -- the application key *is* the account -- so
// `ka00001` is the only way to check the credential points where you meant. The session binds an
// empty selector for the same reason.

#include "kiwoom_credentials.hpp"

#include <algotradelib/kiwoom>

#include <boost/asio/ssl/context.hpp>

#include <exception>
#include <memory>
#include <iostream>
#include <utility>

namespace net = boost::asio;
namespace ssl = net::ssl;

int main(int argc, char** argv) {
  using algotradelib::BlockingSession;
  using algotradelib::BrokerCaps;
  using algotradelib::kiwoom::KiwoomBlockingClient;

  try {
    // `--credentials DIR [--kiwoom-account LABEL]`, else the environment (kiwoom_credentials.hpp).
    const auto directory = kis_credentials::take_directory(argc, argv);
    const auto label = kiwoom_credentials::take_label(argc, argv);
    kiwoom_credentials::refuse_unknown_flags(argc, argv);
    algotradelib::kiwoom::KiwoomConfig config;
    kiwoom_credentials::load(directory, label, config, std::cout);
    // Kiwoom's balance inquiry names one venue and publishes no combined value, and whether that
    // selects which positions are listed or only how they are valued is not stated anywhere. So
    // there is no default and a caller states the venue deliberately.
    config.holdings_venue_ = algotradelib::Venue::KRX;
    // And what naming it *means* is a separate statement, which this example does not make: with
    // both scopes left unstated the normalized holdings verb refuses and so does anything that can
    // open a position, which is what an example should demonstrate rather than assume away. A real
    // program asserts them once it has measured what its account returns -- and an assertion is not
    // a measurement.

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

    // **The normalized `session.holdings()` verb refuses while the scope is unstated**, because
    // kt00018 names one venue and nothing says whether that filters the positions or only values
    // them -- so this library will not present one venue's answer as the account's holdings unless
    // a caller says that is what it is. The broker-specific inquiry below answers for the
    // configured venue regardless, and running it for KRX and again for NXT is the **diagnostic**
    // the question needs.
    //
    // It is not by itself a proof, and that is worth knowing before reading the output: two reads
    // are not a snapshot, so rows that differ may record a fill between them rather than a filtered
    // listing, and an empty account makes both answers identical. Run it on a quiet account and
    // read it as evidence, not as an answer.
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
