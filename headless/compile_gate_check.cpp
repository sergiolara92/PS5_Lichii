// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of headless/compile_gate.inc: how many pipeline compiles run at once for a given
// free pool. Build: c++ -std=c++20 -pthread headless/compile_gate_check.cpp && ./a.out
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace Common {
#include "compile_gate.inc"
}

static unsigned long long free_bytes;
static unsigned long long FreeMemory() { return free_bytes; }

// The most compiles inside the gate at once, 8 threads each holding it 20 ms.
static unsigned MostAtOnce() {
    std::atomic<unsigned> inside{0}, most{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            Common::CompileGateEnter();
            const unsigned now = ++inside;
            for (unsigned seen = most.load(); now > seen && !most.compare_exchange_weak(seen, now);) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            --inside;
            Common::CompileGateLeave();
        });
    }
    for (auto& thread : threads) thread.join();
    return most.load();
}

int main() {
    assert(MostAtOnce() > 2); // no reading installed (the PC): all at once
    Common::compile_gate_free.store(&FreeMemory);
    free_bytes = 6ULL << 30;
    assert(MostAtOnce() == 4);
    free_bytes = 3ULL << 30;
    assert(MostAtOnce() == 2);
    free_bytes = 2ULL << 30;
    assert(MostAtOnce() == 2);
    free_bytes = 512ULL << 20;
    assert(MostAtOnce() == 1);
    assert(Common::compile_gate_running == 0);
    std::puts("compile gate PASS");
}
