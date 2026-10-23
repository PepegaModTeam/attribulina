"""NFS-VltEd's type registry (A/_0007_0014.cs) and the class it leaves in place
for each type and game.

The registry makes six dictionaries, one per game (lines 25-31), and registers
a C# class per AttribSys type name for a list of games (lines 33-1101). A list
is either written out (objArray[0] = MW, ...), the game ALL (expanded at
lines 1202-1216 to a 6-entry list), or a compiled static array whose bytes the
decompilation lacks (`__fieldref (\\u000E\\u000A.\\u000D)`). ARRAYS below says
what each of those holds, and why.
"""

import re

from common import IDENT, REGISTRY, ident_name, read

# The game enum (A/_0001.cs): \u000D is 3, and so on. ALL is 9.
GAME_IDS = {'\\u000D': 'MW', '\\u001D': 'C', '\\u0013': 'PS', '\\u000E': 'UC', '\\u0005': 'W', '\\u000B': 'WB',
            '\\u001F': 'ALL'}
GAME_VALUES = {'MW': 3, 'C': 4, 'PS': 5, 'UC': 6, 'W': 7, 'WB': 8}
GAMES = ['MW', 'C', 'PS', 'UC', 'W', 'WB']

# Each game list of 3 or more entries is a compiled static array: a field of
# A/_000E_000A.cs, named here <field>/<entries> (_000D/5 is the 20-byte \u000D).
# Roslyn names those fields by the SHA-1 of their bytes and emits them sorted by
# name; the obfuscator then renamed them per field type in that order. So the
# declaration order of all of them, whatever their size, is the SHA-1 order. A
# game list holds int32 game ids (MW=3 C=4 PS=5 UC=6 W=7 WB=8), ascending.
#
# Twelve lists are known without the order: nine by their types (a list names
# only games that have its types, and these hold exactly the games that do), and
# three by other code, since Roslyn stores equal bytes once: the 12-byte \u001D
# is also the message box's Abort, Retry, Ignore buttons (A/_0008.cs:96,
# DialogResult 3, 4, 5), \u0005 its Yes, No, Cancel (6, 7, 2), and the 24-byte
# \u0011 the 6-entry list ALL expands to. Their SHA-1s ascend in field order,
# which confirms the order, and the lists between them can only be those whose
# SHA-1 sorts there. scripts/nfsms/arrays.py prints that evidence and checks the
# table below against it.
#
# lists: every list the evidence leaves (with the rule that a list names only
# games that have one of its types). A game in all of them is in the list
# (inferred, or read when other code shows the bytes), a game in some of them
# uncertain, a game in none out.
ARRAYS = {
    '_000D/3': dict(lists=['UC,W,WB'], why='UC,W,WB: only these games have its Sound:: types; fits the SHA-1 order'),
    '_001D/3': dict(lists=['MW,C,PS'], read=True,
                    why="MW,C,PS: the same 12 bytes are the message box's Abort, Retry, Ignore buttons "
                        "(DialogResult 3, 4, 5; A/_0008.cs:96)"),
    '_0013/3': dict(lists=['C,PS,UC'], why='C,PS,UC: only these games have the eRewardCard* types; fits the SHA-1 order'),
    '_000E/3': dict(lists=['C,W,WB'], why='C,W,WB: only these games have its Csis:: types; fits the SHA-1 order'),
    '_001D/4': dict(lists=['MW,C,W,WB'],
                    why='MW,C,W,WB: only these games have Csis::Type_SoundFX_*, and it is the complement of the '
                        'PS,UC TireEffectRecord class; fits the SHA-1 order'),
    '_0013/4': dict(lists=['PS,UC,W,WB'], why='PS,UC,W,WB: only these games have its types; fits the SHA-1 order'),
    '_0005/4': dict(lists=['C,PS,W,WB'], why='C,PS,W,WB: only these games have its types; fits the SHA-1 order'),
    '_0009/5': dict(lists=['MW,C,PS,W,WB'],
                    why='MW,C,PS,W,WB: the complement of the UC-only CollisionReactionRecord class; fits the SHA-1 order'),
    '_000D/5': dict(lists=['C,PS,UC,W,WB'],
                    why='C,PS,UC,W,WB: the complement of the MW-only ControllerDataRecord/JunkmanMod/StringKey '
                        'classes; fits the SHA-1 order'),
    '_001D/5': dict(lists=['MW,C,UC,W,WB'],
                    why='MW,C,UC,W,WB: only these games have Csis::Type_car_type; fits the SHA-1 order'),
    '_0009/3': dict(lists=['PS,W,WB', 'PS,UC,W'],
                    why='its field sorts before _0011/5 (MW,C,PS,UC,WB); of the lists that do, PS,W,WB and '
                        "PS,UC,W name only games that have EngineLFOParams (PS,UC,W alone if _0011/3 is the "
                        "message box's OK, Yes, Retry)"),
    '_0011/5': dict(lists=['MW,C,PS,UC,WB'],
                    why='MW,C,PS,UC,WB: the only 5-game list whose SHA-1 sorts before _000D/3 (UC,W,WB), as '
                        'its field does'),
    '_001A/3': dict(lists=['C,UC,W', 'MW,C,WB', 'MW,UC,W'],
                    why='its field sorts between _0011/5 (MW,C,PS,UC,WB) and _0009/4, where the lists are '
                        'C,UC,W / MW,C,WB / MW,UC,W'),
    '_0009/4': dict(lists=['C,PS,UC,WB'],
                    why='C,PS,UC,WB: its field sorts between _0011/5 (MW,C,PS,UC,WB) and _000D/3 (UC,W,WB), '
                        'where the 4-game lists are C,PS,UC,WB and MW,UC,W,WB, and MW has none of its types'),
    '_001A/4': dict(lists=['MW,C,PS,UC'],
                    why='MW,C,PS,UC: its field and _000D/4 after it sort between _000D/3 (UC,W,WB) and '
                        '_001D/3 (MW,C,PS), where only MW,C,PS,UC and C,UC,W,WB do'),
    '_000D/4': dict(lists=['C,UC,W,WB'],
                    why='C,UC,W,WB: its field and _001A/4 before it sort between _000D/3 (UC,W,WB) and '
                        '_001D/3 (MW,C,PS), where only MW,C,PS,UC and C,UC,W,WB do'),
    '_000E/4': dict(lists=['MW,C,PS,WB'],
                    why='MW,C,PS,WB: the only 4-game list whose SHA-1 sorts between _000D/5 (C,PS,UC,W,WB) '
                        'and _001D/5 (MW,C,UC,W,WB), as its field does'),
    '_001F/3': dict(lists=['C,PS,WB'],
                    why='C,PS,WB: its field and _0002/3 after it sort between _001D/5 (MW,C,UC,W,WB) and '
                        '_0005/4 (C,PS,W,WB), where only C,PS,WB and MW,C,W do'),
    '_0002/3': dict(lists=['MW,C,W'],
                    why='MW,C,W: its field and _001F/3 before it sort between _001D/5 (MW,C,UC,W,WB) and '
                        '_0005/4 (C,PS,W,WB), where only C,PS,WB and MW,C,W do'),
}

COUNT = {1: 'one', 2: 'two', 3: 'three', 4: 'four', 5: 'five'}


def array_status(array, game):
    """('read' | 'inferred' | 'uncertain' | None, reason) for a game and a static array."""
    a = ARRAYS[array]
    lists = [l.split(',') for l in a['lists']]
    n = sum(game in l for l in lists)
    why = a['why']
    if n == 0:
        return None, why
    if n == len(lists):
        if len(lists) > 1:
            why += '; %s with %s' % ('both' if len(lists) == 2 else 'all', game)
        return ('read' if a.get('read') else 'inferred'), why
    return 'uncertain', why + '; %s of them with %s' % (COUNT[n], game)


def parse():
    """Every registration: dict(line, type, cls, games, array, align). games is a
    list of game codes, or None for a static array (then array is its name)."""
    src = read(REGISTRY)
    regs = []
    cur = None
    for i, l in enumerate(src.split('\n'), 1):
        m = re.search(r'\\u0001\[\] (objArray\d+) = \\u0019\\u0016\\u000E\.\\u0011\((\d+)\);', l)
        if m:
            cur = dict(arr=m.group(1), n=int(m.group(2)), elems={}, array=None)
            continue
        m = re.search(r'(objArray\d+)\[(\d+)\] = \\u0001\.(\\u[0-9A-F]{4});', l)
        if m and cur and m.group(1) == cur['arr']:
            cur['elems'][int(m.group(2))] = GAME_IDS[m.group(3)]
            continue
        m = re.search(r'\(\(Array\) (objArray\d+), __fieldref \(\\u000E\\u000A\.(\\u[0-9A-F]{4})\)\)', l)
        if m and cur and m.group(1) == cur['arr']:
            cur['array'] = ident_name(m.group(2)) + '/' + str(cur['n'])
            continue
        m = re.search(r'\\u0007\\u0014\.\\u0011<(' + IDENT + r')>\("([^"]+)", (?:\(byte\) (\d+), )?(objArray\d+)\);', l)
        if m:
            assert cur and cur['arr'] == m.group(4), (i, l)
            games = None if cur['array'] else [cur['elems'][k] for k in sorted(cur['elems'])]
            if games == ['ALL']:
                games = list(GAMES)
                label = 'ALL'
            else:
                label = cur['array'] or ','.join(games)
            regs.append(dict(line=i, type=m.group(2), cls='A/' + ident_name(m.group(1)) + '.cs', games=games,
                             array=cur['array'], label=label, align=m.group(3)))
            cur = None
    return regs


def candidates(regs, game):
    """For one game: type -> [(registration, confidence, reason)], in registry order."""
    out = {}
    for r in regs:
        if r['array']:
            conf, why = array_status(r['array'], game)
            if conf is None:
                continue
            reason = 'game list is a static array (%s); %s' % (r['array'], why)
        elif game in r['games']:
            conf, reason = 'read', 'registered for ' + r['label']
        else:
            continue
        out.setdefault(r['type'], []).append((r, conf, reason))
    return out


def classes_for(regs, game):
    """type -> (registration, confidence, reason, others): the class NFS-VltEd uses
    for the type in that game. A later registration replaces an earlier one in
    the game's dictionary; one that surely holds the game is taken over a static
    array that only may. others: the candidates not taken."""
    out = {}
    for t, cands in candidates(regs, game).items():
        pick = ([c for c in cands if c[1] != 'uncertain'] or cands)[-1]
        out[t] = pick + ([c for c in cands if c is not pick],)
    return out
