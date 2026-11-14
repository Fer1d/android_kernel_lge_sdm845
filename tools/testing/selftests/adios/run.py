#!/usr/bin/env python3
"""Compile actual scheduler C with host mocks; never build/run a kernel.
Includes the complete scheduler translation unit (only Linux includes removed),
actual lib/kstrtox conversions, dispatch tail, and core elevator allocation,
release, get/put, init/exit, switch and change bodies (without source rewrites).
Negative controls demonstrate the old double-put and the core module leak;
passing does NOT mean modular switch failures are fixed. Kconfig must be bool.
Mock boundary and limitations are documented in host.h. Temporary outputs only.
Run: python3 -I /home/ssx/android-kernel/tools/testing/selftests/adios/run.py
"""
import os
from pathlib import Path
import re
import resource
import subprocess
import tempfile

# The negative exit-lifecycle test intentionally aborts a child, without cores.
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def function(text, name):
    match = re.search(r'^(?:static )?[\w *]+?\b' +
                      re.escape(name) + r'\([^;]*?\)\n\{', text, re.M)
    if not match:
        raise RuntimeError('cannot extract ' + name)
    start = match.start()
    pos = match.end()
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[start:pos] + '\n'


src = (ROOT / 'block/adios-iosched.c').read_text()
src = re.sub(r'^#include <linux/[^>]+>\n', '', src, flags=re.M)
kstr = (ROOT / 'lib/kstrtox.c').read_text()
parser = ''.join(function(kstr, f) for f in (
    '_parse_integer_fixup_radix', '_parse_integer', '_kstrtoull',
    'kstrtoull', 'kstrtouint'))
elevator = (ROOT / 'block/elevator.c').read_text()
core = 'static struct kobj_type elv_ktype;\n' + ''.join(function(elevator, f) for f in (
    'elevator_put', 'elevator_get', 'elevator_alloc', 'elevator_release'))
core += '\nstatic struct kobj_type elv_ktype = { elevator_release };\n'
core += '\nstatic char chosen_elevator[ELV_NAME_MAX];\n'
core += ''.join(function(elevator, f) for f in (
    'elevator_init', 'elevator_exit', 'elevator_switch', '__elevator_change',
    'elevator_change', 'elv_dispatch_add_tail'))
kconfig = (ROOT / 'block/Kconfig.iosched').read_text()
assert re.search(r'config IOSCHED_ADIOS\n\tbool [^\n]+\n\tdefault n\n', kconfig)
assert 'REQ_OP_WRITE_SAME' in (ROOT / 'include/linux/blk_types.h').read_text()
with tempfile.TemporaryDirectory(prefix='adios-host-') as tmp:
    tmp = Path(tmp)
    unit = tmp / 'actual.c'
    unit.write_text('#include "host.h"\n' + parser + core + src +
                    '\n#include "test.c"\n')
    exe = tmp / 'adios-test'
    cmd = [os.environ.get('CC', 'gcc'), '-std=gnu89', '-g', '-O1',
           '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
           '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
           '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
           '-I', str(HERE), str(unit), '-o', str(exe)]
    subprocess.run(cmd, check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    subprocess.run([str(exe)], check=True, env=env)
