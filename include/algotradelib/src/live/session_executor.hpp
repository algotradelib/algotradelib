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

  /**
   * This executor puts orders in front of a real broker, which `runner::runner_detail::sends_to_a_broker` reads.
   *
   * It makes a runner built on it require an account it has actually read before it places, however
   * that runner was assembled -- by `live::bind`, by `bind_journaled`, or by hand as
   * `doxygen/vignettes/binding-a-broker.md` shows. An empty `Portfolio` is the permissive answer to
   * every ceiling, so a run whose holdings read failed would otherwise size an entry as though the
   * account held nothing. A live run that genuinely reads no holdings says `allow_absent_portfolio()`.
   */
  static constexpr bool reaches_a_broker = true;
  /** The bound broker's capability set, forwarded from the session. */
  using Caps = typename Session::Caps;
  /** The bound broker's tag, forwarded from the session. */
  using BrokerTag = typename Session::BrokerTag;

  /**
   * Binds a session.
   *
   * Held by pointer rather than by value: a session owns a client and an executor, and a runner
   * does not own the connection it borrows.
   */
  explicit SessionExecutor(Session& session, Venue default_route = Venue::UNSPECIFIED)
  : session_(&session), default_route_(default_route) {}

  /**
   * Returns the venue an order that names none is sent to.
   *
   * **The one routing input a strategy runner has.** Nothing on the strategy path names a venue --
   * routing is a fact about the account and the broker, not about the strategy -- so every order a
   * runner plans asks for `UNSPECIFIED`, and this is what it is sent with instead. An order that
   * does name a venue keeps it.
   *
   * `UNSPECIFIED`, the default, sends none and takes the broker's own documented default: KRX at KIS,
   * no choice at Toss, and **a refusal at Kiwoom**, which documents no default and so requires one to
   * be stated here (`Venue::KRX`, `Venue::NXT`, or `Venue::COMBINED` for Kiwoom's SOR). At KIS,
   * `Venue::COMBINED` is how a runner opts into SOR, which a KIS demo account refuses. A route the
   * broker cannot take is refused by its adapter before any request, as for any order.
   *
   * Fixed for the executor's life, so "where did this executor send an unrouted order" has one
   * answer. The runner writes the route into the order before sending it
   * (`runner::Runner` asks `requested_route`), so the journal line, the row it reserves and the
   * report all name the route that went on the wire rather than the one the strategy left blank.
   *
   * Whether Kiwoom's order listing reports a SOR order as SOR or as the venue it rested at has not
   * been observed. If it is the latter, recovery reads the listed row as contradicting the recorded
   * route and keeps the instrument held for reconciliation. `Venue::KRX` or `Venue::NXT` has no such
   * doubt.
   */
  Venue default_route() const noexcept { return default_route_; }

  /** Returns `asked`, or `default_route()` where `asked` is `UNSPECIFIED`: the venue this executor sends. */
  Venue requested_route(Venue asked) const noexcept {
    return asked == Venue::UNSPECIFIED ? default_route_ : asked;
  }

  /**
   * Returns the venue an order carrying `asked` will actually reach.
   *
   * The binding knows this and nothing above it does: KIS documents an omitted
   * exchange field as proceeding at KRX, and Toss routes nowhere at all. It is
   * exposed here so a journaling wrapper can record the venue the order will
   * reach **before** the durable line is written, rather than the `UNSPECIFIED`
   * a strategy left behind -- which recovery reads as a positive claim that no
   * venue was reached, and confirms a row from any venue against.
   *
   * Resolving it here rather than in the request builder keeps the write
   * ordering `ai/order-journal` chose: the line is still appended before any
   * network I/O, and this is a pure function of the order.
   *
   * **Always answers, with `default_route()` applied first.** A broker's traits
   * that say how it routes an omitted venue are asked about the requested
   * route; traits that do not -- `effective_route` is optional, and a
   * hand-written session need have no traits at all -- get the requested route
   * itself. Without that fallback a third party's adapter bound with a default
   * would journal `UNSPECIFIED` while sending the default. With no default the
   * fallback is `asked`, which is what the journal recorded before.
   */
  Venue effective_route(Venue asked) const noexcept {
    return traits_route<Session>(requested_route(asked), 0);
  }

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
    // A broker that does not declare `has_trigger_price` is refused it by the session before its
    // adapter runs, which `outcome_of` reports as a local refusal: nothing was sent.
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
   * Cancels one working order in full, and reports whether the withdrawal might have happened.
   *
   * `cancel_all` carries the order's own shape and amendable quantity, so a broker that requires
   * either receives it without this code restating what the order already said.
   *
   * An order with no broker reference is refused here rather than sent, because a withdrawal
   * addressed to an empty string can only fail.
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
   * Returns what the broker says may be sold of `instrument`, when it takes asking.
   *
   * **Only exists where the holdings snapshot does not already carry the figure.** At a broker
   * whose `sellable_source` is `HOLDINGS` the answer arrives with every position the runner already
   * polls, so this compiles to nothing there and the runner never calls it -- one `if constexpr`
   * decides between free and one request per instrument sold.
   *
   * Nothing on failure rather than an error, because the caller's next move is the same either way:
   * a sellable quantity nobody could establish is unknown, and unknown refuses. The reason is in
   * `last_error()` for the report.
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

  /** Whether asking this broker for a sellable quantity costs a request. */
  static constexpr bool inquires_sellable = Caps::sellable_source == SellableSource::INQUIRY;

  /**
   * Returns what this broker's inquiry answer is evidence of.
   *
   * The runner writes this beside the figure rather than letting it inherit the holdings read's
   * basis, because an inquiry is a separate reading: the holdings poll established nothing about a
   * number it never reported.
   */
  static constexpr SellableBasis sellable_inquiry_basis() {
    // Detected on `Caps` rather than read off it, because `Caps` is a caller's type parameter and
    // need not be `BrokerCaps`: a hand-written capability set states the constants it cares about.
    // One that says nothing about a basis has established nothing, so the guard refuses -- the same
    // fail-closed default a third-party adapter's traits get.
    return caps_detail::stated_inquiry_basis<Caps>::value;
  }

  /** Returns the reference the broker issued for the last accepted order. */
  const std::optional<OrderRef<BrokerTag>>& last_ref() const noexcept { return last_ref_; }

  /** Returns why the last instruction failed, for the runner to log. */
  const std::optional<BrokerError>& last_error() const noexcept { return last_error_; }

private:
  /** Asks the session's traits where `requested` goes, where they say. */
  template <typename S>
  static constexpr auto traits_route(Venue requested, int) noexcept
    -> decltype(S::Traits::effective_route(requested)) {
    return S::Traits::effective_route(requested);
  }

  /** The fallback, for traits that do not say: the requested route is the most that is known. */
  template <typename S>
  static constexpr Venue traits_route(Venue requested, long) noexcept { return requested; }

  Session* session_;
  Venue default_route_;
  std::optional<OrderRef<BrokerTag>> last_ref_;
  std::optional<BrokerError> last_error_;
};

} // namespace live
} // namespace algotradelib
