#!/usr/bin/env python3
"""Lists every EventSys event definition the PC game registers, from the
sorted hash array at 0x9884A8 and the definition pointers at 0xA5D540 (226
entries, the search in 0x6A7FF0). Definition layout: {u32 hash, char* name,
u32 flags, fire fn, u32 nfields, FieldDef* fields, u32 size}; FieldDef is
{u32 type id, u32 StringHash32("f"+name), u32 size<<16 | offset}.
Field names are taken from nfs-data/ProStreet08Milestone.h (the f* members
of the event class), matched by hash. Flag bit 1 marks events that the Lua
E<Name>(...) globals accept.
    python3 -I -B event_defs.py <repo root> > events_pc.tsv"""
import os, sys, struct, re
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, 'tools', 'pc'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcmap
from msg_table import lookup2

TYPES = {0x9a98b235: 'float', 0x570a3a9d: 'int', 0xcb6e83a7: 'uint', 0xadfefc2c: 'string',
         0x91d0a144: 'hash(string)', 0x63e5073a: 'hash64(string)', 0x25ec65c7: 'uint8', 0x837e34ee: 'int8',
         0x65346453: 'int16', 0x9647a9ca: 'uint16', 0x0c6c765b: 'int64', 0xbcbc0bf0: 'uint64', 0x49fe54c6: 'double'}

# f* members per E class from the header
fields = {}
cur = None
CLS = re.compile(rb'^class (E[A-Za-z0-9_]*) : public EventSys::Event')
FLD = re.compile(rb'^\s*private (.+?) f([A-Za-z0-9_]+);')
with open(os.path.join(root, 'nfs-data', 'ProStreet08Milestone.h'), 'rb') as f:
    for line in f:
        m = CLS.match(line)
        if m:
            cur = m.group(1).decode(); fields.setdefault(cur, {}); continue
        if cur and line.startswith(b'}'):
            cur = None; continue
        if cur:
            m = FLD.match(line)
            if m:
                nm = m.group(2).decode(); fields[cur][lookup2('f' + nm)] = (nm, m.group(1).decode().strip())

def cs(va):
    b = pcmap.read(va, 80); e = b.find(b'\0')
    s = b[:e] if e > 0 else b''
    return s.decode() if s and all(32 <= c < 127 for c in s) else None

N = 226
hs = struct.unpack('<%dI' % N, pcmap.read(0x9884a8, N * 4))
ptrs = struct.unpack('<%dI' % N, pcmap.read(0xa5d540, N * 4))
rows = []
for h, p in zip(hs, ptrs):
    rh, nm, flags, fn, nf, fp, size = struct.unpack('<7I', pcmap.read(p, 28))
    name = cs(nm) or '?'
    fl = []
    hdr = fields.get(name, {})
    for j in range(nf):
        th, fh, so = struct.unpack("<3I", pcmap.read(fp + 12 * j, 12))
        fname, htype = hdr.get(fh, ('%08x' % fh, '?'))
        fl.append('%s:%s' % (fname, TYPES.get(th, '%08x' % th)))
    rows.append((name, flags, nf, ' '.join(fl), h == rh))
for r in sorted(rows):
    print('%s\t%d\t%d\t%s\t%s' % r)
