#include "version_lock.hpp"

#include <iostream>

VersionLock::VersionLock(const VersionLock &vl) noexcept
{
    // TODO: is this well implemented?
    vlock = vl.vlock.load();
}

bool VersionLock::acquire()
{
    Value val = this->sample();
    if (val.locked)
    {
        return false;
    }

    return this->cmp_n_swap(true, val.version, val.lock);
}

bool VersionLock::release()
{
    Value val = this->sample();
    if (!val.locked)
    {
        std::cout << "[VersionLock\tRelease]: releasing unlocked lock" << std::endl;
        return false;
    }

    return this->cmp_n_swap(false, val.version, val.lock);
}

bool VersionLock::versioned_release(uint64_t new_version)
{
    Value val = this->sample();
    if (!val.locked)
    {
        std::cout << "[VersionLock\tVersionedRelease]: releasing unlocked lock" << std::endl;
        return false;
    }

    return this->cmp_n_swap(false, new_version, val.lock);
}

VersionLock::Value VersionLock::sample()
{
    uint64_t current = vlock.load();
    return parse(current);
}

bool VersionLock::cmp_n_swap(bool do_lock, uint64_t desired_version, uint64_t compare_to)
{
    uint64_t new_lock = serialize(do_lock, desired_version);
    return this->vlock.compare_exchange_strong(compare_to, new_lock);
}

uint64_t VersionLock::serialize(bool locked, uint64_t version)
{
    if ((version >> 63) == 1)
    {
        std::cout << "[VersionLock\tSerialize]: version overflow" << std::endl;
        throw -1;
    }

    if (locked)
    {
        return ((uint64_t)1 << 63) | version;
    }
    return version;
}

VersionLock::Value VersionLock::parse(uint64_t serialized)
{
    uint64_t version = (((uint64_t)1 << 63) - 1) & serialized;
    uint64_t locked_bit = serialized >> 63;
    return {locked_bit == 1, version, serialized};
}