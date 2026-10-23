// Counts gameplay collections that set the given fields themselves, and shows a few.
// Usage: count_fields <repo> <field> [...]
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
        uint32_t k = StringHash32(argv[i]);
        int n = 0;
        for (const Collection* c : gameplay->collections) {
            const Attribute* a = c->find(k);
            if (!a) continue;
            if (++n <= 4) {
                std::printf("  %s: %s n=%u [", argv[i], db.names().format(c->key).c_str(), a->count);
                for (uint32_t b = 0; b < a->count * a->elemSize && b < 12; ++b) std::printf("%02x", a->data.bytes[b]);
                std::printf("]\n");
            }
        }
        std::printf("%s: %d collections\n", argv[i], n);
    }
}
