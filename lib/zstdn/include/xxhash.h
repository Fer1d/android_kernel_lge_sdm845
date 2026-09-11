/*
 * xxHash - Extremely Fast Hash algorithm
 * Copyright (C) 2012-2016, Yann Collet.
 *
 * BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following disclaimer
 *     in the documentation and/or other materials provided with the
 *     distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation. This program is dual-licensed; you may select
 * either version 2 of the GNU General Public License ("GPL") or BSD license
 * ("BSD").
 *
 * You can contact the author at:
 * - xxHash homepage: https://cyan4973.github.io/xxHash/
 * - xxHash source repository: https://github.com/Cyan4973/xxHash
 */

/*
 * Notice extracted from xxHash homepage:
 *
 * xxHash is an extremely fast Hash algorithm, running at RAM speed limits.
 * It also successfully passes all tests from the SMHasher suite.
 *
 * Comparison (single thread, Windows Seven 32 bits, using SMHasher on a Core 2
 * Duo @3GHz)
 *
 * Name            Speed       Q.Score   Author
 * xxHash          5.4 GB/s     10
 * CrapWow         3.2 GB/s      2       Andrew
 * MumurHash 3a    2.7 GB/s     10       Austin Appleby
 * SpookyHash      2.0 GB/s     10       Bob Jenkins
 * SBox            1.4 GB/s      9       Bret Mulvey
 * Lookup3         1.2 GB/s      9       Bob Jenkins
 * SuperFastHash   1.2 GB/s      1       Paul Hsieh
 * CityHash64      1.05 GB/s    10       Pike & Alakuijala
 * FNV             0.55 GB/s     5       Fowler, Noll, Vo
 * CRC32           0.43 GB/s     9
 * MD5-32          0.33 GB/s    10       Ronald L. Rivest
 * SHA1-32         0.28 GB/s    10
 *
 * Q.Score is a measure of quality of the hash function.
 * It depends on successfully passing SMHasher test set.
 * 10 is a perfect score.
 *
 * A 64-bits version, named xxh64n offers much better speed,
 * but for 64-bits applications only.
 * Name     Speed on 64 bits    Speed on 32 bits
 * xxh64n       13.8 GB/s            1.9 GB/s
 * xxh32n        6.8 GB/s            6.0 GB/s
 */

#ifndef XXHASH_H
#define XXHASH_H

#include <linux/types.h>

/*-****************************
 * Simple Hash Functions
 *****************************/

/**
 * xxh32n() - calculate the 32-bit hash of the input with a given seed.
 *
 * @input:  The data to hash.
 * @length: The length of the data to hash.
 * @seed:   The seed can be used to alter the result predictably.
 *
 * Speed on Core 2 Duo @ 3 GHz (single thread, SMHasher benchmark) : 5.4 GB/s
 *
 * Return:  The 32-bit hash of the data.
 */
uint32_t xxh32n(const void *input, size_t length, uint32_t seed);

/**
 * xxh64n() - calculate the 64-bit hash of the input with a given seed.
 *
 * @input:  The data to hash.
 * @length: The length of the data to hash.
 * @seed:   The seed can be used to alter the result predictably.
 *
 * This function runs 2x faster on 64-bit systems, but slower on 32-bit systems.
 *
 * Return:  The 64-bit hash of the data.
 */
uint64_t xxh64n(const void *input, size_t length, uint64_t seed);

/**
 * xxhash() - calculate wordsize hash of the input with a given seed
 * @input:  The data to hash.
 * @length: The length of the data to hash.
 * @seed:   The seed can be used to alter the result predictably.
 *
 * If the hash does not need to be comparable between machines with
 * different word sizes, this function will call whichever of xxh32n()
 * or xxh64n() is faster.
 *
 * Return:  wordsize hash of the data.
 */

static inline unsigned long xxhash(const void *input, size_t length,
				   uint64_t seed)
{
#if BITS_PER_LONG == 64
       return xxh64n(input, length, seed);
#else
       return xxh32n(input, length, seed);
#endif
}

/*-****************************
 * Streaming Hash Functions
 *****************************/

/*
 * These definitions are only meant to allow allocation of XXH state
 * statically, on stack, or in a struct for example.
 * Do not use members directly.
 */

/**
 * struct xxh32n_state - private xxh32n state, do not use members directly
 */
struct xxh32n_state {
	uint32_t total_len_32;
	uint32_t large_len;
	uint32_t v1;
	uint32_t v2;
	uint32_t v3;
	uint32_t v4;
	uint32_t mem32[4];
	uint32_t memsize;
};

/**
 * struct xxh32n_state - private xxh64n state, do not use members directly
 */
struct xxh64n_state {
	uint64_t total_len;
	uint64_t v1;
	uint64_t v2;
	uint64_t v3;
	uint64_t v4;
	uint64_t mem64[4];
	uint32_t memsize;
};

/**
 * xxh32n_reset() - reset the xxh32n state to start a new hashing operation
 *
 * @state: The xxh32n state to reset.
 * @seed:  Initialize the hash state with this seed.
 *
 * Call this function on any xxh32n_state to prepare for a new hashing operation.
 */
void xxh32n_reset(struct xxh32n_state *state, uint32_t seed);

/**
 * xxh32n_update() - hash the data given and update the xxh32n state
 *
 * @state:  The xxh32n state to update.
 * @input:  The data to hash.
 * @length: The length of the data to hash.
 *
 * After calling xxh32n_reset() call xxh32n_update() as many times as necessary.
 *
 * Return:  Zero on success, otherwise an error code.
 */
int xxh32n_update(struct xxh32n_state *state, const void *input, size_t length);

/**
 * xxh32n_digest() - produce the current xxh32n hash
 *
 * @state: Produce the current xxh32n hash of this state.
 *
 * A hash value can be produced at any time. It is still possible to continue
 * inserting input into the hash state after a call to xxh32n_digest(), and
 * generate new hashes later on, by calling xxh32n_digest() again.
 *
 * Return: The xxh32n hash stored in the state.
 */
uint32_t xxh32n_digest(const struct xxh32n_state *state);

/**
 * xxh64n_reset() - reset the xxh64n state to start a new hashing operation
 *
 * @state: The xxh64n state to reset.
 * @seed:  Initialize the hash state with this seed.
 */
void xxh64n_reset(struct xxh64n_state *state, uint64_t seed);

/**
 * xxh64n_update() - hash the data given and update the xxh64n state
 * @state:  The xxh64n state to update.
 * @input:  The data to hash.
 * @length: The length of the data to hash.
 *
 * After calling xxh64n_reset() call xxh64n_update() as many times as necessary.
 *
 * Return:  Zero on success, otherwise an error code.
 */
int xxh64n_update(struct xxh64n_state *state, const void *input, size_t length);

/**
 * xxh64n_digest() - produce the current xxh64n hash
 *
 * @state: Produce the current xxh64n hash of this state.
 *
 * A hash value can be produced at any time. It is still possible to continue
 * inserting input into the hash state after a call to xxh64n_digest(), and
 * generate new hashes later on, by calling xxh64n_digest() again.
 *
 * Return: The xxh64n hash stored in the state.
 */
uint64_t xxh64n_digest(const struct xxh64n_state *state);

/*-**************************
 * Utils
 ***************************/

/**
 * xxh32n_copy_state() - copy the source state into the destination state
 *
 * @src: The source xxh32n state.
 * @dst: The destination xxh32n state.
 */
void xxh32n_copy_state(struct xxh32n_state *dst, const struct xxh32n_state *src);

/**
 * xxh64n_copy_state() - copy the source state into the destination state
 *
 * @src: The source xxh64n state.
 * @dst: The destination xxh64n state.
 */
void xxh64n_copy_state(struct xxh64n_state *dst, const struct xxh64n_state *src);

#endif /* XXHASH_H */
