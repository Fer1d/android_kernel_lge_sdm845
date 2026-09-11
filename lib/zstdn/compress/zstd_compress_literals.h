/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTDN_COMPRESS_LITERALS_H
#define ZSTDN_COMPRESS_LITERALS_H

#include "zstd_compress_internal.h" /* ZSTDN_hufCTables_t, ZSTDN_minGain() */


size_t ZSTDN_noCompressLiterals (void* dst, size_t dstCapacity, const void* src, size_t srcSize);

size_t ZSTDN_compressRleLiteralsBlock (void* dst, size_t dstCapacity, const void* src, size_t srcSize);

size_t ZSTDN_compressLiterals (ZSTDN_hufCTables_t const* prevHuf,
                              ZSTDN_hufCTables_t* nextHuf,
                              ZSTDN_strategy strategy, int disableLiteralCompression,
                              void* dst, size_t dstCapacity,
                        const void* src, size_t srcSize,
                              void* entropyWorkspace, size_t entropyWorkspaceSize,
                        const int bmi2);

#endif /* ZSTDN_COMPRESS_LITERALS_H */
