#pragma once

// Version-specific entry points behind Lua.h.

#include "core/lua/Lua.h"

namespace attrib::lua {

struct Chunk;
struct Function;

CompileResult Compile50(std::string_view source);
DecompileResult Decompile50(const Chunk& chunk);

// Appends a listing of f and its nested functions (Disassemble's format).
void ListFunction(const Function& f, const std::string& name, std::string& out);

} // namespace attrib::lua
