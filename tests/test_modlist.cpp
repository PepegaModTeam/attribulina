// The list of installed mods (core/ModList.h, attribulina_mods.txt), what
// names a mod (its title and version, else its file's SHA-256), and an nfsms
// script's # amod block, which gives it a title, author, version and game in
// comments NFS-VltEd skips.

#include "Test.h"

#include "core/Hash.h"
#include "core/Install.h"
#include "core/ModList.h"
#include "core/Nfsms.h"

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
