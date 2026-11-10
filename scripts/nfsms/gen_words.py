#!/usr/bin/env python3
"""Write NFS-VltEd's word tables, data/nfsms/<game>_pc.txt, for Most Wanted,
Carbon, Pro Street, Undercover and World. See README.md.

    python3 scripts/nfsms/gen_words.py [--game MW|C|PS|UC|W|WB]... [--out DIR] [--schema-out DIR]

Paths are the repo's, wherever it runs from. Reads NFS-VltEd/ (the
decompilation), data/schema/, the PC databases' type tables under
attrib/databases/, and for Undercover nfs-data/SpeedDebugOpt-enUS.h, from which
it lays out Undercover's PC types in memory (--schema-out also writes them).
"""

import sys

sys.dont_write_bytecode = True

import argparse  # noqa: E402
import os  # noqa: E402
import re  # noqa: E402
import textwrap  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import arrays  # noqa: E402
from common import REPO, class_file, rel  # noqa: E402
from decode import decode_class  # noqa: E402
import registry  # noqa: E402
from schema import Schema, from_gen  # noqa: E402
import typetable  # noqa: E402

SCHEMAS = os.path.join(REPO, 'data', 'schema')

# checks: the other platforms' schemas, with the databases whose type tables say
# which types each has (none seen for Undercover's PS2). The platforms that share
# the PC's schema (Pro Street's 360 and Wii, Carbon's PS3 and Wii, Most Wanted's
# Xbox and 360 beta) lay the types out as the PC does.
GAMES = {
    'MW': dict(name='Most Wanted', out='mw_pc.txt', schema='mw_pc.txt',
               checks=[('mw_gamecube.txt', ['gamecube/mw-vanilla', 'ps2/mw-beta'])],
               example='update_field tires x GRIP_SCALE Front 1.1',
               note='''Most Wanted's StringKey is 16 bytes (a 64-bit hash, the 32-bit one, the string):
NFS-VltEd's classes for it skip both hashes, and a MODEL field has a third word,
Collision64.'''),
    'C': dict(name='Carbon', out='carbon_pc.txt', schema='carbon_pc.txt',
              checks=[('carbon_ps2.txt', ['ps2/carbon-vanilla', 'gamecube/carbon-vanilla']),
                      ('carbon_x360.txt', ['360/carbon-latedemo']),
                      ('carbon_ps2_demo.txt', ['ps2/carbon-demo'])],
              example='update_field tires x GRIP_SCALE Front 1.1',
              note='''Carbon's 360 files (a late demo) and its PS2 demo have Most Wanted's 16-byte
StringKey, so a StringKey there is laid out otherwise (see the "# on" comments).'''),
    'PS': dict(name='Pro Street', out='prostreet_pc.txt', schema='prostreet_pc.txt',
               checks=[('prostreet_ps2.txt', ['ps2/prostreet-vanilla'])],
               example='update_field pvehicle x TORQUE MinY 90', note=None),
    # NFS-VltEd's W (the final build) and WB (the 2009 and 2010 betas), which
    # one schema lays out.
    'W': dict(name='World', out='world_pc.txt', schema='world_pc.txt', checks=[],
              example='update_field pursuitlevels x Roadblock_CopTypes[0] Value 2',
              note='''World has no header: our schema's World types come from Carbon's headers, the 2010
beta's EASharp bindings, Undercover's debug header and, for some, NFS-VltEd's own
World classes (scripts/world/world_types.h), so most paths name members those
sources name, and the words NFS-VltEd numbers lead to Value<n> members.'''),
    'WB': dict(name="World's betas", out='world_beta_pc.txt', schema='world_pc.txt', checks=[],
               example='update_field pursuitlevels x Roadblock_CopTypes[0] Value 2',
               note='''NFS-VltEd's WB is World's betas, the 2009 and 2010 builds here, which the editor reads
with the final build's schema (data/schema/world_pc.txt): the paths name its members.
A script for WB runs on a beta's files, one for W on the final build's.'''),
    'UC': dict(name='Undercover', out='undercover_pc.txt', schema=None,
               checks=[('undercover_wii.txt', ['wii/undercover-vanilla']), ('undercover_ps2.txt', [])],
               example='update_field car_tuning x TORQUE MinY 90',
               note='''NFS-VltEd's Undercover is the PC game, the current-generation one (PC, 360). Its Wii
and PS2 versions are another game on Carbon's engine with mostly other types (the
"# not on wii" comments). This script lays the PC's types out from
nfs-data/SpeedDebugOpt-enUS.h (the 360 debug build, member order only) at the PC's
alignment, checked against the PC type table's sizes, as data/schema/undercover_pc.txt
does, and the paths name those members.'''),
}

# Notes on a class, by its file.
NOTES = {
    'A/_001B_0012.cs': "the game's order is DegPerG, MaxGs, DegPerSec (PS, UC, MW and Carbon headers alike). "
                       "NFS-VltEd declares DegPerG, DegPerSec, MaxG and its setters are proxies, so the "
                       "decompilation can't show whether DegPerSec lands at 4 or 8. Taken in declaration order "
                       "here; may be swapped.",
    'A/_0017_0019.cs': "Type is declared first but read last (offset 12). It carries DataType key on a string "
                       "property, so how a value converts is unclear.",
    'A/_0008_0015.cs': "no GraphScale: that is A/_0019_0012.cs, registered for UC/W/WB. update_collection <class> "
                       "<node> TORQUE YValues 6 Value 0.98 sets element 6 of Y. That the list property yields its "
                       "Items list is inferred (the property iterator isn't in the decompilation; "
                       "A/_000C_0001.cs:290-298 needs an IList).",
    'A/_000D_0017.cs': "the three count bytes are written from the list lengths; OfferID (52) has no attribute, "
                       "so scripts can't set it.",
    'A/_001C_0012.cs': "'3' and '4' are the author's member numbers (1-based), not offsets.",
    'A/_0001_0012.cs': "the hash at 0 is recomputed from the text when written.",
    'A/_0005_0012.cs': "NFS-VltEd reads the value as eight u16s; the game's struct is four floats "
                       "(SpeedDebugOpt-enUS.h), so each word sets half of one.",
    'A/_0014_0012.cs': "NFS-VltEd reads the value as seven u32s in hex; past partNameHash each covers parts of "
                       "several members.",
    'A/_001E_0012.cs': "these games' TireEffectRecord has no tire condition: it starts with the emitter's "
                       "RefSpec. Taken in declaration order, TireCondition reads its class key, EmitterClass its "
                       "collection key and EmitterCollection the runtime pointer after them.",
}

# Attrib::StringKey in a field named MODEL: A/_0007_0014.cs:1262-1293 returns
# its own class before the registry is consulted, MW's at line 1286.
MODEL = {'MW': ('A/_0017_0015.cs', 1286)}
MODEL_OTHER = ('A/_0007_0012.cs', 1293)


def vlt_type(p, k):
    """NFS-VltEd's own type for a word: what it reads and writes at the offset and
    how it parses the text (the attribute's DataType). u32/key: a name hashed with
    StringHash32 (a number in hex as itself); /binkey: a decimal number as itself,
    else bStringHash; /key64: StringHash64; /hex: 0x hex or decimal; i32>f32: the
    file holds an i32 that NFS-VltEd shows as a float."""
    dt = p['dt'] if p else 0
    t = k.replace('->', '>')
    if dt == 3:
        return t + '/key'
    if dt in (4, 8):
        return t + '/binkey'
    if dt == 2:
        return t + '/key64'
    if dt in (1, 6):
        return t + '/hex'
    return t


def kindstr(p, k):
    dt = p['dt'] if p else 0
    if k == 'u32' and dt == 3:
        return 'key'
    if k == 'u32' and dt in (4, 8):
        return 'binkey'
    if k == 'u32' and dt in (1, 6):
        return 'u32 hex'
    if k == 'i32' and dt == 3:
        return 'key (i32 read)'
    return k


# Types Undercover's header lacks, by exact name and only at the PC table's size:
# Pro Street's first (the game before it), then Carbon's and Most Wanted's.
UC_FALLBACKS = ['nfs-data/ProStreet08Milestone.h', 'nfs-data/carbon/ps3-milestone/EBOOT.ELF.h',
                'exes/gamecube/mw-vanilla/NFSMWRELEASE.h']


def undercover_schema(types, out_dir=None):
    """Undercover's PC types laid out from its 360 debug build's header (no
    offsets: natural PC layout), checked against the PC type table's sizes."""
    sys.path.insert(0, os.path.join(REPO, 'scripts'))
    import gen_schema
    header = os.path.join(REPO, 'nfs-data', 'SpeedDebugOpt-enUS.h')
    primary = gen_schema.parse_header(header, with_offsets=False)
    fallbacks = []
    for f in UC_FALLBACKS:
        fd, fe = gen_schema.parse_header(os.path.join(REPO, f), with_offsets=False)
        fallbacks.append((f, fd, fe))
    g = gen_schema.Gen(primary, fallbacks, list(types.items()), 'PC', 'Undercover', natural=True)
    g.run()
    if out_dir:
        path = os.path.join(out_dir, 'undercover_pc.txt')
        with open(path, 'w', newline='\n') as f:
            g.emit(f, 'undercover', 'Need for Speed: Undercover', ['pc'],
                   'Laid out by scripts/nfsms/gen_words.py from SpeedDebugOpt-enUS.h for the word table only.')
        print('wrote', path)
    mism = g.report.get('attribute size mismatch', []) + [(n, None, None) for n in
                                                           g.report.get('size mismatch (left out)', [])]
    return from_gen(g, 'undercover_pc'), mism


class Table:
    def __init__(self, game, regs, sch, checks, dbtypes):
        self.game = game
        self.cfg = GAMES[game]
        self.regs = regs
        self.sch = sch
        self.checks = checks          # [(label, Schema, {type: size} or None)]
        self.db = dbtypes             # name -> size
        self.stats = dict(types=0, words=0, with_path=0, conf={}, plus=[], failures=[], sizes=[], absent=[],
                          checks={})

    def word(self, word, off, path, names, rest='', covered=None, vtype=None):
        """A word line: <word> <offset> [:<type>] [<path>] [list ...]  # covered members."""
        assert ' ' not in word and word not in ('list', 'single', 'end'), word
        assert path is None or (path != 'list' and ' ' not in path), path
        s = '  %s %d' % (word, off)
        if vtype:
            s += ' :' + vtype
        if path:
            s += ' ' + path
        s += rest
        redundant = path is not None and len(names) == 1 and path.split('+')[0] == names[0]
        if names and not redundant:
            s += '  # ' + ','.join(names)
        elif not names and covered is not None:
            s += '  # no member of our schema at these bytes'
        return s

    def words(self, t, d, st):
        out = []
        paths = []                    # (word, offset, path, names)
        for what, p, off, k, sz in d['out']:
            path, names = self.sch.path(t, off, sz) if st else (None, [])
            if what == 'prop':
                if p and p['attr']:
                    out.append(self.word(p['name'], off, path, names, covered=st, vtype=vlt_type(p, k)))
                    paths.append((p['name'], off, path, names))
                else:
                    nm = p['cs'] + ' (no attribute: not a script property)' if p else '?'
                    out.append('  # %s %d %s%s' % (nm, off, kindstr(p, k), (' schema=' + ','.join(names)) if names else ''))
            elif what == 'list':
                kind, _, desc, elems = k
                if not (p and p['attr']):
                    nm = (p['cs'] + ' (no attribute: not a script property)') if p else '?'
                    out.append('  # %s %d %s (%s)%s' % (nm, off, 'string' if kind == 'string' else 'list ' + kind, desc,
                                                       (' schema=' + ','.join(names)) if names else ''))
                elif kind == 'ptr2u32':
                    out.append('  # %s %d: ptr2u32, not editable%s' % (p['name'], off, ('  # ' + ','.join(names)) if names else ''))
                elif elems:
                    out.append(self.word(p['name'], off, path, names, ' list ' + ' '.join(elems), vtype=kind))
                    paths.append((p['name'], off, path, names))
                elif kind in ('string', 'blob'):
                    out.append(self.word(p['name'], off, path, names, vtype=kind))
                    paths.append((p['name'], off, path, names))
                else:
                    raise SystemExit('%s: list kind %s not handled' % (t, kind))
        for w, off, path, names in paths:
            self.stats['words'] += 1
            if path:
                got = self.sch.resolve(t, path)[0]
                assert got == off, (t, w, path, got, off)
            if path and '+' in path:
                self.stats['plus'].append('%s %s %s' % (t, w, path))
        return out, paths

    def platform_notes(self, t, st, paths):
        """Comment lines for the platforms whose schema lays the type out otherwise."""
        out = []
        if not st:
            return out
        for label, cs, ptypes in self.checks:
            c = self.stats['checks'].setdefault(label, dict(types=0, words=0, failed=0, moved=0, absent=0, nostruct=0, size=0))
            if ptypes is not None and t not in ptypes:
                out.append('  # not on %s: its files have no %s' % (label, t))
                self.stats['absent'].append((self.game, label, t))
                c['absent'] += 1
                continue
            cst = cs.structs.get(t)
            if cst is None:
                if any(p for _, _, p, _ in paths):
                    c['nostruct'] += 1
                    why = 'no struct %s in %s%s, so the paths don\'t resolve there' % (
                        t, cs.name, ', though its files have the type (%d bytes)' % ptypes[t] if ptypes else '')
                    out.append('  # not on %s: %s' % (label, why))
                    self.stats['failures'].append((self.game, label, t, '(every word)', why))
                continue
            bad = []
            c['types'] += 1
            for w, off, path, names in paths:
                if path:
                    c['words'] += 1
                    got, why = cs.resolve(t, path)
                    if got is None:
                        c['failed'] += 1
                        bad.append('word %s (%s): %s' % (w, path, why))
                elif names:
                    # no path: the PC offset, which holds only where the members stay put
                    moved = [n for n in names if cs.offset_of(t, n) != self.sch.offset_of(t, n)]
                    if moved:
                        c['moved'] += 1
                        bad.append('word %s has no path, and %s %s elsewhere there' % (w, ','.join(moved), 'is' if len(moved) == 1 else 'are'))
            for b in bad:
                out.append('  # not on %s: %s' % (label, b))
                self.stats['failures'].append((self.game, label, t, b, ''))
            if cst['size'] != st['size']:
                c['size'] += 1
                if bad:
                    out.append('  # not on %s: %d bytes there, not %d' % (label, cst['size'], st['size']))
                else:
                    out.append('  # on %s: %d bytes there, not %d; every word has a path that resolves' % (label, cst['size'], st['size']))
                self.stats['sizes'].append((self.game, label, t, cst['size'], st['size'], bool(bad)))
        return out

    def block(self, t, reg, conf, why, others):
        path = class_file(reg['cls'])
        d = decode_class(path)
        st = self.sch.structs.get(t)
        name = self.cfg['name']
        b = ['type ' + t, '  # ' + rel(path)]
        if st and st['size'] != d['size']:
            b.append('  # size differs: schema %d' % st['size'])
        if t in self.db and self.db[t] != d['size']:
            b.append('  # size differs: %s database type size %d' % (name, self.db[t]))
        if t not in self.db:
            b.append('  # not a type of the %s PC database' % name)
        elif not st and t not in self.sch.enums:
            b.append('  # no struct of this name in our schema (database kind: %s)' % typetable.kind_of(t, self.db[t]))
        b.append('  # %s (%s; registry line %d)' % (conf, why, reg['line']))
        for o, oconf, owhy in others:
            if o['line'] > reg['line']:
                b.append('  # note: %s (%s; registry line %d) comes later and replaces it if its list holds %s'
                         % (o['cls'], o['label'], o['line'], self.game))
            else:
                b.append('  # note: %s (%s; registry line %d) comes earlier, so this one replaces it'
                         % (o['cls'], o['label'], o['line']))
        if len([p for p in d['props'] if p['attr']]) <= 1:
            b.append('  single')
        w, paths = self.words(t, d, st)
        b += w
        b += self.platform_notes(t, st, paths)
        for p in d['problems']:
            b.append('  # note: ' + p)
        if reg['cls'] in NOTES:
            b.append('  # note: ' + NOTES[reg['cls']])
        else:
            b += swapped(paths)
        b.append('end')
        self.stats['types'] += 1
        self.stats['conf'][conf] = self.stats['conf'].get(conf, 0) + 1
        if any(p for _, _, p, _ in paths):
            self.stats['with_path'] += 1
        return '\n'.join(b)

    def model_block(self):
        cls, line = MODEL.get(self.game, MODEL_OTHER)
        d = decode_class(class_file(cls))
        t = 'Attrib::StringKey'
        st = self.sch.structs.get(t)
        b = ['type Attrib::StringKey(MODEL)', '  # ' + cls, '  # read (A/_0007_0014.cs:%d)' % line]
        w, paths = self.words(t, d, st)
        b += w
        b += self.platform_notes(t, st, paths)
        for p in d['problems']:
            b.append('  # note: ' + p)
        b.append('end')
        self.stats['types'] += 1
        self.stats['conf']['read'] = self.stats['conf'].get('read', 0) + 1
        if any(p for _, _, p, _ in paths):
            self.stats['with_path'] += 1
        return '\n'.join(b)

    def header(self):
        name = self.cfg['name']
        article = 'An' if name[0] in 'AEIOU' else 'A'
        schema = ('our schema (data/schema/%s)' % self.cfg['schema'] if self.cfg['schema']
                  else "Undercover's PC types (below)")
        paras = [
            "NFS-VltEd 4.6's names for the parts of %s (PC) values: the words nfsms scripts write "
            "after the field (%s). doc/nfsms.md. Written by scripts/nfsms/gen_words.py; rerun it "
            "rather than editing by hand." % (name, self.cfg['example']),
            "Read from the NFS-VltEd decompilation (NFS-VltEd/). A/_0007_0014.cs registers one C# class "
            "per AttribSys type and game, keyed by StringHash32 of the type name; each class names its "
            "properties with an attribute and reads them in order, which gives each name its offset in "
            "the PC files NFS-VltEd reads. The path after the offset names the member of %s at "
            "those bytes, so a word finds its member by name on a platform whose layout differs "
            "(4-byte bools move later members). %s %s type not listed here gets NFS-VltEd's raw class: "
            'its one property is Data, the value\'s bytes in hex ("00 00 80 3F").' % (schema, article, name),
        ]
        if self.cfg['note']:
            paras.append(self.cfg['note'].replace('\n', ' '))
        fmt = """game <code>
type <AttribSys type name>      Attrib::StringKey(MODEL): a StringKey field named MODEL
  single                        one property: NFS-VltEd ignores the word after the field
  <word> <offset> :<type> [<path>] [list <element words>]
end"""
        paras2 = [
            '<path>: member names joined with ".", [i] for an element of an inline array, down to the '
            'member the word sets (Reaction.mClassKey, ValueMin[1]). A word over several members has the '
            'deepest member holding them all (X for X.mArray and X.mLength). A path ending in +N: the '
            'word\'s bytes start N bytes into that member. No path: the word\'s members have nothing in common but the '
            'value itself, or the type has no struct in our schema (enums, vectors, simple values), where '
            'the offset is the same on every platform.',
            '<type>: what NFS-VltEd itself reads and writes at the offset, which its NFS-VltEd compatible '
            'reading uses (doc/nfsms.md): u8, i8, u16, i16, u32, i32, u64, i64, f32, f64, bool8 (a byte, 1 '
            'is true), bool32; then /key (a name, hashed with StringHash32; 0x and 8 hex digits is the '
            'number), /binkey (a decimal number is itself, else bStringHash), /key64 or /hex (0x hex or '
            'decimal). i32>f32: the file holds an i32 that NFS-VltEd shows as a float. A list word has '
            'the list\'s kind (f32list, keylist, refspeclist, ...), string a char* and blob a size and '
            'pointer.',
            'The comments give where each class was read and how sure the game assignment is: "read" '
            'means registered for %s or for every game, or in a compiled array whose bytes other code '
            'shows; "inferred" and "uncertain" mean the game list is a compiled array the decompilation '
            'lacks, so it was worked out (see the comment and scripts/nfsms/registry.py). After a word, '
            '"# x" lists our schema\'s members at its bytes where the path doesn\'t name them all. '
            '"# not on <platform>" marks a word that can\'t be found that way in that platform\'s schema, '
            'or a type its files lack; "# on <platform>" a type laid out otherwise there whose words all '
            'find their members.' % name,
        ]

        def wrap(p):
            return textwrap.fill(p, width=84, initial_indent='# ', subsequent_indent='# ', break_on_hyphens=False)

        out = '\n#\n'.join([wrap(p) for p in paras] + ['\n'.join('# ' + l for l in fmt.split('\n'))] +
                            [wrap(p) for p in paras2])
        return out + '\n\ngame %s\n' % self.game

    def build(self):
        blocks = []
        for t, (reg, conf, why, others) in sorted(registry.classes_for(self.regs, self.game).items(),
                                                  key=lambda kv: kv[0].lower()):
            blocks.append(self.block(t, reg, conf, why, others))
        blocks.append(self.model_block())
        return self.header() + '\n' + '\n\n'.join(blocks) + '\n'


def plain(name):
    """A word or member name without case, separators or a leading m."""
    if len(name) > 1 and name[0] == 'm' and name[1].isupper():
        name = name[1:]
    return re.sub(r'[^a-z0-9]', '', name.lower())


def swapped(paths):
    """A note when, in declaration order, a word sets the member another word is
    named after: the setters are proxies, so the order is an assumption."""
    leaf = {}
    for w, off, path, names in paths:
        if path and len(names) == 1:
            leaf[w] = (plain(re.sub(r'\[\d+\]$', '', path.split('+')[0].split('.')[-1])), path)
    named = {l for l, _ in leaf.values()}
    involved = [w for w, (l, _) in leaf.items() if plain(w) != l and plain(w) in named]
    if not involved:
        return []
    return ['  # note: taken in declaration order, %s, though the names say otherwise. The setters are '
            "proxies, so the decompilation can't show which is right." %
            ' and '.join('%s sets %s' % (w, leaf[w][1]) for w in involved)]


def label_of(cs):
    return ' and '.join(cs.platforms) + (' demo' if '_demo' in cs.name else '')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', action='append', choices=sorted(GAMES))
    ap.add_argument('--out', default=os.path.join(REPO, 'data', 'nfsms'))
    ap.add_argument('--schema-out')
    args = ap.parse_args()
    regs = registry.parse()
    # the static array table must say what its evidence does
    have = {g: set(typetable.game_types(REPO, g)) for g in registry.GAMES}
    for f, err in arrays.check(arrays.analyse(regs, have)):
        if f != '-' or err != 'agrees':
            print('warning: registry.ARRAYS %s: %s (scripts/nfsms/arrays.py)' % (f, err))
    for game in args.game or ['MW', 'C', 'PS', 'UC', 'W', 'WB']:
        cfg = GAMES[game]
        db = typetable.game_types(REPO, game)
        if cfg['schema']:
            sch = Schema.load(os.path.join(SCHEMAS, cfg['schema']))
        else:
            sch, mism = undercover_schema(db, args.schema_out)
            for m in mism:
                print('  undercover_pc: size mismatch', m)
        checks = []
        for f, folders in cfg['checks']:
            cs = Schema.load(os.path.join(SCHEMAS, f))
            ptypes = None
            if folders:
                ptypes = {}
                for folder in folders:
                    for n, size in typetable.read_types(typetable.attributes_bin(
                            os.path.join(REPO, 'attrib', 'databases', folder))):
                        ptypes.setdefault(n, size)
            checks.append((label_of(cs), cs, ptypes))
        tab = Table(game, regs, sch, checks, db)
        text = tab.build()
        path = os.path.join(args.out, cfg['out'])
        with open(path, 'w', newline='\n') as f:
            f.write(text)
        s = tab.stats
        print('wrote %s: %d types, %d words, %d types with a path, %s' % (
            os.path.relpath(path, REPO), s['types'], s['words'], s['with_path'],
            ', '.join('%s %d' % kv for kv in sorted(s['conf'].items()))))
        for x in s['plus']:
            print('  +N path:', x)
        for x in s['failures']:
            print('  not on %s: %s %s %s' % (x[1], x[2], x[3], x[4]))
        for x in s['sizes']:
            print('  on %s: %s is %d bytes, not %d%s' % (x[1], x[2], x[3], x[4], '' if x[5] else ' (all words resolve)'))
        for x in s['absent']:
            print('  not on %s: no %s in its files' % (x[1], x[2]))
        for label, c in s['checks'].items():
            print('  check %s: %d types with a struct there, %d words with a path (%d fail), %d without a path whose '
                  'members move; %d types not in its files, %d without a struct there, %d of another size'
                  % (label, c['types'], c['words'], c['failed'], c['moved'], c['absent'], c['nostruct'], c['size']))


if __name__ == '__main__':
    main()
