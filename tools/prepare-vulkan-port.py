"""Adapt the pinned Eden Vulkan platform boundary and required renderer fixes."""
from pathlib import Path
import struct
import subprocess
import re
import sys
source, output = map(Path, sys.argv[1:])
port = Path(__file__).resolve().parents[1] / 'headless'
# EDEN_VK_PORT_LENIENT=1 (the Lichii base until every edit is re-targeted, 2026-11-08): an edit whose anchor is
# missing or ambiguous is skipped and listed (VK-PORT-SKIP) instead of stopping the build.
import os
LENIENT = os.environ.get('EDEN_VK_PORT_LENIENT') == '1'
SKIPPED = []
# EDEN_VK_PORT_UPSTREAM=<file names, comma-separated> (bisecting): those files keep the upstream text whole.
UPSTREAM_FILES = {name for name in os.environ.get('EDEN_VK_PORT_UPSTREAM', '').split(',') if name}
def adapt(relative, destination, replacements):
    text = original = (source / relative).read_text()
    if Path(relative).name in UPSTREAM_FILES:
        print(f"VK-PORT-UPSTREAM {relative}: kept whole (EDEN_VK_PORT_UPSTREAM)", file=sys.stderr)
        replacements = []
    skipped = False  # lenient: a file's edits often depend on each other, so one skip keeps the upstream file
    for old, new in replacements:
        if not old or text.count(old) != 1:
            if LENIENT:
                SKIPPED.append(f"{relative}: {old.splitlines()[0][:80] if old.strip() else '(empty)'}")
                print(f"VK-PORT-SKIP {relative}: {(old.splitlines() or [''])[0][:80]}", file=sys.stderr)
                skipped = True
                continue
            raise RuntimeError(f"Pinned Vulkan anchor changed: {relative}: {old[:60]}")
        text = text.replace(old, new)
    if skipped:
        print(f"VK-PORT-SKIP {relative}: upstream file kept whole", file=sys.stderr)
        text = original
    # The flush-area fast path trusts the image-page marks texture_cache.h's edits set: without them
    # every read would skip the texture cache, so the fast path goes too.
    # EDEN_VK_PORT_NO_FLUSH_FAST=1 (bisecting) drops the fast path alone.
    if 'texture_cache.h' in UPSTREAM_FILES or os.environ.get('EDEN_VK_PORT_NO_FLUSH_FAST') == '1':
        text = text.replace('if (::Eden::ImagePages::NeverHeld(addr, size)) {', 'if (false) {')
    path = output / destination
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)

adapt('src/core/frontend/emu_window.h', 'include/core/frontend/emu_window.h',
      [('    Xcb,', '    Xcb,\n    PS5,')])
adapt('src/video_core/renderer_vulkan/vk_rasterizer.cpp', 'vulkan_rasterizer.cpp', [
    # Compute synchronization (dev_vulkan.h compute_barriers): indirect
    # dispatches returned before the pre-dispatch barrier, and no barrier made a dispatch's
    # writes visible to later indirect arguments, vertex/index fetch, shaders or copies.
    ('void RasterizerVulkan::DispatchCompute() {',
     """namespace {
constexpr VkMemoryBarrier COMPUTE_WRITE_BARRIER{
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .pNext = nullptr,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                     VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
};
constexpr VkMemoryBarrier COMPUTE_INPUT_BARRIER{
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .pNext = nullptr,
    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
};
constexpr VkPipelineStageFlags COMPUTE_CONSUMER_STAGES =
    VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
    VK_PIPELINE_STAGE_TRANSFER_BIT;
} // namespace

void RasterizerVulkan::DispatchCompute() {"""),
    # (The pre-dispatch barrier of the indirect path is upstream's RecordDispatchBarrier since Lichii.)
    ('            cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);\n',
     """            cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);
            if (::Eden::DevVulkan::compute_barriers)
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, COMPUTE_CONSUMER_STAGES, 0,
                                       COMPUTE_WRITE_BARRIER);
"""),
    ('                               [&] { cmdbuf.Dispatch(dim[0], dim[1], dim[2]); });\n',
     """                               [&] { cmdbuf.Dispatch(dim[0], dim[1], dim[2]); });
        if (::Eden::DevVulkan::compute_barriers)
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, COMPUTE_CONSUMER_STAGES, 0,
                                   COMPUTE_WRITE_BARRIER);
"""),
    # CounterEnable below starts a query immediately. A deferred clear has no
    # active pass yet, so later realization would end that query in a different
    # render-pass scope (VUID-vkCmdEndQuery-None-07007). Keep the immediate path
    # when the guest enables occlusion counting.
    ('const bool can_defer_clear = ENABLE_DEFERRED_CLEAR &&',
     'const bool can_defer_clear = ENABLE_DEFERRED_CLEAR && !regs.zpass_pixel_count_enable &&'),
    ('#include <algorithm>', '#include <algorithm>\n#include "performance.h"'),
    # Guest threads take the cache locks here (tracked-page writes, flush-area reads).
    ('bool RasterizerVulkan::OnCPUWrite(DAddr addr, u64 size) {',
     'bool RasterizerVulkan::OnCPUWrite(DAddr addr, u64 size) {\n    ::Eden::Performance::Timer guest_write_timer(::Eden::Performance::guest_cpu_write);'),
    ('VideoCore::RasterizerDownloadArea RasterizerVulkan::GetFlushArea(DAddr addr, u64 size) {',
     'VideoCore::RasterizerDownloadArea RasterizerVulkan::GetFlushArea(DAddr addr, u64 size) {\n    ::Eden::Performance::Timer guest_read_timer(::Eden::Performance::guest_cpu_read);'),
    # (Our image-pages fast path is gone: upstream's own, at the top of GetFlushArea, also requires that
    # no GPU write ever marked the pages. Ours skipped GPU-written buffers, and OoT read stale exposure
    # data: the picture came out ~35% too bright, 2026-11-09.)
    # ... and wait for those locks spinning briefly before sleeping (performance.h GuestCacheLock).
    ('        std::scoped_lock lock{texture_cache.mutex};\n        auto area = texture_cache.GetFlushArea(addr, size);',
     '        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);\n'
     '        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};\n        auto area = texture_cache.GetFlushArea(addr, size);'),
    ('        std::scoped_lock lock{buffer_cache.mutex};\n        if (buffer_cache.OnCPUWrite(addr, size)) {',
     '        ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);\n'
     '        std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};\n        if (buffer_cache.OnCPUWrite(addr, size)) {'),
    ('        std::scoped_lock lock{texture_cache.mutex};\n        texture_cache.WriteMemory(addr, size);\n    }\n    pipeline_cache.InvalidateRegion(addr, size);',
     '        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);\n'
     '        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};\n        texture_cache.WriteMemory(addr, size);\n    }\n    pipeline_cache.InvalidateRegion(addr, size);'),
    # (Our dispatch interval and empty-flush skip are replaced by upstream's SubmissionPolicy since Lichii:
    # a hand-off every 32 draws, a draw budget per submission and deferred fence flushes.)
    # Where the ~4 us a draw spends goes: six phases under one running clock, each Mark taking the
    # gap since the previous one so they do not overlap. The Timer still wraps the whole function,
    # so a draw that returns early is charged for its unmarked tail and the sum of the six falls
    # short by that amount plus two clock reads per phase. control/scheduler.cpp aside, this is the
    # only place the dispatch's time can be spent - so its six numbers should add up to it.
    ('''    namespace Profile = VideoCommon::Nx2Profile;
    if (InvalidateBeforeUse()) {
        gpu.InvalidateGPUCache();
    }
    {
        Profile::Scope scope{Profile::Zone::FlushWork};
        FlushWork();
        gpu_memory->FlushCaching();
    }
''', '''    const ::Eden::Performance::Timer rasterizer_draw_timer(::Eden::Performance::rasterizer_draw);
    auto phase = ::Eden::Performance::MarkStart();
    namespace Profile = VideoCommon::Nx2Profile;
    if (InvalidateBeforeUse()) {
        gpu.InvalidateGPUCache();
    }
    {
        Profile::Scope scope{Profile::Zone::FlushWork};
        FlushWork();
        gpu_memory->FlushCaching();
    }
    ::Eden::Performance::Mark(::Eden::Performance::draw_flush, phase);
'''),
    ('''        return pipeline_cache.CurrentGraphicsPipeline();
    }()};
''', '''        return pipeline_cache.CurrentGraphicsPipeline();
    }()};
    ::Eden::Performance::Mark(::Eden::Performance::draw_pipeline, phase);
'''),
    ('''    pipeline->SetEngine(maxwell3d, gpu_memory);
    const u32 repack_dirty''', '''    pipeline->SetEngine(maxwell3d, gpu_memory);
    // Waiting for both cache mutexes. SetEngine between them and here is two pointer assignments,
    // and the hold itself carries on through what follows: the phases below measure it.
    ::Eden::Performance::Mark(::Eden::Performance::draw_lock_wait, phase);
    const u32 repack_dirty'''),
    ('''        TraceRenderState(is_indexed, "configure-failed", nullptr);
        return;
    }
''', '''        TraceRenderState(is_indexed, "configure-failed", nullptr);
        return;
    }
    ::Eden::Performance::Mark(::Eden::Performance::draw_config, phase);
'''),
    ('''    {
        Profile::Scope scope{Profile::Zone::DrawRecord};
''', '''    ::Eden::Performance::Mark(::Eden::Performance::draw_state, phase);
    {
        Profile::Scope scope{Profile::Zone::DrawRecord};
'''),
    ('''        draw_func();
    }
    if (probe) {''', '''        draw_func();
    }
    ::Eden::Performance::Mark(::Eden::Performance::draw_record, phase);
    if (probe) {'''),
    # Development: where OnCPUWrite spends its time (performance.h CpuWriteDetail).
    ("""    DEBUG_ASSERT(addr != 0 || size != 0);
    CountGuestAccess(g_guest_access.write_locked);
    {
        ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);
        std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
    }
    {
        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        texture_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return false;
""",
     """    DEBUG_ASSERT(addr != 0 || size != 0);
    CountGuestAccess(g_guest_access.write_locked);
    // A write to the page this core's last queued write covers joins it (performance.h DeferWrite).
    if (::Eden::Performance::cpu_write_defer.load(std::memory_order_relaxed) &&
        ::Eden::Performance::ExtendDeferredWrite(addr, size, ::Eden::Performance::cpu_write_core)) {
        return false;
    }
    // Under the buffer cache lock: mark the buffer pages after this write CPU-modified as their own
    // first writes would (performance.h cpu_write_ahead), up to a page no buffer covers or one the
    // GPU modified.
    const auto write_ahead = [this](DAddr written, u64 written_size) {
        const unsigned pages = ::Eden::Performance::cpu_write_ahead.load(std::memory_order_relaxed);
        unsigned marked = 0;
        DAddr page = Common::AlignDown(written + written_size - 1, Core::DEVICE_PAGESIZE) + Core::DEVICE_PAGESIZE;
        for (; marked < pages; ++marked, page += Core::DEVICE_PAGESIZE)
            if (!buffer_cache.IsRegionRegistered(page, Core::DEVICE_PAGESIZE) ||
                buffer_cache.OnCPUWrite(page, Core::DEVICE_PAGESIZE))
                break;
        return marked;
    };
    if (::Eden::Performance::cpu_write_detail.load(std::memory_order_relaxed)) [[unlikely]] {
        using Detail = ::Eden::Performance::CpuWriteDetail;
        Detail& stats = ::Eden::Performance::cpu_write_stats[::Eden::Performance::cpu_write_core % ::Eden::Performance::kGuestCores];
        long long mark = ::Eden::Performance::NowNs();
        const auto lap = [&](std::atomic<unsigned long long> Detail::*total) {
            const long long now = ::Eden::Performance::NowNs();
            (stats.*total).fetch_add(static_cast<unsigned long long>(now - mark), std::memory_order_relaxed);
            mark = now;
        };
        {
            ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);
            std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
            lap(&Detail::buffer_wait_ns);
            const bool registered = buffer_cache.IsRegionRegistered(addr, size);
            const bool gpu_modified = buffer_cache.OnCPUWrite(addr, size);
            lap(&Detail::buffer_ns);
            (!registered ? stats.unregistered : gpu_modified ? stats.gpu_modified : stats.cpu_modified)
                .fetch_add(1, std::memory_order_relaxed);
            if (gpu_modified) {
                return true;
            }
            if (registered)
                stats.ahead_pages.fetch_add(write_ahead(addr, size), std::memory_order_relaxed);
            lap(&Detail::buffer_ns);
        }
        {
            ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
            std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
            lap(&Detail::texture_wait_ns);
            texture_cache.WriteMemory(addr, size);
            lap(&Detail::texture_ns);
        }
        pipeline_cache.InvalidateRegion(addr, size);
        lap(&Detail::shader_ns);
        return false;
    }
    {
        ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);
        std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
        write_ahead(addr, size);
    }
    {
        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        texture_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return false;
"""),
    # A forced release blocks the GPU thread until the fence thread has retired
    # every older fence. Time only that producer-side drain.
    ('    fence_manager.WaitPendingFences(force);',
     '''    if (force) {
        ::Eden::Performance::Timer drain_timer(::Eden::Performance::gpu_fence_drain, 0, true);
        fence_manager.WaitPendingFences(force);
    } else {
        fence_manager.WaitPendingFences(force);
    }'''),
    # UpdateDynamicStates asked the cache for the current pipeline a second time
    # per draw, recomputing the same fixed-state key. Reuse PrepareDraw's result;
    # other callers keep the original lookup.
    ('template <typename Func>\nvoid RasterizerVulkan::PrepareDraw(bool is_indexed, Func&& draw_func, bool is_indirect) {',
     '''namespace {
// GPU thread only: the pipeline PrepareDraw refreshed for the draw in progress.
GraphicsPipeline* prepared_pipeline = nullptr;
} // namespace

template <typename Func>
void RasterizerVulkan::PrepareDraw(bool is_indexed, Func&& draw_func, bool is_indirect) {'''),
    ('    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};\n    // update engine as channel may be different.',
     '''    ::Eden::Performance::YieldToGuests();  // a guest thread asleep on a cache lock goes first
    ::Eden::AstcPolicy::Draw();
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    prepared_pipeline = pipeline;
    SCOPE_EXIT {
        prepared_pipeline = nullptr;
    };
    // update engine as channel may be different.'''),
    # (UpdateDynamicStates reusing the draw's pipeline is upstream's draw_pipeline since Lichii.)
    # Development GPU fault probe (dev_vulkan.h, pipeline_trace=on): once the runner
    # creates /app0/sync-draws.txt, report every draw/dispatch to klog and wait for the
    # GPU, so the last report before a fatal GPU fault names the operation.
    ('#include "performance.h"', '#include "performance.h"\n#include "dev_vulkan.h"\n#include "diagnostics.h"\n#include "image_pages.h"\n#include "astc_policy.h"\n#include <filesystem>'),
    ('GraphicsPipeline* prepared_pipeline = nullptr;',
     'GraphicsPipeline* prepared_pipeline = nullptr;\n'
     '// The pipeline of the last prepared draw, for the development fault probe.\n'
     'GraphicsPipeline* probe_pipeline = nullptr;\n'
     'bool SyncProbeActive() {\n'
     '    static unsigned poll = 0;\n'
     '    static bool active = false;\n'
     '    if (!active && ::Eden::DevVulkan::trace_pipelines && (++poll & 255) == 0)\n'
     '        active = std::filesystem::exists("/app0/sync-draws.txt");\n'
     '    return active;\n'
     '}\n'
     'void SyncProbe(Scheduler& scheduler, const char* kind, const void* pipeline) {\n'
     '    if (!SyncProbeActive()) return;\n'
     '    static unsigned long long sequence = 0;\n'
     '    char text[96];\n'
     '    std::snprintf(text, sizeof(text), "%s #%llu p=%p", kind, ++sequence, pipeline);\n'
     '    ::Eden::Report("sync", text);\n'
     '    scheduler.Finish();\n'
     '}'),
    ('    prepared_pipeline = pipeline;\n', '    prepared_pipeline = pipeline;\n    probe_pipeline = pipeline;\n'),
    # Development: how long draw preparation holds the cache locks guest writes wait for.
    ('    probe_pipeline = pipeline;\n',
     '    probe_pipeline = pipeline;\n'
     '    const long long hold_start = ::Eden::Performance::cpu_write_detail.load(std::memory_order_relaxed)\n'
     '        ? ::Eden::Performance::NowNs() : 0;\n'
     '    SCOPE_EXIT {\n'
     '        if (hold_start != 0) ::Eden::Performance::AddSince(::Eden::Performance::draw_cache_hold, hold_start);\n'
     '    };\n'),
    ('        }\n    });\n}\n\nvoid RasterizerVulkan::DrawIndirect() {',
     '        }\n    });\n    SyncProbe(scheduler, "draw", probe_pipeline);\n}\n\nvoid RasterizerVulkan::DrawIndirect() {'),
    ('void RasterizerVulkan::DrawTexture() {', 'void RasterizerVulkan::DrawTexture() {\n    SCOPE_EXIT { SyncProbe(scheduler, "draw_texture", nullptr); };'),
    ('void RasterizerVulkan::Clear(u32 layer_count) {', 'void RasterizerVulkan::Clear(u32 layer_count) {\n    SCOPE_EXIT { SyncProbe(scheduler, "clear", nullptr); };'),
    ('    }()};\n    if (!pipeline) {\n        if (kepler_compute->IsNativeMode()) {\n            // A program rejected before returns no pipeline without throwing (ComputeShader).',
     '    }()};\n    SCOPE_EXIT { SyncProbe(scheduler, "dispatch", pipeline); };\n    if (!pipeline) {\n        if (kepler_compute->IsNativeMode()) {\n            // A program rejected before returns no pipeline without throwing (ComputeShader).'),
    # Guest writes while the GPU thread holds a cache lock are queued instead of waiting
    # (performance.h cpu_write_defer); the GPU thread runs them before its next draw.
    ('''    {
        ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);
        std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
        write_ahead(addr, size);
    }
    {
        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        texture_cache.WriteMemory(addr, size);
    }
''',
     '''    {
        if (!::Eden::Performance::GuestCacheLockOrDefer(buffer_cache.mutex)) {
            ::Eden::Performance::DeferWrite(addr, size, ::Eden::Performance::cpu_write_core);
            return false;
        }
        std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
        write_ahead(addr, size);
    }
    {
        if (!::Eden::Performance::GuestCacheLockOrDefer(texture_cache.mutex)) {
            ::Eden::Performance::DeferWrite(addr, size, ::Eden::Performance::cpu_write_core);
            return false;
        }
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        texture_cache.WriteMemory(addr, size);
    }
'''),
    ('''            ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);
            std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
            lap(&Detail::buffer_wait_ns);
''',
     '''            if (!::Eden::Performance::GuestCacheLockOrDefer(buffer_cache.mutex)) {
                lap(&Detail::buffer_wait_ns);
                ::Eden::Performance::DeferWrite(addr, size, ::Eden::Performance::cpu_write_core);
                return false;
            }
            std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
            lap(&Detail::buffer_wait_ns);
'''),
    ('''            ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
            std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
            lap(&Detail::texture_wait_ns);
''',
     '''            if (!::Eden::Performance::GuestCacheLockOrDefer(texture_cache.mutex)) {
                lap(&Detail::texture_wait_ns);
                ::Eden::Performance::DeferWrite(addr, size, ::Eden::Performance::cpu_write_core);
                return false;
            }
            std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
            lap(&Detail::texture_wait_ns);
'''),
    # Guest reads may skip the download check while the texture cache lock is held (cpu_read_skip).
    ('''        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        auto area = texture_cache.GetFlushArea(addr, size);''',
     '''        if (::Eden::Performance::cpu_read_skip.load(std::memory_order_relaxed)) {
            if (!texture_cache.mutex.try_lock()) {
                ::Eden::Performance::cpu_read_skipped.fetch_add(1, std::memory_order_relaxed);
                // Empty: HandleRasterizerDownload does not remember it, so the next read checks again.
                return VideoCore::RasterizerDownloadArea{.start_address = addr, .end_address = addr, .preemtive = true};
            }
        } else {
            ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
        }
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        auto area = texture_cache.GetFlushArea(addr, size);'''),
    ('        if (hold_start != 0) ::Eden::Performance::AddSince(::Eden::Performance::draw_cache_hold, hold_start);\n',
     '        if (hold_start != 0) ::Eden::Performance::AddHold(hold_start);\n'),
    # Dynamic vertex input: a stream with instancing on and frequency 0 advances per vertex, as the
    # pipeline's static vertex input (binding_divisors) and OpenGL's divisor 0 take it. Here it was
    # an instance-rate binding with divisor 0, every vertex reading element 0: geometry collapsed
    # (objects vanishing for a frame, a whole scene drawn as one flat colour).
    ('        const bool is_instanced{regs.vertex_stream_instances.IsInstancingEnabled(binding)};\n',
     '        const bool is_instanced{regs.vertex_stream_instances.IsInstancingEnabled(binding) &&\n'
     '                                input_binding.frequency != 0};\n'),
])
# Host conditional rendering (VK_EXT_conditional_rendering, dev_vulkan.h hcr_mode): count each
# decision (EDEN_DEV_GUEST cond= slots 13-19); hcr=exact sends the conditions Eden would draw
# unconditionally, or predicate from a pending host query, to the exact CPU evaluation;
# hcr=cpu evaluates every condition on the CPU and predicates from constant 0/1 buffers.
adapt('src/video_core/renderer_vulkan/vk_query_cache.cpp', 'vulkan_query_cache.cpp', [
    ('#include "video_core/vulkan_common/vulkan_wrapper.h"\n',
     '#include "video_core/vulkan_common/vulkan_wrapper.h"\n#include "dev_vulkan.h"\n#include "performance.h"\n'),
    ('    vk::Buffer hcr_resolve_buffer;\n',
     '    vk::Buffer hcr_resolve_buffer;\n    vk::Buffer hcr_constant_buffer; // hcr=cpu: predicates 0 and 1\n'),
    ('        hcr_resolve_buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);\n    }\n',
     '''        hcr_resolve_buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);
        if (device.IsExtConditionalRendering() && ::Eden::DevVulkan::hcr_mode == 2) {
            VkBufferCreateInfo constant_ci = buffer_ci;
            constant_ci.size = 2 * sizeof(u32);
            constant_ci.usage = VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
            hcr_constant_buffer = memory_allocator.CreateBuffer(constant_ci, MemoryUsage::Upload);
            const u32 predicates[2]{0, 1};
            std::memcpy(hcr_constant_buffer.Mapped().data(), predicates, sizeof(predicates));
            hcr_constant_buffer.Flush();
        }
    }
'''),
    ('''bool QueryCacheRuntime::HostConditionalRenderingCompareValue(VideoCommon::LookupData object_1,
                                                             [[maybe_unused]] bool qc_dirty) {
    if (!impl->device.IsExtConditionalRendering()) {
        TraceConditionalRendering("extension-unavailable", false, object_1.address);
        return false;
    }
    HostConditionalRenderingCompareBCImpl(object_1.address, true, true);
    TraceConditionalRendering("resolve-zero", true, object_1.address);
    return true;
}''',
     '''namespace {
// hcr=cpu: Maxwell3D::ProcessQueryCondition's exact evaluation (the read flushes pending GPU
// results), then the driver predicates the draws from a constant buffer. kind: 0 Conditional,
// 1 IfEqual, 2 IfNotEqual.
bool HostConditionalRenderingOnCpu(QueryCacheRuntime& runtime, QueryCacheRuntimeImpl& impl, DAddr address,
                                   unsigned kind) {
    if (address == 0 || !impl.hcr_constant_buffer) {
        ::Eden::Performance::CountCondition(13);
        runtime.EndHostConditionalRendering();
        return false;
    }
    u32 compare[6]{};
    impl.device_memory.ReadBlock(address, compare, sizeof(compare));
    const bool equal = compare[0] == compare[4] && compare[1] == compare[5];
    const bool execute = kind == 0 ? compare[0] != 0 && compare[1] != 0 : (kind == 1 ? equal : !equal);
    ::Eden::Performance::CountCondition(execute ? 17 : 18);
    const bool was_running = impl.is_hcr_running;
    if (was_running) {
        runtime.PauseHostConditionalRendering();
    }
    impl.hcr_setup.buffer = *impl.hcr_constant_buffer;
    impl.hcr_setup.offset = execute ? sizeof(u32) : 0;
    impl.hcr_setup.flags = 0;
    impl.hcr_is_set = true;
    impl.is_hcr_running = false;
    if (was_running) {
        runtime.ResumeHostConditionalRendering();
    }
    return true;
}
} // namespace

bool QueryCacheRuntime::HostConditionalRenderingCompareValue(VideoCommon::LookupData object_1,
                                                             [[maybe_unused]] bool qc_dirty) {
    if (!impl->device.IsExtConditionalRendering()) {
        TraceConditionalRendering("extension-unavailable", false, object_1.address);
        return false;
    }
    if (::Eden::DevVulkan::hcr_mode == 2) {
        return HostConditionalRenderingOnCpu(*this, *impl, object_1.address, 0);
    }
    if (::Eden::DevVulkan::hcr_mode == 1 && qc_dirty) {
        ::Eden::Performance::CountCondition(19);
        EndHostConditionalRendering();
        return false;
    }
    ::Eden::Performance::CountCondition(16);
    HostConditionalRenderingCompareBCImpl(object_1.address, true, true);
    TraceConditionalRendering("resolve-zero", true, object_1.address);
    return true;
}'''),
    ('''    if (!impl->device.IsExtConditionalRendering()) {
        TraceConditionalRendering("extension-unavailable", false, object_1.address, object_2.address);
        return false;
    }

    const auto check_in_bc = [&](DAddr address) {''',
     '''    if (!impl->device.IsExtConditionalRendering()) {
        TraceConditionalRendering("extension-unavailable", false, object_1.address, object_2.address);
        return false;
    }
    if (::Eden::DevVulkan::hcr_mode == 2) {
        return HostConditionalRenderingOnCpu(*this, *impl, object_1.address, equal_check ? 1 : 2);
    }

    const auto check_in_bc = [&](DAddr address) {'''),
    ('''    if (!is_in_ac[0] && !is_in_ac[1]) {
        EndHostConditionalRendering();
        TraceConditionalRendering("uncached", false, object_1.address, object_2.address);
        return false;
    }

    if (!qc_dirty && !is_in_bc[0] && !is_in_bc[1]) {
        EndHostConditionalRendering();
        TraceConditionalRendering("guest-synchronized", false, object_1.address, object_2.address);
        return false;
    }
''',
     '''    if (!is_in_ac[0] && !is_in_ac[1]) {
        ::Eden::Performance::CountCondition(13);
        EndHostConditionalRendering();
        TraceConditionalRendering("uncached", false, object_1.address, object_2.address);
        return false;
    }

    if (!qc_dirty && !is_in_bc[0] && !is_in_bc[1]) {
        ::Eden::Performance::CountCondition(13);
        EndHostConditionalRendering();
        TraceConditionalRendering("guest-synchronized", false, object_1.address, object_2.address);
        return false;
    }
    // hcr=exact: a pending host query is not in any buffer yet; the CPU evaluation waits for it.
    if (::Eden::DevVulkan::hcr_mode == 1 && qc_dirty) {
        ::Eden::Performance::CountCondition(19);
        EndHostConditionalRendering();
        return false;
    }
'''),
    ('''            HostConditionalRenderingCompareValueImpl(*objects[j], equal_check);
            TraceConditionalRendering(''',
     '''            ::Eden::Performance::CountCondition(15);
            HostConditionalRenderingCompareValueImpl(*objects[j], equal_check);
            TraceConditionalRendering('''),
    ('''    if (!is_gpu_high) {
        EndHostConditionalRendering();
        TraceConditionalRendering("accuracy-bypass", true, object_1.address, object_2.address);
        return true;
    }

    if (!is_in_bc[0] && !is_in_bc[1]) {
        EndHostConditionalRendering();
        TraceConditionalRendering("query-cache-bypass", true, object_1.address, object_2.address);
        return true;
    }
    HostConditionalRenderingCompareBCImpl(object_1.address, equal_check);''',
     '''    // Eden draws these unconditionally; hcr=exact evaluates them on the CPU instead.
    if (!is_gpu_high || (!is_in_bc[0] && !is_in_bc[1])) {
        ::Eden::Performance::CountCondition(14);
        EndHostConditionalRendering();
        const bool unconditional = ::Eden::DevVulkan::hcr_mode != 1;
        TraceConditionalRendering(!is_gpu_high ? "accuracy-bypass" : "query-cache-bypass", unconditional,
                                  object_1.address, object_2.address);
        return unconditional;
    }
    ::Eden::Performance::CountCondition(16);
    HostConditionalRenderingCompareBCImpl(object_1.address, equal_check);'''),
])
adapt('src/video_core/renderer_vulkan/vk_swapchain.cpp', 'vulkan_swapchain.cpp', [
    # An acquisition error has no usable image index. Forward it to the existing
    # renderer recovery path instead of indexing or presenting an invalid image.
    ('LOG_ERROR(Render_Vulkan, "vkAcquireNextImageKHR returned {}", string_VkResult(result));',
     'LOG_ERROR(Render_Vulkan, "vkAcquireNextImageKHR returned {}", string_VkResult(result));\n        vk::Check(result);'),
    ('LOG_CRITICAL(Render_Vulkan, "Failed to present with error {}", string_VkResult(result));',
     'LOG_CRITICAL(Render_Vulkan, "Failed to present with error {}", string_VkResult(result));\n        vk::Check(result);'),
])
# Keep the overlay in the existing display composition pass (also used for
# frontend screenshots), excluding guest applet captures. Cache its pipeline.
# Preserve upstream ABI, including consumers whose quoted includes resolve to
# the original header. Display/capture purpose belongs to the draw call.
adapt('src/video_core/present.h', 'include/video_core/present.h', [])
adapt('src/video_core/renderer_vulkan/vk_present_manager.h',
      'include/video_core/renderer_vulkan/vk_present_manager.h', [
    ('#include <mutex>', '#include <mutex>\n#include <exception>'),
    ('    std::jthread present_thread;',
     '    std::exception_ptr present_failure;\n    std::jthread present_thread;'),
])
adapt('src/video_core/renderer_vulkan/vk_present_manager.cpp', 'vulkan_present_manager.cpp', [
    ('#include "video_core/renderer_vulkan/vk_present_manager.h"',
     '#include "video_core/renderer_vulkan/vk_present_manager.h"\n#include "gpu_failure.h"\n#include "performance.h"'),
    # GetRenderFrame runs on the GPU thread: time its free-frame and
    # present-completion waits (FIFO back-pressure on the producer).
    ('    // Wait for free presentation frames\n    std::unique_lock lock{free_mutex};',
     '    // Wait for free presentation frames\n    ::Eden::Performance::Timer present_wait_timer(::Eden::Performance::gpu_present_wait, 0, true);\n    std::unique_lock lock{free_mutex};'),
    ('PresentThread(token);', '''try {
            PresentThread(token);
        } catch (...) {
            const auto error = std::current_exception();
            {
                std::scoped_lock lock{queue_mutex, free_mutex};
                present_failure = error;
                present_queue.clear();
            }
            free_cv.notify_all();
            frame_cv.notify_all();
            Eden::RecordGpuFailure(error);
        }'''),
    ('free_cv.wait(lock, [this] { return !free_queue.empty(); });',
     '''free_cv.wait(lock, [this] { return present_failure || !free_queue.empty(); });
    if (present_failure) std::rethrow_exception(present_failure);'''),
    ('            present_queue.push_back(frame);',
     '            if (present_failure) return;\n            present_queue.push_back(frame);'),
    ('frame_cv.wait(queue_lock, [this] { return present_queue.empty(); });',
     'frame_cv.wait(queue_lock, [this] { return present_failure || present_queue.empty(); });'),
])
# Descriptor buffers: Eden's null buffer (nullDescriptor devices) has device address 0,
# but WriteDescriptorBuffer passed it with the binding's range. RADV then builds a real
# descriptor at GPU address 0; desktop kernels only log the resulting page fault, the PS5
# kills the title (TCP read at VA 0).
# The spec requires a null pAddressInfo (or VK_WHOLE_SIZE) for a null descriptor.
# Development A/B switches for device features (headless/dev_vulkan.h, dev-settings).
adapt('src/video_core/vulkan_common/vulkan_device.cpp', 'vulkan_device.cpp', [
    # 16 MiB blocks: the pool the console's GPU allocates from is the CPU's and the guest's too, and
    # what a block holds goes back to it only when the block is empty. With 64 MiB blocks Mario Kart
    # World's collector deleted 2,440 images and the allocator held 3,812 MiB before and after (the
    # 584 left were spread over every block), and the pool ran out. Anything over 8 MiB is then an
    # allocation of its own, given back when freed; the page tables are the same (one per 2 MiB).
    ('            .preferredLargeHeapBlockSize = 16u * 1024u * 1024u,',
     '            .preferredLargeHeapBlockSize = 16u * 1024u * 1024u,'),
    ('#include <algorithm>', '#include <algorithm>\n#include <cstdio>\n#include "dev_vulkan.h"\n#include "performance.h"'),
    # The allocator counts what it holds itself instead of asking the driver. The driver's figures
    # are wrong on the console: it counts each allocation of graphics memory twice (as VRAM and as
    # visible VRAM), so the allocator saw the budget of the 8 GiB heap used up at 4 GiB. From there
    # it could make no more of its 256 MiB blocks and gave every image memory of its own: 926
    # allocations where 40 blocks had done, a third of what it held unused (1.3 of 3.9 GiB in a large
    # open-world game at 2x), and the shared pool cut into hundreds of pieces. Without the driver's
    # budget its own count stands against four fifths of the heap.
    ('    if (extensions.memory_budget) {\n        flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;\n    }\n',
     '    // The driver\'s budget is not asked for: its count is double (tools/prepare-vulkan-port.py).\n'),
    # "Memory in use" for the texture and buffer caches: on the console, the budget less what is
    # left of the pool the CPU shares with the GPU (performance.h, graphics_memory_free).
    ('u64 Device::GetDeviceMemoryUsage() const {\n',
     'namespace {\n'
     '// The largest block of the shared pool that was free when the device was made: about the most\n'
     '// graphics can be given in this session.\n'
     'u64 graphics_capacity = 0;\n'
     '}\n\n'
     'u64 Device::GetDeviceMemoryUsage() const {\n'
     '    if (const auto free_memory = ::Eden::Performance::graphics_memory_free.load(std::memory_order_relaxed);\n'
     '        free_memory && ::Eden::Performance::graphics_usage_from_pool.load(std::memory_order_relaxed)) {\n'
     '        // The caches evict what is unused 1.6 GiB under the budget and evict hard 0.8 GiB under\n'
     '        // it. Where the pool could never give graphics 6.4 GiB, those marks become a quarter\n'
     '        // and an eighth of what it could give.\n'
     '        const double scale = graphics_capacity != 0 && graphics_capacity < 6400_MiB\n'
     '                                 ? static_cast<double>(6400_MiB) / static_cast<double>(graphics_capacity)\n'
     '                                 : 1.0;\n'
     '        const u64 free_bytes = static_cast<u64>(static_cast<double>(free_memory()) * scale);\n'
     '        return device_access_memory > free_bytes ? device_access_memory - free_bytes : 0;\n'
     '    }\n'),
    ('    extensions.descriptor_buffer = features.descriptor_buffer.descriptorBuffer;',
     '    extensions.descriptor_buffer = features.descriptor_buffer.descriptorBuffer &&\n'
     '        !::Eden::DevVulkan::disable_descriptor_buffer;'),
    ('    features.robustness2.robustBufferAccess2 = VK_FALSE;\n'
     '    features.robustness2.robustImageAccess2 = VK_FALSE;\n'
     '    extensions.robustness_2 = features.robustness2.nullDescriptor;',
     '    if (!::Eden::DevVulkan::robustness2) {\n'
     '        features.robustness2.robustBufferAccess2 = VK_FALSE;\n'
     '        features.robustness2.robustImageAccess2 = VK_FALSE;\n'
     '    }\n'
     '    if (::Eden::DevVulkan::disable_null_descriptor) {\n'
     '        features.robustness2.nullDescriptor = VK_FALSE;\n'
     '    }\n'
     '    extensions.robustness_2 = features.robustness2.nullDescriptor;'),
    ('            (queue_family_properties[*graphics].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;',
     '            (queue_family_properties[*graphics].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0 &&\n'
     '            !::Eden::DevVulkan::disable_sparse;'),
    ('            features.custom_border_color.customBorderColorWithoutFormat;\n    }\n'
     '    RemoveExtensionFeatureIfUnsuitable(extensions.custom_border_color',
     '            features.custom_border_color.customBorderColorWithoutFormat &&\n'
     '            !::Eden::DevVulkan::disable_custom_border;\n    }\n'
     '    RemoveExtensionFeatureIfUnsuitable(extensions.custom_border_color'),
    # dev-settings conditional_rendering=off: guest render-enable conditions are resolved
    # without VK_EXT_conditional_rendering (the path Eden takes on drivers without it).
    ('    // VK_EXT_border_color_swizzle\n',
     '    if (::Eden::DevVulkan::disable_conditional_rendering) {\n'
     '        extensions.conditional_rendering = false;\n    }\n'
     '    // VK_EXT_border_color_swizzle\n'),
    # The heaps and the budget the texture/buffer caches size themselves from.
    ('            device_access_memory = std::min<u64>(device_access_memory, normal_memory + scaler_memory);\n'
     '        }\n    }\n}\n',
     '            device_access_memory = std::min<u64>(device_access_memory, normal_memory + scaler_memory);\n'
     '        }\n    }\n'
     '    if (const auto free_memory = ::Eden::Performance::graphics_memory_free.load(std::memory_order_relaxed)) {\n'
     '        graphics_capacity = free_memory();\n'
     '    }\n'
     '    std::printf("EDEN_VULKAN_MEMORY integrated=%d heaps=%zu local=%llu initial_usage=%llu device_access=%llu "\n'
     '                "capacity=%llu\\n",\n'
     '                int(is_integrated), valid_heap_memory.size(), static_cast<unsigned long long>(local_memory),\n'
     '                static_cast<unsigned long long>(device_initial_usage),\n'
     '                static_cast<unsigned long long>(device_access_memory),\n'
     '                static_cast<unsigned long long>(graphics_capacity));\n}\n'),
])
# Development GPU fault probe (dev_vulkan.h, submit_sync=on): wait for every
# submission, so the CPU never records against work the GPU has not finished.
GPU_TIME_HELPERS = r'''
namespace {
// GPU execution time, for the FPS overlay (performance.h, gpu_time_wanted) and for the
// development report (dev_vulkan.h gpu_time): timestamps at the start and end
// of every scheduler command buffer, summed once the GPU has run them and printed every
// 5 s as EDEN_GPU_TIME. Worker thread only.
struct GpuTimeProbe {
    static constexpr u32 SLOTS = 512;
    vk::QueryPool pool;
    PFN_vkCmdWriteTimestamp write{};
    double period_ns{};
    u64 next{}, first_pending{};
    u32 current{};
    u64 busy_ns{}, max_ns{}, submissions{};
    std::chrono::steady_clock::time_point report{};
    bool failed{};
    bool stamped{}; // the command buffer being recorded has its first timestamp
} gpu_time;

bool GpuTimeWanted() {
    return ::Eden::DevVulkan::gpu_time || ::Eden::Performance::gpu_time_wanted.load(std::memory_order_relaxed);
}
void GpuTimeGaveUp() {
    gpu_time.failed = true;
    ::Eden::Performance::gpu_time_failed.store(true, std::memory_order_relaxed);
}

void GpuTimeBegin(const Device& device, vk::CommandBuffer cmdbuf) {
    gpu_time.stamped = false;
    if (!GpuTimeWanted() || gpu_time.failed) return;
    if (!gpu_time.write) {
        const auto& dld = device.GetDispatchLoader();
        gpu_time.write = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
            dld.vkGetDeviceProcAddr(*device.GetLogical(), "vkCmdWriteTimestamp"));
        gpu_time.period_ns = device.GetPhysical().GetProperties().limits.timestampPeriod;
        if (!gpu_time.write || gpu_time.period_ns <= 0) { GpuTimeGaveUp(); return; }
        gpu_time.pool = device.GetLogical().CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = GpuTimeProbe::SLOTS * 2,
            .pipelineStatistics = 0,
        });
        gpu_time.report = std::chrono::steady_clock::now();
    }
    if (gpu_time.next - gpu_time.first_pending >= GpuTimeProbe::SLOTS) {
        GpuTimeGaveUp(); // results stopped arriving; never overwrite a pending slot
        std::printf("EDEN_GPU_TIME failed=ring\n");
        return;
    }
    gpu_time.current = static_cast<u32>(gpu_time.next++ % GpuTimeProbe::SLOTS);
    cmdbuf.ResetQueryPool(*gpu_time.pool, gpu_time.current * 2, 2);
    gpu_time.write(*cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *gpu_time.pool, gpu_time.current * 2);
    gpu_time.stamped = true;
}

void GpuTimeEnd(vk::CommandBuffer cmdbuf) {
    if (!gpu_time.stamped) return;
    gpu_time.write(*cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *gpu_time.pool, gpu_time.current * 2 + 1);
}

void GpuTimeCollect(const Device& device) {
    if (gpu_time.failed || !gpu_time.write) return;
    while (gpu_time.first_pending + 1 < gpu_time.next) {
        const u32 slot = static_cast<u32>(gpu_time.first_pending % GpuTimeProbe::SLOTS);
        std::array<u64, 2> stamps{};
        if (device.GetLogical().GetQueryResults(*gpu_time.pool, slot * 2, 2, sizeof(stamps), stamps.data(),
                                               sizeof(u64), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) break;
        const u64 ns = stamps[1] > stamps[0]
                           ? static_cast<u64>(double(stamps[1] - stamps[0]) * gpu_time.period_ns) : 0;
        gpu_time.busy_ns += ns;
        ::Eden::Performance::gpu_time_busy_ns.fetch_add(ns, std::memory_order_relaxed);
        ::Eden::Performance::gpu_time_results.fetch_add(1, std::memory_order_relaxed);
        gpu_time.max_ns = (std::max)(gpu_time.max_ns, ns);
        ++gpu_time.submissions;
        ++gpu_time.first_pending;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(now - gpu_time.report).count();
    if (wall >= 5'000'000'000 && ::Eden::DevVulkan::gpu_time) {
        std::printf("EDEN_GPU_TIME wall_ms=%.1f busy_ms=%.1f submissions=%llu max_ms=%.2f\n", wall / 1e6,
                    gpu_time.busy_ns / 1e6, static_cast<unsigned long long>(gpu_time.submissions),
                    gpu_time.max_ns / 1e6);
        gpu_time.busy_ns = gpu_time.max_ns = gpu_time.submissions = 0;
        gpu_time.report = now;
    }
}
} // namespace
'''
# While the console's pool is short, every staging buffer the GPU is done with goes back at the
# frame's end. Eden frees them slowly - one size class a frame, 16 buffers at most, so a buffer
# can stay 64 frames after its last use - and a course loading in Mario Kart World stages its
# textures a layer at a time: the buffers it left behind held the pool its next request needed.
adapt('src/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp', 'vulkan_staging_buffer_pool.cpp', [
    ('#include <algorithm>', '#include <algorithm>\n#include "performance.h"'),
    ('    ReleaseCache(MemoryUsage::Download);\n}',
     '    ReleaseCache(MemoryUsage::Download);\n'
     '    if (::Eden::Performance::GraphicsMemoryIsShort()) {\n'
     '        for (auto* cache : {&device_local_cache, &upload_cache, &download_cache}) {\n'
     '            for (auto& level : *cache) {\n'
     '                std::erase_if(level.entries, [this](const StagingBuffer& entry) {\n'
     '                    return !entry.deferred && scheduler.IsFree(entry.tick);\n'
     '                });\n'
     '                level.delete_index = 0;\n'
     '                level.iterate_index = 0;\n'
     '            }\n'
     '        }\n'
     '    }\n'
     '}'),
])
# The software blitter keeps its scratch at the largest blit it has done: three 64 MiB buffers in
# Mario Kart World, held for the session in the pool the guest and the GPU share. Past 16 MiB they
# go back once the blit has written its result (the scoped guest write ends with its block).
# The blitter's large scratch buffers are released by upstream itself (ReleaseLargeBuffers, since Lichii):
# the derived copy is the upstream file.
adapt('src/video_core/engines/sw_blitter/blitter.cpp', 'sw_blitter.cpp', [])
adapt('src/video_core/renderer_vulkan/vk_scheduler.cpp', 'vulkan_scheduler.cpp', [
    ('#include <memory>', '#include <memory>\n#include <array>\n#include <atomic>\n#include <chrono>\n#include <cstdio>\n#include "dev_vulkan.h"\n#include "performance.h"'),
    ('namespace Vulkan {\n', 'namespace Vulkan {\n' + GPU_TIME_HELPERS),
    ('        .pInheritanceInfo = nullptr,\n    });\n    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());',
     '        .pInheritanceInfo = nullptr,\n    });\n    GpuTimeBegin(device, current_cmdbuf);\n'
     '    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());'),
    ('        upload_cmdbuf.End();\n        if (trace_gpu_recording) {',
     '        upload_cmdbuf.End();\n        GpuTimeEnd(cmdbuf);\n        if (trace_gpu_recording) {'),
    ('            trace_frame_end = 0;\n        }\n    });\n    chunk->MarkSubmit();',
     '            trace_frame_end = 0;\n        }\n        GpuTimeCollect(device);\n    });\n    chunk->MarkSubmit();'),
    ('    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore);\n'
     '    AllocateNewContext();\n    last_flush = std::chrono::steady_clock::now();',
     '    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore);\n'
     '    if (::Eden::DevVulkan::sync_submissions) {\n        Wait(signal_value);\n    }\n'
     '    AllocateNewContext();\n    last_flush = std::chrono::steady_clock::now();'),
])
adapt('src/video_core/renderer_vulkan/pipeline_helper.h',
      'include/video_core/renderer_vulkan/pipeline_helper.h', [
    ('            VkDescriptorGetInfoEXT get_info{',
     '            const VkDescriptorAddressInfoEXT* const buffer_info{\n'
     '                entry.address.address != 0 ? &address_info : nullptr};\n'
     '            VkDescriptorGetInfoEXT get_info{'),
    ('get_info.data.pUniformBuffer = &address_info;', 'get_info.data.pUniformBuffer = buffer_info;'),
    ('get_info.data.pStorageBuffer = &address_info;', 'get_info.data.pStorageBuffer = buffer_info;'),
    ('get_info.data.pUniformTexelBuffer = &address_info;', 'get_info.data.pUniformTexelBuffer = buffer_info;'),
    ('get_info.data.pStorageTexelBuffer = &address_info;', 'get_info.data.pStorageTexelBuffer = buffer_info;'),
])
adapt('src/video_core/renderer_vulkan/vk_blit_screen.h',
      'include/video_core/renderer_vulkan/vk_blit_screen.h', [
    ('VkFormat current_swapchain_view_format);',
     'VkFormat current_swapchain_view_format, bool draw_hud = false);'),
])
adapt('src/video_core/renderer_vulkan/renderer_vulkan.h',
      'include/video_core/renderer_vulkan/renderer_vulkan.h', [
    ('    void Composite(', '    void PresentLoading();\n    void Composite('),
    ('#ifdef HAS_LSFG\n#include "video_core/renderer_vulkan/present/frame_gen.h"\n#endif',
     '#ifdef HAS_LSFG\n#include "video_core/renderer_vulkan/present/frame_gen.h"\n#endif\n'
     '#ifdef HAS_MIHAWKGEN\n#include "mihawkgen.h"\n#include "mihawkgen_bridge.h"\n#include "video_core/nx2_perf.h"\n#endif'),
    ('#ifdef HAS_LSFG\n    FrameGen frame_gen;\n#endif',
     '#ifdef HAS_LSFG\n    FrameGen frame_gen;\n#endif\n'
     '#ifdef HAS_MIHAWKGEN\n    Eden::MihawkGen mihawkgen_;\n    bool mihawkgen_init_{};\n#endif'),
])
adapt('src/video_core/renderer_vulkan/renderer_vulkan.cpp', 'vulkan_renderer.cpp', [
    ('#include "video_core/renderer_vulkan/renderer_vulkan.h"',
     '#include "video_core/renderer_vulkan/renderer_vulkan.h"\n#include "display_refresh.h"\n'
     '#include "diagnostics.h"\n#include "hud.h"'),
    # The overlay's resolution piece (hud.h): the guest's own picture, taken as it is handed to the
    # display. The OpenGL renderer does not say, so the piece stays off there (vulkan_loading.inc
    # does the same for the loading screen).
    ('void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {',
     'void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {\n'
     + (port / 'vulkan_guest_resolution.inc').read_text()),
    # The HUD counted the guest rate only: OnFrameDisplayed() runs once per Composite(), while the
    # frames frame generation adds are presented through PresentManager::Present() (hud.h). Count
    # every frame handed to the present manager.
    ('        present_manager.Present(generated);\n    }',
     '        present_manager.Present(generated);\n'
     '        Eden::CountPresentedFrame();\n'
     '    }'),
    # A frame the display has no refresh for is not presented: a game patched for more frames
    # than the output shows keeps its pace instead of waiting for a refresh per frame
    # (headless/display_refresh.h). The frame still ends as any other.
    ('    RenderScreenshot(framebuffers);\n    const auto acquiring',  # Lichii: timed acquire
     '    RenderScreenshot(framebuffers);\n'
     '    if (::Eden::Display::SkipFrame()) {\n'
     '        gpu.RendererFrameEndNotify();\n'
     '        rasterizer.TickFrame();\n'
     '        return;\n'
     '    }\n'
     '    const auto acquiring'),
    ('    Report();', '    PresentLoading();\n    Report();'),
    ('swapchain.GetImageViewFormat());', 'swapchain.GetImageViewFormat(), true);'),
    ('layout, 1, format);', 'layout, 1, format, true);'),
    ('void RendererVulkan::Report() const {',
     (port / 'vulkan_loading.inc').read_text() + '\nvoid RendererVulkan::Report() const {'),
    # Present() only records the hand-off into a fresh chunk. Dispatch it now so
    # the present thread gets this frame immediately instead of after the
    # producer's next 8-draw dispatch (upstream does this only with LSFG).
    ('    present_manager.Present(frame);\n    const auto presented',  # Lichii: timed present
     '    present_manager.Present(frame);\n    Eden::CountPresentedFrame();\n    const auto presented'),
    ('                                 : std::chrono::steady_clock::time_point{};\n#ifdef HAS_LSFG\n    scheduler.DispatchWork();\n#endif',
     '                                 : std::chrono::steady_clock::time_point{};\n    scheduler.DispatchWork();'),
    # MihawkGen neural frame generation: skip the LSFG block when MihawkGen is active,
    # and run MihawkGen's own present path instead (headless/vulkan_mihawkgen.inc).
    ('#ifdef HAS_LSFG\n    void(frame_gen.WantedGenerations',
     '#ifdef HAS_LSFG\n'
     '#ifndef HAS_MIHAWKGEN\n'
     '#define EDEN_LSFG_GATE true\n'
     '#else\n'
     '#define EDEN_LSFG_GATE (!Eden::MihawkGenBridge::active.load(std::memory_order_relaxed))\n'
     '#endif\n'
     '    if (EDEN_LSFG_GATE) {\n'
     '    void(frame_gen.WantedGenerations'),
    ('        Eden::CountPresentedFrame();\n    }\n#endif',
     '        Eden::CountPresentedFrame();\n    }\n    } // LSFG gate\n#endif\n'
     + (port / 'vulkan_mihawkgen.inc').read_text()),
    ('} // namespace Vulkan', '''} // namespace Vulkan
namespace Eden {
void PresentVulkanLoading(VideoCore::RendererBase& renderer) {
    static_cast<Vulkan::RendererVulkan&>(renderer).PresentLoading();
}
}'''),
])
adapt('src/video_core/renderer_vulkan/present/window_adapt_pass.h',
      'include/video_core/renderer_vulkan/present/window_adapt_pass.h', [
    # The overlay and the loading screen are drawn in this pass when it is given the allocator
    # their picture's memory comes from (vulkan_hud_prepare.inc).
    ('class RasterizerVulkan;\n\nclass WindowAdaptPass final {',
     'class RasterizerVulkan;\nclass MemoryAllocator;\n\nclass WindowAdaptPass final {'),
    ('const Layout::FramebufferLayout& layout, Frame* dst);',
     'const Layout::FramebufferLayout& layout, Frame* dst, MemoryAllocator* hud_memory = nullptr);'),
    ('    vk::Pipeline coverage_pipeline;',
     '    vk::Pipeline coverage_pipeline;\n    vk::PipelineLayout hud_layout;\n    vk::Pipeline hud_pipeline;\n'
     '    vk::Image hud_art;\n    vk::ImageView hud_art_view;\n    vk::Sampler hud_sampler;\n'
     '    vk::DescriptorSetLayout hud_set_layout;\n    vk::DescriptorPool hud_pool;\n'
     '    vk::DescriptorSets hud_sets;'),
])
adapt('src/video_core/renderer_vulkan/present/window_adapt_pass.cpp', 'vulkan_window_adapt_pass.cpp', [
    ('#include "core/frontend/framebuffer_layout.h"',
     '#include "core/frontend/framebuffer_layout.h"\n#include "hud.h"\n#include "loading_art.h"\n'
     '#include "vulkan_hud_shaders.h"'),
    ('const Layout::FramebufferLayout& layout, Frame* dst) {',
     'const Layout::FramebufferLayout& layout, Frame* dst, MemoryAllocator* hud_memory) {'),
    ('    scheduler.Record([=](vk::CommandBuffer cmdbuf) {',
     (port / 'vulkan_hud_prepare.inc').read_text() + '\n    scheduler.Record([=](vk::CommandBuffer cmdbuf) {'),
    ('        cmdbuf.EndRenderPass();',
     (port / 'vulkan_hud_draw.inc').read_text() + '\n        cmdbuf.EndRenderPass();'),
])
adapt('src/video_core/renderer_vulkan/vk_blit_screen.cpp', 'vulkan_blit_screen.cpp', [
    ('VkFormat current_swapchain_view_format) {',
     'VkFormat current_swapchain_view_format, bool draw_hud) {'),
    ('image_index, layers, framebuffers, layout, frame);',
     'image_index, layers, framebuffers, layout, frame, draw_hud ? &memory_allocator : nullptr);'),
])
shader_header = '#pragma once\n#include <cstdint>\n'
for stage in ('vert', 'frag'):
    binary = output / f'vulkan_hud.{stage}.spv'
    subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.0',
                    '-o', str(binary), str(port / f'vulkan_hud.{stage}')], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(binary)], check=True)
    data = binary.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    shader_header += f'inline constexpr uint32_t EDEN_HUD_{stage.upper()}_SPV[] = {{\n'
    shader_header += ','.join(hex(word) for word in words) + '\n};\n'
path = output / 'vulkan_hud_shaders.h'
if not path.exists() or path.read_text() != shader_header:
    path.write_text(shader_header)
# The allocator's budget is the driver's, and on the console it does not say what is available: the
# driver counts each allocation of graphics memory twice (as VRAM and as visible VRAM), so the budget
# of the 8 GiB heap is used up at 4 GiB, and it knows nothing of the pool the CPU shares. Eden asks
# for images and buffers within the budget and takes a refusal as final (its Commit paths do not:
# they ask again outside the budget). A large open-world game at 2x ended with "ran out of graphics
# memory" while the pool had 0.75 GiB in one block and 1.5 GiB free. An image or a buffer the budget
# refuses is asked for again outside it, as Commit does; the driver refuses when the pool has no
# room, and that is still the session's end.
allocator = '#include <chrono>\n#include <thread>\n' + (source / 'src/video_core/vulkan_common/vulkan_memory_allocator.cpp').read_text()
# Fritchii v5.3 asks again over the budget for staging buffers only, without giving anything back: the
# recovery below does both for every buffer, so its block goes back to the plain call.
_v53_buffer = re.search(r'        VkResult result = vmaCreateBuffer\(allocator, &ci, &alloc_ci, &handle, &allocation,\n'
                        r' +&alloc_info\);\n(?:.*\n)*?        vk::Check\(result\);\n', allocator)
if _v53_buffer:
    allocator = allocator.replace(_v53_buffer[0], '        vk::Check(vmaCreateBuffer(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info));\n')
for call in ('vmaCreateImage(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info)',
             'vmaCreateBuffer(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info)',
             'vmaCreateBufferWithAlignment(allocator, &ci, &alloc_ci, min_alignment, &handle,\n'
             '                                           &allocation, &alloc_info)'):
    old = '    vk::Check(' + call + ');\n'
    # Eglantine asks again outside the budget for images itself (relaxed.flags): nothing to add.
    if 'Image' in call and allocator.count(old) == 0 and \
            'relaxed.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;' in allocator:
        continue
    if allocator.count(old) != 1:
        if LENIENT:
            SKIPPED.append(str(f'Pinned Vulkan allocator changed: {call[:28]} is there {allocator.count(old)} times')); print('VK-PORT-SKIP', f'Pinned Vulkan allocator changed: {call[:28]} is there {allocator.count(old)} times', file=sys.stderr)
            continue
        raise RuntimeError(f'Pinned Vulkan allocator changed: {call[:28]} is there {allocator.count(old)} times')
    allocator = allocator.replace(old, (
        '    VkResult created = CALL;\n'
        '    if (created != VK_SUCCESS) {\n'
        '        VmaAllocationCreateInfo outside_budget = alloc_ci;\n'
        '        outside_budget.flags &= ~static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT);\n'
        '        created = AGAIN;\n'
        '        const auto times = ::Eden::Performance::outside_budget.fetch_add(1, std::memory_order_relaxed) + 1;\n'
        '        if ((times & (times - 1)) == 0)  // the 1st, 2nd, 4th, 8th ... time\n'
        '            std::fprintf(stderr, "[ProsperoEden] graphics memory: the allocator\'s budget refused %llu requests; "\n'
        '                         "asked again outside it (result %d): DETAIL\\n", static_cast<unsigned long long>(times), int(created),\n'
        '                         VALUES);\n'
        # Refused outside the budget too: the GPU thread gives back what it can (performance.h,
        # graphics_memory_recover) and it is asked again, four times; a census of what holds the
        # memory is written the first time (vma-oom.json). Only then does the session end here.
        '        if (created != VK_SUCCESS) {\n'
        '            static std::atomic<bool> census_written{false};\n'
        '            if (!census_written.exchange(true)) {\n'
        '                char* json = nullptr;\n'
        '                vmaBuildStatsString(allocator, &json, VK_TRUE);\n'
        '                if (std::FILE* file = std::fopen("/data/prosperoeden/logs/vma-oom.json", "wb")) {\n'
        '                    std::fputs(json, file);\n'
        '                    std::fclose(file);\n'
        '                }\n'
        '                vmaFreeStatsString(allocator, json);\n'
        '            }\n'
        '        }\n'
        '        for (int attempt = 0; created != VK_SUCCESS && attempt < 156; ++attempt) {\n'
        '            const auto pack_now = ::Eden::Performance::guest_memory_pack_now.load(std::memory_order_acquire);\n'
        '            // The first attempt only frees graphics memory; then a few slots packed here (8 at most,\n'
        '            // ~5 ms each) and the scanner woken to pack more, for ~3 s before giving up (run fritchii-45\n'
        '            // ended at 6 attempts; packing 256 MiB here froze OOT ~1 s, fritchii-34, 2.3 s fritchii-44).\n'
        '            const bool packed = attempt > 0 && pack_now != nullptr && pack_now((attempt > 10 ? std::size_t{128} : std::size_t{32}) << 20) != 0;\n'
        '            const auto recover = ::Eden::Performance::graphics_memory_recover.load(std::memory_order_acquire);\n'
        '            if (!(recover != nullptr && recover(attempt)) && !packed) {\n'
        '                std::this_thread::sleep_for(std::chrono::milliseconds(20));\n'
        '            }\n'
        '            created = AGAIN;\n'
        '            if (created == VK_SUCCESS) {\n'
        '                std::fprintf(stderr, "[ProsperoEden] graphics memory: given after recovery %d: DETAIL\\n", attempt, VALUES);\n'
        '            }\n'
        '        }\n'
        '        if (created != VK_SUCCESS) {\n'
        '            VmaBudget held[VK_MAX_MEMORY_HEAPS]{};\n'
        '            vmaGetHeapBudgets(allocator, held);\n'
        '            unsigned long long blocks = 0, used = 0;\n'
        '            for (const VmaBudget& heap : held) {\n'
        '                blocks += heap.statistics.blockBytes;\n'
        '                used += heap.statistics.allocationBytes;\n'
        '            }\n'
        '            const auto free_block = ::Eden::Performance::graphics_memory_free.load(std::memory_order_relaxed);\n'
        '            std::fprintf(stderr, "[ProsperoEden] graphics memory: refused outside the budget too (result %d): the pool\'s "\n'
        '                         "largest free block is %llu MiB; the allocator holds %llu MiB in blocks, %llu MiB of it in use\\n",\n'
        '                         int(created), free_block ? free_block() >> 20 : 0ull, blocks >> 20, used >> 20);\n'
        '        }\n'
        '    }\n'
        '    vk::Check(created);\n'
        ).replace('CALL', call).replace('AGAIN', call.replace('&alloc_ci', '&outside_budget'))
        .replace('DETAIL', 'image %ux%ux%u, %u layers, %u levels, %u samples, format %d' if 'Image' in call
                 else 'buffer of %llu bytes, usage 0x%x')
        .replace('VALUES', 'ci.extent.width, ci.extent.height, ci.extent.depth, ci.arrayLayers, ci.mipLevels, '
                 'unsigned(ci.samples), int(ci.format)' if 'Image' in call
                 else 'static_cast<unsigned long long>(ci.size), unsigned(ci.usage)'))
# Out of the console's pool for an image too (the texture cache's images, which may fail and then
# went on without one): the guest's packed memory and the GPU thread's idle memory are given back
# (performance.h guest_memory_pack_now, graphics_memory_recover) and it is asked again, six times.
image_oom = ('    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge) {\n'
             '        pressure_events.fetch_add(1, std::memory_order_relaxed);\n    }\n')
if allocator.count(image_oom) == 1:
    allocator = allocator.replace(image_oom, (
        '    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge) {\n'
        '        static std::atomic<bool> census_written{false};\n'
        '        if (!census_written.exchange(true)) {\n'
        '            char* json = nullptr;\n'
        '            vmaBuildStatsString(allocator, &json, VK_TRUE);\n'
        '            if (std::FILE* file = std::fopen("/data/prosperoeden/logs/vma-oom-image.json", "wb")) {\n'
        '                std::fputs(json, file);\n'
        '                std::fclose(file);\n'
        '            }\n'
        '            vmaFreeStatsString(allocator, json);\n'
        '        }\n'
        '    }\n'
        '    for (int attempt = 0; result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge && attempt < 156; ++attempt) {\n'
        '        const auto pack_now = ::Eden::Performance::guest_memory_pack_now.load(std::memory_order_acquire);\n'
        '        const bool packed = attempt > 0 && pack_now != nullptr && pack_now((attempt > 10 ? std::size_t{128} : std::size_t{32}) << 20) != 0;\n'
        '        const auto recover = ::Eden::Performance::graphics_memory_recover.load(std::memory_order_acquire);\n'
        '        if (!(recover != nullptr && recover(attempt)) && !packed) {\n'
        '            std::this_thread::sleep_for(std::chrono::milliseconds(20));\n'
        '        }\n'
        '        VmaAllocationCreateInfo again = alloc_ci;\n'
        '        again.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;\n'
        '        result = vmaCreateImage(allocator, &ci, &again, &handle, &allocation, &alloc_info);\n'
        '        if (result == VK_SUCCESS) {\n'
        '            std::fprintf(stderr, "[ProsperoEden] graphics memory: image %ux%ux%u given after recovery %d\\n",\n'
        '                         ci.extent.width, ci.extent.height, ci.extent.depth, attempt);\n'
        '        }\n'
        '    }\n') + image_oom)
else:
    SKIPPED.append('allocator image recovery'); print('VK-PORT-SKIP allocator image recovery', file=sys.stderr)
# A block-compressed image whose mip levels are uneven in 4x4 blocks (HasUnevenBlockMips in the
# texture cache) is read past its end by the console's texture unit: the next page was unmapped and the
# GPU faulted (OOT on the Pro, "Unmapped page access" from TCP at a page boundary, twice with these
# ASTC textures as BC3; as RGBA8, which Lichii keeps them as, they did not fit the pool). Such images get
# their memory with room after them: the reads land in it.
uneven_anchor = ('    const u64 texels = u64{ci.extent.width} * ci.extent.height * ci.extent.depth * ci.arrayLayers;\n'
                 '    const bool huge = texels > MaxBackedImageTexels;\n')
uneven_fn_anchor = 'VkResult MemoryAllocator::AllocateImage('
if allocator.count(uneven_anchor) == 1 and allocator.count(uneven_fn_anchor) == 1:
    allocator = allocator.replace(uneven_fn_anchor, r"""namespace {
// A 2D block-compressed image whose level sizes in 4x4 blocks are not its level 0 count shifted down.
bool EdenUnevenBlockCompressed(const VkImageCreateInfo& ci) {
    if (ci.imageType != VK_IMAGE_TYPE_2D || ci.mipLevels <= 1 || ci.format < VK_FORMAT_BC1_RGB_UNORM_BLOCK ||
        ci.format > VK_FORMAT_BC7_SRGB_BLOCK) {
        return false;
    }
    const u32 blocks_x = (ci.extent.width + 3) / 4, blocks_y = (ci.extent.height + 3) / 4;
    for (u32 level = 1; level < ci.mipLevels; ++level) {
        const u32 level_x = ((std::max)(ci.extent.width >> level, 1U) + 3) / 4;
        const u32 level_y = ((std::max)(ci.extent.height >> level, 1U) + 3) / 4;
        if (level_x != (std::max)(blocks_x >> level, 1U) || level_y != (std::max)(blocks_y >> level, 1U)) {
            return true;
        }
    }
    return false;
}
} // Anonymous namespace

""" + uneven_fn_anchor, 1)
    allocator = allocator.replace(uneven_anchor, uneven_anchor + r"""    if (!huge && EdenUnevenBlockCompressed(ci)) {
        const auto& dld = device.GetDispatchLoader();
        VkImage padded{};
        if (dld.vkCreateImage(*device.GetLogical(), &ci, nullptr, &padded) == VK_SUCCESS) {
            VkMemoryRequirements requirements{};
            dld.vkGetImageMemoryRequirements(*device.GetLogical(), padded, &requirements);
            const VkDeviceSize room = (std::min)(VkDeviceSize{1} << 20, (std::max)(VkDeviceSize{256} << 10, requirements.size / 4));
            requirements.size += room;
            VmaAllocationCreateInfo padded_ci = alloc_ci;
            padded_ci.flags &= ~static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT);
            VmaAllocation padded_allocation{};
            VmaAllocationInfo padded_info{};
            if (vmaAllocateMemory(allocator, &requirements, &padded_ci, &padded_allocation, &padded_info) == VK_SUCCESS &&
                vmaBindImageMemory(allocator, padded_allocation, padded) == VK_SUCCESS) {
                static std::atomic<u32> padded_images{};
                if (padded_images.fetch_add(1, std::memory_order_relaxed) < 8) {
                    LOG_INFO(Render_Vulkan, "Image {}x{} ({} levels, {} layers, format {}): uneven block mips, {} KiB of room after it",
                             ci.extent.width, ci.extent.height, ci.mipLevels, ci.arrayLayers, static_cast<u32>(ci.format),
                             room >> 10);
                }
                image = vk::Image(padded, ci.usage, *device.GetLogical(), allocator, padded_allocation,
                                  device.GetDispatchLoader());
                return VK_SUCCESS;
            }
            if (padded_allocation != VK_NULL_HANDLE) vmaFreeMemory(allocator, padded_allocation);
            dld.vkDestroyImage(*device.GetLogical(), padded, nullptr);
        }
    }
""")
else:
    raise RuntimeError('Pinned AllocateImage changed: uneven block-compressed room')
# What the allocator holds in its blocks, for the FPS overlay (performance.h, graphics_memory_held):
# asked while there is an allocator, a sum of counters it keeps.
held_ctor = ('MemoryAllocator::MemoryAllocator(const Device &device_)\n', 'MemoryAllocator::~MemoryAllocator() = default;\n',
             '                    device_.GetPhysical().GetProperties().limits.bufferImageGranularity} {\n')
held_ok = all(allocator.count(anchor) == 1 for anchor in held_ctor)
if not held_ok:
    if not LENIENT:
        raise RuntimeError('Pinned Vulkan allocator changed: held counters')
    SKIPPED.append('allocator held counters'); print('VK-PORT-SKIP allocator held counters', file=sys.stderr)
allocator = allocator.replace(held_ctor[0], (
    'namespace {\n'
    'std::atomic<VmaAllocator> overlay_allocator{nullptr};\n'
    'unsigned long long GraphicsMemoryHeld() {\n'
    '    const VmaAllocator allocator = overlay_allocator.load(std::memory_order_acquire);\n'
    '    if (allocator == nullptr) return 0;\n'
    '    VmaBudget held[VK_MAX_MEMORY_HEAPS]{};\n'
    '    vmaGetHeapBudgets(allocator, held);\n'
    '    unsigned long long blocks = 0;\n'
    '    for (const VmaBudget& heap : held) blocks += heap.statistics.blockBytes;\n'
    '    return blocks;\n'
    '}\n'
    '} // namespace\n\n') + held_ctor[0])
allocator = allocator.replace(held_ctor[2], held_ctor[2] + (
    '    overlay_allocator.store(allocator, std::memory_order_release);\n'
    '    ::Eden::Performance::graphics_memory_held.store(&GraphicsMemoryHeld, std::memory_order_release);\n'))
allocator = allocator.replace(held_ctor[1], (
    'MemoryAllocator::~MemoryAllocator() {\n'
    '    ::Eden::Performance::graphics_memory_held.store(nullptr, std::memory_order_release);\n'
    '    overlay_allocator.store(nullptr, std::memory_order_release);\n'
    '}\n'))
# Development census: when the console's pool first turns critical (and twice more), VMA's
# detailed map - every allocation with its size and kind, image or buffer - goes to the logs, to
# say what holds graphics memory as a course loads (tools: a JSON per dump).
vma_budgets = ('                 budgets[heap].statistics.blockBytes >> 20);\n    }\n')
if allocator.count(vma_budgets) != 1:
    if not LENIENT:
        raise RuntimeError('Pinned Vulkan allocator changed: LogMemoryStatistics budgets')
    SKIPPED.append('allocator budget log'); print('VK-PORT-SKIP allocator budget log', file=sys.stderr)
allocator = allocator.replace(vma_budgets, vma_budgets + (
    '    static int vma_dumps = 0;\n'
    '    if (vma_dumps < 3 && ::Eden::Performance::GraphicsMemoryIsCritical()) {\n'
    '        char* json = nullptr;\n'
    '        vmaBuildStatsString(allocator, &json, VK_TRUE);\n'
    '        char path[96];\n'
    '        std::snprintf(path, sizeof(path), "/data/prosperoeden/logs/vma-%d.json", vma_dumps++);\n'
    '        if (std::FILE* file = std::fopen(path, "wb")) {\n'
    '            std::fputs(json, file);\n'
    '            std::fclose(file);\n'
    '        }\n'
    '        vmaFreeStatsString(allocator, json);\n'
    '    }\n'))
include = '#include "video_core/vulkan_common/vulkan_memory_allocator.h"\n'
if allocator.count(include) != 1:
    if not LENIENT:
        raise RuntimeError('Pinned Vulkan allocator changed: its own include')
    SKIPPED.append('allocator include'); print('VK-PORT-SKIP allocator include', file=sys.stderr)
allocator = allocator.replace(include, include + '#include <atomic>\n#include <cstdio>\n#include "performance.h"\n')
path = output / 'vulkan_memory_allocator.cpp'
if not path.exists() or path.read_text() != allocator:
    path.write_text(allocator)
adapt('src/video_core/vulkan_common/vulkan.h', 'include/video_core/vulkan_common/vulkan.h',
      [('#else\n#define VK_USE_PLATFORM_XLIB_KHR', '#elif defined(PS5_NATIVE)\n// Native display-plane presentation, no desktop window-system headers.\n#else\n#define VK_USE_PLATFORM_XLIB_KHR')])
adapt('src/video_core/vulkan_common/vulkan_instance.cpp', 'vulkan_instance.cpp', [
    ('namespace Vulkan {', 'extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);\n\nnamespace Vulkan {'),
    ('    case Core::Frontend::WindowSystemType::Headless:', '    case Core::Frontend::WindowSystemType::PS5:\n        extensions.push_back(VK_KHR_DISPLAY_EXTENSION_NAME);\n        break;\n    case Core::Frontend::WindowSystemType::Headless:'),
    ('#else\n    case Core::Frontend::WindowSystemType::X11:', '#elif !defined(PS5_NATIVE)\n    case Core::Frontend::WindowSystemType::X11:'),
    ('''    if (!library.IsOpen()) {
        LOG_ERROR(Render_Vulkan, "Vulkan library not available");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!library.GetSymbol("vkGetInstanceProcAddr", &dld.vkGetInstanceProcAddr)) {
        LOG_ERROR(Render_Vulkan, "vkGetInstanceProcAddr not present in Vulkan");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }''', '    (void)library;\n    dld.vkGetInstanceProcAddr = &::vkGetInstanceProcAddr;'),
])
adapt('src/video_core/vulkan_common/vulkan_library.cpp', 'vulkan_library.cpp', [
    ('#if defined(__ANDROID__) && defined(ARCHITECTURE_arm64)', '#if defined(PS5_NATIVE)\n    return std::make_shared<Common::DynamicLibrary>();\n#elif defined(__ANDROID__) && defined(ARCHITECTURE_arm64)'),
])
adapt('src/video_core/vulkan_common/vulkan_wrapper.h', 'include/video_core/vulkan_common/vulkan_wrapper.h', [
    ('#include <array>', '#include <array>\n#include <cstdio>\n#include "performance.h"'),
    ('return dld->vkQueueSubmit(queue,', 'auto timer = Eden::Performance::VulkanTimer(2);\n        return dld->vkQueueSubmit(queue,'),
    ('return dld->vkQueueSubmit2(queue,', 'auto timer = Eden::Performance::VulkanTimer(2);\n        return dld->vkQueueSubmit2(queue,'),
    ('return dld->vkWaitForFences(owner,', 'auto timer = Eden::Performance::VulkanTimer(3);\n        return dld->vkWaitForFences(owner,'),
    ('const VkResult result = dld->vkWaitSemaphores(owner,', 'auto timer = Eden::Performance::VulkanTimer(4);\n        const VkResult result = dld->vkWaitSemaphores(owner,'),
    ('return dld->vkDeviceWaitIdle(handle);', 'auto timer = Eden::Performance::VulkanTimer(5);\n        return dld->vkDeviceWaitIdle(handle);'),
    ('''inline void Check(VkResult result) {
    if (result != VK_SUCCESS) {
        throw Exception(result);''', '''inline void Check(VkResult result, const char* file = __builtin_FILE(),
                  int line = __builtin_LINE(), const char* operation = __builtin_FUNCTION()) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "[ProsperoEden] Vulkan failure: %s:%d (%s) VkResult=%d\\n",
                     file, line, operation, static_cast<int>(result));
        throw Exception(result);'''),
])
adapt('src/video_core/vulkan_common/vulkan_wrapper.cpp', 'vulkan_wrapper.cpp', [
    ('#include "video_core/vulkan_common/vulkan_wrapper.h"', '#include "video_core/vulkan_common/vulkan_wrapper.h"\n#include "performance.h"'),
    ('auto const result = dld->vkCreateGraphicsPipelines(', 'auto timer = Eden::Performance::VulkanTimer(0);\n    auto const result = dld->vkCreateGraphicsPipelines('),
    ('Check(dld->vkCreateComputePipelines(', 'auto timer = Eden::Performance::VulkanTimer(1);\n    Check(dld->vkCreateComputePipelines('),
])
try:
    download_source = (source / 'src/video_core/texture_cache/texture_cache.h').read_text()
    download_begin = download_source.index('void TextureCache<P>::DownloadMemory(DAddr cpu_addr, size_t size)')
    download_loop = download_source.index('    for (const ImageId image_id : images) {', download_begin)
    download_prefix = download_source[download_begin:download_loop]
    # Reuse DeleteImage's exact accounting to stop lookahead when its existing
    # pressure policy changes. Fail on source drift rather than inventing a budget.
    delete_begin = download_source.index('    if (image.HasScaled()) {', download_source.index('void TextureCache<P>::DeleteImage('))
    delete_end = download_source.index('    const GPUVAddr gpu_addr', delete_begin)
    reclaimed_bytes = download_source[delete_begin:delete_end].replace('total_used_memory -=', 'bytes +=').replace(
        'slot_images[image_id]', 'image')
    gc_begin = download_source.index('void TextureCache<P>::RunGarbageCollector() {')
    gc_end = download_source.index('\ntemplate <class P>', gc_begin)
    gc_original = download_source[gc_begin:gc_end]
    gc = gc_original
    gc_setup = '''    const auto ReclaimedBytes = [&](const Image& image) {
            u64 bytes = 0;
    ''' + reclaimed_bytes + '''        return bytes;
        };
    ''' + (port / 'vulkan_gc_downloads.inc').read_text()
    for old, new in [
        ('    const auto Cleanup = [this, &num_iterations, &high_priority_mode, &aggressive_mode,\n'
         '                          &evict_old_costly](ImageId image_id) {',
         gc_setup + '\n    const auto Cleanup = [&, this](ImageId image_id) {'),
        ('        if (must_download) {',
         '        if (must_download && KeepLargeDirty(image)) {\n            return false;\n        }\n'
         '        if (must_download && !TakePrefetched(image_id, image)) {'),
        # Time the per-image drain Cleanup still takes for an image the prefetch could not batch.
        # One GPU pipeline wait per eviction, against one wait for a whole batch in the batched path:
        # this is the stall the prefetch exists to remove, so keep it visible.
        ('                image.DownloadMemory(map, copies);\n                runtime.Finish();\n'
         '                SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, map.mapped_span, swizzle_data_buffer);',
         '                image.DownloadMemory(map, copies);\n'
         '                {\n'
         '                    const ::Eden::Performance::Timer drain_timer(\n'
         '                        ::Eden::Performance::gc_unbatched_drain, 1);\n'
         '                    runtime.Finish();\n'
         '                }\n'
         '                SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, map.mapped_span, swizzle_data_buffer);'),
        ('(!high_priority_mode && must_download)) {', '(!DirtyEvictions() && must_download)) {'),
        ('    Configure(false);', '    Configure(false);\n    PrefetchDownloads();'),
        ('        Configure(true);', '        Configure(true);\n        PrefetchDownloads();'),
        # The eviction budget counts visits, not evictions: skipped images (decoding,
        # costly, or dirty images kept below the critical mark) burn it and stop the
        # scan, so kept images shield everything behind them. Count evictions; bound
        # the scan separately. gc_scan=upstream restores visit counting.
        ('    size_t num_iterations = 0;',
         '    size_t num_iterations = 0;\n    size_t num_scanned = 0;\n    constexpr size_t max_scanned = 8192;'),
        ('        if (num_iterations == 0) {\n            return true;\n        }\n        --num_iterations;',
         '        if (num_iterations == 0 || num_scanned >= max_scanned) {\n            return true;\n        }\n        ++num_scanned;\n        if (::Eden::Performance::gc_count_visits.load(std::memory_order_relaxed)) {\n            --num_iterations;\n        }'),
        # A costly image (ASTC kept as BC3, converted on the CPU) goes only past Eden's own critical
        # mark, not when the console's pool alone is critical: rebuilding it decodes it again on the
        # CPU, in memory the pool did not have (Mario Kart World: 1,561 evicted in one window, 3.4 GB
        # of conversions and uploads in the next, and the decoder's allocation ended the session).
        ('        if ((!aggressive_mode && !evict_old_costly && True(image.flags & ImageFlagBits::CostlyLoad)) ||\n            (!DirtyEvictions() && must_download)) {\n            return false;\n        }',
         '        if ((!(aggressive_mode && total_used_memory >= critical_memory) && !::Eden::Performance::gc_old_costly && !evict_old_costly && True(image.flags & ImageFlagBits::CostlyLoad)) || (!DirtyEvictions() && must_download)) {\n            return false;\n        }\n        if (!::Eden::Performance::gc_count_visits.load(std::memory_order_relaxed)) {\n            --num_iterations;\n        }'),
        # The aggressive pass gets its own scan allowance: Configure resets the
        # eviction budget per pass, and the scan counter must reset with it, or a
        # first pass spent on skips would stillborn the second one. Runs after the
        # Configure(true) pair above, whose first line this matches.
        ('        Configure(true);',
         '        Configure(true);\n        num_scanned = 0;'),
        # While the console's pool is short (performance.h, GraphicsMemoryIsShort) the collector
        # works as if past its first mark, past both once the pool is critical (GraphicsMemoryIsCritical),
        # and stays so for the whole run instead of easing off after one eviction under a mark the
        # pool never reaches.
        ('    u64 ticks_to_destroy = 0;\n',
         '    u64 ticks_to_destroy = 0;\n    const bool memory_short = ::Eden::Performance::GraphicsMemoryIsShort();\n'),
        ('        num_iterations = aggressive_mode ? 40 : (high_priority_mode ? 20 : 10);\n',
         '        if (memory_short) {\n'
         '            // Short but not critical, only what 600 frames left unused goes (EDEN_NX2_TC_SHORT_AGE):\n'
         '            // at 60, and 10 whenever use passed the texture cache\'s own critical mark, a race\n'
         '            // evicted and rebuilt 1,000-2,400 textures every 600 frames, many still in use, while\n'
         '            // the pool stayed short throughout (an image freed inside a graphics block does not\n'
         '            // give the pool a larger free block). On the console the pool decides: critical\n'
         '            // (GraphicsMemoryIsCritical) collects as before.\n'
         '            static const u64 short_age = [] {\n'
         '                const char* value = std::getenv("EDEN_NX2_TC_SHORT_AGE");\n'
         '                return value != nullptr ? std::strtoull(value, nullptr, 10) : 600ULL;\n'
         '            }();\n'
         '            high_priority_mode = true;\n'
         '            aggressive_mode = allow_aggressive && ::Eden::Performance::GraphicsMemoryIsCritical();\n'
         '            ticks_to_destroy = (std::max)(aggressive_mode ? u64{10} : short_age, min_age);\n'
         '        }\n'
         '        num_iterations = aggressive_mode ? 40 : (high_priority_mode ? 20 : 10);\n'),
        ('        if (aggressive_mode && total_used_memory < critical_memory) {',
         '        if (aggressive_mode && !memory_short && total_used_memory < critical_memory) {'),
        ('        } else if (high_priority_mode && total_used_memory < expected_memory) {',
         '        } else if (high_priority_mode && !memory_short && total_used_memory < expected_memory) {'),
        ('    if (total_used_memory >= critical_memory) {\n        Configure(true);',
         '    if (total_used_memory >= critical_memory || memory_short) {\n        Configure(true);'),    # While the pool is short, costly images 300 frames unused (a course left behind) go too, in a
        # pass of their own: kept whenever only the pool was short, Mario Kart World's next course took
        # 1.2 GB of graphics memory within five seconds with nothing the collector could give back, and
        # waiting until the pool was critical left a course load too little room (pro27: 1.37 GB usable,
        # then out of memory within one window).
        ('    if (total_used_memory >= critical_memory || memory_short) {\n        Configure(true);\n        num_scanned = 0;\n        PrefetchDownloads();\n        Collect();\n    }\n',
         '    if (total_used_memory >= critical_memory || memory_short) {\n        Configure(true);\n        num_scanned = 0;\n        PrefetchDownloads();\n        Collect();\n    }\n'
         '    // On a light frame (a loading screen: astc_policy.h) the course left behind is unused\n'
         '    // after 10 frames already, and the next one is about to load.\n'
         '    const u64 short_old_age = ::Eden::AstcPolicy::InBackground() ? 300 : 10;\n'
         '    if (memory_short && frame_tick > short_old_age) {\n'
         '        Configure(true);\n'
         '        num_scanned = 0;\n'
         '        ::Eden::Performance::gc_old_costly = true;\n'
         '        PrefetchDownloads();\n'
         '        lru_cache.ForEachItemBelow(frame_tick - short_old_age, Cleanup);\n'
         '        ::Eden::Performance::gc_old_costly = false;\n'
         '    }\n'),
    ]:
        if gc.count(old) != 1:
            raise RuntimeError(f'Pinned GC anchor changed: {old}')
        gc = gc.replace(old, new)
    # Every pass walks its candidates in block order (vulkan_gc_downloads.inc, ForEachCandidate).
    for below in ('short_old_age',):
        walk = f'lru_cache.ForEachItemBelow(frame_tick - {below}, Cleanup)'
        if walk not in gc:
            raise RuntimeError(f'Pinned GC anchor changed: {walk}')
        gc = gc.replace(walk, f'ForEachCandidate(frame_tick - {below}, Cleanup)', 1)
except (ValueError, RuntimeError) as gc_drift:
    if not LENIENT:
        raise
    print(f'VK-PORT-SKIP texture collector rewrite (Lichii keeps its own): {gc_drift}', file=sys.stderr)
    SKIPPED.append('texture collector rewrite')
    download_prefix = gc_original = gc = ''
texture_costs = []
for signature, index in (
    ('RunGarbageCollector(bool out_of_memory)', 13), ('DownloadMemory(DAddr cpu_addr, size_t size)', 14),
    ('PopAsyncFlushes()', 15),
):
    anchor = f'void TextureCache<P>::{signature} {{'
    scope = f'''
    std::optional<Eden::Performance::Timer> cost_timer;
    if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {{
        if (Eden::Performance::vulkan_cost_enabled.load(std::memory_order_relaxed))
            cost_timer.emplace(Eden::Performance::vulkan_api[{index}]);
    }}
'''
    if index == 13:
        scope += '''    if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {
        if ((cost_timer || Eden::Performance::texture_budget_log.load(std::memory_order_relaxed)) &&
            frame_tick % 300 == 0)
            std::printf("EDEN_VULKAN_TEXTURE_BUDGET frame=%llu usage=%llu expected=%llu critical=%llu "
                        "keep_dirty=%d memory_short=%d\\n",
                        static_cast<unsigned long long>(frame_tick),
                        static_cast<unsigned long long>(total_used_memory),
                        static_cast<unsigned long long>(expected_memory),
                        static_cast<unsigned long long>(critical_memory),
                        int(Eden::Performance::gc_keep_dirty.load(std::memory_order_relaxed)),
                        int(Eden::Performance::graphics_memory_short.load(std::memory_order_relaxed)));
    }
'''
    texture_costs.append((anchor, anchor + scope))
# Large textures re-upload only the block rows guest writes changed (headless/texture_partial.h).
VULKAN_CACHE = 'if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {'
texture_partial = [
    # The page lists belong to this cache's images: a title started after another in the same
    # process (return to menu) must not see the previous cache's ImageId indices.
    ('    : runtime{runtime_}, device_memory{device_memory_} {\n',
     '    : runtime{runtime_}, device_memory{device_memory_} {\n'
     '    ' + VULKAN_CACHE + '\n'
     '        Eden::TexturePartial::images.clear();\n'
     '    }\n'),
    ('            MarkLayersWritten(image, cpu_addr, size);\n'
     '            return;\n'
     '        }\n'
     '        if (True(image.flags & ImageFlagBits::CpuModified)) {\n'
     '            return;\n'
     '        }\n'
     '        image.flags |= ImageFlagBits::CpuModified;',
     '            MarkLayersWritten(image, cpu_addr, size);\n'
     '            return;\n'
     '        }\n'
     '        ' + VULKAN_CACHE + '\n'
     '            if (Eden::TexturePartial::Write(device_memory, image, image_id.index, cpu_addr, size)) {\n'
     '                return;\n'
     '            }\n'
     '        }\n'
     '        if (True(image.flags & ImageFlagBits::CpuModified)) {\n'
     '            return;\n'
     '        }\n'
     '        image.flags |= ImageFlagBits::CpuModified;'),
    ('    ASSERT(True(image.flags & ImageFlagBits::Tracked));\n'
     '    image.flags &= ~ImageFlagBits::Tracked;',
     '    ASSERT(True(image.flags & ImageFlagBits::Tracked));\n'
     '    ' + VULKAN_CACHE + '\n'
     '        Eden::TexturePartial::End(device_memory, image_id.index);\n'
     '    }\n'
     '    image.flags &= ~ImageFlagBits::Tracked;'),
    ('    if (invalidate) {\n'
     '        image.flags &= ~(ImageFlagBits::CpuModified | ImageFlagBits::GpuModified);',
     '    if (invalidate) {\n'
     '        ' + VULKAN_CACHE + '\n'
     '            Eden::TexturePartial::End(device_memory, image_id.index);\n'
     '        }\n'
     '        image.flags &= ~(ImageFlagBits::CpuModified | ImageFlagBits::GpuModified);'),
    ('    ASSERT_MSG(False(image.flags & ImageFlagBits::Tracked), "Image was not untracked");',
     '    ASSERT_MSG(False(image.flags & ImageFlagBits::Tracked), "Image was not untracked");\n'
     '    ' + VULKAN_CACHE + '\n'
     '        Eden::TexturePartial::images.erase(image_id.index);\n'
     '    }'),
    ('    if (False(image.flags & ImageFlagBits::CpuModified)) {\n'
     '        // Only upload modified images\n'
     '        return;\n'
     '    }\n',
     '    if (False(image.flags & ImageFlagBits::CpuModified)) {\n'
     '        // Only upload modified images\n'
     '        return;\n'
     '    }\n'
     '    ' + VULKAN_CACHE + '\n' + (port / 'texture_partial_refresh.inc').read_text() + '    }\n'),
]
# A large texture array (2D, several layers, 64 MiB or more) the CPU unswizzles or converts is
# uploaded a layer at a time, and an ASTC one converted in the background is decoded a layer at a
# time: Eden staged it whole, a heap buffer and a staging buffer each the image's size (341 and
# 361 MiB for Mario Kart World's terrain arrays) asked of the pool the guest's memory and the GPU
# share, at the moment a course loads - the requests that ended its sessions. The bytes are the
# same; the largest block is one layer. Arrays the GPU decodes keep Eden's own layer path.
LAYERED = ('image.info.type == ImageType::e2D && image.info.resources.layers > 1 && '
           'image.guest_size_bytes >= LayerUploadMinSize')
texture_layers = [
    # Upstream traces uploads (EDEN_NX2_EXP7) around this point: the layer path goes right after
    # the trace's start and returns before it, like the chunked path keeps the trace.
    ('    const u64 faults_begin = upload_trace ? Common::GetProcessPageFaults() : 0;\n'
     '    if (!UploadInChunks(image)) {\n'
     '        auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));\n',
     '    const u64 faults_begin = upload_trace ? Common::GetProcessPageFaults() : 0;\n'
     '    ' + VULKAN_CACHE + '\n'
     '        if (' + LAYERED + ' &&\n'
     '            False(image.flags & (ImageFlagBits::AcceleratedUpload | ImageFlagBits::Sparse |\n'
     '                                 ImageFlagBits::Remapped))) {\n'
     '            const s32 layers = image.info.resources.layers;\n'
     '            ImageInfo layer_info = image.info;\n'
     '            layer_info.resources.layers = 1;\n'
     '            const size_t layer_map = MapSizeBytes(image) / layers;\n'
     '            for (s32 layer = 0; layer < layers; ++layer) {\n'
     '                const GPUVAddr layer_addr = image.gpu_addr + u64(layer) * image.info.layer_stride;\n'
     '                auto layer_staging = runtime.UploadStagingBuffer(layer_map);\n'
     '                Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::UnsafeRead> swizzle_data(\n'
     '                    *gpu_memory, layer_addr, image.info.layer_stride, &swizzle_data_buffer);\n'
     '                if (True(image.flags & ImageFlagBits::Converted)) {\n'
     '                    unswizzle_data_buffer.resize_destructive((std::max<size_t>)(\n'
     '                        image.unswizzled_size_bytes / layers, CalculateUnswizzledSizeBytes(layer_info)));\n'
     '                    auto copies = UnswizzleImage(*gpu_memory, layer_addr, layer_info, swizzle_data,\n'
     '                                                 unswizzle_data_buffer);\n'
     '                    // The layer\'s converted size, where it is above the estimate (as the decode).\n'
     '                    auto layout = copies;\n'
     '                    ConvertedLayout(layer_info, layout);\n'
     '                    auto converted_staging = layer_staging;\n'
     '                    if (!layout.empty() && layout.back().buffer_offset + layout.back().buffer_size > layer_map) {\n'
     '                        converted_staging = runtime.UploadStagingBuffer(layout.back().buffer_offset +\n'
     '                                                                        layout.back().buffer_size);\n'
     '                    }\n'
     '                    ConvertImage(unswizzle_data_buffer, layer_info, converted_staging.mapped_span, copies);\n'
     '                    for (auto& copy : copies) copy.image_subresource.base_layer = layer;\n'
     '                    image.UploadMemory(converted_staging, copies);\n'
     '                } else {\n'
     '                    auto copies = UnswizzleImage(*gpu_memory, layer_addr, layer_info, swizzle_data,\n'
     '                                                 layer_staging.mapped_span);\n'
     '                    for (auto& copy : copies) copy.image_subresource.base_layer = layer;\n'
     '                    image.UploadMemory(layer_staging, copies);\n'
     '                }\n'
     '            }\n'
     '            runtime.InsertUploadMemoryBarrier();\n'
     '            return;\n'
     '        }\n'
     '    }\n'
     '    if (!UploadInChunks(image)) {\n'
     '        auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));\n'),
    ('    image.flags |= ImageFlagBits::IsDecoding;\n'
     '    ++async_decodes_queued;\n'
     '    auto decode = std::make_unique<AsyncDecodeContext>();',
     '    ' + VULKAN_CACHE + '\n'
     '        if (' + LAYERED + ') {\n'
     '            image.flags |= ImageFlagBits::IsDecoding;\n'
     '            const s32 layers = image.info.resources.layers;\n'
     '            ImageInfo layer_info = image.info;\n'
     '            layer_info.resources.layers = 1;\n'
     '            const size_t layer_out = MapSizeBytes(image) / layers;\n'
     '            // A layer\'s own sizes, not a share of the whole image\'s estimates: a decode that\n'
     '            // wrote past its buffer corrupted the heap (TextureDecoder "mutex lock failed").\n'
     '            const size_t layer_in = (std::max<size_t>)(image.unswizzled_size_bytes / layers,\n'
     '                                                       CalculateUnswizzledSizeBytes(layer_info));\n'
     '            const bool zero = False(image.flags & ImageFlagBits::ZeroFilled);\n'
     '            image.flags |= ImageFlagBits::ZeroFilled;\n'
     '            for (s32 layer = 0; layer < layers; ++layer) {\n'
     '                ++async_decodes_queued;\n'
     '                auto layer_decode = std::make_unique<AsyncDecodeContext>();\n'
     '                auto* layer_ptr = layer_decode.get();\n'
     '                layer_decode->image_id = image_id;\n'
     '                async_decodes.push_back(std::move(layer_decode));\n'
     '                const GPUVAddr layer_addr = image.gpu_addr + u64(layer) * image.info.layer_stride;\n'
     '                std::vector<u8> layer_input(layer_in, 0);\n'
     '                Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::UnsafeRead> layer_swizzle(\n'
     '                    *gpu_memory, layer_addr, image.info.layer_stride, &swizzle_data_buffer);\n'
     '                auto layer_copies = UnswizzleImage(*gpu_memory, layer_addr, layer_info, layer_swizzle,\n'
     '                                                   layer_input);\n'
     '                auto zero_copies = layer_copies;\n'
     '                ConvertedLayout(layer_info, zero_copies);\n'
     '                const size_t out = zero_copies.empty() ? layer_out :\n'
     '                    (std::max<size_t>)(layer_out, zero_copies.back().buffer_offset + zero_copies.back().buffer_size);\n'
     '                if (out > layer_out) {\n'
     '                    static std::atomic<u32> reported{};\n'
     '                    if (reported.fetch_add(1) < 8) {\n'
     '                        LOG_WARNING(HW_GPU, "Layer decode at {:#x} needs {} bytes, {} estimated", layer_addr, out, layer_out);\n'
     '                    }\n'
     '                }\n'
     '                if (zero) {\n'
     '                    for (auto& copy : zero_copies) copy.image_subresource.base_layer = layer;\n'
     '                    auto zero_staging = runtime.UploadStagingBuffer(out);\n'
     '                    std::memset(zero_staging.mapped_span.data(), 0, out);\n'
     '                    image.UploadMemory(zero_staging, zero_copies);\n'
     '                }\n'
     '                auto layer_func = [layer_out = out, layer, copies = layer_copies, info = layer_info,\n'
     '                                   input = std::move(layer_input), async_decode = layer_ptr]() mutable {\n'
     '                    async_decode->decoded_data.resize_destructive(layer_out);\n'
     '                    std::span copies_span{copies.data(), copies.size()};\n'
     '                    ConvertImage(input, info, async_decode->decoded_data, copies_span);\n'
     '                    for (auto& copy : copies) copy.image_subresource.base_layer = layer;\n'
     '                    async_decode->copies = std::move(copies);\n'
     '                    async_decode->complete.store(true, std::memory_order_release);\n'
     '                };\n'
     '                texture_decode_worker.QueueWork(std::move(layer_func));\n'
     '            }\n'
     '            if (zero) runtime.InsertUploadMemoryBarrier();\n'
     '            return;\n'
     '        }\n'
     '    }\n'
     '    image.flags |= ImageFlagBits::IsDecoding;\n'
     '    ++async_decodes_queued;\n'
     '    auto decode = std::make_unique<AsyncDecodeContext>();'),
    # A layer's result is staged at its own size, and the image stays decoding until its last
    # layer has landed (the collector does not take an image that is decoding).
    ('        auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));\n'
     '        std::memcpy(staging.mapped_span.data(), async_decode->decoded_data.data(),',
     '        auto staging = runtime.UploadStagingBuffer(async_decode->decoded_data.size());\n'
     '        std::memcpy(staging.mapped_span.data(), async_decode->decoded_data.data(),'),
    ('        image.UploadMemory(staging, async_decode->copies);\n'
     '        image.flags &= ~ImageFlagBits::IsDecoding;',
     '        image.UploadMemory(staging, async_decode->copies);\n'
     '        if (std::none_of(async_decodes.begin(), async_decodes.end(), [&](const auto& other) {\n'
     '                return other.get() != async_decode && other->image_id == async_decode->image_id &&\n'
     '                       !other->cancelled;\n'
     '            }))\n'
     '            image.flags &= ~ImageFlagBits::IsDecoding;'),
    # The decode thread hands a result over with complete's release store, the GPU thread takes
    # it with an acquire load: no mutex per decode. Each one's std::mutex was created at its first
    # lock, a texture array's course load queues one per layer (121 for a terrain array), and in
    # 4 of ~24 races a decode thread's lock failed ("mutex lock failed: Invalid argument",
    # 325-328 s into a session, always the same point of the run).
    ('        // TODO: Do we need this lock?\n'
     '        std::unique_lock lock{async_decode->mutex};\n'
     '        async_decode->copies = std::move(copies);\n'
     '        async_decode->complete = true;\n',
     '        async_decode->copies = std::move(copies);\n'
     '        async_decode->complete.store(true, std::memory_order_release);\n'),
    ('        {\n'
     '            // Release the lock before the context is erased below: unlocking it afterwards wrote\n'
     '            // into freed memory (heap corruption, crashes minutes into title 1).\n'
     '            std::unique_lock lock{async_decode->mutex};\n'
     '            if (!async_decode->complete) {\n'
     '                ++i;\n'
     '                continue;\n'
     '            }\n'
     '        }\n',
     '        // After this the context is the GPU thread\'s alone: the decode thread\'s store was\n'
     '        // its last touch of it.\n'
     '        if (!async_decode->complete.load(std::memory_order_acquire)) {\n'
     '            ++i;\n'
     '            continue;\n'
     '        }\n'),
    # The 600-frame texture statistics at the memory images really hold (sparse images at what is
    # bound, ASTC at the size it is kept as), with the largest images and the formats in the log:
    # what fills the pool when the collector starts evicting textures still in use.
    ('            const u64 bytes = GetScaledImageSizeBytes(*image);\n',
     '            const u64 bytes = ImageMemoryBytes(*image) + (image->HasScaled() ? GetScaledImageSizeBytes(*image) : 0);\n'),
    ('            LOG_DEBUG(HW_GPU, "  {} MiB: {:#x} {}x{}x{} layers {} levels {} format {} type {} flags {:#x}",',
     '            LOG_INFO(HW_GPU, "  {} MiB: {:#x} {}x{}x{} layers {} levels {} format {} type {} flags {:#x}",'),
    ('            LOG_DEBUG(HW_GPU, "  format {}: {} MiB in {} images", formats[i].second, formats[i].first >> 20,',
     '            LOG_INFO(HW_GPU, "  format {}: {} MiB in {} images", formats[i].second, formats[i].first >> 20,'),
    # A whole-image decode too: its buffer at the converted layout's size where that is above
    # the estimate (TickAsyncDecode then drops the result with an error instead of the decode
    # writing past the buffer).
    ('    const size_t out_size = MapSizeBytes(image);\n',
     '    auto converted_layout = copies;\n'
     '    ConvertedLayout(image.info, converted_layout);\n'
     '    const size_t out_size = converted_layout.empty() ? MapSizeBytes(image) :\n'
     '        (std::max<size_t>)(MapSizeBytes(image), converted_layout.back().buffer_offset +\n'
     '                                                converted_layout.back().buffer_size);\n'),
]
adapt('src/video_core/texture_cache/texture_cache.h',
      'vulkan-cache/video_core/texture_cache/texture_cache.h', [
    ('namespace VideoCommon {', '#include "performance.h"\n#include "texture_partial.h"\n#include "image_pages.h"\n#include "astc_policy.h"\n#include "common/scope_exit.h"\nnamespace Vulkan { class TextureCacheRuntime; }\n\nnamespace VideoCommon {'),
    # An ASTC texture kept as BC3 is converted in the background in a busy frame only, and in
    # place on a light screen (headless/astc_policy.h). The frame is counted where the texture
    # cache ends its own.
    ('    if (True(image.flags & ImageFlagBits::AsynchronousDecode)) {\n        QueueAsyncDecode(image, image_id);\n        return;\n    }',
     '    if (True(image.flags & ImageFlagBits::AsynchronousDecode) && ::Eden::AstcPolicy::InBackground()) {\n        QueueAsyncDecode(image, image_id);\n        return;\n    }'),
    ('    TickAsyncDecode();\n    TickAsyncUnswizzle();', '    ::Eden::AstcPolicy::Frame();\n    TickAsyncDecode();\n    TickAsyncUnswizzle();'),
    # Every place an image is registered at is marked before the texture cache can find the
    # image there (headless/image_pages.h): the two inserts below are the only ones.
    ('        auto map_id =\n            slot_map_views.insert(image.gpu_addr, image.cpu_addr, image.guest_size_bytes, image_id);',
     '        ::Eden::ImagePages::Mark(image.cpu_addr, image.guest_size_bytes);\n'
     '        auto map_id =\n            slot_map_views.insert(image.gpu_addr, image.cpu_addr, image.guest_size_bytes, image_id);'),
    ('            auto map_id = slot_map_views.insert(gpu_addr, cpu_addr, size, image_id);',
     '            ::Eden::ImagePages::Mark(cpu_addr, size);\n'
     '            auto map_id = slot_map_views.insert(gpu_addr, cpu_addr, size, image_id);'),
] + ([(download_prefix, download_prefix + (port / 'vulkan_download_batch.inc').read_text() + '\n'),
      (gc_original, gc)] if gc_original else []) + [
    # RADV reports all Vulkan allocations, not just cached textures. A game's
    # measured ~1.9 GiB working set triggered dirty eviction at the old 1.6 GiB
    # threshold despite a 4 GiB budget. Retain 40% headroom; keep the original
    # critical threshold and minimum 1 GiB spacing. OpenGL remains unchanged.
    ('const s64 min_vacancy_expected = (6 * mem_threshold) / 10;',
     'const s64 min_vacancy_expected =\n            ((std::is_same_v<Runtime, Vulkan::TextureCacheRuntime> ? 4 : 6) * mem_threshold) / 10;'),
] + texture_partial + texture_costs + texture_layers)

# The collector also runs as out of memory while the console's pool is critical (performance.h,
# GraphicsMemoryIsCritical: under 1 GiB usable free; never on the PC). Lichii collects near the
# driver's device budget, which on the console is a 6 GB figure for the pool the guest and the
# heap also take: OOT's save load on the Pro held 3.5 GB of graphics with 5.8 GB of guest memory
# and 2.8 GB of heap, and the next 9 MB buffer found no block. Its own edit, so it stays when the
# edits above are left out (a lenient Lichii build keeps the upstream file).
texture_cache_path = output / 'vulkan-cache/video_core/texture_cache/texture_cache.h'
texture_cache_text = texture_cache_path.read_text()
pool_old = ('    if (out_of_memory || total_used_memory > minimum_memory) {\n'
            '        RunGarbageCollector(out_of_memory);\n    }\n')
if texture_cache_text.count(pool_old) == 1:
    texture_cache_text = texture_cache_text.replace(pool_old,
        '    const bool pool_critical = ::Eden::Performance::GraphicsMemoryIsCritical();\n'
        '    if (out_of_memory || pool_critical || total_used_memory > minimum_memory) {\n'
        '        RunGarbageCollector(out_of_memory || pool_critical);\n    }\n')
    # Collected that way, a huge GPU-written image is written back first: its size three times over
    # (swizzle buffer on the heap, staging, newly committed guest memory). OOT's 288 MiB 12288x12288
    # shadow map in Hyrule Field needed ~860 MiB with 0.8 GiB free and the guest stalled for minutes
    # (run fritchii-24). On the console such images stay; smaller ones are written back as before. So do
    # huge images that are costly to load: OOT's 1365x1365x90 ASTC terrain array (~220 MiB as BC3), taken
    # in Hyrule Field and wanted again at once, froze the game 1.95 s re-decoding 90 layers (fritchii-29).
    keep_old = '        if (must_download) {\n'
    if texture_cache_text.count(keep_old) == 1:
        texture_cache_text = texture_cache_text.replace(keep_old,
            '#ifdef PS5_NATIVE\n'
            '        if ((must_download || True(image.flags & ImageFlagBits::CostlyLoad)) &&\n'
            '            image.unswizzled_size_bytes > 64_MiB) {\n'
            '            return false;  // kept: a write-back needs three times its size, a reload seconds\n'
            '        }\n'
            '#endif\n' + keep_old, 1)
    else:
        print('VK-PORT-SKIP', 'Collector write-back anchor changed: huge GPU-written images not kept', file=sys.stderr)
    if '"performance.h"' not in texture_cache_text:
        texture_cache_text = texture_cache_text.replace('namespace VideoCommon {', '#include "performance.h"\n\nnamespace VideoCommon {', 1)
    if texture_cache_path.read_text() != texture_cache_text:
        texture_cache_path.write_text(texture_cache_text)
elif '::Eden::Performance::GraphicsMemoryIsCritical();\n    if (out_of_memory || pool_critical' not in texture_cache_text:
    raise RuntimeError('Pinned texture collector call changed: pool-critical collection')

# Large texture arrays uploaded and decoded a layer at a time (texture_layers above) as their own group:
# the texture cache's other edits are left out on a lenient base, and without these an ASTC array kept
# as BC3 was staged whole (OOT's 1365x1365x90: a 290 MB heap buffer and a 512 MiB staging buffer
# asked of the Pro's pool at once, then the guest found no block). Fritchii counts uploads before
# its upload timer, so the first edit goes in after the counters rather than replacing the upload.
layers_path = output / 'vulkan-cache/video_core/texture_cache/texture_cache.h'
layers_text = layers_path.read_text()
if 'image.info.layer_stride;\n                auto layer_staging' not in layers_text:
    layers_tail = ('    if (!UploadInChunks(image)) {\n'
                   '        auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));\n'
                   '        UploadImageContents(image, staging);\n')
    layer_edits = list(texture_layers)
    first_old, first_new = layer_edits[0]
    # The first edit ends with the upload it goes before (with or without the line after it).
    tail = next(t for t in (layers_tail, layers_tail.rsplit('        UploadImageContents', 1)[0])
                if first_old.endswith(t) and first_new.endswith(t))
    layer_edits[0] = (first_old[:-len(tail)], first_new[:-len(tail)])
    for old, new in layer_edits:
        if layers_text.count(old) != 1:
            raise RuntimeError(f'Pinned texture cache changed: layer uploads: {old.strip().splitlines()[0][:60]}')
        layers_text = layers_text.replace(old, new)
    if '"performance.h"' not in layers_text:
        layers_text = layers_text.replace('namespace VideoCommon {', '#include "performance.h"\n\nnamespace VideoCommon {', 1)
    # The OpenGL renderer includes this header too: the Vulkan runtime is only named in the checks.
    layers_text = layers_text.replace('namespace VideoCommon {', 'namespace Vulkan {\nclass TextureCacheRuntime;\n}\n\nnamespace VideoCommon {', 1)
    layers_path.write_text(layers_text)

adapt('src/video_core/texture_cache/util.cpp', 'texture_util.cpp', [
    ('#include <vector>', '#include <cstring>\n#include <vector>\n#include "texture_partial.h"'),
    ('} // namespace VideoCommon', (port / 'texture_rows.inc').read_text() + '\n} // namespace VideoCommon'),
])

# The four calls inside bind_geometry and the three inside bind_update. They sit in buffer_cache.h
# rather than in Configure, and both spend their time in SynchronizeBuffer's page walk over the
# memory tracker - once for the index buffer, once per enabled vertex buffer, once per per-stage
# segment, every draw, dirty or not. Which of the seven that is cannot be decided by reading it:
# the texture-buffer loop looked like the cost of bind and measured 2.8%.
adapt('src/video_core/buffer_cache/buffer_cache.h',
      'vulkan-cache/video_core/buffer_cache/buffer_cache.h', [
    ('#include "video_core/buffer_cache/buffer_cache_base.h"',
     '#include "video_core/buffer_cache/buffer_cache_base.h"\n#include "performance.h"'),
    ('void BufferCache<P>::BindHostGeometryBuffers(bool is_indexed) {',
     'void BufferCache<P>::BindHostGeometryBuffers(bool is_indexed) {\n'
     '    long long since_geo = ::Eden::Performance::MarkStart();'),
    ('    BindHostVertexBuffers();\n'
     '    BindHostTransformFeedbackBuffers();\n'
     '    if (current_draw_indirect) {\n'
     '        BindHostDrawIndirectBuffers();\n'
     '    }\n'
     '}',
     '    ::Eden::Performance::Mark(::Eden::Performance::geo_index, since_geo);\n'
     '    BindHostVertexBuffers();\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::geo_vertex, since_geo);\n'
     '    BindHostTransformFeedbackBuffers();\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::geo_xf, since_geo);\n'
     '    if (current_draw_indirect) {\n'
     '        BindHostDrawIndirectBuffers();\n'
     '    }\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::geo_indirect, since_geo);\n'
     '}'),
    ('void BufferCache<P>::DoUpdateGraphicsBuffers(bool is_indexed) {',
     'void BufferCache<P>::DoUpdateGraphicsBuffers(bool is_indexed) {\n'
     '    long long since_upd = ::Eden::Performance::MarkStart();'),
    ('        UpdateTransformFeedbackBuffers();\n'
     '        for (size_t stage = 0; stage < NUM_STAGES; ++stage) {\n'
     '            UpdateUniformBuffers(stage);\n'
     '            UpdateStorageBuffers(stage);\n'
     '            UpdateTextureBuffers(stage);\n'
     '        }\n'
     '        if (current_draw_indirect) {\n'
     '            UpdateDrawIndirect();\n'
     '        }',
     '        UpdateTransformFeedbackBuffers();\n'
     '        ::Eden::Performance::Mark(::Eden::Performance::upd_state, since_upd);\n'
     '        for (size_t stage = 0; stage < NUM_STAGES; ++stage) {\n'
     '            UpdateUniformBuffers(stage);\n'
     '            UpdateStorageBuffers(stage);\n'
     '            UpdateTextureBuffers(stage);\n'
     '        }\n'
     '        ::Eden::Performance::Mark(::Eden::Performance::upd_stage, since_upd);\n'
     '        if (current_draw_indirect) {\n'
     '            UpdateDrawIndirect();\n'
     '        }\n'
     '        ::Eden::Performance::Mark(::Eden::Performance::upd_indirect, since_upd);'),
])

# These costs sit outside vkCreateGraphicsPipelines: guest shader translation,
# worker readiness, and background serialization can block different threads.
# The six phases inside Configure. draw_config is all of Configure but SetEngine's two pointer
# assignments, and ConfigureImpl is ~210 lines; without these the 2.78 us a draw spends
# configuring arrives as one number with nothing to aim at. One running clock, marked at the
# boundaries the source already has.
configure_phases = [
    ('bool GraphicsPipeline::ConfigureImpl(bool is_indexed) {',
     'bool GraphicsPipeline::ConfigureImpl(bool is_indexed) {\n'
     '    long long since = ::Eden::Performance::MarkStart();'),
    ('    buffer_cache.SetUniformBuffersState(enabled_uniform_buffer_masks, &uniform_buffer_sizes);',
     '    buffer_cache.SetUniformBuffersState(enabled_uniform_buffer_masks, &uniform_buffer_sizes);\n'
     '    // Sampler and image tables synced, uniform-buffer state set.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_sync, since);'),
    ('            texture_cache.FillImageViews(std::span(views.data(), views.size()), false,\n'
     '                                         Spec::has_images);\n'
     '        }\n'
     '    }',
     '            texture_cache.FillImageViews(std::span(views.data(), views.size()), false,\n'
     '                                         Spec::has_images);\n'
     '        }\n'
     '    }\n'
     '    // Every texture handle read out of guest memory, every sampler and view filled.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_read, since);\n'
     '    long long since_bind = ::Eden::Performance::MarkStart();'),
    # Inside cfg_read and cfg_desc, on clocks of their own (lever-stage3 census): the texture
    # handle reads out of guest memory apart from the view fill, and the per-stage buffer binds
    # (uniform streaming) apart from the descriptor pushes.
    ('    const bool via_header_index{regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding};',
     '    long long since_handles = ::Eden::Performance::MarkStart();\n'
     '    const bool via_header_index{regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding};'),
    ('    ASSERT(views.size() == num_image_elements);',
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_handles, since_handles);\n'
     '    ASSERT(views.size() == num_image_elements);'),
    ('            buffer_cache.BindHostStageBuffers(stage);',
     '            long long since_stage = ::Eden::Performance::MarkStart();\n'
     '            buffer_cache.BindHostStageBuffers(stage);\n'
     '            ::Eden::Performance::Mark(::Eden::Performance::cfg_stage_buffers, since_stage);'),
    ('    if (regs.transform_feedback_enabled != 0) {',
     '    // The texture-buffer loop over every enabled stage.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::bind_tbo, since_bind);\n'
     '    if (regs.transform_feedback_enabled != 0) {'),
    ('    buffer_cache.UpdateGraphicsBuffers(is_indexed);',
     '    buffer_cache.UpdateGraphicsBuffers(is_indexed);\n'
     '    // Index, vertex, transform-feedback and per-stage buffer updates done.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::bind_update, since_bind);'),
    ('    buffer_cache.BindHostGeometryBuffers(is_indexed);',
     '    buffer_cache.BindHostGeometryBuffers(is_indexed);\n'
     '    // Storage, texture and geometry buffers bound.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::bind_geometry, since_bind);\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_bind, since);'),
    ('    texture_cache.UpdateRenderTargets(false);',
     '    // Descriptor queue acquired, image descriptors pushed, uploads barriered.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_desc, since);\n'
     '    texture_cache.UpdateRenderTargets(false);'),
    ('    if (IsBuilt() && (!pipeline || build->Failed())) {',
     '    // Render targets current, feedback loops checked.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_rt, since);\n'
     '    if (IsBuilt() && (!pipeline || build->Failed())) {'),
    ('        configured = ConfigureDraw(rescaling, render_area);\n    }\n',
     '        configured = ConfigureDraw(rescaling, render_area);\n    }\n'
     '    // Compare the descriptor payload against last draw\'s and record the bindings.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_draw, since);\n'),
]
for name in ('vk_graphics_pipeline', 'vk_compute_pipeline'):
    anchors = [
        ('#include <algorithm>', '#include <algorithm>\n#include "performance.h"'),
    ]
    # The draw's wait for its pipeline to be built (the mesh emulation's own wait, nested deeper, stays out).
    if name == 'vk_graphics_pipeline':
        anchors.append(('        scheduler.Record([this](vk::CommandBuffer) {\n            const auto begin = std::chrono::steady_clock::now();',
                        '        scheduler.Record([this](vk::CommandBuffer) {\n            auto timer = Eden::Performance::VulkanTimer(16);\n            const auto begin = std::chrono::steady_clock::now();'))
    else:
        anchors.append(('        scheduler.Record([this](vk::CommandBuffer) {\n            RunBuild();\n            std::unique_lock lock{build_mutex};',
                        '        scheduler.Record([this](vk::CommandBuffer) {\n            auto timer = Eden::Performance::VulkanTimer(16);\n            RunBuild();\n            std::unique_lock lock{build_mutex};'))
    if name == 'vk_graphics_pipeline':
        anchors += configure_phases
    adapt(f'src/video_core/renderer_vulkan/{name}.cpp', name + '_cost.cpp', anchors)
shader_costs = [
    ('#include <algorithm>', '#include <algorithm>\n#include <cstdio>\n#include <set>\n#include <unordered_set>\n'
     '#include "performance.h"\n#include "dev_vulkan.h"\n#include "diagnostics.h"'),
    # Development crash probes: report each pipeline's first use (stage hashes) to klog,
    # which survives a GPU fault that kills the title.
    ('    current_pipeline = pipeline.get();\n    return BuiltPipeline(current_pipeline);\n}',
     '    current_pipeline = pipeline.get();\n'
     '    if (::Eden::DevVulkan::trace_pipelines) {\n'
     '        static std::unordered_set<const void*> traced;\n'
     '        if (traced.insert(current_pipeline).second) {\n'
     '            const auto& h = graphics_key.unique_hashes;\n'
     '            char text[256];\n'
     '            std::snprintf(text, sizeof(text), "G%zu p=%p new=%d va=%llx vb=%llx tc=%llx te=%llx gs=%llx fs=%llx",\n'
     '                          traced.size(), (const void*)current_pipeline, int(is_new), (unsigned long long)h[0], (unsigned long long)h[1],\n'
     '                          (unsigned long long)h[2], (unsigned long long)h[3], (unsigned long long)h[4],\n'
     '                          (unsigned long long)h[5]);\n'
     '            ::Eden::Report("pipeline", text);\n'
     '            LOG_INFO(Render_Vulkan, "EDEN_PIPELINE_FIRST_USE {}", text);\n'
     '        }\n'
     '    }\n'
     '    return BuiltPipeline(current_pipeline);\n}'),
    ('    if (!is_new) {\n        return pipeline.get();\n    }\n    pipeline = CreateComputePipeline(key, shader);\n    return pipeline.get();\n}',
     '    if (is_new) {\n'
     '        pipeline = CreateComputePipeline(key, shader);\n'
     '    }\n'
     '    if (::Eden::DevVulkan::trace_pipelines && pipeline) {\n'
     '        static std::unordered_set<const void*> traced;\n'
     '        if (traced.insert(pipeline.get()).second) {\n'
     '            char text[160];\n'
     '            std::snprintf(text, sizeof(text), "C%zu p=%p new=%d cs=%llx shared=%u wg=%u,%u,%u", traced.size(),\n'
     '                          (const void*)pipeline.get(), int(is_new), (unsigned long long)key.unique_hash, unsigned(key.shared_memory_size),\n'
     '                          unsigned(key.workgroup_size[0]), unsigned(key.workgroup_size[1]),\n'
     '                          unsigned(key.workgroup_size[2]));\n'
     '            ::Eden::Report("pipeline", text);\n'
     '            LOG_INFO(Render_Vulkan, "EDEN_PIPELINE_FIRST_USE {}", text);\n'
     '        }\n'
     '    }\n'
     '    return pipeline.get();\n}'),
    ('std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {',
     'std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {\n    auto timer = Eden::Performance::VulkanTimer(17);'),
    ('    const ComputePipelineCacheKey& key, const ShaderInfo* shader) {',
     '    const ComputePipelineCacheKey& key, const ShaderInfo* shader) {\n    auto timer = Eden::Performance::VulkanTimer(17);'),
    ('                                                 u32 cache_version) try {',
     '                                                 u32 cache_version) try {\n    auto timer = Eden::Performance::VulkanTimer(18);'),
    ('}\n\nPipelineCache::~PipelineCache()',
     '    if (Eden::Performance::vulkan_cost_enabled.load(std::memory_order_relaxed))\n'
     '        std::printf("EDEN_VULKAN_DYNAMIC blend=%u enables=%u vertex=%u\\n",\n'
     '                    unsigned(dynamic_features.has_extended_dynamic_state_3_blend),\n'
     '                    unsigned(dynamic_features.has_extended_dynamic_state_3_enables),\n'
     '                    unsigned(dynamic_features.has_dynamic_vertex_input));\n'
     '}\n\nPipelineCache::~PipelineCache()'),
    # Static vertex input (no attribute) for pipelines whose vertex shader reads no vertex attribute
    # (dev_vulkan.h static_vertex_input): with VK_EXT_vertex_input_dynamic_state the console's
    # driver drew them wrong (a full-screen triangle from the vertex index covering a game's
    # cutscenes with a flat colour). Building a pipeline records whether its vertex shader reads
    # attributes; the static key has no attribute, so the disk cache keeps loading it.
    ('#include "video_core/engines/maxwell_3d.h"',
     '#include "video_core/engines/maxwell_3d.h"\n#include "video_core/dirty_flags.h"\n'
     '#include <atomic>\n#include <mutex>\n#include <set>\n#include <unordered_set>\n'
     '#include "video_core/renderer_vulkan/vk_state_tracker.h"'),
    ('GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {',
     """namespace {
std::mutex eden_attributeless_mutex;
using EdenVertexIdentity = std::array<u64, 2>;
std::set<EdenVertexIdentity> eden_attributeless_vertex;
std::atomic<u32> eden_attributeless_generation{0};
bool eden_static_vertex_input_bound = false;

void EdenRecordAttributeless(EdenVertexIdentity vertex_hash, const Shader::Info& info) {
    for (size_t index = 0; index < Shader::IR::NUM_GENERICS; ++index) {
        if (info.loads.Generic(index)) {
            return;
        }
    }
    std::scoped_lock lock{eden_attributeless_mutex};
    if (eden_attributeless_vertex.insert(vertex_hash).second) {
        eden_attributeless_generation.fetch_add(1, std::memory_order_release);
    }
}

// GPU thread: whether the vertex shader reads no attribute, as far as built pipelines tell.
bool EdenAttributeless(EdenVertexIdentity vertex_hash) {
    static EdenVertexIdentity last_hash{};
    static u32 last_generation = ~0u;
    static bool last_result = false;
    const u32 generation = eden_attributeless_generation.load(std::memory_order_acquire);
    if (vertex_hash != last_hash || generation != last_generation) {
        std::scoped_lock lock{eden_attributeless_mutex};
        last_hash = vertex_hash;
        last_generation = generation;
        last_result = eden_attributeless_vertex.contains(vertex_hash);
    }
    return last_result;
}

// A static key without attributes (what the loader takes back from the disk cache).
bool EdenAttributelessStaticKey(const FixedPipelineState& state) {
    return state.dynamic_vertex_input == 0 &&
           std::ranges::none_of(state.attributes,
                                [](const auto& attribute) { return attribute.raw != 0; });
}
} // Anonymous namespace

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {"""),
    ('    graphics_key.state.Refresh(*maxwell3d, dynamic_features);\n',
     """    graphics_key.state.Refresh(*maxwell3d, dynamic_features);
    const bool static_vertex_input = graphics_key.state.dynamic_vertex_input != 0 &&
                                     ::Eden::DevVulkan::static_vertex_input &&
                                     EdenAttributeless({graphics_key.unique_hashes[0], graphics_key.unique_hashes[1]});
    if (static_vertex_input) {
        // Instance-rate streams as the registers set them (with every stream per vertex the
        // driver drew these wrong too); no attribute, so the key does not depend on what the
        // registers hold for attributes the shader does not read.
        const auto& regs = maxwell3d->regs;
        graphics_key.state.dynamic_vertex_input.Assign(0);
        graphics_key.state.enabled_divisors = 0;
        graphics_key.state.attributes = {};
        graphics_key.state.binding_divisors = {};
        for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
            const bool is_enabled = regs.vertex_stream_instances.IsInstancingEnabled(index);
            graphics_key.state.binding_divisors[index] =
                is_enabled ? regs.vertex_streams[index].frequency : 0;
            graphics_key.state.enabled_divisors |= (is_enabled ? u64{1} : 0) << index;
        }
        // The union's attribute_types now holds divisors: the next dynamic draw recomputes them
        // and sets its vertex input again (a static pipeline replaced it).
        maxwell3d->dirty.flags[Dirty::VertexInput] = true;
    }
    if (static_vertex_input && !eden_static_vertex_input_bound) {
        // Rebind the vertex buffers with their strides, dynamic state of the static pipeline.
        auto& flags = maxwell3d->dirty.flags;
        flags[VideoCommon::Dirty::VertexBuffers] = true;
        for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
            flags[VideoCommon::Dirty::VertexBuffer0 + index] = true;
        }
    }
    eden_static_vertex_input_bound = static_vertex_input;
"""),
    ('            (key.state.dynamic_vertex_input != 0) !=\n                dynamic_features.has_dynamic_vertex_input) {',
     '            ((key.state.dynamic_vertex_input != 0) !=\n'
     '                 dynamic_features.has_dynamic_vertex_input &&\n'
     '             !(dynamic_features.has_dynamic_vertex_input &&\n'
     '               ::Eden::DevVulkan::static_vertex_input &&\n'
     '               EdenAttributelessStaticKey(key.state)))) {'),
    ('    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};\n',
     '    if (uses_vertex_b) {\n'
     '        EdenRecordAttributeless({key.unique_hashes[0], key.unique_hashes[1]}, programs[1].info);\n'
     '    }\n'
     '    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};\n'),
]
# Time both graphics and compute preparation, without modifying the compiler or
# reading guest state on another thread. These intervals overlap shader_prepare.
shader_source = (source / 'src/video_core/renderer_vulkan/vk_pipeline_cache.cpp').read_text()
for expression, index, count in (
    ('main_pools.ReleaseContents()', 19, 2),
    ('TranslateProgram(pools.inst, pools.block, env, cfg, host_info)', 21, 3),
    ('EmitSPIRV(profile, runtime_info, program, binding)', 22, 1),
    ('EmitSPIRV(profile, program)', 22, 1),
    ('BuildShader(device, code)', 23, 2),
):
    if shader_source.count(expression) != count:
        if LENIENT:
            SKIPPED.append(str(f'Shader cost anchor changed: {expression}')); print('VK-PORT-SKIP', f'Shader cost anchor changed: {expression}', file=sys.stderr)
            continue
        raise RuntimeError(f'Shader cost anchor changed: {expression}')
    shader_source = shader_source.replace(expression,
        f'([&] {{ auto cost = Eden::Performance::VulkanTimer({index}); '
        f'return {expression}; }}())')
for statement in (
    'Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);',
    'Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};',
):
    expected = 2 if 'cfg_offset' in statement else 1  # graphics exception dump too
    if shader_source.count(statement) != expected:
        if LENIENT:
            SKIPPED.append(str(f'Shader CFG anchor changed: {statement}')); print('VK-PORT-SKIP', f'Shader CFG anchor changed: {statement}', file=sys.stderr)
            continue
        raise RuntimeError(f'Shader CFG anchor changed: {statement}')
    shader_source = shader_source.replace(statement,
        'auto cfg_cost = Eden::Performance::VulkanTimer(20);\n        ' + statement +
        '\n        cfg_cost.reset();')
for old, new in shader_costs:
    if shader_source.count(old) != 1:
        if LENIENT:
            SKIPPED.append(str(f'Shader pipeline anchor changed: {old[:60]}')); print('VK-PORT-SKIP', f'Shader pipeline anchor changed: {old[:60]}', file=sys.stderr)
            continue
        raise RuntimeError(f'Shader pipeline anchor changed: {old[:60]}')
    shader_source = shader_source.replace(old, new)
# The driver's pipeline cache file (vulkan_pipelines.bin). Eden rewrote it in place (the source
# now writes a temporary file and renames it; this keeps a failed copy from removing the cache): opening it
# emptied it, and a large game's cache takes seconds to write, so a session that ended during the
# write (the ten-second limit on leaving a game, a crash, rest mode) left it empty or cut off, and
# every pipeline was compiled again at the next start (#170). It is now written beside the old
# file and renamed over it once whole, and leaving a game rewrites it only when pipelines were
# added since it was last written.
# The anchors include the timer the cost patches above add to the function.
pipeline_cache_writes = [
    ('PipelineCache::~PipelineCache() {\n'
     '    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {\n'
     '        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,\n'
     '                                     CACHE_VERSION);\n'
     '    }\n'
     '}\n',
     'PipelineCache::~PipelineCache() {\n'
     '    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {\n'
     '        // A flush still being written would race this one for the same file.\n'
     '        serialization_thread.WaitForRequests();\n'
     '        // Written after the game loaded and at each flush: with no pipeline added since,\n'
     '        // the file already holds everything, and writing it again only delays leaving.\n'
     '        const bool written = last_flush.time_since_epoch().count() != 0;\n'
     '        if (!written || pipelines_since_flush != 0) {\n'
     '            SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,\n'
     '                                         CACHE_VERSION);\n'
     '        }\n'
     '    }\n'
     '}\n'),
    ('''void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache,
                                                 u32 cache_version) try {
    auto timer = Eden::Performance::VulkanTimer(18);
    // Written beside the cache and renamed over it: the emulator stopped mid-write left the cache
    // empty, and every later boot failed to read it.
    auto temporary = filename;
    temporary += ".tmp";
    std::ofstream file(temporary, std::ios::binary);
    file.exceptions(std::ifstream::failbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    file.write(VULKAN_CACHE_MAGIC_NUMBER.data(), VULKAN_CACHE_MAGIC_NUMBER.size())
        .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));

    size_t cache_size = 0;
    std::vector<char> cache_data;
    if (pipeline_cache) {
        pipeline_cache.Read(&cache_size, nullptr);
        cache_data.resize(cache_size);
        pipeline_cache.Read(&cache_size, cache_data.data());
    }
    file.write(cache_data.data(), cache_size);
    file.close();
    std::error_code error;
    std::filesystem::rename(temporary, filename, error);
    if (error) {
        LOG_ERROR(Common_Filesystem, "Failed to replace Vulkan driver pipeline cache file {}: {}",
                  Common::FS::PathToUTF8String(filename), error.message());
        std::filesystem::remove(temporary, error);
        return;
    }

    LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
             Common::FS::PathToUTF8String(filename));

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}
''',
'''void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache,
                                                 u32 cache_version) {
    auto timer = Eden::Performance::VulkanTimer(18);  // as the port times the write it replaces
    // Written whole beside the cache, then renamed over it: until the rename the previous cache
    // stays as it was, and a write that fails or is cut short leaves it alone.
    auto partial = filename;
    partial += ".part";
    std::error_code error;
    try {
        size_t cache_size = 0;
        std::vector<char> cache_data;
        if (pipeline_cache) {
            pipeline_cache.Read(&cache_size, nullptr);
            cache_data.resize(cache_size);
            pipeline_cache.Read(&cache_size, cache_data.data());
        }
        {
            std::ofstream file(partial, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                LOG_ERROR(Common_Filesystem, "Failed to open Vulkan driver pipeline cache file {}",
                          Common::FS::PathToUTF8String(partial));
                return;
            }
            file.exceptions(std::ofstream::failbit | std::ofstream::badbit);
            file.write(VULKAN_CACHE_MAGIC_NUMBER.data(), VULKAN_CACHE_MAGIC_NUMBER.size())
                .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version))
                .write(cache_data.data(), static_cast<std::streamsize>(cache_size));
            file.flush();
        }
        std::filesystem::rename(partial, filename, error);
        if (error) {
            LOG_ERROR(Common_Filesystem, "Failed to replace Vulkan driver pipeline cache file {}: {}",
                      Common::FS::PathToUTF8String(filename), error.message());
            std::filesystem::remove(partial, error);
            return;
        }
        LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
                 Common::FS::PathToUTF8String(filename));
    } catch (const std::ios_base::failure& e) {
        LOG_ERROR(Common_Filesystem, "{}", e.what());
        std::filesystem::remove(partial, error);
    } catch (const std::bad_alloc&) {
        // A copy of a large cache did not fit in memory: keep the one on disk.
        LOG_ERROR(Render_Vulkan, "No memory to copy the Vulkan driver pipeline cache; kept the previous file");
        std::filesystem::remove(partial, error);
    }
}
'''),
]
for old, new in pipeline_cache_writes:
    if shader_source.count(old) != 1:
        if LENIENT:
            SKIPPED.append(str(f'Pipeline cache write anchor changed: {old[:60]}')); print('VK-PORT-SKIP', f'Pipeline cache write anchor changed: {old[:60]}', file=sys.stderr)
            continue
        raise RuntimeError(f'Pipeline cache write anchor changed: {old[:60]}')
    shader_source = shader_source.replace(old, new)
shader_output = output / 'vk_pipeline_cache_cost.cpp'
if not shader_output.exists() or shader_output.read_text() != shader_source:
    shader_output.write_text(shader_source)

# Scope only the existing synchronous operations; never alter their ordering.
# Report these caller costs separately from the overlapping API totals.
import re
# Development GPU fault probes (dev_vulkan.h): multi-range storage buffers are
# the one path that binds sparse ranges of the console's device memory.
probes = {
    'vk_multi_range_buffer': [
        ('#include <algorithm>', '#include <algorithm>\n#include <cstdio>\n#include "dev_vulkan.h"\n#include "diagnostics.h"'),
        ('    if (bind_result != VK_SUCCESS) {\n        return SparseBuffer{};\n    }',
         '    if (bind_result != VK_SUCCESS) {\n'
         '        if (::Eden::DevVulkan::trace_pipelines) ::Eden::Report("multirange", "sparse bind failed");\n'
         '        return SparseBuffer{};\n    }'),
        ('    retired.push_back(Retired{',
         '    if (::Eden::DevVulkan::trace_pipelines) {\n'
         '        static unsigned retires = 0;\n'
         '        if (++retires <= 64) {\n'
         '            char text[96];\n'
         '            std::snprintf(text, sizeof(text), "retire #%u sparse=%d tick=%llu", retires,\n'
         '                          int(bool(entry.sparse_handle)), (unsigned long long)scheduler.CurrentTick());\n'
         '            ::Eden::Report("multirange", text);\n'
         '        }\n'
         '    }\n'
         '    retired.push_back(Retired{'),
        ('    entries.emplace(key, std::move(entry));',
         '    if (::Eden::DevVulkan::trace_pipelines) {\n'
         '        static unsigned created = 0;\n'
         '        if (++created <= 256) {\n'
         '            char text[192];\n'
         '            std::snprintf(text, sizeof(text), "new #%u key=%llx total=%llu sources=%zu sparse=%d addr=%llx",\n'
         '                          created, (unsigned long long)key, (unsigned long long)total, sources.size(),\n'
         '                          int(bool(entry.sparse_handle)), (unsigned long long)entry.address);\n'
         '            ::Eden::Report("multirange", text);\n'
         '        }\n'
         '    }\n'
         '    entries.emplace(key, std::move(entry));'),
    ],
    'vk_buffer_cache': [
        ('#include <algorithm>', '#include <algorithm>\n#include "dev_vulkan.h"'),
        ('bool BufferCacheRuntime::BindMultiRangeStorageBuffer(u64 key, bool is_written) {\n',
         'bool BufferCacheRuntime::BindMultiRangeStorageBuffer(u64 key, bool is_written) {\n'
         '    if (::Eden::DevVulkan::disable_multi_range) {\n        return false;\n    }\n'),
    ],
    # With custom border colours off (the console default), report which clamp-to-border
    # samplers ask for a colour the three standard ones cannot give exactly.
    'vk_texture_cache': [
        ('#include <algorithm>', '#include <algorithm>\n#include <cstdio>\n#include "dev_vulkan.h"\n#include "diagnostics.h"'),
        ('    has_custom_border_colors = samples_border && device.IsCustomBorderColorUsable();',
         '    if (::Eden::DevVulkan::trace_pipelines && samples_border) {\n'
         '        static std::vector<std::array<float, 4>> reported;\n'
         '        const auto report = [](const std::array<float, 4>& c, const char* kind) {\n'
         '            if (c == std::array<float, 4>{0, 0, 0, 0} || c == std::array<float, 4>{0, 0, 0, 1} ||\n'
         '                c == std::array<float, 4>{1, 1, 1, 1} || reported.size() >= 32 ||\n'
         '                std::find(reported.begin(), reported.end(), c) != reported.end()) return;\n'
         '            reported.push_back(c);\n'
         '            char text[128];\n'
         '            std::snprintf(text, sizeof(text), "non-standard %s border %g %g %g %g", kind, c[0], c[1],\n'
         '                          c[2], c[3]);\n'
         '            ::Eden::Report("sampler", text);\n'
         '        };\n'
         '        report(border_color, "linear");\n'
         '        if (tsc.srgb_conversion) report(srgb_border_color, "srgb");\n'
         '    }\n'
         '    has_custom_border_colors = samples_border && device.IsCustomBorderColorUsable();'),
    ],
}
# An ASTC-to-BC transcode decodes into an RGBA copy of the whole image (4 bytes a pixel, every
# level) that lives until the GPU has used it. A course load makes hundreds in a frame, which
# held 6.7 GB of the pool the console's GPU shares with the CPU (the next request found none).
# So the copies in flight are bounded: by bytes (256 MiB), waiting for the oldest copy only. The
# earlier bound (a Finish from 16 pending) waited for the whole GPU every 16 textures: 40-60 % of
# Mario Kart World's 470-560 ms race frames on the Pro (lever-hitches, hitch_pc samples). What is
# recorded is submitted every 32 MiB, so the GPU works through the copies while more are recorded
# and the oldest is usually done before the bound is reached.
# Lichii keeps finished decode images for reuse (up to 256 MiB) but not the ones still in flight;
# they are counted from their shape here.
probes.setdefault('vk_texture_cache', []).append(
    ('    constexpr VkFormat DecodeFormat = VK_FORMAT_R8G8B8A8_UNORM;\n',
     '    {\n'
     '        using namespace Common::Literals;\n'
     '        const auto held_by = [](const PendingTranscode& t) {\n'
     '            u64 bytes{};\n'
     '            for (u32 level = 0; level < t.levels; ++level) {\n'
     '                bytes += u64{(std::max)(t.width >> level, 1U)} * (std::max)(t.height >> level, 1U) * 4;\n'
     '            }\n'
     '            return bytes * t.layers;\n'
     '        };\n'
     '        const u64 decode_bytes = u64{width} * height * layers * 4 * (levels > 1 ? 4 : 3) / 3;\n'
     '        u64 held_bytes = 0, recorded_bytes = 0;\n'
     '        for (const PendingTranscode& held : pending_transcodes) {\n'
     '            held_bytes += held_by(held);\n'
     '            recorded_bytes += held.tick >= scheduler.CurrentTick() ? held_by(held) : 0;\n'
     '        }\n'
     '        if (recorded_bytes >= 32_MiB) {\n'
     '            scheduler.Flush();\n'
     '        }\n'
     '        while (!pending_transcodes.empty() && held_bytes + decode_bytes > 128_MiB) {\n'
     '            scheduler.Wait(pending_transcodes.front().tick);\n'
     '            while (!pending_transcodes.empty() && scheduler.IsFree(pending_transcodes.front().tick)) {\n'
     '                held_bytes -= held_by(pending_transcodes.front());\n'
     '                pending_transcodes.pop_front();\n'
     '            }\n'
     '        }\n'
     '    }\n'
     '    constexpr VkFormat DecodeFormat = VK_FORMAT_R8G8B8A8_UNORM;\n'))
for name, index, expected in (
    ('vk_fence_manager', 6, 1), ('vk_buffer_cache', 7, 2),  # 0127: the quad index buffer no longer waits
    ('vk_texture_cache', 8, 1), ('vk_swapchain', 9, 6),
    ('vk_descriptor_buffer', 10, 2), ('vk_multi_range_buffer', 11, 1),
    ('vk_compute_pass', 12, 1),
):
    relative = f'src/video_core/renderer_vulkan/{name}.cpp'
    destination = 'vulkan_swapchain.cpp' if name == 'vk_swapchain' else name + '_cost.cpp'
    # Preserve the earlier swapchain error-recovery adaptation.
    path = output / destination
    text = path.read_text() if name == 'vk_swapchain' else (source / relative).read_text()
    pattern = r'(?m)^( +)(scheduler\.(?:Wait|Finish)\([^\n]*\);)$'
    matches = list(re.finditer(pattern, text))
    if len(matches) != expected:
        # Lenient (Lichii base): the wait timers are diagnostics; every wait site still gets one.
        if not LENIENT:
            raise RuntimeError(f'Wait-site anchors changed: {name}: {len(matches)} != {expected}')
        SKIPPED.append(f'Wait-site count: {name}: {len(matches)} != {expected}')
        print('VK-PORT-SKIP', f'Wait-site count: {name}: {len(matches)} != {expected}', file=sys.stderr)
    text = re.sub(pattern, lambda m: m[1] + '{ auto timer = Eden::Performance::VulkanTimer('
                  + str(index) + '); ' + m[2] + ' }', text)
    if name == 'vk_compute_pass':
        # ASTC decode has no CPU consumer. Its existing image barrier orders
        # later shaders, and staging/descriptor reuse is protected by ticks.
        text = text.replace('VulkanTimer(12); scheduler.Finish();',
                            'VulkanTimer(12); scheduler.Flush();')
    before_probes = text  # lenient: the probes depend on each other, so one skip drops them all
    for probe_old, probe_new in probes.get(name, []):
        if text.count(probe_old) != 1:
            if LENIENT:
                SKIPPED.append(str(f'Probe anchor changed: {name}: {probe_old[:60]}')); print('VK-PORT-SKIP', f'Probe anchor changed: {name}: {probe_old[:60]} (all its probes dropped)', file=sys.stderr)
                text = before_probes
                break
            raise RuntimeError(f'Probe anchor changed: {name}: {probe_old[:60]}')
        text = text.replace(probe_old, probe_new)
    text = '#include "performance.h"\n' + text
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
# Out of device memory, the allocator asks the texture cache runtime what it can give back
# (performance.h, graphics_memory_recover): on the runtime's own thread, the ASTC decode copies it
# keeps, the staging buffers the GPU is done with, and from the second attempt everything after a
# wait for the GPU. Installed by the runtime's first TickFrame, on the thread that ticks it. Its own
# edit: the timers and probes above may be left out on a lenient base.
# shortcut: the runtime has no destructor to uninstall it; the owner-thread check keeps other threads out,
# and a session ends with its GPU thread. Add a destructor hook if a runtime is ever recreated in a session.
runtime_path = output / 'vk_texture_cache_cost.cpp'
runtime_text = runtime_path.read_text()
tick_old = 'void TextureCacheRuntime::TickFrame() {\n'
if runtime_text.count(tick_old) != 1:
    raise RuntimeError('Pinned TextureCacheRuntime::TickFrame changed: graphics memory recovery')
staging_header = (source / 'src/video_core/renderer_vulkan/vk_staging_buffer_pool.h').read_text()
release_idle = '            staging_buffer_pool.ReleaseIdle();\n' if 'void ReleaseIdle();' in staging_header else ''
runtime_text = runtime_text.replace(tick_old, (
    'namespace {\n'
    'std::function<bool(int)> eden_memory_recover;\n'
    'std::atomic<std::thread::id> eden_memory_recover_owner{};\n'
    'bool EdenRecoverGraphicsMemory(int attempt) {\n'
    '    if (std::this_thread::get_id() != eden_memory_recover_owner.load(std::memory_order_acquire) ||\n'
    '        !eden_memory_recover) {\n'
    '        return false;\n'
    '    }\n'
    '    return eden_memory_recover(attempt);\n'
    '}\n'
    '} // Anonymous namespace\n\n'
    + tick_old +
    '    if (eden_memory_recover_owner.load(std::memory_order_relaxed) != std::this_thread::get_id()) {\n'
    '        eden_memory_recover = [this](int attempt) {\n'
    '            if (attempt >= 1) {\n'
    '                scheduler.Finish();\n'
    '            }\n'
    '            free_transcodes.clear();\n'
    '            while (!pending_transcodes.empty() && scheduler.IsFree(pending_transcodes.front().tick)) {\n'
    '                pending_transcodes.pop_front();\n'
    '            }\n'
    + release_idle +
    '            return true;\n'
    '        };\n'
    '        eden_memory_recover_owner.store(std::this_thread::get_id(), std::memory_order_release);\n'
    '        ::Eden::Performance::graphics_memory_recover.store(&EdenRecoverGraphicsMemory, std::memory_order_release);\n'
    '    }\n'), 1)
# Fritchii's VkImage pool (EXP42) keeps up to 512 MiB of deleted render targets for reuse and leaves them
# out of the collector's figure. On the console those bytes come out of the pool the guest and the heap
# share: in OOT's Hyrule Field on the Pro it held 509 MiB with 0.7 GiB left free, and the guest-memory
# packer starved (a 230 s freeze, run fritchii-24). Under 2 GiB free it keeps nothing, under 3 GiB 128 MiB.
pool_trim = 'void TextureCacheRuntime::TrimImagePool(u64 max_bytes) {\n'
if runtime_text.count(pool_trim) == 1:
    runtime_text = runtime_text.replace(tick_old, tick_old + (
        '    if (const auto free_memory = ::Eden::Performance::graphics_memory_free.load(std::memory_order_relaxed)) {\n'
        '        const u64 free_bytes = free_memory();  // refreshed at most every 250 ms\n'
        '        if (free_bytes != 0 && free_bytes < (u64{3} << 30)) {\n'
        '            TrimImagePool(free_bytes < (u64{2} << 30) ? 0 : u64{128} << 20);\n'
        '        }\n'
        '    }\n'), 1)
    runtime_text = runtime_text.replace('            free_transcodes.clear();\n',
                                        '            free_transcodes.clear();\n            TrimImagePool(0);\n', 1)
# The console: an ASTC image with uneven BC block mips is re-compressed to BC3 like any other, not kept
# as RGBA8 (four times BC3). Lichii keeps them as RGBA8 for AMD's seam lines on distant terrain (Windows
# driver); in OOT's save load on the Pro its 1365x1365x90 array took 967 MiB that way, and a cap of
# 64 MiB a texture still left 64+ 1536x1536 textures at 12 MiB each (~800 MB): the pool ran out both
# times. EDEN_UNEVEN_RGBA8_ALL=1 keeps them as RGBA8 again (to compare the seams).
uneven_old = '    return HasUnevenBlockMips(info) && !IsHugeAstcArray(device, info);\n}'
if runtime_text.count(uneven_old) == 1:
    runtime_text = runtime_text.replace(uneven_old, (
        '#ifdef PS5_NATIVE\n'
        '    static const bool keep_all = [] {\n'
        '        const char* value = std::getenv("EDEN_UNEVEN_RGBA8_ALL");\n'
        '        return value != nullptr && value[0] == \'1\';\n'
        '    }();\n'
        '    if (!keep_all) {\n'
        '        return false;\n'
        '    }\n'
        '#endif\n') + uneven_old)
else:
    raise RuntimeError('Pinned IsUnevenMipRgba8Astc changed: console RGBA8 cap')
# An ASTC array is re-compressed to BC a few layers at a time (~64 MiB of RGBA8 decode), into one small
# decode image used again for each batch. All at once, OOT's 1365x1365x90 terrain array took a 967 MiB
# decode image and a 213 MiB staging buffer on entering Hyrule Field, with ~0.5 GiB of the Pro's pool
# free: allocations failed, recovery evicted, and frames stalled 0.3-1.8 s (run fritchii-40).
transcode_start = runtime_text.find('void TextureCacheRuntime::TranscodeAstcToBcn(')
transcode_end = runtime_text.find('void TextureCacheRuntime::TransitionImageLayout(', transcode_start)
transcode = runtime_text[transcode_start:transcode_end] if transcode_start >= 0 and transcode_end > 0 else ''
transcode_edits = [
    ('    const u32 layers = static_cast<u32>(info.resources.layers);\n',
     '    const u32 layers = static_cast<u32>(info.resources.layers);\n'
     '    u64 layer_decode_bytes = 0;\n'
     '    for (u32 level = 0; level < levels; ++level) {\n'
     '        layer_decode_bytes += u64{(std::max)(width >> level, 1U)} * (std::max)(height >> level, 1U) * 4;\n'
     '    }\n'
     '    const u32 batch = static_cast<u32>(\n'
     '        std::clamp<u64>((u64{64} << 20) / (std::max)(layer_decode_bytes, u64{1}), 1, layers));\n'),
    ('height * layers * 4 * (levels > 1 ? 4 : 3) / 3;', 'height * batch * 4 * (levels > 1 ? 4 : 3) / 3;'),
    ('t.levels == levels && t.layers == layers;', 't.levels == levels && t.layers == batch;'),
    ('            .arrayLayers = layers,\n', '            .arrayLayers = batch,\n'),
    ('            .layers = layers,\n            .unused_frames', '            .layers = batch,\n            .unused_frames'),
]
tail_start = transcode.find('    astc_decoder_pass->AssembleInto(\n')
tail_end = transcode.find('    pending.tick = scheduler.CurrentTick();\n')
if transcode and tail_start > 0 and tail_end > tail_start and \
        all(transcode.count(old) == 1 for old, _ in transcode_edits) and \
        transcode.count('bc_encoder_pass->Encode(') == 1:
    for old, new in transcode_edits:
        transcode = transcode.replace(old, new)
    tail_start = transcode.find('    astc_decoder_pass->AssembleInto(\n')
    tail_end = transcode.find('    pending.tick = scheduler.CurrentTick();\n')
    transcode = transcode[:tail_start] + r'''    const bool bc3 = !IsOpaqueBc1Astc(device, info) &&
                     Settings::GetAstcRecompression() != Settings::AstcRecompression::Bc1;
    const u64 block_bytes = bc3 ? 16 : 8;
    VideoCommon::ImageInfo batch_info = info;
    for (u32 first = 0; first < layers; first += batch) {
        const u32 count = (std::min)(batch, layers - first);
        batch_info.resources.layers = static_cast<s32>(count);
        StagingBufferRef input = map;  // the guest layout keeps each layer's mip chain together
        input.offset += VkDeviceSize{first} * info.layer_stride;
        astc_decoder_pass->AssembleInto(
            batch_info, 0, size_t{count} * info.layer_stride,
            ASTCDecoderPass::Target{
                .image = *pending.image,
                .aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT,
                .initialized = first != 0,  // after the last batch's encode read it (write after read)
                .level_view = [&pending](s32 level) { return *pending.views[level]; },
                .wait = true,
            },
            input, swizzles);

        // Encode every level of these layers into one buffer, then copy it into the image.
        boost::container::small_vector<VideoCommon::BufferImageCopy, 16> copies;
        u64 total = 0;
        for (u32 level = 0; level < levels; ++level) {
            const u32 level_width = (std::max)(width >> level, 1U);
            const u32 level_height = (std::max)(height >> level, 1U);
            const u32 blocks_x = Common::DivCeil(level_width, 4U);
            const u32 blocks_y = Common::DivCeil(level_height, 4U);
            const u64 size = u64{blocks_x} * blocks_y * count * block_bytes;
            copies.push_back({
                .buffer_offset = total,
                .buffer_size = size,
                .buffer_row_length = blocks_x * 4,
                .buffer_image_height = blocks_y * 4,
                .image_subresource{
                    .base_level = static_cast<s32>(level),
                    .base_layer = static_cast<s32>(first),
                    .num_layers = static_cast<s32>(count),
                },
                .image_offset{0, 0, 0},
                .image_extent{level_width, level_height, 1},
            });
            total += size;
        }
        const auto output = staging_buffer_pool.Request(total, MemoryUsage::DeviceLocal);
        for (u32 level = 0; level < levels; ++level) {
            bc_encoder_pass->Encode(*pending.views[level], copies[level].image_extent.width,
                                    copies[level].image_extent.height, count, bc3, output.buffer,
                                    output.offset, total, copies[level].buffer_offset);
        }
        scheduler.Record([](vk::CommandBuffer cmdbuf) {
            const VkMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, barrier);
        });
        image.UploadMemory(output.buffer, output.offset, copies);
    }
''' + transcode[tail_end:]
    runtime_text = runtime_text[:transcode_start] + transcode + runtime_text[transcode_end:]
elif LENIENT:
    SKIPPED.append('ASTC-to-BC transcode in layer batches'); print('VK-PORT-SKIP ASTC-to-BC transcode in layer batches', file=sys.stderr)
else:
    raise RuntimeError('Pinned TranscodeAstcToBcn changed: layer batches')
# Finished decode images kept for reuse: 32 MiB on the console (256 MiB, with 256 MiB in flight, held
# 413 MiB of scratch when OOT's Hyrule Field ran out of memory on the Pro, run fritchii-45).
free_cap = '    static constexpr u64 MaxFreeTranscodeBytes = u64{256} << 20;\n'
if runtime_text.count(free_cap) == 1:
    runtime_text = runtime_text.replace(free_cap, '    static constexpr u64 MaxFreeTranscodeBytes = u64{32} << 20;\n')
else:
    SKIPPED.append('decode scratch reuse cap'); print('VK-PORT-SKIP decode scratch reuse cap', file=sys.stderr)
runtime_text = '#include <atomic>\n#include <functional>\n#include <thread>\n' + runtime_text
if runtime_path.read_text() != runtime_text:
    runtime_path.write_text(runtime_text)
# Eden loads these shaders when the renderer starts, whether frames are to be generated or not:
# the reasons are only written when the setting is on, or every session without the user's
# Lossless.dll would say so.
adapt('src/video_core/renderer_vulkan/present/lsfg_shaders.cpp', 'vulkan_lsfg_shaders.cpp', [
    ('#include "video_core/vulkan_common/vulkan_device.h"',
     '#include <string>\n#include "common/settings.h"\n#include "video_core/vulkan_common/vulkan_device.h"\n#include "diagnostics.h"'),
    ("""    if (!device.IsVulkanMemoryModelSupported() || !device.HasNullDescriptor() ||
        !device.IsFloat16Supported()) {
        return;
    }""",
     """    const bool wanted = Settings::values.frame_gen.GetValue();
    if (!device.IsVulkanMemoryModelSupported() || !device.HasNullDescriptor() ||
        !device.IsFloat16Supported()) {
        if (wanted)
            Eden::Report("framegen",
                         ("device features: vulkanMemoryModel=" +
                          std::to_string(device.IsVulkanMemoryModelSupported()) + " nullDescriptor=" +
                          std::to_string(device.HasNullDescriptor()) + " float16=" +
                          std::to_string(device.IsFloat16Supported()) +
                          " -- all are required, and dev-settings.txt null_descriptor=off turns "
                          "the second one off").c_str());
        return;
    }"""),
    ("""    VideoCore::FrameGen::ShaderModules code;
    if (VideoCore::FrameGen::LoadShaderModules(code) != VideoCore::FrameGen::LosslessStatus::Ok) {
        return;
    }""",
     """    VideoCore::FrameGen::ShaderModules code;
    const VideoCore::FrameGen::LosslessStatus status = VideoCore::FrameGen::LoadShaderModules(code);
    if (status != VideoCore::FrameGen::LosslessStatus::Ok) {
        if (wanted)
            Eden::Report("framegen",
                         ("Lossless.dll " + VideoCore::FrameGen::GetLosslessDllPath().string() +
                          " status=" + std::to_string(static_cast<u32>(status)) +
                          " (0 ok, 1 not installed, 2 unreadable, 3 not a PE, 4 no shaders in it, "
                          "5 translation failed, 6 cache unusable)").c_str());
        return;
    }"""),
    ("""    valid = true;""",
     """    valid = true;
    if (wanted)
        Eden::Report("framegen", ("shaders ready: " + std::to_string(code.size()) + " modules").c_str());"""),
])
# Two members, so that "frames are being generated" and "this image cannot take them" are said once
# per session rather than once per process or once per frame: this object is built per renderer,
# and the renderer is built per game session.
adapt('src/video_core/renderer_vulkan/present/frame_gen.h',
      'include/video_core/renderer_vulkan/present/frame_gen.h', [
    ('    bool dumped{};', '    bool dumped{};\n    bool generation_reported{};\n    bool storage_reported{};'),
])
adapt('src/video_core/renderer_vulkan/present/frame_gen.cpp', 'vulkan_frame_gen.cpp', [
    ('#include "video_core/vulkan_common/vulkan_device.h"',
     '#include "video_core/vulkan_common/vulkan_device.h"\n#include "diagnostics.h"'),
    ("""    if (!shaders->IsValid() && Settings::values.frame_gen_type.GetValue() != Settings::FrameGenType::Fsr3) {
        unavailable = true;
        return;
    }""",
     """    if (!shaders->IsValid() && Settings::values.frame_gen_type.GetValue() != Settings::FrameGenType::Fsr3) {
        Eden::Report("framegen", "LsfgShaders is not valid; see the lines above");
        unavailable = true;
        return;
    }"""),
    ("""    if (!frame->storage_view) {
        warm_streak = 0;
        return;
    }""",
     """    if (!frame->storage_view) {
        if (!storage_reported) {
            storage_reported = true;
            Eden::Report("framegen",
                         "the frame image has no storage view: the swapchain format lacks "
                         "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, so frames cannot be generated");
        }
        warm_streak = 0;
        return;
    }"""),
    # The gates above only say why it is not running; this says that it is.
    ("""    generated = warm && warm_streak >= LSFG_RECURRENCE_FRAMES && plan.generations > 0;""",
     """    generated = warm && warm_streak >= LSFG_RECURRENCE_FRAMES && plan.generations > 0;
    if (generated && !generation_reported) {
        generation_reported = true;
        Eden::Report("framegen",
                     ("generating " + std::to_string(plan.generations) +
                      " extra frame(s) per guest frame").c_str());
    }"""),
])
# Split the dispatch into its three phases (Tegra::Control::Scheduler::Push): the channel lock,
# queueing the submit list, and executing the guest's methods. The dispatch timer wraps the whole
# call from gpu_thread.cpp, so these say which of the three it is spent in.
adapt('src/video_core/control/scheduler.cpp', 'control_scheduler.cpp', [
    ('#include <memory>', '#include <memory>\n#include "performance.h"'),
    ('        std::unique_lock lk(scheduling_guard);\n'
     '        auto it = channels.find(channel);\n'
     '        ASSERT(it != channels.end());\n'
     '        channel_state = it->second;\n'
     '    }\n'
     '    BindChannel(gpu, channel_state->bind_id);\n'
     '    // Process commands outside the lock to reduce contention.\n'
     '    // Multiple channels can prepare their commands in parallel.\n'
     '    channel_state->payload->dma_pusher.Push(std::move(entries));\n'
     '    channel_state->payload->dma_pusher.DispatchCalls();',
     '        const ::Eden::Performance::Timer sched_lock_timer(::Eden::Performance::gpu_sched_lock);\n'
     '        std::unique_lock lk(scheduling_guard);\n'
     '        auto it = channels.find(channel);\n'
     '        ASSERT(it != channels.end());\n'
     '        channel_state = it->second;\n'
     '    }\n'
     '    BindChannel(gpu, channel_state->bind_id);\n'
     '    // Process commands outside the lock to reduce contention.\n'
     '    // Multiple channels can prepare their commands in parallel.\n'
     '    {\n'
     '        const ::Eden::Performance::Timer dma_push_timer(::Eden::Performance::gpu_dma_push);\n'
     '        channel_state->payload->dma_pusher.Push(std::move(entries));\n'
     '    }\n'
     '    {\n'
     '        const ::Eden::Performance::Timer dma_dispatch_timer(::Eden::Performance::gpu_dma_dispatch);\n'
     '        channel_state->payload->dma_pusher.DispatchCalls();\n'
     '    }'),
])
shader_header = '#pragma once\n#include <cstdint>\n'
for stage in ('vert', 'frag'):
    binary = output / f'vulkan_hud.{stage}.spv'
    subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.0',
                    '-o', str(binary), str(port / f'vulkan_hud.{stage}')], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(binary)], check=True)
    data = binary.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    shader_header += f'inline constexpr uint32_t EDEN_HUD_{stage.upper()}_SPV[] = {{\n'
    shader_header += ','.join(hex(word) for word in words) + '\n};\n'
path = output / 'vulkan_hud_shaders.h'
if not path.exists() or path.read_text() != shader_header:
    path.write_text(shader_header)

if SKIPPED:
    print(f'VK-PORT-SKIP total: {len(SKIPPED)} edits skipped', file=sys.stderr)
