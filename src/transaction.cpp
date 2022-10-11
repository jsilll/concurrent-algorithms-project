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

Transaction::WriteLog::WriteLog(const Segment *segment, const size_t size, const void *source)
    : segment(const_cast<Segment *>(segment)), size(size)
{
    // int res = posix_memalign(&value, segment->align, size);

    value = malloc(size);

    if (value == nullptr)
    {
        throw std::bad_alloc();
    }

    memcpy(value, source, size);
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
    auto rs_node = rs_.begin();
    while (rs_node != nullptr)
    {
        auto segment = rs_node->content.segment;
        // In case the validation fails, the transaction is aborted.
        if (segment->versioned_write_lock.IsLocked() or rv_ < segment->versioned_write_lock.Version())
        {
            return false;
        }

        rs_node = rs_node->next;
    }

    return true;
}

void Transaction::Commit()
{
    auto ws_node = ws_.begin();
    while (ws_node != nullptr)
    {
        memcpy(ws_node->content.segment->data, ws_node->content.value, ws_node->content.size);
        ws_node = ws_node->next;
    }
}

bool Transaction::LockWriteSet()
{
    auto ws_node = ws_.begin();
    while (ws_node != nullptr)
    {
        // In case not all of these locks are
        // successfully acquired, the transaction fails
        if (!ws_node->content.segment->versioned_write_lock.Lock())
        {
            // Unlock all the previously acquired locks
            ws_node = ws_node->prev;
            while (ws_node != nullptr)
            {
                ws_node->content.segment->versioned_write_lock.Unlock();
                ws_node = ws_node->prev;
            }

            return false;
        }

        ws_node = ws_node->prev;
    }

    return true;
}

void Transaction::UnlockWriteSet(unsigned int wv)
{
    auto ws_node = ws_.begin();
    while (ws_node != nullptr)
    {
        ws_node->content.segment->versioned_write_lock.Unlock(wv);
        ws_node = ws_node->next;
    }
}