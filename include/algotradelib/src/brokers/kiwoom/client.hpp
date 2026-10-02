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
 * Selects a Kiwoom trading account — and carries nothing, because Kiwoom carries nothing.
 *
 * **No Kiwoom endpoint takes an account number.** Not the order endpoints (`kt10000`-`kt10003`),
 * not the balance (`kt00018`), not the deposit inquiry (`kt00001`), not the unfilled listing
 * (`ka10075`), not the execution detail (`kt00007`). The application key *is* the account, and
 * `ka00001` 계좌번호조회 exists precisely because a caller otherwise has no way to find out which
 * one it is.
 *
 * So the selector is empty rather than absent. The port binds an account at construction for every
 * broker, and an empty selector is the honest answer to "which account did you bind": the one the
 * credential drives, and there is no other.
 */
struct KiwoomAccount {
  /** Two selectors are equal because a Kiwoom credential drives exactly one account. */
  friend bool operator==(const KiwoomAccount&, const KiwoomAccount&) { return true; }
};

/** One account Kiwoom's `ka00001` reports for the bound credential. */
struct KiwoomAccountInfo {
  /** The account number as Kiwoom reported it, for display. */
  std::string account_number_;
};

/**
 * Identifies one accepted Kiwoom order.
 *
 * **Kiwoom addresses an order by order number *and* symbol.** `kt10002` 정정주문 and `kt10003`
 * 취소주문 both require `stk_cd` beside `orig_ord_no`, so an order reference is not reducible to
 * the order number alone — exactly the situation `OrderRef`'s two parts exist for, and the same
 * shape KIS needs for its branch code.
 *
 * The symbol is stored as Kiwoom's six-digit code without the tape suffix and without the `A`
 * prefix some of its account listings add, because that is the form its order endpoints take.
 */
struct KiwoomOrderRef {
  /** Kiwoom's `ord_no`. */
  std::string order_number_;
  /** Kiwoom's `stk_cd`, which its amendment endpoints require beside the order number. */
  std::string symbol_;
};

/**
 * Quantity and price instruction for a Kiwoom modification or cancellation.
 *
 * Unlike KIS, Kiwoom does **not** re-send the order's shape on an amendment: `kt10002` carries
 * `mdfy_qty`, `mdfy_uv` and `mdfy_cond_uv`, and `kt10003` carries `cncl_qty` alone. So an
 * amendment cannot change the price basis, the validity condition or the session, and an
 * instruction asking for one of those changes is refused rather than sent as a reprice that
 * silently leaves the shape alone (Decision 6).
 *
 * `all_remaining_` **does** have a Kiwoom counterpart, and it is the quantity itself: both endpoints
 * document `0` as 잔량 전부. See `quantity_`.
 */
struct KiwoomAmendment {
  /** The order to amend, which carries the symbol Kiwoom requires beside the order number. */
  KiwoomOrderRef order_;
  /** Where the order lives; Kiwoom's `dmst_stex_tp`. */
  Venue venue_{Venue::UNSPECIFIED};
  /**
   * The quantity affected, or zero to mean everything still resting.
   *
   * **Kiwoom spells "all remaining" as the quantity `0`**, on both endpoints: `kt10002` documents
   * `mdfy_qty` as "'0' 입력 시 잔량 전부 정정" and `kt10003` documents `cncl_qty` as
   * "'0' 입력시 잔량 전부 취소". That is a better answer than a number read from a listing, because
   * the listing can be stale: a partial fill landing between the read and the cancel makes a
   * stated quantity larger than what is left, and Kiwoom's own `0` is evaluated when it processes
   * the instruction.
   *
   * `all_remaining_` on a normalized `AmendInstruction` is what selects it; see
   * `BrokerTraits::to_amendment`.
   */
  Decimal64 quantity_;
  /** The new price, on a modification. Absent on a cancellation and on an unpriced basis. */
  std::optional<Decimal64> price_;
  /**
   * The new trigger price, which Kiwoom carries as `mdfy_cond_uv`.
   *
   * Set by `BrokerTraits::to_amendment` from `OrderShape::trigger_price_` on a modification of a
   * stop-limit (`28`) order, and never on a cancellation: `kt10003` has no trigger field, and a
   * cancellation carrying one is refused here rather than acknowledged with the trigger dropped.
   * `reprice` restates the resting trigger, so a repriced stop keeps it rather than sending the
   * empty `mdfy_cond_uv` whose meaning Kiwoom does not document.
   */
  std::optional<Decimal64> condition_price_;
};

/**
 * Kiwoom acknowledgement of an accepted order, modification, or cancellation.
 *
 * Acceptance means Kiwoom recorded the instruction, not that it executed. Kiwoom's order responses
 * carry only `ord_no` (and, on an amendment, `base_orig_ord_no` and the affected quantity), so the
 * resulting state must be read back through `ka10075` or `kt00007`.
 */
struct KiwoomOrderAck {
  /** The normalized acknowledgement, whose `accepted_at_` is local receipt. */
  OrderAck ack_;
  /** The reference a later amendment must address. */
  KiwoomOrderRef order_;
  /** Kiwoom's `base_orig_ord_no` on an amendment: the order this instruction acted on. */
  std::string base_order_number_;
  /** The quantity Kiwoom echoed on an amendment, `mdfy_qty` or `cncl_qty`. Empty on a placement. */
  std::optional<Decimal64> amended_quantity_;
};

/** One Kiwoom order that is still open to modification or cancellation. */
struct KiwoomOpenOrder {
  /** The order's normalized state. */
  OrderSnapshot order_;
  /** The reference an amendment must address. */
  KiwoomOrderRef order_ref_;
  /** Kiwoom's `orig_ord_no`: which earlier order this one superseded. Lineage, not an address. */
  std::string original_order_number_;
  /** How much Kiwoom says is still unfilled, `oso_qty`, and therefore still amendable. */
  std::optional<Decimal64> amendable_quantity_;
  /**
   * Kiwoom's `trde_tp`, preserved verbatim — and it is a Korean display name, not a code.
   *
   * **This is why `OrderSnapshot::shape_code_` is empty on every Kiwoom row.** Kiwoom takes a
   * numeric `trde_tp` on a request (`0`, `3`, `10`…) and reports a Korean name on a response
   * (`보통`, `시장가`, `보통(IOC)`), and `Code<8>` holds `A`-`Z`, `0`-`9` and `_` only. Storing
   * the numeric code this adapter *inferred* from the name would make a field documented as
   * "what the broker sent" carry something the broker did not send, so it stays empty and the raw
   * name is kept here, reachable through `session.client()`.
   */
  std::string shape_name_;
};

/** One page of Kiwoom orders that remain modifiable or cancellable. */
struct KiwoomOpenOrderPage {
  /** The rows on this page. */
  std::vector<KiwoomOpenOrder> orders_;
  /** When the page arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

/**
 * Selector for one Kiwoom order-history inquiry.
 *
 * **One calendar date, not a range.** `kt00007` 계좌별주문체결내역상세요청 takes a single `ord_dt`,
 * so a query spanning more than one day is refused by the traits layer rather than answered with
 * one day of it.
 */
struct KiwoomHistoryQuery {
  /** The single 주문일자 to list. */
  Date date_;
  /** Restrict to one symbol, which Kiwoom expresses as `stk_cd`. Empty lists every symbol. */
  std::string symbol_;
};

/**
 * Returns today's calendar date in Seoul.
 *
 * Kiwoom dates every request and response in Korean local time, so "today" for a Kiwoom call is
 * today there, not wherever the process happens to run.
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
 * Refuses an instruction that can *open or enlarge* a position while this library cannot establish
 * what the account already holds.
 *
 * `kt00018` names one venue and publishes no all-venue value, so the normalized holdings verb
 * refuses (see `BrokerTraits::refuse_holdings`) -- and an instruction sized against an empty
 * portfolio is sized against the *permissive* baseline for `RiskGuard`'s position ceiling and for
 * anything sizing an entry. Leaving that to the loop's send gate left a configuration-shaped hole:
 * a schedule with no holdings cadence never calls the refusing port at all. So the refusal lives at
 * this adapter's own boundary, where the unanswered question is, and no schedule routes around it.
 *
 * **It covers a placement and a replacement, and not a cancellation.** The first version covered
 * placements alone, on the reasoning that an amendment addresses an order the broker already has
 * and never consults the portfolio -- which is true and is not the point. A resting buy priced
 * below the market is not executable; `kt10002` can move it to a price that is, and the fill opens
 * or enlarges exactly the position this cannot measure. Repricing is a risk-changing instruction
 * wearing an amendment's clothes.
 *
 * A **cancellation** stays available, and must: `kt10003` only ever reduces what is resting, it
 * needs the order's reference rather than the portfolio, and an order that cannot be withdrawn
 * keeps resting and can fill. Reducing a resting order's size is still expressible while this
 * holds, through `cancel_quantity()`.
 *
 * Applied by the client, which every placement and every amendment passes through -- the port's own
 * `place` and `replace` both reach it -- so neither the normalized verb nor `session.client()`
 * routes around it.
 *
 * Deleted when `issues/kiwoom-api-holdings-venue-scope.md` is answered; it is one request pair.
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
    "read issues/kiwoom-api-holdings-venue-scope.md -- an assertion that is wrong undercounts the "
    "account. Two requests settle it for real; the experiment is in that issue";
  return error;
}

/**
 * Refuses an order carrying an idempotency key, which Kiwoom does not publish.
 *
 * Applied by the client and by the traits layer, for the reason KIS applies its own twice: the port
 * refuses before a client is asked, and the client refuses whether or not the caller came through
 * the port.
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

/** Returns whether `trigger` is a value `cond_uv` and `mdfy_cond_uv` can carry: positive, whole won. */
inline bool valid_trigger_value(const Decimal64& trigger) {
  return trigger.coefficient() > 0 && trigger.scale() == 0;
}

/**
 * Returns an error when a trigger price does not match its price basis.
 *
 * `28` 스톱지정가 needs one in `cond_uv` and every other `trde_tp` has nowhere to put one, so a
 * trigger stated on it would be sent as `""` and dropped (Decision 6). The trigger's direction and
 * its relation to the limit are not checked: the specification documents neither.
 *
 * Applied to a placement by the client and to a modification by the traits layer, which is the one
 * that still knows the basis -- `kt10002` carries no `trde_tp`, so `KiwoomAmendment` has none.
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

namespace detail {

/**
 * Reads one of Kiwoom's signed magnitude strings as a price or a quantity.
 *
 * **Kiwoom prefixes a market-data number with a change *direction*, not a sign.** `"+181400"` and
 * `"-91200"` are prices of 181,400 and 91,200 won; the specification says so field by field
 * ("단위: 원, 부호가 포함된 숫자"). Parsing `-91200` as a negative price produces a number that is
 * wrong by a factor of minus one, is a perfectly valid `Decimal64`, and passes every type check
 * downstream — so every price and quantity this adapter reads from a market-data response goes
 * through here, and the direction is read separately from `pred_pre_sig` where it is wanted.
 *
 * Account responses pad to a fixed width with leading zeros (`"000000017598258"`), which
 * `Decimal64::parse` handles, and may carry a genuine minus on a profit-and-loss figure — so
 * `signed_value` is the entry point for those, and this one is for the market-data fields whose
 * sign is a direction.
 *
 * Returns nothing for an empty or unparsable field, which is how Kiwoom spells "no value".
 */
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

/** Reads a magnitude, or zero where Kiwoom reported nothing. */
inline Decimal64 magnitude_or_zero(const json::object& row, std::string_view key) {
  const auto value = magnitude(row, key);
  return value ? *value : Decimal64();
}

/**
 * Reads a field whose minus sign is arithmetic rather than a direction.
 *
 * **Use this only where the value can genuinely be negative**: holdings' profit and loss and return
 * ratios, and the `kt00001` / `kt00010` deposit figures, which go negative when the account is in
 * 미수. Everything that is a price, a quantity or a holdings money total goes through `magnitude`,
 * because none of those can be negative and Kiwoom documents
 * *both* kinds with the same phrase: `kt00018`'s 현재가 is "단위: 원, 좌측 0-padding 처리된 부호 포함
 * 12자리 숫자", the identical wording it uses for 평가손익 on the row beside it.
 *
 * So the field description cannot tell them apart, and only the *meaning* can: a 현재가 of minus
 * 59,000 won is not a price, so a sign there is a direction — while a 평가손익 of minus 196,888 won
 * is exactly what it says. Reading a mark price as negative would put it straight into
 * `HoldingPosition::last_price_` and from there into every valuation that reads a holding.
 *
 * Values are zero-padded to a fixed width, sometimes with the sign inside the padding, and
 * `Decimal64::parse` reads both the sign and the leading zeros.
 */
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

/**
 * Reads a magnitude that must be there, and throws when it is not.
 *
 * **`magnitude_or_zero` cannot be used for a figure a risk check consumes.** It turns a missing,
 * blank *or malformed* field into zero, and zero is a statement: a holdings row whose `rmnd_qty`
 * came back garbled would be a successful listing saying the account holds none of that
 * instrument, and `Portfolio::net_quantity_of` would undercount the exposure a position ceiling is
 * measured against. A working order whose `ord_qty` was unreadable would understate
 * `Portfolio::working_quantity`, which is what the sellable guard deducts a resting sell with.
 *
 * A *reported* zero is still zero and still fine. It is the unreadable one that must fail the row,
 * because the two are indistinguishable afterwards.
 *
 * **The line is drawn at what a risk check consumes, and no wider**, because the failure mode of
 * this guard is the opposite one: a field made required that Kiwoom may legitimately omit fails a
 * valid listing, and a listing that fails is a poll that fails. So it covers `rmnd_qty`
 * (`Portfolio::net_quantity_of`), and `ord_qty` and `cntr_qty` (`Portfolio::working_quantity`),
 * plus a bar's `trde_qty`, which a strategy reads directly. The reporting figures beside them --
 * cost basis, market value, profit and loss, the account totals, the deposit detail -- stay
 * tolerant: a missing one there is wrong in a report, not in an order, and an account with no
 * positions is the kind of edge case that should not fail a poll.
 */
inline Decimal64 required_magnitude(const json::object& row, std::string_view key) {
  const auto value = magnitude(row, key);
  if (!value) {
    throw std::runtime_error("missing or unreadable numeric field: " + std::string(key));
  }
  return *value;
}

/** Reads a genuinely signed figure that must be there, and throws when it is not. */
inline Decimal64 required_signed(const json::object& row, std::string_view key) {
  const auto value = signed_value(row, key);
  if (!value) {
    throw std::runtime_error("missing or unreadable numeric field: " + std::string(key));
  }
  return *value;
}

/** Reads a signed field, or zero where Kiwoom reported nothing. */
inline Decimal64 signed_or_zero(const json::object& row, std::string_view key) {
  const auto value = signed_value(row, key);
  return value ? *value : Decimal64();
}

/** Reads a string field, or an empty string where Kiwoom reported none. */
inline std::string text_field(const json::object& row, std::string_view key) {
  const auto value = row.if_contains(key);
  if (!value || !value->is_string()) {
    return {};
  }
  return std::string(value->as_string());
}

/**
 * Returns a symbol as Kiwoom's order endpoints spell it.
 *
 * **Kiwoom's account listings prefix a symbol with `A`** — `kt00018` reports `"A005930"` and
 * `kt00007` reports `"A069500"` — while `ka10075` and every market-data endpoint report the bare
 * six digits. An instrument identity built from the prefixed form would not compare equal to one
 * built from the bare form, so a holding and its own working order would stop matching. The prefix
 * is stripped once, here.
 *
 * The tape suffix is stripped for the same reason: `005930_AL` is the same instrument as `005930`,
 * read off a different book.
 */
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

/** Reads Kiwoom's `HHmmss` or `HH:mm:ss` clock time as an instant on `date` in Seoul. */
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

/** Reads Kiwoom's `YYYYMMDD` as a calendar date, or nothing when it is not one. */
inline std::optional<Date> compact_date(std::string_view text) {
  return broker_detail::compact_date(text);
}

/**
 * Reads a `YYYYMMDD` field that states a date or leaves it blank, as one of three answers.
 *
 * Reads the raw value rather than `text_field`, which maps every non-string to `""` and would so
 * report a number as a date not stated. Not stated: absent, `null` or `""`. Stated: eight digits
 * forming a real date. Unreadable: anything else -- **including `"00000000"`**, since nothing in
 * Kiwoom's specification uses a zero date as a blank, and KIS's use of one says nothing about Kiwoom.
 */
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

/** Renders a calendar date as Kiwoom's `YYYYMMDD`. */
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

/**
 * One row of Kiwoom's order-shape vocabulary.
 *
 * **Kiwoom spells this one field three different ways**, and the adapter needs all three:
 *
 * - `kt10000` / `kt10001` take a **numeric** `trde_tp` on a request: `0`, `3`, `10`, `40`.
 * - `ka10075` 미체결 and `ka10076` 체결 report a **Korean name**, using `보통` for a plain limit,
 *   and their label list includes `시간외단일가`, which no request table has.
 * - `kt00007` and `kt00009` 주문체결내역 report a Korean name too — but a *different* one, using
 *   `지정가` where the other listings say `보통`, and publishing neither `시간외단일가` nor the GTP
 *   shapes.
 *
 * All three live in one table rather than three, because a shape that could be requested under one
 * spelling and read back as something else is exactly the drift a single table exists to prevent.
 * A row therefore carries the code and up to two names; `alternate_name_` is null where the two
 * listings agree, and `code_` is null for a shape that exists only on a read path.
 */
struct KiwoomOrderShapeRow {
  /** The numeric `trde_tp` a request carries, or null for a shape this library cannot send. */
  const char* code_;
  /** The name `kt10000`, `ka10075` and `ka10076` use. */
  const char* name_;
  /** The name `kt00007` and `kt00009` use, or null where it is the same as `name_`. */
  const char* alternate_name_;
  /** The normalized price basis. */
  OrderType type_;
  /** The normalized execution condition. */
  TimeInForce time_in_force_;
  /** The normalized session. */
  OrderSession session_;
  /** Whether this adapter may *send* the code, as opposed to reading it. */
  bool requestable_;
};

/**
 * Kiwoom's `trde_tp` table, transcribed from `docs/api/kiwoom/kiwoom-rest-api-spec.json`.
 *
 * The request column comes from `kt10000`'s `requestIo`, the first name column from `ka10075`'s and
 * `ka10076`'s `responseIo`, and the second from `kt00007`'s and `kt00009`'s.
 *
 * The twenty request codes factor into the core's three axes with nothing left over, which is worth
 * recording: the three-axis decomposition was derived from KIS's `ORD_DVSN` and Toss's
 * `timeInForce`, and Kiwoom shares no wire format with either.
 *
 * **One row is read-only.** `28` 스톱지정가 was a second until `OrderRequest::trigger_price_`
 * existed; its trigger goes in `cond_uv`, and `invalid_trigger` refuses a `28` without one.
 *
 * `시간외단일가` has **no code at all**. It appears in `ka10075`'s and `ka10076`'s label lists and in
 * no request table, so this library could not place one even if `OrderRequest` could express it.
 * Reading it as a *limit* in the after-hours auction is an inference rather than something the
 * specification states — it names the auction without giving its price rule — and it is the same
 * inference the KIS adapter makes about its own response-only `07`, about a field it likewise
 * cannot send.
 *
 * **There is deliberately no per-venue column.** KIS's workbook publishes which codes each venue
 * offers and the KIS adapter refuses a mismatch locally. Kiwoom publishes one flat list plus a
 * separate `dmst_stex_tp` selector and no code-by-venue matrix. Inventing a restriction would refuse
 * orders Kiwoom accepts; inventing permission would be a guess the other way. So a combination
 * Kiwoom has no code for at all is refused before any I/O, and a venue-specific refusal comes from
 * Kiwoom with its own `return_code`, preserved.
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
 * Maps a normalized order shape to Kiwoom's `trde_tp`.
 *
 * A combination Kiwoom has no code for, or has a read-only code for, is rejected with
 * `INVALID_ARGUMENT` before any token fetch or socket write. It is never sent under a neighbouring
 * code (Decision 6).
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
 * Returns the row a response's Korean `trde_tp` names, or null when the table has none.
 *
 * **Both name columns are searched**, because `ka10075` and `kt00007` disagree about what to call a
 * limit order and a caller reading a working order through one and its history through the other
 * must get the same shape. A label in neither column yields null, and the caller leaves the
 * normalized fields `UNKNOWN` with the raw text preserved rather than mapping it to a neighbour.
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

/** Returns the row a numeric `trde_tp` names, or null when the table has none. */
inline const KiwoomOrderShapeRow* shape_by_code(std::string_view code) noexcept {
  for (const auto& row : kOrderShapes) {
    if (row.code_ != nullptr && code == row.code_) {
      return &row;
    }
  }
  return nullptr;
}

/** Returns Kiwoom's `dmst_stex_tp` value for `venue`, or an empty string where it sends none. */
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
 * Returns the venue a Kiwoom order carrying `asked` actually reaches.
 *
 * **Kiwoom documents `dmst_stex_tp` as a required field with no default**, so unlike KIS there is
 * no "omitted means KRX" rule to record. An order that names no venue is refused before any I/O
 * rather than being sent with a guessed one, and this function exists to say that in one place:
 * an unspecified venue resolves to itself, and `validate_order` refuses it.
 */
inline constexpr Venue kiwoom_effective_route(Venue asked) noexcept { return asked; }

/** Returns the venue Kiwoom's `stex_tp` / `dmst_stex_tp` names on a read path. */
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
 * Returns the venue a `ka10075` working-order row reports, from its `stex_tp` and `sor_yn`.
 *
 * **`sor_yn` is read in case `stex_tp` alone does not say SOR.** Kiwoom documents the two
 * separately -- `stex_tp` 거래소구분 as `0 : 통합, 1 : KRX, 2 : NXT` and `sor_yn` SOR 여부값 as
 * `Y,N`. Its only example SOR row (`ka10076`'s) reads `stex_tp 0`, `stex_tp_txt SOR`, `sor_yn Y`,
 * which `stex_tp` already reads as `COMBINED`; but nothing documents how a resting SOR order is listed.
 * If it came under the book it rests on with the flag set, `stex_tp` alone would say KRX while the
 * journal recorded `COMBINED`, recovery would read it as a different order, and a withdrawal would be
 * sent with `KRX` instead of the `SOR` it was placed with. So `Y` means `Venue::COMBINED` whatever
 * `stex_tp` says. Anything else, including an absent or unreadable
 * flag, leaves `stex_tp` to answer: the flag is informational for every other order, and one odd
 * row must not cost a listing. Documented, not yet observed on an account.
 */
inline Venue listed_venue(std::string_view stex_tp, std::string_view sor_yn) noexcept {
  if (sor_yn == "Y") {
    return Venue::COMBINED;
  }
  return normalized_venue(stex_tp);
}

/**
 * Returns the side Kiwoom's `io_tp_nm` names.
 *
 * Kiwoom spells it several ways across its listings — `+매수`, `-매도`, `현금매수`, `현금매도`,
 * `융자매수`, `매수정정`, `매도취소` — and publishes the field as free text with no closed value
 * list, so the test is which of 매수 or 매도 the text contains rather than an exact match.
 *
 * **Nothing here defaults.** `OrderSide` has no unknown member, and its default is `BUY`, so a
 * label this function cannot read would be *reported as a buy* if the caller left the field alone
 * — and a resting sell reported as a buy is not counted against the sellable quantity by
 * `RiskGuard`. Every caller therefore fails the row rather than accepting the default; see
 * `parse_open_order` and `parse_history_row`.
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
 * Returns the lifecycle state Kiwoom's `ord_stt` / `acpt_tp` names.
 *
 * Kiwoom publishes no status *code*, only a Korean word: `접수` (received), `확인` (confirmed —
 * which is what it calls an accepted modification or cancellation), and `체결` (filled). An
 * unrecognized word is `UNKNOWN` rather than being mapped to a neighbour, because both
 * specifications this library follows require a client to tolerate a status it does not know.
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
 * Returns the `api-id` of the chart endpoint that publishes `interval`, or null when none does.
 *
 * Null is unreachable today -- Kiwoom publishes all five intervals the core names -- and the
 * function is written to be able to say so anyway, because the alternative is what it replaced: a
 * lookup that fell through to the daily endpoint, so a sixth `BarInterval` added later would have
 * silently returned *daily bars* for it. `-Wswitch` would flag the new enumerator in a build with
 * warnings as errors, and this is what happens in one without.
 */
inline const char* chart_api_id(BarInterval interval) noexcept {
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
  }
  return nullptr;
}

/** Returns the array field that endpoint returns its rows in, or null when there is no endpoint. */
inline const char* chart_rows_field(BarInterval interval) noexcept {
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
  }
  return nullptr;
}

/**
 * Refuses a bar interval Kiwoom publishes no chart endpoint for.
 *
 * Reads the same table the request builder does rather than answering from a constant, so the
 * capability and the request cannot disagree. Every interval the core names is backed today, so
 * this refuses nothing -- and it will refuse the right thing on the day one is not.
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
    std::string("Kiwoom publishes no chart endpoint for ") +
    std::string(bar_interval_name(interval)) + " bars";
  return error;
}

/**
 * Kiwoom's deposit detail, `kt00001` 예수금상세현황요청, as the broker reports it.
 *
 * The port's `buying_power` verb answers one question — what may be spent — and reads
 * `spendable_` from it. The rest is here because Kiwoom's answer is richer than the port's
 * question and Decision 2b says to expose the difference rather than hide it or fake it.
 */
struct KiwoomDeposit {
  /**
   * `100stk_ord_alow_amt` 100%종목주문가능금액: what the account can buy funding the position in
   * full.
   *
   * **This, and not `orderable_`, is what `BuyingPower::cash_` is filled from**, and the difference
   * is leverage. `BuyingPower` documents its own field as "the credit-free figure both brokers
   * publish -- Toss's `cashBuyingPower`, KIS's `nrcvb_buy_amt`", and warns that "a number without
   * the model is how a strategy discovers leverage by accident". Kiwoom's own example makes the gap
   * concrete: 주문가능금액 is 85,341 against a 100%종목주문가능금액 of 12,550, on an account holding
   * 3,915,500 of 대용금 -- collateral, not cash. 미수없는매수금액 is what KIS means and
   * `100stk_ord_alow_amt` is what Kiwoom means by it; it agrees with `d2_entra` to the won.
   */
  Decimal64 spendable_;
  /** `ord_alow_amt` 주문가능금액: orderable *including* what margin and 대용금 allow. */
  Decimal64 orderable_;
  /** `entr` 예수금: the cash deposit. */
  Decimal64 deposit_;
  /** `d2_entra` D+2 추정예수금: the deposit once today's trades settle. */
  Decimal64 settled_deposit_;
  /** `repl_amt` 대용금: securities pledged as collateral, which is not cash. */
  Decimal64 substitute_amount_;
  /** `pymn_alow_amt` 출금가능금액: what may be withdrawn. */
  Decimal64 withdrawable_;
  /** The currency every figure above is in, which is won. */
  Currency currency_;
  /** When the figures arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

/** One margin tier of Kiwoom's `kt00010` answer: what may be ordered at that 증거금률. */
struct KiwoomMarginTier {
  /** The margin rate the row applies to, as a percentage: 20, 30, 40, 50, 60 or 100. */
  int margin_percent_{0};
  /** Whether this row is the 증거금감면 (reduced-margin) variant of its rate. */
  bool reduced_{false};
  /**
   * `profa_NNord_alow_amt`: the orderable amount at this rate, where Kiwoom reported one.
   *
   * Optional rather than zero-defaulted, because a tier Kiwoom did not answer for is not a tier at
   * which nothing may be ordered. The two are indistinguishable once a missing field has become a
   * zero, and this is the figure a caller gates a size on.
   */
  std::optional<Decimal64> amount_;
  /** `profa_NNord_alowq`: the orderable quantity at this rate. Empty as for `amount_`. */
  std::optional<Decimal64> quantity_;
};

/**
 * Kiwoom's `kt00010` 주문인출가능금액요청: what may be ordered, per margin tier, for one instrument.
 *
 * **This is why `BrokerCaps::has_buying_power_quantity` is true for Kiwoom.** That constant asks
 * whether the *broker* publishes an affordable quantity, not whether the port's verb returns one —
 * the port asks only for an amount, because Toss reports no quantity at all. Kiwoom publishes seven
 * tiers of amount-and-quantity, and a caller gating on the capability reaches them here through
 * `session.client()`.
 *
 * There is deliberately **no single "affordable quantity"** on this type. Which tier applies
 * depends on the instrument's 증거금률, which Kiwoom states per row and not as a selection, so
 * collapsing the seven into one would be this library choosing a margin rate on the caller's
 * behalf.
 */
struct KiwoomOrderableAmount {
  /** Every 증거금률 tier Kiwoom answered with, in the order it publishes them. */
  std::vector<KiwoomMarginTier> tiers_;
  /** `ord_alowa` 주문가능현금: the cash available to order with. */
  Decimal64 orderable_cash_;
  /** `wthd_alowa` 인출가능금액. */
  Decimal64 withdrawable_;
  /** `d2entra` D+2 추정예수금. */
  Decimal64 settled_deposit_;
  /** `profa_rdex_aplc_tp`: whether the 증거금감면 tiers are the ones in force (`1`) or not (`0`). */
  bool margin_reduction_applies_{false};
  /** The currency every amount above is in, which is won. */
  Currency currency_;
  /** When the figures arrived, on this machine's clock. */
  std::chrono::system_clock::time_point received_at_;
};

/**
 * Asynchronous Kiwoom REST client.
 *
 * Every Kiwoom request is a `POST` to a *group* path — `/api/dostk/acnt`, `/api/dostk/ordr`,
 * `/api/dostk/mrkcond` — with the operation named by an `api-id` header rather than by the path.
 * `make_request` is the one place that shape is built, so a new endpoint cannot quietly disagree
 * with the others about the envelope.
 */
class KiwoomClient : public std::enable_shared_from_this<KiwoomClient> {
public:
  /** Completion handler for one normalized quote. */
  using QuoteHandler = net::any_completion_handler<void(Result<QuoteSnapshot>)>;
  /** Completion handler for one normalized bar series. */
  using BarsHandler = net::any_completion_handler<void(Result<BarSeries>)>;
  /** Completion handler for one normalized order book. */
  using DepthHandler = net::any_completion_handler<void(Result<DepthSnapshot>)>;
  /** Completion handler for one instrument's price bounds. */
  using PriceLimitsHandler = net::any_completion_handler<void(Result<PriceLimits>)>;
  /** Completion handler for recent executions. */
  using TradesHandler = net::any_completion_handler<void(Result<std::vector<TradePrint>>)>;
  /** Completion handler for one instrument's reference data. */
  using InstrumentHandler = net::any_completion_handler<void(Result<Instrument>)>;
  /** Completion handler for one normalized holdings snapshot. */
  using HoldingsHandler = net::any_completion_handler<void(Result<HoldingsSnapshot>)>;
  /** Completion handler for one normalized buying-power reading. */
  using BuyingPowerHandler = net::any_completion_handler<void(Result<BuyingPower>)>;
  /** Completion handler for Kiwoom's own fuller deposit detail. */
  using DepositHandler = net::any_completion_handler<void(Result<KiwoomDeposit>)>;
  /** Completion handler for Kiwoom's per-margin-tier orderable amounts and quantities. */
  using OrderableHandler = net::any_completion_handler<void(Result<KiwoomOrderableAmount>)>;
  /** Completion handler for the one account the bound credential drives. */
  using AccountsHandler = net::any_completion_handler<void(Result<std::vector<KiwoomAccountInfo>>)>;
  /** Completion handler for one Kiwoom order acknowledgement. */
  using OrderAckHandler = net::any_completion_handler<void(Result<KiwoomOrderAck>)>;
  /** Completion handler for one page of modifiable or cancellable orders. */
  using OpenOrderHandler = net::any_completion_handler<void(Result<KiwoomOpenOrderPage>)>;
  /** Completion handler for one normalized page of order history. */
  using OrderPageHandler = net::any_completion_handler<void(Result<OrderPage>)>;
  /** Internal completion handler for a shared access-token refresh. */
  using TokenHandler = net::any_completion_handler<void(std::optional<BrokerError>)>;

  /**
   * Constructs a client for one Kiwoom environment.
   *
   * `config` is a value sink because the client retains its own credentials; lvalues are copied
   * once and rvalues are moved into the client.
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

  /** Requests and normalizes a current quote, from `ka10007` 시세표성정보요청. */
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

  /** Requests today's upper and lower price bound, from the same `ka10007` response. */
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

  /** Requests the resting orders on both sides of the book, from `ka10004` 주식호가요청. */
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

  /** Requests recent executions, newest first, from `ka10003` 체결정보요청. */
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

  /** Requests static reference data for one instrument, from `ka10100` 종목정보 조회. */
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

  /** Requests bars, paging Kiwoom's `base_dt` backwards until `count_` is satisfied. */
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
   * Returns the configuration this client was built with.
   *
   * Exposed because the port has to read one run-time assertion out of it -- what the operator says
   * `dmst_stex_tp` means -- to decide whether the normalized holdings verb may answer at all. A
   * capability constant cannot carry that, since it varies per configuration rather than per adapter.
   */
  const KiwoomConfig& config() const noexcept { return config_; }

  /**
   * Returns the strand every completion of this client is initiated from.
   *
   * Exposed for the port's one completion that performs no I/O -- the holdings refusal while the
   * scope is unstated -- so it is posted where every other completion of this client starts, and an
   * `io_context` running this client counts it as outstanding work.
   */
  net::strand<net::any_io_executor> get_executor() const noexcept { return strand_; }

  /** Requests every holding, paging `kt00018` 계좌평가잔고내역요청. */
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

  /** Requests what the account may spend, from `kt00001` 예수금상세현황요청. */
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

  /** Requests Kiwoom's own fuller deposit detail, which `async_buying_power` reads one field of. */
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
   * Requests what may be ordered of one instrument, per margin tier, from `kt00010`.
   *
   * Kiwoom-specific on purpose: the port asks only for an amount, because Toss publishes no
   * quantity. `BrokerCaps::has_buying_power_quantity` is what points a caller here.
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

  /** Requests the one account the bound credential drives, from `ka00001` 계좌번호조회. */
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

  /** Submits a new order, to `kt10000` 매수주문 or `kt10001` 매도주문 depending on the side. */
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

  /** Modifies an existing order, through `kt10002` 정정주문. */
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

  /** Cancels an existing order, through `kt10003` 취소주문. */
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

  /** Requests every order still open to amendment, paging `ka10075` 미체결요청. */
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

  /** Requests one day of order history, paging `kt00007` 계좌별주문체결내역상세요청. */
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

  /** Returns this client's pacing statistics, or null when it is unpaced. */
  const network::RateLimitStats* rate_limit_stats() const {
    const auto* limiter = http_->rate_limits();
    return limiter ? &limiter->stats() : nullptr;
  }

  /** Cancels transport work and queued operations. */
  void close() { http_->close(); }

private:
  friend struct KiwoomClientTestAccess;

  net::strand<net::any_io_executor> strand_;
  KiwoomConfig config_;
  std::shared_ptr<network::AsyncHttpClient> http_;
  KiwoomRest token_;
  bool refreshing_{false};
  std::deque<TokenHandler> token_waiters_;

  /** Most pages one paged inquiry may consume, for the reason every paging path is bounded. */
  static constexpr int page_budget_ = 50;

  /**
   * Builds one Kiwoom request: the group path, the operation header, and the token.
   *
   * **Every Kiwoom endpoint has this shape**, which is why it is built once. The operation is the
   * `api-id` header rather than the path — `/api/dostk/acnt` alone serves the balance, the deposit,
   * the unfilled listing and the execution detail — so a request with the right path and the wrong
   * header is a valid request for the wrong thing, and there is exactly one place that pairs them.
   */
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

  /**
   * Returns the continuation a response carries: nothing when it reports no further page.
   *
   * **An absent value and an empty one are different answers**, which is why this is an optional
   * rather than a string. `cont-yn: Y` means Kiwoom has another page; `next-key` is how to ask for
   * it. A `Y` with a missing or blank key is therefore *another page this walk cannot fetch* — and
   * collapsing that into "no further page", which returning a bare empty string did, turned a
   * listing the adapter could not finish into one it reported as complete.
   *
   * - `std::nullopt` — Kiwoom offered no further page. The listing is complete.
   * - a non-empty string — another page, and the key to ask for it.
   * - an empty string — another page, and no usable key. Incomplete.
   */
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

  /**
   * Fetches a token when the held one has expired, coalescing concurrent waiters.
   *
   * Identical in shape to the KIS and Toss clients: at most one refresh is ever in flight, and
   * every other request waits behind it.
   */
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

  /** Parses a token response and releases every waiter, with the failure if there was one. */
  void finish_token(Result<network::HttpResponse> result) {
    refreshing_ = false;
    std::optional<BrokerError> failure;
    if (!result) {
      failure = std::move(result.error());
    } else if (result.value().result_int() < 200 || result.value().result_int() >= 300) {
      failure = broker_detail::http_error(result.value());
      failure->category_ = ErrorCategory::AUTHENTICATION;
    } else {
      BrokerError parse_error;
      auto document = broker_detail::parse_object(result.value().body(), parse_error);
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

  /**
   * Returns a response's declared payload, or the error ending it.
   *
   * Applies the HTTP status check, the JSON parse, and Kiwoom's own `return_code` before a parser
   * looks at any field, so a declared failure is never read as data.
   */
  static Result<json::object> document(Result<network::HttpResponse> response) {
    if (!response) {
      return Result<json::object>(std::move(response.error()));
    }
    if (response.value().result_int() < 200 || response.value().result_int() >= 300) {
      return Result<json::object>(broker_detail::http_error(response.value()));
    }
    BrokerError parse_error;
    auto parsed = broker_detail::parse_object(response.value().body(), parse_error);
    if (!parsed) {
      return Result<json::object>(std::move(parse_error));
    }
    if (auto declared = kiwoom_declared_error(*parsed)) {
      return Result<json::object>(std::move(*declared));
    }
    return Result<json::object>(std::move(*parsed));
  }

  /** Refuses an instrument this adapter's domestic endpoints cannot address. */
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

  /** Builds the `{"stk_cd": ...}` body every market-data endpoint takes, with the tape suffix. */
  std::string symbol_body(const InstrumentId& instrument) const {
    json::object body;
    body["stk_cd"] = config_.tape_symbol(instrument.symbol_);
    return json::serialize(body);
  }

  /** Issues one market-data read and hands the response to `parse`. */
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

  /**
   * Normalizes `ka10007` into a quote.
   *
   * The last price, the volume and the top of the book all come off one response, so the quote
   * carries a `top_of_book_` — which neither shipped adapter fills, because neither KIS's nor
   * Toss's quote endpoint publishes one. Kiwoom's does, in the same response, at no extra request.
   */
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

  /**
   * Normalizes `ka10007`'s price bounds.
   *
   * KRX sets a limit on every listed equity, so both bounds are expected; a blank or zero bound is
   * reported as absent rather than as a bound of nothing.
   */
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

  /**
   * Normalizes `ka10004` into the ten levels each side.
   *
   * Kiwoom names the first level differently from the rest — `sel_fpr_bid` and `buy_fpr_bid` for
   * level one, `sel_2th_pre_bid` through `sel_10th_pre_bid` for the others — so the field names are
   * built rather than listed, and level one is spelled out.
   */
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

  /** Appends one book level, skipping the empty ones Kiwoom pads a thin book with. */
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

  /** Returns a decimal only when it is strictly positive, which is how Kiwoom spells "no value". */
  static std::optional<Decimal64> positive(std::optional<Decimal64> value) {
    if (!value || value->coefficient() <= 0) {
      return std::nullopt;
    }
    return value;
  }

  /**
   * Returns an order's trigger from `key`, or nothing.
   *
   * `ka10075`'s `stop_pric` and `kt00007`'s `cond_uv` are both documented as "스톱지정가주문
   * 스톱가", and `kt00007`'s own example carries `"0000000000"` on a market order -- so an ordinary
   * order reports zero, and a positive value is recorded **whatever the row's basis**, for the
   * reason `OrderSnapshot::trigger_price_` gives. An empty, zero or unreadable value leaves the
   * trigger unreported rather than failing the listing: a trigger is one fact about one order, and
   * the row is still the order, which must stay cancellable.
   */
  static std::optional<Decimal64> reported_trigger(const json::object& row, std::string_view key) {
    try {
      return positive(detail::magnitude(row, key));
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }

  /** Builds a `PROTOCOL` failure from a parser exception. */
  static BrokerError protocol_error(const char* code, const std::exception& exception) {
    BrokerError error;
    error.category_ = ErrorCategory::PROTOCOL;
    error.broker_code_ = code;
    error.message_ = exception.what();
    return error;
  }

  /** Delivers `result` to `handler` on the strand, exactly once. */
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

  /**
   * Holds admission after one of Kiwoom's three ceilings refused a request.
   *
   * Kiwoom documents the ceilings separately — per API (`1700`), account-wide (`1701`) and per API
   * group (`1702`) — but states **no allowance and no retry-after** with any of them, so there is
   * nothing to correct the budget *to*. Admission is held for a second and the refusal is counted;
   * the configured budget is left exactly as the caller set it, for the reason the KIS adapter
   * gives: reducing a stated budget from a guess is the silent downgrade this library refuses
   * everywhere else. `KiwoomRateLimitLayer` is what tells a caller watching the refusals *which*
   * ceiling to lower.
   */
  void note_rate_limit(const BrokerError& error) {
    if (error.category_ != ErrorCategory::RATE_LIMIT) {
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

  /** Issues the recent-execution read, whose parser needs the requested count. */
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

  /**
   * Normalizes `ka10003`'s recent executions, newest first as Kiwoom sends them.
   *
   * Kiwoom reports a print's time as `HHmmss` with no date, so the KST calendar day is supplied
   * from the clock. A print is intraday by construction, so this is the day it happened on — the
   * same reasoning the KIS adapter records for the same field.
   */
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

  /** Issues the reference-data read, which takes the bare symbol rather than a tape-suffixed one. */
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

  /** Normalizes `ka10100` into reference data. */
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

  /**
   * Maps Kiwoom's `marketCode` to a normalized segment.
   *
   * `0` is 거래소 (KOSPI) and `10` is 코스닥, which `ka10099` documents as its 시장구분 values. The
   * rest of that list — K-OTC, 코넥스, ETN, ELW and the funds — has no `MarketSegment::Known`
   * member, so the raw code is preserved through `market_segment` rather than collapsed into one
   * of the two this library does know.
   */
  static MarketSegment kiwoom_market_segment(std::string_view code) {
    if (code == "0") {
      return market_segment("KOSPI");
    }
    if (code == "10") {
      return market_segment("KOSDAQ");
    }
    return market_segment(code);
  }

  /**
   * Reads a listing state from `ka10100`'s `auditInfo`.
   *
   * Kiwoom publishes 감리구분 as a Korean phrase — `정상`, `투자주의환기종목`, `관리종목` — and
   * publishes no delisting date and no `SCHEDULED` state at all. Everything it reports is a listed
   * instrument under some degree of supervision, so the honest reading is `ACTIVE` for the states
   * it names and `UNKNOWN` for a phrase this adapter has not seen: supervision is not delisting,
   * and inventing a `DELISTED` from a warning would be worse than saying nothing.
   */
  static InstrumentStatus kiwoom_status(std::string_view audit) {
    if (audit == "정상" || audit == "투자주의환기종목" || audit == "관리종목") {
      return InstrumentStatus::ACTIVE;
    }
    return InstrumentStatus::UNKNOWN;
  }

  /** What a paged listing should do after one page. */
  enum class PageStep {
    /** Kiwoom offered no further page: the listing is complete. */
    FINISH,
    /** Kiwoom offered another page and this walk may fetch it. */
    CONTINUE,
    /** Kiwoom still offered another page and this walk stopped: the listing is incomplete. */
    INCOMPLETE,
  };

  /** One bar request spanning however many Kiwoom pages it needs. */
  struct BarOperation {
    BarQuery query_;
    std::vector<Bar> descending_;
    /** The upper bound for the next page, as a KST calendar date. */
    Date date_{};
    std::string next_key_;
    /** Whether Kiwoom said another page exists, which is not the same as having its key. */
    bool more_{false};
    int pages_{0};
    BarsHandler completion_;

    BarOperation(BarQuery query, BarsHandler completion)
    : query_(std::move(query)), completion_(std::move(completion)) {}
  };

  /** Validates a bar request and starts walking Kiwoom's pages. */
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

  /** Refuses a bar request Kiwoom cannot answer, before any I/O. */
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

  /** Issues one page of a bar request. */
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

  /** Accumulates one page of bars and either continues or finishes. */
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

  /**
   * Records the page's continuation key and returns whether the walk should stop.
   *
   * **The key is recorded before the decision is taken, not after it**, and that ordering is the
   * point of this being one function. `finish_bars` reads `next_key_` to decide whether the series
   * carries a continuation cursor, and the walk stops with data still available precisely when
   * `count_` is satisfied -- which is exactly when a caller needs that cursor to page further
   * back. Assigning the key only on the path that continues left the *previous* page's key here,
   * so a satisfied request reported a stale cursor or none.
   *
   * A page that returned nothing also ends the walk. Without that, a broker answering `Y` with an
   * empty array would spend the whole page budget learning nothing.
   */
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

  /**
   * Normalizes one chart row.
   *
   * A minute row stamps `cntr_tm` as `yyyyMMddHHmmss`; every other interval stamps `dt` as
   * `yyyyMMdd`. `cur_prc` is the period's close and, like every market-data price, carries a
   * direction prefix rather than a sign.
   */
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

  /**
   * Reverses the accumulation, trims to the requested count, and returns the series.
   *
   * Pure, and separate from the completion for that reason: what a walk *produced* is the thing
   * worth checking, and checking it should not require a strand, a socket or a completion token.
   */
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

  /** Composes the series and delivers it. */
  void finish_bars(const std::shared_ptr<BarOperation>& operation) {
    deliver(std::move(operation->completion_), Result<BarSeries>(compose_bars(*operation)));
  }

  /** Returns the KST calendar date an instant falls on. */
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

  /**
   * Records the page's continuation key and says what the walk should do next.
   *
   * One function for the three account listings, because the decision is the same for all of them
   * and the consequence of getting it wrong is the same too. It is separate from the handlers so
   * that the decision can be exercised without a socket, which is what
   * `plan/README.md` means by observing the mechanism rather than a seam.
   */
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

  /**
   * Returns the failure a listing that stopped while more remained should end with.
   *
   * **A guard that bounds a walk must not also decide the walk finished.** Kiwoom says `cont-yn: Y`
   * with a `next-key` exactly when there is another page, so stopping on the page budget -- or on a
   * page that added no rows while Kiwoom still offered another -- leaves rows unread. Returning
   * what was gathered would hand a *partial* answer to code that cannot tell: a working-order
   * listing missing a resting sell understates what `RiskGuard` deducts, and partial holdings
   * understate exposure, which is the permissive direction for a position ceiling.
   *
   * Bars are the exception and do not come through here: `BarSeries` carries `next_cursor_`, so a
   * bar walk can say "there is more" in the value itself. Holdings, working orders and order
   * history have nowhere to say it, so they fail instead.
   */
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

  /** One holdings request spanning however many `kt00018` pages it needs. */
  struct HoldingsOperation {
    HoldingsSnapshot snapshot_;
    std::string next_key_;
    int pages_{0};
    bool totals_seen_{false};
    HoldingsHandler completion_;

    explicit HoldingsOperation(HoldingsHandler completion) : completion_(std::move(completion)) {}
  };

  /** Validates the configured holdings venue and starts paging. */
  void start_holdings(HoldingsHandler completion) {
    if (auto invalid = validate_holdings_venue()) {
      deliver(std::move(completion), Result<HoldingsSnapshot>(std::move(*invalid)));
      return;
    }
    auto operation = std::make_shared<HoldingsOperation>(std::move(completion));
    request_holdings(operation);
  }

  /**
   * Refuses a holdings venue `kt00018` cannot express.
   *
   * `kt00018`'s `dmst_stex_tp` documents `KRX` and `NXT` and no all-venue value, so there is
   * nothing for `COMBINED` or `UNSPECIFIED` to be sent as. Resolving either to one of the two
   * would answer a different question from the one asked and would do it silently, which is what
   * Decision 6 forbids — and holdings are what the sellable guard and every position ceiling are
   * measured against. See `KiwoomConfig::holdings_venue_` and
   * `issues/kiwoom-api-holdings-venue-scope.md`.
   */
  /**
   * Refuses an instruction that can open or enlarge a position while the holdings scope is unstated.
   *
   * Nothing once the operator has asserted what `dmst_stex_tp` means: the assertion is what this
   * refusal exists to demand, so honouring it is the whole point rather than a weakening. It does
   * **not** promise a holdings read has succeeded -- see `KiwoomConfig::holdings_position_scope_`
   * for what an assertion does and does not claim, and note that the runner's own portfolio gate is
   * a separate rule that still applies.
   */
  /**
   * Returns what `trde_able_qty` on a `kt00018` row is evidence of, given what the operator asserted.
   *
   * **A named decision with one caller**, rather than three lines inside the paging branch that
   * completes a snapshot. That branch is unreachable without a live HTTP round trip, so a rule
   * written there is a rule no test can drive -- and slice 25 spent a round learning that a
   * decision worth making is worth naming (`validate_amendment`). This is the same shape.
   *
   * The coverage is the operator's assertion and nothing more. The resting treatment is this
   * library's own conservative rule either way and is deliberately not a caller's to state:
   * deducting a resting sell Kiwoom had also deducted refuses a trade, while not deducting one
   * sells the same shares twice, and only the first error is recoverable. `verified_` stays false
   * because an assertion is not a measurement.
   */
  static SellableBasis holdings_sellable_basis(KiwoomSellableScope scope) {
    return SellableBasis{
      scope == KiwoomSellableScope::WHOLE_HOLDING ? SellableBasis::Coverage::WHOLE_HOLDING
                                                 : SellableBasis::Coverage::UNESTABLISHED,
      SellableBasis::RestingTreatment::DEDUCT,
      false,
    };
  }

  static std::optional<BrokerError> holdings_scope_refusal(KiwoomPositionScope scope) {
    if (scope == KiwoomPositionScope::UNSPECIFIED) {
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
      "KiwoomConfig::holdings_venue_ to Venue::KRX or Venue::NXT deliberately, having read "
      "issues/kiwoom-api-holdings-venue-scope.md";
    return error;
  }

  /** Issues one page of the holdings inquiry. */
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

  /** Builds `kt00018`'s body: one summed listing, from the configured venue. */
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

  /** Accumulates one page of holdings and either continues or completes. */
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

  /**
   * Appends one `kt00018` page's rows and, on the first page only, its totals.
   *
   * **A symbol that appears twice fails the snapshot.** The request asks for a summed listing
   * (`qry_tp: "1"`), and the specification's own response example answers exactly that request
   * with two `A005930` rows carrying 3 and 8 shares -- so a split listing is a real possibility
   * and not a hypothetical. It is refused rather than accepted for a reason that costs money:
   * `Portfolio` reads the *first* matching row's `sellable_quantity_`, so a split holding would
   * report 3 sellable shares where Kiwoom said 11, and which 3 would depend on row order.
   * Summing them instead is not this library's call to make -- whether two lots' *sellable*
   * quantities add is a statement about what the broker meant, which is precisely what
   * `plan/todo/split-position-rows.md` exists for. So the snapshot fails loudly, once, with the
   * symbol named.
   *
   * **The totals come from Kiwoom's summary fields and are never summed from the rows**, which is
   * what `HoldingsTotals` documents: a paged listing can return part of the positions beside an
   * account-wide summary, so the two need not agree and the broker's own figure is the one to
   * report. Only the first page's summary is taken, because every page repeats it.
   */
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

  /** Returns whether `symbol` already has a row in `snapshot`. */
  static bool duplicate_symbol(const HoldingsSnapshot& snapshot, const std::string& symbol) {
    for (const auto& position : snapshot.positions_) {
      if (position.instrument_.symbol_ == symbol) {
        return true;
      }
    }
    return false;
  }

  /** Normalizes one `kt00018` position row. */
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

  /** Issues the deposit inquiry and reduces it to what the port asked for. */
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

  /** Issues `kt00001` 예수금상세현황요청. */
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

  /** Normalizes `kt00001` into Kiwoom's own deposit detail. */
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

  /** Issues `kt00010` 주문인출가능금액요청 for one instrument. */
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

  /** Normalizes `kt00010` into the per-margin-tier amounts and quantities. */
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

  /** Issues `ka00001` 계좌번호조회, which answers with the one account the credential drives. */
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

  /**
   * Normalizes `ka00001` into a listing of one.
   *
   * A listing rather than a single value, because that is the port's shape — and the shape is
   * honest here: Kiwoom answers with the account the credential drives and there is no other to
   * choose between.
   */
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

  /**
   * Returns an error when a quantity is not a whole number of shares.
   *
   * `allow_zero` is Kiwoom's all-remaining value on an amendment, which it documents as the
   * quantity `0`. A *placement* of zero shares is still refused.
   */
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

  /**
   * Returns an error when a price does not match its price basis.
   *
   * `carries_caller_price`, not `== LIMIT`: 조건부지정가 is a limit instruction until the close, and
   * treating it as unpriced is how KIS's adapter once transmitted `ORD_UNPR="0"` — an order at zero
   * rather than at the stated limit.
   */
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

  /**
   * Returns an error when a Kiwoom order names no venue.
   *
   * **Kiwoom documents `dmst_stex_tp` with no default**, so unlike KIS there is no "omitted means
   * KRX" rule this adapter could record and journal. An order that names no venue is refused before
   * any I/O rather than sent to a venue this library chose, because recovery has to be able to say
   * where an order went and a guessed venue makes that a fabrication.
   */
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

  /** The identity venue this adapter reports every Korean instrument at. */
  static constexpr Venue instrument_venue() { return Venue::COMBINED; }

  /** Returns an error when an order cannot be represented as a Kiwoom instruction. */
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

  /**
   * Returns an error when an amendment cannot be represented as a Kiwoom instruction.
   *
   * Its own function for the reason the KIS adapter gives: the decision a test needs to exercise is
   * the one the submission path takes, not one a test re-derives from the same inputs.
   */
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

  /** Builds `kt10000`/`kt10001`'s body. The side selects the endpoint, not a field. */
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

  /** Builds `kt10002`'s or `kt10003`'s body. */
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

  /** Validates and submits a placement. */
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
            [self, symbol, completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(std::move(completion), parse_order_ack(symbol, std::move(result)));
            }
          );
        }
      )
    );
  }

  /** Validates and submits a modification or a cancellation. */
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
          self->http_->async_request(
            std::move(request),
            [self, symbol, completion = std::move(completion)](
              Result<network::HttpResponse> result
            ) mutable {
              self->deliver(std::move(completion), parse_order_ack(symbol, std::move(result)));
            }
          );
        }
      )
    );
  }

  /**
   * Normalizes an order response into an acknowledgement and a reference.
   *
   * The symbol comes from the request rather than the response, because Kiwoom's order responses
   * carry `ord_no` and the venue and no `stk_cd` at all — and the reference needs the symbol to be
   * usable for a later amendment.
   */
  static Result<KiwoomOrderAck> parse_order_ack(
    const std::string& symbol,
    Result<network::HttpResponse> result
  ) {
    auto body = document(std::move(result));
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

  /** One amendable-order listing spanning however many `ka10075` pages it needs. */
  struct OpenOrderOperation {
    KiwoomOpenOrderPage page_;
    std::string next_key_;
    int pages_{0};
    OpenOrderHandler completion_;

    explicit OpenOrderOperation(OpenOrderHandler completion) : completion_(std::move(completion)) {}
  };

  /** Starts the amendable-order listing. */
  void start_open_orders(OpenOrderHandler completion) {
    auto operation = std::make_shared<OpenOrderOperation>(std::move(completion));
    request_open_orders(operation);
  }

  /** Issues one page of `ka10075` 미체결요청. */
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

  /** Accumulates one page of amendable orders and either continues or completes. */
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

  /**
   * Returns the side a row states, and throws when it states none this adapter can read.
   *
   * Throwing fails the listing, which is deliberate and is the one place this adapter does *not*
   * tolerate an unrecognized value. Everywhere else an unknown wire value becomes `UNKNOWN` with
   * the raw text preserved, because both specifications require a client to tolerate one and a
   * single odd row must not cost a whole listing. `OrderSide` has no `UNKNOWN`: the choice is
   * between failing and asserting a side, and asserting the wrong one puts a resting sell into the
   * picture as a buy, where `RiskGuard` will not deduct it from what may be sold. A listing that
   * fails is a poll that fails, which the runner already treats as a reason to hold.
   */
  static OrderSide read_side(const std::string& label) {
    if (const auto side = normalized_order_side(label)) {
      return *side;
    }
    throw std::runtime_error(
      "order row states a side this adapter cannot read: '" + label +
      "'; it names neither 매수 nor 매도"
    );
  }

  /** Normalizes one `ka10075` unfilled row. */
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
    // `tm` is `HHmmss` with no date. The calendar day is today in Seoul, which is sound *for this
    // endpoint*: `ka10075` 미체결 lists the orders still unfilled, and a KRX day order does not
    // outlive its session, so an unfilled order was placed today. The history path takes its date
    // from the request instead, because there the row can be any day's.
    order.ordered_at_ = detail::seoul_timestamp(today_in_seoul(), detail::text_field(row, "tm"));
    order.execution_.filled_quantity_ = detail::required_magnitude(row, "cntr_qty");
    resolve_open_status(open_order.amendable_quantity_, order);
    return open_order;
  }

  /**
   * Refuses to report an order closed on a row that says quantity is still outstanding.
   *
   * ## The contradiction
   *
   * `normalized_order_status` is shared with the history parser, so it maps `체결` to `FILLED` and
   * `취소` to `CANCELED` -- correct there, and on *this* endpoint those words would contradict the
   * row they arrive on. `ka10075` is the 미체결 listing, its array is `oso`, and `oso_qty` is the
   * quantity still outstanding. The specification documents `ord_stt` here with the single value
   * `접수`, so nothing establishes that Kiwoom ever sends the other two on this inquiry.
   *
   * **This is therefore a defensive rule, not a repair of an observed incident.** What is
   * established is only that if such a row arrived, the adapter would mishandle it, and that the
   * response carries the field needed to refuse to.
   *
   * ## Why a wrong answer here is not cosmetic
   *
   * `strategy::order_is_open` is false for `FILLED` and `CANCELED`, and
   * `Portfolio::working_quantity` skips any row that is not open. A resting order reported closed
   * would read as **zero** resting quantity: no sellable deduction, no working-order component in
   * the position ceiling, and a reconciliation that plans a second order beside the one already
   * out.
   *
   * ## The rule
   *
   * A word and a number disagree, and only one of them can be checked, so the number wins:
   * a positive `oso_qty` means the order is still open whatever `ord_stt` says. Which open status
   * follows from the filled quantity, the same way KIS's parser answers the same question --
   * `PARTIAL_FILLED` where something has filled, `PENDING` where nothing has. A row reporting
   * nothing outstanding keeps whatever the broker said, so this narrows a contradiction rather than
   * refusing to believe the broker.
   *
   * Applied here rather than inside `normalized_order_status`, because the contradicting quantity
   * exists only on this endpoint and the history parser reads those words legitimately.
   *
   * The two statuses tested are the two `normalized_order_status` can produce that mean the order
   * is over, named here rather than asked of `strategy::order_is_open`: an adapter must not depend
   * on the strategy layer, and `core/` is where the two layers may meet.
   */
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

  /**
   * Writes the three normalized shape facts from Kiwoom's Korean label.
   *
   * **`OrderSnapshot::shape_code_` is deliberately left empty.** Its documentation calls it "the
   * broker's own order-shape code, preserved verbatim", and Kiwoom does not send a code on a read
   * path at all — it sends a Korean display name, which `Code<8>` cannot hold. Writing the numeric
   * code this adapter *inferred* from the name would put something the broker never sent into a
   * field defined as what it did. The raw label is kept on `KiwoomOpenOrder::shape_name_` instead.
   *
   * A label the table does not know leaves all three fields `UNKNOWN` and does not fail the row,
   * because both specifications this library follows require a client to tolerate a value it does
   * not recognize, and one odd row must not fail a whole listing.
   */
  static void apply_shape(std::string_view label, OrderSnapshot& order) {
    const auto* row = shape_by_name(label);
    if (row == nullptr) {
      return;
    }
    order.type_ = row->type_;
    order.time_in_force_ = row->time_in_force_;
    order.session_ = row->session_;
  }

  /** One order-history listing spanning however many `kt00007` pages it needs. */
  struct HistoryOperation {
    KiwoomHistoryQuery query_;
    OrderPage page_;
    std::string next_key_;
    int pages_{0};
    OrderPageHandler completion_;

    HistoryOperation(KiwoomHistoryQuery query, OrderPageHandler completion)
    : query_(std::move(query)), completion_(std::move(completion)) {}
  };

  /** Starts the order-history listing for one calendar date. */
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

  /** Issues one page of `kt00007` 계좌별주문체결내역상세요청. */
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

  /** Accumulates one page of order history and either continues or completes. */
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
            parse_history_row(value.as_object(), operation->query_.date_)
          );
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

  /** Normalizes one `kt00007` row. */
  static OrderSnapshot parse_history_row(const json::object& row, const Date& date) {
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
    // `shape_by_name` searches both columns so the two listings agree about one order.
    apply_shape(detail::text_field(row, "trde_tp"), order);
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

  /**
   * Reads a history row's lifecycle state.
   *
   * `kt00007` publishes two columns and neither alone is the answer: `acpt_tp` 접수구분 says the
   * order was received, and `mdfy_cncl` 정정취소 says whether this row is a 정정확인 or a 취소확인.
   * A cancellation confirmation is a cancelled order, and nothing else on the row says so.
   *
   * A fill is read from the quantities rather than from a word, because `kt00007` publishes no
   * 체결 state at all — `ord_remnq` 주문잔량 is what remains.
   */
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
 * Blocking facade over `KiwoomClient`, driving its own event loop.
 *
 * Shares `kiwoom::Broker` with the asynchronous client, so a reference obtained through either is
 * usable with the other.
 */
class KiwoomBlockingClient {
public:
  /** Constructs a blocking client for one Kiwoom environment. */
  KiwoomBlockingClient(
    ssl::context& context,
    KiwoomConfig config,
    network::HttpClientOptions options = {}
  )
  : client_(
      std::make_shared<KiwoomClient>(ioc_.get_executor(), context, std::move(config), options)
    ) {}

  /** Returns the current normalized quote or a structured error. */
  Result<QuoteSnapshot> quote(InstrumentId instrument) {
    return run<QuoteSnapshot>(
      [&](auto&& sink) { client_->async_quote(std::move(instrument), std::move(sink)); },
      "quote"
    );
  }

  /** Returns today's upper and lower price bound. */
  Result<PriceLimits> price_limits(InstrumentId instrument) {
    return run<PriceLimits>(
      [&](auto&& sink) { client_->async_price_limits(std::move(instrument), std::move(sink)); },
      "price limits"
    );
  }

  /** Returns the resting orders on both sides of the book. */
  Result<DepthSnapshot> depth(InstrumentId instrument) {
    return run<DepthSnapshot>(
      [&](auto&& sink) { client_->async_depth(std::move(instrument), std::move(sink)); },
      "depth"
    );
  }

  /** Returns recent executions, newest first. */
  Result<std::vector<TradePrint>> trades(InstrumentId instrument, int count) {
    return run<std::vector<TradePrint>>(
      [&](auto&& sink) { client_->async_trades(std::move(instrument), count, std::move(sink)); },
      "trades"
    );
  }

  /** Returns static reference data for one instrument. */
  Result<Instrument> instrument(InstrumentId subject) {
    return run<Instrument>(
      [&](auto&& sink) { client_->async_instrument(std::move(subject), std::move(sink)); },
      "instrument"
    );
  }

  /** Returns bars, paging inside the client. */
  Result<BarSeries> bars(BarQuery query) {
    return run<BarSeries>(
      [&](auto&& sink) { client_->async_bars(std::move(query), std::move(sink)); },
      "bars"
    );
  }

  /** Returns the configuration this facade's client was built with; see `KiwoomClient::config`. */
  const KiwoomConfig& config() const noexcept { return client_->config(); }

  /** Returns every holding for the bound credential. */
  Result<HoldingsSnapshot> holdings(KiwoomAccount account) {
    return run<HoldingsSnapshot>(
      [&](auto&& sink) { client_->async_holdings(account, std::move(sink)); },
      "holdings"
    );
  }

  /** Returns what the bound credential's account may spend. */
  Result<BuyingPower> buying_power(Currency currency, KiwoomAccount account) {
    return run<BuyingPower>(
      [&](auto&& sink) { client_->async_buying_power(currency, account, std::move(sink)); },
      "buying power"
    );
  }

  /** Returns Kiwoom's own fuller deposit detail. */
  Result<KiwoomDeposit> deposit() {
    return run<KiwoomDeposit>(
      [&](auto&& sink) { client_->async_deposit(std::move(sink)); },
      "deposit"
    );
  }

  /** Returns what may be ordered of one instrument, per margin tier. */
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

  /** Returns the one account the bound credential drives. */
  Result<std::vector<KiwoomAccountInfo>> accounts() {
    return run<std::vector<KiwoomAccountInfo>>(
      [&](auto&& sink) { client_->async_accounts(std::move(sink)); },
      "accounts"
    );
  }

  /** Submits a new order. */
  Result<KiwoomOrderAck> place_order(KiwoomAccount account, OrderRequest order) {
    return run<KiwoomOrderAck>(
      [&](auto&& sink) {
        client_->async_place_order(account, std::move(order), std::move(sink));
      },
      "place order"
    );
  }

  /** Modifies an existing order. */
  Result<KiwoomOrderAck> replace_order(KiwoomAccount account, KiwoomAmendment amendment) {
    return run<KiwoomOrderAck>(
      [&](auto&& sink) {
        client_->async_replace_order(account, std::move(amendment), std::move(sink));
      },
      "replace order"
    );
  }

  /** Cancels an existing order. */
  Result<KiwoomOrderAck> cancel_order(KiwoomAccount account, KiwoomAmendment amendment) {
    return run<KiwoomOrderAck>(
      [&](auto&& sink) {
        client_->async_cancel_order(account, std::move(amendment), std::move(sink));
      },
      "cancel order"
    );
  }

  /** Returns every order still open to amendment. */
  Result<KiwoomOpenOrderPage> open_orders(KiwoomAccount account) {
    return run<KiwoomOpenOrderPage>(
      [&](auto&& sink) { client_->async_open_orders(account, std::move(sink)); },
      "open orders"
    );
  }

  /** Returns one day of order history. */
  Result<OrderPage> order_history(KiwoomAccount account, KiwoomHistoryQuery query) {
    return run<OrderPage>(
      [&](auto&& sink) {
        client_->async_order_history(account, std::move(query), std::move(sink));
      },
      "order history"
    );
  }

  /** Cancels transport work and queued operations. */
  void close() { client_->close(); }

  /** Returns this client's pacing statistics, or null when it is unpaced. */
  const network::RateLimitStats* rate_limit_stats() const { return client_->rate_limit_stats(); }

private:
  /** Drives one asynchronous operation to completion on the private loop. */
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
