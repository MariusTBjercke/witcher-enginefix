# Group a sampler.exe CSV by full call-stack signature, per thread.
#
# timeline.py collapses stacks to one label; this prints the exact chains, which is what identifies
# "who is the main thread waiting on" (e.g. the DXVK present frame-latency signal: the deepest frame is
# our own dinput8.dll Present hook).
#
# Usage: python -I tools\profiler\stacks.py <samples.csv> [tid] [top]
#   tid: only that thread (default: every thread, together with the sample count)
#   top: how many stacks per thread (default 12)
#
# Stacks are EIP first, then EBP-chain return addresses (innermost to outermost).
import bisect
import collections
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
path = sys.argv[1]
want = sys.argv[2] if len(sys.argv) > 2 else None
top = int(sys.argv[3]) if len(sys.argv) > 3 else 12

mods, rows = [], collections.defaultdict(list)
for line in open(path, encoding="utf-8"):
    p = line.strip().split(",")
    if len(p) < 3:
        continue
    if p[0] == "#mod":
        mods.append((int(p[1], 16), int(p[2], 16), p[3].lower()))
    else:
        rows[p[1]].append((int(p[0]), [int(a, 16) for a in p[2:] if a]))
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


def sym(a):
    m, rva = module(a)
    if m == "witcher.exe":
        i = bisect.bisect_right(starts, a) - 1
        return names[i] if i >= 0 else f"witcher+{rva:x}"
    return f"{m}+{rva:x}"


total = sum(len(v) for v in rows.values())
for tid, samples in sorted(rows.items(), key=lambda kv: -len(kv[1])):
    if want and tid != want:
        continue
    c = collections.Counter(" <- ".join(sym(a) for a in s) for _, s in samples)
    print(f"\n===== tid {tid}: {len(samples)} samples ({100 * len(samples) / total:.1f}%), "
          f"{len(c)} distinct stacks")
    for sig, n in c.most_common(top):
        print(f"  {100 * n / len(samples):5.1f}%  {n:5d}  {sig}")
