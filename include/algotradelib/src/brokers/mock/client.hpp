#pragma once

#include "./market.hpp"

#include <deque>
#include <stdexcept>
#include <string>
#include <utility>

namespace algotradelib {
namespace mock {

/** @brief The mock broker's one account; it has no selector. */
struct MockAccount {};

/** @brief A cancellation for the mock broker: the order id. */
struct MockAmendment {
  /** @brief The order id. */
  std::string order_id_;
};

/** @brief A call to the mock broker, for scripting a fault (`MockClient::fail_next`). */
enum class Call {
  /** @brief `quote`. */
  QUOTE,
  /** @brief `bars`. */
  BARS,
  /** @brief `instrument`. */
  INSTRUMENT,
  /** @brief `holdings`. */
  HOLDINGS,
  /** @brief `buying_power`. */
  BUYING_POWER,
  /** @brief `place`. */
  PLACE,
  /** @brief `cancel`. */
  CANCEL,
  /** @brief `working_orders`. */
  WORKING_ORDERS,
  /** @brief `order_history`. */
  ORDER_HISTORY,
};

/**
 * @brief The mock broker's client: blocking, answering from a `Market`, after any faults scripted for
 *   the call.
 *
 * Bind it like any blocking client, `BlockingSession<MockClient>(std::make_shared<MockClient>(market),
 * MockAccount{})`. Nothing leaves the process; `environment` is `"paper"`.
 */
class MockClient {
public:
  /**
   * @brief Builds a client over a market.
   * @param market The market; borrowed, and must outlive the client.
   */
  explicit MockClient(Market& market) : market_(&market) {}

  /**
   * @brief Returns the environment this client trades in.
   * @return Always `"paper"`.
   */
  static constexpr std::string_view environment() noexcept { return mock::environment; }

  /**
   * @brief Returns the market.
   * @return The market this client answers from.
   */
  Market& market() const noexcept { return *market_; }

  /**
   * @brief Answers the next calls of a kind with an error instead of asking the market.
   *
   * Nothing happens in the market: a placement failed this way was never accepted, whatever the
   * error says. Faults are used in the order scripted.
   *
   * @param call The kind of call.
   * @param error The error those calls answer.
   * @param times How many calls, at least one.
   * @throws std::invalid_argument if `times` is below one.
   */
  void fail_next(Call call, BrokerError error, int times = 1) {
    if (times < 1) {
      throw std::invalid_argument("a fault is scripted for at least one call");
    }
    for (int k = 0; k < times; ++k) {
      faults_.push_back(Fault{call, error, Effect::FAIL});
    }
  }

  /**
   * @brief Carries out the next placement or cancellation in the market, then answers it with a
   *   transport error that reached the broker, as a lost acknowledgement does: its outcome is
   *   unknown to the caller.
   * @param call `Call::PLACE` or `Call::CANCEL`.
   * @throws std::invalid_argument for another call, which has no outcome to lose.
   */
  void lose_next_reply(Call call) {
    if (call != Call::PLACE && call != Call::CANCEL) {
      throw std::invalid_argument("only a placement or a cancellation has a reply to lose");
    }
    BrokerError error;
    error.category_ = ErrorCategory::TRANSPORT;
    error.reached_broker_ = true;
    error.broker_code_ = "reply_lost";
    error.message_ = "the mock broker carried this out and lost the reply";
    faults_.push_back(Fault{call, std::move(error), Effect::CARRY_OUT});
  }

  /**
   * @brief Answers the next reads of an account's state with what the last one answered, stamped now,
   *   as a broker that has not yet reported a change does.
   *
   * The market moves on underneath: a fill that happened is missing from these answers, and a
   * cancellation of an order they still list may answer `nothing_to_cancel`. A read with no earlier
   * answer to repeat answers normally.
   *
   * @param call `Call::HOLDINGS`, `Call::BUYING_POWER`, `Call::WORKING_ORDERS` or
   *   `Call::ORDER_HISTORY`.
   * @param times How many reads, at least one.
   * @throws std::invalid_argument for another call, or `times` below one.
   */
  void repeat_last(Call call, int times = 1) {
    if (call != Call::HOLDINGS && call != Call::BUYING_POWER && call != Call::WORKING_ORDERS &&
        call != Call::ORDER_HISTORY) {
      throw std::invalid_argument("only a read of the account's state can be repeated");
    }
    if (times < 1) {
      throw std::invalid_argument("a fault is scripted for at least one call");
    }
    for (int k = 0; k < times; ++k) {
      faults_.push_back(Fault{call, BrokerError{}, Effect::REPEAT});
    }
  }

  /**
   * @brief Answers a quote request.
   * @param instrument The instrument.
   * @return As `Market::quote`.
   */
  Result<QuoteSnapshot> quote(const InstrumentId& instrument) {
    if (auto fault = take(Call::QUOTE)) {
      return Result<QuoteSnapshot>(std::move(fault->error_));
    }
    return market_->quote(instrument);
  }

  /**
   * @brief Answers a reference-data request.
   * @param instrument The instrument.
   * @return As `Market::instrument`.
   */
  Result<Instrument> instrument(const InstrumentId& instrument) {
    if (auto fault = take(Call::INSTRUMENT)) {
      return Result<Instrument>(std::move(fault->error_));
    }
    return market_->instrument(instrument);
  }

  /**
   * @brief Answers a bar request.
   * @param query The query.
   * @return As `Market::bars`.
   */
  Result<BarSeries> bars(const BarQuery& query) {
    if (auto fault = take(Call::BARS)) {
      return Result<BarSeries>(std::move(fault->error_));
    }
    return market_->bars(query);
  }

  /**
   * @brief Answers a holdings request.
   * @return As `Market::holdings`.
   */
  Result<HoldingsSnapshot> holdings() {
    return read(Call::HOLDINGS, last_holdings_, [this] { return Result<HoldingsSnapshot>(market_->holdings()); });
  }

  /**
   * @brief Answers a buying-power request.
   * @param currency The currency.
   * @return As `Market::buying_power`.
   */
  Result<BuyingPower> buying_power(const Currency& currency) {
    if (last_buying_power_ && !(last_buying_power_->cash_.currency_ == currency)) {
      last_buying_power_.reset();
    }
    return read(Call::BUYING_POWER, last_buying_power_, [&] { return market_->buying_power(currency); });
  }

  /**
   * @brief Places an order.
   * @param order The order.
   * @return As `Market::place`.
   */
  Result<PlacedOrder<Broker>> place(const OrderRequest& order) {
    auto fault = take(Call::PLACE);
    if (fault && fault->effect_ == Effect::FAIL) {
      return Result<PlacedOrder<Broker>>(std::move(fault->error_));
    }
    auto placed = market_->place(order);
    if (fault) {
      return Result<PlacedOrder<Broker>>(std::move(fault->error_));
    }
    return placed;
  }

  /**
   * @brief Cancels an order.
   * @param amendment The order id.
   * @return As `Market::cancel`.
   */
  Result<PlacedOrder<Broker>> cancel(const MockAmendment& amendment) {
    auto fault = take(Call::CANCEL);
    if (fault && fault->effect_ == Effect::FAIL) {
      return Result<PlacedOrder<Broker>>(std::move(fault->error_));
    }
    auto canceled = market_->cancel(amendment.order_id_);
    if (fault) {
      return Result<PlacedOrder<Broker>>(std::move(fault->error_));
    }
    return canceled;
  }

  /**
   * @brief Answers a working-order request.
   * @return As `Market::working_orders`.
   */
  Result<WorkingOrders<Broker>> working_orders() {
    return read(Call::WORKING_ORDERS, last_working_orders_, [this] {
      return Result<WorkingOrders<Broker>>(market_->working_orders());
    });
  }

  /**
   * @brief Answers an order-history request.
   * @param query The query.
   * @return As `Market::order_history`.
   */
  Result<OrderPage> order_history(const OrderHistoryQuery& query) {
    return read(Call::ORDER_HISTORY, last_history_, [&] { return Result<OrderPage>(market_->order_history(query)); });
  }

private:
  /** What a scripted fault does. */
  enum class Effect {
    /** Answers with the error; the market is not asked. */
    FAIL,
    /** The market carries the call out; the answer is the error. */
    CARRY_OUT,
    /** Answers with the last answer, stamped now. */
    REPEAT,
  };

  /** One scripted fault. */
  struct Fault {
    /** The call it answers. */
    Call call_;
    /** The error it answers with, for `FAIL` and `CARRY_OUT`. */
    BrokerError error_;
    /** What it does. */
    Effect effect_;
  };

  /** Answers an account read: a fault, a repeat of the last answer, or the market's answer, kept. */
  template <typename T, typename Ask>
  Result<T> read(Call call, std::optional<T>& last, Ask ask) {
    auto fault = take(call);
    if (fault && fault->effect_ == Effect::FAIL) {
      return Result<T>(std::move(fault->error_));
    }
    if (fault && last) {
      auto repeated = *last;
      repeated.received_at_ = market_->now();
      return Result<T>(std::move(repeated));
    }
    auto answer = ask();
    if (answer) {
      last = answer.value();
    }
    return answer;
  }

  /** Removes and returns the first fault scripted for a call, if any. */
  std::optional<Fault> take(Call call) {
    for (auto it = faults_.begin(); it != faults_.end(); ++it) {
      if (it->call_ == call) {
        auto fault = std::move(*it);
        faults_.erase(it);
        return fault;
      }
    }
    return std::nullopt;
  }

  Market* market_;
  std::deque<Fault> faults_;
  std::optional<HoldingsSnapshot> last_holdings_;
  std::optional<BuyingPower> last_buying_power_;
  std::optional<WorkingOrders<Broker>> last_working_orders_;
  std::optional<OrderPage> last_history_;
};

} // namespace mock
} // namespace algotradelib
