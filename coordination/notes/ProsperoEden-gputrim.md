# ProsperoEden-gputrim (lever-gputrim): split GPU thread CPU per draw

## 2026-11-03
- Patch numbers: this branch uses 0050-0059 in patches/eglantine/eglantine-a42b4d4-ps5.d/ (other levers
  start at 0043), so merges keep a stable order.
- pc-replay.sh takes CHECKOUT=<path> (default ProsperoEden) to replay another worktree's PC build.
- Note on the replay's "front's CPU": GpuSplit::Drain/WaitFor spin (Pause, then yield) while the back
  catches up, so the front's thread CPU time includes its waits for the back.
- Fresh worktree setup: copy ProsperoEden/.local/toolchain (private LLVM 18) and the coordinator's PC
  run cache user/cache/shader (else the replay skips draws while pipelines build: draws and
  checksums then change from run to run).
- PC replay checksums repeat only with EDEN_NX2_SPLIT=0 (20 of 21; checkpoint 14 varies). With the
  split on they change from run to run, read-ahead off included: use split-off for checksums.
- Profile, PC replay race-5 (wallprof): the front spends ~75% of its time spinning in Drain for MME
  DMA reads; the back is the critical path (busy ~93%). Its PC profile is skewed by NVIDIA-only costs
  (VMA dedicated allocations ~18% of its work) and by descriptor payload overflows (WaitWorker; the
  replay ticks no frames). Counters: 6.6 textures per draw (descriptors unchanged 96%), 5.8 streamed
  uniform buffers per draw (3.2 KiB copied).
- Pro race (s7-base1, split on), back draw path 5.8 us/draw: configure 4.18 (texture reads 1.48,
  buffer binds 1.04, uniform streaming + descriptors 1.37, render targets 0.15), flush 0.78, state
  0.39, pipeline 0.31; the front's cpu_ns (8.5 us/draw) includes its spin waits.
- Pro replay race-5 baseline (1512724): back 2.981 s, front 2.967 s, 405512 draws (second half).
- Dropped: Djihads 202c6b035 uniform stream reuse. 67% of binds reuse, but a miss copies three times;
  Pro replay back 3.066 s (worse). 665938d0b conflicts with the MacroNative anchors in
  headless/CMakeLists.txt; 226162edf is submission timing, not CPU.
- 2026-11-03 21:09: the PC GPU wedged (NVRM out of memory) during my replay gt-ubostats (caps
  3072/4096). PC runs refused until a reboot.
- 22:14: the PC GPU wedged again on my first race-5 replay after the 22:10 reboot, with caps 2048/3072 (Chromium and Discord held ~460 MiB). I stopped all PC GPU runs; Pro only from here.

## 2026-11-03 late: results (lever-gputrim 2c7f9bd, 6046dfa)
- PC replay VRAM burst (8 GB in a second at the window start): the ASTC-to-BC transcodes' RGBA decode
  images lived until TickFrame. Only the console build bounded them (prepare-vulkan-port probe), and
  the replay never ticked a frame. 2c7f9bd: 0053 (bound in the source: 16 pending or 512 MiB; probe
  removed) + the replay ticks FlushCommands/TickFrame every 7 lists (EDEN_GPU_REPLAY_FRAME_LISTS).
  Peak 5.0-5.6 GB in 12 runs, draws equal. Replay CPU numbers before/after the tick: not comparable.
- Pro race profile of the back (gt-rprof1, pc_core=7): back idle ~24% (waiting for the front); its work
  is flat (Configure 4.4%, UpdateVertexBuffer 2.4%, PushImageDescriptors 2.4%, CallMethod 2.3%,
  inline-upload invalidation ~6% in all, storage-buffer layout lock ~2.5%). Front (GPU thread) in the
  race: ~35% draining for MME DMA reads, ~12% for puller acquires, ~18% waiting for the game, ~30% work.
  So the front's us/draw mostly measures its waits; the two threads serialize at the drains.
- 6046dfa (0050 write history opt-in, 0051 handle pages, 0052 storage layout sizes):
  Pro replay race-5 back 2.981 -> 2.952/2.953 s (-1.0%), front 2.967 -> 2.942/2.952 s.
  PC replay (with 2c7f9bd, 5 interleaved runs each, loaded machine) back median 2.835 -> 2.697 s.
  Split-off checksums: 21 of 21 identical to the same build without them. Draws equal everywhere.
  Pro race gt-r53a: 44.5 fps (median 45.4), 8.89 us/draw, no crash (gt-rprof1 with 0050/51 + sampler:
  43.4 fps, 9.06). Race frames 60-350 are 0-byte files in every run since at least s7-base1 (17:08):
  the capture is broken, not this change; frame 380 looks right.
- Not reached: ~7 us/draw. The remaining back costs are flat; the larger lever is the drain
  serialization (front's MME DMA read and acquire drains), which needs a safe way to know a read's
  source has no pending GPU writes.
- 6046dfa Pro confirmation, 3 interleaved race-5 replay pairs (runs/pab-*), both arms with 2c7f9bd
  (0053 bound, replay tick): back 3.095/3.095/3.101 -> 3.078/3.071/3.077 s (-0.7%), front
  3.010/3.005/3.024 -> 3.004/2.990/3.009 s (-0.4%), draws 827101 in all six.

## 2026-11-04: drain census (counter-only build, not committed; runs/gt-census2, one Pro race)
Race part (eden_log 235-321 s, 87 s), the front's drains and their wait (ring catch-up only):
| reason | drains | wait | avoidable if no buffer writer pending (inline ranged) |
| MME DMA read after wait-for-idle | 12067 | 2.8 s | 7588, 0.53 s |
| MME DMA read of a reported/released word | 6908 | 26.7 s | needed for the report itself |
| puller acquire | 27105 | 12.7 s | 10822, 1.27 s |
All drains 42.2 s of 87 s (48% of the front). Option 1 (skip drains with no pending buffer writer,
inline writes by range) would remove ~1.8 s, ~4% of the drain wait. Back writers in the window:
compute dispatches writing storage 275k, inline uploads 14.6M, DMA 190, draws 0.
The large item is reads of report words (63% of drain wait, 3.9 ms a drain): the front drains the
whole ring (and then flushes the query cache) even when the report was sent long before.
PC replay census agrees (sync drains 34% clean but ~10% of their wait; pending row dominant).
- Report-read census (runs/gt-census4, clean race; gt-census3 ended in the course-load GPU
  protection fault seen in other sessions' runs too, sc-pro1/2 just before). Race 252-319 s:
  5118 MME DMA reads of report words, ALL with the report still queued (none waiting on the query
  cache or the host GPU). Drain 21.5 s (32% of the window). A partial wait to the report's ring
  position: 18.2 s (-15%; ~5600 ring words after the report on average). Query-cache flush 9 ms
  in all. The value read changed with the drain in 99% (5081): the pre-drain word is the previous
  result, not the one the queued report writes. PC replay census: same picture (256/256 queued,
  100% changed).
- Kinds census (runs/gt-kinds1, clean race 45.3 fps): every report-word read (17,152 in the session,
  36.1 s of drain) reads a 3D payload semaphore release: 6 words [payload, 0, timestamp lo, hi, 0, 0].
  Payload changed with the drain in 38% (PC replay 2%); the timestamp almost always (the "99%").
  Not occlusion: no lag mode. Proposed instead: answer from the pending release (payload exact,
  timestamp = the front's current tick) when no buffer writer is queued: 12,086 of 17,329 such drains,
  28.0 of 36.8 s of their wait.

## 2026-11-04 early: payload-release forwarding (0054, commit 7dbf393, off by default)
- The report-word reads are 16-byte payload releases from the puller (SEMAPHORE_TRIGGER WriteLong,
  not the 4-byte fence form) or the 3D engine (report semaphore, Payload): the back writes them when
  it runs them (payload as u64, GPU::GetTicks() at +8). The front answers those words itself
  (EDEN_NX2_RELEASE_FORWARD=1): payload, 0, GetTicks(). Without a wait for idle the read's other
  words are read ahead from memory as before; after one, all words must come from releases and no
  queued op may write them (front-noted writers; draw programs learned by the back by
  Maxwell3D::ProgramSetKey, unknown = writing).
- Traps found on the way: the program key must not include the memory manager's layout generation
  (the front reads it ahead of the back); the direct-mapped table needs 65536 slots for MKW's program
  sets; rejected programs are learned as non-writing.
- PC race-5 replay: 512/512 report reads answered, draws equal, 21/21 checksums equal to the same
  build without it; split-on checksums now repeat run to run (the replay's frame tick), but differ
  from split-off in 19 of 21 (also without forwarding: a split-on vs split-off difference).
- My worktree linked an isolated old driver copy (build/radv-isolated from 11-03 17:57) until
  04:13: every Pro run of mine before that (gt-*, fw-a1, fw-b1) used PS5_Mesa 249fe49d. fw-b1
  (forwarding on, old driver): 49.6 fps (median 52.8), 12,800 reads answered, 0 drained; game speed
  92-97%. Clean A/B on the new driver: fw2-a/b (running).
- Forwarding A/B on the current driver (fw2-*, same build, B = EDEN_NX2_RELEASE_FORWARD=1), all clean:
  a1 47.14 fps (median 49.15, 98.4k draws/s, 9.12 us/draw); b1 48.04 (51.13, 99.8k, 9.02);
  a2 48.35 (51.13, 101.3k, 9.01); b2 44.92 (46.01, 103.1k, 8.85; heavier windows). Every report read
  answered (16-17k a race, 0 drained); game speed unchanged. Pair 3 and split-on pictures queued.
- Why the gain is small (PC zone profile, off vs on): MME-read drains 166/s -> 81/s but their wait
  710 -> 648 ms/s, puller drains 90 -> 158 ms/s. The front waits for the back at the next wait for idle
  instead; the census's 32% was mostly the back's work showing up as waiting. The back's per-draw
  cost is the limit.

## 2026-11-04 morning: unchanged-state census (PC race-5 replay, counter-only, runs/gt-ucensus)
Per draw against the previous draw of the same pipeline (392,888 of 393,216 had one; 63% back-to-back):
handles 88%, image views 91%, storage addresses 100%, render targets 92%, vertex/index regs 45%,
uniform ranges 13.7%, uniform bytes 21.9%; texture+storage+targets 82%, all inputs 10%. Same uniform
range with changed bytes: 0.07% (262). Uniforms are fresh per draw (~8 KB, new addresses), so they are
streamed regardless. Potential of an unchanged-texture fast path: ~0.4-0.7 us/draw on the Pro (5-8%);
draw batching not feasible (uniform and vertex ranges change).
- Image-descriptor reuse (unchanged-texture fast path) built and dropped, not committed (diff in the
  side-session notes folder: gputrim-image-descriptor-reuse.diff). Each pipeline kept its last
  draw's image descriptor entries and rescaling bits, valid for the same view/sampler ids while a
  texture-cache generation (bumped in RemoveImageViewReferences and InvalidateScale) held; the views
  were still prepared and the handles and TIC/TSC words re-read. Verify mode: 0 of 327,680 differ
  (a first version compared the union's padding bytes: false alarms). Split verify: same 56 lines
  with it off. But PC race-5 replay, 3 pairs: back 2.599/2.686/2.746 s on vs 2.556/2.632/2.680 off
  (+2%): only 40% of draws can reuse (any view removal invalidates every pipeline, and the texture
  cache's collector removes views all the time), and the bookkeeping on the rest costs more.

## 2026-11-04 (afternoon): forwarding A/B, PC replay memory, determinism
- 0054 forwarding, Pro race pairs (fw2-a* off, fw2-b* EDEN_NX2_RELEASE_FORWARD=1), races.py draws/s:
  off 98.4k/101.3k/97.3k, on 99.8k/103.1k/101.6k (+2.5% mean); us/draw 9.17 -> 8.97. Pictures:
  fw2pic-off completed (21 pictures in runs/fw2pic-off/rt, checksums in runs/fw2pic-off.out);
  fw2pic-on was stopped by the pause, no pictures. The 0054 default is still undecided.
- PC race-5 replays on 0308762 hit the 8 GB watchdog (peak 7.6-8.1 GB) in every run, split on or off,
  frame tick on or off. Cause: 0061 (SlotVector::begin fix). The same build with only 0061 reverted
  peaks at 4.7-5.1 GB and completes (runs/gt-no61-1..3). Suspected path, not proven: with iteration
  fixed, BufferCacheRuntime::TickFrame resets buffer usage tracking again (CanReorderUpload). But
  reordered uploads come from the stream buffer, so the extra memory is probably elsewhere: still open.
  This probably also explains lever-jitregs' full-game PC runs reaching 8 GB at 50-170 s.
- Determinism: that build (0308762 - 0061, + the PC transcode bound on 0044, + the replay frame tick
  FlushCommands/TickFrame every 7 lists) gave 3 of 3 identical checksum sets, all 21 equal
  (runs/gt-no61-1..3). lever-jitregs' nondeterministic runs were 04270d6 + 0053-pc-replay-aid, no tick.

## Where I stopped / next step (paused 2026-11-04 ~12:50 by the user's instruction)
- Branch lever-gputrim: WIP commit on 0308762 re-adds the replay frame tick to headless/gpu_replay.cpp
  (it was reverted with 2c7f9bd). Not merged anywhere.
- Build cache source (~/.cache/ps5-eden-headless.a980212c9236/source) is NOT clean: 0061 reverted
  (original in gt/slot_vector.h.0061), the PC transcode bound applied, and GTALLOC debug in
  vulkan_memory_allocator.cpp (original in gt/vma.cpp.keep2). Restore all three before any real build.
- Next: (1) runs/gt-no61nt was stopped: rerun the same build with EDEN_GPU_REPLAY_FRAME_LISTS=0, 3 runs.
  If those differ, the frame tick is what makes the replay deterministic, and it should go back in for
  every build. (2) Find what 0061 makes hold ~3 GB more (log allocations with 0061 on/off).
  (3) Pro: 3 hashed replays once the Pro is free.
