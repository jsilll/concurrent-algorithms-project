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

// Needed features
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

// Internal headers
#include "tm.hpp"
#include "memory.hpp"

/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
 **/
shared_t tm_create(size_t size, size_t align) noexcept
{
  return static_cast<shared_t>(new SharedMemory(size, align));
}

void tm_destroy(shared_t shared) noexcept
{
  delete static_cast<SharedMemory *>(shared);
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
 **/
void *tm_start(shared_t shared) noexcept
{
  ObjectId addr = static_cast<SharedMemory *>(shared)->start_addr();
  return reinterpret_cast<void *>(addr.to_opaque());
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
 **/
size_t tm_size(shared_t shared) noexcept
{
  return static_cast<SharedMemory *>(shared)->size();
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
 **/
size_t tm_align(shared_t shared) noexcept
{
  return static_cast<SharedMemory *>(shared)->alignment();
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
 **/
tx_t tm_begin(shared_t shared, bool is_ro) noexcept
{
  auto memory = static_cast<SharedMemory *>(shared);
  return reinterpret_cast<tx_t>(memory->begin_tx(is_ro));
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
 **/
bool tm_end(shared_t shared, tx_t tx) noexcept
{
  auto memory = static_cast<SharedMemory*>(shared);
  auto transaction = reinterpret_cast<Transaction*>(tx);

  bool res = memory->end_tx(*transaction);

  delete transaction;
  return res;
}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
 **/
bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size, void *target) noexcept
{
  auto dest = static_cast<char *>(target);
  auto memory = static_cast<SharedMemory*>(shared);
  auto transaction = reinterpret_cast<Transaction*>(tx);

  std::size_t offset = 0;
  auto start = to_object_id(source);
  auto align = memory->alignment();
  while (offset < size)
  {
    if (!memory->read_word(*transaction, start + offset, dest + offset))
    {
      delete transaction;
      return false;
    }

    offset += align;
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
bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target) noexcept
{
  auto src = static_cast<const char *>(source);
  auto memory = static_cast<SharedMemory*>(shared);
  auto transaction = reinterpret_cast<Transaction*>(tx);

  std::size_t offset = 0;
  auto start = to_object_id(target);
  while (offset < size)
  {
    if (!memory->write_word(*transaction, src + offset, start + offset))
    {
      delete transaction;
      return false;
    }

    offset += memory->alignment();
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
Alloc tm_alloc(shared_t shared, tx_t tx, size_t size, void **target) noexcept
{
  auto memory = static_cast<SharedMemory*>(shared);
  auto transaction = reinterpret_cast<Transaction*>(tx);

  ObjectId addr;
  if (!memory->allocate(*transaction, size, &addr))
  {
    return Alloc::nomem;
  }

  *target = reinterpret_cast<void *>(addr.to_opaque());
  return Alloc::success;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param segment Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
 **/
bool tm_free(shared_t shared, tx_t tx, void *target) noexcept
{
  auto memory = static_cast<SharedMemory*>(shared);
  auto transaction = reinterpret_cast<Transaction*>(tx);
  
  auto id = to_object_id(target);
  memory->free(*transaction, id);
  return true;
}
