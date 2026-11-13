// The gameplay model (folders, templates, references, state graphs) and the
// gameplay edits, checked against Pro Street's gameplay.lzc and by writing the
// edited pack out and reading it back.

#include "Test.h"

#include "core/Compression.h"
#include "core/Edit.h"
#include "core/Folder.h"
#include "core/Game.h"
#include "core/Gameplay.h"
#include "core/Hash.h"
#include "core/Schema.h"
#include "core/Writer.h"
#include "core/lua/Lua.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>

using namespace attrib;

namespace {
std::string PS(const char* file) { return test::DataPath(std::string("attrib/databases/pc/prostreet-vanilla/") + file); }

std::unique_ptr<Database> LoadGameplay() {
    auto db = std::make_unique<Database>();
    db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    db->loadPack(PS("ATTRIBUTES.BIN"));
    db->loadPack(PS("gameplay.lzc"));
    return db;
}

Collection* Gp(Database& db, const std::string& name) {
    return db.findCollection(StringHash32("gameplay"), StringHash32(name));
}

std::string Text(const Collection& c, const char* field) {
    const Attribute* a = c.resolve(StringHash32(field));
    const PtrSlot* p = a ? a->data.ptrAt(0) : nullptr;
    return p ? p->text : std::string();
}

uint32_t Key(const Collection& c, const char* field, uint32_t i = 0) {
    const Attribute* a = c.resolve(StringHash32(field));
    return a && i < a->count ? load32(a->elem(i), Endian::Little) : 0;
}

bool Lists(const Collection& owner, uint32_t key) {
    const Attribute* a = owner.find(StringHash32("Children"));
    for (uint32_t i = 0; a && i < a->count; ++i)
        if (load32(a->elem(i), Endian::Little) == key) return true;
    return false;
}

// Writes the gameplay pack and loads it into a fresh database.
std::unique_ptr<Database> Reload(Database& db, const LoadedPack& gp) {
    const Bytes bytes = WritePack(db, gp);
    auto fresh = std::make_unique<Database>();
    fresh->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    fresh->loadPack(PS("ATTRIBUTES.BIN"));
    fresh->loadPack("gameplay.bin", bytes);
    return fresh;
}
} // namespace

TEST("gameplay: folders, templates, owners and reference fields") {
    auto db = LoadGameplay();
    GameplayModel m(*db);
    CHECK(m.valid());
    const GameplayModel::Node* n = m.node("tracks/autobahn_gd/autobahn_gd_grip_1");
    CHECK(n && n->collection);
    CHECK_EQ(n->segment, std::string("autobahn_gd_grip_1"));
    CHECK(n->parent && n->parent->path == "tracks/autobahn_gd");

    Collection* race = Gp(*db, "prostreet_career/bins/race_bin_an_2_1/gr_1");
    CHECK(race && race->parent);
    CHECK_EQ(db->names().format(race->parent->key), std::string("circuit"));
    CHECK(m.isTemplate(race->parent));
    CHECK(!m.isTemplate(race));
    CHECK(m.owner(race) == Gp(*db, "prostreet_career/bins/race_bin_an_2_1"));
    CHECK_EQ(m.childrenVaultName(Gp(*db, "tracks/autobahn_gd")), std::string("autobahn_gd"));

    // TrackLayout points at track layouts; TrackRecordHolder is a path in a Text.
    const auto* layout = m.refField(StringHash32("TrackLayout"));
    CHECK(layout && !layout->isText && !layout->templates.empty());
    CHECK_EQ(db->names().format(layout->templates.front()->key), std::string("track_layout"));
    CHECK(m.resolveKey(Key(*race, "TrackLayout"), layout) == Gp(*db, "tracks/autobahn_gd/autobahn_gd_grip_1"));
    const auto* holder = m.refField(StringHash32("TrackRecordHolder"));
    CHECK(holder && holder->isText);
    CHECK(!m.refField(StringHash32("EventID")));
    const auto* sky = m.refField(StringHash32("SunAndSky"));
    CHECK(sky && sky->targetClass == StringHash32("sunandsky"));
    // Candidates for TrackLayout are layouts, not races.
    const auto cands = m.candidates(*layout, false);
    CHECK(std::find(cands.begin(), cands.end(), Gp(*db, "tracks/autobahn_gd/autobahn_gd_grip_1")) != cands.end());
    CHECK(std::find(cands.begin(), cands.end(), race) == cands.end());

    // State graphs.
    Collection* basicrace = Gp(*db, "basicrace");
    Collection* racing = Gp(*db, "stategraph_basicrace_racing");
    const auto states = m.statesOf(basicrace);
    CHECK(std::find(states.begin(), states.end(), racing) != states.end());
    const auto handlers = m.handlersOf(racing);
    CHECK(std::find(handlers.begin(), handlers.end(), Gp(*db, "basicrace/stategraph_basicrace_racing_handler_notifyescaped")) !=
          handlers.end());
}

TEST("gameplay: handlers compile with their game's Lua: 5.1 for current-gen Undercover and World, 5.0 for the rest") {
    auto version = [](const char* id) {
        Database db;
        db.setTarget(*ParseTarget(id));
        return LuaVersionOf(db);
    };
    for (const char* id : {"mw_pc", "mw_ps2", "carbon_pc", "carbon_wii", "prostreet_pc", "prostreet_x360", "undercover_wii", "undercover_ps2"})
        CHECK(version(id) == lua::Version::Lua50);
    for (const char* id : {"undercover_pc", "undercover_x360", "undercover_ps3", "world_pc"}) CHECK(version(id) == lua::Version::Lua51);
    // A chunk there already keeps its own version.
    Database uc;
    uc.setTarget(*ParseTarget("undercover_pc"));
    const lua::CompileResult r = lua::Compile("function h_00000001(self, message, vars)\nend\n", lua::Version::Lua50);
    CHECK(r.ok() && VersionOfChunk(uc, r.chunk) == lua::Version::Lua50);
    CHECK(VersionOfChunk(uc, {}) == lua::Version::Lua51);
}

TEST("gameplay: an activity runs its templates' states too (Carbon's career_game runs career's initial)") {
    for (const char* folder : {"pc/carbon-vanilla", "360/carbon-vanilla", "wii/undercover-vanilla"}) {
        const std::string dir = test::DataPath(std::string("attrib/databases/") + folder);
        if (!std::filesystem::exists(dir)) continue;
        Database db;
        db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
        for (const FolderFile& f : FolderPacks(dir))
            if (f.skipped.empty()) db.loadPack(f.path);
        GameplayModel m(db);
        Collection* game = Gp(db, "career_game");
        Collection* career = Gp(db, "career");
        Collection* initial = Gp(db, "stategraph_career_game_initial");
        CHECK(game && career && initial && game->parent == career);
        // Its templateref names career, so it is career's own and career_game's by inheritance.
        const auto own = m.statesOf(game);
        const auto all = m.allStatesOf(game);
        CHECK_EQ(own.size(), size_t(4));
        CHECK(std::find(own.begin(), own.end(), initial) == own.end());
        CHECK_EQ(all.size(), size_t(5));
        CHECK(all.back() == initial); // its own first
        CHECK(m.stateTemplate(initial) == career);
        CHECK(m.stateTemplate(Gp(db, "stategraph_career_game_monitor")) == game);
        // ChangeState goes by Name.
        CHECK_EQ(m.stateName(initial), std::string("initial"));
        for (Collection* s : all) CHECK(!m.stateName(s).empty() && m.stateName(s).find("stategraph_") == std::string::npos);
        CHECK_EQ(m.allStatesOf(Gp(db, "drive_to_new_crew")).size(), size_t(9)); // 2 and drive_to_target's 7
        // A template that runs only its parent's graph has none of its own.
        Collection* basicrace = Gp(db, "basicrace");
        for (Collection* t : m.templates())
            if (t->parent == basicrace && m.statesOf(t).empty()) CHECK_EQ(m.allStatesOf(t).size(), m.allStatesOf(basicrace).size());
    }
}

TEST("gameplay: a race mode runs its templates' states, and the handlers it, its templates and its CollectionName own") {
    // Pro Street's lapknockout has no states of its own: it runs basicrace's
    // (through circuit), and owns one handler of theirs.
    auto db = LoadGameplay();
    GameplayModel m(*db);
    Collection* lap = Gp(*db, "lapknockout");
    Collection* circuit = Gp(*db, "circuit");
    Collection* basicrace = Gp(*db, "basicrace");
    Collection* drift = Gp(*db, "driftrace");
    CHECK(lap && circuit && basicrace && drift && lap->parent == circuit);
    if (!lap || !circuit || !basicrace || !drift) return;
    CHECK(m.statesOf(lap).empty());
    CHECK(m.allStatesOf(lap) == m.allStatesOf(basicrace));
    // The State Graph Builder lists them all.
    const std::vector<Collection*> acts = m.graphActivities();
    for (Collection* c : {lap, circuit, basicrace, drift}) CHECK(std::find(acts.begin(), acts.end(), c) != acts.end());
    // The handler lapknockout owns runs for lapknockout, not for basicrace.
    Collection* own = nullptr;
    Collection* state = nullptr;
    for (Collection* s : m.allStatesOf(lap))
        for (Collection* h : m.handlersOf(s))
            if (m.handlerOwner(h) == lap) {
                own = h;
                state = s;
            }
    CHECK(own && state);
    if (!own) return;
    CHECK(m.handlerCountsFor(own, lap));
    CHECK(!m.handlerCountsFor(own, basicrace));
    CHECK(!m.handlerCountsFor(own, drift));
    const std::vector<Collection*> forLap = m.handlersOf(state, lap), forBasic = m.handlersOf(state, basicrace);
    CHECK(std::find(forLap.begin(), forLap.end(), own) != forLap.end());
    CHECK(std::find(forBasic.begin(), forBasic.end(), own) == forBasic.end());
    // basicrace's own handlers of the state run for lapknockout too; driftrace's don't.
    for (Collection* h : m.handlersOf(state)) {
        if (m.handlerOwner(h) == basicrace) CHECK(std::find(forLap.begin(), forLap.end(), h) != forLap.end());
        if (m.handlerOwner(h) == drift) CHECK(std::find(forLap.begin(), forLap.end(), h) == forLap.end());
    }
    // Every handler of every state counts for its owner.
    for (Collection* s : m.allStatesOf(basicrace))
        for (Collection* h : m.handlersOf(s))
            if (Collection* o = m.handlerOwner(h)) CHECK(m.handlerCountsFor(h, o));
}

TEST("gameplay: duplicate a track layout, add a race on it, delete the copy") {
    auto db = LoadGameplay();
    LoadedPack& gp = *db->packs().back();
    const size_t vaults = gp.vaults.size();
    {
        GameplayModel m(*db);
        Collection* src = Gp(*db, "tracks/autobahn_gd/autobahn_gd_grip_1");
        const size_t kids = m.listedChildren(src).size();
        CHECK(kids > 10);
        Collection& copy = DuplicateGameplayElement(*db, m, *src, "tracks/autobahn_gd/autobahn_gd_grip_9", true);
        CHECK_EQ(Text(copy, "Name"), std::string("tracks/autobahn_gd/autobahn_gd_grip_9"));
        CHECK_EQ(Text(copy, "CollectionName"), std::string("tracks/autobahn_gd/autobahn_gd_grip_9"));
        CHECK_EQ(Text(copy, "gameplayvault"), std::string("autobahn_gd_grip_9"));
        CHECK(copy.vault == src->vault);
        CHECK(Lists(*Gp(*db, "tracks/autobahn_gd"), copy.key));
        CHECK(FindVault(gp, "autobahn_gd_grip_9"));
        CHECK_EQ(gp.vaults.size(), vaults + 1);
    }
    {
        GameplayModel m(*db);
        Collection* copy = Gp(*db, "tracks/autobahn_gd/autobahn_gd_grip_9");
        // Its children are copies in the new vault, and its references point at them.
        const auto kids = m.listedChildren(copy);
        CHECK(!kids.empty());
        for (Collection* k : kids) {
            CHECK_EQ(k->vault->name, std::string("autobahn_gd_grip_9"));
            CHECK(db->names().format(k->key).rfind("tracks/autobahn_gd/autobahn_gd_grip_9/", 0) == 0);
        }
        CHECK(Key(*copy, "racestart") == StringHash32("tracks/autobahn_gd/autobahn_gd_grip_9/startgrid1"));
        CHECK(Gp(*db, "tracks/autobahn_gd/autobahn_gd_grip_9/startgrid1"));

        // A new race from the circuit template, in an existing bin, on the new layout.
        NewGameplayElement e;
        e.path = "prostreet_career/bins/race_bin_an_2_1/gr_9";
        e.templ = Gp(*db, "circuit");
        e.owner = Gp(*db, "prostreet_career/bins/race_bin_an_2_1");
        e.childrenVault = "an_2_1_gr_9";
        Collection& race = CreateGameplayElement(*db, m, e);
        CHECK_EQ(race.vault->name, std::string("race_bin_an_2_1"));
        CHECK(Lists(*e.owner, race.key));
        Attribute& tl = OverrideAttribute(*db, race, *race.cls->field(StringHash32("TrackLayout")));
        store32(tl.elem(0), copy->key, Endian::Little);
    }
    {
        // Saved and read back, everything is where it was put.
        auto fresh = Reload(*db, gp);
        for (const auto& v : fresh->packs().back()->vaults) CHECK(v->warnings.empty());
        GameplayModel m(*fresh);
        Collection* copy = Gp(*fresh, "tracks/autobahn_gd/autobahn_gd_grip_9");
        Collection* race = Gp(*fresh, "prostreet_career/bins/race_bin_an_2_1/gr_9");
        CHECK(copy && race);
        CHECK_EQ(copy->vault->name, std::string("autobahn_gd"));
        CHECK_EQ(db->names().format(race->parent->key), std::string("circuit"));
        CHECK(m.resolveKey(Key(*race, "TrackLayout"), m.refField(StringHash32("TrackLayout"))) == copy);
        CHECK_EQ(m.listedChildren(copy).size(), m.listedChildren(Gp(*fresh, "tracks/autobahn_gd/autobahn_gd_grip_1")).size());
        CHECK_EQ(Text(*race, "gameplayvault"), std::string("an_2_1_gr_9"));
    }
    {
        // Deleting the copy takes its children and its vault with it; the race
        // still names it (references elsewhere are left alone).
        GameplayModel m(*db);
        Collection* copy = Gp(*db, "tracks/autobahn_gd/autobahn_gd_grip_9");
        const uint32_t key = copy->key;
        const auto gone = DeleteGameplayElement(*db, m, *copy);
        CHECK(gone.size() > 10);
        CHECK(!Gp(*db, "tracks/autobahn_gd/autobahn_gd_grip_9"));
        CHECK(!Gp(*db, "tracks/autobahn_gd/autobahn_gd_grip_9/startgrid1"));
        CHECK(!Lists(*Gp(*db, "tracks/autobahn_gd"), key));
        CHECK(!FindVault(gp, "autobahn_gd_grip_9"));
        CHECK_EQ(gp.vaults.size(), vaults);
        auto fresh = Reload(*db, gp);
        CHECK(Gp(*fresh, "prostreet_career/bins/race_bin_an_2_1/gr_9"));
    }
}

TEST("gameplay: an unedited gameplay pack still writes back byte for byte after building the model") {
    auto db = LoadGameplay();
    GameplayModel m(*db);
    CHECK(m.valid());
    CHECK(WritePack(*db, *db->packs().back()) == ReadFile(PS("gameplay.bin")));
}

TEST("gameplay: a duplicated message handler answers under its new function name") {
    auto db = LoadGameplay();
    GameplayModel m(*db);
    Collection* h = Gp(*db, "basicrace/stategraph_basicrace_racing_handler_notifyescaped");
    CHECK(h);
    std::vector<std::string> warnings;
    Collection& copy = DuplicateGameplayElement(*db, m, *h, "basicrace/stategraph_basicrace_racing_handler_notifyescaped_9", false,
                                                &warnings);
    for (const auto& w : warnings) std::printf("        %s\n", w.c_str());
    CHECK(warnings.empty());
    const lua::ChunkInfo info = lua::Inspect(HandlerChunk(copy));
    CHECK_EQ(info.globalsSet.size(), size_t(1));
    CHECK_EQ(info.globalsSet.front(), lua::HandlerFunctionName("basicrace/stategraph_basicrace_racing_handler_notifyescaped_9"));
    // Same code otherwise: the original's chunk with the name swapped decompiles alike.
    std::string a = lua::Decompile(HandlerChunk(*h)).source, b = lua::Decompile(HandlerChunk(copy)).source;
    const std::string oldFn = lua::HandlerFunctionName("basicrace/stategraph_basicrace_racing_handler_notifyescaped");
    a.replace(a.find(oldFn), oldFn.size(), info.globalsSet.front());
    CHECK(a == b);
    CHECK(Key(copy, "stateref") == Key(*h, "stateref"));
}

TEST("gameplay: a Most Wanted handler defines a function named for the handler itself; a duplicate gets its own") {
    // Most Wanted names it as the collection (and its CollectionName, which
    // the game looks it up by), not h_<hash>. Its early demos do the same,
    // with StringHash64 keys (and GCollectionKey references).
    for (const char* folder : {"pc/mw-vanilla", "xbox/mw-early-oxm-betademo"}) {
        const std::string dir = test::DataPath(std::string("attrib/databases/") + folder);
        auto load = [&]() {
            auto db = std::make_unique<Database>();
            for (const char* s : {"mw_pc", "mw_xbox_demo"}) db->addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
            db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
            return db;
        };
        auto db = load();
        for (const FolderFile& f : FolderPacks(dir))
            if (f.skipped.empty()) db->loadPack(f.path);
        GameplayModel m(*db);
        CHECK(m.valid());
        const std::string name = db->keys64() ? "stategraph_bin_race_engage_reset_handler_stateenter" : "stategraph_basicrace_done_handler_stateenter";
        Collection* h = Gp(*db, name);
        CHECK(h);
        if (!h) continue;
        CHECK_EQ(HandlerFunction(*db, name), name);
        CHECK_EQ(Text(*h, "CollectionName"), name);
        CHECK(lua::Inspect(HandlerChunk(*h)).globalsSet == std::vector<std::string>{name});
        std::vector<std::string> warnings;
        Collection& copy = DuplicateGameplayElement(*db, m, *h, name + "_9", false, &warnings);
        for (const auto& w : warnings) std::printf("        %s\n", w.c_str());
        CHECK(warnings.empty());
        CHECK(lua::Inspect(HandlerChunk(copy)).globalsSet == std::vector<std::string>{name + "_9"});
        CHECK_EQ(Text(copy, "CollectionName"), name + "_9");
        CHECK(Key(copy, "stateref") == Key(*h, "stateref"));
        // Compiled Lua stays on 16 bytes in Most Wanted's gameplay packs.
        const Attribute* bc = copy.resolve(StringHash32("bytecode"));
        CHECK(bc && bc->data.ptrAt(4) && bc->data.ptrAt(4)->data && bc->data.ptrAt(4)->data->align == 16u);
        if (!db->keys64()) continue;
        // Written and read again: the copy's key, export id and its state reference are StringHash64.
        const Attribute* sr = copy.find(StringHash32("stateref"));
        CHECK(sr && sr->elemSize == 8);
        auto again = load();
        again->setTarget(db->target());
        again->names().add(name + "_9");
        for (const auto& p : db->packs()) again->loadPack(p->path, WritePack(*db, *p));
        Collection* read = Gp(*again, name + "_9");
        CHECK(read && read->exportId == StringHash64("gameplay/" + name + "_9"));
        if (read) {
            CHECK(lua::Inspect(HandlerChunk(*read)).globalsSet == std::vector<std::string>{name + "_9"});
            CHECK_EQ(load64(read->find(StringHash32("stateref"))->elem(0), Endian::Little), load64(h->find(StringHash32("stateref"))->elem(0), Endian::Little));
        }
    }
}

namespace {
// Offset of a vault's .vlt in a pack's directory.
uint32_t VltOffset(const Bytes& pack, const std::string& vault) {
    ByteReader r(pack, Endian::Little);
    const uint32_t n = r.u32(4), strings = r.u32(8);
    for (uint32_t i = 0; i < n; ++i)
        if (r.cstr(strings + r.u32(16 + i * 20)) == vault) return r.u32(16 + i * 20 + 16);
    return 0;
}
} // namespace

TEST("gameplay: saving gameplay.lzc writes gameplay.bin too, so streamed vaults are where the .lzc says") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "attribulina_twin_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::copy_file(PS("gameplay.lzc"), dir / "gameplay.lzc");

    // The edit from the crash report: another Debug.Print in a gpcore handler
    // grows gpcore and moves every vault after it.
    auto db = std::make_unique<Database>();
    db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    db->loadPack(PS("ATTRIBUTES.BIN"));
    LoadedPack& gp = db->loadPack((dir / "gameplay.lzc").string());
    Collection* h = Gp(*db, "basicrace/stategraph_basicrace_racing_handler_playerstateenter");
    CHECK(h && h->vault->name == "gpcore");
    std::string src = lua::Decompile(HandlerChunk(*h)).source;
    const size_t body = src.find('\n');
    src.insert(body + 1, "\tDebug.Print(\"Hi, I'm Jeff Gordon, professional NASCAR driver\")\n");
    const lua::CompileResult cr = lua::Compile(src);
    CHECK(cr.ok());
    SetHandlerChunk(*db, *h, cr.chunk);

    CHECK(PackTwin((dir / "gameplay.lzc").string()));
    SavePack(*db, gp, (dir / "gameplay.lzc").string());
    CHECK(fs::exists(dir / "gameplay.bin"));
    const Bytes lzc = Decompress(ReadFile((dir / "gameplay.lzc").string()));
    const Bytes bin = ReadFile((dir / "gameplay.bin").string());
    CHECK(lzc == bin);
    // race_bin_an_3_1 moved: a retail .bin left beside it is what crashed the game.
    const Bytes retail = ReadFile(PS("gameplay.bin"));
    CHECK(VltOffset(bin, "race_bin_an_3_1") != VltOffset(retail, "race_bin_an_3_1"));
    CHECK(!(bin == retail));

    // A .bin with a .lzc beside it saves the .lzc too, compressed like it was.
    CHECK(PackTwin((dir / "gameplay.bin").string()) == (dir / "gameplay.lzc").string());
    CHECK(!PackTwin(PS("ATTRIBUTES.BIN")));
    auto fromBin = std::make_unique<Database>();
    fromBin->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    fromBin->loadPack(PS("ATTRIBUTES.BIN"));
    LoadedPack& gb = fromBin->loadPack((dir / "gameplay.bin").string());
    SavePack(*fromBin, gb, (dir / "gameplay.bin").string());
    const Bytes again = ReadFile((dir / "gameplay.lzc").string());
    CHECK(ParseLzHeader(again) && ParseLzHeader(again)->kind == LzKind::Jdlz);
    CHECK(Decompress(again) == bin);
    fs::remove_all(dir);
}

TEST("gameplay: the message lists name every handler's message_id in Most Wanted, Pro Street, Carbon and Undercover") {
    // Every list in data/messages, as the editor loads them: the games share names.
    std::vector<MessageDef> all;
    for (const auto& e : std::filesystem::directory_iterator(test::DataPath("data/messages")))
        for (MessageDef& m : LoadMessages(e.path().string())) all.push_back(std::move(m));
    std::unordered_set<uint32_t> named;
    for (const MessageDef& m : all) named.insert(StringHash32(m.name));
    // One entry per name, the game's own first: Carbon's MNotifyOnlineRaceOver
    // has a field, and an inferred name has no game.
    const std::vector<const MessageDef*> carbon = MessagesFor(all, Game::Carbon);
    std::unordered_set<std::string> seen;
    for (const MessageDef* m : carbon) CHECK(seen.insert(m->name).second);
    auto find = [&](const std::string& n) -> const MessageDef* {
        for (const MessageDef* m : carbon)
            if (m->name == n) return m;
        return nullptr;
    };
    const MessageDef* over = find("MNotifyOnlineRaceOver");
    CHECK(over && over->game == Game::Carbon && over->fields.size() == 1 && over->fields[0].first == "ShowPostRace");
    CHECK(StringHash32("MNotifyOnlineRaceOver") == 0xB16576C0u);
    CHECK(find("MNotifyRaceAbandoned"));
    const MessageDef* round = find("MNotifyRoundOver"); // from the Wii's Undercover handler names
    CHECK(round && round->game == Game::Unknown);

    for (const char* folder : {"pc/prostreet-vanilla", "pc/carbon-vanilla", "360/carbon-vanilla", "wii/undercover-vanilla", "pc/undercover-vanilla",
                               "360/undercover-vanilla", "360/undercover-alpha161", "pc/mw-vanilla", "ps2/mw-beta", "ps2/mw-early-opm-betademo"}) {
        const std::string dir = test::DataPath(std::string("attrib/databases/") + folder);
        if (!std::filesystem::exists(dir)) continue;
        Database db;
        db.names().loadFile(test::DataPath("data/attrib_keys.txt")); // the demos' 64-bit keys find their names by it
        for (const FolderFile& f : FolderPacks(dir))
            if (f.skipped.empty()) db.loadPack(f.path);
        size_t handlers = 0;
        for (const auto& cls : db.classes()) {
            if (cls->key != StringHash32("gameplay")) continue;
            for (const Collection* c : cls->collections) {
                const Attribute* a = c->find(StringHash32("message_id"));
                if (!a || a->elemSize != 4) continue;
                const uint32_t id = load32(a->data.bytes.data(), c->vault->raw.endian);
                if (!id) continue;
                ++handlers;
                if (!named.count(id)) std::printf("        %s: message_id 0x%08X has no name\n", folder, id);
                CHECK(named.count(id));
            }
        }
        CHECK(handlers > 100);
    }
}
