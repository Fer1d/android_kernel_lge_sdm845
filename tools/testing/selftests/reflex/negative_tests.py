# SPDX-License-Identifier: GPL-2.0
"""Mutation checks: known regressions must fail; only temporary copies change."""
from pathlib import Path
import runpy
import shutil
import subprocess
import sys

setup = runpy.run_path(str(Path(__file__).with_name('test_support.py')))['setup']
src, out = setup('Reflex negative host state tests using temporary source copies')
files = ('drivers/cpufreq/cpufreq_reflex.c', 'include/linux/cpufreq.h',
         'include/linux/kthread.h')
relative = files[0]
original = (src / relative).read_text()
cases = {
    'missing-helper': ('!cpumask_test_cpu(smp_processor_id(), rfx_pol->policy->cpus)',
                       '!cpufreq_can_do_remote_dvfs(rfx_pol->policy)'),
    'missing-field': ('kthread_bind_mask(thread, policy->related_cpus);',
                      'if (policy->dvfs_possible_from_any_cpu)\n'
                      '\t\tkthread_bind_mask(thread, policy->related_cpus);'),
    'single-dropped-return': ('next_f = cpufreq_driver_fast_switch(rfx_pol->policy, next_f);',
                              'cpufreq_driver_fast_switch(rfx_pol->policy, next_f);'),
    'shared-dropped-return': ('next_f = cpufreq_driver_fast_switch(rfx_pol->policy, next_f);',
                              'cpufreq_driver_fast_switch(rfx_pol->policy, next_f);'),
    'failed-cur-overwrite': ('if (next_f != CPUFREQ_ENTRY_INVALID)', 'if (true)'),
    'wrong-worker-mask': ('kthread_bind_mask(thread, policy->related_cpus);',
                          'kthread_bind_mask(thread, policy->cpus);'),
    'remote-fast-allowed': ('!cpumask_test_cpu(smp_processor_id(), rfx_pol->policy->cpus)',
                            '(!cpumask_test_cpu(smp_processor_id(), rfx_pol->policy->cpus) && false)'),
}
for name, (old, new) in cases.items():
    tree = out / name
    for relative_file in files:
        dest = tree / relative_file
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src / relative_file, dest)
    assert old in original, name
    if name == 'shared-dropped-return':
        pos = original.rindex(old)
        mutated = original[:pos] + original[pos:].replace(old, new, 1)
    else:
        mutated = original.replace(old, new, 1)
    (tree / relative).write_text(mutated)
    result = subprocess.run([sys.executable, '-I',
                             str(Path(__file__).with_name('review_tests.py')),
                             '--src', str(tree), '--out', str(out / 'artifacts')],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (out / (name + '.log')).write_text(result.stdout)
    assert result.returncode != 0, 'mutation escaped: ' + name
    # Reject incidental harness/tool failures as evidence of regression detection.
    assert ('AssertionError' in result.stdout or 'Assertion `' in result.stdout
            or 'Assertion ' in result.stdout), result.stdout
    if name not in ('missing-helper', 'missing-field'):
        assert 'error:' not in result.stdout, result.stdout
    print('PASS rejected:', name, flush=True)
print('PASS all seven negative regressions rejected; original source untouched')
