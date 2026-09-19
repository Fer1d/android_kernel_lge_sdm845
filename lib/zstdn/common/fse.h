/* ******************************************************************
 * FSE : Finite State Entropy codec
 * Public Prototypes declaration
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


#ifndef FSEN_H
#define FSEN_H


/*-*****************************************
*  Dependencies
******************************************/
#include "zstd_deps.h"    /* size_t, ptrdiff_t */


/*-*****************************************
*  FSEN_PUBLIC_API : control library symbols visibility
******************************************/
#if defined(FSEN_DLL_EXPORT) && (FSEN_DLL_EXPORT==1) && defined(__GNUC__) && (__GNUC__ >= 4)
#  define FSEN_PUBLIC_API __attribute__ ((visibility ("default")))
#elif defined(FSEN_DLL_EXPORT) && (FSEN_DLL_EXPORT==1)   /* Visual expected */
#  define FSEN_PUBLIC_API __declspec(dllexport)
#elif defined(FSEN_DLL_IMPORT) && (FSEN_DLL_IMPORT==1)
#  define FSEN_PUBLIC_API __declspec(dllimport) /* It isn't required but allows to generate better code, saving a function pointer load from the IAT and an indirect jump.*/
#else
#  define FSEN_PUBLIC_API
#endif

/*------   Version   ------*/
#define FSEN_VERSION_MAJOR    0
#define FSEN_VERSION_MINOR    9
#define FSEN_VERSION_RELEASE  0

#define FSEN_LIB_VERSION FSEN_VERSION_MAJOR.FSEN_VERSION_MINOR.FSEN_VERSION_RELEASE
#define FSEN_QUOTE(str) #str
#define FSEN_EXPAND_AND_QUOTE(str) FSEN_QUOTE(str)
#define FSEN_VERSION_STRING FSEN_EXPAND_AND_QUOTE(FSEN_LIB_VERSION)

#define FSEN_VERSION_NUMBER  (FSEN_VERSION_MAJOR *100*100 + FSEN_VERSION_MINOR *100 + FSEN_VERSION_RELEASE)
FSEN_PUBLIC_API unsigned FSEN_versionNumber(void);   /*< library version number; to be used when checking dll version */


/*-****************************************
*  FSE simple functions
******************************************/
/*! FSEN_compress() :
    Compress content of buffer 'src', of size 'srcSize', into destination buffer 'dst'.
    'dst' buffer must be already allocated. Compression runs faster is dstCapacity >= FSEN_compressBound(srcSize).
    @return : size of compressed data (<= dstCapacity).
    Special values : if return == 0, srcData is not compressible => Nothing is stored within dst !!!
                     if return == 1, srcData is a single byte symbol * srcSize times. Use RLE compression instead.
                     if FSEN_isError(return), compression failed (more details using FSEN_getErrorName())
*/
FSEN_PUBLIC_API size_t FSEN_compress(void* dst, size_t dstCapacity,
                             const void* src, size_t srcSize);

/*! FSEN_decompress():
    Decompress FSE data from buffer 'cSrc', of size 'cSrcSize',
    into already allocated destination buffer 'dst', of size 'dstCapacity'.
    @return : size of regenerated data (<= maxDstSize),
              or an error code, which can be tested using FSEN_isError() .

    ** Important ** : FSEN_decompress() does not decompress non-compressible nor RLE data !!!
    Why ? : making this distinction requires a header.
    Header management is intentionally delegated to the user layer, which can better manage special cases.
*/
FSEN_PUBLIC_API size_t FSEN_decompress(void* dst,  size_t dstCapacity,
                               const void* cSrc, size_t cSrcSize);


/*-*****************************************
*  Tool functions
******************************************/
FSEN_PUBLIC_API size_t FSEN_compressBound(size_t size);       /* maximum compressed size */

/* Error Management */
FSEN_PUBLIC_API unsigned    FSEN_isError(size_t code);        /* tells if a return value is an error code */
FSEN_PUBLIC_API const char* FSEN_getErrorName(size_t code);   /* provides error code string (useful for debugging) */


/*-*****************************************
*  FSE advanced functions
******************************************/
/*! FSEN_compress2() :
    Same as FSEN_compress(), but allows the selection of 'maxSymbolValue' and 'tableLog'
    Both parameters can be defined as '0' to mean : use default value
    @return : size of compressed data
    Special values : if return == 0, srcData is not compressible => Nothing is stored within cSrc !!!
                     if return == 1, srcData is a single byte symbol * srcSize times. Use RLE compression.
                     if FSEN_isError(return), it's an error code.
*/
FSEN_PUBLIC_API size_t FSEN_compress2 (void* dst, size_t dstSize, const void* src, size_t srcSize, unsigned maxSymbolValue, unsigned tableLog);


/*-*****************************************
*  FSE detailed API
******************************************/
/*!
FSEN_compress() does the following:
1. count symbol occurrence from source[] into table count[] (see hist.h)
2. normalize counters so that sum(count[]) == Power_of_2 (2^tableLog)
3. save normalized counters to memory buffer using writeNCount()
4. build encoding table 'CTable' from normalized counters
5. encode the data stream using encoding table 'CTable'

FSEN_decompress() does the following:
1. read normalized counters with readNCount()
2. build decoding table 'DTable' from normalized counters
3. decode the data stream using decoding table 'DTable'

The following API allows targeting specific sub-functions for advanced tasks.
For example, it's possible to compress several blocks using the same 'CTable',
or to save and provide normalized distribution using external method.
*/

/* *** COMPRESSION *** */

/*! FSEN_optimalTableLog():
    dynamically downsize 'tableLog' when conditions are met.
    It saves CPU time, by using smaller tables, while preserving or even improving compression ratio.
    @return : recommended tableLog (necessarily <= 'maxTableLog') */
FSEN_PUBLIC_API unsigned FSEN_optimalTableLog(unsigned maxTableLog, size_t srcSize, unsigned maxSymbolValue);

/*! FSEN_normalizeCount():
    normalize counts so that sum(count[]) == Power_of_2 (2^tableLog)
    'normalizedCounter' is a table of short, of minimum size (maxSymbolValue+1).
    useLowProbCount is a boolean parameter which trades off compressed size for
    faster header decoding. When it is set to 1, the compressed data will be slightly
    smaller. And when it is set to 0, FSEN_readNCount() and FSEN_buildDTable() will be
    faster. If you are compressing a small amount of data (< 2 KB) then useLowProbCount=0
    is a good default, since header deserialization makes a big speed difference.
    Otherwise, useLowProbCount=1 is a good default, since the speed difference is small.
    @return : tableLog,
              or an errorCode, which can be tested using FSEN_isError() */
FSEN_PUBLIC_API size_t FSEN_normalizeCount(short* normalizedCounter, unsigned tableLog,
                    const unsigned* count, size_t srcSize, unsigned maxSymbolValue, unsigned useLowProbCount);

/*! FSEN_NCountWriteBound():
    Provides the maximum possible size of an FSE normalized table, given 'maxSymbolValue' and 'tableLog'.
    Typically useful for allocation purpose. */
FSEN_PUBLIC_API size_t FSEN_NCountWriteBound(unsigned maxSymbolValue, unsigned tableLog);

/*! FSEN_writeNCount():
    Compactly save 'normalizedCounter' into 'buffer'.
    @return : size of the compressed table,
              or an errorCode, which can be tested using FSEN_isError(). */
FSEN_PUBLIC_API size_t FSEN_writeNCount (void* buffer, size_t bufferSize,
                                 const short* normalizedCounter,
                                 unsigned maxSymbolValue, unsigned tableLog);

/*! Constructor and Destructor of FSEN_CTable.
    Note that FSEN_CTable size depends on 'tableLog' and 'maxSymbolValue' */
typedef unsigned FSEN_CTable;   /* don't allocate that. It's only meant to be more restrictive than void* */
FSEN_PUBLIC_API FSEN_CTable* FSEN_createCTable (unsigned maxSymbolValue, unsigned tableLog);
FSEN_PUBLIC_API void        FSEN_freeCTable (FSEN_CTable* ct);

/*! FSEN_buildCTable():
    Builds `ct`, which must be already allocated, using FSEN_createCTable().
    @return : 0, or an errorCode, which can be tested using FSEN_isError() */
FSEN_PUBLIC_API size_t FSEN_buildCTable(FSEN_CTable* ct, const short* normalizedCounter, unsigned maxSymbolValue, unsigned tableLog);

/*! FSEN_compress_usingCTable():
    Compress `src` using `ct` into `dst` which must be already allocated.
    @return : size of compressed data (<= `dstCapacity`),
              or 0 if compressed data could not fit into `dst`,
              or an errorCode, which can be tested using FSEN_isError() */
FSEN_PUBLIC_API size_t FSEN_compress_usingCTable (void* dst, size_t dstCapacity, const void* src, size_t srcSize, const FSEN_CTable* ct);

/*!
Tutorial :
----------
The first step is to count all symbols. FSEN_count() does this job very fast.
Result will be saved into 'count', a table of unsigned int, which must be already allocated, and have 'maxSymbolValuePtr[0]+1' cells.
'src' is a table of bytes of size 'srcSize'. All values within 'src' MUST be <= maxSymbolValuePtr[0]
maxSymbolValuePtr[0] will be updated, with its real value (necessarily <= original value)
FSEN_count() will return the number of occurrence of the most frequent symbol.
This can be used to know if there is a single symbol within 'src', and to quickly evaluate its compressibility.
If there is an error, the function will return an ErrorCode (which can be tested using FSEN_isError()).

The next step is to normalize the frequencies.
FSEN_normalizeCount() will ensure that sum of frequencies is == 2 ^'tableLog'.
It also guarantees a minimum of 1 to any Symbol with frequency >= 1.
You can use 'tableLog'==0 to mean "use default tableLog value".
If you are unsure of which tableLog value to use, you can ask FSEN_optimalTableLog(),
which will provide the optimal valid tableLog given sourceSize, maxSymbolValue, and a user-defined maximum (0 means "default").

The result of FSEN_normalizeCount() will be saved into a table,
called 'normalizedCounter', which is a table of signed short.
'normalizedCounter' must be already allocated, and have at least 'maxSymbolValue+1' cells.
The return value is tableLog if everything proceeded as expected.
It is 0 if there is a single symbol within distribution.
If there is an error (ex: invalid tableLog value), the function will return an ErrorCode (which can be tested using FSEN_isError()).

'normalizedCounter' can be saved in a compact manner to a memory area using FSEN_writeNCount().
'buffer' must be already allocated.
For guaranteed success, buffer size must be at least FSEN_headerBound().
The result of the function is the number of bytes written into 'buffer'.
If there is an error, the function will return an ErrorCode (which can be tested using FSEN_isError(); ex : buffer size too small).

'normalizedCounter' can then be used to create the compression table 'CTable'.
The space required by 'CTable' must be already allocated, using FSEN_createCTable().
You can then use FSEN_buildCTable() to fill 'CTable'.
If there is an error, both functions will return an ErrorCode (which can be tested using FSEN_isError()).

'CTable' can then be used to compress 'src', with FSEN_compress_usingCTable().
Similar to FSEN_count(), the convention is that 'src' is assumed to be a table of char of size 'srcSize'
The function returns the size of compressed data (without header), necessarily <= `dstCapacity`.
If it returns '0', compressed data could not fit into 'dst'.
If there is an error, the function will return an ErrorCode (which can be tested using FSEN_isError()).
*/


/* *** DECOMPRESSION *** */

/*! FSEN_readNCount():
    Read compactly saved 'normalizedCounter' from 'rBuffer'.
    @return : size read from 'rBuffer',
              or an errorCode, which can be tested using FSEN_isError().
              maxSymbolValuePtr[0] and tableLogPtr[0] will also be updated with their respective values */
FSEN_PUBLIC_API size_t FSEN_readNCount (short* normalizedCounter,
                           unsigned* maxSymbolValuePtr, unsigned* tableLogPtr,
                           const void* rBuffer, size_t rBuffSize);

/*! FSEN_readNCount_bmi2():
 * Same as FSEN_readNCount() but pass bmi2=1 when your CPU supports BMI2 and 0 otherwise.
 */
FSEN_PUBLIC_API size_t FSEN_readNCount_bmi2(short* normalizedCounter,
                           unsigned* maxSymbolValuePtr, unsigned* tableLogPtr,
                           const void* rBuffer, size_t rBuffSize, int bmi2);

/*! Constructor and Destructor of FSEN_DTable.
    Note that its size depends on 'tableLog' */
typedef unsigned FSEN_DTable;   /* don't allocate that. It's just a way to be more restrictive than void* */
FSEN_PUBLIC_API FSEN_DTable* FSEN_createDTable(unsigned tableLog);
FSEN_PUBLIC_API void        FSEN_freeDTable(FSEN_DTable* dt);

/*! FSEN_buildDTable():
    Builds 'dt', which must be already allocated, using FSEN_createDTable().
    return : 0, or an errorCode, which can be tested using FSEN_isError() */
FSEN_PUBLIC_API size_t FSEN_buildDTable (FSEN_DTable* dt, const short* normalizedCounter, unsigned maxSymbolValue, unsigned tableLog);

/*! FSEN_decompress_usingDTable():
    Decompress compressed source `cSrc` of size `cSrcSize` using `dt`
    into `dst` which must be already allocated.
    @return : size of regenerated data (necessarily <= `dstCapacity`),
              or an errorCode, which can be tested using FSEN_isError() */
FSEN_PUBLIC_API size_t FSEN_decompress_usingDTable(void* dst, size_t dstCapacity, const void* cSrc, size_t cSrcSize, const FSEN_DTable* dt);

/*!
Tutorial :
----------
(Note : these functions only decompress FSE-compressed blocks.
 If block is uncompressed, use memcpy() instead
 If block is a single repeated byte, use memset() instead )

The first step is to obtain the normalized frequencies of symbols.
This can be performed by FSEN_readNCount() if it was saved using FSEN_writeNCount().
'normalizedCounter' must be already allocated, and have at least 'maxSymbolValuePtr[0]+1' cells of signed short.
In practice, that means it's necessary to know 'maxSymbolValue' beforehand,
or size the table to handle worst case situations (typically 256).
FSEN_readNCount() will provide 'tableLog' and 'maxSymbolValue'.
The result of FSEN_readNCount() is the number of bytes read from 'rBuffer'.
Note that 'rBufferSize' must be at least 4 bytes, even if useful information is less than that.
If there is an error, the function will return an error code, which can be tested using FSEN_isError().

The next step is to build the decompression tables 'FSEN_DTable' from 'normalizedCounter'.
This is performed by the function FSEN_buildDTable().
The space required by 'FSEN_DTable' must be already allocated using FSEN_createDTable().
If there is an error, the function will return an error code, which can be tested using FSEN_isError().

`FSEN_DTable` can then be used to decompress `cSrc`, with FSEN_decompress_usingDTable().
`cSrcSize` must be strictly correct, otherwise decompression will fail.
FSEN_decompress_usingDTable() result will tell how many bytes were regenerated (<=`dstCapacity`).
If there is an error, the function will return an error code, which can be tested using FSEN_isError(). (ex: dst buffer too small)
*/

#endif  /* FSEN_H */

#if !defined(FSEN_H_FSE_STATIC_LINKING_ONLY)
#define FSEN_H_FSE_STATIC_LINKING_ONLY

/* *** Dependency *** */
#include "bitstream.h"


/* *****************************************
*  Static allocation
*******************************************/
/* FSE buffer bounds */
#define FSEN_NCOUNTBOUND 512
#define FSEN_BLOCKBOUND(size) ((size) + ((size)>>7) + 4 /* fse states */ + sizeof(size_t) /* bitContainer */)
#define FSEN_COMPRESSBOUND(size) (FSEN_NCOUNTBOUND + FSEN_BLOCKBOUND(size))   /* Macro version, useful for static allocation */

/* It is possible to statically allocate FSE CTable/DTable as a table of FSEN_CTable/FSEN_DTable using below macros */
#define FSEN_CTABLE_SIZE_U32(maxTableLog, maxSymbolValue)   (1 + (1<<((maxTableLog)-1)) + (((maxSymbolValue)+1)*2))
#define FSEN_DTABLE_SIZE_U32(maxTableLog)                   (1 + (1<<(maxTableLog)))

/* or use the size to malloc() space directly. Pay attention to alignment restrictions though */
#define FSEN_CTABLE_SIZE(maxTableLog, maxSymbolValue)   (FSEN_CTABLE_SIZE_U32(maxTableLog, maxSymbolValue) * sizeof(FSEN_CTable))
#define FSEN_DTABLE_SIZE(maxTableLog)                   (FSEN_DTABLE_SIZE_U32(maxTableLog) * sizeof(FSEN_DTable))


/* *****************************************
 *  FSE advanced API
 ***************************************** */

unsigned FSEN_optimalTableLog_internal(unsigned maxTableLog, size_t srcSize, unsigned maxSymbolValue, unsigned minus);
/*< same as FSEN_optimalTableLog(), which used `minus==2` */

/* FSEN_compress_wksp() :
 * Same as FSEN_compress2(), but using an externally allocated scratch buffer (`workSpace`).
 * FSEN_COMPRESS_WKSP_SIZE_U32() provides the minimum size required for `workSpace` as a table of FSEN_CTable.
 */
#define FSEN_COMPRESS_WKSP_SIZE_U32(maxTableLog, maxSymbolValue)   ( FSEN_CTABLE_SIZE_U32(maxTableLog, maxSymbolValue) + ((maxTableLog > 12) ? (1 << (maxTableLog - 2)) : 1024) )
size_t FSEN_compress_wksp (void* dst, size_t dstSize, const void* src, size_t srcSize, unsigned maxSymbolValue, unsigned tableLog, void* workSpace, size_t wkspSize);

size_t FSEN_buildCTable_raw (FSEN_CTable* ct, unsigned nbBits);
/*< build a fake FSEN_CTable, designed for a flat distribution, where each symbol uses nbBits */

size_t FSEN_buildCTable_rle (FSEN_CTable* ct, unsigned char symbolValue);
/*< build a fake FSEN_CTable, designed to compress always the same symbolValue */

/* FSEN_buildCTable_wksp() :
 * Same as FSEN_buildCTable(), but using an externally allocated scratch buffer (`workSpace`).
 * `wkspSize` must be >= `FSEN_BUILD_CTABLE_WORKSPACE_SIZE_U32(maxSymbolValue, tableLog)` of `unsigned`.
 */
#define FSEN_BUILD_CTABLE_WORKSPACE_SIZE_U32(maxSymbolValue, tableLog) (maxSymbolValue + 2 + (1ull << (tableLog - 2)))
#define FSEN_BUILD_CTABLE_WORKSPACE_SIZE(maxSymbolValue, tableLog) (sizeof(unsigned) * FSEN_BUILD_CTABLE_WORKSPACE_SIZE_U32(maxSymbolValue, tableLog))
size_t FSEN_buildCTable_wksp(FSEN_CTable* ct, const short* normalizedCounter, unsigned maxSymbolValue, unsigned tableLog, void* workSpace, size_t wkspSize);

#define FSEN_BUILD_DTABLE_WKSP_SIZE(maxTableLog, maxSymbolValue) (sizeof(short) * (maxSymbolValue + 1) + (1ULL << maxTableLog) + 8)
#define FSEN_BUILD_DTABLE_WKSP_SIZE_U32(maxTableLog, maxSymbolValue) ((FSEN_BUILD_DTABLE_WKSP_SIZE(maxTableLog, maxSymbolValue) + sizeof(unsigned) - 1) / sizeof(unsigned))
FSEN_PUBLIC_API size_t FSEN_buildDTable_wksp(FSEN_DTable* dt, const short* normalizedCounter, unsigned maxSymbolValue, unsigned tableLog, void* workSpace, size_t wkspSize);
/*< Same as FSEN_buildDTable(), using an externally allocated `workspace` produced with `FSEN_BUILD_DTABLE_WKSP_SIZE_U32(maxSymbolValue)` */

size_t FSEN_buildDTable_raw (FSEN_DTable* dt, unsigned nbBits);
/*< build a fake FSEN_DTable, designed to read a flat distribution where each symbol uses nbBits */

size_t FSEN_buildDTable_rle (FSEN_DTable* dt, unsigned char symbolValue);
/*< build a fake FSEN_DTable, designed to always generate the same symbolValue */

#define FSEN_DECOMPRESS_WKSP_SIZE_U32(maxTableLog, maxSymbolValue) (FSEN_DTABLE_SIZE_U32(maxTableLog) + FSEN_BUILD_DTABLE_WKSP_SIZE_U32(maxTableLog, maxSymbolValue) + (FSEN_MAX_SYMBOL_VALUE + 1) / 2 + 1)
#define FSEN_DECOMPRESS_WKSP_SIZE(maxTableLog, maxSymbolValue) (FSEN_DECOMPRESS_WKSP_SIZE_U32(maxTableLog, maxSymbolValue) * sizeof(unsigned))
size_t FSEN_decompress_wksp(void* dst, size_t dstCapacity, const void* cSrc, size_t cSrcSize, unsigned maxLog, void* workSpace, size_t wkspSize);
/*< same as FSEN_decompress(), using an externally allocated `workSpace` produced with `FSEN_DECOMPRESS_WKSP_SIZE_U32(maxLog, maxSymbolValue)` */

size_t FSEN_decompress_wksp_bmi2(void* dst, size_t dstCapacity, const void* cSrc, size_t cSrcSize, unsigned maxLog, void* workSpace, size_t wkspSize, int bmi2);
/*< Same as FSEN_decompress_wksp() but with dynamic BMI2 support. Pass 1 if your CPU supports BMI2 or 0 if it doesn't. */

typedef enum {
   FSEN_repeat_none,  /*< Cannot use the previous table */
   FSEN_repeat_check, /*< Can use the previous table but it must be checked */
   FSEN_repeat_valid  /*< Can use the previous table and it is assumed to be valid */
 } FSEN_repeat;

/* *****************************************
*  FSE symbol compression API
*******************************************/
/*!
   This API consists of small unitary functions, which highly benefit from being inlined.
   Hence their body are included in next section.
*/
typedef struct {
    ptrdiff_t   value;
    const void* stateTable;
    const void* symbolTT;
    unsigned    stateLog;
} FSEN_CState_t;

static void FSEN_initCState(FSEN_CState_t* CStatePtr, const FSEN_CTable* ct);

static void FSEN_encodeSymbol(BIT_CStream_t* bitC, FSEN_CState_t* CStatePtr, unsigned symbol);

static void FSEN_flushCState(BIT_CStream_t* bitC, const FSEN_CState_t* CStatePtr);

/*<
These functions are inner components of FSEN_compress_usingCTable().
They allow the creation of custom streams, mixing multiple tables and bit sources.

A key property to keep in mind is that encoding and decoding are done **in reverse direction**.
So the first symbol you will encode is the last you will decode, like a LIFO stack.

You will need a few variables to track your CStream. They are :

FSEN_CTable    ct;         // Provided by FSEN_buildCTable()
BIT_CStream_t bitStream;  // bitStream tracking structure
FSEN_CState_t  state;      // State tracking structure (can have several)


The first thing to do is to init bitStream and state.
    size_t errorCode = BIT_initCStream(&bitStream, dstBuffer, maxDstSize);
    FSEN_initCState(&state, ct);

Note that BIT_initCStream() can produce an error code, so its result should be tested, using FSEN_isError();
You can then encode your input data, byte after byte.
FSEN_encodeSymbol() outputs a maximum of 'tableLog' bits at a time.
Remember decoding will be done in reverse direction.
    FSEN_encodeByte(&bitStream, &state, symbol);

At any time, you can also add any bit sequence.
Note : maximum allowed nbBits is 25, for compatibility with 32-bits decoders
    BIT_addBits(&bitStream, bitField, nbBits);

The above methods don't commit data to memory, they just store it into local register, for speed.
Local register size is 64-bits on 64-bits systems, 32-bits on 32-bits systems (size_t).
Writing data to memory is a manual operation, performed by the flushBits function.
    BIT_flushBits(&bitStream);

Your last FSE encoding operation shall be to flush your last state value(s).
    FSEN_flushState(&bitStream, &state);

Finally, you must close the bitStream.
The function returns the size of CStream in bytes.
If data couldn't fit into dstBuffer, it will return a 0 ( == not compressible)
If there is an error, it returns an errorCode (which can be tested using FSEN_isError()).
    size_t size = BIT_closeCStream(&bitStream);
*/


/* *****************************************
*  FSE symbol decompression API
*******************************************/
typedef struct {
    size_t      state;
    const void* table;   /* precise table may vary, depending on U16 */
} FSEN_DState_t;


static void     FSEN_initDState(FSEN_DState_t* DStatePtr, BIT_DStream_t* bitD, const FSEN_DTable* dt);

static unsigned char FSEN_decodeSymbol(FSEN_DState_t* DStatePtr, BIT_DStream_t* bitD);

static unsigned FSEN_endOfDState(const FSEN_DState_t* DStatePtr);

/*<
Let's now decompose FSEN_decompress_usingDTable() into its unitary components.
You will decode FSE-encoded symbols from the bitStream,
and also any other bitFields you put in, **in reverse order**.

You will need a few variables to track your bitStream. They are :

BIT_DStream_t DStream;    // Stream context
FSEN_DState_t  DState;     // State context. Multiple ones are possible
FSEN_DTable*   DTablePtr;  // Decoding table, provided by FSEN_buildDTable()

The first thing to do is to init the bitStream.
    errorCode = BIT_initDStream(&DStream, srcBuffer, srcSize);

You should then retrieve your initial state(s)
(in reverse flushing order if you have several ones) :
    errorCode = FSEN_initDState(&DState, &DStream, DTablePtr);

You can then decode your data, symbol after symbol.
For information the maximum number of bits read by FSEN_decodeSymbol() is 'tableLog'.
Keep in mind that symbols are decoded in reverse order, like a LIFO stack (last in, first out).
    unsigned char symbol = FSEN_decodeSymbol(&DState, &DStream);

You can retrieve any bitfield you eventually stored into the bitStream (in reverse order)
Note : maximum allowed nbBits is 25, for 32-bits compatibility
    size_t bitField = BIT_readBits(&DStream, nbBits);

All above operations only read from local register (which size depends on size_t).
Refueling the register from memory is manually performed by the reload method.
    endSignal = FSEN_reloadDStream(&DStream);

BIT_reloadDStream() result tells if there is still some more data to read from DStream.
BIT_DStream_unfinished : there is still some data left into the DStream.
BIT_DStream_endOfBuffer : Dstream reached end of buffer. Its container may no longer be completely filled.
BIT_DStream_completed : Dstream reached its exact end, corresponding in general to decompression completed.
BIT_DStream_tooFar : Dstream went too far. Decompression result is corrupted.

When reaching end of buffer (BIT_DStream_endOfBuffer), progress slowly, notably if you decode multiple symbols per loop,
to properly detect the exact end of stream.
After each decoded symbol, check if DStream is fully consumed using this simple test :
    BIT_reloadDStream(&DStream) >= BIT_DStream_completed

When it's done, verify decompression is fully completed, by checking both DStream and the relevant states.
Checking if DStream has reached its end is performed by :
    BIT_endOfDStream(&DStream);
Check also the states. There might be some symbols left there, if some high probability ones (>50%) are possible.
    FSEN_endOfDState(&DState);
*/


/* *****************************************
*  FSE unsafe API
*******************************************/
static unsigned char FSEN_decodeSymbolFast(FSEN_DState_t* DStatePtr, BIT_DStream_t* bitD);
/* faster, but works only if nbBits is always >= 1 (otherwise, result will be corrupted) */


/* *****************************************
*  Implementation of inlined functions
*******************************************/
typedef struct {
    int deltaFindState;
    U32 deltaNbBits;
} FSEN_symbolCompressionTransform; /* total 8 bytes */

MEM_STATIC void FSEN_initCState(FSEN_CState_t* statePtr, const FSEN_CTable* ct)
{
    const void* ptr = ct;
    const U16* u16ptr = (const U16*) ptr;
    const U32 tableLog = MEM_read16(ptr);
    statePtr->value = (ptrdiff_t)1<<tableLog;
    statePtr->stateTable = u16ptr+2;
    statePtr->symbolTT = ct + 1 + (tableLog ? (1<<(tableLog-1)) : 1);
    statePtr->stateLog = tableLog;
}


/*! FSEN_initCState2() :
*   Same as FSEN_initCState(), but the first symbol to include (which will be the last to be read)
*   uses the smallest state value possible, saving the cost of this symbol */
MEM_STATIC void FSEN_initCState2(FSEN_CState_t* statePtr, const FSEN_CTable* ct, U32 symbol)
{
    FSEN_initCState(statePtr, ct);
    {   const FSEN_symbolCompressionTransform symbolTT = ((const FSEN_symbolCompressionTransform*)(statePtr->symbolTT))[symbol];
        const U16* stateTable = (const U16*)(statePtr->stateTable);
        U32 nbBitsOut  = (U32)((symbolTT.deltaNbBits + (1<<15)) >> 16);
        statePtr->value = (nbBitsOut << 16) - symbolTT.deltaNbBits;
        statePtr->value = stateTable[(statePtr->value >> nbBitsOut) + symbolTT.deltaFindState];
    }
}

MEM_STATIC void FSEN_encodeSymbol(BIT_CStream_t* bitC, FSEN_CState_t* statePtr, unsigned symbol)
{
    FSEN_symbolCompressionTransform const symbolTT = ((const FSEN_symbolCompressionTransform*)(statePtr->symbolTT))[symbol];
    const U16* const stateTable = (const U16*)(statePtr->stateTable);
    U32 const nbBitsOut  = (U32)((statePtr->value + symbolTT.deltaNbBits) >> 16);
    BIT_addBits(bitC, statePtr->value, nbBitsOut);
    statePtr->value = stateTable[ (statePtr->value >> nbBitsOut) + symbolTT.deltaFindState];
}

MEM_STATIC void FSEN_flushCState(BIT_CStream_t* bitC, const FSEN_CState_t* statePtr)
{
    BIT_addBits(bitC, statePtr->value, statePtr->stateLog);
    BIT_flushBits(bitC);
}


/* FSEN_getMaxNbBits() :
 * Approximate maximum cost of a symbol, in bits.
 * Fractional get rounded up (i.e : a symbol with a normalized frequency of 3 gives the same result as a frequency of 2)
 * note 1 : assume symbolValue is valid (<= maxSymbolValue)
 * note 2 : if freq[symbolValue]==0, @return a fake cost of tableLog+1 bits */
MEM_STATIC U32 FSEN_getMaxNbBits(const void* symbolTTPtr, U32 symbolValue)
{
    const FSEN_symbolCompressionTransform* symbolTT = (const FSEN_symbolCompressionTransform*) symbolTTPtr;
    return (symbolTT[symbolValue].deltaNbBits + ((1<<16)-1)) >> 16;
}

/* FSEN_bitCost() :
 * Approximate symbol cost, as fractional value, using fixed-point format (accuracyLog fractional bits)
 * note 1 : assume symbolValue is valid (<= maxSymbolValue)
 * note 2 : if freq[symbolValue]==0, @return a fake cost of tableLog+1 bits */
MEM_STATIC U32 FSEN_bitCost(const void* symbolTTPtr, U32 tableLog, U32 symbolValue, U32 accuracyLog)
{
    const FSEN_symbolCompressionTransform* symbolTT = (const FSEN_symbolCompressionTransform*) symbolTTPtr;
    U32 const minNbBits = symbolTT[symbolValue].deltaNbBits >> 16;
    U32 const threshold = (minNbBits+1) << 16;
    assert(tableLog < 16);
    assert(accuracyLog < 31-tableLog);  /* ensure enough room for renormalization double shift */
    {   U32 const tableSize = 1 << tableLog;
        U32 const deltaFromThreshold = threshold - (symbolTT[symbolValue].deltaNbBits + tableSize);
        U32 const normalizedDeltaFromThreshold = (deltaFromThreshold << accuracyLog) >> tableLog;   /* linear interpolation (very approximate) */
        U32 const bitMultiplier = 1 << accuracyLog;
        assert(symbolTT[symbolValue].deltaNbBits + tableSize <= threshold);
        assert(normalizedDeltaFromThreshold <= bitMultiplier);
        return (minNbBits+1)*bitMultiplier - normalizedDeltaFromThreshold;
    }
}


/* ======    Decompression    ====== */

typedef struct {
    U16 tableLog;
    U16 fastMode;
} FSEN_DTableHeader;   /* sizeof U32 */

typedef struct
{
    unsigned short newState;
    unsigned char  symbol;
    unsigned char  nbBits;
} FSEN_decode_t;   /* size == U32 */

MEM_STATIC void FSEN_initDState(FSEN_DState_t* DStatePtr, BIT_DStream_t* bitD, const FSEN_DTable* dt)
{
    const void* ptr = dt;
    const FSEN_DTableHeader* const DTableH = (const FSEN_DTableHeader*)ptr;
    DStatePtr->state = BIT_readBits(bitD, DTableH->tableLog);
    BIT_reloadDStream(bitD);
    DStatePtr->table = dt + 1;
}

MEM_STATIC BYTE FSEN_peekSymbol(const FSEN_DState_t* DStatePtr)
{
    FSEN_decode_t const DInfo = ((const FSEN_decode_t*)(DStatePtr->table))[DStatePtr->state];
    return DInfo.symbol;
}

MEM_STATIC void FSEN_updateState(FSEN_DState_t* DStatePtr, BIT_DStream_t* bitD)
{
    FSEN_decode_t const DInfo = ((const FSEN_decode_t*)(DStatePtr->table))[DStatePtr->state];
    U32 const nbBits = DInfo.nbBits;
    size_t const lowBits = BIT_readBits(bitD, nbBits);
    DStatePtr->state = DInfo.newState + lowBits;
}

MEM_STATIC BYTE FSEN_decodeSymbol(FSEN_DState_t* DStatePtr, BIT_DStream_t* bitD)
{
    FSEN_decode_t const DInfo = ((const FSEN_decode_t*)(DStatePtr->table))[DStatePtr->state];
    U32 const nbBits = DInfo.nbBits;
    BYTE const symbol = DInfo.symbol;
    size_t const lowBits = BIT_readBits(bitD, nbBits);

    DStatePtr->state = DInfo.newState + lowBits;
    return symbol;
}

/*! FSEN_decodeSymbolFast() :
    unsafe, only works if no symbol has a probability > 50% */
MEM_STATIC BYTE FSEN_decodeSymbolFast(FSEN_DState_t* DStatePtr, BIT_DStream_t* bitD)
{
    FSEN_decode_t const DInfo = ((const FSEN_decode_t*)(DStatePtr->table))[DStatePtr->state];
    U32 const nbBits = DInfo.nbBits;
    BYTE const symbol = DInfo.symbol;
    size_t const lowBits = BIT_readBitsFast(bitD, nbBits);

    DStatePtr->state = DInfo.newState + lowBits;
    return symbol;
}

MEM_STATIC unsigned FSEN_endOfDState(const FSEN_DState_t* DStatePtr)
{
    return DStatePtr->state == 0;
}



#ifndef FSEN_COMMONDEFS_ONLY

/* **************************************************************
*  Tuning parameters
****************************************************************/
/*!MEMORY_USAGE :
*  Memory usage formula : N->2^N Bytes (examples : 10 -> 1KB; 12 -> 4KB ; 16 -> 64KB; 20 -> 1MB; etc.)
*  Increasing memory usage improves compression ratio
*  Reduced memory usage can improve speed, due to cache effect
*  Recommended max value is 14, for 16KB, which nicely fits into Intel x86 L1 cache */
#ifndef FSEN_MAX_MEMORY_USAGE
#  define FSEN_MAX_MEMORY_USAGE 14
#endif
#ifndef FSEN_DEFAULT_MEMORY_USAGE
#  define FSEN_DEFAULT_MEMORY_USAGE 13
#endif
#if (FSEN_DEFAULT_MEMORY_USAGE > FSEN_MAX_MEMORY_USAGE)
#  error "FSEN_DEFAULT_MEMORY_USAGE must be <= FSEN_MAX_MEMORY_USAGE"
#endif

/*!FSEN_MAX_SYMBOL_VALUE :
*  Maximum symbol value authorized.
*  Required for proper stack allocation */
#ifndef FSEN_MAX_SYMBOL_VALUE
#  define FSEN_MAX_SYMBOL_VALUE 255
#endif

/* **************************************************************
*  template functions type & suffix
****************************************************************/
#define FSEN_FUNCTION_TYPE BYTE
#define FSEN_FUNCTION_EXTENSION
#define FSEN_DECODE_TYPE FSEN_decode_t


#endif   /* !FSEN_COMMONDEFS_ONLY */


/* ***************************************************************
*  Constants
*****************************************************************/
#define FSEN_MAX_TABLELOG  (FSEN_MAX_MEMORY_USAGE-2)
#define FSEN_MAX_TABLESIZE (1U<<FSEN_MAX_TABLELOG)
#define FSEN_MAXTABLESIZE_MASK (FSEN_MAX_TABLESIZE-1)
#define FSEN_DEFAULT_TABLELOG (FSEN_DEFAULT_MEMORY_USAGE-2)
#define FSEN_MIN_TABLELOG 5

#define FSEN_TABLELOG_ABSOLUTE_MAX 15
#if FSEN_MAX_TABLELOG > FSEN_TABLELOG_ABSOLUTE_MAX
#  error "FSEN_MAX_TABLELOG > FSEN_TABLELOG_ABSOLUTE_MAX is not supported"
#endif

#define FSEN_TABLESTEP(tableSize) (((tableSize)>>1) + ((tableSize)>>3) + 3)


#endif /* FSEN_STATIC_LINKING_ONLY */


