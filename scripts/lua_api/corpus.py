#!/usr/bin/env python3
"""Groups the decompiled handlers by identical body (function name ignored)
and writes one file with each body once, preceded by the handlers using it.
    python3 -I corpus.py <dump dir> <out file>"""
import re, sys, collections
from pathlib import Path

dump = Path(sys.argv[1]); out = Path(sys.argv[2])
rows = [l.split('\t') for l in (dump / 'handlers.tsv').read_text().splitlines()[1:]]
groups = collections.OrderedDict()
for r in sorted(rows, key=lambda r: (r[4].split('/')[0], r[5], r[6], r[1])):
    func = r[0]
    src = (dump / 'handlers' / (func + '.lua')).read_text()
    body = '\n'.join(l for l in src.splitlines() if not l.startswith('-- '))
    body = body.replace(func, 'h_X')
    groups.setdefault(body, []).append(r)
with out.open('w') as f:
    for body, rs in groups.items():
        f.write('=' * 70 + '\n')
        for r in rs:
            f.write(f'-- {r[0]} {r[6]} state={r[5]} owner={r[4]} trans={r[7]}\n')
        f.write(body.rstrip() + '\n')
print(len(groups), 'bodies')
