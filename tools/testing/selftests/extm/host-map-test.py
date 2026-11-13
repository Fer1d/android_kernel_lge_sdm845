#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host-only execution of verbatim dm-extm-file C; no kernel/device access.

The shim models only API boundaries, not F2FS leases or DM concurrency.
Run: python3 tools/testing/selftests/extm/host-map-test.py [--cc clang]
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile


def extract(text, pattern):
    """Extract a whole C definition, balancing braces outside comments/strings."""
    match = re.search(pattern, text, re.M)
    if not match:
        raise RuntimeError(f"source definition not found: {pattern}")
    start = match.start()
    opening = text.index("{", match.end())
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(text, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                end = token.end()
                if text[end:end + 1] == ";":
                    end += 1
                return text[start:end]
    raise RuntimeError("unbalanced C definition")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[3]
    source_path = root / "drivers/md/dm-extm-file.c"
    source = source_path.read_text()
    header = (root / "include/linux/f2fs_extm.h").read_text()
    definitions = [extract(header, r'^struct f2fs_extm_extent\s*'),
                   extract(source, r'^struct extm_c\s*')]
    maximum = re.search(r'^#define EXTM_MAX_BYTES .+$', source, re.M)
    if not maximum:
        raise RuntimeError("EXTM_MAX_BYTES not found")
    definitions.append(maximum.group())
    # There is no separate extm_validate: execute the complete real ctr,
    # including its argument checks, extent validation and unwind paths.
    for name in ("extm_ctr", "extm_error", "extm_map"):
        definitions.append(extract(source, rf'^static int {name}\s*\([^;]*?\)\s*'))
    print(f"Source: {source_path}\nSHA256: {hashlib.sha256(source.encode()).hexdigest()}", flush=True)
    with tempfile.TemporaryDirectory(prefix="extm-host-") as directory:
        tmp = Path(directory)
        (tmp / "actual.c").write_text("\n\n".join(definitions) + "\n")
        binary = tmp / "host-map-test"
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                        "-O1", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-I", str(tmp),
                        str(here / "host-map-shim.c"), "-o", str(binary)], check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        subprocess.run([str(binary)], check=True, env=env)
    print("PASS: actual ctr/map (including inline binary search), ASan+UBSan; host shim only.")
    print("Not evidence of F2FS lease/concurrency correctness or real DM/block I/O correctness.")


if __name__ == "__main__":
    main()
