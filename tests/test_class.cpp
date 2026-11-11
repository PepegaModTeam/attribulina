// Classes (core/ClassDesign.h, core/ClassList.h): retail classes laid out
// again from their designs, `class` statements, the retail class lists that
// restore a deleted class, new classes saved and loaded, redesigns carrying
// their values, and the game's classes staying as they are.

#include "Test.h"

#include "core/ClassDesign.h"
#include "core/ClassList.h"
#include "core/Edit.h"
#include "core/Folder.h"
#include "core/GenHeader.h"
#include "core/Hash.h"
#include "core/Script.h"
#include "core/TypeLayouts.h"
#include "core/Writer.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using namespace attrib;
namespace sc = attrib::script;
namespace fs = std::filesystem;

namespace {

// Every supported game folder, and the class list made from it (no title:
// another folder's list, whose classes are the same).
struct ClassFolder {
    const char* folder;
    const char* list;
    const char* title;
};
const ClassFolder kFolders[] = {
    {"pc/prostreet-vanilla", "prostreet_pc", "Pro Street, PC retail"},
    {"360/prostreet-vanilla", "prostreet_x360", "Pro Street, Xbox 360 retail"},
    {"ps2/prostreet-vanilla", "prostreet_ps2", "Pro Street, PS2 retail"},
    {"wii/prostreet-vanilla", "prostreet_wii", "Pro Street, Wii retail"},
    {"pc/carbon-vanilla", "carbon_pc", "Carbon, PC retail"},
    {"ps3/carbon-vanilla", "carbon_ps3", "Carbon, PS3 retail"},
    {"ps3/carbon-latebeta", "carbon_ps3", nullptr},
    {"wii/carbon-vanilla", "carbon_wii", "Carbon, Wii retail"},
    {"ps2/carbon-vanilla", "carbon_ps2", "Carbon, PS2 retail"},
    {"gamecube/carbon-vanilla", "carbon_gamecube", "Carbon, GameCube retail"},
    {"360/carbon-vanilla", "carbon_x360", "Carbon, Xbox 360 retail"},
    {"360/carbon-latedemo", "carbon_x360_late_demo", "Carbon, Xbox 360 late demo"},
    {"ps2/carbon-demo", "carbon_ps2_demo", "Carbon, PS2 demo"},
    {"pc/mw-vanilla", "mw_pc", "Most Wanted, PC retail"},
    {"xbox/mw-vanilla", "mw_xbox", "Most Wanted, Xbox retail"},
    {"gamecube/mw-vanilla", "mw_gamecube", "Most Wanted, GameCube retail"},
    {"360/mw-beta", "mw_x360", "Most Wanted, Xbox 360 beta"},
    {"ps2/mw-beta", "mw_ps2", "Most Wanted, PS2 beta"},
    {"ps2/mw-early-opm-betademo", "mw_ps2_early_demo", "Most Wanted, PS2 OPM beta demo"},
    {"xbox/mw-early-oxm-betademo", "mw_xbox_early_demo", "Most Wanted, Xbox OXM beta demo"},
    {"360/undercover-vanilla", "undercover_x360", "Undercover, Xbox 360 retail"},
    {"360/undercover-alpha161", "undercover_x360_alpha", "Undercover, Xbox 360 alpha (build 161)"},
    {"pc/undercover-vanilla", "undercover_pc", "Undercover, PC retail"},
    {"wii/undercover-vanilla", "undercover_wii", "Undercover, Wii retail"},
    {"pc/world-final", "world_pc", "World, PC final build"},
    {"pc/world-2010-beta", "world_pc_2010_beta", "World, PC 2010 beta"},
    {"pc/world-2009-beta", "world_pc_2009_beta", "World, PC 2009 beta"},
    // World's builds between, by the folders' build numbers: the last of each Vers
    // value (the others have the same classes with other collection reserves).
    {"pc/world-various/GLOBAL 77", "world_pc_77", "World, PC build 77"},
    {"pc/world-various/GLOBAL 190", "world_pc_190", "World, PC build 190"},
    {"pc/world-various/GLOBAL 270", "world_pc_270", "World, PC build 270"},
    {"pc/world-various/GLOBAL 396", "world_pc_396", "World, PC build 396"},
    {"pc/world-various/GLOBAL 593", "world_pc_593", "World, PC build 593"},
    {"pc/world-various/GLOBAL 704", "world_pc_704", "World, PC build 704"},
    {"pc/world-various/GLOBAL 749", "world_pc_749", "World, PC build 749"},
    {"pc/world-various/GLOBAL 846", "world_pc_846", "World, PC build 846"},
    {"pc/world-various/GLOBAL 922", "world_pc_922", "World, PC build 922"},
    {"pc/world-various/GLOBAL 1169", "world_pc_1169", "World, PC build 1169"},
    {"pc/world-various/GLOBAL 1251", "world_pc_1251", "World, PC build 1251"},
    {"pc/world-various/GLOBAL 1334", "world_pc_1334", "World, PC build 1334"},
    {"pc/world-various/GLOBAL 1410-1412", "world_pc_1410_1412", "World, PC builds 1410 to 1412"},
};

std::vector<std::string> SortedTxt(const std::string& dir) {
    std::vector<std::string> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".txt") v.push_back(e.path().string());
    std::sort(v.begin(), v.end());
    return v;
}

const std::vector<std::shared_ptr<const TypeSchema>>& Schemas() {
    static std::vector<std::shared_ptr<const TypeSchema>> all;
    if (all.empty())
        for (const std::string& f : SortedTxt(test::DataPath("data/schema"))) all.push_back(TypeSchema::Load(f));
    return all;
}

const std::vector<std::shared_ptr<const ClassList>>& Lists() {
    static std::vector<std::shared_ptr<const ClassList>> all;
    if (all.empty())
        for (const std::string& f : SortedTxt(test::DataPath("data/classes"))) all.push_back(ClassList::Load(f));
    return all;
}

std::string FolderPath(const char* folder) { return test::DataPath(std::string("attrib/databases/") + folder); }

// A folder's attributes.bin (every class is defined there), read as the app reads it.
std::unique_ptr<Database> LoadAttributes(const char* folder, bool lists = true) {
    auto db = std::make_unique<Database>();
    for (const auto& s : Schemas()) db->addTypeSchema(s);
    if (lists)
        for (const auto& l : Lists()) db->addClassList(l);
    db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    const std::string dir = FolderPath(folder);
    db->setTarget(DetectTarget(dir).target);
    const std::optional<std::string> f = FindFileNoCase(dir, "attributes.bin");
    if (!f) throw std::runtime_error(std::string("no attributes.bin in ") + folder);
    db->loadPack(*f);
    return db;
}

bool Available(const char* folder) { return fs::exists(FolderPath(folder)); }

bool SameDefs(const ClassDef& a, const ClassDef& b) {
    if (a.fields.size() != b.fields.size()) return false;
    for (size_t i = 0; i < a.fields.size(); ++i) {
        const FieldDef& x = a.fields[i];
        const FieldDef& y = b.fields[i];
        if (x.key != y.key || x.typeHash != y.typeHash || x.offset != y.offset || x.size != y.size || x.maxCount != y.maxCount ||
            x.flags != y.flags || x.alignment != y.alignment)
            return false;
    }
    return a.key == b.key && a.layoutSize == b.layoutSize && a.layoutCount == b.layoutCount && a.layoutKeyShift == b.layoutKeyShift &&
           a.staticSize == b.staticSize && a.collectionReserve == b.collectionReserve;
}

// Static values alike, nested blocks by content (index tables follow the collections: left out).
bool SameStatics(const ClassDef& a, const ClassDef& b) {
    Bytes x = a.staticData.bytes, y = b.staticData.bytes;
    x.resize(a.staticSize, 0);
    y.resize(b.staticSize, 0);
    std::vector<std::pair<uint32_t, uint32_t>> skip;
    for (const FieldDef& f : a.fields)
        if (f.isStatic() && f.type && f.type->name == kIndexTableType) skip.emplace_back(f.offset, f.offset + f.size);
    auto skipped = [&](uint32_t off) {
        for (auto& [lo, hi] : skip)
            if (off >= lo && off < hi) return true;
        return false;
    };
    for (uint32_t i = 0; i < x.size() && i < y.size(); ++i)
        if (!skipped(i) && x[i] != y[i]) return false;
    auto ptrs = [&](const DataBlock& d) {
        std::vector<std::string> v;
        for (const PtrSlot& p : d.ptrs)
            if (!skipped(p.offset))
                v.push_back(std::to_string(p.offset) + (p.isString ? ":" + p.text : p.data ? ":" + std::string(p.data->bytes.begin(), p.data->bytes.end()) : ""));
        std::sort(v.begin(), v.end());
        return v;
    };
    return x.size() == y.size() && ptrs(a.staticData) == ptrs(b.staticData);
}

sc::ApplyReport Run(Database& db, const std::string& text) {
    sc::ApplySettings st;
    st.fragment = true;
    return sc::Apply(db, sc::Parse(text, "test.amod"), st);
}

void CheckOk(const sc::ApplyReport& r) {
    for (const auto& m : r.messages)
        if (m.level == sc::Message::Level::Error) std::printf("        %d:%d %s\n", m.pos.line, m.pos.col, m.text.c_str());
    CHECK(r.ok);
}

std::unique_ptr<Database> Reload(const Database& db, const LoadedPack& pack) {
    auto out = std::make_unique<Database>();
    for (const auto& s : Schemas()) out->addTypeSchema(s);
    for (const auto& l : Lists()) out->addClassList(l);
    out->setTarget(db.target());
    out->names() = db.names();
    LoadedPack& p = out->loadPack(pack.path, WritePack(db, pack));
    for (const auto& v : p.vaults) {
        for (const auto& w : v->warnings) std::printf("        %s: %s\n", v->name.c_str(), w.c_str());
        CHECK(v->warnings.empty());
    }
    return out;
}

std::string Exported(const Database& db, uint32_t classKey) {
    std::vector<const Collection*> cols;
    if (const ClassDef* c = db.findClass(classKey))
        for (const Collection* x : c->collections) cols.push_back(x);
    return sc::Print(sc::NewScript(db, sc::Export(db, cols)));
}

} // namespace

TEST("classes: write the retail class lists (ATTRIB_WRITE_CLASS_LISTS=1)") {
    if (!std::getenv("ATTRIB_WRITE_CLASS_LISTS")) return;
    fs::create_directories(test::DataPath("data/classes"));
    for (const ClassFolder& f : kFolders) {
        if (!Available(f.folder) || !f.title) continue;
        const auto db = LoadAttributes(f.folder, false);
        std::ofstream out(test::DataPath(std::string("data/classes/") + f.list + ".txt"), std::ios::binary);
        out << WriteClassList(*db, f.title);
        std::printf("        %s: %zu classes\n", f.list, db->classes().size());
    }
}

TEST("classes: every retail class lays out from its design as stored, in every supported folder") {
    size_t classes = 0, fields = 0, offsets = 0, aligns = 0, flags = 0, counts = 0, sizes = 0, classValues = 0;
    std::set<std::string> misaligned;
    for (const ClassFolder& f : kFolders) {
        if (!Available(f.folder)) continue;
        const auto db = LoadAttributes(f.folder, false);
        for (const auto& cls : db->classes()) {
            const ClassDesign d = DesignOf(*db, *cls);
            ClassDef laid;
            laid.collections = cls->collections;
            LayOutClass(*db, d, laid);
            if (!SameDefs(laid, *cls)) std::printf("        %s: %s differs\n", f.folder, db->names().format(cls->key).c_str());
            CHECK(SameDefs(laid, *cls));
            // Alignments come from a type alone too (a database whose classes were all deleted).
            for (const FieldDef& fd : cls->fields)
                if (fd.type && NaturalAlignment(*fd.type) != fd.alignment)
                    misaligned.insert(std::string(f.folder) + ": " + fd.type->name + " is " + std::to_string(1u << fd.alignment) + "-aligned, by itself " +
                                      std::to_string(1u << NaturalAlignment(*fd.type)));
            ++classes;
            classValues += d.layoutSize.has_value() + d.layoutCount.has_value() + d.layoutKeyShift.has_value() + d.staticSize.has_value();
            for (const FieldDesign& fd : d.fields) {
                ++fields;
                offsets += fd.offset.has_value();
                aligns += fd.alignment.has_value();
                flags += fd.flags.has_value();
                counts += fd.maxCount.has_value();
                sizes += fd.size.has_value();
            }
        }
    }
    // Alignments and flags go by the types alone (a restore doesn't depend on the other classes).
    CHECK_EQ(aligns, size_t(0));
    CHECK_EQ(flags, size_t(0));
    std::printf("        %zu classes, %zu fields; stored values the rules don't give: %zu offsets, %zu alignments, %zu flags, %zu counts, "
                "%zu sizes, %zu class values\n",
                classes, fields, offsets, aligns, flags, counts, sizes, classValues);
    for (const std::string& m : misaligned) std::printf("        %s\n", m.c_str());
    CHECK(misaligned.empty());
}

TEST("classes: class statements print, parse and lay out as the class") {
    for (const ClassFolder& f : kFolders) {
        if (!Available(f.folder)) continue;
        const auto db = LoadAttributes(f.folder, false);
        sc::Script s = sc::NewScript(*db);
        for (const auto& cls : db->classes()) s.statements.push_back(sc::ClassStatement(*db, *cls));
        const std::string text = sc::Print(s);
        const sc::Script back = sc::Parse(text, f.list);
        CHECK_EQ(sc::Print(back), text);
        size_t i = 0;
        for (const sc::Statement& st : back.statements) {
            if (st.kind != sc::Statement::Kind::Class) continue;
            const ClassDef& cls = *db->classes()[i++];
            CHECK_EQ(ClassStatementKey(st), cls.key);
            ClassDef laid;
            LayOutClass(*db, sc::DesignFromStatement(*db, st), laid);
            CHECK(SameDefs(laid, cls));
        }
        CHECK_EQ(i, db->classes().size());
    }
}

TEST("classes: every retail class, deleted, comes back from its list as it was") {
    for (const ClassFolder& f : kFolders) {
        if (!Available(f.folder)) continue;
        const auto original = LoadAttributes(f.folder);
        const auto db = LoadAttributes(f.folder);
        const ClassList* list = RetailClassesFor(*db);
        CHECK(list != nullptr);
        if (list->source != std::string(f.list) + ".txt") std::printf("        %s: %s fits better\n", f.folder, list->source.c_str());
        CHECK(list->source == std::string(f.list) + ".txt");
        CHECK(MissingRetailClasses(*db).empty());
        std::vector<uint32_t> keys;
        for (const auto& c : db->classes()) keys.push_back(c->key);
        for (uint32_t k : keys) {
            CHECK(IsGameClass(*db, k));
            ClassEditor(*db).remove(*db->findClass(k));
        }
        CHECK(db->classes().empty());
        CHECK_EQ(db->collectionCount(), size_t(0));
        CHECK_EQ(MissingRetailClasses(*db).size(), keys.size());
        for (uint32_t k : keys) CheckOk(RestoreRetailClass(*db, k));
        for (uint32_t k : keys) {
            const ClassDef* a = original->findClass(k);
            const ClassDef* b = db->findClass(k);
            CHECK(b != nullptr);
            if (!SameDefs(*a, *b) || !SameStatics(*a, *b)) std::printf("        %s: %s differs\n", f.folder, db->names().format(k).c_str());
            CHECK(SameDefs(*a, *b));
            CHECK(SameStatics(*a, *b));
        }
        // The pack still writes and reads back.
        for (const auto& pk : db->packs()) Reload(*db, *pk);
    }
}

namespace {

const char* kModClass = R"(
class mod_racer in vault db {
    layout SPEED Float
    layout TARGET RefSpec
    layout WEIGHTS Float[4]
    layout ENABLED Bool
    layout POS Vector4
    entry COUNT Int32
    entry NAMES StringKey[]
    entry TINT Vector4
}
collection mod_racer/base under root in vault db {
    SPEED = 1.5
    WEIGHTS = [1, 2, 3]
    ENABLED = true
    POS = (1, 2, 3, 4)
    COUNT = 7
    NAMES = ["a", "b"]
}
collection mod_racer/child under mod_racer/base in vault db {
    SPEED = 3
}
)";

} // namespace

TEST("classes: a new class lays out naturally, saves, loads and holds its values") {
    if (!Available("pc/prostreet-vanilla")) return;
    auto db = LoadAttributes("pc/prostreet-vanilla");
    const size_t before = db->classes().size();
    const uint32_t declared = db->numClassesDeclared();
    const sc::ApplyReport r = Run(*db, kModClass);
    CheckOk(r);
    CHECK_EQ(r.classesCreated, size_t(1));
    CHECK_EQ(db->classes().size(), before + 1);
    const ClassDef* cls = db->findClass(StringHash32("mod_racer"));
    CHECK(cls != nullptr);
    CHECK(!IsGameClass(*db, cls->key));
    auto def = [&](const char* name) { return cls->field(StringHash32(name)); };
    // Natural layout: RefSpec at 4, the array's header at 16 and its slots at 24, Vector4 on 16.
    CHECK_EQ(def("SPEED")->offset, uint16_t(0));
    CHECK_EQ(def("TARGET")->offset, uint16_t(4));
    CHECK_EQ(def("WEIGHTS")->offset, uint16_t(16));
    CHECK_EQ(def("WEIGHTS")->maxCount, uint16_t(4));
    CHECK_EQ(def("ENABLED")->offset, uint16_t(40));
    CHECK_EQ(def("POS")->offset, uint16_t(48));
    CHECK_EQ(def("POS")->alignment, uint8_t(4));
    CHECK_EQ(cls->layoutSize, 64u);
    CHECK_EQ(cls->layoutCount, uint16_t(5));
    CHECK_EQ(def("COUNT")->flags, uint8_t(DefFlag::IsBound));
    CHECK_EQ(def("NAMES")->flags, uint8_t(DefFlag::IsBound | DefFlag::Array));
    CHECK_EQ(def("WEIGHTS")->flags, uint8_t(DefFlag::IsBound | DefFlag::Array | DefFlag::InLayout));
    CHECK_EQ(cls->staticSize, 0u);
    CHECK(std::is_sorted(cls->fields.begin(), cls->fields.end(), [](const FieldDef& a, const FieldDef& b) { return a.key < b.key; }));
    // Room for one more class, as retail files have.
    CHECK(db->numClassesDeclared() >= declared);
    CHECK(db->numClassesDeclared() >= (db->classes().size() + 2) * 3 / 2);

    const auto back = Reload(*db, *db->packs()[0]);
    const ClassDef* cls2 = back->findClass(cls->key);
    CHECK(cls2 != nullptr);
    CHECK(SameDefs(*cls, *cls2));
    CHECK(SameStatics(*cls, *cls2));
    CHECK_EQ(Exported(*back, cls->key), Exported(*db, cls->key));
    CHECK_EQ(back->numClassesDeclared(), db->numClassesDeclared());
    CHECK_EQ(sc::Print(sc::NewScript(*back, {sc::ClassStatement(*back, *cls2)})), sc::Print(sc::NewScript(*db, {sc::ClassStatement(*db, *cls)})));
    const Collection* child = back->findCollection(cls->key, StringHash32("child"));
    CHECK(child != nullptr);
    const Attribute* count = child->resolve(StringHash32("COUNT"));
    CHECK(count && count->data.bytes.size() == 4 && load32(count->data.bytes.data(), Endian::Little) == 7);
    // A Vector4 entry array header pads to 16 (Database.h ArrayPad).
    CheckOk(Run(*db, "edit mod_racer/child { TINT = (1, 1, 1, 1) }\n"));
}

TEST("classes: a redesign of the user's own class carries its collections' values") {
    if (!Available("pc/prostreet-vanilla")) return;
    auto db = LoadAttributes("pc/prostreet-vanilla");
    CheckOk(Run(*db, kModClass));
    // COUNT moves into the layout (child takes what it inherited), NAMES goes,
    // WEIGHTS keeps two slots, TARGET becomes an entry, a field is added.
    const sc::ApplyReport r = Run(*db, R"(
class mod_racer {
    layout SPEED Float
    layout COUNT Int32
    layout WEIGHTS Float[2]
    layout ENABLED Bool
    layout POS Vector4
    entry TARGET RefSpec
    entry TINT Vector4
    entry EXTRA Float
}
)");
    CheckOk(r);
    CHECK_EQ(r.classesChanged, size_t(1));
    const ClassDef* cls = db->findClass(StringHash32("mod_racer"));
    CHECK(cls->field(StringHash32("NAMES")) == nullptr);
    CHECK(cls->field(StringHash32("COUNT"))->inLayout());
    const auto back = Reload(*db, *db->packs()[0]);
    const std::string text = Exported(*back, cls->key);
    CHECK(text.find("NAMES") == std::string::npos);
    CHECK(text.find("WEIGHTS = [1, 2]") != std::string::npos);
    const Collection* child = back->findCollection(cls->key, StringHash32("child"));
    const Attribute* count = child->find(StringHash32("COUNT"));
    CHECK(count && count->inLayout && load32(count->data.bytes.data(), Endian::Little) == 7);
    // The same design again changes nothing.
    const sc::ApplyReport again = Run(*db, sc::Print(sc::NewScript(*db, {sc::ClassStatement(*db, *cls)})));
    CheckOk(again);
    CHECK_EQ(again.classesChanged, size_t(0));
    // A rename follows into the collections' export ids.
    ClassDef* c = db->findClass(StringHash32("mod_racer"));
    ClassEditor(*db).rename(*c, "mod_racer2");
    CHECK(db->findCollection(StringHash32("mod_racer2"), StringHash32("base")) != nullptr);
    CHECK_EQ(uint32_t(db->findCollection(StringHash32("mod_racer2"), StringHash32("base"))->exportId), StringHash32("mod_racer2/base"));
    Reload(*db, *db->packs()[0]);
    // And it goes, with its collections.
    const size_t cols = db->collectionCount();
    const sc::ApplyReport del = Run(*db, "delete class mod_racer2\n");
    CheckOk(del);
    CHECK_EQ(del.classesDeleted, size_t(1));
    CHECK_EQ(db->collectionCount(), cols - 2);
    CHECK(db->findClass(StringHash32("mod_racer2")) == nullptr);
}

TEST("classes: the game's classes can't be redefined or renamed, only deleted and restored") {
    if (!Available("pc/prostreet-vanilla")) return;
    auto db = LoadAttributes("pc/prostreet-vanilla");
    ClassDef* pv = db->findClass(StringHash32("pvehicle"));
    CHECK(pv && IsGameClass(*db, pv->key));
    ClassDesign d = DesignOf(*db, *pv);
    FieldDesign extra;
    extra.name = "MOD_EXTRA";
    extra.key = StringHash32(extra.name);
    extra.typeHash = StringHash32("EA::Reflection::Float");
    d.fields.push_back(extra);
    bool threw = false;
    try {
        ClassEditor(*db).redesign(*pv, d);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        ClassEditor(*db).rename(*pv, "pvehicle_mod");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    // In a script: as it is, nothing happens; changed, it stops.
    const std::string same = sc::Print(sc::NewScript(*db, {sc::ClassStatement(*db, *pv)}));
    const sc::ApplyReport ok = sc::Apply(*db, sc::Parse(same, "same.amod"));
    CheckOk(ok);
    CHECK(!db->packs()[0]->dirty);
    const sc::ApplyReport bad = Run(*db, "class pvehicle {\n    entry MOD_EXTRA Float\n}\n");
    CHECK(!bad.ok);
    CHECK(!bad.messages.empty() && bad.messages.back().text.find("class of the game") != std::string::npos);
    // The other games' platforms count too: Pro Street PS3 has no list of its own.
    Database ps3;
    for (const auto& l : Lists()) ps3.addClassList(l);
    ps3.setTarget(Target{Game::ProStreet, Platform::PS3});
    CHECK(IsGameClass(ps3, StringHash32("pvehicle")));
    CHECK(!IsGameClass(ps3, StringHash32("mod_racer")));
}

TEST("classes: a new class has no static fields and goes in the database's vault") {
    if (!Available("pc/prostreet-vanilla")) return;
    auto db = LoadAttributes("pc/prostreet-vanilla");
    const sc::ApplyReport r = Run(*db, "class mod_static {\n    static SCALE Float = 2\n}\n");
    CHECK(!r.ok);
    CHECK(!r.messages.empty() && r.messages.back().text.find("static") != std::string::npos);
    CHECK(db->findClass(StringHash32("mod_static")) == nullptr);
    // A restored retail class keeps its static field (sunandsky's PreviewSunAndSky).
    CheckOk(Run(*db, "delete class sunandsky\n"));
    CheckOk(RestoreRetailClass(*db, StringHash32("sunandsky")));
    CHECK(db->findClass(StringHash32("sunandsky"))->staticSize == 12);
    // Only the database's vault holds classes.
    auto all = LoadAttributes("pc/prostreet-vanilla");
    all->loadPack(test::DataPath("attrib/databases/pc/prostreet-vanilla/cars_vault.BIN"));
    Vault* cars = all->packs()[1]->vaults.front().get();
    bool threw = false;
    try {
        ClassDesign d;
        d.name = "mod_elsewhere";
        d.key = StringHash32(d.name);
        ClassEditor(*all).create(*cars, d);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST("classes: a diff makes, changes and deletes classes") {
    if (!Available("pc/prostreet-vanilla")) return;
    const auto base = LoadAttributes("pc/prostreet-vanilla");
    const auto cur = LoadAttributes("pc/prostreet-vanilla");
    CheckOk(Run(*cur, kModClass));
    CheckOk(Run(*cur, "class mod_racer {\n    layout SPEED Float\n    entry COUNT Int32\n    entry MORE Float[3]\n}\nedit mod_racer/child { MORE = [1, 2] }\n"));
    CheckOk(Run(*cur, "delete class smackable\n"));
    const sc::Script diff = sc::Diff(*base, *cur);
    const std::string text = sc::Print(diff);
    CHECK(text.find("class mod_racer in vault db") != std::string::npos);
    CHECK(text.find("delete class smackable") != std::string::npos);
    CHECK(text.find("delete smackable/") == std::string::npos);
    auto applied = sc::CloneDatabase(*base);
    CheckOk(sc::Apply(*applied, sc::Parse(text, "diff.amod")));
    size_t left = 0;
    for (const sc::Statement& s : sc::Diff(*applied, *cur).statements)
        left += s.kind != sc::Statement::Kind::Comment && s.kind != sc::Statement::Kind::Header && s.kind != sc::Statement::Kind::Meta;
    CHECK_EQ(left, size_t(0));
    CHECK_EQ(Exported(*applied, StringHash32("mod_racer")), Exported(*cur, StringHash32("mod_racer")));
}

namespace {

bool Has(const std::string& text, const std::string& part) {
    if (text.find(part) != std::string::npos) return true;
    std::printf("        missing: %s\n", part.c_str());
    return false;
}

// Stand-ins for the types a target's headers leave to the SDK, beyond sdk_stub.h.
std::string SdkStandIns(const Database& db, const std::set<std::string>& names) {
    static const std::set<std::string> stub = {"Attrib::StringKey", "Attrib::RefSpec", "Attrib::Blob", "UMath::Vector2",
                                               "UMath::Vector3", "UMath::Vector4", "UMath::Matrix4"};
    std::string out = "// Stand-ins for the types these headers leave to an SDK.\n#pragma once\n\n";
    for (const std::string& n : names) {
        if (stub.count(n)) continue;
        uint32_t size = 4, align = 4;
        bool isEnum = false;
        if (const TypeInfo* t = db.typeByHash(StringHash32(n))) {
            size = t->size;
            align = 1u << NaturalAlignment(*t);
            isEnum = t->kind == TypeKind::Enum;
        } else if (const TypeSchema* sc = db.typeSchema()) {
            if (const StructDef* st = sc->findStruct(n)) size = st->size, align = st->align;
            else if (const EnumDef* en = sc->findEnum(n)) size = en->size, isEnum = true;
        }
        std::vector<std::string> parts;
        for (size_t b = 0;;) {
            const size_t e = n.find("::", b);
            parts.push_back(n.substr(b, e == std::string::npos ? std::string::npos : e - b));
            if (e == std::string::npos) break;
            b = e + 2;
        }
        std::string open, close;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            open += "namespace " + parts[i] + " { ";
            close += "} ";
        }
        const std::string& last = parts.back();
        if (isEnum)
            out += open + "enum " + last + " : " + (size == 1 ? "unsigned char" : size == 2 ? "unsigned short" : "int") + " {}; " + close + "\n";
        else
            out += open + "struct alignas(" + std::to_string(align) + ") " + last + " { unsigned char bytes[" + std::to_string(size) + "]; }; " +
                   close + "\n";
    }
    return out;
}

} // namespace

TEST("classes: Attrib::Gen headers follow the game's generated classes") {
    if (!Available("pc/prostreet-vanilla")) return;
    auto db = LoadAttributes("pc/prostreet-vanilla");
    CheckOk(Run(*db, kModClass));
    const std::string h = GenHeader(*db, *db->findClass(StringHash32("mod_racer")));
    char key[16];
    std::snprintf(key, sizeof(key), "0x%08Xu", StringHash32("mod_racer"));
    CHECK(Has(h, "class mod_racer : private Instance"));
    // Namespace contents are indented, a level for each namespace.
    CHECK(Has(h, "namespace Attrib\n{\n    namespace Gen\n    {\n\n        class mod_racer : private Instance\n        {\n        public:\n"));
    CHECK(Has(h, "        };\n\n        // The offsets the class's definitions give.\n"));
    CHECK(Has(h, ";\n\n    } // namespace Gen\n} // namespace Attrib\n"));
    CHECK(Has(h, std::string("static const unsigned int kClassKey = ") + key + "; // \"mod_racer\""));
    CHECK(Has(h, "                Attrib::Private _Array_WEIGHTS; // 0x10: the array's header"));
    CHECK(Has(h, "                float WEIGHTS[4]; // 0x18"));
    CHECK(Has(h, "                UMath::Vector4 POS; // 0x30"));
    CHECK(Has(h, "        static_assert(sizeof(mod_racer::_LayoutStruct) == 64"));
    CHECK(Has(h, "        static_assert(offsetof(mod_racer::_LayoutStruct, POS) == 0x30"));
    CHECK(Has(h, "            const float& SPEED() const { return Layout()->SPEED; }"));
    CHECK(Has(h, "            unsigned int Num_NAMES() const { return Get(kKey_NAMES).GetLength(); }"));
    CHECK(Has(h, "                if (index < Layout()->_Array_WEIGHTS.GetLength()) return Layout()->WEIGHTS[index];"));
    CHECK(Has(h, "                TAttrib<int> a = GetOrClone<int>(kKey_COUNT);"));
    CHECK(Has(h, "SetDefaultLayout(sizeof(_LayoutStruct))"));
    CHECK(h.find("DefaultArea()") == std::string::npos);
    // The game's own smackable, member for member as its _LayoutStruct (ProStreet08Milestone.h).
    const std::string sm = GenHeader(*db, *db->findClass(StringHash32("smackable")));
    for (const char* m : {"Attrib::RefSpec SFX; // 0x0", "Attrib::StringKey EventSequencer; // 0xC", "const char* CollectionName; // 0x14",
                          "bool NO_CAR_EFFECT; // 0x18", "float MASS; // 0x1C", "bool IsWooshable; // 0x20", "float DETACH_FORCE; // 0x24",
                          "unsigned int COST_TO_STATE; // 0x28"})
        CHECK(Has(sm, m));
    CHECK(Has(sm, "            bool BEHAVIORS(Attrib::StringKey& result, unsigned int index) const"));
    // A layout larger than the game's default data area (1368 bytes in Pro Street) gets one of its own.
    CheckOk(Run(*db, "class mod_big {\n    layout TABLE Float[1000]\n}\n"));
    const std::string big = GenHeader(*db, *db->findClass(StringHash32("mod_big")));
    CHECK(Has(big, "            static const void* DefaultArea()"));
    CHECK(big.find("DefaultDataArea(") == std::string::npos);
    CHECK(big.find("SetDefaultLayout") == std::string::npos);
    CHECK(!DesignWarnings(*db, DesignOf(*db, *db->findClass(StringHash32("mod_big")))).empty());
    // Most Wanted's Instance takes an owner, and its smackable is laid out as its DWARF dump says.
    if (Available("pc/mw-vanilla")) {
        auto mw = LoadAttributes("pc/mw-vanilla");
        const std::string m = GenHeader(*mw, *mw->findClass(StringHash32("smackable")));
        CHECK(Has(m, "struct smackable : Instance"));
        CHECK(Has(m, "unsigned int msgPort, UTL::COM::IUnknown* owner)"));
        CHECK(Has(m, "GenerateUniqueKey(const char* name, bool registerName) const { return GUKeyInternal(ClassKey(), name, registerName); }"));
        for (const char* x : {"Attrib::StringKey EventSequencer; // 0x0", "const char* CollectionName; // 0x10", "float MASS; // 0x14",
                              "float DETACH_FORCE; // 0x18", "bool NO_CAR_EFFECT; // 0x1C", "bool IsWooshable; // 0x1D"})
            CHECK(Has(m, x));
    }

    // With ATTRIB_GEN_HEADERS_DIR, every class of the PC games' headers for
    // scripts/check_gen_headers.sh to compile as 32-bit code.
    const char* dir = std::getenv("ATTRIB_GEN_HEADERS_DIR");
    if (!dir) return;
    for (const ClassFolder& f : kFolders) {
        const std::string folder = f.folder;
        if (folder.rfind("pc/", 0) != 0 || !f.title || !Available(f.folder)) continue;
        const auto d = LoadAttributes(f.folder);
        CheckOk(Run(*d, kModClass));
        const std::string out = std::string(dir) + "/" + f.list;
        fs::create_directories(out);
        std::set<std::string> sdk;
        std::string all = "#include \"sdk_stub.h\"\n#include \"types.h\"\n";
        size_t skipped = 0;
        for (const auto& c : d->classes()) {
            GenHeaderOptions o;
            o.types = true;
            const std::string text = GenHeader(*d, *c, o);
            // Every type it names that it doesn't define.
            bool templ = false;
            for (const FieldDef& fd : c->fields) {
                if (fd.isStatic()) continue;
                const std::string t = CppTypeOf(fd.type);
                if (t.find('<') != std::string::npos) templ = true;
                if (t.find(' ') == std::string::npos && t.find('*') == std::string::npos &&
                    text.find("struct " + t + " //") == std::string::npos && text.find("enum class " + t + " :") == std::string::npos &&
                    !(t == "bool" || t == "int" || t == "float" || t == "double" || t == "char" || t == "short"))
                    sdk.insert(t);
            }
            if (const size_t at = text.find("//   types: "); at != std::string::npos) {
                const size_t b = at + 12, e = text.find('\n', b);
                std::stringstream ss(text.substr(b, e - b));
                for (std::string n; std::getline(ss, n, ',');) sdk.insert(n.substr(n.find_first_not_of(' ')));
            }
            if (templ) {
                ++skipped;
                continue;
            }
            const std::string file = CppIdentifier(d->names().format(c->key)) + ".h";
            std::ofstream(out + "/" + file, std::ios::binary) << text;
            all += "#include \"" + file + "\"\n";
        }
        std::ofstream(out + "/types.h", std::ios::binary) << SdkStandIns(*d, sdk);
        std::ofstream(out + "/all.cpp", std::ios::binary) << all;
        std::printf("        %s: %zu headers (%zu with template types left out)\n", f.list, d->classes().size() - skipped, skipped);
    }
}

TEST("classes: a restored class's empty index table fills as its collections come (current-gen Undercover)") {
    if (!Available("360/undercover-vanilla")) return;
    auto db = LoadAttributes("360/undercover-vanilla");
    CheckOk(Run(*db, "delete class aud_moment\n"));
    CheckOk(RestoreRetailClass(*db, StringHash32("aud_moment")));
    const ClassDef* cls = db->findClass(StringHash32("aud_moment"));
    const FieldDef* table = nullptr;
    for (const FieldDef& f : cls->fields)
        if (f.isStatic() && f.type && f.type->name == kIndexTableType) table = &f;
    CHECK(table != nullptr);
    CHECK_EQ(load32(cls->staticData.bytes.data() + table->offset, Endian::Big), 0u);
    CheckOk(Run(*db, "create aud_moment/mod_a under root in vault db\ncreate aud_moment/mod_b under aud_moment/mod_a\n"));
    // Each parent (0 for the roots) with its children: two keys.
    CHECK_EQ(load32(cls->staticData.bytes.data() + table->offset, Endian::Big), 2u);
    Reload(*db, *db->packs()[0]);
}
