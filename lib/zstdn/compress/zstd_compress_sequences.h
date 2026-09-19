/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTDN_COMPRESS_SEQUENCES_H
#define ZSTDN_COMPRESS_SEQUENCES_H

#include "../common/fse.h" /* FSEN_repeat, FSEN_CTable */
#include "../common/zstd_internal.h" /* symbolEncodingType_e, ZSTDN_strategy */

typedef enum {
    ZSTDN_defaultDisallowed = 0,
    ZSTDN_defaultAllowed = 1
} ZSTDN_defaultPolicy_e;

symbolEncodingType_e
ZSTDN_selectEncodingType(
        FSEN_repeat* repeatMode, unsigned const* count, unsigned const max,
        size_t const mostFrequent, size_t nbSeq, unsigned const FSELog,
        FSEN_CTable const* prevCTable,
        short const* defaultNorm, U32 defaultNormLog,
        ZSTDN_defaultPolicy_e const isDefaultAllowed,
        ZSTDN_strategy const strategy);

size_t
ZSTDN_buildCTable(void* dst, size_t dstCapacity,
                FSEN_CTable* nextCTable, U32 FSELog, symbolEncodingType_e type,
                unsigned* count, U32 max,
                const BYTE* codeTable, size_t nbSeq,
                const S16* defaultNorm, U32 defaultNormLog, U32 defaultMax,
                const FSEN_CTable* prevCTable, size_t prevCTableSize,
                void* entropyWorkspace, size_t entropyWorkspaceSize);

size_t ZSTDN_encodeSequences(
            void* dst, size_t dstCapacity,
            FSEN_CTable const* CTable_MatchLength, BYTE const* mlCodeTable,
            FSEN_CTable const* CTable_OffsetBits, BYTE const* ofCodeTable,
            FSEN_CTable const* CTable_LitLength, BYTE const* llCodeTable,
            seqDef const* sequences, size_t nbSeq, int longOffsets, int bmi2);

size_t ZSTDN_fseBitCost(
    FSEN_CTable const* ctable,
    unsigned const* count,
    unsigned const max);

size_t ZSTDN_crossEntropyCost(short const* norm, unsigned accuracyLog,
                             unsigned const* count, unsigned const max);
#endif /* ZSTDN_COMPRESS_SEQUENCES_H */
