/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Reflex CPUFreq governor: 6.12 -> 4.9 compatibility shims.
 *
 * Local cube-law-only adaptation of the v0.4.0 backport.
 * No modern EM or adjust_perf interface is supplied.
 */
#ifndef _CPUFREQ_REFLEX_COMPAT_H
#define _CPUFREQ_REFLEX_COMPAT_H

#include <linux/bitops.h>
#include <linux/cpufreq.h>
#include <linux/math64.h>
#include <linux/types.h>

/*
 * 4.9 keeps arch_scale_freq_invariant() private to kernel/sched/sched.h,
 * while 6.12 exports it from <linux/sched/topology.h>. Mirror 4.9's own
 * definition so the governor sees the same answer schedutil does.
 */
#ifndef arch_scale_freq_invariant
#ifdef arch_scale_freq_capacity
#define arch_scale_freq_invariant()	(true)
#else
#define arch_scale_freq_invariant()	(false)
#endif
#endif

/*
 * 4.9 has no to_gov_attr_set(); the tunables kobject is embedded directly.
 */
static inline struct gov_attr_set *to_gov_attr_set(struct kobject *kobj)
{
	return container_of(kobj, struct gov_attr_set, kobj);
}

static inline unsigned long rfx_map_util_perf(unsigned long util)
{
	return util + (util >> 2);
}

/*
 * This tree has driver flags but no CPUFREQ_NEED_UPDATE_LIMITS contract.
 * Limits explicitly invalidate the cached request in rfx_limits().
 * This private compatibility test must not inspect unrelated local bits.
 */
#ifndef CPUFREQ_NEED_UPDATE_LIMITS
#define CPUFREQ_NEED_UPDATE_LIMITS	BIT(0)
#endif

static inline bool cpufreq_driver_test_flags(u16 flags)
{
	return false;
}

/*
 * 4.9's struct cpufreq_policy has no ->transition_delay_us: the only knob
 * is the driver's transition_latency, in ns. Convert it the same way
 * drivers/cpufreq/cpufreq_governor.c does when it derives the sampling
 * rate, and never return 0 (an interval of 0 would disable rate limiting).
 */
static inline unsigned int
cpufreq_policy_transition_delay_us(struct cpufreq_policy *policy)
{
	unsigned int delay = policy->cpuinfo.transition_latency / 1000;

	return delay ? delay : 1;
}

#endif /* _CPUFREQ_REFLEX_COMPAT_H */
