/**
 * @file   murmur_hash3.cpp
 * @author João Silveira <joao.freixialsilveira@epfl.ch>
 *
 * @section DESCRIPTION
 *
 * Murmur Hash 3 Interface
 * https://github.com/aappleby/smhasher
 **/

#pragma once

// External Headers
#include <cstdint>

void MurmurHash3_x64_128(const void *key, int len, uint32_t seed, void *out);
