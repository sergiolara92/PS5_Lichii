#!/usr/bin/env python3
# vkmem.py <run dir> <eden-headless> [every s=30]: live device memory from the vkmem layer, by the Eden
# code that asked for it (the first frame of the app above the allocator plumbing), grouped into
# categories, over time; then the call sites that grew most after the menus and whether they came down.
import collections, re, subprocess, sys
run, binary = sys.argv[1], sys.argv[2]
every = int(sys.argv[3]) if len(sys.argv) > 3 else 30

heaps = {}
ticks = []  # (t, {stack: live})
cur = None
for line in open(f'{run}/vkmem.txt'):
    f = line.split()
    if f[0] == 'H':
        for h in f[1:]:
            i, size, kind = h.split(':'); heaps[int(i)] = kind
    elif f[0] == 'T':
        cur = {}; ticks.append((int(f[1]), cur))
    elif f[0] == 'E' and cur is not None:
        cur[int(f[1])] = int(f[2])
stacks = {}
for line in open(f'{run}/vkmem.txt.stacks'):
    f = line.split()
    stacks[int(f[1])] = (int(f[2]), int(f[3]), [int(x, 16) for x in f[4:]])
maps = []
for line in open(f'{run}/vkmem.txt.maps'):
    m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \S+\s*(.*)', line)
    if m and 'x' in m.group(3):
        maps.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(4), 16), m.group(5).strip()))
segs = []
for l in subprocess.run(['readelf', '-lW', binary], capture_output=True, text=True).stdout.split('\n'):
    f = l.split()
    if f and f[0] == 'LOAD':
        segs.append((int(f[1], 16), int(f[2], 16), int(f[4], 16)))
def where(ip):
    for s, e, off, name in maps:
        if s <= ip < e:
            o = ip - s + off
            if name.endswith('eden-headless'):
                for a, v, size in segs:
                    if a <= o < a + size:
                        return 'app', o - a + v
            return name.split('/')[-1] or '?', o
    return '?', ip
app_addrs = sorted({where(ip)[1] - 1 for _, _, fr in stacks.values() for ip in fr if where(ip)[0] == 'app'})
sym = {}
if app_addrs:
    # Return addresses - 1: the call instruction, not the one after it.
    out = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', binary] + [hex(a) for a in app_addrs],
                         capture_output=True, text=True).stdout.split('\n')
    for i, a in enumerate(app_addrs):
        sym[a] = re.sub(r'\(.*', '', out[2 * i].replace('(anonymous namespace)', '{anon}'))
PLUMBING = re.compile(r'Vma|vma[A-Z_]|MemoryAllocator|MemoryCommit|^Vulkan::vk::|^vk::|AllocateMemory|'
                      r'std::|operator new|__|Common::Slot|^Vulkan::Device::|CreateBuffer$|CreateImage$|'
                      r'^Vulkan::Buffer::Buffer$|^Vulkan::Image::Image$|^Vulkan::\{anon\}::|MakeImage|MakeBuffer')
def owner(frames):
    names = []
    for ip in frames:
        mod, a = where(ip)
        if mod == 'app':
            n = sym.get(a - 1, '?')
            names.append(n)
            if not PLUMBING.search(n):
                return n, names
    return (names[-1] if names else '(outside the app)'), names
CATS = [('texture cache', r'TextureCache|Image|ImageView|Sampler|Framebuffer|RenderPass|texture|ASTC|Transcode|Decode'),
        ('buffer cache', r'BufferCache|Buffer<|BufferCacheRuntime|UniformRing|StreamBuffer'),
        ('staging', r'Staging'),
        ('GPU-only / sparse', r'Sparse|GpuOnly|nx2_gpu|SubCreate|Dense'),
        ('descriptors', r'Descriptor'),
        ('pipelines / shaders', r'Pipeline|Shader'),
        ('scheduler / command buffers', r'Scheduler|CommandPool|MasterSemaphore|Fence|QueryPool|Query'),
        ('presentation', r'Present|Swapchain|Blit|Layer|Composite|FSR|SMAA|FXAA')]
def category(name):
    for c, rx in CATS:
        if re.search(rx, name):
            return c
    return 'other: ' + name[:60]
own = {s: owner(fr) for s, (_, _, fr) in stacks.items()}
cat = {s: category(own[s][0]) for s in stacks}
kindname = {0: '', 1: ' (dedicated image)', 2: ' (dedicated buffer)'}

times = [t for t, _ in ticks]
sel = [i for i, t in enumerate(times) if t % every == 0] + [len(ticks) - 1]
cats = collections.defaultdict(dict)
for i in sel:
    t, live = ticks[i]
    acc = collections.Counter()
    for s, v in live.items():
        heap = stacks.get(s, (-1, 0, []))[0]
        acc[cat.get(s, '?') + ('' if heaps.get(heap) == 'device' else ' [host heap]')] += v
    for c, v in acc.items():
        cats[c][t] = v
print(f'{len(ticks)} s of samples; heaps {heaps}')
print('live MiB by category, at t (s after device creation):')
print(f'{"":44}' + ''.join(f'{ticks[i][0]:>7}' for i in sel))
for c in sorted(cats, key=lambda c: -max(cats[c].values())):
    print(f'{c[:44]:44}' + ''.join(f'{cats[c].get(ticks[i][0], 0) / 2**20:7.0f}' for i in sel))
tot = [sum(v for s, v in ticks[i][1].items() if heaps.get(stacks.get(s, (-1,))[0]) == 'device') for i in sel]
print(f'{"device-local total":44}' + ''.join(f'{v / 2**20:7.0f}' for v in tot))
# Growth by call site: from the last sample before t0 to the peak, and the end value.
t0 = int(sys.argv[4]) if len(sys.argv) > 4 else 180
base = next((live for t, live in reversed(ticks) if t <= t0), {})
peak = collections.Counter(); end = ticks[-1][1]
for t, live in ticks:
    for s, v in live.items():
        peak[s] = max(peak[s], v)
grow = sorted(((peak[s] - base.get(s, 0), s) for s in peak), reverse=True)[:15]
print(f'\ncall sites that grew most after {t0} s (MiB: at {t0} s -> peak -> end; live allocations at end):')
for g, s in grow:
    if g < 16 * 2**20: break
    heap, kind, _ = stacks[s]
    print(f'  {base.get(s, 0)/2**20:6.0f} -> {peak[s]/2**20:6.0f} -> {end.get(s, 0)/2**20:6.0f}  {cat[s]:24} {own[s][0][:80]}{kindname[kind]}'
          + ('' if heaps.get(heap) == 'device' else ' [host heap]'))
    print('        via ' + ' <- '.join(n[:50] for n in own[s][1][:4]))
