// pmcbr (pmcjit with precise branch-miss samples, kind b, one per 2,003, instead of instructions) <from> <seconds> <thread-name-prefix> <out> <command...>: runs the command (as NS2_WRAPPER, so it is
// the app's parent and may read its memory under ptrace_scope=1) and from <from> seconds takes precise
// (PEBS, precise_ip=2) user-mode cycle and instruction samples of its threads named <prefix>*, one per
// 100,003 events, for <seconds>; then reads the code bytes at every sampled address in executable
// memfd mappings (the JIT's code) from /proc/<child>/mem. Writes
// "<kind> <tid> <ip> <16 bytes hex or ->" lines (kind c = cycles, i = instructions) to <out>;
// jitmix.py turns them into the retired-instruction and cycle mix of JIT code by instruction kind.
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGES 256  // ring buffer pages per event (power of two)
#define PERIOD 100003
#define MAX_SAMPLES (1 << 24)

struct ring { int fd; struct perf_event_mmap_page* meta; char* data; size_t size; char kind; };
struct sample { char kind; uint32_t tid; uint64_t ip; };
static struct sample* samples;
static size_t sample_count;

static int open_sampler(pid_t tid, uint64_t config) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.size = sizeof attr;
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = config;
    attr.sample_period = config == PERF_COUNT_HW_BRANCH_MISSES ? 2003 : PERIOD;  // misses are rarer
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
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
            uint64_t body[2] = {0, 0};  // ip, then pid/tid
            for (size_t i = 0; i < sizeof body; ++i) ((char*)body)[i] = r->data[(tail + sizeof h + i) % r->size];
            samples[sample_count++] = (struct sample){r->kind, (uint32_t)(body[1] >> 32), body[0]};
        }
        tail += h.size;
    }
    __atomic_store_n(&r->meta->data_tail, tail, __ATOMIC_RELEASE);
}

struct code_map { uint64_t start, end; };

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: pmcbr <from> <seconds> <prefix> <out> <command...>\n"); return 2; }
    const pid_t child = fork();
    if (child == 0) { execvp(argv[5], argv + 5); perror("execvp"); _exit(127); }
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
    for (struct dirent* e; (e = readdir(dir)) && n < 510;) {
        if (e->d_name[0] == '.') continue;
        char comm_path[300], comm[64] = {0};
        snprintf(comm_path, sizeof comm_path, "%s/%s/comm", path, e->d_name);
        FILE* f = fopen(comm_path, "r");
        if (!f) continue;
        if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
        fclose(f);
        if (strncmp(comm, argv[3], strlen(argv[3])) != 0) continue;
        for (int k = 0; k < 2; ++k) {
            struct ring* r = &rings[n];
            r->kind = k ? 'b' : 'c';
            r->fd = open_sampler(atoi(e->d_name), k ? PERF_COUNT_HW_BRANCH_MISSES : PERF_COUNT_HW_CPU_CYCLES);
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
    char cmd[600];
    snprintf(cmd, sizeof cmd, "cp /proc/%d/maps '%s.maps'", (int)child, argv[4]);
    if (system(cmd) != 0) fprintf(stderr, "maps copy failed\n");
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
        if (got) { ++jit; for (int i = 0; i < 16; ++i) fprintf(out, "%02x", bytes[i]); } else fputc('-', out);
        fputc('\n', out);
    }
    fclose(out);
    fprintf(stderr, "pmcbr: threads=%d samples=%zu in_jit=%zu code_maps=%d\n", n / 2, sample_count, jit, map_count);
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
