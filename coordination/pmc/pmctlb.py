#!/usr/bin/env python3
# pmctlb.py <out> [first s] [last s]: per thread-name group, shares of cycles from a pmctlb run
# (Skylake-family events): page walks (dTLB load/store, iTLB), memory stalls, top-down.
import collections, sys
G = [['cyc', 'dtlb_load_walk', 'dtlb_store_walk', 'itlb_walk', 'stlb_hit_load'],
     ['cyc', 'stalls_total', 'stalls_mem', 'stalls_l2miss', 'stalls_l3miss'],
     ['cyc', 'idq_not_delivered', 'uops_issued', 'retire_slots', 'recovery_cycles'],
     ['cyc', 'iftag_stall', 'br_misp', 'machine_clears', 'instr']]
first = int(sys.argv[2]) if len(sys.argv) > 2 else 0
last = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
tot = collections.defaultdict(lambda: [[0.0] * 5 for _ in G])
for line in open(sys.argv[1]):
    f = line.split()
    if len(f) < 22 or not first <= int(f[0]) <= last:
        continue
    key = f[1].rstrip('0123456789_') or f[1]
    v = [float(x) for x in f[2:22]]
    for g in range(4):
        for k in range(5):
            tot[key][g][k] += v[g * 5 + k]
for key, t in sorted(tot.items(), key=lambda kv: -kv[1][0][0]):
    c = [t[g][0] or 1 for g in range(4)]
    slots = 4 * c[2]
    fe = t[2][1] / slots; ret = t[2][3] / slots; bad = (t[2][2] - t[2][3] + 4 * t[2][4]) / slots
    print(f'{key:14} cycles {t[0][0]/1e9:8.2f} G  IPC {t[3][4]/c[3]:.2f}')
    print(f'   page walks: dTLB load {100*t[0][1]/c[0]:.1f}%  store {100*t[0][2]/c[0]:.1f}%  iTLB {100*t[0][3]/c[0]:.1f}%  (STLB hits {1000*t[0][4]/t[3][4]:.2f}/kinst)')
    print(f'   stalls: any {100*t[1][1]/c[1]:.1f}%  memory {100*t[1][2]/c[1]:.1f}%  L2 miss {100*t[1][3]/c[1]:.1f}%  L3 miss {100*t[1][4]/c[1]:.1f}%')
    print(f'   top-down: retiring {100*ret:.1f}%  front-end {100*fe:.1f}%  bad spec {100*bad:.1f}%  back-end {100*(1-ret-fe-bad):.1f}%  icache stall {100*t[3][1]/c[3]:.1f}%  br_misp/kinst {1000*t[3][2]/t[3][4]:.2f}')
