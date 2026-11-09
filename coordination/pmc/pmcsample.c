// pmcsample <pid> <thread-name-prefix> <seconds> <out>: user-mode cycle and instruction samples
// (perf_event_open, one sample per 200,003 events) for the threads of <pid> whose name starts with the
// prefix. Writes "<kind> <ip>" lines (kind c = cycles, i = instructions) to <out>, and copies
// /proc/<pid>/maps to <out>.maps; pmcregions.py turns them into time and IPC per code region.
#define _GNU_SOURCE
#include <dirent.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PAGES 256  // ring buffer pages per event (power of two)
#define PERIOD 200003

struct ring { int fd; struct perf_event_mmap_page* meta; char* data; size_t size; char kind; };

static int open_sampler(pid_t tid, uint64_t config) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.size = sizeof attr;
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = config;
    attr.sample_period = PERIOD;
    attr.sample_type = PERF_SAMPLE_IP;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.disabled = 1;
    attr.precise_ip = 0;
    return (int)syscall(SYS_perf_event_open, &attr, tid, -1, -1, 0);
}

static void drain(struct ring* r, FILE* out) {
    uint64_t head = __atomic_load_n(&r->meta->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = r->meta->data_tail;
    while (tail < head) {
        struct perf_event_header h;
        for (size_t i = 0; i < sizeof h; ++i) ((char*)&h)[i] = r->data[(tail + i) % r->size];
        if (h.type == PERF_RECORD_SAMPLE) {
            uint64_t ip = 0;
            for (size_t i = 0; i < sizeof ip; ++i) ((char*)&ip)[i] = r->data[(tail + sizeof h + i) % r->size];
            fprintf(out, "%c %llx\n", r->kind, (unsigned long long)ip);
        }
        tail += h.size;
    }
    __atomic_store_n(&r->meta->data_tail, tail, __ATOMIC_RELEASE);
}

int main(int argc, char** argv) {
    if (argc != 5) { fprintf(stderr, "usage: pmcsample <pid> <prefix> <seconds> <out>\n"); return 2; }
    char path[256];
    snprintf(path, sizeof path, "/proc/%s/task", argv[1]);
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
        if (strncmp(comm, argv[2], strlen(argv[2])) != 0) continue;
        for (int k = 0; k < 2; ++k) {
            struct ring* r = &rings[n];
            r->kind = k ? 'i' : 'c';
            r->fd = open_sampler(atoi(e->d_name), k ? PERF_COUNT_HW_INSTRUCTIONS : PERF_COUNT_HW_CPU_CYCLES);
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
    FILE* out = fopen(argv[4], "w");
    if (!out) { perror(argv[4]); return 1; }
    for (int i = 0; i < n; ++i) ioctl(rings[i].fd, PERF_EVENT_IOC_ENABLE, 0);
    const int seconds = atoi(argv[3]);
    for (int t = 0; t < seconds * 20; ++t) {  // drain every 50 ms so the rings never fill
        usleep(50000);
        for (int i = 0; i < n; ++i) drain(&rings[i], out);
    }
    for (int i = 0; i < n; ++i) { ioctl(rings[i].fd, PERF_EVENT_IOC_DISABLE, 0); drain(&rings[i], out); }
    fclose(out);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "cp /proc/%s/maps '%s.maps'", argv[1], argv[4]);
    if (system(cmd) != 0) fprintf(stderr, "maps copy failed\n");
    fprintf(stderr, "threads=%d events=%d\n", n / 2, n);
    return 0;
}
