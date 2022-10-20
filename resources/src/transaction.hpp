/**
 * @file   transaction.hpp
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
 * Transaction Interface
 **/

#pragma once

// External Headers
#include <new>
#include <list>
#include <cstring>
#include <algorithm>

// Internal Headers
#include "expect.hpp"
#include "memory.hpp"
#include "bloom_filter.hpp"
#include "versioned_lock.hpp"
#include "segment_allocator.hpp"

struct TransactionDescriptor
{
    std::atomic_uint_fast32_t refcount{1};
    VersionedLock::Timestamp commit_time{0};

    std::vector<ObjectId> segments_to_delete{};
    std::vector<std::unique_ptr<ObjectVersion>> objects_to_delete{};

    TransactionDescriptor *next{nullptr};
};

struct Transaction
{
    struct WriteEntry
    {
        ObjectId addr;
        Object &obj;
        std::unique_ptr<char[]> written;
    };

    struct ReadEntry
    {
        ObjectId addr;
        Object &obj;
    };

    bool is_ro;

    TransactionDescriptor *start_point;
    VersionedLock::Timestamp start_time;

    std::vector<ObjectId> free_set;
    std::vector<ObjectId> alloc_set;
    std::vector<ReadEntry> read_set;
    std::vector<WriteEntry> write_set;

    Transaction(bool is_ro, TransactionDescriptor *start_point, VersionedLock::Timestamp start_time)
        : is_ro(is_ro), start_point(start_point), start_time(start_time)
    {
    }

    WriteEntry *find_write_entry(ObjectId addr) noexcept
    {
        for (auto &entry : write_set)
        {
            if (entry.addr == addr)
            {
                return &entry;
            }
        }

        return nullptr;
    }
};
