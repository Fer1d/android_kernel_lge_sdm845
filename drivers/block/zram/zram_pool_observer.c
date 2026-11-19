// SPDX-License-Identifier: GPL-2.0
/* On-demand diagnostics for the original zram zsmalloc pool. */
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/timekeeping.h>

#include "zram_drv.h"

#define ZRAM_POOL_LOW_MIB	512U
#define ZRAM_POOL_HIGH_MIB	768U
#define ZRAM_POOL_SCAN_SLOTS	1024U
#define ZRAM_POOL_AGE_SECONDS	60U
#define ZRAM_POOL_AGE_NS	((s64)ZRAM_POOL_AGE_SECONDS * NSEC_PER_SEC)
#define ZRAM_POOL_MIB		(1ULL << 20)

struct zram_pool_sample {
	u64 pool_bytes;
	u64 scan_slots;
	u64 scan_start;
	unsigned int visited;
	unsigned int busy;
	unsigned int eligible;
	unsigned int aged;
	unsigned int age_unknown;
	bool initialized;
	bool high;
	bool backing;
};

#ifdef CONFIG_ZRAM_WRITEBACK
static void zram_pool_scan(struct zram *zram, struct zram_pool_sample *s)
{
	u64 cursor = zram->observer_cursor;
	s64 sample_ns = 0;

	/* Original zram slot helpers take a u32 index. */
	s->scan_slots = min_t(u64, zram->disksize >> PAGE_SHIFT, 1ULL << 32);
	s->scan_start = cursor;
#if defined(CONFIG_ZRAM_MEMORY_TRACKING) || defined(CONFIG_ZRAM_ACCESS_TRACKING)
	sample_ns = ktime_to_ns(ktime_get_boottime());
#endif
	while (s->visited < min_t(u64, ZRAM_POOL_SCAN_SLOTS, s->scan_slots)) {
		s64 access_ns;
		int rc;

		rc = zram_pool_observer_slot(zram, cursor, &access_ns);
		if (rc < 0) {
			s->busy++;
		} else if (rc) {
			s->eligible++;
			if ((IS_ENABLED(CONFIG_ZRAM_MEMORY_TRACKING) || IS_ENABLED(CONFIG_ZRAM_ACCESS_TRACKING))) {
				if (access_ns <= 0 || access_ns > sample_ns)
					s->age_unknown++;
				else if (sample_ns - access_ns >=
					 ZRAM_POOL_AGE_NS)
					s->aged++;
			}
		}
		s->visited++;
		if (++cursor == s->scan_slots)
			cursor = 0;
	}
	zram->observer_cursor = cursor;
}
#endif

/* Serialize readers; init_lock pins the pool and excludes reset. */
static void zram_pool_collect(struct zram *zram, struct zram_pool_sample *s)
{
	mutex_lock(&zram->observer_lock);
	down_read(&zram->init_lock);
#ifdef CONFIG_ZRAM_WRITEBACK
	s->backing = !!zram->backing_dev;
#endif
	if (!zram->disksize)
		goto out;

	s->initialized = true;
	s->pool_bytes = (u64)zs_get_total_pages(zram->mem_pool) << PAGE_SHIFT;
	if (s->pool_bytes >= ZRAM_POOL_HIGH_MIB * ZRAM_POOL_MIB)
		zram->observer_high = true;
	else if (s->pool_bytes <= ZRAM_POOL_LOW_MIB * ZRAM_POOL_MIB)
		zram->observer_high = false;
	s->high = zram->observer_high;
#ifdef CONFIG_ZRAM_WRITEBACK
	zram_pool_scan(zram, s);
#endif
out:
	up_read(&zram->init_lock);
	mutex_unlock(&zram->observer_lock);
}

static int zram_pool_show(struct seq_file *m, void *unused)
{
	struct zram_pool_sample s = { };

	zram_pool_collect(m->private, &s);
	seq_printf(m, "initialized %u\nphysical_pool_bytes %llu\n"
		   "low_mib %u\nhigh_mib %u\npressure %s\nbacking_attached %u\n",
		   s.initialized, s.pool_bytes, ZRAM_POOL_LOW_MIB,
		   ZRAM_POOL_HIGH_MIB, !s.initialized ? "uninitialized" :
		   s.high ? "high" : "low", s.backing);
	if (!IS_ENABLED(CONFIG_ZRAM_WRITEBACK)) {
		seq_puts(m, "eligibility unsupported\naccess_age unsupported\n");
		return 0;
	}

	seq_printf(m, "eligibility filtered_idle\nmin_idle_markings 1\n"
		   "scan_limit %u\nscan_slots %llu\nscan_start %llu\n"
		   "slots_visited %u\nslots_busy %u\neligible_slots %u\n",
		   ZRAM_POOL_SCAN_SLOTS, s.scan_slots, s.scan_start,
		   s.visited, s.busy, s.eligible);
	if ((IS_ENABLED(CONFIG_ZRAM_MEMORY_TRACKING) || IS_ENABLED(CONFIG_ZRAM_ACCESS_TRACKING)))
		seq_printf(m, "access_age boottime\nmin_age_seconds %u\n"
			   "eligible_aged_slots %u\neligible_age_unknown_slots %u\n",
			   ZRAM_POOL_AGE_SECONDS, s.aged, s.age_unknown);
	else
		seq_puts(m, "access_age unsupported\n");
	return 0;
}

static int zram_pool_open(struct inode *inode, struct file *file)
{
	return single_open(file, zram_pool_show, inode->i_private);
}

static const struct file_operations zram_pool_fops = {
	.owner = THIS_MODULE,
	.open = zram_pool_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

void zram_pool_observer_register(struct zram *zram)
{
	/* The full debugfs proxy drains active callbacks before removal. */
	debugfs_create_file("pool_observer", 0400, zram->debugfs_dir,
			    zram, &zram_pool_fops);
}
