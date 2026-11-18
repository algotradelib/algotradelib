#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

namespace algotradelib {
namespace analytics {

/**
 * @file
 * @brief Block bootstrap indices that are the same on every platform for a seed and a path.
 */

/** @brief How a block bootstrap draws its blocks of consecutive returns. */
enum class BlockScheme {
  /** @brief Blocks of `block` returns that never wrap past the last. */
  MOVING,
  /** @brief Blocks of `block` returns that wrap from the last to the first. */
  CIRCULAR,
  /** @brief Wrapping blocks whose lengths are geometric with mean `block` (Politis and Romano). */
  STATIONARY,
};

/**
 * @brief Draws the steps of one bootstrap path: entry `k` names the step from close `j` to close `j + 1`, as `j`,
 *   the path takes at step `k`.
 *
 * Path `path` of `seed` comes from its own `std::mt19937_64`, seeded by `std::seed_seq` with the low and high 32 bits
 * of `seed` and then of `path`, so it is reproducible alone, on any thread and in any order. Draws use only the
 * engine's integers (no standard distribution, whose algorithm the standard leaves open), so a path is the same on
 * every standard library. A stationary block restarts with probability exactly `1 / block`; `block` 1 is the plain
 * bootstrap.
 *
 * @param count The number of returns (closes minus one); the path has as many entries, each below it.
 * @param scheme How blocks are drawn.
 * @param block The block length, or the mean block length for `STATIONARY`.
 * @param seed The seed.
 * @param path The path's number.
 * @return The indices.
 * @throws std::invalid_argument if `count` or `block` is zero, or `block` exceeds `count` for `MOVING` or
 *   `CIRCULAR`.
 */
inline std::vector<std::size_t> bootstrap_indices(
  std::size_t count, BlockScheme scheme, std::size_t block, std::uint64_t seed, std::uint64_t path
) {
  if (count == 0 || block == 0 || (scheme != BlockScheme::STATIONARY && block > count)) {
    throw std::invalid_argument("bootstrap_indices needs a positive count and block, and a block within count");
  }
  std::seed_seq seq{
    static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32), static_cast<std::uint32_t>(path),
    static_cast<std::uint32_t>(path >> 32)
  };
  std::mt19937_64 engine(seq);
  // Unbiased: values below `threshold` are the incomplete last run of `n` and are drawn again.
  const auto below = [&engine](std::uint64_t n) {
    const std::uint64_t threshold = (std::uint64_t{0} - n) % n;
    for (;;) {
      const std::uint64_t x = engine();
      if (x >= threshold) {
        return static_cast<std::size_t>(x % n);
      }
    }
  };
  std::vector<std::size_t> out;
  out.reserve(count);
  if (scheme == BlockScheme::STATIONARY) {
    std::size_t at = below(count);
    out.push_back(at);
    while (out.size() < count) {
      at = below(block) == 0 ? below(count) : (at + 1) % count;
      out.push_back(at);
    }
    return out;
  }
  while (out.size() < count) {
    const std::size_t start = scheme == BlockScheme::MOVING ? below(count - block + 1) : below(count);
    for (std::size_t step = 0; step < block && out.size() < count; ++step) {
      out.push_back((start + step) % count);
    }
  }
  return out;
}

} // namespace analytics
} // namespace algotradelib
