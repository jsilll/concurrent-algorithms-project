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
 * Transaction Related Data Structures Implementation
 **/

#include "transaction.hpp"

// External Headers
#include <cstring>

Transaction::WriteLog::WriteLog(const Segment *segment, const size_t size, const void *source)
    : segment(segment), size(size)
{
    value = std::malloc(size);
    memcpy(value, source, size);
}

Transaction::WriteLog::~WriteLog()
{
    std::free(value);
}

Transaction::ReadLog::ReadLog(const Segment *segment)
    : segment(segment)
{
}