/* SPDX-License-Identifier: GPL-2.0 */
/* Userspace stubs; allocator and helper bodies are inserted by run.py. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_KERNEL 0
#define ZERO_SIZE_PTR ((void *)16)
static int fail_k, fail_v, kc, vc, kfreed, vfreed;
static gfp_t kflags, vflags;
static size_t ksize, vsize;
static void *live_k, *live_v;
static unsigned long cases;

static void reset(int kfail, int vfail)
{
	assert(!live_k && !live_v);
	fail_k = kfail;
	fail_v = vfail;
	kc = vc = kfreed = vfreed = 0;
	kflags = vflags = 0;
	ksize = vsize = 0;
}

static void *allocate(size_t size, gfp_t flags)
{
	void *p = malloc(size);
	assert(p);
	memset(p, flags & __GFP_ZERO ? 0 : 0xa5, size);
	return p;
}

static void *kmalloc(size_t size, gfp_t flags)
{
	kc++;
	kflags = flags;
	ksize = size;
	if (!size)
		return ZERO_SIZE_PTR;
	if (fail_k)
		return NULL;
	assert(!live_k);
	return live_k = allocate(size, flags);
}

static void *__vmalloc(size_t size, gfp_t flags, int prot)
{
	assert(prot == PAGE_KERNEL && size > PAGE_SIZE);
	assert((flags & GFP_KERNEL) == GFP_KERNEL);
	vc++;
	vflags = flags;
	vsize = size;
	if (fail_v)
		return NULL;
	assert(!live_v);
	return live_v = allocate(size, flags);
}

static int is_vmalloc_addr(const void *p)
{
	return p && p == live_v;
}

static void kfree(const void *p)
{
	if (!p || p == ZERO_SIZE_PTR)
		return;
	assert(p == live_k);
	free(live_k);
	live_k = NULL;
	kfreed++;
}

static void vfree(const void *p)
{
	assert(p && p == live_v);
	free(live_v);
	live_v = NULL;
	vfreed++;
}

/* EXTRACTED_FUNCTIONS */

static void check_bytes(void *p, size_t size, int zero)
{
	size_t i;
	if (!p || p == ZERO_SIZE_PTR)
		return;
	for (i = 0; i < size; i++)
		assert(((unsigned char *)p)[i] == (zero ? 0 : 0xa5));
}

static void allocation_case(size_t size, gfp_t flags, int zero,
			    int kfail, int vfail)
{
	gfp_t effective = flags | (zero ? __GFP_ZERO : 0);
	gfp_t expected = effective;
	int compatible = (effective & GFP_KERNEL) == GFP_KERNEL;
	int fallback = compatible && size > PAGE_SIZE && kfail;
	void *p;

	reset(kfail, vfail);
	if (compatible && size > PAGE_SIZE) {
		expected |= __GFP_NOWARN;
		if (!(effective & __GFP_REPEAT) ||
		    size <= (PAGE_SIZE << PAGE_ALLOC_COSTLY_ORDER))
			expected |= __GFP_NORETRY;
	}
	p = zero ? kvzalloc(size, flags) : kvmalloc(size, flags);
	assert(kc == 1 && kflags == expected && ksize == size);
	assert(vc == fallback);
	if (fallback)
		assert(vflags == effective && vsize == size);
	if (!size)
		assert(p == ZERO_SIZE_PTR);
	else
		assert(!!p == (!kfail || (fallback && !vfail)));
	check_bytes(p, size, !!(effective & __GFP_ZERO));
	kvfree(p);
	assert(kfreed == !!(size && !kfail));
	assert(vfreed == !!(fallback && !vfail));
	assert(!live_k && !live_v);
	cases++;
}

static void array_cases(void)
{
	void *p;
	size_t n;
	const size_t sizes[] = { 2, 3, 16, PAGE_SIZE, SIZE_MAX };

	for (n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++) {
		size_t size = sizes[n];
		reset(1, 1);
		assert(!kvmalloc_array(SIZE_MAX / size + 1, size, GFP_KERNEL));
		assert(!kvcalloc(SIZE_MAX / size + 1, size, GFP_KERNEL));
		assert(!kc && !vc);
		/* The largest non-overflowing product reaches the allocator. */
		assert(!kvmalloc_array(SIZE_MAX / size, size, GFP_KERNEL));
		assert(kc == 1 && ksize == (SIZE_MAX / size) * size);
		assert(vc == 1 && vsize == ksize);
		cases += 3;
	}
	reset(1, 1);
	assert(!kvmalloc_array(1, SIZE_MAX, GFP_KERNEL));
	assert(kc == 1 && vc == 1 && ksize == SIZE_MAX);
	reset(0, 0);
	p = kvcalloc(0, SIZE_MAX, GFP_KERNEL);
	assert(p == ZERO_SIZE_PTR && kc == 1 && !vc);
	kvfree(p);
	reset(0, 0);
	p = kvmalloc_array(SIZE_MAX, 0, GFP_KERNEL);
	assert(p == ZERO_SIZE_PTR && kc == 1 && !vc);
	kvfree(p);
	for (n = 0; n < 2; n++) {
		reset(n, 0);
		p = kvcalloc(PAGE_SIZE, 2, GFP_KERNEL);
		assert(p && ksize == 2 * PAGE_SIZE);
		check_bytes(p, 2 * PAGE_SIZE, 1);
		kvfree(p);
		assert(kfreed + vfreed == 1);
	}
	reset(1, 1);
	assert(!kvcalloc(PAGE_SIZE, 2, GFP_KERNEL));
	assert(kc == 1 && vc == 1 && !kfreed && !vfreed);
	kvfree(NULL);
	kvfree(ZERO_SIZE_PTR);
	cases += 6;
}

int main(void)
{
	const size_t sizes[] = { 0, 1, PAGE_SIZE - 1, PAGE_SIZE,
		PAGE_SIZE + 1, PAGE_SIZE << PAGE_ALLOC_COSTLY_ORDER,
		(PAGE_SIZE << PAGE_ALLOC_COSTLY_ORDER) + 1 };
	const gfp_t flags[] = { GFP_KERNEL, GFP_KERNEL_ACCOUNT,
		GFP_KERNEL | __GFP_ZERO, GFP_KERNEL | __GFP_REPEAT,
		GFP_KERNEL_ACCOUNT | __GFP_REPEAT | __GFP_NOWARN,
		GFP_NOFS, GFP_NOIO, GFP_ATOMIC, GFP_NOWAIT, 0,
		GFP_NOFS | __GFP_REPEAT | __GFP_ZERO };
	size_t i, j;
	int zero, kfail, vfail;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		for (j = 0; j < sizeof(flags) / sizeof(flags[0]); j++)
			for (zero = 0; zero < 2; zero++)
				for (kfail = 0; kfail < 2; kfail++)
					for (vfail = 0; vfail < 2; vfail++)
						allocation_case(sizes[i], flags[j], zero, kfail, vfail);
	array_cases();
	assert(!live_k && !live_v);
	printf("PASS: %lu cases (flags/zero/overflow/failures/kvfree)\n", cases);
	return 0;
}
