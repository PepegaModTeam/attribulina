// Lists the gameplay collections whose parent chain contains a template.
// Usage: children <repo> <template> [...]
#include "core/Database.h"
#include "core/Hash.h"
#include <cstdio>
using namespace attrib;
int main(int argc, char** argv) {
    std::string root = argv[1];
    Database db;
    db.names().loadFile(root + "/data/attrib_keys.txt");
    const std::string dir = root + "/attrib/databases/pc/prostreet-vanilla/";
    db.loadPack(dir + "ATTRIBUTES.BIN");
    db.loadPack(dir + "gameplay.lzc");
    const ClassDef* gameplay = db.findClass(StringHash32("gameplay"));
    for (int i = 2; i < argc; ++i) {
        uint32_t t = StringHash32(argv[i]);
        int n = 0;
        for (const Collection* c : gameplay->collections) {
            for (const Collection* p = c->parent; p; p = p->parent)
                if (p->key == t) {
                    std::string nm = db.names().format(c->key);
                    if (nm.find("stategraph") == std::string::npos && ++n <= 6) std::printf("  %s <- %s\n", argv[i], nm.c_str());
                    break;
                }
        }
        std::printf("%s: %d elements (not counting handlers)\n", argv[i], n);
    }
}
