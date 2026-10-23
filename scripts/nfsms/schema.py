"""Our type schemas (data/schema/*.txt, format in src/core/Schema.h): the
members a byte range of a value covers, as a member path, and whether a path
resolves in another platform's schema.

A path names members from the type's struct down: Reaction.mClassKey,
ValueMin[1] (an element of an inline array), TitleHash.hash. +N after it says
the bytes start N bytes into that member.
"""

import os
import re

# Members a header names only to pad (Undercover's __align0[3]); a word over one sets
# the member before it.
PADDING = re.compile(r'^__(align|pad)')

PRIM = {'bool': 1, 'bool32': 4, 'int8': 1, 'uint8': 1, 'int16': 2, 'uint16': 2, 'int32': 4, 'uint32': 4,
        'int64': 8, 'uint64': 8, 'float': 4, 'double': 8, 'char': 1, 'cstr': 4, 'ptr': 4}


class Schema:
    def __init__(self, name, game=None, platforms=(), structs=None, enums=None):
        self.name = name            # file name, for messages
        self.game = game
        self.platforms = list(platforms)
        self.structs = structs or {}  # name -> dict(size, members=[dict(name, type, off, n, bits)])
        self.enums = enums or {}      # name -> size

    @staticmethod
    def load(path):
        s = Schema(os.path.basename(path))
        cur = None
        with open(path) as f:
            for line in f:
                line = line.split('#')[0].rstrip()
                if not line.strip():
                    continue
                p = line.split()
                if p[0] == 'schema':
                    s.game = p[1]
                    s.platforms = re.sub(r'"[^"]*"', '', line).split()[2:]
                elif p[0] == 'struct':
                    cur = dict(size=int(p[2]), members=[])
                    s.structs[p[1]] = cur
                elif p[0] == 'enum':
                    s.enums[p[1]] = int(p[2])
                    cur = None
                elif p[0] == 'end':
                    cur = None
                elif cur is not None and line.startswith('  '):
                    m = dict(name=p[0], type=p[1], off=int(p[2]), n=1, bits=None)
                    for x in p[3:]:
                        k, v = x.split('=', 1)
                        if k == 'n':
                            m['n'] = int(v)
                        elif k == 'bits':
                            m['bits'] = v
                    cur['members'].append(m)
        return s

    def size_of(self, t):
        if t in PRIM:
            return PRIM[t]
        if t in self.enums:
            return self.enums[t]
        if t in self.structs:
            return self.structs[t]['size']
        return None

    def leaves(self, sname, base=0, prefix=()):
        """[(offset, size, path components, type)] for every leaf member."""
        out = []
        for m in self.structs[sname]['members']:
            t = m['type']
            esz = self.size_of(t) or 4
            for i in range(m['n']):
                comp = prefix + (m['name'] + ('[%d]' % i if m['n'] > 1 else ''),)
                o = base + m['off'] + i * esz
                if t in self.structs and self.structs[t]['members']:
                    out += self.leaves(t, o, comp)
                else:
                    out.append((o, esz, comp, t))
        return out

    def path(self, sname, off, size):
        """(path, covered leaf names) for the bytes [off, off + size) of a value of
        struct sname, padding members left out. path is None when the leaves have
        no member in common."""
        lv = [x for x in self.leaves(sname) if not PADDING.match(x[2][-1])]
        hit = [x for x in lv if x[0] < off + size and off < x[0] + x[1]]
        if not hit:
            return None, []
        names = ['.'.join(x[2]) for x in hit]
        common = list(hit[0][2])
        for x in hit[1:]:
            k = 0
            while k < len(common) and k < len(x[2]) and common[k] == x[2][k]:
                k += 1
            common = common[:k]
        if not common:
            return None, names
        start = self.offset_of(sname, '.'.join(common))[0]
        p = '.'.join(common)
        if off > start:
            p += '+%d' % (off - start)
        return p, names

    def offset_of(self, sname, path):
        """(offset, size) of the member a path names (without +N), or (None, why)."""
        st = sname
        off = 0
        size = None
        comps = path.split('.')
        for k, c in enumerate(comps):
            m = re.match(r'^(\w+)(?:\[(\d+)\])?$', c)
            if st not in self.structs:
                return None, '%s is not a struct' % '.'.join(comps[:k]) if k else 'no struct ' + sname
            mem = next((x for x in self.structs[st]['members'] if x['name'] == m.group(1)), None)
            if mem is None:
                return None, 'no member %s in %s' % (m.group(1), st)
            esz = self.size_of(mem['type'])
            if m.group(2) is not None:
                i = int(m.group(2))
                if mem['n'] <= i:
                    return None, '%s has %d element%s' % (mem['name'], mem['n'], '' if mem['n'] == 1 else 's')
                off += mem['off'] + i * (esz or 0)
                size = esz
            else:
                if mem['n'] > 1:
                    return None, '%s is an array' % mem['name']
                off += mem['off']
                size = esz
            st = mem['type']
        return off, size

    def resolve(self, sname, path):
        """(offset, size) a path (with +N) leads to here, or (None, why)."""
        base, _, plus = path.partition('+')
        off, size = self.offset_of(sname, base)
        if off is None:
            return None, size
        n = int(plus) if plus else 0
        return off + n, (size - n if size is not None else None)


def from_gen(gen, name):
    """A Schema from scripts/gen_schema.py's Gen after run()."""
    import gen_schema
    s = Schema(name)
    for n, e in gen.enums.items():
        s.enums[gen_schema.sanitize(n)] = e['size']
    for n, st in gen.structs.items():
        s.structs[gen_schema.sanitize(n)] = dict(
            size=st['size'],
            members=[dict(name=m['name'], type=gen_schema.sanitize(m['type']), off=m['off'], n=m['n'],
                          bits=m.get('bits')) for m in st['members']])
    return s
