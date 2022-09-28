#pragma once

#include <atomic>
#include <map>
#include <new>
#include <unordered_set>
#include <vector>

#include "version_lock.hpp"

/**
 * @brief TargetSrc
 *
 */
struct TargetSrc
{
    void *src{};
    uintptr_t target{};
};

/**
 * @brief Transaction
 *
 */
struct Transaction
{
    bool is_ro{};                          // Read Only
    uint64_t rv{};                         // Read-Version
    uint64_t wv{};                         // Write-Version
    std::unordered_set<void *> read_set;   // Set Of Read Words
    std::map<uintptr_t, void *> write_set; // Target Word -> Src Word
};

/**
 * @brief WordLock
 *
 */
struct WordLock
{
    uint64_t word{};
    VersionLock vlock{};
};

/**
 * @brief Shared Memory Region
 *
 */
struct SharedRegion
{
    void *fixed_segment{};           // Start of the non-deallocable memory segment
    size_t size{};                   // Size of the non-deallocable memory segment (in bytes)
    size_t align{};                  // Size of a word in the shared memory SharedRegion (in bytes)
    std::vector<WordLock> mem;       // Memory
    std::atomic_uint64_t seg_cnt{2}; // Number of Segments ??

    /**
     * @brief Constructs a new Shared Region struct
     *
     * @param size
     * @param align
     */
    SharedRegion(size_t size, size_t align) : size(size), align(align){};

    /**
     * @brief Gets a word from the shared region
     *
     * @param addr
     * @return WordLock&
     */
    WordLock &operator[](uintptr_t addr);

    /**
     * @brief Releases the lock set of
     * the first i words
     *
     * @param reg
     * @param i
     */
    void release_lock_set(uint32_t i, Transaction transaction);

    /**
     * @brief Tries and acquires the sets
     *
     * @param i
     * @param transaction
     * @return int
     */
    int try_acquire_sets(uint *i, Transaction transaction);

    /**
     * @brief Validates the readset
     *
     * @param transaction
     * @return true
     * @return false
     */
    bool validate_readset(Transaction transaction);

    /**
     * @brief Commits a transaction
     *
     * @param transaction
     * @return true
     * @return false
     */
    bool commit(Transaction transaction);
};