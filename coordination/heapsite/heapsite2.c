// heapsite.c with deeper sites (8 return addresses), the aligned allocators, a live-size histogram,
// and a dump of every site >= 256 KiB live to /work/hs/<seconds>.txt every 10 s (PC diagnostics).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PTR_SLOTS (1u << 25)
#define SITE_SLOTS (1u << 17)
#define DEPTH 8
typedef struct { uintptr_t ptr; uint32_t size; uint32_t site; } Entry;
typedef struct { uintptr_t pc[DEPTH]; _Atomic int64_t bytes, blocks; _Atomic int used; } Site;
static Entry *table;
static Site sites[SITE_SLOTS];
static _Atomic int64_t hist_bytes[40], hist_blocks[40], live_total;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *(*real_malloc)(size_t), *(*real_calloc)(size_t, size_t), *(*real_realloc)(void *, size_t);
static int (*real_posix_memalign)(void **, size_t, size_t);
static void *(*real_aligned_alloc)(size_t, size_t), *(*real_memalign)(size_t, size_t);
static void (*real_free)(void *);
static __thread int inside;
static char boot[1 << 16]; static size_t boot_used;
static struct timespec start;

static void init(void) {
    if (real_malloc) return;
    real_malloc = dlsym(RTLD_NEXT, "malloc"); real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc"); real_free = dlsym(RTLD_NEXT, "free");
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc"); real_memalign = dlsym(RTLD_NEXT, "memalign");
    table = real_calloc(PTR_SLOTS, sizeof(Entry));
    clock_gettime(CLOCK_MONOTONIC, &start);
}
static int size_class(uint64_t n) { int c = 0; while (n > 1 && c < 39) { n >>= 1; ++c; } return c; }
__attribute__((noinline)) static uint32_t site_of(void) {
    void *frames[DEPTH + 3]; int n = backtrace(frames, DEPTH + 3);
    uintptr_t pc[DEPTH];
    uint64_t h = 0;
    for (int i = 0; i < DEPTH; ++i) { pc[i] = n > i + 3 ? (uintptr_t)frames[i + 3] : 0; h = (h ^ pc[i]) * 0x9e3779b97f4a7c15ull; }
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
static void dump(long seconds) {
    char name[64]; mkdir("/work/hs", 0755);
    snprintf(name, sizeof name, "/work/hs/%04ld.txt", seconds);
    FILE *out = fopen(name, "w"); if (!out) return;
    fprintf(out, "live %lld\n", (long long)live_total);
    for (int c = 0; c < 40; ++c)
        if (hist_blocks[c]) fprintf(out, "class %d %lld bytes %lld blocks\n", c, (long long)hist_bytes[c], (long long)hist_blocks[c]);
    for (uint32_t i = 0; i < SITE_SLOTS; ++i) {
        Site *s = &sites[i];
        if (atomic_load(&s->used) != 2 || s->bytes < (256 << 10)) continue;
        fprintf(out, "site %lld %lld", (long long)s->bytes, (long long)s->blocks);
        for (int f = 0; f < DEPTH && s->pc[f]; ++f) {
            Dl_info info; uintptr_t off = 0; const char *obj = "?";
            if (dladdr((void *)s->pc[f], &info) && info.dli_fname) { obj = info.dli_fname; off = s->pc[f] - (uintptr_t)info.dli_fbase; }
            fprintf(out, " %s+0x%lx", strrchr(obj, '/') ? strrchr(obj, '/') + 1 : obj, (unsigned long)off);
        }
        fputc('\n', out);
    }
    fclose(out);
}
__attribute__((noinline)) static void record(void *p, size_t size) {
    if (!p || !table || inside) return;
    inside = 1;
    uint32_t site = site_of();
    atomic_fetch_add(&sites[site].bytes, size); atomic_fetch_add(&sites[site].blocks, 1);
    int c = size_class(size); atomic_fetch_add(&hist_bytes[c], size); atomic_fetch_add(&hist_blocks[c], 1);
    atomic_fetch_add(&live_total, size);
    uint64_t h = (uintptr_t)p * 0x9e3779b97f4a7c15ull;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < PTR_SLOTS; ++i) {
        Entry *e = &table[(h + i) & (PTR_SLOTS - 1)];
        if (e->ptr == 0 || e->ptr == 1) { e->ptr = (uintptr_t)p; e->size = size > 0xffffffffu ? 0xffffffffu : (uint32_t)size; e->site = site; break; }
    }
    pthread_mutex_unlock(&lock);
    static _Atomic unsigned tick; static _Atomic long last;
    if ((atomic_fetch_add(&tick, 1) & 0xfff) == 0) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
        long seconds = now.tv_sec - start.tv_sec, was = atomic_load(&last);
        if (seconds >= was + 10 && atomic_compare_exchange_strong(&last, &was, seconds)) dump(seconds);
    }
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
            int c = size_class(e->size); atomic_fetch_sub(&hist_bytes[c], e->size); atomic_fetch_sub(&hist_blocks[c], 1);
            atomic_fetch_sub(&live_total, e->size);
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
int posix_memalign(void **out, size_t align, size_t n) {
    if (!real_posix_memalign) init();
    int r = real_posix_memalign(out, align, n); if (!r) record(*out, n); return r;
}
void *aligned_alloc(size_t align, size_t n) {
    if (!real_aligned_alloc) init();
    void *p = real_aligned_alloc(align, n); record(p, n); return p;
}
void *memalign(size_t align, size_t n) {
    if (!real_memalign) init();
    void *p = real_memalign(align, n); record(p, n); return p;
}
void free(void *p) {
    if ((char *)p >= boot && (char *)p < boot + sizeof boot) return;
    if (!real_free) init();
    forget(p); real_free(p);
}
