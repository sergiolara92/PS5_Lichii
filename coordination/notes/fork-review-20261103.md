# Fork and upstream review, 2026-11-03

Base: Eglantine a42b4d4 (on hotfix/amd-sparse-lazy-crash, forked from Eglantine nx2 at 7b6f27b91,
Oct 31) plus patches 0001-0038. "In tree" checked with git apply --check against the patched source
and the port's build-time edits.

## Translator (CPU JIT) sources
- Upstream Eden (git.eden-emu.dev/eden-emu/eden, cloned to forks/Eden.git): dynarmic since October is
  cleanups (constant pool, configure struct, CMake, arm64 relocations, SM3/SM4 tests). 4fe5f62c3 is in
  our tree, but its A64 sign-extension part was reverted upstream (6374f7f51, "breaks MK8D"); the path
  only runs when guest backing sits below 2^39, which it does not on the PS5 (no shl/sar in the hot
  blocks). Latent only.
- My old port (PS5_ProsperoEden): every JIT commit (direct page table and shared page lookups b942163,
  free-register moves etc. 64a1ee7, checked fastmem demotion 4372034, batch commit 9c898fe, ThinLTO
  96d8f4a) is already carried by headless/. Upstream ProsperoEden has nothing after 2026-10-17.
- No prior art found for keeping guest registers in host registers across blocks.

## Djihads80/Eglantine nx2 (351 commits beyond the fork point)
Ranked; "no" = not in our tree.
1. 256b1dc34 command lists copied before their channel waited are read from memory (dma_pusher,
   puller, nvhost_gpu; 18 lines). MKW fills lists after submit: stale copies = wrong draws. Low risk, applies. no
2. Task/mesh programs 310dd596d, 02504aeb3, eb02d1dd5, 924338226 (+6d9821d50 EDEN_NX2_NO_MESH): MKW's
   rejected stage 3/4 programs. High risk: PS5 RADV mesh shaders are off by default (RADV_PS5_MESH=1). no
3. edabc3e61 memory_manager copies big pages without host memory page by page: crash with lazily
   backed regions (our 0031 default). Low risk, applies. no
4. a407703f6 parts: FindBuffer/MarkWrittenBuffer bounds guard (MKW crash ~4.5 min) and ASTC transcode
   wait=true (BC3 flashing on RADV). Low risk. partly
5. Sparse tile remap (nx2:gpu 216) 4d6a13e3d, 139b7e5ef, 1c794af56, 3d6156434, e277b4ba6: MKW terrain
   streaming leaks ~2.5 GB/10 min without it. High risk, large; EDEN_NX2_SPARSE_REMAP=0 A/B. no
6. Lowering fixes ef3ae4ae2 (FSEL modifiers) + 7f42af81a (cache bump), 2abe49ad7 (FFMA rrc negate,
   branch fix/nx2-ffma-rrc-negate). Low risk. no
7. Ampere lowering series (~45 commits, lower_to_maxwell.h 2925 vs our 2320 lines): fewer rejected
   programs. Medium risk; port whole file, re-apply 0004/0005, bump cache. no
8. Texture handles from storage buffers 561f87abe ... c83eb1d1f (texture_pass.cpp +432). Medium-high. no
9. Guest readback c75fa8b3f, 638795281 (branch neo.ai/gpu-buffer-readback-optimization) + 06e211e0d:
   the ~2.5% of core 0 in reads of GPU-tracked memory. Medium, untested by its author. no
10. Pipelines: 8551ee646 drop SPIR-V modules after build (safe); boot set of last 2 sessions
    be76402ab/5de94bdd1/a999d34f3 and eviction 2f94fb968 (reverted for Switch 2 on another branch). no
11. Texture cache 446c34aa8, f398ff2e1, 3871de9da: deleted images freed once the GPU is past them
    (1-1.5 GB on PC), slot vectors bounded. Medium. no
12. 8266a6a91 ASTC opaque scan by table (224 -> 37 us per texture on the GPU thread). Low. no
13. 6d4c59563 reuse ASTC transcode decode images. Medium. no
14. c7dffce68 compact dynarmic per-block tables (100-200 MB est.). Medium, anchors. partly
15. f8813998e loose RomFS built once, when read. Low-medium. no
16. 00285925f bitmap scans skip empty words (slot_vector, k_priority_queue). Low-medium. no
17. 202c6b035, 46b2577bf + 8556e2701 reuse unchanged small uniform buffers. Low-medium. no
18. 3005cc414, affa8969a crash guards. Very low. no
19. 594e07ee5 buffer collector starts at 512 MiB on Switch 2. Low. no
20. 8f1435f8d GPU-written command lists read from copies (40 -> 45 fps PC replay). High; we dropped
    the earlier 6612817cb approach (black bush). no
Skipped: RADV sparse-unbound workarounds (our winsys maps a zero page), host_memory/punch-hole and
sparse_large_vector changes (replaced by the port), 2 GiB graphics pool, present pacing, GPU-thread
mapping, other-game fixes (FSR/DLSS stand-in, vertex repack, X8Z24, 32 SSBOs), video, network, UI, CI.

## PokeDinner (Franseven) master since 2026-10-25 (383 commits; 140 not in our base by subject)
1. 57cb8bb1a deferred instance state: extra instances of an instanced draw are drawn before NVN's
   macro resets base instance / c[0][0x644] (draw_manager, maxwell_3d). Possible misplaced/invisible
   instanced objects in MKW. Medium; split front needs the same. no (draw part applies)
2. Shader translation speed (Wael3rd) a3c4ce20a, ce9686500, 981247484, 8b32fcb62, 692780fab,
   2b1eda0eb: identical SPIR-V over 11,883 MKW modules; CFG 337 -> 181 ms, one compute 593 -> 75 ms.
   Low; all apply in order. no
3. Async native pipeline translation 9a876a744 + 6d8f95f08 (+452dc2736, b59fd995b, c40406010):
   2-4 ms per new pipeline off the GPU thread, but draws skipped until ready. Medium-high. no
4. 06c9f77a5 WalkBlock ignores stale page runs at unmapped pages (5 lines). Low, applies. no
5. c3be47da9 (+73f25533a) copy back only CPU-read buffer pages; held writes released per list.
   Medium; overlaps 0003 and Djihads readback. partly
6. 9582ebe2d drop the Switch 2 semaphore mirror (clobbers 3D report counters: pop-in). Medium-high,
   needs MKW A/B. no
7. 91ec32e66 GP-start black screen after a movie (Djihads 031e58e93 is the better version). no
8. 7e8c527e2 (+780b9be03) re-upload only changed pages of large streamed images. Medium. no
9. 57ffa55a8 generic part: WaitForIdle with a real memory barrier (ours: TOP_OF_PIPE, no barrier),
   MEMBAR; ValidId guard; passthrough mask reset. Low-medium. no
10. d39c3b4fd VMA falls back to host memory when VRAM is full. Low, applies. no
11. 46df8e9cb threaded movie decode (1440p VP9). Low, applies. no
12. Shader correctness: 8b4384dda, 2a0bd8e68, ff676b650, 1064c5cb0, 87df5ae27, d27746a0d, eace83603;
    lowering additions 34f0511fd, 56f620345, a0b7413bf, 481621423, 795f8b269, bbdc15f65, c2038bc67,
    ad1c417ab, c8bb9a3be, d54f979fb, fdb3d9044, 303b20eb5 (5116272a1, 2d53aac0a, 0b31adfe7 conflict;
    4adbb5fd0 changes Switch 1 behaviour). Low. no
Skipped: 5a9573121 (drains per small macro payload), 76df4591a (one Bananza shader), 2eff58fb0 (drops
the ASTC wait Djihads adds), 908cc6d3b (sync pipelines), 892eaec46 (2048 ASTC cap, opt-in at most),
desktop VRAM rules, macOS hypervisor, Windows/MoltenVK, audio, applets, Switch 1 upstream, NSZ.

## Zortos (zortos293) nx2 since 2026-10-25 (555 commits; 289 in our base, 239 also Djihads nx2)
1. e6d53e728 (= 9a5a1fbdf) dynarmic EndOfAllocScope skips untouched locations; DefineValueImpl scans
   debug-only. MKW 18.5 -> 12.8 us per compiled block; >60 ms frames in the first 45 s 57 -> 31.
   Low-medium. no (applies). (Earlier note said reg_alloc.cmake "goes further": check overlap.)
2. 049c9633e texture handles holding a header byte offset (index*32): black via null view. Low. no
3. Cube views over 2D-array images (8cd5c6a44 ImageView + IsCubeCompatible, ~15 lines): point-light
   shadow samples read zero. Low. no
4. Clear(): no ZPass counter on deferred load-op clears (8cd5c6a44, one line; 62bc7538b): occlusion
   query begun outside a render pass, invalid Vulkan; MKW visibility tests. Low. no
5. 40902b09c unwritten sparse blocks stay without memory when cleared (~1 GB at MKW boot). Medium;
   needs the 216 remap series and 0029/0031 adaptation. no
6. Multi-range storage segments (b3f983a6b, 5bef7354b owners): stale slot ids after a merge. Medium-low. no
7. Storage-buffer index guard (8cd5c6a44): >16 SSBOs write past storage_buffers[stage]. Low. no
8. Native geometry-stage programs (8cd5c6a44 geometry_exit.h ...): MKW "Unsupported native graphics
   shader stage 4". High, large. no
9. badc91ebc block_range_information multimap index: only together with c7dffce68. no
10. SPSCQueue slot lifetime / PopWaitFor (common/bounded_threadsafe_queue.h). Low. no
11. GPU::Impl destructor order (NotifyShutdown, renderer reset before channels): clean shutdown. Low. no
12. df8d77511 audio circular-buffer sink free. 3 lines. no
13. 914423e87 dynarmic drops the cached code page on invalidation. 2 lines. no
14. ASTC->BC transcode decode format / view formats (Vulkan validity). Low. no
15. Quad control only with maximal reconvergence; CanDiscardPendingDraw. Low. no
Risk signal: Zortos reverted eb99da607 (= PokeDinner 57cb8bb1a deferred instance state) in ac22c4087.

## seedds/nxnative and azmasm/switch2mac (MIT; clones in forks/nxnative, forks/switch2mac)
Native ARM execution on Apple chips (offline NSO rewriting, NVN replaced by a Metal layer, Ryujinx-based
shader translation): nothing for an x86-64 JIT. Worth taking:
1. MKW's mesh/task pipeline semantics (switch2mac docs/mario-kart-world.md "Ground, roads and mesh
   shaders"; nx2_lower lower_to_maxwell.h case 0x923/0x927): mode words for header, task payload (+4 mesh
   group count), per-vertex planes of 0x80 bytes, primitive index bytes, count header. Ours drops the
   0x927 writes on 11 programs and rejects stages 3/4 (shader_environment.cpp:555). Semantics only; our
   own Vulkan mesh path; check PS5 RADV mesh support first (RADV_PS5_MESH=1, GFX10_3+). High value, large.
2. UPOPC (0x2bf) -> POPC on the uniform registers' GPR copies (~5 lines), needed with 1.
3. Broader SURED (0x59f): all dims except 1, every reduction op; ours (0004) dim 3 / ADD only. Low.
4. Spin-wait detection for exclusive-lock loops without WFE/YIELD (idea only). Unknown value.
Caution for 64-bit fastmem: MKW's CPU writes terrain height tiles after their textures exist; the window
must keep CPU-write tracking (page protection) or the near ground goes flat/invisible.
Skipped: SDK patch tables with original Nintendo bytes, NVN2 entry points of unclear origin (legal), all
native-ARM CPU code, Metal/MetalFX layer, pipeline manifests, matrix-unit HMMA, nn::codec movies.
