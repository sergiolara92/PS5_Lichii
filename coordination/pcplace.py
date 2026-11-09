#!/usr/bin/env python3
# pcplace.py <name>:<cpus>+...+rest:<cpus> <command...>: NS2_WRAPPER for PC placement experiments (the
# PC build places no threads). Runs the command and, every second, gives each new thread of it the
# CPUs of the first spec whose name prefix matches its name ("rest" = every other thread); cpus are
# comma lists, e.g. CPUCore_0:0+GPUBack:5+rest:6,7,8,9 (no spaces: run-ns2-host.sh splits
# NS2_WRAPPER on them). Placements go to stderr once per thread.
import ctypes, os, signal, subprocess, sys, time

spec = []
for item in sys.argv[1].split('+'):
    name, cpus = item.rsplit(':', 1)
    spec.append((name, {int(c) for c in cpus.split(',')}))
# The app dies with this wrapper: bwrap's --die-with-parent kills only its own child (this script),
# and an orphaned app would keep the PC GPU and its video memory.
libc = ctypes.CDLL(None, use_errno=True)
child = subprocess.Popen(sys.argv[2:], preexec_fn=lambda: libc.prctl(1, signal.SIGKILL))  # PR_SET_PDEATHSIG
signal.signal(signal.SIGTERM, lambda *_: child.terminate())
placed = {}  # tid -> name it was placed under (threads are renamed after they start)
while child.poll() is None:
    try:
        tids = os.listdir(f'/proc/{child.pid}/task')
    except OSError:
        break
    for tid in tids:
        try:
            comm = open(f'/proc/{child.pid}/task/{tid}/comm').read().strip()
        except OSError:
            continue
        if placed.get(tid) == comm:
            continue
        for name, cpus in spec:
            if name == 'rest' or comm.startswith(name):
                try:
                    os.sched_setaffinity(int(tid), cpus)
                    print(f'pcplace: {tid} {comm} -> {sorted(cpus)}', file=sys.stderr, flush=True)
                except OSError as e:
                    print(f'pcplace: {tid} {comm}: {e}', file=sys.stderr, flush=True)
                break
        placed[tid] = comm
    time.sleep(1)
sys.exit(child.wait())
