"""Merges the bStringHash name lists into data/bin_keys.txt.

    python3 scripts/merge_bin_keys.py

The sources are scripts/bin_keys/BinaryMainKeys<Game>.txt, one name per line:
the lists for each game (UndercoverCG is current-gen Undercover, PC and 360;
Undercover is its Wii and PS2 version), with the names found for Pro Street,
Nitro, Undercover and Carbon appended to their game's list. The editor and
attribcli read only data/bin_keys.txt.

Two names can share a hash. The editor keeps the first name it reads for a
hash, so the merged file holds one name per hash, chosen by these rules in
order: the name more lists have; a name without spaces at its ends; the name
from the list earliest in GAMES; the name first in byte order. Every such
choice is printed. The output is sorted by bytes, with CRLF line ends.

The editor also adds attrib_keys.txt's names to the same dictionary before
this file, so a name there wins over this file's for the same bStringHash; the
script prints those too. A string in a loaded vault wins over both."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = os.path.join(ROOT, 'scripts', 'bin_keys')
OUT = os.path.join(ROOT, 'data', 'bin_keys.txt')
# AttribSys games first, then the older games' lists.
GAMES = ['ProStreet', 'Carbon', 'UndercoverCG', 'Undercover', 'Nitro', 'World', 'MostWanted', 'Shared',
         'Underground2', 'Underground1', 'HotPursuit2']


def bin_hash(name):
    h = 0xFFFFFFFF
    for c in name:
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def read_lines(path):
    out = []
    for line in open(path, 'rb').read().split(b'\n'):
        line = line.rstrip(b'\r')
        if line:
            out.append(line)
    return out


def main():
    files = sorted(f for f in os.listdir(SOURCES) if f.startswith('BinaryMainKeys') and f.endswith('.txt'))
    games = [f[len('BinaryMainKeys'):-4] for f in files]
    unknown = [g for g in games if g not in GAMES]
    if unknown:
        sys.exit('merge_bin_keys.py: add %s to GAMES' % ', '.join(unknown))
    lists = {g: read_lines(os.path.join(SOURCES, 'BinaryMainKeys%s.txt' % g)) for g in games}
    holders = {}
    for g in games:
        for n in lists[g]:
            holders.setdefault(n, set()).add(g)
    by_hash = {}
    for n in holders:
        by_hash.setdefault(bin_hash(n), []).append(n)

    def rank(n):
        return (-len(holders[n]), n != n.strip(), min(GAMES.index(g) for g in holders[n]), n)

    chosen = []
    clashes = 0
    for h, names in sorted(by_hash.items()):
        names.sort(key=rank)
        chosen.append(names[0])
        if len(names) > 1:
            clashes += 1
            print('%08X %s (%s) over %s' % (h, names[0].decode('latin-1'), ', '.join(sorted(holders[names[0]])),
                                            ', '.join('%s (%s)' % (n.decode('latin-1'), ', '.join(sorted(holders[n])))
                                                      for n in names[1:])))
    chosen.sort()
    with open(OUT, 'wb') as f:
        f.write(b''.join(n + b'\r\n' for n in chosen))

    keys_path = os.path.join(ROOT, 'data', 'attrib_keys.txt')
    shadowed = 0
    if os.path.exists(keys_path):
        mine = {bin_hash(n): n for n in chosen}
        for k in read_lines(keys_path):
            n = mine.get(bin_hash(k))
            if n is not None and n != k:
                shadowed += 1
                print('attrib_keys.txt %s takes %08X from %s' % (k.decode('latin-1'), bin_hash(k), n.decode('latin-1')))
    print('%d names from %d lists, %d hashes in %s; %d hashes had more than one name, %d are taken by attrib_keys.txt'
          % (len(holders), len(games), len(chosen), os.path.relpath(OUT, ROOT), clashes, shadowed))


if __name__ == '__main__':
    main()
