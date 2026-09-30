# algotradelib

<!-- multilingual: start -->
[![en](https://img.shields.io/badge/lang-en-red.svg?style=plastic)](README.md)
[![ko-kr](https://img.shields.io/badge/lang-ko--kr-green.svg?style=plastic)](README.ko-kr.md)
<!-- multilingual: end -->

<!-- badges: start -->
![License](https://img.shields.io/badge/license-MIT-blue.svg?style=plastic)
[![Lifecycle: experimental](https://img.shields.io/badge/lifecycle-experimental-orange.svg?style=plastic)](https://lifecycle.r-lib.org/articles/stages.html#experimental)
<!-- badges: end -->

C++17 building blocks for event-driven trading through Korean broker APIs — currently Korea
Investment & Securities (KIS), Kiwoom Securities and Toss Securities.

Header-only, consumed through CMake. A strategy is a plain struct that performs no I/O and names no
broker, so one source is replayed against history, dry-run with no credentials, and then run live
without being rewritten — against KIS, Toss or Kiwoom, where a runner needs a few statements from you
first (see "Brokers and accounts"). Selecting a broker is choosing a type parameter.

> **This library sends real orders to real accounts, and has not yet been run end to end against a
> broker account**: every broker behaviour it relies on was read from the brokers' specifications. It is
> in development (`0.0.0.9000`, no release yet). Rehearse on a demo account first, and read "Brokers and
> accounts" and the Disclaimer below before you do.

## Architecture

![How the modules depend on one another](doxygen/figures/architecture.png)

Blue is what you bring; orange carries the transport. Green reaches no transport header and
instantiates no adapter or TLS code — asserted at compile time, not by convention, so there is no
instantiated code path from a backtest to a market. It does **not** mean libssl is off the link line; the package exports one interface target for every
consumer, as the prerequisites below explain. The
[architecture vignette](doxygen/vignettes/architecture.md) has the full picture, the dependency rule
underneath it, and what that compile-time claim does and does not cover.

## Prerequisites

| Dependency | Floor | Needed for |
| --- | --- | --- |
| A C++17 compiler | — | everything |
| CMake | 3.10 for the library, **3.16** for the project templates below | everything |
| Boost | 1.87, component `json` | everything |
| OpenSSL | 3.0 | everything (see below) |
| Eigen | 3.4 | the analytics layer, on by default |

Three of those floors are worth a sentence each.

**The two CMake numbers are not a typo.** The library itself configures under 3.10, so an existing
project on an old CMake can consume it. The consumer `CMakeLists.txt` shown below — and the one the
scaffold generates — ask for 3.16, which is simply a saner floor to start a new project on. Lower it
to 3.10 in your own project if you need to; nothing here requires the newer version.

**OpenSSL's floor is an API minimum, not a recommendation.** The headers call OpenSSL directly only
to set the TLS server name, which OpenSSL has offered since long before 3.0; everything else goes
through Boost.Asio's `ssl` layer. So the build accepts 3.0. But upstream support for 3.0 ended in
September 2026: for anything that trades, use a supported line (3.5 LTS or newer), or a distribution
package that still receives security fixes. The only version this library has been built and tested
against is 3.6.x.

**Boost and OpenSSL are required even for a backtest that touches no network.** The library exports
one interface target for every consumer, so an offline program still links `libssl`, `libcrypto` and
`boost_json`. That is measured, not assumed — inspecting a dry-run binary's dynamic dependencies
shows them. What is genuinely true is the narrower claim above: a dry run or a replay instantiates
no adapter and no transport code, asserted at compile time by `tests/test-runner-isolation.cpp`.

## Install

### The fastest start

```sh
tools/atl-new-project.sh my-alpha
```

That writes a complete project — `CMakeLists.txt`, `src/main.cpp`, `README.md`, `.gitignore` — whose
`main.cpp` is [`examples/example-backtest-compare.cpp`](examples/example-backtest-compare.cpp): your
own strategy replayed beside `BuyAndHold`, `SmaCrossover` and `DonchianBreakout` on identical bars.

```
  strategy                return     Sharpe     max dd  fills
  -----------------------------------------------------------
  MyStrategy(5)         -10.22 %     -0.375    13.11 %      2
  BuyAndHold             10.89 %      0.250    13.35 %      1
  SmaCrossover(3,8)       9.67 %      0.308     4.93 %      3
  DonchianBreakout(5)    12.17 %      0.383     4.82 %      3
```

It does not touch your git index — adding a submodule writes to your repository, and a script that
does that is one you cannot run twice — so it prints the command instead. `--mode package` generates
the installed-library variant, and `--help` lists the rest.

The generated project is built and run by this library's own test suite
(`tests/scaffold-smoke.sh`), which is the only reason to trust that the instructions below still
work.

### As a submodule

```sh
git submodule add https://github.com/ygeunkim/algotradelib.git libs/algotradelib
git submodule update --init --recursive
```

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_alpha LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_subdirectory(libs/algotradelib)

add_executable(my_alpha src/main.cpp)
target_link_libraries(my_alpha PRIVATE algotradelib::algotradelib)
```

That is the whole of it. `ATL_BUILD_EXAMPLES` and `ATL_BUILD_TESTS` default to `OFF`, so a submodule
build compiles nothing but your own sources against the headers.

### As an installed package

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build --target install
```

```cmake
find_package(algotradelib REQUIRED)
target_link_libraries(my_alpha PRIVATE algotradelib::algotradelib)
```

Configure your project with `-DCMAKE_PREFIX_PATH=/your/prefix` if that is not a location CMake
already searches. The installed package config records whether the library was built with a backend,
so a consumer is never asked to restate how it was configured and cannot get it wrong.

### Checking the version

`<algotradelib/version>` (also included by `<algotradelib/core>`) defines `ATL_VERSION_MAJOR`,
`ATL_VERSION_MINOR`, `ATL_VERSION_PATCH` and `ATL_VERSION_DEV`, and `ATL_VERSION_STRING`. The header is
the only place the version is written; CMake reads it, so the package version always agrees.

```cpp
#include <algotradelib/version>

#if ATL_VERSION >= ATL_VERSION_ENCODE(0, 1, 0)
// something 0.1.0 added
#endif
```

`ATL_VERSION` encodes the release only, as `BOOST_VERSION` does. `ATL_VERSION_DEV` is 0 for a release
and otherwise marks unreleased work after it: today's `0.0.0.9000` is the development version before
any release.

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `ATL_USE_EIGEN` | `ON` | the analytics backend; see below |
| `ATL_BUILD_EXAMPLES` | `OFF` | the `example-*` programs in `examples/` |
| `ATL_BUILD_TESTS` | `OFF` | the Catch2 suite in `tests/` |
| `ATL_ENABLE_COVERAGE` | `OFF` | coverage instrumentation for that suite |

`ATL_USE_EIGEN=OFF` does not make analytics slower — it removes them.
`<algotradelib/analytics>` becomes a deliberate `#error` naming the option to set,
`backtest::summarize` and `backtest::Report` are gone, and `<algotradelib/vanilla>` ships only
`BuyAndHold` and `TimedLiquidation`, since every other vanilla computes an indicator. The replay
engine itself runs either way, so a build without a backend can still execute a strategy; it just
cannot score one.

To build and run the library's own checks:

```sh
cmake -S . -B build -DATL_BUILD_EXAMPLES=ON -DATL_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build/tests --output-on-failure
```

## Using it

A strategy receives events and emits intents. It holds no session, so this same struct runs in all
three configurations below.

```cpp
#include <algotradelib/strategy>

using namespace algotradelib;

struct MyStrategy {
  template <class Ctx>
  void on_bar(Ctx& ctx, const strategy::BarEvent& bar) {
    strategy::TargetPosition target;
    target.instrument_ = bar.instrument_;
    target.target_quantity_ = Decimal64::parse("10");
    ctx.submit(target);          // state a position, not an order
  }
};
```

**Replay it against history.** `Backtester` is `Runner` with a replay clock and a fill model; the
fill never comes from the bar that produced the order, so the lookahead discipline is structural.

```cpp
#include <algotradelib/backtest>

backtest::Backtester<MyStrategy> engine(MyStrategy{}, config, fills);
for (const auto& bar : series.bars_) {
  engine.feed(strategy::make_bar_event(series.instrument_, series.interval_, series.stamp_, bar));
}
engine.finish();
const auto report = backtest::summarize(engine.result(), engine.fills(), periods_per_year);
```

**Run it live.** Copy [`examples/example-live-program.cpp`](examples/example-live-program.cpp) and
[`examples/live_program.hpp`](examples/live_program.hpp), and replace the strategy — `BuyAndHold` of one
share — with yours. It is the production path, for KIS and Toss (on Kiwoom it observes, and places
nothing until you state what its holdings inquiry covers and lift the market-phase limit, since Kiwoom
publishes no calendar):

```sh
export KIS_APP_KEY=... KIS_APP_SECRET=... KIS_CANO=... KIS_DEMO=1
./build/bin/example-live-program kis 005930           # observe: real data, nothing can be sent
./build/bin/example-live-program kis 005930 --live    # trade, through a journal
```

Without `--live` the runner is bound with `live::bind_observer`, which has nothing to send through.
With it, `live_program::trade` does what a restartable run needs, in the order it needs it:

```cpp
runner::FileJournal journal(settings.journal_path_, settings.journal_scope_);  // one per account
if (!journal.open()) { /* a runner that cannot record what it sends does not send */ }

auto bound = live::bind_journaled(
  session, journal, std::move(strategy), settings.limits_,
  live::default_series_capacity, settings.default_route_  // where unrouted orders go; Kiwoom: KRX
);
const auto recovery = runner::recover_runner(bound, journal);  // printed; a global halt stops here
// ... reference data for each instrument, then:
live::SessionSource<Session, runner::FileJournal> source(session, settings.universe_);
source.set_journal(journal);                    // lets the listings it reads resolve the journal

runner::PollingLoop<decltype(bound), decltype(source), Clock> loop(bound, source, settings.schedule_);
loop.set_maximum_portfolio_age(settings.maximum_portfolio_age_);        // every age stated
loop.set_maximum_price_limits_age(settings.maximum_price_limits_age_);
loop.set_control_file(runner::FileStopChannel(settings.control_file_)); // echo withdraw > it
const auto report = loop.run(stop);             // stop also comes from SIGINT / SIGTERM
// ... the report, its notes, and closing_complete()
```

The journal is named after the account and scoped to broker, demo or live, and account, so a demo
journal refuses to continue as a live one. The exit code tells the endings apart: 0 clean, 1 a closing
pass that did not get everything out, 2 journal refused, 3 recovery halted (the program prints the
manual procedure), 4 reference data unreadable, 5 polling abandoned, 6 a local failure during the run
(usually a journal write; read the broker's order list), 7 the account could not be settled (a Toss
key reaching several accounts needs `--toss-account`). Within an iteration the loop polls the order
list, then holdings, then bars, of whichever are due, so the strategy decides on the bars read at start against an account already read.
The buying-power guard is left off, its default. **On Kiwoom it places nothing as shipped** — see "Brokers and accounts" below.
[Recovering after a crash](doxygen/vignettes/recovering-after-a-crash.md) explains each step and the
verdicts `recover_runner` returns.

Bars reach a replay either from a broker (`example-backtest-broker` fetches real history through KIS
or Toss) or from `Bar` values you construct yourself. This library reads no files on purpose: parsing
somebody's CSV is not a thing it can do honestly for everyone, and it belongs in your project where
its assumptions are yours.

Seven of the programs in `examples/` need no credentials, no network and no broker at all, and are
the fastest way to see each layer work: `example-strategy-dryrun`, `example-backtest-offline`,
`example-backtest-compare`, `example-live-loop`, `example-order-journal`,
`example-analytics-offline` and `example-rate-limit-stats`. `example-kis-place-order` is offline too
— it constructs and prints an `OrderRequest` and can send nothing. The rest read credentials from
the environment; the [broker clients](doxygen/vignettes/broker-clients.md) page lists which.

## Brokers and accounts

| Broker | State | Paper trading |
| --- | --- | --- |
| KIS | market data, calendar, reference data, holdings, buying power, orders (including stop-limit), order history | default (`KIS_DEMO=0` for live) |
| Kiwoom | market data, reference data, buying power, orders (including stop-limit) and order history through the session; **no calendar**; **holdings refuse** until you state what its `kt00018` inquiry covers (below); **a strategy runner needs a stated route** (below) | default (`KIWOOM_DEMO=0` for live) |
| Toss | market data, calendar, reference data, holdings, buying power, orders (no stop-limit), order history | **none** |

**KIS and Kiwoom are demo unless you say live.** Live — the one choice that moves real money — has
to be written down: `KIS_DEMO=0` / `KIWOOM_DEMO=0`, `"demo": false` in a credential file, or
`is_demo_ = false` in code. Anything unstated uses the broker's 모의투자 (paper-trading) endpoints, where
live keys are refused, so a forgotten setting fails instead of trading. A value that is set must be
exactly `1` or `0`; anything else refuses to load.

**Toss has no paper-trading environment.** A Toss program with credentials trades real money; rehearse
a strategy with a replay or a dry run, or on a KIS or Kiwoom demo account, first.

**Kiwoom's holdings need a statement from you.** `kt00018`, the inquiry holdings come from, takes a
venue and does not say whether it filters positions by venue or values them by it. Rather than guess,
the adapter refuses holdings — and so every sellable and position check — until
`KiwoomConfig::holdings_venue_` and `holdings_position_scope_` are set (and
`holdings_sellable_scope_` for checked sells). The members' documentation explains each choice.

**A Kiwoom strategy runner needs its route stated.** Kiwoom requires every order to name its venue,
and the orders a strategy produces name none, so bind the runner with a default route —
`live::bind_journaled(session, journal, strategy, limits, live::default_series_capacity, Venue::KRX)`,
or `Venue::NXT`, or `Venue::COMBINED` for Kiwoom's SOR, which every Kiwoom order endpoint documents.
Prefer KRX or NXT for now: Kiwoom's working-order listing marks a SOR order with a separate `sor_yn`
flag, which the adapter reads so that recovery after a crash recognises the order, but neither that
flag nor whether a Kiwoom demo account accepts SOR has been seen on an account (see
`SessionExecutor::default_route`). Without a route the adapter refuses each order before sending it. `example-live-program` states KRX. Kiwoom also publishes no trading calendar, so a
runner's market phase stays unknown and the default limits open no position.

**KIS: own-account customers only.** The library sends `custtype P` (일반), which covers individual
*and* corporate customers trading their own account. KIS's partner program for licensed firms serving
third parties (`custtype B`, with `personalseckey`, per-member `phone_number` / `ip_addr` and `gt_uid`)
is not supported.

**Order routing across both Korean books is opt-in.** At KIS an order that names no venue routes to
KRX; `Venue::COMBINED` asks for SOR, and a runner opts in by binding with `Venue::COMBINED` as its
default route. That is not the default because a KIS demo account refuses SOR, and a default that
fails on demo would stop a demo run rehearsing the live one. As at Kiwoom, prefer KRX or NXT until SOR
has been observed: whether KIS lists a SOR order back as SOR or as the venue it rested at is unknown,
and recovery after a crash depends on it (see `SessionExecutor::default_route`). A Kiwoom order must
name its venue. Toss offers no choice.

## Documentation

The [user guide](doxygen/vignettes/guide.md) is eleven pages in the order they are worth reading:

| | Page | What it answers |
| --- | --- | --- |
| 1 | [Architecture](doxygen/vignettes/architecture.md) | what the pieces are, and why the strategy holds no session |
| 2 | [Writing a strategy](doxygen/vignettes/writing-a-strategy.md) | the handlers, warm-up, prices, reaching a broker-specific feature |
| 3 | [Binding a broker](doxygen/vignettes/binding-a-broker.md) | the limits, how finely a target is chased, what killing the process does |
| 4 | [Market data](doxygen/vignettes/market-data.md) | bars and paging, the stamp conventions, the price grid, the calendar |
| 5 | [Analytics conventions](doxygen/vignettes/analytics-conventions.md) | two numeric types, two kinds of absence, the smoothing constants |
| 6 | [Reading a backtest honestly](doxygen/vignettes/reading-a-backtest.md) | what the fill model assumes, and what to distrust first |
| 7 | [Running a strategy live](doxygen/vignettes/running-a-strategy-live.md) | the three stops, the gates, the supervisor grace period |
| 8 | [Recovering after a crash](doxygen/vignettes/recovering-after-a-crash.md) | the journal, the five verdicts, reading them under pressure |
| 9 | [The broker port](doxygen/vignettes/the-broker-port.md) | `BrokerTraits`, `BrokerCaps`, order identity — read this to add a broker |
| 10 | [Talking to a broker directly](doxygen/vignettes/broker-clients.md) | the KIS and Toss clients, and the order-safety rules |
| 11 | [Transport and request pacing](doxygen/vignettes/transport-and-pacing.md) | the pacer, the traffic classes, how each broker reports a refusal |

The same pages, with the API reference, build into HTML with Doxygen, in a build directory configured
as in "Build options":

```sh
cmake --build build --target docs      # doxygen/docs/html/index.html, stamped with the version
```

The target exists when CMake finds Doxygen and this library is the top-level project; a project that
adds it with `add_subdirectory` does not get it. `doxygen doxygen/Doxyfile`, run from the repository
root, builds the same pages without the version.

## Disclaimer

algotradelib is released under the MIT license; see [LICENSE](LICENSE). The software is provided as
is, without warranty of any kind, and the authors are not liable for any loss, including trading
losses, arising from its use. Nothing in this repository is investment advice.
