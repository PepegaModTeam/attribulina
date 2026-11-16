"""OGVI's model of a game's values, for nfsms scripts run as OGVI runs them
(NFS-VltEd compatible off): VaultLib's classes (quirk-analysis/VaultLib,
heyitsleo's library, which OGVI and the community Attribulator are built on),
the C# properties each one has and the bytes each reads.

Attribulator's update_field finds the words after the field as C# property
names by reflection, exactly as written, and converts the value by the
property's C# type (Attribulator.ModScript.API/Utils/PropertyUtils.cs,
Attribulator.API/Utils/ValueConversionUtils.cs). So the table gives, for each
AttribSys type a game's files use, the VaultLib class the game registers for it
(TypeRegistry: the game's own assembly, then VaultLib.Frameworks.Speed, then
VaultLib.Core's), what kind of value it is, and every public property down to
the C# primitives, with the byte its Read method reads it from and our member
there. The C# enums those properties use follow, with their values.
"""

import os
import re

from common import REPO
from schema import PADDING

VAULTLIB = os.path.join(REPO, 'quirk-analysis', 'VaultLib')
FOLDERS = {'MW': 'MostWanted', 'C': 'Carbon', 'PS': 'ProStreet', 'UC': 'Undercover', 'W': 'World', 'WB': 'World'}

PRIMS = {'bool': 1, 'sbyte': 1, 'byte': 1, 'short': 2, 'ushort': 2, 'int': 4, 'uint': 4, 'long': 8, 'ulong': 8,
         'float': 4, 'double': 8}
READS = {'ReadSingle': 'float', 'ReadDouble': 'double', 'ReadUInt32': 'uint', 'ReadInt32': 'int', 'ReadByte': 'byte',
         'ReadSByte': 'sbyte', 'ReadBoolean': 'bool', 'ReadUInt16': 'ushort', 'ReadInt16': 'short',
         'ReadUInt64': 'ulong', 'ReadInt64': 'long', 'ReadPointer': 'uint'}
CS_TYPES = {'Single': 'float', 'Double': 'double', 'UInt32': 'uint', 'Int32': 'int', 'Byte': 'byte',
            'SByte': 'sbyte', 'Boolean': 'bool', 'UInt16': 'ushort', 'Int16': 'short', 'UInt64': 'ulong',
            'Int64': 'long', 'String': 'string'}
ENUM_BASE = {'byte': 'byte', 'sbyte': 'sbyte', 'short': 'short', 'ushort': 'ushort', 'int': 'int', 'uint': 'uint',
             'long': 'long', 'ulong': 'ulong'}


def body(src, at):
    """The text between the brace at or after `at` and its match, and the index after it."""
    i = src.index('{', at)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i + 1:j], j + 1
    return src[i + 1:], len(src)


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


class Cls:
    def __init__(self, name, folder, file):
        self.name = name
        self.folder = folder
        self.file = file
        self.generic = False
        self.base = None
        self.interfaces = []
        self.typeinfo = []
        self.prim = None      # PrimitiveInfo's C# type
        self.props = {}       # name -> (C# type, settable, accessor text)
        self.order = []       # property names in declaration order
        self.fields = {}      # private field name -> C# type
        self.ctor = ''
        self.read = None


class Enum:
    def __init__(self, name, folder, file, base, values, typeinfo, problem=None):
        self.name, self.folder, self.file, self.base = name, folder, file, base
        self.values = values  # [(name, value)]
        self.typeinfo = typeinfo
        self.problem = problem


def eval_enum(entries):
    out = []
    names = {}
    nxt = 0
    for e in entries:
        e = e.strip()
        if not e:
            continue
        e = re.sub(r'^\[[^\]]*\]\s*', '', e)
        m = re.match(r'^(\w+)\s*(?:=\s*(.+))?$', e, re.S)
        if not m:
            return out, 'cannot read "%s"' % e
        n, expr = m.group(1), m.group(2)
        if expr is not None:
            x = re.sub(r'(?<=[0-9A-Fa-f])[uUlL]+\b', '', expr.strip())
            x = re.sub(r'\b(?:unchecked|checked)\b', '', x)
            x = re.sub(r'\(\s*(?:int|uint|long|ulong|short|ushort|byte|sbyte)\s*\)', '', x)
            try:
                v = eval(x, {'__builtins__': {}}, dict(names))
            except Exception:
                return out, 'cannot evaluate %s = %s' % (n, expr)
        else:
            v = nxt
        names[n] = v
        out.append((n, v))
        nxt = v + 1
    return out, None


def parse_file(path, folder):
    src = strip_comments(open(path, encoding='utf-8-sig', errors='replace').read())
    rel = os.path.relpath(path, VAULTLIB).replace(os.sep, '/')
    classes, enums = [], []
    for m in re.finditer(r'((?:\[[^\]]*\]\s*)*)public\s+(?:sealed\s+|abstract\s+|partial\s+)*enum\s+(\w+)\s*(?::\s*(\w+))?\s*\{',
                         src):
        attrs = m.group(1)
        ti = [a or b for a, b in re.findall(r'VLTTypeInfo\(\s*(?:"([^"]*)"|nameof\((\w+)\))', attrs)]
        b, _ = body(src, m.end() - 1)
        vals, problem = eval_enum(b.split(','))
        enums.append(Enum(m.group(2), folder, rel, ENUM_BASE.get(m.group(3) or 'int', 'int'), vals, ti, problem))
    for m in re.finditer(r'((?:\[[^\]]*\]\s*)*)public\s+((?:sealed\s+|abstract\s+|partial\s+)*)class\s+(\w+)(<[^>]*>)?\s*'
                         r'(?::\s*([^{]+))?\{', src):
        c = Cls(m.group(3), folder, rel)
        c.generic = bool(m.group(4))
        c.abstract = 'abstract' in m.group(2)
        attrs = m.group(1)
        c.typeinfo = [a or b for a, b in re.findall(r'VLTTypeInfo\(\s*(?:"([^"]*)"|nameof\((\w+)\))', attrs)]
        pi = re.search(r'PrimitiveInfo\(typeof\((\w+)\)\)', attrs)
        if pi:
            c.prim = CS_TYPES.get(pi.group(1), pi.group(1))
        if m.group(5):
            parts = [p.strip() for p in re.split(r',(?![^<]*>)', m.group(5)) if p.strip()]
            parts = [re.sub(r'\s+where\s.*$', '', p, flags=re.S) for p in parts]
            if parts and not re.match(r'^I[A-Z]', parts[0]):
                c.base = parts[0]
                c.interfaces = parts[1:]
            else:
                c.interfaces = parts
        text, _ = body(src, m.end() - 1)
        for pm in re.finditer(r'public\s+((?:override\s+|virtual\s+|abstract\s+|new\s+|static\s+)*)([\w<>\[\],\.]+)\s+(\w+)\s*(\{|=>)',
                              text):
            if 'static' in pm.group(1) or pm.group(2) in ('enum', 'class', 'struct', 'interface', 'delegate', 'event'):
                continue
            # only members of this class body (depth 0)
            d = text[:pm.start()].count('{') - text[:pm.start()].count('}')
            if d != 0:
                continue
            t, n = pm.group(2), pm.group(3)
            if pm.group(4) == '=>':
                acc = text[pm.end():text.index(';', pm.end())]
                settable = False
            else:
                acc, _ = body(text, pm.end() - 1)
                settable = bool(re.search(r'(?<![\w])(?<!private )(?<!protected )(?<!internal )set\b', acc))
            c.props[n] = (t, settable, acc)
            c.order.append(n)
        for fm in re.finditer(r'(?:private|protected|internal)\s+(?:readonly\s+)?([\w<>\[\],\.]+)\s+(_?\w+)\s*(?:=[^;]*;|;|\{\s*get;)',
                              text):
            if text[:fm.start()].count('{') - text[:fm.start()].count('}') == 0:
                c.fields[fm.group(2)] = fm.group(1)
        c.text = text
        for cm in re.finditer(r'public\s+%s\s*\([^)]*\)\s*(?::\s*(?:base|this)\([^)]*\)\s*)?\{' % re.escape(c.name), text):
            b, _ = body(text, cm.end() - 1)
            c.ctor += b + '\n'
        rd = re.search(r'public\s+(?:override\s+|sealed\s+)*void\s+Read\(Vault\s+\w+,\s*BinaryReader\s+br\)\s*\{', text)
        if rd:
            c.read, _ = body(text, rd.end() - 1)
        classes.append(c)
    return classes, enums


def load():
    """All classes and enums of the folders a game uses, by folder."""
    if hasattr(load, 'cache'):
        return load.cache
    out = {}
    for folder in os.listdir(VAULTLIB):
        p = os.path.join(VAULTLIB, folder)
        if not os.path.isdir(p) or not folder.startswith('VaultLib.'):
            continue
        cl, en = {}, {}
        for root, _, files in os.walk(p):
            for f in sorted(files):
                if f.endswith('.cs'):
                    c, e = parse_file(os.path.join(root, f), folder)
                    for x in c:
                        cl.setdefault(x.name, x)
                    for x in e:
                        en.setdefault(x.name, x)
        out[folder] = (cl, en)
    load.cache = out
    return out


class Game:
    """VaultLib as one game registers it."""

    def __init__(self, game):
        self.game = game
        allf = load()
        own = 'VaultLib.Support.' + FOLDERS[game]
        # Class and enum lookup by C# name: the game's assembly, the framework, the bases, the core.
        self.folders = [own, 'VaultLib.Frameworks.Speed', 'VaultLib.ModernBase', 'VaultLib.LegacyBase', 'VaultLib.Core']
        self.classes = {}
        self.enums = {}
        for f in reversed(self.folders):
            cl, en = allf[f]
            self.classes.update(cl)
            self.enums.update(en)
        # TypeRegistry: GLOBAL is VaultLib.Core; the game's dictionary gets its
        # StringKey, then the framework's types, then its own assembly's.
        self.registry = {}
        for f in ('VaultLib.Core', 'VaultLib.Frameworks.Speed', own):
            cl, en = allf[f]
            for x in list(cl.values()) + list(en.values()):
                if isinstance(x, Cls) and (x.generic or getattr(x, 'abstract', False)):
                    continue
                for n in x.typeinfo:
                    self.registry[n] = x
            if f == 'VaultLib.Core':
                if game == 'MW':
                    self.registry['Attrib::StringKey'] = allf['VaultLib.LegacyBase'][0]['StringKey64']
                else:
                    self.registry['Attrib::StringKey'] = allf['VaultLib.ModernBase'][0]['StringKey']

    def cls(self, name):
        name = re.sub(r'^(?:[\w\.]+\.)?(\w+)$', r'\1', name)
        return self.classes.get(name)

    def chain(self, c):
        out = []
        seen = set()
        while c is not None and c.name not in seen:
            out.append(c)
            seen.add(c.name)
            b = re.sub(r'<.*$', '', c.base) if c.base else None
            c = self.cls(b) if b else None
        return out

    def kind(self, c):
        """prim/<type>, refspec, string (IStringValue), or struct."""
        ch = self.chain(c)
        names = [x.name for x in ch]
        if 'PrimitiveTypeBase' in names:
            for x in ch:
                if x.prim:
                    return 'prim/' + x.prim
            return 'prim/?'
        if 'BaseRefSpec' in names:
            return 'refspec'
        if any('IStringValue' in i for x in ch for i in x.interfaces):
            return 'string'
        return 'struct'

    def props(self, c):
        """{name: (C# type, settable)} for every public instance property, the
        class's own over its bases'."""
        out = {}
        order = []
        for x in reversed(self.chain(c)):
            for n in x.order:
                t, s, _ = x.props[n]
                if n not in out:
                    order.append(n)
                out[n] = (t, s)
        return out, order

    def accessor(self, c, name):
        for x in self.chain(c):
            if name in x.props:
                return x.props[name][2]
        return ''

    def ctype(self, t, cur=None):
        """A C# property type as the table writes it: a primitive, string,
        enum/<name>, obj/<class>, or <element>[] for an array."""
        t = t.strip()
        if t.endswith('[]'):
            return self.ctype(t[:-2], cur) + '[]'
        t = re.sub(r'^System\.', '', t)
        if t in PRIMS or t == 'string':
            return t
        if t in CS_TYPES:
            return CS_TYPES[t]
        g = re.match(r'^(\w+)<(.+)>$', t)
        if g:
            return 'obj/%s<%s>' % (g.group(1), self.ctype(g.group(2)))
        short = t.split('.')[-1]
        if short in self.enums:
            return 'enum/' + short
        if self.cls(short):
            return 'obj/' + short
        return 'unknown/' + t


class Layout:
    """The properties of a class at their bytes, read from its Read method."""

    def __init__(self, game):
        self.g = game
        self.cache = {}

    def size(self, ctype):
        """The bytes a C# type's Read takes, or None. A string read at a
        place is a hash (RefSpec's parts) or a Text's pointer: 4 bytes."""
        if ctype in PRIMS:
            return PRIMS[ctype]
        if ctype == 'string':
            return 4
        if ctype.startswith('enum/'):
            e = self.g.enums.get(ctype[5:])
            return PRIMS[e.base] if e else None
        if ctype.startswith('obj/'):
            name = ctype[4:]
            if name.startswith('DynamicSizeArray<'):
                return 8
            if name.startswith('VLTPointerContainer<') or name in ('Text',):
                return 4
            lay = self.of(self.g.cls(name))
            return lay['size'] if lay and lay['problem'] is None else None
        return None

    def of(self, c):
        """dict(size, entries=[(name, off, ctype, count)], problem) for class c:
        each public property Read fills, at its offset; a property read through a
        private field (a Text behind a string) under the property's name."""
        if c is None:
            return None
        if c.name in self.cache:
            return self.cache[c.name]
        self.cache[c.name] = None  # recursion guard
        lay = self._of(c)
        self.cache[c.name] = lay
        return lay

    def _of(self, c):
        props, order = self.g.props(c)
        kind = self.g.kind(c)
        if kind.startswith('prim/'):
            p = kind[5:]
            if c.name == 'Text' or p == 'string':
                return dict(size=4, entries=[('Value', 0, 'string', 0)], problem=None)
            # VLTEnumType<T> and the EA::Reflection classes: one Value.
            t = self.g.ctype(props['Value'][0]) if 'Value' in props else p
            return dict(size=self.size(t) or PRIMS.get(p), entries=[('Value', 0, t, 0)], problem=None)
        read = None
        for x in self.g.chain(c):
            if x.read is not None:
                read = x.read
                break
        if read is None:
            return dict(size=None, entries=[], problem='no Read method')
        return self.decode(c, props, read)

    def array_len(self, c, name):
        for x in self.g.chain(c):
            m = re.search(r'\b%s\s*=\s*new\s+[\w\.]+\[(\d+)\]' % re.escape(name), x.ctor)
            if m:
                return int(m.group(1))
        return None

    def backed(self, c, field, props):
        """The public property a private field stands behind (_slotNameText: SlotName),
        by its accessors or a line that copies it (KitName = _kitName.Value)."""
        for n in props:
            acc = self.g.accessor(c, n)
            if re.search(r'\b%s\b' % re.escape(field), acc):
                return n
        for x in self.g.chain(c):
            for m in re.finditer(r'\b(\w+)\s*=\s*%s\.(?:Value|Data|Items)\b' % re.escape(field), getattr(x, 'text', '')):
                if m.group(1) in props:
                    return m.group(1)
        return None

    def decode(self, c, props, read):
        src = re.sub(r'"(?:[^"\\]|\\.)*"', '""', read)
        # Checks that throw or log, which read nothing.
        src = re.sub(r'if\s*\((?:[^()]|\([^()]*\))*\)\s*\{\s*throw[^;]*;\s*\}', '', src)
        src = re.sub(r'if\s*\((?:[^()]|\([^()]*\))*\)\s*throw[^;]*;', '', src)
        # The 32-bit databases' branch.
        src = re.sub(r'if\s*\(\s*vault\.Database\.Options\.Type\s*==\s*DatabaseType\.X86Database\s*\)\s*\{(.*?)\}\s*else\s*\{.*?\}',
                     r'\1', src, flags=re.S)
        src = re.sub(r'if\s*\(\s*vault\.Database\.Options\.Type\s*==\s*DatabaseType\.X64Database\s*\)\s*\{.*?\}\s*else\s*\{(.*?)\}',
                     r'\1', src, flags=re.S)
        src = re.sub(r'if\s*\(\s*!?\s*Enum\.IsDefined[^{]*\{[^}]*\}', '', src, flags=re.S)
        src = re.sub(r'Debug\.\w+\([^;]*\);', '', src)
        out = []
        off = 0
        stmts = self.statements(src)
        for st in stmts:
            got = self.statement(c, props, st, off)
            if isinstance(got, str):
                return dict(size=None, entries=out, problem=got)
            off, ents = got
            out += ents
        return dict(size=off, entries=out, problem=None)

    @staticmethod
    def statements(src):
        """Statements, a for loop as one ('for', bound, body statements)."""
        out = []
        i = 0
        s = src
        while i < len(s):
            while i < len(s) and s[i] in ' \t\r\n;':
                i += 1
            if i >= len(s):
                break
            m = re.match(r'for\s*\(\s*(?:var|int)\s+(\w+)\s*=\s*0\s*;\s*\1\s*<\s*([^;]+?)\s*;\s*\1\+\+\s*\)\s*', s[i:])
            if m:
                j = i + m.end()
                if s[j] == '{':
                    b, end = body(s, j)
                    out.append(('for', m.group(1), m.group(2), Layout.statements(b)))
                    i = end
                else:
                    k = s.index(';', j)
                    out.append(('for', m.group(1), m.group(2), [s[j:k].strip()]))
                    i = k + 1
                continue
            if s[i] == '{':
                b, end = body(s, i)
                out += Layout.statements(b)
                i = end
                continue
            k = s.find(';', i)
            if k < 0:
                k = len(s)
            out.append(' '.join(s[i:k].split()))
            i = k + 1
        return out

    def statement(self, c, props, st, off):
        """(offset after, [entries]) or a problem."""
        if isinstance(st, tuple):
            _, var, bound, stmts = st
            n = None
            bm = re.match(r'^(\w+)\.Length$', bound)
            if re.match(r'^\d+$', bound):
                n = int(bound)
            elif bm:
                n = self.array_len(c, bm.group(1))
            if n is None:
                return 'a loop to %s' % bound
            # X[i] = br.ReadY(); or X[i] = br.ReadEnum<T>(); or X[i].Read(vault, br) after X[i] = new T(...)
            ents = []
            prop = None
            et = None
            for s in stmts:
                m = re.match(r'^(\w+)\[%s\]\s*=\s*(?:\(\w+\)\s*)?br\.(\w+)\(\)$' % var, s)
                m2 = re.match(r'^(\w+)\[%s\]\s*=\s*br\.ReadEnum<([\w\.]+)>\(\)$' % var, s)
                m3 = re.match(r'^(\w+)\[%s\]\.Read\(\w+,\s*br\)$' % var, s)
                m4 = re.match(r'^(\w+)\[%s\]\s*=\s*new\s' % var, s)
                if m and m.group(2) in READS:
                    prop, et = m.group(1), READS[m.group(2)]
                elif m2:
                    prop, et = m2.group(1), 'enum/' + m2.group(2).split('.')[-1]
                elif m3:
                    prop = m3.group(1)
                    et = self.g.ctype(props[prop][0])[:-2] if prop in props else None
                elif m4:
                    continue
                else:
                    return 'a loop doing "%s"' % s
            if prop not in props or et is None:
                return 'a loop over %s, no property' % prop
            size = self.size(et)
            if size is None:
                return 'a loop over %s of %s' % (prop, et)
            ents.append((prop, off, et + '[%d]' % n, n))
            return off + n * size, ents
        m = re.match(r'^(?:(?:var|uint|int|ushort|short|byte|float|long|ulong)\s+)?(\w+)\s*=\s*(?:\(\s*[\w\.]+\s*\)\s*)?br\.(\w+)\(\)$', st)
        if m and m.group(2) in READS:
            n, t = m.group(1), READS[m.group(2)]
            name = n if n in props else self.backed(c, n, props) if n.startswith('_') else None
            if name:
                ct = self.g.ctype(props[name][0])
                return off + PRIMS[t], [(name, off, ct if ct in PRIMS or ct.startswith('enum/') or ct == 'string' else t, 0)]
            return off + PRIMS[t], []
        m = re.match(r'^(\w+)\s*=\s*(?:\(\s*[\w\.]+\s*\)\s*)?br\.ReadEnum<([\w\.]+)>\(\)$', st)
        if m:
            et = 'enum/' + m.group(2).split('.')[-1]
            size = self.size(et)
            if size is None:
                return 'no enum ' + m.group(2)
            n = m.group(1)
            name = n if n in props else self.backed(c, n, props)
            return off + size, ([(name, off, et, 0)] if name else [])
        m = re.match(r'^(\w+)\s*=\s*HashManager\.ResolveVLT\(br\.ReadUInt32\(\)\)$', st)
        if m:
            n = m.group(1)
            name = n if n in props else self.backed(c, n, props)
            return off + 4, ([(name, off, 'string', 0)] if name else [])
        m = re.match(r'^(\w+)\s*=\s*br\.ReadArray\(\s*(?:br\.(\w+)|br\.ReadEnum<([\w\.]+)>|\(\)\s*=>\s*br\.(\w+)\(\))\s*,\s*(\d+)\s*\)$', st)
        if m:
            n = m.group(1)
            et = READS.get(m.group(2) or m.group(4) or '') or ('enum/' + m.group(3).split('.')[-1] if m.group(3) else None)
            size = self.size(et) if et else None
            if size is None:
                return 'an array read "%s"' % st
            cnt = int(m.group(5))
            return off + cnt * size, ([(n, off, et + '[%d]' % cnt, cnt)] if n in props else [])
        m = re.match(r'^(\w+)\.Read\(\w+,\s*br\)$', st)
        if m:
            n = m.group(1)
            if n in props:
                ct = self.g.ctype(props[n][0])
            elif n in c.fields or any(n in x.fields for x in self.g.chain(c)):
                ft = next(x.fields[n] for x in self.g.chain(c) if n in x.fields)
                ct = self.g.ctype(ft)
                name = self.backed(c, n, props)
                size = self.size(ct)
                if size is None:
                    return 'a field %s of %s' % (n, ct)
                if not name:
                    return off + size, []
                # A string kept in a Text, an array behind a property (_xArray: XValues).
                pt = self.g.ctype(props[name][0])
                if pt == 'string':
                    return off + size, [(name, off, 'string', 0)]
                return off + size, [(name, off, 'obj/' + ct[4:] if ct.startswith('obj/') else ct, -1)]
            else:
                return 'reads %s, no property' % n
            size = self.size(ct)
            if size is None:
                return 'a property %s of %s' % (n, ct)
            return off + size, [(n, off, ct, 0)]
        m = re.match(r'^br\.(\w+)\(\)$', st)
        if m and m.group(1) in READS:
            return off + PRIMS[READS[m.group(1)]], []
        m = re.match(r'^br\.AlignReader\((\d+)\)$', st)
        if m:
            a = int(m.group(1))
            return (off + a - 1) // a * a, []
        m = re.match(r'^br\.ReadBytes\((\d+)\)$', st)
        if m:
            return off + int(m.group(1)), []
        if re.match(r'^(?:var\s+)?\w+\s*=\s*new\s', st):
            return off, []
        return 'stops at "%s"' % st


def flatten(game, lay, cls, prefix, base, out, problems, depth=0):
    """Every property down to the primitives: (name, offset, ctype, settable).
    An object is listed, then its parts as name.part; an array's elements as
    name[i]."""
    props, _ = game.g.props(cls)
    for name, off, ct, cnt in lay['entries']:
        full = prefix + name
        settable = props.get(name, ('', False))[1]
        if ct.startswith('obj/') and not ct.endswith(']'):
            sub = ct[4:]
            if cnt == -1:
                # a private object behind the property (_xArray behind XValues): not reached by name
                out.append((full, base + off, 'obj/' + sub + '!', settable))
                continue
            sc = game.g.cls(re.sub(r'<.*$', '', sub))
            out.append((full, base + off, ct, settable))
            if sub.startswith('DynamicSizeArray<') or sub.startswith('VLTPointerContainer<') or sub.startswith('List<'):
                continue
            sl = game.of(sc)
            if sl is None or sl['problem']:
                problems.append('%s: %s' % (full, sl['problem'] if sl else 'no class'))
                continue
            if depth < 8:
                flatten(game, sl, sc, full + '.', base + off, out, problems, depth + 1)
            continue
        am = re.match(r'^(.*)\[(\d+)\]$', ct)
        if am:
            et = am.group(1)
            n = int(am.group(2))
            size = game.size(et)
            out.append((full, base + off, et + '[%d]' % n, settable))
            for i in range(n):
                if et.startswith('obj/'):
                    sc = game.g.cls(et[4:])
                    sl = game.of(sc)
                    out.append(('%s[%d]' % (full, i), base + off + i * size, et, False))
                    if sl and not sl['problem']:
                        flatten(game, sl, sc, '%s[%d].' % (full, i), base + off + i * size, out, problems, depth + 1)
                else:
                    out.append(('%s[%d]' % (full, i), base + off + i * size, et, False))
            continue
        out.append((full, base + off, ct, settable))


class Model:
    def __init__(self, game, sch, used):
        self.g = Game(game)
        self.layout = Layout(self.g)
        self.sch = sch
        self.used = used

    def of(self, c):
        return self.layout.of(c)

    def size(self, ct):
        return self.layout.size(ct)

    def our_path(self, t, off, size):
        if t not in self.sch.structs:
            return None
        p, _ = self.sch.path(t, off, size)
        if p is None:
            return None
        got = self.sch.resolve(t, p)
        if got[0] != off:
            return None
        return p

    def block(self, t):
        c = self.g.registry.get(t)
        if c is None:
            return None, []
        if isinstance(c, Enum):
            lines = ['ogvi %s enum/%s %s' % (t, c.name, c.name), '  # %s' % c.file, 'end']
            return '\n'.join(lines), ['enum/' + c.name]
        kind = self.g.kind(c)
        if c.name == 'Matrix':
            kind = 'matrix'
        lay = self.of(c)
        if c.name == 'Colour':
            # One uint, R its top byte: the parts are bytes of the number (3 is the top).
            kind = 'colour'
            lay = dict(size=4, entries=[('R', 3, 'byte', 0), ('G', 2, 'byte', 0), ('B', 1, 'byte', 0), ('A', 0, 'byte', 0)],
                       problem=None)
        lines = ['ogvi %s %s %s' % (t, kind, c.name), '  # %s' % c.file]
        enums = []
        if lay is None:
            lines.append('  # no layout')
        else:
            out, problems = [], []
            flatten(self, lay, c, '', 0, out, problems)
            if lay['problem']:
                lines.append('  # VaultLib\'s Read method %s: the properties after it have no bytes here' % lay['problem'])
            for p in problems:
                lines.append('  # %s' % p)
            props, order = self.g.props(c)
            listed = {n.split('.')[0].split('[')[0] for n, *_ in out}
            for name, off, ct, settable in out:
                size = self.size(ct) if not ct.startswith('obj/') and not ct.endswith(']') else None
                path = self.our_path(t, off, size) if size and kind != 'colour' else None
                s = '  %s %d :%s' % (name, off, ct)
                if path:
                    s += ' ' + path
                if not settable and '.' not in name and '[' not in name:
                    s += ' ro'
                lines.append(s)
                for e in re.findall(r'enum/(\w+)', ct):
                    enums.append('enum/' + e)
            # Public properties Read doesn't fill: reflection finds them, the file
            # doesn't hold them.
            for n in order:
                if n not in listed:
                    pt, settable = props[n]
                    lines.append('  %s - :%s%s' % (n, self.g.ctype(pt), '' if settable else ' ro'))
        lines.append('end')
        return '\n'.join(lines), enums

    def enum_block(self, name):
        e = self.g.enums.get(name)
        if e is None:
            return None
        lines = ['ogvienum %s %s' % (name, e.base), '  # %s' % e.file]
        if e.problem:
            lines.append('  # %s' % e.problem)
        for n, v in e.values:
            lines.append('  %s %d' % (n, v))
        lines.append('end')
        return '\n'.join(lines)


def blocks(sch, game, used):
    """The table's ogvi blocks for the types a game's files use, then the enums
    they name."""
    m = Model(game, sch, used)
    out = []
    enums = set()
    missing = []
    for t in sorted(used, key=str.lower):
        b, en = m.block(t)
        if b is None:
            missing.append(t)
            continue
        out.append(b)
        enums.update(x[5:] for x in en)
    for e in sorted(enums, key=str.lower):
        b = m.enum_block(e)
        if b:
            out.append(b)
    return out, missing
