// SPDX-License-Identifier: GPL-2.0
/* Host simulation only. actual_helpers.h is extracted afresh from the driver.
 * No devices or kernel execution. Assertions are runtime checks.
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <linux/zram.h>

typedef uint64_t sector_t;
typedef unsigned int fmode_t;
#define U32_MAX UINT32_MAX
#define U64_MAX UINT64_MAX
#define __user
#define ENOIOCTLCMD 515
#define CAP_SYS_ADMIN 21
#define FMODE_READ 1
#define FMODE_WRITE 2

typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
#define S64_MAX INT64_MAX
#define ktime_set(s, ns) ((s) * 1000000000LL + (ns))
typedef long long atomic64_t;
#define CONFIG_ZRAM_WRITEBACK 1
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define SECTORS_PER_PAGE (PAGE_SIZE >> 9)
#define BIT(n) (1UL << (n))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define WARN_ON_ONCE(x) assert(!(x))
#define REQ_OP_WRITE 1
#define REQ_SYNC 2
#define SLOTS 256
#define BLOCKS 512
#define WORD_BITS (sizeof(unsigned long) * 8)

struct page { unsigned char data[PAGE_SIZE]; };
struct bio_vec { struct page *bv_page; unsigned int bv_len, bv_offset; };
struct bio {
	struct bio_vec *bi_io_vec;
	unsigned int bi_max_vecs, count;
	void *bi_bdev, *bi_private;
	void (*bi_end_io)(struct bio *bio);
	int bi_error;
	struct { unsigned long bi_sector; } bi_iter;
	bool active;
};
struct zram_table_entry {
	union { unsigned long handle, element; };
	unsigned long flags;
	s64 ac_time;
};
struct zram_stats {
	atomic64_t bd_writes, bd_count, pages_stored, huge_pages, same_pages;
	atomic64_t compr_data_size, lowratio_pages, bd_reads;
};
struct zram {
	struct zram_table_entry table[SLOTS];
	unsigned long bitmap[BLOCKS / WORD_BITS];
	unsigned long nr_pages;
	bool wb_limit_enable;
	u64 bd_wb_limit;
	int wb_limit_lock, wb_mutex, init_lock;
	u64 disksize, wb_instance, wb_generation;
	struct { int kobj; } *disk;
	u64 demand_bytes, demand_sequence, demand_instance, demand_generation;
	unsigned int policy_low_mib, policy_high_mib, policy_age_seconds;
	bool policy_enabled, demand_active, demand_initialized, demand_backing;
	void *bdev, *mem_pool, *backing_dev;
	struct zram_stats stats;
};

#define disk_to_dev(d) (d)
static unsigned int notifications;
static void sysfs_notify(void *kobj, void *group, const char *name)
{
	(void)kobj; (void)group; assert(!strcmp(name, "pool_demand"));
	notifications++;
}

static unsigned int signal_at, signal_calls, add_fail_at, submit_count;
static unsigned int live_bios, peak_live_bios, live_pages, live_handles;
static int io_error;
static bool interrupt_bio_build;
static void (*during_io)(void);
static void (*during_bio_build)(void);
static void (*before_end_io)(unsigned int serial);
static void (*during_read)(unsigned int index);
static void (*during_resched)(void);
static struct zram *active_zram;
static bool handles[SLOTS + 1];
static unsigned int submitted_sizes[128];
static unsigned long submitted_blocks[SLOTS];
static struct {
	struct bio *bio;
	int error;
} pending_bios[128];
static unsigned int completion_order[128], nr_completed;
static unsigned int pending_count, peak_pending;
static bool defer_bios, defer_worker, newest_first;
static int completion_errors[128];
static bool zram_wb_reap_one(void);
#define current NULL
static void spin_lock(int *lock) { assert(!*lock); *lock = 1; }
static void spin_unlock(int *lock) { assert(*lock); *lock = 0; }
#define DEFINE_SPINLOCK(name) int name
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; spin_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); spin_unlock(lock); } while (0)
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, new_value) ((value) = (new_value))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(name) struct list_head name = { &(name), &(name) }
#define list_first_entry(head, type, member) container_of((head)->next, type, member)
static bool list_empty(const struct list_head *head) { return head->next == head; }
static void list_add_tail(struct list_head *node, struct list_head *head)
{
	node->prev = head->prev;
	node->next = head;
	head->prev->next = node;
	head->prev = node;
}
static void list_del(struct list_head *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
	node->prev = node->next = NULL;
}

typedef int wait_queue_head_t;
#define DECLARE_WAIT_QUEUE_HEAD(name) wait_queue_head_t name
#define init_waitqueue_head(queue) (*(queue) = 0)
static void wake_up(wait_queue_head_t *queue) { (void)queue; }
#define wake_up_all(queue) wake_up(queue)
#define wake_up_interruptible(queue) wake_up(queue)
static bool host_progress(void);
static bool signal_pending(void *task);
#define wait_event(queue, condition) do { \
	(void)(&(queue)); \
	while (!(condition)) \
		assert(host_progress()); \
} while (0)
#define wait_event_interruptible(queue, condition) ({ \
	int result = 0; \
	(void)(&(queue)); \
	while (!(condition)) { \
		if (signal_pending(current)) { result = -EINTR; break; } \
		assert(host_progress()); \
	} \
	result; \
})
static bool stop_worker;
static bool kthread_should_stop(void) { return stop_worker; }
#define ERR_PTR(err) ((void *)(intptr_t)(err))
#define PTR_ERR(ptr) ((long)(intptr_t)(ptr))
#define IS_ERR(ptr) ((uintptr_t)(ptr) >= (uintptr_t)-4095)
static bool signal_pending(void *task)
{
	(void)task;
	return signal_at && ++signal_calls >= signal_at;
}
static void bit_spin_lock(unsigned int bit, unsigned long *flags)
{
	assert(!(*flags & BIT(bit)));
	*flags |= BIT(bit);
}
static void bit_spin_unlock(unsigned int bit, unsigned long *flags)
{
	assert(*flags & BIT(bit));
	*flags &= ~BIT(bit);
}
static void atomic64_inc(atomic64_t *v) { ++*v; }
static void atomic64_dec(atomic64_t *v) { assert(*v > 0); --*v; }
static void atomic64_add(unsigned long n, atomic64_t *v) { *v += n; }
static void atomic64_sub(unsigned long n, atomic64_t *v)
{
	assert(*v >= (long long)n);
	*v -= n;
}
static unsigned long find_next_zero_bit(unsigned long *map, unsigned long n,
				       unsigned long start)
{
	while (start < n && (map[start / WORD_BITS] & BIT(start % WORD_BITS)))
		start++;
	return start;
}
static int test_and_set_bit(unsigned long bit, unsigned long *map)
{
	assert(bit && bit < BLOCKS);
	int old = !!(map[bit / WORD_BITS] & BIT(bit % WORD_BITS));
	map[bit / WORD_BITS] |= BIT(bit % WORD_BITS);
	return old;
}
static int test_and_clear_bit(unsigned long bit, unsigned long *map)
{
	assert(bit && bit < BLOCKS);
	int old = !!(map[bit / WORD_BITS] & BIT(bit % WORD_BITS));
	map[bit / WORD_BITS] &= ~BIT(bit % WORD_BITS);
	return old;
}
static void zs_free(void *pool, unsigned long handle)
{
	(void)pool;
	assert(handle && handle <= SLOTS && handles[handle]);
	handles[handle] = false;
	assert(live_handles);
	live_handles--;
}
static void bio_init(struct bio *bio)
{
	memset(bio, 0, sizeof(*bio));
	bio->active = true;
	if (++live_bios > peak_live_bios)
		peak_live_bios = live_bios;
}
static void bio_set_op_attrs(struct bio *bio, int op, int flags)
{
	assert(bio->active && op == REQ_OP_WRITE && flags == REQ_SYNC);
}
static unsigned int bio_add_page(struct bio *bio, struct page *page,
				 unsigned int len, unsigned int offset)
{
	assert(bio->active && page && len == PAGE_SIZE && !offset);
	if (add_fail_at && bio->count + 1 == add_fail_at)
		return 0;
	assert(bio->count < bio->bi_max_vecs);
	bio->bi_io_vec[bio->count++] = (struct bio_vec){ page, len, offset };
	if (interrupt_bio_build)
		signal_at = 1;
	if (during_bio_build)
		during_bio_build();
	return len;
}
static void record_bio(struct bio *bio)
{
	assert(bio->active && bio->count && active_zram);
	assert(submit_count < sizeof(submitted_sizes) / sizeof(submitted_sizes[0]));
	submitted_sizes[submit_count++] = bio->count;
	unsigned long first = bio->bi_iter.bi_sector / SECTORS_PER_PAGE;
	assert(first && first + bio->count <= BLOCKS);
	for (unsigned int i = 0; i < bio->count; i++) {
		unsigned long block = first + i;
		assert(active_zram->bitmap[block / WORD_BITS] & BIT(block % WORD_BITS));
		/* Read shim fills each snapshot with its logical index. Verify that
		 * the real store did not join discontiguous backing allocations. */
		unsigned int index = bio->bi_io_vec[i].bv_page->data[0];
		assert(index < SLOTS);
		submitted_blocks[index] = block;
	}
}

static void deliver_bio(unsigned int serial)
{
	struct bio *bio = pending_bios[serial].bio;

	assert(bio && bio->active && bio->bi_end_io && pending_count);
	pending_bios[serial].bio = NULL;
	pending_count--;
	completion_order[nr_completed++] = serial;
	if (before_end_io)
		before_end_io(serial);
	bio->bi_error = pending_bios[serial].error;
	bio->bi_end_io(bio);
}

static void submit_bio(struct bio *bio)
{
	unsigned int serial = submit_count;

	record_bio(bio);
	if (during_io)
		during_io();
	pending_bios[serial].bio = bio;
	pending_bios[serial].error = completion_errors[serial] ?: io_error;
	if (++pending_count > peak_pending)
		peak_pending = pending_count;
	if (!defer_bios) {
		deliver_bio(serial);
		if (!defer_worker)
			assert(zram_wb_reap_one());
	}
}

static bool deliver_pending_bio(void)
{
	for (unsigned int i = 0; i < submit_count; i++) {
		unsigned int serial = newest_first ? submit_count - 1 - i : i;

		if (pending_bios[serial].bio) {
			deliver_bio(serial);
			return true;
		}
	}
	return false;
}

/* Cooperative dispatch: run the real worker's nonblocking iteration at a
 * modeled wait, and deliver BIO callbacks separately from slot installation. */
static bool host_progress(void)
{
	return zram_wb_reap_one() || deliver_pending_bio();
}

static void bio_reset(struct bio *bio)
{
	assert(bio->active && live_bios);
	bio->active = false;
	live_bios--;
}

/* Minimal host contracts around the extracted store, not replacement logic. */
#define GFP_KERNEL 0
#define GFP_NOIO 1
struct device_attribute { int unused; };
struct gendisk { void *private_data; };
struct device { struct gendisk disk; };
struct block_device { struct gendisk *bd_disk; };
/* Non-ioctl members are type-correct stubs; both tables are extracted verbatim. */
struct block_device_operations {
	int (*open)(struct block_device *, fmode_t);
	int (*ioctl)(struct block_device *, fmode_t, unsigned int, unsigned long);
	int (*compat_ioctl)(struct block_device *, fmode_t, unsigned int, unsigned long);
	void (*swap_slot_free_notify)(struct block_device *, unsigned long);
	int (*rw_page)(struct block_device *, sector_t, struct page *, bool);
	void *owner;
};
#define THIS_MODULE ((void *)0)
static int zram_open(struct block_device *b, fmode_t m) { return 0; }
static void zram_slot_free_notify(struct block_device *b, unsigned long i) { }
static int zram_rw_page(struct block_device *b, sector_t s, struct page *p, bool w)
{
	return 0;
}
static bool admin = true, copyin_fault;
static size_t copyout_bytes = SIZE_MAX;
static unsigned int copyouts;
static void *compat_pointer;
static bool capable(int cap) { assert(cap == CAP_SYS_ADMIN); return admin; }
static void *compat_ptr(unsigned long arg)
{
	assert(arg == 0x12345678);
	return compat_pointer;
}
static size_t copy_from_user(void *to, const void *from, size_t n)
{
	if (copyin_fault)
		return n;
	memcpy(to, from, n);
	return 0;
}
static size_t copy_to_user(void *to, const void *from, size_t n)
{
	size_t copied = copyout_bytes < n ? copyout_bytes : n;
	copyouts++;
	memcpy(to, from, copied);
	return n - copied;
}
static void *memchr_inv(const void *p, int c, size_t n)
{
	const unsigned char *b = p;
	for (size_t i = 0; i < n; i++)
		if (b[i] != (unsigned char)c)
			return (void *)(b + i);
	return NULL;
}
static u64 ktime_get_ns(void) { static u64 now = 100; return ++now; }
static u64 random_script[4], random_count, random_pos;
static u64 get_random_u64(void)
{
	if (random_pos < random_count)
		return random_script[random_pos++];
	static u64 cookie = 100;
	return ++cookie;
}
static u64 atomic64_read(atomic64_t *v) { return *v; }
static unsigned long sampled_pool_pages = 7;
static unsigned long zs_get_total_pages(void *pool) { (void)pool; return sampled_pool_pages; }
#define dev_to_disk(dev) (&(dev)->disk)
static unsigned int alloc_fail_at, alloc_calls, live_allocs;
static int read_fail_index = -1;
static bool mutex_interrupted;
static void down_read(int *lock) { spin_lock(lock); }
static void up_read(int *lock) { spin_unlock(lock); }
static int mutex_lock_interruptible(int *lock)
{
	if (mutex_interrupted) return -EINTR;
	spin_lock(lock);
	return 0;
}
static void mutex_unlock(int *lock) { spin_unlock(lock); }
static void cond_resched(void)
{
	if (during_resched)
		during_resched();
}
static bool sysfs_streq(const char *buf, const char *value)
{
	size_t len = strlen(buf);
	if (len && buf[len - 1] == '\n') len--;
	return strlen(value) == len && !memcmp(buf, value, len);
}
static int kstrtoul(const char *s, unsigned int base, unsigned long *out)
{
	if (!s || !*s || *s == '-' || *s == ' ' || *s == '\t') return -EINVAL;
	char *end;
	errno = 0;
	unsigned long value = strtoul(s, &end, base);
	if (errno || end == s || (*end && strcmp(end, "\n"))) return -EINVAL;
	*out = value;
	return 0;
}
static int kstrtouint(const char *s, unsigned int base, unsigned int *out)
{
	unsigned long value;
	int ret = kstrtoul(s, base, &value);
	if (ret || value > UINT_MAX) return -EINVAL;
	*out = value;
	return 0;
}
static void *host_alloc(size_t size)
{
	if (alloc_fail_at && ++alloc_calls == alloc_fail_at) return NULL;
	void *p = calloc(1, size);
	assert(p);
	live_allocs++;
	return p;
}
static void *kzalloc(size_t size, int flags)
{
	(void)flags;
	return host_alloc(size);
}
static void kfree(void *ptr)
{
	if (ptr) { assert(live_allocs); live_allocs--; free(ptr); }
}
static char *kstrndup(const char *s, size_t n, int flags)
{
	(void)flags;
	char *p = host_alloc(n + 1);
	if (p) memcpy(p, s, n);
	return p;
}
static struct page *alloc_page(int flags)
{
	(void)flags;
	struct page *page = host_alloc(sizeof(*page));
	if (page) live_pages++;
	return page;
}
static void __free_page(struct page *page)
{
	assert(page && live_pages);
	live_pages--;
	kfree(page);
}
static int zram_read_from_zspool(struct zram *zram, struct page *page, u32 index);
#include "actual_helpers.h"
static int zram_read_from_zspool(struct zram *zram, struct page *page, u32 index)
{
	assert(index < SLOTS && page && zram_test_flag(zram, index, ZRAM_LOCK));
	assert(zram_test_flag(zram, index, ZRAM_UNDER_WB));
	assert(zram_test_flag(zram, index, ZRAM_IDLE));
	if (during_read)
		during_read(index);
	if ((int)index == read_fail_index) return -EIO;
	memset(page->data, index, sizeof(page->data));
	return 0;
}

static void reset(struct zram *zram)
{
	assert(!live_bios && !live_pages && !live_handles && !live_allocs);
	assert(!pending_count && nr_completed == submit_count);
	memset(zram, 0, sizeof(*zram));
	memset(handles, 0, sizeof(handles));
	memset(submitted_blocks, 0, sizeof(submitted_blocks));
	zram->nr_pages = BLOCKS;
	signal_at = signal_calls = add_fail_at = submit_count = 0;
	nr_completed = peak_pending = peak_live_bios = 0;
	defer_bios = defer_worker = newest_first = interrupt_bio_build = false;
	memset(completion_errors, 0, sizeof(completion_errors));
	io_error = 0;
	alloc_fail_at = alloc_calls = 0;
	read_fail_index = -1;
	mutex_interrupted = false;
	during_io = NULL;
	during_bio_build = NULL;
	before_end_io = NULL;
	during_read = NULL;
	during_resched = NULL;
	active_zram = zram;
	admin = true;
	copyin_fault = false;
	copyout_bytes = SIZE_MAX;
	copyouts = 0;
	random_count = random_pos = 0;
	zram_wb_generation_advance(zram);
}
static void object(struct zram *zram, unsigned int index)
{
	assert(index < SLOTS && !handles[index + 1]);
	handles[index + 1] = true;
	live_handles++;
	zram->table[index].handle = index + 1;
	zram->table[index].flags = 100 | BIT(ZRAM_COMPRESS_LOW) |
		(3UL << ZRAM_IDLE_COUNT_SHIFT);
	zram->stats.pages_stored++;
	zram->stats.compr_data_size += 100;
	zram->stats.lowratio_pages++;
}
static void finish(struct zram *zram)
{
	for (unsigned int i = 0; i < SLOTS; i++) {
		assert(!(zram->table[i].flags & (BIT(ZRAM_UNDER_WB) | BIT(ZRAM_IDLE) |
					       BIT(ZRAM_LOCK))));
		if (zram_allocated(zram, i)) {
			zram_slot_lock(zram, i);
			zram_free_page(zram, i);
			zram_slot_unlock(zram, i);
		}
	}
	for (unsigned int i = 0; i < BLOCKS / WORD_BITS; i++)
		assert(!zram->bitmap[i]);
	if (zram->stats.bd_count || zram->stats.pages_stored)
		fprintf(stderr, "remaining bd_count=%lld pages_stored=%lld\n",
			zram->stats.bd_count, zram->stats.pages_stored);
	assert(!zram->stats.bd_count && !zram->stats.pages_stored);
	assert(!zram->stats.compr_data_size && !zram->stats.lowratio_pages);
	assert(!zram->wb_limit_lock && !zram->wb_mutex && !zram->init_lock);
	assert(!live_pages && !live_handles && !live_bios && !live_allocs);
	assert(!pending_count && nr_completed == submit_count);
	assert(!zram_wb_reap_one());
}
static void test_budget(void)
{
	struct zram zram;
	const u64 unit = 1ULL << (PAGE_SHIFT - 12);
	const u64 budgets[] = { 0, unit - 1, unit, 31 * unit, 32 * unit + unit - 1 };
	for (unsigned int i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
		reset(&zram);
		zram.wb_limit_enable = true;
		zram.bd_wb_limit = budgets[i];
		unsigned int got = zram_wb_reserve(&zram, 32);
		assert(got == min_t(u64, 32, budgets[i] / unit));
		assert(zram.bd_wb_limit == budgets[i] - got * unit);
		zram_wb_refund(&zram, got);
		assert(zram.bd_wb_limit == budgets[i]);
		finish(&zram);
	}
	reset(&zram);
	assert(zram_wb_reserve(&zram, 32) == 32);
	zram_wb_refund(&zram, 32);
	assert(!zram.bd_wb_limit);
	finish(&zram);
}
static void invalidate(void)
{
	struct zram *zram = active_zram;
	/* Simulated scheduling points, not actual concurrent reads/writes. */
	zram_slot_lock(zram, 0);
	zram_free_page(zram, 0); /* free, preserves UNDER_WB */
	zram_slot_unlock(zram, 0);
	zram_slot_lock(zram, 1);
	zram_free_page(zram, 1);
	object(zram, 1); /* replacement reuses the same handle: ABA */
	zram->table[1].flags |= BIT(ZRAM_UNDER_WB) | BIT(ZRAM_LOCK);
	zram_slot_unlock(zram, 1);
	zram_slot_lock(zram, 2);
	zram_clear_flag(zram, 2, ZRAM_IDLE); /* simulated read */
	zram_slot_unlock(zram, 2);
	zram_slot_lock(zram, 3);
	zram_clear_flag(zram, 3, ZRAM_UNDER_WB);
	zram_slot_unlock(zram, 3);
}
static void candidates(struct zram *zram, unsigned int nr, bool sparse)
{
	zram->disksize = (u64)(sparse ? nr * 2 : nr) << PAGE_SHIFT;
	zram->backing_dev = zram;
	for (unsigned int i = 0; i < nr; i++) {
		unsigned int index = sparse ? i * 2 : i;
		object(zram, index);
		zram->table[index].flags |= BIT(ZRAM_IDLE);
	}
}
static ssize_t store(struct zram *zram, const char *command)
{
	struct device dev = { .disk.private_data = zram };
	return writeback_store(&dev, NULL, command, strlen(command));
}
static void clear_idle(struct zram *zram)
{
	for (unsigned int i = 0; i < SLOTS; i++)
		zram->table[i].flags &= ~BIT(ZRAM_IDLE);
}
static struct zram_wb_exec run_idle(struct zram *zram, u64 attempts, u64 installed)
{
	struct zram_wb_exec req = {
		.mode = IDLE_WRITEBACK,
		.max_attempt_pages = attempts,
		.max_installed_pages = installed,
	};
	int ret;

	down_read(&zram->init_lock);
	ret = zram_writeback_engine(zram, &req);
	up_read(&zram->init_lock);
	assert(ret == req.op_errno);
	return req;
}

static void test_async_window(void)
{
	/* Cover inline end_io, deferred worker with inline end_io, and
	 * deferred/out-of-order end_io with a worker step between callbacks. */
	for (unsigned int scenario = 0; scenario < 3; scenario++) {
		struct zram zram;
		struct zram_wb_exec req;
		const unsigned int pages = 5 * 32 + 1;

		reset(&zram);
		candidates(&zram, pages, false);
		defer_worker = scenario == 1;
		defer_bios = newest_first = scenario == 2;
		req = run_idle(&zram, pages, pages);
		assert(!req.op_errno && req.stop_reason == ZRAM_WB_STOP_INSTALLED_LIMIT);
		assert(req.selected_pages == pages && req.submitted_pages == pages);
		assert(req.installed_pages == pages && !req.invalidated_pages);
		assert(submit_count == 6 && nr_completed == 6);
		assert(peak_live_bios == (scenario ? 4 : 1));
		assert(peak_pending == (defer_bios ? 4 : 1));
		assert(completion_order[0] == (defer_bios ? 3 : 0));
		assert(completion_order[5] == (defer_bios ? 0 : 5));
		for (unsigned int i = 0; i < pages; i++)
			assert(submitted_blocks[i] == zram.table[i].element);
		finish(&zram);
	}
}

static void test_async_failure(void)
{
	struct zram zram;
	struct zram_wb_exec req;
	const unsigned int pages = 6 * 32;
	const u64 unit = 1ULL << (PAGE_SHIFT - 12);

	reset(&zram);
	candidates(&zram, pages, false);
	zram.wb_limit_enable = true;
	zram.bd_wb_limit = pages * unit;
	defer_bios = newest_first = true;
	completion_errors[3] = -ENOSPC; /* first observed */
	completion_errors[1] = -EIO;
	req = run_idle(&zram, pages, pages);
	assert(req.op_errno == -ENOSPC && req.stop_reason == ZRAM_WB_STOP_ERROR);
	assert(req.error_stage == ZRAM_WB_STAGE_IO && !req.invalidated_pages);
	assert(submit_count == 4 && nr_completed == 4 && peak_pending == 4);
	for (unsigned int i = 0; i < 4; i++)
		assert(completion_order[i] == 3 - i);
	assert(req.submitted_pages == 128 && req.installed_pages == 64);
	assert(zram.stats.bd_writes == 128 && zram.stats.bd_count == 64);
	assert(zram.bd_wb_limit == (pages - 128) * unit);
	for (unsigned int i = 0; i < pages; i++)
		assert(zram_test_flag(&zram, i, ZRAM_WB) == (i < 128 && (i / 32) % 2 == 0));
	clear_idle(&zram);
	finish(&zram);
}

static void fail_during_next_build(void)
{
	if (submit_count == 1 && pending_count == 1)
		deliver_bio(0);
}

static void test_async_uncommitted(void)
{
	struct zram zram;
	struct zram_wb_exec req;
	const u64 unit = 1ULL << (PAGE_SHIFT - 12);

	reset(&zram);
	candidates(&zram, 65, false);
	zram.wb_limit_enable = true;
	zram.bd_wb_limit = 65 * unit;
	defer_bios = true;
	completion_errors[0] = -EIO;
	during_bio_build = fail_during_next_build;
	req = run_idle(&zram, 65, 65);
	assert(req.op_errno == -EIO && req.stop_reason == ZRAM_WB_STOP_ERROR);
	assert(req.error_stage == ZRAM_WB_STAGE_IO && !req.invalidated_pages);
	/* The second BIO already reserved quota, but hadn't committed. */
	assert(req.selected_pages == 64 && req.submitted_pages == 32);
	assert(!req.installed_pages && zram.stats.bd_writes == 32);
	assert(submit_count == 1 && nr_completed == 1 && peak_live_bios == 2);
	assert(zram.bd_wb_limit == 33 * unit);
	clear_idle(&zram);
	finish(&zram);
}

static void invalidate_first_bio(unsigned int serial)
{
	if (!serial)
		invalidate();
}

static void test_cancel(void)
{
	struct zram zram;
	struct zram_wb_exec req;
	const u64 unit = 1ULL << (PAGE_SHIFT - 12);

	reset(&zram);
	candidates(&zram, 65, false);
	zram.wb_limit_enable = true;
	zram.bd_wb_limit = 33 * unit;
	defer_bios = newest_first = true;
	before_end_io = invalidate_first_bio;
	req = run_idle(&zram, 65, 65);
	assert(req.stop_reason == ZRAM_WB_STOP_GLOBAL_LIMIT && req.op_errno == -EIO);
	assert(req.submitted_pages == 33 && req.installed_pages == 29);
	assert(req.invalidated_pages == 4 && !zram.bd_wb_limit);
	assert(submit_count == 2 && completion_order[0] == 1 && completion_order[1] == 0);
	assert(!zram_allocated(&zram, 0));
	for (unsigned int i = 1; i <= 3; i++) {
		assert(!zram_test_flag(&zram, i, ZRAM_WB));
		assert(zram_test_flag(&zram, i, ZRAM_COMPRESS_LOW));
		assert(zram_idle_count(&zram, i) == 3 && handles[i + 1]);
	}
	for (unsigned int i = 4; i < 65; i++)
		assert(zram_test_flag(&zram, i, ZRAM_WB) == (i < 33));
	clear_idle(&zram);
	finish(&zram);
}

static void test_store_sizes(void)
{
	const unsigned int sizes[] = {1, 31, 32, 33, 65};
	for (unsigned int sparse = 0; sparse <= 1; sparse++) {
		for (unsigned int n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++) {
			struct zram zram;
			unsigned int nr = sizes[n];
			reset(&zram);
			candidates(&zram, nr, sparse);
			assert(store(&zram, "idle") == 4);
			assert(zram.stats.bd_writes == nr && zram.stats.bd_count == nr);
			for (unsigned int i = 0; i < nr; i++) {
				unsigned int index = sparse ? i * 2 : i;
				assert(submitted_blocks[index] == zram.table[index].element);
			}
			assert(submit_count == (nr + 31) / 32);
			for (unsigned int i = 0; i < submit_count; i++)
				assert(submitted_sizes[i] == (i + 1 < submit_count ? 32 :
							 ((nr - 1) % 32) + 1));
			finish(&zram);
		}
	}
	/* Occupied backing blocks force actual store to split its BIOs. */
	struct zram zram;
	reset(&zram);
	candidates(&zram, 33, true);
	for (unsigned int i = 2; i < 100; i += 2)
		assert(!test_and_set_bit(i, zram.bitmap));
	assert(store(&zram, "idle") == 4);
	assert(submit_count == 33 && zram.stats.bd_count == 33);
	for (unsigned int i = 0; i < 33; i++)
		assert(submitted_blocks[i * 2] == zram.table[i * 2].element);
	for (unsigned int i = 0; i < submit_count; i++) assert(submitted_sizes[i] == 1);
	for (unsigned int i = 2; i < 100; i += 2)
		assert(test_and_clear_bit(i, zram.bitmap));
	finish(&zram);
}
static void test_store_requestmax(void)
{
	const unsigned int limits[] = {0, 1, 31, 32, 33, 65, 100};
	for (unsigned int n = 0; n < sizeof(limits) / sizeof(limits[0]); n++) {
		struct zram zram;
		char command[40];
		reset(&zram);
		candidates(&zram, 65, false);
		snprintf(command, sizeof(command), "idle %u 1", limits[n]);
		ssize_t len = strlen(command);
		assert(store(&zram, command) == len);
		unsigned int expected = limits[n] < 65 ? limits[n] : 65;
		assert(zram.stats.bd_count == expected && zram.stats.bd_writes == expected);
		clear_idle(&zram); /* unselected objects legitimately remain IDLE */
		finish(&zram);
	}
}
static void test_store_modes(void)
{
	struct zram zram;

	reset(&zram);
	candidates(&zram, 3, false);
	for (unsigned int i = 1; i < 3; i++) {
		zram_set_flag(&zram, i, ZRAM_HUGE);
		atomic64_inc(&zram.stats.huge_pages);
	}
	/* HUGE ignores both the current idle mark and compression filter. */
	zram_clear_flag(&zram, 1, ZRAM_IDLE);
	zram_clear_flag(&zram, 1, ZRAM_COMPRESS_LOW);
	atomic64_dec(&zram.stats.lowratio_pages);
	assert(store(&zram, "huge") == 4);
	assert(zram.stats.bd_writes == 2 && zram.stats.bd_count == 2);
	assert(!zram_test_flag(&zram, 0, ZRAM_WB));
	assert(zram_test_flag(&zram, 1, ZRAM_WB));
	assert(zram_test_flag(&zram, 2, ZRAM_WB));
	clear_idle(&zram);
	finish(&zram);

	reset(&zram);
	candidates(&zram, 3, false);
	zram_clear_flag(&zram, 2, ZRAM_IDLE);
	assert(store(&zram, "page_index=2") == 12);
	assert(zram.stats.bd_writes == 1 && zram.stats.bd_count == 1);
	assert(zram_test_flag(&zram, 2, ZRAM_WB));
	assert(!zram_test_flag(&zram, 0, ZRAM_WB));
	assert(!zram_test_flag(&zram, 1, ZRAM_WB));
	assert(store(&zram, "page_index=3") == -EINVAL);
	assert(submit_count == 1 && zram.stats.bd_writes == 1);
	clear_idle(&zram);
	finish(&zram);

	/* Count zero removes only the threshold; 11 is clamped to 10 in sysfs. */
	const char *commands[] = {"idle 5 0", "idle 5 10", "idle 5 11", "idle"};
	const unsigned int counts[] = {0, 9, 10, 10, 10};

	for (unsigned int n = 0; n < 4; n++) {
		reset(&zram);
		candidates(&zram, 5, false);
		for (unsigned int i = 0; i < 5; i++) {
			zram_clear_idle_count(&zram, i);
			zram.table[i].flags |= (unsigned long)counts[i] <<
				ZRAM_IDLE_COUNT_SHIFT;
		}
		zram_clear_flag(&zram, 3, ZRAM_IDLE);
		zram_clear_flag(&zram, 4, ZRAM_COMPRESS_LOW);
		atomic64_dec(&zram.stats.lowratio_pages);
		assert(store(&zram, commands[n]) == (ssize_t)strlen(commands[n]));
		for (unsigned int i = 0; i < 5; i++) {
			bool expected = i == 2 || ((n == 0 || n == 3) && i < 2) ||
				(n == 3 && i == 4);

			assert(zram_test_flag(&zram, i, ZRAM_WB) == expected);
		}
		assert(zram.stats.bd_writes == (n == 0 ? 3 : n == 3 ? 4 : 1));
		assert(zram.stats.bd_count == zram.stats.bd_writes);
		clear_idle(&zram);
		finish(&zram);
	}
}
static void test_store_exclusions(void)
{
	/* A positive candidate must be excluded independently by each guard. */
	const enum zram_pageflags excludes[] = {ZRAM_SAME, ZRAM_WB, ZRAM_UNDER_WB};
	const char *commands[] = {"idle", "huge", "idle 1 0", "page_index=0"};

	for (unsigned int c = 0; c < 4; c++) {
		for (unsigned int f = 0; f < 3; f++) {
			struct zram zram;

			reset(&zram);
			candidates(&zram, 1, false);
			zram_set_flag(&zram, 0, ZRAM_HUGE);
			atomic64_inc(&zram.stats.huge_pages);
			/* Inject the selection guard only; restore it before freeing. */
			zram_set_flag(&zram, 0, excludes[f]);
			unsigned long before = zram.table[0].flags;

			assert(store(&zram, commands[c]) == (ssize_t)strlen(commands[c]));
			assert(!submit_count && !zram.stats.bd_writes);
			assert(zram.table[0].flags == before && handles[1]);
			zram_clear_flag(&zram, 0, excludes[f]);
			clear_idle(&zram);
			finish(&zram);
		}
	}
}
static void interrupt_during_io(void)
{
	/* Delivery at the submit scheduling point, not a real host signal. */
	signal_at = 1;
}
static void test_secondary_alloc_failure(void)
{
	/* #34 is the second batch, #35 its first page. In either case, wait
	 * for a usable batch rather than aborting or reusing an empty batch. */
	for (unsigned int fail = 34; fail <= 35; fail++) {
		struct zram zram;
		struct zram_wb_exec req;
		const unsigned int pages = 4 * 32 + 1;

		reset(&zram);
		candidates(&zram, pages, false);
		defer_bios = true;
		alloc_fail_at = fail;
		req = run_idle(&zram, pages, pages);
		assert(!req.op_errno && req.installed_pages == pages);
		assert(req.submitted_pages == pages && !req.invalidated_pages);
		assert(submit_count == 5 && nr_completed == 5);
		finish(&zram);
	}
}

static void test_store_faults(void)
{
	/* Allocation #1 is batch, #2 first page; later failures allow fallback.
	 * Include failed second page, mid-batch and final preallocation. */
	const unsigned int faults[] = {1, 2, 3, 18, 33};
	for (unsigned int i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
		struct zram zram;
		reset(&zram);
		candidates(&zram, 65, false);
		alloc_fail_at = faults[i];
		ssize_t ret = store(&zram, "idle");
		assert(ret == (faults[i] <= 2 ? -ENOMEM : 4));
		assert(zram.stats.bd_count == (faults[i] <= 2 ? 0 : 65));
		clear_idle(&zram);
		finish(&zram);
	}
	for (unsigned int scenario = 0; scenario < 9; scenario++) {
		struct zram zram;
		const u64 unit = 1ULL << (PAGE_SHIFT - 12);
		u64 before, submitted;

		reset(&zram);
		candidates(&zram, 65, false);
		if (scenario == 0) mutex_interrupted = true;
		if (scenario == 1) signal_at = 5; /* preallocation interrupted */
		if (scenario == 2) during_resched = interrupt_during_io; /* one snapshot */
		if (scenario == 3) read_fail_index = 1;
		if (scenario == 4) io_error = -EIO;
		if (scenario == 5) add_fail_at = 2;
		if (scenario == 8) interrupt_bio_build = true;
		zram.wb_limit_enable = true;
		before = zram.bd_wb_limit = (scenario == 6 ? 31 : scenario == 7 ? 0 : 65) * unit;
		ssize_t ret = store(&zram, "idle");
		assert(ret == (scenario <= 2 || scenario == 8 ? -EINTR : -EIO));
		submitted = scenario == 4 ? 32 : scenario == 6 ? 31 : 0;
		assert(zram.stats.bd_count == (scenario == 6 ? 31 : 0));
		assert(zram.stats.bd_writes == submitted);
		assert(zram.bd_wb_limit == before - submitted * unit);
		clear_idle(&zram);
		finish(&zram);
	}
	/* Signal stops new work; successful committed IO still installs on drain. */
	{
		struct zram zram;
		reset(&zram);
		candidates(&zram, 65, false);
		zram.wb_limit_enable = true;
		zram.bd_wb_limit = 65ULL << (PAGE_SHIFT - 12);
		defer_bios = true;
		during_io = interrupt_during_io;
		assert(store(&zram, "idle") == -EINTR);
		assert(submit_count == 1 && zram.stats.bd_writes == 32);
		assert(zram.stats.bd_count == 32 && nr_completed == 1);
		assert(zram.bd_wb_limit == (33ULL << (PAGE_SHIFT - 12)));
		clear_idle(&zram);
		finish(&zram);
	}
	/* Backing space runs out with queued snapshots: flush then ENOSPC. */
	struct zram zram;
	reset(&zram);
	candidates(&zram, 33, false);
	zram.nr_pages = 10;
	assert(store(&zram, "idle") == -ENOSPC);
	assert(zram.stats.bd_count == 9 && zram.stats.bd_writes == 9);
	clear_idle(&zram);
	finish(&zram);
}
#ifdef CONFIG_ZRAM_WRITEBACK_IOCTL
static int ioctl_route(const struct block_device_operations *ops,
		       struct zram *zram, unsigned int cmd, void *arg,
		       fmode_t mode, bool compat)
{
	struct gendisk disk = { .private_data = zram };
	struct block_device bdev = { .bd_disk = &disk };

	if (compat) {
		assert(ops->compat_ioctl);
		compat_pointer = arg;
		return ops->compat_ioctl(&bdev, mode, cmd, 0x12345678);
	}
	assert(ops->ioctl);
	return ops->ioctl(&bdev, mode, cmd, (unsigned long)arg);
}
static int ioctl_call(struct zram *zram, unsigned int cmd, void *arg, fmode_t mode,
		      bool compat)
{
	return ioctl_route(&zram_devops, zram, cmd, arg, mode, compat);
}
static struct zram_wb_exec request(struct zram *zram, u64 attempts, u64 installed)
{
	return (struct zram_wb_exec) {
		.size = sizeof(struct zram_wb_exec), .version = ZRAM_WB_ABI_VERSION,
		.mode = ZRAM_WB_MODE_FILTERED_IDLE, .request_id = 1,
		.expected_instance = zram->wb_instance, .expected_generation = zram->wb_generation,
		.max_attempt_pages = attempts, .max_installed_pages = installed, .idle_count = 1,
	};
}
static void exec_ok(struct zram *zram, struct zram_wb_exec *req)
{
	assert(!ioctl_call(zram, ZRAM_IOC_WB_EXEC, req, FMODE_READ | FMODE_WRITE, false));
	assert(req->duration_ns && req->stop_reason && req->op_errno <= 0);
	assert(req->selected_pages <= req->scanned_pages);
	assert(req->submitted_pages <= req->selected_pages);
	assert(req->installed_pages <= req->submitted_pages);
	assert(req->installed_pages <= req->max_installed_pages);
	assert(req->submitted_pages <= req->max_attempt_pages);
	assert(req->installed_pages + req->invalidated_pages <= req->selected_pages);
}
static void invalidate_once(void);
static void test_exec_counts_caps(void)
{
	const unsigned int limits[] = {0, 1, 31, 32, 33, 65, 100};
	for (unsigned int i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
		struct zram zram;
		reset(&zram);
		candidates(&zram, 65, true);
		struct zram_wb_exec req = request(&zram, limits[i], limits[i]);
		exec_ok(&zram, &req);
		unsigned int n = limits[i] < 65 ? limits[i] : 65;
		assert(req.installed_pages == n && req.submitted_pages == n);
		assert(req.selected_pages == n && !req.invalidated_pages && !req.op_errno);
		assert(req.scanned_pages == (!n ? 0 : limits[i] > 65 ? 130 : 2 * n - 1));
		assert(req.stop_reason == (!limits[i] ? ZRAM_WB_STOP_NO_WORK :
			limits[i] > 65 ? ZRAM_WB_STOP_COMPLETE : ZRAM_WB_STOP_INSTALLED_LIMIT));
		clear_idle(&zram);
		finish(&zram);
	}
	/* Independent attempt limit: first batch loses four identities. */
	for (unsigned int enough_attempts = 0; enough_attempts < 2; enough_attempts++) {
		struct zram zram;
		reset(&zram);
		candidates(&zram, 65, false);
		during_io = invalidate_once;
		struct zram_wb_exec req = request(&zram, enough_attempts ? 36 : 33, 32);
		exec_ok(&zram, &req);
		assert(req.submitted_pages == (enough_attempts ? 36 : 33));
		assert(req.installed_pages == (enough_attempts ? 32 : 29));
		assert(req.invalidated_pages == 4 && !req.op_errno);
		assert(req.stop_reason == (enough_attempts ? ZRAM_WB_STOP_INSTALLED_LIMIT :
			ZRAM_WB_STOP_ATTEMPT_LIMIT));
		clear_idle(&zram);
		finish(&zram);
	}
	/* Per-call zero never disables/refills the global quota. */
	struct zram zram;
	reset(&zram);
	candidates(&zram, 1, false);
	zram.wb_limit_enable = true;
	zram.bd_wb_limit = 123;
	struct zram_wb_exec req = request(&zram, 9, 0);
	exec_ok(&zram, &req);
	assert(req.stop_reason == ZRAM_WB_STOP_NO_WORK && zram.bd_wb_limit == 123);
	assert(!req.scanned_pages && !req.selected_pages && !submit_count);
	clear_idle(&zram);
	finish(&zram);
}
static void invalidate_once(void)
{
	invalidate();
	during_io = NULL;
}
static void invalidate_one_once(void)
{
	zram_slot_lock(active_zram, 0);
	assert(zram_test_flag(active_zram, 0, ZRAM_UNDER_WB));
	zram_clear_flag(active_zram, 0, ZRAM_IDLE);
	zram_slot_unlock(active_zram, 0);
	during_io = NULL;
}
static void test_exec_caps_split(void)
{
	for (unsigned int gaps = 0; gaps < 2; gaps++) {
		struct zram zram;
		struct zram_wb_exec req;

		reset(&zram);
		candidates(&zram, 65, false);
		if (gaps) {
			for (unsigned int i = 2; i < 100; i += 2)
				assert(!test_and_set_bit(i, zram.bitmap));
		} else {
			alloc_fail_at = 3; /* Batch + one page, then fallback. */
		}
		/* Disabled global limit must ignore and preserve even a tiny balance. */
		zram.bd_wb_limit = 1;
		during_io = invalidate_one_once;
		req = request(&zram, 33, 32);
		exec_ok(&zram, &req);
		assert(req.scanned_pages == 33 && req.selected_pages == 33);
		assert(req.submitted_pages == 33 && req.installed_pages == 32);
		assert(req.invalidated_pages == 1 && !req.op_errno);
		assert(req.stop_reason == ZRAM_WB_STOP_INSTALLED_LIMIT);
		assert(req.error_stage == ZRAM_WB_STAGE_NONE && submit_count == 33);
		assert(!zram.wb_limit_enable && zram.bd_wb_limit == 1);
		assert(zram.stats.bd_writes == 33 && zram.stats.bd_count == 32);
		assert(!zram_test_flag(&zram, 0, ZRAM_WB) && handles[1]);
		for (unsigned int i = 1; i <= 32; i++)
			assert(submitted_blocks[i] == zram.table[i].element);
		for (unsigned int i = 0; i < submit_count; i++)
			assert(submitted_sizes[i] == 1);
		if (gaps) {
			for (unsigned int i = 2; i < 100; i += 2)
				assert(test_and_clear_bit(i, zram.bitmap));
		} else {
			assert(alloc_calls == 3);
		}
		clear_idle(&zram);
		finish(&zram);
	}
}
static void free_previous_install(void)
{
	if (submit_count == 2) {
		zram_slot_lock(active_zram, 0);
		zram_free_page(active_zram, 0);
		zram_slot_unlock(active_zram, 0);
	}
}
static void invalidate_pending_then_read_error(unsigned int index)
{
	if (index == 1) {
		zram_slot_lock(active_zram, 0);
		zram_clear_flag(active_zram, 0, ZRAM_IDLE);
		zram_slot_unlock(active_zram, 0);
	}
}
static void invalidate_pending_then_signal(void)
{
	zram_slot_lock(active_zram, 0);
	zram_clear_flag(active_zram, 0, ZRAM_IDLE);
	zram_slot_unlock(active_zram, 0);
	signal_at = 1;
	during_resched = NULL;
}
static void test_exec_invalidations(void)
{
	struct zram zram;
	reset(&zram);
	candidates(&zram, 33, false);
	during_io = free_previous_install;
	struct zram_wb_exec req = request(&zram, 33, 33);
	exec_ok(&zram, &req);
	assert(req.installed_pages == 33 && zram.stats.bd_count == 32);
	assert(!req.invalidated_pages); /* later frees are not failed installations */
	finish(&zram);

	for (unsigned int scenario = 0; scenario < 3; scenario++) {
		reset(&zram);
		candidates(&zram, 5, false);
		req = request(&zram, 5, 5);
		if (scenario == 0) {
			during_io = invalidate_once;
			zram.wb_limit_enable = true;
			zram.bd_wb_limit = 2ULL << (PAGE_SHIFT - 12);
		} else if (scenario == 1) {
			during_read = invalidate_pending_then_read_error;
			read_fail_index = 1;
		} else {
			during_resched = invalidate_pending_then_signal;
		}
		exec_ok(&zram, &req);
		assert(req.invalidated_pages == (scenario == 0 ? 4 : 1));
		assert(req.selected_pages == (scenario == 0 ? 5 : scenario == 1 ? 2 : 1));
		assert(req.submitted_pages == (scenario == 0 ? 2 : 0));
		assert(!req.installed_pages);
		assert(req.stop_reason == (scenario == 0 ? ZRAM_WB_STOP_GLOBAL_LIMIT :
			scenario == 1 ? ZRAM_WB_STOP_ERROR : ZRAM_WB_STOP_INTERRUPTED));
		assert(req.op_errno == (scenario == 2 ? -EINTR : -EIO));
		assert(req.error_stage == (scenario == 1 ? ZRAM_WB_STAGE_SNAPSHOT_READ : 0));
		clear_idle(&zram);
		finish(&zram);
	}
}
static void io_error_and_signal(void)
{
	io_error = -EIO;
	signal_at = 1;
}
static void test_exec_faults(void)
{
	const u64 unit = 1ULL << (PAGE_SHIFT - 12);
	for (unsigned int scenario = 0; scenario < 11; scenario++) {
		struct zram zram;
		reset(&zram);
		candidates(&zram, 65, false);
		struct zram_wb_exec req = request(&zram, 33, 33);
		zram.wb_limit_enable = true;
		zram.bd_wb_limit = 65 * unit;
		u32 stage = 0, stop = ZRAM_WB_STOP_ERROR;
		int error = -EIO;
		unsigned int submitted = 0, installed = 0;
		if (scenario < 2) {
			alloc_fail_at = scenario + 1; stage = ZRAM_WB_STAGE_ALLOC; error = -ENOMEM;
		}
		if (scenario == 2) {
			read_fail_index = 33;
			stage = ZRAM_WB_STAGE_SNAPSHOT_READ;
			req.max_attempt_pages = req.max_installed_pages = 65;
			submitted = installed = 32;
		}
		if (scenario == 3) {
			add_fail_at = 2;
			stage = ZRAM_WB_STAGE_BIO_BUILD;
		}
		if (scenario == 4) {
			io_error = -EIO;
			stage = ZRAM_WB_STAGE_IO;
			submitted = 32;
		}
		if (scenario == 5) {
			during_io = io_error_and_signal;
			stage = ZRAM_WB_STAGE_IO;
			submitted = 32;
		}
		if (scenario == 6) {
			during_io = interrupt_during_io;
			stop = ZRAM_WB_STOP_INTERRUPTED;
			error = -EINTR;
			submitted = installed = 32;
		}
		if (scenario == 7) {
			mutex_interrupted = true;
			stop = ZRAM_WB_STOP_INTERRUPTED;
			error = -EINTR;
		}
		if (scenario == 8) {
			zram.bd_wb_limit = unit - 1;
			stop = ZRAM_WB_STOP_GLOBAL_LIMIT;
		}
		if (scenario == 9) {
			zram.bd_wb_limit = 31 * unit;
			stop = ZRAM_WB_STOP_GLOBAL_LIMIT;
			submitted = installed = 31;
		}
		if (scenario == 10) {
			zram.nr_pages = 10;
			stop = ZRAM_WB_STOP_BACKING_FULL;
			error = -ENOSPC;
			submitted = installed = 9;
		}
		u64 before = zram.bd_wb_limit;
		exec_ok(&zram, &req);
		assert(req.op_errno == error && req.stop_reason == stop && req.error_stage == stage);
		assert(req.submitted_pages == submitted && req.installed_pages == installed);
		assert(!req.invalidated_pages && zram.bd_wb_limit == before - submitted * unit);
		clear_idle(&zram);
		finish(&zram);
	}
}
static void fail_second_bio(void)
{
	if (submit_count == 2) {
		assert(active_zram->stats.bd_count == 32);
		io_error = -EIO;
	}
}
static void test_exec_late_io_error(void)
{
	struct zram zram;
	struct zram_wb_exec req;
	const u64 unit = 1ULL << (PAGE_SHIFT - 12);

	reset(&zram);
	candidates(&zram, 65, false);
	zram.wb_limit_enable = true;
	zram.bd_wb_limit = 65 * unit;
	during_io = fail_second_bio;
	req = request(&zram, 64, 64);
	exec_ok(&zram, &req);
	assert(req.scanned_pages == 64 && req.selected_pages == 64);
	assert(req.submitted_pages == 64 && req.installed_pages == 32);
	assert(!req.invalidated_pages && req.op_errno == -EIO);
	assert(req.stop_reason == ZRAM_WB_STOP_ERROR); /* Not ATTEMPT_LIMIT. */
	assert(req.error_stage == ZRAM_WB_STAGE_IO);
	assert(submit_count == 2 && submitted_sizes[0] == 32 && submitted_sizes[1] == 32);
	assert(zram.stats.bd_count == 32 && zram.stats.bd_writes == 64);
	assert(zram.bd_wb_limit == unit); /* Failed second BIO is not refunded. */
	for (unsigned int i = 0; i < 64; i++) {
		assert(zram_test_flag(&zram, i, ZRAM_WB) == (i < 32));
		assert(handles[i + 1] == (i >= 32));
		assert(!zram_test_flag(&zram, i, ZRAM_UNDER_WB));
		assert(!zram_test_flag(&zram, i, ZRAM_IDLE));
	}
	assert(zram_test_flag(&zram, 64, ZRAM_IDLE));
	clear_idle(&zram);
	finish(&zram);
}
static void test_exec_validation(void)
{
	struct zram zram;
	reset(&zram);
	candidates(&zram, 1, false);
	struct zram_wb_exec base = request(&zram, 1, 1), req;
	for (unsigned int scenario = 0; scenario < 12; scenario++) {
		req = base;
		if (scenario == 0)
			req.size--;
		if (scenario == 1)
			req.version++;
		if (scenario == 2)
			req.flags = 1;
		if (scenario == 3)
			req.request_id = 0;
		if (scenario == 4)
			req.reserved0 = 1;
		if (scenario == 5)
			req.mode = 4;
		if (scenario == 6)
			req.idle_count = 11;
		if (scenario == 7)
			req.mode = ZRAM_WB_MODE_IDLE; /* count must be zero */
		if (scenario == 8)
			req.page_index = 1;
		if (scenario == 9)
			req.max_attempt_pages = 0;
		if (scenario == 10) {
			req.mode = ZRAM_WB_MODE_PAGE;
			req.idle_count = 0;
			req.page_index = 1ULL << 32;
		}
		if (scenario == 11)
			req.max_installed_pages = 2;
		assert(ioctl_call(&zram, ZRAM_IOC_WB_EXEC, &req, FMODE_WRITE, false) ==
		       (scenario == 1 ? -EPROTONOSUPPORT : -EINVAL));
	}
	/* Every byte of reserved inputs and output region must be zero. */
	for (size_t i = offsetof(struct zram_wb_exec, reserved); i < sizeof(req); i++) {
		req = base;
		((unsigned char *)&req)[i] = 1;
		assert(ioctl_call(&zram, ZRAM_IOC_WB_EXEC, &req, FMODE_WRITE, false) == -EINVAL);
	}
	req = base;
	admin = false;
	assert(ioctl_call(&zram, ZRAM_IOC_WB_EXEC, &req, FMODE_WRITE, false) == -EPERM);
	admin = true;
	assert(ioctl_call(&zram, ZRAM_IOC_WB_EXEC, &req, FMODE_READ, false) == -EBADF);
	copyin_fault = true;
	assert(ioctl_call(&zram, ZRAM_IOC_WB_EXEC, &req, FMODE_WRITE, false) == -EFAULT);
	copyin_fault = false;
	assert(ioctl_call(&zram, _IO('Z', 0x20), &req, FMODE_WRITE, false) == -EINVAL);
	assert(ioctl_call(&zram, _IO('Z', 0x23), &req, FMODE_WRITE, false) == -ENOIOCTLCMD);
	assert(!copyouts && !submit_count && !live_pages && !live_allocs);
	/* Page zero is legal; nonzero page index must not truncate into zero. */
	req = base; req.mode = ZRAM_WB_MODE_PAGE; req.idle_count = 0;
	exec_ok(&zram, &req);
	assert(req.installed_pages == 1 && !req.op_errno);
	finish(&zram);
}
static void test_status_lifecycle(void)
{
	struct zram zram;
	reset(&zram);
	struct zram_wb_status status = { .size = sizeof(status), .version = ZRAM_WB_ABI_VERSION };
	assert(!ioctl_call(&zram, ZRAM_IOC_WB_STATUS, &status, FMODE_READ, true));
	assert(status.page_size == PAGE_SIZE && status.instance && status.generation == 1);
	assert(status.capabilities == 15 && !status.state_flags && !status.disksize_bytes);
	struct zram_wb_exec req = request(&zram, 0, 0);
	exec_ok(&zram, &req);
	assert(req.stop_reason == ZRAM_WB_STOP_NOT_READY && req.op_errno == -ENODEV);
	candidates(&zram, 1, false);
	req = request(&zram, 0, 0);
	zram_wb_generation_advance(&zram);
	exec_ok(&zram, &req);
	assert(req.stop_reason == ZRAM_WB_STOP_STALE && req.op_errno == -ESTALE);
	req = request(&zram, 1, 1); req.expected_instance++;
	exec_ok(&zram, &req);
	assert(req.stop_reason == ZRAM_WB_STOP_STALE && !submit_count);
	status = (struct zram_wb_status){ .size = sizeof(status), .version = 1 };
	zram.wb_limit_enable = true;
	zram.bd_wb_limit = 7;
	assert(!ioctl_call(&zram, ZRAM_IOC_WB_STATUS, &status, FMODE_READ, false));
	assert(status.state_flags == 7 && status.wb_limit_remaining_4k == 7);
	assert(status.disksize_bytes == PAGE_SIZE && status.mem_used_bytes == 7 * PAGE_SIZE);
	assert(status.pages_stored == 1 && status.compr_data_bytes == 100 && status.lowratio_pages == 1);
	assert(!status.bd_count_pages && !status.bd_reads_pages && !status.bd_writes_pages);
	assert(!memchr_inv(status.reserved_out, 0, sizeof(status.reserved_out)));
	/* STATUS rejects input outputs/reserved byte-for-byte, even on read-only FD. */
	for (size_t i = offsetof(struct zram_wb_status, flags); i < sizeof(status); i++) {
		status = (struct zram_wb_status){ .size = sizeof(status), .version = 1 };
		((unsigned char *)&status)[i] = 1;
		assert(ioctl_call(&zram, ZRAM_IOC_WB_STATUS, &status, FMODE_READ, false) == -EINVAL);
	}
	status = (struct zram_wb_status){ .size = sizeof(status), .version = 2 };
	assert(ioctl_call(&zram, ZRAM_IOC_WB_STATUS, &status, FMODE_READ, false) == -EPROTONOSUPPORT);
	status.version = 1; status.size--;
	assert(ioctl_call(&zram, ZRAM_IOC_WB_STATUS, &status, FMODE_READ, false) == -EINVAL);
	status.size++;
	admin = false;
	assert(ioctl_call(&zram, ZRAM_IOC_WB_STATUS, &status, FMODE_READ, false) == -EPERM);
	admin = true;
	copyin_fault = true;
	assert(ioctl_call(&zram, ZRAM_IOC_WB_STATUS, &status, FMODE_READ, false) == -EFAULT);
	copyin_fault = false; copyout_bytes = 80;
	assert(ioctl_call(&zram, ZRAM_IOC_WB_STATUS, &status, FMODE_READ, false) == -EFAULT);
	copyout_bytes = SIZE_MAX;
	u64 old_instance = zram.wb_instance;
	zram.wb_generation = U64_MAX;
	random_script[0] = 0; random_script[1] = old_instance; random_script[2] = old_instance + 1;
	random_count = 3;
	zram_wb_generation_advance(&zram);
	assert(zram.wb_generation == 1 && zram.wb_instance == old_instance + 1 && random_pos == 3);
	clear_idle(&zram);
	finish(&zram);
	/* Resettable statistics never overwrite the identity fields. */
	old_instance = zram.wb_instance;
	memset(&zram.stats, 0, sizeof(zram.stats));
	assert(zram.wb_instance == old_instance && zram.wb_generation == 1);
}
static void test_fops_routing(void)
{
	const struct block_device_operations *tables[] = {
		&zram_devops, &zram_wb_devops,
	};

	for (unsigned int table = 0; table < 2; table++) {
		for (unsigned int compat = 0; compat < 2; compat++) {
			struct zram zram;
			struct zram_wb_exec req;
			struct zram_wb_status status;
			const struct block_device_operations *ops = tables[table];

			reset(&zram);
			candidates(&zram, 1, false);
			req = request(&zram, 1, 1);
			assert(ioctl_route(ops, &zram, ZRAM_IOC_WB_EXEC, &req,
					   FMODE_READ, compat) == -EBADF);
			status = (struct zram_wb_status) {
				.size = sizeof(status), .version = ZRAM_WB_ABI_VERSION,
			};
			admin = false;
			assert(ioctl_route(ops, &zram, ZRAM_IOC_WB_STATUS, &status,
					   FMODE_READ, compat) == -EPERM);
			assert(!copyouts && !submit_count);
			admin = true;
			assert(!ioctl_route(ops, &zram, ZRAM_IOC_WB_STATUS, &status,
					    FMODE_READ, compat));
			assert(status.instance == zram.wb_instance);
			assert(status.generation == zram.wb_generation);
			assert(status.page_size == PAGE_SIZE && status.pages_stored == 1);
			assert(status.capabilities == 15 && status.state_flags == 3);
			assert(!ioctl_route(ops, &zram, ZRAM_IOC_WB_EXEC, &req,
					    FMODE_WRITE, compat));
			assert(req.installed_pages == 1 && req.submitted_pages == 1);
			assert(req.stop_reason == ZRAM_WB_STOP_INSTALLED_LIMIT);
			assert(!req.op_errno && copyouts == 2 && submit_count == 1);
			assert(ioctl_route(ops, &zram, _IO('Z', 0x23), &req,
					   FMODE_WRITE, compat) == -ENOIOCTLCMD);
			finish(&zram);
		}
	}
}
static void test_copyout_compat(void)
{
	for (unsigned int compat = 0; compat < 2; compat++) {
		for (unsigned int partial = 0; partial < 3; partial++) {
			struct zram zram;
			reset(&zram);
			candidates(&zram, 65, false);
			zram.wb_limit_enable = true;
			zram.bd_wb_limit = 65ULL << (PAGE_SHIFT - 12);
			struct zram_wb_exec req = request(&zram, 33, 33);
			copyout_bytes = partial == 0 ? 0 : partial == 1 ? 125 : sizeof(req);
			assert(ioctl_call(&zram, ZRAM_IOC_WB_EXEC, &req, FMODE_WRITE, compat) ==
			       (partial < 2 ? -EFAULT : 0));
			assert(submit_count == 2 && zram.stats.bd_count == 33 && zram.stats.bd_writes == 33);
			assert(zram.bd_wb_limit == (32ULL << (PAGE_SHIFT - 12)) && copyouts == 1);
			/* Reusing an ID is a new request, not a deduplicated reply. */
			copyout_bytes = SIZE_MAX;
			req = request(&zram, 1, 1);
			assert(!ioctl_call(&zram, ZRAM_IOC_WB_EXEC, &req, FMODE_WRITE, compat));
			assert(req.installed_pages == 1 && req.scanned_pages == 34);
			assert(zram.stats.bd_count == 34 && copyouts == 2);
			clear_idle(&zram);
			finish(&zram);
		}
	}
}
#endif
static void test_demand(void)
{
	struct zram z;
	int fake_disk = 0;
	reset(&z);
	z.disk = (void *)&fake_disk;
	z.policy_low_mib = 1;
	z.policy_high_mib = 2;
	z.disksize = 8ULL << PAGE_SHIFT;
	z.backing_dev = &z;
	zram_policy_sample(&z, false);
	assert(!z.demand_active && z.demand_sequence);
	z.policy_enabled = true;
	/* Shim physical-pool sampler is stats.pages_stored. */
	sampled_pool_pages = (2ULL << 20) >> PAGE_SHIFT;
	zram_policy_sample(&z, false);
	assert(z.demand_active);
	u64 sequence = z.demand_sequence;
	sampled_pool_pages = (3ULL << 19) >> PAGE_SHIFT;
	zram_policy_sample(&z, false);
	assert(z.demand_active && z.demand_sequence == sequence);
	zram_policy_sample(&z, true);
	assert(z.demand_sequence == sequence + 1); /* aging/config epoch */
	sampled_pool_pages = (1ULL << 20) >> PAGE_SHIFT;
	zram_policy_sample(&z, false);
	assert(!z.demand_active);
	sampled_pool_pages = (2ULL << 20) >> PAGE_SHIFT;
	zram_policy_sample(&z, false);
	assert(z.demand_active);
	z.backing_dev = NULL;
	zram_policy_sample(&z, false);
	assert(!z.demand_active);
	sequence = z.demand_sequence;
	z.wb_generation++;
	zram_policy_sample(&z, false);
	assert(z.demand_sequence == sequence + 1);
	sampled_pool_pages = 7;
	finish(&z);
}

static void test_policy(void)
{
	struct zram z;
	struct zram_wb_policy_exec p = {0};

	p.exec.size = sizeof(p);
	p.exec.version = ZRAM_WB_POLICY_ABI_VERSION;
	p.exec.request_id = 1;
	p.exec.max_installed_pages = 2;
	p.exec.max_attempt_pages = 2;
	p.max_scanned_pages = 4;
	p.demand_sequence = 1;
	p.min_age_ns = 1;
	assert(!zram_wb_policy_validate(&p));
	p.max_scanned_pages = U64_MAX;
	assert(zram_wb_policy_validate(&p) == -EINVAL);
	p.max_scanned_pages = 4;
	p.next_index = 1;
	assert(zram_wb_policy_validate(&p) == -EINVAL);
	p.next_index = 0;
	p.exec.scanned_pages = 1;
	assert(zram_wb_policy_validate(&p) == -EINVAL);
	p.exec.scanned_pages = 0;
	reset(&z);
	z.disksize = 8ULL << PAGE_SHIFT;
	for (unsigned int i = 0; i < 8; i++) {
		object(&z, i);
		/* No IDLE flag, no compress-low filter and no cumulative markings. */
		z.table[i].flags &= ~(BIT(ZRAM_COMPRESS_LOW) | ZRAM_IDLE_COUNT_MASK);
		z.stats.lowratio_pages--;
		z.table[i].ac_time = 1;
	}
	z.table[6].ac_time = 0; /* unknown */
	z.table[7].ac_time = INT64_MAX; /* future */
	p.start_index = 6;
	assert(!zram_writeback_engine_policy(&z, &p.exec, &p));
	assert(p.exec.scanned_pages == 4 && p.exec.installed_pages == 2);
	assert(p.exec.submitted_pages == 2 && p.next_index == 2);
	assert(p.exec.stop_reason == ZRAM_WB_STOP_INSTALLED_LIMIT);
	finish(&z);
	reset(&z);
	z.disksize = 8ULL << PAGE_SHIFT;
	memset(&p.exec.scanned_pages, 0,
	       sizeof(p.exec) - offsetof(struct zram_wb_exec, scanned_pages));
	p.start_index = 7;
	p.next_index = 0;
	assert(!zram_writeback_engine_policy(&z, &p.exec, &p));
	assert(p.exec.scanned_pages == 4 && !p.exec.submitted_pages);
	assert(p.exec.stop_reason == ZRAM_WB_STOP_SCANNED_LIMIT && p.next_index == 3);
	finish(&z);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(!zram_wb_thread); /* Host does not start the kernel thread. */
	stop_worker = true;
	assert(!zram_wb_completion_thread(NULL)); /* Idle kthread stop path. */
	stop_worker = false;
#ifdef CONFIG_ZRAM_WRITEBACK_IOCTL
	puts("TAP version 13\n1..15");
#else
	puts("TAP version 13\n1..9");
#endif
	test_demand();
	puts("ok 1 - actual demand hysteresis, config/age and lifecycle sequences");
	test_policy();
	puts("ok 2 - cold policy actual selector, wrapping cursor, scan bound and strict validation");
	test_budget();
	puts("ok 3 - extracted reserve/refund budget units and disabled limit");
	test_async_window();
	puts("ok 4 - async inline/deferred/out-of-order completion with a four-BIO window");
	test_cancel();
	puts("ok 5 - deferred short grant, budget charging and free/rewrite/read/ABA invalidation");
	test_async_failure();
	test_async_uncommitted();
	puts("ok 6 - first completion error stops submissions, refunds uncommitted work, and drains");
	test_store_sizes();
	puts("ok 7 - extracted store 1/31/32/33/65 pages and logical/backing gaps");
	test_store_requestmax();
	test_store_modes();
	test_store_exclusions();
	puts("ok 8 - legacy caps/modes/page-index/count boundaries/exclusion guards");
	test_store_faults();
	test_secondary_alloc_failure();
	puts("ok 9 - extracted engine OOM fallback/signal/read/IO/BIO/budget/ENOSPC cleanup");
#ifdef CONFIG_ZRAM_WRITEBACK_IOCTL
	test_exec_counts_caps();
	test_exec_caps_split();
	puts("ok 10 - dual caps + invalidation + gaps/prealloc-one; disabled quota unchanged");
	test_exec_invalidations();
	puts("ok 11 - install-point counts and single-count submitted/unsubmitted invalidations");
	test_exec_faults();
	test_exec_late_io_error();
	puts("ok 12 - EXEC errors/stages/precedence; second-BIO failure keeps first installs");
	test_exec_validation();
	puts("ok 13 - strict EXEC validation, all reserved/output bytes and permissions");
	test_status_lifecycle();
	puts("ok 14 - sampled STATUS, read-only access, stale/not-ready/generation/wrap");
	test_copyout_compat();
	test_fops_routing();
	puts("ok 15 - actual fops native/compat routing and partial copyout; no ID dedup");
#endif
	return 0;
}
