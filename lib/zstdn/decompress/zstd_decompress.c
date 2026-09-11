/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


/* ***************************************************************
*  Tuning parameters
*****************************************************************/
/*!
 * HEAPMODE :
 * Select how default decompression function ZSTDN_decompress() allocates its context,
 * on stack (0), or into heap (1, default; requires malloc()).
 * Note that functions with explicit context such as ZSTDN_decompressDCtx() are unaffected.
 */
#ifndef ZSTDN_HEAPMODE
#  define ZSTDN_HEAPMODE 1
#endif

/*!
*  LEGACY_SUPPORT :
*  if set to 1+, ZSTDN_decompress() can decode older formats (v0.1+)
*/

/*!
 *  MAXWINDOWSIZE_DEFAULT :
 *  maximum window size accepted by DStream __by default__.
 *  Frames requiring more memory will be rejected.
 *  It's possible to set a different limit using ZSTDN_DCtx_setMaxWindowSize().
 */
#ifndef ZSTDN_MAXWINDOWSIZE_DEFAULT
#  define ZSTDN_MAXWINDOWSIZE_DEFAULT (((U32)1 << ZSTDN_WINDOWLOG_LIMIT_DEFAULT) + 1)
#endif

/*!
 *  NO_FORWARD_PROGRESS_MAX :
 *  maximum allowed nb of calls to ZSTDN_decompressStream()
 *  without any forward progress
 *  (defined as: no byte read from input, and no byte flushed to output)
 *  before triggering an error.
 */
#ifndef ZSTDN_NO_FORWARD_PROGRESS_MAX
#  define ZSTDN_NO_FORWARD_PROGRESS_MAX 16
#endif


/*-*******************************************************
*  Dependencies
*********************************************************/
#include "../common/zstd_deps.h"   /* ZSTDN_memcpy, ZSTDN_memmove, ZSTDN_memset */
#include "../common/cpu.h"         /* bmi2 */
#include "../common/mem.h"         /* low level memory routines */
#define FSEN_STATIC_LINKING_ONLY
#include "../common/fse.h"
#define HUFN_STATIC_LINKING_ONLY
#include "../common/huf.h"
#include "xxhash.h" /* xxh64n_reset, xxh64n_update, xxh64n_digest, XXH64 */
#include "../common/zstd_internal.h"  /* blockProperties_t */
#include "zstd_decompress_internal.h"   /* ZSTDN_DCtx */
#include "zstd_ddict.h"  /* ZSTDN_DDictDictContent */
#include "zstd_decompress_block.h"   /* ZSTDN_decompressBlock_internal */




/* ***********************************
 * Multiple DDicts Hashset internals *
 *************************************/

#define DDICT_HASHSET_MAX_LOAD_FACTOR_COUNT_MULT 4
#define DDICT_HASHSET_MAX_LOAD_FACTOR_SIZE_MULT 3   /* These two constants represent SIZE_MULT/COUNT_MULT load factor without using a float.
                                                     * Currently, that means a 0.75 load factor.
                                                     * So, if count * COUNT_MULT / size * SIZE_MULT != 0, then we've exceeded
                                                     * the load factor of the ddict hash set.
                                                     */

#define DDICT_HASHSET_TABLE_BASE_SIZE 64
#define DDICT_HASHSET_RESIZE_FACTOR 2

/* Hash function to determine starting position of dict insertion within the table
 * Returns an index between [0, hashSet->ddictPtrTableSize]
 */
static size_t ZSTDN_DDictHashSet_getIndex(const ZSTDN_DDictHashSet* hashSet, U32 dictID) {
    const U64 hash = xxh64n(&dictID, sizeof(U32), 0);
    /* DDict ptr table size is a multiple of 2, use size - 1 as mask to get index within [0, hashSet->ddictPtrTableSize) */
    return hash & (hashSet->ddictPtrTableSize - 1);
}

/* Adds DDict to a hashset without resizing it.
 * If inserting a DDict with a dictID that already exists in the set, replaces the one in the set.
 * Returns 0 if successful, or a zstd error code if something went wrong.
 */
static size_t ZSTDN_DDictHashSet_emplaceDDict(ZSTDN_DDictHashSet* hashSet, const ZSTDN_DDict* ddict) {
    const U32 dictID = ZSTDN_getDictID_fromDDict(ddict);
    size_t idx = ZSTDN_DDictHashSet_getIndex(hashSet, dictID);
    const size_t idxRangeMask = hashSet->ddictPtrTableSize - 1;
    RETURN_ERROR_IF(hashSet->ddictPtrCount == hashSet->ddictPtrTableSize, GENERIC, "Hash set is full!");
    DEBUGLOG(4, "Hashed index: for dictID: %u is %zu", dictID, idx);
    while (hashSet->ddictPtrTable[idx] != NULL) {
        /* Replace existing ddict if inserting ddict with same dictID */
        if (ZSTDN_getDictID_fromDDict(hashSet->ddictPtrTable[idx]) == dictID) {
            DEBUGLOG(4, "DictID already exists, replacing rather than adding");
            hashSet->ddictPtrTable[idx] = ddict;
            return 0;
        }
        idx &= idxRangeMask;
        idx++;
    }
    DEBUGLOG(4, "Final idx after probing for dictID %u is: %zu", dictID, idx);
    hashSet->ddictPtrTable[idx] = ddict;
    hashSet->ddictPtrCount++;
    return 0;
}

/* Expands hash table by factor of DDICT_HASHSET_RESIZE_FACTOR and
 * rehashes all values, allocates new table, frees old table.
 * Returns 0 on success, otherwise a zstd error code.
 */
static size_t ZSTDN_DDictHashSet_expand(ZSTDN_DDictHashSet* hashSet, ZSTDN_customMem customMem) {
    size_t newTableSize = hashSet->ddictPtrTableSize * DDICT_HASHSET_RESIZE_FACTOR;
    const ZSTDN_DDict** newTable = (const ZSTDN_DDict**)ZSTDN_customCalloc(sizeof(ZSTDN_DDict*) * newTableSize, customMem);
    const ZSTDN_DDict** oldTable = hashSet->ddictPtrTable;
    size_t oldTableSize = hashSet->ddictPtrTableSize;
    size_t i;

    DEBUGLOG(4, "Expanding DDict hash table! Old size: %zu new size: %zu", oldTableSize, newTableSize);
    RETURN_ERROR_IF(!newTable, memory_allocation, "Expanded hashset allocation failed!");
    hashSet->ddictPtrTable = newTable;
    hashSet->ddictPtrTableSize = newTableSize;
    hashSet->ddictPtrCount = 0;
    for (i = 0; i < oldTableSize; ++i) {
        if (oldTable[i] != NULL) {
            FORWARD_IF_ERROR(ZSTDN_DDictHashSet_emplaceDDict(hashSet, oldTable[i]), "");
        }
    }
    ZSTDN_customFree((void*)oldTable, customMem);
    DEBUGLOG(4, "Finished re-hash");
    return 0;
}

/* Fetches a DDict with the given dictID
 * Returns the ZSTDN_DDict* with the requested dictID. If it doesn't exist, then returns NULL.
 */
static const ZSTDN_DDict* ZSTDN_DDictHashSet_getDDict(ZSTDN_DDictHashSet* hashSet, U32 dictID) {
    size_t idx = ZSTDN_DDictHashSet_getIndex(hashSet, dictID);
    const size_t idxRangeMask = hashSet->ddictPtrTableSize - 1;
    DEBUGLOG(4, "Hashed index: for dictID: %u is %zu", dictID, idx);
    for (;;) {
        size_t currDictID = ZSTDN_getDictID_fromDDict(hashSet->ddictPtrTable[idx]);
        if (currDictID == dictID || currDictID == 0) {
            /* currDictID == 0 implies a NULL ddict entry */
            break;
        } else {
            idx &= idxRangeMask;    /* Goes to start of table when we reach the end */
            idx++;
        }
    }
    DEBUGLOG(4, "Final idx after probing for dictID %u is: %zu", dictID, idx);
    return hashSet->ddictPtrTable[idx];
}

/* Allocates space for and returns a ddict hash set
 * The hash set's ZSTDN_DDict* table has all values automatically set to NULL to begin with.
 * Returns NULL if allocation failed.
 */
static ZSTDN_DDictHashSet* ZSTDN_createDDictHashSet(ZSTDN_customMem customMem) {
    ZSTDN_DDictHashSet* ret = (ZSTDN_DDictHashSet*)ZSTDN_customMalloc(sizeof(ZSTDN_DDictHashSet), customMem);
    DEBUGLOG(4, "Allocating new hash set");
    if (!ret)
        return NULL;
    ret->ddictPtrTable = (const ZSTDN_DDict**)ZSTDN_customCalloc(DDICT_HASHSET_TABLE_BASE_SIZE * sizeof(ZSTDN_DDict*), customMem);
    if (!ret->ddictPtrTable) {
        ZSTDN_customFree(ret, customMem);
        return NULL;
    }
    ret->ddictPtrTableSize = DDICT_HASHSET_TABLE_BASE_SIZE;
    ret->ddictPtrCount = 0;
    return ret;
}

/* Frees the table of ZSTDN_DDict* within a hashset, then frees the hashset itself.
 * Note: The ZSTDN_DDict* within the table are NOT freed.
 */
static void ZSTDN_freeDDictHashSet(ZSTDN_DDictHashSet* hashSet, ZSTDN_customMem customMem) {
    DEBUGLOG(4, "Freeing ddict hash set");
    if (hashSet && hashSet->ddictPtrTable) {
        ZSTDN_customFree((void*)hashSet->ddictPtrTable, customMem);
    }
    if (hashSet) {
        ZSTDN_customFree(hashSet, customMem);
    }
}

/* Public function: Adds a DDict into the ZSTDN_DDictHashSet, possibly triggering a resize of the hash set.
 * Returns 0 on success, or a ZSTD error.
 */
static size_t ZSTDN_DDictHashSet_addDDict(ZSTDN_DDictHashSet* hashSet, const ZSTDN_DDict* ddict, ZSTDN_customMem customMem) {
    DEBUGLOG(4, "Adding dict ID: %u to hashset with - Count: %zu Tablesize: %zu", ZSTDN_getDictID_fromDDict(ddict), hashSet->ddictPtrCount, hashSet->ddictPtrTableSize);
    if (hashSet->ddictPtrCount * DDICT_HASHSET_MAX_LOAD_FACTOR_COUNT_MULT / hashSet->ddictPtrTableSize * DDICT_HASHSET_MAX_LOAD_FACTOR_SIZE_MULT != 0) {
        FORWARD_IF_ERROR(ZSTDN_DDictHashSet_expand(hashSet, customMem), "");
    }
    FORWARD_IF_ERROR(ZSTDN_DDictHashSet_emplaceDDict(hashSet, ddict), "");
    return 0;
}

/*-*************************************************************
*   Context management
***************************************************************/
size_t ZSTDN_sizeof_DCtx (const ZSTDN_DCtx* dctx)
{
    if (dctx==NULL) return 0;   /* support sizeof NULL */
    return sizeof(*dctx)
           + ZSTDN_sizeof_DDict(dctx->ddictLocal)
           + dctx->inBuffSize + dctx->outBuffSize;
}

size_t ZSTDN_estimateDCtxSize(void) { return sizeof(ZSTDN_DCtx); }


static size_t ZSTDN_startingInputLength(ZSTDN_format_e format)
{
    size_t const startingInputLength = ZSTDN_FRAMEHEADERSIZE_PREFIX(format);
    /* only supports formats ZSTDN_f_zstd1 and ZSTDN_f_zstd1_magicless */
    assert( (format == ZSTDN_f_zstd1) || (format == ZSTDN_f_zstd1_magicless) );
    return startingInputLength;
}

static void ZSTDN_DCtx_resetParameters(ZSTDN_DCtx* dctx)
{
    assert(dctx->streamStage == zdss_init);
    dctx->format = ZSTDN_f_zstd1;
    dctx->maxWindowSize = ZSTDN_MAXWINDOWSIZE_DEFAULT;
    dctx->outBufferMode = ZSTDN_bm_buffered;
    dctx->forceIgnoreChecksum = ZSTDN_d_validateChecksum;
    dctx->refMultipleDDicts = ZSTDN_rmd_refSingleDDict;
}

static void ZSTDN_initDCtx_internal(ZSTDN_DCtx* dctx)
{
    dctx->staticSize  = 0;
    dctx->ddict       = NULL;
    dctx->ddictLocal  = NULL;
    dctx->dictEnd     = NULL;
    dctx->ddictIsCold = 0;
    dctx->dictUses = ZSTDN_dont_use;
    dctx->inBuff      = NULL;
    dctx->inBuffSize  = 0;
    dctx->outBuffSize = 0;
    dctx->streamStage = zdss_init;
    dctx->legacyContext = NULL;
    dctx->previousLegacyVersion = 0;
    dctx->noForwardProgress = 0;
    dctx->oversizedDuration = 0;
    dctx->bmi2 = ZSTDN_cpuid_bmi2(ZSTDN_cpuid());
    dctx->ddictSet = NULL;
    ZSTDN_DCtx_resetParameters(dctx);
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    dctx->dictContentEndForFuzzing = NULL;
#endif
}

ZSTDN_DCtx* ZSTDN_initStaticDCtx(void *workspace, size_t workspaceSize)
{
    ZSTDN_DCtx* const dctx = (ZSTDN_DCtx*) workspace;

    if ((size_t)workspace & 7) return NULL;  /* 8-aligned */
    if (workspaceSize < sizeof(ZSTDN_DCtx)) return NULL;  /* minimum size */

    ZSTDN_initDCtx_internal(dctx);
    dctx->staticSize = workspaceSize;
    dctx->inBuff = (char*)(dctx+1);
    return dctx;
}

ZSTDN_DCtx* ZSTDN_createDCtx_advanced(ZSTDN_customMem customMem)
{
    if ((!customMem.customAlloc) ^ (!customMem.customFree)) return NULL;

    {   ZSTDN_DCtx* const dctx = (ZSTDN_DCtx*)ZSTDN_customMalloc(sizeof(*dctx), customMem);
        if (!dctx) return NULL;
        dctx->customMem = customMem;
        ZSTDN_initDCtx_internal(dctx);
        return dctx;
    }
}

ZSTDN_DCtx* ZSTDN_createDCtx(void)
{
    DEBUGLOG(3, "ZSTDN_createDCtx");
    return ZSTDN_createDCtx_advanced(ZSTDN_defaultCMem);
}

static void ZSTDN_clearDict(ZSTDN_DCtx* dctx)
{
    ZSTDN_freeDDict(dctx->ddictLocal);
    dctx->ddictLocal = NULL;
    dctx->ddict = NULL;
    dctx->dictUses = ZSTDN_dont_use;
}

size_t ZSTDN_freeDCtx(ZSTDN_DCtx* dctx)
{
    if (dctx==NULL) return 0;   /* support free on NULL */
    RETURN_ERROR_IF(dctx->staticSize, memory_allocation, "not compatible with static DCtx");
    {   ZSTDN_customMem const cMem = dctx->customMem;
        ZSTDN_clearDict(dctx);
        ZSTDN_customFree(dctx->inBuff, cMem);
        dctx->inBuff = NULL;
        if (dctx->ddictSet) {
            ZSTDN_freeDDictHashSet(dctx->ddictSet, cMem);
            dctx->ddictSet = NULL;
        }
        ZSTDN_customFree(dctx, cMem);
        return 0;
    }
}

/* no longer useful */
void ZSTDN_copyDCtx(ZSTDN_DCtx* dstDCtx, const ZSTDN_DCtx* srcDCtx)
{
    size_t const toCopy = (size_t)((char*)(&dstDCtx->inBuff) - (char*)dstDCtx);
    ZSTDN_memcpy(dstDCtx, srcDCtx, toCopy);  /* no need to copy workspace */
}

/* Given a dctx with a digested frame params, re-selects the correct ZSTDN_DDict based on
 * the requested dict ID from the frame. If there exists a reference to the correct ZSTDN_DDict, then
 * accordingly sets the ddict to be used to decompress the frame.
 *
 * If no DDict is found, then no action is taken, and the ZSTDN_DCtx::ddict remains as-is.
 *
 * ZSTDN_d_refMultipleDDicts must be enabled for this function to be called.
 */
static void ZSTDN_DCtx_selectFrameDDict(ZSTDN_DCtx* dctx) {
    assert(dctx->refMultipleDDicts && dctx->ddictSet);
    DEBUGLOG(4, "Adjusting DDict based on requested dict ID from frame");
    if (dctx->ddict) {
        const ZSTDN_DDict* frameDDict = ZSTDN_DDictHashSet_getDDict(dctx->ddictSet, dctx->fParams.dictID);
        if (frameDDict) {
            DEBUGLOG(4, "DDict found!");
            ZSTDN_clearDict(dctx);
            dctx->dictID = dctx->fParams.dictID;
            dctx->ddict = frameDDict;
            dctx->dictUses = ZSTDN_use_indefinitely;
        }
    }
}


/*-*************************************************************
 *   Frame header decoding
 ***************************************************************/

/*! ZSTDN_isFrame() :
 *  Tells if the content of `buffer` starts with a valid Frame Identifier.
 *  Note : Frame Identifier is 4 bytes. If `size < 4`, @return will always be 0.
 *  Note 2 : Legacy Frame Identifiers are considered valid only if Legacy Support is enabled.
 *  Note 3 : Skippable Frame Identifiers are considered valid. */
unsigned ZSTDN_isFrame(const void* buffer, size_t size)
{
    if (size < ZSTDN_FRAMEIDSIZE) return 0;
    {   U32 const magic = MEM_readLE32(buffer);
        if (magic == ZSTDN_MAGICNUMBER) return 1;
        if ((magic & ZSTDN_MAGIC_SKIPPABLE_MASK) == ZSTDN_MAGIC_SKIPPABLE_START) return 1;
    }
    return 0;
}

/* ZSTDN_frameHeaderSize_internal() :
 *  srcSize must be large enough to reach header size fields.
 *  note : only works for formats ZSTDN_f_zstd1 and ZSTDN_f_zstd1_magicless.
 * @return : size of the Frame Header
 *           or an error code, which can be tested with ZSTDN_isError() */
static size_t ZSTDN_frameHeaderSize_internal(const void* src, size_t srcSize, ZSTDN_format_e format)
{
    size_t const minInputSize = ZSTDN_startingInputLength(format);
    RETURN_ERROR_IF(srcSize < minInputSize, srcSize_wrong, "");

    {   BYTE const fhd = ((const BYTE*)src)[minInputSize-1];
        U32 const dictID= fhd & 3;
        U32 const singleSegment = (fhd >> 5) & 1;
        U32 const fcsId = fhd >> 6;
        return minInputSize + !singleSegment
             + ZSTDN_did_fieldSize[dictID] + ZSTDN_fcs_fieldSize[fcsId]
             + (singleSegment && !fcsId);
    }
}

/* ZSTDN_frameHeaderSize() :
 *  srcSize must be >= ZSTDN_frameHeaderSize_prefix.
 * @return : size of the Frame Header,
 *           or an error code (if srcSize is too small) */
size_t ZSTDN_frameHeaderSize(const void* src, size_t srcSize)
{
    return ZSTDN_frameHeaderSize_internal(src, srcSize, ZSTDN_f_zstd1);
}


/* ZSTDN_getFrameHeader_advanced() :
 *  decode Frame Header, or require larger `srcSize`.
 *  note : only works for formats ZSTDN_f_zstd1 and ZSTDN_f_zstd1_magicless
 * @return : 0, `zfhPtr` is correctly filled,
 *          >0, `srcSize` is too small, value is wanted `srcSize` amount,
 *           or an error code, which can be tested using ZSTDN_isError() */
size_t ZSTDN_getFrameHeader_advanced(ZSTDN_frameHeader* zfhPtr, const void* src, size_t srcSize, ZSTDN_format_e format)
{
    const BYTE* ip = (const BYTE*)src;
    size_t const minInputSize = ZSTDN_startingInputLength(format);

    ZSTDN_memset(zfhPtr, 0, sizeof(*zfhPtr));   /* not strictly necessary, but static analyzer do not understand that zfhPtr is only going to be read only if return value is zero, since they are 2 different signals */
    if (srcSize < minInputSize) return minInputSize;
    RETURN_ERROR_IF(src==NULL, GENERIC, "invalid parameter");

    if ( (format != ZSTDN_f_zstd1_magicless)
      && (MEM_readLE32(src) != ZSTDN_MAGICNUMBER) ) {
        if ((MEM_readLE32(src) & ZSTDN_MAGIC_SKIPPABLE_MASK) == ZSTDN_MAGIC_SKIPPABLE_START) {
            /* skippable frame */
            if (srcSize < ZSTDN_SKIPPABLEHEADERSIZE)
                return ZSTDN_SKIPPABLEHEADERSIZE; /* magic number + frame length */
            ZSTDN_memset(zfhPtr, 0, sizeof(*zfhPtr));
            zfhPtr->frameContentSize = MEM_readLE32((const char *)src + ZSTDN_FRAMEIDSIZE);
            zfhPtr->frameType = ZSTDN_skippableFrame;
            return 0;
        }
        RETURN_ERROR(prefix_unknown, "");
    }

    /* ensure there is enough `srcSize` to fully read/decode frame header */
    {   size_t const fhsize = ZSTDN_frameHeaderSize_internal(src, srcSize, format);
        if (srcSize < fhsize) return fhsize;
        zfhPtr->headerSize = (U32)fhsize;
    }

    {   BYTE const fhdByte = ip[minInputSize-1];
        size_t pos = minInputSize;
        U32 const dictIDSizeCode = fhdByte&3;
        U32 const checksumFlag = (fhdByte>>2)&1;
        U32 const singleSegment = (fhdByte>>5)&1;
        U32 const fcsID = fhdByte>>6;
        U64 windowSize = 0;
        U32 dictID = 0;
        U64 frameContentSize = ZSTDN_CONTENTSIZE_UNKNOWN;
        RETURN_ERROR_IF((fhdByte & 0x08) != 0, frameParameter_unsupported,
                        "reserved bits, must be zero");

        if (!singleSegment) {
            BYTE const wlByte = ip[pos++];
            U32 const windowLog = (wlByte >> 3) + ZSTDN_WINDOWLOG_ABSOLUTEMIN;
            RETURN_ERROR_IF(windowLog > ZSTDN_WINDOWLOG_MAX, frameParameter_windowTooLarge, "");
            windowSize = (1ULL << windowLog);
            windowSize += (windowSize >> 3) * (wlByte&7);
        }
        switch(dictIDSizeCode)
        {
            default:
                assert(0);  /* impossible */
                ZSTDN_FALLTHROUGH;
            case 0 : break;
            case 1 : dictID = ip[pos]; pos++; break;
            case 2 : dictID = MEM_readLE16(ip+pos); pos+=2; break;
            case 3 : dictID = MEM_readLE32(ip+pos); pos+=4; break;
        }
        switch(fcsID)
        {
            default:
                assert(0);  /* impossible */
                ZSTDN_FALLTHROUGH;
            case 0 : if (singleSegment) frameContentSize = ip[pos]; break;
            case 1 : frameContentSize = MEM_readLE16(ip+pos)+256; break;
            case 2 : frameContentSize = MEM_readLE32(ip+pos); break;
            case 3 : frameContentSize = MEM_readLE64(ip+pos); break;
        }
        if (singleSegment) windowSize = frameContentSize;

        zfhPtr->frameType = ZSTDN_frame;
        zfhPtr->frameContentSize = frameContentSize;
        zfhPtr->windowSize = windowSize;
        zfhPtr->blockSizeMax = (unsigned) MIN(windowSize, ZSTDN_BLOCKSIZE_MAX);
        zfhPtr->dictID = dictID;
        zfhPtr->checksumFlag = checksumFlag;
    }
    return 0;
}

/* ZSTDN_getFrameHeader() :
 *  decode Frame Header, or require larger `srcSize`.
 *  note : this function does not consume input, it only reads it.
 * @return : 0, `zfhPtr` is correctly filled,
 *          >0, `srcSize` is too small, value is wanted `srcSize` amount,
 *           or an error code, which can be tested using ZSTDN_isError() */
size_t ZSTDN_getFrameHeader(ZSTDN_frameHeader* zfhPtr, const void* src, size_t srcSize)
{
    return ZSTDN_getFrameHeader_advanced(zfhPtr, src, srcSize, ZSTDN_f_zstd1);
}


/* ZSTDN_getFrameContentSize() :
 *  compatible with legacy mode
 * @return : decompressed size of the single frame pointed to be `src` if known, otherwise
 *         - ZSTDN_CONTENTSIZE_UNKNOWN if the size cannot be determined
 *         - ZSTDN_CONTENTSIZE_ERROR if an error occurred (e.g. invalid magic number, srcSize too small) */
unsigned long long ZSTDN_getFrameContentSize(const void *src, size_t srcSize)
{
    {   ZSTDN_frameHeader zfh;
        if (ZSTDN_getFrameHeader(&zfh, src, srcSize) != 0)
            return ZSTDN_CONTENTSIZE_ERROR;
        if (zfh.frameType == ZSTDN_skippableFrame) {
            return 0;
        } else {
            return zfh.frameContentSize;
    }   }
}

static size_t readSkippableFrameSize(void const* src, size_t srcSize)
{
    size_t const skippableHeaderSize = ZSTDN_SKIPPABLEHEADERSIZE;
    U32 sizeU32;

    RETURN_ERROR_IF(srcSize < ZSTDN_SKIPPABLEHEADERSIZE, srcSize_wrong, "");

    sizeU32 = MEM_readLE32((BYTE const*)src + ZSTDN_FRAMEIDSIZE);
    RETURN_ERROR_IF((U32)(sizeU32 + ZSTDN_SKIPPABLEHEADERSIZE) < sizeU32,
                    frameParameter_unsupported, "");
    {
        size_t const skippableSize = skippableHeaderSize + sizeU32;
        RETURN_ERROR_IF(skippableSize > srcSize, srcSize_wrong, "");
        return skippableSize;
    }
}

/* ZSTDN_findDecompressedSize() :
 *  compatible with legacy mode
 *  `srcSize` must be the exact length of some number of ZSTD compressed and/or
 *      skippable frames
 *  @return : decompressed size of the frames contained */
unsigned long long ZSTDN_findDecompressedSize(const void* src, size_t srcSize)
{
    unsigned long long totalDstSize = 0;

    while (srcSize >= ZSTDN_startingInputLength(ZSTDN_f_zstd1)) {
        U32 const magicNumber = MEM_readLE32(src);

        if ((magicNumber & ZSTDN_MAGIC_SKIPPABLE_MASK) == ZSTDN_MAGIC_SKIPPABLE_START) {
            size_t const skippableSize = readSkippableFrameSize(src, srcSize);
            if (ZSTDN_isError(skippableSize)) {
                return ZSTDN_CONTENTSIZE_ERROR;
            }
            assert(skippableSize <= srcSize);

            src = (const BYTE *)src + skippableSize;
            srcSize -= skippableSize;
            continue;
        }

        {   unsigned long long const ret = ZSTDN_getFrameContentSize(src, srcSize);
            if (ret >= ZSTDN_CONTENTSIZE_ERROR) return ret;

            /* check for overflow */
            if (totalDstSize + ret < totalDstSize) return ZSTDN_CONTENTSIZE_ERROR;
            totalDstSize += ret;
        }
        {   size_t const frameSrcSize = ZSTDN_findFrameCompressedSize(src, srcSize);
            if (ZSTDN_isError(frameSrcSize)) {
                return ZSTDN_CONTENTSIZE_ERROR;
            }

            src = (const BYTE *)src + frameSrcSize;
            srcSize -= frameSrcSize;
        }
    }  /* while (srcSize >= ZSTDN_frameHeaderSize_prefix) */

    if (srcSize) return ZSTDN_CONTENTSIZE_ERROR;

    return totalDstSize;
}

/* ZSTDN_getDecompressedSize() :
 *  compatible with legacy mode
 * @return : decompressed size if known, 0 otherwise
             note : 0 can mean any of the following :
                   - frame content is empty
                   - decompressed size field is not present in frame header
                   - frame header unknown / not supported
                   - frame header not complete (`srcSize` too small) */
unsigned long long ZSTDN_getDecompressedSize(const void* src, size_t srcSize)
{
    unsigned long long const ret = ZSTDN_getFrameContentSize(src, srcSize);
    ZSTDN_STATIC_ASSERT(ZSTDN_CONTENTSIZE_ERROR < ZSTDN_CONTENTSIZE_UNKNOWN);
    return (ret >= ZSTDN_CONTENTSIZE_ERROR) ? 0 : ret;
}


/* ZSTDN_decodeFrameHeader() :
 * `headerSize` must be the size provided by ZSTDN_frameHeaderSize().
 * If multiple DDict references are enabled, also will choose the correct DDict to use.
 * @return : 0 if success, or an error code, which can be tested using ZSTDN_isError() */
static size_t ZSTDN_decodeFrameHeader(ZSTDN_DCtx* dctx, const void* src, size_t headerSize)
{
    size_t const result = ZSTDN_getFrameHeader_advanced(&(dctx->fParams), src, headerSize, dctx->format);
    if (ZSTDN_isError(result)) return result;    /* invalid header */
    RETURN_ERROR_IF(result>0, srcSize_wrong, "headerSize too small");

    /* Reference DDict requested by frame if dctx references multiple ddicts */
    if (dctx->refMultipleDDicts == ZSTDN_rmd_refMultipleDDicts && dctx->ddictSet) {
        ZSTDN_DCtx_selectFrameDDict(dctx);
    }

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    /* Skip the dictID check in fuzzing mode, because it makes the search
     * harder.
     */
    RETURN_ERROR_IF(dctx->fParams.dictID && (dctx->dictID != dctx->fParams.dictID),
                    dictionary_wrong, "");
#endif
    dctx->validateChecksum = (dctx->fParams.checksumFlag && !dctx->forceIgnoreChecksum) ? 1 : 0;
    if (dctx->validateChecksum) xxh64n_reset(&dctx->xxhState, 0);
    dctx->processedCSize += headerSize;
    return 0;
}

static ZSTDN_frameSizeInfo ZSTDN_errorFrameSizeInfo(size_t ret)
{
    ZSTDN_frameSizeInfo frameSizeInfo;
    frameSizeInfo.compressedSize = ret;
    frameSizeInfo.decompressedBound = ZSTDN_CONTENTSIZE_ERROR;
    return frameSizeInfo;
}

static ZSTDN_frameSizeInfo ZSTDN_findFrameSizeInfo(const void* src, size_t srcSize)
{
    ZSTDN_frameSizeInfo frameSizeInfo;
    ZSTDN_memset(&frameSizeInfo, 0, sizeof(ZSTDN_frameSizeInfo));


    if ((srcSize >= ZSTDN_SKIPPABLEHEADERSIZE)
        && (MEM_readLE32(src) & ZSTDN_MAGIC_SKIPPABLE_MASK) == ZSTDN_MAGIC_SKIPPABLE_START) {
        frameSizeInfo.compressedSize = readSkippableFrameSize(src, srcSize);
        assert(ZSTDN_isError(frameSizeInfo.compressedSize) ||
               frameSizeInfo.compressedSize <= srcSize);
        return frameSizeInfo;
    } else {
        const BYTE* ip = (const BYTE*)src;
        const BYTE* const ipstart = ip;
        size_t remainingSize = srcSize;
        size_t nbBlocks = 0;
        ZSTDN_frameHeader zfh;

        /* Extract Frame Header */
        {   size_t const ret = ZSTDN_getFrameHeader(&zfh, src, srcSize);
            if (ZSTDN_isError(ret))
                return ZSTDN_errorFrameSizeInfo(ret);
            if (ret > 0)
                return ZSTDN_errorFrameSizeInfo(ERROR(srcSize_wrong));
        }

        ip += zfh.headerSize;
        remainingSize -= zfh.headerSize;

        /* Iterate over each block */
        while (1) {
            blockProperties_t blockProperties;
            size_t const cBlockSize = ZSTDN_getcBlockSize(ip, remainingSize, &blockProperties);
            if (ZSTDN_isError(cBlockSize))
                return ZSTDN_errorFrameSizeInfo(cBlockSize);

            if (ZSTDN_blockHeaderSize + cBlockSize > remainingSize)
                return ZSTDN_errorFrameSizeInfo(ERROR(srcSize_wrong));

            ip += ZSTDN_blockHeaderSize + cBlockSize;
            remainingSize -= ZSTDN_blockHeaderSize + cBlockSize;
            nbBlocks++;

            if (blockProperties.lastBlock) break;
        }

        /* Final frame content checksum */
        if (zfh.checksumFlag) {
            if (remainingSize < 4)
                return ZSTDN_errorFrameSizeInfo(ERROR(srcSize_wrong));
            ip += 4;
        }

        frameSizeInfo.compressedSize = (size_t)(ip - ipstart);
        frameSizeInfo.decompressedBound = (zfh.frameContentSize != ZSTDN_CONTENTSIZE_UNKNOWN)
                                        ? zfh.frameContentSize
                                        : nbBlocks * zfh.blockSizeMax;
        return frameSizeInfo;
    }
}

/* ZSTDN_findFrameCompressedSize() :
 *  compatible with legacy mode
 *  `src` must point to the start of a ZSTD frame, ZSTD legacy frame, or skippable frame
 *  `srcSize` must be at least as large as the frame contained
 *  @return : the compressed size of the frame starting at `src` */
size_t ZSTDN_findFrameCompressedSize(const void *src, size_t srcSize)
{
    ZSTDN_frameSizeInfo const frameSizeInfo = ZSTDN_findFrameSizeInfo(src, srcSize);
    return frameSizeInfo.compressedSize;
}

/* ZSTDN_decompressBound() :
 *  compatible with legacy mode
 *  `src` must point to the start of a ZSTD frame or a skippeable frame
 *  `srcSize` must be at least as large as the frame contained
 *  @return : the maximum decompressed size of the compressed source
 */
unsigned long long ZSTDN_decompressBound(const void* src, size_t srcSize)
{
    unsigned long long bound = 0;
    /* Iterate over each frame */
    while (srcSize > 0) {
        ZSTDN_frameSizeInfo const frameSizeInfo = ZSTDN_findFrameSizeInfo(src, srcSize);
        size_t const compressedSize = frameSizeInfo.compressedSize;
        unsigned long long const decompressedBound = frameSizeInfo.decompressedBound;
        if (ZSTDN_isError(compressedSize) || decompressedBound == ZSTDN_CONTENTSIZE_ERROR)
            return ZSTDN_CONTENTSIZE_ERROR;
        assert(srcSize >= compressedSize);
        src = (const BYTE*)src + compressedSize;
        srcSize -= compressedSize;
        bound += decompressedBound;
    }
    return bound;
}


/*-*************************************************************
 *   Frame decoding
 ***************************************************************/

/* ZSTDN_insertBlock() :
 *  insert `src` block into `dctx` history. Useful to track uncompressed blocks. */
size_t ZSTDN_insertBlock(ZSTDN_DCtx* dctx, const void* blockStart, size_t blockSize)
{
    DEBUGLOG(5, "ZSTDN_insertBlock: %u bytes", (unsigned)blockSize);
    ZSTDN_checkContinuity(dctx, blockStart, blockSize);
    dctx->previousDstEnd = (const char*)blockStart + blockSize;
    return blockSize;
}


static size_t ZSTDN_copyRawBlock(void* dst, size_t dstCapacity,
                          const void* src, size_t srcSize)
{
    DEBUGLOG(5, "ZSTDN_copyRawBlock");
    RETURN_ERROR_IF(srcSize > dstCapacity, dstSize_tooSmall, "");
    if (dst == NULL) {
        if (srcSize == 0) return 0;
        RETURN_ERROR(dstBuffer_null, "");
    }
    ZSTDN_memcpy(dst, src, srcSize);
    return srcSize;
}

static size_t ZSTDN_setRleBlock(void* dst, size_t dstCapacity,
                               BYTE b,
                               size_t regenSize)
{
    RETURN_ERROR_IF(regenSize > dstCapacity, dstSize_tooSmall, "");
    if (dst == NULL) {
        if (regenSize == 0) return 0;
        RETURN_ERROR(dstBuffer_null, "");
    }
    ZSTDN_memset(dst, b, regenSize);
    return regenSize;
}

static void ZSTDN_DCtx_trace_end(ZSTDN_DCtx const* dctx, U64 uncompressedSize, U64 compressedSize, unsigned streaming)
{
    (void)dctx;
    (void)uncompressedSize;
    (void)compressedSize;
    (void)streaming;
}


/*! ZSTDN_decompressFrame() :
 * @dctx must be properly initialized
 *  will update *srcPtr and *srcSizePtr,
 *  to make *srcPtr progress by one frame. */
static size_t ZSTDN_decompressFrame(ZSTDN_DCtx* dctx,
                                   void* dst, size_t dstCapacity,
                             const void** srcPtr, size_t *srcSizePtr)
{
    const BYTE* const istart = (const BYTE*)(*srcPtr);
    const BYTE* ip = istart;
    BYTE* const ostart = (BYTE*)dst;
    BYTE* const oend = dstCapacity != 0 ? ostart + dstCapacity : ostart;
    BYTE* op = ostart;
    size_t remainingSrcSize = *srcSizePtr;

    DEBUGLOG(4, "ZSTDN_decompressFrame (srcSize:%i)", (int)*srcSizePtr);

    /* check */
    RETURN_ERROR_IF(
        remainingSrcSize < ZSTDN_FRAMEHEADERSIZE_MIN(dctx->format)+ZSTDN_blockHeaderSize,
        srcSize_wrong, "");

    /* Frame Header */
    {   size_t const frameHeaderSize = ZSTDN_frameHeaderSize_internal(
                ip, ZSTDN_FRAMEHEADERSIZE_PREFIX(dctx->format), dctx->format);
        if (ZSTDN_isError(frameHeaderSize)) return frameHeaderSize;
        RETURN_ERROR_IF(remainingSrcSize < frameHeaderSize+ZSTDN_blockHeaderSize,
                        srcSize_wrong, "");
        FORWARD_IF_ERROR( ZSTDN_decodeFrameHeader(dctx, ip, frameHeaderSize) , "");
        ip += frameHeaderSize; remainingSrcSize -= frameHeaderSize;
    }

    /* Loop on each block */
    while (1) {
        size_t decodedSize;
        blockProperties_t blockProperties;
        size_t const cBlockSize = ZSTDN_getcBlockSize(ip, remainingSrcSize, &blockProperties);
        if (ZSTDN_isError(cBlockSize)) return cBlockSize;

        ip += ZSTDN_blockHeaderSize;
        remainingSrcSize -= ZSTDN_blockHeaderSize;
        RETURN_ERROR_IF(cBlockSize > remainingSrcSize, srcSize_wrong, "");

        switch(blockProperties.blockType)
        {
        case bt_compressed:
            decodedSize = ZSTDN_decompressBlock_internal(dctx, op, (size_t)(oend-op), ip, cBlockSize, /* frame */ 1);
            break;
        case bt_raw :
            decodedSize = ZSTDN_copyRawBlock(op, (size_t)(oend-op), ip, cBlockSize);
            break;
        case bt_rle :
            decodedSize = ZSTDN_setRleBlock(op, (size_t)(oend-op), *ip, blockProperties.origSize);
            break;
        case bt_reserved :
        default:
            RETURN_ERROR(corruption_detected, "invalid block type");
        }

        if (ZSTDN_isError(decodedSize)) return decodedSize;
        if (dctx->validateChecksum)
            xxh64n_update(&dctx->xxhState, op, decodedSize);
        if (decodedSize != 0)
            op += decodedSize;
        assert(ip != NULL);
        ip += cBlockSize;
        remainingSrcSize -= cBlockSize;
        if (blockProperties.lastBlock) break;
    }

    if (dctx->fParams.frameContentSize != ZSTDN_CONTENTSIZE_UNKNOWN) {
        RETURN_ERROR_IF((U64)(op-ostart) != dctx->fParams.frameContentSize,
                        corruption_detected, "");
    }
    if (dctx->fParams.checksumFlag) { /* Frame content checksum verification */
        RETURN_ERROR_IF(remainingSrcSize<4, checksum_wrong, "");
        if (!dctx->forceIgnoreChecksum) {
            U32 const checkCalc = (U32)xxh64n_digest(&dctx->xxhState);
            U32 checkRead;
            checkRead = MEM_readLE32(ip);
            RETURN_ERROR_IF(checkRead != checkCalc, checksum_wrong, "");
        }
        ip += 4;
        remainingSrcSize -= 4;
    }
    ZSTDN_DCtx_trace_end(dctx, (U64)(op-ostart), (U64)(ip-istart), /* streaming */ 0);
    /* Allow caller to get size read */
    *srcPtr = ip;
    *srcSizePtr = remainingSrcSize;
    return (size_t)(op-ostart);
}

static size_t ZSTDN_decompressMultiFrame(ZSTDN_DCtx* dctx,
                                        void* dst, size_t dstCapacity,
                                  const void* src, size_t srcSize,
                                  const void* dict, size_t dictSize,
                                  const ZSTDN_DDict* ddict)
{
    void* const dststart = dst;
    int moreThan1Frame = 0;

    DEBUGLOG(5, "ZSTDN_decompressMultiFrame");
    assert(dict==NULL || ddict==NULL);  /* either dict or ddict set, not both */

    if (ddict) {
        dict = ZSTDN_DDict_dictContent(ddict);
        dictSize = ZSTDN_DDict_dictSize(ddict);
    }

    while (srcSize >= ZSTDN_startingInputLength(dctx->format)) {


        {   U32 const magicNumber = MEM_readLE32(src);
            DEBUGLOG(4, "reading magic number %08X (expecting %08X)",
                        (unsigned)magicNumber, ZSTDN_MAGICNUMBER);
            if ((magicNumber & ZSTDN_MAGIC_SKIPPABLE_MASK) == ZSTDN_MAGIC_SKIPPABLE_START) {
                size_t const skippableSize = readSkippableFrameSize(src, srcSize);
                FORWARD_IF_ERROR(skippableSize, "readSkippableFrameSize failed");
                assert(skippableSize <= srcSize);

                src = (const BYTE *)src + skippableSize;
                srcSize -= skippableSize;
                continue;
        }   }

        if (ddict) {
            /* we were called from ZSTDN_decompress_usingDDict */
            FORWARD_IF_ERROR(ZSTDN_decompressBegin_usingDDict(dctx, ddict), "");
        } else {
            /* this will initialize correctly with no dict if dict == NULL, so
             * use this in all cases but ddict */
            FORWARD_IF_ERROR(ZSTDN_decompressBegin_usingDict(dctx, dict, dictSize), "");
        }
        ZSTDN_checkContinuity(dctx, dst, dstCapacity);

        {   const size_t res = ZSTDN_decompressFrame(dctx, dst, dstCapacity,
                                                    &src, &srcSize);
            RETURN_ERROR_IF(
                (ZSTDN_getErrorCode(res) == ZSTDN_error_prefix_unknown)
             && (moreThan1Frame==1),
                srcSize_wrong,
                "At least one frame successfully completed, "
                "but following bytes are garbage: "
                "it's more likely to be a srcSize error, "
                "specifying more input bytes than size of frame(s). "
                "Note: one could be unlucky, it might be a corruption error instead, "
                "happening right at the place where we expect zstd magic bytes. "
                "But this is _much_ less likely than a srcSize field error.");
            if (ZSTDN_isError(res)) return res;
            assert(res <= dstCapacity);
            if (res != 0)
                dst = (BYTE*)dst + res;
            dstCapacity -= res;
        }
        moreThan1Frame = 1;
    }  /* while (srcSize >= ZSTDN_frameHeaderSize_prefix) */

    RETURN_ERROR_IF(srcSize, srcSize_wrong, "input not entirely consumed");

    return (size_t)((BYTE*)dst - (BYTE*)dststart);
}

size_t ZSTDN_decompress_usingDict(ZSTDN_DCtx* dctx,
                                 void* dst, size_t dstCapacity,
                           const void* src, size_t srcSize,
                           const void* dict, size_t dictSize)
{
    return ZSTDN_decompressMultiFrame(dctx, dst, dstCapacity, src, srcSize, dict, dictSize, NULL);
}


static ZSTDN_DDict const* ZSTDN_getDDict(ZSTDN_DCtx* dctx)
{
    switch (dctx->dictUses) {
    default:
        assert(0 /* Impossible */);
        ZSTDN_FALLTHROUGH;
    case ZSTDN_dont_use:
        ZSTDN_clearDict(dctx);
        return NULL;
    case ZSTDN_use_indefinitely:
        return dctx->ddict;
    case ZSTDN_use_once:
        dctx->dictUses = ZSTDN_dont_use;
        return dctx->ddict;
    }
}

size_t ZSTDN_decompressDCtx(ZSTDN_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize)
{
    return ZSTDN_decompress_usingDDict(dctx, dst, dstCapacity, src, srcSize, ZSTDN_getDDict(dctx));
}


size_t ZSTDN_decompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize)
{
#if defined(ZSTDN_HEAPMODE) && (ZSTDN_HEAPMODE>=1)
    size_t regenSize;
    ZSTDN_DCtx* const dctx = ZSTDN_createDCtx();
    RETURN_ERROR_IF(dctx==NULL, memory_allocation, "NULL pointer!");
    regenSize = ZSTDN_decompressDCtx(dctx, dst, dstCapacity, src, srcSize);
    ZSTDN_freeDCtx(dctx);
    return regenSize;
#else   /* stack mode */
    ZSTDN_DCtx dctx;
    ZSTDN_initDCtx_internal(&dctx);
    return ZSTDN_decompressDCtx(&dctx, dst, dstCapacity, src, srcSize);
#endif
}


/*-**************************************
*   Advanced Streaming Decompression API
*   Bufferless and synchronous
****************************************/
size_t ZSTDN_nextSrcSizeToDecompress(ZSTDN_DCtx* dctx) { return dctx->expected; }

/*
 * Similar to ZSTDN_nextSrcSizeToDecompress(), but when when a block input can be streamed,
 * we allow taking a partial block as the input. Currently only raw uncompressed blocks can
 * be streamed.
 *
 * For blocks that can be streamed, this allows us to reduce the latency until we produce
 * output, and avoid copying the input.
 *
 * @param inputSize - The total amount of input that the caller currently has.
 */
static size_t ZSTDN_nextSrcSizeToDecompressWithInputSize(ZSTDN_DCtx* dctx, size_t inputSize) {
    if (!(dctx->stage == ZSTDds_decompressBlock || dctx->stage == ZSTDds_decompressLastBlock))
        return dctx->expected;
    if (dctx->bType != bt_raw)
        return dctx->expected;
    return MIN(MAX(inputSize, 1), dctx->expected);
}

ZSTDN_nextInputType_e ZSTDN_nextInputType(ZSTDN_DCtx* dctx) {
    switch(dctx->stage)
    {
    default:   /* should not happen */
        assert(0);
        ZSTDN_FALLTHROUGH;
    case ZSTDds_getFrameHeaderSize:
        ZSTDN_FALLTHROUGH;
    case ZSTDds_decodeFrameHeader:
        return ZSTDnit_frameHeader;
    case ZSTDds_decodeBlockHeader:
        return ZSTDnit_blockHeader;
    case ZSTDds_decompressBlock:
        return ZSTDnit_block;
    case ZSTDds_decompressLastBlock:
        return ZSTDnit_lastBlock;
    case ZSTDds_checkChecksum:
        return ZSTDnit_checksum;
    case ZSTDds_decodeSkippableHeader:
        ZSTDN_FALLTHROUGH;
    case ZSTDds_skipFrame:
        return ZSTDnit_skippableFrame;
    }
}

static int ZSTDN_isSkipFrame(ZSTDN_DCtx* dctx) { return dctx->stage == ZSTDds_skipFrame; }

/* ZSTDN_decompressContinue() :
 *  srcSize : must be the exact nb of bytes expected (see ZSTDN_nextSrcSizeToDecompress())
 *  @return : nb of bytes generated into `dst` (necessarily <= `dstCapacity)
 *            or an error code, which can be tested using ZSTDN_isError() */
size_t ZSTDN_decompressContinue(ZSTDN_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize)
{
    DEBUGLOG(5, "ZSTDN_decompressContinue (srcSize:%u)", (unsigned)srcSize);
    /* Sanity check */
    RETURN_ERROR_IF(srcSize != ZSTDN_nextSrcSizeToDecompressWithInputSize(dctx, srcSize), srcSize_wrong, "not allowed");
    ZSTDN_checkContinuity(dctx, dst, dstCapacity);

    dctx->processedCSize += srcSize;

    switch (dctx->stage)
    {
    case ZSTDds_getFrameHeaderSize :
        assert(src != NULL);
        if (dctx->format == ZSTDN_f_zstd1) {  /* allows header */
            assert(srcSize >= ZSTDN_FRAMEIDSIZE);  /* to read skippable magic number */
            if ((MEM_readLE32(src) & ZSTDN_MAGIC_SKIPPABLE_MASK) == ZSTDN_MAGIC_SKIPPABLE_START) {        /* skippable frame */
                ZSTDN_memcpy(dctx->headerBuffer, src, srcSize);
                dctx->expected = ZSTDN_SKIPPABLEHEADERSIZE - srcSize;  /* remaining to load to get full skippable frame header */
                dctx->stage = ZSTDds_decodeSkippableHeader;
                return 0;
        }   }
        dctx->headerSize = ZSTDN_frameHeaderSize_internal(src, srcSize, dctx->format);
        if (ZSTDN_isError(dctx->headerSize)) return dctx->headerSize;
        ZSTDN_memcpy(dctx->headerBuffer, src, srcSize);
        dctx->expected = dctx->headerSize - srcSize;
        dctx->stage = ZSTDds_decodeFrameHeader;
        return 0;

    case ZSTDds_decodeFrameHeader:
        assert(src != NULL);
        ZSTDN_memcpy(dctx->headerBuffer + (dctx->headerSize - srcSize), src, srcSize);
        FORWARD_IF_ERROR(ZSTDN_decodeFrameHeader(dctx, dctx->headerBuffer, dctx->headerSize), "");
        dctx->expected = ZSTDN_blockHeaderSize;
        dctx->stage = ZSTDds_decodeBlockHeader;
        return 0;

    case ZSTDds_decodeBlockHeader:
        {   blockProperties_t bp;
            size_t const cBlockSize = ZSTDN_getcBlockSize(src, ZSTDN_blockHeaderSize, &bp);
            if (ZSTDN_isError(cBlockSize)) return cBlockSize;
            RETURN_ERROR_IF(cBlockSize > dctx->fParams.blockSizeMax, corruption_detected, "Block Size Exceeds Maximum");
            dctx->expected = cBlockSize;
            dctx->bType = bp.blockType;
            dctx->rleSize = bp.origSize;
            if (cBlockSize) {
                dctx->stage = bp.lastBlock ? ZSTDds_decompressLastBlock : ZSTDds_decompressBlock;
                return 0;
            }
            /* empty block */
            if (bp.lastBlock) {
                if (dctx->fParams.checksumFlag) {
                    dctx->expected = 4;
                    dctx->stage = ZSTDds_checkChecksum;
                } else {
                    dctx->expected = 0; /* end of frame */
                    dctx->stage = ZSTDds_getFrameHeaderSize;
                }
            } else {
                dctx->expected = ZSTDN_blockHeaderSize;  /* jump to next header */
                dctx->stage = ZSTDds_decodeBlockHeader;
            }
            return 0;
        }

    case ZSTDds_decompressLastBlock:
    case ZSTDds_decompressBlock:
        DEBUGLOG(5, "ZSTDN_decompressContinue: case ZSTDds_decompressBlock");
        {   size_t rSize;
            switch(dctx->bType)
            {
            case bt_compressed:
                DEBUGLOG(5, "ZSTDN_decompressContinue: case bt_compressed");
                rSize = ZSTDN_decompressBlock_internal(dctx, dst, dstCapacity, src, srcSize, /* frame */ 1);
                dctx->expected = 0;  /* Streaming not supported */
                break;
            case bt_raw :
                assert(srcSize <= dctx->expected);
                rSize = ZSTDN_copyRawBlock(dst, dstCapacity, src, srcSize);
                FORWARD_IF_ERROR(rSize, "ZSTDN_copyRawBlock failed");
                assert(rSize == srcSize);
                dctx->expected -= rSize;
                break;
            case bt_rle :
                rSize = ZSTDN_setRleBlock(dst, dstCapacity, *(const BYTE*)src, dctx->rleSize);
                dctx->expected = 0;  /* Streaming not supported */
                break;
            case bt_reserved :   /* should never happen */
            default:
                RETURN_ERROR(corruption_detected, "invalid block type");
            }
            FORWARD_IF_ERROR(rSize, "");
            RETURN_ERROR_IF(rSize > dctx->fParams.blockSizeMax, corruption_detected, "Decompressed Block Size Exceeds Maximum");
            DEBUGLOG(5, "ZSTDN_decompressContinue: decoded size from block : %u", (unsigned)rSize);
            dctx->decodedSize += rSize;
            if (dctx->validateChecksum) xxh64n_update(&dctx->xxhState, dst, rSize);
            dctx->previousDstEnd = (char*)dst + rSize;

            /* Stay on the same stage until we are finished streaming the block. */
            if (dctx->expected > 0) {
                return rSize;
            }

            if (dctx->stage == ZSTDds_decompressLastBlock) {   /* end of frame */
                DEBUGLOG(4, "ZSTDN_decompressContinue: decoded size from frame : %u", (unsigned)dctx->decodedSize);
                RETURN_ERROR_IF(
                    dctx->fParams.frameContentSize != ZSTDN_CONTENTSIZE_UNKNOWN
                 && dctx->decodedSize != dctx->fParams.frameContentSize,
                    corruption_detected, "");
                if (dctx->fParams.checksumFlag) {  /* another round for frame checksum */
                    dctx->expected = 4;
                    dctx->stage = ZSTDds_checkChecksum;
                } else {
                    ZSTDN_DCtx_trace_end(dctx, dctx->decodedSize, dctx->processedCSize, /* streaming */ 1);
                    dctx->expected = 0;   /* ends here */
                    dctx->stage = ZSTDds_getFrameHeaderSize;
                }
            } else {
                dctx->stage = ZSTDds_decodeBlockHeader;
                dctx->expected = ZSTDN_blockHeaderSize;
            }
            return rSize;
        }

    case ZSTDds_checkChecksum:
        assert(srcSize == 4);  /* guaranteed by dctx->expected */
        {
            if (dctx->validateChecksum) {
                U32 const h32 = (U32)xxh64n_digest(&dctx->xxhState);
                U32 const check32 = MEM_readLE32(src);
                DEBUGLOG(4, "ZSTDN_decompressContinue: checksum : calculated %08X :: %08X read", (unsigned)h32, (unsigned)check32);
                RETURN_ERROR_IF(check32 != h32, checksum_wrong, "");
            }
            ZSTDN_DCtx_trace_end(dctx, dctx->decodedSize, dctx->processedCSize, /* streaming */ 1);
            dctx->expected = 0;
            dctx->stage = ZSTDds_getFrameHeaderSize;
            return 0;
        }

    case ZSTDds_decodeSkippableHeader:
        assert(src != NULL);
        assert(srcSize <= ZSTDN_SKIPPABLEHEADERSIZE);
        ZSTDN_memcpy(dctx->headerBuffer + (ZSTDN_SKIPPABLEHEADERSIZE - srcSize), src, srcSize);   /* complete skippable header */
        dctx->expected = MEM_readLE32(dctx->headerBuffer + ZSTDN_FRAMEIDSIZE);   /* note : dctx->expected can grow seriously large, beyond local buffer size */
        dctx->stage = ZSTDds_skipFrame;
        return 0;

    case ZSTDds_skipFrame:
        dctx->expected = 0;
        dctx->stage = ZSTDds_getFrameHeaderSize;
        return 0;

    default:
        assert(0);   /* impossible */
        RETURN_ERROR(GENERIC, "impossible to reach");   /* some compiler require default to do something */
    }
}


static size_t ZSTDN_refDictContent(ZSTDN_DCtx* dctx, const void* dict, size_t dictSize)
{
    dctx->dictEnd = dctx->previousDstEnd;
    dctx->virtualStart = (const char*)dict - ((const char*)(dctx->previousDstEnd) - (const char*)(dctx->prefixStart));
    dctx->prefixStart = dict;
    dctx->previousDstEnd = (const char*)dict + dictSize;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    dctx->dictContentBeginForFuzzing = dctx->prefixStart;
    dctx->dictContentEndForFuzzing = dctx->previousDstEnd;
#endif
    return 0;
}

/*! ZSTDN_loadDEntropy() :
 *  dict : must point at beginning of a valid zstd dictionary.
 * @return : size of entropy tables read */
size_t
ZSTDN_loadDEntropy(ZSTDN_entropyDTables_t* entropy,
                  const void* const dict, size_t const dictSize)
{
    const BYTE* dictPtr = (const BYTE*)dict;
    const BYTE* const dictEnd = dictPtr + dictSize;

    RETURN_ERROR_IF(dictSize <= 8, dictionary_corrupted, "dict is too small");
    assert(MEM_readLE32(dict) == ZSTDN_MAGIC_DICTIONARY);   /* dict must be valid */
    dictPtr += 8;   /* skip header = magic + dictID */

    ZSTDN_STATIC_ASSERT(offsetof(ZSTDN_entropyDTables_t, OFTable) == offsetof(ZSTDN_entropyDTables_t, LLTable) + sizeof(entropy->LLTable));
    ZSTDN_STATIC_ASSERT(offsetof(ZSTDN_entropyDTables_t, MLTable) == offsetof(ZSTDN_entropyDTables_t, OFTable) + sizeof(entropy->OFTable));
    ZSTDN_STATIC_ASSERT(sizeof(entropy->LLTable) + sizeof(entropy->OFTable) + sizeof(entropy->MLTable) >= HUFN_DECOMPRESS_WORKSPACE_SIZE);
    {   void* const workspace = &entropy->LLTable;   /* use fse tables as temporary workspace; implies fse tables are grouped together */
        size_t const workspaceSize = sizeof(entropy->LLTable) + sizeof(entropy->OFTable) + sizeof(entropy->MLTable);
#ifdef HUFN_FORCE_DECOMPRESS_X1
        /* in minimal huffman, we always use X1 variants */
        size_t const hSize = HUFN_readDTableX1_wksp(entropy->hufTable,
                                                dictPtr, dictEnd - dictPtr,
                                                workspace, workspaceSize);
#else
        size_t const hSize = HUFN_readDTableX2_wksp(entropy->hufTable,
                                                dictPtr, (size_t)(dictEnd - dictPtr),
                                                workspace, workspaceSize);
#endif
        RETURN_ERROR_IF(HUFN_isError(hSize), dictionary_corrupted, "");
        dictPtr += hSize;
    }

    {   short offcodeNCount[MaxOff+1];
        unsigned offcodeMaxValue = MaxOff, offcodeLog;
        size_t const offcodeHeaderSize = FSEN_readNCount(offcodeNCount, &offcodeMaxValue, &offcodeLog, dictPtr, (size_t)(dictEnd-dictPtr));
        RETURN_ERROR_IF(FSEN_isError(offcodeHeaderSize), dictionary_corrupted, "");
        RETURN_ERROR_IF(offcodeMaxValue > MaxOff, dictionary_corrupted, "");
        RETURN_ERROR_IF(offcodeLog > OffFSELog, dictionary_corrupted, "");
        ZSTDN_buildFSETable( entropy->OFTable,
                            offcodeNCount, offcodeMaxValue,
                            OF_base, OF_bits,
                            offcodeLog,
                            entropy->workspace, sizeof(entropy->workspace),
                            /* bmi2 */0);
        dictPtr += offcodeHeaderSize;
    }

    {   short matchlengthNCount[MaxML+1];
        unsigned matchlengthMaxValue = MaxML, matchlengthLog;
        size_t const matchlengthHeaderSize = FSEN_readNCount(matchlengthNCount, &matchlengthMaxValue, &matchlengthLog, dictPtr, (size_t)(dictEnd-dictPtr));
        RETURN_ERROR_IF(FSEN_isError(matchlengthHeaderSize), dictionary_corrupted, "");
        RETURN_ERROR_IF(matchlengthMaxValue > MaxML, dictionary_corrupted, "");
        RETURN_ERROR_IF(matchlengthLog > MLFSELog, dictionary_corrupted, "");
        ZSTDN_buildFSETable( entropy->MLTable,
                            matchlengthNCount, matchlengthMaxValue,
                            ML_base, ML_bits,
                            matchlengthLog,
                            entropy->workspace, sizeof(entropy->workspace),
                            /* bmi2 */ 0);
        dictPtr += matchlengthHeaderSize;
    }

    {   short litlengthNCount[MaxLL+1];
        unsigned litlengthMaxValue = MaxLL, litlengthLog;
        size_t const litlengthHeaderSize = FSEN_readNCount(litlengthNCount, &litlengthMaxValue, &litlengthLog, dictPtr, (size_t)(dictEnd-dictPtr));
        RETURN_ERROR_IF(FSEN_isError(litlengthHeaderSize), dictionary_corrupted, "");
        RETURN_ERROR_IF(litlengthMaxValue > MaxLL, dictionary_corrupted, "");
        RETURN_ERROR_IF(litlengthLog > LLFSELog, dictionary_corrupted, "");
        ZSTDN_buildFSETable( entropy->LLTable,
                            litlengthNCount, litlengthMaxValue,
                            LL_base, LL_bits,
                            litlengthLog,
                            entropy->workspace, sizeof(entropy->workspace),
                            /* bmi2 */ 0);
        dictPtr += litlengthHeaderSize;
    }

    RETURN_ERROR_IF(dictPtr+12 > dictEnd, dictionary_corrupted, "");
    {   int i;
        size_t const dictContentSize = (size_t)(dictEnd - (dictPtr+12));
        for (i=0; i<3; i++) {
            U32 const rep = MEM_readLE32(dictPtr); dictPtr += 4;
            RETURN_ERROR_IF(rep==0 || rep > dictContentSize,
                            dictionary_corrupted, "");
            entropy->rep[i] = rep;
    }   }

    return (size_t)(dictPtr - (const BYTE*)dict);
}

static size_t ZSTDN_decompress_insertDictionary(ZSTDN_DCtx* dctx, const void* dict, size_t dictSize)
{
    if (dictSize < 8) return ZSTDN_refDictContent(dctx, dict, dictSize);
    {   U32 const magic = MEM_readLE32(dict);
        if (magic != ZSTDN_MAGIC_DICTIONARY) {
            return ZSTDN_refDictContent(dctx, dict, dictSize);   /* pure content mode */
    }   }
    dctx->dictID = MEM_readLE32((const char*)dict + ZSTDN_FRAMEIDSIZE);

    /* load entropy tables */
    {   size_t const eSize = ZSTDN_loadDEntropy(&dctx->entropy, dict, dictSize);
        RETURN_ERROR_IF(ZSTDN_isError(eSize), dictionary_corrupted, "");
        dict = (const char*)dict + eSize;
        dictSize -= eSize;
    }
    dctx->litEntropy = dctx->fseEntropy = 1;

    /* reference dictionary content */
    return ZSTDN_refDictContent(dctx, dict, dictSize);
}

size_t ZSTDN_decompressBegin(ZSTDN_DCtx* dctx)
{
    assert(dctx != NULL);
    dctx->expected = ZSTDN_startingInputLength(dctx->format);  /* dctx->format must be properly set */
    dctx->stage = ZSTDds_getFrameHeaderSize;
    dctx->processedCSize = 0;
    dctx->decodedSize = 0;
    dctx->previousDstEnd = NULL;
    dctx->prefixStart = NULL;
    dctx->virtualStart = NULL;
    dctx->dictEnd = NULL;
    dctx->entropy.hufTable[0] = (HUFN_DTable)((HufLog)*0x1000001);  /* cover both little and big endian */
    dctx->litEntropy = dctx->fseEntropy = 0;
    dctx->dictID = 0;
    dctx->bType = bt_reserved;
    ZSTDN_STATIC_ASSERT(sizeof(dctx->entropy.rep) == sizeof(repStartValue));
    ZSTDN_memcpy(dctx->entropy.rep, repStartValue, sizeof(repStartValue));  /* initial repcodes */
    dctx->LLTptr = dctx->entropy.LLTable;
    dctx->MLTptr = dctx->entropy.MLTable;
    dctx->OFTptr = dctx->entropy.OFTable;
    dctx->HUFptr = dctx->entropy.hufTable;
    return 0;
}

size_t ZSTDN_decompressBegin_usingDict(ZSTDN_DCtx* dctx, const void* dict, size_t dictSize)
{
    FORWARD_IF_ERROR( ZSTDN_decompressBegin(dctx) , "");
    if (dict && dictSize)
        RETURN_ERROR_IF(
            ZSTDN_isError(ZSTDN_decompress_insertDictionary(dctx, dict, dictSize)),
            dictionary_corrupted, "");
    return 0;
}


/* ======   ZSTDN_DDict   ====== */

size_t ZSTDN_decompressBegin_usingDDict(ZSTDN_DCtx* dctx, const ZSTDN_DDict* ddict)
{
    DEBUGLOG(4, "ZSTDN_decompressBegin_usingDDict");
    assert(dctx != NULL);
    if (ddict) {
        const char* const dictStart = (const char*)ZSTDN_DDict_dictContent(ddict);
        size_t const dictSize = ZSTDN_DDict_dictSize(ddict);
        const void* const dictEnd = dictStart + dictSize;
        dctx->ddictIsCold = (dctx->dictEnd != dictEnd);
        DEBUGLOG(4, "DDict is %s",
                    dctx->ddictIsCold ? "~cold~" : "hot!");
    }
    FORWARD_IF_ERROR( ZSTDN_decompressBegin(dctx) , "");
    if (ddict) {   /* NULL ddict is equivalent to no dictionary */
        ZSTDN_copyDDictParameters(dctx, ddict);
    }
    return 0;
}

/*! ZSTDN_getDictID_fromDict() :
 *  Provides the dictID stored within dictionary.
 *  if @return == 0, the dictionary is not conformant with Zstandard specification.
 *  It can still be loaded, but as a content-only dictionary. */
unsigned ZSTDN_getDictID_fromDict(const void* dict, size_t dictSize)
{
    if (dictSize < 8) return 0;
    if (MEM_readLE32(dict) != ZSTDN_MAGIC_DICTIONARY) return 0;
    return MEM_readLE32((const char*)dict + ZSTDN_FRAMEIDSIZE);
}

/*! ZSTDN_getDictID_fromFrame() :
 *  Provides the dictID required to decompress frame stored within `src`.
 *  If @return == 0, the dictID could not be decoded.
 *  This could for one of the following reasons :
 *  - The frame does not require a dictionary (most common case).
 *  - The frame was built with dictID intentionally removed.
 *    Needed dictionary is a hidden information.
 *    Note : this use case also happens when using a non-conformant dictionary.
 *  - `srcSize` is too small, and as a result, frame header could not be decoded.
 *    Note : possible if `srcSize < ZSTDN_FRAMEHEADERSIZE_MAX`.
 *  - This is not a Zstandard frame.
 *  When identifying the exact failure cause, it's possible to use
 *  ZSTDN_getFrameHeader(), which will provide a more precise error code. */
unsigned ZSTDN_getDictID_fromFrame(const void* src, size_t srcSize)
{
    ZSTDN_frameHeader zfp = { 0, 0, 0, ZSTDN_frame, 0, 0, 0 };
    size_t const hError = ZSTDN_getFrameHeader(&zfp, src, srcSize);
    if (ZSTDN_isError(hError)) return 0;
    return zfp.dictID;
}


/*! ZSTDN_decompress_usingDDict() :
*   Decompression using a pre-digested Dictionary
*   Use dictionary without significant overhead. */
size_t ZSTDN_decompress_usingDDict(ZSTDN_DCtx* dctx,
                                  void* dst, size_t dstCapacity,
                            const void* src, size_t srcSize,
                            const ZSTDN_DDict* ddict)
{
    /* pass content and size in case legacy frames are encountered */
    return ZSTDN_decompressMultiFrame(dctx, dst, dstCapacity, src, srcSize,
                                     NULL, 0,
                                     ddict);
}


/*=====================================
*   Streaming decompression
*====================================*/

ZSTDN_DStream* ZSTDN_createDStream(void)
{
    DEBUGLOG(3, "ZSTDN_createDStream");
    return ZSTDN_createDStream_advanced(ZSTDN_defaultCMem);
}

ZSTDN_DStream* ZSTDN_initStaticDStream(void *workspace, size_t workspaceSize)
{
    return ZSTDN_initStaticDCtx(workspace, workspaceSize);
}

ZSTDN_DStream* ZSTDN_createDStream_advanced(ZSTDN_customMem customMem)
{
    return ZSTDN_createDCtx_advanced(customMem);
}

size_t ZSTDN_freeDStream(ZSTDN_DStream* zds)
{
    return ZSTDN_freeDCtx(zds);
}


/* ***  Initialization  *** */

size_t ZSTDN_DStreamInSize(void)  { return ZSTDN_BLOCKSIZE_MAX + ZSTDN_blockHeaderSize; }
size_t ZSTDN_DStreamOutSize(void) { return ZSTDN_BLOCKSIZE_MAX; }

size_t ZSTDN_DCtx_loadDictionary_advanced(ZSTDN_DCtx* dctx,
                                   const void* dict, size_t dictSize,
                                         ZSTDN_dictLoadMethod_e dictLoadMethod,
                                         ZSTDN_dictContentType_e dictContentType)
{
    RETURN_ERROR_IF(dctx->streamStage != zdss_init, stage_wrong, "");
    ZSTDN_clearDict(dctx);
    if (dict && dictSize != 0) {
        dctx->ddictLocal = ZSTDN_createDDict_advanced(dict, dictSize, dictLoadMethod, dictContentType, dctx->customMem);
        RETURN_ERROR_IF(dctx->ddictLocal == NULL, memory_allocation, "NULL pointer!");
        dctx->ddict = dctx->ddictLocal;
        dctx->dictUses = ZSTDN_use_indefinitely;
    }
    return 0;
}

size_t ZSTDN_DCtx_loadDictionary_byReference(ZSTDN_DCtx* dctx, const void* dict, size_t dictSize)
{
    return ZSTDN_DCtx_loadDictionary_advanced(dctx, dict, dictSize, ZSTDN_dlm_byRef, ZSTDN_dct_auto);
}

size_t ZSTDN_DCtx_loadDictionary(ZSTDN_DCtx* dctx, const void* dict, size_t dictSize)
{
    return ZSTDN_DCtx_loadDictionary_advanced(dctx, dict, dictSize, ZSTDN_dlm_byCopy, ZSTDN_dct_auto);
}

size_t ZSTDN_DCtx_refPrefix_advanced(ZSTDN_DCtx* dctx, const void* prefix, size_t prefixSize, ZSTDN_dictContentType_e dictContentType)
{
    FORWARD_IF_ERROR(ZSTDN_DCtx_loadDictionary_advanced(dctx, prefix, prefixSize, ZSTDN_dlm_byRef, dictContentType), "");
    dctx->dictUses = ZSTDN_use_once;
    return 0;
}

size_t ZSTDN_DCtx_refPrefix(ZSTDN_DCtx* dctx, const void* prefix, size_t prefixSize)
{
    return ZSTDN_DCtx_refPrefix_advanced(dctx, prefix, prefixSize, ZSTDN_dct_rawContent);
}


/* ZSTDN_initDStream_usingDict() :
 * return : expected size, aka ZSTDN_startingInputLength().
 * this function cannot fail */
size_t ZSTDN_initDStream_usingDict(ZSTDN_DStream* zds, const void* dict, size_t dictSize)
{
    DEBUGLOG(4, "ZSTDN_initDStream_usingDict");
    FORWARD_IF_ERROR( ZSTDN_DCtx_reset(zds, ZSTDN_reset_session_only) , "");
    FORWARD_IF_ERROR( ZSTDN_DCtx_loadDictionary(zds, dict, dictSize) , "");
    return ZSTDN_startingInputLength(zds->format);
}

/* note : this variant can't fail */
size_t ZSTDN_initDStream(ZSTDN_DStream* zds)
{
    DEBUGLOG(4, "ZSTDN_initDStream");
    return ZSTDN_initDStream_usingDDict(zds, NULL);
}

/* ZSTDN_initDStream_usingDDict() :
 * ddict will just be referenced, and must outlive decompression session
 * this function cannot fail */
size_t ZSTDN_initDStream_usingDDict(ZSTDN_DStream* dctx, const ZSTDN_DDict* ddict)
{
    FORWARD_IF_ERROR( ZSTDN_DCtx_reset(dctx, ZSTDN_reset_session_only) , "");
    FORWARD_IF_ERROR( ZSTDN_DCtx_refDDict(dctx, ddict) , "");
    return ZSTDN_startingInputLength(dctx->format);
}

/* ZSTDN_resetDStream() :
 * return : expected size, aka ZSTDN_startingInputLength().
 * this function cannot fail */
size_t ZSTDN_resetDStream(ZSTDN_DStream* dctx)
{
    FORWARD_IF_ERROR(ZSTDN_DCtx_reset(dctx, ZSTDN_reset_session_only), "");
    return ZSTDN_startingInputLength(dctx->format);
}


size_t ZSTDN_DCtx_refDDict(ZSTDN_DCtx* dctx, const ZSTDN_DDict* ddict)
{
    RETURN_ERROR_IF(dctx->streamStage != zdss_init, stage_wrong, "");
    ZSTDN_clearDict(dctx);
    if (ddict) {
        dctx->ddict = ddict;
        dctx->dictUses = ZSTDN_use_indefinitely;
        if (dctx->refMultipleDDicts == ZSTDN_rmd_refMultipleDDicts) {
            if (dctx->ddictSet == NULL) {
                dctx->ddictSet = ZSTDN_createDDictHashSet(dctx->customMem);
                if (!dctx->ddictSet) {
                    RETURN_ERROR(memory_allocation, "Failed to allocate memory for hash set!");
                }
            }
            assert(!dctx->staticSize);  /* Impossible: ddictSet cannot have been allocated if static dctx */
            FORWARD_IF_ERROR(ZSTDN_DDictHashSet_addDDict(dctx->ddictSet, ddict, dctx->customMem), "");
        }
    }
    return 0;
}

/* ZSTDN_DCtx_setMaxWindowSize() :
 * note : no direct equivalence in ZSTDN_DCtx_setParameter,
 * since this version sets windowSize, and the other sets windowLog */
size_t ZSTDN_DCtx_setMaxWindowSize(ZSTDN_DCtx* dctx, size_t maxWindowSize)
{
    ZSTDN_bounds const bounds = ZSTDN_dParam_getBounds(ZSTDN_d_windowLogMax);
    size_t const min = (size_t)1 << bounds.lowerBound;
    size_t const max = (size_t)1 << bounds.upperBound;
    RETURN_ERROR_IF(dctx->streamStage != zdss_init, stage_wrong, "");
    RETURN_ERROR_IF(maxWindowSize < min, parameter_outOfBound, "");
    RETURN_ERROR_IF(maxWindowSize > max, parameter_outOfBound, "");
    dctx->maxWindowSize = maxWindowSize;
    return 0;
}

size_t ZSTDN_DCtx_setFormat(ZSTDN_DCtx* dctx, ZSTDN_format_e format)
{
    return ZSTDN_DCtx_setParameter(dctx, ZSTDN_d_format, (int)format);
}

ZSTDN_bounds ZSTDN_dParam_getBounds(ZSTDN_dParameter dParam)
{
    ZSTDN_bounds bounds = { 0, 0, 0 };
    switch(dParam) {
        case ZSTDN_d_windowLogMax:
            bounds.lowerBound = ZSTDN_WINDOWLOG_ABSOLUTEMIN;
            bounds.upperBound = ZSTDN_WINDOWLOG_MAX;
            return bounds;
        case ZSTDN_d_format:
            bounds.lowerBound = (int)ZSTDN_f_zstd1;
            bounds.upperBound = (int)ZSTDN_f_zstd1_magicless;
            ZSTDN_STATIC_ASSERT(ZSTDN_f_zstd1 < ZSTDN_f_zstd1_magicless);
            return bounds;
        case ZSTDN_d_stableOutBuffer:
            bounds.lowerBound = (int)ZSTDN_bm_buffered;
            bounds.upperBound = (int)ZSTDN_bm_stable;
            return bounds;
        case ZSTDN_d_forceIgnoreChecksum:
            bounds.lowerBound = (int)ZSTDN_d_validateChecksum;
            bounds.upperBound = (int)ZSTDN_d_ignoreChecksum;
            return bounds;
        case ZSTDN_d_refMultipleDDicts:
            bounds.lowerBound = (int)ZSTDN_rmd_refSingleDDict;
            bounds.upperBound = (int)ZSTDN_rmd_refMultipleDDicts;
            return bounds;
        default:;
    }
    bounds.error = ERROR(parameter_unsupported);
    return bounds;
}

/* ZSTDN_dParam_withinBounds:
 * @return 1 if value is within dParam bounds,
 * 0 otherwise */
static int ZSTDN_dParam_withinBounds(ZSTDN_dParameter dParam, int value)
{
    ZSTDN_bounds const bounds = ZSTDN_dParam_getBounds(dParam);
    if (ZSTDN_isError(bounds.error)) return 0;
    if (value < bounds.lowerBound) return 0;
    if (value > bounds.upperBound) return 0;
    return 1;
}

#define CHECK_DBOUNDS(p,v) {                \
    RETURN_ERROR_IF(!ZSTDN_dParam_withinBounds(p, v), parameter_outOfBound, ""); \
}

size_t ZSTDN_DCtx_getParameter(ZSTDN_DCtx* dctx, ZSTDN_dParameter param, int* value)
{
    switch (param) {
        case ZSTDN_d_windowLogMax:
            *value = (int)ZSTDN_highbit32((U32)dctx->maxWindowSize);
            return 0;
        case ZSTDN_d_format:
            *value = (int)dctx->format;
            return 0;
        case ZSTDN_d_stableOutBuffer:
            *value = (int)dctx->outBufferMode;
            return 0;
        case ZSTDN_d_forceIgnoreChecksum:
            *value = (int)dctx->forceIgnoreChecksum;
            return 0;
        case ZSTDN_d_refMultipleDDicts:
            *value = (int)dctx->refMultipleDDicts;
            return 0;
        default:;
    }
    RETURN_ERROR(parameter_unsupported, "");
}

size_t ZSTDN_DCtx_setParameter(ZSTDN_DCtx* dctx, ZSTDN_dParameter dParam, int value)
{
    RETURN_ERROR_IF(dctx->streamStage != zdss_init, stage_wrong, "");
    switch(dParam) {
        case ZSTDN_d_windowLogMax:
            if (value == 0) value = ZSTDN_WINDOWLOG_LIMIT_DEFAULT;
            CHECK_DBOUNDS(ZSTDN_d_windowLogMax, value);
            dctx->maxWindowSize = ((size_t)1) << value;
            return 0;
        case ZSTDN_d_format:
            CHECK_DBOUNDS(ZSTDN_d_format, value);
            dctx->format = (ZSTDN_format_e)value;
            return 0;
        case ZSTDN_d_stableOutBuffer:
            CHECK_DBOUNDS(ZSTDN_d_stableOutBuffer, value);
            dctx->outBufferMode = (ZSTDN_bufferMode_e)value;
            return 0;
        case ZSTDN_d_forceIgnoreChecksum:
            CHECK_DBOUNDS(ZSTDN_d_forceIgnoreChecksum, value);
            dctx->forceIgnoreChecksum = (ZSTDN_forceIgnoreChecksum_e)value;
            return 0;
        case ZSTDN_d_refMultipleDDicts:
            CHECK_DBOUNDS(ZSTDN_d_refMultipleDDicts, value);
            if (dctx->staticSize != 0) {
                RETURN_ERROR(parameter_unsupported, "Static dctx does not support multiple DDicts!");
            }
            dctx->refMultipleDDicts = (ZSTDN_refMultipleDDicts_e)value;
            return 0;
        default:;
    }
    RETURN_ERROR(parameter_unsupported, "");
}

size_t ZSTDN_DCtx_reset(ZSTDN_DCtx* dctx, ZSTDN_ResetDirective reset)
{
    if ( (reset == ZSTDN_reset_session_only)
      || (reset == ZSTDN_reset_session_and_parameters) ) {
        dctx->streamStage = zdss_init;
        dctx->noForwardProgress = 0;
    }
    if ( (reset == ZSTDN_reset_parameters)
      || (reset == ZSTDN_reset_session_and_parameters) ) {
        RETURN_ERROR_IF(dctx->streamStage != zdss_init, stage_wrong, "");
        ZSTDN_clearDict(dctx);
        ZSTDN_DCtx_resetParameters(dctx);
    }
    return 0;
}


size_t ZSTDN_sizeof_DStream(const ZSTDN_DStream* dctx)
{
    return ZSTDN_sizeof_DCtx(dctx);
}

size_t ZSTDN_decodingBufferSize_min(unsigned long long windowSize, unsigned long long frameContentSize)
{
    size_t const blockSize = (size_t) MIN(windowSize, ZSTDN_BLOCKSIZE_MAX);
    unsigned long long const neededRBSize = windowSize + blockSize + (WILDCOPY_OVERLENGTH * 2);
    unsigned long long const neededSize = MIN(frameContentSize, neededRBSize);
    size_t const minRBSize = (size_t) neededSize;
    RETURN_ERROR_IF((unsigned long long)minRBSize != neededSize,
                    frameParameter_windowTooLarge, "");
    return minRBSize;
}

size_t ZSTDN_estimateDStreamSize(size_t windowSize)
{
    size_t const blockSize = MIN(windowSize, ZSTDN_BLOCKSIZE_MAX);
    size_t const inBuffSize = blockSize;  /* no block can be larger */
    size_t const outBuffSize = ZSTDN_decodingBufferSize_min(windowSize, ZSTDN_CONTENTSIZE_UNKNOWN);
    return ZSTDN_estimateDCtxSize() + inBuffSize + outBuffSize;
}

size_t ZSTDN_estimateDStreamSize_fromFrame(const void* src, size_t srcSize)
{
    U32 const windowSizeMax = 1U << ZSTDN_WINDOWLOG_MAX;   /* note : should be user-selectable, but requires an additional parameter (or a dctx) */
    ZSTDN_frameHeader zfh;
    size_t const err = ZSTDN_getFrameHeader(&zfh, src, srcSize);
    if (ZSTDN_isError(err)) return err;
    RETURN_ERROR_IF(err>0, srcSize_wrong, "");
    RETURN_ERROR_IF(zfh.windowSize > windowSizeMax,
                    frameParameter_windowTooLarge, "");
    return ZSTDN_estimateDStreamSize((size_t)zfh.windowSize);
}


/* *****   Decompression   ***** */

static int ZSTDN_DCtx_isOverflow(ZSTDN_DStream* zds, size_t const neededInBuffSize, size_t const neededOutBuffSize)
{
    return (zds->inBuffSize + zds->outBuffSize) >= (neededInBuffSize + neededOutBuffSize) * ZSTDN_WORKSPACETOOLARGE_FACTOR;
}

static void ZSTDN_DCtx_updateOversizedDuration(ZSTDN_DStream* zds, size_t const neededInBuffSize, size_t const neededOutBuffSize)
{
    if (ZSTDN_DCtx_isOverflow(zds, neededInBuffSize, neededOutBuffSize))
        zds->oversizedDuration++;
    else
        zds->oversizedDuration = 0;
}

static int ZSTDN_DCtx_isOversizedTooLong(ZSTDN_DStream* zds)
{
    return zds->oversizedDuration >= ZSTDN_WORKSPACETOOLARGE_MAXDURATION;
}

/* Checks that the output buffer hasn't changed if ZSTDN_obm_stable is used. */
static size_t ZSTDN_checkOutBuffer(ZSTDN_DStream const* zds, ZSTDN_outBuffer const* output)
{
    ZSTDN_outBuffer const expect = zds->expectedOutBuffer;
    /* No requirement when ZSTDN_obm_stable is not enabled. */
    if (zds->outBufferMode != ZSTDN_bm_stable)
        return 0;
    /* Any buffer is allowed in zdss_init, this must be the same for every other call until
     * the context is reset.
     */
    if (zds->streamStage == zdss_init)
        return 0;
    /* The buffer must match our expectation exactly. */
    if (expect.dst == output->dst && expect.pos == output->pos && expect.size == output->size)
        return 0;
    RETURN_ERROR(dstBuffer_wrong, "ZSTDN_d_stableOutBuffer enabled but output differs!");
}

/* Calls ZSTDN_decompressContinue() with the right parameters for ZSTDN_decompressStream()
 * and updates the stage and the output buffer state. This call is extracted so it can be
 * used both when reading directly from the ZSTDN_inBuffer, and in buffered input mode.
 * NOTE: You must break after calling this function since the streamStage is modified.
 */
static size_t ZSTDN_decompressContinueStream(
            ZSTDN_DStream* zds, char** op, char* oend,
            void const* src, size_t srcSize) {
    int const isSkipFrame = ZSTDN_isSkipFrame(zds);
    if (zds->outBufferMode == ZSTDN_bm_buffered) {
        size_t const dstSize = isSkipFrame ? 0 : zds->outBuffSize - zds->outStart;
        size_t const decodedSize = ZSTDN_decompressContinue(zds,
                zds->outBuff + zds->outStart, dstSize, src, srcSize);
        FORWARD_IF_ERROR(decodedSize, "");
        if (!decodedSize && !isSkipFrame) {
            zds->streamStage = zdss_read;
        } else {
            zds->outEnd = zds->outStart + decodedSize;
            zds->streamStage = zdss_flush;
        }
    } else {
        /* Write directly into the output buffer */
        size_t const dstSize = isSkipFrame ? 0 : (size_t)(oend - *op);
        size_t const decodedSize = ZSTDN_decompressContinue(zds, *op, dstSize, src, srcSize);
        FORWARD_IF_ERROR(decodedSize, "");
        *op += decodedSize;
        /* Flushing is not needed. */
        zds->streamStage = zdss_read;
        assert(*op <= oend);
        assert(zds->outBufferMode == ZSTDN_bm_stable);
    }
    return 0;
}

size_t ZSTDN_decompressStream(ZSTDN_DStream* zds, ZSTDN_outBuffer* output, ZSTDN_inBuffer* input)
{
    const char* const src = (const char*)input->src;
    const char* const istart = input->pos != 0 ? src + input->pos : src;
    const char* const iend = input->size != 0 ? src + input->size : src;
    const char* ip = istart;
    char* const dst = (char*)output->dst;
    char* const ostart = output->pos != 0 ? dst + output->pos : dst;
    char* const oend = output->size != 0 ? dst + output->size : dst;
    char* op = ostart;
    U32 someMoreWork = 1;

    DEBUGLOG(5, "ZSTDN_decompressStream");
    RETURN_ERROR_IF(
        input->pos > input->size,
        srcSize_wrong,
        "forbidden. in: pos: %u   vs size: %u",
        (U32)input->pos, (U32)input->size);
    RETURN_ERROR_IF(
        output->pos > output->size,
        dstSize_tooSmall,
        "forbidden. out: pos: %u   vs size: %u",
        (U32)output->pos, (U32)output->size);
    DEBUGLOG(5, "input size : %u", (U32)(input->size - input->pos));
    FORWARD_IF_ERROR(ZSTDN_checkOutBuffer(zds, output), "");

    while (someMoreWork) {
        switch(zds->streamStage)
        {
        case zdss_init :
            DEBUGLOG(5, "stage zdss_init => transparent reset ");
            zds->streamStage = zdss_loadHeader;
            zds->lhSize = zds->inPos = zds->outStart = zds->outEnd = 0;
            zds->legacyVersion = 0;
            zds->hostageByte = 0;
            zds->expectedOutBuffer = *output;
            ZSTDN_FALLTHROUGH;

        case zdss_loadHeader :
            DEBUGLOG(5, "stage zdss_loadHeader (srcSize : %u)", (U32)(iend - ip));
            {   size_t const hSize = ZSTDN_getFrameHeader_advanced(&zds->fParams, zds->headerBuffer, zds->lhSize, zds->format);
                if (zds->refMultipleDDicts && zds->ddictSet) {
                    ZSTDN_DCtx_selectFrameDDict(zds);
                }
                DEBUGLOG(5, "header size : %u", (U32)hSize);
                if (ZSTDN_isError(hSize)) {
                    return hSize;   /* error */
                }
                if (hSize != 0) {   /* need more input */
                    size_t const toLoad = hSize - zds->lhSize;   /* if hSize!=0, hSize > zds->lhSize */
                    size_t const remainingInput = (size_t)(iend-ip);
                    assert(iend >= ip);
                    if (toLoad > remainingInput) {   /* not enough input to load full header */
                        if (remainingInput > 0) {
                            ZSTDN_memcpy(zds->headerBuffer + zds->lhSize, ip, remainingInput);
                            zds->lhSize += remainingInput;
                        }
                        input->pos = input->size;
                        return (MAX((size_t)ZSTDN_FRAMEHEADERSIZE_MIN(zds->format), hSize) - zds->lhSize) + ZSTDN_blockHeaderSize;   /* remaining header bytes + next block header */
                    }
                    assert(ip != NULL);
                    ZSTDN_memcpy(zds->headerBuffer + zds->lhSize, ip, toLoad); zds->lhSize = hSize; ip += toLoad;
                    break;
            }   }

            /* check for single-pass mode opportunity */
            if (zds->fParams.frameContentSize != ZSTDN_CONTENTSIZE_UNKNOWN
                && zds->fParams.frameType != ZSTDN_skippableFrame
                && (U64)(size_t)(oend-op) >= zds->fParams.frameContentSize) {
                size_t const cSize = ZSTDN_findFrameCompressedSize(istart, (size_t)(iend-istart));
                if (cSize <= (size_t)(iend-istart)) {
                    /* shortcut : using single-pass mode */
                    size_t const decompressedSize = ZSTDN_decompress_usingDDict(zds, op, (size_t)(oend-op), istart, cSize, ZSTDN_getDDict(zds));
                    if (ZSTDN_isError(decompressedSize)) return decompressedSize;
                    DEBUGLOG(4, "shortcut to single-pass ZSTDN_decompress_usingDDict()")
                    ip = istart + cSize;
                    op += decompressedSize;
                    zds->expected = 0;
                    zds->streamStage = zdss_init;
                    someMoreWork = 0;
                    break;
            }   }

            /* Check output buffer is large enough for ZSTDN_odm_stable. */
            if (zds->outBufferMode == ZSTDN_bm_stable
                && zds->fParams.frameType != ZSTDN_skippableFrame
                && zds->fParams.frameContentSize != ZSTDN_CONTENTSIZE_UNKNOWN
                && (U64)(size_t)(oend-op) < zds->fParams.frameContentSize) {
                RETURN_ERROR(dstSize_tooSmall, "ZSTDN_obm_stable passed but ZSTDN_outBuffer is too small");
            }

            /* Consume header (see ZSTDds_decodeFrameHeader) */
            DEBUGLOG(4, "Consume header");
            FORWARD_IF_ERROR(ZSTDN_decompressBegin_usingDDict(zds, ZSTDN_getDDict(zds)), "");

            if ((MEM_readLE32(zds->headerBuffer) & ZSTDN_MAGIC_SKIPPABLE_MASK) == ZSTDN_MAGIC_SKIPPABLE_START) {  /* skippable frame */
                zds->expected = MEM_readLE32(zds->headerBuffer + ZSTDN_FRAMEIDSIZE);
                zds->stage = ZSTDds_skipFrame;
            } else {
                FORWARD_IF_ERROR(ZSTDN_decodeFrameHeader(zds, zds->headerBuffer, zds->lhSize), "");
                zds->expected = ZSTDN_blockHeaderSize;
                zds->stage = ZSTDds_decodeBlockHeader;
            }

            /* control buffer memory usage */
            DEBUGLOG(4, "Control max memory usage (%u KB <= max %u KB)",
                        (U32)(zds->fParams.windowSize >>10),
                        (U32)(zds->maxWindowSize >> 10) );
            zds->fParams.windowSize = MAX(zds->fParams.windowSize, 1U << ZSTDN_WINDOWLOG_ABSOLUTEMIN);
            RETURN_ERROR_IF(zds->fParams.windowSize > zds->maxWindowSize,
                            frameParameter_windowTooLarge, "");

            /* Adapt buffer sizes to frame header instructions */
            {   size_t const neededInBuffSize = MAX(zds->fParams.blockSizeMax, 4 /* frame checksum */);
                size_t const neededOutBuffSize = zds->outBufferMode == ZSTDN_bm_buffered
                        ? ZSTDN_decodingBufferSize_min(zds->fParams.windowSize, zds->fParams.frameContentSize)
                        : 0;

                ZSTDN_DCtx_updateOversizedDuration(zds, neededInBuffSize, neededOutBuffSize);

                {   int const tooSmall = (zds->inBuffSize < neededInBuffSize) || (zds->outBuffSize < neededOutBuffSize);
                    int const tooLarge = ZSTDN_DCtx_isOversizedTooLong(zds);

                    if (tooSmall || tooLarge) {
                        size_t const bufferSize = neededInBuffSize + neededOutBuffSize;
                        DEBUGLOG(4, "inBuff  : from %u to %u",
                                    (U32)zds->inBuffSize, (U32)neededInBuffSize);
                        DEBUGLOG(4, "outBuff : from %u to %u",
                                    (U32)zds->outBuffSize, (U32)neededOutBuffSize);
                        if (zds->staticSize) {  /* static DCtx */
                            DEBUGLOG(4, "staticSize : %u", (U32)zds->staticSize);
                            assert(zds->staticSize >= sizeof(ZSTDN_DCtx));  /* controlled at init */
                            RETURN_ERROR_IF(
                                bufferSize > zds->staticSize - sizeof(ZSTDN_DCtx),
                                memory_allocation, "");
                        } else {
                            ZSTDN_customFree(zds->inBuff, zds->customMem);
                            zds->inBuffSize = 0;
                            zds->outBuffSize = 0;
                            zds->inBuff = (char*)ZSTDN_customMalloc(bufferSize, zds->customMem);
                            RETURN_ERROR_IF(zds->inBuff == NULL, memory_allocation, "");
                        }
                        zds->inBuffSize = neededInBuffSize;
                        zds->outBuff = zds->inBuff + zds->inBuffSize;
                        zds->outBuffSize = neededOutBuffSize;
            }   }   }
            zds->streamStage = zdss_read;
            ZSTDN_FALLTHROUGH;

        case zdss_read:
            DEBUGLOG(5, "stage zdss_read");
            {   size_t const neededInSize = ZSTDN_nextSrcSizeToDecompressWithInputSize(zds, (size_t)(iend - ip));
                DEBUGLOG(5, "neededInSize = %u", (U32)neededInSize);
                if (neededInSize==0) {  /* end of frame */
                    zds->streamStage = zdss_init;
                    someMoreWork = 0;
                    break;
                }
                if ((size_t)(iend-ip) >= neededInSize) {  /* decode directly from src */
                    FORWARD_IF_ERROR(ZSTDN_decompressContinueStream(zds, &op, oend, ip, neededInSize), "");
                    ip += neededInSize;
                    /* Function modifies the stage so we must break */
                    break;
            }   }
            if (ip==iend) { someMoreWork = 0; break; }   /* no more input */
            zds->streamStage = zdss_load;
            ZSTDN_FALLTHROUGH;

        case zdss_load:
            {   size_t const neededInSize = ZSTDN_nextSrcSizeToDecompress(zds);
                size_t const toLoad = neededInSize - zds->inPos;
                int const isSkipFrame = ZSTDN_isSkipFrame(zds);
                size_t loadedSize;
                /* At this point we shouldn't be decompressing a block that we can stream. */
                assert(neededInSize == ZSTDN_nextSrcSizeToDecompressWithInputSize(zds, iend - ip));
                if (isSkipFrame) {
                    loadedSize = MIN(toLoad, (size_t)(iend-ip));
                } else {
                    RETURN_ERROR_IF(toLoad > zds->inBuffSize - zds->inPos,
                                    corruption_detected,
                                    "should never happen");
                    loadedSize = ZSTDN_limitCopy(zds->inBuff + zds->inPos, toLoad, ip, (size_t)(iend-ip));
                }
                ip += loadedSize;
                zds->inPos += loadedSize;
                if (loadedSize < toLoad) { someMoreWork = 0; break; }   /* not enough input, wait for more */

                /* decode loaded input */
                zds->inPos = 0;   /* input is consumed */
                FORWARD_IF_ERROR(ZSTDN_decompressContinueStream(zds, &op, oend, zds->inBuff, neededInSize), "");
                /* Function modifies the stage so we must break */
                break;
            }
        case zdss_flush:
            {   size_t const toFlushSize = zds->outEnd - zds->outStart;
                size_t const flushedSize = ZSTDN_limitCopy(op, (size_t)(oend-op), zds->outBuff + zds->outStart, toFlushSize);
                op += flushedSize;
                zds->outStart += flushedSize;
                if (flushedSize == toFlushSize) {  /* flush completed */
                    zds->streamStage = zdss_read;
                    if ( (zds->outBuffSize < zds->fParams.frameContentSize)
                      && (zds->outStart + zds->fParams.blockSizeMax > zds->outBuffSize) ) {
                        DEBUGLOG(5, "restart filling outBuff from beginning (left:%i, needed:%u)",
                                (int)(zds->outBuffSize - zds->outStart),
                                (U32)zds->fParams.blockSizeMax);
                        zds->outStart = zds->outEnd = 0;
                    }
                    break;
            }   }
            /* cannot complete flush */
            someMoreWork = 0;
            break;

        default:
            assert(0);    /* impossible */
            RETURN_ERROR(GENERIC, "impossible to reach");   /* some compiler require default to do something */
    }   }

    /* result */
    input->pos = (size_t)(ip - (const char*)(input->src));
    output->pos = (size_t)(op - (char*)(output->dst));

    /* Update the expected output buffer for ZSTDN_obm_stable. */
    zds->expectedOutBuffer = *output;

    if ((ip==istart) && (op==ostart)) {  /* no forward progress */
        zds->noForwardProgress ++;
        if (zds->noForwardProgress >= ZSTDN_NO_FORWARD_PROGRESS_MAX) {
            RETURN_ERROR_IF(op==oend, dstSize_tooSmall, "");
            RETURN_ERROR_IF(ip==iend, srcSize_wrong, "");
            assert(0);
        }
    } else {
        zds->noForwardProgress = 0;
    }
    {   size_t nextSrcSizeHint = ZSTDN_nextSrcSizeToDecompress(zds);
        if (!nextSrcSizeHint) {   /* frame fully decoded */
            if (zds->outEnd == zds->outStart) {  /* output fully flushed */
                if (zds->hostageByte) {
                    if (input->pos >= input->size) {
                        /* can't release hostage (not present) */
                        zds->streamStage = zdss_read;
                        return 1;
                    }
                    input->pos++;  /* release hostage */
                }   /* zds->hostageByte */
                return 0;
            }  /* zds->outEnd == zds->outStart */
            if (!zds->hostageByte) { /* output not fully flushed; keep last byte as hostage; will be released when all output is flushed */
                input->pos--;   /* note : pos > 0, otherwise, impossible to finish reading last block */
                zds->hostageByte=1;
            }
            return 1;
        }  /* nextSrcSizeHint==0 */
        nextSrcSizeHint += ZSTDN_blockHeaderSize * (ZSTDN_nextInputType(zds) == ZSTDnit_block);   /* preload header of next block */
        assert(zds->inPos <= nextSrcSizeHint);
        nextSrcSizeHint -= zds->inPos;   /* part already loaded*/
        return nextSrcSizeHint;
    }
}

size_t ZSTDN_decompressStream_simpleArgs (
                            ZSTDN_DCtx* dctx,
                            void* dst, size_t dstCapacity, size_t* dstPos,
                      const void* src, size_t srcSize, size_t* srcPos)
{
    ZSTDN_outBuffer output = { dst, dstCapacity, *dstPos };
    ZSTDN_inBuffer  input  = { src, srcSize, *srcPos };
    /* ZSTDN_compress_generic() will check validity of dstPos and srcPos */
    size_t const cErr = ZSTDN_decompressStream(dctx, &output, &input);
    *dstPos = output.pos;
    *srcPos = input.pos;
    return cErr;
}
