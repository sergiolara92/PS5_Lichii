#!/usr/bin/env bash
# Runs ON the PS5 Linux lab (base PS5, /data = M.2). Live-drive a lab run of the PC build:
#   labdrive.sh start [seconds] | press <mask> <ms> | shot <name> | log [n] | stop
#   start: the PC build on the PS5 GPU (RADV) in the lab's desktop session; LAB_GAME picks the game,
#          LAB_EXTRA adds environment settings ("VAR=value VAR2=value").
#          Memory as the console runs (gpu_accuracy=low cram_fine=on cram_dedup=on) and texture limits for the
#          lab, whose GPU memory comes out of the same 14 GB.
#   press: <mask> <ms> [lx ly rx ry]: DualSense bits as on the PC (0x2000 Circle = Switch A, 0x4000 Cross); sticks 0-255.
#   shot:  saves the next frame as /data/lab/shots/<name>.ppm and prints its path.
set -u
P=/data/lab/pe; R=/data/lab/cache/ns2-host/run; O=/data/lab/shots; mkdir -p $O
case ${1:?command} in
start)
    rm -f $R/pad.txt $R/capture-now.txt $R/frame-*.ppm
    export DISPLAY=:0 XAUTHORITY=$(ls /run/user/1000/xauth_* | head -1)
    ulimit -c unlimited  # a crash leaves a core for coredumpctl (systemd-coredump)
    cd $P && setsid env EDEN_HOST_VULKAN=1 EDEN_HOST_GPU_LOW=1 EDEN_HOST_CRAM_DEDUP=1 EDEN_HOST_CRAM_FINE=1 EDEN_NX2_TC_EXPECTED_MIB=2048 \
        EDEN_NX2_TC_CRITICAL_MIB=2560 EDEN_HOST_PAD_FILE=/work/pad.txt EDEN_HOST_CAPTURE_FILE=/work/capture-now.txt \
        EDEN_HOST_PRESENT=immediate NS2_RUN_SECONDS=${2:-1200} EDEN_HOST_GAME_SECONDS=${2:-1200} \
        MESA_SHADER_CACHE_DIR=/work/mesa-cache ${LAB_EXTRA:-} \
        tools/run-ns2-host.sh /data/lab/keys /data/lab/firmware-empty "${LAB_GAME:-/data/lab/games/oot.nup}" \
        > $O/run.out 2>&1 < /dev/null &
    echo started;;
press)  # press <mask> <ms> [lx ly rx ry]: sticks 0-255, 128 = centre (same as the console's compat-input.txt)
    printf '%s %s %s %s\n' "$(date +%s%N)" "${2:?mask}" "${3:-200}" "${*:4}" > $R/pad.txt.new && mv $R/pad.txt.new $R/pad.txt;;
shot)
    before=$(ls $R/frame-*.ppm 2>/dev/null | wc -l); touch $R/capture-now.txt
    for _ in $(seq 100); do sleep 0.2; [[ $(ls $R/frame-*.ppm 2>/dev/null | wc -l) -gt $before ]] && break; done
    sleep 0.5; f=$(ls -t $R/frame-*.ppm 2>/dev/null | head -1)
    [[ -n $f ]] || { echo "no frame captured" >&2; exit 1; }
    mv "$f" "$O/${2:?name}.ppm"; rm -f $R/frame-*.ppm; echo "$O/$2.ppm";;
log)
    tail -${2:-5} $R/user/log/eden_log.txt | cut -c1-220;;
stop)
    pkill -TERM -f '^/app/eden-headless'; sleep 3; pkill -KILL -f '^/app/eden-headless'; true;;
esac
