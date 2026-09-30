#pragma once

#include "./network.hpp"

#include "../../core/types.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace algotradelib {
namespace kiwoom {

// https://openapi.kiwoom.com/guide/apiguide
// Token issue:  au10001  POST https://api.kiwoom.com/oauth2/token
// Token revoke: au10002  POST https://api.kiwoom.com/oauth2/revoke
struct KiwoomConfig;
struct KiwoomRest;

/**
 * Which venue's book a Kiwoom domestic market-data call reads.
 *
 * Kiwoom-private, for the reason `kis::MarketTape` is KIS-private: a tape is Korean market
 * structure -- two venues trading one listing, plus the consolidated feed over them -- and
 * broker-neutral `core/` has nothing to normalize it to. Whether the two adapters should share one
 * such enum is an open question for the operator, recorded at
 * `issues/kiwoom-api-shared-tape-vocabulary.md`; they are declared separately today because an
 * adapter owns its vendor vocabulary and because the two encode the same choice differently.
 *
 * **Kiwoom encodes the tape as a suffix on the symbol**, not as a request parameter:
 * `005930` is KRX, `005930_NX` is NXT, `005930_AL` is SOR, the consolidated book. See
 * `KiwoomConfig::market_tape_`.
 */
enum class MarketTape {
  /** The consolidated SOR book over KRX and NXT (`_AL`). */
  CONSOLIDATED,
  /** KRX alone, which is the unsuffixed symbol. */
  KRX,
  /** NXT alone (`_NX`). */
  NXT,
};

/**
 * Returns the suffix Kiwoom spells `tape` with, appended to a six-digit symbol.
 *
 * KRX is the unsuffixed form, so this returns an empty string for it rather than a code.
 */
inline constexpr const char* tape_suffix(MarketTape tape) noexcept {
  switch (tape) {
    case MarketTape::KRX:
      return "";
    case MarketTape::NXT:
      return "_NX";
    case MarketTape::CONSOLIDATED:
      return "_AL";
  }
  return "_AL";
}

/**
 * Reads Kiwoom's `yyyyMMddHHmmss` stamp, in Korean local time, as an absolute instant.
 *
 * Returns the epoch default when the text is not fourteen digits, which is what
 * `network::AuthBody::from_json` turns into a refusal rather than into a token with an invented
 * lifetime.
 */
inline std::chrono::system_clock::time_point parse_compact_expiry(std::string_view text) {
  if (text.size() != 14) {
    return {};
  }
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return {};
    }
  }
  const auto number = [text](std::size_t offset, std::size_t length) {
    int value = 0;
    for (std::size_t index = offset; index < offset + length; ++index) {
      value = value * 10 + (text[index] - '0');
    }
    return value;
  };
  std::tm parts{};
  parts.tm_year = number(0, 4) - 1900;
  parts.tm_mon = number(4, 2) - 1;
  parts.tm_mday = number(6, 2);
  parts.tm_hour = number(8, 2);
  parts.tm_min = number(10, 2);
  parts.tm_sec = number(12, 2);
  parts.tm_isdst = 0;
#ifdef _WIN32
  const std::time_t utc = _mkgmtime(&parts) - 9 * 3600;
#else
  const std::time_t utc = timegm(&parts) - 9 * 3600;
#endif
  return std::chrono::system_clock::from_time_t(utc);
}

/**
 * What a Kiwoom balance request's `dmst_stex_tp` means for the positions it lists.
 *
 * Kiwoom's specification does not say, and the two readings have opposite failure modes, so this is
 * asserted by whoever runs the account and never inferred by this library. See
 * `KiwoomConfig::holdings_position_scope_` for the argument and
 * `issues/kiwoom-api-holdings-venue-scope.md` for the experiment that settles it.
 */
enum class KiwoomPositionScope {
  /** Nobody has stated it: the holdings verb refuses, as does anything that can open a position. */
  UNSPECIFIED,
  /**
   * Asserted: the parameter selects whose price values a position, and every position is listed.
   *
   * The more plausible reading of a 계좌평가 inquiry, because custody at a Korean broker is not held
   * per venue -- a share is a share, and KRX and NXT are two places to trade it. Plausible is not
   * established, which is exactly why this is an assertion and why nothing sets it by default.
   */
  VALUATION_ONLY,
};

/**
 * What a Kiwoom balance row's `trde_able_qty` covers.
 *
 * Independent of `KiwoomPositionScope` on purpose: nothing in the specification ties the two fields'
 * scopes together, and this is the one that can admit a sell of shares that are not there.
 */
enum class KiwoomSellableScope {
  /** Nobody has stated it: the published basis is unestablished and a checked sell refuses. */
  UNSPECIFIED,
  /** Asserted: it covers every share held of the instrument, whatever venue it trades on. */
  WHOLE_HOLDING,
};

/** Kiwoom application credentials and live/demo endpoint selection. */
struct KiwoomConfig : public network::ClientCredentials<KiwoomConfig> {
  /**
   * Whether to use Kiwoom's 모의투자 (paper-trading) endpoints. **Demo unless stated otherwise.**
   *
   * Live is the one choice that moves real money, so it is the one that has to be written down:
   * `KIWOOM_DEMO=0` in the environment, `"demo": false` in a credential file, or `is_demo_ = false` in code.
   * Anything left unstated is demo -- and live keys sent to the demo endpoints are refused at the first
   * token request, so a forgotten setting fails instead of trading.
   */
  bool is_demo_ = true;

  /**
   * Which book Kiwoom's domestic market-data calls read.
   *
   * **One setting for every market-data verb**, which is the rule `ai/market-data-tape` settled for
   * KIS after five of its seven verbs had quietly disagreed with the other two about which market
   * they described. The quote, the depth, the recent prints, the price bounds and every bar request
   * append the same suffix, so a caller's last price and their bars always describe one book.
   *
   * **Deliberately not read from `InstrumentId::venue_`.** That field is identity -- it participates
   * in equality, and a holding carries no venue -- so deriving the tape from it would make a
   * market-data setting able to invalidate an order. Routing is `OrderRequest::venue_`, and it is a
   * separate question from which book a price came off.
   *
   * Consolidated by default, which is what a Korean equity's price means when nobody says otherwise
   * and what the other two adapters already answer.
   */
  MarketTape market_tape_ = MarketTape::CONSOLIDATED;

  /**
   * Which book `kt00018` 계좌평가잔고내역요청 reports holdings from.
   *
   * **`kt00018` has no all-venue value.** Its `dmst_stex_tp` documents `KRX` and `NXT` and nothing
   * else, while `ka10075`'s `stex_tp` offers `0 : 통합` and `kt00007`'s offers `%:(전체)`. So one
   * balance request cannot be shown to cover both books, and holdings are what the sellable guard
   * and every position ceiling are measured against.
   *
   * **This adapter states one book rather than sending two requests and merging them**, and that is
   * the safe direction rather than the lazy one. If `dmst_stex_tp` selects which venue's *price*
   * values the position -- the more plausible reading of a 계좌평가 inquiry, since custody at a
   * Korean broker is not held per venue -- then merging two responses would **double every
   * position**. A doubled holding defeats a position ceiling silently and in the permissive
   * direction; an incomplete one refuses trades, which is loud. So nothing is merged.
   *
   * **It has no default, and that is the point.** `UNSPECIFIED` refuses every holdings request
   * before any I/O, naming this gap. Defaulting to `KRX` -- Kiwoom's own choice in the `kt00018`
   * request example -- would have been a silent answer to a question nobody has answered: if the
   * parameter filters positions rather than valuing them, a KRX-only snapshot omits whatever is
   * held on NXT, and an **under-reported holding is the permissive direction** for a position
   * ceiling, which then admits buys against room that does not exist.
   *
   * So a caller states the book deliberately, having read why. `COMBINED` is refused too, because
   * `kt00018` publishes no value for it.
   *
   * What the parameter actually selects needs an account to settle, and it is one request pair:
   * the experiment is written out at `issues/kiwoom-api-holdings-venue-scope.md`.
   */
  Venue holdings_venue_ = Venue::UNSPECIFIED;

  /**
   * What the operator asserts `dmst_stex_tp` *means* for the positions `kt00018` lists.
   *
   * `holdings_venue_` above names the book a request is sent for. This states what naming it does,
   * which is the thing the specification never says: `kt00018`'s `dmst_stex_tp` documents
   * `KRX:한국거래소,NXT:넥스트트레이드` and no meaning, and both quantity fields' descriptions carry
   * units and zero-padding and nothing else.
   *
   * **The library will not infer this, and the reason is recorded rather than assumed.** Sending
   * both requests and comparing them was analysed and rejected: two reads are not a snapshot, so
   * disjoint row sets are evidence of a change *between* the reads rather than of what the parameter
   * means -- read KRX holding A, sell A and buy B, read NXT, and the union is `{A, B}` when only B
   * is held. `issues/kiwoom-api-holdings-venue-scope.md` carries the full argument.
   *
   * So a caller may **assert** it, and asserting is not knowing: an assertion that is wrong
   * *undercounts* the account, which admits a buy at a ceiling already reached and refuses or
   * undersizes a sell. That is the milder of the two failure directions and it is not safe. The
   * experiment that would settle it is two requests and about a minute of an account's time.
   */
  KiwoomPositionScope holdings_position_scope_ = KiwoomPositionScope::UNSPECIFIED;

  /**
   * What the operator asserts `trde_able_qty` covers.
   *
   * **Separate from the position scope above, because the specification promises nothing about
   * either and certainly not that they agree.** `rmnd_qty` could be the whole account while
   * `trde_able_qty` is what may be traded on the named venue -- both `desc` columns say only
   * "단위: 1주, 좌측 0-padding 처리된 부호 포함 15자리 숫자". One switch covering both would assume
   * exactly the thing nobody has checked, and the sellable half is the one that admits a wrong-sized
   * sell rather than merely refusing one.
   *
   * `UNSPECIFIED` publishes an unestablished `SellableBasis`, so `RiskGuard` refuses a checked sell
   * instead of measuring it against a figure of unknown coverage. Asserting `WHOLE_HOLDING` supplies
   * the coverage; the *resting* treatment is this library's conservative `DEDUCT` either way and is
   * not a caller's to state, because over-deducting only ever refuses a trade.
   */
  KiwoomSellableScope holdings_sellable_scope_ = KiwoomSellableScope::UNSPECIFIED;

  /** Reads Kiwoom credentials and environment selection from process variables. */
  void from_env_impl() {
    const char* key = std::getenv("KIWOOM_APP_KEY");
    const char* secret = std::getenv("KIWOOM_SECRET_KEY");
    if (!key || !secret) {
      throw std::runtime_error("Add KIWOOM_APP_KEY and KIWOOM_SECRET_KEY environment variables.");
    }
    // Read before anything is assigned, so a refused load leaves this config as it was.
    const bool demo = demo_from_env();
    key_ = key;
    secret_ = secret;
    is_demo_ = demo;
  }

  /**
   * Reads credentials from a JSON file.
   *
   * Expected format: `{ "appkey": "...", "secretkey": "...", "demo": false }`, which is the
   * spelling Kiwoom's own `au10001` request body uses. Without `"demo"` the config is demo; when
   * present it must be `true` or `false`.
   */
  void from_json_impl(const std::string& filename) {
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
      throw std::runtime_error("Cannot open Kiwoom credential file: " + filename);
    }
    const std::string content(
      std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>{}
    );
    boost::system::error_code ec;
    const auto jv = json::parse(content, ec);
    if (ec || !jv.is_object()) {
      throw std::runtime_error(
        "Failed to parse Kiwoom credentials file: " + filename +
        (ec ? " — " + ec.message() : std::string(" — not a JSON object"))
      );
    }
    const auto& doc = jv.as_object();
    const json::value* key = doc.if_contains("appkey");
    const json::value* secret = doc.if_contains("secretkey");
    if (!key || !secret || !key->is_string() || !secret->is_string()) {
      throw std::runtime_error("Credential file should contain appkey and secretkey as strings");
    }
    // Everything is read and checked before anything is assigned, so a refused file leaves this
    // config -- and the endpoint it already selected -- exactly as it was.
    const bool demo = demo_from_json(doc, filename);
    key_ = key->get_string().c_str();
    secret_ = secret->get_string().c_str();
    is_demo_ = demo;
  }

  /**
   * Reads `KIWOOM_DEMO`: unset is demo, `1` is demo, `0` is live, anything else refuses.
   *
   * No trimming, no `true` or `yes`: a value that has to be interpreted is one somebody could have
   * meant the other way, and guessing wrong in either direction is worse than refusing.
   */
  static bool demo_from_env() {
    const char* demo = std::getenv("KIWOOM_DEMO");
    if (!demo || std::string_view(demo) == "1") {
      return true;
    }
    if (std::string_view(demo) == "0") {
      return false;
    }
    throw std::runtime_error(
      std::string("KIWOOM_DEMO is \"") + demo + "\"; set it to 1 for the demo endpoints or 0 to trade "
      "live, or leave it unset for demo."
    );
  }

  /** Reads a credential file's `"demo"`: absent is demo, otherwise it must be a JSON boolean. */
  static bool demo_from_json(const json::object& doc, const std::string& filename) {
    const json::value* demo = doc.if_contains("demo");
    if (!demo) {
      return true;
    }
    if (!demo->is_bool()) {
      throw std::runtime_error(
        "Kiwoom credential file " + filename + " has a \"demo\" that is not true or false; write "
        "true for the demo endpoints or false to trade live, or leave it out for demo."
      );
    }
    return demo->get_bool();
  }

  /** The JSON field Kiwoom expects the application key under. */
  static constexpr std::string_view key_field() noexcept { return "appkey"; }

  /** The JSON field Kiwoom expects the application secret under. */
  static constexpr std::string_view secret_field() noexcept { return "secretkey"; }

  /** The REST host for the selected environment. */
  const char* rest_host() const noexcept {
    return is_demo_ ? "mockapi.kiwoom.com" : "api.kiwoom.com";
  }

  /** The REST port, which is the same for both environments. */
  const char* rest_port() const noexcept { return "443"; }

  /** The WebSocket host for the selected environment. */
  const char* ws_host() const noexcept {
    return is_demo_ ? "mockapi.kiwoom.com" : "api.kiwoom.com";
  }

  /** The WebSocket port, which is the same for both environments. */
  const char* ws_port() const noexcept { return "10000"; }

  /** Returns `symbol` as this configuration's tape spells it, for a market-data request. */
  std::string tape_symbol(std::string_view symbol) const {
    return std::string(symbol) + tape_suffix(market_tape_);
  }
};

/**
 * Parsed Kiwoom REST OAuth token.
 *
 * **Kiwoom is why `network::AuthBody` names no field of its own.** Its `au10001` returns the token
 * under `token`, not `access_token`, and states the expiry as `expires_dt` -- an absolute
 * `yyyyMMddHHmmss` stamp in Korean local time -- with no `expires_in` anywhere in the response.
 */
struct KiwoomRest : public network::AuthBody<KiwoomRest> {
  /** Kiwoom's `token_type`, which the specification's example gives as `bearer`. */
  std::string token_type_;

  /** The field Kiwoom returns the bearer token under. */
  static constexpr std::string_view token_field() noexcept { return "token"; }

  /**
   * Reads Kiwoom's absolute expiry, which is the only expiry it states.
   *
   * A missing or malformed `expires_dt` leaves `expires_at_` unset, and the base then refuses the
   * response rather than treating the token as valid for a guessed day.
   */
  void from_json_impl(const json::object& doc) {
    if (const auto expiry = doc.if_contains("expires_dt"); expiry && expiry->is_string()) {
      expires_at_ = parse_compact_expiry(std::string_view(expiry->as_string()));
    }
    if (const auto type = doc.if_contains("token_type"); type && type->is_string()) {
      token_type_ = type->as_string().c_str();
    }
  }
};

} // namespace kiwoom
} // namespace algotradelib
