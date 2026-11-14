#!/usr/bin/env python3
"""Compile actual allocator/helper bodies against deterministic userspace stubs."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent


def function(text, name):
    pattern = r"^(?:static inline )?void(?:\s*\*|\s+)" + name + r"\([^;]*?\)\n\{"
    matches = list(re.finditer(pattern, text, re.M))
    if len(matches) != 1:
        raise RuntimeError(f"expected one definition of {name}, found {len(matches)}")
    start = matches[0].start()
    end = text.index("\n}", matches[0].end()) + 2
    return text[start:end] + "\n"


util = (ROOT / "mm/util.c").read_text()
mm = (ROOT / "include/linux/mm.h").read_text()
slab = (ROOT / "include/linux/slab.h").read_text()
for name in ("kvmalloc_array", "kvcalloc"):
    if re.search(r"\b" + name + r"\s*\(", slab):
        raise RuntimeError(f"duplicate helper in slab.h: {name}")
bodies = "\n".join(function(util, n) for n in ("kvmalloc", "kvzalloc", "kvfree"))
bodies += "\n".join(function(mm, n) for n in ("kvmalloc_array", "kvcalloc"))
# Extract the real GFP macro dependency graph, not invented flag values.
gfp = (ROOT / "include/linux/gfp.h").read_text().replace("\\\n", "")
defs = dict(re.findall(r"^#define\s+(\w+)\s+([^\n]+)", gfp, re.M))
wanted = ["GFP_KERNEL", "GFP_NOFS", "GFP_NOIO", "GFP_ATOMIC", "GFP_NOWAIT",
          "GFP_KERNEL_ACCOUNT", "__GFP_ZERO", "__GFP_REPEAT", "__GFP_NORETRY",
          "__GFP_NOWARN", "__GFP_NOFAIL"]
macros = {}


def add_macro(name):
    if name in macros:
        return
    value = re.sub(r"/\*.*?\*/", "", defs[name]).replace("__force", "")
    for dep in re.findall(r"\b(?:___?GFP_\w+|GFP_\w+)\b", value):
        add_macro(dep)
    macros[name] = value


for name in wanted:
    add_macro(name)
zone = (ROOT / "include/linux/mmzone.h").read_text()
costly = re.search(r"^#define\s+PAGE_ALLOC_COSTLY_ORDER\s+(\d+)", zone, re.M).group(1)
preamble = "#include <stddef.h>\ntypedef unsigned int gfp_t;\n"
preamble += "".join(f"#define {k} {v}\n" for k, v in macros.items())
preamble += f"#define PAGE_ALLOC_COSTLY_ORDER {costly}\n"
harness = (HERE / "test.c").read_text()
source = preamble + harness.replace("/* EXTRACTED_FUNCTIONS */", bodies)
cc = shlex.split(os.environ.get("CC", "cc"))
with tempfile.TemporaryDirectory(prefix="kvmalloc-selftest-") as tmp:
    src = Path(tmp) / "test.c"
    src.write_text(source)
    for page_size in (4096, 65536):
        for mode, extra in (("normal", []), ("asan-ubsan", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"])):
            binary = Path(tmp) / f"test-{page_size}-{mode}"
            subprocess.run(cc + ["-std=gnu99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", f"-DPAGE_SIZE={page_size}UL"] + extra + [str(src), "-o", str(binary)], check=True)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
            print(f"RUN page_size={page_size} {mode}", flush=True)
            subprocess.run([str(binary)], check=True, env=env)
