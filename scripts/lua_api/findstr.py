#!/usr/bin/env python3
"""Finds NUL-terminated strings in an executable and prints their offsets.
    python3 -I -B findstr.py <exe> <word> [<word> ...]"""
import sys, re
data = open(sys.argv[1], 'rb').read()
for w in sys.argv[2:]:
    pat = re.compile(rb'(?<=\x00)' + re.escape(w.encode()) + rb'\x00')
    hits = [m.start() for m in pat.finditer(data)]
    print('%-22s %s' % (w, ' '.join('%x' % h for h in hits[:6]) or '-'))
