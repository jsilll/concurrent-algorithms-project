#pragma once

#include <atomic>
#include <map>
#include <unordered_set>
#include <vector>

#include "version_lock.hpp"

struct target_src
{
    uintptr_t target;
    void *src;
};

struct Transaction
{
    std::unordered_set<void *> read_set;   // set of read words
    std::map<uintptr_t, void *> write_set; // target word -> src word
    uint64_t rv;                           // read-version
    uint64_t wv;                           // write-version
    bool read_only = false;
};

static thread_local Transaction transaction;

struct WordLock
{
    VersionLock vlock;
    uint64_t word = 0;
};

/**
 * @brief Shared Memory Region
 *
 */
struct region
{
    size_t size;  // Size of the non-deallocable memory segment (in bytes)
    size_t align; // Size of a word in the shared memory region (in bytes)
    std::atomic_uint64_t seg_cnt = 2;
    std::vector<std::vector<WordLock>> mem;
    region(size_t size, size_t align) : size(size), align(align), mem(500, std::vector<WordLock>(1500)) {}
};