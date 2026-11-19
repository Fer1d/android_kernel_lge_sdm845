#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile the live observer and filtered-idle predicate against a host shim."""
import argparse
import os
from pathlib import Path
import re
import runpy
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    helpers = runpy.run_path(str(HERE / 'zram_writeback_host_test.py'))
    source = helpers['SOURCE']
    function, braced = helpers['function'], helpers['braced']
    drv = source.read_text()
    header = source.with_suffix('.h').read_text()
    observer = (source.parent / 'zram_pool_observer.c').read_text()
    prelude = re.findall(r'^#define (?:ZRAM_(?:FLAG_SHIFT|IDLE_COUNT_SHIFT))\b.*',
                         header, re.M)
    prelude.append(braced(header, header.index('enum zram_pageflags {')) + ';')
    functions = [function(header, 'zram_idle_count')]
    functions += [function(drv, name) for name in
                  ('zram_slot_trylock', 'zram_slot_unlock', 'zram_test_flag',
                   'zram_get_obj_size', 'zram_allocated', 'zram_writeback_match')]
    modes = re.findall(r'^#define (?:FILTERED_IDLE|IDLE|HUGE)_WRITEBACK\b.*', drv, re.M)
    functions.append(braced(drv, drv.index('int zram_pool_observer_slot(')))
    reset = function(drv, 'zram_reset_device')
    reset_hook = re.search(r'#ifdef CONFIG_ZRAM_POOL_OBSERVER\n(.*?)#endif',
                           reset, re.S)[1]
    assert reset.index('down_write(&zram->init_lock)') < reset.index(reset_hook)
    assert reset.index(reset_hook) < reset.index('if (!init_done(zram))')
    remove = function(drv, 'zram_remove')
    assert remove.index('zram_debugfs_unregister(zram)') < remove.index('kfree(zram)')
    assert 'debugfs_create_file("pool_observer", 0400' in observer
    # Kernel file I/O is not emulated; compile show/collect/scan unchanged.
    core = re.sub(r'^#include .*\n', '', observer.split('static int zram_pool_open')[0], flags=re.M)
    with tempfile.TemporaryDirectory(prefix='zram-observer-host-') as tmp:
        build = Path(tmp)
        (build / 'observer_flags.h').write_text('\n'.join(prelude) + '\n')
        (build / 'observer_core.h').write_text('\n'.join(
            ['#ifdef CONFIG_ZRAM_WRITEBACK'] + modes + functions + ['#endif', core]))
        (build / 'observer_reset.h').write_text(reset_hook)
        cc = shlex.split(os.environ.get('HOSTCC', 'gcc'))
        if args.sanitize:
            cc += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
        for shift in (12, 14, 16):
            for config in ((), ('ZRAM_WRITEBACK',), ('ZRAM_WRITEBACK', 'ZRAM_MEMORY_TRACKING')):
                binary = build / 'observer-test'
                subprocess.run(cc + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                     '-Wno-unused-parameter', '-pthread', '-I', str(build),
                                     f'-DPAGE_SHIFT={shift}'] +
                               [f'-DCONFIG_{key}=1' for key in config] +
                               [str(HERE / 'zram_pool_observer_host_test.c'), '-o', str(binary)],
                               check=True)
                subprocess.run([binary], check=True)
                print(f'PASS PAGE_SHIFT={shift}, {" + ".join(config) or "no writeback"}', flush=True)


if __name__ == '__main__':
    main()
