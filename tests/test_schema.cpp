// Type schema tests: the generated Pro Street schema parses, binds to the
// database's types, agrees with the verified pointer table, and decodes values.

#include "Test.h"

#include "app/ValueEditors.h"
#include "core/Database.h"
#include "core/Hash.h"
#include "core/Schema.h"
#include "core/Writer.h"

#include <algorithm>

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
    CHECK_EQ(s->enumCount(), size_t(122));
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
