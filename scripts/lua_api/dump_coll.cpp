// Prints the fields of the gameplay collections whose name matches a filter.
// Usage: dump_coll <repo> <substring>
#include "core/Database.h"
#include "core/Hash.h"
#include <cstdio>
#include <cstring>
using namespace attrib;
int main(int argc, char** argv) {
    std::string root = argv[1];
    Database db;
    db.names().loadFile(root + "/data/attrib_keys.txt");
    const std::string dir = root + "/attrib/databases/pc/prostreet-vanilla/";
    db.loadPack(dir + "ATTRIBUTES.BIN");
    db.loadPack(dir + "gameplay.lzc");
    const ClassDef* gameplay = db.findClass(StringHash32("gameplay"));
    int shown = 0;
    for (const Collection* c : gameplay->collections) {
        std::string n = db.names().format(c->key);
        if (n.find(argv[2]) == std::string::npos) continue;
        if (++shown > 6) break;
        std::printf("== %s (parent %s, vault %s)\n", n.c_str(), c->parent ? db.names().format(c->parent->key).c_str() : "-", c->vault ? c->vault->name.c_str() : "");
        for (const Attribute& a : c->attributes) {
            std::printf("  %-28s n=%u size=%u", db.names().format(a.key).c_str(), a.count, a.elemSize);
            for (uint32_t i = 0; i < a.count && i < 4; ++i) {
                std::printf(" [");
                for (uint32_t b = 0; b < a.elemSize && b < 16; ++b) std::printf("%02x", a.elem(i)[b]);
                for (const PtrSlot& p : a.data.ptrs)
                    if (p.isString && p.offset >= i * a.elemSize && p.offset < (i + 1) * a.elemSize) std::printf(" \"%s\"", p.text.c_str());
                std::printf("]");
            }
            std::printf("\n");
        }
    }
}
