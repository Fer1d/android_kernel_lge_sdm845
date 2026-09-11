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
#define ZSTDN_DEPS_NEED_MALLOC
#include "zstd_deps.h"   /* ZSTDN_malloc, ZSTDN_calloc, ZSTDN_free, ZSTDN_memset */
#include "error_private.h"
#include "zstd_internal.h"


/*-****************************************
*  Version
******************************************/
unsigned ZSTDN_versionNumber(void) { return ZSTDN_VERSION_NUMBER; }

const char* ZSTDN_versionString(void) { return ZSTDN_VERSION_STRING; }


/*-****************************************
*  ZSTD Error Management
******************************************/
#undef ZSTDN_isError   /* defined within zstd_internal.h */
/*! ZSTDN_isError() :
 *  tells if a return value is an error code
 *  symbol is required for external callers */
unsigned ZSTDN_isError(size_t code) { return ERR_isError(code); }

/*! ZSTDN_getErrorName() :
 *  provides error code string from function result (useful for debugging) */
const char* ZSTDN_getErrorName(size_t code) { return ERR_getErrorName(code); }

/*! ZSTDN_getError() :
 *  convert a `size_t` function result into a proper ZSTDN_errorCode enum */
ZSTDN_ErrorCode ZSTDN_getErrorCode(size_t code) { return ERR_getErrorCode(code); }

/*! ZSTDN_getErrorString() :
 *  provides error code string from enum */
const char* ZSTDN_getErrorString(ZSTDN_ErrorCode code) { return ERR_getErrorString(code); }



/*=**************************************************************
*  Custom allocator
****************************************************************/
void* ZSTDN_customMalloc(size_t size, ZSTDN_customMem customMem)
{
    if (customMem.customAlloc)
        return customMem.customAlloc(customMem.opaque, size);
    return ZSTDN_malloc(size);
}

void* ZSTDN_customCalloc(size_t size, ZSTDN_customMem customMem)
{
    if (customMem.customAlloc) {
        /* calloc implemented as malloc+memset;
         * not as efficient as calloc, but next best guess for custom malloc */
        void* const ptr = customMem.customAlloc(customMem.opaque, size);
        ZSTDN_memset(ptr, 0, size);
        return ptr;
    }
    return ZSTDN_calloc(1, size);
}

void ZSTDN_customFree(void* ptr, ZSTDN_customMem customMem)
{
    if (ptr!=NULL) {
        if (customMem.customFree)
            customMem.customFree(customMem.opaque, ptr);
        else
            ZSTDN_free(ptr);
    }
}
