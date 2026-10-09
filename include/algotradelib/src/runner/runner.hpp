#pragma once

#include "./context.hpp"
#include "./dispatch.hpp"
#include "./execution.hpp"
#include "./journal.hpp"

#include "../strategy/reconcile.hpp"
#include "../strategy/risk.hpp"
#include "../strategy/sellable_convention.hpp"

#include <algotradelib/core>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

namespace runner_detail {

// Whether an executor can ask the broker what may be sold of one instrument.
//
// Detected rather than required, for the same reason the strategy's handlers are: a dry runner, a
// replay's queueing executor and a test double have no broker to ask, and none of them should have
// to grow a method that returns nothing. An executor bound to a broker whose holdings already carry
// the figure declares it too and answers nothing, so the runner's own condition stays one test.
template <typename Executor, typename = void>
struct fills_sellable : std::false_type {};

template <typename Executor>
struct fills_sellable<
  Executor,
  std::void_t<
    decltype(std::declval<Executor&>().sellable_quantity(std::declval<const InstrumentId&>()))
  >
> : std::true_type {};

// Whether `Executor` states what its sellable *inquiry* answer is evidence of.
//
// Detected rather than required, and the default is what matters: an executor that answers a
// sellable quantity and says nothing about its basis is treated as having established nothing, so
// the guard refuses rather than measuring a sell against a figure nobody characterized. A consumer's
// own executor gets that default without being told it exists, which is the point -- the alternative
// default would silently enforce against whatever its author happened to return.
template <typename Executor, typename = void>
struct states_sellable_basis : std::false_type {};

template <typename Executor>
struct states_sellable_basis<
  Executor,
  std::void_t<decltype(Executor::sellable_inquiry_basis())>
> : std::true_type {};

// Returns what `Executor`'s inquiry answer is evidence of, or an unestablished basis.
template <typename Executor>
constexpr SellableBasis sellable_inquiry_basis_of() {
  if constexpr (states_sellable_basis<Executor>::value) {
    return Executor::sellable_inquiry_basis();
  } else {
    return SellableBasis{};
  }
}

// Whether `Executor` can say which reference the broker issued for the order it just accepted.
//
// Detected rather than required, for the same reason as above: a dry runner and a replay's
// queueing executor have no broker to have been answered by. A reservation written without one is
// still worth writing -- a limit that ignored the order would be the worse failure -- but it cannot
// be cancelled by reference until a listing supplies one.
template <typename Executor, typename = void>
struct reports_last_ref : std::false_type {};

template <typename Executor>
struct reports_last_ref<
  Executor, std::void_t<decltype(std::declval<const Executor&>().last_ref())>
> : std::true_type {};

// Whether `Executor` decides which venue an order that names none is sent to.
//
// Detected rather than required: only a broker binding routes, and a dry runner, an observer and a
// replay's queueing executor send the planned venue as it stands. `live::SessionExecutor` answers
// with its `default_route()`, and a `JournalingExecutor` forwards its inner executor's answer.
template <typename Executor, typename = void>
struct resolves_route : std::false_type {};

template <typename Executor>
struct resolves_route<
  Executor, std::void_t<decltype(std::declval<const Executor&>().requested_route(Venue::UNSPECIFIED))>
> : std::true_type {};

// Whether `Executor` can say **why** the last instruction failed.
//
// Detected rather than required, on the same terms as the two above: a dry runner refuses nothing
// and a replay's queueing executor has no broker to be refused by, so neither should have to grow a
// method that answers nothing. An executor that binds a session declares
// `const std::optional<BrokerError>& last_error() const` and its refusals then reach
// `Runner::last_broker_refusal`, where a report can carry the broker's own code and message instead
// of a count with nothing to chase.
//
// **The *shape* is detected, not the name.** An earlier version asked only whether `last_error()`
// was a valid expression, which would have selected this branch for any executor that happened to
// have a method of that name -- one returning `std::optional<std::string>`, say -- and then failed
// to compile on `broker_code_`. A name is not a contract; the return type is.
template <typename Executor, typename = void>
struct states_last_error : std::false_type {};

template <typename Executor>
struct states_last_error<
  Executor,
  std::enable_if_t<
    std::is_same_v<
      std::decay_t<decltype(std::declval<const Executor&>().last_error())>,
      std::optional<BrokerError>
    >
  >
> : std::true_type {};

// Whether `Executor` journals what it sends (`JournalingExecutor`); a runner sending through one to a
// broker is serialized unless told otherwise (`Runner::allow_overlapping_orders`).
template <typename Executor, typename = void>
struct journals : std::false_type {};

template <typename Executor>
struct journals<Executor, std::void_t<decltype(std::declval<const Executor&>().journal_failures())>>
  : std::true_type {};

// Whether `Executor` writes what serialization needs: the holding an order was decided against, the
// line it wrote for the order, and a line the runner builds (a discharge or a hold).
template <typename Executor, typename = void>
struct serializes : std::false_type {};

template <typename Executor>
struct serializes<
  Executor,
  std::void_t<
    decltype(std::declval<Executor&>().place(
      std::declval<const strategy::PlannedOrder&>(), std::optional<Decimal64>{}
    )),
    decltype(std::declval<const Executor&>().last_entry()),
    decltype(std::declval<Executor&>().record(std::declval<JournalEntry&>()))
  >
> : std::true_type {};

} // namespace runner_detail

/**
 * @brief A cancellation a hold refused (nothing was sent), and which hold.
 * @tparam BrokerTag The broker tag.
 */
template <typename BrokerTag>
struct RefusedCancel {
  /** @brief The order that was to be cancelled. */
  OrderRef<BrokerTag> ref_;
  /** @brief The hold that stopped it. */
  strategy::HaltReason reason_{strategy::HaltReason::OPERATOR};
};

/**
 * @brief A cancellation that was refused, by the broker or by the binding.
 * @tparam BrokerTag The broker tag.
 */
template <typename BrokerTag>
struct RejectedCancel {
  /** @brief The order that was to be cancelled. */
  OrderRef<BrokerTag> ref_;
  /** @brief `true` if the broker refused it; `false` if the binding refused to send it. */
  bool by_broker_{true};
};

/**
 * @brief What one runner pass decided and did, sorted by outcome (returned by `Runner::feed`).
 *
 * The buckets list outcomes of orders, cancellations and plans, not one entry per instruction: a
 * target already met adds nothing, and one target can produce several orders and cancellations.
 *
 * @tparam BrokerTag The broker tag.
 */
template <typename BrokerTag>
struct DryRunReport {
  /** @brief Orders that passed every check and were sent (in a dry run, would have been). */
  std::vector<strategy::PlannedOrder> would_place_;
  /**
   * @brief Cancellations the broker accepted.
   *
   * Accepted is not done: cancellation is asynchronous, so the order may still be working (or fill)
   * until a listing says otherwise. Every other cancellation outcome has its own bucket.
   */
  std::vector<OrderRef<BrokerTag>> would_cancel_;
  /**
   * @brief Instructions not placed: reconciliation could not build them, the broker refused them
   *   (`BROKER_REFUSED`), or the binding refused to send them (`BINDING_REFUSED`).
   *
   * In each case the order does not exist and nothing needs reconciling.
   */
  std::vector<strategy::Rejection> rejected_;
  /** @brief Instructions the risk guard refused (a limit or a hold). */
  std::vector<strategy::GuardRejection> refused_;
  /**
   * @brief Orders held back because an earlier order for the instrument has an unknown outcome, or
   *   local machinery cannot send; not retried automatically.
   */
  std::vector<WithheldOrder> withheld_;
  /**
   * @brief Whole plans (cancellations and placements) held back before any part was sent; see
   *   `WithheldPlanReason`.
   */
  std::vector<WithheldPlan<BrokerTag>> withheld_plans_;
  /**
   * @brief Orders whose outcome is unknown (`SendOutcome::INDETERMINATE`, which at KIS, Toss and
   *   Kiwoom includes most of the broker's own refusals); the instrument now needs reconciling.
   */
  std::vector<strategy::PlannedOrder> indeterminate_;
  /**
   * @brief Cancellations that were refused, by the broker or the binding.
   *
   * The order may still be working, or may have just filled (a common reason to refuse).
   */
  std::vector<RejectedCancel<BrokerTag>> rejected_cancel_;
  /** @brief Cancellations with an unknown outcome; the order may or may not still rest. */
  std::vector<OrderRef<BrokerTag>> indeterminate_cancel_;
  /** @brief Cancellations naming an order the portfolio does not list; not sent. */
  std::vector<OrderRef<BrokerTag>> unresolved_cancel_;
  /**
   * @brief Orders not sent because local machinery was unavailable (`SendOutcome::UNAVAILABLE`, such
   *   as a failed journal write); the runner stops sending.
   */
  std::vector<WithheldOrder> unavailable_;
  /** @brief Cancellations not sent because local machinery was unavailable. */
  std::vector<OrderRef<BrokerTag>> unavailable_cancel_;
  /**
   * @brief Cancellations refused by a hold that blocks withdrawals (`OPERATOR`, `STOPPING`; see
   *   `strategy::halt_blocks`).
   *
   * A hold that only means data is missing (such as `PORTFOLIO_STALE`) does not refuse a named
   * cancellation; `LOCAL_FAILURE` puts it in `unavailable_cancel_` instead.
   */
  std::vector<RefusedCancel<BrokerTag>> refused_cancel_;
  /** @brief Lines the strategy logged during the pass. */
  std::vector<std::string> log_;

  /**
   * @brief Tests whether the pass did nothing at all.
   * @return `true` if every bucket except `log_` is empty.
   */
  bool quiet() const noexcept {
    // Every outcome bucket belongs here; one left out would report as silence.
    return would_place_.empty() && would_cancel_.empty() && rejected_.empty() &&
      refused_.empty() && withheld_.empty() && withheld_plans_.empty() && indeterminate_.empty() &&
      unresolved_cancel_.empty() && unavailable_.empty() && unavailable_cancel_.empty() &&
      refused_cancel_.empty() && indeterminate_cancel_.empty() && rejected_cancel_.empty();
  }
};

/** @brief The broker tag of a dry run, whose order references reach no broker. */
struct DryTag {
  /** @brief The name an order reference prints: `"dryrun"`. */
  static constexpr std::string_view name = "dryrun";
};

/**
 * @brief Runs one strategy: feeds it events, reconciles what it asks for, checks the risk limits,
 *   and sends through an executor.
 *
 * The same class serves a dry run (the default `RecordingExecutor`, which sends nothing), a live
 * session (`live::SessionExecutor`) and a backtest (`backtest::QueueingExecutor`). Feed events with
 * `feed`; each call runs the matching handler and returns a `DryRunReport` of what it decided. A
 * live program usually drives it through `runner::PollingLoop`.
 *
 * A dry-run program contains no broker or network code: this header includes none.
 *
 * @tparam Strategy The strategy type, held by value (read its state with `strategy()`).
 * @tparam CapsType The broker capabilities the strategy sees.
 * @tparam BrokerTag The broker tag of order references.
 * @tparam Executor Where instructions are sent.
 */
template <
  typename Strategy,
  typename CapsType = strategy::NoCaps,
  typename BrokerTag = DryTag,
  typename Executor = RecordingExecutor<DryTag>
>
class Runner {
public:
  /** @brief The broker tag (`DryTag` for a dry run). */
  using Tag = BrokerTag;
  /** @brief The context type handed to the strategy's handlers. */
  using Context = runner::Context<BrokerTag, CapsType>;
  /** @brief What one `feed` returns. */
  using Report = DryRunReport<BrokerTag>;

  ATL_ASSERT_STRATEGY(Strategy, Context);

  /**
   * @brief `true` when this runner sends to a broker and the strategy defines `on_fill`, which then
   *   never fires (no shipped live source produces fills).
   *
   * Nothing is refused; `PollingLoop` puts a note in its report. A program that feeds `Fill` events
   * itself does reach `on_fill`.
   */
  static constexpr bool on_fill_never_fires =
    runner_detail::sends_to_a_broker<Executor>::value &&
    strategy::strategy_detail::declares_on_fill<Strategy>;

  /**
   * @brief `true` when this runner keeps each order it sends in `Portfolio::working_` until a broker
   *   listing accounts for it: a live runner whose broker lists working orders (every shipped
   *   binding).
   *
   * `PollingLoop` then requires the working-order poll (see `PollSchedule::working_orders_`).
   */
  static constexpr bool keeps_sent_orders_until_listed =
    runner_detail::sends_to_a_broker<Executor>::value && CapsType::has_working_orders;

  /**
   * @brief `true` when this runner journals what it sends to a broker (a `JournalingExecutor`), which
   *   makes it serialized unless `allow_overlapping_orders` is called; see `serialized`.
   */
  static constexpr bool journals_to_a_broker =
    runner_detail::sends_to_a_broker<Executor>::value && runner_detail::journals<Executor>::value;

  /**
   * @brief Constructs a runner.
   * @param strategy The strategy.
   * @param limits The risk limits.
   * @param series_capacity How many trailing bars each cached series keeps; must be positive with a
   *   linear algebra backend, or the first bar fed throws `std::invalid_argument`.
   * @param executor Where instructions are sent.
   */
  explicit Runner(
    Strategy strategy = {},
    strategy::RiskLimits limits = {},
    std::size_t series_capacity = 512,
    Executor executor = {}
  )
  : strategy_(std::move(strategy)),
    guard_(std::move(limits)),
    context_(series_capacity),
    executor_(std::move(executor)) {}

  /**
   * @brief Returns the executor.
   * @return The executor.
   */
  const Executor& executor() const noexcept { return executor_; }
  /**
   * @brief Returns the executor.
   * @return The executor.
   */
  Executor& executor() noexcept { return executor_; }

  /**
   * @brief Returns the instruments with an order of unknown outcome.
   * @return The ledger.
   */
  const IndeterminacyLedger& ledger() const noexcept { return ledger_; }
  /**
   * @brief Returns the ledger, so an operator who has read the broker's order list can clear an entry.
   * @return The ledger.
   */
  IndeterminacyLedger& ledger() noexcept { return ledger_; }

  /**
   * @brief Returns why local machinery stopped sending, if it did.
   *
   * The hold (`strategy::HaltReason::LOCAL_FAILURE`) stays until `resume_after_local_failure`, even
   * if the failure was transient. The detail is the first failure recorded through this runner.
   *
   * @return The detail, or `std::nullopt` while sending works.
   */
  std::optional<std::string> local_failure() const {
    if (!guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      return std::nullopt;
    }
    return guard_.hold_detail(strategy::HaltReason::LOCAL_FAILURE);
  }

  /**
   * @brief Clears the local-failure hold, after an operator has fixed the cause.
   *
   * It also acknowledges the journal failures seen so far, so the same failure does not halt again.
   * Other holds, such as an operator's halt, stay.
   */
  void resume_after_local_failure() {
    guard_.resume(strategy::HaltReason::LOCAL_FAILURE);
    acknowledged_journal_failures_ = executor_journal_failures();
  }

  /**
   * @brief Sets how old the holdings may be before a reconciliation plan computed from them is held
   *   back (`WithheldPlanReason::PORTFOLIO_STALE`).
   *
   * It applies to every plan that would cancel something, and to placement-only plans while
   * `require_known_portfolio` is on. It takes no hold on the guard. Age is measured against the later
   * of `observed_at()` and `Context::now()`. `PollingLoop` sets it from its own configuration.
   *
   * @param age The maximum age, or `std::nullopt` for no limit.
   */
  void set_maximum_portfolio_age(std::optional<std::chrono::nanoseconds> age) {
    maximum_portfolio_age_ = age;
  }

  /**
   * @brief Sets the current wall-clock instant, used to measure the age of holdings and price bands.
   *
   * `PollingLoop` sets it each iteration. Without it, age is measured against `Context::now()`, which
   * for a daily bar is its period end and understates age.
   *
   * @param at The instant.
   */
  void set_observed_at(std::chrono::system_clock::time_point at) { observed_at_ = at; }

  /**
   * @brief Returns the instant last set by `set_observed_at`.
   * @return The instant, or the epoch if never set.
   */
  std::chrono::system_clock::time_point observed_at() const noexcept { return observed_at_; }

  /**
   * @brief Lets this runner place orders before holdings have ever been read.
   *
   * `PollingLoop::allow_absent_portfolio` calls it. A plan that would cancel something is still held
   * back until holdings are read.
   */
  void allow_absent_portfolio() { allow_absent_portfolio_ = true; }

  /**
   * @brief Sets whether placing requires holdings to have been read and fresh.
   *
   * On by default for an executor that can reach a broker, off for a dry run or a replay. Off, a
   * placement-only plan skips both the read and the age check. `PollingLoop` sets it each iteration.
   * A plan that would cancel something is held back unless holdings have been read and are fresh,
   * whatever this setting.
   *
   * @param required Whether placing requires read holdings.
   */
  void require_known_portfolio(bool required) { require_known_portfolio_ = required; }

  /**
   * @brief Stops this runner sending, with the `LOCAL_FAILURE` hold, until
   *   `resume_after_local_failure`.
   *
   * Used, for example, by recovery when a damaged journal shows something was in flight but not what.
   *
   * @param detail Why, for the operator.
   */
  void halt_locally(std::string detail) { note_local_failure(std::move(detail)); }

  /**
   * @brief Returns how many placements may have left an order at the broker (accepted or of unknown outcome).
   * @return The count over the runner's life; compare it before and after to tell whether a listing is out of date.
   */
  std::size_t placements_at_broker() const noexcept { return placements_at_broker_; }

  /**
   * @brief Returns how many orders were sent in a market phase `RiskLimits::permitted_phases_` does
   *   not permit, because they close toward flat.
   *
   * Counts sends, including ones the broker then rejected (likely outside a session). Not by itself
   * a fault: it shows a flatten after the close, or a strategy restating a close every pass (limit
   * that with `RiskLimits::max_orders_per_window_`).
   *
   * @return The count over the runner's life.
   */
  std::size_t sends_outside_permitted_phase() const noexcept {
    return sends_outside_permitted_phase_;
  }

  /**
   * @brief Turns on the `strategy::SellableConventionProbe`, which infers whether the broker's
   *   sellable figure already excludes resting sells.
   *
   * Off by default. It only observes; it sends and requests nothing extra. Read the answer from
   * `sellable_convention()`.
   *
   * @param enabled Whether to probe.
   */
  void probe_sellable_convention(bool enabled = true) { probing_sellable_ = enabled; }

  /**
   * @brief Shows the current portfolio to the sellable-convention probe, if it is on.
   *
   * Called on every pass and by `PollingLoop` after it polls. Skipped while the working-order listing
   * is newer than the holdings.
   */
  void observe_sellable_convention() {
    // Skipped, not forgotten, while the listing is newer than the holdings: the resting quantity
    // and the sellable figure would come from two moments, and a sell starting to rest between them
    // reads as a figure that did not move. The baseline stays the last paired reading.
    if (probing_sellable_ && !context_.portfolio().listing_newer_than_holdings()) {
      sellable_probe_.observe(context_.portfolio());
    }
  }

  /**
   * @brief Returns the sellable-convention probe.
   * @return What it has inferred, with its counts.
   */
  const strategy::SellableConventionProbe<Tag>& sellable_convention() const noexcept {
    return sellable_probe_;
  }

  /**
   * @brief Returns the strategy, to read the state it kept.
   * @return The strategy.
   */
  const Strategy& strategy() const noexcept { return strategy_; }
  /**
   * @brief Returns the strategy.
   * @return The strategy.
   */
  Strategy& strategy() noexcept { return strategy_; }

  /**
   * @brief Returns the context.
   * @return The context.
   */
  const Context& context() const noexcept { return context_; }
  /**
   * @brief Returns the context, to arrange its state (instruments, portfolio) before feeding events.
   * @return The context.
   */
  Context& context() noexcept { return context_; }

  /**
   * @brief Returns the risk guard.
   * @return The guard.
   */
  const strategy::RiskGuard& guard() const noexcept { return guard_; }
  /**
   * @brief Returns the risk guard, to halt or resume trading.
   * @return The guard.
   */
  strategy::RiskGuard& guard() noexcept { return guard_; }

  /**
   * @brief Records an instrument's price band for today, used to clamp order prices.
   *
   * The band is dated by the earlier of the exchange's timestamp, where the broker gave one (Toss,
   * Kiwoom), and the instant last set with `set_observed_at` (the only date of a KIS band); a future
   * exchange timestamp cannot make it fresher. A band recorded before any instant was set is dated at
   * the epoch, so any realistic age limit rejects it. Its age is measured against the later of
   * `observed_at()` and `Context::now()`; a band dated after that instant is never applied, whatever
   * the policy. Whether a band is applied depends on `set_maximum_price_limits_age`.
   *
   * @param id The instrument (bands are kept per full identity, venue included).
   * @param limits The band.
   */
  void set_price_limits(const InstrumentId& id, PriceLimits limits) {
    DatedBand dated;
    dated.received_at_ = observed_at_;
    dated.limits_ = std::move(limits);
    price_limits_.insert_or_assign(id, std::move(dated));
  }

  /**
   * @brief Sets how old a price band may be and still be applied to an order's price.
   *
   * With no policy stated (the default), **no band is applied**; call
   * `allow_unbounded_price_limits_age` to apply bands of any age.
   *
   * A band not applied leaves the strategy's own price on the order, as if no band were known. That
   * has costs: the exchange may reject a price outside today's real band that the clamp would have
   * fixed (an exit below today's floor, say), or a local check (notional, buying power, exposure)
   * may refuse a buy priced above the ceiling. `unclamped_orders_rejected` and
   * `unclamped_orders_refused` count those; an order accepted at the raw price that never filled is
   * not counted.
   *
   * @param age The maximum age. `std::nullopt` withdraws the policy (so no band is applied); it does
   *   not mean "any age".
   */
  void set_maximum_price_limits_age(std::optional<std::chrono::nanoseconds> age) {
    maximum_price_limits_age_ = age;
    // This call is now the whole policy, so it also withdraws "any age".
    allow_unbounded_price_limits_age_ = false;
  }

  /** @brief Applies price bands of any age (replacing a stated maximum). */
  void allow_unbounded_price_limits_age() {
    allow_unbounded_price_limits_age_ = true;
    maximum_price_limits_age_.reset();
  }

  /**
   * @brief Returns how many plans were built while a band was known for the instrument but not applied (counted per plan, not per order).
   * @return The count over the runner's life.
   */
  std::size_t price_bands_withheld() const noexcept { return price_bands_withheld_; }

  /**
   * @brief Tests whether a price-band age policy has been stated (a maximum, or any age).
   * @return `true` if one has.
   */
  bool price_limits_age_policy_stated() const noexcept {
    return maximum_price_limits_age_.has_value() || allow_unbounded_price_limits_age_;
  }

  /**
   * @brief Returns how many sent orders the broker refused while carrying a price a withheld band
   *   would have clamped.
   *
   * Co-occurrence, not proof that the band caused the refusal. Orders refused before sending are in
   * `unclamped_orders_refused`.
   *
   * @return The count over the runner's life.
   */
  std::size_t unclamped_orders_rejected() const noexcept { return unclamped_orders_rejected_; }

  /**
   * @brief Returns how many orders carrying a price a withheld band would have clamped were refused
   *   before sending (by the risk guard or the binding; not counting `UNAVAILABLE`).
   * @return The count over the runner's life.
   */
  std::size_t unclamped_orders_refused() const noexcept { return unclamped_orders_refused_; }

  /**
   * @brief Returns how many orders reconciliation could not build (any `strategy::RejectReason`
   *   before sending, including a quantity that rounded to zero).
   *
   * One of six counters of what became of placements: `orders_unbuildable`,
   * `orders_reconcile_withheld`, `orders_guard_refused`, `orders_binding_refused`,
   * `orders_broker_refused`, `orders_indeterminate`. All count over the runner's life, so a loop
   * differences them; the per-instruction reasons are in each pass's `Report`. See also
   * `last_unbuildable`.
   *
   * @return The count.
   */
  std::size_t orders_unbuildable() const noexcept { return orders_unbuildable_; }
  /**
   * @brief Returns how many orders were held back because the instrument had an order of unknown outcome; see `orders_unbuildable`.
   * @return The count.
   */
  std::size_t orders_reconcile_withheld() const noexcept { return orders_reconcile_withheld_; }
  /**
   * @brief Returns how many orders the risk guard refused, for a limit or a hold; see `orders_unbuildable`.
   * @return The count.
   */
  std::size_t orders_guard_refused() const noexcept { return orders_guard_refused_; }
  /**
   * @brief Returns how many orders the binding refused to send (no request made); see `orders_unbuildable`.
   * @return The count.
   */
  std::size_t orders_binding_refused() const noexcept { return orders_binding_refused_; }
  /**
   * @brief Returns how many orders the broker refused (`SendOutcome::REJECTED`; a request was spent);
   *   see `last_broker_refusal`. At KIS, Toss and Kiwoom most refusals count as unknown outcomes
   *   instead.
   * @return The count.
   */
  std::size_t orders_broker_refused() const noexcept { return orders_broker_refused_; }
  /**
   * @brief Returns how many orders had an unknown outcome (`SendOutcome::INDETERMINATE`, which at
   *   KIS, Toss and Kiwoom includes most of the broker's own refusals).
   * @return The count.
   */
  std::size_t orders_indeterminate() const noexcept { return orders_indeterminate_; }

  /**
   * @brief Returns how many whole reconciliation plans were held back (any `WithheldPlanReason`:
   *   holdings absent, stale or superseded, or a placement hold); the reasons are in each
   *   `Report::withheld_plans_`.
   * @return The count over the runner's life.
   */
  std::size_t plans_withheld() const noexcept { return plans_withheld_; }

  /**
   * @brief Returns the targets waiting for a stop on their instrument to be cancelled (at most one per
   *   instrument).
   *
   * A target that needs a trade while a stop rests cancels the stop and waits
   * (`strategy::RejectReason::STOP_ORDER_RESTING`); the runner keeps it so it is not lost when no
   * strategy callback follows. It is:
   * - replaced by any later target or order intent the strategy states for the instrument;
   * - released (and settled) by `retry_deferred` once each stop it waits on is no longer listed and
   *   its history row is terminal with nothing filled;
   * - dropped without sending if a stop shows any fill (counted in `deferred_dropped_after_fill`; a
   *   partial fill's remainder is left for the strategy to restate);
   * - kept while a stop is still listed or its history is missing or not terminal.
   *
   * Not journalled: a restart loses it.
   *
   * @return The waiting targets.
   */
  std::vector<strategy::TargetPosition> deferred_targets() const {
    std::vector<strategy::TargetPosition> targets;
    targets.reserve(deferred_targets_.size());
    for (const auto& deferred : deferred_targets_) {
      targets.push_back(deferred.target_);
    }
    return targets;
  }

  /**
   * @brief Returns how many targets have waited for a stop's cancellation; see `deferred_targets`.
   * @return The count over the runner's life.
   */
  std::size_t targets_deferred() const noexcept { return targets_deferred_; }

  /**
   * @brief Returns how many waiting targets were dropped because a stop they waited on had filled.
   * @return The count over the runner's life.
   */
  std::size_t deferred_dropped_after_fill() const noexcept { return deferred_dropped_after_fill_; }

  /**
   * @brief Returns from when the broker's order history must be read to decide waiting targets.
   * @return The earliest instant a target whose stops are all unlisted began waiting, or
   *   `std::nullopt` if none can be decided from history yet.
   */
  std::optional<std::chrono::system_clock::time_point> deferred_awaiting_evidence() const {
    std::optional<std::chrono::system_clock::time_point> earliest;
    for (const auto& deferred : deferred_targets_) {
      if (stop_listed(deferred.target_.instrument_)) {
        continue;
      }
      if (!earliest || deferred.deferred_at_ < *earliest) {
        earliest = deferred.deferred_at_;
      }
    }
    return earliest;
  }

  /**
   * @brief Decides each waiting target against the broker's order history, and settles what it
   *   released.
   *
   * `live::SessionSource` calls it after a working-order poll. It calls no handler.
   *
   * @param evidence History rows covering the instants `deferred_awaiting_evidence` asked for.
   * @return The report of the settlement.
   */
  Report retry_deferred(const std::vector<OrderSnapshot>& evidence) {
    return release_deferred(&evidence);
  }

  /**
   * @brief Returns how many sent orders are still counted as reserved until the broker accounts for
   *   them.
   *
   * A runner that keeps sent orders (`keeps_sent_orders_until_listed`) remembers each order the
   * broker accepted with a reference, and each order of unknown outcome if its executor reports
   * references (has `last_ref()`, as `live::SessionExecutor` does), provided holdings had been read
   * and the instrument's net holding was known when it was sent. Before each pass it rebuilds a
   * reserved working row from what neither holdings (moved in the order's direction since it was
   * sent) nor a listing yet explains. An entry ends when holdings show the whole quantity, or the
   * broker's history shows the order ended with less (`confirm_from_history`). This stops a target
   * from ordering again while a fill has not reached the holdings yet.
   *
   * An accepted order not tracked (no reference, or sent before holdings were read, as
   * `allow_absent_portfolio` permits) has no such protection: its reservation lasts only until the
   * next listing. An order of unknown outcome not tracked (an executor without `last_ref()`) gets no
   * reservation at all once the ledger entry is resolved. Only orders sent in this process are
   * tracked; a restart starts with none.
   *
   * @return The number of such orders.
   */
  std::size_t unconfirmed_orders() const noexcept { return unconfirmed_.size(); }

  /**
   * @brief Rebuilds the reserved working rows against the portfolio as it is now.
   *
   * Every pass does this first; call it before reading `working_` outside a pass.
   */
  void refresh_unconfirmed() { confirm_reservations(); }

  /**
   * @brief Tells the runner what a reconciliation sweep found for an order of unknown outcome.
   *
   * With an order id (found in the broker's listing), the order is tracked like an accepted one;
   * without (it was never placed), it is forgotten with its reserved row. `live::SessionSource`
   * calls this for each recorded `MATCHED` or `NOT_PLACED` placement; a source of your own, or an
   * operator clearing the ledger by hand, should too, or the order's instrument is withheld again
   * after the confirmation timeout.
   *
   * A serialized runner (`serialized`) names only the order whose journal line is `journal_id`, and
   * forgets nothing: absence from a listing is not proof that the order was never placed, so the order
   * stays until holdings or an operator account for it (`release_obligations`).
   *
   * @param instrument The instrument.
   * @param side The order's side.
   * @param order_id The broker's order id, or `std::nullopt` if the order was not placed.
   * @param journal_id The id of the order's journal line; without it a serialized runner does nothing,
   *   and any other runner names its newest unnamed order on the instrument and side.
   */
  void resolve_indeterminate_placement(
    const InstrumentId& instrument, OrderSide side, const std::optional<std::string>& order_id,
    const std::string& journal_id = {}
  ) {
    if constexpr (keeps_sent_orders_until_listed) {
      if (order_id && order_id->empty()) {
        return;
      }
      auto found = unconfirmed_.end();
      if (serialized()) {
        // The exact order the sweep answered, or none: a guess could name another order, and a late
        // answer to one already accounted for finds nothing to change.
        found = std::find_if(unconfirmed_.begin(), unconfirmed_.end(), [&](const Unconfirmed& entry) {
          return !journal_id.empty() && entry.journal_ && entry.journal_->id_ == journal_id &&
            entry.ref_.primary_.empty();
        });
        if (found == unconfirmed_.end() || !order_id) {
          return;
        }
      } else {
        // The newest: the sweep answers the order just sent, and an older one still unnamed is left over
        // from a hold cleared by hand without naming it.
        const auto newest = std::find_if(unconfirmed_.rbegin(), unconfirmed_.rend(), [&](const Unconfirmed& entry) {
          return entry.ref_.primary_.empty() && entry.instrument_ == instrument && entry.side_ == side;
        });
        if (newest == unconfirmed_.rend()) {
          return;
        }
        found = std::prev(newest.base());
      }
      // The row first: it is recognized by the reference about to change, and left behind it would be
      // counted beside the rebuilt one.
      auto portfolio = context_.portfolio();
      portfolio.working_.erase(
        std::remove_if(
          portfolio.working_.begin(), portfolio.working_.end(),
          [&](const WorkingOrder<Tag>& working) { return is_reserved_row_of(working, *found); }
        ),
        portfolio.working_.end()
      );
      context_.set_portfolio(std::move(portfolio));
      if (order_id) {
        // The primary part only: a broker's row is matched on it everywhere, and the listing that
        // carries the order supplies the rest of the reference.
        found->ref_.primary_ = *order_id;
        found->row_.ref_.primary_ = *order_id;
        // Named now, so a fresh start: a hand-cleared hold may have let it miss its history meanwhile.
        found->history_missed_ = false;
        if (found->unexplained_since_) {
          found->unexplained_since_ = freshness_instant();
        }
      } else {
        unconfirmed_.erase(found);
      }
      confirm_reservations();
    } else {
      (void)instrument;
      (void)side;
      (void)order_id;
      (void)journal_id;
    }
  }

  /**
   * @brief Tests whether this runner is serialized: at most one order per instrument that holdings
   *   have not yet accounted for.
   *
   * On by default for a runner that journals what it sends to a broker (`journals_to_a_broker`:
   * `live::bind_journaled`, or a `JournalingExecutor` of your own). In this mode:
   * - a plan that would place on an instrument is withheld whole, its derived cancellations included
   *   (`WithheldPlanReason::AWAITING_CONFIRMATION`), while an order this runner placed there is not yet
   *   accounted for. So a protective stop cannot be placed beside an unconfirmed entry, a resting order
   *   holds its instrument until it has filled or been withdrawn and is accounted for, and a target cannot
   *   reprice its own order: cancel it with a `CancelIntent` and state the target again. A withheld target
   *   is kept and planned again after a newer holdings read; an order intent is reported, not kept. A
   *   target waiting for its stop's withdrawal (`deferred_targets`) waits for the stop to be accounted
   *   for too;
   * - an order is accounted for when a holdings read shows exactly the holding it was decided against
   *   plus what it filled (all of it, or what its own dated, ended history row says), or that row shows it
   *   ended with nothing filled. The journal records it (`JournalOutcome::CONFIRMED`) before the
   *   instrument is free again, and only a holdings read received after it sizes the next order;
   * - an order whose reply was lost is accounted for only once a sweep names it, and holdings that move
   *   past what an order can explain hold it for an operator (`JournalOutcome::HELD`); see
   *   `release_obligations`. A timeout marks the ledger but releases nothing;
   * - `runner::recover_runner` restores every order a previous process left unaccounted for, so a
   *   restart cannot send it again, and nothing is placed before it has run. Orders from an overlapping
   *   or older journal, or two on one instrument, are restored for an operator to release.
   *
   * It assumes this runner is the only one trading the account, that a holdings read received later
   * shows no older content, and that the journal's clock and the holdings' `as_of_` are one clock (as
   * they are live; a simulated market must journal on its own clock). Orders placed any other way are outside it, as is an executor that drives
   * this runner from inside its own `place`. A runner that journals to a broker but lacks what this
   * needs (a broker that lists working orders, and an executor with `last_entry` and `record`, as
   * `JournalingExecutor` has) places nothing until `allow_overlapping_orders` is called.
   *
   * @return Whether serialized.
   */
  bool serialized() const noexcept { return journals_to_a_broker && !overlapping_; }

  /**
   * @brief Lets several orders on one instrument be outstanding at once, as an unjournaled runner does,
   *   with that runner's known limitations.
   *
   * After a restart, orders a previous process sent and holdings had not yet shown are forgotten, and a
   * target can place them again. A buy and a sell on one instrument whose fills have not reached holdings
   * can both be taken as accounted for, and a later order confirmed by another's fill. Choose it only for
   * a strategy that needs overlapping orders (an entry beside its protective stop) and accepts those.
   *
   * @throws std::logic_error once anything was placed, or after `runner::recover_runner`.
   */
  void allow_overlapping_orders() {
    if (placements_at_broker_ != 0 || recovered_) {
      throw std::logic_error("allow_overlapping_orders must be chosen before recovery and before trading");
    }
    overlapping_ = true;
  }

  /**
   * @brief Returns what a serialized runner still owes on an instrument.
   * @param instrument The instrument.
   * @return The most restrictive state of its orders not yet accounted for: `OPERATOR` over `UNNAMED`
   *   over `NAMED`; `NONE` if there are none or the runner is not serialized.
   */
  ObligationState obligation_state(const InstrumentId& instrument) const {
    auto state = ObligationState::NONE;
    if (!serialized()) {
      return state;
    }
    for (const auto& entry : unconfirmed_) {
      if (!(entry.instrument_ == instrument)) {
        continue;
      }
      if (entry.operator_only_) {
        return ObligationState::OPERATOR;
      }
      if (entry.ref_.primary_.empty()) {
        state = ObligationState::UNNAMED;
      } else if (state == ObligationState::NONE) {
        state = ObligationState::NAMED;
      }
    }
    return state;
  }

  /**
   * @brief Accounts for every order a serialized runner still owes on an instrument, on an operator's
   *   word.
   *
   * For an order held for an operator, one whose reply was lost and no sweep named, one from an older
   * journal, or one whose holdings never arrived: read the broker's order list and holdings first. Each
   * order gets a `JournalOutcome::CONFIRMED` line naming `reason` and is then forgotten; the next order on
   * the instrument needs a holdings read received after this call. The indeterminacy ledger is left as it
   * is. If a line cannot be written, that order is kept and the runner stops sending
   * (`HaltReason::LOCAL_FAILURE`).
   *
   * @param instrument The instrument.
   * @param reason Why, for the journal.
   * @return `false` if any line could not be written.
   */
  bool release_obligations(const InstrumentId& instrument, const std::string& reason) {
    bool written = true;
    if constexpr (can_serialize) {
      for (auto at = unconfirmed_.begin(); at != unconfirmed_.end();) {
        if (!(at->instrument_ == instrument)) {
          ++at;
          continue;
        }
        auto line = closing_line(*at, JournalOutcome::CONFIRMED, std::nullopt, "released by an operator: " + reason);
        if (!executor_.record(line)) {
          written = false;
          note_executor_failure(false);
          ++at;
          continue;
        }
        set_boundary(instrument, line.recorded_at_, /*strict=*/true);
        drop_reserved_row(*at);
        at = unconfirmed_.erase(at);
      }
      confirm_reservations();
    } else {
      (void)instrument;
      (void)reason;
    }
    return written;
  }

  /**
   * @brief Restores the orders a previous process left unaccounted for; `runner::recover_runner` calls
   *   it, and a serialized runner places nothing until it has run.
   *
   * Every placement with no `CONFIRMED` or `REJECTED` line comes back, named by an `ACCEPTED` or
   * `RESOLVED` line's order id or else unnamed, measured from the holding its lines recorded. One with
   * no recorded holding (an overlapping or older journal), lines that disagree or were damaged, a `HELD`
   * line, or another such order on its instrument, is held for an operator and marks the ledger. Each
   * instrument's latest `CONFIRMED` line means only a holdings read received after it sizes the next
   * order. Does nothing else for a runner that is not serialized.
   *
   * @param replay The journal's contents.
   */
  void restore_obligations(const JournalReplay& replay) {
    recovered_ = true;
    if constexpr (can_serialize) {
      if (!serialized()) {
        return;
      }
      restore_from(replay);
    } else {
      (void)replay;
    }
  }

  /**
   * @brief Returns how many orders a serialized runner withheld because another order on the
   *   instrument was not yet accounted for, or the runner could not yet measure one; see `serialized`.
   *   Whole plans withheld for it are in `plans_withheld`.
   * @return The count over the runner's life.
   */
  std::size_t orders_awaiting_confirmation() const noexcept { return orders_awaiting_confirmation_; }

  /**
   * @brief Returns how many orders a serialized runner owes that nothing yet explains: no listing shows
   *   them resting and holdings do not show them, or only an operator may account for them.
   *
   * `PollingLoop` reports these after a closing pass, which cannot see them.
   *
   * @return The count; zero for a runner that is not serialized.
   */
  std::size_t obligations_unexplained() const noexcept {
    if (!serialized()) {
      return 0;
    }
    return static_cast<std::size_t>(std::count_if(unconfirmed_.begin(), unconfirmed_.end(), [](const Unconfirmed& entry) {
      return entry.operator_only_ || entry.residual_.coefficient() != 0;
    }));
  }

  /**
   * @brief Sets how long a sent order's quantity may stay unexplained before its instrument is
   *   withheld (ten minutes by default).
   *
   * When it passes, the broker's history is read once more; if that does not settle it, or holdings
   * still do not show the filled quantity one more timeout later, the instrument is marked in the
   * indeterminacy ledger for an operator to clear. Too short a timeout costs trading on that
   * instrument, never a duplicate order. It needs a source that advances `Portfolio::as_of_` on each
   * holdings read.
   *
   * @param timeout The timeout.
   */
  void set_confirmation_timeout(std::chrono::nanoseconds timeout) { confirmation_timeout_ = timeout; }

  /**
   * @brief Returns how many sent orders timed out unconfirmed, each withholding its instrument.
   * @return The count over the runner's life.
   */
  std::size_t orders_unconfirmed_timed_out() const noexcept { return orders_unconfirmed_timed_out_; }

  /**
   * @brief Returns from when the broker's order history must be read to settle sent orders.
   *
   * An order needs it when part of it is unexplained and either it passed the confirmation timeout
   * (listed or not), or no listing carries it and it was listed before or its cancellation was
   * accepted. Excluded: an order the history already settled; one whose history had no answer after
   * the timeout (until a later listing or holdings explain it again); and an order of unknown outcome
   * with no reference while its instrument is in the ledger.
   *
   * @return The earliest such order's send instant, or `std::nullopt` if none needs history.
   */
  std::optional<std::chrono::system_clock::time_point> unconfirmed_awaiting_evidence() {
    // Against the portfolio as it stands: a listing just installed may carry, or no longer carry, an
    // order, and no pass has run since.
    confirm_reservations();
    std::optional<std::chrono::system_clock::time_point> earliest;
    const auto now = freshness_instant();
    for (const auto& entry : unconfirmed_) {
      if (!awaits_evidence(entry, now)) {
        continue;
      }
      if (!earliest || entry.sent_at_ < *earliest) {
        earliest = entry.sent_at_;
      }
    }
    return earliest;
  }

  /**
   * @brief Settles reserved orders against the broker's order history; sends nothing.
   *
   * A terminal row for an order caps the quantity holdings must still show at its filled quantity;
   * an order that ended with nothing filled is forgotten at once. After the runner has seen the phase
   * `CLOSED` or `POST_CLOSE` since the order was sent, an open row of a `DAY` order counts as
   * terminal (its filled quantity is final). An order the history settled, or had no answer for
   * after the timeout, withholds its instrument (`orders_unconfirmed_timed_out`) if a holdings read
   * a timeout later still does not show it; see `set_confirmation_timeout`.
   *
   * @param evidence History rows covering the instants `unconfirmed_awaiting_evidence` asked for.
   */
  void confirm_from_history(const std::vector<OrderSnapshot>& evidence) {
    confirm_reservations();
    const auto now = freshness_instant();
    for (auto& entry : unconfirmed_) {
      entry.consulted_ = awaits_evidence(entry, now);
      if (!entry.consulted_) {
        continue;
      }
      // An entry not yet named matches no row: a row whose number is missing would read as equal.
      if (entry.ref_.primary_.empty()) {
        continue;
      }
      const auto* row = history_row_for(
        evidence, entry.ref_.primary_, entry.instrument_, entry.side_,
        entry.sent_at_ - order_time_tolerance_, entry.sent_at_ + order_time_tolerance_
      );
      // Serialized: only a dated row that ended the order and filled no more than it, and none while a
      // listing still shows the order open, since one of the two is wrong. A row an undated read
      // returned, or a session's close, says nothing: holdings can still show the whole quantity.
      if (row != nullptr && serialized() &&
          (!row->ordered_at_ || entry.listed_now_ || !ends_the_order(row->status_) ||
           decimal_less(entry.row_.order_.quantity_, row->execution_.filled_quantity_))) {
        row = nullptr;
      }
      if (row != nullptr && ((entry.session_ended_ && !serialized()) || !strategy::order_is_open(row->status_))) {
        if (decimal_less(row->execution_.filled_quantity_, entry.expected_)) {
          entry.expected_ = row->execution_.filled_quantity_;
        }
        entry.settled_ = true;
        // Holdings get a full timeout from here to show it (`confirm_reservations`).
        entry.unexplained_since_ = now;
      }
    }
    // Past the timeout and the history had no answer: noted, and judged by `confirm_reservations` against
    // the next holdings read, as a settled order is. Marking here, on the clock, marked a fill one
    // holdings read early on a loop polling slower than the timeout (the listing, and so this history,
    // is read before holdings in each iteration).
    for (auto& entry : unconfirmed_) {
      if (entry.consulted_ && !entry.settled_ && timed_out(entry, now)) {
        entry.history_missed_ = true;
      }
    }
    // Retires what the history just settled, and withholds what the holdings in hand already condemn.
    confirm_reservations();
  }

  /**
   * @brief Re-submits retained targets and kept intents once they can be acted on, and settles them.
   *
   * The runner keeps (rather than drops):
   * - a target withheld as `PORTFOLIO_ABSENT`, `PORTFOLIO_STALE` or `PORTFOLIO_SUPERSEDED`, until
   *   holdings are usable (any runner, a dry run included);
   * - a target or order intent refused for the market phase (a daily strategy deciding while the
   *   market is shut), until `RiskLimits::permitted_phases_` admits the phase;
   * - a target or order intent the broker could not be reached for, or refused for its rate limit,
   *   until a holdings read newer than the failure arrives.
   *
   * The last two only in a runner that sends to a broker. Not kept: anything decided under the
   * `STOPPING`, `OPERATOR` or `LOCAL_FAILURE` hold; an intent with a client order id the broker
   * answered; an intent whose instrument's net holding is unknown. So a protective stop with a client
   * order id, refused out of hours, is not installed at the open by itself (one without an id is).
   *
   * At release, a kept intent is dropped (`kept_intents_dropped`) if the instrument's holding
   * changed since (except a `TARGET_WEIGHT` intent, which is planned again against the new holding),
   * or if it carries a trigger and a stop now rests on the instrument.
   *
   * Nothing is released while holdings were never read or predate the listing, or while any hold is
   * in force. If this method runs while `OPERATOR` or `LOCAL_FAILURE` holds, it discards every kept
   * intent and every target kept for the phase or the broker (targets kept for the account stay).
   * Halting and resuming without calling it in between discards nothing, so the kept work can still
   * go out after the resume; call `clear_retained_targets` to drop it. A later target or intent for
   * the instrument replaces a retained one. `PollingLoop` calls this once per iteration; a program
   * driving a `Runner` itself must call it (or `clear_retained_targets`).
   *
   * @return The report of the settlement, empty if nothing was released.
   */
  Report retry_retained_targets() {
    // Once per iteration, so the context shows what this iteration's holdings confirmed even when no
    // pass runs (`unconfirmed_orders`).
    confirm_reservations();
    if (guard_.halted(strategy::HaltReason::OPERATOR) ||
        guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      retained_targets_.erase(
        std::remove_if(
          retained_targets_.begin(), retained_targets_.end(),
          [](const RetainedTarget& kept) { return kept.for_phase_; }
        ),
        retained_targets_.end()
      );
      kept_intents_.clear();
    }
    if ((retained_targets_.empty() && kept_intents_.empty()) ||
        context_.portfolio().as_of_ == std::chrono::system_clock::time_point{} ||
        context_.portfolio().listing_newer_than_holdings() || guard_.blocking_reason()) {
      return Report{};
    }
    const bool phase_permits = guard_.permits_phase(context_.phase());
    std::vector<RetainedTarget> kept;
    bool released = false;
    const auto as_of = context_.portfolio().as_of_;
    // Kept because the broker could not be reached: not offered again until it has answered since.
    const auto awaiting_broker = [&](const auto& entry) { return entry.after_ && !(*entry.after_ < as_of); };
    for (auto& entry : retained_targets_) {
      if ((entry.for_phase_ && !phase_permits) || awaiting_broker(entry)) {
        kept.push_back(std::move(entry));
        continue;
      }
      context_.submit(std::move(entry.target_));
      released = true;
    }
    retained_targets_ = std::move(kept);
    // Kept intents follow, in order, once the phase permits; each goes out only if the holding it was
    // decided against is unchanged (and, for a stop, no stop has come to rest meanwhile).
    if (phase_permits && !kept_intents_.empty()) {
      auto intents = std::move(kept_intents_);
      kept_intents_.clear();
      for (auto& entry : intents) {
        if (awaiting_broker(entry)) {
          kept_intents_.push_back(std::move(entry));
          continue;
        }
        const auto held = context_.portfolio().net_quantity_of(entry.intent_.instrument());
        const auto gap = held ? decimal_subtract(*held, entry.held_) : std::nullopt;
        // A weight is planned again as the position it states, against the holding at release, so
        // the book it was decided against does not bind it.
        const bool weight = entry.intent_.size().kind() == strategy::SizeKind::TARGET_WEIGHT;
        const bool same_book = weight || (gap && gap->coefficient() == 0);
        const bool stop_now_rests = entry.intent_.trigger() && stop_listed(entry.intent_.instrument());
        if (!same_book || stop_now_rests) {
          ++kept_intents_dropped_;
          continue;
        }
        context_.submit(std::move(entry.intent_));
        released = true;
      }
    }
    if (!released) {
      return Report{};
    }
    return settle();
  }

  /**
   * @brief Returns how many order intents are kept for the market phase, an unreachable broker or a
   *   rate limit; see `retry_retained_targets`.
   * @return The number kept now.
   */
  std::size_t intents_kept_for_phase() const noexcept { return kept_intents_.size(); }

  /**
   * @brief Returns how many kept intents were dropped at release, because the holding they were
   *   decided against had changed or a stop now rests.
   * @return The count over the runner's life.
   */
  std::size_t kept_intents_dropped() const noexcept { return kept_intents_dropped_; }

  /**
   * @brief Returns how many targets are retained, for any reason; see `retry_retained_targets`.
   * @return The number retained now.
   */
  std::size_t targets_retained() const noexcept { return retained_targets_.size(); }

  /**
   * @brief Returns how many retained targets wait for the market phase, an unreachable broker or a
   *   rate limit.
   * @return The number retained now.
   */
  std::size_t targets_retained_for_phase() const noexcept {
    return static_cast<std::size_t>(std::count_if(
      retained_targets_.begin(), retained_targets_.end(),
      [](const RetainedTarget& kept) { return kept.for_phase_; }
    ));
  }

  /** @brief Discards every retained target and kept intent (`PollingLoop` does this when stopping). */
  void clear_retained_targets() noexcept {
    retained_targets_.clear();
    kept_intents_.clear();
  }

  /**
   * @brief Releases and settles every waiting target whose stops are no longer listed, without
   *   reading history.
   *
   * Only for a replay, whose ledger is the broker: a stop it withdrew cannot have filled. A live
   * caller uses `retry_deferred`.
   *
   * @return The report of the settlement.
   */
  Report retry_deferred_on_one_snapshot() { return release_deferred(nullptr); }

  /**
   * @brief Returns how many instructions (placements and cancellations) were not sent because local machinery was unavailable; each leaves the `LOCAL_FAILURE` hold.
   * @return The count over the runner's life.
   */
  std::size_t instructions_unavailable() const noexcept { return instructions_unavailable_; }

  /**
   * @brief Returns how many cancellations were refused, by the broker or the binding (`RejectedCancel::by_broker_` tells which).
   * @return The count over the runner's life.
   */
  std::size_t cancels_rejected() const noexcept { return cancels_rejected_; }
  /**
   * @brief Returns how many cancellations a hold refused before sending.
   * @return The count over the runner's life.
   */
  std::size_t cancels_refused() const noexcept { return cancels_refused_; }
  /**
   * @brief Returns how many cancellations named an order the portfolio does not list.
   * @return The count over the runner's life.
   */
  std::size_t cancels_unresolved() const noexcept { return cancels_unresolved_; }
  /**
   * @brief Returns how many cancellations had an unknown outcome (the order may or may not have been cancelled).
   * @return The count over the runner's life.
   */
  std::size_t cancels_indeterminate() const noexcept { return cancels_indeterminate_; }

  /**
   * @brief Returns the broker's code and message for the most recent order it refused
   *   (`SendOutcome::REJECTED`; at KIS, Toss and Kiwoom most refusals are `INDETERMINATE` and not
   *   recorded here).
   * @return The refusal, or `std::nullopt` if none (or the executor cannot report errors, as in a dry
   *   run or replay). A refusal with no detail clears it, so a nonzero `orders_broker_refused` can
   *   sit beside an empty value.
   */
  const std::optional<BrokerRefusal>& last_broker_refusal() const noexcept {
    return last_broker_refusal_;
  }

  /**
   * @brief Returns why reconciliation could not build the most recent instruction it refused.
   *
   * For example, a live weight-sized intent is refused `NO_PORTFOLIO_VALUE` on every pass unless
   * `set_portfolio_value` was called.
   *
   * @return The rejection, or `std::nullopt` if none.
   */
  const std::optional<strategy::Rejection>& last_unbuildable() const noexcept {
    return last_unbuildable_;
  }

  /**
   * @brief Sets how small a drift from a target is not worth an order; see `strategy::TradeTolerance`.
   * @param tolerance The floors; all unset (the default) trades every share.
   */
  void set_trade_tolerance(strategy::TradeTolerance tolerance) {
    tolerance_ = std::move(tolerance);
  }

  /**
   * @brief Sets only the share-count floor of the trade tolerance.
   *
   * Across instruments at different price levels, prefer a notional or fractional floor.
   *
   * @param quantity The smallest drift worth trading, in shares.
   */
  void set_minimum_trade(Decimal64 quantity) {
    tolerance_.minimum_quantity_ = std::move(quantity);
  }

  /**
   * @brief Sets the portfolio's total value, which a weight-sized intent needs live.
   *
   * Without it a weighted intent is refused (`NO_PORTFOLIO_VALUE`). The value is taken to be in each
   * instrument's own currency; use the `Money` overload to state it.
   *
   * @param value The total value.
   */
  void set_portfolio_value(Decimal64 value) {
    portfolio_value_ = std::move(value);
    portfolio_value_currency_.reset();
  }

  /**
   * @brief Sets the portfolio's total value and its currency.
   *
   * A weight on an instrument in another currency is then refused. The value is kept until replaced
   * or cleared with `clear_portfolio_value`.
   *
   * @param value The total value and currency.
   */
  void set_portfolio_value(Money value) {
    portfolio_value_ = std::move(value.amount_);
    portfolio_value_currency_ = std::move(value.currency_);
  }

  /** @brief Forgets the portfolio value, so weighted intents are refused again. */
  void clear_portfolio_value() noexcept {
    portfolio_value_.reset();
    portfolio_value_currency_.reset();
  }

  /**
   * @brief Calls `on_start` and settles what it asked for.
   * @return The pass's report.
   */
  Report start() {
    dispatch::start(strategy_, context_);
    return settle();
  }

  /**
   * @brief Calls `on_stop` and settles what it asked for.
   *
   * For a caller driving a runner directly; `PollingLoop` uses `notify_stop` during a stop instead.
   *
   * @return The pass's report.
   */
  Report stop() {
    dispatch::stop(strategy_, context_);
    return settle();
  }

  /**
   * @brief Calls `on_stop` and settles what it asked for while `PollingLoop`'s `STOPPING` hold stands,
   *   so the strategy's parting orders are refused rather than sent beside the operator's closing
   *   orders.
   * @return The pass's report.
   */
  Report notify_stop() {
    dispatch::stop(strategy_, context_);
    return settle();
  }

  /**
   * @brief Settles instructions already queued on the context, without calling a handler.
   *
   * `PollingLoop` queues its closing orders and settles them with this.
   *
   * @return The pass's report.
   */
  Report settle_closing() { return settle(); }

  /**
   * @brief Feeds one bar: caches it, sets the last price and the clock to the bar's period end, calls
   *   `on_bar`, and settles what the strategy asked for.
   * @param event The bar.
   * @return The pass's report.
   * @throws std::invalid_argument (with a linear algebra backend) if the bar is not later than the
   *   cached series' newest bar.
   */
  Report feed(const strategy::BarEvent& event) {
#if defined(ATL_HAS_EIGEN)
    context_.record_bar(event);
#endif
    context_.set_last_price(event.instrument_, event.bar_.close_);
    context_.set_now(event.period_end_);
    confirm_reservations();
    dispatch::bar(strategy_, context_, event);
    return settle();
  }

  /**
   * @brief Feeds one bar as history: like `feed`, but whatever the strategy asks for is discarded
   *   (counted in `warmup_instructions_discarded`).
   *
   * Use it for every bar older than the newest one, so a restart or a backlog does not act on an old
   * signal at today's price. A source of your own should feed a read's new bars in period-end order
   * across instruments, and feed live (with `feed`) only each instrument's newest closed bar, and
   * only if its period ended strictly after every bar fed as history from the same read, across
   * instruments. A newest bar that ties or trails one (a suspended name, or one a bar behind) is
   * history too, or an order could be decided on a stale cross-section at today's price.
   *
   * @param event The bar.
   * @throws std::invalid_argument (with a linear algebra backend) if the bar is not later than the
   *   cached series' newest bar.
   */
  void feed_warmup(const strategy::BarEvent& event) {
#if defined(ATL_HAS_EIGEN)
    context_.record_bar(event);
#endif
    context_.set_last_price(event.instrument_, event.bar_.close_);
    context_.set_now(event.period_end_);
    dispatch::bar(strategy_, context_, event);
    warmup_instructions_discarded_ += context_.drain().size();
  }

  /**
   * @brief Returns how many instructions `feed_warmup` discarded.
   * @return The count over the runner's life.
   */
  std::size_t warmup_instructions_discarded() const noexcept { return warmup_instructions_discarded_; }

  /**
   * @brief Feeds one quote: sets the last price and the clock, calls `on_quote`, and settles.
   * @param event The quote.
   * @return The pass's report.
   */
  Report feed(const strategy::QuoteEvent& event) {
    context_.set_last_price(event.quote_.instrument_, event.quote_.last_price_);
    context_.set_now(event.quote_.received_at_);
    confirm_reservations();
    dispatch::quote(strategy_, context_, event);
    return settle();
  }

  /**
   * @brief Feeds one execution: calls `on_fill` and settles.
   * @param event The fill.
   * @return The pass's report.
   */
  Report feed(const strategy::Fill& event) {
    confirm_reservations();
    dispatch::fill(strategy_, context_, event);
    return settle();
  }

  /**
   * @brief Feeds one order update: calls `on_order_update` and settles.
   * @param event The order's new state.
   * @return The pass's report.
   */
  Report feed(const OrderSnapshot& event) {
    confirm_reservations();
    dispatch::order_update(strategy_, context_, event);
    return settle();
  }

  /**
   * @brief Feeds a market phase change: sets it on the context, calls `on_phase`, and settles.
   * @param phase The new phase.
   * @return The pass's report.
   */
  Report feed(strategy::MarketPhase phase) {
    context_.set_phase(phase);
    // A DAY order does not outlive its session, whatever its history row says afterwards: a broker may
    // leave an expired order's remainder on the row (KIS reads it from `rmn_qty`), so the row's filled
    // quantity is taken as final once the session has closed (`confirm_from_history`).
    if (phase == strategy::MarketPhase::CLOSED || phase == strategy::MarketPhase::POST_CLOSE) {
      for (auto& entry : unconfirmed_) {
        if (entry.row_.order_.time_in_force_ == TimeInForce::DAY) {
          entry.session_ended_ = true;
        }
      }
    }
    confirm_reservations();
    dispatch::phase(strategy_, context_, phase);
    return settle();
  }

private:
  // Drains the queue and turns it into a report.
  //
  // **Cancellations are collected before any order is placed**, across the whole queue, rather than
  // per instruction. That is the sequencing `Plan` documents as the caller's obligation, and here
  // is the caller: a replacement placed before its predecessor is withdrawn is briefly a double
  // position, while cancelling first leaves the instrument unrepresented for a moment instead.
  //
  // A guard refusal drops that instruction and nothing else. One instrument breaching a position
  // ceiling must not silence the rest of a cross-sectional rebalance.
  Report settle() {
    // Before anything is planned, because this pass has not yet added anything of its own: the
    // probe must not read this runner's intentions as the broker's report. The portfolio can still
    // carry a reservation an *earlier* pass persisted, which is why those rows are marked and why
    // the probe skips them rather than relying on when it is called.
    observe_sellable_convention();
    // After the probe, which must read the broker's report rather than this runner's rows, and before
    // anything is planned: whatever replaced `working_` since the last pass, each order this runner
    // sent is counted again at what nothing yet explains (`unconfirmed_orders`).
    confirm_reservations();

    Report report;
    auto instructions = context_.drain();
    kept_this_pass_.clear();
    drop_superseded_deferred(instructions);
    std::vector<strategy::PlannedOrder> pending;
    // The instruction each pending order came from, aligned with `pending`, so a phase refusal can
    // keep the target that produced it (`retain_for_phase`). Points into `instructions`, which lives
    // for the whole pass.
    std::vector<const Instruction<Tag>*> origin;

    // **One withdrawal per order per pass.** A strategy that clears what is resting and then states
    // a target asks twice for the same order: the explicit `CancelIntent` goes out, and then
    // `plan_for` reconciles the target against `context_.portfolio()` -- which does not carry the
    // cancellation just sent -- and plans a withdrawal for the same row. Two requests, and a broker
    // answering the second with "already cancelled" then reads as a rejected withdrawal in the
    // report.
    //
    // **Only the duplicate send is suppressed; the picture planning reads is untouched.** Removing
    // the row from that view is the tempting fix and it is unsafe: `SendOutcome::ACCEPTED` on a
    // withdrawal means the broker took the *request*, and `PENDING_CANCEL` counts as open in
    // `strategy::order_is_open`. Sizing a replacement against an emptied book would place a fresh
    // order of the full target while the original may still fill -- doubling a position to save a
    // request. Reconciliation keeps treating a possibly-working order as working, which is the
    // right answer until a listing says otherwise.
    //
    // Keyed on *attempted*, not on accepted: no outcome here means the order is gone, and a
    // withdrawal that was refused locally will be refused the same way a second time.
    std::vector<OrderRef<Tag>> attempted;
    const auto attempt_cancel = [this, &attempted, &report](const OrderRef<Tag>& ref) {
      // An unaddressed reference is exempt. Two of them compare equal, so deduping would collapse
      // two distinct unsendable withdrawals into one report entry and hide the second.
      if (!ref.primary_.empty() || !ref.secondary_.empty()) {
        for (const auto& seen : attempted) {
          if (seen == ref) {
            return;
          }
        }
        attempted.push_back(ref);
      }
      send_cancel(ref, report);
    };
    // Serialized: the instruments a plan in this pass places on, so a second plan for one of them is
    // withheld before its withdrawals go out (`serial_hold`).
    std::vector<InstrumentId> claimed;

    for (const auto& instruction : instructions) {
      // **A later target or order replaces a retained target**: the strategy has spoken again about this
      // instrument, and replaying what it said before would override it. A withheld target is retained
      // again below, as the latest. A `CancelIntent` names an order, not an instrument, and leaves it.
      forget_retained(instruction_instrument(instruction));
      if (const auto* cancel = std::get_if<strategy::CancelIntent<Tag>>(&instruction)) {
        attempt_cancel(cancel->ref_);
        continue;
      }

      const auto planned = plan_for(instruction);
      // **Checked before a single cancellation goes out, and on the plan rather than on its parts.**
      //
      // `plan_for` derives both halves from `context_.portfolio()`: which rows oppose the target, and
      // what remains to be placed. So a plan is only as good as the snapshot under it, and the
      // cancellations are sent below -- before the guard has seen the placements at all. Deciding the
      // halves separately is how a reprice becomes a bare withdrawal.
      //
      // Only a plan that would cancel something is stopped. One that only places is left to the guard
      // exactly as before, so a direct `OrderIntent` keeps reporting as a refusal rather than quietly
      // changing bucket.
      const bool would_cancel = !planned.cancel_.empty();
      if (const auto reason = plan_withhold_reason(would_cancel)) {
        // **A missing account stops the whole plan; a hold on placements stops only a plan that
        // would also withdraw something.**
        //
        // The difference is which of the two the *guard* can be relied on to catch. A held placement
        // is refused by `RiskGuard` a few lines below, with the hold named on its own report entry,
        // and that is the better result -- so a plan with nothing to cancel is left to it. What the
        // guard cannot catch is data: a snapshot nobody has read is not a hold, and the loop's hold
        // for it is not yet installed at every point an instruction can be settled. A source's
        // `read_order_list` runs before `PollingLoop` installs any hold and is handed a mutable
        // runner, so a target settled inside that call reaches here with the guard clear. Deciding
        // absence and staleness here, on the snapshot itself, is what makes the rule hold at every
        // send site rather than at most of them.
        // A missing-account reason is only ever returned when it should stop this plan, so it needs
        // no second test here. A hold on placements stops only a plan that would also withdraw
        // something: a placement-only plan is left to `RiskGuard` below, which refuses it with the
        // hold named on its own report entry, and that is the better result.
        const bool data_is_missing = *reason != WithheldPlanReason::PLACEMENT_HELD;
        if (data_is_missing || would_cancel) {
          // The whole plan, before any of its parts were counted separately. **Four reasons reach
        // here** and this counts them together: an account never read, one older than the stated
        // maximum, one read before the listing it would be reconciled with, and a hold refusing
        // placements for a plan that also wanted to withdraw something -- the last two on a perfectly
        // fresh portfolio. Counted at all because the loop's own gate
        // does not always describe it: with `allow_absent_portfolio` the gate can be open while this
        // fires.
        ++plans_withheld_;
        std::string detail = withhold_detail(*reason);
        if (data_is_missing) {
          if (retain_withheld(instruction)) {
            detail = "retained for the next holdings read";
          } else if (*reason == WithheldPlanReason::PORTFOLIO_SUPERSEDED &&
                     std::holds_alternative<strategy::OrderIntent>(instruction)) {
            detail = "dropped: an order intent states a size against the mis-ordered picture";
          }
        }
        report.withheld_plans_.push_back(WithheldPlan<Tag>{
            *reason,
            instruction_instrument(instruction),
            instruction_tag(instruction),
            planned.cancel_,
            planned.place_.size(),
            std::move(detail),
          });
          continue;
        }
      }
      // **Serialized: decided on the plan, before any of its withdrawals go.** A plan whose placement will
      // be withheld must not withdraw first: a reprice would become a bare withdrawal, and a protective
      // order could be withdrawn with its replacement left waiting. A plan that only withdraws is not
      // held; one that withdraws a stop and defers its target waits for the stop to be accounted for in
      // `release_deferred` instead.
      if (serialized() && !planned.place_.empty()) {
        std::optional<std::string> hold;
        for (const auto& order : planned.place_) {
          hold = serial_hold(order.instrument_, claimed);
          if (hold) {
            break;
          }
        }
        if (hold) {
          ++plans_withheld_;
          if (retain_withheld(instruction, /*for_phase=*/false, context_.portfolio().as_of_)) {
            *hold += "; retained for a later holdings read";
          }
          report.withheld_plans_.push_back(WithheldPlan<Tag>{
            WithheldPlanReason::AWAITING_CONFIRMATION,
            instruction_instrument(instruction),
            instruction_tag(instruction),
            planned.cancel_,
            planned.place_.size(),
            std::move(*hold),
          });
          continue;
        }
        for (const auto& order : planned.place_) {
          claimed.push_back(order.instrument_);
        }
      }
      for (const auto& ref : planned.cancel_) {
        attempt_cancel(ref);
      }
      defer_if_waiting_on_stop(instruction, planned);
      for (const auto& rejection : planned.rejected_) {
        // Reconciliation's own refusal: it could not build a valid instruction at all, which is a
        // different fact from a broker saying no. **Not a diagnosis**: the same bucket holds a
        // quantity that rounded away on the lot, which is a deliberate drop. The reason is on the
        // entry.
        ++orders_unbuildable_;
        last_unbuildable_ = rejection;
        report.rejected_.push_back(rejection);
      }
      for (const auto& order : planned.place_) {
        pending.push_back(order);
        origin.push_back(&instruction);
      }
    }

    strategy::GuardInputs inputs;
    inputs.now_ = context_.now();
    inputs.phase_ = context_.phase();

    // **Every order in this pass is checked against a view that already contains the orders this
    // pass has sent.** Without that, a limit is re-evaluated from the same starting point for every
    // order, and two orders that each fit can breach it together: with a hundred sellable and no
    // resting order, two sells of sixty both pass and twenty shares are sold that were not there.
    //
    // A copy per pass, mutated as orders are accepted, closes that within one `settle`.
    // `persist_reservation` closes the same hazard *across* passes: always for a runner that sends to
    // a broker listing working orders, and otherwise for the limits that read what is outstanding.
    // Nothing has to recognise its own order in a later listing: a working-order poll assigns
    // `working_` wholesale (a holdings poll keeps it), so the broker's own answer replaces the
    // reservation when it arrives. Between a placement and that poll, the context portfolio is
    // therefore the broker's last report **plus** what this runner has sent since -- which is what a
    // target and a limit have to size against, and why the rows are marked so that anything inferring
    // the broker's behaviour from the listing can tell them apart.
    auto view = context_.portfolio();
    // Resolved once for the pass. The currencies and marks do not move as orders are accepted --
    // only quantities do, and the guard reads those from the view -- and resolving per order would
    // repeat a map lookup per instrument for every instrument. Built only when the ceiling is on, so
    // a caller who has not asked for it pays nothing.
    if (guard_.limits().max_gross_exposure_) {
      inputs.exposure_basis_ = exposure_basis_for(view, pending);
    }
    for (std::size_t at = 0; at < pending.size(); ++at) {
      auto& order = pending[at];
      const auto& source_instruction = *origin[at];
      // Checked before the guards, because an unresolved instrument is a safety hold rather than a
      // limit breach: reporting a notional ceiling for an order that was never going to be sent
      // would point an operator at the wrong problem.
      // Checked before the ledger and before the guards, and checked *per order* rather than once
      // at the top. A journal that failed on the first order of a batch will fail on the second, so
      // a batch that carried on would produce one unexplained refusal per instrument. This is also
      // what makes the condition hold for a direct `feed()`, which never reaches `PollingLoop`'s
      // gate at all.
      if (guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
        ++instructions_unavailable_;
        report.unavailable_.push_back(
          WithheldOrder{
            WithheldReason::LOCAL_FAILURE,
            order,
            guard_.hold_detail(strategy::HaltReason::LOCAL_FAILURE),
          }
        );
        continue;
      }
      if (ledger_.requires_reconcile(order.instrument_)) {
        // The indeterminacy ledger, not a guard: this instrument has an outcome nobody knows, so
        // nothing further goes out for it until a listing settles the question. Counted apart from
        // every other origin because the operator's response is to reconcile rather than to relax a
        // limit or fix a price.
        ++orders_reconcile_withheld_;
        report.withheld_.push_back(
          WithheldOrder{
            WithheldReason::RECONCILE_REQUIRED,
            order,
            std::string(ledger_.detail(order.instrument_)),
          }
        );
        continue;
      }

      // **Serialized, again at the send**: an order placed earlier in this pass on the same instrument is
      // an order not yet accounted for. The baseline is taken here, once, and the same value goes to the
      // journal and to the order's record (`track_unconfirmed`).
      std::optional<Decimal64> baseline;
      if (serialized()) {
        if (auto hold = serial_hold(order.instrument_, {})) {
          ++orders_awaiting_confirmation_;
          if (retain_withheld(source_instruction, /*for_phase=*/false, context_.portfolio().as_of_)) {
            *hold += "; retained for a later holdings read";
          }
          report.withheld_.push_back(WithheldOrder{WithheldReason::AWAITING_CONFIRMATION, order, std::move(*hold)});
          continue;
        }
        baseline = context_.portfolio().net_quantity_of(order.instrument_);
      }

      fill_sellable_if_needed(view, order);

      inputs.reference_price_ = context_.last_price(order.instrument_);
      // The currency the order is priced in, which only the reference data knows. A buying-power
      // limit is per currency, and an unresolved instrument leaves it absent rather than defaulting
      // to one: guessing here would spend one currency's budget on another's order.
      const auto reference = context_.instrument(order.instrument_);
      inputs.order_currency_ =
        reference ? std::optional<Currency>(reference->currency_) : std::nullopt;
      // `evaluate` rather than `check`, for one fact `check` drops: whether the phase limit admitted
      // this order only because it closes a position. Nothing downstream can recover that -- the
      // order, the portfolio and the report all look the same either way -- and it is the one thing
      // an operator needs to see, because it is this library sending into a session the operator's
      // own limits said not to trade in.
      auto decision = guard_.evaluate(order, view, inputs);
      if (decision.rejection_) {
        // **Counted here and not at the send site, because there is no send.** A buy left above a
        // withheld ceiling is the larger notional, so every limit that multiplies quantity by price
        // sees a bigger number than the clamped order would have shown it. (A sell left below a
        // withheld floor is the smaller notional; these limits are not what stops that one.)
        if (order.price_would_have_been_clamped_) {
          ++unclamped_orders_refused_;
        }
        ++orders_guard_refused_;
        if (decision.rejection_->reason_ == strategy::GuardReason::PHASE_NOT_PERMITTED) {
          retain_for_phase(source_instruction);
        }
        report.refused_.push_back(std::move(*decision.rejection_));
        continue;
      }

      // **The route it is sent with, written into the order before anything records it.** A
      // strategy names no venue, and a binding may send one anyway (`SessionExecutor::default_route`).
      // Resolved here rather than only at the wire so the journal line, the row `reserve` keeps --
      // which `cancel_all` and every other instruction built from it restate -- and the report all
      // name the venue the broker was asked for, not the blank the strategy left.
      order.venue_ = requested_route(order.venue_);
      const auto outcome = place_order(order, baseline);
      // **Counted where the outcome is known, not where the exception was granted.** An admitted
      // order can still be refused by the sellable check, a buying-power limit or either ceiling and
      // never leave the process, so counting at the guard would report a proxy for the harm rather
      // than the harm. `REJECTED` counts: the request went out and the broker answered it, which is
      // exactly the nightly-rejection case this exception was chosen knowing about, and a witness
      // that stayed at zero through every one of them would be silent in the case it exists for.
      // `REFUSED_LOCALLY` and `UNAVAILABLE` do not: nothing was sent. This is the same division
      // `note_sent` already makes, for the same reason.
      if (decision.admitted_outside_phase_ &&
          (outcome == SendOutcome::ACCEPTED || outcome == SendOutcome::REJECTED ||
           outcome == SendOutcome::INDETERMINATE)) {
        ++sends_outside_permitted_phase_;
      }
      // **Split on whether a broker was asked, which is the division `send_outcome_reached_broker`
      // already makes.** `REJECTED` means the exchange saw this price and refused it.
      // `REFUSED_LOCALLY` means the binding did -- and that is a price-dependent refusal too: a
      // limit price no adapter will send is a limit price the withheld band might have moved into
      // range. The concrete case is a sell at zero that `Price::checked` admits
      // because zero sits on a stated grid. Counting only the first left that invisible.
      // `INDETERMINATE` is excluded because it is not a refusal at all -- the request may well be
      // resting -- and `UNAVAILABLE` because it is local machinery being unavailable, which no price
      // affects.
      if (order.price_would_have_been_clamped_) {
        if (outcome == SendOutcome::REJECTED) {
          ++unclamped_orders_rejected_;
        } else if (outcome == SendOutcome::REFUSED_LOCALLY) {
          ++unclamped_orders_refused_;
        }
      }
      // After every send, not only after a refusal. A terminal journal write that fails leaves the
      // broker's own outcome intact -- the send happened, and saying otherwise would be a lie -- but
      // it still means the journal is broken, and the runner must stop.
      note_executor_failure(outcome == SendOutcome::UNAVAILABLE);
      switch (outcome) {
        case SendOutcome::ACCEPTED:
          // Counted against the rate limit only once the instruction actually went, so an order a
          // guard refused or the ledger withheld does not spend budget.
          guard_.note_sent(inputs.now_);
          ++placements_at_broker_;
          // And it is working now, as far as anything later in this pass is concerned, and its
          // money is spent. An `INDETERMINATE` send is counted the same way below, with no reference.
          reserve(
            view, order, inputs.reference_price_, inputs.order_currency_, last_ref(),
            guard_.limits().enforce_buying_power_
          );
          persist_reservation(order, inputs.reference_price_, inputs.order_currency_, last_ref());
          track_unconfirmed(order, last_ref(), /*indeterminate=*/false, baseline);
          report.would_place_.push_back(std::move(order));
          break;
        case SendOutcome::REJECTED:
          // A refusal is an answer: the order does not exist, so nothing needs reconciling. It
          // still consumed a request, which is what the broker will have counted.
          guard_.note_sent(inputs.now_);
          ++orders_broker_refused_;
          // An exit the phase limit let out of session, refused by a broker not taking orders then:
          // the target waits for the session instead of for its next bar, which may be a day away.
          if (decision.admitted_outside_phase_) {
            retain_for_phase(source_instruction, /*broker_answered=*/true);
          } else if (last_error_is(broker_rate_limited)) {
            // **Turned away for the broker's rate limit, which is about the moment.** Kept as an
            // unreachable send is, and offered again after a newer holdings read: dropped, a daily
            // strategy's opening order or protective stop refused once at a busy open waited a day
            // (in a simulated run, one `EGW00201` at 09:00 moved a buy to the next session and left a position
            // without its stop all day). The broker processed nothing, so a retry cannot double it.
            retain_for_phase(source_instruction, /*broker_answered=*/true, context_.portfolio().as_of_);
          }
          note_broker_refusal(order);
          report.rejected_.push_back(
            strategy::Rejection{
              strategy::RejectReason::BROKER_REFUSED, order.instrument_, order.tag_
            }
          );
          break;
        case SendOutcome::INDETERMINATE:
          guard_.note_sent(inputs.now_);
          // Counted like an acceptance for the purpose below: the order may exist, and a listing
          // read before this moment cannot show it either way.
          ++placements_at_broker_;
          ledger_.mark(order.instrument_, "an order request has an unknown outcome");
          ++orders_indeterminate_;
          // **Counted as if accepted, until the sweep says what became of it.** The ledger
          // withholds the instrument meanwhile; but once a sweep finds the order in the broker's
          // listing it clears the ledger, and a market order that filled is then in no listing while
          // holdings may not show it yet, so a target sent it again (twice the target, in one
          // process). Reserved and tracked here with the holding at send as the baseline, and handed
          // its reference by `resolve_indeterminate_placement`. **No reference** is passed on purpose:
          // `last_ref()` still names the previous accepted order, and a row carrying it could be
          // withdrawn in that order's place.
          // A serialized runner records it whatever its executor reports: it owns the instrument until
          // accounted for.
          if constexpr (keeps_sent_orders_until_listed) {
            if (serialized() || runner_detail::reports_last_ref<Executor>::value) {
              reserve(
                view, order, inputs.reference_price_, inputs.order_currency_, std::nullopt,
                guard_.limits().enforce_buying_power_
              );
              persist_reservation(order, inputs.reference_price_, inputs.order_currency_, std::nullopt);
              track_unconfirmed(order, std::nullopt, /*indeterminate=*/true, baseline);
            }
          }
          report.indeterminate_.push_back(std::move(order));
          break;
        case SendOutcome::REFUSED_LOCALLY:
          // **No `note_sent`**: the binding refused it, so no request went out and the broker
          // counted nothing. Spending rate-limit budget here would refuse a later order that would
          // have been allowed, for a request nobody made. Reported as a rejection because that is
          // what it is from the order's point of view -- it does not exist, and nothing needs
          // reconciling -- and the runner keeps trading, because the next instruction may be
          // perfectly sendable. The reason is carried on the entry itself rather than left to
          // `last_error()`: a later accepted order in the same pass clears that, so a report can
          // outlive the only record of which refusal this was.
          ++orders_binding_refused_;
          if (decision.admitted_outside_phase_) {
            retain_for_phase(source_instruction);
          } else if (last_error_is(broker_unreachable)) {
            // **The broker could not be reached, so the decision is kept rather than lost.** Dropped,
            // a daily strategy's target or protective stop released at the open into a dropped
            // connection waited for its next bar, a day away (in a simulated run, a 30-minute outage at 09:00 moved a
            // buy to the next session and left a position without its stop all day). Kept as a phase
            // refusal is -- discarded by a stop, an operator's halt or a local failure, replaced by a
            // later statement, an intent checked against the book -- and offered again only after a
            // holdings read newer than this one, which is the evidence the broker answers again: a
            // retry every tick into a dead connection would wait out a connect timeout per instrument
            // per iteration, and hold up a stop behind it.
            retain_for_phase(source_instruction, /*broker_answered=*/false, context_.portfolio().as_of_);
          }
          report.rejected_.push_back(
            strategy::Rejection{
              strategy::RejectReason::BINDING_REFUSED, order.instrument_, order.tag_
            }
          );
          break;
        case SendOutcome::UNAVAILABLE:
          // No `note_sent`: nothing went out, so the broker counted nothing and no rate-limit
          // budget was spent. The ledger is not marked either -- an instruction that was never
          // attempted leaves no doubt about the position, and withholding the instrument would be
          // withholding it for the wrong reason.
          ++instructions_unavailable_;
          report.unavailable_.push_back(
            WithheldOrder{
              WithheldReason::LOCAL_FAILURE,
              order,
              guard_.halted(strategy::HaltReason::LOCAL_FAILURE) ?
                guard_.hold_detail(strategy::HaltReason::LOCAL_FAILURE) :
                std::string("local machinery refused to send"),
            }
          );
          break;
      }
    }

    // The pass's instructions die with it; so do the pointers kept to deduplicate them.
    kept_this_pass_.clear();
    report.log_ = context_.log().take();
    return report;
  }

  // Sends one cancellation, and marks the instrument when its outcome is unknown.
  //
  // A cancellation that fails in transit leaves the same question an order does -- the order may
  // now be withdrawn or may still be resting -- so it obliges the same reconciliation. A
  // cancellation is not gated by the ledger, because withdrawing an order is the one action that
  // cannot make an unresolved position worse.
  // Asks the broker what may be sold, but only when nothing else can answer.
  //
  // Four conditions, and every one of them is there to avoid a request:
  //
  // - the executor can ask at all -- a dry run and a replay cannot, and compile to nothing here;
  // - the caller enforces the limit, so nobody pays for a figure no check reads;
  // - the order is a sell, since the figure says nothing about buying;
  // - and the snapshot has not already answered, which at a `HOLDINGS` broker it always has.
  //
  // The answer is written to the context's portfolio as well as to this pass's view, so a later
  // pass in the same holdings cycle reuses it rather than asking again. The next holdings poll
  // replaces the positions wholesale and the figure goes with them, which is the right lifetime: it
  // is exactly as fresh as the position it describes.
  void fill_sellable_if_needed(
    strategy::Portfolio<Tag>& view,
    const strategy::PlannedOrder& order
  ) {
    if constexpr (runner_detail::fills_sellable<Executor>::value) {
      if (!guard_.limits().enforce_sellable_quantity_ || order.side_ != OrderSide::SELL) {
        return;
      }
      if (view.sellable_quantity_of(order.instrument_)) {
        return;
      }
      const auto answered = executor_.sellable_quantity(order.instrument_);
      if (!answered) {
        // Left absent, so the guard refuses for the reason it already has. Reporting a failure to
        // establish the figure as a sellable quantity of zero would be a different claim.
        return;
      }
      write_sellable(view, order.instrument_, *answered);
      auto persisted = context_.portfolio();
      write_sellable(persisted, order.instrument_, *answered);
      context_.set_portfolio(std::move(persisted));
    } else {
      static_cast<void>(view);
      static_cast<void>(order);
    }
  }

  // Sends one order, stating the baseline to the journal in serialized mode.
  SendOutcome place_order(const strategy::PlannedOrder& order, const std::optional<Decimal64>& baseline) {
    if constexpr (can_serialize) {
      if (serialized()) {
        return executor_.place(order, baseline);
      }
    }
    (void)baseline;
    return executor_.place(order);
  }

  // Returns the venue the executor sends an order asking for `asked` to; `asked` where it does not route.
  Venue requested_route(Venue asked) const {
    if constexpr (runner_detail::resolves_route<Executor>::value) {
      return executor_.requested_route(asked);
    } else {
      return asked;
    }
  }

  // Returns the reference the broker issued for the order just accepted, where there is one.
  //
  // Nothing where the executor has no broker to have been answered by, which is every dry run and
  // every replay.
  std::optional<OrderRef<Tag>> last_ref() const {
    if constexpr (runner_detail::reports_last_ref<Executor>::value) {
      return executor_.last_ref();
    } else {
      return std::nullopt;
    }
  }

  // An order the broker accepted, or one whose request failed in transit, that holdings have not yet
  // shown in full; see `unconfirmed_orders`.
  struct Unconfirmed {
    // Empty while the order's outcome is unknown; see `resolve_indeterminate_placement`.
    OrderRef<Tag> ref_;
    InstrumentId instrument_;
    OrderSide side_{OrderSide::BUY};
    // What holdings must still show: the order's quantity, or less once its history says so.
    Decimal64 expected_;
    // The instrument's net holding when it was sent.
    Decimal64 baseline_;
    // The reserved row as `reserve` wrote it; rebuilt with the residual quantity.
    WorkingOrder<Tag> row_;
    std::chrono::system_clock::time_point sent_at_{};
    // Whether any listing has carried it, and whether the latest one does.
    bool listed_{false};
    bool listed_now_{false};
    // Whether this runner's withdrawal of it was accepted.
    bool withdrawn_{false};
    // Whether a terminal history row has capped `expected_`.
    bool settled_{false};
    // Whether it is a DAY order and the session it was sent in has closed since.
    bool session_ended_{false};
    // Whether the current `confirm_from_history` looked for it.
    bool consulted_{false};
    // Whether the history, read past the timeout, had no terminal row for it.
    bool history_missed_{false};
    // The quantity nothing explains, as of the last rebuild, and since when some has been.
    Decimal64 residual_;
    std::optional<std::chrono::system_clock::time_point> unexplained_since_;
    // Serialized mode (`serialized`). The order's journal line, whose id names it to a sweep and to the
    // line that accounts for it.
    std::optional<JournalEntry> journal_;
    // Whether `ref_` came from the broker's own acceptance, so a cancellation can be built from it; a
    // restored order or one a sweep named carries the order id alone.
    bool broker_ref_{false};
    // Restored from the journal at startup: its history is read once even though no listing carried it.
    bool restored_{false};
    // Only an operator may account for it (`release_obligations`).
    bool operator_only_{false};
    // Whether its timeout has marked the ledger, which happens once.
    bool noticed_{false};
  };

  // The instant after which a holdings read may size the next order on an instrument (serialized
  // mode): the read that accounted for its last order (that read itself qualifies), or the instant an
  // operator released it or a previous process confirmed it (only a later read qualifies).
  struct Boundary {
    InstrumentId instrument_;
    std::chrono::system_clock::time_point at_{};
    bool strict_{false};
  };

  // Records `instrument`'s boundary, replacing an earlier one.
  void set_boundary(const InstrumentId& instrument, std::chrono::system_clock::time_point at, bool strict) {
    for (auto& boundary : boundaries_) {
      if (boundary.instrument_ == instrument) {
        if (boundary.at_ < at || (!(at < boundary.at_) && strict)) {
          boundary.at_ = at;
          boundary.strict_ = strict;
        }
        return;
      }
    }
    boundaries_.push_back(Boundary{instrument, at, strict});
  }

  // Why the holdings in hand cannot be the baseline of an order on `instrument`, or nothing.
  std::optional<std::string> baseline_missing(const InstrumentId& instrument) const {
    const auto& portfolio = context_.portfolio();
    if (portfolio.as_of_ == std::chrono::system_clock::time_point{}) {
      return std::string("holdings have not been read, so the order would have no baseline");
    }
    if (!portfolio.net_quantity_of(instrument)) {
      return std::string("the holding's rows do not add up, so the order would have no baseline");
    }
    for (const auto& boundary : boundaries_) {
      if (boundary.instrument_ == instrument &&
          (boundary.strict_ ? !(boundary.at_ < portfolio.as_of_) : portfolio.as_of_ < boundary.at_)) {
        return std::string(
          "the holdings were read before the last order on this instrument was accounted for; waiting for a "
          "newer read"
        );
      }
    }
    return std::nullopt;
  }

  // Why a serialized runner may not place on `instrument` now, or nothing; `claimed` lists the
  // instruments an earlier plan in this pass places on.
  std::optional<std::string> serial_hold(const InstrumentId& instrument, const std::vector<InstrumentId>& claimed) const {
    if (!can_serialize) {
      return std::string(
        "this runner journals to a broker but cannot serialize its orders (it needs a broker that lists working "
        "orders, and an executor with last_entry and record); see allow_overlapping_orders"
      );
    }
    if (!recovered_) {
      return std::string("nothing is placed before runner::recover_runner has run");
    }
    for (const auto& entry : unconfirmed_) {
      if (!(entry.instrument_ == instrument)) {
        continue;
      }
      if (entry.operator_only_) {
        return "an order on this instrument" + named(entry) + " is held for an operator (release_obligations)";
      }
      if (entry.ref_.primary_.empty()) {
        return std::string("an order on this instrument has an unknown outcome and no sweep has named it");
      }
      return "an order on this instrument" + named(entry) + " is not yet accounted for by holdings";
    }
    for (const auto& id : claimed) {
      if (id == instrument) {
        return std::string("an earlier instruction in this pass places on this instrument");
      }
    }
    return baseline_missing(instrument);
  }

  static std::string named(const Unconfirmed& entry) {
    return entry.ref_.primary_.empty() ? std::string() : " (" + entry.ref_.primary_ + ")";
  }

  // Whether a history row's status ends the original order, so its filled quantity is final.
  // `CANCEL_REJECTED` and `REPLACE_REJECTED` say a request about it failed, not that it ended.
  static bool ends_the_order(OrderStatus status) noexcept {
    return status == OrderStatus::FILLED || status == OrderStatus::CANCELED || status == OrderStatus::REJECTED;
  }

  // The line that accounts for, or holds, an order (serialized mode).
  static JournalEntry closing_line(
    const Unconfirmed& entry, JournalOutcome outcome, std::optional<Decimal64> holding, std::string detail
  ) {
    JournalEntry line = entry.journal_ ? *entry.journal_ : JournalEntry{};
    line.outcome_ = outcome;
    line.order_id_ = entry.ref_.primary_;
    line.baseline_ = std::move(holding);
    line.detail_ = std::move(detail);
    line.requires_operator_ = outcome == JournalOutcome::HELD;
    return line;
  }

  // Writes the line accounting for `entry`; on failure the runner stops sending and `entry` stays.
  bool discharge(const Unconfirmed& entry, std::optional<Decimal64> holding, std::string detail) {
    if constexpr (can_serialize) {
      auto line = closing_line(entry, JournalOutcome::CONFIRMED, std::move(holding), std::move(detail));
      if (executor_.record(line)) {
        return true;
      }
      note_executor_failure(false);
    } else {
      (void)entry;
      (void)holding;
      (void)detail;
    }
    return false;
  }

  // Holds `entry` for an operator, durably; on failure the runner stops sending and `entry` is unchanged.
  void hold_for_operator(Unconfirmed& entry, const std::string& detail) {
    if constexpr (can_serialize) {
      auto line = closing_line(entry, JournalOutcome::HELD, std::nullopt, detail);
      if (!executor_.record(line)) {
        note_executor_failure(false);
        return;
      }
      entry.operator_only_ = true;
      ledger_.mark(entry.instrument_, detail + "; an operator must account for it (release_obligations)");
    } else {
      (void)entry;
      (void)detail;
    }
  }

  // Removes `entry`'s reserved row from the context portfolio.
  void drop_reserved_row(const Unconfirmed& entry) {
    auto portfolio = context_.portfolio();
    portfolio.working_.erase(
      std::remove_if(
        portfolio.working_.begin(), portfolio.working_.end(),
        [&](const WorkingOrder<Tag>& working) { return is_reserved_row_of(working, entry); }
      ),
      portfolio.working_.end()
    );
    context_.set_portfolio(std::move(portfolio));
  }

  // Restores what a journal left unaccounted for; see `restore_obligations`.
  void restore_from(const JournalReplay& replay) {
    const auto same = [](const std::optional<Decimal64>& a, const std::optional<Decimal64>& b) {
      return a.has_value() == b.has_value() && (!a || (!decimal_less(*a, *b) && !decimal_less(*b, *a)));
    };
    // Every line repeats the instruction; a `CONFIRMED` or `HELD` line carries another holding.
    const auto same_instruction = [&](const JournalEntry& a, const JournalEntry& b) {
      return a.instrument_ == b.instrument_ && a.side_ == b.side_ && a.type_ == b.type_ &&
        a.time_in_force_ == b.time_in_force_ && a.session_ == b.session_ && a.venue_ == b.venue_ &&
        same(a.quantity_, b.quantity_) && same(a.price_, b.price_) && same(a.trigger_price_, b.trigger_price_) &&
        same(a.baseline_, b.baseline_);
    };
    struct Fold {
      JournalEntry line_;
      std::chrono::system_clock::time_point sent_at_{};
      std::string name_;
      bool done_{false};
      bool operator_only_{false};
      std::string why_;
    };
    std::vector<Fold> folds;
    for (const auto& line : replay.entries_) {
      if (line.action_ != JournalAction::PLACE || line.instrument_.symbol_.empty()) {
        continue;
      }
      const bool closing = line.outcome_ == JournalOutcome::CONFIRMED || line.outcome_ == JournalOutcome::HELD;
      auto fold = std::find_if(folds.begin(), folds.end(), [&](const Fold& seen) { return seen.line_.id_ == line.id_; });
      if (fold == folds.end()) {
        folds.push_back(Fold{line, line.recorded_at_, {}, false, false, {}});
        fold = std::prev(folds.end());
        if (closing) {
          fold->line_.baseline_.reset();
        }
      } else {
        if (!closing && !same_instruction(fold->line_, line)) {
          fold->operator_only_ = true;
          fold->why_ = "its journal lines disagree about the order";
        }
        if (line.recorded_at_ < fold->sent_at_) {
          fold->sent_at_ = line.recorded_at_;
        }
      }
      if (line.requires_operator_ && line.outcome_ != JournalOutcome::HELD) {
        fold->operator_only_ = true;
        fold->why_ = line.detail_.empty() ? std::string("a damaged journal line") : line.detail_;
      }
      switch (line.outcome_) {
        case JournalOutcome::ACCEPTED:
        case JournalOutcome::RESOLVED:
          if (!line.order_id_.empty()) {
            fold->name_ = line.order_id_;
          }
          break;
        case JournalOutcome::CONFIRMED:
          fold->done_ = true;
          set_boundary(line.instrument_, line.recorded_at_, /*strict=*/true);
          break;
        case JournalOutcome::REJECTED:
          fold->done_ = true;
          break;
        case JournalOutcome::HELD:
          fold->operator_only_ = true;
          fold->why_ = line.detail_;
          break;
        case JournalOutcome::PENDING:
        case JournalOutcome::INDETERMINATE:
          break;
      }
    }

    std::vector<Unconfirmed> restored;
    // Why each restored order is held for an operator, aligned with `restored`.
    std::vector<std::string> why;
    for (auto& fold : folds) {
      if (fold.done_) {
        continue;
      }
      Unconfirmed entry;
      entry.journal_ = fold.line_;
      entry.ref_.primary_ = fold.name_;
      entry.instrument_ = fold.line_.instrument_;
      entry.side_ = fold.line_.side_;
      entry.expected_ = fold.line_.quantity_;
      entry.baseline_ = fold.line_.baseline_.value_or(Decimal64::parse("0"));
      entry.sent_at_ = fold.sent_at_;
      entry.residual_ = fold.line_.quantity_;
      entry.restored_ = true;
      entry.operator_only_ = fold.operator_only_ || !fold.line_.baseline_;
      if (!fold.line_.baseline_ && fold.why_.empty()) {
        fold.why_ = "its journal line records no baseline (an overlapping or older runner wrote it)";
      }
      // The reserved row, rebuilt from the journal's record of the order.
      strategy::PlannedOrder order;
      order.instrument_ = fold.line_.instrument_;
      order.side_ = fold.line_.side_;
      order.type_ = fold.line_.type_;
      order.time_in_force_ = fold.line_.time_in_force_;
      order.session_ = fold.line_.session_;
      order.venue_ = fold.line_.venue_.value_or(Venue::UNSPECIFIED);
      order.quantity_ = fold.line_.quantity_;
      order.price_ = fold.line_.price_;
      order.trigger_price_ = fold.line_.trigger_price_;
      strategy::Portfolio<Tag> scratch;
      reserve(scratch, order, std::nullopt, std::nullopt, entry.ref_, /*spends=*/false);
      entry.row_ = scratch.working_.back();
      restored.push_back(std::move(entry));
      why.push_back(fold.why_);
    }
    for (std::size_t at = 0; at < restored.size(); ++at) {
      for (std::size_t other = 0; other < restored.size(); ++other) {
        if (other != at && restored[other].instrument_ == restored[at].instrument_) {
          restored[at].operator_only_ = true;
          if (why[at].empty()) {
            why[at] = "another order on its instrument is also unaccounted for";
          }
        }
      }
    }
    for (std::size_t at = 0; at < restored.size(); ++at) {
      if (restored[at].operator_only_) {
        ledger_.mark(
          restored[at].instrument_,
          "a previous process left an order" + named(restored[at]) + " only an operator may account for (" +
            why[at] + "); see release_obligations"
        );
      }
      unconfirmed_.push_back(std::move(restored[at]));
    }
    confirm_reservations();
  }
  // Remembers an order the broker just accepted, so a listing that lacks it cannot retire its
  // reservation; see `unconfirmed_orders`. Called right after `persist_reservation` wrote its row.
  //
  // Only for a runner that keeps sent orders until listed, an order with a broker reference, and a
  // portfolio that has been read: without a reference no listing or history row can be matched to it,
  // and without a read portfolio there is no baseline. Those keep the old behaviour, where the next
  // listing retires the row.
  //
  // `indeterminate`: an order whose request failed in transit, tracked with no reference until a sweep
  // names it (`resolve_indeterminate_placement`). While the indeterminacy ledger withholds its instrument
  // it shows no row (nothing can be placed there anyway, and a resting order's own listed row would be
  // counted twice) and does not time out; only holdings can cover it.
  //
  // Serialized, every such order is recorded, named or not, with the baseline the send stated and the
  // journal line it wrote: the gate guaranteed the holdings, and the order owns its instrument until
  // accounted for.
  void track_unconfirmed(
    const strategy::PlannedOrder& order, const std::optional<OrderRef<Tag>>& ref, bool indeterminate = false,
    const std::optional<Decimal64>& baseline = std::nullopt
  ) {
    if constexpr (keeps_sent_orders_until_listed) {
      const auto& portfolio = context_.portfolio();
      if constexpr (can_serialize) {
        if (serialized() && baseline && !portfolio.working_.empty()) {
          Unconfirmed entry;
          if (ref && !indeterminate) {
            entry.ref_ = *ref;
            entry.broker_ref_ = !ref->primary_.empty();
          }
          entry.instrument_ = order.instrument_;
          entry.side_ = order.side_;
          entry.expected_ = order.quantity_;
          entry.baseline_ = *baseline;
          entry.row_ = portfolio.working_.back();
          entry.sent_at_ = freshness_instant();
          entry.residual_ = order.quantity_;
          entry.journal_ = executor_.last_entry();
          if (entry.sent_at_ != std::chrono::system_clock::time_point{}) {
            entry.unexplained_since_ = entry.sent_at_;
          }
          unconfirmed_.push_back(std::move(entry));
          return;
        }
      }
      (void)baseline;
      if ((!indeterminate && (!ref || ref->primary_.empty())) ||
          portfolio.as_of_ == std::chrono::system_clock::time_point{} || portfolio.working_.empty()) {
        return;
      }
      const auto held = portfolio.net_quantity_of(order.instrument_);
      if (!held) {
        return;
      }
      Unconfirmed entry;
      if (ref) {
        entry.ref_ = *ref;
      }
      entry.instrument_ = order.instrument_;
      entry.side_ = order.side_;
      entry.expected_ = order.quantity_;
      entry.baseline_ = *held;
      entry.row_ = portfolio.working_.back();
      entry.sent_at_ = freshness_instant();
      entry.residual_ = order.quantity_;
      entry.unexplained_since_ = entry.sent_at_;
      unconfirmed_.push_back(std::move(entry));
    } else {
      (void)order;
      (void)ref;
      (void)indeterminate;
    }
  }

  // Whether a reserved row carries the reference the broker gave when it accepted the order.
  bool cancellable_reservation(const WorkingOrder<Tag>& working) const {
    if (working.ref_.primary_.empty()) {
      return false;
    }
    for (const auto& entry : unconfirmed_) {
      if (is_reserved_row_of(working, entry)) {
        return entry.broker_ref_;
      }
    }
    return true;
  }

  // Records that the broker accepted this runner's withdrawal of `ref`, if it is a sent order still counted.
  void note_withdrawal_accepted(const OrderRef<Tag>& ref) {
    for (auto& entry : unconfirmed_) {
      // The primary part alone, as everywhere a broker's own row is matched to the order.
      if (!entry.ref_.primary_.empty() && entry.ref_.primary_ == ref.primary_) {
        entry.withdrawn_ = true;
      }
    }
  }

  // Whether some of `entry`'s quantity has been unexplained for longer than the confirmation timeout.
  bool timed_out(const Unconfirmed& entry, std::chrono::system_clock::time_point now) const {
    return entry.unexplained_since_ && now - *entry.unexplained_since_ > confirmation_timeout_;
  }

  // How far a broker's order time may stand from this runner's own record of the order. Order numbers
  // repeat only across days, so an hour tells two orders apart and
  // still allows for a clock that is off.
  static constexpr std::chrono::hours order_time_tolerance_{1};

  // The history row that answers for an order: a row with its number and instrument (and side, if
  // given) whose order time, where stated, is within `[earliest, latest]`. Null if there is none, or if
  // two such rows disagree on what filled or on whether the order is still open: a number reused on
  // another day must not answer for this order, and when the rows cannot tell which is which, neither
  // does. Rows that agree are one answer (a dated read can return an undated row once per day).
  static const OrderSnapshot* history_row_for(
    const std::vector<OrderSnapshot>& evidence, const std::string& order_id, const InstrumentId& instrument,
    std::optional<OrderSide> side, std::optional<std::chrono::system_clock::time_point> earliest,
    std::optional<std::chrono::system_clock::time_point> latest
  ) {
    const OrderSnapshot* found = nullptr;
    for (const auto& row : evidence) {
      if (row.order_id_ != order_id || !(row.instrument_ == instrument) || (side && row.side_ != *side)) {
        continue;
      }
      if (row.ordered_at_ && ((earliest && *row.ordered_at_ < *earliest) || (latest && *latest < *row.ordered_at_))) {
        continue;
      }
      if (found == nullptr) {
        found = &row;
      } else if (decimal_less(found->execution_.filled_quantity_, row.execution_.filled_quantity_) ||
                 decimal_less(row.execution_.filled_quantity_, found->execution_.filled_quantity_) ||
                 strategy::order_is_open(found->status_) != strategy::order_is_open(row.status_)) {
        return nullptr;
      }
    }
    return found;
  }

  // Whether only the broker's history can settle `entry` now; see `unconfirmed_awaiting_evidence`.
  bool awaits_evidence(const Unconfirmed& entry, std::chrono::system_clock::time_point now) const {
    // A settled entry has had its answer: what it filled is known, and only holdings remain to show it.
    // One the history had no answer for past the timeout waits on holdings too, without a read per poll.
    // One with no reference yet never times out *while the ledger holds its instrument*: a sweep will
    // name the order or forget it (`resolve_indeterminate_placement`). Once something else lifted the
    // hold -- an operator clearing an `ambiguous` sweep by hand -- it is an ordinary entry again: its
    // history read finds no row, and a holdings read a timeout on withholds the instrument, counted,
    // rather than leaving an order that may never have existed reserved for the rest of the run.
    // Serialized, an order nobody named waits for a sweep or an operator, and one held for an operator
    // waits for the operator: no history row can be matched to the first or account for the second.
    if (entry.residual_.coefficient() == 0 || entry.settled_ || entry.history_missed_ || entry.operator_only_ ||
        (entry.ref_.primary_.empty() && (serialized() || ledger_.requires_reconcile(entry.instrument_)))) {
      return false;
    }
    if (timed_out(entry, now)) {
      return true;
    }
    // A restored order may have left every listing while no process was running.
    return !entry.listed_now_ && (entry.listed_ || entry.withdrawn_ || entry.restored_);
  }

  // Whether `working` is this runner's reserved row for `entry`.
  static bool is_reserved_row_of(const WorkingOrder<Tag>& working, const Unconfirmed& entry) {
    return working.reserved_ && working.ref_.primary_ == entry.ref_.primary_ &&
      working.order_.instrument_ == entry.instrument_ && working.order_.side_ == entry.side_;
  }

  // Rebuilds the reserved row of every sent order still counted, and forgets those holdings confirm.
  //
  // Run before every handler and every pass, so whatever replaced `working_` since -- a listing, or a
  // source of the caller's own -- the strategy and the guard see each order's unexplained quantity:
  //
  // - **covered**: holdings movement since the first unconfirmed order's baseline on the instrument, in
  //   the orders' direction, credited in the order they were sent, each up to what it expects. Orders
  //   on both sides of one instrument are credited only when holdings equal the baseline plus all of
  //   them, since a movement could be either side's;
  // - **listed**: what a listing still shows remaining for the same reference, instrument and side;
  // - the row is put back with `expected - covered - listed`, when that is positive, and **never
  //   spends buying power again** -- the pass that sent the order already did;
  // - the entry ends once `covered` reaches what it expects.
  void confirm_reservations() {
    if constexpr (keeps_sent_orders_until_listed) {
      if (unconfirmed_.empty()) {
        return;
      }
      const auto zero = Decimal64::parse("0");
      const auto now = freshness_instant();
      auto portfolio = context_.portfolio();
      // Every row this list owns comes out first, and the residual goes back in below.
      portfolio.working_.erase(
        std::remove_if(
          portfolio.working_.begin(), portfolio.working_.end(),
          [&](const WorkingOrder<Tag>& working) {
            for (const auto& entry : unconfirmed_) {
              if (is_reserved_row_of(working, entry)) {
                return true;
              }
            }
            return false;
          }
        ),
        portfolio.working_.end()
      );
      if (serialized()) {
        confirm_serialized(portfolio, now);
        context_.set_portfolio(std::move(portfolio));
        return;
      }

      std::vector<bool> retire(unconfirmed_.size(), false);
      // Settled by history, or past the timeout with no history answer, and still not in a holdings
      // read taken a full timeout later: withheld, with what holdings did show credited to it.
      std::vector<bool> expired(unconfirmed_.size(), false);
      std::vector<bool> done(unconfirmed_.size(), false);
      for (std::size_t first = 0; first < unconfirmed_.size(); ++first) {
        if (done[first]) {
          continue;
        }
        const auto& instrument = unconfirmed_[first].instrument_;
        const auto direction = unconfirmed_[first].side_;
        std::vector<std::size_t> group;
        bool mixed = false;
        for (std::size_t at = first; at < unconfirmed_.size(); ++at) {
          if (unconfirmed_[at].instrument_ == instrument) {
            group.push_back(at);
            done[at] = true;
            mixed = mixed || unconfirmed_[at].side_ != direction;
          }
        }
        // `a` signed in the group's direction: what a buy adds to a long holding, a sell takes away.
        const auto signed_in = [&](const Unconfirmed& entry, const Decimal64& quantity) {
          return entry.side_ == direction ? quantity : decimal_subtract(zero, quantity).value_or(zero);
        };

        // What a listing still shows of each order: that part cannot have reached holdings.
        std::vector<Decimal64> listed(group.size(), zero);
        std::vector<Decimal64> fillable(group.size(), zero);
        for (std::size_t at = 0; at < group.size(); ++at) {
          auto& entry = unconfirmed_[group[at]];
          entry.listed_now_ = false;
          for (const auto& working : portfolio.working_) {
            if (!working.reserved_ && !entry.ref_.primary_.empty() && working.ref_.primary_ == entry.ref_.primary_ &&
                working.order_.instrument_ == entry.instrument_ && working.order_.side_ == entry.side_ &&
                strategy::order_is_open(working.order_.status_)) {
              entry.listed_ = true;
              entry.listed_now_ = true;
              listed[at] = decimal_subtract(working.order_.quantity_, working.order_.execution_.filled_quantity_)
                             .value_or(zero);
              break;
            }
          }
          const auto left = decimal_subtract(entry.expected_, listed[at]).value_or(zero);
          fillable[at] = decimal_less(zero, left) ? left : zero;
        }

        // The holdings movement since the group's baseline -- the first entry's, which a retirement
        // below advances -- in the group's direction.
        std::optional<Decimal64> moved;
        if (const auto held = portfolio.net_quantity_of(instrument)) {
          moved = direction == OrderSide::BUY ? decimal_subtract(*held, unconfirmed_[first].baseline_) :
                                                decimal_subtract(unconfirmed_[first].baseline_, *held);
        }
        std::vector<Decimal64> covered(group.size(), zero);
        if (moved && !mixed) {
          // Credited in the order they were sent, each up to the part no listing still shows.
          auto left = *moved;
          for (std::size_t at = 0; at < group.size() && decimal_less(zero, left); ++at) {
            covered[at] = decimal_less(left, fillable[at]) ? left : fillable[at];
            left = decimal_subtract(left, covered[at]).value_or(zero);
          }
        } else if (moved) {
          // Both sides: a movement could be either side's, so it counts only when it is exactly what
          // all of them would make. A resting order a listing shows in full adds nothing, so a
          // protective stop does not hold up the buy beside it.
          std::optional<Decimal64> total = zero;
          for (std::size_t at = 0; at < group.size() && total; ++at) {
            total = decimal_add(*total, signed_in(unconfirmed_[group[at]], fillable[at]));
          }
          // A sum of zero is no evidence: two opposite orders that have not filled at all look like
          // two that both have. They wait for their history, and once it has said what each one
          // filled, holdings that did not move are evidence after all. That includes the resting order
          // above, which waits for no history (it once held them up until they timed out).
          bool all_settled = true;
          for (std::size_t at = 0; at < group.size(); ++at) {
            all_settled = all_settled && (unconfirmed_[group[at]].settled_ || fillable[at].coefficient() == 0);
          }
          if (total && (total->coefficient() != 0 || all_settled) && !decimal_less(*total, *moved) &&
              !decimal_less(*moved, *total)) {
            covered = fillable;
          }
        }

        // Retired entries advance the baseline the rest are measured from, so the movement that
        // confirmed one is never counted again for the next.
        std::optional<Decimal64> advanced = unconfirmed_[first].baseline_;
        for (std::size_t at = 0; at < group.size(); ++at) {
          auto& entry = unconfirmed_[group[at]];
          if (!decimal_less(covered[at], entry.expected_)) {
            retire[group[at]] = true;
            if (advanced) {
              advanced = direction == OrderSide::BUY ?
                decimal_add(*advanced, signed_in(entry, entry.expected_)) :
                decimal_subtract(*advanced, signed_in(entry, entry.expected_));
            }
            continue;
          }
          auto residual = decimal_subtract(entry.expected_, covered[at]).value_or(zero);
          residual = decimal_subtract(residual, listed[at]).value_or(zero);
          if (!decimal_less(zero, residual)) {
            entry.residual_ = zero;
            entry.unexplained_since_.reset();
            // Explained again, so a later gap starts over: a fresh timeout and a fresh history read.
            entry.history_missed_ = false;
            continue;
          }
          entry.residual_ = residual;
          if (!entry.unexplained_since_) {
            entry.unexplained_since_ = now;
          }
          // Judged by the holdings read, not the clock: the listing (and so the history) is polled
          // before holdings in each iteration, and on a loop polling slower than the timeout the
          // clock alone marked a fill one holdings read before it would have shown.
          if ((entry.settled_ || entry.history_missed_) && timed_out(entry, now) &&
              portfolio.as_of_ - *entry.unexplained_since_ > confirmation_timeout_) {
            // A holdings answer read a whole timeout after the quantity became unexplained (or after the
            // history settled it) does not show it, and the history either said what it filled or had
            // nothing to say: an outside trade, a disagreement or an order nobody here can account for.
            // Withheld for the operator.
            ++orders_unconfirmed_timed_out_;
            ledger_.mark(
              entry.instrument_,
              entry.settled_ ?
                "an order this runner sent (" + entry.ref_.primary_ +
                  ") filled by the order history, and holdings did not show it within the confirmation "
                  "timeout" :
                std::string("an order this runner sent") +
                  (entry.ref_.primary_.empty() ? " (its reply lost, never named by a sweep)" :
                                                 " (" + entry.ref_.primary_ + ")") +
                  " was not confirmed by holdings, the order list or the order history in time"
            );
            expired[group[at]] = true;
            // What holdings did show of it stays credited to it, not to the next order.
            if (advanced) {
              advanced = direction == OrderSide::BUY ? decimal_add(*advanced, signed_in(entry, covered[at])) :
                                                       decimal_subtract(*advanced, signed_in(entry, covered[at]));
            }
            continue;
          }
          // Not shown while the ledger holds an order not yet named: no placement on the instrument can
          // go out then, and beside the broker's own row for the same order a resting one would count
          // twice, so a target would withdraw the real one. The sweep that lifts the hold names it.
          if (entry.ref_.primary_.empty() && ledger_.requires_reconcile(entry.instrument_)) {
            continue;
          }
          auto row = entry.row_;
          row.order_.quantity_ = residual;
          row.amendable_quantity_ = residual;
          portfolio.working_.push_back(std::move(row));
        }
        for (const auto at : group) {
          if (!retire[at] && advanced) {
            unconfirmed_[at].baseline_ = *advanced;
          }
        }
      }

      for (std::size_t at = unconfirmed_.size(); at-- > 0;) {
        if (retire[at] || expired[at]) {
          unconfirmed_.erase(unconfirmed_.begin() + static_cast<std::ptrdiff_t>(at));
        }
      }
      context_.set_portfolio(std::move(portfolio));
    }
  }

  // The serialized half of `confirm_reservations`: one order per instrument, so no group arithmetic.
  //
  // Each order is accounted for, with a `CONFIRMED` line written first, when it is named, no listing
  // shows it open, and either its history capped what it filled at zero, or a holdings read shows
  // exactly its baseline plus what it filled. Holdings past that hold it for an operator. Anything else
  // rebuilds its reserved row from what nothing explains yet; a timeout marks the ledger once and
  // releases nothing.
  void confirm_serialized(strategy::Portfolio<Tag>& portfolio, std::chrono::system_clock::time_point now) {
    const auto zero = Decimal64::parse("0");
    const auto epoch = std::chrono::system_clock::time_point{};
    const bool read = portfolio.as_of_ != epoch;
    const auto equal = [](const Decimal64& a, const Decimal64& b) { return !decimal_less(a, b) && !decimal_less(b, a); };
    for (auto at = unconfirmed_.begin(); at != unconfirmed_.end();) {
      auto& entry = *at;
      // What a listing still shows of it: that part cannot have reached holdings.
      Decimal64 listed = zero;
      entry.listed_now_ = false;
      for (const auto& working : portfolio.working_) {
        if (!working.reserved_ && !entry.ref_.primary_.empty() && working.ref_.primary_ == entry.ref_.primary_ &&
            working.order_.instrument_ == entry.instrument_ && working.order_.side_ == entry.side_ &&
            strategy::order_is_open(working.order_.status_)) {
          entry.listed_ = true;
          entry.listed_now_ = true;
          listed = decimal_subtract(working.order_.quantity_, working.order_.execution_.filled_quantity_).value_or(zero);
          break;
        }
      }
      // The holding, and its movement in the order's direction since the baseline, from an actual read.
      std::optional<Decimal64> held;
      std::optional<Decimal64> moved;
      if (read) {
        held = portfolio.net_quantity_of(entry.instrument_);
        if (held) {
          moved = entry.side_ == OrderSide::BUY ? decimal_subtract(*held, entry.baseline_) :
                                                  decimal_subtract(entry.baseline_, *held);
        }
      }
      const bool is_named = !entry.ref_.primary_.empty();
      if (!entry.operator_only_ && is_named && !entry.listed_now_) {
        if (entry.expected_.coefficient() == 0) {
          if (discharge(entry, std::nullopt, "its own history row shows it ended with nothing filled")) {
            at = unconfirmed_.erase(at);
            continue;
          }
        } else if (moved && equal(*moved, entry.expected_)) {
          if (discharge(entry, held, "holdings show " + held->str() + ": its baseline " + entry.baseline_.str() +
                                       " and the " + entry.expected_.str() + " it filled")) {
            set_boundary(entry.instrument_, portfolio.as_of_, /*strict=*/false);
            at = unconfirmed_.erase(at);
            continue;
          }
        } else if (moved && decimal_less(entry.expected_, *moved)) {
          hold_for_operator(
            entry, "holdings moved " + moved->str() + " from the baseline " + entry.baseline_.str() + ", more than the " +
                     entry.expected_.str() + " an order this runner sent" + named(entry) + " can explain"
          );
        }
      }

      // What nothing explains yet: what it expects, less what holdings show of it, less what is listed.
      Decimal64 covered = zero;
      const auto fillable = decimal_subtract(entry.expected_, listed).value_or(zero);
      if (!entry.operator_only_ && moved && decimal_less(zero, *moved) && decimal_less(zero, fillable)) {
        covered = decimal_less(*moved, fillable) ? *moved : fillable;
      }
      auto residual = decimal_subtract(fillable, covered).value_or(entry.expected_);
      if (!decimal_less(zero, residual)) {
        entry.residual_ = zero;
        entry.unexplained_since_.reset();
        entry.history_missed_ = false;
        ++at;
        continue;
      }
      entry.residual_ = residual;
      // Not from the epoch: a restored order's clock starts with the first real instant.
      if (!entry.unexplained_since_ && now != epoch) {
        entry.unexplained_since_ = now;
      }
      if (!entry.noticed_ && !entry.operator_only_ && (entry.settled_ || entry.history_missed_ || !is_named) &&
          timed_out(entry, now) && portfolio.as_of_ - *entry.unexplained_since_ > confirmation_timeout_) {
        // A notice, not a release: the order still holds its instrument, and later evidence can still
        // account for it.
        entry.noticed_ = true;
        ++orders_unconfirmed_timed_out_;
        ledger_.mark(
          entry.instrument_,
          "an order this runner sent" + named(entry) +
            " was not accounted for by holdings within the confirmation timeout; it still holds the instrument"
        );
      }
      // Not shown while the ledger holds an order not yet named; see the overlapping branch.
      if (!is_named && ledger_.requires_reconcile(entry.instrument_)) {
        ++at;
        continue;
      }
      auto row = entry.row_;
      row.order_.quantity_ = residual;
      row.amendable_quantity_ = residual;
      portfolio.working_.push_back(std::move(row));
      ++at;
    }
  }

  // Carries this pass's reservation into the next one.
  //
  // **The in-pass view dies with the pass**, so without this a second pass before the next poll
  // starts from the broker's last word again and can re-spend what the first pass already
  // committed. The fix needs no matcher, because a working-order poll *replaces* what it
  // refreshes: `poll_working_orders` assigns `working_` wholesale, so an untracked reservation
  // written here is discarded the moment the broker's own listing arrives. (A holdings poll does not: it refreshes
  // positions and keeps `working_`.) Nothing has to recognise its own order in a listing -- which is
  // the half of reserve-and-match that carries a matcher's failure modes.
  //
  // **Always, for a runner that sends to a broker which lists working orders.** Target
  // reconciliation reads the same portfolio, and an order this runner just sent has to count as
  // working there, whatever the limits: a target restated on the next pass would otherwise be sent
  // again. It was once scoped to the limits below, and the cold start showed the cost --
  // `SessionSource` then fed a whole history read one pass per bar before the next listing, so a
  // target strategy under default limits sent one order per history bar (`tests/test-live.cpp`,
  // `[own-orders]`). It now feeds history through `feed_warmup`, which sends nothing; the reservation
  // still covers every other pass before a listing (a runner fed by hand, two names in one bar
  // poll, or bar polls that come faster than the listing's). A listing and holdings bound its life
  // (`unconfirmed_orders`), so the loop holds such a run while no listing will come -- the source
  // publishes none, no working-order cadence is set, or the last listing is older than the portfolio
  // age policy (`PollingLoop::gate_for`).
  //
  // **Scoped to the limits that read it everywhere else.** A dry run, `live::bind_observer` and a
  // custom executor that reaches no broker write a row with no broker reference: a later target
  // that opposes it would ask to withdraw an order identified by nothing, and a dry run has no
  // listing to replace the row at all. A broker that publishes no working-order inquiry never
  // replaces it either. For them the reservation persists only for the limits that size against
  // what is outstanding, as before. `max_position_quantity_` is one of those limits: its ceiling
  // counts same-side working orders, so an accepted order that stopped being visible at the end of a
  // pass would let the next bar place another of the same size.
  //
  // A custom executor that reaches a broker but reports no reference (`reports_last_ref`) is still
  // counted as keeping its orders: a duplicate order is the worse failure, and the listing the loop
  // insists on retires the unaddressable row.
  //
  // A listing replaces `working_` wholesale, and so used to retire the row whether or not it carried the
  // order -- a market order that filled at once is in no listing, and holdings lagging the fill did not
  // show it either. For a runner that keeps sent orders until listed, the row is now rebuilt
  // before every pass from what nothing explains yet (`track_unconfirmed`, `unconfirmed_orders`).
  void persist_reservation(
    const strategy::PlannedOrder& order,
    const std::optional<Decimal64>& reference_price,
    const std::optional<Currency>& currency,
    const std::optional<OrderRef<Tag>>& ref
  ) {
    const bool sells = guard_.limits().enforce_sellable_quantity_ && order.side_ == OrderSide::SELL;
    const bool buys = guard_.limits().enforce_buying_power_ && order.side_ == OrderSide::BUY;
    const bool capped = guard_.limits().max_position_quantity_.has_value();
    // The exposure ceiling reads what is outstanding for the same reason the position ceiling does,
    // so it needs the reservation for the same reason. Left out, a caller enabling only this limit
    // would get the within-pass fix and not the across-pass one: ten bought on each of two bars,
    // with no poll between, passes twice against a ceiling one of them should have reached. That is
    // precisely the omission `max_position_quantity_` once had.
    const bool exposed = guard_.limits().max_gross_exposure_.has_value();
    // A stop is read by target reconciliation whatever the limits, which refuses a target while one
    // rests (`RejectReason::STOP_ORDER_RESTING`). Not persisted, a stop placed on this pass would be
    // invisible to a target on the next one until a working-order poll listed it -- and "flatten"
    // would then go out beside the stop, both of them sells.
    const bool conditional = carries_trigger_price(order.type_);
    // A broker that will list the order back, reached by this runner: the listing and holdings
    // account for the row (`track_unconfirmed`), so the reservation is always written. See above for
    // why every other runner keeps the scoping.
    if (!keeps_sent_orders_until_listed && !sells && !buys && !capped && !exposed && !conditional) {
      return;
    }
    auto persisted = context_.portfolio();
    reserve(
      persisted, order, reference_price, currency, ref,
      guard_.limits().enforce_buying_power_
    );
    context_.set_portfolio(std::move(persisted));
  }

  // Resolves what every instrument the exposure ceiling must value is denominated in, and marked at.
  //
  // **This is the half `RiskGuard` cannot do**, which is why the guard takes it as an input rather
  // than deriving it. A position states its own currency and last price; a working order on an
  // instrument with no position states neither, and an opening buy is exactly that case. The runner
  // is where reference data and the quote cache live, so it is where the lookup belongs -- the same
  // reasoning that puts `order_currency_` here.
  //
  // **An instrument this cannot resolve is left out, and leaving it out is how the refusal
  // happens.** `projected_gross_exposure` returns nothing when the basis does not cover something
  // the portfolio holds or has working, and the guard turns that into `VALUE_NOT_KNOWN`. Inventing
  // a currency or a price here would be a silent pass, which this check exists to prevent.
  //
  // **Only fields somebody explicitly set are read**, which rules out the position's own
  // `market_value_.currency_` as a fallback however tempting it is. `Currency` default-constructs to
  // KRW and `HoldingPosition::last_price_` to zero, so an untouched position does not read as
  // unresolved -- it reads as Korean won marked at nothing, resolves, and contributes zero to the
  // sum. That is the silent omission this refusal exists to prevent, wearing the costume of an
  // answer. It is the same trap `backtest::ReplayLedger` was in.
  //
  // So: the currency comes from registered reference data, which is also where `order_currency_`
  // comes from, so the two cannot disagree about one instrument. The mark comes from the quote cache
  // or from a position's `last_price_` **when that price is not zero** -- a mark of zero is not a
  // valuation, and accepting one would value the position at nothing.
  //
  // The cost, and it is the intended direction: an account holding an instrument the caller never
  // registered refuses every order while the ceiling is on. Visible and diagnosable, rather than a
  // ceiling quietly measuring part of the portfolio.
  std::vector<strategy::InstrumentBasis> exposure_basis_for(
    const strategy::Portfolio<Tag>& view,
    const std::vector<strategy::PlannedOrder>& pending
  ) const {
    std::vector<InstrumentId> instruments;
    const auto note = [&instruments](const InstrumentId& id) {
      for (const auto& seen : instruments) {
        if (seen == id) {
          return;
        }
      }
      instruments.push_back(id);
    };
    for (const auto& position : view.positions_) {
      // Flat rows carry no exposure and are skipped for the reason `projected_gross_exposure` gives:
      // a replay keeps a lot after it returns to flat, and requiring a mark for one would let a
      // closed position refuse orders in an instrument it has nothing to do with.
      if (position.quantity_.coefficient() == 0) {
        continue;
      }
      note(position.instrument_);
    }
    for (const auto& working : view.working_) {
      if (strategy::order_is_open(working.order_.status_)) {
        note(working.order_.instrument_);
      }
    }
    for (const auto& order : pending) {
      note(order.instrument_);
    }

    std::vector<strategy::InstrumentBasis> basis;
    basis.reserve(instruments.size());
    for (const auto& id : instruments) {
      const HoldingPosition* position = nullptr;
      for (const auto& held : view.positions_) {
        if (held.instrument_ == id) {
          position = &held;
          break;
        }
      }

      const auto reference = context_.instrument(id);
      if (!reference) {
        continue;
      }

      // **Positive, not merely non-zero.** `Context::set_last_price` stores whatever a bar or quote
      // carried, with no validation, so a malformed price reaches here from market data rather than
      // only from a caller mistake. A negative mark would make an instrument's exposure negative and
      // let it offset real holdings until the ceiling passed anything at all; zero values the
      // position at nothing, which is the silent omission this resolution exists to refuse. Either
      // leaves the instrument unresolved, and the guard turns that into `VALUE_NOT_KNOWN`.
      const auto zero = Decimal64::parse("0");
      const auto quoted = context_.last_price(id);
      // **A quote that is present and not a price leaves the instrument unresolved**, rather than
      // falling through to the position's older mark. Falling back would be the quiet outcome: the
      // ceiling would answer from a stale figure while the current quote said something impossible,
      // and nothing would report that the data was wrong. Absent is a different thing from invalid,
      // and only absent gets the fallback.
      if (quoted && !decimal_less(zero, *quoted)) {
        continue;
      }
      auto price = quoted;
      if (!price && position != nullptr && decimal_less(zero, position->last_price_)) {
        price = position->last_price_;
      }
      if (!price) {
        continue;
      }
      basis.push_back(strategy::InstrumentBasis{id, reference->currency_, *price});
    }
    return basis;
  }

  // Writes a sellable quantity onto the position for `instrument`, when there is one.
  static void write_sellable(
    strategy::Portfolio<Tag>& portfolio,
    const InstrumentId& instrument,
    const Decimal64& quantity
  ) {
    for (auto& position : portfolio.positions_) {
      if (position.instrument_ == instrument) {
        position.sellable_quantity_ = quantity;
        // **The inquiry is its own reading, so it carries its own basis -- on this row only.** The
        // holdings poll that built this portfolio established nothing about a figure it never
        // reported, and the inquiry answered for *this* instrument and no other. Writing the basis to
        // the portfolio would let one instrument's declaration certify every other row's figure: a
        // populated but undeclared quantity on another instrument would become enforceable, and a
        // wrong-sized sell of it would reach the wire. An executor that declares nothing leaves this
        // unestablished, and the guard then refuses.
        position.sellable_basis_ = runner_detail::sellable_inquiry_basis_of<Executor>();
        return;
      }
    }
  }

  // Records an accepted order in `view` so the rest of the pass sizes against it.
  //
  // Written as a working order because that is what it now is, and because the alternative --
  // adjusting the position -- would be a lie: nothing has filled.
  //
  // **It carries the reference the broker just issued**, and that is not cosmetic. No guard reads a
  // reference, but reconciliation does: a row opposing the next target is put on the cancel list by
  // its `ref_`, and a synthetic row with an empty one would have the runner ask a broker to
  // withdraw an order identified by nothing. The shape and the amendable quantity are filled for
  // the same reason -- `cancel_all` builds the instruction from them, and a broker that requires a
  // quantity would be sent zero. An order just accepted and unfilled is amendable in full.
  //
  // Where the executor could not supply a reference -- a dry run, a replay, a custom executor
  // that does not report one, or an order whose reply was lost (passed no reference on purpose: the
  // executor's last one belongs to an earlier order) -- the row is still written, because a limit
  // that ignored the order would be the worse failure. Whether such a row can still be withdrawn is the executor's
  // question, not this one's: a replay's engine matches a cancellation on instrument and side and
  // needs no reference at all, while `live::SessionExecutor` refuses to send one to a broker.
  //
  // `spends` decides whether a buy also draws down the published buying-power reading, and it is
  // the caller's buying-power check rather than anything this function can see. The two were once
  // entangled: a caller who had enabled only the position ceiling watched
  // `portfolio().buying_power_in()` fall after a buy, which is a public figure a strategy is free
  // to size its next order from -- a behaviour change well past enforcing a ceiling.
  static void reserve(
    strategy::Portfolio<Tag>& view,
    const strategy::PlannedOrder& order,
    const std::optional<Decimal64>& reference_price,
    const std::optional<Currency>& currency,
    const std::optional<OrderRef<Tag>>& ref,
    bool spends
  ) {
    WorkingOrder<Tag> working;
    working.order_.instrument_ = order.instrument_;
    working.order_.side_ = order.side_;
    working.order_.status_ = OrderStatus::PENDING;
    working.order_.type_ = order.type_;
    working.order_.session_ = order.session_;
    // The venue this runner asked the order to be routed to, which is
    // `PlannedOrder::venue_` -- already resolved against the binding's default
    // route before the send -- and **not** the instrument's: the instrument's is
    // identity, and an order whose identity carried a route would stop matching
    // its own holding. A broker's own listing fills this field from what it
    // routed the order to; until one arrives, this row is what `cancel_all`
    // reads, and leaving it `UNSPECIFIED` would withdraw the order at the
    // broker's default venue rather than at the one it went to.
    working.order_.venue_ = order.venue_;
    working.order_.price_ = order.price_;
    // A stop's trigger, so the reserved row has the shape the broker's listing will report --
    // `cancel_all` restates it and `OrderUpdateWatch` compares it.
    working.order_.trigger_price_ = order.trigger_price_;
    working.order_.time_in_force_ = order.time_in_force_;
    working.order_.quantity_ = order.quantity_;
    working.order_.execution_.filled_quantity_ = Decimal64::parse("0");
    working.amendable_quantity_ = order.quantity_;
    if (ref) {
      working.ref_ = *ref;
    }
    // Marked as ours. A limit sizing against what is outstanding must count it; anything inferring
    // the broker's own behaviour from the listing must not, because this row is arithmetic rather
    // than something the broker said.
    working.reserved_ = true;
    view.working_.push_back(std::move(working));

    // A buy also spends money that the rest of the pass may not spend again. The estimate is the
    // same one the guard checked the order against -- its own price, or the last quote for a market
    // order -- and it is an *estimate*: a market order fills where it fills, and commission is not
    // in it. Under-reserving would be the dangerous direction, and neither of those makes the
    // reservation smaller than the order's stated cost.
    if (!spends || order.side_ != OrderSide::BUY || !currency) {
      return;
    }
    const auto price = order.price_ ? order.price_ : reference_price;
    if (!price) {
      return;
    }
    const auto spent = decimal_multiply(order.quantity_, *price);
    if (!spent) {
      return;
    }
    for (auto& reading : view.buying_power_) {
      if (!(reading.cash_.currency_ == *currency)) {
        continue;
      }
      if (const auto left = decimal_subtract(reading.cash_.amount_, *spent)) {
        reading.cash_.amount_ = *left;
      }
      return;
    }
  }

  // Returns why a plan derived from the current portfolio must not be acted on, or nothing.
  //
  // **The policy is evaluated here, against the snapshot in hand, rather than being pushed in as a
  // verdict somebody else computed earlier.** A `bool` set by the loop would be a judgement about
  // whatever portfolio was current when the loop last looked, and the loop looks at points a source
  // callback can get between: `PollingLoop::gate_for` calls `read_order_list` -- which is handed a
  // mutable runner and can feed -- before it ever reaches its own age check. A plan settled inside
  // that call would be checked against a stale verdict. Asking the question at settlement time makes
  // the answer structurally current, and there is no invalidation protocol to get wrong.
  // Drops the deferred targets `instructions` supersede.
  //
  // Every pass, so a target or order the strategy states for an instrument -- in answer to an order
  // update, a bar, anything -- replaces the fallback before `retry_deferred` could release it. See
  // `deferred_targets` for the whole contract.
  void drop_superseded_deferred(const std::vector<Instruction<Tag>>& instructions) {
    if (deferred_targets_.empty()) {
      return;
    }
    const auto drop = [this](const InstrumentId& instrument) {
      for (auto at = deferred_targets_.begin(); at != deferred_targets_.end(); ++at) {
        if (at->target_.instrument_ == instrument) {
          deferred_targets_.erase(at);
          return;
        }
      }
    };
    for (const auto& instruction : instructions) {
      if (const auto* target = std::get_if<strategy::TargetPosition>(&instruction)) {
        drop(target->instrument_);
      } else if (const auto* intent = std::get_if<strategy::OrderIntent>(&instruction)) {
        drop(intent->instrument());
      }
    }
  }

  // The release loop behind `retry_deferred` and `retry_deferred_on_one_snapshot`. `evidence` is the
  // broker's history, or null for a caller whose own ledger decided every stop.
  Report release_deferred(const std::vector<OrderSnapshot>* evidence) {
    bool released = false;
    for (auto at = deferred_targets_.begin(); at != deferred_targets_.end();) {
      // Serialized, a stop this runner placed is not done until it is accounted for (`serialized`).
      if (stop_listed(at->target_.instrument_) ||
          obligation_state(at->target_.instrument_) != ObligationState::NONE) {
        ++at;
        continue;
      }
      if (evidence != nullptr) {
        bool decided = true;
        bool filled = false;
        for (const auto& stop : at->stops_) {
          // Matched to the order time its listing showed, where it showed one. Without it (KIS lists no
          // time), a row of another day under the same number cannot be told apart unless two rows
          // disagree.
          std::optional<std::chrono::system_clock::time_point> earliest;
          std::optional<std::chrono::system_clock::time_point> latest;
          if (stop.ordered_at_) {
            earliest = *stop.ordered_at_ - order_time_tolerance_;
            latest = *stop.ordered_at_ + order_time_tolerance_;
          }
          const auto* row = history_row_for(
            *evidence, stop.ref_.primary_, at->target_.instrument_, std::nullopt, earliest, latest
          );
          if (row == nullptr) {
            decided = false;
            continue;
          }
          if (row->execution_.filled_quantity_.coefficient() != 0) {
            filled = true;
          } else if (strategy::order_is_open(row->status_)) {
            decided = false;
          }
        }
        if (filled) {
          ++deferred_dropped_after_fill_;
          at = deferred_targets_.erase(at);
          continue;
        }
        if (!decided) {
          ++at;
          continue;
        }
      }
      context_.submit(at->target_);
      at = deferred_targets_.erase(at);
      released = true;
    }
    // **Nothing released, nothing settled.** A pass has effects of its own -- it observes the
    // sellable convention from the portfolio -- and this runs on every working-order poll.
    if (!released) {
      return Report{};
    }
    // Exempt from `PORTFOLIO_SUPERSEDED` for this pass only; see `plan_withhold_reason`. Scoped, so a
    // settle that throws cannot leave the exemption on for every plan after it.
    struct Exempt {
      bool& flag_;
      explicit Exempt(bool& flag) : flag_(flag) { flag_ = true; }
      ~Exempt() { flag_ = false; }
      Exempt(const Exempt&) = delete;
      Exempt& operator=(const Exempt&) = delete;
    } exempt(releasing_deferred_);
    return settle();
  }

  // Drops the retained target on `instrument`, if there is one.
  void forget_retained(const InstrumentId& instrument) {
    if (instrument.symbol_.empty()) {
      return;
    }
    retained_targets_.erase(
      std::remove_if(
        retained_targets_.begin(), retained_targets_.end(),
        [&](const RetainedTarget& kept) { return kept.target_.instrument_ == instrument; }
      ),
      retained_targets_.end()
    );
    // Every kept intent for the instrument, not the first: a later statement supersedes all of them.
    kept_intents_.erase(
      std::remove_if(
        kept_intents_.begin(), kept_intents_.end(),
        [&](const KeptIntent& kept) { return kept.intent_.instrument() == instrument; }
      ),
      kept_intents_.end()
    );
  }

  // Keeps a target withheld for want of a trustworthy account, to plan again once there is one.
  //
  // Withheld because the account was never read, is too old, or predates the listing: each is a fact
  // about the data, and the next holdings read can change it. One per instrument, the latest kept, as
  // for `deferred_targets_`. Returns false, retaining nothing:
  //
  // - for anything but a `TargetPosition`. An `OrderIntent` states a quantity against the picture that
  //   was wrong, and replaying it later would act on a decision nobody re-made. (An intent the market
  //   phase stopped is a different case -- the picture was right, the hour was not -- and is kept by
  //   `keep_intent_for_phase`, with its holding checked at release.)
  // - while a stop, an operator's halt or a local failure holds. What a strategy says then -- `on_stop`
  //   included -- must not be replayed once the hold lifts.
  bool retain_withheld(
    const Instruction<Tag>& instruction, bool for_phase = false,
    std::optional<std::chrono::system_clock::time_point> after = std::nullopt
  ) {
    const auto* target = std::get_if<strategy::TargetPosition>(&instruction);
    if (target == nullptr) {
      return false;
    }
    if (guard_.halted(strategy::HaltReason::STOPPING) || guard_.halted(strategy::HaltReason::OPERATOR) ||
        guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      return false;
    }
    // The latest statement wins, and so does its reason: a target retained for the account and then
    // restated and refused for the phase waits for the phase.
    for (auto& kept : retained_targets_) {
      if (kept.target_.instrument_ == target->instrument_) {
        kept = RetainedTarget{*target, for_phase, after};
        return true;
      }
    }
    retained_targets_.push_back(RetainedTarget{*target, for_phase, after});
    return true;
  }

  // Keeps a target whose order the market phase stopped, for the first permitted phase.
  //
  // Two routes reach it, both only for a runner that sends to a broker: the phase limit refused an
  // order that opens or grows a position, or it admitted one that reduces a position outside the
  // phase and the broker (or the binding) refused it -- which is what a broker not taking orders at
  // midnight does. See `retry_retained_targets`.
  void retain_for_phase(
    const Instruction<Tag>& instruction, bool broker_answered = false,
    std::optional<std::chrono::system_clock::time_point> after = std::nullopt
  ) {
    if constexpr (runner_detail::sends_to_a_broker<Executor>::value) {
      if (retain_withheld(instruction, /*for_phase=*/true, after)) {
        return;
      }
      keep_intent_for_phase(instruction, broker_answered, after);
    } else {
      (void)instruction;
      (void)broker_answered;
      (void)after;
    }
  }

  // Whether the error behind the placement just refused satisfies `predicate` (`broker_unreachable`,
  // `broker_rate_limited`).
  //
  // Read immediately after the send, which is the only moment `last_error()` is this order's. An
  // executor that states no error has said nothing about why, so its refusals stay final.
  template <typename Predicate>
  bool last_error_is(Predicate predicate) const {
    if constexpr (runner_detail::states_last_error<Executor>::value) {
      const auto& error = executor_.last_error();
      return error && predicate(*error);
    } else {
      (void)predicate;
      return false;
    }
  }

  // Keeps an order intent whose order the market phase stopped, for the first permitted phase.
  //
  // The intent half of `retain_for_phase`: a daily strategy's protective stop, decided on the bar fed
  // at midnight and refused by a broker not taking orders then, would otherwise be restated at the
  // next midnight and refused again, and the position would never be protected live. Kept once per
  // instruction (one instruction can produce two refused orders), in the order stated, with the
  // instrument's holding at this moment, which `retry_retained_targets` checks before releasing it;
  // any intent carrying a trigger, a stop buy as much as a stop sell, is also dropped there when a stop
  // rests on the instrument. **Not** while a stop, an operator's halt or a local failure holds; **not** an intent carrying an
  // idempotency key that a broker answered, because a second order would reuse the key.
  void keep_intent_for_phase(
    const Instruction<Tag>& instruction, bool broker_answered,
    std::optional<std::chrono::system_clock::time_point> after = std::nullopt
  ) {
    const auto* intent = std::get_if<strategy::OrderIntent>(&instruction);
    if (intent == nullptr) {
      return;
    }
    if (guard_.halted(strategy::HaltReason::STOPPING) || guard_.halted(strategy::HaltReason::OPERATOR) ||
        guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      return;
    }
    if (broker_answered && intent->client_order_id()) {
      return;
    }
    for (const auto* seen : kept_this_pass_) {
      if (seen == &instruction) {
        return;
      }
    }
    const auto held = context_.portfolio().net_quantity_of(intent->instrument());
    if (!held) {
      return;
    }
    kept_this_pass_.push_back(&instruction);
    kept_intents_.push_back(KeptIntent{*intent, *held, after});
  }

  // Returns whether the portfolio lists an open stop order on `instrument`.
  bool stop_listed(const InstrumentId& instrument) const {
    for (const auto& working : context_.portfolio().working_) {
      if (working.order_.instrument_ == instrument &&
          strategy::order_is_open(working.order_.status_) &&
          strategy::is_stop_order(working.order_)) {
        return true;
      }
    }
    return false;
  }

  // Keeps a target whose plan is waiting on a stop's withdrawal; see `deferred_targets`.
  void defer_if_waiting_on_stop(
    const Instruction<Tag>& instruction,
    const strategy::Plan<Tag>& planned
  ) {
    const auto* target = std::get_if<strategy::TargetPosition>(&instruction);
    if (target == nullptr) {
      return;
    }
    for (const auto& rejection : planned.rejected_) {
      if (rejection.reason_ != strategy::RejectReason::STOP_ORDER_RESTING) {
        continue;
      }
      // The stops it waits on, as the listing shows them now -- `PENDING_CANCEL` included, since that
      // is exactly a stop whose outcome is not yet known.
      DeferredTarget deferred;
      deferred.target_ = *target;
      deferred.deferred_at_ = context_.now();
      for (const auto& working : context_.portfolio().working_) {
        if (working.order_.instrument_ == target->instrument_ &&
            strategy::order_is_open(working.order_.status_) &&
            strategy::is_stop_order(working.order_)) {
          deferred.stops_.push_back({working.ref_, working.order_.ordered_at_});
        }
      }
      for (auto& kept : deferred_targets_) {
        if (kept.target_.instrument_ == target->instrument_) {
          kept = std::move(deferred);
          ++targets_deferred_;
          return;
        }
      }
      deferred_targets_.push_back(std::move(deferred));
      ++targets_deferred_;
      return;
    }
  }

  std::optional<WithheldPlanReason> plan_withhold_reason(bool would_cancel) const {
    const auto& portfolio = context_.portfolio();
    // **A picture the library can prove is mis-ordered, so no opt-out reaches it.** The opt-outs below
    // accept an account nobody read or one read too long ago; this one was read, recently, and before
    // the listing it would be reconciled with -- which is the order in which a fill between the two
    // reads makes a position vanish. Exempt only while a deferred target is being released: that
    // release fires on the stop's own history showing it ended unfilled, so the listing change behind
    // it is by construction not a fill.
    if (!releasing_deferred_ && portfolio.listing_newer_than_holdings()) {
      return WithheldPlanReason::PORTFOLIO_SUPERSEDED;
    }
    // A default-constructed `Portfolio` carries the epoch, and no broker reports a snapshot taken in
    // 1970. The same sentinel the loop uses; both writers of `as_of_` in this library stamp it.
    const bool never_read = portfolio.as_of_ == std::chrono::system_clock::time_point{};
    const bool too_old = !never_read && maximum_portfolio_age_ &&
      freshness_instant() - portfolio.as_of_ > *maximum_portfolio_age_;

    if (never_read || too_old) {
      // **A plan that would withdraw something is stopped whatever was opted out of.** The opt-outs
      // say the operator accepts *sizing* against an account this library cannot vouch for. They do
      // not say it should withdraw orders it cannot account for, and the two are not the same
      // concession: an account that merely looks flat makes a resting protective order look opposed.
      if (would_cancel) {
        return never_read ? WithheldPlanReason::PORTFOLIO_ABSENT : WithheldPlanReason::PORTFOLIO_STALE;
      }
      // A placement is the half the opt-outs are about, and it is also the half a driving loop's
      // hold would otherwise catch -- so it is stopped here only for a run that asked for the
      // account to be known.
      if (require_known_portfolio_ && !(never_read && allow_absent_portfolio_)) {
        return never_read ? WithheldPlanReason::PORTFOLIO_ABSENT : WithheldPlanReason::PORTFOLIO_STALE;
      }
      // **Falls through rather than returning.** An opted-out placement still has to face the hold
      // check below: returning early here was a defect, because it let a plan out under an operator's
      // halt on the strength of an unrelated opt-out.
    }
    // Any placement hold, not an enumerated few: the harm is that the replacement will be refused
    // while the withdrawal has already gone, and every reason that refuses a placement does that.
    if (guard_.blocking_reason()) {
      return WithheldPlanReason::PLACEMENT_HELD;
    }
    return std::nullopt;
  }

  // Returns the instant a snapshot's age is measured against.
  //
  // The later of the decision instant and the last clock reading a loop supplied, so neither can be
  // used to make an old snapshot look fresh: a late bar cannot rewind the clock, and a loop that has
  // not spoken yet cannot erase the decision instant.
  std::chrono::system_clock::time_point freshness_instant() const {
    return observed_at_ > context_.now() ? observed_at_ : context_.now();
  }

  // Returns the detail recorded with a withheld plan, naming the hold where there is one.
  std::string withhold_detail(WithheldPlanReason reason) const {
    if (reason != WithheldPlanReason::PLACEMENT_HELD) {
      return {};
    }
    const auto holding = guard_.blocking_reason();
    return holding ? std::string(strategy::halt_reason_name(*holding)) : std::string{};
  }

  // Returns the instrument an instruction is about, for reporting a plan that was not acted on.
  static InstrumentId instruction_instrument(const Instruction<Tag>& instruction) {
    if (const auto* target = std::get_if<strategy::TargetPosition>(&instruction)) {
      return target->instrument_;
    }
    if (const auto* order = std::get_if<strategy::OrderIntent>(&instruction)) {
      return order->instrument();
    }
    return InstrumentId{};
  }

  // Returns the tag an instruction carries, so a withheld plan can be attributed to it.
  static std::string instruction_tag(const Instruction<Tag>& instruction) {
    if (const auto* target = std::get_if<strategy::TargetPosition>(&instruction)) {
      return target->tag_;
    }
    if (const auto* order = std::get_if<strategy::OrderIntent>(&instruction)) {
      return order->tag();
    }
    return {};
  }

  void send_cancel(const OrderRef<Tag>& ref, Report& report) {
    // Gated like a placement, and in the same order. A cancellation is the one instruction that
    // cannot make an unresolved *position* worse, which is why the ledger does not gate it -- but it
    // still goes through the journal, and a journal that cannot record it must not let it be sent
    // unrecorded.
    if (guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      ++instructions_unavailable_;
      report.unavailable_cancel_.push_back(ref);
      return;
    }
    // And a halt stops this runner *sending*, which a cancellation is -- but only a halt that is
    // about this instruction. The argument for refusing here is `StopIntent` reaching this runner
    // from a signal and from a file: exempting cancellations wholesale would let
    // `echo withdraw > /var/run/atl/stop` send an instruction that an operator's own halt forbade,
    // and a kill switch a control file can partly bypass is not one. An operator who wants out
    // resumes first, which is one deliberate act and is visible in the report either way.
    //
    // **That argument covers the holds somebody set, and not the holds that merely mean this runner
    // is uninformed.** Refusing to withdraw a resting order because the *holdings* poll failed is
    // worse than the exposure it guards: the order keeps resting and can fill, and the reference it
    // would be withdrawn by came from a working-order read that succeeded. So the question asked is
    // `InstructionClass::WITHDRAWAL`, and `strategy::halt_blocks` holds the division and its
    // reasoning. The row is still resolved below, and a reference the snapshot cannot place is
    // reported rather than sent blind.
    if (const auto holding =
          guard_.blocking_reason_for(strategy::InstructionClass::WITHDRAWAL)) {
      ++cancels_refused_;
      report.refused_cancel_.push_back(RefusedCancel<Tag>{ref, *holding});
      return;
    }
    // Resolved against the portfolio first, because building a cancellation needs the order's own
    // quantity on a broker that requires one. A reference not in the snapshot means the snapshot is
    // stale, and sending a cancellation for an order the runner cannot describe is exactly the kind
    // of blind instruction that should be visible instead.
    // The broker's own row first: a reserved row for the same order is this runner's arithmetic.
    const WorkingOrder<Tag>* working = nullptr;
    for (const auto& row : context_.portfolio().working_) {
      if (row.ref_ == ref && (working == nullptr || (working->reserved_ && !row.reserved_))) {
        working = &row;
      }
    }
    if (working == nullptr) {
      ++cancels_unresolved_;
      report.unresolved_cancel_.push_back(ref);
      return;
    }
    // **Serialized: a reserved row whose reference the broker did not give is not sent.** A restored
    // order, or one a sweep named, carries the order id alone, which some brokers cannot cancel by; and a
    // row with no id addresses nothing. Refused as the binding would, before anything is sent.
    if (working->reserved_ && serialized() && !cancellable_reservation(*working)) {
      ++cancels_rejected_;
      report.rejected_cancel_.push_back(RejectedCancel<Tag>{ref, /*by_broker=*/false});
      return;
    }
    const auto outcome = executor_.cancel(*working);
    note_executor_failure(outcome == SendOutcome::UNAVAILABLE);
    // Switched over rather than tested one outcome at a time, deliberately. Three different outcomes
    // once fell through to `would_cancel_` -- the bucket that says the
    // broker withdrew the order -- because each fix handled the case it was told about and left the
    // rest. A switch with no default makes the next added enumerator a compile error under
    // `-Werror`, which is the only version of this that stays fixed.
    switch (outcome) {
      case SendOutcome::ACCEPTED:
        // The broker took the request. Whether the order stops working is a later fact, which only
        // its own listing reports: cancellation is asynchronous, and a broker that answers here can
        // still reject the withdrawal afterwards.
        //
        // **Counted against the rate limit, like a placement.** A withdrawal is a request the
        // broker counted, and a window that does not see it is not the window the operator
        // configured -- a `WITHDRAW` over thirty resting orders is thirty requests the guard was
        // blind to, and the next placement is then checked against a window it believes is empty.
        // It is still never *refused* by that limit: `send_cancel` calls no `check`, because a rate
        // limit must never be the reason an order cannot be withdrawn.
        guard_.note_sent(context_.now());
        // A sent order still counted may have filled before the withdrawal; its history decides.
        note_withdrawal_accepted(ref);
        report.would_cancel_.push_back(ref);
        return;
      case SendOutcome::REJECTED:
        // The broker refused the request, which is an answer: nothing needs reconciling. It is not
        // a withdrawal that happened, and it is not proof the order is working either -- a fill is
        // one of the ordinary reasons a withdrawal is refused. It still spent a request.
        guard_.note_sent(context_.now());
        ++cancels_rejected_;
        report.rejected_cancel_.push_back(RejectedCancel<Tag>{ref, /*by_broker=*/true});
        return;
      case SendOutcome::INDETERMINATE:
        // Marked *and* reported apart from `would_cancel_`. The mark was always here; reporting it
        // as a withdrawal that happened was not right beside it, because the order may still be
        // resting and a caller reading `would_cancel_` would believe it is not.
        ledger_.mark(working->order_.instrument_, "a cancellation request has an unknown outcome");
        // Counted like an acceptance, for the same reason a placement is: the request may well have
        // reached the broker, and a window that assumes it did not is the one that under-counts.
        guard_.note_sent(context_.now());
        ++cancels_indeterminate_;
        report.indeterminate_cancel_.push_back(ref);
        return;
      case SendOutcome::REFUSED_LOCALLY:
        // No `note_sent`, for the reason the placement path gives: nothing was sent, so the broker
        // counted nothing, and a rate limit must never become the reason a later order cannot go.
        // Reported beside the broker's own refusals because what it means for the order is the
        // same -- it was not withdrawn -- while `by_broker_` on the entry says which of the two
        // it was. Not `last_error()`: a later instruction in the same pass clears that.
        ++cancels_rejected_;
        report.rejected_cancel_.push_back(RejectedCancel<Tag>{ref, /*by_broker=*/false});
        return;
      case SendOutcome::UNAVAILABLE:
        // Not reported as a withdrawal that happened, because it did not. A cancellation nobody
        // sent must not read as one the broker has.
        ++instructions_unavailable_;
        report.unavailable_cancel_.push_back(ref);
        return;
    }
  }

  // Plans a weight-sized intent as the target position it states (`strategy::weight_target`).
  //
  // **A resting stop refuses it, and nothing is withdrawn.** Target reconciliation withdraws a
  // resting stop when a trade is needed and waits for the stop's own history before trading, which
  // only a `TargetPosition` in the instruction stream can do (`defer_if_waiting_on_stop`); a weight
  // intent is not one, and an intent has never touched a stop. So a weight on an instrument with a
  // stop resting is refused `STOP_ORDER_RESTING` and the stop stays -- unless the holding already
  // meets it, when nothing is traded or withdrawn at all.
  strategy::Plan<Tag> plan_weight(
    const strategy::OrderIntent& intent,
    const strategy::ReconcileInputs& inputs
  ) const {
    strategy::Plan<Tag> plan;
    const auto converted = strategy::weight_target(intent, inputs);
    if (!converted.target_) {
      plan.rejected_.push_back(strategy::Rejection{
        converted.refused_.value_or(strategy::RejectReason::NOT_REPRESENTABLE), intent.instrument(),
        intent.tag()});
      return plan;
    }
    if (stop_listed(intent.instrument())) {
      // Met already: nothing to trade, and the stop protects what the weight means to hold, which
      // is `STOP_ORDER_RESTING`'s own rule for a met target. Unlike a target, nothing else on the
      // instrument is withdrawn either.
      const auto held = context_.portfolio().net_quantity_of(intent.instrument());
      const auto gap = held ? decimal_subtract(*held, converted.target_->target_quantity_) : std::nullopt;
      if (gap && gap->coefficient() == 0) {
        return plan;
      }
      plan.rejected_.push_back(strategy::Rejection{
        strategy::RejectReason::STOP_ORDER_RESTING, intent.instrument(), intent.tag()});
      return plan;
    }
    return strategy::reconcile(*converted.target_, context_.portfolio(), inputs);
  }

  // Reconciles one queued instruction against the current portfolio.
  //
  // The reference data is held in a local for the duration of the call, because
  // `ReconcileInputs` borrows it rather than copying: an `Instrument` carries a tick table, and
  // copying one per instruction per bar would be paying repeatedly for a value that does not
  // change.
  strategy::Plan<Tag> plan_for(const Instruction<Tag>& instruction) {
    const auto id = instrument_of(instruction);
    const auto reference = context_.instrument(id);
    const auto band = band_for(id);

    strategy::ReconcileInputs inputs;
    inputs.instrument_ = reference ? &*reference : nullptr;
    inputs.reference_price_ = context_.last_price(id);
    inputs.limits_ = band.applied_;
    inputs.portfolio_value_ = portfolio_value_;
    inputs.portfolio_value_currency_ = portfolio_value_currency_;
    inputs.tolerance_ = tolerance_;
    inputs.reserved_rows_are_unconfirmed_ = keeps_sent_orders_until_listed;

    auto plan = [&] {
      if (const auto* target = std::get_if<strategy::TargetPosition>(&instruction)) {
        return strategy::reconcile(*target, context_.portfolio(), inputs);
      }
      if (const auto* intent = std::get_if<strategy::OrderIntent>(&instruction)) {
        // **A weight is a holding, so it is planned as the position it states.** Reconciled as an
        // order it would buy the whole allocation again at every rebalance, which is what
        // `SizeKind::TARGET_WEIGHT` promises it does not do; as a target it is diffed against the
        // position and the working orders, like any target.
        if (intent->size().kind() == strategy::SizeKind::TARGET_WEIGHT) {
          return plan_weight(*intent, inputs);
        }
        return strategy::reconcile(*intent, context_.portfolio(), inputs);
      }
      return strategy::Plan<Tag>{};
    }();

    // Counted per plan rather than per order, and counted even when the plan placed nothing: the
    // figure answers "how often did this run decline to use a band it was holding", which is a fact
    // about the policy. Whether it cost anything is the other counter's question.
    if (band.withheld_) {
      ++price_bands_withheld_;
      mark_withheld_band(plan, *band.withheld_);
    }
    return plan;
  }

  // Records what the broker said about `order`, where the executor can say.
  //
  // Called at the `REJECTED` outcome and nowhere else, which is the only moment the answer is this
  // order's: `SessionExecutor::last_error()` is cleared by the next accepted instruction in the same
  // pass, so reading it later would attribute somebody else's silence to this refusal -- or nothing
  // at all.
  void note_broker_refusal(const strategy::PlannedOrder& order) {
    if constexpr (runner_detail::states_last_error<Executor>::value) {
      const auto& error = executor_.last_error();
      if (!error) {
        // **Cleared, not left alone.** An executor may answer `REJECTED` and state nothing; the
        // counter still moves, and a record kept from an *earlier* refusal would then be handed to
        // the next run as though it described this one -- the wrong instrument under the wrong tag.
        // `SessionExecutor` always states something on this path, so this is about a binding
        // somebody else writes: the contract is "the last refusal, or nothing", never "the last
        // refusal that happened to come with words".
        last_broker_refusal_.reset();
        return;
      }
      BrokerRefusal refusal;
      refusal.code_ = error->broker_code_;
      refusal.message_ = error->message_;
      refusal.instrument_ = order.instrument_;
      refusal.tag_ = order.tag_;
      last_broker_refusal_ = std::move(refusal);
    } else {
      static_cast<void>(order);
    }
  }

  // Returns which instrument a queued instruction concerns.
  static InstrumentId instrument_of(const Instruction<Tag>& instruction) {
    if (const auto* target = std::get_if<strategy::TargetPosition>(&instruction)) {
      return target->instrument_;
    }
    if (const auto* intent = std::get_if<strategy::OrderIntent>(&instruction)) {
      return intent->instrument();
    }
    return InstrumentId{};
  }

  // A band and what its age is measured from; see `set_price_limits`.
  struct DatedBand {
    // The band as the broker published it.
    PriceLimits limits_;
    // When this runner was told about it, which is all an unstamped band has.
    std::chrono::system_clock::time_point received_at_{};
    // Returns the instant the band's age is measured from; see `set_price_limits`.
    //
    // **The exchange's stamp may only make a band look older, never fresher.** It is there to catch
    // a successful fetch of a week-old band, which is an ageing question; a stamp *later* than the
    // moment this process received it answers nothing -- it is two clocks disagreeing, by ordinary
    // skew at best and by a broker's bug at worst -- and taking it at face value would let a band
    // date itself into the future and outlive any stated maximum. Capping it at the receipt instant
    // keeps the case it exists for and gives the other one no purchase, with no invented tolerance
    // for how far apart two clocks may be.
    std::chrono::system_clock::time_point as_of() const noexcept {
      if (!limits_.exchange_time_) {
        return received_at_;
      }
      return *limits_.exchange_time_ < received_at_ ? *limits_.exchange_time_ : received_at_;
    }
  };
  // Which band a plan may use, and which it was denied; see `band_for`.
  struct BandDecision {
    // The band to clamp with, when there is one this runner will stand behind.
    std::optional<PriceLimits> applied_;
    // The band that was held back, when one was, so the decision can be counted.
    std::optional<PriceLimits> withheld_;
  };

  // Returns the band to apply to `id`'s prices, and separately the one that was held back.
  //
  // Exactly one of the two is filled when a band is registered, and neither when none is. The
  // withheld half exists so the decision can be counted and reported: a band silently dropped is
  // the same failure as a band silently applied.
  BandDecision band_for(const InstrumentId& id) const {
    BandDecision decision;
    const auto found = price_limits_.find(id);
    if (found == price_limits_.end()) {
      return decision;
    }
    if (!maximum_price_limits_age_ && !allow_unbounded_price_limits_age_) {
      // No policy stated. Not applied, and deliberately not the same as an absent band: the caller
      // asked for clamping and is getting none, which `price_bands_withheld` reports and a driving
      // loop names in its notes.
      decision.withheld_ = found->second.limits_;
      return decision;
    }
    // **A band dated after the instant it is being aged at is not applied, whatever the policy.**
    // `as_of` can no longer exceed the receipt stamp, so the only way here is a caller moving
    // `set_observed_at` backwards -- reporting day 20, registering a band, then reporting day 10.
    // The subtraction below would then be negative and pass every positive maximum, so a band from
    // the caller's own future would clamp. A duration this runner cannot compute is not an age, and
    // a band whose age is unknown is the case the whole policy exists for.
    if (found->second.as_of() > freshness_instant()) {
      decision.withheld_ = found->second.limits_;
      return decision;
    }
    if (maximum_price_limits_age_ &&
        freshness_instant() - found->second.as_of() > *maximum_price_limits_age_) {
      decision.withheld_ = found->second.limits_;
      return decision;
    }
    decision.applied_ = found->second.limits_;
    return decision;
  }

  // Marks the orders in `plan` whose price the band that was **not** applied would have moved.
  //
  // A statement about the planned price, not a prediction about the wire: clamping happens before
  // tick rounding (`reconcile_detail::prepare_price`), so a bound that binds here could in principle
  // round back to the same price. What the flag says is the checkable thing -- this order's price
  // lies outside the withheld band on the side the clamp corrects.
  static void mark_withheld_band(strategy::Plan<Tag>& plan, const PriceLimits& withheld) {
    for (auto& order : plan.place_) {
      if (!order.price_) {
        continue;
      }
      bool would_move = false;
      static_cast<void>(strategy::reconcile_detail::clamp_to_limits(
        *order.price_, order.side_, std::optional<PriceLimits>(withheld), would_move
      ));
      order.price_would_have_been_clamped_ = would_move;
    }
  }

  Strategy strategy_;
  strategy::RiskGuard guard_;
  // Placements sent that may have left an order resting; see `placements_at_broker`.
  std::size_t placements_at_broker_{0};
  // Orders sent under a phase the limits did not permit; see `sends_outside_permitted_phase`.
  std::size_t sends_outside_permitted_phase_{0};
  // How old a snapshot may be before a plan derived from it is withheld; see the setter.
  std::optional<std::chrono::nanoseconds> maximum_portfolio_age_;
  // The last wall-clock instant a driving loop reported; see `set_observed_at`.
  std::chrono::system_clock::time_point observed_at_{};
  // Whether the caller stated this runner may act with no snapshot at all; see the setter.
  bool allow_absent_portfolio_{false};
  // Whether a placement needs a portfolio that has actually been read; see the setter.
  //
  // Defaulted from the executor: a runner that can reach a broker requires it, and one that cannot
  // does not. That is what makes the rule hold for a live runner assembled by hand rather than only
  // for one the binding factories produced.
  bool require_known_portfolio_{runner_detail::sends_to_a_broker<Executor>::value};
  bool probing_sellable_{false};
  strategy::SellableConventionProbe<Tag> sellable_probe_;
  Context context_;
  // Records the first local failure, keeping it rather than overwriting with a later one.
  void note_local_failure(std::string detail) {
    // The *first* reason is kept, not the latest: a storage failure cascades, and the reason the
    // cascade started is the one an operator needs. Taking the hold again would only replace it.
    if (!guard_.halted(strategy::HaltReason::LOCAL_FAILURE)) {
      guard_.halt(strategy::HaltReason::LOCAL_FAILURE, std::move(detail));
    }
  }

  // Makes the executor's own local failure sticky on the runner, if it reported one.
  //
  // Called after every send rather than only after a refusal, because the two are not the same
  // event. A *pre-send* journal write that fails produces `UNAVAILABLE` and nothing was sent. A
  // *terminal* journal write that fails happens after the broker has already answered: the send
  // really did happen, so the outcome passed back stays the broker's, and only this call records
  // that the journal is no longer trustworthy.
  void note_executor_failure(bool refused) {
    const auto failures = executor_journal_failures();
    if (failures > acknowledged_journal_failures_) {
      auto detail = executor_journal_error();
      note_local_failure(
        detail.empty() ? std::string("the order journal could not be written") : std::move(detail)
      );
      return;
    }
    if (refused && failures == 0) {
      // An executor with no journal behind it can still refuse locally, and it reports no count.
      // Nothing to acknowledge, so a refusal always halts and a resume that does not fix the cause
      // simply halts again on the next send -- which is the truthful outcome.
      note_local_failure("local machinery refused to send");
    }
  }

  // Returns how many journal writes the executor has had fail, or zero when it cannot say.
  std::size_t executor_journal_failures() const { return executor_journal_failures_impl(0); }

  template <typename E = Executor>
  auto executor_journal_failures_impl(int) const
    -> decltype(std::declval<const E&>().journal_failures()) {
    return executor_.journal_failures();
  }

  std::size_t executor_journal_failures_impl(...) const { return 0; }

  // Returns what the executor says went wrong, or empty when it reports nothing or cannot.
  std::string executor_journal_error() const { return executor_journal_error_impl(0); }

  template <typename E = Executor>
  auto executor_journal_error_impl(int) const
    -> decltype(std::declval<const E&>().last_journal_error(), std::string()) {
    return executor_.last_journal_error();
  }

  std::string executor_journal_error_impl(...) const { return {}; }

  Executor executor_;
  IndeterminacyLedger ledger_;
  std::size_t acknowledged_journal_failures_{0};
  std::map<InstrumentId, DatedBand> price_limits_;
  // How old a band may be before it is not applied; see `set_maximum_price_limits_age`.
  std::optional<std::chrono::nanoseconds> maximum_price_limits_age_;
  // Whether the caller stated this runner applies a band of any age; see the setter.
  bool allow_unbounded_price_limits_age_{false};
  // Plans built while a band was held and not applied; see `price_bands_withheld`.
  std::size_t price_bands_withheld_{0};
  // Sent orders a broker refused after a withheld band would have moved them; see the accessor.
  std::size_t unclamped_orders_rejected_{0};
  // Orders nothing sent to a broker while carrying an unclamped price; see the accessor.
  std::size_t unclamped_orders_refused_{0};
  // Instructions that failed by each origin, and refusals a broker made; see the accessors.
  std::size_t orders_unbuildable_{0};
  // @copydoc Runner::orders_unbuildable_
  std::size_t orders_reconcile_withheld_{0};
  // @copydoc Runner::orders_unbuildable_
  std::size_t orders_guard_refused_{0};
  // @copydoc Runner::orders_unbuildable_
  std::size_t orders_binding_refused_{0};
  // @copydoc Runner::orders_unbuildable_
  std::size_t orders_broker_refused_{0};
  // @copydoc Runner::orders_unbuildable_
  std::size_t orders_indeterminate_{0};
  // Withdrawals that failed, by origin; see `cancels_rejected`.
  std::size_t cancels_rejected_{0};
  // @copydoc Runner::cancels_rejected_
  std::size_t cancels_refused_{0};
  // @copydoc Runner::cancels_rejected_
  std::size_t cancels_unresolved_{0};
  // @copydoc Runner::cancels_rejected_
  std::size_t cancels_indeterminate_{0};
  // Plans held back whole, for any of three reasons; see `plans_withheld`.
  std::size_t plans_withheld_{0};
  std::size_t warmup_instructions_discarded_{0};
  // One target waiting for a stop's withdrawal, and when a listing first showed the stop gone.
  struct DeferredTarget {
    // The target, exactly as the strategy stated it.
    strategy::TargetPosition target_;
    // The stops it waits on, whose own history rows decide its release, with the order time their
    // listing showed (if any) to tell them from another day's order under the same number.
    struct Stop {
      OrderRef<Tag> ref_;
      std::optional<std::chrono::system_clock::time_point> ordered_at_;
    };
    std::vector<Stop> stops_;
    // When it was deferred, which bounds the history that can hold those rows.
    std::chrono::system_clock::time_point deferred_at_{};
  };
  // Targets waiting for a stop's withdrawal; see `deferred_targets`.
  std::vector<DeferredTarget> deferred_targets_;

  std::vector<Unconfirmed> unconfirmed_;
  // Serialized mode; see `serialized`.
  static constexpr bool can_serialize =
    keeps_sent_orders_until_listed && runner_detail::serializes<Executor>::value;
  bool overlapping_{false};
  bool recovered_{false};
  std::vector<Boundary> boundaries_;
  std::size_t orders_awaiting_confirmation_{0};
  std::chrono::nanoseconds confirmation_timeout_{std::chrono::minutes(10)};
  std::size_t orders_unconfirmed_timed_out_{0};
  // Targets withheld for want of a trustworthy account, one per instrument; see `retry_retained_targets`.
  // A retained target, and whether it waits for the market phase rather than for the account.
  struct RetainedTarget {
    strategy::TargetPosition target_;
    bool for_phase_{false};
    // Kept because the broker could not be reached: planned again only after a later holdings read.
    std::optional<std::chrono::system_clock::time_point> after_;
  };
  std::vector<RetainedTarget> retained_targets_;
  // An order intent kept for the phase, and the instrument's net holding when it was kept.
  struct KeptIntent {
    strategy::OrderIntent intent_;
    Decimal64 held_;
    // As for `RetainedTarget::after_`.
    std::optional<std::chrono::system_clock::time_point> after_;
  };
  std::vector<KeptIntent> kept_intents_;
  std::size_t kept_intents_dropped_{0};
  // The instructions kept for the phase in the current pass, so each is kept once.
  std::vector<const Instruction<Tag>*> kept_this_pass_;
  // True only inside a deferred release's settle, which `plan_withhold_reason` exempts.
  bool releasing_deferred_{false};
  // How many were ever deferred; see `targets_deferred`.
  std::size_t targets_deferred_{0};
  // How many were dropped after a fill; see `deferred_dropped_after_fill`.
  std::size_t deferred_dropped_after_fill_{0};
  // Instructions local machinery was unavailable to send; see the accessor.
  std::size_t instructions_unavailable_{0};
  // What the broker said about the most recent order it refused; see the accessor.
  std::optional<BrokerRefusal> last_broker_refusal_;
  std::optional<strategy::Rejection> last_unbuildable_;
  std::optional<Decimal64> portfolio_value_;
  std::optional<Currency> portfolio_value_currency_;
  strategy::TradeTolerance tolerance_;
};

/**
 * @brief A runner that sends nothing: handlers fire and instructions are recorded by a
 *   `RecordingExecutor`.
 *
 * It runs the same pipeline as a live session. A dry-run program contains no broker or network code
 * (though the package's link line still carries the transport libraries).
 *
 * @code
 * runner::DryRunner<MyStrategy> dry(MyStrategy{}, limits);
 * dry.context().set_instrument(reference);
 * const auto report = dry.feed(strategy::make_bar_event(id, BarInterval::ONE_DAY, BarStamp::CLOSE, bar));
 * // report.would_place_ holds what would have been sent
 * @endcode
 *
 * @tparam Strategy The strategy type.
 * @tparam CapsType The broker capabilities the strategy sees.
 */
template <typename Strategy, typename CapsType = strategy::NoCaps>
using DryRunner = Runner<Strategy, CapsType, DryTag, RecordingExecutor<DryTag>>;

} // namespace runner
} // namespace algotradelib
