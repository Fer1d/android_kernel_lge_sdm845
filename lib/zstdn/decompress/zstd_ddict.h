/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


#ifndef ZSTDN_DDICT_H
#define ZSTDN_DDICT_H

/*-*******************************************************
 *  Dependencies
 *********************************************************/
#include "../common/zstd_deps.h"   /* size_t */
#include "zstd.h"     /* ZSTDN_DDict, and several public functions */


/*-*******************************************************
 *  Interface
 *********************************************************/

/* note: several prototypes are already published in `zstd.h` :
 * ZSTDN_createDDict()
 * ZSTDN_createDDict_byReference()
 * ZSTDN_createDDict_advanced()
 * ZSTDN_freeDDict()
 * ZSTDN_initStaticDDict()
 * ZSTDN_sizeof_DDict()
 * ZSTDN_estimateDDictSize()
 * ZSTDN_getDictID_fromDict()
 */

const void* ZSTDN_DDict_dictContent(const ZSTDN_DDict* ddict);
size_t ZSTDN_DDict_dictSize(const ZSTDN_DDict* ddict);

void ZSTDN_copyDDictParameters(ZSTDN_DCtx* dctx, const ZSTDN_DDict* ddict);



#endif /* ZSTDN_DDICT_H */
