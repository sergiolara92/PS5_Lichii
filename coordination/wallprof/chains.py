import sys, collections, re
exec(open('report.py').read().split("for tag, ss in sorted(samples.items()):")[0])
# Most common caller chains (own frames only, innermost first) of GPU samples containing MATCH.
match = sys.argv[4]; depth = int(sys.argv[5]) if len(sys.argv) > 5 else 8
gpu = [t for t, n in tags.items() if n == 'GPU'][0]
chains = collections.Counter(); total = 0
for pcs in samples[gpu]:
    frames = [fn(a, k) for k, a in enumerate(pcs[2:])]
    if not any(match in f for f in frames): continue
    total += 1
    own = [re.sub(r'<.*', '', f)[-60:] for f in frames if not ('.so' in f) and not f.startswith('0x') and f != '?']
    i = next(i for i, f in enumerate(own) if match in f) if any(match in f for f in own) else 0
    chains[' < '.join(own[i:i + depth])] += 1
n = len(samples[gpu])
print(f'{total} of {n} GPU samples ({total*100/n:.1f}%) contain {match}')
for c, k in chains.most_common(12): print(f'{k*100/n:5.1f}%  {c}')
