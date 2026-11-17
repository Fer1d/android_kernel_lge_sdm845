/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_ZRAM_H
#define _UAPI_LINUX_ZRAM_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define ZRAM_WB_ABI_VERSION	1

#define ZRAM_WB_MODE_PAGE		0
#define ZRAM_WB_MODE_HUGE		1
#define ZRAM_WB_MODE_IDLE		2
#define ZRAM_WB_MODE_FILTERED_IDLE	3

#define ZRAM_WB_STOP_NONE		0
#define ZRAM_WB_STOP_COMPLETE		1
#define ZRAM_WB_STOP_NO_WORK		2
#define ZRAM_WB_STOP_ATTEMPT_LIMIT	3
#define ZRAM_WB_STOP_INSTALLED_LIMIT	4
#define ZRAM_WB_STOP_GLOBAL_LIMIT		5
#define ZRAM_WB_STOP_BACKING_FULL		6
#define ZRAM_WB_STOP_INTERRUPTED		7
#define ZRAM_WB_STOP_ERROR		8
#define ZRAM_WB_STOP_STALE		9
#define ZRAM_WB_STOP_NOT_READY		10

#define ZRAM_WB_STAGE_NONE		0
#define ZRAM_WB_STAGE_ALLOC		1
#define ZRAM_WB_STAGE_SNAPSHOT_READ	2
#define ZRAM_WB_STAGE_BIO_BUILD		3
#define ZRAM_WB_STAGE_IO			4

#define ZRAM_WB_CAP_EXEC			(1ULL << 0)
#define ZRAM_WB_CAP_STATUS		(1ULL << 1)
#define ZRAM_WB_CAP_FILTERED_IDLE		(1ULL << 2)
#define ZRAM_WB_CAP_DUAL_LIMIT		(1ULL << 3)

#define ZRAM_WB_STATE_INITIALIZED	(1U << 0)
#define ZRAM_WB_STATE_BACKING		(1U << 1)
#define ZRAM_WB_STATE_GLOBAL_LIMIT	(1U << 2)

/*
 * Exactly 160 bytes on native and compat ABIs; no pointers or native longs.
 * Zero the whole struct, then set size, version and the input fields.
 * flags, reserved0, reserved[] and every output field MUST be zero on input.
 * request_id is a nonzero correlation ID, NOT a deduplication/replay key.
 * Budgets and counters use kernel pages, not 4KiB units. Either zero budget
 * requests no work; max_installed_pages must not exceed max_attempt_pages.
 * idle_count is 0..10 for FILTERED_IDLE and zero otherwise. page_index is
 * zero except in PAGE mode. expected_instance/generation come from STATUS.
 * An accepted operation returns ioctl success and a negative errno in
 * op_errno (or zero), including stale/not-ready results. Malformed input,
 * permission and transport errors use the syscall errno.
 * EFAULT on copyout can follow effects: never blindly retry/replay EXEC.
 */
struct zram_wb_exec {
	__u32 size;
	__u32 version;
	__u32 flags;
	__u32 mode;
	__aligned_u64 request_id;
	__aligned_u64 expected_instance;
	__aligned_u64 expected_generation;
	__aligned_u64 max_attempt_pages;
	__aligned_u64 max_installed_pages;
	__aligned_u64 page_index;
	__u32 idle_count;
	__u32 reserved0;
	__aligned_u64 reserved[3];
	/* Outputs: request-local counts, not global-statistic differences. */
	__aligned_u64 scanned_pages;
	__aligned_u64 selected_pages;
	__aligned_u64 submitted_pages;
	__aligned_u64 installed_pages;
	__aligned_u64 invalidated_pages;
	__aligned_u64 duration_ns;
	__u32 stop_reason;
	__s32 op_errno;
	__u32 error_stage;
	__u32 reserved_out;
};

/*
 * Exactly 192 bytes. Only size and version are nonzero on input.
 * CAP_SYS_ADMIN is required; a read-only descriptor is sufficient.
 * Lifecycle identity/state is protected by init_lock. Statistics and the
 * remaining global limit are individually sampled, NOT a coherent snapshot.
 * STATUS does not retrieve or certify the result of a previous EXEC.
 */
struct zram_wb_status {
	__u32 size;
	__u32 version;
	__u32 flags;
	__u32 reserved0;
	__aligned_u64 reserved[2];
	/* Outputs */
	__aligned_u64 capabilities;
	__aligned_u64 instance;
	__aligned_u64 generation;
	__aligned_u64 disksize_bytes;
	__u32 page_size;
	__u32 state_flags;
	__aligned_u64 wb_limit_remaining_4k;
	__aligned_u64 pages_stored;
	__aligned_u64 same_pages;
	__aligned_u64 huge_pages;
	__aligned_u64 lowratio_pages;
	__aligned_u64 bd_count_pages;
	__aligned_u64 bd_reads_pages;
	__aligned_u64 bd_writes_pages;
	__aligned_u64 compr_data_bytes;
	__aligned_u64 mem_used_bytes;
	__aligned_u64 reserved_out[5];
};

#define ZRAM_IOC_WB_EXEC		_IOWR('Z', 0x20, struct zram_wb_exec)
#define ZRAM_IOC_WB_STATUS	_IOWR('Z', 0x21, struct zram_wb_status)

#endif /* _UAPI_LINUX_ZRAM_H */
