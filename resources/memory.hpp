/**
 * @file   memory.hpp
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
 * STM's memory layout Interface.
 **/

#pragma once

// External Headers
#include <new>
#include <tm.hpp>
#include <atomic>
#include <cerrno>
#include <vector>
#include <cstdlib>
#include <cstring>

// Internal Headers
#include "spin_lock.hpp"

/**
 * @brief Segment of Memory
 *
 */
struct Segment
{
    size_t size;
    size_t align;

    void *start{nullptr};
    Segment *next{nullptr};
    Segment *prev{nullptr};

    SpinLock versioned_write_lock;

    Segment(size_t size, size_t align)
        : size(size), align(align)
    {
        if (posix_memalign(&start, align, size) != 0)
        {
            throw std::bad_alloc();
        }

        std::memset(start, 0, size);
    }

    ~Segment()
    {
        std::free(start);
    }
};

/**
 * @brief Shared Memory Region.
 */
struct SharedRegion
{
    size_t size;
    size_t align;
    Segment first;
    std::atomic_uint gv{0};

    SharedRegion(size_t size, size_t align)
        : size(size), align(align), first(size, align)
    {
    }

    ~SharedRegion()
    {
        while (first.next != nullptr)
        {
            auto next = first.next;
            first.next = first.next->next;
            delete next;
        }
    }

    Segment *FindSegment(const void *ptr)
    {
        Segment *curr = &first;
        while (curr != nullptr and
               curr->start <= ptr and
               ptr <= curr->start + (curr->size * curr->align))
               curr = curr->next;
        return curr;
    }
};
