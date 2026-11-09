#!/usr/bin/env bash
# Runs ON the PS5 Linux lab during a game run. lab-frontend.sh [seconds] [cores]: samples the emulated CPU
# cores' threads (default CPUCore_0..2 = OOT's critical chain) and prints, for JIT code / emulator code / all:
# cycles, front-end starvation (decode queue empty), instruction fills from L2 and from L3+DRAM, iTLB walks,
# mispredicted branches, per 1k instructions and as a share of cycles.
set -u
secs=${1:-12}; cores=${2:-"CPUCore_0 CPUCore_1 CPUCore_2"}
p=$(pgrep -f "^/app/eden-headless" | head -1); [[ -n $p ]] || { echo "no run" >&2; exit 1; }
tids=$(for t in /proc/$p/task/*; do n=$(cat $t/comm); for c in $cores; do [[ $n == "$c" ]] && printf "%s," "${t##*/}"; done; done)
ev=cycles/period=1000003/,instructions/period=1000003/,ic_fetch_stall.ic_stall_dq_empty/period=200003/
ev=$ev,ic_cache_fill_l2/period=2003/,ic_cache_fill_sys/period=2003/,bp_l1_tlb_miss_l2_tlb_miss/period=503/
ev=$ev,ex_ret_brn_misp/period=2003/,ls_refills_from_sys.ls_mabresp_lcl_dram/period=503/
ev=$ev,de_dis_uop_queue_empty_di0/period=200003/,de_dis_uops_from_decoder.opcache_dispatched/period=1000003/,de_dis_uops_from_decoder.decoder_dispatched/period=1000003/
out=/tmp/fe.$$.data
sudo perf record -q -e "$ev" -t "${tids%,}" -o $out -- sleep "$secs" 2>/dev/null
# Per event: total count and the share in each code region (JIT = the memfd code cache).
sudo perf report -i $out --stdio --no-children --sort dso -g none 2>/dev/null | awk '
/^# Samples:/ { ev = $NF; gsub(/\x27|\/.*/, "", ev) }
/^# Event count/ { total[ev] = $NF }
/^ +[0-9]/ { pct = $1; sub(/%/, "", pct); region = ($2 ~ /eden-jit/) ? "jit" : ($2 ~ /eden-headless/) ? "emu" : "other"
             share[ev, region] += pct / 100 }
END {
  split("jit emu all", R, " ")
  printf "%-6s %7s %6s %10s %8s %9s %9s %9s %8s %8s\n", "code", "cyc%", "IPC", "uopq-empty%", "opcache%", "icL2/ki", "icSys/ki", "iTLB/ki", "misp/ki", "DRAM/ki"
  for (k = 1; k <= 3; k++) { r = R[k]
    c = (r == "all") ? total["cycles"] : total["cycles"] * share["cycles", r]
    i = (r == "all") ? total["instructions"] : total["instructions"] * share["instructions", r]
    split("de_dis_uop_queue_empty_di0 ic_cache_fill_l2 ic_cache_fill_sys bp_l1_tlb_miss_l2_tlb_miss ex_ret_brn_misp ls_refills_from_sys.ls_mabresp_lcl_dram de_dis_uops_from_decoder.opcache_dispatched de_dis_uops_from_decoder.decoder_dispatched", E, " ")
    for (j = 1; j <= 8; j++) v[j] = (r == "all") ? total[E[j]] : total[E[j]] * share[E[j], r]
    printf "%-6s %6.1f%% %6.2f %10.1f%% %7.0f%% %9.2f %9.2f %9.3f %8.2f %8.2f\n", r, 100 * c / total["cycles"], i / c,
      100 * v[1] / c, 100 * v[7] / (v[7] + v[8]), 1000 * v[2] / i, 1000 * v[3] / i, 1000 * v[4] / i, 1000 * v[5] / i, 1000 * v[6] / i }
  printf "seconds=%s cycles=%.3g instructions=%.3g\n", "'"$secs"'", total["cycles"], total["instructions"]
}'
sudo rm -f $out
