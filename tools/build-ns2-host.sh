#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux (PC) build of the Switch 2 research tree (make ns2-host), for traces in about a minute
# without the console. It mirrors the tree make ns2 prepared into its own folder, so neither
# build disturbs the other, and uses no renderer (Settings::RendererBackend::Null): the PC's
# GPU is never touched. Run a game with tools/run-ns2-host.sh.
set -euo pipefail
export PATH=${PROSPERO_TOOLCHAIN_BIN:+$PROSPERO_TOOLCHAIN_BIN:}/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
scratch=$(cat .local/headless-cache)
[[ -f $scratch/source/.applied-patches ]] || { echo "Prepare the Switch 2 tree first (make ns2-host does)" >&2; exit 1; }
host="$scratch/ns2-host"
mkdir -p "$host/build" build/ns2-host
# Same mtimes as the PS5 tree: only what changed there recompiles here.
rsync -a --delete "$scratch/source/" "$host/source/"
python3 -B tools/patch-host-ffmpeg.py "$host/source" "$host/build"
cmake -S "$host/source" -B "$host/build" -G Ninja \
    -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=OFF -DCMAKE_EXE_LINKER_FLAGS= -DEDEN_HOST_ASAN=OFF \
    -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" \
    -DCMAKE_PROJECT_yuzu_INCLUDE="$root/headless/inject.cmake" \
    -DPROSPERO_DIRECT_FRONTEND_ROUTE=ON -DPROSPERO_DIRECT_ALLOW_EXECUTE=ON \
    -DENABLE_QT=OFF -DYUZU_CMD=OFF -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF \
    -DYUZU_TESTS=OFF -DBUILD_TESTING=OFF -DENABLE_OPENGL=OFF -DENABLE_CUBEB=OFF \
    -DENABLE_WEB_SERVICE=OFF -DENABLE_LIBUSB=OFF -DYUZU_CRASH_DUMPS=OFF -DEDEN_HOST_VULKAN=ON \
    -DYUZU_USE_EXTERNAL_FFMPEG=ON -DENABLE_WERROR=OFF -Dzstd_FORCE_BUNDLED=ON \
    -DVulkanHeaders_FORCE_BUNDLED=ON -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
    -DDYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=ON -DDYNARMIC_IGNORE_ASSERTS=OFF \
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_RENDER=OFF -DSDL_GPU=OFF
cmake --build "$host/build" --target eden-headless -j "${EDEN_BUILD_JOBS:-$(nproc)}"
cp "$host/build/bin/eden-headless" build/ns2-host/eden-headless
echo "== Built build/ns2-host/eden-headless"
