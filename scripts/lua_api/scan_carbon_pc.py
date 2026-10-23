#!/usr/bin/env python3
"""Lists what NFS Carbon's PC executable (NFSC.exe, read through tools/pc with
NFS_PC_BUILD=carbon) gives gameplay Lua, in scan_wii.py's columns.

    python3 -I -B scan_carbon_pc.py <repo root> <bindings.tsv> <events.tsv> [<name file> ...]

bindings.tsv: every binder call in LuaBindery::BindToGameCode and
  BindCustomMethods (named in NFSC.map). A function binder takes the table
  name in edi and pushes the C function and the name; a method binder takes
  the metatable name in eax and pushes the this-adjustment, the member
  function and the name; BindSingleton takes the global's name in eax and
  pushes the metatable name and the object. Return and parameter types come
  from the binder's map name (CodeWarrior codes, BindFunction_b_P8ISimable___F...)
  when the map names it, else are left empty. Stub: the bound function's first
  instructions.
events.tsv: the EventSys registry, found from the code that searches
  gEventKeyOrderTable (an implicit binary tree of StringHash32 keys) and then
  reads the definition pointer table. Field names as in scan_wii.py.
scan_mw.py reuses the binding scan for Most Wanted's PC and Xbox builds,
setting `pcmap` to a pemap.Image.
"""
import os, re, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools', 'pc'))
sys.path.insert(0, HERE)
import x86  # noqa: E402
from msg_table import lookup2  # noqa: E402
from scan_wii import cw_types, names_pool, EV_TYPES  # noqa: E402

pcmap = None    # the image read: tools/pc's pcmap with NFS_PC_BUILD=carbon (main), or a pemap.Image


def cstr(va):
    if va is None or not (pcmap.DATA_LO <= va < pcmap.DATA_HI): return None
    b = pcmap.read(va, 128); e = b.find(b'\0')
    if e <= 0: return None
    s = b[:e]
    return s.decode() if all(0x20 <= c < 0x7f for c in s) else None


def is_code(v):
    return v is not None and pcmap.TEXT_LO <= v < pcmap.TEXT_HI


def binder_sig(name):
    """(return, params) from a map name such as BindFunction_b_P16GRuntimeInstance___FP9lua_State...,
    or None when the binder has no name of its own."""
    if '+0x' in name: return None
    m = re.match(r'^Bind(Void)?(Function|Method|Singleton)_(.*?)___F', name)
    if not m:
        if name.startswith('BindRawFunction('): return 'raw', ['raw']
        if name.startswith('BindVoidFunction('): return 'void', []
        return None
    t = cw_types(m.group(3))
    if m.group(2) == 'Singleton': return '', []
    if m.group(2) == 'Method': return 'void', t[1:]
    if m.group(1): return 'void', t
    return t[0], t[1:]


def stub_kind(va, ret):
    t = [i.text for i in x86.disasm(pcmap.read(va, 16), va, count=3)]
    if not t: return ''
    if ret == 'raw':
        return 'does nothing' if t[0] == 'xor eax, eax' and t[1].startswith('ret') else ''
    if t[0].startswith('ret'): return 'does nothing'
    if ret == 'void' and re.match(r'^mov (al|eax), \[0x[0-9a-f]+\]$', t[0]) and t[1].startswith('ret'):
        return 'does nothing'          # reads a flag into the unused return register
    if t[0] in ('xor eax, eax', 'xor al, al') and t[1].startswith('ret'): return 'always returns 0'
    if t[0] in ('mov al, 0x1', 'mov eax, 0x1') and t[1].startswith('ret'): return 'always returns 1'
    if t[0] == 'fldz' and t[1].startswith('ret'): return 'always returns 0'
    return ''


IMM = re.compile(r'^mov (e[a-d]x|e[sd]i|ebx|ebp), (0x[0-9a-f]+)$')
XOR = re.compile(r'^xor (e\w\w), \1$')
PUSHI = re.compile(r'^push (-?0x[0-9a-f]+|\d+)$')
PUSHR = re.compile(r'^push (e\w\w)$')


def scan(lo, hi):
    """(site, target, pushed args first to last, eax, edi) for every call in lo..hi."""
    regs, pushes, out = {}, [], []
    for ins in x86.disasm(pcmap.read(lo, hi - lo), lo):
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
            out.append((ins.va, ins.target or 0, list(reversed(pushes)), regs.get('eax'), regs.get('edi')))
            pushes = []
            for r in ('eax', 'ecx', 'edx'): regs.pop(r, None)
            continue
        if t.startswith(('ret', 'jmp')):
            pushes, regs = [], {}
            continue
        if t.startswith('add esp'): continue
        m = re.match(r'^\w+ (e\w\w),', t)
        if m and not t.startswith(('cmp', 'test', 'push')): regs.pop(m.group(1), None)
    return out


def scan_bindings(makers):
    """The bindings.tsv rows of the binder calls in the functions starting at makers."""
    rows, seen = [], set()
    for a in sorted(makers):
        end = a
        for ins in x86.disasm(pcmap.read(a, 0x2000), a):     # to the function's ret
            end = ins.va + ins.length
            if ins.text.startswith('ret'): break
        for site, tgt, args, eax, edi in scan(a, end):
            strs = [cstr(v) for v in args if cstr(v)]
            fns = [v for v in args if is_code(v)]
            tname = pcmap.name(tgt)
            if cstr(eax) and len(strs) == 1 and fns:
                kind, tbl, name, fn = 'method', cstr(eax), strs[0], fns[0]
            elif cstr(eax) and len(strs) == 1:
                kind, tbl, name, fn = 'object', strs[0], cstr(eax), None
            elif cstr(edi) and len(strs) == 1 and fns:
                kind, tbl, name, fn = 'function', cstr(edi), strs[0], fns[0]
            else:
                if tname.startswith('Bind'): print('unread binding at %x' % site, file=sys.stderr)
                continue
            if (kind, tbl, name) in seen: continue
            seen.add((kind, tbl, name))
            sig = binder_sig(tname)
            ret, params = sig if sig else ('', [])
            stub = stub_kind(fn, ret) if fn else ''
            rows.append((kind, tbl, name, '%x' % fn if fn else '-', pcmap.name(fn) if fn else '-', tname,
                         ret, ', '.join(params), stub))
    return rows


def main():
    global pcmap
    root, out_b, out_e = sys.argv[1:4]
    pool = names_pool(sys.argv[4:])
    os.environ['NFS_PC_BUILD'] = 'carbon'
    sys.path.insert(0, os.path.join(root, 'tools', 'pc'))
    import pcmap as image
    pcmap = image
    makers = [a for a, n in pcmap.SYMS.items() if n.startswith(('LuaBindery::BindToGameCode', 'LuaBindery::BindCustomMethods'))]
    rows = scan_bindings(makers)
    with open(out_b, 'w') as f:
        for r in rows: f.write('\t'.join(r) + '\n')

    # events
    keys = next(a for a, n in pcmap.DATA_SYMS.items() if n == 'gEventKeyOrderTable')
    text = pcmap.read(pcmap.TEXT_LO, pcmap.TEXT_HI - pcmap.TEXT_LO)
    m = re.search(rb'\x8b\x0c\x85' + struct.pack('<I', keys) + rb'.{0,40}?\x3d(....).{0,40}?\x8b\x04\x85(....)', text, re.S)
    count, ptrs = struct.unpack('<I', m.group(1))[0], struct.unpack('<I', m.group(2))[0]
    evs = []
    for i in range(count):
        h = pcmap.u32(keys + 4 * i); p = pcmap.u32(ptrs + 4 * i)
        rh, nm, flags, fire, nf, fp, size = struct.unpack('<7I', pcmap.read(p, 28))
        name = cstr(nm)
        if rh != h or lookup2(name) != h: print('event %s: hash mismatch' % name, file=sys.stderr)
        fl = []
        for j in range(nf):
            th, fh, so = struct.unpack('<3I', pcmap.read(fp + 12 * j, 12))
            fl.append('%s:%s' % (pool.get(fh, '%08x' % fh), EV_TYPES.get(th, '%08x' % th)))
        evs.append((name, flags, nf, ' '.join(fl), size, 'fire' if fire else '-'))
    with open(out_e, 'w') as f:
        for r in sorted(evs): f.write('%s\t%d\t%d\t%s\t%d\t%s\n' % r)
    print('NFSC.exe: %d bindings, %d events (%d with flag bit 1); registry %x/%x' % (
        len(rows), len(evs), sum(1 for r in evs if r[1] & 2), keys, ptrs), file=sys.stderr)


if __name__ == '__main__':
    main()
