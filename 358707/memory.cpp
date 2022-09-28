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

Segment::Segment(size_t s, size_t a)
{
    data = std::aligned_alloc(a, s);

    if (unlikely(data == nullptr))
    {
        throw std::bad_alloc();
    }

    std::memset(data, 0, s);
}

Segment::~Segment()
{
    std::free(data);
}

SharedRegion::SharedRegion(size_t s, size_t a)
    : size(s), align(a), first(s, a)
{
}
