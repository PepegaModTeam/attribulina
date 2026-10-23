#!/usr/bin/env python3
"""Dumps the sorted {StringHash32(name), lua_CFunction} table in nfs.exe that
backs the M*/E* globals, names each entry from the message/event records
(x86/messages_pc.tsv) and reads how many Lua arguments each function wants
(the `cmp eax, N` after lua_gettop).
    python3 -I -B lookup_table.py <repo root> <messages_pc.tsv> <any entry va hex>"""
import os, sys, struct, re
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, 'tools', 'pc'))
import pcmap, x86
names = {}
for l in open(sys.argv[2]):
    p = l.rstrip('\n').split('\t')
    if len(p) < 2: continue
    names[p[0]] = p
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from msg_table import lookup2  # noqa
hname = {lookup2(n): n for n in names}
va = int(sys.argv[3], 16) - 4
def entry(a):
    h, f = struct.unpack('<II', pcmap.read(a, 8))
    return f, h
# the pairs are {fn, hash}: walk back and forward while fn is code
lo = va
while True:
    f, h = entry(lo - 8)
    if not (pcmap.TEXT_LO <= f < pcmap.TEXT_HI) or h >= entry(lo)[1]: break
    lo -= 8
hi = va
while True:
    f, h = entry(hi)
    if not (pcmap.TEXT_LO <= f < pcmap.TEXT_HI) or (hi > va and h <= entry(hi - 8)[1]): break
    hi += 8
print('# table %x..%x, %d entries' % (lo, hi, (hi - lo) // 8), file=sys.stderr)
for a in range(lo, hi, 8):
    f, h = entry(a)
    nargs = None
    for ins in x86.disasm(pcmap.read(f, 0x60), f, count=24):
        m = re.match(r'cmp eax, (0x[0-9a-f]+)$', ins.text)
        if m: nargs = int(m.group(1), 16); break
    print('%x\t%08x\t%x\t%s\t%s' % (a, h, f, hname.get(h, '?'), nargs))
