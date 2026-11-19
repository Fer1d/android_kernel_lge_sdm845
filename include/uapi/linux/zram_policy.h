/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _ZRAM_POLICY_UAPI_H
#define _ZRAM_POLICY_UAPI_H

#include <linux/zram.h>

#define ZRAM_WB_POLICY_ABI_VERSION 1
#define ZRAM_WB_STOP_SCANNED_LIMIT 11

/* Separate command: never pass this structure to the v1 EXEC command.
 * exec.size is sizeof(struct zram_wb_policy_exec), exec.version is the policy
 * version. exec.mode/page_index/idle_count, all outputs and reserved are zero.
 * exec identity, request_id, accounting and copyout rules are those of v1.
 * Limits: 0 < installed <= attempted <= scanned < U64_MAX.
 * Age is positive boottime nanoseconds; unknown/future access times are skipped.
 * next_index is output only. A call scans at most one wrapping device traversal.
 */
struct zram_wb_policy_exec {
	struct zram_wb_exec exec;
	__aligned_u64 demand_sequence;
	__aligned_u64 max_scanned_pages;
	__aligned_u64 min_age_ns;
	__aligned_u64 start_index;
	__aligned_u64 next_index;
	__aligned_u64 reserved[3];
};

#define ZRAM_IOC_WB_POLICY_EXEC _IOWR('Z', 0x22, struct zram_wb_policy_exec)
#endif
