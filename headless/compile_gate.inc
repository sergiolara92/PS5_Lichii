// Pipeline compiles take turns when the console's pool runs low (headless/CMakeLists.txt derives
// common/thread_worker.h with this). Installed by performance.cpp on the console: the pool's
// usable free memory, read at most every 250 ms. Unset (the PC), every compile runs at once.
inline std::atomic<unsigned long long (*)()> compile_gate_free{nullptr};
inline std::mutex compile_gate_mutex;
inline std::condition_variable compile_gate_condition;
inline unsigned compile_gate_running = 0;

// How many compiles may run with this much free: four from 4 GiB, two from 1.5 GiB, else one.
// Never all of them on the console: a running compile keeps its memory until it ends, and OOT's
// save load on the Pro started a dozen with 4.7 GB free, then 2.7 GB of textures arrived within
// five seconds and the compiles' next requests found nothing.
inline unsigned CompileGateAllowed() {
    const auto free_memory = compile_gate_free.load(std::memory_order_relaxed);
    if (free_memory == nullptr) {
        return ~0u;
    }
    const unsigned long long free = free_memory();
    // Two from 1.5 GiB, not 2.5: one at a time with 1-2.5 GiB free (OOT's Hyrule Field) queued six new
    // pipelines behind each other and their draws waited 2.7 s (run fritchii-31).
    return free >= (4ULL << 30) ? 4u : free >= (3ULL << 29) ? 2u : 1u;
}

inline void CompileGateEnter() {
    std::unique_lock lock{compile_gate_mutex};
    // Bounded: a compile that waits for another compile of this pool must not wait for ever.
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (compile_gate_running >= CompileGateAllowed() && std::chrono::steady_clock::now() < give_up) {
        compile_gate_condition.wait_for(lock, std::chrono::milliseconds(20));
    }
    ++compile_gate_running;
}

inline void CompileGateLeave() {
    {
        std::scoped_lock lock{compile_gate_mutex};
        --compile_gate_running;
    }
    compile_gate_condition.notify_one();
}
