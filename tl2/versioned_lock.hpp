#pragma once

#include <atomic>
#include <climits>
#include <cstdint>

class VersionedLock
{
public:
  struct TimeStamp
  {
    bool locked;
    std::uint32_t version;
  };

private:
  static constexpr std::uint32_t LOCKED_MASK = 0x80000000;
  static constexpr std::uint32_t VERSION_MASK = ~LOCKED_MASK;

private:
  std::atomic_uint32_t counter{0};

public:

  TimeStamp Sample() const noexcept
  {
    auto val = counter.load();
    return TimeStamp{val && LOCKED_MASK, val & VERSION_MASK};
  }

  bool Validate(std::uint32_t rv) noexcept
  {
    auto current = counter.load();
    return !(current & LOCKED_MASK || (current & VERSION_MASK) > rv);
  }

  bool TryLock(std::uint32_t last) noexcept
  {
    auto current = counter.load();
    if (current & LOCKED_MASK || (current & VERSION_MASK) > last)
    {
      return false;
    }

    const auto desired = current | LOCKED_MASK;
    return counter.compare_exchange_strong(current, desired);
  }

  void Unlock() noexcept
  {
    auto val = counter.load();
    counter.store(val & VERSION_MASK);
  }

  void Unlock(std::uint32_t version) noexcept
  {
    counter.store(version & VERSION_MASK);
  }
};
