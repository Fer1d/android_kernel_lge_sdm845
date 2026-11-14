# SPDX-License-Identifier: GPL-2.0
"""Actual Kconfig resolution and preprocessed C provider/initcall host checks."""
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess

setup = runpy.run_path(str(Path(__file__).with_name('test_support.py')))['setup']
root, out = setup('Default governor and elevator source regression tests')

# Build only the host Kconfig parser, never invoke the kernel build system.
ksrc = root / 'scripts/kconfig'
for name in ('zconf.tab', 'zconf.lex', 'zconf.hash'):
    shutil.copyfile(ksrc / (name + '.c_shipped'), out / (name + '.c'))
conf = out / 'conf'
subprocess.run(['cc', '-std=gnu99', '-D_GNU_SOURCE', '-I', str(ksrc),
                str(ksrc / 'conf.c'), str(out / 'zconf.tab.c'), '-o', str(conf)],
               check=True)
kfile = out / 'Kconfig'
kfile.write_text('mainmenu "Host default tests"\n' + ''.join(
    'config ' + name + '\n\tbool "' + name + '"\n'
    for name in ('MODULES', 'BLOCK', 'CPU_FREQ', 'SMP', 'THERMAL', 'BLK_CGROUP', 'IRQ_WORK', 'SRCU'))
    .replace('config CPU_FREQ\n\tbool "CPU_FREQ"\n', '') +
    '\nsource "' + str(root / 'block/Kconfig.iosched') + '"\n' +
    'source "' + str(root / 'drivers/cpufreq/Kconfig') + '"\n')

def resolve(name, lines):
    work = out / name
    work.mkdir()
    config = work / '.config'
    config.write_text('\n'.join(lines) + '\n')
    env = dict(os.environ, srctree=str(root), KCONFIG_CONFIG=str(config),
               KCONFIG_AUTOCONFIG=str(work / 'auto.conf'),
               KCONFIG_AUTOHEADER=str(work / 'autoconf.h'))
    result = subprocess.run([str(conf), '--olddefconfig', str(kfile)],
                            cwd=work, env=env, text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    assert 'warning:' not in result.stderr, result.stderr
    return dict(re.findall(r'^CONFIG_(\w+)=(.*)$', config.read_text(), re.M))

base = ['CONFIG_BLOCK=y', 'CONFIG_CPU_FREQ=y', 'CONFIG_SMP=y']
active = resolve('active', base + ['CONFIG_DEFAULT_ADIOS=y',
    'CONFIG_IOSCHED_ADIOS=y', 'CONFIG_IOSCHED_ANXIETY=y',
    'CONFIG_CPU_FREQ_DEFAULT_GOV_REFLEX=y',
    '# CONFIG_CPU_FREQ_GOV_SCHEDUTIL is not set',
    '# CONFIG_CPU_FREQ_GOV_PERFORMANCE is not set'])
for symbol in ('CPU_FREQ_DEFAULT_GOV_REFLEX', 'CPU_FREQ_GOV_REFLEX',
               'CPU_FREQ_GOV_SCHEDUTIL', 'CPU_FREQ_GOV_PERFORMANCE',
               'IOSCHED_ADIOS', 'IOSCHED_ANXIETY', 'IRQ_WORK',
               'CPU_FREQ_GOV_ATTR_SET'):
    assert active[symbol] == 'y', (symbol, active)
assert active['DEFAULT_IOSCHED'] == '"adios"'
assert sum(v == 'y' for k, v in active.items()
           if k.startswith('CPU_FREQ_DEFAULT_GOV_')) == 1
for name, lines in (
    ('no-smp', ['CONFIG_CPU_FREQ=y', 'CONFIG_CPU_FREQ_DEFAULT_GOV_REFLEX=y']),
    ('no-cpufreq', ['CONFIG_SMP=y', 'CONFIG_CPU_FREQ_DEFAULT_GOV_REFLEX=y']),
    ('no-adios', base + ['CONFIG_DEFAULT_ADIOS=y']),
    ('not-default', base + ['CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y',
                           'CONFIG_CPU_FREQ_GOV_REFLEX=y', 'CONFIG_IOSCHED_ADIOS=y'])):
    cfg = resolve(name, lines)
    assert cfg.get('CPU_FREQ_DEFAULT_GOV_REFLEX') != 'y', (name, cfg)
    assert cfg.get('DEFAULT_ADIOS') != 'y', (name, cfg)
    if name == 'not-default':
        assert cfg['CPU_FREQ_GOV_REFLEX'] == 'y'
        assert cfg['CPU_FREQ_DEFAULT_GOV_PERFORMANCE'] == 'y'
anxiety = resolve('anxiety', base + ['CONFIG_DEFAULT_ANXIETY=y', 'CONFIG_IOSCHED_ANXIETY=y'])
assert anxiety['DEFAULT_IOSCHED'] == '"anxiety"'
print('PASS actual Kconfig: default choices, derived strings, dependency closure, false combinations, Anxiety alternative')

# Preprocess actual C (Linux headers removed at the host boundary); compile/link
# each resolved strong provider alongside the core weak provider. No fake
# default function is supplied. Other C bodies are not host-kernel emulation.
paths = list((root / 'drivers/cpufreq').glob('*.c')) + [root / 'kernel/sched/cpufreq_schedutil.c']
providers = [p for p in paths if re.search(r'^struct cpufreq_governor \*cpufreq_default_governor\(void\)', p.read_text(), re.M)]
assert len(providers) == 8, providers

def preprocess(path, cfg):
    text = re.sub(r'^\s*#\s*include[^\n]*', '', path.read_text(), flags=re.M)
    text = '\n'.join('#define CONFIG_' + k + ' ' + v for k, v in cfg.items()
                     if v == 'y') + '\n' + text
    return subprocess.check_output(['cc', '-E', '-P', '-x', 'c', '-'], input=text, text=True)

names = ('PERFORMANCE', 'POWERSAVE', 'USERSPACE', 'ONDEMAND', 'CONSERVATIVE',
         'INTERACTIVE', 'SCHEDUTIL', 'REFLEX')
for name in names:
    cfg = resolve('gov-' + name.lower(), base + ['CONFIG_CPU_FREQ_DEFAULT_GOV_' + name + '=y'])
    bodies = []
    for path in providers:
        pp = preprocess(path, cfg)
        bodies += re.findall(r'struct cpufreq_governor \*cpufreq_default_governor\(void\)\s*\{[^}]*\}', pp)
    assert len(bodies) == 1, (name, bodies)
    target = re.search(r'return\s+\(?&(\w+(?:\.gov)?)\)?;', bodies[0]).group(1)
    decl = ('static struct { struct cpufreq_governor gov; } ' + target.split('.')[0]
            if '.' in target else 'static struct cpufreq_governor ' + target)
    unit = out / ('provider-' + name.lower() + '.c')
    unit.write_text('struct cpufreq_governor { int unused; };\n' + decl + ';\n' + bodies[0] + '\nint main(void) { return cpufreq_default_governor() != &' + target + '; }\n')
    core = (root / 'drivers/cpufreq/cpufreq.c').read_text()
    weak = re.search(r'__weak struct cpufreq_governor \*cpufreq_default_governor\(void\)\s*\{[^}]*\}', core).group(0)
    weakfile = out / 'weak.c'
    weakfile.write_text('#define __weak __attribute__((weak))\n#define NULL ((void *)0)\nstruct cpufreq_governor;\n' + weak)
    exe = unit.with_suffix('')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(unit), str(weakfile), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    if name == 'REFLEX':
        assert target == 'reflex_gov'
        pp = preprocess(root / 'drivers/cpufreq/cpufreq_reflex.c', cfg)
        assert 'postcore_initcall(reflex_gov_init);' in pp
        assert 'module_init(reflex_gov_init);' not in pp
pp = preprocess(root / 'drivers/cpufreq/cpufreq_reflex.c', {'CPU_FREQ_GOV_REFLEX': 'y'})
assert 'module_init(reflex_gov_init);' in pp
assert 'postcore_initcall(reflex_gov_init);' not in pp
assert 'cpufreq_default_governor(void)' not in pp
assert 'core_initcall(cpufreq_core_init)' in (root / 'drivers/cpufreq/cpufreq.c').read_text()
qcom = (root / 'drivers/cpufreq/qcom-cpufreq.c').read_text()
assert 'subsys_initcall(msm_cpufreq_register)' in qcom
assert 'cpufreq_register_driver(&msm_cpufreq_driver)' in qcom
main = (root / 'init/main.c').read_text()
assert main.index('wait_for_completion(&kthreadd_done)') < main.index('workqueue_init();') < main.index('do_pre_smp_initcalls();', main.index('workqueue_init();'))
perf = preprocess(root / 'drivers/cpufreq/cpufreq_performance.c', active)
assert 'cpufreq_fallback_governor(void)' in perf
assert 'fs_initcall(cpufreq_gov_performance_init);' in perf
source = (root / 'drivers/cpufreq/cpufreq_reflex.c').read_text()
init = re.search(r'static int __init reflex_gov_init\(void\)\s*\{[^}]*\}', source).group(0)
unit = out / 'register.c'
unit.write_text('''#include <assert.h>
#define __init
#define pr_info(...) ((void)0)
struct cpufreq_governor { int unused; };
static struct cpufreq_governor reflex_gov;
static int result, calls;
static int cpufreq_register_governor(struct cpufreq_governor *gov)
{ assert(gov == &reflex_gov); calls++; return result; }
''' + init + '''
int main(void) { result = -12; assert(reflex_gov_init() == -12);
result = 0; assert(reflex_gov_init() == 0); assert(calls == 2); return 0; }
''')
subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(out / 'register')], check=True)
subprocess.run([str(out / 'register')], check=True)
print('PASS all eight defaults: singleton strong provider + weak-core host link/run; Reflex postcore before Qualcomm subsys; non-default module init; built-in performance fallback; actual registration success/error propagation')
