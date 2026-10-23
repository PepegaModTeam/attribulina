#!/usr/bin/env bash
# Compile-checks the Windows editor (Win32 + D3D11 host included) on a non-Windows
# machine by cross-compiling with Zig's clang for x86_64-windows-gnu, then links
# Attribulina.exe and attribcli.exe. It can't run them; it catches Windows-only
# compile and link errors before an MSVC build does.
#
# Usage: scripts/check_windows_build.sh /path/to/zig [outdir]
# Get Zig from https://ziglang.org/download/ (tested with 0.16.0).
set -euo pipefail

ZIG=${1:?usage: $0 /path/to/zig [outdir]}
OUT=${2:-$(mktemp -d)}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p "$OUT"
export ZIG_GLOBAL_CACHE_DIR="$OUT/zig-cache" ZIG_LOCAL_CACHE_DIR="$OUT/zig-cache"

FLAGS=(-target x86_64-windows-gnu -std=c++20 -O1 -DNOMINMAX
       -Isrc -Ithirdparty/imgui -Ithirdparty/imgui/backends -Ithirdparty/imgui/misc/cpp -IBBXCompression
       -Ithirdparty/lua50 -Ithirdparty/lua51)

jobs=()
for f in src/core/*.cpp src/core/lua/*.cpp src/app/*.cpp; do jobs+=("$f|-Wall -Wextra"); done
jobs+=("src/platform/win32/main_win32.cpp|-Wall -Wextra -DUNICODE -D_UNICODE")
for f in thirdparty/imgui/*.cpp thirdparty/imgui/misc/cpp/imgui_stdlib.cpp \
         thirdparty/imgui/backends/imgui_impl_win32.cpp thirdparty/imgui/backends/imgui_impl_dx11.cpp; do
    jobs+=("$f|-w")
done
# Lua 5.0.1, compiled as C++ in namespace lua50 (see thirdparty/lua50/lua50_config.h).
for f in thirdparty/lua50/*.cpp thirdparty/lua51/*.cpp; do jobs+=("$f|-w"); done
for f in huffdecode huffencode jlzdecode jlzencode refdecode refencode OldLZDecompress libcodex; do
    jobs+=("BBXCompression/$f.cpp|-w -include cstdlib -include cstring")
done

# attribcli's own sources go in cli/, so the editor's link doesn't see its main.
mkdir -p "$OUT/cli"
for f in src/cli/*.cpp; do jobs+=("$f|-Wall -Wextra -DUNICODE -D_UNICODE|cli/"); done

status=0
for j in "${jobs[@]}"; do
    src=${j%%|*}
    rest=${j#*|}
    extra=${rest%%|*}
    sub=""
    [ "$rest" != "$extra" ] && sub=${rest#*|}
    obj="$OUT/$sub$(echo "$src" | tr '/' '_').o"
    # shellcheck disable=SC2086
    if ! "$ZIG" c++ "${FLAGS[@]}" $extra -c "$src" -o "$obj"; then
        echo "FAILED: $src"
        status=1
    fi
done
[ $status -eq 0 ] || exit $status

# MinGW names the shader compiler import library d3dcompiler_47 (MSVC: d3dcompiler).
"$ZIG" c++ -target x86_64-windows-gnu -municode -mwindows "$OUT"/*.o \
    -ld3d11 -ldxgi -ld3dcompiler_47 -ldwmapi -lgdi32 -luser32 -lole32 -lshell32 -luuid -lcomdlg32 -limm32 -lwindowscodecs \
    -o "$OUT/Attribulina.exe" > "$OUT/link.log" 2>&1 || { grep -E "error" "$OUT/link.log" | grep -v "warning:" | head -20; exit 1; }
echo "OK: $OUT/Attribulina.exe"

"$ZIG" c++ -target x86_64-windows-gnu -municode "$OUT"/src_core_*.o "$OUT"/thirdparty_lua50_*.o "$OUT"/thirdparty_lua51_*.o "$OUT"/BBXCompression_*.o \
    "$OUT"/cli/*.o -o "$OUT/attribcli.exe" > "$OUT/link_cli.log" 2>&1 || { grep -E "error" "$OUT/link_cli.log" | grep -v "warning:" | head -20; exit 1; }
echo "OK: $OUT/attribcli.exe"
