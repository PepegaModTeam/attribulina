// Lists every field of the gameplay class: name, AttribSys type, array flag.
// Usage: gameplay_fields <repo>   (tab separated: name, type, array 0/1)
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
    for (const FieldDef& f : gameplay->fields)
        std::printf("%s\t%s\t%d\n", db.names().format(f.key).c_str(), f.type ? f.type->name.c_str() : db.binNames().format(f.typeHash).c_str(), f.isArray() ? 1 : 0);
}
