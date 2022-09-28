#include "macros.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string.h>
#include <tm.hpp>
#include <unordered_set>
#include <vector>

static std::atomic_uint version_clock = 0; // global version clock

WordLock &getWordLock(struct SharedRegion *reg, uintptr_t addr)
{
    return reg->mem[addr >> 32][((addr << 32) >> 32) / reg->align];
}

void reset_transaction()
{
    transaction.rv = 0;
    transaction.read_only = false;
    for (const auto &ptr : transaction.write_set)
    {
        free(ptr.second);
    }
    transaction.write_set.clear();
    transaction.read_set.clear();
}

shared_t tm_create(size_t size, size_t align) noexcept
{
    SharedRegion *region = new struct SharedRegion(size, align);
    if (unlikely(!region))
        return invalid_shared;
    return region;
}

void tm_destroy(shared_t shared) noexcept
{
    struct SharedRegion *reg = (struct SharedRegion *)shared;
    delete reg;
}

void *tm_start([[maybe_unused]] shared_t shared) noexcept
{
    return (void *)((uint64_t)1 << 32);
}

size_t tm_size(shared_t shared) noexcept
{
    return ((struct SharedRegion *)shared)->size;
}

size_t tm_align(shared_t shared) noexcept
{
    return ((struct SharedRegion *)shared)->align;
}

tx_t tm_begin([[maybe_unused]] shared_t shared, bool is_ro) noexcept
{
    transaction.rv = version_clock.load();
    transaction.read_only = is_ro;
    return (uintptr_t)&transaction;
}

bool tm_write(shared_t shared, [[maybe_unused]] tx_t tx, void const *source, size_t size, void *target) noexcept
{
    struct SharedRegion *reg = (struct SharedRegion *)shared;

    for (size_t i = 0; i < size / reg->align; i++)
    {
        uintptr_t target_word = (uintptr_t)target + reg->align * i;    // shared
        void *src_word = (void *)((uintptr_t)source + reg->align * i); // private
        void *src_copy = malloc(reg->align);                           // be sure to free this
        memcpy(src_copy, src_word, reg->align);
        transaction.write_set[target_word] = src_copy; // target->src
    }

    return true;
}

bool tm_read(shared_t shared, [[maybe_unused]] tx_t tx, void const *source, size_t size, void *target) noexcept
{
    struct SharedRegion *reg = (struct SharedRegion *)shared;

    // for each word
    for (size_t i = 0; i < size / reg->align; i++)
    {
        uintptr_t word_addr = (uintptr_t)source + reg->align * i;
        WordLock &word = getWordLock(reg, word_addr);                     // shared
        void *target_word = (void *)((uintptr_t)target + reg->align * i); // private

        if (!transaction.read_only)
        {
            auto it = transaction.write_set.find(word_addr); // O(logn)
            if (it != transaction.write_set.end())
            { // found in write-set
                memcpy(target_word, it->second, reg->align);
                continue;
            }
        }

        VersionLockValue prev_val = word.vlock.Sample();
        memcpy(target_word, &word.word, reg->align); // read word
        VersionLockValue post_val = word.vlock.Sample();

        if (post_val.locked || (prev_val.version != post_val.version) ||
            (prev_val.version > transaction.rv))
        {
            reset_transaction();
            return false;
        }

        if (!transaction.read_only)
            transaction.read_set.emplace((void *)word_addr);
    }

    return true;
}

void release_lock_set(SharedRegion *reg, uint i)
{
    if (i == 0)
        return;
    for (const auto &target_src : transaction.write_set)
    {
        WordLock &wl = getWordLock(reg, target_src.first);
        wl.vlock.Release();
        if (i <= 1)
            break;
        i--;
    }
}

int try_acquire_sets(SharedRegion *reg, uint *i)
{
    *i = 0;
    for (const auto &target_src : transaction.write_set)
    {
        WordLock &wl = getWordLock(reg, target_src.first);
        bool acquired = wl.vlock.TryAcquire();
        if (!acquired)
        {
            release_lock_set(reg, *i);
            return false;
        }
        *i = *i + 1;
    }
    return true;
}

bool validate_readset(SharedRegion *reg)
{
    for (const auto word : transaction.read_set)
    {
        WordLock &wl = getWordLock(reg, (uintptr_t)word);
        VersionLockValue val = wl.vlock.Sample();
        if ((val.locked) || val.version > transaction.rv)
        {
            return false;
        }
    }

    return true;
}

// release locks and update their version
bool commit(SharedRegion *reg)
{

    for (const auto target_src : transaction.write_set)
    {
        WordLock &wl = getWordLock(reg, target_src.first);
        memcpy(&wl.word, target_src.second, reg->align);
        if (!wl.vlock.VersionedRelease(transaction.wv))
        {
            printf("[Commit]: VersionedRelease failed\n");
            reset_transaction();
            return false;
        }
    }

    reset_transaction();
    return true;
}

bool tm_end(shared_t shared, [[maybe_unused]] tx_t tx) noexcept
{
    if (transaction.read_only || transaction.write_set.empty())
    {
        reset_transaction();
        return true;
    }

    struct SharedRegion *reg = (struct SharedRegion *)shared;

    uint tmp;
    if (!try_acquire_sets(reg, &tmp))
    {
        reset_transaction();
        return false;
    }

    transaction.wv = version_clock.fetch_add(1) + 1;

    if ((transaction.rv != transaction.wv - 1) && !validate_readset(reg))
    {
        release_lock_set(reg, tmp);
        reset_transaction();
        return false;
    }

    return commit(reg);
}

Alloc tm_alloc(shared_t shared, [[maybe_unused]] tx_t tx, [[maybe_unused]] size_t size, void **target) noexcept
{
    struct SharedRegion *reg = ((struct SharedRegion *)shared);
    *target = (void *)(reg->seg_cnt.fetch_add(1) << 32);
    return Alloc::success;
}

bool tm_free([[maybe_unused]] shared_t shared, [[maybe_unused]] tx_t tx, [[maybe_unused]] void *segment) noexcept
{
    return true;
}
