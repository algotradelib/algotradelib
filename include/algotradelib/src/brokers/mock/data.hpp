#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace algotradelib {
namespace mock {

/**
 * @brief The mock vendor: records the caller gives, each released once the clock reaches the time it was
 *   published.
 *
 * It is a data poll as `runner::PollingLoop::set_data_poll` takes one: on the mock market's clock
 * (`mock::MarketClock`) one clock moves the market and the feed, and the loop stamps each record with the
 * reading at which it received it.
 *
 * @code
 * mock::DataFeed<Record> feed({{published_at, record}, ...});
 * loop.set_data_poll(feed); // a copy; std::ref(feed) to keep reading `remaining()`
 * @endcode
 *
 * @tparam T The record type, the strategy's `data_type`.
 */
template <typename T>
class DataFeed {
public:
  /** @brief A record and when it was published. */
  using Entry = std::pair<std::chrono::system_clock::time_point, T>;

  /**
   * @brief Builds a feed over records in any order; records published at one instant keep their order.
   * @param entries The records with their publication times.
   */
  explicit DataFeed(std::vector<Entry> entries) : entries_(std::move(entries)) {
    std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
      return a.first < b.first;
    });
  }

  /**
   * @brief Returns the records published at or before `now` and not returned before, in time order.
   * @param now The clock reading.
   * @return The records; empty when nothing is new. Never `std::nullopt`: the mock never fails a read.
   */
  std::optional<std::vector<T>> operator()(std::chrono::system_clock::time_point now) {
    std::vector<T> out;
    while (next_ < entries_.size() && !(now < entries_[next_].first)) {
      out.push_back(entries_[next_].second);
      ++next_;
    }
    return out;
  }

  /**
   * @brief Returns how many records are still to be released.
   * @return The count.
   */
  std::size_t remaining() const noexcept {
    return entries_.size() - next_;
  }

private:
  std::vector<Entry> entries_;
  std::size_t next_{0};
};

} // namespace mock
} // namespace algotradelib
