#!/usr/bin/env bash
# replay-run.sh for the worktree the console is reserved for (runs/reserved.for); otherwise the same.
# One GPU replay on the Pro (notes/gpu-replay.md): install a checkout's dev build, replay a capture
# already on the console instead of the game, and print the replay's result lines.
#   replay-run.sh <tag> <checkout> <capture on the console> [DEV_SETTINGS words...]
# e.g. replay-run.sh r1 ../ProsperoEden /data/prosperoeden/captures/mkw-race-3.bin
# Shares runs/console.lock with console-run.sh: one run at a time on the Pro.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
: "${PS5_HOST:?set PS5_HOST to the console address}"; export PS5_HOST; HOST=$PS5_HOST
tag=${1:?tag}; checkout=${2:?checkout}; capture=${3:?capture on the console}; shift 3
DEV_SETTINGS="${*:-gpu_accuracy=low gpu_split=off cram_fine=on cram_dedup=on} gpu_replay=$capture"
out=$C/runs/$tag; mkdir -p $out
exec 9>"$C/runs/console.lock"
echo "waiting for the console lock..."
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
# The worktree the console is reserved for skips the ticket order: the other waiters' tickets stay
# alive behind the reservation and would otherwise hold it forever.
until ! reserved_for_other && ! compgen -G "$C/runs/priority.want.*" > /dev/null && { [[ -f "$C/runs/reserved.for" ]] || [[ $(oldest_ticket) == "$ticket" ]]; } && flock -n 9; do sleep 5; done
rm -f "$ticket"
echo "console lock taken: $tag (replay $capture)" | tee -a "$C/runs/console.log"
cd "$checkout" || exit 1
[[ -d build/dev/PPSA99008 ]] || { echo "no build/dev/PPSA99008 in $checkout (make ns2-dev)"; exit 1; }
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
# A crash notice from an earlier run would open the launcher instead of the replay.
ftp rm /data/prosperoeden/logs/crash-note.txt 2>/dev/null
ftp put $APP/dev-settings.txt "$DEV_SETTINGS"
cd /home/mihawk/Desktop/PS5_Homebrews/PS5_Vulkan
# The replay shuts the emulated system down and ends the title once it has printed its result, so
# the run ends when the title exits (run-title.py's kill at the timeout is only the watchdog). The
# result is in the app's stderr.log, not in the klog: a poll reads it, and after a crash (the app
# restarts into the launcher) sends the launcher's quit at once instead of waiting for the timeout.
PS5_HOST=$HOST timeout 900 python3 tools/run-title.py PPSA99008 \
    --until 'EDEN_GUEST_EXCEPTION|Userspace PANIC' \
    --timeout 840 --output $out/klog.log > $out/run.out 2>&1 &
runner=$!
while kill -0 $runner 2>/dev/null; do
    sleep 15
    ftp get /data/prosperoeden/logs/stderr.log $out/stderr.poll 2>/dev/null
    if grep -aq "after_crash=1" $out/stderr.poll 2>/dev/null && ! grep -aq "quit sent" $out/run.note 2>/dev/null; then
        ftp put $APP/quit-app.txt "quit $(date +%s%N)"; echo "quit sent (crash)" >> $out/run.note
    fi
done
wait $runner
for n in stderr:/data/prosperoeden/logs/stderr.log eden_log:/data/prosperoeden/user/log/eden_log.txt; do
    ftp get ${n#*:} $out/${n%%:*}.txt
done
ftp rm $APP/dev-settings.txt 2>/dev/null
echo "DONE $tag"
grep -a "GPU replay" $out/stderr.txt | tail -4
tail -2 $out/run.out | sed -E 's/192\.168\.[0-9]+\.[0-9]+/192.168.x.y/g'
