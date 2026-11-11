// Lua tests: the compiled handlers in the gameplay databases must read and
// write back byte for byte, and decompiling then compiling each one must give
// back the identical chunk. Lua 5.0: Pro Street (and the Wii's Undercover);
// Lua 5.1: Undercover on PC and 360, and World's lithium.bin, whose source
// lithium.lua is also the compiler's oracle.

#include "Test.h"

#include "core/Compression.h"
#include "core/Database.h"
#include "core/Folder.h"
#include "core/Hash.h"
#include "core/lua/Chunk.h"
#include "core/lua/Lua.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace attrib;

namespace {

struct Handler {
    uint32_t collectionKey;
    Bytes chunk; // unpacked from the bytecode blob's LZHeader block
};

// Every `bytecode` blob of a folder's gameplay class.
std::vector<Handler> LoadHandlers(const std::string& folder) {
    std::vector<Handler> out;
    const std::string dir = test::DataPath("attrib/databases/" + folder);
    const auto attributes = FindFileNoCase(dir, "attributes.bin");
    auto gameplay = FindFileNoCase(dir, "gameplay.lzc");
    if (!gameplay) gameplay = FindFileNoCase(dir, "gameplay.bin");
    if (!attributes || !gameplay) return out;
    Database db;
    db.loadPack(*attributes);
    db.loadPack(*gameplay);
    const ClassDef* cls = db.findClass(StringHash32("gameplay"));
    if (!cls) return out;
    for (const Collection* c : cls->collections) {
        const Attribute* a = c->find(StringHash32("bytecode"));
        if (!a) continue;
        for (uint32_t i = 0; i < a->count; ++i) {
            const PtrSlot* p = a->data.ptrAt(i * a->elemSize + 4); // Attrib::Blob { u32 size; void* data }
            if (!p || !p->data) continue;
            out.push_back({c->key, Decompress(p->data->bytes)});
        }
    }
    return out;
}

// Pro Street PC's handlers (loaded once for all tests).
const std::vector<Handler>& Handlers() {
    static const std::vector<Handler> handlers = LoadHandlers("pc/prostreet-vanilla");
    return handlers;
}

// Undercover's on the 360 (retail, and the alpha's 207) and the PC, Lua 5.1.
const std::vector<Handler>& Undercover360() {
    static const std::vector<Handler> handlers = LoadHandlers("360/undercover-vanilla");
    return handlers;
}
const std::vector<Handler>& UndercoverPc() {
    static const std::vector<Handler> handlers = LoadHandlers("pc/undercover-vanilla");
    return handlers;
}
const std::vector<Handler>& UndercoverAlpha() {
    static const std::vector<Handler> handlers = LoadHandlers("360/undercover-alpha161");
    return handlers;
}

std::string ReadText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// World 2009 beta's lithium.bin: one chunk whose main function holds 268
// former handler chunks' main functions, each defining one handler.
const lua::Chunk& Lithium() {
    static const lua::Chunk chunk =
        lua::ReadChunk(ReadFile(test::DataPath("attrib/databases/pc/world-2009-beta/lithium.bin")));
    return chunk;
}

void Strip(lua::Function& f) {
    f.source.reset();
    f.lineInfo.clear();
    f.locals.clear();
    f.upvalueNames.clear();
    for (auto& p : f.functions) Strip(p);
}

// One of lithium.bin's functions as a chunk of its own, stripped like
// Undercover's (lithium.bin keeps sources, the handler hash, and line info).
Bytes LithiumPiece(const lua::Function& f) {
    lua::Chunk c;
    c.version = lua::Version::Lua51;
    c.main = f;
    Strip(c.main);
    return lua::WriteChunk(c);
}

// The global a former handler chunk defines.
std::string DefinedGlobal(const lua::Function& f) {
    for (const auto& k : f.constants)
        if (k.type == lua::Constant::Type::String && k.string.rfind("h_", 0) == 0) return k.string;
    return {};
}

std::string Hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

size_t FirstDiff(const Bytes& a, const Bytes& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return i;
    return n;
}

} // namespace

TEST("lua: the gameplay database holds 251 Lua 5.0 handlers") {
    const auto& hs = Handlers();
    CHECK_EQ(hs.size(), size_t(251));
    for (const Handler& h : hs) CHECK(lua::DetectVersion(h.chunk) == lua::Version::Lua50);
    // Not chunks: an LZHeader block, nothing, a stock luac 5.0 header (double numbers).
    CHECK(!lua::DetectVersion(Bytes{'H', 'U', 'F', 'F', 1, 0x10, 0, 0}));
    CHECK(!lua::DetectVersion(Bytes{}));
    const Bytes stock = {0x1B, 'L', 'u', 'a', 0x50, 0x01, 0x04, 0x04, 0x04, 0x06, 0x08, 0x09, 0x09, 0x08, 0, 0, 0, 0};
    CHECK(!lua::DetectVersion(stock));
    // Undercover/World's Lua 5.1 header, with nothing after it: not a chunk.
    const Bytes lua51 = {0x1B, 'L', 'u', 'a', 0x51, 0x00, 0x01, 0x04, 0x04, 0x04, 0x04, 0x00};
    CHECK(lua::DetectVersion(lua51) == lua::Version::Lua51);
    bool threw = false;
    try {
        lua::ReadChunk(lua51);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
    // Stock luac 5.1 (double numbers).
    const Bytes stock51 = {0x1B, 'L', 'u', 'a', 0x51, 0x00, 0x01, 0x04, 0x04, 0x04, 0x08, 0x00};
    CHECK(!lua::DetectVersion(stock51));
}

TEST("lua: every handler chunk reads and writes back byte for byte") {
    int same = 0;
    for (const Handler& h : Handlers()) {
        const Bytes written = lua::WriteChunk(lua::ReadChunk(h.chunk));
        if (written == h.chunk) ++same;
        else std::printf("        h_%s: first difference at 0x%zX\n", Hex8(h.collectionKey).c_str(), FirstDiff(written, h.chunk));
    }
    CHECK_EQ(same, int(Handlers().size()));
}

TEST("lua: every handler decompiles to source that compiles back byte for byte") {
    int same = 0;
    for (const Handler& h : Handlers()) {
        const lua::DecompileResult d = lua::Decompile(h.chunk);
        const lua::CompileResult c = lua::Compile(d.source);
        if (c.ok() && c.chunk == h.chunk && d.warnings.empty() && d.exact) {
            ++same;
            continue;
        }
        std::printf("        h_%s: %s\n", Hex8(h.collectionKey).c_str(),
                    !c.ok() ? c.error.c_str() : d.warnings.empty() ? "different bytecode" : d.warnings[0].c_str());
    }
    std::printf("        %d of %zu identical\n", same, Handlers().size());
    CHECK_EQ(same, int(Handlers().size()));
}

TEST("lua: each handler defines the global named after its collection") {
    // StringHash32 of the collection name is the collection's key.
    CHECK_EQ(lua::HandlerFunctionName("basicrace/stategraph_basicrace_racing_handler_notifyescaped"),
             std::string("h_8C272D2D"));
    CHECK_EQ(lua::HandlerFunctionName("basicrace/stategraph_basicrace_racing_handler_triggerenter"),
             std::string("h_2B6A5CD7"));
    for (const Handler& h : Handlers()) {
        const lua::ChunkInfo info = lua::Inspect(h.chunk);
        CHECK(info.version == lua::Version::Lua50);
        CHECK_EQ(info.functions >= 2u, true);
        CHECK_EQ(info.globalsSet.size(), size_t(1));
        CHECK_EQ(info.globalsSet[0], "h_" + Hex8(h.collectionKey));
    }
}

TEST("lua: decompiled handlers read like the authored scripts") {
    const uint32_t key = StringHash32("basicrace/stategraph_basicrace_racing_handler_notifyescaped");
    const Handler* found = nullptr;
    for (const Handler& h : Handlers())
        if (h.collectionKey == key) found = &h;
    CHECK(found != nullptr);
    const lua::DecompileResult d = lua::Decompile(found->chunk);
    CHECK(d.warnings.empty());
    // The handler starts on line 1, like the tool that wrote them.
    CHECK(d.source.rfind("function h_8C272D2D(self, message, vars)\n", 0) == 0);
    const std::string listing = lua::Disassemble(found->chunk);
    CHECK(listing.find("SETGLOBAL") != std::string::npos);
    CHECK(listing.find("\"h_8C272D2D\"") != std::string::npos);
}

TEST("lua: compiling reports errors with their line") {
    const lua::CompileResult bad = lua::Compile("function h_1(self, message, vars)\n"
                                                "\tlocal x = 1\n"
                                                "\tif x then\n"
                                                "\t\tx = = 2\n"
                                                "\tend\n"
                                                "end\n");
    CHECK(!bad.ok());
    CHECK_EQ(bad.line, 4);
    CHECK(bad.error.rfind("line 4: ", 0) == 0);
    const lua::CompileResult unclosed = lua::Compile("function h_1(self, message, vars)\n\tif x then\nend\n");
    CHECK(!unclosed.ok());
    CHECK_EQ(unclosed.line, 4);
    const lua::CompileResult good = lua::Compile("function h_1(self, message, vars) end\n");
    CHECK(good.ok());
    CHECK(lua::DetectVersion(good.chunk) == lua::Version::Lua50);
    // Lua 5.1 reports errors the same way.
    const lua::CompileResult bad51 = lua::Compile("function h_1(self, message, vars)\n\tx = = 2\nend\n", lua::Version::Lua51);
    CHECK(!bad51.ok());
    CHECK_EQ(bad51.line, 2);
    CHECK(bad51.error.rfind("line 2: ", 0) == 0);
    // `...` is 5.1 syntax; 5.0 only has `arg`.
    CHECK(!lua::Compile("function f(...) return ... end\n", lua::Version::Lua50).ok());
    CHECK(lua::Compile("function f(...) return ... end\n", lua::Version::Lua51).ok());
    CHECK(!lua::Compile("\x1bLuaQ", lua::Version::Lua51).ok());
}

TEST("lua: compiled source decompiles back to the same bytecode") {
    // Constructs the game's handlers don't use, compiled with the embedded
    // compiler and taken through the decompiler.
    static const char* const kSources[] = {
        "function h(self, message, vars)\n"
        "\tlocal i = 0\n"
        "\twhile i < 10 do\n"
        "\t\ti = i + 1\n"
        "\t\tif i == 5 then break end\n"
        "\tend\n"
        "\trepeat\n"
        "\t\tlocal y = g()\n"
        "\t\th(y)\n"
        "\tuntil i <= 0 or vars.done\n"
        "\tfor j = 10, 1, -1 do print(j) end\n"
        "\tfor k, v in pairs(vars) do print(k, v) end\n"
        "end\n",

        "function h(self, message, vars)\n"
        "\tif a then f() elseif b and c then g() else k() end\n"
        "\tif not (a and b) or c then f() end\n"
        "\tif x.y >= y.z and 5 < x then f() end\n"
        "\tlocal t = a == b\n"
        "\tlocal u = a and b or c\n"
        "\tvars.q = vars.q or {}\n"
        "\tprint(a and b, c or d, not e)\n"
        "\treturn a or b\n"
        "end\n",

        "function h(self, message, vars)\n"
        "\tlocal t = {1, 2, x = 3, [vars.k] = 4, {5}, f()}\n"
        "\tlocal a, b = f()\n"
        "\ta, b = b, a\n"
        "\tvars.a, vars.b = vars.b, vars.a\n"
        "\tlocal s = \"tab\\t \\\"quote\\\" \\200\" .. a .. (b .. 1.5)\n"
        "\tlocal n = -2.5 * (a + 1e10) ^ 2\n"
        "\treturn t, s, n, vars:get():set(-1)\n"
        "end\n",

        "function h(self, message, vars)\n"
        "\tlocal count = 0\n"
        "\tlocal function bump(n)\n"
        "\t\tcount = count + n\n"
        "\t\treturn count\n"
        "\tend\n"
        "\tlocal function fact(n)\n"
        "\t\tif n <= 1 then return 1 end\n"
        "\t\treturn n * fact(n - 1)\n"
        "\tend\n"
        "\tfor i = 1, 3 do\n"
        "\t\tvars[i] = function() return i + count end\n"
        "\tend\n"
        "\tdo\n"
        "\t\tlocal z = 5\n"
        "\t\tvars.z = function() z = z + 1 return z end\n"
        "\tend\n"
        "\tvars.va = function(...) return arg.n, unpack(arg) end\n"
        "\treturn bump(1), fact(5)\n"
        "end\n",
    };
    for (const char* src : kSources) {
        const lua::CompileResult first = lua::Compile(src);
        CHECK(first.ok());
        const lua::DecompileResult d = lua::Decompile(first.chunk);
        const lua::CompileResult again = lua::Compile(d.source);
        if (!again.ok() || again.chunk != first.chunk || !d.warnings.empty())
            std::printf("        decompiled:\n%s\n", d.source.c_str());
        CHECK(again.ok());
        CHECK(d.warnings.empty());
        CHECK(d.exact);
        CHECK(again.chunk == first.chunk);
    }
}

TEST("lua: Most Wanted's, Carbon's and the Wii's Undercover handlers read, write and decompile exactly with Lua 5.0") {
    // The 5.1 work must leave the other games' 5.0 path as it was.
    for (const char* folder : {"pc/mw-vanilla", "pc/carbon-vanilla", "360/carbon-vanilla", "wii/undercover-vanilla"}) {
        if (!std::filesystem::exists(test::DataPath(std::string("attrib/databases/") + folder))) continue;
        const std::vector<Handler> hs = LoadHandlers(folder);
        CHECK(!hs.empty());
        int same = 0;
        for (const Handler& h : hs) {
            CHECK(lua::DetectVersion(h.chunk) == lua::Version::Lua50);
            CHECK(lua::WriteChunk(lua::ReadChunk(h.chunk)) == h.chunk);
            const lua::DecompileResult d = lua::Decompile(h.chunk);
            const lua::CompileResult c = lua::Compile(d.source, lua::Version::Lua50);
            if (c.ok() && c.chunk == h.chunk && d.exact) ++same;
        }
        std::printf("        %s: %d of %zu identical\n", folder, same, hs.size());
        CHECK_EQ(same, int(hs.size()));
    }
}

TEST("lua: Undercover's gameplay databases hold 214 Lua 5.1 handlers each, its 360 alpha's 207") {
    for (const auto* hs : {&Undercover360(), &UndercoverPc(), &UndercoverAlpha()}) {
        if (hs == &UndercoverAlpha() && !std::filesystem::exists(test::DataPath("attrib/databases/360/undercover-alpha161"))) continue;
        CHECK_EQ(hs->size(), size_t(hs == &UndercoverAlpha() ? 207 : 214));
        for (const Handler& h : *hs) {
            CHECK(lua::DetectVersion(h.chunk) == lua::Version::Lua51);
            // Named as Pro Street names them: "h_" + the collection's key.
            const lua::ChunkInfo info = lua::Inspect(h.chunk);
            CHECK(info.version == lua::Version::Lua51);
            CHECK_EQ(info.globalsSet.size(), size_t(1));
            CHECK_EQ(info.globalsSet[0], "h_" + Hex8(h.collectionKey));
        }
    }
    // The Wii's Undercover is the Carbon engine's game and stays on Lua 5.0.
    const std::vector<Handler> wii = LoadHandlers("wii/undercover-vanilla");
    CHECK(!wii.empty());
    for (const Handler& h : wii) CHECK(lua::DetectVersion(h.chunk) == lua::Version::Lua50);
}

TEST("lua: every Lua 5.1 chunk reads and writes back byte for byte") {
    int same = 0, total = 0;
    for (const auto* hs : {&Undercover360(), &UndercoverPc(), &UndercoverAlpha()})
        for (const Handler& h : *hs) {
            ++total;
            const Bytes written = lua::WriteChunk(lua::ReadChunk(h.chunk));
            if (written == h.chunk) ++same;
            else std::printf("        h_%s: first difference at 0x%zX\n", Hex8(h.collectionKey).c_str(), FirstDiff(written, h.chunk));
        }
    CHECK_EQ(same, total);
    const Bytes lithium = ReadFile(test::DataPath("attrib/databases/pc/world-2009-beta/lithium.bin"));
    CHECK(lua::WriteChunk(lua::ReadChunk(lithium)) == lithium);
    CHECK_EQ(Lithium().main.functions.size(), size_t(268));
}

namespace {

// Compares two functions' compiled form: code, constants, header and nested
// functions, not the source name or debug information. `ulp`: count number
// constants between 0 and 1 that are one unit in the last place above ours.
bool SameCode(const lua::Function& a, const lua::Function& b, int* ulp = nullptr) {
    if (a.params != b.params || a.vararg != b.vararg || a.upvalues != b.upvalues || a.maxStackSize != b.maxStackSize ||
        a.lineDefined != b.lineDefined || a.lastLineDefined != b.lastLineDefined || a.code != b.code ||
        a.constants.size() != b.constants.size() || a.functions.size() != b.functions.size())
        return false;
    for (size_t i = 0; i < a.constants.size(); ++i) {
        if (a.constants[i] == b.constants[i]) continue;
        const auto& x = a.constants[i];
        const auto& y = b.constants[i];
        uint32_t xb, yb;
        std::memcpy(&xb, &x.number, 4);
        std::memcpy(&yb, &y.number, 4);
        if (!ulp || x.type != lua::Constant::Type::Number || y.type != lua::Constant::Type::Number || xb != yb + 1 ||
            !(y.number > 0 && y.number < 1))
            return false;
        ++*ulp;
    }
    for (size_t i = 0; i < a.functions.size(); ++i)
        if (!SameCode(a.functions[i], b.functions[i], ulp)) return false;
    return true;
}

} // namespace

TEST("lua: compiling World's lithium.lua reproduces lithium.bin") {
    // lithium.lua is the source of lithium.bin. Each `function h_` line
    // starts a former handler chunk; compiled on its own, it must give the
    // matching function of lithium.bin.
    std::map<std::string, const lua::Function*> byName;
    for (const auto& f : Lithium().main.functions) byName[DefinedGlobal(f)] = &f;
    const std::string text = ReadText(test::DataPath("attrib/databases/pc/world-2009-beta/lithium.lua"));
    std::vector<std::pair<std::string, std::string>> pieces;
    for (size_t pos = 0; pos < text.size();) {
        const size_t nl = text.find('\n', pos);
        const size_t end = nl == std::string::npos ? text.size() : nl + 1;
        const std::string line = text.substr(pos, end - pos);
        if (line.rfind("function h_", 0) == 0) pieces.push_back({line.substr(9, 10), ""});
        if (!pieces.empty()) pieces.back().second += line;
        pos = end;
    }
    CHECK_EQ(pieces.size(), size_t(268));
    int exact = 0, nearly = 0, ulps = 0;
    for (const auto& [name, source] : pieces) {
        const auto it = byName.find(name);
        CHECK(it != byName.end());
        const lua::CompileResult r = lua::Compile(source, lua::Version::Lua51);
        CHECK(r.ok());
        const lua::Chunk ours = lua::ReadChunk(r.chunk);
        if (SameCode(*it->second, ours.main)) {
            ++exact;
        } else if (SameCode(*it->second, ours.main, &ulps)) {
            ++nearly;
        } else {
            std::printf("        %s differs\n", name.c_str());
        }
    }
    // The tool that built lithium.bin read numerals between 0 and 1 one unit
    // in the last place high (0.5 became 0.50000006); everything else is
    // Lua 5.1.3's code generator with float numbers.
    std::printf("        %d of 268 identical, %d more with %d such numerals\n", exact, nearly, ulps);
    CHECK_EQ(exact, 260);
    CHECK_EQ(nearly, 8);
}

TEST("lua: every Lua 5.1 handler decompiles to source that compiles back byte for byte") {
    auto roundTrip = [](const Bytes& chunk, const std::string& name) {
        const lua::DecompileResult d = lua::Decompile(chunk);
        const lua::CompileResult c = lua::Compile(d.source, lua::Version::Lua51);
        if (c.ok() && c.chunk == chunk && d.warnings.empty() && d.exact) return true;
        std::printf("        %s: %s\n", name.c_str(),
                    !c.ok() ? c.error.c_str() : d.warnings.empty() ? "different bytecode" : d.warnings[0].c_str());
        return false;
    };
    for (const auto* hs : {&Undercover360(), &UndercoverPc(), &UndercoverAlpha()}) {
        int same = 0;
        for (const Handler& h : *hs) same += roundTrip(h.chunk, "h_" + Hex8(h.collectionKey));
        std::printf("        %d of %zu identical\n", same, hs->size());
        CHECK_EQ(same, int(hs->size()));
    }
    int same = 0;
    for (const auto& f : Lithium().main.functions) same += roundTrip(LithiumPiece(f), DefinedGlobal(f));
    std::printf("        %d of 268 of lithium.bin's identical\n", same);
    CHECK_EQ(same, 268);
}

TEST("lua: decompiled Lua 5.1 handlers keep their lines and read like the authored scripts") {
    // h_89128CA5 opens with a closure on the handler's own first line; the
    // handler's `end` is on line 55, the closure's on 11.
    const Handler* found = nullptr;
    for (const Handler& h : Undercover360())
        if (h.collectionKey == 0x89128CA5) found = &h;
    CHECK(found != nullptr);
    const lua::DecompileResult d = lua::Decompile(found->chunk);
    CHECK(d.exact);
    CHECK(d.source.rfind("function h_89128CA5(self, message, vars) local function ", 0) == 0);
    std::vector<std::string> lines;
    std::istringstream in(d.source);
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    CHECK_EQ(lines.size(), size_t(55));
    CHECK_EQ(lines[54], std::string("end"));
    CHECK(lines[10].find("end") != std::string::npos);
    CHECK(d.source.find("RaceStatus:Cleanup()") != std::string::npos);
    const std::string listing = lua::Disassemble(found->chunk);
    CHECK(listing.rfind("-- Lua 5.1 chunk\n", 0) == 0);
    CHECK(listing.find("\"h_89128CA5\"") != std::string::npos);
    CHECK(listing.find("(lines 1-55)") != std::string::npos);
}

TEST("lua: compiled Lua 5.1 source decompiles back to the same bytecode") {
    // Constructs the game's handlers don't use, compiled with the embedded
    // 5.1 compiler and taken through the decompiler.
    static const char* const kSources[] = {
        "function h(self, message, vars)\n"
        "\tlocal i = 0\n"
        "\twhile i < 10 do\n"
        "\t\ti = i + 1\n"
        "\t\tif i == 5 then break end\n"
        "\tend\n"
        "\trepeat\n"
        "\t\tlocal y = g()\n"
        "\t\th(y)\n"
        "\tuntil y <= 0 or vars.done\n"
        "\tfor j = 10, 1, -1 do print(j) end\n"
        "\tfor k, v in pairs(vars) do print(k, v) end\n"
        "end\n",

        "function h(self, message, vars)\n"
        "\tif a then f() elseif b and c then g() else k() end\n"
        "\tif not (a and b) or c then f() end\n"
        "\tif x.y >= y.z and 5 < x then f() end\n"
        "\tlocal t = a == b\n"
        "\tlocal u = a and b or c\n"
        "\tvars.q = vars.q or {}\n"
        "\tprint(a and b, c or d, not e)\n"
        "\treturn a or b\n"
        "end\n",

        "function h(self, message, vars)\n"
        "\tlocal t = {1, 2, x = 3, [vars.k] = 4, {5}, f()}\n"
        "\tlocal a, b = f()\n"
        "\ta, b = b, a\n"
        "\tvars.a, vars.b = vars.b, vars.a\n"
        "\tlocal s = \"tab\\t \\\"quote\\\" \\200\" .. a .. (b .. 1.5)\n"
        "\tlocal n = -2.5 * (a + 1e10) ^ 2 % 7\n"
        "\treturn t, s, n, #t, 2 * a, vars:get():set(-1)\n"
        "end\n",

        "function h(self, message, vars)\n"
        "\tlocal count = 0\n"
        "\tlocal function bump(n)\n"
        "\t\tcount = count + n\n"
        "\t\treturn count\n"
        "\tend\n"
        "\tlocal function fact(n)\n"
        "\t\tif n <= 1 then return 1 end\n"
        "\t\treturn n * fact(n - 1)\n"
        "\tend\n"
        "\tfor i = 1, 3 do\n"
        "\t\tvars[i] = function() return i + count end\n"
        "\tend\n"
        "\tdo\n"
        "\t\tlocal z = 5\n"
        "\t\tvars.z = function() z = z + 1 return z end\n"
        "\tend\n"
        "\tvars.va = function(...) return select(\"#\", ...), ... end\n"
        "\tvars.old = function(a, ...) return arg.n, unpack(arg) end\n"
        "\treturn bump(1), fact(5)\n"
        "end\n",

        // A list long enough for two SETLISTs; loops that start together.
        "function h(self, message, vars)\n"
        "\tlocal x, y\n"
        "\tlocal list = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,\n"
        "\t\t26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50,\n"
        "\t\t51, 52, f()}\n"
        "\twhile true do\n"
        "\t\trepeat\n"
        "\t\t\tx = (x or 0) + 1\n"
        "\t\tuntil x > 3\n"
        "\t\tif y then break end\n"
        "\t\ty = not y\n"
        "\tend\n"
        "\trepeat\n"
        "\t\twhile x do x = x - 1 end\n"
        "\tuntil true == false\n"
        "\tfor k, v, w in next, list do y = k % 2 == 0 and v or w end\n"
        "\treturn x, y, list\n"
        "end\n",

        // The main chunk takes `...`; `local` before the first instruction
        // compiles to nothing.
        "local a, b, c = ...\n"
        "local d = select(2, ...)\n"
        "function h_1(self, message, vars)\n"
        "\tlocal n, m\n"
        "\tif #vars > 0 and vars[#vars] then\n"
        "\t\tvars[#vars] = n\n"
        "\telseif m == 0 then\n"
        "\t\tvars[1] = true\n"
        "\tend\n"
        "\treturn a, b, c, d\n"
        "end\n",
    };
    for (const char* src : kSources) {
        const lua::CompileResult first = lua::Compile(src, lua::Version::Lua51);
        CHECK(first.ok());
        const lua::DecompileResult d = lua::Decompile(first.chunk);
        const lua::CompileResult again = lua::Compile(d.source, lua::Version::Lua51);
        if (!again.ok() || again.chunk != first.chunk || !d.warnings.empty())
            std::printf("        decompiled:\n%s\n", d.source.c_str());
        CHECK(again.ok());
        CHECK(d.warnings.empty());
        CHECK(d.exact);
        CHECK(again.chunk == first.chunk);
    }
}

TEST("lua: Lua 5.0 and 5.1 compile and decompile side by side, each by its own chunks' header") {
    // The two embedded compilers live in one program (namespaces lua50 and
    // lua51); the version is chosen by the caller for compiling and by the
    // chunk's header for everything else.
    const char* src = "function h_2B6A5CD7(self, message, vars)\n"
                      "\tif message.Count > 2 then\n"
                      "\t\tvars.done = not vars.done\n"
                      "\tend\n"
                      "\tfor i = 1, 3 do Debug.Print(\"tick \" .. i) end\n"
                      "end\n";
    const lua::CompileResult c50 = lua::Compile(src, lua::Version::Lua50);
    const lua::CompileResult c51 = lua::Compile(src, lua::Version::Lua51);
    CHECK(c50.ok());
    CHECK(c51.ok());
    const Bytes h50 = {0x1B, 'L', 'u', 'a', 0x50, 0x01, 0x04, 0x04, 0x04, 0x06, 0x08, 0x09, 0x09, 0x04};
    const Bytes h51 = {0x1B, 'L', 'u', 'a', 0x51, 0x00, 0x01, 0x04, 0x04, 0x04, 0x04, 0x00};
    CHECK(Bytes(c50.chunk.begin(), c50.chunk.begin() + long(h50.size())) == h50);
    CHECK(Bytes(c51.chunk.begin(), c51.chunk.begin() + long(h51.size())) == h51);
    CHECK(lua::DetectVersion(c50.chunk) == lua::Version::Lua50);
    CHECK(lua::DetectVersion(c51.chunk) == lua::Version::Lua51);
    CHECK(lua::ReadChunk(c50.chunk).version == lua::Version::Lua50);
    CHECK(lua::ReadChunk(c51.chunk).version == lua::Version::Lua51);
    CHECK(lua::Inspect(c50.chunk).version == lua::Version::Lua50);
    CHECK(lua::Inspect(c51.chunk).version == lua::Version::Lua51);
    CHECK(lua::Inspect(c50.chunk).globalsSet == std::vector<std::string>{"h_2B6A5CD7"});
    CHECK(lua::Inspect(c51.chunk).globalsSet == std::vector<std::string>{"h_2B6A5CD7"});
    // 5.0 has no FORPREP; 5.1's numeric for starts with one.
    CHECK(lua::Disassemble(c50.chunk).find("FORPREP") == std::string::npos);
    CHECK(lua::Disassemble(c51.chunk).find("FORPREP") != std::string::npos);
    CHECK(lua::Disassemble(c50.chunk).rfind("-- Lua 5.0 chunk\n", 0) == 0);
    CHECK(lua::Disassemble(c51.chunk).rfind("-- Lua 5.1 chunk\n", 0) == 0);
    const lua::DecompileResult d50 = lua::Decompile(c50.chunk);
    const lua::DecompileResult d51 = lua::Decompile(c51.chunk);
    CHECK(d50.exact);
    CHECK(d51.exact);
    CHECK(lua::Compile(d50.source, lua::Version::Lua50).chunk == c50.chunk);
    CHECK(lua::Compile(d51.source, lua::Version::Lua51).chunk == c51.chunk);
    // A chunk of one version never matches the other's compile of the same text.
    CHECK(lua::Compile(d51.source, lua::Version::Lua50).chunk != c51.chunk);
    // And the games' own chunks, one of each, in the same process.
    CHECK(lua::Decompile(Handlers()[0].chunk).exact);
    CHECK(lua::Decompile(Undercover360()[0].chunk).exact);
}
