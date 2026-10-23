#!/usr/bin/env python3
"""Gives every PC Lua binding its C++ signature from the Xbox 360 milestone
PDB (MSVC-mangled public symbols), matched by name:
  function T.N  -> ?T_N@@YA..., ?N@T@GameHooksCore@@YA..., ?T_N@T@GameHooksCore@@YA...
  method   C:N  -> ?N@C@@[QU][AB]A...
and, when the PC map names the C function itself, that name.
    python3 -I -B signatures.py <bindings_pc.tsv> <milestone_syms.txt> > sigs.tsv
Output adds: milestone symbol, demangled return type, demangled parameters.
"""
import sys, re, collections

BASIC = {'X': 'void', 'D': 'char', 'E': 'unsigned char', 'F': 'short', 'G': 'unsigned short', 'H': 'int',
         'I': 'unsigned int', 'J': 'long', 'K': 'unsigned long', 'M': 'float', 'N': 'double', 'C': 'signed char'}
EXT = {'N': 'bool', 'J': '__int64', 'K': 'unsigned __int64', 'W': 'wchar_t'}


class Demangler:
    def __init__(self, s):
        self.s, self.i, self.backrefs, self.names = s, 0, [], []

    def peek(self): return self.s[self.i] if self.i < len(self.s) else ''

    def take(self, n=1):
        r = self.s[self.i:self.i + n]; self.i += n; return r

    def name(self):
        # a name fragment up to '@', or a template ?$Name@args@, or a name backref digit
        if self.peek().isdigit():
            return self.names[int(self.take())]
        if self.s.startswith('?$', self.i):
            self.i += 2
            base = self.s[self.i:self.s.index('@', self.i)]; self.i += len(base) + 1
            saved = self.backrefs; self.backrefs = []
            args = []
            while self.peek() != '@':
                args.append(self.type())
            self.take(); self.backrefs = saved
            n = '%s<%s>' % (base, ','.join(args))
            self.names.append(n)
            return n
        j = self.s.index('@', self.i)
        n = self.s[self.i:j]; self.i = j + 1
        self.names.append(n)
        return n

    def qualified(self):
        parts = []
        while self.peek() != '@':
            parts.append(self.name())
        self.take()
        return '::'.join(reversed(parts))

    def type(self, param=False):
        start = self.i
        c = self.take()
        if c.isdigit():
            return self.backrefs[int(c)]
        if c in BASIC: t = BASIC[c]
        elif c == '_': t = EXT.get(self.take(), '?')
        elif c in 'PQAB':                     # pointer / reference with cv of pointee
            cv = self.take()
            inner = self.type()
            const = 'const ' if cv in 'BD' else ''
            t = const + inner + (' *' if c in 'PQ' else ' &')
        elif c in 'VUT':
            t = self.qualified()
        elif c == 'W':
            self.take(); t = self.qualified()
        else:
            t = '?' + c
        if param and self.i - start > 1:
            self.backrefs.append(t)
        return t

    def function(self):
        # ?name@scope...@@ <access> ...
        assert self.take() == '?'
        fname = self.name()
        scope = []
        while self.peek() != '@':
            scope.append(self.name())
        self.take()
        acc = self.take()
        if acc == 'Y':           # free function: Y <callconv>
            self.take(); this = None
        else:                    # member: Q/U/A/I/E/M... + this-cv + callconv
            this = self.take(); self.take()
        ret = self.type()
        params = []
        if self.peek() == 'X':
            self.take()
        else:
            while self.peek() not in ('@', 'Z', ''):
                params.append(self.type(param=True))
        return '::'.join(list(reversed(scope)) + [fname]), ret, params, this == 'B'


def demangle(sym):
    try:
        d = Demangler(sym)
        return d.function()
    except Exception as e:
        return None


def main():
    syms = collections.defaultdict(list)
    for line in open(sys.argv[2]):
        a, s = line.rstrip('\n').split(' ', 1)
        m = re.match(r'\?([A-Za-z0-9_]+)@', s)
        if m: syms[m.group(1)].append(s)
    for line in open(sys.argv[1]):
        r = line.rstrip('\n').split('\t')
        kind, tbl, name = r[0], r[1], r[2]
        cands = []
        if kind == 'function':
            for s in syms.get('%s_%s' % (tbl, name), []) + syms.get(name, []):
                if (s.startswith('?%s_%s@@YA' % (tbl, name)) or s.startswith('?%s@%s@GameHooksCore@@YA' % (name, tbl))
                        or s.startswith('?%s_%s@%s@GameHooksCore@@YA' % (tbl, name, tbl))):
                    cands.append(s)
        elif kind == 'method':
            for s in syms.get(name, []):
                if re.match(r'\?%s@%s@@[QU][AB]A' % (re.escape(name), re.escape(tbl)), s):
                    cands.append(s)
        sym = cands[0] if cands else ''
        dm = demangle(sym) if sym else None
        ret, params = (dm[1], ', '.join(dm[2])) if dm else ('', '')
        print('\t'.join(r + [sym, ret, params, str(len(cands))]))


if __name__ == '__main__':
    main()
