# The analytics layer: two numeric worlds and three conventions {#analytics-conventions}

The analytics layer computes. Everything below it — quotes, orders, holdings, market data —
reports what a broker said. The two need different number types, and most of the ways this
layer can quietly go wrong are ways of forgetting that. This page is the set of rules that
prevent it.

The whole path, in one block: exact decimals in, a computation, and one gated route back out to
something a broker will accept.

```cpp
#include <algotradelib/analytics>
#include <algotradelib/krx>

namespace analytics = algotradelib::analytics;

// The one place a broker's exact decimals become scalars.
const analytics::SeriesFrame frame = analytics::from_bars(bars);

const auto close = frame.close(); // a view, not a copy

// Your own computation: here the mean of the last 20 closes, from a frame of at least 20 rows.
const auto window = close.tail(20);
const bool complete = (window.array() == window.array()).all(); // no absent close in it
const auto average = window.mean();

// And the one route back to something a broker will accept.
const auto rule = algotradelib::krx::equity_price_rule();
const auto limit = complete ?
  analytics::round_scalar_to_tick(average, rule, algotradelib::OrderSide::SELL) :
  std::nullopt;
```

`example-analytics-offline` runs that path with bars built in code — no credentials, no network,
identical output every time.

This layer is also the only one that needs a linear-algebra backend, and it is optional. With
`-DATL_USE_EIGEN=OFF` the quote, order, holdings and market-data headers all still build, and
`<algotradelib/analytics>` refuses to compile with a message naming the option to set, rather than
producing twenty errors about missing types.

## Why there are two numeric types at all

`Decimal64` holds what a broker said, exactly: a 64-bit integer coefficient and a decimal
scale. `70000.50` is the *value* seventy thousand and a half, held without binary floating
point, and it stays that through a round trip, a reconciliation, and a comparison. One field is
the exception: Toss can state an order's amount with more digits than a `Decimal64` holds, and
`OrderSnapshot::order_amount_` is then rounded to 18 significant digits.

What it does **not** preserve is the broker's formatting. `parse` normalizes away trailing
zeros, so `parse("70000.50")` is the coefficient `700005` at scale `1`, the same
representation `parse("70000.5")` gives — which is exactly why two spellings of one figure
reconcile equal. Only a value built at an explicit scale keeps that scale; see
`Decimal64::operator==`. That exactness is not a nicety. An order
price is a value an exchange either accepts or rejects, a filled quantity is a number a
ledger must balance, and a price that is right to within one part in `2^52` is still wrong.

`math::Scalar` — `double` by default — holds what a computation needs. A standard deviation,
a regression coefficient, and a correlation are all irrational in general, and there is no
exact decimal representation to preserve. Trying to keep them exact would mean growing the
scale on every operation until the coefficient overflowed.

So the library uses both, and the discipline is that **they meet in exactly one place**.

## Convention 1: one gated boundary, and it is narrower outbound

Every conversion lives in `analytics/conversion.hpp`:

- `to_scalar(Decimal64)` is total. Within 53 bits of coefficient (every price and quantity
  the bundled brokers quote) and 22 decimal places, in the default round-to-nearest mode, it
  gives the nearest double, the value `std::strtod` or Python's `float` gives for the
  decimal's text: `to_scalar(Decimal64(7, 2))` is `0.07`.
  Beyond those bounds it is an approximation. Arithmetic on the result still rounds, so
  compare computed values with a tolerance.
- `to_decimal(Scalar, scale)` returns `std::optional`. It refuses a non-finite value and a
  value that would overflow the coefficient, and it rounds half away from zero at the scale
  it was asked for. It honours that scale literally: `to_decimal(1.5, 3)` is `1.500`, which
  is numerically equal to `Decimal64::parse("1.5")` but not `==` to it, because the scale is
  part of the value.
- `round_scalar_to_tick(Scalar, rule, side, scale)` is the **only** sanctioned route from a
  computed number to an order price. A value correct to four decimal places and off the
  exchange's tick ladder is rejected exactly as firmly as a wrong one, so the rounding and
  the grid are one step rather than two.

The asymmetry is deliberate. Going in, a broker's number is always representable. Coming
out, a computation can produce an infinity, a `NaN`, or a magnitude no decimal coefficient
holds, and each of those has to be refused rather than truncated into something plausible.

`PriceRule::Kind::UNKNOWN` refuses to round at all.
An instrument whose tick ladder the library does not know (a warrant or a foreign ETF in the KRX tables) gets no answer rather than a guess, because a guessed tick is an order the exchange will reject at best and fill at the wrong price at worst.
An `UNCONSTRAINED` rule (Toss's US listings, the mock broker) is different: its price is only rounded to the `scale` passed, which defaults to 0, so pass the market's (2 for US dollars).

## Convention 2: two kinds of absence, and every reduction masks both

There is one representation for "no observation": `math::missing()`, a quiet `NaN`. It stands for
a warm-up region, a gap in a panel, a field a broker did not report, and a window that did not
hold enough data — all of them, so a caller learns one check,
`math::observed(value)`.

There is a second: `math::undefined()`, for "the data was there and the statistic does not exist
for it". A Sharpe ratio over returns that never varied. A Sortino ratio over returns that never fell
below their target. A drawdown ratio on an equity curve whose peak is zero.

Both are quiet `NaN`s, so `observed()` is false for both and every reduction masks both — nothing
downstream has to know the difference. It is readable only through `math::is_undefined()`, and it
exists because there are decisions where it matters: ranking strategies, "not enough history yet"
and "no downside risk at all" are very different answers, and collapsing them loses the
distinction exactly where a caller would act on it.

**The caveat, stated precisely.** The distinction lives in the `NaN` payload, and IEEE-754 leaves
the payload of a propagated `NaN` implementation-defined. `undefined() + 1.0` is still a `NaN` but
need not still be *this* one. So test it on a value a function returned, and treat anything
computed from it as merely absent. Storing it in a matrix and reading it back is a bit copy and
preserves it.

`missing()` rather than zero, always. Zero is a number that means something: a return of zero is a
claim the price did not move, a weight of zero is a decision to hold nothing. An absent value is
not a claim.

One thing neither covers: an infinity. `NaN` is masked everywhere; `inf` would survive every
reduction and arrive at a comparison as the best result ever measured. So the functions that could
produce one refuse instead — a Sortino ratio with no downside, a drawdown ratio from a zero peak.

## Convention 3: nothing is annualized for you

Where packages differ, each function states which convention it implements, next to the arithmetic,
because the differences change decisions rather than decorating them:

| Function | Convention, and the other one |
| --- | --- |
| `sortino` | shortfalls divided by **all** observations, not by the losing ones |
| `sharpe` | per period, over the sample deviation (divisor `count - 1`). `annualize_ratio` is separate, and scales by `sqrt(periods)` |

Annualization is a separate call
because the factor is the square root of the period count, not the count (confusing them overstates
a daily Sharpe ratio about sixteenfold), and because the period count is a calendar question — 245
KRX trading days, 252 US, 12 months — that no default answers correctly for everyone.

## Numerical choices that are not stylistic

Three places where the obvious formula is the wrong one, all of them tested at the level
where they break:

- **Variance.** The statistics take two passes over a series rather than computing
  `E[x²] - E[x]²`. At price levels near `1e8` the one-pass form returns a variance of 2 where the
  answer is 1, a standard deviation wrong by 41%, with no diagnostic, because the two terms agree
  to more digits than a `double` has. It can even come out negative and become a `NaN` at the
  square root, with nothing to say arithmetic rather than data produced it.
- **Masking is paid for once.** A statistic splits its input through `math::observed_parts` into
  values-with-absences-zeroed plus a 0/1 presence mask. A sum is then `values_.sum()` and its
  divisor `present_.sum()` — two reductions over contiguous memory with no branch and no
  per-element predicate. Adding zero for an absent entry is exact, so the answers are unchanged.
  Deviations vectorize through the same pair: subtracting `mean * present_` leaves an absent row
  contributing exactly zero, so a variance is one `squaredNorm()`.
  A constant series is recognized with the absent rows left out too, so equal values around a gap
  are a constant, not a dispersion.
- **The masking vocabulary lives in the seam, not in the statistics.** `observed_parts` is defined
  in `math/eigen.hpp`, so the statistics do not spell their own absence test. The layer above still
  uses the backend's expression API for its arithmetic (sums, norms, segments), so a second backend
  would supply more than this one function.

  Eigen's plugin macros (`EIGEN_MATRIXBASE_PLUGIN` and friends) would allow `series.observedSum()`
  instead, and are deliberately not used: they must be defined before any Eigen header anywhere in
  the translation unit, so a consumer who includes Eigen first would silently get a different
  `Eigen::Matrix` than this library's own translation units — an ODR violation with no diagnostic.
  Appropriate for an application that owns its whole build; not for a header-only library other
  people include.

## What this layer deliberately does not compute

There are no indicators, no regression and no covariance estimator, and the omission is settled
rather than pending.

Indicators belong to the strategy that uses them: moving averages, z-scores and the RSI are in the
replication repositories and in atls's built-ins, and a strategy written against this library keeps
its own window (`example-backtest-offline`, `example-strategy-dryrun`).

Fitting is one call — `x.colPivHouseholderQr().solve(y)`, where `y` may be a matrix of targets and the
result a matrix of coefficients. A sample covariance of a complete observation matrix is two lines:

```cpp
const math::Matrix centered = observations.rowwise() - observations.colwise().mean();
const math::Matrix result = (centered.transpose() * centered) / (observations.rows() - 1);
```

What a library function would add on top of those is not arithmetic but *choices*: which
predictors, over what estimation window, how a target aligns to lagged features, whether an
incomplete row is dropped or imputed, pairwise or listwise deletion. Those are modelling decisions,
and answering them with defaults would make a strategy's choices while appearing to offer a utility.

**The line, stated so it can be applied again:** ship what has one right answer everyone agrees on;
defer what requires choosing. `sortino` ships with its divisor documented rather than parameterized because there *is* a correct answer.
"Covariance of series with gaps" has no canonical answer, so it waits for a consumer that can say
what it needs.

## Where the numbers live

| Type | Holds | Storage |
| --- | --- | --- |
| `TimeIndex` | the instants rows correspond to | one `Instant` per row |
| `SeriesFrame` | one instrument, every bar field | column-major, rows are time |
| `Panel` | one field, many instruments, aligned | column-major, one column per instrument |
| `SeriesBuffer` | the trailing `n` observations a live loop feeds | `2n` rows, compacted amortized |

Column-major with time on the rows is load-bearing rather than a default taken: one series is
then contiguous, so `column()` hands out a `math::SeriesView` referencing the frame's own
memory and a computation never walks a strided view of its input. It is also why the same
code serves a completed frame and a live buffer — both are one contiguous series, which
is what keeps a backtest and a live loop from computing different things.

`SeriesBuffer` fills until it holds `n` observations and from then on drops the oldest with each
push, in storage taken once: `2n` rows, compacted at an amortized one row moved per push, with
`compactions()` exposing the bound. It is a buffer and computes nothing: what a strategy computes
over it is the strategy's own. "Rolling" and "expanding" windows keep the meaning they have in
walk-forward evaluation. There is
deliberately **no unbounded buffer**: the statistics that would want one — an all-time high, the
worst drawdown so far — need a running value the strategy keeps, such as a peak or a
sum, not the history.

The exponential average is the one worth a sentence of its own. It depends on all of its history, so
recomputing one over a trailing `SeriesBuffer` reseeds it every bar and does not reproduce what a live
loop would have produced; a strategy fitted over frames and traded that way would be trading a slightly
different indicator, and the difference does not announce itself. A live strategy keeps the level
instead: seeded with the first observed close, skipping a bar whose close is unobserved, and updated
on every later observed close as `level = level + alpha * (close - level)`, in that order. The same
update over a frame then gives the same numbers, bit for bit.

## What is single-owner, and where parallelism goes

Every stateful type here — `SeriesFrame`, `Panel`, `SeriesBuffer` — is
owned by one caller and is not synchronized, the model the rest of the library follows. A
mutex inside `SeriesBuffer` would not help with the real problem anyway: reading a trailing
window and then acting on it is a *sequence* that has to be atomic with respect to the next
push, and no lock inside the class can provide that.

Parallelism therefore belongs across units of work — one instrument, one parameter set — each
owning its own containers. That is also where the win is: a column-wise reduction over a few
thousand rows loses to thread hand-off, while two thousand independent instruments scale
nearly linearly.

Eigen's own threading is left entirely alone. It threads dense matrix products when `_OPENMP`
is defined and the work clears its internal threshold, and this library neither enables nor
suppresses that — in particular it never defines `EIGEN_DONT_PARALLELIZE`, which is a
compile-time amputation with no runtime path back. What the build does do is link an OpenMP
runtime when the flags define `_OPENMP`, so that Eigen's threaded path resolves instead of
failing at link time on `__kmpc_*` symbols with nothing to connect them to a matrix product.
