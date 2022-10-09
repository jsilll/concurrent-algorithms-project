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

// Internal Headers
#include "memory.hpp"
#include "bloom_filter.hpp"
#include "doubly_linked_list.hpp"

/**
 * @brief Transaction
 *
 */
class Transaction
{
public:
    /**
     * @brief Maintains the information
     * associated with a write within a
     * transaction.
     *
     */
    struct WriteLog
    {
        Segment *segment;
        const size_t size;
        void *value;

        WriteLog(const Segment *segment, const size_t size, const size_t align, const void *source);
        
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
        Segment *segment;

        ReadLog(const Segment *segment);
    };

private:
    bool ro_;         // read only
    uint32_t rv_;     // read version
    shared_t region_; // memory region

    BloomFilter<10, 3> wbf_;        // write bloom
    DoublyLinkedList<ReadLog> rs_;  // read set
    DoublyLinkedList<WriteLog> ws_; // write set

public:
    Transaction(bool ro, uint32_t rv, shared_t region);

    inline bool ro() { return ro_; }
    inline void ro(bool ro) { ro_ = ro; }

    inline uint32_t rv() { return rv_; }
    inline void rv(uint32_t rv) { rv_ = rv; }

    inline shared_t region() { return region_; }
    inline void region(shared_t region) { region_ = region; }

    inline BloomFilter<10, 3> &wbf() { return wbf_; }
    inline DoublyLinkedList<ReadLog> &rs() { return rs_; }
    inline DoublyLinkedList<WriteLog> &ws() { return ws_; }

    void Commit();
    bool LockWriteSet();
    bool ValidateReadSet();
    void UnlockWriteSet(unsigned int wv);
};
