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
        CHECK(s.inst.status() == "Choose the mod to install.");
        CHECK(!s.inst.canGoTo(Step::Game));
        CHECK(!s.inst.logFilePath().empty());

        // Browse... gives the mod, and the game's folder comes next.
        s.host.nextOpenFile = (root / "louder.amod").string();
        CHECK(s.click("Browse...##mod"));
        CHECK(s.inst.mod());
        CHECK(s.inst.step() == Step::Game);
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
        CHECK_EQ(s.inst.log().back().text, std::string("Installation successful: The mod is installed."));
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
                                      "file_copy \"readme.txt\" \"Loud Mod readme.txt\"\n");
    {
        // In the background, as the program runs it: the command line's game folder and mod.
        Session s(PathsIn(root), true);
        s.inst.openArguments({game.string(), (mod / "Loud Mod.nfsms").string()});
        CHECK(s.inst.step() == Step::Game);
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

        // Another game's mod: the steps after Game are closed.
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
        // The next run starts with the folder chosen before, read once a mod opens.
        Session s(PathsIn(root));
        CHECK_EQ(s.inst.recentGames().size(), size_t(1));
        CHECK(s.inst.openMod((root / "stops.amod").string()));
        s.frames(1);
        CHECK(s.inst.gameLoaded());
        CHECK_EQ(fs::path(s.inst.game().folder.game), game);
    }
    // Each run wrote its own log.
    CHECK_EQ(LogFiles(root).size(), size_t(2));
    fs::remove_all(root);
}
