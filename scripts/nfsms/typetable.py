"""The type table of a game's attributes.bin: (name, size) per type, in order.

Reads the DatabaseLoadData export the way src/core/Database.cpp does: its type
count at +8, the type names behind the pointer at +12 (a PtrN fixup into the
bin), the sizes from +16. Either byte order, and Most Wanted's 20-byte ExpN
entries.
"""

import os
import struct

# The PC databases each table describes. World's final and beta builds stand in
# for NFS-VltEd's W and WB (both betas together for WB).
FOLDERS = {
    'MW': ['pc/mw-vanilla'],
    'C': ['pc/carbon-vanilla'],
    'PS': ['pc/prostreet-vanilla'],
    'UC': ['pc/undercover-vanilla'],
    'W': ['pc/world-final'],
    'WB': ['pc/world-2009-beta', 'pc/world-2010-beta'],
}


def string_hash32(s, c=0xABCDEF00):
    """Attrib's StringHash32 (lookup2)."""
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

    k = s.encode('latin-1')
    n = len(k)
    a = b = 0x9E3779B9
    i = 0
    while n - i >= 12:
        a = (a + int.from_bytes(k[i:i + 4], 'little')) & M
        b = (b + int.from_bytes(k[i + 4:i + 8], 'little')) & M
        c = (c + int.from_bytes(k[i + 8:i + 12], 'little')) & M
        a, b, c = mix(a, b, c)
        i += 12
    c = (c + n) & M
    r = k[i:] + bytes(11)
    a = (a + int.from_bytes(r[0:4], 'little')) & M
    b = (b + int.from_bytes(r[4:8], 'little')) & M
    c = (c + (int.from_bytes(r[8:11], 'little') << 8)) & M
    return mix(a, b, c)[2]


def fourcc(s):
    return int.from_bytes(s.encode('latin-1'), 'big')


def read_types(path):
    """[(name, size)] from the DatabaseLoadData in a pack."""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'VPAK':
        raise SystemExit('%s: not a VPAK pack' % path)
    count = struct.unpack_from('<I', d, 4)[0]
    want = string_hash32('Attrib::DatabaseLoadData')
    for i in range(count):
        _, binsz, vltsz, binoff, vltoff = struct.unpack_from('<5I', d, 0x10 + i * 0x14)
        vlt = d[vltoff:vltoff + vltsz]
        bin_ = d[binoff:binoff + binsz]
        e = next((en for en in '<>' if struct.unpack_from(en + 'I', vlt, 0)[0] in (fourcc('Vers'), fourcc('DepN'))), None)
        if e is None:
            raise SystemExit('%s: a vault starts with neither Vers nor DepN' % path)

        def u32(off):
            return struct.unpack_from(e + 'I', vlt, off)[0]

        chunks = {}
        off = 0
        while off + 8 <= len(vlt):
            cid, size = u32(off), u32(off + 4)
            if size < 8:
                break
            chunks.setdefault(cid, (off + 8, size - 8))
            off += size
        dat, datsize = chunks[fourcc('DatN')]
        p, _ = chunks[fourcc('ExpN')]
        n = u32(p)

        def exports(size):
            out = []
            for k in range(n):
                q = p + 4 + k * size
                x = (u32(q), u32(q + 4), u32(q + size - 8), u32(q + size - 4))  # id, type, size, offset
                if x[3] < dat or x[3] + x[2] > dat + datsize:
                    return None
                out.append(x)
            return out

        exps = exports(16) or exports(20) or []
        p, size = chunks[fourcc('PtrN')]
        fixups = {}
        block = -1
        for q in range(p, p + size - 11, 12):
            kind, index = struct.unpack_from(e + 'HH', vlt, q + 4)
            if kind == 2:
                block = index
            elif kind == 3 and block == 0:
                fixups[u32(q)] = u32(q + 8)
        for _, etype, _, eoff in exps:
            if etype != want:
                continue
            out = []
            o = fixups[eoff + 12]
            for k in range(u32(eoff + 8)):
                z = bin_.index(b'\0', o)
                out.append((bin_[o:z].decode('latin-1'), u32(eoff + 16 + 4 * k)))
                o = z + 1
            return out
    raise SystemExit('%s: no DatabaseLoadData' % path)


def attributes_bin(folder):
    for f in sorted(os.listdir(folder)):
        if f.lower() == 'attributes.bin':
            return os.path.join(folder, f)
    raise SystemExit('%s: no attributes.bin' % folder)


def game_types(root, game):
    """name -> size for a game's PC database (the first folder that has a type wins)."""
    out = {}
    for folder in FOLDERS[game]:
        for name, size in read_types(attributes_bin(os.path.join(root, 'attrib', 'databases', folder))):
            out.setdefault(name, size)
    return out


# The editor's own kinds for built-in types (src/core/Database.cpp ClassifyType,
# TypeKindName): a name it knows at the size it expects, else bytes.
KINDS = {
    'EA::Reflection::Bool': ('bool', 1), 'EA::Reflection::Int8': ('int8', 1), 'EA::Reflection::UInt8': ('uint8', 1),
    'EA::Reflection::Int16': ('int16', 2), 'EA::Reflection::UInt16': ('uint16', 2),
    'EA::Reflection::Int32': ('int32', 4), 'EA::Reflection::UInt32': ('uint32', 4),
    'EA::Reflection::Int64': ('int64', 8), 'EA::Reflection::UInt64': ('uint64', 8),
    'EA::Reflection::Float': ('float', 4), 'EA::Reflection::Double': ('double', 8),
    'EA::Reflection::Char': ('char', 1), 'EA::Reflection::Text': ('text', 4),
    'Attrib::StringKey': ('string key', 8), 'Attrib::RefSpec': ('reference', 12), 'Attrib::Key': ('key', 4),
    'UCrc32': ('key', 4), 'GCollectionKey': ('key', 4), 'DefinitionKey': ('key', 4),
    'type_bStringHash': ('bin key', 4), 'Attrib::Types::Colour': ('colour', 4),
    'Attrib::Types::FloatColour': ('float colour', 16), 'Attrib::Types::ShaderColour': ('float colour', 16),
    'Attrib::Types::Vector2': ('vector2', 8), 'Attrib::Types::Vector3': ('vector3', 12),
    'Attrib::Types::Vector4': ('vector4', 16), 'Attrib::Types::Quaternion': ('quaternion', 16),
    'Attrib::Types::Matrix': ('matrix', 64), 'Attrib::Blob': ('blob', 8), 'Curve': ('curve', 44),
}


def kind_of(name, size):
    k = KINDS.get(name)
    if k is None:
        return 'bytes'
    if k[1] != size and not (k[0] == 'bool' and size == 4):
        return 'bytes'
    return k[0]
