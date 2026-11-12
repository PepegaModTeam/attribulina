"""A minimal DWARF 2 reader for CodeWarrior's PowerPC ELFs (big-endian).

    python3 scripts/dwarf2.py exes/wii/nitro-japan-oldelf/nfs_na_f.elf

Nitro's old Wii ELF carries DWARF 2 (.debug_info, .debug_abbrev) from the
MetroWerks compiler: types with member offsets, but no namespaces, and a
class's own types aren't nested in it. read() gives every DIE by its offset;
gen_schema.py's parse_dwarf turns the types into the dicts its header parsers
make. Only the forms that compiler writes are read."""
import struct, sys

TAGS = {0x01: 'array_type', 0x02: 'class_type', 0x04: 'enumeration_type', 0x05: 'formal_parameter', 0x0b: 'lexical_block',
        0x0d: 'member', 0x0f: 'pointer_type', 0x10: 'reference_type', 0x11: 'compile_unit', 0x13: 'structure_type',
        0x15: 'subroutine_type', 0x16: 'typedef', 0x17: 'union_type', 0x18: 'unspecified_parameters', 0x1c: 'inheritance',
        0x21: 'subrange_type', 0x1f: 'ptr_to_member_type', 0x24: 'base_type', 0x26: 'const_type', 0x28: 'enumerator',
        0x2e: 'subprogram', 0x34: 'variable', 0x35: 'volatile_type', 0x39: 'namespace'}
ATS = {0x01: 'sibling', 0x02: 'location', 0x03: 'name', 0x0b: 'byte_size', 0x0c: 'bit_offset', 0x0d: 'bit_size',
       0x10: 'stmt_list', 0x11: 'low_pc', 0x12: 'high_pc', 0x13: 'language', 0x1b: 'comp_dir', 0x1c: 'const_value',
       0x1d: 'containing_type', 0x25: 'producer', 0x2f: 'upper_bound', 0x32: 'accessibility', 0x38: 'data_member_location',
       0x3a: 'decl_file', 0x3b: 'decl_line', 0x3c: 'declaration', 0x3e: 'encoding', 0x3f: 'external', 0x40: 'frame_base',
       0x43: 'macro_info', 0x49: 'type', 0x4c: 'virtuality', 0x2007: 'HP_unit_size'}


def uleb(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1
        r |= (x & 0x7f) << s; s += 7
        if x < 0x80: return r, i


def sleb(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1
        r |= (x & 0x7f) << s; s += 7
        if x < 0x80:
            if x & 0x40: r -= 1 << s
            return r, i


def sections(path):
    d = open(path, 'rb').read()
    shoff, = struct.unpack_from('>I', d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('>HHH', d, 0x2e)
    hdrs = [struct.unpack_from('>IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
    strtab = hdrs[shstrndx]
    out = {}
    for h in hdrs:
        nm = d[strtab[4] + h[0]:d.index(b'\0', strtab[4] + h[0])].decode()
        out[nm] = (h[3], h[4], h[5], d[h[4]:h[4] + h[5]] if h[1] != 8 else b'')  # addr, off, size, bytes
    return d, out


class Die:
    __slots__ = ('off', 'tag', 'at', 'children', 'parent')

    def __init__(self, off, tag, parent):
        self.off, self.tag, self.at, self.children, self.parent = off, tag, {}, [], parent

    def name(self):
        return self.at.get('name')


def read(path):
    """-> {offset: Die} for every DIE, and the compile units' DIEs."""
    _, secs = sections(path)
    ab = secs['.debug_abbrev'][3]; info = secs['.debug_info'][3]
    abbrevs = {}

    def load_abbrevs(off):
        if off in abbrevs: return abbrevs[off]
        t = {}; i = off
        while True:
            code, i = uleb(ab, i)
            if code == 0: break
            tag, i = uleb(ab, i)
            has_children = ab[i]; i += 1
            specs = []
            while True:
                a, i = uleb(ab, i); f, i = uleb(ab, i)
                if a == 0 and f == 0: break
                specs.append((a, f))
            t[code] = (tag, has_children, specs)
        abbrevs[off] = t
        return t

    dies = {}; cus = []
    i = 0
    while i < len(info):
        length, ver, aoff, psize = struct.unpack_from('>IHIB', info, i)
        end = i + 4 + length
        table = load_abbrevs(aoff)
        j = i + 11
        stack = [None]
        while j < end:
            off = j
            code, j = uleb(info, j)
            if code == 0:
                stack.pop()
                if not stack: stack = [None]
                continue
            tag, hc, specs = table[code]
            die = Die(off, TAGS.get(tag, hex(tag)), stack[-1])
            for a, f in specs:
                if f == 0x01: v, = struct.unpack_from('>I', info, j); j += 4          # addr
                elif f == 0x0b: v = info[j]; j += 1                                    # data1
                elif f == 0x05: v, = struct.unpack_from('>H', info, j); j += 2        # data2
                elif f == 0x06: v, = struct.unpack_from('>I', info, j); j += 4        # data4
                elif f == 0x0c: v = info[j]; j += 1                                    # flag
                elif f == 0x10: v, = struct.unpack_from('>I', info, j); j += 4        # ref_addr
                elif f == 0x08:                                                        # string
                    e = info.index(b'\0', j); v = info[j:e].decode('latin-1'); j = e + 1
                elif f == 0x0f: v, j = uleb(info, j)                                   # udata
                elif f == 0x0d: v, j = sleb(info, j)                                   # sdata
                elif f == 0x09: n, j = uleb(info, j); v = info[j:j + n]; j += n       # block
                elif f == 0x0a: n = info[j]; j += 1; v = info[j:j + n]; j += n        # block1
                elif f == 0x03: n, = struct.unpack_from('>H', info, j); j += 2; v = info[j:j + n]; j += n  # block2
                elif f == 0x04: n, = struct.unpack_from('>I', info, j); j += 4; v = info[j:j + n]; j += n  # block4
                elif f == 0x11: v, = struct.unpack_from('>I', info, j); j += 4; v += i  # ref4 (CU-relative)
                else: raise ValueError('form 0x%x at 0x%x' % (f, off))
                die.at[ATS.get(a, hex(a))] = v
            dies[off] = die
            if stack[-1] is not None: stack[-1].children.append(die)
            else: cus.append(die)
            if hc: stack.append(die)
        i = end
    return dies, cus


def member_offset(die):
    """DW_AT_data_member_location as a byte offset (DW_OP_plus_uconst)."""
    v = die.at.get('data_member_location')
    if v is None: return None
    if isinstance(v, int): return v
    if v and v[0] == 0x23:
        return uleb(v, 1)[0]
    raise ValueError('location %r' % v)


if __name__ == '__main__':
    dies, cus = read(sys.argv[1])
    print(len(dies), 'dies', len(cus), 'cus')
