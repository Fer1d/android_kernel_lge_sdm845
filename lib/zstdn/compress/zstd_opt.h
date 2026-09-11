/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTDN_OPT_H
#define ZSTDN_OPT_H


#include "zstd_compress_internal.h"

/* used in ZSTDN_loadDictionaryContent() */
void ZSTDN_updateTree(ZSTDN_matchState_t* ms, const BYTE* ip, const BYTE* iend);

size_t ZSTDN_compressBlock_btopt(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_btultra(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_btultra2(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);


size_t ZSTDN_compressBlock_btopt_dictMatchState(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_btultra_dictMatchState(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);

size_t ZSTDN_compressBlock_btopt_extDict(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTDN_compressBlock_btultra_extDict(
        ZSTDN_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);

        /* note : no btultra2 variant for extDict nor dictMatchState,
         * because btultra2 is not meant to work with dictionaries
         * and is only specific for the first block (no prefix) */


#endif /* ZSTDN_OPT_H */
