#pragma once

#include <atomic>
#include <stdint.h>

struct VersionLockValue
{
    bool locked;
    uint64_t version;
    uint64_t lock; // locked | version (concatenated)
};

class VersionLock
{
private:
    std::atomic_uint64_t vlock{};

public:
    VersionLock() noexcept = default;
    VersionLock(const VersionLock &vl) noexcept;

    /**
     * @brief Tries to acquire the lock
     *
     * @return true
     * @return false
     */
    bool TryAcquire();

    /**
     * @brief Releases the lock
     *
     * @return true
     * @return false
     */
    bool Release();

    /**
     * @brief Atomicaly sets lock version and releases lock
     *
     * @param new_version
     * @return true
     * @return false
     */
    bool VersionedRelease(uint64_t new_version);

    /**
     * @brief Atomicaly samples lock and returns {lock bit, version} as VersionLockValue
     *
     * @return VersionLockValue
     */
    VersionLockValue Sample();

    // return true if CAS succeeds, false otherwise
    /**
     * @brief Returns true if Compare&Swap
     * succeeds, false otherwise
     *
     * @param do_lock
     * @param desired_version
     * @param compare_to
     * @return true
     * @return false
     */
    bool TryCompareAndSwap(bool do_lock, uint64_t desired_version, uint64_t compare_to);

    // concats lock bit and version into a uint64
    /**
     * @brief Concats lock bit and version into a uint64
     *
     * @param locked
     * @param version
     * @return uint64_t
     */
    uint64_t Serialize(bool locked, uint64_t version);

    /**
     * @brief Returns {lock bit, version} as VersionLockValue of given uint64
     *
     * @param serialized
     * @return VersionLockValue
     */
    VersionLockValue Parse(uint64_t serialized);
};