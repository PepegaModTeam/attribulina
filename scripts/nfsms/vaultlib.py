"""VaultLib's names for the parts of a game's values: the C# property names that
the community Attribulator's mod scripts write after the field (update_field
pvehicle x TORQUE MinX 1000), read from VaultLib's source in quirk-analysis/VaultLib
(heyitsleo's library, which Attribulator and OGVI are built on).

Most of them are our schema's member names, as Attribulina's own reading takes
them (in any case, without an m prefix). For the others, decode() reads the
class's Read(Vault, BinaryReader) method in order, which gives each property the
bytes it reads, and aliases() maps it to our member at those bytes. A name that
is one of the type's own member names elsewhere (VaultLib's GMapTriangle counts
mPoint1 to mPoint3, the header mPoint0 to mPoint2) gets no alias: our names
win, and the table lists it as a clash.
"""

import os
import re

from common import REPO
from schema import PADDING

VAULTLIB = os.path.join(REPO, 'quirk-analysis', 'VaultLib')
# The assemblies each game registers: SpeedFramework's types, then its own.
FOLDERS = {'MW': 'MostWanted', 'C': 'Carbon', 'PS': 'ProStreet', 'UC': 'Undercover', 'W': 'World', 'WB': 'World'}

# VaultLib.Core's references, which its classes read as one property: their size
# and the parts Attribulator's scripts name (it takes Class and Collection for a
# RefSpec's ClassKey and CollectionKey too).
CORE = {'RefSpec': (12, [('ClassKey', 0, 4), ('CollectionKey', 4, 4), ('Class', 0, 4), ('Collection', 4, 4)]),
        'RefSpecPacked': (8, [('ClassKey', 0, 4), ('CollectionKey', 4, 4), ('Class', 0, 4), ('Collection', 4, 4)]),
        'AttributeRefSpec': (8, [('ClassKey', 0, 4), ('DefinitionKey', 4, 4)])}

READS = {'ReadSingle': 4, 'ReadUInt32': 4, 'ReadInt32': 4, 'ReadByte': 1, 'ReadSByte': 1, 'ReadBoolean': 1,
         'ReadUInt16': 2, 'ReadInt16': 2, 'ReadUInt64': 8, 'ReadInt64': 8, 'ReadDouble': 8}


def plain(name):
    """A name without case, separators or a leading m."""
    if len(name) > 1 and name[0] == 'm' and name[1].isupper():
        name = name[1:]
    return re.sub(r'[^a-z0-9]', '', name.lower())


def body(src, at):
    """The text between the brace at or after `at` and its match."""
    i = src.index('{', at)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i + 1:j]
    return src[i + 1:]


def parse_file(path):
    """{type name: dict(file, props=[(C# type, name)], read, pointers)} for each
    class in the file that VLTTypeInfo registers."""
    src = open(path, encoding='utf-8-sig', errors='replace').read()
    out = {}
    for m in re.finditer(r'((?:\[VLTTypeInfo\([^\]]*\)\]\s*)+)(?:\[[^\]]*\]\s*)*public (?:sealed )?class (\w+)', src):
        names = [a or b for a, b in re.findall(r'VLTTypeInfo\(\s*(?:"([^"]*)"|nameof\((\w+)\))', m.group(1))]
        cls = body(src, m.end())
        props = re.findall(r'public ([\w<>\[\],\. ]+?) (\w+) \{ get; set; \}', cls)
        rd = re.search(r'public override void Read\(Vault \w+, BinaryReader br\)', cls)
        ptr = re.search(r'public void ReadPointerData\(Vault \w+, BinaryReader br\)', cls)
        info = dict(file=os.path.relpath(path, VAULTLIB).replace(os.sep, '/'), cls=m.group(2), props=props,
                    read=body(cls, rd.end()) if rd else None, pointers=body(cls, ptr.end()) if ptr else '')
        for n in names:
            out[n] = info
    return out


def classes(game):
    """The VaultLib classes a game registers, by type name: SpeedFramework's, then
    the game's own over them."""
    out = {}
    for folder in ('VaultLib.Frameworks.Speed', 'VaultLib.Support.' + FOLDERS[game]):
        for root, _, files in os.walk(os.path.join(VAULTLIB, folder)):
            for f in sorted(files):
                if f.endswith('.cs'):
                    out.update(parse_file(os.path.join(root, f)))
    return out


ENUM_TYPES = {'byte': 1, 'sbyte': 1, 'short': 2, 'ushort': 2, 'int': 4, 'uint': 4, 'long': 8, 'ulong': 8}


def enum_size(name):
    """The bytes ReadEnum<name> reads: its C# underlying type's (int unless said)."""
    if not hasattr(enum_size, 'table'):
        enum_size.table = {}
        for root, _, files in os.walk(VAULTLIB):
            for f in files:
                if f.endswith('.cs'):
                    src = open(os.path.join(root, f), encoding='utf-8-sig', errors='replace').read()
                    for m in re.finditer(r'\benum (\w+)\s*(?::\s*(\w+))?\s*\{', src):
                        enum_size.table[m.group(1)] = ENUM_TYPES.get(m.group(2) or 'int', 4)
    return enum_size.table.get(name)


def member_type(sch, t, path):
    """The type of the member a path (without +N) names in struct t."""
    st = t
    for c in path.split('.'):
        name = re.sub(r'\[\d+\]$', '', c)
        m = next((x for x in sch.structs.get(st, {}).get('members', []) if x['name'] == name), None)
        if m is None:
            return None
        st = m['type']
    return st


def member_at(sch, t, off):
    """Our top-level member of struct t starting at off, padding left out."""
    for m in sch.structs[t]['members']:
        if m['off'] == off and not PADDING.match(m['name']):
            return m
    return None


def decode(sch, t, info):
    """[(property, offset, size, the property's own offset)] for the public
    properties Read() fills, in order (a part of a reference: Name.ClassKey),
    and the problem that stopped the reading (None when it read to the end)."""
    if not info['read']:
        return [], 'no Read method'
    st = sch.structs[t]
    pub = {n for _, n in info['props']}
    ctype = {n: c for c, n in info['props']}
    # A private field ReadPointerData hands to a property: _xArray.Data into XValues,
    # or the position of _ptrPackages before Entries is read.
    priv = {}
    for a, b in re.findall(r'(\w+) = (_\w+)\.(?:Data|Value|Items)\b', info['pointers']):
        priv[b] = a
    for b, a in re.findall(r'Position = (_\w+);\s*(?:\w+ \w+ = [^;]*;\s*)*(\w+) = new', info['pointers']):
        priv[b] = a
    out = []
    off = 0
    for s in [x.strip() for x in info['read'].split(';')]:
        if not s or s.startswith('//'):
            continue
        s = re.sub(r'^//[^\n]*\n\s*', '', s)
        m = re.match(r'^(\w+) = (?:\(\w+\) ?)?br\.(\w+)\(\)$', s)
        if m and m.group(2) in READS:
            name, size = m.group(1), READS[m.group(2)]
        elif re.match(r'^(\w+) = br\.ReadEnum<[\w\.]+>\(\)$', s):
            name = s.split(' ')[0]
            size = enum_size(re.search(r'<([\w\.]+)>', s).group(1).split('.')[-1])
        elif re.match(r'^(\w+)\.Read\(\w+, br\)$', s):
            name = s.split('.')[0]
            mm = member_at(sch, t, off)
            size = (sch.size_of(mm['type']) or 0) * mm['n'] if mm else None
            core = CORE.get(ctype.get(name, '').split('.')[-1])
            if core and core[0] != size:
                # One property over several of our members: its parts, each its own.
                if name in pub:
                    out += [(name + '.' + n, off + o, z, off) for n, o, z in core[1]]
                off += core[0]
                continue
        elif re.match(r'^br\.(\w+)\(\)$', s) and s[3:-2] in READS:
            off += READS[s[3:-2]]
            continue
        elif re.match(r'^br\.AlignReader\((\d+)\)$', s):
            n = int(re.match(r'^br\.AlignReader\((\d+)\)$', s).group(1))
            off = (off + n - 1) // n * n
            continue
        elif re.match(r'^br\.ReadBytes\((\d+)\)$', s):
            off += int(re.match(r'^br\.ReadBytes\((\d+)\)$', s).group(1))
            continue
        else:
            return out, 'stops at "%s"' % s.replace('\n', ' ')
        if not size:
            return out, 'no member of ours at %d for %s' % (off, name)
        name = priv.get(name, name)
        if name in pub:
            out.append((name, off, size, off))
        off += size
    if off != st['size'] and (off + 3) // 4 * 4 != st['size']:
        return out, 'reads %d bytes of %d' % (off, st['size'])
    return out, None


def aliases(sch, t, info):
    """(aliases, clashes, problem): aliases are (VaultLib name, our member path) for
    the names that aren't ours; clashes (name, its offset, our member, its offset)
    the names VaultLib reads where we have another member (a bitfield of ours aside,
    which VaultLib may read a byte at a time). A type with a clash gets no aliases."""
    reads, problem = decode(sch, t, info)
    if problem and not problem.startswith('stops at'):
        return [], [], problem  # the reads don't line up with ours: none of them holds
    own = {plain(m['name']): m for m in sch.structs[t]['members']}
    out, clash = [], []
    for name, off, size, start in reads:
        head = name.split('.')[0]
        mine = own.get(plain(head))
        if mine is not None:
            # Ours, and then its part as our own reading takes it (mEmitter ClassKey).
            if mine['off'] != start and not mine['bits'] and head not in [c[0] for c in clash]:
                clash.append((head, start, mine['name'], mine['off']))
            continue
        path, names = sch.path(t, off, size)
        if not path or '+' in path:
            continue
        leaf = [x for x in sch.leaves(t) if '.'.join(x[2]) == path]
        if leaf and leaf[0][3] in ('ptr', 'cstr'):
            continue  # a pointer the game sets (a reference's mCollectionPtr): nothing sets it
        # A struct of one member (type_bStringHash's hash) is one value to the editor: the member holding it.
        while '.' in path:
            parent = path.rsplit('.', 1)[0]
            ptype = member_type(sch, t, parent)
            if ptype not in sch.structs or len(sch.structs[ptype]['members']) != 1:
                break
            path = parent
        got = sch.resolve(t, path)
        if got[0] != off or got[1] != size:
            continue
        out.append((name, path))
    # A name that is ours elsewhere would read one way here and another there.
    return ([] if clash else out), clash, problem


def blocks(sch, game):
    """The table's vaultlib blocks for a game, and the clashes, as lines."""
    out = []
    for t, info in sorted(classes(game).items(), key=lambda kv: kv[0].lower()):
        if t not in sch.structs:
            continue
        al, clash, _ = aliases(sch, t, info)
        if not al and not clash:
            continue
        b = ['vaultlib ' + t, '  # ' + info['file']]
        b += ['  %s %s' % (n, p) for n, p in al]
        for n, o, mine, mo in clash:
            b.append('  # not taken: VaultLib reads %s at byte %d, where our %s is; our %s is at %d. Our names win.'
                     % (n, o, sch.path(t, o, 1)[0] or 'padding', mine, mo))
        b.append('end')
        out.append('\n'.join(b))
    return out
