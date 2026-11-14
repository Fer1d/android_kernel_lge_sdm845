#!/usr/bin/env python3
"""Extract the actual kernel C implementation and run host sanitizer tests."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[4])
parser.add_argument('--cc', default='gcc')
args = parser.parse_args()
root = args.root.resolve()
kconfig = (root / 'init/Kconfig').read_text()
for name, parent in [('SCHED_BORE_DEFAULT_ON', 'SCHED_BORE'),
                     ('SCHED_POC_DEFAULT_ACTIVE', 'SCHED_POC_SELECTOR')]:
    entry = kconfig.split('config ' + name + '\n', 1)[1].split('\nconfig ', 1)[0]
    assert '\tdepends on ' + parent + '\n' in entry
    assert '\tdefault n\n' in entry
source = (root / 'kernel/sched/bore.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
header = (root / 'include/linux/sched.h').read_text()
ctx = header[header.index('struct bore_ctx {'):]
ctx = ctx[:ctx.index('};') + 2]
shim = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
#define U64_MAX UINT64_MAX
#define IS_ENABLED(c) (c)
#define __ro_after_init
#define __read_mostly
#define __init
#define __setup(name, fn) static int (*setup_fn)(char *) __attribute__((unused)) = fn
#define READ_ONCE(x) (x)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define min_t(t,a,b) min((t)(a), (t)(b))
#define max_t(t,a,b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
static int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static u64 div64_u64(u64 a, u64 b) { return a / b; }
static int kstrtouint(char *s, unsigned int base, unsigned int *v)
{
    char *end;
    unsigned long n = strtoul(s, &end, base);
    if (!*s || *end || n > UINT32_MAX) return -1;
    *v = n;
    return 0;
}
'''
tests = r'''
static u64 rng = 0xe79065c41a09ULL;
static u64 next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
int main(void)
{
    struct task_struct p = {0}, child = {0}, before;
    static const struct { u64 ns; u16 penalty; } golden[] = {
        {0,0}, {1,0}, {(1ULL<<23)-1,0}, {1ULL<<23,192},
        {1ULL<<24,576}, {1ULL<<25,960}, {UINT64_MAX,10239}
    };
    assert(sysctl_sched_bore == 0);
    assert(!bore_setup("2") && !bore_setup("garbage"));
    for (unsigned int i = 0; i < sizeof(golden)/sizeof(golden[0]); i++)
        assert(bore_penalty(golden[i].ns) == golden[i].penalty);
    puts("ok golden: source Q8 interpolation and cap");
    for (unsigned int i = 0; i < 200000; i++) {
        u64 d = next();
        unsigned int penalty = i % 40;
        p.bore.penalty = penalty << 8;
        __uint128_t ref = (__uint128_t)d * (40 + penalty) / 40;
        u64 expected = ref > U64_MAX ? U64_MAX : (u64)ref;
        assert(bore_vruntime_delta(&p, d) == expected);
        assert(bore_vruntime_delta(&p, U64_MAX) == U64_MAX);
        assert(bore_vruntime_delta(&p, 0) == 0);
    }
    puts("ok edge: 200000 quotient/remainder saturation comparisons");
    for (unsigned int s = 0; s <= 16; s++) {
        sysctl_sched_burst_smoothness = s;
        for (unsigned int i = 0; i < 1000; i++) {
            u16 a = next() % 10240, b = next() % 10240;
            assert(bore_smooth(a,b) == ((u64)a * ((1U<<s)-1) + b) / (1U<<s));
        }
    }
    sysctl_sched_burst_smoothness = 1;
    assert(bore_setup("1"));
    memset(&p, 0, sizeof(p));
    assert(bore_account_delta(&p, 1ULL<<25, 1000) == 1075);
    assert(p.bore.curr_penalty == 960 && p.bore.penalty == 960);
    bore_sleep(&p);
    assert(p.bore.prev_penalty == 480 && p.bore.penalty == 480);
    assert(!p.bore.burst_time && !p.bore.curr_penalty);
    p.bore.burst_time = U64_MAX - 3;
    bore_account_delta(&p, 10, 1);
    assert(p.bore.burst_time == U64_MAX);
    bore_account_delta(&p, U64_MAX, 1);
    assert(p.bore.burst_time == U64_MAX);
    puts("ok normal/sleep/smoothing/burst overflow");
    child = p;
    bore_fork(&child, &p);
    assert(!child.bore.burst_time && !child.bore.curr_penalty);
    assert(child.bore.penalty == p.bore.penalty);
    sysctl_sched_burst_inherit_type = 0;
    bore_fork(&child, &p);
    assert(!child.bore.penalty && !child.bore.prev_penalty);
    puts("ok fork: only 0/1 inheritance, child burst reset");
    /* Unit-only toggle; production flag is immutable after boot. */
    assert(bore_setup("0"));
    before = p;
    for (unsigned int i = 0; i < 10000; i++) {
        u64 d = next();
        assert(bore_account_delta(&p, next(), d) == d);
        bore_sleep(&p);
        assert(!memcmp(&before, &p, sizeof(p)));
    }
    child = p;
    sysctl_sched_burst_inherit_type = 1;
    bore_fork(&child, &p);
    assert(!child.bore.penalty && !child.bore.burst_time);
    puts("ok off: identity accounting, no state mutation, no inherited state");
    sysctl_sched_burst_penalty_offset = 0;
    sysctl_sched_burst_penalty_scale = 65535;
    assert(bore_penalty(U64_MAX) == 10239);
    sysctl_sched_burst_penalty_offset = 255;
    assert(!bore_penalty(U64_MAX));
    puts("PASS actual extracted C (normal/edge/golden/fork/off)");
    return 0;
}
'''
# Check integration outside schedstats/debug, and original real-runtime paths.
fair = (root / 'kernel/sched/fair.c').read_text()
start = fair.index('\ndequeue_entity(struct cfs_rq')
end = fair.index('update_stats_dequeue(cfs_rq, se, flags)', start)
hook = fair[start:end]
assert hook.index('update_curr(cfs_rq)') < hook.index('bore_sleep(task_of(se))')
assert '(flags & DEQUEUE_SLEEP) && entity_is_task(se)' in hook
assert 'schedstat' not in hook.replace('schedstats', '')
for line in ['cpuacct_charge(curtask, delta_exec);', 'account_group_exec_runtime(curtask, delta_exec);', 'account_cfs_rq_runtime(cfs_rq, delta_exec);', 'curr->sum_exec_runtime += delta_exec;']:
    assert line in fair
sysctl = (root / 'kernel/sysctl.c').read_text()
# Preprocessor condition stack: constants/table must not require SCHED_DEBUG.
stack = []
for line in sysctl.splitlines():
    directive = line.lstrip().split()
    if directive and directive[0] in ('#if', '#ifdef', '#ifndef'):
        stack.append(line)
    elif directive and directive[0] == '#endif':
        stack.pop()
    if 'static int bore_max_' in line or '.procname = "sched_bore"' in line:
        assert not any('SCHED_DEBUG' in cond for cond in stack)
assert '.mode = 0444' in sysctl[sysctl.index('.procname = "sched_bore"'):][:200]
assert '__ro_after_init' in source and '__setup("sched_bore=", bore_setup)' in source
print('ok static integration: sleep ordering, real runtime, DEBUG=n constants, boot-fixed flag', flush=True)

# Compile the complete shared initializer, not a Python model of its reset.
core = (root / 'kernel/sched/core.c').read_text()
def c_function(text, signature):
    a = text.index(signature)
    b = text.index('{', a)
    level, i = 1, b + 1
    while level:
        if text[i] == '{': level += 1
        elif text[i] == '}': level -= 1
        i += 1
    return text[a:i] + '\n'
shared = c_function(core, 'static void __sched_fork(')
idle = c_function(core, 'void init_idle(')
fork = c_function(core, 'int sched_fork(')
assert '__sched_fork(0, idle);' in idle and 'bore_fork(' not in idle
assert fork.index('__sched_fork(clone_flags, p);') < fork.index('bore_fork(p, current);')
assert '#ifdef CONFIG_SCHED_BORE\n' in shared
lifecycle_shim = r'''
struct task_struct {
    struct bore_ctx bore;
    int on_rq;
    struct { int on_rq, group_node; u64 exec_start, sum_exec_runtime,
        prev_sum_exec_runtime, nr_migrations, vruntime; } se;
    u64 last_sleep_ts, last_cpu_selected_ts;
    struct { int rb_node; } dl;
    struct { int run_list, timeout, time_slice, on_rq, on_list; } rt;
};
#define INIT_LIST_HEAD(p) (*(p)=0)
#define RB_CLEAR_NODE(p) (*(p)=0)
#define init_dl_task_timer(p) ((void)(p))
#define __dl_clear_params(p) ((void)(p))
static int sched_rr_timeslice = 100;
'''
lifecycle_tests = r'''
static void clean(struct task_struct *p) {
    assert(!p->bore.burst_time && !p->bore.curr_penalty &&
           !p->bore.prev_penalty && !p->bore.penalty);
}
int main(int argc, char **argv) {
    struct task_struct parent, child, idle;
    unsigned int expected = CONFIG_SCHED_BORE_DEFAULT_ON;
    assert(sysctl_sched_bore == expected);
    assert(!bore_setup("2") && !bore_setup("garbage"));
    assert(sysctl_sched_bore == expected);
    if (argc > 1) { assert(bore_setup(argv[1])); expected = atoi(argv[1]); }
    assert(sysctl_sched_bore == expected);
    memset(&parent, 0xa5, sizeof(parent)); parent.bore.penalty = 960;
    idle = parent;
    __sched_fork(0, &idle); clean(&idle);
    child = parent;
    __sched_fork(0, &child); clean(&child);
    bore_fork(&child, &parent);
    assert(!child.bore.burst_time && !child.bore.curr_penalty);
    assert(child.bore.prev_penalty == (expected ? 960 : 0));
    assert(child.bore.penalty == child.bore.prev_penalty);
    assert(parent.bore.penalty == 960);
    sysctl_sched_burst_inherit_type = 0;
    child = parent; __sched_fork(0, &child); bore_fork(&child, &parent);
    clean(&child);
    puts("PASS actual C: config default/cmdline override/shared idle reset/fork inheritance");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='bore-boot-test-') as temp:
    temp = Path(temp)
    for default in (0, 1):
        c = temp / ('boot%d.c' % default)
        binary = temp / ('boot%d' % default)
        c.write_text('#define CONFIG_SCHED_BORE 1\n#define CONFIG_SCHED_BORE_DEFAULT_ON %d\n' % default +
                     shim + ctx + lifecycle_shim + source + shared + lifecycle_tests)
        subprocess.run([args.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', str(c), '-o', str(binary)], check=True)
        for cmdline in ([], ['0'], ['1']):
            subprocess.run([str(binary), *cmdline], check=True)
with tempfile.TemporaryDirectory(prefix='bore-selftest-') as temp:
    temp = Path(temp)
    c = temp / 'extracted.c'
    c.write_text('#define CONFIG_SCHED_BORE_DEFAULT_ON 0\n' + shim + ctx + '\nstruct task_struct { struct bore_ctx bore; };\n' + source + tests)
    for name, flags in [('normal', []), ('asan-ubsan', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'])]:
        binary = temp / name
        subprocess.run([args.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *flags, str(c), '-o', str(binary)], check=True)
        print('RUN ' + name, flush=True)
        subprocess.run([str(binary)], check=True)
