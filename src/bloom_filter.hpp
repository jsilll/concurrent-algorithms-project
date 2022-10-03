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
 * Bloom Filter Implementation
 **/

#pragma once

// External Exclude
#include <array>

// Internal Includes
#include "murmur_hash3.hpp"


/**
 * @brief Bloom Filter
 *
 * @tparam M - size of the array
 * @tparam K - number of hashes
 */
template <int M, int K>
class BloomFilter
{
    std::array<bool, M> bits_;

public:
    BloomFilter()
    {
        Clear();
    }

    void Clear() 
    {
        bits_.fill(false);
    }

    bool Lookup(const void *key, int len)
    {
        for (int i = 0; i < K; ++i)
        {
            __uint128_t hash;
            MurmurHash3_x64_128(key, len, i, &hash);
            if (bits_[hash % M] == false)
            {
                return false;
            }
        }

        return true;
    }

    void Insert(const void *key, int len)
    {
        for (int i = 0; i < K; ++i)
        {
            __uint128_t hash;
            MurmurHash3_x64_128(key, len, i, &hash);
            bits_[hash % M] = true;
        }
    }
};
