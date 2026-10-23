#pragma once

// Compiled Lua chunks from gameplay `bytecode` blobs: reading, disassembling,
// decompiling and compiling.
//
// The chunk is what's inside the blob's LZHeader block (Compression.h unpacks
// it). Every game and platform stores it little-endian with a 4-byte float
// lua_Number and 4-byte size_t, so stock luac can't read or write it:
//   Lua 5.0 (MW, Carbon, Pro Street): 1B 'Lua' 50 01 04 04 04 06 08 09 09 04, test number (float)
//   Lua 5.1 (Undercover, World):      1B 'Lua' 51 00 01 04 04 04 04 00
// Retail chunks are stripped (no line info, local or upvalue names). Each one
// defines a single global handler function named HandlerFunctionName(<the
// messagehandler collection's name>) taking (self, message, vars); Most
// Wanted names it after the collection itself (its CollectionName field,
// which the game looks the function up by). Both versions are implemented,
// each with its own embedded compiler (packages/lua50, packages/lua51) and
// decompiler. Inspect, Disassemble and Decompile take the version from the
// chunk's header; Compile takes it from the caller. doc/lua.md has the
// formats and how the decompilers work.

#include "core/Bytes.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace attrib::lua {

enum class Version : uint8_t { Lua50, Lua51 };

// The chunk's Lua version from its header; nullopt if it isn't a chunk in the
// game format.
std::optional<Version> DetectVersion(ByteSpan chunk);

// "h_" + StringHash32(name) as 8 uppercase hex digits:
// "basicrace/stategraph_basicrace_racing_handler_triggerenter" -> "h_2B6A5CD7".
// "h_" + StringHash32 of the name in uppercase hex; `named` (Most Wanted): the name itself.
std::string HandlerFunctionName(std::string_view handlerCollectionName, bool named = false);

// What a chunk touches, for listings and checks.
struct ChunkInfo {
    Version version = Version::Lua50;
    uint32_t functions = 0;                 // including the main chunk
    uint32_t instructions = 0;              // over all functions
    std::vector<std::string> globalsSet;    // SETGLOBAL names in the main chunk, in order
    std::vector<std::string> globalsRead;   // GETGLOBAL names anywhere, sorted, unique
    std::vector<std::string> methodsCalled; // `obj:name(...)` names anywhere, sorted, unique
};
// Throws FormatError on a malformed or unsupported chunk.
ChunkInfo Inspect(ByteSpan chunk);

// A luac -l style listing with constants resolved. Throws FormatError.
std::string Disassemble(ByteSpan chunk);

struct DecompileResult {
    std::string source;
    // Anything the decompiler couldn't express exactly; the source still
    // compiles but may not reproduce the original bytecode.
    std::vector<std::string> warnings;
    // Compiling `source` gives back the chunk's code, constants and functions
    // (Decompile checks this itself).
    bool exact = false;
};
// Throws FormatError on a malformed or unsupported chunk.
DecompileResult Decompile(ByteSpan chunk);

// Compiles `source` with the given version's compiler (Lua 5.0.1 or 5.1.3,
// lua_Number = float) into a stripped chunk in the game format.

struct CompileResult {
    Bytes chunk;       // in the game format, stripped like retail chunks
    std::string error; // empty on success: "line N: message"
    int line = 0;      // 1-based line of the error, 0 if none
    bool ok() const { return error.empty(); }
};
CompileResult Compile(std::string_view source, Version version = Version::Lua50);

} // namespace attrib::lua
