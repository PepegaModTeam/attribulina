// The mod installer (src/installer) and what it shares with attribcli
// (core/Install.h): finding a game's database, reading mods, and installing
// them through the installer's steps into a copy of Pro Street's GLOBAL
// folder with its memory files, with the timestamped log it writes.

#include "Test.h"
#include "UiProbe.h"

#include "cli/Cli.h"
#include "core/Hash.h"
#include "core/Install.h"
#include "core/MemoryFile.h"
#include "core/ModList.h"
#include "core/Script.h"
#include "installer/Installer.h"
#include "platform/headless/HeadlessHost.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace attrib;
using installer::Outcome;
using installer::Step;

void ProbeBefore();
void ProbeAfter();

namespace {

void WriteText(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

std::string ReadText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// A game's folder: GLOBAL with Pro Street's ATTRIBUTES.BIN and FE_ATTRIB.BIN
// and its memory files. Returns the folder holding it, "game" and the rest.
fs::path MakeGame(const std::string& name) {
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    const fs::path global = root / "game" / "GLOBAL";
    fs::create_directories(global);
    const fs::path vanilla = test::DataPath("attrib/databases/pc/prostreet-vanilla");
    for (const char* f : {"ATTRIBUTES.BIN", "FE_ATTRIB.BIN"}) fs::copy_file(vanilla / f, global / f);
    for (const auto& e : fs::directory_iterator(test::DataPath("attrib/memoryfiles/pc/prostreet")))
        fs::copy_file(e.path(), global / e.path().filename());
    fs::create_directories(root / "user");
    return root;
}

installer::Paths PathsIn(const fs::path& root) {
    installer::Paths p;
    p.exeDir = test::Root();
    p.dataDir = test::DataPath("data");
    p.userDir = (root / "user").string();
    p.logDirs = {(root / "logs").string()};
    return p;
}

// A value as attribcli's get prints it.
std::string Get(const fs::path& pack, const std::string& collection, const std::string& field) {
    cli::Env env;
    env.dataDir = test::DataPath("data");
    std::ostringstream out, err;
    cli::Run({"get", pack.string(), collection, field}, out, err, env);
    return out.str();
}

// The installer and its window, as a user drives it.
struct Session {
    platform::HeadlessImGui imgui;
    platform::HeadlessHost host;
    installer::Installer inst{host};

    explicit Session(const installer::Paths& paths, bool background = false) {
        imgui.beforeFrame = ProbeBefore;
        imgui.afterFrame = ProbeAfter;
        test::UiProbe::get().conflicts.clear();
        inst.setBackground(background);
        inst.setup(1.0f, paths);
        frames(2);
    }
    void frames(int n) {
        imgui.run([&]() { inst.frame(); }, n);
    }
    // Clicks the item drawn last frame with this label; false when none was.
    bool click(const std::string& label) {
        const test::ProbeItem* it = test::UiProbe::get().find(label);
        if (!it) return false;
        imgui.click([&]() { inst.frame(); }, (it->rect.Min.x + it->rect.Max.x) * 0.5f, (it->rect.Min.y + it->rect.Max.y) * 0.5f);
        frames(1);
        return true;
    }
    // In the window whose name holds `window`.
    bool clickIn(const std::string& label, const std::string& window) {
        const test::ProbeItem* it = test::UiProbe::get().find(label, window);
        if (!it) return false;
        imgui.click([&]() { inst.frame(); }, (it->rect.Min.x + it->rect.Max.x) * 0.5f, (it->rect.Min.y + it->rect.Max.y) * 0.5f);
        frames(1);
        return true;
    }
    bool drawn(const std::string& label) const { return test::UiProbe::get().find(label) != nullptr; }
    // Drawn whole inside the window, not cut off at its bottom.
    bool onScreen(const std::string& label) const {
        const test::ProbeItem* it = test::UiProbe::get().find(label);
        return it && it->rect.Min.y >= 0 && it->rect.Max.y <= ImGui::GetIO().DisplaySize.y - ImGui::GetStyle().WindowPadding.y + 1;
    }
    std::string logText() const {
        std::string t;
        for (const installer::LogLine& l : inst.log()) t += l.text + "\n";
        return t;
    }
};

std::vector<fs::path> LogFiles(const fs::path& root) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root / "logs", ec)) v.push_back(e.path());
    return v;
}

} // namespace

TEST("install: a game's database is in its GLOBAL folder, in any case; the game's folder, GLOBAL or attributes.bin name it") {
    const fs::path root = fs::temp_directory_path() / "attribulina_install_find";
    fs::remove_all(root);
    const fs::path game = root / "NFS Game", global = game / "Global";
    fs::create_directories(global);
    WriteText(global / "attributes.bin", "");
    for (const std::string& chosen : {game.string(), game.string() + "/", global.string(), (global / "attributes.bin").string()}) {
        const auto g = install::FindGameFolder(chosen);
        CHECK(g);
        if (!g) continue;
        CHECK_EQ(fs::path(g->game), game);
        CHECK_EQ(fs::path(g->global), global);
        CHECK_EQ(fs::path(g->database), global / "attributes.bin");
    }
    std::string why;
    CHECK(!install::FindGameFolder(root.string(), &why));
    CHECK(why.find("has no GLOBAL folder") != std::string::npos);
    fs::create_directories(root / "Other" / "GLOBAL");
    CHECK(!install::FindGameFolder((root / "Other").string(), &why));
    CHECK(why.find("has no attributes.bin") != std::string::npos);
    CHECK(!install::FindGameFolder((root / "missing").string(), &why));
    CHECK(why.find("There is no folder") != std::string::npos);
    fs::remove_all(root);
}

TEST("install: a mod's title, options and setup are read; one for another game is refused before it runs") {
    const fs::path root = fs::temp_directory_path() / "attribulina_install_mods";
    fs::remove_all(root);
    fs::create_directories(root);
    WriteText(root / "louder.amod", "attribulina 1\ngame carbon_pc\ntitle \"Louder\"\nauthor \"Someone\"\nversion \"1.2\"\n"
                                    "description \"Makes the rev limiter louder.\"\n"
                                    "option frames integer \"Frames\" default 5 min 1 max 50\n"
                                    "edit audio_revlimiter/default { total_frames_short = $frames }\n");
    Database ps;
    ps.setTarget({Game::ProStreet, Platform::PC});
    const install::Mod amod = install::ReadMod((root / "louder.amod").string());
    CHECK(!amod.isNfsms);
    CHECK_EQ(amod.title, std::string("Louder"));
    CHECK_EQ(amod.author, std::string("Someone"));
    CHECK_EQ(amod.version, std::string("1.2"));
    CHECK_EQ(amod.description, std::string("Makes the rev limiter louder."));
    CHECK(amod.hasOptions());
    CHECK_EQ(install::ModProblem(amod, ps), std::string("This mod is for Carbon, and the folder holds Pro Street."));

    // The game on another platform is a warning only: values go in by their fields' types.
    WriteText(root / "ps2.amod", "attribulina 1\ngame prostreet_ps2\nedit audio_revlimiter/default { total_frames_short = 9 }\n");
    const install::Mod ps2 = install::ReadMod((root / "ps2.amod").string());
    CHECK(install::ModProblem(ps2, ps).empty());
    CHECK(install::ModWarning(ps2, ps).find("written for Pro Street on PS2") != std::string::npos);
    CHECK(!ps2.hasOptions());
    CHECK_EQ(ps2.title, std::string("ps2")); // no title line: the file's name

    WriteText(root / "Loud Mod.nfsms", "version 4.6\ngame C\nui_text Makes the rev limiter loud.\n"
                                       "ui_control checkbox loud false \"Loud\"\n"
                                       "ui_option loud\nupdate_field audio_revlimiter default total_frames_long 99\nui_option loud\n");
    install::Mod ns = install::ReadMod((root / "Loud Mod.nfsms").string());
    CHECK(ns.isNfsms);
    CHECK_EQ(ns.title, std::string("Loud Mod"));
    CHECK_EQ(ns.description, std::string("Makes the rev limiter loud."));
    CHECK(ns.hasOptions());
    CHECK_EQ(install::ModProblem(ns, ps), std::string("This mod is for Carbon, and the folder holds Pro Street."));
    WriteText(root / "ps.nfsms", "version 4.6\ngame PS C\nupdate_field audio_revlimiter default total_frames_long 99\n");
    CHECK(install::ModProblem(install::ReadMod((root / "ps.nfsms").string()), ps).empty());
    // NFS-VltEd never read Nitro.
    Database nitro;
    nitro.setTarget({Game::Nitro, Platform::Wii});
    CHECK(!install::ModProblem(install::ReadMod((root / "ps.nfsms").string()), nitro).empty());

    WriteText(root / "broken.amod", "attribulina 1\nedit audio_revlimiter/default {\n");
    bool threw = false;
    try {
        install::ReadMod((root / "broken.amod").string());
    } catch (const script::ScriptError&) {
        threw = true;
    }
    CHECK(threw);
    fs::remove_all(root);
}

TEST("installer: a mod script installs through the steps: its options, the packs it changes kept as .bak, the memory file's copy, the log") {
    const fs::path root = MakeGame("attribulina_installer_amod");
    const fs::path game = root / "game", global = game / "GLOBAL";
    WriteText(root / "louder.amod", "attribulina 1\ngame prostreet_pc\ntitle \"Louder\"\n"
                                    "option frames integer \"Frames\" default 5 min 1 max 50\n"
                                    "edit audio_revlimiter/default { total_frames_short = $frames }\n");
    const Bytes attributes = ReadFile((global / "ATTRIBUTES.BIN").string());
    const Bytes fe = ReadFile((global / "FE_ATTRIB.BIN").string());
    const Bytes inGame = ReadFile((global / "InGameMemoryFile.bin").string());
    {
        Session s(PathsIn(root));
        CHECK(s.inst.step() == Step::Mod);
        CHECK(s.inst.status() == "Choose the mod to install and the game's folder.");
        CHECK(!s.inst.canGoTo(Step::Options));
        CHECK(!s.inst.logFilePath().empty());
        CHECK(s.drawn("1   Mod and Game"));

        // Browse... gives the mod, and the game's folder is chosen on the same page.
        s.host.nextOpenFile = (root / "louder.amod").string();
        CHECK(s.click("Browse...##mod"));
        CHECK(s.inst.mod());
        CHECK(s.inst.step() == Step::Mod);
        CHECK(!s.inst.canGoTo(Step::Options));
        CHECK(s.host.title == "Attribulina Mod Installer - Louder");
        s.host.nextOpenFolder = game.string();
        CHECK(s.click("Browse...##game"));
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        CHECK(s.inst.game().target == (Target{Game::ProStreet, Platform::PC}));
        CHECK(s.inst.game().detection.certain);
        CHECK(std::find(s.inst.game().copies.begin(), s.inst.game().copies.end(),
                        std::string("GlobalMemoryFile.bin holds a copy of GLOBAL\\ATTRIBUTES.BIN")) != s.inst.game().copies.end());
        CHECK(s.inst.modProblem().empty());
        // Another game and platform picked from the list reads the files again
        // as that one (the folder passed is the installer's own, cleared as it starts).
        s.inst.chooseGame(s.inst.game().folder.game, Target{Game::ProStreet, Platform::Xbox360});
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        CHECK(s.inst.game().chosen);
        CHECK(s.inst.game().target == (Target{Game::ProStreet, Platform::Xbox360}));
        CHECK_EQ(fs::path(s.inst.game().folder.game), game);
        s.frames(1); // draws the list of games and platforms (a combo, which the probe can't name)
        s.inst.chooseGame(game.string(), Target{Game::ProStreet, Platform::PC});
        s.frames(1);
        CHECK(s.inst.game().target == (Target{Game::ProStreet, Platform::PC}));

        CHECK(s.click("Next >"));
        CHECK(s.inst.step() == Step::Options);
        CHECK_EQ(s.inst.options().at("frames"), std::string("5"));
        s.inst.options()["frames"] = "7";
        s.frames(1);
        CHECK(s.click("Next >"));
        CHECK(s.inst.step() == Step::Install);
        CHECK(s.onScreen("Install"));
        CHECK(s.onScreen("Show the Log"));
        CHECK(s.inst.outcome() == Outcome::None);
        CHECK(s.click("Install"));
        s.inst.wait();
        s.frames(2);
        CHECK(s.inst.outcome() == Outcome::Succeeded);
        CHECK_EQ(s.inst.result().headline, std::string("The mod is installed."));
        CHECK(s.inst.statusLevel() == installer::Level::Success);
        CHECK(s.inst.status() == "Installation successful.");
        CHECK(s.drawn("Install Another Mod"));
        CHECK(s.drawn("Close"));

        // The Log, with how it went at the end.
        CHECK(s.click("Show the Log"));
        s.frames(2);
        CHECK(s.drawn("Copy All"));
        CHECK(s.onScreen("Close"));
        CHECK(s.onScreen("Show the Log"));
        CHECK(s.inst.log().back().level == installer::Level::Success);
        const std::string last = s.inst.log().back().text; // log() is a copy: keep the line, not a reference into it
        CHECK_EQ(last, std::string("Installation successful: The mod is installed."));
        CHECK(s.click("Open the Log File"));
        CHECK(s.host.openedPaths == std::vector<std::string>{s.inst.logFilePath()});
        CHECK(s.click("Close"));
        CHECK(s.host.quitRequested);
        for (const auto& c : test::UiProbe::get().conflicts) std::printf("        ID conflict: %s\n", c.c_str());
        CHECK(test::UiProbe::get().conflicts.empty());
    }
    // The pack it changed, its original as .bak, and the memory file's copy of it.
    const Bytes now = ReadFile((global / "ATTRIBUTES.BIN").string());
    CHECK(now != attributes);
    CHECK(ReadFile((global / "ATTRIBUTES.BIN.bak").string()) == attributes);
    CHECK(Get(global / "ATTRIBUTES.BIN", "audio_revlimiter/default", "total_frames_short").find("total_frames_short = 7") != std::string::npos);
    const auto memory = ReadMemoryFile(ReadFile((global / "GlobalMemoryFile.bin").string()));
    CHECK(memory && memory->find(GameFileHash("GLOBAL\\ATTRIBUTES.BIN"))->bytes == now);
    CHECK(fs::exists(global / "GlobalMemoryFile.bin.bak"));
    // Nothing else.
    CHECK(ReadFile((global / "FE_ATTRIB.BIN").string()) == fe);
    CHECK(!fs::exists(global / "FE_ATTRIB.BIN.bak"));
    CHECK(ReadFile((global / "InGameMemoryFile.bin").string()) == inGame);
    // One timestamped log, written as it went.
    const std::vector<fs::path> logs = LogFiles(root);
    CHECK_EQ(logs.size(), size_t(1));
    if (!logs.empty()) {
        CHECK(logs[0].filename().string().rfind("AttribulinaModInstaller_", 0) == 0);
        const std::string log = ReadText(logs[0]);
        CHECK(log.rfind("Attribulina Mod Installer, ", 0) == 0);
        CHECK(log.find("option frames = 7\n") != std::string::npos);
        CHECK(log.find("saved " + (global / "ATTRIBUTES.BIN").string()) != std::string::npos);
        CHECK(log.find("updated " + (global / "GlobalMemoryFile.bin").string() + ": its copy of GLOBAL\\ATTRIBUTES.BIN") != std::string::npos);
        CHECK(log.find("Installation successful: The mod is installed.\n") != std::string::npos);
    }
    // The game's folder is remembered.
    CHECK(ReadText(root / "user" / installer::kSettingsFileName).find("GameFolder=" + game.string() + "\n") != std::string::npos);
    fs::remove_all(root);
}

TEST("installer: an nfsms mod runs NFS-VltEd compatible with the setup's choices; a failing line leaves it installed in part; file_copy copies into the game") {
    const fs::path root = MakeGame("attribulina_installer_nfsms");
    const fs::path game = root / "game", global = game / "GLOBAL", mod = root / "Loud Mod";
    fs::create_directories(mod);
    WriteText(mod / "readme.txt", "Loud.\n");
    WriteText(mod / "Loud Mod.nfsms", "version 4.6\ngame PS\nui_text Makes the rev limiter loud.\n"
                                      "ui_control checkbox loud false \"Loud\"\n"
                                      "update_field audio_revlimiter default total_frames_short 21\n"
                                      "ui_option loud\nupdate_field audio_revlimiter default total_frames_long 99\nui_option loud\n"
                                      "update_field audio_revlimiter no_such_node total_frames_long 1\n"
                                      "file_copy \"readme.txt\" \"Loud Mod readme.txt\"\n"
                                      "# amod begin\n# title \"Loud\"\n# version \"2\"\n# amod end\n");
    {
        // In the background, as the program runs it: the command line's game folder and mod.
        Session s(PathsIn(root), true);
        s.inst.openArguments({game.string(), (mod / "Loud Mod.nfsms").string()});
        CHECK(s.inst.step() == Step::Mod);
        CHECK(s.inst.mod() && s.inst.mod()->isNfsms);
        s.frames(1);
        s.inst.wait();
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        CHECK(s.click("Next >"));
        CHECK(s.inst.step() == Step::Options);
        CHECK(s.drawn("Loud"));
        CHECK(!s.inst.choices().checkboxes["loud"]);
        CHECK(s.click("Loud"));
        CHECK(s.inst.choices().checkboxes["loud"]);
        CHECK(s.click("Next >"));
        CHECK(s.click("Install"));
        // Closing waits for the installation.
        if (s.inst.busy()) {
            CHECK(!s.inst.requestClose());
            s.frames(1);
        }
        s.inst.wait();
        s.frames(1);
        CHECK(s.inst.requestClose());
        CHECK(s.inst.outcome() == Outcome::Partly);
        CHECK_EQ(s.inst.result().headline, std::string("The mod is installed, but 1 line of its script failed."));
        CHECK(s.inst.status() == "Installation finished with errors.");
        CHECK(s.logText().find("Loud Mod.nfsms:9:") != std::string::npos); // the line that failed
        CHECK(s.inst.log().back().level == installer::Level::Warning);
        CHECK(test::UiProbe::get().conflicts.empty());
    }
    CHECK(Get(global / "ATTRIBUTES.BIN", "audio_revlimiter/default", "total_frames_short").find("total_frames_short = 21") != std::string::npos);
    CHECK(Get(global / "ATTRIBUTES.BIN", "audio_revlimiter/default", "total_frames_long").find("total_frames_long = 99") != std::string::npos);
    CHECK_EQ(ReadText(game / "Loud Mod readme.txt"), std::string("Loud.\n"));
    // On the game's list by the title and version its # amod block gives, with the files it wrote.
    const auto list = ReadModList(global.string());
    CHECK(list && list->mods.size() == 1);
    if (list && !list->mods.empty()) {
        const InstalledMod& m = list->mods[0];
        CHECK_EQ(m.title, std::string("Loud"));
        CHECK_EQ(m.version, std::string("2"));
        CHECK(m.nfsms);
        CHECK_EQ(m.result, std::string("installed, 1 line failed"));
        CHECK(std::find(m.options.begin(), m.options.end(), std::make_pair(std::string("loud"), std::string("on"))) != m.options.end());
        for (const char* f : {"Loud Mod readme.txt", "GLOBAL\\ATTRIBUTES.BIN", "GLOBAL\\GlobalMemoryFile.bin"})
            CHECK(std::find(m.files.begin(), m.files.end(), std::string(f)) != m.files.end());
    }
    const std::vector<fs::path> logs = LogFiles(root);
    CHECK_EQ(logs.size(), size_t(1));
    if (!logs.empty()) {
        const std::string log = ReadText(logs[0]);
        CHECK(log.find("Installation finished with errors: The mod is installed, but 1 line of its script failed.\n") != std::string::npos);
        // A script's message names its level once.
        CHECK(log.find(" Loud Mod.nfsms:9:1: error: class 'audio_revlimiter' does not contain node 'no_such_node'\n") != std::string::npos);
        CHECK(log.find("error: Loud Mod.nfsms") == std::string::npos);
    }
    fs::remove_all(root);
}

TEST("installer: Advanced Options' NFS-VltEd compatible switch is on by default, kept in mod_installer.ini, and an nfsms mod reads by it") {
    const fs::path root = MakeGame("attribulina_installer_vlted");
    const fs::path game = root / "game", global = game / "GLOBAL", mod = root / "Kit.nfsms";
    // mKitNumber is our member's name: NFS-VltEd has KitNumber only.
    WriteText(mod, "version 4.6\ngame PS\nupdate_field presetride player_d_day Parts[7] mKitNumber 4\n"
                   "# amod begin\n# title \"Kit\"\n# amod end\n");
    auto install = [&](Session& s) {
        s.inst.openArguments({game.string(), mod.string()});
        s.frames(1);
        s.inst.wait();
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        if (!s.inst.installedNote().empty()) s.inst.installAnyway() = true;
        CHECK(s.click("Next >"));
        CHECK(s.inst.step() == Step::Options);
        CHECK(s.click("Next >"));
        CHECK(s.click("Install"));
        s.inst.wait();
        s.frames(1);
    };
    {
        // On: the line fails, as in NFS-VltEd.
        Session s(PathsIn(root));
        CHECK(s.inst.vltEd());
        install(s);
        CHECK(s.inst.outcome() == Outcome::Partly);
        CHECK(s.logText().find("  read as NFS-VltEd 4.6 reads it (NFS-VltEd compatible)\n") != std::string::npos);
        CHECK(s.logText().find("Kit.nfsms:3:") != std::string::npos);
    }
    CHECK(Get(global / "ATTRIBUTES.BIN", "presetride/player_d_day", "Parts").find("mKitNumber = 4") == std::string::npos);
    {
        // Off in Advanced Options, before a mod is chosen: the editor's own reading takes our member names.
        Session s(PathsIn(root));
        CHECK(s.click("Advanced Options"));
        CHECK(s.drawn("NFS-VltEd compatible"));
        CHECK(s.click("NFS-VltEd compatible"));
        CHECK(!s.inst.vltEd());
        CHECK(ReadText(root / "user" / installer::kSettingsFileName).find("NfsmsVltEd=0\n") != std::string::npos);
        CHECK(s.inst.trackMods()); // the other switch stays as it was
        s.imgui.key([&]() { s.inst.frame(); }, ImGuiKey_Escape); // closes the popup
        s.frames(1);
        install(s);
        CHECK(s.inst.outcome() == Outcome::Succeeded);
        CHECK(s.logText().find("  read the editor's own way (NFS-VltEd compatible is off)\n") != std::string::npos);
        CHECK(test::UiProbe::get().conflicts.empty());
    }
    CHECK(Get(global / "ATTRIBUTES.BIN", "presetride/player_d_day", "Parts").find("mKitNumber = 4") != std::string::npos);
    {
        // The next run starts with it off, and turning it on again is kept.
        Session s(PathsIn(root));
        CHECK(!s.inst.vltEd());
        s.inst.setVltEd(true);
        CHECK(ReadText(root / "user" / installer::kSettingsFileName).find("NfsmsVltEd=1\n") != std::string::npos);
    }
    fs::remove_all(root);
}

TEST("installer: a mod script that stops saves nothing; a mod for another game can't be installed; the folder chosen before loads with the next mod") {
    const fs::path root = MakeGame("attribulina_installer_fail");
    const fs::path game = root / "game", global = game / "GLOBAL";
    WriteText(root / "stops.amod", "attribulina 1\ngame prostreet_pc\nedit audio_revlimiter/default { total_frames_short = 9 }\n"
                                   "edit pvehicle/no_such_car { HornType = 2 }\n");
    WriteText(root / "carbon.amod", "attribulina 1\ngame carbon_pc\nedit audio_revlimiter/default { total_frames_short = 9 }\n");
    const Bytes attributes = ReadFile((global / "ATTRIBUTES.BIN").string());
    {
        Session s(PathsIn(root));
        CHECK(s.inst.openMod((root / "stops.amod").string()));
        s.inst.chooseGame(game.string());
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        s.inst.goTo(Step::Install);
        CHECK(s.inst.step() == Step::Install);
        s.inst.install();
        s.inst.wait();
        s.frames(1);
        CHECK(s.inst.outcome() == Outcome::Failed);
        CHECK_EQ(s.inst.result().headline, std::string("The mod wasn't installed."));
        CHECK(s.inst.result().detail.find("stops.amod:4:") != std::string::npos);
        CHECK(s.inst.result().detail.find("Nothing was saved") != std::string::npos);
        CHECK(s.inst.status() == "Installation failed.");

        // Another game's mod: the steps after the first are closed.
        s.inst.installAnother();
        CHECK(s.inst.step() == Step::Mod);
        CHECK(s.inst.openMod((root / "carbon.amod").string()));
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        CHECK_EQ(s.inst.modProblem(), std::string("This mod is for Carbon, and the folder holds Pro Street."));
        CHECK(s.inst.statusLevel() == installer::Level::Error);
        CHECK(!s.inst.canGoTo(Step::Options));
        CHECK(!s.inst.canInstall());

        // A mod that doesn't read stays on the first step.
        WriteText(root / "broken.amod", "attribulina 1\nedit audio_revlimiter/default {\n");
        CHECK(!s.inst.openMod((root / "broken.amod").string()));
        CHECK(s.inst.step() == Step::Mod);
        CHECK(!s.inst.modError().empty());
        s.frames(1);
    }
    CHECK(ReadFile((global / "ATTRIBUTES.BIN").string()) == attributes);
    CHECK(!fs::exists(global / "ATTRIBUTES.BIN.bak"));
    CHECK(!fs::exists(global / "GlobalMemoryFile.bin.bak"));
    {
        // The next run starts with the folder chosen before, read at once, with no mod.
        Session s(PathsIn(root));
        CHECK_EQ(s.inst.recentGames().size(), size_t(1));
        CHECK(s.inst.gameLoaded());
        CHECK(!s.inst.mod());
        CHECK_EQ(fs::path(s.inst.game().folder.game), game);
        CHECK(s.inst.status().rfind("Found ", 0) == 0);
        CHECK(s.inst.openMod((root / "stops.amod").string()));
        s.frames(1);
        CHECK(s.inst.gameLoaded());
    }
    // Each run wrote its own log.
    CHECK_EQ(LogFiles(root).size(), size_t(2));
    fs::remove_all(root);
}

TEST("installer: once installed, the steps before stay locked until Install Another Mod; the mod goes on GLOBAL's list, and installing it again asks first") {
    const fs::path root = MakeGame("attribulina_installer_list");
    const fs::path game = root / "game", global = game / "GLOBAL";
    const std::string louder = "attribulina 1\ngame prostreet_pc\ntitle \"Louder\"\nauthor \"Someone\"\nversion \"1.0\"\n"
                               "option frames integer \"Frames\" default 5 min 1 max 50\n"
                               "edit audio_revlimiter/default { total_frames_short = $frames }\n";
    WriteText(root / "louder.amod", louder);
    WriteText(root / "louder 1.1.amod", "attribulina 1\ngame prostreet_pc\ntitle \"LOUDER\"\nversion \"1.1\"\n"
                                        "edit audio_revlimiter/default { total_frames_short = 11 }\n");
    // A mod without a title: its file's SHA-256 names it.
    WriteText(root / "plain.amod", "attribulina 1\nedit audio_revlimiter/default { total_frames_long = 12 }\n");
    auto installed = [&](Session& s) {
        CHECK(s.click("Install"));
        s.inst.wait();
        s.frames(2);
        CHECK(s.inst.outcome() == Outcome::Succeeded);
    };
    {
        Session s(PathsIn(root));
        CHECK(s.inst.trackMods()); // on by default
        CHECK(s.inst.openMod((root / "louder.amod").string()));
        s.inst.chooseGame(game.string());
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        CHECK(!s.inst.game().mods); // no list yet
        CHECK(s.inst.installedNote().empty());
        s.inst.goTo(Step::Install);
        s.frames(1);
        installed(s);
        CHECK(s.inst.result().detail.find("It is on the game's list of installed mods") != std::string::npos);
        CHECK(s.inst.game().mods && s.inst.game().mods->mods.size() == 1);

        // Done: the steps before can't be gone back to, by the list, Back, a dropped file or a folder.
        CHECK(s.inst.finished());
        for (Step st : {Step::Mod, Step::Options}) CHECK(!s.inst.canGoTo(st));
        CHECK(!s.drawn("< Back"));
        CHECK(s.click("1   Mod and Game"));
        CHECK(s.inst.step() == Step::Install);
        s.inst.dropped((root / "plain.amod").string());
        s.inst.dropped(game.string());
        s.inst.goTo(Step::Mod);
        s.frames(1);
        CHECK(s.inst.step() == Step::Install);
        CHECK(s.inst.outcome() == Outcome::Succeeded);
        CHECK_EQ(s.inst.mod()->title, std::string("Louder"));
        CHECK(s.inst.status() == "This installation is done. Install Another Mod starts over.");
        CHECK(!s.inst.canInstall());

        // Install Another Mod starts over, the game kept; the same mod again asks first.
        CHECK(s.click("Install Another Mod"));
        CHECK(s.inst.step() == Step::Mod);
        CHECK(s.inst.openMod((root / "louder.amod").string()));
        s.frames(1);
        CHECK(s.inst.installedNote().find("Louder 1.0 is installed here already (installed ") == 0);
        CHECK(!s.inst.canGoTo(Step::Options));
        CHECK(!s.inst.canInstall());
        CHECK(s.click("Next >")); // greyed out
        CHECK(s.inst.step() == Step::Mod);
        CHECK(s.click("Install it anyway"));
        CHECK(s.inst.installAnyway());
        CHECK(s.click("Next >"));
        CHECK(s.inst.step() == Step::Options);
        s.inst.options()["frames"] = "8";
        s.inst.goTo(Step::Install);
        s.frames(1);
        // It changes what its first installation changed: nothing is written
        // until the user says, and the page offers to uninstall that one first.
        const Bytes before = ReadFile((global / "ATTRIBUTES.BIN").string());
        CHECK(s.click("Install"));
        s.inst.wait();
        s.frames(2);
        CHECK(s.inst.outcome() == Outcome::None);
        CHECK(!s.inst.finished());
        CHECK_EQ(s.inst.conflicts().size(), size_t(1));
        CHECK(s.drawn("Uninstall Louder 1.0 first"));
        CHECK(s.drawn("Install Anyway"));
        CHECK(!s.inst.canInstall());
        CHECK(s.logText().find("Louder changes 1 thing Louder 1.0 changed: audio_revlimiter/default total_frames_short") != std::string::npos);
        CHECK(ReadFile((global / "ATTRIBUTES.BIN").string()) == before);
        CHECK(s.click("Install Anyway"));
        s.inst.wait();
        s.frames(2);
        CHECK(s.inst.outcome() == Outcome::Succeeded);

        // Another version of it: the same question, in other words.
        CHECK(s.click("Install Another Mod"));
        CHECK(s.inst.openMod((root / "louder 1.1.amod").string()));
        s.frames(1);
        CHECK(s.inst.installedNote().find("Louder 1.0 is installed here (installed ") == 0);
        CHECK(s.inst.installedNote().find("Installing version 1.1 over it") != std::string::npos);

        // Advanced Options: the list off, so nothing asks and nothing is added.
        CHECK(s.click("Advanced Options"));
        CHECK(s.drawn("Keep a list of installed mods"));
        CHECK(s.click("Keep a list of installed mods"));
        CHECK(!s.inst.trackMods());
        CHECK(s.inst.installedNote().empty());
        CHECK(s.inst.canGoTo(Step::Options));
        CHECK(ReadText(root / "user" / installer::kSettingsFileName).find("TrackMods=0\n") != std::string::npos);
        s.imgui.key([&]() { s.inst.frame(); }, ImGuiKey_Escape); // closes the popup
        s.frames(1);
        // The window with the list: its button is beside Advanced Options.
        CHECK(s.drawn("Installed Mods..."));
        CHECK(s.click("Installed Mods..."));
        s.frames(1);
        CHECK(test::UiProbe::get().find("Louder", "Installed Mods")); // a row each time it was installed
        CHECK_EQ(test::UiProbe::get().count("Louder", "Installed Mods"), 2);
        CHECK(!s.drawn("Copy##sha256"));
        CHECK(s.click("Louder"));
        s.frames(1);
        CHECK(s.drawn("Copy##sha256")); // the picked mod in full
        CHECK(s.click("Copy##sha256"));
        CHECK_EQ(std::string(ImGui::GetClipboardText()), Sha256Hex(louder));
        CHECK(s.click("Close"));
        s.frames(1);
        CHECK(!test::UiProbe::get().find("Louder", "Installed Mods"));
        s.inst.goTo(Step::Install);
        s.frames(1);
        installed(s);
        CHECK(test::UiProbe::get().conflicts.empty());
    }
    auto list = ReadModList(global.string());
    CHECK(list && list->mods.size() == 2); // the third went in with the list off
    if (list && list->mods.size() == 2) {
        const InstalledMod& m = list->mods[0];
        CHECK_EQ(m.title, std::string("Louder"));
        CHECK_EQ(m.author, std::string("Someone"));
        CHECK_EQ(m.version, std::string("1.0"));
        CHECK(!m.nfsms);
        CHECK_EQ(m.sha256, Sha256Hex(louder));
        CHECK_EQ(fs::path(m.script), root / "louder.amod");
        CHECK_EQ(m.game, std::string("prostreet_pc"));
        CHECK(m.by.rfind("Attribulina Mod Installer ", 0) == 0);
        CHECK_EQ(m.result, std::string("installed"));
        CHECK(m.options == (std::vector<std::pair<std::string, std::string>>{{"frames", "5"}}));
        CHECK(m.files == (std::vector<std::string>{"GLOBAL\\ATTRIBUTES.BIN", "GLOBAL\\GlobalMemoryFile.bin"}));
        CHECK_EQ(m.installed.size(), size_t(19));
        CHECK(list->mods[1].options == (std::vector<std::pair<std::string, std::string>>{{"frames", "8"}}));
    }
    {
        // The next run keeps the list off; on again, a mod without a title is named by its file.
        Session s(PathsIn(root));
        CHECK(!s.inst.trackMods());
        s.inst.setTrackMods(true);
        CHECK(s.inst.openMod((root / "plain.amod").string()));
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        CHECK(s.inst.installedNote().empty());
        CHECK(s.logText().find("it names no title, so its file's SHA-256") != std::string::npos);
        s.inst.goTo(Step::Install);
        s.frames(1);
        installed(s);
        CHECK(s.click("Install Another Mod"));
        CHECK(s.inst.openMod((root / "plain.amod").string()));
        s.frames(1);
        CHECK(s.inst.installedNote().find("plain.amod is installed here already") == 0);
    }
    list = ReadModList(global.string());
    CHECK(list && list->mods.size() == 3);
    if (list && list->mods.size() == 3) {
        CHECK(list->mods[2].title.empty());
        CHECK_EQ(list->mods[2].name(), std::string("plain.amod"));
    }
    {
        // A list that doesn't read is left as it is, and says so.
        WriteText(global / kModListFile, "attribulina mods 1\nmod \"Broken\n");
        Session s(PathsIn(root));
        CHECK(s.inst.openMod((root / "louder.amod").string()));
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        CHECK(!s.inst.game().modsError.empty());
        CHECK(s.inst.installedNote().empty());
        s.inst.goTo(Step::Install);
        s.frames(1);
        installed(s);
        CHECK(s.logText().find("The list of installed mods wasn't updated") != std::string::npos);
        CHECK_EQ(ReadText(global / kModListFile), std::string("attribulina mods 1\nmod \"Broken\n"));
    }
    fs::remove_all(root);
}

TEST("installer: the game's list opens without a mod, and uninstalling a mod takes the ones that build on it, once the confirmation lists them") {
    const fs::path root = MakeGame("attribulina_installer_dependents");
    const fs::path game = root / "game", global = game / "GLOBAL";
    const Bytes vanilla = ReadFile((global / "ATTRIBUTES.BIN").string());
    const Bytes vanillaMemory = ReadFile((global / "GlobalMemoryFile.bin").string());
    WriteText(root / "maker.amod", "attribulina 1\ngame prostreet_pc\ntitle \"Maker\"\nversion \"1\"\n"
                                   "create pvehicle/dep_car from pvehicle/street\n"
                                   "edit audio_revlimiter/default { total_frames_short = 20 }\n");
    WriteText(root / "user.amod", "attribulina 1\ngame prostreet_pc\ntitle \"User\"\nversion \"1\"\n"
                                  "edit pvehicle/dep_car { HornType = 3 }\n");
    WriteText(root / "rival.amod", "attribulina 1\ngame prostreet_pc\ntitle \"Rival\"\nversion \"1\"\n"
                                   "edit audio_revlimiter/default { total_frames_short = 30 }\n");
    auto install = [&](Session& s, const fs::path& mod) {
        if (s.inst.finished()) CHECK(s.click("Install Another Mod"));
        CHECK(s.inst.openMod(mod.string()));
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        s.inst.goTo(Step::Install);
        s.frames(1);
        CHECK(s.click("Install"));
        s.inst.wait();
        s.frames(2);
    };
    {
        Session s(PathsIn(root));
        // No mod, no game yet: the button waits for the game.
        CHECK(s.drawn("Installed Mods..."));
        CHECK(s.click("Installed Mods..."));
        CHECK(!test::UiProbe::get().find("Close", "Installed Mods"));
        s.host.nextOpenFolder = game.string();
        CHECK(s.click("Browse...##game"));
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        CHECK(!s.inst.mod());
        CHECK(s.inst.step() == Step::Mod);
        install(s, root / "maker.amod");
        CHECK(s.inst.outcome() == Outcome::Succeeded);
        install(s, root / "user.amod");
        CHECK(s.inst.outcome() == Outcome::Succeeded);
    }
    {
        // A new run: the folder chosen before loads with no mod, and the list is a click away.
        Session s(PathsIn(root));
        CHECK(s.inst.gameLoaded());
        CHECK(!s.inst.mod());
        CHECK_EQ(s.inst.game().traced.size(), size_t(2));
        CHECK(s.click("Installed Mods..."));
        s.frames(1);
        CHECK(test::UiProbe::get().find("Maker", "Installed Mods"));
        CHECK(s.click("Maker"));
        CHECK(s.click("Uninstall..."));
        // Worked out first: User builds on the car Maker made, so it goes too, and the question says so.
        CHECK(s.inst.uninstallAsk().has_value());
        if (s.inst.uninstallAsk()) {
            CHECK_EQ(s.inst.uninstallAsk()->all.size(), size_t(2));
            CHECK(s.inst.uninstallAsk()->dependents.find("User 1 builds on what Maker 1 changed, so it is uninstalled with it") == 0);
        }
        CHECK(s.drawn("Cancel##confirm"));
        CHECK(!s.drawn("Uninstall")); // the button names them all
        CHECK(s.drawn("Uninstall All of Them"));
        CHECK(ReadFile((global / "ATTRIBUTES.BIN").string()) != vanilla); // nothing written by asking
        CHECK(s.click("Cancel##confirm"));
        CHECK(!s.inst.uninstallAsk());
        CHECK(s.click("Uninstall..."));
        CHECK(s.click("Uninstall All of Them"));
        s.inst.wait();
        s.frames(2);
        CHECK(s.inst.modsMessage().find("Uninstalled Maker 1 and User 1: ") == 0);
        CHECK(s.inst.game().traced.empty());
        CHECK(ReadFile((global / "ATTRIBUTES.BIN").string()) == vanilla);
        CHECK(ReadFile((global / "GlobalMemoryFile.bin").string()) == vanillaMemory);
        CHECK(s.clickIn("Close", "Installed Mods"));
        s.frames(1);

        // On the conflict page: ticking Maker takes User with it, and the page says so first.
        install(s, root / "maker.amod");
        install(s, root / "user.amod");
        install(s, root / "rival.amod");
        CHECK(s.inst.outcome() == Outcome::None);
        CHECK_EQ(s.inst.conflicts().size(), size_t(1));
        if (!s.inst.conflicts().empty()) {
            CHECK(s.inst.conflicts()[0].mod.title == "Maker");
            CHECK_EQ(s.inst.conflicts()[0].with.size(), size_t(1));
            CHECK(s.inst.conflicts()[0].withText.find("User 1 builds on what Maker 1 changed") == 0);
        }
        CHECK(s.click("Uninstall the Ticked Mods, Then Install"));
        s.inst.wait();
        s.frames(2);
        CHECK(s.inst.outcome() == Outcome::Succeeded);
        CHECK(s.inst.result().detail.find("Maker 1 and User 1 were uninstalled first.") != std::string::npos);
        auto list = ReadModList(global.string());
        CHECK(list && list->mods.size() == 1 && list->mods[0].title == "Rival");
        CHECK(test::UiProbe::get().conflicts.empty());
    }
    fs::remove_all(root);
}

TEST("installer: a mod that changes what one on the list changed asks first: uninstall that one first, or install anyway; "
     "the Installed Mods window uninstalls a mod or takes it off the list") {
    const fs::path root = MakeGame("attribulina_installer_uninstall");
    const fs::path game = root / "game", global = game / "GLOBAL";
    const Bytes vanilla = ReadFile((global / "ATTRIBUTES.BIN").string());
    const Bytes vanillaMemory = ReadFile((global / "GlobalMemoryFile.bin").string());
    WriteText(root / "engine.amod", "attribulina 1\ngame prostreet_pc\ntitle \"Engine\"\nversion \"1\"\n"
                                    "edit audio_revlimiter/default { total_frames_short = 20\n total_frames_long = 21 }\n");
    WriteText(root / "sound.amod", "attribulina 1\ngame prostreet_pc\ntitle \"Sound\"\nversion \"1\"\n"
                                   "edit audio_revlimiter/default { total_frames_short = 30 }\n");
    WriteText(root / "apart.amod", "attribulina 1\ngame prostreet_pc\ntitle \"Apart\"\n"
                                   "edit audio_revlimiter/default { load_for_short = 0.5 }\n");
    WriteText(root / "readme.txt", "Copied.\n");
    WriteText(root / "copier.nfsms", "version 4.6\ngame PS\nupdate_field audio_revlimiter default total_frames_short 40\n"
                                     "file_copy \"readme.txt\" \"copier readme.txt\"\n# amod begin\n# title \"Copier\"\n# amod end\n");
    auto install = [&](Session& s, const fs::path& mod) {
        if (s.inst.finished()) CHECK(s.click("Install Another Mod"));
        CHECK(s.inst.openMod(mod.string()));
        s.frames(1);
        if (!s.inst.gameLoaded()) {
            s.inst.chooseGame(game.string());
            s.frames(1);
        }
        CHECK(s.inst.gameLoaded());
        s.inst.goTo(Step::Install);
        s.frames(1);
        CHECK(s.click("Install"));
        s.inst.wait();
        s.frames(2);
    };
    auto value = [&](const char* field) {
        auto db = std::make_unique<Database>();
        db->addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
        db->loadPack((global / "ATTRIBUTES.BIN").string());
        Collection* c = db->findCollection(StringHash32("audio_revlimiter"), StringHash32("default"));
        const Attribute* a = c ? c->find(StringHash32(field)) : nullptr;
        return a ? load32(a->data.bytes.data(), Endian::Little) : 0u;
    };
    const uint32_t shortWas = value("total_frames_short"), longWas = value("total_frames_long");
    {
        Session s(PathsIn(root));
        install(s, root / "engine.amod");
        CHECK(s.inst.outcome() == Outcome::Succeeded);
        auto list = ReadModList(global.string());
        CHECK(list && list->mods.size() == 1);
        const std::string engineTrace = list && !list->mods.empty() ? list->mods[0].trace : std::string();
        CHECK(engineTrace.rfind("attribulina_traces\\", 0) == 0);
        CHECK(fs::exists(TracePath(global.string(), engineTrace)));
        CHECK(s.logText().find("with what it changed in ") != std::string::npos);

        // Sound changes Engine's value: Engine goes first, ticked as it starts.
        install(s, root / "sound.amod");
        CHECK(s.inst.outcome() == Outcome::None);
        CHECK_EQ(s.inst.conflicts().size(), size_t(1));
        CHECK(s.inst.conflicts().empty() || s.inst.conflicts()[0].uninstall);
        CHECK_EQ(value("total_frames_short"), 20u);
        CHECK(s.click("Uninstall the Ticked Mods, Then Install"));
        s.inst.wait();
        s.frames(2);
        CHECK(s.inst.outcome() == Outcome::Succeeded);
        CHECK(s.inst.result().detail.find("Engine 1 was uninstalled first.") != std::string::npos);
        CHECK_EQ(value("total_frames_short"), 30u);
        CHECK_EQ(value("total_frames_long"), longWas);
        list = ReadModList(global.string());
        CHECK(list && list->mods.size() == 1 && list->mods[0].title == "Sound");
        CHECK(!fs::exists(TracePath(global.string(), engineTrace)));

        // An nfsms mod that copies files is checked first without them: none
        // is copied until the user says.
        install(s, root / "copier.nfsms");
        CHECK(s.inst.outcome() == Outcome::None);
        CHECK_EQ(s.inst.conflicts().size(), size_t(1));
        CHECK(!fs::exists(game / "copier readme.txt"));
        CHECK(s.click("Install Anyway"));
        s.inst.wait();
        s.frames(2);
        CHECK(s.inst.outcome() == Outcome::Succeeded);
        CHECK(fs::exists(game / "copier readme.txt"));
        CHECK_EQ(value("total_frames_short"), 40u);

        // Apart changes nothing the others do: it goes in without asking.
        install(s, root / "apart.amod");
        CHECK(s.inst.outcome() == Outcome::Succeeded);
        CHECK_EQ(s.inst.game().traced.size(), size_t(3));

        // The window: uninstalling Sound, under Copier, which changes the same value, keeps Copier's.
        CHECK(s.click("Installed Mods..."));
        s.frames(1);
        CHECK(s.click("Sound"));
        CHECK(s.click("Uninstall..."));
        CHECK(s.drawn("Cancel##confirm"));
        CHECK(s.click("Uninstall"));
        s.inst.wait();
        s.frames(2);
        CHECK(s.inst.modsMessage().find("Uninstalled Sound 1: ") == 0);
        CHECK(s.inst.modsMessage().find("undone and done again") != std::string::npos);
        CHECK_EQ(value("total_frames_short"), 40u);
        list = ReadModList(global.string());
        CHECK(list && list->mods.size() == 2 && list->mods[0].title == "Copier");
        // Copier's trace now goes back to the value before Sound.
        CHECK(list && !list->mods.empty() &&
              ReadText(TracePath(global.string(), list->mods[0].trace)).find("total_frames_short = " + std::to_string(shortWas) + " was 40") !=
                  std::string::npos);

        // Copier and Apart uninstalled: the database is the game's again, byte for byte; the copied file stays.
        for (const char* name : {"Copier", "Apart"}) {
            CHECK(s.click(name));
            CHECK(s.click("Uninstall..."));
            CHECK(s.click("Uninstall"));
            s.inst.wait();
            s.frames(2);
            CHECK(s.inst.modsMessage().find(std::string("Uninstalled ") + name) == 0);
        }
        CHECK(ReadFile((global / "ATTRIBUTES.BIN").string()) == vanilla);
        CHECK(ReadFile((global / "GlobalMemoryFile.bin").string()) == vanillaMemory);
        CHECK(fs::exists(game / "copier readme.txt"));
        CHECK(s.inst.game().traced.empty());
        CHECK(!fs::exists(global / kTraceFolder) || fs::is_empty(global / kTraceFolder));

        // One with no trace (installed before traces were kept) can only be taken off the list.
        ModList old;
        InstalledMod m;
        m.title = "Old";
        old.mods.push_back(m);
        WriteModList(global.string(), old);
        CHECK(s.clickIn("Close", "Installed Mods"));
        s.frames(1);
        CHECK(s.click("Install Another Mod"));
        s.inst.chooseGame(game.string());
        s.frames(1);
        CHECK(s.click("Installed Mods..."));
        s.frames(1);
        CHECK(s.click("Old"));
        CHECK(!s.inst.canUninstall(0));
        CHECK(s.click("Uninstall...")); // greyed out
        CHECK(!s.drawn("Cancel##confirm"));
        CHECK(s.click("Remove from List..."));
        CHECK(s.click("Remove from List"));
        s.frames(1);
        CHECK(s.inst.modsMessage().find("Took Old off the list.") == 0);
        list = ReadModList(global.string());
        CHECK(list && list->mods.empty());
        CHECK(test::UiProbe::get().conflicts.empty());
    }
    fs::remove_all(root);
}
