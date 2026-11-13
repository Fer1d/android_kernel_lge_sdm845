// SPDX-License-Identifier: GPL-2.0
/* Host simulation only. actual_helpers.h is extracted afresh from the driver.
 * No devices, threads or kernel execution. Assertions are runtime checks.
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
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
	void *bi_bdev;
	struct { unsigned long bi_sector; } bi_iter;
	bool active;
};
struct zram_table_entry {
	union { unsigned long handle, element; };
	unsigned long flags;
};
struct zram_stats {
	atomic64_t bd_writes, bd_count, pages_stored, huge_pages, same_pages;
	atomic64_t compr_data_size, lowratio_pages;
};
struct zram {
	struct zram_table_entry table[SLOTS];
	unsigned long bitmap[BLOCKS / WORD_BITS];
	unsigned long nr_pages;
	bool wb_limit_enable;
	u64 bd_wb_limit;
	int wb_limit_lock, wb_mutex, init_lock;
	u64 disksize;
	void *bdev, *mem_pool, *backing_dev;
	struct zram_stats stats;
};

static unsigned int signal_at, signal_calls, add_fail_at, submit_count;
static unsigned int live_bios, live_pages, live_handles;
static int io_error;
static void (*during_io)(void);
static struct zram *active_zram;
static bool handles[SLOTS + 1];
static unsigned int submitted_sizes[128];
static unsigned long submitted_blocks[SLOTS];
#define current NULL
static void spin_lock(int *lock) { assert(!*lock); *lock = 1; }
static void spin_unlock(int *lock) { assert(*lock); *lock = 0; }
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
	live_bios++;
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
	return len;
}
static int submit_bio_wait(struct bio *bio)
{
	assert(bio->active && bio->count && active_zram);
	assert(submit_count < 128);
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
	if (during_io)
		during_io();
	return io_error;
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
static void cond_resched(void) { }
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
	if ((int)index == read_fail_index) return -EIO;
	memset(page->data, index, sizeof(page->data));
	return 0;
}

static void reset(struct zram *zram)
{
	assert(!live_bios && !live_pages && !live_handles && !live_allocs);
	memset(zram, 0, sizeof(*zram));
	memset(handles, 0, sizeof(handles));
	memset(submitted_blocks, 0, sizeof(submitted_blocks));
	zram->nr_pages = BLOCKS;
	signal_at = signal_calls = add_fail_at = submit_count = 0;
	io_error = 0;
	alloc_fail_at = alloc_calls = 0;
	read_fail_index = -1;
	mutex_interrupted = false;
	during_io = NULL;
	active_zram = zram;
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
static void prepare(struct zram *zram, struct zram_wb_batch *batch,
		    unsigned int nr)
{
	memset(batch, 0, sizeof(*batch));
	for (unsigned int i = 0; i < nr; i++) {
		object(zram, i);
		zram->table[i].flags |= BIT(ZRAM_UNDER_WB) | BIT(ZRAM_IDLE);
		batch->entries[i].index = i;
		batch->entries[i].blk_idx = alloc_block_bdev(zram);
		assert(batch->entries[i].blk_idx);
		batch->pages[i] = calloc(1, sizeof(struct page));
		assert(batch->pages[i]);
		live_pages++;
	}
	batch->nr = nr;
}
static void release_pages(struct zram_wb_batch *batch, unsigned int nr)
{
	for (unsigned int i = 0; i < nr; i++) {
		assert(batch->pages[i] && live_pages);
		free(batch->pages[i]);
		batch->pages[i] = NULL;
		live_pages--;
	}
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
static void test_flush(void)
{
	const u64 unit = 1ULL << (PAGE_SHIFT - 12);
	/* success, short grant, no grant, IO error, BIO add failure,
	 * signal before reservation, signal after reservation. */
	for (unsigned int scenario = 0; scenario < 7; scenario++) {
		struct zram zram;
		struct zram_wb_batch batch;
		unsigned long written = 7;
		reset(&zram);
		prepare(&zram, &batch, 32);
		zram.wb_limit_enable = true;
		zram.bd_wb_limit = (scenario == 1 ? 31 : scenario == 2 ? 0 : 32) * unit;
		u64 before = zram.bd_wb_limit;
		if (scenario == 3) io_error = -EIO;
		if (scenario == 4) add_fail_at = 17;
		if (scenario == 5) signal_at = 1;
		if (scenario == 6) signal_at = 2;
		int ret = zram_wb_flush(&zram, &batch, &written);
		unsigned int submitted = scenario == 0 || scenario == 3 ? 32 :
					 scenario == 1 ? 31 : 0;
		unsigned int installed = scenario == 3 ? 0 : submitted;
		assert(ret == (scenario == 0 ? 0 : scenario >= 5 ? -EINTR : -EIO));
		assert(!batch.nr && written == 7 + installed);
		assert(zram.bd_wb_limit == before - submitted * unit);
		assert(zram.stats.bd_writes == submitted);
		assert(zram.stats.bd_count == installed);
		assert(submit_count == !!submitted);
		assert(!zram_wb_complete(&zram, &batch, 0, -EIO));
		release_pages(&batch, 32);
		finish(&zram);
	}
	struct zram zram;
	struct zram_wb_batch batch = {0};
	unsigned long written = 0;
	reset(&zram);
	assert(!zram_wb_flush(&zram, &batch, &written));
	assert(!submit_count && !written);
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
static void test_cancel(void)
{
	struct zram zram;
	struct zram_wb_batch batch;
	unsigned long written = 0;
	reset(&zram);
	prepare(&zram, &batch, 5);
	zram.wb_limit_enable = true;
	zram.bd_wb_limit = 5ULL << (PAGE_SHIFT - 12);
	during_io = invalidate;
	assert(!zram_wb_flush(&zram, &batch, &written));
	assert(written == 1 && !zram.bd_wb_limit);
	assert(!zram_allocated(&zram, 0));
	for (unsigned int i = 1; i <= 3; i++) {
		assert(!zram_test_flag(&zram, i, ZRAM_WB));
		assert(zram_test_flag(&zram, i, ZRAM_COMPRESS_LOW));
		assert(zram.table[i].flags >> ZRAM_IDLE_COUNT_SHIFT == 3);
		assert(handles[i + 1]);
	}
	assert(zram_test_flag(&zram, 4, ZRAM_WB));
	release_pages(&batch, 5);
	finish(&zram);
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
static void interrupt_during_io(void)
{
	/* Delivery at the submit scheduling point, not a real host signal. */
	signal_at = 1;
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
	for (unsigned int scenario = 0; scenario < 8; scenario++) {
		struct zram zram;
		reset(&zram);
		candidates(&zram, 65, false);
		if (scenario == 0) mutex_interrupted = true;
		if (scenario == 1) signal_at = 5; /* preallocation interrupted */
		if (scenario == 2) signal_at = 35; /* one pending snapshot */
		if (scenario == 3) read_fail_index = 1;
		if (scenario == 4) io_error = -EIO;
		if (scenario == 5) add_fail_at = 2;
		if (scenario >= 6) {
			zram.wb_limit_enable = true;
			zram.bd_wb_limit = (scenario == 6 ? 31 : 0) *
				(1ULL << (PAGE_SHIFT - 12));
		}
		ssize_t ret = store(&zram, "idle");
		assert(ret == (scenario <= 2 ? -EINTR : -EIO));
		assert(zram.stats.bd_count == (scenario == 6 ? 31 : 0));
		assert(zram.stats.bd_writes == (scenario == 4 ? 32 : scenario == 6 ? 31 : 0));
		clear_idle(&zram);
		finish(&zram);
	}
	/* Signal arriving in submit: attempted IO remains charged, no installation. */
	{
		struct zram zram;
		reset(&zram);
		candidates(&zram, 65, false);
		zram.wb_limit_enable = true;
		zram.bd_wb_limit = 65ULL << (PAGE_SHIFT - 12);
		during_io = interrupt_during_io;
		assert(store(&zram, "idle") == -EINTR);
		assert(submit_count == 1 && zram.stats.bd_writes == 32);
		assert(!zram.stats.bd_count);
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
int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("TAP version 13\n1..6");
	test_budget();
	puts("ok 1 - extracted reserve/refund budget units and disabled limit");
	test_flush();
	puts("ok 2 - extracted flush success/error/partial/BIO failure/signal cleanup");
	test_cancel();
	puts("ok 3 - extracted complete simulated free/rewrite/read/ABA cancellation");
	test_store_sizes();
	puts("ok 4 - extracted store 1/31/32/33/65 pages and logical/backing gaps");
	test_store_requestmax();
	puts("ok 5 - extracted store request max including zero and multi-batch");
	test_store_faults();
	puts("ok 6 - extracted store OOM fallback/signal/read/IO/BIO/budget/ENOSPC cleanup");
	return 0;
}
