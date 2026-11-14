/* Host scaffolding around extracted fair.c, not a scheduler/WALT emulator. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

struct list_head { struct list_head *next, *prev; };
static void init_list(struct list_head *h) { h->next = h->prev = h; }
static bool list_empty(struct list_head *h) { return h->next == h; }
static void list_del(struct list_head *n)
{
	n->prev->next = n->next; n->next->prev = n->prev; init_list(n);
}
static void list_add(struct list_head *n, struct list_head *h)
{
	n->next = h->next; n->prev = h; h->next->prev = n; h->next = n;
}
static void list_move_tail(struct list_head *n, struct list_head *h)
{
	list_del(n); n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
#define list_first_entry(h, type, member) \
	((type *)((char *)(h)->next - offsetof(type, member)))
#define max_t(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#define lockdep_assert_held(lock) assert(*(lock))
#define schedstat_add(stat, n) ((stat) += (n))
#define sched_feat(name) (lb_min)
#define LBF_ALL_PINNED 0x01
#define LBF_NEED_BREAK 0x02
#define LBF_IGNORE_BIG_TASKS 0x100
#define LBF_IGNORE_PREFERRED_CLUSTER_TASKS 0x200
#define CPU_NOT_IDLE 0
#define CPU_NEWLY_IDLE 1
#define CPU_IDLE 2
static const unsigned int sched_nr_migrate_break = 32;
static int fair_sched_class;
struct task_struct {
	struct { struct list_head group_node; struct { int nr_failed_migrations_affine, nr_failed_migrations_running, nr_forced_migrations, nr_failed_migrations_hot; } statistics; } se;
	unsigned long cost;
	int cpu, queued, refs, eligible, moves, running, reject_on_redo, hot;
	void *sched_class;
};
struct root_domain { int overutilized; };
static struct root_domain rd = { 1 };
struct rq { struct list_head cfs_tasks; int nr_running, lock; struct root_domain *rd; };
struct domain { int nr_balance_failed, lb_gained[3], lb_hot_gained[3], cache_nice_tries; };
struct lb_env {
	struct rq *src_rq, *dst_rq;
	int src_cpu, dst_cpu, idle;
	long imbalance;
	unsigned int flags, loop, loop_break, loop_max;
	struct domain *sd;
	int *dst_grpmask, *cpus, new_dst_cpu;
	struct list_head tasks;
};
static struct rq queues[3];
static struct task_struct task[40];
static struct domain domain;
static bool lb_min, cross_cluster, small_dst, active, isolated;
static int mutate_successor, migrations;
static struct rq *cpu_rq(int cpu) { assert(cpu >= 0 && cpu < 3); return &queues[cpu]; }
static bool same_cluster(int a, int b) { (void)a; (void)b; return !cross_cluster; }
static int cpu_capacity(int cpu) { return small_dst && cpu == 1 ? 1 : 2; }
#ifdef CONFIG_SCHED_CAMBYSES_BUDGET
static void get_task_struct(struct task_struct *p) { assert(p->refs >= 1); p->refs++; }
static void put_task_struct(struct task_struct *p) { assert(p->refs > 1); p->refs--; }
static bool cpu_active(int cpu) { (void)cpu; return active; }
static bool cpu_isolated(int cpu) { (void)cpu; return isolated; }
static bool task_on_rq_queued(struct task_struct *p) { return p->queued == 1; }
static int task_cpu(struct task_struct *p) { return p->cpu; }
static struct rq *task_rq(struct task_struct *p) { return cpu_rq(p->cpu); }
static bool task_running(struct rq *rq, struct task_struct *p) { (void)rq; return p->running; }
#endif
#ifndef CONFIG_SCHED_CAMBYSES_BUDGET
static bool task_running(struct rq *rq, struct task_struct *p) { (void)rq; return p->running; }
#endif
#define schedstat_inc(stat) (++(stat))
#define LBF_SOME_PINNED 8
#define LBF_DST_PINNED 4
#define for_each_cpu_and(cpu, mask, cpus) for ((cpu) = 0; (cpu) < 3; (cpu)++)
static int task_group(struct task_struct *p) { return !p->eligible; }
static bool throttled_lb_pair(int group, int src, int dst)
{ (void)src; (void)dst; return group; }
static int *tsk_cpus_allowed(struct task_struct *p) { return &p->eligible; }
static bool cpumask_test_cpu(int cpu, int *mask) { (void)cpu; return *mask; }
static bool energy_aware(void) { return false; }
static bool task_in_related_thread_group(struct task_struct *p) { (void)p; return false; }
static unsigned long task_util(struct task_struct *p) { return p->cost; }
static long cpu_util_cum(int cpu, int unused) { (void)cpu; (void)unused; return 0; }
static int schedtune_task_boost(struct task_struct *p) { return p->reject_on_redo ? 1 : 0; }
static int migrate_degrades_locality(struct task_struct *p, struct lb_env *env)
{ (void)env; return p->hot; }
static int task_hot(struct task_struct *p, struct lb_env *env)
{ (void)env; return p->hot; }
#include "predicate.h"
static unsigned long task_h_load(struct task_struct *p) { return p->cost; }
static void detach_task(struct task_struct *p, struct lb_env *env)
{
	int i;
	assert(env->src_rq->lock && p->queued == 1 && p->cpu == env->src_cpu);
	/* No extra references/candidates may survive double_lock_balance. */
	for (i = 0; i < 40; i++) assert(task[i].refs == 1);
	assert(p->moves == 0);
	p->moves++; migrations++; p->queued = 2;
	list_del(&p->se.group_node); env->src_rq->nr_running--;
	env->src_rq->lock = 0;
	/* Simulate a successor being dequeued while double_lock_balance drops src. */
	if (mutate_successor && task[1].queued == 1) {
		list_del(&task[1].se.group_node); task[1].queued = 0;
		env->src_rq->nr_running--;
	}
	env->src_rq->lock = 1; p->cpu = env->dst_cpu;
}
#include "selected.h"

static struct lb_env setup(long budget, int count)
{
	struct lb_env env;
	int i;
	memset(queues, 0, sizeof(queues)); memset(task, 0, sizeof(task));
	memset(&domain, 0, sizeof(domain)); memset(&env, 0, sizeof(env));
	rd.overutilized = 1;
	lb_min = cross_cluster = small_dst = isolated = false; active = true;
	mutate_successor = migrations = 0;
	for (i = 0; i < 3; i++) { init_list(&queues[i].cfs_tasks); queues[i].rd = &rd; }
	for (i = 0; i < 40; i++) {
		init_list(&task[i].se.group_node); task[i].refs = 1;
		task[i].sched_class = &fair_sched_class;
		if (i < count) {
			task[i].eligible = task[i].queued = 1;
			list_add(&task[i].se.group_node, &queues[0].cfs_tasks);
			list_move_tail(&task[i].se.group_node, &queues[0].cfs_tasks);
		}
	}
	/* Restore numerical order after insertion. */
	init_list(&queues[0].cfs_tasks);
	for (i = count - 1; i >= 0; i--) list_add(&task[i].se.group_node, &queues[0].cfs_tasks);
	queues[0].nr_running = count + 1; queues[0].lock = 1;
	env.src_rq = &queues[0]; env.dst_rq = &queues[1]; env.dst_cpu = 1;
	env.imbalance = budget; env.loop_max = count; env.loop_break = 32;
	env.sd = &domain; init_list(&env.tasks);
	return env;
}
/* Returning an env by value requires reinitializing its self-linked head. */
#define SETUP(budget, count) do { env = setup(budget, count); init_list(&env.tasks); } while (0)
static void no_refs(void) { int i; for (i = 0; i < 40; i++) assert(task[i].refs == 1); }

int main(void)
{
	struct lb_env env;
	SETUP(10, 3);
	task[0].cost = 12; task[1].cost = 7; task[2].cost = 3;
#ifdef CONFIG_SCHED_CAMBYSES_BUDGET
	assert(detach_tasks(&env) == 2);
	assert(!task[0].moves && task[1].moves && task[2].moves && !env.imbalance);
#else
	assert(detach_tasks(&env) == 1);
	assert(task[0].moves && !task[1].moves && env.imbalance == -2);
#endif
	no_refs();
#ifdef CONFIG_SCHED_CAMBYSES_BUDGET
	/* Smallest overshoot, stable first-on-tie, strict even on two-task rq. */
	SETUP(10, 3); task[0].cost = 30; task[1].cost = task[2].cost = 20;
	assert(detach_tasks(&env) == 1 && task[1].moves && !env.imbalance); no_refs();
	SETUP(10, 1); task[0].cost = 11;
	assert(detach_tasks(&env) == 1 && (env.flags & LBF_CAMBYSES_MOVED)); no_refs();
	/* Sticky across calls, retarget and vendor retries: no second concession. */
	env.imbalance = 1; env.loop = 0; env.loop_max = 2;
	task[1].cost = 9; task[1].queued = task[1].eligible = 1;
	list_add(&task[1].se.group_node, &env.src_rq->cfs_tasks); env.src_rq->nr_running++;
	env.dst_cpu = 2; env.dst_rq = &queues[2]; small_dst = cross_cluster = true;
	assert(detach_tasks(&env) == 0 && !task[1].moves); no_refs();
	/* Vendor redo must discard the first-pass cheapest candidate. */
	SETUP(10, 2); small_dst = true; rd.overutilized = 0; task[0].cost = 20; task[0].reject_on_redo = 1;
	task[1].cost = 30;
	assert(detach_tasks(&env) == 1 && task[0].moves && !task[1].moves); no_refs();
	/* Successful strict detach forbids using an earlier overshoot. */
	SETUP(10, 3); task[0].cost = 20; task[1].cost = 5; task[2].eligible = 0;
	assert(detach_tasks(&env) == 1 && task[1].moves && !task[0].moves && env.imbalance == 5); no_refs();
	/* Zero cost clamps to one; huge overshoot saturates without signed UB. */
	SETUP(1, 1); assert(detach_tasks(&env) == 1 && !env.imbalance); no_refs();
	SETUP(10, 1); task[0].cost = ULONG_MAX;
	assert(detach_tasks(&env) == 1 && !env.imbalance); no_refs();
	/* Eligibility and target revalidation, independently exercise each gate. */
	SETUP(10, 1); assert(lb_concession_valid(&task[0], &env));
	task[0].queued = 0; assert(!lb_concession_valid(&task[0], &env)); task[0].queued = 2;
	assert(!lb_concession_valid(&task[0], &env)); task[0].queued = 1;
	task[0].cpu = 2; assert(!lb_concession_valid(&task[0], &env)); task[0].cpu = 0;
	task[0].running = 1; assert(!lb_concession_valid(&task[0], &env)); task[0].running = 0;
	task[0].eligible = 0; assert(!lb_concession_valid(&task[0], &env)); task[0].eligible = 1;
	task[0].sched_class = NULL; assert(!lb_concession_valid(&task[0], &env));
	task[0].sched_class = &fair_sched_class;
	env.src_rq = &queues[2]; assert(!lb_concession_valid(&task[0], &env)); env.src_rq = &queues[0];
	env.dst_rq = &queues[2]; assert(!lb_concession_valid(&task[0], &env)); env.dst_rq = &queues[1];
	active = false; assert(!lb_concession_valid(&task[0], &env)); active = true;
	isolated = true; assert(!lb_concession_valid(&task[0], &env)); isolated = false;
	/* LB_MIN rejection must not be resurrected by concession. */
	SETUP(1, 1); task[0].cost = 2; lb_min = true;
	assert(detach_tasks(&env) == 0); no_refs();
#endif
#ifdef CONFIG_SCHED_CAMBYSES_BUDGET
	/* P2: task 33 must be scanned after the first 32-task pause. */
	SETUP(10, 34);
	{ int i; for (i = 0; i < 34; i++) task[i].cost = 1000; }
	task[32].cost = 50;
	assert(detach_tasks(&env) == 0 && !migrations && (env.flags & LBF_NEED_BREAK));
	no_refs(); env.src_rq->lock = 0; env.src_rq->lock = 1;
	env.flags &= ~LBF_NEED_BREAK;
	assert(detach_tasks(&env) == 1 && task[32].moves && !task[0].moves); no_refs();
	/* P2: actual migration predicate must account the hot candidate once. */
	SETUP(10, 1); task[0].cost = 100; task[0].hot = 1;
	domain.nr_balance_failed = 1;
	assert(detach_tasks(&env) == 1);
	assert(domain.lb_hot_gained[0] == 1 && task[0].se.statistics.nr_forced_migrations == 1);
	assert(lb_concession_valid(&task[0], &env) == false); no_refs();
#endif

	/* A removed old successor must never be visited after the lock drop. */
	SETUP(10, 3); task[0].cost = 4; task[1].cost = 3; task[2].cost = 6;
	mutate_successor = 1;
	assert(detach_tasks(&env) == 2 && task[0].moves && !task[1].moves && task[2].moves);
	assert(task[1].queued == 0 && !env.imbalance); no_refs();
	/* Empty/zero budget and newly-idle stop retain legacy behavior. */
	SETUP(0, 1); assert(detach_tasks(&env) == 0); no_refs();
	SETUP(10, 0); assert(detach_tasks(&env) == 0); no_refs();
	SETUP(10, 2); env.idle = CPU_NEWLY_IDLE; task[0].cost = 4; task[1].cost = 6;
#ifdef CONFIG_PREEMPT
	assert(detach_tasks(&env) == 1);
#else
	assert(detach_tasks(&env) == 2);
#endif
	no_refs();
	puts("PASS extracted detach_tasks budget, overshoot, redo, lifetime and lock-drop tests");
	return 0;
}
