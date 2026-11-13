// SPDX-License-Identifier: GPL-2.0
/* API-boundary simulation only. actual.c is extracted verbatim on every run. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef uint64_t u64;
typedef uint64_t sector_t;
#define SECTOR_SHIFT 9
#define GFP_KERNEL 0
#define DM_MAPIO_SUBMITTED 0
#define DM_MAPIO_REMAPPED 1
#define REQ_OP_READ 0U
#define REQ_OP_WRITE 1U
#define REQ_OP_FLUSH 2U
#define REQ_OP_DISCARD 3U
#define REQ_OP_WRITE_ZEROES 4U
#define REQ_OP_SECURE_ERASE 5U
#define REQ_PREFLUSH (1U << 8)
#define REQ_FUA (1U << 9)
#define REQ_SYNC (1U << 10)
struct inode { u64 size; unsigned long i_ino; };
struct block_device { struct inode *bd_inode; };
struct dm_dev { struct block_device *bdev; };
struct file { struct inode inode; };
struct f2fs_extm_lease { int unused; };
struct dm_target {
    sector_t begin, len;
    void *private;
    const char *error;
    unsigned int num_flush_bios;
};
struct bio {
    struct { sector_t bi_sector; unsigned int bi_size; } bi_iter;
    struct block_device *bi_bdev;
    unsigned int bi_opf;
    int bi_error, ended;
    unsigned int partial_calls;
    sector_t accepted;
};
static unsigned int bio_op(const struct bio *b) { return b->bi_opf & 255; }
static unsigned int bio_sectors(const struct bio *b) { return b->bi_iter.bi_size >> 9; }
static sector_t dm_target_offset(struct dm_target *t, sector_t s) { return s - t->begin; }
static void bio_endio(struct bio *b) { b->ended++; }
static void dm_accept_partial_bio(struct bio *b, sector_t n)
{
    assert(n && n < bio_sectors(b));
    b->partial_calls++;
    b->accepted = n;
    b->bi_iter.bi_size = (unsigned int)(n << 9);
}
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
static void *kzalloc(size_t n, int flags) { (void)flags; return calloc(1, n); }
#define kfree free
static int kstrtoull(const char *s, unsigned int base, u64 *v)
{
    char *end;
    unsigned long long n;
    if (!*s || *s == '-') return -EINVAL;
    errno = 0;
    n = strtoull(s, &end, base);
    if (errno || *end) return -EINVAL;
    *v = n;
    return 0;
}
static struct file fake_file = { { 0, 123 } };
static struct inode device_inode;
static struct block_device backing = { &device_inode };
static struct block_device original;
static struct dm_dev fake_dev = { &backing };
static struct f2fs_extm_lease fake_lease;
static const struct f2fs_extm_extent *fixture;
static unsigned int fixture_count;
static int file_refs, lease_refs, device_refs;
static struct file *extm_open(const char *path)
{
    (void)path; /* Path traversal and real file opening are explicitly out of scope. */
    file_refs++;
    return &fake_file;
}
static int f2fs_extm_acquire(struct file *f, u64 bytes, struct f2fs_extm_lease **l)
{
    (void)f; (void)bytes; lease_refs++; *l = &fake_lease; return 0;
}
static struct block_device *f2fs_extm_bdev(const struct f2fs_extm_lease *l)
{ (void)l; return &backing; }
static int dm_get_device_extm(struct dm_target *t, struct file *f,
    const struct f2fs_extm_lease *l,
    struct block_device *(*get)(const struct f2fs_extm_lease *), struct dm_dev **d)
{
    (void)t; (void)f; assert(get(l) == &backing);
    device_refs++; *d = &fake_dev; return 0;
}
static const struct f2fs_extm_extent *f2fs_extm_extents(struct f2fs_extm_lease *l)
{ (void)l; return fixture; }
static unsigned int f2fs_extm_nr_extents(struct f2fs_extm_lease *l)
{ (void)l; return fixture_count; }
static u64 i_size_read(struct inode *i) { return i->size; }
static struct inode *file_inode(struct file *f) { return &f->inode; }
static void filp_close(struct file *f, void *unused)
{ (void)f; (void)unused; assert(file_refs == 1); file_refs--; }
static void dm_put_device(struct dm_target *t, struct dm_dev *d)
{ (void)t; (void)d; assert(device_refs == 1); device_refs--; }
static void f2fs_extm_release(struct f2fs_extm_lease *l)
{ (void)l; assert(lease_refs == 1); lease_refs--; }

#include "actual.c"

static unsigned int checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); abort(); } } while (0)

static void clean_target(struct dm_target *t)
{
    /* Not extm_dtr: no real flush/release semantics are claimed here. */
    struct extm_c *ec = t->private;
    dm_put_device(t, ec->dev);
    f2fs_extm_release(ec->lease);
    free(ec);
    t->private = NULL;
}
static int construct(struct dm_target *t, const struct f2fs_extm_extent *e,
                     unsigned int n, u64 bytes, sector_t capacity)
{
    char size[32];
    char *argv[] = { "/host-fixture", size };
    snprintf(size, sizeof(size), "%llu", (unsigned long long)bytes);
    fixture = e; fixture_count = n;
    device_inode.size = capacity << 9;
    int r = extm_ctr(t, 2, argv);
    CHECK(file_refs == 0);
    if (r) CHECK(!device_refs && !lease_refs && !t->private);
    else CHECK(device_refs == 1 && lease_refs == 1 && t->num_flush_bios == 1);
    return r;
}
static struct bio make_bio(sector_t sector, unsigned int n, unsigned int op)
{
    struct bio b = { .bi_iter = { sector, n << 9 }, .bi_bdev = &original,
                     .bi_opf = op };
    return b;
}
static void mapped(struct dm_target *t, sector_t s, unsigned int n,
                   unsigned int op, sector_t physical, unsigned int accepted)
{
    struct bio b = make_bio(s, n, op);
    CHECK(extm_map(t, &b) == DM_MAPIO_REMAPPED);
    CHECK(b.bi_bdev == &backing && b.bi_iter.bi_sector == physical);
    CHECK(bio_sectors(&b) == accepted && b.bi_opf == op);
    CHECK(!b.bi_error && !b.ended);
    CHECK(b.partial_calls == (unsigned int)(accepted < n));
    if (accepted < n) CHECK(b.accepted == accepted);
}
static void rejected(struct dm_target *t, sector_t s, unsigned int n,
                     unsigned int op, int error)
{
    struct bio b = make_bio(s, n, op);
    CHECK(extm_map(t, &b) == DM_MAPIO_SUBMITTED);
    CHECK(b.bi_error == error && b.ended == 1);
    CHECK(b.bi_bdev == &original && b.bi_iter.bi_sector == s);
    CHECK(bio_sectors(&b) == n && !b.partial_calls && b.bi_opf == op);
}
static void validation(void)
{
    struct f2fs_extm_extent good[] = { {0, 100, 4}, {4, 200, 8}, {12, 50, 4} };
    struct f2fs_extm_extent bad[3];
    struct dm_target t = { .len = 16 };
    CHECK(construct(&t, good, 3, 8192, 208) == 0);
    clean_target(&t);
    for (unsigned int kind = 0; kind < 10; kind++) {
        memcpy(bad, good, sizeof(bad));
        switch (kind) {
        case 0: bad[0].logical_sector = 1; break; /* leading hole */
        case 1: bad[1].logical_sector = 5; break; /* middle hole */
        case 2: bad[1].logical_sector = 3; break; /* overlap */
        case 3: bad[1].nr_sectors = 0; break;
        case 4: bad[2].nr_sectors = 3; break; /* trailing hole */
        case 5: bad[2].nr_sectors = 5; break; /* exceeds target */
        case 6: bad[1].physical_sector = 209; break;
        case 7: bad[1].physical_sector = 205; break;
        case 8: bad[1].physical_sector = UINT64_MAX; break;
        case 9: bad[1].nr_sectors = UINT64_MAX; break;
        }
        CHECK(construct(&t, bad, 3, 8192, 208) == -EINVAL);
    }
    CHECK(construct(&t, NULL, 3, 8192, 208) == -EINVAL);
    CHECK(construct(&t, good, 0, 8192, 208) == -EINVAL);
    CHECK(construct(&t, good, 3, 0, 208) == -EINVAL);
    CHECK(construct(&t, good, 3, 8193, 208) == -EINVAL);
    CHECK(construct(&t, good, 3, 8192 + 512, 208) == -EINVAL);
    CHECK(construct(&t, good, 3, (4ULL << 30) + 512, 208) == -EINVAL);
    t.begin = 1;
    CHECK(construct(&t, good, 3, 8192, 208) == -EINVAL);
    t.begin = 0;
    char *badargs[] = { "/host-fixture", "not-a-size" };
    CHECK(extm_ctr(&t, 2, badargs) == -EINVAL);
    CHECK(extm_ctr(&t, 0, NULL) == -EINVAL);
    puts("ok: real ctr target validation (holes/overlap/size/capacity/overflow)");
}
static void mapping(void)
{
    struct f2fs_extm_extent e[] = { {0, 100, 4}, {4, 900, 8}, {12, 50, 4} };
    struct dm_target t = { .len = 16 };
    CHECK(!construct(&t, e, 3, 8192, 1000));
    mapped(&t, 3, 10, REQ_OP_WRITE | REQ_FUA | REQ_SYNC, 103, 1);
    mapped(&t, 4, 9, REQ_OP_WRITE | REQ_FUA, 900, 8);
    mapped(&t, 12, 1, REQ_OP_WRITE | REQ_FUA, 50, 1);
    /* Replay the logical remainder as DM would after partial acceptance. */
    sector_t cursor = 2;
    unsigned int left = 14, splits = 0;
    while (left) {
        struct bio b = make_bio(cursor, left, REQ_OP_WRITE | REQ_FUA);
        CHECK(extm_map(&t, &b) == DM_MAPIO_REMAPPED);
        unsigned int n = bio_sectors(&b);
        CHECK(n && n <= left && (b.bi_opf & REQ_FUA));
        CHECK(b.bi_iter.bi_sector == (splits == 0 ? 102 : splits == 1 ? 900 : 50));
        cursor += n; left -= n; splits++;
    }
    CHECK(cursor == 16 && splits == 3);
    /* Exhaustive bounded oracle: linear lookup is test expectation only;
     * implementation under test is the actual inline binary search. */
    for (unsigned int s = 0; s < 16; s++) {
        unsigned int k = s < 4 ? 0 : s < 12 ? 1 : 2;
        for (unsigned int n = 1; n <= 16 - s; n++) {
            unsigned int remain = (unsigned int)(e[k].logical_sector + e[k].nr_sectors - s);
            mapped(&t, s, n, REQ_OP_READ, e[k].physical_sector + s - e[k].logical_sector,
                   n < remain ? n : remain);
        }
    }
    mapped(&t, UINT64_MAX, 0, REQ_OP_FLUSH | REQ_PREFLUSH, 0, 0);
    mapped(&t, 0, 0, REQ_OP_READ, 0, 0);
    mapped(&t, 0, 0, REQ_OP_WRITE | REQ_FUA, 0, 0);
    rejected(&t, 0, 1, REQ_OP_WRITE | REQ_PREFLUSH, -EIO);
    rejected(&t, 0, 1, REQ_OP_FLUSH, -EIO);
    for (unsigned int op = REQ_OP_DISCARD; op <= REQ_OP_SECURE_ERASE; op++) {
        rejected(&t, 0, 1, op, -EOPNOTSUPP);
        rejected(&t, 0, 0, op, -EOPNOTSUPP);
    }
    rejected(&t, 16, 1, REQ_OP_READ, -EIO);
    rejected(&t, 15, 2, REQ_OP_WRITE, -EIO);
    rejected(&t, UINT64_MAX, 2, REQ_OP_READ, -EIO);
    /* Defense-in-depth lookup tests with deliberately invalid fixtures;
     * these must not be confused with constructor-accepted mappings. */
    e[0].logical_sector = 1;
    rejected(&t, 0, 1, REQ_OP_READ, -EIO);
    e[0].logical_sector = 0; e[0].nr_sectors = 2;
    rejected(&t, 2, 1, REQ_OP_READ, -EIO);
    clean_target(&t);
    puts("ok: actual binary search/remap/partial/remainder/FUA/flush/unsupported");
}
static void large_mapping(void)
{
    const sector_t sectors = (4ULL << 30) >> 9;
    struct f2fs_extm_extent e[] = { {0, 128, sectors} };
    struct dm_target t = { .len = sectors };
    CHECK(!construct(&t, e, 1, 4ULL << 30, sectors + 128));
    mapped(&t, 0, 1, REQ_OP_READ, 128, 1);
    mapped(&t, sectors - 1, 1, REQ_OP_WRITE | REQ_FUA, sectors + 127, 1);
    rejected(&t, sectors - 1, 2, REQ_OP_READ, -EIO);
    rejected(&t, sectors, 1, REQ_OP_WRITE, -EIO);
    rejected(&t, UINT64_MAX, 1, REQ_OP_READ, -EIO);
    clean_target(&t);
    struct f2fs_extm_extent fragmented[] = {
        {0, 9000000, sectors - 2}, {sectors - 2, 64, 2}
    };
    CHECK(!construct(&t, fragmented, 2, 4ULL << 30, 18000000));
    mapped(&t, sectors - 3, 3, REQ_OP_WRITE | REQ_FUA,
           9000000 + sectors - 3, 1);
    mapped(&t, sectors - 2, 2, REQ_OP_WRITE | REQ_FUA, 64, 2);
    rejected(&t, sectors - 2, 3, REQ_OP_READ, -EIO);
    clean_target(&t);
    puts("ok: one-extent/fragmented 4 GiB boundary (metadata only, no data allocation)");
}
int main(void)
{
    validation(); mapping(); large_mapping();
    CHECK(!file_refs && !lease_refs && !device_refs);
    printf("PASS: %u assertions\n", checks);
    return 0;
}
