#!/usr/bin/env python3
"""Host C tests of extracted production code; no kernel/device safety claim."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / 'kernel/sched/fair.c'
BASE = '97c1beea4278468ddea9f967019211c4330c6f03'


def tokens(text):
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
    return re.findall(r'\w+|[^\s]', text)


def main():
    source = SOURCE.read_text()
    begin = source.index('#ifdef CONFIG_SCHED_CAMBYSES_BUDGET\n/* Sticky')
    end = source.index('#endif /* CONFIG_SCHED_CAMBYSES_BUDGET */', begin)
    region = source[begin:end] + '#endif\n'
    baseline = subprocess.check_output(
        ['git', '-C', str(ROOT), 'show', BASE + ':kernel/sched/fair.c'], text=True)
    start = baseline.index('static int detach_tasks(struct lb_env *env)')
    finish = baseline.index('\n/*\n * attach_task()', start)
    original = baseline[start:finish]
    # Everything outside the isolated region must match HEAD, including BORE/POC.
    pred_begin = source.index('#ifdef CONFIG_SCHED_CAMBYSES_BUDGET\n')
    pred_end = source.index('#endif /* migration predicate */', pred_begin) + len('#endif /* migration predicate */\n')
    predicate = source[pred_begin:pred_end]
    base_pred_begin = baseline.index('static\nint can_migrate_task(')
    base_pred_end = baseline.index('\n/*\n * detach_task()', base_pred_begin)
    assert source[:pred_begin] == baseline[:base_pred_begin]
    assert source[pred_end:begin] == baseline[base_pred_end:start]
    assert source[end + len('#endif /* CONFIG_SCHED_CAMBYSES_BUDGET */\n'):] == baseline[finish:]
    cc = os.environ.get('CC', 'cc')
    with tempfile.TemporaryDirectory(prefix='cambyses-host-') as tmp:
        tmp = Path(tmp)
        selection = tmp / 'selection.c'
        selection.write_text(region)
        (tmp / 'predicate.h').write_text(predicate)
        assert tokens(predicate.split('#else /* original migration predicate */\n')[1].split('#endif /* migration predicate */')[0]) == tokens(baseline[base_pred_begin:base_pred_end])
        legacy = tmp / 'legacy.c'
        legacy.write_text(original)
        assert tokens(region.split('#else /* original vendor path, token-for-token unchanged */\n')[1].rsplit('#endif', 1)[0]) == tokens(original)
        for defines in ([], ['-DCONFIG_PREEMPT']):
            off = subprocess.check_output([cc, '-E', '-P', '-x', 'c', *defines, str(selection)], text=True)
            expected = subprocess.check_output([cc, '-E', '-P', '-x', 'c', *defines, str(legacy)], text=True)
            assert tokens(off) == tokens(expected), 'CONFIG=n/off differs from original'
        print('PASS CONFIG=n/off preprocessed source tokens; outside-region BORE/POC unchanged', flush=True)
        for preempt in (False, True):
            for enabled in (False, True):
                binary = tmp / 'host-test'
                command = [cc, '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', '-g',
                           '-fsanitize=undefined', '-fno-sanitize-recover=all',
                           '-I', str(tmp), '-o', str(binary),
                           str(Path(__file__).with_name('harness.c'))]
                # The off build exercises the exact production legacy function.
                (tmp / 'selected.h').write_text(region)
                if enabled:
                    command.append('-DCONFIG_SCHED_CAMBYSES_BUDGET')
                if preempt:
                    command.append('-DCONFIG_PREEMPT')
                subprocess.run(command, check=True)
                subprocess.run([str(binary)], check=True)
        # Negative controls reintroduce each pre-review defect independently.
        for name, broken in (
            ('pause concession', region.replace('LBF_CAMBYSES_MOVED | LBF_NEED_BREAK', 'LBF_CAMBYSES_MOVED')),
            ('duplicate hot accounting', region.replace('can_migrate_task_check(p, env, false)', 'can_migrate_task(p, env)')),
        ):
            assert broken != region
            (tmp / 'selected.h').write_text(broken)
            subprocess.run([cc, '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                            '-Wno-sign-compare', '-DCONFIG_SCHED_CAMBYSES_BUDGET',
                            '-I', str(tmp), '-o', str(binary),
                            str(Path(__file__).with_name('harness.c'))], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    cwd=tmp)
            assert result.returncode != 0, name + ' negative control unexpectedly passed'
            expected = 'LBF_NEED_BREAK' if name == 'pause concession' else 'lb_hot_gained'
            assert expected in result.stderr, result.stderr
            print('PASS negative control rejected: ' + name, flush=True)
    print('PASS all extracted-C tests (on/off x PREEMPT on/off, UBSan)')


if __name__ == '__main__':
    main()
