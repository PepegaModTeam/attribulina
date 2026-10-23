"""A small ECMA-335 metadata reader for the EASharp bindings of World's 2010 beta.

World's gameplay code is exposed to C# through EASharp: exes/pc/world-2010-beta/
gameplay.dll is the managed side (enums, and classes whose properties wrap
native members), and every native accessor is registered by name in that
build's nfsw.exe. Only what scripts/world/easharp.py needs is read: TypeDefs,
Fields, Constants, Properties and ImplMap, from the #~ tables.
"""
import struct

def pe_sections(d):
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    ns = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    magic = struct.unpack_from('<H', d, pe + 24)[0]
    dd = pe + 24 + (96 if magic == 0x10B else 112)
    secs = []
    for i in range(ns):
        s = pe + 24 + opt + i * 40
        nm, vs, va, rs, ra = struct.unpack_from('<8sIIII', d, s)
        secs.append((va, max(vs, rs), ra))
    return dd, secs

def rva2off(secs, rva):
    for va, sz, ra in secs:
        if va <= rva < va + sz: return ra + rva - va
    raise ValueError(hex(rva))

TABLES = ['Module', 'TypeRef', 'TypeDef', 'FieldPtr', 'Field', 'MethodPtr', 'MethodDef', 'ParamPtr', 'Param',
          'InterfaceImpl', 'MemberRef', 'Constant', 'CustomAttribute', 'FieldMarshal', 'DeclSecurity', 'ClassLayout',
          'FieldLayout', 'StandAloneSig', 'EventMap', 'EventPtr', 'Event', 'PropertyMap', 'PropertyPtr', 'Property',
          'MethodSemantics', 'MethodImpl', 'ModuleRef', 'TypeSpec', 'ImplMap', 'FieldRVA', 'EncLog', 'EncMap',
          'Assembly', 'AssemblyProcessor', 'AssemblyOS', 'AssemblyRef', 'AssemblyRefProcessor', 'AssemblyRefOS',
          'File', 'ExportedType', 'ManifestResource', 'NestedClass', 'GenericParam', 'MethodSpec', 'GenericParamConstraint']
CODED = {
    'TypeDefOrRef': (2, ['TypeDef', 'TypeRef', 'TypeSpec']),
    'HasConstant': (2, ['Field', 'Param', 'Property']),
    'HasCustomAttribute': (5, ['MethodDef', 'Field', 'TypeRef', 'TypeDef', 'Param', 'InterfaceImpl', 'MemberRef', 'Module',
                               'DeclSecurity', 'Property', 'Event', 'StandAloneSig', 'ModuleRef', 'TypeSpec', 'Assembly',
                               'AssemblyRef', 'File', 'ExportedType', 'ManifestResource', 'GenericParam',
                               'GenericParamConstraint', 'MethodSpec']),
    'HasFieldMarshal': (1, ['Field', 'Param']),
    'HasDeclSecurity': (2, ['TypeDef', 'MethodDef', 'Assembly']),
    'MemberRefParent': (3, ['TypeDef', 'TypeRef', 'ModuleRef', 'MethodDef', 'TypeSpec']),
    'HasSemantics': (1, ['Event', 'Property']),
    'MethodDefOrRef': (1, ['MethodDef', 'MemberRef']),
    'MemberForwarded': (1, ['Field', 'MethodDef']),
    'Implementation': (2, ['File', 'AssemblyRef', 'ExportedType']),
    'CustomAttributeType': (3, ['?0', '?1', 'MethodDef', 'MemberRef', '?4']),
    'ResolutionScope': (2, ['Module', 'ModuleRef', 'AssemblyRef', 'TypeRef']),
    'TypeOrMethodDef': (1, ['TypeDef', 'MethodDef']),
}
# column kinds: u8 u16 u32 str guid blob, a table name (simple index) or a coded index name
SCHEMA = {
    'Module': ['u16', 'str', 'guid', 'guid', 'guid'],
    'TypeRef': ['ResolutionScope', 'str', 'str'],
    'TypeDef': ['u32', 'str', 'str', 'TypeDefOrRef', 'Field', 'MethodDef'],
    'FieldPtr': ['Field'], 'Field': ['u16', 'str', 'blob'], 'MethodPtr': ['MethodDef'],
    'MethodDef': ['u32', 'u16', 'u16', 'str', 'blob', 'Param'], 'ParamPtr': ['Param'], 'Param': ['u16', 'u16', 'str'],
    'InterfaceImpl': ['TypeDef', 'TypeDefOrRef'], 'MemberRef': ['MemberRefParent', 'str', 'blob'],
    'Constant': ['u8', 'u8', 'HasConstant', 'blob'], 'CustomAttribute': ['HasCustomAttribute', 'CustomAttributeType', 'blob'],
    'FieldMarshal': ['HasFieldMarshal', 'blob'], 'DeclSecurity': ['u16', 'HasDeclSecurity', 'blob'],
    'ClassLayout': ['u16', 'u32', 'TypeDef'], 'FieldLayout': ['u32', 'Field'], 'StandAloneSig': ['blob'],
    'EventMap': ['TypeDef', 'Event'], 'EventPtr': ['Event'], 'Event': ['u16', 'str', 'TypeDefOrRef'],
    'PropertyMap': ['TypeDef', 'Property'], 'PropertyPtr': ['Property'], 'Property': ['u16', 'str', 'blob'],
    'MethodSemantics': ['u16', 'MethodDef', 'HasSemantics'], 'MethodImpl': ['TypeDef', 'MethodDefOrRef', 'MethodDefOrRef'],
    'ModuleRef': ['str'], 'TypeSpec': ['blob'], 'ImplMap': ['u16', 'MemberForwarded', 'str', 'ModuleRef'],
    'FieldRVA': ['u32', 'Field'], 'EncLog': ['u32', 'u32'], 'EncMap': ['u32'],
    'Assembly': ['u32', 'u16', 'u16', 'u16', 'u16', 'u32', 'blob', 'str', 'str'], 'AssemblyProcessor': ['u32'],
    'AssemblyOS': ['u32', 'u32', 'u32'], 'AssemblyRef': ['u16', 'u16', 'u16', 'u16', 'u32', 'blob', 'str', 'str', 'blob'],
    'AssemblyRefProcessor': ['u32', 'AssemblyRef'], 'AssemblyRefOS': ['u32', 'u32', 'u32', 'AssemblyRef'],
    'File': ['u32', 'str', 'blob'], 'ExportedType': ['u32', 'u32', 'str', 'str', 'Implementation'],
    'ManifestResource': ['u32', 'u32', 'str', 'Implementation'], 'NestedClass': ['TypeDef', 'TypeDef'],
    'GenericParam': ['u16', 'u16', 'TypeOrMethodDef', 'str'], 'MethodSpec': ['MethodDefOrRef', 'blob'],
    'GenericParamConstraint': ['GenericParam', 'TypeDefOrRef'],
}

class Meta:
    def __init__(self, path):
        d = open(path, 'rb').read()
        dd, secs = pe_sections(d)
        clr_rva, clr_size = struct.unpack_from('<II', d, dd + 14 * 8)
        clr = rva2off(secs, clr_rva)
        md_rva, md_size = struct.unpack_from('<II', d, clr + 8)
        md = rva2off(secs, md_rva)
        assert d[md:md + 4] == b'BSJB'
        vlen = struct.unpack_from('<I', d, md + 12)[0]
        p = md + 16 + vlen
        nstreams = struct.unpack_from('<H', d, p + 2)[0]
        p += 4
        self.streams = {}
        for _ in range(nstreams):
            off, size = struct.unpack_from('<II', d, p)
            p += 8
            e = d.index(b'\0', p)
            name = d[p:e].decode()
            p = (e + 4) & ~3
            self.streams[name] = d[md + off:md + off + size]
        self.d = d
        self._tables()

    def _tables(self):
        t = self.streams.get('#~') or self.streams['#-']
        heaps = t[6]
        valid = struct.unpack_from('<Q', t, 8)[0]
        p = 24
        rows = {}
        for i, name in enumerate(TABLES):
            if valid >> i & 1:
                rows[name] = struct.unpack_from('<I', t, p)[0]
                p += 4
        for i in range(len(TABLES), 64):
            if valid >> i & 1: p += 4  # unknown table
        self.rows = rows
        strw = 4 if heaps & 1 else 2; guidw = 4 if heaps & 2 else 2; blobw = 4 if heaps & 4 else 2
        def width(kind):
            if kind == 'u8': return 1
            if kind == 'u16': return 2
            if kind == 'u32': return 4
            if kind == 'str': return strw
            if kind == 'guid': return guidw
            if kind == 'blob': return blobw
            if kind in CODED:
                bits, tabs = CODED[kind]
                m = max(rows.get(x, 0) for x in tabs)
                return 2 if m < (1 << (16 - bits)) else 4
            return 2 if rows.get(kind, 0) < 65536 else 4
        self.tables = {}
        for name in TABLES:
            n = rows.get(name, 0)
            if not n: continue
            cols = SCHEMA[name]
            ws = [width(c) for c in cols]
            out = []
            for r in range(n):
                vals = []
                for c, w in zip(cols, ws):
                    v = int.from_bytes(t[p:p + w], 'little'); p += w
                    vals.append(v)
                out.append(vals)
            self.tables[name] = out

    def string(self, i):
        s = self.streams['#Strings']
        return s[i:s.index(b'\0', i)].decode('utf-8', 'replace')

    def blob(self, i):
        b = self.streams['#Blob']
        x = b[i]
        if x & 0x80 == 0: n, h = x, 1
        elif x & 0xC0 == 0x80: n, h = ((x & 0x3F) << 8) | b[i + 1], 2
        else: n, h = ((x & 0x1F) << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3], 4
        return b[i + h:i + h + n]

    def coded(self, kind, v):
        bits, tabs = CODED[kind]
        return tabs[v & ((1 << bits) - 1)], v >> bits

    def type_name(self, kind, v):
        tab, idx = self.coded(kind, v)
        if idx == 0: return None
        row = self.tables[tab][idx - 1]
        if tab == 'TypeDef': return (self.string(row[2]) + '.' if row[2] else '') + self.string(row[1])
        if tab == 'TypeRef': return (self.string(row[2]) + '.' if row[2] else '') + self.string(row[1])
        return tab

    def typedefs(self):
        """[(index, namespace, name, flags, extends, [field rows (index, flags, name, sig)])]"""
        tds = self.tables['TypeDef']; fields = self.tables.get('Field', [])
        out = []
        for i, (flags, name, ns, ext, fl, ml) in enumerate(tds):
            end = tds[i + 1][4] if i + 1 < len(tds) else len(fields) + 1
            fs = [(j, fields[j - 1][0], self.string(fields[j - 1][1]), self.blob(fields[j - 1][2])) for j in range(fl, end)]
            out.append((i + 1, self.string(ns), self.string(name), flags, self.type_name('TypeDefOrRef', ext), fs))
        return out

    def constants(self):
        out = {}
        for typ, _, parent, val in self.tables.get('Constant', []):
            tab, idx = self.coded('HasConstant', parent)
            if tab == 'Field': out[idx] = (typ, self.blob(val))
        return out

    def nested(self):
        return {n: e for n, e in self.tables.get('NestedClass', [])}

    def class_layouts(self):
        return {p: (pack, size) for pack, size, p in self.tables.get('ClassLayout', [])}

    def field_layouts(self):
        return {f: off for off, f in self.tables.get('FieldLayout', [])}

ELEM = {0x02: ('<?', 1), 0x04: ('<b', 1), 0x05: ('<B', 1), 0x06: ('<h', 2), 0x07: ('<H', 2), 0x08: ('<i', 4),
        0x09: ('<I', 4), 0x0A: ('<q', 8), 0x0B: ('<Q', 8)}


def enums(m):
    """{qualified C++ name: (size, [(name, value)])} from the enums of an assembly."""
    tds = m.typedefs(); nest = m.nested(); consts = m.constants()
    out = {}
    for t in tds:
        if t[4] != 'System.Enum': continue
        i, ns, n = t[0], t[1], t[2]
        while i in nest:
            i = nest[i]; o = tds[i - 1]; n = o[2] + '::' + n; ns = o[1]
        name = (ns.replace('.', '::') + '::' if ns else '') + n
        size = None; vals = []
        for fi, flags, fname, sig in t[5]:
            if fname == 'value__':
                size = ELEM.get(sig[1], (None, None))[1]
                continue
            if fi in consts:
                typ, blob = consts[fi]
                fmt, w = ELEM[typ]
                vals.append((fname, struct.unpack(fmt, blob[:w])[0]))
        out[name] = (size, vals)
    return out


def exports(path):
    """{name: va} from a PE's export table."""
    d = open(path, 'rb').read()
    dd, secs = pe_sections(d)
    rva, size = struct.unpack_from('<II', d, dd)
    if not rva: return {}, d, secs
    o = rva2off(secs, rva)
    _, _, _, _, _, base, nfun, nnames, afun, aname, aord = struct.unpack_from('<IIHHIIIIIII', d, o)
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    ib = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    out = {}
    for i in range(nnames):
        no = struct.unpack_from('<I', d, rva2off(secs, aname) + i * 4)[0]
        ordi = struct.unpack_from('<H', d, rva2off(secs, aord) + i * 2)[0]
        frva = struct.unpack_from('<I', d, rva2off(secs, afun) + ordi * 4)[0]
        e = d.index(b'\0', rva2off(secs, no))
        out[d[rva2off(secs, no):e].decode()] = ib + frva
    return out, d, secs


def easharp_functions(exe, names):
    """{expose name: function va} from the registration code of an EASharp host
    executable, which stores { ..., function, name, ... } records with
    `mov dword ptr [esp + X], imm` (the function at X - 4)."""
    import capstone
    d = open(exe, 'rb').read()
    dd, secs = pe_sections(d)
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    ib = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    def va(off):
        for v, sz, ra in secs:
            if ra <= off < ra + sz: return ib + v + off - ra
    strings = {}
    for n in names:
        i = d.find(n.encode() + b'\0')
        if i > 0 and d[i - 1] == 0: strings[va(i)] = n
    refs = []
    for sva in strings:
        i = d.find(struct.pack('<I', sva))
        while i >= 0:
            refs.append(i); i = d.find(struct.pack('<I', sva), i + 1)
    if not refs: return {}
    lo, hi = min(refs) - 0x40, max(refs) + 0x40
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    slots = {}
    out = {}
    # disassemble from a little before the first reference; re-sync on failure
    o = lo
    while o < hi:
        got = False
        for ins in md.disasm(d[o:hi], va(o)):
            got = True
            o = ins.address - va(lo) + lo + ins.size
            if ins.mnemonic == 'mov' and len(ins.operands) == 2 and ins.operands[0].type == capstone.x86.X86_OP_MEM \
                    and ins.operands[1].type == capstone.x86.X86_OP_IMM and ins.operands[0].mem.base == capstone.x86.X86_REG_ESP:
                disp = ins.operands[0].mem.disp; imm = ins.operands[1].imm & 0xFFFFFFFF
                slots[disp] = imm
                if imm in strings and disp - 4 in slots:
                    out[strings[imm]] = slots[disp - 4]
        if not got: o += 1
    return out


def sig_type(m, b, i):
    """(type name, next index) of a type in a signature blob."""
    t = b[i]; i += 1
    simple = {0x01: 'void', 0x02: 'bool', 0x03: 'char', 0x04: 'int8', 0x05: 'uint8', 0x06: 'int16', 0x07: 'uint16',
              0x08: 'int32', 0x09: 'uint32', 0x0A: 'int64', 0x0B: 'uint64', 0x0C: 'float', 0x0D: 'double', 0x0E: 'string',
              0x18: 'IntPtr', 0x19: 'UIntPtr', 0x1C: 'object'}
    if t in simple: return simple[t], i
    if t in (0x11, 0x12):  # valuetype / class + TypeDefOrRefEncoded
        x = b[i]
        if x & 0x80 == 0: v, i = x, i + 1
        elif x & 0xC0 == 0x80: v, i = ((x & 0x3F) << 8) | b[i + 1], i + 2
        else: v, i = ((x & 0x1F) << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3], i + 4
        return m.type_name('TypeDefOrRef', v), i
    if t == 0x0F:
        n, i = sig_type(m, b, i); return n + '*', i
    if t == 0x1D:
        n, i = sig_type(m, b, i); return n + '[]', i
    if t == 0x10:
        n, i = sig_type(m, b, i); return n + '&', i
    return 'type%02x' % t, i


def properties(m):
    """{typedef index: [(name, type)]}."""
    props = m.tables.get('Property', []); pmap = m.tables.get('PropertyMap', [])
    out = {}
    for k, (parent, first) in enumerate(pmap):
        end = pmap[k + 1][1] if k + 1 < len(pmap) else len(props) + 1
        lst = []
        for j in range(first, end):
            flags, name, sig = props[j - 1]
            b = m.blob(sig)
            # 0x08 or 0x28, param count, type
            ty, _ = sig_type(m, b, 2)
            lst.append((m.string(name), ty))
        out[parent] = lst
    return out
