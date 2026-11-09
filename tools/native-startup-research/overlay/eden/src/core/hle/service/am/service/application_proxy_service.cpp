// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/service/application_proxy.h"
#include "core/hle/service/am/service/application_proxy_service.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::AM {

IApplicationProxyService::IApplicationProxyService(Core::System& system_,
                                                   WindowSystem& window_system)
    : ServiceFramework{system_, "appletOE"}, m_window_system{window_system} {
    static const FunctionInfo functions[] = {
        {0, D<&IApplicationProxyService::OpenApplicationProxy>, "OpenApplicationProxy"},
        {10, D<&IApplicationProxyService::OpenApplicationProxyWithFlags>,
         "OpenApplicationProxyWithFlags"},
    };
    RegisterHandlers(functions);
}

IApplicationProxyService::~IApplicationProxyService() = default;

Result IApplicationProxyService::OpenApplicationProxy(
    Out<SharedPointer<IApplicationProxy>> out_application_proxy, ClientProcessId pid,
    InCopyHandle<Kernel::KProcess> process_handle) {
    LOG_DEBUG(Service_AM, "called");

    if (const auto applet = this->GetAppletFromProcessId(pid)) {
        *out_application_proxy = std::make_shared<IApplicationProxy>(
            system, applet, process_handle.Get(), m_window_system);
        R_SUCCEED();
    } else {
        UNIMPLEMENTED();
        R_THROW(ResultUnknown);
    }
}

Result IApplicationProxyService::OpenApplicationProxyWithFlags(
    Out<SharedPointer<IApplicationProxy>> out_application_proxy, u32 flags, ClientProcessId pid,
    InCopyHandle<Kernel::KProcess> process_handle) {
    LOG_DEBUG(Service_AM, "called, flags={:#x}", flags);
    if (flags == 0) {
        R_RETURN(OpenApplicationProxy(out_application_proxy, pid, process_handle));
    }
    // Observed attribute 2 is an experimental native-client proxy variant.
    // Its full retail semantics are not asserted. Preserve it on the real proxy,
    // restrict to the verified research process, and expose only implemented
    // interfaces. Missing subsequent commands still fail, never dummy-success.
    auto* const process = process_handle.Get();
    R_UNLESS(flags == 2 && process && process->GetProgramId() == 0x0400c3f00006e000ULL &&
                 process->GetProcessId() == pid.pid &&
                 process->GetPageTable().HasResearchRemoteRegion(), ResultUnknown);
    const auto applet = this->GetAppletFromProcessId(pid);
    R_UNLESS(applet, ResultUnknown);
    auto proxy = std::make_shared<IApplicationProxy>(system, applet, process, m_window_system, flags);
    LOG_DEBUG(Service_AM, "created real application proxy: attribute={}, pid={}",
              proxy->CreationAttribute(), pid.pid);
    *out_application_proxy = std::move(proxy);
    R_SUCCEED();

}

std::shared_ptr<Applet> IApplicationProxyService::GetAppletFromProcessId(ProcessId process_id) {
    return m_window_system.GetByAppletResourceUserId(process_id.pid);
}

} // namespace Service::AM
