#!/usr/bin/env bash
# Runs ON the PS5 Linux lab during an OOT gameplay run. lab-measure.sh <label>: fps over 40 s, the
# critical cores' front-end counters (lab-frontend.sh) and their hot JIT code footprint.
set -u
cd /data/lab/bin
tag=${1:?label}
R=/data/lab/cache/ns2-host/run
echo "== $tag ${LAB_EXTRA:-}"
# fps: the texture cache ticks once per game frame and prints every 120 frames; each 120-frame window gives
# one fps sample, so the average (frames / time) and the median window are both reported.
frame() { grep -a "EDEN_TC frame" $R/stderr.log | tail -1 | grep -oE "frame=[0-9]+" | cut -d= -f2; }
f0=$(frame); until [[ $(frame) != "$f0" ]]; do sleep 0.05; done
fa=$(frame); ta=$(date +%s%N); fb=$fa; tb=$ta; windows=()
while (( $(date +%s%N) - ta < ${LAB_FPS_SECONDS:-40}000000000 )); do
    f=$(frame)
    if [[ $f != "$fb" ]]; then now=$(date +%s%N); windows+=("$(( (f - fb) * 1000000000000 / (now - tb) ))"); fb=$f; tb=$now; fi
    sleep 0.05
done
printf '%s\n' "${windows[@]}" | sort -n | awk -v n=$((fb - fa)) -v t=$((tb - ta)) '{w[NR] = $1 / 1000}
  END { printf "fps avg %.2f median %.2f min %.2f (%d frames in %.1f s, %d windows)\n", n / (t / 1e9), w[int((NR + 1) / 2)], w[1], n, t / 1e9, NR }'
./lab-frontend.sh 15
# Hot code footprint of the critical cores (cycles samples inside the JIT code cache).
p=$(pgrep -f "^/app/eden-headless" | head -1)
tids=$(for t in /proc/$p/task/*; do case $(cat $t/comm) in CPUCore_0|CPUCore_1|CPUCore_2) printf "%s," "${t##*/}";; esac; done)
sudo perf record -q -e cycles -c 50021 -t "${tids%,}" -o /tmp/ab.data -- sleep 12 2>/dev/null
sudo perf script -i /tmp/ab.data -F ip,dso 2>/dev/null | awk '/eden-jit/{print $1}' > /tmp/ab-ips.txt
python3 -I - <<'EOF'
from collections import Counter
ips = [int(l, 16) for l in open("/tmp/ab-ips.txt")]; n = len(ips)
for g, name in ((64, "lines"), (4096, "4K pages")):
    acc = 0
    for k, (_, v) in enumerate(Counter(i // g for i in ips).most_common(), 1):
        acc += v
        if acc / n >= 0.5:
            print(f"50% of JIT cycles: {k} {name} ({k * g / 1024:.0f} KiB)"); break
EOF
sudo rm -f /tmp/ab.data
