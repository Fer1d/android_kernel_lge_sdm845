#!/usr/bin/env python3
"""Host-only tests. Never rewrite the source tree or produce a patch."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

BASELINE = '42758fbbe770918f9c654e78791e3893fa7dbef4'
DRIVER = 'drivers/scsi/ufs/ufshcd.c'


def masked(text):
    # Preserve offsets/newlines while ignoring braces in strings and comments.
    pattern = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda m: re.sub(r'[^\n]', ' ', m.group()), text,
                  flags=re.S)


def extract(text, name):
    clean = masked(text)
    pattern = (r'^(?:static\s+)?(?:inline\s+)?(?:int|void|bool)\s+'
               + re.escape(name) + r'\s*\([^;{}]*\)\s*\{')
    hits = list(re.finditer(pattern, clean, re.M))
    if len(hits) != 1:
        raise ValueError(f'expected one definition of {name}, found {len(hits)}')
    start, pos = hits[0].start(), hits[0].end()
    depth = 1
    while depth and pos < len(clean):
        depth += (clean[pos] == '{') - (clean[pos] == '}')
        pos += 1
    if depth:
        raise ValueError(f'unterminated function: {name}')
    return text[start:pos]


def span(text, begin, end):
    if text.count(begin) != 1 or text.count(end) != 1:
        raise ValueError(f'nonunique/missing extraction boundary: {begin!r}, {end!r}')
    a, b = text.index(begin), text.index(end)
    if b <= a:
        raise ValueError('reversed extraction boundaries')
    return text[a:b]


def run(command, **kwargs):
    print('+', shlex.join(map(str, command)), flush=True)
    return subprocess.run(command, check=True, **kwargs)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--src', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--hostcc', default=os.environ.get('HOSTCC', 'cc'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--baseline-source', type=Path,
                   help='trusted ufshcd.c exported from the exact fixed baseline when git object is unavailable')
    args = p.parse_args()
    src, out = args.src.resolve(), args.out.resolve()
    if out == src or src in out.parents:
        raise ValueError('--out must be outside --src (source stays read-only)')
    out.mkdir(parents=True, exist_ok=True)
    source = (src / DRIVER).read_text()
    here = Path(__file__).resolve().parent
    manifest = []

    def function(name, text=source, path=DRIVER):
        result = extract(text, name)
        manifest.append(f'{path}: {name}: sha256={hashlib.sha256(result.encode()).hexdigest()}')
        return result

    # This is deliberately not HEAD. No network operations, even in shallow trees.
    if args.baseline_source:
        baseline = args.baseline_source.read_text()
        print(f'Baseline explicit export: {args.baseline_source}; provenance must be {BASELINE}')
    else:
        try:
            baseline = subprocess.check_output(
                ['git', '-C', str(src), 'show', f'{BASELINE}:{DRIVER}'], text=True,
                stderr=subprocess.PIPE)
        except subprocess.CalledProcessError as exc:
            raise ValueError(f'fixed baseline {BASELINE} missing (including shallow clones); '
                             'provide --baseline-source with a trusted exact export; no fetch or HEAD fallback') from exc
    if function('ufshcd_remove') != extract(baseline, 'ufshcd_remove'):
        raise ValueError('normal remove differs from fixed baseline ' + BASELINE)
    print('PASS: normal remove equals fixed baseline ' + BASELINE)

    lifecycle = '\n'.join(function(n) for n in (
        'ufshcd_queue_recovery', 'ufshcd_init_handoff', 'ufshcd_scan_handoff', 'ufshcd_hold_all'))
    scan = '\n'.join(function(n) for n in (
        'ufshcd_queue_recovery', 'ufshcd_scan_handoff', 'ufshcd_async_scan', 'ufshcd_card_detect_handler'))
    hibern8 = '\n'.join(function(n) for n in (
        '__ufshcd_uic_hibern8_enter', 'ufshcd_uic_hibern8_enter', 'ufshcd_uic_hibern8_exit'))
    tm = function('ufshcd_issue_tm_cmd')
    tm_prefix = span(tm, '\thost = hba->host;', '\n\tspin_lock_irqsave(host->host_lock, flags);')
    manifest.append('TM: actual acquisition/hold/unwind prefix ONLY; remainder is a test stub')
    init = function('ufshcd_init')
    # Execute the complete suffix, not a handwritten ordering model. All real
    # failure labels are retained, including IRQ fencing and PM unwind.
    init_suffix = init[init.index('\t/* IRQ registration */'):]
    manifest.append('init: actual suffix from IRQ registration through final return; earlier setup stubbed')
    if not (init.index('INIT_WORK(&hba->eh_work') < init.index('devm_request_irq') <
            init.index('ufshcd_hba_enable') < init.index('ufshcd_init_handoff') <
            init.index('async_schedule')):
        raise ValueError('unexpected init work/IRQ/enable/handoff/async source order')
    tail = span(init, '/* No synchronous failure exits below this point. */', '\nout_remove_scsi_host:')
    if re.search(r'\bgoto\b', masked(tail)):
        raise ValueError('synchronous failure after LIVE handoff')
    bus = []
    for filename, name in [('ufshcd-pltfrm.c', 'ufshcd_pltfrm_init'),
                           ('ufshcd-pci.c', 'ufshcd_pci_probe')]:
        path = 'drivers/scsi/ufs/' + filename
        bus.append(function(name, (src / path).read_text(), path))
    pm = function('ufshcd_queue_recovery') + '\n' + function('ufshcd_init_handoff')
    pm += '\nint ufshcd_init(struct ufs_hba *hba, void *mmio_base, unsigned int irq)\n{\n'
    pm += 'int err; struct Scsi_Host *host=hba->host; struct device *dev=hba->dev;\n'
    pm += '(void)mmio_base; dev->hba=hba; hba->init_lifecycle=UFSHCD_INIT;\n' + init_suffix
    pm += '\n' + '\n'.join(bus)
    suites = {'lifecycle': lifecycle, 'scan': scan, 'hibern8': hibern8, 'tm': tm_prefix, 'pm': pm}
    flags = ['-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-unused-function',
             '-Wno-unused-label']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
    for name, actual in suites.items():
        template = (here / (name + '_shim.c')).read_text()
        if template.count('/* ACTUAL_SOURCE */') != 1:
            raise ValueError(f'bad shim marker: {name}')
        cfile, exe = out / (name + '.c'), out / name
        cfile.write_text(template.replace('/* ACTUAL_SOURCE */', actual))
        run(shlex.split(args.hostcc) + flags + [str(cfile), '-o', str(exe)])
        environment = os.environ.copy()
        if args.sanitize:
            environment['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1:abort_on_error=1'
            environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        run([str(exe)], cwd=out, env=environment)
    (out / 'extraction-manifest.txt').write_text('\n'.join(manifest) + '\n')
    print('PASS: all five extracted-source suites')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
