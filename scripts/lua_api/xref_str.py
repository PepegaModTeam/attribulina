#!/usr/bin/env python3
"""For each name, finds the NUL-terminated string in nfs.exe and every place
its address appears as a 32-bit value (code immediates or data pointers).
    python3 -I -B xref_str.py <repo root> <name> [...]"""
import os, sys, re, struct
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, 'tools', 'pc'))
import pcmap
img = pcmap.IMG
def va_of(off):
    for seg, (lo, raw) in pcmap.SEGMENTS.items():
        if raw <= off < raw + 0x600000: return lo + off - raw
for w in sys.argv[2:]:
    out = []
    for m in re.finditer(b'(?<=\\x00)' + re.escape(w.encode()) + b'\\x00', img):
        va = va_of(m.start())
        if va is None: continue
        refs = [va_of(m2.start()) for m2 in re.finditer(re.escape(struct.pack('<I', va)), img)]
        refs = [r for r in refs if r is not None]
        out.append('%x <- %s' % (va, ' '.join('%x(%s)' % (r, pcmap.name(r) if pcmap.TEXT_LO <= r < pcmap.TEXT_HI else (pcmap.data_name(r) or 'data')) for r in refs[:6])))
    print(w, ':', '; '.join(out) or '-')
