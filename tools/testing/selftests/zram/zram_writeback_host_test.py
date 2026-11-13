#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host-only execution of actual zram writeback helpers in a C shim.

Run: python3 -I tools/testing/selftests/zram/zram_writeback_host_test.py
Requires host gcc with AddressSanitizer and UndefinedBehaviorSanitizer.
Never opens /dev, sysfs, or invokes the device-oriented zram shell tests.
A missing implementation is KSFT_SKIP (4), not a passing model test.
Kernel locks, BIOs, allocations and slot state are simulated; this is not
kernel concurrency, device I/O, or a kernel configuration build test.
"""

import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

KSFT_SKIP = 4
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SOURCE = ROOT / "drivers/block/zram/zram_drv.c"
REQUIRED = ("zram_wb_reserve", "zram_wb_refund", "zram_wb_complete",
            "zram_wb_flush")


def masked(text):
    """Preserve offsets while ignoring comments and C string/char literals."""
    pattern = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda m: "".join("\n" if c == "\n" else " "
                                            for c in m[0]), text, flags=re.S)


def braced(text, start):
    clean = masked(text)
    opening = clean.index("{", start)
    depth = 1
    end = opening + 1
    while depth and end < len(clean):
        depth += (clean[end] == "{") - (clean[end] == "}")
        end += 1
    if depth:
        raise ValueError("unclosed C block")
    return text[start:end]


def function(text, name):
    clean = masked(text)
    match = re.search(r"^static\s+[^;{}]*\b" + re.escape(name) +
                      r"\s*\([^;{}]*\)\s*\{", clean, re.M)
    if not match:
        raise ValueError("missing actual helper " + name)
    return braced(text, match.start())


def main():
    text = SOURCE.read_text()
    missing = []
    for name in REQUIRED:
        try:
            function(text, name)
        except ValueError:
            missing.append(name)
    if missing:
        print("TAP version 13\n1..0 # SKIP waiting for actual helpers: " +
              ", ".join(missing))
        return KSFT_SKIP

    shim = HERE / "zram_writeback_host_shim.c"
    if not shim.exists():
        print("TAP version 13\n1..0 # SKIP actual helpers now present; "
              "host shim must be matched to their implemented interface")
        return KSFT_SKIP
    try:
        generated = extract(text)
    except ValueError as exc:
        print("Bail out! unsupported implementation: " + str(exc))
        return 1
    print("# extracted helper snapshot sha256=" +
          hashlib.sha256(generated.encode()).hexdigest(), flush=True)
    cc = shlex.split(os.environ.get("HOSTCC", "gcc"))
    with tempfile.TemporaryDirectory(prefix="zram-writeback-host-") as tmp:
        build = Path(tmp)
        (build / "actual_helpers.h").write_text(generated)
        for shift in (12, 14, 16):
            exe = build / ("test-" + str(shift))
            command = cc + ["-std=gnu11", "-g", "-O1", "-Wall", "-Wextra",
                            "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
                            "-fno-omit-frame-pointer", "-fno-pie",
                            "-no-pie", "-fsanitize=address,undefined",
                            "-DPAGE_SHIFT=" + str(shift), "-I", str(build),
                            str(shim), "-o", str(exe)]
            subprocess.run(command, check=True)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
            print("# actual helper execution, simulated PAGE_SIZE=" +
                  str(1 << shift), flush=True)
            subprocess.run([str(exe)], check=True, env=env)
    return 0


def extract(text):
    header = (SOURCE.parent / "zram_drv.h").read_text()
    enum = re.search(r"enum zram_pageflags\s*\{", masked(header))
    batch = re.search(r"struct zram_wb_batch\s*\{", masked(text))
    if not enum or not batch:
        raise ValueError("missing actual flags or batch declaration")
    defines = []
    for name in ("ZRAM_FLAG_SHIFT", "ZRAM_COMP_PRIORITY_MASK",
                 "ZRAM_IDLE_COUNT_SHIFT", "ZRAM_IDLE_COUNT_MAX",
                 "ZRAM_IDLE_COUNT_BITS", "ZRAM_IDLE_COUNT_MASK"):
        match = re.search(r"^#define\s+" + name + r"\s+[^\n]+", header, re.M)
        if not match:
            raise ValueError("missing actual constant " + name)
        defines.append(match[0])
    match = re.search(r"^#define ZRAM_WB_BATCH_SIZE\s+\d+", text, re.M)
    if not match:
        raise ValueError("missing actual batch size")
    declarations = "\n".join(defines + [braced(header, enum.start()) + ";",
                                         match[0], braced(text, batch.start()) + ";"])
    for name in ("PAGE_WB_SIG", "PAGE_WRITEBACK", "HUGE_WRITEBACK",
                 "IDLE_WRITEBACK", "FILTERED_IDLE_WRITEBACK"):
        match = re.search(r"^#define\s+" + name + r"\s+[^\n]+", text, re.M)
        if not match:
            raise ValueError("missing actual writeback mode " + name)
        declarations += "\n" + match[0]
    functions = [function(header, "zram_clear_idle_count"),
                 function(header, "zram_idle_count")]
    for name in ("zram_slot_lock", "zram_slot_unlock", "zram_get_handle",
                 "zram_set_handle", "zram_test_flag", "zram_set_flag",
                 "zram_clear_flag", "zram_set_element", "zram_get_element",
                 "zram_get_obj_size", "zram_set_obj_size", "zram_set_priority",
                 "zram_allocated", "alloc_block_bdev", "free_block_bdev",
                 "zram_free_page") + REQUIRED:
        functions.append(function(text, name))
    for name in ("init_done", "dev_to_zram", "writeback_parse_filter",
                 "zram_writeback_match", "writeback_store"):
        functions.append(function(text, name))
    return declarations + "\n" + "\n\n".join(functions) + "\n"


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError) as exc:
        print("Bail out! " + str(exc), file=sys.stderr)
        sys.exit(1)
