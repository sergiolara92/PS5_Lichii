#!/usr/bin/env bash
# dkb-console-run.sh <tag> <checkout> [MEASURE=90] [DEV_SETTINGS words...]: unattended run of the
# dev build's title (Bananza) on the console. Installs, launches, presses Circle (Switch A) every
# 6 s for NAV seconds (default 150) from the first frame (title, file select, cutscenes), then
# measures MEASURE seconds, ends through the emulator (TouchPad+L1, then quit-app.txt), fetches the
# logs and prints every 5 s window's fps and draws per frame (dkb-windows.py).
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
: "${PS5_HOST:?set PS5_HOST to the console address}"; export PS5_HOST; HOST=$PS5_HOST; APP=/data/homebrew/PPSA99009
tag=${1:?tag}; checkout=$(realpath ${2:?checkout}); MEASURE=${3:-90}; NAV=${NAV:-300}; shift 3 2>/dev/null || shift $#
DEV_SETTINGS="${*:-gpu_accuracy=low cram_fine=on cram_dedup=on}"
out=$C/runs/$tag; mkdir -p $out
exec 9>"$C/runs/console.lock"; flock 9
echo "console lock taken: $tag ($checkout, unattended dkb)" | tee -a "$C/runs/console.log"
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
# Never install over a running title (an earlier run's orphan, its runner gone): its files and dev settings would
# change under it. run-title.py refuses to launch then too, but only after the install.
procs=$(cd /home/mihawk/Desktop/PS5_Homebrews/PS5_Vulkan && PS5_HOST=$HOST timeout 60 python3 -c \
    'import sys; sys.path.insert(0, "tools"); import ps5_console as c; print(c.ps5vkctl_command(c.load_settings(), "procs", timeout=20))' 2>&1)
case " $procs " in *" count=0 "*) ;; *) echo "the console is running an application ($procs); not installing"; exit 1;; esac
cd "$checkout" && timeout 900 make install PS5_HOST=$HOST APP=${INSTALL_APP:-build/dev/PPSA99009} > $out/install.log 2>&1 || { echo "INSTALL FAILED"; exit 1; }
for n in compat-input.txt quit-app.txt capture-once.txt; do ftp rm $APP/$n 2>/dev/null; done
ftp rm /data/lichii/logs/crash-note.txt 2>/dev/null
# The last session's heap.log stays until this one writes its own: the polls would read it.
ftp rm /data/lichii/logs/heap.log 2>/dev/null
ftp put $APP/dev-settings.txt "$DEV_SETTINGS"
# LIVE_AB="name=a|b" (LIVE_PERIOD s, default 20): while measuring, the live switch file (headless/live_switches.inc)
# alternates arm A (name=a) and B (name=b) in ABBA order; livestat.py gives the paired verdict at the end. Use with
# MEASURE=300 or so. A/A check: name=v|v. LIVE_FIXED="a=1 b=2": switches written with every period (both arms).
LIVE=/data/lichii/live-switches.txt
ftp rm $LIVE 2>/dev/null
cd /home/mihawk/Desktop/PS5_Homebrews/PS5_Vulkan
PS5_HOST=$HOST timeout 1500 python3 tools/run-title.py PPSA99009 --until 'Userspace PANIC' --timeout 1400 \
    --output $out/klog.log > $out/run.out 2>&1 &
runner=$!
id=$(date +%s)
press() { id=$((id + 1)); ftp put $APP/compat-input.txt "$id $1 $2"; }
windows() { ftp get /data/lichii/logs/heap.log $out/heap.poll 2>/dev/null; grep -a "^EDEN_VULKAN_FRAME" $out/heap.poll 2>/dev/null | sed -E 's/.*fps=([0-9.]+).*/\1/'; }
started=$(date +%s); phase=title
# Title and navigation poll every NAV_EVERY s (default 10 as before; 2 cuts boot-to-gameplay: each menu step waits a loop); measuring
# keeps 10 s. BOOT_ONLY=<s>: navigate (A presses) but quit that long after the first frame (boot-crash hunting, no measure).
while kill -0 $runner 2>/dev/null; do
    if [[ $phase == title || $phase == nav || $phase == boot ]]; then sleep ${NAV_EVERY:-10}; else sleep 10; fi
    now=$(date +%s)
    if [[ $phase == title ]]; then
        if [[ $(windows | wc -l) -ge 1 ]]; then phase=nav; nav_from=$now; echo "first frame at $((now - started)) s"
            [[ -n ${BOOT_ONLY:-} ]] && phase=boot; fi
        (( now - started > 600 )) && { echo "no frame in 600 s"; phase=quit; }
    elif [[ $phase == boot ]]; then
        # Navigation goes on (A presses) as the crash may need the menus or the save loading; quit at the limit.
        if (( now - nav_from >= BOOT_ONLY )); then echo "boot only: quitting $BOOT_ONLY s after the first frame"; phase=quit
        else press $((0x2000)) 200; fi
    elif [[ $phase == nav ]]; then
        # Gameplay: the last two 5 s windows drew over GAMEPLAY_DRAWS (1,000) draws a frame. Until
        # then press A.
        windows >/dev/null
        if [[ $(python3 -I $C/dkb-windows.py $out/heap.poll 2>/dev/null | head -1 | tr ' ' '\n' | tail -2 | awk -F/ -v min=${GAMEPLAY_DRAWS:-1000} '$2 > min' | wc -l) -ge 2 ]]; then
            # Leave the scene the save starts in: B closes a dialog the A presses opened (Bananza:
            # the monkey's talk loops at a high frame rate), then the left stick walks forward
            # (WALK_MS, default 1500), and the scene settles before measuring.
            press $((0x4000)) 200; sleep 2; press $((0x4000)) 200; sleep 2
            id=$((id + 1)); ftp put $APP/compat-input.txt "$id 0 ${WALK_MS:-1500} 128 0 128 128"
            sleep $(( ${WALK_MS:-1500} / 1000 + 6 )); now=$(date +%s)
            phase=measure; measure_from=$now; echo "gameplay, walked, measuring from $((now - started)) s"
        elif (( now - nav_from >= NAV )); then
            echo "no gameplay after $NAV s of presses"; phase=quit
        else
            press $((0x2000)) 200
        fi
    elif [[ $phase == measure ]]; then
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
        # Turn the camera right (right stick) for CAM_MS every CAM_EVERY s while measuring, the same
        # pattern every run, so the windows cover changing views (CAM_EVERY=0: no turns).
        if (( ${CAM_EVERY:-10} > 0 )) && (( now - ${last_cam:-0} >= ${CAM_EVERY:-10} )); then
            id=$((id + 1)); ftp put $APP/compat-input.txt "$id 0 ${CAM_MS:-2500} 128 128 255 128"; last_cam=$now
        fi
        if [[ -n ${LIVE_AB:-} ]]; then
            k=$(( (now - measure_from) / ${LIVE_PERIOD:-20} ))
            if [[ ${k} != ${live_k:-} ]]; then
                live_k=$k; arms=(A B B A); arm=${arms[k % 4]}; ab=${LIVE_AB#*=}; value=${ab%%|*}; [[ $arm == B ]] && value=${ab#*|}
                ftp put $LIVE "$(printf 'arm=%s\nperiod=%d\n%s=%s\n%s' $arm $k "${LIVE_AB%%=*}" "$value" "${LIVE_FIXED:+$(tr ' ' '\n' <<< "$LIVE_FIXED")}")"
            fi
        fi
        (( now - measure_from >= MEASURE )) && phase=quit
    elif [[ $phase == quit ]]; then
        ftp rm $LIVE 2>/dev/null
        press $((0x100400)) 800; sleep 12; ftp put $APP/quit-app.txt "quit $(date +%s%N)"; sleep 10
    fi
done
wait $runner
for n in heap:/data/lichii/logs/heap.log stderr:/data/lichii/logs/stderr.log eden_log:/data/lichii/user/log/eden_log.txt; do
    ftp get ${n#*:} $out/${n%%:*}.txt
done
ftp rm $APP/dev-settings.txt 2>/dev/null
grep -aq "GPU_FAULT\|App Crash" $out/klog.log && echo "CRASH: $(grep -a 'exception:\|App Crash' $out/klog.log | head -2 | tr '\n' ' ')"
python3 -I $C/dkb-windows.py $out/heap.txt
[[ -n ${LIVE_AB:-} ]] && python3 -I $C/livestat.py $out/heap.txt "${LIVE_AB%%=*}"
echo "DONE $tag"
