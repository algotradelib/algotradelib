// A runner killed with an order in flight, and the restart that finds out what happened.
//
// No broker, no credentials, no network, and no signal raised. The "crash" is a scope that ends
// without the terminal line ever being written, which is exactly what a `SIGKILL` leaves on disk,
// and the "restart" is a fresh ledger reading the file its predecessor left behind.
//
// The point it demonstrates is the one sentence the whole feature rests on: **an empty ledger does
// not mean nothing is owed -- it means nothing is remembered.**
//
// Run it as:
//
//   example-order-journal

#include <algotradelib/runner>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace runner = algotradelib::runner;
namespace strategy = algotradelib::strategy;

using algotradelib::Country;
using algotradelib::Decimal64;
using algotradelib::InstrumentId;
using algotradelib::OrderSide;
using algotradelib::OrderSnapshot;
using algotradelib::OrderStatus;
using algotradelib::OrderType;
using algotradelib::Venue;
using Clock = std::chrono::system_clock;

InstrumentId samsung() { return InstrumentId{Country::KR, Venue::COMBINED, "005930"}; }

/** The instant this run pretends to be sending at, so the output is identical every time. */
Clock::time_point sent_at() { return Clock::time_point{} + std::chrono::hours(24 * 20000); }

strategy::PlannedOrder buy_ten() {
  strategy::PlannedOrder order;
  order.instrument_ = samsung();
  order.side_ = OrderSide::BUY;
  order.type_ = OrderType::LIMIT;
  order.quantity_ = Decimal64::parse("10");
  order.price_ = Decimal64::parse("70000");
  return order;
}

/**
 * An executor that never returns, standing in for a process killed mid-request.
 *
 * It throws rather than failing, because a failure would be *classified* -- `outcome_of` would call
 * it indeterminate and a terminal line would be written. The case worth demonstrating is the one
 * where no error path runs at all: a default `SIGKILL` handler does not unwind, so the journal is
 * left holding a `PENDING` line and nothing else.
 */
template <typename Tag>
class KilledExecutor {
public:
  runner::SendOutcome place(const strategy::PlannedOrder&) {
    throw std::runtime_error("the process died with the request on the wire");
  }
  runner::SendOutcome cancel(const algotradelib::WorkingOrder<Tag>&) {
    return runner::SendOutcome::ACCEPTED;
  }
};

/** Prints the journal exactly as `tail` would show it during an incident. */
void show_journal(const std::string& path) {
  std::FILE* handle = std::fopen(path.c_str(), "rb");
  if (handle == nullptr) {
    std::cout << "  (no journal)\n";
    return;
  }
  std::string text;
  char buffer[1024];
  while (const auto read = std::fread(buffer, 1, sizeof(buffer), handle)) {
    text.append(buffer, read);
  }
  std::fclose(handle);

  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find('\n', begin);
    const auto stop = end == std::string::npos ? text.size() : end;
    std::cout << "  " << text.substr(begin, stop - begin);
    if (end == std::string::npos) {
      std::cout << "   <- no newline: this write never finished";
    }
    std::cout << "\n";
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
}

/** A row as the broker's order list would report the order that did, in fact, get through. */
OrderSnapshot listed_order() {
  OrderSnapshot snapshot;
  snapshot.order_id_ = "KRX-4412";
  snapshot.instrument_ = samsung();
  snapshot.side_ = OrderSide::BUY;
  snapshot.type_ = OrderType::LIMIT;
  snapshot.status_ = OrderStatus::PENDING;
  snapshot.quantity_ = Decimal64::parse("10");
  snapshot.price_ = Decimal64::parse("70000");
  snapshot.ordered_at_ = sent_at();
  return snapshot;
}

} // namespace

int main() {
  const std::string path = "example-order-journal.jsonl";
  std::remove(path.c_str());

  std::cout << "1. A runner sends one order, and is killed before it learns the outcome.\n\n";
  {
    runner::FileJournal journal(path);
    if (!journal.open()) {
      std::cout << "could not open " << path << " for appending\n";
      return 1;
    }
    runner::JournalingExecutor<KilledExecutor<runner::DryTag>, runner::FileJournal,
      runner::ManualClock>
      executor{KilledExecutor<runner::DryTag>{}, journal, runner::ManualClock{sent_at()}};
    executor.set_id_prefix("run-a");
    try {
      executor.place(buy_ten());
    } catch (const std::exception& error) {
      std::cout << "   " << error.what() << "\n\n";
    }
  }

  std::cout << "   What the file holds:\n";
  show_journal(path);
  std::cout
    << "\n   One line, and it says `pending`. The order may be resting on the exchange or may\n"
    << "   never have arrived. Nothing in this process knows which, and the process is gone.\n\n";

  std::cout << "2. A new process starts. Its ledger is empty.\n\n";
  runner::IndeterminacyLedger ledger;
  std::cout << "   Before recovery, may it trade " << samsung().symbol_
    << "? " << (ledger.requires_reconcile(samsung()) ? "no" : "yes")
    << "  <- and that is the trap\n";

  const auto recovery = runner::recover_into(runner::read_journal_file(path), ledger);
  std::cout << "   After recovery:                  "
    << (ledger.requires_reconcile(samsung()) ? "no" : "yes") << "\n";
  std::cout << "   Instruments marked: " << recovery.marked_.size()
    << ", instructions owed: " << recovery.open_.size() << "\n\n";

  std::cout << "3. The broker's own order list is read, and it is the only thing that resolves.\n\n";

  // Too soon. An accepted write does not appear in a listing instantly, so an absent row this early
  // is not evidence that the order does not exist.
  const auto early = runner::reconcile_against(
    recovery.open_, {}, sent_at() + std::chrono::seconds(5), ledger
  );
  std::cout << "   Five seconds later, an empty listing: "
    << runner::match_verdict_name(early.matches_.front().verdict_)
    << " -- still withheld\n";

  // Later, the row is there. The order existed after all.
  const auto settled = runner::reconcile_against(
    recovery.open_, {listed_order()}, sent_at() + std::chrono::minutes(10), ledger
  );
  std::cout << "   Ten minutes later, the order is listed: "
    << runner::match_verdict_name(settled.matches_.front().verdict_) << " as "
    << settled.matches_.front().order_id_ << "\n";
  std::cout << "   May it trade " << samsung().symbol_ << " now? "
    << (ledger.requires_reconcile(samsung()) ? "no" : "yes") << "\n\n";

  std::cout
    << "Had the listing stayed empty past the window, the verdict would have been `not_placed`\n"
    << "and the instrument would have cleared just the same -- the order does not exist. Had two\n"
    << "rows matched, it would have been `ambiguous`, nothing would clear, and an operator would\n"
    << "decide. The one thing that never happens is a guess.\n";

  std::remove(path.c_str());
  return 0;
}
