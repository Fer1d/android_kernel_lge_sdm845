/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_F2FS_EXTM_H
#define _LINUX_F2FS_EXTM_H
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
struct file;
struct block_device;
struct f2fs_extm_lease;
struct f2fs_extm_extent {
	sector_t logical_sector;
	sector_t physical_sector;
	sector_t nr_sectors;
};
#if IS_REACHABLE(CONFIG_F2FS_FS)
int f2fs_extm_acquire(struct file *, u64, struct f2fs_extm_lease **);
struct block_device *f2fs_extm_bdev(const struct f2fs_extm_lease *);
unsigned int f2fs_extm_nr_extents(const struct f2fs_extm_lease *);
const struct f2fs_extm_extent *f2fs_extm_extents(const struct f2fs_extm_lease *);
/* Caller must stop submissions, drain all raw I/O and flush before release. */
void f2fs_extm_release(struct f2fs_extm_lease *);
#else
static inline int f2fs_extm_acquire(struct file *f, u64 size,
		struct f2fs_extm_lease **lease) { return -EOPNOTSUPP; }
static inline struct block_device *f2fs_extm_bdev(const struct f2fs_extm_lease *l)
{ return NULL; }
static inline unsigned int f2fs_extm_nr_extents(const struct f2fs_extm_lease *l)
{ return 0; }
static inline const struct f2fs_extm_extent *f2fs_extm_extents(
		const struct f2fs_extm_lease *l) { return NULL; }
static inline void f2fs_extm_release(struct f2fs_extm_lease *l) { }
#endif
#endif
