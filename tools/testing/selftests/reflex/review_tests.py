# SPDX-License-Identifier: GPL-2.0
import runpy
from pathlib import Path
setup = runpy.run_path(str(Path(__file__).with_name('test_support.py')))['setup']
import re, subprocess
w, out = setup('Reflex actual worker and fast callback state host tests')
s=(w/'drivers/cpufreq/cpufreq_reflex.c').read_text()
def fn(name):
 m=re.search(r'^(?:static[^\n]*\n)?[^\n]*\b'+name+r'\([^;]*?\)\n\{',s,re.M);assert m,name
 end=m.end();depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[m.start():end]+'\n'
# Reject the exact missing API/field instead of making the harness supply them.
for forbidden in ('cpufreq_can_do_remote_dvfs','dvfs_possible_from_any_cpu'):
 assert forbidden not in s,forbidden
 assert forbidden not in (Path(__file__).with_name('run_tests.py')).read_text(),forbidden
header=(w/'include/linux/cpufreq.h').read_text();kt=(w/'include/linux/kthread.h').read_text()
assert 'fast_switch_enabled' in header and 'related_cpus' in header and 'kthread_bind_mask' in kt
c=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>
typedef uint64_t u64;typedef int64_t s64;
struct cpumask {unsigned bits;};
struct cpufreq_policy {bool fast_switch_enabled;struct cpumask *cpus,*related_cpus;unsigned cur;};
struct update_util_data {int unused;};struct kthread_work {void (*fn)(struct kthread_work*);};struct kthread_worker {int unused;};struct task_struct {int unused;};struct irq_work {int unused;};struct mutex {int unused;};
struct rfx_policy {struct cpufreq_policy *policy;int update_lock;bool limits_changed,need_freq_update,work_in_progress;u64 last_freq_update_time;s64 freq_update_delay_ns;unsigned next_freq;struct kthread_work work;struct kthread_worker worker;struct task_struct *thread;struct irq_work irq_work;struct mutex work_lock;};
struct rfx_cpu {struct update_util_data update_util;struct rfx_policy *rfx_policy;unsigned cpu;u64 last_update;};
struct sched_param {int sched_priority;};
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define unlikely(x) (x)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,y) ((x)=(y))
#define smp_mb() ((void)0)
#define CPUFREQ_ENTRY_INVALID (~0u)
#define CPUFREQ_NEED_UPDATE_LIMITS 1
#define SCHED_CPUFREQ_UCLAMP 256
#define SCHED_CPUFREQ_FORCE_UPDATE 128
#define SCHED_CPUFREQ_RT_DL 3
#define MAX_USER_RT_PRIO 100
#define SCHED_FIFO 1
#define CPUFREQ_RELATION_L 0
#define pr_err(...) ((void)0)
#define pr_warn(...) ((void)0)
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((intptr_t)(p))
static unsigned caller,actual,fast_calls,target_calls,last_target,bind_calls,wakes,stops,create_calls;static int create_error,scheduler_error;static struct task_struct task;static struct cpumask *bound;static unsigned candidate=1000;
static unsigned smp_processor_id(void){return caller;}
static bool cpumask_test_cpu(unsigned cpu,const struct cpumask *m){return m->bits&(1u<<cpu);}
static unsigned cpumask_first(const struct cpumask *m){return __builtin_ctz(m->bits);}
static bool cpufreq_driver_test_flags(unsigned f){(void)f;return false;}
static void raw_spin_lock(int *l){assert(!*l);*l=1;}static void raw_spin_unlock(int *l){assert(*l);*l=0;}
#define raw_spin_lock_irqsave(l,f) ((f)=0,raw_spin_lock(l))
#define raw_spin_unlock_irqrestore(l,f) ((void)(f),raw_spin_unlock(l))
static unsigned long arch_scale_cpu_capacity(void *p,unsigned cpu){(void)p;(void)cpu;return 1024;}
static void rfx_iowait_boost(struct rfx_cpu*c,u64 t,unsigned f){(void)c;(void)t;(void)f;}
static void rfx_sample(struct rfx_cpu*c,u64 t){(void)c;(void)t;}
static void rfx_ignore_dl_rate_limit(struct rfx_cpu*c){(void)c;}
static unsigned long rfx_iowait_apply(struct rfx_cpu*c,u64 t,unsigned long cap){(void)c;(void)t;(void)cap;return 0;}
static unsigned long rfx_util(struct rfx_cpu*c,unsigned long b){(void)c;(void)b;return 400;}
static unsigned rfx_floor(struct rfx_cpu*c,unsigned long cap){(void)c;(void)cap;return 0;}
static unsigned rfx_get_next_freq(struct rfx_policy*q,unsigned long u,unsigned long cap,unsigned f){(void)q;(void)u;(void)cap;(void)f;return candidate;}
static unsigned rfx_next_freq_shared(struct rfx_cpu*c,u64 t){(void)c;(void)t;return candidate;}
static unsigned cpufreq_driver_fast_switch(struct cpufreq_policy*p,unsigned f){(void)p;last_target=f;fast_calls++;return actual;}
static void irq_work_queue(struct irq_work*w){(void)w;}
static void mutex_lock(struct mutex*m){(void)m;}static void mutex_unlock(struct mutex*m){(void)m;}
static void mutex_init(struct mutex*m){(void)m;}
static int __cpufreq_driver_target(struct cpufreq_policy*p,unsigned f,unsigned relation){(void)p;(void)relation;target_calls++;last_target=f;return 0;}
static void kthread_init_work(struct kthread_work*w,void(*f)(struct kthread_work*)){w->fn=f;}
static void kthread_init_worker(struct kthread_worker*w){(void)w;}
static int kthread_worker_fn(void *p){(void)p;return 0;}
static struct task_struct*kthread_create(int(*fn)(void*),void*p,const char*name,unsigned cpu){(void)fn;(void)p;(void)name;(void)cpu;create_calls++;return create_error?(void*)(intptr_t)create_error:&task;}
static int sched_setscheduler_nocheck(struct task_struct*t,int policy,const struct sched_param*p){assert(t==&task&&policy==SCHED_FIFO&&p->sched_priority==50);return scheduler_error;}
static int kthread_stop(struct task_struct*t){assert(t==&task);stops++;return 0;}
static void kthread_bind_mask(struct task_struct*t,const struct cpumask*m){assert(t==&task);bound=(struct cpumask*)m;bind_calls++;}
static void rfx_irq_work(struct irq_work*w){(void)w;}
static void init_irq_work(struct irq_work*w,void(*fn)(struct irq_work*)){(void)w;(void)fn;}
static int wake_up_process(struct task_struct*t){assert(t==&task);wakes++;return 0;}
'''
for name in ('rfx_should_update_freq','rfx_update_next_freq','rfx_deferred_update','rfx_update_single_freq','rfx_update_shared','rfx_work','rfx_kthread_create'):c+=fn(name)
c+=r'''
int main(void){
 struct cpumask active={.bits=3},related={.bits=15};
 struct cpufreq_policy p={.fast_switch_enabled=true,.cpus=&active,.related_cpus=&related,.cur=300};
 struct rfx_policy q={.policy=&p,.freq_update_delay_ns=100};struct rfx_cpu cpu={.rfx_policy=&q};
 void(*callbacks[])(struct update_util_data*,u64,unsigned)={rfx_update_single_freq,rfx_update_shared};
 for(unsigned i=0;i<2;i++){
  q.next_freq=0;q.last_freq_update_time=0;caller=0;actual=937;p.cur=300;unsigned before=fast_calls;
  callbacks[i](&cpu.update_util,100,SCHED_CPUFREQ_UCLAMP);assert(fast_calls==before+1&&p.cur==937&&last_target==1000&&q.next_freq==1000&&!q.update_lock);
  candidate=1200;actual=CPUFREQ_ENTRY_INVALID;callbacks[i](&cpu.update_util,200,SCHED_CPUFREQ_UCLAMP);assert(p.cur==937&&q.next_freq==1200&&!q.update_lock);
  caller=2;candidate=1400;before=fast_calls;callbacks[i](&cpu.update_util,300,SCHED_CPUFREQ_UCLAMP);assert(fast_calls==before&&p.cur==937&&q.need_freq_update&&q.next_freq==1200);
  caller=1;actual=1333;callbacks[i](&cpu.update_util,301,0);assert(fast_calls==before+1&&p.cur==1333&&!q.need_freq_update);
  p.fast_switch_enabled=false;caller=3;candidate=1600;callbacks[i](&cpu.update_util,400,SCHED_CPUFREQ_UCLAMP);assert(q.work_in_progress);unsigned targets=target_calls;rfx_work(&q.work);assert(!q.work_in_progress&&target_calls==targets+1&&last_target==1600&&!q.update_lock);p.fast_switch_enabled=true;candidate=1000;
 }
 assert(rfx_kthread_create(&q)==0&&create_calls==0);
 p.fast_switch_enabled=false;assert(rfx_kthread_create(&q)==0&&bound==&related&&bind_calls==1&&wakes==1&&q.thread==&task&&q.work.fn==rfx_work);
 create_error=-12;assert(rfx_kthread_create(&q)==-12&&bind_calls==1&&wakes==1);create_error=0;scheduler_error=-1;assert(rfx_kthread_create(&q)==-1&&stops==1&&bind_calls==1&&wakes==1);
 puts("PASS extracted actual single/shared callbacks: actual != requested, invalid preserves cur, caller gate, forced retry, slow worker; worker create/bind/FIFO50/error states; missing API stubs rejected");
}
'''
f=out/'review-state.c';f.write_text(c)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined,address','-g',str(f),'-o',str(f.with_suffix(''))],check=True)
subprocess.run([str(f.with_suffix(''))],check=True)
