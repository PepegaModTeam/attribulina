// Lua 5.0 compiling through the embedded Lua 5.0.1 parser (packages/lua50),
// written out in the game format and stripped like the retail chunks.
#include "Chunk.h"
#include "Lua50.h"

#include "lua50_compile.h"

#include <cstdlib>

namespace attrib::lua {

namespace {

Function Convert(const lua50::CompiledFunction& in, bool main) {
    Function f;
    // luac -s replaces every source with "=(none)"; ldump then writes NULL for
    // a nested function whose source equals its parent's.
    if (main) f.source = "=(none)";
    f.lineDefined = in.lineDefined;
    f.upvalues = uint8_t(in.upvalues);
    f.params = uint8_t(in.params);
    f.vararg = in.vararg ? 1 : 0;
    f.maxStackSize = uint8_t(in.maxStackSize);
    f.code = in.code;
    f.constants.reserve(in.constants.size());
    for (const auto& k : in.constants) {
        switch (k.type) {
            case lua50::CompiledConstant::Number: f.constants.push_back(Constant::Num(k.number)); break;
            case lua50::CompiledConstant::String: f.constants.push_back(Constant::Str(k.string)); break;
            default: f.constants.push_back(Constant::Nil()); break;
        }
    }
    f.functions.reserve(in.functions.size());
    for (const auto& p : in.functions) f.functions.push_back(Convert(p, false));
    return f;
}

} // namespace

CompileResult Compile50(std::string_view source) {
    CompileResult r;
    lua50::CompiledFunction out;
    std::string error;
    // "=lua" makes Lua's messages start with "lua:<line>:".
    if (!lua50::CompileSource(source.data(), source.size(), "=lua", out, error)) {
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
    c.version = Version::Lua50;
    c.main = Convert(out, true);
    r.chunk = WriteChunk(c);
    return r;
}

} // namespace attrib::lua
