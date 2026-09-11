// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (c) Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include "zstd.h"

#include "common/zstd_deps.h"
#include "common/zstd_internal.h"

#define ZSTDN_FORWARD_IF_ERR(ret)            \
	do {                                \
		size_t const __ret = (ret); \
		if (ZSTDN_isError(__ret))    \
			return __ret;       \
	} while (0)

static size_t zstd_cctx_init(zstd_cctx *cctx, const zstd_parameters *parameters,
	unsigned long long pledged_src_size)
{
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_reset(
		cctx, ZSTDN_reset_session_and_parameters));
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_setPledgedSrcSize(
		cctx, pledged_src_size));
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_setParameter(
		cctx, ZSTDN_c_windowLog, parameters->cParams.windowLog));
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_setParameter(
		cctx, ZSTDN_c_hashLog, parameters->cParams.hashLog));
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_setParameter(
		cctx, ZSTDN_c_chainLog, parameters->cParams.chainLog));
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_setParameter(
		cctx, ZSTDN_c_searchLog, parameters->cParams.searchLog));
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_setParameter(
		cctx, ZSTDN_c_minMatch, parameters->cParams.minMatch));
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_setParameter(
		cctx, ZSTDN_c_targetLength, parameters->cParams.targetLength));
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_setParameter(
		cctx, ZSTDN_c_strategy, parameters->cParams.strategy));
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_setParameter(
		cctx, ZSTDN_c_contentSizeFlag, parameters->fParams.contentSizeFlag));
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_setParameter(
		cctx, ZSTDN_c_checksumFlag, parameters->fParams.checksumFlag));
	ZSTDN_FORWARD_IF_ERR(ZSTDN_CCtx_setParameter(
		cctx, ZSTDN_c_dictIDFlag, !parameters->fParams.noDictIDFlag));
	return 0;
}

int zstd_min_clevel(void)
{
	return ZSTDN_minCLevel();
}

int zstd_max_clevel(void)
{
	return ZSTDN_maxCLevel();
}

size_t zstd_compress_bound(size_t src_size)
{
	return ZSTDN_compressBound(src_size);
}

zstd_parameters zstd_get_params(int level,
	unsigned long long estimated_src_size)
{
	return ZSTDN_getParams(level, estimated_src_size, 0);
}

size_t zstd_cctx_workspace_bound(const zstd_compression_parameters *cparams)
{
	return ZSTDN_estimateCCtxSize_usingCParams(*cparams);
}

zstd_cctx *zstd_init_cctx(void *workspace, size_t workspace_size)
{
	if (workspace == NULL)
		return NULL;
	return ZSTDN_initStaticCCtx(workspace, workspace_size);
}

size_t zstd_compress_cctx(zstd_cctx *cctx, void *dst, size_t dst_capacity,
	const void *src, size_t src_size, const zstd_parameters *parameters)
{
	ZSTDN_FORWARD_IF_ERR(zstd_cctx_init(cctx, parameters, src_size));
	return ZSTDN_compress2(cctx, dst, dst_capacity, src, src_size);
}

size_t zstd_cstream_workspace_bound(const zstd_compression_parameters *cparams)
{
	return ZSTDN_estimateCStreamSize_usingCParams(*cparams);
}

zstd_cstream *zstd_init_cstream(const zstd_parameters *parameters,
	unsigned long long pledged_src_size, void *workspace, size_t workspace_size)
{
	zstd_cstream *cstream;

	if (workspace == NULL)
		return NULL;

	cstream = ZSTDN_initStaticCStream(workspace, workspace_size);
	if (cstream == NULL)
		return NULL;

	/* 0 means unknown in linux zstd API but means 0 in new zstd API */
	if (pledged_src_size == 0)
		pledged_src_size = ZSTDN_CONTENTSIZE_UNKNOWN;

	if (ZSTDN_isError(zstd_cctx_init(cstream, parameters, pledged_src_size)))
		return NULL;

	return cstream;
}

size_t zstd_reset_cstream(zstd_cstream *cstream,
	unsigned long long pledged_src_size)
{
	return ZSTDN_resetCStream(cstream, pledged_src_size);
}

size_t zstd_compress_stream(zstd_cstream *cstream, zstd_out_buffer *output,
	zstd_in_buffer *input)
{
	return ZSTDN_compressStream(cstream, output, input);
}

size_t zstd_flush_stream(zstd_cstream *cstream, zstd_out_buffer *output)
{
	return ZSTDN_flushStream(cstream, output);
}

size_t zstd_end_stream(zstd_cstream *cstream, zstd_out_buffer *output)
{
	return ZSTDN_endStream(cstream, output);
}

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Zstd Compressor");
