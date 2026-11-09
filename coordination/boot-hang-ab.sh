#!/usr/bin/env bash
# boot-hang-ab.sh <pairs>: short boot runs (console-run.sh with SHIFT=300: quit at ~125 s, a hung
# title killed at ~240 s), lazy dense backing on (default) and off, interleaved; prints per boot
# whether the shader cache rebuild finished.
set -u
C=/home/mihawk/Desktop/PS5_Homebrews/PS5_Lichii/coordination; P=$C/../ProsperoEden
S="gpu_accuracy=low cram_fine=on cram_dedup=on"
for i in $(seq 1 ${1:-8}); do
  for mode in on off; do
    t=bh-$mode-$i; extra=$([ $mode = off ] && echo env=EDEN_NX2_DENSE_LAZY_MIB=0)
    SHIFT=300 ./console-run.sh $t $P $S $extra > runs/$t.out 2>&1
    last=$(grep -a "Shader cache:" runs/$t/stderr.txt 2>/dev/null | tail -1 | sed 's/.*Shader cache: //')
    frames=$(grep -ac "EDEN_VULKAN_FRAME " runs/$t/heap.txt 2>/dev/null)
    echo "$t | $last | frame windows $frames | $(tail -2 runs/$t.out | grep -o 'exited after [0-9.]* s\|timed out after [0-9.]* s\|crashed after [0-9.]* s')"
  done
done
echo done
