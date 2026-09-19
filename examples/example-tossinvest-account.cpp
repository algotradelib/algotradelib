#include <algotradelib/tossinvest>

#include <boost/asio/ssl/context.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ssl = boost::asio::ssl;

namespace {

void print_error(std::string_view operation, const algotradelib::BrokerError& error) {
  std::cerr << operation << " failed";
  if (!error.broker_code_.empty()) {
    std::cerr << " [" << error.broker_code_ << ']';
  }
  if (!error.request_id_.empty()) {
    std::cerr << " request=" << error.request_id_;
  }
  if (error.http_status_ != 0) {
    std::cerr << " HTTP " << error.http_status_;
  }
  if (!error.message_.empty()) {
    std::cerr << ": " << error.message_;
  }
  std::cerr << '\n';
}

std::string mask_account_number(std::string_view account_number) {
  if (account_number.size() <= 4) {
    return std::string(account_number.size(), '*');
  }
  return std::string(account_number.size() - 4, '*') +
    std::string(account_number.substr(account_number.size() - 4));
}

void print_holdings(const algotradelib::HoldingsSnapshot& snapshot) {
  std::cout << "positions: " << snapshot.positions_.size() << '\n';
  for (const auto& position : snapshot.positions_) {
    std::cout << position.instrument_.symbol_ << "  " << position.name_ << '\n';
    std::cout << "  quantity: " << position.quantity_.str() << '\n';
    std::cout << "  last price: " << position.last_price_.str() << ' '
      << position.market_value_.currency_.str() << '\n';
    std::cout << "  market value: " << position.market_value_.amount_.str() << ' '
      << position.market_value_.currency_.str() << '\n';
    std::cout << "  unrealized P/L: " << position.unrealized_profit_loss_.amount_.str() << ' '
      << position.unrealized_profit_loss_.currency_.str() << '\n';
  }

  std::cout << "totals: " << snapshot.totals_.size() << '\n';
  for (const auto& total : snapshot.totals_) {
    std::cout << total.currency_.str() << "  purchase=" << total.purchase_value_.str()
      << "  market=" << total.market_value_.str()
      << "  unrealized=" << total.unrealized_profit_loss_.str() << '\n';
  }
}

std::int64_t parse_account_sequence(const char* value) {
  const std::string text(value);
  std::size_t parsed = 0;
  const auto sequence = std::stoll(text, &parsed);
  if (parsed != text.size() || sequence <= 0) {
    throw std::invalid_argument("account sequence must be a positive integer");
  }
  return sequence;
}

} // namespace

int main(int argc, char* argv[]) {
  try {
    algotradelib::tossinvest::TossConfig config;
    config.from_env();

    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    algotradelib::tossinvest::TossBlockingClient client(tls, std::move(config));
    auto accounts_result = client.accounts();
    if (!accounts_result) {
      print_error("account discovery", accounts_result.error());
      return 1;
    }

    const auto& accounts = accounts_result.value();
    std::cout << "accounts: " << accounts.size() << '\n';
    for (const auto& account : accounts) {
      std::cout << account.sequence_ << "  " << mask_account_number(account.account_number_)
        << "  " << account.account_type_ << '\n';
    }
    if (accounts.empty()) {
      return 0;
    }

    const std::int64_t selected_sequence =
      argc > 1 ? parse_account_sequence(argv[1]) : accounts.front().sequence_;
    const auto selected = std::find_if(
      accounts.begin(),
      accounts.end(),
      [selected_sequence](const algotradelib::tossinvest::TossAccount& account) {
        return account.sequence_ == selected_sequence;
      }
    );
    if (selected == accounts.end()) {
      throw std::invalid_argument("selected account sequence was not returned by discovery");
    }

    std::cout << "selected account: " << selected_sequence << '\n';
    auto holdings_result = client.holdings(selected_sequence);
    if (!holdings_result) {
      print_error("holdings request", holdings_result.error());
      return 1;
    }

    print_holdings(holdings_result.value());
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "example failed: " << exception.what() << '\n';
    return 1;
  }
}
