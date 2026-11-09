#!/usr/bin/env bash
# Runs ON the PS5 Linux lab. lab-oot-ab.sh <tag> ["VAR=value ..."]: one OOT gameplay measurement with
# the given environment: loads the save (lab-oot-load.sh), then prints fps over 40 s, the critical
# cores' front-end counters (lab-frontend.sh) and their hot JIT code footprint, and stops the run.
# Results also go to /data/lab/shots/<tag>.txt.
set -u
cd /data/lab/bin
tag=${1:?tag}; export LAB_EXTRA=${2:-}
R=/data/lab/cache/ns2-host/run
./labdrive.sh stop >/dev/null 2>&1
./lab-oot-load.sh "$tag" 3600 >/dev/null 2>&1
# The third press can land in a menu transition: one more confirm (harmless once loading).
sleep 8; ./labdrive.sh press 0x2000 300; sleep 40
./lab-measure.sh "$tag" 2>&1 | tee /data/lab/shots/$tag.txt
./labdrive.sh stop >/dev/null 2>&1
