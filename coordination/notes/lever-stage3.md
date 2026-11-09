# lever-stage3: a third stage for the split GPU thread (2026-11-04)

Branch `lever-stage3` (worktree ProsperoEden-stage3, from overnight-60fps 3eb7238), patches 0080-0089.
Status (2026-11-04 afternoon): **A and A2 built, verified and handed over** (lever-stage3 3756792);
**B built, correct, measured slower: dropped** (kept on lever-stage3-b); **C's real ceiling written
below, for the user's decision.** Results first; the original design and census follow.


## Results (Pro, race-5 replay unless noted; base overnight-60fps 2ded091)

| option | what | back, second half | races | verdict |
| --- | --- | --- | --- | --- |
| A (0082, f06610b) | plain 3D register writes travel in packed Regs blocks from the front | -2.3% (3 pairs) | 0 faults in 6 (3eb7238 base) | handed over |
| A2 (0085, 3756792) | constant-buffer data words batched into CbData blocks, one guest-memory write | A+A2: 2.864/2.863/2.897 vs 3.130/3.042/3.121 s, **-7.2%** | on 8.15/7.95/8.61 vs off 8.93/9.12/8.86 us/draw (**-8.2%**), 107-115k vs 101-103k draws/s, 0 GPU faults in 7 | handed over |
| B (0086, lever-stage3-b cf48b69) | read-ahead helper thread: handle words and uniform sources read ahead, validated by a write log | 3.600 s vs ~2.87 (+25%) | not run | dropped |

Gates for A and A2: draws equal in every replay; EDEN_NX2_SPLIT_VERIFY 3,521 checks, 0 differences on
and off; replay pictures on vs off worst 0.02% (Pro noise); race frame 380 by eye right. One A2 race
(s3a2r-on3) died at 134 s before the race in a guest core (CPUCore_3 SIGSEGV in the fastmem fault
handler, lever-fastmem's diagnosis: a JIT fault at no registered fastmem site; not the GPU threads).

### Why B lost (measured, not guessed)
B was correct: verify mode re-read every handle word and uniform buffer it supplied, 0 differences over
the whole window (after the replay's own page writes bump the map epoch, replay-only), packets for 82%
of draws, 72% of handle words and 81% of uniform buffers taken from them. But the phases it targets did
not get cheaper: handle reads 0.416 us/draw with packet words against 0.393 reading guest memory,
uniform streaming 1.049 against 0.978 copying from guest memory. Neither read is latency-bound on the
Pro (the same lesson as lever-jitregs' 0073 prefetch: 0 gain). Its cost is real: packet lookup and
validation ~0.65 us/draw, logging every back write plus the helper's polling of shared lines ~0.84.
So there is no read-ahead gain to harvest, at any overhead.

## C's real ceiling (for the user's decision)
C = a second back thread that runs every engine method (register state, draw-manager state, constant-
buffer data) and hands the rasterizer thread a per-draw register delta, with every rasterizer call
forwarded in order. Measured on 2ded091 + A + A2 (s3a2-on2): back 7.06 us a draw = draw path 4.61 +
everything else 2.45. Of the 2.45 (census 3, s3-cen3):
- must stay with the rasterizer: compute dispatches ~0.40 (they run the caches and record), deferred
  calls ~0.5 (fences, syncpoints, query flushes), constant-buffer binds ~0.08 (buffer cache), queries
  and clears ~0.1, inline-to-memory uploads ~0.2 (they invalidate the caches);
- movable: ring decode and register application (Regs/CbData blocks), draw-manager state methods and
  the batched constant-buffer data writes, ~0.6-0.9 us; the rasterizer thread then applies ~20
  register values a draw (~0.1-0.15).
Net: ~0.5-0.75 us a draw, **-7 to -10% of the back**; with the guest-memory writes moved ahead of the
back's own reads of the same memory (uniform streaming, buffer syncs) it needs an ordered write log or
undo to stay correct, a register-delta handoff for ~3,600 registers' worth of state, and a FrontVerify-
style check. Weeks of work and the highest-risk change in the split, for about what A2 gave.
Heavy race windows after A+A2: ~6.3 us a draw in races -> C ~5.7-5.9 us -> 3,100 draws 17.7-18.3 ms
(~55-57 fps). **No split of the back's methods reaches a locked 60 in the 3,100-draw windows.**

What remains is the draw path's own CPU work (config 3.37 us: view fill, buffer binds, uniform
streaming, descriptors), serialized on one thread by the caches and the one Vulkan command stream.
The levers there are doing less of it per draw (lever-gputrim's unchanged-state fast path, its census
5-8%) or a renderer redesign that records Vulkan commands on two threads (secondary command buffers per
render pass, the caches split by ownership), far larger than C.

## Why
In the heavy MKW race windows (~3,100 draws a frame) the split's back is the GPU thread's floor:
~6.8 us a draw is ~21 ms a frame, a ~47 fps ceiling that no wait removal lifts. A locked 60 needs
the back under 16.7 ms there: <= 5.4 us a draw, a 21% cut.

## Measured: where the back's time goes
Pro replay race-5, second half (405,512 draws, split on, usual settings), runs s3-time1 (clean
timing) and s3-cen1 (census on; same times within 1%). Back thread CPU 3.058 s = **7.54 us a draw**
(a replay costs ~10% more a draw than a race; use the shares, not the absolutes).

| part | us/draw | share | what |
| --- | --- | --- | --- |
| flush | 0.59 | 8% | FlushWork + FlushCaching: invalidations from the constant-buffer/inline writes since the last draw |
| pipeline | 0.26 | 3% | key from the registers, map lookup |
| lock | 0.05 | 1% | cache mutexes |
| config: sync | 0.04 | 1% | |
| config: **handle reads** | **0.37** | 5% | texture handles read from guest memory (cfg_handles, new) |
| config: view fill | 0.42 | 6% | FillImageViews (texture cache) |
| config: buffer binds | 0.87 | 12% | index/vertex/storage updates and binds (buffer cache) |
| config: **stage buffers** | **1.12** | 15% | BindHostStageBuffers: uniform streaming (stream allocation + guest-memory copy) (cfg_stage_buffers, new) |
| config: other desc | 0.30 | 4% | descriptor acquire, image descriptor pushes, upload barrier |
| config: rt + draw | 0.23 | 3% | render targets, ConfigureDraw |
| state + record | 0.34 | 5% | dynamic state, draw command |
| **draw path** | **4.67** | 62% | |
| deferred calls | ~0.5 | 7% | the front's Defer'd work (1.09 a draw) |
| **method entries** | **~2.3** | 31% | ring decode + CallMethod for 57.2 entries a draw, with the compute dispatches, clears and uploads they trigger (~0.3) |

Census (s3-cen1/s3-cen2, per draw): 58.3 ring entries = 8.07 sunk register writes + 20.67 plain
macro writes (**28.7 plain register writes**), 8.45 executable methods + 19.97 executable macro writes
(**28.4 executable**: draw-manager state, draws, constant-buffer data and binds, queries), 1.09
deferred calls. 19.6 distinct methods written between two draws. Plain writes do **not** coalesce:
between two ordering entries 28.72 of 28.74 are to distinct registers. (s3-cen2's per-entry clock
costs ~60 ns an entry on the Pro, so only its counts and the call time are used.)

Race cross-check (fw2-a2, 2,990-3,190 draws a frame, DEV counters): draw path 5.4-5.7 us, same
proportions (read 1.2, bind 1.0, desc 1.4, flush 0.8).

## What could leave the back, what must stay in order
| class | us/draw (replay) | items | constraint |
| --- | --- | --- | --- |
| **P: pure reads** | ~1.05 (14%) | handle reads 0.37, uniform source bytes inside the stream copies ~0.55, pipeline key ~0.1, storage-buffer address reads ~0.05 | functions of the registers + guest memory at the draw's place in the stream |
| **Mp: plain register writes** | ~0.4-0.7 (6-9%) | 28.7 entries | stores + dirty flags; the front already resolves them (FrontState regs, shadow RAM, same-value skip) |
| **Mx: executable methods** | ~1.6-1.9 (22-25%) | 28.4 entries + triggered work | call into the rasterizer (constant-buffer binds, draws, queries, clears), write guest memory (constant-buffer data, inline uploads: WriteBlockCached, flushed at the next draw) |
| **C+R: caches and recording** | ~4.2 (55%) | view fill, buffer binds/syncs, stream allocation, render targets, FlushCaching, pushes, dynamic state, record, calls | caches create/evict/upload by scheduler tick and feed one Vulkan command stream in draw order: one thread |

Dependencies between draws: registers (Mp, Mx), guest memory written in-stream (constant-buffer
data, inline uploads, rare DMA copies and buffer downloads at fences), and every cache's state
(C+R). Splitting C+R over two threads needs two command streams and a shared eviction clock: out of scope.

## Options (gain on the back; race estimate at 6.8 us and 3,100 draws a frame)
| option | moves off the back | back | race GPU-thread per frame | risk |
| --- | --- | --- | --- | --- |
| today | - | 7.54 | 21.1 ms (47 fps) | |
| 0. prefetch helper only | nothing (prefetches P's lines into the back's L3) | -0-5% | 20-21 ms | none, but prior is low: lever-jitregs' same-draw prefetch (0073) measured 0 on the Pro |
| **A. register blocks** | Mp: the front sends plain writes as one packed (method, value) entry, applied in a tight loop | -0.4-0.6 (6-8%) | 19.5-19.8 ms | low: front path only, FrontVerify compares both register files |
| **B. read-ahead helper** | P: a third thread follows the ring 8-64 draws ahead (reads, never consumes), keeps a register copy, and hands the back each draw's handle words and uniform source bytes; the back uses them only if no in-stream write, download or unmap happened since (epoch), else reads itself | -0.8-0.9 net (11-12%) | 18.6-18.8 ms | medium: verify mode re-reads and counts mismatches |
| **A+B** | Mp + P | -16-20% | **17.0-17.7 ms (56-59 fps)**; 2,500-draw windows 13.8-14.3 ms | |
| C. full register stage | Mp + Mx + P: the helper runs every engine method (rasterizer calls forwarded in order, per-draw register delta ~20 values) | -43-48% | 11.5-12.4 ms | high: guest-memory writes (constant-buffer data, inline uploads) then run ahead of the back's buffer/texture reads of the same memory; needs an ordered write log or undo; FlushCaching's accumulator and the draw manager's state become shared; weeks |

Handoff cost (B): the packet ring is on the back's CCX (front CPU 8, back 10, helper on 12 or 14,
not an SMT sibling); ~0.1-0.2 us a draw for the back to read ~0.5 KiB of packet from L3. A helper
reading the split ring between `consumed` and `published` cannot be overwritten by the front.

## Recommendation and staged plan
A+B get the heavy windows from ~47 to 56-59 fps and every lighter window under 16.7 ms; with
lever-jitregs' 0072 (-3% on the back, not merged) and gputrim's forwarding they are at the edge of 60.
Only C clears 60 in the 3,100-draw windows with margin, at a much larger risk. Plan:
1. **A, register blocks** (0082): Kind::Regs entry; FrontWrite appends plain writes, any other
   entry flushes the block first; the back applies with ProcessShadowRam + dirty flags as
   ConsumeSinkImpl does. Checks: replay draws equal; EDEN_NX2_SPLIT_VERIFY clean over a replay;
   split-off/on checksums vs the same build without it; 3 interleaved Pro replay pairs (back CPU).
2. **B, read-ahead helper** (0083-0084): register copy and draw detection from the ring, handle and
   uniform-source packets, epoch validity (in-stream writes, downloads, unmaps, address-space
   switches bump it), EDEN_NX2_STAGE3_VERIFY re-reads and counts mismatches (must be 0). Same checks,
   then a race A/B.
3. **C** only if 1+2 measure short of 60 in the heavy windows, designed as its own note first.

Coordination: A touches the front's FrontWrite and gpu_split.cpp (lever-gputrim owns the waits
there); B's uniform-source bytes feed lever-jitregs' BindStreamedUniformBuffer (0072): stream
allocation stays on the back.

## Tools
- `coordination/replay-run-reserved.sh`: replay-run.sh for the worktree the console is reserved
  for (the shared queue otherwise waits forever behind the held tickets).
- Replay lines (dev builds): `GPU replay: second half, us per draw: ...` always;
  `... back ring entries per draw: ...` with `env=EDEN_NX2_STAGE3_CENSUS=1` (adds ~60 ns an entry).
