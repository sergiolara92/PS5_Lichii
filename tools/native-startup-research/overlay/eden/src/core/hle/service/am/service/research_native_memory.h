// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <new>
#include <algorithm>
#include "common/alignment.h"
#include "core/hle/kernel/kernel.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
namespace Service::AM::ResearchNativeMemory {
class Pin final {
public:
    Pin(Core::System& system,Kernel::KProcess* process,Kernel::KProcessAddress address,size_t size, Nvidia::NvCore::Container& container)
      : system{system},kernel{system.Kernel()},owner{kernel,process},container{container},address{address},size{size} {}
    Result Acquire() {
        R_UNLESS(!locked,Kernel::ResultInvalidState);
        bool io=true;
        R_TRY(owner->GetPageTable().LockForMapDeviceAddressSpace(&io,address,size,
            Kernel::KMemoryPermission::UserReadWrite,false,true));
        locked=true;
        R_UNLESS(!io,Kernel::ResultInvalidCurrentMemory);
        try { session=container.OpenSession(owner.GetPointerUnsafe()); opened=true; }
        catch(const std::bad_alloc&) { R_THROW(Kernel::ResultOutOfResource); }
        R_SUCCEED();
    }
    ~Pin() {
        // Core marks shutdown before destroying modules/containers. Their whole
        // address spaces are discarded there; never reenter a dying Container.
        if (!system.IsShuttingDown()) {
            if (locked) {
                const auto result=owner->GetPageTable().UnlockForDeviceAddressSpace(address,size);
                if(result.IsError()) LOG_CRITICAL(Service_AM,"Native backing guard unlock failed: {:#x}",result.raw);
            }
            if (opened) container.CloseSession(session);
        }
    }
private:
    Core::System& system;
    Kernel::KernelCore& kernel;
    Kernel::KScopedAutoObject<Kernel::KProcess> owner;
    Nvidia::NvCore::Container& container;
    Nvidia::NvCore::SessionId session{};
    bool opened{};
    Kernel::KProcessAddress address;
    size_t size;
    bool locked{};
};
class Context final {
public:
    Context(Core::System& system,Kernel::KProcess* process,std::shared_ptr<Nvidia::Module> module)
      : system{system},owner{system.Kernel(),process},module{std::move(module)} {
        if(this->module && process->GetPageTable().HasResearchRemoteRegion()) {
            session=this->module->GetContainer().OpenSession(process);
            opened=true;
        }
    }
    ~Context() {
        if(!opened)return;
        for(auto id:owned)Map().FreeHandle(id,true);
        module->GetContainer().CloseSession(session);
    }
    Result Boot(u64 requested,u64* selected) {
        std::scoped_lock lock(mutex);
        R_UNLESS(opened,Kernel::ResultInvalidState);
        R_UNLESS(!booted,Kernel::ResultInvalidState);
        // Exactly the observed SDK mode: marked normal memory, real native objects.
        R_UNLESS(requested==2,Kernel::ResultNotImplemented);
        booted=true;*selected=2;R_SUCCEED();
    }
    Result Create(u64 size,u32* out) {
        std::scoped_lock lock(mutex);
        R_UNLESS(booted,Kernel::ResultInvalidState);
        // Explicit current NvMap 32-bit pin-area limit, not a retail capacity.
        R_UNLESS(size && size<=0xfffff000ULL && Common::IsAligned(size,Kernel::PageSize),Kernel::ResultInvalidSize);
        R_UNLESS(owned.size()<1024,Kernel::ResultOutOfResource);
        std::shared_ptr<Nvidia::NvCore::NvMap::Handle> handle;
        R_UNLESS(Map().CreateHandle(size,handle,true)==Nvidia::NvResult::Success,Kernel::ResultOutOfMemory);
        { std::scoped_lock handle_lock(handle->mutex); handle->session_id=session; }
        try {owned.insert(handle->id);} catch(const std::bad_alloc&) {Map().FreeHandle(handle->id,true);R_THROW(Kernel::ResultOutOfResource);}
        *out=handle->id;R_SUCCEED();
    }
    Result Bind(u32 id,u64 address,u64 size,u64 flags,u64 offset) {
        std::scoped_lock lock(mutex);
        R_UNLESS(booted && owned.contains(id),Kernel::ResultInvalidHandle);
        R_UNLESS(flags==2 && offset==0,Kernel::ResultNotImplemented);
        auto handle=Map().GetHandle(id);
        R_UNLESS(handle,Kernel::ResultInvalidHandle);
        { std::scoped_lock handle_lock(handle->mutex);
          R_UNLESS(!handle->allocated && size==handle->size,Kernel::ResultInvalidState); }
        R_UNLESS(size && Common::IsAligned(address,Kernel::PageSize) &&
            address<=~u64{0}-size,Kernel::ResultInvalidMemoryRegion);
        auto& table=owner->GetPageTable();
        R_UNLESS(table.Contains(address,size),Kernel::ResultInvalidCurrentMemory);
        u64 current=address;const auto end=address+size;
        while(current<end) {
            Kernel::KMemoryInfo info{};Kernel::Svc::PageInfo page{};
            R_TRY(table.QueryInfo(&info,&page,current));
            constexpr auto allowed=Kernel::KMemoryAttribute::ResearchGpuSharable|Kernel::KMemoryAttribute::DeviceShared;
            R_UNLESS(info.m_state==Kernel::KMemoryState::Normal &&
                info.m_permission==Kernel::KMemoryPermission::UserReadWrite &&
                (info.m_attribute&Kernel::KMemoryAttribute::ResearchGpuSharable)!=Kernel::KMemoryAttribute::None &&
                (info.m_attribute&~allowed)==Kernel::KMemoryAttribute::None && info.m_ipc_lock_count==0,
                Kernel::ResultInvalidCurrentMemory);
            const u64 next=info.m_address+info.m_size;
            R_UNLESS(next>current,Kernel::ResultInvalidCurrentMemory);current=std::min(next,end);
        }
        std::shared_ptr<Pin> pin;
        try {pin=std::make_shared<Pin>(system,owner.GetPointerUnsafe(),Kernel::KProcessAddress{address},size,module->GetContainer());}
        catch(const std::bad_alloc&){R_THROW(Kernel::ResultOutOfResource);}
        R_TRY(pin->Acquire());
        current=address;
        while(current<end) {
            Kernel::KMemoryInfo info{};Kernel::Svc::PageInfo page{};
            R_TRY(table.QueryInfo(&info,&page,current));
            const auto required=Kernel::KMemoryAttribute::ResearchGpuSharable|Kernel::KMemoryAttribute::DeviceShared;
            R_UNLESS(info.m_state==Kernel::KMemoryState::Normal &&
                info.m_permission==Kernel::KMemoryPermission::UserReadWrite && info.m_attribute==required &&
                info.m_device_use_count>0 && info.m_ipc_lock_count==0,Kernel::ResultInvalidCurrentMemory);
            const u64 next=info.m_address+info.m_size;R_UNLESS(next>current,Kernel::ResultInvalidCurrentMemory);
            current=std::min(next,end);
        }

        Nvidia::NvCore::NvMap::Handle::Flags map_flags{};
        R_UNLESS(handle->Alloc(map_flags,Kernel::PageSize,0,address,session,std::move(pin))==Nvidia::NvResult::Success,
            Kernel::ResultInvalidState);
        R_SUCCEED();
    }
    Result Destroy(u32 id) {
        std::scoped_lock lock(mutex);
        R_UNLESS(owned.contains(id),Kernel::ResultInvalidHandle);
        R_UNLESS(Map().FreeHandle(id,true).has_value(),Kernel::ResultInvalidHandle);
        owned.erase(id);R_SUCCEED();
    }
    Nvidia::NvCore::NvMap& Map(){return module->GetContainer().GetNvMapFile();}
private:
    Core::System& system;
    Kernel::KScopedAutoObject<Kernel::KProcess> owner;
    std::shared_ptr<Nvidia::Module> module;
    Nvidia::NvCore::SessionId session{};
    std::mutex mutex;
    std::unordered_set<u32> owned;
    bool opened{},booted{};
};
}
