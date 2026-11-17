#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host-only execution of actual zram writeback helpers in a C shim.

Run: python3 -I tools/testing/selftests/zram/zram_writeback_host_test.py
Requires host gcc; --sanitize adds AddressSanitizer and UndefinedBehaviorSanitizer.
Uses temporary build files only. Native execution models an LP64 kernel;
-m32/-m64 compile checks test the actual fixed-width userspace ABI, not a
32-bit zram kernel (whose pre-existing slot flags require separate work).
Never opens /dev, sysfs, or invokes the device-oriented zram shell tests.
A missing implementation is KSFT_SKIP (4), not a passing model test.
Kernel locks, BIOs, allocations and slot state are simulated; this is not
kernel concurrency, device I/O, or a kernel configuration build test.
"""

import argparse
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true",
                        help="enable AddressSanitizer and UndefinedBehaviorSanitizer")
    args = parser.parse_args()
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
    cc = shlex.split(os.environ.get("HOSTCC", "gcc")) + ["-D__EXPORTED_HEADERS__"]
    with tempfile.TemporaryDirectory(prefix="zram-writeback-host-") as tmp:
        build = Path(tmp)
        (build / "actual_helpers.h").write_text(generated)
        layout_checks(cc, build)
        config_checks(cc, build, text)
        for shift in (12, 14, 16):
            exe = build / ("test-" + str(shift))
            command = cc + ["-std=gnu11", "-g", "-O1", "-Wall", "-Wextra",
                            "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
                            "-fno-omit-frame-pointer", "-fno-pie",
                            "-no-pie", "-DCONFIG_ZRAM_WRITEBACK_IOCTL=1",
                            "-DCONFIG_COMPAT=1", "-DPAGE_SHIFT=" + str(shift),
                            "-I", str(build), "-I", str(ROOT / "include/uapi"),
                            str(shim), "-o", str(exe)]
            if args.sanitize:
                command.append("-fsanitize=address,undefined")
            subprocess.run(command, check=True)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
            print("# actual helper execution, simulated PAGE_SIZE=" +
                  str(1 << shift), flush=True)
            subprocess.run([str(exe)], check=True, env=env)
            # Compile/run the same legacy engine with the feature disabled.
            off = build / ("test-off-" + str(shift))
            command = [str(off) if part == str(exe) else part for part in command
                       if part != "-DCONFIG_ZRAM_WRITEBACK_IOCTL=1"]
            command.append("-Wno-unused-function")  # ioctl-only shim stubs
            subprocess.run(command, check=True)
            print("# CONFIG_ZRAM_WRITEBACK_IOCTL=n actual legacy engine, PAGE_SIZE=" +
                  str(1 << shift), flush=True)
            subprocess.run([str(off)], check=True, env=env)
    return 0


def layout_checks(cc, build):
    """Compile real UAPI at both word sizes without a 32-bit libc dependency."""
    # headers_install removes this non-UAPI include from linux/stddef.h.
    (build / "linux").mkdir()
    (build / "linux/compiler.h").write_text("/* exported UAPI: no kernel compiler definitions */\n")
    asm = build / "asm"
    asm.mkdir()
    for name in ("types", "ioctl", "posix_types", "bitsperlong"):
        (asm / (name + ".h")).write_text("#include <asm-generic/" + name + ".h>\n")
    checks = ['#include <linux/zram.h>',
              '#define OFF(t, f, n) _Static_assert(__builtin_offsetof(struct t, f) == n, #f)',
              '_Static_assert(sizeof(struct zram_wb_exec) == 160, "exec size");',
              '_Static_assert(sizeof(struct zram_wb_status) == 192, "status size");',
              '_Static_assert(__alignof__(struct zram_wb_exec) == 8, "exec align");',
              '_Static_assert(__alignof__(struct zram_wb_status) == 8, "status align");',
              '_Static_assert(ZRAM_IOC_WB_EXEC == 0xc0a05a20U, "exec cmd");',
              '_Static_assert(ZRAM_IOC_WB_STATUS == 0xc0c05a21U, "status cmd");']
    exec_fields = dict(size=0, version=4, flags=8, mode=12, request_id=16,
                       expected_instance=24, expected_generation=32,
                       max_attempt_pages=40, max_installed_pages=48, page_index=56,
                       idle_count=64, reserved0=68, reserved=72, scanned_pages=96,
                       selected_pages=104, submitted_pages=112, installed_pages=120,
                       invalidated_pages=128, duration_ns=136, stop_reason=144,
                       op_errno=148, error_stage=152, reserved_out=156)
    status_fields = dict(size=0, version=4, flags=8, reserved0=12, reserved=16,
                         capabilities=32, instance=40, generation=48,
                         disksize_bytes=56, page_size=64, state_flags=68,
                         wb_limit_remaining_4k=72, pages_stored=80, same_pages=88,
                         huge_pages=96, lowratio_pages=104, bd_count_pages=112,
                         bd_reads_pages=120, bd_writes_pages=128,
                         compr_data_bytes=136, mem_used_bytes=144, reserved_out=152)
    for struct, fields in (("zram_wb_exec", exec_fields), ("zram_wb_status", status_fields)):
        checks.extend(f"OFF({struct}, {field}, {offset});" for field, offset in fields.items())
    source = build / "layout.c"
    source.write_text("\n".join(checks) + "\n")
    for bits in (32, 64):
        subprocess.run(cc + ["-std=gnu11", "-Werror", "-ffreestanding", "-fsyntax-only",
                             "-m" + str(bits), "-I", str(build),
                             "-I", str(ROOT / "include/uapi"), str(source)], check=True)
        print(f"# real UAPI {bits}-bit size/alignment/all offsets/ioctl numbers: PASS", flush=True)


def actual_fops(text):
    tables = []
    for name in ("zram_devops", "zram_wb_devops"):
        found = re.search(r"static const struct block_device_operations " + name + r"\s*=\s*\{", text)
        if not found:
            raise ValueError("missing actual fops " + name)
        tables.append(braced(text, found.start()) + ";")
    return "\n".join(tables)


def config_checks(cc, build, text):
    """Actual dispatch and both fops compile with ioctl off and compat on/off."""
    prefix = """
#include <linux/zram.h>
#define ENOIOCTLCMD 515
#define THIS_MODULE ((void *)0)
typedef unsigned int fmode_t;
struct block_device { int unused; };
struct zram { int unused; };
struct block_device_operations {
    int (*open)(struct block_device *, fmode_t);
    int (*ioctl)(struct block_device *, fmode_t, unsigned int, unsigned long);
    int (*compat_ioctl)(struct block_device *, fmode_t, unsigned int, unsigned long);
    void (*swap_slot_free_notify)(void);
    void (*rw_page)(void);
    void *owner;
};
static int zram_open(struct block_device *b, fmode_t m) { return 0; }
static void zram_slot_free_notify(void) { }
static void zram_rw_page(void) { }
#ifdef CONFIG_COMPAT
static void *compat_ptr(unsigned long a) { return (void *)a; }
#endif
"""
    code = prefix + function(text, "zram_ioctl") + "\n#ifdef CONFIG_COMPAT\n" + \
        function(text, "zram_compat_ioctl") + "\n#endif\n" + actual_fops(text)
    code += """
int main(void) {
    struct block_device b;
    struct zram z;
    zram_wb_generation_advance(&z);
    if (zram_devops.ioctl(&b, 0, ZRAM_IOC_WB_EXEC, 0) != -ENOIOCTLCMD ||
        zram_wb_devops.ioctl(&b, 0, ZRAM_IOC_WB_STATUS, 0) != -ENOIOCTLCMD) return 1;
#ifdef CONFIG_COMPAT
    if (zram_devops.compat_ioctl(&b, 0, ZRAM_IOC_WB_EXEC, 0) != -ENOIOCTLCMD ||
        zram_wb_devops.compat_ioctl(&b, 0, ZRAM_IOC_WB_STATUS, 0) != -ENOIOCTLCMD) return 1;
#endif
    return 0;
}
"""
    stub = re.search(r"static inline void zram_wb_generation_advance[^\n]+", text)
    if not stub:
        raise ValueError("missing CONFIG-off generation stub")
    source = build / "config-off.c"
    source.write_text(prefix + stub[0] + "\n" + code[len(prefix):])
    for compat in (False, True):
        exe = build / ("config-off-" + str(compat))
        command = cc + ["-std=gnu11", "-Werror", "-Wall", "-Wextra", "-Wno-unused-parameter",
                        "-I", str(build), "-I", str(ROOT / "include/uapi"),
                        str(source), "-o", str(exe)]
        if compat:
            command.append("-DCONFIG_COMPAT=1")
        subprocess.run(command, check=True)
        subprocess.run([str(exe)], check=True)
        print(f"# CONFIG_ZRAM_WRITEBACK_IOCTL=n dispatch/fops, compat={compat}: PASS", flush=True)
    # Check lifecycle call sites too; helpers are executed in the main shim.
    for name in ("reset_bdev", "backing_dev_store", "disksize_store", "zram_reset_device", "zram_add"):
        assert "zram_wb_generation_advance(zram)" in function(text, name), name
    reset = function(text, "zram_reset_device")
    assert reset.index("zram_wb_generation_advance(zram)") < reset.index("if (!init_done(zram))")
    assert "zram_reset_device(zram)" in function(text, "zram_remove")
    print("# lifecycle generation call sites (including reset early return/removal): PASS", flush=True)


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
                 "zram_free_page", "zram_wb_stop") + REQUIRED:
        functions.append(function(text, name))
    for name in ("init_done", "dev_to_zram", "writeback_parse_filter",
                 "zram_writeback_match", "zram_writeback_engine", "writeback_store"):
        functions.append(function(text, name))
    functions.append("#ifdef CONFIG_ZRAM_WRITEBACK_IOCTL")
    for name in ("zram_wb_new_instance", "zram_wb_generation_advance"):
        functions.append(function(text, name))
    functions.append("#else\n" + re.search(
        r"static inline void zram_wb_generation_advance[^\n]+", text)[0] + "\n#endif")
    functions.append("#ifdef CONFIG_ZRAM_WRITEBACK_IOCTL")
    for name in ("zram_wb_exec_validate", "zram_wb_exec_ioctl", "zram_wb_status_ioctl"):
        functions.append(function(text, name))
    functions.append("#endif")
    functions.append(function(text, "zram_ioctl"))
    functions.append("#ifdef CONFIG_COMPAT\n" + function(text, "zram_compat_ioctl") + "\n#endif")
    functions.append(actual_fops(text))
    return declarations + "\n" + "\n\n".join(functions) + "\n"


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError) as exc:
        print("Bail out! " + str(exc), file=sys.stderr)
        sys.exit(1)
