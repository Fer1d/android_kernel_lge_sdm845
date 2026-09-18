#!/usr/bin/env python3
#
# Turn the tree's dtbo.img (the Android DTBO image the build produces) into a
# flashable dtbo_a / dtbo_b partition image for judyln.
#
# The partition on the device is 8 MiB, the built dtbo.img is ~3.7 MiB and has
# no AVB metadata, so this pads it out with the vbmeta blob + footer taken from
# the stock partition dump (like patch-boot-stage2.py does for boot).
#
# Usage:
#   pack-dtbo-partition.py [--dtbo .out/arch/arm64/boot/dtbo.img]
#                          [--template /home/ssx/LGE/out/dtbo_a.orig.img]
#                          [--out /home/ssx/LGE/out/dtbo_a.stage2.img]

import argparse
import hashlib
import os
import struct
import sys

DT_TABLE_MAGIC = 0xD7B7AB1E
AVB_MAGIC = b"AVBf"


def parse_dtbo_image(d):
    magic, total_size, header_size, entry_size, count, entries_off, page, ver = \
        struct.unpack_from(">8I", d, 0)
    if magic != DT_TABLE_MAGIC:
        sys.exit("not a DTBO image (magic 0x%08x)" % magic)
    entries = []
    for i in range(count):
        off = entries_off + i * entry_size
        dt_size, dt_offset, dt_id, dt_rev = struct.unpack_from(">4I", d, off)
        entries.append((dt_size, dt_offset, dt_id, dt_rev))
    return {
        "total_size": total_size, "header_size": header_size,
        "entry_size": entry_size, "count": count,
        "entries_off": entries_off, "page": page, "version": ver,
        "entries": entries,
    }


def avb_footer(d):
    t = d[-64:]
    if t[:4] != AVB_MAGIC:
        return None
    major, minor, orig, voff, vsize = struct.unpack_from(">IIQQQ", t, 4)
    return {"major": major, "minor": minor, "orig": orig,
            "vbmeta_offset": voff, "vbmeta_size": vsize}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--dtbo", default=os.path.join(
        here, "..", ".out", "arch", "arm64", "boot", "dtbo.img"))
    ap.add_argument("--template", default="/home/ssx/LGE/out/dtbo_a.orig.img",
                    help="stock partition dump to take size and AVB area from")
    ap.add_argument("--out", default="/home/ssx/LGE/out/dtbo_a.stage2.img")
    args = ap.parse_args()

    img = open(args.dtbo, "rb").read()
    tpl = open(args.template, "rb").read()
    info = parse_dtbo_image(img)
    print("dtbo image  : %s (%d bytes, %d entries, image size %d)"
          % (args.dtbo, len(img), info["count"], info["total_size"]))
    for size, offset, dt_id, rev in info["entries"][:6]:
        model = ""
        if size:
            fdt = img[offset:offset + size]
            if fdt[:4] == b"\xd0\x0d\xfe\xed":
                model = fdt[36:36 + 32].split(b"\x00")[0].decode("utf-8", "replace")
        print("   entry id=0x%08x size=%-8d offset=%-9d %s" % (dt_id, size, offset, model))

    if len(img) > len(tpl):
        sys.exit("built dtbo (%d) does not fit the partition (%d)" % (len(img), len(tpl)))

    footer = avb_footer(tpl)
    out = bytearray(img)
    out += b"\x00" * (len(tpl) - len(out))
    if footer is None:
        print("no AVB footer in the template, writing a plain padded image")
    else:
        # keep the stock vbmeta area and footer where they are; the dtbo image
        # no longer matches its hash, which only matters if verification is on
        vbmeta = tpl[footer["vbmeta_offset"]:footer["vbmeta_offset"] + footer["vbmeta_size"]]
        footer_off = len(tpl) - 64
        voff = footer_off - len(vbmeta)
        voff -= voff % 64
        if len(img) > voff:
            sys.exit("dtbo image (%d) would overlap the vbmeta area (%d)" % (len(img), voff))
        out = bytearray(img)
        out += b"\x00" * (voff - len(out))
        out += vbmeta
        out += b"\x00" * (footer_off - len(out))
        fb = bytearray(tpl[-64:])
        struct.pack_into(">IIQQQ", fb, 4, footer["major"], footer["minor"],
                         len(img), voff, len(vbmeta))
        out += fb
        print("AVB footer  : vbmeta %d -> %d (hashes stale, verification must be off)"
              % (footer["vbmeta_offset"], voff))

    if len(out) != len(tpl):
        sys.exit("refusing to write: size changed (%d != %d)" % (len(out), len(tpl)))
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    open(args.out, "wb").write(bytes(out))

    # self check: the image must still parse as a DTBO table
    check = parse_dtbo_image(bytes(out))
    if check["count"] != info["count"] or check["total_size"] != info["total_size"]:
        sys.exit("self check failed: header changed")
    print("template    : %s (%d bytes)" % (args.template, len(tpl)))
    print("output      : %s (%d bytes)" % (args.out, len(out)))
    print("              md5    %s" % hashlib.md5(bytes(out)).hexdigest())
    print("              sha256 %s" % hashlib.sha256(bytes(out)).hexdigest())
    print("self check  : ok (%d entries, DTBO header intact)" % check["count"])


if __name__ == "__main__":
    main()
