/**
 * @file   tm.cpp
 * @author João Silveira <joao.freixialsilveira@epfl.ch>
 *
 * @section LICENSE
 *
 * Copyright © 2018-2021 Sébastien Rouault.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * any later version. Please see https://gnu.org/licenses/gpl.html
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * @section DESCRIPTION
 *
 * Implementation of the STM.
 **/

// External Headers
#include <cstdlib>
#include <cstring>

// Internal Headers
#include <tm.hpp>

#include "expect.hpp"
#include "memory.hpp"
#include "version_lock.hpp"

// Merdas Necessarias (ou nao ;) )

static std::atomic_uint global_vc{}; // global version clock

WordLock &getWordLock(struct region *reg, uintptr_t addr)
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

void release_lock_set(region *reg, uint i)
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

int try_acquire_sets(region *reg, uint *i)
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

bool validate_readset(region *reg)
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
bool commit(region *reg)
{

    for (const auto target_src : transaction.write_set)
    {
        WordLock &wl = getWordLock(reg, target_src.first);
        memcpy(&wl.word, target_src.second, reg->align);
        if (!wl.vlock.VersionedRelease(transaction.wv))
        {
            // printf("[Commit]: VersionedRelease failed\n");
            reset_transaction();
            return false;
        }
    }

    reset_transaction();
    return true;
}

/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
 **/
shared_t tm_create(size_t size, size_t align) noexcept
{
    region *region = new struct region(size, align);
    if (unlikely(!region))
    {
        return invalid_shared;
    }

    return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
 **/
void tm_destroy([[maybe_unused]] shared_t shared) noexcept
{
    struct region *reg = (struct region *)shared;
    delete reg;
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
 **/
void *tm_start([[maybe_unused]] shared_t shared) noexcept
{
    return (void *)((uint64_t)1 << 32);
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
 **/
size_t tm_size([[maybe_unused]] shared_t shared) noexcept
{
    return ((struct region *)shared)->size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
 **/
size_t tm_align([[maybe_unused]] shared_t shared) noexcept
{
    return ((struct region *)shared)->align;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
 **/
tx_t tm_begin([[maybe_unused]] shared_t shared, [[maybe_unused]] bool is_ro) noexcept
{
    transaction.rv = global_vc.load();
    transaction.read_only = is_ro;
    return (uintptr_t)&transaction;
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
 **/
bool tm_end([[maybe_unused]] shared_t shared, [[maybe_unused]] tx_t tx) noexcept
{
    if (transaction.read_only || transaction.write_set.empty())
    {
        reset_transaction();
        return true;
    }

    struct region *reg = (struct region *)shared;

    uint tmp;
    if (!try_acquire_sets(reg, &tmp))
    {
        reset_transaction();
        return false;
    }

    transaction.wv = global_vc.fetch_add(1) + 1;

    if ((transaction.rv != transaction.wv - 1) && !validate_readset(reg))
    {
        release_lock_set(reg, tmp);
        reset_transaction();
        return false;
    }

    return commit(reg);
}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
 **/
bool tm_read([[maybe_unused]] shared_t shared, [[maybe_unused]] tx_t tx, [[maybe_unused]] void const *source, [[maybe_unused]] size_t size, [[maybe_unused]] void *target) noexcept
{
    struct region *reg = (struct region *)shared;

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

        if (post_val.locked || (prev_val.version != post_val.version) || (prev_val.version > transaction.rv))
        {
            reset_transaction();
            return false;
        }

        if (!transaction.read_only)
        {
            transaction.read_set.emplace((void *)word_addr);
        }
    }

    return true;
}

/** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
 **/
bool tm_write([[maybe_unused]] shared_t shared, [[maybe_unused]] tx_t tx, [[maybe_unused]] void const *source, [[maybe_unused]] size_t size, [[maybe_unused]] void *target) noexcept
{
    struct region *reg = (struct region *)shared;

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

/** [thread-safe] Memory allocation in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param size   Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param target Pointer in private memory receiving the address of the first byte of the newly allocated, aligned segment
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
 **/
Alloc tm_alloc([[maybe_unused]] shared_t shared, [[maybe_unused]] tx_t tx, [[maybe_unused]] size_t size, [[maybe_unused]] void **target) noexcept
{
    struct region *reg = ((struct region *)shared);
    *target = (void *)(reg->seg_cnt.fetch_add(1) << 32);
    return Alloc::success;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
 **/
bool tm_free([[maybe_unused]] shared_t shared, [[maybe_unused]] tx_t tx, [[maybe_unused]] void *target) noexcept
{
    return true;
}
