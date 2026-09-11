/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTDN_FAST_H
#define ZSTDN_FAST_H


#include "../common/mem.h"      /* U32 */
#include "zstd_compress_internal.h"

void ZSTDN_fillHashTable(ZSTDN_matchState_t* ms,
                        void const* end, ZSTDN_dictTableLoadMethod_e dtlm);
size_t ZSTDN_compressBlock_fast(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_fast_dictMatchState(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_fast_extDict(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);


#endif /* ZSTDN_FAST_H */
