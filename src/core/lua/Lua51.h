#pragma once

// Lua 5.1 entry points behind Lua.h.

#include "core/lua/Lua.h"

namespace attrib::lua {

struct Chunk;
struct Function;

CompileResult Compile51(std::string_view source);
DecompileResult Decompile51(const Chunk& chunk);

// Appends a listing of a Lua 5.1 function and its nested functions
// (Disassemble's format).
void ListFunction51(const Function& f, const std::string& name, std::string& out);

} // namespace attrib::lua
