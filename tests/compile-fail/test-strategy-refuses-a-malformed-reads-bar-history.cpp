// expect: reads_bar_history must be a public `static constexpr bool`
// variants: 1 2 3 4 5 6 7 8
// Deliberately does not compile. `strategy-refuses-a-malformed-reads-bar-history-<n>` builds this file and
// passes only when the diagnostic is the declaration's own: an int (1), a non-static member (2), a private
// one (3), a function (4), a static bool that is not const (5), a reference to a bool (6), a class type
// converting to bool whose `operator&` claims a `const bool*` (7), each through `ATL_ASSERT_STRATEGY`, and the
// int again through a runner (8). Read as absent, any of them would keep or drop the history against what
// was written.
#include <algotradelib/runner>

using namespace algotradelib;

inline constexpr bool no = false;

struct PolicyProxy {
  constexpr operator bool() const { return false; }
  constexpr const bool* operator&() const { return nullptr; }
};

struct Probe {
#if ATL_PROBE_VARIANT == 1 || ATL_PROBE_VARIANT == 8
  static constexpr int reads_bar_history = 0;
#elif ATL_PROBE_VARIANT == 2
  const bool reads_bar_history = false;
#elif ATL_PROBE_VARIANT == 3
private:
  static constexpr bool reads_bar_history = false;

public:
#elif ATL_PROBE_VARIANT == 4
  static constexpr bool reads_bar_history() { return false; }
#elif ATL_PROBE_VARIANT == 5
  static bool reads_bar_history;
#elif ATL_PROBE_VARIANT == 6
  static constexpr const bool& reads_bar_history = no;
#elif ATL_PROBE_VARIANT == 7
  static constexpr PolicyProxy reads_bar_history{};
#endif
  template <class Ctx> void on_bar(Ctx&, const strategy::BarEvent&) {}
};

#if ATL_PROBE_VARIANT == 8
bool records() { return runner::DryRunner<Probe>::records_bar_history; }
#else
ATL_ASSERT_STRATEGY(Probe, strategy::MinimalContext);
#endif
