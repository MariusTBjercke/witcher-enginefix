# Minimal Lua 5.0 bytecode (.luc) disassembler for The Witcher's GUI scripts.
# Usage: python luac50.py <file.luc> [function-name-filter]
import struct
import sys

OPS = ["MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETGLOBAL", "GETTABLE", "SETGLOBAL", "SETUPVAL",
       "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "POW", "UNM", "NOT", "CONCAT", "JMP", "EQ",
       "LT", "LE", "TEST", "CALL", "TAILCALL", "RETURN", "FORLOOP", "TFORLOOP", "TFORPREP", "SETLIST", "SETLISTO",
       "CLOSE", "CLOSURE"]
MAXSTACK = 250  # RK operands >= MAXSTACK are constants in 5.0


class Reader:
    def __init__(self, data):
        self.d, self.p = data, 0

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.d, self.p)
        self.p += struct.calcsize(fmt)
        return v[0] if len(v) == 1 else v

    def string(self):
        n = self.take("I")
        s = self.d[self.p:self.p + n - 1].decode("latin1") if n else None
        self.p += n
        return s


def load_function(r):
    f = {"source": r.string(), "line": r.take("i"), "nups": r.take("B"), "nparams": r.take("B"),
         "vararg": r.take("B"), "maxstack": r.take("B")}
    n_lines = r.take("i")
    r.p += 4 * n_lines  # line info
    f["locals"] = []
    for _ in range(r.take("i")):
        f["locals"].append((r.string(), r.take("i"), r.take("i")))
    f["upvals"] = [r.string() for _ in range(r.take("i"))]
    f["consts"] = []
    for _ in range(r.take("i")):
        t = r.take("B")
        if t == 3:
            f["consts"].append(r.take("d"))
        elif t == 4:
            f["consts"].append(r.string())
        elif t == 0:
            f["consts"].append(None)
        else:
            raise ValueError(f"constant type {t} at {r.p - 1:#x}")
    f["protos"] = [load_function(r) for _ in range(r.take("i"))]
    f["code"] = [r.take("I") for _ in range(r.take("i"))]
    return f


def rk(f, x):
    return repr(f["consts"][x - MAXSTACK]) if x >= MAXSTACK else f"R{x}"


def dump(f, name, flt, depth=0):
    show = flt is None or flt in name
    if show:
        print(f"\n=== {name}  (line {f['line']}, params {f['nparams']}, locals {[l[0] for l in f['locals']]})")
    for pc, ins in enumerate(f["code"]):
        op, c, b, a = ins & 0x3F, (ins >> 6) & 0x1FF, (ins >> 15) & 0x1FF, ins >> 24
        bx = ins >> 6 & 0x3FFFF
        sbx = bx - 131071
        o = OPS[op] if op < len(OPS) else f"OP{op}"
        k = f["consts"]
        if o in ("LOADK", "GETGLOBAL", "SETGLOBAL"):
            arg = f"R{a} {k[bx]!r}"
        elif o in ("GETTABLE", "SELF"):
            arg = f"R{a} R{b}[{rk(f, c)}]"
        elif o == "SETTABLE":
            arg = f"R{a}[{rk(f, b)}] = {rk(f, c)}"
        elif o in ("JMP", "FORLOOP", "TFORPREP"):
            arg = f"-> {pc + 1 + sbx}" + (f" (R{a})" if o != "JMP" else "")
        elif o in ("EQ", "LT", "LE"):
            arg = f"{a} {rk(f, b)} {rk(f, c)}"
        elif o == "CLOSURE":
            arg = f"R{a} proto{bx}"
        elif o in ("ADD", "SUB", "MUL", "DIV", "POW"):
            arg = f"R{a} = {rk(f, b)} {o} {rk(f, c)}"
        elif o == "GETUPVAL":
            arg = f"R{a} = upval {f['upvals'][b] if b < len(f['upvals']) else b}"
        else:
            arg = f"A={a} B={b} C={c}"
        if show:
            print(f"  {pc:4} {o:9} {arg}")
    # name sub-functions by the SETTABLE/SETGLOBAL that stores their closure
    names = {}
    for pc, ins in enumerate(f["code"]):
        if ins & 0x3F == OPS.index("CLOSURE"):
            reg, idx = ins >> 24, ins >> 6 & 0x3FFFF
            for nxt in f["code"][pc + 1:pc + 8]:
                o2, c2, b2, a2 = nxt & 0x3F, (nxt >> 6) & 0x1FF, (nxt >> 15) & 0x1FF, nxt >> 24
                if OPS[o2] == "SETTABLE" and c2 == reg and b2 >= MAXSTACK:
                    names[idx] = str(f["consts"][b2 - MAXSTACK]); break
                if OPS[o2] == "SETGLOBAL" and a2 == reg:
                    names[idx] = str(f["consts"][nxt >> 6 & 0x3FFFF]); break
    for i, p in enumerate(f["protos"]):
        dump(p, names.get(i, f"{name}/proto{i}"), flt, depth + 1)


data = open(sys.argv[1], "rb").read()
assert data[:5] == b"\x1bLua\x50", "not Lua 5.0 bytecode"
r = Reader(data)
r.p = 5 + 1 + 3 + 4 + 1 + 8  # endianness, 3 sizes (int, size_t, Instruction), op/A/B/C bits, number size, test number
dump(load_function(r), "main", sys.argv[2] if len(sys.argv) > 2 else None)
