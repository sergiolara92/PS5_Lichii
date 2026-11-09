// Live heap by call site (PC diagnostics only). LD_PRELOAD it; SIGUSR... no: a file /work/heapsite.dump
// appearing makes the next allocation write the 25 heaviest sites (bytes, blocks, return addresses)
// to /work/heapsite.txt. Each allocation records its size and a site of three return addresses.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PTR_SLOTS (1u << 24)   // live pointers (16 M)
#define SITE_SLOTS (1u << 16)
typedef struct { uintptr_t ptr; uint32_t size; uint32_t site; } Entry;
typedef struct { uintptr_t pc[3]; _Atomic uint64_t bytes, blocks; _Atomic int used; } Site;
static Entry *table;
static Site sites[SITE_SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *(*real_malloc)(size_t), *(*real_calloc)(size_t, size_t), *(*real_realloc)(void *, size_t);
static void (*real_free)(void *);
static __thread int inside;
static char boot[1 << 16]; static size_t boot_used;

static void init(void) {
    if (real_malloc) return;
    real_malloc = dlsym(RTLD_NEXT, "malloc"); real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc"); real_free = dlsym(RTLD_NEXT, "free");
    table = real_calloc(PTR_SLOTS, sizeof(Entry));
}
static uint32_t site_of(void) {
    void *frames[6]; int n = backtrace(frames, 6);
    uintptr_t pc[3] = {n > 3 ? (uintptr_t)frames[3] : 0, n > 4 ? (uintptr_t)frames[4] : 0, n > 5 ? (uintptr_t)frames[5] : 0};
    uint64_t h = pc[0] * 0x9e3779b97f4a7c15ull ^ pc[1] * 0xc2b2ae3d27d4eb4full ^ pc[2] * 0x165667b19e3779f9ull;
    for (uint32_t i = 0; i < SITE_SLOTS; ++i) {
        Site *s = &sites[(h + i) & (SITE_SLOTS - 1)];
        int expected = 0;
        if (atomic_load(&s->used) == 2 && !memcmp(s->pc, pc, sizeof pc)) return (h + i) & (SITE_SLOTS - 1);
        if (atomic_compare_exchange_strong(&s->used, &expected, 1)) {
            memcpy(s->pc, pc, sizeof pc); atomic_store(&s->used, 2); return (h + i) & (SITE_SLOTS - 1);
        }
        while (atomic_load(&s->used) == 1) {}
        if (!memcmp(s->pc, pc, sizeof pc)) return (h + i) & (SITE_SLOTS - 1);
    }
    return 0;
}
static void dump(void) {
    if (access("/work/heapsite.dump", F_OK) != 0) return;
    unlink("/work/heapsite.dump");
    FILE *out = fopen("/work/heapsite.txt", "w"); if (!out) return;
    static uint32_t order[SITE_SLOTS]; uint32_t n = 0;
    for (uint32_t i = 0; i < SITE_SLOTS; ++i) if (atomic_load(&sites[i].used) == 2 && sites[i].bytes) order[n++] = i;
    for (uint32_t k = 0; k < 25 && k < n; ++k) {
        uint32_t best = k;
        for (uint32_t j = k + 1; j < n; ++j) if (sites[order[j]].bytes > sites[order[best]].bytes) best = j;
        uint32_t t = order[k]; order[k] = order[best]; order[best] = t;
        Site *s = &sites[order[k]];
        fprintf(out, "%llu bytes %llu blocks", (unsigned long long)s->bytes, (unsigned long long)s->blocks);
        for (int f = 0; f < 3; ++f) {
            Dl_info info; uintptr_t off = 0; const char *obj = "?";
            if (dladdr((void *)s->pc[f], &info) && info.dli_fname) { obj = info.dli_fname; off = s->pc[f] - (uintptr_t)info.dli_fbase; }
            fprintf(out, " %s+0x%lx", strrchr(obj, '/') ? strrchr(obj, '/') + 1 : obj, (unsigned long)off);
        }
        fputc('\n', out);
    }
    fclose(out);
}
static void record(void *p, size_t size) {
    if (!p || !table || inside) return;
    inside = 1;
    uint32_t site = site_of();
    atomic_fetch_add(&sites[site].bytes, size); atomic_fetch_add(&sites[site].blocks, 1);
    uint64_t h = (uintptr_t)p * 0x9e3779b97f4a7c15ull;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < PTR_SLOTS; ++i) {
        Entry *e = &table[(h + i) & (PTR_SLOTS - 1)];
        if (e->ptr == 0 || e->ptr == 1) { e->ptr = (uintptr_t)p; e->size = size > 0xffffffffu ? 0xffffffffu : (uint32_t)size; e->site = site; break; }
    }
    pthread_mutex_unlock(&lock);
    if (size > (100u << 20)) {  // one big block: say where, at once
        void *frames[12]; int n = backtrace(frames, 12);
        FILE *big = fopen("/work/heapsite-big.txt", "a");
        if (big) {
            fprintf(big, "%zu", size);
            for (int f = 3; f < n; ++f) {
                Dl_info info; uintptr_t off = 0; const char *obj = "?";
                if (dladdr(frames[f], &info) && info.dli_fname) { obj = info.dli_fname; off = (uintptr_t)frames[f] - (uintptr_t)info.dli_fbase; }
                fprintf(big, " %s+0x%lx", strrchr(obj, '/') ? strrchr(obj, '/') + 1 : obj, (unsigned long)off);
            }
            fputc('\n', big); fclose(big);
        }
    }
    static _Atomic unsigned tick; if ((atomic_fetch_add(&tick, 1) & 0xffff) == 0) dump();
    inside = 0;
}
static void forget(void *p) {
    if (!p || !table) return;
    uint64_t h = (uintptr_t)p * 0x9e3779b97f4a7c15ull;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < PTR_SLOTS; ++i) {
        Entry *e = &table[(h + i) & (PTR_SLOTS - 1)];
        if (e->ptr == (uintptr_t)p) {
            atomic_fetch_sub(&sites[e->site].bytes, e->size); atomic_fetch_sub(&sites[e->site].blocks, 1);
            e->ptr = 1; break;
        }
        if (e->ptr == 0) break;
    }
    pthread_mutex_unlock(&lock);
}
void *malloc(size_t n) {
    if (!real_malloc) { if (!dlsym) return 0; init(); }
    void *p = real_malloc(n); record(p, n); return p;
}
void *calloc(size_t a, size_t b) {
    if (!real_calloc) {  // dlsym itself calls calloc
        size_t n = (a * b + 15) & ~15ul; void *p = boot + boot_used; boot_used += n; return p;
    }
    void *p = real_calloc(a, b); record(p, a * b); return p;
}
void *realloc(void *old, size_t n) {
    if (!real_realloc) init();
    forget(old); void *p = real_realloc(old, n); record(p, n); return p;
}
void free(void *p) {
    if ((char *)p >= boot && (char *)p < boot + sizeof boot) return;
    if (!real_free) init();
    forget(p); real_free(p);
}
