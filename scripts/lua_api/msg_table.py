#!/usr/bin/env python3
"""Lists the Hermes message and EventSys event definitions in nfs.exe's
.rdata. A definition is {u32 StringHash32(name), char* name, u32 flags,
fn, u32 nfields, FieldDef* fields}; a FieldDef is {u32 type id,
u32 StringHash32("f" + field name), u32 size<<16 | offset}.
Message field names come from data/messages/prostreet_pc.txt.
    python3 -I -B msg_table.py <repo root> > messages_pc.tsv
Output: name, definition address, fn, field count, fields (Name:type), flags.
lookup2() (StringHash32) is imported by the other scripts.
"""
import os, sys, struct

TYPES = {0x9a98b235: 'float', 0x570a3a9d: 'int', 0xcb6e83a7: 'uint', 0xadfefc2c: 'string',
         0x91d0a144: 'hash(string)', 0x63e5073a: 'hash64(string)', 0x25ec65c7: 'uint8', 0x837e34ee: 'int8',
         0x65346453: 'int16', 0x9647a9ca: 'uint16', 0x0c6c765b: 'int64', 0xbcbc0bf0: 'uint64', 0x49fe54c6: 'double'}


def lookup2(s, init=0xABCDEF00):
    """StringHash32: Bob Jenkins' lookup2 with initial value 0xABCDEF00."""
    k = s.encode(); a = b = 0x9E3779B9; c = init; n = len(k); i = 0
    M = 0xffffffff

    def mix(a, b, c):
        a = (a - b - c) & M; a ^= c >> 13; b = (b - c - a) & M; b ^= (a << 8) & M; c = (c - a - b) & M; c ^= b >> 13
        a = (a - b - c) & M; a ^= c >> 12; b = (b - c - a) & M; b ^= (a << 16) & M; c = (c - a - b) & M; c ^= b >> 5
        a = (a - b - c) & M; a ^= c >> 3; b = (b - c - a) & M; b ^= (a << 10) & M; c = (c - a - b) & M; c ^= b >> 15
        return a, b, c
    while n - i >= 12:
        a = (a + struct.unpack_from('<I', k, i)[0]) & M
        b = (b + struct.unpack_from('<I', k, i + 4)[0]) & M
        c = (c + struct.unpack_from('<I', k, i + 8)[0]) & M
        a, b, c = mix(a, b, c); i += 12
    c = (c + len(k)) & M
    for j, ch in enumerate(k[i:]):
        if j < 4: a = (a + (ch << (8 * j))) & M
        elif j < 8: b = (b + (ch << (8 * (j - 4)))) & M
        else: c = (c + (ch << (8 * (j - 8 + 1)))) & M
    return mix(a, b, c)[2]


def main():
    root = sys.argv[1]
    sys.path.insert(0, os.path.join(root, 'tools', 'pc'))
    import pcmap
    names = {}
    for line in open(os.path.join(root, 'data', 'messages', 'prostreet_pc.txt')):
        if line.startswith('#') or not line.strip(): continue
        parts = line.split()
        for p in parts[1:]:
            f = p.split(':', 1)[0]
            names[lookup2('f' + f)] = f

    def cs(va):
        b = pcmap.read(va, 80); e = b.find(b'\0')
        s = b[:e] if e > 0 else b''
        return s.decode() if s and all(32 <= c < 127 for c in s) else None

    out = []
    va, hi = pcmap.DATA_LO, 0xA4A000
    while va < hi - 24:
        h, nm, flags, fn, nf, fp = struct.unpack('<6I', pcmap.read(va, 24))
        if flags < 0x100 and pcmap.DATA_LO <= nm < pcmap.DATA_HI and pcmap.TEXT_LO <= fn < pcmap.TEXT_HI:
            s = cs(nm)
            if s and s[0] in 'ME' and lookup2(s) == h:
                fl = []
                for j in range(nf if nf < 64 else 0):
                    th, fh, so = struct.unpack('<3I', pcmap.read(fp + 12 * j, 12))
                    fl.append('%s:%s' % (names.get(fh, '%08x' % fh), TYPES.get(th, '%08x' % th)))
                out.append((s, va, fn, nf, ' '.join(fl), flags))
                va += 24; continue
        va += 4
    for r in out:
        print('%s\t%x\t%x\t%d\t%s\t%d' % r)
    print(len(out), 'records', file=sys.stderr)


if __name__ == '__main__':
    main()
