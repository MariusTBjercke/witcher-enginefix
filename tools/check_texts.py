"""Checks the translated texts in mod\\System\\Scripts\\enginefix_hook.lua (the TEXTS table) against the game.

Usage: python -I tools\\check_texts.py <game folder>

The GUI draws Lua strings as UTF-8 with the font of the current text language, and the fonts of some
languages (Chinese, Russian) only hold the characters the game itself uses. So every character of a text
must occur in that language's talk table (Data\\dialog_<id>.tlk, TLK V3.0, UTF-8 strings). Also checks that
each language has every key the English entry has. Exit code 1 on any problem.
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOOK = os.path.join(ROOT, "mod", "System", "Scripts", "enginefix_hook.lua")


def tlk_chars(path):
    data = open(path, "rb").read()
    count, offset = struct.unpack_from("<II", data, 12)
    chars = set()
    for i in range(count):
        flags, = struct.unpack_from("<I", data, 20 + i * 40)
        start, size = struct.unpack_from("<II", data, 20 + i * 40 + 28)
        if flags & 1:
            chars.update(data[offset + start:offset + start + size].decode("utf-8", "replace"))
    return chars


def read_texts():
    src = open(HOOK, encoding="utf-8").read()
    block = re.search(r"^local TEXTS = \{\n(.*?)^\}", src, re.S | re.M)
    if not block:
        raise SystemExit("TEXTS table not found in " + HOOK)
    texts, lang = {}, None
    for line in block.group(1).splitlines():
        m = re.match(r"\s*\[(\d+)\]\s*=\s*\{", line)
        if m:
            lang = int(m.group(1))
            texts[lang] = {}
            continue
        m = re.match(r'\s*(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"', line)
        if m and lang is not None:
            texts[lang][m.group(1)] = m.group(2)
    return texts


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    sys.stdout.reconfigure(encoding="utf-8")
    texts = read_texts()
    keys = set(texts.get(3, {}))
    bad = 0
    for lang, entries in sorted(texts.items()):
        path = os.path.join(argv[1], "Data", "dialog_%d.tlk" % lang)
        if not os.path.exists(path):
            print("%2d: %s not found" % (lang, path))
            bad += 1
            continue
        chars = tlk_chars(path)
        for key in sorted(keys - set(entries)):
            print("%2d: missing %s" % (lang, key))
            bad += 1
        for key, text in sorted(entries.items()):
            missing = sorted(set(ch for ch in text if ch not in chars))
            if missing:
                print("%2d %s: not in the game's texts: %s  (%s)" % (lang, key, " ".join(missing), text))
                bad += 1
        print("%2d: %d texts checked" % (lang, len(entries)))
    print("OK" if not bad else "%d problem(s)" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
