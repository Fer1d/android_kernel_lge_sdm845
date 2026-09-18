#!/bin/bash
# rom/repack-erofs-image.sh
# 把 LG 官方 ext4 system.img 原样重打包为 EROFS（judyln / sdm845 / Linux 4.9）
#
# 为什么必须 root：
#   1) 源镜像里的 uid/gid 不是当前用户（/init 是 0:2000，/system/bin/* 多为 0:2000）；
#   2) security.selinux 标签只能由 root 写入目标文件系统。
#   少了这两样，打出来的系统盘属主全错、且不带任何 SELinux 标签（enforcing 下会大量拒绝）。
#
# 用法：
#   sudo bash rom/repack-erofs-image.sh
#   SRC=/path/system.img OUT=/path/out.img COMP='-zlz4' sudo -E bash rom/repack-erofs-image.sh
#
# 可用变量：SRC OUT STAGE COMP KEEP（KEEP=0 完成后删除暂存目录）
set -euo pipefail

SRC=${SRC:-/home/ssx/LGE/LG-G7N30H-25.5.20/system.img}
STAGE=${STAGE:-/home/ssx/LGE/out/system-erofs-stage}
OUT=${OUT:-/home/ssx/LGE/out/system-judyln-erofs.img}
COMP=${COMP:--zlz4hc}
KEEP=${KEEP:-1}
MNT=/mnt/lge-system-src
PART_MAX=4756340736          # judyln system 分区字节数（TWRP 实测，镜像不得超过）

die() { echo "错误: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "必须 root 运行：sudo bash $0"
[ -f "$SRC" ] || die "找不到源镜像 $SRC"

# erofs-utils：优先用系统里的（1.6 起 xattr 处理已修好），否则用免 root 解出来的 1.4
XATTR_OPT=
if command -v mkfs.erofs >/dev/null 2>&1; then
    MKFS=$(command -v mkfs.erofs); FSCK=$(command -v fsck.erofs); DUMP=$(command -v dump.erofs)
    export LD_LIBRARY_PATH=
else
    EROFSROOT=${EROFSROOT:-/tmp/erofsroot/usr}
    MKFS=$EROFSROOT/bin/mkfs.erofs; FSCK=$EROFSROOT/bin/fsck.erofs; DUMP=$EROFSROOT/bin/dump.erofs
    export LD_LIBRARY_PATH=$EROFSROOT/lib/x86_64-linux-gnu
    # 1.4 的 xattr 容忍度默认只有 2，超过就静默停止写 xattr：实测 432 个带标签的
    # 文件只写进去 111 个，整盘标签几乎全丢（4.9 内核侧看不见任何标签）。
    # 放到足够大才会把所有条目的 security.selinux 都写进镜像。
    XATTR_OPT='-x 1000000'
fi
[ -x "$MKFS" ] || die "找不到 mkfs.erofs"
command -v python3 >/dev/null || die "缺 python3"

xattr_of() {
    python3 - "$1" <<'PY'
import os, sys
try:
    print(os.getxattr(sys.argv[1], 'security.selinux').decode().rstrip('\0'))
except Exception as e:
    print('(无标签: %s)' % e)
PY
}

echo "== 源镜像 =="
ls -l "$SRC"; md5sum "$SRC"

echo "== 挂载源镜像（只读 loop）=="
mkdir -p "$MNT"
mountpoint -q "$MNT" && umount "$MNT"
mount -o loop,ro "$SRC" "$MNT"
trap 'mountpoint -q "$MNT" && umount "$MNT"' EXIT

echo "== 复制到暂存目录（保留 uid/gid/权限/硬链接/xattr/SELinux 标签）=="
rm -rf "$STAGE"; mkdir -p "$STAGE"
rsync -aHAX --numeric-ids "$MNT"/ "$STAGE"/

echo "== 标签与属主自检 =="
for f in system/bin/init system/bin/sh etc/init.rc init; do
    [ -e "$STAGE/$f" ] && printf '  %-18s %s  %s\n' "$f" "$(stat -c '%u:%g %a' "$STAGE/$f")" "$(xattr_of "$STAGE/$f")"
done
case "$(xattr_of "$STAGE/system/bin/init")" in
    *"无标签"*) die "暂存目录丢了 SELinux 标签，停止（检查 $STAGE 所在文件系统是否支持 security.* xattr）";;
esac

umount "$MNT"

echo "== 布局自检（system-as-root）=="
ls -ld "$STAGE/system" "$STAGE/product" "$STAGE/system/product" "$STAGE/init" 2>&1 | sed 's/^/  /'

echo "== 生成 EROFS（$COMP）=="
time "$MKFS" $XATTR_OPT $COMP "$OUT" "$STAGE"

echo "== 完整性校验 =="
"$FSCK" -p "$OUT" || die "fsck.erofs 报错"
"$DUMP" -s "$OUT"
"$DUMP" -S "$OUT" | sed -n '1,20p'

# 标签校验：EROFS 盘上的 xattr 名是「前缀索引 + 后缀」存的，security. 只占一个索引
# 字节，所以字面量 security.selinux 永远数不到（曾据此误判过一次）。可靠的量法是数
# 标签值里的 u:object_r:，并要求它不少于带标签的条目数；镜像里还会混入文件正文的
# 同名串，所以只做下界判断。
LABELED=$(python3 - "$STAGE" <<'PY'
import os, sys
root = sys.argv[1]
lab = 0
for dp, dns, fns in os.walk(root):
    for n in dns + fns:
        try:
            os.getxattr(os.path.join(dp, n), 'security.selinux', follow_symlinks=False)
            lab += 1
        except OSError:
            pass
print(lab)
PY
)
N=$(grep -a -o 'u:object_r:' "$OUT" | wc -l)
echo "  暂存目录带标签条目数: $LABELED"
echo "  镜像内 u:object_r: 出现: $N 次"
[ "$N" -ge "$LABELED" ] || die "镜像里的 SELinux 标签不足（$N < $LABELED）：mkfs 丢了 xattr，检查 xattr 容忍度"

SZ=$(stat -c %s "$OUT")
echo "== 结果 =="
ls -l "$OUT"; md5sum "$OUT"
echo "  大小 $SZ 字节  分区上限 $PART_MAX 字节"
[ "$SZ" -le "$PART_MAX" ] || die "镜像超过分区大小！"
[ "$KEEP" = 1 ] || { echo "  清理暂存目录"; rm -rf "$STAGE"; }
echo
echo "刷机（二选一，注意当前 active 槽）："
echo "  fastboot flash system_a $OUT     # ROM 在 A 槽时"
echo "  fastboot flash system_b $OUT     # 目前在 B 槽测试时"
