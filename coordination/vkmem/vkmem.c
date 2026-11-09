// vkmem: a Vulkan layer (VK_LAYER_SCOUT_vkmem) that records every vkAllocateMemory / vkFreeMemory with
// its size, memory heap, dedicated image/buffer and call stack, and every second writes the live bytes
// per call stack to $VKMEM_OUT (default /work/vkmem.txt), new stacks to <out>.stacks and the process
// maps to <out>.maps (for symbolizing; vkmem.py turns it into live MiB per Eden subsystem over time).
// For finding what grows in video memory on the PC (coordination/notes/lever-scout.md).
// Enable: VK_ADD_LAYER_PATH=<dir with VkLayer_scout_vkmem.json> VK_INSTANCE_LAYERS=VK_LAYER_SCOUT_vkmem
// shortcut: one VkInstance and one VkDevice (Eden creates one of each); a second device is not tracked.
#define _GNU_SOURCE
#include <execinfo.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#define EXPORT __attribute__((visibility("default")))
enum { FRAMES = 14, STACKS = 1 << 14, ALLOCS = 1 << 20 };

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static PFN_vkAllocateMemory next_alloc;
static PFN_vkFreeMemory next_free;
static VkInstance the_instance;
static VkDevice the_device;
static VkPhysicalDeviceMemoryProperties props;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

struct stack { uint64_t key; void* frames[FRAMES]; int depth, heap, kind; int64_t live, count; int written; };
static struct stack stacks[STACKS];
struct alloc { uint64_t memory; int stack; uint64_t size; };
static struct alloc allocs[ALLOCS];  // open addressing by handle
static FILE *out, *stacks_out;
static char out_path[512];

static uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h; }

static int find_stack(void** frames, int depth, int heap, int kind) {
    uint64_t key = mix(heap, kind);
    for (int i = 0; i < depth; ++i) key = mix(key, (uint64_t)frames[i]);
    for (uint64_t i = key % STACKS, n = 0; n < STACKS; i = (i + 1) % STACKS, ++n) {
        if (stacks[i].key == key) return (int)i;
        if (stacks[i].key == 0) {
            stacks[i].key = key; stacks[i].depth = depth; stacks[i].heap = heap; stacks[i].kind = kind;
            memcpy(stacks[i].frames, frames, sizeof(void*) * depth);
            return (int)i;
        }
    }
    return 0;  // table full: lumped into slot 0
}

static struct alloc* slot(uint64_t memory, int insert) {
    for (uint64_t i = mix(0, memory) % ALLOCS, n = 0; n < ALLOCS; i = (i + 1) % ALLOCS, ++n) {
        if (allocs[i].memory == memory) return &allocs[i];
        if (allocs[i].memory == 0) return insert ? &allocs[i] : NULL;
    }
    return NULL;
}

static void* dumper(void* arg) {
    (void)arg;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int tick = 1;; ++tick) {
        sleep(1);
        pthread_mutex_lock(&lock);
        fprintf(out, "T %d\n", tick);
        for (int i = 0; i < STACKS; ++i) {
            struct stack* s = &stacks[i];
            if (!s->key) continue;
            if (!s->written) {
                fprintf(stacks_out, "S %d %d %d", i, s->heap, s->kind);
                for (int f = 0; f < s->depth; ++f) fprintf(stacks_out, " %p", s->frames[f]);
                fputc('\n', stacks_out);
                s->written = 1;
            }
            if (s->live) fprintf(out, "E %d %lld %lld\n", i, (long long)s->live, (long long)s->count);
        }
        fflush(out);
        fflush(stacks_out);
        pthread_mutex_unlock(&lock);
        if (tick % 30 == 1) {  // the maps, for symbolizing (libraries load late)
            char path[600];
            snprintf(path, sizeof path, "%s.maps", out_path);
            FILE* src = fopen("/proc/self/maps", "r");
            FILE* dst = fopen(path, "w");
            for (int ch; src && dst && (ch = fgetc(src)) != EOF;) fputc(ch, dst);
            if (src) fclose(src);
            if (dst) fclose(dst);
        }
    }
    return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL scout_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
                                                           const VkAllocationCallbacks* allocator, VkDeviceMemory* memory) {
    VkResult result = next_alloc(device, info, allocator, memory);
    if (result != VK_SUCCESS || device != the_device) return result;
    int kind = 0;  // 0 plain, 1 dedicated image, 2 dedicated buffer
    for (const VkBaseInStructure* p = info->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            const VkMemoryDedicatedAllocateInfo* d = (const VkMemoryDedicatedAllocateInfo*)p;
            kind = d->image ? 1 : d->buffer ? 2 : 0;
        }
    void* frames[FRAMES + 1];
    const int depth = backtrace(frames, FRAMES + 1) - 1;  // drop this function
    const int heap = info->memoryTypeIndex < props.memoryTypeCount ? (int)props.memoryTypes[info->memoryTypeIndex].heapIndex : -1;
    pthread_mutex_lock(&lock);
    const int s = find_stack(frames + 1, depth, heap, kind);
    stacks[s].live += (int64_t)info->allocationSize;
    stacks[s].count += 1;
    struct alloc* a = slot((uint64_t)*memory, 1);
    if (a) { a->memory = (uint64_t)*memory; a->stack = s; a->size = info->allocationSize; }
    pthread_mutex_unlock(&lock);
    return result;
}

static VKAPI_ATTR void VKAPI_CALL scout_FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator) {
    if (device == the_device && memory != VK_NULL_HANDLE) {
        pthread_mutex_lock(&lock);
        struct alloc* a = slot((uint64_t)memory, 0);
        if (a) {
            stacks[a->stack].live -= (int64_t)a->size;
            stacks[a->stack].count -= 1;
            a->memory = UINT64_MAX;  // tombstone: keeps later probes of the chain working
        }
        pthread_mutex_unlock(&lock);
    }
    next_free(device, memory, allocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL scout_CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                                           VkInstance* instance) {
    VkLayerInstanceCreateInfo* link = (VkLayerInstanceCreateInfo*)info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo*)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult result = create(info, allocator, instance);
    if (result == VK_SUCCESS) { next_gipa = gipa; the_instance = *instance; }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL scout_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                                         const VkAllocationCallbacks* allocator, VkDevice* device) {
    VkLayerDeviceCreateInfo* link = (VkLayerDeviceCreateInfo*)info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo*)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(the_instance, "vkCreateDevice");
    VkResult result = create(physical, info, allocator, device);
    if (result != VK_SUCCESS || the_device) return result;
    next_gdpa = gdpa;
    next_alloc = (PFN_vkAllocateMemory)gdpa(*device, "vkAllocateMemory");
    next_free = (PFN_vkFreeMemory)gdpa(*device, "vkFreeMemory");
    ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(the_instance, "vkGetPhysicalDeviceMemoryProperties"))(physical, &props);
    const char* path = getenv("VKMEM_OUT");
    snprintf(out_path, sizeof out_path, "%s", path ? path : "/work/vkmem.txt");
    char spath[600];
    snprintf(spath, sizeof spath, "%s.stacks", out_path);
    out = fopen(out_path, "w");
    stacks_out = fopen(spath, "w");
    if (out && stacks_out) {
        fprintf(out, "H");
        for (uint32_t h = 0; h < props.memoryHeapCount; ++h)
            fprintf(out, " %u:%llu:%s", h, (unsigned long long)props.memoryHeaps[h].size,
                    props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device" : "host");
        fputc('\n', out);
        pthread_t t;
        pthread_create(&t, NULL, dumper, NULL);
        pthread_detach(t);
        the_device = *device;
    }
    return result;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL scout_GetDeviceProcAddr(VkDevice device, const char* name);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL scout_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)scout_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)scout_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)scout_CreateInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)scout_CreateDevice;
    if (!strcmp(name, "vkAllocateMemory")) return (PFN_vkVoidFunction)scout_AllocateMemory;
    if (!strcmp(name, "vkFreeMemory")) return (PFN_vkVoidFunction)scout_FreeMemory;
    return next_gipa ? next_gipa(instance, name) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL scout_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)scout_GetDeviceProcAddr;
    if (!strcmp(name, "vkAllocateMemory")) return (PFN_vkVoidFunction)scout_AllocateMemory;
    if (!strcmp(name, "vkFreeMemory")) return (PFN_vkVoidFunction)scout_FreeMemory;
    return next_gdpa(device, name);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = scout_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = scout_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
