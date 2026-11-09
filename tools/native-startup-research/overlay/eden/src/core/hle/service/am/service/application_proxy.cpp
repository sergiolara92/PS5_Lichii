// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/am/service/applet_common_functions.h"
#include "core/hle/service/am/service/application_functions.h"
#include "core/hle/service/am/service/application_proxy.h"
#include "core/hle/service/am/service/audio_controller.h"
#include "core/hle/service/am/service/common_state_getter.h"
#include "core/hle/service/am/service/debug_functions.h"
#include "core/hle/service/am/service/display_controller.h"
#include "core/hle/service/am/service/library_applet_creator.h"
#include "core/hle/service/am/service/process_winding_controller.h"
#include "core/hle/service/am/service/self_controller.h"
#include "core/hle/service/am/service/window_controller.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::AM {

// Explicit, fixed research HLE policy, not a claim about retail GPU quotas.
// The actual SDK GetGraphicsScalingParameters returns three u32s; its scale
// conversion accepts 10/20/30. SetScreenRatioForDebug takes two unsigned values.
// Choose legal scale 10 (public enum 0) with unmodified display ratio 1:1.
// This policy is only exposed by the guarded verified-process attribute2 proxy.
// Dynamic quota changes/debug mutations and retail scheduling are unsupported.
struct GraphicsScalingParameters {
    u32 scale;
    u32 ratio_numerator;
    u32 ratio_denominator;
};
static_assert(sizeof(GraphicsScalingParameters) == 12);
static_assert(offsetof(GraphicsScalingParameters, ratio_numerator) == 4);
static_assert(offsetof(GraphicsScalingParameters, ratio_denominator) == 8);
struct ResearchGraphicsState {
    std::mutex lock;
    const GraphicsScalingParameters fixed_parameters{10, 1, 1};
    bool notification_enabled{};
};

class IGraphicsScalingParametersFunctions final
    : public ServiceFramework<IGraphicsScalingParametersFunctions> {
public:
    IGraphicsScalingParametersFunctions(Core::System& system_,
                                        std::shared_ptr<ResearchGraphicsState> state)
        : ServiceFramework{system_, "IGraphicsScalingParametersFunctions"}, m_state{std::move(state)} {
        static const FunctionInfo functions[] = {
            {10, D<&IGraphicsScalingParametersFunctions::GetParameters>, "GetGraphicsScalingParameters"},
            {1000, nullptr, "SetSupportingGraphicsScalingParametersForDebug"},
            {1010, nullptr, "SetGpuQuotaForDebug"},
            {1020, nullptr, "ClearGpuQuotaForDebug"},
            {1030, nullptr, "SetScreenRatioForDebug"},
            {1040, nullptr, "ClearScreenRatioForDebug"},
            {1050, D<&IGraphicsScalingParametersFunctions::SetNotificationEnabled>,
             "SetGraphicsScaleChangedNotificationEnabled"},
        };
        RegisterHandlers(functions);
    }
private:
    Result GetParameters(Out<GraphicsScalingParameters> out_parameters) {
        std::scoped_lock lk{m_state->lock};
        *out_parameters = m_state->fixed_parameters;
        LOG_INFO(Service_AM, "research fixed graphics policy: scale={}, ratio={}/{}; retail quotas unmodelled",
                 m_state->fixed_parameters.scale, m_state->fixed_parameters.ratio_numerator,
                 m_state->fixed_parameters.ratio_denominator);
        R_SUCCEED();
    }
    Result SetNotificationEnabled(bool enabled) {
        // Persist the subscription. The fixed policy has no transitions, hence
        // no change notifications to manufacture. Debug mutation stays absent.
        std::scoped_lock lk{m_state->lock};
        m_state->notification_enabled = enabled;
        LOG_INFO(Service_AM, "research fixed graphics policy notification subscription={}", enabled);
        R_SUCCEED();
    }
    const std::shared_ptr<ResearchGraphicsState> m_state;
};

class IPerformanceFunctions final : public ServiceFramework<IPerformanceFunctions> {
public:
    explicit IPerformanceFunctions(Core::System& system_)
        : ServiceFramework{system_, "IPerformanceFunctions"} {
        static const FunctionInfo functions[] = {
            {0, D<&IPerformanceFunctions::IsCpuOverclockEnabled>, "IsCpuOverclockEnabled"},
            {1, D<&IPerformanceFunctions::SetCpuOverclockEnabled>, "SetCpuOverclockEnabled"},
        };
        RegisterHandlers(functions);
    }
private:
    Result IsCpuOverclockEnabled(Out<bool> out_enabled) {
        // This research model does not implement an overclocked CPU mode.
        *out_enabled = false;
        LOG_INFO(Service_AM, "research CPU overclock supported=false enabled=false");
        R_SUCCEED();
    }
    Result SetCpuOverclockEnabled(bool enabled) {
        // Never accept an enable request without implementing a clock change.
        LOG_INFO(Service_AM, "research CPU overclock request={}; enable unsupported", enabled);
        R_UNLESS(!enabled, ResultUnknown);
        R_SUCCEED();
    }
};

IApplicationProxy::IApplicationProxy(Core::System& system_, std::shared_ptr<Applet> applet,
                                     Kernel::KProcess* process, WindowSystem& window_system,
                                     u32 creation_attribute)
    : ServiceFramework{system_, "IApplicationProxy"},
      m_window_system{window_system}, m_process{process}, m_applet{std::move(applet)},
      m_creation_attribute{creation_attribute}, m_graphics_state{std::make_shared<ResearchGraphicsState>()} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IApplicationProxy::GetCommonStateGetter>, "GetCommonStateGetter"},
        {1, D<&IApplicationProxy::GetSelfController>, "GetSelfController"},
        {2, D<&IApplicationProxy::GetWindowController>, "GetWindowController"},
        {3, D<&IApplicationProxy::GetAudioController>, "GetAudioController"},
        {4, D<&IApplicationProxy::GetDisplayController>, "GetDisplayController"},
        {10, D<&IApplicationProxy::GetProcessWindingController>, "GetProcessWindingController"},
        {11, D<&IApplicationProxy::GetLibraryAppletCreator>, "GetLibraryAppletCreator"},
        {20, D<&IApplicationProxy::GetApplicationFunctions>, "GetApplicationFunctions"},
        {40, D<&IApplicationProxy::GetPerformanceFunctions>, "GetPerformanceFunctions"},
        {60, D<&IApplicationProxy::GetGraphicsScalingParametersFunctions>, "GetGraphicsScalingParametersFunctions"},
        {1000, D<&IApplicationProxy::GetDebugFunctions>, "GetDebugFunctions"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IApplicationProxy::~IApplicationProxy() = default;

Result IApplicationProxy::GetAudioController(
    Out<SharedPointer<IAudioController>> out_audio_controller) {
    LOG_DEBUG(Service_AM, "called");
    *out_audio_controller = std::make_shared<IAudioController>(system);
    R_SUCCEED();
}

Result IApplicationProxy::GetDisplayController(
    Out<SharedPointer<IDisplayController>> out_display_controller) {
    LOG_DEBUG(Service_AM, "called");
    *out_display_controller = std::make_shared<IDisplayController>(system, m_applet);
    R_SUCCEED();
}

Result IApplicationProxy::GetProcessWindingController(
    Out<SharedPointer<IProcessWindingController>> out_process_winding_controller) {
    LOG_DEBUG(Service_AM, "called");
    *out_process_winding_controller = std::make_shared<IProcessWindingController>(system, m_applet);
    R_SUCCEED();
}

Result IApplicationProxy::GetDebugFunctions(
    Out<SharedPointer<IDebugFunctions>> out_debug_functions) {
    LOG_DEBUG(Service_AM, "called");
    *out_debug_functions = std::make_shared<IDebugFunctions>(system);
    R_SUCCEED();
}

Result IApplicationProxy::GetWindowController(
    Out<SharedPointer<IWindowController>> out_window_controller) {
    LOG_DEBUG(Service_AM, "called");
    *out_window_controller = std::make_shared<IWindowController>(system, m_applet, m_window_system);
    R_SUCCEED();
}

Result IApplicationProxy::GetSelfController(
    Out<SharedPointer<ISelfController>> out_self_controller) {
    LOG_DEBUG(Service_AM, "called");
    *out_self_controller = std::make_shared<ISelfController>(system, m_applet, m_process);
    R_SUCCEED();
}

Result IApplicationProxy::GetCommonStateGetter(
    Out<SharedPointer<ICommonStateGetter>> out_common_state_getter) {
    LOG_DEBUG(Service_AM, "called");
    *out_common_state_getter = std::make_shared<ICommonStateGetter>(system, m_applet);
    R_SUCCEED();
}

Result IApplicationProxy::GetLibraryAppletCreator(
    Out<SharedPointer<ILibraryAppletCreator>> out_library_applet_creator) {
    LOG_DEBUG(Service_AM, "called");
    *out_library_applet_creator =
        std::make_shared<ILibraryAppletCreator>(system, m_applet, m_window_system);
    R_SUCCEED();
}

Result IApplicationProxy::GetApplicationFunctions(
    Out<SharedPointer<IApplicationFunctions>> out_application_functions) {
    LOG_DEBUG(Service_AM, "called");
    *out_application_functions = std::make_shared<IApplicationFunctions>(system, m_applet);
    R_SUCCEED();
}

Result IApplicationProxy::GetGraphicsScalingParametersFunctions(
    Out<SharedPointer<IGraphicsScalingParametersFunctions>> out_functions) {
    // The observed native-client variant is only available to the verified
    // research process. This does not grant absent parameter/debug operations.
    R_UNLESS(m_creation_attribute == 2 && m_applet && m_process &&
                 m_process->GetProgramId() == 0x0400c3f00006e000ULL &&
                 m_process->GetPageTable().HasResearchRemoteRegion(), ResultUnknown);
    *out_functions = std::make_shared<IGraphicsScalingParametersFunctions>(system, m_graphics_state);
    LOG_INFO(Service_AM, "created graphics-scaling research interface; explicit fixed policy");
    R_SUCCEED();
}

Result IApplicationProxy::GetPerformanceFunctions(
    Out<SharedPointer<IPerformanceFunctions>> out_functions) {
    R_UNLESS(m_creation_attribute == 2 && m_applet && m_process &&
                 m_process->GetProgramId() == 0x0400c3f00006e000ULL &&
                 m_process->GetPageTable().HasResearchRemoteRegion(), ResultUnknown);
    *out_functions = std::make_shared<IPerformanceFunctions>(system);
    LOG_INFO(Service_AM, "created research performance interface; overclock unsupported");
    R_SUCCEED();
}

} // namespace Service::AM
