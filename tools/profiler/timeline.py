# Main-thread timeline for a sampler.exe CSV: how busy the main thread is per second, and what it does
# during long busy stretches (hitches / loading).
# Usage: python timeline.py <samples.csv> <main_tid>
import bisect
import collections
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
path, main_tid = sys.argv[1], sys.argv[2]

mods, rows = [], []
for line in open(path, encoding="utf-8"):
    p = line.strip().split(",")
    if p[0] == "#mod":
        mods.append((int(p[1], 16), int(p[2], 16), p[3].lower()))
    elif len(p) >= 3 and p[1] == main_tid:
        rows.append((int(p[0]), [int(a, 16) for a in p[2:]]))
mods.sort()


def module(a):
    for base, size, name in mods:
        if base <= a < base + size:
            return name, a - base
    return "?", a


starts, names = [], []
for line in open(os.path.join(here, "functions.txt"), encoding="utf-8"):
    n, _, a = line.strip().rpartition(" at ")
    if n:
        starts.append(int(a, 16))
        names.append(n)
order = sorted(range(len(starts)), key=starts.__getitem__)
starts, names = [starts[i] for i in order], [names[i] for i in order]

dx = sorted((int(r, 16), n) for r, n in (l.split() for l in open(os.path.join(here, "d3dx9_exports.txt"))))
dx_rva = [r for r, _ in dx]


def label(stack):
    """Best description of what a sample is doing."""
    mods_in = [module(a) for a in stack]
    top = mods_in[0][0]
    dx_frames = [rva for m, rva in mods_in if m == "d3dx9_35.dll"]
    if dx_frames:
        # Nearest-export names are unreliable (ID3DXEffect methods etc. are not exported), so name the
        # d3dx9 work by the witcher.exe function that called into it.
        caller = next((a for a in stack if module(a)[0] == "witcher.exe"), None)
        if caller is None:
            return "d3dx9<-?"
        i = bisect.bisect_right(starts, caller) - 1
        return "d3dx9<-" + names[i]
    if top in ("ntdll.dll", "kernelbase.dll", "win32u.dll"):
        caller = next((f"{m}+{rva:x}" for m, rva in mods_in[1:] if m not in ("ntdll.dll", "kernelbase.dll", "kernel32.dll", "win32u.dll")), "?")
        return "wait<-" + caller
    if top == "witcher.exe":
        i = bisect.bisect_right(starts, stack[0]) - 1
        return "witcher:" + names[i]
    return top


labels = [(t, label(s)) for t, s in rows]
counts = collections.Counter(l for _, l in labels)
idle_key = counts.most_common(1)[0][0] if counts else ""
print(f"{len(rows)} main-thread samples; most common state (treated as idle/frame-limiter): {idle_key}")
print("\nTop states:")
for k, c in counts.most_common(15):
    print(f"  {100 * c / len(rows):5.1f}%  {k}")

print("\nBusy % per 5 s (main thread not in the idle state):")
buckets = collections.defaultdict(lambda: [0, 0])
for t, l in labels:
    b = buckets[t // 5000]
    b[1] += 1
    b[0] += l != idle_key
line = []
for k in sorted(buckets):
    busy, tot = buckets[k]
    line.append(f"{k * 5:>4}s {100 * busy // max(tot, 1):3d}%")
for i in range(0, len(line), 6):
    print("  " + " | ".join(line[i:i + 6]))

print("\nBusy stretches >= 100 ms (likely hitches or loading):")
run = None
stretches = []
for t, l in labels:
    if l != idle_key:
        if run is None:
            run = [t, t, collections.Counter()]
        run[1] = t
        run[2][l] += 1
    elif run:
        stretches.append(run)
        run = None
if run:
    stretches.append(run)
for a, b, c in stretches:
    if b - a >= 100:
        print(f"  {a / 1000:7.2f}s  {b - a:6d} ms  " + ", ".join(f"{k} {v}" for k, v in c.most_common(3)))
