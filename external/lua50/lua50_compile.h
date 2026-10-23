// Entry point into the embedded Lua 5.0.1 compiler (lua50_compile.cpp).
// Kept free of Lua's own headers so callers don't see Lua's internals.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lua50 {

struct CompiledConstant {
    enum Type : int { Nil = 0, Number = 3, String = 4 } type = Nil;
    float number = 0;
    std::string string;
};

// One compiled function, without debug information (line info, local and
// upvalue names), which the games' chunks don't carry.
struct CompiledFunction {
    int lineDefined = 0;
    int upvalues = 0;
    int params = 0;
    bool vararg = false;
    int maxStackSize = 0;
    std::vector<uint32_t> code;
    std::vector<CompiledConstant> constants;
    std::vector<CompiledFunction> functions;
};

// Parses and compiles Lua 5.0 source text. On a syntax error, returns false
// with Lua's message ("<chunkName>:<line>: <message> near `<token>'") in error.
bool CompileSource(const char* source, size_t size, const char* chunkName, CompiledFunction& out,
                   std::string& error);

} // namespace lua50
