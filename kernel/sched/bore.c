// SPDX-License-Identifier: GPL-2.0
/*
 * Experimental source-derived CFS adaptation, based on Baka 0b1b6bdf6ce9.
 * Not the complete upstream BORE 7 scheduler. See selftests/bore/README.
 */
#include <linux/sched/bore.h>
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/math64.h>

/* Boot-fixed: no task walk, stale-state re-enable or live vruntime rollback. */
unsigned int sysctl_sched_bore __ro_after_init =
	IS_ENABLED(CONFIG_SCHED_BORE_DEFAULT_ON);
unsigned int sysctl_sched_burst_penalty_offset __read_mostly = 24;
unsigned int sysctl_sched_burst_penalty_scale __read_mostly = 1536;
unsigned int sysctl_sched_burst_smoothness __read_mostly = 1;
unsigned int sysctl_sched_burst_inherit_type __read_mostly = 1;

static int __init bore_setup(char *str)
{
	unsigned int enabled;

	if (kstrtouint(str, 0, &enabled) || enabled > 1)
		return 0;
	sysctl_sched_bore = enabled;
	return 1;
}
__setup("sched_bore=", bore_setup);

#define BORE_MAX_PENALTY ((40U << 8) - 1)

static u16 bore_penalty(u64 ns)
{
	u32 log, diff;
	unsigned int offset = READ_ONCE(sysctl_sched_burst_penalty_offset);
	unsigned int scale = READ_ONCE(sysctl_sched_burst_penalty_scale);
	int leading;

	if (!ns)
		return 0;
	leading = fls64(ns);
	/* Source Q8 leading-bit interpolation (not exact log2(ns + 1)). */
	log = (leading << 8) | ((ns << (64 - leading)) >> 56);
	if (log <= offset * 256)
		return 0;
	diff = log - offset * 256;
	return min_t(u64, (u64)diff * scale >> 10, BORE_MAX_PENALTY);
}

static u16 bore_smooth(u16 prev, u16 curr)
{
	unsigned int smooth = READ_ONCE(sysctl_sched_burst_smoothness);

	/* Bound matches sysctl; defensive clamp also protects the shift. */
	smooth = min(smooth, 16U);
	return ((u32)prev * ((1U << smooth) - 1) + curr) >> smooth;
}

void bore_fork(struct task_struct *p, struct task_struct *parent)
{
	unsigned int enabled = READ_ONCE(sysctl_sched_bore);

	/* dup_task_struct copied the parent: clear even when disabled. */
	p->bore.burst_time = 0;
	p->bore.curr_penalty = 0;
	p->bore.prev_penalty = enabled &&
		READ_ONCE(sysctl_sched_burst_inherit_type) == 1 ?
		READ_ONCE(parent->bore.penalty) : 0;
	p->bore.penalty = p->bore.prev_penalty;
}

void bore_sleep(struct task_struct *p)
{
	struct bore_ctx *bc = &p->bore;
	unsigned int enabled = READ_ONCE(sysctl_sched_bore);

	if (!enabled)
		return;
	bc->prev_penalty = bore_smooth(bc->prev_penalty, bc->curr_penalty);
	bc->curr_penalty = 0;
	bc->penalty = bc->prev_penalty;
	bc->burst_time = 0;
}

static void bore_update(struct task_struct *p, u64 delta_exec)
{
	struct bore_ctx *bc = &p->bore;

	bc->burst_time = min_t(u64, U64_MAX - delta_exec,
				bc->burst_time) + delta_exec;
	bc->curr_penalty = bore_penalty(bc->burst_time);
	bc->penalty = max_t(u16, bc->curr_penalty,
		bore_smooth(bc->prev_penalty, bc->curr_penalty));
}

static u64 bore_vruntime_delta(struct task_struct *p, u64 delta)
{
	unsigned int penalty = min_t(unsigned int, p->bore.penalty >> 8, 39);
	u32 factor = 40 + penalty;
	u64 quotient, remainder, tail;

	/* floor(delta * factor / 40), saturating BEFORE any overflow. */
	quotient = div64_u64(delta, 40);
	remainder = delta - quotient * 40;
	tail = div64_u64(remainder * factor, 40);
	if (quotient > div64_u64(U64_MAX - tail, factor))
		return U64_MAX;
	return quotient * factor + tail;
}

u64 bore_account_delta(struct task_struct *p, u64 runtime, u64 delta)
{
	/* One enable snapshot covers both state update and scaling. */
	unsigned int enabled = READ_ONCE(sysctl_sched_bore);

	if (!enabled)
		return delta;
	bore_update(p, runtime);
	return bore_vruntime_delta(p, delta);
}
