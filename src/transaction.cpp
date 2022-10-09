/**
 * @file   transaction.cpp
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
 * Transaction Implementation
 **/

#include "transaction.hpp"

// External Headers
#include <new>
#include <cstring>

// Internal Headers
#include "expect.hpp"

Transaction::WriteLog::WriteLog(const Segment *segment, const size_t size, const size_t align, const void *source)
    : segment(const_cast<Segment *>(segment)), size(size)
{
    if (unlikely(posix_memalign(&value, align, size) != 0))
    {
        throw std::bad_alloc();
    }

    std::memcpy(value, source, size);
}

Transaction::WriteLog::~WriteLog()
{
    std::free(value);
}

Transaction::ReadLog::ReadLog(const Segment *segment)
    : segment(const_cast<Segment *>(segment))
{
}

Transaction::Transaction(bool ro, uint32_t rv, shared_t region) 
: ro_(ro), rv_(rv), region_(region)
{
}

bool Transaction::ValidateReadSet()
{
    auto node = rs_.begin();
    while (node != nullptr)
    {
        auto segment = node->content.segment;
        // In case the validation fails, the transaction is aborted.
        if (segment->versioned_write_lock.IsLocked() or rv_ < segment->versioned_write_lock.Version())
        {
            return false;
        }

        node = node->next;
    }

    return true;
}

void Transaction::Commit()
{
    auto node = ws_.begin();
    while (node != nullptr)
    {
        memcpy(node->content.segment->data, node->content.value, node->content.size);
        node = node->next;
    }
}

bool Transaction::LockWriteSet()
{
    auto node = ws_.begin();
    while (node != nullptr)
    {
        // In case not all of these locks are
        // successfully acquired, the transaction fails
        if (!node->content.segment->versioned_write_lock.Lock())
        {
            // Unlock all the previously acquired locks
            node = node->prev;
            while (node != nullptr)
            {
                node->content.segment->versioned_write_lock.Unlock();
                node = node->prev;
            }

            return false;
        }

        node = node->prev;
    }

    return true;
}

void Transaction::UnlockWriteSet(unsigned int wv)
{
    auto node = ws_.begin();
    while (node != nullptr)
    {
        node->content.segment->versioned_write_lock.Unlock(wv);
        node = node->next;
    }
}