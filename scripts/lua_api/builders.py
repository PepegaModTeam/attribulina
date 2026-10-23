#!/usr/bin/env python3
"""Maps each Hermes message delivered to Lua to the C function that turns it
into the handler's `message` table. The game registers one stub per message:
`push <builder or 0>; ...; mov dword ptr [esp+N], <StringHash32(name)>; ...;
call LuaMessageDeliveryInfo::LuaMessageDeliveryInfo`. For each builder it
lists the field keys pushed (lua_pushstring of a string constant) and the
calls made in between, which show how each field is converted.
    python3 -I -B builders.py <repo root> <messages_pc.tsv> > builders.tsv"""
import os, sys, re
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, 'tools', 'pc'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcmap, x86
from msg_table import lookup2
names = {}
for l in open(sys.argv[2]):
    p = l.split('\t')
    names[lookup2(p[0])] = p[0]
for line in open(os.path.join(root, 'data', 'messages', 'prostreet_pc.txt')):
    if line.startswith('#') or not line.strip(): continue
    names.setdefault(lookup2(line.split()[0]), line.split()[0])
for extra in ('MStateEnter', 'MStateExit'):
    names[lookup2(extra)] = extra
CTOR = 0x66bc30
lo, hi = 0x6c0000, 0x6c8000
ins = x86.disasm(pcmap.read(lo, hi - lo), lo)
def cs(va):
    b = pcmap.read(va, 64); e = b.find(b'\0')
    s = b[:e] if e > 0 else b''
    return s.decode() if s and all(32 <= c < 127 for c in s) else None
found = {}
for i, it in enumerate(ins):
    if it.text.startswith('call') and it.target == CTOR:
        win = ins[max(0, i - 14):i]
        builder, msg = None, None
        for w in win:
            m = re.match(r'push (0x[0-9a-f]+)$', w.text)
            if m and pcmap.TEXT_LO <= int(m.group(1), 16) < pcmap.TEXT_HI: builder = int(m.group(1), 16)
            m = re.match(r'mov dword ptr \[esp\+0x[0-9a-f]+\], (0x[0-9a-f]+)$', w.text)
            if m and int(m.group(1), 16) in names: msg = names[int(m.group(1), 16)]
        if msg: found[msg] = builder
PUSHSTRING, SETTABLE, PUSHNUM, PUSHBOOL, PUSHNIL, ATTACH = 0x6ecc60, 0x6e5a00, 0x6dff40, 0x6dff60, 0x6dff20, 0x6ef040
for msg, b in sorted(found.items()):
    fields = []
    if b:
        key, kinds, last = None, [], None
        for it in x86.disasm(pcmap.read(b, 0x400), b):
            m = re.match(r'push (0x[0-9a-f]+)$', it.text)
            if m:
                last = cs(int(m.group(1), 16))
            if it.text.startswith('call') and it.target:
                t = it.target
                if t == PUSHSTRING and key is None and last:
                    key, kinds = last, []
                elif t == PUSHSTRING and key is not None:
                    kinds.append('string')
                elif t == PUSHNUM: kinds.append('number')
                elif t == PUSHBOOL: kinds.append('boolean')
                elif t == PUSHNIL: kinds.append('nil')
                elif t == ATTACH: kinds.append(last or 'userdata')
                elif t == SETTABLE and key is not None:
                    ks = [k for k in kinds if k != 'nil'] or ['nil']
                    typ = ks[0] + (' or nil' if 'nil' in kinds and ks[0] != 'nil' else '')
                    fields.append('%s:%s' % (key, typ.replace(' ', '_')))
                    key = None
                last = None if t != ATTACH else last
            if it.text.startswith('ret'): break
    print('%s\t%s\t%s' % (msg, ('%x' % b) if b else '-', ' '.join(fields)))
