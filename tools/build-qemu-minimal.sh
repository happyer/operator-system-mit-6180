#!/bin/sh
#
# 本地编译一个「最小化 QEMU」——只包含 riscv64-softmmu，专门给 xv6 用。
#
# 为什么需要这个脚本：
#   macOS 14 (Sonoma) 上 Homebrew 对所有 formula 都没有预编译包（bottle），
#   `brew install qemu` 会连带从源码编译 12+ 个依赖（gnutls / libssh / capstone /
#   ncurses / libusb / vde / dtc / meson / ...），耗时 1 小时以上。
#   而 xv6 只需要 `qemu-system-riscv64`，本脚本把不需要的功能全部关掉，
#   只依赖系统已有的 glib / pixman / ninja / pkgconf，10~20 分钟即可完成。
#
# libfdt(dtc)：qemu 的 riscv 机器需要 libfdt；这里使用 QEMU 内置 subproject
#   （源码包的 subprojects/dtc 已手工 clone 好），因此无需安装 Homebrew dtc。
#
# 用法:  sh build-qemu-minimal.sh
#
set -e

SRC=/Users/a1/Downloads/mit61810/tools/qemu-build/qemu-11.1.2
PREFIX=/Users/a1/Downloads/mit61810/tools/qemu-local
JOBS=$(sysctl -n hw.ncpu)

[ -d "$SRC" ] || { echo "错误: 找不到 QEMU 源码 $SRC"; exit 1; }

cd "$SRC"

echo "=== [1/3] configure (target=riscv64-softmmu, prefix=$PREFIX) ==="
./configure \
  --target-list=riscv64-softmmu \
  --prefix="$PREFIX" \
  --python=/opt/homebrew/bin/python3 \
  --ninja=/opt/homebrew/bin/ninja \
  --enable-fdt=internal \
  --disable-download \
  --disable-docs \
  --disable-guest-agent \
  --disable-tools \
  --disable-gtk \
  --disable-cocoa \
  --disable-sdl \
  --disable-vnc \
  --disable-curses \
  --disable-opengl \
  --disable-capstone \
  --disable-gnutls \
  --disable-libssh \
  --disable-slirp \
  --disable-vde \
  --disable-hvf \
  --disable-werror \
  --audio-drv-list=

echo "=== [2/3] make -j$JOBS ==="
make -j"$JOBS"

echo "=== [3/3] make install ==="
make install

echo "=== DONE ==="
"$PREFIX/bin/qemu-system-riscv64" --version
