"""Paths and helpers shared by the nfsms word table scripts.

The NFS-VltEd decompilation names its files after the obfuscated identifiers:
the class \\u0007\\u0014 is A/_0007_0014.cs. An identifier that starts with
\\u0020 became a file name with a leading space ("A/ _0010.cs"); the tables
write it as A/_0020_0010.cs.
"""

import functools
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VLTED = os.path.join(REPO, 'NFS-VltEd')
CLASSES = os.path.join(VLTED, 'A')
REGISTRY = os.path.join(CLASSES, '_0007_0014.cs')

IDENT = r'(?:\\u[0-9A-F]{4})+'


def ident_file(ident):
    """The file of an identifier written as \\uXXXX escapes."""
    name = ''
    for p in re.findall(r'\\u([0-9A-Fa-f]{4})', ident):
        name += ' ' if p == '0020' else '_' + p.upper()
    return os.path.join(CLASSES, name + '.cs')


def ident_name(ident):
    """An identifier as the tables name its file: _XXXX_XXXX."""
    return '_' + '_'.join(re.findall(r'\\u([0-9A-Fa-f]{4})', ident))


def class_file(cls):
    """A/_0012_0019.cs (the table form) to the file's path."""
    segs = cls[2:-3].split('_')[1:]
    return os.path.join(CLASSES, ''.join(' ' if s == '0020' else '_' + s for s in segs) + '.cs')


def rel(path):
    """A class file's path as the tables write it."""
    r = os.path.relpath(path, VLTED).replace(os.sep, '/')
    return r.replace('A/ _', 'A/_0020_')


@functools.lru_cache(None)
def read(path):
    try:
        with open(path, encoding='utf-8-sig') as f:
            return f.read()
    except FileNotFoundError:
        return None


@functools.lru_cache(None)
def delegate_sig(ident):
    """What a proxy delegate or static helper declares: ('delegate', return type,
    parameters), ('static', ...), ('field', type) or ('?',)."""
    s = read(ident_file(ident))
    if s is None:
        return None
    m = re.search(r'internal delegate (.+?) (?:\\u[0-9A-F]{4})+\((.*?)\);', s, re.S)
    if m:
        return ('delegate', m.group(1).strip(), re.sub(r'\s+', ' ', m.group(2)).strip())
    m = re.search(r'internal static (.+?) \\u0011\((.*?)\) => (.*?);', s, re.S)
    if m:
        return ('static', m.group(1).strip(), m.group(2).strip(), m.group(3).strip())
    m = re.search(r'internal static (.+?) \\u0011;', s)
    if m:
        return ('field', m.group(1).strip())
    return ('?',)


def method_body(src, header_regex):
    """The text between the braces of the first method whose header matches."""
    m = re.search(header_regex, src)
    if not m:
        return None
    i = src.index('{', m.end() - 1) if src[m.end() - 1] != '{' else m.end() - 1
    depth = 0
    j = i
    while True:
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i + 1:j]
        j += 1


NOISE = re.compile(r'^(label_\d+:|switch \(\d\)|case 0:|goto label_\d+;|default:|break;|if \(false\)|'
                   r'// ISSUE.*|RuntimeMethodHandle runtimeMethodHandle.*|\{|\}|)$')


def clean(body):
    """A method body's statements without the obfuscator's dead switches."""
    return [l.strip() for l in body.split('\n') if not NOISE.match(l.strip())]
