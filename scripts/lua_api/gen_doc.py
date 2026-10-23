#!/usr/bin/env python3
"""Writes lua-api.md from lua-api.template.md (beside this script) and api.json.
    python3 -I -B gen_doc.py <luaapi dir>
"""
import collections, json, os, re, sys

base = sys.argv[1]
P = lambda *a: os.path.join(base, *a)
api = json.load(open(P('api.json')))
tmpl = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lua-api.template.md')).read()

def cell(s):
    return s.replace('|', '\\|')

def code(s):
    return '`%s`' % s.replace('`', "'") if s else ''

def sig(b, holder=None):
    names = b['pnames']
    ps = ['%s: %s' % (names[i], t) if i < len(names) else t for i, t in enumerate(b['params'])]
    head = holder or b['key']
    return '%s(%s)%s' % (head, ', '.join(ps), (' -> ' + b['ret']) if b['ret'] else '')

def plural(n, w):
    return '%d %s%s' % (n, w, '' if n == 1 else 's')

def retail(b):
    if not b['uses']: return 'none'
    t = ', '.join(b['templates'][:4]) + (', ...' if len(b['templates']) > 4 else '')
    s = '%s in %s (%s)' % (plural(b['uses'], 'call'), plural(b['handlers'], 'handler'), t)
    if b['example']: s += '. ' + code(b['example'])
    return s

def what(b):
    d = b['desc']
    if b['stub'] and b['stub'].lower() not in d.lower():
        d += ' This build: %s.' % b['stub']
    return d

# ---------------------------------------------------------------- summary
B = api['bindings']
used = [b for b in B if b['uses']]
ev_names = {e['name'] for e in api['events']}
n_unknown = len(api['unknown_uses'])
n_ev = len(api['event_uses'])
n_msg = len(api['message_sends'])
total = len(used) + n_ev + n_msg + 1 + n_unknown
stubs_used = [b for b in used if b['stub']]
summary = f"""## In numbers

The 251 retail handlers use {total} distinct global names, functions and methods:

- {len(used)} of the game's {len(B)} bound functions and methods. Every one is tied to its C function in `nfs.exe`, with an address and a signature taken from its binder. {len(stubs_used)} of them are stubs in the PC build.
- {n_ev} events through `E` globals and {n_msg} messages through `M` globals.
- `ChangeState`.
- {n_unknown} names that the PC build does not have. See [Names the retail scripts use that do not exist on PC](#names-the-retail-scripts-use-that-do-not-exist-on-pc).

They also read {len(api['reads']['self'])} fields of `self`, {len(api['reads']['message'])} fields of `message` and {len(api['reads']['vars'])} fields of `vars`, and write {len(api['writes'])} fields of `vars`. `vars` belongs to the scripts, so its fields are listed only where they hold constants.

`data/lua/prostreet_pc.txt` has every name in one line each. The Sequence Builder's Lua editor colours them and shows the line on hover.
"""

cs = api['changestate']
changestate = '%s in %s' % (plural(cs['count'], 'time'), plural(cs['handlers'], 'handler'))

# ---------------------------------------------------------------- self fields
fields = {}
for l in open(P('dump', 'gameplay_fields.tsv')):
    n, t, a = l.rstrip('\n').split('\t')
    fields[n] = (t, a == '1')
LUA_OF = {'EA::Reflection::Bool': 'boolean', 'EA::Reflection::Text': 'string', 'EA::Reflection::Char': 'string',
          'Attrib::StringKey': 'string', 'GCollectionKey': 'instance or nil', 'Attrib::RefSpec': 'instance or nil'}
def lua_of(t):
    if t in LUA_OF: return LUA_OF[t]
    if re.match(r'EA::Reflection::(U?Int\d+|Float|Double)$', t): return 'number'
    return 'nil (no accessor)'
uses = [l.rstrip('\n').split('\t') for l in open(P('dump', 'uses.tsv'))]
meta = {}
for l in open(P('dump', 'handlers.tsv')).read().splitlines()[1:]:
    r = l.split('\t'); meta[r[0]] = r
def tmpl_of(func):
    o = meta[func][4]
    return o.split('/')[-2] if '/' in o else o
read_by = collections.defaultdict(set)     # field -> templates
on_self = collections.defaultdict(bool)
for u in uses:
    if u[0] != 'read': continue
    lab = u[1]
    m = re.match(r'^(.*)\.([A-Za-z0-9_]+)$', lab)
    if not m: continue
    basel, f = m.groups()
    if basel in ('P/vars', 'P/message') or basel.startswith('P/vars') or basel.startswith('G/'): continue
    if f not in fields: continue
    read_by[f].add(tmpl_of(u[5]))
    if basel == 'P/self': on_self[f] = True
rows = []
for f in sorted(read_by, key=str.lower):
    t, arr = fields[f]
    lv = lua_of(t)
    if arr: lv = 'array of ' + ('instances or nil' if 'instance' in lv else lv + 's')
    rows.append('| `%s` | `%s`%s | %s | %s |' % (f, t, ' array' if arr else '', lv, ', '.join(sorted(read_by[f]))))
self_fields = """These are the instance fields the retail handlers read, on `self` or on other elements (`message.Sender.CashReward`, `self.TrackLayout.racestart`). The type is the field's type in the `gameplay` class.

| Field | AttribSys type | Lua value | Read by |
|---|---|---|---|
""" + '\n'.join(rows) + """

`p2p` also reads `sender.SectionIdex`, which is not a field of the class, so it is always nil. The field is `SectionIndex`."""
# check the claim above
assert 'SectionIdex' not in fields and 'SectionIndex' in fields

# ---------------------------------------------------------------- messages
msgs = {m['name']: m for m in api['messages']}
mreads = collections.defaultdict(set)
for u in uses:
    if u[0] == 'read':
        m = re.match(r'^P/message\.([A-Za-z0-9_]+)$', u[1])
        if m: mreads[meta[u[5]][6]].add(m.group(1))
def recv_str(m):
    if m['name'] in ('MStateEnter', 'MStateExit'):
        return 'nil from state changes'
    if not m['fields']: return '`{}`'
    return ', '.join('`%s` %s' % (f, t) for f, t in m['fields'])
handled_rows = []
for m in sorted(api['messages'], key=lambda m: (-m['handled'], m['name'])):
    if not m['handled']: continue
    rd = ', '.join(sorted(mreads.get(m['name'], []))) or 'none'
    handled_rows.append('| `%s` | %d | %s | %s |' % (m['name'], m['handled'], cell(recv_str(m)), rd))
messages_handled = """| Message | Handlers | Fields | Read by retail |
|---|---|---|---|
""" + '\n'.join(handled_rows)

sent_rows = []
for name, s in sorted(api['message_sends'].items()):
    sent_rows.append('| `%s` | %d | %s |' % (name, s['count'], code(cell(s['example']))))
messages_sent = """| Message | Calls | Example |
|---|---|---|
""" + '\n'.join(sent_rows)

TID_SEND = {'afee4972': 'number', '1febe3fe': 'number', '8d53823c': 'number', '6ce5937c': 'not checked',
            '199f65ef': 'boolean', 'cc99ef72': 'simable', '2431b5e7': 'string',
            '74391cc5': 'always 0', '272cd66d': 'always zero', 'e467365e': 'not usable'}
def send_args(name):
    fs = []
    recvd = msgs[name]['fields']
    for i, (f, tid) in enumerate(msg_order.get(name, [])):
        if re.fullmatch(r'[0-9a-f]{8}', f) and i < len(recvd): f = recvd[i][0]
        fs.append('%s: %s' % (f, TID_SEND.get(tid, 'not checked')))
    return '(' + ', '.join(['channel'] + fs) + ')'
msg_order = {}
for l in open(P('x86', 'messages_pc.tsv')):
    r = l.rstrip('\n').split('\t')
    if r[0].startswith('M'):
        msg_order[r[0]] = [tuple(f.split(':')) for f in r[4].split()] if r[4] else []
all_msgs = ['| Message | Received as | Send with | Retail |', '|---|---|---|---|']
for m in api['messages']:
    all_msgs.append('| `%s` | %s | `%s%s` | %s |' % (m['name'], cell(recv_str(m)), m['name'], cell(send_args(m['name'])),
                                                   m['handled'] or ''))

# ---------------------------------------------------------------- events
evs = {e['name']: e for e in api['events']}
EVT = {'hash(string)': 'hash', 'hash64(string)': 'hash64', 'uint': 'uint'}
def ev_args(e):
    return '(' + ', '.join('%s: %s' % (f, EVT.get(t, t)) for f, t in e['fields']) + ')'
ev_rows = []
for name in sorted(set(api['event_uses']) | {u['key'] for u in api['unknown_uses'] if u['key'].startswith('E')}):
    if name in evs:
        e = evs[name]
        ex = api['event_uses'][name]['example']
        ok = 'yes' if e['scriptable'] else 'no, not callable from Lua'
        ev_rows.append('| `%s%s` | %s | %s |' % (name, ev_args(e), ok, code(cell(ex))))
    else:
        u = [u for u in api['unknown_uses'] if u['key'] == name][0]
        ev_rows.append('| `%s` | no, not registered: the call does nothing | %s |' % (name, code(cell(u['example']))))
events_used = """| Event | Works on PC | Example |
|---|---|---|
""" + '\n'.join(ev_rows)
all_ev = ['| Event | Arguments |', '|---|---|']
for e in sorted(api['events'], key=lambda e: e['name']):
    if e['scriptable']:
        all_ev.append('| `%s` | `%s` |' % (e['name'], ev_args(e)))

# ---------------------------------------------------------------- functions
by_table = collections.defaultdict(list)
for b in B:
    by_table[b['table']].append(b)
TABLE_NOTE = {
    'Debug': 'Every `Debug` function except `ShowScreenMessage` is a stub in the PC build. `Debug.Print` and `Debug.Assert` share one empty function at `402CC0`, so a hook there gives them a body; with one installed they work as their names say.',
    'Game': 'Index conventions, from the scripts: `Game.GetRacerIndex`, `GetRacerElement`, `GetRacerCharacter` and `SetRacerLapsLeft` count racers from 1. `RaceStatus:GetRacerInfoByIndex`, `racerInfo:GetIndex()`, `RaceStatus:ShowTurnIndicator` and `HideTurnIndicator` count from 0. `Game.ForceAIControl` and `ClearAIControl` take player 0 or 1.',
    'LuaDebug': 'A stub in the PC build.',
    'Bin': 'Both are stubs in the PC build.',
}
funcs = ["When the Retail column says none, the description is a guess from the name. The nfs.exe column is the address of the C function."]
for t in sorted(k for k in by_table if not by_table[k][0]['kind'] == 'method'):
    funcs.append('\n### %s\n' % t)
    if t in TABLE_NOTE: funcs.append(TABLE_NOTE[t] + '\n')
    funcs.append('| Function | What it does | Retail | nfs.exe |')
    funcs.append('|---|---|---|---|')
    for b in sorted(by_table[t], key=lambda b: b['name']):
        funcs.append('| `%s` | %s | %s | `%s` |' % (cell(sig(b)), cell(what(b)), cell(retail(b)), b['fn'].upper()))

# ---------------------------------------------------------------- objects and methods
objects = api['objects']; dynamic = api['dynamic']
OBJ_ROWS = [
    ('RaceStatus', 'GRaceStatus', 'set at startup to `GRaceStatus::fObj`', ''),
    ('ProStreetGame', 'GProStreetGame', 'set at startup', ''),
    ('Career', 'GCareer', 'set at startup', 'No methods are bound on PC.'),
    ('GEvent', 'GEvent', 'set at startup', 'The current race day event.'),
    ('GMoviePlayer', 'GMoviePlayer', 'set at startup', ''),
    ('GHub', 'GHub', 'looked up on every read, from `GProStreetGame`', 'nil outside a hub.'),
    ('GTier', 'GTier', 'looked up on every read, from `GProStreetGame`', 'nil when there is no current tier.'),
    ('GRaceParameters', 'GRaceParameters', 'looked up on every read', 'nil when there is no current race.'),
]
nmeth = collections.Counter(b['table'] for b in B if b['kind'] == 'method')
assert set(objects) == {r[0] for r in OBJ_ROWS[:5]} and set(dynamic) == {r[0] for r in OBJ_ROWS[5:]}
obj = ['| Global | Type | How it gets there | Methods | Notes |', '|---|---|---|---|---|']
for g, t, how, note in OBJ_ROWS:
    obj.append('| `%s` | `%s` | %s | %d | %s |' % (g, t, how, nmeth.get(t, 0), note))
obj.append('')
obj.append('Other userdata types reach scripts as values:')
obj.append('')
obj.append('| Type | Comes from | Methods |')
obj.append('|---|---|---|')
obj.append('| `GRacerInfo` | `RaceStatus:GetRacerInfo`, `GetRacerInfoByIndex`, `GetRacerInfoByOpponentId` | %d |' % nmeth['GRacerInfo'])
obj.append('| `ISimable` | message fields such as `Racer` and `Element`, `Game.GetRacerElement`, `racerInfo:GetSimable()` | %d |' % nmeth['ISimable'])
obj.append('| `GRuntimeInstance` | `self`, element fields, message fields such as `Sender` | none, fields instead |')
obj.append('| `LuaAttribArrayInfo` | array fields of instances | none, `Count` and `[i]` |')
obj.append('| `GWrapper<T>` | message fields such as `MHubCarChanged.CarHandle` | none |')
METHOD_ORDER = [('GRaceStatus', 'RaceStatus'), ('GRacerInfo', 'racerInfo'), ('ISimable', 'simable'), ('GEvent', 'GEvent'),
                ('GHub', 'GHub'), ('GTier', 'GTier'), ('GRaceParameters', 'GRaceParameters'), ('GMoviePlayer', 'GMoviePlayer'),
                ('GProStreetGame', 'ProStreetGame')]
meths = []
for t, holder in METHOD_ORDER:
    lst = sorted(by_table[t], key=lambda b: b['name'])
    meths.append('\n### %s\n' % t)
    if t == 'GRacerInfo':
        meths.append('A racer in the current race. Get one with `RaceStatus:GetRacerInfo(simable)`; it is nil for a car that is not racing.\n')
    if t == 'ISimable':
        meths.append('A car in the world. The userdata holds a handle, not a pointer, so it stays safe to keep in `vars`.\n')
    meths.append('| Method | What it does | Retail | nfs.exe |')
    meths.append('|---|---|---|---|')
    for b in lst:
        meths.append('| `%s` | %s | %s | `%s` |' % (cell(sig(b, holder + ':' + b['name'])), cell(what(b)), cell(retail(b)), b['fn'].upper()))

# ---------------------------------------------------------------- constants
constants = """```lua
-- basicrace, stategraph_basicrace_initial (GRacerInfo::State)
vars.State = {kState_Initial = 1, kState_NIS = 2, kState_PreStaging = 3, kState_Staging = 4,
	kState_PostStaging = 5, kState_Handicap = 6, kState_Countdown = 7, kState_Racing = 8,
	kState_RaceOver = 9, kState_Spectate = 10, kState_Done = 11}
-- GRacerInfo::Type
vars.RacerType = {kType_Human = 1, kType_Artifical = 2}
-- NIS::eNIS_TYPE, compared with MNISComplete.SceneType and MNISFailed.SceneType
vars.NisType = {Unknown = 0, Intro = 1, Arrest = 2, Ending = 3, World = 4, Moment = 5, PIP = 6,
	UcapPIP = 7, Ucap = 8, Countdown = 9, Career = 10, MomentDynamic = 11, FinishCam = 12, HUB = 13,
	Replay = 14, HUB_Challenge = 15, HUB_Pinkslip = 16, Crowd = 17}
-- GRace::Behaviour, compared with RaceStatus:GetRaceBehaviour()
vars.Behaviour = {kRaceBehaviour_Single = 1, kRaceBehaviour_Dual = 2}
-- org_monitor, stategraph_org_monitor_initial (GRace::Mode, kRaceMode_None = 0 is left out)
vars.Mode = {kRaceMode_Grip = 1, kRaceMode_HighSpeedChallenge = 2, kRaceMode_Drag = 3, kRaceMode_Drift = 4}
```

Racer states go to `RaceStatus:StateCatchupAll`, `StateOverrideAll`, `StateCheckAll` and `racerInfo:SetState`, and come back in `MPlayerStateEnter.State`, `MPlayerStateExit.State` and `MRaceStateEnter.State`. Race modes go to `ProStreetGame:GetTrackRecordCount` and `UnlockEliteOrgTier`.

A table defined in one activity's `vars` is not visible in another's. Every race made from basicrace or one of its child templates runs basicrace's `initial` handler, which defines `State`, `RacerType`, `NisType` and `Behaviour` in that race's `vars`. A script for another activity has to define them again."""

# ---------------------------------------------------------------- missing names
ORIGIN = {
    'Demo.SetRaceCompleteForFE': ('Most Wanted (`speed.exe` has the string)', 'Calling a field of the nil global `Demo` is an error, so the game exits.'),
    'Demo.StorePursuitRepForFE': ('Most Wanted', 'Same as above.'),
    'EBailPursuit': ('Most Wanted and Carbon', 'The `E` closure finds no such event and returns. Nothing happens.'),
    'EShowPostPursuit': ('not found in the MW or Carbon executables either', 'Nothing happens.'),
    'GCareer:AbortBossFlow': ('Carbon', 'Calling a nil method is an error. The call sits behind `self.BossRace`, which no retail element sets to true, so it never runs.'),
    'Game.NotifyCheckpointReached': ('Most Wanted and Carbon', 'Calling nil is an error.'),
    'Game.SetRacerGoal': ('Most Wanted and Carbon', 'Calling nil is an error.'),
    'GetRacerIndex': ('a mistake: the function is `Game.GetRacerIndex`', 'Calling nil is an error.'),
}
miss = ['These names appear in retail handlers but are not bound in the PC build. None of them appears in the 360 milestone symbols either. The strings of most of them are in the earlier games\' executables, so they are leftovers.', '',
        '| Name | Where | Origin | What happens |', '|---|---|---|---|']
for u in api['unknown_uses']:
    o, h = ORIGIN[u['key']]
    nm = 'Career:AbortBossFlow' if u['key'] == 'GCareer:AbortBossFlow' else u['key']
    miss.append('| `%s` | %s | %s | %s |' % (nm, ', '.join(u['templates']), o, h))
miss.append('')
miss.append('No element in the retail `gameplay.lzc` is made from `pursuit_mini_game` or `checkpointrace`, so these lines never run in the shipped game. Copy those scripts and they end the game when they reach these lines.')

out = tmpl
for k, v in [('SUMMARY', summary), ('CHANGESTATE_COUNT', changestate), ('SELF_FIELDS', self_fields),
             ('MESSAGES_HANDLED', messages_handled), ('MESSAGES_SENT', messages_sent), ('EVENTS_USED', events_used),
             ('FUNCTIONS', '\n'.join(funcs)), ('OBJECTS', '\n'.join(obj)), ('METHODS', '\n'.join(meths)),
             ('CONSTANTS', constants), ('MISSING', '\n'.join(miss)), ('ALL_MESSAGES', '\n'.join(all_msgs)),
             ('ALL_EVENTS', '\n'.join(all_ev))]:
    assert '<!--%s-->' % k in out, k
    out = out.replace('<!--%s-->' % k, v)
assert '<!--' not in out
open(P('lua-api.md'), 'w').write(out)
print('lua-api.md', len(out.splitlines()), 'lines')
