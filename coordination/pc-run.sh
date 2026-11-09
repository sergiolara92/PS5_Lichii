#!/usr/bin/env bash
# One PC run of Mario Kart World from a checkout's PC build (make ns2-host first), one at a time
# (runs/pc.lock: the PC GPU takes one test at a time). Extra arguments are environment settings.
#   pc-run.sh <checkout> [VAR=value ...]   e.g. EDEN_HOST_VULKAN=1 NS2_RUN_SECONDS=220
#     NS2_GAME=<.xci or folder> EDEN_HOST_PAD="45:150:4:0.25:0x6000" EDEN_HOST_GPU_LOW=1 EDEN_NX2_SPLIT=0
# Logs: <the checkout's build cache>/ns2-host/run/stderr.log and user/log/eden_log.txt.
# wallprof/wallprof.so samples named threads' stacks (see wallprof.c; report.py symbolizes).
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
checkout=${1:?checkout}; shift
exec 9>"$C/runs/pc.lock"
# Out of video memory, the NVIDIA driver stops creating devices until a reboot ("GspRmAlloc failed"
# after NVRM out-of-memory lines): refuse to run on it rather than test on a broken GPU. Out-of-memory
# assertions alone are pressure the driver survived (Vulkan still lists the GPU).
if journalctl -k -b 2>/dev/null | grep -q "NVRM:.*GspRmAlloc failed" ||
   ! timeout 30 vulkaninfo --summary 2>/dev/null | grep -q "deviceName.*NVIDIA"; then
    echo "PC GPU driver wedged since this boot (NVRM out of memory): reboot the PC first" >&2
    exit 3
fi
echo "waiting for the PC lock..."; flock 9
# An earlier run killed under a wrapper (NS2_WRAPPER) can leave its emulator running with GBs of
# video memory: we hold the lock, so any eden-headless still alive is a leftover.
pkill -TERM -f '^/app/eden-headless' 2>/dev/null && sleep 5 && pkill -KILL -f '^/app/eden-headless' 2>/dev/null
# Out of video memory the driver wedges until a reboot: refuse a run with too little free (desktop
# apps hold some), after waiting up to 3 min for a killed run's memory to come back. PC_MIN_FREE_MIB,
# default 6000.
for _ in $(seq 36); do
    free=$(timeout 15 nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits 2>/dev/null | head -1)
    [[ -n $free && $free -ge ${PC_MIN_FREE_MIB:-6000} ]] && break
    sleep 5
done
if [[ -z $free || $free -lt ${PC_MIN_FREE_MIB:-6000} ]]; then
    echo "PC GPU has ${free:-unknown} MiB of video memory free (< ${PC_MIN_FREE_MIB:-6000}): close GPU-heavy apps first" >&2
    exit 3
fi
cd "$checkout" || exit 1
# NS2_GAME among the settings picks the game (.xci or folder).
for setting in "$@"; do [[ $setting == NS2_GAME=* ]] && NS2_GAME=${setting#NS2_GAME=}; done
# PC_QUIET_DUMP=<seconds>: when the game's log has been quiet that long (a boot or a race that stopped
# moving), every thread's stack once into runs/pc-quiet-dump-<time>.txt (EDEN_HOST_STOP_DUMP lets gdb attach).
quiet_env=(); [[ -n ${PC_QUIET_DUMP:-} ]] && quiet_env=(EDEN_HOST_STOP_DUMP=/work/stop-dump.txt)
game_log=$(cat .local/headless-cache)/ns2-host/run/user/log/eden_log.txt
# Its own process group, so the watchdog below can stop all of it.
# Keys and firmware overridable: the fixed Downloads paths they used to live at are gone. A GPU
# replay needs neither to run the guest, but the front-end still opens them.
PC_KEYS=${PC_KEYS:-/home/mihawk/Desktop/Win-Games}
PC_FIRMWARE=${PC_FIRMWARE:-/home/mihawk/Desktop/Win-Games/firmware-empty}
setsid env "$@" "${quiet_env[@]}" tools/run-ns2-host.sh "$PC_KEYS" \
    "$PC_FIRMWARE" "${NS2_GAME:-/home/mihawk/Desktop/Win-Games/mkw/base [0400C3F00006E000].xci}" &
run=$!
trap 'kill -TERM -- -$run 2>/dev/null' INT TERM
# Video-memory watchdog: every 0.5 s; the run is stopped when free video memory falls under
# PC_FLOOR_MIB (default 800), before an allocation can fail inside the driver. Peak use goes to
# runs/pc-vram.log; PC_VRAM_TRACE=<file> also logs "<epoch seconds> <MiB used>" at every sample.
peak=0; stopped=0; dumped=0; started=$(date +%s)
while kill -0 $run 2>/dev/null; do
    if [[ -n ${PC_QUIET_DUMP:-} && $dumped == 0 && -f $game_log ]] && (( $(date +%s) - started > 60 &&
            $(date +%s) - $(stat -c %Y "$game_log") > PC_QUIET_DUMP )); then
        dumped=1
        for p in $(pgrep -f "^/app/eden-headless"); do
            grep -qs "$(dirname "$(dirname "$(dirname "$game_log")")")" /proc/$p/mountinfo || continue
            dump=$C/runs/pc-quiet-dump-$(date +%H%M%S).txt
            gdb -batch -ex 'set sysroot /' -ex 'info threads' -ex 'thread apply all bt' build/ns2-host/eden-headless -p $p > "$dump" 2>&1
            echo "pc-run: log quiet for ${PC_QUIET_DUMP} s: thread stacks in $dump" >&2
        done
    fi
    read -r used free < <(timeout 5 nvidia-smi --query-gpu=memory.used,memory.free --format=csv,noheader,nounits 2>/dev/null | tr -d ',' | head -1)
    if [[ -n ${free:-} ]]; then
        (( used > peak )) && peak=$used
        [[ -n ${PC_VRAM_TRACE:-} ]] && echo "$(date +%s.%N) $used" >> "$PC_VRAM_TRACE"
        if (( free < ${PC_FLOOR_MIB:-800} )); then
            echo "pc-run watchdog: ${free} MiB of video memory free: stopping the run" >&2
            kill -TERM -- -$run 2>/dev/null; sleep 5; kill -KILL -- -$run 2>/dev/null; stopped=1; break
        fi
    fi
    sleep 0.5
done
wait $run; code=$?
echo "$(date '+%F %T') $checkout peak_used_mib=$peak watchdog_stop=$stopped exit=$code" >> "$C/runs/pc-vram.log"
echo "pc-run: peak video memory in use ${peak} MiB (watchdog stop: $stopped)" >&2
exit $code
