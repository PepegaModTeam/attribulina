// Type schema tests: the generated Pro Street schema parses, binds to the
// database's types, agrees with the verified pointer table, and decodes values.

#include "Test.h"

#include "app/ValueEditors.h"
#include "core/ClassDesign.h"
#include "core/Database.h"
#include "core/Folder.h"
#include "core/Game.h"
#include "core/Hash.h"
#include "core/Schema.h"
#include "core/Script.h"
#include "core/Writer.h"

#include <algorithm>
#include <filesystem>
#include <functional>

using namespace attrib;

namespace {
std::string PS(const char* file) { return test::DataPath(std::string("attrib/databases/pc/prostreet-vanilla/") + file); }

std::shared_ptr<const TypeSchema> ProStreetSchema() {
    static auto s = TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt"));
    return s;
}

void LoadAll(Database& db) {
    db.addTypeSchema(ProStreetSchema());
    db.loadPack(PS("ATTRIBUTES.BIN"));
    db.loadPack(PS("cars_vault.BIN"));
    db.loadPack(PS("FE_ATTRIB.BIN"));
    db.loadPack(PS("gameplay.lzc"));
}
} // namespace

TEST("schema: the Pro Street schema parses") {
    auto s = ProStreetSchema();
    CHECK(s->title == "Need for Speed: ProStreet");
    CHECK_EQ(s->structCount(), size_t(85));
    CHECK_EQ(s->enumCount(), size_t(123));
    const StructDef* elr = s->findStruct("EffectLinkageRecord");
    CHECK(elr && elr->size == 32 && elr->members.size() == 4);
    CHECK(elr->members[0].st && elr->members[0].st->name == "Attrib::RefSpec");
    CHECK(s->findEnum("GRace::Type") != nullptr);
}

TEST("schema: parse errors name the line") {
    bool threw = false;
    try {
        TypeSchema::Parse("struct A 4 4\n  x nosuchtype 0\nend\n", "inline");
    } catch (const FormatError& e) {
        threw = std::string(e.what()).find("unknown type nosuchtype") != std::string::npos;
    }
    CHECK(threw);
    threw = false;
    try {
        TypeSchema::Parse("enum E 4\n  A zz\nend\n", "inline");
    } catch (const FormatError& e) {
        threw = std::string(e.what()).find("inline:2") != std::string::npos;
    }
    CHECK(threw);
}

TEST("schema: binds to the database's types") {
    Database db;
    db.addTypeSchema(ProStreetSchema());
    db.loadPack(PS("ATTRIBUTES.BIN"));
    CHECK(db.typeSchema() == ProStreetSchema().get());
    size_t structs = 0, enums = 0, unknown = 0;
    for (const TypeInfo& t : db.types()) {
        structs += t.kind == TypeKind::Struct;
        enums += t.kind == TypeKind::Enum;
        unknown += t.kind == TypeKind::Unknown;
    }
    std::printf("        %zu struct types, %zu enum types, %zu still unknown\n", structs, enums, unknown);
    CHECK(db.typeByHash(StringHash32("EffectLinkageRecord"))->kind == TypeKind::Struct);
    CHECK(db.typeByHash(StringHash32("GRace::Type"))->kind == TypeKind::Enum);
    // Built-in kinds keep their editors but still know their schema struct.
    const TypeInfo* curve = db.typeByHash(StringHash32("Curve"));
    CHECK(curve->kind == TypeKind::Curve && curve->schemaStruct);
    CHECK(enums > 100);
    CHECK(unknown < 25);
}

TEST("schema: each platform's database picks its schema, in either order") {
    auto pc = ProStreetSchema();
    auto ps2 = TypeSchema::Load(test::DataPath("data/schema/prostreet_ps2.txt"));
    CHECK_EQ(pc->boolSize, 1u);
    CHECK_EQ(ps2->boolSize, 4u);
    CHECK_EQ(ps2->findStruct("FEPartData")->size, 68u);
    for (const char* platform : {"pc", "360", "ps2"}) {
        for (bool pcFirst : {true, false}) {
            Database db;
            db.addTypeSchema(pcFirst ? pc : ps2);
            db.addTypeSchema(pcFirst ? ps2 : pc);
            db.loadPack(test::DataPath(std::string("attrib/databases/") + platform + "/prostreet-vanilla/ATTRIBUTES.BIN"));
            CHECK(db.typeSchema() == (std::string(platform) == "ps2" ? ps2.get() : pc.get()));
        }
    }
}

TEST("schema: derived pointer layouts match the verified table") {
    auto s = ProStreetSchema();
    for (const char* name : {"Attrib::StringKey", "ControllerDataRecord", "CCarKitSlotEntry", "TargetTimeOverrides", "SlotStats",
                             "FEPartCamera", "Curve", "VariableArray", "DynamicSizeArray", "CCarSlotEntry", "FEPartData",
                             "Attrib::Blob"}) {
        const StructDef* st = s->findStruct(name);
        const PointerLayout* hand = st ? PointerLayoutFor(name, st->size) : nullptr;
        CHECK(st && hand);
        const PointerLayout d = DerivePointerLayout(*st, *s);
        auto strings = d.strings;
        auto handStrings = hand->strings;
        std::sort(strings.begin(), strings.end());
        std::sort(handStrings.begin(), handStrings.end());
        if (strings != handStrings || d.nested.size() != hand->nested.size())
            std::printf("        %s differs: %zu/%zu strings, %zu/%zu nested\n", name, strings.size(), handStrings.size(),
                        d.nested.size(), hand->nested.size());
        CHECK(strings == handStrings);
        CHECK_EQ(d.nested.size(), hand->nested.size());
        for (size_t i = 0; i < d.nested.size() && i < hand->nested.size(); ++i) {
            CHECK_EQ(d.nested[i].offset, hand->nested[i].offset);
            CHECK_EQ(d.nested[i].elemSize, hand->nested[i].elemSize);
            CHECK(d.nested[i].count == hand->nested[i].count);
            if (d.nested[i].count != CountSource::One) CHECK_EQ(d.nested[i].countOffset, hand->nested[i].countOffset);
        }
    }
}

TEST("schema: every pack still round trips with the schema applied") {
    Database db;
    LoadAll(db);
    const char* files[] = {"ATTRIBUTES.BIN", "cars_vault.BIN", "FE_ATTRIB.BIN", "gameplay.bin"};
    for (size_t i = 0; i < db.packs().size(); ++i) {
        const LoadedPack& p = *db.packs()[i];
        for (const auto& v : p.vaults) CHECK(v->exactlyWritable);
        CHECK(WritePack(db, p) == ReadFile(PS(files[i])));
    }
}

TEST("schema: struct values decode through the schema") {
    Database db;
    db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    LoadAll(db);
    ClassDef* pv = db.findClass(StringHash32("pvehicle"));
    Collection* def = db.findCollection(pv->key, StringHash32("default"));
    const Attribute* hit = def->find(StringHash32("OnHitGround"));
    CHECK(hit && hit->type->kind == TypeKind::Struct);
    const app::ValueType vt = app::TypeOfAttribute(*hit, "OnHitGround");
    CHECK(app::ExpandsIntoMembers(vt));
    const MemberDef* surface = vt.st->member("mSurface");
    const MemberDef* minSpeed = vt.st->member("mMinSpeed");
    CHECK(surface && minSpeed);
    const app::ValueType st = app::TypeOfMember(*surface);
    CHECK(st.kind == TypeKind::RefSpec);
    CHECK_EQ(app::FormatValue(db, hit->data, surface->offset, st, Endian::Little), std::string("simsurface/default"));
    // Element 0 has no effect (collection key 0); the others name theirs.
    const MemberDef* effect = vt.st->member("mEffect");
    const app::ValueType et = app::TypeOfMember(*effect);
    CHECK_EQ(app::FormatValue(db, hit->data, effect->offset, et, Endian::Little), std::string("effects/(none)"));
    size_t resolved = 0;
    for (uint32_t i = 1; i < hit->count; ++i) {
        const std::string fx = app::FormatValue(db, hit->data, i * hit->elemSize + effect->offset, et, Endian::Little);
        resolved += fx.rfind("effects/", 0) == 0 && fx.find("0x") == std::string::npos;
    }
    CHECK_EQ(resolved, size_t(hit->count - 1));
    CHECK_EQ(app::FormatValue(db, hit->data, minSpeed->offset, app::TypeOfMember(*minSpeed), Endian::Little), std::string("1"));

    // An enum-typed field reads as its identifier.
    size_t named = 0, enumValues = 0;
    for (const auto& cls : db.classes())
        for (const Collection* c : cls->collections)
            for (const Attribute& a : c->attributes) {
                if (!a.type || a.type->kind != TypeKind::Enum) continue;
                const app::ValueType et = app::TypeOfAttribute(a, "");
                for (uint32_t i = 0; i < a.count; ++i) {
                    ++enumValues;
                    named += et.en->nameOf(app::ReadInteger(a.data, i * a.elemSize, et, Endian::Little)) != nullptr;
                }
            }
    std::printf("        %zu of %zu enum values have a name\n", named, enumValues);
    CHECK(enumValues > 1000);
    CHECK(named * 100 >= enumValues * 95);
}

TEST("schema: a field line gives a class's integer field an enum") {
    auto s = TypeSchema::Parse("enum E 4\n  A 0\n  B 1\nend\nfield cls fld E\n", "inline");
    CHECK(s->fieldEnum(StringHash32("cls"), StringHash32("fld")) == s->findEnum("E"));
    CHECK(s->fieldEnum(StringHash32("cls"), StringHash32("other")) == nullptr);
    CHECK(s->fieldEnum(StringHash32("other"), StringHash32("fld")) == nullptr);
    bool threw = false;
    try {
        TypeSchema::Parse("field cls fld Nope\n", "inline");
    } catch (const FormatError& e) {
        threw = std::string(e.what()).find("inline:1") != std::string::npos && std::string(e.what()).find("Nope") != std::string::npos;
    }
    CHECK(threw);
}

TEST("schema: light_flares_cg's flare_texture is an eLightFlareTextures in every game, and every stored value names a texture") {
    namespace fs = std::filesystem;
    // Most Wanted's build has three textures (and the count), the later games
    // add the cat's eye and the lamp post flare, and the Wii's Undercover and
    // Nitro the sun's six. Every game folder; a folder of builds
    // (pc/world-various) stands for each of them.
    std::vector<std::pair<std::string, size_t>> folders;
    for (const auto& platform : fs::directory_iterator(test::DataPath("attrib/databases")))
        for (const auto& game : fs::directory_iterator(platform.path())) {
            const std::string name = game.path().filename().string();
            const std::string dir = platform.path().filename().string() + "/" + name;
            const size_t values = name.rfind("mw", 0) == 0 ? 4 : dir == "wii/undercover-vanilla" || dir == "wii/nitro-vanilla" ? 12 : 6;
            if (FindFileNoCase(game.path().string(), "attributes.bin")) folders.emplace_back(dir, values);
            else
                for (const auto& build : fs::directory_iterator(game.path()))
                    if (FindFileNoCase(build.path().string(), "attributes.bin")) folders.emplace_back(dir + "/" + build.path().filename().string(), values);
        }
    std::sort(folders.begin(), folders.end());
    CHECK(folders.size() >= 20);
    std::vector<std::shared_ptr<const TypeSchema>> schemas;
    for (const auto& e : fs::directory_iterator(test::DataPath("data/schema")))
        if (e.path().extension() == ".txt") schemas.push_back(TypeSchema::Load(e.path().string()));
    size_t seen = 0;
    for (const auto& [where, values] : folders) {
        const std::string dir = test::DataPath("attrib/databases/" + where);
        if (!fs::exists(dir)) continue;
        Database db;
        db.setTarget(DetectTarget(dir).target);
        for (const auto& s : schemas) db.addTypeSchema(s);
        // The names first: the early Most Wanted demos' keys are StringHash64 of them.
        db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
        // attributes.bin, which defines the classes, comes first.
        const std::vector<FolderFile> packs = FolderPacks(dir);
        CHECK(!packs.empty());
        if (packs.empty()) continue;
        db.loadPack(packs.front().path);
        const ClassDef* cls = db.findClass(StringHash32("light_flares_cg"));
        if (!cls) std::fprintf(stderr, "%s: no light_flares_cg in %s\n", where.c_str(), packs.front().path.c_str());
        CHECK(cls);
        if (!cls) continue;
        const FieldDef* f = cls->field(StringHash32("flare_texture"));
        CHECK(f && f->fieldEnum && f->fieldEnum->name == "eLightFlareTextures");
        if (!f || !f->fieldEnum) continue;
        if (f->fieldEnum->values.size() != values) std::fprintf(stderr, "%s: %zu values\n", where.c_str(), f->fieldEnum->values.size());
        CHECK_EQ(f->fieldEnum->values.size(), values);
        CHECK(f->type && f->type->kind == TypeKind::UInt32); // the type table's type stays
        const FieldDef* other = cls->field(StringHash32("MinSize"));
        CHECK(other && !other->fieldEnum);
        for (const Collection* c : cls->collections)
            if (const Attribute* a = c->find(f->key)) {
                const ValueType vt = TypeOfAttribute(*a, "flare_texture");
                CHECK(vt.kind == TypeKind::Enum && vt.en == f->fieldEnum && vt.typeName == "eLightFlareTextures");
                const int64_t v = ReadInteger(a->data, 0, vt, c->vault->raw.endian);
                if (!f->fieldEnum->nameOf(v)) std::fprintf(stderr, "%s: %s has flare_texture %lld\n", where.c_str(),
                                                           db.names().format(c->key).c_str(), static_cast<long long>(v));
                CHECK(f->fieldEnum->nameOf(v) != nullptr);
                ++seen;
            }
        // A class laid out again (restored, or a design applied) keeps it.
        ClassDef again;
        LayOutClass(db, DesignOf(db, *cls), again);
        const FieldDef* g = again.field(f->key);
        CHECK(g && g->fieldEnum == f->fieldEnum);
    }
    CHECK(seen > 100);
}

TEST("schema: Nitro's types come from its older Wii build's DWARF and its own files; every stored value of an enum has a name") {
    namespace fs = std::filesystem;
    namespace sc = attrib::script;
    const std::string dir = test::DataPath("attrib/databases/wii/nitro-vanilla");
    if (!fs::exists(dir)) return;
    Database db;
    db.setTarget(DetectTarget(dir).target);
    CHECK_EQ(db.target().id(), std::string("nitro_wii"));
    for (const auto& e : fs::directory_iterator(test::DataPath("data/schema")))
        if (e.path().extension() == ".txt") db.addTypeSchema(TypeSchema::Load(e.path().string()));
    db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    db.binNames().loadFile(test::DataPath("data/attrib_keys.txt"));
    db.binNames().loadFile(test::DataPath("data/bin_keys.txt"));
    for (const FolderFile& f : FolderPacks(dir))
        if (f.skipped.empty()) db.loadPack(f.path);
    CHECK(db.typeSchema() && db.typeSchema()->source.find("nitro_wii.txt") != std::string::npos);
    // Nitro's race types, from the DWARF: not Undercover's (Knockout is 3 there).
    const EnumDef* race = db.typeSchema()->findEnum("GRace::Type");
    CHECK(race && race->nameOf(9) && *race->nameOf(9) == "kRaceType_TeamRace");
    // Every type that holds a pointer is described: the type layouts cover each
    // one (the round trip in test_writer), and only types without data stay raw.
    size_t unknown = 0;
    for (const TypeInfo& t : db.types()) unknown += t.kind == TypeKind::Unknown;
    CHECK(unknown <= 13);
    auto text = [&](const char* cls, const char* name) {
        const Collection* c = db.findCollection(StringHash32(cls), StringHash32(name));
        CHECK(c != nullptr);
        if (!c) return std::string();
        std::vector<const Collection*> cs = {c};
        return sc::Print(sc::NewScript(db, sc::Export(db, cs)));
    };
    auto has = [](const std::string& t, const char* what) {
        if (t.find(what) == std::string::npos) std::fprintf(stderr, "no %s in:\n%s\n", what, t.substr(0, 2000).c_str());
        return t.find(what) != std::string::npos;
    };
    // The four types of its own (scripts/nitro/nitro_types.h) and their strings.
    CHECK(has(text("interactive_music_settings", "tracks"), "Name = \"AlltheCash\""));
    CHECK(has(text("frontend_sfx", "feng_sound_map"), "mSoundName = \"BTN_BACK\""));
    CHECK(has(text("presetridepackage", "tier3_1st"), "MCOROLLA_MADRID_01"));
    CHECK(has(text("parameter_maps", "reverb"), "DebugName = \"Tunnel\""));
    // Names found for Nitro: fields from its code, collections by their pattern.
    CHECK(has(text("audio_pickup", "nitro_level_1"), "collection audio_pickup/nitro_level_1 under audio_pickup/nitro"));
    CHECK(db.names().find(StringHash32("DefaultHwVoice")) != nullptr);
    // Every enum value stored names a value of its enum, in attributes and in
    // struct members, but for ParticleTextureRecord's 0 (no texture), which
    // Carbon's files hold too and no enum names.
    size_t values = 0, unnamed = 0;
    std::function<void(const DataBlock&, uint32_t, const ValueType&, Endian)> walk = [&](const DataBlock& b, uint32_t off, const ValueType& vt, Endian e) {
        if (off + vt.size > b.bytes.size()) return;
        if (vt.kind == TypeKind::Enum && vt.en) {
            const int64_t v = ReadInteger(b, off, vt, e);
            ++values;
            if (!vt.en->nameOf(v) && !(vt.en->name == "eTEG_ParticleTextures" && v == 0)) {
                std::fprintf(stderr, "%s %lld has no name\n", vt.en->name.c_str(), static_cast<long long>(v));
                ++unnamed;
            }
        } else if (vt.kind == TypeKind::Struct && vt.st)
            for (const MemberDef& m : vt.st->members)
                if (!m.isBitfield())
                    for (uint32_t i = 0; i < m.count; ++i) walk(b, off + m.offset + i * m.elemSize, TypeOfMember(m), e);
    };
    for (const auto& cls : db.classes())
        for (const Collection* c : cls->collections)
            for (const Attribute& a : c->attributes) {
                const ValueType vt = TypeOfAttribute(a, db.names().format(a.key));
                for (uint32_t i = 0; i < a.count; ++i) walk(a.data, uint32_t(a.elem(i) - a.data.bytes.data()), vt, c->vault->raw.endian);
            }
    CHECK(values > 1000);
    CHECK_EQ(unnamed, size_t(0));
}
