# Lists the table of contents of a .TheWitcherSave file (RGMH container).
# Trailer (last 12 bytes): toc_offset, data_end?, entry_count. Entry: u32 name_len, name, u32 offset, u32 size.
# Usage: python savetoc.py [save file]   (default: newest save)
import glob
import os
import struct
import sys

if len(sys.argv) > 1:
    path = sys.argv[1]
else:
    saves = glob.glob(os.path.expanduser("~/Documents/The Witcher/saves/*.TheWitcherSave"))
    path = max(saves, key=os.path.getmtime)

data = open(path, "rb").read()
magic, version, header_size, _ = struct.unpack_from("<4sIII", data, 0)
trailer = struct.unpack_from("<3I", data, len(data) - 12)
print(f"{os.path.basename(path)}: {len(data)} bytes, {magic} v{version}, header {header_size:#x}, trailer {[hex(x) for x in trailer]}")

for toc in trailer[:2]:
    try:
        entries, p = [], toc
        for _ in range(trailer[2]):
            (n,) = struct.unpack_from("<I", data, p)
            name = data[p + 4:p + 4 + n].decode("latin1")
            size, off = struct.unpack_from("<2I", data, p + 4 + n)  # size first, then offset
            entries.append((name.split(chr(92))[-1], off, size))
            p += 12 + n
    except (struct.error, UnicodeDecodeError):
        continue
    if all(0 < off < len(data) for _, off, _ in entries):
        break
else:
    sys.exit("could not parse TOC")

print(f"TOC at {toc:#x}, {len(entries)} entries")
for name, off, size in sorted(entries, key=lambda e: e[1])[:6]:
    print(f"  first  {off:#010x} {size:>9}  {name}")
for name, off, size in sorted(entries, key=lambda e: -e[2])[:6]:
    print(f"  big    {off:#010x} {size:>9}  {name}")
for name, off, size in entries:
    if any(k in name.lower() for k in ("nfo", "screen", "portrait", ".tga", ".dds", ".smm", "player")):
        print(f"  meta   {off:#010x} {size:>9}  {name}")
