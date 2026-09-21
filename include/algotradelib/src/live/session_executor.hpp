#pragma once

#include "../runner/execution.hpp"
#include "../strategy/reconcile.hpp"

#include <algotradelib/broker>

#include <optional>
#include <string>
#include <utility>

namespace algotradelib {
namespace live {

/**
 * Sends a runner's instructions through a bound broker session.
 *
 * The only component in the whole strategy stack that performs I/O, and the entire reason the
 * pipeline was parameterized on an executor: every decision above this has already been tested
 * without a broker, and what remains here is a translation plus one error classification.
 *
 * Two translations happen, and both are places where a mistake would reach an exchange:
 *
 * - **A `PlannedOrder` becomes an `OrderRequest`.** Reconciliation has already put the price on the
 *   tick grid and the quantity on the lot, so this adds nothing numeric; it moves fields.
 * - **A failure becomes a `SendOutcome`** through `runner::outcome_of`, which decides whether the
 *   order might exist. That function carries the reasoning; this one just asks it.
 */
template <typename Session>
class SessionExecutor {
public:
  using Caps = typename Session::Caps;
  using BrokerTag = typename Session::BrokerTag;

  /**
   * Binds a session.
   *
   * Held by pointer rather than by value: a session owns a client and an executor, and a runner
   * does not own the connection it borrows.
   */
  explicit SessionExecutor(Session& session) : session_(&session) {}

  /**
   * Places one order, and reports whether it might now exist.
   *
   * A planned order carrying an idempotency key for a broker that accepts none is **refused rather
   * than sent with the key dropped**. Dropping it would make a retry look safe on a broker that
   * offers no such guarantee, which is the specific mistake `OrderRequest::client_order_id_`
   * documents. The strategy asked for a guarantee the binding cannot provide, and saying so is the
   * only honest answer.
   */
  runner::SendOutcome place(const strategy::PlannedOrder& order) {
    if (order.client_order_id_) {
      if constexpr (!Caps::has_client_order_id) {
        last_error_ = BrokerError{};
        last_error_->category_ = ErrorCategory::INVALID_ARGUMENT;
        last_error_->message_ =
          "this broker accepts no idempotency key; the intent asked for one";
        return runner::SendOutcome::REJECTED;
      }
    }

    OrderRequest request;
    request.instrument_ = order.instrument_;
    request.side_ = order.side_;
    request.type_ = order.type_;
    request.time_in_force_ = order.time_in_force_;
    request.quantity_ = order.quantity_;
    request.price_ = order.price_;
    if constexpr (Caps::has_client_order_id) {
      request.client_order_id_ = order.client_order_id_;
    }

    auto result = session_->place(std::move(request));
    if (result) {
      last_ref_ = result.value().ref_;
      last_error_.reset();
      return runner::SendOutcome::ACCEPTED;
    }
    last_error_ = result.error();
    return runner::outcome_of(*last_error_);
  }

  /**
   * Cancels one working order in full, and reports whether the withdrawal might have happened.
   *
   * `cancel_all` carries the order's own shape and amendable quantity, so a broker that requires
   * either receives it without this code restating what the order already said.
   */
  runner::SendOutcome cancel(const WorkingOrder<BrokerTag>& working) {
    auto result = session_->cancel(working.ref_, cancel_all(working));
    if (result) {
      last_error_.reset();
      return runner::SendOutcome::ACCEPTED;
    }
    last_error_ = result.error();
    return runner::outcome_of(*last_error_);
  }

  /** Returns the reference the broker issued for the last accepted order. */
  const std::optional<OrderRef<BrokerTag>>& last_ref() const noexcept { return last_ref_; }

  /** Returns why the last instruction failed, for the runner to log. */
  const std::optional<BrokerError>& last_error() const noexcept { return last_error_; }

private:
  Session* session_;
  std::optional<OrderRef<BrokerTag>> last_ref_;
  std::optional<BrokerError> last_error_;
};

} // namespace live
} // namespace algotradelib
