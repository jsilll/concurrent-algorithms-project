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

// Internal Headers
#include "expect.hpp"
#include "memory.hpp"
#include "bloom_filter.hpp"

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
        void *dest;
        const size_t size;
        void *value;

        WriteLog(void *dest, const size_t size, const void *source)
            : dest(dest), size(size)
        {
            // int res = posix_memalign(&value, segment->align, size);

            value = malloc(size);

            if (unlikely(value == nullptr))
            {
                throw std::bad_alloc();
            }

            memcpy(value, source, size);
        }

        ~WriteLog()
        {
            std::free(value);
        }
    };

    /**
     * @brief Maintains the information
     * associated with a read within a
     * transaction.
     *
     */
    struct ReadLog
    {
        void *src;

        ReadLog(void *src) 
        : src(src) {};
    };

private:
    bool ro_;              // read only
    uint32_t rv_;          // read version
    SharedRegion *region_; // memory region

    std::list<ReadLog> rs_;  // read set
    std::list<WriteLog> ws_; // write set
    BloomFilter<10, 3> wbf_; // write bloom

public:
    Transaction(bool ro, SharedRegion *region)
        : ro_(ro), rv_(region->gv.load()), region_(region) {}

    inline bool ro() { return ro_; }
    inline void ro(bool ro) { ro_ = ro; }

    inline uint32_t rv() { return rv_; }
    inline void rv(uint32_t rv) { rv_ = rv; }

    inline SharedRegion *region() { return region_; }

    inline std::list<ReadLog> &rs() { return rs_; }
    inline std::list<WriteLog> &ws() { return ws_; }
    inline BloomFilter<10, 3> &wbf() { return wbf_; }

    void Commit()
    {
        // TODO

        // auto ws_node = ws_.begin();
        // for (auto const &write : ws_)
        // {
        //     memcpy(write.segment->start, write.value, write.size);
        // }

        // while (ws_node != nullptr)
        // {
        //     ws_node = ws_node->next;
        // }
    }

    bool LockWriteSet()
    {
        auto ws_node = ws_.begin();
        while (ws_node != nullptr)
        {
            // In case not all of these locks are
            // successfully acquired, the transaction fails
            if (!ws_node->content.segment->versioned_write_lock.Lock())
            {
                // Unlock all the previously acquired locks
                ws_node = ws_node->prev;
                while (ws_node != nullptr)
                {
                    ws_node->content.segment->versioned_write_lock.Unlock();
                    ws_node = ws_node->prev;
                }

                return false;
            }

            ws_node = ws_node->prev;
        }

        return true;
    }

    bool ValidateReadSet()
    {
        // TODO

        // for (auto const &read : rs_)
        // {
        //     if (read.segment->versioned_write_lock.IsLocked()
        //     or rv_ < read.segment->versioned_write_lock.Version())
        //     {
        //         return false;
        //     }
        // }

        return true;
    }

    void UnlockWriteSet(unsigned int wv)
    {
        // TODO

        // auto ws_node = ws_.begin();
        // while (ws_node != nullptr)
        // {
        //     ws_node->content.segment->versioned_write_lock.Unlock(wv);
        //     ws_node = ws_node->next;
        // }
    }
};
