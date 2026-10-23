// Build configuration for the embedded Lua 5.1.3 core (src/, unmodified).
//
// Each lua51_*.cpp wrapper compiles one Lua source file as C++ inside
// namespace lua51, so none of its symbols (lua_*, luaX_*, ...) clash with
// the Lua 5.0 core (namespace lua50) linked into the same program. The C
// headers the Lua sources use are included here first, outside the
// namespace, so their include guards keep them out of it.
//
// The games compiled their scripts with lua_Number = float. luaconf.h sets
// double unconditionally, so it is included here (it only defines macros in
// this configuration) and its number settings are replaced. Numbers are
// still read with strtod and then rounded to float, as lua_str2number does.

#pragma once

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Every core file defines LUA_CORE before its first include; luaconf.h's
// number operations are only visible with it.
#define LUA_CORE
#include "src/luaconf.h"

#undef LUA_NUMBER_DOUBLE
#undef LUA_NUMBER
#define LUA_NUMBER float
#undef LUA_NUMBER_SCAN
#define LUA_NUMBER_SCAN "%f"
#undef LUA_NUMBER_FMT
#define LUA_NUMBER_FMT "%.9g"
#undef lua_number2int
#define lua_number2int(i, d) ((i) = (int)(d))
#undef lua_number2integer
#define lua_number2integer(i, d) ((i) = (lua_Integer)(d))
