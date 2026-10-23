#!/usr/bin/env python3
"""Lists what current-gen Undercover (PC and Xbox 360, Lua 5.1) gives
gameplay Lua, in the columns of scan_wii.py, so that write_list.py reads them
the same way. The source is the Xbox 360 debug build,
nfs-data/SpeedDebugOpt-enUS.exe (the memory image that
tools/xex/extract_image.py --undercover makes from the .xex) with its PDB;
the PC build, exes/pc/undercover/nfs_steam.exe with its IDA map, is compared
with it, as scan_mw.py compares Most Wanted's builds.

    python3 -I -B scan_uc360.py <repo root> <work folder> [<name file> ...]

Names: the PDB's public symbols (S_PUB32, MSVC-mangled, so they carry the
types; tools/xex/symmap.py reads the same records but keeps one name per
address and none longer than 512 bytes) and its procedure records
(S_GPROC32, S_LPROC32: the start, length and plain name of every function,
static ones too, such as the thunk templates and the message bindings).

The game binds Lua in these places, all read here:
- LuaBindery::BindToGameCode, BindCustomMethods, BindEnums, the
  ILuaBindable classes' BindStaticMethods (LuaHubBinder,
  LuaRaceParametersBinder) and Runtime::CreateState. Their calls are read in
  order: GetGlobalTable(T), lua_pushstring(N), lua_pushlightuserfunc(F),
  lua_pushcclosure(thunk) and SetInGlobalTable(T) make the function T.N;
  LoadMetatable(M), lua_pushstring(N), a member function pointer,
  lua_pushcclosure(thunk) and lua_settable(-3) the method N of metatable M;
  lua_pushstring(G), lua_newuserdata, LoadMetatable(M), lua_setmetatable and
  lua_settable(LUA_GLOBALSINDEX) the global object G; lua_pushnumber in place
  of a closure a number (BindEnums: GRaceType.*); lua_pushcclosure and
  lua_setfield(LUA_GLOBALSINDEX, N) the bare global N (_ALERT). A name bound
  twice keeps the second binding.
- The LuaFuncBinderProxy objects (_lfbp_<Table>_<Name>), which
  LuaFuncBinderBase::BindAll binds after BindCustomMethods. Each one's name
  (+8) and table (+72) are copied by its dynamic initializer, read from the
  two bStrNCpy calls there; its function (+136) is in its static data or
  stored by the initializer; its vtable, stored last, names the binder
  template.
- The ILuaBindable classes' DoCustomLookup: the globals Runtime's
  HandleGlobalIndex answers by asking them (GHub, GEvent, GRaceParameters),
  looked up on every read. Their metatable is the one
  ReturnValue<T *>::Translate gets from the registry.
- HandleGlobalIndex answers a name starting with E with
  GameCore::Lua::LuaEventBinding, which fires the event of that name when its
  definition has a fire function and flag bit 1 and the call passes exactly
  its field count (the EventSys registry, LookupEvent's tables).
- It answers a name starting with M with the post handler of the
  MessageBindable registered for that message (MessageBindable::Initialize
  registers them all: hash, post handler, bind handler): the argument count
  the handler checks (its cmpwi after lua_gettop), the message's fields from
  the Hermes registry (LookupMessage's tables) and how it reads each argument.
Types come from the bound C function's mangled name (a GameHooksCore
function, a <Table>_<Name> function, a member function) when one of the names
at its address is the bound name, else from the thunk's template arguments.
A method bound through a virtual call thunk (??_9) takes the thunk's types.

The PC build binds the same way, through code the map doesn't name: its
binder functions are found from the strings of the names the 360's make and
read by the arguments of their calls (lua_pushcclosure(L, thunk, n) and the
strings and code addresses before it, or a binder template's push function,
push name, table in edi); its LuaFuncBinderProxy initializers are named in the
map; its event definitions are found from a pointer to the 360's first one by
hash; its MessageBindable::Initialize from its push bind, push post, hash
sequence.

Written to the work folder:
  uc360_bindings.tsv, uc360_events.tsv, uc360_messages.tsv: the 360 debug
    build's (bindings.tsv columns: kind, table or metatable, name, C function,
    its symbol, binder (thunk, LuaFuncBinder vtable or lookup), return type,
    parameter types, stub: what the bound function does when it does nothing).
  uc_bindings.tsv: what write_list.py reads: the 360's bindings, with the
    PC's stub in place of the 360's and a tenth column, a note where the
    builds differ ("Not a stub in the 360 debug build.", "Not bound by the PC
    build."). The C function stays the 360's, so that names bound to one
    function show it (the PC's linker folded identical stubs into one).
  uc_constants.tsv: the numbers: table, name, value, the enum they come from.
  uc_events.tsv: the PC's event definitions (the 360's if not found).
  uc_messages.tsv: the 360's M<Name> globals that the PC registers too.
  uc_builds.txt: what the two builds bind differently.
Field names of events and messages come from their StringHash32("f" + name),
looked up among the identifiers in the name files given.
"""
import bisect, collections, os, re, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import capstone  # noqa: E402
from msg_table import lookup2  # noqa: E402
from scan_wii import names_pool, EV_TYPES  # noqa: E402

BASE = 0x82000000
LUA_GLOBALSINDEX, LUA_REGISTRYINDEX = -10002, -10000


# ---------------------------------------------------------------- the image and its PDB
class Image:
    def __init__(self, exe, pdb):
        self.img = open(exe, 'rb').read()
        img = self.img
        pe = struct.unpack_from('<I', img, 0x3c)[0]
        n, opt = struct.unpack_from('<H', img, pe + 6)[0], struct.unpack_from('<H', img, pe + 20)[0]
        self.sections = []                      # (name, va, size)
        for i in range(n):
            o = pe + 24 + opt + 40 * i
            name = img[o:o + 8].rstrip(b'\0').decode('latin1')
            vs, va = struct.unpack_from('<II', img, o + 8)
            self.sections.append((name, BASE + va, vs))
        self.text = next((va, va + vs) for nm, va, vs in self.sections if nm == '.text')
        secva = [va - BASE for nm, va, vs in self.sections]
        d = open(pdb, 'rb').read()
        self.pub = collections.defaultdict(list)     # address -> mangled names
        self.procs = {}                               # address -> (length, name)
        for tag in (b'\x0e\x11', b'\x0f\x11', b'\x10\x11'):
            i = 0
            while True:
                i = d.find(tag, i + 1)
                if i < 0: break
                reclen = struct.unpack_from('<H', d, i - 2)[0]
                if tag == b'\x0e\x11':               # S_PUB32: flags, offset, segment, name
                    if not 14 <= reclen <= 4096: continue
                    off, seg = struct.unpack_from('<IH', d, i + 6)
                    at, length = i + 12, None
                else:                                # S_LPROC32/S_GPROC32: ..., length at +12, offset at +28
                    if not 36 <= reclen <= 4096: continue
                    length = struct.unpack_from('<I', d, i + 14)[0]
                    off, seg = struct.unpack_from('<IH', d, i + 30)
                    at = i + 37
                if not 1 <= seg <= len(secva): continue
                e = d.find(b'\0', at, i + 2 + reclen)
                name = d[at:e] if e > at else b''
                if not name or any(c < 0x20 or c > 0x7e for c in name): continue
                va = BASE + secva[seg - 1] + off
                name = name.decode('latin1')
                if length is None:
                    if name not in self.pub[va]: self.pub[va].append(name)
                elif length < 0x10000 and self.text[0] <= va < self.text[1]:
                    self.procs.setdefault(va, (length, name))
        self.by_pub = {}
        for a, ns in self.pub.items():
            for nm in ns: self.by_pub.setdefault(nm, a)
        self.by_proc = {}
        for a, (ln, nm) in self.procs.items(): self.by_proc.setdefault(nm, a)
        self.pstarts = sorted(self.procs)
        self.md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)

    def read(self, va, n):
        o = va - BASE
        return self.img[o:o + n] if 0 <= o < len(self.img) else b''

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('>I', b)[0] if len(b) == 4 else None

    def f32(self, va):
        b = self.read(va, 4)
        return struct.unpack('>f', b)[0] if len(b) == 4 else None

    def cstr(self, va, limit=128):
        if va is None: return None
        b = self.read(va, limit)
        e = b.find(b'\0')
        if e <= 0: return None
        s = b[:e]
        return s.decode() if all(32 <= c < 127 for c in s) else None

    def is_code(self, va):
        return va is not None and self.text[0] <= va < self.text[1]

    def proc_name(self, va):
        return self.procs[va][1] if va in self.procs else None

    def func_end(self, va):
        if va in self.procs: return va + self.procs[va][0]
        i = bisect.bisect_right(self.pstarts, va)
        return self.pstarts[i] if i < len(self.pstarts) else va + 0x400

    def name(self, va):
        """A name for an address: its public names, else its procedure's."""
        if va in self.pub: return self.pub[va][0]
        if va in self.procs: return self.procs[va][1]
        i = bisect.bisect_right(self.pstarts, va) - 1
        if i >= 0:
            s = self.pstarts[i]
            return '%s+0x%x' % (self.procs[s][1], va - s)
        return '%x' % va

    def disasm(self, lo, hi):
        return self.md.disasm(self.read(lo, hi - lo), lo)


# ---------------------------------------------------------------- walking a function
VOLATILE = ['r0'] + ['r%d' % i for i in range(3, 13)]
FVOLATILE = ['f%d' % i for i in range(0, 14)]
QUIET = ('stwu', 'stmw', 'cmpwi', 'cmplwi', 'cmpw', 'cmplw', 'b', 'beq', 'bne', 'blt', 'bgt', 'ble', 'bge',
         'blr', 'mtlr', 'mtctr', 'stb', 'sth', 'stfs', 'stfd', 'nop', 'bctrl', 'bctr', 'std', 'stwx', 'stbx')


def walk(im, lo, hi, on_call):
    """ppcelf.walk for the 360 build: a linear walk tracking constants in
    registers (lis/li/addi/addis/ori/mr, lwz from a known address or the
    stack frame, ld from the stack frame, lfs from a known address). At every
    `bl` it calls on_call(site, target, regs, fregs, stack, codes), codes
    being the code addresses materialized since the call before, then forgets
    the volatile registers. Returns every code address materialized."""
    regs, fregs, stack, codes, every = {'r1': 0}, {}, {}, [], []

    def imm(x):
        return int(x, 0)

    def mem(op):
        d, base = op.rstrip(')').split('(')
        return imm(d), base

    def note(r):
        v = regs.get(r)
        if im.is_code(v):
            codes.append(v)
            every.append(v)
    for ins in im.disasm(lo, hi):
        m = ins.mnemonic
        ops = [o.strip() for o in ins.op_str.split(',')] if ins.op_str else []
        try:
            if m == 'lis':
                regs[ops[0]] = (imm(ops[1]) << 16) & 0xffffffff
            elif m == 'li':
                regs[ops[0]] = imm(ops[1]) & 0xffffffff
            elif m in ('addi', 'addic', 'addis'):
                b = regs.get(ops[1])
                k = imm(ops[2]) << (16 if m == 'addis' else 0)
                regs[ops[0]] = None if b is None else (b + k) & 0xffffffff
                note(ops[0])
            elif m == 'ori':
                b = regs.get(ops[1])
                regs[ops[0]] = None if b is None else b | imm(ops[2])
                note(ops[0])
            elif m == 'mr':
                regs[ops[0]] = regs.get(ops[1])
            elif m in ('lwz', 'ld'):
                d, base = mem(ops[1])
                b = regs.get(base)
                if base == 'r1':
                    regs[ops[0]] = stack.get(d)
                elif m == 'lwz' and b is not None and im.read((b + d) & 0xffffffff, 4):
                    regs[ops[0]] = im.u32((b + d) & 0xffffffff)
                else:
                    regs[ops[0]] = None
            elif m == 'stw':
                d, base = mem(ops[1])
                if base == 'r1': stack[d] = regs.get(ops[0])
            elif m == 'lfs':
                d, base = mem(ops[1])
                b = regs.get(base)
                fregs[ops[0]] = im.f32((b + d) & 0xffffffff) if b is not None else None
            elif m == 'fmr':
                fregs[ops[0]] = fregs.get(ops[1])
            elif m == 'bl':
                on_call(ins.address, imm(ops[0]), regs, fregs, stack, codes)
                for r in VOLATILE: regs.pop(r, None)
                for r in FVOLATILE: fregs.pop(r, None)
                codes = []
            elif m in QUIET:
                pass
            elif ops and re.match(r'^r\d+$', ops[0]):
                regs.pop(ops[0], None)
            elif ops and re.match(r'^f\d+$', ops[0]):
                fregs.pop(ops[0], None)
        except ValueError:
            if ops: regs.pop(ops[0], None)
    return every


# ---------------------------------------------------------------- types
BASIC = {'X': 'void', 'D': 'char', 'C': 'signed char', 'E': 'unsigned char', 'F': 'short', 'G': 'unsigned short',
         'H': 'int', 'I': 'unsigned int', 'J': 'long', 'K': 'unsigned long', 'M': 'float', 'N': 'double',
         'O': 'long double', '_N': 'bool', '_J': 'long long', '_K': 'unsigned long long', '_W': 'wchar_t'}


class Demangle:
    """The parts of MSVC's mangling that bound functions use: a qualified
    name, then a function type of basic types, pointers and references to
    them or to classes, structs, enums and class templates."""

    def __init__(self, s):
        self.s, self.i, self.names, self.args = s, 0, [], []

    def peek(self, n=1):
        return self.s[self.i:self.i + n]

    def name_part(self):
        s = self.s
        if s[self.i].isdigit():
            k = int(s[self.i]); self.i += 1
            return self.names[k]
        if s.startswith('?$', self.i):
            self.i += 2
            j = s.index('@', self.i)
            base = s[self.i:j]; self.i = j + 1
            outer = (self.names, self.args)
            self.names, self.args = [base], []
            targs = []
            while self.peek() != '@':
                targs.append(self.type())
            self.i += 1
            self.names, self.args = outer
            n = '%s<%s>' % (base, ','.join(targs))
            self.names.append(n)
            return n
        j = s.index('@', self.i)
        n = s[self.i:j]; self.i = j + 1
        self.names.append(n)
        return n

    def qualified(self):
        parts = []
        while self.peek() != '@':
            parts.append(self.name_part())
        self.i += 1
        return '::'.join(reversed(parts))

    def type(self, arg=False):
        s, start = self.s, self.i
        c = s[self.i]
        if arg and c.isdigit():
            self.i += 1
            return self.args[int(c)]
        if c == '_':
            t = BASIC[s[self.i:self.i + 2]]; self.i += 2
        elif c in BASIC:
            t = BASIC[c]; self.i += 1
        elif c in 'PQAR':                        # pointer, const pointer, reference
            cv = s[self.i + 1]; self.i += 2
            inner = self.type()
            t = ('const ' if cv in 'BD' else '') + inner + (' *' if c in 'PQ' else ' &')
        elif c in 'VUT':
            self.i += 1
            t = self.qualified()
        elif c == 'W':
            self.i += 2
            t = 'enum ' + self.qualified()
        elif s.startswith('$$C', self.i):
            cv = s[self.i + 3]; self.i += 4
            t = ('const ' if cv in 'BD' else '') + self.type()
        else:
            raise ValueError('type code %r in %s' % (c, s))
        if arg and self.i - start > 1: self.args.append(t)
        return t

    def function(self):
        """(name, scope, return type, [parameter types]) of '?name@scope@@<type>', or None."""
        s = self.s
        if not s.startswith('?') or s.startswith('??'): return None
        self.i = 1
        full = self.qualified()
        kind = s[self.i]; self.i += 1
        if kind in 'QIAUMEBJCRKS' or kind == 'Y':
            if kind in 'QIAUMEBJR': self.i += 1          # cv of this
            self.i += 1                                  # calling convention
        else:
            return None
        if self.peek() == '?':                           # a class returned by value
            self.i += 2
        ret = self.type()
        params = []
        if self.peek() == 'X':
            self.i += 1
        else:
            while self.peek() not in ('@', 'Z', ''):
                params.append(self.type(arg=True))
            if self.peek() == '@': self.i += 1
        if self.peek() != 'Z': return None
        parts = full.split('::')
        return parts[-1], '::'.join(parts[:-1]), ret, params


def demangle(sym):
    try:
        return Demangle(sym).function()
    except (ValueError, IndexError, KeyError):
        return None


def c_type(t):
    """A PROC name's C++ spelling ('char const *', 'GWrapper<unsigned int> ')
    the way Demangle writes it ('const char *')."""
    t = t.strip()
    m = re.match(r'^(.*?) const \*$', t)
    if m: return 'const %s *' % m.group(1)
    if t.startswith('enum '): return t
    return t


def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '<': depth += 1
        elif ch == '>': depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''
        else:
            cur += ch
    if cur.strip(): out.append(cur)
    return [c_type(a) for a in out]


def thunk_types(name):
    """(return, params, class) from a thunk's plain name:
    'VoidBinder::Arg1<char const *>::Thunk', 'Binder<bool>::Arg1<int>::Thunk',
    'VoidFunctionArg2Thunk<bool,char const *>',
    'MethodBinder<bool,RaceStatusLuaProxy>::Arg1<int>::ThunkMethod',
    'VoidMethodBinder<GHub>::ThunkMethod'; None for anything else."""
    if not name: return None
    m = re.match(r'^(Void)?(Function|Method)Arg\d+Thunk<(.*)>$', name)
    if m:                        # <return,> <class,> parameters
        a = split_args(m.group(3))
        ret = 'void' if m.group(1) else a.pop(0)
        cls = a.pop(0) if m.group(2) == 'Method' else None
        return ret, a, cls
    m = re.match(r'^(Void)?(Method)?Binder(?:<(.*?)>)?((?:::Arg\d+<.*?>)*)::Thunk(?:Method)?$', name)
    if not m: return None
    head = split_args(m.group(3)) if m.group(3) else []
    args = [c_type(a) for a in re.findall(r'::Arg\d+<(.*?)>(?=::|$)', m.group(4))]
    cls = None
    if m.group(2):
        cls = head[-1] if head else None
        head = head[:-1]
    ret = 'void' if m.group(1) else (head[0] if head else None)
    return ret, args, cls


def binder_types(name):
    """(return, params) from a LuaFuncBinder vtable's name,
    ??_7?$LuaFuncBinderVoid2@PBDH@@6B@ or ??_7LuaFuncBinderVoid0@@6B@."""
    m = re.match(r'^\?\?_7(\?\$)?LuaFuncBinder(Void)?(\d)(@?)(.*?)@@6B@$', name)
    if not m: return None
    if not m.group(1): return 'void', []
    d = Demangle(m.group(5) + '@')
    try:
        ts = []
        while d.peek() not in ('@', ''):
            ts.append(d.type())
    except (ValueError, IndexError, KeyError):
        return None
    return ('void', ts) if m.group(2) else (ts[0], ts[1:])


def stub_kind(im, va, ret):
    """What a function that does nothing does: its first instructions."""
    ins = [(i.mnemonic, i.op_str) for i in im.disasm(va, va + 12)]
    if not ins: return ''
    if ins[0][0] == 'blr': return 'does nothing'
    if ret == 'raw':
        return 'does nothing' if ins[0] == ('li', 'r3, 0') and len(ins) > 1 and ins[1][0] == 'blr' else ''
    if len(ins) > 1 and ins[1][0] == 'blr' and ins[0][0] == 'li' and ins[0][1].startswith('r3, '):
        return 'always returns %d' % int(ins[0][1].split(',')[1], 0)
    return ''


# ---------------------------------------------------------------- the binder functions
class Bindings:
    def __init__(self, im):
        self.im = im
        self.rows, self.constants, self.seen = [], [], set()
        self.where, self.made = None, collections.defaultdict(list)     # binder function -> keys it made
        self.rebound = []
        api = ['lua_pushstring', 'lua_pushcclosure', 'lua_pushlightuserfunc', 'lua_settable', 'lua_setmetatable',
               'lua_newuserdata', 'lua_pushnumber', 'lua_getfield', 'lua_setfield', 'lua_settop']
        self.api = {im.by_pub[n]: n for n in api}
        for n in ('GetGlobalTable', 'SetInGlobalTable', 'LoadMetatable'):
            self.api[im.by_pub['?%s@LuaBindery@@SAXPAUlua_State@@PBD@Z' % n]] = n

    def add(self, row):
        """A binding; one made again under the same name replaces it, as the
        lua_settable that makes it does (RaceStatus:StateCheckAny is bound
        with two parameters, then with one)."""
        key = tuple(row[:3])
        if key in self.seen:
            i = next(i for i, r in enumerate(self.rows) if tuple(r[:3]) == key)
            self.rebound.append('%s %s.%s: %s(%s), then %s(%s)' % (key + (self.rows[i][6], self.rows[i][7], row[6], row[7])))
            self.rows[i] = row
            return
        self.seen.add(key)
        self.rows.append(row)
        self.made[self.where].append(key)

    def choose(self, fn, want, cls=None):
        """The demangled name at fn whose function name is `want` (and whose
        scope ends with cls, for a method); for a method, else the one member
        function of cls there (bound under another name); or None."""
        found = []
        for sym in self.im.pub.get(fn, []):
            d = demangle(sym)
            if d and (cls is None or d[1].split('::')[-1] == cls):
                if d[0] == want: return sym, d
                found.append((sym, d))
        return found[0] if cls and len(found) == 1 else None

    def thunk(self, va):
        """(return, params, class) of a thunk at va, from its procedure name."""
        return thunk_types(self.im.proc_name(va))

    def function_row(self, tbl, name, fn, closure, binder_name):
        im = self.im
        ret, params, fsym = None, None, im.name(fn) if fn else '-'
        if closure is not None and fn == closure:                # a lua_CFunction itself
            ret, params = 'raw', ['raw']
            fsym = im.proc_name(fn) or im.name(fn)
        else:
            ch = self.choose(fn, name) or self.choose(fn, '%s_%s' % (tbl, name))
            if ch:
                fsym, (n_, scope, ret, params) = ch[0], ch[1]
            else:
                t = self.thunk(closure) if closure else None
                if t: ret, params = t[0], t[1]
                else: print('no types for %s.%s (%s, %s)' % (tbl, name, im.name(fn), binder_name), file=sys.stderr)
        stub = stub_kind(im, fn, ret) if fn and ret else ''
        self.add(['function', tbl, name, '%x' % fn if fn else '-', fsym, binder_name, ret or '',
                  'raw' if ret == 'raw' else ', '.join(params or []), stub])

    def method_row(self, mt, name, pmf, closure):
        im = self.im
        t = self.thunk(closure)
        ret, params, cls = t if t else (None, None, None)
        fsym = '-'
        if pmf is not None:
            ch = self.choose(pmf, name, cls)
            if ch and not ch[0].startswith('??_9'):
                fsym, d = ch
                if (ret, params) != (d[2], d[3]) and t:
                    print('method %s:%s: thunk %s, function %s' % (mt, name, (ret, params), d[2:]), file=sys.stderr)
                ret, params = d[2], d[3]
            else:
                fsym = im.pub[pmf][0] if pmf in im.pub else im.name(pmf)
        if ret is None: print('no types for method %s:%s' % (mt, name), file=sys.stderr)
        virtual = fsym.startswith('??_9')
        stub = stub_kind(im, pmf, ret) if pmf and ret and not virtual else ''
        self.add(['method', mt, name, '%x' % pmf if pmf else '-', fsym, im.name(closure) if closure else '-',
                  ret or '', ', '.join(params or []), stub])

    def scan(self, label):
        """The bindings a binder function makes, read from its calls in order."""
        im = self.im
        self.where, start = label, im.by_proc[label]
        st = {'mt': None, 'strs': [], 'up': None, 'closure': None, 'num': None, 'setmt': False, 'pmf': []}

        def reset():
            st.update(strs=[], up=None, closure=None, num=None, setmt=False, pmf=[])

        def on_call(site, t, regs, fregs, stack, codes):
            st['pmf'] += codes
            what = self.api.get(t)
            r4, r5 = regs.get('r4'), regs.get('r5')
            if what == 'GetGlobalTable':
                reset()
            elif what == 'LoadMetatable':
                st['mt'] = im.cstr(r4)
            elif what == 'lua_pushstring':
                st['strs'].append(im.cstr(r4))
            elif what == 'lua_pushlightuserfunc':
                st['up'] = r4
            elif what == 'lua_pushcclosure':
                st['closure'] = r4
            elif what == 'lua_pushnumber':
                st['num'] = fregs.get('f1')
            elif what == 'lua_setmetatable':
                st['setmt'] = True
            elif what == 'SetInGlobalTable':
                tbl, name = im.cstr(r4), st['strs'][-1] if st['strs'] else None
                if not name:
                    print('%x: SetInGlobalTable without a name' % site, file=sys.stderr)
                elif st['num'] is not None and st['closure'] is None:
                    v = st['num']
                    self.constants.append((tbl, name, ('%d' % v) if v == int(v) else repr(v)))
                    self.made[label].append(('constant', tbl, name))
                elif st['closure'] is not None:
                    fn = st['up'] if st['up'] is not None else st['closure']
                    self.function_row(tbl, name, fn, st['closure'], im.name(st['closure']))
                reset()
            elif what == 'lua_settable':
                r4s = r4 - 0x100000000 if r4 is not None and r4 & 0x80000000 else r4
                name = st['strs'][0] if st['strs'] else None
                if r4s == LUA_GLOBALSINDEX and st['setmt'] and name:
                    self.add(['object', st['mt'], name, '-', '-', 'BindCustomMethods', '', '', ''])
                elif r4s == LUA_GLOBALSINDEX and st['closure'] is not None and name:
                    self.function_row('', name, st['closure'], st['closure'], 'lua_settable(LUA_GLOBALSINDEX)')
                elif r4s == -3 and st['mt'] and name and st['closure'] is not None:
                    pmf = [c for c in st['pmf'] if c != st['closure']]
                    self.method_row(st['mt'], name, pmf[-1] if pmf else None, st['closure'])
                elif (name or st['closure']) and name != '__index':      # CreateState: the globals' metatable
                    print('%x: lua_settable(%s) not read (%s)' % (site, r4s, st), file=sys.stderr)
                reset()
            elif what == 'lua_setfield':                      # lua_setfield(L, LUA_GLOBALSINDEX, name)
                r4s = r4 - 0x100000000 if r4 is not None and r4 & 0x80000000 else r4
                if r4s == LUA_GLOBALSINDEX and st['closure'] is not None and im.cstr(r5):
                    self.function_row('', im.cstr(r5), st['closure'], st['closure'], 'lua_setfield(LUA_GLOBALSINDEX)')
                reset()
            elif what == 'lua_settop':
                reset()
        walk(im, start, im.func_end(start), on_call)

    def proxies(self):
        """The LuaFuncBinderProxy objects (_lfbp_<Table>_<Name>)."""
        im = self.im
        self.where = 'LuaFuncBinderProxy objects'
        objs = {a: n for a, ns in im.pub.items() for n in ns if n.startswith('?_lfbp')}
        inits = self.initializers(set(objs))
        bstrncpy = im.by_pub['?bStrNCpy@@YAPADPADPBDH@Z']
        for obj in sorted(objs, key=lambda a: objs[a]):
            fn = im.u32(obj + 136)                      # set in the static data, or by the initializer
            init = inits.get(obj)
            strs, vt = [], None
            if init:
                def on_call(site, t, regs, fregs, stack, codes):
                    if t == bstrncpy: strs.append(im.cstr(regs.get('r4')))
                made = walk(im, init, im.func_end(init), on_call)
                if made: fn = made[-1]
                vt = self.vtable_of(init)
            if len(strs) != 2 or not im.is_code(fn):
                print('%s: name and table %s, function %s' % (objs[obj], strs, fn), file=sys.stderr)
                continue
            name, tbl = strs
            vname = (im.pub.get(vt) or ['-'])[0] if vt else '-'
            ch = self.choose(fn, '%s_%s' % (tbl, name)) or self.choose(fn, name)
            bt = binder_types(vname)
            if ch:
                fsym, (n_, scope, ret, params) = ch[0], ch[1]
                if bt and (ret, params) != bt:
                    print('%s.%s: function %s, binder %s' % (tbl, name, (ret, params), bt), file=sys.stderr)
            elif bt:
                fsym, (ret, params) = im.name(fn), bt
            else:
                print('no types for %s.%s' % (tbl, name), file=sys.stderr)
                continue
            self.add(['function', tbl, name, '%x' % fn, fsym, vname, ret, ', '.join(params), stub_kind(im, fn, ret)])

    def initializers(self, objs):
        """{object: the function that stores its address} for the dynamic initializers: the
        functions holding a lis/addi pair that makes the object's address."""
        im = self.im
        his = collections.defaultdict(set)
        for t in objs:
            his[((t >> 16) + (1 if t & 0x8000 else 0)) & 0xffff].add(t)
        lo, hi = im.text
        data = im.img[lo - BASE:hi - BASE]
        W = struct.unpack('>%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
        out = {}
        for k, w in enumerate(W):
            if (w >> 26) == 15 and ((w >> 16) & 31) == 0 and (w & 0xffff) in his:
                rd, h = (w >> 21) & 31, w & 0xffff
                for j in range(k + 1, min(k + 16, len(W))):
                    v = W[j]
                    if (v >> 26) == 14 and ((v >> 16) & 31) == rd:
                        val = ((h << 16) + ((v & 0xffff) - 0x10000 if v & 0x8000 else v & 0xffff)) & 0xffffffff
                        if val in objs and val not in out:
                            a = lo + 4 * k
                            while a > lo and im.u32(a) != 0x7d8802a6: a -= 4    # back to its mflr r12
                            out[val] = a
                        break
        return out

    def vtable_of(self, init):
        """The last vtable address an initializer materializes."""
        vt = None
        regs = {}
        for ins in self.im.disasm(init, self.im.func_end(init)):
            ops = [o.strip() for o in ins.op_str.split(',')] if ins.op_str else []
            if ins.mnemonic == 'lis': regs[ops[0]] = (int(ops[1], 0) << 16) & 0xffffffff
            elif ins.mnemonic == 'addi' and ops[1] in regs:
                v = (regs[ops[1]] + int(ops[2], 0)) & 0xffffffff
                if any(n.startswith('??_7') for n in self.im.pub.get(v, [])): vt = v
            if ins.mnemonic == 'blr': break
        return vt

    def lookups(self):
        """The globals the ILuaBindable classes answer in DoCustomLookup."""
        im = self.im
        bstrcmp = im.by_pub['?bStrCmp@@YAHPBD0@Z']
        for a, (ln, nm) in sorted(im.procs.items()):
            if not nm.endswith('::DoCustomLookup') or nm.startswith('LuaBindery::'): continue
            self.where, names = nm, []

            def on_call(site, t, regs, fregs, stack, codes):
                if t == bstrcmp: names.append([im.cstr(regs.get('r4')), None])
                pn = im.proc_name(t) or ''
                if pn.startswith('ReturnValue<') and names and names[-1][1] is None: names[-1][1] = t
            walk(im, a, a + ln, on_call)
            for g, tr in names:
                mt = None
                if tr:
                    def on_get(site, t, regs, fregs, stack, codes):
                        nonlocal mt
                        if self.api.get(t) == 'lua_getfield': mt = im.cstr(regs.get('r5'))
                    walk(im, tr, im.func_end(tr), on_get)
                if not g or not mt:
                    print('%s: lookup %s, metatable %s' % (nm, g, mt), file=sys.stderr)
                    continue
                self.add(['object', mt, g, '-', '-', nm, '', '', ''])


# ---------------------------------------------------------------- registries
def registry(im, func):
    """[(hash, definition address, 7 words)] of a LookupEvent/LookupMessage:
    the count it compares against and the pair of tables (keys, definition
    pointers) whose entries agree."""
    a = im.by_pub[func]
    vals, count = [], None
    regs = {}
    for ins in im.disasm(a, im.func_end(a)):
        ops = [o.strip() for o in ins.op_str.split(',')] if ins.op_str else []
        if ins.mnemonic == 'lis': regs[ops[0]] = (int(ops[1], 0) << 16) & 0xffffffff
        elif ins.mnemonic == 'addi' and ops[1] in regs: vals.append((regs[ops[1]] + int(ops[2], 0)) & 0xffffffff)
        elif ins.mnemonic == 'cmplwi' and len(ops) == 3 and count is None and int(ops[2], 0) > 1:
            count = int(ops[2], 0)
    for k in vals:
        for p in vals:
            if k == p: continue
            ok = all(im.u32(p + 4 * i) and im.u32(im.u32(p + 4 * i)) == im.u32(k + 4 * i) for i in range(count))
            if ok:
                return [(im.u32(k + 4 * i), im.u32(p + 4 * i), struct.unpack('>7I', im.read(im.u32(p + 4 * i), 28)))
                        for i in range(count)]
    raise SystemExit('%s: no key and definition tables found' % func)


READS = {'lua_tonumber': 'number', 'lua_toboolean': 'boolean', 'lua_tolstring': 'string', 'lua_touserdata': 'userdata'}


def messages(im, pool):
    """messages.tsv rows: the MessageBindables MessageBindable::Initialize
    registers with a post handler."""
    hermes = {}
    for h, p, rec in registry(im, '?LookupMessage@Registry@Hermes@@YAPBUMessageDef@2@UHash32@EventSys@@@Z'):
        rh, nm, flags, fn, nf, fp = rec[:6]
        fl = []
        for j in range(nf if nf < 64 else 0):
            th, fh, so = struct.unpack('>3I', im.read(fp + 12 * j, 12))
            fl.append('%s:%08x' % (pool.get(fh, '%08x' % fh), th))
        hermes[rh] = (im.cstr(nm), fl)
    init = im.by_pub['?Initialize@MessageBindable@Lua@GameCore@@SAXXZ']
    ctor = im.by_pub['??0MessageBindable@Lua@GameCore@@QAA@UHash32@EventSys@@P6AHPAUlua_State@@@ZP6AX1PBVMessage@Hermes@@@Z@Z']
    found = []

    def on_call(site, t, regs, fregs, stack, codes):
        if t == ctor:
            r4 = regs.get('r4')
            found.append((stack.get(r4) if r4 is not None else None, regs.get('r5'), regs.get('r6')))
    walk(im, init, im.func_end(init), on_call)
    api = {im.by_pub[n]: n for n in list(READS) + ['lua_gettop']}
    rows = []
    for h, post, bind in found:
        name, fields = hermes.get(h, (None, ['?']))
        pn = im.proc_name(post) if post else None
        if pn and pn.endswith('_PostMessage_LuaBinding'):
            if name and pn != name + '_PostMessage_LuaBinding':
                print('message %s: post handler %s' % (name, pn), file=sys.stderr)
            name = name or pn[:-len('_PostMessage_LuaBinding')]
        if not name or lookup2(name) != h:
            print('message %08x (%s): no name' % (h or 0, name), file=sys.stderr)
            continue
        if not post: continue
        nargs, prev = None, None
        for ins in im.disasm(post, post + 0x40):
            if prev == 'gettop' and ins.mnemonic == 'cmpwi':
                nargs = int(ins.op_str.split(',')[-1], 0); break
            prev = 'gettop' if ins.mnemonic == 'bl' and api.get(int(ins.op_str, 0)) == 'lua_gettop' else None
        reads = {}

        def on_read(site, t, regs, fregs, stack, codes):
            k = READS.get(api.get(t))
            if k and regs.get('r4') is not None: reads.setdefault(regs['r4'], k)
        walk(im, post, im.func_end(post), on_read)
        args = ','.join(reads.get(i, '-') for i in range(1, (nargs or 0) + 1))
        rows.append((name, nargs, ' '.join(fields), args))
    return sorted(rows)


def events(im, pool):
    defs = [rec for h, p, rec in registry(im, '?LookupEvent@Registry@EventSys@@YAPBUEventDef@2@UHash32@2@@Z')]
    return event_rows(defs, im.read, im.cstr, pool, lambda b_: struct.unpack('>3I', b_))


# ---------------------------------------------------------------- the PC build
class PC:
    """exes/pc/undercover/nfs_steam.exe with its IDA map, through pemap and
    scan_carbon_pc (cstr, is_code, stub_kind). The map names the
    LuaFuncBinderProxy initializers and little else of this, so the binder
    functions are found from the bound names' strings and read by the
    arguments of their calls."""

    def __init__(self, root):
        sys.path.insert(0, os.path.join(root, 'tools', 'pc'))
        import pemap
        import scan_carbon_pc
        import x86
        self.x86, self.X = x86, scan_carbon_pc
        d = os.path.join(root, 'exes', 'pc', 'undercover')
        self.img = img = pemap.Image(os.path.join(d, 'nfs_steam.exe'), os.path.join(d, 'nfs_steam.map'))
        scan_carbon_pc.pcmap = img
        self.text = img.read(img.TEXT_LO, img.TEXT_HI - img.TEXT_LO)
        self.data = [(lo, img.IMG[ra:ra + rs]) for name, lo, vs, ra, rs in img.SECTIONS if name in ('.rdata', '.data')]

    def cstr(self, va):
        return self.X.cstr(va) if va is not None else None

    def ident(self, va):
        s = self.cstr(va)
        return s if s and re.fullmatch(r'[A-Za-z_]\w*', s) else None

    def strings(self, s):
        """Addresses of the string s in .rdata and .data."""
        out, pat = [], b'\0' + s.encode() + b'\0'
        for lo, data in self.data:
            j = data.find(pat)
            while j >= 0:
                out.append(lo + j + 1)
                j = data.find(pat, j + 1)
        return out

    def find_data(self, pat):
        out = []
        for lo, data in self.data:
            j = data.find(pat)
            while j >= 0:
                if j % 4 == 0: out.append(lo + j)
                j = data.find(pat, j + 1)
        return out

    def refs(self, va):
        """Code addresses holding va as a 32-bit immediate."""
        out, pat = [], struct.pack('<I', va)
        j = self.text.find(pat)
        while j >= 0:
            out.append(self.img.TEXT_LO + j)
            j = self.text.find(pat, j + 1)
        return out

    def func_start(self, a):
        """The 16-byte boundary after the padding or ret before a."""
        lo = a - 0x8000
        while a > lo and not (a % 16 == 0 and (self.img.read(a - 1, 1) in (b'\xcc', b'\xc3'))):
            a -= 1
        return a

    def func_end(self, a):
        end = a
        for ins in self.x86.disasm(self.img.read(a, 0x8000), a):
            end = ins.va + ins.length
            if ins.text.startswith('ret') and (end % 16 == 0 or self.img.read(end, 1) == b'\xcc'): break
        return end

    def calls(self, lo, hi):
        """[(site, target, arguments first to last, code addresses moved into a
        register or stored since the call before, edi)] for every call in lo..hi."""
        imm = re.compile(r'^mov (e\w\w), (0x[0-9a-f]+)$')
        store = re.compile(r'^mov dword ptr \[e\w\w(?:\+0x[0-9a-f]+)?\], (0x[0-9a-f]+|e\w\w)$')
        regs, pushes, codes, out = {}, [], [], []
        for ins in self.x86.disasm(self.img.read(lo, hi - lo), lo):
            t = ins.text
            m = imm.match(t)
            if m:
                regs[m.group(1)] = int(m.group(2), 16)
                if self.X.is_code(regs[m.group(1)]): codes.append(regs[m.group(1)])
                continue
            m = re.match(r'^push (-?0x[0-9a-f]+|\d+)$', t)
            if m:
                pushes.append(int(m.group(1), 0) & 0xffffffff); continue
            m = re.match(r'^push (e\w\w)$', t)
            if m:
                pushes.append(regs.get(m.group(1))); continue
            m = store.match(t)
            if m:
                v = int(m.group(1), 16) if m.group(1).startswith('0x') else regs.get(m.group(1))
                if self.X.is_code(v): codes.append(v)
                continue
            if t.startswith('call'):
                out.append((ins.va, ins.target or 0, list(reversed(pushes)), codes, regs.get('edi')))
                pushes, codes = [], []
                for r in ('eax', 'ecx', 'edx'): regs.pop(r, None)
                continue
            if t.startswith(('ret', 'jmp')):
                pushes, regs = [], {}
                continue
            m = re.match(r'^(?:xor (e\w\w), \1|\w+ (e\w\w),)', t)
            if m and not t.startswith(('cmp', 'test', 'push')): regs.pop(m.group(1) or m.group(2), None)
        return out

    def counterparts(self, names):
        """The PC functions that reference most of these names' strings, as (start, end)."""
        votes = collections.Counter()
        for n in set(names):
            for va in self.strings(n):
                for r in self.refs(va):
                    votes[self.func_start(r)] += 1
        if not votes: return []
        top = votes.most_common(1)[0][1]
        return [(a, self.func_end(a)) for a, v in votes.items() if v == top]

    def closures(self, lo, hi):
        """(table or metatable, name, C function, thunk, upvalues) of each
        lua_pushcclosure(L, thunk, n) call in lo..hi: the name is the last
        string passed since the closure before (or the one a
        lua_setfield(L, LUA_GLOBALSINDEX, name) right after it passes), the
        table the one before it, the C function the last other code address
        since then (a lua_pushlightuserfunc argument or a stored member
        function), else the thunk itself (a lua_CFunction). A call to a binder
        template (push the C function, push the name, the table in edi) is one
        too, with the template as its thunk. Also every identifier string
        passed."""
        out, strs, codes, every = [], [], [], []
        for site, tgt, args, moved, edi in self.calls(lo, hi):
            s = [self.ident(v) for v in args if self.ident(v)]
            every += s
            if len(args) >= 3 and self.X.is_code(args[1]) and args[2] in (0, 1):
                others = [c for c in codes + moved if c != args[1]]
                fn = others[-1] if others and args[2] == 1 else args[1]
                out.append([strs[-2] if len(strs) > 1 else '', strs[-1] if strs else '', fn, args[1], args[2]])
                strs, codes = [], []
            elif len(args) == 2 and self.ident(args[0]) and self.X.is_code(args[1]) and self.ident(edi):
                out.append([self.cstr(edi), self.cstr(args[0]), args[1], tgt, 1])
                every.append(self.cstr(edi))
                strs, codes = [], []
            elif len(args) == 3 and args[1] == 0xffffd8ee and self.ident(args[2]) and out and not strs:
                out[-1][:2] = ['', self.cstr(args[2])]          # lua_setfield(L, LUA_GLOBALSINDEX, name)
            else:
                strs += s
                codes += moved + [a for a in args if self.X.is_code(a)]
        return [tuple(c) for c in out], every

    def proxies(self):
        """{(table, name): C function} from the LuaFuncBinderProxy initializers:
        push table; push function; push name; mov ecx, object; call, or the
        constructor inlined: bStrNCpy(object + 8, name, 63), the function
        stored at object + 0x88, bStrNCpy(object + 0x48, table, 63)."""
        out = {}
        for m in re.finditer(rb'\x68(....)\x68(....)\x68(....)\xb9(....)\xe8', self.text, re.S):
            t, f, n, o = (struct.unpack('<I', g)[0] for g in m.groups())
            if self.ident(t) and self.ident(n) and self.X.is_code(f):
                out[(self.cstr(t), self.cstr(n))] = f
        for a, nm in self.img.SYMS.items():
            if not nm.startswith('_dynamic_initializer_for___lfbp') or nm.endswith('_SEH'): continue
            b = self.img.read(a, self.func_end(a) - a)
            cp = {struct.unpack('<I', m.group(2))[0]: struct.unpack('<I', m.group(1))[0]
                  for m in re.finditer(rb'\x6a\x3f\x68(....)\x68(....)', b, re.S)}      # destination -> source
            if len(cp) != 2: continue
            o = min(cp) - 8
            if o + 0x48 not in cp: continue
            f = None
            for m in re.finditer(rb'\xc7\x05(....)(....)', b, re.S):
                if struct.unpack('<I', m.group(1))[0] == o + 0x88: f = struct.unpack('<I', m.group(2))[0]
            if f is None: f = self.img.u32(o + 0x88)
            out[(self.cstr(cp[o + 0x48]), self.cstr(cp[o + 8]))] = f
        return out

    def registry(self, name0):
        """[(hash, name, flags, fn, field count, fields address, size)] of the
        PC's definitions table (EventSys or Hermes) holding name0: the run of
        pointers, around one to name0's definition, to definitions whose
        names start like name0 and hash to their first word."""
        u32 = self.img.u32

        def valid(d):
            n = self.cstr(u32(d + 4)) if d and u32(d + 4) else None
            return n and n[0] == name0[0] and lookup2(n) == u32(d)
        for d0 in self.find_data(struct.pack('<I', lookup2(name0))):
            if self.cstr(u32(d0 + 4)) != name0: continue
            for p0 in self.find_data(struct.pack('<I', d0)):
                lo, hi = p0, p0 + 4
                while valid(u32(lo - 4)): lo -= 4
                while valid(u32(hi)): hi += 4
                if hi - lo > 32:
                    return [struct.unpack('<7I', self.img.read(u32(p), 28)) for p in range(lo, hi, 4)]
        return None

    def message_bindables(self):
        """[(hash, post handler, bind handler)] of MessageBindable::Initialize:
        push bind; push post; push ecx; mov ecx, esp; mov dword ptr [ecx], hash."""
        out = []
        for m in re.finditer(rb'\x68(....)\x68(....)\x51\x8b\xcc\xc7\x01(....)', self.text, re.S):
            b, p, h = (struct.unpack('<I', g)[0] for g in m.groups())
            if self.X.is_code(b) and self.X.is_code(p): out.append((h, p, b))
        return out

    def arg_count(self, fn):
        """The `cmp eax, N` after a lua_CFunction's first call (lua_gettop)."""
        after = False
        for ins in self.x86.disasm(self.img.read(fn, 0x60), fn):
            if after:
                m = re.match(r'^cmp eax, (0x[0-9a-f]+)$', ins.text)
                if m: return int(m.group(1), 16)
                if ins.text == 'test eax, eax': return 0
                if ins.text.startswith('add esp'): continue
                return None
            if ins.text.startswith('call'): after = True
        return None


def event_rows(defs, read, cstr, pool, unpack):
    """events.tsv rows from definitions (hash, name, flags, fire, field count, fields, size)."""
    out = []
    for h, nm, flags, fire, nf, fp, size in defs:
        name = cstr(nm)
        fl = []
        for j in range(nf):
            th, fh, so = unpack(read(fp + 12 * j, 12))
            fl.append('%s:%s' % (pool.get(fh, '%08x' % fh), EV_TYPES.get(th, '%08x' % th)))
        if lookup2(name) != h: print('event %s: hash mismatch' % name, file=sys.stderr)
        out.append((name, flags, nf, ' '.join(fl), size, 'fire' if fire else '-'))
    return sorted(out)


def compare_pc(pc, b, ev360, ms360, pool, report):
    """What the PC binds of the 360's bindings ({key: the PC's C function, or
    None for an object or a number}), the PC's stubs, the PC's events.tsv
    rows, its MessageBindables, and what the builds bind differently, into
    report."""
    rows360 = {tuple(r[:3]): r for r in b.rows}
    pc_rows, extra = {}, []
    for label, keys in b.made.items():
        if label == 'LuaFuncBinderProxy objects':
            got = pc.proxies()
            for k in keys:
                if got.get(k[1:]): pc_rows[k] = got[k[1:]]
            extra += ['function %s.%s' % k for k in sorted(set(got) - {k[1:] for k in keys})]
            report.append('%s: %d on the 360, %d on the PC (the initializers nfs_steam.map names)' % (
                label, len(keys), len(got)))
            continue
        if label.endswith('::DoCustomLookup'):              # the names among the PC's strings
            for k in keys:
                if pc.strings(k[2]): pc_rows[k] = None
            continue
        best = None
        for span in pc.counterparts([k[2] for k in keys]):
            cl, every = pc.closures(*span)
            cl = [c for c in cl if not c[1].startswith('__')]      # CreateState: the globals' metatable
            byname = {(c[0], c[1]): c[2] for c in cl}
            score = sum(2 if (k[1], k[2]) in byname else 1 if k[2] in every else 0 for k in keys)
            if best is None or score > best[0]: best = (score, span, cl, every, byname)
        if not best:
            report.append('%s: no PC counterpart found' % label)
            continue
        score, span, cl, every, byname = best
        for k in keys:
            kind, tbl, name = k
            if kind in ('function', 'method') and (tbl, name) in byname:
                pc_rows[k] = byname[(tbl, name)]
            elif kind in ('object', 'constant') and name in every and (not tbl or tbl in every):
                pc_rows[k] = None
        mine = {(k[1], k[2]) for k in keys}
        extra += ['%s.%s' % c[:2] for c in cl if c[:2] not in mine]
        s360 = {x for k in keys for x in k[1:] if x}
        report.append('%s: %d binding%s on the 360; the PC function at %x..%x makes %d; identifier strings only '
                      'on the PC: %s' % (label, len(keys), '' if len(keys) == 1 else 's', span[0], span[1], len(cl),
                                      ', '.join(sorted(n for n in set(every) - s360 if not n.startswith('__'))) or 'none'))
    keys360 = list(rows360) + [('constant',) + c[:2] for c in b.constants]
    missing = [k for k in keys360 if k not in pc_rows]
    report.append('bindings the 360 debug build makes and the PC scan didn\'t find: %s' % (
        ', '.join('%s %s.%s' % k for k in missing) or 'none'))
    if missing:
        report.append('  of those, names missing from the PC\'s strings: %s' % (
            ', '.join('%s.%s' % k[1:] for k in missing if not pc.strings(k[2])) or 'none'))
    report.append('bindings only the PC makes: %s' % (', '.join(extra) or 'none'))
    stubs = {}
    for k, r in rows360.items():
        fn = pc_rows.get(k)
        stubs[k] = pc.X.stub_kind(fn, r[6]) if fn and r[6] and not r[4].startswith('??_9') else ''
    report.append('stubs that differ between the 360 debug build and the PC: %s' % ('; '.join(
        '%s %s.%s: 360 "%s", PC "%s"' % (k + (rows360[k][8], stubs[k])) for k in rows360
        if k in pc_rows and stubs[k] != rows360[k][8]) or 'none'))
    # events: the PC's own definitions
    reg = None
    for name0 in sorted((r[0] for r in ev360), key=lookup2)[:3]:
        reg = reg or pc.registry(name0)
    pc_ev = event_rows(reg, pc.img.read, pc.cstr, pool, lambda b_: struct.unpack('<3I', b_)) if reg else None
    if pc_ev is None:
        report.append('events: no PC registry found')
    else:
        lua = lambda rows: {r[0]: r for r in rows if r[1] & 2 and r[5] == 'fire'}      # noqa: E731
        x, p_ = lua(ev360), lua(pc_ev)
        report.append('events: %d on the 360 (%d that Lua can fire), %d on the PC (%d); Lua can fire only on the '
                      '360: %s; only on the PC: %s; definitions that differ: %s' % (
                          len(ev360), len(x), len(pc_ev), len(p_), ', '.join(sorted(set(x) - set(p_))) or 'none',
                          ', '.join(sorted(set(p_) - set(x))) or 'none',
                          ', '.join(sorted(n for n in set(x) & set(p_) if x[n] != p_[n])) or 'none'))
    # messages: the PC's MessageBindables and their argument counts
    pm = {h: pc.arg_count(post) for h, post, bind in pc.message_bindables()}
    m360 = {lookup2(r[0]): r for r in ms360}
    report.append('messages: %d M<Name> globals on the 360, %d MessageBindables on the PC; only on the 360: %s; '
                  'only on the PC (hashes): %s; argument counts that differ: %s' % (
                      len(m360), len(pm), ', '.join(sorted(m360[h][0] for h in set(m360) - set(pm))) or 'none',
                      ', '.join(sorted('%08x' % h for h in set(pm) - set(m360))) or 'none',
                      ', '.join(sorted('%s (360 %s, PC %s)' % (m360[h][0], m360[h][1], pm[h])
                                       for h in set(m360) & set(pm) if str(pm[h]) != str(m360[h][1]))) or 'none'))
    return pc_rows, stubs, pc_ev, set(pm)


def main():
    root, work = sys.argv[1:3]
    pool = names_pool(sys.argv[3:])
    os.makedirs(work, exist_ok=True)
    w = lambda n: os.path.join(work, n)  # noqa: E731

    def write(name, rows):
        with open(w(name), 'w') as f:
            for r in rows: f.write('\t'.join(str(x) for x in r) + '\n')
    im = Image(os.path.join(root, 'nfs-data', 'SpeedDebugOpt-enUS.exe'), os.path.join(root, 'nfs-data', 'SpeedDebugOpt-enUS.pdb'))
    b = Bindings(im)
    makers = ['LuaBindery::BindToGameCode', 'LuaBindery::BindCustomMethods', 'BindEnums',
              'GameCore::Lua::Runtime::CreateState']
    makers += sorted(n for n in im.by_proc if n.endswith('::BindStaticMethods'))
    for n in makers:
        b.scan(n)
    b.proxies()
    b.lookups()
    ev = events(im, pool)
    ms = messages(im, pool)
    write('uc360_bindings.tsv', b.rows)
    write('uc360_events.tsv', ev)
    write('uc360_messages.tsv', ms)
    report = ['360 debug build (nfs-data/SpeedDebugOpt-enUS.exe): %d bindings (%s), %d numbers, %d events (%d that '
              'Lua can fire), %d M<Name> globals' % (
                  len(b.rows), ', '.join('%d %s' % (v, k) for k, v in sorted(collections.Counter(r[0] for r in b.rows).items())),
                  len(b.constants), len(ev), sum(1 for r in ev if r[1] & 2 and r[5] == 'fire'), len(ms)),
              'bound twice, the second replacing the first: %s' % ('; '.join(b.rebound) or 'none')]
    pc = PC(root)
    pc_rows, stubs, pc_ev, pc_m = compare_pc(pc, b, ev, ms, pool, report)
    # what write_list.py reads: the 360's bindings with the PC's stubs, and a note where they differ
    rows = []
    for r in b.rows:
        k = tuple(r[:3])
        if k not in pc_rows:
            note = 'Not bound by the PC build.'
        elif stubs[k] != r[8]:
            note = 'Not a stub in the 360 debug build.' if not r[8] else 'In the 360 debug build it %s.' % r[8]
        else:
            note = ''
        rows.append(r[:8] + [stubs[k] if k in pc_rows else r[8], note])
    write('uc_bindings.tsv', rows)
    enum = [n[len('BindRawNumber<enum '):-1] for n in im.by_proc if n.startswith('BindRawNumber<enum ')]
    write('uc_constants.tsv', [c + (enum[0] if len(enum) == 1 else '',) for c in b.constants])
    write('uc_events.tsv', pc_ev if pc_ev else ev)
    write('uc_messages.tsv', [m for m in ms if lookup2(m[0]) in pc_m])
    with open(w('uc_builds.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    print('\n'.join(report), file=sys.stderr)


if __name__ == '__main__':
    main()
