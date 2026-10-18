"""Reads The Witcher EE resource archives: KEY V1.1 + BIFF V1.1, ERF/MOD V1.0, and GFF V3.2 files.

Usage (python -I tools\\witcherres.py ...):
  keys <Data dir> [name filter]          list resources from main.key and M*_*.key (name, type, bif)
  get  <Data dir> <resref.ext> <out>     extract one resource found through the KEY files
  erf  <file.mod> [name filter]          list an ERF/MOD archive
  erfget <file.mod> <resref.ext> <out>   extract one resource from an ERF/MOD
  gff  <file> [maxdepth]                 dump a GFF file (.git .are .dlg .utd .qst ...) as indented text
  find <file-or-dir> <text>              list GFF fields whose value contains text (case-insensitive)

Resource type numbers are not mapped to extensions for KEY archives (the Witcher adds many new
types); `keys` prints the number and `get` matches by resref plus the number or by resref alone.
"""
import os
import struct
import sys

ERF_TYPES = {  # the common Aurora type ids
    2002: "mdl", 2009: "nss", 2010: "ncs", 2012: "are", 2017: "2da", 2023: "git", 2025: "uti",
    2027: "utc", 2029: "dlg", 2030: "itp", 2032: "utt", 2033: "dds", 2035: "uts", 2037: "gff",
    2038: "fac", 2040: "ute", 2042: "utd", 2044: "utp", 2046: "gic", 2047: "gui", 2051: "utm",
    2056: "jrl", 2058: "utw", 2064: "ndb", 2065: "ptm", 2066: "ptt", 3000: "lyt", 3001: "vis",
    3002: "rim", 2070: "ifo", 2014: "ifo", 2015: "bic", 2016: "wok", 2018: "txi", 2019: "git",
}


def rd(data, off, fmt):
    return struct.unpack_from("<" + fmt, data, off)


# ---- KEY / BIF -------------------------------------------------------------------------

def read_key(path):
    data = open(path, "rb").read()
    if data[:8] != b"KEY V1.1":
        raise ValueError(f"{path}: not KEY V1.1")
    bif_count, key_count, _unk, off_file, off_res = rd(data, 8, "5I")
    bifs = []
    for i in range(bif_count):
        size, name_off, name_size = rd(data, off_file + i * 12, "3I")
        bifs.append(data[name_off:name_off + name_size].split(b"\0")[0].decode("latin-1"))
    res = []
    for i in range(key_count):
        o = off_res + i * 26
        name = data[o:o + 16].split(b"\0")[0].decode("latin-1").lower()
        rtype, rid, flags = rd(data, o + 16, "HII")
        bif_index = flags >> 20
        res.append((name, rtype, bifs[bif_index] if bif_index < len(bifs) else f"?{bif_index}", rid))
    return res


def read_bif_entry(path, index):
    with open(path, "rb") as f:
        head = f.read(20)
        if head[:8] != b"BIFFV1.1":
            raise ValueError(f"{path}: not BIFF V1.1")
        var_count, _fixed, off_var = rd(head, 8, "3I")
        f.seek(off_var)
        table = f.read(var_count * 20)
        for i in range(var_count):
            rid, _flags, off, size, rtype = rd(table, i * 20, "5I")
            if (rid & 0xFFFFF) == index:
                f.seek(off)
                return f.read(size)
    raise KeyError(f"{path}: no resource {index}")


def all_keys(data_dir):
    names = ["main.key"] + sorted(n for n in os.listdir(data_dir) if n.lower().startswith(("m1_", "m2_")) and n.lower().endswith(".key"))
    out = []
    for n in names:
        p = os.path.join(data_dir, n)
        if os.path.exists(p):
            for name, rtype, bif, rid in read_key(p):
                out.append((name, rtype, bif, rid, n))
    return out


# ---- ERF / MOD --------------------------------------------------------------------------

def read_erf(path):
    data = open(path, "rb").read()
    sig = data[:8]
    if sig[4:] not in (b"V1.0", b"V1.1"):
        raise ValueError(f"{path}: unknown ERF {sig!r}")
    _lang, _locsize, count, _offloc, off_keys, off_res = rd(data, 8, "6I")
    entries = []
    for i in range(count):
        o = off_keys + i * 24
        name = data[o:o + 16].split(b"\0")[0].decode("latin-1").lower()
        rid, rtype = rd(data, o + 16, "IH")
        off, size = rd(data, off_res + rid * 8, "2I")
        entries.append((name, rtype, off, size))
    return data, entries


def erf_name(name, rtype):
    return f"{name}.{ERF_TYPES.get(rtype, str(rtype))}"


# ---- GFF ----------------------------------------------------------------------------------

class Gff:
    def __init__(self, data):
        self.d = data
        self.ftype = data[:8].decode("latin-1")
        (self.so, self.sc, self.fo, self.fc, self.lo, self.lc, self.fdo, self.fdc,
         self.fio, self.fic, self.lio, self.lic) = rd(data, 8, "12I")
        if not self.ftype.endswith("V3.2") and not self.ftype.endswith("V3.3"):
            raise ValueError(f"not a GFF: {self.ftype!r}")

    def label(self, i):
        o = self.lo + i * 16
        return self.d[o:o + 16].split(b"\0")[0].decode("latin-1")

    def struct(self, i):
        stype, data_or_off, count = rd(self.d, self.so + i * 12, "3I")
        if count == 1:
            idx = [data_or_off]
        else:
            idx = list(rd(self.d, self.fio + data_or_off, f"{count}I")) if count else []
        return stype, [self.field(j) for j in idx]

    def field(self, i):
        ftype, lab, val = rd(self.d, self.fo + i * 12, "3I")
        name = self.label(lab)
        d, fd = self.d, self.fdo + val
        if ftype in (0, 2, 4):
            v = val & (0xFF if ftype == 0 else 0xFFFF if ftype == 2 else 0xFFFFFFFF)
        elif ftype == 1:
            v = struct.unpack("<b", struct.pack("<B", val & 0xFF))[0]
        elif ftype == 3:
            v = struct.unpack("<h", struct.pack("<H", val & 0xFFFF))[0]
        elif ftype == 5:
            v = struct.unpack("<i", struct.pack("<I", val))[0]
        elif ftype == 6:
            v = rd(d, fd, "Q")[0]
        elif ftype == 7:
            v = rd(d, fd, "q")[0]
        elif ftype == 8:
            v = struct.unpack("<f", struct.pack("<I", val))[0]
        elif ftype == 9:
            v = rd(d, fd, "d")[0]
        elif ftype == 10:
            n = rd(d, fd, "I")[0]
            v = d[fd + 4:fd + 4 + n].decode("latin-1")
        elif ftype == 11:
            n = d[fd]
            v = d[fd + 1:fd + 1 + n].decode("latin-1")
        elif ftype == 12:
            _total, strref, cnt = rd(d, fd, "3I")
            parts, o = [], fd + 12
            for _ in range(cnt):
                sid, n = rd(d, o, "2I")
                parts.append(f"[{sid}]" + d[o + 8:o + 8 + n].decode("latin-1"))
                o += 8 + n
            v = f"strref {strref} " + " ".join(parts) if parts or strref != 0xFFFFFFFF else ""
        elif ftype == 13:
            n = rd(d, fd, "I")[0]
            v = f"<void {n} bytes>"
        elif ftype == 14:
            v = ("struct", self.struct(val))
        elif ftype == 15:
            o = self.lio + val
            n = rd(d, o, "I")[0]
            v = ("list", [self.struct(s) for s in rd(d, o + 4, f"{n}I")]) if n else ("list", [])
        elif ftype == 16:
            v = rd(d, fd, "4f")
        elif ftype == 17:
            v = rd(d, fd, "3f")
        elif ftype == 18:
            v = f"strref {rd(d, fd + 4, 'I')[0]}"
        else:
            v = f"<type {ftype} raw {val:#x}>"
        return name, v

    def root(self):
        return self.struct(0)


def dump(node, indent=0, maxdepth=99, out=sys.stdout):
    stype, fields = node
    for name, v in fields:
        pad = "  " * indent
        if isinstance(v, tuple) and v and v[0] == "struct":
            out.write(f"{pad}{name}: struct {v[1][0]}\n")
            if indent < maxdepth:
                dump(v[1], indent + 1, maxdepth, out)
        elif isinstance(v, tuple) and v and v[0] == "list":
            out.write(f"{pad}{name}: list[{len(v[1])}]\n")
            if indent < maxdepth:
                for k, s in enumerate(v[1]):
                    out.write(f"{pad}  [{k}] struct {s[0]}\n")
                    dump(s, indent + 2, maxdepth, out)
        else:
            out.write(f"{pad}{name} = {v!r}\n")


def find(node, text, path=""):
    stype, fields = node
    for name, v in fields:
        p = f"{path}/{name}"
        if isinstance(v, tuple) and v and v[0] == "struct":
            yield from find(v[1], text, p)
        elif isinstance(v, tuple) and v and v[0] == "list":
            for k, s in enumerate(v[1]):
                yield from find(s, text, f"{p}[{k}]")
        elif text in str(v).lower():
            yield p, v


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == "keys":
        flt = argv[3].lower() if len(argv) > 3 else ""
        for name, rtype, bif, rid, key in all_keys(argv[2]):
            if flt in name:
                print(f"{name:20} type {rtype:5} {bif} #{rid} ({key})")
    elif cmd == "get":
        want = argv[3].lower()
        base, _, ext = want.partition(".")
        hits = [r for r in all_keys(argv[2]) if r[0] == base and (not ext or ext == str(r[1]) or ERF_TYPES.get(r[1]) == ext)]
        if not hits:
            print("not found")
            return 1
        name, rtype, bif, rid, key = hits[-1]
        data = read_bif_entry(os.path.join(argv[2], bif.replace("\\", os.sep).split(os.sep)[-1] if not os.path.exists(os.path.join(argv[2], bif)) else bif), rid)
        open(argv[4], "wb").write(data)
        print(f"{name} type {rtype} from {bif} ({key}): {len(data)} bytes -> {argv[4]}")
    elif cmd == "erf":
        flt = argv[3].lower() if len(argv) > 3 else ""
        _, entries = read_erf(argv[2])
        for name, rtype, off, size in entries:
            if flt in name:
                print(f"{erf_name(name, rtype):24} type {rtype:5} {size:9} bytes")
    elif cmd == "erfget":
        data, entries = read_erf(argv[2])
        want = argv[3].lower()
        for name, rtype, off, size in entries:
            if erf_name(name, rtype) == want or name == want:
                open(argv[4], "wb").write(data[off:off + size])
                print(f"{erf_name(name, rtype)}: {size} bytes -> {argv[4]}")
                return 0
        print("not found")
        return 1
    elif cmd == "gff":
        g = Gff(open(argv[2], "rb").read())
        print(g.ftype)
        dump(g.root(), 0, int(argv[3]) if len(argv) > 3 else 99)
    elif cmd == "find":
        text = argv[3].lower()
        paths = [argv[2]] if os.path.isfile(argv[2]) else [os.path.join(r, f) for r, _, fs in os.walk(argv[2]) for f in fs]
        for p in paths:
            try:
                g = Gff(open(p, "rb").read())
            except Exception:
                continue
            for fp, v in find(g.root(), text):
                print(f"{p}: {fp} = {v!r}")
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
