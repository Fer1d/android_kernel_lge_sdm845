#!/usr/bin/env python3
"""Compile actual POC C with deterministic kernel mocks; never build the kernel.
Run: python3 -I tools/testing/selftests/poc/run.py [source-root] [baseline-git-root]
Stats ABI: legacy attempt/candidate/selected, EAS attempt/candidate/selected,
rejected, policy fallback, no candidate. mode: 0 off, 1 shadow, 2 active.
Tests model lifecycle ordering, not the Linux memory model or an SMP stress run.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[4]
source = (root / 'kernel/sched/poc_selector.c').read_text()
source = source[:source.index('static int poc_mode_sysctl')]
source = '\n'.join(l for l in source.splitlines() if not l.startswith('#include'))
shim = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <stddef.h>
typedef uint64_t u64;
typedef struct { u64 v; } atomic64_t;
#define ATOMIC64_INIT(x) {x}
#define DEFINE_STATIC_KEY_FALSE(n) int n
#define DEFINE_PER_CPU(t,n) t n
#define DEFINE_MUTEX(n) int n
#define hweight64(x) __builtin_popcountll(x)
#define atomic64_inc(p) (++(p)->v)
#define atomic64_read(p) ((p)->v)
#define atomic64_set(p,x) ((p)->v=(x))
#define atomic64_or(x,p) ((p)->v|=(x))
#define atomic64_andnot(x,p) ((p)->v&=~(x))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define static_branch_unlikely(p) (*(p))
#define static_branch_disable(p) (*(p)=0)
#define static_branch_enable(p) (*(p)=1)
#define synchronize_sched() (++graces)
#define __this_cpu_inc_return(n) (++n)
#define IS_ENABLED(c) (c)
#define CONFIG_NUMA_BALANCING 0
#define CONFIG_SCHED_SMT 0
#define CONFIG_SCHED_WALT 1
#define CONFIG_UCLAMP_TASK 1
#define TASK_WAKING 2
#define SCHED_BOOST_NONE 0
#define PF_WAKE_UP_IDLE 1
#define SCHED_CAPACITY_SCALE 1024
#define UCLAMP_MIN 0
#define UCLAMP_MAX 1
#define EAS_USE_NEED_IDLE 0
int nr_cpu_ids=8, graces, local_cpu;
struct cpumask { u64 bits; };
struct task_struct { int state, flags, rtg, boost, tune, prefer, clamp; struct cpumask allowed; } task, *current=&task;
struct rq { unsigned long clamp[2]; } rqs[64];
bool online[64], active[64], isolated[64], irq[64], reserved[64], fits[64], idle[64];
unsigned long caps[64]; int nodes[64];
static int checked(int cpu) { assert(cpu>=0 && cpu<nr_cpu_ids && cpu<64); return cpu; }
#define cpu_online(c) online[checked(c)]
#define cpu_active(c) active[checked(c)]
#define cpu_isolated(c) isolated[checked(c)]
#define cpu_to_node(c) nodes[checked(c)]
#define capacity_orig_of(c) caps[checked(c)]
#define walt_cpu_high_irqload(c) irq[checked(c)]
#define is_reserved(c) reserved[checked(c)]
#define boosted_task_util(p) 100
#define task_fits_max(p,c) fits[checked(c)]
#define idle_cpu(c) idle[checked(c)]
#define cpu_rq(c) (&rqs[checked(c)])
#define uclamp_rq_get(r,k) ((r)->clamp[k])
#define tsk_cpus_allowed(p) (&(p)->allowed)
#define cpumask_bits(p) (&(p)->bits)
static bool cpumask_test_cpu(int c,const struct cpumask *m) { checked(c); return m->bits & (1ULL<<c); }
#define task_related_thread_group(p) ((p)->rtg ? (void *)1 : NULL)
#define task_boost_policy(p) ((p)->boost)
#define schedtune_task_boost(p) ((p)->tune)
#define schedtune_prefer_idle(p) ((p)->prefer)
#define task_uclamp_active(p) ((p)->clamp)
int feature;
#define sched_feat(x) feature
#define smp_processor_id() local_cpu
static void on_each_cpu(void (*fn)(void *),void *arg,int wait) { int c; (void)wait; for(c=0;c<nr_cpu_ids;c++) if(online[c]) { local_cpu=c; fn(arg); } }
struct sched_domain { struct cpumask span; } domain;
struct sched_domain *sd_llc=&domain;
#define per_cpu(n,c) (checked(c),n)
#define rcu_dereference(p) (p)
#define sched_domain_span(sd) (&(sd)->span)
int sysctl_sched_cstate_aware;
'''
tests = r'''
static void reset(void) {
 int c; task=(struct task_struct){.state=TASK_WAKING,.allowed={255}};
 domain.span.bits=255; feature=0; sysctl_sched_cstate_aware=0;
 for(c=0;c<64;c++) { online[c]=active[c]=fits[c]=idle[c]=true;
 isolated[c]=irq[c]=reserved[c]=false; caps[c]=512; nodes[c]=0;
 rqs[c].clamp[0]=0; rqs[c].clamp[1]=1024; }
}
static int old_pick(u64 mask, unsigned int counter) {
 unsigned int rot=counter & 63;
 if(!mask) return -1;
 u64 rotated=rot ? (mask>>rot)|(mask<<(64-rot)) : mask;
 return (__builtin_ctzll(rotated)+rot)&63;
}
static void distributions(void) {
 u64 masks[]={255,240,0x8000000000000081ULL,0xaaaaaaaaaaaaaaaaULL,~0ULL};
 unsigned int a,k,round; int counts[64],old[64]={0};
 for(a=0;a<sizeof(masks)/sizeof(masks[0]);a++) {
  unsigned int n=hweight64(masks[a]);
  for(round=0;round<4;round++) {
   for(k=0;k<64;k++) counts[k]=0;
   for(k=0;k<n;k++) counts[poc_pick(masks[a],round*n+k)]++;
   for(k=0;k<64;k++) assert(counts[k]==(int)((masks[a]>>k)&1));
  }
 }
 for(k=0;k<64;k++) old[old_pick(255,k)]++;
 assert(old[0]==57); for(k=1;k<8;k++) assert(old[k]==1);
 puts("PASS RR full-period contiguous/sparse/high-bit masks; old negative control 57:1");
}
int main(void) {
 int i,j; struct cpumask scope={255};
 distributions();
 reset(); assert(poc_pick(0,0)==-1); assert(poc_cpu_bit(-1)==0);
 assert(poc_cpu_bit(64)==0); assert(poc_cpu_bit(8)==0);
 for(i=0;i<64;i++) for(j=0;j<128;j++) assert(poc_pick(1ULL<<i,j)==i);
 assert(!poc_candidate_valid(&task,-1,0,&scope));
 assert(!poc_candidate_valid(&task,64,0,&scope));
 assert(!poc_candidate_valid(&task,0,64,&scope));
 assert(poc_candidate_valid(&task,1,0,&scope));
#define REJECT(field,value) field[1]=value; assert(!poc_candidate_valid(&task,1,0,&scope)); reset()
 REJECT(online,false); REJECT(active,false); REJECT(isolated,true);
 REJECT(irq,true); REJECT(reserved,true); REJECT(fits,false);
 REJECT(idle,false); REJECT(caps,1024); REJECT(nodes,1);
 task.allowed.bits=1; assert(!poc_candidate_valid(&task,1,0,&scope)); reset();
 scope.bits=1; assert(!poc_candidate_valid(&task,1,0,&scope)); scope.bits=255;
 rqs[1].clamp[0]=100; assert(!poc_candidate_valid(&task,1,0,&scope)); reset();
 rqs[1].clamp[1]=500; assert(!poc_candidate_valid(&task,1,0,&scope)); reset();
#define POLICY(field,value) task.field=value; assert(!poc_policy_eligible(&task)); reset()
 POLICY(rtg,1); POLICY(boost,1); POLICY(tune,-1); POLICY(flags,1);
 POLICY(clamp,1); POLICY(state,0); feature=1; POLICY(prefer,1);
 assert(poc_candidate(&task,0,&scope,true)==-1); assert(!poc_stats[POC_EAS_ATTEMPTS].v);
 assert(!sched_poc_writer && !sched_poc_reader && !poc_mode);
 poc_set_mode(1); assert(sched_poc_writer && sched_poc_reader && poc_mode==1);
 assert(poc_idle_mask.v==255); assert(poc_candidate(&task,0,&scope,true)==-1);
 assert(poc_stats[POC_EAS_CANDIDATES].v==1); assert(poc_legacy(&task,0)==-1);
 poc_set_mode(2); assert(poc_candidate(&task,0,&scope,true)>=0);
 assert(poc_legacy(&task,0)>=0);
 /* Stale offline, outside-group, affinity, and invalid high-ID hints rejected. */
 poc_idle_mask.v=1ULL<<7; online[7]=false; assert(poc_candidate(&task,0,&scope,true)==-1);
 reset(); poc_idle_mask.v=1ULL<<7; scope.bits=15; assert(poc_candidate(&task,0,&scope,true)==-1);
 scope.bits=255; task.allowed.bits=15; assert(poc_candidate(&task,0,&scope,true)==-1);
 reset(); nr_cpu_ids=4; poc_idle_mask.v=1ULL<<7; assert(poc_candidate(&task,0,&scope,true)==-1);
 nr_cpu_ids=8; reset();
 /* Domain replacement uses the current scope, not cached relative geometry. */
 domain.span.bits=15; poc_idle_mask.v=240; assert(poc_legacy(&task,0)==-1);
 domain.span.bits=240; assert(poc_legacy(&task,4)>=4);
 sched_poc_idle_update(7,false); assert(!(poc_idle_mask.v & 128));
 sched_poc_idle_update(7,true); assert(poc_idle_mask.v & 128);
 poc_set_mode(0); assert(!sched_poc_reader && !sched_poc_writer && !poc_idle_mask.v);
 i=graces; poc_set_mode(0); assert(i==graces);
 assert(poc_candidate(&task,0,&scope,true)==-1);
 puts("PASS actual C: range/RR/policy/rejection/off/shadow/active/hotplug/domain/mode transitions");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='poc-c-test-') as temp:
    c = pathlib.Path(temp) / 'poc.c'
    exe = pathlib.Path(temp) / 'poc-test'
    c.write_text(shim + source + tests)
    subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-variable','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

# Assert disabled integration is original text after removing guarded additions.
# The scalar hint loop is separately compiled below using its actual source.
fair = (root / 'kernel/sched/fair.c').read_text()
start = fair.index('\t\tfor (; (i = poc_hint >= 0 ?')
end = fair.index('\n#else', start)
loop = fair[start:end]
reset_fn = tests[tests.index('static void reset'):tests.index('static int old_pick')]
recheck_start = fair.index('\t\t\tpoc_hint = -1;')
recheck_end = fair.index('\n#endif', recheck_start)
recheck = fair[recheck_start:recheck_end]
# Copy production rejection statements (not reimplementations) into the trial.
def statement_block(start_text, end_text):
    a = fair.index(start_text, fair.index('retry:'))
    return fair[a:fair.index(end_text, a)]
common_online = statement_block('\t\t\tif (!cpu_online(i) || cpu_isolated(i))', '\n\n')
common_irq = statement_block('\t\t\tif (walt_cpu_high_irqload(i) || is_reserved(i))', '\n\n')
overutil = statement_block('\t\t\tif (cpu_check_overutil_condition(i, max(raw_util, new_util)))', '\n\n')
idle_loss = statement_block('\t\t\tif (poc_trial && !idle_cpu(i))', '\n#endif')
loop_test = shim + source + reset_fn + r'''
#define max(a,b) ((a)>(b)?(a):(b))
int rejection_reason;
#define sched_group_cpus(sg) (&domain.span)
#define cpu_check_overutil_condition(i,u) (rejection_reason==4)
int main(void) {
 int hint,reason;
 for(hint=0;hint<8;hint++) for(reason=0;reason<6;reason++) {
 reset(); rejection_reason=reason;
 struct task_struct *p=&task; int cpu=0;
 int i=-1, poc_hint=hint, poc_resume=-1, nr=0, seen[8]={0}; bool poc_trial=false;
 unsigned long raw_util=0,new_util=0;
 struct cpumask search_cpus={255};
''' + loop + r'''
  if(poc_trial) {
   /* Exercise each production rejection phase independently. */
   if(reason==0) active[i]=false;
''' + recheck + r'''
   if(reason==1) online[i]=false;
   if(reason==2) irq[i]=true;
   if(reason==3) reserved[i]=true;
''' + common_online + '\n' + common_irq + '\n' + overutil + r'''
   if(reason==5) idle[i]=false;
''' + idle_loss + r'''
   assert(!"trial should have been rejected");
  }
  seen[i]++; nr++; search_cpus.bits &= ~(1ULL<<i);
 }
 assert(nr==8); for(i=0;i<8;i++) assert(seen[i]==1);
 }
 puts("PASS actual fair loop/rejection statements: revalidation/offline/IRQ/reserved/overutil/idle-loss restore scan");
 return 0;
}
'''
loop_test = loop_test.replace('struct sched_domain {', 'static int cpumask_next(int c,const struct cpumask *m) { for(++c;c<nr_cpu_ids;c++) if(m->bits & (1ULL<<c)) return c; return nr_cpu_ids; }\nstruct sched_domain {')
with tempfile.TemporaryDirectory(prefix='poc-loop-test-') as temp:
    c = pathlib.Path(temp) / 'loop.c'; exe = pathlib.Path(temp) / 'loop'
    c.write_text(loop_test)
    subprocess.run(['cc','-std=gnu99','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

# Extract only POC conditionals, without expanding unrelated kernel headers.
def without_poc(text):
    lines = text.splitlines(keepends=True)
    out = []; depth = 0; keep = True
    for line in lines:
        if not depth and line.strip() == '#ifdef CONFIG_SCHED_POC_SELECTOR':
            depth = 1; keep = False; continue
        if depth:
            if line.lstrip().startswith(('#if ', '#ifdef ', '#ifndef ')):
                depth += 1
            elif line.strip() == '#endif':
                depth -= 1
                if not depth:
                    keep = True; continue
            elif line.strip() == '#else' and depth == 1:
                keep = True; continue
            if keep:
                out.append(line)
        else:
            out.append(line)
    assert depth == 0
    return ''.join(out)

baseline_root = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else root
baseline = subprocess.check_output(['git', '-C', str(baseline_root),
                                   'show', '0943f923a568:kernel/sched/fair.c'], text=True)
# Ignore formatting-only differences in the split common conditions.
assert ''.join(without_poc(fair).split()) == ''.join(baseline.split())
print('PASS config=n fair integration: original 0943 token stream')

# Actual Case A.1 block: proves restricted active increments EAS_SELECTED,
# not just shadow candidate counts. Keep its production return/condition.
start = fair.index('\t\t\t\tif (idle_cpu(i)) {', fair.index('Case A.1: IDLE CPU'))
end = fair.index('\n\t\t\t\t}', start) + len('\n\t\t\t\t}')
case = fair[start:end]
prelude = shim + source + r'''
#define schedstat_inc(x) ((void)0)
#define trace_sched_find_best_target(...) ((void)0)
static int eas_case(int i, bool poc_trial) {
'''
ending = r'''
 return -1;
}
int main(void) {
 reset(); poc_set_mode(1);
 assert(poc_candidate(&task,0,&domain.span,true)==-1);
 assert(!poc_stats[POC_EAS_SELECTED].v);
 poc_set_mode(2);
 int c=poc_candidate(&task,0,&domain.span,true); assert(c>=0);
 assert(eas_case(c,true)==c); assert(poc_stats[POC_EAS_SELECTED].v==1);
 idle[c]=false; assert(eas_case(c,true)==-1);
 assert(poc_stats[POC_EAS_SELECTED].v==1);
 puts("PASS actual fair Case A.1: active selects and EAS_SELECTED counts, shadow does not");
 return 0;
}
'''
reset_fn = tests[tests.index('static void reset'):tests.index('static int old_pick')]
with tempfile.TemporaryDirectory(prefix='poc-eas-test-') as temp:
    c = pathlib.Path(temp) / 'eas.c'; exe = pathlib.Path(temp) / 'eas'
    c.write_text('#define CONFIG_SCHED_POC_SELECTOR 1\n' + prelude + case +
                 ending[:ending.index('int main(void)')] + reset_fn +
                 ending[ending.index('int main(void)'):])
    subprocess.run(['cc','-std=gnu99','-fsanitize=undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

# Actual 4.9 parser and POC handlers/table/init. Kernel allocation/uaccess,
# locking, hotplug and register_sysctl are mocks, not a mounted /proc test.
def c_function(text, signature):
    a = text.index(signature); b = text.index('{', a); level = 1; i = b + 1
    while level:
        if text[i] == '{': level += 1
        elif text[i] == '}': level -= 1
        i += 1
    return text[a:i] + '\n'

sysctl = (baseline_root / 'kernel/sysctl.c').read_text()
parser = '#define TMPBUFLEN 22\n'
for name in ['static size_t proc_skip_spaces(', 'static int proc_get_long(',
             'static int proc_put_long(', 'static int proc_put_char(',
             'static int do_proc_dointvec_conv(']:
    parser += c_function(sysctl, name)
# Whitespace literals are copied from the production parser separator.
parser += "static const char proc_wspace_sep[] = { ' ', '\\t', '\\n' };\n"
for name in ['static int __do_proc_dointvec(', 'static int do_proc_dointvec(']:
    parser += c_function(sysctl, name)
a = sysctl.index('struct do_proc_dointvec_minmax_conv_param {')
b = sysctl.index('};', a) + 2
parser += sysctl[a:b] + '\n'
parser += c_function(sysctl, 'static int do_proc_dointvec_minmax_conv(')
parser += c_function(sysctl, 'int proc_dointvec_minmax(')
control_shim = r'''
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#define __user
#define __init
#define late_initcall(x)
#define PAGE_SIZE 4096
#define SYSCTL_WRITES_STRICT 1
#define SYSCTL_WRITES_WARN 0
int sysctl_writes_strict=1, locked, hotplug_locked, registration_fail;
typedef long long loff_t;
struct ctl_table {
 const char *procname; void *data; int maxlen,mode;
 int (*proc_handler)(struct ctl_table *,int,void *,size_t *,loff_t *);
 void *extra1,*extra2;
};
static void mutex_lock(int *p) { (void)p; assert(!locked); locked=1; }
static void mutex_unlock(int *p) { (void)p; assert(locked && !hotplug_locked); locked=0; }
static void get_online_cpus(void) { assert(locked && !hotplug_locked); hotplug_locked=1; }
static void put_online_cpus(void) { assert(hotplug_locked); hotplug_locked=0; }
static char *skip_spaces(char *s) { while(isspace((unsigned char)*s)) s++; return s; }
#define simple_strtoul(s,e,b) strtoul(s,e,b)
static void *memdup_user_nul(void *s,size_t n) { char *p=malloc(n+1); assert(p); memcpy(p,s,n); p[n]=0; return p; }
#define IS_ERR(p) false
#define PTR_ERR(p) (-ENOMEM)
#define kfree(p) free(p)
#define copy_to_user(d,s,n) (memcpy(d,s,n),0)
#define put_user(v,p) (*(p)=(v),0)
#define warn_sysctl_write(t) ((void)0)
static int proc_doulongvec_minmax(struct ctl_table *t,int w,void *b,size_t *n,loff_t *p) {
 (void)t;(void)w;(void)b;(void)n;(void)p; return 0;
}
static void *register_sysctl(const char *path,struct ctl_table *t) {
 int n=0; assert(!strcmp(path,"kernel"));
 while(t[n].procname) { assert(n<2 && t[n].proc_handler); n++; }
 assert(n==2 && t[0].mode==0644 && t[1].mode==0444);
 return registration_fail ? NULL : t;
}
'''
# libc defines loff_t on GNU hosts; use its native type for handler mocks.
control_shim = control_shim.replace('typedef long long loff_t;', '')
full_source = (root / 'kernel/sched/poc_selector.c').read_text()
controls = full_source[full_source.index('static int poc_mode_sysctl'):]
control_tests = r'''
static int write_mode(char *text,loff_t pos) {
 size_t n=strlen(text); int ret=poc_mode_sysctl(&poc_sysctls[0],1,text,&n,&pos);
 assert(!locked && !hotplug_locked); return ret;
}
int main(void) {
 char b[32]={0}; size_t n=sizeof(b); loff_t pos=0; reset();
 assert(poc_sysctl_init()==0); registration_fail=1;
 assert(poc_sysctl_init()==-ENOMEM); registration_fail=0;
 assert(write_mode("2\n",0)==0 && poc_mode==2);
 int g=graces; assert(write_mode("2",0)==0 && graces==g);
 assert(write_mode("3",0)==-EINVAL && poc_mode==2);
 assert(write_mode("-1",0)==-EINVAL && poc_mode==2);
 assert(write_mode("x",0)==-EINVAL && poc_mode==2);
 assert(write_mode("2147483648",0)==-EINVAL && poc_mode==2);
 assert(write_mode(" \t",0)==-EINVAL && poc_mode==2);
 assert(write_mode("0",1)==0 && poc_mode==2); /* strict nonzero offset */
 assert(write_mode(" 1\n",0)==0 && poc_mode==1);
 assert(poc_mode_sysctl(&poc_sysctls[0],0,b,&n,&pos)==0);
 assert(n==2 && !memcmp(b,"1\n",2));
 assert(poc_stats_sysctl(&poc_sysctls[1],1,b,&n,&pos)==-EPERM);
 assert(write_mode("0",0)==0 && !poc_mode && !sched_poc_reader && !sched_poc_writer);
 puts("PASS actual sysctl parser/POC controls: bounds/read/offset/idempotence/sentinel/registration failure");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='poc-control-test-') as temp:
    c = pathlib.Path(temp) / 'control.c'; exe = pathlib.Path(temp) / 'control'
    c.write_text(shim + control_shim + parser + source + controls + reset_fn + control_tests)
    subprocess.run(['cc','-std=gnu99','-fsanitize=undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('BOUNDARY: mocked uaccess/registration/locks; no real procfs, SMP concurrency or kernel build validation')
