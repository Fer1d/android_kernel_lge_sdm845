// SPDX-License-Identifier: GPL-2.0
/*
 * Restricted 4.9 POC candidate experiment, derived from the atomic64 idle
 * bitmap/affinity/RR search of Masahito Suzuki's POC v2.6.3 and Rangga's
 * a14de411a712..aae434a0d671 backport. Not a verbatim upstream selector.
 *
 * Only <=64 CPU, non-SMT systems are eligible. A global CPU-ID bitmap avoids
 * storing topology-relative bits across domain replacement: each lookup uses
 * the current RCU-protected domain/group and live capacity, then validates the
 * selected CPU. Bitmap entries are hints, never CPU eligibility or reservations.
 * No lockless representation, recent-CPU shortcut, or overload skip-fallback.
 * Included by fair.c; deliberately not a separate Makefile object.
 */
#include <linux/cpu.h>
#include <linux/sysctl.h>

DEFINE_STATIC_KEY_FALSE(sched_poc_writer);
static DEFINE_STATIC_KEY_FALSE(sched_poc_reader);
static atomic64_t poc_idle_mask = ATOMIC64_INIT(0);
static DEFINE_PER_CPU(unsigned int, poc_rr);
static DEFINE_MUTEX(poc_control_lock);
/* 0 off, 1 shadow, 2 restricted active. Published after writer resync. */
static int poc_mode;

enum poc_stat {
	POC_LEGACY_ATTEMPTS, POC_LEGACY_CANDIDATES, POC_LEGACY_SELECTED,
	POC_EAS_ATTEMPTS, POC_EAS_CANDIDATES, POC_EAS_SELECTED,
	POC_REJECTED, POC_POLICY_FALLBACK, POC_NO_CANDIDATE, POC_NR_STATS
};
static atomic64_t poc_stats[POC_NR_STATS];

static void poc_count(enum poc_stat stat)
{
	atomic64_inc(&poc_stats[stat]);
}

/* POC_TEST_BEGIN primitives */
static bool poc_cpu_in_range(int cpu)
{
	return cpu >= 0 && cpu < nr_cpu_ids && cpu < 64;
}

static u64 poc_cpu_bit(int cpu)
{
	if (!poc_cpu_in_range(cpu))
		return 0;
	return 1ULL << cpu;
}

static int poc_pick(u64 mask, unsigned int counter)
{
	unsigned int pick;

	if (!mask)
		return -1;
	/* Select by rank, not bit-position rotation: holes must not give the
	 * following CPU extra probability. At most 63 bits are cleared.
	 */
	pick = counter % hweight64(mask);
	while (pick--)
		mask &= mask - 1;
	return __builtin_ctzll(mask);
}
/* POC_TEST_END primitives */

void sched_poc_idle_update(int cpu, bool idle)
{
	u64 bit = poc_cpu_bit(cpu);

	if (!bit)
		return;
	if (idle)
		atomic64_or(bit, &poc_idle_mask);
	else
		atomic64_andnot(bit, &poc_idle_mask);
}

static void poc_resync_cpu(void *unused)
{
	int cpu = smp_processor_id();

	/* Runs on the owning CPU; no remote store can overwrite its newer exit. */
	sched_poc_idle_update(cpu, cpu_online(cpu) && idle_cpu(cpu));
}

/* POC_TEST_BEGIN transition */
static void poc_set_mode(int mode)
{
	/* Caller serializes controls and holds get_online_cpus(). */
	if (mode == READ_ONCE(poc_mode))
		return;

	/* Stop consumers first; existing RCU-sched lookups finish before reset. */
	static_branch_disable(&sched_poc_reader);
	synchronize_sched();
	WRITE_ONCE(poc_mode, 0);
	static_branch_disable(&sched_poc_writer);
	synchronize_sched();
	atomic64_set(&poc_idle_mask, 0);
	if (!mode)
		return;

	/* Writers precede resync, consumers follow it. Never reset a live map. */
	static_branch_enable(&sched_poc_writer);
	on_each_cpu(poc_resync_cpu, NULL, 1);
	WRITE_ONCE(poc_mode, mode);
	static_branch_enable(&sched_poc_reader);
}
/* POC_TEST_END transition */

static bool poc_has_rtg(struct task_struct *p)
{
#ifdef CONFIG_SCHED_WALT
	/* Reject membership, not only a currently preferred RTG cluster. */
	return task_related_thread_group(p) != NULL;
#else
	return false;
#endif
}

static bool poc_policy_eligible(struct task_struct *p)
{
	/* No NUMA balancing calls, fork/exec, running-task migration, or SMT. */
	if (IS_ENABLED(CONFIG_NUMA_BALANCING) || IS_ENABLED(CONFIG_SCHED_SMT) ||
	    p->state != TASK_WAKING || poc_has_rtg(p) ||
	    task_boost_policy(p) != SCHED_BOOST_NONE ||
	    schedtune_task_boost(p) != 0 ||
	    ((current->flags | p->flags) & PF_WAKE_UP_IDLE) ||
	    (sched_feat(EAS_USE_NEED_IDLE) && schedtune_prefer_idle(p)))
		return false;
	return !task_uclamp_active(p);
}

/* POC_TEST_BEGIN candidate */
static bool poc_candidate_valid(struct task_struct *p, int cpu, int seed,
				const struct cpumask *scope)
{
	/* Range checks MUST precede cpumask, rq, per-CPU, capacity or node access. */
	if (!poc_cpu_in_range(cpu) || !poc_cpu_in_range(seed))
		return false;
	if (!cpumask_test_cpu(cpu, scope) ||
	    !cpumask_test_cpu(cpu, tsk_cpus_allowed(p)) ||
	    !cpu_online(cpu) || !cpu_active(cpu) || cpu_isolated(cpu))
		return false;
	if (cpu_to_node(cpu) != cpu_to_node(seed) ||
	    capacity_orig_of(cpu) != capacity_orig_of(seed) ||
	    walt_cpu_high_irqload(cpu) || is_reserved(cpu) ||
	    !task_fits_max(p, cpu) ||
	    boosted_task_util(p) > capacity_orig_of(cpu) || !idle_cpu(cpu))
		return false;
#ifdef CONFIG_UCLAMP_TASK
	if (uclamp_rq_get(cpu_rq(cpu), UCLAMP_MIN) ||
	    uclamp_rq_get(cpu_rq(cpu), UCLAMP_MAX) != SCHED_CAPACITY_SCALE)
		return false;
#endif
	return true;
}
/* POC_TEST_END candidate */

/* Caller holds RCU and preemption disabled, including both fair entry paths. */
static int poc_candidate(struct task_struct *p, int seed,
			 const struct cpumask *scope, bool eas)
{
	u64 mask;
	int cpu;
	unsigned int counter;

	if (!static_branch_unlikely(&sched_poc_reader))
		return -1;
	poc_count(eas ? POC_EAS_ATTEMPTS : POC_LEGACY_ATTEMPTS);
	if (!poc_cpu_in_range(seed) || !poc_policy_eligible(p)) {
		poc_count(POC_POLICY_FALLBACK);
		return -1;
	}
	/* NR_CPUS <= 64 and 64BIT are Kconfig requirements. */
	mask = atomic64_read(&poc_idle_mask) & cpumask_bits(scope)[0] &
		cpumask_bits(tsk_cpus_allowed(p))[0];
	counter = __this_cpu_inc_return(poc_rr);
	while (mask) {
		cpu = poc_pick(mask, counter);
		/* poc_pick(nonzero) is always in [0, 63], even if a
		 * stale/offline bit lies beyond today's nr_cpu_ids.
		 */
		mask &= ~(1ULL << cpu);
		if (!poc_candidate_valid(p, cpu, seed, scope)) {
			poc_count(POC_REJECTED);
			continue;
		}
		poc_count(eas ? POC_EAS_CANDIDATES : POC_LEGACY_CANDIDATES);
		return READ_ONCE(poc_mode) == 2 ? cpu : -1;
	}
	poc_count(POC_NO_CANDIDATE);
	return -1;
}

/* No eager clearing: EAS can reject a hint before choosing its final target. */
static int poc_legacy(struct task_struct *p, int target)
{
	struct sched_domain *sd;
	int cpu;

	if (!static_branch_unlikely(&sched_poc_reader) ||
	    sysctl_sched_cstate_aware || !poc_cpu_in_range(target))
		return -1;
	sd = rcu_dereference(per_cpu(sd_llc, target));
	if (!sd)
		return -1;
	cpu = poc_candidate(p, target, sched_domain_span(sd), false);
	if (cpu >= 0 && poc_candidate_valid(p, cpu, target, sched_domain_span(sd))) {
		poc_count(POC_LEGACY_SELECTED);
		return cpu;
	}
	return -1;
}

static int poc_mode_sysctl(struct ctl_table *table, int write,
			   void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int mode, ret;
	struct ctl_table tmp = *table;

	mutex_lock(&poc_control_lock);
	mode = READ_ONCE(poc_mode);
	tmp.data = &mode;
	ret = proc_dointvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (!ret && write) {
		get_online_cpus();
		poc_set_mode(mode);
		put_online_cpus();
	}
	mutex_unlock(&poc_control_lock);
	return ret;
}

static int poc_stats_sysctl(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long values[POC_NR_STATS];
	struct ctl_table tmp = *table;
	int i;

	if (write)
		return -EPERM;
	for (i = 0; i < POC_NR_STATS; i++)
		values[i] = atomic64_read(&poc_stats[i]);
	tmp.data = values;
	return proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
}

static int poc_zero;
static int poc_two = 2;
static struct ctl_table poc_sysctls[] = {
	{
		.procname = "sched_poc_mode",
		.maxlen = sizeof(int),
		.mode = 0644,
		.proc_handler = poc_mode_sysctl,
		.extra1 = &poc_zero,
		.extra2 = &poc_two,
	},
	{
		/* legacy attempt/candidate/selected; EAS attempt/candidate/selected;
		 * rejected, policy fallback, no candidate. Monotonic snapshots.
		 */
		.procname = "sched_poc_stats",
		.maxlen = sizeof(unsigned long) * POC_NR_STATS,
		.mode = 0444,
		.proc_handler = poc_stats_sysctl,
	},
	{ }
};

static int __init poc_sysctl_init(void)
{
	if (!register_sysctl("kernel", poc_sysctls))
		return -ENOMEM;
	if (IS_ENABLED(CONFIG_SCHED_POC_DEFAULT_ACTIVE)) {
		/* Match sysctl lock order and the complete writer/resync protocol. */
		mutex_lock(&poc_control_lock);
		get_online_cpus();
		poc_set_mode(2);
		put_online_cpus();
		mutex_unlock(&poc_control_lock);
	}
	return 0;
}
late_initcall(poc_sysctl_init);
