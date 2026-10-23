#pragma once

// In-memory form of a compiled Lua chunk in the games' format, with a reader
// and a writer that reproduce the file byte for byte.
//
// Lua 5.0 game chunk (all fields little-endian, size_t and int 4 bytes):
//   header   1B 'Lua' 50 01 04 04 04 06 08 09 09 04, then 3.14159265358979323846e7 as float
//   function string source (u32 size including the NUL, 0 = NULL), i32 lineDefined,
//            u8 nups, u8 numparams, u8 is_vararg, u8 maxstacksize,
//            i32 n + n x i32 line info,
//            i32 n + n x (string name, i32 startpc, i32 endpc) locals,
//            i32 n + n x string upvalue names,
//            i32 n + n x (u8 type, value) constants (0 nil, 3 float, 4 string),
//            i32 n + n x function nested functions,
//            i32 n + n x u32 code
// A nested function's source is NULL when it equals its parent's.
//
// Lua 5.1 game chunk (ldump.c of 5.1, the same sizes):
//   header   1B 'Lua' 51 00 01 04 04 04 04 00 (format 0, little-endian, int,
//            size_t, Instruction and lua_Number 4 bytes, numbers not integral)
//   function string source, i32 lineDefined, i32 lastLineDefined,
//            u8 nups, u8 numparams, u8 is_vararg (VARARG_* flags), u8 maxstacksize,
//            i32 n + n x u32 code,
//            i32 n + n x (u8 type, value) constants (0 nil, 1 boolean u8, 3 float, 4 string),
//            i32 n + n x function nested functions,
//            i32 n + n x i32 line info,
//            i32 n + n x (string name, i32 startpc, i32 endpc) locals,
//            i32 n + n x string upvalue names
// Stripped 5.1 chunks have a NULL source everywhere.

#include "core/Bytes.h"
#include "core/lua/Lua.h"

#include <optional>
#include <string>
#include <vector>

namespace attrib::lua {

struct Constant {
    enum class Type : uint8_t { Nil = 0, Boolean = 1, Number = 3, String = 4 };
    Type type = Type::Nil;
    bool boolean = false; // Lua 5.1 only
    float number = 0;
    std::string string;

    static Constant Nil() { return {}; }
    static Constant Num(float v) { Constant c; c.type = Type::Number; c.number = v; return c; }
    static Constant Str(std::string s) { Constant c; c.type = Type::String; c.string = std::move(s); return c; }
    bool operator==(const Constant& o) const;
};

struct LocalVar {
    std::optional<std::string> name;
    int32_t startPc = 0;
    int32_t endPc = 0;
};

struct Function {
    std::optional<std::string> source; // nullopt: stored as NULL
    int32_t lineDefined = 0;
    int32_t lastLineDefined = 0; // Lua 5.1 only
    uint8_t upvalues = 0;
    uint8_t params = 0;
    uint8_t vararg = 0;
    uint8_t maxStackSize = 0;
    std::vector<int32_t> lineInfo;
    std::vector<LocalVar> locals;
    std::vector<std::optional<std::string>> upvalueNames;
    std::vector<Constant> constants;
    std::vector<Function> functions;
    std::vector<uint32_t> code;
};

struct Chunk {
    Version version = Version::Lua50;
    Function main;
};

// Reads either version, by the header. Throws FormatError on a malformed
// chunk or one in another layout (stock luac's double numbers, big-endian, ...).
Chunk ReadChunk(ByteSpan data);
// Writes the layout of chunk.version.
Bytes WriteChunk(const Chunk& chunk);

} // namespace attrib::lua
