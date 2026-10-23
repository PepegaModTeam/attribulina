#!/bin/sh
# Reruns the Lua API scan and writes lua-api.md, lua-api.txt and api.json into
# a work folder; copy them to doc/lua-api.md and data/lua/prostreet_pc.txt.
#   scripts/lua_api/run_all.sh [work folder]   (default: $TMPDIR/attribulina_lua_api)
# Needs: the repo's attrib/ databases, nfs-data/nfs.exe + nfs.map, nfs-data/ProStreet08Milestone.{pdb,h},
# tools/xex/image.bin, premake5, g++, python3. Writes nothing inside the repo.
set -e
S=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$S/../.." && pwd)
B=${1:-${TMPDIR:-/tmp}/attribulina_lua_api}
mkdir -p "$B/bin"
PY="python3 -I -B"
mkdir -p "$B/dump" "$B/x86" "$B/x360"

# 1. the repo's core libraries, built out of tree
L="$B/out/bin/Release"
if [ ! -f "$L/libattribcore.a" ]; then
    (cd "$R" && premake5 gmake --outdir="$B/out")
    make -C "$B/out/build" config=release_native attribcore bbxcompression lua50 -j4
fi
for p in dump_handlers gameplay_fields count_fields children dump_coll; do
    g++ -std=c++20 -O1 -I"$R/src" "$S/$p.cpp" -L"$L" -lattribcore -llua50 -lbbxcompression -o "$B/bin/$p"
done

# 2. the 251 handlers: metadata, decompiled source, listings, what they use
"$B/bin/dump_handlers" "$R" "$B/dump"
$PY "$S/corpus.py" "$B/dump" "$B/dump/corpus.lua"
$PY "$S/analyze.py" "$B/dump" > "$B/dump/uses.tsv"
"$B/bin/gameplay_fields" "$R" > "$B/dump/gameplay_fields.tsv"

# 3. nfs.exe: bindings, message and event definitions, message builders
$PY "$S/x360_syms.py" "$R" > "$B/x360/milestone_syms.txt"
$PY "$S/scan_calls.py" "$R" 6DD000 6F8000 > "$B/x86/calls_6dd.tsv"
$PY "$S/consolidate.py" "$R" "$B/x86/calls_6dd.tsv" > "$B/x86/bindings_pc.tsv"
$PY "$S/signatures.py" "$B/x86/bindings_pc.tsv" "$B/x360/milestone_syms.txt" > "$B/x86/sigs.tsv"
$PY "$S/msg_table.py" "$R" > "$B/x86/messages_pc.tsv"
$PY "$S/lookup_table.py" "$R" "$B/x86/messages_pc.tsv" 98ba74 > "$B/x86/lookup_pc.tsv"
$PY "$S/event_defs.py" "$R" > "$B/x86/events_pc.tsv"
$PY "$S/builders.py" "$R" "$B/x86/messages_pc.tsv" > "$B/x86/builders_pc.tsv"

# 4. merge and write the reference
$PY "$S/build_api.py" "$B" "$R"
$PY "$S/gen_doc.py" "$B"
