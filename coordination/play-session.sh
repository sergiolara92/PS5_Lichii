#!/usr/bin/env bash
# The tester plays a checkout's dev build: install, dev settings, launch, then wait for the tester to
# quit the game. No pad input, no stop: the run ends when the title exits (or after an hour, by
# run-title's watchdog). No GPU capture (play-capture.sh arms one).
#   play-session.sh <tag> <checkout> [DEV_SETTINGS words...]
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
: "${PS5_HOST:?set PS5_HOST to the console address}"; export PS5_HOST; HOST=$PS5_HOST; APP=/data/homebrew/PPSA99009
tag=${1:?tag}; checkout=${2:?checkout}; shift 2
DEV_SETTINGS="${*:-gpu_accuracy=low cram_fine=on cram_dedup=on}"
out=$C/runs/$tag; mkdir -p $out
exec 9>"$C/runs/console.lock"; flock 9
echo "console lock taken: $tag ($checkout, play and capture)" | tee -a "$C/runs/console.log"
cd "$checkout" || exit 1
timeout 900 make install PS5_HOST=$HOST APP=build/dev/PPSA99009 > $out/install.log 2>&1 || { echo "INSTALL FAILED"; exit 1; }
ftp() {
    timeout 60 python3 -I - "$@" <<'PY'
import ftplib, io, sys
f = ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"], 2121, timeout=30); f.login(); f.voidcmd("TYPE I")
op = sys.argv[1]
try:
    if op == "put": f.storbinary("STOR " + sys.argv[2], io.BytesIO(sys.argv[3].encode()))
    elif op == "get":
        with open(sys.argv[3], "wb") as o: f.retrbinary("RETR " + sys.argv[2], o.write)
    elif op == "rm": f.delete(sys.argv[2])
    elif op == "mkd": f.mkd(sys.argv[2])
except ftplib.all_errors as e: print(f"ftp {op} {sys.argv[2]}: {e}", file=sys.stderr)
f.quit()
PY
}
for n in compat-input.txt quit-app.txt capture-once.txt; do ftp rm $APP/$n 2>/dev/null; done
ftp rm /data/lichii/capture-now.txt 2>/dev/null
# A crash notice from an earlier session opens the launcher instead of the game.
ftp rm /data/lichii/logs/crash-note.txt 2>/dev/null
ftp mkd /data/lichii/captures 2>/dev/null
ftp put $APP/dev-settings.txt "$DEV_SETTINGS"
cd /home/mihawk/Desktop/PS5_Homebrews/PS5_Vulkan
echo "launched: play; quit the game when done"
PS5_HOST=$HOST timeout 3700 python3 tools/run-title.py PPSA99009 --until 'Userspace PANIC' --timeout 3600 \
    --output $out/klog.log > $out/run.out 2>&1
ftp rm /data/lichii/capture-now.txt 2>/dev/null
ftp rm $APP/dev-settings.txt 2>/dev/null
for n in heap:/data/lichii/logs/heap.log stderr:/data/lichii/logs/stderr.log eden_log:/data/lichii/user/log/eden_log.txt; do
    ftp get ${n#*:} $out/${n%%:*}.txt
done
echo "DONE $tag"
