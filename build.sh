#!/bin/sh

# ===== 路径配置 =====
CLANG_PATH=/home/ssx/clang-aosp/bin
GCC64_PATH=/home/ssx/gcc-64/bin
GCC32_PATH=/home/ssx/gcc-32/bin
OUTPUT_DIR=".out"
KERNEL_LOG=".out/build.log"

# 清理和准备输出目录
sudo rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

# 初始化日志文件
echo "======= 内核编译日志 $(date) =======" > "${KERNEL_LOG}"
echo "" >> "${KERNEL_LOG}"

# 设置编译环境
# 注意: CC=clang 必须作为 make 参数传入(Makefile 里 CC = $(CROSS_COMPILE)gcc
# 是 = 赋值, export 环境变量不生效, 会导致去找不存在的 aarch64-linux-android-gcc)
export PATH=$PATH:$CLANG_PATH
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=$GCC64_PATH/aarch64-linux-android-
export CROSS_COMPILE_ARM32=$GCC32_PATH/arm-linux-androideabi-

# 生成配置
echo "[$(date '+%Y-%m-%d %H:%M:%S')] 开始生成配置..." | tee -a "${KERNEL_LOG}"
make O="${OUTPUT_DIR}" ARCH=arm64 \
  CC=clang \
  CLANG_TRIPLE=aarch64-linux-gnu- \
  CROSS_COMPILE=${GCC64_PATH}/aarch64-linux-android- \
  STB_EM_defconfig 2>&1 | tee -a "${KERNEL_LOG}" || exit 1

# 执行编译
echo "" >> "${KERNEL_LOG}"
echo "[$(date '+%Y-%m-%d %H:%M:%S')] 开始编译内核..." | tee -a "${KERNEL_LOG}"
make -j$(nproc --all) \
  O="${OUTPUT_DIR}" \
  ARCH=arm64 \
  CC=clang \
  CLANG_TRIPLE=aarch64-linux-gnu- \
  CROSS_COMPILE=${GCC64_PATH}/aarch64-linux-android- 2>&1 | tee -a "${KERNEL_LOG}" || exit 1

echo "" >> "${KERNEL_LOG}"
echo "[$(date '+%Y-%m-%d %H:%M:%S')] 编译完成！" | tee -a "${KERNEL_LOG}"