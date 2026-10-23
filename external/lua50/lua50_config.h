// Build configuration for the embedded Lua 5.0.1 core (src/, unmodified).
//
// Each lua50_*.cpp wrapper compiles one Lua source file as C++ inside
// namespace lua50, so none of its symbols (lua_*, luaX_*, ...) clash with a
// second Lua version linked into the same program. The C headers the Lua
// sources use are included here first, outside the namespace, so their
// include guards keep them out of it.
//
// The games compiled their scripts with lua_Number = float. Numbers are still
// read with strtod and then rounded to float (Lua's default lua_str2number).

#pragma once

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUA_NUMBER float
#define LUA_NUMBER_SCAN "%f"
#define LUA_NUMBER_FMT "%.9g"
