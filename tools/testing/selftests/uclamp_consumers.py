#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host-side scalar tests of the actual WALT/uclamp consumer functions.

Run with python3 -I tools/testing/selftests/uclamp_consumers.py.
No kernel configuration, generated file, or source is modified.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def function(source, name):
    """Extract a definition (not a prototype), retaining its actual C body."""
    import re
    match = re.search(r"(?:^static[^;{}]*\b|^unsigned long\s+|^bool\s+)" +
                      re.escape(name) + r"\([^;{}]*\)\s*\{", source, re.M)
    if not match:
        raise AssertionError("missing definition: " + name)
    start, pos = match.start(), match.end()
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos] + "\n"


GOV = (ROOT / "kernel/sched/cpufreq_schedutil.c").read_text()
FAIR = (ROOT / "kernel/sched/fair.c").read_text()
HEADER = (ROOT / "kernel/sched/sched.h").read_text()

PREAMBLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define SCHED_CAPACITY_SCALE 1024UL
#define NL_RATIO 75
#define SCHED_CPUFREQ_INTERCLUSTER_MIG 4
#define SCHED_CPUFREQ_RT_DL 8
#define SCHED_BOOST_ON_BIG 1
#define UCLAMP_MIN 0
#define UCLAMP_MAX 1
#define UCLAMP_FLAG_IDLE 1
#define READ_ONCE(x) (x)
#define unlikely(x) (x)
#define min(x,y) ((x) < (y) ? (x) : (y))
#define max(x,y) ((x) > (y) ? (x) : (y))
#define max_t(t,x,y) max((t)(x),(t)(y))
#define mult_frac(x,n,d) ((unsigned long)(x) * (n) / (d))
#define for_each_cpu(j,mask) for ((j)=0; (j)<2; ++(j))
#define per_cpu(array,cpu) array[cpu]
#define raw_spin_lock(x) ((void)0)
#define raw_spin_unlock(x) ((void)0)
#define trace_sched_boost_task(...) ((void)0)
typedef uint64_t u64;
typedef int64_t s64;
struct task_struct { unsigned long util, lo, hi; int cpu; };
struct clamp { unsigned long value; };
struct rq { struct clamp uclamp[2]; unsigned int uclamp_flags; };
static struct rq rqs[2];
static unsigned long capacities[2] = {512, 512};
#define cpu_rq(cpu) (&rqs[cpu])
static unsigned long arch_scale_cpu_capacity(void *unused, int cpu)
{ return capacities[cpu]; }
static unsigned int uclamp_eff_value(struct task_struct *p, int id)
{ return id == UCLAMP_MIN ? p->lo : p->hi; }
static unsigned int uclamp_rq_get(struct rq *rq, int id)
{ return rq->uclamp[id].value; }
struct cpufreq_policy {
 struct { unsigned int max_freq; } cpuinfo;
 int *cpus;
};
struct sugov_tunables { unsigned int hispeed_load; bool pl; };
struct sugov_policy {
 struct cpufreq_policy *policy; struct sugov_tunables *tunables;
 unsigned long avg_cap, hispeed_util; unsigned int cached_raw_freq;
};
struct sugov_cpu {
 struct sugov_policy *sg_policy;
 unsigned long iowait_boost, iowait_boost_max;
 unsigned long util, max; unsigned int flags, cpu;
 struct { unsigned long nl, pl; } walt_load;
 u64 last_update;
};
static struct sugov_cpu sugov_cpu[2];
static unsigned int stale_ns = 100;
static int sysctl_sched_use_walt_cpu_util = 1;
static unsigned long observed_util, observed_max;
static unsigned int get_next_freq(struct sugov_policy *p,
 unsigned long util, unsigned long max)
{ observed_util=util; observed_max=max; return 1250UL*util/max; }
static unsigned long task_util(struct task_struct *p) { return p->util; }
static long schedtune_task_margin(struct task_struct *p) { return 0; }
'''

TESTS = r'''
static void differential_neutral(void)
{
 struct sugov_tunables tun={.hispeed_load=90};
 struct cpufreq_policy policy={.cpuinfo.max_freq=2000000};
 struct sugov_policy sp={.policy=&policy, .tunables=&tun,
                         .avg_cap=400, .hispeed_util=450};
 struct sugov_cpu saved[2];
 unsigned long old_util, old_max, old_iowait[2];
 const unsigned long loads[]={0,100,400,700,1300};
 /* Execute the pinned pre-Uclamp function and current function identically. */
 for (int walt=0;walt<2;++walt)
 for (int pl=0;pl<2;++pl)
 for (int a=0;a<5;++a)
 for (int b=0;b<5;++b)
 for (int boost=0;boost<3;++boost)
 for (int stale=0;stale<2;++stale) {
  tun.pl=pl; sysctl_sched_use_walt_cpu_util=walt;
  for (int i=0;i<2;++i) {
   rqs[i].uclamp[0].value=0; rqs[i].uclamp[1].value=1024;
   capacities[i]=i ? 1024 : 512;
   sugov_cpu[i]=(struct sugov_cpu){
    .sg_policy=&sp, .cpu=i, .max=capacities[i],
    .util=loads[i ? b : a], .last_update=(stale && i) ? 0 : 200,
    .iowait_boost=boost ? (boost==1 ? 1000000 : 2000000) : 0,
    .iowait_boost_max=2000000,
    .walt_load={.nl=loads[(a+b+i)%5], .pl=loads[(a+2*b+i)%5]}
   };
   saved[i]=sugov_cpu[i];
  }
  legacy_sugov_next_freq_shared(&sugov_cpu[0],200);
  old_util=observed_util; old_max=observed_max;
  for (int i=0;i<2;++i) {
   old_iowait[i]=sugov_cpu[i].iowait_boost;
   sugov_cpu[i]=saved[i];
  }
  sugov_next_freq_shared(&sugov_cpu[0],200);
  assert(old_util==observed_util && old_max==observed_max);
  for (int i=0;i<2;++i)
   assert(old_iowait[i]==sugov_cpu[i].iowait_boost);
 }
}
static void reset(void)
{
 for (int i=0; i<2; ++i) {
  rqs[i].uclamp[0].value=0; rqs[i].uclamp[1].value=1024;
  rqs[i].uclamp_flags=0;
  sugov_cpu[i]=(struct sugov_cpu){0};
  capacities[i]=512;
 }
}
int main(void)
{
 struct task_struct p={.util=700, .lo=0, .hi=1024};
 struct sugov_tunables tun={.hispeed_load=90, .pl=true};
 struct cpufreq_policy policy={.cpuinfo.max_freq=2000000};
 struct sugov_policy sp={.policy=&policy, .tunables=&tun,
                         .avg_cap=400, .hispeed_util=450};
 unsigned long util, cap;
 reset();
 assert(!task_uclamp_active(&p));
 assert(task_uclamp_util(&p,1300)==1300);
 assert(boosted_task_util(&p)==700);
 assert(cpu_uclamp_util(0,1300,NULL)==1300);
 assert(!sugov_uclamp_active(0));
 assert(!sugov_uclamp_max_active(0));
 /* A neutral little CPU retains SPC/PL values above its capacity. */
 assert(sugov_uclamp_util(0,1300)==1300);
 sugov_cpu[0].sg_policy=&sp;
 sugov_cpu[0].util=400;
 sugov_cpu[0].max=512;
 sugov_cpu[0].walt_load.pl=1300;
 util=400; cap=512;
 sugov_walt_adjust(&sugov_cpu[0],&util,&cap);
 assert(util==1300);
#ifdef CONFIG_UCLAMP_TASK
 p.lo=600; p.hi=800;
 assert(task_uclamp_active(&p));
 assert(task_uclamp_util(&p,100)==600);
 assert(task_uclamp_util(&p,1000)==800);
 p.lo=0; p.hi=256;
 assert(boosted_task_util(&p)==256);
 /* A neutral runnable peer wins over this task's MAX. */
 assert(cpu_uclamp_util(0,700,&p)==700);
 rqs[0].uclamp[1].value=256;
 assert(sugov_uclamp_active(0) && sugov_uclamp_max_active(0));
 assert(sugov_uclamp_util(0,1300)==256);
 /* IOWAIT's kHz ratio is converted before applying WALT or clamps. */
 sugov_cpu[0].iowait_boost=2000000;
 sugov_cpu[0].iowait_boost_max=2000000;
 util=100; cap=512;
 sugov_iowait_boost(&sugov_cpu[0],&util,&cap);
 assert(util==2000000 && cap==2000000);
 sugov_util_to_capacity(0,&util,&cap);
 assert(util==512 && cap==512);
 sugov_walt_adjust(&sugov_cpu[0],&util,&cap);
 assert(sugov_uclamp_util(0,util)==256);
 /* NL and HISPEED likewise cannot escape the final MAX. */
 sugov_cpu[0].walt_load.pl=0;
 sugov_cpu[0].walt_load.nl=400;
 util=400;
 sugov_walt_adjust(&sugov_cpu[0],&util,&cap);
 assert(util==512 && sugov_uclamp_util(0,util)==256);
 sugov_cpu[0].walt_load.nl=0;
 util=400;
 sugov_walt_adjust(&sugov_cpu[0],&util,&cap);
 assert(util==450 && sugov_uclamp_util(0,util)==256);
 /* Aggregated MIN can win over MAX, as in the core helper. */
 rqs[0].uclamp[0].value=700;
 assert(sugov_uclamp_util(0,100)==700);
 reset();
#endif
 /* Test the actual policy loop, including its all-neutral legacy path. */
 tun.pl=false;
 sysctl_sched_use_walt_cpu_util=0;
 for (int i=0;i<2;++i) {
  sugov_cpu[i].sg_policy=&sp; sugov_cpu[i].cpu=i;
  sugov_cpu[i].max=512; sugov_cpu[i].last_update=100;
 }
 sugov_cpu[0].util=400; sugov_cpu[1].util=350;
 sugov_next_freq_shared(&sugov_cpu[0],100);
 assert(observed_util==400 && observed_max==512);
 /* Preserve the old WALT shared ordering when every clamp is neutral. */
 tun.pl=true; sysctl_sched_use_walt_cpu_util=1;
 sugov_cpu[0].walt_load.pl=1300;
 sugov_cpu[1].walt_load.nl=400;
 sugov_cpu[1].util=400;
 sugov_next_freq_shared(&sugov_cpu[0],100);
 assert(observed_util==512 && observed_max==512);
 tun.pl=false; sysctl_sched_use_walt_cpu_util=0;
 sugov_cpu[1].util=350;
#ifdef CONFIG_UCLAMP_TASK
 rqs[0].uclamp[1].value=128;
 sugov_next_freq_shared(&sugov_cpu[0],100);
 assert(observed_util==350 && observed_max==512);
 /* The other CPU's MIN is not squashed by CPU0's MAX. */
 rqs[1].uclamp[0].value=600;
 sugov_next_freq_shared(&sugov_cpu[0],100);
 assert(observed_util==600 && observed_max==512);
 rqs[1].uclamp[0].value=0;
 capacities[1]=1024; sugov_cpu[1].max=1024;
 sugov_cpu[1].util=200;
 sugov_next_freq_shared(&sugov_cpu[0],100);
 assert(observed_util==128 && observed_max==512);
#endif
 differential_neutral();
 puts("uclamp consumer scalar and 600-case legacy differential tests: PASS");
 return 0;
}
'''

# Also protect separation of performance demand from physical busy time.
for name in ("group_norm_util", "group_idle_state", "cpu_util_wake"):
    assert "uclamp" not in function(FAIR, name), name
assert "uclamp_rq_util_without" in function(FAIR, "group_max_util")
for name in ("sugov_update_single", "sugov_update_shared"):
    body = function(GOV, name)
    assert "SCHED_CPUFREQ_UCLAMP" in body
    assert "next_f, force" in body
assert "!force && sugov_up_down_rate_limit" in function(GOV, "sugov_update_commit")
assert "!sugov_uclamp_max_active" in function(GOV, "sugov_update_single")

# Use the real core header helper rather than a separately reimplemented clamp.
helper = function(HEADER, "uclamp_rq_util_with")
functions = helper
for name in ("task_uclamp_active", "task_uclamp_util", "cpu_uclamp_util",
             "boosted_task_util"):
    functions += function(FAIR, name)
for name in ("sugov_uclamp_active", "sugov_uclamp_util",
             "sugov_uclamp_max_active", "sugov_util_to_capacity",
             "sugov_iowait_boost", "sugov_walt_adjust",
             "sugov_next_freq_shared"):
    functions += function(GOV, name)

baseline = subprocess.run(
    ["git", "-C", str(ROOT), "show",
     "0d2fa4cc73acb1d54b2212f36abc65563cf30b21:kernel/sched/cpufreq_schedutil.c"],
    check=True, capture_output=True, text=True).stdout
legacy = function(baseline, "sugov_next_freq_shared").replace(
    "sugov_next_freq_shared", "legacy_sugov_next_freq_shared", 1)
functions += legacy

with tempfile.TemporaryDirectory(prefix="uclamp-consumers.test.") as tmp:
    source = Path(tmp) / "consumers.c"
    source.write_text(PREAMBLE + functions + TESTS)
    for enabled in (False, True):
        binary = Path(tmp) / ("on" if enabled else "off")
        cmd = [os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-Wall",
               "-Werror", "-Wno-unused-function", "-Wno-unused-parameter"]
        if enabled:
            cmd += ["-DCONFIG_UCLAMP_TASK"]
        cmd += [str(source), "-o", str(binary)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(binary)], check=True)
print("CONFIG off/on and performance/busy-time separation: PASS")
