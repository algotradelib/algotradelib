#pragma once

#include "./config.hpp"
#include "./errors.hpp"

#include "../common.hpp"
#include "../../core/instrument.hpp"
#include "../../core/market_data.hpp"
#include "../../core/types.hpp"

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace algotradelib::kiwoom {

/**
 * @brief The Kiwoom account selector, which is empty: no Kiwoom endpoint takes an account number,
 *   since the app key determines the account (`ka00001` reports which).
 */
struct KiwoomAccount {
  /**
   * @brief Compares two selectors (the arguments are not used); always equal, since a Kiwoom
   *   credential drives exactly one account.
   * @return `true`.
   */
  friend bool operator==(const KiwoomAccount&, const KiwoomAccount&) { return true; }
};

/** @brief One account Kiwoom's `ka00001` reports for the bound credential. */
struct KiwoomAccountInfo {
  /** @brief The account number as Kiwoom reported it, for display. */
  std::string account_number_;
};

/**
 * @brief A Kiwoom order's address: order number and symbol, both of which amendments and
 *   cancellations require.
 *
 * The symbol is the six-digit code, without a tape suffix or `A` prefix.
 */
struct KiwoomOrderRef {
  /** @brief Kiwoom's `ord_no`. */
  std::string order_number_;
  /** @brief Kiwoom's `stk_cd`, which its amendment endpoints require beside the order number. */
  std::string symbol_;
};

/**
 * @brief An amendment or cancellation of a Kiwoom order.
 *
 * Kiwoom amends only quantity, price and trigger price (`kt10002`) and cancels by quantity
 * (`kt10003`); the order type, condition and session cannot change, and an amendment asking to
 * change them is refused.
 */
struct KiwoomAmendment {
  /** @brief The order to amend, which carries the symbol Kiwoom requires beside the order number. */
  KiwoomOrderRef order_;
  /**
   * @brief The venue the order rests on, as listed (Kiwoom's `dmst_stex_tp`): `KRX`, `NXT` or
   *   `COMBINED` (SOR). Required: `UNSPECIFIED` is refused before sending, for a cancellation too.
   */
  Venue venue_{Venue::UNSPECIFIED};
  /**
   * @brief The quantity affected, or zero for everything still resting (Kiwoom evaluates `0` when it
   *   processes the instruction, so it cannot overshoot a partial fill).
   */
  Decimal64 quantity_;
  /** @brief The new price, on a modification; absent on an unpriced basis, and refused on a cancellation. */
  std::optional<Decimal64> price_;
  /**
   * @brief The new trigger price (`mdfy_cond_uv`), for amending a stop-limit (`28`) order; must be
   *   empty on a cancellation, which is otherwise refused.
   */
  std::optional<Decimal64> condition_price_;
};

/**
 * @brief Kiwoom's acknowledgement of an accepted order, amendment or cancellation.
 *
 * Acceptance means Kiwoom recorded the instruction, not that it executed; read the state back through
 * the working-order listing (`ka10075`) or order history (`kt00007`).
 */
struct KiwoomOrderAck {
  /** @brief The normalized acknowledgement, whose `accepted_at_` is local receipt. */
  OrderAck ack_;
  /** @brief The reference a later amendment must address. */
  KiwoomOrderRef order_;
  /**
   * @brief Kiwoom's `base_orig_ord_no` (모주문번호) on an amendment: the order the amendment chain
   *   began with, not necessarily the one addressed.
   */
  std::string base_order_number_;
  /** @brief The quantity Kiwoom echoed on an amendment, `mdfy_qty` or `cncl_qty`. Empty on a placement. */
  std::optional<Decimal64> amended_quantity_;
};

/** @brief One Kiwoom order that is still open to modification or cancellation. */
struct KiwoomOpenOrder {
  /** @brief The order's normalized state. */
  OrderSnapshot order_;
  /** @brief The reference an amendment must address. */
  KiwoomOrderRef order_ref_;
  /** @brief Kiwoom's `orig_ord_no`: which earlier order this one superseded. Lineage, not an address. */
  std::string original_order_number_;
  /** @brief How much Kiwoom says is still unfilled, `oso_qty`, and therefore still amendable. */
  std::optional<Decimal64> amendable_quantity_;
  /**
   * @brief Kiwoom's order-type name on the row (`trde_tp`), verbatim, such as `보통` or `시장가`.
   *
   * Responses give a Korean name rather than the numeric request code, so
   * `OrderSnapshot::shape_code_` is empty on Kiwoom rows.
   */
  std::string shape_name_;
};

/** @brief One page of Kiwoom orders that remain modifiable or cancellable. */
struct KiwoomOpenOrderPage {
  /** @brief The rows on this page. */
  std::vector<KiwoomOpenOrder> orders_;
  /** @brief When the page arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

/**
 * @brief A Kiwoom order-history query: one date (`kt00007` takes no range, so a multi-day query is
 *   refused).
 */
struct KiwoomHistoryQuery {
  /** @brief The single 주문일자 to list. */
  Date date_;
  /** @brief Restrict to one symbol, which Kiwoom expresses as `stk_cd`. Empty lists every symbol. */
  std::string symbol_;
};

/**
 * @brief Returns today's date in Korea (UTC+9), the date Kiwoom requests are made in.
 * @return The date.
 */
inline Date today_in_seoul() {
  const auto seoul = std::chrono::system_clock::now() + std::chrono::hours(9);
  const std::time_t raw = std::chrono::system_clock::to_time_t(
    std::chrono::time_point_cast<std::chrono::seconds>(seoul)
  );
  std::tm parts{};
#ifdef _WIN32
  gmtime_s(&parts, &raw);
#else
  gmtime_r(&raw, &parts);
#endif
  Date date;
  date.year_ = parts.tm_year + 1900;
  date.month_ = parts.tm_mon + 1;
  date.day_ = parts.tm_mday;
  return date;
}

/**
 * @brief Makes the error that refuses a new order or an amendment while
 *   `KiwoomConfig::holdings_position_scope_` is not stated.
 *
 * Without it the library cannot establish what the account holds, so an order could be sized against
 * an empty portfolio. An amendment is refused too, since repricing can make a resting order fill.
 * Cancellations, including `cancel_quantity()`, are never refused. The client applies it to every
 * placement and amendment, however they arrive.
 *
 * @return An `INVALID_ARGUMENT` error (`holdings_scope_unestablished`, not sent) naming the setting.
 */
inline std::optional<BrokerError> unestablished_holdings_scope() {
  BrokerError error;
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = false;
  error.broker_code_ = "holdings_scope_unestablished";
  // **Names the way out, not only the gap.** A refusal that reports a fact about a vendor and no
  // action leaves a caller with nothing to do about it, and this one has a remedy: state what the
  // parameter means. Saying which setting that is turns a dead end into a decision.
  error.message_ =
    "this library cannot establish what a Kiwoom account holds -- kt00018 names one venue "
    "(dmst_stex_tp: KRX or NXT) and publishes no all-venue value, and whether that filters the "
    "positions or only values them is not stated -- so it will not place a new order, or reprice a "
    "resting one into a fillable price, against a position it cannot read. Cancelling is "
    "unaffected, including cancel_quantity(). To proceed, assert what the parameter means by "
    "setting KiwoomConfig::holdings_position_scope_ to KiwoomPositionScope::VALUATION_ONLY, having "
    "read its documentation -- an assertion that is wrong undercounts the account";
  return error;
}

/**
 * @brief Refuses an idempotency key, which Kiwoom does not offer (an order carrying
 *   `client_order_id_` is refused before sending).
 * @param order The order.
 * @return An `INVALID_ARGUMENT` error if the order has a client order id, otherwise `std::nullopt`.
 */
inline std::optional<BrokerError> unsupported_client_order_id(const OrderRequest& order) {
  if (!order.client_order_id_) {
    return std::nullopt;
  }
  BrokerError error;
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = false;
  error.broker_code_ = "client_order_id_unsupported";
  error.message_ =
    "Kiwoom publishes no caller-supplied order identifier; its order endpoints accept no such "
    "field and it would be silently dropped";
  return error;
}

/**
 * @brief Tests whether a trigger price is one Kiwoom can carry (`cond_uv`, `mdfy_cond_uv`).
 * @param trigger The trigger price.
 * @return `true` if it is a positive whole number of won.
 */
inline bool valid_trigger_value(const Decimal64& trigger) {
  return trigger.coefficient() > 0 && trigger.scale() == 0;
}

/**
 * @brief Checks that a trigger price is present exactly when the order type takes one (only
 *   `STOP_LIMIT`, Kiwoom's `28`) and is a positive whole number of won.
 *
 * Its direction relative to the limit is not checked (Kiwoom documents none).
 *
 * @param type The order type.
 * @param trigger The trigger price.
 * @return An `INVALID_ARGUMENT` error, or `std::nullopt`.
 */
inline std::optional<BrokerError> invalid_trigger(
  OrderType type,
  const std::optional<Decimal64>& trigger
) {
  BrokerError error;
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = false;
  error.broker_code_ = "invalid_trigger_price";
  if (carries_trigger_price(type)) {
    if (!trigger) {
      error.message_ = "a Kiwoom stop-limit order (28) requires a trigger price in cond_uv";
      return error;
    }
    if (!valid_trigger_value(*trigger)) {
      error.message_ = "a Kiwoom trigger price must be a positive whole number of won";
      return error;
    }
    return std::nullopt;
  }
  if (trigger) {
    error.message_ =
      "Kiwoom takes a trigger price only on a stop-limit order (28); on this basis it would be "
      "dropped";
    return error;
  }
  return std::nullopt;
}

/// \cond ATL_KIWOOM_DETAIL
namespace detail {

// Reads one of Kiwoom's signed magnitude strings as a price or a quantity.
//
// **Kiwoom prefixes a market-data number with a change *direction*, not a sign.** `"+181400"` and
// `"-91200"` are prices of 181,400 and 91,200 won; the specification says so field by field
// ("단위: 원, 부호가 포함된 숫자"). Parsing `-91200` as a negative price produces a number that is
// wrong by a factor of minus one, is a perfectly valid `Decimal64`, and passes every type check
// downstream — so every price and quantity this adapter reads from a market-data response goes
// through here, and the direction is read separately from `pred_pre_sig` where it is wanted.
//
// Account responses pad to a fixed width with leading zeros (`"000000017598258"`), which
// `Decimal64::parse` handles, and may carry a genuine minus on a profit-and-loss figure — so
// `signed_value` is the entry point for those, and this one is for the market-data fields whose
// sign is a direction.
//
// Returns nothing for an empty or unparsable field, which is how Kiwoom spells "no value".
inline std::optional<Decimal64> magnitude(const json::object& row, std::string_view key) {
  const auto value = row.if_contains(key);
  if (!value || !value->is_string()) {
    return std::nullopt;
  }
  std::string_view text(value->as_string());
  if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  for (const char digit : text) {
    if ((digit < '0' || digit > '9') && digit != '.') {
      return std::nullopt;
    }
  }
  return Decimal64::parse(text);
}

// Reads a magnitude, or zero where Kiwoom reported nothing.
inline Decimal64 magnitude_or_zero(const json::object& row, std::string_view key) {
  const auto value = magnitude(row, key);
  return value ? *value : Decimal64();
}

// Reads a field whose minus sign is arithmetic rather than a direction.
//
// **Use this only where the value can genuinely be negative**: holdings' profit and loss and return
// ratios, and the `kt00001` / `kt00010` deposit figures, which go negative when the account is in
// 미수. Everything that is a price, a quantity or a holdings money total goes through `magnitude`,
// because none of those can be negative and Kiwoom documents
// *both* kinds with the same phrase: `kt00018`'s 현재가 is "단위: 원, 좌측 0-padding 처리된 부호 포함
// 12자리 숫자", the identical wording it uses for 평가손익 on the row beside it.
//
// So the field description cannot tell them apart, and only the *meaning* can: a 현재가 of minus
// 59,000 won is not a price, so a sign there is a direction — while a 평가손익 of minus 196,888 won
// is exactly what it says. Reading a mark price as negative would put it straight into
// `HoldingPosition::last_price_` and from there into every valuation that reads a holding.
//
// Values are zero-padded to a fixed width, sometimes with the sign inside the padding, and
// `Decimal64::parse` reads both the sign and the leading zeros.
inline std::optional<Decimal64> signed_value(const json::object& row, std::string_view key) {
  const auto value = row.if_contains(key);
  if (!value || !value->is_string()) {
    return std::nullopt;
  }
  const std::string_view text(value->as_string());
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t index = (text.front() == '+' || text.front() == '-') ? 1 : 0;
  if (index == text.size()) {
    return std::nullopt;
  }
  for (; index < text.size(); ++index) {
    if ((text[index] < '0' || text[index] > '9') && text[index] != '.') {
      return std::nullopt;
    }
  }
  // `Decimal64::parse` reads the sign itself, and the leading zeros Kiwoom pads with are digits
  // like any other, so the padded and signed forms both go through unchanged.
  return Decimal64::parse(text);
}

// Reads a magnitude that must be there, and throws when it is not.
//
// **`magnitude_or_zero` cannot be used for a figure a risk check consumes.** It turns a missing,
// blank *or malformed* field into zero, and zero is a statement: a holdings row whose `rmnd_qty`
// came back garbled would be a successful listing saying the account holds none of that
// instrument, and `Portfolio::net_quantity_of` would undercount the exposure a position ceiling is
// measured against. A working order whose `ord_qty` was unreadable would understate
// `Portfolio::working_quantity`, which is what the sellable guard deducts a resting sell with.
//
// A *reported* zero is still zero and still fine. It is the unreadable one that must fail the row,
// because the two are indistinguishable afterwards.
//
// **The line is drawn at what a risk check consumes, and no wider**, because the failure mode of
// this guard is the opposite one: a field made required that Kiwoom may legitimately omit fails a
// valid listing, and a listing that fails is a poll that fails. So it covers `rmnd_qty`
// (`Portfolio::net_quantity_of`), and `ord_qty` and `cntr_qty` (`Portfolio::working_quantity`),
// plus a bar's `trde_qty`, which a strategy reads directly. The reporting figures beside them --
// cost basis, market value, profit and loss, the account totals, the deposit detail -- stay
// tolerant: a missing one there is wrong in a report, not in an order, and an account with no
// positions is the kind of edge case that should not fail a poll.
inline Decimal64 required_magnitude(const json::object& row, std::string_view key) {
  const auto value = magnitude(row, key);
  if (!value) {
    throw std::runtime_error("missing or unreadable numeric field: " + std::string(key));
  }
  return *value;
}

// Reads a genuinely signed figure that must be there, and throws when it is not.
inline Decimal64 required_signed(const json::object& row, std::string_view key) {
  const auto value = signed_value(row, key);
  if (!value) {
    throw std::runtime_error("missing or unreadable numeric field: " + std::string(key));
  }
  return *value;
}

// Reads a signed field, or zero where Kiwoom reported nothing.
inline Decimal64 signed_or_zero(const json::object& row, std::string_view key) {
  const auto value = signed_value(row, key);
  return value ? *value : Decimal64();
}

// Reads a string field, or an empty string where Kiwoom reported none.
inline std::string text_field(const json::object& row, std::string_view key) {
  const auto value = row.if_contains(key);
  if (!value || !value->is_string()) {
    return {};
  }
  return std::string(value->as_string());
}

// Returns a symbol as Kiwoom's order endpoints spell it.
//
// **Kiwoom's account listings prefix a symbol with `A`** — `kt00018` reports `"A005930"` and
// `kt00007` reports `"A069500"` — while `ka10075` and every market-data endpoint report the bare
// six digits. An instrument identity built from the prefixed form would not compare equal to one
// built from the bare form, so a holding and its own working order would stop matching. The prefix
// is stripped once, here.
//
// The tape suffix is stripped for the same reason: `005930_AL` is the same instrument as `005930`,
// read off a different book.
inline std::string bare_symbol(std::string_view reported) {
  std::string_view symbol = reported;
  if (symbol.size() > 1 && symbol.front() == 'A') {
    symbol.remove_prefix(1);
  }
  if (const auto underscore = symbol.find('_'); underscore != std::string_view::npos) {
    symbol = symbol.substr(0, underscore);
  }
  return std::string(symbol);
}

// Reads Kiwoom's `HHmmss` or `HH:mm:ss` clock time as an instant on `date` in Seoul.
inline std::optional<std::chrono::system_clock::time_point> seoul_timestamp(
  const Date& date,
  std::string_view clock
) {
  std::string digits;
  for (const char character : clock) {
    if (character >= '0' && character <= '9') {
      digits.push_back(character);
    } else if (character != ':') {
      return std::nullopt;
    }
  }
  if (digits.size() != 6) {
    return std::nullopt;
  }
  if (!is_valid_date(date)) {
    return std::nullopt;
  }
  std::tm parts{};
  parts.tm_year = date.year_ - 1900;
  parts.tm_mon = date.month_ - 1;
  parts.tm_mday = date.day_;
  parts.tm_hour = (digits[0] - '0') * 10 + (digits[1] - '0');
  parts.tm_min = (digits[2] - '0') * 10 + (digits[3] - '0');
  parts.tm_sec = (digits[4] - '0') * 10 + (digits[5] - '0');
  parts.tm_isdst = 0;
#ifdef _WIN32
  const std::time_t utc = _mkgmtime(&parts) - 9 * 3600;
#else
  const std::time_t utc = timegm(&parts) - 9 * 3600;
#endif
  return std::chrono::system_clock::from_time_t(utc);
}

// Reads Kiwoom's `YYYYMMDD` as a calendar date, or nothing when it is not one.
inline std::optional<Date> compact_date(std::string_view text) {
  return broker_detail::compact_date(text);
}

// Reads a `YYYYMMDD` field that states a date or leaves it blank, as one of three answers.
//
// Reads the raw value rather than `text_field`, which maps every non-string to `""` and would so
// report a number as a date not stated. Not stated: absent, `null` or `""`. Stated: eight digits
// forming a real date. Unreadable: anything else -- **including `"00000000"`**, since nothing in
// Kiwoom's specification uses a zero date as a blank, and KIS's use of one says nothing about Kiwoom.
inline StatedDate stated_compact_date(const json::object& row, std::string_view key) {
  const auto value = row.if_contains(key);
  if (!value || value->is_null()) {
    return StatedDate{};
  }
  if (!value->is_string()) {
    return StatedDate::unreadable();
  }
  const std::string_view text(value->as_string());
  if (text.empty()) {
    return StatedDate{};
  }
  const auto date = compact_date(text);
  return date ? StatedDate::stated(*date) : StatedDate::unreadable();
}

// Renders a calendar date as Kiwoom's `YYYYMMDD`.
inline std::string compact_text(const Date& date) {
  std::string text(8, '0');
  int year = date.year_;
  for (int index = 3; index >= 0; --index) {
    text[static_cast<std::size_t>(index)] = static_cast<char>('0' + year % 10);
    year /= 10;
  }
  text[4] = static_cast<char>('0' + date.month_ / 10);
  text[5] = static_cast<char>('0' + date.month_ % 10);
  text[6] = static_cast<char>('0' + date.day_ / 10);
  text[7] = static_cast<char>('0' + date.day_ % 10);
  return text;
}

} // namespace detail
/// \endcond

/**
 * @brief One row of Kiwoom's order-type table: the numeric request code, the Korean names its
 *   listings report, and the normalized shape.
 *
 * The unfilled and filled listings (`ka10075` 미체결, `ka10076` 체결) and history (`kt00007`, `kt00009`) name some shapes
 * differently (`보통` vs `지정가`), hence two names. These are the documented names; a server may write
 * another (the demo server's history says `보통가`, which `KiwoomClient` reads on the demo only).
 */
struct KiwoomOrderShapeRow {
  /** @brief The numeric `trde_tp` a request carries, or null for a shape this library cannot send. */
  const char* code_;
  /** @brief The name `kt10000`, `ka10075` and `ka10076` use. */
  const char* name_;
  /** @brief The name `kt00007` and `kt00009` use, or null where it is the same as `name_`. */
  const char* alternate_name_;
  /** @brief The normalized price basis. */
  OrderType type_;
  /** @brief The normalized execution condition. */
  TimeInForce time_in_force_;
  /** @brief The normalized session. */
  OrderSession session_;
  /** @brief Whether this adapter may *send* the code, as opposed to reading it. */
  bool requestable_;
};

/**
 * @brief Kiwoom's order-type (`trde_tp`) table, from its API specification.
 *
 * `시간외단일가` (after-hours single price) has no request code: it can be read, not placed. Kiwoom
 * publishes no per-venue restriction, so a venue-specific refusal comes from Kiwoom with its own
 * code.
 */
inline constexpr std::array<KiwoomOrderShapeRow, 21> kOrderShapes{{
  // code   name              alternate       type                       condition                         session
  {"0",  "보통",           "지정가",      OrderType::LIMIT,             TimeInForce::DAY,                 OrderSession::REGULAR,             true},
  {"3",  "시장가",         nullptr,       OrderType::MARKET,            TimeInForce::DAY,                 OrderSession::REGULAR,             true},
  {"5",  "조건부지정가",   nullptr,       OrderType::CONDITIONAL_LIMIT, TimeInForce::DAY,                 OrderSession::REGULAR,             true},
  {"6",  "최유리지정가",   nullptr,       OrderType::BEST_LIMIT,        TimeInForce::DAY,                 OrderSession::REGULAR,             true},
  {"7",  "최우선지정가",   nullptr,       OrderType::PRIMARY_LIMIT,     TimeInForce::DAY,                 OrderSession::REGULAR,             true},
  {"10", "보통(IOC)",      "지정가(IOC)", OrderType::LIMIT,             TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,             true},
  {"13", "시장가(IOC)",    nullptr,       OrderType::MARKET,            TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,             true},
  {"16", "최유리(IOC)",    nullptr,       OrderType::BEST_LIMIT,        TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,             true},
  {"20", "보통(FOK)",      "지정가(FOK)", OrderType::LIMIT,             TimeInForce::FILL_OR_KILL,        OrderSession::REGULAR,             true},
  {"23", "시장가(FOK)",    nullptr,       OrderType::MARKET,            TimeInForce::FILL_OR_KILL,        OrderSession::REGULAR,             true},
  {"26", "최유리(FOK)",    nullptr,       OrderType::BEST_LIMIT,        TimeInForce::FILL_OR_KILL,        OrderSession::REGULAR,             true},
  {"29", "중간가",         nullptr,       OrderType::MID_PRICE,         TimeInForce::DAY,                 OrderSession::REGULAR,             true},
  {"30", "중간가(IOC)",    nullptr,       OrderType::MID_PRICE,         TimeInForce::IMMEDIATE_OR_CANCEL, OrderSession::REGULAR,             true},
  {"31", "중간가(FOK)",    nullptr,       OrderType::MID_PRICE,         TimeInForce::FILL_OR_KILL,        OrderSession::REGULAR,             true},
  {"61", "장시작전시간외", nullptr,       OrderType::CLOSING_PRICE,     TimeInForce::DAY,                 OrderSession::PRE_MARKET,          true},
  {"81", "장마감후시간외", nullptr,       OrderType::CLOSING_PRICE,     TimeInForce::DAY,                 OrderSession::AFTER_HOURS,         true},
  {"40", "보통(GTP)",      nullptr,       OrderType::LIMIT,             TimeInForce::DAY,                 OrderSession::GTP,                 true},
  {"46", "최유리(GTP)",    nullptr,       OrderType::BEST_LIMIT,        TimeInForce::DAY,                 OrderSession::GTP,                 true},
  {"47", "최우선(GTP)",    nullptr,       OrderType::PRIMARY_LIMIT,     TimeInForce::DAY,                 OrderSession::GTP,                 true},
  {"28", "스톱지정가",     nullptr,       OrderType::STOP_LIMIT,        TimeInForce::DAY,                 OrderSession::REGULAR,             true},
  {nullptr, "시간외단일가", nullptr,      OrderType::LIMIT,             TimeInForce::DAY,                 OrderSession::AFTER_HOURS_AUCTION, false},
}};

/**
 * @brief Finds Kiwoom's order-type code for an order shape.
 * @param type The order type.
 * @param time_in_force The execution condition.
 * @param session The session.
 * @param division Set to the code on success.
 * @return An `INVALID_ARGUMENT` error (before any request) if Kiwoom has no code for the shape or
 *   the code cannot be sent, otherwise `std::nullopt`.
 */
inline std::optional<BrokerError> order_division(
  OrderType type,
  TimeInForce time_in_force,
  OrderSession session,
  std::string& division
) {
  const KiwoomOrderShapeRow* matched = nullptr;
  for (const auto& row : kOrderShapes) {
    if (row.type_ == type && row.time_in_force_ == time_in_force && row.session_ == session) {
      matched = &row;
      break;
    }
  }
  BrokerError error;
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = false;
  error.broker_code_ = "unsupported_order";
  if (matched == nullptr) {
    error.message_ =
      "Kiwoom publishes no order code for this combination of price basis, validity condition and "
      "session";
    return error;
  }
  if (!matched->requestable_ || matched->code_ == nullptr) {
    error.message_ = std::string("Kiwoom reports ") + matched->name_ +
      " on an inquiry but this library cannot place one" +
      (matched->code_ == nullptr ?
         ": it publishes no order code for that shape at all" :
         ": the code is read on a response and not sent on a request");
    return error;
  }
  division = matched->code_;
  return std::nullopt;
}

/**
 * @brief Looks up an order shape by the Korean name a response gives (either listing's spelling).
 * @param name The name.
 * @return The row, or null if neither column has it (the caller then leaves the shape `UNKNOWN`).
 */
inline const KiwoomOrderShapeRow* shape_by_name(std::string_view name) noexcept {
  for (const auto& row : kOrderShapes) {
    if (name == row.name_) {
      return &row;
    }
    if (row.alternate_name_ != nullptr && name == row.alternate_name_) {
      return &row;
    }
  }
  return nullptr;
}

/**
 * @brief Looks up an order shape by Kiwoom's numeric `trde_tp` code.
 * @param code The code.
 * @return The row, or null if the table has none.
 */
inline const KiwoomOrderShapeRow* shape_by_code(std::string_view code) noexcept {
  for (const auto& row : kOrderShapes) {
    if (row.code_ != nullptr && code == row.code_) {
      return &row;
    }
  }
  return nullptr;
}

/**
 * @brief Returns Kiwoom's `dmst_stex_tp` value for a venue.
 * @param venue The venue.
 * @return The code, or an empty string where none is sent.
 */
inline const char* exchange_code(Venue venue) noexcept {
  switch (venue) {
    case Venue::KRX:
      return "KRX";
    case Venue::NXT:
      return "NXT";
    // Kiwoom calls the consolidated book SOR, which is what routes across both venues.
    case Venue::COMBINED:
      return "SOR";
    case Venue::UNSPECIFIED:
      return "";
  }
  return "";
}

/**
 * @brief Returns the venue a Kiwoom order reaches: the one it names (Kiwoom requires one, so an
 *   order naming none is refused before sending).
 * @param asked The order's venue.
 * @return `asked`.
 */
inline constexpr Venue kiwoom_effective_route(Venue asked) noexcept { return asked; }

/**
 * @brief Converts a venue Kiwoom names on a response (`stex_tp`, `dmst_stex_tp`).
 * @param code The value.
 * @return The venue, or `UNSPECIFIED` if unrecognized.
 */
inline Venue normalized_venue(std::string_view code) noexcept {
  if (code == "KRX" || code == "1") {
    return Venue::KRX;
  }
  if (code == "NXT" || code == "2") {
    return Venue::NXT;
  }
  if (code == "SOR" || code == "0") {
    return Venue::COMBINED;
  }
  return Venue::UNSPECIFIED;
}

/**
 * @brief Returns the venue of a working-order row (`ka10075`) from its `stex_tp` and `sor_yn`.
 *
 * `sor_yn` `Y` means `Venue::COMBINED` (SOR) whatever `stex_tp` says; otherwise `stex_tp` decides.
 * How Kiwoom lists a resting SOR order has not been observed.
 *
 * @param stex_tp The row's `stex_tp`.
 * @param sor_yn The row's `sor_yn`.
 * @return The venue.
 */
inline Venue listed_venue(std::string_view stex_tp, std::string_view sor_yn) noexcept {
  if (sor_yn == "Y") {
    return Venue::COMBINED;
  }
  return normalized_venue(stex_tp);
}

/**
 * @brief Reads the side from Kiwoom's free-text `io_tp_nm` (such as `+매수`, `현금매도`,
 *   `매수정정`), by whether it contains 매수 or 매도.
 * @param text The text.
 * @return The side, or `std::nullopt` if neither (the listing being read then fails rather than
 *   default to `BUY`).
 */
inline std::optional<OrderSide> normalized_order_side(std::string_view text) noexcept {
  if (text.find("매수") != std::string_view::npos) {
    return OrderSide::BUY;
  }
  if (text.find("매도") != std::string_view::npos) {
    return OrderSide::SELL;
  }
  return std::nullopt;
}

/**
 * @brief Reads an order status from Kiwoom's Korean word.
 * @param text The word (`ord_stt` or `acpt_tp`).
 * @return `PENDING` for `접수` (received) and `확인` (confirmed), `FILLED` for `체결`, `CANCELED` for
 *   `취소`, and `UNKNOWN` for any other word.
 */
inline OrderStatus normalized_order_status(std::string_view text) noexcept {
  if (text == "접수") {
    return OrderStatus::PENDING;
  }
  if (text == "확인") {
    return OrderStatus::PENDING;
  }
  if (text == "체결") {
    return OrderStatus::FILLED;
  }
  if (text == "취소") {
    return OrderStatus::CANCELED;
  }
  return OrderStatus::UNKNOWN;
}

/**
 * @brief Returns the `api-id` of the chart endpoint for a bar interval.
 * @param interval The interval.
 * @return The id, or null where this client requests none: 5, 15, 30 and 60 minutes, which Kiwoom
 *   serves through `ka10080`'s `tic_scope` but whose row stamps are not established yet.
 */
inline constexpr const char* chart_api_id(BarInterval interval) noexcept {
  switch (interval) {
    case BarInterval::ONE_MINUTE:
      return "ka10080";
    case BarInterval::ONE_DAY:
      return "ka10081";
    case BarInterval::ONE_WEEK:
      return "ka10082";
    case BarInterval::ONE_MONTH:
      return "ka10083";
    case BarInterval::ONE_YEAR:
      return "ka10094";
    case BarInterval::FIVE_MINUTES:
    case BarInterval::FIFTEEN_MINUTES:
    case BarInterval::THIRTY_MINUTES:
    case BarInterval::ONE_HOUR:
      return nullptr;
  }
  return nullptr;
}

/**
 * @brief Returns the array field a bar interval's chart endpoint returns its rows in.
 * @param interval The interval.
 * @return The field name, or null where `chart_api_id` names no endpoint.
 */
inline constexpr const char* chart_rows_field(BarInterval interval) noexcept {
  switch (interval) {
    case BarInterval::ONE_MINUTE:
      return "stk_min_pole_chart_qry";
    case BarInterval::ONE_DAY:
      return "stk_dt_pole_chart_qry";
    case BarInterval::ONE_WEEK:
      return "stk_stk_pole_chart_qry";
    case BarInterval::ONE_MONTH:
      return "stk_mth_pole_chart_qry";
    case BarInterval::ONE_YEAR:
      return "stk_yr_pole_chart_qry";
    case BarInterval::FIVE_MINUTES:
    case BarInterval::FIFTEEN_MINUTES:
    case BarInterval::THIRTY_MINUTES:
    case BarInterval::ONE_HOUR:
      return nullptr;
  }
  return nullptr;
}

/**
 * @brief Refuses a bar interval this client requests no chart for (`chart_api_id`).
 * @param interval The interval.
 * @return An `INVALID_ARGUMENT` error (not sent), or `std::nullopt`.
 */
inline std::optional<BrokerError> unsupported_bar_interval(BarInterval interval) {
  if (chart_api_id(interval) != nullptr && chart_rows_field(interval) != nullptr) {
    return std::nullopt;
  }
  BrokerError error;
  error.category_ = ErrorCategory::INVALID_ARGUMENT;
  error.reached_broker_ = false;
  error.broker_code_ = "unsupported_interval";
  error.message_ =
    std::string("this Kiwoom client does not request ") + std::string(bar_interval_name(interval)) +
    " bars yet (Kiwoom serves them through ka10080's tic_scope; how it stamps them is not established)";
  return error;
}

/**
 * @brief Kiwoom's deposit detail (`kt00001`), as reported; `buying_power` reads `spendable_` from it,
 *   and the rest is reachable through `session.client()`.
 */
struct KiwoomDeposit {
  /**
   * @brief What the account can buy paying in full, without credit (`100stk_ord_alow_amt`);
   *   `BuyingPower::cash_` is this, not `orderable_`, which counts collateral.
   */
  Decimal64 spendable_;
  /** @brief `ord_alow_amt` 주문가능금액: orderable *including* what margin and 대용금 allow. */
  Decimal64 orderable_;
  /** @brief `entr` 예수금: the cash deposit. */
  Decimal64 deposit_;
  /** @brief `d2_entra` D+2 추정예수금: the deposit once today's trades settle. */
  Decimal64 settled_deposit_;
  /** @brief `repl_amt` 대용금: securities pledged as collateral, which is not cash. */
  Decimal64 substitute_amount_;
  /** @brief `pymn_alow_amt` 출금가능금액: what may be withdrawn. */
  Decimal64 withdrawable_;
  /** @brief The currency every figure above is in, which is won. */
  Currency currency_;
  /** @brief When the figures arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

/** @brief One margin tier of Kiwoom's `kt00010` answer: what may be ordered at that 증거금률. */
struct KiwoomMarginTier {
  /** @brief The margin rate the row applies to, as a percentage: 20, 30, 40, 50, 60 or 100. */
  int margin_percent_{0};
  /** @brief Whether this row is the 증거금감면 (reduced-margin) variant of its rate. */
  bool reduced_{false};
  /** @brief The orderable amount at this margin rate (`profa_NNord_alow_amt`), if Kiwoom reported one. */
  std::optional<Decimal64> amount_;
  /** @brief `profa_NNord_alowq`: the orderable quantity at this rate. Empty as for `amount_`. */
  std::optional<Decimal64> quantity_;
};

/**
 * @brief What may be ordered of one instrument, per margin tier (`kt00010`), reachable through
 *   `session.client()`.
 *
 * There is no single affordable quantity: which tier applies depends on the instrument's margin
 * rate, which Kiwoom does not select for you.
 */
struct KiwoomOrderableAmount {
  /**
   * @brief Always seven 증거금률 tiers: 20, 30, 40, 50, 60 and 100 %, then the 60 % 증거금감면 tier
   *   (`reduced_`). A tier's figures are empty where Kiwoom sent none.
   */
  std::vector<KiwoomMarginTier> tiers_;
  /** @brief `ord_alowa` 주문가능현금: the cash available to order with. */
  Decimal64 orderable_cash_;
  /** @brief `wthd_alowa` 인출가능금액. */
  Decimal64 withdrawable_;
  /** @brief `d2entra` D+2 추정예수금. */
  Decimal64 settled_deposit_;
  /** @brief `profa_rdex_aplc_tp`: whether the 증거금감면 tiers are the ones in force (`1`) or not (`0`). */
  bool margin_reduction_applies_{false};
  /** @brief The currency every amount above is in, which is won. */
  Currency currency_;
  /** @brief When the figures arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

/**
 * @brief The asynchronous Kiwoom REST client: market data, account inquiries and orders for domestic
 *   stocks.
 *
 * It requests an access token when first needed and shares one refresh among concurrent requests.
 * Create it with `std::make_shared`. Each completion runs on the handler's associated executor, or on
 * the client's strand if it has none. Before trading, state the holdings settings in `KiwoomConfig`.
 * A blocking wrapper is `KiwoomBlockingClient`.
 */
class KiwoomClient : public std::enable_shared_from_this<KiwoomClient> {
public:
  /** @brief Completion handler for one normalized quote. */
  using QuoteHandler = net::any_completion_handler<void(Result<QuoteSnapshot>)>;
  /** @brief Completion handler for one normalized bar series. */
  using BarsHandler = net::any_completion_handler<void(Result<BarSeries>)>;
  /** @brief Completion handler for one normalized order book. */
  using DepthHandler = net::any_completion_handler<void(Result<DepthSnapshot>)>;
  /** @brief Completion handler for one instrument's price bounds. */
  using PriceLimitsHandler = net::any_completion_handler<void(Result<PriceLimits>)>;
  /** @brief Completion handler for recent executions. */
  using TradesHandler = net::any_completion_handler<void(Result<std::vector<TradePrint>>)>;
  /** @brief Completion handler for one instrument's reference data. */
  using InstrumentHandler = net::any_completion_handler<void(Result<Instrument>)>;
  /** @brief Completion handler for one normalized holdings snapshot. */
  using HoldingsHandler = net::any_completion_handler<void(Result<HoldingsSnapshot>)>;
  /** @brief Completion handler for one normalized buying-power reading. */
  using BuyingPowerHandler = net::any_completion_handler<void(Result<BuyingPower>)>;
  /** @brief Completion handler for Kiwoom's own fuller deposit detail. */
  using DepositHandler = net::any_completion_handler<void(Result<KiwoomDeposit>)>;
  /** @brief Completion handler for Kiwoom's per-margin-tier orderable amounts and quantities. */
  using OrderableHandler = net::any_completion_handler<void(Result<KiwoomOrderableAmount>)>;
  /** @brief Completion handler for the one account the bound credential drives. */
  using AccountsHandler = net::any_completion_handler<void(Result<std::vector<KiwoomAccountInfo>>)>;
  /** @brief Completion handler for one Kiwoom order acknowledgement. */
  using OrderAckHandler = net::any_completion_handler<void(Result<KiwoomOrderAck>)>;
  /** @brief Completion handler for one page of modifiable or cancellable orders. */
  using OpenOrderHandler = net::any_completion_handler<void(Result<KiwoomOpenOrderPage>)>;
  /** @brief Completion handler for one normalized page of order history. */
  using OrderPageHandler = net::any_completion_handler<void(Result<OrderPage>)>;
  /** @brief Internal completion handler for a shared access-token refresh. */
  using TokenHandler = net::any_completion_handler<void(std::optional<BrokerError>)>;

  /**
   * @brief Constructs a client; nothing is sent until the first request.
   * @param executor The executor its strand runs on.
   * @param context The TLS context; borrowed, and must outlive the client.
   * @param config The credentials, environment and holdings settings.
   * @param options HTTP options (set `rate_limit_` to pace requests).
   */
  KiwoomClient(
    net::any_io_executor executor,
    ssl::context& context,
    KiwoomConfig config,
    network::HttpClientOptions options = {}
  )
  : strand_(net::make_strand(std::move(executor))), config_(std::move(config)),
    http_(
      std::make_shared<network::AsyncHttpClient>(
        strand_, context, config_.rest_host(), config_.rest_port(), options
      )
    ) {}

  /**
   * @brief Requests and normalizes a current quote, from `ka10007` 시세표성정보요청.
   * @tparam CompletionToken An Asio completion token.
   * @param instrument The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_quote(InstrumentId instrument, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<QuoteSnapshot>)>(
      [self = shared_from_this()](auto handler, InstrumentId subject) mutable {
        net::post(
          self->strand_,
          [self, subject = std::move(subject), handler = std::move(handler)]() mutable {
            self->start_market_read<QuoteSnapshot>(
              std::move(subject), "ka10007", "/api/dostk/mrkcond", QuoteHandler(std::move(handler)),
              &KiwoomClient::parse_quote
            );
          }
        );
      },
      token,
      std::move(instrument)
    );
  }

  /**
   * @brief Requests today's upper and lower price bound, from the same `ka10007` response.
   * @tparam CompletionToken An Asio completion token.
   * @param instrument The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_price_limits(InstrumentId instrument, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<PriceLimits>)>(
      [self = shared_from_this()](auto handler, InstrumentId subject) mutable {
        net::post(
          self->strand_,
          [self, subject = std::move(subject), handler = std::move(handler)]() mutable {
            self->start_market_read<PriceLimits>(
              std::move(subject), "ka10007", "/api/dostk/mrkcond",
              PriceLimitsHandler(std::move(handler)), &KiwoomClient::parse_price_limits
            );
          }
        );
      },
      token,
      std::move(instrument)
    );
  }

  /**
   * @brief Requests the resting orders on both sides of the book, from `ka10004` 주식호가요청.
   * @tparam CompletionToken An Asio completion token.
   * @param instrument The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_depth(InstrumentId instrument, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<DepthSnapshot>)>(
      [self = shared_from_this()](auto handler, InstrumentId subject) mutable {
        net::post(
          self->strand_,
          [self, subject = std::move(subject), handler = std::move(handler)]() mutable {
            self->start_market_read<DepthSnapshot>(
              std::move(subject), "ka10004", "/api/dostk/mrkcond", DepthHandler(std::move(handler)),
              &KiwoomClient::parse_depth
            );
          }
        );
      },
      token,
      std::move(instrument)
    );
  }

  /**
   * @brief Requests recent executions, newest first, from `ka10003` 체결정보요청.
   * @tparam CompletionToken An Asio completion token.
   * @param instrument The instrument.
   * @param count The most trades to return.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_trades(InstrumentId instrument, int count, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<std::vector<TradePrint>>)>(
      [self = shared_from_this()](auto handler, InstrumentId subject, int wanted) mutable {
        net::post(
          self->strand_,
          [self, subject = std::move(subject), wanted, handler = std::move(handler)]() mutable {
            self->start_trades(std::move(subject), wanted, TradesHandler(std::move(handler)));
          }
        );
      },
      token,
      std::move(instrument),
      count
    );
  }

  /**
   * @brief Requests static reference data for one instrument, from `ka10100` 종목정보 조회.
   * @tparam CompletionToken An Asio completion token.
   * @param instrument The instrument.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_instrument(InstrumentId instrument, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<Instrument>)>(
      [self = shared_from_this()](auto handler, InstrumentId subject) mutable {
        net::post(
          self->strand_,
          [self, subject = std::move(subject), handler = std::move(handler)]() mutable {
            self->start_instrument(std::move(subject), InstrumentHandler(std::move(handler)));
          }
        );
      },
      token,
      std::move(instrument)
    );
  }

  /**
   * @brief Requests bars, following Kiwoom's continuation pages from `before_`'s Korean date (or today).
   *
   * `before_` applies by day only: minute bars later than it on the same date are returned. A short
   * series comes back only when Kiwoom has no more. `next_cursor_` is set when the series is full and
   * Kiwoom said there is more, and never for minute bars. If Kiwoom offers more but its continuation key is blank,
   * a page adds nothing, or 50 pages were read, the request fails (`PROTOCOL`, `incomplete_listing`)
   * instead of returning a partial series. Refused before sending: `count_` of zero or less, or a
   * symbol that is not Korean or malformed.
   *
   * @tparam CompletionToken An Asio completion token.
   * @param query The query.
   * @param token The completion token, called with `Result<BarSeries>`.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_bars(BarQuery query, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<BarSeries>)>(
      [self = shared_from_this()](auto handler, BarQuery wanted) mutable {
        net::post(
          self->strand_,
          [self, wanted = std::move(wanted), handler = std::move(handler)]() mutable {
            self->start_bars(std::move(wanted), BarsHandler(std::move(handler)));
          }
        );
      },
      token,
      std::move(query)
    );
  }

  /**
   * @brief Returns the configuration (the session reads its holdings assertions from it).
   * @return The configuration.
   */
  const KiwoomConfig& config() const noexcept { return config_; }

  /**
   * @brief Returns the client's strand.
   * @return The strand.
   */
  net::strand<net::any_io_executor> get_executor() const noexcept { return strand_; }

  /**
   * @brief Requests the holdings `kt00018` lists for `KiwoomConfig::holdings_venue_` (refused before
   *   sending until it is `KRX` or `NXT`); whether that is every holding is what
   *   `KiwoomConfig::holdings_position_scope_` asserts, which this call does not check.
   *
   * The snapshot fails if one symbol appears on two rows or paging stops while Kiwoom offers more. Its
   * `sellable_basis_` follows `KiwoomConfig::holdings_sellable_scope_`.
   *
   * @tparam CompletionToken An Asio completion token.
   * @param account The account (empty).
   * @param token The completion token, called with `Result<HoldingsSnapshot>`.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_holdings(KiwoomAccount account, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<HoldingsSnapshot>)>(
      [self = shared_from_this()](auto handler, KiwoomAccount) mutable {
        net::post(
          self->strand_,
          [self, handler = std::move(handler)]() mutable {
            self->start_holdings(HoldingsHandler(std::move(handler)));
          }
        );
      },
      token,
      account
    );
  }

  /**
   * @brief Requests what the account may spend, from `kt00001` 예수금상세현황요청.
   * @tparam CompletionToken An Asio completion token.
   * @param currency The currency; anything but KRW is refused before sending.
   * @param account The bound account.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_buying_power(Currency currency, KiwoomAccount account, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<BuyingPower>)>(
      [self = shared_from_this()](auto handler, Currency wanted, KiwoomAccount) mutable {
        net::post(
          self->strand_,
          [self, wanted, handler = std::move(handler)]() mutable {
            self->start_buying_power(wanted, BuyingPowerHandler(std::move(handler)));
          }
        );
      },
      token,
      currency,
      account
    );
  }

  /**
   * @brief Requests Kiwoom's own fuller deposit detail, which `async_buying_power` reads one field of.
   * @tparam CompletionToken An Asio completion token.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_deposit(CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<KiwoomDeposit>)>(
      [self = shared_from_this()](auto handler) mutable {
        net::post(
          self->strand_,
          [self, handler = std::move(handler)]() mutable {
            self->start_deposit(DepositHandler(std::move(handler)));
          }
        );
      },
      token
    );
  }

  /**
   * @brief Requests what may be ordered of one instrument, per margin tier (`kt00010`).
   * @tparam CompletionToken An Asio completion token.
   * @param instrument The instrument.
   * @param side The side.
   * @param price The unit price to evaluate at (`uv`), or empty to send none.
   * @param token The completion token, called with `Result<KiwoomOrderableAmount>`.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_orderable_amount(
    InstrumentId instrument,
    OrderSide side,
    std::optional<Decimal64> price,
    CompletionToken&& token
  ) {
    return net::async_initiate<CompletionToken, void(Result<KiwoomOrderableAmount>)>(
      [self = shared_from_this()](
        auto handler, InstrumentId subject, OrderSide direction, std::optional<Decimal64> limit
      ) mutable {
        net::post(
          self->strand_,
          [
            self, subject = std::move(subject), direction, limit = std::move(limit),
            handler = std::move(handler)
          ]() mutable {
            self->start_orderable_amount(
              std::move(subject), direction, std::move(limit), OrderableHandler(std::move(handler))
            );
          }
        );
      },
      token,
      std::move(instrument),
      side,
      std::move(price)
    );
  }

  /**
   * @brief Requests the one account the bound credential drives, from `ka00001` 계좌번호조회.
   * @tparam CompletionToken An Asio completion token.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_accounts(CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<std::vector<KiwoomAccountInfo>>)>(
      [self = shared_from_this()](auto handler) mutable {
        net::post(
          self->strand_,
          [self, handler = std::move(handler)]() mutable {
            self->start_accounts(AccountsHandler(std::move(handler)));
          }
        );
      },
      token
    );
  }

  /**
   * @brief Places an order (`kt10000` buy, `kt10001` sell).
   *
   * The request is never resent. After an error for which `runner::outcome_of` returns
   * `INDETERMINATE` (`BrokerError::reached_broker_` set and a category of `TRANSPORT`, `CANCELLED`
   * or `PROTOCOL`, or `HTTP` with a status of 500 or more), the order may exist. That includes a
   * reply without an order number and an unrecognized failure code Kiwoom declares but does not
   * document (both `PROTOCOL`). Read `async_open_orders` or `async_order_history` before placing
   * again, since Kiwoom has no idempotency key.
   *
   * Some refusals are recognized as refusals, each from its own server only: the demo server's
   * `800033` (not enough to sell), `RC4057` (the market has not opened) and `RC4058` (the market has
   * closed), and the real server's `571445`
   * (no securities balance), are `INVALID_ARGUMENT` with `reached_broker_` set, which
   * `runner::outcome_of` reads as `REJECTED` (`kiwoom_recognized_refusal`). A demo code from the real
   * server, or the real code from the demo server, stays `PROTOCOL`, as does every other refusal this
   * client does not classify further (its documented error codes keep their categories).
   *
   * Refused before sending, among others: an instrument that is not Korean or whose `venue_` is not
   * `COMBINED` (route with `OrderRequest::venue_`); no venue on the order; a client order id; a
   * quantity that is not a positive whole number; a missing, non-positive or fractional price on a
   * priced type, or a price on an unpriced one; a trigger price missing or invalid on `STOP_LIMIT`,
   * or present on another type; a shape Kiwoom has no code for; and any order while
   * `KiwoomConfig::holdings_position_scope_` is not stated.
   *
   * @tparam CompletionToken An Asio completion token.
   * @param account The account (empty).
   * @param order The order.
   * @param token The completion token, called with `Result<KiwoomOrderAck>`.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_place_order(KiwoomAccount account, OrderRequest order, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<KiwoomOrderAck>)>(
      [self = shared_from_this()](auto handler, KiwoomAccount, OrderRequest placed) mutable {
        net::post(
          self->strand_,
          [self, placed = std::move(placed), handler = std::move(handler)]() mutable {
            self->start_place_order(std::move(placed), OrderAckHandler(std::move(handler)));
          }
        );
      },
      token,
      account,
      std::move(order)
    );
  }

  /**
   * @brief Amends an order's quantity, price or trigger price (`kt10002`).
   *
   * After an error of unknown outcome (see `async_place_order`) the amendment may have been made.
   * Refused before sending, among others: an amendment without a venue, an order number or a
   * symbol; a fractional or negative quantity; a fractional price; an invalid trigger price; and any
   * amendment while `KiwoomConfig::holdings_position_scope_` is not stated.
   *
   * @tparam CompletionToken An Asio completion token.
   * @param account The account (empty).
   * @param amendment The amendment.
   * @param token The completion token, called with `Result<KiwoomOrderAck>`.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_replace_order(
    KiwoomAccount account,
    KiwoomAmendment amendment,
    CompletionToken&& token
  ) {
    return net::async_initiate<CompletionToken, void(Result<KiwoomOrderAck>)>(
      [self = shared_from_this()](auto handler, KiwoomAccount, KiwoomAmendment change) mutable {
        net::post(
          self->strand_,
          [self, change = std::move(change), handler = std::move(handler)]() mutable {
            self->start_amend_order(
              std::move(change), false, OrderAckHandler(std::move(handler))
            );
          }
        );
      },
      token,
      account,
      std::move(amendment)
    );
  }

  /**
   * @brief Cancels an order, all or part of it (`kt10003`).
   *
   * After an error of unknown outcome (see `async_place_order`) the order may still rest. Refused
   * before sending, among others: a cancellation without a venue, an order number or a symbol; a
   * fractional or negative quantity; or one carrying a price or trigger price.
   *
   * Two refusals are recognized as refusals, each from its own server only: the demo server's
   * `RC4033` (nothing left to amend or cancel) and the real server's `506550` (no quantity left to
   * cancel) are `INVALID_ARGUMENT` with `reached_broker_` set (`kiwoom_recognized_refusal`). Each says
   * the cancellation did nothing, not what became of the order: it may have filled. Either code from
   * the other server stays `PROTOCOL`.
   *
   * @tparam CompletionToken An Asio completion token.
   * @param account The account (empty).
   * @param amendment The order and quantity (zero for all remaining).
   * @param token The completion token, called with `Result<KiwoomOrderAck>`.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_cancel_order(
    KiwoomAccount account,
    KiwoomAmendment amendment,
    CompletionToken&& token
  ) {
    return net::async_initiate<CompletionToken, void(Result<KiwoomOrderAck>)>(
      [self = shared_from_this()](auto handler, KiwoomAccount, KiwoomAmendment change) mutable {
        net::post(
          self->strand_,
          [self, change = std::move(change), handler = std::move(handler)]() mutable {
            self->start_amend_order(std::move(change), true, OrderAckHandler(std::move(handler)));
          }
        );
      },
      token,
      account,
      std::move(amendment)
    );
  }

  /**
   * @brief Requests every order still open to amendment, paging `ka10075` 미체결요청.
   * @tparam CompletionToken An Asio completion token.
   * @param account The bound account.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_open_orders(KiwoomAccount account, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(Result<KiwoomOpenOrderPage>)>(
      [self = shared_from_this()](auto handler, KiwoomAccount) mutable {
        net::post(
          self->strand_,
          [self, handler = std::move(handler)]() mutable {
            self->start_open_orders(OpenOrderHandler(std::move(handler)));
          }
        );
      },
      token,
      account
    );
  }

  /**
   * @brief Requests one day of order history, paging `kt00007` 계좌별주문체결내역상세요청.
   * @tparam CompletionToken An Asio completion token.
   * @param account The bound account.
   * @param query The query.
   * @param token The completion token.
   * @return Whatever the completion token returns.
   */
  template <typename CompletionToken>
  auto async_order_history(
    KiwoomAccount account,
    KiwoomHistoryQuery query,
    CompletionToken&& token
  ) {
    return net::async_initiate<CompletionToken, void(Result<OrderPage>)>(
      [self = shared_from_this()](auto handler, KiwoomAccount, KiwoomHistoryQuery wanted) mutable {
        net::post(
          self->strand_,
          [self, wanted = std::move(wanted), handler = std::move(handler)]() mutable {
            self->start_order_history(std::move(wanted), OrderPageHandler(std::move(handler)));
          }
        );
      },
      token,
      account,
      std::move(query)
    );
  }

  /**
   * @brief Returns the rate limiter's statistics.
   *
   * Read them on the client's strand or while no request is in progress (between calls on the
   * blocking client).
   *
   * @return The statistics, or null if the client has no rate-limit budget.
   */
  const network::RateLimitStats* rate_limit_stats() const {
    const auto* limiter = http_->rate_limits();
    return limiter ? &limiter->stats() : nullptr;
  }

  /**
   * @brief Closes the client for good: pending requests, and later ones that would send, complete
   *   with `CANCELLED`. A request already being sent has `reached_broker_` set and an unknown outcome.
   *   A request refused before sending is still refused.
   */
  void close() { http_->close(); }

private:
  friend struct KiwoomClientTestAccess;

  net::strand<net::any_io_executor> strand_;
  KiwoomConfig config_;
  std::shared_ptr<network::AsyncHttpClient> http_;
  KiwoomRest token_;
  bool refreshing_{false};
  std::deque<TokenHandler> token_waiters_;

  // Most pages one paged inquiry may consume, for the reason every paging path is bounded.
  static constexpr int page_budget_ = 50;

  // Builds one Kiwoom request: the group path, the operation header, and the token.
  //
  // **Every Kiwoom endpoint has this shape**, which is why it is built once. The operation is the
  // `api-id` header rather than the path — `/api/dostk/acnt` alone serves the balance, the deposit,
  // the unfilled listing and the execution detail — so a request with the right path and the wrong
  // header is a valid request for the wrong thing, and there is exactly one place that pairs them.
  network::HttpRequest make_request(
    std::string_view api_id,
    std::string_view path,
    std::string body,
    network::RateLimitGroup group,
    std::string_view next_key = {}
  ) const {
    network::HttpRequest request;
    request.group_ = group;
    // Every Kiwoom read is a POST, including the market-data ones. A POST is never replayed after
    // a reconnect, which is the transport's own rule and the conservative one here.
    request.method_ = http::verb::post;
    request.target_ = std::string(path);
    request.body_ = std::move(body);
    request.headers_ = {
      {"authorization", "Bearer " + token_.access_token_},
      {"api-id", std::string(api_id)},
      {"content-type", "application/json;charset=UTF-8"},
    };
    if (!next_key.empty()) {
      request.headers_.emplace_back("cont-yn", "Y");
      request.headers_.emplace_back("next-key", std::string(next_key));
    }
    return request;
  }

  // Returns the continuation a response carries: nothing when it reports no further page.
  //
  // **An absent value and an empty one are different answers**, which is why this is an optional
  // rather than a string. `cont-yn: Y` means Kiwoom has another page; `next-key` is how to ask for
  // it. A `Y` with a missing or blank key is therefore *another page this walk cannot fetch* — and
  // collapsing that into "no further page", which returning a bare empty string did, turned a
  // listing the adapter could not finish into one it reported as complete.
  //
  // - `std::nullopt` — Kiwoom offered no further page. The listing is complete.
  // - a non-empty string — another page, and the key to ask for it.
  // - an empty string — another page, and no usable key. Incomplete.
  static std::optional<std::string> continuation_key(const network::HttpResponse& response) {
    const auto flag = response.base().find("cont-yn");
    if (flag == response.base().end() || flag->value() != "Y") {
      return std::nullopt;
    }
    const auto key = response.base().find("next-key");
    if (key == response.base().end()) {
      return std::string();
    }
    return std::string(key->value());
  }

  // Fetches a token when the held one has expired, coalescing concurrent waiters.
  //
  // Identical in shape to the KIS and Toss clients: at most one refresh is ever in flight, and
  // every other request waits behind it.
  void ensure_token(TokenHandler handler) {
    if (token_.valid()) {
      std::move(handler)(std::nullopt);
      return;
    }
    token_waiters_.push_back(std::move(handler));
    if (refreshing_) {
      return;
    }
    refreshing_ = true;

    network::HttpRequest request;
    request.group_ = network::RateLimitGroup::AUTH;
    request.method_ = http::verb::post;
    request.target_ = "/oauth2/token";
    request.body_ = config_.to_jsonbody();
    request.headers_.emplace_back("content-type", "application/json;charset=UTF-8");
    http_->async_request(
      std::move(request),
      [self = shared_from_this()](Result<network::HttpResponse> result) mutable {
        net::post(self->strand_, [self, result = std::move(result)]() mutable {
          self->finish_token(std::move(result));
        });
      }
    );
  }

  // Parses a token response and releases every waiter, with the failure if there was one.
  void finish_token(Result<network::HttpResponse> result) {
    refreshing_ = false;
    std::optional<BrokerError> failure;
    if (!result) {
      failure = std::move(result.error());
    } else if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      failure = kiwoom_http_error(result.value());
      failure->category_ = ErrorCategory::AUTHENTICATION;
    } else {
      BrokerError parse_error;
      auto document = broker_detail::parse_object(result.value(), parse_error);
      if (!document) {
        failure = std::move(parse_error);
      } else if (auto declared = kiwoom_declared_error(*document)) {
        // Kiwoom answers a refused token with HTTP 200 and a non-zero return_code, so the status
        // says nothing. `8020`, `8011` and `8012` all land here.
        failure = std::move(declared);
      } else {
        try {
          token_.from_json(*document);
        } catch (const std::exception& exception) {
          BrokerError error;
          error.category_ = ErrorCategory::AUTHENTICATION;
          error.broker_code_ = "invalid_token_response";
          error.message_ = exception.what();
          failure = std::move(error);
        }
      }
    }

    if (failure) {
      // The waiters asked for something else, and none of their requests was sent. Same marking as
      // the KIS and Toss clients: an unmarked authentication failure reads to a runner as the
      // broker refusing an order it never received, spending rate-limit budget on nothing.
      failure->reached_broker_ = false;
    }
    auto waiters = std::move(token_waiters_);
    token_waiters_.clear();
    for (auto& waiter : waiters) {
      if (failure) {
        std::move(waiter)(*failure);
      } else {
        std::move(waiter)(std::nullopt);
      }
    }
  }

  // Returns the error for a non-2xx reply: `broker_detail::http_error`, with Kiwoom's `return_code` read
  // from the whole body into `broker_code_` when the body is an object that carries one (the message
  // keeps only the body's start, so a code past it would otherwise be lost). The category stays what
  // the HTTP status says, unlike a 2xx reply's (`kiwoom_declared_error`): on an order, a rate-limit
  // category would turn an unknown 5xx outcome into a refusal (`kis_http_error` keeps orders out of its
  // mapping for that reason), and no Kiwoom non-2xx reply has been observed.
  static BrokerError kiwoom_http_error(const network::HttpResponse& response) {
    auto error = broker_detail::http_error(response);
    boost::system::error_code ec;
    const auto document = json::parse(response.body(), ec);
    if (ec || !document.is_object()) {
      return error;
    }
    if (const auto* code = document.as_object().if_contains("return_code")) {
      if (code->is_int64()) {
        error.broker_code_ = std::to_string(code->as_int64());
      } else if (code->is_uint64()) {
        error.broker_code_ = std::to_string(code->as_uint64());
      } else if (code->is_string()) {
        error.broker_code_ = std::string(code->as_string());
      }
    }
    return error;
  }

  // Returns a response's declared payload, or the error ending it.
  //
  // Applies the HTTP status check, the JSON parse, and Kiwoom's own `return_code` before a parser
  // looks at any field, so a declared failure is never read as data. Only an order reply states an
  // operation: for every other caller the default keeps a refusal from being recognized.
  static Result<json::object> document(
    Result<network::HttpResponse> response,
    KiwoomOrderOperation operation = KiwoomOrderOperation::UNCLASSIFIED,
    bool demo = false
  ) {
    if (!response) {
      return Result<json::object>(std::move(response.error()));
    }
    if (response.value().result_int() < 200 || response.value().result_int() >= 300) {
      return Result<json::object>(kiwoom_http_error(response.value()));
    }
    BrokerError parse_error;
    auto parsed = broker_detail::parse_object(response.value(), parse_error);
    if (!parsed) {
      return Result<json::object>(std::move(parse_error));
    }
    if (auto refused = kiwoom_recognized_refusal(response.value().result_int(), *parsed, operation, demo)) {
      return Result<json::object>(std::move(*refused));
    }
    if (auto declared = kiwoom_declared_error(*parsed)) {
      return Result<json::object>(std::move(*declared));
    }
    return Result<json::object>(std::move(*parsed));
  }

  // Refuses an instrument this adapter's domestic endpoints cannot address.
  static std::optional<BrokerError> validate_instrument(const InstrumentId& instrument) {
    if (instrument.country_ == Country::KR && broker_detail::valid_symbol(instrument.symbol_) &&
        (instrument.symbol_.size() == 6 || instrument.symbol_.size() == 7)) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "invalid_instrument";
    error.message_ =
      "this adapter covers Kiwoom's domestic market, whose instruments are addressed by a six- or "
      "seven-character code";
    return error;
  }

  // Builds the `{"stk_cd": ...}` body every market-data endpoint takes, with the tape suffix.
  std::string symbol_body(const InstrumentId& instrument) const {
    json::object body;
    body["stk_cd"] = config_.tape_symbol(instrument.symbol_);
    return json::serialize(body);
  }

  // Issues one market-data read and hands the response to `parse`.
  template <typename T, typename Handler, typename Parse>
  void start_market_read(
    InstrumentId instrument,
    std::string api_id,
    std::string path,
    Handler completion,
    Parse parse
  ) {
    if (auto invalid = validate_instrument(instrument)) {
      deliver(std::move(completion), Result<T>(std::move(*invalid)));
      return;
    }
    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(), instrument = std::move(instrument),
          api_id = std::move(api_id), path = std::move(path), completion = std::move(completion),
          parse
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(std::move(completion), Result<T>(std::move(*token_error)));
            return;
          }
          auto request = self->make_request(
            api_id, path, self->symbol_body(instrument), network::RateLimitGroup::MARKET_DATA
          );
          self->http_->async_request(
            std::move(request),
            [self, instrument = std::move(instrument), completion = std::move(completion), parse](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(
                std::move(completion), parse(std::move(instrument), std::move(result))
              );
            }
          );
        }
      )
    );
  }

  // Normalizes `ka10007` into a quote.
  //
  // The last price, the volume and the top of the book all come off one response, so the quote
  // carries a `top_of_book_` — which neither shipped adapter fills, because neither KIS's nor
  // Toss's quote endpoint publishes one. Kiwoom's does, in the same response, at no extra request.
  static Result<QuoteSnapshot> parse_quote(
    InstrumentId instrument,
    Result<network::HttpResponse> result
  ) {
    auto body = document(std::move(result));
    if (!body) {
      return Result<QuoteSnapshot>(std::move(body.error()));
    }
    try {
      const auto& row = body.value();
      QuoteSnapshot quote;
      quote.instrument_ = std::move(instrument);
      quote.currency_ = Currency::krw();
      const auto last = detail::magnitude(row, "cur_prc");
      if (!last) {
        throw std::runtime_error("missing or unreadable field: cur_prc");
      }
      quote.last_price_ = *last;
      quote.volume_ = detail::magnitude(row, "trde_qty");
      quote.received_at_ = std::chrono::system_clock::now();
      const auto date = detail::compact_date(detail::text_field(row, "date"));
      if (date) {
        quote.exchange_time_ = detail::seoul_timestamp(*date, detail::text_field(row, "tm"));
      }
      TopOfBook top;
      top.ask_price_ = detail::magnitude(row, "sel_1bid");
      top.ask_quantity_ = detail::magnitude(row, "sel_1bid_req");
      top.bid_price_ = detail::magnitude(row, "buy_1bid");
      top.bid_quantity_ = detail::magnitude(row, "buy_1bid_req");
      if (top.ask_price_ || top.bid_price_) {
        quote.top_of_book_ = top;
      }
      return Result<QuoteSnapshot>(std::move(quote));
    } catch (const std::exception& exception) {
      return Result<QuoteSnapshot>(protocol_error("invalid_quote_response", exception));
    }
  }

  // Normalizes `ka10007`'s price bounds.
  //
  // KRX sets a limit on every listed equity, so both bounds are expected; a blank or zero bound is
  // reported as absent rather than as a bound of nothing.
  static Result<PriceLimits> parse_price_limits(
    InstrumentId instrument,
    Result<network::HttpResponse> result
  ) {
    static_cast<void>(instrument);
    auto body = document(std::move(result));
    if (!body) {
      return Result<PriceLimits>(std::move(body.error()));
    }
    try {
      const auto& row = body.value();
      PriceLimits limits;
      limits.currency_ = Currency::krw();
      limits.upper_ = positive(detail::magnitude(row, "upl_pric"));
      limits.lower_ = positive(detail::magnitude(row, "lst_pric"));
      const auto date = detail::compact_date(detail::text_field(row, "date"));
      if (date) {
        limits.exchange_time_ = detail::seoul_timestamp(*date, detail::text_field(row, "tm"));
      }
      return Result<PriceLimits>(std::move(limits));
    } catch (const std::exception& exception) {
      return Result<PriceLimits>(protocol_error("invalid_price_limit_response", exception));
    }
  }

  // Normalizes `ka10004` into the ten levels each side.
  //
  // Kiwoom names the first level differently from the rest — `sel_fpr_bid` and `buy_fpr_bid` for
  // level one, `sel_2th_pre_bid` through `sel_10th_pre_bid` for the others — so the field names are
  // built rather than listed, and level one is spelled out.
  static Result<DepthSnapshot> parse_depth(
    InstrumentId instrument,
    Result<network::HttpResponse> result
  ) {
    auto body = document(std::move(result));
    if (!body) {
      return Result<DepthSnapshot>(std::move(body.error()));
    }
    try {
      const auto& row = body.value();
      DepthSnapshot book;
      book.instrument_ = std::move(instrument);
      book.currency_ = Currency::krw();
      book.received_at_ = std::chrono::system_clock::now();
      book.exchange_time_ =
        detail::seoul_timestamp(today_in_seoul(), detail::text_field(row, "bid_req_base_tm"));
      append_level(book.asks_, row, "sel_fpr_bid", "sel_fpr_req");
      append_level(book.bids_, row, "buy_fpr_bid", "buy_fpr_req");
      for (int level = 2; level <= 10; ++level) {
        const std::string suffix = std::to_string(level) + "th_pre_";
        append_level(book.asks_, row, "sel_" + suffix + "bid", "sel_" + suffix + "req");
        append_level(book.bids_, row, "buy_" + suffix + "bid", "buy_" + suffix + "req");
      }
      order_depth(book);
      return Result<DepthSnapshot>(std::move(book));
    } catch (const std::exception& exception) {
      return Result<DepthSnapshot>(protocol_error("invalid_depth_response", exception));
    }
  }

  // Appends one book level, skipping the empty ones Kiwoom pads a thin book with.
  static void append_level(
    std::vector<DepthLevel>& side,
    const json::object& row,
    const std::string& price_key,
    const std::string& quantity_key
  ) {
    const auto price = detail::magnitude(row, price_key);
    const auto quantity = detail::magnitude(row, quantity_key);
    if (!price || price->coefficient() == 0) {
      return;
    }
    DepthLevel level;
    level.price_ = *price;
    level.quantity_ = quantity ? *quantity : Decimal64();
    side.push_back(level);
  }

  // Returns a decimal only when it is strictly positive, which is how Kiwoom spells "no value".
  static std::optional<Decimal64> positive(std::optional<Decimal64> value) {
    if (!value || value->coefficient() <= 0) {
      return std::nullopt;
    }
    return value;
  }

  // Returns an order's trigger from `key`, or nothing.
  //
  // `ka10075`'s `stop_pric` and `kt00007`'s `cond_uv` are both documented as "스톱지정가주문
  // 스톱가", and `kt00007`'s own example carries `"0000000000"` on a market order -- so an ordinary
  // order reports zero, and a positive value is recorded **whatever the row's basis**, for the
  // reason `OrderSnapshot::trigger_price_` gives. An empty, zero or unreadable value leaves the
  // trigger unreported rather than failing the listing: a trigger is one fact about one order, and
  // the row is still the order, which must stay cancellable.
  static std::optional<Decimal64> reported_trigger(const json::object& row, std::string_view key) {
    try {
      return positive(detail::magnitude(row, key));
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }

  // Builds a `PROTOCOL` failure from a parser exception.
  static BrokerError protocol_error(const char* code, const std::exception& exception) {
    BrokerError error;
    error.category_ = ErrorCategory::PROTOCOL;
    error.broker_code_ = code;
    error.message_ = exception.what();
    return error;
  }

  // Delivers `result` to `handler` on the strand, exactly once.
  template <typename Handler, typename T>
  void deliver(Handler handler, Result<T> result) {
    if (!result) {
      note_rate_limit(result.error());
    }
    auto executor = net::get_associated_executor(handler, strand_);
    net::dispatch(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
      std::move(handler)(std::move(result));
    });
  }

  // Holds admission after one of Kiwoom's three ceilings refused a request.
  //
  // Kiwoom documents the ceilings separately — per API (`1700`), account-wide (`1701`) and per API
  // group (`1702`) — but states **no allowance and no retry-after** with any of them, so there is
  // nothing to correct the budget *to*. Admission is held for a second and the refusal is counted;
  // the configured budget is left exactly as the caller set it, for the reason the KIS adapter
  // gives: reducing a stated budget from a guess is the silent downgrade this library refuses
  // everywhere else. `KiwoomRateLimitLayer` is what tells a caller watching the refusals *which*
  // ceiling to lower.
  void note_rate_limit(const BrokerError& error) {
    if (!error_category_is_rate_limit(error.category_)) {
      return;
    }
    auto* limiter = http_->rate_limits();
    if (limiter == nullptr) {
      return;
    }
    limiter->hold(
      error.retry_after_ ?
        std::chrono::duration_cast<network::RateLimitController::Duration>(*error.retry_after_) :
        std::chrono::duration_cast<network::RateLimitController::Duration>(std::chrono::seconds(1))
    );
  }

  // Issues the recent-execution read, whose parser needs the requested count.
  void start_trades(InstrumentId instrument, int count, TradesHandler completion) {
    using Prints = std::vector<TradePrint>;
    if (count <= 0) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_count";
      error.message_ = "a recent-execution request must ask for at least one print";
      deliver(std::move(completion), Result<Prints>(std::move(error)));
      return;
    }
    if (auto invalid = validate_instrument(instrument)) {
      deliver(std::move(completion), Result<Prints>(std::move(*invalid)));
      return;
    }
    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(), instrument = std::move(instrument), count,
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(std::move(completion), Result<Prints>(std::move(*token_error)));
            return;
          }
          auto request = self->make_request(
            "ka10003", "/api/dostk/stkinfo", self->symbol_body(instrument),
            network::RateLimitGroup::MARKET_DATA
          );
          self->http_->async_request(
            std::move(request),
            [self, count, completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(std::move(completion), parse_trades(std::move(result), count));
            }
          );
        }
      )
    );
  }

  // Normalizes `ka10003`'s recent executions, newest first as Kiwoom sends them.
  //
  // Kiwoom reports a print's time as `HHmmss` with no date, so the KST calendar day is supplied
  // from the clock. A print is intraday by construction, so this is the day it happened on — the
  // same reasoning the KIS adapter records for the same field.
  static Result<std::vector<TradePrint>> parse_trades(
    Result<network::HttpResponse> result,
    int count
  ) {
    using Prints = std::vector<TradePrint>;
    auto body = document(std::move(result));
    if (!body) {
      return Result<Prints>(std::move(body.error()));
    }
    try {
      const auto rows = body.value().if_contains("cntr_infr");
      if (!rows || !rows->is_array()) {
        throw std::runtime_error("missing array field: cntr_infr");
      }
      const auto today = today_in_seoul();
      Prints prints;
      for (const auto& value : rows->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("cntr_infr row is not an object");
        }
        if (static_cast<int>(prints.size()) >= count) {
          break;
        }
        const auto& row = value.as_object();
        const auto price = detail::magnitude(row, "cur_prc");
        const auto quantity = detail::magnitude(row, "cntr_trde_qty");
        if (!price || !quantity) {
          continue;
        }
        TradePrint print;
        print.price_ = *price;
        print.quantity_ = *quantity;
        print.currency_ = Currency::krw();
        print.exchange_time_ = detail::seoul_timestamp(today, detail::text_field(row, "tm"));
        prints.push_back(print);
      }
      return Result<Prints>(std::move(prints));
    } catch (const std::exception& exception) {
      return Result<Prints>(protocol_error("invalid_trades_response", exception));
    }
  }

  // Issues the reference-data read, which takes the bare symbol rather than a tape-suffixed one.
  void start_instrument(InstrumentId instrument, InstrumentHandler completion) {
    if (auto invalid = validate_instrument(instrument)) {
      deliver(std::move(completion), Result<Instrument>(std::move(*invalid)));
      return;
    }
    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(), instrument = std::move(instrument),
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(std::move(completion), Result<Instrument>(std::move(*token_error)));
            return;
          }
          // `ka10100` documents `stk_cd` as 종목코드 6자리 and names no tape, unlike every
          // market-data endpoint. Reference data is not a price, so it has no book to come off.
          json::object body;
          body["stk_cd"] = instrument.symbol_;
          auto request = self->make_request(
            "ka10100", "/api/dostk/stkinfo", json::serialize(body),
            network::RateLimitGroup::REFERENCE
          );
          self->http_->async_request(
            std::move(request),
            [self, instrument = std::move(instrument), completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(
                std::move(completion), parse_instrument(std::move(instrument), std::move(result))
              );
            }
          );
        }
      )
    );
  }

  // Normalizes `ka10100` into reference data.
  static Result<Instrument> parse_instrument(
    InstrumentId instrument,
    Result<network::HttpResponse> result
  ) {
    auto body = document(std::move(result));
    if (!body) {
      return Result<Instrument>(std::move(body.error()));
    }
    try {
      const auto& row = body.value();
      Instrument subject;
      subject.id_ = std::move(instrument);
      subject.name_ = detail::text_field(row, "name");
      subject.currency_ = Currency::krw();
      subject.segment_ = kiwoom_market_segment(detail::text_field(row, "marketCode"));
      subject.type_ = SecurityType::STOCK;
      subject.status_ = kiwoom_status(detail::text_field(row, "auditInfo"));
      subject.shares_outstanding_ = positive(detail::magnitude(row, "listCount"));
      subject.listed_on_ = detail::stated_compact_date(row, "regDay");
      subject.price_rule_ = price_rule_for(subject.id_.country_, subject.type_);
      return Result<Instrument>(std::move(subject));
    } catch (const std::exception& exception) {
      return Result<Instrument>(protocol_error("invalid_instrument_response", exception));
    }
  }

  // Maps Kiwoom's `marketCode` to a normalized segment.
  //
  // `0` is 거래소 (KOSPI) and `10` is 코스닥, which `ka10099` documents as its 시장구분 values. The
  // rest of that list — K-OTC, 코넥스, ETN, ELW and the funds — has no `MarketSegment::Known`
  // member, so the raw code is preserved through `market_segment` rather than collapsed into one
  // of the two this library does know.
  static MarketSegment kiwoom_market_segment(std::string_view code) {
    if (code == "0") {
      return market_segment("KOSPI");
    }
    if (code == "10") {
      return market_segment("KOSDAQ");
    }
    return market_segment(code);
  }

  // Reads a listing state from `ka10100`'s `auditInfo`.
  //
  // Kiwoom publishes 감리구분 as a Korean phrase — the field lists `정상`, `관리종목`, `거래정지`,
  // `단기과열`, `투자주의`, `투자경고`, and the `ka10099` example writes `투자주의환기종목` — and publishes no
  // delisting date and no `SCHEDULED` state at all. Everything it reports is a listed instrument
  // under some degree of supervision (a halt included), so the honest reading is `ACTIVE` for the
  // phrases it publishes and `UNKNOWN` for any other: supervision is not delisting, and inventing a
  // `DELISTED` from a warning would be worse than saying nothing.
  static InstrumentStatus kiwoom_status(std::string_view audit) {
    for (const std::string_view listed :
         {"정상", "관리종목", "거래정지", "단기과열", "투자주의", "투자경고", "투자주의환기종목"}) {
      if (audit == listed) {
        return InstrumentStatus::ACTIVE;
      }
    }
    return InstrumentStatus::UNKNOWN;
  }

  // What a paged listing should do after one page.
  enum class PageStep {
    // Kiwoom offered no further page: the listing is complete.
    FINISH,
    // Kiwoom offered another page and this walk may fetch it.
    CONTINUE,
    // Kiwoom still offered another page and this walk stopped: the listing is incomplete.
    INCOMPLETE,
  };

  // One bar request spanning however many Kiwoom pages it needs.
  struct BarOperation {
    BarQuery query_;
    std::vector<Bar> descending_;
    // The upper bound for the next page, as a KST calendar date.
    Date date_{};
    std::string next_key_;
    // Whether Kiwoom said another page exists, which is not the same as having its key.
    bool more_{false};
    int pages_{0};
    BarsHandler completion_;

    BarOperation(BarQuery query, BarsHandler completion)
    : query_(std::move(query)), completion_(std::move(completion)) {}
  };

  // Validates a bar request and starts walking Kiwoom's pages.
  void start_bars(BarQuery query, BarsHandler completion) {
    if (auto invalid = validate_bar_query(query)) {
      deliver(std::move(completion), Result<BarSeries>(std::move(*invalid)));
      return;
    }
    auto operation = std::make_shared<BarOperation>(std::move(query), std::move(completion));
    const auto bound = operation->query_.before_ ?
      *operation->query_.before_ :
      std::chrono::system_clock::now();
    operation->date_ = seoul_date_of(bound);
    request_bars(operation);
  }

  // Refuses a bar request Kiwoom cannot answer, before any I/O.
  static std::optional<BrokerError> validate_bar_query(const BarQuery& query) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    if (auto invalid = validate_instrument(query.instrument_)) {
      return invalid;
    }
    if (query.count_ <= 0) {
      error.broker_code_ = "invalid_count";
      error.message_ = "a bar request must ask for at least one bar";
      return error;
    }
    return unsupported_bar_interval(query.interval_);
  }

  // Issues one page of a bar request.
  void request_bars(std::shared_ptr<BarOperation> operation) {
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), operation](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(
              std::move(operation->completion_), Result<BarSeries>(std::move(*token_error))
            );
            return;
          }
          json::object body;
          body["stk_cd"] = self->config_.tape_symbol(operation->query_.instrument_.symbol_);
          if (operation->query_.interval_ == BarInterval::ONE_MINUTE) {
            body["tic_scope"] = "1";
          }
          // `upd_stkpc_tp`: `1` asks for prices adjusted for corporate actions, `0` for raw ones.
          // `BarQuery::adjusted_` exists because the brokers disagree about how to spell it, and
          // Kiwoom's spelling is the opposite of KIS's `FID_ORG_ADJ_PRC`.
          body["upd_stkpc_tp"] = operation->query_.adjusted_ ? "1" : "0";
          body["base_dt"] = detail::compact_text(operation->date_);
          auto request = self->make_request(
            chart_api_id(operation->query_.interval_), "/api/dostk/chart",
            json::serialize(body), network::RateLimitGroup::MARKET_DATA, operation->next_key_
          );
          self->http_->async_request(
            std::move(request),
            [self, operation](Result<network::HttpResponse> result) mutable {
              net::post(self->strand_, [self, operation, result = std::move(result)]() mutable {
                self->handle_bars_page(operation, std::move(result));
              });
            }
          );
        }
      )
    );
  }

  // Accumulates one page of bars and either continues or finishes.
  void handle_bars_page(
    const std::shared_ptr<BarOperation>& operation,
    Result<network::HttpResponse> result
  ) {
    if (!result) {
      deliver(std::move(operation->completion_), Result<BarSeries>(std::move(result.error())));
      return;
    }
    const auto next_key = continuation_key(result.value());
    auto body = document(std::move(result));
    if (!body) {
      deliver(std::move(operation->completion_), Result<BarSeries>(std::move(body.error())));
      return;
    }
    const std::size_t before = operation->descending_.size();
    try {
      const auto rows = body.value().if_contains(chart_rows_field(operation->query_.interval_));
      if (!rows || !rows->is_array()) {
        throw std::runtime_error(
          std::string("missing array field: ") + chart_rows_field(operation->query_.interval_)
        );
      }
      for (const auto& value : rows->as_array()) {
        if (!value.is_object()) {
          throw std::runtime_error("chart row is not an object");
        }
        operation->descending_.push_back(
          parse_bar(value.as_object(), operation->query_.interval_)
        );
      }
    } catch (const std::exception& exception) {
      deliver(
        std::move(operation->completion_),
        Result<BarSeries>(protocol_error("invalid_bars_response", exception))
      );
      return;
    }

    switch (advance_bars(*operation, std::move(next_key), before)) {
      case PageStep::FINISH:
        finish_bars(operation);
        return;
      case PageStep::INCOMPLETE:
        deliver(
          std::move(operation->completion_),
          Result<BarSeries>(incomplete_listing("bars", operation->pages_))
        );
        return;
      case PageStep::CONTINUE:
        request_bars(operation);
        return;
    }
  }

  // Records the page's continuation key and returns whether the walk should stop.
  //
  // **The key is recorded before the decision is taken, not after it**, and that ordering is the
  // point of this being one function. `finish_bars` reads `next_key_` to decide whether the series
  // carries a continuation cursor, and the walk stops with data still available precisely when
  // `count_` is satisfied -- which is exactly when a caller needs that cursor to page further
  // back. Assigning the key only on the path that continues left the *previous* page's key here,
  // so a satisfied request reported a stale cursor or none.
  //
  // A page that returned nothing also ends the walk. Without that, a broker answering `Y` with an
  // empty array would spend the whole page budget learning nothing.
  static PageStep advance_bars(
    BarOperation& operation,
    std::optional<std::string> next_key,
    std::size_t before
  ) {
    operation.more_ = next_key.has_value();
    operation.next_key_ = next_key ? std::move(*next_key) : std::string();
    ++operation.pages_;
    const bool grew = operation.descending_.size() > before;
    const bool satisfied =
      static_cast<int>(operation.descending_.size()) >= operation.query_.count_;
    if (satisfied || !operation.more_) {
      return PageStep::FINISH;
    }
    // **Kiwoom offered more and the count is not satisfied.** Either this walk fetches the next
    // page or the series is short of what was asked for -- and a short series is not a finished
    // one. Returning it would be the listing defect in another place: a caller reads `bars_.size()`
    // and cannot tell "this is all there is" from "the walk gave up".
    //
    // A keyless page is the same thing arriving differently: Kiwoom offered a page and no way to
    // ask for it.
    if (operation.next_key_.empty() || !grew || operation.pages_ >= page_budget_) {
      return PageStep::INCOMPLETE;
    }
    return PageStep::CONTINUE;
  }

  // Normalizes one chart row.
  //
  // A minute row stamps `cntr_tm` as `yyyyMMddHHmmss`; every other interval stamps `dt` as
  // `yyyyMMdd`. `cur_prc` is the period's close and, like every market-data price, carries a
  // direction prefix rather than a sign.
  static Bar parse_bar(const json::object& row, BarInterval interval) {
    Bar bar;
    if (interval == BarInterval::ONE_MINUTE) {
      const auto stamp = detail::text_field(row, "cntr_tm");
      if (stamp.size() != 14) {
        throw std::runtime_error("invalid minute bar timestamp");
      }
      const auto date = detail::compact_date(std::string_view(stamp).substr(0, 8));
      const auto timestamp =
        date ? detail::seoul_timestamp(*date, std::string_view(stamp).substr(8)) : std::nullopt;
      if (!timestamp) {
        throw std::runtime_error("invalid minute bar timestamp");
      }
      bar.timestamp_ = *timestamp;
    } else {
      const auto date = detail::compact_date(detail::text_field(row, "dt"));
      const auto timestamp = date ? detail::seoul_timestamp(*date, "000000") : std::nullopt;
      if (!timestamp) {
        throw std::runtime_error("invalid bar date");
      }
      bar.timestamp_ = *timestamp;
    }
    const auto high = detail::magnitude(row, "high_pric");
    const auto low = detail::magnitude(row, "low_pric");
    const auto close = detail::magnitude(row, "cur_prc");
    if (!high || !low || !close) {
      throw std::runtime_error("bar row is missing a price");
    }
    bar.high_ = *high;
    bar.low_ = *low;
    bar.close_ = *close;
    bar.volume_ = detail::required_magnitude(row, "trde_qty");
    bar.open_ = detail::magnitude(row, "open_pric");
    bar.traded_value_ = detail::magnitude(row, "trde_prica");
    return bar;
  }

  // Reverses the accumulation, trims to the requested count, and returns the series.
  //
  // Pure, and separate from the completion for that reason: what a walk *produced* is the thing
  // worth checking, and checking it should not require a strand, a socket or a completion token.
  static BarSeries compose_bars(BarOperation& operation) {
    BarSeries series;
    series.instrument_ = operation.query_.instrument_;
    series.interval_ = operation.query_.interval_;
    series.currency_ = Currency::krw();
    series.received_at_ = std::chrono::system_clock::now();
    // What was asked for is what was sent (`upd_stkpc_tp`), and the series has to say which prices
    // it carries: the field defaults to `true`, so a raw request whose result never set it would
    // hand a caller unadjusted prices labelled as adjusted.
    series.adjusted_ = operation.query_.adjusted_;
    // The same convention the KIS adapter records: Kiwoom stamps a minute bar with the time the
    // period ended and a period bar with its trading date, which is the period's open. Neither
    // specification states it in words, and an off-by-one-bar lookahead is the defect `BarStamp`
    // exists to make visible, so the two adapters agree on the reading rather than differing
    // silently.
    series.stamp_ = operation.query_.interval_ == BarInterval::ONE_MINUTE ?
      BarStamp::CLOSE :
      BarStamp::OPEN;

    auto& rows = operation.descending_;
    if (static_cast<int>(rows.size()) > operation.query_.count_) {
      rows.resize(static_cast<std::size_t>(operation.query_.count_));
    }
    series.bars_.assign(rows.rbegin(), rows.rend());
    // The oldest bar in hand, which is what a caller passes back as `before_` to page further
    // back. Emitted only when Kiwoom said there is more *and* a caller could act on it: an
    // exhausted history reports no cursor rather than one that would return nothing, and a minute
    // series reports none because `base_dt` is a date and cannot resume inside a day. A minute
    // walk that cannot satisfy its count fails instead; see `advance_bars`.
    const bool resumable = operation.query_.interval_ != BarInterval::ONE_MINUTE;
    if (!series.bars_.empty() && operation.more_ && resumable) {
      series.next_cursor_ = format_iso8601_utc(series.bars_.front().timestamp_);
    }
    return series;
  }

  // Composes the series and delivers it.
  void finish_bars(const std::shared_ptr<BarOperation>& operation) {
    deliver(std::move(operation->completion_), Result<BarSeries>(compose_bars(*operation)));
  }

  // Returns the KST calendar date an instant falls on.
  static Date seoul_date_of(std::chrono::system_clock::time_point instant) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(
      std::chrono::time_point_cast<std::chrono::seconds>(instant + std::chrono::hours(9))
    );
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &raw);
#else
    gmtime_r(&raw, &parts);
#endif
    Date date;
    date.year_ = parts.tm_year + 1900;
    date.month_ = parts.tm_mon + 1;
    date.day_ = parts.tm_mday;
    return date;
  }

  // Records the page's continuation key and says what the walk should do next.
  //
  // One function for the three account listings, because the decision is the same for all of them
  // and the consequence of getting it wrong is the same too. It is separate from the handlers so
  // that the decision can be exercised without a socket.
  static PageStep advance_page(
    std::string& next_key_slot,
    int& pages,
    bool grew,
    std::optional<std::string> next_key
  ) {
    ++pages;
    if (!next_key) {
      return PageStep::FINISH;
    }
    // Kiwoom offered another page. Either this walk fetches it or the listing is incomplete --
    // there is no third answer, including when the key it offered is unusable.
    if (next_key->empty() || !grew || pages >= page_budget_) {
      return PageStep::INCOMPLETE;
    }
    next_key_slot = std::move(*next_key);
    return PageStep::CONTINUE;
  }

  // Returns the failure a listing that stopped while more remained should end with.
  //
  // **A guard that bounds a walk must not also decide the walk finished.** Kiwoom says `cont-yn: Y`
  // with a `next-key` exactly when there is another page, so stopping on the page budget -- or on a
  // page that added no rows while Kiwoom still offered another -- leaves rows unread. Returning
  // what was gathered would hand a *partial* answer to code that cannot tell: a working-order
  // listing missing a resting sell understates what `RiskGuard` deducts, and partial holdings
  // understate exposure, which is the permissive direction for a position ceiling.
  //
  // Bars are the exception and do not come through here: `BarSeries` carries `next_cursor_`, so a
  // bar walk can say "there is more" in the value itself. Holdings, working orders and order
  // history have nowhere to say it, so they fail instead.
  static BrokerError incomplete_listing(std::string_view what, int pages) {
    BrokerError error;
    error.category_ = ErrorCategory::PROTOCOL;
    error.broker_code_ = "incomplete_listing";
    error.reached_broker_ = true;
    error.message_ = std::string("Kiwoom still offered another page of ") + std::string(what) +
      " after " + std::to_string(pages) +
      " page(s); the listing is incomplete and is refused rather than returned in part";
    return error;
  }

  // One holdings request spanning however many `kt00018` pages it needs.
  struct HoldingsOperation {
    HoldingsSnapshot snapshot_;
    std::string next_key_;
    int pages_{0};
    bool totals_seen_{false};
    HoldingsHandler completion_;

    explicit HoldingsOperation(HoldingsHandler completion) : completion_(std::move(completion)) {}
  };

  // Validates the configured holdings venue and starts paging.
  void start_holdings(HoldingsHandler completion) {
    if (auto invalid = validate_holdings_venue()) {
      deliver(std::move(completion), Result<HoldingsSnapshot>(std::move(*invalid)));
      return;
    }
    auto operation = std::make_shared<HoldingsOperation>(std::move(completion));
    request_holdings(operation);
  }

  // Refuses a holdings venue `kt00018` cannot express.
  //
  // `kt00018`'s `dmst_stex_tp` documents `KRX` and `NXT` and no all-venue value, so there is
  // nothing for `COMBINED` or `UNSPECIFIED` to be sent as. Resolving either to one of the two
  // would answer a different question from the one asked and would do it silently, which this
  // library refuses to do — and holdings are what the sellable guard and every position ceiling are
  // measured against. See `KiwoomConfig::holdings_venue_`.
  // Refuses an instruction that can open or enlarge a position while the holdings scope is unstated.
  //
  // Nothing once the operator has asserted what `dmst_stex_tp` means: the assertion is what this
  // refusal exists to demand, so honouring it is the whole point rather than a weakening. It does
  // **not** promise a holdings read has succeeded -- see `KiwoomConfig::holdings_position_scope_`
  // for what an assertion does and does not claim, and note that the runner's own portfolio gate is
  // a separate rule that still applies.
  // Returns what `trde_able_qty` on a `kt00018` row is evidence of, given what the operator asserted.
  //
  // **A named decision with one caller**, rather than three lines inside the paging branch that
  // completes a snapshot. That branch is unreachable without a live HTTP round trip, so a rule
  // written there is a rule no test can drive. A decision worth making is worth naming
  // (`validate_amendment`), and this is the same shape.
  //
  // The coverage is the operator's assertion and nothing more. The resting treatment is this
  // library's own conservative rule either way and is deliberately not a caller's to state:
  // deducting a resting sell Kiwoom had also deducted refuses a trade, while not deducting one
  // sells the same shares twice, and only the first error is recoverable. `verified_` stays false
  // because an assertion is not a measurement.
  static SellableBasis holdings_sellable_basis(KiwoomSellableScope scope) {
    return SellableBasis{
      scope == KiwoomSellableScope::WHOLE_HOLDING ? SellableBasis::Coverage::WHOLE_HOLDING
                                                 : SellableBasis::Coverage::UNESTABLISHED,
      SellableBasis::RestingTreatment::DEDUCT,
      false,
    };
  }

  static std::optional<BrokerError> holdings_scope_refusal(KiwoomPositionScope scope) {
    if (!kiwoom_position_scope_asserted(scope)) {
      return unestablished_holdings_scope();
    }
    return std::nullopt;
  }

  std::optional<BrokerError> validate_holdings_venue() const {
    if (config_.holdings_venue_ == Venue::KRX || config_.holdings_venue_ == Venue::NXT) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "holdings_venue_required";
    error.message_ =
      "Kiwoom's balance inquiry names one venue and publishes no combined value, and whether that "
      "selects which positions are listed or only how they are valued is not stated; set "
      "KiwoomConfig::holdings_venue_ to Venue::KRX or Venue::NXT deliberately, having read its "
      "documentation";
    return error;
  }

  // Issues one page of the holdings inquiry.
  void request_holdings(std::shared_ptr<HoldingsOperation> operation) {
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), operation](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(
              std::move(operation->completion_), Result<HoldingsSnapshot>(std::move(*token_error))
            );
            return;
          }
          auto request = self->make_request(
            "kt00018", "/api/dostk/acnt", self->holdings_body(),
            network::RateLimitGroup::ACCOUNT, operation->next_key_
          );
          self->http_->async_request(
            std::move(request),
            [self, operation](Result<network::HttpResponse> result) mutable {
              net::post(self->strand_, [self, operation, result = std::move(result)]() mutable {
                self->handle_holdings_page(operation, std::move(result));
              });
            }
          );
        }
      )
    );
  }

  // Builds `kt00018`'s body: one summed listing, from the configured venue.
  std::string holdings_body() const {
    json::object body;
    // `1:합산` asks Kiwoom to sum the lots of one instrument into one row; `2:개별` reports them
    // separately. Summed is what this library asks for -- but the specification's own example
    // answers a `qry_tp: "1"` request with *two* `A005930` rows, so asking is not the same as
    // getting, and `append_holdings_page` refuses a listing that comes back split rather than
    // trusting the request. See `duplicate_symbol` there for why that matters.
    body["qry_tp"] = "1";
    body["dmst_stex_tp"] = exchange_code(config_.holdings_venue_);
    return json::serialize(body);
  }

  // Accumulates one page of holdings and either continues or completes.
  void handle_holdings_page(
    const std::shared_ptr<HoldingsOperation>& operation,
    Result<network::HttpResponse> result
  ) {
    if (!result) {
      deliver(
        std::move(operation->completion_), Result<HoldingsSnapshot>(std::move(result.error()))
      );
      return;
    }
    const auto next_key = continuation_key(result.value());
    auto body = document(std::move(result));
    if (!body) {
      deliver(
        std::move(operation->completion_), Result<HoldingsSnapshot>(std::move(body.error()))
      );
      return;
    }
    const std::size_t before = operation->snapshot_.positions_.size();
    try {
      append_holdings_page(operation->snapshot_, operation->totals_seen_, body.value());
    } catch (const std::exception& exception) {
      deliver(
        std::move(operation->completion_),
        Result<HoldingsSnapshot>(protocol_error("invalid_holdings_response", exception))
      );
      return;
    }
    const bool grew = operation->snapshot_.positions_.size() > before;
    switch (advance_page(operation->next_key_, operation->pages_, grew, std::move(next_key))) {
      case PageStep::FINISH:
        operation->snapshot_.received_at_ = std::chrono::system_clock::now();
        // **What the `trde_able_qty` figures on these rows are evidence of, per the operator's own
        // assertion and nothing more.** `WHOLE_HOLDING` only when somebody has stated that the
        // venue parameter does not restrict the figure; unestablished otherwise, which makes
        // `RiskGuard` refuse a checked sell rather than measure one against a number that may
        // describe one book out of two. The *resting* treatment is this library's conservative rule
        // either way and is not a caller's to assert: deducting a resting sell Kiwoom had also
        // deducted refuses a trade, where the opposite sells the same shares twice.
        operation->snapshot_.sellable_basis_ =
          holdings_sellable_basis(config_.holdings_sellable_scope_);
        deliver(
          std::move(operation->completion_),
          Result<HoldingsSnapshot>(std::move(operation->snapshot_))
        );
        return;
      case PageStep::INCOMPLETE:
        deliver(
          std::move(operation->completion_),
          Result<HoldingsSnapshot>(incomplete_listing("holdings", operation->pages_))
        );
        return;
      case PageStep::CONTINUE:
        request_holdings(operation);
        return;
    }
  }

  // Appends one `kt00018` page's rows and, on the first page only, its totals.
  //
  // **A symbol that appears twice fails the snapshot.** The request asks for a summed listing
  // (`qry_tp: "1"`), and the specification's own response example answers exactly that request
  // with two `A005930` rows carrying 3 and 8 shares -- so a split listing is a real possibility
  // and not a hypothetical. It is refused rather than accepted for a reason that costs money:
  // `Portfolio` reads the *first* matching row's `sellable_quantity_`, so a split holding would
  // report 3 sellable shares where Kiwoom said 11, and which 3 would depend on row order.
  // Summing them instead is not this library's call to make -- whether two lots' *sellable*
  // quantities add is a statement about what the broker meant, which the specification does not
  // settle. So the snapshot fails loudly, once, with the symbol named.
  //
  // **The totals come from Kiwoom's summary fields and are never summed from the rows**, which is
  // what `HoldingsTotals` documents: a paged listing can return part of the positions beside an
  // account-wide summary, so the two need not agree and the broker's own figure is the one to
  // report. Only the first page's summary is taken, because every page repeats it.
  static void append_holdings_page(
    HoldingsSnapshot& snapshot,
    bool& totals_seen,
    const json::object& page
  ) {
    if (!totals_seen) {
      HoldingsTotals totals;
      totals.currency_ = Currency::krw();
      totals.purchase_value_ = detail::magnitude_or_zero(page, "tot_pur_amt");
      totals.market_value_ = detail::magnitude_or_zero(page, "tot_evlt_amt");
      totals.unrealized_profit_loss_ = detail::signed_or_zero(page, "tot_evlt_pl");
      snapshot.totals_.push_back(totals);
      totals_seen = true;
    }
    const auto rows = page.if_contains("acnt_evlt_remn_indv_tot");
    if (!rows) {
      return;
    }
    if (!rows->is_array()) {
      throw std::runtime_error("acnt_evlt_remn_indv_tot is not an array");
    }
    for (const auto& value : rows->as_array()) {
      if (!value.is_object()) {
        throw std::runtime_error("holdings row is not an object");
      }
      auto position = parse_holding(value.as_object());
      if (duplicate_symbol(snapshot, position.instrument_.symbol_)) {
        throw std::runtime_error(
          "holdings listing reports " + position.instrument_.symbol_ +
          " on more than one row although it was requested summed (qry_tp=1)"
        );
      }
      snapshot.positions_.push_back(std::move(position));
    }
  }

  // Returns whether `symbol` already has a row in `snapshot`.
  static bool duplicate_symbol(const HoldingsSnapshot& snapshot, const std::string& symbol) {
    for (const auto& position : snapshot.positions_) {
      if (position.instrument_.symbol_ == symbol) {
        return true;
      }
    }
    return false;
  }

  // Normalizes one `kt00018` position row.
  static HoldingPosition parse_holding(const json::object& row) {
    HoldingPosition position;
    position.instrument_.country_ = Country::KR;
    // A holding is reported without a venue, so it carries the identity venue -- the combined one
    // -- and not whichever book `dmst_stex_tp` named. An identity that carried the request's venue
    // would stop matching the same instrument's working orders and quotes.
    position.instrument_.venue_ = Venue::COMBINED;
    position.instrument_.symbol_ = detail::bare_symbol(detail::text_field(row, "stk_cd"));
    position.name_ = detail::text_field(row, "stk_nm");
    position.quantity_ = detail::required_magnitude(row, "rmnd_qty");
    position.sellable_quantity_ = detail::magnitude(row, "trde_able_qty");
    position.average_purchase_price_ = detail::magnitude_or_zero(row, "pur_pric");
    position.last_price_ = detail::magnitude_or_zero(row, "cur_prc");
    position.purchase_value_ = Money{detail::magnitude_or_zero(row, "pur_amt"), Currency::krw()};
    position.market_value_ = Money{detail::magnitude_or_zero(row, "evlt_amt"), Currency::krw()};
    position.unrealized_profit_loss_ =
      Money{detail::signed_or_zero(row, "evltv_prft"), Currency::krw()};
    // `prft_rt` is a percentage with a sign; the normalized field is a fraction.
    if (const auto percent = detail::signed_value(row, "prft_rt")) {
      position.unrealized_return_ = broker_detail::percent_to_ratio(
        detail::text_field(row, "prft_rt")
      );
    }
    return position;
  }

  // Issues the deposit inquiry and reduces it to what the port asked for.
  void start_buying_power(Currency currency, BuyingPowerHandler completion) {
    if (!(currency == Currency::krw())) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "unsupported_currency";
      error.message_ = "Kiwoom's domestic deposit inquiry answers in won";
      deliver(std::move(completion), Result<BuyingPower>(std::move(error)));
      return;
    }
    start_deposit(
      DepositHandler(
        [self = shared_from_this(), completion = std::move(completion)](
          Result<KiwoomDeposit> result
        ) mutable {
          if (!result) {
            self->deliver(std::move(completion), Result<BuyingPower>(std::move(result.error())));
            return;
          }
          BuyingPower power;
          power.cash_ = Money{result.value().spendable_, result.value().currency_};
          power.received_at_ = result.value().received_at_;
          self->deliver(std::move(completion), Result<BuyingPower>(std::move(power)));
        }
      )
    );
  }

  // Issues `kt00001` 예수금상세현황요청.
  void start_deposit(DepositHandler completion) {
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), completion = std::move(completion)](
          std::optional<BrokerError> token_error
        ) mutable {
          if (token_error) {
            self->deliver(std::move(completion), Result<KiwoomDeposit>(std::move(*token_error)));
            return;
          }
          json::object body;
          // `3:추정조회` includes today's unsettled activity; `2:일반조회` does not. The estimate is
          // what a figure named "what may be spent" has to mean intraday.
          body["qry_tp"] = "3";
          auto request = self->make_request(
            "kt00001", "/api/dostk/acnt", json::serialize(body),
            network::RateLimitGroup::ACCOUNT
          );
          self->http_->async_request(
            std::move(request),
            [self, completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(std::move(completion), parse_deposit(std::move(result)));
            }
          );
        }
      )
    );
  }

  // Normalizes `kt00001` into Kiwoom's own deposit detail.
  static Result<KiwoomDeposit> parse_deposit(Result<network::HttpResponse> result) {
    auto body = document(std::move(result));
    if (!body) {
      return Result<KiwoomDeposit>(std::move(body.error()));
    }
    try {
      const auto& row = body.value();
      KiwoomDeposit deposit;
      deposit.currency_ = Currency::krw();
      deposit.received_at_ = std::chrono::system_clock::now();
      const auto spendable = detail::signed_value(row, "100stk_ord_alow_amt");
      if (!spendable) {
        throw std::runtime_error("missing or unreadable field: 100stk_ord_alow_amt");
      }
      deposit.spendable_ = *spendable;
      deposit.orderable_ = detail::signed_or_zero(row, "ord_alow_amt");
      deposit.deposit_ = detail::signed_or_zero(row, "entr");
      deposit.settled_deposit_ = detail::signed_or_zero(row, "d2_entra");
      deposit.substitute_amount_ = detail::signed_or_zero(row, "repl_amt");
      deposit.withdrawable_ = detail::signed_or_zero(row, "pymn_alow_amt");
      return Result<KiwoomDeposit>(std::move(deposit));
    } catch (const std::exception& exception) {
      return Result<KiwoomDeposit>(protocol_error("invalid_deposit_response", exception));
    }
  }

  // Issues `kt00010` 주문인출가능금액요청 for one instrument.
  void start_orderable_amount(
    InstrumentId instrument,
    OrderSide side,
    std::optional<Decimal64> price,
    OrderableHandler completion
  ) {
    if (auto invalid = validate_instrument(instrument)) {
      deliver(std::move(completion), Result<KiwoomOrderableAmount>(std::move(*invalid)));
      return;
    }
    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(), instrument = std::move(instrument), side,
          price = std::move(price), completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(
              std::move(completion), Result<KiwoomOrderableAmount>(std::move(*token_error))
            );
            return;
          }
          json::object body;
          body["stk_cd"] = instrument.symbol_;
          body["trde_tp"] = side == OrderSide::SELL ? "1" : "2";
          body["uv"] = price ? price->str() : "";
          auto request = self->make_request(
            "kt00010", "/api/dostk/acnt", json::serialize(body),
            network::RateLimitGroup::ACCOUNT
          );
          self->http_->async_request(
            std::move(request),
            [self, completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(std::move(completion), parse_orderable_amount(std::move(result)));
            }
          );
        }
      )
    );
  }

  // Normalizes `kt00010` into the per-margin-tier amounts and quantities.
  static Result<KiwoomOrderableAmount> parse_orderable_amount(
    Result<network::HttpResponse> result
  ) {
    auto body = document(std::move(result));
    if (!body) {
      return Result<KiwoomOrderableAmount>(std::move(body.error()));
    }
    try {
      const auto& row = body.value();
      KiwoomOrderableAmount orderable;
      orderable.currency_ = Currency::krw();
      orderable.received_at_ = std::chrono::system_clock::now();
      // The amounts stay signed: a deposit figure can genuinely be negative when the account is
      // in 미수, and none of them is a price, so no direction prefix applies to them. The
      // quantities cannot be negative and are read as magnitudes.
      orderable.orderable_cash_ = detail::signed_or_zero(row, "ord_alowa");
      orderable.withdrawable_ = detail::signed_or_zero(row, "wthd_alowa");
      orderable.settled_deposit_ = detail::signed_or_zero(row, "d2entra");
      orderable.margin_reduction_applies_ =
        detail::text_field(row, "profa_rdex_aplc_tp") == "1";
      for (const int percent : {20, 30, 40, 50, 60, 100}) {
        KiwoomMarginTier tier;
        tier.margin_percent_ = percent;
        const std::string prefix = "profa_" + std::to_string(percent) + "ord_alow";
        tier.amount_ = detail::signed_value(row, prefix + "_amt");
        tier.quantity_ = detail::magnitude(row, prefix + "q");
        orderable.tiers_.push_back(tier);
      }
      KiwoomMarginTier reduced;
      reduced.margin_percent_ = 60;
      reduced.reduced_ = true;
      reduced.amount_ = detail::signed_value(row, "profa_rdex_60ord_alow_amt");
      reduced.quantity_ = detail::magnitude(row, "profa_rdex_60ord_alowq");
      orderable.tiers_.push_back(reduced);
      return Result<KiwoomOrderableAmount>(std::move(orderable));
    } catch (const std::exception& exception) {
      return Result<KiwoomOrderableAmount>(
        protocol_error("invalid_orderable_amount_response", exception)
      );
    }
  }

  // Issues `ka00001` 계좌번호조회, which answers with the one account the credential drives.
  void start_accounts(AccountsHandler completion) {
    using Accounts = std::vector<KiwoomAccountInfo>;
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), completion = std::move(completion)](
          std::optional<BrokerError> token_error
        ) mutable {
          if (token_error) {
            self->deliver(std::move(completion), Result<Accounts>(std::move(*token_error)));
            return;
          }
          auto request = self->make_request(
            "ka00001", "/api/dostk/acnt", "{}", network::RateLimitGroup::ACCOUNT
          );
          self->http_->async_request(
            std::move(request),
            [self, completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(std::move(completion), parse_accounts(std::move(result)));
            }
          );
        }
      )
    );
  }

  // Normalizes `ka00001` into a listing of one.
  //
  // A listing rather than a single value, because that is the port's shape — and the shape is
  // honest here: Kiwoom answers with the account the credential drives and there is no other to
  // choose between.
  static Result<std::vector<KiwoomAccountInfo>> parse_accounts(
    Result<network::HttpResponse> result
  ) {
    using Accounts = std::vector<KiwoomAccountInfo>;
    auto body = document(std::move(result));
    if (!body) {
      return Result<Accounts>(std::move(body.error()));
    }
    try {
      const auto number = detail::text_field(body.value(), "acctNo");
      if (number.empty()) {
        throw std::runtime_error("missing string field: acctNo");
      }
      Accounts accounts;
      accounts.push_back(KiwoomAccountInfo{number});
      return Result<Accounts>(std::move(accounts));
    } catch (const std::exception& exception) {
      return Result<Accounts>(protocol_error("invalid_accounts_response", exception));
    }
  }

  // Returns an error when a quantity is not a whole number of shares.
  //
  // `allow_zero` is Kiwoom's all-remaining value on an amendment, which it documents as the
  // quantity `0`. A *placement* of zero shares is still refused.
  static std::optional<BrokerError> validate_quantity(
    const Decimal64& quantity,
    bool allow_zero = false
  ) {
    const bool positive = quantity.coefficient() > 0;
    if ((positive || (allow_zero && quantity.coefficient() == 0)) && quantity.scale() == 0) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "invalid_quantity";
    error.message_ = allow_zero ?
      "a Kiwoom amendment quantity must be a whole number of shares, or zero for all remaining" :
      "a Kiwoom order quantity must be a positive whole number of shares";
    return error;
  }

  // Returns an error when a price does not match its price basis.
  //
  // `carries_caller_price`, not `== LIMIT`: 조건부지정가 is a limit instruction until the close, and
  // treating it as unpriced is how KIS's adapter once transmitted `ORD_UNPR="0"` — an order at zero
  // rather than at the stated limit.
  static std::optional<BrokerError> validate_price(
    OrderType type,
    const std::optional<Decimal64>& price
  ) {
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "invalid_price";
    if (carries_caller_price(type)) {
      if (!price) {
        error.message_ = "a Kiwoom order of this price basis requires a price";
        return error;
      }
      if (price->coefficient() <= 0) {
        error.message_ = "a Kiwoom order price must be positive";
        return error;
      }
      if (price->scale() != 0) {
        error.message_ = "a Kiwoom domestic order price must be a whole number of won";
        return error;
      }
      return std::nullopt;
    }
    if (price) {
      error.message_ =
        "Kiwoom takes no price for this basis; a market, best-limit, primary-limit, mid-price or "
        "closing-price order is priced by the book";
      return error;
    }
    return std::nullopt;
  }

  // Returns an error when a Kiwoom order names no venue.
  //
  // **Kiwoom documents `dmst_stex_tp` with no default**, so unlike KIS there is no "omitted means
  // KRX" rule this adapter could record and journal. An order that names no venue is refused before
  // any I/O rather than sent to a venue this library chose, because recovery has to be able to say
  // where an order went and a guessed venue makes that a fabrication.
  static std::optional<BrokerError> validate_venue(Venue venue) {
    if (venue == Venue::KRX || venue == Venue::NXT || venue == Venue::COMBINED) {
      return std::nullopt;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    error.broker_code_ = "venue_required";
    error.message_ =
      "Kiwoom requires dmst_stex_tp on every order and documents no default; set "
      "OrderRequest::venue_ to KRX, NXT or COMBINED (Kiwoom's SOR), or, for a strategy runner, "
      "bind it with a default route (live::bind / bind_journaled, SessionExecutor)";
    return error;
  }

  // The identity venue this adapter reports every Korean instrument at.
  static constexpr Venue instrument_venue() { return Venue::COMBINED; }

  // Returns an error when an order cannot be represented as a Kiwoom instruction.
  static std::optional<BrokerError> validate_order(
    KiwoomPositionScope scope,
    const OrderRequest& order,
    std::string& division
  ) {
    if (auto error = unsupported_client_order_id(order)) {
      return error;
    }
    if (auto error = validate_instrument(order.instrument_)) {
      return error;
    }
    // Kiwoom reports every Korean instrument at one identity venue, and a holding carries no venue
    // at all -- so an order whose `instrument_.venue_` said anything else would not be the same
    // `InstrumentId` as its own holding, and `Portfolio::working_quantity` would read zero resting
    // quantity for it. A caller who set it there means *routing*, which is `OrderRequest::venue_`.
    if (order.instrument_.venue_ != instrument_venue()) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_instrument";
      error.message_ =
        "this adapter identifies a Korean instrument by its combined venue; set "
        "OrderRequest::venue_ to route the order to a particular exchange instead of naming one on "
        "the instrument";
      return error;
    }
    if (auto error = validate_quantity(order.quantity_)) {
      return error;
    }
    if (auto error = validate_price(order.type_, order.price_)) {
      return error;
    }
    if (auto error = invalid_trigger(order.type_, order.trigger_price_)) {
      return error;
    }
    if (auto error = validate_venue(order.venue_)) {
      return error;
    }
    if (auto error = order_division(order.type_, order.time_in_force_, order.session_, division)) {
      return error;
    }
    // **Last**, so a caller hears about their own order's problems before this one. An order that
    // is malformed is malformed whatever this library knows about the account, and reporting the
    // holdings gap first would bury the mistake the caller can actually fix.
    return holdings_scope_refusal(scope);
  }

  // Returns an error when an amendment cannot be represented as a Kiwoom instruction.
  //
  // Its own function for the reason the KIS adapter gives: the decision a test needs to exercise is
  // the one the submission path takes, not one a test re-derives from the same inputs.
  static std::optional<BrokerError> validate_amendment(
    KiwoomPositionScope scope,
    const KiwoomAmendment& amendment,
    bool cancel
  ) {
    if (auto error = validate_quantity(amendment.quantity_, true)) {
      return error;
    }
    if (auto error = validate_venue(amendment.venue_)) {
      return error;
    }
    BrokerError error;
    error.category_ = ErrorCategory::INVALID_ARGUMENT;
    error.reached_broker_ = false;
    if (amendment.order_.order_number_.empty()) {
      error.broker_code_ = "invalid_order_reference";
      error.message_ = "a Kiwoom amendment requires the original order number";
      return error;
    }
    if (amendment.order_.symbol_.empty()) {
      // kt10002 and kt10003 both require `stk_cd` beside `orig_ord_no`. A reference that lost the
      // symbol cannot address an order at all, which is why `KiwoomOrderRef` carries both.
      error.broker_code_ = "invalid_order_reference";
      error.message_ =
        "a Kiwoom amendment requires the order's symbol beside its number; both are part of the "
        "reference this adapter issues";
      return error;
    }
    if (cancel && amendment.price_) {
      error.broker_code_ = "invalid_price";
      error.message_ = "Kiwoom's cancellation carries a quantity and no price";
      return error;
    }
    if (cancel && amendment.condition_price_) {
      // `kt10003` has no trigger field, so a stated trigger cannot be sent -- and acknowledging
      // the instruction after dropping it would report success for a change that did not happen,
      // which is the same failure refusing a price on a cancellation exists to stop. The
      // normalized helpers never set this, so no port path is affected.
      error.broker_code_ = "invalid_condition_price";
      error.message_ =
        "Kiwoom's cancellation carries a quantity and no trigger price, so it cannot change an "
        "order's stop condition on the way out; cancel the order and place a new one instead";
      return error;
    }
    if (!cancel && amendment.price_ && amendment.price_->scale() != 0) {
      error.broker_code_ = "invalid_price";
      error.message_ = "a Kiwoom domestic order price must be a whole number of won";
      return error;
    }
    // Only the value: whether the order's basis wants a trigger at all is decided where the basis
    // is known, `BrokerTraits<KiwoomClient>::to_amendment`, because `kt10002` carries none.
    if (!cancel && amendment.condition_price_ && !valid_trigger_value(*amendment.condition_price_)) {
      error.broker_code_ = "invalid_trigger_price";
      error.message_ = "a Kiwoom trigger price must be a positive whole number of won";
      return error;
    }
    if (!cancel) {
      // Last, and cancellations excepted; see `unestablished_holdings_scope`. A replacement can
      // move a resting buy to a fillable price, which opens or enlarges the position this library
      // cannot measure. A cancellation only ever reduces what is resting.
      return holdings_scope_refusal(scope);
    }
    return std::nullopt;
  }

  // Builds `kt10000`/`kt10001`'s body. The side selects the endpoint, not a field.
  static network::HttpRequest place_order_request(
    const KiwoomClient& client,
    const OrderRequest& order,
    std::string_view division
  ) {
    json::object body;
    body["dmst_stex_tp"] = exchange_code(order.venue_);
    body["stk_cd"] = order.instrument_.symbol_;
    body["ord_qty"] = order.quantity_.str();
    // An unpriced basis is sent with an empty `ord_uv` rather than a zero: `carries_caller_price`
    // decides which, and a zero would be a price the caller did not state.
    body["ord_uv"] = order.price_ ? order.price_->str() : "";
    body["trde_tp"] = std::string(division);
    // `cond_uv` is the stop trigger. `invalid_trigger` has restricted it to `28`, so every other
    // body carries the `""` it always has.
    body["cond_uv"] = order.trigger_price_ ? order.trigger_price_->str() : "";
    return client.make_request(
      order.side_ == OrderSide::BUY ? "kt10000" : "kt10001",
      "/api/dostk/ordr",
      json::serialize(body),
      network::RateLimitGroup::TRADING
    );
  }

  // Builds `kt10002`'s or `kt10003`'s body.
  static network::HttpRequest amend_order_request(
    const KiwoomClient& client,
    const KiwoomAmendment& amendment,
    bool cancel
  ) {
    json::object body;
    body["dmst_stex_tp"] = exchange_code(amendment.venue_);
    body["orig_ord_no"] = amendment.order_.order_number_;
    body["stk_cd"] = amendment.order_.symbol_;
    if (cancel) {
      body["cncl_qty"] = amendment.quantity_.str();
    } else {
      body["mdfy_qty"] = amendment.quantity_.str();
      body["mdfy_uv"] = amendment.price_ ? amendment.price_->str() : "";
      body["mdfy_cond_uv"] =
        amendment.condition_price_ ? amendment.condition_price_->str() : "";
    }
    return client.make_request(
      cancel ? "kt10003" : "kt10002",
      "/api/dostk/ordr",
      json::serialize(body),
      network::RateLimitGroup::TRADING
    );
  }

  // Validates and submits a placement.
  void start_place_order(OrderRequest order, OrderAckHandler completion) {
    std::string division;
    if (auto invalid = validate_order(config_.holdings_position_scope_, order, division)) {
      deliver(std::move(completion), Result<KiwoomOrderAck>(std::move(*invalid)));
      return;
    }
    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(), order = std::move(order), division = std::move(division),
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(
              std::move(completion), Result<KiwoomOrderAck>(std::move(*token_error))
            );
            return;
          }
          auto request = place_order_request(*self, order, division);
          const std::string symbol = order.instrument_.symbol_;
          self->http_->async_request(
            std::move(request),
            [self, symbol, demo = self->config_.is_demo_, completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(
                std::move(completion),
                parse_order_ack(symbol, std::move(result), KiwoomOrderOperation::PLACE, demo)
              );
            }
          );
        }
      )
    );
  }

  // Validates and submits a modification or a cancellation.
  void start_amend_order(KiwoomAmendment amendment, bool cancel, OrderAckHandler completion) {
    if (auto invalid = validate_amendment(config_.holdings_position_scope_, amendment, cancel)) {
      deliver(std::move(completion), Result<KiwoomOrderAck>(std::move(*invalid)));
      return;
    }
    ensure_token(
      TokenHandler(
        [
          self = shared_from_this(), amendment = std::move(amendment), cancel,
          completion = std::move(completion)
        ](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(
              std::move(completion), Result<KiwoomOrderAck>(std::move(*token_error))
            );
            return;
          }
          auto request = amend_order_request(*self, amendment, cancel);
          const std::string symbol = amendment.order_.symbol_;
          const auto operation = cancel ? KiwoomOrderOperation::CANCEL : KiwoomOrderOperation::AMEND;
          self->http_->async_request(
            std::move(request),
            [self, symbol, operation, demo = self->config_.is_demo_, completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(
                std::move(completion), parse_order_ack(symbol, std::move(result), operation, demo)
              );
            }
          );
        }
      )
    );
  }

  // Normalizes an order response into an acknowledgement and a reference.
  //
  // The symbol comes from the request rather than the response, because Kiwoom's order responses
  // carry `ord_no` and the venue and no `stk_cd` at all — and the reference needs the symbol to be
  // usable for a later amendment. `operation` and `demo` describe the request this reply answers;
  // left out, no refusal is recognized.
  static Result<KiwoomOrderAck> parse_order_ack(
    const std::string& symbol,
    Result<network::HttpResponse> result,
    KiwoomOrderOperation operation = KiwoomOrderOperation::UNCLASSIFIED,
    bool demo = false
  ) {
    auto body = document(std::move(result), operation, demo);
    if (!body) {
      return Result<KiwoomOrderAck>(std::move(body.error()));
    }
    try {
      const auto& row = body.value();
      const auto number = detail::text_field(row, "ord_no");
      if (number.empty()) {
        throw std::runtime_error("missing string field: ord_no");
      }
      KiwoomOrderAck ack;
      ack.ack_.order_id_ = number;
      ack.ack_.accepted_at_ = std::chrono::system_clock::now();
      ack.order_ = KiwoomOrderRef{number, symbol};
      ack.base_order_number_ = detail::text_field(row, "base_orig_ord_no");
      if (auto quantity = detail::magnitude(row, "mdfy_qty")) {
        ack.amended_quantity_ = *quantity;
      } else if (auto canceled = detail::magnitude(row, "cncl_qty")) {
        ack.amended_quantity_ = *canceled;
      }
      return Result<KiwoomOrderAck>(std::move(ack));
    } catch (const std::exception& exception) {
      return Result<KiwoomOrderAck>(protocol_error("invalid_order_response", exception));
    }
  }

  // One amendable-order listing spanning however many `ka10075` pages it needs.
  struct OpenOrderOperation {
    KiwoomOpenOrderPage page_;
    std::string next_key_;
    int pages_{0};
    OpenOrderHandler completion_;

    explicit OpenOrderOperation(OpenOrderHandler completion) : completion_(std::move(completion)) {}
  };

  // Starts the amendable-order listing.
  void start_open_orders(OpenOrderHandler completion) {
    auto operation = std::make_shared<OpenOrderOperation>(std::move(completion));
    request_open_orders(operation);
  }

  // Issues one page of `ka10075` 미체결요청.
  void request_open_orders(std::shared_ptr<OpenOrderOperation> operation) {
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), operation](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(
              std::move(operation->completion_),
              Result<KiwoomOpenOrderPage>(std::move(*token_error))
            );
            return;
          }
          json::object body;
          body["all_stk_tp"] = "0";
          body["trde_tp"] = "0";
          body["stk_cd"] = "";
          // `0 : 통합` -- unlike the balance inquiry, the unfilled listing *does* publish an
          // all-venue value, so a working order resting at either venue is listed. Nothing is
          // scoped here and nothing needs to be.
          body["stex_tp"] = "0";
          auto request = self->make_request(
            "ka10075", "/api/dostk/acnt", json::serialize(body),
            network::RateLimitGroup::ACCOUNT, operation->next_key_
          );
          self->http_->async_request(
            std::move(request),
            [self, operation](Result<network::HttpResponse> result) mutable {
              net::post(self->strand_, [self, operation, result = std::move(result)]() mutable {
                self->handle_open_orders_page(operation, std::move(result));
              });
            }
          );
        }
      )
    );
  }

  // Accumulates one page of amendable orders and either continues or completes.
  void handle_open_orders_page(
    const std::shared_ptr<OpenOrderOperation>& operation,
    Result<network::HttpResponse> result
  ) {
    if (!result) {
      deliver(
        std::move(operation->completion_), Result<KiwoomOpenOrderPage>(std::move(result.error()))
      );
      return;
    }
    const auto next_key = continuation_key(result.value());
    auto body = document(std::move(result));
    if (!body) {
      deliver(
        std::move(operation->completion_), Result<KiwoomOpenOrderPage>(std::move(body.error()))
      );
      return;
    }
    const std::size_t before = operation->page_.orders_.size();
    try {
      const auto rows = body.value().if_contains("oso");
      if (rows != nullptr) {
        if (!rows->is_array()) {
          throw std::runtime_error("oso is not an array");
        }
        for (const auto& value : rows->as_array()) {
          if (!value.is_object()) {
            throw std::runtime_error("oso row is not an object");
          }
          operation->page_.orders_.push_back(parse_open_order(value.as_object()));
        }
      }
    } catch (const std::exception& exception) {
      deliver(
        std::move(operation->completion_),
        Result<KiwoomOpenOrderPage>(protocol_error("invalid_open_orders_response", exception))
      );
      return;
    }
    const bool grew = operation->page_.orders_.size() > before;
    switch (advance_page(operation->next_key_, operation->pages_, grew, std::move(next_key))) {
      case PageStep::FINISH:
        operation->page_.received_at_ = std::chrono::system_clock::now();
        deliver(
          std::move(operation->completion_),
          Result<KiwoomOpenOrderPage>(std::move(operation->page_))
        );
        return;
      case PageStep::INCOMPLETE:
        deliver(
          std::move(operation->completion_),
          Result<KiwoomOpenOrderPage>(incomplete_listing("working orders", operation->pages_))
        );
        return;
      case PageStep::CONTINUE:
        request_open_orders(operation);
        return;
    }
  }

  // Returns the side a row states, and throws when it states none this adapter can read.
  //
  // Throwing fails the listing, which is deliberate and is the one place this adapter does *not*
  // tolerate an unrecognized value. Everywhere else an unknown wire value becomes `UNKNOWN` with
  // the raw text preserved, because both specifications require a client to tolerate one and a
  // single odd row must not cost a whole listing. `OrderSide` has no `UNKNOWN`: the choice is
  // between failing and asserting a side, and asserting the wrong one puts a resting sell into the
  // picture as a buy, where `RiskGuard` will not deduct it from what may be sold. A listing that
  // fails is a poll that fails, which the runner already treats as a reason to hold.
  static OrderSide read_side(const std::string& label) {
    if (const auto side = normalized_order_side(label)) {
      return *side;
    }
    throw std::runtime_error(
      "order row states a side this adapter cannot read: '" + label +
      "'; it names neither 매수 nor 매도"
    );
  }

  // Normalizes one `ka10075` unfilled row.
  static KiwoomOpenOrder parse_open_order(const json::object& row) {
    KiwoomOpenOrder open_order;
    const auto number = detail::text_field(row, "ord_no");
    const auto symbol = detail::bare_symbol(detail::text_field(row, "stk_cd"));
    open_order.order_ref_ = KiwoomOrderRef{number, symbol};
    open_order.original_order_number_ = detail::text_field(row, "orig_ord_no");
    open_order.amendable_quantity_ = detail::magnitude(row, "oso_qty");
    open_order.shape_name_ = detail::text_field(row, "trde_tp");

    OrderSnapshot& order = open_order.order_;
    order.order_id_ = number;
    order.instrument_.country_ = Country::KR;
    order.instrument_.venue_ = instrument_venue();
    order.instrument_.symbol_ = symbol;
    order.name_ = detail::text_field(row, "stk_nm");
    order.currency_ = Currency::krw();
    order.side_ = read_side(detail::text_field(row, "io_tp_nm"));
    apply_shape(open_order.shape_name_, order);
    order.status_ = normalized_order_status(detail::text_field(row, "ord_stt"));
    order.venue_ = listed_venue(
      detail::text_field(row, "stex_tp"), detail::text_field(row, "sor_yn")
    );
    order.quantity_ = detail::required_magnitude(row, "ord_qty");
    order.price_ = positive(detail::magnitude(row, "ord_pric"));
    order.trigger_price_ = reported_trigger(row, "stop_pric");
    // `tm` is `HHmmss` with no date. The calendar day is today in Seoul, and only assumed: the demo
    // server has listed an order of the previous day after midnight. The history path takes its date
    // from the request; where both carry the order, a sweep drops this row and keeps the dated one.
    order.ordered_at_ = detail::seoul_timestamp(today_in_seoul(), detail::text_field(row, "tm"));
    order.ordered_date_assumed_ = true;
    order.execution_.filled_quantity_ = detail::required_magnitude(row, "cntr_qty");
    resolve_open_status(open_order.amendable_quantity_, order);
    return open_order;
  }

  // Refuses to report an order closed on a row that says quantity is still outstanding.
  //
  // ## The contradiction
  //
  // `normalized_order_status` is shared with the history parser, so it maps `체결` to `FILLED` and
  // `취소` to `CANCELED` -- correct there, and on *this* endpoint those words would contradict the
  // row they arrive on. `ka10075` is the 미체결 listing, its array is `oso`, and `oso_qty` is the
  // quantity still outstanding. The specification documents `ord_stt` here with the single value
  // `접수`, so nothing establishes that Kiwoom ever sends the other two on this inquiry.
  //
  // **This is therefore a defensive rule, not a repair of an observed incident.** What is
  // established is only that if such a row arrived, the adapter would mishandle it, and that the
  // response carries the field needed to refuse to.
  //
  // ## Why a wrong answer here is not cosmetic
  //
  // `strategy::order_is_open` is false for `FILLED` and `CANCELED`, and
  // `Portfolio::working_quantity` skips any row that is not open. A resting order reported closed
  // would read as **zero** resting quantity: no sellable deduction, no working-order component in
  // the position ceiling, and a reconciliation that plans a second order beside the one already
  // out.
  //
  // ## The rule
  //
  // A word and a number disagree, and only one of them can be checked, so the number wins:
  // a positive `oso_qty` means the order is still open whatever `ord_stt` says. Which open status
  // follows from the filled quantity, the same way KIS's parser answers the same question --
  // `PARTIAL_FILLED` where something has filled, `PENDING` where nothing has. A row reporting
  // nothing outstanding keeps whatever the broker said, so this narrows a contradiction rather than
  // refusing to believe the broker.
  //
  // Applied here rather than inside `normalized_order_status`, because the contradicting quantity
  // exists only on this endpoint and the history parser reads those words legitimately.
  //
  // The two statuses tested are the two `normalized_order_status` can produce that mean the order
  // is over, named here rather than asked of `strategy::order_is_open`: an adapter must not depend
  // on the strategy layer, and `core/` is where the two layers may meet.
  static void resolve_open_status(
    const std::optional<Decimal64>& amendable, OrderSnapshot& order
  ) {
    const bool reported_over =
      order.status_ == OrderStatus::FILLED || order.status_ == OrderStatus::CANCELED;
    if (!reported_over) {
      return;
    }
    if (!amendable || amendable->coefficient() <= 0) {
      return;
    }
    order.status_ = order.execution_.filled_quantity_.coefficient() > 0 ?
      OrderStatus::PARTIAL_FILLED :
      OrderStatus::PENDING;
  }

  // Writes the three normalized shape facts from Kiwoom's Korean label.
  //
  // **`OrderSnapshot::shape_code_` is deliberately left empty.** Its documentation calls it "the
  // broker's own order-shape code, preserved verbatim", and Kiwoom does not send a code on a read
  // path at all — it sends a Korean display name, which `Code<8>` cannot hold. Writing the numeric
  // code this adapter *inferred* from the name would put something the broker never sent into a
  // field defined as what it did. The raw label is kept on `KiwoomOpenOrder::shape_name_` instead.
  //
  // A label the table does not know leaves all three fields `UNKNOWN` and does not fail the row,
  // because both specifications this library follows require a client to tolerate a value it does
  // not recognize, and one odd row must not fail a whole listing.
  static void apply_shape(std::string_view label, OrderSnapshot& order) {
    const auto* row = shape_by_name(label);
    if (row == nullptr) {
      return;
    }
    order.type_ = row->type_;
    order.time_in_force_ = row->time_in_force_;
    order.session_ = row->session_;
  }

  // What `close_cancelled_originals` needs of a `kt00007` row beyond its normalized snapshot.
  struct HistoryLink {
    // The order a completed cancellation row names in `ori_ord`; empty for any other row.
    std::string cancels_;
    // Whether the row is an ordinary order (`mdfy_cncl` empty or 일반), not a cancel or a modification.
    bool plain_{false};
    // `ord_remnq`, or nothing if absent or unreadable.
    std::optional<Decimal64> remaining_;
  };

  // One order-history listing spanning however many `kt00007` pages it needs.
  struct HistoryOperation {
    KiwoomHistoryQuery query_;
    OrderPage page_;
    // One per row of `page_.orders_`, for `close_cancelled_originals`.
    std::vector<HistoryLink> links_;
    std::string next_key_;
    int pages_{0};
    OrderPageHandler completion_;

    HistoryOperation(KiwoomHistoryQuery query, OrderPageHandler completion)
    : query_(std::move(query)), completion_(std::move(completion)) {}
  };

  // Starts the order-history listing for one calendar date.
  void start_order_history(KiwoomHistoryQuery query, OrderPageHandler completion) {
    if (!is_valid_date(query.date_)) {
      BrokerError error;
      error.category_ = ErrorCategory::INVALID_ARGUMENT;
      error.reached_broker_ = false;
      error.broker_code_ = "invalid_date";
      error.message_ = "a Kiwoom order-history inquiry requires a valid Gregorian date";
      deliver(std::move(completion), Result<OrderPage>(std::move(error)));
      return;
    }
    auto operation = std::make_shared<HistoryOperation>(std::move(query), std::move(completion));
    request_order_history(operation);
  }

  // Issues one page of `kt00007` 계좌별주문체결내역상세요청.
  void request_order_history(std::shared_ptr<HistoryOperation> operation) {
    ensure_token(
      TokenHandler(
        [self = shared_from_this(), operation](std::optional<BrokerError> token_error) mutable {
          if (token_error) {
            self->deliver(
              std::move(operation->completion_), Result<OrderPage>(std::move(*token_error))
            );
            return;
          }
          json::object body;
          body["ord_dt"] = detail::compact_text(operation->query_.date_);
          // `1:주문순` lists in order-number order, which is the order the rows were created in.
          body["qry_tp"] = "1";
          body["stk_bond_tp"] = "1";
          body["sell_tp"] = "0";
          body["stk_cd"] = operation->query_.symbol_;
          body["fr_ord_no"] = "";
          // `%` is 전체: every venue. Unlike the balance inquiry, this one publishes an all-venue
          // value, so a history listing is never scoped to one book.
          body["dmst_stex_tp"] = "%";
          auto request = self->make_request(
            "kt00007", "/api/dostk/acnt", json::serialize(body),
            network::RateLimitGroup::ACCOUNT, operation->next_key_
          );
          self->http_->async_request(
            std::move(request),
            [self, operation](Result<network::HttpResponse> result) mutable {
              net::post(self->strand_, [self, operation, result = std::move(result)]() mutable {
                self->handle_order_history_page(operation, std::move(result));
              });
            }
          );
        }
      )
    );
  }

  // Accumulates one page of order history and either continues or completes.
  void handle_order_history_page(
    const std::shared_ptr<HistoryOperation>& operation,
    Result<network::HttpResponse> result
  ) {
    if (!result) {
      deliver(std::move(operation->completion_), Result<OrderPage>(std::move(result.error())));
      return;
    }
    const auto next_key = continuation_key(result.value());
    auto body = document(std::move(result));
    if (!body) {
      deliver(std::move(operation->completion_), Result<OrderPage>(std::move(body.error())));
      return;
    }
    const std::size_t before = operation->page_.orders_.size();
    try {
      const auto rows = body.value().if_contains("acnt_ord_cntr_prps_dtl");
      if (rows != nullptr) {
        if (!rows->is_array()) {
          throw std::runtime_error("acnt_ord_cntr_prps_dtl is not an array");
        }
        for (const auto& value : rows->as_array()) {
          if (!value.is_object()) {
            throw std::runtime_error("order history row is not an object");
          }
          operation->page_.orders_.push_back(
            parse_history_row(value.as_object(), operation->query_.date_, config_.is_demo_)
          );
          operation->links_.push_back(history_link(value.as_object(), config_.is_demo_));
        }
      }
    } catch (const std::exception& exception) {
      deliver(
        std::move(operation->completion_),
        Result<OrderPage>(protocol_error("invalid_order_history_response", exception))
      );
      return;
    }
    const bool grew = operation->page_.orders_.size() > before;
    switch (advance_page(operation->next_key_, operation->pages_, grew, std::move(next_key))) {
      case PageStep::FINISH:
        operation->page_.received_at_ = std::chrono::system_clock::now();
        // Every page has been walked, so there is no cursor for a caller to continue from.
        // `has_history_cursor` says the same thing at compile time.
        operation->page_.next_cursor_.reset();
        // After the last page: a cancellation and its original may be on different pages.
        close_cancelled_originals(operation->page_.orders_, operation->links_);
        deliver(std::move(operation->completion_), Result<OrderPage>(std::move(operation->page_)));
        return;
      case PageStep::INCOMPLETE:
        deliver(
          std::move(operation->completion_),
          Result<OrderPage>(incomplete_listing("order history", operation->pages_))
        );
        return;
      case PageStep::CONTINUE:
        request_order_history(operation);
        return;
    }
  }

  // Reads a `kt00007` row's link to the order it cancels.
  //
  // A cancellation row is `mdfy_cncl` 취소확인, the documented word. The demo server writes 취소 with
  // `acpt_tp` 주문완료 instead (observed 2026-10-06 and 2026-10-08); that tuple counts on the demo only,
  // since a bare 취소 names the kind of amendment, not a completed one.
  static HistoryLink history_link(const json::object& row, bool demo) {
    HistoryLink link;
    const auto amendment = detail::text_field(row, "mdfy_cncl");
    link.plain_ = amendment.empty() || amendment == "일반";
    link.remaining_ = detail::magnitude(row, "ord_remnq");
    const bool cancellation = amendment == "취소확인" ||
      (demo && amendment == "취소" && detail::text_field(row, "acpt_tp") == "주문완료");
    if (cancellation) {
      const auto original = detail::text_field(row, "ori_ord");
      if (original.find_first_not_of('0') != std::string::npos) {
        link.cancels_ = original;
      }
    }
    return link;
  }

  // Marks an original order `CANCELED` when a cancellation row names it and nothing of it remains.
  //
  // `kt00007` keeps the original's own row as an ordinary order with no state word for the
  // cancellation (`acpt_tp` 주문완료 on the demo), so without this it reads `UNKNOWN` and never ends.
  // The original must be the only row with that number, plain, of the same symbol and side, with
  // `ord_remnq` present and zero (a refused or pending cancel leaves a remainder) and less filled than
  // ordered (a cancel that lost to a full fill stays `FILLED`). Its `cntr_qty` stays the filled quantity.
  static void close_cancelled_originals(std::vector<OrderSnapshot>& orders, const std::vector<HistoryLink>& links) {
    for (std::size_t cancel = 0; cancel < orders.size() && cancel < links.size(); ++cancel) {
      if (links[cancel].cancels_.empty()) {
        continue;
      }
      std::optional<std::size_t> found;
      bool ambiguous = false;
      for (std::size_t at = 0; at < orders.size() && at < links.size(); ++at) {
        if (orders[at].order_id_ != links[cancel].cancels_) {
          continue;
        }
        ambiguous = ambiguous || found.has_value();
        found = at;
      }
      if (!found || ambiguous) {
        continue;
      }
      auto& original = orders[*found];
      const auto& link = links[*found];
      if (!link.plain_ || !link.remaining_ || link.remaining_->coefficient() != 0 ||
          original.instrument_.symbol_ != orders[cancel].instrument_.symbol_ ||
          original.side_ != orders[cancel].side_ ||
          !decimal_less(original.execution_.filled_quantity_, original.quantity_)) {
        continue;
      }
      original.status_ = OrderStatus::CANCELED;
    }
  }

  // Normalizes one `kt00007` row.
  static OrderSnapshot parse_history_row(const json::object& row, const Date& date, bool demo = false) {
    OrderSnapshot order;
    order.order_id_ = detail::text_field(row, "ord_no");
    order.instrument_.country_ = Country::KR;
    order.instrument_.venue_ = instrument_venue();
    // `kt00007` prefixes the symbol with `A` where `ka10075` does not, so a listing that kept the
    // prefix would build an `InstrumentId` that did not compare equal to the same instrument's
    // working order or holding.
    order.instrument_.symbol_ = detail::bare_symbol(detail::text_field(row, "stk_cd"));
    order.name_ = detail::text_field(row, "stk_nm");
    order.currency_ = Currency::krw();
    order.side_ = read_side(detail::text_field(row, "io_tp_nm"));
    // The label vocabulary here differs from `ka10075`'s -- 지정가 rather than 보통 -- and
    // `shape_by_name` searches both columns so the two listings agree about one order. The demo
    // server writes 보통가 for an ordinary limit order (observed 2026-10-06 and 2026-10-13); that
    // spelling counts on the demo only, as its cancellation words do.
    const auto label = detail::text_field(row, "trde_tp");
    apply_shape(demo && label == "보통가" ? std::string_view("보통") : std::string_view(label), order);
    order.status_ = history_status(row);
    order.venue_ = normalized_venue(detail::text_field(row, "dmst_stex_tp"));
    order.quantity_ = detail::required_magnitude(row, "ord_qty");
    order.price_ = positive(detail::magnitude(row, "ord_uv"));
    order.trigger_price_ = reported_trigger(row, "cond_uv");
    order.ordered_at_ = detail::seoul_timestamp(date, detail::text_field(row, "ord_tm"));
    order.execution_.filled_quantity_ = detail::required_magnitude(row, "cntr_qty");
    if (const auto average = positive(detail::magnitude(row, "cntr_uv"))) {
      order.execution_.average_filled_price_ = average;
    }
    return order;
  }

  // Reads a history row's lifecycle state.
  //
  // `kt00007` publishes two columns and neither alone is the answer: `acpt_tp` 접수구분 says the
  // order was received, and `mdfy_cncl` 정정취소 says whether this row is a 정정확인 or a 취소확인.
  // A cancellation confirmation is a cancelled order, and nothing else on the row says so.
  //
  // A fill is read from the quantities rather than from a word, because `kt00007` publishes no
  // 체결 state at all — `ord_remnq` 주문잔량 is what remains.
  static OrderStatus history_status(const json::object& row) {
    const auto amendment = detail::text_field(row, "mdfy_cncl");
    if (amendment == "취소확인") {
      return OrderStatus::CANCELED;
    }
    if (amendment == "정정확인") {
      return OrderStatus::REPLACED;
    }
    const auto filled = detail::required_magnitude(row, "cntr_qty");
    const auto remaining = detail::magnitude(row, "ord_remnq");
    if (filled.coefficient() > 0) {
      if (remaining && remaining->coefficient() == 0) {
        return OrderStatus::FILLED;
      }
      return OrderStatus::PARTIAL_FILLED;
    }
    return normalized_order_status(detail::text_field(row, "acpt_tp"));
  }
};

/**
 * @brief A blocking Kiwoom client: each call runs a `KiwoomClient` request on a private event loop
 *   and returns its result. Not thread-safe. Order references work with either client.
 *
 * A call whose loop stops before the request completes returns a `TRANSPORT` error
 * (`event_loop_stopped`).
 */
class KiwoomBlockingClient {
public:
  /**
   * @brief Constructs a client.
   * @param context The TLS context; borrowed, and must outlive the client.
   * @param config The credentials and environment.
   * @param options HTTP options.
   */
  KiwoomBlockingClient(
    ssl::context& context,
    KiwoomConfig config,
    network::HttpClientOptions options = {}
  )
  : client_(
      std::make_shared<KiwoomClient>(ioc_.get_executor(), context, std::move(config), options)
    ) {}

  /**
   * @brief Returns the current normalized quote or a structured error.
   * @param instrument The instrument.
   * @return The result, or an error.
   */
  Result<QuoteSnapshot> quote(InstrumentId instrument) {
    return run<QuoteSnapshot>(
      [&](auto&& sink) { client_->async_quote(std::move(instrument), std::move(sink)); },
      "quote"
    );
  }

  /**
   * @brief Returns today's upper and lower price bound.
   * @param instrument The instrument.
   * @return The result, or an error.
   */
  Result<PriceLimits> price_limits(InstrumentId instrument) {
    return run<PriceLimits>(
      [&](auto&& sink) { client_->async_price_limits(std::move(instrument), std::move(sink)); },
      "price limits"
    );
  }

  /**
   * @brief Returns the resting orders on both sides of the book.
   * @param instrument The instrument.
   * @return The result, or an error.
   */
  Result<DepthSnapshot> depth(InstrumentId instrument) {
    return run<DepthSnapshot>(
      [&](auto&& sink) { client_->async_depth(std::move(instrument), std::move(sink)); },
      "depth"
    );
  }

  /**
   * @brief Returns recent executions, newest first.
   * @param instrument The instrument.
   * @param count The most trades to return.
   * @return The result, or an error.
   */
  Result<std::vector<TradePrint>> trades(InstrumentId instrument, int count) {
    return run<std::vector<TradePrint>>(
      [&](auto&& sink) { client_->async_trades(std::move(instrument), count, std::move(sink)); },
      "trades"
    );
  }

  /**
   * @brief Returns static reference data for one instrument.
   * @param subject The instrument.
   * @return The result, or an error.
   */
  Result<Instrument> instrument(InstrumentId subject) {
    return run<Instrument>(
      [&](auto&& sink) { client_->async_instrument(std::move(subject), std::move(sink)); },
      "instrument"
    );
  }

  /**
   * @brief Returns bars; see `KiwoomClient::async_bars`.
   * @param query The query.
   * @return The result, or an error.
   */
  Result<BarSeries> bars(BarQuery query) {
    return run<BarSeries>(
      [&](auto&& sink) { client_->async_bars(std::move(query), std::move(sink)); },
      "bars"
    );
  }

  /**
   * @brief Returns the configuration; see `KiwoomClient::config`.
   * @return The configuration.
   */
  const KiwoomConfig& config() const noexcept { return client_->config(); }

  /**
   * @brief Returns the holdings for `KiwoomConfig::holdings_venue_`; see `KiwoomClient::async_holdings`.
   * @param account The account (empty).
   * @return The result, or an error.
   */
  Result<HoldingsSnapshot> holdings(KiwoomAccount account) {
    return run<HoldingsSnapshot>(
      [&](auto&& sink) { client_->async_holdings(account, std::move(sink)); },
      "holdings"
    );
  }

  /**
   * @brief Returns what the bound credential's account may spend.
   * @param currency The currency; anything but KRW is refused before sending.
   * @param account The bound account.
   * @return The result, or an error.
   */
  Result<BuyingPower> buying_power(Currency currency, KiwoomAccount account) {
    return run<BuyingPower>(
      [&](auto&& sink) { client_->async_buying_power(currency, account, std::move(sink)); },
      "buying power"
    );
  }

  /**
   * @brief Returns Kiwoom's own fuller deposit detail.
   * @return The result, or an error.
   */
  Result<KiwoomDeposit> deposit() {
    return run<KiwoomDeposit>(
      [&](auto&& sink) { client_->async_deposit(std::move(sink)); },
      "deposit"
    );
  }

  /**
   * @brief Returns what may be ordered of one instrument, per margin tier.
   * @param instrument The instrument.
   * @param side The side.
   * @param price The unit price to evaluate at (`uv`), or empty to send none.
   * @return The result, or an error.
   */
  Result<KiwoomOrderableAmount> orderable_amount(
    InstrumentId instrument,
    OrderSide side,
    std::optional<Decimal64> price = std::nullopt
  ) {
    return run<KiwoomOrderableAmount>(
      [&](auto&& sink) {
        client_->async_orderable_amount(
          std::move(instrument), side, std::move(price), std::move(sink)
        );
      },
      "orderable amount"
    );
  }

  /**
   * @brief Returns the one account the bound credential drives.
   * @return The result, or an error.
   */
  Result<std::vector<KiwoomAccountInfo>> accounts() {
    return run<std::vector<KiwoomAccountInfo>>(
      [&](auto&& sink) { client_->async_accounts(std::move(sink)); },
      "accounts"
    );
  }

  /**
   * @brief Submits a new order. The outcome can be unknown after an error; see
   *   `KiwoomClient::async_place_order`.
   * @param account The bound account.
   * @param order The order.
   * @return The result, or an error.
   */
  Result<KiwoomOrderAck> place_order(KiwoomAccount account, OrderRequest order) {
    return run<KiwoomOrderAck>(
      [&](auto&& sink) {
        client_->async_place_order(account, std::move(order), std::move(sink));
      },
      "place order"
    );
  }

  /**
   * @brief Modifies an existing order. The outcome can be unknown after an error; see
   *   `KiwoomClient::async_replace_order`.
   * @param account The bound account.
   * @param amendment The amendment.
   * @return The result, or an error.
   */
  Result<KiwoomOrderAck> replace_order(KiwoomAccount account, KiwoomAmendment amendment) {
    return run<KiwoomOrderAck>(
      [&](auto&& sink) {
        client_->async_replace_order(account, std::move(amendment), std::move(sink));
      },
      "replace order"
    );
  }

  /**
   * @brief Cancels an existing order. The outcome can be unknown after an error; see
   *   `KiwoomClient::async_cancel_order`.
   * @param account The bound account.
   * @param amendment The amendment.
   * @return The result, or an error.
   */
  Result<KiwoomOrderAck> cancel_order(KiwoomAccount account, KiwoomAmendment amendment) {
    return run<KiwoomOrderAck>(
      [&](auto&& sink) {
        client_->async_cancel_order(account, std::move(amendment), std::move(sink));
      },
      "cancel order"
    );
  }

  /**
   * @brief Returns every order still open to amendment.
   * @param account The bound account.
   * @return The result, or an error.
   */
  Result<KiwoomOpenOrderPage> open_orders(KiwoomAccount account) {
    return run<KiwoomOpenOrderPage>(
      [&](auto&& sink) { client_->async_open_orders(account, std::move(sink)); },
      "open orders"
    );
  }

  /**
   * @brief Returns one day of order history.
   * @param account The bound account.
   * @param query The query.
   * @return The result, or an error.
   */
  Result<OrderPage> order_history(KiwoomAccount account, KiwoomHistoryQuery query) {
    return run<OrderPage>(
      [&](auto&& sink) {
        client_->async_order_history(account, std::move(query), std::move(sink));
      },
      "order history"
    );
  }

  /** @brief Closes the client for good; see `KiwoomClient::close`. */
  void close() { client_->close(); }

  /**
   * @brief Returns the rate limiter's statistics.
   *
   * Read them on the client's strand or while no request is in progress (between calls on the
   * blocking client).
   *
   * @return The statistics, or null if the client has no rate-limit budget.
   */
  const network::RateLimitStats* rate_limit_stats() const { return client_->rate_limit_stats(); }

private:
  // Drives one asynchronous operation to completion on the private loop.
  template <typename T, typename Start>
  Result<T> run(Start start, std::string_view operation) {
    std::optional<Result<T>> result;
    ioc_.restart();
    start([&result](Result<T> value) { result.emplace(std::move(value)); });
    ioc_.run();
    if (!result) {
      BrokerError error;
      error.category_ = ErrorCategory::TRANSPORT;
      error.broker_code_ = "event_loop_stopped";
      error.message_ = "event loop stopped before " + std::string(operation) + " completion";
      return Result<T>(std::move(error));
    }
    return std::move(*result);
  }

  net::io_context ioc_;
  std::shared_ptr<KiwoomClient> client_;
};

} // namespace algotradelib::kiwoom
