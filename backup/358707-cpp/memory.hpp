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
 * Definition of the STM's memory layout.
 **/

#pragma once

// External Headers
#include <cstring>

// Internal Headers
#include <tm.hpp>

/**
 * @brief List of dynamically allocated segments.
 */
struct SegmentNode
{
    struct SegmentNode *prev;
    struct SegmentNode *next;
    // TODO: ? uint8_t segment[] // segment of dynamic size
};

using SegmentList = SegmentNode *;

/**
 * @brief Simple Shared Memory Region (a.k.a Transactional Memory).
 */
struct Region
{
    /**
     * @brief Size of the non-deallocable memory segment
     * (in bytes)
     */
    size_t size;

    /**
     * @brief Size of a word in the shared memory region
     * (in bytes)
     */
    size_t align;

    /**
     * @brief Start of the shared memory region
     * (i.e., of the non-deallocable memory segment)
     */
    void *start;

    /**
     * @brief Shared memory segments dynamically
     * allocated via tm_alloc within transactions
     */
    SegmentList allocs{};

    /**
     * @brief Construct a new Region struct
     * Initializes the non-deallocable memory segment
     *
     * @param s
     * @param a
     * @param st
     */
    Region(size_t s, size_t a) : size(s), align(a)
    {
    }
};