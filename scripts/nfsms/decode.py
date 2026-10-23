"""Decode one NFS-VltEd value class: its properties and the offset each one
reads, from the order of the reads in its Read(BinaryReader) method.

The setters are proxy delegates, so which property a read lands in is taken
from the property declaration order, matched by type. The C# names ValueN
confirm that order where a class uses them.
"""

import re

from common import IDENT, clean, delegate_sig, method_body, read

# The DataType of a property attribute (A/_0002_0004.cs, in enum order).
DT = {'\\u0011': 0, '\\u0009': 1, '\\u001A': 2, '\\u000D': 3, '\\u001D': 4, '\\u0013': 5, '\\u000E': 6,
      '\\u0005': 7, '\\u000B': 8}
READ = {'float': ('f32', 4), 'uint': ('u32', 4), 'int': ('i32', 4), 'byte': ('u8', 1), 'sbyte': ('i8', 1),
        'short': ('i16', 2), 'ushort': ('u16', 2), 'bool': ('bool8', 1), 'long': ('i64', 8), 'ulong': ('u64', 8),
        'double': ('f64', 8)}
# List classes a property may hold: (kind, bytes read, description, element words).
LISTS = {
    '\\u000C\\u0004': ('f32list', 8, 'pointer + int32 count; elements have one property, Value (f32)', ['Value']),
    '\\u0010\\u0004': ('regionlist', 8, 'pointer + int32 count; elements have Region (u32) and Zone (u32)',
                       ['Region', 'Zone']),
    '\\u000A\\u0004': ('keylist', 4, 'pointer only; the element count is a u8 elsewhere in the struct; elements '
                       'have one property, Value (key)', ['Value']),
    '\\u0003\\u0004': ('ptr2u32', 4, 'pointer only, to two u32 that have no properties (not editable)', None),
    '\\u0020\\u0004': ('bytes', 8, 'pointer + int32 count; property Data is a byte[] (count x elemsize bytes)', None),
    '\\u0005\\u0004': ('string', 4, 'char* string pointer; property String', None),
    '\\u0014\\u0004': ('blob', 8, 'i32 size + pointer; property Blob is the byte[] behind it (read with a '
                       '16-byte compression header)', None),
    '\\u000D\\u0015': ('packagelist', 4, 'pointer only; the element count is a u8 elsewhere in the struct; elements '
                       'have Package (u32) and Level (u32)', ['Package', 'Level']),
    '\\u0017\\u0004': ('refspeclist', 8, 'pointer + i32 count; elements have Group (key), Name (key), Value (u32): '
                       'one RefSpec each', ['Group', 'Name', 'Value']),
}
LIST_TYPES = ('\\u0009\\u0004', '\\u000A\\u0004', '\\u000C\\u0004', '\\u0005\\u0004', '\\u0003\\u0004', '\\u0010\\u0004')
HIDDEN = ('DataRow', 'Field', 'OptionalData', 'ArrayIndex', 'IsSingleValue', 'IsMatched')


def parse_props(src):
    """Property declarations in order, with their [\\u0018\\u0004(...)] attribute."""
    props = []
    for m in re.finditer(r'(\[\\u0018\\u0004(?:\(((?:"[^"]*"|[^)"])*)\))?\]\s*\n\s*)?public '
                         r'((?:[\w<>\[\]]|\\u[0-9A-F]{4})+) ((?:\w|\\u[0-9A-F]{4})+)\s*(\{|\n)', src):
        attr, args, typ, name = m.group(1), m.group(2), m.group(3), m.group(4)
        if typ in ('override', 'sealed', 'class', 'static'):
            continue
        if name.startswith('get_') or name.startswith('set_'):
            continue
        p = dict(cs=name, type=typ, attr=bool(attr), name=None, dt=0, ro=False)
        if attr:
            if args:
                mm = re.match(r'\s*"([^"]*)"', args)
                p['name'] = mm.group(1) if mm else 'Value'
                md = re.search(r'DataType = \\u0002\\u0004\.(\\u[0-9A-F]{4})', args)
                if md:
                    p['dt'] = DT[md.group(1)]
                if 'ReadOnly = true' in args:
                    p['ro'] = True
            else:
                p['name'] = 'Value'
        props.append(p)
    return props


def decode_class(path):
    """dict(props, out, size, problems). out lists the reads in order:
    (what, property, offset, kind, size) with what in prop, list, count, skip, skip0."""
    src = read(path)
    cname = re.search(r'public sealed class (' + IDENT + r') : \\u001E\\u001C', src)
    props = [p for p in parse_props(src) if p['cs'] not in HIDDEN]
    ctor = method_body(src, r'public ' + re.escape(cname.group(1)) + r'\(\)\s*\{') if cname else None
    ctor_inst = []
    if ctor:
        for m in re.finditer(r'new (' + IDENT + r')\((\d*)\)', ctor):
            ctor_inst.append((m.group(1), m.group(2)))
    body = method_body(src, r'public override void \\u0011\(BinaryReader _param1\)\s*\{')
    stm = clean(body) if body else []
    out = []
    off = 0
    getmap = {}
    problems = []
    # the constructor's list instances go to the list-typed properties in order
    for p, ci in zip([p for p in props if p['type'] in LIST_TYPES], ctor_inst):
        p['inst'] = ci
    used = set()

    def tmatch(p, t):
        return p['type'] == t or (t == '\\u0009\\u0004' and p['type'].startswith('\\u'))

    def next_prop(t):
        first = None
        for i, p in enumerate(props):
            if i in used:
                continue
            if first is None:
                first = i
            if tmatch(p, t):
                used.add(i)
                if i != first:
                    problems.append('out of declaration order: %s read before %s' % (p['cs'], props[first]['cs']))
                return p
        problems.append('no property of type %s left' % t)
        return None

    i = 0
    while i < len(stm):
        s = stm[i]
        i += 1
        m = re.match(r'^(' + IDENT + r')\.\\u[0-9A-F]{4}\(\(object\) this, (\(float\) )?(' + IDENT +
                     r')\.\\u0011\(\(object\) _param1\)( == \(byte\) 1| == 1)?\);$', s)
        if m:
            setter, cast, rd, cmp = m.groups()
            rs = delegate_sig(rd)
            ss = delegate_sig(setter)
            kind, sz = READ[rs[1]]
            vt = ss[2].split(',')[1].strip().split(' ')[0] if ss and ss[0] == 'delegate' else '?'
            p = next_prop(vt)
            if cast:
                kind = kind + '->f32'
            if cmp:
                kind = 'bool8' if rs[1] == 'byte' else 'bool32'
            out.append(('prop', p, off, kind, sz))
            off += sz
            continue
        m = re.match(r'^(' + IDENT + r')\.\\u[0-9A-F]{4}\(\(object\) this\)\.\\u000E\(_param1\);$', s)
        if m:
            g = m.group(1)
            if g in getmap:
                p = getmap[g]
            else:
                gs = delegate_sig(g)
                p = next_prop(gs[1] if gs else '?')
                getmap[g] = p
            inst = p.get('inst', (p['type'], ''))[0] if p else '?'
            lk = LISTS.get(inst, ('list?', 4, inst, None))
            out.append(('list', p, off, lk, lk[1]))
            off += lk[1]
            continue
        m = re.match(r'^(' + IDENT + r')\.\\u[0-9A-F]{4}\(\(object\) this\)\.set_\\u000B\((' + IDENT +
                     r')\.\\u0011\(\(object\) _param1\)\);$', s)
        if m:
            g = m.group(1)
            p = next_prop(delegate_sig(g)[1])
            getmap[g] = p
            out.append(('count', p, off, 'u8', 1))
            off += 1
            continue
        # a read kept in a local or a field: no property
        m = re.match(r'^(?:\w+ num\d* = \(\w+\) |this\.' + IDENT + r' = )?(' + IDENT +
                     r')\.\\u0011\(\(object\) _param1\);$', s)
        if m:
            kind, sz = READ[delegate_sig(m.group(1))[1]]
            out.append(('skip', None, off, kind, sz))
            off += sz
            continue
        m = re.match(r'^if \((' + IDENT + r')\.\\u0011\(\(object\) _param1\) != 0U\)$', s)
        if m:
            out.append(('skip0', None, off, 'u32', 4))
            off += 4
            while i < len(stm) and (stm[i] == 'else' or stm[i].startswith('throw')):
                i += 1
            continue
        # a count read into a list the class keeps in a field
        m = re.match(r'^' + IDENT + r'\.\\u0011\(\(object\) this\.' + IDENT + r', (' + IDENT +
                     r')\.\\u0011\(\(object\) _param1\)\);$', s)
        if m:
            kind, sz = READ[delegate_sig(m.group(1))[1]]
            out.append(('count', None, off, kind, sz))
            off += sz
            continue
        m = re.match(r'^\\u0005\\u0013\\u000D\.\\u0011\(\(object\) _param1, (\d+)\);$', s)
        if m:
            n = int(m.group(1))
            out.append(('skip', None, off, 'bytes', n))
            off += n
            continue
        if s == 'else' or s.startswith('throw'):
            continue
        problems.append('unparsed: ' + s)
    return dict(props=props, out=out, size=off, problems=problems, ctor=ctor_inst)
