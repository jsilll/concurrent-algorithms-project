#pragma once

#include <atomic>
#include <climits>
#include <cstdint>

class VersionedLock
{
public:
  using Timestamp = std::uint_fast32_t;

  [[nodiscard]] Timestamp version() const noexcept
  {
    return counter.load(std::memory_order_acquire) & VERSION_MASK;
  }

  [[nodiscard]] bool locked() const noexcept
  {
    return counter.load(std::memory_order_acquire) & LOCKED_MASK;
  }

  [[nodiscard]] bool validate(Timestamp last_seen) noexcept
  {
    auto current = counter.load(std::memory_order_acquire);
    return !(current & LOCKED_MASK || (current & VERSION_MASK) > last_seen);
  }

  [[nodiscard]] bool try_lock(Timestamp last_seen) noexcept
  {
    auto current = counter.load(std::memory_order_acquire);
    if (current & LOCKED_MASK || (current & VERSION_MASK) > last_seen)
    {
      return false;
    }
    const auto desired = current | LOCKED_MASK;
    return counter.compare_exchange_strong(
        current, desired, std::memory_order_release, std::memory_order_relaxed);
  }

  void unlock() noexcept
  {
    counter.store(version(), std::memory_order_release);
  }

  void unlock(Timestamp new_version) noexcept
  {
    counter.store(new_version, std::memory_order_release);
  }

private:
  static inline Timestamp LOCKED_MASK = Timestamp(1)
                                        << (CHAR_BIT * sizeof(Timestamp) - 1);
  static inline Timestamp VERSION_MASK = ~LOCKED_MASK;

  std::atomic<Timestamp> counter{0};
};
