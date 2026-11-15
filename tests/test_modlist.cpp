// The list of installed mods (core/ModList.h, attribulina_mods.txt), what
// names a mod (its title and version, else its file's SHA-256), and an nfsms
// script's # amod block, which gives it a title, author, version and game in
// comments NFS-VltEd skips.

#include "Test.h"

#include "core/Hash.h"
#include "core/Install.h"
#include "core/ModList.h"
#include "core/ModTrace.h"
#include "core/Nfsms.h"
#include "core/Script.h"
#include "core/Writer.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace attrib;

namespace {

void WriteText(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

std::string ReadText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

namespace sc = attrib::script;

std::unique_ptr<Database> LoadProStreet() {
    auto db = std::make_unique<Database>();
    db->addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
    db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    for (const char* f : {"ATTRIBUTES.BIN", "cars_vault.BIN", "FE_ATTRIB.BIN"})
        db->loadPack(test::DataPath(std::string("attrib/databases/pc/prostreet-vanilla/") + f));
    return db;
}

std::string Messages(const sc::ApplyReport& r) {
    std::string s;
    for (const sc::Message& m : r.messages) s += std::to_string(m.pos.line) + ": " + m.text + "\n";
    return s;
}

// Installs `text` into `db` as the mod `title`, traced as the installer traces it.
TracedMod InstallTraced(Database& db, const std::string& title, const std::string& text) {
    auto before = sc::CloneDatabase(db);
    const sc::ApplyReport r = sc::Apply(db, sc::Parse(text, title + ".amod"));
    if (!r.ok) std::fprintf(stderr, "%s", Messages(r).c_str());
    CHECK(r.ok);
    TracedMod t;
    t.mod.title = title;
    t.mod.installed = "2026-11-15 10:00:00";
    t.trace = std::make_shared<const sc::Script>(MakeTrace(*before, db, t.mod));
    return t;
}

// Statements that change something.
size_t Changes(const sc::Script& s) {
    size_t n = 0;
    for (const sc::Statement& st : s.statements)
        n += st.kind != sc::Statement::Kind::Header && st.kind != sc::Statement::Kind::Meta && st.kind != sc::Statement::Kind::Comment;
    return n;
}

// An integer value of a collection, own or inherited, of 1, 2 or 4 bytes.
uint32_t ValueOf(Database& db, const char* cls, const char* name, const char* field) {
    Collection* c = db.findCollection(StringHash32(cls), StringHash32(name));
    CHECK(c != nullptr);
    if (!c) return 0;
    sc::FieldPath p;
    p.field = field;
    Bytes b;
    const ValueType vt = sc::TypeAt(db, *c, p, &b);
    if (vt.size == 1 && !b.empty()) return b[0];
    if (vt.size == 2 && b.size() >= 2) return load16(b.data(), Endian::Little);
    return b.size() >= 4 ? load32(b.data(), Endian::Little) : 0;
}

std::map<std::string, Bytes> PackBytes(const Database& db) {
    std::map<std::string, Bytes> out;
    for (const auto& pk : db.packs()) out[pk->path] = WritePack(db, *pk);
    return out;
}

bool Throws(const std::string& text, const std::string& part) {
    try {
        ModList::Parse(text, "mods.txt");
    } catch (const std::exception& e) {
        return std::string(e.what()).find(part) != std::string::npos;
    }
    return false;
}

} // namespace

TEST("modlist: SHA-256 gives FIPS 180-4's digests, across the padding's block boundary") {
    CHECK_EQ(Sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(Sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    // 55, 56 and 64 bytes: the length fits after the 1 bit, then needs a block of its own.
    CHECK_EQ(Sha256Hex(std::string(55, 'a')), std::string("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
    CHECK_EQ(Sha256Hex(std::string(56, 'a')), std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
    CHECK_EQ(Sha256Hex(std::string(64, 'a')), std::string("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
    CHECK_EQ(Sha256Hex(std::string(1000000, 'a')), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST("modlist: the file reads and writes back, quotes doubled, a later version's lines kept") {
    ModList list;
    InstalledMod a;
    a.title = "Shifted \"Lite\"";
    a.author = "someone";
    a.version = "1.2";
    a.script = "D:\\Mods\\Shifted\\Shifted.nfsms";
    a.sha256 = Sha256Hex("x");
    a.nfsms = true;
    a.game = "prostreet_pc";
    a.installed = "2026-11-14 15:30:12";
    a.by = "Attribulina Mod Installer 1.0";
    a.result = "installed, 2 lines failed";
    a.options = {{"Loud", "on"}, {"Sound", "Quiet one"}};
    a.files = {"GLOBAL\\ATTRIBUTES.BIN", "GLOBAL\\GlobalMemoryFile.bin"};
    InstalledMod b; // no title: named by its file
    b.script = "C:\\x\\tune.amod";
    b.sha256 = Sha256Hex("y");
    list.mods = {a, b};
    const std::string text = list.print();
    CHECK(text.find("attribulina mods 1\n") != std::string::npos);
    CHECK(text.find("mod \"Shifted \"\"Lite\"\"\"\n") != std::string::npos);
    CHECK(text.find("    script \"D:\\Mods\\Shifted\\Shifted.nfsms\"\n") != std::string::npos); // backslashes as they are
    CHECK(text.find("\nmod\n") != std::string::npos);
    const ModList back = ModList::Parse(text);
    CHECK_EQ(back.mods.size(), size_t(2));
    CHECK_EQ(back.print(), text);
    CHECK_EQ(back.mods[0].title, a.title);
    CHECK_EQ(back.mods[0].options.size(), size_t(2));
    CHECK_EQ(back.mods[0].options[1].second, std::string("Quiet one"));
    CHECK(back.mods[0].nfsms);
    CHECK_EQ(back.mods[0].files[0], std::string("GLOBAL\\ATTRIBUTES.BIN"));
    CHECK_EQ(back.mods[1].name(), std::string("tune.amod"));
    CHECK_EQ(back.mods[0].name(), a.title);

    // A line a later version writes stays with its mod; Windows line ends read too.
    const std::string later = "attribulina mods 2\r\n# a note\r\nmod \"A\"\r\n    undo \"A.undo.amod\"\r\n    version \"1\"\r\nend\r\n";
    const ModList l = ModList::Parse(later);
    CHECK_EQ(l.version, 2);
    CHECK_EQ(l.mods.size(), size_t(1));
    CHECK_EQ(l.mods[0].unknown.size(), size_t(1));
    CHECK(l.print().find("    undo \"A.undo.amod\"\n") != std::string::npos);
    CHECK(l.print().find("attribulina mods 2\n") != std::string::npos);

    CHECK(Throws("", "starts with `attribulina mods 1`"));
    CHECK(Throws("attribulina mods 1\nversion \"1\"\n", "mods.txt:2: `version` outside a mod"));
    CHECK(Throws("attribulina mods 1\nmod \"A\n", "mods.txt:2: a quote that doesn't end"));
    CHECK(Throws("attribulina mods 1\nmod \"A\"\n", "has no `end`"));
    CHECK(Throws("attribulina mods 1\nmod \"A\"\nmod \"B\"\nend\n", "mods.txt:3: mod inside a mod"));
    CHECK(Throws("attribulina mods 1\nmod \"A\"\noption \"x\"\nend\n", "option takes 2 texts"));
}

TEST("modlist: added to in place through a .tmp; a folder without the file has no list") {
    const fs::path dir = fs::temp_directory_path() / "attribulina_modlist_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    CHECK(!ReadModList(dir.string()));
    InstalledMod a;
    a.title = "A";
    AddToModList(dir.string(), {a});
    InstalledMod b;
    b.title = "B";
    const ModList both = AddToModList(dir.string(), {b});
    CHECK_EQ(both.mods.size(), size_t(2));
    const auto read = ReadModList(dir.string());
    CHECK(read && read->mods.size() == 2 && read->mods[1].title == "B");
    CHECK(!fs::exists(dir / "attribulina_mods.txt.tmp"));
    // One that doesn't read is left as it is.
    WriteText(dir / kModListFile, "not a list\n");
    bool threw = false;
    try {
        AddToModList(dir.string(), {a});
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(ReadText(dir / kModListFile), std::string("not a list\n"));
    fs::remove_all(dir);

    CHECK_EQ(ModListFileName("/games/ps/GLOBAL/ATTRIBUTES.BIN", "/games/ps"), std::string("GLOBAL\\ATTRIBUTES.BIN"));
    CHECK_EQ(ModListFileName("/elsewhere/x.bin", "/games/ps"), std::string("\\elsewhere\\x.bin"));
}

TEST("modlist: a mod is the same by its title, in any case, or by its file; versions tell installs apart") {
    ModList list;
    InstalledMod a;
    a.title = "Shifted";
    a.version = "1.0";
    a.sha256 = "aa";
    a.installed = "2026-11-14 15:30:12";
    InstalledMod untitled;
    untitled.sha256 = "bb";
    untitled.script = "C:\\m\\tune.nfsms";
    list.mods = {a, untitled};

    auto found = FindInstalled(list, {" shifted ", "1.0", "cc", ""});
    CHECK(found.size() == 1 && found[0].sameVersion);
    CHECK(InstalledNote(found, {"Shifted", "1.0", "cc", ""}).find("Shifted 1.0 is installed here already (installed 2026-11-14 15:30:12)") == 0);
    found = FindInstalled(list, {"Shifted", "1.1", "cc", ""});
    CHECK(found.size() == 1 && !found[0].sameVersion);
    CHECK(InstalledNote(found, {"Shifted", "1.1", "cc", ""}).find("Installing version 1.1 over it") != std::string::npos);
    // Without a title only the same file matches; the same file under another title does too.
    CHECK(FindInstalled(list, {"", "", "bb", ""}).size() == 1);
    CHECK(FindInstalled(list, {"", "", "dd", ""}).empty());
    found = FindInstalled(list, {"Renamed", "", "AA", ""});
    CHECK(found.size() == 1 && found[0].sameVersion);
    CHECK(InstalledNote({}, {"x", "", "", ""}).empty());
}

TEST("modlist: an nfsms script's # amod block (or // amod) gives the mod its title, version and game") {
    const std::string text =
        "version 4.6\n"
        "game PS\n"
        "# amod begin\n"
        "# attribulina 1\n"
        "# game prostreet_all\n"
        "#title \"Shifted\"\n"
        "\n"
        "#   author \"someone\"\n"
        "# version \"1.2\"\n"
        "# amod end\n"
        "ui_text Shifts faster.\n";
    const nfsms::Script s = nfsms::Parse(text, "Shifted.nfsms");
    CHECK(s.modInfoError.empty());
    CHECK_EQ(s.modInfoLine, 3);
    std::vector<std::string> warnings;
    const script::Script info = install::ModInfoOf(s, &warnings);
    CHECK(warnings.empty());
    CHECK_EQ(info.meta("title"), std::string("Shifted"));
    CHECK_EQ(info.meta("author"), std::string("someone"));
    CHECK_EQ(info.meta("version"), std::string("1.2"));
    CHECK_EQ(info.meta("game"), std::string("prostreet_all"));
    // The block's lines are comments: no command comes of them.
    for (const nfsms::Command& c : s.commands) CHECK(c.name == "version" || c.name == "game" || c.name == "ui_text");

    // // works as the marker too, as mods write their notes.
    const nfsms::Script slashes = nfsms::Parse("// AMOD Begin\n// title \"Two\"\n//amod   end\n", "two.nfsms");
    CHECK_EQ(install::ModInfoOf(slashes).meta("title"), std::string("Two"));

    // Mistakes are warnings with their line, and the block counts for nothing.
    auto warningOf = [](const std::string& t) {
        std::vector<std::string> w;
        const script::Script i = install::ModInfoOf(nfsms::Parse(t, "m.nfsms"), &w);
        CHECK(i.statements.empty() || !w.empty());
        return w.empty() ? std::string() : w.front();
    };
    CHECK(warningOf("# amod begin\n# title \"A\"\n").find("m.nfsms: line 1: # amod begin has no # amod end") == 0);
    CHECK(warningOf("# amod begin\nupdate_field a b c 1\n# amod end\n").find("line 2 is in the # amod block, but isn't a comment") != std::string::npos);
    CHECK(warningOf("# amod begin\n# title \"A\n# amod end\n").find("m.nfsms:2:") == 0); // the file's line
    CHECK(warningOf("# amod begin\n# edit pvehicle/a { x = 1 }\n# amod end\n").find("m.nfsms:2: only game, title") == 0);
    CHECK(warningOf("# amod begin\n# amod end\n# amod begin\n# amod end\n").find("a second # amod begin") != std::string::npos);
    CHECK(warningOf("# title \"no block\"\n").empty());
}

TEST("modlist: ReadMod names a mod by its title and version, else by its file's SHA-256") {
    const fs::path dir = fs::temp_directory_path() / "attribulina_modlist_readmod";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string titled = "version 4.6\n# amod begin\n# title \"Louder\"\n# version \"2\"\n# description \"Horns.\"\n# amod end\nui_text Not this.\n";
    WriteText(dir / "louder.nfsms", titled);
    install::Mod m = install::ReadMod((dir / "louder.nfsms").string());
    CHECK(m.titled);
    CHECK_EQ(m.title, std::string("Louder"));
    CHECK_EQ(m.version, std::string("2"));
    CHECK_EQ(m.description, std::string("Horns.")); // the block's, not the ui_text lines
    CHECK_EQ(m.sha256, Sha256Hex(titled));
    CHECK_EQ(m.identity().title, std::string("Louder"));

    WriteText(dir / "plain.nfsms", "version 4.6\nui_text Its text.\n");
    m = install::ReadMod((dir / "plain.nfsms").string());
    CHECK(!m.titled);
    CHECK_EQ(m.title, std::string("plain"));
    CHECK_EQ(m.description, std::string("Its text."));
    CHECK(m.identity().title.empty()); // the file's hash names it
    CHECK_EQ(m.identity().sha256, Sha256Hex("version 4.6\nui_text Its text.\n"));

    WriteText(dir / "tune.amod", "attribulina 1\ntitle \"Tune\"\nauthor \"me\"\nversion \"3\"\n");
    m = install::ReadMod((dir / "tune.amod").string());
    CHECK(m.titled);
    CHECK_EQ(m.author, std::string("me"));

    WriteText(dir / "bad.nfsms", "# amod begin\n# title \"A\n# amod end\n");
    m = install::ReadMod((dir / "bad.nfsms").string());
    CHECK(!m.titled);
    CHECK_EQ(m.warnings.size(), size_t(1));
    fs::remove_all(dir);
}

TEST("modlist: // after an nfsms command isn't a comment, and the error says so") {
    const nfsms::Command c = nfsms::ParseLine("update_field pvehicle a HornType 3 // louder", 1);
    CHECK_EQ(c.name, std::string("update_field"));
    CHECK_EQ(c.args.size(), size_t(6)); // // and louder are parameters, as NFS-VltEd reads them
    CHECK(nfsms::ParseLine("// update_field pvehicle a HornType 3", 1).name.empty());
    CHECK(nfsms::ParseLine("   //note", 1).name.empty());

    // NFS-VltEd's parameter count fails the line; the message says why.
    Database db;
    db.loadPack(test::DataPath("attrib/databases/pc/prostreet-vanilla/ATTRIBUTES.BIN"));
    nfsms::RunSettings st;
    st.vltEd = true;
    const script::ApplyReport r = nfsms::Run(db, nfsms::Parse("update_field pvehicle 911gt3 HornType 3 // louder\n"), st);
    CHECK_EQ(r.errors(), size_t(1));
    bool hinted = false;
    for (const script::Message& m : r.messages) hinted |= m.text.find("after a command, use #") != std::string::npos;
    CHECK(hinted);
}

TEST("mod traces: uninstalling the only mod gives the database back, byte for byte") {
    auto db = LoadProStreet();
    auto base = sc::CloneDatabase(*db);
    std::map<std::string, Bytes> original = PackBytes(*db);
    const TracedMod a = InstallTraced(*db, "Everything",
                                      "attribulina 2\n"
                                      "edit audio_revlimiter/default { total_frames_short = 77\n load_for_short = 0.125 }\n"
                                      "edit car_list/all_cars { append Cars = pvehicle/street if missing }\n"
                                      "edit collisionsfx_damage/hood { VolMap = [5, 6, 7, 8, 9] }\n"
                                      "create pvehicle/trace_car from pvehicle/street { HornType = 3 }\n"
                                      "create pvehicle/trace_kid under pvehicle/trace_car\n"
                                      "rename car/gtrproto to gtrproto2\n"
                                      "move pvehicle/street under root\n");
    const std::string text = sc::Print(*a.trace);
    CHECK(text.find("title \"Undo Everything\"") != std::string::npos);
    CHECK(text.find("# The trace of Everything, installed 2026-11-15 10:00:00.") != std::string::npos);
    CHECK(text.find("delete pvehicle/trace_kid") != std::string::npos);
    CHECK(text.find("rename car/gtrproto2 to gtrproto") != std::string::npos);
    CHECK(text.find("    remove Cars = pvehicle/street\n") != std::string::npos);
    CHECK(text.find("total_frames_short = ") != std::string::npos && text.find(" was 77") != std::string::npos);
    // It reads back as it was written.
    CHECK_EQ(sc::Print(sc::Parse(text, "trace.amod")), text);

    const UninstallResult r = Uninstall(*db, {a}, {0});
    if (!r.ok) std::fprintf(stderr, "%s\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(!r.redone);
    CHECK(r.retraced.empty());
    CHECK_EQ(r.removed.size(), size_t(1));
    for (const std::string& n : r.notes) std::fprintf(stderr, "        note: %s\n", n.c_str());
    CHECK(r.notes.empty());
    CHECK(r.deleted >= 2 && r.valuesSet >= 3);
    const sc::Script left = sc::Diff(*base, *db);
    if (Changes(left)) std::fprintf(stderr, "%s", sc::Print(left).c_str());
    CHECK_EQ(Changes(left), size_t(0));
    const std::map<std::string, Bytes> now = PackBytes(*db);
    for (const auto& [path, bytes] : original) {
        const bool same = now.count(path) && now.at(path) == bytes;
        if (!same) std::fprintf(stderr, "        %s differs after uninstalling\n", path.c_str());
        CHECK(same);
    }
    // A collection deleted comes back with its values, though the file may
    // keep it in another place.
    const TracedMod gone = InstallTraced(*db, "Delete", "attribulina 2\ndelete emittergroup/terraindriving with children\n");
    CHECK(sc::Print(*gone.trace).find("collection emittergroup/terraindriving under ") != std::string::npos);
    CHECK(Uninstall(*db, {gone}, {0}).ok);
    CHECK_EQ(Changes(sc::Diff(*base, *db)), size_t(0));
}

TEST("mod traces: what two mods change in common") {
    auto db = LoadProStreet();
    const TracedMod a = InstallTraced(*db, "A", "attribulina 2\nedit audio_revlimiter/default { total_frames_short = 77 }\n"
                                                "edit car_list/all_cars { append Cars = pvehicle/street if missing }\n");
    const TracedMod b = InstallTraced(*db, "B", "attribulina 2\nedit audio_revlimiter/default { total_frames_long = 5 }\n"
                                                "edit car_list/all_cars { append Cars = pvehicle/default if missing }\n");
    const TracedMod c = InstallTraced(*db, "C", "attribulina 2\nedit audio_revlimiter/default { total_frames_short = 5 }\n"
                                                "create pvehicle/trace_car from pvehicle/street\n");
    const TracedMod d = InstallTraced(*db, "D", "attribulina 2\nedit pvehicle/trace_car { HornType = 3 }\n");
    const std::vector<TracedMod> mods{a, b, c};
    // Another field, and two appends to the same list: apart.
    CHECK(FindConflicts(PlacesOf(*b.trace), {a}).empty());
    // The same value.
    std::vector<ModConflict> found = FindConflicts(PlacesOf(*c.trace), {a, b});
    CHECK_EQ(found.size(), size_t(1));
    if (!found.empty()) {
        CHECK_EQ(found[0].index, size_t(0));
        CHECK_EQ(found[0].count, size_t(1));
        CHECK_EQ(ConflictText(found[0], "A"), std::string("changes 1 thing A changed: audio_revlimiter/default total_frames_short"));
    }
    // A value of a collection another made builds on that mod: no conflict.
    CHECK(FindConflicts(PlacesOf(*d.trace), mods).empty());
    // Making a collection another made (deleting or renaming it, too) does conflict.
    const TracedMod e = InstallTraced(*db, "E", "attribulina 2\ndelete pvehicle/trace_car\n");
    found = FindConflicts(PlacesOf(*e.trace), mods);
    CHECK_EQ(found.size(), size_t(1));
    if (!found.empty()) CHECK_EQ(found[0].index, size_t(2));

    ModPlace whole, member, other, cls;
    whole.cls = member.cls = other.cls = cls.cls = StringHash32("vehicle");
    whole.key = member.key = other.key = StringHash32("default");
    whole.field = member.field = other.field = StringHash32("TIRES");
    member.steps.push_back({"GRIP_SCALE", -1});
    other.steps.push_back({"STATIC_GRIP", -1});
    CHECK(Clash(whole, member));
    CHECK(!Clash(member, other));
    cls.kind = ModPlace::Kind::Class;
    CHECK(Clash(cls, member));
    ModPlace parent = member;
    parent.kind = ModPlace::Kind::Parent;
    CHECK(!Clash(parent, member));
    parent.kind = ModPlace::Kind::Collection;
    CHECK(Clash(parent, member));
}

TEST("mod traces: uninstalling a mod a later one changed too does the later one again, and traces it again") {
    auto db = LoadProStreet();
    auto base = sc::CloneDatabase(*db);
    const uint32_t shortWas = ValueOf(*db, "audio_revlimiter", "default", "total_frames_short");
    const uint32_t longWas = ValueOf(*db, "audio_revlimiter", "default", "total_frames_long");
    const TracedMod a = InstallTraced(*db, "A", "attribulina 2\nedit audio_revlimiter/default { total_frames_short = 77\n total_frames_long = 66 }\n");
    const TracedMod b = InstallTraced(*db, "B", "attribulina 2\nedit audio_revlimiter/default { total_frames_short = 55 }\n"
                                                "edit pvehicle/street { HornType = 3 }\n");
    const TracedMod apart = InstallTraced(*db, "Apart", "attribulina 2\nedit pvehicle/default { HornType = 2 }\n");
    std::vector<TracedMod> mods{a, b, apart};
    UninstallResult r = Uninstall(*db, mods, {0});
    if (!r.ok) std::fprintf(stderr, "%s\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(r.redone);
    CHECK_EQ(r.retraced.size(), size_t(2)); // B and the one after it
    CHECK_EQ(ValueOf(*db, "audio_revlimiter", "default", "total_frames_short"), 55u); // B's stays
    CHECK_EQ(ValueOf(*db, "audio_revlimiter", "default", "total_frames_long"), longWas);
    CHECK_EQ(ValueOf(*db, "pvehicle", "street", "HornType"), 3u);
    CHECK_EQ(ValueOf(*db, "pvehicle", "default", "HornType"), 2u);
    // B's new trace sets the value back to what it was before A.
    for (const auto& [i, trace] : r.retraced) mods[i].trace = trace;
    const std::string bText = sc::Print(*mods[1].trace);
    CHECK(bText.find("total_frames_short = " + std::to_string(shortWas)) != std::string::npos);
    mods.erase(mods.begin());
    r = Uninstall(*db, mods, {0, 1});
    CHECK(r.ok);
    CHECK(!r.redone);
    CHECK_EQ(Changes(sc::Diff(*base, *db)), size_t(0));
}

TEST("mod traces: a mod that builds on another keeps it installed; a value changed since is set back with a note") {
    auto db = LoadProStreet();
    const TracedMod a = InstallTraced(*db, "Maker", "attribulina 2\ncreate pvehicle/trace_car from pvehicle/street\n"
                                                    "edit audio_revlimiter/default { total_frames_short = 77 }\n");
    const TracedMod b = InstallTraced(*db, "User", "attribulina 2\nedit pvehicle/trace_car { HornType = 3 }\n");
    auto before = sc::CloneDatabase(*db);
    UninstallResult r = Uninstall(*db, {a, b}, {0});
    CHECK(!r.ok);
    CHECK(r.error.find("User builds on what Maker changed") == 0);
    // Without the later one: the value changed by hand goes back, with a note.
    db = std::move(before);
    CHECK(sc::Apply(*db, sc::Parse("attribulina 1\nedit audio_revlimiter/default { total_frames_short = 12 }\n", "hand.amod")).ok);
    TracedMod none;
    none.mod.title = "Old";
    none.problem = "what it changed wasn't kept when it was installed (it has no trace)";
    r = Uninstall(*db, {a, b, none}, {0, 1});
    CHECK(r.ok);
    bool noted = false, unknown = false;
    for (const std::string& n : r.notes) {
        noted |= n.find("Maker: 1 value isn't what it set") == 0;
        unknown |= n.find("Old was installed after Maker and User") == 0;
    }
    CHECK(noted);
    CHECK(unknown);
    CHECK(!db->findCollection(StringHash32("pvehicle"), StringHash32("trace_car")));
    // One without a trace can't be uninstalled.
    r = Uninstall(*db, {a, b, none}, {2});
    CHECK(!r.ok);
    CHECK(r.error.find("Old can't be uninstalled: what it changed wasn't kept") == 0);
}

TEST("mod traces: uninstalling a mod takes the ones that build on it, and theirs, as a package manager does; the others stay") {
    auto db = LoadProStreet();
    auto base = sc::CloneDatabase(*db);
    const TracedMod maker = InstallTraced(*db, "Maker", "attribulina 2\ncreate pvehicle/trace_car from pvehicle/street\n"
                                                        "edit audio_revlimiter/default { total_frames_short = 77 }\n");
    const TracedMod user = InstallTraced(*db, "User", "attribulina 2\nedit pvehicle/trace_car { HornType = 3 }\n");
    // A collection under the one Maker made: no value in common, but it can't stay without it.
    const TracedMod kid = InstallTraced(*db, "Kid", "attribulina 2\ncreate pvehicle/trace_kid under pvehicle/trace_car\n");
    const TracedMod grandkid = InstallTraced(*db, "Grandkid", "attribulina 2\nedit pvehicle/trace_kid { HornType = 2 }\n");
    const TracedMod apart = InstallTraced(*db, "Apart", "attribulina 2\nedit pvehicle/default { HornType = 2 }\n"
                                                        "edit audio_revlimiter/default { total_frames_long = 5 }\n");
    const std::vector<TracedMod> mods{maker, user, kid, grandkid, apart};
    const std::map<std::string, Bytes> installed = PackBytes(*db);

    const UninstallPlan plan = PlanUninstall(*db, mods, {0});
    if (!plan.ok()) std::fprintf(stderr, "%s\n", plan.error.c_str());
    CHECK(plan.ok());
    CHECK(plan.asked == std::vector<size_t>{0});
    CHECK((plan.which == std::vector<size_t>{0, 1, 2, 3}));
    std::vector<size_t> deps;
    for (const auto& [i, why] : plan.dependents) {
        deps.push_back(i);
        CHECK(!why.empty());
    }
    CHECK((deps == std::vector<size_t>{1, 2, 3}));
    const std::string text = DependentsText(plan, mods);
    if (text.find("User, Kid and Grandkid build on what Maker changed, so they are uninstalled with it: User (") != 0)
        std::fprintf(stderr, "%s\n", text.c_str());
    CHECK(text.find("User, Kid and Grandkid build on what Maker changed, so they are uninstalled with it: User (") == 0);
    // Planning changes nothing.
    CHECK(PackBytes(*db) == installed);

    const UninstallResult r = Uninstall(*db, mods, plan.which, {}, plan.around);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(!db->findCollection(StringHash32("pvehicle"), StringHash32("trace_car")));
    CHECK(!db->findCollection(StringHash32("pvehicle"), StringHash32("trace_kid")));
    // The one that builds on nothing of theirs stays, done again if it had to be.
    CHECK_EQ(ValueOf(*db, "pvehicle", "default", "HornType"), 2u);
    CHECK_EQ(ValueOf(*db, "audio_revlimiter", "default", "total_frames_long"), 5u);
    std::vector<TracedMod> left{apart};
    for (const auto& [i, trace] : r.retraced)
        if (i == 4) left[0].trace = trace;
    CHECK(Uninstall(*db, left, {0}).ok);
    CHECK_EQ(Changes(sc::Diff(*base, *db)), size_t(0));

    // Asked for one that nothing builds on: no more than it.
    auto again = sc::CloneDatabase(*base);
    const TracedMod a = InstallTraced(*again, "A", "attribulina 2\nedit pvehicle/default { HornType = 2 }\n");
    const TracedMod b = InstallTraced(*again, "B", "attribulina 2\nedit pvehicle/default { HornType = 3 }\n");
    const UninstallPlan only = PlanUninstall(*again, {a, b}, {0});
    CHECK(only.ok());
    CHECK((only.which == std::vector<size_t>{0}));
    CHECK(only.dependents.empty());
    CHECK(DependentsText(only, {a, b}).empty());
}

TEST("mod traces: the list's traces are written beside it, follow its entries and go with them") {
    const fs::path dir = fs::temp_directory_path() / "attribulina_mod_traces";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto db = LoadProStreet();
    TracedMod a = InstallTraced(*db, "A: the \"first\"", "attribulina 2\nedit audio_revlimiter/default { total_frames_short = 77 }\n");
    a.mod.version = "1.0";
    TracedMod b = InstallTraced(*db, "B", "attribulina 2\nedit audio_revlimiter/default { total_frames_short = 55 }\n");
    ModListUpdate u;
    u.add.push_back({a.mod, a.trace});
    u.add.push_back({b.mod, b.trace});
    u.add.push_back({b.mod, nullptr}); // one without
    std::vector<std::string> problems;
    ModList list = UpdateModList(dir.string(), u, &problems);
    CHECK(problems.empty());
    CHECK_EQ(list.mods.size(), size_t(3));
    CHECK_EQ(list.mods[0].trace, std::string("attribulina_traces\\2026-11-15 100000 A_ the _first_ 1.0.amod"));
    CHECK_EQ(list.mods[1].trace, std::string("attribulina_traces\\2026-11-15 100000 B.amod"));
    CHECK(list.mods[2].trace.empty());
    CHECK(fs::exists(dir / kTraceFolder / "2026-11-15 100000 A_ the _first_ 1.0.amod"));
    auto read = ReadModList(dir.string());
    CHECK(read && read->mods.size() == 3 && read->mods[0].trace == list.mods[0].trace);
    std::vector<TracedMod> rows = ReadTraces(dir.string(), *read);
    CHECK(rows[0].trace && rows[1].trace && !rows[2].trace);
    CHECK_EQ(sc::Print(*rows[1].trace), sc::Print(*b.trace));
    CHECK(rows[2].problem.find("no trace") != std::string::npos);

    // In the editor: one more applied, not saved yet, then A uninstalled.
    ModListUpdate pending;
    TracedMod c = InstallTraced(*db, "C", "attribulina 2\nedit audio_revlimiter/default { total_frames_short = 33 }\n");
    pending.add.push_back({c.mod, c.trace});
    rows = WithUpdate(rows, pending);
    CHECK_EQ(rows.size(), size_t(4));
    CHECK(rows[3].pending());
    UninstallResult r = Uninstall(*db, rows, {0});
    CHECK(r.ok);
    CHECK(r.redone);
    FoldUninstall(pending, rows, r);
    CHECK_EQ(pending.remove.size(), size_t(1));
    CHECK_EQ(pending.retrace.size(), size_t(1)); // B, on the file
    CHECK_EQ(pending.add.size(), size_t(1));     // C, with its new trace
    CHECK(pending.add[0].trace != c.trace);
    rows = WithUpdate(ReadTraces(dir.string(), *read), pending);
    CHECK_EQ(rows.size(), size_t(3));
    CHECK_EQ(rows[0].mod.title, std::string("B"));
    // Uninstalling the one not saved yet takes it out of what saving adds.
    r = Uninstall(*db, rows, {2});
    CHECK(r.ok);
    FoldUninstall(pending, rows, r);
    CHECK(pending.add.empty());
    list = UpdateModList(dir.string(), pending, &problems);
    CHECK(problems.empty());
    CHECK_EQ(list.mods.size(), size_t(2));
    CHECK(!fs::exists(dir / kTraceFolder / "2026-11-15 100000 A_ the _first_ 1.0.amod"));
    rows = ReadTraces(dir.string(), list);
    CHECK(rows[0].trace);
    if (rows[0].trace) CHECK(sc::Print(*rows[0].trace) != sc::Print(*b.trace)); // rewritten
    fs::remove_all(dir);
}



TEST("mod traces: a line setting the value it checks (an earlier build's `PartDetails = null was null`) is left out, and the mod uninstalls") {
    // Builds before 2026-11-15 read an FEPartDetail of files NFS-VltEd saved as the
    // empty camera list's block that points at it too, and traced it so: `was`
    // can't check a pointer, and the user's uninstall stopped there.
    const std::string vlted = test::DataPath("attrib/databases/pc/prostreet-vlted/");
    if (!fs::exists(vlted + "FE_ATTRIB.BIN")) return;
    const fs::path dir = fs::temp_directory_path() / "attribulina_mod_traces_unchanged";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto db = std::make_unique<Database>();
    db->addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
    db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    for (const char* f : {"ATTRIBUTES.BIN", "FE_ATTRIB.BIN"}) db->loadPack(vlted + f);
    const std::map<std::string, Bytes> original = PackBytes(*db);
    TracedMod m = InstallTraced(*db, "Prices", "attribulina 2\nedit part_fedata/generic_wheel_group_03 { partData[1].price = 1500 }\n");
    std::string text = sc::Print(*m.trace);
    CHECK(text.find("PartDetails") == std::string::npos); // this build traces only the price
    const size_t at = text.find("    partData[1].price = ");
    CHECK(at != std::string::npos);
    if (at == std::string::npos) return;
    text.insert(text.find('\n', at) + 1, "    partData[1].PartDetails = null was null\n");
    m.trace = std::make_shared<const sc::Script>(sc::Parse(text, "trace.amod"));
    {
        // As written, the line stops the uninstall.
        auto copy = sc::CloneDatabase(*db);
        const UninstallResult r = Uninstall(*copy, {m}, {0});
        CHECK(!r.ok && r.error.find("was checks a single value") != std::string::npos);
    }
    ModListUpdate u;
    u.add.push_back({m.mod, m.trace});
    std::vector<std::string> problems;
    const ModList list = UpdateModList(dir.string(), u, &problems);
    CHECK(problems.empty());
    const std::vector<TracedMod> rows = ReadTraces(dir.string(), list);
    CHECK(rows.size() == 1 && rows[0].trace);
    if (rows.size() != 1 || !rows[0].trace) return;
    const std::string read = sc::Print(*rows[0].trace);
    CHECK(read.find("PartDetails") == std::string::npos && read.find("partData[1].price = ") != std::string::npos);
    const UninstallResult r = Uninstall(*db, rows, {0});
    if (!r.ok) std::fprintf(stderr, "%s\n", r.error.c_str());
    CHECK(r.ok);
    const std::map<std::string, Bytes> now = PackBytes(*db);
    for (const auto& [path, bytes] : original) CHECK(now.count(path) && now.at(path) == bytes);
    fs::remove_all(dir);
}
