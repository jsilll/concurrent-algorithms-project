/**
 * @file   memory.cpp
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
 * STM's memory layout Implementation.
 **/

#include "memory.hpp"

// External Headers
#include <new>
#include <cerrno>
#include <cstring>
#include <cstdlib>

// Internal Headers
#include "expect.hpp"

Segment::Segment(size_t size, size_t align)
: size(size), align(align), versioned_locks(size / align, 0)
{
    int res = posix_memalign(&start, align, size);
    
    if (res != 0)
    {
        throw std::bad_alloc();
    }

    std::memset(start, 0, size);
}

Segment::~Segment()
{
    std::free(start);
}

SharedRegion::SharedRegion(size_t size, size_t align)
    : size(size), align(align), first(size, align)
{}

SharedRegion::~SharedRegion() 
{
    while (first.next != nullptr) 
    {
        auto next = first.next;
        first.next = first.next->next;
        delete next;
    }
}