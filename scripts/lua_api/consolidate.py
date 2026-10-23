#!/usr/bin/env python3
"""Turns scan_calls output for the Lua binding module into one row per
binding: kind (function/method/object), table or metatable, name, C function,
its map name, binder, binder map name.
    python3 -I -B consolidate.py <repo root> <calls.tsv> > bindings_pc.tsv"""
import os, sys
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, 'tools', 'pc'))
import pcmap
seen = set()
for line in open(sys.argv[2]):
    site, tgt, tname, args, eax, ecx, edx = line.rstrip('\n').split('\t')
    a = args.split(' ') if args else []
    strs = [x[1:-1] for x in a if x.startswith('"')]
    fns = [int(x[1:], 16) for x in a if x.startswith('@')]
    tgt = int(tgt, 16)
    row = None
    if tgt == 0x6f1ba0 and ecx.startswith('"'):
        # BindObject: ecx = global name, pushed metatable name (or the same name
        # when an earlier call consumed the push), pushed object pointer
        row = ('object', strs[0] if strs else ecx[1:-1], ecx[1:-1], 0)
    elif eax.startswith('"') and len(strs) == 1 and fns:
        row = ('method', eax[1:-1], strs[0], fns[0])
    elif len(strs) == 2 and fns and a[0].startswith('"') and a[1].startswith('@'):
        row = ('function', strs[1], strs[0], fns[0])
    if not row: continue
    kind, tbl, name, fn = row
    key = (kind, tbl, name)
    if key in seen: continue       # BindToGameCode repeats Table/LuaDebug from BindCustomMethods
    seen.add(key)
    print('\t'.join([kind, tbl, name, '%x' % fn if fn else '-', pcmap.name(fn) if fn else '-', '%x' % tgt, pcmap.name(tgt), site]))
