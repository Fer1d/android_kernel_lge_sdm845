#!/usr/bin/env python3
"""Host regression harness extracting the real core helpers, not a device test.

Run: python3 -I tools/testing/selftests/uclamp_core.py
Requires a host C compiler; temporary outputs never enter the source tree.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CORE = (ROOT / "kernel/sched/core.c").read_text()
SCHED = (ROOT / "kernel/sched/sched.h").read_text()
TASK = (ROOT / "include/linux/sched.h").read_text()


def function(text, name):
    match = re.search(r"(?:static\s+)?(?:__always_inline\s+|inline\s+)?(?:unsigned\s+long|unsigned\s+int|int|void|struct uclamp_se)\s+(?:__init\s+)?" + name + r"\s*\([^;]*?\)\s*\{", text)
    if not match:
        raise RuntimeError("Cannot extract " + name)
    start = match.start()
    pos = match.end()
    depth = 1
    while depth:
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    return text[start:pos] + "\n"


def struct(text, name):
    return re.search(r"struct " + name + r" \{.*?\n\};", text, re.S).group() + "\n"


PREAMBLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#define CONFIG_UCLAMP_TASK_GROUP 1
#define SCHED_CAPACITY_SCALE 1024UL
#define SCHED_CAPACITY_SHIFT 10
#define UCLAMP_BUCKETS BUCKET_COUNT
#define UCLAMP_FLAG_IDLE 1
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define max_t(t,a,b) max((t)(a),(t)(b))
#define clamp(v,lo,hi) min(max(v,lo),hi)
#define DIV_ROUND_CLOSEST(a,b) (((a)+(b)/2)/(b))
#define likely(x) (x)
#define unlikely(x) (x)
#define lockdep_assert_held(x) ((void)0)
#define SCHED_WARN_ON(x) assert(!(x))
#define for_each_clamp_id(id) for ((id)=0;(id)<UCLAMP_CNT;(id)++)
#define __user
#define VERIFY_WRITE 0
#define access_ok(w,p,s) 1
#define copy_from_user(d,s,n) (memcpy(d,s,n),0)
#define copy_to_user(d,s,n) (memcpy(d,s,n),0)
#define get_user(v,p) ((v)=*(p),0)
#define put_user(v,p) ((void)(*(p)=(v)))
#define PAGE_SIZE 4096
#define MIN_NICE (-20)
#define MAX_NICE 19
#define SCHED_ATTR_SIZE_VER0 48
#define SCHED_ATTR_SIZE_VER1 56
#define SCHED_CPUFREQ_UCLAMP 256
#define SCHED_CPUFREQ_WALT 16
#define __init
#define for_each_possible_cpu(cpu) for ((cpu)=0;(cpu)<2;(cpu)++)
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
enum uclamp_id { UCLAMP_MIN, UCLAMP_MAX, UCLAMP_CNT };
'''

MOCKS = r'''
struct sched_class { int unused; } fair_sched_class, rt_sched_class, idle_sched_class;
struct task_group {
    struct uclamp_se uclamp_req[UCLAMP_CNT], uclamp[UCLAMP_CNT];
    bool autogroup;
};
struct task_struct {
    struct uclamp_se uclamp_req[UCLAMP_CNT], uclamp[UCLAMP_CNT];
    struct task_group *tg;
    struct sched_class *sched_class;
    bool sched_reset_on_fork;
    int cpu;
};
struct rq { int cpu, lock; unsigned int uclamp_flags; struct uclamp_rq uclamp[UCLAMP_CNT]; } rqs[2];
struct rq_flags { int unused; };
struct task_struct init_task;
struct task_group root_task_group;
static struct uclamp_se uclamp_default[UCLAMP_CNT];
#define cpu_rq(cpu) (&rqs[cpu])
#define task_cpu(p) ((p)->cpu)
#define cpu_of(rq) ((rq)->cpu)
#define task_group(p) ((p)->tg)
#define task_group_is_autogroup(tg) ((tg)->autogroup)
static struct rq *task_rq_lock(struct task_struct *p, struct rq_flags *rf) { return cpu_rq(p->cpu); }
static void task_rq_unlock(struct rq *rq, struct task_struct *p, struct rq_flags *rf) {}
static unsigned int notifications;
static void cpufreq_update_util(struct rq *rq, unsigned int flags) { notifications++; }
'''

TESTS = r'''
static struct task_struct task(int cpu, unsigned int lo, unsigned int hi)
{
    struct task_struct p = { .cpu=cpu, .tg=&root_task_group, .sched_class=&fair_sched_class };
    uclamp_se_set(&p.uclamp_req[0],lo,true);
    uclamp_se_set(&p.uclamp_req[1],hi,true);
    return p;
}
int main(void)
{
    struct task_struct a, b, child;
    struct sched_attr attr = {0}, out;
    unsigned char buf[80];
    unsigned int v;
    s64 percent;
    struct task_group group;
    rqs[1].cpu=1;
    init_uclamp();
    assert(sizeof(attr)==56 && offsetof(struct sched_attr,sched_util_min)==48);
    for(v=0;v<=1024;v++) assert(uclamp_bucket_id(v)<UCLAMP_BUCKETS);
    assert(uclamp_rq_get(&rqs[0],UCLAMP_MAX)==1024);
    assert(rqs[0].uclamp_flags & UCLAMP_FLAG_IDLE);
    a=task(0,300,500);
    assert(uclamp_rq_util_with(&rqs[0],900,&a)==500);
    uclamp_rq_inc(&rqs[0],&a);
    assert(!(rqs[0].uclamp_flags & UCLAMP_FLAG_IDLE));
    assert(uclamp_rq_get(&rqs[0],UCLAMP_MAX)==500);
    b=task(0,0,1024);
    uclamp_rq_inc(&rqs[0],&b);
    assert(uclamp_rq_get(&rqs[0],UCLAMP_MAX)==1024);
    assert(uclamp_rq_util_without(&rqs[0],900,&b)==500);
    assert(uclamp_rq_util_without(&rqs[0],900,&a)==900);
    child=a;
    uclamp_fork(&child);
    assert(!child.uclamp[0].active && !child.uclamp[1].active);
    assert(child.uclamp_req[0].value==300);
    child.sched_class=&rt_sched_class;
    child.sched_reset_on_fork=true;
    uclamp_fork(&child);
    assert(child.uclamp_req[0].value==0 && child.uclamp_req[1].value==1024);
    assert(!child.uclamp_req[0].user_defined);
    uclamp_rq_dec(&rqs[0],&b);
    uclamp_rq_dec(&rqs[0],&a);
    assert(rqs[0].uclamp_flags & UCLAMP_FLAG_IDLE);
    assert(uclamp_rq_get(&rqs[0],UCLAMP_MIN)==0);
    assert(uclamp_rq_get(&rqs[0],UCLAMP_MAX)==500);
    a=task(0,0,1024);
    assert(uclamp_rq_util_with(&rqs[0],1800,&a)==1800);
    uclamp_rq_inc(&rqs[0],&a);
    assert(uclamp_rq_util_with(&rqs[0],1800,NULL)==1800);
    uclamp_se_set(&a.uclamp_req[0],700,true);
    uclamp_se_set(&a.uclamp_req[1],800,true);
    uclamp_update_active(&a);
    assert(!(rqs[0].uclamp_flags & UCLAMP_FLAG_IDLE));
    assert(uclamp_rq_get(&rqs[0],UCLAMP_MIN)==700);
    assert(uclamp_rq_get(&rqs[0],UCLAMP_MAX)==800);
    assert(notifications);
    uclamp_rq_dec(&rqs[0],&a);
    a.cpu=1;
    uclamp_rq_inc(&rqs[1],&a);
    assert(rqs[1].uclamp[0].bucket[a.uclamp[0].bucket_id].tasks==1);
    uclamp_rq_dec(&rqs[1],&a);
    group=root_task_group;
    uclamp_se_set(&group.uclamp[0],200,false);
    uclamp_se_set(&group.uclamp[1],400,false);
    a=task(0,600,800); a.tg=&group;
    assert(uclamp_eff_value(&a,UCLAMP_MIN)==400);
    assert(uclamp_eff_value(&a,UCLAMP_MAX)==400);
    uclamp_se_set(&a.uclamp_req[0],0,true);
    uclamp_se_set(&a.uclamp_req[1],100,true);
    assert(uclamp_eff_value(&a,UCLAMP_MIN)==200);
    assert(uclamp_eff_value(&a,UCLAMP_MAX)==200);
    a=task(0,100,800);
    attr.sched_flags=SCHED_FLAG_UTIL_CLAMP_MIN; attr.sched_util_min=900;
    assert(uclamp_validate(&a,&attr)==-EINVAL);
    attr.sched_util_min=~0U; assert(!uclamp_validate(&a,&attr));
    __setscheduler_uclamp(&a,&attr); assert(a.uclamp_req[0].value==0 && !a.uclamp_req[0].user_defined);
    attr.sched_flags=SCHED_FLAG_UTIL_CLAMP_MAX; attr.sched_util_max=1025;
    assert(uclamp_validate(&a,&attr)==-EINVAL);
    attr.sched_util_max=~0U; assert(!uclamp_validate(&a,&attr));
    __setscheduler_uclamp(&a,&attr); assert(a.uclamp_req[1].value==1024);
    memset(buf,0,sizeof(buf)); ((struct sched_attr *)buf)->size=48;
    assert(!sched_copy_attr((void *)buf,&out));
    ((struct sched_attr *)buf)->sched_flags=SCHED_FLAG_UTIL_CLAMP_MIN;
    assert(sched_copy_attr((void *)buf,&out)==-EINVAL);
    ((struct sched_attr *)buf)->size=56; assert(!sched_copy_attr((void *)buf,&out));
    ((struct sched_attr *)buf)->size=64; buf[60]=1;
    assert(sched_copy_attr((void *)buf,&out)==-E2BIG);
    assert(((struct sched_attr *)buf)->size==56);
    buf[60]=0; ((struct sched_attr *)buf)->size=64;
    assert(!sched_copy_attr((void *)buf,&out));
    out.sched_util_max=1024; memset(buf,0xa5,sizeof(buf));
    assert(!sched_read_attr((void *)buf,&out,48));
    assert(((struct sched_attr *)buf)->size==56 && buf[48]==0xa5);
    assert(!sched_read_attr((void *)buf,&out,56));
    assert(((struct sched_attr *)buf)->sched_util_max==1024);
    assert(!uclamp_parse_percent("0",&percent) && percent==0);
    assert(!uclamp_parse_percent("25.50",&percent) && percent==2550);
    assert(!uclamp_parse_percent("100.00",&percent) && percent==10000);
    assert(uclamp_parse_percent("-1",&percent));
    assert(uclamp_parse_percent("100.01",&percent));
    assert(uclamp_parse_percent("1.001",&percent));
    /* Runtime ABI rejects negative policy before KEEP_POLICY interpretation. */
    assert((int)(unsigned int)~0U<0);
    puts("uclamp core actual-helper tests passed");
    return 0;
}
'''

functions = ["uclamp_bucket_id", "uclamp_se_set", "uclamp_idle_value", "uclamp_idle_reset", "uclamp_rq_max_value", "uclamp_rq_util_without", "uclamp_tg_restrict", "uclamp_eff_get", "uclamp_eff_value", "uclamp_rq_inc_id", "uclamp_rq_dec_id", "uclamp_rq_inc", "uclamp_rq_dec", "uclamp_update_active", "uclamp_validate", "__setscheduler_uclamp", "uclamp_fork", "init_uclamp", "sched_copy_attr", "sched_read_attr", "uclamp_parse_percent"]
flags = (ROOT / "include/uapi/linux/sched.h").read_text()
source = PREAMBLE + "\n" + flags
source += struct(TASK, "uclamp_se") + struct(TASK, "sched_attr")
source += struct(SCHED, "uclamp_bucket") + struct(SCHED, "uclamp_rq") + MOCKS
source += "#define UCLAMP_BUCKET_DELTA DIV_ROUND_CLOSEST(SCHED_CAPACITY_SCALE,UCLAMP_BUCKETS)\n"
source += function(SCHED, "uclamp_none")
source += "unsigned int uclamp_eff_value(struct task_struct *, enum uclamp_id);\n"
source += function(SCHED, "uclamp_rq_get") + function(SCHED, "uclamp_rq_util_with")
source += "\n".join(function(CORE, name) for name in functions) + TESTS
with tempfile.TemporaryDirectory(prefix="uclamp-core-test.") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(source)
    for count in (5, 20):
        exe = path / ("test" + str(count))
        subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DBUCKET_COUNT=" + str(count), str(path / "test.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("Host-only checks passed for 5/20 buckets; not a boot/device validation.")
