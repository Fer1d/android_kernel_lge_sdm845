/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTDN_ERRORS_H_398273423
#define ZSTDN_ERRORS_H_398273423


/*===== dependency =====*/
#include <linux/types.h>   /* size_t */


/* =====   ZSTDERRORLIB_API : control library symbols visibility   ===== */
#define ZSTDERRORLIB_VISIBILITY 
#define ZSTDERRORLIB_API ZSTDERRORLIB_VISIBILITY

/*-*********************************************
 *  Error codes list
 *-*********************************************
 *  Error codes _values_ are pinned down since v1.3.1 only.
 *  Therefore, don't rely on values if you may link to any version < v1.3.1.
 *
 *  Only values < 100 are considered stable.
 *
 *  note 1 : this API shall be used with static linking only.
 *           dynamic linking is not yet officially supported.
 *  note 2 : Prefer relying on the enum than on its value whenever possible
 *           This is the only supported way to use the error list < v1.3.1
 *  note 3 : ZSTDN_isError() is always correct, whatever the library version.
 **********************************************/
typedef enum {
  ZSTDN_error_no_error = 0,
  ZSTDN_error_GENERIC  = 1,
  ZSTDN_error_prefix_unknown                = 10,
  ZSTDN_error_version_unsupported           = 12,
  ZSTDN_error_frameParameter_unsupported    = 14,
  ZSTDN_error_frameParameter_windowTooLarge = 16,
  ZSTDN_error_corruption_detected = 20,
  ZSTDN_error_checksum_wrong      = 22,
  ZSTDN_error_dictionary_corrupted      = 30,
  ZSTDN_error_dictionary_wrong          = 32,
  ZSTDN_error_dictionaryCreation_failed = 34,
  ZSTDN_error_parameter_unsupported   = 40,
  ZSTDN_error_parameter_outOfBound    = 42,
  ZSTDN_error_tableLog_tooLarge       = 44,
  ZSTDN_error_maxSymbolValue_tooLarge = 46,
  ZSTDN_error_maxSymbolValue_tooSmall = 48,
  ZSTDN_error_stage_wrong       = 60,
  ZSTDN_error_init_missing      = 62,
  ZSTDN_error_memory_allocation = 64,
  ZSTDN_error_workSpace_tooSmall= 66,
  ZSTDN_error_dstSize_tooSmall = 70,
  ZSTDN_error_srcSize_wrong    = 72,
  ZSTDN_error_dstBuffer_null   = 74,
  /* following error codes are __NOT STABLE__, they can be removed or changed in future versions */
  ZSTDN_error_frameIndex_tooLarge = 100,
  ZSTDN_error_seekableIO          = 102,
  ZSTDN_error_dstBuffer_wrong     = 104,
  ZSTDN_error_srcBuffer_wrong     = 105,
  ZSTDN_error_maxCode = 120  /* never EVER use this value directly, it can change in future versions! Use ZSTDN_isError() instead */
} ZSTDN_ErrorCode;

/*! ZSTDN_getErrorCode() :
    convert a `size_t` function result into a `ZSTDN_ErrorCode` enum type,
    which can be used to compare with enum list published above */
ZSTDERRORLIB_API ZSTDN_ErrorCode ZSTDN_getErrorCode(size_t functionResult);
ZSTDERRORLIB_API const char* ZSTDN_getErrorString(ZSTDN_ErrorCode code);   /*< Same as ZSTDN_getErrorName, but using a `ZSTDN_ErrorCode` enum argument */



#endif /* ZSTDN_ERRORS_H_398273423 */
