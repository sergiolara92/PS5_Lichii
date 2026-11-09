# lever-jitregs: guest registers in host registers across blocks (2026-11-03)

Verdict: **not worth a stage-1 build.** Even the ideal case (every guest GPR, SP and NZCV
save/reload at block boundaries gone, at no cost) is 7-11% of the critical thread's time; a
realistic cross-block allocator gets a third to a half of that, about 2-5%, for a large and risky
change. That is below the ~10% bar, and nowhere near the 1.4x that 60 fps needs. No code changed
on `lever-jitregs`.

## Measurements
New tools (coordination/): `pmc/pmcjit` (precise PEBS cycle and instruction samples of the guest
cores, run as NS2_WRAPPER so it can read the JIT code bytes at every sampled address),
`pmc/jitmix.py` (retired-instruction mix and cycle mix of JIT code by instruction kind),
`pc-jitmix.sh <tag> <checkout> [FROM] [SECONDS]` (one PC race under pmcjit).

PC race, runs/jm-base-1 (commit 1512724, RTX 3070, CRAM on and no fastmem like the console, so
the page-table path is in the code, 300-360 s, 533k samples in JIT code). Precise samples,
so no skid:

| kind (share of JIT code) | instructions, all cores | cycles, all cores | cycles, busiest core |
| --- | --- | --- | --- |
| guest GPR/SP loads + stores ([r15+0..255]) | 11.1% | 13.8% | 11.7% |
| NZCV load + store | 3.7% | 4.2% | 3.8% |
| guest vector regs | 2.2% | 1.5% | 2.5% |
| halt check, PC store | 1.0% | 2.4% | 1.6% |
| guest memory access (data) | 11.4% | **30.3%** | 31.3% |
| page-table walk (load, checks, alignment) | 12.5% | 8.9% | 8.3% |
| `mov eXX,eXX` zero-extends | 2.5% | 1.0% | 0.9% |

- IPC in JIT code during the race is **1.07-1.12**. The 1.77 in runs/pcpmc-2 was a menu. Race
  code is bound by memory latency (guest data loads take 30% of cycles), not by instruction count.
- The traffic is flat over registers: the biggest single item is NZCV (2.2% of cycles for loads,
  2.0% for stores). After that come x0, x19 and sp loads at ~1.1% each. The pinned pair x8+x26 is
  about 1% of instructions, so the null A/B in runs/pin.res is what this predicts. Also, a pin turns
  each reload into a register copy (reg_pin.cmake GetX `mov result, r12`), so it saves the load's
  latency but no instructions.
- The code is flat: 44k distinct JIT addresses carry the cycles, and the top 64 hold 17%. On the Pro
  (dumps g0out-1 + g0map-3, 282 hot blocks), the mean is 8.6 guest instructions per block, 48% of
  blocks end in b.cond and 28% in tbz/cbz/tbnz.
- The Pro's main-path mix, statically weighted by samples, agrees: GPR/SP 5.9%, NZCV 1.2%, page-table
  walk 8.7%, guest memory 5.6% of instructions.
- Runs jm-base-2 and jm-base-3 never reached the race. The PC was at load average ~67 from other
  sessions' builds, and the time-scripted pad lost sync. Repeat PC races when the PC is quiet.

## Expected gain
Core 0 spends 63% of a race in game JIT code (morning report). If every GPR, SP and NZCV
boundary access vanished and its cycles went with it, that saves 0.63 x 11.5-18% = **7-11%** of the
critical thread (22.7 ms -> ~20.5 ms, about 48 fps at best). Realistic is lower:
- x86-64 leaves about 6-8 allocatable GPRs for guest registers after the allocator's scratch needs
  (RSP, R15 state, R14 page table, R13 fastmem, and RBX/R12 are needed for cmpxchg16b and
  terminals). Guest use is spread over ~25 registers, so a fixed block-entry contract
  covers maybe a third of the traffic.
- Part of the cycles charged to the first load in a block is the cost of fetching that block,
  which stays whatever the register handling.
- Estimate: **2-5% of the frame**.

## Design, should it ever be wanted (ranked by value/risk)
0. Cheap, local, ~1% (the only part I would do): dynarmic's If/CheckBit terminals reload NZCV from
   JitState (`EmitX64::EmitCond`: `mov eax,[r15+nzcv]`, then sahf/LoadRequiredFlagsForCondFromRax),
   so every `cmp` + `b.cond` block does a store-forwarding round trip on its branch. If the block's
   last SetNZCV value is still in a host register at the terminal, branch on it instead.
   Also the SetW zero-extend (`mov eXX,eXX` before every 32-bit store, upstream's "TODO: zext
   tracking"): 2.5% of instructions, though cheap ones.
1. Dead-store elimination at block ends: drop a guest-register store when every direct successor
   overwrites the register before reading it (guest liveness over direct branches only; indirect
   branches, SVC, calls and exception-capable code count as reads).
2. Superblocks across conditional branches (b.cond/cbz/tbz to a side exit, continuing on the
   fall-through), so GetSetElimination works over the trace. A side exit is a barrier for
   Set-Set elimination only.
3. Per-chain register allocation with a block-entry contract (the guest registers that live in host
   registers on entry, stored at every exit to the dispatcher, at C++ calls and at fastmem faults).

## Risks
- Exceptions and fastmem faults: the fault handler recompiles or falls back from inside a block, so
  every register held in a host register must be written back first, or the handler must know where
  it is (a per-site register map).
- SVCs and interrupts: the state must be exact at every return to the dispatcher (halt, SVC, a
  context switch reads all of JitState). Dead-store elimination leaves dead values stale there: the
  guest can't see them, but crash dumps and backtraces show them.
- The shared JIT across cores: blocks are shared, so any contract is global. Changing it means
  flushing and recompiling the code cache, and a block linked under one contract can never be
  entered under another.
- Upstream drift: these are deep dynarmic changes, applied as cmake text patches pinned to anchors.

## Where the time actually is
Guest data loads (30% of JIT cycles: cache misses in the game's own data) and the page-table walk on
every access (~9% of cycles, 12.5% of instructions) dominate. The second is the fastmem lever's
target (PC fastmem measured +17%: notes/a64-fastmem-console-design.md).

# Guest-memory copies on the split GPU thread's back (2026-11-04)

## What is copied (0070 counters, PC race-5 replay, 300 frames, EDEN_NX2_COPY_STATS=1/2)
| kind | per frame | unchanged since that slot's/address's last copy |
| --- | --- | --- |
| streamed uniform buffers (<= 4 KiB, every draw) | 15,800 copies, 8.5 MB | 62% of copies, 68% of bytes (same slot, same address, same bytes) |
| image uploads (UploadImageContents) | 13 images, 5.9 MB | 10% of bytes |
| ASTC opacity scans (copy, then scan) | 8, 1.7 MB | (copy removed by 0071) |
| buffer uploads (MappedUploadMemory) | 130 copies, 1.4 MB | ~0% |

Back L3-miss loads in libc memcpy by caller (lever-scout's sc2-rep2, 003aa0a): uniform streaming 28%,
image uploads 14%, ASTC scan copy 10%, buffer uploads 12%, GuestMemory reads 7%; 13% is the replay
player applying the guest's page writes (replay-only, not in the game; gputrim confirmed).

## Patches (eglantine .d, all general)
- 0070 copy counters (EDEN_COPY_STATS lines with EDEN_NX2_COPY_STATS set; =2 also checks unchanged bytes).
- 0071 ASTC opacity scan reads guest memory in place (no 1.7 MB/frame copy into a scan buffer).
- 0072 streamed uniform buffers: rebind the slot's last stream range when address, size and bytes match
  and the range belongs to this submission (the stream ring frees by submission tick, the same
  invariant every stream already relies on). EDEN_NX2_COPY_REUSE=0 turns it off. 62% of streams rebound.
- 0073 prefetch of the draw's streamed uniform sources when the pipeline sets its uniform state (before
  its texture/descriptor work). EDEN_NX2_UNIFORM_PREFETCH=0 turns it off.

## PC results (race-5 replay, RTX 3070 PC; PC timing noise ~±15% from other sessions' load)
- Checksums: equal to the 0070 baseline in 15 of 16 runs with 0071+; one run (jr-pf-off2) differed at
  list 900 only. Six repeats afterwards (3 baseline, 3 with 0071-0073) all equal; I count it as a rare
  timing-dependent flake, but it is the only mismatch seen and it was on the 0072 path.
- Back CPU (second half), interleaved A/B, normalised by the front (unchanged by these patches):
  0070 0.81/0.80, 0072 0.81/0.82/0.79: no difference measurable on the PC.
- L3-miss profile after 0072: libc memcpy still 16.7% of the back's cycles (17.7% before) and 49% of its
  L3-miss loads: the rebound copies were cache hits, the misses are the copies of data that changed.
  With 0073: libc 38-42% of L3-miss loads (47% with it off), cycles unchanged within noise.

## Pro results (race-5 replay, split on, gpu_accuracy=low, build of de211d5 on 9329792, driver f5610fe2)
Back CPU, second half (405,512 draws); one build, arms by env; 0 GPU faults in all 9 runs:
| arm | runs | back s | us/draw |
| --- | --- | --- | --- |
| 0072+0073 off | 3.124, 3.145 | 3.135 | 7.73 |
| 0072+0073 on | 3.033, 3.032, 3.031 | 3.032 | 7.48 |
| 0072 only | 3.027 | | |
| 0073 only | 3.040 | | |
-3.3% of the back's time; each patch alone gives about the same, together no more (both cut the
uniform path's misses). Pro checksums can't validate: they differ between any two runs, for every
session's builds (pic-base vs pic-p4 too). The 0072-only/0073-only pairs' second runs were cancelled
by the stage-3 Pro reservation. If one of the two is to go, it should be 0072 (more code, per-runtime
state); 0073 alone is ~15 lines and can't change results.

## Upstream candidates (PC replays, 04270d6 base + a PC-only copy of 0053, shelved/jitregs/)
- On 04270d6 PC replays need 0053's decode bound (else the video-memory watchdog stops them at 8 GB):
  shelved/jitregs/0053-pc-replay-aid.diff is 0053 with its wait under #ifndef __PROSPERO__ (no-op on
  the console), applied to my ns2-host builds only. With it, PC replay checksums are NOT reproducible
  on this base: two runs of the same binary differ at every list (jr-big2, jr-cand-A2). Before the
  revert they were (16 of 17 equal). So no checksum validation is possible on the current base.
- 073b877965 (chunked uploads of 2D images >= 64 MiB), as shelved/jitregs/0075: 4 chunked uploads per
  race-5 replay (the first frame's huge images), none in later frames; back time 2.30 vs 2.38 s (noise
  level); one of two runs still hit the video-memory watchdog (7.3 GB). Not a copy-cost lever; a
  memory-headroom one at most. Not taken.
- Readback trio c75fa8b3f + 638795281 + 06e211e0d, as shelved/jitregs/0074 (their Nx2Profile counters
  left out, one gpu.cpp counter hunk dropped): applies and builds; it changes guest-core reads of
  GPU-written memory, which the replay does not exercise, so it needs game runs (PC full race, Pro
  races). Not taken yet.

# JIT stages (2026-11-04)
- Stage A (9ec00fe, validated): B.cond terminals take NZCV from the store's register (fast_paths.cmake,
  EmitCond). eden-memory-check: 220 fast-path cases PASS (all 14 conditions x 6 operand pairs, on/off).
  PC precise samples, boot window 25-45 s (no race reachable on the PC, see below): NZCV-load cycles
  3.0% -> 0.7-1.0% of JIT-code cycles, NZCV-load instructions 1.5-1.6% -> 0.7-0.9% (2 runs each arm).
  Switches: dev-settings jit_nzcv_reuse=off, PC EDEN_JIT_NZCV=0.
- Dispatcher (measured, PC boot window): the A64 fast-dispatch/RSB terminal handlers take 3.7-5.3% of
  JIT cycles; the single probe `cmp rbx, [r12]` 2.8% (16 MiB table per core: TLB + cache miss). Branch
  misses (pmc/pmcbr, new): 12% of the JIT's mispredicts sit on the two shared handler jumps (jmp rax
  6.4%, jmp [r12+8] 5.6%), the rest are conditional branches. C++ block lookups 0.1% of cycles.
- PC state: full-game PC runs of 0308762 hit the 8 GB video-memory watchdog already at 50-170 s (7.3 GB;
  lever-scout saw 330-355 s on 7504528), with or without the PC transcode bound
  (shelved/jitregs/pc-transcode-bound-on-0044.diff, guarded #ifndef __PROSPERO__, in my build tree only).

## Where I stopped / next step (paused 2026-11-04 ~12:15 by the user's instruction)
- Branch lever-jitregs tip: see the last commit there (WIP dispatch-table commit on top of 9ec00fe and the
  0308762 merge). lever-jitregs-prefetch 0d55103 = 7504528 + 0073 only (for the coordinator).
- Next: (1) finish the B1 A/B (64K-entry dispatch table vs 1M; pc-jitmix.sh + pmc/dispatchshare.py, PC
  boot window, interleaved; runs/jr-b1.res has one B1 run: handlers 5.15%), then eden-memory-check;
  (2) Pro: Stage A and B1 on scout-trace.sh race traces + ticks.py/critpath.py and fps pairs once the
  Pro is free; (3) 0071/0072 Pro race pairs (fault counts, frames); (4) then cross-block registers.
- My build tree (.local/headless-cache 178c479ef787 source) carries the uncommitted PC transcode bound
  (no-op on the console); delete its .unpacked-from to drop it.
