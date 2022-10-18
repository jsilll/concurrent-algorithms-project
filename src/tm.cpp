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

shared_t tm_create(size_t size, size_t align) noexcept
{
  return static_cast<shared_t>(new SharedMemory(size, align));
}

void tm_destroy(shared_t shared) noexcept
{
  delete static_cast<SharedMemory *>(shared);
}

void *tm_start(shared_t shared) noexcept
{
  ObjectId addr = static_cast<SharedMemory *>(shared)->start_addr();
  return reinterpret_cast<void *>(addr.to_opaque());
}

size_t tm_size(shared_t shared) noexcept
{
  return static_cast<SharedMemory *>(shared)->size();
}

size_t tm_align(shared_t shared) noexcept
{
  return static_cast<SharedMemory *>(shared)->alignment();
}

tx_t tm_begin(shared_t shared, bool is_ro) noexcept
{
  auto memory = static_cast<SharedMemory *>(shared);
  return reinterpret_cast<tx_t>(memory->begin_tx(is_ro));
}

bool tm_end(shared_t shared, tx_t tx) noexcept
{
  auto memory = static_cast<SharedMemory*>(shared);
  auto transaction = reinterpret_cast<Transaction*>(tx);

  bool res = memory->end_tx(*transaction);

  delete transaction;
  return res;
}

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

bool tm_free(shared_t shared, tx_t tx, void *target) noexcept
{
  auto memory = static_cast<SharedMemory*>(shared);
  auto transaction = reinterpret_cast<Transaction*>(tx);
  
  auto id = to_object_id(target);
  memory->free(*transaction, id);
  return true;
}
