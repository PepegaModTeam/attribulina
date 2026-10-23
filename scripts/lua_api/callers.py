#!/usr/bin/env python3
"""Every `call rel32` / `jmp rel32` in nfs.exe's .text that targets an address.
    python3 -I -B callers.py <repo root> <target hex> [...]"""
import os, sys, struct
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, 'tools', 'pc'))
import pcmap
lo, hi = pcmap.TEXT_LO, pcmap.TEXT_HI
text = pcmap.read(lo, hi - lo)
targets = {int(a, 16) for a in sys.argv[2:]}
for i in range(len(text) - 5):
    if text[i] in (0xE8, 0xE9):
        t = (lo + i + 5 + struct.unpack_from('<i', text, i + 1)[0]) & 0xffffffff
        if t in targets:
            print('%x -> %x  %s  %s' % (lo + i, t, 'call' if text[i] == 0xE8 else 'jmp', pcmap.name(lo + i)))
