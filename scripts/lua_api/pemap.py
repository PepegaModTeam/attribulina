#!/usr/bin/env python3
"""Symbols and bytes of a 32-bit x86 Windows or Xbox executable (PE .exe or
.xbe) with an IDA map, behind the names scan_carbon_pc.py uses from tools/pc's
pcmap (read, u32, name, SYMS, DATA_SYMS, TEXT_LO/HI, DATA_LO/HI), for builds
pcmap doesn't know: Most Wanted's speed.exe and default.xbe.

    img = pemap.Image('exes/pc/mw/speed.exe', 'exes/pc/mw/speed.map')

The map's "Publics by Value" entries are SEG:OFFSET with SEG the 1-based
index of the image's section (speed.map: 0001 .text, 0002 .rdata, 0003 .data,
whatever its own section table above prints; default.xbe.map: 0001 .text,
0008 .rdata, 0009 .data). Checked on both by reading the strings at their
aLuacall* symbols.
TEXT_LO/HI is the code section (.text); DATA_LO/HI runs from .rdata to the end
of .data.
"""
import bisect
import struct


def sections(img):
    """[(name, virtual address, virtual size, file offset, file size)]."""
    out = []
    if img[:4] == b'XBEH':
        base, n, sh = struct.unpack_from('<I', img, 0x104)[0], *struct.unpack_from('<II', img, 0x11c)
        for i in range(n):
            fl, va, vs, ra, rs, na = struct.unpack_from('<6I', img, sh - base + 56 * i)
            name = img[na - base:img.index(b'\0', na - base)].decode('latin1')
            out.append((name, va, vs, ra, rs))
        return out
    pe = struct.unpack_from('<I', img, 0x3c)[0]
    if img[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE or XBE image')
    n, opt = struct.unpack_from('<H', img, pe + 6)[0], struct.unpack_from('<H', img, pe + 20)[0]
    base = struct.unpack_from('<I', img, pe + 24 + 28)[0]
    for i in range(n):
        o = pe + 24 + opt + 40 * i
        name = img[o:o + 8].rstrip(b'\0').decode('latin1')
        vs, va, rs, ra = struct.unpack_from('<IIII', img, o + 8)
        out.append((name, base + va, vs, ra, rs))
    return out


class Image:
    def __init__(self, exe, map_path):
        self.IMG = open(exe, 'rb').read()
        self.SECTIONS = sections(self.IMG)
        by = {s[0]: s for s in self.SECTIONS}
        t, r, d = by['.text'], by['.rdata'], by['.data']
        self.TEXT_LO, self.TEXT_HI = t[1], t[1] + t[2]
        self.DATA_LO, self.DATA_HI = r[1], d[1] + d[2]
        self.SYMS, self.DATA_SYMS = {}, {}
        started = False
        for line in open(map_path, encoding='latin1'):
            if 'Publics by Value' in line:
                started = True
                continue
            s = line.strip()
            if not started or not s or ':' not in s.split()[0]:
                continue
            head, rest = s.split(None, 1)
            seg, off = head.split(':')
            k = int(seg, 16) - 1
            if not 0 <= k < len(self.SECTIONS):
                continue
            va = self.SECTIONS[k][1] + int(off, 16)
            (self.SYMS if self.TEXT_LO <= va < self.TEXT_HI else self.DATA_SYMS)[va] = rest.strip()
        self.ADDRS = sorted(self.SYMS)

    def name(self, va):
        """symbol+0xN for a code address."""
        i = bisect.bisect_right(self.ADDRS, va) - 1
        if i < 0:
            return '0x%x' % va
        a = self.ADDRS[i]
        return self.SYMS[a] + ('' if a == va else '+0x%x' % (va - a))

    def read(self, va, n):
        for name, lo, vs, ra, rs in self.SECTIONS:
            if lo <= va < lo + max(vs, rs):
                k = va - lo
                if k >= rs:
                    return b''
                return self.IMG[ra + k:ra + min(k + n, rs)]
        return b''

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<I', b)[0] if len(b) == 4 else None
