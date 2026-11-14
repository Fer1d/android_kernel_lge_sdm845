# SPDX-License-Identifier: GPL-2.0
"""Shared CLI/output setup; never emit generated files in the source tree."""
import argparse
from pathlib import Path
import tempfile


def setup(description):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('--src', type=Path,
                        default=Path(__file__).resolve().parents[4],
                        help='kernel source root (default: repository containing script)')
    parser.add_argument('--out', type=Path,
                        help='parent for a unique retained temporary output directory')
    args = parser.parse_args()
    src = args.src.resolve()
    if not (src / 'drivers/cpufreq/cpufreq_reflex.c').is_file():
        parser.error('--src must contain the applied Reflex source')
    parent = args.out.resolve() if args.out else None
    if parent and (parent == src or src in parent.parents):
        parser.error('--out must be outside the source tree')
    if parent:
        parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='reflex-selftest-', dir=parent))
    print('Generated test artifacts:', out, flush=True)
    return src, out
