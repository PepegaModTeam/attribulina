// Database search (app/Search.h) over the full Pro Street PC database with
// the type schema applied, as the editor loads it. Test values are taken from
// the data where possible instead of being hard-coded.

#include "Test.h"

#include "app/Search.h"
#include "core/Database.h"
#include "core/Gameplay.h"
#include "core/Hash.h"
#include "core/Schema.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>

using namespace attrib;
using app::SearchHit;
using app::SearchMode;
using app::SearchQuery;
using app::SearchResult;

namespace {
std::string PS(const char* file) { return test::DataPath(std::string("attrib/databases/pc/prostreet-vanilla/") + file); }

// Every pack, schema and name list, as App::ensureDatabase loads them. Loaded
// once; the tests only read it.
const Database& Full() {
    static std::unique_ptr<Database> db = [] {
        namespace fs = std::filesystem;
        auto d = std::make_unique<Database>();
        for (const auto& e : fs::directory_iterator(test::DataPath("data/schema")))
            if (e.path().extension() == ".txt") d->addTypeSchema(TypeSchema::Load(e.path().string()));
        d->names().loadFile(test::DataPath("data/attrib_keys.txt"));
        d->binNames().loadFile(test::DataPath("data/attrib_keys.txt"));
        d->binNames().loadFile(test::DataPath("data/bin_keys.txt"));
        for (const MessageDef& m : LoadMessages(test::DataPath("data/messages/prostreet_pc.txt"))) d->names().add(m.name);
        d->loadPack(PS("ATTRIBUTES.BIN"));
        d->loadPack(PS("cars_vault.BIN"));
        d->loadPack(PS("FE_ATTRIB.BIN"));
        d->loadPack(PS("gameplay.lzc"));
        return d;
    }();
    return *db;
}

const uint32_t kGameplay = StringHash32("gameplay");
const char* const kRace = "prostreet_career/bins/race_bin_an_2_1/gr_1";
const char* const kLayout = "tracks/autobahn_gd/autobahn_gd_grip_1";

const Collection& Gp(const char* name) {
    const Collection* c = Full().findCollection(kGameplay, StringHash32(name));
    if (!c) throw test::Failure{std::string("no gameplay collection ") + name};
    return *c;
}

std::string Hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

Endian EndianOf(const Collection& c) { return c.vault ? c.vault->raw.endian : Endian::Little; }

std::string FieldName(uint32_t key) { return Full().names().format(key); }

const SearchHit* Find(const SearchResult& r, uint32_t cls, uint32_t key, uint32_t field, const std::string& where) {
    for (const SearchHit& h : r.hits)
        if (h.classKey == cls && h.key == key && h.field == field && h.where == where) return &h;
    return nullptr;
}

SearchResult Values(const std::string& text, bool whole, uint32_t inClass = 0, app::SearchHash hash = app::SearchHash::StringHash32) {
    SearchQuery q;
    q.text = text;
    q.wholeValue = whole;
    q.inClass = inClass;
    q.hexHash = hash;
    q.limit = 1000000;
    return app::RunSearch(Full(), q);
}

SearchResult References(uint32_t cls, uint32_t key, uint32_t inClass = 0) {
    SearchQuery q;
    q.mode = SearchMode::References;
    q.refClass = cls;
    q.refKey = key;
    q.inClass = inClass;
    q.limit = 1000000;
    return app::RunSearch(Full(), q);
}

// The first own single (not array) attribute in the database that `pick` accepts.
struct Found {
    const Collection* c = nullptr;
    const Attribute* a = nullptr;
    app::ValueType vt;
};
Found FirstAttribute(const std::function<bool(const Collection&, const Attribute&, const app::ValueType&)>& pick) {
    const Database& db = Full();
    for (const auto& cls : db.classes())
        for (const Collection* c : cls->collections)
            for (const Attribute& a : c->attributes) {
                if (a.isArray || !a.elemSize || a.data.bytes.size() < a.elemSize) continue;
                app::ValueType vt = app::TypeOfAttribute(a, db.names().format(a.key));
                if (pick(*c, a, vt)) return {c, &a, vt};
            }
    throw test::Failure{"no attribute in the data fits"};
}

// Classes, then collections, by lowercased name.
bool InOrder(const SearchHit& a, const SearchHit& b) {
    const KeyNames& n = Full().names();
    const std::string ca = app::ToLower(n.format(a.classKey)), cb = app::ToLower(n.format(b.classKey));
    if (ca != cb) return ca < cb;
    return app::ToLower(n.format(a.key)) <= app::ToLower(n.format(b.key));
}
} // namespace

// ---------------------------------------------------------- references ---

TEST("search: references to a track layout list the races and the folder") {
    const Database& db = Full();
    const Collection& layout = Gp(kLayout);
    const SearchResult r = References(kGameplay, layout.key);
    CHECK(r.error.empty());
    CHECK_EQ(r.collectionsSearched, db.collectionCount());

    const SearchHit* race = Find(r, kGameplay, StringHash32(kRace), StringHash32("TrackLayout"), "TrackLayout");
    CHECK(race);
    CHECK_EQ(race->value, std::string(kLayout));

    // The folder lists it in Children.
    const Collection& folder = Gp("tracks/autobahn_gd");
    const Attribute* children = folder.find(StringHash32("Children"));
    CHECK(children);
    int index = -1;
    for (uint32_t i = 0; i < children->count && index < 0; ++i)
        if (load32(children->elem(i), EndianOf(folder)) == layout.key) index = int(i);
    CHECK(index >= 0);
    const SearchHit* listed = Find(r, kGameplay, folder.key, StringHash32("Children"), "Children[" + std::to_string(index) + "]");
    CHECK(listed);
    CHECK_EQ(listed->value, std::string(kLayout));

    // Its own Name and CollectionName are its name, not references to it.
    for (const SearchHit& h : r.hits) CHECK(h.key != layout.key);
}

TEST("search: references to a template list what inherits from it") {
    const Collection& circuit = Gp("circuit");
    const SearchResult r = References(kGameplay, circuit.key);
    CHECK(r.error.empty());
    const SearchHit* h = Find(r, kGameplay, StringHash32(kRace), 0, "parent");
    CHECK(h);
    CHECK_EQ(h->value, std::string("circuit"));
    size_t parents = 0;
    for (const SearchHit& x : r.hits)
        if (x.where == "parent") ++parents;
    CHECK_EQ(parents, circuit.children.size());
}

TEST("search: a reference found in the data is found by a references search") {
    const Database& db = Full();
    const Found f = FirstAttribute([&](const Collection& c, const Attribute& a, const app::ValueType& vt) {
        if (vt.kind != TypeKind::RefSpec) return false;
        const uint32_t ck = load32(a.data.bytes.data(), EndianOf(c)), k = load32(a.data.bytes.data() + 4, EndianOf(c));
        return ck && k && db.findCollection(ck, k);
    });
    const uint32_t ck = load32(f.a->data.bytes.data(), EndianOf(*f.c)), k = load32(f.a->data.bytes.data() + 4, EndianOf(*f.c));
    const std::string where = FieldName(f.a->key);

    const SearchResult r = References(ck, k);
    CHECK(r.error.empty());
    const SearchHit* h = Find(r, f.c->classKey, f.c->key, f.a->key, where);
    CHECK(h);
    CHECK_EQ(h->value, db.names().format(ck) + "/" + db.names().format(k));

    // The class as the target: the same hit, and only references of that class.
    const SearchResult rc = References(ck, 0);
    CHECK(rc.error.empty());
    CHECK(Find(rc, f.c->classKey, f.c->key, f.a->key, where));
    for (const SearchHit& x : rc.hits) CHECK(x.value.rfind(db.names().format(ck) + "/", 0) == 0);

    // Limited to the class the reference is in.
    const SearchResult ri = References(ck, k, f.c->classKey);
    CHECK(Find(ri, f.c->classKey, f.c->key, f.a->key, where));
    for (const SearchHit& x : ri.hits) CHECK_EQ(x.classKey, f.c->classKey);
}

TEST("search: references inside pointed-to arrays have their element path") {
    // CCarSlotEntry.parts is a DynamicSizeArray: RefSpecs behind a pointer,
    // with the count beside it.
    const Database& db = Full();
    const StructDef* dyn = db.typeSchema()->findStruct("DynamicSizeArray");
    const StructDef* slot = db.typeSchema()->findStruct("CCarSlotEntry");
    CHECK(dyn && slot);
    const uint32_t partsOff = slot->member("parts")->offset;
    const uint32_t arrayOff = partsOff + dyn->member("mArray")->offset;
    const uint32_t lengthOff = partsOff + dyn->member("mLength")->offset;
    uint32_t ck = 0, k = 0, element = 0;
    const Found f = FirstAttribute([&](const Collection& c, const Attribute& a, const app::ValueType& vt) {
        if (vt.st != slot) return false;
        const PtrSlot* s = a.data.ptrAt(arrayOff);
        const uint32_t n = load32(a.data.bytes.data() + lengthOff, EndianOf(c));
        for (uint32_t i = 0; s && s->data && i < n && (i + 1) * 12 <= s->data->bytes.size(); ++i) {
            ck = load32(s->data->bytes.data() + i * 12, EndianOf(c));
            k = load32(s->data->bytes.data() + i * 12 + 4, EndianOf(c));
            element = i;
            if (ck && k && db.findCollection(ck, k)) return true;
        }
        return false;
    });
    const SearchResult r = References(ck, k, f.c->classKey);
    CHECK(Find(r, f.c->classKey, f.c->key, f.a->key, FieldName(f.a->key) + ".parts.mArray[" + std::to_string(element) + "]"));
}

TEST("search: references to a gameplay path held in a Text field") {
    // TrackRecordHolder holds an element's path instead of its key.
    const Database& db = Full();
    const uint32_t holder = StringHash32("TrackRecordHolder");
    const Found f = FirstAttribute([&](const Collection&, const Attribute& a, const app::ValueType& vt) {
        if (a.key != holder || vt.kind != TypeKind::Text) return false;
        const PtrSlot* s = a.data.ptrAt(0);
        return s && !s->text.empty() && db.findCollection(kGameplay, StringHash32(s->text));
    });
    const std::string path = f.a->data.ptrAt(0)->text;
    const SearchResult r = References(kGameplay, StringHash32(path));
    const SearchHit* h = Find(r, f.c->classKey, f.c->key, holder, "TrackRecordHolder");
    CHECK(h);
    CHECK_EQ(h->value, "\"" + path + "\"");
}

TEST("search: references to something that doesn't exist are an error") {
    SearchResult r = References(kGameplay, StringHash32("no/such/element"));
    CHECK(r.hits.empty());
    CHECK(r.error.rfind("No collection", 0) == 0);
    r = References(StringHash32("no_such_class"), 0);
    CHECK(r.error.rfind("No class", 0) == 0);
    r = References(0, 0);
    CHECK(!r.error.empty());
}

// -------------------------------------------------------------- values ---

TEST("search: a gameplay path is found as a whole value and by part of it") {
    const uint32_t race = StringHash32(kRace), field = StringHash32("TrackLayout");
    SearchResult r = Values(kLayout, true);
    CHECK(r.error.empty());
    const SearchHit* h = Find(r, kGameplay, race, field, "TrackLayout");
    CHECK(h);
    CHECK_EQ(h->value, std::string(kLayout));
    for (const SearchHit& x : r.hits) CHECK(app::ToLower(x.value).find(kLayout) != std::string::npos);

    r = Values("AUTOBAHN_GD_GRIP_1", false);
    CHECK(Find(r, kGameplay, race, field, "TrackLayout"));
    // Part of the name is not the whole value.
    r = Values("autobahn_gd_grip_1", true);
    CHECK(!Find(r, kGameplay, race, field, "TrackLayout"));
}

TEST("search: a float is found by the number the inspector shows") {
    const Found f = FirstAttribute([](const Collection& c, const Attribute& a, const app::ValueType& vt) {
        if (vt.kind != TypeKind::Float) return false;
        const float v = loadf32(a.data.bytes.data(), EndianOf(c));
        return std::fabs(v) > 0.01f && std::fabs(v) < 1e6f && v != std::trunc(v);
    });
    const std::string shown = app::FormatValue(Full(), f.a->data, 0, f.vt, EndianOf(*f.c));
    const SearchResult r = Values(shown, true);
    const SearchHit* h = Find(r, f.c->classKey, f.c->key, f.a->key, FieldName(f.a->key));
    CHECK(h);
    CHECK_EQ(h->value, shown);
}

TEST("search: a key is found by its hash") {
    const Found f = FirstAttribute([](const Collection& c, const Attribute& a, const app::ValueType& vt) {
        return vt.kind == TypeKind::Key && load32(a.data.bytes.data(), EndianOf(c)) != 0;
    });
    const uint32_t v = load32(f.a->data.bytes.data(), EndianOf(*f.c));
    const SearchResult r = Values(Hex(v), true);
    const SearchHit* h = Find(r, f.c->classKey, f.c->key, f.a->key, FieldName(f.a->key));
    CHECK(h);
    CHECK_EQ(h->value, Full().names().format(v));
    // Lower-case hex works too.
    CHECK(Find(Values(app::ToLower(Hex(v)), true), f.c->classKey, f.c->key, f.a->key, FieldName(f.a->key)));
}

TEST("search: an enum is found by its enumerator name and by its number") {
    const Found f = FirstAttribute([](const Collection& c, const Attribute& a, const app::ValueType& vt) {
        return vt.kind == TypeKind::Enum && vt.en && vt.en->nameOf(app::ReadInteger(a.data, 0, vt, EndianOf(c)));
    });
    const int64_t v = app::ReadInteger(f.a->data, 0, f.vt, EndianOf(*f.c));
    const std::string name = *f.vt.en->nameOf(v);
    const std::string where = FieldName(f.a->key);
    const SearchResult r = Values(name, true);
    const SearchHit* h = Find(r, f.c->classKey, f.c->key, f.a->key, where);
    CHECK(h);
    CHECK_EQ(h->value, name);
    CHECK(Find(Values(std::to_string(v), true, f.c->classKey), f.c->classKey, f.c->key, f.a->key, where));
}

TEST("search: a string key is found by its text") {
    const Found f = FirstAttribute([](const Collection&, const Attribute& a, const app::ValueType& vt) {
        const PtrSlot* s = vt.kind == TypeKind::StringKey ? a.data.ptrAt(4) : nullptr;
        return s && s->text.size() > 3;
    });
    const std::string text = f.a->data.ptrAt(4)->text;
    CHECK(Find(Values(text, true), f.c->classKey, f.c->key, f.a->key, FieldName(f.a->key)));
}

TEST("search: values behind pointers have their path") {
    // FEPartData: PartDetails points at one FEPartDetail (no index in the
    // path), autosculptCamera1 at a counted array of MiniRefSpec.
    const Database& db = Full();
    const StructDef* part = db.typeSchema()->findStruct("FEPartData");
    CHECK(part);
    const MemberDef* details = part->member("PartDetails");
    const MemberDef* cams = part->member("autosculptCamera1");
    const MemberDef* camCount = part->member("mCam1Length");
    CHECK(details && cams && camCount);
    const ClassDef* cls = db.findClass(StringHash32("part_fedata"));
    CHECK(cls);
    const uint32_t partData = StringHash32("partData");

    bool sawDetail = false, sawCamera = false;
    for (const Collection* c : cls->collections) {
        const Attribute* a = c->find(partData);
        if (!a || (sawDetail && sawCamera)) continue;
        for (uint32_t i = 0; i < a->count; ++i) {
            const uint32_t base = i * a->elemSize;
            const std::string element = "partData[" + std::to_string(i) + "]";
            const PtrSlot* d = a->data.ptrAt(base + details->offset);
            if (!sawDetail && d && d->data && d->data->bytes.size() >= 4)
                if (const uint32_t logo = load32(d->data->bytes.data(), EndianOf(*c))) {
                    CHECK(Find(Values(Hex(logo), true, cls->key), cls->key, c->key, partData, element + ".PartDetails.Logo"));
                    sawDetail = true;
                }
            const PtrSlot* s = a->data.ptrAt(base + cams->offset);
            const uint32_t n = a->data.bytes[base + camCount->offset];
            if (!sawCamera && s && s->data && n && s->data->bytes.size() >= 4)
                if (const uint32_t key = load32(s->data->bytes.data(), EndianOf(*c))) {
                    CHECK(Find(Values(Hex(key), true, cls->key), cls->key, c->key, partData,
                               element + ".autosculptCamera1[0].mCollectionKey"));
                    sawCamera = true;
                }
        }
    }
    CHECK(sawDetail);
    CHECK(sawCamera);
}

TEST("search: a values search over the whole database is quick") {
    const Database& db = Full();
    for (bool whole : {true, false}) {
        const auto t0 = std::chrono::steady_clock::now();
        const SearchResult r = Values("no value is called this", whole);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("        %s search of %zu collections: %.1f ms\n", whole ? "whole-value" : "contains", r.collectionsSearched, ms);
        CHECK(r.hits.empty());
        CHECK_EQ(r.collectionsSearched, db.collectionCount());
    }
    const auto t0 = std::chrono::steady_clock::now();
    const SearchResult r = Values("1", true);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("        \"1\": %zu hits in %.1f ms\n", r.hits.size(), ms);
}

TEST("search: hits are sorted by class, collection and element") {
    const SearchResult r = Values("autobahn", false);
    CHECK(r.hits.size() > 100);
    for (size_t i = 1; i < r.hits.size(); ++i) {
        const SearchHit &a = r.hits[i - 1], &b = r.hits[i];
        CHECK(InOrder(a, b));
        // Element indices go up numerically ([2] before [10]).
        if (a.key != b.key || a.field != b.field) continue;
        const size_t pa = a.where.find('['), pb = b.where.find('[');
        if (pa == std::string::npos || pb == std::string::npos || a.where.substr(0, pa) != b.where.substr(0, pb)) continue;
        CHECK(std::stoul(a.where.substr(pa + 1)) <= std::stoul(b.where.substr(pb + 1)));
    }
}

TEST("search: the limit keeps the first hits") {
    const SearchResult all = Values("autobahn", false);
    CHECK(!all.truncated);
    SearchQuery q;
    q.text = "autobahn";
    q.limit = 100;
    const SearchResult some = app::RunSearch(Full(), q);
    CHECK(some.truncated);
    CHECK_EQ(some.hits.size(), size_t(100));
    for (size_t i = 0; i < some.hits.size(); ++i) {
        CHECK_EQ(some.hits[i].key, all.hits[i].key);
        CHECK_EQ(some.hits[i].where, all.hits[i].where);
    }
    q.limit = all.hits.size();
    const SearchResult exact = app::RunSearch(Full(), q);
    CHECK(!exact.truncated);
    CHECK_EQ(exact.hits.size(), all.hits.size());
}

TEST("search: an empty query is an error") {
    for (SearchMode m : {SearchMode::Values, SearchMode::Fields}) {
        SearchQuery q;
        q.mode = m;
        q.text = "  ";
        const SearchResult r = app::RunSearch(Full(), q);
        CHECK_EQ(r.error, std::string("Type something to search for"));
        CHECK(r.hits.empty());
    }
}

TEST("search: a collection is found by its name, an unknown name by its hex or part of it, and its children by their parent") {
    // Pro Street's packages nobody has a name for: nothing refers to them but
    // their children's parent link.
    const Database& db = Full();
    const Collection* unnamed = nullptr;
    for (const auto& cls : db.classes())
        for (const Collection* c : cls->collections)
            if (!unnamed && !db.names().find(c->key) && c->children.size() > 2) unnamed = c;
    CHECK(unnamed);
    if (!unnamed) return;
    const std::string hex = Hex(unnamed->key);
    SearchResult r = Values(hex, true);
    const SearchHit* self = Find(r, unnamed->classKey, unnamed->key, 0, "(name)");
    CHECK(self);
    if (self) CHECK_EQ(self->value, hex);
    for (const Collection* ch : unnamed->children) {
        const SearchHit* h = Find(r, ch->classKey, ch->key, 0, "parent");
        CHECK(h);
        if (h) CHECK_EQ(h->value, hex);
    }
    // In lower case, and part of it when the whole value isn't asked for.
    CHECK(Find(Values(app::ToLower(hex), true), unnamed->classKey, unnamed->key, 0, "(name)"));
    const std::string part = hex.substr(0, 6);
    CHECK(Find(Values(part, false), unnamed->classKey, unnamed->key, 0, "(name)"));
    CHECK(!Find(Values(part, true), unnamed->classKey, unnamed->key, 0, "(name)"));
    // Children list their parent only when the query names it whole.
    CHECK(!Find(Values(part, false), unnamed->children.front()->classKey, unnamed->children.front()->key, 0, "parent"));

    // A named collection: by its name, and the template it is made from.
    const Collection& race = Gp(kRace);
    r = Values(kRace, true);
    CHECK(Find(r, kGameplay, race.key, 0, "(name)"));
    if (race.parent) CHECK(Find(Values(db.names().format(race.parentKey), true), kGameplay, race.key, 0, "parent"));
}

TEST("search: a 0x... number finds the text and names it is the StringHash32 or bStringHash of") {
    const Database& db = Full();
    const uint32_t race = StringHash32(kRace), layout = StringHash32("TrackLayout");
    // TrackLayout is a key (GCollectionKey): its stored number as StringHash32,
    // its name's bStringHash as bStringHash.
    CHECK(Find(Values(Hex(race), true), kGameplay, race, 0, "(name)"));
    CHECK(Find(Values(Hex(StringHash32(kLayout)), true), kGameplay, race, layout, "TrackLayout"));
    CHECK(!Find(Values(Hex(StringHash32(kLayout)), true, 0, app::SearchHash::BinHash), kGameplay, race, layout, "TrackLayout"));
    CHECK(!Find(Values(Hex(BinHash(kLayout)), true), kGameplay, race, layout, "TrackLayout"));
    CHECK(Find(Values(Hex(BinHash(kLayout)), true, 0, app::SearchHash::BinHash), kGameplay, race, layout, "TrackLayout"));

    // A Text field stores no hash: its text is hashed, by the hash chosen.
    const Found text = FirstAttribute([](const Collection&, const Attribute& a, const app::ValueType& vt) {
        const PtrSlot* s = vt.kind == TypeKind::Text ? a.data.ptrAt(0) : nullptr;
        return s && s->text.size() > 3;
    });
    const std::string t = text.a->data.ptrAt(0)->text, tw = FieldName(text.a->key);
    const uint32_t tc = text.c->classKey;
    CHECK(Find(Values(Hex(StringHash32(t)), true, tc), tc, text.c->key, text.a->key, tw));
    CHECK(!Find(Values(Hex(BinHash(t)), true, tc), tc, text.c->key, text.a->key, tw));
    CHECK(Find(Values(Hex(BinHash(t)), true, tc, app::SearchHash::BinHash), tc, text.c->key, text.a->key, tw));
    CHECK(!Find(Values(Hex(StringHash32(t)), true, tc, app::SearchHash::BinHash), tc, text.c->key, text.a->key, tw));

    // A bin key (stored as a bStringHash) with a known name: by its value
    // either way, and by its name's StringHash32 as StringHash32.
    const Found bin = FirstAttribute([&](const Collection& c, const Attribute& a, const app::ValueType& vt) {
        if (vt.kind != TypeKind::BinKey) return false;
        const std::string* n = db.binNames().find(load32(a.data.bytes.data(), EndianOf(c)));
        return n && StringHash32(*n) != load32(a.data.bytes.data(), EndianOf(c));
    });
    const uint32_t stored = load32(bin.a->data.bytes.data(), EndianOf(*bin.c));
    const std::string name = *db.binNames().find(stored), where = FieldName(bin.a->key);
    for (app::SearchHash h : {app::SearchHash::StringHash32, app::SearchHash::BinHash})
        CHECK(Find(Values(Hex(stored), true, bin.c->classKey, h), bin.c->classKey, bin.c->key, bin.a->key, where));
    CHECK(Find(Values(Hex(StringHash32(name)), true, bin.c->classKey), bin.c->classKey, bin.c->key, bin.a->key, where));

    // A key (a StringHash32) with a known name: by its name's bStringHash as bStringHash.
    const Found key = FirstAttribute([&](const Collection& c, const Attribute& a, const app::ValueType& vt) {
        return vt.kind == TypeKind::Key && db.names().find(load32(a.data.bytes.data(), EndianOf(c)));
    });
    const std::string keyName = *db.names().find(load32(key.a->data.bytes.data(), EndianOf(*key.c)));
    CHECK(Find(Values(Hex(BinHash(keyName)), true, key.c->classKey, app::SearchHash::BinHash), key.c->classKey, key.c->key, key.a->key,
               FieldName(key.a->key)));

    // Field names: the field's key, or its name's bStringHash.
    SearchQuery q;
    q.mode = SearchMode::Fields;
    q.text = Hex(layout);
    q.inClass = kGameplay;
    CHECK(Find(app::RunSearch(db, q), kGameplay, race, layout, "TrackLayout"));
    q.text = Hex(BinHash("TrackLayout"));
    CHECK(!Find(app::RunSearch(db, q), kGameplay, race, layout, "TrackLayout"));
    q.hexHash = app::SearchHash::BinHash;
    CHECK(Find(app::RunSearch(db, q), kGameplay, race, layout, "TrackLayout"));
}

TEST("search: as bStringHash, a 0x... finds bStringHash values and the text and names it is the bStringHash of, nothing else") {
    const Database& db = Full();
    // Every hit of "0x" alone is a bStringHash value no name is known for.
    const SearchResult none = Values("0x", false, 0, app::SearchHash::BinHash);
    CHECK(!none.hits.empty());
    for (const SearchHit& h : none.hits) {
        CHECK(h.value.rfind("0x", 0) == 0);
        CHECK(h.value != "0x00000000" && h.value != "0xFFFFFFFF"); // no value
    }
    CHECK(none.hits.size() < 100);
    // "0x1" as StringHash32 also finds every 1 and every key shown as 0x1...;
    // as bStringHash only bStringHash values shown so.
    const SearchResult sh = Values("0x1", false), bin = Values("0x1", false, 0, app::SearchHash::BinHash);
    CHECK(sh.hits.size() > 1000);
    CHECK(bin.hits.size() < 20);
    for (const SearchHit& h : bin.hits) CHECK(h.value.rfind("0x1", 0) == 0);
    // A bStringHash value by its whole number; a Text by the bStringHash of its text.
    const Found bk = FirstAttribute([&](const Collection& c, const Attribute& a, const app::ValueType& vt) {
        return vt.kind == TypeKind::BinKey && db.binNames().find(load32(a.data.bytes.data(), EndianOf(c)));
    });
    const uint32_t v = load32(bk.a->data.bytes.data(), EndianOf(*bk.c));
    CHECK(Find(Values(Hex(v), true, 0, app::SearchHash::BinHash), bk.c->classKey, bk.c->key, bk.a->key, FieldName(bk.a->key)));
    CHECK(Values(Hex(0xFFFFFFFFu), true, 0, app::SearchHash::BinHash).hits.size() <
          Values(Hex(0xFFFFFFFFu), true).hits.size()); // empty texts aren't the empty name's
}

TEST("search: Names finds classes and collections by part of their name, the whole name, or 0x...") {
    const Database& db = Full();
    const uint32_t car = StringHash32("car"), pv = StringHash32("pvehicle"), veh = StringHash32("vehicle");
    SearchQuery q;
    q.mode = SearchMode::Names;
    q.text = "LANCEREVOX";
    q.limit = 1000000;
    SearchResult r = app::RunSearch(db, q);
    CHECK(r.error.empty());
    CHECK(Find(r, car, StringHash32("lancerevox"), 0, "(collection)"));
    CHECK(Find(r, pv, StringHash32("lancerevox"), 0, "(collection)"));
    const SearchHit* drag = Find(r, veh, StringHash32("lancerevox_draghpfall"), 0, "(collection)");
    CHECK(drag);
    for (size_t i = 1; i < r.hits.size(); ++i) CHECK(InOrder(r.hits[i - 1], r.hits[i]));
    q.wholeValue = true;
    r = app::RunSearch(db, q);
    CHECK(Find(r, pv, StringHash32("lancerevox"), 0, "(collection)"));
    CHECK(!Find(r, veh, StringHash32("lancerevox_draghpfall"), 0, "(collection)"));
    // A class: its own hit (key 0) before its collections.
    q.text = "pvehicle";
    r = app::RunSearch(db, q);
    CHECK(!r.hits.empty() && r.hits.front().classKey == pv && r.hits.front().key == 0 && r.hits.front().where == "(class)");
    // A collection's parent is its value.
    q.wholeValue = false;
    q.text = app::ToLower("0x") + Hex(StringHash32("lancerevox")).substr(2);
    r = app::RunSearch(db, q);
    const SearchHit* byKey = Find(r, pv, StringHash32("lancerevox"), 0, "(collection)");
    CHECK(byKey);
    if (const Collection* c = db.findCollection(pv, StringHash32("lancerevox")); c && byKey)
        CHECK_EQ(byKey->value, c->parentKey ? db.names().format(c->parentKey) : std::string());
    // In one class only.
    q.text = "lancerevox";
    q.inClass = car;
    r = app::RunSearch(db, q);
    CHECK(!r.hits.empty());
    for (const SearchHit& h : r.hits) CHECK_EQ(h.classKey, car);
    // No text: every collection of the class, or nothing without one.
    q.text = "";
    r = app::RunSearch(db, q);
    CHECK(r.error.empty());
    const ClassDef* carClass = db.findClass(car);
    CHECK(carClass && r.hits.size() == carClass->collections.size());
    q.inClass = 0;
    CHECK(!app::RunSearch(db, q).error.empty());
}

TEST("search: ValueLines lists every value under one with the inspector's paths") {
    // FEPartData: a struct with a pointer to one value (PartDetails, its row
    // "[0]") and a counted array (autosculptCamera1).
    const Database& db = Full();
    const ClassDef* cls = db.findClass(StringHash32("part_fedata"));
    CHECK(cls);
    if (!cls) return;
    const uint32_t partData = StringHash32("partData");
    for (const Collection* c : cls->collections) {
        const Attribute* a = c->find(partData);
        if (!a || !a->count) continue;
        const app::ValueType vt = app::TypeOfAttribute(*a, "partData");
        const std::vector<app::ValueLine> lines = app::ValueLines(db, a->data, 0, vt, EndianOf(*c), "partData[0]");
        CHECK(lines.size() > 4);
        bool logo = false;
        for (const app::ValueLine& l : lines) {
            CHECK(l.path.rfind("partData[0].", 0) == 0);
            logo |= l.path == "partData[0].PartDetails[0].Logo";
        }
        CHECK(logo);
        break;
    }
    // A plain value is one line: its path and the text the inspector shows.
    const Found f = FirstAttribute([](const Collection&, const Attribute&, const app::ValueType& vt) { return vt.kind == TypeKind::Float; });
    const std::vector<app::ValueLine> one = app::ValueLines(db, f.a->data, 0, f.vt, EndianOf(*f.c), "x");
    CHECK_EQ(one.size(), size_t(1));
    if (!one.empty()) {
        CHECK_EQ(one[0].path, std::string("x"));
        CHECK_EQ(one[0].value, app::FormatValue(db, f.a->data, 0, f.vt, EndianOf(*f.c)));
    }
}

// -------------------------------------------------------------- fields ---

TEST("search: fields by name") {
    const uint32_t field = StringHash32("TrackLayout");
    SearchQuery q;
    q.mode = SearchMode::Fields;
    q.text = "TrackLayout";
    q.wholeValue = true;
    SearchResult r = app::RunSearch(Full(), q);
    CHECK(r.error.empty());
    CHECK(std::find(r.fields.begin(), r.fields.end(), std::make_pair(kGameplay, field)) != r.fields.end());
    for (const auto& f : r.fields) CHECK_EQ(f.second, field);
    const SearchHit* h = Find(r, kGameplay, StringHash32(kRace), field, "TrackLayout");
    CHECK(h);
    CHECK_EQ(h->value, std::string(kLayout));
    for (const SearchHit& x : r.hits) CHECK_EQ(x.field, field);

    // Part of the name in any case; arrays are summarized.
    q.text = "children";
    q.wholeValue = false;
    r = app::RunSearch(Full(), q);
    const Collection& folder = Gp("tracks/autobahn_gd");
    const uint32_t children = StringHash32("Children");
    h = Find(r, kGameplay, folder.key, children, "Children");
    CHECK(h);
    const uint32_t n = folder.find(children)->count;
    CHECK_EQ(h->value, std::to_string(n) + (n == 1 ? " element" : " elements"));

    // Limited to one class.
    q.inClass = kGameplay;
    r = app::RunSearch(Full(), q);
    CHECK(!r.hits.empty());
    for (const auto& f : r.fields) CHECK_EQ(f.first, kGameplay);
    for (const SearchHit& x : r.hits) CHECK_EQ(x.classKey, kGameplay);
}

// ------------------------------------------------------------- targets ---

TEST("search: reference targets parse from names and hashes") {
    const Database& db = Full();
    uint32_t cls = 1, key = 1;
    CHECK(app::ParseReferenceTarget(db, std::string("gameplay/") + kLayout, cls, key));
    CHECK_EQ(cls, kGameplay);
    CHECK_EQ(key, StringHash32(kLayout));
    CHECK(app::ParseReferenceTarget(db, "gameplay", cls, key));
    CHECK_EQ(cls, kGameplay);
    CHECK_EQ(key, 0u);
    CHECK(app::ParseReferenceTarget(db, "  " + app::ToLower(Hex(kGameplay)) + "/" + Hex(StringHash32("circuit")) + " ", cls, key));
    CHECK_EQ(cls, kGameplay);
    CHECK_EQ(key, StringHash32("circuit"));

    cls = key = 7;
    CHECK(!app::ParseReferenceTarget(db, "gameplay/no/such/element", cls, key));
    CHECK(!app::ParseReferenceTarget(db, "no_such_class", cls, key));
    CHECK(!app::ParseReferenceTarget(db, "no_such_class/circuit", cls, key));
    CHECK(!app::ParseReferenceTarget(db, "gameplay/", cls, key));
    CHECK(!app::ParseReferenceTarget(db, "", cls, key));
    CHECK_EQ(cls, 7u); // untouched on failure
    CHECK_EQ(key, 7u);
}
