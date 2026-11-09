// pmc <pid> <thread-name-prefix> <seconds>: user-mode hardware counters (perf_event_open, no perf
// tool needed; perf_event_paranoid 2 allows a user's own threads) summed over the threads of <pid>
// whose name starts with the prefix (e.g. CPUCore_ for the emulator's guest cores), for <seconds>.
// Prints cycles, instructions, IPC, branch misses, L1D read misses, LLC misses, dTLB read misses.
#define _GNU_SOURCE
#include <dirent.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { CYCLES, INSTRUCTIONS, BRANCHES, BRANCH_MISSES, L1D_MISSES, LLC_MISSES, DTLB_MISSES, EVENTS };
static const char* names[EVENTS] = {"cycles", "instructions", "branches", "branch_misses", "l1d_read_misses", "llc_misses", "dtlb_read_misses"};

static int open_event(pid_t tid, int which) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.size = sizeof attr;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.disabled = 1;
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    const uint64_t cache = 0;
    switch (which) {
    case CYCLES: attr.type = PERF_TYPE_HARDWARE; attr.config = PERF_COUNT_HW_CPU_CYCLES; break;
    case INSTRUCTIONS: attr.type = PERF_TYPE_HARDWARE; attr.config = PERF_COUNT_HW_INSTRUCTIONS; break;
    case BRANCHES: attr.type = PERF_TYPE_HARDWARE; attr.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS; break;
    case BRANCH_MISSES: attr.type = PERF_TYPE_HARDWARE; attr.config = PERF_COUNT_HW_BRANCH_MISSES; break;
    case L1D_MISSES: attr.type = PERF_TYPE_HW_CACHE;
        attr.config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16); break;
    case LLC_MISSES: attr.type = PERF_TYPE_HARDWARE; attr.config = PERF_COUNT_HW_CACHE_MISSES; break;
    case DTLB_MISSES: attr.type = PERF_TYPE_HW_CACHE;
        attr.config = PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16); break;
    }
    (void)cache;
    return (int)syscall(SYS_perf_event_open, &attr, tid, -1, -1, 0);
}

int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr, "usage: pmc <pid> <thread-name-prefix> <seconds>\n"); return 2; }
    const char* pid = argv[1];
    const char* prefix = argv[2];
    char path[256];
    snprintf(path, sizeof path, "/proc/%s/task", pid);
    DIR* dir = opendir(path);
    if (!dir) { perror(path); return 1; }
    int fds[256][EVENTS];
    int threads = 0;
    for (struct dirent* e; (e = readdir(dir)) && threads < 256;) {
        if (e->d_name[0] == '.') continue;
        char comm_path[300], comm[64] = {0};
        snprintf(comm_path, sizeof comm_path, "%s/%s/comm", path, e->d_name);
        FILE* f = fopen(comm_path, "r");
        if (!f) continue;
        if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
        fclose(f);
        if (strncmp(comm, prefix, strlen(prefix)) != 0) continue;
        for (int i = 0; i < EVENTS; ++i) {
            fds[threads][i] = open_event(atoi(e->d_name), i);
            if (fds[threads][i] < 0) { perror("perf_event_open"); return 1; }
        }
        comm[strcspn(comm, "\n")] = 0;
        fprintf(stderr, "thread %s %s\n", e->d_name, comm);
        ++threads;
    }
    closedir(dir);
    if (!threads) { fprintf(stderr, "no thread named %s*\n", prefix); return 1; }
    for (int t = 0; t < threads; ++t)
        for (int i = 0; i < EVENTS; ++i) ioctl(fds[t][i], PERF_EVENT_IOC_ENABLE, 0);
    sleep((unsigned)atoi(argv[3]));
    double total[EVENTS] = {0};
    for (int t = 0; t < threads; ++t)
        for (int i = 0; i < EVENTS; ++i) {
            uint64_t v[3] = {0};
            if (read(fds[t][i], v, sizeof v) == sizeof v && v[2])
                total[i] += (double)v[0] * (double)v[1] / (double)v[2];  // scaled for multiplexing
        }
    printf("threads=%d", threads);
    for (int i = 0; i < EVENTS; ++i) printf(" %s=%.0f", names[i], total[i]);
    printf("\nIPC=%.2f branch_miss_per_kinst=%.2f branch_miss_rate=%.2f%% l1d_miss_per_kinst=%.2f llc_miss_per_kinst=%.3f dtlb_miss_per_kinst=%.3f\n",
           total[INSTRUCTIONS] / total[CYCLES], 1000 * total[BRANCH_MISSES] / total[INSTRUCTIONS],
           100 * total[BRANCH_MISSES] / total[BRANCHES], 1000 * total[L1D_MISSES] / total[INSTRUCTIONS],
           1000 * total[LLC_MISSES] / total[INSTRUCTIONS], 1000 * total[DTLB_MISSES] / total[INSTRUCTIONS]);
    return 0;
}
