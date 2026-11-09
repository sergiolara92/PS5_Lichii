# lever-hitches: long frames in Mario Kart World races (Pro)

Branch `lever-hitches` (worktree ProsperoEden-hitches), from overnight-60fps 8b36c60.

## 2026-11-04: what the existing logs say
- f1-f3 runs: the 400-640 ms `worst_ms` is in the race's first 5 s window (after the course
  load); later windows 30-130 ms. vi2 "last 600 presents": max 562 ms with the present's GPU
  fence wait max 6 us, so the game itself goes that long without presenting.
- JIT compiles: <20 ms over all cores in the 577 ms window. Not the cause.
- Pipelines: 4166 loaded from disk at boot; vk graphics_pipeline/shader timers stay 0 in races.

## Attribution added (d075d7d, 8904363)
- Patch 0043: `eden_nx2_present_hook` at each game present. Dev builds print `EDEN_HITCH` for
  every frame over 50 ms: GPU thread idle/draw/pipeline/flush/GC drains, guest waits by reason,
  guest reads/writes of GPU memory, fs, JIT, fastmem faults, per-core idle; `vulkan_cost=on`
  adds `vk_*` API times. `hitch_pc=on`: GPU/GPUBack PC + callers every 4 ms during frames over
  60 ms (`EDEN_HITCH_PC`). Summary: `coordination/hitches.py runs/<tag>/heap.txt`.

## hi-a1 (one Pro race, vulkan_cost=on): frames over 50 ms with >=1000 draws
- In every long frame all six guest cores are idle for ~the whole frame: the game waits.
- GPU thread inside draws: 11 frames, 2.9 s (1002, 642, 472, 262 ms). Pipeline/flush < 20 ms;
  vk semaphore_wait 100-360 ms of it; the rest of the draw time is not yet named.
- GPU thread busy outside draws (not idle): 5 frames, 1.0 s (748 ms): guest threads blocked
  reading GPU-tracked memory (cpu_read 1440 ms summed) behind it.
- Fastmem fault storm: 1 frame of 736 ms with 123k fastmem faults, GPU idle (pre-race, present 1033).

## 2026-11-04: causes named (hitch_pc samples, hi-p1 and hi-t1)
1. **ASTC transcode bound** (top): in hi-p1's 557/468 ms race frames the GPU thread's back sat in
   MasterSemaphore::Wait under TextureCacheRuntime::TranscodeAstcToBcn: the bound on RGBA decode
   copies was `scheduler.Finish()` every 16 pending, a whole-GPU wait every 16 new textures
   (vk semaphore_wait 195-363 ms per long frame). Fix (d02c5c3 as a probe; on 0053 it is patch
   0054, 6c42678): bound by bytes only (256 MiB), wait for the oldest copy, submit every 32 MiB.
   hi-t1: semaphore waits in long frames 1-3 ms; worst race frame 329 ms (hi-p1 557, hi-a1 748).
2. **ASTC opacity scan** (after 1): 46 % of the back's samples in hi-t1's long race frames,
   `AstcDataLooksOpaque` reading every block bit by bit. Fix 0045 (03cfebf): block headers from a
   2048-entry block-mode table; same answer on 20M random blocks, 5.5x faster. Not yet on the Pro.
3. Smaller, not fixed: RADV allocate/free of each transcode's decode image (~3 %), extra submits
   from the 32 MiB flushes (vk_submit ~35 ms in a 300 ms frame).
4. Seen once each, not yet repeated: a 736 ms frame with 123k fastmem faults (pre-race, hi-a1),
   and a 748 ms frame with the GPU thread busy outside draws while guests waited in cpu_read.
Note: the GPU front thread in all of them sits in a macro (FrontRunMacro) waiting on the back.

## 2026-11-04: interleaved Pro pairs (gpu_accuracy=low cram_fine=on cram_dedup=on)
Base = overnight-60fps 9329792 (worktree ProsperoEden-hitches-base), fix = lever-hitches 3878f2a
(the same + 0043 attribution, 0044 + probe byte bound on transcodes, 0045 opacity table). Both
elfs checked for radv_ps5_retired_free. Race windows only (`coordination/hitchcmp.py`):

| run | fps | worst frame | windows >200 ms | 3-vblank | 4+-vblank | GPU faults |
| --- | --- | --- | --- | --- | --- | --- |
| hq-base1 | 46.5 | 462 ms | 2 | 54 | 17 | 0 |
| hq-fix1 | 49.6 | 141 ms | 0 | 7 | 12 | 0 |
| hq-base2 | 47.7 | 422 ms | 1 | 24 | 8 | 0 |
| hq-fix2 | 48.7 | 255 ms | 1 | 3 | 7 | 0 |
| hq-base3 | crashed at 202 s, before the race | | | | | 1 |
| hq-fix3 | 47.8 | 172 ms | 0 | 3 | 4 | 0 |

Race pictures (380/410 s) match between the arms; no texture glitches. Not yet measured: the
opacity table alone (it shipped together with the bound). Still open: the residual 140-255 ms
frames (hitch_pc on the fix build names them), the one-off 123k-fastmem-fault frame and the
748 ms cpu_read frame seen in hi-a1.
Tools: hitches.py (EDEN_HITCH summary), hitchpc.py (EDEN_HITCH_PC symbolized), hitchcmp.py.

## 2026-11-04 (round 2, on 3eb7238): what is left
- hq-fix1-3 and hr-a1: the long race frames left (92-317 ms, 5-8 a race) are almost all "GPU
  thread inside draws", and all in the race's first ~15 s (presents ~5,800-6,600, 226-247 s):
  per 600 frames there 2,774-2,893 images created (~2,000 ASTC transcodes) and 1,100-1,300
  costly ones deleted (the memory-short collector, 10-frame age). With vulkan_cost on, those
  frames spend 80-175 ms in vkQueueSubmit (PS5 RADV ring: a submit waits for the GPU when the
  16 MiB ring would wrap onto unrun words) and 20-60 ms in semaphore waits: the GPU is behind.
- One ~100 ms JIT-compile frame after the race (results screen), GPU idle.
- cd5b764: EDEN_HITCH adds gpu_busy (gpu_time=on), transcodes and transcode_mb; hitch_pc also
  samples VulkanWorker. Runs hr-b1/hr-b2 queued (Pro reserved for stage3) to confirm GPU-bound.
- Told lever-crash: rebuilding just-evicted ASTC textures looks like the biggest lever left.

## 2026-11-04 (round 2): race-start transcodes off the critical path, design and PC data
- Design (textures never black): on first use, decode ASTC only from the first level of at most
  256x256 on, fill the larger levels with a linear upscale of it (a blit in the RGBA decode copy),
  BC-encode all levels as now: the texture is complete at once, only blurry. Then refine the large
  levels over the next frames under a per-frame texel budget, from a kept copy of the ASTC data;
  cancel on delete/re-upload, refine at once before the image is a GPU copy source (joins), so no
  blurry level spreads to another image. No view or min-LOD change (textureSize/texelFetch stay right).
- Step 1 only (coarse, never refined) is in lever-hitches 376d621 as dev-settings astc_coarse=on
  (0056, off by default): it bounds what GPU ASTC decode costs the race start.
- PC replay race-5 (worktree ProsperoEden-hitches-pc, branch lever-hitches-pc = lever-hitches +
  lever-gputrim 2c7f9bd for the replay's memory; per-list GPU-thread times added): warm runs,
  coarse 8.00 s GPU work, worst list 1,309 ms, 3 lists over 50 ms; base 7.94 s, 1,527 ms, 3.
  wallprof of the burst: the back's busy time is image creation, half of it CPU UnswizzleTexture
  (the PC build lacks the port's Vulkan adaptations; on the Pro unswizzle was 3-6 %). So the PC
  does not stand for the Pro's burst; the Pro run hr-c1 (astc_coarse=on, gpu_time=on) decides.
- Queued on the Pro (stage-3 reservation): hr-b1, hr-b2 (default, gpu_busy and transcode MB per
  long frame), then hr-c1 (astc_coarse=on).

## 2026-11-04 ~12:00: PAUSED (user's instruction, until Saturday). Where I stopped / next step
- lever-hitches 376d621: 0043 attribution (+ gpu_busy, transcodes in cd5b764), byte-bounded
  transcodes (probe + 0044), 0045 opacity table, 0056 astc_coarse=on experiment (off by default).
  hr-b1 ran (default settings + vulkan_cost hitch_pc gpu_time): 49.4 fps, worst 153 ms, 3-vblank 7,
  4+ 6, 0 GPU faults; its EDEN_HITCH lines now carry gpu_busy and transcode_mb (not analysed yet).
  hr-b2 and hr-c1 (astc_coarse=on) were cancelled before they ran.
  Next: analyse hr-b1's long frames (gpu_busy vs frame, transcode_mb), then run hr-c1; build the
  refine step (design above) only if astc_coarse shrinks the race-start long frames.
- lever-hitches-pc 4543928 (worktree ProsperoEden-hitches-pc): PC-only experiment branch, not for merge.
- lever-tcbytes 6f36228 (worktree ProsperoEden-tcbytes, off overnight-60fps 7897329): patch 0086,
  the texture cache budget counts each image at its allocation size (Vulkan MemoryBlock().size),
  estimate otherwise; EDEN_NX2_TC_GUEST_BYTES=1 / dev-settings tc_guest_bytes=on keeps the old one.
  Not built yet (build stopped at the pause; the tree must re-unpack: .unpacked-from removed).
  Next: make ns2-dev and ns2-host; PC full race at TC 1536/2560 (pc-run.sh, pad as
  pc-race-prof.sh, NS2_RUN_SECONDS=480), check pc-vram.log stays under the watchdog floor 5+ min;
  then a Pro pair (tc_guest_bytes=on vs default) for fps/faults/draws.
