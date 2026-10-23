#!/usr/bin/env python3
"""Lists what Need for Speed Most Wanted (2005) gives gameplay Lua, in the
columns of scan_wii.py and scan_carbon_pc.py, from three builds:

    python3 -I -B scan_mw.py <repo root> <work folder>

GameCube (nfs-data/mw/gc-release/NFSMWRELEASE.ELF, full symbols, and its
DWARF dump exes/gamecube/mw-vanilla/NFSMWRELEASE.h):
  mw_gc_bindings.tsv: scan_wii.py's binding scan of LuaBindery::BindToGameCode
    (the only function that calls a binder; MW has no BindCustomMethods). The
    binders' template names use the older __H mangling (scan_wii.demangle_h).
  mw_gc_events.tsv: the E<Name> globals. MW's global lookup
    (LuaRuntime::HandleGlobalIndex) answers a name starting with E through
    RegisterEvent::GetLuaBinding: gEventKeyOrderTable (StringHash32 of the
    event names, searched as a packed binary tree) indexes gEventLuaBindings,
    one lua_CFunction per event, null for an event Lua can't fire. Each such
    function checks lua_gettop against its argument count, reads the arguments
    in order (lua_tonumber/tostring/toboolean) and passes them to the event's
    constructor, whose parameters are the event's first fields. Columns as in
    scan_wii.py, written so that write_list.py reads them the same way: name,
    flags (2 when the event has a Lua binding, else 0), argument count, the
    arguments as Field:type (field names from the struct E<Name> members in
    the header, types from the reads: number, string, boolean), the size the
    binding allocates, "fire" when it has a binding.
PC (exes/pc/mw/speed.exe + speed.map, an IDA export that names neither the
binders nor LuaBindery::BindToGameCode):
  mw_pc_bindings.tsv: the binder calls in BindToGameCode, found as the
    function holding the `push <C function>; push <name>; mov edi, <table>;
    call <binder>` sequences, read with scan_carbon_pc.py's scan. Return and
    parameter columns are left empty (write_list.py takes them from the
    GameCube build with --types-from); the stub column is the PC function's,
    judged with the GameCube binding's return type.
  mw_pc_events.tsv: the GameCube rows, with the binding and argument count
    taken from the PC's own gEventLuaBindings (found from the three lookups
    that search the same 161-key table: the Lua one is the table whose
    functions all call the same function first, lua_gettop in speed.map; the
    argument count is the `cmp eax, N` after that call).
Xbox (exes/xbox/mw-vanilla/default.xbe + default.xbe.map): mw_xbox_bindings.tsv
  and mw_xbox_events.tsv the same way as the PC's, for comparison. Its
  compiler inlined some binders into BindToGameCode, which the scan doesn't
  read; mw_builds.txt checks those names among its strings instead.
mw_builds.txt: what the three builds bind differently, and which bound names
  are missing from the PS2 builds' strings (exes/ps2/mw-beta/NFS.ELF,
  nfs-data/mw/ps2-milestone/NFS.ELF), which nothing here disassembles (MIPS).
"""
import collections, os, re, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ppcelf, pemap, scan_wii  # noqa: E402
import scan_carbon_pc as X  # noqa: E402  (also puts tools/pc on the path for x86)
import x86  # noqa: E402
from msg_table import lookup2  # noqa: E402

READ_TYPES = {'lua_tonumber': 'number', 'lua_tostring': 'string', 'lua_toboolean': 'boolean'}


# ---------------------------------------------------------------- the header's event fields
def event_fields(header):
    """{E<Name>: [field names]}: the members of each `struct E<Name> : Event`,
    without the f prefix, from the first definition that has members."""
    out = {}
    cur = None
    depth = 0
    with open(header, 'rb') as f:
        for line in f:
            if cur is None:
                m = re.match(rb'^struct (E\w+) : Event /\*', line)
                if m:
                    cur, depth, fields = m.group(1).decode(), line.count(b'{') - line.count(b'}'), []
                continue
            depth += line.count(b'{') - line.count(b'}')
            if depth == 1:
                m = re.match(rb'^\t(?!inline |virtual |static )[^(]*?[\s*&]f([A-Za-z0-9_]+);', line)
                if m: fields.append(m.group(1).decode())
            if depth <= 0:
                if fields and cur not in out: out[cur] = fields
                cur = None
    return out


# ---------------------------------------------------------------- GameCube
def gc_events(e, fields):
    keys, lua = e.by_name['gEventKeyOrderTable'], e.by_name['gEventLuaBindings']
    make = e.by_name['gMakeEventCallbacks']
    count = None
    get = e.by_name['GetLuaBinding__13RegisterEventUi']      # li r3, <count>; bl SearchPackedBinaryTree
    for ins in e.disasm(get, e.func_end(get)):
        if ins.mnemonic == 'li' and ins.op_str.startswith('r3, '):
            count = int(ins.op_str.split(',')[1], 0)
            break
    rows, names = [], {}
    for i in range(count):
        h, fn, mk = e.u32(keys + 4 * i), e.u32(lua + 4 * i), e.u32(make + 4 * i)
        sym = e.syms.get(fn or mk, '')
        name = sym.split('_MakeEvent_')[0] if '_MakeEvent_' in sym else None
        if not name:
            print('event key %08x has no callbacks (skipped)' % h, file=sys.stderr)
            continue
        if lookup2(name) != h: print('event %s: hash mismatch' % name, file=sys.stderr)
        names[h] = name
        if not fn:
            rows.append((name, 0, 0, '', 0, '-'))
            continue
        nargs, size, reads, ctor, prev = None, 0, {}, None, None

        def on_call(site, t, regs, stack):
            nonlocal size, ctor
            s = e.syms.get(t, '')
            if s == '__nw__5EventUi': size = regs.get('r3') or 0
            elif s in READ_TYPES and regs.get('r4') is not None: reads.setdefault(regs['r4'], READ_TYPES[s])
            elif s.startswith('__%d%s' % (len(name), name)): ctor = s
        ppcelf.walk(e, fn, e.func_end(fn), on_call)
        for ins in e.disasm(fn, e.func_end(fn)):
            if prev == 'gettop' and ins.mnemonic == 'cmpwi':
                nargs = int(ins.op_str.split(',')[1], 0)
                break
            prev = 'gettop' if ins.mnemonic == 'bl' and e.syms.get(int(ins.op_str, 0)) == 'lua_gettop' else None
        fl = fields.get(name, [])
        if nargs is None or sorted(reads) != list(range(1, nargs + 1)) or len(fl) < nargs or not ctor:
            print('event %s: %s arguments, reads %s, fields %s, constructor %s' % (name, nargs, reads, fl, ctor),
                  file=sys.stderr)
        args = ['%s:%s' % (fl[k] if k < len(fl) else 'arg%d' % (k + 1), reads.get(k + 1, 'number'))
                for k in range(nargs or 0)]
        rows.append((name, 2, nargs or 0, ' '.join(args), size, 'fire'))
    return sorted(rows), names


# ---------------------------------------------------------------- PC and Xbox
def binder_function(img):
    """Where LuaBindery::BindToGameCode starts: the function holding the
    push/push/mov edi/call sequences, at the 16-byte boundary after the int3
    padding before it."""
    text = img.read(img.TEXT_LO, img.TEXT_HI - img.TEXT_LO)
    hits = []
    for m in re.finditer(rb'\x68(....)\x68(....)\xbf(....)\xe8', text, re.S):
        fn, nm, tb = (struct.unpack('<I', g)[0] for g in m.groups())
        if X.is_code(fn) and X.cstr(nm) and X.cstr(tb) and re.fullmatch(r'[A-Za-z_]\w*', X.cstr(nm)) \
                and re.fullmatch(r'[A-Za-z_]\w*', X.cstr(tb)):
            hits.append(img.TEXT_LO + m.start())
    if not hits: raise SystemExit('no binder calls found')
    first = hits[0]
    if hits[-1] - first > 0x2000: raise SystemExit('binder calls spread over %x bytes' % (hits[-1] - first))
    a = first
    while not (a % 16 == 0 and img.read(a - 2, 2) in (b'\xcc\xcc', b'\xc3\xcc')):
        a -= 1
        if a < first - 0x3000: raise SystemExit('no function start before %x' % first)
    return a


def x86_bindings(img, gc_rows):
    X.pcmap = img
    start = binder_function(img)
    rows = X.scan_bindings([start])
    gc = {tuple(r[:3]): r for r in gc_rows}
    out = []
    for r in rows:
        kind, tbl, name, fn = r[:4]
        g = gc.get((kind, tbl, name))
        stub = X.stub_kind(int(fn, 16), g[6]) if g and fn != '-' else ''
        fsym = r[4] if '+0x' not in r[4] else '-'
        out.append((kind, tbl, name, fn, fsym, '-', '', '', stub))
    return start, out


def x86_events(img, names, gc_events_rows):
    """The PC's or Xbox's own gEventLuaBindings: the GameCube rows with this
    build's binding and argument count."""
    text = img.read(img.TEXT_LO, img.TEXT_HI - img.TEXT_LO)
    found = collections.OrderedDict()
    for m in re.finditer(rb"\x8b\x0c\x85(....)\x3b\xca.{0,32}?\x3d(....).{0,16}?\x8b\x04\x85(....)", text, re.S):
        keys, count, table = (struct.unpack('<I', g)[0] for g in m.groups())
        found.setdefault((keys, count), [])
        if table not in found[(keys, count)]: found[(keys, count)].append(table)
    (keys, count), tables = next((k, v) for k, v in found.items() if len(v) == 3)
    lua, first = None, None
    for t in tables:
        ents = [img.u32(t + 4 * i) for i in range(count)]
        firsts = set()
        for v in ents:
            if not v: continue
            for ins in x86.disasm(img.read(v, 0x40), v):
                if ins.text.startswith('call'):
                    firsts.add(ins.target)
                    break
            else:
                firsts.add(None)
        if len(firsts) == 1:
            lua, first = t, firsts.pop()
    ev = {r[0]: r for r in gc_events_rows}
    rows = []
    for i in range(count):
        h, fn = img.u32(keys + 4 * i), img.u32(lua + 4 * i)
        name = names.get(h)
        if not name:
            if fn: print('%x: key %08x has a Lua binding and no GameCube name' % (keys, h), file=sys.stderr)
            continue
        g = ev[name]
        if not fn:
            rows.append((name, 0, 0, '', 0, '-'))
            continue
        nargs, after = None, False
        for ins in x86.disasm(img.read(fn, 0x60), fn):
            if after:
                m = re.match(r'^cmp eax, (0x[0-9a-f]+)$', ins.text)
                if m: nargs = int(m.group(1), 16)
                elif ins.text == 'test eax, eax': nargs = 0
                elif ins.text.startswith('add esp'): continue
                break
            if ins.text.startswith('call') and ins.target == first: after = True
        args = g[3].split()
        if nargs is None or nargs > len(args):
            args += ['arg%d:number' % (k + 1) for k in range(len(args), nargs or 0)]
        rows.append((name, 2, nargs or 0, ' '.join(args[:nargs or 0]), g[4], 'fire'))
    return (keys, lua, first), sorted(rows)


def main():
    root, work = sys.argv[1:3]
    os.makedirs(work, exist_ok=True)
    p = lambda *a: os.path.join(root, *a)  # noqa: E731
    e = ppcelf.Elf(p('nfs-data', 'mw', 'gc-release', 'NFSMWRELEASE.ELF'), paired_singles=True)
    gc_rows = scan_wii.scan_bindings(e)
    scan_wii.write_bindings(os.path.join(work, 'mw_gc_bindings.tsv'), gc_rows)
    fields = event_fields(p('exes', 'gamecube', 'mw-vanilla', 'NFSMWRELEASE.h'))
    gc_ev, names = gc_events(e, fields)
    scan_wii.write_events(os.path.join(work, 'mw_gc_events.tsv'), gc_ev)
    report = ['GameCube: %d bindings, %d events (%d with a Lua binding)' % (
        len(gc_rows), len(gc_ev), sum(1 for r in gc_ev if r[5] == 'fire'))]
    builds = {'GameCube': gc_rows}
    events = {'GameCube': gc_ev}
    for label, exe, mp in (('PC', p('exes', 'pc', 'mw', 'speed.exe'), p('exes', 'pc', 'mw', 'speed.map')),
                           ('Xbox', p('exes', 'xbox', 'mw-vanilla', 'default.xbe'),
                            p('exes', 'xbox', 'mw-vanilla', 'default.xbe.map'))):
        img = pemap.Image(exe, mp)
        start, rows = x86_bindings(img, gc_rows)
        (keys, lua, first), ev = x86_events(img, names, gc_ev)
        tag = label.lower()
        scan_wii.write_bindings(os.path.join(work, 'mw_%s_bindings.tsv' % tag), rows)
        scan_wii.write_events(os.path.join(work, 'mw_%s_events.tsv' % tag), ev)
        builds[label], events[label] = rows, ev
        report.append('%s: %d bindings from BindToGameCode at %x; %d events (%d with a Lua binding), keys at %x, '
                      'Lua bindings at %x, each calling %x (%s) first' % (
                          label, len(rows), start, len(ev), sum(1 for r in ev if r[5] == 'fire'), keys, lua, first,
                          img.name(first)))
    # compare the builds
    sets = {b: {tuple(r[:3]) for r in rows} for b, rows in builds.items()}
    for b, exe in (('PC', p('exes', 'pc', 'mw', 'speed.exe')), ('Xbox', p('exes', 'xbox', 'mw-vanilla', 'default.xbe'))):
        img = open(exe, 'rb').read()
        only_gc = sets['GameCube'] - sets[b]
        report.append('bindings the GameCube build has and the %s scan didn\'t find: %s' % (
            b, ', '.join('%s %s.%s' % k for k in sorted(only_gc)) or 'none'))
        if only_gc:
            gone = [k for k in sorted(only_gc) if any(b'\0' + w.encode() + b'\0' not in img for w in k[1:])]
            report.append('  of those, names missing from the %s strings: %s' % (
                b, ', '.join('%s.%s' % k[1:] for k in gone) or 'none'))
        report.append('bindings only in the %s build: %s' % (
            b, ', '.join('%s %s.%s' % k for k in sorted(sets[b] - sets['GameCube'])) or 'none'))
        ge = {r[0]: r[2] for r in events['GameCube'] if r[5] == 'fire'}
        be = {r[0]: r[2] for r in events[b] if r[5] == 'fire'}
        diff = sorted(set(ge) ^ set(be)) + sorted(n for n in set(ge) & set(be) if ge[n] != be[n])
        report.append('events that differ between the GameCube and %s (binding or argument count): %s' % (
            b, ', '.join(diff) or 'none'))
    gs = {tuple(r[:3]): r[8] for r in gc_rows}
    for b in ('PC', 'Xbox'):
        d = ['%s.%s: GameCube "%s", %s "%s"' % (r[1], r[2], gs.get(tuple(r[:3]), ''), b, r[8])
             for r in builds[b] if gs.get(tuple(r[:3]), '') != r[8]]
        report.append('stubs that differ between the GameCube and %s: %s' % (b, '; '.join(d) or 'none'))
    words = sorted({r[2] for r in gc_rows} | {r[1] for r in gc_rows if r[1]} | {r[0] for r in gc_ev if r[5] == 'fire'})
    for ps2 in (p('exes', 'ps2', 'mw-beta', 'NFS.ELF'), p('nfs-data', 'mw', 'ps2-milestone', 'NFS.ELF')):
        img = open(ps2, 'rb').read()
        missing = [w for w in words if b'\0' + w.encode() + b'\0' not in img]
        report.append('%s: of the %d bound and event names, missing from its strings: %s' % (
            os.path.relpath(ps2, root), len(words), ', '.join(missing) or 'none'))
    with open(os.path.join(work, 'mw_builds.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    print('\n'.join(report), file=sys.stderr)


if __name__ == '__main__':
    main()
