import sys, collections, re
exec(open('report.py').read().split("for tag, ss in sorted(samples.items()):")[0])
# svcinside.py <base> <binary>: the guest-core threads' wall-clock samples inside Kernel::Svc::Call, by SVC handler
# (the frame below Call) and by the innermost frame (own code or library), as shares of all their samples.
cores = [t for t, n in tags.items() if n.startswith('CPUCore')]
by_svc, leaf, path = collections.Counter(), collections.Counter(), collections.Counter()
total = sum(len(samples[t]) for t in cores); inside = 0
for t in cores:
    for pcs in samples[t]:
        frames = [fn(a, k) for k, a in enumerate(pcs[2:])]
        idx = [i for i, f in enumerate(frames) if 'Kernel::Svc::Call' in f]
        if not idx: continue
        inside += 1
        i = idx[0]
        handler = next((frames[j] for j in range(i - 1, -1, -1) if 'Svc::' in frames[j] and 'Call' not in frames[j]), frames[i - 1] if i else '?')
        by_svc[re.sub(r'\(.*', '', handler)[:70]] += 1
        leaf[re.sub(r'\(.*', '', frames[0])[:80]] += 1
        own = [re.sub(r'\(.*', '', f)[:45] for f in frames[:i] if '.so' not in f and not f.startswith('0x') and f != '?']
        path[' <- '.join(own[:3])] += 1
print(f'{inside * 100 / total:.1f}% of guest-core samples inside Svc::Call ({inside} of {total})')
print('-- by handler'); [print(f'{c * 100 / total:5.2f}%  {f}') for f, c in by_svc.most_common(14)]
print('-- innermost frame'); [print(f'{c * 100 / total:5.2f}%  {f}') for f, c in leaf.most_common(22)]
print('-- innermost own frames'); [print(f'{c * 100 / total:5.2f}%  {f}') for f, c in path.most_common(18)]
