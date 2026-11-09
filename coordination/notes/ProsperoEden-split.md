# ProsperoEden-split (2026-10-31)

Commit c0e2ac9 on eglantine-split: the GPU thread split runs on the Pro.

- Crash cause: the PS5 `dma_batch` derivation (headless/CMakeLists.txt, DmaPusher::ProcessCommands)
  wrote register runs into `engine->method_sink` on the front thread, a vector the split's back owns.
  Garbled upload registers -> WriteBlockImpl on GPU address 0x2109ad3900000000. Still crashed at
  b7575b0 (runs/split0), so ddd367a was not it. Batch now skipped while the split is active.
- Also moved to the back / drained: DispatchCalls' FlushCaching replay, the Vulkan loading screen.
- runs/split1, runs/split2: split on, no crash, ended by the emulator.
- Race fps (5 s windows): split on 24.7 / 22.0 mean, split off (runs/split-off) 22.1. No clear
  gain yet. With the split the front (GPU) thread is ~78% busy and 19% idle; DRAW.ns ~52% on the back.
  The split also loses dma_batch (one ring entry per word now): a ring entry for a run of
  store-only writes is the next thing to try.
- runs/split-off ended out of memory at 416 s (guest-range write fault, 1 MiB largest free): heap's work.
- Not checked: the PC build (ns2-host) with this change.

## Split-safe dma_batch (2026-10-31, branch split-batch, commit 66f84a7 on c0e2ac9)
- With the split on, an incrementing store-only run is one ring entry (GpuSplit::SinkRun, Kind::Sink,
  derived gpu_split.h/.cpp); the back appends it to method_sink. Maxwell3D front register copy takes
  each word through FrontCallMethod while `hold_methods` suppresses per-word entries. KeplerCompute
  native mode has every method in its mask: no runs there.
- Build trap: a derived header that shadows a pinned one is NOT picked up by existing objects
  (ninja deps and ccache direct mode both point at the pinned path). Fixed with a video_core compile
  definition (EDEN_SPLIT_SINK_RUN=1) so the command line changes. Check `ninja -t deps` after merging.
- Runs (gpu_accuracy=low cram_fine=on cram_dedup=on):
  - split-batch1: OOM crash at 422 s (Fermi2D sw blit -> WriteBlockImpl, 1 MiB largest free; same
    signature as split-off). Race 25.3 fps (13 windows from first <22 fps).
  - split-batch2: VK_ERROR_OUT_OF_DEVICE_MEMORY at 353 s, course load; no race windows.
  - split-batch3: clean, ended by the emulator. Race 19.7 fps (12 windows).
  - Same rule on baselines: split1 24.7, split2 22.0, split-off 22.1. No gain shown; noise is large.
- Batch fires: split-batch3 dma_run_words=107M in 60M runs (1.8 words/run) vs 91M single words.
  Runs are short, so a ring entry per run saves little; the per-word front work (FrontCallMethod)
  remains. Not a lever on its own. Crashes were memory (heap's work), not the split.
