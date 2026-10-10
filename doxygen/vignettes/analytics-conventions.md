# The analytics layer: two numeric worlds and four conventions {#analytics-conventions}

The analytics layer computes. Everything below it — quotes, orders, holdings, market data —
reports what a broker said. The two need different number types, and most of the ways this
layer can quietly go wrong are ways of forgetting that. This page is the set of rules that
prevent it.

The whole path, in one block: exact decimals in, a computation, and one gated route back out to
something a broker will accept.

```cpp
#include <algotradelib/analytics>

namespace analytics = algotradelib::analytics;

// The one place a broker's exact decimals become scalars.
const analytics::SeriesFrame frame = analytics::from_bars(bars);

const auto close = frame.close();               // a view, not a copy
const auto average = analytics::sma(close, 20);
const auto bands = analytics::bollinger(close, 20, 2.0);
const auto strength = analytics::rsi(close, 14);

// And the one route back to something a broker will accept.
const auto rule = algotradelib::price_rule_for(instrument.country_, SecurityType::STOCK);
const auto limit = analytics::round_scalar_to_tick(
  bands.upper_[close.size() - 1], rule, OrderSide::SELL
);
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
point, and it stays that through a round trip, a reconciliation, and a comparison.

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

`PriceRule::Kind::UNKNOWN` refuses to round at all. An instrument whose tick ladder the
library does not know — an ETN, a foreign listing on a venue with its own rules — gets no
answer rather than a guess, because a guessed tick is an order the exchange will reject at
best and fill at the wrong price at worst.

## Convention 2: two kinds of absence, and every reduction masks both

There is one representation for "no observation": `math::missing()`, a quiet `NaN`. It stands for
an indicator's warm-up region, a gap in a panel, a field a broker did not report, and a window
that did not hold enough data — all of them, so a caller learns one check,
`math::observed(value)`.

There is a second: `math::undefined()`, for "the data was there and the statistic does not exist
for it". A z-score over a window with no dispersion. A correlation against a constant series. A
Sortino ratio over returns that never fell below their target. A beta against a market that did
not move. A drawdown ratio on an equity curve whose peak is zero.

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
produce one refuse instead — a return over a zero prior price, a Sortino ratio with no downside, a
beta against a market that did not move.

## Convention 3: a result is as long as its input

Row `i` of every output describes row `i` of its input. A function that returned a shorter
series would hand the caller an alignment problem, and an off-by-one there is a lookahead
bug that no test of the function itself can catch: the strategy simply performs
impossibly well.

So the warm-up region is *present* and unobserved, not absent. `returns(prices)` has the
same length as `prices` with an unobserved row 0. `rolling_mean(values, 20)` has nineteen
unobserved rows before its first answer. A fitted regression's residuals carry `missing()`
on the rows the fit could not use.

## Convention 4: two numbers describe a window, and nothing is annualized for you

A window is described by two numbers that answer different questions, and the names say which:

- **`window`** is how many rows are looked at — the trailing span, in rows of the input, whether or
  not those rows carry observations.
- **`min_observed`** is how many of those rows must actually carry a value before a result is
  reported. A count of observations, never a span.

They differ exactly when a series has gaps. `rolling_mean(closes, 20, 15)` looks at twenty rows
and reports as soon as fifteen are observed, dividing by however many it saw. The two-argument
overload, `rolling_mean(closes, 20)`, demands all twenty — the safe default, and what you get
without asking. There is no sentinel value meaning "all": the default is expressed by the overload,
and asking for zero observations is an error.

The check is what masking makes necessary. Because an absent value inside a window is skipped
rather than propagated, a window's divisor is the count of values it saw — so without
`min_observed`, a twenty-row mean computed from three observations would be reported as though it
were a twenty-observation mean, with nothing in the result to say otherwise. That is the second
most common source of lookahead bias after bar stamping, so relaxing it is an explicit request at
the call site.

Every trailing function advances one row at a time. A stride greater than one is not offered and is
also not foreclosed: because the result is always as long as the input, a stride means "compute at
every `k`-th row, leave the rest unobserved", which reaches the API as a trailing defaulted
parameter without changing any existing meaning. Usually the need behind it is resampling rather
than subsampling, which the library does not do yet.

The same principle governs the conventions that differ between packages. Each function states which
one it implements, next to the arithmetic, because the differences change decisions rather than
decorating them:

| Function | Convention, and the other one |
| --- | --- |
| `ema`, `ema_step` | take the smoothing factor `alpha`; `alpha_from_span(n)` is the span convention `2 / (n + 1)`; seeded on the first observed value. `ema_step` is the one-value update; `ema` over a series matches, on observed rows, `pandas` `ewm(alpha=..., adjust=False, ignore_na=True, min_periods=min_observed)` (it reports nothing before `min_observed` observations), which also fills a missing row with the carried level |
| `rsi`, `atr` | Wilder's `1 / n` with Wilder's seed, not `2 / (n + 1)` |
| `rolling_stdev`, `rolling_cov` | sample divisor `count - 1`, not the population `count` |
| `sortino` | shortfalls divided by **all** observations, not by the losing ones |
| `one_way_turnover` | `0.5 * sum(abs(delta w))`; `two_way_turnover` is exactly twice it |
| `sharpe` | per period. `annualize_ratio` is separate, and scales by `sqrt(periods)` |

Two of those are named rather than parameterized on purpose. `one_way_turnover` and
`two_way_turnover` exist as separate functions because turnover is usually multiplied by a cost per
unit traded, and a silent factor of two there separates a profitable strategy from a losing one —
the convention belongs in the name, not only in the documentation. `TurnoverStart` then chooses
whether the first period is measured against an all-cash portfolio — the default, because building a
position is real trading — or left unobserved, which is what you want when comparing strategies
whose histories start on different dates. Annualization is a separate call
because the factor is the square root of the period count, not the count (confusing them overstates
a daily Sharpe ratio about sixteenfold), and because the period count is a calendar question — 245
KRX trading days, 252 US, 12 months — that no default answers correctly for everyone.

## Numerical choices that are not stylistic

Three places where the obvious formula is the wrong one, all of them tested at the level
where they break:

- **Trailing variance.** `rolling_stdev` takes two passes over each window rather than
  computing `E[x²] - E[x]²`. At price levels near `1e8` the one-pass form returns a variance
  of 2 where the answer is 1 — wrong by 41% with no diagnostic — because the two terms agree
  to more digits than a `double` has. It can even come out negative and become a `NaN` at the
  square root, with nothing to say arithmetic rather than data produced it.
- **Masking is paid for once per series, not once per window.** Every trailing function splits its
  input through `math::observed_parts` into values-with-absences-zeroed plus a 0/1 presence mask.
  A window's sum is then `values_.segment(...).sum()` and its divisor `present_.segment(...).sum()`
  — two reductions over contiguous memory with no branch and no per-element predicate. Adding zero
  for an absent entry is exact, so the answers are unchanged; only the summation order differs, and
  the backend's is the more accurate.

  Measured on 200k rows with a 200-row window and a gap every 97th row, this is **7x faster** than
  the equivalent scalar loop, and about 5x faster than masking inside each window — the cost was
  re-deriving the mask, not the loop. Deviations vectorize through the same pair, which is the
  non-obvious part: subtracting `mean * present_` leaves an absent row contributing exactly zero,
  so a variance is one `squaredNorm()`.

  The remaining `O(rows x window)` structure is deliberate. A prefix-sum would make it linear and
  would lose exactly the significant digits the two-pass variance exists to protect.

  `ema` and `cumulative_returns` are the exceptions and cannot be vectorized at all: each value
  depends on the one before it, so there is no reduction to hand a backend.

- **The masking vocabulary lives in the seam, not in the indicators.** `observed_parts`,
  `observed_or` and `both_present` are defined in `math/eigen.hpp`, so no analytics header writes
  `.array().isNaN().select(...)` and the layer above stays backend-agnostic while still running at
  full speed. A second backend implements three small functions rather than having its expression
  API assumed throughout.

  Eigen's plugin macros (`EIGEN_MATRIXBASE_PLUGIN` and friends) would allow `series.observedSum()`
  instead, and are deliberately not used: they must be defined before any Eigen header anywhere in
  the translation unit, so a consumer who includes Eigen first would silently get a different
  `Eigen::Matrix` than this library's own translation units — an ODR violation with no diagnostic.
  Appropriate for an application that owns its whole build; not for a header-only library other
  people include.

And two places where the honest answer is a flag rather than a number:

- **The decompositions are Eigen's own, exposed as aliases**, not wrapped. `math::Llt`,
  `math::Ldlt`, `math::HouseholderQr`, `math::ColPivQr`, `math::FullPivQr`, `math::FullPivLu`,
  `math::Svd` and `math::SelfAdjointEigen` are used directly, so one factorization solves many
  right-hand sides, a right-hand side may be a matrix as easily as a vector, and the rank,
  permutation, singular values and `info()` a decomposition already knows are all still there.
  Wrapping each in a result struct would have copied the factors out and thrown the rest away.

  Which to reach for is a trade-off the caller makes: `ColPivQr` for a design matrix of market
  data, where nearly collinear columns are the normal case and the normal equations square the
  condition number; `Llt` when the system is known symmetric positive definite and speed
  matters; `Ldlt` when it is only semi-definite; `Svd` when the answer must be the minimum-norm
  one or the conditioning is itself the question.

  For decompositions the backend seam is a naming convention rather than a portability
  guarantee, because no two libraries agree on what a factorization object exposes. The numeric
  vocabulary — `Matrix`, `Vector`, `SeriesView` — really is portable. That distinction is worth
  stating instead of letting a uniform-looking `math::` prefix imply more than it delivers.

## What this layer deliberately does not compute

There is no regression and no covariance estimator, and the omission is settled rather than
pending.

Fitting is one call — `math::ColPivQr(x).solve(y)`, where `y` may be a matrix of targets and the
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
defer what requires choosing. `macd(12, 26, 9)` means one thing, and the value of shipping it is
that Wilder's seed and the span convention are got right once instead of re-derived. `sortino`
ships with its divisor documented rather than parameterized because there *is* a correct answer.
"Covariance of series with gaps" has no canonical answer, so it waits for a consumer that can say
what it needs.

Note that this is not the same rule as "ship only what the first strategies use". Applied that way
it would also delete `macd`, `bollinger`, `atr`, `wma`, `sharpe`, `one_way_turnover`,
and `max_drawdown`, since the first set of vanilla strategies needs only `sma`,
`ema`, `rolling_min`/`rolling_max`, `zscore`, `rsi`, `rolling_stdev`, and `Panel`.

## Where the numbers live

| Type | Holds | Storage |
| --- | --- | --- |
| `TimeIndex` | the instants rows correspond to | one `Instant` per row |
| `SeriesFrame` | one instrument, every bar field | column-major, rows are time |
| `Panel` | one field, many instruments, aligned | column-major, one column per instrument |
| `SeriesBuffer` | the trailing `n` observations a live loop feeds | `2n` rows, compacted amortized |

Column-major with time on the rows is load-bearing rather than a default taken: one series is
then contiguous, so `column()` hands out a `math::SeriesView` referencing the frame's own
memory and an indicator never walks a strided view of its input. It is also why the same
`sma` call serves a completed frame and a live buffer — both are one contiguous series, which
is what keeps a backtest and a live loop from computing different things.

`SeriesBuffer` fills until it holds `n` observations and from then on drops the oldest with each
push, in storage taken once: `2n` rows, compacted at an amortized one row moved per push, with
`compactions()` exposing the bound. It is a buffer and computes nothing: the trailing statistics are
the functions in `rolling.hpp`. "Rolling window" keeps the meaning the `rolling_*` functions give it,
and "rolling" and "expanding" windows the one they have in walk-forward evaluation. There is
deliberately **no unbounded buffer**: the statistics that would want one — a cumulative return, an
all-time high, the worst drawdown so far — need a running value the strategy keeps, such as a peak or a
sum, not the history.

The exponential average is the one worth a sentence of its own. It depends on all of its history, so
recomputing one over a trailing `SeriesBuffer` reseeds it every bar and does not reproduce what a live
loop would have produced; a strategy fitted over frames and traded that way would be trading a slightly
different indicator, and the difference does not announce itself. A live strategy keeps the level
instead: `level = ema_step(level, close, alpha)` once per bar makes the same update `ema` makes over a
series, bit for bit unless the arithmetic itself produces a `NaN` (an infinity, an overflow), after
which `ema` stays unobserved and a kept level reseeds.

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
