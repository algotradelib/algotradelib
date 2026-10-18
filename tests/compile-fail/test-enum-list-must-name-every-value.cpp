// expect: probe_values must list every Probe
// Deliberately does not compile. `enum-list-must-name-every-value` builds this file and passes only when
// the diagnostic is the list's own `static_assert`: an enumerator added to an enum and left out of its list
// of values (`all_venues`, `all_halt_reasons`, ...) must fail to build, or a parser or a count derived from
// the list would silently skip it.
#include <algotradelib/core>

#include <array>
#include <string_view>

namespace probe {

enum class Probe {
  FIRST,
  SECOND,
  ADDED,
};

// Has a case for every value, as the switch flag requires.
constexpr std::string_view probe_name(Probe value) noexcept {
  switch (value) {
    case Probe::FIRST:
      return "first";
    case Probe::SECOND:
      return "second";
    case Probe::ADDED:
      return "added";
  }
  return "";
}

// The list was not extended.
inline constexpr std::array<Probe, 2> probe_values{Probe::FIRST, Probe::SECOND};

static_assert(
  algotradelib::lists_every_value(probe_values, [](Probe value) { return !probe_name(value).empty(); }),
  "probe_values must list every Probe"
);

} // namespace probe
