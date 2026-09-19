/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTDN_COMPRESS_ADVANCED_H
#define ZSTDN_COMPRESS_ADVANCED_H

/*-*************************************
*  Dependencies
***************************************/

#include "zstd.h" /* ZSTDN_CCtx */

/*-*************************************
*  Target Compressed Block Size
***************************************/

/* ZSTDN_compressSuperBlock() :
 * Used to compress a super block when targetCBlockSize is being used.
 * The given block will be compressed into multiple sub blocks that are around targetCBlockSize. */
size_t ZSTDN_compressSuperBlock(ZSTDN_CCtx* zc,
                               void* dst, size_t dstCapacity,
                               void const* src, size_t srcSize,
                               unsigned lastBlock);

#endif /* ZSTDN_COMPRESS_ADVANCED_H */
