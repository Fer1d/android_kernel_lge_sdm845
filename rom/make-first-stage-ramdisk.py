#!/usr/bin/env python3
#
# Build a minimal *first stage* ramdisk for judyln, modelled on the one the
# caymanslm (LG G910) boot image ships.
#
# Why not use the stock judyln ramdisk: it is a *recovery* ramdisk. It carries
# /system/bin/recovery, and init's IsRecoveryMode() is just
#
#     access("/system/bin/recovery", F_OK) == 0
#
# so with that file present DoFirstStageMount() bails out immediately
# ("First stage mount skipped (recovery mode)") and nothing is mounted: no
# /system, no pivot, init keeps running from the ramdisk into recovery.
#
# The caymanslm ramdisk is the clean shape we want: sixteen entries, no
# recovery binary, and its own first stage fstab at the ramdisk root. Its
# /init is even statically linked, which is what keeps the ramdisk small -
# judyln's own init is dynamic and would drag in linker64 plus every .so in
# system/lib64 (that is why the stock ramdisk is 10.9 MB).
#
# Both images are Android 10 (os_version 0x14000157 / 0x14000151), so the
# caymanslm init is a drop-in first stage binary for judyln: it only performs
# the first stage and then execs /system/bin/init from judyln's own system
# partition, which stays untouched.
#
# Usage:
#   make-first-stage-ramdisk.py [--init <path>] [--fstab rom/fstab.judyln]
#                               [--out /home/ssx/LGE/out/ramdisk-judyln-firststage.gz]

import argparse
import gzip
import hashlib
import os
import struct
import sys

# Directories in the ramdisk root. The mount points judyln's first stage needs
# are created both here and inside first_stage_ramdisk/, because which root is
# live when the fstab is read depends on force_normal_boot:
#
#   force_normal_boot=1 -> init does mkdir("/first_stage_ramdisk"), bind mounts
#       it to itself and SwitchRoot()s into it, and only *then* reads the fstab
#       (see first_stage_init.cpp). The live root is the first_stage_ramdisk
#       subtree, which is why LOS ships first_stage_ramdisk/fstab.judyln.
#   no flag -> the ramdisk root stays live and /fstab.judyln is used.
#
# fs_mgr mkdir()s single component mount points itself, but not nested ones like
# /mnt/vendor/persist-lg, so those have to exist ahead of time.
MOUNT_POINTS = [
    "system",
    "vendor",
    "oem",
    "oem/OP",
    "mnt",
    "mnt/vendor",
    "mnt/vendor/persist-lg",
]
ENTRIES = [
    ("dev", 0o755, True),
    ("proc", 0o755, True),
    ("sys", 0o755, True),
    ("apex", 0o755, True),
    ("debug_ramdisk", 0o755, True),
] + [(p, 0o755, True) for p in MOUNT_POINTS] \
  + [("first_stage_ramdisk", 0o755, True)] \
  + [("first_stage_ramdisk/" + p, 0o755, True) for p in MOUNT_POINTS]


def cpio_entry(name, mode, data=b"", is_dir=False):
    """One newc entry. Name and data are padded so the next field starts on a
    4 byte boundary (not "pad the name length to 4")."""
    namesize = len(name) + 1
    filesize = len(data)
    fields = (0, mode | (0o040000 if is_dir else 0o100000), 0, 0, 1, 0,
              filesize, 0, 0, 0, 0, namesize, 0)
    out = bytearray(b"070701" + b"".join(b"%08x" % f for f in fields))
    out += name.encode() + b"\x00"
    out += b"\x00" * ((-(len(out))) % 4)
    out += data
    out += b"\x00" * ((-(len(out))) % 4)
    return bytes(out)


def extract_init_from_boot(path):
    """Pull the first stage init out of a boot image.

    The caymanslm ramdisk keeps it at the ramdisk root as "init" and it is
    statically linked; the stock judyln one has it as "system/bin/init" and is
    dynamically linked, so it needs the whole system/lib64 tree and is not
    usable for a minimal ramdisk.
    """
    d = open(path, "rb").read()
    if d[:8] != b"ANDROID!":
        sys.exit("%s is not an android boot image" % path)
    ks = struct.unpack_from("<I", d, 8)[0]
    rs = struct.unpack_from("<I", d, 0x10)[0]
    page = struct.unpack_from("<I", d, 0x24)[0]
    off = page + (ks + page - 1) // page * page
    cpio = gzip.decompress(d[off:off + rs])
    # walk the archive looking for the first stage init
    pos = 0
    while pos + 110 <= len(cpio):
        if cpio[pos:pos + 6] != b"070701":
            break
        f = [int(cpio[pos + 6 + 8 * i:pos + 6 + 8 * (i + 1)], 16) for i in range(13)]
        namesize, filesize = f[11], f[6]
        name = cpio[pos + 110:pos + 110 + namesize - 1].decode("utf-8", "replace")
        data_off = (pos + 110 + namesize + 3) // 4 * 4
        end = (data_off + filesize + 3) // 4 * 4
        if name in ("init", "system/bin/init"):
            print("  init source : %s" % name)
            return cpio[data_off:data_off + filesize]
        pos = end
    sys.exit("no init found in %s" % path)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--init", default="/home/ssx/LGE/G910_extracted/4.boot_a.img",
                    help="boot image to take the statically linked first stage init from")
    ap.add_argument("--fstab", default=os.path.join(here, "fstab.judyln"))
    ap.add_argument("--out",
                    default="/home/ssx/LGE/out/ramdisk-judyln-firststage.gz")
    args = ap.parse_args()

    init = extract_init_from_boot(args.init)
    fstab = open(args.fstab, "rb").read()

    out = bytearray()
    for name, mode, is_dir in ENTRIES:
        out += cpio_entry(name, mode, b"", is_dir)
    out += cpio_entry("init", 0o750, init)
    # both roots, so the image works whether or not force_normal_boot is set
    out += cpio_entry("fstab.judyln", 0o640, fstab)
    out += cpio_entry("first_stage_ramdisk/fstab.judyln", 0o640, fstab)
    out += cpio_entry("TRAILER!!!", 0, b"")

    gz = gzip.compress(bytes(out), 9, mtime=0)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    open(args.out, "wb").write(gz)

    print("init        : %s (%d bytes, %s)" % (args.init, len(init),
          "static" if len(init) > 1500000 else "dynamic?"))
    print("fstab       : %s (%d bytes)" % (args.fstab, len(fstab)))
    print("entries     : %d" % (len(ENTRIES) + 3))
    print("cpio        : %d bytes" % len(out))
    print("output      : %s (%d bytes)" % (args.out, len(gz)))
    print("              sha256 %s" % hashlib.sha256(gz).hexdigest())


if __name__ == "__main__":
    main()
