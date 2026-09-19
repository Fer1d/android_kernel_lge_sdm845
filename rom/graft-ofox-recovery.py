#!/usr/bin/env python3
#
# 把 OrangeFox 作为「恢复负载」移植进已经能正常开机的镜像里。
#
# 前提（已实测）：该镜像正常启动走 ramdisk 首阶段挂载、恢复启动走 ramdisk 根目录下的
# system/bin/recovery。所以只需要替换恢复模式用到的那批文件，正常启动毫不受影响：
#   * init.rc 与 init.recovery.*.rc 只在恢复模式被解析（正常启动切换根之后用的是系统里的
#     /system/etc/init/hw/init.rc），替换它们是安全的；
#   * OrangeFox 的库全部走 /sbin（它的二进制没有 RUNPATH，靠 init.rc 里的
#     export LD_LIBRARY_PATH /sbin）。官方 ramdisk 的 sbin 里只有一个 bnrd，不会撞名；
#     而 /system/lib64 会和 A10 init 撞 6 个库名，所以绝不能把 FOX 的库放那里。
#
# 用法：
#   rom/graft-ofox-recovery.py [--base ...] [--fox ...] [--out ...]

import argparse
import gzip
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load_patcher():
    spec = importlib.util.spec_from_file_location(
        "pb_stage2", os.path.join(HERE, "patch-boot-stage2.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


PB = load_patcher()


def cpio_read(data):
    ents, off = [], 0
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
    out += b"070701" + b"".join(b"%08x" % x for x in fields) + b"TRAILER!!!\x00"
    out += b"\x00" * ((-len(out)) % 4)
    return bytes(out)


def ramdisk_of(path):
    d = open(path, "rb").read()
    h = PB.parse_boot(d)
    rd = d[h["off_ramdisk"]:h["off_ramdisk"] + h["ramdisk_size"]]
    return (gzip.decompress(rd) if rd[:2] == b"\x1f\x8b" else rd), d, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="/home/ssx/LGE/out/boot_a.stage2-rec.img",
                    help="已经能正常开机的那张镜像（正常启动走首阶段挂载）")
    ap.add_argument("--fox", default="/home/ssx/LGE/FOX.img",
                    help="OrangeFox 镜像，恢复负载从这里取")
    ap.add_argument("--kernel", default=None, help="默认沿用 base 里的内核")
    ap.add_argument("--out", default="/home/ssx/LGE/out/boot_a.ofox.img")
    ap.add_argument("--ramdisk-out", default="/home/ssx/LGE/out/ramdisk-ofox.gz")
    args = ap.parse_args()

    base_cpio, base_data, base_h = ramdisk_of(args.base)
    fox_cpio, _, _ = ramdisk_of(args.fox)
    base = cpio_read(base_cpio)
    fox = cpio_read(fox_cpio)

    # 删除 base 里恢复模式才会用到的文件，避免和 FOX 的重名冲突
    drop = {"system/bin/recovery", "init.rc", "system/etc/recovery.fstab"}
    dropped = []
    keep = []
    for e in base:
        n = e[0].rstrip("/")
        if n in drop or (n.startswith("init.recovery.") and n.endswith(".rc")):
            dropped.append(n)
            continue
        keep.append(e)
    print("  从 base 移除恢复负载: %s" % ", ".join(sorted(set(dropped))))

    # 从 FOX 取：整个 sbin（工具+库，走 LD_LIBRARY_PATH=/sbin）、主题、附加工具、
    # 恢复模式的 rc、以及 recover 二进制与 fstab
    want_dirs = ("sbin/", "twres/", "FFiles/")
    src = {}
    for e in fox:
        n = e[0]
        if n.startswith(want_dirs) or n in ("sbin", "twres", "FFiles"):
            src[n] = e
    src["init.rc"] = next(e for e in fox if e[0] == "init.rc")
    for e in fox:
        n = e[0].rstrip("/")
        if n.startswith("init.recovery.") and n.endswith(".rc"):
            src[e[0]] = e
    rec = next((e for e in fox if e[0].rstrip("/") == "sbin/recovery"), None)
    if rec is None:
        sys.exit("FOX 镜像里找不到 sbin/recovery")
    for a, b in (("etc/recovery.fstab", "system/etc/recovery.fstab"),
                 ("etc/twrp.fstab", "system/etc/twrp.fstab")):
        e = next((x for x in fox if x[0].rstrip("/") == a), None)
        if e:
            src[b] = [b, e[1], e[2], e[3], e[4]]
            print("  %s <- FOX 的 %s" % (b, a))
    src["system/bin/recovery"] = ["system/bin/recovery", 0o100755, rec[2], rec[3], rec[4]]
    print("  system/bin/recovery <- OrangeFox 的 sbin/recovery（%d 字节）" % len(rec[4]))

    # 策略与上下文也要换成 FOX 那套：恢复模式用的是这个 ramdisk 自己加载的策略
    # （selinux_setup 读 /sepolicy），LG 的 A10 策略里没有 TWRP 需要的规则（最典型的是
    # recovery 域读 /sbin 下 rootfs 标签文件、以及 TWRP 自己的域/类型）。正常启动不受
    # 影响：切换根之后内核会 FreeRamdisk 把整个 ramdisk 释放，用的是系统分区的策略。
    policy = ["sepolicy", "file_contexts", "file_contexts.bin",
              "plat_file_contexts", "plat_property_contexts", "plat_seapp_contexts",
              "plat_service_contexts", "plat_hwservice_contexts",
              "vendor_file_contexts", "vendor_property_contexts", "vendor_seapp_contexts",
              "vendor_service_contexts", "vendor_hwservice_contexts", "vndservice_contexts"]
    npol = 0
    for nm in policy:
        e = next((x for x in fox if x[0].rstrip("/") == nm), None)
        if e is not None:
            src[nm] = e
            npol += 1
    print("  SELinux 策略/上下文替换 %d 个（恢复模式加载 FOX 那套）" % npol)

    # 恢复服务的可执行文件必须是 /system/bin/recovery：LG 的 SELinux 策略里只有这个路径
    # 带 recovery_exec 标签，init 才能把服务从 init 域转到 recovery 域。FOX 的 rc 指向
    # /sbin/recovery（上下文标签是 rootfs），转换不成立，服务会被拒绝而后静默起不来 ——
    # 现象就是黑屏。FOX 自己的镜像能跑是因为它 cmdline 带 androidboot.selinux=permissive，
    # 而那个会连正常开机一起放松，不能用。
    svc = src.get("init.recovery.service.rc")
    if svc is not None and b"/sbin/recovery" in svc[4]:
        svc[4] = svc[4].replace(b"/sbin/recovery", b"/system/bin/recovery")
        print("  恢复服务改为从 /system/bin/recovery 启动（换到 recovery_exec 标签）")

    ents = keep + [e for k, e in sorted(src.items())]

    # 内核解包器不会自动建中间目录，给每个新增路径补齐父目录条目
    have = {e[0].rstrip("/") for e in ents}
    parents = []
    for e in src.values():
        parts = e[0].rstrip("/").split("/")[:-1]
        for i in range(1, len(parts) + 1):
            p = "/".join(parts[:i])
            if p and p not in have:
                have.add(p)
                parents.append([p, 0o040755, 0, 0, b""])
    if parents:
        print("  补建父目录 %d 个" % len(parents))

    cpio = cpio_write(parents + ents)
    ramdisk = gzip.compress(cpio, 9, mtime=0)
    open(args.ramdisk_out, "wb").write(ramdisk)
    print("  新 ramdisk: %d 条目，cpio %d 字节，压缩后 %d 字节"
          % (len(ents) + len(parents), len(cpio), len(ramdisk)))

    cmd = [sys.executable, os.path.join(HERE, "patch-boot-stage2.py"),
           "--boot", args.base, "--ramdisk", args.ramdisk_out,
           "--cmdline-append", "loglevel=8 printk.devkmsg=on", "--out", args.out]
    if args.kernel:
        cmd += ["--kernel", args.kernel]
    print("  打包：%s" % " ".join(os.path.basename(c) if "/" in c else c for c in cmd))
    os.execv(sys.executable, cmd)


if __name__ == "__main__":
    main()
