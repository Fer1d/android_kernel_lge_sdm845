// SPDX-License-Identifier: GPL-2.0
/* Raw, immutable F2FS extent-lease mapping.  No loop or userspace extents. */
#include "dm.h"
#include <linux/bio.h>
#include <linux/file.h>
#include <linux/f2fs_extm.h>
#include <linux/slab.h>
#include <linux/module.h>

#define DM_MSG_PREFIX "extm-file"
#define EXTM_MAX_BYTES (4ULL << 30)

struct extm_c {
	struct f2fs_extm_lease *lease;
	struct dm_dev *dev;
	const struct f2fs_extm_extent *extents;
	unsigned int nr_extents;
	u64 bytes;
	unsigned long ino;
};

/* Pin each traversed directory, never follow a symlink in any component.
 * This kernel predates LOOKUP_NO_SYMLINKS.  Reject dot components rather
 * than letting a rooted component lookup escape its pinned directory.
 */
static struct file *extm_open(const char *name)
{
	struct file *dir, *next;
	char *copy, *cursor, *component;

	if (name[0] != '/' || !name[1] || name[strlen(name) - 1] == '/')
		return ERR_PTR(-EINVAL);
	copy = kstrdup(name + 1, GFP_KERNEL);
	if (!copy)
		return ERR_PTR(-ENOMEM);
	dir = filp_open("/", O_PATH | O_DIRECTORY, 0);
	if (IS_ERR(dir))
		goto out;
	cursor = copy;
	while ((component = strsep(&cursor, "/")) != NULL) {
		if (!*component || !strcmp(component, ".") ||
		    !strcmp(component, "..")) {
			next = ERR_PTR(-EINVAL);
		} else {
			next = file_open_root(dir->f_path.dentry, dir->f_path.mnt,
				component, cursor ? O_PATH | O_DIRECTORY | O_NOFOLLOW :
				O_RDONLY | O_LARGEFILE | O_NOFOLLOW, 0);
		}
		fput(dir);
		dir = next;
		if (IS_ERR(dir))
			break;
	}
out:
	kfree(copy);
	return dir;
}

static int extm_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct extm_c *ec;
	struct file *file;
	u64 bytes;
	sector_t covered = 0, capacity;
	unsigned int i;
	int r;

	if (argc != 2 || ti->begin || kstrtoull(argv[1], 10, &bytes) ||
	    !bytes || bytes > EXTM_MAX_BYTES || (bytes & 511) ||
	    ti->len != bytes >> SECTOR_SHIFT) {
		ti->error = "Expected <file> <exact-bytes>, 512-byte aligned, at most 4 GiB";
		return -EINVAL;
	}
	ec = kzalloc(sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;
	/* Read-only fd: raw writes are authorized exclusively by the lease. */
	file = extm_open(argv[0]);
	if (IS_ERR(file)) {
		r = PTR_ERR(file);
		goto bad;
	}
	r = f2fs_extm_acquire(file, bytes, &ec->lease);
	if (r)
		goto bad_file;
	r = dm_get_device_extm(ti, file, ec->lease, f2fs_extm_bdev, &ec->dev);
	if (r)
		goto bad_lease;
	ec->extents = f2fs_extm_extents(ec->lease);
	ec->nr_extents = f2fs_extm_nr_extents(ec->lease);
	ec->bytes = bytes;
	if (!ec->nr_extents || !ec->extents) {
		r = -EINVAL;
		goto bad_dev;
	}
	capacity = i_size_read(ec->dev->bdev->bd_inode) >> SECTOR_SHIFT;
	for (i = 0; i < ec->nr_extents; i++) {
		const struct f2fs_extm_extent *e = &ec->extents[i];

		if (!e->nr_sectors || e->logical_sector != covered ||
		    covered > ti->len || e->nr_sectors > ti->len - covered ||
		    e->physical_sector > capacity ||
		    e->nr_sectors > capacity - e->physical_sector) {
			r = -EINVAL;
			goto bad_dev;
		}
		covered += e->nr_sectors;
	}
	if (covered != ti->len) {
		r = -EINVAL;
		goto bad_dev;
	}
	ec->ino = file_inode(file)->i_ino;
	filp_close(file, NULL);
	ti->private = ec;
	ti->num_flush_bios = 1;
	return 0;

bad_dev:
	dm_put_device(ti, ec->dev);
bad_lease:
	f2fs_extm_release(ec->lease);
bad_file:
	filp_close(file, NULL);
bad:
	ti->error = "Cannot acquire exclusive stable F2FS file lease";
	kfree(ec);
	return r;
}

static void extm_dtr(struct dm_target *ti)
{
	struct extm_c *ec = ti->private;
	int r;

	/* DM has retired this table and drained its submitted I/O.  Inactive
	 * tables have never submitted I/O.  Suspend alone never drops a lease.
	 * Do not invalidate the mounted bdev: release handles this file only.
	 */
	r = blkdev_issue_flush(ec->dev->bdev, GFP_KERNEL, NULL);
	if (r)
		DMERR("raw backing flush failed on lease release: %d", r);
	dm_put_device(ti, ec->dev);
	f2fs_extm_release(ec->lease);
	kfree(ec);
}

static int extm_error(struct bio *bio, int error)
{
	bio->bi_error = error;
	bio_endio(bio);
	return DM_MAPIO_SUBMITTED;
}

static int extm_map(struct dm_target *ti, struct bio *bio)
{
	struct extm_c *ec = ti->private;
	const struct f2fs_extm_extent *e;
	sector_t sector = dm_target_offset(ti, bio->bi_iter.bi_sector);
	sector_t remaining;
	unsigned int lo = 0, hi = ec->nr_extents;

	if (bio_op(bio) != REQ_OP_READ && bio_op(bio) != REQ_OP_WRITE &&
	    bio_op(bio) != REQ_OP_FLUSH)
		return extm_error(bio, -EOPNOTSUPP);
	/* DM separates preflush from data before map; never partially accept it. */
	if (!bio_sectors(bio)) {
		bio->bi_bdev = ec->dev->bdev;
		bio->bi_iter.bi_sector = 0;
		return DM_MAPIO_REMAPPED;
	}
	if (sector >= ti->len || bio_sectors(bio) > ti->len - sector)
		return extm_error(bio, -EIO);
	if (bio->bi_opf & REQ_PREFLUSH || bio_op(bio) == REQ_OP_FLUSH)
		return extm_error(bio, -EIO);
	while (lo < hi) {
		unsigned int mid = lo + (hi - lo) / 2;

		if (ec->extents[mid].logical_sector <= sector)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo)
		return extm_error(bio, -EIO);
	e = &ec->extents[lo - 1];
	if (sector - e->logical_sector >= e->nr_sectors)
		return extm_error(bio, -EIO);
	remaining = e->nr_sectors - (sector - e->logical_sector);
	if (bio_sectors(bio) > remaining)
		dm_accept_partial_bio(bio, remaining);
	bio->bi_iter.bi_sector = e->physical_sector + sector - e->logical_sector;
	bio->bi_bdev = ec->dev->bdev;
	/* Leave FUA and other ordinary read/write flags intact. */
	return DM_MAPIO_REMAPPED;
}

static int extm_iterate_devices(struct dm_target *ti,
			       iterate_devices_callout_fn fn, void *data)
{
	struct extm_c *ec = ti->private;
	unsigned int i;
	int r;

	for (i = 0; i < ec->nr_extents; i++) {
		r = fn(ti, ec->dev, ec->extents[i].physical_sector,
		       ec->extents[i].nr_sectors, data);
		if (r)
			return r;
	}
	return 0;
}

static void extm_status(struct dm_target *ti, status_type_t type,
			unsigned int flags, char *result, unsigned int maxlen)
{
	struct extm_c *ec = ti->private;

	/* Do not advertise a pathname as a reopenable identity after rename. */
	snprintf(result, maxlen, "lease-v1 %s %lu %llu %u", ec->dev->name,
		 ec->ino, (unsigned long long)ec->bytes, ec->nr_extents);
}

static struct target_type extm_target = {
	.name = "extm-file",
	.version = {1, 0, 0},
	.features = DM_TARGET_SINGLETON,
	.module = THIS_MODULE,
	.ctr = extm_ctr,
	.dtr = extm_dtr,
	.map = extm_map,
	.iterate_devices = extm_iterate_devices,
	.status = extm_status,
};

static int __init extm_init(void)
{
	return dm_register_target(&extm_target);
}

static void __exit extm_exit(void)
{
	dm_unregister_target(&extm_target);
}
module_init(extm_init);
module_exit(extm_exit);
MODULE_DESCRIPTION("F2FS stable file extent lease target");
MODULE_LICENSE("GPL");
MODULE_ALIAS("dm-extm-file");
