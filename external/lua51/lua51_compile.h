// Entry point into the embedded Lua 5.1.3 compiler (lua51_compile.cpp).
// Kept free of Lua's own headers so callers don't see Lua's internals, and
// so it can sit beside lua50_compile.h in one translation unit.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lua51 {

struct CompiledConstant {
    enum Type : int { Nil = 0, Boolean = 1, Number = 3, String = 4 } type = Nil;
    bool boolean = false;
    float number = 0;
    std::string string;
};

// One compiled function, without debug information (line info, local and
// upvalue names, source), which the games' chunks don't carry.
struct CompiledFunction {
    int lineDefined = 0;
    int lastLineDefined = 0;
    int upvalues = 0;
    int params = 0;
    int vararg = 0; // VARARG_* flags
    int maxStackSize = 0;
    std::vector<uint32_t> code;
    std::vector<CompiledConstant> constants;
    std::vector<CompiledFunction> functions;
};

// Parses and compiles Lua 5.1 source text. On a syntax error, returns false
// with Lua's message ("<chunkName>:<line>: <message> near '<token>'") in error.
bool CompileSource(const char* source, size_t size, const char* chunkName, CompiledFunction& out,
                   std::string& error);

} // namespace lua51
