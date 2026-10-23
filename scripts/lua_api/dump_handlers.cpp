// Dumps every gameplay messagehandler of Pro Street PC: metadata, decompiled
// source and disassembly. Usage: dump_handlers <repo root> <out dir>
// Writes <out>/handlers/<n>.lua, <out>/handlers/<n>.dis and <out>/handlers.tsv.
#include "core/Compression.h"
#include "core/Database.h"
#include "core/Hash.h"
#include "core/lua/Lua.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <filesystem>

using namespace attrib;

static uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: dump_handlers <repo> <out>\n"); return 1; }
    std::string root = argv[1], out = argv[2];
    std::filesystem::create_directories(out + "/handlers");
    Database db;
    db.names().loadFile(root + "/data/attrib_keys.txt");
    // message names
    std::map<uint32_t, std::string> msgs;
    {
        std::ifstream f(root + "/data/messages/prostreet_pc.txt");
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::string name = line.substr(0, line.find(' '));
            msgs[StringHash32(name)] = line;
            db.names().add(name);
        }
    }
    const std::string dir = root + "/attrib/databases/pc/prostreet-vanilla/";
    db.loadPack(dir + "ATTRIBUTES.BIN");
    db.loadPack(dir + "gameplay.lzc");
    const ClassDef* gameplay = db.findClass(StringHash32("gameplay"));
    std::ofstream tsv(out + "/handlers.tsv");
    tsv << "func\tcollection\tparent\tvault\towner\tstate\tmessage\ttransitions\texact\n";
    auto keyOf = [&](const Collection* c, const char* field, int i = 0) -> std::string {
        const Attribute* a = c->resolve(StringHash32(field));
        if (!a || (uint32_t)i >= a->count || a->elemSize < 4) return "";
        uint32_t k = U32(a->elem(i));
        if (std::string(field) == "message_id") {
            auto it = msgs.find(k);
            if (it != msgs.end()) return it->first == k ? it->second.substr(0, it->second.find(' ')) : "";
        }
        return db.names().format(k);
    };
    int n = 0;
    for (const Collection* c : gameplay->collections) {
        const Attribute* a = c->find(StringHash32("bytecode"));
        if (!a) continue;
        for (uint32_t i = 0; i < a->count; ++i) {
            const PtrSlot* p = a->data.ptrAt(i * a->elemSize + 4);
            if (!p || !p->data) continue;
            Bytes chunk = Decompress(p->data->bytes);
            std::string fn = lua::HandlerFunctionName(db.names().format(c->key));
            char hx[16]; std::snprintf(hx, sizeof hx, "h_%08X", c->key);
            std::string cname = db.names().format(c->key);
            std::string trans;
            if (const Attribute* t = c->resolve(StringHash32("transitionlist")))
                for (uint32_t j = 0; j < t->count; ++j) { if (j) trans += ","; trans += db.names().format(U32(t->elem(j))); }
            lua::DecompileResult d = lua::Decompile(chunk);
            std::string state = keyOf(c, "stateref"), owner = keyOf(c, "handler_owner"), msg = keyOf(c, "message_id");
            std::string parent = c->parent ? db.names().format(c->parent->key) : "";
            std::ostringstream hdr;
            hdr << "-- collection: " << cname << "\n-- function: " << hx << "\n-- owner: " << owner << "\n-- state: " << state
                << "\n-- message: " << msg << "\n-- transitions: " << trans << "\n-- vault: " << (c->vault ? c->vault->name : "") << "\n";
            std::string base = out + "/handlers/" + hx;
            std::ofstream(base + ".lua") << hdr.str() << d.source;
            std::ofstream(base + ".dis") << lua::Disassemble(chunk);
            tsv << hx << '\t' << cname << '\t' << parent << '\t' << (c->vault ? c->vault->name : "") << '\t' << owner << '\t' << state
                << '\t' << msg << '\t' << trans << '\t' << (d.exact ? 1 : 0) << '\n';
            ++n;
        }
    }
    std::printf("%d handlers\n", n);
}
