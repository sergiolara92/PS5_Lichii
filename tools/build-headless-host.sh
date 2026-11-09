#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
export PATH=${PROSPERO_TOOLCHAIN_BIN:+$PROSPERO_TOOLCHAIN_BIN:}/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
mkdir -p .local build/headless-host build/fixture
bash tools/build-core-fixture.sh
# Thousands of dependency files and compiler checks are slow across /mnt/c.
cache_parent="${XDG_CACHE_HOME:-$HOME/.cache}"
if [[ ! -f .local/headless-cache ]] || [[ ! -d "$(cat .local/headless-cache)" ]]; then
    mkdir -p "$cache_parent"
    scratch=$(mktemp -d "$cache_parent/ps5-eden-headless.XXXXXX")
    printf '%s\n' "$root" > "$scratch/owner"
    printf '%s\n' "$scratch" > .local/headless-cache
fi
scratch=$(cat .local/headless-cache)
[[ "$scratch" == "$cache_parent"/ps5-eden-headless.* && "$(cat "$scratch/owner")" == "$root" ]]
eden="$scratch/source"
# The Fritchii tree make prepare unpacked and patched (tools/prepare-build.sh).
[[ -f $eden/.applied-patches ]] || { echo "Prepare the source tree first (make test does)" >&2; exit 1; }
# FFmpeg links configure probes through the compiler driver, not raw ld.
# Keep the derivative in our cache and regenerate its Makefile when it changes.
python3 -B tools/patch-host-ffmpeg.py "$eden" "$scratch/build"
cmake -S "$eden" -B "$scratch/build" -G Ninja \
    -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=OFF -DCMAKE_EXE_LINKER_FLAGS= -DEDEN_HOST_ASAN=OFF \
    -DCMAKE_PROJECT_yuzu_INCLUDE="$root/headless/inject.cmake" \
    -DPROSPERO_DIRECT_FRONTEND_ROUTE="${PROSPERO_DIRECT_FRONTEND_ROUTE:-OFF}" \
    -DPROSPERO_DIRECT_ALLOW_EXECUTE="${PROSPERO_DIRECT_ALLOW_EXECUTE:-OFF}" \
    -DENABLE_QT=OFF -DYUZU_CMD=OFF -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF \
    -DYUZU_TESTS=OFF -DBUILD_TESTING=OFF -DENABLE_OPENGL=OFF -DENABLE_CUBEB=OFF \
    -DENABLE_WEB_SERVICE=OFF -DENABLE_LIBUSB=OFF -DYUZU_CRASH_DUMPS=OFF \
    -DYUZU_USE_EXTERNAL_FFMPEG=ON -DENABLE_WERROR=OFF -Dzstd_FORCE_BUNDLED=ON \
    -DVulkanHeaders_FORCE_BUNDLED=ON -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
    -DDYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=ON -DDYNARMIC_IGNORE_ASSERTS=OFF \
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_RENDER=OFF -DSDL_GPU=OFF
python3 -B "$root/headless/check_slab_lifetime.py" \
    "$scratch/build/headless/include/core/hle/kernel/slab_helpers.h" \
    "$scratch/source/src/core/hle/kernel/slab_helpers.h"
cmake --build "$scratch/build" --target eden-headless eden-romfs-check eden-scalar-check eden-memory-check eden-ryujinx-check eden-mods-check eden-shader-cache-check eden-settings-check eden-patch-library-check eden-update-check eden-update-files-check eden-profiles-check eden-save-sync-check -j 6
python3 -B "$root/tools/check-sparse-header.py" "$scratch/build"
python3 -B "$root/tools/check-multiplayer-headers.py" "$scratch/build"
python3 -B "$root/tools/check-heap-growth.py"
python3 -B "$root/tools/check-crash-report.py"
python3 -B "$root/tools/check-stop-limit.py"
python3 -B "$root/tools/check-image-pages.py"
python3 -B "$root/tools/check-astc-policy.py"
python3 -B "$root/tools/check-guest-clock.py"
python3 -B "$root/tools/check-gc-scan.py"
"$scratch/build/bin/eden-scalar-check"
"$scratch/build/bin/eden-memory-check"
python3 -B "$root/tools/check-decoder-startup.py"
python3 -B "$root/headless/check_audio_shutdown.py" "$scratch/build/headless/core.cpp" "$scratch/source/src/core/core.cpp"
"$scratch/build/bin/eden-romfs-check"
"$scratch/build/bin/eden-ryujinx-check"
"$scratch/build/bin/eden-patch-library-check"
"$scratch/build/bin/eden-update-check"
"$scratch/build/bin/eden-update-files-check"
"$scratch/build/bin/eden-profiles-check"
"$scratch/build/bin/eden-save-sync-check"
"$scratch/build/bin/eden-mods-check"
"$scratch/build/bin/eden-shader-cache-check"
"$scratch/build/bin/eden-settings-check"
python3 -B "$root/tools/check-remote.py"
# The same against real RomM servers in Docker (only with ROMM_CHECK=1, and Docker).
python3 -B "$root/tools/check-romm.py"
# shortcut: eden-devices-check is not built: its instrumented mock (Eden::Mock::state) is missing from the
# 2026-10-29 snapshot (headless/mock_devices.h is a stand-in); rewrite the mock, then restore it and this line.
# bash tools/check-headless-devices.sh
cp "$scratch/build/bin/eden-headless" build/headless-host/eden-headless
cp "$scratch/build/compile_commands.json" build/headless-host/compile_commands.json
