/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "hash.h"

/**
 * FurcHash -- a consistent hash function using a binary decision tree.
 * Based on an algorithm by Mark Rabkin with two changes:
 *    1) Uses MurmurHash64A to hash the original key and to generate
 *       additional bits by recursively rehashing.
 *    2) The original recursive algorithm for the decision tree has been
 *       made iterative.
 *
 * Assumes that |m| is 8 million or less (2^FURC_SHIFT).  Making FURC_SHIFT
 * bigger also makes furc_hash modestly slower.
 *
 * Performance is in the sub-500ns range to over 100,000 shards with 13-byte
 * keys.  This version of furc_hash is fairly insensitive to key length since
 * additional bits are generated by re-hashing the initial MurmurHash64A.
 */

/* Seed constant for MurmurHash64A selected by search for optimum diffusion
 * including recursive application.
 */
#define SEED 4193360111ul

/**
 * MurmurHash2, 64-bit versions, by Austin Appleby
 *
 * The same caveats as 32-bit MurmurHash2 apply here - beware of alignment
 * and endian-ness issues if used across multiple platforms.
 *
 * 64-bit hash for 64-bit platforms
 */
uint64_t murmur_hash_64A(const void* const key, const size_t len,
                         const uint32_t seed) {
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    const int r = 47;

    uint64_t h = seed ^ (len * m);

    const uint64_t * data = (const uint64_t *)key;
    const uint64_t * end = data + (len/8);

    while(data != end) {
        uint64_t k = *data++;

        k *= m;
        k ^= k >> r;
        k *= m;

        h ^= k;
        h *= m;
    }

    const uint8_t * data2 = (const uint8_t*)data;

    switch(len & 7) {
        case 7: h ^= (uint64_t)data2[6] << 48;
        case 6: h ^= (uint64_t)data2[5] << 40;
        case 5: h ^= (uint64_t)data2[4] << 32;
        case 4: h ^= (uint64_t)data2[3] << 24;
        case 3: h ^= (uint64_t)data2[2] << 16;
        case 2: h ^= (uint64_t)data2[1] << 8;
        case 1: h ^= (uint64_t)data2[0];
                h *= m;
    };

    h ^= h >> r;
    h *= m;
    h ^= h >> r;

    return h;
}

/* MurmurHash64A performance-optimized for hash of uint64_t keys and seed = M0 */
static uint64_t murmur_rehash_64A(uint64_t k) {
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    const int r = 47;

    uint64_t h = (uint64_t)SEED ^ (sizeof(uint64_t) * m);

    k *= m;
    k ^= k >> r;
    k *= m;

    h ^= k;
    h *= m;

    h ^= h >> r;
    h *= m;
    h ^= h >> r;

    return h;
}

/**
 * furc_get_bit -- the bitstream generator
 *
 * Given a key and an index, provides a pseudorandom bit dependent on both.
 * Caches hash values; a NULL key clears the cache.
 */
static uint32_t furc_get_bit(const char* const key, const size_t len,
                             const uint32_t idx, uint64_t* hash,
                             int32_t* old_ord_p) {
    int32_t ord = (idx >> 6);
    int n;

    if (key == NULL) {
        *old_ord_p = -1;
        return 0;
    }

    if (*old_ord_p < ord) {
        for (n = *old_ord_p + 1; n <= ord; n++) {
            hash[n] = ((n == 0)
                       ? murmur_hash_64A(key, len, SEED)
                       : murmur_rehash_64A(hash[n-1]));
        }
        *old_ord_p = ord;
    }

    return (hash[ord] >> (idx&0x3f)) & 0x1;
}

uint32_t furc_hash_array(const char* const key, const size_t len,
                         const uint32_t m, uint64_t* hash) {
    uint32_t tries;
    uint32_t d;
    uint32_t num;
    uint32_t i;
    uint32_t a;
    int32_t old_ord;

    assert(m <= furc_maximum_pool_size());

    if (m <= 1) {
        return 0;
    }

    furc_get_bit(NULL, 0, 0, hash, &old_ord);
    for (d = 0; m > (1ul << d); d++)
        ;

    a = d;
    for (tries = 0; tries < FURC_MAX_TRIES; tries++) {
        while (!furc_get_bit(key, len, a, hash, &old_ord)) {
            if (--d == 0) {
                return 0;
            }
            a = d;
        }
        a += FURC_SHIFT;
        num = 1;
        for (i = 0; i < d-1; i++) {
            num = (num << 1) | furc_get_bit(key, len, a, hash, &old_ord);
            a += FURC_SHIFT;
        }
        if (num < m) {
            return num;
        }
    }

    // Give up; return 0, which is a legal value in all cases.
    return 0;
}

uint32_t furc_hash(const char* const key, const size_t len, const uint32_t m) {
    uint64_t hash[FURC_CACHE_SIZE];
    return furc_hash_array(key, len, m, hash);
}

inline uint32_t furc_maximum_pool_size(void) {
    return (1 << FURC_SHIFT);
}




/**
 * CRC32 hash function. This is adapted from the libmemcache source,
 * which contained the following notice:
 *
 * The crc32 functions and data was originally written by Spencer
 * Garrett <srg@quick.com> and was extracted from the PostgreSQL source
 * tree via the files contrib/ltree/crc32.[ch].
 */

static const unsigned int crc32tab[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba,
    0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de,
    0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,
    0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940,
    0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116,
    0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a,
    0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818,
    0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c,
    0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
    0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086,
    0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4,
    0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
    0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe,
    0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252,
    0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60,
    0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04,
    0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a,
    0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e,
    0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c,
    0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0,
    0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6,
    0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};

uint32_t crc32_hash(const char* const key, const size_t len) {
	uint32_t crc;
	unsigned int i;

	crc = ~0;

	for (i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32tab[(crc ^ (key[i])) & 0xff];
    }

	//crc = (~crc >> 16) & 0x7fff;
    return (~crc);
}
