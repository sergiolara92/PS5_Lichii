// pmctlb <from s> <seconds> <prefix[,prefix]> <out> <command...>: runs the command (as NS2_WRAPPER,
// the app's parent, so perf_event_open may watch it at perf_event_paranoid 2 / ptrace_scope 1); from
// <from s> on, for <seconds>, counts user-mode Intel Skylake-family events on the threads whose names
// start with a prefix, and writes one line per second per thread name to <out>:
//   <t> <thread> then per group: cycles and its four events (instr is group 3's fourth)
// Groups (each led by cycles, multiplexed, scaled): TLB walks, memory stalls, top-down slots,
// front-end. Report: pmctlb.py <out>. For memory-system levers (page size, code layout).
#define _GNU_SOURCE
#include <dirent.h>
#include <linux/perf_event.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RAW(ev, um, cm, edge) ((ev) | ((um) << 8) | ((uint64_t)(edge) << 18) | ((uint64_t)(cm) << 24))
enum { GROUPS = 4, PER = 4 };
static const char* names[GROUPS][PER] = {
    {"dtlb_load_walk", "dtlb_store_walk", "itlb_walk", "stlb_hit_load"},
    {"stalls_total", "stalls_mem", "stalls_l2miss", "stalls_l3miss"},
    {"idq_not_delivered", "uops_issued", "retire_slots", "recovery_cycles"},
    {"iftag_stall", "br_misp", "machine_clears", "instr"},
};
static const uint64_t raws[GROUPS][PER] = {
    {RAW(0x08, 0x10, 1, 0), RAW(0x49, 0x10, 1, 0), RAW(0x85, 0x10, 1, 0), RAW(0x08, 0x20, 0, 0)},
    {RAW(0xa3, 0x04, 4, 0), RAW(0xa3, 0x14, 20, 0), RAW(0xa3, 0x05, 5, 0), RAW(0xa3, 0x06, 6, 0)},
    {RAW(0x9c, 0x01, 0, 0), RAW(0x0e, 0x01, 0, 0), RAW(0xc2, 0x02, 0, 0), RAW(0x0d, 0x01, 0, 0)},
    {RAW(0x83, 0x04, 0, 0), RAW(0xc5, 0x00, 0, 0), RAW(0xc3, 0x01, 1, 1), 0},
};

static int open_one(pid_t tid, int type, uint64_t config, int leader) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = type;
    a.config = config;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.disabled = leader < 0;
    a.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    return (int)syscall(SYS_perf_event_open, &a, tid, -1, leader, 0);
}

struct Thread {
    pid_t tid;
    char name[32];
    int fd[GROUPS];
    double last[GROUPS][PER + 1];
};
static struct Thread threads[512];
static int nthreads;

static int matches(const char* comm, const char* prefixes) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", prefixes);
    for (char* p = strtok(buf, ","); p; p = strtok(NULL, ","))
        if (strncmp(comm, p, strlen(p)) == 0) return 1;
    return 0;
}

static void scan(pid_t pid, const char* prefixes) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", pid);
    DIR* dir = opendir(path);
    if (!dir) return;
    for (struct dirent* e; (e = readdir(dir)) && nthreads < 512;) {
        if (e->d_name[0] == '.') continue;
        const pid_t tid = atoi(e->d_name);
        int known = 0;
        for (int i = 0; i < nthreads; ++i) known |= threads[i].tid == tid;
        if (known) continue;
        char cp[128], comm[32] = {0};
        snprintf(cp, sizeof cp, "%s/%s/comm", path, e->d_name);
        FILE* f = fopen(cp, "r");
        if (!f) continue;
        if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
        fclose(f);
        comm[strcspn(comm, "\n")] = 0;
        if (!matches(comm, prefixes)) continue;
        struct Thread* t = &threads[nthreads];
        memset(t, 0, sizeof *t);
        t->tid = tid;
        snprintf(t->name, sizeof t->name, "%s", comm);
        int ok = 1;
        for (int g = 0; g < GROUPS && ok; ++g) {
            t->fd[g] = open_one(tid, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, -1);
            ok = t->fd[g] >= 0;
            for (int k = 0; k < PER && ok; ++k) {
                const int type = raws[g][k] ? PERF_TYPE_RAW : PERF_TYPE_HARDWARE;
                const uint64_t config = raws[g][k] ? raws[g][k] : PERF_COUNT_HW_INSTRUCTIONS;
                ok = open_one(tid, type, config, t->fd[g]) >= 0;
            }
            if (ok) ioctl(t->fd[g], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
        }
        if (!ok) { perror("perf_event_open"); continue; }
        fprintf(stderr, "pmctlb: thread %d %s\n", tid, comm);
        ++nthreads;
    }
    closedir(dir);
}

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: pmctlb <from> <seconds> <prefix[,prefix]> <out> <command...>\n"); return 2; }
    const pid_t child = fork();
    // The app dies with this wrapper (bwrap --die-with-parent kills only its own child, this one).
    if (child == 0) { prctl(PR_SET_PDEATHSIG, SIGKILL); execvp(argv[5], argv + 5); perror("execvp"); _exit(127); }
    if (child < 0) { perror("fork"); return 1; }
    signal(SIGTERM, SIG_IGN);
    const int from = atoi(argv[1]), seconds = atoi(argv[2]);
    for (int s = 0; s < from; ++s) {
        sleep(1);
        if (waitpid(child, NULL, WNOHANG) == child) return 0;
    }
    FILE* out = fopen(argv[4], "w");
    if (!out) { perror(argv[4]); return 1; }
    for (int s = 0; s <= seconds; ++s) {
        scan(child, argv[3]);
        for (int i = 0; i < nthreads; ++i) {
            struct Thread* t = &threads[i];
            fprintf(out, "%d %s", s, t->name);
            for (int g = 0; g < GROUPS; ++g) {
                uint64_t v[3 + PER + 1] = {0};
                double now[PER + 1] = {0};
                if (read(t->fd[g], v, sizeof v) > 0 && v[2])
                    for (int k = 0; k < PER + 1; ++k) now[k] = (double)v[3 + k] * (double)v[1] / (double)v[2];
                for (int k = 0; k < PER + 1; ++k) {
                    if (s > 0) fprintf(out, " %.0f", now[k] - t->last[g][k]);
                    t->last[g][k] = now[k];
                }
            }
            fprintf(out, "\n");
        }
        fflush(out);
        sleep(1);
        if (waitpid(child, NULL, WNOHANG) == child) return 0;
    }
    fclose(out);
    fprintf(stderr, "pmctlb: wrote %s\n", argv[4]);
    int status;
    waitpid(child, &status, 0);
    return 0;
}
