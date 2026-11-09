#!/usr/bin/env bash
# One Pro run of a checkout's dev build (make ns2-dev first): install, launch, end through the
# emulator (menu chord, then quit-app.txt in the launcher), fetch the logs.
#   console-run.sh <tag> <checkout> [DEV_SETTINGS words...]
# Only one run at a time uses the console: this takes runs/console.lock (flock) and waits for it.
# Logs: runs/<tag>/ (heap.txt = per-window perf lines, stderr.txt, eden_log.txt, run.out).
# A crash restarts the app (stderr.txt then says after_crash=1): fetch-crash.sh runs/<tag> gets it.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
: "${PS5_HOST:?set PS5_HOST to the console address}"; export PS5_HOST; HOST=$PS5_HOST
tag=${1:?tag}; checkout=${2:?checkout}; shift 2
DEV_SETTINGS="${*:-gpu_accuracy=low gpu_split=off cram_fine=on cram_dedup=on}"
# The timeline below is for a game read from its .xci: SHIFT seconds earlier than the folder dump
# needed (it reaches the race ~107 s sooner). SHIFT=0 for a build that still boots the folder.
SHIFT=${SHIFT:-100}
out=$C/runs/$tag; mkdir -p $out
# PRIORITY=1 goes ahead of the queue (short measurement runs): ordinary runs step aside while a
# priority request waits (runs/priority.want); a run already on the console is never interrupted.
exec 9>"$C/runs/console.lock"
echo "waiting for the console lock..."
if [[ ${PRIORITY:-0} == 1 ]]; then
    want="$C/runs/priority.want.$$"; touch "$want"
    flock 9; rm -f "$want"
else
    # First come, first served: each waiter holds a ticket (runs/queue/<ns>.<pid>) and takes the lock
# only when its ticket is the oldest live one (a dead waiter's ticket is skipped and removed).
mkdir -p "$C/runs/queue"; ticket="$C/runs/queue/$(date +%s%N).$$"; touch "$ticket"
trap 'rm -f "$ticket"' EXIT
oldest_ticket() {
    for t in $(ls "$C/runs/queue" 2>/dev/null | sort); do
        kill -0 "${t##*.}" 2>/dev/null && { echo "$C/runs/queue/$t"; return; }
        rm -f "$C/runs/queue/$t"
    done
}
# The console reserved for one worktree (runs/reserved.for holds its folder name, e.g.
# ProsperoEden-stage3): every other run waits until the file is removed.
reserved_for_other() {
    [[ -f "$C/runs/reserved.for" ]] || return 1
    [[ "$(basename "$(realpath "$C/$checkout" 2>/dev/null || realpath "$checkout" 2>/dev/null)")" != "$(cat "$C/runs/reserved.for")" ]]
}
until ! reserved_for_other && ! compgen -G "$C/runs/priority.want.*" > /dev/null && [[ $(oldest_ticket) == "$ticket" ]] && flock -n 9; do sleep 5; done
rm -f "$ticket"
fi
echo "console lock taken: $tag ($checkout)$([[ ${PRIORITY:-0} == 1 ]] && echo ' [priority]')" | tee -a "$C/runs/console.log"
S=$C  # fetch helpers below
cd "$checkout" || exit 1
[[ -d build/dev/PPSA99008 ]] || { echo "no build/dev/PPSA99008 in $checkout (make ns2-dev)"; exit 1; }
# The build must link the driver the shared archive holds (a stale isolated copy linked the old
# driver into every worktree on 2026-11-04 and voided their fault counts).
siblings=$(cat .local/siblings 2>/dev/null || echo ..)  # the checkout's sibling folder (PROSPERO_SIBLINGS)
shared=$(sha256sum < "$siblings/mihawk-vulkan-review/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" | cut -d' ' -f1)
isolated=$(python3 -c 'import json; print(json.load(open("build/radv-isolated/manifest.json")).get("source_sha256", ""))' 2>/dev/null)
if [[ ${ALLOW_OLD_DRIVER:-0} != 1 && $isolated != "$shared" ]]; then
    echo "REFUSED: $checkout links a driver copy isolated from another archive; rm build/radv-isolated/libvulkan_radeon.ps5.a and rebuild (ALLOW_OLD_DRIVER=1 to run anyway)"
    exit 1
fi
timeout 900 make install PS5_HOST=$HOST APP=build/dev/PPSA99008 > $out/install.log 2>&1 || { echo "INSTALL FAILED"; exit 1; }
ftp() {  # ftp put NAME TEXT | ftp get REMOTE LOCAL | ftp rm REMOTE
    timeout 60 python3 -I - "$@" <<'PY'
import ftplib, io, sys
f = ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"], 2121, timeout=30); f.login(); f.voidcmd("TYPE I")
op = sys.argv[1]
try:
    if op == "put": f.storbinary("STOR " + sys.argv[2], io.BytesIO(sys.argv[3].encode()))
    elif op == "get":
        with open(sys.argv[3], "wb") as o: f.retrbinary("RETR " + sys.argv[2], o.write)
    elif op == "rm": f.delete(sys.argv[2])
except ftplib.all_errors as e: print(f"ftp {op} {sys.argv[2]}: {e}", file=sys.stderr)
f.quit()
PY
}
APP=/data/homebrew/PPSA99008
ftp rm $APP/compat-input.txt 2>/dev/null; ftp rm $APP/quit-app.txt 2>/dev/null
ftp rm /data/prosperoeden/logs/game-frame.ppm 2>/dev/null; ftp rm $APP/capture-once.txt 2>/dev/null
# DEV_SETTINGS="cram_fine=on ...": one-run A/B switches (main.cpp dev-settings.txt); none = defaults.
if [[ -n ${DEV_SETTINGS:-} ]]; then ftp put $APP/dev-settings.txt "$DEV_SETTINGS"; else ftp rm $APP/dev-settings.txt 2>/dev/null; fi
cd /home/mihawk/Desktop/PS5_Homebrews/PS5_Vulkan
PS5_HOST=$HOST timeout $((700 - SHIFT)) python3 tools/run-title.py PPSA99008 \
    --until 'EDEN_GUEST_EXCEPTION|EDEN_GUEST_BACKTRACE|Userspace PANIC' --timeout $((540 - SHIFT)) \
    --output $out/klog.log > $out/run.out 2>&1 &
runner=$!
start=$(date +%s); id=$start
at() { while (( $(date +%s) - start < $1 - SHIFT )); do kill -0 $runner 2>/dev/null || return 1; sleep 1; done; }
press() { id=$((id + 1)); ftp put $APP/compat-input.txt "$id $1 $2"; }
for t in 60 120 170 200 230 260 290 320 350 380 410; do
    at $t || break
    # The picture asked for at the last stop (Vulkan dev builds take one per capture-once.txt,
    # at their next 30 s segment from the session's fourth on), then ask for the next.
    ftp get /data/prosperoeden/logs/game-frame.ppm $out/frame-$t.ppm 2>/dev/null
    ftp put $APP/capture-once.txt "1"
    (( t >= 200 )) && press 24576 300   # Cross+Circle
done
# The race's logs before the stop: a stop that hangs restarts the app, whose new session overwrites them.
ftp get /data/prosperoeden/logs/heap.log $out/heap-prestop.txt
ftp get /data/prosperoeden/user/log/eden_log.txt $out/eden_log-prestop.txt
if kill -0 $runner 2>/dev/null; then
    # TouchPad+L1 back to the launcher, then quit-app.txt there; again every 25 s until the app
    # has gone (a crash restarts it into a loading game, which ignores the shortcut).
    for t in 425 450 475 500 525; do
        at $t && press $((0x100400)) 800
        at $((t + 12)) && ftp put $APP/quit-app.txt "quit $(date +%s%N)"
    done
fi
wait $runner
for n in heap:/data/prosperoeden/logs/heap.log stderr:/data/prosperoeden/logs/stderr.log eden_log:/data/prosperoeden/user/log/eden_log.txt; do
    ftp get ${n#*:} $out/${n%%:*}.txt
done
for n in heap eden_log; do
    # Shorter than before the stop: a restarted session's, so keep the race's.
    if [[ -f $out/$n-prestop.txt ]] && (( $(stat -c %s $out/$n.txt 2>/dev/null || echo 0) < $(stat -c %s $out/$n-prestop.txt) )); then
        mv $out/$n.txt $out/$n-restart.txt 2>/dev/null; cp $out/$n-prestop.txt $out/$n.txt
    fi
done
echo "DONE $tag"; tail -3 $out/run.out | sed -E 's/192\.168\.[0-9]+\.[0-9]+/192.168.x.y/g'
