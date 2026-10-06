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
 * @brief The executor that sends a runner's orders and cancellations through a broker session.
 *
 * It copies a `strategy::PlannedOrder` into an `OrderRequest` (prices and quantities are already on
 * the grid) and classifies a failure with `runner::outcome_of`. Usually made by `bind` or
 * `bind_journaled`.
 *
 * @tparam Session The broker session type (it must not be a market-data-only session).
 */
template <typename Session>
class SessionExecutor {
public:

  /**
   * @brief `true`: a runner built on this executor requires holdings to have been read before it
   *   places, however the runner was assembled (see `Runner::allow_absent_portfolio`).
   */
  static constexpr bool reaches_a_broker = true;
  /** @brief The broker's capabilities, from the session. */
  using Caps = typename Session::Caps;
  /** @brief The broker tag, from the session. */
  using BrokerTag = typename Session::BrokerTag;

  static_assert(
    !Caps::data_only,
    "SessionExecutor sends orders, and this session's client is a market-data source (DataOnlyTraits) "
    "that takes none: bind a broker's session here, and give the data source to SessionSource as its "
    "market-data session"
  );

  /**
   * @brief Constructs an executor for a session.
   * @param session The session; borrowed, and must outlive the executor.
   * @param default_route The venue for an order that names none; see `default_route()`.
   */
  explicit SessionExecutor(Session& session, Venue default_route = Venue::UNSPECIFIED)
  : session_(&session), default_route_(default_route) {}

  /**
   * @brief Returns the venue an order that names none is sent to.
   *
   * A strategy's orders name no venue, so this is the route of every order a runner plans; an order
   * that names a venue keeps it. The runner writes the route into the order before sending, so the
   * journal and reports name it.
   *
   * `UNSPECIFIED` (the default) takes the broker's own default: KRX at KIS, none needed at Toss, and
   * **a refusal at Kiwoom**, which has no default (give `Venue::KRX`, `Venue::NXT`, or
   * `Venue::COMBINED` for its SOR). At KIS, `Venue::COMBINED` selects SOR, which a demo account
   * refuses. A route the broker cannot take is refused before any request.
   *
   * @warning Prefer `Venue::KRX` or `Venue::NXT` to `Venue::COMBINED` until SOR orders have been seen
   *   listed on your account. Journal recovery matches a listed order to the recorded route; if the
   *   broker lists a SOR order under the venue where it rests, recovery can conclude the order was
   *   never placed, and a restarted runner could send it again. Neither broker's SOR listing has been
   *   observed.
   *
   * @return The default route.
   */
  Venue default_route() const noexcept { return default_route_; }

  /**
   * @brief Returns the venue this executor requests for an order.
   * @param asked The order's venue.
   * @return `asked`, or `default_route()` if `asked` is `UNSPECIFIED`.
   */
  Venue requested_route(Venue asked) const noexcept {
    return asked == Venue::UNSPECIFIED ? default_route_ : asked;
  }

  /**
   * @brief Returns the route an order is sent with, for the journal to record before sending (for
   *   SOR, `COMBINED`, not the venue it executes on).
   *
   * `default_route()` is applied first; then the broker's traits say where an unspecified venue goes
   * (KRX at KIS). A session whose traits do not say returns the requested route.
   *
   * @param asked The order's venue.
   * @return The venue.
   */
  Venue effective_route(Venue asked) const noexcept {
    return traits_route<Session>(requested_route(asked), 0);
  }

  /**
   * @brief Places one order.
   *
   * An order with an idempotency key (`client_order_id_`) at a broker that accepts none is refused
   * (`REFUSED_LOCALLY`), not sent without the key.
   *
   * @param order The order.
   * @return The outcome; on failure `last_error()` says why, and on success `last_ref()` is the
   *   broker's reference.
   */
  runner::SendOutcome place(const strategy::PlannedOrder& order) {
    if (order.client_order_id_) {
      if constexpr (!Caps::has_client_order_id) {
        last_error_ = BrokerError{};
        last_error_->category_ = ErrorCategory::INVALID_ARGUMENT;
        last_error_->reached_broker_ = false;
        last_error_->message_ =
          "this broker accepts no idempotency key; the intent asked for one";
        // Refused here, not by the broker: nothing was sent, so nothing may be counted against the
        // rate limit. Reporting this as a broker rejection spent budget for a request nobody made.
        return runner::SendOutcome::REFUSED_LOCALLY;
      }
    }

    OrderRequest request;
    request.instrument_ = order.instrument_;
    request.side_ = order.side_;
    request.type_ = order.type_;
    request.time_in_force_ = order.time_in_force_;
    // All three, every time. A fact not copied here never reaches the wire, so
    // the order placed is not the order the caller planned -- and it fails
    // silently, because a default is a valid value rather than an empty one.
    request.session_ = order.session_;
    // The runner has already written the route in; applied again for an executor driven by hand.
    request.venue_ = requested_route(order.venue_);
    request.quantity_ = order.quantity_;
    request.price_ = order.price_;
    // A broker whose `has_trigger_price` is false is refused it by the session before its adapter runs,
    // which `outcome_of` reports as a local refusal: nothing was sent.
    request.trigger_price_ = order.trigger_price_;
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
   * @brief Cancels the whole remaining quantity of a working order.
   *
   * An order with no broker reference is refused (`REFUSED_LOCALLY`) without a request.
   *
   * @param working The order, as listed (or reserved).
   * @return The outcome (accepted means the request was accepted, not that the order is gone).
   */
  runner::SendOutcome cancel(const WorkingOrder<BrokerTag>& working) {
    if (working.ref_.primary_.empty()) {
      // **A reference naming nothing addresses nothing at a broker.** A row can reach here with an
      // empty one: a runner reserves an order it has just sent so that later limits count it, and
      // an executor that could not report what the broker issued leaves the reference blank.
      // Sending that is a request which can only fail while looking like an attempt, and a
      // cancellation failure is the one this library must not manufacture noise around. Refused as
      // an answer rather than as an outage, because nothing is in doubt: the order was never
      // addressed, so there is nothing to reconcile.
      //
      // The check is here and not in the runner because a reference is only meaningful to a
      // broker. A replay's engine matches a cancellation on instrument and side, and refusing one
      // there would leave a queued order to fill that the caller asked to withdraw.
      last_error_ = BrokerError{};
      last_error_->category_ = ErrorCategory::INVALID_ARGUMENT;
      last_error_->reached_broker_ = false;
      last_error_->message_ =
        "no broker reference for this order; it cannot be withdrawn until a listing supplies one";
      return runner::SendOutcome::REFUSED_LOCALLY;
    }
    auto result = session_->cancel(working.ref_, cancel_all(working));
    if (result) {
      last_error_.reset();
      return runner::SendOutcome::ACCEPTED;
    }
    last_error_ = result.error();
    return runner::outcome_of(*last_error_);
  }

  /**
   * @brief Asks the broker how much of an instrument may be sold, where the broker answers that by a
   *   separate inquiry (`SellableSource::INQUIRY`).
   *
   * Elsewhere the figure comes with the holdings, and this returns `std::nullopt` without a request.
   *
   * @param instrument The instrument.
   * @return The quantity, or `std::nullopt` if not asked or the inquiry failed (`last_error()` says
   *   why); the runner treats an unknown quantity as nothing sellable.
   */
  std::optional<Decimal64> sellable_quantity(const InstrumentId& instrument) {
    if constexpr (Caps::sellable_source == SellableSource::INQUIRY) {
      auto result = session_->sellable_quantity(instrument);
      if (!result) {
        last_error_ = result.error();
        return std::nullopt;
      }
      last_error_.reset();
      return result.value();
    } else {
      static_cast<void>(instrument);
      return std::nullopt;
    }
  }

  /** @brief `true` if the sellable quantity is a separate inquiry (one request per instrument sold). */
  static constexpr bool inquires_sellable = Caps::sellable_source == SellableSource::INQUIRY;

  /**
   * @brief Returns what the broker's sellable-quantity inquiry establishes, recorded with each figure.
   * @return The basis.
   */
  static constexpr SellableBasis sellable_inquiry_basis() {
    // Read through the same gate as `BrokerCaps`, because `Caps` is a caller's type parameter and need
    // not be `BrokerCaps`: a hand-written capability set that makes the inquiry states its basis.
    return caps_detail::inquiry_basis<Caps>();
  }

  /**
   * @brief Returns the reference the broker issued for the last accepted order.
   * @return The reference, or empty if no order was accepted yet.
   */
  const std::optional<OrderRef<BrokerTag>>& last_ref() const noexcept { return last_ref_; }

  /**
   * @brief Returns why the last instruction failed.
   * @return The error, or empty if the last instruction succeeded.
   */
  const std::optional<BrokerError>& last_error() const noexcept { return last_error_; }

private:
  // Asks the session's traits where `requested` goes, where they say.
  template <typename S>
  static constexpr auto traits_route(Venue requested, int) noexcept
    -> decltype(S::Traits::effective_route(requested)) {
    return S::Traits::effective_route(requested);
  }

  // The fallback, for traits that do not say: the requested route is the most that is known.
  template <typename S>
  static constexpr Venue traits_route(Venue requested, long) noexcept { return requested; }

  Session* session_;
  Venue default_route_;
  std::optional<OrderRef<BrokerTag>> last_ref_;
  std::optional<BrokerError> last_error_;
};

} // namespace live
} // namespace algotradelib
