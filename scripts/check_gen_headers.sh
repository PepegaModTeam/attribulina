#!/usr/bin/env bash
# Compiles the Attrib::Gen headers Attribulina writes (core/GenHeader.h) for
# every class of the PC games, and a class made in the editor, against
# gen_headers/sdk_stub.h as 32-bit Windows code with Zig's clang. The game's
# sizes and alignments then check every _LayoutStruct's static_asserts.
#
# Usage: scripts/check_gen_headers.sh /path/to/zig [outdir]
# Needs bin/Debug/attribtests (premake5 gmake && make -C build config=debug_native).
set -euo pipefail

ZIG=${1:?usage: $0 /path/to/zig [outdir]}
OUT=${2:-$(mktemp -d)}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export ZIG_GLOBAL_CACHE_DIR="$OUT/zig-cache" ZIG_LOCAL_CACHE_DIR="$OUT/zig-cache"

ATTRIB_GEN_HEADERS_DIR="$OUT/headers" "$ROOT/bin/Debug/attribtests" --root "$ROOT" "classes: Attrib::Gen headers"

fail=0
for dir in "$OUT"/headers/*/; do
    name=$(basename "$dir")
    defs=()
    case "$name" in mw_*) defs=(-DATTRIB_GEN_MW) ;; esac
    # An object, not -fsyntax-only: zig's driver wants an output to write.
    if "$ZIG" c++ -target x86-windows-gnu -std=c++17 -c -o "$OUT/$name.o" "${defs[@]}" -I "$ROOT/scripts/gen_headers" -I "$dir" "$dir/all.cpp"; then
        echo "ok    $name"
    else
        echo "FAIL  $name"
        fail=1
    fi
done
exit $fail
