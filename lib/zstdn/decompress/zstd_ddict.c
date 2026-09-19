/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/* zstd_ddict.c :
 * concentrates all logic that needs to know the internals of ZSTDN_DDict object */

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
#include "zstd_decompress_internal.h"
#include "zstd_ddict.h"




/*-*******************************************************
*  Types
*********************************************************/
struct ZSTDN_DDict_s {
    void* dictBuffer;
    const void* dictContent;
    size_t dictSize;
    ZSTDN_entropyDTables_t entropy;
    U32 dictID;
    U32 entropyPresent;
    ZSTDN_customMem cMem;
};  /* typedef'd to ZSTDN_DDict within "zstd.h" */

const void* ZSTDN_DDict_dictContent(const ZSTDN_DDict* ddict)
{
    assert(ddict != NULL);
    return ddict->dictContent;
}

size_t ZSTDN_DDict_dictSize(const ZSTDN_DDict* ddict)
{
    assert(ddict != NULL);
    return ddict->dictSize;
}

void ZSTDN_copyDDictParameters(ZSTDN_DCtx* dctx, const ZSTDN_DDict* ddict)
{
    DEBUGLOG(4, "ZSTDN_copyDDictParameters");
    assert(dctx != NULL);
    assert(ddict != NULL);
    dctx->dictID = ddict->dictID;
    dctx->prefixStart = ddict->dictContent;
    dctx->virtualStart = ddict->dictContent;
    dctx->dictEnd = (const BYTE*)ddict->dictContent + ddict->dictSize;
    dctx->previousDstEnd = dctx->dictEnd;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    dctx->dictContentBeginForFuzzing = dctx->prefixStart;
    dctx->dictContentEndForFuzzing = dctx->previousDstEnd;
#endif
    if (ddict->entropyPresent) {
        dctx->litEntropy = 1;
        dctx->fseEntropy = 1;
        dctx->LLTptr = ddict->entropy.LLTable;
        dctx->MLTptr = ddict->entropy.MLTable;
        dctx->OFTptr = ddict->entropy.OFTable;
        dctx->HUFptr = ddict->entropy.hufTable;
        dctx->entropy.rep[0] = ddict->entropy.rep[0];
        dctx->entropy.rep[1] = ddict->entropy.rep[1];
        dctx->entropy.rep[2] = ddict->entropy.rep[2];
    } else {
        dctx->litEntropy = 0;
        dctx->fseEntropy = 0;
    }
}


static size_t
ZSTDN_loadEntropy_intoDDict(ZSTDN_DDict* ddict,
                           ZSTDN_dictContentType_e dictContentType)
{
    ddict->dictID = 0;
    ddict->entropyPresent = 0;
    if (dictContentType == ZSTDN_dct_rawContent) return 0;

    if (ddict->dictSize < 8) {
        if (dictContentType == ZSTDN_dct_fullDict)
            return ERROR(dictionary_corrupted);   /* only accept specified dictionaries */
        return 0;   /* pure content mode */
    }
    {   U32 const magic = MEM_readLE32(ddict->dictContent);
        if (magic != ZSTDN_MAGIC_DICTIONARY) {
            if (dictContentType == ZSTDN_dct_fullDict)
                return ERROR(dictionary_corrupted);   /* only accept specified dictionaries */
            return 0;   /* pure content mode */
        }
    }
    ddict->dictID = MEM_readLE32((const char*)ddict->dictContent + ZSTDN_FRAMEIDSIZE);

    /* load entropy tables */
    RETURN_ERROR_IF(ZSTDN_isError(ZSTDN_loadDEntropy(
            &ddict->entropy, ddict->dictContent, ddict->dictSize)),
        dictionary_corrupted, "");
    ddict->entropyPresent = 1;
    return 0;
}


static size_t ZSTDN_initDDict_internal(ZSTDN_DDict* ddict,
                                      const void* dict, size_t dictSize,
                                      ZSTDN_dictLoadMethod_e dictLoadMethod,
                                      ZSTDN_dictContentType_e dictContentType)
{
    if ((dictLoadMethod == ZSTDN_dlm_byRef) || (!dict) || (!dictSize)) {
        ddict->dictBuffer = NULL;
        ddict->dictContent = dict;
        if (!dict) dictSize = 0;
    } else {
        void* const internalBuffer = ZSTDN_customMalloc(dictSize, ddict->cMem);
        ddict->dictBuffer = internalBuffer;
        ddict->dictContent = internalBuffer;
        if (!internalBuffer) return ERROR(memory_allocation);
        ZSTDN_memcpy(internalBuffer, dict, dictSize);
    }
    ddict->dictSize = dictSize;
    ddict->entropy.hufTable[0] = (HUFN_DTable)((HufLog)*0x1000001);  /* cover both little and big endian */

    /* parse dictionary content */
    FORWARD_IF_ERROR( ZSTDN_loadEntropy_intoDDict(ddict, dictContentType) , "");

    return 0;
}

ZSTDN_DDict* ZSTDN_createDDict_advanced(const void* dict, size_t dictSize,
                                      ZSTDN_dictLoadMethod_e dictLoadMethod,
                                      ZSTDN_dictContentType_e dictContentType,
                                      ZSTDN_customMem customMem)
{
    if ((!customMem.customAlloc) ^ (!customMem.customFree)) return NULL;

    {   ZSTDN_DDict* const ddict = (ZSTDN_DDict*) ZSTDN_customMalloc(sizeof(ZSTDN_DDict), customMem);
        if (ddict == NULL) return NULL;
        ddict->cMem = customMem;
        {   size_t const initResult = ZSTDN_initDDict_internal(ddict,
                                            dict, dictSize,
                                            dictLoadMethod, dictContentType);
            if (ZSTDN_isError(initResult)) {
                ZSTDN_freeDDict(ddict);
                return NULL;
        }   }
        return ddict;
    }
}

/*! ZSTDN_createDDict() :
*   Create a digested dictionary, to start decompression without startup delay.
*   `dict` content is copied inside DDict.
*   Consequently, `dict` can be released after `ZSTDN_DDict` creation */
ZSTDN_DDict* ZSTDN_createDDict(const void* dict, size_t dictSize)
{
    ZSTDN_customMem const allocator = { NULL, NULL, NULL };
    return ZSTDN_createDDict_advanced(dict, dictSize, ZSTDN_dlm_byCopy, ZSTDN_dct_auto, allocator);
}

/*! ZSTDN_createDDict_byReference() :
 *  Create a digested dictionary, to start decompression without startup delay.
 *  Dictionary content is simply referenced, it will be accessed during decompression.
 *  Warning : dictBuffer must outlive DDict (DDict must be freed before dictBuffer) */
ZSTDN_DDict* ZSTDN_createDDict_byReference(const void* dictBuffer, size_t dictSize)
{
    ZSTDN_customMem const allocator = { NULL, NULL, NULL };
    return ZSTDN_createDDict_advanced(dictBuffer, dictSize, ZSTDN_dlm_byRef, ZSTDN_dct_auto, allocator);
}


const ZSTDN_DDict* ZSTDN_initStaticDDict(
                                void* sBuffer, size_t sBufferSize,
                                const void* dict, size_t dictSize,
                                ZSTDN_dictLoadMethod_e dictLoadMethod,
                                ZSTDN_dictContentType_e dictContentType)
{
    size_t const neededSpace = sizeof(ZSTDN_DDict)
                             + (dictLoadMethod == ZSTDN_dlm_byRef ? 0 : dictSize);
    ZSTDN_DDict* const ddict = (ZSTDN_DDict*)sBuffer;
    assert(sBuffer != NULL);
    assert(dict != NULL);
    if ((size_t)sBuffer & 7) return NULL;   /* 8-aligned */
    if (sBufferSize < neededSpace) return NULL;
    if (dictLoadMethod == ZSTDN_dlm_byCopy) {
        ZSTDN_memcpy(ddict+1, dict, dictSize);  /* local copy */
        dict = ddict+1;
    }
    if (ZSTDN_isError( ZSTDN_initDDict_internal(ddict,
                                              dict, dictSize,
                                              ZSTDN_dlm_byRef, dictContentType) ))
        return NULL;
    return ddict;
}


size_t ZSTDN_freeDDict(ZSTDN_DDict* ddict)
{
    if (ddict==NULL) return 0;   /* support free on NULL */
    {   ZSTDN_customMem const cMem = ddict->cMem;
        ZSTDN_customFree(ddict->dictBuffer, cMem);
        ZSTDN_customFree(ddict, cMem);
        return 0;
    }
}

/*! ZSTDN_estimateDDictSize() :
 *  Estimate amount of memory that will be needed to create a dictionary for decompression.
 *  Note : dictionary created by reference using ZSTDN_dlm_byRef are smaller */
size_t ZSTDN_estimateDDictSize(size_t dictSize, ZSTDN_dictLoadMethod_e dictLoadMethod)
{
    return sizeof(ZSTDN_DDict) + (dictLoadMethod == ZSTDN_dlm_byRef ? 0 : dictSize);
}

size_t ZSTDN_sizeof_DDict(const ZSTDN_DDict* ddict)
{
    if (ddict==NULL) return 0;   /* support sizeof on NULL */
    return sizeof(*ddict) + (ddict->dictBuffer ? ddict->dictSize : 0) ;
}

/*! ZSTDN_getDictID_fromDDict() :
 *  Provides the dictID of the dictionary loaded into `ddict`.
 *  If @return == 0, the dictionary is not conformant to Zstandard specification, or empty.
 *  Non-conformant dictionaries can still be loaded, but as content-only dictionaries. */
unsigned ZSTDN_getDictID_fromDDict(const ZSTDN_DDict* ddict)
{
    if (ddict==NULL) return 0;
    return ZSTDN_getDictID_fromDict(ddict->dictContent, ddict->dictSize);
}
