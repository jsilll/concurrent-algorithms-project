#pragma once

#include <atomic>
#include <cstdint>

class VersionLock
{
public:
  struct Value
  {
    bool locked;
    uint64_t version;
  };

private:
  std::atomic_bool locked{false};
  std::atomic_uint64_t version{0};

public:
  inline VersionLock() noexcept = default;

  inline VersionLock(const VersionLock &lock)
  {
    auto val = lock.Sample();
    locked.store(val.locked);
    version.store(val.version);
  }

public:
  inline Value Sample() const noexcept
  {
    return Value{locked.load(), version.load()};
  }

  inline bool TryAcquire() noexcept
  {
    bool expected = false;
    return locked.compare_exchange_strong(expected, true);
  }

  inline void Release() noexcept
  {
    locked.store(false);
  }

  inline void VersionedRelease(uint64_t v) noexcept
  {
    version.store(v);
    locked.store(false);
  }
};