// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define __ARG_PLACEHOLDER_1 0,
#define __enabled_arg(ignore, value, ...) value
#define __enabled_placeholder(arg) __enabled_arg(arg 1, 0)
#define __enabled_value(value) __enabled_placeholder(__ARG_PLACEHOLDER_##value)
#define IS_ENABLED(x) __enabled_value(x)
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define BIT(n) (1UL << (n))
#define NSEC_PER_SEC 1000000000L

typedef uint32_t u32;
typedef unsigned long long u64;
typedef long long s64;

#include "observer_flags.h"

struct entry {
	unsigned long flags;
	s64 ac_time;
};

struct zram {
	pthread_mutex_t observer_lock;
	pthread_rwlock_t init_lock;
	u64 disksize, observer_cursor;
	bool observer_high;
	void *backing_dev;
	unsigned long *mem_pool;
	struct entry *table;
};

struct seq_file {
	void *private;
	char buf[1024];
};

static unsigned long zs_get_total_pages(unsigned long *pages)
{
	assert(pages);
	return *pages;
}

static void seq_printf(struct seq_file *seq, const char *fmt, ...)
{
	size_t offset = strlen(seq->buf);
	va_list args;
	int len;

	va_start(args, fmt);
	len = vsnprintf(seq->buf + offset, sizeof(seq->buf) - offset, fmt, args);
	va_end(args);
	assert(len >= 0 && (size_t)len < sizeof(seq->buf) - offset);
}

#define seq_puts(seq, str) seq_printf(seq, "%s", str)
#define mutex_lock(lock) assert(!pthread_mutex_lock(lock))
#define mutex_unlock(lock) assert(!pthread_mutex_unlock(lock))
#define down_read(lock) assert(!pthread_rwlock_rdlock(lock))
#define up_read(lock) assert(!pthread_rwlock_unlock(lock))

#ifdef CONFIG_ZRAM_WRITEBACK
static int bit_spin_trylock(int bit, unsigned long *flags)
{
	if (*flags & BIT(bit))
		return 0;
	*flags |= BIT(bit);
	return 1;
}

static void bit_spin_unlock(int bit, unsigned long *flags)
{
	assert(*flags & BIT(bit));
	*flags &= ~BIT(bit);
}
#endif

#ifdef CONFIG_ZRAM_MEMORY_TRACKING
#define NOW_NS (120LL * NSEC_PER_SEC)
#define ktime_to_ns(time) (time)
static s64 ktime_get_boottime(void)
{
	return NOW_NS;
}
#endif

#include "observer_core.h"

static void reset(struct zram *zram)
{
	assert(!pthread_rwlock_wrlock(&zram->init_lock));
#include "observer_reset.h"
	zram->disksize = 0;
	zram->table = NULL;
	zram->mem_pool = NULL;
	assert(!pthread_rwlock_unlock(&zram->init_lock));
}

static void show(struct zram *zram, const char *expected)
{
	struct seq_file seq = { .private = zram };

	assert(!zram_pool_show(&seq, NULL));
	assert(strstr(seq.buf, expected));
}

#ifdef CONFIG_ZRAM_WRITEBACK
static void *reader(void *data)
{
	struct zram_pool_sample s = { };

	zram_pool_collect(data, &s);
	return (void *)(uintptr_t)(s.scan_start + 1);
}
#endif

int main(void)
{
	struct entry table[ZRAM_POOL_SCAN_SLOTS * 2];
	unsigned long pages = 0;
	struct zram z = {
		.observer_lock = PTHREAD_MUTEX_INITIALIZER,
		.init_lock = PTHREAD_RWLOCK_INITIALIZER,
	};
	const unsigned long low = ZRAM_POOL_LOW_MIB * ZRAM_POOL_MIB >> PAGE_SHIFT;
	const unsigned long high = ZRAM_POOL_HIGH_MIB * ZRAM_POOL_MIB >> PAGE_SHIFT;
	struct zram_pool_sample s;

	show(&z, "initialized 0\nphysical_pool_bytes 0\n");
	show(&z, "pressure uninitialized\n");
#ifdef CONFIG_ZRAM_WRITEBACK
	z.backing_dev = &z;
	show(&z, "pressure uninitialized\nbacking_attached 1\n");
	z.backing_dev = NULL;
#endif
	z.mem_pool = &pages;
	z.table = table;
	z.disksize = (u64)sizeof(table) / sizeof(table[0]) << PAGE_SHIFT;
	memset(table, 0, sizeof(table));
	pages = high - 1;
	show(&z, "pressure low\nbacking_attached 0\n");
	pages++;
	show(&z, "pressure high\nbacking_attached 0\n");
	pages = low + 1;
	show(&z, "pressure high\n");
	pages--;
	show(&z, "pressure low\n");

#ifdef CONFIG_ZRAM_WRITEBACK
	{
		unsigned long good = 128 | BIT(ZRAM_IDLE) | BIT(ZRAM_COMPRESS_LOW) |
				     BIT(ZRAM_IDLE_COUNT_SHIFT);
		pthread_t a, b;
		void *a_start, *b_start;

		z.observer_cursor = 0;
		table[0].flags = good;
		table[1].flags = good | BIT(ZRAM_LOCK);
		table[2].flags = good | BIT(ZRAM_SAME);
		table[3].flags = good | BIT(ZRAM_WB);
		table[4].flags = good | BIT(ZRAM_UNDER_WB);
		table[5].flags = good & ~BIT(ZRAM_IDLE);
		table[6].flags = good & ~BIT(ZRAM_COMPRESS_LOW);
		table[7].flags = good & ~BIT(ZRAM_IDLE_COUNT_SHIFT);
		table[8].flags = good & ~128UL;
		table[9].flags = good;
		table[10].flags = good;
		table[11].flags = good;
		table[ZRAM_POOL_SCAN_SLOTS].flags = good;
#ifdef CONFIG_ZRAM_MEMORY_TRACKING
		table[0].ac_time = NOW_NS - 60LL * NSEC_PER_SEC;
		table[9].ac_time = table[0].ac_time + 1;
		table[11].ac_time = NOW_NS + 1;
#endif
		s = (struct zram_pool_sample){ };
		zram_pool_collect(&z, &s);
		assert(s.scan_start == 0 && s.visited == ZRAM_POOL_SCAN_SLOTS);
		assert(!s.backing && s.eligible == 4 && s.busy == 1);
		assert(table[0].flags == good && table[1].flags == (good | BIT(ZRAM_LOCK)));
#ifdef CONFIG_ZRAM_MEMORY_TRACKING
		assert(s.aged == 1 && s.age_unknown == 2);
		show(&z, "access_age boottime\nmin_age_seconds 60\n");
#else
		assert(!s.aged && !s.age_unknown);
		show(&z, "access_age unsupported\n");
#endif
		assert(z.observer_cursor == 0);
		assert(!pthread_create(&a, NULL, reader, &z));
		assert(!pthread_create(&b, NULL, reader, &z));
		assert(!pthread_join(a, &a_start));
		assert(!pthread_join(b, &b_start));
		assert(((uintptr_t)a_start == 1 && (uintptr_t)b_start == ZRAM_POOL_SCAN_SLOTS + 1) ||
		       ((uintptr_t)b_start == 1 && (uintptr_t)a_start == ZRAM_POOL_SCAN_SLOTS + 1));
		assert(z.observer_cursor == 0);
		z.backing_dev = &z;
		show(&z, "backing_attached 1\n");
	}
#else
	show(&z, "eligibility unsupported\naccess_age unsupported\n");
#endif
	pages = high;
	s = (struct zram_pool_sample){ };
	zram_pool_collect(&z, &s);
	assert(s.high && s.pool_bytes == high * (1ULL << PAGE_SHIFT));
	z.observer_cursor = 1;
	reset(&z);
	assert(!z.observer_high && !z.observer_cursor);
	show(&z, "pressure uninitialized\n");
	z.disksize = 3ULL << PAGE_SHIFT;
	z.mem_pool = &pages;
	z.table = table;
	pages = low + 1;
	s = (struct zram_pool_sample){ };
	zram_pool_collect(&z, &s);
	assert(!s.high && !z.observer_cursor);
#ifdef CONFIG_ZRAM_WRITEBACK
	assert(s.visited == 3);
#endif
	return 0;
}
