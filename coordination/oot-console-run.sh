#!/usr/bin/env bash
# oot-console-run.sh <tag> <checkout> [MEASURE=100] [DEV_SETTINGS words...]: unattended OOT run on the
# console. Installs the checkout's dev build, launches it, waits for the title screen (two 60 fps
# windows), presses Circle (Switch A) three times (Continue, slot, Load), measures MEASURE seconds
# of gameplay, then ends through the emulator (TouchPad+L1, then quit-app.txt) and fetches the logs.
# Prints the gameplay fps windows and the GPU front thread's cost (oot-gpu-stats.py).
# OOT_ROUTE="lx ly rx ry ms;...": stick steps sent as soon as gameplay starts (e.g. the user's save in Hyrule
# Castle Town out to Hyrule Field), then OOT_ROUTE_WAIT seconds (default 60) for the area to load before measuring.
# OOT_RUN_TIMEOUT=1400: seconds before the hang watchdog kills the title (raise it for a boot that builds every pipeline).
# OOT_MOVE=1: while measuring, keep walking forward and back with the camera panning (3 s steps).
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
: "${PS5_HOST:?set PS5_HOST to the console address}"; export PS5_HOST; HOST=$PS5_HOST; APP=/data/homebrew/PPSA99008
tag=${1:?tag}; checkout=$(realpath ${2:?checkout}); MEASURE=${3:-100}; shift 3 2>/dev/null || shift $#
DEV_SETTINGS="${*:-gpu_accuracy=low cram_fine=on cram_dedup=on}"
out=$C/runs/$tag; mkdir -p $out
exec 9>"$C/runs/console.lock"; flock 9
# The mover subshell (OOT_MOVE) outlives a run that ends early (the game crashed while measuring)
# and keeps the lock and the inputs going: end it with the script.
trap '[[ -n ${mover:-} ]] && kill $mover 2>/dev/null' EXIT
echo "console lock taken: $tag ($checkout, unattended oot)" | tee -a "$C/runs/console.log"
ftp() {
    timeout 60 python3 -I - "$HOST" "$@" <<'PY'
import ftplib, io, sys
f = ftplib.FTP(); f.connect(sys.argv[1], 2121, timeout=30); f.login(); f.voidcmd("TYPE I")
op = sys.argv[2]
try:
    if op == "put": f.storbinary("STOR " + sys.argv[3], io.BytesIO(sys.argv[4].encode()))
    elif op == "get":
        with open(sys.argv[4], "wb") as o: f.retrbinary("RETR " + sys.argv[3], o.write)
    elif op == "rm": f.delete(sys.argv[3])
except ftplib.all_errors as e: print(f"ftp {op} {sys.argv[3]}: {e}", file=sys.stderr)
f.quit()
PY
}
cd "$checkout" && timeout 900 make install PS5_HOST=$HOST APP=build/dev/PPSA99008 > $out/install.log 2>&1 || { echo "INSTALL FAILED"; exit 1; }
for n in compat-input.txt quit-app.txt capture-once.txt; do ftp rm $APP/$n 2>/dev/null; done
ftp rm /data/prosperoeden/logs/crash-note.txt 2>/dev/null
# The last session's heap.log stays until this one writes its own: the polls would read it.
ftp rm /data/prosperoeden/logs/heap.log 2>/dev/null
# The same for the other logs fetched at the end: a session that never started showed the last one's (fritchii-36).
ftp rm /data/prosperoeden/logs/stderr.log 2>/dev/null; ftp rm /data/prosperoeden/user/log/eden_log.txt 2>/dev/null
ftp put $APP/dev-settings.txt "$DEV_SETTINGS"
cd /home/mihawk/Desktop/PS5_Homebrews/PS5_Vulkan
PS5_HOST=$HOST timeout $(( ${OOT_RUN_TIMEOUT:-1400} + 100 )) python3 tools/run-title.py PPSA99008 --until 'Userspace PANIC' --timeout ${OOT_RUN_TIMEOUT:-1400} \
    --output $out/klog.log > $out/run.out 2>&1 &
runner=$!
id=$(date +%s)
press() { id=$((id + 1)); ftp put $APP/compat-input.txt "$id $1 $2"; }
# The console's log as it is, kept in $out/heap.poll only while it has at least as many frame windows as
# the copy before (a crash restarts the app, which starts its log again).
windows() {
    ftp get /data/prosperoeden/logs/heap.log $out/heap.fresh 2>/dev/null
    # grep -c prints 0 and fails on no match, so "|| echo 0" made "0 0"; a missing file prints nothing.
    local fresh polled
    fresh=$(grep -a -c "^EDEN_VULKAN_FRAME" $out/heap.fresh 2>/dev/null); polled=$(grep -a -c "^EDEN_VULKAN_FRAME" $out/heap.poll 2>/dev/null)
    if (( ${fresh:-0} >= ${polled:-0} )); then
        mv -f $out/heap.fresh $out/heap.poll
    fi
    grep -a "^EDEN_VULKAN_FRAME" $out/heap.poll 2>/dev/null | sed -E 's/.*fps=([0-9.]+).*/\1/'
}
started=$(date +%s); phase=title
while kill -0 $runner 2>/dev/null; do
    sleep 10
    now=$(date +%s)
    if [[ $phase == title ]]; then
        # The title screen: two 60 fps windows after the intro, or (a heavier title scene from a later save runs
        # under 60) 45 s after the title video started (vdecsvc configure in the game log).
        if [[ -z ${video_at:-} ]]; then
            ftp get /data/prosperoeden/user/log/eden_log.txt $out/eden.poll 2>/dev/null
            grep -aq "vdecsvc configure" $out/eden.poll 2>/dev/null && video_at=$now
        fi
        if [[ $(windows | awk '$1 >= 55' | wc -l) -ge 2 ]] || { [[ -n ${video_at:-} ]] && (( now - video_at >= 45 )); }; then
            sleep 5; for step in 1 2 3; do press $((0x2000)) 250; sleep 4; done
            phase=loading; continued=$now; echo "pressed Continue at $((now - started)) s"
        elif (( now - started > ${OOT_TITLE_WAIT:-600} )); then echo "no title screen in ${OOT_TITLE_WAIT:-600} s"; phase=quit; fi  # OOT_TITLE_WAIT: longer for a boot that builds every pipeline
    elif [[ $phase == loading ]]; then
        # Gameplay: the first window under 40 fps after the presses.
        n=$(windows | wc -l); last=$(windows | tail -1)
        # Or a fixed time after the presses: Dynamic FPS keeps OOT gameplay at 60, so no slow window may come.
        if [[ -n $last ]] && { awk "BEGIN{exit !($last < 40)}" || (( now - continued >= ${OOT_GAMEPLAY_AFTER:-60} )); }; then  # empty: no window yet (or a restarted app)
            echo "gameplay at $((now - started)) s"; echo "$n" > $out/gameplay-window  # the summary starts here
            # OOT_CAPTURE=1: the frame where gameplay starts (before any route), as $out/spawn.ppm.
            if [[ -n ${OOT_CAPTURE:-} ]]; then
                ftp rm /data/prosperoeden/logs/game-frame.ppm 2>/dev/null; ftp put $APP/capture-once.txt "1"; sleep 12
                ftp get /data/prosperoeden/logs/game-frame.ppm $out/spawn.ppm 2>/dev/null
            fi
            if [[ -n ${OOT_ROUTE:-} ]]; then
                IFS=';' read -ra steps <<< "$OOT_ROUTE"
                for step in "${steps[@]}"; do
                    read -r lx ly rx ry ms <<< "$step"
                    id=$((id + 1)); ftp put $APP/compat-input.txt "$id 0 $ms $lx $ly $rx $ry"
                    sleep $(( (ms + 299) / 1000 + 1 ))
                done
                sleep ${OOT_ROUTE_WAIT:-60}; echo "route done at $(( $(date +%s) - started )) s"
            fi
            if [[ -n ${OOT_MOVE:-} ]]; then
                ( while :; do
                    id=$((id + 1)); ftp put $APP/compat-input.txt "$id 0 2900 128 0 255 128"; sleep 3
                    id=$((id + 1)); ftp put $APP/compat-input.txt "$id 0 2900 128 255 0 128"; sleep 3
                  done ) & mover=$!
            fi
            phase=measure; measure_from=$(date +%s)
        fi
        (( now - continued > 300 )) && phase=quit  # from the presses, not the start: a boot that builds every pipeline is long
    elif [[ $phase == measure ]]; then
        windows >/dev/null  # keeps $out/heap.poll current: the summary reads the gameplay windows from it
        # OOT_WALK="lx ly ms;lx ly ms;...": halfway through, these left-stick pulses in turn (at
        # most 3000 ms each, the input's limit), e.g. out of Link's house into Kokiri Forest; the
        # windows after them measure there.
        if [[ -n ${OOT_WALK:-} && -z ${walked:-} ]] && (( now - measure_from >= MEASURE / 2 )); then
            IFS=';' read -ra steps <<< "$OOT_WALK"
            for step in "${steps[@]}"; do
                read -r lx ly ms <<< "$step"
                id=$((id + 1)); ftp put $APP/compat-input.txt "$id 0 $ms $lx $ly 128 128"
                sleep $(( (ms + 999) / 1000 ))
            done
            walked=1; echo "walked at $((now - started)) s"
        fi
        (( now - measure_from >= MEASURE )) && phase=quit
    elif [[ $phase == quit ]]; then
        windows >/dev/null  # the last gameplay windows, before the quit restarts the log
        [[ -n ${mover:-} ]] && { kill $mover 2>/dev/null; mover=; sleep 1; }
        id=$((id + 1000000))  # above every id the mover subshell sent (the console takes only rising ids)
        ftp get /data/prosperoeden/user/log/eden_log.txt $out/eden_log-prestop.txt 2>/dev/null  # the launcher starts a new log
        press $((0x100400)) 800; sleep 12; ftp rm $APP/compat-input.txt 2>/dev/null  # read; a restarted session must not replay it
        ftp put $APP/quit-app.txt "quit $(date +%s%N)"; sleep 10
    fi
done
wait $runner
for n in heap:/data/prosperoeden/logs/heap.log stderr:/data/prosperoeden/logs/stderr.log eden_log:/data/prosperoeden/user/log/eden_log.txt; do
    ftp get ${n#*:} $out/${n%%:*}.txt
done
ftp rm $APP/dev-settings.txt 2>/dev/null
grep -aq "GPU_FAULT\|App Crash" $out/klog.log && echo "CRASH: $(grep -a 'exception:\|App Crash' $out/klog.log | head -2 | tr '\n' ' ')"
python3 -I $C/oot-gpu-stats.py $out/heap.txt
# Gameplay fps from the copy with the most frames (heap.txt is the console's log after the quit, which a
# restarted app may have begun again): the windows after gameplay started, and the last minute.
python3 -I - $out/gameplay-window $out/heap.txt $out/heap.poll <<'PY'
import re, statistics, sys
def frames(path):
    try: return [float(v) for v in re.findall(r"^EDEN_VULKAN_FRAME .*?fps=([0-9.]+)", open(path, errors="replace").read(), re.M)]
    except OSError: return []
fps = max((frames(p) for p in sys.argv[2:]), key=len)
try: start = int(open(sys.argv[1]).read()) + 6  # 30 s of loading after the gameplay mark
except (OSError, ValueError): start = None
if start is not None and start >= len(fps): start = None
for name, part in (("gameplay", fps[start:] if start is not None else []), ("last minute", fps[-12:] if start is not None else [])):
    if part: print(f"{name}: {len(part)} windows, avg {sum(part)/len(part):.1f}, median {statistics.median(part):.1f}, min {min(part):.1f}")
if start is None: print(f"no gameplay windows ({len(fps)} windows in the log)")
PY
python3 -I $C/oot-frame-lows.py $out/gameplay-window $out/heap.txt $out/heap.poll
echo "DONE $tag"
