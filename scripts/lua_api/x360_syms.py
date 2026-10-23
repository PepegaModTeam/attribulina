#!/usr/bin/env python3
"""Writes the Xbox 360 milestone's public symbols, one per line: <address> <mangled name>.
    python3 -I -B x360_syms.py <repo root> > milestone_syms.txt"""
import os, sys
sys.path.insert(0, os.path.join(sys.argv[1], 'tools', 'xex'))
import symmap
for a in symmap.ADDRS:
    print('%08x %s' % (a, symmap.SYMS[a]))
