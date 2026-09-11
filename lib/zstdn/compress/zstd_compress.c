/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/*-*************************************
*  Dependencies
***************************************/
#include "../common/zstd_deps.h"  /* INT_MAX, ZSTDN_memset, ZSTDN_memcpy */
#include "../common/cpu.h"
#include "../common/mem.h"
#include "hist.h"           /* HIST_countFast_wksp */
#define FSEN_STATIC_LINKING_ONLY   /* FSEN_encodeSymbol */
#include "../common/fse.h"
#define HUFN_STATIC_LINKING_ONLY
#include "../common/huf.h"
#include "zstd_compress_internal.h"
#include "zstd_compress_sequences.h"
#include "zstd_compress_literals.h"
#include "zstd_fast.h"
#include "zstd_double_fast.h"
#include "zstd_lazy.h"
#include "zstd_opt.h"
#include "zstd_ldm.h"
#include "zstd_compress_superblock.h"

/* ***************************************************************
*  Tuning parameters
*****************************************************************/
/*!
 * COMPRESS_HEAPMODE :
 * Select how default decompression function ZSTDN_compress() allocates its context,
 * on stack (0, default), or into heap (1).
 * Note that functions with explicit context such as ZSTDN_compressCCtx() are unaffected.
 */


/*-*************************************
*  Helper functions
***************************************/
/* ZSTDN_compressBound()
 * Note that the result from this function is only compatible with the "normal"
 * full-block strategy.
 * When there are a lot of small blocks due to frequent flush in streaming mode
 * the overhead of headers can make the compressed data to be larger than the
 * return value of ZSTDN_compressBound().
 */
size_t ZSTDN_compressBound(size_t srcSize) {
    return ZSTDN_COMPRESSBOUND(srcSize);
}


/*-*************************************
*  Context memory management
***************************************/
struct ZSTDN_CDict_s {
    const void* dictContent;
    size_t dictContentSize;
    ZSTDN_dictContentType_e dictContentType; /* The dictContentType the CDict was created with */
    U32* entropyWorkspace; /* entropy workspace of HUFN_WORKSPACE_SIZE bytes */
    ZSTDN_cwksp workspace;
    ZSTDN_matchState_t matchState;
    ZSTDN_compressedBlockState_t cBlockState;
    ZSTDN_customMem customMem;
    U32 dictID;
    int compressionLevel; /* 0 indicates that advanced API was used to select CDict params */
};  /* typedef'd to ZSTDN_CDict within "zstd.h" */

ZSTDN_CCtx* ZSTDN_createCCtx(void)
{
    return ZSTDN_createCCtx_advanced(ZSTDN_defaultCMem);
}

static void ZSTDN_initCCtx(ZSTDN_CCtx* cctx, ZSTDN_customMem memManager)
{
    assert(cctx != NULL);
    ZSTDN_memset(cctx, 0, sizeof(*cctx));
    cctx->customMem = memManager;
    cctx->bmi2 = ZSTDN_cpuid_bmi2(ZSTDN_cpuid());
    {   size_t const err = ZSTDN_CCtx_reset(cctx, ZSTDN_reset_parameters);
        assert(!ZSTDN_isError(err));
        (void)err;
    }
}

ZSTDN_CCtx* ZSTDN_createCCtx_advanced(ZSTDN_customMem customMem)
{
    ZSTDN_STATIC_ASSERT(zcss_init==0);
    ZSTDN_STATIC_ASSERT(ZSTDN_CONTENTSIZE_UNKNOWN==(0ULL - 1));
    if ((!customMem.customAlloc) ^ (!customMem.customFree)) return NULL;
    {   ZSTDN_CCtx* const cctx = (ZSTDN_CCtx*)ZSTDN_customMalloc(sizeof(ZSTDN_CCtx), customMem);
        if (!cctx) return NULL;
        ZSTDN_initCCtx(cctx, customMem);
        return cctx;
    }
}

ZSTDN_CCtx* ZSTDN_initStaticCCtx(void* workspace, size_t workspaceSize)
{
    ZSTDN_cwksp ws;
    ZSTDN_CCtx* cctx;
    if (workspaceSize <= sizeof(ZSTDN_CCtx)) return NULL;  /* minimum size */
    if ((size_t)workspace & 7) return NULL;  /* must be 8-aligned */
    ZSTDN_cwksp_init(&ws, workspace, workspaceSize, ZSTDN_cwksp_static_alloc);

    cctx = (ZSTDN_CCtx*)ZSTDN_cwksp_reserve_object(&ws, sizeof(ZSTDN_CCtx));
    if (cctx == NULL) return NULL;

    ZSTDN_memset(cctx, 0, sizeof(ZSTDN_CCtx));
    ZSTDN_cwksp_move(&cctx->workspace, &ws);
    cctx->staticSize = workspaceSize;

    /* statically sized space. entropyWorkspace never moves (but prev/next block swap places) */
    if (!ZSTDN_cwksp_check_available(&cctx->workspace, ENTROPY_WORKSPACE_SIZE + 2 * sizeof(ZSTDN_compressedBlockState_t))) return NULL;
    cctx->blockState.prevCBlock = (ZSTDN_compressedBlockState_t*)ZSTDN_cwksp_reserve_object(&cctx->workspace, sizeof(ZSTDN_compressedBlockState_t));
    cctx->blockState.nextCBlock = (ZSTDN_compressedBlockState_t*)ZSTDN_cwksp_reserve_object(&cctx->workspace, sizeof(ZSTDN_compressedBlockState_t));
    cctx->entropyWorkspace = (U32*)ZSTDN_cwksp_reserve_object(&cctx->workspace, ENTROPY_WORKSPACE_SIZE);
    cctx->bmi2 = ZSTDN_cpuid_bmi2(ZSTDN_cpuid());
    return cctx;
}

/*
 * Clears and frees all of the dictionaries in the CCtx.
 */
static void ZSTDN_clearAllDicts(ZSTDN_CCtx* cctx)
{
    ZSTDN_customFree(cctx->localDict.dictBuffer, cctx->customMem);
    ZSTDN_freeCDict(cctx->localDict.cdict);
    ZSTDN_memset(&cctx->localDict, 0, sizeof(cctx->localDict));
    ZSTDN_memset(&cctx->prefixDict, 0, sizeof(cctx->prefixDict));
    cctx->cdict = NULL;
}

static size_t ZSTDN_sizeof_localDict(ZSTDN_localDict dict)
{
    size_t const bufferSize = dict.dictBuffer != NULL ? dict.dictSize : 0;
    size_t const cdictSize = ZSTDN_sizeof_CDict(dict.cdict);
    return bufferSize + cdictSize;
}

static void ZSTDN_freeCCtxContent(ZSTDN_CCtx* cctx)
{
    assert(cctx != NULL);
    assert(cctx->staticSize == 0);
    ZSTDN_clearAllDicts(cctx);
    ZSTDN_cwksp_free(&cctx->workspace, cctx->customMem);
}

size_t ZSTDN_freeCCtx(ZSTDN_CCtx* cctx)
{
    if (cctx==NULL) return 0;   /* support free on NULL */
    RETURN_ERROR_IF(cctx->staticSize, memory_allocation,
                    "not compatible with static CCtx");
    {
        int cctxInWorkspace = ZSTDN_cwksp_owns_buffer(&cctx->workspace, cctx);
        ZSTDN_freeCCtxContent(cctx);
        if (!cctxInWorkspace) {
            ZSTDN_customFree(cctx, cctx->customMem);
        }
    }
    return 0;
}


static size_t ZSTDN_sizeof_mtctx(const ZSTDN_CCtx* cctx)
{
    (void)cctx;
    return 0;
}


size_t ZSTDN_sizeof_CCtx(const ZSTDN_CCtx* cctx)
{
    if (cctx==NULL) return 0;   /* support sizeof on NULL */
    /* cctx may be in the workspace */
    return (cctx->workspace.workspace == cctx ? 0 : sizeof(*cctx))
           + ZSTDN_cwksp_sizeof(&cctx->workspace)
           + ZSTDN_sizeof_localDict(cctx->localDict)
           + ZSTDN_sizeof_mtctx(cctx);
}

size_t ZSTDN_sizeof_CStream(const ZSTDN_CStream* zcs)
{
    return ZSTDN_sizeof_CCtx(zcs);  /* same object */
}

/* private API call, for dictBuilder only */
const seqStore_t* ZSTDN_getSeqStore(const ZSTDN_CCtx* ctx) { return &(ctx->seqStore); }

/* Returns 1 if compression parameters are such that we should
 * enable long distance matching (wlog >= 27, strategy >= btopt).
 * Returns 0 otherwise.
 */
static U32 ZSTDN_CParams_shouldEnableLdm(const ZSTDN_compressionParameters* const cParams) {
    return cParams->strategy >= ZSTDN_btopt && cParams->windowLog >= 27;
}

static ZSTDN_CCtx_params ZSTDN_makeCCtxParamsFromCParams(
        ZSTDN_compressionParameters cParams)
{
    ZSTDN_CCtx_params cctxParams;
    /* should not matter, as all cParams are presumed properly defined */
    ZSTDN_CCtxParams_init(&cctxParams, ZSTDN_CLEVEL_DEFAULT);
    cctxParams.cParams = cParams;

    if (ZSTDN_CParams_shouldEnableLdm(&cParams)) {
        DEBUGLOG(4, "ZSTDN_makeCCtxParamsFromCParams(): Including LDM into cctx params");
        cctxParams.ldmParams.enableLdm = 1;
        /* LDM is enabled by default for optimal parser and window size >= 128MB */
        ZSTDN_ldm_adjustParameters(&cctxParams.ldmParams, &cParams);
        assert(cctxParams.ldmParams.hashLog >= cctxParams.ldmParams.bucketSizeLog);
        assert(cctxParams.ldmParams.hashRateLog < 32);
    }

    assert(!ZSTDN_checkCParams(cParams));
    return cctxParams;
}

static ZSTDN_CCtx_params* ZSTDN_createCCtxParams_advanced(
        ZSTDN_customMem customMem)
{
    ZSTDN_CCtx_params* params;
    if ((!customMem.customAlloc) ^ (!customMem.customFree)) return NULL;
    params = (ZSTDN_CCtx_params*)ZSTDN_customCalloc(
            sizeof(ZSTDN_CCtx_params), customMem);
    if (!params) { return NULL; }
    ZSTDN_CCtxParams_init(params, ZSTDN_CLEVEL_DEFAULT);
    params->customMem = customMem;
    return params;
}

ZSTDN_CCtx_params* ZSTDN_createCCtxParams(void)
{
    return ZSTDN_createCCtxParams_advanced(ZSTDN_defaultCMem);
}

size_t ZSTDN_freeCCtxParams(ZSTDN_CCtx_params* params)
{
    if (params == NULL) { return 0; }
    ZSTDN_customFree(params, params->customMem);
    return 0;
}

size_t ZSTDN_CCtxParams_reset(ZSTDN_CCtx_params* params)
{
    return ZSTDN_CCtxParams_init(params, ZSTDN_CLEVEL_DEFAULT);
}

size_t ZSTDN_CCtxParams_init(ZSTDN_CCtx_params* cctxParams, int compressionLevel) {
    RETURN_ERROR_IF(!cctxParams, GENERIC, "NULL pointer!");
    ZSTDN_memset(cctxParams, 0, sizeof(*cctxParams));
    cctxParams->compressionLevel = compressionLevel;
    cctxParams->fParams.contentSizeFlag = 1;
    return 0;
}

#define ZSTDN_NO_CLEVEL 0

/*
 * Initializes the cctxParams from params and compressionLevel.
 * @param compressionLevel If params are derived from a compression level then that compression level, otherwise ZSTDN_NO_CLEVEL.
 */
static void ZSTDN_CCtxParams_init_internal(ZSTDN_CCtx_params* cctxParams, ZSTDN_parameters const* params, int compressionLevel)
{
    assert(!ZSTDN_checkCParams(params->cParams));
    ZSTDN_memset(cctxParams, 0, sizeof(*cctxParams));
    cctxParams->cParams = params->cParams;
    cctxParams->fParams = params->fParams;
    /* Should not matter, as all cParams are presumed properly defined.
     * But, set it for tracing anyway.
     */
    cctxParams->compressionLevel = compressionLevel;
}

size_t ZSTDN_CCtxParams_init_advanced(ZSTDN_CCtx_params* cctxParams, ZSTDN_parameters params)
{
    RETURN_ERROR_IF(!cctxParams, GENERIC, "NULL pointer!");
    FORWARD_IF_ERROR( ZSTDN_checkCParams(params.cParams) , "");
    ZSTDN_CCtxParams_init_internal(cctxParams, &params, ZSTDN_NO_CLEVEL);
    return 0;
}

/*
 * Sets cctxParams' cParams and fParams from params, but otherwise leaves them alone.
 * @param param Validated zstd parameters.
 */
static void ZSTDN_CCtxParams_setZstdParams(
        ZSTDN_CCtx_params* cctxParams, const ZSTDN_parameters* params)
{
    assert(!ZSTDN_checkCParams(params->cParams));
    cctxParams->cParams = params->cParams;
    cctxParams->fParams = params->fParams;
    /* Should not matter, as all cParams are presumed properly defined.
     * But, set it for tracing anyway.
     */
    cctxParams->compressionLevel = ZSTDN_NO_CLEVEL;
}

ZSTDN_bounds ZSTDN_cParam_getBounds(ZSTDN_cParameter param)
{
    ZSTDN_bounds bounds = { 0, 0, 0 };

    switch(param)
    {
    case ZSTDN_c_compressionLevel:
        bounds.lowerBound = ZSTDN_minCLevel();
        bounds.upperBound = ZSTDN_maxCLevel();
        return bounds;

    case ZSTDN_c_windowLog:
        bounds.lowerBound = ZSTDN_WINDOWLOG_MIN;
        bounds.upperBound = ZSTDN_WINDOWLOG_MAX;
        return bounds;

    case ZSTDN_c_hashLog:
        bounds.lowerBound = ZSTDN_HASHLOG_MIN;
        bounds.upperBound = ZSTDN_HASHLOG_MAX;
        return bounds;

    case ZSTDN_c_chainLog:
        bounds.lowerBound = ZSTDN_CHAINLOG_MIN;
        bounds.upperBound = ZSTDN_CHAINLOG_MAX;
        return bounds;

    case ZSTDN_c_searchLog:
        bounds.lowerBound = ZSTDN_SEARCHLOG_MIN;
        bounds.upperBound = ZSTDN_SEARCHLOG_MAX;
        return bounds;

    case ZSTDN_c_minMatch:
        bounds.lowerBound = ZSTDN_MINMATCH_MIN;
        bounds.upperBound = ZSTDN_MINMATCH_MAX;
        return bounds;

    case ZSTDN_c_targetLength:
        bounds.lowerBound = ZSTDN_TARGETLENGTH_MIN;
        bounds.upperBound = ZSTDN_TARGETLENGTH_MAX;
        return bounds;

    case ZSTDN_c_strategy:
        bounds.lowerBound = ZSTDN_STRATEGY_MIN;
        bounds.upperBound = ZSTDN_STRATEGY_MAX;
        return bounds;

    case ZSTDN_c_contentSizeFlag:
        bounds.lowerBound = 0;
        bounds.upperBound = 1;
        return bounds;

    case ZSTDN_c_checksumFlag:
        bounds.lowerBound = 0;
        bounds.upperBound = 1;
        return bounds;

    case ZSTDN_c_dictIDFlag:
        bounds.lowerBound = 0;
        bounds.upperBound = 1;
        return bounds;

    case ZSTDN_c_nbWorkers:
        bounds.lowerBound = 0;
        bounds.upperBound = 0;
        return bounds;

    case ZSTDN_c_jobSize:
        bounds.lowerBound = 0;
        bounds.upperBound = 0;
        return bounds;

    case ZSTDN_c_overlapLog:
        bounds.lowerBound = 0;
        bounds.upperBound = 0;
        return bounds;

    case ZSTDN_c_enableDedicatedDictSearch:
        bounds.lowerBound = 0;
        bounds.upperBound = 1;
        return bounds;

    case ZSTDN_c_enableLongDistanceMatching:
        bounds.lowerBound = 0;
        bounds.upperBound = 1;
        return bounds;

    case ZSTDN_c_ldmHashLog:
        bounds.lowerBound = ZSTDN_LDM_HASHLOG_MIN;
        bounds.upperBound = ZSTDN_LDM_HASHLOG_MAX;
        return bounds;

    case ZSTDN_c_ldmMinMatch:
        bounds.lowerBound = ZSTDN_LDM_MINMATCH_MIN;
        bounds.upperBound = ZSTDN_LDM_MINMATCH_MAX;
        return bounds;

    case ZSTDN_c_ldmBucketSizeLog:
        bounds.lowerBound = ZSTDN_LDM_BUCKETSIZELOG_MIN;
        bounds.upperBound = ZSTDN_LDM_BUCKETSIZELOG_MAX;
        return bounds;

    case ZSTDN_c_ldmHashRateLog:
        bounds.lowerBound = ZSTDN_LDM_HASHRATELOG_MIN;
        bounds.upperBound = ZSTDN_LDM_HASHRATELOG_MAX;
        return bounds;

    /* experimental parameters */
    case ZSTDN_c_rsyncable:
        bounds.lowerBound = 0;
        bounds.upperBound = 1;
        return bounds;

    case ZSTDN_c_forceMaxWindow :
        bounds.lowerBound = 0;
        bounds.upperBound = 1;
        return bounds;

    case ZSTDN_c_format:
        ZSTDN_STATIC_ASSERT(ZSTDN_f_zstd1 < ZSTDN_f_zstd1_magicless);
        bounds.lowerBound = ZSTDN_f_zstd1;
        bounds.upperBound = ZSTDN_f_zstd1_magicless;   /* note : how to ensure at compile time that this is the highest value enum ? */
        return bounds;

    case ZSTDN_c_forceAttachDict:
        ZSTDN_STATIC_ASSERT(ZSTDN_dictDefaultAttach < ZSTDN_dictForceLoad);
        bounds.lowerBound = ZSTDN_dictDefaultAttach;
        bounds.upperBound = ZSTDN_dictForceLoad;       /* note : how to ensure at compile time that this is the highest value enum ? */
        return bounds;

    case ZSTDN_c_literalCompressionMode:
        ZSTDN_STATIC_ASSERT(ZSTDN_lcm_auto < ZSTDN_lcm_huffman && ZSTDN_lcm_huffman < ZSTDN_lcm_uncompressed);
        bounds.lowerBound = ZSTDN_lcm_auto;
        bounds.upperBound = ZSTDN_lcm_uncompressed;
        return bounds;

    case ZSTDN_c_targetCBlockSize:
        bounds.lowerBound = ZSTDN_TARGETCBLOCKSIZE_MIN;
        bounds.upperBound = ZSTDN_TARGETCBLOCKSIZE_MAX;
        return bounds;

    case ZSTDN_c_srcSizeHint:
        bounds.lowerBound = ZSTDN_SRCSIZEHINT_MIN;
        bounds.upperBound = ZSTDN_SRCSIZEHINT_MAX;
        return bounds;

    case ZSTDN_c_stableInBuffer:
    case ZSTDN_c_stableOutBuffer:
        bounds.lowerBound = (int)ZSTDN_bm_buffered;
        bounds.upperBound = (int)ZSTDN_bm_stable;
        return bounds;

    case ZSTDN_c_blockDelimiters:
        bounds.lowerBound = (int)ZSTDN_sf_noBlockDelimiters;
        bounds.upperBound = (int)ZSTDN_sf_explicitBlockDelimiters;
        return bounds;

    case ZSTDN_c_validateSequences:
        bounds.lowerBound = 0;
        bounds.upperBound = 1;
        return bounds;

    default:
        bounds.error = ERROR(parameter_unsupported);
        return bounds;
    }
}

/* ZSTDN_cParam_clampBounds:
 * Clamps the value into the bounded range.
 */
static size_t ZSTDN_cParam_clampBounds(ZSTDN_cParameter cParam, int* value)
{
    ZSTDN_bounds const bounds = ZSTDN_cParam_getBounds(cParam);
    if (ZSTDN_isError(bounds.error)) return bounds.error;
    if (*value < bounds.lowerBound) *value = bounds.lowerBound;
    if (*value > bounds.upperBound) *value = bounds.upperBound;
    return 0;
}

#define BOUNDCHECK(cParam, val) { \
    RETURN_ERROR_IF(!ZSTDN_cParam_withinBounds(cParam,val), \
                    parameter_outOfBound, "Param out of bounds"); \
}


static int ZSTDN_isUpdateAuthorized(ZSTDN_cParameter param)
{
    switch(param)
    {
    case ZSTDN_c_compressionLevel:
    case ZSTDN_c_hashLog:
    case ZSTDN_c_chainLog:
    case ZSTDN_c_searchLog:
    case ZSTDN_c_minMatch:
    case ZSTDN_c_targetLength:
    case ZSTDN_c_strategy:
        return 1;

    case ZSTDN_c_format:
    case ZSTDN_c_windowLog:
    case ZSTDN_c_contentSizeFlag:
    case ZSTDN_c_checksumFlag:
    case ZSTDN_c_dictIDFlag:
    case ZSTDN_c_forceMaxWindow :
    case ZSTDN_c_nbWorkers:
    case ZSTDN_c_jobSize:
    case ZSTDN_c_overlapLog:
    case ZSTDN_c_rsyncable:
    case ZSTDN_c_enableDedicatedDictSearch:
    case ZSTDN_c_enableLongDistanceMatching:
    case ZSTDN_c_ldmHashLog:
    case ZSTDN_c_ldmMinMatch:
    case ZSTDN_c_ldmBucketSizeLog:
    case ZSTDN_c_ldmHashRateLog:
    case ZSTDN_c_forceAttachDict:
    case ZSTDN_c_literalCompressionMode:
    case ZSTDN_c_targetCBlockSize:
    case ZSTDN_c_srcSizeHint:
    case ZSTDN_c_stableInBuffer:
    case ZSTDN_c_stableOutBuffer:
    case ZSTDN_c_blockDelimiters:
    case ZSTDN_c_validateSequences:
    default:
        return 0;
    }
}

size_t ZSTDN_CCtx_setParameter(ZSTDN_CCtx* cctx, ZSTDN_cParameter param, int value)
{
    DEBUGLOG(4, "ZSTDN_CCtx_setParameter (%i, %i)", (int)param, value);
    if (cctx->streamStage != zcss_init) {
        if (ZSTDN_isUpdateAuthorized(param)) {
            cctx->cParamsChanged = 1;
        } else {
            RETURN_ERROR(stage_wrong, "can only set params in ctx init stage");
    }   }

    switch(param)
    {
    case ZSTDN_c_nbWorkers:
        RETURN_ERROR_IF((value!=0) && cctx->staticSize, parameter_unsupported,
                        "MT not compatible with static alloc");
        break;

    case ZSTDN_c_compressionLevel:
    case ZSTDN_c_windowLog:
    case ZSTDN_c_hashLog:
    case ZSTDN_c_chainLog:
    case ZSTDN_c_searchLog:
    case ZSTDN_c_minMatch:
    case ZSTDN_c_targetLength:
    case ZSTDN_c_strategy:
    case ZSTDN_c_ldmHashRateLog:
    case ZSTDN_c_format:
    case ZSTDN_c_contentSizeFlag:
    case ZSTDN_c_checksumFlag:
    case ZSTDN_c_dictIDFlag:
    case ZSTDN_c_forceMaxWindow:
    case ZSTDN_c_forceAttachDict:
    case ZSTDN_c_literalCompressionMode:
    case ZSTDN_c_jobSize:
    case ZSTDN_c_overlapLog:
    case ZSTDN_c_rsyncable:
    case ZSTDN_c_enableDedicatedDictSearch:
    case ZSTDN_c_enableLongDistanceMatching:
    case ZSTDN_c_ldmHashLog:
    case ZSTDN_c_ldmMinMatch:
    case ZSTDN_c_ldmBucketSizeLog:
    case ZSTDN_c_targetCBlockSize:
    case ZSTDN_c_srcSizeHint:
    case ZSTDN_c_stableInBuffer:
    case ZSTDN_c_stableOutBuffer:
    case ZSTDN_c_blockDelimiters:
    case ZSTDN_c_validateSequences:
        break;

    default: RETURN_ERROR(parameter_unsupported, "unknown parameter");
    }
    return ZSTDN_CCtxParams_setParameter(&cctx->requestedParams, param, value);
}

size_t ZSTDN_CCtxParams_setParameter(ZSTDN_CCtx_params* CCtxParams,
                                    ZSTDN_cParameter param, int value)
{
    DEBUGLOG(4, "ZSTDN_CCtxParams_setParameter (%i, %i)", (int)param, value);
    switch(param)
    {
    case ZSTDN_c_format :
        BOUNDCHECK(ZSTDN_c_format, value);
        CCtxParams->format = (ZSTDN_format_e)value;
        return (size_t)CCtxParams->format;

    case ZSTDN_c_compressionLevel : {
        FORWARD_IF_ERROR(ZSTDN_cParam_clampBounds(param, &value), "");
        if (value == 0)
            CCtxParams->compressionLevel = ZSTDN_CLEVEL_DEFAULT; /* 0 == default */
        else
            CCtxParams->compressionLevel = value;
        if (CCtxParams->compressionLevel >= 0) return (size_t)CCtxParams->compressionLevel;
        return 0;  /* return type (size_t) cannot represent negative values */
    }

    case ZSTDN_c_windowLog :
        if (value!=0)   /* 0 => use default */
            BOUNDCHECK(ZSTDN_c_windowLog, value);
        CCtxParams->cParams.windowLog = (U32)value;
        return CCtxParams->cParams.windowLog;

    case ZSTDN_c_hashLog :
        if (value!=0)   /* 0 => use default */
            BOUNDCHECK(ZSTDN_c_hashLog, value);
        CCtxParams->cParams.hashLog = (U32)value;
        return CCtxParams->cParams.hashLog;

    case ZSTDN_c_chainLog :
        if (value!=0)   /* 0 => use default */
            BOUNDCHECK(ZSTDN_c_chainLog, value);
        CCtxParams->cParams.chainLog = (U32)value;
        return CCtxParams->cParams.chainLog;

    case ZSTDN_c_searchLog :
        if (value!=0)   /* 0 => use default */
            BOUNDCHECK(ZSTDN_c_searchLog, value);
        CCtxParams->cParams.searchLog = (U32)value;
        return (size_t)value;

    case ZSTDN_c_minMatch :
        if (value!=0)   /* 0 => use default */
            BOUNDCHECK(ZSTDN_c_minMatch, value);
        CCtxParams->cParams.minMatch = value;
        return CCtxParams->cParams.minMatch;

    case ZSTDN_c_targetLength :
        BOUNDCHECK(ZSTDN_c_targetLength, value);
        CCtxParams->cParams.targetLength = value;
        return CCtxParams->cParams.targetLength;

    case ZSTDN_c_strategy :
        if (value!=0)   /* 0 => use default */
            BOUNDCHECK(ZSTDN_c_strategy, value);
        CCtxParams->cParams.strategy = (ZSTDN_strategy)value;
        return (size_t)CCtxParams->cParams.strategy;

    case ZSTDN_c_contentSizeFlag :
        /* Content size written in frame header _when known_ (default:1) */
        DEBUGLOG(4, "set content size flag = %u", (value!=0));
        CCtxParams->fParams.contentSizeFlag = value != 0;
        return CCtxParams->fParams.contentSizeFlag;

    case ZSTDN_c_checksumFlag :
        /* A 32-bits content checksum will be calculated and written at end of frame (default:0) */
        CCtxParams->fParams.checksumFlag = value != 0;
        return CCtxParams->fParams.checksumFlag;

    case ZSTDN_c_dictIDFlag : /* When applicable, dictionary's dictID is provided in frame header (default:1) */
        DEBUGLOG(4, "set dictIDFlag = %u", (value!=0));
        CCtxParams->fParams.noDictIDFlag = !value;
        return !CCtxParams->fParams.noDictIDFlag;

    case ZSTDN_c_forceMaxWindow :
        CCtxParams->forceWindow = (value != 0);
        return CCtxParams->forceWindow;

    case ZSTDN_c_forceAttachDict : {
        const ZSTDN_dictAttachPref_e pref = (ZSTDN_dictAttachPref_e)value;
        BOUNDCHECK(ZSTDN_c_forceAttachDict, pref);
        CCtxParams->attachDictPref = pref;
        return CCtxParams->attachDictPref;
    }

    case ZSTDN_c_literalCompressionMode : {
        const ZSTDN_literalCompressionMode_e lcm = (ZSTDN_literalCompressionMode_e)value;
        BOUNDCHECK(ZSTDN_c_literalCompressionMode, lcm);
        CCtxParams->literalCompressionMode = lcm;
        return CCtxParams->literalCompressionMode;
    }

    case ZSTDN_c_nbWorkers :
        RETURN_ERROR_IF(value!=0, parameter_unsupported, "not compiled with multithreading");
        return 0;

    case ZSTDN_c_jobSize :
        RETURN_ERROR_IF(value!=0, parameter_unsupported, "not compiled with multithreading");
        return 0;

    case ZSTDN_c_overlapLog :
        RETURN_ERROR_IF(value!=0, parameter_unsupported, "not compiled with multithreading");
        return 0;

    case ZSTDN_c_rsyncable :
        RETURN_ERROR_IF(value!=0, parameter_unsupported, "not compiled with multithreading");
        return 0;

    case ZSTDN_c_enableDedicatedDictSearch :
        CCtxParams->enableDedicatedDictSearch = (value!=0);
        return CCtxParams->enableDedicatedDictSearch;

    case ZSTDN_c_enableLongDistanceMatching :
        CCtxParams->ldmParams.enableLdm = (value!=0);
        return CCtxParams->ldmParams.enableLdm;

    case ZSTDN_c_ldmHashLog :
        if (value!=0)   /* 0 ==> auto */
            BOUNDCHECK(ZSTDN_c_ldmHashLog, value);
        CCtxParams->ldmParams.hashLog = value;
        return CCtxParams->ldmParams.hashLog;

    case ZSTDN_c_ldmMinMatch :
        if (value!=0)   /* 0 ==> default */
            BOUNDCHECK(ZSTDN_c_ldmMinMatch, value);
        CCtxParams->ldmParams.minMatchLength = value;
        return CCtxParams->ldmParams.minMatchLength;

    case ZSTDN_c_ldmBucketSizeLog :
        if (value!=0)   /* 0 ==> default */
            BOUNDCHECK(ZSTDN_c_ldmBucketSizeLog, value);
        CCtxParams->ldmParams.bucketSizeLog = value;
        return CCtxParams->ldmParams.bucketSizeLog;

    case ZSTDN_c_ldmHashRateLog :
        if (value!=0)   /* 0 ==> default */
            BOUNDCHECK(ZSTDN_c_ldmHashRateLog, value);
        CCtxParams->ldmParams.hashRateLog = value;
        return CCtxParams->ldmParams.hashRateLog;

    case ZSTDN_c_targetCBlockSize :
        if (value!=0)   /* 0 ==> default */
            BOUNDCHECK(ZSTDN_c_targetCBlockSize, value);
        CCtxParams->targetCBlockSize = value;
        return CCtxParams->targetCBlockSize;

    case ZSTDN_c_srcSizeHint :
        if (value!=0)    /* 0 ==> default */
            BOUNDCHECK(ZSTDN_c_srcSizeHint, value);
        CCtxParams->srcSizeHint = value;
        return CCtxParams->srcSizeHint;

    case ZSTDN_c_stableInBuffer:
        BOUNDCHECK(ZSTDN_c_stableInBuffer, value);
        CCtxParams->inBufferMode = (ZSTDN_bufferMode_e)value;
        return CCtxParams->inBufferMode;

    case ZSTDN_c_stableOutBuffer:
        BOUNDCHECK(ZSTDN_c_stableOutBuffer, value);
        CCtxParams->outBufferMode = (ZSTDN_bufferMode_e)value;
        return CCtxParams->outBufferMode;

    case ZSTDN_c_blockDelimiters:
        BOUNDCHECK(ZSTDN_c_blockDelimiters, value);
        CCtxParams->blockDelimiters = (ZSTDN_sequenceFormat_e)value;
        return CCtxParams->blockDelimiters;

    case ZSTDN_c_validateSequences:
        BOUNDCHECK(ZSTDN_c_validateSequences, value);
        CCtxParams->validateSequences = value;
        return CCtxParams->validateSequences;

    default: RETURN_ERROR(parameter_unsupported, "unknown parameter");
    }
}

size_t ZSTDN_CCtx_getParameter(ZSTDN_CCtx const* cctx, ZSTDN_cParameter param, int* value)
{
    return ZSTDN_CCtxParams_getParameter(&cctx->requestedParams, param, value);
}

size_t ZSTDN_CCtxParams_getParameter(
        ZSTDN_CCtx_params const* CCtxParams, ZSTDN_cParameter param, int* value)
{
    switch(param)
    {
    case ZSTDN_c_format :
        *value = CCtxParams->format;
        break;
    case ZSTDN_c_compressionLevel :
        *value = CCtxParams->compressionLevel;
        break;
    case ZSTDN_c_windowLog :
        *value = (int)CCtxParams->cParams.windowLog;
        break;
    case ZSTDN_c_hashLog :
        *value = (int)CCtxParams->cParams.hashLog;
        break;
    case ZSTDN_c_chainLog :
        *value = (int)CCtxParams->cParams.chainLog;
        break;
    case ZSTDN_c_searchLog :
        *value = CCtxParams->cParams.searchLog;
        break;
    case ZSTDN_c_minMatch :
        *value = CCtxParams->cParams.minMatch;
        break;
    case ZSTDN_c_targetLength :
        *value = CCtxParams->cParams.targetLength;
        break;
    case ZSTDN_c_strategy :
        *value = (unsigned)CCtxParams->cParams.strategy;
        break;
    case ZSTDN_c_contentSizeFlag :
        *value = CCtxParams->fParams.contentSizeFlag;
        break;
    case ZSTDN_c_checksumFlag :
        *value = CCtxParams->fParams.checksumFlag;
        break;
    case ZSTDN_c_dictIDFlag :
        *value = !CCtxParams->fParams.noDictIDFlag;
        break;
    case ZSTDN_c_forceMaxWindow :
        *value = CCtxParams->forceWindow;
        break;
    case ZSTDN_c_forceAttachDict :
        *value = CCtxParams->attachDictPref;
        break;
    case ZSTDN_c_literalCompressionMode :
        *value = CCtxParams->literalCompressionMode;
        break;
    case ZSTDN_c_nbWorkers :
        assert(CCtxParams->nbWorkers == 0);
        *value = CCtxParams->nbWorkers;
        break;
    case ZSTDN_c_jobSize :
        RETURN_ERROR(parameter_unsupported, "not compiled with multithreading");
    case ZSTDN_c_overlapLog :
        RETURN_ERROR(parameter_unsupported, "not compiled with multithreading");
    case ZSTDN_c_rsyncable :
        RETURN_ERROR(parameter_unsupported, "not compiled with multithreading");
    case ZSTDN_c_enableDedicatedDictSearch :
        *value = CCtxParams->enableDedicatedDictSearch;
        break;
    case ZSTDN_c_enableLongDistanceMatching :
        *value = CCtxParams->ldmParams.enableLdm;
        break;
    case ZSTDN_c_ldmHashLog :
        *value = CCtxParams->ldmParams.hashLog;
        break;
    case ZSTDN_c_ldmMinMatch :
        *value = CCtxParams->ldmParams.minMatchLength;
        break;
    case ZSTDN_c_ldmBucketSizeLog :
        *value = CCtxParams->ldmParams.bucketSizeLog;
        break;
    case ZSTDN_c_ldmHashRateLog :
        *value = CCtxParams->ldmParams.hashRateLog;
        break;
    case ZSTDN_c_targetCBlockSize :
        *value = (int)CCtxParams->targetCBlockSize;
        break;
    case ZSTDN_c_srcSizeHint :
        *value = (int)CCtxParams->srcSizeHint;
        break;
    case ZSTDN_c_stableInBuffer :
        *value = (int)CCtxParams->inBufferMode;
        break;
    case ZSTDN_c_stableOutBuffer :
        *value = (int)CCtxParams->outBufferMode;
        break;
    case ZSTDN_c_blockDelimiters :
        *value = (int)CCtxParams->blockDelimiters;
        break;
    case ZSTDN_c_validateSequences :
        *value = (int)CCtxParams->validateSequences;
        break;
    default: RETURN_ERROR(parameter_unsupported, "unknown parameter");
    }
    return 0;
}

/* ZSTDN_CCtx_setParametersUsingCCtxParams() :
 *  just applies `params` into `cctx`
 *  no action is performed, parameters are merely stored.
 *  If ZSTDMT is enabled, parameters are pushed to cctx->mtctx.
 *    This is possible even if a compression is ongoing.
 *    In which case, new parameters will be applied on the fly, starting with next compression job.
 */
size_t ZSTDN_CCtx_setParametersUsingCCtxParams(
        ZSTDN_CCtx* cctx, const ZSTDN_CCtx_params* params)
{
    DEBUGLOG(4, "ZSTDN_CCtx_setParametersUsingCCtxParams");
    RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                    "The context is in the wrong stage!");
    RETURN_ERROR_IF(cctx->cdict, stage_wrong,
                    "Can't override parameters with cdict attached (some must "
                    "be inherited from the cdict).");

    cctx->requestedParams = *params;
    return 0;
}

ZSTDLIB_API size_t ZSTDN_CCtx_setPledgedSrcSize(ZSTDN_CCtx* cctx, unsigned long long pledgedSrcSize)
{
    DEBUGLOG(4, "ZSTDN_CCtx_setPledgedSrcSize to %u bytes", (U32)pledgedSrcSize);
    RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                    "Can't set pledgedSrcSize when not in init stage.");
    cctx->pledgedSrcSizePlusOne = pledgedSrcSize+1;
    return 0;
}

static ZSTDN_compressionParameters ZSTDN_dedicatedDictSearch_getCParams(
        int const compressionLevel,
        size_t const dictSize);
static int ZSTDN_dedicatedDictSearch_isSupported(
        const ZSTDN_compressionParameters* cParams);
static void ZSTDN_dedicatedDictSearch_revertCParams(
        ZSTDN_compressionParameters* cParams);

/*
 * Initializes the local dict using the requested parameters.
 * NOTE: This does not use the pledged src size, because it may be used for more
 * than one compression.
 */
static size_t ZSTDN_initLocalDict(ZSTDN_CCtx* cctx)
{
    ZSTDN_localDict* const dl = &cctx->localDict;
    if (dl->dict == NULL) {
        /* No local dictionary. */
        assert(dl->dictBuffer == NULL);
        assert(dl->cdict == NULL);
        assert(dl->dictSize == 0);
        return 0;
    }
    if (dl->cdict != NULL) {
        assert(cctx->cdict == dl->cdict);
        /* Local dictionary already initialized. */
        return 0;
    }
    assert(dl->dictSize > 0);
    assert(cctx->cdict == NULL);
    assert(cctx->prefixDict.dict == NULL);

    dl->cdict = ZSTDN_createCDict_advanced2(
            dl->dict,
            dl->dictSize,
            ZSTDN_dlm_byRef,
            dl->dictContentType,
            &cctx->requestedParams,
            cctx->customMem);
    RETURN_ERROR_IF(!dl->cdict, memory_allocation, "ZSTDN_createCDict_advanced failed");
    cctx->cdict = dl->cdict;
    return 0;
}

size_t ZSTDN_CCtx_loadDictionary_advanced(
        ZSTDN_CCtx* cctx, const void* dict, size_t dictSize,
        ZSTDN_dictLoadMethod_e dictLoadMethod, ZSTDN_dictContentType_e dictContentType)
{
    RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                    "Can't load a dictionary when ctx is not in init stage.");
    DEBUGLOG(4, "ZSTDN_CCtx_loadDictionary_advanced (size: %u)", (U32)dictSize);
    ZSTDN_clearAllDicts(cctx);  /* in case one already exists */
    if (dict == NULL || dictSize == 0)  /* no dictionary mode */
        return 0;
    if (dictLoadMethod == ZSTDN_dlm_byRef) {
        cctx->localDict.dict = dict;
    } else {
        void* dictBuffer;
        RETURN_ERROR_IF(cctx->staticSize, memory_allocation,
                        "no malloc for static CCtx");
        dictBuffer = ZSTDN_customMalloc(dictSize, cctx->customMem);
        RETURN_ERROR_IF(!dictBuffer, memory_allocation, "NULL pointer!");
        ZSTDN_memcpy(dictBuffer, dict, dictSize);
        cctx->localDict.dictBuffer = dictBuffer;
        cctx->localDict.dict = dictBuffer;
    }
    cctx->localDict.dictSize = dictSize;
    cctx->localDict.dictContentType = dictContentType;
    return 0;
}

ZSTDLIB_API size_t ZSTDN_CCtx_loadDictionary_byReference(
      ZSTDN_CCtx* cctx, const void* dict, size_t dictSize)
{
    return ZSTDN_CCtx_loadDictionary_advanced(
            cctx, dict, dictSize, ZSTDN_dlm_byRef, ZSTDN_dct_auto);
}

ZSTDLIB_API size_t ZSTDN_CCtx_loadDictionary(ZSTDN_CCtx* cctx, const void* dict, size_t dictSize)
{
    return ZSTDN_CCtx_loadDictionary_advanced(
            cctx, dict, dictSize, ZSTDN_dlm_byCopy, ZSTDN_dct_auto);
}


size_t ZSTDN_CCtx_refCDict(ZSTDN_CCtx* cctx, const ZSTDN_CDict* cdict)
{
    RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                    "Can't ref a dict when ctx not in init stage.");
    /* Free the existing local cdict (if any) to save memory. */
    ZSTDN_clearAllDicts(cctx);
    cctx->cdict = cdict;
    return 0;
}

size_t ZSTDN_CCtx_refThreadPool(ZSTDN_CCtx* cctx, ZSTDN_threadPool* pool)
{
    RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                    "Can't ref a pool when ctx not in init stage.");
    cctx->pool = pool;
    return 0;
}

size_t ZSTDN_CCtx_refPrefix(ZSTDN_CCtx* cctx, const void* prefix, size_t prefixSize)
{
    return ZSTDN_CCtx_refPrefix_advanced(cctx, prefix, prefixSize, ZSTDN_dct_rawContent);
}

size_t ZSTDN_CCtx_refPrefix_advanced(
        ZSTDN_CCtx* cctx, const void* prefix, size_t prefixSize, ZSTDN_dictContentType_e dictContentType)
{
    RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                    "Can't ref a prefix when ctx not in init stage.");
    ZSTDN_clearAllDicts(cctx);
    if (prefix != NULL && prefixSize > 0) {
        cctx->prefixDict.dict = prefix;
        cctx->prefixDict.dictSize = prefixSize;
        cctx->prefixDict.dictContentType = dictContentType;
    }
    return 0;
}

/*! ZSTDN_CCtx_reset() :
 *  Also dumps dictionary */
size_t ZSTDN_CCtx_reset(ZSTDN_CCtx* cctx, ZSTDN_ResetDirective reset)
{
    if ( (reset == ZSTDN_reset_session_only)
      || (reset == ZSTDN_reset_session_and_parameters) ) {
        cctx->streamStage = zcss_init;
        cctx->pledgedSrcSizePlusOne = 0;
    }
    if ( (reset == ZSTDN_reset_parameters)
      || (reset == ZSTDN_reset_session_and_parameters) ) {
        RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                        "Can't reset parameters only when not in init stage.");
        ZSTDN_clearAllDicts(cctx);
        return ZSTDN_CCtxParams_reset(&cctx->requestedParams);
    }
    return 0;
}


/* ZSTDN_checkCParams() :
    control CParam values remain within authorized range.
    @return : 0, or an error code if one value is beyond authorized range */
size_t ZSTDN_checkCParams(ZSTDN_compressionParameters cParams)
{
    BOUNDCHECK(ZSTDN_c_windowLog, (int)cParams.windowLog);
    BOUNDCHECK(ZSTDN_c_chainLog,  (int)cParams.chainLog);
    BOUNDCHECK(ZSTDN_c_hashLog,   (int)cParams.hashLog);
    BOUNDCHECK(ZSTDN_c_searchLog, (int)cParams.searchLog);
    BOUNDCHECK(ZSTDN_c_minMatch,  (int)cParams.minMatch);
    BOUNDCHECK(ZSTDN_c_targetLength,(int)cParams.targetLength);
    BOUNDCHECK(ZSTDN_c_strategy,  cParams.strategy);
    return 0;
}

/* ZSTDN_clampCParams() :
 *  make CParam values within valid range.
 *  @return : valid CParams */
static ZSTDN_compressionParameters
ZSTDN_clampCParams(ZSTDN_compressionParameters cParams)
{
#   define CLAMP_TYPE(cParam, val, type) {                                \
        ZSTDN_bounds const bounds = ZSTDN_cParam_getBounds(cParam);         \
        if ((int)val<bounds.lowerBound) val=(type)bounds.lowerBound;      \
        else if ((int)val>bounds.upperBound) val=(type)bounds.upperBound; \
    }
#   define CLAMP(cParam, val) CLAMP_TYPE(cParam, val, unsigned)
    CLAMP(ZSTDN_c_windowLog, cParams.windowLog);
    CLAMP(ZSTDN_c_chainLog,  cParams.chainLog);
    CLAMP(ZSTDN_c_hashLog,   cParams.hashLog);
    CLAMP(ZSTDN_c_searchLog, cParams.searchLog);
    CLAMP(ZSTDN_c_minMatch,  cParams.minMatch);
    CLAMP(ZSTDN_c_targetLength,cParams.targetLength);
    CLAMP_TYPE(ZSTDN_c_strategy,cParams.strategy, ZSTDN_strategy);
    return cParams;
}

/* ZSTDN_cycleLog() :
 *  condition for correct operation : hashLog > 1 */
U32 ZSTDN_cycleLog(U32 hashLog, ZSTDN_strategy strat)
{
    U32 const btScale = ((U32)strat >= (U32)ZSTDN_btlazy2);
    return hashLog - btScale;
}

/* ZSTDN_dictAndWindowLog() :
 * Returns an adjusted window log that is large enough to fit the source and the dictionary.
 * The zstd format says that the entire dictionary is valid if one byte of the dictionary
 * is within the window. So the hashLog and chainLog should be large enough to reference both
 * the dictionary and the window. So we must use this adjusted dictAndWindowLog when downsizing
 * the hashLog and windowLog.
 * NOTE: srcSize must not be ZSTDN_CONTENTSIZE_UNKNOWN.
 */
static U32 ZSTDN_dictAndWindowLog(U32 windowLog, U64 srcSize, U64 dictSize)
{
    const U64 maxWindowSize = 1ULL << ZSTDN_WINDOWLOG_MAX;
    /* No dictionary ==> No change */
    if (dictSize == 0) {
        return windowLog;
    }
    assert(windowLog <= ZSTDN_WINDOWLOG_MAX);
    assert(srcSize != ZSTDN_CONTENTSIZE_UNKNOWN); /* Handled in ZSTDN_adjustCParams_internal() */
    {
        U64 const windowSize = 1ULL << windowLog;
        U64 const dictAndWindowSize = dictSize + windowSize;
        /* If the window size is already large enough to fit both the source and the dictionary
         * then just use the window size. Otherwise adjust so that it fits the dictionary and
         * the window.
         */
        if (windowSize >= dictSize + srcSize) {
            return windowLog; /* Window size large enough already */
        } else if (dictAndWindowSize >= maxWindowSize) {
            return ZSTDN_WINDOWLOG_MAX; /* Larger than max window log */
        } else  {
            return ZSTDN_highbit32((U32)dictAndWindowSize - 1) + 1;
        }
    }
}

/* ZSTDN_adjustCParams_internal() :
 *  optimize `cPar` for a specified input (`srcSize` and `dictSize`).
 *  mostly downsize to reduce memory consumption and initialization latency.
 * `srcSize` can be ZSTDN_CONTENTSIZE_UNKNOWN when not known.
 * `mode` is the mode for parameter adjustment. See docs for `ZSTDN_cParamMode_e`.
 *  note : `srcSize==0` means 0!
 *  condition : cPar is presumed validated (can be checked using ZSTDN_checkCParams()). */
static ZSTDN_compressionParameters
ZSTDN_adjustCParams_internal(ZSTDN_compressionParameters cPar,
                            unsigned long long srcSize,
                            size_t dictSize,
                            ZSTDN_cParamMode_e mode)
{
    const U64 minSrcSize = 513; /* (1<<9) + 1 */
    const U64 maxWindowResize = 1ULL << (ZSTDN_WINDOWLOG_MAX-1);
    assert(ZSTDN_checkCParams(cPar)==0);

    switch (mode) {
    case ZSTDN_cpm_unknown:
    case ZSTDN_cpm_noAttachDict:
        /* If we don't know the source size, don't make any
         * assumptions about it. We will already have selected
         * smaller parameters if a dictionary is in use.
         */
        break;
    case ZSTDN_cpm_createCDict:
        /* Assume a small source size when creating a dictionary
         * with an unkown source size.
         */
        if (dictSize && srcSize == ZSTDN_CONTENTSIZE_UNKNOWN)
            srcSize = minSrcSize;
        break;
    case ZSTDN_cpm_attachDict:
        /* Dictionary has its own dedicated parameters which have
         * already been selected. We are selecting parameters
         * for only the source.
         */
        dictSize = 0;
        break;
    default:
        assert(0);
        break;
    }

    /* resize windowLog if input is small enough, to use less memory */
    if ( (srcSize < maxWindowResize)
      && (dictSize < maxWindowResize) )  {
        U32 const tSize = (U32)(srcSize + dictSize);
        static U32 const hashSizeMin = 1 << ZSTDN_HASHLOG_MIN;
        U32 const srcLog = (tSize < hashSizeMin) ? ZSTDN_HASHLOG_MIN :
                            ZSTDN_highbit32(tSize-1) + 1;
        if (cPar.windowLog > srcLog) cPar.windowLog = srcLog;
    }
    if (srcSize != ZSTDN_CONTENTSIZE_UNKNOWN) {
        U32 const dictAndWindowLog = ZSTDN_dictAndWindowLog(cPar.windowLog, (U64)srcSize, (U64)dictSize);
        U32 const cycleLog = ZSTDN_cycleLog(cPar.chainLog, cPar.strategy);
        if (cPar.hashLog > dictAndWindowLog+1) cPar.hashLog = dictAndWindowLog+1;
        if (cycleLog > dictAndWindowLog)
            cPar.chainLog -= (cycleLog - dictAndWindowLog);
    }

    if (cPar.windowLog < ZSTDN_WINDOWLOG_ABSOLUTEMIN)
        cPar.windowLog = ZSTDN_WINDOWLOG_ABSOLUTEMIN;  /* minimum wlog required for valid frame header */

    return cPar;
}

ZSTDN_compressionParameters
ZSTDN_adjustCParams(ZSTDN_compressionParameters cPar,
                   unsigned long long srcSize,
                   size_t dictSize)
{
    cPar = ZSTDN_clampCParams(cPar);   /* resulting cPar is necessarily valid (all parameters within range) */
    if (srcSize == 0) srcSize = ZSTDN_CONTENTSIZE_UNKNOWN;
    return ZSTDN_adjustCParams_internal(cPar, srcSize, dictSize, ZSTDN_cpm_unknown);
}

static ZSTDN_compressionParameters ZSTDN_getCParams_internal(int compressionLevel, unsigned long long srcSizeHint, size_t dictSize, ZSTDN_cParamMode_e mode);
static ZSTDN_parameters ZSTDN_getParams_internal(int compressionLevel, unsigned long long srcSizeHint, size_t dictSize, ZSTDN_cParamMode_e mode);

static void ZSTDN_overrideCParams(
              ZSTDN_compressionParameters* cParams,
        const ZSTDN_compressionParameters* overrides)
{
    if (overrides->windowLog)    cParams->windowLog    = overrides->windowLog;
    if (overrides->hashLog)      cParams->hashLog      = overrides->hashLog;
    if (overrides->chainLog)     cParams->chainLog     = overrides->chainLog;
    if (overrides->searchLog)    cParams->searchLog    = overrides->searchLog;
    if (overrides->minMatch)     cParams->minMatch     = overrides->minMatch;
    if (overrides->targetLength) cParams->targetLength = overrides->targetLength;
    if (overrides->strategy)     cParams->strategy     = overrides->strategy;
}

ZSTDN_compressionParameters ZSTDN_getCParamsFromCCtxParams(
        const ZSTDN_CCtx_params* CCtxParams, U64 srcSizeHint, size_t dictSize, ZSTDN_cParamMode_e mode)
{
    ZSTDN_compressionParameters cParams;
    if (srcSizeHint == ZSTDN_CONTENTSIZE_UNKNOWN && CCtxParams->srcSizeHint > 0) {
      srcSizeHint = CCtxParams->srcSizeHint;
    }
    cParams = ZSTDN_getCParams_internal(CCtxParams->compressionLevel, srcSizeHint, dictSize, mode);
    if (CCtxParams->ldmParams.enableLdm) cParams.windowLog = ZSTDN_LDM_DEFAULT_WINDOW_LOG;
    ZSTDN_overrideCParams(&cParams, &CCtxParams->cParams);
    assert(!ZSTDN_checkCParams(cParams));
    /* srcSizeHint == 0 means 0 */
    return ZSTDN_adjustCParams_internal(cParams, srcSizeHint, dictSize, mode);
}

static size_t
ZSTDN_sizeof_matchState(const ZSTDN_compressionParameters* const cParams,
                       const U32 forCCtx)
{
    size_t const chainSize = (cParams->strategy == ZSTDN_fast) ? 0 : ((size_t)1 << cParams->chainLog);
    size_t const hSize = ((size_t)1) << cParams->hashLog;
    U32    const hashLog3 = (forCCtx && cParams->minMatch==3) ? MIN(ZSTDN_HASHLOG3_MAX, cParams->windowLog) : 0;
    size_t const h3Size = hashLog3 ? ((size_t)1) << hashLog3 : 0;
    /* We don't use ZSTDN_cwksp_alloc_size() here because the tables aren't
     * surrounded by redzones in ASAN. */
    size_t const tableSpace = chainSize * sizeof(U32)
                            + hSize * sizeof(U32)
                            + h3Size * sizeof(U32);
    size_t const optPotentialSpace =
        ZSTDN_cwksp_alloc_size((MaxML+1) * sizeof(U32))
      + ZSTDN_cwksp_alloc_size((MaxLL+1) * sizeof(U32))
      + ZSTDN_cwksp_alloc_size((MaxOff+1) * sizeof(U32))
      + ZSTDN_cwksp_alloc_size((1<<Litbits) * sizeof(U32))
      + ZSTDN_cwksp_alloc_size((ZSTDN_OPT_NUM+1) * sizeof(ZSTDN_match_t))
      + ZSTDN_cwksp_alloc_size((ZSTDN_OPT_NUM+1) * sizeof(ZSTDN_optimal_t));
    size_t const optSpace = (forCCtx && (cParams->strategy >= ZSTDN_btopt))
                                ? optPotentialSpace
                                : 0;
    DEBUGLOG(4, "chainSize: %u - hSize: %u - h3Size: %u",
                (U32)chainSize, (U32)hSize, (U32)h3Size);
    return tableSpace + optSpace;
}

static size_t ZSTDN_estimateCCtxSize_usingCCtxParams_internal(
        const ZSTDN_compressionParameters* cParams,
        const ldmParams_t* ldmParams,
        const int isStatic,
        const size_t buffInSize,
        const size_t buffOutSize,
        const U64 pledgedSrcSize)
{
    size_t const windowSize = MAX(1, (size_t)MIN(((U64)1 << cParams->windowLog), pledgedSrcSize));
    size_t const blockSize = MIN(ZSTDN_BLOCKSIZE_MAX, windowSize);
    U32    const divider = (cParams->minMatch==3) ? 3 : 4;
    size_t const maxNbSeq = blockSize / divider;
    size_t const tokenSpace = ZSTDN_cwksp_alloc_size(WILDCOPY_OVERLENGTH + blockSize)
                            + ZSTDN_cwksp_alloc_size(maxNbSeq * sizeof(seqDef))
                            + 3 * ZSTDN_cwksp_alloc_size(maxNbSeq * sizeof(BYTE));
    size_t const entropySpace = ZSTDN_cwksp_alloc_size(ENTROPY_WORKSPACE_SIZE);
    size_t const blockStateSpace = 2 * ZSTDN_cwksp_alloc_size(sizeof(ZSTDN_compressedBlockState_t));
    size_t const matchStateSize = ZSTDN_sizeof_matchState(cParams, /* forCCtx */ 1);

    size_t const ldmSpace = ZSTDN_ldm_getTableSize(*ldmParams);
    size_t const maxNbLdmSeq = ZSTDN_ldm_getMaxNbSeq(*ldmParams, blockSize);
    size_t const ldmSeqSpace = ldmParams->enableLdm ?
        ZSTDN_cwksp_alloc_size(maxNbLdmSeq * sizeof(rawSeq)) : 0;


    size_t const bufferSpace = ZSTDN_cwksp_alloc_size(buffInSize)
                             + ZSTDN_cwksp_alloc_size(buffOutSize);

    size_t const cctxSpace = isStatic ? ZSTDN_cwksp_alloc_size(sizeof(ZSTDN_CCtx)) : 0;

    size_t const neededSpace =
        cctxSpace +
        entropySpace +
        blockStateSpace +
        ldmSpace +
        ldmSeqSpace +
        matchStateSize +
        tokenSpace +
        bufferSpace;

    DEBUGLOG(5, "estimate workspace : %u", (U32)neededSpace);
    return neededSpace;
}

size_t ZSTDN_estimateCCtxSize_usingCCtxParams(const ZSTDN_CCtx_params* params)
{
    ZSTDN_compressionParameters const cParams =
                ZSTDN_getCParamsFromCCtxParams(params, ZSTDN_CONTENTSIZE_UNKNOWN, 0, ZSTDN_cpm_noAttachDict);

    RETURN_ERROR_IF(params->nbWorkers > 0, GENERIC, "Estimate CCtx size is supported for single-threaded compression only.");
    /* estimateCCtxSize is for one-shot compression. So no buffers should
     * be needed. However, we still allocate two 0-sized buffers, which can
     * take space under ASAN. */
    return ZSTDN_estimateCCtxSize_usingCCtxParams_internal(
        &cParams, &params->ldmParams, 1, 0, 0, ZSTDN_CONTENTSIZE_UNKNOWN);
}

size_t ZSTDN_estimateCCtxSize_usingCParams(ZSTDN_compressionParameters cParams)
{
    ZSTDN_CCtx_params const params = ZSTDN_makeCCtxParamsFromCParams(cParams);
    return ZSTDN_estimateCCtxSize_usingCCtxParams(&params);
}

static size_t ZSTDN_estimateCCtxSize_internal(int compressionLevel)
{
    int tier = 0;
    size_t largestSize = 0;
    static const unsigned long long srcSizeTiers[4] = {16 KB, 128 KB, 256 KB, ZSTDN_CONTENTSIZE_UNKNOWN};
    for (; tier < 4; ++tier) {
        /* Choose the set of cParams for a given level across all srcSizes that give the largest cctxSize */
        ZSTDN_compressionParameters const cParams = ZSTDN_getCParams_internal(compressionLevel, srcSizeTiers[tier], 0, ZSTDN_cpm_noAttachDict);
        largestSize = MAX(ZSTDN_estimateCCtxSize_usingCParams(cParams), largestSize);
    }
    return largestSize;
}

size_t ZSTDN_estimateCCtxSize(int compressionLevel)
{
    int level;
    size_t memBudget = 0;
    for (level=MIN(compressionLevel, 1); level<=compressionLevel; level++) {
        /* Ensure monotonically increasing memory usage as compression level increases */
        size_t const newMB = ZSTDN_estimateCCtxSize_internal(level);
        if (newMB > memBudget) memBudget = newMB;
    }
    return memBudget;
}

size_t ZSTDN_estimateCStreamSize_usingCCtxParams(const ZSTDN_CCtx_params* params)
{
    RETURN_ERROR_IF(params->nbWorkers > 0, GENERIC, "Estimate CCtx size is supported for single-threaded compression only.");
    {   ZSTDN_compressionParameters const cParams =
                ZSTDN_getCParamsFromCCtxParams(params, ZSTDN_CONTENTSIZE_UNKNOWN, 0, ZSTDN_cpm_noAttachDict);
        size_t const blockSize = MIN(ZSTDN_BLOCKSIZE_MAX, (size_t)1 << cParams.windowLog);
        size_t const inBuffSize = (params->inBufferMode == ZSTDN_bm_buffered)
                ? ((size_t)1 << cParams.windowLog) + blockSize
                : 0;
        size_t const outBuffSize = (params->outBufferMode == ZSTDN_bm_buffered)
                ? ZSTDN_compressBound(blockSize) + 1
                : 0;

        return ZSTDN_estimateCCtxSize_usingCCtxParams_internal(
            &cParams, &params->ldmParams, 1, inBuffSize, outBuffSize,
            ZSTDN_CONTENTSIZE_UNKNOWN);
    }
}

size_t ZSTDN_estimateCStreamSize_usingCParams(ZSTDN_compressionParameters cParams)
{
    ZSTDN_CCtx_params const params = ZSTDN_makeCCtxParamsFromCParams(cParams);
    return ZSTDN_estimateCStreamSize_usingCCtxParams(&params);
}

static size_t ZSTDN_estimateCStreamSize_internal(int compressionLevel)
{
    ZSTDN_compressionParameters const cParams = ZSTDN_getCParams_internal(compressionLevel, ZSTDN_CONTENTSIZE_UNKNOWN, 0, ZSTDN_cpm_noAttachDict);
    return ZSTDN_estimateCStreamSize_usingCParams(cParams);
}

size_t ZSTDN_estimateCStreamSize(int compressionLevel)
{
    int level;
    size_t memBudget = 0;
    for (level=MIN(compressionLevel, 1); level<=compressionLevel; level++) {
        size_t const newMB = ZSTDN_estimateCStreamSize_internal(level);
        if (newMB > memBudget) memBudget = newMB;
    }
    return memBudget;
}

/* ZSTDN_getFrameProgression():
 * tells how much data has been consumed (input) and produced (output) for current frame.
 * able to count progression inside worker threads (non-blocking mode).
 */
ZSTDN_frameProgression ZSTDN_getFrameProgression(const ZSTDN_CCtx* cctx)
{
    {   ZSTDN_frameProgression fp;
        size_t const buffered = (cctx->inBuff == NULL) ? 0 :
                                cctx->inBuffPos - cctx->inToCompress;
        if (buffered) assert(cctx->inBuffPos >= cctx->inToCompress);
        assert(buffered <= ZSTDN_BLOCKSIZE_MAX);
        fp.ingested = cctx->consumedSrcSize + buffered;
        fp.consumed = cctx->consumedSrcSize;
        fp.produced = cctx->producedCSize;
        fp.flushed  = cctx->producedCSize;   /* simplified; some data might still be left within streaming output buffer */
        fp.currentJobID = 0;
        fp.nbActiveWorkers = 0;
        return fp;
}   }

/*! ZSTDN_toFlushNow()
 *  Only useful for multithreading scenarios currently (nbWorkers >= 1).
 */
size_t ZSTDN_toFlushNow(ZSTDN_CCtx* cctx)
{
    (void)cctx;
    return 0;   /* over-simplification; could also check if context is currently running in streaming mode, and in which case, report how many bytes are left to be flushed within output buffer */
}

static void ZSTDN_assertEqualCParams(ZSTDN_compressionParameters cParams1,
                                    ZSTDN_compressionParameters cParams2)
{
    (void)cParams1;
    (void)cParams2;
    assert(cParams1.windowLog    == cParams2.windowLog);
    assert(cParams1.chainLog     == cParams2.chainLog);
    assert(cParams1.hashLog      == cParams2.hashLog);
    assert(cParams1.searchLog    == cParams2.searchLog);
    assert(cParams1.minMatch     == cParams2.minMatch);
    assert(cParams1.targetLength == cParams2.targetLength);
    assert(cParams1.strategy     == cParams2.strategy);
}

void ZSTDN_reset_compressedBlockState(ZSTDN_compressedBlockState_t* bs)
{
    int i;
    for (i = 0; i < ZSTDN_REP_NUM; ++i)
        bs->rep[i] = repStartValue[i];
    bs->entropy.huf.repeatMode = HUFN_repeat_none;
    bs->entropy.fse.offcode_repeatMode = FSEN_repeat_none;
    bs->entropy.fse.matchlength_repeatMode = FSEN_repeat_none;
    bs->entropy.fse.litlength_repeatMode = FSEN_repeat_none;
}

/*! ZSTDN_invalidateMatchState()
 *  Invalidate all the matches in the match finder tables.
 *  Requires nextSrc and base to be set (can be NULL).
 */
static void ZSTDN_invalidateMatchState(ZSTDN_matchState_t* ms)
{
    ZSTDN_window_clear(&ms->window);

    ms->nextToUpdate = ms->window.dictLimit;
    ms->loadedDictEnd = 0;
    ms->opt.litLengthSum = 0;  /* force reset of btopt stats */
    ms->dictMatchState = NULL;
}

/*
 * Controls, for this matchState reset, whether the tables need to be cleared /
 * prepared for the coming compression (ZSTDcrp_makeClean), or whether the
 * tables can be left unclean (ZSTDcrp_leaveDirty), because we know that a
 * subsequent operation will overwrite the table space anyways (e.g., copying
 * the matchState contents in from a CDict).
 */
typedef enum {
    ZSTDcrp_makeClean,
    ZSTDcrp_leaveDirty
} ZSTDN_compResetPolicy_e;

/*
 * Controls, for this matchState reset, whether indexing can continue where it
 * left off (ZSTDirp_continue), or whether it needs to be restarted from zero
 * (ZSTDirp_reset).
 */
typedef enum {
    ZSTDirp_continue,
    ZSTDirp_reset
} ZSTDN_indexResetPolicy_e;

typedef enum {
    ZSTDN_resetTarget_CDict,
    ZSTDN_resetTarget_CCtx
} ZSTDN_resetTarget_e;

static size_t
ZSTDN_reset_matchState(ZSTDN_matchState_t* ms,
                      ZSTDN_cwksp* ws,
                const ZSTDN_compressionParameters* cParams,
                const ZSTDN_compResetPolicy_e crp,
                const ZSTDN_indexResetPolicy_e forceResetIndex,
                const ZSTDN_resetTarget_e forWho)
{
    size_t const chainSize = (cParams->strategy == ZSTDN_fast) ? 0 : ((size_t)1 << cParams->chainLog);
    size_t const hSize = ((size_t)1) << cParams->hashLog;
    U32    const hashLog3 = ((forWho == ZSTDN_resetTarget_CCtx) && cParams->minMatch==3) ? MIN(ZSTDN_HASHLOG3_MAX, cParams->windowLog) : 0;
    size_t const h3Size = hashLog3 ? ((size_t)1) << hashLog3 : 0;

    DEBUGLOG(4, "reset indices : %u", forceResetIndex == ZSTDirp_reset);
    if (forceResetIndex == ZSTDirp_reset) {
        ZSTDN_window_init(&ms->window);
        ZSTDN_cwksp_mark_tables_dirty(ws);
    }

    ms->hashLog3 = hashLog3;

    ZSTDN_invalidateMatchState(ms);

    assert(!ZSTDN_cwksp_reserve_failed(ws)); /* check that allocation hasn't already failed */

    ZSTDN_cwksp_clear_tables(ws);

    DEBUGLOG(5, "reserving table space");
    /* table Space */
    ms->hashTable = (U32*)ZSTDN_cwksp_reserve_table(ws, hSize * sizeof(U32));
    ms->chainTable = (U32*)ZSTDN_cwksp_reserve_table(ws, chainSize * sizeof(U32));
    ms->hashTable3 = (U32*)ZSTDN_cwksp_reserve_table(ws, h3Size * sizeof(U32));
    RETURN_ERROR_IF(ZSTDN_cwksp_reserve_failed(ws), memory_allocation,
                    "failed a workspace allocation in ZSTDN_reset_matchState");

    DEBUGLOG(4, "reset table : %u", crp!=ZSTDcrp_leaveDirty);
    if (crp!=ZSTDcrp_leaveDirty) {
        /* reset tables only */
        ZSTDN_cwksp_clean_tables(ws);
    }

    /* opt parser space */
    if ((forWho == ZSTDN_resetTarget_CCtx) && (cParams->strategy >= ZSTDN_btopt)) {
        DEBUGLOG(4, "reserving optimal parser space");
        ms->opt.litFreq = (unsigned*)ZSTDN_cwksp_reserve_aligned(ws, (1<<Litbits) * sizeof(unsigned));
        ms->opt.litLengthFreq = (unsigned*)ZSTDN_cwksp_reserve_aligned(ws, (MaxLL+1) * sizeof(unsigned));
        ms->opt.matchLengthFreq = (unsigned*)ZSTDN_cwksp_reserve_aligned(ws, (MaxML+1) * sizeof(unsigned));
        ms->opt.offCodeFreq = (unsigned*)ZSTDN_cwksp_reserve_aligned(ws, (MaxOff+1) * sizeof(unsigned));
        ms->opt.matchTable = (ZSTDN_match_t*)ZSTDN_cwksp_reserve_aligned(ws, (ZSTDN_OPT_NUM+1) * sizeof(ZSTDN_match_t));
        ms->opt.priceTable = (ZSTDN_optimal_t*)ZSTDN_cwksp_reserve_aligned(ws, (ZSTDN_OPT_NUM+1) * sizeof(ZSTDN_optimal_t));
    }

    ms->cParams = *cParams;

    RETURN_ERROR_IF(ZSTDN_cwksp_reserve_failed(ws), memory_allocation,
                    "failed a workspace allocation in ZSTDN_reset_matchState");

    return 0;
}

/* ZSTDN_indexTooCloseToMax() :
 * minor optimization : prefer memset() rather than reduceIndex()
 * which is measurably slow in some circumstances (reported for Visual Studio).
 * Works when re-using a context for a lot of smallish inputs :
 * if all inputs are smaller than ZSTDN_INDEXOVERFLOW_MARGIN,
 * memset() will be triggered before reduceIndex().
 */
#define ZSTDN_INDEXOVERFLOW_MARGIN (16 MB)
static int ZSTDN_indexTooCloseToMax(ZSTDN_window_t w)
{
    return (size_t)(w.nextSrc - w.base) > (ZSTDN_CURRENT_MAX - ZSTDN_INDEXOVERFLOW_MARGIN);
}

/*! ZSTDN_resetCCtx_internal() :
    note : `params` are assumed fully validated at this stage */
static size_t ZSTDN_resetCCtx_internal(ZSTDN_CCtx* zc,
                                      ZSTDN_CCtx_params params,
                                      U64 const pledgedSrcSize,
                                      ZSTDN_compResetPolicy_e const crp,
                                      ZSTDN_buffered_policy_e const zbuff)
{
    ZSTDN_cwksp* const ws = &zc->workspace;
    DEBUGLOG(4, "ZSTDN_resetCCtx_internal: pledgedSrcSize=%u, wlog=%u",
                (U32)pledgedSrcSize, params.cParams.windowLog);
    assert(!ZSTDN_isError(ZSTDN_checkCParams(params.cParams)));

    zc->isFirstBlock = 1;

    if (params.ldmParams.enableLdm) {
        /* Adjust long distance matching parameters */
        ZSTDN_ldm_adjustParameters(&params.ldmParams, &params.cParams);
        assert(params.ldmParams.hashLog >= params.ldmParams.bucketSizeLog);
        assert(params.ldmParams.hashRateLog < 32);
    }

    {   size_t const windowSize = MAX(1, (size_t)MIN(((U64)1 << params.cParams.windowLog), pledgedSrcSize));
        size_t const blockSize = MIN(ZSTDN_BLOCKSIZE_MAX, windowSize);
        U32    const divider = (params.cParams.minMatch==3) ? 3 : 4;
        size_t const maxNbSeq = blockSize / divider;
        size_t const buffOutSize = (zbuff == ZSTDb_buffered && params.outBufferMode == ZSTDN_bm_buffered)
                ? ZSTDN_compressBound(blockSize) + 1
                : 0;
        size_t const buffInSize = (zbuff == ZSTDb_buffered && params.inBufferMode == ZSTDN_bm_buffered)
                ? windowSize + blockSize
                : 0;
        size_t const maxNbLdmSeq = ZSTDN_ldm_getMaxNbSeq(params.ldmParams, blockSize);

        int const indexTooClose = ZSTDN_indexTooCloseToMax(zc->blockState.matchState.window);
        ZSTDN_indexResetPolicy_e needsIndexReset =
            (!indexTooClose && zc->initialized) ? ZSTDirp_continue : ZSTDirp_reset;

        size_t const neededSpace =
            ZSTDN_estimateCCtxSize_usingCCtxParams_internal(
                &params.cParams, &params.ldmParams, zc->staticSize != 0,
                buffInSize, buffOutSize, pledgedSrcSize);
        FORWARD_IF_ERROR(neededSpace, "cctx size estimate failed!");

        if (!zc->staticSize) ZSTDN_cwksp_bump_oversized_duration(ws, 0);

        /* Check if workspace is large enough, alloc a new one if needed */
        {
            int const workspaceTooSmall = ZSTDN_cwksp_sizeof(ws) < neededSpace;
            int const workspaceWasteful = ZSTDN_cwksp_check_wasteful(ws, neededSpace);

            DEBUGLOG(4, "Need %zu B workspace", neededSpace);
            DEBUGLOG(4, "windowSize: %zu - blockSize: %zu", windowSize, blockSize);

            if (workspaceTooSmall || workspaceWasteful) {
                DEBUGLOG(4, "Resize workspaceSize from %zuKB to %zuKB",
                            ZSTDN_cwksp_sizeof(ws) >> 10,
                            neededSpace >> 10);

                RETURN_ERROR_IF(zc->staticSize, memory_allocation, "static cctx : no resize");

                needsIndexReset = ZSTDirp_reset;

                ZSTDN_cwksp_free(ws, zc->customMem);
                FORWARD_IF_ERROR(ZSTDN_cwksp_create(ws, neededSpace, zc->customMem), "");

                DEBUGLOG(5, "reserving object space");
                /* Statically sized space.
                 * entropyWorkspace never moves,
                 * though prev/next block swap places */
                assert(ZSTDN_cwksp_check_available(ws, 2 * sizeof(ZSTDN_compressedBlockState_t)));
                zc->blockState.prevCBlock = (ZSTDN_compressedBlockState_t*) ZSTDN_cwksp_reserve_object(ws, sizeof(ZSTDN_compressedBlockState_t));
                RETURN_ERROR_IF(zc->blockState.prevCBlock == NULL, memory_allocation, "couldn't allocate prevCBlock");
                zc->blockState.nextCBlock = (ZSTDN_compressedBlockState_t*) ZSTDN_cwksp_reserve_object(ws, sizeof(ZSTDN_compressedBlockState_t));
                RETURN_ERROR_IF(zc->blockState.nextCBlock == NULL, memory_allocation, "couldn't allocate nextCBlock");
                zc->entropyWorkspace = (U32*) ZSTDN_cwksp_reserve_object(ws, ENTROPY_WORKSPACE_SIZE);
                RETURN_ERROR_IF(zc->blockState.nextCBlock == NULL, memory_allocation, "couldn't allocate entropyWorkspace");
        }   }

        ZSTDN_cwksp_clear(ws);

        /* init params */
        zc->appliedParams = params;
        zc->blockState.matchState.cParams = params.cParams;
        zc->pledgedSrcSizePlusOne = pledgedSrcSize+1;
        zc->consumedSrcSize = 0;
        zc->producedCSize = 0;
        if (pledgedSrcSize == ZSTDN_CONTENTSIZE_UNKNOWN)
            zc->appliedParams.fParams.contentSizeFlag = 0;
        DEBUGLOG(4, "pledged content size : %u ; flag : %u",
            (unsigned)pledgedSrcSize, zc->appliedParams.fParams.contentSizeFlag);
        zc->blockSize = blockSize;

        xxh64n_reset(&zc->xxhState, 0);
        zc->stage = ZSTDcs_init;
        zc->dictID = 0;
        zc->dictContentSize = 0;

        ZSTDN_reset_compressedBlockState(zc->blockState.prevCBlock);

        /* ZSTDN_wildcopy() is used to copy into the literals buffer,
         * so we have to oversize the buffer by WILDCOPY_OVERLENGTH bytes.
         */
        zc->seqStore.litStart = ZSTDN_cwksp_reserve_buffer(ws, blockSize + WILDCOPY_OVERLENGTH);
        zc->seqStore.maxNbLit = blockSize;

        /* buffers */
        zc->bufferedPolicy = zbuff;
        zc->inBuffSize = buffInSize;
        zc->inBuff = (char*)ZSTDN_cwksp_reserve_buffer(ws, buffInSize);
        zc->outBuffSize = buffOutSize;
        zc->outBuff = (char*)ZSTDN_cwksp_reserve_buffer(ws, buffOutSize);

        /* ldm bucketOffsets table */
        if (params.ldmParams.enableLdm) {
            /* TODO: avoid memset? */
            size_t const numBuckets =
                  ((size_t)1) << (params.ldmParams.hashLog -
                                  params.ldmParams.bucketSizeLog);
            zc->ldmState.bucketOffsets = ZSTDN_cwksp_reserve_buffer(ws, numBuckets);
            ZSTDN_memset(zc->ldmState.bucketOffsets, 0, numBuckets);
        }

        /* sequences storage */
        ZSTDN_referenceExternalSequences(zc, NULL, 0);
        zc->seqStore.maxNbSeq = maxNbSeq;
        zc->seqStore.llCode = ZSTDN_cwksp_reserve_buffer(ws, maxNbSeq * sizeof(BYTE));
        zc->seqStore.mlCode = ZSTDN_cwksp_reserve_buffer(ws, maxNbSeq * sizeof(BYTE));
        zc->seqStore.ofCode = ZSTDN_cwksp_reserve_buffer(ws, maxNbSeq * sizeof(BYTE));
        zc->seqStore.sequencesStart = (seqDef*)ZSTDN_cwksp_reserve_aligned(ws, maxNbSeq * sizeof(seqDef));

        FORWARD_IF_ERROR(ZSTDN_reset_matchState(
            &zc->blockState.matchState,
            ws,
            &params.cParams,
            crp,
            needsIndexReset,
            ZSTDN_resetTarget_CCtx), "");

        /* ldm hash table */
        if (params.ldmParams.enableLdm) {
            /* TODO: avoid memset? */
            size_t const ldmHSize = ((size_t)1) << params.ldmParams.hashLog;
            zc->ldmState.hashTable = (ldmEntry_t*)ZSTDN_cwksp_reserve_aligned(ws, ldmHSize * sizeof(ldmEntry_t));
            ZSTDN_memset(zc->ldmState.hashTable, 0, ldmHSize * sizeof(ldmEntry_t));
            zc->ldmSequences = (rawSeq*)ZSTDN_cwksp_reserve_aligned(ws, maxNbLdmSeq * sizeof(rawSeq));
            zc->maxNbLdmSequences = maxNbLdmSeq;

            ZSTDN_window_init(&zc->ldmState.window);
            ZSTDN_window_clear(&zc->ldmState.window);
            zc->ldmState.loadedDictEnd = 0;
        }

        /* Due to alignment, when reusing a workspace, we can actually consume
         * up to 3 extra bytes for alignment. See the comments in zstd_cwksp.h
         */
        assert(ZSTDN_cwksp_used(ws) >= neededSpace &&
               ZSTDN_cwksp_used(ws) <= neededSpace + 3);

        DEBUGLOG(3, "wksp: finished allocating, %zd bytes remain available", ZSTDN_cwksp_available_space(ws));
        zc->initialized = 1;

        return 0;
    }
}

/* ZSTDN_invalidateRepCodes() :
 * ensures next compression will not use repcodes from previous block.
 * Note : only works with regular variant;
 *        do not use with extDict variant ! */
void ZSTDN_invalidateRepCodes(ZSTDN_CCtx* cctx) {
    int i;
    for (i=0; i<ZSTDN_REP_NUM; i++) cctx->blockState.prevCBlock->rep[i] = 0;
    assert(!ZSTDN_window_hasExtDict(cctx->blockState.matchState.window));
}

/* These are the approximate sizes for each strategy past which copying the
 * dictionary tables into the working context is faster than using them
 * in-place.
 */
static const size_t attachDictSizeCutoffs[ZSTDN_STRATEGY_MAX+1] = {
    8 KB,  /* unused */
    8 KB,  /* ZSTDN_fast */
    16 KB, /* ZSTDN_dfast */
    32 KB, /* ZSTDN_greedy */
    32 KB, /* ZSTDN_lazy */
    32 KB, /* ZSTDN_lazy2 */
    32 KB, /* ZSTDN_btlazy2 */
    32 KB, /* ZSTDN_btopt */
    8 KB,  /* ZSTDN_btultra */
    8 KB   /* ZSTDN_btultra2 */
};

static int ZSTDN_shouldAttachDict(const ZSTDN_CDict* cdict,
                                 const ZSTDN_CCtx_params* params,
                                 U64 pledgedSrcSize)
{
    size_t cutoff = attachDictSizeCutoffs[cdict->matchState.cParams.strategy];
    int const dedicatedDictSearch = cdict->matchState.dedicatedDictSearch;
    return dedicatedDictSearch
        || ( ( pledgedSrcSize <= cutoff
            || pledgedSrcSize == ZSTDN_CONTENTSIZE_UNKNOWN
            || params->attachDictPref == ZSTDN_dictForceAttach )
          && params->attachDictPref != ZSTDN_dictForceCopy
          && !params->forceWindow ); /* dictMatchState isn't correctly
                                      * handled in _enforceMaxDist */
}

static size_t
ZSTDN_resetCCtx_byAttachingCDict(ZSTDN_CCtx* cctx,
                        const ZSTDN_CDict* cdict,
                        ZSTDN_CCtx_params params,
                        U64 pledgedSrcSize,
                        ZSTDN_buffered_policy_e zbuff)
{
    {
        ZSTDN_compressionParameters adjusted_cdict_cParams = cdict->matchState.cParams;
        unsigned const windowLog = params.cParams.windowLog;
        assert(windowLog != 0);
        /* Resize working context table params for input only, since the dict
         * has its own tables. */
        /* pledgedSrcSize == 0 means 0! */

        if (cdict->matchState.dedicatedDictSearch) {
            ZSTDN_dedicatedDictSearch_revertCParams(&adjusted_cdict_cParams);
        }

        params.cParams = ZSTDN_adjustCParams_internal(adjusted_cdict_cParams, pledgedSrcSize,
                                                     cdict->dictContentSize, ZSTDN_cpm_attachDict);
        params.cParams.windowLog = windowLog;
        FORWARD_IF_ERROR(ZSTDN_resetCCtx_internal(cctx, params, pledgedSrcSize,
                                                 ZSTDcrp_makeClean, zbuff), "");
        assert(cctx->appliedParams.cParams.strategy == adjusted_cdict_cParams.strategy);
    }

    {   const U32 cdictEnd = (U32)( cdict->matchState.window.nextSrc
                                  - cdict->matchState.window.base);
        const U32 cdictLen = cdictEnd - cdict->matchState.window.dictLimit;
        if (cdictLen == 0) {
            /* don't even attach dictionaries with no contents */
            DEBUGLOG(4, "skipping attaching empty dictionary");
        } else {
            DEBUGLOG(4, "attaching dictionary into context");
            cctx->blockState.matchState.dictMatchState = &cdict->matchState;

            /* prep working match state so dict matches never have negative indices
             * when they are translated to the working context's index space. */
            if (cctx->blockState.matchState.window.dictLimit < cdictEnd) {
                cctx->blockState.matchState.window.nextSrc =
                    cctx->blockState.matchState.window.base + cdictEnd;
                ZSTDN_window_clear(&cctx->blockState.matchState.window);
            }
            /* loadedDictEnd is expressed within the referential of the active context */
            cctx->blockState.matchState.loadedDictEnd = cctx->blockState.matchState.window.dictLimit;
    }   }

    cctx->dictID = cdict->dictID;
    cctx->dictContentSize = cdict->dictContentSize;

    /* copy block state */
    ZSTDN_memcpy(cctx->blockState.prevCBlock, &cdict->cBlockState, sizeof(cdict->cBlockState));

    return 0;
}

static size_t ZSTDN_resetCCtx_byCopyingCDict(ZSTDN_CCtx* cctx,
                            const ZSTDN_CDict* cdict,
                            ZSTDN_CCtx_params params,
                            U64 pledgedSrcSize,
                            ZSTDN_buffered_policy_e zbuff)
{
    const ZSTDN_compressionParameters *cdict_cParams = &cdict->matchState.cParams;

    assert(!cdict->matchState.dedicatedDictSearch);

    DEBUGLOG(4, "copying dictionary into context");

    {   unsigned const windowLog = params.cParams.windowLog;
        assert(windowLog != 0);
        /* Copy only compression parameters related to tables. */
        params.cParams = *cdict_cParams;
        params.cParams.windowLog = windowLog;
        FORWARD_IF_ERROR(ZSTDN_resetCCtx_internal(cctx, params, pledgedSrcSize,
                                                 ZSTDcrp_leaveDirty, zbuff), "");
        assert(cctx->appliedParams.cParams.strategy == cdict_cParams->strategy);
        assert(cctx->appliedParams.cParams.hashLog == cdict_cParams->hashLog);
        assert(cctx->appliedParams.cParams.chainLog == cdict_cParams->chainLog);
    }

    ZSTDN_cwksp_mark_tables_dirty(&cctx->workspace);

    /* copy tables */
    {   size_t const chainSize = (cdict_cParams->strategy == ZSTDN_fast) ? 0 : ((size_t)1 << cdict_cParams->chainLog);
        size_t const hSize =  (size_t)1 << cdict_cParams->hashLog;

        ZSTDN_memcpy(cctx->blockState.matchState.hashTable,
               cdict->matchState.hashTable,
               hSize * sizeof(U32));
        ZSTDN_memcpy(cctx->blockState.matchState.chainTable,
               cdict->matchState.chainTable,
               chainSize * sizeof(U32));
    }

    /* Zero the hashTable3, since the cdict never fills it */
    {   int const h3log = cctx->blockState.matchState.hashLog3;
        size_t const h3Size = h3log ? ((size_t)1 << h3log) : 0;
        assert(cdict->matchState.hashLog3 == 0);
        ZSTDN_memset(cctx->blockState.matchState.hashTable3, 0, h3Size * sizeof(U32));
    }

    ZSTDN_cwksp_mark_tables_clean(&cctx->workspace);

    /* copy dictionary offsets */
    {   ZSTDN_matchState_t const* srcMatchState = &cdict->matchState;
        ZSTDN_matchState_t* dstMatchState = &cctx->blockState.matchState;
        dstMatchState->window       = srcMatchState->window;
        dstMatchState->nextToUpdate = srcMatchState->nextToUpdate;
        dstMatchState->loadedDictEnd= srcMatchState->loadedDictEnd;
    }

    cctx->dictID = cdict->dictID;
    cctx->dictContentSize = cdict->dictContentSize;

    /* copy block state */
    ZSTDN_memcpy(cctx->blockState.prevCBlock, &cdict->cBlockState, sizeof(cdict->cBlockState));

    return 0;
}

/* We have a choice between copying the dictionary context into the working
 * context, or referencing the dictionary context from the working context
 * in-place. We decide here which strategy to use. */
static size_t ZSTDN_resetCCtx_usingCDict(ZSTDN_CCtx* cctx,
                            const ZSTDN_CDict* cdict,
                            const ZSTDN_CCtx_params* params,
                            U64 pledgedSrcSize,
                            ZSTDN_buffered_policy_e zbuff)
{

    DEBUGLOG(4, "ZSTDN_resetCCtx_usingCDict (pledgedSrcSize=%u)",
                (unsigned)pledgedSrcSize);

    if (ZSTDN_shouldAttachDict(cdict, params, pledgedSrcSize)) {
        return ZSTDN_resetCCtx_byAttachingCDict(
            cctx, cdict, *params, pledgedSrcSize, zbuff);
    } else {
        return ZSTDN_resetCCtx_byCopyingCDict(
            cctx, cdict, *params, pledgedSrcSize, zbuff);
    }
}

/*! ZSTDN_copyCCtx_internal() :
 *  Duplicate an existing context `srcCCtx` into another one `dstCCtx`.
 *  Only works during stage ZSTDcs_init (i.e. after creation, but before first call to ZSTDN_compressContinue()).
 *  The "context", in this case, refers to the hash and chain tables,
 *  entropy tables, and dictionary references.
 * `windowLog` value is enforced if != 0, otherwise value is copied from srcCCtx.
 * @return : 0, or an error code */
static size_t ZSTDN_copyCCtx_internal(ZSTDN_CCtx* dstCCtx,
                            const ZSTDN_CCtx* srcCCtx,
                            ZSTDN_frameParameters fParams,
                            U64 pledgedSrcSize,
                            ZSTDN_buffered_policy_e zbuff)
{
    DEBUGLOG(5, "ZSTDN_copyCCtx_internal");
    RETURN_ERROR_IF(srcCCtx->stage!=ZSTDcs_init, stage_wrong,
                    "Can't copy a ctx that's not in init stage.");

    ZSTDN_memcpy(&dstCCtx->customMem, &srcCCtx->customMem, sizeof(ZSTDN_customMem));
    {   ZSTDN_CCtx_params params = dstCCtx->requestedParams;
        /* Copy only compression parameters related to tables. */
        params.cParams = srcCCtx->appliedParams.cParams;
        params.fParams = fParams;
        ZSTDN_resetCCtx_internal(dstCCtx, params, pledgedSrcSize,
                                ZSTDcrp_leaveDirty, zbuff);
        assert(dstCCtx->appliedParams.cParams.windowLog == srcCCtx->appliedParams.cParams.windowLog);
        assert(dstCCtx->appliedParams.cParams.strategy == srcCCtx->appliedParams.cParams.strategy);
        assert(dstCCtx->appliedParams.cParams.hashLog == srcCCtx->appliedParams.cParams.hashLog);
        assert(dstCCtx->appliedParams.cParams.chainLog == srcCCtx->appliedParams.cParams.chainLog);
        assert(dstCCtx->blockState.matchState.hashLog3 == srcCCtx->blockState.matchState.hashLog3);
    }

    ZSTDN_cwksp_mark_tables_dirty(&dstCCtx->workspace);

    /* copy tables */
    {   size_t const chainSize = (srcCCtx->appliedParams.cParams.strategy == ZSTDN_fast) ? 0 : ((size_t)1 << srcCCtx->appliedParams.cParams.chainLog);
        size_t const hSize =  (size_t)1 << srcCCtx->appliedParams.cParams.hashLog;
        int const h3log = srcCCtx->blockState.matchState.hashLog3;
        size_t const h3Size = h3log ? ((size_t)1 << h3log) : 0;

        ZSTDN_memcpy(dstCCtx->blockState.matchState.hashTable,
               srcCCtx->blockState.matchState.hashTable,
               hSize * sizeof(U32));
        ZSTDN_memcpy(dstCCtx->blockState.matchState.chainTable,
               srcCCtx->blockState.matchState.chainTable,
               chainSize * sizeof(U32));
        ZSTDN_memcpy(dstCCtx->blockState.matchState.hashTable3,
               srcCCtx->blockState.matchState.hashTable3,
               h3Size * sizeof(U32));
    }

    ZSTDN_cwksp_mark_tables_clean(&dstCCtx->workspace);

    /* copy dictionary offsets */
    {
        const ZSTDN_matchState_t* srcMatchState = &srcCCtx->blockState.matchState;
        ZSTDN_matchState_t* dstMatchState = &dstCCtx->blockState.matchState;
        dstMatchState->window       = srcMatchState->window;
        dstMatchState->nextToUpdate = srcMatchState->nextToUpdate;
        dstMatchState->loadedDictEnd= srcMatchState->loadedDictEnd;
    }
    dstCCtx->dictID = srcCCtx->dictID;
    dstCCtx->dictContentSize = srcCCtx->dictContentSize;

    /* copy block state */
    ZSTDN_memcpy(dstCCtx->blockState.prevCBlock, srcCCtx->blockState.prevCBlock, sizeof(*srcCCtx->blockState.prevCBlock));

    return 0;
}

/*! ZSTDN_copyCCtx() :
 *  Duplicate an existing context `srcCCtx` into another one `dstCCtx`.
 *  Only works during stage ZSTDcs_init (i.e. after creation, but before first call to ZSTDN_compressContinue()).
 *  pledgedSrcSize==0 means "unknown".
*   @return : 0, or an error code */
size_t ZSTDN_copyCCtx(ZSTDN_CCtx* dstCCtx, const ZSTDN_CCtx* srcCCtx, unsigned long long pledgedSrcSize)
{
    ZSTDN_frameParameters fParams = { 1 /*content*/, 0 /*checksum*/, 0 /*noDictID*/ };
    ZSTDN_buffered_policy_e const zbuff = srcCCtx->bufferedPolicy;
    ZSTDN_STATIC_ASSERT((U32)ZSTDb_buffered==1);
    if (pledgedSrcSize==0) pledgedSrcSize = ZSTDN_CONTENTSIZE_UNKNOWN;
    fParams.contentSizeFlag = (pledgedSrcSize != ZSTDN_CONTENTSIZE_UNKNOWN);

    return ZSTDN_copyCCtx_internal(dstCCtx, srcCCtx,
                                fParams, pledgedSrcSize,
                                zbuff);
}


#define ZSTDN_ROWSIZE 16
/*! ZSTDN_reduceTable() :
 *  reduce table indexes by `reducerValue`, or squash to zero.
 *  PreserveMark preserves "unsorted mark" for btlazy2 strategy.
 *  It must be set to a clear 0/1 value, to remove branch during inlining.
 *  Presume table size is a multiple of ZSTDN_ROWSIZE
 *  to help auto-vectorization */
FORCE_INLINE_TEMPLATE void
ZSTDN_reduceTable_internal (U32* const table, U32 const size, U32 const reducerValue, int const preserveMark)
{
    int const nbRows = (int)size / ZSTDN_ROWSIZE;
    int cellNb = 0;
    int rowNb;
    assert((size & (ZSTDN_ROWSIZE-1)) == 0);  /* multiple of ZSTDN_ROWSIZE */
    assert(size < (1U<<31));   /* can be casted to int */


    for (rowNb=0 ; rowNb < nbRows ; rowNb++) {
        int column;
        for (column=0; column<ZSTDN_ROWSIZE; column++) {
            if (preserveMark) {
                U32 const adder = (table[cellNb] == ZSTDN_DUBT_UNSORTED_MARK) ? reducerValue : 0;
                table[cellNb] += adder;
            }
            if (table[cellNb] < reducerValue) table[cellNb] = 0;
            else table[cellNb] -= reducerValue;
            cellNb++;
    }   }
}

static void ZSTDN_reduceTable(U32* const table, U32 const size, U32 const reducerValue)
{
    ZSTDN_reduceTable_internal(table, size, reducerValue, 0);
}

static void ZSTDN_reduceTable_btlazy2(U32* const table, U32 const size, U32 const reducerValue)
{
    ZSTDN_reduceTable_internal(table, size, reducerValue, 1);
}

/*! ZSTDN_reduceIndex() :
*   rescale all indexes to avoid future overflow (indexes are U32) */
static void ZSTDN_reduceIndex (ZSTDN_matchState_t* ms, ZSTDN_CCtx_params const* params, const U32 reducerValue)
{
    {   U32 const hSize = (U32)1 << params->cParams.hashLog;
        ZSTDN_reduceTable(ms->hashTable, hSize, reducerValue);
    }

    if (params->cParams.strategy != ZSTDN_fast) {
        U32 const chainSize = (U32)1 << params->cParams.chainLog;
        if (params->cParams.strategy == ZSTDN_btlazy2)
            ZSTDN_reduceTable_btlazy2(ms->chainTable, chainSize, reducerValue);
        else
            ZSTDN_reduceTable(ms->chainTable, chainSize, reducerValue);
    }

    if (ms->hashLog3) {
        U32 const h3Size = (U32)1 << ms->hashLog3;
        ZSTDN_reduceTable(ms->hashTable3, h3Size, reducerValue);
    }
}


/*-*******************************************************
*  Block entropic compression
*********************************************************/

/* See doc/zstd_compression_format.md for detailed format description */

void ZSTDN_seqToCodes(const seqStore_t* seqStorePtr)
{
    const seqDef* const sequences = seqStorePtr->sequencesStart;
    BYTE* const llCodeTable = seqStorePtr->llCode;
    BYTE* const ofCodeTable = seqStorePtr->ofCode;
    BYTE* const mlCodeTable = seqStorePtr->mlCode;
    U32 const nbSeq = (U32)(seqStorePtr->sequences - seqStorePtr->sequencesStart);
    U32 u;
    assert(nbSeq <= seqStorePtr->maxNbSeq);
    for (u=0; u<nbSeq; u++) {
        U32 const llv = sequences[u].litLength;
        U32 const mlv = sequences[u].matchLength;
        llCodeTable[u] = (BYTE)ZSTDN_LLcode(llv);
        ofCodeTable[u] = (BYTE)ZSTDN_highbit32(sequences[u].offset);
        mlCodeTable[u] = (BYTE)ZSTDN_MLcode(mlv);
    }
    if (seqStorePtr->longLengthID==1)
        llCodeTable[seqStorePtr->longLengthPos] = MaxLL;
    if (seqStorePtr->longLengthID==2)
        mlCodeTable[seqStorePtr->longLengthPos] = MaxML;
}

/* ZSTDN_useTargetCBlockSize():
 * Returns if target compressed block size param is being used.
 * If used, compression will do best effort to make a compressed block size to be around targetCBlockSize.
 * Returns 1 if true, 0 otherwise. */
static int ZSTDN_useTargetCBlockSize(const ZSTDN_CCtx_params* cctxParams)
{
    DEBUGLOG(5, "ZSTDN_useTargetCBlockSize (targetCBlockSize=%zu)", cctxParams->targetCBlockSize);
    return (cctxParams->targetCBlockSize != 0);
}

/* ZSTDN_entropyCompressSequences_internal():
 * actually compresses both literals and sequences */
MEM_STATIC size_t
ZSTDN_entropyCompressSequences_internal(seqStore_t* seqStorePtr,
                          const ZSTDN_entropyCTables_t* prevEntropy,
                                ZSTDN_entropyCTables_t* nextEntropy,
                          const ZSTDN_CCtx_params* cctxParams,
                                void* dst, size_t dstCapacity,
                                void* entropyWorkspace, size_t entropyWkspSize,
                          const int bmi2)
{
    const int longOffsets = cctxParams->cParams.windowLog > STREAM_ACCUMULATOR_MIN;
    ZSTDN_strategy const strategy = cctxParams->cParams.strategy;
    unsigned* count = (unsigned*)entropyWorkspace;
    FSEN_CTable* CTable_LitLength = nextEntropy->fse.litlengthCTable;
    FSEN_CTable* CTable_OffsetBits = nextEntropy->fse.offcodeCTable;
    FSEN_CTable* CTable_MatchLength = nextEntropy->fse.matchlengthCTable;
    U32 LLtype, Offtype, MLtype;   /* compressed, raw or rle */
    const seqDef* const sequences = seqStorePtr->sequencesStart;
    const BYTE* const ofCodeTable = seqStorePtr->ofCode;
    const BYTE* const llCodeTable = seqStorePtr->llCode;
    const BYTE* const mlCodeTable = seqStorePtr->mlCode;
    BYTE* const ostart = (BYTE*)dst;
    BYTE* const oend = ostart + dstCapacity;
    BYTE* op = ostart;
    size_t const nbSeq = (size_t)(seqStorePtr->sequences - seqStorePtr->sequencesStart);
    BYTE* seqHead;
    BYTE* lastNCount = NULL;

    entropyWorkspace = count + (MaxSeq + 1);
    entropyWkspSize -= (MaxSeq + 1) * sizeof(*count);

    DEBUGLOG(4, "ZSTDN_entropyCompressSequences_internal (nbSeq=%zu)", nbSeq);
    ZSTDN_STATIC_ASSERT(HUFN_WORKSPACE_SIZE >= (1<<MAX(MLFSELog,LLFSELog)));
    assert(entropyWkspSize >= HUFN_WORKSPACE_SIZE);

    /* Compress literals */
    {   const BYTE* const literals = seqStorePtr->litStart;
        size_t const litSize = (size_t)(seqStorePtr->lit - literals);
        size_t const cSize = ZSTDN_compressLiterals(
                                    &prevEntropy->huf, &nextEntropy->huf,
                                    cctxParams->cParams.strategy,
                                    ZSTDN_disableLiteralsCompression(cctxParams),
                                    op, dstCapacity,
                                    literals, litSize,
                                    entropyWorkspace, entropyWkspSize,
                                    bmi2);
        FORWARD_IF_ERROR(cSize, "ZSTDN_compressLiterals failed");
        assert(cSize <= dstCapacity);
        op += cSize;
    }

    /* Sequences Header */
    RETURN_ERROR_IF((oend-op) < 3 /*max nbSeq Size*/ + 1 /*seqHead*/,
                    dstSize_tooSmall, "Can't fit seq hdr in output buf!");
    if (nbSeq < 128) {
        *op++ = (BYTE)nbSeq;
    } else if (nbSeq < LONGNBSEQ) {
        op[0] = (BYTE)((nbSeq>>8) + 0x80);
        op[1] = (BYTE)nbSeq;
        op+=2;
    } else {
        op[0]=0xFF;
        MEM_writeLE16(op+1, (U16)(nbSeq - LONGNBSEQ));
        op+=3;
    }
    assert(op <= oend);
    if (nbSeq==0) {
        /* Copy the old tables over as if we repeated them */
        ZSTDN_memcpy(&nextEntropy->fse, &prevEntropy->fse, sizeof(prevEntropy->fse));
        return (size_t)(op - ostart);
    }

    /* seqHead : flags for FSE encoding type */
    seqHead = op++;
    assert(op <= oend);

    /* convert length/distances into codes */
    ZSTDN_seqToCodes(seqStorePtr);
    /* build CTable for Literal Lengths */
    {   unsigned max = MaxLL;
        size_t const mostFrequent = HIST_countFast_wksp(count, &max, llCodeTable, nbSeq, entropyWorkspace, entropyWkspSize);   /* can't fail */
        DEBUGLOG(5, "Building LL table");
        nextEntropy->fse.litlength_repeatMode = prevEntropy->fse.litlength_repeatMode;
        LLtype = ZSTDN_selectEncodingType(&nextEntropy->fse.litlength_repeatMode,
                                        count, max, mostFrequent, nbSeq,
                                        LLFSELog, prevEntropy->fse.litlengthCTable,
                                        LL_defaultNorm, LL_defaultNormLog,
                                        ZSTDN_defaultAllowed, strategy);
        assert(set_basic < set_compressed && set_rle < set_compressed);
        assert(!(LLtype < set_compressed && nextEntropy->fse.litlength_repeatMode != FSEN_repeat_none)); /* We don't copy tables */
        {   size_t const countSize = ZSTDN_buildCTable(
                op, (size_t)(oend - op),
                CTable_LitLength, LLFSELog, (symbolEncodingType_e)LLtype,
                count, max, llCodeTable, nbSeq,
                LL_defaultNorm, LL_defaultNormLog, MaxLL,
                prevEntropy->fse.litlengthCTable,
                sizeof(prevEntropy->fse.litlengthCTable),
                entropyWorkspace, entropyWkspSize);
            FORWARD_IF_ERROR(countSize, "ZSTDN_buildCTable for LitLens failed");
            if (LLtype == set_compressed)
                lastNCount = op;
            op += countSize;
            assert(op <= oend);
    }   }
    /* build CTable for Offsets */
    {   unsigned max = MaxOff;
        size_t const mostFrequent = HIST_countFast_wksp(
            count, &max, ofCodeTable, nbSeq, entropyWorkspace, entropyWkspSize);  /* can't fail */
        /* We can only use the basic table if max <= DefaultMaxOff, otherwise the offsets are too large */
        ZSTDN_defaultPolicy_e const defaultPolicy = (max <= DefaultMaxOff) ? ZSTDN_defaultAllowed : ZSTDN_defaultDisallowed;
        DEBUGLOG(5, "Building OF table");
        nextEntropy->fse.offcode_repeatMode = prevEntropy->fse.offcode_repeatMode;
        Offtype = ZSTDN_selectEncodingType(&nextEntropy->fse.offcode_repeatMode,
                                        count, max, mostFrequent, nbSeq,
                                        OffFSELog, prevEntropy->fse.offcodeCTable,
                                        OF_defaultNorm, OF_defaultNormLog,
                                        defaultPolicy, strategy);
        assert(!(Offtype < set_compressed && nextEntropy->fse.offcode_repeatMode != FSEN_repeat_none)); /* We don't copy tables */
        {   size_t const countSize = ZSTDN_buildCTable(
                op, (size_t)(oend - op),
                CTable_OffsetBits, OffFSELog, (symbolEncodingType_e)Offtype,
                count, max, ofCodeTable, nbSeq,
                OF_defaultNorm, OF_defaultNormLog, DefaultMaxOff,
                prevEntropy->fse.offcodeCTable,
                sizeof(prevEntropy->fse.offcodeCTable),
                entropyWorkspace, entropyWkspSize);
            FORWARD_IF_ERROR(countSize, "ZSTDN_buildCTable for Offsets failed");
            if (Offtype == set_compressed)
                lastNCount = op;
            op += countSize;
            assert(op <= oend);
    }   }
    /* build CTable for MatchLengths */
    {   unsigned max = MaxML;
        size_t const mostFrequent = HIST_countFast_wksp(
            count, &max, mlCodeTable, nbSeq, entropyWorkspace, entropyWkspSize);   /* can't fail */
        DEBUGLOG(5, "Building ML table (remaining space : %i)", (int)(oend-op));
        nextEntropy->fse.matchlength_repeatMode = prevEntropy->fse.matchlength_repeatMode;
        MLtype = ZSTDN_selectEncodingType(&nextEntropy->fse.matchlength_repeatMode,
                                        count, max, mostFrequent, nbSeq,
                                        MLFSELog, prevEntropy->fse.matchlengthCTable,
                                        ML_defaultNorm, ML_defaultNormLog,
                                        ZSTDN_defaultAllowed, strategy);
        assert(!(MLtype < set_compressed && nextEntropy->fse.matchlength_repeatMode != FSEN_repeat_none)); /* We don't copy tables */
        {   size_t const countSize = ZSTDN_buildCTable(
                op, (size_t)(oend - op),
                CTable_MatchLength, MLFSELog, (symbolEncodingType_e)MLtype,
                count, max, mlCodeTable, nbSeq,
                ML_defaultNorm, ML_defaultNormLog, MaxML,
                prevEntropy->fse.matchlengthCTable,
                sizeof(prevEntropy->fse.matchlengthCTable),
                entropyWorkspace, entropyWkspSize);
            FORWARD_IF_ERROR(countSize, "ZSTDN_buildCTable for MatchLengths failed");
            if (MLtype == set_compressed)
                lastNCount = op;
            op += countSize;
            assert(op <= oend);
    }   }

    *seqHead = (BYTE)((LLtype<<6) + (Offtype<<4) + (MLtype<<2));

    {   size_t const bitstreamSize = ZSTDN_encodeSequences(
                                        op, (size_t)(oend - op),
                                        CTable_MatchLength, mlCodeTable,
                                        CTable_OffsetBits, ofCodeTable,
                                        CTable_LitLength, llCodeTable,
                                        sequences, nbSeq,
                                        longOffsets, bmi2);
        FORWARD_IF_ERROR(bitstreamSize, "ZSTDN_encodeSequences failed");
        op += bitstreamSize;
        assert(op <= oend);
        /* zstd versions <= 1.3.4 mistakenly report corruption when
         * FSEN_readNCount() receives a buffer < 4 bytes.
         * Fixed by https://github.com/facebook/zstd/pull/1146.
         * This can happen when the last set_compressed table present is 2
         * bytes and the bitstream is only one byte.
         * In this exceedingly rare case, we will simply emit an uncompressed
         * block, since it isn't worth optimizing.
         */
        if (lastNCount && (op - lastNCount) < 4) {
            /* NCountSize >= 2 && bitstreamSize > 0 ==> lastCountSize == 3 */
            assert(op - lastNCount == 3);
            DEBUGLOG(5, "Avoiding bug in zstd decoder in versions <= 1.3.4 by "
                        "emitting an uncompressed block.");
            return 0;
        }
    }

    DEBUGLOG(5, "compressed block size : %u", (unsigned)(op - ostart));
    return (size_t)(op - ostart);
}

MEM_STATIC size_t
ZSTDN_entropyCompressSequences(seqStore_t* seqStorePtr,
                       const ZSTDN_entropyCTables_t* prevEntropy,
                             ZSTDN_entropyCTables_t* nextEntropy,
                       const ZSTDN_CCtx_params* cctxParams,
                             void* dst, size_t dstCapacity,
                             size_t srcSize,
                             void* entropyWorkspace, size_t entropyWkspSize,
                             int bmi2)
{
    size_t const cSize = ZSTDN_entropyCompressSequences_internal(
                            seqStorePtr, prevEntropy, nextEntropy, cctxParams,
                            dst, dstCapacity,
                            entropyWorkspace, entropyWkspSize, bmi2);
    if (cSize == 0) return 0;
    /* When srcSize <= dstCapacity, there is enough space to write a raw uncompressed block.
     * Since we ran out of space, block must be not compressible, so fall back to raw uncompressed block.
     */
    if ((cSize == ERROR(dstSize_tooSmall)) & (srcSize <= dstCapacity))
        return 0;  /* block not compressed */
    FORWARD_IF_ERROR(cSize, "ZSTDN_entropyCompressSequences_internal failed");

    /* Check compressibility */
    {   size_t const maxCSize = srcSize - ZSTDN_minGain(srcSize, cctxParams->cParams.strategy);
        if (cSize >= maxCSize) return 0;  /* block not compressed */
    }
    DEBUGLOG(4, "ZSTDN_entropyCompressSequences() cSize: %zu\n", cSize);
    return cSize;
}

/* ZSTDN_selectBlockCompressor() :
 * Not static, but internal use only (used by long distance matcher)
 * assumption : strat is a valid strategy */
ZSTDN_blockCompressor ZSTDN_selectBlockCompressor(ZSTDN_strategy strat, ZSTDN_dictMode_e dictMode)
{
    static const ZSTDN_blockCompressor blockCompressor[4][ZSTDN_STRATEGY_MAX+1] = {
        { ZSTDN_compressBlock_fast  /* default for 0 */,
          ZSTDN_compressBlock_fast,
          ZSTDN_compressBlock_doubleFast,
          ZSTDN_compressBlock_greedy,
          ZSTDN_compressBlock_lazy,
          ZSTDN_compressBlock_lazy2,
          ZSTDN_compressBlock_btlazy2,
          ZSTDN_compressBlock_btopt,
          ZSTDN_compressBlock_btultra,
          ZSTDN_compressBlock_btultra2 },
        { ZSTDN_compressBlock_fast_extDict  /* default for 0 */,
          ZSTDN_compressBlock_fast_extDict,
          ZSTDN_compressBlock_doubleFast_extDict,
          ZSTDN_compressBlock_greedy_extDict,
          ZSTDN_compressBlock_lazy_extDict,
          ZSTDN_compressBlock_lazy2_extDict,
          ZSTDN_compressBlock_btlazy2_extDict,
          ZSTDN_compressBlock_btopt_extDict,
          ZSTDN_compressBlock_btultra_extDict,
          ZSTDN_compressBlock_btultra_extDict },
        { ZSTDN_compressBlock_fast_dictMatchState  /* default for 0 */,
          ZSTDN_compressBlock_fast_dictMatchState,
          ZSTDN_compressBlock_doubleFast_dictMatchState,
          ZSTDN_compressBlock_greedy_dictMatchState,
          ZSTDN_compressBlock_lazy_dictMatchState,
          ZSTDN_compressBlock_lazy2_dictMatchState,
          ZSTDN_compressBlock_btlazy2_dictMatchState,
          ZSTDN_compressBlock_btopt_dictMatchState,
          ZSTDN_compressBlock_btultra_dictMatchState,
          ZSTDN_compressBlock_btultra_dictMatchState },
        { NULL  /* default for 0 */,
          NULL,
          NULL,
          ZSTDN_compressBlock_greedy_dedicatedDictSearch,
          ZSTDN_compressBlock_lazy_dedicatedDictSearch,
          ZSTDN_compressBlock_lazy2_dedicatedDictSearch,
          NULL,
          NULL,
          NULL,
          NULL }
    };
    ZSTDN_blockCompressor selectedCompressor;
    ZSTDN_STATIC_ASSERT((unsigned)ZSTDN_fast == 1);

    assert(ZSTDN_cParam_withinBounds(ZSTDN_c_strategy, strat));
    selectedCompressor = blockCompressor[(int)dictMode][(int)strat];
    assert(selectedCompressor != NULL);
    return selectedCompressor;
}

static void ZSTDN_storeLastLiterals(seqStore_t* seqStorePtr,
                                   const BYTE* anchor, size_t lastLLSize)
{
    ZSTDN_memcpy(seqStorePtr->lit, anchor, lastLLSize);
    seqStorePtr->lit += lastLLSize;
}

void ZSTDN_resetSeqStore(seqStore_t* ssPtr)
{
    ssPtr->lit = ssPtr->litStart;
    ssPtr->sequences = ssPtr->sequencesStart;
    ssPtr->longLengthID = 0;
}

typedef enum { ZSTDbss_compress, ZSTDbss_noCompress } ZSTDN_buildSeqStore_e;

static size_t ZSTDN_buildSeqStore(ZSTDN_CCtx* zc, const void* src, size_t srcSize)
{
    ZSTDN_matchState_t* const ms = &zc->blockState.matchState;
    DEBUGLOG(5, "ZSTDN_buildSeqStore (srcSize=%zu)", srcSize);
    assert(srcSize <= ZSTDN_BLOCKSIZE_MAX);
    /* Assert that we have correctly flushed the ctx params into the ms's copy */
    ZSTDN_assertEqualCParams(zc->appliedParams.cParams, ms->cParams);
    if (srcSize < MIN_CBLOCK_SIZE+ZSTDN_blockHeaderSize+1) {
        if (zc->appliedParams.cParams.strategy >= ZSTDN_btopt) {
            ZSTDN_ldm_skipRawSeqStoreBytes(&zc->externSeqStore, srcSize);
        } else {
            ZSTDN_ldm_skipSequences(&zc->externSeqStore, srcSize, zc->appliedParams.cParams.minMatch);
        }
        return ZSTDbss_noCompress; /* don't even attempt compression below a certain srcSize */
    }
    ZSTDN_resetSeqStore(&(zc->seqStore));
    /* required for optimal parser to read stats from dictionary */
    ms->opt.symbolCosts = &zc->blockState.prevCBlock->entropy;
    /* tell the optimal parser how we expect to compress literals */
    ms->opt.literalCompressionMode = zc->appliedParams.literalCompressionMode;
    /* a gap between an attached dict and the current window is not safe,
     * they must remain adjacent,
     * and when that stops being the case, the dict must be unset */
    assert(ms->dictMatchState == NULL || ms->loadedDictEnd == ms->window.dictLimit);

    /* limited update after a very long match */
    {   const BYTE* const base = ms->window.base;
        const BYTE* const istart = (const BYTE*)src;
        const U32 curr = (U32)(istart-base);
        if (sizeof(ptrdiff_t)==8) assert(istart - base < (ptrdiff_t)(U32)(-1));   /* ensure no overflow */
        if (curr > ms->nextToUpdate + 384)
            ms->nextToUpdate = curr - MIN(192, (U32)(curr - ms->nextToUpdate - 384));
    }

    /* select and store sequences */
    {   ZSTDN_dictMode_e const dictMode = ZSTDN_matchState_dictMode(ms);
        size_t lastLLSize;
        {   int i;
            for (i = 0; i < ZSTDN_REP_NUM; ++i)
                zc->blockState.nextCBlock->rep[i] = zc->blockState.prevCBlock->rep[i];
        }
        if (zc->externSeqStore.pos < zc->externSeqStore.size) {
            assert(!zc->appliedParams.ldmParams.enableLdm);
            /* Updates ldmSeqStore.pos */
            lastLLSize =
                ZSTDN_ldm_blockCompress(&zc->externSeqStore,
                                       ms, &zc->seqStore,
                                       zc->blockState.nextCBlock->rep,
                                       src, srcSize);
            assert(zc->externSeqStore.pos <= zc->externSeqStore.size);
        } else if (zc->appliedParams.ldmParams.enableLdm) {
            rawSeqStore_t ldmSeqStore = kNullRawSeqStore;

            ldmSeqStore.seq = zc->ldmSequences;
            ldmSeqStore.capacity = zc->maxNbLdmSequences;
            /* Updates ldmSeqStore.size */
            FORWARD_IF_ERROR(ZSTDN_ldm_generateSequences(&zc->ldmState, &ldmSeqStore,
                                               &zc->appliedParams.ldmParams,
                                               src, srcSize), "");
            /* Updates ldmSeqStore.pos */
            lastLLSize =
                ZSTDN_ldm_blockCompress(&ldmSeqStore,
                                       ms, &zc->seqStore,
                                       zc->blockState.nextCBlock->rep,
                                       src, srcSize);
            assert(ldmSeqStore.pos == ldmSeqStore.size);
        } else {   /* not long range mode */
            ZSTDN_blockCompressor const blockCompressor = ZSTDN_selectBlockCompressor(zc->appliedParams.cParams.strategy, dictMode);
            ms->ldmSeqStore = NULL;
            lastLLSize = blockCompressor(ms, &zc->seqStore, zc->blockState.nextCBlock->rep, src, srcSize);
        }
        {   const BYTE* const lastLiterals = (const BYTE*)src + srcSize - lastLLSize;
            ZSTDN_storeLastLiterals(&zc->seqStore, lastLiterals, lastLLSize);
    }   }
    return ZSTDbss_compress;
}

static void ZSTDN_copyBlockSequences(ZSTDN_CCtx* zc)
{
    const seqStore_t* seqStore = ZSTDN_getSeqStore(zc);
    const seqDef* seqStoreSeqs = seqStore->sequencesStart;
    size_t seqStoreSeqSize = seqStore->sequences - seqStoreSeqs;
    size_t seqStoreLiteralsSize = (size_t)(seqStore->lit - seqStore->litStart);
    size_t literalsRead = 0;
    size_t lastLLSize;

    ZSTDN_Sequence* outSeqs = &zc->seqCollector.seqStart[zc->seqCollector.seqIndex];
    size_t i;
    repcodes_t updatedRepcodes;

    assert(zc->seqCollector.seqIndex + 1 < zc->seqCollector.maxSequences);
    /* Ensure we have enough space for last literals "sequence" */
    assert(zc->seqCollector.maxSequences >= seqStoreSeqSize + 1);
    ZSTDN_memcpy(updatedRepcodes.rep, zc->blockState.prevCBlock->rep, sizeof(repcodes_t));
    for (i = 0; i < seqStoreSeqSize; ++i) {
        U32 rawOffset = seqStoreSeqs[i].offset - ZSTDN_REP_NUM;
        outSeqs[i].litLength = seqStoreSeqs[i].litLength;
        outSeqs[i].matchLength = seqStoreSeqs[i].matchLength + MINMATCH;
        outSeqs[i].rep = 0;

        if (i == seqStore->longLengthPos) {
            if (seqStore->longLengthID == 1) {
                outSeqs[i].litLength += 0x10000;
            } else if (seqStore->longLengthID == 2) {
                outSeqs[i].matchLength += 0x10000;
            }
        }

        if (seqStoreSeqs[i].offset <= ZSTDN_REP_NUM) {
            /* Derive the correct offset corresponding to a repcode */
            outSeqs[i].rep = seqStoreSeqs[i].offset;
            if (outSeqs[i].litLength != 0) {
                rawOffset = updatedRepcodes.rep[outSeqs[i].rep - 1];
            } else {
                if (outSeqs[i].rep == 3) {
                    rawOffset = updatedRepcodes.rep[0] - 1;
                } else {
                    rawOffset = updatedRepcodes.rep[outSeqs[i].rep];
                }
            }
        }
        outSeqs[i].offset = rawOffset;
        /* seqStoreSeqs[i].offset == offCode+1, and ZSTDN_updateRep() expects offCode
           so we provide seqStoreSeqs[i].offset - 1 */
        updatedRepcodes = ZSTDN_updateRep(updatedRepcodes.rep,
                                         seqStoreSeqs[i].offset - 1,
                                         seqStoreSeqs[i].litLength == 0);
        literalsRead += outSeqs[i].litLength;
    }
    /* Insert last literals (if any exist) in the block as a sequence with ml == off == 0.
     * If there are no last literals, then we'll emit (of: 0, ml: 0, ll: 0), which is a marker
     * for the block boundary, according to the API.
     */
    assert(seqStoreLiteralsSize >= literalsRead);
    lastLLSize = seqStoreLiteralsSize - literalsRead;
    outSeqs[i].litLength = (U32)lastLLSize;
    outSeqs[i].matchLength = outSeqs[i].offset = outSeqs[i].rep = 0;
    seqStoreSeqSize++;
    zc->seqCollector.seqIndex += seqStoreSeqSize;
}

size_t ZSTDN_generateSequences(ZSTDN_CCtx* zc, ZSTDN_Sequence* outSeqs,
                              size_t outSeqsSize, const void* src, size_t srcSize)
{
    const size_t dstCapacity = ZSTDN_compressBound(srcSize);
    void* dst = ZSTDN_customMalloc(dstCapacity, ZSTDN_defaultCMem);
    SeqCollector seqCollector;

    RETURN_ERROR_IF(dst == NULL, memory_allocation, "NULL pointer!");

    seqCollector.collectSequences = 1;
    seqCollector.seqStart = outSeqs;
    seqCollector.seqIndex = 0;
    seqCollector.maxSequences = outSeqsSize;
    zc->seqCollector = seqCollector;

    ZSTDN_compress2(zc, dst, dstCapacity, src, srcSize);
    ZSTDN_customFree(dst, ZSTDN_defaultCMem);
    return zc->seqCollector.seqIndex;
}

size_t ZSTDN_mergeBlockDelimiters(ZSTDN_Sequence* sequences, size_t seqsSize) {
    size_t in = 0;
    size_t out = 0;
    for (; in < seqsSize; ++in) {
        if (sequences[in].offset == 0 && sequences[in].matchLength == 0) {
            if (in != seqsSize - 1) {
                sequences[in+1].litLength += sequences[in].litLength;
            }
        } else {
            sequences[out] = sequences[in];
            ++out;
        }
    }
    return out;
}

/* Unrolled loop to read four size_ts of input at a time. Returns 1 if is RLE, 0 if not. */
static int ZSTDN_isRLE(const BYTE* src, size_t length) {
    const BYTE* ip = src;
    const BYTE value = ip[0];
    const size_t valueST = (size_t)((U64)value * 0x0101010101010101ULL);
    const size_t unrollSize = sizeof(size_t) * 4;
    const size_t unrollMask = unrollSize - 1;
    const size_t prefixLength = length & unrollMask;
    size_t i;
    size_t u;
    if (length == 1) return 1;
    /* Check if prefix is RLE first before using unrolled loop */
    if (prefixLength && ZSTDN_count(ip+1, ip, ip+prefixLength) != prefixLength-1) {
        return 0;
    }
    for (i = prefixLength; i != length; i += unrollSize) {
        for (u = 0; u < unrollSize; u += sizeof(size_t)) {
            if (MEM_readST(ip + i + u) != valueST) {
                return 0;
            }
        }
    }
    return 1;
}

/* Returns true if the given block may be RLE.
 * This is just a heuristic based on the compressibility.
 * It may return both false positives and false negatives.
 */
static int ZSTDN_maybeRLE(seqStore_t const* seqStore)
{
    size_t const nbSeqs = (size_t)(seqStore->sequences - seqStore->sequencesStart);
    size_t const nbLits = (size_t)(seqStore->lit - seqStore->litStart);

    return nbSeqs < 4 && nbLits < 10;
}

static void ZSTDN_confirmRepcodesAndEntropyTables(ZSTDN_CCtx* zc)
{
    ZSTDN_compressedBlockState_t* const tmp = zc->blockState.prevCBlock;
    zc->blockState.prevCBlock = zc->blockState.nextCBlock;
    zc->blockState.nextCBlock = tmp;
}

static size_t ZSTDN_compressBlock_internal(ZSTDN_CCtx* zc,
                                        void* dst, size_t dstCapacity,
                                        const void* src, size_t srcSize, U32 frame)
{
    /* This the upper bound for the length of an rle block.
     * This isn't the actual upper bound. Finding the real threshold
     * needs further investigation.
     */
    const U32 rleMaxLength = 25;
    size_t cSize;
    const BYTE* ip = (const BYTE*)src;
    BYTE* op = (BYTE*)dst;
    DEBUGLOG(5, "ZSTDN_compressBlock_internal (dstCapacity=%u, dictLimit=%u, nextToUpdate=%u)",
                (unsigned)dstCapacity, (unsigned)zc->blockState.matchState.window.dictLimit,
                (unsigned)zc->blockState.matchState.nextToUpdate);

    {   const size_t bss = ZSTDN_buildSeqStore(zc, src, srcSize);
        FORWARD_IF_ERROR(bss, "ZSTDN_buildSeqStore failed");
        if (bss == ZSTDbss_noCompress) { cSize = 0; goto out; }
    }

    if (zc->seqCollector.collectSequences) {
        ZSTDN_copyBlockSequences(zc);
        ZSTDN_confirmRepcodesAndEntropyTables(zc);
        return 0;
    }

    /* encode sequences and literals */
    cSize = ZSTDN_entropyCompressSequences(&zc->seqStore,
            &zc->blockState.prevCBlock->entropy, &zc->blockState.nextCBlock->entropy,
            &zc->appliedParams,
            dst, dstCapacity,
            srcSize,
            zc->entropyWorkspace, ENTROPY_WORKSPACE_SIZE /* statically allocated in resetCCtx */,
            zc->bmi2);

    if (zc->seqCollector.collectSequences) {
        ZSTDN_copyBlockSequences(zc);
        return 0;
    }


    if (frame &&
        /* We don't want to emit our first block as a RLE even if it qualifies because
         * doing so will cause the decoder (cli only) to throw a "should consume all input error."
         * This is only an issue for zstd <= v1.4.3
         */
        !zc->isFirstBlock &&
        cSize < rleMaxLength &&
        ZSTDN_isRLE(ip, srcSize))
    {
        cSize = 1;
        op[0] = ip[0];
    }

out:
    if (!ZSTDN_isError(cSize) && cSize > 1) {
        ZSTDN_confirmRepcodesAndEntropyTables(zc);
    }
    /* We check that dictionaries have offset codes available for the first
     * block. After the first block, the offcode table might not have large
     * enough codes to represent the offsets in the data.
     */
    if (zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode == FSEN_repeat_valid)
        zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode = FSEN_repeat_check;

    return cSize;
}

static size_t ZSTDN_compressBlock_targetCBlockSize_body(ZSTDN_CCtx* zc,
                               void* dst, size_t dstCapacity,
                               const void* src, size_t srcSize,
                               const size_t bss, U32 lastBlock)
{
    DEBUGLOG(6, "Attempting ZSTDN_compressSuperBlock()");
    if (bss == ZSTDbss_compress) {
        if (/* We don't want to emit our first block as a RLE even if it qualifies because
            * doing so will cause the decoder (cli only) to throw a "should consume all input error."
            * This is only an issue for zstd <= v1.4.3
            */
            !zc->isFirstBlock &&
            ZSTDN_maybeRLE(&zc->seqStore) &&
            ZSTDN_isRLE((BYTE const*)src, srcSize))
        {
            return ZSTDN_rleCompressBlock(dst, dstCapacity, *(BYTE const*)src, srcSize, lastBlock);
        }
        /* Attempt superblock compression.
         *
         * Note that compressed size of ZSTDN_compressSuperBlock() is not bound by the
         * standard ZSTDN_compressBound(). This is a problem, because even if we have
         * space now, taking an extra byte now could cause us to run out of space later
         * and violate ZSTDN_compressBound().
         *
         * Define blockBound(blockSize) = blockSize + ZSTDN_blockHeaderSize.
         *
         * In order to respect ZSTDN_compressBound() we must attempt to emit a raw
         * uncompressed block in these cases:
         *   * cSize == 0: Return code for an uncompressed block.
         *   * cSize == dstSize_tooSmall: We may have expanded beyond blockBound(srcSize).
         *     ZSTDN_noCompressBlock() will return dstSize_tooSmall if we are really out of
         *     output space.
         *   * cSize >= blockBound(srcSize): We have expanded the block too much so
         *     emit an uncompressed block.
         */
        {
            size_t const cSize = ZSTDN_compressSuperBlock(zc, dst, dstCapacity, src, srcSize, lastBlock);
            if (cSize != ERROR(dstSize_tooSmall)) {
                size_t const maxCSize = srcSize - ZSTDN_minGain(srcSize, zc->appliedParams.cParams.strategy);
                FORWARD_IF_ERROR(cSize, "ZSTDN_compressSuperBlock failed");
                if (cSize != 0 && cSize < maxCSize + ZSTDN_blockHeaderSize) {
                    ZSTDN_confirmRepcodesAndEntropyTables(zc);
                    return cSize;
                }
            }
        }
    }

    DEBUGLOG(6, "Resorting to ZSTDN_noCompressBlock()");
    /* Superblock compression failed, attempt to emit a single no compress block.
     * The decoder will be able to stream this block since it is uncompressed.
     */
    return ZSTDN_noCompressBlock(dst, dstCapacity, src, srcSize, lastBlock);
}

static size_t ZSTDN_compressBlock_targetCBlockSize(ZSTDN_CCtx* zc,
                               void* dst, size_t dstCapacity,
                               const void* src, size_t srcSize,
                               U32 lastBlock)
{
    size_t cSize = 0;
    const size_t bss = ZSTDN_buildSeqStore(zc, src, srcSize);
    DEBUGLOG(5, "ZSTDN_compressBlock_targetCBlockSize (dstCapacity=%u, dictLimit=%u, nextToUpdate=%u, srcSize=%zu)",
                (unsigned)dstCapacity, (unsigned)zc->blockState.matchState.window.dictLimit, (unsigned)zc->blockState.matchState.nextToUpdate, srcSize);
    FORWARD_IF_ERROR(bss, "ZSTDN_buildSeqStore failed");

    cSize = ZSTDN_compressBlock_targetCBlockSize_body(zc, dst, dstCapacity, src, srcSize, bss, lastBlock);
    FORWARD_IF_ERROR(cSize, "ZSTDN_compressBlock_targetCBlockSize_body failed");

    if (zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode == FSEN_repeat_valid)
        zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode = FSEN_repeat_check;

    return cSize;
}

static void ZSTDN_overflowCorrectIfNeeded(ZSTDN_matchState_t* ms,
                                         ZSTDN_cwksp* ws,
                                         ZSTDN_CCtx_params const* params,
                                         void const* ip,
                                         void const* iend)
{
    if (ZSTDN_window_needOverflowCorrection(ms->window, iend)) {
        U32 const maxDist = (U32)1 << params->cParams.windowLog;
        U32 const cycleLog = ZSTDN_cycleLog(params->cParams.chainLog, params->cParams.strategy);
        U32 const correction = ZSTDN_window_correctOverflow(&ms->window, cycleLog, maxDist, ip);
        ZSTDN_STATIC_ASSERT(ZSTDN_CHAINLOG_MAX <= 30);
        ZSTDN_STATIC_ASSERT(ZSTDN_WINDOWLOG_MAX_32 <= 30);
        ZSTDN_STATIC_ASSERT(ZSTDN_WINDOWLOG_MAX <= 31);
        ZSTDN_cwksp_mark_tables_dirty(ws);
        ZSTDN_reduceIndex(ms, params, correction);
        ZSTDN_cwksp_mark_tables_clean(ws);
        if (ms->nextToUpdate < correction) ms->nextToUpdate = 0;
        else ms->nextToUpdate -= correction;
        /* invalidate dictionaries on overflow correction */
        ms->loadedDictEnd = 0;
        ms->dictMatchState = NULL;
    }
}

/*! ZSTDN_compress_frameChunk() :
*   Compress a chunk of data into one or multiple blocks.
*   All blocks will be terminated, all input will be consumed.
*   Function will issue an error if there is not enough `dstCapacity` to hold the compressed content.
*   Frame is supposed already started (header already produced)
*   @return : compressed size, or an error code
*/
static size_t ZSTDN_compress_frameChunk (ZSTDN_CCtx* cctx,
                                     void* dst, size_t dstCapacity,
                               const void* src, size_t srcSize,
                                     U32 lastFrameChunk)
{
    size_t blockSize = cctx->blockSize;
    size_t remaining = srcSize;
    const BYTE* ip = (const BYTE*)src;
    BYTE* const ostart = (BYTE*)dst;
    BYTE* op = ostart;
    U32 const maxDist = (U32)1 << cctx->appliedParams.cParams.windowLog;

    assert(cctx->appliedParams.cParams.windowLog <= ZSTDN_WINDOWLOG_MAX);

    DEBUGLOG(4, "ZSTDN_compress_frameChunk (blockSize=%u)", (unsigned)blockSize);
    if (cctx->appliedParams.fParams.checksumFlag && srcSize)
        xxh64n_update(&cctx->xxhState, src, srcSize);

    while (remaining) {
        ZSTDN_matchState_t* const ms = &cctx->blockState.matchState;
        U32 const lastBlock = lastFrameChunk & (blockSize >= remaining);

        RETURN_ERROR_IF(dstCapacity < ZSTDN_blockHeaderSize + MIN_CBLOCK_SIZE,
                        dstSize_tooSmall,
                        "not enough space to store compressed block");
        if (remaining < blockSize) blockSize = remaining;

        ZSTDN_overflowCorrectIfNeeded(
            ms, &cctx->workspace, &cctx->appliedParams, ip, ip + blockSize);
        ZSTDN_checkDictValidity(&ms->window, ip + blockSize, maxDist, &ms->loadedDictEnd, &ms->dictMatchState);

        /* Ensure hash/chain table insertion resumes no sooner than lowlimit */
        if (ms->nextToUpdate < ms->window.lowLimit) ms->nextToUpdate = ms->window.lowLimit;

        {   size_t cSize;
            if (ZSTDN_useTargetCBlockSize(&cctx->appliedParams)) {
                cSize = ZSTDN_compressBlock_targetCBlockSize(cctx, op, dstCapacity, ip, blockSize, lastBlock);
                FORWARD_IF_ERROR(cSize, "ZSTDN_compressBlock_targetCBlockSize failed");
                assert(cSize > 0);
                assert(cSize <= blockSize + ZSTDN_blockHeaderSize);
            } else {
                cSize = ZSTDN_compressBlock_internal(cctx,
                                        op+ZSTDN_blockHeaderSize, dstCapacity-ZSTDN_blockHeaderSize,
                                        ip, blockSize, 1 /* frame */);
                FORWARD_IF_ERROR(cSize, "ZSTDN_compressBlock_internal failed");

                if (cSize == 0) {  /* block is not compressible */
                    cSize = ZSTDN_noCompressBlock(op, dstCapacity, ip, blockSize, lastBlock);
                    FORWARD_IF_ERROR(cSize, "ZSTDN_noCompressBlock failed");
                } else {
                    U32 const cBlockHeader = cSize == 1 ?
                        lastBlock + (((U32)bt_rle)<<1) + (U32)(blockSize << 3) :
                        lastBlock + (((U32)bt_compressed)<<1) + (U32)(cSize << 3);
                    MEM_writeLE24(op, cBlockHeader);
                    cSize += ZSTDN_blockHeaderSize;
                }
            }


            ip += blockSize;
            assert(remaining >= blockSize);
            remaining -= blockSize;
            op += cSize;
            assert(dstCapacity >= cSize);
            dstCapacity -= cSize;
            cctx->isFirstBlock = 0;
            DEBUGLOG(5, "ZSTDN_compress_frameChunk: adding a block of size %u",
                        (unsigned)cSize);
    }   }

    if (lastFrameChunk && (op>ostart)) cctx->stage = ZSTDcs_ending;
    return (size_t)(op-ostart);
}


static size_t ZSTDN_writeFrameHeader(void* dst, size_t dstCapacity,
                                    const ZSTDN_CCtx_params* params, U64 pledgedSrcSize, U32 dictID)
{   BYTE* const op = (BYTE*)dst;
    U32   const dictIDSizeCodeLength = (dictID>0) + (dictID>=256) + (dictID>=65536);   /* 0-3 */
    U32   const dictIDSizeCode = params->fParams.noDictIDFlag ? 0 : dictIDSizeCodeLength;   /* 0-3 */
    U32   const checksumFlag = params->fParams.checksumFlag>0;
    U32   const windowSize = (U32)1 << params->cParams.windowLog;
    U32   const singleSegment = params->fParams.contentSizeFlag && (windowSize >= pledgedSrcSize);
    BYTE  const windowLogByte = (BYTE)((params->cParams.windowLog - ZSTDN_WINDOWLOG_ABSOLUTEMIN) << 3);
    U32   const fcsCode = params->fParams.contentSizeFlag ?
                     (pledgedSrcSize>=256) + (pledgedSrcSize>=65536+256) + (pledgedSrcSize>=0xFFFFFFFFU) : 0;  /* 0-3 */
    BYTE  const frameHeaderDescriptionByte = (BYTE)(dictIDSizeCode + (checksumFlag<<2) + (singleSegment<<5) + (fcsCode<<6) );
    size_t pos=0;

    assert(!(params->fParams.contentSizeFlag && pledgedSrcSize == ZSTDN_CONTENTSIZE_UNKNOWN));
    RETURN_ERROR_IF(dstCapacity < ZSTDN_FRAMEHEADERSIZE_MAX, dstSize_tooSmall,
                    "dst buf is too small to fit worst-case frame header size.");
    DEBUGLOG(4, "ZSTDN_writeFrameHeader : dictIDFlag : %u ; dictID : %u ; dictIDSizeCode : %u",
                !params->fParams.noDictIDFlag, (unsigned)dictID, (unsigned)dictIDSizeCode);
    if (params->format == ZSTDN_f_zstd1) {
        MEM_writeLE32(dst, ZSTDN_MAGICNUMBER);
        pos = 4;
    }
    op[pos++] = frameHeaderDescriptionByte;
    if (!singleSegment) op[pos++] = windowLogByte;
    switch(dictIDSizeCode)
    {
        default:
            assert(0); /* impossible */
            ZSTDN_FALLTHROUGH;
        case 0 : break;
        case 1 : op[pos] = (BYTE)(dictID); pos++; break;
        case 2 : MEM_writeLE16(op+pos, (U16)dictID); pos+=2; break;
        case 3 : MEM_writeLE32(op+pos, dictID); pos+=4; break;
    }
    switch(fcsCode)
    {
        default:
            assert(0); /* impossible */
            ZSTDN_FALLTHROUGH;
        case 0 : if (singleSegment) op[pos++] = (BYTE)(pledgedSrcSize); break;
        case 1 : MEM_writeLE16(op+pos, (U16)(pledgedSrcSize-256)); pos+=2; break;
        case 2 : MEM_writeLE32(op+pos, (U32)(pledgedSrcSize)); pos+=4; break;
        case 3 : MEM_writeLE64(op+pos, (U64)(pledgedSrcSize)); pos+=8; break;
    }
    return pos;
}

/* ZSTDN_writeSkippableFrame_advanced() :
 * Writes out a skippable frame with the specified magic number variant (16 are supported),
 * from ZSTDN_MAGIC_SKIPPABLE_START to ZSTDN_MAGIC_SKIPPABLE_START+15, and the desired source data.
 *
 * Returns the total number of bytes written, or a ZSTD error code.
 */
size_t ZSTDN_writeSkippableFrame(void* dst, size_t dstCapacity,
                                const void* src, size_t srcSize, unsigned magicVariant) {
    BYTE* op = (BYTE*)dst;
    RETURN_ERROR_IF(dstCapacity < srcSize + ZSTDN_SKIPPABLEHEADERSIZE /* Skippable frame overhead */,
                    dstSize_tooSmall, "Not enough room for skippable frame");
    RETURN_ERROR_IF(srcSize > (unsigned)0xFFFFFFFF, srcSize_wrong, "Src size too large for skippable frame");
    RETURN_ERROR_IF(magicVariant > 15, parameter_outOfBound, "Skippable frame magic number variant not supported");

    MEM_writeLE32(op, (U32)(ZSTDN_MAGIC_SKIPPABLE_START + magicVariant));
    MEM_writeLE32(op+4, (U32)srcSize);
    ZSTDN_memcpy(op+8, src, srcSize);
    return srcSize + ZSTDN_SKIPPABLEHEADERSIZE;
}

/* ZSTDN_writeLastEmptyBlock() :
 * output an empty Block with end-of-frame mark to complete a frame
 * @return : size of data written into `dst` (== ZSTDN_blockHeaderSize (defined in zstd_internal.h))
 *           or an error code if `dstCapacity` is too small (<ZSTDN_blockHeaderSize)
 */
size_t ZSTDN_writeLastEmptyBlock(void* dst, size_t dstCapacity)
{
    RETURN_ERROR_IF(dstCapacity < ZSTDN_blockHeaderSize, dstSize_tooSmall,
                    "dst buf is too small to write frame trailer empty block.");
    {   U32 const cBlockHeader24 = 1 /*lastBlock*/ + (((U32)bt_raw)<<1);  /* 0 size */
        MEM_writeLE24(dst, cBlockHeader24);
        return ZSTDN_blockHeaderSize;
    }
}

size_t ZSTDN_referenceExternalSequences(ZSTDN_CCtx* cctx, rawSeq* seq, size_t nbSeq)
{
    RETURN_ERROR_IF(cctx->stage != ZSTDcs_init, stage_wrong,
                    "wrong cctx stage");
    RETURN_ERROR_IF(cctx->appliedParams.ldmParams.enableLdm,
                    parameter_unsupported,
                    "incompatible with ldm");
    cctx->externSeqStore.seq = seq;
    cctx->externSeqStore.size = nbSeq;
    cctx->externSeqStore.capacity = nbSeq;
    cctx->externSeqStore.pos = 0;
    cctx->externSeqStore.posInSequence = 0;
    return 0;
}


static size_t ZSTDN_compressContinue_internal (ZSTDN_CCtx* cctx,
                              void* dst, size_t dstCapacity,
                        const void* src, size_t srcSize,
                               U32 frame, U32 lastFrameChunk)
{
    ZSTDN_matchState_t* const ms = &cctx->blockState.matchState;
    size_t fhSize = 0;

    DEBUGLOG(5, "ZSTDN_compressContinue_internal, stage: %u, srcSize: %u",
                cctx->stage, (unsigned)srcSize);
    RETURN_ERROR_IF(cctx->stage==ZSTDcs_created, stage_wrong,
                    "missing init (ZSTDN_compressBegin)");

    if (frame && (cctx->stage==ZSTDcs_init)) {
        fhSize = ZSTDN_writeFrameHeader(dst, dstCapacity, &cctx->appliedParams,
                                       cctx->pledgedSrcSizePlusOne-1, cctx->dictID);
        FORWARD_IF_ERROR(fhSize, "ZSTDN_writeFrameHeader failed");
        assert(fhSize <= dstCapacity);
        dstCapacity -= fhSize;
        dst = (char*)dst + fhSize;
        cctx->stage = ZSTDcs_ongoing;
    }

    if (!srcSize) return fhSize;  /* do not generate an empty block if no input */

    if (!ZSTDN_window_update(&ms->window, src, srcSize)) {
        ms->nextToUpdate = ms->window.dictLimit;
    }
    if (cctx->appliedParams.ldmParams.enableLdm) {
        ZSTDN_window_update(&cctx->ldmState.window, src, srcSize);
    }

    if (!frame) {
        /* overflow check and correction for block mode */
        ZSTDN_overflowCorrectIfNeeded(
            ms, &cctx->workspace, &cctx->appliedParams,
            src, (BYTE const*)src + srcSize);
    }

    DEBUGLOG(5, "ZSTDN_compressContinue_internal (blockSize=%u)", (unsigned)cctx->blockSize);
    {   size_t const cSize = frame ?
                             ZSTDN_compress_frameChunk (cctx, dst, dstCapacity, src, srcSize, lastFrameChunk) :
                             ZSTDN_compressBlock_internal (cctx, dst, dstCapacity, src, srcSize, 0 /* frame */);
        FORWARD_IF_ERROR(cSize, "%s", frame ? "ZSTDN_compress_frameChunk failed" : "ZSTDN_compressBlock_internal failed");
        cctx->consumedSrcSize += srcSize;
        cctx->producedCSize += (cSize + fhSize);
        assert(!(cctx->appliedParams.fParams.contentSizeFlag && cctx->pledgedSrcSizePlusOne == 0));
        if (cctx->pledgedSrcSizePlusOne != 0) {  /* control src size */
            ZSTDN_STATIC_ASSERT(ZSTDN_CONTENTSIZE_UNKNOWN == (unsigned long long)-1);
            RETURN_ERROR_IF(
                cctx->consumedSrcSize+1 > cctx->pledgedSrcSizePlusOne,
                srcSize_wrong,
                "error : pledgedSrcSize = %u, while realSrcSize >= %u",
                (unsigned)cctx->pledgedSrcSizePlusOne-1,
                (unsigned)cctx->consumedSrcSize);
        }
        return cSize + fhSize;
    }
}

size_t ZSTDN_compressContinue (ZSTDN_CCtx* cctx,
                              void* dst, size_t dstCapacity,
                        const void* src, size_t srcSize)
{
    DEBUGLOG(5, "ZSTDN_compressContinue (srcSize=%u)", (unsigned)srcSize);
    return ZSTDN_compressContinue_internal(cctx, dst, dstCapacity, src, srcSize, 1 /* frame mode */, 0 /* last chunk */);
}


size_t ZSTDN_getBlockSize(const ZSTDN_CCtx* cctx)
{
    ZSTDN_compressionParameters const cParams = cctx->appliedParams.cParams;
    assert(!ZSTDN_checkCParams(cParams));
    return MIN (ZSTDN_BLOCKSIZE_MAX, (U32)1 << cParams.windowLog);
}

size_t ZSTDN_compressBlock(ZSTDN_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize)
{
    DEBUGLOG(5, "ZSTDN_compressBlock: srcSize = %u", (unsigned)srcSize);
    { size_t const blockSizeMax = ZSTDN_getBlockSize(cctx);
      RETURN_ERROR_IF(srcSize > blockSizeMax, srcSize_wrong, "input is larger than a block"); }

    return ZSTDN_compressContinue_internal(cctx, dst, dstCapacity, src, srcSize, 0 /* frame mode */, 0 /* last chunk */);
}

/*! ZSTDN_loadDictionaryContent() :
 *  @return : 0, or an error code
 */
static size_t ZSTDN_loadDictionaryContent(ZSTDN_matchState_t* ms,
                                         ldmState_t* ls,
                                         ZSTDN_cwksp* ws,
                                         ZSTDN_CCtx_params const* params,
                                         const void* src, size_t srcSize,
                                         ZSTDN_dictTableLoadMethod_e dtlm)
{
    const BYTE* ip = (const BYTE*) src;
    const BYTE* const iend = ip + srcSize;

    ZSTDN_window_update(&ms->window, src, srcSize);
    ms->loadedDictEnd = params->forceWindow ? 0 : (U32)(iend - ms->window.base);

    if (params->ldmParams.enableLdm && ls != NULL) {
        ZSTDN_window_update(&ls->window, src, srcSize);
        ls->loadedDictEnd = params->forceWindow ? 0 : (U32)(iend - ls->window.base);
    }

    /* Assert that we the ms params match the params we're being given */
    ZSTDN_assertEqualCParams(params->cParams, ms->cParams);

    if (srcSize <= HASH_READ_SIZE) return 0;

    while (iend - ip > HASH_READ_SIZE) {
        size_t const remaining = (size_t)(iend - ip);
        size_t const chunk = MIN(remaining, ZSTDN_CHUNKSIZE_MAX);
        const BYTE* const ichunk = ip + chunk;

        ZSTDN_overflowCorrectIfNeeded(ms, ws, params, ip, ichunk);

        if (params->ldmParams.enableLdm && ls != NULL)
            ZSTDN_ldm_fillHashTable(ls, (const BYTE*)src, (const BYTE*)src + srcSize, &params->ldmParams);

        switch(params->cParams.strategy)
        {
        case ZSTDN_fast:
            ZSTDN_fillHashTable(ms, ichunk, dtlm);
            break;
        case ZSTDN_dfast:
            ZSTDN_fillDoubleHashTable(ms, ichunk, dtlm);
            break;

        case ZSTDN_greedy:
        case ZSTDN_lazy:
        case ZSTDN_lazy2:
            if (chunk >= HASH_READ_SIZE && ms->dedicatedDictSearch) {
                assert(chunk == remaining); /* must load everything in one go */
                ZSTDN_dedicatedDictSearch_lazy_loadDictionary(ms, ichunk-HASH_READ_SIZE);
            } else if (chunk >= HASH_READ_SIZE) {
                ZSTDN_insertAndFindFirstIndex(ms, ichunk-HASH_READ_SIZE);
            }
            break;

        case ZSTDN_btlazy2:   /* we want the dictionary table fully sorted */
        case ZSTDN_btopt:
        case ZSTDN_btultra:
        case ZSTDN_btultra2:
            if (chunk >= HASH_READ_SIZE)
                ZSTDN_updateTree(ms, ichunk-HASH_READ_SIZE, ichunk);
            break;

        default:
            assert(0);  /* not possible : not a valid strategy id */
        }

        ip = ichunk;
    }

    ms->nextToUpdate = (U32)(iend - ms->window.base);
    return 0;
}


/* Dictionaries that assign zero probability to symbols that show up causes problems
 * when FSE encoding. Mark dictionaries with zero probability symbols as FSEN_repeat_check
 * and only dictionaries with 100% valid symbols can be assumed valid.
 */
static FSEN_repeat ZSTDN_dictNCountRepeat(short* normalizedCounter, unsigned dictMaxSymbolValue, unsigned maxSymbolValue)
{
    U32 s;
    if (dictMaxSymbolValue < maxSymbolValue) {
        return FSEN_repeat_check;
    }
    for (s = 0; s <= maxSymbolValue; ++s) {
        if (normalizedCounter[s] == 0) {
            return FSEN_repeat_check;
        }
    }
    return FSEN_repeat_valid;
}

size_t ZSTDN_loadCEntropy(ZSTDN_compressedBlockState_t* bs, void* workspace,
                         const void* const dict, size_t dictSize)
{
    short offcodeNCount[MaxOff+1];
    unsigned offcodeMaxValue = MaxOff;
    const BYTE* dictPtr = (const BYTE*)dict;    /* skip magic num and dict ID */
    const BYTE* const dictEnd = dictPtr + dictSize;
    dictPtr += 8;
    bs->entropy.huf.repeatMode = HUFN_repeat_check;

    {   unsigned maxSymbolValue = 255;
        unsigned hasZeroWeights = 1;
        size_t const hufHeaderSize = HUFN_readCTable((HUFN_CElt*)bs->entropy.huf.CTable, &maxSymbolValue, dictPtr,
            dictEnd-dictPtr, &hasZeroWeights);

        /* We only set the loaded table as valid if it contains all non-zero
         * weights. Otherwise, we set it to check */
        if (!hasZeroWeights)
            bs->entropy.huf.repeatMode = HUFN_repeat_valid;

        RETURN_ERROR_IF(HUFN_isError(hufHeaderSize), dictionary_corrupted, "");
        RETURN_ERROR_IF(maxSymbolValue < 255, dictionary_corrupted, "");
        dictPtr += hufHeaderSize;
    }

    {   unsigned offcodeLog;
        size_t const offcodeHeaderSize = FSEN_readNCount(offcodeNCount, &offcodeMaxValue, &offcodeLog, dictPtr, dictEnd-dictPtr);
        RETURN_ERROR_IF(FSEN_isError(offcodeHeaderSize), dictionary_corrupted, "");
        RETURN_ERROR_IF(offcodeLog > OffFSELog, dictionary_corrupted, "");
        /* fill all offset symbols to avoid garbage at end of table */
        RETURN_ERROR_IF(FSEN_isError(FSEN_buildCTable_wksp(
                bs->entropy.fse.offcodeCTable,
                offcodeNCount, MaxOff, offcodeLog,
                workspace, HUFN_WORKSPACE_SIZE)),
            dictionary_corrupted, "");
        /* Defer checking offcodeMaxValue because we need to know the size of the dictionary content */
        dictPtr += offcodeHeaderSize;
    }

    {   short matchlengthNCount[MaxML+1];
        unsigned matchlengthMaxValue = MaxML, matchlengthLog;
        size_t const matchlengthHeaderSize = FSEN_readNCount(matchlengthNCount, &matchlengthMaxValue, &matchlengthLog, dictPtr, dictEnd-dictPtr);
        RETURN_ERROR_IF(FSEN_isError(matchlengthHeaderSize), dictionary_corrupted, "");
        RETURN_ERROR_IF(matchlengthLog > MLFSELog, dictionary_corrupted, "");
        RETURN_ERROR_IF(FSEN_isError(FSEN_buildCTable_wksp(
                bs->entropy.fse.matchlengthCTable,
                matchlengthNCount, matchlengthMaxValue, matchlengthLog,
                workspace, HUFN_WORKSPACE_SIZE)),
            dictionary_corrupted, "");
        bs->entropy.fse.matchlength_repeatMode = ZSTDN_dictNCountRepeat(matchlengthNCount, matchlengthMaxValue, MaxML);
        dictPtr += matchlengthHeaderSize;
    }

    {   short litlengthNCount[MaxLL+1];
        unsigned litlengthMaxValue = MaxLL, litlengthLog;
        size_t const litlengthHeaderSize = FSEN_readNCount(litlengthNCount, &litlengthMaxValue, &litlengthLog, dictPtr, dictEnd-dictPtr);
        RETURN_ERROR_IF(FSEN_isError(litlengthHeaderSize), dictionary_corrupted, "");
        RETURN_ERROR_IF(litlengthLog > LLFSELog, dictionary_corrupted, "");
        RETURN_ERROR_IF(FSEN_isError(FSEN_buildCTable_wksp(
                bs->entropy.fse.litlengthCTable,
                litlengthNCount, litlengthMaxValue, litlengthLog,
                workspace, HUFN_WORKSPACE_SIZE)),
            dictionary_corrupted, "");
        bs->entropy.fse.litlength_repeatMode = ZSTDN_dictNCountRepeat(litlengthNCount, litlengthMaxValue, MaxLL);
        dictPtr += litlengthHeaderSize;
    }

    RETURN_ERROR_IF(dictPtr+12 > dictEnd, dictionary_corrupted, "");
    bs->rep[0] = MEM_readLE32(dictPtr+0);
    bs->rep[1] = MEM_readLE32(dictPtr+4);
    bs->rep[2] = MEM_readLE32(dictPtr+8);
    dictPtr += 12;

    {   size_t const dictContentSize = (size_t)(dictEnd - dictPtr);
        U32 offcodeMax = MaxOff;
        if (dictContentSize <= ((U32)-1) - 128 KB) {
            U32 const maxOffset = (U32)dictContentSize + 128 KB; /* The maximum offset that must be supported */
            offcodeMax = ZSTDN_highbit32(maxOffset); /* Calculate minimum offset code required to represent maxOffset */
        }
        /* All offset values <= dictContentSize + 128 KB must be representable for a valid table */
        bs->entropy.fse.offcode_repeatMode = ZSTDN_dictNCountRepeat(offcodeNCount, offcodeMaxValue, MIN(offcodeMax, MaxOff));

        /* All repCodes must be <= dictContentSize and != 0 */
        {   U32 u;
            for (u=0; u<3; u++) {
                RETURN_ERROR_IF(bs->rep[u] == 0, dictionary_corrupted, "");
                RETURN_ERROR_IF(bs->rep[u] > dictContentSize, dictionary_corrupted, "");
    }   }   }

    return dictPtr - (const BYTE*)dict;
}

/* Dictionary format :
 * See :
 * https://github.com/facebook/zstd/blob/release/doc/zstd_compression_format.md#dictionary-format
 */
/*! ZSTDN_loadZstdDictionary() :
 * @return : dictID, or an error code
 *  assumptions : magic number supposed already checked
 *                dictSize supposed >= 8
 */
static size_t ZSTDN_loadZstdDictionary(ZSTDN_compressedBlockState_t* bs,
                                      ZSTDN_matchState_t* ms,
                                      ZSTDN_cwksp* ws,
                                      ZSTDN_CCtx_params const* params,
                                      const void* dict, size_t dictSize,
                                      ZSTDN_dictTableLoadMethod_e dtlm,
                                      void* workspace)
{
    const BYTE* dictPtr = (const BYTE*)dict;
    const BYTE* const dictEnd = dictPtr + dictSize;
    size_t dictID;
    size_t eSize;

    ZSTDN_STATIC_ASSERT(HUFN_WORKSPACE_SIZE >= (1<<MAX(MLFSELog,LLFSELog)));
    assert(dictSize >= 8);
    assert(MEM_readLE32(dictPtr) == ZSTDN_MAGIC_DICTIONARY);

    dictID = params->fParams.noDictIDFlag ? 0 :  MEM_readLE32(dictPtr + 4 /* skip magic number */ );
    eSize = ZSTDN_loadCEntropy(bs, workspace, dict, dictSize);
    FORWARD_IF_ERROR(eSize, "ZSTDN_loadCEntropy failed");
    dictPtr += eSize;

    {
        size_t const dictContentSize = (size_t)(dictEnd - dictPtr);
        FORWARD_IF_ERROR(ZSTDN_loadDictionaryContent(
            ms, NULL, ws, params, dictPtr, dictContentSize, dtlm), "");
    }
    return dictID;
}

/* ZSTDN_compress_insertDictionary() :
*   @return : dictID, or an error code */
static size_t
ZSTDN_compress_insertDictionary(ZSTDN_compressedBlockState_t* bs,
                               ZSTDN_matchState_t* ms,
                               ldmState_t* ls,
                               ZSTDN_cwksp* ws,
                         const ZSTDN_CCtx_params* params,
                         const void* dict, size_t dictSize,
                               ZSTDN_dictContentType_e dictContentType,
                               ZSTDN_dictTableLoadMethod_e dtlm,
                               void* workspace)
{
    DEBUGLOG(4, "ZSTDN_compress_insertDictionary (dictSize=%u)", (U32)dictSize);
    if ((dict==NULL) || (dictSize<8)) {
        RETURN_ERROR_IF(dictContentType == ZSTDN_dct_fullDict, dictionary_wrong, "");
        return 0;
    }

    ZSTDN_reset_compressedBlockState(bs);

    /* dict restricted modes */
    if (dictContentType == ZSTDN_dct_rawContent)
        return ZSTDN_loadDictionaryContent(ms, ls, ws, params, dict, dictSize, dtlm);

    if (MEM_readLE32(dict) != ZSTDN_MAGIC_DICTIONARY) {
        if (dictContentType == ZSTDN_dct_auto) {
            DEBUGLOG(4, "raw content dictionary detected");
            return ZSTDN_loadDictionaryContent(
                ms, ls, ws, params, dict, dictSize, dtlm);
        }
        RETURN_ERROR_IF(dictContentType == ZSTDN_dct_fullDict, dictionary_wrong, "");
        assert(0);   /* impossible */
    }

    /* dict as full zstd dictionary */
    return ZSTDN_loadZstdDictionary(
        bs, ms, ws, params, dict, dictSize, dtlm, workspace);
}

#define ZSTDN_USE_CDICT_PARAMS_SRCSIZE_CUTOFF (128 KB)
#define ZSTDN_USE_CDICT_PARAMS_DICTSIZE_MULTIPLIER (6ULL)

/*! ZSTDN_compressBegin_internal() :
 * @return : 0, or an error code */
static size_t ZSTDN_compressBegin_internal(ZSTDN_CCtx* cctx,
                                    const void* dict, size_t dictSize,
                                    ZSTDN_dictContentType_e dictContentType,
                                    ZSTDN_dictTableLoadMethod_e dtlm,
                                    const ZSTDN_CDict* cdict,
                                    const ZSTDN_CCtx_params* params, U64 pledgedSrcSize,
                                    ZSTDN_buffered_policy_e zbuff)
{
    DEBUGLOG(4, "ZSTDN_compressBegin_internal: wlog=%u", params->cParams.windowLog);
    /* params are supposed to be fully validated at this point */
    assert(!ZSTDN_isError(ZSTDN_checkCParams(params->cParams)));
    assert(!((dict) && (cdict)));  /* either dict or cdict, not both */
    if ( (cdict)
      && (cdict->dictContentSize > 0)
      && ( pledgedSrcSize < ZSTDN_USE_CDICT_PARAMS_SRCSIZE_CUTOFF
        || pledgedSrcSize < cdict->dictContentSize * ZSTDN_USE_CDICT_PARAMS_DICTSIZE_MULTIPLIER
        || pledgedSrcSize == ZSTDN_CONTENTSIZE_UNKNOWN
        || cdict->compressionLevel == 0)
      && (params->attachDictPref != ZSTDN_dictForceLoad) ) {
        return ZSTDN_resetCCtx_usingCDict(cctx, cdict, params, pledgedSrcSize, zbuff);
    }

    FORWARD_IF_ERROR( ZSTDN_resetCCtx_internal(cctx, *params, pledgedSrcSize,
                                     ZSTDcrp_makeClean, zbuff) , "");
    {   size_t const dictID = cdict ?
                ZSTDN_compress_insertDictionary(
                        cctx->blockState.prevCBlock, &cctx->blockState.matchState,
                        &cctx->ldmState, &cctx->workspace, &cctx->appliedParams, cdict->dictContent,
                        cdict->dictContentSize, cdict->dictContentType, dtlm,
                        cctx->entropyWorkspace)
              : ZSTDN_compress_insertDictionary(
                        cctx->blockState.prevCBlock, &cctx->blockState.matchState,
                        &cctx->ldmState, &cctx->workspace, &cctx->appliedParams, dict, dictSize,
                        dictContentType, dtlm, cctx->entropyWorkspace);
        FORWARD_IF_ERROR(dictID, "ZSTDN_compress_insertDictionary failed");
        assert(dictID <= UINT_MAX);
        cctx->dictID = (U32)dictID;
        cctx->dictContentSize = cdict ? cdict->dictContentSize : dictSize;
    }
    return 0;
}

size_t ZSTDN_compressBegin_advanced_internal(ZSTDN_CCtx* cctx,
                                    const void* dict, size_t dictSize,
                                    ZSTDN_dictContentType_e dictContentType,
                                    ZSTDN_dictTableLoadMethod_e dtlm,
                                    const ZSTDN_CDict* cdict,
                                    const ZSTDN_CCtx_params* params,
                                    unsigned long long pledgedSrcSize)
{
    DEBUGLOG(4, "ZSTDN_compressBegin_advanced_internal: wlog=%u", params->cParams.windowLog);
    /* compression parameters verification and optimization */
    FORWARD_IF_ERROR( ZSTDN_checkCParams(params->cParams) , "");
    return ZSTDN_compressBegin_internal(cctx,
                                       dict, dictSize, dictContentType, dtlm,
                                       cdict,
                                       params, pledgedSrcSize,
                                       ZSTDb_not_buffered);
}

/*! ZSTDN_compressBegin_advanced() :
*   @return : 0, or an error code */
size_t ZSTDN_compressBegin_advanced(ZSTDN_CCtx* cctx,
                             const void* dict, size_t dictSize,
                                   ZSTDN_parameters params, unsigned long long pledgedSrcSize)
{
    ZSTDN_CCtx_params cctxParams;
    ZSTDN_CCtxParams_init_internal(&cctxParams, &params, ZSTDN_NO_CLEVEL);
    return ZSTDN_compressBegin_advanced_internal(cctx,
                                            dict, dictSize, ZSTDN_dct_auto, ZSTDN_dtlm_fast,
                                            NULL /*cdict*/,
                                            &cctxParams, pledgedSrcSize);
}

size_t ZSTDN_compressBegin_usingDict(ZSTDN_CCtx* cctx, const void* dict, size_t dictSize, int compressionLevel)
{
    ZSTDN_CCtx_params cctxParams;
    {
        ZSTDN_parameters const params = ZSTDN_getParams_internal(compressionLevel, ZSTDN_CONTENTSIZE_UNKNOWN, dictSize, ZSTDN_cpm_noAttachDict);
        ZSTDN_CCtxParams_init_internal(&cctxParams, &params, (compressionLevel == 0) ? ZSTDN_CLEVEL_DEFAULT : compressionLevel);
    }
    DEBUGLOG(4, "ZSTDN_compressBegin_usingDict (dictSize=%u)", (unsigned)dictSize);
    return ZSTDN_compressBegin_internal(cctx, dict, dictSize, ZSTDN_dct_auto, ZSTDN_dtlm_fast, NULL,
                                       &cctxParams, ZSTDN_CONTENTSIZE_UNKNOWN, ZSTDb_not_buffered);
}

size_t ZSTDN_compressBegin(ZSTDN_CCtx* cctx, int compressionLevel)
{
    return ZSTDN_compressBegin_usingDict(cctx, NULL, 0, compressionLevel);
}


/*! ZSTDN_writeEpilogue() :
*   Ends a frame.
*   @return : nb of bytes written into dst (or an error code) */
static size_t ZSTDN_writeEpilogue(ZSTDN_CCtx* cctx, void* dst, size_t dstCapacity)
{
    BYTE* const ostart = (BYTE*)dst;
    BYTE* op = ostart;
    size_t fhSize = 0;

    DEBUGLOG(4, "ZSTDN_writeEpilogue");
    RETURN_ERROR_IF(cctx->stage == ZSTDcs_created, stage_wrong, "init missing");

    /* special case : empty frame */
    if (cctx->stage == ZSTDcs_init) {
        fhSize = ZSTDN_writeFrameHeader(dst, dstCapacity, &cctx->appliedParams, 0, 0);
        FORWARD_IF_ERROR(fhSize, "ZSTDN_writeFrameHeader failed");
        dstCapacity -= fhSize;
        op += fhSize;
        cctx->stage = ZSTDcs_ongoing;
    }

    if (cctx->stage != ZSTDcs_ending) {
        /* write one last empty block, make it the "last" block */
        U32 const cBlockHeader24 = 1 /* last block */ + (((U32)bt_raw)<<1) + 0;
        RETURN_ERROR_IF(dstCapacity<4, dstSize_tooSmall, "no room for epilogue");
        MEM_writeLE32(op, cBlockHeader24);
        op += ZSTDN_blockHeaderSize;
        dstCapacity -= ZSTDN_blockHeaderSize;
    }

    if (cctx->appliedParams.fParams.checksumFlag) {
        U32 const checksum = (U32) xxh64n_digest(&cctx->xxhState);
        RETURN_ERROR_IF(dstCapacity<4, dstSize_tooSmall, "no room for checksum");
        DEBUGLOG(4, "ZSTDN_writeEpilogue: write checksum : %08X", (unsigned)checksum);
        MEM_writeLE32(op, checksum);
        op += 4;
    }

    cctx->stage = ZSTDcs_created;  /* return to "created but no init" status */
    return op-ostart;
}

void ZSTDN_CCtx_trace(ZSTDN_CCtx* cctx, size_t extraCSize)
{
    (void)cctx;
    (void)extraCSize;
}

size_t ZSTDN_compressEnd (ZSTDN_CCtx* cctx,
                         void* dst, size_t dstCapacity,
                   const void* src, size_t srcSize)
{
    size_t endResult;
    size_t const cSize = ZSTDN_compressContinue_internal(cctx,
                                dst, dstCapacity, src, srcSize,
                                1 /* frame mode */, 1 /* last chunk */);
    FORWARD_IF_ERROR(cSize, "ZSTDN_compressContinue_internal failed");
    endResult = ZSTDN_writeEpilogue(cctx, (char*)dst + cSize, dstCapacity-cSize);
    FORWARD_IF_ERROR(endResult, "ZSTDN_writeEpilogue failed");
    assert(!(cctx->appliedParams.fParams.contentSizeFlag && cctx->pledgedSrcSizePlusOne == 0));
    if (cctx->pledgedSrcSizePlusOne != 0) {  /* control src size */
        ZSTDN_STATIC_ASSERT(ZSTDN_CONTENTSIZE_UNKNOWN == (unsigned long long)-1);
        DEBUGLOG(4, "end of frame : controlling src size");
        RETURN_ERROR_IF(
            cctx->pledgedSrcSizePlusOne != cctx->consumedSrcSize+1,
            srcSize_wrong,
             "error : pledgedSrcSize = %u, while realSrcSize = %u",
            (unsigned)cctx->pledgedSrcSizePlusOne-1,
            (unsigned)cctx->consumedSrcSize);
    }
    ZSTDN_CCtx_trace(cctx, endResult);
    return cSize + endResult;
}

size_t ZSTDN_compress_advanced (ZSTDN_CCtx* cctx,
                               void* dst, size_t dstCapacity,
                         const void* src, size_t srcSize,
                         const void* dict,size_t dictSize,
                               ZSTDN_parameters params)
{
    ZSTDN_CCtx_params cctxParams;
    DEBUGLOG(4, "ZSTDN_compress_advanced");
    FORWARD_IF_ERROR(ZSTDN_checkCParams(params.cParams), "");
    ZSTDN_CCtxParams_init_internal(&cctxParams, &params, ZSTDN_NO_CLEVEL);
    return ZSTDN_compress_advanced_internal(cctx,
                                           dst, dstCapacity,
                                           src, srcSize,
                                           dict, dictSize,
                                           &cctxParams);
}

/* Internal */
size_t ZSTDN_compress_advanced_internal(
        ZSTDN_CCtx* cctx,
        void* dst, size_t dstCapacity,
        const void* src, size_t srcSize,
        const void* dict,size_t dictSize,
        const ZSTDN_CCtx_params* params)
{
    DEBUGLOG(4, "ZSTDN_compress_advanced_internal (srcSize:%u)", (unsigned)srcSize);
    FORWARD_IF_ERROR( ZSTDN_compressBegin_internal(cctx,
                         dict, dictSize, ZSTDN_dct_auto, ZSTDN_dtlm_fast, NULL,
                         params, srcSize, ZSTDb_not_buffered) , "");
    return ZSTDN_compressEnd(cctx, dst, dstCapacity, src, srcSize);
}

size_t ZSTDN_compress_usingDict(ZSTDN_CCtx* cctx,
                               void* dst, size_t dstCapacity,
                         const void* src, size_t srcSize,
                         const void* dict, size_t dictSize,
                               int compressionLevel)
{
    ZSTDN_CCtx_params cctxParams;
    {
        ZSTDN_parameters const params = ZSTDN_getParams_internal(compressionLevel, srcSize, dict ? dictSize : 0, ZSTDN_cpm_noAttachDict);
        assert(params.fParams.contentSizeFlag == 1);
        ZSTDN_CCtxParams_init_internal(&cctxParams, &params, (compressionLevel == 0) ? ZSTDN_CLEVEL_DEFAULT: compressionLevel);
    }
    DEBUGLOG(4, "ZSTDN_compress_usingDict (srcSize=%u)", (unsigned)srcSize);
    return ZSTDN_compress_advanced_internal(cctx, dst, dstCapacity, src, srcSize, dict, dictSize, &cctxParams);
}

size_t ZSTDN_compressCCtx(ZSTDN_CCtx* cctx,
                         void* dst, size_t dstCapacity,
                   const void* src, size_t srcSize,
                         int compressionLevel)
{
    DEBUGLOG(4, "ZSTDN_compressCCtx (srcSize=%u)", (unsigned)srcSize);
    assert(cctx != NULL);
    return ZSTDN_compress_usingDict(cctx, dst, dstCapacity, src, srcSize, NULL, 0, compressionLevel);
}

size_t ZSTDN_compress(void* dst, size_t dstCapacity,
               const void* src, size_t srcSize,
                     int compressionLevel)
{
    size_t result;
    ZSTDN_CCtx* cctx = ZSTDN_createCCtx();
    RETURN_ERROR_IF(!cctx, memory_allocation, "ZSTDN_createCCtx failed");
    result = ZSTDN_compressCCtx(cctx, dst, dstCapacity, src, srcSize, compressionLevel);
    ZSTDN_freeCCtx(cctx);
    return result;
}


/* =====  Dictionary API  ===== */

/*! ZSTDN_estimateCDictSize_advanced() :
 *  Estimate amount of memory that will be needed to create a dictionary with following arguments */
size_t ZSTDN_estimateCDictSize_advanced(
        size_t dictSize, ZSTDN_compressionParameters cParams,
        ZSTDN_dictLoadMethod_e dictLoadMethod)
{
    DEBUGLOG(5, "sizeof(ZSTDN_CDict) : %u", (unsigned)sizeof(ZSTDN_CDict));
    return ZSTDN_cwksp_alloc_size(sizeof(ZSTDN_CDict))
         + ZSTDN_cwksp_alloc_size(HUFN_WORKSPACE_SIZE)
         + ZSTDN_sizeof_matchState(&cParams, /* forCCtx */ 0)
         + (dictLoadMethod == ZSTDN_dlm_byRef ? 0
            : ZSTDN_cwksp_alloc_size(ZSTDN_cwksp_align(dictSize, sizeof(void *))));
}

size_t ZSTDN_estimateCDictSize(size_t dictSize, int compressionLevel)
{
    ZSTDN_compressionParameters const cParams = ZSTDN_getCParams_internal(compressionLevel, ZSTDN_CONTENTSIZE_UNKNOWN, dictSize, ZSTDN_cpm_createCDict);
    return ZSTDN_estimateCDictSize_advanced(dictSize, cParams, ZSTDN_dlm_byCopy);
}

size_t ZSTDN_sizeof_CDict(const ZSTDN_CDict* cdict)
{
    if (cdict==NULL) return 0;   /* support sizeof on NULL */
    DEBUGLOG(5, "sizeof(*cdict) : %u", (unsigned)sizeof(*cdict));
    /* cdict may be in the workspace */
    return (cdict->workspace.workspace == cdict ? 0 : sizeof(*cdict))
        + ZSTDN_cwksp_sizeof(&cdict->workspace);
}

static size_t ZSTDN_initCDict_internal(
                    ZSTDN_CDict* cdict,
              const void* dictBuffer, size_t dictSize,
                    ZSTDN_dictLoadMethod_e dictLoadMethod,
                    ZSTDN_dictContentType_e dictContentType,
                    ZSTDN_CCtx_params params)
{
    DEBUGLOG(3, "ZSTDN_initCDict_internal (dictContentType:%u)", (unsigned)dictContentType);
    assert(!ZSTDN_checkCParams(params.cParams));
    cdict->matchState.cParams = params.cParams;
    cdict->matchState.dedicatedDictSearch = params.enableDedicatedDictSearch;
    if (cdict->matchState.dedicatedDictSearch && dictSize > ZSTDN_CHUNKSIZE_MAX) {
        cdict->matchState.dedicatedDictSearch = 0;
    }
    if ((dictLoadMethod == ZSTDN_dlm_byRef) || (!dictBuffer) || (!dictSize)) {
        cdict->dictContent = dictBuffer;
    } else {
         void *internalBuffer = ZSTDN_cwksp_reserve_object(&cdict->workspace, ZSTDN_cwksp_align(dictSize, sizeof(void*)));
        RETURN_ERROR_IF(!internalBuffer, memory_allocation, "NULL pointer!");
        cdict->dictContent = internalBuffer;
        ZSTDN_memcpy(internalBuffer, dictBuffer, dictSize);
    }
    cdict->dictContentSize = dictSize;
    cdict->dictContentType = dictContentType;

    cdict->entropyWorkspace = (U32*)ZSTDN_cwksp_reserve_object(&cdict->workspace, HUFN_WORKSPACE_SIZE);


    /* Reset the state to no dictionary */
    ZSTDN_reset_compressedBlockState(&cdict->cBlockState);
    FORWARD_IF_ERROR(ZSTDN_reset_matchState(
        &cdict->matchState,
        &cdict->workspace,
        &params.cParams,
        ZSTDcrp_makeClean,
        ZSTDirp_reset,
        ZSTDN_resetTarget_CDict), "");
    /* (Maybe) load the dictionary
     * Skips loading the dictionary if it is < 8 bytes.
     */
    {   params.compressionLevel = ZSTDN_CLEVEL_DEFAULT;
        params.fParams.contentSizeFlag = 1;
        {   size_t const dictID = ZSTDN_compress_insertDictionary(
                    &cdict->cBlockState, &cdict->matchState, NULL, &cdict->workspace,
                    &params, cdict->dictContent, cdict->dictContentSize,
                    dictContentType, ZSTDN_dtlm_full, cdict->entropyWorkspace);
            FORWARD_IF_ERROR(dictID, "ZSTDN_compress_insertDictionary failed");
            assert(dictID <= (size_t)(U32)-1);
            cdict->dictID = (U32)dictID;
        }
    }

    return 0;
}

static ZSTDN_CDict* ZSTDN_createCDict_advanced_internal(size_t dictSize,
                                      ZSTDN_dictLoadMethod_e dictLoadMethod,
                                      ZSTDN_compressionParameters cParams, ZSTDN_customMem customMem)
{
    if ((!customMem.customAlloc) ^ (!customMem.customFree)) return NULL;

    {   size_t const workspaceSize =
            ZSTDN_cwksp_alloc_size(sizeof(ZSTDN_CDict)) +
            ZSTDN_cwksp_alloc_size(HUFN_WORKSPACE_SIZE) +
            ZSTDN_sizeof_matchState(&cParams, /* forCCtx */ 0) +
            (dictLoadMethod == ZSTDN_dlm_byRef ? 0
             : ZSTDN_cwksp_alloc_size(ZSTDN_cwksp_align(dictSize, sizeof(void*))));
        void* const workspace = ZSTDN_customMalloc(workspaceSize, customMem);
        ZSTDN_cwksp ws;
        ZSTDN_CDict* cdict;

        if (!workspace) {
            ZSTDN_customFree(workspace, customMem);
            return NULL;
        }

        ZSTDN_cwksp_init(&ws, workspace, workspaceSize, ZSTDN_cwksp_dynamic_alloc);

        cdict = (ZSTDN_CDict*)ZSTDN_cwksp_reserve_object(&ws, sizeof(ZSTDN_CDict));
        assert(cdict != NULL);
        ZSTDN_cwksp_move(&cdict->workspace, &ws);
        cdict->customMem = customMem;
        cdict->compressionLevel = ZSTDN_NO_CLEVEL; /* signals advanced API usage */

        return cdict;
    }
}

ZSTDN_CDict* ZSTDN_createCDict_advanced(const void* dictBuffer, size_t dictSize,
                                      ZSTDN_dictLoadMethod_e dictLoadMethod,
                                      ZSTDN_dictContentType_e dictContentType,
                                      ZSTDN_compressionParameters cParams,
                                      ZSTDN_customMem customMem)
{
    ZSTDN_CCtx_params cctxParams;
    ZSTDN_memset(&cctxParams, 0, sizeof(cctxParams));
    ZSTDN_CCtxParams_init(&cctxParams, 0);
    cctxParams.cParams = cParams;
    cctxParams.customMem = customMem;
    return ZSTDN_createCDict_advanced2(
        dictBuffer, dictSize,
        dictLoadMethod, dictContentType,
        &cctxParams, customMem);
}

ZSTDLIB_API ZSTDN_CDict* ZSTDN_createCDict_advanced2(
        const void* dict, size_t dictSize,
        ZSTDN_dictLoadMethod_e dictLoadMethod,
        ZSTDN_dictContentType_e dictContentType,
        const ZSTDN_CCtx_params* originalCctxParams,
        ZSTDN_customMem customMem)
{
    ZSTDN_CCtx_params cctxParams = *originalCctxParams;
    ZSTDN_compressionParameters cParams;
    ZSTDN_CDict* cdict;

    DEBUGLOG(3, "ZSTDN_createCDict_advanced2, mode %u", (unsigned)dictContentType);
    if (!customMem.customAlloc ^ !customMem.customFree) return NULL;

    if (cctxParams.enableDedicatedDictSearch) {
        cParams = ZSTDN_dedicatedDictSearch_getCParams(
            cctxParams.compressionLevel, dictSize);
        ZSTDN_overrideCParams(&cParams, &cctxParams.cParams);
    } else {
        cParams = ZSTDN_getCParamsFromCCtxParams(
            &cctxParams, ZSTDN_CONTENTSIZE_UNKNOWN, dictSize, ZSTDN_cpm_createCDict);
    }

    if (!ZSTDN_dedicatedDictSearch_isSupported(&cParams)) {
        /* Fall back to non-DDSS params */
        cctxParams.enableDedicatedDictSearch = 0;
        cParams = ZSTDN_getCParamsFromCCtxParams(
            &cctxParams, ZSTDN_CONTENTSIZE_UNKNOWN, dictSize, ZSTDN_cpm_createCDict);
    }

    cctxParams.cParams = cParams;

    cdict = ZSTDN_createCDict_advanced_internal(dictSize,
                        dictLoadMethod, cctxParams.cParams,
                        customMem);

    if (ZSTDN_isError( ZSTDN_initCDict_internal(cdict,
                                    dict, dictSize,
                                    dictLoadMethod, dictContentType,
                                    cctxParams) )) {
        ZSTDN_freeCDict(cdict);
        return NULL;
    }

    return cdict;
}

ZSTDN_CDict* ZSTDN_createCDict(const void* dict, size_t dictSize, int compressionLevel)
{
    ZSTDN_compressionParameters cParams = ZSTDN_getCParams_internal(compressionLevel, ZSTDN_CONTENTSIZE_UNKNOWN, dictSize, ZSTDN_cpm_createCDict);
    ZSTDN_CDict* const cdict = ZSTDN_createCDict_advanced(dict, dictSize,
                                                  ZSTDN_dlm_byCopy, ZSTDN_dct_auto,
                                                  cParams, ZSTDN_defaultCMem);
    if (cdict)
        cdict->compressionLevel = (compressionLevel == 0) ? ZSTDN_CLEVEL_DEFAULT : compressionLevel;
    return cdict;
}

ZSTDN_CDict* ZSTDN_createCDict_byReference(const void* dict, size_t dictSize, int compressionLevel)
{
    ZSTDN_compressionParameters cParams = ZSTDN_getCParams_internal(compressionLevel, ZSTDN_CONTENTSIZE_UNKNOWN, dictSize, ZSTDN_cpm_createCDict);
    ZSTDN_CDict* const cdict = ZSTDN_createCDict_advanced(dict, dictSize,
                                     ZSTDN_dlm_byRef, ZSTDN_dct_auto,
                                     cParams, ZSTDN_defaultCMem);
    if (cdict)
        cdict->compressionLevel = (compressionLevel == 0) ? ZSTDN_CLEVEL_DEFAULT : compressionLevel;
    return cdict;
}

size_t ZSTDN_freeCDict(ZSTDN_CDict* cdict)
{
    if (cdict==NULL) return 0;   /* support free on NULL */
    {   ZSTDN_customMem const cMem = cdict->customMem;
        int cdictInWorkspace = ZSTDN_cwksp_owns_buffer(&cdict->workspace, cdict);
        ZSTDN_cwksp_free(&cdict->workspace, cMem);
        if (!cdictInWorkspace) {
            ZSTDN_customFree(cdict, cMem);
        }
        return 0;
    }
}

/*! ZSTDN_initStaticCDict_advanced() :
 *  Generate a digested dictionary in provided memory area.
 *  workspace: The memory area to emplace the dictionary into.
 *             Provided pointer must 8-bytes aligned.
 *             It must outlive dictionary usage.
 *  workspaceSize: Use ZSTDN_estimateCDictSize()
 *                 to determine how large workspace must be.
 *  cParams : use ZSTDN_getCParams() to transform a compression level
 *            into its relevants cParams.
 * @return : pointer to ZSTDN_CDict*, or NULL if error (size too small)
 *  Note : there is no corresponding "free" function.
 *         Since workspace was allocated externally, it must be freed externally.
 */
const ZSTDN_CDict* ZSTDN_initStaticCDict(
                                 void* workspace, size_t workspaceSize,
                           const void* dict, size_t dictSize,
                                 ZSTDN_dictLoadMethod_e dictLoadMethod,
                                 ZSTDN_dictContentType_e dictContentType,
                                 ZSTDN_compressionParameters cParams)
{
    size_t const matchStateSize = ZSTDN_sizeof_matchState(&cParams, /* forCCtx */ 0);
    size_t const neededSize = ZSTDN_cwksp_alloc_size(sizeof(ZSTDN_CDict))
                            + (dictLoadMethod == ZSTDN_dlm_byRef ? 0
                               : ZSTDN_cwksp_alloc_size(ZSTDN_cwksp_align(dictSize, sizeof(void*))))
                            + ZSTDN_cwksp_alloc_size(HUFN_WORKSPACE_SIZE)
                            + matchStateSize;
    ZSTDN_CDict* cdict;
    ZSTDN_CCtx_params params;

    if ((size_t)workspace & 7) return NULL;  /* 8-aligned */

    {
        ZSTDN_cwksp ws;
        ZSTDN_cwksp_init(&ws, workspace, workspaceSize, ZSTDN_cwksp_static_alloc);
        cdict = (ZSTDN_CDict*)ZSTDN_cwksp_reserve_object(&ws, sizeof(ZSTDN_CDict));
        if (cdict == NULL) return NULL;
        ZSTDN_cwksp_move(&cdict->workspace, &ws);
    }

    DEBUGLOG(4, "(workspaceSize < neededSize) : (%u < %u) => %u",
        (unsigned)workspaceSize, (unsigned)neededSize, (unsigned)(workspaceSize < neededSize));
    if (workspaceSize < neededSize) return NULL;

    ZSTDN_CCtxParams_init(&params, 0);
    params.cParams = cParams;

    if (ZSTDN_isError( ZSTDN_initCDict_internal(cdict,
                                              dict, dictSize,
                                              dictLoadMethod, dictContentType,
                                              params) ))
        return NULL;

    return cdict;
}

ZSTDN_compressionParameters ZSTDN_getCParamsFromCDict(const ZSTDN_CDict* cdict)
{
    assert(cdict != NULL);
    return cdict->matchState.cParams;
}

/*! ZSTDN_getDictID_fromCDict() :
 *  Provides the dictID of the dictionary loaded into `cdict`.
 *  If @return == 0, the dictionary is not conformant to Zstandard specification, or empty.
 *  Non-conformant dictionaries can still be loaded, but as content-only dictionaries. */
unsigned ZSTDN_getDictID_fromCDict(const ZSTDN_CDict* cdict)
{
    if (cdict==NULL) return 0;
    return cdict->dictID;
}


/* ZSTDN_compressBegin_usingCDict_advanced() :
 * cdict must be != NULL */
size_t ZSTDN_compressBegin_usingCDict_advanced(
    ZSTDN_CCtx* const cctx, const ZSTDN_CDict* const cdict,
    ZSTDN_frameParameters const fParams, unsigned long long const pledgedSrcSize)
{
    ZSTDN_CCtx_params cctxParams;
    DEBUGLOG(4, "ZSTDN_compressBegin_usingCDict_advanced");
    RETURN_ERROR_IF(cdict==NULL, dictionary_wrong, "NULL pointer!");
    /* Initialize the cctxParams from the cdict */
    {
        ZSTDN_parameters params;
        params.fParams = fParams;
        params.cParams = ( pledgedSrcSize < ZSTDN_USE_CDICT_PARAMS_SRCSIZE_CUTOFF
                        || pledgedSrcSize < cdict->dictContentSize * ZSTDN_USE_CDICT_PARAMS_DICTSIZE_MULTIPLIER
                        || pledgedSrcSize == ZSTDN_CONTENTSIZE_UNKNOWN
                        || cdict->compressionLevel == 0 ) ?
                ZSTDN_getCParamsFromCDict(cdict)
              : ZSTDN_getCParams(cdict->compressionLevel,
                                pledgedSrcSize,
                                cdict->dictContentSize);
        ZSTDN_CCtxParams_init_internal(&cctxParams, &params, cdict->compressionLevel);
    }
    /* Increase window log to fit the entire dictionary and source if the
     * source size is known. Limit the increase to 19, which is the
     * window log for compression level 1 with the largest source size.
     */
    if (pledgedSrcSize != ZSTDN_CONTENTSIZE_UNKNOWN) {
        U32 const limitedSrcSize = (U32)MIN(pledgedSrcSize, 1U << 19);
        U32 const limitedSrcLog = limitedSrcSize > 1 ? ZSTDN_highbit32(limitedSrcSize - 1) + 1 : 1;
        cctxParams.cParams.windowLog = MAX(cctxParams.cParams.windowLog, limitedSrcLog);
    }
    return ZSTDN_compressBegin_internal(cctx,
                                        NULL, 0, ZSTDN_dct_auto, ZSTDN_dtlm_fast,
                                        cdict,
                                        &cctxParams, pledgedSrcSize,
                                        ZSTDb_not_buffered);
}

/* ZSTDN_compressBegin_usingCDict() :
 * pledgedSrcSize=0 means "unknown"
 * if pledgedSrcSize>0, it will enable contentSizeFlag */
size_t ZSTDN_compressBegin_usingCDict(ZSTDN_CCtx* cctx, const ZSTDN_CDict* cdict)
{
    ZSTDN_frameParameters const fParams = { 0 /*content*/, 0 /*checksum*/, 0 /*noDictID*/ };
    DEBUGLOG(4, "ZSTDN_compressBegin_usingCDict : dictIDFlag == %u", !fParams.noDictIDFlag);
    return ZSTDN_compressBegin_usingCDict_advanced(cctx, cdict, fParams, ZSTDN_CONTENTSIZE_UNKNOWN);
}

size_t ZSTDN_compress_usingCDict_advanced(ZSTDN_CCtx* cctx,
                                void* dst, size_t dstCapacity,
                                const void* src, size_t srcSize,
                                const ZSTDN_CDict* cdict, ZSTDN_frameParameters fParams)
{
    FORWARD_IF_ERROR(ZSTDN_compressBegin_usingCDict_advanced(cctx, cdict, fParams, srcSize), "");   /* will check if cdict != NULL */
    return ZSTDN_compressEnd(cctx, dst, dstCapacity, src, srcSize);
}

/*! ZSTDN_compress_usingCDict() :
 *  Compression using a digested Dictionary.
 *  Faster startup than ZSTDN_compress_usingDict(), recommended when same dictionary is used multiple times.
 *  Note that compression parameters are decided at CDict creation time
 *  while frame parameters are hardcoded */
size_t ZSTDN_compress_usingCDict(ZSTDN_CCtx* cctx,
                                void* dst, size_t dstCapacity,
                                const void* src, size_t srcSize,
                                const ZSTDN_CDict* cdict)
{
    ZSTDN_frameParameters const fParams = { 1 /*content*/, 0 /*checksum*/, 0 /*noDictID*/ };
    return ZSTDN_compress_usingCDict_advanced(cctx, dst, dstCapacity, src, srcSize, cdict, fParams);
}



/* ******************************************************************
*  Streaming
********************************************************************/

ZSTDN_CStream* ZSTDN_createCStream(void)
{
    DEBUGLOG(3, "ZSTDN_createCStream");
    return ZSTDN_createCStream_advanced(ZSTDN_defaultCMem);
}

ZSTDN_CStream* ZSTDN_initStaticCStream(void *workspace, size_t workspaceSize)
{
    return ZSTDN_initStaticCCtx(workspace, workspaceSize);
}

ZSTDN_CStream* ZSTDN_createCStream_advanced(ZSTDN_customMem customMem)
{   /* CStream and CCtx are now same object */
    return ZSTDN_createCCtx_advanced(customMem);
}

size_t ZSTDN_freeCStream(ZSTDN_CStream* zcs)
{
    return ZSTDN_freeCCtx(zcs);   /* same object */
}



/*======   Initialization   ======*/

size_t ZSTDN_CStreamInSize(void)  { return ZSTDN_BLOCKSIZE_MAX; }

size_t ZSTDN_CStreamOutSize(void)
{
    return ZSTDN_compressBound(ZSTDN_BLOCKSIZE_MAX) + ZSTDN_blockHeaderSize + 4 /* 32-bits hash */ ;
}

static ZSTDN_cParamMode_e ZSTDN_getCParamMode(ZSTDN_CDict const* cdict, ZSTDN_CCtx_params const* params, U64 pledgedSrcSize)
{
    if (cdict != NULL && ZSTDN_shouldAttachDict(cdict, params, pledgedSrcSize))
        return ZSTDN_cpm_attachDict;
    else
        return ZSTDN_cpm_noAttachDict;
}

/* ZSTDN_resetCStream():
 * pledgedSrcSize == 0 means "unknown" */
size_t ZSTDN_resetCStream(ZSTDN_CStream* zcs, unsigned long long pss)
{
    /* temporary : 0 interpreted as "unknown" during transition period.
     * Users willing to specify "unknown" **must** use ZSTDN_CONTENTSIZE_UNKNOWN.
     * 0 will be interpreted as "empty" in the future.
     */
    U64 const pledgedSrcSize = (pss==0) ? ZSTDN_CONTENTSIZE_UNKNOWN : pss;
    DEBUGLOG(4, "ZSTDN_resetCStream: pledgedSrcSize = %u", (unsigned)pledgedSrcSize);
    FORWARD_IF_ERROR( ZSTDN_CCtx_reset(zcs, ZSTDN_reset_session_only) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize) , "");
    return 0;
}

/*! ZSTDN_initCStream_internal() :
 *  Note : for lib/compress only. Used by zstdmt_compress.c.
 *  Assumption 1 : params are valid
 *  Assumption 2 : either dict, or cdict, is defined, not both */
size_t ZSTDN_initCStream_internal(ZSTDN_CStream* zcs,
                    const void* dict, size_t dictSize, const ZSTDN_CDict* cdict,
                    const ZSTDN_CCtx_params* params,
                    unsigned long long pledgedSrcSize)
{
    DEBUGLOG(4, "ZSTDN_initCStream_internal");
    FORWARD_IF_ERROR( ZSTDN_CCtx_reset(zcs, ZSTDN_reset_session_only) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize) , "");
    assert(!ZSTDN_isError(ZSTDN_checkCParams(params->cParams)));
    zcs->requestedParams = *params;
    assert(!((dict) && (cdict)));  /* either dict or cdict, not both */
    if (dict) {
        FORWARD_IF_ERROR( ZSTDN_CCtx_loadDictionary(zcs, dict, dictSize) , "");
    } else {
        /* Dictionary is cleared if !cdict */
        FORWARD_IF_ERROR( ZSTDN_CCtx_refCDict(zcs, cdict) , "");
    }
    return 0;
}

/* ZSTDN_initCStream_usingCDict_advanced() :
 * same as ZSTDN_initCStream_usingCDict(), with control over frame parameters */
size_t ZSTDN_initCStream_usingCDict_advanced(ZSTDN_CStream* zcs,
                                            const ZSTDN_CDict* cdict,
                                            ZSTDN_frameParameters fParams,
                                            unsigned long long pledgedSrcSize)
{
    DEBUGLOG(4, "ZSTDN_initCStream_usingCDict_advanced");
    FORWARD_IF_ERROR( ZSTDN_CCtx_reset(zcs, ZSTDN_reset_session_only) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize) , "");
    zcs->requestedParams.fParams = fParams;
    FORWARD_IF_ERROR( ZSTDN_CCtx_refCDict(zcs, cdict) , "");
    return 0;
}

/* note : cdict must outlive compression session */
size_t ZSTDN_initCStream_usingCDict(ZSTDN_CStream* zcs, const ZSTDN_CDict* cdict)
{
    DEBUGLOG(4, "ZSTDN_initCStream_usingCDict");
    FORWARD_IF_ERROR( ZSTDN_CCtx_reset(zcs, ZSTDN_reset_session_only) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_refCDict(zcs, cdict) , "");
    return 0;
}


/* ZSTDN_initCStream_advanced() :
 * pledgedSrcSize must be exact.
 * if srcSize is not known at init time, use value ZSTDN_CONTENTSIZE_UNKNOWN.
 * dict is loaded with default parameters ZSTDN_dct_auto and ZSTDN_dlm_byCopy. */
size_t ZSTDN_initCStream_advanced(ZSTDN_CStream* zcs,
                                 const void* dict, size_t dictSize,
                                 ZSTDN_parameters params, unsigned long long pss)
{
    /* for compatibility with older programs relying on this behavior.
     * Users should now specify ZSTDN_CONTENTSIZE_UNKNOWN.
     * This line will be removed in the future.
     */
    U64 const pledgedSrcSize = (pss==0 && params.fParams.contentSizeFlag==0) ? ZSTDN_CONTENTSIZE_UNKNOWN : pss;
    DEBUGLOG(4, "ZSTDN_initCStream_advanced");
    FORWARD_IF_ERROR( ZSTDN_CCtx_reset(zcs, ZSTDN_reset_session_only) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize) , "");
    FORWARD_IF_ERROR( ZSTDN_checkCParams(params.cParams) , "");
    ZSTDN_CCtxParams_setZstdParams(&zcs->requestedParams, &params);
    FORWARD_IF_ERROR( ZSTDN_CCtx_loadDictionary(zcs, dict, dictSize) , "");
    return 0;
}

size_t ZSTDN_initCStream_usingDict(ZSTDN_CStream* zcs, const void* dict, size_t dictSize, int compressionLevel)
{
    DEBUGLOG(4, "ZSTDN_initCStream_usingDict");
    FORWARD_IF_ERROR( ZSTDN_CCtx_reset(zcs, ZSTDN_reset_session_only) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_setParameter(zcs, ZSTDN_c_compressionLevel, compressionLevel) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_loadDictionary(zcs, dict, dictSize) , "");
    return 0;
}

size_t ZSTDN_initCStream_srcSize(ZSTDN_CStream* zcs, int compressionLevel, unsigned long long pss)
{
    /* temporary : 0 interpreted as "unknown" during transition period.
     * Users willing to specify "unknown" **must** use ZSTDN_CONTENTSIZE_UNKNOWN.
     * 0 will be interpreted as "empty" in the future.
     */
    U64 const pledgedSrcSize = (pss==0) ? ZSTDN_CONTENTSIZE_UNKNOWN : pss;
    DEBUGLOG(4, "ZSTDN_initCStream_srcSize");
    FORWARD_IF_ERROR( ZSTDN_CCtx_reset(zcs, ZSTDN_reset_session_only) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_refCDict(zcs, NULL) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_setParameter(zcs, ZSTDN_c_compressionLevel, compressionLevel) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize) , "");
    return 0;
}

size_t ZSTDN_initCStream(ZSTDN_CStream* zcs, int compressionLevel)
{
    DEBUGLOG(4, "ZSTDN_initCStream");
    FORWARD_IF_ERROR( ZSTDN_CCtx_reset(zcs, ZSTDN_reset_session_only) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_refCDict(zcs, NULL) , "");
    FORWARD_IF_ERROR( ZSTDN_CCtx_setParameter(zcs, ZSTDN_c_compressionLevel, compressionLevel) , "");
    return 0;
}

/*======   Compression   ======*/

static size_t ZSTDN_nextInputSizeHint(const ZSTDN_CCtx* cctx)
{
    size_t hintInSize = cctx->inBuffTarget - cctx->inBuffPos;
    if (hintInSize==0) hintInSize = cctx->blockSize;
    return hintInSize;
}

/* ZSTDN_compressStream_generic():
 *  internal function for all *compressStream*() variants
 *  non-static, because can be called from zstdmt_compress.c
 * @return : hint size for next input */
static size_t ZSTDN_compressStream_generic(ZSTDN_CStream* zcs,
                                          ZSTDN_outBuffer* output,
                                          ZSTDN_inBuffer* input,
                                          ZSTDN_EndDirective const flushMode)
{
    const char* const istart = (const char*)input->src;
    const char* const iend = input->size != 0 ? istart + input->size : istart;
    const char* ip = input->pos != 0 ? istart + input->pos : istart;
    char* const ostart = (char*)output->dst;
    char* const oend = output->size != 0 ? ostart + output->size : ostart;
    char* op = output->pos != 0 ? ostart + output->pos : ostart;
    U32 someMoreWork = 1;

    /* check expectations */
    DEBUGLOG(5, "ZSTDN_compressStream_generic, flush=%u", (unsigned)flushMode);
    if (zcs->appliedParams.inBufferMode == ZSTDN_bm_buffered) {
        assert(zcs->inBuff != NULL);
        assert(zcs->inBuffSize > 0);
    }
    if (zcs->appliedParams.outBufferMode == ZSTDN_bm_buffered) {
        assert(zcs->outBuff !=  NULL);
        assert(zcs->outBuffSize > 0);
    }
    assert(output->pos <= output->size);
    assert(input->pos <= input->size);
    assert((U32)flushMode <= (U32)ZSTDN_e_end);

    while (someMoreWork) {
        switch(zcs->streamStage)
        {
        case zcss_init:
            RETURN_ERROR(init_missing, "call ZSTDN_initCStream() first!");

        case zcss_load:
            if ( (flushMode == ZSTDN_e_end)
              && ( (size_t)(oend-op) >= ZSTDN_compressBound(iend-ip)     /* Enough output space */
                || zcs->appliedParams.outBufferMode == ZSTDN_bm_stable)  /* OR we are allowed to return dstSizeTooSmall */
              && (zcs->inBuffPos == 0) ) {
                /* shortcut to compression pass directly into output buffer */
                size_t const cSize = ZSTDN_compressEnd(zcs,
                                                op, oend-op, ip, iend-ip);
                DEBUGLOG(4, "ZSTDN_compressEnd : cSize=%u", (unsigned)cSize);
                FORWARD_IF_ERROR(cSize, "ZSTDN_compressEnd failed");
                ip = iend;
                op += cSize;
                zcs->frameEnded = 1;
                ZSTDN_CCtx_reset(zcs, ZSTDN_reset_session_only);
                someMoreWork = 0; break;
            }
            /* complete loading into inBuffer in buffered mode */
            if (zcs->appliedParams.inBufferMode == ZSTDN_bm_buffered) {
                size_t const toLoad = zcs->inBuffTarget - zcs->inBuffPos;
                size_t const loaded = ZSTDN_limitCopy(
                                        zcs->inBuff + zcs->inBuffPos, toLoad,
                                        ip, iend-ip);
                zcs->inBuffPos += loaded;
                if (loaded != 0)
                    ip += loaded;
                if ( (flushMode == ZSTDN_e_continue)
                  && (zcs->inBuffPos < zcs->inBuffTarget) ) {
                    /* not enough input to fill full block : stop here */
                    someMoreWork = 0; break;
                }
                if ( (flushMode == ZSTDN_e_flush)
                  && (zcs->inBuffPos == zcs->inToCompress) ) {
                    /* empty */
                    someMoreWork = 0; break;
                }
            }
            /* compress current block (note : this stage cannot be stopped in the middle) */
            DEBUGLOG(5, "stream compression stage (flushMode==%u)", flushMode);
            {   int const inputBuffered = (zcs->appliedParams.inBufferMode == ZSTDN_bm_buffered);
                void* cDst;
                size_t cSize;
                size_t oSize = oend-op;
                size_t const iSize = inputBuffered
                    ? zcs->inBuffPos - zcs->inToCompress
                    : MIN((size_t)(iend - ip), zcs->blockSize);
                if (oSize >= ZSTDN_compressBound(iSize) || zcs->appliedParams.outBufferMode == ZSTDN_bm_stable)
                    cDst = op;   /* compress into output buffer, to skip flush stage */
                else
                    cDst = zcs->outBuff, oSize = zcs->outBuffSize;
                if (inputBuffered) {
                    unsigned const lastBlock = (flushMode == ZSTDN_e_end) && (ip==iend);
                    cSize = lastBlock ?
                            ZSTDN_compressEnd(zcs, cDst, oSize,
                                        zcs->inBuff + zcs->inToCompress, iSize) :
                            ZSTDN_compressContinue(zcs, cDst, oSize,
                                        zcs->inBuff + zcs->inToCompress, iSize);
                    FORWARD_IF_ERROR(cSize, "%s", lastBlock ? "ZSTDN_compressEnd failed" : "ZSTDN_compressContinue failed");
                    zcs->frameEnded = lastBlock;
                    /* prepare next block */
                    zcs->inBuffTarget = zcs->inBuffPos + zcs->blockSize;
                    if (zcs->inBuffTarget > zcs->inBuffSize)
                        zcs->inBuffPos = 0, zcs->inBuffTarget = zcs->blockSize;
                    DEBUGLOG(5, "inBuffTarget:%u / inBuffSize:%u",
                            (unsigned)zcs->inBuffTarget, (unsigned)zcs->inBuffSize);
                    if (!lastBlock)
                        assert(zcs->inBuffTarget <= zcs->inBuffSize);
                    zcs->inToCompress = zcs->inBuffPos;
                } else {
                    unsigned const lastBlock = (ip + iSize == iend);
                    assert(flushMode == ZSTDN_e_end /* Already validated */);
                    cSize = lastBlock ?
                            ZSTDN_compressEnd(zcs, cDst, oSize, ip, iSize) :
                            ZSTDN_compressContinue(zcs, cDst, oSize, ip, iSize);
                    /* Consume the input prior to error checking to mirror buffered mode. */
                    if (iSize > 0)
                        ip += iSize;
                    FORWARD_IF_ERROR(cSize, "%s", lastBlock ? "ZSTDN_compressEnd failed" : "ZSTDN_compressContinue failed");
                    zcs->frameEnded = lastBlock;
                    if (lastBlock)
                        assert(ip == iend);
                }
                if (cDst == op) {  /* no need to flush */
                    op += cSize;
                    if (zcs->frameEnded) {
                        DEBUGLOG(5, "Frame completed directly in outBuffer");
                        someMoreWork = 0;
                        ZSTDN_CCtx_reset(zcs, ZSTDN_reset_session_only);
                    }
                    break;
                }
                zcs->outBuffContentSize = cSize;
                zcs->outBuffFlushedSize = 0;
                zcs->streamStage = zcss_flush; /* pass-through to flush stage */
            }
	    ZSTDN_FALLTHROUGH;
        case zcss_flush:
            DEBUGLOG(5, "flush stage");
            assert(zcs->appliedParams.outBufferMode == ZSTDN_bm_buffered);
            {   size_t const toFlush = zcs->outBuffContentSize - zcs->outBuffFlushedSize;
                size_t const flushed = ZSTDN_limitCopy(op, (size_t)(oend-op),
                            zcs->outBuff + zcs->outBuffFlushedSize, toFlush);
                DEBUGLOG(5, "toFlush: %u into %u ==> flushed: %u",
                            (unsigned)toFlush, (unsigned)(oend-op), (unsigned)flushed);
                if (flushed)
                    op += flushed;
                zcs->outBuffFlushedSize += flushed;
                if (toFlush!=flushed) {
                    /* flush not fully completed, presumably because dst is too small */
                    assert(op==oend);
                    someMoreWork = 0;
                    break;
                }
                zcs->outBuffContentSize = zcs->outBuffFlushedSize = 0;
                if (zcs->frameEnded) {
                    DEBUGLOG(5, "Frame completed on flush");
                    someMoreWork = 0;
                    ZSTDN_CCtx_reset(zcs, ZSTDN_reset_session_only);
                    break;
                }
                zcs->streamStage = zcss_load;
                break;
            }

        default: /* impossible */
            assert(0);
        }
    }

    input->pos = ip - istart;
    output->pos = op - ostart;
    if (zcs->frameEnded) return 0;
    return ZSTDN_nextInputSizeHint(zcs);
}

static size_t ZSTDN_nextInputSizeHint_MTorST(const ZSTDN_CCtx* cctx)
{
    return ZSTDN_nextInputSizeHint(cctx);

}

size_t ZSTDN_compressStream(ZSTDN_CStream* zcs, ZSTDN_outBuffer* output, ZSTDN_inBuffer* input)
{
    FORWARD_IF_ERROR( ZSTDN_compressStream2(zcs, output, input, ZSTDN_e_continue) , "");
    return ZSTDN_nextInputSizeHint_MTorST(zcs);
}

/* After a compression call set the expected input/output buffer.
 * This is validated at the start of the next compression call.
 */
static void ZSTDN_setBufferExpectations(ZSTDN_CCtx* cctx, ZSTDN_outBuffer const* output, ZSTDN_inBuffer const* input)
{
    if (cctx->appliedParams.inBufferMode == ZSTDN_bm_stable) {
        cctx->expectedInBuffer = *input;
    }
    if (cctx->appliedParams.outBufferMode == ZSTDN_bm_stable) {
        cctx->expectedOutBufferSize = output->size - output->pos;
    }
}

/* Validate that the input/output buffers match the expectations set by
 * ZSTDN_setBufferExpectations.
 */
static size_t ZSTDN_checkBufferStability(ZSTDN_CCtx const* cctx,
                                        ZSTDN_outBuffer const* output,
                                        ZSTDN_inBuffer const* input,
                                        ZSTDN_EndDirective endOp)
{
    if (cctx->appliedParams.inBufferMode == ZSTDN_bm_stable) {
        ZSTDN_inBuffer const expect = cctx->expectedInBuffer;
        if (expect.src != input->src || expect.pos != input->pos || expect.size != input->size)
            RETURN_ERROR(srcBuffer_wrong, "ZSTDN_c_stableInBuffer enabled but input differs!");
        if (endOp != ZSTDN_e_end)
            RETURN_ERROR(srcBuffer_wrong, "ZSTDN_c_stableInBuffer can only be used with ZSTDN_e_end!");
    }
    if (cctx->appliedParams.outBufferMode == ZSTDN_bm_stable) {
        size_t const outBufferSize = output->size - output->pos;
        if (cctx->expectedOutBufferSize != outBufferSize)
            RETURN_ERROR(dstBuffer_wrong, "ZSTDN_c_stableOutBuffer enabled but output size differs!");
    }
    return 0;
}

static size_t ZSTDN_CCtx_init_compressStream2(ZSTDN_CCtx* cctx,
                                             ZSTDN_EndDirective endOp,
                                             size_t inSize) {
    ZSTDN_CCtx_params params = cctx->requestedParams;
    ZSTDN_prefixDict const prefixDict = cctx->prefixDict;
    FORWARD_IF_ERROR( ZSTDN_initLocalDict(cctx) , ""); /* Init the local dict if present. */
    ZSTDN_memset(&cctx->prefixDict, 0, sizeof(cctx->prefixDict));   /* single usage */
    assert(prefixDict.dict==NULL || cctx->cdict==NULL);    /* only one can be set */
    if (cctx->cdict)
        params.compressionLevel = cctx->cdict->compressionLevel; /* let cdict take priority in terms of compression level */
    DEBUGLOG(4, "ZSTDN_compressStream2 : transparent init stage");
    if (endOp == ZSTDN_e_end) cctx->pledgedSrcSizePlusOne = inSize + 1;  /* auto-fix pledgedSrcSize */
    {
        size_t const dictSize = prefixDict.dict
                ? prefixDict.dictSize
                : (cctx->cdict ? cctx->cdict->dictContentSize : 0);
        ZSTDN_cParamMode_e const mode = ZSTDN_getCParamMode(cctx->cdict, &params, cctx->pledgedSrcSizePlusOne - 1);
        params.cParams = ZSTDN_getCParamsFromCCtxParams(
                &params, cctx->pledgedSrcSizePlusOne-1,
                dictSize, mode);
    }

    if (ZSTDN_CParams_shouldEnableLdm(&params.cParams)) {
        /* Enable LDM by default for optimal parser and window size >= 128MB */
        DEBUGLOG(4, "LDM enabled by default (window size >= 128MB, strategy >= btopt)");
        params.ldmParams.enableLdm = 1;
    }

    {   U64 const pledgedSrcSize = cctx->pledgedSrcSizePlusOne - 1;
        assert(!ZSTDN_isError(ZSTDN_checkCParams(params.cParams)));
        FORWARD_IF_ERROR( ZSTDN_compressBegin_internal(cctx,
                prefixDict.dict, prefixDict.dictSize, prefixDict.dictContentType, ZSTDN_dtlm_fast,
                cctx->cdict,
                &params, pledgedSrcSize,
                ZSTDb_buffered) , "");
        assert(cctx->appliedParams.nbWorkers == 0);
        cctx->inToCompress = 0;
        cctx->inBuffPos = 0;
        if (cctx->appliedParams.inBufferMode == ZSTDN_bm_buffered) {
            /* for small input: avoid automatic flush on reaching end of block, since
            * it would require to add a 3-bytes null block to end frame
            */
            cctx->inBuffTarget = cctx->blockSize + (cctx->blockSize == pledgedSrcSize);
        } else {
            cctx->inBuffTarget = 0;
        }
        cctx->outBuffContentSize = cctx->outBuffFlushedSize = 0;
        cctx->streamStage = zcss_load;
        cctx->frameEnded = 0;
    }
    return 0;
}

size_t ZSTDN_compressStream2( ZSTDN_CCtx* cctx,
                             ZSTDN_outBuffer* output,
                             ZSTDN_inBuffer* input,
                             ZSTDN_EndDirective endOp)
{
    DEBUGLOG(5, "ZSTDN_compressStream2, endOp=%u ", (unsigned)endOp);
    /* check conditions */
    RETURN_ERROR_IF(output->pos > output->size, dstSize_tooSmall, "invalid output buffer");
    RETURN_ERROR_IF(input->pos  > input->size, srcSize_wrong, "invalid input buffer");
    RETURN_ERROR_IF((U32)endOp > (U32)ZSTDN_e_end, parameter_outOfBound, "invalid endDirective");
    assert(cctx != NULL);

    /* transparent initialization stage */
    if (cctx->streamStage == zcss_init) {
        FORWARD_IF_ERROR(ZSTDN_CCtx_init_compressStream2(cctx, endOp, input->size), "CompressStream2 initialization failed");
        ZSTDN_setBufferExpectations(cctx, output, input);    /* Set initial buffer expectations now that we've initialized */
    }
    /* end of transparent initialization stage */

    FORWARD_IF_ERROR(ZSTDN_checkBufferStability(cctx, output, input, endOp), "invalid buffers");
    /* compression stage */
    FORWARD_IF_ERROR( ZSTDN_compressStream_generic(cctx, output, input, endOp) , "");
    DEBUGLOG(5, "completed ZSTDN_compressStream2");
    ZSTDN_setBufferExpectations(cctx, output, input);
    return cctx->outBuffContentSize - cctx->outBuffFlushedSize; /* remaining to flush */
}

size_t ZSTDN_compressStream2_simpleArgs (
                            ZSTDN_CCtx* cctx,
                            void* dst, size_t dstCapacity, size_t* dstPos,
                      const void* src, size_t srcSize, size_t* srcPos,
                            ZSTDN_EndDirective endOp)
{
    ZSTDN_outBuffer output = { dst, dstCapacity, *dstPos };
    ZSTDN_inBuffer  input  = { src, srcSize, *srcPos };
    /* ZSTDN_compressStream2() will check validity of dstPos and srcPos */
    size_t const cErr = ZSTDN_compressStream2(cctx, &output, &input, endOp);
    *dstPos = output.pos;
    *srcPos = input.pos;
    return cErr;
}

size_t ZSTDN_compress2(ZSTDN_CCtx* cctx,
                      void* dst, size_t dstCapacity,
                      const void* src, size_t srcSize)
{
    ZSTDN_bufferMode_e const originalInBufferMode = cctx->requestedParams.inBufferMode;
    ZSTDN_bufferMode_e const originalOutBufferMode = cctx->requestedParams.outBufferMode;
    DEBUGLOG(4, "ZSTDN_compress2 (srcSize=%u)", (unsigned)srcSize);
    ZSTDN_CCtx_reset(cctx, ZSTDN_reset_session_only);
    /* Enable stable input/output buffers. */
    cctx->requestedParams.inBufferMode = ZSTDN_bm_stable;
    cctx->requestedParams.outBufferMode = ZSTDN_bm_stable;
    {   size_t oPos = 0;
        size_t iPos = 0;
        size_t const result = ZSTDN_compressStream2_simpleArgs(cctx,
                                        dst, dstCapacity, &oPos,
                                        src, srcSize, &iPos,
                                        ZSTDN_e_end);
        /* Reset to the original values. */
        cctx->requestedParams.inBufferMode = originalInBufferMode;
        cctx->requestedParams.outBufferMode = originalOutBufferMode;
        FORWARD_IF_ERROR(result, "ZSTDN_compressStream2_simpleArgs failed");
        if (result != 0) {  /* compression not completed, due to lack of output space */
            assert(oPos == dstCapacity);
            RETURN_ERROR(dstSize_tooSmall, "");
        }
        assert(iPos == srcSize);   /* all input is expected consumed */
        return oPos;
    }
}

typedef struct {
    U32 idx;             /* Index in array of ZSTDN_Sequence */
    U32 posInSequence;   /* Position within sequence at idx */
    size_t posInSrc;        /* Number of bytes given by sequences provided so far */
} ZSTDN_sequencePosition;

/* Returns a ZSTD error code if sequence is not valid */
static size_t ZSTDN_validateSequence(U32 offCode, U32 matchLength,
                                    size_t posInSrc, U32 windowLog, size_t dictSize, U32 minMatch) {
    size_t offsetBound;
    U32 windowSize = 1 << windowLog;
    /* posInSrc represents the amount of data the the decoder would decode up to this point.
     * As long as the amount of data decoded is less than or equal to window size, offsets may be
     * larger than the total length of output decoded in order to reference the dict, even larger than
     * window size. After output surpasses windowSize, we're limited to windowSize offsets again.
     */
    offsetBound = posInSrc > windowSize ? (size_t)windowSize : posInSrc + (size_t)dictSize;
    RETURN_ERROR_IF(offCode > offsetBound + ZSTDN_REP_MOVE, corruption_detected, "Offset too large!");
    RETURN_ERROR_IF(matchLength < minMatch, corruption_detected, "Matchlength too small");
    return 0;
}

/* Returns an offset code, given a sequence's raw offset, the ongoing repcode array, and whether litLength == 0 */
static U32 ZSTDN_finalizeOffCode(U32 rawOffset, const U32 rep[ZSTDN_REP_NUM], U32 ll0) {
    U32 offCode = rawOffset + ZSTDN_REP_MOVE;
    U32 repCode = 0;

    if (!ll0 && rawOffset == rep[0]) {
        repCode = 1;
    } else if (rawOffset == rep[1]) {
        repCode = 2 - ll0;
    } else if (rawOffset == rep[2]) {
        repCode = 3 - ll0;
    } else if (ll0 && rawOffset == rep[0] - 1) {
        repCode = 3;
    }
    if (repCode) {
        /* ZSTDN_storeSeq expects a number in the range [0, 2] to represent a repcode */
        offCode = repCode - 1;
    }
    return offCode;
}

/* Returns 0 on success, and a ZSTDN_error otherwise. This function scans through an array of
 * ZSTDN_Sequence, storing the sequences it finds, until it reaches a block delimiter.
 */
static size_t ZSTDN_copySequencesToSeqStoreExplicitBlockDelim(ZSTDN_CCtx* cctx, ZSTDN_sequencePosition* seqPos,
                                                             const ZSTDN_Sequence* const inSeqs, size_t inSeqsSize,
                                                             const void* src, size_t blockSize) {
    U32 idx = seqPos->idx;
    BYTE const* ip = (BYTE const*)(src);
    const BYTE* const iend = ip + blockSize;
    repcodes_t updatedRepcodes;
    U32 dictSize;
    U32 litLength;
    U32 matchLength;
    U32 ll0;
    U32 offCode;

    if (cctx->cdict) {
        dictSize = (U32)cctx->cdict->dictContentSize;
    } else if (cctx->prefixDict.dict) {
        dictSize = (U32)cctx->prefixDict.dictSize;
    } else {
        dictSize = 0;
    }
    ZSTDN_memcpy(updatedRepcodes.rep, cctx->blockState.prevCBlock->rep, sizeof(repcodes_t));
    for (; (inSeqs[idx].matchLength != 0 || inSeqs[idx].offset != 0) && idx < inSeqsSize; ++idx) {
        litLength = inSeqs[idx].litLength;
        matchLength = inSeqs[idx].matchLength;
        ll0 = litLength == 0;
        offCode = ZSTDN_finalizeOffCode(inSeqs[idx].offset, updatedRepcodes.rep, ll0);
        updatedRepcodes = ZSTDN_updateRep(updatedRepcodes.rep, offCode, ll0);

        DEBUGLOG(6, "Storing sequence: (of: %u, ml: %u, ll: %u)", offCode, matchLength, litLength);
        if (cctx->appliedParams.validateSequences) {
            seqPos->posInSrc += litLength + matchLength;
            FORWARD_IF_ERROR(ZSTDN_validateSequence(offCode, matchLength, seqPos->posInSrc,
                                                cctx->appliedParams.cParams.windowLog, dictSize,
                                                cctx->appliedParams.cParams.minMatch),
                                                "Sequence validation failed");
        }
        RETURN_ERROR_IF(idx - seqPos->idx > cctx->seqStore.maxNbSeq, memory_allocation,
                        "Not enough memory allocated. Try adjusting ZSTDN_c_minMatch.");
        ZSTDN_storeSeq(&cctx->seqStore, litLength, ip, iend, offCode, matchLength - MINMATCH);
        ip += matchLength + litLength;
    }
    ZSTDN_memcpy(cctx->blockState.nextCBlock->rep, updatedRepcodes.rep, sizeof(repcodes_t));

    if (inSeqs[idx].litLength) {
        DEBUGLOG(6, "Storing last literals of size: %u", inSeqs[idx].litLength);
        ZSTDN_storeLastLiterals(&cctx->seqStore, ip, inSeqs[idx].litLength);
        ip += inSeqs[idx].litLength;
        seqPos->posInSrc += inSeqs[idx].litLength;
    }
    RETURN_ERROR_IF(ip != iend, corruption_detected, "Blocksize doesn't agree with block delimiter!");
    seqPos->idx = idx+1;
    return 0;
}

/* Returns the number of bytes to move the current read position back by. Only non-zero
 * if we ended up splitting a sequence. Otherwise, it may return a ZSTD error if something
 * went wrong.
 *
 * This function will attempt to scan through blockSize bytes represented by the sequences
 * in inSeqs, storing any (partial) sequences.
 *
 * Occasionally, we may want to change the actual number of bytes we consumed from inSeqs to
 * avoid splitting a match, or to avoid splitting a match such that it would produce a match
 * smaller than MINMATCH. In this case, we return the number of bytes that we didn't read from this block.
 */
static size_t ZSTDN_copySequencesToSeqStoreNoBlockDelim(ZSTDN_CCtx* cctx, ZSTDN_sequencePosition* seqPos,
                                                       const ZSTDN_Sequence* const inSeqs, size_t inSeqsSize,
                                                       const void* src, size_t blockSize) {
    U32 idx = seqPos->idx;
    U32 startPosInSequence = seqPos->posInSequence;
    U32 endPosInSequence = seqPos->posInSequence + (U32)blockSize;
    size_t dictSize;
    BYTE const* ip = (BYTE const*)(src);
    BYTE const* iend = ip + blockSize;  /* May be adjusted if we decide to process fewer than blockSize bytes */
    repcodes_t updatedRepcodes;
    U32 bytesAdjustment = 0;
    U32 finalMatchSplit = 0;
    U32 litLength;
    U32 matchLength;
    U32 rawOffset;
    U32 offCode;

    if (cctx->cdict) {
        dictSize = cctx->cdict->dictContentSize;
    } else if (cctx->prefixDict.dict) {
        dictSize = cctx->prefixDict.dictSize;
    } else {
        dictSize = 0;
    }
    DEBUGLOG(5, "ZSTDN_copySequencesToSeqStore: idx: %u PIS: %u blockSize: %zu", idx, startPosInSequence, blockSize);
    DEBUGLOG(5, "Start seq: idx: %u (of: %u ml: %u ll: %u)", idx, inSeqs[idx].offset, inSeqs[idx].matchLength, inSeqs[idx].litLength);
    ZSTDN_memcpy(updatedRepcodes.rep, cctx->blockState.prevCBlock->rep, sizeof(repcodes_t));
    while (endPosInSequence && idx < inSeqsSize && !finalMatchSplit) {
        const ZSTDN_Sequence currSeq = inSeqs[idx];
        litLength = currSeq.litLength;
        matchLength = currSeq.matchLength;
        rawOffset = currSeq.offset;

        /* Modify the sequence depending on where endPosInSequence lies */
        if (endPosInSequence >= currSeq.litLength + currSeq.matchLength) {
            if (startPosInSequence >= litLength) {
                startPosInSequence -= litLength;
                litLength = 0;
                matchLength -= startPosInSequence;
            } else {
                litLength -= startPosInSequence;
            }
            /* Move to the next sequence */
            endPosInSequence -= currSeq.litLength + currSeq.matchLength;
            startPosInSequence = 0;
            idx++;
        } else {
            /* This is the final (partial) sequence we're adding from inSeqs, and endPosInSequence
               does not reach the end of the match. So, we have to split the sequence */
            DEBUGLOG(6, "Require a split: diff: %u, idx: %u PIS: %u",
                     currSeq.litLength + currSeq.matchLength - endPosInSequence, idx, endPosInSequence);
            if (endPosInSequence > litLength) {
                U32 firstHalfMatchLength;
                litLength = startPosInSequence >= litLength ? 0 : litLength - startPosInSequence;
                firstHalfMatchLength = endPosInSequence - startPosInSequence - litLength;
                if (matchLength > blockSize && firstHalfMatchLength >= cctx->appliedParams.cParams.minMatch) {
                    /* Only ever split the match if it is larger than the block size */
                    U32 secondHalfMatchLength = currSeq.matchLength + currSeq.litLength - endPosInSequence;
                    if (secondHalfMatchLength < cctx->appliedParams.cParams.minMatch) {
                        /* Move the endPosInSequence backward so that it creates match of minMatch length */
                        endPosInSequence -= cctx->appliedParams.cParams.minMatch - secondHalfMatchLength;
                        bytesAdjustment = cctx->appliedParams.cParams.minMatch - secondHalfMatchLength;
                        firstHalfMatchLength -= bytesAdjustment;
                    }
                    matchLength = firstHalfMatchLength;
                    /* Flag that we split the last match - after storing the sequence, exit the loop,
                       but keep the value of endPosInSequence */
                    finalMatchSplit = 1;
                } else {
                    /* Move the position in sequence backwards so that we don't split match, and break to store
                     * the last literals. We use the original currSeq.litLength as a marker for where endPosInSequence
                     * should go. We prefer to do this whenever it is not necessary to split the match, or if doing so
                     * would cause the first half of the match to be too small
                     */
                    bytesAdjustment = endPosInSequence - currSeq.litLength;
                    endPosInSequence = currSeq.litLength;
                    break;
                }
            } else {
                /* This sequence ends inside the literals, break to store the last literals */
                break;
            }
        }
        /* Check if this offset can be represented with a repcode */
        {   U32 ll0 = (litLength == 0);
            offCode = ZSTDN_finalizeOffCode(rawOffset, updatedRepcodes.rep, ll0);
            updatedRepcodes = ZSTDN_updateRep(updatedRepcodes.rep, offCode, ll0);
        }

        if (cctx->appliedParams.validateSequences) {
            seqPos->posInSrc += litLength + matchLength;
            FORWARD_IF_ERROR(ZSTDN_validateSequence(offCode, matchLength, seqPos->posInSrc,
                                                   cctx->appliedParams.cParams.windowLog, dictSize,
                                                   cctx->appliedParams.cParams.minMatch),
                                                   "Sequence validation failed");
        }
        DEBUGLOG(6, "Storing sequence: (of: %u, ml: %u, ll: %u)", offCode, matchLength, litLength);
        RETURN_ERROR_IF(idx - seqPos->idx > cctx->seqStore.maxNbSeq, memory_allocation,
                        "Not enough memory allocated. Try adjusting ZSTDN_c_minMatch.");
        ZSTDN_storeSeq(&cctx->seqStore, litLength, ip, iend, offCode, matchLength - MINMATCH);
        ip += matchLength + litLength;
    }
    DEBUGLOG(5, "Ending seq: idx: %u (of: %u ml: %u ll: %u)", idx, inSeqs[idx].offset, inSeqs[idx].matchLength, inSeqs[idx].litLength);
    assert(idx == inSeqsSize || endPosInSequence <= inSeqs[idx].litLength + inSeqs[idx].matchLength);
    seqPos->idx = idx;
    seqPos->posInSequence = endPosInSequence;
    ZSTDN_memcpy(cctx->blockState.nextCBlock->rep, updatedRepcodes.rep, sizeof(repcodes_t));

    iend -= bytesAdjustment;
    if (ip != iend) {
        /* Store any last literals */
        U32 lastLLSize = (U32)(iend - ip);
        assert(ip <= iend);
        DEBUGLOG(6, "Storing last literals of size: %u", lastLLSize);
        ZSTDN_storeLastLiterals(&cctx->seqStore, ip, lastLLSize);
        seqPos->posInSrc += lastLLSize;
    }

    return bytesAdjustment;
}

typedef size_t (*ZSTDN_sequenceCopier) (ZSTDN_CCtx* cctx, ZSTDN_sequencePosition* seqPos,
                                       const ZSTDN_Sequence* const inSeqs, size_t inSeqsSize,
                                       const void* src, size_t blockSize);
static ZSTDN_sequenceCopier ZSTDN_selectSequenceCopier(ZSTDN_sequenceFormat_e mode) {
    ZSTDN_sequenceCopier sequenceCopier = NULL;
    assert(ZSTDN_cParam_withinBounds(ZSTDN_c_blockDelimiters, mode));
    if (mode == ZSTDN_sf_explicitBlockDelimiters) {
        return ZSTDN_copySequencesToSeqStoreExplicitBlockDelim;
    } else if (mode == ZSTDN_sf_noBlockDelimiters) {
        return ZSTDN_copySequencesToSeqStoreNoBlockDelim;
    }
    assert(sequenceCopier != NULL);
    return sequenceCopier;
}

/* Compress, block-by-block, all of the sequences given.
 *
 * Returns the cumulative size of all compressed blocks (including their headers), otherwise a ZSTD error.
 */
static size_t ZSTDN_compressSequences_internal(ZSTDN_CCtx* cctx,
                                              void* dst, size_t dstCapacity,
                                              const ZSTDN_Sequence* inSeqs, size_t inSeqsSize,
                                              const void* src, size_t srcSize) {
    size_t cSize = 0;
    U32 lastBlock;
    size_t blockSize;
    size_t compressedSeqsSize;
    size_t remaining = srcSize;
    ZSTDN_sequencePosition seqPos = {0, 0, 0};

    BYTE const* ip = (BYTE const*)src;
    BYTE* op = (BYTE*)dst;
    ZSTDN_sequenceCopier sequenceCopier = ZSTDN_selectSequenceCopier(cctx->appliedParams.blockDelimiters);

    DEBUGLOG(4, "ZSTDN_compressSequences_internal srcSize: %zu, inSeqsSize: %zu", srcSize, inSeqsSize);
    /* Special case: empty frame */
    if (remaining == 0) {
        U32 const cBlockHeader24 = 1 /* last block */ + (((U32)bt_raw)<<1);
        RETURN_ERROR_IF(dstCapacity<4, dstSize_tooSmall, "No room for empty frame block header");
        MEM_writeLE32(op, cBlockHeader24);
        op += ZSTDN_blockHeaderSize;
        dstCapacity -= ZSTDN_blockHeaderSize;
        cSize += ZSTDN_blockHeaderSize;
    }

    while (remaining) {
        size_t cBlockSize;
        size_t additionalByteAdjustment;
        lastBlock = remaining <= cctx->blockSize;
        blockSize = lastBlock ? (U32)remaining : (U32)cctx->blockSize;
        ZSTDN_resetSeqStore(&cctx->seqStore);
        DEBUGLOG(4, "Working on new block. Blocksize: %zu", blockSize);

        additionalByteAdjustment = sequenceCopier(cctx, &seqPos, inSeqs, inSeqsSize, ip, blockSize);
        FORWARD_IF_ERROR(additionalByteAdjustment, "Bad sequence copy");
        blockSize -= additionalByteAdjustment;

        /* If blocks are too small, emit as a nocompress block */
        if (blockSize < MIN_CBLOCK_SIZE+ZSTDN_blockHeaderSize+1) {
            cBlockSize = ZSTDN_noCompressBlock(op, dstCapacity, ip, blockSize, lastBlock);
            FORWARD_IF_ERROR(cBlockSize, "Nocompress block failed");
            DEBUGLOG(4, "Block too small, writing out nocompress block: cSize: %zu", cBlockSize);
            cSize += cBlockSize;
            ip += blockSize;
            op += cBlockSize;
            remaining -= blockSize;
            dstCapacity -= cBlockSize;
            continue;
        }

        compressedSeqsSize = ZSTDN_entropyCompressSequences(&cctx->seqStore,
                                &cctx->blockState.prevCBlock->entropy, &cctx->blockState.nextCBlock->entropy,
                                &cctx->appliedParams,
                                op + ZSTDN_blockHeaderSize /* Leave space for block header */, dstCapacity - ZSTDN_blockHeaderSize,
                                blockSize,
                                cctx->entropyWorkspace, ENTROPY_WORKSPACE_SIZE /* statically allocated in resetCCtx */,
                                cctx->bmi2);
        FORWARD_IF_ERROR(compressedSeqsSize, "Compressing sequences of block failed");
        DEBUGLOG(4, "Compressed sequences size: %zu", compressedSeqsSize);

        if (!cctx->isFirstBlock &&
            ZSTDN_maybeRLE(&cctx->seqStore) &&
            ZSTDN_isRLE((BYTE const*)src, srcSize)) {
            /* We don't want to emit our first block as a RLE even if it qualifies because
            * doing so will cause the decoder (cli only) to throw a "should consume all input error."
            * This is only an issue for zstd <= v1.4.3
            */
            compressedSeqsSize = 1;
        }

        if (compressedSeqsSize == 0) {
            /* ZSTDN_noCompressBlock writes the block header as well */
            cBlockSize = ZSTDN_noCompressBlock(op, dstCapacity, ip, blockSize, lastBlock);
            FORWARD_IF_ERROR(cBlockSize, "Nocompress block failed");
            DEBUGLOG(4, "Writing out nocompress block, size: %zu", cBlockSize);
        } else if (compressedSeqsSize == 1) {
            cBlockSize = ZSTDN_rleCompressBlock(op, dstCapacity, *ip, blockSize, lastBlock);
            FORWARD_IF_ERROR(cBlockSize, "RLE compress block failed");
            DEBUGLOG(4, "Writing out RLE block, size: %zu", cBlockSize);
        } else {
            U32 cBlockHeader;
            /* Error checking and repcodes update */
            ZSTDN_confirmRepcodesAndEntropyTables(cctx);
            if (cctx->blockState.prevCBlock->entropy.fse.offcode_repeatMode == FSEN_repeat_valid)
                cctx->blockState.prevCBlock->entropy.fse.offcode_repeatMode = FSEN_repeat_check;

            /* Write block header into beginning of block*/
            cBlockHeader = lastBlock + (((U32)bt_compressed)<<1) + (U32)(compressedSeqsSize << 3);
            MEM_writeLE24(op, cBlockHeader);
            cBlockSize = ZSTDN_blockHeaderSize + compressedSeqsSize;
            DEBUGLOG(4, "Writing out compressed block, size: %zu", cBlockSize);
        }

        cSize += cBlockSize;
        DEBUGLOG(4, "cSize running total: %zu", cSize);

        if (lastBlock) {
            break;
        } else {
            ip += blockSize;
            op += cBlockSize;
            remaining -= blockSize;
            dstCapacity -= cBlockSize;
            cctx->isFirstBlock = 0;
        }
    }

    return cSize;
}

size_t ZSTDN_compressSequences(ZSTDN_CCtx* const cctx, void* dst, size_t dstCapacity,
                              const ZSTDN_Sequence* inSeqs, size_t inSeqsSize,
                              const void* src, size_t srcSize) {
    BYTE* op = (BYTE*)dst;
    size_t cSize = 0;
    size_t compressedBlocksSize = 0;
    size_t frameHeaderSize = 0;

    /* Transparent initialization stage, same as compressStream2() */
    DEBUGLOG(3, "ZSTDN_compressSequences()");
    assert(cctx != NULL);
    FORWARD_IF_ERROR(ZSTDN_CCtx_init_compressStream2(cctx, ZSTDN_e_end, srcSize), "CCtx initialization failed");
    /* Begin writing output, starting with frame header */
    frameHeaderSize = ZSTDN_writeFrameHeader(op, dstCapacity, &cctx->appliedParams, srcSize, cctx->dictID);
    op += frameHeaderSize;
    dstCapacity -= frameHeaderSize;
    cSize += frameHeaderSize;
    if (cctx->appliedParams.fParams.checksumFlag && srcSize) {
        xxh64n_update(&cctx->xxhState, src, srcSize);
    }
    /* cSize includes block header size and compressed sequences size */
    compressedBlocksSize = ZSTDN_compressSequences_internal(cctx,
                                                           op, dstCapacity,
                                                           inSeqs, inSeqsSize,
                                                           src, srcSize);
    FORWARD_IF_ERROR(compressedBlocksSize, "Compressing blocks failed!");
    cSize += compressedBlocksSize;
    dstCapacity -= compressedBlocksSize;

    if (cctx->appliedParams.fParams.checksumFlag) {
        U32 const checksum = (U32) xxh64n_digest(&cctx->xxhState);
        RETURN_ERROR_IF(dstCapacity<4, dstSize_tooSmall, "no room for checksum");
        DEBUGLOG(4, "Write checksum : %08X", (unsigned)checksum);
        MEM_writeLE32((char*)dst + cSize, checksum);
        cSize += 4;
    }

    DEBUGLOG(3, "Final compressed size: %zu", cSize);
    return cSize;
}

/*======   Finalize   ======*/

/*! ZSTDN_flushStream() :
 * @return : amount of data remaining to flush */
size_t ZSTDN_flushStream(ZSTDN_CStream* zcs, ZSTDN_outBuffer* output)
{
    ZSTDN_inBuffer input = { NULL, 0, 0 };
    return ZSTDN_compressStream2(zcs, output, &input, ZSTDN_e_flush);
}


size_t ZSTDN_endStream(ZSTDN_CStream* zcs, ZSTDN_outBuffer* output)
{
    ZSTDN_inBuffer input = { NULL, 0, 0 };
    size_t const remainingToFlush = ZSTDN_compressStream2(zcs, output, &input, ZSTDN_e_end);
    FORWARD_IF_ERROR( remainingToFlush , "ZSTDN_compressStream2 failed");
    if (zcs->appliedParams.nbWorkers > 0) return remainingToFlush;   /* minimal estimation */
    /* single thread mode : attempt to calculate remaining to flush more precisely */
    {   size_t const lastBlockSize = zcs->frameEnded ? 0 : ZSTDN_BLOCKHEADERSIZE;
        size_t const checksumSize = (size_t)(zcs->frameEnded ? 0 : zcs->appliedParams.fParams.checksumFlag * 4);
        size_t const toFlush = remainingToFlush + lastBlockSize + checksumSize;
        DEBUGLOG(4, "ZSTDN_endStream : remaining to flush : %u", (unsigned)toFlush);
        return toFlush;
    }
}


/*-=====  Pre-defined compression levels  =====-*/

#define ZSTDN_MAX_CLEVEL     22
int ZSTDN_maxCLevel(void) { return ZSTDN_MAX_CLEVEL; }
int ZSTDN_minCLevel(void) { return (int)-ZSTDN_TARGETLENGTH_MAX; }

static const ZSTDN_compressionParameters ZSTDN_defaultCParameters[4][ZSTDN_MAX_CLEVEL+1] = {
{   /* "default" - for any srcSize > 256 KB */
    /* W,  C,  H,  S,  L, TL, strat */
    { 19, 12, 13,  1,  6,  1, ZSTDN_fast    },  /* base for negative levels */
    { 19, 13, 14,  1,  7,  0, ZSTDN_fast    },  /* level  1 */
    { 20, 15, 16,  1,  6,  0, ZSTDN_fast    },  /* level  2 */
    { 21, 16, 17,  1,  5,  0, ZSTDN_dfast   },  /* level  3 */
    { 21, 18, 18,  1,  5,  0, ZSTDN_dfast   },  /* level  4 */
    { 21, 18, 19,  2,  5,  2, ZSTDN_greedy  },  /* level  5 */
    { 21, 19, 19,  3,  5,  4, ZSTDN_greedy  },  /* level  6 */
    { 21, 19, 19,  3,  5,  8, ZSTDN_lazy    },  /* level  7 */
    { 21, 19, 19,  3,  5, 16, ZSTDN_lazy2   },  /* level  8 */
    { 21, 19, 20,  4,  5, 16, ZSTDN_lazy2   },  /* level  9 */
    { 22, 20, 21,  4,  5, 16, ZSTDN_lazy2   },  /* level 10 */
    { 22, 21, 22,  4,  5, 16, ZSTDN_lazy2   },  /* level 11 */
    { 22, 21, 22,  5,  5, 16, ZSTDN_lazy2   },  /* level 12 */
    { 22, 21, 22,  5,  5, 32, ZSTDN_btlazy2 },  /* level 13 */
    { 22, 22, 23,  5,  5, 32, ZSTDN_btlazy2 },  /* level 14 */
    { 22, 23, 23,  6,  5, 32, ZSTDN_btlazy2 },  /* level 15 */
    { 22, 22, 22,  5,  5, 48, ZSTDN_btopt   },  /* level 16 */
    { 23, 23, 22,  5,  4, 64, ZSTDN_btopt   },  /* level 17 */
    { 23, 23, 22,  6,  3, 64, ZSTDN_btultra },  /* level 18 */
    { 23, 24, 22,  7,  3,256, ZSTDN_btultra2},  /* level 19 */
    { 25, 25, 23,  7,  3,256, ZSTDN_btultra2},  /* level 20 */
    { 26, 26, 24,  7,  3,512, ZSTDN_btultra2},  /* level 21 */
    { 27, 27, 25,  9,  3,999, ZSTDN_btultra2},  /* level 22 */
},
{   /* for srcSize <= 256 KB */
    /* W,  C,  H,  S,  L,  T, strat */
    { 18, 12, 13,  1,  5,  1, ZSTDN_fast    },  /* base for negative levels */
    { 18, 13, 14,  1,  6,  0, ZSTDN_fast    },  /* level  1 */
    { 18, 14, 14,  1,  5,  0, ZSTDN_dfast   },  /* level  2 */
    { 18, 16, 16,  1,  4,  0, ZSTDN_dfast   },  /* level  3 */
    { 18, 16, 17,  2,  5,  2, ZSTDN_greedy  },  /* level  4.*/
    { 18, 18, 18,  3,  5,  2, ZSTDN_greedy  },  /* level  5.*/
    { 18, 18, 19,  3,  5,  4, ZSTDN_lazy    },  /* level  6.*/
    { 18, 18, 19,  4,  4,  4, ZSTDN_lazy    },  /* level  7 */
    { 18, 18, 19,  4,  4,  8, ZSTDN_lazy2   },  /* level  8 */
    { 18, 18, 19,  5,  4,  8, ZSTDN_lazy2   },  /* level  9 */
    { 18, 18, 19,  6,  4,  8, ZSTDN_lazy2   },  /* level 10 */
    { 18, 18, 19,  5,  4, 12, ZSTDN_btlazy2 },  /* level 11.*/
    { 18, 19, 19,  7,  4, 12, ZSTDN_btlazy2 },  /* level 12.*/
    { 18, 18, 19,  4,  4, 16, ZSTDN_btopt   },  /* level 13 */
    { 18, 18, 19,  4,  3, 32, ZSTDN_btopt   },  /* level 14.*/
    { 18, 18, 19,  6,  3,128, ZSTDN_btopt   },  /* level 15.*/
    { 18, 19, 19,  6,  3,128, ZSTDN_btultra },  /* level 16.*/
    { 18, 19, 19,  8,  3,256, ZSTDN_btultra },  /* level 17.*/
    { 18, 19, 19,  6,  3,128, ZSTDN_btultra2},  /* level 18.*/
    { 18, 19, 19,  8,  3,256, ZSTDN_btultra2},  /* level 19.*/
    { 18, 19, 19, 10,  3,512, ZSTDN_btultra2},  /* level 20.*/
    { 18, 19, 19, 12,  3,512, ZSTDN_btultra2},  /* level 21.*/
    { 18, 19, 19, 13,  3,999, ZSTDN_btultra2},  /* level 22.*/
},
{   /* for srcSize <= 128 KB */
    /* W,  C,  H,  S,  L,  T, strat */
    { 17, 12, 12,  1,  5,  1, ZSTDN_fast    },  /* base for negative levels */
    { 17, 12, 13,  1,  6,  0, ZSTDN_fast    },  /* level  1 */
    { 17, 13, 15,  1,  5,  0, ZSTDN_fast    },  /* level  2 */
    { 17, 15, 16,  2,  5,  0, ZSTDN_dfast   },  /* level  3 */
    { 17, 17, 17,  2,  4,  0, ZSTDN_dfast   },  /* level  4 */
    { 17, 16, 17,  3,  4,  2, ZSTDN_greedy  },  /* level  5 */
    { 17, 17, 17,  3,  4,  4, ZSTDN_lazy    },  /* level  6 */
    { 17, 17, 17,  3,  4,  8, ZSTDN_lazy2   },  /* level  7 */
    { 17, 17, 17,  4,  4,  8, ZSTDN_lazy2   },  /* level  8 */
    { 17, 17, 17,  5,  4,  8, ZSTDN_lazy2   },  /* level  9 */
    { 17, 17, 17,  6,  4,  8, ZSTDN_lazy2   },  /* level 10 */
    { 17, 17, 17,  5,  4,  8, ZSTDN_btlazy2 },  /* level 11 */
    { 17, 18, 17,  7,  4, 12, ZSTDN_btlazy2 },  /* level 12 */
    { 17, 18, 17,  3,  4, 12, ZSTDN_btopt   },  /* level 13.*/
    { 17, 18, 17,  4,  3, 32, ZSTDN_btopt   },  /* level 14.*/
    { 17, 18, 17,  6,  3,256, ZSTDN_btopt   },  /* level 15.*/
    { 17, 18, 17,  6,  3,128, ZSTDN_btultra },  /* level 16.*/
    { 17, 18, 17,  8,  3,256, ZSTDN_btultra },  /* level 17.*/
    { 17, 18, 17, 10,  3,512, ZSTDN_btultra },  /* level 18.*/
    { 17, 18, 17,  5,  3,256, ZSTDN_btultra2},  /* level 19.*/
    { 17, 18, 17,  7,  3,512, ZSTDN_btultra2},  /* level 20.*/
    { 17, 18, 17,  9,  3,512, ZSTDN_btultra2},  /* level 21.*/
    { 17, 18, 17, 11,  3,999, ZSTDN_btultra2},  /* level 22.*/
},
{   /* for srcSize <= 16 KB */
    /* W,  C,  H,  S,  L,  T, strat */
    { 14, 12, 13,  1,  5,  1, ZSTDN_fast    },  /* base for negative levels */
    { 14, 14, 15,  1,  5,  0, ZSTDN_fast    },  /* level  1 */
    { 14, 14, 15,  1,  4,  0, ZSTDN_fast    },  /* level  2 */
    { 14, 14, 15,  2,  4,  0, ZSTDN_dfast   },  /* level  3 */
    { 14, 14, 14,  4,  4,  2, ZSTDN_greedy  },  /* level  4 */
    { 14, 14, 14,  3,  4,  4, ZSTDN_lazy    },  /* level  5.*/
    { 14, 14, 14,  4,  4,  8, ZSTDN_lazy2   },  /* level  6 */
    { 14, 14, 14,  6,  4,  8, ZSTDN_lazy2   },  /* level  7 */
    { 14, 14, 14,  8,  4,  8, ZSTDN_lazy2   },  /* level  8.*/
    { 14, 15, 14,  5,  4,  8, ZSTDN_btlazy2 },  /* level  9.*/
    { 14, 15, 14,  9,  4,  8, ZSTDN_btlazy2 },  /* level 10.*/
    { 14, 15, 14,  3,  4, 12, ZSTDN_btopt   },  /* level 11.*/
    { 14, 15, 14,  4,  3, 24, ZSTDN_btopt   },  /* level 12.*/
    { 14, 15, 14,  5,  3, 32, ZSTDN_btultra },  /* level 13.*/
    { 14, 15, 15,  6,  3, 64, ZSTDN_btultra },  /* level 14.*/
    { 14, 15, 15,  7,  3,256, ZSTDN_btultra },  /* level 15.*/
    { 14, 15, 15,  5,  3, 48, ZSTDN_btultra2},  /* level 16.*/
    { 14, 15, 15,  6,  3,128, ZSTDN_btultra2},  /* level 17.*/
    { 14, 15, 15,  7,  3,256, ZSTDN_btultra2},  /* level 18.*/
    { 14, 15, 15,  8,  3,256, ZSTDN_btultra2},  /* level 19.*/
    { 14, 15, 15,  8,  3,512, ZSTDN_btultra2},  /* level 20.*/
    { 14, 15, 15,  9,  3,512, ZSTDN_btultra2},  /* level 21.*/
    { 14, 15, 15, 10,  3,999, ZSTDN_btultra2},  /* level 22.*/
},
};

static ZSTDN_compressionParameters ZSTDN_dedicatedDictSearch_getCParams(int const compressionLevel, size_t const dictSize)
{
    ZSTDN_compressionParameters cParams = ZSTDN_getCParams_internal(compressionLevel, 0, dictSize, ZSTDN_cpm_createCDict);
    switch (cParams.strategy) {
        case ZSTDN_fast:
        case ZSTDN_dfast:
            break;
        case ZSTDN_greedy:
        case ZSTDN_lazy:
        case ZSTDN_lazy2:
            cParams.hashLog += ZSTDN_LAZY_DDSS_BUCKET_LOG;
            break;
        case ZSTDN_btlazy2:
        case ZSTDN_btopt:
        case ZSTDN_btultra:
        case ZSTDN_btultra2:
            break;
    }
    return cParams;
}

static int ZSTDN_dedicatedDictSearch_isSupported(
        ZSTDN_compressionParameters const* cParams)
{
    return (cParams->strategy >= ZSTDN_greedy)
        && (cParams->strategy <= ZSTDN_lazy2)
        && (cParams->hashLog >= cParams->chainLog)
        && (cParams->chainLog <= 24);
}

/*
 * Reverses the adjustment applied to cparams when enabling dedicated dict
 * search. This is used to recover the params set to be used in the working
 * context. (Otherwise, those tables would also grow.)
 */
static void ZSTDN_dedicatedDictSearch_revertCParams(
        ZSTDN_compressionParameters* cParams) {
    switch (cParams->strategy) {
        case ZSTDN_fast:
        case ZSTDN_dfast:
            break;
        case ZSTDN_greedy:
        case ZSTDN_lazy:
        case ZSTDN_lazy2:
            cParams->hashLog -= ZSTDN_LAZY_DDSS_BUCKET_LOG;
            break;
        case ZSTDN_btlazy2:
        case ZSTDN_btopt:
        case ZSTDN_btultra:
        case ZSTDN_btultra2:
            break;
    }
}

static U64 ZSTDN_getCParamRowSize(U64 srcSizeHint, size_t dictSize, ZSTDN_cParamMode_e mode)
{
    switch (mode) {
    case ZSTDN_cpm_unknown:
    case ZSTDN_cpm_noAttachDict:
    case ZSTDN_cpm_createCDict:
        break;
    case ZSTDN_cpm_attachDict:
        dictSize = 0;
        break;
    default:
        assert(0);
        break;
    }
    {   int const unknown = srcSizeHint == ZSTDN_CONTENTSIZE_UNKNOWN;
        size_t const addedSize = unknown && dictSize > 0 ? 500 : 0;
        return unknown && dictSize == 0 ? ZSTDN_CONTENTSIZE_UNKNOWN : srcSizeHint+dictSize+addedSize;
    }
}

/*! ZSTDN_getCParams_internal() :
 * @return ZSTDN_compressionParameters structure for a selected compression level, srcSize and dictSize.
 *  Note: srcSizeHint 0 means 0, use ZSTDN_CONTENTSIZE_UNKNOWN for unknown.
 *        Use dictSize == 0 for unknown or unused.
 *  Note: `mode` controls how we treat the `dictSize`. See docs for `ZSTDN_cParamMode_e`. */
static ZSTDN_compressionParameters ZSTDN_getCParams_internal(int compressionLevel, unsigned long long srcSizeHint, size_t dictSize, ZSTDN_cParamMode_e mode)
{
    U64 const rSize = ZSTDN_getCParamRowSize(srcSizeHint, dictSize, mode);
    U32 const tableID = (rSize <= 256 KB) + (rSize <= 128 KB) + (rSize <= 16 KB);
    int row;
    DEBUGLOG(5, "ZSTDN_getCParams_internal (cLevel=%i)", compressionLevel);

    /* row */
    if (compressionLevel == 0) row = ZSTDN_CLEVEL_DEFAULT;   /* 0 == default */
    else if (compressionLevel < 0) row = 0;   /* entry 0 is baseline for fast mode */
    else if (compressionLevel > ZSTDN_MAX_CLEVEL) row = ZSTDN_MAX_CLEVEL;
    else row = compressionLevel;

    {   ZSTDN_compressionParameters cp = ZSTDN_defaultCParameters[tableID][row];
        /* acceleration factor */
        if (compressionLevel < 0) {
            int const clampedCompressionLevel = MAX(ZSTDN_minCLevel(), compressionLevel);
            cp.targetLength = (unsigned)(-clampedCompressionLevel);
        }
        /* refine parameters based on srcSize & dictSize */
        return ZSTDN_adjustCParams_internal(cp, srcSizeHint, dictSize, mode);
    }
}

/*! ZSTDN_getCParams() :
 * @return ZSTDN_compressionParameters structure for a selected compression level, srcSize and dictSize.
 *  Size values are optional, provide 0 if not known or unused */
ZSTDN_compressionParameters ZSTDN_getCParams(int compressionLevel, unsigned long long srcSizeHint, size_t dictSize)
{
    if (srcSizeHint == 0) srcSizeHint = ZSTDN_CONTENTSIZE_UNKNOWN;
    return ZSTDN_getCParams_internal(compressionLevel, srcSizeHint, dictSize, ZSTDN_cpm_unknown);
}

/*! ZSTDN_getParams() :
 *  same idea as ZSTDN_getCParams()
 * @return a `ZSTDN_parameters` structure (instead of `ZSTDN_compressionParameters`).
 *  Fields of `ZSTDN_frameParameters` are set to default values */
static ZSTDN_parameters ZSTDN_getParams_internal(int compressionLevel, unsigned long long srcSizeHint, size_t dictSize, ZSTDN_cParamMode_e mode) {
    ZSTDN_parameters params;
    ZSTDN_compressionParameters const cParams = ZSTDN_getCParams_internal(compressionLevel, srcSizeHint, dictSize, mode);
    DEBUGLOG(5, "ZSTDN_getParams (cLevel=%i)", compressionLevel);
    ZSTDN_memset(&params, 0, sizeof(params));
    params.cParams = cParams;
    params.fParams.contentSizeFlag = 1;
    return params;
}

/*! ZSTDN_getParams() :
 *  same idea as ZSTDN_getCParams()
 * @return a `ZSTDN_parameters` structure (instead of `ZSTDN_compressionParameters`).
 *  Fields of `ZSTDN_frameParameters` are set to default values */
ZSTDN_parameters ZSTDN_getParams(int compressionLevel, unsigned long long srcSizeHint, size_t dictSize) {
    if (srcSizeHint == 0) srcSizeHint = ZSTDN_CONTENTSIZE_UNKNOWN;
    return ZSTDN_getParams_internal(compressionLevel, srcSizeHint, dictSize, ZSTDN_cpm_unknown);
}
