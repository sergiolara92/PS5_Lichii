#!/usr/bin/env bash
# Runs ON the PS5 Linux lab. lab-oot-move.sh <tag> ["VAR=value ..."]: OOT gameplay in motion: loads the save, walks
# out of Link's house into Kokiri Forest (lab-oot-door.sh), then for the whole measurement walks forward and back
# while panning the camera (each 2.9 s step: left stick up or down, right stick right or left), and measures
# (lab-measure.sh: fps average and median, front-end counters, hot code footprint). Results in shots/<tag>.txt.
set -u
cd /data/lab/bin
tag=${1:?tag}; export LAB_EXTRA=${2:-}
./lab-oot-door.sh "$tag" "$LAB_EXTRA" > /dev/null 2>&1
for i in 1 2 3; do ./labdrive.sh press 0 2900 128 0 128 128; sleep 3.2; done
( while :; do
    ./labdrive.sh press 0 2900 128 0 255 128; sleep 3.0
    ./labdrive.sh press 0 2900 128 255 0 128; sleep 3.0
  done ) & mover=$!
sleep 6
./lab-measure.sh "$tag" 2>&1 | tee /data/lab/shots/$tag.txt
kill $mover 2>/dev/null; wait $mover 2>/dev/null
./labdrive.sh shot $tag-moving >/dev/null 2>&1
./labdrive.sh stop >/dev/null 2>&1
