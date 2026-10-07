#pragma once

#include "./clock.hpp"
#include "./execution.hpp"
#include "./stop.hpp"

#include <algotradelib/core>
#include <algotradelib/strategy>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace algotradelib {
namespace runner {

/**
 * @brief What a `PollingLoop` polls for; each has a cadence in `PollSchedule`.
 *
 * Each iteration polls in the order of `all_poll_kinds`: calendar, price limits, working orders,
 * holdings, buying power, bars, quotes.
 */
enum class PollKind {
  /** @brief Completed bars. */
  BARS,
  /** @brief Current quotes, for strategies that act between bars. */
  QUOTES,
  /** @brief Holdings, which orders are sized against. */
  HOLDINGS,
  /** @brief Working orders, which reconciliation and cancellation need. */
  WORKING_ORDERS,
  /** @brief Buying power, polled beside holdings (not before each order). */
  BUYING_POWER,
  /**
   * @brief The trading calendar, from which the loop derives the market phase; polled first, and on
   *   by default.
   *
   * @note A source must install the calendar during this poll only, and must not feed the runner
   *   from it (`live::SessionSource::poll_calendar` does exactly that). A call that both sends an
   *   order and changes the phase is counted (`LoopReport::sends_under_a_revised_phase_`).
   */
  CALENDAR,
  /**
   * @brief Today's price bands, which limit prices are clamped into; polled second, before anything
   *   that feeds.
   *
   * It installs bands with `Runner::set_price_limits` and feeds nothing. Orders decided from a poll
   * use this iteration's bands; an order decided inside `Source::read_order_list` (which runs before
   * the polls) would use the previous iteration's.
   */
  PRICE_LIMITS,
};

/** @brief The number of `PollKind` values. */
inline constexpr std::size_t poll_kind_count = 7;

/**
 * @brief Returns a poll kind's lowercase name, such as `"working_orders"`.
 * @param kind The kind.
 * @return The name.
 */
inline constexpr std::string_view poll_kind_name(PollKind kind) noexcept {
  switch (kind) {
    case PollKind::CALENDAR:
      return "calendar";
    case PollKind::BARS:
      return "bars";
    case PollKind::QUOTES:
      return "quotes";
    case PollKind::HOLDINGS:
      return "holdings";
    case PollKind::WORKING_ORDERS:
      return "working_orders";
    case PollKind::BUYING_POWER:
      return "buying_power";
    case PollKind::PRICE_LIMITS:
      return "price_limits";
  }
  return "unknown";
}

/** @brief Every poll kind, in the order an iteration polls them. */
inline constexpr PollKind all_poll_kinds[] = {
  // First: the phase derived from it decides whether anything else in this iteration may be sent.
  PollKind::CALENDAR,
  // Second, before every poll that can feed, so this iteration's orders use this iteration's bands.
  PollKind::PRICE_LIMITS,
  // The listing, then holdings, before anything that feeds. A fill between the two reads then
  // over-counts the position (harmless) rather than under-counting it (which would buy again).
  PollKind::WORKING_ORDERS,
  PollKind::HOLDINGS,
  // After holdings, so money and positions are equally fresh.
  PollKind::BUYING_POWER,
  PollKind::BARS,
  PollKind::QUOTES,
};

static_assert(
  sizeof(all_poll_kinds) / sizeof(all_poll_kinds[0]) == poll_kind_count,
  "every PollKind must appear in all_poll_kinds: a kind left out is never polled, and the loop "
  "would silently answer with whatever the previous poll left behind"
);

/**
 * @brief What became of one poll.
 *
 * Only `FAILED` counts toward `FailurePolicy`; nothing new (`UNCHANGED`) and a rate-limiter
 * refusal (`RATE_LIMITED`) do not.
 */
enum class PollOutcome {
  /** @brief New data arrived and was fed to the runner. */
  UPDATED,
  /** @brief The poll succeeded and there was nothing new. */
  UNCHANGED,
  /** @brief The poll failed; counted toward the failure budget. */
  FAILED,
  /** @brief The rate limiter refused the poll; not counted as a failure. */
  RATE_LIMITED,
};

/**
 * @brief Returns an outcome's lowercase name, such as `"rate_limited"`.
 * @param outcome The outcome.
 * @return The name.
 */
inline constexpr std::string_view poll_outcome_name(PollOutcome outcome) noexcept {
  switch (outcome) {
    case PollOutcome::UPDATED:
      return "updated";
    case PollOutcome::UNCHANGED:
      return "unchanged";
    case PollOutcome::FAILED:
      return "failed";
    case PollOutcome::RATE_LIMITED:
      return "rate_limited";
  }
  return "unknown";
}

/**
 * @brief How often a `PollingLoop` polls each kind; an unset interval disables that poll.
 *
 * Only `calendar_` is on by default. `tick_` is the loop's own step: it bounds how quickly a stop is
 * noticed, even when no poll is due.
 *
 * @note Members are positional in aggregate initialization (`PollSchedule{std::chrono::seconds(5)}`
 *   sets `bars_`); new members are appended.
 */
struct PollSchedule {
  /** @brief How often to fetch bars. */
  std::optional<std::chrono::nanoseconds> bars_;
  /** @brief How often to fetch quotes. */
  std::optional<std::chrono::nanoseconds> quotes_;
  /** @brief How often to read holdings; without it the portfolio is never refreshed. */
  std::optional<std::chrono::nanoseconds> holdings_;
  /**
   * @brief How often to read the working orders.
   *
   * Do not make it shorter than `holdings_`: a listing newer than the holdings withholds every plan
   * as `PORTFOLIO_SUPERSEDED` (equal cadences read both in one iteration, listing first). Required
   * for a runner that sends to a broker: without it, or when the last listing is older than
   * `PollingLoop::set_maximum_portfolio_age`, the loop holds placements as `PORTFOLIO_STALE` (leave a
   * tick of margin between the cadence and the age).
   */
  std::optional<std::chrono::nanoseconds> working_orders_;
  /**
   * @brief How often to read buying power.
   *
   * With `RiskLimits::enforce_buying_power_` on and this unset, every placement is held
   * (`BUYING_POWER_UNREAD`).
   */
  std::optional<std::chrono::nanoseconds> buying_power_;
  /** @brief The loop's step, which bounds how quickly a stop is noticed; not a poll. */
  std::chrono::nanoseconds tick_{std::chrono::seconds(1)};
  /**
   * @brief How often to read the trading calendar; six hours by default.
   *
   * Without a calendar the phase is `UNKNOWN` and the default `RiskLimits::permitted_phases_`
   * refuses every order that opens or increases a position (closing a position is still allowed).
   * Keep it under the nine hours between local midnight and a 09:00 open. KIS's client caches the
   * calendar, so four polls a day cost about one request.
   */
  std::optional<std::chrono::nanoseconds> calendar_{std::chrono::hours(6)};
  /**
   * @brief How often to read today's price bands.
   *
   * A band is valid for one trading day. Bands are applied only under an age policy; see
   * `PollingLoop::set_maximum_price_limits_age`.
   */
  std::optional<std::chrono::nanoseconds> price_limits_;
  /**
   * @brief Returns the interval for a poll kind.
   * @param kind The kind.
   * @return The interval, or `std::nullopt` if that poll is disabled.
   */
  std::optional<std::chrono::nanoseconds> interval_for(PollKind kind) const noexcept {
    switch (kind) {
      case PollKind::CALENDAR:
        return calendar_;
      case PollKind::BARS:
        return bars_;
      case PollKind::QUOTES:
        return quotes_;
      case PollKind::HOLDINGS:
        return holdings_;
      case PollKind::WORKING_ORDERS:
        return working_orders_;
      case PollKind::BUYING_POWER:
        return buying_power_;
      case PollKind::PRICE_LIMITS:
        return price_limits_;
    }
    return std::nullopt;
  }
};

/**
 * @brief Detects whether a source declares `static constexpr bool provides_working_orders = true`,
 *   meaning it leaves the broker's working orders in the portfolio after an order-list read.
 *
 * `false` when undeclared; a `WITHDRAW` then does not report a complete stop from that source.
 *
 * @tparam Source The source type.
 */
template <typename Source, typename = void>
struct source_provides_working_orders : std::false_type {};

template <typename Source>
struct source_provides_working_orders<
  Source, std::void_t<decltype(Source::provides_working_orders)>
> : std::bool_constant<Source::provides_working_orders> {};

/**
 * @brief Detects `Runner::keeps_sent_orders_until_listed`.
 *
 * `false` when undeclared, so a type wrapping `runner::Runner` for `PollingLoop` must forward the
 * constant, or the loop does not hold it for a missing working-order listing.
 *
 * @tparam Runner The runner type.
 */
template <typename Runner, typename = void>
struct runner_keeps_sent_orders : std::false_type {};

template <typename Runner>
struct runner_keeps_sent_orders<
  Runner, std::void_t<decltype(Runner::keeps_sent_orders_until_listed)>
> : std::bool_constant<Runner::keeps_sent_orders_until_listed> {};

/**
 * @brief Detects whether a runner reports `kept_intents_dropped()`.
 * @tparam Runner The runner type.
 */
template <typename Runner, typename = void>
struct runner_counts_kept_drops : std::false_type {};

template <typename Runner>
struct runner_counts_kept_drops<
  Runner, std::void_t<decltype(std::declval<const Runner&>().kept_intents_dropped())>
> : std::true_type {};

/**
 * @brief Detects whether a runner reports `warmup_instructions_discarded()`.
 * @tparam Runner The runner type.
 */
template <typename Runner, typename = void>
struct runner_counts_warmup : std::false_type {};

template <typename Runner>
struct runner_counts_warmup<
  Runner, std::void_t<decltype(std::declval<const Runner&>().warmup_instructions_discarded())>
> : std::true_type {};

/**
 * @brief Commission and tax the broker stated on the orders a loop happened to poll (Toss states
 *   them; KIS does not).
 *
 * A subtotal of unknown coverage, not a total: an order that fills and leaves the working-order
 * listing between two polls, or is placed and filled between them, is never seen with its final
 * cost.
 */
struct ChargedCosts {
  /**
   * @brief One currency's running figure and how many statements contributed to it.
   *
   * The count tells a stated zero apart from silence.
   */
  struct StatedTotal {
    /** @brief The running figure. */
    Money amount_;
    /** @brief How many statements contributed. */
    std::size_t statements_{0};
  };

  /** @brief Commission stated, per currency; a currency with no statement has no entry (not zero). */
  std::vector<StatedTotal> commission_;
  /** @brief Tax stated, per currency, on the same terms. */
  std::vector<StatedTotal> tax_;
  /** @brief How many orders have stated a cost (including zero); counted once per order, never decreased. */
  std::size_t orders_stating_cost_{0};
  /**
   * @brief How many orders were first seen stating no cost (every KIS order); never decreased, so an
   *   order that later states one is counted in both.
   */
  std::size_t orders_stating_none_{0};
  /**
   * @brief How many orders left the working-order listing between two polls (filled, cancelled or
   *   expired); a diagnostic, not a completeness measure.
   */
  std::size_t working_list_disappearances_{0};
  /**
   * @brief How many times an order restated a different cost at the same filled quantity; the first
   *   figure is kept.
   */
  std::size_t conflicting_restatements_{0};
  /**
   * @brief How many additions or subtractions did not fit a `Decimal64`; nonzero means the figures
   *   are short (it counts attempts, and a re-polled row counts again).
   */
  std::size_t unrepresentable_sums_{0};

  /**
   * @brief Returns the commission stated in a currency.
   * @param currency The currency.
   * @return The figure, or `std::nullopt` if none was stated.
   */
  std::optional<Decimal64> commission_in(const Currency& currency) const {
    return figure_in(commission_, currency);
  }

  /**
   * @brief Returns the tax stated in a currency.
   * @param currency The currency.
   * @return The figure, or `std::nullopt` if none was stated.
   */
  std::optional<Decimal64> tax_in(const Currency& currency) const {
    return figure_in(tax_, currency);
  }

  /**
   * @brief Returns how many commission statements a currency has.
   * @param currency The currency.
   * @return The count.
   */
  std::size_t commission_statements_in(const Currency& currency) const {
    return statements_in(commission_, currency);
  }

  /**
   * @brief Returns how many tax statements a currency has.
   * @param currency The currency.
   * @return The count.
   */
  std::size_t tax_statements_in(const Currency& currency) const {
    return statements_in(tax_, currency);
  }

  /**
   * @brief Adds an amount to its currency's figure in a list of totals.
   * @param totals `commission_` or `tax_`.
   * @param amount The amount.
   * @return `false` (counted in `unrepresentable_sums_`, figure unchanged) if the sum does not fit;
   *   the caller must not then treat the amount as counted.
   */
  bool add(std::vector<StatedTotal>& totals, const Money& amount) {
    for (auto& held : totals) {
      if (held.amount_.currency_ == amount.currency_) {
        const auto sum = decimal_add(held.amount_.amount_, amount.amount_);
        if (!sum) {
          ++unrepresentable_sums_;
          return false;
        }
        held.amount_.amount_ = *sum;
        ++held.statements_;
        return true;
      }
    }
    totals.push_back(StatedTotal{amount, 1});
    return true;
  }

  /**
   * @brief Returns a currency's figure from a list of totals.
   * @param totals The totals.
   * @param currency The currency.
   * @return The figure, or `std::nullopt` if none.
   */
  static std::optional<Decimal64> figure_in(
    const std::vector<StatedTotal>& totals,
    const Currency& currency
  ) {
    for (const auto& held : totals) {
      if (held.amount_.currency_ == currency) {
        return held.amount_.amount_;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Returns a currency's statement count from a list of totals.
   * @param totals The totals.
   * @param currency The currency.
   * @return The count; zero if none.
   */
  static std::size_t statements_in(
    const std::vector<StatedTotal>& totals,
    const Currency& currency
  ) {
    for (const auto& held : totals) {
      if (held.amount_.currency_ == currency) {
        return held.statements_;
      }
    }
    return 0;
  }

  /**
   * @brief Returns what was observed since an earlier copy, for one run's report.
   *
   * A currency appears if its statement count rose (so a stated zero is kept). A money delta can be
   * negative if the broker revised a figure down.
   *
   * @param before The earlier copy.
   * @return The difference; a subtraction that does not fit is counted in `unrepresentable_sums_`.
   */
  ChargedCosts since(const ChargedCosts& before) const {
    ChargedCosts delta;
    std::size_t refused = 0;
    const auto difference = [&refused](
      const std::vector<StatedTotal>& now,
      const std::vector<StatedTotal>& then
    ) {
      std::vector<StatedTotal> out;
      for (const auto& held : now) {
        auto earlier = Decimal64::parse("0");
        std::size_t before_statements = 0;
        for (const auto& was : then) {
          if (was.amount_.currency_ == held.amount_.currency_) {
            earlier = was.amount_.amount_;
            before_statements = was.statements_;
            break;
          }
        }
        // Whether anything was stated is the statement count, not the money.
        if (held.statements_ == before_statements) {
          continue;
        }
        const auto moved = decimal_subtract(held.amount_.amount_, earlier);
        if (!moved) {
          // Counted, not skipped: something was stated and its amount cannot be represented.
          ++refused;
          continue;
        }
        out.push_back(
          StatedTotal{Money{*moved, held.amount_.currency_}, held.statements_ - before_statements}
        );
      }
      return out;
    };
    delta.commission_ = difference(commission_, before.commission_);
    delta.tax_ = difference(tax_, before.tax_);
    delta.orders_stating_cost_ = orders_stating_cost_ - before.orders_stating_cost_;
    delta.orders_stating_none_ = orders_stating_none_ - before.orders_stating_none_;
    delta.working_list_disappearances_ =
      working_list_disappearances_ - before.working_list_disappearances_;
    delta.conflicting_restatements_ = conflicting_restatements_ - before.conflicting_restatements_;
    delta.unrepresentable_sums_ =
      (unrepresentable_sums_ - before.unrepresentable_sums_) + refused;
    return delta;
  }
};

/**
 * @brief Detects whether a source provides `const ChargedCosts& charged_costs() const`, which
 *   `LoopReport` then reports.
 * @tparam Source The source type.
 */
template <typename Source, typename = void>
struct source_states_charged_costs : std::false_type {};

template <typename Source>
struct source_states_charged_costs<
  Source, std::void_t<decltype(std::declval<const Source&>().charged_costs())>
> : std::true_type {};

/**
 * @brief Detects whether a source provides `const std::vector<Currency>& buying_power_currencies()
 *   const`.
 *
 * With it (and currencies named), the buying-power gate holds until every named currency has a fresh
 * reading; otherwise it needs at least one reading, and every reading present must be fresh.
 *
 * @tparam Source The source type.
 */
template <typename Source, typename = void>
struct source_names_buying_power_currencies : std::false_type {};

template <typename Source>
struct source_names_buying_power_currencies<
  Source, std::void_t<decltype(std::declval<const Source&>().buying_power_currencies())>
> : std::true_type {};

/** @brief How a `PollingLoop` responds to polls that keep failing. */
struct FailurePolicy {
  /** @brief Consecutive failures of one poll kind after which the loop stops (`POLL_FAILING`). */
  std::size_t consecutive_limit_{5};
};

/** @brief What one loop iteration did. */
struct IterationReport {
  /** @brief When the iteration ran. */
  std::chrono::system_clock::time_point at_{};
  /** @brief What each poll returned, in order. */
  std::vector<std::pair<PollKind, PollOutcome>> polls_;
  /**
   * @brief The hold that blocked placements, if any.
   *
   * A named cancellation may still have gone out under a hold that only means data is missing
   * (`strategy::halt_blocks`).
   */
  std::optional<strategy::HaltReason> gate_;
  /** @brief Whether anything was fed to the runner. */
  bool fed_{false};
  /**
   * @brief The market phase the loop computed.
   *
   * Under the default limits, `UNKNOWN` means every placement that would open or increase a position
   * was refused (closing ones are still allowed).
   */
  strategy::MarketPhase phase_{strategy::MarketPhase::UNKNOWN};
  /** @brief Whether a phase change was announced to the strategy. */
  bool phase_changed_{false};

  /**
   * @brief Tests whether any poll brought new data.
   * @return `true` if any poll was `UPDATED`.
   */
  bool updated() const noexcept {
    for (const auto& entry : polls_) {
      if (entry.second == PollOutcome::UPDATED) {
        return true;
      }
    }
    return false;
  }
};

/**
 * @brief What a whole `PollingLoop::run` did, and why it ended.
 *
 * Read `notes_` first; they explain anything the run could not do. Scope of the counters: the
 * `orders_*`, `cancels_*`, `plans_withheld_` and `instructions_unavailable_` outcomes (and
 * `last_broker_refusal_`, `last_unbuildable_`) cover the run's polling, before any closing pass,
 * whose results are the `closing_*` fields; `price_bands_withheld_`, `unclamped_orders_*` and
 * `sends_outside_permitted_phase_` cover the whole run, closing pass included.
 */
struct LoopReport {
  /** @brief How many iterations ran. */
  std::size_t iterations_{0};
  /** @brief Commission and tax the broker stated on the orders this run saw; a subtotal (see `ChargedCosts`). */
  ChargedCosts charged_costs_;
  /** @brief How many polls of each kind were attempted, indexed by `PollKind`; see `attempted()`. */
  std::size_t attempted_[poll_kind_count] = {};
  /** @brief How many polls of each kind failed; see `failed()`. */
  std::size_t failed_[poll_kind_count] = {};
  /** @brief How many iterations were blocked from placing (a named cancellation may still have gone). */
  std::size_t blocked_{0};
  /** @brief The stop intent that ended the run, if one did. */
  std::optional<StopIntent> stopped_by_;
  /** @brief The most fundamental hold in force when the run ended, whoever took it. */
  std::optional<strategy::HaltReason> final_gate_;
  /** @brief The phase the last iteration computed. */
  strategy::MarketPhase final_phase_{strategy::MarketPhase::UNKNOWN};
  /**
   * @brief How many calls into a source or strategy both sent something and left the market phase
   *   changed.
   *
   * Always zero with `live::SessionSource` and a strategy that does not write the calendar; nonzero
   * means some code breaks the rule in `PollKind::CALENDAR` (co-occurrence, not proof of a stale send).
   */
  std::size_t sends_under_a_revised_phase_{0};
  /**
   * @brief Plans built while a known price band was not applied (no age policy, too old, or dated
   *   after the clock); see `Runner::price_bands_withheld`.
   */
  std::size_t price_bands_withheld_{0};
  /** @brief Sent orders the broker refused while carrying a price a withheld band would have clamped; see `Runner::unclamped_orders_rejected`. */
  std::size_t unclamped_orders_rejected_{0};
  /** @brief Orders refused before sending while carrying such a price; see `Runner::unclamped_orders_refused`. */
  std::size_t unclamped_orders_refused_{0};
  /** @brief Orders reconciliation could not build during this run's polling; see `Runner::orders_unbuildable`. */
  std::size_t orders_unbuildable_{0};
  /** @brief Orders held back for an instrument of unknown order state; see `Runner::orders_reconcile_withheld`. */
  std::size_t orders_reconcile_withheld_{0};
  /** @brief Orders the risk guard refused; see `Runner::orders_guard_refused`. */
  std::size_t orders_guard_refused_{0};
  /** @brief Orders the binding refused to send; see `Runner::orders_binding_refused`. */
  std::size_t orders_binding_refused_{0};
  /**
   * @brief Orders the broker refused (`SendOutcome::REJECTED`); see `last_broker_refusal_`. At KIS,
   *   Toss and Kiwoom most refusals arrive as unknown outcomes and are counted in
   *   `orders_indeterminate_`.
   */
  std::size_t orders_broker_refused_{0};
  /**
   * @brief Orders with an unknown outcome (`SendOutcome::INDETERMINATE`, including most KIS, Toss and
   *   Kiwoom refusals).
   */
  std::size_t orders_indeterminate_{0};
  /** @brief Sent orders that timed out unconfirmed, each withholding its instrument; see `Runner::set_confirmation_timeout`. */
  std::size_t orders_unconfirmed_timed_out_{0};
  /** @brief Whole plans held back (any `WithheldPlanReason`); see `Runner::plans_withheld`. */
  std::size_t plans_withheld_{0};
  /** @brief Instructions not sent because local machinery was unavailable; see `Runner::instructions_unavailable`. */
  std::size_t instructions_unavailable_{0};
  /** @brief Cancellations refused by the broker or binding; see `Runner::cancels_rejected`. */
  std::size_t cancels_rejected_{0};
  /** @brief Cancellations a hold refused. */
  std::size_t cancels_refused_{0};
  /** @brief Cancellations naming an order the portfolio does not list. */
  std::size_t cancels_unresolved_{0};
  /** @brief Cancellations with an unknown outcome. */
  std::size_t cancels_indeterminate_{0};
  /** @brief The broker's code and message for the last order it refused during this run, if any. */
  std::optional<BrokerRefusal> last_broker_refusal_;
  /** @brief Why reconciliation could not build the last such instruction in this run, if any. */
  std::optional<strategy::Rejection> last_unbuildable_;
  /** @brief How many phase changes were announced to the strategy. */
  std::size_t phase_transitions_{0};
  /**
   * @brief Orders sent in a phase `RiskLimits::permitted_phases_` does not permit, because they close
   *   toward flat; see `Runner::sends_outside_permitted_phase`.
   */
  std::size_t sends_outside_permitted_phase_{0};
  /** @brief Whether the loop gave up because a poll kept failing. */
  bool abandoned_{false};
  /** @brief The poll kind that exhausted its failure budget, if one did. */
  std::optional<PollKind> abandoned_kind_;
  /** @brief Whether the broker's order list was read at least once. */
  bool order_list_read_{false};
  /** @brief Cancellations queued for a `WITHDRAW` or `FLATTEN` (asked for, not necessarily sent). */
  std::size_t withdrawn_{0};
  /** @brief Closing orders queued for a `FLATTEN` (asked for, not necessarily sent). */
  std::size_t flattened_{0};
  /** @brief Closing orders (placements, not cancellations) accepted by the executor (for a dry run, recorded). */
  std::size_t closing_placed_{0};
  /** @brief Closing instructions (placements and cancellations) rejected by reconciliation, the broker or the binding. */
  std::size_t closing_rejected_{0};
  /** @brief Closing instructions the risk guard refused. */
  std::size_t closing_refused_{0};
  /** @brief Closing instructions held back because the instrument has an order of unknown outcome. */
  std::size_t closing_withheld_{0};
  /** @brief Closing instructions (cancellations included) with an unknown outcome. */
  std::size_t closing_indeterminate_{0};
  /** @brief Closing instructions not sent because local machinery was unavailable; fix it, `resume_after_local_failure`, and stop again. */
  std::size_t closing_unavailable_{0};
  /** @brief Closing cancellations naming an order the portfolio does not list; read the broker's list rather than retry. */
  std::size_t closing_unresolved_{0};

  /**
   * @brief Positions whose closing order was sized down, or not sent, because another order could
   *   close them too.
   *
   * A flatten cancels every listed order and then closes the position, but a same-side order can
   * fill while its cancellation is pending. So the close is sized to the holding minus what is
   * working on that side, or not sent at all when the listing cannot be trusted
   * (`closing_source_unverified_`). Makes `closing_complete()` false.
   */
  std::size_t closing_contended_{0};

  /**
   * @brief Positions whose closing order could not be sized: the holding rows do not add up, the
   *   closing quantity is not representable, or the same-side working quantity cannot be subtracted.
   *   Makes `closing_complete()` false.
   */
  std::size_t closing_unsizable_{0};

  /**
   * @brief Whether the closing pass could not trust its picture of the account. Makes
   *   `closing_complete()` false.
   *
   * Set if the order list was never read, the source publishes no working orders, no working-order
   * listing was read or it is older than the portfolio age limit, an order was placed (or sent with
   * an unknown outcome) after that listing, holdings were never read or are too old, or the listing
   * is newer than the holdings. Then a `FLATTEN` places no closing orders at all, while the
   * withdrawals of the orders it knows of are still sent.
   *
   * A verified source still lists only what the broker's listing shows: at Toss, an order of a type
   * the API does not list (such as an after-hours closing-price order placed in the app) is in no
   * listing, so a flatten can neither withdraw it nor size around it.
   */
  bool closing_source_unverified_{false};

  /**
   * @brief Tests whether every closing instruction the loop asked for was accepted, and the loop knew
   *   what to ask for.
   *
   * Meaningful only after a `WITHDRAW` or `FLATTEN` stop (`stopped_by_`): a run that ended otherwise,
   * or with `QUIESCE`, also returns `true`, having checked nothing. `true` does not mean flat or nothing resting: an accepted closing
   * order may not have filled, and an accepted cancellation may not have completed. Read the broker's
   * order list and holdings for the state.
   *
   * @return `false` if any closing instruction was rejected, refused, withheld, of unknown outcome,
   *   unavailable or unresolved, or any position was contended or unsizable, or the source was
   *   unverified.
   */
  bool closing_complete() const noexcept {
    return closing_rejected_ == 0 && closing_refused_ == 0 && closing_withheld_ == 0 &&
      closing_indeterminate_ == 0 && closing_unavailable_ == 0 && closing_unresolved_ == 0 &&
      closing_contended_ == 0 && closing_unsizable_ == 0 && !closing_source_unverified_;
  }
  /** @brief Instructions the strategy's `on_stop` asked for during a stop, refused as intended (the operator's orders go instead). */
  std::size_t stop_handler_refused_{0};
  /** @brief Instructions the strategy's `on_stop` got out during a stop; always zero. */
  std::size_t stop_handler_sent_{0};

  /** @brief Notes for the operator, such as an unrecognized control-file word or a missing policy. */
  std::vector<std::string> notes_;

  /**
   * @brief Returns how many polls of a kind were attempted.
   * @param kind The kind.
   * @return The count.
   */
  std::size_t attempted(PollKind kind) const noexcept {
    return attempted_[static_cast<int>(kind)];
  }

  /**
   * @brief Returns how many polls of a kind failed.
   * @param kind The kind.
   * @return The count.
   */
  std::size_t failed(PollKind kind) const noexcept { return failed_[static_cast<int>(kind)]; }
};

/**
 * @brief A source for tests: each poll returns a queued or default outcome, without any network.
 *
 * It installs a calendar only during `PollKind::CALENDAR` (see `set_calendar`), the rule every
 * source must follow.
 *
 * @tparam Runner The runner type.
 */
template <typename Runner>
class ManualSource {
public:
  /** @brief `true`: its outcomes stand for a real working-order listing. */
  static constexpr bool provides_working_orders = true;

  /**
   * @brief Queues the outcome of the next poll of a kind.
   * @param kind The kind.
   * @param outcome The outcome.
   */
  void queue(PollKind kind, PollOutcome outcome) {
    queued_[static_cast<int>(kind)].push_back(outcome);
  }

  /**
   * @brief Sets the outcome of every unqueued poll of a kind (`UNCHANGED` by default).
   * @param kind The kind.
   * @param outcome The outcome.
   */
  void set_default(PollKind kind, PollOutcome outcome) {
    defaults_[static_cast<int>(kind)] = outcome;
  }

  /**
   * @brief Sets the outcome of the order-list read (`UPDATED` by default).
   * @param outcome The outcome.
   */
  void set_order_list(PollOutcome outcome) { order_list_ = outcome; }

  /**
   * @brief Sets the buying-power currencies the source claims to ask about.
   * @param currencies The currencies.
   */
  void set_buying_power_currencies(std::vector<Currency> currencies) {
    currencies_ = std::move(currencies);
  }

  /**
   * @brief Sets the calendar a successful calendar poll installs, and makes that poll default to
   *   `UPDATED`.
   * @param calendar The calendar.
   */
  void set_calendar(TradingCalendar calendar) {
    calendar_ = std::move(calendar);
    defaults_[static_cast<int>(PollKind::CALENDAR)] = PollOutcome::UPDATED;
  }

  /**
   * @brief Returns the buying-power currencies.
   * @return The currencies.
   */
  const std::vector<Currency>& buying_power_currencies() const noexcept { return currencies_; }

  /**
   * @brief Returns how many times the order list was read.
   * @return The count.
   */
  std::size_t order_list_reads() const noexcept { return order_list_reads_; }

  /**
   * @brief Returns how many times a kind was polled.
   * @param kind The kind.
   * @return The count.
   */
  std::size_t polls(PollKind kind) const noexcept { return polls_[static_cast<int>(kind)]; }

  /**
   * @brief Adds a band a successful price-limits poll installs, and makes that poll default to
   *   `UPDATED`.
   * @param instrument The instrument.
   * @param limits The band.
   */
  void set_price_limits(InstrumentId instrument, PriceLimits limits) {
    price_limits_.emplace_back(std::move(instrument), std::move(limits));
    defaults_[static_cast<int>(PollKind::PRICE_LIMITS)] = PollOutcome::UPDATED;
  }

  /**
   * @brief Polls: returns the next queued outcome or the default, and on `UPDATED` installs the
   *   calendar or bands set for that kind.
   * @param kind The kind.
   * @param runner_instance The runner to install into.
   * @return The outcome.
   */
  PollOutcome poll(PollKind kind, Runner& runner_instance) {
    const int at = static_cast<int>(kind);
    ++polls_[at];
    auto outcome = defaults_[at];
    if (!queued_[at].empty()) {
      outcome = queued_[at].front();
      queued_[at].erase(queued_[at].begin());
    }
    // The one kind whose answer goes somewhere: the loop reads the calendar back out of the context,
    // so a source that did not put it there would leave every phase assertion in this header testing
    // an empty calendar.
    if (kind == PollKind::CALENDAR && calendar_ && outcome == PollOutcome::UPDATED) {
      runner_instance.context().set_calendar(*calendar_);
    }
    // The second kind whose answer goes somewhere -- onto the runner rather than into the context,
    // which is where reconciliation reads a band from. Installed on every successful poll, as a real
    // source does, so a test can age one by letting the loop's clock run.
    if (kind == PollKind::PRICE_LIMITS && outcome == PollOutcome::UPDATED) {
      for (const auto& band : price_limits_) {
        runner_instance.set_price_limits(band.first, band.second);
      }
    }
    return outcome;
  }

  /**
   * @brief Reads the order list: returns the configured outcome. The runner argument is ignored.
   * @return The outcome.
   */
  PollOutcome read_order_list(Runner&) {
    ++order_list_reads_;
    return order_list_;
  }

private:
  // Every kind's default outcome, filled rather than listed.
  //
  // It used to be a list with one entry per `PollKind`, and `PollOutcome::UPDATED` is the zero value
  // -- so adding an enumerator silently defaulted the new kind to "new data arrived" in every test
  // that had not set it. Filling cannot be one short.
  static std::array<PollOutcome, poll_kind_count> unchanged_defaults() {
    std::array<PollOutcome, poll_kind_count> filled{};
    filled.fill(PollOutcome::UNCHANGED);
    return filled;
  }

  std::vector<PollOutcome> queued_[poll_kind_count];
  std::array<PollOutcome, poll_kind_count> defaults_{unchanged_defaults()};
  PollOutcome order_list_{PollOutcome::UPDATED};
  std::vector<Currency> currencies_;
  std::optional<TradingCalendar> calendar_;
  std::vector<std::pair<InstrumentId, PriceLimits>> price_limits_;
  std::size_t polls_[poll_kind_count] = {};
  std::size_t order_list_reads_{0};
};

/**
 * @brief Runs a runner for a live session: polls a source on a schedule, gates placements, derives
 *   the market phase, and honours stop requests.
 *
 * Each iteration: check for a stop (a `StopSource` or the control file); make sure the broker's
 * order list has been read once (until it has, placements are held); derive and install the market
 * phase from the `SessionWindow` and the polled calendar; poll whatever is due, in `all_poll_kinds`
 * order; announce a phase change; retry retained targets.
 *
 * Placements are held through holds on the runner's `strategy::RiskGuard`: `ORDER_LIST_UNREAD`,
 * `PORTFOLIO_ABSENT`, `PORTFOLIO_STALE`, `BUYING_POWER_UNREAD` and `POLL_FAILING` are data holds,
 * under which named cancellations still go (see `strategy::halt_blocks`), so a stop can withdraw
 * known orders even when the order list could not be read. `STOPPING`, taken while a stop is
 * honoured and left in place after it, blocks cancellations too; the loop lifts it only for its own
 * closing instructions. The loop never takes or releases `OPERATOR` or `LOCAL_FAILURE`, which also
 * block cancellations.
 *
 * **State a holdings age policy** (`set_maximum_portfolio_age` or `allow_unbounded_portfolio_age`):
 * without one, every placement is held as `PORTFOLIO_STALE`.
 *
 * A phase the loop cannot establish is reported (`final_phase_` and a note), not held. A stop is
 * honoured with a closing pass (see `StopIntent`); `LoopReport::closing_complete()` says whether
 * everything asked for was accepted, which is not the same as being flat. The closing orders are
 * allowed in any phase but still face every other risk check (sellable quantity, ceilings).
 *
 * @note If you write a source: install the trading calendar during `PollKind::CALENDAR` only, and do
 *   not feed the runner from that poll.
 *
 * @tparam Runner The runner type (usually `runner::Runner`).
 * @tparam Source The source type (`live::SessionSource`, or `ManualSource` in tests).
 * @tparam Clock The clock (`SteadyClock`, or `ManualClock` in tests).
 */
template <typename Runner, typename Source, typename Clock = SteadyClock>
class PollingLoop {
public:
  /**
   * @brief Constructs a loop.
   * @param runner The runner; borrowed, and must outlive the loop.
   * @param source The source; borrowed, and must outlive the loop.
   * @param schedule How often to poll each kind.
   * @param clock The clock.
   */
  PollingLoop(Runner& runner, Source& source, PollSchedule schedule = {}, Clock clock = {})
  : runner_(&runner), source_(&source), schedule_(std::move(schedule)), clock_(std::move(clock)) {}

  /**
   * @brief Returns the clock.
   * @return The clock.
   */
  const Clock& clock() const noexcept { return clock_; }
  /**
   * @brief Returns the clock, for a test to advance.
   * @return The clock.
   */
  Clock& clock() noexcept { return clock_; }

  /**
   * @brief Returns the schedule.
   * @return The schedule.
   */
  const PollSchedule& schedule() const noexcept { return schedule_; }

private:
  // Every monotone instruction-outcome figure the runner holds, read at one instant.
  //
  // Twelve of them, read together rather than one call at a time, because they are differenced
  // against each other's snapshot: a set taken at two instants describes one interval, and twelve
  // separately captured values do not.
  struct InstructionOutcomes {
    std::size_t unbuildable_{0};
    std::size_t unconfirmed_timed_out_{0};
    std::size_t reconcile_withheld_{0};
    std::size_t guard_refused_{0};
    std::size_t binding_refused_{0};
    std::size_t broker_refused_{0};
    std::size_t indeterminate_{0};
    std::size_t cancels_rejected_{0};
    std::size_t cancels_refused_{0};
    std::size_t cancels_unresolved_{0};
    std::size_t cancels_indeterminate_{0};
    std::size_t plans_withheld_{0};
    std::size_t instructions_unavailable_{0};
  };

  // Reads them all from the runner.
  InstructionOutcomes instruction_outcomes() const {
    InstructionOutcomes at;
    at.unbuildable_ = runner_->orders_unbuildable();
    at.unconfirmed_timed_out_ = runner_->orders_unconfirmed_timed_out();
    at.reconcile_withheld_ = runner_->orders_reconcile_withheld();
    at.guard_refused_ = runner_->orders_guard_refused();
    at.binding_refused_ = runner_->orders_binding_refused();
    at.broker_refused_ = runner_->orders_broker_refused();
    at.indeterminate_ = runner_->orders_indeterminate();
    at.cancels_rejected_ = runner_->cancels_rejected();
    at.cancels_refused_ = runner_->cancels_refused();
    at.cancels_unresolved_ = runner_->cancels_unresolved();
    at.cancels_indeterminate_ = runner_->cancels_indeterminate();
    at.plans_withheld_ = runner_->plans_withheld();
    at.instructions_unavailable_ = runner_->instructions_unavailable();
    return at;
  }

  // Writes the difference between two snapshots onto `report`.
  static void note_instruction_outcomes(
    LoopReport& report,
    const InstructionOutcomes& before,
    const InstructionOutcomes& after
  ) {
    report.orders_unbuildable_ = after.unbuildable_ - before.unbuildable_;
    report.orders_unconfirmed_timed_out_ = after.unconfirmed_timed_out_ - before.unconfirmed_timed_out_;
    report.orders_reconcile_withheld_ = after.reconcile_withheld_ - before.reconcile_withheld_;
    report.orders_guard_refused_ = after.guard_refused_ - before.guard_refused_;
    report.orders_binding_refused_ = after.binding_refused_ - before.binding_refused_;
    report.orders_broker_refused_ = after.broker_refused_ - before.broker_refused_;
    report.orders_indeterminate_ = after.indeterminate_ - before.indeterminate_;
    report.cancels_rejected_ = after.cancels_rejected_ - before.cancels_rejected_;
    report.cancels_refused_ = after.cancels_refused_ - before.cancels_refused_;
    report.cancels_unresolved_ = after.cancels_unresolved_ - before.cancels_unresolved_;
    report.cancels_indeterminate_ = after.cancels_indeterminate_ - before.cancels_indeterminate_;
    report.plans_withheld_ = after.plans_withheld_ - before.plans_withheld_;
    report.instructions_unavailable_ = after.instructions_unavailable_ - before.instructions_unavailable_;
  }

public:

  /**
   * @brief Sets how old the holdings (and, for a runner that sends to a broker, the working-order
   *   listing) may be before placements are held (`PORTFOLIO_STALE`).
   *
   * One of this and `allow_unbounded_portfolio_age` is required: without either, every placement is
   * held as `PORTFOLIO_STALE` (and a note says so). The same age bounds a stop's closing pass (see
   * `LoopReport::closing_source_unverified_`).
   *
   * @param age The maximum age.
   */
  void set_maximum_portfolio_age(std::chrono::nanoseconds age) {
    maximum_portfolio_age_ = age;
    // Pushed down as the **policy**, not as a verdict. The runner re-evaluates it against whatever
    // snapshot is in hand when it settles, which is the only version that cannot describe a
    // portfolio that has since been replaced -- a source callback can feed, and settle, between this
    // loop's own checks.
    runner_->set_maximum_portfolio_age(age);
  }

  /**
   * @brief Accepts holdings of any age, instead of stating a maximum.
   *
   * It does not lift the hold for holdings never read, nor the one for a runner whose working-order
   * listing never arrives.
   *
   * @warning A listing is then never too old. If account polls keep being refused for the rate limit
   *   (which never exhausts the failure budget), the loop keeps placing against the last listing,
   *   and sent orders can stay counted as working indefinitely.
   */
  void allow_unbounded_portfolio_age() {
    allow_unbounded_portfolio_age_ = true;
    maximum_portfolio_age_.reset();
    runner_->set_maximum_portfolio_age(std::nullopt);
  }

  /**
   * @brief Sets how old a price band may be and still be applied; passed to the runner (see
   *   `Runner::set_maximum_price_limits_age`).
   * @param age The maximum age; `std::nullopt` withdraws the policy, so no band is applied.
   */
  void set_maximum_price_limits_age(std::optional<std::chrono::nanoseconds> age) {
    runner_->set_maximum_price_limits_age(age);
  }

  /** @brief Applies price bands of any age (see `Runner::allow_unbounded_price_limits_age`). */
  void allow_unbounded_price_limits_age() {
    runner_->allow_unbounded_price_limits_age();
  }

  /**
   * @brief Lets the run place orders before holdings have ever been read (for a broker without a
   *   holdings inquiry, or a run deliberately without that poll).
   *
   * It lifts the `PORTFOLIO_ABSENT` hold, but not an age limit on holdings that were read, and a plan
   * that would cancel something is still held back until holdings are read.
   */
  void allow_absent_portfolio() {
    allow_absent_portfolio_ = true;
    // Pushed down for the same reason the age policy is: this loop's hold is not in force at every
    // point an instruction can be settled, so the runner enforces the absence rule itself and an
    // opt-out that stopped at the hold would not actually opt out of anything.
    runner_->allow_absent_portfolio();
  }

  /**
   * @brief Sets the session window the market phase is derived from (KRX by default, as in a backtest).
   * @param window The window.
   */
  void set_session_window(strategy::SessionWindow window) {
    session_window_ = std::move(window);
  }

  /**
   * @brief Returns the session window.
   * @return The window.
   */
  const strategy::SessionWindow& session_window() const noexcept { return session_window_; }

  /**
   * @brief Sets how many consecutive failures of one poll kind stop the loop.
   * @param policy The policy.
   */
  void set_failure_policy(FailurePolicy policy) { failure_policy_ = std::move(policy); }

  /**
   * @brief Sets the control file an operator writes a stop intent into.
   * @param channel The channel.
   */
  void set_control_file(FileStopChannel channel) { control_ = std::move(channel); }

  /**
   * @brief Runs until a stop is requested, a poll kind exhausts its failure budget, or
   *   `max_iterations` iterations have run.
   *
   * On a stop it honours the intent (cancelling and closing as asked) and calls the strategy's
   * `on_stop` under the `STOPPING` hold.
   *
   * @param stop The stop source to poll (and that a signal handler may set).
   * @param max_iterations The most iterations to run; zero means unbounded.
   * @return The run's report.
   */
  LoopReport run(StopSource& stop, std::size_t max_iterations = 0) {
    LoopReport report;
    // Carried in rather than left false: the field says whether the order list has been read at
    // least once, and the loop remembers that across runs. A second `run` on the same loop had been
    // reporting it as unread, which reads as a startup gate that never opened.
    report.order_list_read_ = order_list_read_;
    // **Differenced around the run, not accumulated from the passes.** An order can leave the
    // process from inside a source's `poll`, which calls `Runner::feed` itself and discards the
    // report it gets back, so a per-pass figure would only ever see what a stop did. The runner's
    // counter is monotonic for exactly this reason, and reading it at both ends is how
    // `RiskGuard::total_sent` is already used around a callback. Taken per `run` so a second run on
    // the same loop reports its own sends rather than the first one's as well.
    const auto outside_phase_before = runner_->sends_outside_permitted_phase();
    // Differenced for the reason above: both are monotone on the runner, which outlives this call.
    const auto bands_withheld_before = runner_->price_bands_withheld();
    const auto unclamped_rejected_before = runner_->unclamped_orders_rejected();
    const auto unclamped_refused_before = runner_->unclamped_orders_refused();
    // **The polling path's own figures, taken before any stop is honoured.** `honour` runs the stop
    // handler and the closing pass through the same `Runner::settle`, so a snapshot taken at the end
    // of `run` would count a refused flatten here *and* in `closing_refused_` -- one failure reported
    // twice, under a contract that says this family describes the polling path.
    const auto outcomes_before = instruction_outcomes();
    auto outcomes_after = outcomes_before;
    // **The record travels with the counters, and for the same reason.** Read after `honour`, a
    // refusal from the *closing* pass would have replaced it while the count still described a
    // polling one -- a report naming an order none of its figures counted.
    std::optional<BrokerRefusal> refusal_at_boundary;
    std::optional<strategy::Rejection> unbuildable_at_boundary;
    // Differenced around the run for the same reason, and the reason is sharper here: the source
    // survives `run` returning, so an absolute figure would report the previous run's charges again.
    const auto charged_before = [&]() {
      if constexpr (source_states_charged_costs<Source>::value) {
        return source_->charged_costs();
      } else {
        return ChargedCosts{};
      }
    }();
    for (auto& due : next_due_) {
      due.reset();
    }
    for (auto& count : consecutive_failures_) {
      count = 0;
    }
    warmup_discarded_at_start_ = warmup_discarded();
    if constexpr (runner_counts_kept_drops<Runner>::value) {
      kept_dropped_at_start_ = runner_->kept_intents_dropped();
    }

    while (max_iterations == 0 || report.iterations_ < max_iterations) {
      const auto iteration = step(stop, report);
      ++report.iterations_;
      if (iteration.gate_) {
        ++report.blocked_;
      }
      report.final_gate_ = iteration.gate_;
      report.final_phase_ = iteration.phase_;
      if (iteration.phase_changed_) {
        ++report.phase_transitions_;
      }
      last_ = iteration;

      if (report.abandoned_) {
        outcomes_after = instruction_outcomes();
        refusal_at_boundary = runner_->last_broker_refusal();
        unbuildable_at_boundary = runner_->last_unbuildable();
        // As for a stop: retained work lives for one run. A run resumed after an outage of unknown
        // length must not place what was decided before it.
        runner_->clear_retained_targets();
        break;
      }
      if (const auto intent = stop.intent()) {
        // Sampled here, before the stop runs anything, for the reason stated where this began.
        outcomes_after = instruction_outcomes();
        refusal_at_boundary = runner_->last_broker_refusal();
        unbuildable_at_boundary = runner_->last_unbuildable();
        honour(*intent, report);
        report.stopped_by_ = intent;
        break;
      }
      clock_.wait_for(until_next(iteration.at_));
    }

    // Deliberately **not** released here. A run that ended because an operator asked it to, or
    // because a poll kept failing, must leave the runner unable to send: releasing the halt on the
    // way out would reopen the gate at exactly the moment the loop stopped watching it. A caller who
    // genuinely wants to trade again resumes the guard themselves, which is a deliberate act.
    //
    // A healthy run that simply exhausted `max_iterations` never halted anything, so there is
    // nothing to leave behind in that case.
    report.sends_outside_permitted_phase_ =
      runner_->sends_outside_permitted_phase() - outside_phase_before;
    report.price_bands_withheld_ = runner_->price_bands_withheld() - bands_withheld_before;
    report.unclamped_orders_rejected_ =
      runner_->unclamped_orders_rejected() - unclamped_rejected_before;
    report.unclamped_orders_refused_ =
      runner_->unclamped_orders_refused() - unclamped_refused_before;
    // Taken at the loop's exit for a run nothing stopped, and at the stop for one something did.
    if (!report.stopped_by_ && !report.abandoned_) {
      outcomes_after = instruction_outcomes();
      refusal_at_boundary = runner_->last_broker_refusal();
      unbuildable_at_boundary = runner_->last_unbuildable();
    }
    note_instruction_outcomes(report, outcomes_before, outcomes_after);
    // **Carried only when this run's polling saw one**, which the count already decides: any refusal
    // during this run overwrote whatever the runner held before it, so a non-zero count means the
    // record is this run's. Comparing the records instead would need an equality `BrokerRefusal` has
    // no reason to have.
    if (report.orders_broker_refused_ != 0) {
      report.last_broker_refusal_ = refusal_at_boundary;
    }
    // The same rule for the reconciliation's own refusals.
    if (report.orders_unbuildable_ != 0) {
      report.last_unbuildable_ = unbuildable_at_boundary;
    }
    // **Said only when a band was actually held back**, not merely because no policy was stated. A
    // run that registers no band and polls none has nothing to decide, and a note telling it to
    // state a policy would be advice about a mechanism it is not using. Once per loop, like the
    // portfolio's own note, because a run that ignores it does not need it every iteration.
    //
    // **Asked of the runner rather than remembered here.** This loop borrows a mutable runner and
    // does not own its policy: a maximum stated directly on the runner before this loop existed
    // would have read as "nobody stated one", and a withdrawal made directly on the runner would
    // have been suppressed by this loop's memory of its own wrapper being called. One policy, one
    // place that answers for it.
    if (report.price_bands_withheld_ > 0 && !runner_->price_limits_age_policy_stated() &&
        !said_no_band_age_policy_) {
      said_no_band_age_policy_ = true;
      report.notes_.emplace_back(
        "no price band age policy: call set_maximum_price_limits_age or "
        "allow_unbounded_price_limits_age"
      );
    }
    if constexpr (source_states_charged_costs<Source>::value) {
      report.charged_costs_ = source_->charged_costs().since(charged_before);
    }
    note_sellable_convention(report);
    note_unestablished_phase(report);
    note_on_fill_never_fires(report);
    note_listing_outpaces_holdings(report);
    note_warmup_discarded(report);
    note_kept_intents_dropped(report);
    return report;
  }

  /**
   * @brief Returns what the last iteration did.
   * @return The iteration report.
   */
  const IterationReport& last() const noexcept { return last_; }

private:
  // Puts the sellable-convention probe's verdict in the report, when it reached one.
  //
  // The probe answers a question neither broker's documentation does -- whether the sellable figure
  // already excludes resting sells -- and it answers it as a by-product of ordinary polling. An
  // operator should not have to write code to read that, so a run that learned something says so,
  // with the counts behind it because one observation is evidence rather than proof.
  //
  // Silent when nothing was observed, which is most runs: a note saying "undetermined" on every run
  // that never sold anything is noise, and noise is how a report stops being read.
  void note_sellable_convention(LoopReport& report) const {
    const auto evidence = runner_->sellable_convention().overall();
    if (evidence.total() == 0) {
      return;
    }
    report.notes_.push_back(
      "broker's sellable figure appears " +
      std::string(strategy::sellable_convention_name(evidence.verdict())) + " (" +
      std::to_string(evidence.net_) + " net, " + std::to_string(evidence.gross_) + " gross)"
    );
  }

  // Returns how many instructions the runner has discarded as history, where it counts them.
  std::size_t warmup_discarded() const {
    if constexpr (runner_counts_warmup<Runner>::value) {
      return runner_->warmup_instructions_discarded();
    } else {
      return 0;
    }
  }

  // Says, once per run, how many instructions were decided on bars fed as history and not sent.
  //
  // Only the newest bar of each instrument's read may send (`Runner::feed_warmup`), so a cold start's
  // history and a backlog after an outage are shown to the strategy and nothing decided on them goes
  // out. Worth a line because it is also what a bar cadence slower than the bars' interval costs: a
  // poll bringing five minute bars acts on the newest one only.
  void note_warmup_discarded(LoopReport& report) const {
    const auto discarded = warmup_discarded() - warmup_discarded_at_start_;
    if (discarded == 0) {
      return;
    }
    report.notes_.push_back(
      std::to_string(discarded) + " instruction(s) decided on bars fed as history were not sent: "
      "only the newest bar of each instrument's read may send (poll bars no slower than their interval)"
    );
  }

  // Says, once per run, how many order intents kept for the phase were dropped at release.
  void note_kept_intents_dropped(LoopReport& report) const {
    if constexpr (runner_counts_kept_drops<Runner>::value) {
      const auto dropped = runner_->kept_intents_dropped() - kept_dropped_at_start_;
      if (dropped != 0) {
        report.notes_.push_back(
          std::to_string(dropped) + " order intent(s) kept for the market phase were not sent at the "
          "open: the instrument's holding had changed since they were decided, or a stop now rested"
        );
      }
    } else {
      static_cast<void>(report);
    }
  }

  // Says, once per run, that the strategy's `on_fill` never fires on this runner.
  //
  // See `Runner::on_fill_never_fires`. On every run rather than only the first, because a report is
  // read on its own, and a note that appeared only in a run nobody kept would be no note at all.
  void note_on_fill_never_fires(LoopReport& report) const {
    if constexpr (Runner::on_fill_never_fires) {
      report.notes_.push_back(
        "strategy defines on_fill; it never fires on this runner (fills are not observed live: "
        "read positions from holdings, see writing-a-strategy.md)"
      );
    } else {
      static_cast<void>(report);
    }
  }

  // Says, once per run, that working orders are polled more often than holdings.
  //
  // Not refused: it is a legitimate choice for a strategy that mostly watches its orders. But every
  // plan made between a listing and the next holdings read is withheld (`PollSchedule::working_orders_`
  // says why), and an operator reading a report full of `portfolio_superseded` should find the cause
  // stated beside it.
  void note_listing_outpaces_holdings(LoopReport& report) const {
    const auto& listing = schedule_.working_orders_;
    const auto& holdings = schedule_.holdings_;
    if (listing && holdings && *listing < *holdings) {
      report.notes_.push_back(
        "working orders are polled more often than holdings; plans made between a listing and the "
        "next holdings read are withheld as portfolio_superseded"
      );
    }
  }

  // Runs one iteration: poll what is due, then let the runner act if the gate is open.
  IterationReport step(StopSource& stop, LoopReport& report) {
    IterationReport iteration;
    iteration.at_ = clock_.now();

    // Read before polling, so an operator's stop is honoured without first spending requests on a
    // broker the loop is about to walk away from.
    check_control_file(stop, report);
    // A stop discards what was retained for a better account read: nothing decided before it is to be
    // replayed after it.
    if (stop.requested()) {
      runner_->clear_retained_targets();
    }

    // **Installed before anything can send, and announced only after the polls.** The two halves are
    // separate on purpose and neither is optional.
    //
    // A source's `poll` calls `Runner::feed`, which settles, so an order can leave the process from
    // inside a poll -- the same fact the gate below is computed early for. If the phase were installed
    // after the polls, the first bar arriving after the close would be settled while the context still
    // said `OPEN`, and an opening order would pass a phase limit that exists to refuse it. Once per
    // session, on the close.
    //
    // Announcing the change is the opposite case: `on_phase` may place orders, and it should see this
    // iteration's polls rather than the previous one's -- fresh prices, a fresh portfolio, and a gate
    // re-evaluated against them. That is what `backtest/engine.hpp:283-306` does for the replay, with
    // the same argument: a handler deciding on a mark from one moment beside a price from another is
    // not looking at a fresher picture, it is looking at an incoherent one.
    install_phase(iteration);

    // **Before anything this loop calls can feed, because several of them can.** The runner measures
    // a snapshot's age against the later of this and its own decision instant, and its decision
    // instant is a bar's period end -- which on a daily bar is yesterday's close. Set here, a plan
    // settled from inside `read_order_list` or a poll is aged against the clock rather than against
    // the event that triggered it.
    runner_->set_observed_at(iteration.at_);
    // Stated here too, and for the same reason: the runner enforces the absence rule at the moment
    // an instruction is decided, which is the only place that is inside every callback this loop
    // makes. A run that opted out says so, and the runner then places as it did before.
    runner_->require_known_portfolio(!allow_absent_portfolio_);

    // **Established before any poll runs, because a poll is what feeds the runner.** A source's
    // `poll` calls `Runner::feed` itself -- `live::SessionSource::poll_bars` does, per instrument --
    // and `feed` settles, so an order can leave the process from inside a poll. Computing the gate
    // afterwards would mean the startup gate and the staleness gate were installed one poll too
    // late, and on the very first iteration they would not be installed at all: a bar arriving
    // before the first order-list read could place an order against a position a predecessor may
    // have left, which is the exact hazard the startup gate exists for.
    hold(gate_for(stop, iteration.at_, report, /*may_read=*/true));

    for (const auto kind : all_poll_kinds) {
      if (!is_due(kind, iteration.at_)) {
        continue;
      }
      // **Re-established immediately before every poll**, because a poll is a point at which an order
      // can leave the process and anything that ran before it may have replaced the calendar the phase
      // was derived from. Three things can have: a previous poll in this same loop, the calendar poll
      // in particular since it is polled first, and `gate_for`'s out-of-band `read_order_list` above.
      //
      // Before rather than after, which is the correction: "after every poll" closed the first two and
      // left the third, because the pre-loop install runs *before* `gate_for` and nothing re-derived
      // the phase between that call and the first poll. Placing it here makes the rule positional
      // instead of enumerated -- every send site is preceded by an install, and no list of which polls
      // can change what has to stay correct.
      //
      // The cost is a date comparison per due poll.
      install_phase(iteration);
      // **And the gate, for the same reason.** The polls before this one may have read the account: on
      // a cold start the hold for an account never read was installed before the polls, and without
      // re-evaluating it here it would still stand when the bars read after this iteration's holdings
      // are fed -- withholding the first decision against an account that has, by then, been read.
      // `may_read` is false: the order list is attempted once per iteration, above.
      hold(gate_for(stop, iteration.at_, report, /*may_read=*/false));
      const auto watch = watch_phase(iteration);
      // **Sampled before the poll, not after it.** A poll can send: `Runner::feed` settles, and a
      // working-order poll now feeds `on_order_update` as well as the bar and quote polls feeding
      // their handlers. An order a handler places during the working-order poll was sent *after*
      // the listing was fetched and cannot be in it -- so counting placements afterwards would
      // stamp that listing as though it covered the order, and `closing_source_verified` would
      // then approve a flatten against a picture missing a resting order. The count taken here is
      // the one that was true when the listing was read.
      const auto placements_before_poll = runner_->placements_at_broker();
      const auto outcome = source_->poll(kind, *runner_);
      note_revised_phase(std::string(poll_kind_name(kind)) + " poll", watch, report);
      // `UNCHANGED` counts, because a listing that is identical to the last one is still a listing
      // -- but only on a source that publishes them at all, which `note_working_orders_read`
      // enforces: a broker without the inquiry answers `UNCHANGED` without asking anybody.
      if (kind == PollKind::WORKING_ORDERS &&
          (outcome == PollOutcome::UPDATED || outcome == PollOutcome::UNCHANGED)) {
        note_working_orders_read(iteration.at_, placements_before_poll);
      }
      ++report.attempted_[static_cast<int>(kind)];
      iteration.polls_.emplace_back(kind, outcome);
      schedule_next(kind, iteration.at_);
      note_outcome(kind, outcome, report);
      if (outcome == PollOutcome::UPDATED) {
        iteration.fed_ = true;
      }
      if (report.abandoned_) {
        // Recorded before returning, so the report does not claim sending was permitted while the
        // guard `note_outcome` just halted says otherwise.
        iteration.gate_ = runner_->guard().blocking_reason();
        return iteration;
      }
    }

    // Observed after the polls, not before: a poll can refresh the listing without a bar arriving,
    // and that is precisely when an order of ours begins showing as resting. Observing first would
    // read every iteration's portfolio one iteration late. A repeat observation of unchanged state
    // concludes nothing, so a quiet iteration costs a scan and teaches the probe nothing.
    runner_->observe_sellable_convention();

    // Re-evaluated after the polls so the report and the next iteration reflect what those polls
    // changed -- a holdings refresh should lift the staleness hold now, not one iteration later.
    // The order list is deliberately not attempted a second time: one attempt per iteration, and
    // the gate simply stands until the next one.
    // The last send site: the announcement below settles what `on_phase` asks for, and the final poll
    // may have replaced the calendar after its own pre-poll install.
    install_phase(iteration);
    hold(gate_for(stop, iteration.at_, report, /*may_read=*/false));

    // Announced last, so the handler runs on everything this iteration established. A change is
    // announced once, not every iteration it persists for.
    if (announced_ != iteration.phase_) {
      // **The clock moves with the phase, and only forwards.** `Runner::feed(MarketPhase)` does not
      // advance `Context::now_`; only a bar or a quote does, from a broker's stamp. So a daily-bar run
      // would hand `on_phase` an instant hours old, and `RiskGuard` files a send at
      // `GuardInputs::now_` and counts sends within `rate_window_` of it -- the phase order and the
      // next bar's order would be filed at what the guard thinks are two unrelated moments, and two
      // orders would pass a one-order window. `backtest/engine.hpp:296-302` documents the same hazard
      // for the replay.
      //
      // Forwards only: a broker stamp ahead of this machine's clock must not be walked backwards,
      // because a handler deciding at an instant *earlier* than the bar it has already been shown is
      // the same incoherence with the sign flipped. The skew is the broker's and is not this loop's
      // to quietly correct.
      //
      // **What this does not fix**, stated because it is easy to read more into it: a bar fed earlier
      // in this same iteration set `now_` to its own period end, which on a daily bar is yesterday's
      // close. So that order is filed at the bar's instant and this one at the wall clock, and a rate
      // window narrower than the gap sees them as unrelated. That is inherent to a bar's decision
      // instant being its period end -- which the replay depends on -- and is not introduced here:
      // before this, the phase order did not exist to be filed at all.
      if (runner_->context().now() < iteration.at_) {
        runner_->context().set_now(iteration.at_);
      }
      announced_ = iteration.phase_;
      iteration.phase_changed_ = true;
      // Watched for the same reason a poll is: `Context::set_calendar` is public, so an `on_phase`
      // handler can revise the calendar and submit in one call, and `settle` runs before this returns.
      const auto watch = watch_phase(iteration);
      runner_->feed(iteration.phase_);
      note_revised_phase("on_phase handler", watch, report);
    }

    // **Last of all the iteration's decisions**, so every handler that could restate the instrument --
    // the bars, the quotes, the listing's updates, and `on_phase` just above -- has spoken first, and
    // its instruction has replaced the retained target; a retry before `on_phase` could send an entry
    // the phase change was about to countermand. And after every account poll and the gate they
    // refreshed, so what is retried is sized against this iteration's holdings, listing and budget,
    // under this iteration's holds -- inside the holdings poll it ran before the gate lifted and before
    // buying power was read.
    runner_->retry_retained_targets();

    // Reported from the guard rather than from what the loop just computed, so the report names the
    // most fundamental hold in force whoever owns it -- an operator's halt, or a journal that
    // failed, outranks anything the loop decided.
    iteration.gate_ = runner_->guard().blocking_reason();
    return iteration;
  }

  // What the phase and the send count were before the loop handed control to somebody else.
  struct PhaseWatch {
    std::chrono::system_clock::time_point at_{};
    strategy::MarketPhase phase_{strategy::MarketPhase::UNKNOWN};
    std::size_t sends_{0};
  };

  // Records the phase and the send count, for comparison after a call into user code.
  PhaseWatch watch_phase(const IterationReport& iteration) const {
    return PhaseWatch{iteration.at_, iteration.phase_, runner_->guard().total_sent()};
  }

  // Counts and names a call that sent an order and left the phase changed behind it.
  //
  // **The one hazard here the loop cannot prevent and can only witness.** It re-derives the phase before
  // every call it makes that could send, but nothing of its runs *inside* such a call: a
  // source that replaces the calendar and then feeds, or an `on_phase` handler that does the same before
  // submitting, settles an order against the phase as it stood when the call began. So the loop looks
  // afterwards, which turns an invisible violation into a line in the report -- the same choice
  // `closing_source_unverified_` makes about a listing it could not trust.
  //
  // **Both halves are required, and each one removes a wrong answer.** The phase must have moved, or an
  // ordinary send would be reported. Something must have been sent, or an ordinary calendar poll would be
  // reported on every iteration -- which is why this no longer excludes the calendar poll by kind. That
  // exclusion was a false-positive workaround and it hid this case: a source obeying
  // the letter of the contract, writing the calendar in the calendar poll and *then* feeding.
  //
  // `total_sent()` rather than the guard's rate-window deque, because that one prunes and can fall, so
  // it cannot be differenced across a call.
  void note_revised_phase(const std::string& what, const PhaseWatch& before, LoopReport& report) {
    if (runner_->guard().total_sent() == before.sends_) {
      return;
    }
    if (phase_at(before.at_) == before.phase_) {
      return;
    }
    ++report.sends_under_a_revised_phase_;
    if (report.sends_under_a_revised_phase_ == 1) {
      // Once per run, not once per call: a strategy doing this on every bar would otherwise bury every
      // other note under thousands of copies of this one.
      // **Worded to exactly what was observed**, which is co-occurrence and not causation. Two cases
      // the stronger claim would have got wrong: a call that sent *first* and revised the calendar
      // afterwards, where no order used the stale answer; and a call that only cancelled, since
      // `Runner::send_cancel` never consults the phase at all. Both still break the writer contract and
      // are worth reporting, so the note states the facts and names the check rather than the verdict.
      report.notes_.push_back(
        "the " + what + " changed the market phase and sent a request in the same call; check whether "
        "that request used the earlier phase. Only the calendar poll may write the trading calendar, "
        "and it must not send");
    }
  }

  // Computes the phase for this iteration and installs it, so nothing can send under a stale one.
  //
  // **Called immediately before every point at which this loop hands control to the source**: once
  // before `gate_for`, which performs an out-of-band order-list read; once before each due poll; and
  // once before the phase announcement. So the invariant is exactly this, and no stronger:
  // **whenever the loop calls into the source, `Context::phase_` is derived from the calendar as it
  // stood at that call.**
  //
  // **What that deliberately does not promise**, because it cannot: a source is free to replace the
  // calendar and then feed the runner inside a single `poll`, and no loop code runs in between, so
  // that order would settle against the phase as it stood when the poll began. `PollKind::CALENDAR`
  // documents the contract that rules it out, `SessionSource` satisfies it -- it writes the calendar in
  // the calendar poll and nowhere else -- and `note_revised_phase` reports a source that
  // breaks it, because a violation nobody is told about is one nobody fixes. The stronger at-send-site
  // invariant does not hold.
  //
  // The instant is the iteration's own `at_` throughout, never re-read from the clock, so a poll that
  // took a second does not move the session boundary mid-iteration.
  void install_phase(IterationReport& iteration) {
    iteration.phase_ = phase_at(iteration.at_);
    // Recorded here rather than derived later, and it outlives a `run` for the same reason `announced_`
    // does: "this loop has never known the phase" is a claim about the loop, not about one call.
    if (iteration.phase_ != strategy::MarketPhase::UNKNOWN) {
      phase_ever_known_ = true;
    }
    runner_->context().set_phase(iteration.phase_);
  }

  // Returns the phase at `at`, from the configured window and whatever calendar has arrived.
  //
  // The composition `ReplayClock` already performs, with a polled calendar in place of a replay's
  // per-bar answer. An uncovered date is `UNKNOWN` and never "closed": `TradingCalendar` draws that
  // distinction because assuming closed stops a strategy on a day that is in fact open, and assuming
  // open lets one order into a holiday.
  //
  // The date is the *market's*, not this process's. `SessionWindow` owns the offset and derives both
  // answers from it, so the day the calendar is asked about and the day the session boundaries are
  // computed for cannot disagree.
  strategy::MarketPhase phase_at(std::chrono::system_clock::time_point at) const {
    const auto& calendar = runner_->context().calendar();
    return session_window_.phase(at, calendar.is_trading_day(session_window_.local_date(at)));
  }

  // The clause naming unreadable calendar rows, or nothing when there were none.
  //
  // A row whose date the broker sent unreadably is dropped, so the day it was for is uncovered and
  // the note above would otherwise send an operator to check a span or a broker that is fine. The
  // count says the broker *did* answer, and what it answered could not be read.
  std::string unreadable_calendar_rows() const {
    const auto unreadable = runner_->context().calendar().unreadable_days_;
    if (unreadable == 0) {
      return {};
    }
    return "; the broker sent " + std::to_string(unreadable) + " calendar row" +
      (unreadable == 1 ? "" : "s") + " whose date could not be read";
  }

  // Says so when a run never established the phase, and names which of the three reasons applies.
  //
  // Silent otherwise, like `note_sellable_convention`: a line on every healthy run is how a report
  // stops being read. But a run that placed nothing because the phase was never known must not look
  // like a quiet market -- and this is **not** a hold on the guard, deliberately. A hold would also
  // stop cancellations, which `Runner::send_cancel` refuses on any blocking reason while never
  // consulting the phase at all; taking one because a reference-data poll failed would make an
  // unknown phase more restrictive than a closed one, and a withdrawal only ever reduces exposure.
  // `RiskGuard` already refuses every placement that opens or increases a position with
  // `PHASE_NOT_PERMITTED`, and permits one that closes a held position -- which is exactly what a
  // hold would wrongly stop, for the same reason it would wrongly stop a cancellation.
  void note_unestablished_phase(LoopReport& report) const {
    if (report.iterations_ == 0 || report.final_phase_ != strategy::MarketPhase::UNKNOWN) {
      return;
    }
    // **"Never" is a claim, so it is tracked rather than derived.** A loop can establish the phase and
    // then lose it -- the local date rolls past what the calendar covers, or the broker revises it --
    // and telling an operator it was never established would send them looking for a configuration
    // fault that is not there.
    //
    // This used to read `report.phase_transitions_ > 0`, which was wrong: `announced_`
    // outlives a `run` while the report's counter restarts at zero, so a run with nothing left to
    // announce reports no transitions and would claim the phase had never been known -- of a loop that
    // spent its first run open. The verdict was right in the case that was tested and the reasoning was
    // wrong, which is the worse of the two.
    const bool ever_known = phase_ever_known_;
    // Three reasons, not four. "The poll never came due" was here and is unreachable: `run` resets
    // `next_due_`, so a configured cadence makes the calendar due on the first iteration of every
    // run. A branch describing a state that cannot happen is worse than no branch, because a reader
    // who sees it believes it can.
    std::string reason;
    if (!schedule_.calendar_) {
      reason = "no calendar cadence is configured";
    } else if (report.failed(PollKind::CALENDAR) == report.attempted(PollKind::CALENDAR)) {
      reason = "every calendar poll failed";
    } else if (runner_->context().calendar().days_.empty()) {
      // What the loop can actually tell: a poll reported success and no calendar arrived. It does
      // **not** claim to know why, because it cannot distinguish a broker that publishes none -- where
      // the request is not even compiled -- from a source that fetched one and installed nothing.
      reason = "a calendar poll succeeded but no calendar arrived (this broker may publish none)";
      reason += unreadable_calendar_rows();
    } else {
      // Named, not merely called uncovered. "The calendar does not cover today" sends a reader to
      // check a span they cannot see; the date is the one thing that turns this note into an action.
      const auto date = session_window_.local_date(last_.at_);
      reason = "the calendar does not cover " + std::to_string(date.year_) + "-" +
        std::to_string(date.month_) + "-" + std::to_string(date.day_);
      reason += unreadable_calendar_rows();
    }
    report.notes_.push_back(
      std::string(ever_known ?
        "the market phase stopped being established (" :
        "the market phase was never established (") +
      reason + "); a guard permitting only known phases refused every placement opening a position" +
      (ever_known ? " after that" : ""));
  }

  // Attempts the startup order-list read once, and returns whether it succeeded.
  //
  // The startup gate's read and a pending stop's both come here, so the two cannot drift apart.
  bool read_order_list_once(std::chrono::system_clock::time_point at, LoopReport& report) {
    // Watched like a poll, because it is the same kind of thing: a call into the source, from which a
    // source that revised the calendar and sent would reach the identical hazard through a different
    // door. The phase in force here is the one `step` installed immediately before calling this.
    const auto watch = PhaseWatch{at, phase_at(at), runner_->guard().total_sent()};
    // As in `step`, and here the startup read dispatches nothing at all -- but the count is taken
    // before the call for the same reason, so that the rule holds positionally rather than by an
    // argument about which reads can send.
    const auto placements_before_read = runner_->placements_at_broker();
    const auto outcome = source_->read_order_list(*runner_);
    note_revised_phase("order-list read", watch, report);
    if (outcome != PollOutcome::UPDATED && outcome != PollOutcome::UNCHANGED) {
      return false;
    }
    order_list_read_ = true;
    report.order_list_read_ = true;
    // The startup read is a working-order read *on a source that has the inquiry*. On one that
    // does not, it reads order history for the journal and never populates `working_`, so
    // `note_working_orders_read` deliberately does nothing and a closing pass stays unverified.
    note_working_orders_read(at, placements_before_read);
    return true;
  }

  // Returns which of the loop's *own* conditions blocks sending, reading the order list if due.
  //
  // Only conditions the loop computes, because a hold is released by whoever took it. `OPERATOR`
  // belongs to a person and `LOCAL_FAILURE` to the runner; a loop that took either would be a
  // second holder of someone else's hold -- the defect `HaltReason` exists to prevent,
  // reintroduced one layer up.
  //
  // **Always evaluated, even while someone else's hold stands.** Short-circuiting on a foreign hold
  // would leave the loop's own hold un-maintained underneath it, so the moment an operator resumed,
  // the guard would be wide open against a portfolio nobody had rechecked.
  //
  // Called twice per iteration -- before the polls, which is what actually protects anything, and
  // after them so the report is current. `may_read` is false on the second call so the order-list
  // read is attempted at most once per iteration.
  std::optional<strategy::HaltReason> gate_for(
    const StopSource& stop,
    std::chrono::system_clock::time_point at,
    LoopReport& report,
    bool may_read
  ) {
    if (stop.requested()) {
      // **A withdraw or a flatten reads the broker's order list first, if it never has.** Each builds
      // its instructions from the picture, and a stop pending before the first iteration -- a process
      // started with `flatten` in its control file -- used to return here before the startup read, so
      // its closing pass was unverified and a flatten closed nothing, for any position. That also left
      // no way to flatten a position behind a resting stop: the in-process flatten withdraws the stop
      // and, rightly, sends nothing beside it, and the restart that should finish the job could not.
      // The read is the startup gate's own, under its conditions; a quiesce sends nothing and needs no
      // picture, so it does not spend the request.
      //
      // **`STOPPING` on every path, the failed read included.** `ORDER_LIST_UNREAD` lets reducing
      // orders through (`halt_blocks`), and nothing but the stop's own may go out once one is pending.
      // Held before the read as well, because the source is a template parameter and may feed.
      const auto intent = stop.intent();
      if (intent && *intent != StopIntent::QUIESCE && !order_list_read_ && may_read &&
          !runner_->guard().halted(strategy::HaltReason::LOCAL_FAILURE)) {
        hold(strategy::HaltReason::STOPPING);
        read_order_list_once(at, report);
      }
      return strategy::HaltReason::STOPPING;
    }
    if (!order_list_read_) {
      // Not attempted when the caller says not to, and not attempted at all while the runner's
      // journal is broken: a runner that cannot record what it does must not be sent looking for
      // more work to fail at. Either way the gate stands, which is the safe answer.
      if (!may_read || runner_->guard().halted(strategy::HaltReason::LOCAL_FAILURE)) {
        return strategy::HaltReason::ORDER_LIST_UNREAD;
      }
      if (!read_order_list_once(at, report)) {
        return strategy::HaltReason::ORDER_LIST_UNREAD;
      }
    }
    // Measured from the snapshot's own `as_of_` rather than from when the poll returned, because
    // that is when the broker says the positions were true. A poll that succeeded at noon carrying
    // a snapshot the broker took at 11:00 is an hour stale, and the poll's own timestamp would call
    // it fresh.
    const auto as_of = runner_->context().portfolio().as_of_;
    if (as_of == std::chrono::system_clock::time_point{}) {
      // A default-constructed `Portfolio` carries the epoch, and no broker ever reports a snapshot
      // taken in 1970, so the epoch means **never populated** -- which is not the same as old, and
      // is the more dangerous of the two: an empty portfolio is the *permissive* answer to every
      // question the send path asks it, so a run that has never read the account sizes an entry as
      // though it were flat.
      //
      // **No longer inside the age check.** It was, and hoisting it was not a one-line change:
      // the hold would reach cancellations, and an order that cannot be withdrawn keeps resting and
      // can fill. That objection is answered rather than avoided -- `strategy::halt_blocks` lets a
      // named withdrawal through a hold that only means this runner is uninformed, and
      // `Runner::plan_withhold_reason` stops the *derived* cancellation that objection overlooked.
      //
      // The opt-out reaches the age comparison below as well, not just this hold. The epoch is 1970
      // and therefore older than any bound an operator would state, so an absence opt-out that did
      // not skip the comparison would simply move the refusal to `PORTFOLIO_STALE`.
      if (!allow_absent_portfolio_) {
        return strategy::HaltReason::PORTFOLIO_ABSENT;
      }
      // Opted out, so the age comparison is skipped rather than fallen through to -- and the checks
      // after this one still run, because an absent portfolio says nothing about buying power.
    }

    // **A run must state how old a snapshot may be, or state that it accepts any age.**
    //
    // This was optional, and optional meant that a run which never thought about holdings sent
    // orders sized against whatever the portfolio happened to contain -- including nothing at all.
    // Fail-open by default was the defect. The library states no default
    // duration of its own, because any constant it picked would be a risk policy it has no basis
    // for: how stale a position may be before it must not be sized against is the operator's
    // judgement about their own market and cadence, not this library's.
    // **Checked after absence, not before it.** Both can be true at once, and "you have never read
    // this account" is the more useful of the two to be told: the missing policy is a configuration
    // fact the note below states in full, while absence is a fact about the data every ceiling and
    // every target subtraction is about to be measured against.
    if (!maximum_portfolio_age_ && !allow_unbounded_portfolio_age_) {
      if (!said_no_age_policy_) {
        said_no_age_policy_ = true;
        report.notes_.emplace_back(
          "no portfolio age policy: call set_maximum_portfolio_age or allow_unbounded_portfolio_age"
        );
      }
      return strategy::HaltReason::PORTFOLIO_STALE;
    }
    if (maximum_portfolio_age_ && as_of != std::chrono::system_clock::time_point{} &&
        at - as_of > *maximum_portfolio_age_) {
      return strategy::HaltReason::PORTFOLIO_STALE;
    }
    // **A runner that keeps its orders until the broker accounts for them needs the listing.** A holdings
    // poll refreshes positions and keeps `working_`; only a working-order poll replaces it. A runner
    // that sends to a broker keeps each order it sent in `working_` until that listing arrives
    // (`Runner::persist_reservation`), so with no listing -- none published, none polled, or one
    // older than the policy -- an order that expired or was refused would stay "working" for ever,
    // a target would stop being chased, and a filled one would be cancelled again on every pass.
    // Held, with the missing piece named once, whatever the portfolio age policy: opting out of a
    // holdings age is not opting out of a listing.
    if constexpr (runner_keeps_sent_orders<Runner>::value) {
      if (const auto missing = listing_missing(at)) {
        if ((said_listing_ & missing->first) == 0) {
          said_listing_ |= missing->first;
          report.notes_.emplace_back(missing->second);
        }
        return strategy::HaltReason::PORTFOLIO_STALE;
      }
    }
    if (runner_->guard().limits().enforce_buying_power_) {
      // Checked only when the caller asked for the limit. A loop with no buying-power cadence and
      // no check configured is an ordinary loop, and must not be held for a reading nobody wants.
      if (const auto unread = buying_power_unread(at)) {
        return *unread;
      }
    }
    return std::nullopt;
  }

  // Returns whether some budget the run needs is missing or too old to size against.
  //
  // **Every configured currency must have its own fresh reading**, not merely one of them. A source
  // that names its currencies is asked, because an account trading two and answering for one is a
  // partial answer: the per-order guard would still refuse the unread currency, but the loop would
  // go on reporting an open gate while a budget it was told to watch never arrived at all.
  //
  // A source that names none falls back to "any reading at all", which is the most the loop can
  // check without knowing what was asked for.
  std::optional<strategy::HaltReason> buying_power_unread(
    std::chrono::system_clock::time_point at
  ) const {
    const auto& readings = runner_->context().portfolio().buying_power_;
    // Aged against the reading's own stamp, for the same reason the portfolio is: a figure that
    // arrived an hour ago is an hour old however recently the poll succeeded.
    const auto usable = [&](const BuyingPower& reading) {
      return !maximum_buying_power_age_ ||
        at - reading.received_at_ <= *maximum_buying_power_age_;
    };
    if constexpr (source_names_buying_power_currencies<Source>::value) {
      const auto& wanted = source_->buying_power_currencies();
      if (!wanted.empty()) {
        for (const auto& currency : wanted) {
          bool found = false;
          for (const auto& reading : readings) {
            if (reading.cash_.currency_ == currency && usable(reading)) {
              found = true;
              break;
            }
          }
          if (!found) {
            return strategy::HaltReason::BUYING_POWER_UNREAD;
          }
        }
        return std::nullopt;
      }
    }
    if (readings.empty()) {
      return strategy::HaltReason::BUYING_POWER_UNREAD;
    }
    for (const auto& reading : readings) {
      if (!usable(reading)) {
        return strategy::HaltReason::BUYING_POWER_UNREAD;
      }
    }
    return std::nullopt;
  }

  // Sets how old a buying-power reading may be before the gate closes on it.
public:
  /**
   * @brief Sets how old buying-power readings may be.
   *
   * Used only with `RiskLimits::enforce_buying_power_` on. Then a missing or too-old reading holds
   * every placement, sells included (`BUYING_POWER_UNREAD`); named cancellations still go. The
   * readings needed: one per currency the source names
   * (`live::SessionSource::set_buying_power_currencies`), or, if it names none, at least one, with
   * every reading present fresh.
   *
   * @param age The maximum age.
   */
  void set_maximum_buying_power_age(std::chrono::nanoseconds age) {
    maximum_buying_power_age_ = age;
  }

private:
  // Reads the control file, escalating `stop` when it names an intent.
  void check_control_file(StopSource& stop, LoopReport& report) {
    if (!control_.enabled()) {
      return;
    }
    if (const auto intent = control_.poll()) {
      stop.request(*intent);
      return;
    }
    if (!control_.unrecognized().empty()) {
      // Reported rather than guessed at, because picking the nearest intent would decide how much of
      // an operator's position to close on their behalf.
      report.notes_.push_back(
        "control file names no intent: '" + control_.unrecognized() + "'");
    }
  }

  // Counts an outcome, and abandons the run when one kind keeps failing.
  void note_outcome(PollKind kind, PollOutcome outcome, LoopReport& report) {
    const int at = static_cast<int>(kind);
    if (outcome == PollOutcome::FAILED) {
      ++report.failed_[at];
      ++consecutive_failures_[at];
      if (consecutive_failures_[at] >= failure_policy_.consecutive_limit_) {
        report.abandoned_ = true;
        report.abandoned_kind_ = kind;
        report.notes_.push_back(
          std::string("gave up after repeated failures polling ") + std::string(poll_kind_name(kind)));
        hold(strategy::HaltReason::POLL_FAILING);
      }
      return;
    }
    // A rate-limit refusal is the limiter working, and nothing new is the normal case between bars.
    // Neither resets the counter either: a kind alternating failure and refusal is still broken.
    if (outcome != PollOutcome::RATE_LIMITED) {
      consecutive_failures_[at] = 0;
    }
  }

  // Carries out `intent`, which is where the three stops actually differ.
  void honour(StopIntent intent, LoopReport& report) {
    hold(strategy::HaltReason::STOPPING);
    // Whenever the stop was requested -- before the iteration or during one of its polls -- nothing
    // retained before it is to be replayed after it, by this run or a resumed one.
    runner_->clear_retained_targets();

    // **The strategy is told first, and cannot trade on the way out.** `on_stop` is dispatched here,
    // while the hold stands, so every instruction it asks for is refused by the guard and counted.
    // It must not be dispatched later: from the moment the loop queues its own closing orders, a
    // strategy's order would be planned against the same snapshot and settled in the same window,
    // going through none of the sizing the closing pass does -- a second full-size close beside the
    // first, or a placement during a `WITHDRAW`, which promises none.
    // **Not watched, and it does not need to be.** `on_stop` runs with a mutable context and can revise
    // the calendar like any other handler, but `hold(STOPPING)` above is already in force, so every
    // instruction it asks for is refused before anything is sent -- which `stop_handler_sent_` asserts as
    // a report field precisely so that ordering cannot be broken silently. The revision it may have made
    // *is* answered, one call later: the closing derive below picks it up.
    //
    // For the record, the complete list of calls this loop makes out of itself is: `source_->poll`,
    // `source_->read_order_list`, `source_->buying_power_currencies` (a const accessor that is handed no
    // runner and so can neither feed nor write a calendar), `runner_->observe_sellable_convention` (which
    // reads the portfolio and dispatches nothing), this one, and `settle_closing`. The two that can send
    // under a phase are watched.
    const auto parting = runner_->notify_stop();
    // `withheld_plans_` counts here for the same reason it counts in `Report::quiet`: an `on_stop`
    // handler may state a target, `STOPPING` is held by then, and a plan that would have cancelled
    // something is withheld whole. Left out of this total, the loop would report that a parting
    // instruction met no resistance while the instruction never went anywhere.
    report.stop_handler_refused_ = parting.refused_.size() + parting.refused_cancel_.size() +
      parting.rejected_.size() + parting.withheld_.size() + parting.withheld_plans_.size() +
      parting.unavailable_.size();
    report.stop_handler_sent_ = parting.would_place_.size() + parting.would_cancel_.size();

    if (intent == StopIntent::QUIESCE) {
      return;
    }

    // **Both intents state their instructions outright rather than expressing them as a target.**
    //
    // A withdraw always had to: there is no target to reconcile against, so the working orders are
    // named one by one. A flatten used to state a zero `TargetPosition` and let reconciliation do
    // the cancelling, which was wrong twice over -- reconciliation keeps a resting order that points
    // the same way as the target, so the flatten could send nothing at all, and it skipped orders on
    // instruments with no position to target. Stating both halves here means one withdrawal per
    // resting order, no instruction derived from another, and no case where the two disagree.
    //
    // Recorded before anything is queued, because it is a statement about the picture the pass is
    // built from rather than about any instruction in it.
    report.closing_source_unverified_ = !closing_source_verified(clock_.now());

    // Sized from what is working, which counts an order this runner sent and holdings do not show yet
    // only once that row is rebuilt: a sell that filled while holdings lag is otherwise sold again.
    runner_->refresh_unconfirmed();
    const auto& portfolio = runner_->context().portfolio();
    if (intent == StopIntent::WITHDRAW) {
      for (const auto& working : portfolio.working_) {
        runner_->context().cancel(working.ref_, "stop-withdraw");
        ++report.withdrawn_;
      }
    } else {
      // **A flatten withdraws everything, then closes only what it cannot double-sell.**
      //
      // Two ways to get this wrong, and the library has now had both. Expressing the flatten as a
      // zero `TargetPosition` lets reconciliation treat a resting same-side order as progress and
      // send nothing at all -- a limit sell that may never fill is not being out of the market.
      // Sending the *whole* holding instead ignores that order, and since a cancellation is a
      // request rather than an outcome, the resting sell can fill while its cancellation is still
      // pending: ten long becomes ten short. The second error is much worse than the first, because
      // it manufactures a position in the opposite direction rather than leaving the one that was
      // already there.
      //
      // So the close is sized to what cannot double up: the holding minus whatever is still working
      // on the same side. Every working order is withdrawn either way, so a cancellation that
      // completes leaves part of the position open rather than a resting order nobody is watching,
      // and a cancellation that fails because the order filled leaves the account flat. Neither
      // outcome can overshoot, and the pass says it could not prove which one happened.
      for (const auto& working : portfolio.working_) {
        runner_->context().cancel(working.ref_, "stop-flatten");
        ++report.withdrawn_;
      }
      // **One close per instrument, sized from the net of its rows.** This looped over rows, and a
      // broker reporting one instrument as several lots is ordinary -- so it went wrong twice at
      // once. Rows of +100 and -40 are a holding of +60, and it submitted SELL 100 *and* BUY 40:
      // outside the session the phase rule refuses both, so the operator's flatten sent nothing
      // although SELL 60 was permitted all along. And the deduction below subtracts what is working
      // on the closing side, which is a figure for the whole instrument -- subtracted once per row,
      // it was charged twice, so rows of +60 and +40 against a working SELL 20 sent 40 and 20 and
      // left 20 of the position behind. Neither is reachable once the instrument is the unit.
      std::vector<InstrumentId> closed;
      for (const auto& position : portfolio.positions_) {
        bool seen = false;
        for (const auto& already : closed) {
          if (already == position.instrument_) {
            seen = true;
            break;
          }
        }
        if (seen) {
          continue;
        }
        closed.push_back(position.instrument_);

        // The net, not this row. A snapshot whose rows will not add up cannot say what flattening
        // would take, and guessing is the one thing a closing pass must not do.
        const auto net = portfolio.net_quantity_of(position.instrument_);
        if (!net) {
          // **Not `closing_contended_`.** That one means a competing order, and its note sends an
          // operator to the order list. What failed here is the holding's own arithmetic.
          ++report.closing_unsizable_;
          continue;
        }
        if (net->coefficient() == 0) {
          // Flat on the net, however many rows say so. A closed position can sit in a snapshot at
          // zero, and two rows can cancel; an order for none of it is one the broker would reject,
          // for nothing.
          continue;
        }
        if (report.closing_source_unverified_) {
          // **Nothing is placed against a listing that cannot be trusted.** The deduction below
          // subtracts what is *known* to be working on the closing side, and an unverified picture
          // is precisely one where that is not all of it: an order this runner sent after the last
          // listing, or a source that cannot enumerate orders at all. Sending a full-size close
          // beside an order nobody can see is the same double sell, reached by a different door --
          // and reporting it afterwards does not recall the instruction.
          //
          // The withdrawals above still went, because cancelling only ever reduces exposure. What
          // is left is the position, held and reported, which is the safe end of every branch here.
          ++report.closing_contended_;
          continue;
        }
        const bool held_long = decimal_less(Decimal64{}, *net);
        const auto magnitude = held_long ?
          std::optional<Decimal64>(*net) :
          decimal_subtract(Decimal64{}, *net);
        if (!magnitude) {
          // **A representable net can have no representable magnitude**: two rows summing to the
          // most negative `Decimal64` have no positive counterpart, so the size of the buy that
          // would close them cannot be stated. This used to `continue` in silence, which moved no
          // count -- so `closing_complete()` returned true over a position the pass had never
          // sized, which is the one thing that call promises it will not do.
          ++report.closing_unsizable_;
          continue;
        }
        const OrderSide closing_side = held_long ? OrderSide::SELL : OrderSide::BUY;
        const auto contending = portfolio.working_quantity(position.instrument_, closing_side);
        const auto sendable = decimal_subtract(*magnitude, contending);
        if (!sendable) {
          // The same silence for the same reason: a size that cannot be computed is not a size of
          // zero, and a pass that skipped it did not knowingly send nothing. **Unlike the two cases
          // above, this one can fail with both inputs representable** -- a held magnitude and a
          // working quantity whose scales cannot be aligned -- which is why the note names the
          // working orders as well as the holdings.
          ++report.closing_unsizable_;
          continue;
        }
        if (contending.coefficient() != 0) {
          // Sized down, and the reduction is a fact about what this pass could not establish rather
          // than about what the operator asked for. Counted so `closing_complete()` cannot claim a
          // flatten that may have left part of the position open.
          ++report.closing_contended_;
        }
        if (decimal_less(*sendable, Decimal64{}) || sendable->coefficient() == 0) {
          // Everything held is already working on the closing side. Sending anything at all here is
          // the over-close, so the withdrawal above is the whole of this instrument's flatten.
          continue;
        }
        runner_->context().submit(
          strategy::OrderIntent::market(
            position.instrument_,
            closing_side,
            strategy::SizeSpec::quantity(*sendable)
          ).with_tag("stop-flatten")
        );
        ++report.flattened_;
      }
    }

    // Lifted so the closing instructions can actually go out -- a withdraw that cannot send is not a
    // withdraw -- and closed again immediately afterwards. The window is exactly one `stop()` call
    // wide, and it is the only point at which a stop in progress can send anything.
    //
    // Only the loop's *own* hold lifts. An operator who halted by hand, or a runner whose journal
    // failed, still refuses every closing instruction -- placements through `RiskGuard::check` and
    // cancellations through the same holds in `Runner::send_cancel` -- and the closing counts say
    // so, so `closing_complete()` is false and the notes say the position is still held. A stop
    // arriving by signal or control file therefore cannot send anything through a halt somebody had
    // already set.
    hold(std::nullopt);
    // **Derived once more, immediately before the only call that can send during a stop.** `on_stop` ran
    // a few lines above with a mutable context, so it may have revised the calendar -- and unlike a
    // strategy's write inside `on_phase`, this one the loop *can* answer, because `settle_closing` is a
    // separate call it makes itself. Without this, a handler that revised the calendar to closed
    // saw the flatten go out under the `OPEN` it replaced.
    //
    // At `clock_.now()` rather than the iteration's instant, because a stop is honoured after the
    // iteration and a closing pass should be checked against the session as it is now.
    const auto closing_at = clock_.now();
    runner_->context().set_phase(phase_at(closing_at));
    const auto watch = PhaseWatch{closing_at, phase_at(closing_at), runner_->guard().total_sent()};
    const auto closing = runner_->settle_closing();
    note_revised_phase("closing pass", watch, report);
    hold(strategy::HaltReason::STOPPING);

    report.closing_placed_ = closing.would_place_.size();
    // **Both halves of every bucket.** A closing instruction is a placement or a cancellation, and
    // each can fail in the same five ways; counting only the placement half is how a stop that sent
    // nothing reported itself complete.
    report.closing_rejected_ = closing.rejected_.size() + closing.rejected_cancel_.size();
    report.closing_refused_ = closing.refused_.size() + closing.refused_cancel_.size();
    report.closing_withheld_ = closing.withheld_.size();
    report.closing_indeterminate_ =
      closing.indeterminate_.size() + closing.indeterminate_cancel_.size();
    report.closing_unavailable_ = closing.unavailable_.size() + closing.unavailable_cancel_.size();
    report.closing_unresolved_ = closing.unresolved_cancel_.size();

    // Named individually rather than summarized, because "your flatten did not go out" is the one
    // note in this whole report an operator must not have to infer.
    if (!closing.refused_.empty() || !closing.refused_cancel_.empty()) {
      report.notes_.push_back(
        "a guard refused " + std::to_string(report.closing_refused_) +
        " closing instruction(s); the position is still held");
    }
    if (!closing.rejected_.empty()) {
      // Deliberately does not name who decided. The bucket holds three things -- reconciliation
      // could not build the instruction, the broker refused it, or the binding refused to send it
      // -- and an operator reading a stop report needs the consequence, which is the same for all
      // three. The report's own entries carry which it was, on `RejectReason` for a placement and
      // on `by_broker_` for a cancellation.
      report.notes_.push_back(
        std::to_string(closing.rejected_.size()) +
        " closing instruction(s) were not placed; the position is still held");
    }
    if (!closing.rejected_cancel_.empty()) {
      report.notes_.push_back(
        std::to_string(closing.rejected_cancel_.size()) +
        " closing cancellation(s) were refused; read the broker's order list for their state");
    }
    if (report.closing_withheld_ != 0) {
      report.notes_.push_back(
        "withheld " + std::to_string(report.closing_withheld_) +
        " closing instruction(s) pending reconciliation; read the broker's order list");
    }
    if (report.closing_indeterminate_ != 0) {
      report.notes_.push_back(
        std::to_string(report.closing_indeterminate_) +
        " closing instruction(s) failed in transit; their outcome is unknown");
    }
    if (report.closing_unavailable_ != 0) {
      report.notes_.push_back(
        "local machinery could not send " + std::to_string(report.closing_unavailable_) +
        " closing instruction(s); no broker was asked and the position is still held");
    }
    if (report.stop_handler_refused_ != 0) {
      report.notes_.push_back(
        "the strategy's on_stop asked for " + std::to_string(report.stop_handler_refused_) +
        " instruction(s) during the stop; none were sent, because an operator's stop is not a"
        " moment for a strategy to trade");
    }
    if (report.closing_contended_ != 0) {
      report.notes_.push_back(
        report.closing_source_unverified_ ?
          "no closing order was sent for " + std::to_string(report.closing_contended_) +
            " position(s), because an order this loop cannot see could close them too; they are"
            " still held and the withdrawals above are all that went" :
          std::to_string(report.closing_contended_) +
            " position(s) were closed only for the part no working order was already selling or"
            " buying; if those withdrawals succeed, part of the position is still held");
    }
    if (report.closing_unsizable_ != 0) {
      // **Names both inputs, because the three causes do not share one.** Two of them are the
      // holdings rows alone -- they will not add up, or the net has no representable closing
      // magnitude. The third subtracts what is already working on the closing side, and that
      // subtraction can fail with both inputs perfectly representable: a held quantity of
      // 1,000,000,000,000,000,000 and a working close of 0.1 cannot be aligned. An earlier note
      // blamed the holdings snapshot alone and would have sent an operator to check one of two
      // inputs. It still does not say "contended", which means a competing order reduced the close;
      // here nothing could be sized at all.
      report.notes_.push_back(
        "no closing order could be sized for " + std::to_string(report.closing_unsizable_) +
        " position(s): the holdings quantities would not add up or fit, or could not be combined"
        " with what is already working on the closing side; nothing was sent for them and they are"
        " still held");
    }
    if (report.closing_source_unverified_) {
      report.notes_.push_back(
        std::string("the stop could not verify what was outstanding: ") + unverified_source_reason() +
        "; orders or positions a previous process left may remain");
    }
    if (report.closing_unresolved_ != 0) {
      report.notes_.push_back(
        std::to_string(report.closing_unresolved_) +
        " closing cancellation(s) named an order the portfolio does not hold; read the broker's"
        " order list");
    }
  }

  // Takes `gate`'s hold on the guard and releases the one this loop took before it.
  //
  // There is no "did I halt it" flag any more, and that is the point: the loop names the hold it
  // takes and releases only that one, so an operator halting by hand into a loop's closed gate is
  // no longer invisible, and the loop reopening its gate no longer lifts their halt with it. The
  // hold is released and retaken rather than diffed, because taking a hold is idempotent.
  //
  // Only the loop's *own* conditions are held. `OPERATOR` belongs to a person and `LOCAL_FAILURE`
  // to the runner; both outlive any gate the loop computes, so they are reported and never taken or
  // released here.
  void hold(std::optional<strategy::HaltReason> gate) {
    if (gate && !owns(*gate)) {
      gate.reset();
    }
    if (held_ == gate) {
      return;
    }
    if (held_) {
      runner_->guard().resume(*held_);
    }
    if (gate) {
      runner_->guard().halt(*gate);
    }
    held_ = gate;
  }

  // Says what is missing for a runner that keeps its sent orders until the broker accounts for them, as a
  // note bit and the note, or nothing when the listing is in hand.
  std::optional<std::pair<unsigned, std::string>> listing_missing(
    std::chrono::system_clock::time_point at
  ) const {
    if constexpr (!source_provides_working_orders<Source>::value) {
      (void)at;
      return std::make_pair(1u, std::string(
        "this runner keeps the orders it sends until a working-order listing or holdings account for "
        "them, and the source publishes no listing: placing holds (declare provides_working_orders and "
        "poll the listing)"
      ));
    } else {
      if (!schedule_.working_orders_) {
        return std::make_pair(2u, std::string(
          "no working-order cadence: this runner keeps the orders it sends until a working-order "
          "listing or holdings account for them, so placing holds until PollSchedule::working_orders_ "
          "is set"
        ));
      }
      if (maximum_portfolio_age_ && working_orders_at_ &&
          at - *working_orders_at_ > *maximum_portfolio_age_) {
        return std::make_pair(4u, std::string(
          "working-order listing older than the portfolio age policy: poll working orders "
          "(PollSchedule::working_orders_) more often than that age, by at least a tick; placing "
          "holds until it is read"
        ));
      }
      return std::nullopt;
    }
  }

  // Returns whether the loop knows what a closing pass at `at` would have to send.
  //
  // Two sources, both required. The broker's own order list must have been read once, which is what
  // the startup gate exists for, and what a stop requested before the first iteration attempts itself
  // (a failed attempt leaves it unread). And the
  // holdings snapshot -- which is where a `WITHDRAW`'s working orders and a `FLATTEN`'s positions
  // actually come from -- must exist, be within the configured age, and be **no older than the
  // working-order listing**: a fill between the two reads leaves the listing before it reaches the
  // holdings, so a flatten would see neither the order nor the shares it bought.
  bool closing_source_verified(std::chrono::system_clock::time_point at) const {
    if (!order_list_read_) {
      return false;
    }
    // The working-order picture is its own source with its own freshness, and `as_of_` is not its
    // timestamp: `SessionSource::poll_holdings` refreshes positions and `as_of_` while *keeping* the
    // working orders it already had, and only a working-order poll replaces those. So a stop can
    // have a snapshot stamped a second ago whose resting orders were last read an hour ago.
    if (!working_orders_at_) {
      return false;
    }
    // And the decisive one, which no timestamp catches: an order this runner sent after that read
    // cannot be in it. A listing is out of date the moment the next placement is accepted, and an
    // indeterminate send counts too, because the order may exist.
    if (runner_->placements_at_broker() != placements_at_working_read_) {
      return false;
    }
    if (maximum_portfolio_age_ && at - *working_orders_at_ > *maximum_portfolio_age_) {
      return false;
    }
    const auto as_of = runner_->context().portfolio().as_of_;
    if (as_of == std::chrono::system_clock::time_point{}) {
      return false;
    }
    // On one clock: both stamps are the local arrival time of their replies, which
    // `working_orders_at_` above is not.
    if (runner_->context().portfolio().listing_newer_than_holdings()) {
      return false;
    }
    return !maximum_portfolio_age_ || at - as_of <= *maximum_portfolio_age_;
  }

  // Records that the working-order picture was just refreshed, with what it could not contain.
  //
  // Does nothing when the source does not publish working orders, so a stop on such a broker stays
  // unverified rather than being told a history read or a no-op poll refreshed a listing it never
  // built.
  void note_working_orders_read(
    std::chrono::system_clock::time_point at, std::size_t placements_before_poll
  ) {
    if constexpr (source_provides_working_orders<Source>::value) {
      working_orders_at_ = at;
      // **The caller's count, taken before the poll ran.** Reading it here instead would include
      // an order an `on_order_update` handler placed *during* the poll -- an order sent after the
      // listing was fetched, which the listing therefore cannot contain.
      placements_at_working_read_ = placements_before_poll;
    } else {
      (void)at;
      (void)placements_before_poll;
    }
  }

  // Returns which half of the closing source is missing, for the operator's note.
  std::string unverified_source_reason() const {
    if constexpr (!source_provides_working_orders<Source>::value) {
      return "this source does not publish the broker's outstanding orders";
    }
    if (!order_list_read_ || !working_orders_at_) {
      return "the broker's order list has not been read";
    }
    if (runner_->placements_at_broker() != placements_at_working_read_) {
      return "an order was sent after the working orders were last read";
    }
    if (runner_->context().portfolio().as_of_ == std::chrono::system_clock::time_point{}) {
      return "no holdings snapshot has arrived";
    }
    if (runner_->context().portfolio().listing_newer_than_holdings()) {
      return "the holdings were read before the working orders, so a fill between them is in neither";
    }
    if (maximum_portfolio_age_ &&
        clock_.now() - *working_orders_at_ > *maximum_portfolio_age_) {
      return "the working orders were read longer ago than the configured maximum";
    }
    return "the holdings snapshot is older than the configured maximum";
  }

  // Returns whether `kind` is due at `at`.
  bool is_due(PollKind kind, std::chrono::system_clock::time_point at) const {
    const auto interval = schedule_.interval_for(kind);
    if (!interval) {
      return false;
    }
    const auto& due = next_due_[static_cast<int>(kind)];
    return !due || at >= *due;
  }

  // Records when `kind` is next due.
  void schedule_next(PollKind kind, std::chrono::system_clock::time_point at) {
    const auto interval = schedule_.interval_for(kind);
    if (!interval) {
      return;
    }
    next_due_[static_cast<int>(kind)] =
      at + std::chrono::duration_cast<std::chrono::system_clock::duration>(*interval);
  }

  // Returns how long to wait before the next iteration.
  //
  // The soonest due poll, capped at the tick so a stop through the control file is noticed within
  // one tick however sparse the polling is. Never negative: a loop that has fallen behind catches up
  // rather than pausing once per overdue poll.
  std::chrono::nanoseconds until_next(std::chrono::system_clock::time_point at) const {
    auto wait = schedule_.tick_;
    for (const auto kind : all_poll_kinds) {
      const auto& due = next_due_[static_cast<int>(kind)];
      if (!due) {
        continue;
      }
      const auto remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(*due - at);
      if (remaining < wait) {
        wait = remaining;
      }
    }
    return wait > std::chrono::nanoseconds::zero() ? wait : std::chrono::nanoseconds::zero();
  }

  Runner* runner_;
  Source* source_;
  PollSchedule schedule_;
  Clock clock_;
  FileStopChannel control_;
  FailurePolicy failure_policy_;
  strategy::SessionWindow session_window_{strategy::SessionWindow::krx()};
  // The phase most recently announced, which is not the same as the phase in force.
  //
  // `UNKNOWN` to begin with rather than "nothing announced yet", because that is genuinely what the
  // loop knows before its first calendar arrives -- and it means a run that starts knowing nothing and
  // then learns the market is open announces that as a change, which is exactly right.
  strategy::MarketPhase announced_{strategy::MarketPhase::UNKNOWN};
  // Whether this loop has ever derived a phase that was not `UNKNOWN`, across every `run`.
  bool phase_ever_known_{false};
  std::optional<std::chrono::nanoseconds> maximum_portfolio_age_;
  // Whether the missing-band-policy note has been said; it is said once per loop.
  bool said_no_band_age_policy_{false};
  // Whether the operator stated that any snapshot age is acceptable; see the setter.
  bool allow_unbounded_portfolio_age_{false};
  // Whether the operator stated that this run may act with no snapshot at all; see the setter.
  bool allow_absent_portfolio_{false};
  // Whether the missing-age-policy note has been reported, so it is said once and not per poll.
  bool said_no_age_policy_{false};
  // Which of `listing_missing`'s notes have been given, so each is said once per loop.
  unsigned said_listing_{0};
  // The runner's discarded-history count when this run began; see `note_warmup_discarded`.
  std::size_t warmup_discarded_at_start_{0};
  // The runner's dropped-kept-intent count when this run began; see `note_kept_intents_dropped`.
  std::size_t kept_dropped_at_start_{0};
  std::optional<std::chrono::nanoseconds> maximum_buying_power_age_;
  std::optional<std::chrono::system_clock::time_point> next_due_[poll_kind_count];
  std::size_t consecutive_failures_[poll_kind_count] = {};
  bool order_list_read_{false};
  // Returns whether `reason` is a condition this loop computes, and may therefore hold.
  static constexpr bool owns(strategy::HaltReason reason) noexcept {
    return reason != strategy::HaltReason::OPERATOR &&
      reason != strategy::HaltReason::LOCAL_FAILURE;
  }

  // When the working-order picture was last refreshed, or nothing while it never has been.
  std::optional<std::chrono::system_clock::time_point> working_orders_at_;
  // `Runner::placements_at_broker()` as of that read, so a later send invalidates it.
  std::size_t placements_at_working_read_{0};

  // The hold this loop currently owns, and the only one it will ever release.
  std::optional<strategy::HaltReason> held_;
  IterationReport last_;
};

} // namespace runner
} // namespace algotradelib
