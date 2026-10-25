// Writer tests: unmodified packs must come back byte for byte, and edits must
// survive a write and re-read without disturbing anything else.

#include "Test.h"

#include "core/Compression.h"
#include "core/Edit.h"
#include "core/Folder.h"
#include "core/Hash.h"
#include "core/Pack.h"
#include "core/Schema.h"
#include "core/Writer.h"

#include <filesystem>
#include <algorithm>
#include <functional>
#include <map>

using namespace attrib;

namespace {
std::string PS(const char* file) { return test::DataPath(std::string("attrib/databases/pc/prostreet-vanilla/") + file); }

size_t FirstDiff(const Bytes& a, const Bytes& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return i;
    return n;
}

void CheckIdentical(const Bytes& written, const Bytes& original, const char* what) {
    if (written != original) {
        std::printf("        %s: sizes %zu vs %zu, first difference at 0x%zX\n", what, written.size(), original.size(),
                    FirstDiff(written, original));
    }
    CHECK(written == original);
}

// Deep, comparable rendering of a collection's values.
std::string Snapshot(const Collection& c) {
    std::string out;
    std::function<void(const DataBlock&)> block = [&](const DataBlock& b) {
        out.append(reinterpret_cast<const char*>(b.bytes.data()), b.bytes.size());
        std::vector<const PtrSlot*> ps;
        for (const auto& p : b.ptrs) ps.push_back(&p);
        std::sort(ps.begin(), ps.end(), [](auto* x, auto* y) { return x->offset < y->offset; });
        for (const PtrSlot* p : ps) {
            out += "@" + std::to_string(p->offset);
            if (p->isString) out += "s:" + p->text;
            else if (p->data) { out += "{"; block(*p->data); out += "}"; }
        }
    };
    out += std::to_string(c.parentKey) + "|";
    for (const Attribute& a : c.attributes) {
        out += "#" + std::to_string(a.key) + "/" + std::to_string(a.count) + (a.inLayout ? "L" : "E") + ":";
        block(a.data);
    }
    return out;
}
} // namespace

TEST("writer: ATTRIBUTES.BIN round trips byte for byte") {
    Database db;
    LoadedPack& p = db.loadPack(PS("ATTRIBUTES.BIN"));
    for (const auto& v : p.vaults) CHECK(v->exactlyWritable);
    CheckIdentical(WritePack(db, p), ReadFile(PS("ATTRIBUTES.BIN")), "ATTRIBUTES.BIN");
}

TEST("writer: cars_vault.BIN and gameplay round trip byte for byte") {
    Database db;
    db.loadPack(PS("ATTRIBUTES.BIN"));
    LoadedPack& cars = db.loadPack(PS("cars_vault.BIN"));
    CheckIdentical(WritePack(db, cars), ReadFile(PS("cars_vault.BIN")), "cars_vault.BIN");
    LoadedPack& gp = db.loadPack(PS("gameplay.lzc"));
    for (const auto& v : gp.vaults) CHECK(v->exactlyWritable);
    CheckIdentical(WritePack(db, gp), ReadFile(PS("gameplay.bin")), "gameplay (uncompressed)");
    // The compressed file decodes to the same pack.
    CHECK(Decompress(WritePackFile(db, gp)) == ReadFile(PS("gameplay.bin")));
}

TEST("writer: FE_ATTRIB.BIN round trips byte for byte") {
    Database db;
    db.loadPack(PS("ATTRIBUTES.BIN"));
    LoadedPack& fe = db.loadPack(PS("FE_ATTRIB.BIN"));
    for (const auto& v : fe.vaults) {
        for (const auto& w : v->warnings) std::printf("        warning: %s\n", w.c_str());
        CHECK(v->exactlyWritable);
    }
    CheckIdentical(WritePack(db, fe), ReadFile(PS("FE_ATTRIB.BIN")), "FE_ATTRIB.BIN");
}

TEST("writer: Pro Street 360 packs round trip byte for byte") {
    auto x = [](const char* f) { return test::DataPath(std::string("attrib/databases/360/prostreet-vanilla/") + f); };
    // Without the type schema (pointers from the hand table) and with it.
    for (bool schema : {false, true}) {
        Database db;
        if (schema) db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
        for (const char* f : {"ATTRIBUTES.BIN", "CARS_VAULT.BIN", "FE_ATTRIB.BIN", "GAMEPLAY.BIN"}) {
            LoadedPack& p = db.loadPack(x(f));
            for (const auto& v : p.vaults) {
                for (const auto& w : v->warnings) std::printf("        %s: %s\n", v->name.c_str(), w.c_str());
                CHECK(v->exactlyWritable);
                CHECK(v->raw.endian == Endian::Big);
            }
            CheckIdentical(WritePack(db, p), ReadFile(x(f)), f);
        }
    }
}

TEST("writer: Pro Street Wii packs round trip byte for byte") {
    auto x = [](const char* f) { return test::DataPath(std::string("attrib/databases/wii/prostreet-vanilla/") + f); };
    // The PS2's Vers value, the PC's layouts, big-endian.
    Database db;
    db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
    db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_ps2.txt")));
    for (const char* f : {"ATTRIBUTES.BIN", "CARS_VAULT.BIN", "FE_ATTRIB.BIN", "GAMEPLAY.LZC"}) {
        LoadedPack& p = db.loadPack(x(f));
        for (const auto& v : p.vaults) {
            for (const auto& w : v->warnings) std::printf("        %s: %s\n", v->name.c_str(), w.c_str());
            CHECK(v->warnings.empty());
            CHECK(v->exactlyWritable);
            CHECK(v->raw.endian == Endian::Big);
        }
        CheckIdentical(WritePack(db, p), std::string(f) == "GAMEPLAY.LZC" ? ReadFile(x("GAMEPLAY.BIN")) : ReadFile(x(f)), f);
    }
    CHECK(db.target() == (Target{Game::ProStreet, Platform::Wii}));
    CHECK(db.typeSchema() && db.typeSchema()->source.find("prostreet_pc") != std::string::npos);
}

TEST("writer: Pro Street PS2 packs round trip byte for byte") {
    auto x = [](const char* f) { return test::DataPath(std::string("attrib/databases/ps2/prostreet-vanilla/") + f); };
    // Both schemas, as the editor loads them: the PS2's FEPartData (4-byte
    // bools) only comes out right with its own.
    Database db;
    db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
    db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_ps2.txt")));
    for (const char* f : {"ATTRIBUTES.BIN", "cars_vault.BIN", "FE_ATTRIB.BIN", "gameplay.bin"}) {
        LoadedPack& p = db.loadPack(x(f));
        for (const auto& v : p.vaults) {
            for (const auto& w : v->warnings) std::printf("        %s: %s\n", v->name.c_str(), w.c_str());
            CHECK(v->warnings.empty());
            CHECK(v->exactlyWritable);
        }
        CheckIdentical(WritePack(db, p), ReadFile(x(f)), f);
    }
    CHECK(db.target() == (Target{Game::ProStreet, Platform::PS2}));
    CHECK(db.typeSchema() && db.typeSchema()->platforms == std::vector<Platform>{Platform::PS2});
}

TEST("writer: Carbon's and the Wii's Undercover packs round trip byte for byte") {
    // Every pack of each folder, with all the schemas, as the editor opens it.
    // Carbon's PC layouts are the PS3's and the Wii's; its PS2's (4-byte
    // bools) are the GameCube's. The Wii's Undercover is Carbon's engine. The
    // two demos use Most Wanted's vault layout and older types, each with
    // its own schema.
    struct Folder {
        std::string path, target, schema;
    };
    const std::vector<Folder> folders = {
        {"pc/carbon-vanilla", "carbon_pc", "carbon_pc.txt"},
        {"ps3/carbon-vanilla", "carbon_ps3", "carbon_pc.txt"},
        {"ps3/carbon-latebeta", "carbon_ps3", "carbon_pc.txt"},
        {"wii/carbon-vanilla", "carbon_wii", "carbon_pc.txt"},
        {"ps2/carbon-vanilla", "carbon_ps2", "carbon_ps2.txt"},
        {"gamecube/carbon-vanilla", "carbon_gamecube", "carbon_ps2.txt"},
        {"360/carbon-latedemo", "carbon_x360", "carbon_x360.txt"},
        {"ps2/carbon-demo", "carbon_ps2", "carbon_ps2_demo.txt"},
        {"wii/undercover-vanilla", "undercover_wii", "undercover_wii.txt"},
        // VLTEd's re-save: every pointer explained, but it lays the packs out its own way.
        {"pc/carbon-vlted", "carbon_pc", "carbon_pc.txt"}};
    for (const auto& [folder, target, schema] : folders) {
        Database db;
        for (const char* s :
             {"prostreet_pc", "prostreet_ps2", "carbon_pc", "carbon_ps2", "carbon_x360", "carbon_ps2_demo", "undercover_wii", "undercover_ps2"})
            db.addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
        for (const FolderFile& f : FolderPacks(test::DataPath("attrib/databases/" + folder))) {
            if (!f.skipped.empty()) continue;
            LoadedPack& p = db.loadPack(f.path);
            for (const auto& v : p.vaults) {
                for (const auto& w : v->warnings) std::printf("        %s %s: %s\n", folder.c_str(), v->name.c_str(), w.c_str());
                CHECK(v->warnings.empty());
                CHECK(v->exactlyWritable);
            }
            if (folder.find("vlted") != std::string::npos) continue;
            const Bytes file = ReadFile(f.path);
            const Bytes original = ParseLzHeader(file) ? Decompress(file) : file;
            if (folder == "ps2/carbon-demo" && f.path.find("gameplay") != std::string::npos) {
                // Two of its vaults keep strings nothing points at, which the
                // model doesn't hold; every other vault is byte for byte.
                const PackFile written = ReadPack(WritePack(db, p)), read = ReadPack(original);
                CHECK_EQ(written.vaults.size(), read.vaults.size());
                size_t same = 0;
                for (size_t i = 0; i < std::min(written.vaults.size(), read.vaults.size()); ++i)
                    same += written.vaults[i].bin == read.vaults[i].bin && written.vaults[i].vlt == read.vaults[i].vlt;
                CHECK_EQ(same, read.vaults.size() - 2);
                continue;
            }
            CheckIdentical(WritePack(db, p), original, (folder + " " + f.path).c_str());
        }
        CHECK_EQ(db.target().id(), target);
        CHECK(db.typeSchema() && db.typeSchema()->source.find(schema) != std::string::npos);
    }
}

TEST("writer: current-gen Undercover: the 360's packs round trip byte for byte; the PC's (NFS-VltEd's re-save) load clean") {
    // Pro Street's layout with StrN before DepN, and four static index tables
    // whose type no type table lists. Both platforms use undercover_pc.txt.
    for (const char* folder : {"360/undercover-vanilla", "pc/undercover-vanilla"}) {
        const std::string dir = test::DataPath(std::string("attrib/databases/") + folder);
        if (!std::filesystem::exists(dir)) continue;
        Database db;
        for (const char* s : {"prostreet_pc", "carbon_pc", "undercover_wii", "undercover_ps2", "undercover_pc"})
            db.addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
        const bool vlted = std::string(folder).rfind("pc/", 0) == 0;
        for (const FolderFile& f : FolderPacks(dir)) {
            if (!f.skipped.empty()) continue;
            LoadedPack& p = db.loadPack(f.path);
            for (const auto& v : p.vaults) {
                for (const auto& w : v->warnings) std::printf("        %s %s: %s\n", folder, v->name.c_str(), w.c_str());
                CHECK(v->warnings.empty());
                CHECK(v->exactlyWritable);
                CHECK(v->raw.strNFirst);
            }
            if (vlted) continue;
            const Bytes file = ReadFile(f.path);
            CheckIdentical(WritePack(db, p), ParseLzHeader(file) ? Decompress(file) : file, (std::string(folder) + " " + f.path).c_str());
        }
        CHECK_EQ(db.target().id(), std::string(vlted ? "undercover_pc" : "undercover_x360"));
        CHECK(db.typeSchema() && db.typeSchema()->source.find("undercover_pc.txt") != std::string::npos);
    }
}

TEST("writer: World's final packs and its betas' round trip byte for byte, their event vaults (gc.vaults) too") {
    // Undercover's chunk order with EndC chunks (none in the 2009 beta); the
    // event vaults are files of their own (Pack.h) with a Sign chunk and a
    // VaultData export. One schema lays out all three builds.
    for (const char* folder : {"pc/world-final", "pc/world-2010-beta", "pc/world-2009-beta"}) {
        const std::string dir = test::DataPath(std::string("attrib/databases/") + folder);
        if (!std::filesystem::exists(dir)) continue;
        const bool b2009 = std::string(folder) == "pc/world-2009-beta";
        Database db;
        for (const char* s : {"carbon_pc", "undercover_pc", "world_pc"})
            db.addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
        size_t events = 0;
        for (const FolderFile& f : FolderPacks(dir)) {
            // The 2009 beta's lithium.bin is its gameplay Lua, not a pack.
            if (!f.skipped.empty()) {
                CHECK(b2009 && f.path.find("lithium.bin") != std::string::npos);
                continue;
            }
            LoadedPack& p = db.loadPack(f.path);
            for (const auto& v : p.vaults) {
                // The final build's coop_pursuit_1 event sets four fields its
                // gameplay class no longer has.
                const size_t expected = v->name == "coop_pursuit_1" && std::string(folder) == "pc/world-final" ? 4 : 0;
                if (v->warnings.size() != expected)
                    for (const auto& w : v->warnings) std::printf("        %s %s: %s\n", folder, v->name.c_str(), w.c_str());
                CHECK_EQ(v->warnings.size(), expected);
                CHECK(v->exactlyWritable);
                CHECK(v->raw.strNFirst && v->raw.endC != b2009);
                if (p.format == PackFormat::VaultFile) {
                    CHECK_EQ(v->raw.sign.size(), size_t(8 + 128 * v->raw.dependencies.size()));
                    CHECK_EQ(v->dataExports.size(), size_t(1));
                    CHECK(!v->exportOrder.empty() && v->exportOrder.back().kind == ExportRef::Kind::Data);
                }
            }
            events += p.format == PackFormat::VaultFile;
            CheckIdentical(WritePack(db, p), ReadFile(f.path), f.path.c_str());
        }
        CHECK(events > 60);
        CHECK_EQ(db.target().id(), std::string("world_pc"));
        CHECK(db.typeSchema() && db.typeSchema()->source.find("world_pc.txt") != std::string::npos);
    }
}

TEST("writer: an edited World event vault keeps its header and VaultData, drops its stale Sign; a new collection goes before VaultData") {
    const std::string dir = test::DataPath("attrib/databases/pc/world-final");
    const std::string event = dir + "/gc.vaults/1_2_1_sprint.bin";
    if (!std::filesystem::exists(event)) return;
    auto load = [&](Database& db) { db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/world_pc.txt"))); db.loadPack(dir + "/attributes.bin"); };
    Database db;
    load(db);
    LoadedPack& pack = db.loadPack(event);
    Vault& v = *pack.vaults.front();
    CHECK(!v.collections.empty());
    if (v.collections.empty()) return;
    Collection& first = *v.collections.front();
    CollectionEditor ed(db);
    Collection& added = ed.create(v, *first.cls, "attribulina_event_child", &first);
    CHECK(v.exportOrder.back().kind == ExportRef::Kind::Data);
    const Bytes written = WritePack(db, pack);
    const std::optional<VaultFileHeader> h = ParseVaultFileHeader(written);
    CHECK(h && h->name == "1_2_1_sprint" && h->binOffset == 0x40 && h->fileSize == written.size());
    CHECK(h && h->vltOffset % 0x40 == 0 && written.size() % 0x40 == 0);

    Database again;
    load(again);
    LoadedPack& p2 = again.loadPack(event, written);
    CHECK(p2.format == PackFormat::VaultFile);
    const Vault& v2 = *p2.vaults.front();
    // The signature no longer signs it; one written unchanged keeps it (the round trips).
    CHECK(!v.raw.sign.empty() && v2.raw.sign.empty());
    CHECK(std::none_of(v2.raw.vltChunks.begin(), v2.raw.vltChunks.end(), [](const RawChunk& k) { return k.id == FourCC("Sign"); }));
    CHECK(v2.raw.endC);
    CHECK(v2.dataExports.size() == 1 && v2.dataExports[0].bytes == v.dataExports[0].bytes && v2.dataExports[0].id == v.dataExports[0].id);
    CHECK(v2.exportOrder.back().kind == ExportRef::Kind::Data);
    const Collection* back = again.findCollection(added.classKey, added.key);
    CHECK(back && back->parent && back->parent->key == first.key && back->vault == &v2);
}

TEST("writer: current-gen Undercover's static index tables follow the collections of their class") {
    const std::string dir = test::DataPath("attrib/databases/360/undercover-vanilla");
    if (!std::filesystem::exists(dir)) return;
    Database db;
    db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/undercover_pc.txt")));
    db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    std::string attributes;
    for (const FolderFile& f : FolderPacks(dir))
        if (f.skipped.empty()) {
            db.loadPack(f.path);
            if (f.path.find("ATTRIBUTES") != std::string::npos) attributes = f.path;
        }
    LoadedPack* pack = nullptr;
    for (const auto& p : db.packs())
        if (p->path == attributes) pack = p.get();
    CHECK(pack);
    if (!pack) return;
    const Bytes original = WritePack(db, *pack);
    // The table as a map, read the way the reader attached it.
    auto table = [&](const char* cls) {
        ClassDef* k = db.findClass(StringHash32(cls));
        const FieldDef* f = k->field(StringHash32("IndexTable_CollectionName"));
        const Endian e = k->vault->raw.endian;
        std::map<uint32_t, std::vector<uint32_t>> out;
        const DataBlock& b = k->staticData;
        const uint32_t n = load32(b.bytes.data() + f->offset, e);
        const DataBlock& keys = *b.ptrAt(f->offset + 4)->data;
        const DataBlock& sets = *b.ptrAt(f->offset + 8)->data;
        const DataBlock& vals = *b.ptrAt(f->offset + 12)->data;
        for (uint32_t i = 0; i < n; ++i) {
            auto& v = out[load32(keys.bytes.data() + i * 4, e)];
            const uint32_t at = load32(sets.bytes.data() + i * 8, e), c = load32(sets.bytes.data() + i * 8 + 4, e);
            for (uint32_t j = 0; j < c; ++j) v.push_back(load32(vals.bytes.data() + (at + j) * 4, e));
        }
        return out;
    };
    for (const char* cls : {"aud_moment", "speech", "traffic_engine", "traffic_horn"}) CHECK(!table(cls).empty());
    CollectionEditor ed(db);
    // aud_moment lists each parent's children: a new child is added, and
    // removing it again gives back the file's bytes.
    ClassDef* moment = db.findClass(StringHash32("aud_moment"));
    Collection* parent = nullptr;
    for (Collection* c : moment->collections)
        if (!c->children.empty()) parent = c;
    CHECK(parent);
    Vault& vault = *parent->vault;
    Collection& kid = ed.create(vault, *moment, "attribulina_moment", parent);
    auto t = table("aud_moment");
    CHECK(t[parent->key].back() == kid.key);
    Collection& grand = ed.create(vault, *moment, "attribulina_moment2", &kid);
    CHECK(table("aud_moment")[kid.key] == std::vector<uint32_t>{grand.key});
    CHECK(ed.remove(grand));
    CHECK(ed.remove(kid));
    CHECK(WritePack(db, *pack) == original);
    // speech lists each collection's parent: a rename and a move follow.
    ClassDef* speech = db.findClass(StringHash32("speech"));
    Collection* leaf = nullptr;
    for (Collection* c : speech->collections)
        if (c->parent && c->children.empty()) leaf = c;
    CHECK(leaf);
    const uint32_t oldKey = leaf->key;
    const std::string oldName = db.names().format(oldKey);
    ed.rename(*leaf, "attribulina_speech");
    auto s = table("speech");
    CHECK(!s.count(oldKey) && s[leaf->key] == std::vector<uint32_t>{leaf->parentKey});
    Collection* other = nullptr;
    for (Collection* c : speech->collections)
        if (c != leaf && c != leaf->parent && !c->parentKey && c->children.size() > 0) other = c;
    if (other) {
        Collection* was = leaf->parent;
        CHECK(ed.move(*leaf, other));
        CHECK(table("speech")[leaf->key] == std::vector<uint32_t>{other->key});
        CHECK(ed.move(*leaf, was));
    }
    if (db.names().find(oldKey)) {
        ed.rename(*leaf, oldName);
        CHECK(WritePack(db, *pack) == original);
    }
    // Written and read again, the tables load with every pointer explained.
    Collection& extra = ed.create(vault, *speech, "attribulina_speech2", leaf->parent);
    Database again;
    again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/undercover_pc.txt")));
    LoadedPack& re = again.loadPack(pack->path, WritePack(db, *pack));
    for (const auto& v : re.vaults) {
        for (const auto& w : v->warnings) std::printf("        %s: %s\n", v->name.c_str(), w.c_str());
        CHECK(v->warnings.empty());
    }
    CHECK(WritePack(again, re) == WritePack(db, *pack));
    (void)extra;
}

TEST("writer: Most Wanted's packs round trip byte for byte") {
    // Most Wanted's own vaults: no Vers chunk, a 0x1c ClassLoadData without
    // static data, a u32 type count in CollectionLoadData, and 32 for a
    // by-value entry. Retail PC, Xbox (gameplay.lzc) and GameCube, and the
    // 360's and the PS2's betas.
    struct Folder {
        std::string path, target, schema;
    };
    const std::vector<Folder> folders = {{"pc/mw-vanilla", "mw_pc", "mw_pc.txt"},
                                         {"xbox/mw-vanilla", "mw_xbox", "mw_pc.txt"},
                                         {"360/mw-beta", "mw_x360", "mw_pc.txt"},
                                         {"gamecube/mw-vanilla", "mw_gamecube", "mw_gamecube.txt"},
                                         {"ps2/mw-beta", "mw_ps2", "mw_gamecube.txt"}};
    for (const auto& [folder, target, schema] : folders) {
        Database db;
        for (const char* s : {"prostreet_pc", "carbon_pc", "carbon_x360", "mw_pc", "mw_gamecube"})
            db.addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
        size_t packs = 0;
        for (const FolderFile& f : FolderPacks(test::DataPath("attrib/databases/" + folder))) {
            if (!f.skipped.empty()) continue;
            LoadedPack& p = db.loadPack(f.path);
            ++packs;
            for (const auto& v : p.vaults) {
                for (const auto& w : v->warnings) std::printf("        %s %s: %s\n", folder.c_str(), v->name.c_str(), w.c_str());
                CHECK(v->warnings.empty());
                CHECK(v->exactlyWritable);
                CHECK(!v->raw.hasVers && v->raw.mostWantedOwn());
            }
            const Bytes file = ReadFile(f.path);
            const Bytes original = ParseLzHeader(file) ? Decompress(file) : file;
            CheckIdentical(WritePack(db, p), original, (folder + " " + f.path).c_str());
        }
        CHECK_EQ(packs, size_t(3));
        CHECK_EQ(db.target().id(), target);
        CHECK(db.typeSchema() && db.typeSchema()->source.find(schema) != std::string::npos);
    }
}

TEST("writer: the early Most Wanted demos' packs (64-bit keys) round trip byte for byte") {
    // The PS2 OPM and Xbox OXM beta demos: Most Wanted's own layout with
    // StringHash64 keys everywhere (DepN, ExpN, PtrN, load data, RefSpecs).
    struct Folder {
        std::string path, target, schema;
    };
    const std::vector<Folder> folders = {{"xbox/mw-early-oxm-betademo", "mw_xbox", "mw_xbox_demo.txt"},
                                         {"ps2/mw-early-opm-betademo", "mw_ps2", "mw_ps2_demo.txt"}};
    for (const auto& [folder, target, schema] : folders) {
        Database db;
        for (const char* s : {"mw_pc", "mw_gamecube", "mw_xbox_demo", "mw_ps2_demo"})
            db.addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
        db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
        size_t packs = 0;
        for (const FolderFile& f : FolderPacks(test::DataPath("attrib/databases/" + folder))) {
            if (!f.skipped.empty()) continue;
            LoadedPack& p = db.loadPack(f.path);
            ++packs;
            for (const auto& v : p.vaults) {
                for (const auto& w : v->warnings) std::printf("        %s %s: %s\n", folder.c_str(), v->name.c_str(), w.c_str());
                CHECK(v->warnings.empty());
                CHECK(v->exactlyWritable);
                CHECK(v->raw.keys64 && v->raw.mostWantedOwn());
            }
            CheckIdentical(WritePack(db, p), ReadFile(f.path), (folder + " " + f.path).c_str());
        }
        CHECK_EQ(packs, size_t(2));
        CHECK(db.keys64());
        CHECK_EQ(db.target().id(), target);
        CHECK(db.typeSchema() && db.typeSchema()->source.find(schema) != std::string::npos);
        // Keys read as the names they hash, and back.
        const ClassDef* car = db.findClass(StringHash32("pvehicle"));
        CHECK(car);
        if (car) CHECK_EQ(db.keyTo64(car->key), StringHash64("pvehicle"));
        // A new vault in the gameplay pack, with a new collection: StringHash64
        // dependency ids, export id and keys; no bin pointers, so no bin group in PtrN.
        LoadedPack& gp = *db.packs().back();
        ClassDef* gameplay = db.findClass(StringHash32("gameplay"));
        CHECK(gameplay && !gp.vaults.empty());
        if (!gameplay || gp.vaults.empty()) continue;
        Vault& nv = CreateVault(gp, "attribulina_vault", *gp.vaults.front());
        CollectionEditor(db).create(nv, *gameplay, "attribulina_element", nullptr);
        const Bytes written = WritePack(db, gp);
        Database again;
        again.setTarget(db.target());
        for (const char* s : {"mw_pc", "mw_gamecube", "mw_xbox_demo", "mw_ps2_demo"})
            again.addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
        again.names().loadFile(test::DataPath("data/attrib_keys.txt"));
        again.names().add("attribulina_element");
        again.loadPack(db.packs().front()->path);
        LoadedPack& back = again.loadPack(gp.path, written);
        const Vault* rv = nullptr;
        for (const auto& v : back.vaults)
            if (v->name == "attribulina_vault") rv = v.get();
        CHECK(rv && rv->raw.keys64 && rv->warnings.empty());
        if (!rv) continue;
        CHECK(rv->raw.dependencies.size() == 2 && rv->raw.dependencies[0].id == StringHash64("attribulina_vault.vlt"));
        const Collection* made = again.findCollection(StringHash32("gameplay"), StringHash32("attribulina_element"));
        CHECK(made && made->vault == rv && made->exportId == StringHash64("gameplay/attribulina_element"));
        CHECK(WritePack(again, back) == written);
    }
}

TEST("writer: an entry array grown past its limit and shrunk back writes as read; spare capacity of zeros reads exactly, of data warns") {
    Database db;
    LoadedPack& pack = db.loadPack(PS("ATTRIBUTES.BIN"));
    const Bytes original = ReadFile(PS("ATTRIBUTES.BIN"));
    auto gear = [](Database& d) { return d.findCollection(StringHash32("vehicle"), StringHash32("997gt3rs"))->find(StringHash32("GEAR_RATIO")); };
    Attribute* g = gear(db);
    CHECK(g && g->isArray && !g->inLayout && g->count == 8 && g->capacity == 8 && ElementLimit(*g) == 9);
    if (!g) return;
    // Past the definition's 9 and back: the capacity follows the count both ways.
    while (g->count < 12) InsertElement(*g, g->count);
    CHECK_EQ(uint32_t(g->capacity), 12u);
    while (g->count > 8) RemoveElement(*g, g->count - 1);
    CHECK_EQ(uint32_t(g->capacity), 8u);
    CheckIdentical(WritePack(db, pack), original, "grown and shrunk back");

    // As earlier builds left it: two spare elements past the count, zeroed.
    g->capacity = 10;
    const Bytes spare = WritePack(db, pack);
    CHECK(spare != original);
    {
        Database again;
        LoadedPack& p = again.loadPack("spare", spare);
        for (const auto& v : p.vaults) {
            for (const auto& w : v->warnings) std::printf("        warning: %s\n", w.c_str());
            CHECK(v->warnings.empty());
            CHECK(v->exactlyWritable);
        }
        Attribute* a = gear(again);
        CHECK(a && a->count == 8 && a->capacity == 10);
        CheckIdentical(WritePack(again, p), spare, "spare capacity, zeroed");
        // Resizing drops it, and the file is the retail one again.
        InsertElement(*a, a->count);
        RemoveElement(*a, a->count - 1);
        CHECK_EQ(uint32_t(a->capacity), 8u);
        CheckIdentical(WritePack(again, p), original, "spare capacity dropped");
    }

    // Spare elements holding data aren't kept: the vault says so.
    Bytes header(8);
    store16(header.data(), 10, Endian::Little);
    store16(header.data() + 2, 8, Endian::Little);
    store16(header.data() + 4, uint16_t(g->elemSize), Endian::Little);
    store16(header.data() + 6, g->encodedTypePad, Endian::Little);
    header.insert(header.end(), g->data.bytes.begin(), g->data.bytes.end());
    Bytes poked = spare;
    const auto at = std::search(poked.begin(), poked.end(), header.begin(), header.end());
    CHECK(at != poked.end());
    if (at == poked.end()) return;
    CHECK(std::search(at + 1, poked.end(), header.begin(), header.end()) == poked.end());
    *(at + header.size()) = 1;
    Database again;
    LoadedPack& p = again.loadPack("poked", poked);
    size_t warned = 0;
    for (const auto& v : p.vaults) {
        for (const auto& w : v->warnings) warned += w.find("997gt3rs") != std::string::npos && w.find("data past its count") != std::string::npos;
        if (!v->warnings.empty()) CHECK(!v->exactlyWritable);
    }
    CHECK_EQ(warned, size_t(1));
}

TEST("writer: edits survive a write and re-read, nothing else changes") {
    Database db;
    LoadedPack& pack = db.loadPack(PS("ATTRIBUTES.BIN"));
    std::map<std::pair<uint32_t, uint32_t>, std::string> before;
    for (const auto& cls : db.classes())
        for (const Collection* c : cls->collections) before[{c->classKey, c->key}] = Snapshot(*c);

    // 1) Grow a StringKey array and give the new element a brand-new string.
    ClassDef* pv = db.findClass(StringHash32("pvehicle"));
    Collection* def = db.findCollection(pv->key, StringHash32("default"));
    Attribute* order = def->find(StringHash32("BEHAVIOR_ORDER"));
    InsertElement(*order, order->count);
    const uint32_t last = order->count - 1;
    store32(order->elem(last), StringHash32("attribulina_behavior"), Endian::Little);
    PtrSlot s;
    s.offset = last * order->elemSize + 4;
    s.isString = true;
    s.text = "attribulina_behavior";
    order->data.ptrs.push_back(s);
    // 2) A new child collection with an overridden float.
    CollectionEditor ed(db);
    Collection& child = ed.create(*def->vault, *pv, "attribulina_child", def);
    const FieldDef* hr = pv->field(StringHash32("HandlingRating"));
    Attribute& own = OverrideAttribute(db, child, *hr);
    InsertElement(own, own.count);
    storef32(own.elem(own.count - 1), 4.25f, Endian::Little);
    std::map<std::pair<uint32_t, uint32_t>, std::string> expected = before;
    expected[{def->classKey, def->key}] = Snapshot(*def);
    expected[{child.classKey, child.key}] = Snapshot(child);

    const Bytes written = WritePack(db, pack);
    Database again;
    again.loadPack("edited", written);
    size_t compared = 0;
    for (const auto& cls : again.classes())
        for (const Collection* c : cls->collections) {
            auto it = expected.find({c->classKey, c->key});
            CHECK(it != expected.end());
            if (it->second != Snapshot(*c))
                std::printf("        changed: %s/%s\n", again.names().format(c->classKey).c_str(), again.names().format(c->key).c_str());
            CHECK(it->second == Snapshot(*c));
            ++compared;
        }
    CHECK_EQ(compared, expected.size());
    Collection* rchild = again.findCollection(pv->key, StringHash32("attribulina_child"));
    CHECK(rchild && rchild->parent && rchild->parent->key == StringHash32("default"));
    for (const auto& v : again.packs()[0]->vaults) {
        for (const auto& w : v->warnings) std::printf("        warning: %s\n", w.c_str());
        CHECK(v->warnings.empty());
    }
}
