#!/usr/bin/env bash
# dkb-session.sh <tag> <descriptor file>: closes a running title (hung sessions), puts the descriptor
# on the console as Bananza's, and starts a play-capture session (capture armed, no pad input).
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination
tag=${1:?tag}; desc=${2:?descriptor}
cd /home/mihawk/Desktop/PS5_Homebrews/PS5_Vulkan
timeout 90 python3 -I -c "
import sys; sys.path.insert(0,'tools'); import ps5_console as p
print(p.ps5vkctl_command(p.load_settings(), 'kill PPSA99009', timeout=60))" 2>&1 | tail -1 | sed -E 's/192\.168\.[0-9.]+/x/g'
while ps -eo args | grep -q '[p]lay-capture.sh'; do sleep 3; done
timeout 30 python3 -I - "$desc" <<'PY'
import ftplib, sys
f = ftplib.FTP(); f.connect(__import__("os").environ["PS5_HOST"], 2121, timeout=20); f.login(); f.voidcmd("TYPE I")
with open(sys.argv[1], "rb") as s: f.storbinary("STOR /data/lichii/roms/donkeykongbananza/Donkey Kong Bananza.eden", s)
f.quit()
PY
shift 2
cd $C && exec ./play-capture.sh $tag ../ProsperoEden dkb-1 900 "$@"
