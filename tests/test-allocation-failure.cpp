// What a failed allocation leaves behind in the lookup tables a wide replay builds. This executable replaces the
// global allocator so a test can make its n-th allocation fail, and walks n from the first allocation to the
// last one the operation makes.

#include <algotradelib/backtest>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace {

// How many more allocations succeed before one fails; negative means none fails.
long allocations_left = -1;

void* allocate(std::size_t size) {
  if (allocations_left == 0) {
    allocations_left = -1;
    throw std::bad_alloc();
  }
  if (allocations_left > 0) {
    --allocations_left;
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace algotradelib;

namespace {

/**
 * Runs `operation` with its n-th allocation failing, for n = 0, 1, ... until it completes; calls `check` after
 * each failure. Returns how many failures were tried.
 */
template <typename Operation, typename Check>
int fail_each_allocation(Operation operation, Check check) {
  for (int n = 0;; ++n) {
    allocations_left = n;
    try {
      operation();
      allocations_left = -1;
      return n;
    } catch (const std::bad_alloc&) {
      allocations_left = -1;
      check();
    }
  }
}

InstrumentId named(const std::string& symbol) { return InstrumentId{Country::KR, Venue::COMBINED, symbol}; }

Currency krw() { return Currency{"KRW"}; }

backtest::Execution bought(const InstrumentId& id, const char* quantity) {
  backtest::Execution execution;
  execution.fill_.instrument_ = id;
  execution.fill_.side_ = OrderSide::BUY;
  execution.fill_.quantity_ = Decimal64::parse(quantity);
  execution.fill_.price_ = Decimal64::parse("1000");
  execution.commission_ = Decimal64::parse("0");
  execution.slippage_ = Decimal64::parse("0");
  return execution;
}

backtest::ReplayLedger holding(const std::string& prefix, int lots) {
  backtest::ReplayLedger ledger(Decimal64::parse("9000000000000000"), krw());
  for (int k = 0; k < lots; ++k) {
    REQUIRE(ledger.apply(bought(named(prefix + std::to_string(k)), "1"), krw()));
  }
  return ledger;
}

// Every lot the ledger lists is the one its lookups find.
void require_lookups_match_positions(const backtest::ReplayLedger& ledger) {
  for (const auto& lot : ledger.positions()) {
    REQUIRE(ledger.quantity_of(lot.instrument_) == std::optional<Decimal64>(lot.quantity_));
  }
}

// Equity as a scan of every lot computes it: the flat ones skipped, the rest multiplied and added in order.
std::optional<Decimal64> equity_by_scan(const backtest::ReplayLedger& ledger) {
  auto total = ledger.cash_in(krw()).value_or(Decimal64::parse("0"));
  for (const auto& lot : ledger.positions()) {
    if (lot.quantity_.coefficient() == 0) {
      continue;
    }
    const auto value = decimal_multiply(lot.quantity_, lot.last_price_);
    const auto sum = value ? decimal_add(total, *value) : std::nullopt;
    if (!sum) {
      return std::nullopt;
    }
    total = *sum;
  }
  return total;
}

void require_equity_as_scanned(const backtest::ReplayLedger& ledger) {
  const auto scanned = equity_by_scan(ledger);
  // Both the full sum and the running total the replay reads.
  for (const auto& equity : {ledger.equity_in(krw()), ledger.equity()}) {
    REQUIRE(equity.has_value() == scanned.has_value());
    if (equity) {
      REQUIRE(equity->coefficient() == scanned->coefficient());
      REQUIRE(equity->scale() == scanned->scale());
    }
  }
}

// A failed `apply` leaves the ledger as it was: the same lots in the same places, cash and executions.
void require_unchanged(const backtest::ReplayLedger& ledger, const backtest::ReplayLedger& before) {
  REQUIRE(ledger.positions().size() == before.positions().size());
  for (std::size_t at = 0; at < ledger.positions().size(); ++at) {
    REQUIRE(ledger.positions()[at].instrument_ == before.positions()[at].instrument_);
    REQUIRE(ledger.positions()[at].quantity_ == before.positions()[at].quantity_);
    REQUIRE(ledger.positions()[at].last_price_ == before.positions()[at].last_price_);
  }
  REQUIRE(ledger.cash_in(krw()) == before.cash_in(krw()));
  REQUIRE(ledger.executions().size() == before.executions().size());
}

bool lists(const backtest::ReplayLedger& ledger, const InstrumentId& id) {
  for (const auto& lot : ledger.positions()) {
    if (lot.instrument_ == id) {
      return true;
    }
  }
  return false;
}

strategy::BarEvent bar_of(const std::string& symbol, BarInterval interval) {
  Bar bar;
  bar.timestamp_ = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 20000);
  const auto price = Decimal64::parse("100");
  bar.open_ = price;
  bar.high_ = price;
  bar.low_ = price;
  bar.close_ = price;
  bar.volume_ = Decimal64::parse("1");
  return strategy::make_bar_event(named(symbol), interval, BarStamp::CLOSE, bar);
}

// Buys ten of the bar's instrument at market on every bar.
struct BuysTen {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    ctx.submit(
      strategy::OrderIntent::market(bar.instrument_, OrderSide::BUY, strategy::SizeSpec::quantity(Decimal64::parse("10")))
    );
  }
};

std::vector<std::string> symbols_of(const strategy::BarSlice& slice) {
  std::vector<std::string> symbols;
  for (const auto& event : slice.bars_) {
    symbols.push_back(event.instrument_.symbol_);
  }
  return symbols;
}

} // namespace

static_assert(std::is_nothrow_move_assignable<backtest::ReplayLedger>::value);
static_assert(std::is_nothrow_move_constructible<backtest::ReplayLedger>::value);
static_assert(std::is_nothrow_move_assignable<strategy::slice_detail::Bucket>::value);
static_assert(std::is_nothrow_move_constructible<strategy::slice_detail::Bucket>::value);

TEST_CASE("a ledger opening a lot finds every lot it lists after a failed allocation") {
  // At 0, 1 and 8 lots before the fill, narrow ledgers whose list of held lots must grow first (a copy's list has
  // no room to spare, and 8 fills the first growth); at 16, 17, 20 and 32: the last scan, the index being built,
  // the index extended, and the index growing its storage.
  for (const int lots : {0, 1, 8, 16, 17, 20, 32}) {
    INFO(lots << " lots before the fill");
    const auto opened = named("NEW");
    const auto before = holding("S", lots);
    auto ledger = before;
    const auto fill = bought(opened, "3");
    bool applied = false;
    const int failures = fail_each_allocation(
      [&] { applied = ledger.apply(fill, krw()); },
      [&] {
        require_lookups_match_positions(ledger);
        require_equity_as_scanned(ledger);
        require_unchanged(ledger, before);
        CHECK_FALSE(lists(ledger, opened));
        ledger = before;
      }
    );
    CHECK(failures > 0);
    CHECK(applied);
    require_lookups_match_positions(ledger);
    CHECK(ledger.quantity_of(opened) == std::optional<Decimal64>(Decimal64::parse("3")));
    CHECK(ledger.positions().size() == static_cast<std::size_t>(lots + 1));
    require_equity_as_scanned(ledger);
  }
}

TEST_CASE("a reopened lot that cannot be recorded leaves equity as a scan of the lots says") {
  // A copy holds its list of held lots without room to spare: 100 lots opened, 99 closed, copied (by
  // construction and by assignment), then one of the closed lots reopened in the copy.
  auto book = holding("S", 100);
  for (int k = 1; k < 100; ++k) {
    auto sold = bought(named("S" + std::to_string(k)), "1");
    sold.fill_.side_ = OrderSide::SELL;
    REQUIRE(book.apply(sold, krw()));
  }
  for (const bool assigned : {false, true}) {
    INFO((assigned ? "copy assigned" : "copy constructed"));
    auto before = book;
    if (assigned) {
      before = holding("T", 3);
      before = book;
    }
    auto ledger = before;
    const auto reopened = bought(named("S50"), "2");
    bool applied = false;
    const int failures = fail_each_allocation(
      [&] { applied = ledger.apply(reopened, krw()); },
      [&] {
        require_lookups_match_positions(ledger);
        // Every allocation is made before the lot changes, so a failure leaves it, and the running equity, as
        // they were.
        require_equity_as_scanned(ledger);
        require_unchanged(ledger, before);
        ledger = before;
      }
    );
    CHECK(failures > 0);
    CHECK(applied);
    CHECK(ledger.quantity_of(named("S50")) == std::optional<Decimal64>(Decimal64::parse("2")));
    require_equity_as_scanned(ledger);
  }
}

TEST_CASE("a reservation that cannot be recorded leaves the rows and the buying power together") {
  // `Runner::persist_reservation` writes in place: the row is appended whole or not at all, and the buying
  // power is lowered only after it is in. A failure later in the pass (after the order was sent) can leave the
  // reservation in; it never leaves half of one.
  strategy::RiskLimits limits;
  limits.enforce_buying_power_ = true;
  limits.enforce_sellable_quantity_ = false;
  limits.permitted_phases_ = std::nullopt;
  runner::DryRunner<BuysTen> dry(BuysTen{}, limits);
  using Tag = runner::DryRunner<BuysTen>::Tag;
  // A symbol too long for a short string, so copying the row allocates.
  const auto symbol = std::string("A-SYMBOL-LONGER-THAN-ANY-SHORT-STRING");
  Instrument reference;
  reference.id_ = named(symbol);
  reference.type_ = SecurityType::STOCK;
  reference.currency_ = krw();
  reference.lot_size_ = Decimal64::parse("1");
  reference.price_rule_ = PriceRule::krx_equity();
  dry.context().set_instrument(reference);
  dry.context().set_phase(strategy::MarketPhase::OPEN);
  // Each attempt a day later, since a bar must be later than the last one fed.
  auto event = bar_of(symbol, BarInterval::ONE_DAY);
  const auto next_day = [&event] {
    event.bar_.timestamp_ += std::chrono::hours(24);
    event.period_end_ += std::chrono::hours(24);
  };
  strategy::Portfolio<Tag> funded;
  funded.as_of_ = event.period_end_;
  funded.cash_.push_back(Money{Decimal64::parse("100000000"), krw()});
  funded.buying_power_.push_back(BuyingPower{Money{Decimal64::parse("100000000"), krw()}, event.period_end_});
  dry.context().set_portfolio(funded);

  const auto require_whole = [&dry] {
    const auto& portfolio = dry.context().portfolio();
    REQUIRE(portfolio.working_.size() <= 1);
    REQUIRE(portfolio.buying_power_.size() == 1);
    // Ten at the bar's close of a hundred.
    const char* left = portfolio.working_.empty() ? "100000000" : "99999000";
    REQUIRE(portfolio.buying_power_[0].cash_.amount_ == Decimal64::parse(left));
  };
  const int failures = fail_each_allocation(
    [&] {
      next_day();
      dry.feed(event);
    },
    [&] {
      require_whole();
      dry.context().set_portfolio(funded);
    }
  );
  CHECK(failures > 0);
  require_whole();
  CHECK(dry.context().portfolio().working_.size() == 1);
}

TEST_CASE("a replay queue copy that fails leaves the destination as it was") {
  // The queue keeps its orders and an index of them by instrument; a copy assigned whole or not at all leaves
  // the two agreeing, so every order the destination lists is found, walked and withdrawn.
  const auto order_in = [](const std::string& symbol) {
    strategy::PlannedOrder order;
    order.instrument_ = named(symbol);
    order.type_ = OrderType::MARKET;
    order.time_in_force_ = TimeInForce::DAY;
    order.quantity_ = Decimal64::parse("1");
    return order;
  };
  using Queue = backtest::QueueingExecutor<backtest::BacktestTag>;
  Queue source;
  for (int k = 0; k < 6; ++k) {
    REQUIRE(source.place(order_in("SOURCE-INSTRUMENT-WITH-A-LONG-NAME-" + std::to_string(k % 3))) ==
      runner::SendOutcome::ACCEPTED);
  }
  Queue destination;
  REQUIRE(destination.place(order_in("DESTINATION-INSTRUMENT-WITH-A-LONG-NAME")) == runner::SendOutcome::ACCEPTED);
  const auto require_consistent = [](Queue& queue, std::size_t expected) {
    REQUIRE(queue.queued().size() == expected);
    std::vector<OrderRef<backtest::BacktestTag>> refs;
    for (const auto& entry : queue.queued()) {
      refs.push_back(entry.ref_);
      std::size_t found = 0;
      queue.for_each_queued_in(entry.order_.instrument_, [&](const auto& mine) { found += mine.ref_ == entry.ref_; });
      REQUIRE(found == 1);
    }
    REQUIRE(refs.size() == expected);
    return refs;
  };
  const int failures = fail_each_allocation(
    [&] { destination = source; },
    [&] {
      auto copy = destination;
      const auto refs = require_consistent(copy, 1);
      for (const auto& ref : refs) {
        REQUIRE(copy.withdraw(ref));
      }
      REQUIRE(copy.queued().empty());
    }
  );
  CHECK(failures > 0);
  const auto refs = require_consistent(destination, 6);
  for (const auto& ref : refs) {
    REQUIRE(destination.withdraw(ref));
  }
  CHECK(destination.queued().empty());
}

TEST_CASE("a ledger copy that fails leaves the destination as it was") {
  const auto source = holding("T", 25);
  const auto original = holding("S", 20);
  auto ledger = original;
  const int failures = fail_each_allocation(
    [&] { ledger = source; },
    [&] {
      REQUIRE(ledger.positions().size() == 20);
      require_lookups_match_positions(ledger);
      require_equity_as_scanned(ledger);
      CHECK(ledger.quantity_of(named("S19")) == std::optional<Decimal64>(Decimal64::parse("1")));
      CHECK_FALSE(ledger.quantity_of(named("T0")));
    }
  );
  CHECK(failures > 0);
  CHECK(ledger.positions().size() == 25);
  require_lookups_match_positions(ledger);
  CHECK_FALSE(ledger.quantity_of(named("S0")));
}

TEST_CASE("a period bucket adding a bar keeps its slice and its ids together after a failed allocation") {
  // At 16, 17, 20 and 32 bars before the add: the last scan, the table being filled, the table extended, and
  // the table growing its storage.
  for (const int bars : {16, 17, 20, 32}) {
    INFO(bars << " bars before the add");
    strategy::slice_detail::Bucket before;
    for (int k = 0; k < bars; ++k) {
      before.add(bar_of("S" + std::to_string(k), BarInterval::ONE_DAY));
    }
    auto bucket = before;
    const auto added = bar_of("NEW", BarInterval::ONE_DAY);
    const int failures = fail_each_allocation(
      [&] { bucket.add(added); },
      [&] {
        // The bar was not added, so it is not held.
        CHECK_FALSE(bucket.holds(added));
        for (int k = 0; k < bars; ++k) {
          REQUIRE(bucket.holds(bar_of("S" + std::to_string(k), BarInterval::ONE_DAY)));
        }
        bucket = before;
      }
    );
    CHECK(failures > 0);
    CHECK(bucket.holds(added));
    const auto slices = bucket.take();
    REQUIRE(slices.size() == 1);
    CHECK(slices[0].bars_.size() == static_cast<std::size_t>(bars + 1));
  }
}

TEST_CASE("a period bucket copy that fails leaves the destination as it was") {
  strategy::slice_detail::Bucket source;
  strategy::slice_detail::Bucket bucket;
  for (int k = 0; k < 25; ++k) {
    source.add(bar_of("T" + std::to_string(k), BarInterval::ONE_DAY));
  }
  for (int k = 0; k < 20; ++k) {
    bucket.add(bar_of("S" + std::to_string(k), BarInterval::ONE_DAY));
  }
  const int failures = fail_each_allocation(
    [&] { bucket = source; },
    [&] {
      CHECK(bucket.holds(bar_of("S19", BarInterval::ONE_DAY)));
      CHECK_FALSE(bucket.holds(bar_of("T0", BarInterval::ONE_DAY)));
    }
  );
  CHECK(failures > 0);
  CHECK(bucket.holds(bar_of("T24", BarInterval::ONE_DAY)));
  CHECK_FALSE(bucket.holds(bar_of("S0", BarInterval::ONE_DAY)));
}

TEST_CASE("a wide slice taken first leaves a narrow one answering by its own bars") {
  // A wide daily slice and a narrow hourly one end at one instant; taking the daily one must not take the
  // hourly one's answers with it, and the hourly one turning wide later gets ids of its own.
  strategy::slice_detail::Bucket bucket;
  for (int k = 0; k < 20; ++k) {
    bucket.add(bar_of("S" + std::to_string(k), BarInterval::ONE_DAY));
  }
  bucket.add(bar_of("S3", BarInterval::ONE_HOUR));
  CHECK(bucket.holds(bar_of("S3", BarInterval::ONE_HOUR)));
  CHECK_FALSE(bucket.holds(bar_of("S4", BarInterval::ONE_HOUR)));

  const auto daily = bucket.take_first();
  CHECK(daily.interval_ == BarInterval::ONE_DAY);
  CHECK(daily.bars_.size() == 20);
  CHECK_FALSE(bucket.holds(bar_of("S5", BarInterval::ONE_DAY)));
  CHECK(bucket.holds(bar_of("S3", BarInterval::ONE_HOUR)));

  for (int k = 0; k < 20; ++k) {
    if (k != 3) {
      bucket.add(bar_of("S" + std::to_string(k), BarInterval::ONE_HOUR));
    }
  }
  for (int k = 0; k < 20; ++k) {
    CHECK(bucket.holds(bar_of("S" + std::to_string(k), BarInterval::ONE_HOUR)));
  }
  CHECK_FALSE(bucket.holds(bar_of("S20", BarInterval::ONE_HOUR)));
  CHECK_FALSE(bucket.holds(bar_of("S0", BarInterval::ONE_DAY)));

  // A new daily slice turning wide answers by its own ids, not the ones taken out with the first; it is smaller
  // than the first, so a position left over from that one would point past its end.
  for (int k = 0; k < 17; ++k) {
    bucket.add(bar_of("D" + std::to_string(k), BarInterval::ONE_DAY));
  }
  CHECK(bucket.holds(bar_of("D16", BarInterval::ONE_DAY)));
  CHECK_FALSE(bucket.holds(bar_of("S0", BarInterval::ONE_DAY)));
  CHECK_FALSE(bucket.holds(bar_of("S19", BarInterval::ONE_DAY)));
  CHECK(bucket.take_first().interval_ == BarInterval::ONE_HOUR);
  CHECK(bucket.take_first().interval_ == BarInterval::ONE_DAY);
  CHECK(bucket.empty());

  // Emptied by `take_first` alone, as the live source empties it, and filled again: the same.
  for (int k = 0; k < 20; ++k) {
    bucket.add(bar_of("E" + std::to_string(k), BarInterval::ONE_DAY));
  }
  CHECK(bucket.holds(bar_of("E0", BarInterval::ONE_DAY)));
  CHECK_FALSE(bucket.holds(bar_of("D0", BarInterval::ONE_DAY)));
  CHECK(bucket.take_first().bars_.size() == 20);
  bucket.add(bar_of("S3", BarInterval::ONE_HOUR));

  // Emptied and reused, the bucket answers by what it holds now.
  bucket.clear();
  CHECK(bucket.empty());
  CHECK_FALSE(bucket.holds(bar_of("S3", BarInterval::ONE_HOUR)));
  for (int k = 0; k < 18; ++k) {
    bucket.add(bar_of("U" + std::to_string(k), BarInterval::ONE_HOUR));
  }
  CHECK(bucket.holds(bar_of("U17", BarInterval::ONE_HOUR)));
  CHECK_FALSE(bucket.holds(bar_of("S3", BarInterval::ONE_HOUR)));
  const auto hourly = bucket.take_first();
  CHECK(symbols_of(hourly).size() == 18);
  CHECK(bucket.empty());
}

TEST_CASE("a wide bucket moved away leaves its source empty and usable") {
  const auto daily = [](const std::string& symbol) { return bar_of(symbol, BarInterval::ONE_DAY); };
  const auto twenty = [&daily](const std::string& prefix) {
    strategy::slice_detail::Bucket bucket;
    for (int k = 0; k < 20; ++k) {
      bucket.add(daily(prefix + std::to_string(k)));
    }
    return bucket;
  };
  // The source holds nothing, and a wide slice added to it again answers by its own bars.
  const auto check_source = [&daily](strategy::slice_detail::Bucket& source) {
    CHECK(source.empty());
    CHECK_FALSE(source.holds(daily("S0")));
    for (int k = 0; k < 18; ++k) {
      source.add(daily("E" + std::to_string(k)));
    }
    CHECK(source.holds(daily("E17")));
    CHECK_FALSE(source.holds(daily("S0")));
  };
  const auto check_destination = [&daily](strategy::slice_detail::Bucket& destination) {
    CHECK(destination.holds(daily("S19")));
    CHECK_FALSE(destination.holds(daily("E0")));
    CHECK(destination.take_first().bars_.size() == 20);
  };

  SECTION("by construction") {
    auto source = twenty("S");
    strategy::slice_detail::Bucket destination(std::move(source));
    check_source(source);
    check_destination(destination);
  }
  SECTION("by assignment") {
    auto source = twenty("S");
    auto destination = twenty("T");
    destination = std::move(source);
    check_source(source);
    check_destination(destination);
  }
}
