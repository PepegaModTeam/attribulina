#!/usr/bin/env python3
"""Generate Attribulina's type schemas from the games' debug headers.

    python3 scripts/gen_schema.py [--game prostreet|carbon|undercover|mw|world] [--platform pc|ps2|wii|x360|xbox|gamecube] [--demo|--alpha]
                                  [--header H] [--enums H] [--attributes ATTRIBUTES.BIN]
                                  [--fallback F ...] [--out data/schema/<game>_<platform>.txt]

Run from the repo root. Each game's defaults are in GAMES below:
    prostreet   --header nfs-data/ProStreet08Milestone.h (X360 milestone PDB dump, has offsets).
                pc writes prostreet_pc.txt (PC, 360, Wii), ps2 prostreet_ps2.txt.
    carbon      --header nfs-data/carbon/ps3-milestone/types_pahole.txt (pahole of the PS3
                build's DWARF, has offsets; no namespaces, no enums) and --enums from the same
                build's IDA header (EBOOT.ELF.h). pc writes carbon_pc.txt (PC, PS3, Wii),
                ps2 carbon_ps2.txt (PS2, GameCube). x360 writes carbon_x360.txt for the retail
                360: the PC's types but for SteeringSensitivityParameter. That one is 20 bytes on
                the PS2, the GameCube and the 360, from scripts/carbon_ps2_x360_types.h. --platform x360 --demo writes carbon_x360_demo.txt from a late
                360 demo (older types: Most Wanted's StringKey, from its header), and
                --platform ps2 --demo carbon_ps2_demo.txt from the PS2 demo.
    undercover  pc writes undercover_pc.txt (PC, Xbox 360) for the current-gen game from
                nfs-data/SpeedDebugOpt-enUS.h (the 360 debug build: member order, no offsets),
                laid out naturally. --platform x360 --alpha writes undercover_x360_alpha.txt
                for the 360 alpha (build 161), with the types it changes from
                scripts/undercover_alpha_types.h. The Wii's and the PS2's version runs on
                Carbon's engine: Carbon's headers, then Undercover's debug header for its own
                types. wii writes undercover_wii.txt; ps2 writes undercover_ps2.txt from the
                Wii's types sized as Carbon's PS2 table has them (no PS2 files seen).
    mw          exes/gamecube/mw-vanilla/NFSMWRELEASE.h (the GameCube build's DWARF dump: no
                offsets, no namespaces), laid out naturally. pc writes mw_pc.txt (PC, Xbox, the
                360 beta), gamecube mw_gamecube.txt (GameCube, the PS2 beta: 4-byte bools).
                --platform xbox --demo and --platform ps2 --demo write mw_xbox_demo.txt and
                mw_ps2_demo.txt for the early beta demos (StringHash64 keys), with the types
                they change from scripts/mw_demo_types.h.
    world       pc writes world_pc.txt for World's final PC build. No World header is known:
                Carbon's headers first (World began as Carbon), then scripts/world/easharp_types.h
                (the 2010 beta's EASharp bindings, scripts/world/easharp.py) and
                scripts/world/world_types.h (what NFS-VltEd's World classes and the data show),
                read before them, then Undercover's debug header for what Carbon lacks.
    --attributes  attrib/databases/<platform>/<game>-vanilla/attributes.bin (its type table)

What it emits: every attribute type in the type table (except the built-in
EA::Reflection::* and Attrib::Types::* primitives) plus every struct/enum they
reach, in the line format the C++ loader reads (src/core/Schema.*):

    schema prostreet "Need for Speed: ProStreet" pc x360
    enum <Name> <size>
      <Identifier> <value>
    end
    struct <Name> <size> <align> [hint=<word>]
      <member> <type> <offset> [n=<count>] [bits=<pcBitOffset>:<bitSize>] [ptr=<target>] [count=<member>] [hint=<word>]
    end

Decisions (see the report at the end of a run for the numbers):
- Layouts. The Pro Street header is the X360 build, Carbon's the PS3 build
  (GCC, big-endian; the pahole dump gives offsets and bit positions in
  declaration order, which are the PC's). For Pro Street every attribute type it
  defines has the same size as the PC type table, and nothing these types
  reach is 16-byte aligned, so the X360 offsets are the PC offsets. The script
  recomputes each struct with PC natural alignment and compares; a struct whose
  offsets would differ is reported (none today).
- Bitfields: MSVC on PowerPC fills a storage unit from the top, x86 from the
  bottom, so the PC position is unitBits - x360BitOffset - bitSize.
- Platforms: the header names every platform the layouts are for: the PC's
  are the 360's and the Wii's for Pro Street, the PS3's and the Wii's for
  Carbon; Carbon's PS2's are the GameCube's (all checked by round trips).
- A 4-byte bool (EA::Reflection::Bool is 4 bytes in the PS2 type table, as
  its compiler makes every bool) is `bool32`. Structs holding one, directly
  or through a struct that moved, are laid out again naturally from the
  header's member order. For the PC the same layout with 1-byte bools is
  checked against the header's offsets; a struct it would move is reported.
- Base classes are inlined: the base's members are emitted at their offsets
  (single inheritance, base at 0). A gap before the first member (a vtable
  pointer) is left as a gap.
- Static members (pdbripper lists them with Offset=0x0 after the real ones) and
  overlapping members (anonymous unions) are dropped: a member is kept only if
  its offset is past the previous member's, or it shares a bitfield unit.
- Pointers: `char *` is `cstr`. Other pointers are `ptr` with their target. The
  target type is only emitted (followed) when it's a primitive, an enum, a type
  reached by value anyway, or a struct with no pointers of its own; otherwise
  the target is written as `void` (runtime pointers such as RefSpec's
  Collection*). `count=` comes from a small table of verified pairs.
- Types missing from the Pro Street header come from the fallback headers
  (no offsets: natural PC layout), looked up by exact name only, and accepted
  only when the computed size equals the PC table size; enums from a fallback
  can't be size-checked and carry a `# source:` comment. Types found nowhere are reported and left out (the
  editor treats them as raw values).
- Names: template arguments lose their spaces; anything else non-token is
  replaced with '_'.
"""

import argparse
import os
import re
import struct
import sys
from collections import OrderedDict, defaultdict

# ----------------------------------------------------------------- PC table ---


def lookup2(k, c=0xABCDEF00):
    M = 0xFFFFFFFF

    def mix(a, b, c):
        a = (a - b - c) & M; a ^= c >> 13
        b = (b - c - a) & M; b ^= (a << 8) & M
        c = (c - a - b) & M; c ^= b >> 13
        a = (a - b - c) & M; a ^= c >> 12
        b = (b - c - a) & M; b ^= (a << 16) & M
        c = (c - a - b) & M; c ^= b >> 5
        a = (a - b - c) & M; a ^= c >> 3
        b = (b - c - a) & M; b ^= (a << 10) & M
        c = (c - a - b) & M; c ^= b >> 15
        return a, b, c

    if isinstance(k, str):
        k = k.encode('latin-1')
    n = len(k); a = b = 0x9E3779B9; i = 0
    while n - i >= 12:
        a = (a + int.from_bytes(k[i:i + 4], 'little')) & M
        b = (b + int.from_bytes(k[i + 4:i + 8], 'little')) & M
        c = (c + int.from_bytes(k[i + 8:i + 12], 'little')) & M
        a, b, c = mix(a, b, c); i += 12
    c = (c + n) & M
    r = k[i:] + bytes(11)
    a = (a + int.from_bytes(r[0:4], 'little')) & M
    b = (b + int.from_bytes(r[4:8], 'little')) & M
    c = (c + (int.from_bytes(r[8:11], 'little') << 8)) & M
    return mix(a, b, c)[2]


def read_pc_types(path):
    """[(name, size)] from the DatabaseLoadData in a pack (either byte order)."""
    d = open(path, 'rb').read()
    if d[:4] != b'VPAK':
        raise SystemExit('%s: not a VPAK' % path)
    n = struct.unpack_from('<I', d, 4)[0]
    for i in range(n):
        _, binsz, vltsz, binoff, vltoff = struct.unpack_from('<5I', d, 0x10 + i * 0x14)
        b = d[binoff:binoff + binsz]; v = d[vltoff:vltoff + vltsz]
        e = '<' if v[:4] in (b'sreV', b'NpeD') else '>'
        chunks = {}; o = 0
        while o + 8 <= len(v):
            t, s = struct.unpack_from(e + '4sI', v, o)
            if s < 8: break
            chunks[(t[::-1] if e == '<' else t).decode('latin-1')] = (o, s); o += s
        eo, es = chunks['ExpN']
        do, ds = chunks['DatN']
        count = struct.unpack_from(e + 'I', v, eo + 8)[0]
        # The early Most Wanted demos: StringHash64 keys, 32-byte export
        # entries after a u32 0, 16-byte PtrN entries, and a u32 0 at
        # DatabaseLoadData + 4.
        if struct.unpack_from(e + 'I', v, eo + 12)[0] == 0 and 16 + count * 32 <= es:
            exps64 = [struct.unpack_from(e + 'QQIIII', v, eo + 16 + 32 * k) for k in range(count)]
            if exps64 and all(do <= x[4] and x[4] + x[3] <= do + ds for x in exps64):
                dbx = [x for x in exps64 if x[1] == 0x0B38846845E9C175]
                if not dbx: continue
                po, ps = chunks['PtrN']; block = None; vfix = {}
                for k in range((ps - 8) // 16):
                    fo, pt, ix, val = struct.unpack_from(e + 'IHHQ', v, po + 8 + 16 * k)
                    if pt == 2: block = ix
                    elif pt == 3 and block == 0: vfix[fo] = val
                off = dbx[0][4]
                ntypes = struct.unpack_from(e + 'I', v, off + 12)[0]
                p = vfix[off + 16]; out = []
                for k in range(ntypes):
                    z = b.index(b'\0', p)
                    out.append((b[p:z].decode('latin-1'), struct.unpack_from(e + 'I', v, off + 20 + 4 * k)[0])); p = z + 1
                return out
        # 16-byte entries (id, type, size, offset), or Most Wanted's 20 with a
        # name offset before the size; every export lies in DatN.
        exps = []
        for size in (16, 20):
            if 12 + count * size > es: break
            exps = [struct.unpack_from(e + 'II', v, eo + 12 + size * k) + struct.unpack_from(e + 'II', v, eo + 12 + size * k + size - 8)
                    for k in range(count)]
            if all(do <= x[3] and x[3] + x[2] <= do + ds for x in exps): break
        dbx = [x for x in exps if x[1] == lookup2('Attrib::DatabaseLoadData')]
        if not dbx: continue
        po, ps = chunks['PtrN']; block = None; vfix = {}
        for k in range((ps - 8) // 12):
            fo, pt, ix, val = struct.unpack_from(e + 'IHHI', v, po + 8 + 12 * k)
            if pt == 2: block = ix
            elif pt == 3 and block == 0: vfix[fo] = val
        off = dbx[0][3]
        ntypes = struct.unpack_from(e + 'I', v, off + 8)[0]
        p = vfix[off + 12]; out = []
        for k in range(ntypes):
            z = b.index(b'\0', p)
            out.append((b[p:z].decode('latin-1'), struct.unpack_from(e + 'I', v, off + 16 + 4 * k)[0])); p = z + 1
        return out
    raise SystemExit('%s: no DatabaseLoadData' % path)


# ------------------------------------------------------------ header parse ---

HDR_RE = re.compile(r'^(struct|class|union) (.+?)(?:// Size=0x([0-9a-fA-F]+) \(Id=(\d+)\))?\s*$')
ENUM_RE = re.compile(r'^enum (\S+)(?:\s*:\s*[^{]+?)?\s*(\{)?\s*$')
MEMBER_RE = re.compile(r'^\s*(?:(?:public|private|protected)\s+)?(.*?);'
                       r'(?:// Offset=0x([0-9a-fA-F]+) Size=0x([0-9a-fA-F]+)'
                       r'(?: BitOffset=0x([0-9a-fA-F]+) BitSize=0x([0-9a-fA-F]+))?)?\s*(?://.*|/\*.*)?$')


def read_lines(path):
    with open(path, 'rb') as f:
        data = f.read().decode('latin-1')
    return data.replace('\r\n', '\n').replace('\r', '\n').split('\n')


def parse_header(path, with_offsets):
    """Top-level struct/class/union and enum definitions.

    with_offsets: pdbripper dumps with `// Offset=` comments. Without them
    (Undercover's dump, IDA and MetroWerks headers) member order is all there is.
    """
    lines = read_lines(path)
    defs = defaultdict(list); enums = defaultdict(list)
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        m = HDR_RE.match(line)
        brace_same_line = line.rstrip().endswith('{')
        if m and not line.rstrip().endswith(';') and (brace_same_line or (i + 1 < n and lines[i + 1].strip() == '{')):
            kind, rest = m.group(1), m.group(2).strip()
            size = int(m.group(3), 16) if m.group(3) else None
            rest = re.sub(r'/\*.*?\*/', '', rest).rstrip(' {').strip()
            bases = []
            if ' : ' in rest:
                rest, b = rest.split(' : ', 1)
                bases = [re.sub(r'^(public|private|protected|virtual)\s+', '', x.strip()) for x in b.split(',')]
            depth = 1; j = i + 1 if brace_same_line else i + 2; members = []
            while j < n and depth > 0:
                l = lines[j]; s = l.strip()
                if s.endswith('{') and not s.startswith('}'):
                    depth += 1
                elif s.startswith('}'):
                    depth -= 1
                elif depth == 1:
                    mm = MEMBER_RE.match(l)
                    if mm and (mm.group(2) is not None or not with_offsets):
                        decl = mm.group(1).strip()
                        is_fnptr = re.search(r'\(\s*\*', decl) is not None
                        if decl and ('(' not in decl or is_fnptr) and not decl.startswith(('typedef', 'friend', 'using ')) \
                                and 'operator' not in decl and not re.match(r'^(struct|class|union|enum) \S+$', decl):
                            members.append(dict(
                                decl=decl, fnptr=is_fnptr,
                                off=int(mm.group(2), 16) if mm.group(2) else None,
                                size=int(mm.group(3), 16) if mm.group(3) else None,
                                bitoff=int(mm.group(4), 16) if mm.group(4) else None,
                                bitsize=int(mm.group(5), 16) if mm.group(5) else None))
                j += 1
            defs[rest].append(dict(kind=kind, name=rest, bases=bases, size=size, members=members, src=path, line=i + 1))
            i = j
            continue
        m = ENUM_RE.match(line)
        if m and (m.group(2) or (i + 1 < n and lines[i + 1].strip() == '{')):
            vals = []; j = i + 1 if m.group(2) else i + 2
            while j < n and not lines[j].strip().startswith('}'):
                s = re.sub(r'//.*', '', lines[j]).strip().rstrip(',').strip()
                if s:
                    k, eq, v = s.partition('=')
                    v = v.strip().rstrip('u').rstrip('U')
                    try:
                        val = int(v, 0) if eq else None
                    except ValueError:
                        val = None
                    vals.append((k.strip(), val))
                j += 1
            # implicit values; IDA writes each value as Enum::kValue
            out = []; nxt = 0; prefix = m.group(1) + '::'
            for k, v in vals:
                if v is None: v = nxt
                if v >= 0x80000000 and v <= 0xFFFFFFFF: v -= 1 << 32
                if k.startswith(prefix): k = k[len(prefix):]
                out.append((k, v)); nxt = v + 1
            enums[m.group(1)].append(dict(name=m.group(1), values=out, src=path, line=i + 1))
            i = j + 1
            continue
        i += 1
    return defs, enums


PAHOLE_HDR_RE = re.compile(r'^(struct|class|union) (.+?) \{$')
PAHOLE_MEMBER_RE = re.compile(r'^\t+(?!/\*)(.+?);\s*/\*\s*(\d+)(?::\s*(\d+))?\s+(\d+)\s*\*/')
PAHOLE_SIZE_RE = re.compile(r'/\* size: (\d+),')


def parse_pahole(path):
    """Structs from a pahole dump of DWARF types (Carbon's PS3 build): the same
    dicts parse_header makes, with offsets. pahole names a type without its
    namespace (StringKey, not Attrib::StringKey) and prints no enums. A
    bitfield's position counts from the start of its unit in declaration
    order, which is the PC's (from the low bit); it is stored in `pcbit`. (No
    Carbon attribute type has a bitfield, so no data has checked this.)
    An anonymous union keeps its first member, an anonymous struct all of
    them; a named nested one becomes a member of unknown type (raw bytes)."""
    lines = read_lines(path)
    defs = defaultdict(list)
    i, n = 0, len(lines)
    while i < n:
        m = PAHOLE_HDR_RE.match(lines[i])
        if not m:
            i += 1
            continue
        kind, rest = m.group(1), m.group(2).strip()
        bases = []
        if ' : ' in rest:
            rest, b = rest.split(' : ', 1)
            bases = [re.sub(r'^(public|private|protected|virtual)\s+', '', x.strip()) for x in b.split(',')]
        members = []; size = None; stack = []  # nested blocks: [kind, members kept so far]
        j = i + 1
        while j < n and lines[j] != '};':
            l = lines[j]; s = l.strip()
            sm = PAHOLE_SIZE_RE.search(l)
            if sm and not stack:
                size = int(sm.group(1))
            elif re.match(r'^(union|struct|class) \{$', s):
                stack.append([s.split()[0], 0])
            elif s.startswith('}') and stack:
                nk, _ = stack.pop()
                mm = PAHOLE_MEMBER_RE.match(l.replace('}', '__%s__' % nk, 1))
                if mm and not stack:
                    members.append(dict(decl=mm.group(1).strip(), fnptr=False, off=int(mm.group(2)),
                                        size=int(mm.group(4)), bitoff=None, bitsize=None, pcbit=None))
            else:
                mm = PAHOLE_MEMBER_RE.match(l)
                keep = mm is not None and not s.startswith('static ')
                if keep and stack:
                    # inside an anonymous union (first member only) or struct, at the top level
                    keep = len(stack) == 1 and (stack[0][0] == 'struct' or stack[0][1] == 0)
                    stack[-1][1] += 1
                if keep:
                    decl = mm.group(1).strip()
                    entry = dict(decl=decl, fnptr=re.search(r'\(\s*\*', decl) is not None, off=int(mm.group(2)),
                                 size=int(mm.group(4)), bitoff=None, bitsize=None, pcbit=None)
                    bm = re.search(r':\s*(\d+)$', decl)
                    if mm.group(3) is not None and bm:
                        # pahole gives the unit's offset, the bit within it and the unit's size
                        entry['bitsize'] = int(bm.group(1))
                        entry['pcbit'] = int(mm.group(3))
                        entry['bitoff'] = entry['size'] * 8 - entry['pcbit'] - entry['bitsize']
                    members.append(entry)
            j += 1
        defs[rest].append(dict(kind=kind, name=rest, bases=bases, size=size, members=members, src=path, line=i + 1,
                               pahole=True))
        i = j + 1
    return defs, {}


# ----------------------------------------------------------- declarations ---

PRIMS = {
    'bool': ('bool', 1), 'char': ('char', 1), 'signed char': ('int8', 1), 'unsigned char': ('uint8', 1),
    'short': ('int16', 2), 'short int': ('int16', 2), 'unsigned short': ('uint16', 2), 'wchar_t': ('uint16', 2),
    'int': ('int32', 4), 'long': ('int32', 4), 'unsigned int': ('uint32', 4), 'unsigned long': ('uint32', 4),
    'unsigned': ('uint32', 4), '__int64': ('int64', 8), 'long long': ('int64', 8),
    'unsigned __int64': ('uint64', 8), 'unsigned long long': ('uint64', 8),
    'float': ('float', 4), 'double': ('double', 8),
    # IDA / EA typedefs
    'Int8': ('int8', 1), 'UInt8': ('uint8', 1), 'Int16': ('int16', 2), 'UInt16': ('uint16', 2),
    'Int32': ('int32', 4), 'UInt32': ('uint32', 4), 'Int64': ('int64', 8), 'UInt64': ('uint64', 8),
    'Float': ('float', 4), 'Bool': ('bool', 1), '__int8': ('int8', 1), 'unsigned __int8': ('uint8', 1),
    '__int16': ('int16', 2), 'unsigned __int16': ('uint16', 2), '__int32': ('int32', 4),
    'unsigned __int32': ('uint32', 4), '_BYTE': ('uint8', 1), '_WORD': ('uint16', 2), '_DWORD': ('uint32', 4),
    '_QWORD': ('uint64', 8), '_BOOL1': ('bool', 1), 'BOOL': ('int32', 4), 'DWORD': ('uint32', 4),
    'WORD': ('uint16', 2), 'BYTE': ('uint8', 1), 'size_t': ('uint32', 4), 'uint': ('uint32', 4),
    # GCC's spellings (pahole) and EA's typedefs
    'short unsigned int': ('uint16', 2), 'long int': ('int32', 4), 'long unsigned int': ('uint32', 4),
    'long long int': ('int64', 8), 'long long unsigned int': ('uint64', 8), 'unsigned short int': ('uint16', 2),
    'int8': ('int8', 1), 'uint8': ('uint8', 1), 'int16': ('int16', 2), 'uint16': ('uint16', 2),
    'int32': ('int32', 4), 'uint32': ('uint32', 4), 'int64': ('int64', 8), 'uint64': ('uint64', 8),
    'int8_t': ('int8', 1), 'uint8_t': ('uint8', 1), 'int16_t': ('int16', 2), 'uint16_t': ('uint16', 2),
    'int32_t': ('int32', 4), 'uint32_t': ('uint32', 4), 'int64_t': ('int64', 8), 'uint64_t': ('uint64', 8),
}
SIZED_INTS = {('int', 1): 'int8', ('int', 2): 'int16', ('int', 4): 'int32', ('int', 8): 'int64',
              ('uint', 1): 'uint8', ('uint', 2): 'uint16', ('uint', 4): 'uint32', ('uint', 8): 'uint64'}
# Unqualified names the fallback headers use for Attrib types.
ALIASES = {'RefSpec': 'Attrib::RefSpec', 'StringKey': 'Attrib::StringKey', 'Blob': 'Attrib::Blob',
           'Key': 'uint32', 'Attrib::Key': 'uint32',
           # EA::Reflection::Text is a const char* (pahole drops the namespace)
           'Text': 'cstr', 'EA::Reflection::Text': 'cstr'}
PRIM_SIZE = {'bool': 1, 'char': 1, 'int8': 1, 'uint8': 1, 'int16': 2, 'uint16': 2, 'int32': 4, 'uint32': 4,
             'int64': 8, 'uint64': 8, 'float': 4, 'double': 8, 'cstr': 4, 'ptr': 4, 'bool32': 4}


def sanitize(name):
    name = name.strip()
    # drop spaces inside template arguments
    out = []; depth = 0
    for ch in name:
        if ch == '<': depth += 1
        elif ch == '>': depth -= 1
        if ch == ' ' and depth > 0: continue
        out.append(ch)
    name = ''.join(out)
    return re.sub(r'[^A-Za-z0-9_:<>,*&]', '_', name)


def parse_decl(decl):
    """-> dict(name, base (type text without qualifiers/pointers), ptr (depth), dims [..], bits, fnptr)"""
    d = decl.strip()
    bits = None
    m = re.match(r'^(.*?)\s*:\s*(\d+)$', d)
    if m and '::' not in d[m.start(2) - 2:m.start(2)]:
        # "unsigned int mKind:8" (not "A::B")
        if re.search(r'[A-Za-z_0-9\]]\s*:\s*\d+$', d):
            d, bits = m.group(1), int(m.group(2))
    if re.search(r'\(\s*\*', d):
        nm = re.search(r'\(\s*\*\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)', d)
        return dict(name=nm.group(1) if nm else 'fn', base='void', ptr=1, dims=[], bits=None, fnptr=True)
    dims = []
    while True:
        m = re.search(r'\[(\d*)\]\s*$', d)
        if not m: break
        dims.insert(0, int(m.group(1)) if m.group(1) else 0)
        d = d[:m.start()].rstrip()
    m = re.search(r'([A-Za-z_][A-Za-z_0-9]*)\s*$', d)
    name = m.group(1)
    t = d[:m.start()].strip()
    ptr = t.count('*') + t.count('&')
    t = t.replace('*', ' ').replace('&', ' ')
    t = re.sub(r'\b(const|volatile|struct|class|enum|union|mutable|static)\b', ' ', t)
    t = ' '.join(t.split())
    return dict(name=name, base=t, ptr=ptr, dims=dims, bits=bits, fnptr=False)


# --------------------------------------------------------------- the model ---

class Gen:
    def __init__(self, primary, fallbacks, pc_types, platform='PC', game='Pro Street', unchecked=(), natural=False, overrides=None):
        self.pdefs, self.penums = primary
        # (paths, defs, enums) whose structs and enums come before the primary
        # header's, laid out naturally: the early Most Wanted demos' changed
        # types, World's own. Each definition keeps its file in 'src'.
        self.overrides = overrides
        # A primary header without offsets or namespaces (Most Wanted's
        # MetroWerks dump): its structs are laid out naturally with the
        # platform's bools, and found by their unqualified names.
        self.natural = natural
        self.fallbacks = fallbacks          # [(path, defs, enums)]
        self.pc = OrderedDict(pc_types)     # the platform's type table (named for the PC, the first one)
        self.platform = platform            # for comments: PC, PS2
        self.game = game                    # for comments: Pro Street, Carbon
        self.unchecked = set(unchecked)     # types whose size on this platform no file shows
        # pahole names types without their namespace: the type table's
        # qualified name for each short one it has.
        short_tab = defaultdict(set)
        for name in self.pc:
            short_tab[name.split('::')[-1]].add(name)
        self.qualify = {k: next(iter(v)) for k, v in short_tab.items() if len(v) == 1 and '::' in next(iter(v))}
        self.bool32 = self.pc.get('EA::Reflection::Bool') == 4
        self.relaid = set()                 # structs laid out again for 4-byte bools
        self.structs = OrderedDict()        # name -> dict(size, align, members, hint, src, notes)
        self.enums = OrderedDict()          # name -> dict(size, values, src)
        self.report = defaultdict(list)
        self.short = defaultdict(set)       # unqualified name -> qualified names (primary)
        for name in list(self.pdefs) + list(self.penums):
            self.short[name.split('::')[-1]].add(name)
        self.state = {}                     # name -> 'busy' | 'done'
        self.field_enums = []               # (class, field, enum, why) for `field` lines

    # pick a definition
    def best_def(self, name, defs, want_size=None):
        ds = defs.get(name) or []
        if not ds and '::' in name and defs is self.pdefs:
            ds = [d for d in defs.get(name.split('::')[-1]) or [] if d.get('pahole') or self.natural]
        ds = [d for d in ds if d['members'] or (d['size'] == 0)]
        if not ds: return None
        if want_size is not None:
            for d in ds:
                if d['size'] == want_size: return d
        full = max(ds, key=lambda d: len(d['members']))
        sizes = {d['size'] for d in ds if d['members']}
        if len(sizes) > 1:
            self.report['duplicate defs with different sizes'].append((name, sorted(s for s in sizes if s is not None)))
        return full

    def resolve_name(self, base, src_defs=None, src_enums=None):
        """Map a type spelled in some header to a schema name and its kind."""
        if base in PRIMS: return ('prim', PRIMS[base][0])
        if base == 'void': return ('void', 'void')
        if base in ALIASES:
            a = ALIASES[base]
            return ('prim', a) if a in PRIM_SIZE else ('struct', a)
        if base in self.qualify and base not in self.pc:
            q = self.qualify[base]
            if q in self.penums and q not in self.pdefs: return ('enum', q)
            if self.natural and base in self.penums and base not in self.pdefs: return ('enum', q)
            return ('struct', q)
        if self.natural and base in self.penums and base not in self.pdefs and len(self.distinct_enums(base)) > 1:
            return ('unknown', base)  # one of several unrelated enums of that name: a raw value
        for defs, enums in ([(self.pdefs, self.penums)] + ([(src_defs, src_enums)] if src_defs is not None else [])):
            if base in enums and base not in defs: return ('enum', base)
            if base in defs: return ('struct', base)
        # unqualified (IDA / MetroWerks headers): map to the primary's qualified name
        cands = self.short.get(base)
        if cands and len(cands) == 1:
            q = next(iter(cands))
            return ('enum', q) if q in self.penums and q not in self.pdefs else ('struct', q)
        # An override's member of a type the type table has, which a fallback
        # header defines (World's 48-byte Curve is Undercover's).
        if self.overrides and src_defs is self.overrides[1] and base in self.pc and self.struct_in_fallbacks(base):
            return ('struct', base)
        return ('unknown', base)

    def distinct_enums(self, short):
        """The primary's enums named `short`, one per set of values."""
        seen = OrderedDict()
        for e in self.penums.get(short) or []:
            seen.setdefault(tuple(e['values']), e)
        return list(seen.values())

    def natural_enum(self, name):
        """A qualified name's enum in a header without namespaces: the one of
        that short name, or among several, the one whose value names a
        fallback header's same-named enum shares most of."""
        cands = self.distinct_enums(name.split('::')[-1])
        if len(cands) <= 1:
            return cands[0] if cands else None
        ref = None
        for path, fdefs, fenums in self.fallbacks:
            if name in fenums:
                ref = {k for k, _ in fenums[name][0]['values']}
                break
        if not ref:
            self.report['ambiguous enums (left out)'].append((name, len(cands)))
            return None
        best = max(cands, key=lambda e: len(ref & {k for k, _ in e['values']}))
        if not ref & {k for k, _ in best['values']}:
            self.report['ambiguous enums (left out)'].append((name, len(cands)))
            return None
        self.report['enums picked by their value names'].append((name, len(cands)))
        return best

    def enum_for(self, name, src_enums=None):
        if name in self.enums: return True
        e = None; src = None
        if self.overrides and name in self.overrides[2]:
            e = self.overrides[2][name][0]
            self.enums[name] = dict(size=4, values=e['values'], src=e['src'], override=True)
            self.report['enums from overrides'].append(name)
            return True
        if name in self.penums and not (self.natural and len(self.distinct_enums(name)) > 1):
            e = self.penums[name][0]
        elif self.natural and self.natural_enum(name) is not None:
            e = self.natural_enum(name)
        else:
            # Exact names only: a short-name match ("Status") picks up some
            # other game's unrelated enum.
            for path, fdefs, fenums in self.fallbacks:
                if name in fenums:
                    e = fenums[name][0]; src = path
                    break
        if e is None: return False
        self.enums[name] = dict(size=4, values=e['values'], src=src)
        if src: self.report['enums from fallback headers'].append((name, os.path.basename(src)))
        return True

    def struct_for(self, name, want_size=None, ctx_defs=None, ctx_enums=None):
        """Emit a struct (and what it needs). Returns (size, align) or None."""
        if name in self.structs:
            s = self.structs[name]; return s['size'], s['align']
        if self.state.get(name) == 'busy':
            return None
        if self.overrides:
            opaths, odefs, oenums = self.overrides
            for key in (name, name.split('::')[-1]):
                cand = self.best_def(key, odefs)
                if cand is None: continue
                self.state[name] = 'busy'
                lay = self.layout(name, cand, odefs, oenums, natural=True)
                if lay is None or (want_size is not None and lay['size'] != want_size):
                    self.state.pop(name, None)
                    if lay is not None: self.report['override size mismatch (left out)'].append((name, lay['size'], want_size))
                    break
                self.state[name] = 'done'
                lay['src'] = cand['src']
                self.report['structs from overrides'].append(name)
                self.structs[name] = lay
                return lay['size'], lay['align']
        d = self.best_def(name, self.pdefs, want_size)
        src_defs, src_enums, src = self.pdefs, self.penums, None
        if d is not None and want_size is not None and d['size'] != want_size and self.bool32 and not self.natural:
            # 4-byte bools may make up the difference: the header's struct, laid out again.
            self.state[name] = 'busy'
            lay = self.layout(name, d, src_defs, src_enums, natural=False)
            if lay is not None and lay['size'] == want_size:
                self.state[name] = 'done'
                lay['src'] = None
                self.structs[name] = lay
                return lay['size'], lay['align']
            self.state.pop(name, None)
        if d is None or (want_size is not None and d['size'] is not None and d['size'] != want_size):
            prim_d = d; d = None
            # Exact names, and an Attrib:: type's own name (MW's header has
            # StringKey without the namespace).
            keys = (name,) + ((name.split('::')[-1],) if name.startswith('Attrib::') else ())
            for path, fdefs, fenums in self.fallbacks:
                for key in keys:
                    cand = self.best_def(key, fdefs)
                    if cand is None: continue
                    if all(re.match(r'^\w+\s+(gap|field_)[0-9A-Fa-f]+', m['decl']) for m in cand['members']):
                        continue  # IDA placeholder (gap0[4]): no information
                    self.state[name] = 'busy'
                    lay = self.layout(name, cand, fdefs, fenums, natural=True)
                    self.state.pop(name, None)
                    if lay and (want_size is None or lay['size'] == want_size):
                        d, src_defs, src_enums, src = cand, fdefs, fenums, path
                        break
                if d: break
            if d is None:
                if prim_d is not None and want_size is not None:
                    self.report['size mismatch (left out)'].append((name, prim_d['size'], want_size))
                return None
        self.state[name] = 'busy'
        lay = self.layout(name, d, src_defs, src_enums, natural=src is not None or self.natural)
        self.state[name] = 'done'
        if lay is None: return None
        lay['src'] = src
        if src: self.report['structs from fallback headers'].append((name, os.path.basename(src)))
        self.structs[name] = lay
        return lay['size'], lay['align']

    def member_type(self, pd, src_defs, src_enums, size_hint):
        """-> (type token, elem size, align, extra dict) or None"""
        if pd['fnptr']:
            return 'ptr', 4, 4, dict(ptr='void')
        if pd['ptr'] > 0:
            if pd['ptr'] == 1 and pd['base'] in ('char', 'const char'):
                return 'cstr', 4, 4, {}
            kind, tname = self.resolve_name(pd['base'], src_defs, src_enums)
            target = 'void'
            if pd['ptr'] == 1:
                if kind == 'prim': target = self.prim(tname)
                elif kind == 'enum' and self.enum_for(tname): target = tname
                elif kind == 'struct' and self.followable(tname, src_defs):
                    if self.struct_for(tname, self.pc.get(tname)) is not None: target = tname
            return 'ptr', 4, 4, dict(ptr=target)
        kind, tname = self.resolve_name(pd['base'], src_defs, src_enums)
        if kind == 'prim':
            tname = self.prim(tname)
            m = re.match(r'^(u?int)\d+$', tname)
            if m and size_hint and size_hint != PRIM_SIZE[tname] and (m.group(1), size_hint) in SIZED_INTS:
                tname = SIZED_INTS[(m.group(1), size_hint)]  # the header's own size (`long` on another compiler)
            return tname, PRIM_SIZE[tname], PRIM_SIZE[tname], {}
        if kind == 'enum':
            if self.enum_for(tname): return tname, 4, 4, {}
            return 'int32', 4, 4, {}
        if kind == 'struct':
            r = self.struct_for(tname, self.pc.get(tname) if tname in self.pc else None)
            if r: return tname, r[0], r[1], {}
        # unknown or unusable: a primitive of the right size
        self.report['unresolved member types (as raw)'].append((pd['base'], size_hint))
        if size_hint in (1, 2, 4, 8):
            t = {1: 'uint8', 2: 'uint16', 4: 'uint32', 8: 'uint64'}[size_hint]
            return t, size_hint, size_hint, {}
        return 'uint8', 1, 1, dict(rawn=size_hint or 0)

    def prim(self, t):
        return 'bool32' if t == 'bool' and self.bool32 else t

    def followable(self, tname, src_defs):
        if tname in self.structs or tname in self.pc: return True
        d = self.best_def(tname, self.pdefs) or (self.best_def(tname, src_defs) if src_defs else None)
        if d is None: return False
        for m in d['members']:
            pd = parse_decl(m['decl'])
            if pd['ptr'] and not (pd['ptr'] == 1 and pd['base'] == 'char'): return False
        return True

    def layout(self, name, d, src_defs, src_enums, natural):
        members = []; cur = 0; align = 1
        last_off = -1; last_unit = None
        raw = d['members']
        # bases first (inlined)
        for b in d['bases']:
            bname = sanitize(re.sub(r'\b(struct|class)\b', '', b).strip())
            r = self.struct_for(bname)
            if r is None:
                self.report['unresolved bases'].append((name, bname)); continue
            bs = self.structs[bname]
            for bm in bs['members']:
                members.append(dict(bm))
            cur = max(cur, r[0]); align = max(align, r[1]); last_off = cur - 1
        if not natural and not d.get('pahole'):
            # Static members come with Offset=0x0; a `const` one at an offset a
            # non-const member also uses is static, never data.
            used = {}
            for m in raw:
                if m['bitoff'] is None and not m['decl'].startswith('const '):
                    used.setdefault(m['off'], True)
            raw = [m for m in raw if not (m['decl'].startswith('const ') and m['bitoff'] is None
                                          and (m['off'] in used or m['off'] == 0))]
        for m in raw:
            pd = parse_decl(m['decl'])
            if not natural:
                off = m['off']
                if m['bitoff'] is None and off <= last_off and not (off == 0 and last_off == -1):
                    continue  # static member or overlapping union member
                if m['bitoff'] is not None and last_unit is not None and off < last_unit[0]:
                    continue
            n = 1
            for dim in pd['dims']: n *= max(dim, 1)
            elem_hint = (m['size'] // n) if (m['size'] and n) else None
            mt = self.member_type(pd, src_defs, src_enums, elem_hint)
            if mt is None: continue
            tok, esz, eal, extra = mt
            if extra.get('rawn'):
                n *= extra.pop('rawn')
            if natural:
                if pd['bits'] is not None and last_unit and last_unit[2] == esz and last_unit[1] + pd['bits'] <= esz * 8:
                    off = last_unit[0]; bitpos = last_unit[1]
                    last_unit = (off, bitpos + pd['bits'], esz)
                else:
                    off = (cur + eal - 1) // eal * eal
                    bitpos = 0
                    last_unit = (off, pd['bits'], esz) if pd['bits'] is not None else None
                entry = dict(name=pd['name'], type=tok, off=off, n=n, extra=extra)
                if pd['bits'] is not None: entry['bits'] = (bitpos, pd['bits'])
                cur = max(cur, off + esz * n)
            else:
                entry = dict(name=pd['name'], type=tok, off=off, n=n, extra=extra)
                if m['bitoff'] is not None:
                    unit_bits = esz * 8
                    entry['bits'] = (unit_bits - m['bitoff'] - m['bitsize'], m['bitsize'])
                    last_unit = (off, 0, esz)
                else:
                    last_unit = None
                cur = max(cur, off + esz * n)
                last_off = off
            align = max(align, eal)
            members.append(entry)
        size = d['size'] if (not natural and d['size'] is not None) else (cur + align - 1) // align * align
        if not natural and self.bool32 and any(e['type'] == 'bool32' or e['type'] in self.relaid for e in members):
            members, size, align = self.relayout(members)
            self.relaid.add(name)
        elif not natural:
            # PC natural layout check: same offsets?
            chk = 0; ok = True; cal = 1
            for e in members:
                if 'bits' in e and e['bits'][0] != 0 and e is not members[0]:
                    continue
                esz = self.size_of(e['type'])
                eal = self.align_of(e['type'])
                cal = max(cal, eal)
                want = (chk + eal - 1) // eal * eal
                if e['off'] < want and e['off'] != 0:
                    ok = False
                chk = max(chk, e['off'] + esz * e['n'])
            if not ok:
                self.report['offsets that PC alignment would move'].append(name)
            if any(e['type'] == 'bool' for e in members):
                # The rule the 4-byte bool layout uses must give the header's offsets here.
                again, _, _ = self.relayout(members)
                if [e['off'] for e in again] != [e['off'] for e in members]:
                    self.report['bool structs the natural layout would move'].append(name)
        return dict(size=size, align=align, members=members, hint=None)

    def relayout(self, members):
        """The members at natural offsets, in order: (members, size, align).
        Bitfields that shared a unit still do; a gap before the first member
        (a vtable pointer) stays."""
        out = []; cur = members[0]['off'] if members else 0; align = 1; prev = None
        for e in members:
            e = dict(e)
            esz, eal = self.size_of(e['type']), self.align_of(e['type'])
            if 'bits' in e and prev is not None and prev[2] and prev[0] == e['off']:
                off = prev[1]
            else:
                off = (cur + eal - 1) // eal * eal
            prev = (e['off'], off, 'bits' in e)
            e['off'] = off
            cur = max(cur, off + esz * e['n'])
            align = max(align, eal)
            out.append(e)
        return out, (cur + align - 1) // align * align, align

    def size_of(self, t):
        if t in PRIM_SIZE: return PRIM_SIZE[t]
        if t in self.enums: return self.enums[t]['size']
        if t in self.structs: return self.structs[t]['size']
        return 0

    def align_of(self, t):
        if t in PRIM_SIZE: return PRIM_SIZE[t]
        if t in self.enums: return 4
        if t in self.structs: return self.structs[t]['align']
        return 1

    # --------------------------------------------------------------- run ---

    def run(self):
        for name, size in self.pc.items():
            if name.startswith(('EA::Reflection::', 'Attrib::Types::')):
                continue
            is_enum = (name in self.penums and name not in self.pdefs) or \
                (self.natural and self.natural_enum(name) is not None and not self.best_def(name, self.pdefs)) or \
                (self.overrides is not None and name in self.overrides[2] and name not in self.overrides[1])
            if is_enum or (name not in self.pdefs and size == 4 and self.enum_in_fallbacks(name)
                           and not self.struct_in_fallbacks(name)):
                if self.enum_for(name):
                    self.report['attribute enums'].append(name)
                    continue
            r = self.struct_for(name, None if name in self.unchecked else size)
            if r is not None and name in self.unchecked:
                self.report['attribute structs of unverified size'].append((name, r[0]))
                continue
            if r is None:
                if self.enum_for(name):
                    self.report['attribute enums'].append(name)
                else:
                    self.report['attribute types not described'].append((name, size))
                continue
            if r[0] != size:
                self.report['attribute size mismatch'].append((name, r[0], size))
            else:
                self.report['attribute structs'].append(name)
        # A struct the header gives another size (Most Wanted's 56-byte
        # Attrib::Definition is not its 16-byte field definition) describes
        # something else: left out unless another struct holds one.
        for name, got, want in self.report.get('attribute size mismatch', []):
            used = any(m['type'] == name for o, s in self.structs.items() if o != name for m in s['members'])
            if not used and name in self.structs:
                del self.structs[name]
                self.report['attribute size mismatch (left out)'].append(name)
        for (cls, field), (enum, why) in self.FIELD_ENUMS.items():
            if self.enum_for(enum):
                self.field_enums.append((cls, field, enum, why))
                self.report['field enums'].append('%s.%s' % (cls, field))
            else:
                self.report['field enums not found'].append('%s.%s: %s' % (cls, field, enum))
        self.apply_hints_and_counts()

    def enum_in_fallbacks(self, name):
        return any(name in fe for _, _, fe in self.fallbacks)

    def struct_in_fallbacks(self, name):
        return any(name in fd for _, fd, _ in self.fallbacks)

    # Verified count pairs for pointer members (struct, pointer member) -> count
    # member, checked against the retail vaults (every fixup's block size).
    COUNTS = {
        ('VariableArray', 'mArray'): 'mLength',
        ('DynamicSizeArray', 'mArray'): 'mLength',
        ('Attrib::Blob', 'mData'): 'mSize',
        ('FEPartData', 'autosculptCamera1'): 'mCam1Length',
        ('FEPartData', 'autosculptCamera2'): 'mCam2Length',
        ('FEPartData', 'autosculptCamera3'): 'mCam3Length',
    }
    # Pointer targets the header spells as void.
    TARGETS = {('Attrib::Blob', 'mData'): 'uint8'}
    # GStringHash is left out: no retail value shows which hash it holds.
    KEY_STRUCTS = {'UCrc32', 'GCollectionKey', 'Attrib::Key', 'DefinitionKey', 'GameCore::StringKey'}
    # Members whose names don't say they hold a StringHash32.
    MEMBER_HINTS = {('GameCore::KeyValuePair', 'KeyHash'): 'key'}
    BINKEY_STRUCTS = {'type_bStringHash'}
    VEC_TYPES = {'UMath::Vector3', 'UMath::Vector4', 'bVector3', 'bVector4', 'UMath::Vector2', 'bVector2'}

    # Fields the type table gives a plain integer type that the game reads as
    # an enum: (class, field) -> (enum, why). Written as `field` lines.
    FIELD_ENUMS = {
        ('light_flares_cg', 'flare_texture'): (
            'eLightFlareTextures',
            'UpdateLightFlareParameters copies it into eLightFlareParams::TextureID (MW GameCube 0x800fa1a0),\n'
            'which picks a texture from eLightFlareTextureNameHashes.'),
    }

    def apply_hints_and_counts(self):
        for sname, s in self.structs.items():
            if sname in self.KEY_STRUCTS: s['hint'] = 'key'
            if sname in self.BINKEY_STRUCTS: s['hint'] = 'binkey'
            names = {m['name'] for m in s['members']}
            for m in s['members']:
                low = m['name'].lower()
                if m['type'] == 'ptr':
                    t = self.TARGETS.get((sname, m['name']))
                    if t: m['extra']['ptr'] = t
                    c = self.COUNTS.get((sname, m['name']))
                    if c is None:
                        # mArray / mLength style pairs next to each other, or N<x> / <x>
                        for cand in (m['name'].replace('Array', 'Length'), 'mNum' + m['name'].lstrip('m'),
                                     'Num' + m['name'], 'Num' + m['name'].lstrip('m'), m['name'] + 'Count'):
                            if cand != m['name'] and cand in names:
                                c = cand; break
                    if c: m['extra']['count'] = c
                if any(w in low for w in ('colour', 'color', 'tint', 'rgb')):
                    if m['type'] in self.VEC_TYPES or (m['type'] == 'float' and m['n'] in (3, 4)):
                        m['extra']['hint'] = 'colour'
                if m['type'] in ('uint32', 'uint64') and (low.endswith('key') or low.endswith('hash32')):
                    m['extra']['hint'] = 'key'
                if (sname, m['name']) in self.MEMBER_HINTS:
                    m['extra']['hint'] = self.MEMBER_HINTS[(sname, m['name'])]

    def emit(self, out, game, title, platforms, note=None):
        w = out.write
        w('# Attribulina type schema, generated by scripts/gen_schema.py. Do not edit by hand:\n')
        w('# rerun the script (see its docstring for the decisions it makes).\n')
        if note:
            for line in note.split('\n'): w('# %s\n' % line)
        w('schema %s "%s" %s\n\n' % (game, title, ' '.join(platforms)))
        for name, e in self.enums.items():
            if e.get('override'): w('# source: %s\n' % os.path.basename(e['src']))
            elif e['src']: w('# source: %s (not in the %s header; values unverified for %s)\n' % (os.path.basename(e['src']), self.game, self.game))
            w('enum %s %d\n' % (sanitize(name), e['size']))
            for k, v in e['values']:
                w('  %s %d\n' % (sanitize(k), v))
            w('end\n\n')
        for name, s in self.structs.items():
            if s.get('src'):
                w('# source: %s (natural %s layout; size matches the %s type table)\n'
                  % (os.path.basename(s['src']), self.platform, self.platform))
            pcs = self.pc.get(name)
            if name in self.unchecked:
                w('# unverified: no %s files seen; laid out with its bools (another platform\'s is %d bytes)\n'
                  % (self.platform, pcs))
            elif pcs is not None and pcs != s['size']:
                w('# MISMATCH: %s type table says %d bytes\n' % (self.platform, pcs))
            w('struct %s %d %d%s\n' % (sanitize(name), s['size'], s['align'], (' hint=' + s['hint']) if s['hint'] else ''))
            for m in s['members']:
                parts = ['  %s %s %d' % (m['name'], sanitize(m['type']), m['off'])]
                if m['n'] != 1: parts.append('n=%d' % m['n'])
                if 'bits' in m: parts.append('bits=%d:%d' % m['bits'])
                ex = m['extra']
                if 'ptr' in ex: parts.append('ptr=%s' % sanitize(ex['ptr']))
                if 'count' in ex: parts.append('count=%s' % ex['count'])
                if 'hint' in ex: parts.append('hint=%s' % ex['hint'])
                w(' '.join(parts) + '\n')
            w('end\n\n')
        for cls, field, enum, why in self.field_enums:
            for line in ('%s.%s: %s' % (cls, field, why)).split('\n'): w('# %s\n' % line)
            w('field %s %s %s\n\n' % (cls, field, sanitize(enum)))


# Current-gen Undercover (PC, 360) isn't Carbon's engine: its own debug header.
UNDERCOVER_CURRENT_GEN = dict(
    header='nfs-data/SpeedDebugOpt-enUS.h', enums=None, natural=True, overrides=None,
    fallback=['nfs-data/ProStreet08Milestone.h', 'nfs-data/carbon/ps3-milestone/EBOOT.ELF.h',
              'exes/gamecube/mw-vanilla/NFSMWRELEASE.h'],
    same={'pc': ['x360']},
    # u8 counts, 2 to 4 elements in the data
    counts={('FEQuickUpgrade', 'Packages'): 'mPackageLength',
            ('FEAutosculptAliasing::Alias', 'sliders'): 'slidersLength'})

# What each game's schema is made from. `same` lists the platforms whose type
# layouts are another's (checked by the round-trip tests). `demos` and
# `alphas` name a platform's demo and alpha folders (--demo, --alpha), with
# `demo_overrides`/`alpha_overrides` the header of the types they change, and
# `retail_overrides` a platform's for its retail files (neither flag).
GAMES = {
    'prostreet': dict(
        title='Need for Speed: ProStreet', name='Pro Street', folder='prostreet-vanilla',
        header='nfs-data/ProStreet08Milestone.h', enums=None,
        fallback=['nfs-data/SpeedDebugOpt-enUS.h', 'nfs-data/carbon/ps3-milestone/EBOOT.ELF.h',
                  'exes/gamecube/mw-vanilla/NFSMWRELEASE.h'],
        same={'pc': ['x360', 'wii']}),
    'carbon': dict(
        title='Need for Speed: Carbon', name='Carbon', folder='carbon-vanilla',
        header='nfs-data/carbon/ps3-milestone/types_pahole.txt', enums='nfs-data/carbon/ps3-milestone/EBOOT.ELF.h',
        fallback=['nfs-data/carbon/ps3-milestone/EBOOT.ELF.h', 'exes/gamecube/mw-vanilla/NFSMWRELEASE.h',
                  'nfs-data/SpeedDebugOpt-enUS.h'],
        same={'pc': ['ps3', 'wii'], 'ps2': ['gamecube']},
        # The retail 360's type table is the PC's but for the PS2's and the
        # GameCube's 20-byte SteeringSensitivityParameter, which no header lays
        # out: carbon_ps2.txt and carbon_x360.txt take it from
        # scripts/carbon_ps2_x360_types.h (not the demos' schemas).
        retail_overrides={'ps2': ['scripts/carbon_ps2_x360_types.h'], 'x360': ['scripts/carbon_ps2_x360_types.h']},
        note={'x360': 'Carbon\'s retail 360: the PC\'s types (the PS3 milestone\'s), but for the 20-byte\n'
                      'SteeringSensitivityParameter it shares with the PS2 and the GameCube, from scripts/carbon_ps2_x360_types.h:\n'
                      'the PC\'s without its two dead zones, read from the files. The 360\'s code doesn\'t read that type.'},
        # --demo: the PS2 demo, an older build (carbon_ps2_demo.txt), and a 360
        # demo made after release, whose types are older still (Most Wanted's
        # 16-byte StringKey; NFSMWRELEASE.h has it): carbon_x360_demo.txt.
        demos={'ps2': 'carbon-demo', 'x360': 'carbon-latedemo'}),
    # The Wii's Undercover runs on Carbon's engine: Carbon's types, then
    # Undercover's (360 debug) header for the ones Carbon lacks. The
    # current-gen game (PC, 360) has its own types: `platforms` below.
    'undercover': dict(  # the Wii's and the PS2's
        title='Need for Speed: Undercover', name='Undercover', folder='undercover-vanilla',
        header='nfs-data/carbon/ps3-milestone/types_pahole.txt', enums='nfs-data/carbon/ps3-milestone/EBOOT.ELF.h',
        fallback=['nfs-data/SpeedDebugOpt-enUS.h', 'nfs-data/carbon/ps3-milestone/EBOOT.ELF.h',
                  'exes/gamecube/mw-vanilla/NFSMWRELEASE.h'],
        same={},
        # No PS2 files seen. The PS2's is the Wii's game: its types, with the
        # sizes of Carbon's PS2 table (4-byte bools) where Carbon has the type.
        tables={'ps2': ['attrib/databases/ps2/carbon-vanilla', 'attrib/databases/wii/undercover-vanilla']},
        # Its own types, read before Carbon's (eLightFlareTextures has the sun's textures).
        overrides=['scripts/undercover_wii_types.h'],
        note={'ps2': 'No PS2 files seen: the types are the Wii\'s, sized as in Carbon\'s PS2 type table where it has\n'
                     'them. The others are laid out with 4-byte bools, unchecked; the editor applies a struct only\n'
                     'where its size matches the files\' type table.',
              'pc': 'Current-gen Undercover. SpeedDebugOpt-enUS.h is the 360 debug build\'s (its SchemaVersion() is\n'
                    'the retail 360 Vers); laid out naturally, its structs match the offsets of that build\'s PDB. The\n'
                    'PC\'s and the 360\'s types have the same sizes, no bitfields and a 12-byte Vector3.',
              'x360_alpha': 'Undercover\'s 360 alpha, build 161: retail\'s types (SpeedDebugOpt-enUS.h, laid out naturally),\n'
                            'and the seven it sizes differently from scripts/undercover_alpha_types.h, read from its files.\n'
                            'Each struct has the size of the alpha\'s type table.'},
        # Settings a platform overrides. Current-gen Undercover (PC, 360, and
        # likely the PS3, whose files no one has seen) isn't Carbon's engine.
        platforms={'pc': UNDERCOVER_CURRENT_GEN, 'x360': UNDERCOVER_CURRENT_GEN},
        # --alpha: the 360 alpha (build 161), whose changed types come from
        # scripts/undercover_alpha_types.h (undercover_x360_alpha.txt).
        alphas={'x360': 'undercover-alpha161'},
        alpha_overrides='scripts/undercover_alpha_types.h'),
    # Most Wanted: the GameCube build's DWARF dump (MetroWerks; no offsets, no
    # namespaces), laid out naturally with each platform's bools. Every
    # platform's type table has the same types and sizes but for the bool
    # (4 bytes on the GameCube and the PS2). The 360's and the PS2's files are
    # betas. Headers with namespaces pick among its same-named enums.
    'mw': dict(
        title='Need for Speed: Most Wanted', name='Most Wanted', folder='mw-vanilla',
        header='exes/gamecube/mw-vanilla/NFSMWRELEASE.h', enums=None, natural=True,
        fallback=['nfs-data/mw/gc-release/mw_gc_july8.h', 'nfs-data/ProStreet08Milestone.h',
                  'nfs-data/carbon/ps3-milestone/EBOOT.ELF.h'],
        same={'pc': ['xbox', 'x360'], 'gamecube': ['ps2']},
        folders={'x360': 'mw-beta', 'ps2': 'mw-beta'},
        # --demo: the early PS2 and Xbox beta demos (StringHash64 keys, wider
        # RefSpecs), whose changed types come from scripts/mw_demo_types.h.
        demos={'ps2': 'mw-early-opm-betademo', 'xbox': 'mw-early-oxm-betademo'},
        demo_overrides='scripts/mw_demo_types.h'),
    # World began as Carbon and took features from Undercover around 2010; no
    # header of any World build is known. Carbon's types first, then World's
    # own (scripts/world/: the 2010 beta's EASharp bindings, and the types
    # NFS-VltEd and the data describe), then Undercover's for what Carbon lacks.
    'world': dict(
        title='Need for Speed: World', name='World', folder='world-final',
        header='nfs-data/carbon/ps3-milestone/types_pahole.txt', enums='nfs-data/carbon/ps3-milestone/EBOOT.ELF.h',
        fallback=['nfs-data/SpeedDebugOpt-enUS.h', 'nfs-data/carbon/ps3-milestone/EBOOT.ELF.h',
                  'nfs-data/ProStreet08Milestone.h', 'exes/gamecube/mw-vanilla/NFSMWRELEASE.h'],
        overrides=['scripts/world/easharp_types.h', 'scripts/world/world_types.h'],
        # The betas' types the final build doesn't have; a name both have takes
        # the final build's size (the 2009 beta's IndexedBarrierName is 2 bytes).
        extra_tables=['attrib/databases/pc/world-2010-beta', 'attrib/databases/pc/world-2009-beta'],
        same={},
        note={'pc': 'World on PC: the final build and its 2009 and 2010 betas. No header of any World build is known:\n'
                    'Carbon\'s types (it began as Carbon), then World\'s own from scripts/world/ (the 2010 beta\'s EASharp\n'
                    'bindings, and what NFS-VltEd\'s World classes and the data show), then Undercover\'s for the types\n'
                    'Carbon lacks. Each struct has the size of the final build\'s type table, or of a beta\'s for the\n'
                    'types the final build lacks.'}),
}


def find_attributes(folder):
    for f in sorted(os.listdir(folder)):
        if f.lower() == 'attributes.bin':
            return os.path.join(folder, f)
    raise SystemExit('%s: no attributes.bin' % folder)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', default='prostreet', choices=sorted(GAMES))
    ap.add_argument('--platform', default='pc', choices=['pc', 'ps2', 'wii', 'x360', 'xbox', 'gamecube'])
    ap.add_argument('--header')
    ap.add_argument('--enums')
    ap.add_argument('--attributes')
    ap.add_argument('--fallback', nargs='*')
    ap.add_argument('--out')
    ap.add_argument('--title')
    ap.add_argument('--demo', action='store_true', help="the game's demo files for the platform (<game>_<platform>_demo.txt)")
    ap.add_argument('--alpha', action='store_true', help="the game's alpha files for the platform (<game>_<platform>_alpha.txt)")
    args = ap.parse_args()
    build = 'demo' if args.demo else 'alpha' if args.alpha else None
    cfg = GAMES[args.game]
    cfg = dict(cfg, **cfg.get('platforms', {}).get(args.platform, {}))
    if cfg.get('counts'):
        Gen.COUNTS = {**Gen.COUNTS, **cfg['counts']}
    args.header = args.header or cfg['header']
    args.enums = args.enums or cfg['enums']
    args.fallback = cfg['fallback'] if args.fallback is None else args.fallback
    args.title = args.title or cfg['title']
    if build:
        folder = cfg.get(build + 's', {}).get(args.platform)
        if not folder: raise SystemExit('no %s %s files for %s' % (args.game, build, args.platform))
        platform_dir = '360' if args.platform == 'x360' else args.platform
        args.attributes = args.attributes or find_attributes('attrib/databases/%s/%s' % (platform_dir, folder))

    args.out = args.out or 'data/schema/%s_%s%s.txt' % (args.game, args.platform, '_' + build if build else '')
    unchecked = []
    tables = cfg.get('tables', {}).get(args.platform)
    if tables and not args.attributes:
        # The last table names the types; each takes its size from the first that has it.
        found = [OrderedDict(read_pc_types(find_attributes(t))) for t in tables]
        pc = []
        for name, size in found[-1].items():
            src = next((t for t in found if name in t), found[-1])
            if src is found[-1] and len(found) > 1: unchecked.append(name)
            pc.append((name, src[name]))
    else:
        folder = cfg.get('folders', {}).get(args.platform, cfg['folder'])
        platform_dir = '360' if args.platform == 'x360' else args.platform
        args.attributes = args.attributes or find_attributes('attrib/databases/%s/%s' % (platform_dir, folder))
        pc = read_pc_types(args.attributes)
        # Other builds' type tables, for the types this one lacks (World's betas).
        for t in cfg.get('extra_tables', []):
            have = {n for n, _ in pc}
            pc += [(n, size) for n, size in read_pc_types(find_attributes(t)) if n not in have]
    if cfg.get('natural'):
        primary = parse_header(args.header, with_offsets=False)
    elif open(args.header, 'rb').read(4096).find(b'/* size: ') >= 0 or args.header.endswith('pahole.txt'):
        primary = parse_pahole(args.header)
    else:
        primary = parse_header(args.header, with_offsets=True)
    if args.enums:
        # pahole prints no enums: the same build's IDA header has them.
        primary = (primary[0], parse_header(args.enums, with_offsets=False)[1])
    fallbacks = []
    for f in args.fallback:
        if os.path.exists(f):
            fd, fe = parse_header(f, with_offsets=False)
            fallbacks.append((f, fd, fe))
    overrides = None
    paths = cfg.get('overrides') or ([cfg[build + '_overrides']] if build and cfg.get(build + '_overrides') else [])
    if not build:
        paths = list(paths) + cfg.get('retail_overrides', {}).get(args.platform, [])
    if paths:
        odefs, oenums = defaultdict(list), defaultdict(list)
        for path in paths:
            fd, fe = parse_header(path, with_offsets=False)
            for k, v in fd.items(): odefs[k].extend(v)
            for k, v in fe.items(): oenums[k].extend(v)
        overrides = (paths, odefs, oenums)
    g = Gen(primary, fallbacks, pc, {'pc': 'PC', 'ps2': 'PS2', 'wii': 'Wii', 'x360': '360', 'xbox': 'Xbox', 'gamecube': 'GameCube'}[args.platform],
            cfg['name'], unchecked, natural=cfg.get('natural', False), overrides=overrides)
    g.run()
    os.makedirs(os.path.dirname(args.out) or '.', exist_ok=True)
    with open(args.out, 'w', newline='\n') as out:
        g.emit(out, args.game, args.title, [args.platform] + ([] if build else cfg['same'].get(args.platform, [])),
               cfg.get('note', {}).get(args.platform + ('_' + build if build else '')))

    nmem = sum(len(s['members']) for s in g.structs.values())
    print('wrote %s: %d structs, %d enums, %d members' % (args.out, len(g.structs), len(g.enums), nmem))
    for k in sorted(g.report):
        v = g.report[k]
        uniq = list(OrderedDict.fromkeys(map(str, v)))
        print('%s: %d' % (k, len(uniq)))
        if k not in ('attribute structs', 'attribute enums'):
            for x in uniq[:60]:
                print('    ', x)


if __name__ == '__main__':
    main()
