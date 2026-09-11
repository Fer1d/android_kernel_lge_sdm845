/* ******************************************************************
 * huff0 huffman codec,
 * part of Finite State Entropy library
 * Copyright (c) Yann Collet, Facebook, Inc.
 *
 * You can contact the author at :
 * - Source repository : https://github.com/Cyan4973/FiniteStateEntropy
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
****************************************************************** */


#ifndef HUFN_H_298734234
#define HUFN_H_298734234

/* *** Dependencies *** */
#include "zstd_deps.h"    /* size_t */


/* *** library symbols visibility *** */
/* Note : when linking with -fvisibility=hidden on gcc, or by default on Visual,
 *        HUF symbols remain "private" (internal symbols for library only).
 *        Set macro FSEN_DLL_EXPORT to 1 if you want HUF symbols visible on DLL interface */
#if defined(FSEN_DLL_EXPORT) && (FSEN_DLL_EXPORT==1) && defined(__GNUC__) && (__GNUC__ >= 4)
#  define HUFN_PUBLIC_API __attribute__ ((visibility ("default")))
#elif defined(FSEN_DLL_EXPORT) && (FSEN_DLL_EXPORT==1)   /* Visual expected */
#  define HUFN_PUBLIC_API __declspec(dllexport)
#elif defined(FSEN_DLL_IMPORT) && (FSEN_DLL_IMPORT==1)
#  define HUFN_PUBLIC_API __declspec(dllimport)  /* not required, just to generate faster code (saves a function pointer load from IAT and an indirect jump) */
#else
#  define HUFN_PUBLIC_API
#endif


/* ========================== */
/* ***  simple functions  *** */
/* ========================== */

/* HUFN_compress() :
 *  Compress content from buffer 'src', of size 'srcSize', into buffer 'dst'.
 * 'dst' buffer must be already allocated.
 *  Compression runs faster if `dstCapacity` >= HUFN_compressBound(srcSize).
 * `srcSize` must be <= `HUFN_BLOCKSIZE_MAX` == 128 KB.
 * @return : size of compressed data (<= `dstCapacity`).
 *  Special values : if return == 0, srcData is not compressible => Nothing is stored within dst !!!
 *                   if HUFN_isError(return), compression failed (more details using HUFN_getErrorName())
 */
HUFN_PUBLIC_API size_t HUFN_compress(void* dst, size_t dstCapacity,
                             const void* src, size_t srcSize);

/* HUFN_decompress() :
 *  Decompress HUF data from buffer 'cSrc', of size 'cSrcSize',
 *  into already allocated buffer 'dst', of minimum size 'dstSize'.
 * `originalSize` : **must** be the ***exact*** size of original (uncompressed) data.
 *  Note : in contrast with FSE, HUFN_decompress can regenerate
 *         RLE (cSrcSize==1) and uncompressed (cSrcSize==dstSize) data,
 *         because it knows size to regenerate (originalSize).
 * @return : size of regenerated data (== originalSize),
 *           or an error code, which can be tested using HUFN_isError()
 */
HUFN_PUBLIC_API size_t HUFN_decompress(void* dst,  size_t originalSize,
                               const void* cSrc, size_t cSrcSize);


/* ***   Tool functions *** */
#define HUFN_BLOCKSIZE_MAX (128 * 1024)                  /*< maximum input size for a single block compressed with HUFN_compress */
HUFN_PUBLIC_API size_t HUFN_compressBound(size_t size);   /*< maximum compressed size (worst case) */

/* Error Management */
HUFN_PUBLIC_API unsigned    HUFN_isError(size_t code);       /*< tells if a return value is an error code */
HUFN_PUBLIC_API const char* HUFN_getErrorName(size_t code);  /*< provides error code string (useful for debugging) */


/* ***   Advanced function   *** */

/* HUFN_compress2() :
 *  Same as HUFN_compress(), but offers control over `maxSymbolValue` and `tableLog`.
 * `maxSymbolValue` must be <= HUFN_SYMBOLVALUE_MAX .
 * `tableLog` must be `<= HUFN_TABLELOG_MAX` . */
HUFN_PUBLIC_API size_t HUFN_compress2 (void* dst, size_t dstCapacity,
                               const void* src, size_t srcSize,
                               unsigned maxSymbolValue, unsigned tableLog);

/* HUFN_compress4X_wksp() :
 *  Same as HUFN_compress2(), but uses externally allocated `workSpace`.
 * `workspace` must have minimum alignment of 4, and be at least as large as HUFN_WORKSPACE_SIZE */
#define HUFN_WORKSPACE_SIZE ((6 << 10) + 256)
#define HUFN_WORKSPACE_SIZE_U32 (HUFN_WORKSPACE_SIZE / sizeof(U32))
HUFN_PUBLIC_API size_t HUFN_compress4X_wksp (void* dst, size_t dstCapacity,
                                     const void* src, size_t srcSize,
                                     unsigned maxSymbolValue, unsigned tableLog,
                                     void* workSpace, size_t wkspSize);

#endif   /* HUFN_H_298734234 */

/* ******************************************************************
 *  WARNING !!
 *  The following section contains advanced and experimental definitions
 *  which shall never be used in the context of a dynamic library,
 *  because they are not guaranteed to remain stable in the future.
 *  Only consider them in association with static linking.
 * *****************************************************************/
#if !defined(HUFN_H_HUF_STATIC_LINKING_ONLY)
#define HUFN_H_HUF_STATIC_LINKING_ONLY

/* *** Dependencies *** */
#include "mem.h"   /* U32 */
#define FSEN_STATIC_LINKING_ONLY
#include "fse.h"


/* *** Constants *** */
#define HUFN_TABLELOG_MAX      12      /* max runtime value of tableLog (due to static allocation); can be modified up to HUFN_ABSOLUTEMAX_TABLELOG */
#define HUFN_TABLELOG_DEFAULT  11      /* default tableLog value when none specified */
#define HUFN_SYMBOLVALUE_MAX  255

#define HUFN_TABLELOG_ABSOLUTEMAX  15  /* absolute limit of HUFN_MAX_TABLELOG. Beyond that value, code does not work */
#if (HUFN_TABLELOG_MAX > HUFN_TABLELOG_ABSOLUTEMAX)
#  error "HUFN_TABLELOG_MAX is too large !"
#endif


/* ****************************************
*  Static allocation
******************************************/
/* HUF buffer bounds */
#define HUFN_CTABLEBOUND 129
#define HUFN_BLOCKBOUND(size) (size + (size>>8) + 8)   /* only true when incompressible is pre-filtered with fast heuristic */
#define HUFN_COMPRESSBOUND(size) (HUFN_CTABLEBOUND + HUFN_BLOCKBOUND(size))   /* Macro version, useful for static allocation */

/* static allocation of HUF's Compression Table */
/* this is a private definition, just exposed for allocation and strict aliasing purpose. never EVER access its members directly */
struct HUFN_CElt_s {
  U16  val;
  BYTE nbBits;
};   /* typedef'd to HUFN_CElt */
typedef struct HUFN_CElt_s HUFN_CElt;   /* consider it an incomplete type */
#define HUFN_CTABLE_SIZE_U32(maxSymbolValue)   ((maxSymbolValue)+1)   /* Use tables of U32, for proper alignment */
#define HUFN_CTABLE_SIZE(maxSymbolValue)       (HUFN_CTABLE_SIZE_U32(maxSymbolValue) * sizeof(U32))
#define HUFN_CREATE_STATIC_CTABLE(name, maxSymbolValue) \
    HUFN_CElt name[HUFN_CTABLE_SIZE_U32(maxSymbolValue)] /* no final ; */

/* static allocation of HUF's DTable */
typedef U32 HUFN_DTable;
#define HUFN_DTABLE_SIZE(maxTableLog)   (1 + (1<<(maxTableLog)))
#define HUFN_CREATE_STATIC_DTABLEX1(DTable, maxTableLog) \
        HUFN_DTable DTable[HUFN_DTABLE_SIZE((maxTableLog)-1)] = { ((U32)((maxTableLog)-1) * 0x01000001) }
#define HUFN_CREATE_STATIC_DTABLEX2(DTable, maxTableLog) \
        HUFN_DTable DTable[HUFN_DTABLE_SIZE(maxTableLog)] = { ((U32)(maxTableLog) * 0x01000001) }


/* ****************************************
*  Advanced decompression functions
******************************************/
size_t HUFN_decompress4X1 (void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize);   /*< single-symbol decoder */
#ifndef HUFN_FORCE_DECOMPRESS_X1
size_t HUFN_decompress4X2 (void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize);   /*< double-symbols decoder */
#endif

size_t HUFN_decompress4X_DCtx (HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize);   /*< decodes RLE and uncompressed */
size_t HUFN_decompress4X_hufOnly(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize); /*< considers RLE and uncompressed as errors */
size_t HUFN_decompress4X_hufOnly_wksp(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize, void* workSpace, size_t wkspSize); /*< considers RLE and uncompressed as errors */
size_t HUFN_decompress4X1_DCtx(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize);   /*< single-symbol decoder */
size_t HUFN_decompress4X1_DCtx_wksp(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize, void* workSpace, size_t wkspSize);   /*< single-symbol decoder */
#ifndef HUFN_FORCE_DECOMPRESS_X1
size_t HUFN_decompress4X2_DCtx(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize);   /*< double-symbols decoder */
size_t HUFN_decompress4X2_DCtx_wksp(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize, void* workSpace, size_t wkspSize);   /*< double-symbols decoder */
#endif


/* ****************************************
 *  HUF detailed API
 * ****************************************/

/*! HUFN_compress() does the following:
 *  1. count symbol occurrence from source[] into table count[] using FSEN_count() (exposed within "fse.h")
 *  2. (optional) refine tableLog using HUFN_optimalTableLog()
 *  3. build Huffman table from count using HUFN_buildCTable()
 *  4. save Huffman table to memory buffer using HUFN_writeCTable()
 *  5. encode the data stream using HUFN_compress4X_usingCTable()
 *
 *  The following API allows targeting specific sub-functions for advanced tasks.
 *  For example, it's possible to compress several blocks using the same 'CTable',
 *  or to save and regenerate 'CTable' using external methods.
 */
unsigned HUFN_optimalTableLog(unsigned maxTableLog, size_t srcSize, unsigned maxSymbolValue);
size_t HUFN_buildCTable (HUFN_CElt* CTable, const unsigned* count, unsigned maxSymbolValue, unsigned maxNbBits);   /* @return : maxNbBits; CTable and count can overlap. In which case, CTable will overwrite count content */
size_t HUFN_writeCTable (void* dst, size_t maxDstSize, const HUFN_CElt* CTable, unsigned maxSymbolValue, unsigned huffLog);
size_t HUFN_writeCTable_wksp(void* dst, size_t maxDstSize, const HUFN_CElt* CTable, unsigned maxSymbolValue, unsigned huffLog, void* workspace, size_t workspaceSize);
size_t HUFN_compress4X_usingCTable(void* dst, size_t dstSize, const void* src, size_t srcSize, const HUFN_CElt* CTable);
size_t HUFN_estimateCompressedSize(const HUFN_CElt* CTable, const unsigned* count, unsigned maxSymbolValue);
int HUFN_validateCTable(const HUFN_CElt* CTable, const unsigned* count, unsigned maxSymbolValue);

typedef enum {
   HUFN_repeat_none,  /*< Cannot use the previous table */
   HUFN_repeat_check, /*< Can use the previous table but it must be checked. Note : The previous table must have been constructed by HUFN_compress{1, 4}X_repeat */
   HUFN_repeat_valid  /*< Can use the previous table and it is assumed to be valid */
 } HUFN_repeat;
/* HUFN_compress4X_repeat() :
 *  Same as HUFN_compress4X_wksp(), but considers using hufTable if *repeat != HUFN_repeat_none.
 *  If it uses hufTable it does not modify hufTable or repeat.
 *  If it doesn't, it sets *repeat = HUFN_repeat_none, and it sets hufTable to the table used.
 *  If preferRepeat then the old table will always be used if valid. */
size_t HUFN_compress4X_repeat(void* dst, size_t dstSize,
                       const void* src, size_t srcSize,
                       unsigned maxSymbolValue, unsigned tableLog,
                       void* workSpace, size_t wkspSize,    /*< `workSpace` must be aligned on 4-bytes boundaries, `wkspSize` must be >= HUFN_WORKSPACE_SIZE */
                       HUFN_CElt* hufTable, HUFN_repeat* repeat, int preferRepeat, int bmi2);

/* HUFN_buildCTable_wksp() :
 *  Same as HUFN_buildCTable(), but using externally allocated scratch buffer.
 * `workSpace` must be aligned on 4-bytes boundaries, and its size must be >= HUFN_CTABLE_WORKSPACE_SIZE.
 */
#define HUFN_CTABLE_WORKSPACE_SIZE_U32 (2*HUFN_SYMBOLVALUE_MAX +1 +1)
#define HUFN_CTABLE_WORKSPACE_SIZE (HUFN_CTABLE_WORKSPACE_SIZE_U32 * sizeof(unsigned))
size_t HUFN_buildCTable_wksp (HUFN_CElt* tree,
                       const unsigned* count, U32 maxSymbolValue, U32 maxNbBits,
                             void* workSpace, size_t wkspSize);

/*! HUFN_readStats() :
 *  Read compact Huffman tree, saved by HUFN_writeCTable().
 * `huffWeight` is destination buffer.
 * @return : size read from `src` , or an error Code .
 *  Note : Needed by HUFN_readCTable() and HUFN_readDTableXn() . */
size_t HUFN_readStats(BYTE* huffWeight, size_t hwSize,
                     U32* rankStats, U32* nbSymbolsPtr, U32* tableLogPtr,
                     const void* src, size_t srcSize);

/*! HUFN_readStats_wksp() :
 * Same as HUFN_readStats() but takes an external workspace which must be
 * 4-byte aligned and its size must be >= HUFN_READ_STATS_WORKSPACE_SIZE.
 * If the CPU has BMI2 support, pass bmi2=1, otherwise pass bmi2=0.
 */
#define HUFN_READ_STATS_WORKSPACE_SIZE_U32 FSEN_DECOMPRESS_WKSP_SIZE_U32(6, HUFN_TABLELOG_MAX-1)
#define HUFN_READ_STATS_WORKSPACE_SIZE (HUFN_READ_STATS_WORKSPACE_SIZE_U32 * sizeof(unsigned))
size_t HUFN_readStats_wksp(BYTE* huffWeight, size_t hwSize,
                          U32* rankStats, U32* nbSymbolsPtr, U32* tableLogPtr,
                          const void* src, size_t srcSize,
                          void* workspace, size_t wkspSize,
                          int bmi2);

/* HUFN_readCTable() :
 *  Loading a CTable saved with HUFN_writeCTable() */
size_t HUFN_readCTable (HUFN_CElt* CTable, unsigned* maxSymbolValuePtr, const void* src, size_t srcSize, unsigned *hasZeroWeights);

/* HUFN_getNbBits() :
 *  Read nbBits from CTable symbolTable, for symbol `symbolValue` presumed <= HUFN_SYMBOLVALUE_MAX
 *  Note 1 : is not inlined, as HUFN_CElt definition is private
 *  Note 2 : const void* used, so that it can provide a statically allocated table as argument (which uses type U32) */
U32 HUFN_getNbBits(const void* symbolTable, U32 symbolValue);

/*
 * HUFN_decompress() does the following:
 * 1. select the decompression algorithm (X1, X2) based on pre-computed heuristics
 * 2. build Huffman table from save, using HUFN_readDTableX?()
 * 3. decode 1 or 4 segments in parallel using HUFN_decompress?X?_usingDTable()
 */

/* HUFN_selectDecoder() :
 *  Tells which decoder is likely to decode faster,
 *  based on a set of pre-computed metrics.
 * @return : 0==HUFN_decompress4X1, 1==HUFN_decompress4X2 .
 *  Assumption : 0 < dstSize <= 128 KB */
U32 HUFN_selectDecoder (size_t dstSize, size_t cSrcSize);

/*
 *  The minimum workspace size for the `workSpace` used in
 *  HUFN_readDTableX1_wksp() and HUFN_readDTableX2_wksp().
 *
 *  The space used depends on HUFN_TABLELOG_MAX, ranging from ~1500 bytes when
 *  HUFN_TABLE_LOG_MAX=12 to ~1850 bytes when HUFN_TABLE_LOG_MAX=15.
 *  Buffer overflow errors may potentially occur if code modifications result in
 *  a required workspace size greater than that specified in the following
 *  macro.
 */
#define HUFN_DECOMPRESS_WORKSPACE_SIZE ((2 << 10) + (1 << 9))
#define HUFN_DECOMPRESS_WORKSPACE_SIZE_U32 (HUFN_DECOMPRESS_WORKSPACE_SIZE / sizeof(U32))

#ifndef HUFN_FORCE_DECOMPRESS_X2
size_t HUFN_readDTableX1 (HUFN_DTable* DTable, const void* src, size_t srcSize);
size_t HUFN_readDTableX1_wksp (HUFN_DTable* DTable, const void* src, size_t srcSize, void* workSpace, size_t wkspSize);
#endif
#ifndef HUFN_FORCE_DECOMPRESS_X1
size_t HUFN_readDTableX2 (HUFN_DTable* DTable, const void* src, size_t srcSize);
size_t HUFN_readDTableX2_wksp (HUFN_DTable* DTable, const void* src, size_t srcSize, void* workSpace, size_t wkspSize);
#endif

size_t HUFN_decompress4X_usingDTable(void* dst, size_t maxDstSize, const void* cSrc, size_t cSrcSize, const HUFN_DTable* DTable);
#ifndef HUFN_FORCE_DECOMPRESS_X2
size_t HUFN_decompress4X1_usingDTable(void* dst, size_t maxDstSize, const void* cSrc, size_t cSrcSize, const HUFN_DTable* DTable);
#endif
#ifndef HUFN_FORCE_DECOMPRESS_X1
size_t HUFN_decompress4X2_usingDTable(void* dst, size_t maxDstSize, const void* cSrc, size_t cSrcSize, const HUFN_DTable* DTable);
#endif


/* ====================== */
/* single stream variants */
/* ====================== */

size_t HUFN_compress1X (void* dst, size_t dstSize, const void* src, size_t srcSize, unsigned maxSymbolValue, unsigned tableLog);
size_t HUFN_compress1X_wksp (void* dst, size_t dstSize, const void* src, size_t srcSize, unsigned maxSymbolValue, unsigned tableLog, void* workSpace, size_t wkspSize);  /*< `workSpace` must be a table of at least HUFN_WORKSPACE_SIZE_U32 unsigned */
size_t HUFN_compress1X_usingCTable(void* dst, size_t dstSize, const void* src, size_t srcSize, const HUFN_CElt* CTable);
/* HUFN_compress1X_repeat() :
 *  Same as HUFN_compress1X_wksp(), but considers using hufTable if *repeat != HUFN_repeat_none.
 *  If it uses hufTable it does not modify hufTable or repeat.
 *  If it doesn't, it sets *repeat = HUFN_repeat_none, and it sets hufTable to the table used.
 *  If preferRepeat then the old table will always be used if valid. */
size_t HUFN_compress1X_repeat(void* dst, size_t dstSize,
                       const void* src, size_t srcSize,
                       unsigned maxSymbolValue, unsigned tableLog,
                       void* workSpace, size_t wkspSize,   /*< `workSpace` must be aligned on 4-bytes boundaries, `wkspSize` must be >= HUFN_WORKSPACE_SIZE */
                       HUFN_CElt* hufTable, HUFN_repeat* repeat, int preferRepeat, int bmi2);

size_t HUFN_decompress1X1 (void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize);   /* single-symbol decoder */
#ifndef HUFN_FORCE_DECOMPRESS_X1
size_t HUFN_decompress1X2 (void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize);   /* double-symbol decoder */
#endif

size_t HUFN_decompress1X_DCtx (HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize);
size_t HUFN_decompress1X_DCtx_wksp (HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize, void* workSpace, size_t wkspSize);
#ifndef HUFN_FORCE_DECOMPRESS_X2
size_t HUFN_decompress1X1_DCtx(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize);   /*< single-symbol decoder */
size_t HUFN_decompress1X1_DCtx_wksp(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize, void* workSpace, size_t wkspSize);   /*< single-symbol decoder */
#endif
#ifndef HUFN_FORCE_DECOMPRESS_X1
size_t HUFN_decompress1X2_DCtx(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize);   /*< double-symbols decoder */
size_t HUFN_decompress1X2_DCtx_wksp(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize, void* workSpace, size_t wkspSize);   /*< double-symbols decoder */
#endif

size_t HUFN_decompress1X_usingDTable(void* dst, size_t maxDstSize, const void* cSrc, size_t cSrcSize, const HUFN_DTable* DTable);   /*< automatic selection of sing or double symbol decoder, based on DTable */
#ifndef HUFN_FORCE_DECOMPRESS_X2
size_t HUFN_decompress1X1_usingDTable(void* dst, size_t maxDstSize, const void* cSrc, size_t cSrcSize, const HUFN_DTable* DTable);
#endif
#ifndef HUFN_FORCE_DECOMPRESS_X1
size_t HUFN_decompress1X2_usingDTable(void* dst, size_t maxDstSize, const void* cSrc, size_t cSrcSize, const HUFN_DTable* DTable);
#endif

/* BMI2 variants.
 * If the CPU has BMI2 support, pass bmi2=1, otherwise pass bmi2=0.
 */
size_t HUFN_decompress1X_usingDTable_bmi2(void* dst, size_t maxDstSize, const void* cSrc, size_t cSrcSize, const HUFN_DTable* DTable, int bmi2);
#ifndef HUFN_FORCE_DECOMPRESS_X2
size_t HUFN_decompress1X1_DCtx_wksp_bmi2(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize, void* workSpace, size_t wkspSize, int bmi2);
#endif
size_t HUFN_decompress4X_usingDTable_bmi2(void* dst, size_t maxDstSize, const void* cSrc, size_t cSrcSize, const HUFN_DTable* DTable, int bmi2);
size_t HUFN_decompress4X_hufOnly_wksp_bmi2(HUFN_DTable* dctx, void* dst, size_t dstSize, const void* cSrc, size_t cSrcSize, void* workSpace, size_t wkspSize, int bmi2);
#ifndef HUFN_FORCE_DECOMPRESS_X2
size_t HUFN_readDTableX1_wksp_bmi2(HUFN_DTable* DTable, const void* src, size_t srcSize, void* workSpace, size_t wkspSize, int bmi2);
#endif

#endif /* HUFN_STATIC_LINKING_ONLY */

