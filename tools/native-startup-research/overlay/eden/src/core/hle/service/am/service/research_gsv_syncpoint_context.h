// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdio>
#include <memory>
#include <mutex>
#include "core/core.h"
#include "common/scope_exit.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/am/service/research_syncpoint_pool.h"
namespace Service::AM::ResearchSyncpoint {
class Context {
public:
    Context(Core::System& system, Kernel::KProcess* process, std::shared_ptr<Nvidia::Module> module)
        : owner{system.Kernel(), process}, services{system, "GSVSyncpointContext"},
          module{std::move(module)}, events{[this](Kernel::KEvent* e){services.CloseEvent(e);}} {}
    bool Initialize() {
        if (!module) return false;
        // Bind the real existing backend; do not manufacture counter values.
        (void)module->GetContainer().GetSyncpointManager();
        return events.Initialize([this]{return services.CreateEvent("GSVSyncpointEvent");});
    }
    Kernel::KProcess* Process() const { return owner.GetPointerUnsafe(); }
    std::mutex mutex;
    // Explicit research policy, not a retail capability assertion. Every
    // advertised slot is backed by a successfully allocated kernel event.
    static constexpr std::size_t EventBudget = 63;
private:
    // Declared first so the process ref outlives all resources on destruction.
    Kernel::KScopedAutoObject<Kernel::KProcess> owner;
    KernelHelpers::ServiceContext services;
    std::shared_ptr<Nvidia::Module> module;
public:
    OwnedPool<Kernel::KEvent, EventBudget> events;
};
class Session final : public ServiceFramework<Session> {
public:
    Session(Core::System& system, std::shared_ptr<Context> context)
        : ServiceFramework{system,"IGsvSyncpointResearch"},context{std::move(context)} {}
    Result HandleSyncRequest(Kernel::KServerSession&,HLERequestContext& ctx) override {
        const auto guard=LockService();
        const auto* p=ctx.CommandBuffer(); const auto cmd=ctx.GetCommand();
        const bool report=logged++<16;
        if(report){std::fprintf(stderr,"GSV_SYNC command=%u tipc=%u pid=%llu request=",cmd,static_cast<unsigned>(ctx.IsTipc()),static_cast<unsigned long long>(context->Process()->GetProcessId()));for(unsigned i=0;i<8;++i)std::fprintf(stderr,"%08x%s",p[i],i==7?"\n":",");}
        Result result=Kernel::ResultNotImplemented; u64 echo{};u32 capacity{};
        std::scoped_lock shared_guard{context->mutex};
        if(!ctx.IsTipc())result=Kernel::ResultInvalidCombination;
        else if(ctx.GetThread().GetOwnerProcess()!=context->Process())result=Kernel::ResultInvalidHandle;
        else if(cmd==0){
            if(p[0]!=0x10||p[1]!=2)result=Kernel::ResultInvalidCombination;
            else{echo=u64{p[2]}|(u64{p[3]}<<32);result=ResultSuccess;}
        }else if(cmd==1){
            if(p[0]!=0x11||p[1]!=0)result=Kernel::ResultInvalidCombination;
            else{capacity=static_cast<u32>(context->events.Size());result=ResultSuccess;}
        }else if(cmd==21){
            if(p[0]!=0x25||p[1]!=1)result=Kernel::ResultInvalidCombination;
            else result=context->events.SetActiveCount(p[2])?ResultSuccess:Kernel::ResultInvalidSize;
        }
        if(result.IsSuccess()&&cmd==0){IPC::ResponseBuilder rb{ctx,4};rb.Push(result.raw);rb.Push(echo);}
        else if(result.IsSuccess()&&cmd==1){IPC::ResponseBuilder rb{ctx,3};rb.Push(result.raw);rb.Push(capacity);}
        else{IPC::ResponseBuilder rb{ctx,2};rb.Push(result.raw);}
        if(report){std::fprintf(stderr,"GSV_SYNC result=%08x real_events=%zu active=%zu no_fence_completion_faked=1\n",result.raw,context->events.Size(),context->events.ActiveCount());std::fflush(stderr);}
        return system.IsPoweredOn()?ctx.WriteToOutgoingCommandBuffer():ResultSuccess;
    }
private:std::shared_ptr<Context> context;unsigned logged{};
};
inline Result CreateSession(Core::System& system,HLERequestContext& ctx,std::shared_ptr<Context> context,Kernel::KClientSession** out){
    auto& kernel=system.Kernel();auto* owner=Kernel::GetCurrentProcessPointer(kernel);
    if(!owner)return Kernel::ResultInvalidState;
    Kernel::KScopedResourceReservation reservation(kernel,owner,Kernel::LimitableResource::SessionCountMax);
    if(!reservation.Succeeded())return Kernel::ResultLimitReached;
    auto* session=Kernel::KSession::Create(kernel);if(!session)return Kernel::ResultOutOfResource;
    session->Initialize(kernel,nullptr,0);reservation.Commit();Kernel::KSession::Register(kernel,session);
    auto rollback=SCOPE_GUARD{session->GetClientSession().Close(kernel);session->GetServerSession().Close(kernel);};
    auto& server=ctx.GetManager()->GetServerManager();auto manager=std::make_shared<SessionRequestManager>(kernel,server);
    manager->SetSessionHandler(std::make_shared<Session>(system,std::move(context)));
    const auto result=server.RegisterSession(&session->GetServerSession(),std::move(manager));if(result.IsError())return result;
    *out=&session->GetClientSession();rollback.Cancel();return ResultSuccess;
}
}
