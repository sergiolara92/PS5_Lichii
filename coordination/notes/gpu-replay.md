# GPU command capture and replay (2026-11-01, design)

Goal: replay a recorded stretch of Mario Kart World through the real GPU thread (single or split),
on the PC and on the Pro, with no game CPU, kernel or services: deterministic per-draw and split
measurements in about a minute instead of a 6-minute, noisy console run.

## What the GPU thread consumes (a42b4d4)
- One input queue (video_core/gpu_thread.h): SubmitListCommand (channel + command list entries:
  GPU VA and size of pushbuffer segments), Flush/Invalidate/FlushAndInvalidate region, GPUTick,
  RunCommand (functions: not serializable, must be identified and handled one by one).
- Switch 2 command lists arrive through nvhost_gpu::SubmitGPFIFOImpl -> GPU::PushGPUEntries, as on
  the Switch. Presents: nx2_gsv PresentClientBuffer -> GPU::RequestComposite.
- Memory: the GPU side reads through DeviceMemoryManager, device page -> physical page of the
  DeviceMemory backing (compressed_physical_ptr, physical_base). Map() takes a process VAddr + ASID
  and resolves physical pages through the process: replay needs a physical-address map instead.
- GPU address spaces: Tegra::MemoryManager per nvhost_as_gpu (GPU VA -> DAddr), mapped by
  nvhost_as_gpu and nx2_gsv (SubRegisterMemory, SubSubmitJob/NormalMapGpuMemory4K, SubUnmapJob,
  sparse regions).
- CPU writes reach the GPU caches through the rasterizer notifications Core::Memory makes for
  tracked pages (OnCPUWrite / InvalidateRegion) and through the queue's region commands.

## Volume (Pro, race, ab-both-1)
Per frame: 7 command-list submissions, ~44,000 command words (~176 KB of pushbuffer), ~2,400 draws.
Guest memory written (PC, headless/dirty_probe.h, soft-dirty over the whole 20 GiB backing, 4 KiB
pages, CPU and GPU writes alike, run pc-dirty2): steady 90-130 MiB/s, bursts 400-785 MiB/s at loads.
An upper bound on what the GPU needs (CPU-only game data is included).

## Decision (2026-11-01): snapshot at a race frame, then a window
From boot is ~24 GB of raw page writes for the 240 s to the race: too big. Instead, at a submission
boundary in the race: the GPU-mapped physical pages (a few GB, compressed), device and GPU-VA maps,
and the GPU engines' state (register files, macro code and positions, shadow RAM, channel and puller
state); then a window of N frames (stream + GPU-mapped pages written between submissions, ~0.5-1 GB
raw for 5-10 s, less once filtered and compressed). Cost: an engine-state inventory and restore.

## Plan
1. Measure (PC): physical memory mapped for the GPU at the race, and CPU-written bytes per frame in it.
2. Capture (PC build only), from a race frame (see Decision): snapshot, then the GPU queue in order,
   device and GPU-VA map/unmap, rasterizer notifications, and before each submission the GPU-mapped
   physical pages written since the last one (content, deduplicated, compressed).
3. Replay (PC, then Pro): a replay entry (dev setting) builds System + renderer + GPU without a game,
   maps physical pages directly, feeds the stream unthrottled, and reports per frame GPU-thread time
   and draws; validation against the capture by per-frame draw counts and framebuffer hashes.

## Split finding on the way (PC, split on, profile per second)
Front: drain puller 402 ms (2,936/s), drain MME DMA read 149 ms (474/s), macro 113 ms, idle 159 ms:
the front waits ~55% of the time; this, not the work split, is why the split gains nothing.

## Status (2026-11-01 evening)
Working on the PC (commits 043694a recorder, 6df9658 replay, fdffbb7 filter + CPU time):
- Capture: EDEN_HOST_GPU_CAPTURE=/work/capture.bin _AFTER_S=200 _AT=0 _COUNT=2100, split off
  (EDEN_NX2_SPLIT=0), through coordination/pc-run.sh with NS2_GAME=<the .xci>. ~5 GB per window.
- Replay: EDEN_HOST_GPU_REPLAY=/work/capture.bin (same runner). Reports draws (equal to the
  capture's) and GPU thread CPU time: 16.42 / 16.33 / 16.40 s for one capture (spread <1%).
- Captures live in GITHUB_ProsperoEden_20261029/captures/ (game-derived data: never in a repo,
  never shared); move one into the run folder as /work/capture.bin to replay it.
- Next: the player in the console build (dev setting), the capture copied to the Pro.

## Done (2026-11-01 night): PC and Pro
- Console: coordination/replay-run.sh <tag> <checkout> /data/lichii/captures/mkw-race-4.bin
  [dev settings]: install, replay, clean exit, 165 s. Prints "GPU thread CPU time" (the number to
  compare) and draws (must equal the capture's). Repeats: 15.51 / 15.49 / 15.48 / 15.49 s.
- PC: pc-run.sh ... EDEN_HOST_GPU_REPLAY=/work/capture.bin (capture moved into the run folder).
  12.03 s for mkw-race-4. Never time a replay while anything else builds or runs (a compile made
  it 27 s instead of 15).
- Capture mkw-race-4 (5.9 GB, split off, 2,100 lists, 576,623 draws) is on the PC
  (captures/mkw-race-4.bin) and on the console (/data/lichii/captures/).

## Overnight 2026-11-02: CPU cuts don't move the race fps
- Real-game A/B on the Pro (3 interleaved pairs, race windows): base 22.6 / 24.0 / 25.2 fps,
  overnight-60fps (native macros + 0008, -7.1% GPU thread CPU in replay) 23.1 / 23.5 / 24.4.
  No gain: the race is not bound by GPU-thread CPU time alone.
- Same in the replay: second half wall 8.7 s against 5.2 s GPU-thread CPU. With gpu_profile=on
  (its timers add ~4 s of CPU) CPU 9.38 s, wall 9.45 s: the wall barely moves when the CPU work
  grows, so something else paces it at ~8.7 s per 1,050 lists (host GPU progress or a sync on it).
- Patch 0010 counts where the GPU thread blocks (suspended channels, MasterSemaphore::Wait,
  WaitWorker); the replay prints it for the second half.
- Patch 0009 (split only): acquires met by a release the front already sent the back skip the
  drain. Replay bug fixed on the way: with the split on, RunInOrder work runs on the back, and
  the replay's own Defer from there wrote into the ring from the wrong thread (deterministic hang
  at the window's half). The front's CPU time is now read from the push hook.
- Pro replay, split on (front macros) with 0009: front CPU 4.24 s, back 3.84 s for the half (split
  off: 5.28 s on one thread); wall 10.5 s. Profiled split windows: the front idles (queue empty)
  37% and sleeps on suspended channels 22%; the back waits for work 60%. The Pro replay is bound
  by its player (reading the 5.9 GB capture, applying the guest's page writes), so its wall time
  says nothing about either thread: compare CPU times only.
- Real game on the Pro, one build (0009+0010), split off vs on, 3 interleaved pairs:
  off 22.96 / (crash) / 27.00 fps, GPU thread busy 0.92-0.93, 14.7-16.6 us/draw;
  on 22.68 / 24.17 / 23.21 fps, GPU (front) thread busy 0.65-0.67, 12.0 us/draw.
  The split takes a third off the GPU thread and the fps does not move.
  (split-a-2 crashed at 326 s: TextureDecoder "mutex lock failed: Invalid argument", the known
  timing-dependent one from ProsperoEden-cores.md, split off.)
- Guest threads in race windows (heap.txt EDEN_DEV_WAIT_THREAD): every active guest thread waits
  78-94% of the time; job workers on arbitration, two threads mostly on sync (GPU fences), the
  main thread 62% arbitration + 14% sync. Guest cores idle ~55%.
- So in the race neither the GPU thread nor the guest CPU is the limit: the game waits on GPU
  completion. Next: is it the host GPU's work (internal resolution 0.5x vs 1x) or the latency of
  getting work to the GPU and fences back (submission batching, fence thread)?
- Internal resolution 0.5x against 1x in the race (dev setting resolution=0.5x, new): 0.5x 19.7 fps
  (and a crash), 1x 25.2 / 25.2. A quarter of the pixels is not faster: the host GPU's pixel work
  is not what limits the race.
- Across every split-off run tonight fps follows the GPU thread's cost per draw:
  fps ~ 0.92 / (draws per frame x us per draw), 14.7 us -> 27 fps, 17.5 us -> 21 fps, same build
  and settings. Run-to-run spread of that cost is ~+-9%, so a real-game A/B of 3 pairs cannot
  resolve a 7% CPU cut; the replay's GPU-thread CPU time (deterministic) stays the measure.
  With the split the back takes over as the limit at about the same cost per draw.
- Crash seen twice tonight (split-a-2, res2-a-2), both at 326-327 s after start, split off: the
  TextureDecoder "mutex lock failed: Invalid argument" (eboot+0xc21f5 / +0xc22a5). Same as in
  ProsperoEden-cores.md (363 s). Repeats at a fixed point of the session: worth a fix of its own.
- The 325-328 s crash, symbolized (build/headless-native/llvm-pie.elf, offsets are elf addresses):
  TextureDecoder thread, TextureCache::QueueAsyncDecode's decode lambda -> std::mutex::lock ->
  EINVAL. Context lifetimes in Queue/TickAsyncDecode look sound, so the likely cause is a heap
  overrun corrupting a neighbouring AsyncDecodeContext: the decode buffers were sized from
  shares of whole-image estimates (MapSizeBytes / layers, unswizzled_size / layers), not from the
  layer's own layout. tools/prepare-vulkan-port.py now sizes them from ConvertedLayout and
  CalculateUnswizzledSizeBytes (and logs "Layer decode at ... needs N bytes" when the estimate
  was short). Hit 4 times in ~20 runs tonight, always 325-328 s after start.
- Size guards alone (4 races): no "needs N bytes" warning, the crash again at 326 s. Then the
  decode handoff without a mutex (commit 265d918), 6 races: 5 to the end with no crash, 1 ended at
  323 s by a guest "Userspace PANIC" (svcBreak, debug err 0x1A80A) - the same point of the run.
  So the mutex failure was a symptom of something at that course transition; memory is the first
  suspect (crash reports: largest free block ~102 MiB of 13.6 GiB at that moment).

## State at the end of the night (overnight-60fps, nothing merged)
- Race on the Pro: ~24-25 fps -> ~29-30 fps (GPU thread's SMT sibling idle, de43cf9), worst frame
  per 5 s window ~105 -> ~70 ms.
- What limits it: the GPU thread's CPU per draw (now ~13 us at ~2,450 draws a frame). Not the host
  GPU (0.5x resolution no faster), not the guest CPU (~45% busy), not the split's front (the back
  takes over at the same cost). 60 fps needs ~6.5 us a draw.
- Stability: something at ~323-328 s of the scripted run (course transition) ends ~1 run in 5.

## Correction (2026-11-02 morning): the ~325 s failures are the session's end, not a course change
- console-run.sh presses the return-to-menu chord at 425 - SHIFT = 325 s. Every TextureDecoder
  crash report (dec-b-2, back-b-3, place-b-3) has the full ~200 s of race windows and
  "shortcut_return_to_menu" in its heap log: the race was over, the crash is in the teardown.
- Root cause: TextureCache declared texture_decode_worker before async_decodes, so the contexts
  were destroyed first and the worker's thread was joined after, while a queued decode still
  wrote into its context. Patch 0011 (e67a2b8) declares the worker after them. 265d918 (no mutex)
  only turned the visible crash into a silent use-after-free; 0011 is the fix.
- nomx-b-1's guest panic (svcBreak after SetTerminateResult 0x1A80A at 323 s) is a separate
  failure: its reports stop at ~215 s (eden_log silent from 215 s to 323 s), so the game hung mid-
  race and panicked when the exit chord reached it. 1 in ~24 runs; not investigated yet.
- Verification (6 consecutive runs) still to do: at 06:11 the title was already running on the
  console (not started by any session), so the runs stopped after fix11-1..3 found it busy
  (those three are not results; fix11-1 also installed the new build over it).
- Check runs at head 6dba2f1 (0011 + 4a553bd + 6dba2f1, runs astc-*, shot-*): no TextureDecoder
  crash in 8. But all 8 now log SetTerminateResult 0x1a80a (CMIF module, desc 212) + "Userspace
  PANIC" exactly when return-to-menu starts (heap: shortcut_return_to_menu; core_shutdown and
  SESSION_END follow ~1.8 s later as before; the early ones at 117 s / 160 s were return-to-menu
  too, while the user held the controller). Before (nomx-*, dec-*: ~20 runs) the same moment logged
  SetUserSystemClockContext, the game's orderly exit, and no panic. So it is a regression in the exit
  path, not the scripted quit itself. Suspect 4a553bd (controller polled under the replay): 0011
  only changes destruction order inside the GPU teardown, which runs after core_shutdown.
  To confirm: one run of the head with 4a553bd reverted.

## Race capture (2026-11-02): captures/mkw-race-5.bin
- mkw-race-4 turned out to be the free-roam drive behind the main menu, not a race. New capture on
  the PC: coordination/pc-capture-race.sh mkw-race-5 300 (scripted pad as the Pro's development
  replay: EDEN_HOST_PAD="45:47:10:1:0xc00;50:200:5:1:0x2000;200:900:0.5:0.44:0x2000"), 2,100
  lists from 300 s, 827,101 draws (mkw-race-4: 576,623), lap 1 of 3 in pictures. 6.2 GB, also on
  the console at /data/lichii/captures/mkw-race-5.bin.
- Pictures: coordination/pc-replay-pictures.sh <name> (PC), EDEN_GPU_REPLAY_DUMP=all on the Pro.
- PC runs cap the texture cache at 4-5 GB (EDEN_NX2_TC_EXPECTED_MIB/CRITICAL_MIB): filling the
  RTX 3070's 8 GB left its driver unable to create a device until a reboot.

## 2026-11-02 afternoon: fork ports on the race replay (mkw-race-5)

- Pro, split off, second-half GPU thread CPU (two runs each): baseline 4.357/4.354 s; with 0017
  (lazy descriptor tables, array ids), 0018 (SEM_EXECUTE acquires suspend, latch) and 0019
  (destruction-ring credit) 4.272/4.272 s (-1.9%); adding 0020 (block-aware collector order)
  4.291/4.291 s (+0.4% for the ordering). Draws equal throughout.
- Pictures: Pro baseline vs all four ports, 20 of 21 identical, one 0.02% (Pro noise). PC, all 21
  checksums identical.
- SEM_EXECUTE acquires never went unmet in 200+ logged runs ("not met, continuing" absent), so 0018
  changes nothing for this game; it is there for correctness.
- PC split on vs off (race-5): 20 of 21 checksums identical; checkpoint 15 differs in 50 pixels in a
  7x11 patch at the left edge (max 43/255). No texture corruption in the replay.
- PC VRAM: the race-5 dump-all replay at TC caps 4096/5120 ran the 3070 out of memory (driver wedged
  until reboot). Caps 3072/4096 ran clean twice.
- Tools: pro-pictures.sh <tag> [capture] (Pro dump-all, no build), picdiff.py <a> <b>.
- Pro split-on replay with 0014/0015 (son-1/2): back 3.567/3.572 s, front 3.504/3.512 s, against
  4.291 s for the single GPU thread; draws equal, no hang. Races (qmd3-on vs qmd3-off): 33.5/33.1
  vs 33.0/32.6 fps; the back is 83-85% busy against 90-91% unsplit. The split moves the limit to
  the front's waits (drain puller, MME DMA reads); next: those drains.
- Port races (ports-1/2, split off): 32.4/33.1 fps, 12.9/13.0 us per draw (13.1/13.2 before),
  collector evictions 11.9k/9.8k per run (12.2k/11.2k before), no crash.

## 2026-11-02 evening: front drains (0022-0025)

- MME DMA reads drained the back each time (445/s). Check (0021): in races 2% of the reads changed
  with the drain, all report words (timestamps, occlusion results 1 -> 0); none on GPU-written
  buffers. 0022/0024: drain only after a wait for idle or for a word the back is still to write
  (reports and releases noted by the front). Replay: of 4096 reads, 1629 drain after a wait for
  idle, 273 for a pending write.
- Acquires (0023): met from guest memory or a latch with no drain; a suspended channel's full check
  at most every 2 ms. Puller drains 2,658/s -> 210/s.
- Races, split on: 35.2/35.6 fps (draws/s 73.9k/72.9k) against 33.5/33.1 before; split off 32.4-33.
- 0019 (destruction-ring credit) removed: ra-1 ran out of graphics memory at a course load (crash
  report VK_ERROR_OUT_OF_DEVICE_MEMORY, largest free block 8 MiB). On the console an evicted image
  inside a 16 MiB block returns nothing to the pool until the block empties.
- Profiled race (ra-prof): the back's split wait is 66 ms/s; methods 219 ms/s and "other" 181 ms/s
  are its largest costs (profile overhead inflates both).

## 2026-11-02 night: where a race frame's time goes now (back on its own core, 38-39 fps)
PC sampler (pc_sample=on pc_core=<n> pc_fast=on env=EDEN_PC_FAST_FROM=150; pcsym.py, pccallers.py):
- back (pc_core=7): ~40% asleep waiting for work (std::atomic wait in BackLoop).
- front (pc_core=4): ~63% asleep in the GPU thread's command queue (condition_variable::wait in
  StartThread): waiting for the game to submit. Drain 6.8%.
- guest core 0 (pc_core=0): JIT code ~60%, idle (PhysicalCore::Idle and its wait) ~28%.
- Frames are vsync-paced: in 5 s, 172 took two vblanks, 14 one, 5 three (EDEN_VULKAN_INTERVALS):
  most frames finish a little over 16.7 ms. 60 fps needs every frame's critical path under it.
- Guest threads: MainThread waits 79%, the six job threads (BaseProcExecutor0, 183-187) 45-63%.
  Six emulated cores: 0-3 pinned to CPUs 0,2,4,6; CPUCore_4/5 float on 1,3,5,7,12 with the Vulkan
  worker, texture decoder and pipeline builders.
- Present path in races (nx2:vi2 "last 600 presents"): frame 23-28 ms, blocked on the GPU fence
  ~2 us, vblank pacing ~1 us. The game's present does not wait for Eden's GPU work: the frame time
  is the game's own CPU frame. Guest cores idle 30-50% (PC wallprof, pcrace1): the game's job
  threads wait on each other; suspects: guest scheduling/wake latency, JIT speed.
- Guest wake-to-run (0028, EDEN_NX2_SCHED_STATS): ~27.6k wakes/s, mean 19 us, 91% under 10 us,
  ~420/s wait 200 us-5 ms (cores busy with other game threads). EDEN_NX2_IDLE_SPIN_US=100 changed
  neither the distribution nor fps (35.9/35.8 vs 37.5): not host wake latency.
- cpu_accuracy=unsafe: 39.6/38.5 fps vs 37.5/38.6, within noise. Placement: CPUCore_4 on CPU 12
  (37.2 vs 39.8 default); CPUCore_4/5 on 9/11, the front's and back's siblings (34.0, GPU threads
  11.6-12.8 us/draw vs 9.8): both worse.
- Earlier A64 fastmem study (PS5_ProsperoEden docs/PERFORMANCE_FINDINGS.md): built, no gain; the
  game's code is bound by memory latency, not instruction count.
- g45-2 ended in a GPU page fault (GPU_FAULT_PAGE_FAULT_ASYNC, no crash report) at ~227 s: course
  load; just before, radv/ps5 placed 562 MiB and 128 MiB allocations "in pieces (no single free
  range)" and ~5 GB of GPU-only regions were backed. Same memory squeeze as the staging OOM.
- GPU-only regions (nx2:gpu 214): sparse ones (~20 GB virtual) are backed as written; dense ones
  (~2.9 GB logged, 250-382 MiB each) were backed whole. 0029: EDEN_NX2_DENSE_LAZY_MIB=<n> backs
  dense regions >= n MiB as written too (opt-in while measured).
- 0029 lazy dense (EDEN_NX2_DENSE_LAZY_MIB=16): GPU-only peak 5657 -> 2193/2791 MiB, lz-2 38.4 fps;
  lz-1 ended in the same GPU page fault as g45-2 (GPU Protection fault, client TCP read, unmapped
  VA 0x403a3b5000 / 0x400f988000), both right after the course load's usual log lines. The driver
  maps unbound sparse pages to a zero block, so it is a read of freed or out-of-range memory.
  Suspect: macro read-ahead (0022-0026) giving stale draw parameters at a course load. 0030 makes
  the learned part (0026) opt-in; races nt-1..4 check the fault rate.
- PC GPU wedged again at 17:54 (NVRM NV_ERR_NO_MEMORY, then GspRmAlloc failed) during the full-game
  PC race pcsch at TC caps 3072/4096: the full game needs more VRAM than a replay. pc-race-prof.sh
  now caps 2048/3072, and pc-run.sh refuses to run while the kernel log since boot shows the wedge.
- Layout test (compact memory + dedup off, lay-1): race frames and fps unchanged, +1.3 GB memory.
  lay-2 hung at boot in the shader cache rebuild (3758 of 4097); the next boot built all 4097 and
  raced at 40.0 fps: an occasional pipeline-build deadlock, first in ~30 boots.

## 2026-11-03 night: fork ports
- 0032 (PokeDinner cd98b8bed): MME DMA reads flush the query cache for their range; split front
  drains then flushes for any read of a word a report was ever sent to (GpuSplit::EverReported).
- GPU-filled command lists (zortos 6612817cb/fae3bc64a) tried and dropped. Reading every entry on a
  GPU-written page from memory: replay black scene (CPU-written lists on those pages are refilled
  for the next frame), real race 33.8 fps (446k drains). Only entries the buffer cache holds dirty
  GPU words for: replay +1445 draws, a large black bush in front of the camera at checkpoint 2.
  The snapshot (submit-time words) stays.
- Djihads80 shader fixes: bcf8471dc already in; 461015ea5 applies but changes SSBO tracking for all
  shaders (needs a shader cache version bump; no MKW symptom); 60bc4fa0c and c31a22ebe conflict.
  MKW rejects native shaders for whole stages 3 and 4 ("Unsupported native graphics shader stage"),
  so some effects are not drawn: a visual-completeness task for later.
- Guest scheduler trace (0034, tr-1, 374 frames, mean 26.7 ms): emulated cores busy 59-75% per
  frame. Six job threads (182-187, BaseProcExecutor*) run 10-14 ms each per frame (~67 ms of work);
  182 (core 0, shared with MainThread) runs longest in 342 of 374 frames: 14.4 ms in ~60 bursts of
  240 us, resuming from an idle core 96% of the time. The frame is the game's job chain.
- The console runs our races with all CPU cores at 3200 MHz (klog; GPU 2259 MHz).
- 0035: PC race pctr1 crashed (SIGSEGV) in RangeMap::InternalMap from MemoryManager::Unmap on the
  back thread at a course load: PageTableOp updated kind_map without the guard.
- cpu_accuracy=unsafe, 3 interleaved pairs: 38.0/37.0 (one boot hung) vs 39.1/37.5/37.7 fps. No gain.
- Wait classification (0036, tr-2): every runnable-to-running wait over 50 us happened with the core
  busy running another game thread (never idle): no wake-up delivery delay. Thread 182 (critical,
  core 0) never waited over 50 us (2129/s, ~4 us). Its gaps are dependencies on the other job
  threads. The frame is the speed of the game's code.
- Boot hang in the shader cache rebuild, twice in ~45 boots (lay-2 at 3758/4097, ab-u1 at
  3535/4101, both ~50 s, same last log line). Not a cache entry: the next boots built all. Needs a
  thread dump of a hung title to find.
- The "boot hangs" (lay-2, ab-u1, gen2-1) are stop failures: each klog holds a race session that ran,
  then "Stopping the game", "did not stop in time; starting ProsperoEden again" (10 s limit), then a
  restarted boot that hung in "main cache_load" (pipeline cache). The restart overwrote heap.log, so
  races.py saw "no race windows". Stuck step: gpu_core->NotifyShutdown (gen2-1, ab-u1: the GPU
  thread did not finish) or kernel.CloseServices (lay-2). 0037 prints the split's state then.
- Stop hang located (0037, gg-2): front=drain, back=call in Puller::ProcessFenceActionMethod's
  syncpoint increment (rasterizer->SignalSyncPoint): SyncpointManager::Increment holds its guard
  while running the waiting actions (game kernel events), during ShutdownMainProcess.

## 2026-11-04 (lever-jitregs): checksums are not reproducible on 04270d6+
- PC race-5 replay with EDEN_GPU_REPLAY_HASH=1: before the 0050-0053 revert, 16 of 17 runs of builds
  with the same output path gave identical colour-target checksums. On 04270d6 (plus a PC-only copy of
  0053's decode bound, coordination/shelved/jitregs/0053-pc-replay-aid.diff, without which the replay
  hits the 8 GB video-memory watchdog), two runs of the same binary differ at every list (runs/jr-big2,
  runs/jr-cand-A2). Draw counts still match the capture.
- Pro: every pair of runs differs at every list, for every session's builds (also pic-base vs pic-p4),
  so the Pro checksum never validated anything.
- So the image check is weaker than assumed: on this base, "draws equal" is the only replay check left.
  Use pictures (pro-pictures.sh / picdiff.py) or a deterministic base for output changes.
