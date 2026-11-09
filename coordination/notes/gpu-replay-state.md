# GPU replay: engine state to save and restore (2026-11-01)

Inventory of a42b4d4 + our patches (read-only research), for gpu-replay.md's snapshot.
R = scratch-z/src (a42b4d4 patched). SAVE = serialize; REBUILD = recreate or let refill; DRAIN = empty first.

## Before the snapshot (a clean boundary needs more than an idle GPU thread)
- GpuSplit::Drain(); every engine's ConsumeSink() (PS5 dma_batch leaves runs in method_sink).
- Fences: rasterizer->FlushCommands(), WaitForIdle(), ReleaseFences(true) (pending callbacks hold
  semaphore/query writes and IncrementHost).
- FlushRegion(CacheType::All) over all mapped device ranges: GPU-written images/buffers must reach
  guest memory, or the lazily refilled caches lose render targets. NX2 sparse images live only on the
  host (nx2.h:147-166): download or serialize.
- MemoryManager::FlushCaching() per address space (accumulator + PS5 deferred CPU-write queues).
- Best with no pending acquire (Scheduler::HasPendingChannels() false); else save them (below).

## Per channel (~120 KB)
- Maxwell3D: regs (14 KB), shadow_state (14 KB), macro_positions, macro_engine.uploaded_macro_code,
  executing_macro/macro_params/macro_segments/current_macro_dirty, native_data_ram (12 KB),
  upload_state (if mid-upload), draw_manager draw_state/instance_index/draw_texture_state/
  indirect_state/inline index vector, state.shader_stages[].const_buffers (then re-bind them into the
  buffer cache: BindGraphicsUniformBuffer), execute_on, replace_table. REBUILD: macro cache, dirty
  tables (set all flags), split FrontState (move its executing_macro/parameters back after Drain).
- KeplerCompute: regs (13 KB), native_registers (16 KB), native_bind_pointer, native_compute_uploaded,
  native_executing_macro, native_parameters (authoritative copy in FrontState when front macros run);
  Switch 1 upload_state/uploads. REBUILD via SetNativeMacroMode / SetNativeDataRam.
- KeplerMemory regs + upload_state; Fermi2D regs; MaxwellDMA regs (8 KB).
- Every engine (EngineInterface): method_sink (or consume), current_dma_segment, current_dirty.
- DmaPusher: dma_state (method_count can be non-zero across lists), dma_increment_once,
  subchannel types (rebind pointers), split_method_end; queue/offsets only if suspended on an acquire.
- Puller: regs (~1 KB: semaphore address/sequence, fence), bound_engines[8], pending_acquire.
- ChannelState: channel id -> address space -> program_id; recreate channels in recorded id order
  (ids are sequential from 1) after Tegra::g_nx2_ampere = true; then overwrite engine state.

## Shared (process statics, never reset: a second game in one app session inherits them too)
- Nx2MacroContext (channel_state.cpp:31): engine/compute_engine native_code[0x3000] + write offset +
  code generation, positions[0x80], compute_positions[0x80]. REBUILD the decoders/JITs/memos.
- NativeQmdScheduler::states (native_qmd.h:137): QMD dependency counters.
- Host1x::SyncpointManager syncpoints_guest[192] and syncpoints_host[192] (absolute values).
- g_nx2_semaphore_regions[32], g_nx2_sparse_state + g_nx2_sparse_low/high; a replay-owned
  g_nx2_sparse_backing handler (the game's backs blocks from its process page table).

## Address spaces
- Tegra::MemoryManager per AS: constructor bits (NX2 ~40/17/12), mappings as coalesced runs
  {gpu_va, size, mapped|reserved, dev_addr, kind, big|small} replayed with Map/MapSparse after the
  rasterizer is bound; native_syncpoint_shim (NX2 acquires read syncpoints through it).
- DeviceMemoryManager (SMMU): runs {dev_page, phys_page, count} from tracked_entries; the allocator's
  allocated ranges (AllocateFixed on restore). Needed: a MapPhysical(DAddr, PAddr, size) with Map's
  loop and cpu_backing_address = 0 (UpdatePagesCachedCountNoLock then skips; MarkRegionCaching calls
  at .inc:634/644 are not null-checked). Physical page contents at the same DeviceMemory offsets.

## Capture hooks (one global sequence counter across all three)
- Submissions: GPU::Impl::PushGPUEntries (gpu.cpp:205), with NX2 submitted_words when snapshotted.
- Maps: Tegra::MemoryManager::Map/MapSparse/Unmap; DeviceMemoryManager::Map/Unmap/Allocate*/Free
  (log the physical pages after Map); Nx2 semaphore/sparse table changes. Nx2Unmap/Release/sparse
  destroy run on the GPU thread (RunInOrder); Nx2Map on service threads.
- CPU-side: GPU::Impl::OnCPUWrite / InvalidateRegion / FlushRegion / OnCPURead /
  FlushAndInvalidateRegion. They fire for tracked pages only: the dirty-page diff before each
  submission is still required.

## Not reproducible bit for bit
GetTicks() in query timestamps (StampQueryResult) and occlusion/statistics counters.
