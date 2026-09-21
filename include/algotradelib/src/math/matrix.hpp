#pragma once

#include "./scalar.hpp"

/**
 * Selects the linear algebra backend the `math` module is built on.
 *
 * One backend is supported today and the selection exists because that is expected to
 * change: a consumer may prefer another library, or this one may carry its own
 * implementation. A new backend is a sibling header defining the same vocabulary —
 * `Matrix`, `Vector`, `SeriesView`, `MutableSeriesView`, `MatrixView`, `Index` — and one
 * more branch here. Nothing above this header changes.
 *
 * `math/scalar.hpp` needs no backend at all, so `Scalar`, `missing()`, and `observed()`
 * are available whether one is configured or not.
 */
#if defined(ATL_HAS_EIGEN)
#include "./eigen.hpp"
#else
#error \
  "The math module's linear algebra needs a backend. Configure the library with \
ATL_USE_EIGEN=ON, or link algotradelib::algotradelib from a build that was configured \
with it. Quote, order, and market-data headers do not require one, and neither does \
math/scalar.hpp."
#endif
