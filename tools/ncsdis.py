"""Disassembles a compiled NWScript (.ncs, "NCS V1.0") from The Witcher EE.

Usage: python -I tools\\ncsdis.py <file.ncs> [nwscriptdefn.nss]

Engine calls (ACTION) are named from the game's System\\Scripts\\nwscriptdefn.nss: the routine number
is the index of the function prototype in that file (RunClientLua = 0x2D2 is a known check).
"""
import re
import struct
import sys

DEFAULT_DEFN = r"D:\SteamLibrary\steamapps\common\The Witcher Enhanced Edition\System\Scripts\nwscriptdefn.nss"

OPS = {
    0x01: "CPDOWNSP", 0x02: "RSADD", 0x03: "CPTOPSP", 0x04: "CONST", 0x05: "ACTION", 0x06: "LOGAND",
    0x07: "LOGOR", 0x08: "INCOR", 0x09: "EXCOR", 0x0A: "BOOLAND", 0x0B: "EQUAL", 0x0C: "NEQUAL",
    0x0D: "GEQ", 0x0E: "GT", 0x0F: "LT", 0x10: "LEQ", 0x11: "SHLEFT", 0x12: "SHRIGHT", 0x13: "USHRIGHT",
    0x14: "ADD", 0x15: "SUB", 0x16: "MUL", 0x17: "DIV", 0x18: "MOD", 0x19: "NEG", 0x1A: "COMP",
    0x1B: "MOVSP", 0x1C: "STORE_STATEALL", 0x1D: "JMP", 0x1E: "JSR", 0x1F: "JZ", 0x20: "RETN",
    0x21: "DESTRUCT", 0x22: "NOT", 0x23: "DECISP", 0x24: "INCISP", 0x25: "JNZ", 0x26: "CPDOWNBP",
    0x27: "CPTOPBP", 0x28: "DECIBP", 0x29: "INCIBP", 0x2A: "SAVEBP", 0x2B: "RESTOREBP",
    0x2C: "STORE_STATE", 0x2D: "NOP",
}
TYPES = {3: "I", 4: "F", 5: "S", 6: "O", 0x24: "struct"}


def load_names(path):
    names = []
    try:
        text = open(path, encoding="latin-1").read()
    except OSError:
        return names
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    for m in re.finditer(r"^\s*(?:\w+)\s+(\w+)\s*\([^;{]*\)\s*;", text, re.M):
        names.append(m.group(1))
    return names


def disasm(data, names):
    if data[:8] != b"NCS V1.0":
        raise ValueError("not NCS V1.0")
    pc, end = 13, len(data)
    out = []
    while pc < end:
        op, t = data[pc], data[pc + 1]
        start = pc
        pc += 2
        name = OPS.get(op, f"op{op:02x}")
        arg = ""
        if op in (0x01, 0x03, 0x26, 0x27):
            off, size = struct.unpack_from(">ih", data, pc); pc += 6
            arg = f"{off}, {size}"
        elif op == 0x04:
            if t == 3:
                arg = str(struct.unpack_from(">i", data, pc)[0]); pc += 4
            elif t == 4:
                arg = repr(struct.unpack_from(">f", data, pc)[0]); pc += 4
            elif t == 5:
                n = struct.unpack_from(">H", data, pc)[0]
                arg = repr(data[pc + 2:pc + 2 + n].decode("latin-1")); pc += 2 + n
            elif t == 6:
                v = struct.unpack_from(">i", data, pc)[0]; pc += 4
                arg = "OBJECT_SELF" if v == 0 else "OBJECT_INVALID" if v == 1 else f"obj {v}"
            else:
                arg = f"<const type {t}>"
        elif op == 0x05:
            rid, argc = struct.unpack_from(">HB", data, pc); pc += 3
            fn = names[rid] if rid < len(names) else "?"
            arg = f"{fn} (#{rid:#x}, {argc} args)"
        elif op in (0x0B, 0x0C) and t == 0x24:
            arg = f"size {struct.unpack_from('>H', data, pc)[0]}"; pc += 2
        elif op in (0x1B, 0x23, 0x24, 0x28, 0x29):
            arg = str(struct.unpack_from(">i", data, pc)[0]); pc += 4
        elif op in (0x1D, 0x1E, 0x1F, 0x25):
            rel = struct.unpack_from(">i", data, pc)[0]; pc += 4
            arg = f"{start + rel:#06x}"
        elif op == 0x21:
            a, b, c = struct.unpack_from(">hhh", data, pc); pc += 6
            arg = f"{a}, {b}, {c}"
        elif op == 0x2C:
            a, b = struct.unpack_from(">ii", data, pc); pc += 8
            arg = f"{a}, {b}"
        tname = TYPES.get(t, f"{t:#x}") if op not in (0x05, 0x1B, 0x1D, 0x1E, 0x1F, 0x20, 0x25, 0x2A, 0x2B) else ""
        out.append(f"{start:#06x}  {name:<10} {tname:<6} {arg}")
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    names = load_names(argv[2] if len(argv) > 2 else DEFAULT_DEFN)
    for line in disasm(open(argv[1], "rb").read(), names):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
