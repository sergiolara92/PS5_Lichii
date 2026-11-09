#!/usr/bin/env bash
# Runs ON the PS5 Linux lab. lab-oot-field-measure.sh <tag> ["VAR=value ..."]: our build into Hyrule Field from the
# user's save (lab-oot-field.sh), then walks forward and back with the camera panning while lab-measure.sh runs.
set -u
cd /data/lab/bin
tag=${1:?tag}
./lab-oot-field.sh "$tag" "${2:-}"
( while :; do ./labdrive.sh press 0 2900 128 0 255 128; sleep 3.0; ./labdrive.sh press 0 2900 128 255 0 128; sleep 3.0; done ) & mover=$!
sleep 6; LAB_FPS_SECONDS=60 ./lab-measure.sh "$tag" 2>&1 | grep -E "^==|^fps|^jit"
kill $mover 2>/dev/null; wait $mover 2>/dev/null
./labdrive.sh shot "$tag-end" >/dev/null 2>&1
vmstat 1 3 | tail -1 | awk '{print "swap in/out pages/s:", $7, $8}'; swapon --show | tail -2
./labdrive.sh stop >/dev/null 2>&1
