#!/usr/bin/env bash
# Runs ON the PS5 Linux lab. lab-oot-field.sh <tag> ["VAR=value ..."]: the user's save (Hyrule Castle Town, slot list
# picks the latest), walks south out of the town gate into Hyrule Field (8 x 2.9 s down, sidestep, 6 down) and waits for
# the field to load. Leaves the game running in the field; prints memory and captures shots/<tag>-field.ppm.
set -u
cd /data/lab/bin
tag=${1:?tag}; export LAB_EXTRA=${2:-}
./labdrive.sh stop >/dev/null 2>&1
LAB_SAVE=oot-user-20261107 ./lab-oot-load.sh "$tag" 3600 >/dev/null 2>&1
sleep 8; ./labdrive.sh press 0x2000 300; sleep 45
# 8 steps south, a sidestep left of the gate guard, 6 more south (the Pro and fork routes are the same).
for i in $(seq 8); do ./labdrive.sh press 0 2900 128 255 128 128; sleep 3.2; done
./labdrive.sh press 0 1500 0 128 128 128; sleep 1.8
for i in $(seq 6); do ./labdrive.sh press 0 2900 128 255 128 128; sleep 3.2; done
# The field takes a while to load (frames keep coming at the gate meanwhile): wait as the fork route does.
sleep 70
for _ in $(seq 24); do ./labdrive.sh shot "$tag-field" >/dev/null 2>&1 && break; sleep 5; done
free -m | sed -n 2,3p
