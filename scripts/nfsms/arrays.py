#!/usr/bin/env python3
"""The evidence for registry.ARRAYS: which games each compiled game list holds.

    python3 scripts/nfsms/arrays.py

A/_000E_000A.cs declares the compiled arrays as fields typed by size. Roslyn
names each by the SHA-1 of its bytes and emits them sorted by name; the
obfuscator then renamed them per type in that order (\\u0011, \\u0009, \\u001A, ...),
so the declaration order is the SHA-1 order across all sizes. A game list is
int32 game ids (MW=3 C=4 PS=5 UC=6 W=7 WB=8). This prints:

- every game list the registry uses, with its types and which games' PC
  databases have them (attrib/databases);
- the anchors: lists the types fix (a list names only games that have one of
  its types, and these hold exactly that many), and arrays other code shows
  (the message box's button lists, the 6-entry ALL); their hashes must ascend;
- for the other lists, every ascending list of games that sorts where its field
  is, alone and together with its neighbours, with and without the rule that a
  list names only games that have one of its types.
"""

import sys

sys.dont_write_bytecode = True

import hashlib  # noqa: E402
import itertools  # noqa: E402
import os  # noqa: E402
import re  # noqa: E402
import struct  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common import CLASSES, REPO, ident_name, read  # noqa: E402
import registry  # noqa: E402
import typetable  # noqa: E402

IDS = registry.GAME_VALUES
NAMES = {v: k for k, v in IDS.items()}

# Fixed lists: by the types (exactly the games that have them), or by other code.
ANCHORS = {
    '_000D/3': ('UC,W,WB', 'only these games have its Sound:: types'),
    '_000E/3': ('C,W,WB', 'only these games have its Csis:: types'),
    '_0013/3': ('C,PS,UC', 'only these games have the eRewardCard* types'),
    '_001D/4': ('MW,C,W,WB', 'only these games have Csis::Type_SoundFX_*'),
    '_0013/4': ('PS,UC,W,WB', 'only these games have CAR_SLOT_ID and the others'),
    '_0005/4': ('C,PS,W,WB', 'only these games have LightStreakSplineRecord and the NIS:: types'),
    '_0009/5': ('MW,C,PS,W,WB', 'only these games have eVOL_MOMENT and STICH_COLLISION_TYPE'),
    '_000D/5': ('C,PS,UC,W,WB', 'only these games have EAIBreakerType and the others'),
    '_001D/5': ('MW,C,UC,W,WB', 'only these games have Csis::Type_car_type'),
}
# Bytes other code shows: (int32 values, what).
SHOWN = {
    '_001D/3': ([3, 4, 5], "the message box's Abort, Retry, Ignore (A/_0008.cs:96)"),
    '_0005/3': ([6, 7, 2], "the message box's Yes, No, Cancel (A/_0008.cs:103)"),
    '_0011/6': ([3, 4, 5, 6, 7, 8], 'ALL (A/_0007_0014.cs:1212-1214)'),
}
SIZES = {}


def sha1(values):
    return hashlib.sha1(b''.join(struct.pack('<i', v) for v in values)).hexdigest().upper()


def fields():
    """[(field name, entries of 4 bytes or None)] in declaration order."""
    src = read(os.path.join(CLASSES, '_000E_000A.cs'))
    sizes = {}
    for m in re.finditer(r'Size = (\d+), Pack = 1\)\]\s*private struct ((?:\\u[0-9A-F]{4})+)', src):
        sizes[m.group(2)] = int(m.group(1))
    out = []
    for m in re.finditer(r'internal static readonly (?:\\u000E\\u000A\.((?:\\u[0-9A-F]{4})+)|int) ((?:\\u[0-9A-F]{4})+);', src):
        size = sizes[m.group(1)] if m.group(1) else 4
        out.append(('%s/%s' % (ident_name(m.group(2)), size // 4 if size % 4 == 0 else '%dB' % size), size))
    return out


def lists(n):
    return [c for c in itertools.combinations(sorted(NAMES), n)]


def name(c):
    return ','.join(NAMES[v] for v in c)


def analyse(regs, have):
    """dict(order, used, known, pos, ascending, any={array: [lists]}, strict={array: [lists]}),
    lists as tuples of game ids, for the game lists no anchor fixes."""
    used = {}
    for r in regs:
        if r['array']:
            used.setdefault(r['array'], []).append(r['type'])
    order = fields()
    known = {}
    for f, (l, why) in ANCHORS.items():
        known[f] = sha1([IDS[g] for g in l.split(',')])
    for f, (v, why) in SHOWN.items():
        known[f] = sha1(v)
    pos = {f: i for i, (f, _) in enumerate(order)}
    seq = sorted(known, key=lambda f: pos[f])
    out = dict(order=order, used=used, known=known, pos=pos, seq=seq,
               ascending=all(known[a] < known[b] for a, b in zip(seq, seq[1:])))
    # the other game lists, in order, between the anchors
    unknown = [f for f, _ in order if f in used and f not in known]
    for strict in (False, True):
        cands = {}
        for f in unknown:
            n = int(f.split('/')[1])
            lo = max([known[k] for k in known if pos[k] < pos[f]], default='')
            hi = min([known[k] for k in known if pos[k] > pos[f]], default='G')
            cands[f] = [c for c in lists(n) if lo < sha1(c) < hi and
                        (not strict or all(any(t in have[NAMES[v]] for t in used[f]) for v in c))]
        # lists between the same two anchors must ascend among themselves too
        found = {}
        group = []
        for f in unknown + [None]:
            if group and (f is None or any(pos[group[-1]] < pos[k] < pos[f] for k in known)):
                ok = {g: set() for g in group}
                for combo in itertools.product(*[cands[g] for g in group]):
                    hs = [sha1(c) for c in combo]
                    if all(x < y for x, y in zip(hs, hs[1:])):
                        for g, c in zip(group, combo):
                            ok[g].add(c)
                for g in group:
                    found[g] = sorted(ok[g], key=sha1)
                group = []
            if f is not None:
                group.append(f)
        out['strict' if strict else 'any'] = found
    return out


def main():
    regs = registry.parse()
    have = {g: set(typetable.game_types(REPO, g)) for g in registry.GAMES}
    a = analyse(regs, have)
    order, used, known, pos = a['order'], a['used'], a['known'], a['pos']
    print('compiled arrays in declaration (SHA-1) order:')
    for i, (f, size) in enumerate(order):
        tag = 'game list: ' + ', '.join(used[f]) if f in used else ''
        print('  %2d %-9s %s' % (i, f, tag))
    print()
    print('game lists and the games that have their types:')
    for f in [f for f, _ in order if f in used]:
        print('  ' + f)
        for t in used[f]:
            print('     %-40s %s' % (t, ','.join(g for g in registry.GAMES if t in have[g])))
    print()
    print('anchors in field order (their hashes must ascend):')
    for f in a['seq']:
        print('  %2d %-8s %s  %s' % (pos[f], f, known[f][:8], ANCHORS.get(f, SHOWN.get(f))[0]))
    print('  ascending:', a['ascending'])
    for key, title in (('any', 'any ascending list'), ('strict', 'naming only games that have one of their types')):
        print()
        print('the other game lists, %s:' % title)
        for f, got in a[key].items():
            print('  %-8s %s' % (f, ' / '.join('%s (%s)' % (name(c), sha1(c)[:6]) for c in got) or 'none'))
    print()
    print('registry.ARRAYS against this:')
    for f, err in check(a):
        print('  %-8s %s' % (f, err))


def check(a):
    """[(array, problem)] where registry.ARRAYS doesn't say what the analysis does."""
    out = []
    for f in a['used']:
        if f not in registry.ARRAYS:
            out.append((f, 'missing'))
            continue
        mine = sorted(registry.ARRAYS[f]['lists'])
        if f in ANCHORS:
            want = [ANCHORS[f][0]]
        elif f in SHOWN:
            want = [name(tuple(sorted(SHOWN[f][0])))]
        else:
            want = sorted(name(c) for c in a['strict'][f])
        if mine != sorted(want):
            out.append((f, 'ARRAYS has %s, the analysis %s' % (' / '.join(mine), ' / '.join(want))))
    if not a['ascending']:
        out.append(('-', 'the anchors are out of order'))
    return out or [('-', 'agrees')]


if __name__ == '__main__':
    main()
