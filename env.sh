# xv6 / MIT 6.1810 开发环境激活脚本
# 用法:  source env.sh

_ROOT="/Users/a1/Downloads/mit61810"

# RISC-V 交叉工具链（xPack，已补齐 riscv64-unknown-elf-* 前缀与 rv64gc/lp64d 参数）
export XV6_TOOLCHAIN="$_ROOT/tools/xpack-riscv-none-elf-gcc-14.2.0-3/bin"

# 本地编译的 QEMU（仅 riscv64-softmmu，见 tools/build-qemu-minimal.sh）
export XV6_QEMU="$_ROOT/tools/qemu-local/bin"

# 独立 venv，仅用于 `make grade`（grade 脚本需要 pexpect）
export XV6_VENV="$_ROOT/tools/venv/bin"

export PATH="$XV6_VENV:$XV6_TOOLCHAIN:$XV6_QEMU:/opt/homebrew/bin:/usr/local/bin:$PATH"

# 便捷别名
alias xv6='cd /Users/a1/Downloads/mit61810/xv6-labs-2025'
alias xv6book='open /Users/a1/Downloads/mit61810/docs/book-riscv-rev5.pdf'

# 快速自检
xv6-check() {
  echo "--- RISC-V compiler ---"
  command -v riscv64-unknown-elf-gcc && riscv64-unknown-elf-gcc --version | head -1
  echo "--- QEMU ---"
  command -v qemu-system-riscv64 && qemu-system-riscv64 --version | head -1
  echo "--- xv6 source ---"
  ls -d /Users/a1/Downloads/mit61810/xv6-labs-2025 && grep -h '^LAB' /Users/a1/Downloads/mit61810/xv6-labs-2025/conf/lab.mk
  echo "--- grade 依赖 (pexpect) ---"
  python3 -c "import pexpect;print('pexpect',pexpect.__version__,'OK')" 2>&1 | tail -1
}
