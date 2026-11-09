#pragma once

#include "../../core/instrument.hpp"

namespace algotradelib {
namespace krx {

/**
 * @brief Returns the KRX stock tick table (January 2023 revision).
 * @return The rule.
 */
inline PriceRule equity_price_rule() {
  PriceRule rule;
  rule.kind_ = PriceRule::Kind::STATED;
  rule.source_ = "KRX cash equity tick table, 2023-01 revision (library-maintained)";
  rule.bands_ = {
    TickBand{Decimal64::parse("0"), Decimal64::parse("1")},
    TickBand{Decimal64::parse("2000"), Decimal64::parse("5")},
    TickBand{Decimal64::parse("5000"), Decimal64::parse("10")},
    TickBand{Decimal64::parse("20000"), Decimal64::parse("50")},
    TickBand{Decimal64::parse("50000"), Decimal64::parse("100")},
    TickBand{Decimal64::parse("200000"), Decimal64::parse("500")},
    TickBand{Decimal64::parse("500000"), Decimal64::parse("1000")},
  };
  return rule;
}

/**
 * @brief Returns the KRX tick table for ETFs and ETNs (December 2023 revision).
 * @return The rule.
 */
inline PriceRule fund_price_rule() {
  PriceRule rule;
  rule.kind_ = PriceRule::Kind::STATED;
  rule.source_ = "KRX ETF and ETN tick table, 2023-12 revision (library-maintained)";
  rule.bands_ = {
    TickBand{Decimal64::parse("0"), Decimal64::parse("1")},
    TickBand{Decimal64::parse("2000"), Decimal64::parse("5")},
  };
  return rule;
}

/**
 * @brief Returns the KRX price grid for a kind of security listed on KRX.
 *
 * Stocks, foreign stocks, depositary receipts, infrastructure funds and REITs get the stock table, and ETFs and ETNs the ETF and ETN table.
 * Foreign ETFs, warrants and unknown types get an `UNKNOWN` rule.
 *
 * @param type The security type.
 * @return The rule.
 */
inline PriceRule price_rule_for(SecurityType type) {
  switch (type) {
    case SecurityType::STOCK:
    case SecurityType::FOREIGN_STOCK:
    case SecurityType::DEPOSITARY_RECEIPT:
    case SecurityType::INFRASTRUCTURE_FUND:
    case SecurityType::REIT:
      return equity_price_rule();
    case SecurityType::ETF:
    case SecurityType::ETN:
      return fund_price_rule();
    case SecurityType::FOREIGN_ETF:
      return PriceRule::unknown("no price grid is confirmed for this foreign ETF class");
    case SecurityType::STOCK_WARRANT:
    case SecurityType::UNKNOWN:
      break;
  }
  return PriceRule::unknown("no KRX price grid is known for this security type");
}

} // namespace krx
} // namespace algotradelib
