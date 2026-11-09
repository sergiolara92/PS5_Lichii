#!/usr/bin/env bash
# Runs ON the PS5 Linux lab, with a fork (labfork.py start) already in Hyrule Castle Town from the user's save.
# lab-fork-field.sh <fork>: the same route as ours (down 8 x 2.9 s, left 1.5 s past the guard, down 6 x 2.9 s), waits
# for Hyrule Field, then walks forward and back with the camera panning while measuring fps for 40 s (MangoHud).
set -u
cd /data/lab/bin
fork=${1:?fork}
for i in $(seq 8); do ./labfork.py key Down 2900; sleep 0.3; done
./labfork.py key Left 1500; sleep 0.3
for i in $(seq 6); do ./labfork.py key Down 2900; sleep 0.3; done
sleep 70
./labfork.py shot /data/lab/shots/$fork-field.png
( while :; do ./labfork.py key Up+l 2900; ./labfork.py key Down+j 2900; done ) & mover=$!
sleep 46
kill $mover 2>/dev/null; wait $mover 2>/dev/null
echo "== $fork, Hyrule Field, moving:"; ./labfork.py fps $fork 40
./labfork.py shot /data/lab/shots/$fork-field-end.png
