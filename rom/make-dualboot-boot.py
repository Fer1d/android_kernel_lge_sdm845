#!/usr/bin/env python3
#
# 让 judyln 的 boot 分区同时支持「第二次启动（force_normal_boot 两阶段流程）」
# 和「恢复模式（OrangeFox）」——即 rec 与第二启动共存在同一个镜像里。
#
# 为什么能共存（都用真实镜像里的字符串核实过）：
#   * 正常启动时引导程序传 skip_initramfs，恢复启动时不传；
#   * 打开 CONFIG_INITRAMFS_IGNORE_SKIP_FLAG 后内核不再跳过 ramdisk，并把
#     skip_initramfs 改写成 want_initramf；
#   * CONFIG_PROC_CMDLINE_APPEND_ANDROID_FORCE_NORMAL_BOOT 只在看见
#     skip_initramfs 时才追加 androidboot.force_normal_boot=1。所以恢复启动
#     天然拿不到这个参数，不需要改内核；
#   * 正常启动：init 的 ForceNormalBoot() 把 /first_stage_ramdisk bind 到自己
#     再 SwitchRoot() 进去，随后 IsRecoveryMode() 查的是
#     first_stage_ramdisk/system/bin/recovery —— 那里我们不放恢复程序，
#     首阶段挂载照常执行，开机进 Android；
#   * 恢复启动：不切换，init 在 ramdisk 根看到 system/bin/recovery →
#     "First stage mount skipped (recovery mode)" → 进 OrangeFox。
#
# 做法：以 FOX 的 ramdisk 为基底（保留 sbin/recovery、twres、FFiles、
# etc/recovery.fstab 等全部 OrangeFox 内容），换上官方 A10 的 init 与其
# 运行时库（官方 ramdisk 的 system/bin/init + linker64 + system/lib64），
# 再把首阶段 fstab 和挂载点铺进 first_stage_ramdisk/。
#
# 用法：
#   rom/make-dualboot-boot.py [--fox ...] [--stock ...] [--kernel ...] [--out ...]

import argparse
import gzip
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FSTAB_NAME = "fstab.judyln"


def load_stage2():
    path = os.path.join(HERE, "patch-boot-stage2.py")
    spec = importlib.util.spec_from_file_location("pb_stage2", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


PB = load_stage2()


def cpio_read(data):
    ents = []
    off = 0
    while off + 110 <= len(data):
        if data[off:off + 6] not in (b"070701", b"070702"):
            break
        f = [int(data[off + 6 + 8 * i:off + 6 + 8 * (i + 1)], 16) for i in range(13)]
        mode, uid, gid, size, nsize = f[1], f[2], f[3], f[6], f[11]
        name = data[off + 110:off + 110 + nsize - 1].decode("utf-8", "replace")
        doff = (off + 110 + nsize + 3) // 4 * 4
        body = data[doff:doff + size]
        if name == "TRAILER!!!":
            break
        ents.append([name, mode, uid, gid, body])
        off = (doff + size + 3) // 4 * 4
    return ents


def cpio_write(ents):
    out = bytearray()
    for name, mode, uid, gid, body in ents:
        fields = (0, mode, uid, gid, 1, 0, len(body), 0, 0, 0, 0, len(name) + 1, 0)
        out += b"070701" + b"".join(b"%08x" % x for x in fields)
        out += name.encode() + b"\x00"
        out += b"\x00" * ((-len(out)) % 4)
        out += body
        out += b"\x00" * ((-len(out)) % 4)
    fields = (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 11, 0)
    out += b"070701" + b"".join(b"%08x" % x for x in fields)
    out += b"TRAILER!!!\x00"
    out += b"\x00" * ((-len(out)) % 4)
    return bytes(out)


def entries_of(boot_path):
    data = open(boot_path, "rb").read()
    h = PB.parse_boot(data)
    rd = data[h["off_ramdisk"]:h["off_ramdisk"] + h["ramdisk_size"]]
    if rd[:2] == b"\x1f\x8b":
        rd = gzip.decompress(rd)
    return data, h, cpio_read(rd)


def find(ents, name):
    for e in ents:
        if e[0] == name:
            return e
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fox", default="/home/ssx/LGE/LG-G7N30H-25.5.20/boot.img",
                    help="FOX/OrangeFox boot image whose ramdisk is the base")
    ap.add_argument("--stock", default="/home/ssx/LGE/judyln-stock.boot.img",
                    help="official judyln boot image (A10 init + its libs come from here)")
    ap.add_argument("--kernel", default="/home/ssx/android-kernel/.out/arch/arm64/boot/Image.gz-dtb")
    ap.add_argument("--fstab", default=os.path.join(HERE, FSTAB_NAME))
    ap.add_argument("--out", default="/home/ssx/LGE/out/boot_a.dualboot.img")
    ap.add_argument("--ramdisk-out", default=None)
    ap.add_argument("--cmdline-from", choices=("stock", "fox"), default="stock",
                    help="stock 用官方 cmdline（enforcing、无调试参数），fox 用 FOX 的")
    args = ap.parse_args()

    fox_data, fox_h, fox_ents = entries_of(args.fox)
    stock_data, stock_h, stock_ents = entries_of(args.stock)
    fstab = open(args.fstab, "rb").read()
    kernel = open(args.kernel, "rb").read()

    # FOX 的 /system 是符号链接（TWRP 惯例），A10 init 需要的文件要按它指向的真实
    # 路径放，否则 /init -> /system/bin/init 解析不到。
    sysent = find(fox_ents, "system")
    if sysent and (sysent[1] & 0o170000) == 0o120000:
        sysroot = sysent[4].decode().lstrip("/")
    else:
        sysroot = "system"
    print("  FOX 的 /system -> %s（A10 运行时放这里）" % sysroot)

    ents = [e for e in fox_ents if e[0] != "init"]
    old = find(ents, sysroot + "/bin/init")
    if old:
        ents.remove(old)
    ents = [e for e in ents if not e[0].startswith(sysroot + "/lib64/")]

    def put(name, body, mode):
        ents.append([name, mode, 0, 0, body])

    # 官方 A10 init 与它的运行时
    stock_init = find(stock_ents, "system/bin/init")
    stock_linker = find(stock_ents, "system/bin/linker64")
    if not stock_init or not stock_linker:
        sys.exit("官方 ramdisk 里找不到 system/bin/init 或 linker64")
    put(sysroot + "/bin", b"", 0o040755)
    put(sysroot + "/lib64", b"", 0o040755)
    put(sysroot + "/bin/init", stock_init[4], 0o100755)
    put(sysroot + "/bin/linker64", stock_linker[4], 0o100755)
    nlib = 0
    for e in stock_ents:
        if e[0].startswith("system/lib64/"):
            put(sysroot + "/lib64/" + e[0].split("/")[-1], e[4], e[1])
            nlib += 1
    print("  A10 init %d 字节，linker64 %d 字节，随行库 %d 个"
          % (len(stock_init[4]), len(stock_linker[4]), nlib))

    # /init -> /system/bin/init，恢复程序挂在 /system/bin/recovery -> /sbin/recovery
    put("init", b"/system/bin/init", 0o120777)
    put(sysroot + "/bin/recovery", b"/sbin/recovery", 0o120777)
    fox_rec = find(fox_ents, "sbin/recovery")
    print("  OrangeFox 恢复程序: %s"
          % ("sbin/recovery %d 字节" % len(fox_rec[4]) if fox_rec else "缺失！"))

    # 首阶段 fstab 与挂载点：只铺进 first_stage_ramdisk/（正常启动切换后使用），
    # ramdisk 根不放挂载点目录，避免破坏 FOX 的 system 符号链接。
    mounts = ["system", "vendor", "oem", "oem/OP", "mnt", "mnt/vendor",
              "mnt/vendor/persist-lg", "dev", "proc", "sys"]
    put("first_stage_ramdisk", b"", 0o040755)
    for m in mounts:
        put("first_stage_ramdisk/" + m, b"", 0o040755)
    put(FSTAB_NAME, fstab, 0o100644)
    put("first_stage_ramdisk/" + FSTAB_NAME, fstab, 0o100644)

    cpio = cpio_write(ents)
    ramdisk = gzip.compress(cpio, 9, mtime=0)
    print("  ramdisk: %d 个条目，%d 字节（压缩后）" % (len(ents), len(ramdisk)))

    page = fox_h["page"]
    src = fox_data if args.cmdline_from == "fox" else stock_data
    cmdline = src[0x40:0x240].split(b"\x00", 1)[0]
    out = bytearray(fox_data[:page])
    out[0x40:0x240] = cmdline.ljust(0x200, b"\x00")[:0x200]
    struct.pack_into("<I", out, 0x08, len(kernel))
    struct.pack_into("<I", out, 0x10, len(ramdisk))
    out += kernel
    out += b"\x00" * ((-len(out)) % page)
    out += ramdisk
    out += b"\x00" * ((-len(out)) % page)
    out += fox_data[fox_h["off_second"]:fox_h["image_end"]]
    boot_img = bytes(out)
    if len(boot_img) != PB.align(len(boot_img), page):
        boot_img += b"\x00" * ((-len(boot_img)) % page)
    print("  cmdline: %s" % cmdline.decode("utf-8", "replace"))

    footer = PB.avb_footer(fox_data)
    if footer is None:
        part = boot_img
        print("  无 AVB footer，按裸 boot 镜像写出")
    else:
        vbmeta = fox_data[footer["vbmeta_offset"]:
                          footer["vbmeta_offset"] + footer["vbmeta_size"]]
        footer_off = len(fox_data) - 64
        voff = footer_off - len(vbmeta)
        voff -= voff % 64
        if len(boot_img) > voff:
            sys.exit("镜像 %d 字节放不进 vbmeta 之前的 %d 字节空间" % (len(boot_img), voff))
        fb = bytearray(fox_data[-64:])
        struct.pack_into(">IIQQQ", fb, 4, footer["major"], footer["minor"],
                         len(boot_img), voff, len(vbmeta))
        part = bytearray(boot_img)
        part += b"\x00" * (voff - len(part))
        part += vbmeta
        part += b"\x00" * (footer_off - len(part))
        part += fb
        part = bytes(part)
        print("  AVB footer 已搬移: vbmeta %d -> %d（哈希失效，校验必须关闭）"
              % (footer["vbmeta_offset"], voff))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    open(args.out, "wb").write(part)
    if args.ramdisk_out:
        open(args.ramdisk_out, "wb").write(ramdisk)
    print("内核    : %d 字节" % len(kernel))
    print("输出    : %s（%d 字节，分区上限 67108864）" % (args.out, len(part)))
    if len(part) > 67108864:
        sys.exit("超过 boot 分区大小！")


if __name__ == "__main__":
    main()
