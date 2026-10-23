#!/usr/bin/env python3
"""Reads a 32-bit big-endian PowerPC ELF with a symbol table (the Wii and
GameCube builds, CodeWarrior): sections, symbols, bytes, and a register-tracking
walk over a function. Imported by scan_wii.py and scan_mw.py; run alone it
disassembles:
    python3 -I -B ppcelf.py <elf> <symbol or hex address> [bytes]
Needs capstone (PPC). Elf(path, paired_singles=True) also decodes the Gekko's
paired-single loads and stores (psq_l, psq_st), which the GameCube build's
prologues use; without it a disassembly stops at the first one.
"""
import bisect, re, struct, sys
import capstone


class Elf:
    def __init__(self, path, paired_singles=False):
        self.path = path
        self.img = open(path, 'rb').read()
        img = self.img
        if img[:4] != b'\x7fELF' or img[4] != 1 or img[5] != 2:
            raise ValueError('%s: not a 32-bit big-endian ELF' % path)
        shoff, = struct.unpack_from('>I', img, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('>HHH', img, 0x2e)
        hs = [struct.unpack_from('>IIIIIIIIII', img, shoff + i * shentsize) for i in range(shnum)]
        so = hs[shstrndx][4]
        self.sections = []          # (name, addr, off, size, type, flags)
        for h in hs:
            n = img[so + h[0]:img.index(b'\0', so + h[0])].decode('latin1')
            self.sections.append((n, h[3], h[4], h[5], h[1], h[2]))
        self.syms = {}              # addr -> name (functions and objects)
        self.by_name = {}
        self.sizes = {}
        for i, (n, addr, off, size, typ, flags) in enumerate(self.sections):
            if typ != 2:
                continue
            strtab = self.sections[hs[i][6]][2]
            for j in range(0, size, 16):
                st_name, value, ssize, info, other, shndx = struct.unpack_from('>IIIBBH', img, off + j)
                if not st_name:
                    continue
                name = img[strtab + st_name:img.index(b'\0', strtab + st_name)].decode('latin1')
                self.by_name.setdefault(name, value)
                if (info & 15) in (1, 2) and value:        # OBJECT, FUNC
                    self.syms.setdefault(value, name)
                    self.sizes[value] = ssize
        self.addrs = sorted(self.syms)
        self.md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN
                              | (capstone.CS_MODE_PS if paired_singles else 0))

    # -- symbols
    def name(self, a):
        i = bisect.bisect_right(self.addrs, a) - 1
        if i < 0:
            return '%x' % a
        s = self.addrs[i]
        return self.syms[s] + ('' if s == a else '+0x%x' % (a - s))

    def func_end(self, a):
        if self.sizes.get(a):
            return a + self.sizes[a]
        i = bisect.bisect_right(self.addrs, a)
        return self.addrs[i] if i < len(self.addrs) else a + 0x400

    # -- bytes
    def where(self, va):
        for n, addr, off, size, typ, flags in self.sections:
            if typ == 8 or not (flags & 2) or not size:
                continue
            if addr <= va < addr + size:
                return off + va - addr, addr + size - va
        return None, 0

    def section_of(self, va):
        for n, addr, off, size, typ, flags in self.sections:
            if (flags & 2) and addr <= va < addr + size:
                return n
        return None

    def read(self, va, n):
        off, avail = self.where(va)
        return b'' if off is None else self.img[off:off + min(n, avail)]

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('>I', b)[0] if len(b) == 4 else None

    def cstr(self, va, limit=128):
        b = self.read(va, limit)
        e = b.find(b'\0')
        if e <= 0:
            return None
        s = b[:e]
        return s.decode() if all(32 <= c < 127 for c in s) else None

    def is_code(self, va):
        return self.section_of(va) in ('.init', '.text')

    def disasm(self, lo, hi):
        return self.md.disasm(self.read(lo, hi - lo), lo)

    def callers(self, targets):
        """{target: [call sites]} for every `bl` in .text."""
        out = {t: [] for t in targets}
        for n, addr, off, size, typ, flags in self.sections:
            if n != '.text':
                continue
            for i in range(0, size - 3, 4):
                w, = struct.unpack_from('>I', self.img, off + i)
                if (w >> 26) != 18 or (w & 3) != 1:
                    continue
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = (addr + i + li) & 0xffffffff
                if t in out:
                    out[t].append(addr + i)
        return out


VOLATILE = ['r0'] + ['r%d' % i for i in range(3, 13)]
QUIET = ('stwu', 'stmw', 'cmpwi', 'cmplwi', 'cmpw', 'cmplw', 'b', 'beq', 'bne', 'blt', 'bgt', 'ble', 'bge',
         'blr', 'mtlr', 'mtctr', 'stb', 'sth', 'stfs', 'stfd', 'nop', 'bctrl', 'bctr')


def walk(elf, lo, hi, on_call):
    """Walks lo..hi linearly, tracking constants in registers: lis/addi/ori/li/mr,
    lwz from a known address (static data), and words stored in the stack
    frame. r1 is 0 throughout (the prologue's stwu is ignored), so a register
    holding a stack address holds its frame offset. At every `bl` it calls
    on_call(site, target, regs, stack) and then forgets the volatile
    registers. r13 and r2 are the small-data bases."""
    regs = {'r13': elf.by_name.get('_SDA_BASE_'), 'r2': elf.by_name.get('_SDA2_BASE_'), 'r1': 0}
    stack = {}

    def imm(x):
        return int(x, 0)
    for ins in elf.disasm(lo, hi):
        m = ins.mnemonic
        ops = [o.strip() for o in ins.op_str.split(',')] if ins.op_str else []
        if m == 'lis':
            regs[ops[0]] = (imm(ops[1]) << 16) & 0xffffffff
        elif m == 'li':
            regs[ops[0]] = imm(ops[1]) & 0xffffffff
        elif m in ('addi', 'addic', 'addic.', 'addis'):
            b = regs.get(ops[1])
            k = imm(ops[2]) << (16 if m == 'addis' else 0)
            regs[ops[0]] = None if b is None else (b + k) & 0xffffffff
        elif m == 'ori':
            b = regs.get(ops[1])
            regs[ops[0]] = None if b is None else b | imm(ops[2])
        elif m == 'mr':
            regs[ops[0]] = regs.get(ops[1])
        elif m == 'lwz':
            d, mem = ops[1].rstrip(')').split('(')
            b = regs.get(mem)
            if mem == 'r1':
                regs[ops[0]] = stack.get(imm(d))
            elif b is not None and elf.where((b + imm(d)) & 0xffffffff)[0] is not None:
                regs[ops[0]] = elf.u32((b + imm(d)) & 0xffffffff)
            else:
                regs[ops[0]] = None
        elif m == 'stw':
            d, mem = ops[1].rstrip(')').split('(')
            if mem == 'r1':
                stack[imm(d)] = regs.get(ops[0])
        elif m == 'bl':
            on_call(ins.address, imm(ops[0]), regs, stack)
            for r in VOLATILE:
                regs.pop(r, None)
        elif m in QUIET:
            pass
        elif ops and re.match(r'^r\d+$', ops[0]):
            regs.pop(ops[0], None)      # anything else that writes a register forgets it


if __name__ == '__main__':
    e = Elf(sys.argv[1], paired_singles=True)
    a = e.by_name.get(sys.argv[2])
    a = a if a is not None else int(sys.argv[2], 16)
    n = int(sys.argv[3], 0) if len(sys.argv) > 3 else e.func_end(a) - a
    for ins in e.disasm(a, a + n):
        note = ''
        if ins.mnemonic == 'bl' or ins.mnemonic == 'b':
            note = e.name(int(ins.op_str, 0))
        print('%08x  %-8s %-28s %s' % (ins.address, ins.mnemonic, ins.op_str, note))
