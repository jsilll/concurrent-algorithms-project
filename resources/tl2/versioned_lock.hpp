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
    auto current = counter.load();
    return TimeStamp{IsLocked(current), Version(current)};
  }

  bool Validate(std::uint32_t rv) noexcept
  {
    auto current = counter.load();
    return !(IsLocked(current) || Version(current) > rv);
  }

  bool TryLock(std::uint32_t rv) noexcept
  {
    auto current = counter.load();

    if (IsLocked(current) || Version(current) > rv)
    {
      return false;
    }

    return counter.compare_exchange_strong(current, Lock(current));
  }

  void Unlock() noexcept
  {
    counter.store(Version(counter.load()));
  }

  void Unlock(std::uint64_t wv) noexcept
  {
    counter.store(wv);
  }

private:
  static inline std::uint64_t Lock(std::uint64_t val) noexcept
  {
    return val | LOCKED_MASK;
  }

  static inline bool IsLocked(std::uint64_t val) noexcept
  {
    return val & LOCKED_MASK;
  }

  static inline std::uint64_t Version(std::uint64_t val) noexcept
  {
    return val & VERSION_MASK;
  }
};
