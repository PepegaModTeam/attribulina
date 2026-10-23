#!/usr/bin/env python3
"""Merges the scans into one model of the Pro Street PC Lua API and writes
lua-api.txt (one entry per line) and api.json (everything, for the doc).

Inputs (all under the luaapi directory, made by the other scan scripts):
  x86/sigs.tsv         bindings found in nfs.exe + milestone C++ symbols
  x86/events_pc.tsv    EventSys event definitions (E* globals)
  x86/messages_pc.tsv  Hermes message definitions (M* globals)
  x86/builders_pc.tsv  how each message becomes the handler's `message` table
  x86/lookup_pc.tsv    the M* send functions and the argument count they want
  dump/uses.tsv        what the 251 handlers use (analyze.py)
  dump/handlers/*.lua  decompiled handlers, for examples
  scan/descriptions.tsv  hand-written one-line descriptions
    python3 -I -B build_api.py <luaapi dir> <repo root>
"""
import collections, json, os, re, sys

base, root = sys.argv[1], sys.argv[2]
P = lambda *a: os.path.join(base, *a)
sys.path.insert(0, os.path.join(root, 'tools', 'pc'))
import pcmap, x86  # noqa: E402

# ---------------------------------------------------------------- bindings
OBJECTS = {}            # global name -> metatable
rows = []
for line in open(P('x86', 'sigs.tsv')):
    r = line.rstrip('\n').split('\t')
    kind, tbl, name, fn, fname, binder, bname, site, sym, ret, params, ncand = (r + [''] * 12)[:12]
    if kind == 'object':
        OBJECTS[name] = tbl
        continue
    rows.append(dict(kind=kind, table=tbl, name=name, fn=int(fn, 16), fname=fname, binder=binder,
                     sym=sym, ret=ret, params=params, ncand=int(ncand or 0)))
# dynamic objects (LuaHubBinder/LuaTierBinder/LuaRaceParametersBinder::DoCustomLookup)
DYNAMIC = {'GHub': 'GHub', 'GTier': 'GTier', 'GRaceParameters': 'GRaceParameters'}

# one signature per binder, from the milestone symbols of the functions it binds
votes = collections.defaultdict(collections.Counter)
for b in rows:
    if b['sym'] and b['ncand'] == 1:
        votes[b['binder']][(b['ret'], b['params'])] += 1
BINDER_SIG = {k: c.most_common(1)[0][0] for k, c in votes.items()}
BINDER_SIG['6f3b60'] = ('raw', 'raw')               # BindRawFunction: a lua_CFunction
BINDER_SIG['6f1d10'] = ('bool', '')                 # ISimable:IsPlayer
BINDER_SIG['6f54a0'] = ('GRacerInfo *', 'ISimable *')  # GRaceStatus:GetRacerInfo(ISimable *)
for b in rows:
    sig = BINDER_SIG.get(b['binder'])
    b['cret'], b['cparams'] = sig if sig else (b['ret'], b['params'])

# stubs: read the first instruction(s) of the bound function
def stub_kind(va):
    ins = x86.disasm(pcmap.read(va, 16), va, count=3)
    t = [i.text for i in ins]
    if t[0].startswith('ret'): return 'does nothing'
    if t[0] == 'xor eax, eax' and t[1].startswith('ret'): return 'always returns 0'
    if t[0] == 'xor al, al' and t[1].startswith('ret'): return 'always returns false'
    if t[0] in ('mov al, 0x1', 'mov eax, 0x1') and t[1].startswith('ret'): return 'always returns true'
    return ''
RAW = {'Table.Randomize': ('void', 'table'), 'Debug.ExecuteDebugScript': ('void', 'table'), 'LuaDebug.ExecuteLine': ('void', 'string')}
for b in rows:
    k = b['table'] + '.' + b['name']
    if k in RAW: b['cret'], b['cparams'] = RAW[k]
for b in rows:
    b['stub'] = stub_kind(b['fn'])
    if b['fname'].startswith('GameHooksCore::Debug::Debug_PrintInstance'):
        b['stub'] = 'does nothing'          # reads a flag and returns; void binding

LUA = {'void': '', 'bool': 'boolean', 'int': 'number', 'unsigned int': 'number', 'float': 'number',
       'double': 'number', 'const char *': 'string', 'GRuntimeInstance *': 'instance',
       'ISimable *': 'simable', 'GRacerInfo *': 'racerinfo', 'GWrapper<unsigned int>': 'handle',
       'raw': '...', 'table': 'table', 'string': 'string'}
def lua_type(c):
    return LUA.get(c.strip(), c.strip())

# ---------------------------------------------------------------- script uses
uses = [l.rstrip('\n').split('\t') for l in open(P('dump', 'uses.tsv'))]
meta = {}
for l in open(P('dump', 'handlers.tsv')).read().splitlines()[1:]:
    r = l.split('\t'); meta[r[0]] = r
srcs = {f[:-4]: open(P('dump', 'handlers', f)).read() for f in os.listdir(P('dump', 'handlers')) if f.endswith('.lua')}

by_key = {}
for b in rows:
    sep = '.' if b['kind'] == 'function' else ':'
    b['key'] = b['table'] + sep + b['name']
    by_key[b['key']] = b

def owner_template(func):
    o = meta[func][4]
    return o.split('/')[-2] if '/' in o else o

def to_key(kind, label):
    """Maps an analyzer label (G/Game.X, ret(G/RaceStatus:GetRacerInfo):Y, ...) to a binding key."""
    m = re.match(r'^G/([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z0-9_]+)$', label)
    if m:
        t, n = m.groups()
        if t in OBJECTS: return OBJECTS[t] + ':' + n         # RaceStatus.DoDDayReset
        return t + '.' + n
    m = re.match(r'^G/([A-Za-z_][A-Za-z0-9_]*):([A-Za-z0-9_]+)$', label)
    if m:
        t, n = m.groups()
        return OBJECTS.get(t, DYNAMIC.get(t, t)) + ':' + n
    m = re.match(r'^G/([A-Za-z_][A-Za-z0-9_]*)$', label)
    if m: return m.group(1)
    m = re.search(r':([A-Za-z0-9_]+)$', label)
    if m:
        n = m.group(1)
        if 'GetRacerInfo' in label: return 'GRacerInfo:' + n
        if n == 'IsPlayer': return 'ISimable:IsPlayer'
        return '?:' + n
    return None

usage = collections.defaultdict(lambda: dict(count=0, handlers=set(), templates=set(), nargs=collections.Counter(),
                                             used_result=0, dot=0, by_nargs=collections.defaultdict(set)))
for u in uses:
    kind, label, nargs, nres = u[0], u[1], u[2], u[3]
    if kind not in ('call', 'method'): continue
    k = to_key(kind, label)
    if not k: continue
    e = usage[k]
    e['count'] += 1; e['handlers'].add(u[5]); e['templates'].add(owner_template(u[5]))
    e['nargs'][nargs] += 1
    e['by_nargs'][nargs].add(u[5])
    if nres not in ('0',): e['used_result'] += 1
    if kind == 'call' and ':' in k: e['dot'] += 1

def example(key):
    if key not in usage: return None
    t, sep, n = (re.split(r'([.:])', key, maxsplit=1) + ['', ''])[:3] if re.search(r'[.:]', key) else (key, '', '')
    if sep == '.':
        pat = re.compile(r'\b%s\.%s\(' % (re.escape(t), re.escape(n)))
    elif sep == ':':
        pat = re.compile(r'[:.]%s\(' % re.escape(n))
    else:
        pat = re.compile(r'(?<![.:\w])%s\(' % re.escape(key))
    u = usage[key]
    common = u['nargs'].most_common(1)[0][0]
    best = None
    for func in sorted(u['by_nargs'][common]) or sorted(u['handlers']):
        for line in srcs[func].splitlines():
            if line.startswith('--'): continue
            if pat.search(line):
                s_ = line.strip()
                if best is None or len(s_) < len(best[0]): best = (s_, func)
                break
    return best

# ---------------------------------------------------------------- events/messages
events = []
for l in open(P('x86', 'events_pc.tsv')):
    name, flags, nf, fields, ok = l.rstrip('\n').split('\t')
    events.append(dict(name=name, flags=int(flags), scriptable=bool(int(flags) & 2),
                       fields=[f.split(':', 1) for f in fields.split()] if fields else []))
msg_defs = {}
for l in open(P('x86', 'messages_pc.tsv')):
    r = l.rstrip('\n').split('\t')
    if r[0].startswith('M'):
        msg_defs[r[0]] = [f.split(':', 1) for f in r[4].split()] if r[4] else []
recv = {}
for l in open(P('x86', 'builders_pc.tsv')):
    r = l.rstrip('\n').split('\t')
    fl = []
    for f in (r[2].split() if len(r) > 2 and r[2] else []):
        n, t = f.split(':', 1); t = t.replace('_or_nil', ' or nil')
        if t not in ('number', 'string', 'boolean', 'nil'):
            t = t.replace(' or nil', '') + ' or nil'
        fl.append([n, t])
    recv[r[0]] = dict(builder=r[1], fields=fl)
send_nargs = {}
for l in open(P('x86', 'lookup_pc.tsv')):
    r = l.rstrip('\n').split('\t')
    send_nargs[r[3]] = None if r[4] == 'None' else int(r[4])
hdr_types = {}
for line in open(os.path.join(root, 'data', 'messages', 'prostreet_pc.txt')):
    if line.startswith('#') or not line.strip(): continue
    p = line.split(); hdr_types[p[0]] = dict(x.split(':', 1) for x in p[1:])
handled = collections.Counter(meta[f][6] for f in meta)

# ---------------------------------------------------------------- field reads
reads = collections.defaultdict(lambda: collections.defaultdict(set))   # root -> field -> templates
for u in uses:
    if u[0] != 'read': continue
    m = re.match(r'^P/(self|message|vars)\.([A-Za-z0-9_]+)$', u[1])
    if m: reads[m.group(1)][m.group(2)].add(owner_template(u[5]) if m.group(1) != 'message' else meta[u[5]][6])
writes = collections.defaultdict(set)
for u in uses:
    if u[0] == 'write':
        m = re.match(r'^P/vars\.([A-Za-z0-9_]+)$', u[1])
        if m: writes[m.group(1)].add(owner_template(u[5]))

# ---------------------------------------------------------------- descriptions
desc = {}
for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'descriptions.tsv')):
    if not l.strip() or l.startswith('#'): continue
    k, d = l.rstrip('\n').split('\t', 1)
    desc[k] = d

pnames = {}
for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'params.tsv')):
    if not l.strip() or l.startswith('#'): continue
    k, d = l.rstrip('\n').split('\t', 1)
    pnames[k] = [x.strip() for x in d.split(',')]

# ---------------------------------------------------------------- model
api = dict(bindings=[], objects=OBJECTS, dynamic=DYNAMIC, events=events, messages=[], unknown_uses=[], reads={}, writes={})
for b in sorted(rows, key=lambda b: (b['table'], b['name'])):
    u = usage.get(b['key'])
    ex = example(b['key']) if u else None
    params = [lua_type(p) for p in b['cparams'].split(', ')] if b['cparams'] and b['cparams'] != 'raw' else []
    api['bindings'].append(dict(
        key=b['key'], kind=b['kind'], table=b['table'], name=b['name'],
        cpp=(b['sym'] if b['ncand'] == 1 else '') or b['fname'], fn='%x' % b['fn'], stub=b['stub'],
        cret=b['cret'], cparams=b['cparams'], ret=lua_type(b['cret']) if b['cret'] != 'raw' else '...',
        params=params if b['cparams'] != 'raw' else ['...'],
        uses=u['count'] if u else 0, handlers=len(u['handlers']) if u else 0,
        templates=sorted(u['templates']) if u else [], nargs=dict(u['nargs']) if u else {},
        dot=u['dot'] if u else 0,
        example=ex[0] if ex else '', example_handler=ex[1] if ex else '',
        pnames=pnames.get(b['key'], []),
        desc=desc.get(b['key'], '')))
for m in sorted(recv):
    api['messages'].append(dict(name=m, fields=recv[m]['fields'], header=hdr_types.get(m, {}),
                                send_nargs=send_nargs.get(m), handled=handled.get(m, 0),
                                sent=usage.get(m, {}).get('count', 0) if m in usage else 0,
                                desc=desc.get(m, '')))
known = set(by_key) | {m for m in recv} | {e['name'] for e in events} | {'ChangeState'}
for k, u in sorted(usage.items()):
    if k not in known:
        api['unknown_uses'].append(dict(key=k, count=u['count'], handlers=sorted(u['handlers']),
                                        templates=sorted(u['templates']), example=(example(k) or ('', ''))[0]))
for root_, fields in reads.items():
    api['reads'][root_] = {f: sorted(t) for f, t in sorted(fields.items())}
api['writes'] = {f: sorted(t) for f, t in sorted(writes.items())}
api['event_uses'] = {e['name']: dict(count=usage[e['name']]['count'], nargs=dict(usage[e['name']]['nargs']),
                                     example=(example(e['name']) or ('', ''))[0])
                     for e in events if e['name'] in usage}
api['message_sends'] = {m: dict(count=usage[m]['count'], nargs=dict(usage[m]['nargs']),
                                example=(example(m) or ('', ''))[0]) for m in recv if m in usage}
api['changestate'] = dict(count=usage['ChangeState']['count'], handlers=len(usage['ChangeState']['handlers']))
json.dump(api, open(P('api.json'), 'w'), indent=1, sort_keys=True)

def sig(params, ret, names=()):
    ps = ['%s: %s' % (names[i], t) if i < len(names) else t for i, t in enumerate(params)]
    return '(%s)%s' % (', '.join(ps), (' -> ' + ret) if ret else '')
HDR_LUA = {'int': 'number', 'unsigned_int': 'number', 'float': 'number', 'bool': 'boolean', 'char*': 'string',
           'HSIMABLE__*': 'simable', 'GCollectionKey': 'number', 'UMath::Vector3': 'nil', 'UMath::Vector4': 'nil',
           'unsigned_long_long': 'number'}
EV_LUA = {'string': 'string', 'hash(string)': 'string', 'hash64(string)': 'string', 'float': 'number', 'double': 'number'}
TABLE_DESC = {
    'Activity': 'start and suspend activities', 'Audio': 'audio game-flow flags', 'Bin': 'career bin queries; stubs on PC',
    'Camera': 'camera control', 'Debug': 'debug output; everything but ShowScreenMessage is a stub on PC',
    'Game': 'most game functions: races, racers, triggers, cops, drift, UI flow', 'HUD': 'HUD messages',
    'Lua': 'Lua runtime control', 'LuaDebug': 'debug console; a stub on PC', 'Math': 'RandomInt', 'MiniMap': 'minimap; a stub on PC',
    'Movie': 'movie playback settings', 'Music': 'music control', 'NIS': 'in-game cinematics (NIS)', 'Platform': 'platform queries',
    'Racer': 'per-racer control by index', 'Render': 'render effects', 'Speech': 'race speech', 'Table': 'Randomize',
    'Track': 'track zone switching'}
lines = []
for t in ['Activity', 'Audio', 'Bin', 'Camera', 'Debug', 'Game', 'HUD', 'Lua', 'LuaDebug', 'Math', 'MiniMap', 'Movie',
          'Music', 'NIS', 'Platform', 'Racer', 'Render', 'Speech', 'Table', 'Track']:
    lines.append('table %s  # %s' % (t, TABLE_DESC.get(t, 'table of game functions')))
for g, mt in sorted(list(OBJECTS.items()) + list(DYNAMIC.items())):
    lines.append('table %s  # %s' % (g, desc.get(g, 'game object (%s userdata)' % mt)))
for b in api['bindings']:
    name = b['key']
    if b['kind'] == 'method':
        # methods are listed under the global that holds the object, plus the type
        holders = [g for g, mt in list(OBJECTS.items()) + list(DYNAMIC.items()) if mt == b['table']]
        names = [h + ':' + b['name'] for h in holders] or [b['table'] + ':' + b['name']]
    else:
        names = [name]
    d = b['desc'] or ''
    if b['stub'] and b['stub'] not in d.lower(): d += ' (%s on PC)' % b['stub']
    for n in names:
        lines.append('%s %s %s  # %s' % ('method' if b['kind'] == 'method' else 'function', n, sig(b['params'], b['ret'], b['pnames']), d))
lines.append('function ChangeState (state) -> nil  # Queues a switch of this activity to the state whose Name is state ("done"); the handler keeps running')
for e in events:
    if not e['scriptable']: continue
    ps = ['%s: %s' % (f[0], EV_LUA.get(f[1], 'number')) for f in e['fields']]
    lines.append('function %s (%s)  # Fires the %s event; needs exactly %d argument%s' % (
        e['name'], ', '.join(ps), e['name'], len(ps), '' if len(ps) == 1 else 's'))
for m in api['messages']:
    TID = {'afee4972': 'number', '1febe3fe': 'number', '8d53823c': 'number', '199f65ef': 'boolean', 'cc99ef72': 'simable',
           '2431b5e7': 'string', '74391cc5': 'ignored', '272cd66d': 'ignored'}
    ps = ['channel: string']
    for i, f in enumerate(msg_defs.get(m['name'], [])):
        n = f[0] if not re.fullmatch(r'[0-9a-f]{8}', f[0]) or i >= len(m['fields']) else m['fields'][i][0]
        ps.append('%s: %s' % (n, TID.get(f[1], 'any')))
    lines.append('function %s (%s)  # Sends the %s message on a channel ("Gameplay", "Adapter"); needs exactly %d argument%s' % (
        m['name'], ', '.join(ps), m['name'], len(ps), '' if len(ps) == 1 else 's'))
CONSTS = [('vars.State', 'GRacerInfo::State', [('kState_Initial', 1), ('kState_NIS', 2), ('kState_PreStaging', 3), ('kState_Staging', 4),
              ('kState_PostStaging', 5), ('kState_Handicap', 6), ('kState_Countdown', 7), ('kState_Racing', 8), ('kState_RaceOver', 9),
              ('kState_Spectate', 10), ('kState_Done', 11)]),
          ('vars.RacerType', 'GRacerInfo::Type', [('kType_Human', 1), ('kType_Artifical', 2)]),
          ('vars.NisType', 'NIS::eNIS_TYPE', [('Unknown', 0), ('Intro', 1), ('Arrest', 2), ('Ending', 3), ('World', 4), ('Moment', 5),
              ('PIP', 6), ('UcapPIP', 7), ('Ucap', 8), ('Countdown', 9), ('Career', 10), ('MomentDynamic', 11), ('FinishCam', 12),
              ('HUB', 13), ('Replay', 14), ('HUB_Challenge', 15), ('HUB_Pinkslip', 16), ('Crowd', 17)]),
          ('vars.Behaviour', 'GRace::Behaviour', [('kRaceBehaviour_Single', 1), ('kRaceBehaviour_Dual', 2)]),
          ('vars.Mode', 'GRace::Mode', [('kRaceMode_Grip', 1), ('kRaceMode_HighSpeedChallenge', 2), ('kRaceMode_Drag', 3), ('kRaceMode_Drift', 4)])]
for tname, enum, vals in CONSTS:
    for n, v in vals:
        lines.append('constant %s.%s = %d  # %s; a table the retail scripts define themselves, not the game' % (tname, n, v, enum))
LUA_OF = {'EA::Reflection::Bool': 'boolean', 'EA::Reflection::Text': 'string', 'EA::Reflection::Char': 'string',
          'Attrib::StringKey': 'string', 'GCollectionKey': 'instance', 'Attrib::RefSpec': 'instance'}
for l in open(P('dump', 'gameplay_fields.tsv')):
    n, t, arr = l.rstrip('\n').split('\t')
    lt = LUA_OF.get(t) or ('number' if re.match(r'EA::Reflection::(U?Int\d+|Float|Double)$', t) else 'nil')
    if arr == '1': lt = 'array of ' + lt
    lines.append('constant self.%s -> %s  # gameplay field (%s%s), readable on any instance; read-only%s' % (
        n, lt, t, ' array' if arr == '1' else '', '' if not lt.endswith('nil') else '; this type has no accessor, so it reads as nil'))
mf = collections.defaultdict(lambda: [set(), set()])
for m in api['messages']:
    for f, t in m['fields']:
        mf[f][0].add(t); mf[f][1].add(m['name'])
for f in sorted(mf):
    ts, ms = mf[f]
    ms = sorted(ms)
    TN = lambda t: t.replace('GRuntimeInstance', 'instance').replace('ISimable', 'simable').replace('GWrapper<T>', 'handle')
    lines.append('constant message.%s -> %s  # field of %s' % (f, ' | '.join(sorted({TN(t) for t in ts})), ', '.join(ms[:6]) + (' and %d more' % (len(ms) - 6) if len(ms) > 6 else '')))
for n in ['print', 'type', 'tostring', 'tonumber', 'pairs', 'ipairs', 'next', 'unpack', 'assert', 'error', 'pcall', 'xpcall',
          'setmetatable', 'getmetatable', 'rawget', 'rawset', 'rawequal', 'loadstring', 'loadfile', 'dofile', 'require',
          'collectgarbage', 'gcinfo', 'getfenv', 'setfenv', 'newproxy', '_G', '_VERSION', 'LUA_PATH',
          'string', 'table', 'math', 'io', 'os', 'coroutine', 'debug']:
    lines.append('stdlib %s  # NOT available: the game opens no Lua standard library, so this global is nil' % n)
for k, d in [('self', 'the activity instance whose handler runs (GRuntimeInstance)'), ('message', 'the message as a table, or nil for MStateEnter/MStateExit'),
             ('vars', 'the activity\'s own table, kept between messages')]:
    lines.append('constant %s  # handler parameter: %s' % (k, d))
open(P('lua-api.txt'), 'w').write('# Pro Street PC gameplay Lua API. <kind> <name> <signature>  # description\n'
    '# Types: number, string, boolean, table, instance (GRuntimeInstance userdata), simable (ISimable userdata),\n'
    '# racerinfo (GRacerInfo userdata), handle (GWrapper userdata). See lua-api.md.\n' + '\n'.join(lines) + '\n')

# ---------------------------------------------------------------- summary
used_keys = set(usage)
print('bindings', len(rows), 'used by scripts', sum(1 for b in rows if b['key'] in used_keys))
print('distinct names used by scripts', len(used_keys))
print('  bindings', sum(1 for k in used_keys if k in by_key), ' events', sum(1 for k in used_keys if k in {e["name"] for e in events}),
      ' messages', sum(1 for k in used_keys if k in recv), ' ChangeState', int('ChangeState' in used_keys),
      ' unknown', len(api['unknown_uses']))
for u in api['unknown_uses']: print('  unknown:', u['key'], u['count'], u['templates'], u['example'])
print('missing descriptions', sum(1 for b in api['bindings'] if not b['desc']))
