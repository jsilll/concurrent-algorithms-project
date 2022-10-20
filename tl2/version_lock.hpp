#pragma once

#include <atomic>
#include <cstdint>

struct VersionLockValue
{
  bool locked;
  uint64_t version;
  uint64_t lock;
};

class VersionLock
{
private:
  std::atomic_uint64_t vlock;

public:
  VersionLock() : vlock(0) {}
  VersionLock(const VersionLock &vl) { vlock = vl.vlock.load(); }

  bool TryAcquire()
  {
    VersionLockValue val = this->Sample();
    if (val.locked)
    {
      return false;
    }

    return this->TryCompareAndSwap(true, val.version, val.lock);
  }

  // releases lock
  bool Release()
  {
    VersionLockValue val = this->Sample();
    if (!val.locked)
    {
      return false;
    }

    return this->TryCompareAndSwap(false, val.version, val.lock);
  }

  // atomicaly sets lock version and releases lock
  bool VersionedRelease(uint64_t new_version)
  {
    VersionLockValue val = this->Sample();
    if (!val.locked)
    {
      return false;
    }

    return this->TryCompareAndSwap(false, new_version, val.lock);
  }

  // atomicaly samples lock and returns {lock bit, version} as VersionLockValue
  VersionLockValue Sample()
  {
    uint64_t current = vlock.load();
    return Parse(current);
  }

  // return true if CAS succeeds, false otherwise
  bool TryCompareAndSwap(bool do_lock, uint64_t desired_version,
                         uint64_t compare_to)
  {
    uint64_t new_lock = Serialize(do_lock, desired_version);
    return this->vlock.compare_exchange_strong(compare_to, new_lock);
  }

  // concats lock bit and version into a uint64
  uint64_t Serialize(bool locked, uint64_t version)
  {
    if ((version >> 63) == 1)
    {
      throw -1;
    }

    if (locked)
    {
      return ((uint64_t)1 << 63) | version;
    }
    return version;
  }

  // returns {lock bit, version} as VersionLockValue of given uint64
  VersionLockValue Parse(uint64_t serialized)
  {
    uint64_t version = (((uint64_t)1 << 63) - 1) & serialized;
    uint64_t locked_bit = serialized >> 63;
    return {locked_bit == 1, version, serialized};
  }
};