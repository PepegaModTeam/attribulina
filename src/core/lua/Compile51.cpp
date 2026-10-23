// Lua 5.1 compiling through the embedded Lua 5.1.3 parser (packages/lua51),
// written out in the game format and stripped like the retail chunks.
#include "Chunk.h"
#include "Lua51.h"

#include "lua51_compile.h"

#include <cstdlib>

namespace attrib::lua {

namespace {

Function Convert(const lua51::CompiledFunction& in) {
    Function f;
    // Stripped (luac -s): every source is NULL, and there is no line info,
    // local or upvalue names.
    f.lineDefined = in.lineDefined;
    f.lastLineDefined = in.lastLineDefined;
    f.upvalues = uint8_t(in.upvalues);
    f.params = uint8_t(in.params);
    f.vararg = uint8_t(in.vararg);
    f.maxStackSize = uint8_t(in.maxStackSize);
    f.code = in.code;
    f.constants.reserve(in.constants.size());
    for (const auto& k : in.constants) {
        switch (k.type) {
            case lua51::CompiledConstant::Boolean: {
                Constant c;
                c.type = Constant::Type::Boolean;
                c.boolean = k.boolean;
                f.constants.push_back(c);
                break;
            }
            case lua51::CompiledConstant::Number: f.constants.push_back(Constant::Num(k.number)); break;
            case lua51::CompiledConstant::String: f.constants.push_back(Constant::Str(k.string)); break;
            default: f.constants.push_back(Constant::Nil()); break;
        }
    }
    f.functions.reserve(in.functions.size());
    for (const auto& p : in.functions) f.functions.push_back(Convert(p));
    return f;
}

} // namespace

CompileResult Compile51(std::string_view source) {
    CompileResult r;
    lua51::CompiledFunction out;
    std::string error;
    // "=lua" makes Lua's messages start with "lua:<line>:".
    if (!lua51::CompileSource(source.data(), source.size(), "=lua", out, error)) {
        std::string msg = error;
        int line = 0;
        if (msg.rfind("lua:", 0) == 0) {
            char* end = nullptr;
            line = int(std::strtol(msg.c_str() + 4, &end, 10));
            if (end && *end == ':') {
                msg = end + 1;
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
            }
        }
        r.line = line;
        r.error = "line " + std::to_string(line) + ": " + msg;
        return r;
    }
    Chunk c;
    c.version = Version::Lua51;
    c.main = Convert(out);
    r.chunk = WriteChunk(c);
    return r;
}

} // namespace attrib::lua
