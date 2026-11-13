// SPDX-License-Identifier: GPL-2.0
/* Exclusive raw-data leases. This is not the advisory F2FS pin-file ABI. */
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/blkdev.h>
#include <linux/pagemap.h>
#include <linux/sort.h>
#include <linux/vmalloc.h>
#include <linux/f2fs_extm.h>
#include <linux/f2fs_fs.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/mount.h>
#include "f2fs.h"
#include "node.h"
#include "segment.h"

#define EXTM_PREPARING 1
#define EXTM_ACTIVE 2
#define EXTM_MAX_BYTES (1ULL << 32)
#define EXTM_MAX_BLOCKS (EXTM_MAX_BYTES >> 12)

struct f2fs_extm_lease {
	struct delayed_work retry;
	struct file *file;
	struct block_device *bdev;
	struct f2fs_extm_extent *extents;
	unsigned int nr;
};

static void extm_gc_lock(struct inode *inode)
{
	down_write(&F2FS_I(inode)->extm_cache);
	/* Same order as OPU DIO; GC takes the opposite order with trylocks. */
	down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	down_write(&F2FS_I(inode)->i_gc_rwsem[READ]);
	down_write(&F2FS_I(inode)->i_mmap_sem);
}

static void extm_gc_unlock(struct inode *inode)
{
	up_write(&F2FS_I(inode)->i_mmap_sem);
	up_write(&F2FS_I(inode)->i_gc_rwsem[READ]);
	up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	up_write(&F2FS_I(inode)->extm_cache);
}

static int extm_block_cmp(const void *a, const void *b)
{
	block_t x = *(const block_t *)a, y = *(const block_t *)b;

	return (x > y) - (x < y);
}

static int extm_scan(struct inode *inode, struct f2fs_extm_lease *lease,
		unsigned int blocks)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	block_t *physical;
	unsigned int i;
	int ret = 0;

	physical = kvmalloc((size_t)blocks * sizeof(*physical), GFP_KERNEL);
	if (!physical)
		return -ENOMEM;
	for (i = 0; i < blocks; i++) {
		struct f2fs_map_blocks map = { .m_lblk = i, .m_len = 1 };
		struct f2fs_extm_extent *e;
		sector_t sector;

		/* Never allocate or convert unwritten blocks. */
		ret = f2fs_map_blocks(inode, &map, 0, F2FS_GET_BLOCK_DEFAULT);
		if (ret)
			goto out;
		if (!map.m_len || !(map.m_flags & F2FS_MAP_MAPPED) ||
		    map.m_pblk == NULL_ADDR || map.m_pblk == NEW_ADDR ||
		    !f2fs_is_valid_blkaddr(sbi, map.m_pblk, DATA_GENERIC)) {
			ret = -EINVAL;
			goto out;
		}
		physical[i] = map.m_pblk;
		sector = (sector_t)map.m_pblk << 3;
		if (sector + 8 > i_size_read(lease->bdev->bd_inode) >> 9) {
			ret = -EINVAL;
			goto out;
		}
		if (lease->nr) {
			e = &lease->extents[lease->nr - 1];
			if (e->physical_sector + e->nr_sectors == sector) {
				e->nr_sectors += 8;
				continue;
			}
		}
		e = &lease->extents[lease->nr++];
		e->logical_sector = (sector_t)i << 3;
		e->physical_sector = sector;
		e->nr_sectors = 8;
		cond_resched();
	}
	sort(physical, blocks, sizeof(*physical), extm_block_cmp, NULL);
	for (i = 1; i < blocks; i++)
		if (physical[i] == physical[i - 1]) {
			ret = -EFSCORRUPTED;
			break;
		}
 out:
	kvfree(physical);
	return ret;
}

/* The queued work owns the entire lease until cache invalidation succeeds. */
static void extm_release_retry(struct work_struct *work)
{
	struct f2fs_extm_lease *lease = container_of(to_delayed_work(work),
			struct f2fs_extm_lease, retry);

	f2fs_extm_release(lease);
}

int f2fs_extm_acquire(struct file *file, u64 expected_size,
		struct f2fs_extm_lease **result)
{
	struct inode *inode = file_inode(file);
	struct f2fs_inode_info *fi;
	struct f2fs_sb_info *sbi;
	struct f2fs_extm_lease *lease;
	bool denied = false, mapping_denied = false, preparing = false;
	unsigned int blocks;
	int ret;

	if (!result)
		return -EINVAL;
	*result = NULL;
	if (file->f_op != &f2fs_file_operations || !S_ISREG(inode->i_mode) ||
	    !(file->f_mode & FMODE_READ) || (file->f_mode & FMODE_WRITE))
		return -EINVAL;
	if (PAGE_SIZE != 4096 || inode->i_sb->s_blocksize != 4096 ||
	    !expected_size || expected_size > EXTM_MAX_BYTES ||
	    !IS_ALIGNED(expected_size, 4096))
		return -EINVAL;
	fi = F2FS_I(inode);
	sbi = F2FS_I_SB(inode);
	/* No recursion through DM, loop, multi-device or zoned F2FS. */
	if (sbi->s_ndevs || !inode->i_sb->s_bdev ||
	    !strncmp(inode->i_sb->s_bdev->bd_disk->disk_name, "dm-", 3) ||
	    !strncmp(inode->i_sb->s_bdev->bd_disk->disk_name, "loop", 4))
		return -EOPNOTSUPP;
	blocks = expected_size >> 12;
	if (blocks > EXTM_MAX_BLOCKS)
		return -E2BIG;
	lease = kzalloc(sizeof(*lease), GFP_KERNEL);
	if (!lease)
		return -ENOMEM;
	lease->extents = kvmalloc((size_t)blocks * sizeof(*lease->extents), GFP_KERNEL);
	if (!lease->extents) {
		kfree(lease);
		return -ENOMEM;
	}
	INIT_DELAYED_WORK(&lease->retry, extm_release_retry);
	lease->bdev = inode->i_sb->s_bdev;
	/* Freeze may quiesce first, but cannot complete with a provisional lease. */
	ret = mnt_want_write(file->f_path.mnt);
	if (ret)
		goto free_lease;
	sb_start_write(inode->i_sb);
	mutex_lock(&sbi->extm_mutex);
	if (sbi->extm_management || f2fs_readonly(inode->i_sb) ||
	    f2fs_cp_error(sbi) || is_sbi_flag_set(sbi, SBI_IS_SHUTDOWN) ||
	    is_sbi_flag_set(sbi, SBI_CP_DISABLED)) {
		mutex_unlock(&sbi->extm_mutex);
		ret = -EBUSY;
		goto end_write;
	}
	sbi->extm_leases++;
	mutex_unlock(&sbi->extm_mutex);

	/* Admission before inode_lock, matching ioctl and mmap entry points. */
	down_write(&fi->extm_admission);
	inode_lock(inode);
	ret = -EBUSY;
	if (fi->extm_state || fi->extm_ever_mapped || mapping_mapped(inode->i_mapping) ||
	    IS_SWAPFILE(inode) || IS_ENCRYPTED(inode) ||
	    f2fs_has_inline_data(inode) || f2fs_is_atomic_file(inode) ||
	    f2fs_is_volatile_file(inode) || f2fs_is_pinned_file(inode))
		goto unlock;
	if (i_size_read(inode) != expected_size) {
		ret = -EINVAL;
		goto unlock;
	}
	ret = inode_permission(inode, MAY_WRITE);
	if (ret)
		goto unlock;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode)) {
		ret = -EPERM;
		goto unlock;
	}
	ret = deny_write_access(file);
	if (ret)
		goto unlock;
	denied = true;
	ret = mapping_deny_writable(inode->i_mapping);
	if (ret)
		goto unlock;
	mapping_denied = true;
	extm_gc_lock(inode);
	WRITE_ONCE(fi->extm_state, EXTM_PREPARING);
	preparing = true;
	extm_gc_unlock(inode);
	/* Admission drains synchronous reads; this also drains queued async DIO. */
	inode_dio_wait(inode);
	/* No GC locks or cp_rwsem held: fsync can run its own checkpoint. */
	ret = f2fs_sync_file(file, 0, LLONG_MAX, 0);
	if (ret)
		goto unlock;
	extm_gc_lock(inode);
	/* Wait page locks/readahead/writeback; pinned splice pages fail closed. */
	ret = invalidate_inode_pages2(inode->i_mapping);
	if (!ret)
		ret = extm_scan(inode, lease, blocks);
	if (!ret) {
		__module_get(THIS_MODULE);
		lease->file = get_file(file);
		WRITE_ONCE(fi->extm_state, EXTM_ACTIVE);
		*result = lease;
	}
	extm_gc_unlock(inode);
 unlock:
	if (ret) {
		if (preparing) {
			extm_gc_lock(inode);
			WRITE_ONCE(fi->extm_state, 0);
			extm_gc_unlock(inode);
		}
		if (mapping_denied)
			mapping_allow_writable(inode->i_mapping);
		if (denied)
			allow_write_access(file);
	}
	inode_unlock(inode);
	up_write(&fi->extm_admission);
	if (ret) {
		mutex_lock(&sbi->extm_mutex);
		sbi->extm_leases--;
		mutex_unlock(&sbi->extm_mutex);
	}
 end_write:
	sb_end_write(inode->i_sb);
	mnt_drop_write(file->f_path.mnt);
 free_lease:
	if (ret) {
		kvfree(lease->extents);
		kfree(lease);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(f2fs_extm_acquire);

struct block_device *f2fs_extm_bdev(const struct f2fs_extm_lease *lease)
{
	return lease->bdev;
}
EXPORT_SYMBOL_GPL(f2fs_extm_bdev);

unsigned int f2fs_extm_nr_extents(const struct f2fs_extm_lease *lease)
{
	return lease->nr;
}
EXPORT_SYMBOL_GPL(f2fs_extm_nr_extents);

const struct f2fs_extm_extent *f2fs_extm_extents(const struct f2fs_extm_lease *lease)
{
	return lease->extents;
}
EXPORT_SYMBOL_GPL(f2fs_extm_extents);

void f2fs_extm_release(struct f2fs_extm_lease *lease)
{
	struct file *file;
	struct inode *inode;
	struct f2fs_sb_info *sbi;

	if (!lease)
		return;
	file = lease->file;
	inode = file_inode(file);
	sbi = F2FS_I_SB(inode);
	down_write(&F2FS_I(inode)->extm_admission);
	inode_lock(inode);
	extm_gc_lock(inode);
	/* Never fsync stale cached data over completed raw writes. */
	if (invalidate_inode_pages2(inode->i_mapping)) {
		/* Fail closed: keep deny-write, ACTIVE, mount and module references. */
		pr_err_ratelimited("F2FS EXTM: inode %lu release quarantined; retrying\n",
				inode->i_ino);
		extm_gc_unlock(inode);
		inode_unlock(inode);
		up_write(&F2FS_I(inode)->extm_admission);
		queue_delayed_work(system_unbound_wq, &lease->retry, 5 * HZ);
		return;
	}
	WRITE_ONCE(F2FS_I(inode)->extm_state, 0);
	mapping_allow_writable(inode->i_mapping);
	allow_write_access(file);
	extm_gc_unlock(inode);
	inode_unlock(inode);
	up_write(&F2FS_I(inode)->extm_admission);
	mutex_lock(&sbi->extm_mutex);
	sbi->extm_leases--;
	mutex_unlock(&sbi->extm_mutex);
	kvfree(lease->extents);
	kfree(lease);
	fput(file);
	module_put(THIS_MODULE);
}
EXPORT_SYMBOL_GPL(f2fs_extm_release);
