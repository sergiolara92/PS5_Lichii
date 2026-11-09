# ProsperoEden-draws (2026-10-31)

Branch eglantine-draws, two commits on 85b0e2e (not pushed):
- 547c9cb: dev marks/Timers read the TSC inline, without the fence (dev builds only). Also adds
  dev-settings `pc_sample=on` (the Pro's GPU-thread PC sampler through the runner) and `marks=off`
  (not tried in a run yet).
- 8487106: DeleteImage no longer writes CORRUPT_ID into every entry of image_view_ids
  (tools/prepare-vulkan-port.py, texture_cache.h). It was a debug poison: nothing reads those
  entries before they are written again.

Race windows (5 s windows >= 20, settings `gpu_accuracy=low gpu_split=off cram_fine=on cram_dedup=on pc_sample=on`):

| run | windows | GPU-thread cpu per draw | draw path | fps |
| --- | --- | --- | --- | --- |
| draws-base (85b0e2e, OOM at 361 s) | 2 | 19.2 / 17.8 us | 9.1 / 8.4 | ~17.5 |
| draws-clock (547c9cb) | 19 | 18.21 us | 7.87 | 22.8 |
| draws-delete2 (8487106) | 18 | 16.74 us | 7.87 | 24.9 |

clock -> delete2 is like for like: same deletion rate in the race (eden_log "Texture cache last
600 frames": 9962 in 137 s against 10545 in 148 s). base -> clock has only two base windows, so
treat it as rough.

## Findings
- **Texture deletion is a big GPU-thread cost in races.** The collector deletes ~70 images/s
  (memory_short=1 the whole race). Before 8487106: DeleteImage 5.3% + RemoveImageViewReferences
  2.1% of GPU-thread samples; after: 0.0% + 0.3%. The rest of a deletion's cost is still there:
  two 64 KiB fills of the descriptor read bitmaps (DescriptorTable::Invalidate), every bound
  texture taking the slow path once afterwards, and the RemoveImageViewReferences/RemoveFramebuffers
  walks. Fewer deletions (memory) would help draws too.
- Clock reads on the Pro: GetTimeNS is `lfence; rdtsc; lfence`, out of line, 27 ns in a loop.
  A draw did ~29 of them; GetTimeNS was 3.1% of GPU-thread samples. Release builds don't do these reads.
- What's left in the race samples (delete2): macro engine 7.7%, Maxwell3D::CallMethod 6.6%,
  DmaPusher 8% (that's the interpreter itself); Configure ~2%; per-draw uniform-buffer copies
  (BindHostGraphicsUniformBuffer -> memcpy) ~1.7%; ASTC/BCN worker waits ~2%; idle ~12%.
- **ccache hazard for new header overrides:** the PS5 build compiles through ccache (direct
  mode). A new derived header that shadows an upstream one (for example a vulkan-cache
  texture_cache_base.h) is not picked up: 24 objects came back from the cache with the old
  layout, even with OBJECT_DEPENDS. I dropped that change (a reverse index for
  RemoveImageViewReferences). Any new override of a widely included header needs CCACHE_NODIRECT,
  or a cleared cache, for the first build.
- The PC build (ns2-host) has none of the port's Vulkan adaptations, and its race profile is
  dominated by GPU ASTC (MasterSemaphore::Wait 13%). It is no guide for the Pro's draw costs.

## Tools
In the side-session notes folder (local_131d4498...): `perdraw.py heap.txt` (per-window us per
draw and fps) and `native_pc.py heap.txt llvm-pie.elf first_s last_s` (the Pro's GPU-thread PC
samples by function, from a `pc_sample=on` run). The dev build also logs caller chains
(EDEN_PERF_NATIVE_CALLERS) for samples in libraries.
