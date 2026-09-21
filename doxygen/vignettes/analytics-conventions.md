# The analytics layer: two numeric worlds and four conventions {#analytics-conventions}

The analytics layer computes. Everything below it — quotes, orders, holdings, market data —
reports what a broker said. The two need different number types, and most of the ways this
layer can quietly go wrong are ways of forgetting that. This page is the set of rules that
prevent it.

## Why there are two numeric types at all

`Decimal64` holds what a broker said, exactly: a 64-bit integer coefficient and a decimal
scale. `70000.50` is the coefficient `7000050` at scale `2`, and it stays that through a
round trip, a reconciliation, and a comparison. That property is not a nicety. An order
price is a value an exchange either accepts or rejects, a filled quantity is a number a
ledger must balance, and a price that is right to within one part in `2^52` is still wrong.

`math::Scalar` — `double` by default — holds what a computation needs. A standard deviation,
a regression coefficient, and a correlation are all irrational in general, and there is no
exact decimal representation to preserve. Trying to keep them exact would mean growing the
scale on every operation until the coefficient overflowed.

So the library uses both, and the discipline is that **they meet in exactly one place**.

## Convention 1: one gated boundary, and it is narrower outbound

Every conversion lives in `analytics/conversion.hpp`:

- `to_scalar(Decimal64)` is total. Every price and quantity a broker can report has a
  coefficient inside 53 bits, so `double` holds it exactly; the loss is theoretical here
  rather than practical.
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

## Convention 2: `missing()` is one value, and every reduction masks it

There is one representation for "no observation": `math::missing()`, a quiet `NaN`. It stands
for an indicator's warm-up region, a gap in a panel, a field a broker did not report, and a
window that did not hold enough data — all of them, so a caller learns one check,
`math::observed(value)`.

Every reduction in the layer skips it rather than propagating it. One absent print does not
blank a column. The consequence to keep in mind is that a window's divisor is the count of
values it actually saw, which is why `min_observations` counts observations rather than rows.

`missing()` rather than zero, always. Zero is a number that means something: a return of
zero is a claim the price did not move, a weight of zero is a decision to hold nothing. An
absent value is not a claim.

One thing `missing()` does **not** cover: an infinity. `NaN` is masked everywhere; `inf`
would survive every reduction and arrive at a comparison as the best result ever measured.
So the functions that could produce one refuse instead — a return over a zero prior price, a
Sortino ratio with no downside, a beta against a market that did not move.

## Convention 3: a result is as long as its input

Row `i` of every output describes row `i` of its input. A function that returned a shorter
series would hand the caller an alignment problem, and an off-by-one there is a lookahead
bug that no test of the function itself can catch: the strategy simply performs
impossibly well.

So the warm-up region is *present* and unobserved, not absent. `returns(prices)` has the
same length as `prices` with an unobserved row 0. `rolling_mean(values, 20)` has nineteen
unobserved rows before its first answer. A fitted regression's residuals carry `missing()`
on the rows the fit could not use.

## Convention 4: a window reports only when it has enough, and nothing is annualized for you

`min_observations` defaults to the whole window. Answering from a partially filled window is
the second most common source of lookahead bias after bar stamping, and it will not happen
here without being asked for in the call.

The same principle governs the conventions that differ between packages. Each function states
which one it implements, next to the arithmetic, because the differences change decisions
rather than decorating them:

| Function | Convention, and the other one |
| --- | --- |
| `ema` | `alpha = 2 / (span + 1)`, seeded on the first observed value |
| `rsi`, `atr` | Wilder's `1 / n` with Wilder's seed, not `2 / (n + 1)` |
| `rolling_stdev`, `covariance` | sample divisor `count - 1`, not the population `count` |
| `sortino` | shortfalls divided by **all** observations, not by the losing ones |
| `turnover` | one-way, `0.5 * sum(abs(delta w))`; doubling gives two-way |
| `sharpe` | per period. `annualize_ratio` is separate, and scales by `sqrt(periods)` |

Annualization is a separate named call for two reasons. The factor is the square root of the
period count, not the count — confusing them overstates a daily Sharpe ratio about sixteen
fold — and the period count is a calendar question (245 KRX trading days, 252 US, 12 months)
that no default answers correctly for everyone.

## Numerical choices that are not stylistic

Three places where the obvious formula is the wrong one, all of them tested at the level
where they break:

- **Trailing variance.** `rolling_stdev` takes two passes over each window rather than
  computing `E[x²] - E[x]²`. At price levels near `1e8` the one-pass form returns a variance
  of 2 where the answer is 1 — wrong by 41% with no diagnostic — because the two terms agree
  to more digits than a `double` has. It can even come out negative and become a `NaN` at the
  square root, with nothing to say arithmetic rather than data produced it.
- **Running variance.** `RunningMoments` uses Welford's method, because an accumulator cannot
  take the second pass. It never forms a sum of squares at all.
- **Running sums.** `RunningSum` uses Neumaier compensation. A live total grows far larger
  than the returns being added to it, and a naive `total += value` loses roughly a digit per
  factor of ten between them.

And two places where the honest answer is a flag rather than a number:

- **`ols` uses column-pivoting QR**, not the normal equations. Nearly collinear factor columns
  are the normal case in market data, the normal equations square the condition number, and
  the pivoting yields the rank — the only honest way to tell a caller its factors were not
  independent. `Regression::full_rank_` is worth checking: with it false the fitted values are
  still meaningful and the individual coefficients are not.
- **`covariance` deletes pairwise**, so its result need not be positive semi-definite. That is
  not a bug to be hidden: a panel of a few hundred instruments with scattered gaps may have no
  complete row at all, so listwise deletion would return nothing. Running
  `nearest_positive_definite` over the result is the expected next step. Note what clipping
  does not preserve — the diagonal — so a *correlation* matrix comes back with a diagonal
  slightly above one and needs rescaling if that matters.

## Where the numbers live

| Type | Holds | Storage |
| --- | --- | --- |
| `TimeIndex` | the instants rows correspond to | one `Instant` per row |
| `SeriesFrame` | one instrument, every bar field | column-major, rows are time |
| `Panel` | one field, many instruments, aligned | column-major, one column per instrument |
| `RollingWindow` | the trailing `n` observations a live loop feeds | `2n` rows, compacted amortized |

Column-major with time on the rows is load-bearing rather than a default taken: one series is
then contiguous, so `column()` hands out a `math::SeriesView` referencing the frame's own
memory and an indicator never walks a strided view of its input. It is also why the same
`sma` call serves a completed frame and a live window — both are one contiguous series, which
is what keeps a backtest and a live loop from computing different things.

## What is single-owner, and where parallelism goes

Every stateful type here — `SeriesFrame`, `Panel`, `RollingWindow`, every accumulator — is
owned by one caller and is not synchronized, the model the rest of the library follows. A
mutex inside `RollingWindow` would not help with the real problem anyway: reading a trailing
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
