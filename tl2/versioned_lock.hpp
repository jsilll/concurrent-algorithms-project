#pragma once

#include <atomic>
#include <cstdint>

#define LOCKED_MASK 0x8000000000000000
#define VERSION_MASK 0x7FFFFFFFFFFFFFFF

class VersionedLock
{
public:
  struct TimeStamp
  {
    bool locked;
    std::uint64_t version;
  };

private:
  std::atomic_uint64_t counter{0};

public:
  TimeStamp Sample() const noexcept
  {
    auto val = counter.load();
    return TimeStamp{static_cast<bool>(val & LOCKED_MASK), val & VERSION_MASK};
  }

  bool Validate(std::uint32_t rv) noexcept
  {
    auto current = counter.load();
    return !(current & LOCKED_MASK || (current & VERSION_MASK) > rv);
  }

  bool TryLock(std::uint32_t rv) noexcept
  {
    auto current = counter.load();

    if (current & LOCKED_MASK || (current & VERSION_MASK) > rv)
    {
      return false;
    }

    return counter.compare_exchange_strong(current, current | LOCKED_MASK);
  }

  void Unlock() noexcept
  {
    counter.store(counter.load() & VERSION_MASK);
  }

  void Unlock(std::uint64_t wv) noexcept
  {
    counter.store(wv);
  }
};
