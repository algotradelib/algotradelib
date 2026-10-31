// expect: no member named 'has_history_cursor'|'has_history_cursor' is not a member
// Deliberately does not compile. `conformance-requires-every-capability` builds this file and passes only
// when the diagnostic names the one constant this adapter leaves out. `has_history_cursor` gates no verb,
// so nothing but `ATL_ASSERT_BROKER`'s read of every constant finds it before a consumer's build does.
#include "mock_broker.hpp"

namespace probe {

/** The in-memory client again, under an adapter that forgets one answer. */
struct OmitsHistoryCursor : vendor::MockClient {};

} // namespace probe

namespace algotradelib {

template <>
struct BrokerTraits<probe::OmitsHistoryCursor> : mock_port::Members {
  static constexpr bool has_account_discovery = false;
  static constexpr bool has_client_order_id = false;
  static constexpr bool has_working_orders = true;
  static constexpr bool has_amendable_quantity = true;
  static constexpr bool working_orders_state_terminal = true;
  static constexpr bool has_trigger_price = false;
  static constexpr bool data_only = false;
  static constexpr bool has_order_by_id = false;
  static constexpr bool has_partial_cancel = true;
  static constexpr bool history_requires_range = false;
  // has_history_cursor: not answered.
  static constexpr bool has_history_limit = false;
  static constexpr bool has_history_symbol_filter = false;
  static constexpr bool has_buying_power = false;
  static constexpr bool has_buying_power_quantity = false;
  static constexpr SellableSource sellable_source = SellableSource::NONE;
  static constexpr bool has_commission_schedule = false;
  static constexpr bool has_depth = false;
  static constexpr bool has_price_limits = false;
  static constexpr bool has_trade_prints = false;
  static constexpr bool has_batch_prices = false;
  static constexpr bool has_reference_data = false;
  static constexpr bool has_batch_reference_data = false;
  static constexpr bool has_calendar = false;
  static constexpr bool has_calendar_range = false;
  static constexpr bool has_calendar_sessions = false;
};

} // namespace algotradelib

ATL_ASSERT_BROKER(probe::OmitsHistoryCursor);
