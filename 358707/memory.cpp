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
 * Implementation of the STM's memory layout.
 **/

#include "memory.hpp"

// External Headers
#include <new>
#include <cstring>
#include <cstdlib>

// Internal Headers
#include "expect.hpp"

SegmentNode::SegmentNode(size_t s, size_t a)
{
    start = std::aligned_alloc(a, s);

    if (unlikely(start == nullptr))
    {
        throw std::bad_alloc();
    }

    std::memset(start, 0, s);
}

SegmentNode::~SegmentNode()
{
    if (prev != nullptr)
    {
        prev->next = next;
    }

    if (next != nullptr)
    {
        next->prev = prev;
    }

    std::free(start);
}

SharedRegion::SharedRegion(size_t s, size_t a)
    : size(s), align(a)
{
    start = std::aligned_alloc(a, s);

    if (unlikely(start == nullptr))
    {
        throw std::bad_alloc();
    }

    std::memset(start, 0, s);
}

SharedRegion::~SharedRegion()
{
    while (allocs_head)
    {
        SegmentNode *tail = allocs_head->next;
        delete allocs_head;
        allocs_head = tail;
    }

    std::free(start);
}

void SharedRegion::PushSegmentNode(SegmentNode *node)
{
    node->next = allocs_head;

    if (node->next)
    {
        node->next->prev = node;
    }

    allocs_head = node;
}

void SharedRegion::PopSegmentNode()
{
    if (allocs_head)
    {
        allocs_head = allocs_head->next;
    }
}
