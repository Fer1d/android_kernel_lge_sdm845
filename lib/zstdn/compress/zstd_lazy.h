/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTDN_LAZY_H
#define ZSTDN_LAZY_H


#include "zstd_compress_internal.h"

/*
 * Dedicated Dictionary Search Structure bucket log. In the
 * ZSTDN_dedicatedDictSearch mode, the hashTable has
 * 2 ** ZSTDN_LAZY_DDSS_BUCKET_LOG entries in each bucket, rather than just
 * one.
 */
#define ZSTDN_LAZY_DDSS_BUCKET_LOG 2

U32 ZSTDN_insertAndFindFirstIndex(ZSTDN_matchState_t* ms, const BYTE* ip);

void ZSTDN_dedicatedDictSearch_lazy_loadDictionary(ZSTDN_matchState_t* ms, const BYTE* const ip);

void ZSTDN_preserveUnsortedMark (U32* const table, U32 const size, U32 const reducerValue);  /*! used in ZSTDN_reduceIndex(). preemptively increase value of ZSTDN_DUBT_UNSORTED_MARK */

size_t ZSTDN_compressBlock_btlazy2(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_lazy2(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_lazy(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_greedy(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);

size_t ZSTDN_compressBlock_btlazy2_dictMatchState(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_lazy2_dictMatchState(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_lazy_dictMatchState(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_greedy_dictMatchState(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);

size_t ZSTDN_compressBlock_lazy2_dedicatedDictSearch(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_lazy_dedicatedDictSearch(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_greedy_dedicatedDictSearch(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);

size_t ZSTDN_compressBlock_greedy_extDict(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_lazy_extDict(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_lazy2_extDict(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_btlazy2_extDict(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);


#endif /* ZSTDN_LAZY_H */
