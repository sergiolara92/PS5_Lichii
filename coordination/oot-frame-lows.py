#!/usr/bin/env python3
"""oot-frame-lows.py <gameplay-window> <log> [<log> ...]: every frame from the gameplay mark on (the
EDEN_FRAME_TIMES lines, one per 5 s window: 1 ms buckets, frames of 100 ms and more exactly), as
average fps, median fps, 10 % and 1 % lows (the average fps of the slowest 10 % / 1 % of frames),
and the worst frames. Takes the log with the most windows (a restarted app may have begun another)."""
import re
import sys


def windows(path):
    try:
        text = open(path, errors='replace').read()
    except OSError:
        return []
    out = []
    for buckets, longs in re.findall(r'^EDEN_FRAME_TIMES ?(\S*) long=(\S*)$', text, re.M):
        frames = []
        for item in filter(None, buckets.split(',')):
            ms, count = item.split(':')
            frames += [int(ms) + 0.5] * int(count)
        frames += [float(v) for v in longs.split(',') if v]
        out.append(frames)
    return out


def lows(frames):
    frames = sorted(frames)
    n = len(frames)
    total = sum(frames)
    def slowest(share):
        part = frames[-max(1, int(n * share)):]
        return 1000.0 * len(part) / sum(part)
    return {'frames': n, 'avg': 1000.0 * n / total, 'median': 1000.0 / frames[n // 2],
            'low10': slowest(0.10), 'low1': slowest(0.01), 'worst_ms': frames[-5:][::-1]}


def main():
    try:
        start = int(open(sys.argv[1]).read())
    except (OSError, ValueError):
        start = None
    found = max((windows(p) for p in sys.argv[2:]), key=len)
    if start is None or start >= len(found):
        print(f'frame lows: no gameplay windows ({len(found)} windows with frame times)')
        return
    for name, part in (('gameplay', found[start:]), ('last minute', found[-12:])):
        frames = [f for w in part for f in w]
        if not frames:
            continue
        s = lows(frames)
        print(f"{name} frames: {s['frames']}, avg {s['avg']:.1f}, median {s['median']:.1f}, "
              f"10% low {s['low10']:.1f}, 1% low {s['low1']:.1f} fps; worst ms "
              + ' '.join(f'{v:.0f}' for v in s['worst_ms']))


if __name__ == '__main__':
    main()
