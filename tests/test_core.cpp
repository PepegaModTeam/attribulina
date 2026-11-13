// Core library tests against the retail Pro Street PC databases.

#include "Test.h"

#include "core/Compression.h"
#include "core/Curve.h"
#include "core/Database.h"
#include "core/Edit.h"
#include "core/Hash.h"
#include "core/Paint.h"
#include "core/Schema.h"
#include "core/UserFolder.h"
#include "core/Values.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace attrib;

namespace {
const char* kProStreet = "attrib/databases/pc/prostreet-vanilla/";

std::string PS(const char* file) { return test::DataPath(std::string(kProStreet) + file); }

Database& ProStreetDb() {
    static Database* db = [] {
        auto* d = new Database();
        d->loadPack(PS("ATTRIBUTES.BIN"));
        return d;
    }();
    return *db;
}
} // namespace

TEST("hash: StringHash32 and BinHash") {
    CHECK_EQ(StringHash32("Attrib::DatabaseLoadData"), 0xCBBC628Fu);
    CHECK_EQ(StringHash32("Attrib::ClassLoadData"), 0x5E970CBCu);
    CHECK_EQ(StringHash32("Attrib::CollectionLoadData"), 0x8E112EB7u);
    CHECK_EQ(StringHash32("db.vlt"), 0x18228ADEu);
    CHECK_EQ(StringHash32("basicrace/stategraph_basicrace_racing_handler_triggerenter"), 0x2B6A5CD7u);
    // StringHash64, as the early Most Wanted demos' vaults hold it.
    CHECK_EQ(StringHash64("db.vlt"), 0xA2CC374D1050E73Eull);
    CHECK_EQ(StringHash64("db.bin"), 0xCE274DB98549AB4Dull);
    CHECK_EQ(StringHash64("Attrib::DatabaseLoadData"), 0x0B38846845E9C175ull); // 24 bytes: one full round
    CHECK_EQ(StringHash64("Attrib::CollectionLoadData"), 0xAD303B8F42B3307Eull);
    CHECK_EQ(StringHash64("nos/default"), 0x67D4312146E703A3ull);
    CHECK_EQ(StringHash64("EA::Reflection::Float"), 0xE22228FBB3C209D8ull);
    CHECK_EQ(StringHash64("race_bin_10/10_2_1_sprint/startgrid"), 0x95FFCB0810DCE55Full);
    CHECK_EQ(StringHash64("gameplay/race_bin_10/10_2_1_sprint/startgrid"), 0xAE04533612488838ull);
    CHECK_EQ(StringHash64(""), 0ull);
    CHECK_EQ(StringHash32(""), StringHash32(std::string_view()));
    CHECK_EQ(BinHash(""), 0xFFFFFFFFu);
    CHECK_EQ(BinHash("a"), 0xFFFFFFFFu * 33u + 'a');
}

TEST("compression: gameplay.lzc decodes to gameplay.bin") {
    const Bytes lzc = ReadFile(PS("gameplay.lzc"));
    const Bytes bin = ReadFile(PS("gameplay.bin"));
    auto h = ParseLzHeader(lzc);
    CHECK(h && h->kind == LzKind::Jdlz);
    CHECK(Decompress(lzc) == bin);
}

TEST("compression: JDLZ/HUFF/RAWW/RFPK round trip") {
    const Bytes bin = ReadFile(PS("gameplay.bin"));
    const Bytes sample(bin.begin(), bin.begin() + 200000);
    for (LzKind k : {LzKind::Jdlz, LzKind::Huff, LzKind::Raww, LzKind::Rfpk}) {
        const Bytes packed = Compress(k, sample);
        auto h = ParseLzHeader(packed);
        CHECK(h && h->kind == k);
        CHECK(ParseLzKind(LzKindName(k)) == k);
        // HUFF and RFPK count their stream only, like the game's own blocks.
        CHECK_EQ(h->compressedSize, uint32_t(packed.size()) - (k == LzKind::Huff || k == LzKind::Rfpk ? 16u : 0u));
        CHECK_EQ(h->uncompressedSize, uint32_t(sample.size()));
        CHECK(Decompress(packed) == sample);
    }
    // A RefPack stream whose own size isn't the header's is refused before it is decoded.
    Bytes bad = Compress(LzKind::Rfpk, sample);
    bad[0x10 + 4] ^= 1;
    bool threw = false;
    try {
        Decompress(bad);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST("compression: Undercover's 360 alpha's gameplay.lzc is RFPK: it decodes to gameplay.bin, and the game's encoder writes it again") {
    const std::string dir = test::DataPath("attrib/databases/360/undercover-alpha161");
    if (!std::filesystem::exists(dir)) return;
    const Bytes lzc = ReadFile(dir + "/GAMEPLAY.LZC");
    auto h = ParseLzHeader(lzc);
    CHECK(h && h->kind == LzKind::Rfpk && h->version == 1 && h->flags == 0);
    const Bytes bin = Decompress(lzc);
    CHECK(bin == ReadFile(dir + "/GAMEPLAY.BIN"));
    CHECK(Compress(LzKind::Rfpk, bin) == lzc);
}

TEST("load: ATTRIBUTES.BIN schema") {
    Database& db = ProStreetDb();
    CHECK_EQ(db.types().size(), size_t(251));
    CHECK_EQ(db.classes().size(), size_t(141));
    CHECK_EQ(db.collectionCount(), size_t(6356));
    const TypeInfo* f = db.typeByHash(StringHash32("EA::Reflection::Float"));
    CHECK(f && f->size == 4 && f->kind == TypeKind::Float);
    const TypeInfo* curve = db.typeByHash(StringHash32("Curve"));
    CHECK(curve && curve->size == 44 && curve->kind == TypeKind::Curve);
    for (const auto& pack : db.packs())
        for (const auto& v : pack->vaults)
            CHECK(v->warnings.empty());
}

TEST("load: pvehicle/default values") {
    Database& db = ProStreetDb();
    ClassDef* pv = db.findClass(StringHash32("pvehicle"));
    CHECK(pv);
    CHECK_EQ(pv->fields.size(), size_t(46));
    CHECK_EQ(pv->layoutSize, 44u);
    Collection* def = db.findCollection(pv->key, StringHash32("default"));
    CHECK(def);

    // Layout field: frontend RefSpec at layout offset 8.
    const Attribute* frontend = def->find(StringHash32("frontend"));
    CHECK(frontend && frontend->inLayout && frontend->elemSize == 12);
    CHECK_EQ(load32(frontend->elem(0), Endian::Little), 0x85885722u);
    CHECK_EQ(load32(frontend->elem(0) + 4, Endian::Little), 0xC1C32DAFu);

    // Array entry of EffectLinkageRecord, 9 elements.
    const Attribute* hit = def->find(StringHash32("OnHitGround"));
    CHECK(hit && hit->isArray && hit->count == 9 && hit->elemSize == 32);

    // StringKey with its string resolved through the pointer table.
    const Attribute* order = def->find(StringHash32("BEHAVIOR_ORDER"));
    CHECK(order && order->isArray && order->count == 8 && order->elemSize == 8);
    const PtrSlot* s = order->data.ptrAt(4);
    CHECK(s && s->isString);
    CHECK_EQ(StringHash32(s->text), load32(order->elem(0), Endian::Little));
}

TEST("load: every StringKey hash matches its string") {
    Database& db = ProStreetDb();
    size_t checked = 0;
    for (const auto& cls : db.classes())
        for (const Collection* c : cls->collections)
            for (const Attribute& a : c->attributes) {
                if (!a.type || a.type->kind != TypeKind::StringKey) continue;
                for (uint32_t i = 0; i < a.count; ++i) {
                    const PtrSlot* s = a.data.ptrAt(i * a.elemSize + 4);
                    const uint32_t h = load32(a.elem(i), Endian::Little);
                    if (!s) { CHECK_EQ(h, 0u); continue; }
                    CHECK(s->isString);
                    // Retail quirk: song names with non-ASCII letters were written
                    // with '?' in place of them but hashed from the original
                    // ("D?N?"), so the stored hash must be kept as loaded.
                    if (s->text.find('?') != std::string::npos) continue;
                    // An empty string is stored with key 0, not StringHash32("").
                    CHECK_EQ(s->text.empty() ? 0u : StringHash32(s->text), h);
                    ++checked;
                }
            }
    CHECK(checked > 1000);
}

TEST("load: cars_vault and gameplay on top of the database") {
    Database db;
    db.loadPack(PS("ATTRIBUTES.BIN"));
    const size_t base = db.collectionCount();
    LoadedPack& cars = db.loadPack(PS("cars_vault.BIN"));
    CHECK_EQ(cars.vaults.size(), size_t(1));
    LoadedPack& gp = db.loadPack(PS("gameplay.lzc"));
    CHECK(gp.compression && *gp.compression == LzKind::Jdlz);
    CHECK_EQ(gp.vaults.size(), size_t(0x11E));
    CHECK(db.collectionCount() > base);
    for (const auto& pack : db.packs())
        for (const auto& v : pack->vaults)
            for (const auto& w : v->warnings)
                std::printf("        warning (%s): %s\n", v->name.c_str(), w.c_str());
}

TEST("load: collections without a database are rejected") {
    Database db;
    bool threw = false;
    try {
        db.loadPack(PS("cars_vault.BIN"));
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST("curve: regenerated Y2 matches the retail curves") {
    Database& db = ProStreetDb();
    size_t curves = 0, stale = 0;
    for (const auto& cls : db.classes())
        for (const Collection* c : cls->collections)
            for (const Attribute& a : c->attributes) {
                if (!a.type || a.type->kind != TypeKind::Curve) continue;
                for (uint32_t i = 0; i < a.count; ++i) {
                    CurveValue cv = ReadCurve(a, i, Endian::Little);
                    CHECK_EQ(cv.x.size(), cv.y.size());
                    CHECK_EQ(cv.y2.size(), cv.x.size());
                    const auto y2 = NaturalY2(cv.x, cv.y);
                    float worst = 0;
                    for (size_t k = 0; k < y2.size(); ++k)
                        worst = std::max(worst, std::abs(y2[k] - cv.y2[k]) / std::max(1.0f, std::abs(cv.y2[k])));
                    ++curves;
                    if (worst >= 1e-3f) ++stale;
                }
            }
    std::printf("        %zu curves, %zu with stale Y2\n", curves, stale);
    CHECK_EQ(curves, size_t(3622));
    // One retail curve (class 0xE3C144F9, collection "close") had its X/Y
    // edited without regenerating Y2; every other one reproduces.
    CHECK_EQ(stale, size_t(1));
}

TEST("curve: a matrix's curve evaluates as the games read it") {
    auto near = [](float a, float b) { return std::abs(a - b) < 1e-5f; };
    // GetValueFromSpline (Most Wanted PC 0x73DAB0, GameCube 0x802E6548): the
    // y column's Bernstein polynomial; x plays no part.
    CurvePoints c;
    const float xs[4] = {0.3f, 0.9f, 0.1f, 0.5f}, ys[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    for (int i = 0; i < 4; ++i) {
        c.x[i] = xs[i];
        c.y[i] = ys[i];
    }
    CHECK(near(CurveYAt(c, 0.0f), 0.0f) && near(CurveYAt(c, 1.0f), 1.0f));
    CHECK(near(CurveYAt(c, 0.5f), 0.5f)); // 3 * 0.5 * 0.25 + 0.125
    CHECK(near(CurveYAt(c, 0.25f), 3 * 0.25f * 0.75f * 0.75f + 0.25f * 0.25f * 0.25f));
    CHECK(near(MatrixCurveValue(c, MatrixCurveRead::YOverT, 0.25f), CurveYAt(c, 0.25f)));
    CHECK(near(MatrixCurveValue(c, MatrixCurveRead::YOverT, 2.0f), 1.0f)); // t is held to 0..1
    // The Bezier: points along y = x with uneven handles stay on y = x, so
    // SplineMap's lookup by x gives x back; before the first point and past
    // the last it holds the ends.
    CurvePoints line;
    const float ls[4] = {0.0f, 0.1f, 0.7f, 1.0f};
    for (int i = 0; i < 4; ++i) line.x[i] = line.y[i] = ls[i];
    for (float x = 0.05f; x < 1.0f; x += 0.1f) CHECK(near(SplineMapValue(line, x), x));
    CHECK(near(SplineMapValue(line, -1.0f), 0.0f) && near(SplineMapValue(line, 2.0f), 1.0f));
    float px, py;
    CurvePointAt(line, 0.5f, px, py);
    CHECK(near(px, py) && near(px, 0.125f * 0 + 0.375f * 0.1f + 0.375f * 0.7f + 0.125f));
    // Rows are (x, y, z, w); writing a point leaves z and w, in either byte order.
    for (Endian e : {Endian::Little, Endian::Big}) {
        uint8_t m[64];
        for (int k = 0; k < 16; ++k) storef32(m + k * 4, float(k), e);
        WriteCurvePoint(m, 2, 0.25f, 0.75f, e);
        const CurvePoints r = ReadCurvePoints(m, e);
        CHECK(r.x[2] == 0.25f && r.y[2] == 0.75f && r.x[1] == 4.0f && r.y[3] == 13.0f);
        CHECK(loadf32(m + 40, e) == 10.0f && loadf32(m + 44, e) == 11.0f);
    }
    // Which fields read which way.
    CHECK(MatrixCurveOf("visuallook", "BlackBloomCurve").read == MatrixCurveRead::YOverT);
    CHECK(MatrixCurveOf("visuallook", "DetailMapCurve").read == MatrixCurveRead::YOverT);
    CHECK(MatrixCurveOf("visuallookeffect", "graph").read == MatrixCurveRead::YOverT);
    CHECK(MatrixCurveOf("engineaudio", "PhysicsRPM_Map").read == MatrixCurveRead::YOverT);
    const MatrixCurveInfo rgb = MatrixCurveOf("visuallook", "RGBCurve");
    CHECK(rgb.read == MatrixCurveRead::Bezier && rgb.reader);
    CHECK(MatrixCurveOf("shiftpattern", "Up_Engage_Curve").read == MatrixCurveRead::Bezier);
    const MatrixCurveInfo unread = MatrixCurveOf("visualrgbtweaker", "red");
    CHECK(unread.read == MatrixCurveRead::Bezier && !unread.reader);
}

TEST("edit: array insert/remove keeps pointer slots attached") {
    Database db;
    db.loadPack(PS("ATTRIBUTES.BIN"));
    ClassDef* pv = db.findClass(StringHash32("pvehicle"));
    Collection* def = db.findCollection(pv->key, StringHash32("default"));
    Attribute* order = def->find(StringHash32("BEHAVIOR_ORDER"));
    CHECK(order && order->count == 8);
    const std::string second = order->data.ptrAt(1 * 8 + 4)->text;
    InsertElement(*order, 0);
    CHECK_EQ(order->count, 9u);
    CHECK(order->data.ptrAt(4) == nullptr);
    CHECK_EQ(order->data.ptrAt(2 * 8 + 4)->text, second);
    RemoveElement(*order, 0);
    RemoveElement(*order, 0);
    CHECK_EQ(order->count, 7u);
    CHECK_EQ(order->data.ptrAt(4)->text, second);
}

TEST("edit: override, revert and collection lifetime") {
    Database db;
    db.loadPack(PS("ATTRIBUTES.BIN"));
    ClassDef* pv = db.findClass(StringHash32("pvehicle"));
    Collection* def = db.findCollection(pv->key, StringHash32("default"));
    CollectionEditor ed(db);
    Collection& child = ed.create(*def->vault, *pv, "attribulina_test", def);
    CHECK(db.findCollection(pv->key, StringHash32("attribulina_test")) == &child);
    CHECK(child.parent == def);

    // Inherited until overridden; the override is an independent copy.
    const FieldDef* f = pv->field(StringHash32("BEHAVIOR_ORDER"));
    const Collection* owner = nullptr;
    CHECK(child.resolve(f->key, &owner) && owner == def);
    Attribute& own = OverrideAttribute(db, child, *f);
    CHECK(child.resolve(f->key, &owner) && owner == &child);
    own.data.ptrAt(4)->text = "changed";
    CHECK(def->find(f->key)->data.ptrAt(4)->text != "changed");
    CHECK(RevertAttribute(child, f->key));
    CHECK(child.resolve(f->key, &owner) && owner == def);

    ed.rename(child, "attribulina_test2");
    CHECK(db.findCollection(pv->key, StringHash32("attribulina_test")) == nullptr);
    CHECK(!ed.remove(*def)); // has children
    CHECK(ed.remove(child));
    CHECK(db.findCollection(pv->key, StringHash32("attribulina_test2")) == nullptr);
}

TEST("hash: bStringHash names resolve through Database::binNames") {
    KeyNames bin(&BinHash);
    bin.add("SCENERY_GROUP_DOOR");
    CHECK(bin.find(BinHash("SCENERY_GROUP_DOOR")) != nullptr);
    CHECK(bin.find(StringHash32("SCENERY_GROUP_DOOR")) == nullptr);
    CHECK_EQ(bin.format(0x1234ABCDu), std::string("0x1234ABCD"));
    // Every vault string goes into both dictionaries.
    Database& db = ProStreetDb();
    const std::string* any = nullptr;
    for (const auto& v : db.packs().front()->vaults)
        if (!v->stringOrder.empty()) any = &v->stringOrder.front();
    CHECK(any);
    CHECK(db.binNames().find(BinHash(*any)) != nullptr);
    CHECK(db.names().find(StringHash32(*any)) != nullptr);
}

TEST("hash: a vault's own string names its hash over a listed name that shares it") {
    // Two names in the list share the hash: Carbon's character and Pro
    // Street's checkpoint, whose CollectionName the vault holds.
    const uint32_t key = StringHash32("tracks/portland/portland_grip_1/checkpoint1");
    CHECK_EQ(key, StringHash32("characters/major_crew_darius/alan"));
    KeyNames n(&StringHash32);
    n.add("characters/major_crew_darius/alan");
    n.add("tracks/portland/portland_grip_1/checkpoint1");
    CHECK_EQ(*n.find(key), std::string("characters/major_crew_darius/alan"));
    CHECK(n.find64(StringHash64("characters/major_crew_darius/alan")) != nullptr);
    n.addFromData("tracks/portland/portland_grip_1/checkpoint1");
    CHECK_EQ(*n.find(key), std::string("tracks/portland/portland_grip_1/checkpoint1"));
    CHECK(n.find64(StringHash64("characters/major_crew_darius/alan")) == nullptr);
    CHECK(n.find64(StringHash64("tracks/portland/portland_grip_1/checkpoint1")) != nullptr);
    // The first string the data has keeps it; a listed name added later doesn't take it.
    n.addFromData("characters/major_crew_darius/alan");
    n.add("characters/major_crew_darius/alan");
    CHECK_EQ(*n.find(key), std::string("tracks/portland/portland_grip_1/checkpoint1"));
    KeyNames copy = n;
    copy.addFromData("characters/major_crew_darius/alan");
    CHECK_EQ(*copy.find(key), std::string("tracks/portland/portland_grip_1/checkpoint1"));

    Database db;
    db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    CHECK_EQ(db.names().format(key), std::string("characters/major_crew_darius/alan"));
    db.loadPack(PS("ATTRIBUTES.BIN"));
    db.loadPack(PS("gameplay.lzc"));
    CHECK_EQ(db.names().format(key), std::string("tracks/portland/portland_grip_1/checkpoint1"));
}

TEST("compression: CompressBest follows the game's LZCompress choice") {
    // Retail Lua chunks: 235 HUFF and 16 JDLZ, each the smaller of the two.
    Database db;
    db.loadPack(PS("ATTRIBUTES.BIN"));
    db.loadPack(PS("gameplay.lzc"));
    const ClassDef* gp = db.findClass(StringHash32("gameplay"));
    int n = 0, same = 0, identicalHuff = 0, identicalJdlz = 0;
    for (const Collection* c : gp->collections) {
        const Attribute* a = c->find(StringHash32("bytecode"));
        const PtrSlot* p = a ? a->data.ptrAt(4) : nullptr;
        if (!p || !p->data) continue;
        const Bytes raw = Decompress(p->data->bytes);
        const Bytes best = CompressBest(raw);
        const LzKind kind = ParseLzHeader(p->data->bytes)->kind;
        same += ParseLzHeader(best)->kind == kind;
        // Re-encoding gives the game's bytes, header included (HUFF counts
        // only its stream in CompressedSize).
        if (best == p->data->bytes) ++(kind == LzKind::Huff ? identicalHuff : identicalJdlz);
        CHECK(Decompress(best) == raw);
        ++n;
    }
    CHECK_EQ(n, 251);
    CHECK_EQ(same, 251);
    CHECK_EQ(identicalHuff, 235);
    CHECK_EQ(identicalJdlz, 11);
    // Incompressible data that HUFF can't shrink goes RAWW, with the game's
    // CompressedSize (data size), and still decodes.
    Bytes noise(4096);
    uint32_t x = 12345;
    for (uint8_t& b : noise) { x = x * 1103515245u + 12345u; b = uint8_t(x >> 24); }
    const Bytes stored = CompressBest(noise);
    const auto h = ParseLzHeader(stored);
    CHECK(h.has_value());
    if (h->kind == LzKind::Raww) CHECK_EQ(h->compressedSize, uint32_t(noise.size()));
    CHECK(Decompress(stored) == noise);
}

TEST("paint: stock paints read as the chooser's colours and write back") {
    Database db;
    db.loadPack(PS("ATTRIBUTES.BIN"));
    db.loadPack(PS("cars_vault.BIN"));
    const ClassDef* car = db.findClass(StringHash32("car"));
    auto stock = [&](const char* name) -> Attribute* {
        Collection* c = db.findCollection(car->key, StringHash32(name));
        return c ? c->find(StringHash32("stockPaint")) : nullptr;
    };
    // challenger71: swatch1 kit 8 = step 24, hue 1 - 24/90 (264 degrees, purple).
    Attribute* ch = stock("challenger71");
    CHECK(ch);
    PaintColour pc = ReadPaint(ch->data, 0, Endian::Little);
    CHECK(pc.hueKnown);
    CHECK_EQ(pc.step, 24);
    CHECK(std::abs(pc.h * 360.0f - 264.0f) < 0.01f);
    CHECK(std::abs(pc.s - 0.5f) < 1e-6f && std::abs(pc.v - 0.8f) < 1e-6f);
    db.names().add("generic_body_paint_group_gloss");
    CHECK_EQ(PaintFinish(db.names(), load32(ch->data.bytes.data() + 8, Endian::Little)), std::string("gloss"));
    // corvette: swatch0 kit 0 is step 0, red.
    PaintColour red = ReadPaint(stock("corvette")->data, 0, Endian::Little);
    CHECK_EQ(red.step, 0);
    CHECK(red.h < 1e-6f);

    // Writing a colour snaps to a step and round-trips; unchanged values keep their bytes.
    DataBlock b = ch->data;
    const Bytes before = b.bytes;
    WritePaint(b, 0, pc.h, pc.s, pc.v, Endian::Little);
    CHECK(b.bytes == before);
    WritePaint(b, 0, 200.0f / 360.0f, 0.25f, 0.75f, Endian::Little); // nearest step: 40 (200 deg)
    PaintColour w = ReadPaint(b, 0, Endian::Little);
    CHECK_EQ(w.step, 40);
    CHECK_EQ(PaintSwatchIndex(load32(b.bytes.data() + 20, Endian::Little)), 2);
    CHECK_EQ(load32(b.bytes.data() + 28, Endian::Little), 8u);
    CHECK(std::abs(w.s - 0.25f) < 1e-6f && std::abs(w.v - 0.75f) < 1e-6f);
    WritePaint(b, 0, 0.999f, 1, 1, Endian::Little); // just below red wraps to step 0
    CHECK_EQ(ReadPaint(b, 0, Endian::Little).step, 0);
}

TEST("paint: current-gen Undercover's paints read their hue from car_paint_swatch/swatch") {
    for (const char* folder : {"360/undercover-vanilla", "pc/undercover-vanilla"}) {
        Database db;
        db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/undercover_pc.txt")));
        const LoadedPack& pack = db.loadPack(test::DataPath(std::string("attrib/databases/") + folder + "/ATTRIBUTES.BIN"));
        const Endian e = pack.vaults.front()->raw.endian;
        const PaintSwatches sw = ReadPaintSwatches(db);
        CHECK_EQ(sw.rgb.size(), size_t(70));
        if (sw.rgb.size() != 70) continue;
        CHECK_EQ(sw.rgb[0], 0xFFFF0000u); // red, then round the hues
        CHECK(std::abs(sw.hue[0]) < 1e-6f);
        CHECK(std::abs(sw.hue[40] * 360.0f - 180.0f) < 0.01f); // 00FFFF
        CHECK(std::abs(sw.hue[50] * 360.0f - 240.0f) < 0.01f); // 0000FF
        auto paints = [&](const char* name) -> Attribute* {
            Collection* c = db.findCollection(StringHash32("presetride"), StringHash32(name));
            return c ? c->find(StringHash32("Paints")) : nullptr;
        };
        Attribute* first = paints("first_car");
        CHECK(first && first->elemSize == 32);
        if (!first) continue;
        const ValueType vt = TypeOfAttribute(*first, "Paints");
        CHECK(vt.st && IsSwatchPaint(*vt.st));
        // Its body: swatch 46 (0066FF, 216 degrees), saturation 1, variance 0.592382.
        const PaintColour pc = ReadSwatchPaint(first->data, 0, sw, e);
        CHECK_EQ(pc.step, 46);
        CHECK(pc.hueKnown);
        CHECK(std::abs(pc.h * 360.0f - 216.0f) < 0.01f);
        CHECK(std::abs(pc.s - 1.0f) < 1e-6f && std::abs(pc.v - 0.592382f) < 1e-6f);

        // The same colour writes back unchanged; S and V alone keep the swatch.
        DataBlock b = first->data;
        const Bytes before = b.bytes;
        WriteSwatchPaint(b, 0, pc.h, pc.s, pc.v, sw, e);
        CHECK(b.bytes == before);
        WriteSwatchPaint(b, 0, pc.h, 0.25f, 0.75f, sw, e);
        PaintColour w = ReadSwatchPaint(b, 0, sw, e);
        CHECK_EQ(w.step, 46);
        CHECK(std::abs(w.s - 0.25f) < 1e-6f && std::abs(w.v - 0.75f) < 1e-6f);
        // A hue picks the nearest swatch: 200 degrees is 43's 197.9, not 44's 204.
        WriteSwatchPaint(b, 0, 200.0f / 360.0f, 0.25f, 0.75f, sw, e);
        CHECK_EQ(ReadSwatchPaint(b, 0, sw, e).step, 43);
        WriteSwatchPaint(b, 0, 0.999f, 1, 1, sw, e); // just below red: 0, not 69 (353.9)
        CHECK_EQ(ReadSwatchPaint(b, 0, sw, e).step, 0);
        CHECK(std::equal(b.bytes.begin() + 28, b.bytes.end(), before.begin() + 28)); // the rest stays

        // An index past the table shows swatch 0, as the game uses it, until a hue is picked.
        Attribute* traffic = paints("nis_traffic");
        CHECK(traffic);
        if (!traffic) continue;
        DataBlock t = traffic->data;
        const PaintColour tc = ReadSwatchPaint(t, 0, sw, e);
        CHECK_EQ(tc.step, 100);
        CHECK(!tc.hueKnown);
        CHECK(tc.h == 0.0f);
        WriteSwatchPaint(t, 0, tc.h, 0.5f, 0.5f, sw, e);
        CHECK_EQ(ReadSwatchPaint(t, 0, sw, e).step, 100);
        WriteSwatchPaint(t, 0, 0.5f, 0.5f, 0.5f, sw, e);
        CHECK_EQ(ReadSwatchPaint(t, 0, sw, e).step, 40);
    }
    // Without the class there is no table.
    Database ps;
    ps.loadPack(PS("ATTRIBUTES.BIN"));
    CHECK(ReadPaintSwatches(ps).empty());
}

TEST("paint: current-gen Undercover's vinyl colours take a swatch's hue, with saturation and brightness in 255ths") {
    for (const char* folder : {"360/undercover-vanilla", "pc/undercover-vanilla"}) {
        Database db;
        db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/undercover_pc.txt")));
        db.loadPack(test::DataPath(std::string("attrib/databases/") + folder + "/ATTRIBUTES.BIN"));
        const PaintSwatches sw = ReadPaintSwatches(db);
        CHECK_EQ(sw.rgb.size(), size_t(70));
        if (sw.rgb.size() != 70) continue;
        Collection* ride = db.findCollection(StringHash32("presetride"), StringHash32("racer_072"));
        Attribute* layers = ride ? ride->find(StringHash32("VinylLayers")) : nullptr;
        CHECK(layers && layers->count > 0);
        if (!layers || !layers->count) continue;
        const ValueType vt = TypeOfAttribute(*layers, "VinylLayers");
        const MemberDef* colors = vt.st ? vt.st->member("colors") : nullptr;
        CHECK(colors && colors->count == 4 && colors->st && IsVinylColour(*colors->st));
        if (!colors || !colors->st) continue;
        // The first layer's first colour is swatch 33, saturation 64, brightness 9;
        // the other three are 255, colours the layer doesn't use.
        DataBlock b = layers->data;
        const uint32_t at = colors->offset;
        const PaintColour pc = ReadVinylColour(b, at, sw);
        CHECK_EQ(pc.step, 33);
        CHECK(pc.hueKnown);
        CHECK(std::abs(pc.h - sw.hue[33]) < 1e-6f);
        CHECK(std::abs(pc.s - 64 / 255.0f) < 1e-6f && std::abs(pc.v - 9 / 255.0f) < 1e-6f);

        // The same colour writes back unchanged; S and V alone keep the swatch.
        const Bytes before = b.bytes;
        WriteVinylColour(b, at, pc.h, pc.s, pc.v, sw);
        CHECK(b.bytes == before);
        WriteVinylColour(b, at, pc.h, 0.5f, 1.0f, sw);
        CHECK_EQ(int(b.bytes[at]), 33);
        CHECK_EQ(int(b.bytes[at + 1]), 128);
        CHECK_EQ(int(b.bytes[at + 2]), 255);
        // A hue picks the nearest swatch: 200 degrees is 43's 197.9, not 44's 204.
        WriteVinylColour(b, at, 200.0f / 360.0f, 0.5f, 1.0f, sw);
        CHECK_EQ(int(b.bytes[at]), 43);

        // 255 is past the table: hue 0, as the game's RGB2HSV gives the black it
        // reads there, and kept while only S and V change.
        const PaintColour unused = ReadVinylColour(b, at + 3, sw);
        CHECK_EQ(unused.step, 255);
        CHECK(!unused.hueKnown);
        CHECK(unused.h == 0.0f);
        WriteVinylColour(b, at + 3, unused.h, 0.5f, 0.5f, sw);
        CHECK_EQ(int(b.bytes[at + 3]), 255);
        CHECK(std::equal(b.bytes.begin() + at + 6, b.bytes.end(), before.begin() + at + 6)); // the rest stays
    }
}

TEST("user folder: an attribulina.ini beside the program keeps the user's files there, else they go to the per-user folder") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "attribulina_userfolder_test";
    fs::remove_all(dir);
    const fs::path exe = dir / "program", per = dir / "roaming" / "Attribulina";
    fs::create_directories(exe);
    // No settings file beside the program: the per-user folder, made for it.
    UserFolder u = ChooseUserFolder(exe.string(), per.string());
    CHECK(!u.portable && u.path == per.string());
    CHECK(fs::is_directory(per));
    // With one there, everything stays beside the program, as it always did.
    std::ofstream(exe / kSettingsFileName) << "[Attribulina][Settings]\n";
    u = ChooseUserFolder(exe.string(), per.string());
    CHECK(u.portable && u.path == exe.string());
    // A platform without a per-user folder, or one that can't be made, stays portable.
    const fs::path bare = dir / "bare";
    fs::create_directories(bare);
    CHECK(ChooseUserFolder(bare.string(), "").portable);
    std::ofstream(dir / "a_file") << "x";
    u = ChooseUserFolder(bare.string(), (dir / "a_file" / "Attribulina").string());
    CHECK(u.portable && u.path == bare.string());
    if (std::getenv("HOME")) CHECK(!PerUserFolder().empty());
    fs::remove_all(dir);
}
