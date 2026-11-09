// pmcmiss <from> <seconds> <thread-name-prefix> <out> <command...>: pmcjit's wrapper with other events:
// precise user-mode samples of cycles (c, one per 100,003), loads that missed L3 (m,
// MEM_LOAD_RETIRED.L3_MISS, one per 1,009) and L2 (l, .L2_MISS, one per 10,007) on Skylake-family
// cores, for the threads named <prefix>*. Writes "<kind> <tid> <ip> -" lines and <out>.maps;
// L3-miss lines carry "<caller,caller..." (LBR call stack, newest first). pmcmiss.py symbolizes them.
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGES 256  // ring buffer pages per event (power of two)
#define PERIOD 100003
#define MAX_SAMPLES (1 << 22)

struct ring { int fd; struct perf_event_mmap_page* meta; char* data; size_t size; char kind; };
enum { CALLERS = 4 };
struct sample { char kind; uint32_t tid; uint64_t ip; uint64_t from[CALLERS]; };
static struct sample* samples;
static size_t sample_count;

static int open_sampler(pid_t tid, int type, uint64_t config, uint64_t period) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.size = sizeof attr;
    attr.type = type;
    attr.config = config;
    attr.sample_period = period;
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
    if (type == PERF_TYPE_RAW && config == 0x20d1) {
        // L3-miss samples also carry the LBR call stack: the call sites that led to the load.
        attr.sample_type |= PERF_SAMPLE_BRANCH_STACK;
        attr.branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_CALL_STACK;
    }
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.disabled = 1;
    attr.precise_ip = 2;
    return (int)syscall(SYS_perf_event_open, &attr, tid, -1, -1, 0);
}

static void drain(struct ring* r) {
    uint64_t head = __atomic_load_n(&r->meta->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = r->meta->data_tail;
    while (tail < head) {
        struct perf_event_header h;
        for (size_t i = 0; i < sizeof h; ++i) ((char*)&h)[i] = r->data[(tail + i) % r->size];
        if (h.type == PERF_RECORD_SAMPLE && sample_count < MAX_SAMPLES) {
            uint64_t body[3 + 3 * CALLERS] = {0};  // ip, pid/tid, then branch stack nr + {from, to, flags}
            const size_t want = (sizeof h + sizeof body <= h.size ? sizeof body : h.size - sizeof h);
            for (size_t i = 0; i < want; ++i) ((char*)body)[i] = r->data[(tail + sizeof h + i) % r->size];
            struct sample x = {r->kind, (uint32_t)(body[1] >> 32), body[0], {0}};
            if (r->kind == 'm')
                for (uint64_t k = 0; k < body[2] && k < CALLERS; ++k) x.from[k] = body[3 + 3 * k];
            samples[sample_count++] = x;
        }
        tail += h.size;
    }
    __atomic_store_n(&r->meta->data_tail, tail, __ATOMIC_RELEASE);
}

struct code_map { uint64_t start, end; };

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: pmcmiss <from> <seconds> <prefix> <out> <command...>\n"); return 2; }
    const pid_t child = fork();
    // The app dies with this wrapper (bwrap --die-with-parent kills only its own child, this one).
    if (child == 0) { prctl(PR_SET_PDEATHSIG, SIGKILL); execvp(argv[5], argv + 5); perror("execvp"); _exit(127); }
    if (child < 0) { perror("fork"); return 1; }
    samples = malloc(sizeof *samples * MAX_SAMPLES);
    for (int t = 0; t < atoi(argv[1]); ++t) {
        sleep(1);
        if (waitpid(child, NULL, WNOHANG) == child) return 1;
    }
    char path[256];
    snprintf(path, sizeof path, "/proc/%d/task", (int)child);
    DIR* dir = opendir(path);
    if (!dir) { perror(path); return 1; }
    struct ring rings[512];
    int n = 0;
    const long page = sysconf(_SC_PAGESIZE);
    for (struct dirent* e; (e = readdir(dir)) && n < 507;) {
        if (e->d_name[0] == '.') continue;
        char comm_path[300], comm[64] = {0};
        snprintf(comm_path, sizeof comm_path, "%s/%s/comm", path, e->d_name);
        FILE* f = fopen(comm_path, "r");
        if (!f) continue;
        if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
        fclose(f);
        if (strncmp(comm, argv[3], strlen(argv[3])) != 0) continue;
        fprintf(stderr, "pmcmiss: thread %s %s", e->d_name, comm);
        for (int k = 0; k < 3; ++k) {
            struct ring* r = &rings[n];
            r->kind = "cml"[k];
            r->fd = k == 0 ? open_sampler(atoi(e->d_name), PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, 100003)
                           : open_sampler(atoi(e->d_name), PERF_TYPE_RAW, k == 1 ? 0x20d1 : 0x10d1, k == 1 ? 1009 : 10007);
            if (r->fd < 0) { perror("perf_event_open"); return 1; }
            void* m = mmap(NULL, (PAGES + 1) * page, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
            if (m == MAP_FAILED) { perror("mmap"); return 1; }
            r->meta = m;
            r->data = (char*)m + page;
            r->size = PAGES * page;
            ++n;
        }
    }
    closedir(dir);
    for (int i = 0; i < n; ++i) ioctl(rings[i].fd, PERF_EVENT_IOC_ENABLE, 0);
    char cmd[600];
    // At the start, while the app surely runs. Copied here, not by a child process: ptrace_scope 1 lets only the parent read the app's maps.
    snprintf(cmd, sizeof cmd, "%s.maps", argv[4]);
    snprintf(path, sizeof path, "/proc/%d/maps", (int)child);
    FILE* src = fopen(path, "r");
    FILE* dst = fopen(cmd, "w");
    for (int ch; src && dst && (ch = fgetc(src)) != EOF;) fputc(ch, dst);
    if (src) fclose(src);
    if (dst) fclose(dst);
    const int seconds = atoi(argv[2]);
    for (int t = 0; t < seconds * 20; ++t) {  // drain every 50 ms so the rings never fill
        usleep(50000);
        for (int i = 0; i < n; ++i) drain(&rings[i]);
    }
    for (int i = 0; i < n; ++i) { ioctl(rings[i].fd, PERF_EVENT_IOC_DISABLE, 0); drain(&rings[i]); }

    // The JIT's executable memfd views; their bytes come from the child's memory.
    struct code_map maps[64];
    int map_count = 0;
    snprintf(path, sizeof path, "/proc/%d/maps", (int)child);
    FILE* mf = fopen(path, "r");
    char line[512];
    while (mf && fgets(line, sizeof line, mf) && map_count < 64) {
        unsigned long long start, end;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3) continue;
        if (perms[2] != 'x' || !strstr(line, "memfd:")) continue;
        maps[map_count++] = (struct code_map){start, end};
    }
    if (mf) fclose(mf);
    snprintf(path, sizeof path, "/proc/%d/mem", (int)child);
    const int mem = open(path, O_RDONLY);
    if (mem < 0) perror(path);
    FILE* out = fopen(argv[4], "w");
    if (!out) { perror(argv[4]); return 1; }
    size_t jit = 0;
    for (size_t s = 0; s < sample_count; ++s) {
        const struct sample* x = &samples[s];
        unsigned char bytes[16];
        int got = 0;
        for (int i = 0; i < map_count && !got && mem >= 0; ++i)
            if (x->ip >= maps[i].start && x->ip + 16 <= maps[i].end)
                got = pread(mem, bytes, 16, (off_t)x->ip) == 16;
        fprintf(out, "%c %u %llx ", x->kind, x->tid, (unsigned long long)x->ip);
        for (int k = 0; k < CALLERS && x->from[k]; ++k) fprintf(out, "%s%llx", k ? "," : "<", (unsigned long long)x->from[k]);
        if (x->from[0]) fputc(' ', out);
        if (got) { ++jit; for (int i = 0; i < 16; ++i) fprintf(out, "%02x", bytes[i]); } else fputc('-', out);
        fputc('\n', out);
    }
    fclose(out);
    fprintf(stderr, "pmcmiss: threads=%d samples=%zu in_jit=%zu code_maps=%d\n", n / 3, sample_count, jit, map_count);
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
