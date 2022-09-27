#pragma once

#include <atomic>
#include <stdint.h>

class VersionLock
{
private:
    std::atomic_uint64_t vlock{};

public:
    struct Value
    {
        bool locked;
        uint64_t version;
        uint64_t lock; // locked | version (concatenated)
    };

    VersionLock() noexcept = default;
    VersionLock(const VersionLock &vl) noexcept;

    /**
     * @brief Tries to acquire the lock
     *
     * @return true
     * @return false
     */
    bool acquire();

    /**
     * @brief Releases the lock
     *
     * @return true
     * @return false
     */
    bool release();

    /**
     * @brief Atomicaly sets lock version and releases lock
     *
     * @param new_version
     * @return true
     * @return false
     */
    bool versioned_release(uint64_t new_version);

    /**
     * @brief Atomicaly samples lock and returns {lock bit, version} as Value
     *
     * @return Value
     */
    Value sample();

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
    bool cmp_n_swap(bool do_lock, uint64_t desired_version, uint64_t compare_to);

    /**
     * @brief Concats lock bit and version into a uint64
     *
     * @param locked
     * @param version
     * @return uint64_t
     */
    uint64_t serialize(bool locked, uint64_t version);

    /**
     * @brief Returns {lock bit, version} as Value of given uint64
     *
     * @param serialized
     * @return Value
     */
    Value parse(uint64_t serialized);
};