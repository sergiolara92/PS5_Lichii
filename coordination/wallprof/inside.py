import sys, collections, re
exec(open('report.py').read().split("for tag, ss in sorted(samples.items()):")[0])
# Within samples containing MATCH: the innermost own (eden-headless) frame, and callees of MATCH.
match = sys.argv[4]
gpu = [t for t, n in tags.items() if n == 'GPU'][0]
leaf = collections.Counter(); child = collections.Counter(); total = 0
for pcs in samples[gpu]:
    frames = [fn(a, k) for k, a in enumerate(pcs[2:])]
    idx = [i for i, f in enumerate(frames) if match in f]
    if not idx: continue
    total += 1
    own = [f for f in frames if '.so' not in f and not f.startswith('0x') and f != '?']
    leaf[(own[0] if own else frames[0])[:90]] += 1
    i = idx[0]
    if i > 0: child[frames[i - 1][:90]] += 1
n = len(samples[gpu])
print(f'{total*100/n:.1f}% of GPU samples in {match}')
print('-- innermost own frame'); [print(f'{c*100/n:5.1f}%  {f}') for f, c in leaf.most_common(25)]
print('-- direct callees'); [print(f'{c*100/n:5.1f}%  {f}') for f, c in child.most_common(15)]
