#include <bits/stdc++.h>

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
