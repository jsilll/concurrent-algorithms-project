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
static std::atomic_uint global_version{};

/**
 * @brief Current transaction being run
 */
static thread_local Transaction transaction;

/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
 **/
shared_t tm_create([[maybe_unused]] size_t size, [[maybe_unused]] size_t align) noexcept
{
    try
    {
        return new SharedRegion(size, align);
    }
    catch (const std::exception &e)
    {
        return invalid_shared;
    }
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
 **/
void tm_destroy([[maybe_unused]] shared_t shared) noexcept
{
    delete static_cast<SharedRegion *>(shared);
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
 **/
void *tm_start([[maybe_unused]] shared_t shared) noexcept
{
    // TODO: make it thread safe
    return static_cast<SharedRegion *>(shared)->first.data;
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
 **/
size_t tm_size([[maybe_unused]] shared_t shared) noexcept
{
    // TODO: make it thread safe
    return static_cast<SharedRegion *>(shared)->size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
 **/
size_t tm_align([[maybe_unused]] shared_t shared) noexcept
{
    // TODO: make it thread safe
    return static_cast<SharedRegion *>(shared)->align;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
 **/
tx_t tm_begin([[maybe_unused]] shared_t shared, [[maybe_unused]] bool read_only) noexcept
{
    // TODO: make it thread safe
    transaction.read_only = read_only;
    return reinterpret_cast<tx_t>(&transaction);
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
 **/
bool tm_end([[maybe_unused]] shared_t shared, [[maybe_unused]] tx_t tx) noexcept
{
    // TODO: make it thread safe
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
bool tm_read([[maybe_unused]] shared_t shared, [[maybe_unused]] tx_t tx, [[maybe_unused]] void const *source, [[maybe_unused]] size_t size, [[maybe_unused]] void *target) noexcept
{
    // TODO: make it thread safe
    // auto segment = reinterpret_cast<Segment*>(source);
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
bool tm_write([[maybe_unused]] shared_t shared, [[maybe_unused]] tx_t tx, [[maybe_unused]] void const *source, [[maybe_unused]] size_t size, [[maybe_unused]] void *target) noexcept
{
    // TODO: make it thread safe
    std::memcpy(target, source, size);
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
    // TODO: make it thread safe
    try
    {
        auto region = static_cast<SharedRegion *>(shared);
        auto node = new DoublyLinkedList<Segment>::Node(size, region->align);
        region->allocs.Push(node);
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

bool tm_free([[maybe_unused]] shared_t shared, [[maybe_unused]] tx_t tx, [[maybe_unused]] void *segment) noexcept
{
    // TODO: make it thread safe

    // Garanteed by C/C++ std. to work
    auto node = reinterpret_cast<DoublyLinkedList<Segment>::Node *>(segment);

    // Check to see if this is the first node the linked list of SharedRegion
    if (node->prev == nullptr)
    {
        static_cast<SharedRegion *>(shared)->allocs.Pop();
    }

    delete node;

    return true;
}
