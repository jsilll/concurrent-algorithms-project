#include "spin_lock.hpp"

void SpinLock::Lock()
{
    bool expected = false;
    while (true)
    {
        if (locked_.compare_exchange_strong(expected, true))
        {
            break;
        }
    }
}

void SpinLock::Unlock()
{
    version_++;
    locked_.store(false);
}