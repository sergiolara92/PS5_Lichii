#!/usr/bin/env bash
# Runs ON the PS5 Linux lab. lab-oot-door.sh <tag> ["VAR=value ..."]: loads the OOT save (Link's house), walks
# to the door (left stick down 4 x 2.9 s), opens it (A) and reports whether frames keep coming 90 s later.
set -u
cd /data/lab/bin
tag=${1:?tag}; export LAB_EXTRA=${2:-}
R=/data/lab/cache/ns2-host/run
frame() { grep -a "EDEN_TC frame" $R/stderr.log | tail -1 | grep -oE "frame=[0-9]+" | cut -d= -f2; }
./labdrive.sh stop >/dev/null 2>&1
./lab-oot-load.sh "$tag" 3600 >/dev/null 2>&1
sleep 8; ./labdrive.sh press 0x2000 300; sleep 35
for i in 1 2 3 4; do ./labdrive.sh press 0 2900 128 255 128 128; sleep 3.2; done
./labdrive.sh press 0x2000 300
sleep 60; a=$(frame); sleep 30; b=$(frame)
echo "== $tag ${LAB_EXTRA}: frames 60-90 s after the door: $a -> $b $([[ $a != "$b" ]] && echo MOVING || echo STUCK)"
./labdrive.sh shot $tag-after-door >/dev/null 2>&1
