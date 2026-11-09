# lever-scout: ranked levers toward a locked 60 fps in MKW races on the Pro

Measured on overnight-60fps 2faa894 (worktree ProsperoEden-scout) and the newest Pro races of other
levers. Refreshed after every merge. Tools: `bound.py`, `ticks.py`, `scout-trace.sh`, `pc-tlb.sh` +
`pmc/pmctlb` (this folder).

## How a race frame is limited (2026-11-04)

1. **Most race windows are GPU-thread-bound, not guest-bound.** Per 5 s window: the GPU thread's busy
   time per frame and the MainThread's wait on GPU fences ("sync") per frame (`bound.py heap.txt`):

   | run (Pro, newest builds) | race windows | GPU-bound | mean fps |
   | --- | --- | --- | --- |
   | gt-r53a (gputrim) | 15 | 7 | 44.5 |
   | cr-v9 / cr-v10 / cr-v11 (crash fix, fastmem) | 20 / 14 / 18 | 13 / 13 / 13 | 48.3 / 44.7 / 47.5 |
   | hi-a1 (hitches) | 20 | 20 | 41.9 |
   | g2-learn (fastmem) | 16 | 13 | 47.8 |

   GPU-bound windows: 2,000-3,200 draws a frame, GPU thread busy 18-28 ms a frame, MainThread waits
   1.5-9 ms a frame on fences, game speed 81-91% (the game slows down instead of catching up).
   Guest-bound windows (1,650-1,750 draws): 47-57 fps, GPU thread busy ~16-17 ms (already at the edge).
   So for 60 the GPU thread must handle ~3,100 draws in 16.7 ms at the race start: ~5.4 us a draw
   (now ~8.2-9), a 35% cut, and ~10% even in the light windows.

2. **The guest gap is smaller than the mean frame says.** MKW runs a fixed 60 Hz game tick; when
   behind it runs two ticks in a frame and presents it with interval 2 (patch 0040 logs it). Frames
   therefore alternate between 1-tick (S) and 2-tick (L) frames (`ticks.py sched-trace.bin`):

   | Pro trace | S (1 tick) | L (2 ticks) | tick T | critical thread 182 per tick | model fps | measured |
   | --- | --- | --- | --- | --- | --- | --- |
   | tr-2 (2026-11-02) | 21.30 ms (47%) | 29.02 ms | 7.72 ms | 5.17 ms | 39.5 | 39.3 |
   | tr-1 (2026-11-02) | 22.01 ms (38%) | 29.40 ms | 7.38 ms | 5.15 ms | | 37.5 |

   A locked 60 needs S < 16.7 ms, not the mean frame. fps is non-linear in the work: with T/S fixed,
   cutting all guest work 10% takes tr-2 from 39.5 to 47.7 fps, 15% to 52.5. In guest-bound windows
   today S is 17.2-19 ms (from each window's mean frame and its share of 2-vblank frames, with T/S as
   in the traces; gt-r53a predicted 23% / 18% double frames, logged 21% / 17%): the guest is 3-12%
   from 60 there, not 25-32%.

3. **Memory system (PC counters, `pc-tlb.sh` / `pmc/pmctlb`, `pmc/pmcmiss`).**
   - Guest cores (sc-tlb2, 300-334 s, fastmem on): dTLB walks 1.2%, iTLB 0.7% of cycles. **Correction
     (01:40): not a race.** On the PC the merged build is still in the course load at 300-334 s (no
     presents for 13 s at 285 s in sc-smtn-n1, ~1 frame a second at 345-365 s in sc-tlb3), so these
     are loading numbers. The TLB lever is unmeasured in a race; it stays low on priors (the Pro's
     IPC-1.1 JIT code is data-latency bound, jitregs' PEBS mix), not on a measurement.
   - GPU back thread, race-5 replay, second half (sc-rep1, sc-rep6, sc-rep8): IPC 1.31, 41% of
     cycles stalled on memory, **27% on L3 misses** (front: 2%). Half the back's L3-miss loads
     (49.5%) are in libc memcpy's 256 B-2 KiB loop, 16% of its cycles. LBR call stacks of those
     misses: BufferCache::BindHostGraphicsUniformBuffers 31%, Tegra::MemoryManager::ReadBlockImpl
     22%, a std::function 15%, DeviceMemoryManager::WalkBlock 13%. So ~11% of the back's cycles are
     copying guest memory (uniforms, reads) that is not in cache. On the Pro the source was just
     written by the guest cores on the other CCX (guest on CPUs 0-7 = CCX0, front 8 / back 10 =
     CCX1) and a CCX has 4 MB of L3 (PC: 12 MB), so expect a larger share there.
   - The full-game PC race is stopped by the video-memory watchdog at the course load (~330 s) even
     at TC caps 1536/2560 (sc-tlb1, sc-tlb2, and two other sessions' runs tonight): PC full-game
     profiling past 330 s needs lower caps or a smaller desktop footprint.

## The guest's critical path in a 1-tick frame (2026-11-04, Pro traces of 04270d6 + fixed driver)
Tools: `critpath.py` (walks each frame back from the present through "ran", "waited for a core",
"preempted" and "woken by" links; the trace has no waker, so a wake is attributed to one of the guest
threads running at that instant, weighted by how often each wakes that thread over the whole trace;
none running = external), `guestcost.py` (emulator counters over the traced windows). Traces sc-rv-2/3/4
and sc-smtr-a1 (default placement, same race stretch), 1-tick frames 17.7-18.4 ms.

Path per 1-tick frame, by link kind (stable across the four traces):

| link | ms per 1-tick frame | what it is |
| --- | --- | --- |
| guest code running | 14.0-14.9 | the game's threads in the JIT (below) |
| preempted | 2.0-3.2 | a path thread runnable but displaced by another guest thread on its core |
| external wait | 0.45-0.71 | woken with no guest thread running: GPU fences, IPC replies, timers (MainThread most) |
| waiting for a core | 0.24-0.37 | runnable to running |

Threads on the path (run time per frame): 182 BaseProcExecutor0 4.4-7.0 ms, 116 0.9-4.8 ms (the split
between 182 and 116 moves from trace to trace: wake attribution is ambiguous, 73% of wakes have 3+
guest threads running), MainThread 86 1.0-1.5 ms, then 107, 189 ModuleSystemWorker2, 183, presenter 32,
0.3-0.7 ms each. Chain, most frequent links per frame: 32 <-> MainThread (present handshake, 4-5),
ModuleSystemWorker1/2 <-> MainThread (2-3), 182 <-> 183 (job ping-pong, 1.6-2.4), Presentation 90 <-> 32.
Cores (the game's affinity): 182 and MainThread both on guest core 0 (500 + 226 ms/s: they serialize),
job threads 183-187 one per core 1-5 (315-420 ms/s each), 116 floats over 1-5 (135 ms/s), 127 core 1,
141 core 4, presenter 32 core 3.
Preemption is plain core contention: while 116 was preempted (70-130 ms/s), another of its cores was
idle only 1-2% of that time; 127 and 141 are single-core. No scheduler or migration lever there.

Emulator costs inside the run time (counters over the traced windows, guest cores):
- reads/writes of GPU-tracked memory: 8-26 + 24-40 ms/s, ~1-1.6% of guest busy time, ~0.15-0.25 ms a frame
- fastmem64 bookkeeping (track/protect/fault census): 2.0-2.4% of guest run time, ~0.3-0.4 ms a frame
- JIT compilation ~1 ms/s per core (~0.02 ms a frame); fastmem faults ~1/s; dispatcher misses ~1-5k/s
- MainThread IPC: ~23 us a call, ~4-5 calls a frame, ~0.1 ms a frame
So ~13.5 ms of the 14.5 ms run time is the game's own code; earlier core-0 sampling found it flat (hottest
block 0.9%, hottest 64 KiB ~15%): no single routine.

### Guest levers ranked by ms removed from the 1-tick path (60 needs 1.1-1.7 ms off ~18 ms)
1. **JIT code speed of the game's threads**: ~17 ms of the path is guest code (14.5 ms run + 2.6 ms
   preempted by other guest threads that are also running game code), so 1% faster JIT code ~0.17 ms.
   Known items: cross-block guest registers 2-5% (~0.35-0.85 ms), NZCV kept in host flags at b.cond
   terminals ~1% (~0.17 ms), dispatcher/terminal overhead ~3% of core 0 on an older build (<= 0.5 ms).
   Owners: lever-jitregs, lever-fastmem. This is the only guest lever that reaches 1.1-1.7 ms.
2. **External waits** (MainThread on GPU fences / IPC replies): 0.45-0.71 ms; shrinks with GPU pipeline
   latency (stage 3, gputrim), not a guest change.
3. **fastmem64 bookkeeping + GPU-tracked memory access**: ~0.3-0.4 + ~0.15-0.25 ms (lever-fastmem).
Not levers: guest scheduling / placement (contention, see above; the SMT pin moved time from
"preempted" to "external" without shortening the frame), JIT compilation (0.02 ms), IPC (0.1 ms).

## Refresh 2026-11-04 07:00: overnight-60fps 3eb7238 (lever-hitches 0043-0045; 0050-0053 reverted at 04270d6)
No new Pro measurement at this tip (console reserved for ProsperoEden-stage3). Ranking, updated:
1. **GPU pipeline throughput** (front/back serialization, now also the third GPU-thread stage): still
   the limit in most race windows; the clean 04270d6 runs agree (sc-rv-2/3/4: 45.3-46.5 fps, GPU-bound
   bands 2,100-4,000 draws at 36-44 fps, MainThread waiting 3-8 ms a frame on fences there). Owners:
   lever-gputrim (report-word drains, 63% of the front's drain wait) and the stage-3 session.
2. **Guest-memory copies on the back** (~11-18% of its cycles, cache-miss bound; PC replay): unchanged,
   still to be passed to gputrim.
3. **Guest side in the light windows**: the 1-tick frame on 04270d6 is 17.7-18.4 ms (4 traces, same
   stretch), need -6 to -9%. The SMT-sibling pin cut the critical thread's own run time 12% but not the
   frame (see the A/B above): not a lever as tried; the critical path runs through other job threads.
4. RADV -O3 -march=znver2: packages ready (build/pkg-o2, build/pkg-o3 at 3eb7238), queued as
   `pro-radv.sh sc-radv 3` (2 replay pairs, then 3 race pairs) behind the reservation.
5. TLB / 2 MiB pages: unmeasured in a race (no PC race reachable), low on priors.

## SMT-sibling A/B on the Pro (2026-11-04, 04270d6 + fixed driver isolated from 694ec6)
Arm a: default placement (CPUCore_4/5 float on 1,3,5,7,12; CPU 1 is guest core 0's sibling). Arm b:
pin=CPUCore_4:5 pin=CPUCore_5:7. `pro-smt.sh`, `abwin.py`; sched trace 230-245 s after the guest's first
switch. Runs: a = sc-rv-2/3/4, sc-smtr-a1/a2 (5); b = sc-smtr-b1/b2 (2). Faults on this build: a 0 of 5
(0 of 6 counting the warm-up sc-rv-1), b 0 of 2. (On ebfa563, with 0050-0053: a 2 faults in 3, b 1 in 2.)

Race windows matched by draws per frame (`abwin.py`):

| draws/frame | arm a fps (windows) | arm b fps (windows) | b/a |
| --- | --- | --- | --- |
| 1500-1800 (guest-bound) | 52.5 (39) | 53.0 (6) | 1.01 |
| 1800-2100 | 44.8 (10) | 50.9 (5) | 1.14 |
| 2100-2500 | 43.9 (27) | 51.2 (5) | 1.17 |
| 2500-2900 | 38.6 (15) | 39.2 (8) | 1.02 |
| 2900-4000 | 36.2 (20) | 37.8 (7) | 1.05 |

Traced 1-tick frames in the same race stretch: a 17.69 / 18.02 / 18.39 / 17.81 ms, b 18.40 ms (b1; b2's
and a2's traces fell in a heavier stretch). The critical thread 182 runs **7.81 ms** per 1-tick frame
with the pin against 8.79-8.96 ms in all four default runs (-12%): the sibling contention is real for
that thread, but the frame does not get shorter, so its critical path is not thread 182's run time
alone (the pinned CPUCore_4/5 now share cores with guest cores 2/3).

Verdict: **not shown; do not make it the default yet.** Where the guest limits (1500-1800 draws) the pin
is +1%; the +14-17% in the middle bands rests on 5 pinned windows from 2 runs. Missing: 3 more clean
pinned runs (b3 is queued behind the console reservation) to resolve +-3% in the guest-bound band, and a
trace per arm in the same stretch. Next variant worth a run: move only the busiest secondary off CPU 1
(or pin CPUCore_4/5 to CPU 12 + one sibling), since pinning both onto guest cores 2/3 moves the contention.

## Stability of the merged branch (my runs)
| run | build | settings | end |
| --- | --- | --- | --- |
| sc-pro1 | 2faa894 | usual + sched trace + pc_sample back | GPU_FAULT_PAGE_FAULT_ASYNC at 222 s (TCP read, unmapped VA 0x428b84e000, after "562 MiB in 4 pieces") |
| sc-pro2 | 2faa894 | same | GPU_FAULT_PAGE_FAULT_ASYNC at 226 s, in the race (VA 0x42fb7f1000) |

Pro, merged branch: 2 runs, 2 course-load GPU faults, 0 clean ends. PC (not crashes): sc-tlb1 and
sc-tlb2 were stopped by pc-run's video-memory watchdog at the ~330 s course load (TC caps 2048/3072
and 1536/2560).

## What grows in the shared memory pool through the loads (2026-11-04 10:00)
Pro, from the emulator's own accounting (`poolsplit.py runs/<tag>`: EDEN_PERF_DIRECT guest_held and
graphics_held = what VMA holds in its blocks, the texture cache's own usage estimate, and the nx2:gpu
"GPU-only memory in use" log lines). sc-rv-2 (04270d6 + fixed driver, clean race), MiB:

| time (s) | phase | guest memory | VMA blocks | GPU-only (sparse) | free | largest free |
| --- | --- | --- | --- | --- | --- | --- |
| 85 | title | 3,080 | 996 | 0 | 6,631 | 6,170 |
| 145 | world loaded | 5,176 | 3,749 | 2,189 | 1,831 | 1,408 |
| 185 | race course load | 5,390 | 3,766 | 2,190 | 1,424 | 432 |
| 270 | racing | 5,466 | 3,653 | 3,064 | 1,468 | 201 |

- **GPU-only sparse backing is the category that grows most and never comes down**: 0 -> ~1.5 GB at
  the world load (130-140 s, 256 MiB steps within a second), -> ~3.06 GB through the race (206-287 s),
  3.0-3.2 GB at the end, in all four clean runs (sc-rv-2/3/4, sc-smtr-a1: end 3,102 / 3,117 / 3,033 /
  3,199 MiB). Released over a whole run: 28-33 MiB. The game's sparse regions (~20 GB of address space)
  are backed 4 MiB at a time when the GPU first writes a block (nx2_gsv BackSparseWrite,
  SparseBlockSize = 4 MiB), and a backed block is returned only when its whole region is destroyed
  (FreeReleasedRegions); the regions live for the session. The game's unmap command (213,
  NormalUnmapGpuMemory4K, ~1,100 a run) drops only the GPU mapping (Nx2GpuMemoryRegistry::Unmap), not
  the backing. So every 4 MiB block the GPU ever wrote stays backed: either the game keeps using all of
  it, or this is backing held longer than needed. Not measured yet: how much of the 3 GB the GPU still
  reads late in the race, and how much is lost to the 4 MiB granularity. Both decide whether returning
  blocks (on the game's unmaps, or blocks no live image/buffer covers) is worth it. The same 3 GB is
  what the texture cache's evictions compete with (VMA blocks dip 3.75 -> 2.89 GB at 165 s).
- Guest memory backing: 3.1 -> 5.2 GB at the world load, 5.4-5.5 GB after; flat in the race.
- VMA blocks (textures, buffers, staging): 1.0 -> 3.7 GB at the world load, flat after with dips
  (evictions); the texture cache's own estimate is 4.7-4.9 GB (it counts guest sizes, not VMA's bytes).
- Free memory: 6.6 GB -> 1.3-1.5 GB, and the largest free block 6.2 GB -> 0.2-0.4 GB (fragmented).

## What fills the PC's video memory (2026-11-04 11:00, d287a90, sc-vkm4: vkmem layer, a boot that did not stall)
Device-local live memory by Eden caller (`vkmem/vkmem.py runs/sc-vkm4`), MiB:

| t (s) | 15 | 45 | 90 | 135 | 173 |
| --- | --- | --- | --- | --- | --- |
| texture cache images (TextureCache::JoinImages; VMA blocks + dedicated) | 187 | 4,274 | 5,510 | 3,259 | 5,835 |
| buffer cache (UpdateVertexBuffer etc.) | 0 | 480 | 480 | 480 | 544 |
| presentation, scheduler | 27 | 27 | 27 | 27 | 27 |
| host-visible staging (StagingBufferPool; dedicated transients up to 0.5 GB, freed) | 256 | 904 | 264 | 296 | 264 |

- It is the texture cache, and it is PC-specific: on the PC, ASTC images are decoded on the GPU to
  uncompressed formats ("8192x8192x68 ASTC image would be 23210 MiB decoded"), while the cache's budget
  (TC caps 1536/2560 MiB) counts guest sizes, so real image bytes run at ~2x the critical cap and the
  caps cannot hold it. The cache does evict (5.5 -> 3.3 GB at 135 s) and grows back: not a leak.
  The Pro transcodes ASTC to BC instead, so this is not the Pro's growth (that is the GPU-only sparse
  backing, above).
- Buffer cache: flat at 0.48-0.54 GB on 2ded091+ (with 0061); host staging: transients come back down.
- To make PC races fit the RTX 3070 (and the PC a race instrument again): count decoded bytes in the
  PC texture budget, or transcode ASTC to BC on the PC as on the Pro. Not mine (product code).
- This boot did not stall (PC_QUIET_DUMP=60 left no dump); the watchdog stopped it at ~170 s.

## 0061 re-check (2026-11-04 10:30, 2ded091): pending on the Pro
PC: both vkmem runs at 2ded091 (sc-vkm2, sc-vkm3) stayed on the boot loading map: no GPU-only region was
ever created (no world load), logs quiet after 57-88 s, 2.4-2.9 GB for 400 s. With sc-vkm0/vkm1 and
sc-pct4 that is 5 of the last 6 PC boots stuck there, with and without the layer, so the PC cannot
answer it. The queued traced Pro races (runs/sc-tip.sh, after the driver A/B) now install the 2ded091
package (build/pkg-tip, fix linked); `poolsplit.py runs/sc-tip-N` against sc-rv-2/3/4 is the re-check.

## PC instrument state (2026-11-04 10:00, 7504528)
Full-game PC races still cannot be traced. Correction to 08:50: those times were measured from before
pc-run's lock wait. Relative to the run's own start, sc-pct3 used 1.7 GB at 0 s, 5.5 GB at 60 s, 6.9 GB
at 150 s, 7.35 GB at 210 s, and the 800 MiB watchdog floor stopped it at ~215-230 s (desktop ~0.85 GB),
so no trace window after ~220 s is ever written. Caps 1024/2048 do not change the peak (sc-pct2,
7.48 GB). Other runs of the same build sit on the boot loading map for ~150 s with no presents and never
grow (sc-vkm0: 3.2 GB peak, map at 120 and 300 s; sc-pct4): the PC boot is not repeatable either.
New tool for the PC: `vkmem/` (Vulkan layer: live device memory per call stack, every second;
`pc-vkmem.sh`, `vkmem/vkmem.py` names the Eden caller: TextureCache::JoinImages, BufferCache::Update*,
StagingBufferPool). sc-vkm1 (a stuck-boot run, so no race): at 401 s 2.9 GB device-local (images 2.4 GB,
1 GB of it dedicated images made at ~390 s; buffer cache 0.4 GB) plus 0.87 GB of host-visible staging
(peak 0.54 GB of dedicated staging buffers that went back down to 0.1 GB).

## PC instrument state (2026-11-04 01:40, 003aa0a)
Full-game PC races of the merged build do not reach a usable race: TC caps 1536/2560 run into the
video-memory watchdog at the course load (~330 s; sc-tlb1, sc-tlb2); caps 1024/2048 crawl at ~1 frame
a second at 345-365 s (sc-tlb3) or go silent from ~125 s with no present after that (sc-shot1, no
screenshot completed at 200 s). The PC SMT-sibling A/B (pc-smt.sh, pcplace.py) is ready but has no
race to measure. Replays (pc-replay.sh) are fine. Two traps fixed on the way: a run killed by its
timeout left the emulator running under an NS2_WRAPPER (bwrap's --die-with-parent kills only the
wrapper), holding ~5 GB of video memory; pcplace.py, pmctlb and pmcmiss now give the app a
parent-death signal. And a killed run frees its video memory minutes after pc-run releases the
lock, so the next run's free-memory check fails: pc-smt.sh waits for >= 6000 MiB free.

## Refresh 2026-11-04 00:55: overnight-60fps 003aa0a (+ 6046dfa, gputrim 0050-0052)
PC race-5 replay, second half (sc2-rep1 counters, sc2-rep2 L3-miss samples): back L3-miss stalls
24.7% of cycles (24.2% before), memory stalls 41%; libc memcpy 17.7% of the back's cycles and 47% of
its L3-miss loads (16.2% / 49.5% before). Replay CPU times were not compared: two runs of the same
build gave 3.87 and 2.71 s for the back (PC load), so only the Pro replay can resolve 0.7%. Ranking
unchanged; the Pro gate (course-load GPU fault) is unchanged too, 6046dfa does not carry the RADV fix.

## Ranking (2026-11-04 00:50, overnight-60fps 2faa894; still valid at 003aa0a)

"Need" = the cut 60 fps asks of that side; "share" = the measured share of the frame the lever attacks.

| # | lever | share it attacks | realistic gain | cost / risk | owner |
| --- | --- | --- | --- | --- | --- |
| 1 | **GPU pipeline: front/back drain serialization** (front waits for the back on MME DMA reads and puller acquires, the back then idles for the front) | front: ~35% drains for MME DMA reads + ~12% acquires; back ~24% idle waiting for the front (gt-rprof1). The GPU side limits 77% of race windows (79 of 103, table above); need there: -10% (2,000 draws) to -35% (3,100 draws) of GPU-thread time per frame | if front and back overlap fully, a frame costs the back's work: ~-20-25% GPU time per frame, i.e. ~41 -> ~52 fps in the heavy windows and 60 in most light ones | medium-large: needs a safe "no pending GPU write to this read" proof (report words, 0024's pending writes); a wrong answer is a stale macro parameter (the 0026 course-load GPU fault history) | lever-gputrim (it names it as its next lever but works per-draw costs): **bigger than a per-draw item, worth a session of its own** |
| 2 | **GPU back: guest-memory copies** (uniform-buffer streaming, MemoryManager::ReadBlockImpl, DeviceMemoryManager::WalkBlock) | ~11% of the back's cycles on the PC replay, with 27% of all back cycles stalled on L3 misses; larger on the Pro (source written by guest cores on the other CCX, 4 MB L3 per CCX) | 5-10% of the back's time (fewer/fused copies, skip unchanged uniform ranges by hash or page-dirty bits, prefetch the source) | medium; correctness is local (uniform data must still be a snapshot at draw time) | lever-gputrim (per-draw); the cache-miss angle is new to it |
| 3 | **Guest side in the light windows** (1,650-1,750 draws, guest-bound) | 1-tick frame S = 17.2-19 ms there (tick model, `ticks.py`), need -3% to -12%; fps is non-linear (each -5% of guest work ~ +4-5 fps there) | candidates, not yet measured on this build: SMT sibling of the critical guest core (CPU 1 hosts CPUCore_4/5 and other secondaries; A/B secondary_mask without CPU 1, dev setting only), JIT dispatcher ~3%, GPU-tracked memory reads on guest cores ~2.5%, NZCV-in-register at b.cond terminals ~1% (lever-jitregs item 0) | low (placement: a dev setting) to medium (JIT) | placement: nobody; JIT: lever-fastmem/lever-jitregs |
| 4 | 2 MiB pages for guest memory / JIT code (TLB) | not measured in a race (the PC counters were taken during the course load, see 3.) | probably small (prior only) | medium (PS5 direct-memory mapping alignment) | nobody; not worth a session now |
| 5 | RADV built -O3 -march=znver2 (now -O2 -g, NDEBUG, asserts off) | RADV's share of the GPU threads not measured on the Pro (both profiling races crashed) | likely 1-3% of the GPU threads | low (rebuild + re-pin, shared archive: coordinator's call) | nobody |

Gate for all Pro measurement: on overnight-60fps both my profiling races (sc-pro1, sc-pro2: scheduler
trace + back-thread sampler) ended at 222-226 s in the course-load GPU fault (TCP read of an unmapped
page right after "radv/ps5: 562 MiB in 4 pieces"). lever-crash's fix (PS5_Mesa f1436b69, 0 faults in
cr-v1..v11) is not merged yet; until it is, Pro races of the merged state are unreliable (2 of 2 of mine crashed).

Pending (next refresh, once the crash fix is merged): the guest-core SMT A/B, `pro-smt.sh <tag>
../ProsperoEden-scout 3` (arm b: pin=CPUCore_4:5 pin=CPUCore_5:7; each run also traces 270-285 s, so it
measures S and T on the current build); the back/front sampler for RADV's share.

## Owned levers, as measured here
- lever-gputrim (GPU thread per draw): **the biggest lever by far**, and bigger than a per-draw trim:
  the GPU side limits 77% of race windows (47-100% per run). Its drain serialization (#1) is worth more
  than the remaining per-draw costs.
- lever-fastmem, lever-jitregs (guest JIT): only the guest-bound windows (~23% of the race), and
  there 3-12% from 60.

## Where I stopped (2026-11-04, paused until Saturday by the user)
- Worktree ProsperoEden-scout: detached at overnight-60fps d287a90, clean, no branch of its own (no product
  commits). Packages kept: build/pkg-o2 and build/pkg-o3 (3eb7238, RADV -O2 shared vs -O3 znver2 release
  copy in build/radv-o3), build/pkg-tip (2ded091); build/dev is the 04270d6 package. build/radv-isolated
  is back on the shared archive.
- Cancelled before they reached the console: the pinned SMT run sc-smtr-b3r, the RADV A/B (pro-radv.sh
  sc-radv 3), the two traced tip races (runs/sc-tip.sh). Finished: sc-smtr-a3 (default arm, 45.9 fps,
  clean; trace in runs/sc-smtr-a3, not yet in the A/B tables).
- Next steps, in order: (1) the RADV -O3 A/B: `pro-radv.sh sc-radv 3` (it waits for a "b3r-done" line
  in runs/sc-smtr.sh.out; add it by hand if the b3 re-run is dropped); (2) Pro pool and critical path on
  the current tip: rebuild build/pkg-tip there, then runs/sc-tip.sh (or scout-trace.sh + poolsplit.py +
  ticks.py/critpath.py); (3) SMT A/B: 3 more clean pinned runs before any verdict, plus a3 in abwin.py;
  (4) the GPU-only sparse backing question: how much of the ~3 GB the GPU still touches late in a race.
- PC: race traces need the texture budget to count decoded ASTC bytes (or ASTC->BC on the PC), see
  "What fills the PC's video memory".
