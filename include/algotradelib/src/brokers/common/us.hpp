#pragma once

#include "../../core/instrument.hpp"

namespace algotradelib {
namespace us {

/**
 * @brief Returns the US equity price grid: $0.01 at or above $1, $0.0001 below (Regulation NMS Rule 612).
 *
 * Rule 612 sets the smallest increment an order may be priced in for an NMS stock (one listed on a US national
 * exchange); OTC equities are not covered.
 * The half-penny tier of the 2024 amendments, for some stocks, was not checked: if it is in effect, this grid refuses
 * an `x.xx5` limit on those stocks, which is the safe direction.
 *
 * @return The rule.
 */
inline PriceRule equity_price_rule() {
  PriceRule rule;
  rule.kind_ = PriceRule::Kind::STATED;
  rule.source_ =
    "US Regulation NMS Rule 612 minimum increments, $0.01 from $1 and $0.0001 below (library-maintained; the 2024 "
    "half-penny tier not checked)";
  rule.bands_ = {
    TickBand{Decimal64::parse("0"), Decimal64::parse("0.0001")},
    TickBand{Decimal64::parse("1"), Decimal64::parse("0.01")},
  };
  return rule;
}

} // namespace us
} // namespace algotradelib
