#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs a game in the Linux build of the Switch 2 tree (make ns2-host), on the PC:
#   tools/run-ns2-host.sh <keys folder> <firmware NCA folder> <game folder with exefs/ and romfs/, or .xci/.nsp/.nup>
# Your files are only read. The app expects /data/prosperoeden, so it runs in a private sandbox
# (bubblewrap, no root) where that path is a folder of this checkout's build cache; nothing is
# created outside it. Diagnostics come from the environment as on the console (EDEN_LOG_FILTER,
# EDEN_HOST_SVC_TRACE, EDEN_GUEST_PROBE, ...). Output: <run>/stderr.log and <run>/user/log/.
# EDEN_HOST_VULKAN=1 renders with the PC's GPU in a window; EDEN_HOST_CAPTURE="200,260" then saves
# the frames at those seconds as <run>/frame-<seconds>.ppm. NS2_WRAPPER runs the app under a
# command (e.g. "gdb -batch -ex run -ex bt --args").
# A run ends at a guest fault or after NS2_RUN_SECONDS (120).
set -euo pipefail
[[ $# -eq 3 ]] || { sed -n 3,4p "$0" >&2; exit 2; }
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
keys=$(realpath "$1") firmware=$(realpath "$2") game=$(realpath "$3")
mount=/data/prosperoeden/roms/game
# A single-file game (card image, submission package or NUP) is mounted as one file.
single=false
case $game in *.xci|*.nsp|*.nup) single=true; mount=/data/prosperoeden/roms/game.${game##*.} ;; esac
for need in "$keys/prod.keys" "$($single && echo "$game" || echo "$game/exefs/main")"; do [[ -f $need ]] || { echo "Missing $need" >&2; exit 2; }; done
# NS2_BINARY: another build of this checkout (a PGO training or ASan build).
binary="${NS2_BINARY:-$root/build/ns2-host/eden-headless}"
[[ -x $binary ]] || { echo "Build it first: make ns2-host" >&2; exit 2; }
run="$(cat "$root/.local/headless-cache")/ns2-host/run"
mkdir -p "$run/user/nand/system/Contents/registered" "$run/user/keys" "$run/data/roms/game" "$run/data/keys"
export EDEN_HOST_MEMORY_12GB=${EDEN_HOST_MEMORY_12GB-1}
# The Switch 2 route loads a game from a .eden descriptor (as on the console), written here for
# the sandbox's paths (no research profiles: Eglantine's source runs games without them).
python3 - "$game" "$run/data/roms/game.eden" "$mount" <<'PY'
import json, pathlib, sys
game, mount = pathlib.Path(sys.argv[1]), sys.argv[3]
kind = mount.rsplit('.', 1)[-1] if '.' in pathlib.Path(mount).name else ''
if kind in ('xci', 'nsp', 'nup'):  # ExeFS and RomFS from inside the one file
    files = {kind: mount, 'required_modules': ['main']}
else:
    modules = [m for m in ('rtld', 'main', 'subsdk0', 'subsdk1', 'subsdk2', 'subsdk3', 'sdk') if (game / 'exefs' / m).is_file()]
    files = {'exefs': mount + '/exefs', 'romfs': {'kind': 'loose', 'path': mount + '/romfs'}, 'required_modules': modules}
pathlib.Path(sys.argv[2]).write_text(json.dumps({'schema': 1, 'enabled': True, 'mode': 'execute', **files}, indent=2) + '\n')
PY
echo "== $(basename "$game"): log in $run/stderr.log"
# Eden opens the firmware NCAs for writing, so they are a copy here (rsync: once per change).
rsync -a --delete "$firmware/" "$run/user/nand/system/Contents/registered/"
rm -f "$run/stderr.log"
# EDEN_HOST_VULKAN=1: render with the PC's GPU into an X11 window (the null renderer otherwise),
# so the sandbox also gets the GPU devices, the X11 socket and its authority file.
gpu=()
if [[ -n ${EDEN_HOST_VULKAN:-} ]]; then
    for device in /dev/dri /dev/nvidia*; do [[ -e $device ]] && gpu+=(--dev-bind "$device" "$device"); done
    # The NVIDIA driver finds its GPU through /sys; without it Vulkan falls back to llvmpipe.
    gpu+=(--ro-bind /sys /sys --bind /tmp/.X11-unix /tmp/.X11-unix)
    [[ -n ${XAUTHORITY:-} && -f $XAUTHORITY ]] && gpu+=(--ro-bind "$XAUTHORITY" "$XAUTHORITY")
fi
bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin \
    --symlink usr/sbin /sbin --ro-bind /etc /etc --proc /proc --dev /dev --tmpfs /tmp \
    --ro-bind "$binary" /app/eden-headless --bind "$run" /work --bind "$run/data" /data/prosperoeden \
    --ro-bind "$keys" /data/prosperoeden/keys --ro-bind "$keys" /work/user/keys \
    --ro-bind "$game" "$mount" --chdir /work --die-with-parent "${gpu[@]}" \
    ${NS2_WRAPPER:-} /app/eden-headless /data/prosperoeden/roms/game.eden --game 2> "$run/stderr.log" &
pid=$!
# A guest fault halts the emulated CPU but not the app: end the run once its backtrace is out,
# or after NS2_RUN_SECONDS (default 120) of running.
for ((second = 0; second < ${NS2_RUN_SECONDS:-120}; ++second)); do
    kill -0 "$pid" 2>/dev/null || break
    if grep -q -e '^EDEN_GUEST_BACKTRACE' -e 'Userspace PANIC' "$run/stderr.log" 2>/dev/null; then
        sleep 2; echo "== guest fault after ${second} s"; break
    fi
    sleep 1
done
if kill -0 "$pid" 2>/dev/null; then kill "$pid"; status=124; fi
wait "$pid" 2>/dev/null || status=${status:-$?}
echo "== exit ${status:-0}"
exit "${status:-0}"
