// Copying collections through clipboard text (core/Clip.h), into the same
// database and into another one, and moving collections (CollectionEditor::
// move / moveToVault). Every edit is written out and read back.

#include "Test.h"

#include "core/Clip.h"
#include "core/Edit.h"
#include "core/Hash.h"
#include "core/Writer.h"

#include <algorithm>
#include <functional>

using namespace attrib;

namespace {
std::string Vanilla(const char* file) { return test::DataPath(std::string("attrib/databases/pc/prostreet-vanilla/") + file); }
std::string Vlted(const char* file) { return test::DataPath(std::string("attrib/databases/pc/prostreet-vlted/") + file); }

std::unique_ptr<Database> Load(const std::string& attributes) {
    auto db = std::make_unique<Database>();
    db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    db->loadPack(attributes);
    return db;
}

// Writes the pack and loads it alone into a fresh database.
std::unique_ptr<Database> Reload(Database& db, const LoadedPack& pack) {
    const Bytes bytes = WritePack(db, pack);
    auto fresh = std::make_unique<Database>();
    fresh->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    fresh->loadPack(pack.path, bytes);
    return fresh;
}

bool SameBlock(const DataBlock& a, const DataBlock& b) {
    if (a.bytes != b.bytes || a.ptrs.size() != b.ptrs.size()) return false;
    auto byOffset = [](const DataBlock& d) {
        std::vector<const PtrSlot*> v;
        for (const PtrSlot& p : d.ptrs) v.push_back(&p);
        std::sort(v.begin(), v.end(), [](const PtrSlot* x, const PtrSlot* y) { return x->offset < y->offset; });
        return v;
    };
    const auto pa = byOffset(a), pb = byOffset(b);
    for (size_t i = 0; i < pa.size(); ++i) {
        const PtrSlot& x = *pa[i];
        const PtrSlot& y = *pb[i];
        if (x.offset != y.offset || x.isString != y.isString || x.text != y.text || bool(x.data) != bool(y.data)) return false;
        if (x.data && !SameBlock(*x.data, *y.data)) return false;
    }
    return true;
}

// The collections set the same own values.
bool SameValues(const Collection& a, const Collection& b) {
    if (a.attributes.size() != b.attributes.size()) return false;
    for (const Attribute& x : a.attributes) {
        const Attribute* y = b.find(x.key);
        if (!y || y->count != x.count || y->elemSize != x.elemSize || y->inLayout != x.inLayout || y->capacity != x.capacity ||
            !SameBlock(x.data, y->data))
            return false;
    }
    return true;
}

size_t Pointers(const Collection& c) {
    size_t n = 0;
    for (const Attribute& a : c.attributes) n += a.data.ptrs.size();
    return n;
}

int ExportIndex(const Vault& v, const Collection* c) {
    for (size_t i = 0; i < v.exportOrder.size(); ++i)
        if (v.exportOrder[i].collection == c) return int(i);
    return -1;
}

Clip RoundTrip(const Clip& c) {
    const std::optional<Clip> back = DecodeClip(EncodeClip(c));
    CHECK(back);
    return *back;
}

size_t SubtreeSize(const Collection& c) {
    size_t n = 1;
    for (const Collection* k : c.children) n += SubtreeSize(*k);
    return n;
}
} // namespace

TEST("clip: a copy pasted next to its original gets a copy name and the same values, also after saving") {
    auto db = Load(Vanilla("ATTRIBUTES.BIN"));
    // A collection with a parent, no children and strings or nested data.
    Collection* src = nullptr;
    for (const auto& cls : db->classes())
        for (Collection* c : cls->collections)
            if (!src && c->parent && c->children.empty() && c->vault == c->parent->vault && Pointers(*c) >= 2 &&
                db->names().find(c->key))
                src = c;
    CHECK(src);
    const std::string name = db->names().format(src->key);
    const Clip clip = RoundTrip(CopyCollections(*db, {src}, false));
    CHECK_EQ(clip.collections.size(), size_t(1));
    CHECK_EQ(clip.collections[0].name, name);

    PasteResult r = PasteClip(*db, clip, *src->cls, src->parent, *src->vault);
    CHECK(r.warnings.empty());
    CHECK_EQ(r.created.size(), size_t(1));
    Collection* copy = r.created[0];
    CHECK_EQ(db->names().format(copy->key), name + "_copy");
    CHECK(copy->parent == src->parent);
    CHECK(copy->vault == src->vault);
    CHECK(SameValues(*src, *copy));
    // Its own data, not the original's.
    CHECK(copy->attributes.front().data.bytes.data() != src->attributes.front().data.bytes.data());
    // A second paste counts up.
    PasteResult r2 = PasteClip(*db, clip, *src->cls, src->parent, *src->vault);
    CHECK_EQ(db->names().format(r2.created[0]->key), name + "_copy2");

    auto fresh = Reload(*db, *src->vault->pack);
    for (const auto& v : fresh->packs().back()->vaults) CHECK(v->warnings.empty());
    Collection* a = fresh->findCollection(src->classKey, src->key);
    Collection* b = fresh->findCollection(src->classKey, StringHash32(name + "_copy"));
    CHECK(a && b);
    CHECK(b->parent && b->parent->key == src->parent->key);
    CHECK(SameValues(*a, *b));
}

TEST("clip: with children, the copied tree keeps its shape") {
    auto db = Load(Vanilla("ATTRIBUTES.BIN"));
    Collection* src = nullptr;
    for (const auto& cls : db->classes())
        for (Collection* c : cls->collections)
            if (!src && !c->children.empty() && SubtreeSize(*c) <= 30 && SubtreeSize(*c) > c->children.size() + 1 &&
                db->names().find(c->key))
                src = c;
    if (!src) // no grandchildren anywhere small enough: any small tree
        for (const auto& cls : db->classes())
            for (Collection* c : cls->collections)
                if (!src && c->children.size() >= 2 && SubtreeSize(*c) <= 30) src = c;
    CHECK(src);
    const size_t total = SubtreeSize(*src);
    const Clip clip = RoundTrip(CopyCollections(*db, {src}, true));
    CHECK_EQ(clip.collections.size(), total);
    PasteResult r = PasteClip(*db, clip, *src->cls, src->parent, *src->vault);
    CHECK_EQ(r.created.size(), total);
    Collection* copy = r.created[0];
    CHECK_EQ(SubtreeSize(*copy), total);
    CHECK_EQ(copy->children.size(), src->children.size());
    // Each copied child is a copy of one of the original's children.
    for (Collection* k : copy->children) {
        const std::string kn = db->names().format(k->key);
        CHECK(kn.size() > 5 && kn.compare(kn.size() - 5, 5, "_copy") == 0);
        Collection* orig = db->findCollection(k->classKey, StringHash32(kn.substr(0, kn.size() - 5)));
        CHECK(orig && orig->parent == src);
        CHECK(SameValues(*orig, *k));
    }
    // Copying a root that is under another root copies it once, below it.
    const Clip both = CopyCollections(*db, {src->children.front(), src}, true);
    CHECK_EQ(both.collections.size(), total);
    CHECK_EQ(both.collections[0].key, src->key);

    auto fresh = Reload(*db, *src->vault->pack);
    Collection* again = fresh->findCollection(copy->classKey, copy->key);
    CHECK(again);
    CHECK_EQ(SubtreeSize(*again), total);
}

TEST("clip: pastes into another database (vanilla to vlted), keeping free names and bare keys") {
    auto a = Load(Vanilla("ATTRIBUTES.BIN"));
    auto b = Load(Vlted("ATTRIBUTES.BIN"));
    Collection* src = nullptr;
    for (const auto& cls : a->classes())
        for (Collection* c : cls->collections)
            if (!src && c->parent && c->children.empty() && Pointers(*c) >= 1 && a->names().find(c->key) &&
                b->findCollection(c->classKey, c->parentKey))
                src = c;
    CHECK(src);
    Clip clip = RoundTrip(CopyCollections(*a, {src}, false));
    ClassDef* cls = b->findClass(src->classKey);
    Collection* parent = b->findCollection(src->classKey, src->parentKey);
    CHECK(cls && parent);

    // The name is taken there too: a copy name, the values from vanilla.
    PasteResult r = PasteClip(*b, clip, *cls, parent, *parent->vault);
    CHECK(r.warnings.empty());
    CHECK_EQ(b->names().format(r.created[0]->key), a->names().format(src->key) + "_copy");
    CHECK(SameValues(*src, *r.created[0]));

    // A free name is kept, and so is a bare key (with the original's export id).
    clip.collections[0].name = "attribulina_clip_free";
    clip.collections[0].key = StringHash32("attribulina_clip_free");
    Collection* free = PasteClip(*b, clip, *cls, parent, *parent->vault).created[0];
    CHECK_EQ(free->key, StringHash32("attribulina_clip_free"));
    CHECK_EQ(free->exportId, CollectionExportId(b->names(), cls->key, "attribulina_clip_free"));
    clip.collections[0].name.clear();
    clip.collections[0].key = 0x0BADF00Du;
    clip.collections[0].exportId = 0x12345678u;
    Collection* bare = PasteClip(*b, clip, *cls, parent, *parent->vault).created[0];
    CHECK_EQ(bare->key, 0x0BADF00Du);
    CHECK_EQ(bare->exportId, 0x12345678u);

    // Values of the other byte order aren't pasted.
    Clip big = clip;
    big.endian = Endian::Big;
    bool threw = false;
    try {
        PasteClip(*b, big, *cls, parent, *parent->vault);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    auto fresh = Reload(*b, *parent->vault->pack);
    Collection* kept = fresh->findCollection(cls->key, StringHash32("attribulina_clip_free"));
    CHECK(kept && kept->parent && kept->parent->key == parent->key);
    Collection* orig = a->findCollection(src->classKey, src->key);
    CHECK(SameValues(*orig, *kept));
    CHECK(fresh->findCollection(cls->key, 0x0BADF00Du));
}

TEST("clip: moving a collection under a later sibling keeps parents first in the vault") {
    auto db = Load(Vanilla("ATTRIBUTES.BIN"));
    // Two leaf siblings in one vault, the first exported before the second.
    Collection *x = nullptr, *y = nullptr;
    for (const auto& cls : db->classes())
        for (Collection* p : cls->collections) {
            if (x || p->children.size() < 2) continue;
            for (Collection* c1 : p->children)
                for (Collection* c2 : p->children)
                    if (!x && c1 != c2 && c1->vault == c2->vault && c1->children.empty() && c2->children.empty() &&
                        ExportIndex(*c1->vault, c1) < ExportIndex(*c1->vault, c2)) {
                        x = c1;
                        y = c2;
                    }
        }
    CHECK(x && y);
    CollectionEditor ed(*db);
    CHECK(ed.move(*x, y));
    CHECK(x->parent == y);
    CHECK_EQ(x->parentKey, y->key);
    CHECK(std::find(y->children.begin(), y->children.end(), x) != y->children.end());
    CHECK(ExportIndex(*x->vault, y) < ExportIndex(*x->vault, x));
    // A collection can't go under itself or under its own child.
    CHECK(!ed.move(*y, x));
    CHECK(!ed.move(*x, x));

    auto fresh = Reload(*db, *x->vault->pack);
    Collection* fx = fresh->findCollection(x->classKey, x->key);
    CHECK(fx && fx->parent && fx->parent->key == y->key);

    // Back to the root, and into another vault of the same pack.
    CHECK(ed.move(*x, nullptr));
    CHECK(!x->parent && x->parentKey == 0);
    Vault* other = &CreateVault(*x->vault->pack, "attribulina_move_test", *x->vault);
    Vault* before = x->vault;
    ed.moveToVault(*x, *other);
    CHECK(x->vault == other);
    CHECK_EQ(ExportIndex(*before, x), -1);
    CHECK(ExportIndex(*other, x) >= 0);
    auto moved = Reload(*db, *other->pack);
    Collection* mx = moved->findCollection(x->classKey, x->key);
    CHECK(mx && mx->vault->name == other->name && !mx->parent);
}

TEST("clip: copy names, damaged text, and what a pack holds") {
    auto db = Load(Vanilla("ATTRIBUTES.BIN"));
    Collection* c = nullptr;
    for (const auto& cls : db->classes())
        for (Collection* k : cls->collections)
            if (!c && db->names().find(k->key)) c = k;
    CHECK(c);
    const std::string n = db->names().format(c->key);
    CHECK_EQ(CopyName(*db, c->classKey, n), n + "_copy");
    CollectionEditor(*db).create(*c->vault, *c->cls, n + "_copy", nullptr);
    CHECK_EQ(CopyName(*db, c->classKey, n), n + "_copy2");
    CHECK_EQ(CopyName(*db, c->classKey, n + "_copy"), n + "_copy2");
    CHECK_EQ(CopyName(*db, c->classKey, n + "_copy7"), n + "_copy7");

    CHECK(!DecodeClip(""));
    CHECK(!DecodeClip("pvehicle/default"));
    CHECK(!DecodeClip("Attribulina collections 1\nnot base64 at all!"));
    CHECK(!DecodeClip("Attribulina collections 2\nQUNMUA=="));
    // Cut short or with bytes changed, it never crashes, and a cut is never
    // taken for a whole clip.
    const std::string text = EncodeClip(CopyCollections(*db, {c}, false));
    CHECK(DecodeClip(text));
    CHECK(DecodeClip(std::string(text.begin(), text.end() - 1))); // without the final newline
    for (size_t len = 0; len + 8 < text.size(); len += 7) {
        const std::optional<Clip> cut = DecodeClip(text.substr(0, len));
        CHECK(!cut);
    }
    for (size_t at = 30; at < text.size(); at += 11) {
        std::string t = text;
        t[at] = t[at] == 'A' ? 'B' : 'A';
        (void)DecodeClip(t);
    }

    const PackSummary attrs = DescribePack(ReadFile(Vanilla("ATTRIBUTES.BIN")));
    CHECK(attrs.hasDatabase);
    CHECK_EQ(attrs.vaults.size(), db->packs().front()->vaults.size());
    const PackSummary gp = DescribePack(ReadFile(Vanilla("gameplay.lzc")));
    CHECK(!gp.hasDatabase);
    CHECK(gp.vaults.size() > 100);
}
