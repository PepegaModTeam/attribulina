#!/usr/bin/env python3
"""Lists what a Wii build (CodeWarrior ELF with a symbol table: Carbon's or
Undercover's) gives gameplay Lua, in the same columns as scan_carbon_pc.py.
scan_mw.py reuses its binding scan for Most Wanted's GameCube build.

    python3 -I -B scan_wii.py <elf> <bindings.tsv> <events.tsv> <messages.tsv> [<name file> ...]

bindings.tsv: every call to a binder template (BindFunction<...>,
  BindVoidFunction<...>, BindRawFunction, BindVoidMethod<...>,
  BindMethod<...>, BindSingleton<...>) in the functions that make them
  (LuaBindery::BindToGameCode, BindCustomMethods), and the lua_CFunctions
  those functions set as bare globals (lua_pushcclosure, lua_pushstring,
  lua_settable on LUA_GLOBALSINDEX; table column empty). Columns: kind
  (function/method/object), table or metatable, name, C function address,
  its symbol, binder symbol, return type, parameter types (CodeWarrior codes
  turned into C), stub (what the C function does when it does nothing).
events.tsv: the EventSys registry (LookupEvent's tables): name, flags, field
  count, fields (Name:type), definition size. Flag bit 1 marks the events the
  E<Name> globals fire.
messages.tsv: the M<Name> globals: HandleGlobalIndex's table of
  {StringHash32(name), lua_CFunction} (HandleMessageIndex), the argument count
  the function wants (its `cmpwi r3, N` after lua_gettop), the message's
  fields from the Hermes registry (LookupMessage's tables), and how the
  function reads each argument (lua_tonumber/toboolean/tostring/touserdata,
  "-" for one it never reads). Columns: name, arguments, fields (Name:type id),
  reads. Empty when the build has no M globals (Carbon).
Field names come from their StringHash32("f" + name), looked up among the
identifiers in the name files given (headers, data/messages/*.txt).
"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppcelf  # noqa: E402
from msg_table import lookup2  # noqa: E402

EV_TYPES = {0x9a98b235: 'float', 0x570a3a9d: 'int', 0xcb6e83a7: 'uint', 0xadfefc2c: 'string',
            0x91d0a144: 'hash(string)', 0x63e5073a: 'hash64(string)', 0x25ec65c7: 'uint8', 0x837e34ee: 'int8',
            0x65346453: 'int16', 0x9647a9ca: 'uint16', 0x0c6c765b: 'int64', 0xbcbc0bf0: 'uint64', 0x49fe54c6: 'double'}

# ------------------------------------------------------------ CodeWarrior types
BASIC = {'b': 'bool', 'i': 'int', 'f': 'float', 'd': 'double', 'c': 'char', 'v': 'void', 's': 'short',
         'l': 'long', 'x': 'long long', 'w': 'wchar_t'}


def cw_types(s):
    """'P16GRuntimeInstance,PCc,b' (or with '_' between, as in NFSC.map) ->
    ['GRuntimeInstance *', 'const char *', 'bool']."""
    out, i = [], 0
    while i < len(s):
        if s[i] in ',_':
            i += 1
            continue
        quals, const, unsigned = '', False, False
        while i < len(s) and s[i] in 'PRCU':
            if s[i] == 'P': quals += '*'
            elif s[i] == 'R': quals += '&'
            elif s[i] == 'C': const = True
            else: unsigned = True
            i += 1
        if i < len(s) and s[i].isdigit():
            j = i
            while s[j].isdigit(): j += 1
            n = int(s[i:j]); base = s[j:j + n]; i = j + n
        elif i < len(s) and s[i] == 'Q':
            n = int(s[i + 1]); i += 2; parts = []
            for _ in range(n):
                j = i
                while s[j].isdigit(): j += 1
                k = int(s[i:j]); parts.append(s[j:j + k]); i = j + k
            base = '::'.join(parts)
        elif i < len(s):
            base = BASIC.get(s[i], '?' + s[i]); i += 1
        else:
            break
        if unsigned: base = 'unsigned ' + base
        t = ('const ' if const else '') + base + ((' ' + quals[::-1]) if quals else '')
        out.append(t)
    return out


def one_type(s, i):
    """Where the CodeWarrior type code starting at s[i] ends."""
    while s[i] in 'PRCU': i += 1
    if s[i].isdigit():
        j = i
        while s[j].isdigit(): j += 1
        return j + int(s[i:j])
    if s[i] == 'Q':
        j = i + 2
        for _ in range(int(s[i + 1])):
            k = j
            while s[k].isdigit(): k += 1
            j = k + int(s[j:k])
        return j
    return i + 1


def demangle_h(name):
    """The GameCube compiler's template names, BindFunction__H2ZbZP8ISimable_<params>,
    in the Wii's form, BindFunction<b,P8ISimable>__F<params>. Other names are
    returned as they are."""
    m = re.match(r'^(Bind\w+?)__H(\d)(Z.*)$', name)
    if not m: return name
    s, i, args = m.group(3), 0, []
    for _ in range(int(m.group(2))):
        if s[i] != 'Z': return name
        j = one_type(s, i + 1)
        args.append(s[i + 1:j])
        i = j
    if s[i] != '_': return name
    return '%s<%s>__F%s' % (m.group(1), ','.join(args), s[i + 1:])


def binder_kind(name):
    """(kind, return type, params) from a binder's symbol, or None."""
    name = demangle_h(name)
    m = re.match(r'^BindFunction<([^>]*)>__F', name)
    if m:
        t = cw_types(m.group(1)); return 'function', t[0], t[1:]
    m = re.match(r'^BindVoidFunction<([^>]*)>__F', name)
    if m: return 'function', 'void', cw_types(m.group(1))
    if name.startswith('BindVoidFunction__F'): return 'function', 'void', []
    if name.startswith('BindRawFunction__F'): return 'function', 'raw', ['raw']
    m = re.match(r'^BindVoidMethod<([^>]*)>__F', name)
    if m:
        t = cw_types(m.group(1)); return 'method', 'void', t[1:]
    m = re.match(r'^BindMethod<([^>]*)>__F', name)
    if m:
        t = cw_types(m.group(1)); return 'method', t[0], t[2:]
    if name.startswith('BindSingleton<'): return 'object', '', []
    return None


def stub_kind(e, va, ret):
    """What a function that does nothing does: its first instructions."""
    ins = [(i.mnemonic, i.op_str) for i in e.disasm(va, va + 12)]
    if not ins: return ''
    if ins[0][0] == 'blr': return 'does nothing'
    if ret == 'void' and len(ins) > 1 and ins[1][0] == 'blr' and ins[0][0] in ('lbz', 'lhz', 'lwz') \
            and ins[0][1].split(',')[0] in ('r0', 'r3'):
        return 'does nothing'                  # reads a flag into a register nothing uses
    if len(ins) > 1 and ins[1][0] == 'blr' and ins[0][0] == 'li' and ins[0][1].startswith('r3, '):
        v = int(ins[0][1].split(',')[1], 0)
        return 'always returns %d' % v
    return ''


def raw_stub(e, va):
    """A lua_CFunction that returns 0 at once pushes nothing and does nothing."""
    ins = [(i.mnemonic, i.op_str) for i in e.disasm(va, va + 8)]
    if len(ins) > 1 and ins[0] == ('li', 'r3, 0') and ins[1][0] == 'blr': return 'does nothing'
    return ''


READS = {'lua_tonumber': 'number', 'lua_toboolean': 'boolean', 'lua_tostring': 'string', 'lua_touserdata': 'userdata'}


def walk_values(e, a):
    """[(address, mnemonic, value)] of every register value the walk computes."""
    out = []
    regs = {'r13': e.by_name.get('_SDA_BASE_'), 'r2': e.by_name.get('_SDA2_BASE_')}
    for ins in e.disasm(a, e.func_end(a)):
        ops = [o.strip() for o in ins.op_str.split(',')] if ins.op_str else []
        m = ins.mnemonic
        try:
            if m == 'lis': regs[ops[0]] = (int(ops[1], 0) << 16) & 0xffffffff
            elif m == 'li': regs[ops[0]] = int(ops[1], 0) & 0xffffffff
            elif m in ('addi', 'addic', 'addic.') and regs.get(ops[1]) is not None:
                regs[ops[0]] = (regs[ops[1]] + int(ops[2], 0)) & 0xffffffff
                out.append((ins.address, m, regs[ops[0]]))
            elif m in ('cmplwi', 'cmpwi') and len(ops) == 2:
                out.append((ins.address, m, int(ops[1], 0)))
        except ValueError:
            pass
    return out


def names_pool(files):
    pool = {}
    for p in files:
        with open(p, 'rb') as f:
            for w in set(re.findall(rb'[A-Za-z_][A-Za-z0-9_]*', f.read())):
                w = w.decode()
                for n in (w[1:] if w.startswith('f') and len(w) > 1 and w[1].isupper() else None, w):
                    if n: pool.setdefault(lookup2('f' + n), n)
    return pool


def registry(e, func):
    """Definitions from a LookupEvent/LookupMessage: its key count and the
    pointer table it indexes after the search."""
    vals = walk_values(e, e.by_name[func])
    count = next(v for a, m, v in vals if m == 'cmplwi')
    datas = [v for a, m, v in vals if m.startswith('addi') and e.section_of(v) in ('.data', '.rodata')]
    keys, ptrs = datas[0], datas[1]
    out = []
    for i in range(count):
        p = e.u32(ptrs + 4 * i)
        h = e.u32(keys + 4 * i)
        rec = struct.unpack('>7I', e.read(p, 28))
        out.append((h, p, rec))
    return out


def scan_bindings(e):
    """The bindings.tsv rows of an Elf."""
    binders = {a: n for a, n in e.syms.items() if binder_kind(n)}
    calls = e.callers(binders)
    makers = sorted({e.name(s).split('+')[0] for v in calls.values() for s in v})
    rows, seen = [], set()
    for mk in makers:
        a = e.by_name[mk]
        pending = {}

        def on_call(site, t, regs, stack):
            # a bare global: lua_pushcclosure(fn), lua_pushstring(name), lua_settable(LUA_GLOBALSINDEX)
            sym = e.syms.get(t)
            if sym == 'lua_pushcclosure': pending['fn'] = regs.get('r4')
            elif sym == 'lua_pushstring': pending['name'] = e.cstr(regs['r4']) if regs.get('r4') else None
            elif sym == 'lua_settable':
                fn, name = pending.pop('fn', None), pending.pop('name', None)
                if regs.get('r4') == 0xffffd8ef and fn and name and ('function', '', name) not in seen:
                    seen.add(('function', '', name))
                    rows.append(('function', '', name, '%x' % fn, e.syms.get(fn, '-'), 'lua_settable(LUA_GLOBALSINDEX)',
                                 'raw', 'raw', raw_stub(e, fn)))
            if t not in binders: return
            kind, ret, params = binder_kind(binders[t])
            r4, r5, r6 = (regs.get(r) for r in ('r4', 'r5', 'r6'))
            if kind == 'function':
                tbl, name, fn = e.cstr(r6) if r6 else '', e.cstr(r4), r5
            elif kind == 'method':
                pmf = [stack.get(r6 + k) for k in (0, 4, 8)] if r6 is not None else []
                fn = next((w for w in pmf if w and e.is_code(w)), None)
                tbl, name = e.cstr(r4), e.cstr(r5)
            else:
                tbl, name, fn = e.cstr(r6), e.cstr(r4), None
            if not name:
                print('unresolved binding at %x (%s)' % (site, binders[t]), file=sys.stderr)
                return
            key = (kind, tbl, name)
            if key in seen: return
            seen.add(key)
            stub = ''
            if fn and e.is_code(fn):
                stub = raw_stub(e, fn) if ret == 'raw' else stub_kind(e, fn, ret)
            rows.append((kind, tbl, name, '%x' % fn if fn else '-', e.syms.get(fn, '-') if fn else '-',
                         binders[t], ret, ', '.join(params), stub))
        ppcelf.walk(e, a, e.func_end(a), on_call)
    return rows


def write_bindings(path, rows):
    with open(path, 'w') as f:
        for r in rows:
            f.write('\t'.join(r) + '\n')


def write_events(path, evs):
    """evs: (name, flags, field count, fields, size, fire) rows."""
    with open(path, 'w') as f:
        for r in evs:
            f.write('%s\t%d\t%d\t%s\t%d\t%s\n' % r)


def main():
    elf, out_b, out_e, out_m = sys.argv[1:5]
    pool = names_pool(sys.argv[5:])
    e = ppcelf.Elf(elf)
    rows = scan_bindings(e)
    write_bindings(out_b, rows)

    # ---------------------------------------------------------------- events
    evs = registry(e, 'LookupEvent__Q28EventSys8RegistryFQ28EventSys6Hash32')
    out = []
    for h, p, (rh, nm, flags, fire, nf, fp, size) in sorted(evs, key=lambda r: e.cstr(r[2][1]) or ''):
        name = e.cstr(nm)
        fl = []
        for j in range(nf):
            th, fh, so = struct.unpack('>3I', e.read(fp + 12 * j, 12))
            fl.append('%s:%s' % (pool.get(fh, '%08x' % fh), EV_TYPES.get(th, '%08x' % th)))
        if lookup2(name) != h: print('event %s: hash mismatch' % name, file=sys.stderr)
        out.append((name, flags, nf, ' '.join(fl), size, 'fire' if fire else '-'))
    write_events(out_e, out)

    # ---------------------------------------------------------------- messages
    msgs = []
    hmi = e.by_name.get('HandleMessageIndex__FUi')
    if hmi:
        vals = walk_values(e, hmi)
        table = next(v for a, m, v in vals if m == 'addi' and e.section_of(v) in ('.data', '.rodata'))
        count = next(v for a, m, v in vals if m == 'cmplwi')
        hermes = {}
        for h, p, rec in registry(e, 'LookupMessage__Q26Hermes8RegistryFQ28EventSys6Hash32'):
            rh, nm, flags, fn, nf, fp = rec[:6]
            fl = []
            for j in range(nf if nf < 64 else 0):
                th, fh, so = struct.unpack('>3I', e.read(fp + 12 * j, 12))
                fl.append('%s:%08x' % (pool.get(fh, '%08x' % fh), th))
            hermes[e.cstr(nm)] = fl
        for i in range(count):
            h, fn = struct.unpack('>II', e.read(table + 8 * i, 8))
            sym = e.syms.get(fn, '')
            name = sym.split('_PostMessage_LuaBinding')[0] if '_PostMessage_LuaBinding' in sym else '?'
            if name == '?' or lookup2(name) != h:
                print('message entry %d: %08x %s' % (i, h, sym), file=sys.stderr)
            nargs = None
            prev = None
            for ins in e.disasm(fn, fn + 0x40):
                if prev == 'gettop' and ins.mnemonic == 'cmpwi':
                    nargs = int(ins.op_str.split(',')[1], 0); break
                prev = 'gettop' if ins.mnemonic == 'bl' and e.syms.get(int(ins.op_str, 0)) == 'lua_gettop' else None
            # how the sender reads each argument: lua_to<kind>(L, index)
            reads = {}

            def on_read(site, t, regs, stack):
                k = READS.get(e.syms.get(t))
                if k and regs.get('r4') is not None: reads.setdefault(regs['r4'], k)
            ppcelf.walk(e, fn, e.func_end(fn), on_read)
            args = ','.join(reads.get(i, '-') for i in range(1, (nargs or 0) + 1))
            msgs.append((name, nargs, ' '.join(hermes.get(name, ['?'])), args))
    with open(out_m, 'w') as f:
        for m in sorted(msgs):
            f.write('%s\t%s\t%s\t%s\n' % m)
    print('%s: %d bindings, %d events (%d with flag bit 1), %d messages' % (
        os.path.basename(elf), len(rows), len(evs), sum(1 for r in evs if r[2][2] & 2), len(msgs)), file=sys.stderr)


if __name__ == '__main__':
    main()
