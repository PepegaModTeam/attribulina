#!/usr/bin/env python3
"""World's GameCore types from the 2010 beta's EASharp bindings.

    python3 scripts/world/easharp.py [--out scripts/world/easharp_types.h]

Writes a header that scripts/gen_schema.py --game world reads before Carbon's
(see world_types.h for the types this can't give):

- enums: every enum of exes/pc/world-2010-beta/gameplay.dll, with its values
  (the Constant table), named as in C++ (GameCore::EventMode). Enums the
  compiler named Enum<N> (anonymous ones) are left out.
- structs: the classes that wrap a native struct, whose properties each have a
  native setter. EASharp registers each accessor by name in that build's
  nfsw.exe (records of { ..., function, name } stored on the stack); the
  setter's store gives the member's offset and width, the property its type.
  A struct is written only when its members' offsets are what natural layout
  gives them.

The types are the 2010 beta's. The final build's type table has the same
sizes for those it shares, and its data reads sensibly with them
(gameplay.BlackBoardMap: { kBlackBoard_AI, ai_event, 3 }); a value the final
build added to an enum shows as a number.
"""

import argparse
import os
import re
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capstone  # noqa: E402
from clrmeta import Meta, easharp_functions, enums, pe_sections, properties, rva2off  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = 'exes/pc/world-2010-beta/gameplay.dll'
EXE = 'exes/pc/world-2010-beta/nfsw.exe'
PREFIX = '_EASharp_expose___'

# Managed property types to the C++ spelling the schema generator reads.
CPP = {'uint32': 'unsigned int', 'int32': 'int', 'float': 'float', 'uint8': 'unsigned char', 'int8': 'signed char',
       'uint16': 'unsigned short', 'int16': 'short', 'bool': 'bool', 'double': 'double', 'string': 'const char *'}
WIDTH = {'unsigned int': 4, 'int': 4, 'float': 4, 'unsigned char': 1, 'signed char': 1, 'unsigned short': 2, 'short': 2,
         'bool': 1, 'double': 8, 'const char *': 4}


def setter_store(d, secs, va):
    """(offset, width) of the first store through the object pointer ([esp + 4])
    in a native setter, or None."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    o = rva2off(secs, va - 0x400000)
    self_regs = set()
    for ins in md.disasm(d[o:o + 64], va):
        ops = ins.operands
        if ins.mnemonic == 'ret':
            return None
        if ins.mnemonic == 'mov' and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_REG and \
                ops[1].type == capstone.x86.X86_OP_MEM and ops[1].mem.base == capstone.x86.X86_REG_ESP and ops[1].mem.disp == 4:
            self_regs.add(ops[0].reg)
            continue
        if len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_MEM and ops[0].mem.base in self_regs and \
                ins.mnemonic in ('mov', 'movss', 'movsd', 'movq', 'fstp'):
            return ops[0].mem.disp, ops[0].size
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default='scripts/world/easharp_types.h')
    args = ap.parse_args()
    os.chdir(REPO)

    m = Meta(DLL)
    es = enums(m)
    names = [m.string(r[2]) for r in m.tables['ImplMap']]
    fns = easharp_functions(EXE, names)
    d = open(EXE, 'rb').read()
    _, secs = pe_sections(d)

    # The setter of each member: its expose names come in pairs, setter first
    # (two parameters, no return value).
    md_rows = m.tables['MethodDef']
    setters = {}
    for flags, mf, name, scope in m.tables['ImplMap']:
        n = m.string(name)
        parts = n[len(PREFIX):].split('__')
        if len(parts) < 3: continue
        owner, member = '::'.join(parts[:-2]), parts[-2]
        tab, idx = m.coded('MemberForwarded', mf)
        sig = m.blob(md_rows[idx - 1][4])
        if sig[1] == 2 and sig[2] == 0x01 and (owner, member) not in setters and n in fns:
            setters[(owner, member)] = fns[n]

    tds = m.typedefs()
    props = properties(m)
    structs = []
    for t in tds:
        if t[4] != 'EASharp.ExposedCommonBase' or t[0] not in props: continue
        owner = (t[1].replace('.', '::') + '::' if t[1] else '') + t[2]
        members = []
        for pname, ptype in props[t[0]]:
            va = setters.get((owner, pname))
            st = setter_store(d, secs, va) if va else None
            if st is None:
                members = None
                break
            cpp = CPP.get(ptype, ptype.replace('.', '::'))
            members.append((st[0], st[1], cpp, pname))
        if not members: continue
        members.sort()
        # Natural layout must give the same offsets.
        off, ok = 0, True
        for mo, w, cpp, pname in members:
            width = WIDTH.get(cpp) or (es[cpp][0] if cpp in es else w)
            off = (off + width - 1) // width * width if width in (2, 4, 8) else off
            if mo != off: ok = False
            off = mo + width
        if ok: structs.append((owner, members))

    with open(args.out, 'w', newline='\n') as out:
        w = out.write
        w('// World\'s GameCore types, from the 2010 beta\'s EASharp bindings (%s and\n' % DLL)
        w('// the accessors its nfsw.exe registers). Written by scripts/world/easharp.py;\n')
        w('// rerun it rather than editing by hand. scripts/gen_schema.py --game world reads\n')
        w('// this before Carbon\'s headers; a struct is used only where World\'s type table\n')
        w('// has its size. Member offsets are the native setters\' (in comments).\n\n')
        for name in sorted(es):
            size, vals = es[name]
            if re.search(r'(^|::)Enum\d+$', name) or not vals: continue
            w('enum %s {\n' % name)
            for k, v in vals:
                w('\t%s = %d,\n' % (k, v))
            w('};\n\n')
        for owner, members in structs:
            w('struct %s {\n' % owner)
            for mo, width, cpp, pname in members:
                w('\t%s %s; // +%d, %d bytes\n' % (cpp, pname, mo, width))
            w('};\n\n')
    print('wrote %s: %d enums, %d structs' % (args.out, sum(1 for n in es if es[n][1] and not re.search(r'(^|::)Enum\d+$', n)),
                                              len(structs)))
    for owner, members in structs:
        print('   ', owner, [(p, mo) for mo, _, _, p in members])


if __name__ == '__main__':
    main()
