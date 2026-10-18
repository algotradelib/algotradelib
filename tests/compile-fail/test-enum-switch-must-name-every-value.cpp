// expect: error: enumeration value 'ADDED' not handled in switch
// Deliberately does not compile. `enum-switch-must-name-every-value` builds this file and passes only when
// the compiler refuses a `switch` that misses an enumerator. The library's switches over its enums have no
// `default:`, and its test and example builds turn the missing case into an error (`-Werror=switch` in
// tests/CMakeLists.txt); this probe fails if that flag is lost, because the pattern
// requires an error, not the warning the same text would be without it.
#include <string_view>

namespace probe {

enum class Probe {
  FIRST,
  SECOND,
  ADDED,
};

// `ADDED` was added to the enum and not here.
constexpr std::string_view probe_name(Probe value) noexcept {
  switch (value) {
    case Probe::FIRST:
      return "first";
    case Probe::SECOND:
      return "second";
  }
  return "";
}

static_assert(probe_name(Probe::FIRST) == "first");

} // namespace probe
