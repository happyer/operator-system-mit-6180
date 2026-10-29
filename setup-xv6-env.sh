#!/usr/bin/env bash
#
# MIT 6.1810 / 6.S081 (xv6-labs-2025) 学习环境一键搭建 —— macOS
#
# 用法:
#     bash setup-xv6-env.sh              # 完整安装（工具链 + QEMU）
#     bash setup-xv6-env.sh --skip-qemu  # 只装 RISC-V 工具链
#
# 设计要点:
#   1) 官方推荐 Homebrew 的 riscv-software-src/riscv tap，但该 formula 只在
#      macOS 15+ 提供预编译包；macOS 14 及更早会从源码编译（约 6.5GB、数小时）。
#      本脚本改用 xPack 预编译包（约 316MB），几分钟即可完成。
#   2) xPack 的二进制前缀是 riscv-none-elf-，而 xv6 的 Makefile 会去探测
#      riscv64-unknown-elf-，且 xPack 默认架构是 rv32i/ilp32（软浮点），
#      链接器默认 elf32。脚本用「符号链接 + 参数注入 wrapper」补齐差异，
#      全程不修改 xv6 源码，保证 git diff 只包含你自己的实验代码。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$ROOT/tools"
XPACK_VER="${XPACK_VER:-14.2.0-3}"
SKIP_QEMU=0
[ "${1:-}" = "--skip-qemu" ] && SKIP_QEMU=1

blue()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m[!]\033[0m %s\n' "$*"; }
die()   { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 0. 平台检查
[ "$(uname -s)" = "Darwin" ] || die "本脚本面向 macOS。Linux 请直接: sudo apt install gcc-riscv64-linux-gnu qemu-system-misc"
ARCH="$(uname -m)"
case "$ARCH" in
  arm64)  XPACK_ARCH=darwin-arm64 ;;
  x86_64) XPACK_ARCH=darwin-x64  ;;
  *)      die "不支持的 CPU 架构: $ARCH" ;;
esac
MACOS_VER="$(sw_vers -productVersion)"
blue "平台: macOS $MACOS_VER / $ARCH"

# ------------------------------------------------- 1. RISC-V 交叉编译工具链
mkdir -p "$TOOLS"
TC_NAME="xpack-riscv-none-elf-gcc-$XPACK_VER"
if [ ! -x "$TOOLS/$TC_NAME/bin/riscv-none-elf-gcc" ]; then
  URL="https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v${XPACK_VER}/${TC_NAME}-${XPACK_ARCH}.tar.gz"
  blue "下载 RISC-V 工具链（约 316MB）:"
  echo "     $URL"
  curl -L --retry 3 -C - -o "$TOOLS/$TC_NAME.tar.gz" "$URL"
  blue "解压 ..."
  tar xzf "$TOOLS/$TC_NAME.tar.gz" -C "$TOOLS"
fi

BIN="$TOOLS/$TC_NAME/bin"
[ -x "$BIN/riscv-none-elf-gcc" ] || die "工具链不可用: $BIN/riscv-none-elf-gcc"

blue "生成 riscv64-unknown-elf-* 别名（xv6 Makefile 靠它自动探测工具链）..."
cd "$BIN"
for f in riscv-none-elf-*; do
  case "$f" in
    riscv-none-elf-gcc|riscv-none-elf-ld) continue ;;  # 这两个改用 wrapper
  esac
  ln -sf "$f" "riscv64-unknown-elf-${f#riscv-none-elf-}"
done

# xPack 默认 rv32i/ilp32 + ld 默认 elf32；xv6 需要 rv64gc/lp64d + elf64。
# 用 wrapper 只注入参数，不改 xv6 源码。
cat > riscv64-unknown-elf-gcc <<EOF
#!/bin/sh
exec "$BIN/riscv-none-elf-gcc" -march=rv64gc -mabi=lp64d "\$@"
EOF
cat > riscv64-unknown-elf-ld <<EOF
#!/bin/sh
exec "$BIN/riscv-none-elf-ld" -melf64lriscv "\$@"
EOF
# xv6 Makefile 中有 AS = \$(TOOLPREFIX)gas（实际未被引用），补上以防万一
cat > riscv64-unknown-elf-gas <<EOF
#!/bin/sh
exec "$BIN/riscv-none-elf-as" -march=rv64gc -mabi=lp64d "\$@"
EOF
chmod +x riscv64-unknown-elf-gcc riscv64-unknown-elf-ld riscv64-unknown-elf-gas

blue "工具链自检 ..."
PATH="$BIN:$PATH"
riscv64-unknown-elf-gcc --version | head -1 | sed 's/^/     /'
printf 'int answer(void){return 42;}\n' > "$TOOLS/.selftest.c"
riscv64-unknown-elf-gcc -mcmodel=medany -ffreestanding -c "$TOOLS/.selftest.c" -o "$TOOLS/.selftest.o"
if riscv64-unknown-elf-readelf -h "$TOOLS/.selftest.o" | grep -q 'ELF64'; then
  blue "✓ 工具链可用（能生成 ELF64 RISC-V 目标文件）"
else
  die "工具链自检失败"
fi
rm -f "$TOOLS/.selftest.c" "$TOOLS/.selftest.o"

# ------------------------------------------------------------------ 2. QEMU
if [ "$SKIP_QEMU" = "1" ]; then
  warn "已跳过 QEMU 安装（--skip-qemu）"
else
  if command -v qemu-system-riscv64 >/dev/null 2>&1; then
    blue "已存在 QEMU: $(qemu-system-riscv64 --version | head -1)"
  else
    command -v brew >/dev/null 2>&1 || die "未找到 Homebrew，无法安装 QEMU（也可用 Docker 方案，见 README.md）"
    warn "未找到 qemu-system-riscv64，将执行: brew install qemu"
    if [ "${MACOS_VER%%.*}" -lt 15 ]; then
      warn "macOS $MACOS_VER < 15：Homebrew 没有 qemu 预编译包，会从源码编译（约 20-40 分钟，请耐心等待）"
    fi
    brew install qemu
  fi
fi

# ------------------------------------------------------- 3. 生成激活脚本 env.sh
cat > "$ROOT/env.sh" <<EOF
# source 本文件以激活 xv6 开发环境:  source env.sh
export XV6_TOOLCHAIN="$BIN"
export PATH="\$XV6_TOOLCHAIN:/opt/homebrew/bin:/usr/local/bin:\$PATH"

# 便捷别名
alias xv6='cd "\$ROOT/xv6-labs-2025"'
EOF
blue "已生成 $ROOT/env.sh"

echo
blue "安装完成。后续使用:"
echo "     source $ROOT/env.sh"
echo "     cd $ROOT/xv6-labs-2025 && make qemu      # 启动 xv6（退出: Ctrl-A 然后 X）"
echo "     cd $ROOT/xv6-labs-2025 && make grade     # 跑当前 lab 的评分脚本"
