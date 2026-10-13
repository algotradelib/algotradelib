// Kiwoom adapter and the core OAuth change it forced.
//
// Every response body here is taken from `docs/api/kiwoom/kiwoom-rest-api-spec.json`'s own
// `responseExample` for the endpoint under test, trimmed to the fields the parser reads. Where a
// test uses a value the specification does not publish, it says so.

#include <algotradelib/kis>
#include <algotradelib/kiwoom>
#include <algotradelib/live>
#include <algotradelib/tossinvest>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace algotradelib;

namespace algotradelib::kiwoom {

/** Reaches the client's pure request builders and parsers, which are private by design. */
struct KiwoomClientTestAccess {
  static Result<QuoteSnapshot> quote(InstrumentId instrument, network::HttpResponse response) {
    return KiwoomClient::parse_quote(
      std::move(instrument), Result<network::HttpResponse>(std::move(response))
    );
  }

  static Result<PriceLimits> price_limits(network::HttpResponse response) {
    return KiwoomClient::parse_price_limits(
      InstrumentId{}, Result<network::HttpResponse>(std::move(response))
    );
  }

  static Result<DepthSnapshot> depth(InstrumentId instrument, network::HttpResponse response) {
    return KiwoomClient::parse_depth(
      std::move(instrument), Result<network::HttpResponse>(std::move(response))
    );
  }

  static Result<std::vector<TradePrint>> trades(network::HttpResponse response, int count) {
    return KiwoomClient::parse_trades(
      Result<network::HttpResponse>(std::move(response)), count
    );
  }

  static Result<Instrument> instrument(InstrumentId subject, network::HttpResponse response) {
    return KiwoomClient::parse_instrument(
      std::move(subject), Result<network::HttpResponse>(std::move(response))
    );
  }

  static Result<KiwoomDeposit> deposit(network::HttpResponse response) {
    return KiwoomClient::parse_deposit(Result<network::HttpResponse>(std::move(response)));
  }

  static Result<KiwoomOrderableAmount> orderable(network::HttpResponse response) {
    return KiwoomClient::parse_orderable_amount(
      Result<network::HttpResponse>(std::move(response))
    );
  }

  static Result<std::vector<KiwoomAccountInfo>> accounts(network::HttpResponse response) {
    return KiwoomClient::parse_accounts(Result<network::HttpResponse>(std::move(response)));
  }

  static Result<KiwoomOrderAck> order_ack(
    const std::string& symbol,
    network::HttpResponse response
  ) {
    return KiwoomClient::parse_order_ack(
      symbol, Result<network::HttpResponse>(std::move(response)), KiwoomOrderOperation::UNCLASSIFIED, false
    );
  }

  static Result<KiwoomOrderAck> order_ack(
    network::HttpResponse response,
    KiwoomOrderOperation operation,
    bool demo
  ) {
    return KiwoomClient::parse_order_ack(
      "005930", Result<network::HttpResponse>(std::move(response)), operation, demo
    );
  }

  static KiwoomOpenOrder open_order(const json::object& row) {
    return KiwoomClient::parse_open_order(row);
  }

  static OrderSnapshot history_row(const json::object& row, const Date& date) {
    return KiwoomClient::parse_history_row(row, date);
  }

  // A day's history as the completed listing returns it.
  static std::vector<OrderSnapshot> history_rows(const std::vector<std::string>& rows, const Date& date, bool demo) {
    std::vector<OrderSnapshot> orders;
    std::vector<KiwoomClient::HistoryLink> links;
    for (const auto& row : rows) {
      const auto object = json::parse(row).as_object();
      orders.push_back(KiwoomClient::parse_history_row(object, date));
      links.push_back(KiwoomClient::history_link(object, demo));
    }
    KiwoomClient::close_cancelled_originals(orders, links);
    return orders;
  }

  static void holdings_page(HoldingsSnapshot& snapshot, bool& totals_seen, const json::object& p) {
    KiwoomClient::append_holdings_page(snapshot, totals_seen, p);
  }

  /**
   * Drives the real order validator, with the holdings scope the caller wants to test under.
   *
   * The scope defaults to `UNSPECIFIED` so every test written before it existed still asks the
   * question it was written to ask -- and so the *default* is what an unqualified call exercises,
   * which is the behaviour this adapter ships with.
   */
  /** Drives the real basis decision the holdings path uses. */
  static SellableBasis holdings_sellable_basis(kiwoom::KiwoomSellableScope scope) {
    return KiwoomClient::holdings_sellable_basis(scope);
  }

  static std::optional<BrokerError> validate_order(
    const OrderRequest& order,
    std::string& code,
    KiwoomPositionScope scope = KiwoomPositionScope::UNSPECIFIED
  ) {
    return KiwoomClient::validate_order(scope, order, code);
  }

  static std::optional<BrokerError> validate_amendment(
    const KiwoomAmendment& amendment,
    bool cancel,
    KiwoomPositionScope scope = KiwoomPositionScope::UNSPECIFIED
  ) {
    return KiwoomClient::validate_amendment(scope, amendment, cancel);
  }

  /** Drives the real paging decision the three account listings share. */
  static int page_step(
    std::string& key,
    int& pages,
    bool grew,
    std::optional<std::string> next_key
  ) {
    return static_cast<int>(
      KiwoomClient::advance_page(key, pages, grew, std::move(next_key))
    );
  }

  static BrokerError incomplete(std::string_view what, int pages) {
    return KiwoomClient::incomplete_listing(what, pages);
  }

  static constexpr int kFinish = static_cast<int>(KiwoomClient::PageStep::FINISH);
  static constexpr int kContinue = static_cast<int>(KiwoomClient::PageStep::CONTINUE);
  static constexpr int kIncomplete = static_cast<int>(KiwoomClient::PageStep::INCOMPLETE);

  static std::optional<std::string> continuation(const network::HttpResponse& response) {
    return KiwoomClient::continuation_key(response);
  }

  /** Builds a placement request through the same function the submission path uses. */
  static network::HttpRequest place_request(
    const KiwoomClient& client,
    const OrderRequest& order,
    std::string_view division
  ) {
    return KiwoomClient::place_order_request(client, order, division);
  }

  /** Builds an amendment request through the same function the submission path uses. */
  static network::HttpRequest amend_request(
    const KiwoomClient& client,
    const KiwoomAmendment& amendment,
    bool cancel
  ) {
    return KiwoomClient::amend_order_request(client, amendment, cancel);
  }

  static std::string market_body(const KiwoomClient& client, const InstrumentId& instrument) {
    return client.symbol_body(instrument);
  }

  static std::string holdings_body(const KiwoomClient& client) { return client.holdings_body(); }

  static std::optional<BrokerError> holdings_venue(const KiwoomClient& client) {
    return client.validate_holdings_venue();
  }

  /**
   * Runs one step of the real bar walk: accumulate a page, record its continuation, decide, finish.
   *
   * Neither function needed a seam to be reachable: `advance_bars` records the continuation and
   * decides, `compose_bars` produces the series, and the walk is the two of them plus a request.
   *
   * `pages` is a list of (bars this page added, continuation key the response carried). The walk
   * stops when `advance_bars` says so, and `compose_bars` produces the series -- both the functions
   * the asynchronous walk itself calls. Only the HTTP round trip is left out.
   */
  static Result<BarSeries> walk(
    BarQuery query,
    const std::vector<std::pair<int, std::optional<std::string>>>& pages
  ) {
    KiwoomClient::BarOperation operation(std::move(query), KiwoomClient::BarsHandler());
    for (const auto& page : pages) {
      const std::size_t before = operation.descending_.size();
      for (int index = 0; index < page.first; ++index) {
        Bar bar;
        // One bar per day, descending, as Kiwoom sends them.
        bar.timestamp_ = std::chrono::system_clock::from_time_t(
          1774483200 - 86400 * static_cast<std::int64_t>(operation.descending_.size())
        );
        bar.close_ = Decimal64(70000, 0);
        operation.descending_.push_back(bar);
      }
      const auto step = KiwoomClient::advance_bars(operation, page.second, before);
      if (step == KiwoomClient::PageStep::INCOMPLETE) {
        // The same failure the asynchronous walk delivers.
        return Result<BarSeries>(KiwoomClient::incomplete_listing("bars", operation.pages_));
      }
      if (step == KiwoomClient::PageStep::FINISH) {
        break;
      }
    }
    return Result<BarSeries>(KiwoomClient::compose_bars(operation));
  }
};

} // namespace algotradelib::kiwoom

namespace {

/** Returns the instrument used throughout these tests. */
InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

/** Wraps a JSON body as a successful HTTP response. */
network::HttpResponse ok_body(std::string body) {
  network::HttpResponse response{http::status::ok, 11};
  response.body() = std::move(body);
  return response;
}

/** Parses a request body so a field can be asserted without matching serialization. */
json::object body_of(const network::HttpRequest& request) {
  return json::parse(request.body_).as_object();
}

/** Builds a client against a throwaway loop; none of these tests performs I/O. */
struct ClientFixture {
  net::io_context ioc_;
  ssl::context tls_{ssl::context::tls_client};
  std::shared_ptr<kiwoom::KiwoomClient> client_;

  explicit ClientFixture(kiwoom::KiwoomConfig config = {})
  : client_(
      std::make_shared<kiwoom::KiwoomClient>(ioc_.get_executor(), tls_, std::move(config))
    ) {}
};

} // namespace

// ---------------------------------------------------------------------------------------------
// The core change: network::AuthBody
// ---------------------------------------------------------------------------------------------

TEST_CASE("a token response is read under the field its own broker names", "[core][oauth]") {
  // Kiwoom's au10001 response example, verbatim apart from the truncated token.
  kiwoom::KiwoomRest kiwoom_token;
  kiwoom_token.from_json(
    json::parse(
      R"({"expires_dt":"20991107083713","token_type":"bearer","token":"WQJCwyqInphKnR3bSRtB9NE1lv","return_code":0})"
    ).as_object()
  );
  CHECK(kiwoom_token.access_token_ == "WQJCwyqInphKnR3bSRtB9NE1lv");
  CHECK(kiwoom_token.token_type_ == "bearer");
  CHECK(kiwoom_token.valid());

  // The same body read by a broker that spells the field `access_token` finds nothing, which is
  // the whole reason the field is declared per broker rather than defaulted.
  tossinvest::TossRest toss_token;
  CHECK_THROWS(
    toss_token.from_json(json::parse(R"({"token":"x","expires_in":86400})").as_object())
  );

  toss_token.from_json(json::parse(R"({"access_token":"x","expires_in":86400})").as_object());
  CHECK(toss_token.access_token_ == "x");
  CHECK(toss_token.valid());

  kis::KisRest kis_token;
  kis_token.from_json(
    json::parse(
      R"({"access_token":"y","expires_in":86400,"access_token_token_expired":"2099-08-30 08:10:10"})"
    ).as_object()
  );
  CHECK(kis_token.access_token_ == "y");
  CHECK(kis_token.valid());
}

TEST_CASE("a token whose lifetime nobody stated is refused, not given one", "[core][oauth]") {
  // This is the behaviour change. The base used to supply `now + 24h` for an absent `expires_in`,
  // which is a guessed expiry presented as a stated one -- and Kiwoom sends no `expires_in` at
  // all, so every Kiwoom token would have inherited the guess regardless of `expires_dt`.
  tossinvest::TossRest silent;
  CHECK_THROWS(silent.from_json(json::parse(R"({"access_token":"x"})").as_object()));
  CHECK_FALSE(silent.valid());

  // A malformed relative expiry reaches the same refusal rather than throwing out of `stoll` with
  // a message naming neither the broker nor the field.
  tossinvest::TossRest malformed;
  CHECK_THROWS(
    malformed.from_json(json::parse(R"({"access_token":"x","expires_in":"soon"})").as_object())
  );
  CHECK_FALSE(malformed.valid());

  // Kiwoom's own absolute expiry, malformed the same way.
  kiwoom::KiwoomRest bad_date;
  CHECK_THROWS(
    bad_date.from_json(json::parse(R"({"token":"x","expires_dt":"2099-11-07"})").as_object())
  );
  CHECK_FALSE(bad_date.valid());
}

// ---------------------------------------------------------------------------------------------
// The order-shape table and its three vocabularies
// ---------------------------------------------------------------------------------------------

TEST_CASE("every requestable shape maps to the code Kiwoom publishes", "[kiwoom][orders]") {
  for (const auto& row : kiwoom::kOrderShapes) {
    std::string code;
    const auto refusal =
      kiwoom::order_division(row.type_, row.time_in_force_, row.session_, code);
    if (row.requestable_) {
      INFO("row " << row.name_);
      REQUIRE_FALSE(refusal);
      CHECK(code == row.code_);
    } else {
      INFO("read-only row " << row.name_);
      REQUIRE(refusal);
      CHECK(refusal->category_ == ErrorCategory::INVALID_ARGUMENT);
      CHECK_FALSE(refusal->reached_broker_);
      CHECK(refusal->broker_code_ == "unsupported_order");
    }
  }
}

TEST_CASE("an unrecognized label leaves the shape unknown and preserves it", "[kiwoom][orders]") {
  CHECK(kiwoom::shape_by_name("장중대량") == nullptr);

  auto row = json::parse(
    R"({"ord_no":"0000069","stk_cd":"005930","ord_stt":"접수","stk_nm":"삼성전자",
        "ord_qty":"1","ord_pric":"0","oso_qty":"1","orig_ord_no":"0000000",
        "io_tp_nm":"+매수","trde_tp":"장중대량","tm":"154113","cntr_qty":"0","stex_tp":"1"})"
  ).as_object();
  const auto open_order = kiwoom::KiwoomClientTestAccess::open_order(row);

  CHECK(open_order.order_.type_ == OrderType::UNKNOWN);
  CHECK(open_order.order_.time_in_force_ == TimeInForce::UNKNOWN);
  CHECK(open_order.order_.session_ == OrderSession::UNKNOWN);
  // The raw label is kept where a caller can still read it.
  CHECK(open_order.shape_name_ == "장중대량");
  // And the rest of the row still parsed: one odd shape must not cost the listing.
  CHECK(open_order.order_.order_id_ == "0000069");
  CHECK(open_order.order_.side_ == OrderSide::BUY);
}

TEST_CASE("a working-order row flagged sor_yn Y reads as SOR whatever stex_tp says", "[kiwoom][orders][sor]") {
  // ka10075 documents stex_tp (0 : 통합, 1 : KRX, 2 : NXT) and sor_yn (Y,N) separately, so a SOR
  // order may be listed under the book it rests on with the flag set.
  CHECK(kiwoom::listed_venue("1", "Y") == Venue::COMBINED);
  CHECK(kiwoom::listed_venue("2", "Y") == Venue::COMBINED);
  CHECK(kiwoom::listed_venue("1", "N") == Venue::KRX);
  CHECK(kiwoom::listed_venue("2", "") == Venue::NXT);
  CHECK(kiwoom::listed_venue("0", "N") == Venue::COMBINED);
  // An unreadable flag is not an error; stex_tp answers.
  CHECK(kiwoom::listed_venue("1", "y") == Venue::KRX);
  CHECK(kiwoom::listed_venue("", "") == Venue::UNSPECIFIED);

  auto row = json::parse(
    R"({"ord_no":"0000071","stk_cd":"005930","ord_stt":"접수","stk_nm":"삼성전자",
        "ord_qty":"1","ord_pric":"70000","oso_qty":"1","orig_ord_no":"0000000",
        "io_tp_nm":"+매수","trde_tp":"보통","tm":"154113","cntr_qty":"0",
        "stex_tp":"1","stex_tp_txt":"KRX","sor_yn":"Y"})"
  ).as_object();
  const auto sor = kiwoom::KiwoomClientTestAccess::open_order(row);
  CHECK(sor.order_.venue_ == Venue::COMBINED);

  row["sor_yn"] = "N";
  CHECK(kiwoom::KiwoomClientTestAccess::open_order(row).order_.venue_ == Venue::KRX);
  row.erase("sor_yn");
  CHECK(kiwoom::KiwoomClientTestAccess::open_order(row).order_.venue_ == Venue::KRX);

  // What the flag is for: a runner bound with Venue::COMBINED journals COMBINED, and recovery must
  // recognise the listed row as that order rather than as a different one resting at KRX.
  runner::OpenInstruction instruction;
  instruction.entry_.venue_ = Venue::COMBINED;
  CHECK(runner::route_evidence(instruction, sor.order_) == runner::RouteEvidence::CONFIRMS);
  row["sor_yn"] = "N";
  CHECK(
    runner::route_evidence(instruction, kiwoom::KiwoomClientTestAccess::open_order(row).order_) ==
    runner::RouteEvidence::CONTRADICTS
  );
}

TEST_CASE("a combination Kiwoom has no code for is refused before any I/O", "[kiwoom][orders]") {
  std::string code;
  // Kiwoom publishes no market order in the closing auction.
  const auto refusal = kiwoom::order_division(
    OrderType::MARKET, TimeInForce::DAY, OrderSession::CLOSING_AUCTION, code
  );
  REQUIRE(refusal);
  CHECK(refusal->category_ == ErrorCategory::INVALID_ARGUMENT);
  CHECK_FALSE(refusal->reached_broker_);
  CHECK(code.empty());
}

// ---------------------------------------------------------------------------------------------
// return_code, on an HTTP 200
// ---------------------------------------------------------------------------------------------

TEST_CASE("every documented return_code lands in the right category", "[kiwoom][errors]") {
  // Driven from the whole of the specification's errorCodeList, so a code nobody thought about is
  // a failure here rather than a silent protocol error at run time.
  struct Row {
    const char* code_;
    ErrorCategory category_;
  };
  static constexpr Row kCodes[] = {
    {"1501", ErrorCategory::INVALID_ARGUMENT}, {"1504", ErrorCategory::INVALID_ARGUMENT},
    {"1505", ErrorCategory::INVALID_ARGUMENT}, {"1511", ErrorCategory::INVALID_ARGUMENT},
    {"1512", ErrorCategory::AUTHENTICATION},   {"1513", ErrorCategory::AUTHENTICATION},
    {"1514", ErrorCategory::AUTHENTICATION},   {"1515", ErrorCategory::AUTHENTICATION},
    {"1516", ErrorCategory::AUTHENTICATION},   {"1517", ErrorCategory::INVALID_ARGUMENT},
    {"1687", ErrorCategory::RATE_LIMIT},       {"1700", ErrorCategory::RATE_LIMIT},
    {"1701", ErrorCategory::RATE_LIMIT},       {"1702", ErrorCategory::RATE_LIMIT},
    {"1901", ErrorCategory::INVALID_ARGUMENT}, {"1902", ErrorCategory::INVALID_ARGUMENT},
    {"1903", ErrorCategory::INVALID_ARGUMENT}, {"1999", ErrorCategory::PROTOCOL},
    {"8001", ErrorCategory::AUTHENTICATION},   {"8002", ErrorCategory::AUTHENTICATION},
    {"8003", ErrorCategory::AUTHENTICATION},   {"8005", ErrorCategory::AUTHENTICATION},
    {"8006", ErrorCategory::AUTHENTICATION},   {"8009", ErrorCategory::AUTHENTICATION},
    {"8010", ErrorCategory::AUTHENTICATION},   {"8011", ErrorCategory::AUTHENTICATION},
    {"8012", ErrorCategory::AUTHENTICATION},   {"8015", ErrorCategory::AUTHENTICATION},
    {"8016", ErrorCategory::AUTHENTICATION},   {"8020", ErrorCategory::AUTHENTICATION},
    {"8030", ErrorCategory::AUTHENTICATION},   {"8031", ErrorCategory::AUTHENTICATION},
    {"8040", ErrorCategory::AUTHENTICATION},   {"8050", ErrorCategory::AUTHENTICATION},
    {"8103", ErrorCategory::AUTHENTICATION},   {"8104", ErrorCategory::INVALID_ARGUMENT},
    {"8200", ErrorCategory::INVALID_ARGUMENT},
  };
  // The specification's list has thirty-seven entries and every one is named above.
  static_assert(sizeof(kCodes) / sizeof(kCodes[0]) == 37, "errorCodeList has 37 entries");

  for (const auto& row : kCodes) {
    INFO("return_code " << row.code_);
    CHECK(kiwoom::kiwoom_error_category(row.code_) == row.category_);
  }
  // An undocumented code is a protocol failure rather than anything more specific.
  CHECK(kiwoom::kiwoom_error_category("9999") == ErrorCategory::PROTOCOL);
}

TEST_CASE("a refused token on an ordinary endpoint is an authentication failure", "[kiwoom][errors]") {
  // This is the case that matters beyond classification tidiness. 8005 and 8010 arrive on an
  // ordinary endpoint with HTTP 200; read as a protocol failure, the client's refresh path -- which
  // keys on AUTHENTICATION -- would never run and every later request would fail identically.
  auto expired = kiwoom::KiwoomClientTestAccess::quote(
    samsung(), ok_body(R"({"return_code":8005,"return_msg":"Token이 유효하지 않습니다"})")
  );
  REQUIRE_FALSE(expired);
  CHECK(expired.error().category_ == ErrorCategory::AUTHENTICATION);
  CHECK(expired.error().broker_code_ == "8005");
  CHECK(expired.error().reached_broker_);

  auto throttled = kiwoom::KiwoomClientTestAccess::quote(
    samsung(), ok_body(R"({"return_code":1700,"return_msg":"허용된 API 요청 개수를 초과"})")
  );
  REQUIRE_FALSE(throttled);
  CHECK(throttled.error().category_ == ErrorCategory::RATE_LIMIT);
  CHECK(kiwoom::kiwoom_rate_limit_layer(throttled.error().broker_code_) ==
        kiwoom::KiwoomRateLimitLayer::ENDPOINT);

  // A demo key calling an endpoint 모의투자 does not serve is availability, not authentication.
  auto unavailable = kiwoom::KiwoomClientTestAccess::quote(
    samsung(), ok_body(R"({"return_code":8104,"return_msg":"모의투자에서 지원하지 않는 API"})")
  );
  REQUIRE_FALSE(unavailable);
  CHECK(unavailable.error().category_ == ErrorCategory::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------------------------
// Reading Kiwoom's numbers
// ---------------------------------------------------------------------------------------------

TEST_CASE("a market-data sign is a direction, not a minus", "[kiwoom][parsing]") {
  // ka10007 documents these fields as "단위: 원, 부호가 포함된 숫자". `-91200` is a price of
  // 91,200 won that fell; read as a negative number it is wrong by a factor of minus one, and it
  // is a perfectly valid Decimal64 that passes every check downstream.
  auto quote = kiwoom::KiwoomClientTestAccess::quote(
    samsung(),
    ok_body(
      R"({"return_code":0,"cur_prc":"-91200","trde_qty":"8735","date":"20260325","tm":"093000",
          "sel_1bid":"+91300","sel_1bid_req":"120","buy_1bid":"-91200","buy_1bid_req":"340"})"
    )
  );
  REQUIRE(quote);
  CHECK(quote.value().last_price_ == Decimal64(91200, 0));
  CHECK(quote.value().volume_ == Decimal64(8735, 0));
  REQUIRE(quote.value().top_of_book_);
  CHECK(quote.value().top_of_book_->ask_price_ == Decimal64(91300, 0));
  CHECK(quote.value().top_of_book_->bid_price_ == Decimal64(91200, 0));
  REQUIRE(quote.value().exchange_time_);
}

// ---------------------------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------------------------

TEST_CASE("the market-data tape is one setting for every verb", "[kiwoom][requests]") {
  kiwoom::KiwoomConfig consolidated;
  ClientFixture combined(consolidated);
  CHECK(
    json::parse(kiwoom::KiwoomClientTestAccess::market_body(*combined.client_, samsung()))
      .as_object()
      .at("stk_cd")
      .as_string() == "005930_AL"
  );

  kiwoom::KiwoomConfig nxt;
  nxt.market_tape_ = kiwoom::MarketTape::NXT;
  ClientFixture next(nxt);
  CHECK(
    json::parse(kiwoom::KiwoomClientTestAccess::market_body(*next.client_, samsung()))
      .as_object()
      .at("stk_cd")
      .as_string() == "005930_NX"
  );

  kiwoom::KiwoomConfig krx;
  krx.market_tape_ = kiwoom::MarketTape::KRX;
  ClientFixture korea(krx);
  CHECK(
    json::parse(kiwoom::KiwoomClientTestAccess::market_body(*korea.client_, samsung()))
      .as_object()
      .at("stk_cd")
      .as_string() == "005930"
  );

  // An *order* carries the bare symbol whatever the tape says: the tape is a question about which
  // book a price came off, not about which instrument this is.
  OrderRequest order;
  order.instrument_ = samsung();
  order.type_ = OrderType::MARKET;
  order.venue_ = Venue::NXT;
  order.quantity_ = Decimal64(1, 0);
  const auto request = kiwoom::KiwoomClientTestAccess::place_request(*next.client_, order, "3");
  CHECK(body_of(request).at("stk_cd").as_string() == "005930");
}

TEST_CASE("a continuation is followed only when the broker offers one", "[kiwoom][requests]") {
  network::HttpResponse more{http::status::ok, 11};
  more.set("cont-yn", "Y");
  more.set("next-key", "page-2");
  const auto offered = kiwoom::KiwoomClientTestAccess::continuation(more);
  REQUIRE(offered);
  CHECK(*offered == "page-2");

  network::HttpResponse last{http::status::ok, 11};
  last.set("cont-yn", "N");
  last.set("next-key", "page-2");
  CHECK_FALSE(kiwoom::KiwoomClientTestAccess::continuation(last));

  network::HttpResponse silent{http::status::ok, 11};
  CHECK_FALSE(kiwoom::KiwoomClientTestAccess::continuation(silent));

  // `cont-yn: Y` with no usable key is another page this walk cannot fetch -- which is *not* the
  // same answer as no further page, and reporting it as one turned a listing the adapter could not
  // finish into one it called complete.
  network::HttpResponse keyless{http::status::ok, 11};
  keyless.set("cont-yn", "Y");
  const auto unusable = kiwoom::KiwoomClientTestAccess::continuation(keyless);
  REQUIRE(unusable);
  CHECK(unusable->empty());

  network::HttpResponse blank{http::status::ok, 11};
  blank.set("cont-yn", "Y");
  blank.set("next-key", "");
  const auto empty_key = kiwoom::KiwoomClientTestAccess::continuation(blank);
  REQUIRE(empty_key);
  CHECK(empty_key->empty());
}

// ---------------------------------------------------------------------------------------------
// Pre-flight refusals
// ---------------------------------------------------------------------------------------------

TEST_CASE("an order naming no venue is refused rather than routed", "[kiwoom][orders]") {
  // KIS documents an omitted exchange as proceeding at KRX, so it can record where an unrouted
  // order went. Kiwoom documents no default, so there is nothing to record and nothing to guess.
  OrderRequest order;
  order.instrument_ = samsung();
  order.type_ = OrderType::MARKET;
  order.quantity_ = Decimal64(1, 0);
  order.venue_ = Venue::UNSPECIFIED;

  std::string code;
  const auto refusal = kiwoom::KiwoomClientTestAccess::validate_order(order, code);
  REQUIRE(refusal);
  CHECK(refusal->broker_code_ == "venue_required");
  CHECK_FALSE(refusal->reached_broker_);
  CHECK(kiwoom::kiwoom_effective_route(Venue::UNSPECIFIED) == Venue::UNSPECIFIED);
}

TEST_CASE("a Kiwoom runner's order passes the venue check once its binding states a route", "[kiwoom][orders][route]") {
  // **The real adapter, not a model of it.** Every order a strategy plans names no venue, and this
  // adapter refuses that before any I/O -- so a Kiwoom runner placed nothing at all until its binding
  // could state a route. The holdings scope is left unasserted, which the adapter checks *after* the
  // venue: nothing here can reach the network, and a refusal naming the scope instead of the venue
  // is the proof that the venue check passed.
  ssl::context tls(ssl::context::tls_client);
  kiwoom::KiwoomConfig config;
  config.holdings_venue_ = Venue::KRX;
  using Session = BlockingSession<kiwoom::KiwoomBlockingClient>;
  Session session(std::make_shared<kiwoom::KiwoomBlockingClient>(tls, config), kiwoom::KiwoomAccount{});

  strategy::PlannedOrder order;
  order.instrument_ = samsung();
  order.side_ = OrderSide::BUY;
  order.type_ = OrderType::LIMIT;
  order.quantity_ = Decimal64(1, 0);
  order.price_ = Decimal64(70000, 0);
  REQUIRE(order.venue_ == Venue::UNSPECIFIED);

  live::SessionExecutor<Session> unrouted(session);
  CHECK(unrouted.place(order) == runner::SendOutcome::REFUSED_LOCALLY);
  REQUIRE(unrouted.last_error());
  CHECK(unrouted.last_error()->broker_code_ == "venue_required");
  CHECK_FALSE(unrouted.last_error()->reached_broker_);
  // The refusal tells a runner's operator where the route is stated, not only a direct caller.
  CHECK(unrouted.last_error()->message_.find("default route") != std::string::npos);

  live::SessionExecutor<Session> routed(session, Venue::KRX);
  CHECK(routed.place(order) == runner::SendOutcome::REFUSED_LOCALLY);
  REQUIRE(routed.last_error());
  CHECK(routed.last_error()->broker_code_ == "holdings_scope_unestablished");
  // And the journal would record where it went: Kiwoom's route is what was asked for.
  CHECK(routed.effective_route(Venue::UNSPECIFIED) == Venue::KRX);

  // What that order would put on the wire.
  ClientFixture fixture;
  OrderRequest request;
  request.instrument_ = order.instrument_;
  request.side_ = order.side_;
  request.type_ = order.type_;
  request.quantity_ = order.quantity_;
  request.price_ = order.price_;
  request.venue_ = routed.requested_route(order.venue_);
  const auto body = body_of(kiwoom::KiwoomClientTestAccess::place_request(*fixture.client_, request, "0"));
  CHECK(body.at("dmst_stex_tp").as_string() == "KRX");
}

TEST_CASE("an instrument naming a routing venue is refused", "[kiwoom][orders]") {
  // Identity is `InstrumentId::venue_` and routing is `OrderRequest::venue_`. A caller who sets
  // the first means the second, and routing on it silently is how a holding stops matching its
  // own working order.
  OrderRequest order;
  order.instrument_ = InstrumentId{Country::KR, Venue::NXT, "005930"};
  order.type_ = OrderType::MARKET;
  order.venue_ = Venue::NXT;
  order.quantity_ = Decimal64(1, 0);

  std::string code;
  const auto refusal = kiwoom::KiwoomClientTestAccess::validate_order(order, code);
  REQUIRE(refusal);
  CHECK(refusal->broker_code_ == "invalid_instrument");
}

TEST_CASE("a priced basis needs its price and an unpriced one refuses it", "[kiwoom][orders]") {
  OrderRequest order;
  order.instrument_ = samsung();
  order.venue_ = Venue::KRX;
  order.quantity_ = Decimal64(1, 0);
  std::string code;

  // 조건부지정가 is a limit instruction until the close, so it carries a caller price. Treating it
  // as unpriced is how KIS once transmitted an order at zero.
  order.type_ = OrderType::CONDITIONAL_LIMIT;
  auto refusal = kiwoom::KiwoomClientTestAccess::validate_order(order, code);
  REQUIRE(refusal);
  CHECK(refusal->broker_code_ == "invalid_price");

  order.price_ = Decimal64(70000, 0);
  // The shape is now valid, so the code is chosen -- and what remains is the holdings refusal,
  // which is deliberately last so a caller hears about their own order first.
  const auto remaining = kiwoom::KiwoomClientTestAccess::validate_order(order, code);
  REQUIRE(remaining);
  CHECK(remaining->broker_code_ == "holdings_scope_unestablished");
  CHECK(code == "5");

  // A best-limit order is priced by the book, so a stated price is refused rather than dropped.
  order.type_ = OrderType::BEST_LIMIT;
  refusal = kiwoom::KiwoomClientTestAccess::validate_order(order, code);
  REQUIRE(refusal);
  CHECK(refusal->broker_code_ == "invalid_price");
}

TEST_CASE("a history query wider than a day is refused", "[kiwoom][history]") {
  using Traits = BrokerTraits<kiwoom::KiwoomClient>;
  OrderHistoryQuery query;
  query.from_ = Date{2026, 3, 23};
  query.to_ = Date{2026, 3, 25};
  auto converted = Traits::to_history_query(kiwoom::KiwoomAccount{}, query);
  REQUIRE_FALSE(converted);
  CHECK(converted.error().broker_code_ == "single_date_only");
  CHECK_FALSE(converted.error().reached_broker_);

  query.to_ = query.from_;
  query.symbol_ = "005930";
  converted = Traits::to_history_query(kiwoom::KiwoomAccount{}, query);
  REQUIRE(converted);
  CHECK(converted.value().symbol_ == "005930");
  CHECK(converted.value().date_.day_ == 23);

  query.to_.reset();
  query.limit_ = 10;
  converted = Traits::to_history_query(kiwoom::KiwoomAccount{}, query);
  REQUIRE_FALSE(converted);
  CHECK(converted.error().broker_code_ == "limit_unsupported");

  OrderHistoryQuery undated;
  converted = Traits::to_history_query(kiwoom::KiwoomAccount{}, undated);
  REQUIRE_FALSE(converted);
  CHECK(converted.error().broker_code_ == "date_required");
}

TEST_CASE("a holdings venue must be stated, and COMBINED cannot be", "[kiwoom][holdings]") {
  // kt00018 documents KRX and NXT and no combined value, so there is nothing for COMBINED to be
  // sent as -- and nothing for an unstated venue to default to either. Holdings are what the
  // sellable guard and every position ceiling measure against, and an under-reported holding is
  // the *permissive* direction for a position ceiling, so the default is a refusal.
  kiwoom::KiwoomConfig unstated;
  CHECK(unstated.holdings_venue_ == Venue::UNSPECIFIED);
  ClientFixture silent(unstated);
  auto refusal = kiwoom::KiwoomClientTestAccess::holdings_venue(*silent.client_);
  REQUIRE(refusal);
  CHECK(refusal->broker_code_ == "holdings_venue_required");
  CHECK_FALSE(refusal->reached_broker_);

  kiwoom::KiwoomConfig combined;
  combined.holdings_venue_ = Venue::COMBINED;
  ClientFixture wrong(combined);
  refusal = kiwoom::KiwoomClientTestAccess::holdings_venue(*wrong.client_);
  REQUIRE(refusal);
  CHECK(refusal->broker_code_ == "holdings_venue_required");

  kiwoom::KiwoomConfig krx;
  krx.holdings_venue_ = Venue::KRX;
  ClientFixture right(krx);
  CHECK_FALSE(kiwoom::KiwoomClientTestAccess::holdings_venue(*right.client_));
  const auto body = json::parse(
    kiwoom::KiwoomClientTestAccess::holdings_body(*right.client_)
  ).as_object();
  CHECK(body.at("dmst_stex_tp").as_string() == "KRX");
  CHECK(body.at("qry_tp").as_string() == "1");
}

TEST_CASE("a holdings listing that comes back split is refused", "[kiwoom][holdings]") {
  // The request asks for a summed listing, and the specification's own response example answers
  // exactly that request with two A005930 rows carrying 3 and 8 shares. `Portfolio` reads the
  // *first* matching row's sellable quantity, so accepting the split would report 3 sellable
  // shares where Kiwoom said 11 -- and which 3 would depend on row order.
  HoldingsSnapshot snapshot;
  bool totals_seen = false;
  CHECK_THROWS(kiwoom::KiwoomClientTestAccess::holdings_page(
    snapshot,
    totals_seen,
    json::parse(
      R"({"tot_pur_amt":"000000017598258","tot_evlt_amt":"000000025789890",
          "tot_evlt_pl":"000000008138825",
          "acnt_evlt_remn_indv_tot":[
            {"stk_cd":"A005930","stk_nm":"삼성전자","rmnd_qty":"000000000000003",
             "trde_able_qty":"000000000000003","pur_pric":"000000000124500",
             "cur_prc":"000000059000","evltv_prft":"-00000000196888","prft_rt":"-52.71",
             "pur_amt":"000000000373500","evlt_amt":"000000000177000"},
            {"stk_cd":"A005930","stk_nm":"삼성전자","rmnd_qty":"000000000000008",
             "trde_able_qty":"000000000000008","pur_pric":"000000000209178",
             "cur_prc":"000000085000","evltv_prft":"-00000000995004","prft_rt":"-59.46",
             "pur_amt":"000000001673430","evlt_amt":"000000000680000"}]})"
    ).as_object()
  ));

  // Two *different* instruments on one page are ordinary and are not refused.
  HoldingsSnapshot two;
  bool seen = false;
  CHECK_NOTHROW(kiwoom::KiwoomClientTestAccess::holdings_page(
    two,
    seen,
    json::parse(
      R"({"tot_pur_amt":"0","tot_evlt_amt":"0","tot_evlt_pl":"0",
          "acnt_evlt_remn_indv_tot":[
            {"stk_cd":"A005930","rmnd_qty":"3","trde_able_qty":"3","pur_pric":"124500",
             "cur_prc":"59000","pur_amt":"373500","evlt_amt":"177000","evltv_prft":"-196888"},
            {"stk_cd":"A000660","rmnd_qty":"8","trde_able_qty":"8","pur_pric":"209178",
             "cur_prc":"85000","pur_amt":"1673430","evlt_amt":"680000","evltv_prft":"-995004"}]})"
    ).as_object()
  ));
  CHECK(two.positions_.size() == 2);
}

// ---------------------------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------------------------

TEST_CASE("the book reads ten levels from Kiwoom's irregular field names", "[kiwoom][depth]") {
  // ka10004 names level one `sel_fpr_bid` and the rest `sel_Nth_pre_bid`, so the names are built
  // for the second onwards and spelled out for the first.
  auto depth = kiwoom::KiwoomClientTestAccess::depth(
    samsung(),
    ok_body(
      R"({"return_code":0,"bid_req_base_tm":"162000",
          "sel_fpr_bid":"+91300","sel_fpr_req":"120",
          "sel_2th_pre_bid":"+91400","sel_2th_pre_req":"200",
          "buy_fpr_bid":"-91200","buy_fpr_req":"340",
          "buy_2th_pre_bid":"-91100","buy_2th_pre_req":"410",
          "buy_3th_pre_bid":"0","buy_3th_pre_req":"0"})"
    )
  );
  REQUIRE(depth);
  const auto& book = depth.value();
  REQUIRE(book.asks_.size() == 2);
  REQUIRE(book.bids_.size() == 2);
  // `order_depth` sorts, so the best of each side is first whatever order Kiwoom sent.
  CHECK(book.asks_.front().price_ == Decimal64(91300, 0));
  CHECK(book.bids_.front().price_ == Decimal64(91200, 0));
  CHECK(book.bids_.front().quantity_ == Decimal64(340, 0));
  // An empty level is not a level at zero.
  CHECK(book.bids_.size() == 2);
}

TEST_CASE("price bounds come off the quote response", "[kiwoom][market-data]") {
  auto limits = kiwoom::KiwoomClientTestAccess::price_limits(
    ok_body(R"({"return_code":0,"upl_pric":"+118500","lst_pric":"-63900","date":"20260325","tm":"093000"})")
  );
  REQUIRE(limits);
  CHECK(limits.value().upper_ == Decimal64(118500, 0));
  CHECK(limits.value().lower_ == Decimal64(63900, 0));

  // A blank or zero bound is absent, not a bound of nothing.
  auto blank = kiwoom::KiwoomClientTestAccess::price_limits(
    ok_body(R"({"return_code":0,"upl_pric":"","lst_pric":"0"})")
  );
  REQUIRE(blank);
  CHECK_FALSE(blank.value().upper_);
  CHECK_FALSE(blank.value().lower_);
}

TEST_CASE("recent executions stop at the count asked for", "[kiwoom][market-data]") {
  auto prints = kiwoom::KiwoomClientTestAccess::trades(
    ok_body(
      R"({"return_code":0,"cntr_infr":[
           {"tm":"130429","cur_prc":"+53500","cntr_trde_qty":"1010"},
           {"tm":"130153","cur_prc":"+68900","cntr_trde_qty":"456"},
           {"tm":"125947","cur_prc":"+55000","cntr_trde_qty":"1000"}]})"
    ),
    2
  );
  REQUIRE(prints);
  REQUIRE(prints.value().size() == 2);
  CHECK(prints.value().front().price_ == Decimal64(53500, 0));
  CHECK(prints.value().front().quantity_ == Decimal64(1010, 0));
  REQUIRE(prints.value().front().exchange_time_);
}

TEST_CASE("Kiwoom reads a listing date as stated, not stated, or unreadable", "[kiwoom][reference]") {
  const auto parse = [](std::string_view reg_day) {
    return kiwoom::KiwoomClientTestAccess::instrument(
      samsung(),
      ok_body(
        std::string(R"({"return_code":0,"code":"005930","name":"x","marketCode":"0",)") +
        R"("auditInfo":"정상","listCount":"0000000026034239")" +
        (reg_day.empty() ? std::string() : std::string(R"(,"regDay":)") + std::string(reg_day)) + "}"
      )
    );
  };
  using State = StatedDate::State;

  const auto read = parse(R"("20090803")");
  REQUIRE(read);
  CHECK(read.value().listed_on_ == StatedDate::stated(Date{2009, 8, 3}));

  for (const auto* blank : {"", R"("")", "null"}) {
    const auto result = parse(blank);
    REQUIRE(result);
    CHECK(result.value().listed_on_.state() == State::NOT_STATED);
  }

  // Unlike KIS, a zero date is damage here: nothing in Kiwoom's specification uses one as a blank.
  // And a number is a date stated in a form nothing here reads, not a date withheld.
  for (const auto* damaged : {R"("00000000")", R"("2009083x")", R"("20091332")", "20090803"}) {
    const auto result = parse(damaged);
    REQUIRE(result);
    CHECK(result.value().listed_on_.state() == State::UNREADABLE);
    CHECK(result.value().currency_ == Currency::krw());
  }
  // Kiwoom publishes no delisting date at all.
  CHECK(read.value().delisted_on_.state() == State::NOT_STATED);
}

TEST_CASE("reference data normalizes Kiwoom's market code", "[kiwoom][reference]") {
  auto listed = kiwoom::KiwoomClientTestAccess::instrument(
    samsung(),
    ok_body(
      R"({"return_code":0,"code":"005930","name":"삼성전자","listCount":"0000000026034239",
          "auditInfo":"정상","regDay":"20090803","marketCode":"0","marketName":"거래소"})"
    )
  );
  REQUIRE(listed);
  CHECK(listed.value().name_ == "삼성전자");
  CHECK(listed.value().segment_.known_ == MarketSegment::Known::KOSPI);
  CHECK(listed.value().status_ == InstrumentStatus::ACTIVE);
  CHECK(listed.value().shares_outstanding_ == Decimal64(26034239, 0));
  CHECK(listed.value().listed_on_ == StatedDate::stated(Date{2009, 8, 3}));

  auto kosdaq = kiwoom::KiwoomClientTestAccess::instrument(
    samsung(), ok_body(R"({"return_code":0,"name":"x","marketCode":"10","auditInfo":"관리종목"})")
  );
  REQUIRE(kosdaq);
  CHECK(kosdaq.value().segment_.known_ == MarketSegment::Known::KOSDAQ);
  // Supervision is not delisting.
  CHECK(kosdaq.value().status_ == InstrumentStatus::ACTIVE);

  // A market this library has no member for is preserved rather than collapsed into one it does.
  auto etn = kiwoom::KiwoomClientTestAccess::instrument(
    samsung(), ok_body(R"({"return_code":0,"name":"x","marketCode":"60","auditInfo":"정상"})")
  );
  REQUIRE(etn);
  CHECK(etn.value().segment_.known_ == MarketSegment::Known::UNKNOWN);
  CHECK(etn.value().segment_.other_.str() == "60");
}

TEST_CASE("buying power is the credit-free figure, not the orderable one", "[kiwoom][account]") {
  // Kiwoom's own kt00001 example. 주문가능금액 is 85,341 against a 100%종목주문가능금액 of 12,550,
  // on an account holding 3,915,500 of 대용금 -- collateral, not cash. Reporting the larger figure
  // as spendable cash is how a strategy discovers leverage by accident.
  auto deposit = kiwoom::KiwoomClientTestAccess::deposit(
    ok_body(
      R"({"return_code":0,"entr":"000000000017534","repl_amt":"000000003915500",
          "pymn_alow_amt":"000000000085341","ord_alow_amt":"000000000085341",
          "100stk_ord_alow_amt":"000000000012550","d2_entra":"000000000012550"})"
    )
  );
  REQUIRE(deposit);
  CHECK(deposit.value().spendable_ == Decimal64(12550, 0));
  CHECK(deposit.value().orderable_ == Decimal64(85341, 0));
  CHECK(deposit.value().substitute_amount_ == Decimal64(3915500, 0));
  // The figure the port reports is the smaller one, and it agrees with D+2 예수금.
  CHECK(deposit.value().spendable_ == deposit.value().settled_deposit_);
}

TEST_CASE("Kiwoom publishes an affordable quantity per margin tier", "[kiwoom][account]") {
  // This is what `has_buying_power_quantity` claims, so it is asserted rather than assumed.
  STATIC_REQUIRE(BrokerCaps<kiwoom::KiwoomClient>::has_buying_power_quantity);
  auto orderable = kiwoom::KiwoomClientTestAccess::orderable(
    ok_body(
      R"({"return_code":0,"profa_20ord_alow_amt":"000000012550","profa_20ord_alowq":"0000000012",
          "profa_30ord_alow_amt":"000000012550","profa_30ord_alowq":"0000000008",
          "profa_40ord_alow_amt":"000000012550","profa_40ord_alowq":"0000000006",
          "profa_50ord_alow_amt":"000000012550","profa_50ord_alowq":"0000000005",
          "profa_60ord_alow_amt":"000000012550","profa_60ord_alowq":"0000000004",
          "profa_rdex_60ord_alow_amt":"000000012550","profa_rdex_60ord_alowq":"0000000004",
          "profa_100ord_alow_amt":"000000012550","profa_100ord_alowq":"0000000002",
          "ord_alowa":"000000085341","wthd_alowa":"000000085341","d2entra":"000000012550",
          "profa_rdex_aplc_tp":"0"})"
    )
  );
  REQUIRE(orderable);
  REQUIRE(orderable.value().tiers_.size() == 7);
  CHECK(orderable.value().tiers_.front().margin_percent_ == 20);
  CHECK(orderable.value().tiers_.front().quantity_ == Decimal64(12, 0));
  CHECK(orderable.value().tiers_.back().reduced_);
  CHECK(orderable.value().tiers_.back().margin_percent_ == 60);
  CHECK_FALSE(orderable.value().margin_reduction_applies_);
  CHECK(orderable.value().orderable_cash_ == Decimal64(85341, 0));
}

TEST_CASE("the account listing names the one account the credential drives", "[kiwoom][account]") {
  auto accounts = kiwoom::KiwoomClientTestAccess::accounts(
    ok_body(R"({"acctNo":"0123456789","return_code":0})")
  );
  REQUIRE(accounts);
  REQUIRE(accounts.value().size() == 1);
  CHECK(accounts.value().front().account_number_ == "0123456789");

  // Kiwoom publishes no account type, and an empty string says so rather than inventing one.
  using Traits = BrokerTraits<kiwoom::KiwoomClient>;
  CHECK(Traits::display_of(accounts.value().front()) == "0123456789");
  CHECK(Traits::type_of(accounts.value().front()).empty());
}

TEST_CASE("an order acknowledgement carries the symbol from the request", "[kiwoom][orders]") {
  // Kiwoom's order response has ord_no and no stk_cd, and the reference needs both to be usable
  // for a later amendment.
  auto placed = kiwoom::KiwoomClientTestAccess::order_ack(
    "005930", ok_body(R"({"ord_no":"00024","return_code":0,"return_msg":"정상"})")
  );
  REQUIRE(placed);
  CHECK(placed.value().order_.order_number_ == "00024");
  CHECK(placed.value().order_.symbol_ == "005930");

  const auto ref = BrokerTraits<kiwoom::KiwoomClient>::make_ref(placed.value().order_);
  CHECK(ref.primary_ == "00024");
  CHECK(ref.secondary_ == "005930");
  const auto round_trip = BrokerTraits<kiwoom::KiwoomClient>::from_ref(ref);
  CHECK(round_trip.order_number_ == placed.value().order_.order_number_);
  CHECK(round_trip.symbol_ == placed.value().order_.symbol_);

  auto amended = kiwoom::KiwoomClientTestAccess::order_ack(
    "005930",
    ok_body(R"({"ord_no":"00025","base_orig_ord_no":"00024","mdfy_qty":"3","return_code":0})")
  );
  REQUIRE(amended);
  CHECK(amended.value().base_order_number_ == "00024");
  CHECK(amended.value().amended_quantity_ == Decimal64(3, 0));
}

// The bodies below are constructed from Kiwoom's documented reply envelope with the codes and the
// messages the demo server sent (2026-10-12, 2026-10-13). No reply body was recorded.
namespace {

using kiwoom::KiwoomOrderOperation;

constexpr const char* KIWOOM_NOTHING_TO_CANCEL = "[2000](RC4033:모의투자 정정/취소할 수량이 없습니다.)";
constexpr const char* KIWOOM_NOTHING_TO_SELL = "[2000](800033:모의투자 매도가능수량이 부족합니다.)";

/** A Kiwoom failure envelope with outer code 20 and the given message, and `extra` appended inside the object. */
std::string kiwoom_refusal_body(const std::string& message, const std::string& extra = "") {
  return R"({"return_code":20,"return_msg":")" + message + "\"" + extra + "}";
}

/** A response with the given status. */
network::HttpResponse body_with_status(std::string body, int status) {
  network::HttpResponse response{static_cast<http::status>(status), 11};
  response.body() = std::move(body);
  return response;
}

/** Whether an order reply is read as the recognized refusal `code`. */
bool kiwoom_recognized(const Result<kiwoom::KiwoomOrderAck>& result, const std::string& code) {
  return !result && result.error().category_ == ErrorCategory::INVALID_ARGUMENT &&
    result.error().reached_broker_ && result.error().broker_code_ == code;
}

/** Whether an order reply keeps the unknown-outcome reading of outer code 20. */
bool kiwoom_unknown(const Result<kiwoom::KiwoomOrderAck>& result) {
  return !result && result.error().category_ == ErrorCategory::PROTOCOL &&
    runner::outcome_of(result.error()) == runner::SendOutcome::INDETERMINATE;
}

} // namespace

TEST_CASE("the code inside a Kiwoom order refusal is read from the whole message only", "[kiwoom][orders]") {
  CHECK(kiwoom::kiwoom_refusal_code(KIWOOM_NOTHING_TO_CANCEL) == "RC4033");
  CHECK(kiwoom::kiwoom_refusal_code(KIWOOM_NOTHING_TO_SELL) == "800033");
  CHECK(kiwoom::kiwoom_refusal_code("[2000](A:b)") == "A");
  CHECK(kiwoom::kiwoom_refusal_code("[2000](RC4033:a:b (c))") == "RC4033");

  for (const char* message : {
         "",
         "[2000]",
         "[2000](",
         "[2000]()",
         "[2000](:text)",
         "[2000](RC4033)",
         "[2000](RC4033:)",
         "[2000](RC4033:text",
         "[2000](RC4033:text) ",
         " [2000](RC4033:text)",
         "[2001](RC4033:text)",
         "[2000] (RC4033:text)",
         "(RC4033:text)",
         "[2000](rc4033:text)",
         "[2000](RC 4033:text)",
         "[2000](RC-4033:text)",
         "주문이 거부되었습니다 [2000](RC4033:text)",
         "RC4033",
         "모의투자 정정/취소할 수량이 없습니다. RC4033",
       }) {
    CAPTURE(message);
    CHECK(kiwoom::kiwoom_refusal_code(message).empty());
  }
}

TEST_CASE("a Kiwoom demo cash order refusal the client recognizes is a rejection", "[kiwoom][orders]") {
  SECTION("a cancellation with nothing left to cancel") {
    const auto result = kiwoom::KiwoomClientTestAccess::order_ack(
      ok_body(kiwoom_refusal_body(KIWOOM_NOTHING_TO_CANCEL)), KiwoomOrderOperation::CANCEL, true
    );
    REQUIRE_FALSE(result);
    CHECK(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
    CHECK(result.error().reached_broker_);
    CHECK(result.error().http_status_ == 200);
    CHECK(result.error().broker_code_ == "RC4033");
    CHECK(
      result.error().message_ ==
      std::string(KIWOOM_NOTHING_TO_CANCEL) +
        " (return_code 20; recognized refusal: Kiwoom demo, cash cancellation, RC4033)"
    );
    CHECK(runner::outcome_of(result.error()) == runner::SendOutcome::REJECTED);
  }

  SECTION("a placement with not enough to sell") {
    const auto result = kiwoom::KiwoomClientTestAccess::order_ack(
      ok_body(kiwoom_refusal_body(KIWOOM_NOTHING_TO_SELL)), KiwoomOrderOperation::PLACE, true
    );
    REQUIRE_FALSE(result);
    CHECK(result.error().category_ == ErrorCategory::INVALID_ARGUMENT);
    CHECK(result.error().reached_broker_);
    CHECK(result.error().http_status_ == 200);
    CHECK(result.error().broker_code_ == "800033");
    CHECK(
      result.error().message_ ==
      std::string(KIWOOM_NOTHING_TO_SELL) +
        " (return_code 20; recognized refusal: Kiwoom demo, cash placement, 800033)"
    );
    CHECK(runner::outcome_of(result.error()) == runner::SendOutcome::REJECTED);
  }

  SECTION("the outer code as the string 20, empty acknowledgement fields, or the original order echoed") {
    CHECK(kiwoom_recognized(
      kiwoom::KiwoomClientTestAccess::order_ack(
        ok_body(std::string(R"({"return_code":"20","return_msg":")") + KIWOOM_NOTHING_TO_CANCEL + "\"}"),
        KiwoomOrderOperation::CANCEL,
        true
      ),
      "RC4033"
    ));
    for (const char* extra : {
           R"(,"ord_no":"")",
           R"(,"ord_no":"","cncl_qty":"","mdfy_qty":"")",
           R"(,"cncl_qty":"000000000000")",
           R"(,"base_orig_ord_no":"0036037")",
           R"(,"dmst_stex_tp":"KRX")",
         }) {
      CAPTURE(extra);
      CHECK(kiwoom_recognized(
        kiwoom::KiwoomClientTestAccess::order_ack(
          ok_body(kiwoom_refusal_body(KIWOOM_NOTHING_TO_CANCEL, extra)), KiwoomOrderOperation::CANCEL, true
        ),
        "RC4033"
      ));
    }
  }
}

TEST_CASE("a Kiwoom order refusal outside the recognized replies stays an unknown outcome", "[kiwoom][orders]") {
  const std::string cancel = kiwoom_refusal_body(KIWOOM_NOTHING_TO_CANCEL);
  const std::string place = kiwoom_refusal_body(KIWOOM_NOTHING_TO_SELL);
  using Access = kiwoom::KiwoomClientTestAccess;

  SECTION("the real server, another operation, or no operation") {
    CHECK(kiwoom_unknown(Access::order_ack(ok_body(cancel), KiwoomOrderOperation::CANCEL, false)));
    CHECK(kiwoom_unknown(Access::order_ack(ok_body(place), KiwoomOrderOperation::PLACE, false)));
    CHECK(kiwoom_unknown(Access::order_ack(ok_body(cancel), KiwoomOrderOperation::PLACE, true)));
    CHECK(kiwoom_unknown(Access::order_ack(ok_body(cancel), KiwoomOrderOperation::AMEND, true)));
    CHECK(kiwoom_unknown(Access::order_ack(ok_body(place), KiwoomOrderOperation::CANCEL, true)));
    CHECK(kiwoom_unknown(Access::order_ack(ok_body(place), KiwoomOrderOperation::AMEND, true)));
    CHECK(kiwoom_unknown(Access::order_ack(ok_body(cancel), KiwoomOrderOperation::UNCLASSIFIED, true)));
    CHECK(kiwoom_unknown(Access::order_ack("005930", ok_body(cancel))));

    // What the unrecognized reading carries: the outer code, Kiwoom's message alone, and no status.
    const auto live = Access::order_ack(ok_body(cancel), KiwoomOrderOperation::CANCEL, false);
    CHECK(live.error().broker_code_ == "20");
    CHECK(live.error().message_ == KIWOOM_NOTHING_TO_CANCEL);
    CHECK(live.error().http_status_ == 0);
  }

  SECTION("a reply that is not an order reply never states an operation") {
    // The shared reader's default: the same body on an inquiry is the generic declared failure.
    const auto deposit = Access::deposit(ok_body(cancel));
    REQUIRE_FALSE(deposit);
    CHECK(deposit.error().category_ == ErrorCategory::PROTOCOL);
    CHECK(deposit.error().broker_code_ == "20");
  }

  SECTION("a success status other than 200") {
    CHECK(kiwoom_unknown(Access::order_ack(body_with_status(cancel, 201), KiwoomOrderOperation::CANCEL, true)));
    CHECK(kiwoom_unknown(Access::order_ack(body_with_status(cancel, 202), KiwoomOrderOperation::CANCEL, true)));
  }

  SECTION("another outer code, or one of another type") {
    for (const char* code : {"2", "21", "200", "20.0", "\"20 \"", "\" 20\"", "\"020\"", "\"2000\""}) {
      CAPTURE(code);
      const auto result = Access::order_ack(
        ok_body(std::string(R"({"return_code":)") + code + R"(,"return_msg":")" + KIWOOM_NOTHING_TO_CANCEL + "\"}"),
        KiwoomOrderOperation::CANCEL,
        true
      );
      REQUIRE_FALSE(result);
      CHECK(result.error().category_ != ErrorCategory::INVALID_ARGUMENT);
      CHECK(result.error().message_.find("recognized refusal") == std::string::npos);
    }
    // A code this client already classifies keeps its class.
    const auto limited = Access::order_ack(
      ok_body(std::string(R"({"return_code":1700,"return_msg":")") + KIWOOM_NOTHING_TO_CANCEL + "\"}"),
      KiwoomOrderOperation::CANCEL,
      true
    );
    REQUIRE_FALSE(limited);
    CHECK(limited.error().category_ == ErrorCategory::RATE_LIMIT);
  }

  SECTION("another inner code, a message of another shape, or none") {
    for (const char* message : {
           "[2000](RC4034:모의투자 정정/취소할 수량이 없습니다.)",
           "[2000](800033:모의투자 매도가능수량이 부족합니다.)",
           "[2000](RC40330:text)",
           "[2000](XRC4033:text)",
           "[2001](RC4033:text)",
           "RC4033",
           "[2000](RC4033:text) ",
           "the order was refused: [2000](RC4033:text)",
           "모의투자 정정/취소할 수량이 없습니다.",
           "",
         }) {
      CAPTURE(message);
      CHECK(kiwoom_unknown(
        Access::order_ack(ok_body(kiwoom_refusal_body(message)), KiwoomOrderOperation::CANCEL, true)
      ));
    }
    for (const char* body : {
           R"({"return_code":20})",
           R"({"return_code":20,"return_msg":null})",
           R"x({"return_code":20,"return_msg":["[2000](RC4033:text)"]})x",
         }) {
      CAPTURE(body);
      CHECK(kiwoom_unknown(Access::order_ack(ok_body(body), KiwoomOrderOperation::CANCEL, true)));
    }
  }

  SECTION("anything an acknowledgement carries") {
    for (const char* extra : {
           R"(,"ord_no":"0036038")",
           R"(,"ord_no":36038)",
           R"(,"ord_no":0)",
           R"(,"ord_no":null)",
           R"(,"ord_no":" ")",
           R"(,"cncl_qty":"1")",
           R"(,"cncl_qty":"000000000001")",
           R"(,"cncl_qty":1)",
           R"(,"cncl_qty":0)",
           R"(,"cncl_qty":"+0")",
           R"(,"mdfy_qty":"3")",
           R"(,"mdfy_qty":null)",
         }) {
      CAPTURE(extra);
      CHECK(kiwoom_unknown(
        Access::order_ack(
          ok_body(kiwoom_refusal_body(KIWOOM_NOTHING_TO_CANCEL, extra)), KiwoomOrderOperation::CANCEL, true
        )
      ));
      CHECK(kiwoom_unknown(
        Access::order_ack(
          ok_body(kiwoom_refusal_body(KIWOOM_NOTHING_TO_SELL, extra)), KiwoomOrderOperation::PLACE, true
        )
      ));
    }
  }

  SECTION("an HTTP error with the same body keeps its own reading") {
    const auto server = Access::order_ack(body_with_status(cancel, 500), KiwoomOrderOperation::CANCEL, true);
    REQUIRE_FALSE(server);
    CHECK(server.error().category_ == ErrorCategory::HTTP);
    CHECK(runner::outcome_of(server.error()) == runner::SendOutcome::INDETERMINATE);

    const auto client = Access::order_ack(body_with_status(cancel, 400), KiwoomOrderOperation::CANCEL, true);
    REQUIRE_FALSE(client);
    CHECK(client.error().category_ == ErrorCategory::HTTP);
    CHECK(client.error().http_status_ == 400);
  }

  SECTION("an accepted order is read as before") {
    const auto accepted = Access::order_ack(
      ok_body(R"x({"ord_no":"00024","return_code":0,"return_msg":"[2000](RC4033:text)"})x"),
      KiwoomOrderOperation::CANCEL,
      true
    );
    REQUIRE(accepted);
    CHECK(accepted.value().order_.order_number_ == "00024");
  }
}

TEST_CASE("a history row's state comes from two columns, not one", "[kiwoom][history]") {
  const auto date = Date{2026, 3, 25};
  const auto base = std::string(
    R"("ord_no":"0000050","stk_cd":"A005930","trde_tp":"지정가","ord_qty":"0000000010",
        "ord_uv":"0000070000","stk_nm":"삼성전자","io_tp_nm":"현금매수","dmst_stex_tp":"KRX",
        "ord_tm":"13:05:43")"
  );

  // A cancellation confirmation is a cancelled order, and nothing else on the row says so.
  const auto canceled = kiwoom::KiwoomClientTestAccess::history_row(
    json::parse("{" + base + R"(,"acpt_tp":"접수","mdfy_cncl":"취소확인","cntr_qty":"0","ord_remnq":"0")" + "}")
      .as_object(),
    date
  );
  CHECK(canceled.status_ == OrderStatus::CANCELED);

  const auto replaced = kiwoom::KiwoomClientTestAccess::history_row(
    json::parse("{" + base + R"(,"acpt_tp":"접수","mdfy_cncl":"정정확인","cntr_qty":"0","ord_remnq":"0")" + "}")
      .as_object(),
    date
  );
  CHECK(replaced.status_ == OrderStatus::REPLACED);

  // kt00007 publishes no 체결 state, so a fill is read from the quantities.
  const auto filled = kiwoom::KiwoomClientTestAccess::history_row(
    json::parse("{" + base + R"(,"acpt_tp":"접수","mdfy_cncl":"","cntr_qty":"0000000010","cntr_uv":"0000070000","ord_remnq":"0000000000")" + "}")
      .as_object(),
    date
  );
  CHECK(filled.status_ == OrderStatus::FILLED);
  CHECK(filled.execution_.average_filled_price_ == Decimal64(70000, 0));

  const auto partial = kiwoom::KiwoomClientTestAccess::history_row(
    json::parse("{" + base + R"(,"acpt_tp":"접수","mdfy_cncl":"","cntr_qty":"0000000004","ord_remnq":"0000000006")" + "}")
      .as_object(),
    date
  );
  CHECK(partial.status_ == OrderStatus::PARTIAL_FILLED);

  const auto pending = kiwoom::KiwoomClientTestAccess::history_row(
    json::parse("{" + base + R"(,"acpt_tp":"접수","mdfy_cncl":"","cntr_qty":"0","ord_remnq":"0000000010")" + "}")
      .as_object(),
    date
  );
  CHECK(pending.status_ == OrderStatus::PENDING);
  // The label vocabulary here is kt00007's, and it reads back to the same shape ka10075's does.
  CHECK(pending.type_ == OrderType::LIMIT);
  CHECK(pending.time_in_force_ == TimeInForce::DAY);
  REQUIRE(pending.ordered_at_);
}

TEST_CASE("a cancelled order's own history row ends once its cancellation names it", "[kiwoom][history]") {
  const auto date = Date{2026, 10, 6};
  // The demo server's rows for a far limit buy and its cancel (2026-10-06, F45), at 10 shares rather than the observed 1
  // so that a partial fill can be written.
  const auto row = [](
    const std::string& number, const std::string& original, const std::string& amendment,
    const std::string& filled, const std::string& remaining, const std::string& side = "현금매수"
  ) {
    auto text = std::string(R"({"ord_no":")") + number + R"(","ori_ord":")" + original +
      R"(","stk_cd":"A005930","stk_nm":"삼성전자","io_tp_nm":")" + side +
      R"(","trde_tp":"보통가","acpt_tp":"주문완료","mdfy_cncl":")" + amendment +
      R"(","ord_qty":"10","ord_uv":"221000","cntr_qty":")" + filled + R"(","dmst_stex_tp":"KRX","ord_tm":"09:01:12")";
    if (!remaining.empty()) {
      text += R"(,"ord_remnq":")" + remaining + "\"";
    }
    return text + "}";
  };
  const auto original = row("0013356", "0000000", "일반", "0", "0");
  const auto cancel = row("0013459", "0013356", "취소", "0", "0");
  const auto confirmed = row("0013459", "0013356", "취소확인", "0", "0");
  using Access = kiwoom::KiwoomClientTestAccess;

  SECTION("the demo's words close the original, with nothing filled") {
    const auto orders = Access::history_rows({original, cancel}, date, true);
    CHECK(orders[0].status_ == OrderStatus::CANCELED);
    CHECK(orders[0].execution_.filled_quantity_.coefficient() == 0);
    // The cancellation row is not an order, and keeps what it read before.
    CHECK(orders[1].status_ == OrderStatus::UNKNOWN);
  }
  SECTION("a bare 취소 on a real account is not a completed cancellation") {
    const auto orders = Access::history_rows({original, cancel}, date, false);
    CHECK(orders[0].status_ == OrderStatus::UNKNOWN);
  }
  SECTION("the documented 취소확인 closes it on either server, from a later page too") {
    CHECK(Access::history_rows({original, confirmed}, date, false)[0].status_ == OrderStatus::CANCELED);
    CHECK(Access::history_rows({confirmed, original}, date, true)[1].status_ == OrderStatus::CANCELED);
  }
  SECTION("a partial fill stays the filled quantity") {
    const auto orders = Access::history_rows({row("0013356", "0000000", "일반", "3", "0"), confirmed}, date, false);
    CHECK(orders[0].status_ == OrderStatus::CANCELED);
    CHECK(orders[0].execution_.filled_quantity_ == Decimal64(3, 0));
  }
  SECTION("a cancel that lost to a full fill leaves it filled") {
    const auto orders = Access::history_rows({row("0013356", "0000000", "일반", "10", "0"), confirmed}, date, false);
    CHECK(orders[0].status_ == OrderStatus::FILLED);
  }
  SECTION("a remainder, or none stated, keeps it open") {
    CHECK(
      Access::history_rows({row("0013356", "0000000", "일반", "0", "10"), confirmed}, date, false)[0].status_ !=
      OrderStatus::CANCELED
    );
    CHECK(
      Access::history_rows({row("0013356", "0000000", "일반", "0", ""), confirmed}, date, false)[0].status_ !=
      OrderStatus::CANCELED
    );
  }
  SECTION("an ambiguous or mismatched original is left alone") {
    const auto twice = Access::history_rows({original, original, confirmed}, date, false);
    CHECK(twice[0].status_ != OrderStatus::CANCELED);
    CHECK(twice[1].status_ != OrderStatus::CANCELED);
    const auto sell = Access::history_rows({row("0013356", "0000000", "일반", "0", "0", "현금매도"), confirmed}, date, false);
    CHECK(sell[0].status_ != OrderStatus::CANCELED);
  }
}

// ---------------------------------------------------------------------------------------------
// The port
// ---------------------------------------------------------------------------------------------

TEST_CASE("Kiwoom's capability answers are what the adapter can back", "[kiwoom][port]") {
  using Caps = BrokerCaps<kiwoom::KiwoomClient>;
  STATIC_REQUIRE(Caps::has_account_discovery);
  STATIC_REQUIRE_FALSE(Caps::has_client_order_id);
  STATIC_REQUIRE(Caps::has_working_orders);
  STATIC_REQUIRE(Caps::has_amendable_quantity);
  // `ka10075` is the 미체결 listing and its specification documents `ord_stt` as `접수` alone.
  // `normalized_order_status` accepts `체결` and `취소` only because the history parser shares it,
  // and what a parser can represent is evidence about the parser rather than about what the
  // endpoint sends. Answering `true` here would leave a caller waiting for an event that may never
  // arrive.
  STATIC_REQUIRE_FALSE(Caps::working_orders_state_terminal);
  STATIC_REQUIRE_FALSE(Caps::has_order_by_id);
  STATIC_REQUIRE(Caps::has_partial_cancel);
  STATIC_REQUIRE_FALSE(Caps::history_requires_range);
  STATIC_REQUIRE_FALSE(Caps::has_history_cursor);
  STATIC_REQUIRE_FALSE(Caps::has_history_limit);
  STATIC_REQUIRE(Caps::has_history_symbol_filter);
  STATIC_REQUIRE(Caps::has_depth);
  STATIC_REQUIRE(Caps::has_price_limits);
  STATIC_REQUIRE(Caps::has_trade_prints);
  STATIC_REQUIRE_FALSE(Caps::has_batch_prices);
  STATIC_REQUIRE(Caps::has_buying_power);
  // NONE rather than HOLDINGS: kt00018 does report trde_able_qty on every row, but the port does
  // not serve that listing at all while its venue scope is unestablished, and a capability says
  // what this library can do rather than what the broker publishes.
  STATIC_REQUIRE(Caps::sellable_source == SellableSource::NONE);
  STATIC_REQUIRE_FALSE(Caps::has_commission_schedule);
  STATIC_REQUIRE(Caps::has_reference_data);
  STATIC_REQUIRE_FALSE(Caps::has_batch_reference_data);
  STATIC_REQUIRE_FALSE(Caps::has_calendar);
  STATIC_REQUIRE(Caps::supports_interval(BarInterval::ONE_MINUTE));
  STATIC_REQUIRE(Caps::supports_interval(BarInterval::ONE_YEAR));

  // Kiwoom disagrees with both shipped brokers about the history shape, which is the point of
  // asserting it here rather than only in the traits.
  STATIC_REQUIRE(BrokerCaps<kis::KisClient>::history_requires_range);
  STATIC_REQUIRE(BrokerCaps<kis::KisClient>::has_calendar);
  STATIC_REQUIRE(BrokerCaps<tossinvest::TossClient>::has_calendar);
}

TEST_CASE("both Kiwoom clients share one broker tag", "[kiwoom][port]") {
  // Conformance itself is asserted in test-capability-conformance.cpp, once per client.
  // One tag, so a reference read through the blocking facade is usable with the asynchronous
  // client and is a different *type* from another broker's.
  STATIC_REQUIRE(
    std::is_same_v<
      BrokerTraits<kiwoom::KiwoomClient>::BrokerTag,
      BrokerTraits<kiwoom::KiwoomBlockingClient>::BrokerTag
    >
  );
  STATIC_REQUIRE_FALSE(
    std::is_same_v<OrderRef<kiwoom::Broker>, OrderRef<kis::Broker>>
  );
}

TEST_CASE("a Kiwoom session binds an account that carries nothing", "[kiwoom][port]") {
  ssl::context tls(ssl::context::tls_client);
  BlockingSession<kiwoom::KiwoomBlockingClient> session(
    std::make_shared<kiwoom::KiwoomBlockingClient>(tls, kiwoom::KiwoomConfig{}),
    kiwoom::KiwoomAccount{}
  );
  CHECK(session.broker_name() == "kiwoom");
  // There is nothing to compare, which is the honest answer for a broker whose credential is its
  // account.
  CHECK(session.account() == kiwoom::KiwoomAccount{});
}

TEST_CASE("a calendar request to Kiwoom is refused rather than answered", "[kiwoom][port]") {
  using Traits = BrokerTraits<kiwoom::KiwoomClient>;
  const auto refusal = Traits::validate_calendar(Country::KR, std::nullopt);
  REQUIRE(refusal);
  CHECK(refusal->broker_code_ == "calendar_unsupported");
  CHECK_FALSE(refusal->reached_broker_);
}

// ---------------------------------------------------------------------------------------------
// The bar walk
// ---------------------------------------------------------------------------------------------

TEST_CASE("a bar walk reports a cursor exactly when it can be acted on", "[kiwoom][bars]") {
  BarQuery query;
  query.instrument_ = samsung();
  query.interval_ = BarInterval::ONE_DAY;
  query.count_ = 6;

  // Two pages of three, and Kiwoom says there is more after the second. The count is satisfied, so
  // the walk stops *with data still available* -- which is precisely when the caller needs the
  // cursor, and precisely the case that was broken when the key was recorded after the decision.
  const auto satisfied = kiwoom::KiwoomClientTestAccess::walk(
    query, {{3, std::string("page-2")}, {3, std::string("page-3")}}
  );
  REQUIRE(satisfied);
  CHECK(satisfied.value().bars_.size() == 6);
  REQUIRE(satisfied.value().next_cursor_);
  // The cursor is the oldest bar in hand, which is what `before_` takes to page further back.
  CHECK(*satisfied.value().next_cursor_ ==
        format_iso8601_utc(satisfied.value().bars_.front().timestamp_));
  // Ascending, so the oldest is first and the newest last.
  CHECK(satisfied.value().bars_.front().timestamp_ < satisfied.value().bars_.back().timestamp_);
  CHECK(satisfied.value().stamp_ == BarStamp::OPEN);

  // An exhausted history reports no cursor rather than one that would return nothing.
  const auto exhausted = kiwoom::KiwoomClientTestAccess::walk(
    query, {{3, std::string("page-2")}, {2, std::nullopt}}
  );
  REQUIRE(exhausted);
  CHECK(exhausted.value().bars_.size() == 5);
  CHECK_FALSE(exhausted.value().next_cursor_);

  // A page that adds nothing while Kiwoom still offers more is an incomplete walk, not a finished
  // one: returning the two bars gathered so far would claim a series the request did not get.
  const auto stalled = kiwoom::KiwoomClientTestAccess::walk(
    query, {{2, std::string("page-2")}, {0, std::string("page-3")}, {9, std::string("page-4")}}
  );
  REQUIRE_FALSE(stalled);
  CHECK(stalled.error().broker_code_ == "incomplete_listing");

  // Kiwoom offering another page with no key to ask for it is the same answer.
  const auto keyless = kiwoom::KiwoomClientTestAccess::walk(
    query, {{2, std::string("page-2")}, {2, std::string()}}
  );
  REQUIRE_FALSE(keyless);
  CHECK(keyless.error().broker_code_ == "incomplete_listing");

  // More bars than asked for are trimmed to the count, newest kept.
  const auto trimmed = kiwoom::KiwoomClientTestAccess::walk(query, {{10, std::nullopt}});
  REQUIRE(trimmed);
  CHECK(trimmed.value().bars_.size() == 6);

  // What was asked for is what the series says it carries.
  CHECK(trimmed.value().adjusted_);
  query.adjusted_ = false;
  const auto raw = kiwoom::KiwoomClientTestAccess::walk(query, {{10, std::nullopt}});
  REQUIRE(raw);
  CHECK_FALSE(raw.value().adjusted_);
  query.adjusted_ = true;
}

TEST_CASE("a modification with no resting shape is refused", "[kiwoom][orders]") {
  // `OrderShape::type_` is a plain value defaulting to LIMIT, so a hand-built instruction cannot
  // say "leave the basis alone". Accepting it against a best-limit order would send a price and
  // leave the basis where it was -- success reported for a change that did not happen.
  using Traits = BrokerTraits<kiwoom::KiwoomClient>;
  OrderRef<kiwoom::Broker> ref;
  ref.primary_ = "0000069";
  ref.secondary_ = "005930";

  AmendInstruction bare;
  bare.quantity_ = Decimal64(3, 0);
  bare.venue_ = Venue::KRX;
  bare.shape_ = OrderShape(OrderType::LIMIT, Decimal64(71000, 0));
  const auto refused = Traits::to_replacement(ref, bare);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "incomplete_shape");
  CHECK_FALSE(refused.error().reached_broker_);

  // A cancellation preserves no shape, so the same instruction cancels without a resting shape.
  AmendInstruction cancel;
  cancel.quantity_ = Decimal64(3, 0);
  cancel.venue_ = Venue::KRX;
  const auto accepted = Traits::to_cancellation(ref, cancel);
  REQUIRE(accepted);
  CHECK(accepted.value().quantity_ == Decimal64(3, 0));
  CHECK_FALSE(accepted.value().price_);

  // And a change of basis against a known resting shape is refused as a basis change, not as an
  // unknown one -- the two messages have to be distinguishable or the caller cannot act on either.
  AmendInstruction basis;
  basis.quantity_ = Decimal64(3, 0);
  basis.venue_ = Venue::KRX;
  basis.shape_ = OrderShape(OrderType::LIMIT, Decimal64(71000, 0), TimeInForce::DAY, OrderSession::REGULAR);
  basis.resting_ = OrderShape(
    OrderType::BEST_LIMIT, std::nullopt, TimeInForce::DAY, OrderSession::REGULAR
  );
  const auto wrong_basis = Traits::to_replacement(ref, basis);
  REQUIRE_FALSE(wrong_basis);
  CHECK(wrong_basis.error().broker_code_ == "shape_change_unsupported");
  CHECK(wrong_basis.error().message_.find("price basis") != std::string::npos);
}

TEST_CASE("a row whose side cannot be read fails the listing", "[kiwoom][orders]") {
  // `OrderSide` has no unknown member and its default is BUY, so a label this adapter cannot read
  // would be reported as a *buy* if the field were left alone -- and a resting sell reported as a
  // buy is not deducted from what may be sold. This is the one unrecognized value the adapter
  // refuses rather than tolerates, because there is no way to represent not knowing.
  const auto unreadable = json::parse(
    R"({"ord_no":"0000069","stk_cd":"005930","ord_stt":"접수","stk_nm":"삼성전자",
        "ord_qty":"1","ord_pric":"0","oso_qty":"1","io_tp_nm":"장내이체","trde_tp":"보통",
        "tm":"154113","cntr_qty":"0","stex_tp":"1"})"
  ).as_object();
  CHECK_THROWS(kiwoom::KiwoomClientTestAccess::open_order(unreadable));

  // Every spelling the two listings document is readable, including the coloured and credit forms.
  for (const char* label : {"+매수", "-매도", "매수", "매도", "현금매수", "현금매도",
                            "융자매수", "융자매도", "매수정정", "매도취소", "매수신용"}) {
    INFO(label);
    CHECK(kiwoom::normalized_order_side(label).has_value());
  }
  CHECK(*kiwoom::normalized_order_side("현금매도") == OrderSide::SELL);
  CHECK(*kiwoom::normalized_order_side("매수정정") == OrderSide::BUY);
}

TEST_CASE("all remaining is Kiwoom's own zero, not a quantity read from a listing", "[kiwoom][orders]") {
  // kt10002 documents mdfy_qty as "'0' 입력 시 잔량 전부 정정" and kt10003 documents cncl_qty as
  // "'0' 입력시 잔량 전부 취소". Sending Kiwoom's zero is better than sending a number read from a
  // working-order listing, because a partial fill landing in between makes that number too large.
  using Traits = BrokerTraits<kiwoom::KiwoomClient>;
  OrderRef<kiwoom::Broker> ref;
  ref.primary_ = "0000069";
  ref.secondary_ = "005930";

  AmendInstruction everything;
  everything.venue_ = Venue::KRX;
  everything.all_remaining_ = true;
  const auto converted = Traits::to_cancellation(ref, everything);
  REQUIRE(converted);
  CHECK(converted.value().quantity_ == Decimal64());

  ClientFixture fixture;
  const auto request =
    kiwoom::KiwoomClientTestAccess::amend_request(*fixture.client_, converted.value(), true);
  CHECK(body_of(request).at("cncl_qty").as_string() == "0");
  // And the client accepts it: a zero quantity is valid on an amendment and refused on an order.
  CHECK_FALSE(kiwoom::KiwoomClientTestAccess::validate_amendment(converted.value(), true));

  OrderRequest nothing;
  nothing.instrument_ = samsung();
  nothing.type_ = OrderType::MARKET;
  nothing.venue_ = Venue::KRX;
  nothing.quantity_ = Decimal64();
  std::string code;
  const auto refused = kiwoom::KiwoomClientTestAccess::validate_order(nothing, code);
  REQUIRE(refused);
  CHECK(refused->broker_code_ == "invalid_quantity");

  // Neither all_remaining_ nor a quantity is still refused: there is nothing to act on.
  AmendInstruction bare;
  bare.venue_ = Venue::KRX;
  const auto empty = Traits::to_cancellation(ref, bare);
  REQUIRE_FALSE(empty);
  CHECK(empty.error().broker_code_ == "quantity_required");
}

TEST_CASE("a listing that stops with pages remaining is refused, not returned", "[kiwoom][paging]") {
  // A guard that bounds a walk must not also decide the walk finished. Kiwoom says `cont-yn: Y`
  // with a `next-key` exactly when another page remains, so returning what was gathered hands a
  // partial answer to code that cannot tell: a working-order listing missing a resting sell
  // understates what RiskGuard deducts, and partial holdings understate exposure, which is the
  // permissive direction for a position ceiling.
  using Access = kiwoom::KiwoomClientTestAccess;
  std::string key;
  int pages = 0;

  // Kiwoom offered nothing further: complete.
  CHECK(Access::page_step(key, pages, true, std::nullopt) == Access::kFinish);
  CHECK(key.empty());

  // Kiwoom offered another page and the page grew: continue, carrying the key forward.
  pages = 0;
  CHECK(Access::page_step(key, pages, true, std::string("page-2")) == Access::kContinue);
  CHECK(key == "page-2");

  // Kiwoom offered another page and this one added nothing: incomplete, not finished.
  pages = 0;
  key.clear();
  CHECK(Access::page_step(key, pages, false, std::string("page-2")) == Access::kIncomplete);

  // The page budget is reached while Kiwoom still offers more: incomplete, not finished.
  pages = 49;
  key.clear();
  CHECK(Access::page_step(key, pages, true, std::string("page-51")) == Access::kIncomplete);
  CHECK(pages == 50);

  // Kiwoom offered another page and gave no key for it: incomplete, not finished. This is the
  // case that used to read as "no further page" and return a partial listing as success.
  pages = 0;
  key.clear();
  CHECK(Access::page_step(key, pages, true, std::string()) == Access::kIncomplete);
  CHECK(key.empty());

  // And the failure says which listing and is attributed to the broker, because Kiwoom answered.
  const auto error = Access::incomplete("holdings", 50);
  CHECK(error.category_ == ErrorCategory::PROTOCOL);
  CHECK(error.broker_code_ == "incomplete_listing");
  CHECK(error.reached_broker_);
  CHECK(error.message_.find("holdings") != std::string::npos);
}

TEST_CASE("the shape table is the codes Kiwoom publishes, and only those", "[kiwoom][orders]") {
  // Transcribed literally from `kt10000.requestIo.trde_tp.desc` rather than read back out of the
  // table the implementation uses, so a code mistyped in one place is not confirmed by the other.
  // The loop over `kOrderShapes` elsewhere in this file cannot catch that on its own.
  //
  // "0:보통, 3:시장가, 5:조건부지정가, 81:장마감후시간외, 61:장시작전시간외, 6:최유리지정가,
  //  7:최우선지정가, 10:보통(IOC), 13:시장가(IOC), 16:최유리(IOC), 20:보통(FOK), 23:시장가(FOK),
  //  26:최유리(FOK), 28:스톱지정가, 29:중간가, 30:중간가(IOC), 31:중간가(FOK), 40:보통(GTP),
  //  46:최유리(GTP), 47:최우선(GTP)"
  static constexpr std::pair<const char*, const char*> kPublished[] = {
    {"0", "보통"},         {"3", "시장가"},       {"5", "조건부지정가"}, {"81", "장마감후시간외"},
    {"61", "장시작전시간외"}, {"6", "최유리지정가"}, {"7", "최우선지정가"}, {"10", "보통(IOC)"},
    {"13", "시장가(IOC)"}, {"16", "최유리(IOC)"}, {"20", "보통(FOK)"},   {"23", "시장가(FOK)"},
    {"26", "최유리(FOK)"}, {"28", "스톱지정가"},  {"29", "중간가"},      {"30", "중간가(IOC)"},
    {"31", "중간가(FOK)"}, {"40", "보통(GTP)"},   {"46", "최유리(GTP)"}, {"47", "최우선(GTP)"},
  };
  static_assert(sizeof(kPublished) / sizeof(kPublished[0]) == 20, "kt10000 publishes 20 codes");

  // Every published code is in the table under the name the request list gives it.
  for (const auto& published : kPublished) {
    INFO(published.first << ' ' << published.second);
    const auto* by_code = kiwoom::shape_by_code(published.first);
    REQUIRE(by_code != nullptr);
    CHECK(std::string_view(by_code->name_) == published.second);
    // And that name reads back to the same row, so request and response agree.
    CHECK(kiwoom::shape_by_name(published.second) == by_code);
  }

  // And the table carries no code Kiwoom does not publish. `시간외단일가` is the one extra row and
  // it carries no code at all, which is why it is counted separately.
  int coded = 0;
  int codeless = 0;
  for (const auto& row : kiwoom::kOrderShapes) {
    if (row.code_ == nullptr) {
      ++codeless;
      CHECK_FALSE(row.requestable_);
      continue;
    }
    ++coded;
    bool published = false;
    for (const auto& entry : kPublished) {
      published = published || std::string_view(row.code_) == entry.first;
    }
    INFO("table code " << row.code_);
    CHECK(published);
  }
  CHECK(coded == 20);
  CHECK(codeless == 1);
}

TEST_CASE("the interval guard reads the same table the request does", "[kiwoom][bars]") {
  // The guard used to answer from a constant, so it could have said "supported" for an interval
  // the request builder had no endpoint for -- and the builder fell through to the daily endpoint,
  // which would have returned *daily bars* under another interval's name.
  for (const auto interval : {BarInterval::ONE_MINUTE, BarInterval::ONE_DAY, BarInterval::ONE_WEEK,
                              BarInterval::ONE_MONTH, BarInterval::ONE_YEAR}) {
    INFO(bar_interval_name(interval));
    CHECK_FALSE(kiwoom::unsupported_bar_interval(interval));
    REQUIRE(kiwoom::chart_api_id(interval) != nullptr);
    REQUIRE(kiwoom::chart_rows_field(interval) != nullptr);
    CHECK(BrokerCaps<kiwoom::KiwoomClient>::supports_interval(interval));
  }
  // Each interval has its own endpoint and its own row field; none shares with another.
  CHECK(std::string_view(kiwoom::chart_api_id(BarInterval::ONE_WEEK)) == "ka10082");
  CHECK(std::string_view(kiwoom::chart_rows_field(BarInterval::ONE_WEEK)) == "stk_stk_pole_chart_qry");
  CHECK(std::string_view(kiwoom::chart_api_id(BarInterval::ONE_YEAR)) == "ka10094");
}

TEST_CASE("a priced limit order can be cancelled through the runner's own helper", "[kiwoom][orders]") {
  // **Built with `cancel_all(working)`, not by hand**, because the hand-built instruction is what
  // hid this: `cancel_all` copies the resting order's whole shape -- price included -- so that a
  // broker which re-sends the shape withdraws the order it was given. Kiwoom's kt10003 carries a
  // quantity and nothing else, and refusing every priced cancellation made every priced limit
  // order uncancellable through `SessionExecutor::withdraw`, which is what a WITHDRAW or a FLATTEN
  // calls. The orders would have stayed resting at the exchange.
  using Traits = BrokerTraits<kiwoom::KiwoomClient>;

  WorkingOrder<kiwoom::Broker> working;
  working.ref_ = OrderRef<kiwoom::Broker>{"0000069", "005930"};
  working.order_.instrument_ = samsung();
  working.order_.type_ = OrderType::LIMIT;
  working.order_.time_in_force_ = TimeInForce::DAY;
  working.order_.session_ = OrderSession::REGULAR;
  working.order_.venue_ = Venue::KRX;
  working.order_.price_ = Decimal64(70000, 0);
  working.order_.quantity_ = Decimal64(10, 0);
  working.amendable_quantity_ = Decimal64(7, 0);

  const auto withdraw = Traits::to_cancellation(working.ref_, cancel_all(working));
  REQUIRE(withdraw);
  // `cancel_all` sets all_remaining_, so Kiwoom's own zero goes on the wire rather than the 7 the
  // listing reported -- which a partial fill may already have invalidated.
  CHECK(withdraw.value().quantity_ == Decimal64());
  CHECK(withdraw.value().order_.symbol_ == "005930");
  CHECK_FALSE(withdraw.value().price_);

  // A partial cancellation through the other helper works the same way and keeps its quantity.
  const auto partial =
    Traits::to_cancellation(working.ref_, cancel_quantity(working, Decimal64(3, 0)));
  REQUIRE(partial);
  CHECK(partial.value().quantity_ == Decimal64(3, 0));

  // A market order has no price to carry and was never affected; it still cancels.
  WorkingOrder<kiwoom::Broker> unpriced = working;
  unpriced.order_.type_ = OrderType::MARKET;
  unpriced.order_.price_.reset();
  CHECK(Traits::to_cancellation(unpriced.ref_, cancel_all(unpriced)));

  // And a cancellation that asks for a *different* price is still refused: Kiwoom cannot reprice
  // an order on the way out, and silently dropping the request would report success for it.
  auto repricing = cancel_all(working);
  repricing.shape_.price_ = Decimal64(71000, 0);
  const auto refused = Traits::to_cancellation(working.ref_, repricing);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "price_on_cancellation");

  // A price stated with no resting shape to compare it against is refused too, because nothing
  // says it is a restatement rather than a request.
  AmendInstruction bare;
  bare.venue_ = Venue::KRX;
  bare.quantity_ = Decimal64(3, 0);
  bare.shape_ = OrderShape(OrderType::LIMIT, Decimal64(70000, 0));
  const auto unknown = Traits::to_cancellation(working.ref_, bare);
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error().broker_code_ == "price_on_cancellation");
}

TEST_CASE("the two holdings assertions are independent of each other", "[kiwoom][holdings]") {
  // **Two switches, not one, and this is the test that keeps them apart.** kt00018's `dmst_stex_tp`
  // desc is "KRX:한국거래소,NXT:넥스트트레이드" and both quantity fields' descs carry units and
  // zero-padding, so nothing ties `rmnd_qty`'s scope to `trde_able_qty`'s. One switch covering both
  // would assume exactly that, and the sellable half is the one that can admit a sell of shares
  // that are not there.
  using Access = kiwoom::KiwoomClientTestAccess;

  // Asserting the *positions* scope establishes nothing about the sellable figure.
  const auto unstated = Access::holdings_sellable_basis(kiwoom::KiwoomSellableScope::UNSPECIFIED);
  CHECK(unstated.coverage_ == SellableBasis::Coverage::UNESTABLISHED);
  CHECK_FALSE(unstated.is_enforceable());

  const auto stated = Access::holdings_sellable_basis(kiwoom::KiwoomSellableScope::WHOLE_HOLDING);
  CHECK(stated.coverage_ == SellableBasis::Coverage::WHOLE_HOLDING);
  CHECK(stated.is_enforceable());

  // Neither assertion is a measurement, and the type says so rather than a comment saying so.
  CHECK_FALSE(unstated.verified_);
  CHECK_FALSE(stated.verified_);

  // The resting treatment is this library's, not the caller's: conservative under both, because
  // over-deducting only ever refuses a trade. A caller cannot assert it away.
  CHECK(unstated.resting_ == SellableBasis::RestingTreatment::DEDUCT);
  CHECK(stated.resting_ == SellableBasis::RestingTreatment::DEDUCT);

  // And the config's defaults are both unstated, so neither is answered by a silent default.
  kiwoom::KiwoomConfig fresh;
  CHECK(fresh.holdings_position_scope_ == kiwoom::KiwoomPositionScope::UNSPECIFIED);
  CHECK(fresh.holdings_sellable_scope_ == kiwoom::KiwoomSellableScope::UNSPECIFIED);
}

TEST_CASE("the normalized holdings verb refuses while its scope is unestablished", "[kiwoom][holdings]") {
  // The port promises an *account's* holdings. kt00018 names one venue, publishes no all-venue
  // value, and does not say whether that filters the positions or only values them -- so this
  // library cannot answer the question the port asks, and says so rather than answering a
  // narrower one under the same name. An understated holding is the permissive direction for a
  // position ceiling and for anything sizing an exit.
  ssl::context tls(ssl::context::tls_client);
  kiwoom::KiwoomConfig config;
  config.holdings_venue_ = Venue::KRX;
  BlockingSession<kiwoom::KiwoomBlockingClient> session(
    std::make_shared<kiwoom::KiwoomBlockingClient>(tls, config), kiwoom::KiwoomAccount{}
  );

  const auto refused = session.holdings();
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "holdings_scope_unestablished");
  CHECK(refused.error().category_ == ErrorCategory::INVALID_ARGUMENT);
  // Nothing was sent: this is a refusal to make a claim, not a broker failure.
  CHECK_FALSE(refused.error().reached_broker_);
  // And it points at the inquiry that is still available, which is how the question gets settled.
  CHECK(refused.error().message_.find("session.client().holdings()") != std::string::npos);

  // The capability follows the port rather than the broker: kt00018 does report trde_able_qty, and
  // this library cannot stand behind it while the listing it comes from may be missing a venue.
  STATIC_REQUIRE(BrokerCaps<kiwoom::KiwoomBlockingClient>::sellable_source == SellableSource::NONE);

  // The broker-specific inquiry is untouched, and still validates its own venue.
  kiwoom::KiwoomConfig unstated;
  ClientFixture silent(unstated);
  REQUIRE(kiwoom::KiwoomClientTestAccess::holdings_venue(*silent.client_));
}

TEST_CASE("an unreadable risk quantity fails the row rather than reading as zero", "[kiwoom][parsing]") {
  // Zero is a statement. A holdings row whose `rmnd_qty` came back unreadable, turned into zero,
  // is a successful listing saying the account holds none of that instrument -- and
  // `Portfolio::net_quantity_of` would undercount the exposure a position ceiling is measured
  // against. A working order whose `ord_qty` was unreadable would understate
  // `Portfolio::working_quantity`, which is what the sellable guard deducts a resting sell with.
  //
  // A *reported* zero is still zero and still fine. It is the unreadable one that has to fail,
  // because afterwards the two are indistinguishable.
  const auto holding = [](const char* quantity) {
    return std::string(
      R"({"tot_pur_amt":"0","tot_evlt_amt":"0","tot_evlt_pl":"0",
          "acnt_evlt_remn_indv_tot":[{"stk_cd":"A005930","rmnd_qty":")") + quantity +
      R"(","trde_able_qty":"3","pur_pric":"124500","cur_prc":"59000","pur_amt":"373500",
          "evlt_amt":"177000","evltv_prft":"-196888"}]})";
  };

  for (const char* unreadable : {"garbled", "", "3 shares", "--3"}) {
    INFO("rmnd_qty " << unreadable);
    HoldingsSnapshot snapshot;
    bool seen = false;
    CHECK_THROWS(kiwoom::KiwoomClientTestAccess::holdings_page(
      snapshot, seen, json::parse(holding(unreadable)).as_object()
    ));
  }

  // A reported zero is accepted and stays zero.
  HoldingsSnapshot empty;
  bool seen = false;
  CHECK_NOTHROW(kiwoom::KiwoomClientTestAccess::holdings_page(
    empty, seen, json::parse(holding("000000000000000")).as_object()
  ));
  REQUIRE(empty.positions_.size() == 1);
  CHECK(empty.positions_.front().quantity_ == Decimal64());

  // **And the line is drawn no wider than that.** The failure mode of this guard is the opposite
  // one: a field made required that Kiwoom may legitimately omit fails a valid listing. So a row
  // carrying the quantity and none of the reporting figures still parses -- an account with no
  // cost basis to report is not a reason to fail a poll.
  HoldingsSnapshot sparse;
  bool sparse_seen = false;
  CHECK_NOTHROW(kiwoom::KiwoomClientTestAccess::holdings_page(
    sparse,
    sparse_seen,
    json::parse(
      R"({"acnt_evlt_remn_indv_tot":[{"stk_cd":"A005930","rmnd_qty":"3","trde_able_qty":"3"}]})"
    ).as_object()
  ));
  REQUIRE(sparse.positions_.size() == 1);
  CHECK(sparse.positions_.front().quantity_ == Decimal64(3, 0));
  CHECK(sparse.positions_.front().sellable_quantity_ == Decimal64(3, 0));
  // The reporting figures read as zero rather than failing, and the totals row is still there.
  CHECK(sparse.positions_.front().market_value_.amount_ == Decimal64());
  CHECK(sparse.totals_.size() == 1);

  // The same for a working order's quantity and its filled quantity.
  const auto order = [](const char* quantity, const char* filled) {
    return std::string(
      R"({"ord_no":"0000069","stk_cd":"005930","ord_stt":"접수","stk_nm":"삼성전자","ord_qty":")") +
      quantity + R"(","ord_pric":"70000","oso_qty":"7","io_tp_nm":"+매수","trde_tp":"보통",
          "tm":"154113","cntr_qty":")" + filled + R"(","stex_tp":"1"})";
  };
  CHECK_THROWS(
    kiwoom::KiwoomClientTestAccess::open_order(json::parse(order("garbled", "3")).as_object())
  );
  CHECK_THROWS(
    kiwoom::KiwoomClientTestAccess::open_order(json::parse(order("10", "garbled")).as_object())
  );
  const auto readable =
    kiwoom::KiwoomClientTestAccess::open_order(json::parse(order("10", "3")).as_object());
  CHECK(readable.order_.quantity_ == Decimal64(10, 0));
  CHECK(readable.order_.execution_.filled_quantity_ == Decimal64(3, 0));
}

TEST_CASE("a placement is refused while the holdings scope is unestablished", "[kiwoom][orders]") {
  // A new order is the one instruction that needs a portfolio, and Kiwoom's cannot be established.
  // Leaving this to the loop's send gate left a configuration-shaped hole: a schedule with no
  // holdings cadence never calls the refusing port at all, and would have sized against an empty
  // portfolio -- the permissive baseline for a position ceiling.
  using Traits = BrokerTraits<kiwoom::KiwoomClient>;

  OrderRequest order;
  order.instrument_ = samsung();
  order.side_ = OrderSide::BUY;
  order.type_ = OrderType::LIMIT;
  order.venue_ = Venue::KRX;
  order.quantity_ = Decimal64(10, 0);
  order.price_ = Decimal64(70000, 0);

  // **The refusal is in the client, and that is where it belongs.** Every placement reaches it --
  // including the port's, because `BrokerSession::place` calls the client immediately after its
  // pre-flight and the client refuses before any I/O -- so there is one boundary rather than two
  // that could disagree, and the message is chosen after the order's own validation.
  std::string code;
  const auto direct = kiwoom::KiwoomClientTestAccess::validate_order(order, code);
  REQUIRE(direct);
  CHECK(direct->broker_code_ == "holdings_scope_unestablished");
  CHECK(direct->category_ == ErrorCategory::INVALID_ARGUMENT);
  CHECK_FALSE(direct->reached_broker_);

  // The port's own pre-flight still refuses what it is for, and nothing else: an idempotency key
  // Kiwoom does not publish. It deliberately does not repeat the holdings refusal, because it does
  // not validate the order's venue, price or shape and would report the wrong problem first.
  CHECK_FALSE(Traits::validate_place(order));
  OrderRequest keyed = order;
  keyed.client_order_id_ = "abc";
  const auto through_port = Traits::validate_place(keyed);
  REQUIRE(through_port);
  CHECK(through_port->broker_code_ == "client_order_id_unsupported");

  // A caller's own mistake is still reported first: this order names no venue, and that is what it
  // hears about, because it is the thing they can fix.
  OrderRequest unrouted = order;
  unrouted.venue_ = Venue::UNSPECIFIED;
  const auto own_mistake = kiwoom::KiwoomClientTestAccess::validate_order(unrouted, code);
  REQUIRE(own_mistake);
  CHECK(own_mistake->broker_code_ == "venue_required");

  // **Cancelling is unaffected**, which is the point of the split: a cancellation only ever
  // reduces what is resting, and an order that could not be withdrawn would keep resting and could
  // fill. A *replacement* is refused too -- see the reprice test -- because repricing a resting buy
  // into a fillable price opens the position this adapter cannot measure.
  WorkingOrder<kiwoom::Broker> working;
  working.ref_ = OrderRef<kiwoom::Broker>{"0000069", "005930"};
  working.order_.instrument_ = samsung();
  working.order_.type_ = OrderType::LIMIT;
  working.order_.time_in_force_ = TimeInForce::DAY;
  working.order_.session_ = OrderSession::REGULAR;
  working.order_.venue_ = Venue::KRX;
  working.order_.price_ = Decimal64(70000, 0);
  working.amendable_quantity_ = Decimal64(7, 0);

  CHECK(Traits::to_cancellation(working.ref_, cancel_all(working)));
  kiwoom::KiwoomAmendment amendment;
  amendment.order_ = kiwoom::KiwoomOrderRef{"0000069", "005930"};
  amendment.venue_ = Venue::KRX;
  amendment.quantity_ = Decimal64(3, 0);
  CHECK_FALSE(kiwoom::KiwoomClientTestAccess::validate_amendment(amendment, true));
}

TEST_CASE("a reprice is refused while the holdings scope is unestablished", "[kiwoom][orders]") {
  // **The first version of this guard covered placements alone, and that was wrong.** The reasoning
  // was that an amendment addresses an order the broker already has and never consults the
  // portfolio -- true, and not the point. A resting buy priced below the market is not executable;
  // kt10002 can move it to a price that is, and the fill opens or enlarges exactly the position
  // this adapter cannot measure. Repricing is a risk-changing instruction wearing an amendment's
  // clothes.
  using Traits = BrokerTraits<kiwoom::KiwoomClient>;

  WorkingOrder<kiwoom::Broker> working;
  working.ref_ = OrderRef<kiwoom::Broker>{"0000069", "005930"};
  working.order_.instrument_ = samsung();
  working.order_.type_ = OrderType::LIMIT;
  working.order_.time_in_force_ = TimeInForce::DAY;
  working.order_.session_ = OrderSession::REGULAR;
  working.order_.venue_ = Venue::KRX;
  working.order_.price_ = Decimal64(70000, 0);
  working.amendable_quantity_ = Decimal64(7, 0);

  // The conversion still succeeds -- it is a well-formed instruction -- and the client refuses it
  // before any I/O, which is the boundary no caller routes around.
  const auto converted = Traits::to_replacement(working.ref_, reprice(working, Decimal64(71000, 0)));
  REQUIRE(converted);
  const auto refused = kiwoom::KiwoomClientTestAccess::validate_amendment(converted.value(), false);
  REQUIRE(refused);
  CHECK(refused->broker_code_ == "holdings_scope_unestablished");
  CHECK_FALSE(refused->reached_broker_);

  // **Cancelling is unaffected, and must be**: kt10003 only ever reduces what is resting, and an
  // order that cannot be withdrawn keeps resting and can fill.
  const auto withdraw = Traits::to_cancellation(working.ref_, cancel_all(working));
  REQUIRE(withdraw);
  CHECK_FALSE(kiwoom::KiwoomClientTestAccess::validate_amendment(withdraw.value(), true));

  // Reducing a resting order's size is still expressible while this holds.
  const auto partial =
    Traits::to_cancellation(working.ref_, cancel_quantity(working, Decimal64(3, 0)));
  REQUIRE(partial);
  CHECK_FALSE(kiwoom::KiwoomClientTestAccess::validate_amendment(partial.value(), true));

  // A malformed amendment still hears about itself first, for the same reason a placement does.
  kiwoom::KiwoomAmendment nameless;
  nameless.order_ = kiwoom::KiwoomOrderRef{"0000069", ""};
  nameless.venue_ = Venue::KRX;
  nameless.quantity_ = Decimal64(3, 0);
  const auto own_mistake = kiwoom::KiwoomClientTestAccess::validate_amendment(nameless, false);
  REQUIRE(own_mistake);
  CHECK(own_mistake->broker_code_ == "invalid_order_reference");
}

TEST_CASE("a cancellation refuses a trigger it cannot carry", "[kiwoom][orders]") {
  // kt10003 sends a quantity and nothing else. A stated trigger would be dropped on the way to the
  // wire, and acknowledging the instruction afterwards would report success for a change that did
  // not happen -- the same failure refusing a *price* on a cancellation exists to stop. This is
  // reachable only through `session.client()`; the normalized helpers never set the field.
  kiwoom::KiwoomAmendment amendment;
  amendment.order_ = kiwoom::KiwoomOrderRef{"0000069", "005930"};
  amendment.venue_ = Venue::KRX;
  amendment.quantity_ = Decimal64(3, 0);
  amendment.condition_price_ = Decimal64(69000, 0);

  const auto refused = kiwoom::KiwoomClientTestAccess::validate_amendment(amendment, true);
  REQUIRE(refused);
  CHECK(refused->broker_code_ == "invalid_condition_price");
  CHECK_FALSE(refused->reached_broker_);

  // And the request builder really does drop it, which is why the refusal has to be here.
  ClientFixture fixture;
  const auto request =
    kiwoom::KiwoomClientTestAccess::amend_request(*fixture.client_, amendment, true);
  const auto body = body_of(request);
  CHECK_FALSE(body.contains("mdfy_cond_uv"));
  CHECK_FALSE(body.contains("cond_uv"));

  // Without the trigger the same cancellation is fine.
  amendment.condition_price_.reset();
  CHECK_FALSE(kiwoom::KiwoomClientTestAccess::validate_amendment(amendment, true));
}

TEST_CASE("a cancellation refuses every shape change it cannot carry", "[kiwoom][orders]") {
  // kt10003 transmits a quantity and nothing else, so *every* other stated field is one Kiwoom
  // cannot act on. The price and the trigger were refused first because acting on them is what a
  // caller would expect; these three are refused for the same reason and it is not a lesser one --
  // reporting acceptance for a stated change that never reached the broker is the defect, whatever
  // the field.
  using Traits = BrokerTraits<kiwoom::KiwoomClient>;

  WorkingOrder<kiwoom::Broker> working;
  working.ref_ = OrderRef<kiwoom::Broker>{"0000069", "005930"};
  working.order_.instrument_ = samsung();
  working.order_.type_ = OrderType::LIMIT;
  working.order_.time_in_force_ = TimeInForce::DAY;
  working.order_.session_ = OrderSession::REGULAR;
  working.order_.venue_ = Venue::KRX;
  working.order_.price_ = Decimal64(70000, 0);
  working.amendable_quantity_ = Decimal64(7, 0);

  // The ordinary withdrawal is unaffected: `cancel_all` copies the resting shape into both halves,
  // so every comparison sees a restatement.
  REQUIRE(Traits::to_cancellation(working.ref_, cancel_all(working)));
  REQUIRE(Traits::to_cancellation(working.ref_, cancel_quantity(working, Decimal64(3, 0))));

  // A stated validity condition that differs from the resting one is refused.
  auto condition = cancel_all(working);
  condition.shape_.time_in_force_ = TimeInForce::FILL_OR_KILL;
  auto refused = Traits::to_cancellation(working.ref_, condition);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "shape_change_unsupported");
  CHECK(refused.error().message_.find("validity condition") != std::string::npos);

  // So is a session...
  auto session = cancel_all(working);
  session.shape_.session_ = OrderSession::AFTER_HOURS;
  refused = Traits::to_cancellation(working.ref_, session);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().message_.find("session") != std::string::npos);

  // ...and a price basis.
  auto basis = cancel_all(working);
  basis.shape_.type_ = OrderType::MARKET;
  refused = Traits::to_cancellation(working.ref_, basis);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().message_.find("price basis") != std::string::npos);

  // A condition stated with no resting shape to compare against cannot be shown to be a
  // restatement, so it is refused rather than assumed harmless.
  AmendInstruction bare;
  bare.venue_ = Venue::KRX;
  bare.quantity_ = Decimal64(3, 0);
  bare.shape_.time_in_force_ = TimeInForce::IMMEDIATE_OR_CANCEL;
  refused = Traits::to_cancellation(working.ref_, bare);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "shape_change_unsupported");

  // A bare cancellation that states nothing still works. `OrderShape::type_` is a plain value with
  // a `LIMIT` default, so "left alone" and "stated LIMIT" are the same bytes and only the default
  // is ambiguous.
  AmendInstruction plain;
  plain.venue_ = Venue::KRX;
  plain.quantity_ = Decimal64(3, 0);
  CHECK(Traits::to_cancellation(working.ref_, plain));

  // **But a value that differs from the default is not ambiguous**, even with no resting shape: a
  // caller writing `MARKET` on a cancellation is saying something, kt10003 transmits no basis, and
  // accepting it would report acceptance for a change the broker never saw.
  AmendInstruction stated = plain;
  stated.shape_.type_ = OrderType::MARKET;
  refused = Traits::to_cancellation(working.ref_, stated);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().broker_code_ == "shape_change_unsupported");
  CHECK(refused.error().message_.find("price basis") != std::string::npos);
}

TEST_CASE("a row with nothing outstanding keeps the status the broker stated", "[kiwoom][orders]") {
  // The rule narrows a contradiction; it is not a blanket refusal to believe the broker. With
  // `oso_qty` at zero the two fields agree and the word stands.
  const auto settled = kiwoom::KiwoomClientTestAccess::open_order(
    json::parse(
      R"({"ord_no":"0000069","stk_cd":"005930","ord_stt":"체결","stk_nm":"삼성전자",
          "ord_qty":"10","ord_pric":"70000","oso_qty":"0","orig_ord_no":"0000068",
          "io_tp_nm":"-매도","trde_tp":"보통","tm":"154113","cntr_qty":"10","stex_tp":"1"})"
    ).as_object()
  );
  CHECK(settled.order_.status_ == OrderStatus::FILLED);
}

TEST_CASE("an unfilled row that states nothing has filled reports pending", "[kiwoom][orders]") {
  const auto untouched = kiwoom::KiwoomClientTestAccess::open_order(
    json::parse(
      R"({"ord_no":"0000069","stk_cd":"005930","ord_stt":"취소","stk_nm":"삼성전자",
          "ord_qty":"10","ord_pric":"70000","oso_qty":"10","orig_ord_no":"0000068",
          "io_tp_nm":"+매수","trde_tp":"보통","tm":"154113","cntr_qty":"0","stex_tp":"1"})"
    ).as_object()
  );
  CHECK(untouched.order_.status_ == OrderStatus::PENDING);
}

// ---------------------------------------------------------------------------------------------
// Stop-limit orders
// ---------------------------------------------------------------------------------------------

TEST_CASE("Kiwoom places, amends and reads back a stop-limit order's trigger", "[kiwoom][trigger]") {
  using Traits = BrokerTraits<kiwoom::KiwoomClient>;
  ClientFixture fixture;

  OrderRequest stop;
  stop.instrument_ = samsung();
  stop.side_ = OrderSide::SELL;
  stop.type_ = OrderType::STOP_LIMIT;
  stop.venue_ = Venue::KRX;
  stop.quantity_ = Decimal64(10, 0);
  stop.price_ = Decimal64(69000, 0);
  stop.trigger_price_ = Decimal64(69500, 0);

  SECTION("28 is composed, and the trigger goes in cond_uv") {
    std::string code;
    // A settled holdings scope, so the only refusals possible are about the order itself.
    REQUIRE_FALSE(kiwoom::KiwoomClientTestAccess::validate_order(
      stop, code, kiwoom::KiwoomPositionScope::VALUATION_ONLY
    ));
    CHECK(code == "28");
    const auto body = json::parse(
      kiwoom::KiwoomClientTestAccess::place_request(*fixture.client_, stop, code).body_
    ).as_object();
    CHECK(body.at("trde_tp").as_string() == "28");
    CHECK(body.at("ord_uv").as_string() == "69000");
    CHECK(body.at("cond_uv").as_string() == "69500");
  }

  SECTION("every other body keeps its empty cond_uv") {
    auto limit = stop;
    limit.type_ = OrderType::LIMIT;
    limit.trigger_price_.reset();
    const auto body = json::parse(
      kiwoom::KiwoomClientTestAccess::place_request(*fixture.client_, limit, "0").body_
    ).as_object();
    CHECK(body.at("cond_uv").as_string() == "");
  }

  SECTION("a trigger the order cannot carry, or a stop without one, is refused before I/O") {
    const auto refused = [](OrderRequest order) {
      std::string code;
      const auto error = kiwoom::KiwoomClientTestAccess::validate_order(
        order, code, kiwoom::KiwoomPositionScope::VALUATION_ONLY
      );
      REQUIRE(error);
      CHECK(error->broker_code_ == "invalid_trigger_price");
      CHECK_FALSE(error->reached_broker_);
    };
    auto missing = stop;
    missing.trigger_price_.reset();
    refused(missing);
    auto on_limit = stop;
    on_limit.type_ = OrderType::LIMIT;
    refused(on_limit);
    for (const auto* value : {"0", "-69500", "69500.5"}) {
      auto bad = stop;
      bad.trigger_price_ = Decimal64::parse(value);
      INFO("trigger " << value);
      refused(bad);
    }
  }

  WorkingOrder<kiwoom::Broker> working;
  working.ref_.primary_ = "0000069";
  working.ref_.secondary_ = "005930";
  working.order_.type_ = OrderType::STOP_LIMIT;
  working.order_.time_in_force_ = TimeInForce::DAY;
  working.order_.session_ = OrderSession::REGULAR;
  working.order_.venue_ = Venue::KRX;
  working.order_.price_ = Decimal64(69000, 0);
  working.order_.trigger_price_ = Decimal64(69500, 0);
  working.amendable_quantity_ = Decimal64(10, 0);

  SECTION("a reprice restates the trigger in mdfy_cond_uv rather than sending it empty") {
    auto amendment = Traits::to_replacement(working.ref_, reprice(working, Decimal64(68000, 0)));
    REQUIRE(amendment);
    CHECK(amendment.value().condition_price_ == Decimal64(69500, 0));
    const auto body = json::parse(
      kiwoom::KiwoomClientTestAccess::amend_request(*fixture.client_, amendment.value(), false).body_
    ).as_object();
    CHECK(body.at("mdfy_uv").as_string() == "68000");
    CHECK(body.at("mdfy_cond_uv").as_string() == "69500");
  }

  SECTION("a stated trigger is sent as the new one") {
    auto amendment = Traits::to_replacement(
      working.ref_,
      replace_with(
        working,
        OrderShape{
          OrderType::STOP_LIMIT, Decimal64(69000, 0), std::nullopt, std::nullopt, Decimal64(69800, 0)
        }
      )
    );
    REQUIRE(amendment);
    CHECK(amendment.value().condition_price_ == Decimal64(69800, 0));
  }

  SECTION("a stop is withdrawn by cancel_all, which carries no trigger to kt10003") {
    // The regression the design exists to prevent: `cancel_all` restates the resting trigger, and
    // copying it into the cancellation would make the client refuse every stop withdrawal.
    auto amendment = Traits::to_cancellation(working.ref_, cancel_all(working));
    REQUIRE(amendment);
    CHECK_FALSE(amendment.value().condition_price_);
    CHECK_FALSE(kiwoom::KiwoomClientTestAccess::validate_amendment(amendment.value(), true));
  }

  SECTION("a cancellation stating a different trigger is refused as a change it cannot make") {
    auto instruction = cancel_all(working);
    instruction.shape_.trigger_price_ = Decimal64(69800, 0);
    const auto amendment = Traits::to_cancellation(working.ref_, instruction);
    REQUIRE_FALSE(amendment);
    CHECK(amendment.error().broker_code_ == "trigger_on_cancellation");
  }

  SECTION("a trigger on a modification of a basis that takes none is refused") {
    auto limit = working;
    limit.order_.type_ = OrderType::LIMIT;
    const auto amendment = Traits::to_replacement(limit.ref_, reprice(limit, Decimal64(68000, 0)));
    REQUIRE_FALSE(amendment);
    CHECK(amendment.error().broker_code_ == "invalid_trigger_price");
  }

  SECTION("the working listing reports the trigger from stop_pric") {
    const auto row = kiwoom::KiwoomClientTestAccess::open_order(json::parse(
      R"({"ord_no":"0000069","stk_cd":"005930","ord_stt":"접수","stk_nm":"삼성전자",
          "ord_qty":"10","ord_pric":"69000","oso_qty":"10","orig_ord_no":"0000000",
          "io_tp_nm":"-매도","trde_tp":"스톱지정가","tm":"154113","cntr_qty":"0","stex_tp":"1",
          "stop_pric":"69500"})"
    ).as_object());
    CHECK(row.order_.type_ == OrderType::STOP_LIMIT);
    CHECK(row.order_.trigger_price_ == Decimal64(69500, 0));
  }

  SECTION("an ordinary order's zero is not a trigger, and an unreadable one costs only the trigger") {
    const auto plain = kiwoom::KiwoomClientTestAccess::open_order(json::parse(
      R"({"ord_no":"0000070","stk_cd":"005930","ord_stt":"접수","stk_nm":"삼성전자",
          "ord_qty":"10","ord_pric":"70000","oso_qty":"10","orig_ord_no":"0000000",
          "io_tp_nm":"+매수","trde_tp":"보통","tm":"154113","cntr_qty":"0","stex_tp":"1",
          "stop_pric":"0"})"
    ).as_object());
    CHECK_FALSE(plain.order_.trigger_price_);
    const auto unreadable = kiwoom::KiwoomClientTestAccess::open_order(json::parse(
      R"({"ord_no":"0000071","stk_cd":"005930","ord_stt":"접수","stk_nm":"삼성전자",
          "ord_qty":"10","ord_pric":"69000","oso_qty":"10","orig_ord_no":"0000000",
          "io_tp_nm":"-매도","trde_tp":"스톱지정가","tm":"154113","cntr_qty":"0","stex_tp":"1",
          "stop_pric":"69.5.0"})"
    ).as_object());
    CHECK(unreadable.order_.type_ == OrderType::STOP_LIMIT);
    CHECK_FALSE(unreadable.order_.trigger_price_);
  }

  SECTION("a positive trigger on a row that is not a stop is recorded, and blocks a modification") {
    // Kiwoom contradicting its own `trde_tp` is kept where a caller can see it; a reprice then
    // restates a trigger on a basis that takes none and is refused, while the withdrawal works.
    const auto row = kiwoom::KiwoomClientTestAccess::open_order(json::parse(
      R"({"ord_no":"0000072","stk_cd":"005930","ord_stt":"접수","stk_nm":"삼성전자",
          "ord_qty":"10","ord_pric":"70000","oso_qty":"10","orig_ord_no":"0000000",
          "io_tp_nm":"-매도","trde_tp":"보통","tm":"154113","cntr_qty":"0","stex_tp":"1",
          "stop_pric":"69500"})"
    ).as_object());
    REQUIRE(row.order_.type_ == OrderType::LIMIT);
    REQUIRE(row.order_.trigger_price_ == Decimal64(69500, 0));

    WorkingOrder<kiwoom::Broker> contradicted;
    contradicted.ref_.primary_ = "0000072";
    contradicted.ref_.secondary_ = "005930";
    contradicted.order_ = row.order_;
    contradicted.order_.venue_ = Venue::KRX;
    contradicted.amendable_quantity_ = row.amendable_quantity_;
    const auto modified =
      Traits::to_replacement(contradicted.ref_, reprice(contradicted, Decimal64(69000, 0)));
    REQUIRE_FALSE(modified);
    CHECK(modified.error().broker_code_ == "invalid_trigger_price");
    const auto canceled = Traits::to_cancellation(contradicted.ref_, cancel_all(contradicted));
    REQUIRE(canceled);
    CHECK_FALSE(canceled.value().condition_price_);
  }

  SECTION("the history listing reports the trigger from cond_uv") {
    const auto row = kiwoom::KiwoomClientTestAccess::history_row(
      json::parse(
        R"({"ord_no":"0000050","stk_cd":"A005930","trde_tp":"스톱지정가","crd_tp":"보통매매",
            "ord_qty":"0000000010","ord_uv":"0000069000","cnfm_qty":"0000000000","acpt_tp":"접수",
            "rsrv_tp":"","ord_tm":"13:05:43","ori_ord":"0000000","stk_nm":"삼성전자",
            "io_tp_nm":"현금매도","loan_dt":"","cntr_qty":"0000000000","cntr_uv":"0000000000",
            "ord_remnq":"0000000010","comm_ord_tp":"영웅문4","mdfy_cncl":"","cnfm_tm":"",
            "dmst_stex_tp":"KRX","cond_uv":"0000069500"})"
      ).as_object(),
      Date{2026, 3, 28}
    );
    CHECK(row.type_ == OrderType::STOP_LIMIT);
    CHECK(row.trigger_price_ == Decimal64(69500, 0));
  }
}
