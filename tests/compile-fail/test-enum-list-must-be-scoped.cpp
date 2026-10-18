// expect: lists_every_value checks a list of a scoped enum's values
// Deliberately does not compile. `enum-list-must-be-scoped` builds this file and passes only when
// `lists_every_value` refuses an unscoped enum: its check converts the list's length to the enum, which is
// defined only for a type with a fixed underlying type, and an `enum class` always has one.
#include <algotradelib/core>

#include <array>

namespace probe {

enum Plain {
  FIRST,
  SECOND,
};

inline constexpr std::array<Plain, 2> plain_values{FIRST, SECOND};

static_assert(algotradelib::lists_every_value(plain_values, [](Plain value) { return value == FIRST || value == SECOND; }));

} // namespace probe
