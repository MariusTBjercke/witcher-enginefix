"""Builds the .ncs files in mod\\Data\\Override\\EngineFix without the game's compiler.

Usage: python -I tools\\build_ef_ncs.py [--check]

A tiny NCS V1.0 assembler: big-endian operands, jumps relative to the jumping instruction, engine
routines numbered by their position in System\\Scripts\\nwscriptdefn.nss (see tools\\ncsdis.py).
The programs below are ef_q3043_door.nss and ef_lootall.nss written out by hand; tools\\ncsdis.py
disassembles the result. --check only verifies that the committed .ncs files match what this script
would write.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ncsdis  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "mod", "Data", "Override", "EngineFix")

# Routine numbers in nwscriptdefn.nss (1.5). Checked against the file when it is available.
ROUTINES = {
    "ExecuteScript": 0x08, "GetIsObjectValid": 0x2A, "GetObjectByTag": 0xC8, "GetLocked": 0x145,
    "GetQuestCurrentPhaseIndex": 0x1C6, "GetQuestPhaseIndex": 0x1CA, "RunClientLua": 0x2D2,
}
II = 0x20  # int, int operand type of binary operators


class Asm:
    def __init__(self):
        self.code = []      # (label or None, bytes or callable(pos, labels) -> bytes, size)
        self.labels = {}

    def emit(self, b):
        self.code.append((None, b, len(b)))

    def label(self, name):
        self.code.append((name, b"", 0))

    def jump(self, op, target):
        self.code.append((None, lambda pos, labels, op=op, t=target: bytes([op, 0]) + struct.pack(">i", labels[t] - pos), 6))

    def const_i(self, v):
        self.emit(bytes([0x04, 3]) + struct.pack(">i", v))

    def const_s(self, s):
        b = s.encode("latin-1")
        self.emit(bytes([0x04, 5]) + struct.pack(">H", len(b)) + b)

    def const_self(self):
        self.emit(bytes([0x04, 6]) + struct.pack(">i", 0))

    def action(self, name, argc):
        self.emit(bytes([0x05, 0]) + struct.pack(">HB", ROUTINES[name], argc))

    def cptopsp(self, off):
        self.emit(bytes([0x03, 1]) + struct.pack(">ih", off, 4))

    def movsp(self, off):
        self.emit(bytes([0x1B, 0]) + struct.pack(">i", off))

    def binop(self, op):
        self.emit(bytes([op, II]))

    def retn(self):
        self.emit(bytes([0x20, 0]))

    def assemble(self):
        pos = 13
        for name, _b, size in self.code:
            if name:
                self.labels[name] = pos
            pos += size
        out, pos = bytearray(), 13
        for name, b, size in self.code:
            chunk = b(pos, self.labels) if callable(b) else b
            assert len(chunk) == size
            out += chunk
            pos += size
        return b"NCS V1.0" + b"B" + struct.pack(">I", 13 + len(out)) + bytes(out)


def door_program():
    q = "q3043_trisshan"
    a = Asm()
    a.jump(0x1E, "main")                       # JSR main
    a.retn()
    a.label("main")
    a.const_s(q)
    a.action("GetQuestCurrentPhaseIndex", 1)   # [cur]
    a.const_s("Dandellion shows the way"); a.const_s(""); a.const_s(q)
    a.action("GetQuestPhaseIndex", 3)          # [cur, way]
    a.cptopsp(-8); a.cptopsp(-8)               # [cur, way, cur, way]
    a.binop(0x0B)                              # EQUAL -> [cur, way, eq1]
    a.const_s("Walka z salamandrami"); a.const_s(""); a.const_s(q)
    a.action("GetQuestPhaseIndex", 3)          # [cur, way, eq1, fight]
    a.cptopsp(-16); a.cptopsp(-8)              # [.., fight, cur, fight]
    a.binop(0x0B)                              # [cur, way, eq1, fight, eq2]
    a.cptopsp(-12)                             # [.., eq2, eq1]
    a.binop(0x07)                              # LOGOR -> [cur, way, eq1, fight, phase_ok]
    a.jump(0x1F, "out_phase")                  # JZ (pops) -> [cur, way, eq1, fight]
    a.const_i(0); a.const_s("q3043_sal_door1")
    a.action("GetObjectByTag", 2)              # [.., d1]
    a.const_i(0); a.const_s("q3043_sal_door2")
    a.action("GetObjectByTag", 2)              # [.., d1, d2]
    a.cptopsp(-8); a.action("GetLocked", 1)    # [.., d1, d2, l1]
    a.cptopsp(-8); a.action("GetLocked", 1)    # [.., d1, d2, l1, l2]
    a.binop(0x07)                              # LOGOR -> [.., d1, d2, locked]
    a.cptopsp(-12); a.action("GetIsObjectValid", 1)  # [.., d1, d2, locked, valid]
    a.binop(0x06)                              # LOGAND -> [.., d1, d2, go]
    a.jump(0x1F, "out_doors")                  # JZ (pops) -> [cur, way, eq1, fight, d1, d2]
    a.const_self(); a.const_s("q3043_unlock")
    a.action("ExecuteScript", 2)
    a.const_s("EngineFix_KidnapDoorUnlocked()")
    a.action("RunClientLua", 1)
    a.label("out_doors")
    a.movsp(-8)                                # drop d1, d2
    a.label("out_phase")
    a.movsp(-16)                               # drop cur, way, eq1, fight
    a.retn()
    return a.assemble()


def lootall_program():
    # The same layout the game's compiler emits for a one-line RunClientLua script.
    a = Asm()
    a.jump(0x1E, "main")                       # JSR main
    a.retn()
    a.label("main")
    a.const_s("EngineFix_QuickLoot()")
    a.action("RunClientLua", 1)
    a.retn()
    return a.assemble()


PROGRAMS = {"ef_q3043_door.ncs": door_program, "ef_lootall.ncs": lootall_program}


def check_routines():
    names = ncsdis.load_names(ncsdis.DEFAULT_DEFN)
    if not names:
        return
    for name, rid in ROUTINES.items():
        if names[rid] != name:
            raise SystemExit(f"routine {name} is #{names.index(name):#x} in nwscriptdefn.nss, not {rid:#x}")


def main(argv):
    check_routines()
    failed = 0
    for name, program in PROGRAMS.items():
        out = os.path.join(OUT_DIR, name)
        data = program()
        if "--check" in argv:
            same = os.path.exists(out) and open(out, "rb").read() == data
            print(f"{name}: " + ("up to date" if same else "OUT OF DATE"))
            failed |= not same
            continue
        os.makedirs(OUT_DIR, exist_ok=True)
        open(out, "wb").write(data)
        print(f"wrote {out} ({len(data)} bytes)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
