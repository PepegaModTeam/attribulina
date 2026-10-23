#!/usr/bin/env python3
"""Finds every call in a range of nfs.exe whose arguments (pushes since the
previous call, plus eax/ecx/edx set by `mov reg, imm32`) include a string and
a code address. That is the shape of every Lua binding: a name, a table or
metatable name, and the C function (or member function) bound.

    python3 -I -B scan_calls.py <repo root> <lo hex> <hi hex> > calls.tsv
Output columns: site, call target, target name, args (pushed last first),
eax, ecx, edx. Strings print as "text", code addresses as @addr.
"""
import os, sys, re
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, 'tools', 'pc'))
import pcmap, x86

lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)

def cstr(va):
    if not (pcmap.DATA_LO <= va < pcmap.DATA_HI): return None
    b = pcmap.read(va, 128)
    e = b.find(b'\0')
    if e <= 0: return None
    s = b[:e]
    if all(0x20 <= c < 0x7f for c in s): return s.decode()
    return None

def show(v):
    if v is None: return '?'
    s = cstr(v)
    if s is not None: return '"%s"' % s
    if pcmap.TEXT_LO <= v < pcmap.TEXT_HI: return '@%x' % v
    return '%x' % v

IMM = re.compile(r'^mov (e[a-d]x|e[sd]i|ebx|ebp), (0x[0-9a-f]+)$')
XOR = re.compile(r'^xor (e\w\w), \1$')
PUSHI = re.compile(r'^push (-?0x[0-9a-f]+|\d+)$')
PUSHR = re.compile(r'^push (e\w\w)$')
data = pcmap.read(lo, hi - lo)
regs, pushes = {}, []
for ins in x86.disasm(data, lo):
    t = ins.text
    m = IMM.match(t)
    if m: regs[m.group(1)] = int(m.group(2), 16); continue
    m = XOR.match(t)
    if m: regs[m.group(1)] = 0; continue
    m = PUSHI.match(t)
    if m: pushes.append(int(m.group(1), 0) & 0xffffffff); continue
    m = PUSHR.match(t)
    if m: pushes.append(regs.get(m.group(1))); continue
    if t.startswith('call'):
        args = list(reversed(pushes))
        vals = args + [regs.get('eax'), regs.get('ecx'), regs.get('edx')]
        has_s = any(v is not None and cstr(v) for v in vals)
        has_c = any(v is not None and pcmap.TEXT_LO <= v < pcmap.TEXT_HI for v in vals)
        ecx_s = regs.get('ecx') is not None and cstr(regs.get('ecx'))
        if has_s and (has_c or ecx_s or len([v for v in vals if v is not None and cstr(v)]) >= 2):
            tgt = ins.target or 0
            print('%08x\t%08x\t%s\t%s\t%s\t%s\t%s' % (ins.va, tgt, pcmap.name(tgt) if tgt else t,
                  ' '.join(show(v) for v in args), show(regs.get('eax')), show(regs.get('ecx')), show(regs.get('edx'))))
        pushes = []
        for r in ('eax', 'ecx', 'edx'): regs.pop(r, None)
        continue
    if t.startswith(('ret', 'jmp')):
        pushes = []; regs = {}
        continue
    # any other write to a tracked register forgets it
    m = re.match(r'^\w+ (e\w\w),', t)
    if m and not t.startswith(('cmp', 'test', 'push')): regs.pop(m.group(1), None)
