#include "Lua.h"

#include "Chunk.h"
#include "Lua50.h"
#include "Lua51.h"
#include "Opcodes50.h"
#include "Opcodes51.h"

#include "core/Hash.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <set>

namespace attrib::lua {

namespace {

std::string Hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

// A constant as it would be written in source, shortened for listings.
std::string ConstantText(const Constant& k) {
    switch (k.type) {
        case Constant::Type::Nil: return "nil";
        case Constant::Type::Boolean: return k.boolean ? "true" : "false";
        case Constant::Type::Number: {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%.9g", double(k.number));
            return buf;
        }
        case Constant::Type::String: {
            std::string s = "\"";
            for (unsigned char c : k.string) {
                if (c == '"' || c == '\\') { s += '\\'; s += char(c); }
                else if (c == '\n') s += "\\n";
                else if (c < 32 || c >= 127) { char b[8]; std::snprintf(b, sizeof b, "\\%03d", c); s += b; }
                else s += char(c);
            }
            return s + "\"";
        }
    }
    return "?";
}

void InspectFunction(const Function& f, bool main, ChunkInfo& info, std::set<std::string>& reads,
                     std::set<std::string>& methods) {
    using namespace op50;
    ++info.functions;
    info.instructions += uint32_t(f.code.size());
    auto kstr = [&](int idx) -> const std::string* {
        if (idx < 0 || size_t(idx) >= f.constants.size() || f.constants[idx].type != Constant::Type::String)
            return nullptr;
        return &f.constants[idx].string;
    };
    for (uint32_t i : f.code) {
        const Op op = GetOp(i);
        if (op == GETGLOBAL) {
            if (auto* s = kstr(GetBx(i))) reads.insert(*s);
        } else if (op == SETGLOBAL && main) {
            if (auto* s = kstr(GetBx(i))) info.globalsSet.push_back(*s);
        } else if (op == SELF && IsK(GetC(i))) {
            if (auto* s = kstr(KIndex(GetC(i)))) methods.insert(*s);
        }
    }
    for (const auto& p : f.functions) InspectFunction(p, false, info, reads, methods);
}

void InspectFunction51(const Function& f, bool main, ChunkInfo& info, std::set<std::string>& reads,
                       std::set<std::string>& methods) {
    using namespace op51;
    ++info.functions;
    info.instructions += uint32_t(f.code.size());
    auto kstr = [&](int idx) -> const std::string* {
        if (idx < 0 || size_t(idx) >= f.constants.size() || f.constants[idx].type != Constant::Type::String)
            return nullptr;
        return &f.constants[idx].string;
    };
    for (size_t pc = 0; pc < f.code.size(); ++pc) {
        const uint32_t i = f.code[pc];
        const Op op = GetOp(i);
        if (op == GETGLOBAL) {
            if (auto* s = kstr(GetBx(i))) reads.insert(*s);
        } else if (op == SETGLOBAL && main) {
            if (auto* s = kstr(GetBx(i))) info.globalsSet.push_back(*s);
        } else if (op == SELF && IsK(GetC(i))) {
            if (auto* s = kstr(KIndex(GetC(i)))) methods.insert(*s);
        } else if (op == SETLIST && GetC(i) == 0) {
            ++pc; // the next word is the block number, not an instruction
        }
    }
    for (const auto& p : f.functions) InspectFunction51(p, false, info, reads, methods);
}

} // namespace

void ListFunction51(const Function& f, const std::string& name, std::string& out) {
    using namespace op51;
    char buf[256];
    std::string vararg;
    if (f.vararg & VARARG_ISVARARG) vararg = (f.vararg & VARARG_NEEDSARG) ? " + ... (arg)" : " + ...";
    std::snprintf(buf, sizeof buf,
                  "function %s (lines %d-%d): %zu instructions, %d params%s, %d registers, %d upvalues, "
                  "%zu constants, %zu functions\n",
                  name.c_str(), int(f.lineDefined), int(f.lastLineDefined), f.code.size(), int(f.params),
                  vararg.c_str(), int(f.maxStackSize), int(f.upvalues), f.constants.size(), f.functions.size());
    out += buf;
    auto K = [&](int idx) -> std::string {
        if (idx < 0 || size_t(idx) >= f.constants.size()) return "<bad constant " + std::to_string(idx) + ">";
        return ConstantText(f.constants[idx]);
    };
    auto RK = [&](int rk) -> std::string { return IsK(rk) ? K(KIndex(rk)) : "R" + std::to_string(rk); };
    for (size_t pc = 0; pc < f.code.size(); ++pc) {
        const uint32_t i = f.code[pc];
        const Op op = GetOp(i);
        const int a = GetA(i), b = GetB(i), c = GetC(i);
        std::string args, note;
        switch (GetMode(op)) {
            case Mode::ABC: std::snprintf(buf, sizeof buf, "%d %d %d", a, b, c); break;
            case Mode::ABx: std::snprintf(buf, sizeof buf, "%d %d", a, GetBx(i)); break;
            case Mode::AsBx: std::snprintf(buf, sizeof buf, "%d %d", a, GetSBx(i)); break;
        }
        args = buf;
        switch (op) {
            case LOADK: case GETGLOBAL: case SETGLOBAL: note = K(GetBx(i)); break;
            case GETTABLE: case SELF: note = RK(c); break;
            case SETTABLE: note = RK(b) + " = " + RK(c); break;
            case ADD: case SUB: case MUL: case DIV: case MOD: case POW: case EQ: case LT: case LE:
                note = RK(b) + " " + RK(c);
                break;
            case JMP: case FORLOOP: case FORPREP: note = "to " + std::to_string(int(pc) + 2 + GetSBx(i)); break;
            case CLOSURE: note = "function " + name + "." + std::to_string(GetBx(i)); break;
            default: break;
        }
        if (op >= NUM_OPCODES) note = "invalid opcode " + std::to_string(int(op));
        std::snprintf(buf, sizeof buf, "  %4zu  %-9s %-12s", pc + 1, Name(op), args.c_str());
        std::string line = buf;
        if (!note.empty()) line += " ; " + note;
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line + "\n";
        if (op == SETLIST && c == 0 && pc + 1 < f.code.size()) {
            ++pc;
            std::snprintf(buf, sizeof buf, "  %4zu  %-9s %u", pc + 1, "(block)", unsigned(f.code[pc]));
            out += std::string(buf) + "\n";
        }
    }
    if (!f.constants.empty()) {
        out += "  constants:\n";
        for (size_t k = 0; k < f.constants.size(); ++k) {
            std::snprintf(buf, sizeof buf, "  %4zu  ", k);
            out += buf + ConstantText(f.constants[k]) + "\n";
        }
    }
    for (size_t p = 0; p < f.functions.size(); ++p) {
        out += "\n";
        ListFunction51(f.functions[p], name + "." + std::to_string(p), out);
    }
}

void ListFunction(const Function& f, const std::string& name, std::string& out) {
    using namespace op50;
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "function %s (line %d): %zu instructions, %d params%s, %d registers, %d upvalues, "
                  "%zu constants, %zu functions\n",
                  name.c_str(), int(f.lineDefined), f.code.size(), int(f.params), f.vararg ? " + ..." : "",
                  int(f.maxStackSize), int(f.upvalues), f.constants.size(), f.functions.size());
    out += buf;
    auto K = [&](int idx) -> std::string {
        if (idx < 0 || size_t(idx) >= f.constants.size()) return "<bad constant " + std::to_string(idx) + ">";
        return ConstantText(f.constants[idx]);
    };
    auto RK = [&](int rk) -> std::string { return IsK(rk) ? K(KIndex(rk)) : "R" + std::to_string(rk); };
    for (size_t pc = 0; pc < f.code.size(); ++pc) {
        const uint32_t i = f.code[pc];
        const Op op = GetOp(i);
        const int a = GetA(i), b = GetB(i), c = GetC(i);
        std::string args, note;
        switch (GetMode(op)) {
            case Mode::ABC: std::snprintf(buf, sizeof buf, "%d %d %d", a, b, c); break;
            case Mode::ABx: std::snprintf(buf, sizeof buf, "%d %d", a, GetBx(i)); break;
            case Mode::AsBx: std::snprintf(buf, sizeof buf, "%d %d", a, GetSBx(i)); break;
        }
        args = buf;
        switch (op) {
            case LOADK: case GETGLOBAL: case SETGLOBAL: note = K(GetBx(i)); break;
            case GETTABLE: case SELF: note = RK(c); break;
            case SETTABLE: note = RK(b) + " = " + RK(c); break;
            case ADD: case SUB: case MUL: case DIV: case POW: case EQ: case LT: case LE:
                note = RK(b) + " " + RK(c);
                break;
            case JMP: case FORLOOP: case TFORPREP: note = "to " + std::to_string(int(pc) + 2 + GetSBx(i)); break;
            case CLOSURE: note = "function " + name + "." + std::to_string(GetBx(i)); break;
            default: break;
        }
        if (op >= NUM_OPCODES) note = "invalid opcode " + std::to_string(int(op));
        std::snprintf(buf, sizeof buf, "  %4zu  %-9s %-12s", pc + 1, Name(op), args.c_str());
        std::string line = buf;
        if (!note.empty()) line += " ; " + note;
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line + "\n";
    }
    if (!f.constants.empty()) {
        out += "  constants:\n";
        for (size_t k = 0; k < f.constants.size(); ++k) {
            std::snprintf(buf, sizeof buf, "  %4zu  ", k);
            out += buf + ConstantText(f.constants[k]) + "\n";
        }
    }
    for (size_t p = 0; p < f.functions.size(); ++p) {
        out += "\n";
        ListFunction(f.functions[p], name + "." + std::to_string(p), out);
    }
}

std::string HandlerFunctionName(std::string_view handlerCollectionName, bool named) {
    if (named) return std::string(handlerCollectionName);
    return "h_" + Hex8(StringHash32(handlerCollectionName));
}

ChunkInfo Inspect(ByteSpan chunk) {
    const Chunk c = ReadChunk(chunk);
    ChunkInfo info;
    info.version = c.version;
    std::set<std::string> reads, methods;
    if (c.version == Version::Lua51) InspectFunction51(c.main, true, info, reads, methods);
    else InspectFunction(c.main, true, info, reads, methods);
    info.globalsRead.assign(reads.begin(), reads.end());
    info.methodsCalled.assign(methods.begin(), methods.end());
    return info;
}

std::string Disassemble(ByteSpan chunk) {
    const Chunk c = ReadChunk(chunk);
    if (c.version == Version::Lua51) {
        std::string out = "-- Lua 5.1 chunk\n";
        ListFunction51(c.main, "main", out);
        return out;
    }
    std::string out = "-- Lua 5.0 chunk\n";
    ListFunction(c.main, "main", out);
    return out;
}

DecompileResult Decompile(ByteSpan chunk) {
    const Chunk c = ReadChunk(chunk);
    if (c.version == Version::Lua51) return Decompile51(c);
    return Decompile50(c);
}

CompileResult Compile(std::string_view source, Version version) {
    if (version == Version::Lua51) return Compile51(source);
    return Compile50(source);
}

} // namespace attrib::lua
