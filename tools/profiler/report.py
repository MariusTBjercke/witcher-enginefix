# Summarises a sampler.exe CSV: hottest threads, modules and witcher.exe functions (self and inclusive).
# Usage: python report.py <samples.csv> [functions.txt] [top_n]
import bisect
import collections
import sys

samples_path = sys.argv[1]
funcs_path = sys.argv[2] if len(sys.argv) > 2 else __file__.replace("report.py", "functions.txt")
top_n = int(sys.argv[3]) if len(sys.argv) > 3 else 25

starts, names = [], []
for line in open(funcs_path, encoding="utf-8"):
    name, _, addr = line.strip().rpartition(" at ")
    if name:
        starts.append(int(addr, 16))
        names.append(name)
order = sorted(range(len(starts)), key=starts.__getitem__)
starts = [starts[i] for i in order]
names = [names[i] for i in order]

rows, mods = [], []
for line in open(samples_path, encoding="utf-8"):
    parts = line.strip().split(",")
    if parts[0] == "#mod":
        mods.append((int(parts[1], 16), int(parts[2], 16), parts[3]))
    elif len(parts) >= 3:
        rows.append((int(parts[1]), [int(a, 16) for a in parts[2:]]))
mods.sort()
exe = next((m for m in mods if m[2].lower() == "witcher.exe"), (0x400000, 0xB00000, "witcher.exe"))


def module(a):
    for base, size, name in mods:
        if base <= a < base + size:
            return name
    return "?"


def func(a):
    if not exe[0] <= a < exe[0] + exe[1]:
        return None
    i = bisect.bisect_right(starts, a) - 1
    return names[i] if i >= 0 else None


total = len(rows)
by_thread = collections.Counter(t for t, _ in rows)
print(f"{total} samples")
print("\nThreads:")
for t, n in by_thread.most_common(8):
    print(f"  tid {t:6}  {n:6}  {100 * n / total:5.1f}%")

main_tid = by_thread.most_common(1)[0][0]
for label, sel in (("ALL THREADS", None), (f"BUSIEST THREAD {main_tid}", main_tid)):
    sub = [s for t, s in rows if sel is None or t == sel]
    n = len(sub)
    self_mod = collections.Counter(module(s[0]) for s in sub)
    self_fn, incl_fn = collections.Counter(), collections.Counter()
    for s in sub:
        # self: first witcher.exe frame (time in CRT/driver is charged to the engine function that called it)
        first = next((f for f in map(func, s) if f), None)
        if first:
            self_fn[first] += 1
        for f in {f for f in map(func, s) if f}:
            incl_fn[f] += 1
    print(f"\n===== {label}: {n} samples")
    print("Modules (where EIP was):")
    for m, c in self_mod.most_common(10):
        print(f"  {100 * c / n:5.1f}%  {m}")
    print("witcher.exe functions, self (incl. CRT/OS calls they make):")
    for f, c in self_fn.most_common(top_n):
        print(f"  {100 * c / n:5.1f}%  {f}")
    print("witcher.exe functions, inclusive:")
    for f, c in incl_fn.most_common(top_n):
        print(f"  {100 * c / n:5.1f}%  {f}")
