// Vaults made, renamed and deleted (core/Edit.h), World's event vault files
// made and dropped, and the script statements and diffs that do the same.

#include "Test.h"

#include "core/Edit.h"
#include "core/Folder.h"
#include "core/Game.h"
#include "core/Gameplay.h"
#include "core/Hash.h"
#include "core/Pack.h"
#include "core/Schema.h"
#include "core/Script.h"
#include "core/Writer.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>

using namespace attrib;
namespace sc = attrib::script;

namespace {

std::string PS(const char* file) { return test::DataPath(std::string("attrib/databases/pc/prostreet-vanilla/") + file); }
std::string WorldDir() { return test::DataPath("attrib/databases/pc/world-final"); }

void LoadProStreet(Database& db) {
    db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
    db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    db.loadPack(PS("ATTRIBUTES.BIN"));
    db.loadPack(PS("gameplay.lzc"));
}

void LoadWorld(Database& db, const std::vector<std::string>& events) {
    db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/world_pc.txt")));
    db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    db.loadPack(WorldDir() + "/attributes.bin");
    for (const std::string& e : events) db.loadPack(WorldDir() + "/gc.vaults/" + e);
}

LoadedPack& Pack(Database& db, const char* file) {
    for (const auto& p : db.packs())
        if (std::filesystem::path(p->path).filename() == file) return *p;
    throw test::Failure{std::string("no pack ") + file};
}

std::string Messages(const sc::ApplyReport& r) {
    std::string s;
    for (const sc::Message& m : r.messages) s += std::to_string(m.pos.line) + ": " + m.text + "\n";
    return s;
}

size_t Count(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

} // namespace

TEST("vaults: a name is refused when empty, unfit for a file name, too long for an event vault, or another vault's in any case") {
    if (!std::filesystem::exists(PS("gameplay.lzc"))) return;
    Database db;
    LoadProStreet(db);
    LoadedPack& gp = Pack(db, "gameplay.lzc");
    Vault* gpcore = FindVault(gp, "gpcore");
    CHECK(gpcore);
    for (const std::string& bad : std::vector<std::string>{"", " x", "x ", "a/b", "a\\b", "a:b", "a*b", "a\"b", "a\x01"}) CHECK(!VaultNameProblem(db, gp, bad).empty());
    CHECK(!VaultNameProblem(db, gp, "GPCORE").empty());
    CHECK(!VaultNameProblem(db, gp, "db").empty()); // attributes.bin's: names are the database's
    CHECK(VaultNameProblem(db, gp, "my vault_2").empty());
    CHECK(VaultNameProblem(db, gp, "GPCore", gpcore).empty()); // its own name, in another case
    CHECK(FindVault(db, "db") && FindVault(db, "db")->isDatabase);
    LoadedPack eventFile;
    eventFile.format = PackFormat::VaultFile;
    CHECK(!VaultNameProblem(db, eventFile, std::string(0x2C, 'a')).empty());
    CHECK(VaultNameProblem(db, eventFile, std::string(0x2B, 'a')).empty());
}

TEST("vaults: a renamed vault keeps its collections; its dependencies and the gameplayvault naming it follow, and renaming it back writes the pack as read") {
    if (!std::filesystem::exists(PS("gameplay.lzc"))) return;
    Database db;
    LoadProStreet(db);
    LoadedPack& gp = Pack(db, "gameplay.lzc");
    const Bytes before = WritePack(db, gp);
    Vault* v = FindVault(gp, "autobahn_gd");
    CHECK(v && !v->collections.empty());
    if (!v) return;
    const size_t held = v->collections.size();
    const Collection* track = db.findCollection(StringHash32("gameplay"), StringHash32("tracks/autobahn_gd"));
    CHECK(track);

    const std::vector<Collection*> named = RenameVault(db, *v, "attribulina_autobahn");
    CHECK(named.size() == 1 && named[0] == track);
    CHECK_EQ(GameplayModel(db).childrenVaultName(track), std::string("attribulina_autobahn"));
    CHECK(v->name == "attribulina_autobahn" && v->raw.name == v->name && !FindVault(gp, "autobahn_gd"));
    CHECK(v->raw.dependencies.size() == 2 && v->raw.dependencies[0].name == "attribulina_autobahn.vlt" &&
          v->raw.dependencies[0].id == StringHash32("attribulina_autobahn.vlt") && v->raw.dependencies[1].name == "attribulina_autobahn.bin");
    CHECK(std::is_sorted(gp.vaults.begin(), gp.vaults.end(), [](const auto& a, const auto& b) { return a->name < b->name; }));
    // The database's vault and event vaults keep their names; taken names are refused.
    CHECK(FindVault(db, "db"));
    bool threw = false;
    try { RenameVault(db, *FindVault(db, "db"), "attribulina_db"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { RenameVault(db, *v, "GPCORE"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && v->name == "attribulina_autobahn");

    // Written and read again.
    const Bytes renamed = WritePack(db, gp);
    Database again;
    again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
    again.loadPack(PS("ATTRIBUTES.BIN"));
    LoadedPack& back = again.loadPack(gp.path, renamed);
    const Vault* rv = FindVault(back, "attribulina_autobahn");
    CHECK(rv && rv->warnings.empty() && rv->collections.size() == held && !FindVault(back, "autobahn_gd"));
    CHECK_EQ(GameplayModel(again).childrenVaultName(again.findCollection(StringHash32("gameplay"), StringHash32("tracks/autobahn_gd"))),
             std::string("attribulina_autobahn"));

    RenameVault(db, *v, "autobahn_gd");
    CHECK(WritePack(db, gp) == before);
}

TEST("vaults: deleting a vault with what it holds takes its collections and those under them, each child first") {
    if (!std::filesystem::exists(PS("gameplay.lzc"))) return;
    Database db;
    LoadProStreet(db);
    LoadedPack& gp = Pack(db, "gameplay.lzc");
    Vault* v = FindVault(gp, "autobahn_gd");
    CHECK(v);
    if (!v) return;
    // A collection in another vault made from one of its collections goes too.
    Collection& base = *v->collections.front();
    Collection& made = CollectionEditor(db).create(*FindVault(gp, "gpcore"), *base.cls, "attribulina_made_from", &base);
    const std::vector<Collection*> all = VaultContents(*v);
    CHECK(all.size() == v->collections.size() + 1);
    CHECK(std::find(all.begin(), all.end(), &made) < std::find(all.begin(), all.end(), &base));
    std::unordered_map<const Collection*, size_t> at;
    for (size_t i = 0; i < all.size(); ++i) at[all[i]] = i;
    CHECK_EQ(at.size(), all.size());
    for (const Collection* c : v->collections) CHECK(at.count(c));
    for (const Collection* c : all)
        for (const Collection* ch : c->children) CHECK(at.count(ch) && at[ch] < at[c]);
    CHECK(!RemoveVault(gp, *v));
    std::vector<std::pair<uint32_t, uint32_t>> gone;
    CollectionEditor ed(db);
    for (Collection* c : all) {
        gone.emplace_back(c->classKey, c->key);
        CHECK(ed.remove(*c));
    }
    CHECK(RemoveVault(gp, *v));
    Database again;
    again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
    again.loadPack(PS("ATTRIBUTES.BIN"));
    LoadedPack& back = again.loadPack(gp.path, WritePack(db, gp));
    CHECK(!FindVault(back, "autobahn_gd"));
    for (const auto& [cls, key] : gone) CHECK(!again.findCollection(cls, key));
    for (const auto& w : back.vaults) CHECK(w->warnings.empty());
}

// A mod's Most Wanted gameplay.bin held twelve vaults of one collection each
// (JVpursuit, 100 bytes at DatN's 0x58), and loading it threw
// "CollectionLoadData export is too small": read as 16-byte entries, the one
// 20-byte export entry gave size 0 at offset 100, which lies in DatN.
TEST("vaults: a Most Wanted vault holding one collection larger than DatN's offset reads back") {
    const std::string dir = test::DataPath("attrib/databases/pc/mw-vanilla");
    if (!std::filesystem::exists(dir + "/gameplay.bin")) return;
    Database db;
    db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/mw_pc.txt")));
    db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    db.loadPack(dir + "/attributes.bin");
    LoadedPack& gp = db.loadPack(dir + "/gameplay.bin");
    Collection* lone = nullptr;
    for (const auto& v : gp.vaults)
        for (Collection* c : v->collections)
            if (!lone && c->children.empty() && c->loadedEntryKeys.size() >= 4) lone = c;
    CHECK(lone);
    if (!lone) return;
    const uint32_t cls = lone->classKey, key = lone->key;
    Vault& nv = CreateVault(gp, "attribulina_pursuit", *gp.vaults.front());
    CollectionEditor(db).moveToVault(*lone, nv);
    CHECK_EQ(nv.collections.size(), size_t(1));

    Database again;
    again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/mw_pc.txt")));
    again.loadPack(dir + "/attributes.bin");
    LoadedPack& back = again.loadPack(gp.path, WritePack(db, gp));
    const Vault* rv = FindVault(back, "attribulina_pursuit");
    CHECK(rv && rv->warnings.empty() && rv->raw.mostWantedOwn() && rv->collections.size() == 1);
    if (!rv) return;
    // The case itself: the export's size reaches past DatN's offset.
    CHECK(rv->raw.exports.size() == 1 && rv->raw.exports[0].size >= rv->raw.datOffset);
    const Collection* read = again.findCollection(cls, key);
    CHECK(read && read->vault == rv && read->attributes.size() == lone->attributes.size());
}

TEST("vaults: a vault made in World's packs has its EndC chunks, and a new event vault file writes, reads back and drops again") {
    if (!std::filesystem::exists(WorldDir())) return;
    Database db;
    LoadWorld(db, {"1_2_1_sprint.bin"});
    LoadedPack& attrs = Pack(db, "attributes.bin");
    Vault& nv = CreateVault(attrs, "attribulina_vault", *attrs.vaults.front());
    CHECK(nv.raw.endC && nv.raw.strNFirst);
    {
        Database again;
        again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/world_pc.txt")));
        LoadedPack& back = again.loadPack(attrs.path, WritePack(db, attrs));
        const Vault* rv = FindVault(back, "attribulina_vault");
        CHECK(rv && rv->raw.endC && rv->warnings.empty());
    }

    const Vault* model = EventVaultModel(db);
    CHECK(model && model->pack->format == PackFormat::VaultFile);
    const std::string path = NewEventVaultPath(db, "attribulina_event");
    CHECK(std::filesystem::path(path).filename() == "attribulina_event.bin" &&
          std::filesystem::path(path).parent_path() == std::filesystem::path(WorldDir() + "/gc.vaults"));
    const size_t packs = db.packs().size();
    LoadedPack& ev = CreateEventVaultFile(db, path, "attribulina_event", *model);
    CHECK(ev.dirty && ev.format == PackFormat::VaultFile && ev.vaults.size() == 1 && db.packs().size() == packs + 1);
    bool threw = false;
    try { CreateVault(ev, "second", *model); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && ev.vaults.size() == 1);
    threw = false;
    try { CreateEventVaultFile(db, path, "attribulina_event2", *model); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    Vault& v = *ev.vaults.front();
    ClassDef* gameplay = db.findClass(StringHash32("gameplay"));
    CHECK(gameplay);
    if (!gameplay) return;
    Collection& made = CollectionEditor(db).create(v, *gameplay, "attribulina_event_race", nullptr);
    CHECK(v.exportOrder.back().kind == ExportRef::Kind::Data);

    const Bytes written = WritePack(db, ev);
    const std::optional<VaultFileHeader> h = ParseVaultFileHeader(written);
    CHECK(h && h->name == "attribulina_event" && h->binOffset == 0x40 && h->fileSize == written.size());
    Database again;
    LoadWorld(again, {});
    LoadedPack& back = again.loadPack(path, written);
    CHECK(back.format == PackFormat::VaultFile && back.vaults.size() == 1);
    const Vault& rv = *back.vaults.front();
    CHECK(rv.name == "attribulina_event" && rv.raw.endC && rv.raw.strNFirst && rv.raw.sign.empty() && rv.warnings.empty());
    CHECK(rv.raw.dependencies.size() == 2 && rv.raw.dependencies[1].name == "attribulina_event.bin");
    CHECK(rv.dataExports.size() == 1 && rv.dataExports[0].id == StringHash32("VaultData") &&
          rv.dataExports[0].type == ExportType::VaultData && rv.dataExports[0].bytes == Bytes(4, 0));
    CHECK(rv.exportOrder.back().kind == ExportRef::Kind::Data);
    const Collection* race = again.findCollection(made.classKey, made.key);
    CHECK(race && race->vault == &rv);
    CHECK(WritePack(again, back) == written);

    // An event vault file goes once its vault is empty.
    CHECK(!db.removePack(ev));
    CHECK(CollectionEditor(db).remove(made));
    CHECK(db.removePack(ev));
    CHECK_EQ(db.packs().size(), packs);
}

TEST("vaults: scripts rename a vault, and a diff writes the rename as one statement that gives the same packs") {
    if (!std::filesystem::exists(PS("gameplay.lzc"))) return;
    Database db;
    LoadProStreet(db);
    auto base = sc::CloneDatabase(db);
    const sc::ApplyReport r = sc::Apply(db, sc::Parse("attribulina 1\nrename vault autobahn_gd to \"attribulina autobahn\"\n", "test.amod"));
    if (!r.ok) std::fprintf(stderr, "%s", Messages(r).c_str());
    CHECK(r.ok && r.touched.size() == 1);
    CHECK(FindVault(db, "attribulina autobahn") && !FindVault(db, "autobahn_gd"));
    // Again: it is renamed already.
    CHECK(sc::Apply(db, sc::Parse("attribulina 1\nrename vault autobahn_gd to \"attribulina autobahn\"\n", "test.amod")).ok);
    CHECK(!sc::Apply(db, sc::Parse("attribulina 1\nrename vault no_such_vault to x\n", "test.amod")).ok);
    CHECK(!sc::Apply(db, sc::Parse("attribulina 1\nrename vault gpcore to db\n", "test.amod")).ok);

    const std::string text = sc::Print(sc::Diff(*base, db));
    CHECK(text.find("rename vault autobahn_gd to \"attribulina autobahn\"\n") != std::string::npos);
    CHECK_EQ(Count(text, "create vault"), size_t(0));
    CHECK_EQ(Count(text, "delete vault"), size_t(0));
    CHECK_EQ(Count(text, "to vault"), size_t(0)); // no moves
    const sc::ApplyReport d = sc::Apply(*base, sc::Parse(text, "diff.amod"));
    if (!d.ok) std::fprintf(stderr, "%s", Messages(d).c_str());
    CHECK(d.ok);
    for (size_t i = 0; i < db.packs().size(); ++i) CHECK(WritePack(*base, *base->packs()[i]) == WritePack(db, *db.packs()[i]));
}

TEST("vaults: in World, scripts make and delete event vault files, and a diff between databases with gc.vaults carries them") {
    if (!std::filesystem::exists(WorldDir())) return;
    Database db;
    db.setTarget(Target{ Game::World, Platform::PC });
    LoadWorld(db, {"1_2_1_sprint.bin", "gpcore.bin"});
    auto base = sc::CloneDatabase(db);
    const std::string make = "attribulina 1\n"
                             "create vault attribulina_event in attribulina_event.bin\n"
                             "collection gameplay/attribulina_event_race in vault attribulina_event { }\n";
    const sc::ApplyReport r = sc::Apply(db, sc::Parse(make, "test.amod"));
    if (!r.ok) std::fprintf(stderr, "%s", Messages(r).c_str());
    CHECK(r.ok);
    const Vault* v = FindVault(db, "attribulina_event");
    CHECK(v && v->pack->format == PackFormat::VaultFile && v->collections.size() == 1 && r.touched.count(v->pack));
    if (!v) return;
    CHECK(std::filesystem::path(v->pack->path).parent_path() == std::filesystem::path(WorldDir() + "/gc.vaults"));
    // Only World's event vaults are files of their own; the file must be a .bin.
    CHECK(!sc::Apply(db, sc::Parse("attribulina 1\ncreate vault x in x.txt\n", "test.amod")).ok);

    const std::string text = sc::Print(sc::Diff(*base, db));
    CHECK(text.find("create vault attribulina_event in attribulina_event.bin\n") != std::string::npos);
    CHECK(text.find("gameplay/attribulina_event_race") != std::string::npos);
    auto replay = sc::CloneDatabase(*base);
    const sc::ApplyReport d = sc::Apply(*replay, sc::Parse(text, "diff.amod"));
    if (!d.ok) std::fprintf(stderr, "%s", Messages(d).c_str());
    CHECK(d.ok && replay->packs().size() == db.packs().size());
    for (size_t i = 0; i < db.packs().size(); ++i) CHECK(WritePack(*replay, *replay->packs()[i]) == WritePack(db, *db.packs()[i]));

    // Deleted again: the collection, then the vault, whose file goes.
    const sc::ApplyReport del = sc::Apply(db, sc::Parse("attribulina 1\ndelete gameplay/attribulina_event_race\ndelete vault attribulina_event\n", "test.amod"));
    if (!del.ok) std::fprintf(stderr, "%s", Messages(del).c_str());
    CHECK(del.ok && del.removedFiles.size() == 1 && !FindVault(db, "attribulina_event") && db.packs().size() == base->packs().size());
    for (const LoadedPack* p : del.touched) CHECK(std::any_of(db.packs().begin(), db.packs().end(), [&](const auto& q) { return q.get() == p; }));
    const std::string gone = sc::Print(sc::Diff(*replay, db));
    CHECK(gone.find("delete vault attribulina_event\n") != std::string::npos);
    const sc::ApplyReport back = sc::Apply(*replay, sc::Parse(gone, "diff.amod"));
    CHECK(back.ok && back.removedFiles.size() == 1 && replay->packs().size() == db.packs().size());
}

TEST("vaults: a vault made in each pack of every game folder, with a collection in it, writes and reads back") {
    namespace fs = std::filesystem;
    const fs::path root = test::DataPath("attrib/databases");
    if (!fs::exists(root)) return;
    std::vector<std::shared_ptr<const TypeSchema>> schemas;
    for (const auto& e : fs::directory_iterator(test::DataPath("data/schema")))
        if (e.path().extension() == ".txt") schemas.push_back(TypeSchema::Load(e.path().string()));
    size_t tried = 0;
    std::vector<fs::path> folders;
    for (const auto& platform : fs::directory_iterator(root))
        if (platform.is_directory())
            for (const auto& f : fs::directory_iterator(platform.path()))
                // Not the -vlted re-saves: the user's edits in prostreet-vlted make two of
                // its packs write shorter the second time, which is expected. Nor Pro Street's OGVI save, whose
                // empty camera lists VaultLib points at the block after them (not repaired, the user's call:
                // VaultLib.h). Nor a folder of builds (pc/world-various: some 9800 packs of the final build's
                // format, which test_writer covers).
                if (f.is_directory() && f.path().filename().string().find("-vlted") == std::string::npos && f.path().filename() != "prostreet-ogvi" &&
                    FindFileNoCase(f.path().string(), "attributes.bin"))
                    folders.push_back(f.path());
    std::sort(folders.begin(), folders.end());
    for (const fs::path& folder : folders) {
        std::vector<std::string> files;
        for (const FolderFile& f : FolderPacks(folder.string()))
            if (f.skipped.empty()) files.push_back(f.path);
        auto load = [&](Database& db) {
            for (const auto& s : schemas) db.addTypeSchema(s);
            db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
        };
        Database db;
        load(db);
        for (const std::string& f : files) db.loadPack(f);
        ClassDef* cls = db.findClass(StringHash32("gameplay"));
        for (size_t i = 0; !cls && i < db.classes().size(); ++i)
            if (!db.classes()[i]->collections.empty()) cls = db.classes()[i].get();
        CHECK(cls);
        if (!cls) continue;
        for (size_t i = 0; i < db.packs().size(); ++i) {
            LoadedPack& pk = *db.packs()[i];
            if (pk.format != PackFormat::Vpak || pk.vaults.empty()) continue;
            const std::string name = "attribulina_new_vault_" + std::to_string(i);
            CHECK(VaultNameProblem(db, pk, name).empty());
            Vault& v = CreateVault(pk, name, *pk.vaults.front());
            const std::string colName = "attribulina_new_collection_" + std::to_string(i);
            CollectionEditor(db).create(v, *cls, colName, nullptr);
            const size_t vaults = pk.vaults.size();
            const Bytes written = WritePack(db, pk);
            // Read back beside the pack that defines the classes.
            Database again;
            load(again);
            again.names().add(colName);
            if (i != 0) again.loadPack(files.front());
            LoadedPack& back = again.loadPack(pk.path, written);
            const Vault* rv = FindVault(back, name);
            const Collection* rc = again.findCollection(cls->key, StringHash32(colName));
            if (!rv || !rv->warnings.empty() || !rc || rc->vault != rv)
                std::printf("        %s %s: vault %s\n", folder.string().c_str(), fs::path(pk.path).filename().string().c_str(),
                            !rv ? "missing" : !rv->warnings.empty() ? rv->warnings.front().c_str() : "lacks its collection");
            CHECK(rv && rv->warnings.empty() && rv->exactlyWritable && rv->raw.endian == v.raw.endian && rv->raw.endC == v.raw.endC);
            CHECK(rc && rc->vault == rv);
            CHECK_EQ(back.vaults.size(), vaults);
            const Bytes rewritten = WritePack(again, back);
            if (rewritten != written) {
                size_t at = 0;
                while (at < rewritten.size() && at < written.size() && rewritten[at] == written[at]) ++at;
                std::printf("        %s %s: written again, differs at 0x%zX (sizes %zu, %zu)\n", folder.string().c_str(),
                            fs::path(pk.path).filename().string().c_str(), at, written.size(), rewritten.size());
            }
            CHECK(rewritten == written);
            ++tried;
        }
    }
    CHECK(tried > 60);
}

TEST("packs: unloading one takes its collections out; what other packs inherit from them falls back, and their files write as read") {
    if (!std::filesystem::exists(WorldDir() + "/gc.vaults/gpcore.bin")) return;
    // World's event vaults make collections from gpcore.bin's templates.
    Database db;
    LoadWorld(db, {"gpcore.bin", "15_2_1_sprint.bin"});
    LoadedPack* gpcore = nullptr;
    LoadedPack* event = nullptr;
    for (const auto& p : db.packs()) {
        if (p->path.find("gpcore.bin") != std::string::npos) gpcore = p.get();
        if (p->path.find("15_2_1_sprint.bin") != std::string::npos) event = p.get();
    }
    CHECK(gpcore && event);
    if (!gpcore || !event) return;
    const Bytes eventFile = ReadFile(event->path), gpcoreFile = ReadFile(gpcore->path);
    std::vector<Collection*> children;
    for (const auto& v : event->vaults)
        for (Collection* c : v->collections)
            if (c->parent && c->parent->vault->pack == gpcore) children.push_back(c);
    CHECK(!children.empty());
    const uint32_t gameplay = StringHash32("gameplay");
    const uint32_t someKey = gpcore->vaults.front()->collections.front()->key;

    // The database's pack and a pack holding classes can't go alone.
    for (const auto& p : db.packs())
        if (p->path.find("attributes.bin") != std::string::npos) CHECK(!db.unloadProblem(*p).empty());

    // A second copy of gpcore.bin, loaded last, is what names find and children inherit from.
    LoadedPack& copy = db.loadPack(WorldDir() + "/gc.vaults/gpcore_copy.bin", gpcoreFile);
    for (Collection* c : children) CHECK(c->parent && c->parent->vault->pack == &copy);
    const size_t before = db.collectionCount();
    size_t inCopy = 0;
    for (const auto& v : copy.vaults) inCopy += v->collections.size();
    CHECK(inCopy > 0);
    CHECK(db.unloadPack(copy).empty());
    CHECK_EQ(db.collectionCount(), before - inCopy);
    for (Collection* c : children) {
        CHECK(c->parent && c->parent->vault->pack == gpcore);
        if (c->parent) CHECK(std::count(c->parent->children.begin(), c->parent->children.end(), c) == 1);
    }
    const Collection* found = db.findCollection(gameplay, someKey);
    CHECK(found && found->vault->pack == gpcore);

    // Unloaded alone, its children keep their parent's key and write as read.
    CHECK(db.unloadPack(*gpcore).empty());
    CHECK(!db.findCollection(gameplay, someKey));
    for (Collection* c : children) CHECK(!c->parent && c->parentKey);
    for (const auto& cls : db.classes())
        for (const Collection* c : cls->collections) {
            CHECK(c->vault && (c->vault->pack == event || c->vault->pack->path.find("attributes.bin") != std::string::npos));
            for (const Collection* ch : c->children) CHECK(ch->parent == c);
        }
    CHECK(WritePack(db, *event) == eventFile);
    CHECK_EQ(db.packs().size(), size_t(2));
    // Loaded again, they find their parents.
    db.loadPack(WorldDir() + "/gc.vaults/gpcore.bin");
    for (Collection* c : children) CHECK(c->parent && c->parent->key == c->parentKey);
}

namespace {

std::string Carbon(const char* file) { return test::DataPath(std::string("attrib/databases/pc/carbon-vanilla/") + file); }

// Where two u32 values stand side by side in a file, or npos: in Carbon's
// attributes.bin, visuallook's ClassLoadData (its key, its layout size 24
// bytes on) and colorcurves' CollectionLoadData (its key, then its class's).
size_t FindPair(const Bytes& b, uint32_t first, uint32_t second, size_t gap) {
    size_t found = std::string::npos, hits = 0;
    for (size_t i = 0; i + gap + 4 <= b.size(); i += 4)
        if (load32(b.data() + i, Endian::Little) == first && load32(b.data() + i + gap, Endian::Little) == second) found = i, ++hits;
    return hits == 1 ? found : std::string::npos;
}

void CarbonDatabase(Database& db) {
    db.setTarget(Target{Game::Carbon, Platform::PC});
    db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/carbon_pc.txt")));
    db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
}

const uint32_t kVisualLook = StringHash32("visuallook"), kColorCurves = 0xE7CF45FE;

} // namespace

TEST("vaults: a pack that fails to load partway leaves the database as it was (crashdumps/crash5)") {
    // The app had gone on with a database whose attributes.bin failed halfway:
    // its type table, classes and first collections stayed, pointing at a vault
    // freed with the pack, the folder's other packs loaded over them, and the
    // recorder's copy of that database threw (Database::rollBack).
    if (!std::filesystem::exists(Carbon("attributes.bin"))) return;
    const Bytes retail = ReadFile(Carbon("attributes.bin"));
    Bytes b = retail;
    const size_t at = FindPair(b, kColorCurves, kVisualLook, 4);
    CHECK(at != std::string::npos);
    if (at == std::string::npos) return;
    store32(b.data() + at + 4, 0x0BADC1A5u, Endian::Little); // a class no vault defines
    auto fails = [](Database& db, const std::string& path, const Bytes& bytes, const char* message) {
        try {
            db.loadPack(path, bytes);
        } catch (const FormatError& e) {
            if (std::string(e.what()).find(message) == std::string::npos) std::printf("        %s\n", e.what());
            return std::string(e.what()).find(message) != std::string::npos;
        }
        return false;
    };

    // The database's own pack: nothing of it stays, and a pack of collections
    // loaded next fails as in a database with none.
    Database db;
    CarbonDatabase(db);
    CHECK(fails(db, Carbon("attributes.bin"), b, "uses an unknown class"));
    CHECK(db.packs().empty() && !db.hasSchema() && db.classes().empty() && db.collectionCount() == 0 && !db.typeSchema());
    CHECK(fails(db, Carbon("fe_attrib.bin"), ReadFile(Carbon("fe_attrib.bin")), "no database is loaded"));
    CHECK(db.packs().empty());
    // The file as it is loads into it as into a new database.
    db.loadPack(Carbon("attributes.bin"));
    Database fresh;
    CarbonDatabase(fresh);
    fresh.loadPack(Carbon("attributes.bin"));
    CHECK_EQ(db.collectionCount(), fresh.collectionCount());
    CHECK_EQ(db.classes().size(), fresh.classes().size());
    CHECK(db.typeSchema() && db.typeSchema()->source.find("carbon_pc.txt") != std::string::npos);
    CHECK(WritePack(db, *db.packs().front()) == retail);

    // A second pack that names the same collections and fails after some: the
    // names are the first pack's again, and nothing points at the second.
    const LoadedPack& first = *db.packs().front();
    CHECK(fails(db, test::DataPath("attributes_copy.bin"), b, "uses an unknown class"));
    CHECK_EQ(db.packs().size(), size_t(1));
    CHECK_EQ(db.collectionCount(), fresh.collectionCount());
    size_t elsewhere = 0;
    for (const auto& cls : db.classes())
        for (const Collection* c : cls->collections) {
            elsewhere += c->vault->pack != &first;
            elsewhere += db.findCollection(c->classKey, c->key) != c;
        }
    CHECK_EQ(elsewhere, size_t(0));
    CHECK(script::CloneDatabase(db)->collectionCount() == fresh.collectionCount());
}
