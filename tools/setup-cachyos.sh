#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The build's private LLVM 18 on CachyOS (or Arch Linux). Changes nothing outside this repository:
# no packages, no sudo, nothing on the system PATH. Other PS5 projects on the same PC rely on the
# system LLVM (23), and the Payload SDK takes any llvm-config-NN it finds on the PATH over it.
#
# Eden, FFmpeg and the boilerplate are compiled with LLVM 18 under Ubuntu's names (clang-18,
# ld.lld-18, ...). This writes those names as small wrappers into .local/toolchain/bin, which only
# this repository's build puts on its PATH (Makefile, tools/*.sh). They run a private, unpacked
# LLVM 18 (LLVM18_PREFIX, default ~/.local/opt/llvm18: Arch's llvm18, llvm18-libs, clang18, lld18
# and compiler-rt18 packages extracted there, not installed). Clang 18 cannot parse the system's
# GCC 16 libstdc++, so a host (Linux) compile uses a private GCC 14 (GCC14_PREFIX, default
# ~/.local/opt/gcc14: gcc14 and gcc14-libs extracted); a compile for another target (the PS5's
# x86_64-sie-ps5) passes through unchanged. The Payload SDK keeps the system LLVM: the build sets
# LLVM_CONFIG=/usr/bin/llvm-config. Run again at any time; it rewrites only .local/toolchain/bin.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
llvm18=${LLVM18_PREFIX:-$HOME/.local/opt/llvm18}
gcc14=${GCC14_PREFIX:-$HOME/.local/opt/gcc14}
llvm_bin=$llvm18/usr/lib/llvm18/bin
libs=$llvm18/usr/lib:$llvm18/usr/lib/llvm18/lib
gcc_dir=$(ls -d "$gcc14"/usr/lib/gcc/x86_64-pc-linux-gnu/14.* 2> /dev/null | head -n 1)
[[ -x $llvm_bin/clang ]] || { echo "no LLVM 18 at $llvm18 (set LLVM18_PREFIX)" >&2; exit 1; }
[[ -n $gcc_dir ]] || { echo "no GCC 14 at $gcc14 (set GCC14_PREFIX)" >&2; exit 1; }

bin=$root/.local/toolchain/bin
rm -rf "$bin"
mkdir -p "$bin"
wrap() {   # wrap NAME TOOL [host]: $bin/NAME runs the private LLVM 18's TOOL
    local host=
    [[ ${3:-} == host ]] && host="
for arg; do
    case \$arg in --target=*linux*) break ;; --target=*|-target) exec \"$llvm_bin/$2\" \"\$@\" ;; esac
done
set -- --gcc-install-dir=\"$gcc_dir\" \"\$@\""
    cat > "$bin/$1" <<EOF
#!/bin/sh
# Written by tools/setup-cachyos.sh: the private LLVM 18's $2.
export LD_LIBRARY_PATH="$libs\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"$host
exec "$llvm_bin/$2" "\$@"
EOF
    chmod 0755 "$bin/$1"
}
wrap clang-18 clang host
wrap clang++-18 clang++ host
for tool in ld.lld llvm-ar llvm-ranlib llvm-nm llvm-readobj llvm-objcopy llvm-objdump llvm-symbolizer; do
    wrap "$tool-18" "$tool"
done
wrap lld-18 ld.lld   # clang's -fuse-ld=lld-18
# Eden's FFmpeg step runs gmake; Arch's GNU make has no such name.
ln -sfn /usr/bin/make "$bin/gmake"

printf '#include <vector>\nint main() { std::vector<int> v; v.push_back(1); return v.empty(); }\n' |
    "$bin/clang++-18" -std=c++20 -x c++ - -o /dev/null ||
    { echo "clang++-18 cannot build a host C++ program with $gcc_dir" >&2; exit 1; }
echo "LLVM 18 (private): $("$bin/clang-18" --version | head -n 1)"
echo "Payload SDK LLVM (system): $(/usr/bin/llvm-config --version)"
env -i HOME="$HOME" PATH="$bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
    bash "$root/tools/check-toolchain.sh"
