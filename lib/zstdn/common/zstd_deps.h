/* SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause */
/*
 * Copyright (c) Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/*
 * This file provides common libc dependencies that zstd requires.
 * The purpose is to allow replacing this file with a custom implementation
 * to compile zstd without libc support.
 */

/* Need:
 * NULL
 * INT_MAX
 * UINT_MAX
 * ZSTDN_memcpy()
 * ZSTDN_memset()
 * ZSTDN_memmove()
 */
#ifndef ZSTDN_DEPS_COMMON
#define ZSTDN_DEPS_COMMON

#include <linux/limits.h>
#include <linux/stddef.h>

#define ZSTDN_memcpy(d,s,n) __builtin_memcpy((d),(s),(n))
#define ZSTDN_memmove(d,s,n) __builtin_memmove((d),(s),(n))
#define ZSTDN_memset(d,s,n) __builtin_memset((d),(s),(n))

#endif /* ZSTDN_DEPS_COMMON */

/*
 * Define malloc as always failing. That means the user must
 * either use ZSTDN_customMem or statically allocate memory.
 * Need:
 * ZSTDN_malloc()
 * ZSTDN_free()
 * ZSTDN_calloc()
 */
#ifdef ZSTDN_DEPS_NEED_MALLOC
#ifndef ZSTDN_DEPS_MALLOC
#define ZSTDN_DEPS_MALLOC

#define ZSTDN_malloc(s) ({ (void)(s); NULL; })
#define ZSTDN_free(p) ((void)(p))
#define ZSTDN_calloc(n,s) ({ (void)(n); (void)(s); NULL; })

#endif /* ZSTDN_DEPS_MALLOC */
#endif /* ZSTDN_DEPS_NEED_MALLOC */

/*
 * Provides 64-bit math support.
 * Need:
 * U64 ZSTDN_div64(U64 dividend, U32 divisor)
 */
#ifdef ZSTDN_DEPS_NEED_MATH64
#ifndef ZSTDN_DEPS_MATH64
#define ZSTDN_DEPS_MATH64

#include <linux/math64.h>

static uint64_t ZSTDN_div64(uint64_t dividend, uint32_t divisor) {
  return div_u64(dividend, divisor);
}

#endif /* ZSTDN_DEPS_MATH64 */
#endif /* ZSTDN_DEPS_NEED_MATH64 */

/*
 * This is only requested when DEBUGLEVEL >= 1, meaning
 * it is disabled in production.
 * Need:
 * assert()
 */
#ifdef ZSTDN_DEPS_NEED_ASSERT
#ifndef ZSTDN_DEPS_ASSERT
#define ZSTDN_DEPS_ASSERT

#include <linux/kernel.h>

#define assert(x) WARN_ON((x))

#endif /* ZSTDN_DEPS_ASSERT */
#endif /* ZSTDN_DEPS_NEED_ASSERT */

/*
 * This is only requested when DEBUGLEVEL >= 2, meaning
 * it is disabled in production.
 * Need:
 * ZSTDN_DEBUG_PRINT()
 */
#ifdef ZSTDN_DEPS_NEED_IO
#ifndef ZSTDN_DEPS_IO
#define ZSTDN_DEPS_IO

#include <linux/printk.h>

#define ZSTDN_DEBUG_PRINT(...) pr_debug(__VA_ARGS__)

#endif /* ZSTDN_DEPS_IO */
#endif /* ZSTDN_DEPS_NEED_IO */

/*
 * Only requested when MSAN is enabled.
 * Need:
 * intptr_t
 */
#ifdef ZSTDN_DEPS_NEED_STDINT
#ifndef ZSTDN_DEPS_STDINT
#define ZSTDN_DEPS_STDINT

/*
 * The Linux Kernel doesn't provide intptr_t, only uintptr_t, which
 * is an unsigned long.
 */
typedef long intptr_t;

#endif /* ZSTDN_DEPS_STDINT */
#endif /* ZSTDN_DEPS_NEED_STDINT */
