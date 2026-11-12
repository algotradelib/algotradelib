#include <algotradelib/backtest>
#include <algotradelib/mock>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <cerrno>

#include <dlfcn.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// While set, `fsync` fails as a disk that cannot confirm a write does: on every descriptor, or on folders only.
bool fail_fsync = false;
bool fail_folder_fsync = false;

} // namespace

// Replaces the C library's `fsync` in this test program only, so a flush can fail after a write succeeded.
extern "C" int fsync(int descriptor) {
  struct stat status{};
  const bool folder = ::fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode);
  if (fail_fsync || (fail_folder_fsync && folder)) {
    errno = EIO;
    return -1;
  }
  using Sync = int (*)(int);
  static const auto real = reinterpret_cast<Sync>(::dlsym(RTLD_NEXT, "fsync"));
  if (real == nullptr) {
    std::fputs("test-mock-file: the C library's fsync was not found\n", stderr);
    std::abort();
  }
  return real(descriptor);
}
#endif

using namespace algotradelib;

namespace {

using Clock = std::chrono::system_clock;

/** An instant in UTC. */
Clock::time_point utc(int year, int month, int day, int hour, int minute) {
  const long long a = (month <= 2) ? year - 1 : year;
  const long long m = (month <= 2) ? month + 12 : month;
  const long long era_days = 365 * a + a / 4 - a / 100 + a / 400 + (153 * (m - 3) + 2) / 5 + day - 1;
  return Clock::time_point{} + std::chrono::hours((era_days - 719468) * 24 + hour) + std::chrono::minutes(minute);
}

Decimal64 dec(const char* text) {
  return Decimal64::parse(text);
}

InstrumentId samsung() {
  return InstrumentId{Country::KR, Venue::COMBINED, "005930"};
}

InstrumentId hynix() {
  return InstrumentId{Country::KR, Venue::COMBINED, "000660"};
}

Bar bar(Clock::time_point at, const char* open, const char* high, const char* low, const char* close) {
  Bar made;
  made.timestamp_ = at;
  made.open_ = dec(open);
  made.high_ = dec(high);
  made.low_ = dec(low);
  made.close_ = dec(close);
  made.volume_ = dec("1000");
  return made;
}

/** 005930's bars stamped at the close (15:30 KST) and 000660's at the open (00:00 KST), 2026-03-02 to 03-06. */
std::vector<BarSeries> two_instruments() {
  BarSeries first;
  first.instrument_ = samsung();
  first.stamp_ = BarStamp::CLOSE;
  first.currency_ = Currency{"KRW"};
  first.bars_ = {
    bar(utc(2026, 3, 2, 6, 30), "100", "110", "100", "105"),
    bar(utc(2026, 3, 3, 6, 30), "106", "115", "105", "110"),
    bar(utc(2026, 3, 4, 6, 30), "100", "105", "95", "98"),
    bar(utc(2026, 3, 5, 6, 30), "99", "104", "97", "103"),
    bar(utc(2026, 3, 6, 6, 30), "104", "108", "101", "107"),
  };
  BarSeries second;
  second.instrument_ = hynix();
  second.stamp_ = BarStamp::OPEN;
  second.currency_ = Currency{"KRW"};
  second.bars_ = {
    bar(utc(2026, 3, 1, 15, 0), "200", "210", "195", "205"),
    bar(utc(2026, 3, 2, 15, 0), "204", "220", "200", "215"),
    bar(utc(2026, 3, 3, 15, 0), "214", "216", "190", "192"),
    bar(utc(2026, 3, 4, 15, 0), "193", "200", "188", "199"),
    bar(utc(2026, 3, 5, 15, 0), "199", "205", "196", "204"),
  };
  return {first, second};
}

/** Ten million won, a commission with a minimum, slippage and a sell tax. */
mock::MarketSettings costed() {
  mock::MarketSettings settings;
  settings.cash_ = Money{dec("10000000"), Currency{"KRW"}};
  settings.start_ = utc(2026, 3, 2, 6, 30);
  settings.fills_ =
    backtest::NextBarFillModel(
      backtest::CommissionModel::of(dec("0.00015"), dec("10")), backtest::SlippageModel::of_basis_points(dec("5"))
    )
      .with_sell_tax(backtest::SellTax{dec("0.0020"), Country::KR});
  return settings;
}

std::vector<Instrument> references() {
  Instrument reference;
  reference.id_ = samsung();
  reference.name_ = "Samsung Electronics";
  reference.type_ = SecurityType::STOCK;
  reference.status_ = InstrumentStatus::ACTIVE;
  reference.currency_ = Currency{"KRW"};
  reference.lot_size_ = dec("1");
  reference.price_rule_ = PriceRule::unconstrained();
  return {reference};
}

OrderRequest limit_order(const InstrumentId& instrument, OrderSide side, const char* quantity, const char* price) {
  OrderRequest order;
  order.instrument_ = instrument;
  order.side_ = side;
  order.type_ = OrderType::LIMIT;
  order.quantity_ = dec(quantity);
  order.price_ = dec(price);
  return order;
}

OrderRequest market_order(const InstrumentId& instrument, OrderSide side, const char* quantity) {
  OrderRequest order;
  order.instrument_ = instrument;
  order.side_ = side;
  order.type_ = OrderType::MARKET;
  order.quantity_ = dec(quantity);
  return order;
}

/** A run in steps: placements filled and not, refused ones, cancellations and clock moves, through the client and the clock. */
std::vector<std::function<void(mock::Market&)>> steps() {
  return {
    [](mock::Market& market) {
      REQUIRE(market.place(limit_order(samsung(), OrderSide::BUY, "10", "107")));
      // No bar of 000660 has ended: refused, and it takes no id.
      REQUIRE(market.place(market_order(hynix(), OrderSide::BUY, "5")).error().broker_code_ == "no_price");
      REQUIRE(market.place(market_order(samsung(), OrderSide::BUY, "20")));
    },
    [](mock::Market& market) {
      market.advance_to(utc(2026, 3, 2, 16, 0));
      REQUIRE(market.place(market_order(hynix(), OrderSide::BUY, "5")));
      REQUIRE(
        market.place(limit_order(samsung(), OrderSide::BUY, "1000000", "100")).error().broker_code_ ==
        "insufficient_cash"
      );
    },
    [](mock::Market& market) {
      mock::MarketClock(market).wait_for(std::chrono::hours(24));
      REQUIRE(market.place(limit_order(samsung(), OrderSide::SELL, "5", "120")));
      REQUIRE(market.place(limit_order(samsung(), OrderSide::BUY, "3", "50")));
    },
    [](mock::Market& market) {
      REQUIRE(market.cancel("5"));
      REQUIRE(market.cancel("99").error().broker_code_ == "nothing_to_cancel");
      market.advance_to(utc(2026, 3, 5, 7, 0));
      auto sell = market_order(hynix(), OrderSide::SELL, "5");
      sell.time_in_force_ = TimeInForce::IMMEDIATE_OR_CANCEL;
      REQUIRE(market.place(sell));
      mock::MockClient client(market);
      REQUIRE(client.place(market_order(samsung(), OrderSide::SELL, "10")));
    },
    [](mock::Market& market) {
      market.advance_to(utc(2026, 3, 7, 0, 0));
    },
  };
}

/** Checks that two markets hold the same account, clock and orders. */
void check_same(const mock::Market& kept, const mock::Market& plain) {
  CHECK(kept.now() == plain.now());
  CHECK(kept.halted() == plain.halted());
  CHECK(kept.cash() == plain.cash());
  const auto kept_power = kept.buying_power(Currency{"KRW"});
  const auto plain_power = plain.buying_power(Currency{"KRW"});
  REQUIRE(kept_power);
  REQUIRE(plain_power);
  CHECK(kept_power.value().cash_ == plain_power.value().cash_);
  const auto kept_orders = kept.order_history({}).orders_;
  const auto plain_orders = plain.order_history({}).orders_;
  REQUIRE(kept_orders.size() == plain_orders.size());
  for (std::size_t at = 0; at < kept_orders.size(); ++at) {
    const auto& a = kept_orders[at];
    const auto& b = plain_orders[at];
    CHECK(a.order_id_ == b.order_id_);
    CHECK(a.instrument_ == b.instrument_);
    CHECK(a.side_ == b.side_);
    CHECK(a.status_ == b.status_);
    CHECK(a.quantity_ == b.quantity_);
    CHECK(a.price_ == b.price_);
    CHECK(a.ordered_at_ == b.ordered_at_);
    CHECK(a.canceled_at_ == b.canceled_at_);
    CHECK(a.execution_.filled_quantity_ == b.execution_.filled_quantity_);
    CHECK(a.execution_.average_filled_price_ == b.execution_.average_filled_price_);
    CHECK(a.execution_.filled_at_ == b.execution_.filled_at_);
    CHECK(a.execution_.commission_ == b.execution_.commission_);
    CHECK(a.execution_.tax_ == b.execution_.tax_);
  }
  const auto kept_holdings = kept.holdings();
  const auto plain_holdings = plain.holdings();
  REQUIRE(kept_holdings);
  REQUIRE(plain_holdings);
  const auto& kept_rows = kept_holdings.value().positions_;
  const auto& plain_rows = plain_holdings.value().positions_;
  REQUIRE(kept_rows.size() == plain_rows.size());
  for (std::size_t at = 0; at < kept_rows.size(); ++at) {
    CHECK(kept_rows[at].instrument_ == plain_rows[at].instrument_);
    CHECK(kept_rows[at].quantity_ == plain_rows[at].quantity_);
    CHECK(kept_rows[at].average_purchase_price_ == plain_rows[at].average_purchase_price_);
    CHECK(kept_rows[at].market_value_ == plain_rows[at].market_value_);
  }
  const auto kept_working = kept.working_orders().orders_;
  const auto plain_working = plain.working_orders().orders_;
  REQUIRE(kept_working.size() == plain_working.size());
  for (std::size_t at = 0; at < kept_working.size(); ++at) {
    CHECK(kept_working[at].order_.order_id_ == plain_working[at].order_.order_id_);
  }
}

/** A file in a folder of its own under the temporary folder, named for the process and the case; removed after. */
class TempFile {
public:
  explicit TempFile(const char* name)
  : folder_(
      std::filesystem::temp_directory_path() / ("atl-test-mock-file-" + std::to_string(process_id()) + "-" + name)
    ) {
    std::filesystem::remove_all(folder_);
    std::filesystem::create_directories(folder_);
    path_ = (folder_ / "market.state").string();
  }

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove_all(folder_, ignored);
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  const std::string& str() const noexcept {
    return path_;
  }

private:
  static long process_id() {
#if defined(_WIN32)
    return static_cast<long>(::_getpid());
#else
    return static_cast<long>(::getpid());
#endif
  }

  std::filesystem::path folder_;
  std::string path_;
};

std::string bytes_of(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_bytes(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

/** Opens the file over the two instruments with costs, and requires that it opened. */
std::unique_ptr<mock::MarketFile> kept(const TempFile& file) {
  auto opened = std::make_unique<mock::MarketFile>(file.str(), two_instruments(), costed(), references());
  INFO(opened->rejection());
  REQUIRE(opened->open());
  return opened;
}

/** A file holding the first step's changes. */
std::string started(const TempFile& file) {
  steps()[0](kept(file)->market());
  return bytes_of(file.str());
}

} // namespace

TEST_CASE("two mock markets given the same calls agree", "[mock][file]") {
  mock::Market first(two_instruments(), costed(), references());
  mock::Market second(two_instruments(), costed(), references());
  for (const auto& step : steps()) {
    step(first);
    step(second);
    check_same(first, second);
  }
  // The run fills, cancels and expires orders in both instruments.
  const auto orders = first.order_history({}).orders_;
  REQUIRE(orders.size() == 7);
  CHECK(orders[0].status_ == OrderStatus::FILLED);
  CHECK(orders[2].status_ == OrderStatus::FILLED);
  CHECK(orders[3].status_ == OrderStatus::CANCELED);
  CHECK(orders[4].status_ == OrderStatus::CANCELED);
  CHECK(orders[5].status_ == OrderStatus::FILLED);
  CHECK(orders[6].status_ == OrderStatus::FILLED);
}

TEST_CASE("a mock market reopened from its file equals one never closed", "[mock][file]") {
  const TempFile file("reopen");
  mock::Market plain(two_instruments(), costed(), references());
  for (const auto& step : steps()) {
    step(plain);
    {
      auto opened = kept(file);
      step(opened->market());
      check_same(opened->market(), plain);
    }
    // Opened again, the file replays to the same market.
    check_same(kept(file)->market(), plain);
  }
  CHECK(kept(file)->market().cash() == plain.cash());
}

TEST_CASE("a mock market file holds one header and one line per change", "[mock][file]") {
  const TempFile file("lines");
  const auto text = started(file);
  // The header and two placements: the refused one is not recorded.
  std::size_t lines = 0;
  for (const char ch : text) {
    lines += ch == '\n' ? 1 : 0;
  }
  CHECK(lines == 3);
  CHECK(text.rfind("{\"mock_market\":1,\"semantics\":1,\"fingerprint\":\"", 0) == 0);
  CHECK(text.back() == '\n');
  // A read, a refused call and a move to the same instant record nothing.
  {
    auto opened = kept(file);
    auto& market = opened->market();
    CHECK(market.holdings());
    CHECK_FALSE(market.cancel("99"));
    market.advance_to(market.now());
  }
  CHECK(bytes_of(file.str()) == text);
}

TEST_CASE("a mock market file of another market is refused and left as it was", "[mock][file]") {
  const TempFile file("other-market");
  const auto text = started(file);
  using Change = std::function<void(std::vector<BarSeries>&, mock::MarketSettings&, std::vector<Instrument>&)>;
  const std::vector<std::pair<const char*, Change>> changes = {
    {"a close",
      [](std::vector<BarSeries>& bars, mock::MarketSettings&, std::vector<Instrument>&) {
        bars[0].bars_[3].close_ = dec("102");
      }},
    {"a volume",
      [](std::vector<BarSeries>& bars, mock::MarketSettings&, std::vector<Instrument>&) {
        bars[1].bars_[0].volume_ = dec("999");
      }},
    {"a bar more",
      [](std::vector<BarSeries>& bars, mock::MarketSettings&, std::vector<Instrument>&) {
        bars[0].bars_.push_back(bar(utc(2026, 3, 9, 6, 30), "107", "108", "106", "107"));
      }},
    {"the series' order",
      [](std::vector<BarSeries>& bars, mock::MarketSettings&, std::vector<Instrument>&) {
        std::swap(bars[0], bars[1]);
      }},
    {"a stamp",
      [](std::vector<BarSeries>& bars, mock::MarketSettings&, std::vector<Instrument>&) {
        bars[1].stamp_ = BarStamp::CLOSE;
      }},
    {"the cash",
      [](std::vector<BarSeries>&, mock::MarketSettings& settings, std::vector<Instrument>&) {
        settings.cash_.amount_ = dec("10000001");
      }},
    {"the start",
      [](std::vector<BarSeries>&, mock::MarketSettings& settings, std::vector<Instrument>&) {
        settings.start_ += std::chrono::seconds(1);
      }},
    {"the session",
      [](std::vector<BarSeries>&, mock::MarketSettings& settings, std::vector<Instrument>&) {
        settings.session_.close_ += std::chrono::minutes(1);
      }},
    {"the calendar",
      [](std::vector<BarSeries>&, mock::MarketSettings& settings, std::vector<Instrument>&) {
        TradingCalendar calendar;
        calendar.days_ = {TradingDay{Date{2026, 3, 2}, true}};
        settings.calendar_ = calendar;
      }},
    {"the commission's minimum",
      [](std::vector<BarSeries>&, mock::MarketSettings& settings, std::vector<Instrument>&) {
        settings.fills_ =
          backtest::NextBarFillModel(
            backtest::CommissionModel::of(dec("0.00015"), dec("11")), settings.fills_.slippage()
          )
            .with_sell_tax(*settings.fills_.sell_tax());
      }},
    {"the slippage",
      [](std::vector<BarSeries>&, mock::MarketSettings& settings, std::vector<Instrument>&) {
        settings.fills_ =
          backtest::NextBarFillModel(settings.fills_.commission(), backtest::SlippageModel::of_basis_points(dec("6")))
            .with_sell_tax(*settings.fills_.sell_tax());
      }},
    {"the sell tax",
      [](std::vector<BarSeries>&, mock::MarketSettings& settings, std::vector<Instrument>&) {
        settings.fills_ = settings.fills_.with_sell_tax(backtest::SellTax{dec("0.0021"), Country::KR});
      }},
    {"the market-price rule",
      [](std::vector<BarSeries>&, mock::MarketSettings& settings, std::vector<Instrument>&) {
        settings.fills_ = settings.fills_.with_market_price(backtest::MarketPrice::DECISION_CLOSE);
      }},
    {"the limit price",
      [](std::vector<BarSeries>&, mock::MarketSettings& settings, std::vector<Instrument>&) {
        settings.fills_ =
          backtest::NextBarFillModel(
            settings.fills_.commission(), settings.fills_.slippage(), backtest::LimitFillPrice::AT_OPEN_IF_BETTER
          )
            .with_sell_tax(*settings.fills_.sell_tax());
      }},
    {"an instrument's lot",
      [](std::vector<BarSeries>&, mock::MarketSettings&, std::vector<Instrument>& instruments) {
        instruments[0].lot_size_ = dec("10");
      }},
    {"an instrument's price grid",
      [](std::vector<BarSeries>&, mock::MarketSettings&, std::vector<Instrument>& instruments) {
        instruments[0].price_rule_.kind_ = PriceRule::Kind::STATED;
        instruments[0].price_rule_.bands_ = {TickBand{dec("0"), dec("1")}};
      }},
    {"an instrument's name",
      [](std::vector<BarSeries>&, mock::MarketSettings&, std::vector<Instrument>& instruments) {
        instruments[0].name_ = "Samsung";
      }},
    {"reference data given for the other instrument",
      [](std::vector<BarSeries>&, mock::MarketSettings&, std::vector<Instrument>& instruments) {
        instruments[0].id_ = hynix();
      }},
  };
  for (const auto& [name, change] : changes) {
    DYNAMIC_SECTION(name) {
      auto bars = two_instruments();
      auto settings = costed();
      auto instruments = references();
      change(bars, settings, instruments);
      mock::MarketFile other(file.str(), bars, settings, instruments);
      CHECK_FALSE(other.open());
      CHECK(other.rejection().find("belongs to another market") != std::string::npos);
      CHECK(bytes_of(file.str()) == text);
    }
  }
  // Untouched, it opens.
  CHECK(kept(file)->market().order_history({}).orders_.size() == 2);
}

TEST_CASE("a mock market file compares the whole commission schedule but not its name", "[mock][file]") {
  const TempFile file("schedule");
  struct Schedule {
    const char* rate_{"0.00015"};
    const char* institution_{"0.00004"};
    Date until_{2026, 12, 31};
    Venue venue_{Venue::KRX};
    Country dating_{Country::KR};
    const char* tariff_{"online"};
  };
  const auto settings_with = [](const Schedule& stated) {
    CommissionRate rate;
    rate.country_ = Country::KR;
    rate.venue_ = *VenueKey::normalized(Venue::KRX);
    rate.rate_ = dec(stated.rate_);
    rate.institution_rate_ = dec(stated.institution_);
    rate.until_ = stated.until_;
    CommissionSchedule schedule;
    schedule.origin_ = CommissionOrigin::CALLER_STATED;
    schedule.tariff_ = stated.tariff_;
    schedule.rates_ = {rate};
    auto policy = backtest::CommissionPolicy::scheduled(schedule, *VenueKey::normalized(stated.venue_), stated.dating_);
    REQUIRE(policy);
    auto settings = costed();
    settings.fills_ = backtest::NextBarFillModel(std::move(policy.value()));
    return settings;
  };
  {
    mock::MarketFile first(file.str(), two_instruments(), settings_with(Schedule{}));
    REQUIRE(first.open());
    REQUIRE(first.market().place(limit_order(samsung(), OrderSide::BUY, "1", "107")));
  }
  const auto text = bytes_of(file.str());
  const auto opens = [&](const Schedule& stated) {
    mock::MarketFile again(file.str(), two_instruments(), settings_with(stated));
    CHECK(bytes_of(file.str()) == text);
    return again.open();
  };
  Schedule rate;
  rate.rate_ = "0.00016";
  Schedule institution;
  institution.institution_ = "0.00005";
  Schedule bound;
  bound.until_ = Date{2026, 12, 30};
  Schedule venue;
  venue.venue_ = Venue::NXT;
  Schedule dating;
  dating.dating_ = Country::US;
  Schedule name;
  name.tariff_ = "branch";
  CHECK_FALSE(opens(rate));
  CHECK_FALSE(opens(institution));
  CHECK_FALSE(opens(bound));
  CHECK_FALSE(opens(venue));
  CHECK_FALSE(opens(dating));
  CHECK(opens(name));
  CHECK(opens(Schedule{}));
}

TEST_CASE("a mock market file of another format or rules or no such file is refused", "[mock][file]") {
  const TempFile file("other-format");
  const auto text = started(file);
  const auto refused = [&](const std::string& changed, const char* reason) {
    write_bytes(file.str(), changed);
    mock::MarketFile other(file.str(), two_instruments(), costed(), references());
    CHECK_FALSE(other.open());
    CHECK(other.rejection().find(reason) != std::string::npos);
    CHECK(bytes_of(file.str()) == changed);
    // A file that did not open takes no change.
    CHECK(
      other.market().place(limit_order(samsung(), OrderSide::BUY, "1", "100")).error().broker_code_ ==
      "state_not_written"
    );
    CHECK_THROWS_AS(other.market().advance_to(utc(2026, 3, 3, 0, 0)), mock::MarketFileError);
    CHECK(bytes_of(file.str()) == changed);
  };
  const auto replaced = [&](const std::string& from, const std::string& to) {
    auto changed = text;
    const auto at = changed.find(from);
    REQUIRE(at != std::string::npos);
    return changed.replace(at, from.size(), to);
  };
  SECTION("another format") {
    refused(replaced("\"mock_market\":1", "\"mock_market\":2"), "another format");
  }
  SECTION("other market rules") {
    refused(replaced("\"semantics\":1", "\"semantics\":2"), "other market rules");
  }
  SECTION("a file of something else") {
    refused("hello\n", "not a mock market's state file");
    refused("hello", "not a mock market's state file");
  }
  SECTION("a line that does not replay") {
    refused(replaced("\"place\"", "\"plaec\""), "does not replay");
  }
}

TEST_CASE("a mock market's file refuses a caller's own price function", "[mock][file]") {
  const TempFile file("own-price");
  auto settings = costed();
  settings.fills_ = settings.fills_.with_market_price("decision-close", [](const backtest::MarketPriceInput& in) {
    return in.decision_price_;
  });
  {
    mock::MarketFile refused(file.str(), two_instruments(), settings, references());
    CHECK_FALSE(refused.open());
    CHECK(refused.rejection().find("caller's own function") != std::string::npos);
  }
  // Nothing was created.
  CHECK_FALSE(std::ifstream(file.str()).good());
  const auto text = started(file);
  mock::MarketFile refused(file.str(), two_instruments(), settings, references());
  CHECK_FALSE(refused.open());
  CHECK(bytes_of(file.str()) == text);
}

TEST_CASE("a line cut short is removed and never replayed", "[mock][file]") {
  const TempFile file("torn");
  const auto text = started(file);
  SECTION("after the header") {
    // A whole placement but for its newline, as a killed writer leaves it.
    write_bytes(
      file.str(),
      text + "{\"at\":" + std::to_string(utc(2026, 3, 2, 6, 30).time_since_epoch().count()) + ",\"cancel\":\"1\"}"
    );
    {
      auto opened = kept(file);
      CHECK(opened->market().working_orders().orders_.size() == 2);
    }
    CHECK(bytes_of(file.str()) == text);
  }
  SECTION("in the header") {
    write_bytes(file.str(), text.substr(0, 20));
    {
      auto opened = kept(file);
      CHECK(opened->market().order_history({}).orders_.empty());
    }
    CHECK(bytes_of(file.str()) == text.substr(0, text.find('\n') + 1));
  }
}

TEST_CASE("a kept mock market can be copied without its file but not assigned to", "[mock][file]") {
  const TempFile file("copy");
  const auto text = started(file);
  auto opened = kept(file);
  mock::Market copy = opened->market();
  REQUIRE(copy.place(limit_order(samsung(), OrderSide::BUY, "1", "100")));
  copy.advance_to(utc(2026, 3, 4, 0, 0));
  CHECK(bytes_of(file.str()) == text);
  CHECK_THROWS_AS(opened->market() = copy, std::logic_error);
  CHECK_THROWS_AS(opened->market() = mock::Market(two_instruments(), costed()), std::logic_error);
  CHECK(opened->market().order_history({}).orders_.size() == 2);
  // An ordinary market takes a copy of the kept one.
  mock::Market plain(two_instruments(), costed());
  plain = opened->market();
  CHECK(plain.order_history({}).orders_.size() == 2);
}

TEST_CASE("a kept mock market cannot be moved from", "[mock][file]") {
  const TempFile file("move");
  const auto text = started(file);
  auto opened = kept(file);
  auto& market = opened->market();
  const auto unchanged = [&] {
    CHECK(market.order_history({}).orders_.size() == 2);
    CHECK(market.cash() == Money{dec("10000000"), Currency{"KRW"}});
    CHECK(market.instrument(samsung()));
    CHECK(bytes_of(file.str()) == text);
  };
  CHECK_THROWS_AS(mock::Market(std::move(market)), std::logic_error);
  unchanged();
  mock::Market plain(two_instruments(), costed());
  CHECK_THROWS_AS(plain = std::move(market), std::logic_error);
  unchanged();
  CHECK_THROWS_AS(std::swap(market, plain), std::logic_error);
  unchanged();
  // Still the file's market: it records the next change.
  REQUIRE(market.place(limit_order(samsung(), OrderSide::BUY, "1", "100")));
  CHECK(bytes_of(file.str()).size() > text.size());
  // An ordinary market moves as before.
  mock::Market moved(std::move(plain));
  CHECK(moved.now() == utc(2026, 3, 2, 6, 30));
}

TEST_CASE("a kept mock market that halted reopens halted once at the same bar", "[mock][file]") {
  const TempFile file("halt");
  // The sale's proceeds fit an exact decimal; cash plus them does not.
  BarSeries series;
  series.instrument_ = samsung();
  series.currency_ = Currency{"KRW"};
  series.bars_ = {
    bar(utc(2026, 3, 2, 6, 30), "1", "1", "1", "1"),
    bar(utc(2026, 3, 3, 6, 30), "1", "1", "1", "1"),
    bar(utc(2026, 3, 4, 6, 30), "1000000000000000000", "1000000000000000000", "1", "1"),
  };
  mock::MarketSettings settings;
  settings.cash_ = Money{dec("9000000000000000000"), Currency{"KRW"}};
  settings.start_ = utc(2026, 3, 2, 6, 30);
  std::optional<std::string> reason;
  {
    mock::MarketFile opened(file.str(), {series}, settings);
    REQUIRE(opened.open());
    auto& market = opened.market();
    REQUIRE(market.place(market_order(samsung(), OrderSide::BUY, "1")));
    market.advance_to(utc(2026, 3, 3, 6, 30));
    REQUIRE(market.place(limit_order(samsung(), OrderSide::BUY, "1", "1")));
    REQUIRE(market.place(market_order(samsung(), OrderSide::SELL, "1")));
    CHECK_THROWS_AS(market.advance_to(utc(2026, 3, 5, 6, 30)), std::runtime_error);
    REQUIRE(market.halted());
    reason = market.halted();
    REQUIRE(market.cancel("3"));
  }
  const auto text = bytes_of(file.str());
  for (int round = 0; round < 2; ++round) {
    mock::MarketFile opened(file.str(), {series}, settings);
    INFO(opened.rejection());
    REQUIRE(opened.open());
    const auto& market = opened.market();
    CHECK(market.halted() == reason);
    CHECK(market.now() == utc(2026, 3, 4, 6, 30));
    const auto orders = market.order_history({}).orders_;
    REQUIRE(orders.size() == 3);
    CHECK(orders[0].status_ == OrderStatus::FILLED);
    // The earlier order on the bar that halted filled, and stands.
    CHECK(orders[1].status_ == OrderStatus::FILLED);
    CHECK(orders[1].execution_.filled_at_ == utc(2026, 3, 4, 6, 30));
    CHECK(orders[2].status_ == OrderStatus::CANCELED);
    CHECK(orders[2].canceled_at_ == utc(2026, 3, 4, 6, 30));
    const auto holdings = market.holdings();
    REQUIRE(holdings);
    CHECK(holdings.value().positions_.front().quantity_ == dec("2"));
    // Opening writes nothing, and the halted market records nothing it refuses.
    CHECK_THROWS_AS(opened.market().advance_to(utc(2026, 3, 6, 0, 0)), std::runtime_error);
    CHECK(opened.market().place(market_order(samsung(), OrderSide::BUY, "1")).error().broker_code_ == "market_halted");
    CHECK(bytes_of(file.str()) == text);
  }
}

TEST_CASE("the mock market reads its cash before reservations", "[mock][file]") {
  mock::Market market(two_instruments(), costed(), references());
  REQUIRE(market.place(limit_order(samsung(), OrderSide::BUY, "10", "100")));
  CHECK(market.cash() == Money{dec("10000000"), Currency{"KRW"}});
  CHECK(market.buying_power(Currency{"KRW"}).value().cash_.amount_ == dec("9999000"));
}

#if !defined(_WIN32)

namespace {

/** Lowers the process's file-size limit, ignoring the signal that would end it, until destroyed. */
class FileSizeLimit {
public:
  explicit FileSizeLimit(rlim_t bytes) {
    REQUIRE(::getrlimit(RLIMIT_FSIZE, &saved_) == 0);
    previous_ = ::signal(SIGXFSZ, SIG_IGN);
    auto lowered = saved_;
    lowered.rlim_cur = bytes;
    REQUIRE(::setrlimit(RLIMIT_FSIZE, &lowered) == 0);
  }

  ~FileSizeLimit() {
    ::setrlimit(RLIMIT_FSIZE, &saved_);
    ::signal(SIGXFSZ, previous_);
  }

  FileSizeLimit(const FileSizeLimit&) = delete;
  FileSizeLimit& operator=(const FileSizeLimit&) = delete;

private:
  struct rlimit saved_{};
  void (*previous_)(int){SIG_DFL};
};

/** Lets `fsync` work again when a test leaves, however it leaves. */
class SyncRestored {
public:
  SyncRestored() = default;

  ~SyncRestored() {
    fail_fsync = false;
    fail_folder_fsync = false;
  }

  SyncRestored(const SyncRestored&) = delete;
  SyncRestored& operator=(const SyncRestored&) = delete;
};

/** Checks that a market whose file failed refuses every change and leaves the file as it is. */
void check_refuses_changes(mock::MarketFile& opened) {
  const auto text = bytes_of(opened.path());
  auto& market = opened.market();
  CHECK(opened.failed());
  const auto placed = market.place(limit_order(samsung(), OrderSide::BUY, "1", "100"));
  REQUIRE_FALSE(placed);
  CHECK(placed.error().broker_code_ == "state_not_written");
  CHECK_FALSE(placed.error().reached_broker_);
  CHECK(market.cancel("1").error().broker_code_ == "state_not_written");
  try {
    market.advance_to(market.now() + std::chrono::hours(1));
    FAIL("the clock moved");
  } catch (const mock::MarketFileError& error) {
    CHECK(error.kind() == mock::MarketFileError::Kind::NOT_WRITTEN);
  }
  CHECK(bytes_of(opened.path()) == text);
}

} // namespace

TEST_CASE("a mock market line that cannot be written whole is not carried out", "[mock][file]") {
  const TempFile file("short-write");
  const auto text = started(file);
  SECTION("part of the line") {
    {
      auto opened = kept(file);
      const auto before = opened->market().order_history({}).orders_.size();
      {
        const FileSizeLimit limit(text.size() + 5);
        const auto placed = opened->market().place(limit_order(samsung(), OrderSide::BUY, "1", "100"));
        REQUIRE_FALSE(placed);
        CHECK(placed.error().category_ == ErrorCategory::TRANSPORT);
        CHECK_FALSE(placed.error().reached_broker_);
        CHECK(placed.error().broker_code_ == "state_not_written");
      }
      CHECK(opened->market().order_history({}).orders_.size() == before);
      CHECK(bytes_of(file.str()).size() == text.size() + 5);
      check_refuses_changes(*opened);
    }
    // Reopened, the part is removed and the placement never happened.
    auto opened = kept(file);
    CHECK(opened->market().order_history({}).orders_.size() == 2);
    CHECK(bytes_of(file.str()) == text);
  }
  SECTION("a clock move, none of it") {
    auto opened = kept(file);
    {
      const FileSizeLimit limit(text.size());
      try {
        opened->market().advance_to(utc(2026, 3, 4, 0, 0));
        FAIL("the clock moved");
      } catch (const mock::MarketFileError& error) {
        CHECK(error.kind() == mock::MarketFileError::Kind::NOT_WRITTEN);
      }
    }
    CHECK(opened->market().now() == utc(2026, 3, 2, 6, 30));
    check_refuses_changes(*opened);
  }
}

TEST_CASE("a mock market line written but not flushed may be carried out on reopening", "[mock][file]") {
  const SyncRestored restored;
  const TempFile file("failed-flush");
  started(file);
  SECTION("a placement") {
    {
      auto opened = kept(file);
      fail_fsync = true;
      const auto placed = opened->market().place(limit_order(samsung(), OrderSide::BUY, "1", "100"));
      fail_fsync = false;
      REQUIRE_FALSE(placed);
      CHECK(placed.error().category_ == ErrorCategory::TRANSPORT);
      CHECK(placed.error().reached_broker_);
      CHECK(placed.error().broker_code_ == "state_unconfirmed");
      // Not carried out here.
      CHECK(opened->market().order_history({}).orders_.size() == 2);
      check_refuses_changes(*opened);
    }
    // The line was written whole, so reopening carries it out.
    const auto orders = kept(file)->market().order_history({}).orders_;
    REQUIRE(orders.size() == 3);
    CHECK(orders[2].price_ == dec("100"));
  }
  SECTION("a clock move") {
    {
      auto opened = kept(file);
      fail_fsync = true;
      try {
        opened->market().advance_to(utc(2026, 3, 4, 0, 0));
        FAIL("the clock moved");
      } catch (const mock::MarketFileError& error) {
        CHECK(error.kind() == mock::MarketFileError::Kind::UNCONFIRMED);
      }
      fail_fsync = false;
      CHECK(opened->market().now() == utc(2026, 3, 2, 6, 30));
      check_refuses_changes(*opened);
    }
    CHECK(kept(file)->market().now() == utc(2026, 3, 4, 0, 0));
  }
  SECTION("a new file's header") {
    const TempFile fresh("failed-flush-header");
    fail_fsync = true;
    mock::MarketFile opened(fresh.str(), two_instruments(), costed());
    fail_fsync = false;
    CHECK_FALSE(opened.open());
    CHECK(opened.rejection().find("could not be started") != std::string::npos);
  }
}

TEST_CASE("a mock market file another market holds is refused", "[mock][file]") {
  const TempFile file("locked");
  const auto text = started(file);
  SECTION("in this process") {
    auto first = kept(file);
    mock::MarketFile second(file.str(), two_instruments(), costed(), references());
    CHECK_FALSE(second.open());
    CHECK(second.rejection().find("open in another market") != std::string::npos);
  }
  SECTION("in another process") {
    int ready[2];
    int release[2];
    REQUIRE(::pipe(ready) == 0);
    REQUIRE(::pipe(release) == 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
      // The child holds the file until the parent has tried, then leaves without unwinding into Catch2.
      ::close(ready[0]);
      ::close(release[1]);
      try {
        mock::MarketFile holder(file.str(), two_instruments(), costed(), references());
        const char held = holder.open() ? 'y' : 'n';
        (void)!::write(ready[1], &held, 1);
        char done = 0;
        (void)!::read(release[0], &done, 1);
      } catch (...) {
        ::_exit(1);
      }
      ::_exit(0);
    }
    ::close(ready[1]);
    ::close(release[0]);
    char held = 0;
    const auto got = ::read(ready[0], &held, 1);
    bool refused = false;
    std::string why;
    {
      mock::MarketFile parent(file.str(), two_instruments(), costed(), references());
      refused = !parent.open();
      why = parent.rejection();
    }
    const char done = 'x';
    (void)!::write(release[1], &done, 1);
    int status = 0;
    ::waitpid(child, &status, 0);
    ::close(ready[0]);
    ::close(release[1]);
    REQUIRE(got == 1);
    CHECK(held == 'y');
    CHECK(refused);
    CHECK(why.find("open in another market") != std::string::npos);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(bytes_of(file.str()) == text);
    // The child is gone, and its lock with it.
    CHECK(kept(file)->open());
  }
}

TEST_CASE("a mock market file is flushed with its folder before it is opened", "[mock][file]") {
  const SyncRestored restored;
  SECTION("a folder that cannot be flushed, from the first open on") {
    const TempFile file("folder-flush");
    fail_folder_fsync = true;
    {
      mock::MarketFile first(file.str(), two_instruments(), costed());
      CHECK_FALSE(first.open());
      CHECK(first.rejection().find("could not be started") != std::string::npos);
    }
    // The header was written whole, and its name is still not known to be on the disk.
    const auto text = bytes_of(file.str());
    REQUIRE(text.back() == '\n');
    {
      mock::MarketFile again(file.str(), two_instruments(), costed());
      CHECK_FALSE(again.open());
      CHECK(again.rejection().find("could not be flushed") != std::string::npos);
      CHECK(
        again.market().place(limit_order(samsung(), OrderSide::BUY, "1", "100")).error().broker_code_ ==
        "state_not_written"
      );
    }
    fail_folder_fsync = false;
    CHECK(bytes_of(file.str()) == text);
    mock::MarketFile last(file.str(), two_instruments(), costed());
    CHECK(last.open());
  }
  SECTION("a change written and not flushed by an earlier process") {
    const TempFile file("recovered-flush");
    started(file);
    {
      auto opened = kept(file);
      fail_fsync = true;
      CHECK_THROWS_AS(opened->market().advance_to(utc(2026, 3, 4, 0, 0)), mock::MarketFileError);
    }
    const auto text = bytes_of(file.str());
    {
      mock::MarketFile again(file.str(), two_instruments(), costed(), references());
      CHECK_FALSE(again.open());
      CHECK(again.rejection().find("could not be flushed") != std::string::npos);
    }
    fail_fsync = false;
    CHECK(bytes_of(file.str()) == text);
    // Flushed, it stands, once.
    CHECK(kept(file)->market().now() == utc(2026, 3, 4, 0, 0));
    CHECK(kept(file)->market().now() == utc(2026, 3, 4, 0, 0));
    std::size_t moves = 0;
    for (auto at = text.find("\"advance\""); at != std::string::npos; at = text.find("\"advance\"", at + 1)) {
      ++moves;
    }
    CHECK(moves == 1);
    CHECK(bytes_of(file.str()) == text);
  }
}

#endif
