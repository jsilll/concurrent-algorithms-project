#include "version_lock.hpp"

VersionLock::VersionLock(const VersionLock &vl)
{
    // TODO: is this well implemented?
    vlock = vl.vlock.load();
}

bool VersionLock::TryAcquire()
{
    VersionLockValue val = this->Sample();
    if (val.locked)
    {
        return false;
    }

    return this->TryCompareAndSwap(true, val.version, val.lock);
}

bool VersionLock::Release()
{
    VersionLockValue val = this->Sample();
    if (!val.locked)
    {
        // printf("[VersionLock\tRelease]: releasing unlocked lock\n");
        return false;
    }

    return this->TryCompareAndSwap(false, val.version, val.lock);
}

bool VersionLock::VersionedRelease(uint64_t new_version)
{
    VersionLockValue val = this->Sample();
    if (!val.locked)
    {
        // printf("[VersionLock\tVersionedRelease]: releasing unlocked lock\n");
        return false;
    }

    return this->TryCompareAndSwap(false, new_version, val.lock);
}

VersionLockValue VersionLock::Sample()
{
    uint64_t current = vlock.load();
    return Parse(current);
}

bool VersionLock::TryCompareAndSwap(bool do_lock, uint64_t desired_version,
                                    uint64_t compare_to)
{
    uint64_t new_lock = Serialize(do_lock, desired_version);
    return this->vlock.compare_exchange_strong(compare_to, new_lock);
}

uint64_t VersionLock::Serialize(bool locked, uint64_t version)
{
    if ((version >> 63) == 1)
    {
        // printf("[VersionLock\tSerialize]: version overflow\n");
        throw -1;
    }

    if (locked)
    {
        return ((uint64_t)1 << 63) | version;
    }
    return version;
}

VersionLockValue VersionLock::Parse(uint64_t serialized)
{
    uint64_t version = (((uint64_t)1 << 63) - 1) & serialized;
    uint64_t locked_bit = serialized >> 63;
    return {locked_bit == 1, version, serialized};
}