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

#include <tm.hpp>

// Internal Headers
#include "spin_lock.hpp"
#include "doubly_linked_list.hpp"

/**
 * @brief Segment of Memory
 *
 */
struct Segment
{
    void *data{};
    SpinLock vlock;

    explicit Segment(size_t s, size_t a);
    ~Segment();
};

/**
 * @brief Shared Memory Region.
 */
struct SharedRegion
{
    size_t size;
    size_t align;
    Segment first;
    DoublyLinkedList<Segment> allocs{};

    explicit SharedRegion(size_t s, size_t a);
    ~SharedRegion();
};
