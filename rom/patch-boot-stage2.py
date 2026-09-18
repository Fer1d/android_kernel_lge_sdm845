#!/usr/bin/env python3
#
# Give the official (stock) judyln boot image two-stage init support.
#
# Why: on judyln the kernel normally skips the boot ramdisk (the bootloader
# passes skip_initramfs), so init runs straight from the system partition and
# the first stage mounts nothing - /vendor, /mnt/vendor/persist-lg and /oem/OP
# come from the device tree fstab instead (fs_mgr falls back to
# /proc/device-tree/firmware/android/fstab).
#
# With CONFIG_INITRAMFS_IGNORE_SKIP_FLAG and
# CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT the kernel keeps the
# ramdisk and reports androidboot.force_normal_boot=1 in /proc/cmdline, which
# is what init's FirstStageMain() needs to treat this recovery ramdisk as a
# normal boot. The ramdisk then has to carry a first stage fstab itself,
# because the device tree entries are disabled (fs_mgr skips entries whose
# status is not ok, and only falls back to a file when the device tree yields
# nothing). fs_mgr looks for it at /odm/etc/fstab.<hw>, /vendor/etc/fstab.<hw>
# and finally /fstab.<hw> - with hw = androidboot.hardware = judyln, so the
# file has to be /fstab.judyln at the root of the ramdisk.
#
# Usage:
#   patch-boot-stage2.py [--boot rom/judyln.boot.img] [--fstab rom/fstab.judyln]
#                        [--out /home/ssx/LGE/out/boot_a.stage2.img]

import argparse
import gzip
import hashlib
import os
import struct
import sys

ANDROID_MAGIC = b"ANDROID!"
AVB_MAGIC = b"AVBf"
FSTAB_NAME = "fstab.judyln"


def align(n, page):
    return (n + page - 1) // page * page


def read_u32(d, off):
    return struct.unpack_from("<I", d, off)[0]


def parse_boot(d):
    if d[:8] != ANDROID_MAGIC:
        sys.exit("not an android boot image (no ANDROID! magic)")
    page = read_u32(d, 0x24)
    hdr = {
        "page": page,
        "kernel_size": read_u32(d, 0x08),
        "ramdisk_size": read_u32(d, 0x10),
        "second_size": read_u32(d, 0x18),
        "dt_size": read_u32(d, 0x28),  # header v0 only
        "header_size_field": read_u32(d, 0x650),
    }
    if hdr["header_size_field"] != 0:
        sys.exit("header version >= 1 is not handled by this script")
    off_kernel = page
    off_ramdisk = off_kernel + align(hdr["kernel_size"], page)
    off_second = off_ramdisk + align(hdr["ramdisk_size"], page)
    off_dt = off_second + align(hdr["second_size"], page)
    hdr["off_kernel"] = off_kernel
    hdr["off_ramdisk"] = off_ramdisk
    hdr["off_second"] = off_second
    hdr["off_dt"] = off_dt
    hdr["image_end"] = off_dt + align(hdr["dt_size"], page)
    return hdr


def cpio_header(name, size, mode=0o100644):
    fields = (0, mode, 0, 0, 1, 0, size, 0, 0, 0, 0, len(name) + 1, 0)
    out = b"070701" + b"".join(b"%08x" % f for f in fields)
    out += name.encode() + b"\x00"
    out += b"\x00" * ((-len(out)) % 4)
    return out


def _cpio_entries(archive):
    """Yield (header_offset, name, entry_bytes, end) for every newc entry.

    The thirteen header fields are 8 character ASCII hex, and both the name and
    the file data are padded so that the *next* field starts on a 4 byte
    boundary - name padding is not "pad the name length to 4", it is
    "pad until offset + 110 + namesize is 4 byte aligned".
    """
    off = 0
    while off + 110 <= len(archive):
        if archive[off:off + 6] not in (b"070701", b"070702"):
            return
        try:
            fields = [int(archive[off + 6 + 8 * i:off + 6 + 8 * (i + 1)], 16)
                      for i in range(13)]
        except ValueError:
            return
        filesize, namesize = fields[6], fields[11]
        name = archive[off + 110:off + 110 + namesize - 1].decode("utf-8", "replace")
        data_off = (off + 110 + namesize + 3) // 4 * 4
        entry_end = (data_off + filesize + 3) // 4 * 4
        yield off, name, archive[off:entry_end], entry_end
        off = entry_end


def cpio_drop(archive, names):
    """Remove entries by name; every other entry is copied byte for byte."""
    out = bytearray()
    dropped = set()
    for _off, name, entry, _end in _cpio_entries(archive):
        if name in names:
            print("  drop entry    : %s (%d bytes)" % (name, len(entry)))
            dropped.add(name)
            continue
        out += entry
    for name in names - dropped:
        print("  drop entry    : %s NOT FOUND" % name)
    return bytes(out)


def cpio_insert(archive, name, data, mode=0o100644):
    """Insert one entry (file or directory) right before the final TRAILER!!!."""
    trailer = archive.rfind(b"TRAILER!!!")
    if trailer < 0:
        sys.exit("no cpio TRAILER!!! in the ramdisk")
    hdr_off = trailer - 110  # newc header is 110 bytes, name follows
    if archive[hdr_off:hdr_off + 6] not in (b"070701", b"070702"):
        sys.exit("could not locate the trailer header")
    entry = cpio_header(name, len(data), mode) + data
    entry += b"\x00" * ((-len(entry)) % 4)
    return archive[:hdr_off] + entry + archive[hdr_off:]


def avb_footer(d):
    t = d[-64:]
    if t[:4] != AVB_MAGIC:
        return None
    major, minor, orig, voff, vsz = struct.unpack_from(">IIQQQ", t, 4)
    return {"major": major, "minor": minor, "orig": orig,
            "vbmeta_offset": voff, "vbmeta_size": vsz}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--boot", default=os.path.join(here, "judyln.boot.img"))
    ap.add_argument("--fstab", default=os.path.join(here, FSTAB_NAME))
    ap.add_argument("--out", default="/home/ssx/LGE/out/boot_a.stage2.img")
    ap.add_argument("--kernel", default=None,
                    help="optional Image.gz-dtb to put in place of the stock kernel")
    ap.add_argument("--cmdline-append", default="androidboot.force_normal_boot=1",
                    help="extra cmdline put in the boot header, so init does not treat "
                         "this ramdisk as a recovery boot")
    # On by default: the stock ramdisk is a *recovery* ramdisk, and init skips
    # the whole first stage mount while /system/bin/recovery exists
    # (IsRecoveryMode() in init/util.cpp), so an image without this is known
    # not to boot. --keep-recovery only makes sense when swapping in a real
    # first stage ramdisk that never had the binary.
    ap.add_argument("--keep-recovery", dest="drop_recovery", action="store_false",
                    help="do NOT remove /system/bin/recovery (only sane with a real "
                         "first stage ramdisk)")
    ap.add_argument("--ramdisk", default=None,
                    help="use this ramdisk (.gz) instead of the stock one: the "
                         "fstab is then expected to be inside it already "
                         "(see make-first-stage-ramdisk.py)")
    ap.add_argument("--ramdisk-out", default=None,
                    help="also write the patched ramdisk gzip here")
    args = ap.parse_args()

    orig = open(args.boot, "rb").read()
    fstab = open(args.fstab, "rb").read()
    h = parse_boot(orig)
    page = h["page"]

    kernel = orig[h["off_kernel"]:h["off_kernel"] + h["kernel_size"]]
    if args.kernel:
        kernel = open(args.kernel, "rb").read()

    ramdisk = orig[h["off_ramdisk"]:h["off_ramdisk"] + h["ramdisk_size"]]
    if args.ramdisk:
        # a ramdisk built from scratch already carries its own fstab
        ramdisk_new = open(args.ramdisk, "rb").read()
        print("  ramdisk src   : %s (%d bytes)" % (args.ramdisk, len(ramdisk_new)))
    else:
        cpio = gzip.decompress(ramdisk) if ramdisk[:2] == b"\x1f\x8b" else ramdisk
        cpio_before = cpio
        if args.drop_recovery:
            cpio = cpio_drop(cpio, {"system/bin/recovery"})
        # fs_mgr mkdir()s single component mount points, but nested ones like
        # /mnt/vendor/persist-lg must exist. Both roots are populated because
        # with force_normal_boot set init SwitchRoot()s into the
        # first_stage_ramdisk subtree *before* it reads the fstab, while
        # without the flag the ramdisk root stays live.
        mount_points = ["system", "vendor", "oem", "oem/OP",
                        "mnt", "mnt/vendor", "mnt/vendor/persist-lg"]
        for mnt in mount_points:
            cpio = cpio_insert(cpio, mnt, b"", 0o040755)
        # the kernel's initramfs unpacker does not create missing parents, so
        # the directory itself has to be an entry too
        cpio = cpio_insert(cpio, "first_stage_ramdisk", b"", 0o040755)
        for mnt in mount_points:
            cpio = cpio_insert(cpio, "first_stage_ramdisk/" + mnt, b"", 0o040755)
        cpio = cpio_insert(cpio, FSTAB_NAME, fstab)
        cpio = cpio_insert(cpio, "first_stage_ramdisk/" + FSTAB_NAME, fstab)
        ramdisk_new = gzip.compress(cpio, 9, mtime=0)

    # header is copied verbatim except for the sizes and the cmdline
    cmdline_field = orig[0x40:0x240].split(b"\x00", 1)[0]
    if args.cmdline_append and args.cmdline_append.encode() not in cmdline_field:
        cmdline_field = (cmdline_field + b" " + args.cmdline_append.encode()).strip()
        print("  cmdline       : %s" % cmdline_field.decode("utf-8", "replace"))
    # header is copied verbatim except for the two sizes we change
    out = bytearray(orig[:page])
    if args.cmdline_append:
        out[0x40:0x240] = cmdline_field.ljust(0x200, b"\x00")[:0x200]
    struct.pack_into("<I", out, 0x08, len(kernel))
    struct.pack_into("<I", out, 0x10, len(ramdisk_new))
    out += kernel
    out += b"\x00" * ((-len(out)) % page)
    out += ramdisk_new
    out += b"\x00" * ((-len(out)) % page)
    # whatever sits after the ramdisk (second stage / dt stub) is kept as-is
    out += orig[h["off_second"]:h["image_end"]]
    boot_img = bytes(out)
    if len(boot_img) != align(len(boot_img), page):
        boot_img += b"\x00" * ((-len(boot_img)) % page)

    footer = avb_footer(orig)
    if footer is None:
        part = boot_img
        note = "no AVB footer, written as a plain boot image"
    else:
        # The vbmeta struct and its footer are moved after the new image; the
        # hashes inside stay stale, which only matters with verification on.
        # avbtool layout: [image][padding] vbmeta [padding] footer(64) at the end.
        vbmeta = orig[footer["vbmeta_offset"]:
                      footer["vbmeta_offset"] + footer["vbmeta_size"]]
        footer_off = len(orig) - 64
        voff = footer_off - len(vbmeta)
        voff -= voff % 64
        if len(boot_img) > voff:
            sys.exit("boot image (%d) no longer fits before the vbmeta area (%d)"
                     % (len(boot_img), voff))
        footer_bytes = bytearray(orig[-64:])
        struct.pack_into(">IIQQQ", footer_bytes, 4, footer["major"],
                         footer["minor"], len(boot_img), voff, len(vbmeta))
        part = bytearray(boot_img)
        part += b"\x00" * (voff - len(part))
        part += vbmeta
        part += b"\x00" * (footer_off - len(part))
        part += footer_bytes
        part = bytes(part)
        note = ("AVB footer moved: vbmeta %d -> %d (hashes stale, "
                "verification must be off)" % (footer["vbmeta_offset"], voff))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    open(args.out, "wb").write(part)
    if args.ramdisk_out:
        os.makedirs(os.path.dirname(args.ramdisk_out), exist_ok=True)
        open(args.ramdisk_out, "wb").write(ramdisk_new)

    print("boot        : %s (%d bytes)" % (args.boot, len(orig)))
    print("kernel      : %s (%d bytes)" %
          (args.kernel if args.kernel else "stock, kept", len(kernel)))
    print("fstab       : %s (%d bytes)" % (args.fstab, len(fstab)))
    if args.ramdisk:
        print("ramdisk     : stock %d -> custom %d bytes"
              % (len(ramdisk), len(ramdisk_new)))
    else:
        print("ramdisk     : %d -> %d bytes (cpio %d -> %d)"
              % (len(ramdisk), len(ramdisk_new), len(cpio_before), len(cpio)))
    print("boot image  : %d -> %d bytes" % (h["image_end"], len(boot_img)))
    print("output      : %s (%d bytes)" % (args.out, len(part)))
    print("              sha256 %s" % hashlib.sha256(part).hexdigest())
    print("note        : %s" % note)
    if len(part) != len(orig):
        sys.exit("refusing to write: partition size changed (%d != %d)"
                 % (len(part), len(orig)))

    # self check: re-parse what we just built
    h2 = parse_boot(part)
    if h2["ramdisk_size"] != len(ramdisk_new):
        sys.exit("self check failed: header ramdisk_size is %d, expected %d"
                 % (h2["ramdisk_size"], len(ramdisk_new)))
    if part[h2["off_kernel"]:h2["off_kernel"] + h2["kernel_size"]] != kernel:
        sys.exit("self check failed: kernel payload mismatch")
    rd = part[h2["off_ramdisk"]:h2["off_ramdisk"] + h2["ramdisk_size"]]
    if FSTAB_NAME.encode() not in gzip.decompress(rd):
        sys.exit("self check failed: %s is not in the new ramdisk" % FSTAB_NAME)
    print("self check  : ok (kernel byte identical, %s embedded)" % FSTAB_NAME)


if __name__ == "__main__":
    main()
