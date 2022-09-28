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
#include <iostream>

// Internal Headers
#include <tm.hpp>

#include "expect.hpp"
#include "memory.hpp"
#include "version_lock.hpp"

/**
 * @brief Global version clock
 *
 */
static std::atomic_uint version_clock{};

/**
 * @brief This variable stores the information about
 * the current transaction being processed
 */
static thread_local Transaction transaction;

/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
 **/
shared_t tm_create(size_t size, size_t align) noexcept
{
    try
    {
        return new struct SharedRegion(size, align);
    }
    catch (std::bad_alloc &e)
    {
        return invalid_shared;
    };
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
 **/
void tm_destroy([[maybe_unused]] shared_t shared) noexcept
{
    delete static_cast<struct SharedRegion *>(shared);
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
 **/
void *tm_start([[maybe_unused]] shared_t shared) noexcept
{
    return static_cast<struct SharedRegion *>(shared)->start;
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
 **/
size_t tm_size([[maybe_unused]] shared_t shared) noexcept
{
    return static_cast<struct SharedRegion *>(shared)->size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
 **/
size_t tm_align([[maybe_unused]] shared_t shared) noexcept
{
    return static_cast<struct SharedRegion *>(shared)->align;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
 **/
tx_t tm_begin([[maybe_unused]] shared_t shared, [[maybe_unused]] bool is_ro) noexcept
{
    transaction.is_ro = is_ro;
    transaction.rv = version_clock.load();
    return reinterpret_cast<uintptr_t>(&transaction);
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
 **/
bool tm_end([[maybe_unused]] shared_t shared, [[maybe_unused]] tx_t tx) noexcept
{
    if (transaction.is_ro)
    {
        transaction = {};
        return true;
    }

    uint tmp;
    auto *reg = static_cast<struct SharedRegion *>(shared);
    if (!reg->try_acquire_sets(&tmp, transaction))
    {
        transaction = {};
        return false;
    }

    transaction.wv = version_clock.fetch_add(1) + 1;

    if ((transaction.rv != transaction.wv - 1) && !reg->validate_readset(transaction))
    {
        reg->release_lock_set(tmp, transaction);
        transaction = {};
        return false;
    }

    return reg->commit(transaction);
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
    struct SharedRegion *reg = static_cast<struct SharedRegion *>(shared);

    for (size_t i = 0; i < (size / reg->align); i++)
    {
        uintptr_t word_addr = (uintptr_t)source + reg->align * i;
        WordLock &word = (*reg)[word_addr];
        void *target_word = (void *)((uintptr_t)target + reg->align * i); // private

        if (!transaction.is_ro)
        {
            auto it = transaction.write_set.find(word_addr); // O(logn)
            if (it != transaction.write_set.end())
            { // found in write-set
                memcpy(target_word, it->second, reg->align);
                continue;
            }
        }

        VersionLock::Value prev_val = word.vlock.sample();
        memcpy(target_word, &word.word, reg->align); // read word
        VersionLock::Value post_val = word.vlock.sample();

        if (post_val.locked || (prev_val.version != post_val.version) || (prev_val.version > transaction.rv))
        {
            transaction = {};
            return false;
        }

        if (!transaction.is_ro)
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
    struct SharedRegion *reg = static_cast<struct SharedRegion *>(shared);

    for (size_t i = 0; i < (size / reg->align); i++)
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
    struct SharedRegion *reg = ((struct SharedRegion *)shared);
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