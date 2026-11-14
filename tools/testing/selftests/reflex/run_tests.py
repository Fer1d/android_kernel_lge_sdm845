# SPDX-License-Identifier: GPL-2.0
import runpy
from pathlib import Path
setup = runpy.run_path(str(Path(__file__).with_name('test_support.py')))['setup']
import re, subprocess
w, out = setup('Reflex extracted utilization, notification and lifecycle host tests')
s=(w/'drivers/cpufreq/cpufreq_reflex.c').read_text();h=(w/'kernel/sched/cpufreq_schedutil.c').read_text();sh=(w/'kernel/sched/sched.h').read_text()
def fn(text,name):
 m=re.search(r'^.*\b'+name+r'\([^;]*?\)\n\{',text,re.M);assert m,name
 start=m.start();a=m.end();depth=1
 while depth:
  if text[a]=='{':depth+=1
  if text[a]=='}':depth-=1
  a+=1
 return text[start:a]+'\n'
preamble=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>
typedef uint64_t u64; typedef int64_t s64; typedef uint32_t u32;
#define CONFIG_UCLAMP_TASK 1
#define SCHED_CAPACITY_SCALE 1024UL
#define RFX_FLOOR_N 65
#define UCLAMP_MIN 0
#define UCLAMP_MAX 1
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,y) ((x)=(y))
#define unlikely(x) (x)
#define smp_mb() ((void)0)
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define max_t(t,a,b) max((t)(a),(t)(b))
#define clamp(x,l,h) min(max((x),(l)),(h))
#define div64_u64(a,b) ((u64)(a)/(b))
#define mul_u64_u32_shr(a,b,c) ((u64)(((__uint128_t)(a)*(b))>>(c)))
#define CPUFREQ_NEED_UPDATE_LIMITS 1
struct cpufreq_policy { unsigned long ref; bool fast_switch_enabled; unsigned cpus; };
struct rfx_policy { struct cpufreq_policy *policy; unsigned floor[65],alpha_us,next_freq,cached_raw_freq; bool need_freq_update,limits_changed; u64 last_freq_update_time; s64 freq_update_delay_ns; };
struct rfx_cpu {unsigned cpu;struct rfx_policy *rfx_policy; unsigned long util,bw_min,bw_max,H;bool censored;unsigned N;};
static unsigned long raw[4],low[4],high[4],caps[4];static struct rfx_cpu cpus[4];
#define per_cpu(ignore,j) cpus[j]
#define for_each_cpu(j,mask) for ((j)=0;(j)<(mask);(j)++)
static unsigned long arch_scale_cpu_capacity(void *p,unsigned j){(void)p;return caps[j];}
static unsigned caller;
#define smp_processor_id() caller
#define cpumask_test_cpu(cpu,mask) ((cpu)<(mask))
static bool cpufreq_driver_test_flags(int f){(void)f;return false;}
static unsigned cpufreq_driver_resolve_freq(struct cpufreq_policy *p,unsigned f){(void)p;return f;}
static void sugov_get_util(unsigned long *u,unsigned long *c,int cpu){*u=raw[cpu];*c=caps[cpu];}
static bool sugov_uclamp_active(int cpu){return low[cpu]||high[cpu]!=1024;}
#define cpu_rq(cpu) (cpu)
static unsigned long uclamp_rq_get(int cpu,int id){return id==UCLAMP_MIN?low[cpu]:high[cpu];}
static unsigned long rfx_map_util_perf(unsigned long u){return u+(u>>2);}
static bool rfx_active(struct rfx_cpu *c){(void)c;return true;}
static unsigned long rfx_iowait_apply(struct rfx_cpu *c,u64 t,unsigned long cap){(void)c;(void)t;(void)cap;return 0;}
'''
code=preamble+fn(h,'cpufreq_get_effective_util')+fn(h,'cpufreq_get_capacity_ref_freq').replace('policy->cpuinfo.max_freq','policy->ref')+fn(s,'rfx_get_util')+fn(s,'rfx_util')+fn(s,'rfx_floor')+fn(s,'rfx_next_freq_shared')+fn(s,'rfx_should_update_freq')+fn(s,'rfx_update_next_freq')+fn(s,'rfx_icbrt')
a=h.index('#define RFX_LOAD_AVG_PERIOD');b=h.index('EXPORT_SYMBOL_GPL(cpufreq_pelt_decay)',a);code+=h[a:b]
code+=r'''
int main(void){
 struct cpufreq_policy p={.ref=2400000,.cpus=2};
 struct rfx_policy q={.policy=&p,.alpha_us=1000,.freq_update_delay_ns=1000000};
 for(unsigned i=0;i<4;i++){caps[i]=1024;high[i]=1024;cpus[i].cpu=i;cpus[i].rfx_policy=&q;cpus[i].censored=true;}
 for(unsigned i=0;i<65;i++)q.floor[i]=2400000;
 unsigned long count=0;
 for(unsigned c=256;c<=1024;c+=256)for(unsigned lo=0;lo<=1024;lo+=128)for(unsigned hi=0;hi<=1024;hi+=128)for(unsigned u=0;u<=1280;u+=80)for(unsigned boost=0;boost<=1024;boost+=128){
  caps[0]=c;low[0]=lo;high[0]=hi;raw[0]=u;cpus[0].H=1024;
  unsigned long l=min((unsigned long)lo,c),r=max(l,min((unsigned long)hi,c));
  unsigned long got=rfx_util(&cpus[0],boost);assert(got>=l&&got<=r);
  assert(rfx_floor(&cpus[0],c)<=p.ref*r/c);count++;
 }
 caps[0]=caps[1]=1024;low[0]=low[1]=0;high[0]=256;high[1]=1024;raw[0]=1024;raw[1]=512;cpus[0].H=cpus[1].H=0;
 assert(rfx_next_freq_shared(&cpus[0],0)==2400000); /* sibling not capped */
 q.floor[64]=0;assert(rfx_next_freq_shared(&cpus[0],0)==1500000); /* 1.25 * 512 */
 caps[1]=512;assert(rfx_next_freq_shared(&cpus[0],0)==2400000); /* per CPU normalization */
 assert(!rfx_should_update_freq(&q,1));q.need_freq_update=true;assert(rfx_should_update_freq(&q,1));
 assert(rfx_update_next_freq(&q,1,1234));assert(!q.need_freq_update);
 q.limits_changed=true;assert(rfx_should_update_freq(&q,2));assert(q.need_freq_update&&!q.limits_changed);
 assert(cpufreq_pelt_decay(1024,0)==1024);assert(cpufreq_pelt_decay(1024,32ULL<<20)==511);assert(cpufreq_pelt_decay(1024,2048ULL<<20)==0);
 for(u64 n=0;n<100000;n++){u64 c=rfx_icbrt(n);assert(c*c*c<=n&&(c+1)*(c+1)*(c+1)>n);}
 printf("PASS actual extracted C: %lu clamp/envelope/IO/floor combinations; shared capacity, force/rate/cache, decay, cube root\n",count);
 return 0;
}
'''
(out/'extracted.c').write_text(code)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined,address','-g',str(out/'extracted.c'),'-o',str(out/'extracted')],check=True)
subprocess.run([str(out/'extracted')],check=True)
# Four notification configurations compile without any walt_disabled reference in !WALT.
gate=fn(sh,'cpufreq_update_util')
for walt in (0,1):
 for disabled in (0,1):
  c='''#include <stdbool.h>\n#include <assert.h>\ntypedef unsigned long long u64;\nstruct rq {};\nstruct update_util_data {void (*func)(struct update_util_data*,u64,unsigned);};\n#define SCHED_CPUFREQ_WALT 16\n#define SCHED_CPUFREQ_UCLAMP 256\nstatic unsigned called;\nstatic void cb(struct update_util_data*d,u64 c,unsigned f){(void)d;(void)c;(void)f;called++;}\nstatic struct update_util_data hook={cb},*ptr=&hook;\n#define per_cpu_ptr(p,c) ((void)(c),&ptr)\n#define rcu_dereference_sched(x) (x)\n#define cpu_of(rq) ((void)(rq),0)\n#define rq_clock(rq) ((void)(rq),1)\n#define sched_ktime_clock() 2\n'''
  if walt:c+=f'#define CONFIG_SCHED_WALT 1\nstatic bool walt_disabled={disabled};\nstatic bool sysctl_sched_use_walt_cpu_util=true;\n'
  c+=gate+'\nint main(void){struct rq r;cpufreq_update_util(&r,256);assert(called==1);cpufreq_update_util(&r,0);assert(called=='+str(1 if walt and not disabled else 2)+');'
  if walt:c+='sysctl_sched_use_walt_cpu_util=false;cpufreq_update_util(&r,0);assert(called=='+str(2 if not disabled else 3)+');'
  c+='return 0;}\n';f=out/f'gate-{walt}-{disabled}.c';f.write_text(c)
  subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(f),'-o',str(f.with_suffix(''))],check=True);subprocess.run([str(f.with_suffix(''))],check=True)
print('PASS actual gate C: WALT on/off, runtime disabled and PELT selected, UCLAMP pass-through')
# Source invariants, not runtime lifecycle tests.
stop=fn(s,'rfx_stop');assert stop.index('cpufreq_remove_update_util_hook')<stop.index('synchronize_sched')<stop.index('irq_work_sync')<stop.index('kthread_cancel_work_sync')
start=fn(s,'rfx_start');assert start.index('memset')<start.index('cpufreq_add_update_util_hook')
init=fn(s,'rfx_init');assert 'kobject_put(&tunables->attr_set.kobj)' in init and 'policy->governor_data = NULL' in init and 'cpufreq_disable_fast_switch(policy)' in init
assert 'MAX_USER_RT_PRIO / 2' in fn(s,'rfx_kthread_create')
assert 'linux/energy_model.h' not in s
k=(w/'drivers/cpufreq/Kconfig').read_text()
reflex=k[k.index('config CPU_FREQ_GOV_REFLEX'):k.index('if CPU_FREQ_GOV_SCHEDUTIL')]
assert '\tbool ' in reflex and '\tdefault n' in reflex
assert 'config CPU_FREQ_DEFAULT_GOV_REFLEX\n' in k
baseline=subprocess.run(['git','-C',str(w),'show','97c1beea4278:kernel/sched/cpufreq_schedutil.c'],capture_output=True,text=True)
if baseline.returncode == 0:
 assert h.split('\n#ifdef CONFIG_CPU_FREQ_GOV_REFLEX')[0].replace('#include <linux/math64.h>\n#include <linux/sched/cpufreq.h>\n','').rstrip()==baseline.stdout.rstrip()
 basek=subprocess.check_output(['git','-C',str(w),'show','97c1beea4278:drivers/cpufreq/Kconfig'],text=True)
 added=k[k.index('config CPU_FREQ_DEFAULT_GOV_REFLEX'):k.index('endchoice')]
 assert k[:k.index('endchoice')].replace(added,'')==basek[:basek.index('endchoice')]
 print('PASS historical schedutil body and existing default choices unchanged')
else:
 print('SKIP historical baseline comparison: base object unavailable (exported/shallow tree)')
print('PASS static lifecycle ordering/init rollback/FIFO50; bool/default n/Reflex default choice')
subprocess.run(['python3','-I',str(Path(__file__).with_name('default_tests.py')),
                '--src',str(w),'--out',str(out)],check=True)
