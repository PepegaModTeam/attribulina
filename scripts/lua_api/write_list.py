#!/usr/bin/env python3
"""Writes a game's gameplay Lua list (data/lua/<game>_<platform>.txt, in the
format of data/lua/prostreet_pc.txt) from the scans of its executable
(scan_carbon_pc.py, scan_wii.py, scan_mw.py, scan_uc360.py), reusing Pro
Street's list where a name is the same.

    python3 -I -B write_list.py <out.txt> --game NAME --platform WORD --exe PATH
        --bindings B.tsv [--types-from B2.tsv --types-from-name TEXT] --events E.tsv
        [--messages M.tsv --channels "A,B"] [--constants C.tsv] --prostreet data/lua/prostreet_pc.txt
        [--binders TEXT] [--events-text TEXT] [--note TEXT ...] [--shared-functions]
        [--report REPORT.txt]

--types-from: another build's bindings.tsv whose types fill in the bindings
  the main scan has none for (a PC binder with no map name of its own);
  --types-from-name says which build that is, for the header.
--channels: the channel names the game's retail handlers pass to M<Name>.
--constants: numbers the game sets in a table at startup (scan_uc360.py's
  BindEnums: GRaceType.*): table, name, value, the enum they come from.
--binders, --events-text: what the header says binds the functions and which
  events the E<Name> globals fire (the defaults are Carbon's and Undercover's).
--note: a paragraph added to the header, after the one on --types-from.
--shared-functions: a function bound under two names says so ("Bound to the
  same function as Game.KnockoutRacer on PC.").
Rules:
- A function, method or table that Pro Street's list has under the same kind
  and name keeps Pro Street's signature and description when Pro Street's
  parameter and return types are the ones this game's binder has, less the
  notes about Pro Street's PC build, its retail scripts and the names its code
  uses. Otherwise the signature is written from this game's binder (types
  only) and the description left out. A name Pro Street doesn't have gets a signature from
  its binder and no description, unless NOTES below has one (read from the
  code). A table of functions keeps Pro Street's description only when it
  binds a function Pro Street's table of that name has.
- A bound C function that does nothing or returns a constant gets that as the
  last sentence of its description ("Does nothing on PC."). A tenth column in
  the bindings, a note, follows it (scan_uc360.py: how another build differs).
- A global object keeps Pro Street's description with this game's metatable
  in it, and with "looked up on every read" or "set at startup" as this
  game's binder says (an object whose binder column names a DoCustomLookup is
  looked up on every read).
- E<Name>: the events with flag bit 1 and a fire function, their arguments
  from the event's fields, written the way build_api.py writes Pro Street's.
- M<Name>: the messages the global lookup sends, their arguments from how the
  sender reads each one (scan_wii.py); one it never reads is "ignored".
"""
import argparse, collections, re, textwrap

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--game', required=True)
ap.add_argument('--platform', required=True)
ap.add_argument('--exe', required=True)
ap.add_argument('--bindings', required=True)
ap.add_argument('--types-from')
ap.add_argument('--types-from-name', default='another build')
ap.add_argument('--events', required=True)
ap.add_argument('--messages')
ap.add_argument('--channels', default='')
ap.add_argument('--constants')
ap.add_argument('--prostreet', required=True)
ap.add_argument('--binders', default='LuaBindery::BindToGameCode and BindCustomMethods bind')
ap.add_argument('--events-text', default='events marked callable from Lua')
ap.add_argument('--note', action='append', default=[])
ap.add_argument('--shared-functions', action='store_true')
ap.add_argument('--report')
A = ap.parse_args()
PLAT = A.platform

LUA = {'void': '', 'bool': 'boolean', 'int': 'number', 'unsigned int': 'number', 'float': 'number',
       'double': 'number', 'const char *': 'string', 'GRuntimeInstance *': 'instance', 'ISimable *': 'simable',
       'GRacerInfo *': 'racerinfo', 'raw': '...'}
NUMBERS = {'short', 'unsigned short', 'char', 'unsigned char', 'long', 'unsigned long', 'long long', 'unsigned long long'}
EV_LUA = {'string': 'string', 'hash(string)': 'string', 'hash64(string)': 'string', 'float': 'number', 'double': 'number',
          'boolean': 'boolean'}
SIMABLE, COLLECTION_KEY = 'cc99ef72', '74391cc5'          # Hermes field type ids (HSIMABLE__*, GCollectionKey)
STDLIB = ['print', 'type', 'tostring', 'tonumber', 'pairs', 'ipairs', 'next', 'unpack', 'assert', 'error', 'pcall',
          'xpcall', 'setmetatable', 'getmetatable', 'rawget', 'rawset', 'rawequal', 'loadstring', 'loadfile', 'dofile',
          'require', 'collectgarbage', 'gcinfo', 'getfenv', 'setfenv', 'newproxy', '_G', '_VERSION', 'LUA_PATH',
          'string', 'table', 'math', 'io', 'os', 'coroutine', 'debug']
# Names Pro Street doesn't bind, described from their code: (signature, description).
NOTES = {
    'SPrintf': ('(key: string, ...) -> string', 'Returns the localized string named key (FEHashUpper, GetLocalizedString) '
                'formatted with up to five more arguments, at most 127 characters.'),
    'GetMetatable': ('(any) -> table', 'Returns the metatable of its last argument (lua_getmetatable).'),
    '_ALERT': ('(message: string)', 'Prints "LUA ALERT: " and the message to the debug output '
               '(GameCore::Lua::Runtime::Alert, set by Runtime::CreateState).'),
}


def tsv(p):
    return [l.rstrip('\n').split('\t') for l in open(p) if l.strip()] if p else []


def lua_type(c):
    c = c.strip()
    if c in LUA: return LUA[c]
    if c in NUMBERS or c.startswith('enum '): return 'number'
    if c.startswith('GWrapper<'): return 'handle'
    return 'userdata' if c.endswith('*') else c


# ---------------------------------------------------------------- Pro Street's list
PS = {}
for l in open(A.prostreet):
    if l.startswith('#') or not l.strip(): continue
    head, _, desc = l.rstrip('\n').partition('  # ')
    kind, name = head.split(' ', 2)[:2]
    PS[(kind, name)] = (head[len(kind) + len(name) + 2:].strip(), desc)


def sig_types(sig):
    """'(a: number, b) -> boolean' -> (['number', 'b'], 'boolean')."""
    m = re.match(r'^\((.*?)\)(?:\s*->\s*(.*))?$', sig)
    if not m: return None
    ps = [p.split(': ')[-1].strip() for p in m.group(1).split(',')] if m.group(1).strip() else []
    return ps, (m.group(2) or '').strip()


NOTE = re.compile(r'\b(on PC|retail|stubs?)\b')
# sentences about Pro Street's own scripts and the names its code uses
PS_ONLY = re.compile(r'^(Scripts\b|The \w+ state\b|Used before|Names:|The function references)')


def strip_notes(desc):
    """Pro Street's notes about its PC build, its retail scripts and its code's names, out of a description."""
    d = re.sub(r'\s*\((?:[^()]|\([^()]*\))*\b(?:retail|on PC)\b(?:[^()]|\([^()]*\))*\)', '', desc)
    if '.' not in d:                                       # a table's "what; note"
        return '; '.join(p for p in d.split('; ') if not NOTE.search(p))
    parts = re.split(r'(?<=\.)\s+(?=[A-Z(])', d)
    return ' '.join(p for p in parts if not NOTE.search(p) and not PS_ONLY.match(p)).strip()


def stub_sentence(stub, ret):
    if not stub: return ''
    m = re.match(r'always returns (\d+)', stub)
    if m:
        v = m.group(1)
        if ret == 'bool': v = 'false' if v == '0' else 'true'
        return 'Always returns %s on %s.' % (v, PLAT)
    return '%s on %s.' % (stub[0].upper() + stub[1:], PLAT)


report = collections.defaultdict(list)
used_types = set()


def note_types(sig):
    used_types.update(re.findall(r'\b(table|instance|simable|racerinfo|handle|userdata|any|ignored)\b', sig))
    if '...' in sig: used_types.add('...')


# ---------------------------------------------------------------- bindings
rows = tsv(A.bindings)
other = {tuple(r[:3]): r for r in tsv(A.types_from)}
objects = {r[2]: r[1] for r in rows if r[0] == 'object'}          # global -> metatable
looked_up = {r[2] for r in rows if r[0] == 'object' and 'DoCustomLookup' in r[5]}
bound = collections.defaultdict(list)                            # C function -> names, for --shared-functions
for r in rows:
    if r[0] == 'function' and r[3] != '-': bound[r[3]].append(r[1] + '.' + r[2] if r[1] else r[2])
lines = []
for r in sorted((r for r in rows if r[0] != 'object'), key=lambda r: (r[1], r[2])):
    kind, tbl, name, fn, fsym, binder, ret, params, stub, note = (r + [''] * 10)[:10]
    if not ret and tuple(r[:3]) in other:
        o = other[tuple(r[:3])]
        ret, params = o[6], o[7]
        report['types from %s' % A.types_from_name].append('%s %s.%s' % (kind, tbl, name))
    if ret:
        ptypes = ['...'] if params == 'raw' else [lua_type(p) for p in params.split(', ')] if params else []
        rtype = lua_type(ret) if ret != 'raw' else ''
        mine = '(%s)%s' % (', '.join(ptypes), (' -> ' + rtype) if rtype else '')
    else:
        ptypes, rtype, mine = None, None, '(...)'
        report['no types'].append('%s %s.%s' % (kind, tbl, name))
    if kind == 'method':
        names = ['%s:%s' % (h, name) for h in sorted(g for g, mt in objects.items() if mt == tbl)] or [tbl + ':' + name]
    else:
        names = [tbl + '.' + name if tbl else name]
    for n in names:
        sig, desc = mine, ''
        ps = PS.get((kind, n))
        if ps:
            st = sig_types(ps[0])
            if ptypes is not None and st is not None and (ret == 'raw' or (st[0] == ptypes and st[1] == rtype)):
                sig, desc = ps[0], strip_notes(ps[1])
                report['reused from Pro Street'].append(n)
                if desc != ps[1]: report['notes dropped'].append('%s: %s -> %s' % (n, ps[1], desc))
            else:
                report['types differ from Pro Street'].append('%s: Pro Street %s, here %s' % (n, ps[0], mine))
        elif n in NOTES:
            sig, desc = NOTES[n]
            report['written from the code'].append(n)
        else:
            report['only here, no description'].append(n)
        s = stub_sentence(stub, ret)
        if s and s.lower()[:-1] not in desc.lower():
            desc = (desc + ' ' + s).strip()
            report['stub notes'].append('%s: %s' % (n, s))
        same = [o for o in bound.get(fn, []) if o != n] if A.shared_functions and kind == 'function' else []
        if same:
            desc = (desc + ' Bound to the same function as %s on %s.' % (' and '.join(same), PLAT)).strip()
            report['bound to the same function'].append('%s: %s' % (n, ', '.join(same)))
        if note:
            desc = (desc + ' ' + note).strip()
            report['notes'].append('%s: %s' % (n, note))
        note_types(sig)
        lines.append('%s %s %s%s' % (kind, n, sig, ('  # ' + desc) if desc else ''))

# tables: the function tables, then the objects
consts = tsv(A.constants)
tlines = []
for t in sorted({r[1] for r in rows if r[0] == 'function' and r[1]} | {c[0] for c in consts}) + sorted(objects):
    ps = PS.get(('table', t))
    if ps and t not in objects and not any(r[0] == 'function' and r[1] == t and ('function', t + '.' + r[2]) in PS
                                           for r in rows):
        report['tables whose functions Pro Street\'s table of that name doesn\'t have'].append('table ' + t)
        ps = None
    d = strip_notes(ps[1]) if ps else ('game object (%s userdata)' % objects[t] if t in objects else '')
    if t in objects and ps:
        d = re.sub(r'\(\w+ userdata\)', '(%s userdata)' % objects[t], d)
        if t in looked_up and 'set at startup' in d:
            d = d.replace('set at startup', 'looked up on every read; nil when there is none')
        elif t not in looked_up and 'looked up on every read' in d:
            d = re.sub(r'looked up on every read(; [^;]*)?', 'set at startup', d)
        if d != strip_notes(ps[1]): report['object descriptions changed'].append('%s: %s' % (t, d))
    elif t in objects and t in looked_up:
        d += ', looked up on every read; nil when there is none'
    if not ps and any(c[0] == t for c in consts):
        e = next((c[3] for c in consts if c[0] == t and len(c) > 3 and c[3]), '')
        d = 'numbers%s, set at startup' % (' (%s)' % e if e else '')
    report['reused from Pro Street' if ps else 'tables only here'].append('table ' + t)
    tlines.append('table %s%s' % (t, ('  # ' + d) if d else ''))
for c in consts:
    e = c[3] if len(c) > 3 and c[3] else ''
    lines.append('constant %s.%s = %s  # %sset at startup' % (c[0], c[1], c[2], ('%s; ' % e) if e else ''))
    report['numbers'].append('%s.%s' % (c[0], c[1]))
lines = tlines + lines

# ChangeState: set before every delivery (GActivity::BuildActivityTables)
lines.append('function ChangeState %s  # %s' % PS[('function', 'ChangeState')])
report['reused from Pro Street'].append('ChangeState')

# ---------------------------------------------------------------- events
for r in tsv(A.events):
    name, flags, nf, fields, size, fire = r
    if not (int(flags) & 2) or fire != 'fire': continue
    fl = [f.split(':', 1) for f in fields.split()] if fields else []
    ps = ['%s%s' % ('' if re.fullmatch(r'[0-9a-f]{8}', f[0]) else f[0] + ': ', EV_LUA.get(f[1], 'number')) for f in fl]
    line = 'function %s (%s)  # Fires the %s event; needs exactly %d argument%s' % (
        name, ', '.join(ps), name, len(ps), '' if len(ps) == 1 else 's')
    old = PS.get(('function', name))
    if old and 'function %s %s  # %s' % (name, old[0], old[1]) == line:
        report['events as in Pro Street'].append(name)
    elif old:
        report['events unlike Pro Street'].append('%s: Pro Street %s, here (%s)' % (name, old[0], ', '.join(ps)))
    else:
        report['events only here'].append(name)
    lines.append(line)

# ---------------------------------------------------------------- messages
chan = ' (%s)' % ', '.join('"%s"' % c for c in A.channels.split(',')) if A.channels else ''
for r in tsv(A.messages):
    name, nargs, fields, reads = (r + [''] * 4)[:4]
    fl = [f.split(':', 1) for f in fields.split()] if fields else []
    rd = reads.split(',') if reads else []
    ps = ['channel: string']
    for i, (fname, tid) in enumerate(fl):
        how = rd[i + 1] if i + 1 < len(rd) else '-'
        t = {'-': 'ignored', 'userdata': {SIMABLE: 'simable', COLLECTION_KEY: 'instance'}.get(tid, 'userdata')}.get(how, how)
        ps.append('%s%s' % ('' if re.fullmatch(r'[0-9a-f]{8}', fname) else fname + ': ', t))
    if nargs == 'None' or int(nargs) != len(ps):
        report['messages whose argument check differs from their fields'].append('%s: %s vs %d' % (name, nargs, len(ps)))
    n = int(nargs) if nargs != 'None' else len(ps)
    line = 'function %s (%s)  # Sends the %s message on a channel%s; needs exactly %d argument%s' % (
        name, ', '.join(ps), name, chan, n, '' if n == 1 else 's')
    old = PS.get(('function', name))
    if old and old[0] == '(%s)' % ', '.join(ps):
        report['messages with Pro Street\'s arguments'].append(name)
    elif old:
        report['messages unlike Pro Street'].append('%s: Pro Street %s, here (%s)' % (name, old[0], ', '.join(ps)))
    else:
        report['messages only here'].append(name)
    note_types(line.split('  # ')[0])
    lines.append(line)

for n in STDLIB:
    lines.append('stdlib %s  # NOT available: the game opens no Lua standard library, so this global is nil' % n)

# ---------------------------------------------------------------- header
TYPE_WORDS = [('table', 'table'), ('instance', 'instance (GRuntimeInstance userdata)'),
              ('simable', 'simable (ISimable userdata)'), ('racerinfo', 'racerinfo (GRacerInfo userdata)'),
              ('handle', 'handle (GWrapper userdata)'), ('userdata', 'userdata (another game object)'),
              ('any', 'any (a value of any type)'), ('ignored', 'ignored (a message argument the game never reads)')]
words = ['number', 'string', 'boolean'] + [w for t, w in TYPE_WORDS if t in used_types]
has_m = bool(tsv(A.messages))
bare = [r[2] for r in rows if r[0] == 'function' and not r[1]]
paras = ['%s %s gameplay Lua API. <kind> <name> <signature>  # description' % (A.game, PLAT),
         'Made by scripts/lua_api/run_others.sh from %s: the functions, methods and objects that '
         '%s%s, %s%sthe E<Name> globals (%s) and ChangeState. The game opens no Lua standard library.' % (
             A.exe, A.binders, ' (%s as %s)' % (', '.join(bare), 'a bare global' if len(bare) == 1 else 'bare globals')
             if bare else '', 'the numbers the game sets in %s, ' % ' and '.join(sorted({c[0] for c in consts})) if consts else '',
             'the M<Name> globals (messages the global lookup sends), ' if has_m else '', A.events_text)]
if not has_m:
    paras.append('There are no M<Name> globals: the game\'s global lookup only answers names that start with E.')
if report.get('types from %s' % A.types_from_name):
    paras.append('The types of the %d bindings whose PC binder has no map name come from %s, which binds the same '
                 'names.' % (len(report['types from %s' % A.types_from_name]), A.types_from_name))
paras += A.note
paras += ['Signatures and descriptions of names Pro Street also binds come from data/lua/prostreet_pc.txt and were '
          'not checked against this game; the stub notes ("Does nothing on %s.") were. The E<Name>%s lines are '
          'written from this game\'s own definitions.' % (PLAT, ' and M<Name>' if has_m else ''),
          'Types: %s.%s' % (', '.join(words), ' "..." takes any arguments.' if '...' in used_types else '')]
hdr = []
for p_ in paras:
    hdr += ['# ' + l for l in textwrap.wrap(p_, 108, break_on_hyphens=False)]
with open(A.out, 'w') as f:
    f.write('\n'.join(hdr + lines) + '\n')

counts = collections.Counter(l.split(' ', 1)[0] for l in lines)
summary = ['%s: %s' % (A.out, ', '.join('%d %s' % (counts[k], k) for k in sorted(counts)))]
summary += ['%s: %d' % (k, len(report[k])) for k in sorted(report)]
if A.report:
    with open(A.report, 'w') as f:
        f.write('\n'.join(summary) + '\n\n')
        for k in sorted(report):
            f.write('## %s (%d)\n%s\n\n' % (k, len(report[k]), '\n'.join(report[k])))
print('\n'.join(summary))
