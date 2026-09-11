/* ******************************************************************
 * FSE : Finite State Entropy decoder
 * Copyright (c) Yann Collet, Facebook, Inc.
 *
 *  You can contact the author at :
 *  - FSE source repository : https://github.com/Cyan4973/FiniteStateEntropy
 *  - Public forum : https://groups.google.com/forum/#!forum/lz4c
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
****************************************************************** */


/* **************************************************************
*  Includes
****************************************************************/
#include "debug.h"      /* assert */
#include "bitstream.h"
#include "compiler.h"
#define FSEN_STATIC_LINKING_ONLY
#include "fse.h"
#include "error_private.h"
#define ZSTDN_DEPS_NEED_MALLOC
#include "zstd_deps.h"


/* **************************************************************
*  Error Management
****************************************************************/
#define FSEN_isError ERR_isError
#define FSEN_STATIC_ASSERT(c) DEBUG_STATIC_ASSERT(c)   /* use only *after* variable declarations */


/* **************************************************************
*  Templates
****************************************************************/
/*
  designed to be included
  for type-specific functions (template emulation in C)
  Objective is to write these functions only once, for improved maintenance
*/

/* safety checks */
#ifndef FSEN_FUNCTION_EXTENSION
#  error "FSEN_FUNCTION_EXTENSION must be defined"
#endif
#ifndef FSEN_FUNCTION_TYPE
#  error "FSEN_FUNCTION_TYPE must be defined"
#endif

/* Function names */
#define FSEN_CAT(X,Y) X##Y
#define FSEN_FUNCTION_NAME(X,Y) FSEN_CAT(X,Y)
#define FSEN_TYPE_NAME(X,Y) FSEN_CAT(X,Y)


/* Function templates */
FSEN_DTable* FSEN_createDTable (unsigned tableLog)
{
    if (tableLog > FSEN_TABLELOG_ABSOLUTE_MAX) tableLog = FSEN_TABLELOG_ABSOLUTE_MAX;
    return (FSEN_DTable*)ZSTDN_malloc( FSEN_DTABLE_SIZE_U32(tableLog) * sizeof (U32) );
}

void FSEN_freeDTable (FSEN_DTable* dt)
{
    ZSTDN_free(dt);
}

static size_t FSEN_buildDTable_internal(FSEN_DTable* dt, const short* normalizedCounter, unsigned maxSymbolValue, unsigned tableLog, void* workSpace, size_t wkspSize)
{
    void* const tdPtr = dt+1;   /* because *dt is unsigned, 32-bits aligned on 32-bits */
    FSEN_DECODE_TYPE* const tableDecode = (FSEN_DECODE_TYPE*) (tdPtr);
    U16* symbolNext = (U16*)workSpace;
    BYTE* spread = (BYTE*)(symbolNext + maxSymbolValue + 1);

    U32 const maxSV1 = maxSymbolValue + 1;
    U32 const tableSize = 1 << tableLog;
    U32 highThreshold = tableSize-1;

    /* Sanity Checks */
    if (FSEN_BUILD_DTABLE_WKSP_SIZE(tableLog, maxSymbolValue) > wkspSize) return ERROR(maxSymbolValue_tooLarge);
    if (maxSymbolValue > FSEN_MAX_SYMBOL_VALUE) return ERROR(maxSymbolValue_tooLarge);
    if (tableLog > FSEN_MAX_TABLELOG) return ERROR(tableLog_tooLarge);

    /* Init, lay down lowprob symbols */
    {   FSEN_DTableHeader DTableH;
        DTableH.tableLog = (U16)tableLog;
        DTableH.fastMode = 1;
        {   S16 const largeLimit= (S16)(1 << (tableLog-1));
            U32 s;
            for (s=0; s<maxSV1; s++) {
                if (normalizedCounter[s]==-1) {
                    tableDecode[highThreshold--].symbol = (FSEN_FUNCTION_TYPE)s;
                    symbolNext[s] = 1;
                } else {
                    if (normalizedCounter[s] >= largeLimit) DTableH.fastMode=0;
                    symbolNext[s] = normalizedCounter[s];
        }   }   }
        ZSTDN_memcpy(dt, &DTableH, sizeof(DTableH));
    }

    /* Spread symbols */
    if (highThreshold == tableSize - 1) {
        size_t const tableMask = tableSize-1;
        size_t const step = FSEN_TABLESTEP(tableSize);
        /* First lay down the symbols in order.
         * We use a uint64_t to lay down 8 bytes at a time. This reduces branch
         * misses since small blocks generally have small table logs, so nearly
         * all symbols have counts <= 8. We ensure we have 8 bytes at the end of
         * our buffer to handle the over-write.
         */
        {
            U64 const add = 0x0101010101010101ull;
            size_t pos = 0;
            U64 sv = 0;
            U32 s;
            for (s=0; s<maxSV1; ++s, sv += add) {
                int i;
                int const n = normalizedCounter[s];
                MEM_write64(spread + pos, sv);
                for (i = 8; i < n; i += 8) {
                    MEM_write64(spread + pos + i, sv);
                }
                pos += n;
            }
        }
        /* Now we spread those positions across the table.
         * The benefit of doing it in two stages is that we avoid the the
         * variable size inner loop, which caused lots of branch misses.
         * Now we can run through all the positions without any branch misses.
         * We unroll the loop twice, since that is what emperically worked best.
         */
        {
            size_t position = 0;
            size_t s;
            size_t const unroll = 2;
            assert(tableSize % unroll == 0); /* FSEN_MIN_TABLELOG is 5 */
            for (s = 0; s < (size_t)tableSize; s += unroll) {
                size_t u;
                for (u = 0; u < unroll; ++u) {
                    size_t const uPosition = (position + (u * step)) & tableMask;
                    tableDecode[uPosition].symbol = spread[s + u];
                }
                position = (position + (unroll * step)) & tableMask;
            }
            assert(position == 0);
        }
    } else {
        U32 const tableMask = tableSize-1;
        U32 const step = FSEN_TABLESTEP(tableSize);
        U32 s, position = 0;
        for (s=0; s<maxSV1; s++) {
            int i;
            for (i=0; i<normalizedCounter[s]; i++) {
                tableDecode[position].symbol = (FSEN_FUNCTION_TYPE)s;
                position = (position + step) & tableMask;
                while (position > highThreshold) position = (position + step) & tableMask;   /* lowprob area */
        }   }
        if (position!=0) return ERROR(GENERIC);   /* position must reach all cells once, otherwise normalizedCounter is incorrect */
    }

    /* Build Decoding table */
    {   U32 u;
        for (u=0; u<tableSize; u++) {
            FSEN_FUNCTION_TYPE const symbol = (FSEN_FUNCTION_TYPE)(tableDecode[u].symbol);
            U32 const nextState = symbolNext[symbol]++;
            tableDecode[u].nbBits = (BYTE) (tableLog - BIT_highbit32(nextState) );
            tableDecode[u].newState = (U16) ( (nextState << tableDecode[u].nbBits) - tableSize);
    }   }

    return 0;
}

size_t FSEN_buildDTable_wksp(FSEN_DTable* dt, const short* normalizedCounter, unsigned maxSymbolValue, unsigned tableLog, void* workSpace, size_t wkspSize)
{
    return FSEN_buildDTable_internal(dt, normalizedCounter, maxSymbolValue, tableLog, workSpace, wkspSize);
}


#ifndef FSEN_COMMONDEFS_ONLY

/*-*******************************************************
*  Decompression (Byte symbols)
*********************************************************/
size_t FSEN_buildDTable_rle (FSEN_DTable* dt, BYTE symbolValue)
{
    void* ptr = dt;
    FSEN_DTableHeader* const DTableH = (FSEN_DTableHeader*)ptr;
    void* dPtr = dt + 1;
    FSEN_decode_t* const cell = (FSEN_decode_t*)dPtr;

    DTableH->tableLog = 0;
    DTableH->fastMode = 0;

    cell->newState = 0;
    cell->symbol = symbolValue;
    cell->nbBits = 0;

    return 0;
}


size_t FSEN_buildDTable_raw (FSEN_DTable* dt, unsigned nbBits)
{
    void* ptr = dt;
    FSEN_DTableHeader* const DTableH = (FSEN_DTableHeader*)ptr;
    void* dPtr = dt + 1;
    FSEN_decode_t* const dinfo = (FSEN_decode_t*)dPtr;
    const unsigned tableSize = 1 << nbBits;
    const unsigned tableMask = tableSize - 1;
    const unsigned maxSV1 = tableMask+1;
    unsigned s;

    /* Sanity checks */
    if (nbBits < 1) return ERROR(GENERIC);         /* min size */

    /* Build Decoding Table */
    DTableH->tableLog = (U16)nbBits;
    DTableH->fastMode = 1;
    for (s=0; s<maxSV1; s++) {
        dinfo[s].newState = 0;
        dinfo[s].symbol = (BYTE)s;
        dinfo[s].nbBits = (BYTE)nbBits;
    }

    return 0;
}

FORCE_INLINE_TEMPLATE size_t FSEN_decompress_usingDTable_generic(
          void* dst, size_t maxDstSize,
    const void* cSrc, size_t cSrcSize,
    const FSEN_DTable* dt, const unsigned fast)
{
    BYTE* const ostart = (BYTE*) dst;
    BYTE* op = ostart;
    BYTE* const omax = op + maxDstSize;
    BYTE* const olimit = omax-3;

    BIT_DStream_t bitD;
    FSEN_DState_t state1;
    FSEN_DState_t state2;

    /* Init */
    CHECK_F(BIT_initDStream(&bitD, cSrc, cSrcSize));

    FSEN_initDState(&state1, &bitD, dt);
    FSEN_initDState(&state2, &bitD, dt);

#define FSEN_GETSYMBOL(statePtr) fast ? FSEN_decodeSymbolFast(statePtr, &bitD) : FSEN_decodeSymbol(statePtr, &bitD)

    /* 4 symbols per loop */
    for ( ; (BIT_reloadDStream(&bitD)==BIT_DStream_unfinished) & (op<olimit) ; op+=4) {
        op[0] = FSEN_GETSYMBOL(&state1);

        if (FSEN_MAX_TABLELOG*2+7 > sizeof(bitD.bitContainer)*8)    /* This test must be static */
            BIT_reloadDStream(&bitD);

        op[1] = FSEN_GETSYMBOL(&state2);

        if (FSEN_MAX_TABLELOG*4+7 > sizeof(bitD.bitContainer)*8)    /* This test must be static */
            { if (BIT_reloadDStream(&bitD) > BIT_DStream_unfinished) { op+=2; break; } }

        op[2] = FSEN_GETSYMBOL(&state1);

        if (FSEN_MAX_TABLELOG*2+7 > sizeof(bitD.bitContainer)*8)    /* This test must be static */
            BIT_reloadDStream(&bitD);

        op[3] = FSEN_GETSYMBOL(&state2);
    }

    /* tail */
    /* note : BIT_reloadDStream(&bitD) >= FSEN_DStream_partiallyFilled; Ends at exactly BIT_DStream_completed */
    while (1) {
        if (op>(omax-2)) return ERROR(dstSize_tooSmall);
        *op++ = FSEN_GETSYMBOL(&state1);
        if (BIT_reloadDStream(&bitD)==BIT_DStream_overflow) {
            *op++ = FSEN_GETSYMBOL(&state2);
            break;
        }

        if (op>(omax-2)) return ERROR(dstSize_tooSmall);
        *op++ = FSEN_GETSYMBOL(&state2);
        if (BIT_reloadDStream(&bitD)==BIT_DStream_overflow) {
            *op++ = FSEN_GETSYMBOL(&state1);
            break;
    }   }

    return op-ostart;
}


size_t FSEN_decompress_usingDTable(void* dst, size_t originalSize,
                            const void* cSrc, size_t cSrcSize,
                            const FSEN_DTable* dt)
{
    const void* ptr = dt;
    const FSEN_DTableHeader* DTableH = (const FSEN_DTableHeader*)ptr;
    const U32 fastMode = DTableH->fastMode;

    /* select fast mode (static) */
    if (fastMode) return FSEN_decompress_usingDTable_generic(dst, originalSize, cSrc, cSrcSize, dt, 1);
    return FSEN_decompress_usingDTable_generic(dst, originalSize, cSrc, cSrcSize, dt, 0);
}


size_t FSEN_decompress_wksp(void* dst, size_t dstCapacity, const void* cSrc, size_t cSrcSize, unsigned maxLog, void* workSpace, size_t wkspSize)
{
    return FSEN_decompress_wksp_bmi2(dst, dstCapacity, cSrc, cSrcSize, maxLog, workSpace, wkspSize, /* bmi2 */ 0);
}

typedef struct {
    short ncount[FSEN_MAX_SYMBOL_VALUE + 1];
    FSEN_DTable dtable[1]; /* Dynamically sized */
} FSEN_DecompressWksp;


FORCE_INLINE_TEMPLATE size_t FSEN_decompress_wksp_body(
        void* dst, size_t dstCapacity,
        const void* cSrc, size_t cSrcSize,
        unsigned maxLog, void* workSpace, size_t wkspSize,
        int bmi2)
{
    const BYTE* const istart = (const BYTE*)cSrc;
    const BYTE* ip = istart;
    unsigned tableLog;
    unsigned maxSymbolValue = FSEN_MAX_SYMBOL_VALUE;
    FSEN_DecompressWksp* const wksp = (FSEN_DecompressWksp*)workSpace;

    DEBUG_STATIC_ASSERT((FSEN_MAX_SYMBOL_VALUE + 1) % 2 == 0);
    if (wkspSize < sizeof(*wksp)) return ERROR(GENERIC);

    /* normal FSE decoding mode */
    {
        size_t const NCountLength = FSEN_readNCount_bmi2(wksp->ncount, &maxSymbolValue, &tableLog, istart, cSrcSize, bmi2);
        if (FSEN_isError(NCountLength)) return NCountLength;
        if (tableLog > maxLog) return ERROR(tableLog_tooLarge);
        assert(NCountLength <= cSrcSize);
        ip += NCountLength;
        cSrcSize -= NCountLength;
    }

    if (FSEN_DECOMPRESS_WKSP_SIZE(tableLog, maxSymbolValue) > wkspSize) return ERROR(tableLog_tooLarge);
    workSpace = wksp->dtable + FSEN_DTABLE_SIZE_U32(tableLog);
    wkspSize -= sizeof(*wksp) + FSEN_DTABLE_SIZE(tableLog);

    CHECK_F( FSEN_buildDTable_internal(wksp->dtable, wksp->ncount, maxSymbolValue, tableLog, workSpace, wkspSize) );

    {
        const void* ptr = wksp->dtable;
        const FSEN_DTableHeader* DTableH = (const FSEN_DTableHeader*)ptr;
        const U32 fastMode = DTableH->fastMode;

        /* select fast mode (static) */
        if (fastMode) return FSEN_decompress_usingDTable_generic(dst, dstCapacity, ip, cSrcSize, wksp->dtable, 1);
        return FSEN_decompress_usingDTable_generic(dst, dstCapacity, ip, cSrcSize, wksp->dtable, 0);
    }
}

/* Avoids the FORCE_INLINE of the _body() function. */
static size_t FSEN_decompress_wksp_body_default(void* dst, size_t dstCapacity, const void* cSrc, size_t cSrcSize, unsigned maxLog, void* workSpace, size_t wkspSize)
{
    return FSEN_decompress_wksp_body(dst, dstCapacity, cSrc, cSrcSize, maxLog, workSpace, wkspSize, 0);
}

#if DYNAMIC_BMI2
TARGET_ATTRIBUTE("bmi2") static size_t FSEN_decompress_wksp_body_bmi2(void* dst, size_t dstCapacity, const void* cSrc, size_t cSrcSize, unsigned maxLog, void* workSpace, size_t wkspSize)
{
    return FSEN_decompress_wksp_body(dst, dstCapacity, cSrc, cSrcSize, maxLog, workSpace, wkspSize, 1);
}
#endif

size_t FSEN_decompress_wksp_bmi2(void* dst, size_t dstCapacity, const void* cSrc, size_t cSrcSize, unsigned maxLog, void* workSpace, size_t wkspSize, int bmi2)
{
#if DYNAMIC_BMI2
    if (bmi2) {
        return FSEN_decompress_wksp_body_bmi2(dst, dstCapacity, cSrc, cSrcSize, maxLog, workSpace, wkspSize);
    }
#endif
    (void)bmi2;
    return FSEN_decompress_wksp_body_default(dst, dstCapacity, cSrc, cSrcSize, maxLog, workSpace, wkspSize);
}


typedef FSEN_DTable DTable_max_t[FSEN_DTABLE_SIZE_U32(FSEN_MAX_TABLELOG)];



#endif   /* FSEN_COMMONDEFS_ONLY */
