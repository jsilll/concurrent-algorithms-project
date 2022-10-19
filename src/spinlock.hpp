#pragma once

#include <atomic>

class SpinLock
{
public:
  inline bool try_lock() noexcept
  {
    return !flag.test_and_set(std::memory_order_acquire);
  }

  inline void lock() noexcept
  {
    while (!try_lock())
    {
      // spin;
    }
  };

  inline void unlock() noexcept { flag.clear(std::memory_order_release); }

private:
  std::atomic_flag flag = ATOMIC_FLAG_INIT;
};
