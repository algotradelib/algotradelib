#pragma once

#include "./scalar.hpp"

/**
 * @file
 * @brief The linear algebra types of the `math` module: `Matrix`, `Vector`, `SeriesView`,
 *   `MutableSeriesView`, `MatrixView` and `Index`.
 *
 * They come from Eigen, which needs a build configured with `ATL_USE_EIGEN=ON`; without it,
 * including this header is a compile error. `math/scalar.hpp` (`Scalar`, `missing()`,
 * `observed()`) needs no backend.
 */
// A new backend is a sibling header defining the same six names, plus one branch here.
#if defined(ATL_HAS_EIGEN)
#include "./eigen.hpp"
#else
#error \
  "The math module's linear algebra needs a backend. Configure the library with \
ATL_USE_EIGEN=ON, or link algotradelib::algotradelib from a build that was configured \
with it. Quote, order, and market-data headers do not require one, and neither does \
math/scalar.hpp."
#endif
