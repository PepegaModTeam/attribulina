#!/usr/bin/env python3
"""Collects what the Pro Street handler scripts use from the game, from the
bytecode listings written by dump_handlers (handlers/<func>.dis).

It follows each register's origin through the code in instruction order
(parameters, globals, table reads, call results, upvalues, arguments passed
to local functions), which is exact for the straight-line shapes the
handlers use. Output, tab separated, one row per use site:
    kind  name  nargs  nresults  args  handler  message  owner
kind: call (global function or table function), method (obj:Name(...)),
read (field read), write (field write), compare.
    python3 -I -B analyze.py <dump dir> > uses.tsv
"""
import re, sys, os, collections
from pathlib import Path

dump = Path(sys.argv[1])
meta = {}
for l in (dump / 'handlers.tsv').read_text().splitlines()[1:]:
    r = l.split('\t')
    meta[r[0]] = r

FUNC = re.compile(r'^function (\S+) \(line \d+\): \d+ instructions, (\d+) params, \d+ registers, (\d+) upvalues')
INS = re.compile(r'^\s+(\d+)\s+([A-Z]+)\s+(-?\d+)(?:\s+(-?\d+))?(?:\s+(-?\d+))?')
CONST = re.compile(r'^\s+(\d+)\s+(.*)$')

def parse(text):
    funcs, cur, mode = {}, None, None
    for line in text.splitlines():
        m = FUNC.match(line)
        if m:
            cur = {'name': m.group(1), 'params': int(m.group(2)), 'nups': int(m.group(3)), 'code': [], 'k': []}
            funcs[cur['name']] = cur; mode = 'code'; continue
        if line.strip() == 'constants:':
            mode = 'k'; continue
        if cur is None: continue
        if mode == 'code':
            m = INS.match(line)
            if m:
                cur['code'].append((m.group(2), int(m.group(3)), int(m.group(4) or 0), int(m.group(5) or 0)))
        elif mode == 'k':
            m = CONST.match(line)
            if m:
                v = m.group(2)
                cur['k'].append(v[1:-1] if v.startswith('"') else v)
    return funcs

rows = []
def emit(*r): rows.append(r)

def kname(f, rk):
    return f['k'][rk - 250] if rk >= 250 else None

def isstr(f, rk):
    if rk < 250: return False
    return True

def lit(f, rk):
    v = f['k'][rk - 250]
    return v

def run(funcs, fname, params, upvals, ctx):
    f = funcs[fname]
    R = {}
    for i, p in enumerate(params): R[i] = p
    def lab(r): return R.get(r, '?')
    def rk(x):
        if x >= 250:
            v = f['k'][x - 250]
            return ('K/' + v) if not re.fullmatch(r'-?[\d.e+-]+|nil', v) else ('N/' + v)
        return lab(x)
    pending = []   # closures: (reg, child name, ups)
    code = f['code']
    children = {}
    i = 0
    child_idx = 0
    while i < len(code):
        op, a, b, c = code[i]
        if op == 'GETGLOBAL':
            R[a] = 'G/' + f['k'][b]
        elif op == 'GETTABLE':
            key = kname(f, c)
            base = lab(b)
            if key is not None and not re.fullmatch(r'-?[\d.e+-]+', key):
                R[a] = base + '.' + key
                emit('read', base + '.' + key, '', '', '', *ctx)
            else:
                R[a] = base + '[]'
        elif op == 'SELF':
            key = kname(f, c)
            R[a + 1] = lab(b)
            R[a] = lab(b) + ':' + (key or '?')
        elif op == 'MOVE':
            R[a] = lab(b)
        elif op == 'LOADK':
            v = f['k'][b]
            R[a] = ('N/' + v) if re.fullmatch(r'-?[\d.e+-]+', v) else ('K/' + v)
        elif op == 'LOADBOOL':
            R[a] = 'B/' + ('true' if b else 'false')
        elif op == 'LOADNIL':
            for r in range(a, b + 1): R[r] = 'nil'
        elif op == 'NEWTABLE':
            R[a] = 'table'
        elif op in ('ADD', 'SUB', 'MUL', 'DIV', 'POW', 'UNM'):
            R[a] = 'num'
        elif op == 'NOT':
            R[a] = 'bool'
        elif op == 'CONCAT':
            R[a] = 'str'
        elif op == 'GETUPVAL':
            R[a] = upvals[b] if b < len(upvals) else '?'
        elif op == 'CLOSURE':
            child = '%s.%d' % (fname, b)
            nups = funcs[child]['nups']
            ups = []
            for j in range(nups):
                uop, ua, ub, uc = code[i + 1 + j]
                ups.append(lab(ub) if uop == 'MOVE' else (upvals[ub] if ub < len(upvals) else '?'))
            R[a] = 'closure:' + child
            children[child] = {'ups': ups, 'calls': []}
            i += nups
        elif op == 'SETTABLE':
            key = kname(f, b)
            base = lab(a)
            if key is not None and not re.fullmatch(r'-?[\d.e+-]+', key):
                emit('write', base + '.' + key, '', '', rk(c), *ctx)
        elif op == 'SETGLOBAL':
            emit('setglobal', f['k'][b], '', '', lab(a), *ctx)
        elif op in ('EQ', 'LT', 'LE'):
            emit('compare', '%s %s %s' % (rk(b), op, rk(c)), '', '', '', *ctx)
        elif op in ('CALL', 'TAILCALL'):
            fn = lab(a)
            nargs = (b - 1) if b else -1
            args = [lab(a + 1 + j) for j in range(nargs)] if nargs >= 0 else ['...']
            nres = (c - 1) if c else -1
            if fn.startswith('closure:'):
                ch = fn[8:]
                if ch in children: children[ch]['calls'].append(args)
                res = 'ret(' + fn + ')'
            else:
                if re.search(r':[A-Za-z_?]+$', fn):
                    kind = 'method'
                    args = args[1:]
                    if nargs > 0: nargs -= 1
                else:
                    kind = 'call'
                emit(kind, fn, str(nargs), str(nres), ' | '.join(args), *ctx)
                res = 'ret(' + fn + ')'
            for j in range(max(nres, 1)):
                R[a + j] = res if j == 0 else res + '#%d' % (j + 1)
        elif op == 'FORPREP' or op == 'FORLOOP':
            R[a + 3] = 'num'
        elif op == 'TFORLOOP':
            for j in range(c): R[a + 2 + j] = 'iter'
        i += 1
    if fname == 'main': return
    for ch, info in children.items():
        nparams = funcs[ch]['params']
        ps = []
        for j in range(nparams):
            seen = sorted({c[j] for c in info['calls'] if j < len(c)})
            ps.append(seen[0] if len(seen) == 1 else ('|'.join(seen) if seen else 'P/%s.%d' % (ch, j)))
        run(funcs, ch, ps, info['ups'], ctx)

for dis in sorted((dump / 'handlers').glob('*.dis')):
    func = dis.stem
    funcs = parse(dis.read_text())
    m = meta[func]
    ctx = (func, m[6], m[4].split('/')[-1] if '/' in m[4] else m[4])
    run(funcs, 'main.0', ['P/self', 'P/message', 'P/vars'], [], ctx)

for r in rows:
    print('\t'.join(r))
