// SPDX-License-Identifier: GPL-3.0-or-later
// Host (Linux) stand-ins for the PS5 pad, user service and AudioOut entry points.
// Rewritten 2026-10-29: the shipped source snapshot lacked the original file.
// One connected controller at rest, silent audio paced at 48 kHz, POSIX files.
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <cstdlib>
#include "ps5_pad.hpp"
#include "native_audio.hpp"

extern "C" {

std::int32_t sceUserServiceInitialize(void*) { return 0; }
std::int32_t sceUserServiceGetInitialUser(std::int32_t* user_id) {
    if (user_id) *user_id = 1;
    return 0;
}
std::int32_t sceUserServiceTerminate() { return 0; }
int sceUserServiceGetForegroundUser(int* user) {
    if (user) *user = 1;
    return 0;
}
// Layout matches pad.cpp's LoginUserIdList: four ids, unused entries -1.
int sceUserServiceGetLoginUserIdList(void* list) {
    if (!list) return -1;
    auto* ids = static_cast<std::int32_t*>(list);
    ids[0] = 1; ids[1] = ids[2] = ids[3] = -1;
    return 0;
}

std::int32_t scePadInit() { return 0; }
std::int32_t scePadOpen(std::int32_t, std::int32_t, std::int32_t, const void*) { return 1; }
std::int32_t scePadGetHandle(std::int32_t, std::int32_t, std::int32_t) { return 1; }
std::int32_t scePadClose(std::int32_t) { return 0; }
namespace {
// EDEN_HOST_PAD="start:end:period:hold:mask[;start:end:period:hold:mask...]" (seconds from process
// start, hex or decimal mask): each segment presses `mask` for `hold` seconds every `period` seconds
// between `start` and `end`; segments add up. E.g. "125:200:3:0.3:0xc00" taps L+R while a game's
// title screen may be up. Tests on the PC only.
// Stick positions held by EDEN_HOST_PAD_FILE (lx, ly, rx, ry; 128 = centre).
std::uint8_t scripted_sticks[4] = {128, 128, 128, 128};

std::uint32_t ScriptedButtons() {
    static const auto start_time = std::chrono::steady_clock::now();
    struct Script { double start{}, end{}, period{}, hold{}; std::uint32_t mask{}; };
    static const std::vector<Script> scripts = [] {
        std::vector<Script> list;
        const char* text = std::getenv("EDEN_HOST_PAD");
        for (char* cursor = const_cast<char*>(text); cursor != nullptr && *cursor != '\0';) {
            Script s{};
            s.start = std::strtod(cursor, &cursor);
            if (*cursor == ':') s.end = std::strtod(cursor + 1, &cursor);
            if (*cursor == ':') s.period = std::strtod(cursor + 1, &cursor);
            if (*cursor == ':') s.hold = std::strtod(cursor + 1, &cursor);
            if (*cursor == ':') s.mask = static_cast<std::uint32_t>(std::strtoul(cursor + 1, &cursor, 0));
            if (s.period > 0 && s.mask != 0) list.push_back(s);
            if (*cursor != ';') break;
            ++cursor;
        }
        return list;
    }();
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    std::uint32_t buttons = 0;
    // EDEN_HOST_PAD_FILE=<path>: "<id> <mask> <milliseconds> [lx ly rx ry]" presses mask (and holds the
    // sticks, 0-255, 128 = centre) for that long whenever the id changes (a test driver steering the
    // game live, the same format as the console's compat-input.txt).
    if (static const char* const pad_file = std::getenv("EDEN_HOST_PAD_FILE"); pad_file) {
        static unsigned long long last_id = 0;
        static std::uint32_t held = 0;
        static unsigned held_sticks[4] = {128, 128, 128, 128};
        static double held_until = 0, last_read = -1;
        if (now - last_read >= 0.05) {
            last_read = now;
            if (std::FILE* file = std::fopen(pad_file, "r")) {
                unsigned long long id = 0;
                long mask = 0;
                unsigned long ms = 0;
                unsigned sticks[4] = {128, 128, 128, 128};
                const int read = std::fscanf(file, "%llu %li %lu %u %u %u %u", &id, &mask, &ms, &sticks[0], &sticks[1],
                                             &sticks[2], &sticks[3]);
                if (read >= 3 && id != last_id) {
                    last_id = id;
                    held = static_cast<std::uint32_t>(mask);
                    for (int i = 0; i < 4; ++i) held_sticks[i] = read == 7 && sticks[i] <= 255 ? sticks[i] : 128;
                    held_until = now + ms / 1000.0;
                }
                std::fclose(file);
            }
        }
        if (now < held_until) {
            buttons |= held;
            for (int i = 0; i < 4; ++i) scripted_sticks[i] = static_cast<std::uint8_t>(held_sticks[i]);
        } else {
            for (auto& stick : scripted_sticks) stick = 128;
        }
    }
    for (const auto& script : scripts) {
        if (now < script.start || now > script.end) continue;
        const double phase = now - script.start;
        if (phase - script.period * static_cast<long long>(phase / script.period) < script.hold) buttons |= script.mask;
    }
    return buttons;
}
} // namespace

std::int32_t scePadReadState(std::int32_t, ps5::pad::Data* data) {
    if (!data) return -1;
    std::memset(data, 0, sizeof(*data));
    data->connected = 1;
    // A controller at rest has its sticks centred (zeroed, both read as pushed up and left).
    data->buttons = ScriptedButtons();
    data->left_stick = {scripted_sticks[0], scripted_sticks[1]};
    data->right_stick = {scripted_sticks[2], scripted_sticks[3]};
    data->timestamp_us = static_cast<decltype(data->timestamp_us)>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return 0;
}
std::int32_t scePadRead(std::int32_t handle, ps5::pad::Data* data, std::int32_t capacity) {
    if (!data || capacity < 1) return 0;
    return scePadReadState(handle, data) == 0 ? 1 : 0;
}
std::int32_t scePadGetControllerInformation(std::int32_t, ps5::pad::ControllerInformation* information) {
    if (information) std::memset(information, 0, sizeof(*information));
    return 0;
}
std::int32_t scePadSetMotionSensorState(std::int32_t, bool) { return 0; }
std::int32_t scePadSetTiltCorrectionState(std::int32_t, bool) { return 0; }
std::int32_t scePadSetAngularVelocityDeadbandState(std::int32_t, bool) { return 0; }
std::int32_t scePadResetOrientation(std::int32_t) { return 0; }
std::int32_t scePadSetVibration(std::int32_t, const ps5::pad::Vibration*) { return 0; }
std::int32_t scePadSetVibrationMode(std::int32_t, std::int32_t) { return 0; }
std::int32_t scePadSetLightBar(std::int32_t, const ps5::pad::Color*) { return 0; }
std::int32_t scePadResetLightBar(std::int32_t) { return 0; }
std::int32_t scePadSetTriggerEffect(std::int32_t, const ps5::pad::TriggerEffect*) { return 0; }
std::int32_t scePadGetTriggerEffectState(std::int32_t, ps5::pad::TriggerEffectState* state) {
    if (state) std::memset(state, 0, sizeof(*state));
    return 0;
}

int sceKernelOpen(const char* path, int flags, std::uint16_t mode) { return ::open(path, flags, mode); }
int sceKernelClose(int descriptor) { return ::close(descriptor); }
std::int64_t sceKernelRead(int descriptor, void* buffer, std::size_t length) {
    return ::read(descriptor, buffer, length);
}

int sceAudioOutInit(void) { return 0; }
int sceAudioOutOpen(int, int, int, std::uint32_t, std::uint32_t, std::uint32_t) { return 1; }
int sceAudioOutClose(int) { return 0; }
int sceAudioOutOutput(int, const void*) {
    // One grain of 256 frames at 48 kHz, as the console blocks for it.
    std::this_thread::sleep_for(std::chrono::microseconds(5333));
    return 0;
}
int sceAudioOutSetVolume(int, int, const int*) { return 0; }

}  // extern "C"

// Host diagnostics: print a raw stack trace and the module map on a fatal signal.
#include <csignal>
#include <cstdio>
#include <execinfo.h>
namespace {
void HostCrashTrace(int signal, siginfo_t* info, void*) {
    void* frames[64];
    const int n = backtrace(frames, 64);
    std::fprintf(stderr, "EDEN_HOST_CRASH signal=%d addr=%p frames=%d\n", signal, info ? info->si_addr : nullptr, n);
    for (int i = 0; i < n; ++i) std::fprintf(stderr, "EDEN_HOST_CRASH frame=%p\n", frames[i]);
    if (FILE* maps = std::fopen("/proc/self/maps", "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), maps)) if (std::strstr(line, "eden-headless")) std::fputs(line, stderr);
        std::fclose(maps);
    }
    std::fflush(stderr);
    _exit(128 + signal);
}
[[maybe_unused]] const bool host_crash_trace_installed = [] {
    if (!std::getenv("EDEN_HOST_CRASH_TRACE")) return false;
    struct sigaction action{};
    action.sa_sigaction = HostCrashTrace;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGABRT}) sigaction(s, &action, nullptr);
    return true;
}();
}  // namespace

// Host diagnostics: on SIGTERM/SIGINT (timeout, Ctrl+C) write out Eden's log before leaving,
// so a bounded run keeps its trace.
#include "common/logging.h"
namespace {
void HostFlushAndExit(int signal) {
    std::fprintf(stderr, "EDEN_HOST_STOP signal=%d flushing log\n", signal);
    std::fflush(stderr);
    Common::Log::Stop();
    _exit(128 + signal);
}
[[maybe_unused]] const bool host_stop_installed = [] {
    if (!std::getenv("EDEN_HOST_CRASH_TRACE")) return false;
    std::signal(SIGTERM, HostFlushAndExit);
    std::signal(SIGINT, HostFlushAndExit);
    return true;
}();
}  // namespace

// The CPU_TRACE research profile reports SVC history through this hook; the host frontend
// has no SVC recorder, so it only notes the call.
extern "C" void prospero_research_svc_trace_dump() {
    std::fputs("CPU_TRACE svc_trace_dump unavailable_in_host_frontend=1\n", stderr);
    std::fflush(stderr);
}
