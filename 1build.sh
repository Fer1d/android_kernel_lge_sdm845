#!/bin/sh

# ============================================================
# 安卓内核编译脚本 (Clang 18 + ThinLTO 版)
# Clang 版本: r510928 (基于 LLVM 18 工具链)
# 目标架构: arm64
# 特性: 启用 clang ThinLTO (CONFIG_LTO_CLANG_THIN=y)
#   - 编译:  -flto=thin (由 CONFIG_LTO_CLANG_THIN 自动加入 KBUILD_CFLAGS)
#   - 链接:  中间 LTO 链接用 ld.lld (Makefile 自动),最终 vmlinux 由原链接器
#            链接;内核对象为 LLVM IR bitcode, 在 LTO vmlinux.o 阶段统一优化
#   - 归档:  llvm-ar / llvm-nm (thin archives + IR 符号表)
# ============================================================

# ===== 路径配置 =====
CLANG_PATH=/home/ssx/toolchain/clang-r510928/bin
GCC64_PATH=/home/ssx/toolchain/gcc-64/bin
GCC32_PATH=/home/ssx/toolchain/gcc-32/bin
OUTPUT_DIR=".out"
KERNEL_LOG=".out/build.log"

# ===== 清理和准备输出目录 =====
rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

# ===== 初始化日志文件 =====
echo "======= 内核编译日志 $(date) =======" > "${KERNEL_LOG}"
echo "" >> "${KERNEL_LOG}"

# ===== 设置编译环境 =====
# 将 CLANG_PATH 放在 PATH 最前面,优先使用 LLVM 工具链 (含 ld.lld/llvm-ar/llvm-dis)
export PATH=$CLANG_PATH:$PATH
export CLANG_TRIPLE=aarch64-linux-gnu-

# 注意: CC/LD/AR 等变量在 Makefile 内会被显式赋值,
# 必须作为 make 参数传入(命令行覆盖优先级最高)。
# ThinLTO 必需: CC=clang 生成 LLVM IR, AR=llvm-ar 处理 thin archives / IR 符号表。
# 注意: 不要传 LD=ld.lld! Makefile 会自动让 LTO 步骤用 ld.lld,而最终 vmlinux
#       用 LDFINAL_vmlinux(原链接器)链接;显式 LD=ld.lld 会导致最终链接
#       也走 lld 并报 -shared/-pie 冲突。

# ===== 生成配置 (defconfig) =====
echo "[$(date '+%Y-%m-%d %H:%M:%S')] 开始生成配置 (STB_EM_defconfig)..." | tee -a "${KERNEL_LOG}"
make O="${OUTPUT_DIR}" \
     ARCH=arm64 \
     CC=clang \
     CLANG_TRIPLE=aarch64-linux-gnu- \
     CROSS_COMPILE=${GCC64_PATH}/aarch64-linux-android- \
     CROSS_COMPILE_ARM32=${GCC32_PATH}/arm-linux-androideabi- \
     LLVM_IAS=1 \
     STB_EM_defconfig 2>&1 | tee -a "${KERNEL_LOG}" || exit 1

# ===== 检查 LTO 是否启用 =====
if grep -q '^CONFIG_LTO_CLANG_THIN=y' "${OUTPUT_DIR}/.config"; then
  echo "[ThinLTO] CONFIG_LTO_CLANG_THIN=y - ThinLTO 已启用" | tee -a "${KERNEL_LOG}"
else
  echo "[ThinLTO] 警告: 未检测到 CONFIG_LTO_CLANG_THIN=y, 请检查 defconfig" | tee -a "${KERNEL_LOG}"
fi

# ===== 编译内核 =====
echo "" >> "${KERNEL_LOG}"
echo "[$(date '+%Y-%m-%d %H:%M:%S')] 开始编译内核 (使用 $(nproc) 个并行任务, ThinLTO)..." | tee -a "${KERNEL_LOG}"
make -j$(nproc) \
     O="${OUTPUT_DIR}" \
     ARCH=arm64 \
     CC=clang \
     CLANG_TRIPLE=aarch64-linux-gnu- \
     CROSS_COMPILE=${GCC64_PATH}/aarch64-linux-android- \
     CROSS_COMPILE_ARM32=${GCC32_PATH}/arm-linux-androideabi- \
     AR=llvm-ar \
     NM=llvm-nm \
     OBJCOPY=llvm-objcopy \
     OBJDUMP=llvm-objdump \
     READELF=llvm-readelf \
     STRIP=llvm-strip \
     LLVM_IAS=1 \
     KCFLAGS="-O3 -Wno-error=strict-prototypes" \
     2>&1 | tee -a "${KERNEL_LOG}" || exit 1

# ===== 编译完成 =====
echo "" >> "${KERNEL_LOG}"
echo "[$(date '+%Y-%m-%d %H:%M:%S')] 编译完成!内核映像位于 ${OUTPUT_DIR}/arch/arm64/boot/Image" | tee -a "${KERNEL_LOG}"
echo "完整日志保存在: ${KERNEL_LOG}"
