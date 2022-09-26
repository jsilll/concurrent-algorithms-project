#include "memory.hpp"

#include <cstring>
#include <iostream>

WordLock &SharedRegion::operator[](uintptr_t addr)
{
    return mem[addr >> 32];
}

/**
 * @brief Releases the lock set of
 * the first i words
 *
 * @param reg
 * @param i
 */
void SharedRegion::release_lock_set(uint32_t i, Transaction transaction)
{
    if (i == 0)
    {
        return;
    }

    for (const auto &target_src : transaction.write_set)
    {
        WordLock &wl = (*this)[target_src.first];
        wl.vlock.release();
        if (i <= 1)
        {
            break;
        }

        i--;
    }
}

int SharedRegion::try_acquire_sets(uint *i, Transaction transaction)
{
    *i = 0;
    for (const auto &target_src : transaction.write_set)
    {
        WordLock &wl = (*this)[target_src.first];
        bool acquired = wl.vlock.acquire();
        if (!acquired)
        {
            release_lock_set(*i, transaction);
            return false;
        }

        *i = *i + 1;
    }

    return true;
}

bool SharedRegion::validate_readset(Transaction transaction)
{
    for (const auto word : transaction.read_set)
    {
        WordLock &wl = (*this)[(uintptr_t)word];
        VersionLock::Value val = wl.vlock.sample();
        if ((val.locked) || val.version > transaction.rv)
        {
            return false;
        }
    }

    return true;
}

bool SharedRegion::commit(Transaction transaction)
{
    for (const auto target_src : transaction.write_set)
    {
        WordLock &wl = (*this)[target_src.first];
        memcpy(&wl.word, target_src.second, align);
        if (!wl.vlock.versioned_release(transaction.wv))
        {
            std::cout << "[Commit]: versioned_release failed" << std::endl;
            transaction = {};
            return false;
        }
    }

    transaction = {};
    return true;
}