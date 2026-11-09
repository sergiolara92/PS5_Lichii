# ProsperoEden-crash (branch lever-crash): the course-load GPU page fault

## 2026-11-03 evening
- All 9 faults on record (g45-2, lz-1, fm64-s2d, fmp-use1, fmq-use1, s3-use2, s6-base, s7-use1,
  s7-use2): client TCP read, "Unmapped page access", VA in RADV's device-memory region
  0x400f988000..0x436aa8c000, 76 waves at one shader PC; every one in a run that placed an
  allocation in pieces. Runs with many in-pieces placements (p=16 lines) never faulted, so the
  pieces themselves are not the cause; they mark the course-load memory squeeze.
- RADV's PS5 winsys frees a buffer (sceKernelMunmap + release) and a sparse range at once on
  destroy. On Linux the kernel keeps a freed buffer's pages until the submissions using it have
  run; here a GPU read still queued faults. Suspect: memory freed while the GPU still reads it.
- Instrumented driver (private copy in build/radv-isolated of this worktree only; the shared
  radv-release is untouched): stderr lines `radv/ps5 va + / - / b / u / miss` give every
  object's range, so a faulting run names the object the VA belonged to.
- cr-i1, cr-s1 (instrumented; s1 with TC caps 1536/2048 for heavy eviction): no fault, no
  in-pieces placement, no overlap of live objects, no mapping of anything else in the region, no
  memory freed while a sparse range binds it. Eviction alone does not fault.
- The "562 MiB" buffer is a transient 560.6 MiB device-local buffer made at every course load
  with 512/247/147/128 MiB ones; it lives 0-100 ms. In-pieces runs are those where it lands in pieces.
- Fix (PS5_Mesa f1436b69, local): a destroyed buffer or sparse range is given back only once the
  GPU has run every submission made before the destroy; a failed allocation waits for held memory.
  cr-f1 (fix + log): 4,179 of 8,423 frees came with 1-4 submissions still queued (the 560.6 MiB
  one with 3, each 5.8 GB sparse image range with 1-3): all of those were unmapped at once before.
  No fault; race 41.5 fps (median 43.2) vs 40.6 (39.8) for cr-i1.
- Console lock: console-run.sh's 10 s poll loses to back-to-back series; my runs queue with a
  blocking flock (scratch crun.sh) that yields to priority requests and to older waiters.

## 2026-11-04 (after the two PC reboots)
- Verification, fix driver (gpu_accuracy=low cram_fine=on cram_dedup=on; v1-v7 at 1512724 with
  fastmem64=on, v8-v11 at 8b36c60 where it is the default): cr-v1..v11 no GPU fault; 8 of them
  placed the 562 MiB buffer in pieces (5-17 pieces). Race fps v1-v7 45.0 42.5 42.8 43.4 44.6 41.8
  42.9; at 8b36c60 v8-v11 45.7 48.3 44.7 47.5 (mean 46.6) vs unfixed cr-b1/b2 48.5 45.9 (47.2):
  within noise.
- PS5_Mesa f5610fe2 pushed to main; lever-crash 351319a re-pins tools/deps.json. To build it,
  mihawk-mesa-review must move to f5610fe2 (the shared radv-release then rebuilds); every worktree
  still pinned to 249fe49d would then rebuild the driver back, so do it at the merge.
- Not done: naming the exact emulator object a faulting run read (no instrumented run faulted in
  the 3 tried). The 562 MiB transient buffer is the likeliest (lives ~50 ms, freed with 3
  submissions queued, and the faults only came in runs that placed it in pieces).

# Texture correctness in races (2026-11-04)

## Measure (headless/texture_churn.h, hooks in tools/prepare-vulkan-port.py)
- EDEN_TEXTURE_CHURN every 600 frames in heap.txt: evictions by collector mode, images rebuilt at an
  evicted image's address within 60 / 600 frames, null views, images sampled while their background
  decode runs, written sparse layers left without memory. dev-settings churn_capture=on takes a
  picture (spike-<n>.ppm in the console's log folder) at frames with >= 8 rebuilds or null views;
  fetch with the shared notes' lever-crash/spikes.py.
- tx-m1 (ebfa563 + counters, race 45.4 fps): ~10k evictions; in the race the collector is in its
  critical mode (pool usable free < 1 GiB, images idle 10 frames) for 800-1,260 a window;
  rebuilt60 = 426 in the window the race begins (147 in one frame, a 442 ms frame), then 24-102;
  rebuilt600 14-83. Null views 0, sampled while decoding 0 (ASTC goes through the in-order GPU
  transcode; async decodes 0), written sparse layers unbacked 67 in the run (the 8192x8192x68 RGBA
  array at its 341 MiB cap). Ten spike pictures (menus, map, race start, race): textures correct;
  one all-black picture at 60 fps is a screen fade.
- So today the churn costs hitches, not black textures; the only content-less case counted is the
  sparse layer cap.

## Eviction (lever-crash b10dd82)
- Where rebuilds come from (A = counters only, 6 runs): menu-screen returns (100-170 rebuilds in one
  frame; the cache's own marks evicted textures idle 25-50 frames with 3-7 GB of the pool free)
  and, in races, the pool's short mode (idle >= 600 frames; 10-35 quick rebuilds per 600 frames,
  sections of the course coming back) plus its critical mode when usable free < 1 GiB (idle 10).
- Change: oldest-first age ladder (600, 120, mode age) in the pressure pass; the cache's own marks
  take only images idle 600 frames while >= 3 GiB of the pool is usable. Ungated (600 always) kept
  menu textures into the course load and left two of four races in critical mode (up to 5,747
  10-frame evictions): hence the 3 GiB gate.
- tp-a1 / tp-b1: rebuilds 60-600 frames after eviction 589 / 297, worst frame 166 / 48, critical
  evictions 794 / 0, race fps 47.6 / 46.6 (A runs 45.2-48.1 across the day). More pairs queued
  (tp-a3, tp-b3, tp-b2 again); the console was mostly busy with replay series.
- Raising the budget needs memory outside the texture cache: in a race the 13.3 GB pool holds
  5.9 GB guest, 4.0 GB graphics (2.5 GB textures in ~1,500 images, 0.5 GB buffers), ~3.3 GB rest,
  1.1-1.25 GB free. The ASTC transcode's RGBA decode images (up to 512 MiB pending, single ones of
  503-560 MiB) are the course-load spike; asked lever-hitches (d02c5c3) about slicing them.
- Open: written sparse layers left without memory (47-58 a run, 8192x8192x68 RGBA array capped at
  384 MiB on the Pro); as BC it would hold 4x the layers. Not yet seen in a picture.

# Course-load GPU fault after the driver fix (2026-11-04)
- Still happens with PS5_Mesa f5610fe2: hq-base3 (lever-hitches base, build checked for the fix)
  at 202 s. Runs before the pin landed in their worktrees (sc-smtq-*, gt-census3, fw-a1) had the
  old driver.
- All 17 recorded faults (g45-2 .. hq-base3, four sessions, two days) have every faulting wave at
  the same PC, 0x2_000A_AB7C.
- Which shader: the host model of the PS5 driver (PS5_Mesa's ps5 winsys built for Linux, with an
  upload log; build in ~/.cache/lever-crash-radv-host, ICD in the ns2-host run folder) under this
  worktree's PC build (pc-run.sh, VK_ICD_FILENAMES=/work/radv/icd.json MESA_VK_WSI_DEBUG=sw) uploads
  the same sequence: 848 vertex-input prologs precompiled at device creation fill the 256 KiB
  arena and the 512 KiB one up to 0x2A8F4 (console: arenas at 0x200040000 / 0x200080000 at device
  creation); next comes, on the console, the ASTC decoder compute shader (103 KB) at 0x2AA00 (the
  PC has the software WSI's 0x70-byte copy shader first). Fault PC 0x2AB7C = decoder + 0x17C: the
  instruction after its block load (+0x170 buffer_load_dwordx4 of a 16-byte ASTC block from the
  input buffer, binding 0; +0x178 s_waitcnt vmcnt(0)).
- So: the ASTC decoder (ASTCDecoderPass::AssembleInto) reads its input staging buffer at an
  unmapped address. The descriptor ranges match the staging sizes on paper (MapSizeBytes, the
  trimmed `skip` path, the layer path) and the staging pool frees only after IsFree(tick); the
  buffer loads are bounds-checked against the range. Remaining explanations: an input buffer
  freed before the work reading it was submitted (the driver fix covers only work already
  submitted at the free), or a stale descriptor. Next: one Pro run with the decoder's input
  ranges logged (buffer VA, offset, range, tick) beside the driver's address log names the buffer.

# GPU-only sparse backing (2026-11-04, lever-crash 3f1476f)
- What the backing is: nx2:gpu backs a sparse region's 4 MiB block when the GPU side first writes
  it (Nx2BackSparseWrite before every MemoryManager write: DMA copies, texture/buffer downloads,
  flushes). What the GPU renders lives in Vulkan images; the backing holds what was written back to
  guest memory, and the texture cache re-reads it when it uploads an image there again (after an
  eviction, a new view). So an idle block is not dead: it may be the only copy of a texture whose
  Vulkan image the collector can drop without a download (images uploaded from guest memory are
  not GPU-modified).
- Dead for sure: blocks no GPU address reaches. Eden's 213 unmap drops the translation (gmmu
  Unmap, not restored to the region's device range) and a 211 map replaces it; neither comes back
  until the region is destroyed and created again.
- Census (patch 0054, EDEN_NX2_SPARSE_CENSUS=1): EDEN_NX2_SPARSE every 10 s (when the span is
  touched) with backed blocks, read/written in 10 / 60 s, never read, idle 60 s, covered by a later
  map / unmap. PC (world load only; the run stalls before a race): 560 blocks (2.2 GB) backed by
  77 s, all written and read during the load; at 138 s 557 idle for 60 s, 0 covered.
- Next (Pro, after stage 3): a race with the census to split the ~3 GB into covered (release now,
  correct by sparse semantics: unbound pages read zero), idle-but-sourcing-live-images, and in use.
  Releasing idle source blocks would need the texture cache to download (or keep) the images they
  feed before evicting them: not a free win.

# Where I stopped (2026-11-04, paused until Saturday) — lever-crash 7e22d70
- Course-load GPU fault: the faulting instruction is the ASTC decoder's input-block load (all 17
  faults). Next: one Pro race with the decoder's input buffers logged (VA, offset, range, tick) beside
  the driver's address log, to name the buffer read after it was unmapped.
- Texture churn (b10dd82): counters, spike pictures, oldest-first ladder, 600-frame marks with >= 3 GiB
  usable. Next: finish the A/B (tp-a3/tp-b3 and tp-b2 were cancelled; build/ab-A, build/ab-B hold the
  packages, but rebuild both on 7e22d70 first).
- Sparse backing census (0090, EDEN_NX2_SPARSE_CENSUS=1): built in build/dev on 7e22d70. Next: a Pro
  race with it; release covered blocks if the race shows many.
- Queued runs cancelled; nothing of mine on the Pro or the PC.
