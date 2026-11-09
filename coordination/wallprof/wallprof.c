// LD_PRELOAD wall-clock sampler: from WALLPROF_FROM seconds, every WALLPROF_US microseconds of
// real time, interrupts each thread named in WALLPROF_THREADS (comma list) and writes its stack
// (return addresses) to WALLPROF_OUT. Waits are sampled too, so a thread's whole time is split.
#define _GNU_SOURCE
#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { Depth = 64, MaxThreads = 32 };
static int out = -1;
struct Record { int32_t tag, depth; void* pc[Depth]; };

static void on_sample(int sig, siginfo_t* info, void* context) {
    (void)sig; (void)context;
    struct Record r;
    r.tag = info->si_value.sival_int;
    r.depth = backtrace(r.pc, Depth);
    if (out >= 0) (void)!write(out, &r, sizeof r);
}

static void* helper(void* arg) {
    (void)arg;
    const char* names = getenv("WALLPROF_THREADS");
    const char* from = getenv("WALLPROF_FROM");
    const char* us = getenv("WALLPROF_US");
    sleep(from ? (unsigned)atoi(from) : 60);
    char path[512];
    snprintf(path, sizeof path, "%s.maps", getenv("WALLPROF_OUT"));
    int maps_in = open("/proc/self/maps", O_RDONLY), maps_out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char buf[65536]; ssize_t n;
    while ((n = read(maps_in, buf, sizeof buf)) > 0) (void)!write(maps_out, buf, (size_t)n);
    close(maps_in); close(maps_out);
    // Tag = position of the thread in /proc/self/task order; names go to the .tags file.
    snprintf(path, sizeof path, "%s.tags", getenv("WALLPROF_OUT"));
    FILE* tags = fopen(path, "w");
    DIR* dir = opendir("/proc/self/task");
    struct dirent* e; int tag = 0;
    while ((e = readdir(dir)) && tag < MaxThreads) {
        if (e->d_name[0] == '.') continue;
        char comm_path[64], comm[32] = {0};
        snprintf(comm_path, sizeof comm_path, "/proc/self/task/%s/comm", e->d_name);
        int fd = open(comm_path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t got = read(fd, comm, sizeof comm - 1); close(fd);
        if (got > 0 && comm[got - 1] == '\n') comm[got - 1] = 0;
        char list[512]; snprintf(list, sizeof list, ",%s,", names ? names : "GPU");
        char key[40]; snprintf(key, sizeof key, ",%s,", comm);
        if (!strstr(list, key)) continue;
        struct sigevent ev = {0};
        ev.sigev_notify = SIGEV_THREAD_ID;
        ev.sigev_signo = SIGPROF;
        ev.sigev_value.sival_int = tag;
        ev._sigev_un._tid = atoi(e->d_name);
        timer_t timer;
        if (timer_create(CLOCK_MONOTONIC, &ev, &timer) != 0) continue;
        long period = us ? atol(us) : 2000;
        struct itimerspec spec = {{period / 1000000, (period % 1000000) * 1000}, {0, 1000000 + tag * 37000}};
        timer_settime(timer, 0, &spec, NULL);
        fprintf(tags, "%d %s %s\n", tag, e->d_name, comm);
        ++tag;
    }
    closedir(dir); fclose(tags);
    return NULL;
}

__attribute__((constructor)) static void start(void) {
    const char* path = getenv("WALLPROF_OUT");
    if (!path) return;
    void* warm[4]; backtrace(warm, 4); // loads the unwinder before any signal
    out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    pthread_t t; pthread_create(&t, NULL, helper, NULL); pthread_detach(t);
}
