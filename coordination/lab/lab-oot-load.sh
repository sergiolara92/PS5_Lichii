#!/usr/bin/env bash
# Runs ON the PS5 Linux lab. lab-oot-load.sh <tag> [seconds]: OOT from the backed-up save
# (title -> Continue -> slot -> Load) and leaves it running in gameplay; frames in
# /data/lab/shots/<tag>-*.ppm. Drive further with labdrive.sh press/shot.
set -u
cd /data/lab/bin
tag=${1:?tag}
R=/data/lab/cache/ns2-host/run; L=$R/user/log/eden_log.txt
# The save goes into whichever user profile this run directory created (first boot makes one).
S=$(ls -d $R/user/nand/user/save/0000000000000000/*/0400486024DEC000 2>/dev/null | head -1)
[[ -n $S ]] || { echo "no OOT save folder yet: boot OOT once first" >&2; exit 1; }
# LAB_SAVE: a save folder under /data/lab/save-backups (default: Link's house, the first backup).
rm -rf "${S:?}"/slot_* ; cp -r /data/lab/save-backups/${LAB_SAVE:-oot-0400486024DEC000-20261104-220405}/. "$S"/
rm -f "$L"
./labdrive.sh start ${2:-3600} >/dev/null
until grep -a -q "vdecsvc configure" "$L" 2>/dev/null; do sleep 2; done
sleep 30
# A first run on RADV can block for minutes compiling huge compute shaders: presses during that
# stall are lost, so wait until a frame can be captured (the stall ends), up to 10 min.
for _ in $(seq 30); do ./labdrive.sh shot $tag-title >/dev/null 2>&1 && break; sleep 20; done
for step in 1 2 3; do ./labdrive.sh press 0x2000 300; sleep 5; done
sleep 60
./labdrive.sh shot $tag-game1; sleep 10; ./labdrive.sh shot $tag-game2
