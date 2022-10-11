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

#include <tm.hpp>

// External Headers
#include <atomic>
#include <cstring>
#include <exception>

// Internal Headers
#include "expect.hpp"
#include "memory.hpp"
#include "transaction.hpp"

/**
 * @brief Global version clock, as described in TL2
 *
 */
static std::atomic_uint gv{0};

/**
 * @brief Current transaction being run
 */
// static thread_local Transaction transaction;

/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
 **/
shared_t tm_create(size_t size, size_t align) noexcept
{
    try
    {
        return static_cast<void *>(new SharedRegion(size, align));
    }
    catch (const std::exception &e)
    {
        return invalid_shared;
    }
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
 **/
void tm_destroy(shared_t shared) noexcept
{
    delete static_cast<SharedRegion *>(shared);
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
 **/
void *tm_start(shared_t shared) noexcept
{
    return static_cast<void *>(static_cast<SharedRegion *>(shared)->first.data);
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
 **/
size_t tm_size(shared_t shared) noexcept
{
    return static_cast<SharedRegion *>(shared)->size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
 **/
size_t tm_align(shared_t shared) noexcept
{
    return static_cast<SharedRegion *>(shared)->align;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
 **/
tx_t tm_begin(shared_t shared, bool ro) noexcept
{
    try
    {
        return reinterpret_cast<tx_t>(new Transaction(ro, gv.load(), shared));
    }
    catch (const std::exception &e)
    {
        return invalid_tx;
    }
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
 **/
bool tm_end([[maybe_unused]] shared_t shared, tx_t tx) noexcept
{
    auto transaction = reinterpret_cast<Transaction *>(tx);

    if (transaction->ro())
    {
        return true;
    }

    // Lock the write-set in any convenient order using
    // bounded-spinning to avoid indefinite dealock.
    if (!transaction->LockWriteSet())
    {
        return false;
    }

    // Upon successful completion of lock acquisition of all
    // locks in the write-set perform an increment and fetch
    auto wv = gv.fetch_add(1) + 1;

    // In the special case where rv + 1 it is not necessary to
    // validate the read-set, as it is guaranteed that no
    // concurrently executing transaction could have modified it.
    if ((transaction->rv() + 1) == wv)
    {
        // For each location in the write-set, store to the location
        // the new value from the write-set and release the locations lock
        // by setting the version value to the write-version wv clearing
        // the
        transaction->UnlockWriteSet(wv);
        return true;
    }

    // Validate for each location in the read-set that the version
    // number associated with the versioned-write-lock is <= rv.
    // We also verify that these memory locations have not been locked by other threads.
    if (!transaction->ValidateReadSet())
    {
        transaction->UnlockWriteSet(wv);
        return false;
    }

    transaction->Commit();
    transaction->UnlockWriteSet(wv);
    return true;
}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
 **/
bool tm_read([[maybe_unused]] shared_t shared, tx_t tx, void const *source, size_t size, void *target) noexcept
{
    auto transaction = reinterpret_cast<Transaction *>(tx);
    auto segment = const_cast<Segment *>(reinterpret_cast<const Segment *>(source));

    // Using a bloom filter, first check if the
    // load adress already appears in the write-set, if so
    // it returns the last value written (provides the illusion
    // of processor consistency and avoids read-after-write-hazards).
    if (!transaction->ro() && transaction->wbf().Lookup(segment, sizeof(Segment *)))
    {
        auto node = transaction->ws().end();
        while (node != nullptr)
        {
            if (node->content.segment == segment)
            {
                // A load instruction sampling the associated lock is inserted before each original load,
                // which is then followed by post-validation code checking that the location's versioned
                // write-lock is free and has not changed.
                if (segment->versioned_write_lock.IsLocked() or transaction->rv() < segment->versioned_write_lock.Version())
                {
                    return false;
                }

                try
                {
                    auto read_log = new DoublyLinkedList<Transaction::ReadLog>::Node(segment);
                    transaction->rs().PushBack(read_log);
                }
                catch (const std::exception &e)
                {
                    return false;
                }

                std::memcpy(target, node->content.value, size);

                return true;
            }
            node = node->prev;
        }
    }

    try
    {
        auto read_log = new DoublyLinkedList<Transaction::ReadLog>::Node(segment);
        transaction->rs().PushBack(read_log);
    }
    catch (const std::exception &e)
    {
        return false;
    }

    // A load instruction sampling the associated lock is inserted before each original load,
    // which is then followed by post-validation code checking that the location's versioned
    // write-lock is free and has not changed.
    if (segment->versioned_write_lock.IsLocked() or transaction->rv() < segment->versioned_write_lock.Version())
    {
        return false;
    }

    std::memcpy(target, source, size);

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
bool tm_write([[maybe_unused]] shared_t shared, tx_t tx, void const *source, size_t size, void *target) noexcept
{
    auto transaction = reinterpret_cast<Transaction *>(tx);
    auto segment = reinterpret_cast<const Segment *>(target);
    transaction->wbf().Insert(segment, sizeof(Segment *));

    try
    {
        auto write_log = new DoublyLinkedList<Transaction::WriteLog>::Node(segment, size, source);
        transaction->ws().PushBack(write_log);
    }
    catch (const std::exception &e)
    {
        return false;
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
Alloc tm_alloc(shared_t shared, [[maybe_unused]] tx_t tx, size_t size, void **target) noexcept
{
    try
    {
        auto region = static_cast<SharedRegion *>(shared);
        auto node = new DoublyLinkedList<Segment>::Node(size, region->align);
        region->allocs.PushBack(node);
        *target = node->content.data;
        return Alloc::success;
    }
    catch (const std::exception &e)
    {
        return Alloc::nomem;
    }
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param segment Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
 **/

bool tm_free(shared_t shared, [[maybe_unused]] tx_t tx, void *segment) noexcept
{
    auto node = reinterpret_cast<DoublyLinkedList<Segment>::Node *>(segment);

    if (node->prev == nullptr)
    {
        static_cast<SharedRegion *>(shared)->allocs.PopFront();
    }
    else if (node->next == nullptr)
    {
        static_cast<SharedRegion *>(shared)->allocs.PopBack();
    }

    delete node;

    return true;
}
