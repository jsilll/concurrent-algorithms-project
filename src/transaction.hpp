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
 * Transaction Related Data Structures Interface
 **/

#pragma once

// Internal Headers
#include "memory.hpp"
#include "bloom_filter.hpp"
#include "doubly_linked_list.hpp"

/**
 * @brief Transaction
 *
 */
struct Transaction
{
    /**
     * @brief Maintains the information
     * associated with a write within a
     * transaction.
     *
     */
    struct WriteLog
    {
        const Segment *segment;
        const size_t size;
        void *value;

        WriteLog(const Segment *segment, const size_t size, const void *source);

        ~WriteLog();
    };

    /**
     * @brief Maintains the information
     * associated with a read within a
     * transaction.
     *
     */
    struct ReadLog
    {
        const Segment *segment;

        ReadLog(const Segment *segment);
    };

    bool read_only{};
    uint32_t read_version{};
    BloomFilter<10, 3> write_bf;
    DoublyLinkedList<ReadLog> read_set;
    DoublyLinkedList<WriteLog> write_set;
};
