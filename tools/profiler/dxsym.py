# Names the d3dx9_35.dll time of main-thread samples with Microsoft's public PDB (via cdb), because
# nearest-export names are wrong for d3dx9 (see AGENTS.md).
# Usage: python dxsym.py <samples.csv> <main_tid> <t0_ms>-<t1_ms> [...] [--sym <symbol path>]
# Default symbol path downloads the PDB from Microsoft's symbol server into .\symbols.
import collections
import os
import re
import subprocess
import sys
import tempfile

CDB = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe"
DLL = r"C:\Windows\SysWOW64\d3dx9_35.dll"

args = sys.argv[1:]
sym = "srv*symbols*https://msdl.microsoft.com/download/symbols"
if "--sym" in args:
    i = args.index("--sym")
    sym = args[i + 1]
    del args[i:i + 2]
path, tid = args[0], args[1]
wins = [tuple(int(x) for x in w.split("-")) for w in args[2:]] or [(0, 1 << 62)]

mods, rows = [], []
for line in open(path, encoding="utf-8"):
    p = line.strip().split(",")
    if p[0] == "#mod":
        mods.append((int(p[1], 16), int(p[2], 16), p[3].lower()))
    elif len(p) >= 3 and p[1] == tid and any(a <= int(p[0]) <= b for a, b in wins):
        rows.append([int(a, 16) for a in p[2:]])


def module(a):
    for base, size, name in mods:
        if base <= a < base + size:
            return name, a - base
    return "?", a


leaf, incl, top = collections.Counter(), collections.Counter(), collections.Counter()
for s in rows:
    m = [module(a) for a in s]
    dx = [rva for name, rva in m if name == "d3dx9_35.dll"]
    if not dx:
        continue
    leaf[dx[0]] += 1  # first d3dx9 frame: the d3dx9 function running or calling out
    top[m[0][0] if m[0][0] != "d3dx9_35.dll" else "(d3dx9 itself)"] += 1
    for rva in set(dx):
        incl[rva] += 1

rvas = sorted(set(leaf) | set(incl))
with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
    f.write("".join(f".echo RVA {r:x}; ln d3dx9_35+0x{r:x};\n" for r in rvas) + "q\n")
    cmdfile = f.name
out = subprocess.run([CDB, "-z", DLL, "-y", sym, "-cf", cmdfile], capture_output=True, text=True, errors="replace").stdout
os.unlink(cmdfile)
name, cur = {}, None
for line in out.splitlines():
    m = re.match(r"RVA ([0-9a-f]+)", line)
    if m:
        cur = int(m.group(1), 16)
    elif cur is not None and cur not in name and (m := re.match(r"\([0-9a-f`]+\)\s+(\S+)", line)):
        name[cur] = re.sub(r"\+0x[0-9a-f]+$", "", m.group(1))

total = sum(leaf.values())
print(f"{len(rows)} samples in the windows, {total} with d3dx9 frames; top of stack: {top.most_common(6)}")
for title, counter in (("By first d3dx9 frame", leaf), ("Inclusive (EBP chains may be incomplete)", incl)):
    byfn = collections.Counter()
    for r, c in counter.items():
        byfn[name.get(r, f"?{r:x}")] += c
    print(f"\n{title}:")
    for fn, c in byfn.most_common(25):
        print(f"  {100 * c / max(total, 1):5.1f}%  {fn}")
