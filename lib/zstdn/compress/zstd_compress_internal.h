/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/* This header contains definitions
 * that shall **only** be used by modules within lib/compress.
 */

#ifndef ZSTDN_COMPRESS_H
#define ZSTDN_COMPRESS_H

/*-*************************************
*  Dependencies
***************************************/
#include "../common/zstd_internal.h"
#include "zstd_cwksp.h"


/*-*************************************
*  Constants
***************************************/
#define kSearchStrength      8
#define HASH_READ_SIZE       8
#define ZSTDN_DUBT_UNSORTED_MARK 1   /* For btlazy2 strategy, index ZSTDN_DUBT_UNSORTED_MARK==1 means "unsorted".
                                       It could be confused for a real successor at index "1", if sorted as larger than its predecessor.
                                       It's not a big deal though : candidate will just be sorted again.
                                       Additionally, candidate position 1 will be lost.
                                       But candidate 1 cannot hide a large tree of candidates, so it's a minimal loss.
                                       The benefit is that ZSTDN_DUBT_UNSORTED_MARK cannot be mishandled after table re-use with a different strategy.
                                       This constant is required by ZSTDN_compressBlock_btlazy2() and ZSTDN_reduceTable_internal() */


/*-*************************************
*  Context memory management
***************************************/
typedef enum { ZSTDcs_created=0, ZSTDcs_init, ZSTDcs_ongoing, ZSTDcs_ending } ZSTDN_compressionStage_e;
typedef enum { zcss_init=0, zcss_load, zcss_flush } ZSTDN_cStreamStage;

typedef struct ZSTDN_prefixDict_s {
    const void* dict;
    size_t dictSize;
    ZSTDN_dictContentType_e dictContentType;
} ZSTDN_prefixDict;

typedef struct {
    void* dictBuffer;
    void const* dict;
    size_t dictSize;
    ZSTDN_dictContentType_e dictContentType;
    ZSTDN_CDict* cdict;
} ZSTDN_localDict;

typedef struct {
    HUFN_CElt CTable[HUFN_CTABLE_SIZE_U32(255)];
    HUFN_repeat repeatMode;
} ZSTDN_hufCTables_t;

typedef struct {
    FSEN_CTable offcodeCTable[FSEN_CTABLE_SIZE_U32(OffFSELog, MaxOff)];
    FSEN_CTable matchlengthCTable[FSEN_CTABLE_SIZE_U32(MLFSELog, MaxML)];
    FSEN_CTable litlengthCTable[FSEN_CTABLE_SIZE_U32(LLFSELog, MaxLL)];
    FSEN_repeat offcode_repeatMode;
    FSEN_repeat matchlength_repeatMode;
    FSEN_repeat litlength_repeatMode;
} ZSTDN_fseCTables_t;

typedef struct {
    ZSTDN_hufCTables_t huf;
    ZSTDN_fseCTables_t fse;
} ZSTDN_entropyCTables_t;

typedef struct {
    U32 off;            /* Offset code (offset + ZSTDN_REP_MOVE) for the match */
    U32 len;            /* Raw length of match */
} ZSTDN_match_t;

typedef struct {
    U32 offset;         /* Offset of sequence */
    U32 litLength;      /* Length of literals prior to match */
    U32 matchLength;    /* Raw length of match */
} rawSeq;

typedef struct {
  rawSeq* seq;          /* The start of the sequences */
  size_t pos;           /* The index in seq where reading stopped. pos <= size. */
  size_t posInSequence; /* The position within the sequence at seq[pos] where reading
                           stopped. posInSequence <= seq[pos].litLength + seq[pos].matchLength */
  size_t size;          /* The number of sequences. <= capacity. */
  size_t capacity;      /* The capacity starting from `seq` pointer */
} rawSeqStore_t;

UNUSED_ATTR static const rawSeqStore_t kNullRawSeqStore = {NULL, 0, 0, 0, 0};

typedef struct {
    int price;
    U32 off;
    U32 mlen;
    U32 litlen;
    U32 rep[ZSTDN_REP_NUM];
} ZSTDN_optimal_t;

typedef enum { zop_dynamic=0, zop_predef } ZSTDN_OptPrice_e;

typedef struct {
    /* All tables are allocated inside cctx->workspace by ZSTDN_resetCCtx_internal() */
    unsigned* litFreq;           /* table of literals statistics, of size 256 */
    unsigned* litLengthFreq;     /* table of litLength statistics, of size (MaxLL+1) */
    unsigned* matchLengthFreq;   /* table of matchLength statistics, of size (MaxML+1) */
    unsigned* offCodeFreq;       /* table of offCode statistics, of size (MaxOff+1) */
    ZSTDN_match_t* matchTable;    /* list of found matches, of size ZSTDN_OPT_NUM+1 */
    ZSTDN_optimal_t* priceTable;  /* All positions tracked by optimal parser, of size ZSTDN_OPT_NUM+1 */

    U32  litSum;                 /* nb of literals */
    U32  litLengthSum;           /* nb of litLength codes */
    U32  matchLengthSum;         /* nb of matchLength codes */
    U32  offCodeSum;             /* nb of offset codes */
    U32  litSumBasePrice;        /* to compare to log2(litfreq) */
    U32  litLengthSumBasePrice;  /* to compare to log2(llfreq)  */
    U32  matchLengthSumBasePrice;/* to compare to log2(mlfreq)  */
    U32  offCodeSumBasePrice;    /* to compare to log2(offreq)  */
    ZSTDN_OptPrice_e priceType;   /* prices can be determined dynamically, or follow a pre-defined cost structure */
    const ZSTDN_entropyCTables_t* symbolCosts;  /* pre-calculated dictionary statistics */
    ZSTDN_literalCompressionMode_e literalCompressionMode;
} optState_t;

typedef struct {
  ZSTDN_entropyCTables_t entropy;
  U32 rep[ZSTDN_REP_NUM];
} ZSTDN_compressedBlockState_t;

typedef struct {
    BYTE const* nextSrc;    /* next block here to continue on current prefix */
    BYTE const* base;       /* All regular indexes relative to this position */
    BYTE const* dictBase;   /* extDict indexes relative to this position */
    U32 dictLimit;          /* below that point, need extDict */
    U32 lowLimit;           /* below that point, no more valid data */
} ZSTDN_window_t;

typedef struct ZSTDN_matchState_t ZSTDN_matchState_t;
struct ZSTDN_matchState_t {
    ZSTDN_window_t window;   /* State for window round buffer management */
    U32 loadedDictEnd;      /* index of end of dictionary, within context's referential.
                             * When loadedDictEnd != 0, a dictionary is in use, and still valid.
                             * This relies on a mechanism to set loadedDictEnd=0 when dictionary is no longer within distance.
                             * Such mechanism is provided within ZSTDN_window_enforceMaxDist() and ZSTDN_checkDictValidity().
                             * When dict referential is copied into active context (i.e. not attached),
                             * loadedDictEnd == dictSize, since referential starts from zero.
                             */
    U32 nextToUpdate;       /* index from which to continue table update */
    U32 hashLog3;           /* dispatch table for matches of len==3 : larger == faster, more memory */
    U32* hashTable;
    U32* hashTable3;
    U32* chainTable;
    int dedicatedDictSearch;  /* Indicates whether this matchState is using the
                               * dedicated dictionary search structure.
                               */
    optState_t opt;         /* optimal parser state */
    const ZSTDN_matchState_t* dictMatchState;
    ZSTDN_compressionParameters cParams;
    const rawSeqStore_t* ldmSeqStore;
};

typedef struct {
    ZSTDN_compressedBlockState_t* prevCBlock;
    ZSTDN_compressedBlockState_t* nextCBlock;
    ZSTDN_matchState_t matchState;
} ZSTDN_blockState_t;

typedef struct {
    U32 offset;
    U32 checksum;
} ldmEntry_t;

typedef struct {
    BYTE const* split;
    U32 hash;
    U32 checksum;
    ldmEntry_t* bucket;
} ldmMatchCandidate_t;

#define LDM_BATCH_SIZE 64

typedef struct {
    ZSTDN_window_t window;   /* State for the window round buffer management */
    ldmEntry_t* hashTable;
    U32 loadedDictEnd;
    BYTE* bucketOffsets;    /* Next position in bucket to insert entry */
    size_t splitIndices[LDM_BATCH_SIZE];
    ldmMatchCandidate_t matchCandidates[LDM_BATCH_SIZE];
} ldmState_t;

typedef struct {
    U32 enableLdm;          /* 1 if enable long distance matching */
    U32 hashLog;            /* Log size of hashTable */
    U32 bucketSizeLog;      /* Log bucket size for collision resolution, at most 8 */
    U32 minMatchLength;     /* Minimum match length */
    U32 hashRateLog;       /* Log number of entries to skip */
    U32 windowLog;          /* Window log for the LDM */
} ldmParams_t;

typedef struct {
    int collectSequences;
    ZSTDN_Sequence* seqStart;
    size_t seqIndex;
    size_t maxSequences;
} SeqCollector;

struct ZSTDN_CCtx_params_s {
    ZSTDN_format_e format;
    ZSTDN_compressionParameters cParams;
    ZSTDN_frameParameters fParams;

    int compressionLevel;
    int forceWindow;           /* force back-references to respect limit of
                                * 1<<wLog, even for dictionary */
    size_t targetCBlockSize;   /* Tries to fit compressed block size to be around targetCBlockSize.
                                * No target when targetCBlockSize == 0.
                                * There is no guarantee on compressed block size */
    int srcSizeHint;           /* User's best guess of source size.
                                * Hint is not valid when srcSizeHint == 0.
                                * There is no guarantee that hint is close to actual source size */

    ZSTDN_dictAttachPref_e attachDictPref;
    ZSTDN_literalCompressionMode_e literalCompressionMode;

    /* Multithreading: used to pass parameters to mtctx */
    int nbWorkers;
    size_t jobSize;
    int overlapLog;
    int rsyncable;

    /* Long distance matching parameters */
    ldmParams_t ldmParams;

    /* Dedicated dict search algorithm trigger */
    int enableDedicatedDictSearch;

    /* Input/output buffer modes */
    ZSTDN_bufferMode_e inBufferMode;
    ZSTDN_bufferMode_e outBufferMode;

    /* Sequence compression API */
    ZSTDN_sequenceFormat_e blockDelimiters;
    int validateSequences;

    /* Internal use, for createCCtxParams() and freeCCtxParams() only */
    ZSTDN_customMem customMem;
};  /* typedef'd to ZSTDN_CCtx_params within "zstd.h" */

#define COMPRESS_SEQUENCES_WORKSPACE_SIZE (sizeof(unsigned) * (MaxSeq + 2))
#define ENTROPY_WORKSPACE_SIZE (HUFN_WORKSPACE_SIZE + COMPRESS_SEQUENCES_WORKSPACE_SIZE)

/*
 * Indicates whether this compression proceeds directly from user-provided
 * source buffer to user-provided destination buffer (ZSTDb_not_buffered), or
 * whether the context needs to buffer the input/output (ZSTDb_buffered).
 */
typedef enum {
    ZSTDb_not_buffered,
    ZSTDb_buffered
} ZSTDN_buffered_policy_e;

struct ZSTDN_CCtx_s {
    ZSTDN_compressionStage_e stage;
    int cParamsChanged;                  /* == 1 if cParams(except wlog) or compression level are changed in requestedParams. Triggers transmission of new params to ZSTDMT (if available) then reset to 0. */
    int bmi2;                            /* == 1 if the CPU supports BMI2 and 0 otherwise. CPU support is determined dynamically once per context lifetime. */
    ZSTDN_CCtx_params requestedParams;
    ZSTDN_CCtx_params appliedParams;
    U32   dictID;
    size_t dictContentSize;

    ZSTDN_cwksp workspace; /* manages buffer for dynamic allocations */
    size_t blockSize;
    unsigned long long pledgedSrcSizePlusOne;  /* this way, 0 (default) == unknown */
    unsigned long long consumedSrcSize;
    unsigned long long producedCSize;
    struct xxh64n_state xxhState;
    ZSTDN_customMem customMem;
    ZSTDN_threadPool* pool;
    size_t staticSize;
    SeqCollector seqCollector;
    int isFirstBlock;
    int initialized;

    seqStore_t seqStore;      /* sequences storage ptrs */
    ldmState_t ldmState;      /* long distance matching state */
    rawSeq* ldmSequences;     /* Storage for the ldm output sequences */
    size_t maxNbLdmSequences;
    rawSeqStore_t externSeqStore; /* Mutable reference to external sequences */
    ZSTDN_blockState_t blockState;
    U32* entropyWorkspace;  /* entropy workspace of ENTROPY_WORKSPACE_SIZE bytes */

    /* Wether we are streaming or not */
    ZSTDN_buffered_policy_e bufferedPolicy;

    /* streaming */
    char*  inBuff;
    size_t inBuffSize;
    size_t inToCompress;
    size_t inBuffPos;
    size_t inBuffTarget;
    char*  outBuff;
    size_t outBuffSize;
    size_t outBuffContentSize;
    size_t outBuffFlushedSize;
    ZSTDN_cStreamStage streamStage;
    U32    frameEnded;

    /* Stable in/out buffer verification */
    ZSTDN_inBuffer expectedInBuffer;
    size_t expectedOutBufferSize;

    /* Dictionary */
    ZSTDN_localDict localDict;
    const ZSTDN_CDict* cdict;
    ZSTDN_prefixDict prefixDict;   /* single-usage dictionary */

    /* Multi-threading */

    /* Tracing */
};

typedef enum { ZSTDN_dtlm_fast, ZSTDN_dtlm_full } ZSTDN_dictTableLoadMethod_e;

typedef enum {
    ZSTDN_noDict = 0,
    ZSTDN_extDict = 1,
    ZSTDN_dictMatchState = 2,
    ZSTDN_dedicatedDictSearch = 3
} ZSTDN_dictMode_e;

typedef enum {
    ZSTDN_cpm_noAttachDict = 0,  /* Compression with ZSTDN_noDict or ZSTDN_extDict.
                                 * In this mode we use both the srcSize and the dictSize
                                 * when selecting and adjusting parameters.
                                 */
    ZSTDN_cpm_attachDict = 1,    /* Compression with ZSTDN_dictMatchState or ZSTDN_dedicatedDictSearch.
                                 * In this mode we only take the srcSize into account when selecting
                                 * and adjusting parameters.
                                 */
    ZSTDN_cpm_createCDict = 2,   /* Creating a CDict.
                                 * In this mode we take both the source size and the dictionary size
                                 * into account when selecting and adjusting the parameters.
                                 */
    ZSTDN_cpm_unknown = 3,       /* ZSTDN_getCParams, ZSTDN_getParams, ZSTDN_adjustParams.
                                 * We don't know what these parameters are for. We default to the legacy
                                 * behavior of taking both the source size and the dict size into account
                                 * when selecting and adjusting parameters.
                                 */
} ZSTDN_cParamMode_e;

typedef size_t (*ZSTDN_blockCompressor) (
        ZSTDN_matchState_t* bs, seqStore_t* seqStore, U32 rep[ZSTDN_REP_NUM],
        void const* src, size_t srcSize);
ZSTDN_blockCompressor ZSTDN_selectBlockCompressor(ZSTDN_strategy strat, ZSTDN_dictMode_e dictMode);


MEM_STATIC U32 ZSTDN_LLcode(U32 litLength)
{
    static const BYTE LL_Code[64] = {  0,  1,  2,  3,  4,  5,  6,  7,
                                       8,  9, 10, 11, 12, 13, 14, 15,
                                      16, 16, 17, 17, 18, 18, 19, 19,
                                      20, 20, 20, 20, 21, 21, 21, 21,
                                      22, 22, 22, 22, 22, 22, 22, 22,
                                      23, 23, 23, 23, 23, 23, 23, 23,
                                      24, 24, 24, 24, 24, 24, 24, 24,
                                      24, 24, 24, 24, 24, 24, 24, 24 };
    static const U32 LL_deltaCode = 19;
    return (litLength > 63) ? ZSTDN_highbit32(litLength) + LL_deltaCode : LL_Code[litLength];
}

/* ZSTDN_MLcode() :
 * note : mlBase = matchLength - MINMATCH;
 *        because it's the format it's stored in seqStore->sequences */
MEM_STATIC U32 ZSTDN_MLcode(U32 mlBase)
{
    static const BYTE ML_Code[128] = { 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
                                      16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
                                      32, 32, 33, 33, 34, 34, 35, 35, 36, 36, 36, 36, 37, 37, 37, 37,
                                      38, 38, 38, 38, 38, 38, 38, 38, 39, 39, 39, 39, 39, 39, 39, 39,
                                      40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
                                      41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
                                      42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
                                      42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42 };
    static const U32 ML_deltaCode = 36;
    return (mlBase > 127) ? ZSTDN_highbit32(mlBase) + ML_deltaCode : ML_Code[mlBase];
}

typedef struct repcodes_s {
    U32 rep[3];
} repcodes_t;

MEM_STATIC repcodes_t ZSTDN_updateRep(U32 const rep[3], U32 const offset, U32 const ll0)
{
    repcodes_t newReps;
    if (offset >= ZSTDN_REP_NUM) {  /* full offset */
        newReps.rep[2] = rep[1];
        newReps.rep[1] = rep[0];
        newReps.rep[0] = offset - ZSTDN_REP_MOVE;
    } else {   /* repcode */
        U32 const repCode = offset + ll0;
        if (repCode > 0) {  /* note : if repCode==0, no change */
            U32 const currentOffset = (repCode==ZSTDN_REP_NUM) ? (rep[0] - 1) : rep[repCode];
            newReps.rep[2] = (repCode >= 2) ? rep[1] : rep[2];
            newReps.rep[1] = rep[0];
            newReps.rep[0] = currentOffset;
        } else {   /* repCode == 0 */
            ZSTDN_memcpy(&newReps, rep, sizeof(newReps));
        }
    }
    return newReps;
}

/* ZSTDN_cParam_withinBounds:
 * @return 1 if value is within cParam bounds,
 * 0 otherwise */
MEM_STATIC int ZSTDN_cParam_withinBounds(ZSTDN_cParameter cParam, int value)
{
    ZSTDN_bounds const bounds = ZSTDN_cParam_getBounds(cParam);
    if (ZSTDN_isError(bounds.error)) return 0;
    if (value < bounds.lowerBound) return 0;
    if (value > bounds.upperBound) return 0;
    return 1;
}

/* ZSTDN_noCompressBlock() :
 * Writes uncompressed block to dst buffer from given src.
 * Returns the size of the block */
MEM_STATIC size_t ZSTDN_noCompressBlock (void* dst, size_t dstCapacity, const void* src, size_t srcSize, U32 lastBlock)
{
    U32 const cBlockHeader24 = lastBlock + (((U32)bt_raw)<<1) + (U32)(srcSize << 3);
    RETURN_ERROR_IF(srcSize + ZSTDN_blockHeaderSize > dstCapacity,
                    dstSize_tooSmall, "dst buf too small for uncompressed block");
    MEM_writeLE24(dst, cBlockHeader24);
    ZSTDN_memcpy((BYTE*)dst + ZSTDN_blockHeaderSize, src, srcSize);
    return ZSTDN_blockHeaderSize + srcSize;
}

MEM_STATIC size_t ZSTDN_rleCompressBlock (void* dst, size_t dstCapacity, BYTE src, size_t srcSize, U32 lastBlock)
{
    BYTE* const op = (BYTE*)dst;
    U32 const cBlockHeader = lastBlock + (((U32)bt_rle)<<1) + (U32)(srcSize << 3);
    RETURN_ERROR_IF(dstCapacity < 4, dstSize_tooSmall, "");
    MEM_writeLE24(op, cBlockHeader);
    op[3] = src;
    return 4;
}


/* ZSTDN_minGain() :
 * minimum compression required
 * to generate a compress block or a compressed literals section.
 * note : use same formula for both situations */
MEM_STATIC size_t ZSTDN_minGain(size_t srcSize, ZSTDN_strategy strat)
{
    U32 const minlog = (strat>=ZSTDN_btultra) ? (U32)(strat) - 1 : 6;
    ZSTDN_STATIC_ASSERT(ZSTDN_btultra == 8);
    assert(ZSTDN_cParam_withinBounds(ZSTDN_c_strategy, strat));
    return (srcSize >> minlog) + 2;
}

MEM_STATIC int ZSTDN_disableLiteralsCompression(const ZSTDN_CCtx_params* cctxParams)
{
    switch (cctxParams->literalCompressionMode) {
    case ZSTDN_lcm_huffman:
        return 0;
    case ZSTDN_lcm_uncompressed:
        return 1;
    default:
        assert(0 /* impossible: pre-validated */);
        ZSTDN_FALLTHROUGH;
    case ZSTDN_lcm_auto:
        return (cctxParams->cParams.strategy == ZSTDN_fast) && (cctxParams->cParams.targetLength > 0);
    }
}

/*! ZSTDN_safecopyLiterals() :
 *  memcpy() function that won't read beyond more than WILDCOPY_OVERLENGTH bytes past ilimit_w.
 *  Only called when the sequence ends past ilimit_w, so it only needs to be optimized for single
 *  large copies.
 */
static void ZSTDN_safecopyLiterals(BYTE* op, BYTE const* ip, BYTE const* const iend, BYTE const* ilimit_w) {
    assert(iend > ilimit_w);
    if (ip <= ilimit_w) {
        ZSTDN_wildcopy(op, ip, ilimit_w - ip, ZSTDN_no_overlap);
        op += ilimit_w - ip;
        ip = ilimit_w;
    }
    while (ip < iend) *op++ = *ip++;
}

/*! ZSTDN_storeSeq() :
 *  Store a sequence (litlen, litPtr, offCode and mlBase) into seqStore_t.
 *  `offCode` : distance to match + ZSTDN_REP_MOVE (values <= ZSTDN_REP_MOVE are repCodes).
 *  `mlBase` : matchLength - MINMATCH
 *  Allowed to overread literals up to litLimit.
*/
HINT_INLINE UNUSED_ATTR
void ZSTDN_storeSeq(seqStore_t* seqStorePtr, size_t litLength, const BYTE* literals, const BYTE* litLimit, U32 offCode, size_t mlBase)
{
    BYTE const* const litLimit_w = litLimit - WILDCOPY_OVERLENGTH;
    BYTE const* const litEnd = literals + litLength;
#if defined(DEBUGLEVEL) && (DEBUGLEVEL >= 6)
    static const BYTE* g_start = NULL;
    if (g_start==NULL) g_start = (const BYTE*)literals;  /* note : index only works for compression within a single segment */
    {   U32 const pos = (U32)((const BYTE*)literals - g_start);
        DEBUGLOG(6, "Cpos%7u :%3u literals, match%4u bytes at offCode%7u",
               pos, (U32)litLength, (U32)mlBase+MINMATCH, (U32)offCode);
    }
#endif
    assert((size_t)(seqStorePtr->sequences - seqStorePtr->sequencesStart) < seqStorePtr->maxNbSeq);
    /* copy Literals */
    assert(seqStorePtr->maxNbLit <= 128 KB);
    assert(seqStorePtr->lit + litLength <= seqStorePtr->litStart + seqStorePtr->maxNbLit);
    assert(literals + litLength <= litLimit);
    if (litEnd <= litLimit_w) {
        /* Common case we can use wildcopy.
	 * First copy 16 bytes, because literals are likely short.
	 */
        assert(WILDCOPY_OVERLENGTH >= 16);
        ZSTDN_copy16(seqStorePtr->lit, literals);
        if (litLength > 16) {
            ZSTDN_wildcopy(seqStorePtr->lit+16, literals+16, (ptrdiff_t)litLength-16, ZSTDN_no_overlap);
        }
    } else {
        ZSTDN_safecopyLiterals(seqStorePtr->lit, literals, litEnd, litLimit_w);
    }
    seqStorePtr->lit += litLength;

    /* literal Length */
    if (litLength>0xFFFF) {
        assert(seqStorePtr->longLengthID == 0); /* there can only be a single long length */
        seqStorePtr->longLengthID = 1;
        seqStorePtr->longLengthPos = (U32)(seqStorePtr->sequences - seqStorePtr->sequencesStart);
    }
    seqStorePtr->sequences[0].litLength = (U16)litLength;

    /* match offset */
    seqStorePtr->sequences[0].offset = offCode + 1;

    /* match Length */
    if (mlBase>0xFFFF) {
        assert(seqStorePtr->longLengthID == 0); /* there can only be a single long length */
        seqStorePtr->longLengthID = 2;
        seqStorePtr->longLengthPos = (U32)(seqStorePtr->sequences - seqStorePtr->sequencesStart);
    }
    seqStorePtr->sequences[0].matchLength = (U16)mlBase;

    seqStorePtr->sequences++;
}


/*-*************************************
*  Match length counter
***************************************/
static unsigned ZSTDN_NbCommonBytes (size_t val)
{
    if (MEM_isLittleEndian()) {
        if (MEM_64bits()) {
#       if (__GNUC__ >= 4)
            return (__builtin_ctzll((U64)val) >> 3);
#       else
            static const int DeBruijnBytePos[64] = { 0, 0, 0, 0, 0, 1, 1, 2,
                                                     0, 3, 1, 3, 1, 4, 2, 7,
                                                     0, 2, 3, 6, 1, 5, 3, 5,
                                                     1, 3, 4, 4, 2, 5, 6, 7,
                                                     7, 0, 1, 2, 3, 3, 4, 6,
                                                     2, 6, 5, 5, 3, 4, 5, 6,
                                                     7, 1, 2, 4, 6, 4, 4, 5,
                                                     7, 2, 6, 5, 7, 6, 7, 7 };
            return DeBruijnBytePos[((U64)((val & -(long long)val) * 0x0218A392CDABBD3FULL)) >> 58];
#       endif
        } else { /* 32 bits */
#       if (__GNUC__ >= 3)
            return (__builtin_ctz((U32)val) >> 3);
#       else
            static const int DeBruijnBytePos[32] = { 0, 0, 3, 0, 3, 1, 3, 0,
                                                     3, 2, 2, 1, 3, 2, 0, 1,
                                                     3, 3, 1, 2, 2, 2, 2, 0,
                                                     3, 1, 2, 0, 1, 0, 1, 1 };
            return DeBruijnBytePos[((U32)((val & -(S32)val) * 0x077CB531U)) >> 27];
#       endif
        }
    } else {  /* Big Endian CPU */
        if (MEM_64bits()) {
#       if (__GNUC__ >= 4)
            return (__builtin_clzll(val) >> 3);
#       else
            unsigned r;
            const unsigned n32 = sizeof(size_t)*4;   /* calculate this way due to compiler complaining in 32-bits mode */
            if (!(val>>n32)) { r=4; } else { r=0; val>>=n32; }
            if (!(val>>16)) { r+=2; val>>=8; } else { val>>=24; }
            r += (!val);
            return r;
#       endif
        } else { /* 32 bits */
#       if (__GNUC__ >= 3)
            return (__builtin_clz((U32)val) >> 3);
#       else
            unsigned r;
            if (!(val>>16)) { r=2; val>>=8; } else { r=0; val>>=24; }
            r += (!val);
            return r;
#       endif
    }   }
}


MEM_STATIC size_t ZSTDN_count(const BYTE* pIn, const BYTE* pMatch, const BYTE* const pInLimit)
{
    const BYTE* const pStart = pIn;
    const BYTE* const pInLoopLimit = pInLimit - (sizeof(size_t)-1);

    if (pIn < pInLoopLimit) {
        { size_t const diff = MEM_readST(pMatch) ^ MEM_readST(pIn);
          if (diff) return ZSTDN_NbCommonBytes(diff); }
        pIn+=sizeof(size_t); pMatch+=sizeof(size_t);
        while (pIn < pInLoopLimit) {
            size_t const diff = MEM_readST(pMatch) ^ MEM_readST(pIn);
            if (!diff) { pIn+=sizeof(size_t); pMatch+=sizeof(size_t); continue; }
            pIn += ZSTDN_NbCommonBytes(diff);
            return (size_t)(pIn - pStart);
    }   }
    if (MEM_64bits() && (pIn<(pInLimit-3)) && (MEM_read32(pMatch) == MEM_read32(pIn))) { pIn+=4; pMatch+=4; }
    if ((pIn<(pInLimit-1)) && (MEM_read16(pMatch) == MEM_read16(pIn))) { pIn+=2; pMatch+=2; }
    if ((pIn<pInLimit) && (*pMatch == *pIn)) pIn++;
    return (size_t)(pIn - pStart);
}

/* ZSTDN_count_2segments() :
 *  can count match length with `ip` & `match` in 2 different segments.
 *  convention : on reaching mEnd, match count continue starting from iStart
 */
MEM_STATIC size_t
ZSTDN_count_2segments(const BYTE* ip, const BYTE* match,
                     const BYTE* iEnd, const BYTE* mEnd, const BYTE* iStart)
{
    const BYTE* const vEnd = MIN( ip + (mEnd - match), iEnd);
    size_t const matchLength = ZSTDN_count(ip, match, vEnd);
    if (match + matchLength != mEnd) return matchLength;
    DEBUGLOG(7, "ZSTDN_count_2segments: found a 2-parts match (current length==%zu)", matchLength);
    DEBUGLOG(7, "distance from match beginning to end dictionary = %zi", mEnd - match);
    DEBUGLOG(7, "distance from current pos to end buffer = %zi", iEnd - ip);
    DEBUGLOG(7, "next byte : ip==%02X, istart==%02X", ip[matchLength], *iStart);
    DEBUGLOG(7, "final match length = %zu", matchLength + ZSTDN_count(ip+matchLength, iStart, iEnd));
    return matchLength + ZSTDN_count(ip+matchLength, iStart, iEnd);
}


/*-*************************************
 *  Hashes
 ***************************************/
static const U32 prime3bytes = 506832829U;
static U32    ZSTDN_hash3(U32 u, U32 h) { return ((u << (32-24)) * prime3bytes)  >> (32-h) ; }
MEM_STATIC size_t ZSTDN_hash3Ptr(const void* ptr, U32 h) { return ZSTDN_hash3(MEM_readLE32(ptr), h); } /* only in zstd_opt.h */

static const U32 prime4bytes = 2654435761U;
static U32    ZSTDN_hash4(U32 u, U32 h) { return (u * prime4bytes) >> (32-h) ; }
static size_t ZSTDN_hash4Ptr(const void* ptr, U32 h) { return ZSTDN_hash4(MEM_read32(ptr), h); }

static const U64 prime5bytes = 889523592379ULL;
static size_t ZSTDN_hash5(U64 u, U32 h) { return (size_t)(((u  << (64-40)) * prime5bytes) >> (64-h)) ; }
static size_t ZSTDN_hash5Ptr(const void* p, U32 h) { return ZSTDN_hash5(MEM_readLE64(p), h); }

static const U64 prime6bytes = 227718039650203ULL;
static size_t ZSTDN_hash6(U64 u, U32 h) { return (size_t)(((u  << (64-48)) * prime6bytes) >> (64-h)) ; }
static size_t ZSTDN_hash6Ptr(const void* p, U32 h) { return ZSTDN_hash6(MEM_readLE64(p), h); }

static const U64 prime7bytes = 58295818150454627ULL;
static size_t ZSTDN_hash7(U64 u, U32 h) { return (size_t)(((u  << (64-56)) * prime7bytes) >> (64-h)) ; }
static size_t ZSTDN_hash7Ptr(const void* p, U32 h) { return ZSTDN_hash7(MEM_readLE64(p), h); }

static const U64 prime8bytes = 0xCF1BBCDCB7A56463ULL;
static size_t ZSTDN_hash8(U64 u, U32 h) { return (size_t)(((u) * prime8bytes) >> (64-h)) ; }
static size_t ZSTDN_hash8Ptr(const void* p, U32 h) { return ZSTDN_hash8(MEM_readLE64(p), h); }

MEM_STATIC FORCE_INLINE_ATTR
size_t ZSTDN_hashPtr(const void* p, U32 hBits, U32 mls)
{
    switch(mls)
    {
    default:
    case 4: return ZSTDN_hash4Ptr(p, hBits);
    case 5: return ZSTDN_hash5Ptr(p, hBits);
    case 6: return ZSTDN_hash6Ptr(p, hBits);
    case 7: return ZSTDN_hash7Ptr(p, hBits);
    case 8: return ZSTDN_hash8Ptr(p, hBits);
    }
}

/* ZSTDN_ipow() :
 * Return base^exponent.
 */
static U64 ZSTDN_ipow(U64 base, U64 exponent)
{
    U64 power = 1;
    while (exponent) {
      if (exponent & 1) power *= base;
      exponent >>= 1;
      base *= base;
    }
    return power;
}

#define ZSTDN_ROLL_HASH_CHAR_OFFSET 10

/* ZSTDN_rollingHash_append() :
 * Add the buffer to the hash value.
 */
static U64 ZSTDN_rollingHash_append(U64 hash, void const* buf, size_t size)
{
    BYTE const* istart = (BYTE const*)buf;
    size_t pos;
    for (pos = 0; pos < size; ++pos) {
        hash *= prime8bytes;
        hash += istart[pos] + ZSTDN_ROLL_HASH_CHAR_OFFSET;
    }
    return hash;
}

/* ZSTDN_rollingHash_compute() :
 * Compute the rolling hash value of the buffer.
 */
MEM_STATIC U64 ZSTDN_rollingHash_compute(void const* buf, size_t size)
{
    return ZSTDN_rollingHash_append(0, buf, size);
}

/* ZSTDN_rollingHash_primePower() :
 * Compute the primePower to be passed to ZSTDN_rollingHash_rotate() for a hash
 * over a window of length bytes.
 */
MEM_STATIC U64 ZSTDN_rollingHash_primePower(U32 length)
{
    return ZSTDN_ipow(prime8bytes, length - 1);
}

/* ZSTDN_rollingHash_rotate() :
 * Rotate the rolling hash by one byte.
 */
MEM_STATIC U64 ZSTDN_rollingHash_rotate(U64 hash, BYTE toRemove, BYTE toAdd, U64 primePower)
{
    hash -= (toRemove + ZSTDN_ROLL_HASH_CHAR_OFFSET) * primePower;
    hash *= prime8bytes;
    hash += toAdd + ZSTDN_ROLL_HASH_CHAR_OFFSET;
    return hash;
}

/*-*************************************
*  Round buffer management
***************************************/
#if (ZSTDN_WINDOWLOG_MAX_64 > 31)
# error "ZSTDN_WINDOWLOG_MAX is too large : would overflow ZSTDN_CURRENT_MAX"
#endif
/* Max current allowed */
#define ZSTDN_CURRENT_MAX ((3U << 29) + (1U << ZSTDN_WINDOWLOG_MAX))
/* Maximum chunk size before overflow correction needs to be called again */
#define ZSTDN_CHUNKSIZE_MAX                                                     \
    ( ((U32)-1)                  /* Maximum ending current index */            \
    - ZSTDN_CURRENT_MAX)          /* Maximum beginning lowLimit */

/*
 * ZSTDN_window_clear():
 * Clears the window containing the history by simply setting it to empty.
 */
MEM_STATIC void ZSTDN_window_clear(ZSTDN_window_t* window)
{
    size_t const endT = (size_t)(window->nextSrc - window->base);
    U32 const end = (U32)endT;

    window->lowLimit = end;
    window->dictLimit = end;
}

/*
 * ZSTDN_window_hasExtDict():
 * Returns non-zero if the window has a non-empty extDict.
 */
MEM_STATIC U32 ZSTDN_window_hasExtDict(ZSTDN_window_t const window)
{
    return window.lowLimit < window.dictLimit;
}

/*
 * ZSTDN_matchState_dictMode():
 * Inspects the provided matchState and figures out what dictMode should be
 * passed to the compressor.
 */
MEM_STATIC ZSTDN_dictMode_e ZSTDN_matchState_dictMode(const ZSTDN_matchState_t *ms)
{
    return ZSTDN_window_hasExtDict(ms->window) ?
        ZSTDN_extDict :
        ms->dictMatchState != NULL ?
            (ms->dictMatchState->dedicatedDictSearch ? ZSTDN_dedicatedDictSearch : ZSTDN_dictMatchState) :
            ZSTDN_noDict;
}

/*
 * ZSTDN_window_needOverflowCorrection():
 * Returns non-zero if the indices are getting too large and need overflow
 * protection.
 */
MEM_STATIC U32 ZSTDN_window_needOverflowCorrection(ZSTDN_window_t const window,
                                                  void const* srcEnd)
{
    U32 const curr = (U32)((BYTE const*)srcEnd - window.base);
    return curr > ZSTDN_CURRENT_MAX;
}

/*
 * ZSTDN_window_correctOverflow():
 * Reduces the indices to protect from index overflow.
 * Returns the correction made to the indices, which must be applied to every
 * stored index.
 *
 * The least significant cycleLog bits of the indices must remain the same,
 * which may be 0. Every index up to maxDist in the past must be valid.
 * NOTE: (maxDist & cycleMask) must be zero.
 */
MEM_STATIC U32 ZSTDN_window_correctOverflow(ZSTDN_window_t* window, U32 cycleLog,
                                           U32 maxDist, void const* src)
{
    /* preemptive overflow correction:
     * 1. correction is large enough:
     *    lowLimit > (3<<29) ==> current > 3<<29 + 1<<windowLog
     *    1<<windowLog <= newCurrent < 1<<chainLog + 1<<windowLog
     *
     *    current - newCurrent
     *    > (3<<29 + 1<<windowLog) - (1<<windowLog + 1<<chainLog)
     *    > (3<<29) - (1<<chainLog)
     *    > (3<<29) - (1<<30)             (NOTE: chainLog <= 30)
     *    > 1<<29
     *
     * 2. (ip+ZSTDN_CHUNKSIZE_MAX - cctx->base) doesn't overflow:
     *    After correction, current is less than (1<<chainLog + 1<<windowLog).
     *    In 64-bit mode we are safe, because we have 64-bit ptrdiff_t.
     *    In 32-bit mode we are safe, because (chainLog <= 29), so
     *    ip+ZSTDN_CHUNKSIZE_MAX - cctx->base < 1<<32.
     * 3. (cctx->lowLimit + 1<<windowLog) < 1<<32:
     *    windowLog <= 31 ==> 3<<29 + 1<<windowLog < 7<<29 < 1<<32.
     */
    U32 const cycleMask = (1U << cycleLog) - 1;
    U32 const curr = (U32)((BYTE const*)src - window->base);
    U32 const currentCycle0 = curr & cycleMask;
    /* Exclude zero so that newCurrent - maxDist >= 1. */
    U32 const currentCycle1 = currentCycle0 == 0 ? (1U << cycleLog) : currentCycle0;
    U32 const newCurrent = currentCycle1 + maxDist;
    U32 const correction = curr - newCurrent;
    assert((maxDist & cycleMask) == 0);
    assert(curr > newCurrent);
    /* Loose bound, should be around 1<<29 (see above) */
    assert(correction > 1<<28);

    window->base += correction;
    window->dictBase += correction;
    if (window->lowLimit <= correction) window->lowLimit = 1;
    else window->lowLimit -= correction;
    if (window->dictLimit <= correction) window->dictLimit = 1;
    else window->dictLimit -= correction;

    /* Ensure we can still reference the full window. */
    assert(newCurrent >= maxDist);
    assert(newCurrent - maxDist >= 1);
    /* Ensure that lowLimit and dictLimit didn't underflow. */
    assert(window->lowLimit <= newCurrent);
    assert(window->dictLimit <= newCurrent);

    DEBUGLOG(4, "Correction of 0x%x bytes to lowLimit=0x%x", correction,
             window->lowLimit);
    return correction;
}

/*
 * ZSTDN_window_enforceMaxDist():
 * Updates lowLimit so that:
 *    (srcEnd - base) - lowLimit == maxDist + loadedDictEnd
 *
 * It ensures index is valid as long as index >= lowLimit.
 * This must be called before a block compression call.
 *
 * loadedDictEnd is only defined if a dictionary is in use for current compression.
 * As the name implies, loadedDictEnd represents the index at end of dictionary.
 * The value lies within context's referential, it can be directly compared to blockEndIdx.
 *
 * If loadedDictEndPtr is NULL, no dictionary is in use, and we use loadedDictEnd == 0.
 * If loadedDictEndPtr is not NULL, we set it to zero after updating lowLimit.
 * This is because dictionaries are allowed to be referenced fully
 * as long as the last byte of the dictionary is in the window.
 * Once input has progressed beyond window size, dictionary cannot be referenced anymore.
 *
 * In normal dict mode, the dictionary lies between lowLimit and dictLimit.
 * In dictMatchState mode, lowLimit and dictLimit are the same,
 * and the dictionary is below them.
 * forceWindow and dictMatchState are therefore incompatible.
 */
MEM_STATIC void
ZSTDN_window_enforceMaxDist(ZSTDN_window_t* window,
                     const void* blockEnd,
                           U32   maxDist,
                           U32*  loadedDictEndPtr,
                     const ZSTDN_matchState_t** dictMatchStatePtr)
{
    U32 const blockEndIdx = (U32)((BYTE const*)blockEnd - window->base);
    U32 const loadedDictEnd = (loadedDictEndPtr != NULL) ? *loadedDictEndPtr : 0;
    DEBUGLOG(5, "ZSTDN_window_enforceMaxDist: blockEndIdx=%u, maxDist=%u, loadedDictEnd=%u",
                (unsigned)blockEndIdx, (unsigned)maxDist, (unsigned)loadedDictEnd);

    /* - When there is no dictionary : loadedDictEnd == 0.
         In which case, the test (blockEndIdx > maxDist) is merely to avoid
         overflowing next operation `newLowLimit = blockEndIdx - maxDist`.
       - When there is a standard dictionary :
         Index referential is copied from the dictionary,
         which means it starts from 0.
         In which case, loadedDictEnd == dictSize,
         and it makes sense to compare `blockEndIdx > maxDist + dictSize`
         since `blockEndIdx` also starts from zero.
       - When there is an attached dictionary :
         loadedDictEnd is expressed within the referential of the context,
         so it can be directly compared against blockEndIdx.
    */
    if (blockEndIdx > maxDist + loadedDictEnd) {
        U32 const newLowLimit = blockEndIdx - maxDist;
        if (window->lowLimit < newLowLimit) window->lowLimit = newLowLimit;
        if (window->dictLimit < window->lowLimit) {
            DEBUGLOG(5, "Update dictLimit to match lowLimit, from %u to %u",
                        (unsigned)window->dictLimit, (unsigned)window->lowLimit);
            window->dictLimit = window->lowLimit;
        }
        /* On reaching window size, dictionaries are invalidated */
        if (loadedDictEndPtr) *loadedDictEndPtr = 0;
        if (dictMatchStatePtr) *dictMatchStatePtr = NULL;
    }
}

/* Similar to ZSTDN_window_enforceMaxDist(),
 * but only invalidates dictionary
 * when input progresses beyond window size.
 * assumption : loadedDictEndPtr and dictMatchStatePtr are valid (non NULL)
 *              loadedDictEnd uses same referential as window->base
 *              maxDist is the window size */
MEM_STATIC void
ZSTDN_checkDictValidity(const ZSTDN_window_t* window,
                       const void* blockEnd,
                             U32   maxDist,
                             U32*  loadedDictEndPtr,
                       const ZSTDN_matchState_t** dictMatchStatePtr)
{
    assert(loadedDictEndPtr != NULL);
    assert(dictMatchStatePtr != NULL);
    {   U32 const blockEndIdx = (U32)((BYTE const*)blockEnd - window->base);
        U32 const loadedDictEnd = *loadedDictEndPtr;
        DEBUGLOG(5, "ZSTDN_checkDictValidity: blockEndIdx=%u, maxDist=%u, loadedDictEnd=%u",
                    (unsigned)blockEndIdx, (unsigned)maxDist, (unsigned)loadedDictEnd);
        assert(blockEndIdx >= loadedDictEnd);

        if (blockEndIdx > loadedDictEnd + maxDist) {
            /* On reaching window size, dictionaries are invalidated.
             * For simplification, if window size is reached anywhere within next block,
             * the dictionary is invalidated for the full block.
             */
            DEBUGLOG(6, "invalidating dictionary for current block (distance > windowSize)");
            *loadedDictEndPtr = 0;
            *dictMatchStatePtr = NULL;
        } else {
            if (*loadedDictEndPtr != 0) {
                DEBUGLOG(6, "dictionary considered valid for current block");
    }   }   }
}

MEM_STATIC void ZSTDN_window_init(ZSTDN_window_t* window) {
    ZSTDN_memset(window, 0, sizeof(*window));
    window->base = (BYTE const*)"";
    window->dictBase = (BYTE const*)"";
    window->dictLimit = 1;    /* start from 1, so that 1st position is valid */
    window->lowLimit = 1;     /* it ensures first and later CCtx usages compress the same */
    window->nextSrc = window->base + 1;   /* see issue #1241 */
}

/*
 * ZSTDN_window_update():
 * Updates the window by appending [src, src + srcSize) to the window.
 * If it is not contiguous, the current prefix becomes the extDict, and we
 * forget about the extDict. Handles overlap of the prefix and extDict.
 * Returns non-zero if the segment is contiguous.
 */
MEM_STATIC U32 ZSTDN_window_update(ZSTDN_window_t* window,
                                  void const* src, size_t srcSize)
{
    BYTE const* const ip = (BYTE const*)src;
    U32 contiguous = 1;
    DEBUGLOG(5, "ZSTDN_window_update");
    if (srcSize == 0)
        return contiguous;
    assert(window->base != NULL);
    assert(window->dictBase != NULL);
    /* Check if blocks follow each other */
    if (src != window->nextSrc) {
        /* not contiguous */
        size_t const distanceFromBase = (size_t)(window->nextSrc - window->base);
        DEBUGLOG(5, "Non contiguous blocks, new segment starts at %u", window->dictLimit);
        window->lowLimit = window->dictLimit;
        assert(distanceFromBase == (size_t)(U32)distanceFromBase);  /* should never overflow */
        window->dictLimit = (U32)distanceFromBase;
        window->dictBase = window->base;
        window->base = ip - distanceFromBase;
        /* ms->nextToUpdate = window->dictLimit; */
        if (window->dictLimit - window->lowLimit < HASH_READ_SIZE) window->lowLimit = window->dictLimit;   /* too small extDict */
        contiguous = 0;
    }
    window->nextSrc = ip + srcSize;
    /* if input and dictionary overlap : reduce dictionary (area presumed modified by input) */
    if ( (ip+srcSize > window->dictBase + window->lowLimit)
       & (ip < window->dictBase + window->dictLimit)) {
        ptrdiff_t const highInputIdx = (ip + srcSize) - window->dictBase;
        U32 const lowLimitMax = (highInputIdx > (ptrdiff_t)window->dictLimit) ? window->dictLimit : (U32)highInputIdx;
        window->lowLimit = lowLimitMax;
        DEBUGLOG(5, "Overlapping extDict and input : new lowLimit = %u", window->lowLimit);
    }
    return contiguous;
}

/*
 * Returns the lowest allowed match index. It may either be in the ext-dict or the prefix.
 */
MEM_STATIC U32 ZSTDN_getLowestMatchIndex(const ZSTDN_matchState_t* ms, U32 curr, unsigned windowLog)
{
    U32    const maxDistance = 1U << windowLog;
    U32    const lowestValid = ms->window.lowLimit;
    U32    const withinWindow = (curr - lowestValid > maxDistance) ? curr - maxDistance : lowestValid;
    U32    const isDictionary = (ms->loadedDictEnd != 0);
    /* When using a dictionary the entire dictionary is valid if a single byte of the dictionary
     * is within the window. We invalidate the dictionary (and set loadedDictEnd to 0) when it isn't
     * valid for the entire block. So this check is sufficient to find the lowest valid match index.
     */
    U32    const matchLowest = isDictionary ? lowestValid : withinWindow;
    return matchLowest;
}

/*
 * Returns the lowest allowed match index in the prefix.
 */
MEM_STATIC U32 ZSTDN_getLowestPrefixIndex(const ZSTDN_matchState_t* ms, U32 curr, unsigned windowLog)
{
    U32    const maxDistance = 1U << windowLog;
    U32    const lowestValid = ms->window.dictLimit;
    U32    const withinWindow = (curr - lowestValid > maxDistance) ? curr - maxDistance : lowestValid;
    U32    const isDictionary = (ms->loadedDictEnd != 0);
    /* When computing the lowest prefix index we need to take the dictionary into account to handle
     * the edge case where the dictionary and the source are contiguous in memory.
     */
    U32    const matchLowest = isDictionary ? lowestValid : withinWindow;
    return matchLowest;
}



/* debug functions */
#if (DEBUGLEVEL>=2)

MEM_STATIC double ZSTDN_fWeight(U32 rawStat)
{
    U32 const fp_accuracy = 8;
    U32 const fp_multiplier = (1 << fp_accuracy);
    U32 const newStat = rawStat + 1;
    U32 const hb = ZSTDN_highbit32(newStat);
    U32 const BWeight = hb * fp_multiplier;
    U32 const FWeight = (newStat << fp_accuracy) >> hb;
    U32 const weight = BWeight + FWeight;
    assert(hb + fp_accuracy < 31);
    return (double)weight / fp_multiplier;
}

/* display a table content,
 * listing each element, its frequency, and its predicted bit cost */
MEM_STATIC void ZSTDN_debugTable(const U32* table, U32 max)
{
    unsigned u, sum;
    for (u=0, sum=0; u<=max; u++) sum += table[u];
    DEBUGLOG(2, "total nb elts: %u", sum);
    for (u=0; u<=max; u++) {
        DEBUGLOG(2, "%2u: %5u  (%.2f)",
                u, table[u], ZSTDN_fWeight(sum) - ZSTDN_fWeight(table[u]) );
    }
}

#endif



/* ===============================================================
 * Shared internal declarations
 * These prototypes may be called from sources not in lib/compress
 * =============================================================== */

/* ZSTDN_loadCEntropy() :
 * dict : must point at beginning of a valid zstd dictionary.
 * return : size of dictionary header (size of magic number + dict ID + entropy tables)
 * assumptions : magic number supposed already checked
 *               and dictSize >= 8 */
size_t ZSTDN_loadCEntropy(ZSTDN_compressedBlockState_t* bs, void* workspace,
                         const void* const dict, size_t dictSize);

void ZSTDN_reset_compressedBlockState(ZSTDN_compressedBlockState_t* bs);

/* ==============================================================
 * Private declarations
 * These prototypes shall only be called from within lib/compress
 * ============================================================== */

/* ZSTDN_getCParamsFromCCtxParams() :
 * cParams are built depending on compressionLevel, src size hints,
 * LDM and manually set compression parameters.
 * Note: srcSizeHint == 0 means 0!
 */
ZSTDN_compressionParameters ZSTDN_getCParamsFromCCtxParams(
        const ZSTDN_CCtx_params* CCtxParams, U64 srcSizeHint, size_t dictSize, ZSTDN_cParamMode_e mode);

/*! ZSTDN_initCStream_internal() :
 *  Private use only. Init streaming operation.
 *  expects params to be valid.
 *  must receive dict, or cdict, or none, but not both.
 *  @return : 0, or an error code */
size_t ZSTDN_initCStream_internal(ZSTDN_CStream* zcs,
                     const void* dict, size_t dictSize,
                     const ZSTDN_CDict* cdict,
                     const ZSTDN_CCtx_params* params, unsigned long long pledgedSrcSize);

void ZSTDN_resetSeqStore(seqStore_t* ssPtr);

/*! ZSTDN_getCParamsFromCDict() :
 *  as the name implies */
ZSTDN_compressionParameters ZSTDN_getCParamsFromCDict(const ZSTDN_CDict* cdict);

/* ZSTDN_compressBegin_advanced_internal() :
 * Private use only. To be called from zstdmt_compress.c. */
size_t ZSTDN_compressBegin_advanced_internal(ZSTDN_CCtx* cctx,
                                    const void* dict, size_t dictSize,
                                    ZSTDN_dictContentType_e dictContentType,
                                    ZSTDN_dictTableLoadMethod_e dtlm,
                                    const ZSTDN_CDict* cdict,
                                    const ZSTDN_CCtx_params* params,
                                    unsigned long long pledgedSrcSize);

/* ZSTDN_compress_advanced_internal() :
 * Private use only. To be called from zstdmt_compress.c. */
size_t ZSTDN_compress_advanced_internal(ZSTDN_CCtx* cctx,
                                       void* dst, size_t dstCapacity,
                                 const void* src, size_t srcSize,
                                 const void* dict,size_t dictSize,
                                 const ZSTDN_CCtx_params* params);


/* ZSTDN_writeLastEmptyBlock() :
 * output an empty Block with end-of-frame mark to complete a frame
 * @return : size of data written into `dst` (== ZSTDN_blockHeaderSize (defined in zstd_internal.h))
 *           or an error code if `dstCapacity` is too small (<ZSTDN_blockHeaderSize)
 */
size_t ZSTDN_writeLastEmptyBlock(void* dst, size_t dstCapacity);


/* ZSTDN_referenceExternalSequences() :
 * Must be called before starting a compression operation.
 * seqs must parse a prefix of the source.
 * This cannot be used when long range matching is enabled.
 * Zstd will use these sequences, and pass the literals to a secondary block
 * compressor.
 * @return : An error code on failure.
 * NOTE: seqs are not verified! Invalid sequences can cause out-of-bounds memory
 * access and data corruption.
 */
size_t ZSTDN_referenceExternalSequences(ZSTDN_CCtx* cctx, rawSeq* seq, size_t nbSeq);

/* ZSTDN_cycleLog() :
 *  condition for correct operation : hashLog > 1 */
U32 ZSTDN_cycleLog(U32 hashLog, ZSTDN_strategy strat);

/* ZSTDN_CCtx_trace() :
 *  Trace the end of a compression call.
 */
void ZSTDN_CCtx_trace(ZSTDN_CCtx* cctx, size_t extraCSize);

#endif /* ZSTDN_COMPRESS_H */
