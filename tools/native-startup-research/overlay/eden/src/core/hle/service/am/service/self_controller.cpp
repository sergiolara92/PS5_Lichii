// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging.h"
#include "core/hle/result.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/am/service/self_controller.h"
#include "core/hle/service/caps/caps_su.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/nvnflinger/nvnflinger.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/vi/vi_results.h"

#ifdef PROSPERO_GRAPHICS_SUPERVISOR_PROBE
#include <cstdio>
#include <atomic>
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/am/service/research_graphics_bootstrap.h"
#include "core/hle/service/am/service/research_gsv_syncpoint_context.h"
#include "core/hle/service/am/service/research_native_memory.h"
#endif

namespace Service::AM {


#ifdef PROSPERO_GRAPHICS_SUPERVISOR_PROBE
// Host research subset: actual per-client GSV lifecycle only. GPU, display,
// mapping and synchronization services remain explicitly unsupported.
class GraphicsSupervisorRequestProbe final : public ServiceFramework<GraphicsSupervisorRequestProbe> {
public:
    GraphicsSupervisorRequestProbe(Core::System& system_, Kernel::KProcess* process, u64 aruid)
        : ServiceFramework{system_, "IGraphicsSupervisorResearchBootstrap"}, process{process}, aruid{aruid}, state{aruid} {
        process->Open(kernel);
    }
    ~GraphicsSupervisorRequestProbe() override { process->Close(kernel); }
    Result HandleSyncRequest(Kernel::KServerSession&, HLERequestContext& ctx) override {
        const auto guard = LockService();
        const auto command = ctx.GetCommand();
        const auto phase_before = state.GetPhase();
        if (command == 1402) return OpenBoundNvDriver(ctx);
        if (command == 201 || command == 202) return HandleSyncpointContext(ctx);
        if (command == 115 || command == 111 || command == 112 || command == 114)
            return HandleNativeMemory(ctx);
        if (command == 1101 || command == 1102 || command == 1111 || command == 1112) return HandleFrameContext(ctx);
        Result result = Kernel::ResultInvalidCombination;
        if (ctx.IsTipc()) {
            bool process_matches = false;
            if (command == 4 || command == 11) {
                const auto supplied = ctx.GetObjectFromHandle<Kernel::KProcess>(ctx.CommandBuffer()[3]);
                process_matches = supplied.IsNotNull() && supplied.GetPointerUnsafe() == process;
            }
            bool immutable_reservation = false;
            if (command == 4 && process->GetPageTable().HasResearchRemoteRegion()) {
                const auto& table = process->GetPageTable();
                Kernel::KMemoryInfo info{};
                Kernel::Svc::PageInfo page{};
                immutable_reservation = table.QueryInfo(&info, &page, table.GetResearchRemoteRegionStart()).IsSuccess() &&
                    info.m_state == Kernel::KMemoryState::ResearchRemoteReserved &&
                    info.m_address == GetInteger(table.GetResearchRemoteRegionStart()) &&
                    info.m_size == table.GetResearchRemoteRegionSize() &&
                    info.m_permission == Kernel::KMemoryPermission::None &&
                    info.m_attribute == Kernel::KMemoryAttribute::None &&
                    info.m_ipc_lock_count == 0 && info.m_device_use_count == 0;
            }
            using ResearchGraphicsBootstrap::Status;
            const auto status = state.Apply(command,
                std::span<const u32>{ctx.CommandBuffer(), IPC::COMMAND_BUFFER_LENGTH},
                ctx.GetThread().GetOwnerProcess() == process, process_matches, immutable_reservation);
            switch (status) {
            case Status::Success: result = ResultSuccess; break;
            case Status::InvalidPacket: result = Kernel::ResultInvalidCombination; break;
            case Status::InvalidState: result = Kernel::ResultInvalidState; break;
            case Status::InvalidProcess: result = Kernel::ResultInvalidHandle; break;
            case Status::InvalidAruid: result = Kernel::ResultInvalidArgument; break;
            case Status::Unsupported: result = Kernel::ResultNotImplemented; break;
            }
        }
        const bool report = logged++ < 16;
        if (report) {
            std::fprintf(stderr, "GRAPHICS_BOOTSTRAP command=%u tipc=%u pid=%llu phase=%u->%u result=%08x gpu_implemented=0 request=",
                command, static_cast<unsigned>(ctx.IsTipc()), static_cast<unsigned long long>(process->GetProcessId()),
                static_cast<unsigned>(phase_before), static_cast<unsigned>(state.GetPhase()), result.raw);
            for (unsigned i = 0; i < 16; ++i) std::fprintf(stderr, "%08x%s", ctx.CommandBuffer()[i], i == 15 ? "\n" : ",");
        }
        { IPC::ResponseBuilder rb{ctx, 2}; rb.Push(result.raw); }
        if (report) {
            std::fprintf(stderr, "GRAPHICS_BOOTSTRAP reply=%08x,%08x,%08x service_result=%08x\n",
                         ctx.CommandBuffer()[0], ctx.CommandBuffer()[1], ctx.CommandBuffer()[2], result.raw);
            std::fflush(stderr);
        }
        return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
    }
private:
    Result HandleNativeMemory(HLERequestContext& ctx) {
        const auto command=ctx.GetCommand();const auto* p=ctx.CommandBuffer();
        Result result=Kernel::ResultInvalidCombination;u64 selected{};u32 id{};
        const auto read64=[&](unsigned i){return u64{p[i]}|(u64{p[i+1]}<<32);};
        if(ctx.GetThread().GetOwnerProcess()!=process)result=Kernel::ResultInvalidHandle;
        else if(state.GetPhase()!=ResearchGraphicsBootstrap::Phase::ContextCreated)result=Kernel::ResultInvalidState;
        else if(!ctx.IsTipc() || p[0]!=16+command)result=Kernel::ResultInvalidCombination;
        else {
            try {
                if(command==115) {
                    if(p[1]!=2)result=Kernel::ResultInvalidCombination;
                    else if(native_memory)result=Kernel::ResultInvalidState;
                    else if(driver_session.expired())result=Kernel::ResultInvalidState;
                    else {
                        auto source=system.ServiceManager().GetService<Nvidia::NVDRV>("nvdrv:s",true);
                        if(!source || !source->GetModule())result=Kernel::ResultNotFound;
                        else {
                            auto candidate=std::make_shared<ResearchNativeMemory::Context>(system,process,source->GetModule());
                            result=candidate->Boot(read64(2),&selected);
                            if(result.IsSuccess())native_memory=std::move(candidate);
                        }
                    }
                } else if(!native_memory)result=Kernel::ResultInvalidState;
                else if(command==111) {
                    // The original SDK writes only the permission byte, not the other TLS bytes.
                    if(p[1]!=3 || (p[2]&0xff)!=3)result=Kernel::ResultInvalidCombination;
                    else result=native_memory->Create(read64(3),&id);
                } else if(command==112) {
                    result=p[1]==1?native_memory->Destroy(p[2]):Kernel::ResultInvalidCombination;
                } else if(command==114) {
                    if(p[1]!=9)result=Kernel::ResultInvalidCombination;
                    else result=native_memory->Bind(p[2],read64(3),read64(5),read64(7),read64(9));
                }
            } catch(const std::bad_alloc&) {result=Kernel::ResultOutOfResource;}
        }
        {
            IPC::ResponseBuilder rb{ctx,command==115?4u:(command==111?3u:2u)};
            rb.Push(result.raw);
            if(command==115)rb.Push<u64>(selected);
            else if(command==111)rb.Push(id);
        }
        if(native_logged++<24) {
            std::fprintf(stderr,"GSV_NATIVE command=%u result=%08x selected_mode=%llu actual_handle=%u real_nvmap_backend=1 gpu_rendering=0\n",
                command,result.raw,static_cast<unsigned long long>(selected),id);
            std::fflush(stderr);
        }
        return system.IsPoweredOn()?ctx.WriteToOutgoingCommandBuffer():ResultSuccess;
    }
    // GSV 1101/1102, from firmware 0400000000000081 (.text sha256 15065af6...):
    //  1101 (0x8cc94): checks the TIPC header and the session permission bit,
    //       then replies with the result only. It creates no state.
    //  1102 (0x8cd1c -> 0x8d080): permission check, then under the state lock
    //       creates one 64-byte frame context (owner, graphics object ref,
    //       active flag), binds it through the driver and makes it current.
    //       A second call while a context is active returns 0x200f3. Its
    //       reply also carries the result only.
    //  1111 SetGraphicsFrameLengthMode (0x8cf08): value in {0,286,316,376}
    //       else 0x100f3. 1112 SetGraphicsFrameRestartMode (0x8cdd0): value<=2
    //       else 0x280f3. Both need the 1102 context and call the driver.
    // Not emulated here: the firmware driver calls (bind 0x8d67c, mode
    // updates 0x8ce70). Values are stored for Eden's presenter to use later.
    Result HandleFrameContext(HLERequestContext& ctx) {
        const auto command = ctx.GetCommand();
        const auto* p = ctx.CommandBuffer();
        const u32 arg = p[2];
        Result result = Kernel::ResultInvalidCombination;
        if (!ctx.IsTipc() || p[0] != 16 + command || (command < 1111 && p[1] != 0))
            result = Kernel::ResultInvalidCombination;
        else if (ctx.GetThread().GetOwnerProcess() != process)
            result = Kernel::ResultInvalidHandle;
        else if (state.GetPhase() != ResearchGraphicsBootstrap::Phase::ContextCreated)
            result = Kernel::ResultInvalidState;
        else if (command == 1101)
            result = ResultSuccess;
        else if (command == 1111 || command == 1112) {
            // Firmware 0x8cf08 / 0x8cdd0: one payload word; both need the
            // context made current by 1102 ([state+0xae8]).
            const u32 value = arg;
            if (p[1] != 1) result = Kernel::ResultInvalidCombination;
            else if (command == 1111 && value != 0 && value != 286 && value != 316 && value != 376)
                result = Result{0x100f3};
            else if (command == 1112 && value > 2) result = Result{0x280f3};
            else if (!frame_context_active) result = Kernel::ResultInvalidState;
            else {
                (command == 1111 ? frame_length_mode : frame_restart_mode) = value;
                result = ResultSuccess;
            }
        }
        else if (frame_context_active)
            result = Result{0x200f3};
        else {
            frame_context_active = true;
            ++frame_context_generation;
            result = ResultSuccess;
        }
        { IPC::ResponseBuilder rb{ctx, 2}; rb.Push(result.raw); }
        std::fprintf(stderr, "GSV_FRAME_CONTEXT command=%u arg=%08x result=%08x active=%u generation=%u length_mode=%u restart_mode=%u driver_bind_emulated=0 frame_present=0\n",
                     command, arg, result.raw, static_cast<unsigned>(frame_context_active), frame_context_generation,
                     frame_length_mode, frame_restart_mode);
        std::fflush(stderr);
        return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
    }
    Result HandleSyncpointContext(HLERequestContext& ctx) {
        const auto Finish = [&](Result result) {
            { IPC::ResponseBuilder rb{ctx, 2}; rb.Push(result.raw); }
            std::fprintf(stderr, "GSV_SYNC_CONTEXT command=%u result=%08x real_event_capacity=%zu\n",
                         ctx.GetCommand(), result.raw, sync_context ? sync_context->events.Size() : 0);
            std::fflush(stderr);
            return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
        };
        const auto command = ctx.GetCommand();
        if (!ctx.IsTipc() || ctx.CommandBuffer()[0] != 16 + command || ctx.CommandBuffer()[1] != 0)
            return Finish(Kernel::ResultInvalidCombination);
        if (ctx.GetThread().GetOwnerProcess() != process) return Finish(Kernel::ResultInvalidHandle);
        if (state.GetPhase() != ResearchGraphicsBootstrap::Phase::ContextCreated)
            return Finish(Kernel::ResultInvalidState);
        if (command == 201) {
            if (sync_context) return Finish(Kernel::ResultInvalidState);
            auto source = system.ServiceManager().GetService<Nvidia::NVDRV>("nvdrv:s", true);
            if (!source || !source->GetModule()) return Finish(Kernel::ResultNotFound);
            auto candidate = std::make_shared<ResearchSyncpoint::Context>(system, process, source->GetModule());
            if (!candidate->Initialize()) return Finish(Kernel::ResultOutOfResource);
            sync_context = std::move(candidate);
            return Finish(ResultSuccess);
        }
        if (!sync_context) return Finish(Kernel::ResultInvalidState);
        Kernel::KClientSession* client{};
        const auto result = ResearchSyncpoint::CreateSession(system, ctx, sync_context, &client);
        if (result.IsError()) return Finish(result);
        { IPC::ResponseBuilder rb{ctx, 2, 0, 1}; rb.Push(ResultSuccess.raw); rb.PushMoveObjects(ctx, client); }
        std::fprintf(stderr, "GSV_SYNC_CONTEXT command=202 opened=1 kernel_events=%zu no_fences_signalled=1\n", sync_context->events.Size());
        std::fflush(stderr);
        return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
    }
    Result OpenBoundNvDriver(HLERequestContext& ctx) {
        const auto Fail = [&](Result result) {
            { IPC::ResponseBuilder rb{ctx, 2}; rb.Push(result.raw); }
            std::fprintf(stderr, "GSV_NVDRV result=%08x opened=0\n", result.raw);
            std::fflush(stderr);
            return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
        };
        if (!ctx.IsTipc() || ctx.CommandBuffer()[0] != 0x58a || ctx.CommandBuffer()[1] != 0)
            return Fail(Kernel::ResultInvalidCombination);
        if (ctx.GetThread().GetOwnerProcess() != process) return Fail(Kernel::ResultInvalidHandle);
        if (state.GetPhase() != ResearchGraphicsBootstrap::Phase::ContextCreated)
            return Fail(Kernel::ResultInvalidState);
        // A real open session is allowed once; repeat attempts are rejected
        // until its handler is actually destroyed after the client closes it.
        if (!driver_session.expired()) return Fail(Result{0x200f3});
        auto* owner = Kernel::GetCurrentProcessPointer(kernel);
        if (!owner) return Fail(Kernel::ResultInvalidState);
        auto source = system.ServiceManager().GetService<Nvidia::NVDRV>("nvdrv:s", true);
        if (!source || !source->GetModule()) return Fail(Kernel::ResultNotFound);
        Kernel::KScopedResourceReservation reservation(kernel, owner, Kernel::LimitableResource::SessionCountMax);
        if (!reservation.Succeeded()) return Fail(Kernel::ResultLimitReached);
        auto* session = Kernel::KSession::Create(kernel);
        if (!session) return Fail(Kernel::ResultOutOfResource);
        session->Initialize(kernel, nullptr, 0);
        reservation.Commit();
        Kernel::KSession::Register(kernel, session);
        auto rollback = SCOPE_GUARD {
            session->GetClientSession().Close(kernel);
            session->GetServerSession().Close(kernel);
        };
        auto handler = std::make_shared<Nvidia::NVDRV>(system, source->GetModule(), "nvdrv:gsv-research", process, aruid);
        auto& server = ctx.GetManager()->GetServerManager();
        auto manager = std::make_shared<SessionRequestManager>(kernel, server);
        manager->SetSessionHandler(handler);
        const auto registered = server.RegisterSession(&session->GetServerSession(), std::move(manager));
        if (registered.IsError()) return Fail(registered);
        driver_session = handler;
        rollback.Cancel();
        { IPC::ResponseBuilder rb{ctx, 2, 0, 1}; rb.Push(ResultSuccess.raw); rb.PushMoveObjects(ctx, &session->GetClientSession()); }
        std::fprintf(stderr, "GSV_NVDRV opened=1 pid=%llu real_container_session=1 ampere_support=unproven\n",
                     static_cast<unsigned long long>(process->GetProcessId()));
        std::fflush(stderr);
        return system.IsPoweredOn() ? ctx.WriteToOutgoingCommandBuffer() : ResultSuccess;
    }
    std::shared_ptr<ResearchNativeMemory::Context> native_memory;
    unsigned native_logged{};
    std::shared_ptr<ResearchSyncpoint::Context> sync_context;
    std::weak_ptr<Nvidia::NVDRV> driver_session;
    bool frame_context_active{};
    unsigned frame_context_generation{};
    u32 frame_length_mode{};
    u32 frame_restart_mode{};
    Kernel::KProcess* process;
    const u64 aruid;
    ResearchGraphicsBootstrap::State state;
    unsigned logged{};
};
#endif

ISelfController::ISelfController(Core::System& system_, std::shared_ptr<Applet> applet,
                                 Kernel::KProcess* process)
    : ServiceFramework{system_, "ISelfController"}, m_process{process}, m_applet{
                                                                            std::move(applet)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&ISelfController::Exit>, "Exit"},
        {1, D<&ISelfController::LockExit>, "LockExit"},
        {2, D<&ISelfController::UnlockExit>, "UnlockExit"},
        {3, D<&ISelfController::EnterFatalSection>, "EnterFatalSection"},
        {4, D<&ISelfController::LeaveFatalSection>, "LeaveFatalSection"},
        {9, D<&ISelfController::GetLibraryAppletLaunchableEvent>, "GetLibraryAppletLaunchableEvent"},
        {10, D<&ISelfController::SetScreenShotPermission>, "SetScreenShotPermission"},
        {11, D<&ISelfController::SetOperationModeChangedNotification>, "SetOperationModeChangedNotification"},
        {12, D<&ISelfController::SetPerformanceModeChangedNotification>, "SetPerformanceModeChangedNotification"},
        {13, D<&ISelfController::SetFocusHandlingMode>, "SetFocusHandlingMode"},
        {14, D<&ISelfController::SetRestartMessageEnabled>, "SetRestartMessageEnabled"},
        {15, D<&ISelfController::SetScreenShotAppletIdentityInfo>, "SetScreenShotAppletIdentityInfo"},
        {16, D<&ISelfController::SetOutOfFocusSuspendingEnabled>, "SetOutOfFocusSuspendingEnabled"},
        {17, nullptr, "SetControllerFirmwareUpdateSection"},
        {18, nullptr, "SetRequiresCaptureButtonShortPressedMessage"},
        {19, D<&ISelfController::SetAlbumImageOrientation>, "SetAlbumImageOrientation"},
        {20, nullptr, "SetDesirableKeyboardLayout"},
        {21, nullptr, "GetScreenShotProgramId"},
        {40, D<&ISelfController::CreateManagedDisplayLayer>, "CreateManagedDisplayLayer"},
        {41, D<&ISelfController::IsSystemBufferSharingEnabled>, "IsSystemBufferSharingEnabled"},
        {42, D<&ISelfController::GetSystemSharedLayerHandle>, "GetSystemSharedLayerHandle"},
        {43, D<&ISelfController::GetSystemSharedBufferHandle>, "GetSystemSharedBufferHandle"},
        {44, D<&ISelfController::CreateManagedDisplaySeparableLayer>, "CreateManagedDisplaySeparableLayer"},
        {45, nullptr, "SetManagedDisplayLayerSeparationMode"},
        {46, nullptr, "SetRecordingLayerCompositionEnabled"},
        {50, D<&ISelfController::SetHandlesRequestToDisplay>, "SetHandlesRequestToDisplay"},
        {51, D<&ISelfController::ApproveToDisplay>, "ApproveToDisplay"},
        {60, D<&ISelfController::OverrideAutoSleepTimeAndDimmingTime>, "OverrideAutoSleepTimeAndDimmingTime"},
        {61, D<&ISelfController::SetMediaPlaybackState>, "SetMediaPlaybackState"},
        {62, D<&ISelfController::SetIdleTimeDetectionExtension>, "SetIdleTimeDetectionExtension"},
        {63, D<&ISelfController::GetIdleTimeDetectionExtension>, "GetIdleTimeDetectionExtension"},
        {64, nullptr, "SetInputDetectionSourceSet"},
        {65, D<&ISelfController::ReportUserIsActive>, "ReportUserIsActive"},
        {66, nullptr, "GetCurrentIlluminance"},
        {67, D<&ISelfController::IsIlluminanceAvailable>, "IsIlluminanceAvailable"},
        {68, D<&ISelfController::SetAutoSleepDisabled>, "SetAutoSleepDisabled"},
        {69, D<&ISelfController::IsAutoSleepDisabled>, "IsAutoSleepDisabled"},
        {70, nullptr, "ReportMultimediaError"},
        {71, nullptr, "GetCurrentIlluminanceEx"},
        {72, D<&ISelfController::SetInputDetectionPolicy>, "SetInputDetectionPolicy"},
        {80, nullptr, "SetWirelessPriorityMode"},
        {90, D<&ISelfController::GetAccumulatedSuspendedTickValue>, "GetAccumulatedSuspendedTickValue"},
        {91, D<&ISelfController::GetAccumulatedSuspendedTickChangedEvent>, "GetAccumulatedSuspendedTickChangedEvent"},
        {100, D<&ISelfController::SetAlbumImageTakenNotificationEnabled>, "SetAlbumImageTakenNotificationEnabled"},
        {110, nullptr, "SetApplicationAlbumUserData"},
        {120, D<&ISelfController::SaveCurrentScreenshot>, "SaveCurrentScreenshot"},
        {130, D<&ISelfController::SetRecordVolumeMuted>, "SetRecordVolumeMuted"},
        {230, D<&ISelfController::Unknown230>, "Unknown230"},
        {240, D<&ISelfController::Unknown240>, "Unknown240"},
        {1000, nullptr, "GetDebugStorageChannel"},
#ifdef PROSPERO_GRAPHICS_SUPERVISOR_PROBE
        {2000, &ISelfController::GetGraphicsSupervisorProbe, "GetGraphicsSupervisorProbe"},
#endif
    };
    // clang-format on

    RegisterHandlers(functions);

    std::scoped_lock lk{m_applet->lock};
    m_applet->display_layer_manager.Initialize(system, m_process, m_applet->applet_id,
                                               m_applet->library_applet_mode);
}

ISelfController::~ISelfController() {
#ifdef PROSPERO_GRAPHICS_SUPERVISOR_PROBE
    if (m_graphics_probe_client) m_graphics_probe_client->Close(kernel);
#endif
    std::scoped_lock lk{m_applet->lock};
    m_applet->display_layer_manager.Finalize();
}


#ifdef PROSPERO_GRAPHICS_SUPERVISOR_PROBE
void ISelfController::GetGraphicsSupervisorProbe(HLERequestContext& ctx) {
    static std::atomic<unsigned> observations{};
    if (observations.fetch_add(1) < 4) {
        const auto* current = Kernel::GetCurrentProcessPointer(kernel);
        std::fprintf(stderr, "GRAPHICS_REQUEST_PROBE caller_guard program=%016llx pid=%llu remote=%u current_pid=%llu same_process=%u\n",
                     static_cast<unsigned long long>(m_process ? m_process->GetProgramId() : 0),
                     static_cast<unsigned long long>(m_process ? m_process->GetProcessId() : 0),
                     static_cast<unsigned>(m_process && m_process->GetPageTable().HasResearchRemoteRegion()),
                     static_cast<unsigned long long>(current ? current->GetProcessId() : 0),
                     static_cast<unsigned>(current == m_process));
        std::fflush(stderr);
    }
    const auto ReplyError = [&](Result result) {
        IPC::ResponseBuilder rb{ctx, 2}; rb.Push(result);
    };
    if (!m_process || m_process->GetProgramId() != 0x0400c3f00006e000ULL ||
        !m_process->GetPageTable().HasResearchRemoteRegion()) {
        ReplyError(Kernel::ResultNotImplemented); return;
    }
    // The HLE server runs as its own system process. Validate the actual
    // originating thread, never compare the server process with the client.
    if (ctx.GetThread().GetOwnerProcess() != m_process) {
        ReplyError(Kernel::ResultInvalidState); return;
    }
    auto* session_owner = Kernel::GetCurrentProcessPointer(kernel);
    if (!session_owner) { ReplyError(Kernel::ResultInvalidState); return; }
    if (!m_graphics_probe_client) {
        // KSession::Initialize charges and retains this server-side owner.
        // Reserve the same owner's real limit so PostDestroy releases it.
        Kernel::KScopedResourceReservation reservation(kernel, session_owner, Kernel::LimitableResource::SessionCountMax);
        if (!reservation.Succeeded()) { ReplyError(Kernel::ResultLimitReached); return; }
        auto* session = Kernel::KSession::Create(kernel);
        if (!session) { ReplyError(Kernel::ResultOutOfResource); return; }
        session->Initialize(kernel, nullptr, 0);
        reservation.Commit();
        Kernel::KSession::Register(kernel, session);
        auto rollback = SCOPE_GUARD {
            session->GetClientSession().Close(kernel);
            session->GetServerSession().Close(kernel);
        };
        auto& server = ctx.GetManager()->GetServerManager();
        auto manager = std::make_shared<SessionRequestManager>(kernel, server);
        manager->SetSessionHandler(std::make_shared<GraphicsSupervisorRequestProbe>(system, m_process, m_applet->aruid.pid));
        const auto result = server.RegisterSession(&session->GetServerSession(), std::move(manager));
        if (result.IsError()) { ReplyError(result); return; }
        // RegisterSession takes the existing server ref; retain the client ref.
        m_graphics_probe_client = &session->GetClientSession();
        rollback.Cancel();
        std::fprintf(stderr, "GRAPHICS_REQUEST_PROBE endpoint_created pid=%llu aruid=%llu real_session=1 graphics_implemented=0\n",
                     static_cast<unsigned long long>(m_process->GetProcessId()),
                     static_cast<unsigned long long>(m_applet->aruid.pid));
        std::fflush(stderr);
    }
    IPC::ResponseBuilder rb{ctx, 2, 1}; rb.Push(ResultSuccess); rb.PushCopyObjects(ctx, m_graphics_probe_client);
}
#endif

Result ISelfController::Exit() {
    LOG_DEBUG(Service_AM, "called");

    m_applet->process->Terminate();

    R_SUCCEED();
}

Result ISelfController::LockExit() {
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};

    if (m_applet->lifecycle_manager.GetExitRequested()) {
        // With exit already requested, ignore and terminate immediately.
        m_applet->process->Terminate();
    } else {
        // Otherwise, set exit lock state.
        m_applet->exit_locked = true;
        system.SetExitLocked(true);
    }

    R_SUCCEED();
}

Result ISelfController::UnlockExit() {
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};

    m_applet->exit_locked = false;
    system.SetExitLocked(false);

    if (m_applet->lifecycle_manager.GetExitRequested()) {
        m_applet->process->Terminate();
    }

    R_SUCCEED();
}

Result ISelfController::EnterFatalSection() {
    std::scoped_lock lk{m_applet->lock};

    m_applet->fatal_section_count++;
    LOG_DEBUG(Service_AM, "called. Num fatal sections entered: {}", m_applet->fatal_section_count);

    R_SUCCEED();
}

Result ISelfController::LeaveFatalSection() {
    LOG_DEBUG(Service_AM, "called");

    // Entry and exit of fatal sections must be balanced.
    std::scoped_lock lk{m_applet->lock};
    R_UNLESS(m_applet->fatal_section_count > 0, AM::ResultFatalSectionCountImbalance);
    m_applet->fatal_section_count--;

    R_SUCCEED();
}

Result ISelfController::GetLibraryAppletLaunchableEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    m_applet->library_applet_launchable_event.Signal(system.Kernel());
    *out_event = m_applet->library_applet_launchable_event.GetHandle();

    R_SUCCEED();
}

Result ISelfController::SetScreenShotPermission(ScreenshotPermission screen_shot_permission) {
    LOG_DEBUG(Service_AM, "called, permission={}", screen_shot_permission);

    std::scoped_lock lk{m_applet->lock};
    m_applet->screenshot_permission = screen_shot_permission;

    R_SUCCEED();
}

Result ISelfController::SetOperationModeChangedNotification(bool enabled) {
    LOG_INFO(Service_AM, "called, enabled={}", enabled);

    std::scoped_lock lk{m_applet->lock};
    m_applet->lifecycle_manager.SetOperationModeChangedNotificationEnabled(system.Kernel(), enabled);

    R_SUCCEED();
}

Result ISelfController::SetPerformanceModeChangedNotification(bool enabled) {
    LOG_INFO(Service_AM, "called, enabled={}", enabled);

    std::scoped_lock lk{m_applet->lock};
    m_applet->lifecycle_manager.SetPerformanceModeChangedNotificationEnabled(system.Kernel(), enabled);

    R_SUCCEED();
}

Result ISelfController::SetFocusHandlingMode(bool notify, bool background, bool suspend) {
    LOG_INFO(Service_AM, "called, notify={} background={} suspend={}", notify, background, suspend);

    std::scoped_lock lk{m_applet->lock};
    m_applet->lifecycle_manager.SetFocusStateChangedNotificationEnabled(system.Kernel(), notify);
    m_applet->lifecycle_manager.SetFocusHandlingMode(suspend);
    m_applet->UpdateSuspensionStateLocked(true);

    R_SUCCEED();
}

Result ISelfController::SetRestartMessageEnabled(bool enabled) {
    LOG_INFO(Service_AM, "called, enabled={}", enabled);

    std::scoped_lock lk{m_applet->lock};
    m_applet->lifecycle_manager.SetResumeNotificationEnabled(enabled);

    R_SUCCEED();
}

Result ISelfController::SetScreenShotAppletIdentityInfo(
    AppletIdentityInfo screen_shot_applet_identity_info) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->screen_shot_identity = screen_shot_applet_identity_info;

    R_SUCCEED();
}

Result ISelfController::SetOutOfFocusSuspendingEnabled(bool enabled) {
    LOG_INFO(Service_AM, "called, enabled={}", enabled);

    std::scoped_lock lk{m_applet->lock};
    m_applet->lifecycle_manager.SetOutOfFocusSuspendingEnabled(enabled);
    m_applet->UpdateSuspensionStateLocked(false);

    R_SUCCEED();
}

Result ISelfController::SetAlbumImageOrientation(
    Capture::AlbumImageOrientation album_image_orientation) {
    LOG_WARNING(Service_AM, "(STUBBED) called, orientation={}", album_image_orientation);

    std::scoped_lock lk{m_applet->lock};
    m_applet->album_image_orientation = album_image_orientation;

    R_SUCCEED();
}

Result ISelfController::IsSystemBufferSharingEnabled() {
    LOG_INFO(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    R_RETURN(m_applet->display_layer_manager.IsSystemBufferSharingEnabled());
}

Result ISelfController::GetSystemSharedBufferHandle(Out<u64> out_buffer_id) {
    LOG_INFO(Service_AM, "called");

    u64 layer_id;

    std::scoped_lock lk{m_applet->lock};
    R_RETURN(m_applet->display_layer_manager.GetSystemSharedLayerHandle(out_buffer_id, &layer_id));
}

Result ISelfController::GetSystemSharedLayerHandle(Out<u64> out_buffer_id, Out<u64> out_layer_id) {
    LOG_INFO(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    R_RETURN(
        m_applet->display_layer_manager.GetSystemSharedLayerHandle(out_buffer_id, out_layer_id));
}

Result ISelfController::CreateManagedDisplayLayer(Out<u64> out_layer_id) {
    LOG_INFO(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    R_RETURN(m_applet->display_layer_manager.CreateManagedDisplayLayer(out_layer_id));
}

Result ISelfController::CreateManagedDisplaySeparableLayer(Out<u64> out_layer_id,
                                                           Out<u64> out_recording_layer_id) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    std::scoped_lock lk{m_applet->lock};
    R_RETURN(m_applet->display_layer_manager.CreateManagedDisplaySeparableLayer(
        out_layer_id, out_recording_layer_id));
}

Result ISelfController::SetHandlesRequestToDisplay(bool enable) {
    LOG_WARNING(Service_AM, "(STUBBED) called, enable={}", enable);
    R_SUCCEED();
}

Result ISelfController::ApproveToDisplay() {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    R_SUCCEED();
}

Result ISelfController::SetMediaPlaybackState(bool state) {
    LOG_WARNING(Service_AM, "(STUBBED) called, state={}", state);
    R_SUCCEED();
}

Result ISelfController::OverrideAutoSleepTimeAndDimmingTime(s32 a, s32 b, s32 c, s32 d) {
    LOG_WARNING(Service_AM, "(STUBBED) called, a={}, b={}, c={}, d={}", a, b, c, d);
    R_SUCCEED();
}

Result ISelfController::SetIdleTimeDetectionExtension(
    IdleTimeDetectionExtension idle_time_detection_extension) {
    LOG_DEBUG(Service_AM, "(STUBBED) called extension={}", idle_time_detection_extension);

    std::scoped_lock lk{m_applet->lock};
    m_applet->idle_time_detection_extension = idle_time_detection_extension;

    R_SUCCEED();
}

Result ISelfController::GetIdleTimeDetectionExtension(
    Out<IdleTimeDetectionExtension> out_idle_time_detection_extension) {
    LOG_WARNING(Service_AM, "(STUBBED) called");

    std::scoped_lock lk{m_applet->lock};
    *out_idle_time_detection_extension = m_applet->idle_time_detection_extension;

    R_SUCCEED();
}

Result ISelfController::ReportUserIsActive() {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    R_SUCCEED();
}

Result ISelfController::SetAutoSleepDisabled(bool is_auto_sleep_disabled) {
    LOG_DEBUG(Service_AM, "called. is_auto_sleep_disabled={}", is_auto_sleep_disabled);

    // On the system itself, if the previous state of is_auto_sleep_disabled
    // differed from the current value passed in, it'd signify the internal
    // window manager to update (and also increment some statistics like update counts)
    //
    // It'd also indicate this change to an idle handling context.
    //
    // However, given we're emulating this behavior, most of this can be ignored
    // and it's sufficient to simply set the member variable for querying via
    // IsAutoSleepDisabled().

    std::scoped_lock lk{m_applet->lock};
    m_applet->auto_sleep_disabled = is_auto_sleep_disabled;

    R_SUCCEED();
}

Result ISelfController::IsAutoSleepDisabled(Out<bool> out_is_auto_sleep_disabled) {
    LOG_DEBUG(Service_AM, "called.");

    std::scoped_lock lk{m_applet->lock};
    *out_is_auto_sleep_disabled = m_applet->auto_sleep_disabled;

    R_SUCCEED();
}

Result ISelfController::IsIlluminanceAvailable(Out<bool> out_is_illuminance_available) {
    LOG_WARNING(Service_AM, "(stubbed)");
    *out_is_illuminance_available = false;
    R_SUCCEED();
}

Result ISelfController::SetInputDetectionPolicy(InputDetectionPolicy input_detection_policy) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    R_SUCCEED();
}

Result ISelfController::GetAccumulatedSuspendedTickValue(
    Out<u64> out_accumulated_suspended_tick_value) {
    LOG_DEBUG(Service_AM, "called.");

    // This command returns the total number of system ticks since ISelfController creation
    // where the game was suspended. Since Yuzu doesn't implement game suspension, this command
    // can just always return 0 ticks.
    std::scoped_lock lk{m_applet->lock};
    *out_accumulated_suspended_tick_value = m_applet->suspended_ticks;

    R_SUCCEED();
}

Result ISelfController::GetAccumulatedSuspendedTickChangedEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called.");

    *out_event = m_applet->accumulated_suspended_tick_changed_event.GetHandle();
    R_SUCCEED();
}

Result ISelfController::SetAlbumImageTakenNotificationEnabled(bool enabled) {
    LOG_WARNING(Service_AM, "(STUBBED) called. enabled={}", enabled);

    // This service call sets an internal flag whether a notification is shown when an image is
    // captured. Currently we do not support capturing images via the capture button, so this can be
    // stubbed for now.
    std::scoped_lock lk{m_applet->lock};
    m_applet->album_image_taken_notification_enabled = enabled;

    R_SUCCEED();
}

Result ISelfController::SaveCurrentScreenshot(Capture::AlbumReportOption album_report_option) {
    LOG_INFO(Service_AM, "called, report_option={}", album_report_option);

    const auto screenshot_service =
        system.ServiceManager().GetService<Service::Capture::IScreenShotApplicationService>(
            "caps:su");

    if (screenshot_service) {
        screenshot_service->CaptureAndSaveScreenshot(album_report_option);
    }

    R_SUCCEED();
}

Result ISelfController::SetRecordVolumeMuted(bool muted) {
    LOG_WARNING(Service_AM, "(STUBBED) called. muted={}", muted);

    std::scoped_lock lk{m_applet->lock};
    m_applet->record_volume_muted = muted;

    R_SUCCEED();
}

Result ISelfController::Unknown230(u32 in_val, Out<u16> out_val) {
    LOG_WARNING(Service_AM, "(STUBBED) called, in_val={}", in_val);

    *out_val = 0;

    R_SUCCEED();
}

Result ISelfController::Unknown240(u32 in_val) {
    LOG_WARNING(Service_AM, "(STUBBED) called, in_val={}", in_val);

    R_SUCCEED();
}

} // namespace Service::AM
